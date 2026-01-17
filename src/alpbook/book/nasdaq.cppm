module;

import alpbook.book.core;
import alpbook.common;
import alpbook.internal;

#include <cstdint>
#include <expected>

#include "absl/container/flat_hash_map.h"

export module alpbook.book.nasdaq;

export import :state;
namespace alpbook::nasdaq
{
    enum class Error : uint8_t
    {
        MissingId,
    };
    export constexpr auto DEFAULT_PRICE_LEVEL_POOL_SIZE = 50'000;
    export constexpr auto DEFAULT_ORDER_POOL_SIZE = 2'000'000;
    export struct AddOrder
    {
        uint64_t timestamp {};
        uint64_t id {};
        uint64_t price {};
        uint32_t shares {};
        Side side {};
    };
    export struct ExecuteOrder
    {
        uint64_t id {};
        uint32_t shares {};
    };

    export struct DecrementShares
    {
        uint64_t id {};
        uint32_t shares {};
    };

    export struct CancelOrder
    {
        uint64_t id {};
    };

    export struct ReplaceOrder
    {
        uint64_t timestamp {};
        uint64_t oldId {};
        uint64_t newId {};
        uint64_t price {};
        uint32_t shares {};
    };
    struct OrderDetails
    {
        uint32_t orderId;
        uint32_t priceLevelId;
    };
    template<typename M, int PricePoolSize>
    struct PricePool
    {
        internal::ObjectPool<PriceLevel> pool {PricePoolSize};
        M levels {};

        uint32_t allocate(uint64_t price)
        {
            auto id = pool.allocate(price);
            levels[price] = id;
            return id;
        }

        uint32_t getOrDefault(uint64_t price)
        {
            auto it = levels.find(price);
            if (it == levels.end())
            {
                return allocate(price);
            }
            return it->second;
        }
    };

    struct TagChange
    {
    };
    struct TagExecute
    {
    };

    export template<Listener L,
                    uint32_t PricePoolSize = DEFAULT_PRICE_LEVEL_POOL_SIZE,
                    uint32_t OrderPoolSize = DEFAULT_ORDER_POOL_SIZE>
    class Book
    {
        template<typename M>
        using SizedPricePool = PricePool<M, PricePoolSize>;

      public:
        Book(L& listener)
            : listener_(&listener)
        {
        }

        void setListener(L& listener) noexcept { listener_ = &listener; }

        [[nodiscard]] Level getBestBid() const noexcept { return bestBid_; }
        [[nodiscard]] Level getBestAsk() const noexcept { return bestAsk_; }
        [[nodiscard]] Level getBidLevel(int depth) const noexcept;
        [[nodiscard]] Level getAskLevel(int depth) const noexcept;
        [[nodiscard]] quantity_t getBuyVolumeAhead(price_t targetPrice) const noexcept;
        [[nodiscard]] quantity_t getSellVolumeAhead(price_t targetPrice) const noexcept;

        [[nodiscard]] quantity_t getBuyVolumeAheadByOrder(uint64_t orderID) const noexcept;
        [[nodiscard]] quantity_t getSellVolumeAheadByOrder(uint64_t orderID) const noexcept;

        void add(AddOrder order)
        {
            if (order.side == Side::Buy)
            {
                addImpl<Side::Buy>(order.timestamp, order.id, order.price, order.shares);
            }
            else
            {
                addImpl<Side::Sell>(order.timestamp, order.id, order.price, order.shares);
            }
        }

        std::expected<void, Error> execute(ExecuteOrder order)
        {
            return decrementImpl(order.id, order.shares, TagExecute {});
        }
        std::expected<void, Error> reduce(DecrementShares order)
        {
            return decrementImpl(order.id, order.shares, TagChange {});
        }
        std::expected<void, Error> cancel(CancelOrder order)
        {
            auto it = orderToDetails_.find(order.id);
            if (it == orderToDetails_.end()) [[unlikely]]
            {
                return std::unexpected(Error::MissingId);
            }
            auto [id, priceId] = it->second;
            Order& o = orderPool_[id];

            if (o.side == Side::Buy)
            {
                cancelImpl<Side::Buy>(id, priceId, o, TagChange {});
            }
            else
            {
                cancelImpl<Side::Sell>(id, priceId, o, TagChange {});
            }
            return {};
        }

        std::expected<void, Error> replace(ReplaceOrder order)
        {
            auto it = orderToDetails_.find(order.oldId);
            if (it == orderToDetails_.end()) [[unlikely]]
            {
                return std::unexpected(Error::MissingId);
            }
            auto [id, priceId] = it->second;
            Order o = std::move(orderPool_[id]);
            if (o.side == Side::Buy)
            {
                cancelImpl<Side::Buy>(id, priceId, o, TagChange {});
                addImpl<Side::Buy>(order.timestamp, order.newId, order.price, order.shares);
            }
            else
            {
                cancelImpl<Side::Sell>(id, priceId, o, TagChange {});
                addImpl<Side::Sell>(order.timestamp, order.newId, order.price, order.shares);
            }
            return {};
        }

      private:
        template<Side S>
        decltype(auto) getPool()
        {
            if constexpr (S == Side::Buy)
            {
                return buyPool_;
            }
            else
            {
                return sellPool_;
            }
        }

        template<Side S>
        void addImpl(uint64_t timestamp, uint64_t orderId, uint64_t price, uint32_t shares)
        {
            constexpr bool buySide = (S == Side::Buy);
            auto& pool = getPool<S>();

            auto priceId = pool.getOrDefault(price);
            PriceLevel& priceLevel = pool.pool[priceId];
            priceLevel.totalShares += shares;

            Level& best = buySide ? bestBid_ : bestAsk_;
            if (price == best.price)
            {
                best.quantity += shares;
                if constexpr (buySide)
                {
                    listener_->onTopBidChange(best.price, best.quantity);
                }
                else
                {
                    listener_->onTopAskChange(best.price, best.quantity);
                }
            }
            else if (isNewBest(price, best, S))
            {
                best = {price, shares};
                if constexpr (buySide)
                {
                    listener_->onTopBidChange(best.price, best.quantity);
                }
                else
                {
                    listener_->onTopAskChange(best.price, best.quantity);
                }
            }

            auto newId = orderPool_.allocate(timestamp, orderId, shares, S);
            if (priceLevel.tail == INVALID_ID)
            {
                priceLevel.head = newId;
                priceLevel.tail = newId;
            }
            else
            {
                orderPool_[newId].prev = priceLevel.tail;
                orderPool_[priceLevel.tail].next = newId;
                priceLevel.tail = newId;
            }
            orderToDetails_[orderId] = {.orderId = newId, .priceLevelId = priceId};
        }

        template<typename Tag>
        std::expected<void, Error> decrementImpl(uint64_t orderId, uint32_t shares, Tag tag)
        {
            auto it = orderToDetails_.find(orderId);
            if (it == orderToDetails_.end()) [[unlikely]]
            {
                return std::unexpected(Error::MissingId);
            }
            auto [id, priceId] = it->second;
            Order& o = orderPool_[id];

            if (o.side == Side::Buy)
            {
                decrementInner<Side::Buy>(id, priceId, o, shares, tag);
            }
            else
            {
                decrementInner<Side::Sell>(id, priceId, o, shares, tag);
            }
            return {};
        }

        template<Side S, typename Tag>
        void decrementInner(uint64_t id, uint64_t priceId, Order& o, uint32_t shares, Tag tag)
        {
            constexpr bool buySide = (S == Side::Buy);
            auto& pool = getPool<S>();

            if (o.shares == shares)
            {
                cancelImpl<S>(id, priceId, o, tag);
                return;
            }
            o.shares -= shares;
            auto& level = pool.pool[priceId];
            level.totalShares -= shares;

            if constexpr (std::is_same_v<Tag, TagExecute>)
            {
                notifyTrade(level.price, shares, S);
            }

            Level& best = buySide ? bestBid_ : bestAsk_;
            if (o.shares > 0 && level.price == best.price)
            {
                best.quantity = level.totalShares;
                if constexpr (buySide)
                {
                    listener_->onTopBidChange(best.price, best.quantity);
                }
                else
                {
                    listener_->onTopAskChange(best.price, best.quantity);
                }
            }
        }

        template<Side S, typename Tag>
        void cancelImpl(uint32_t id, uint32_t priceId, Order const& o, Tag tag)
        {
            constexpr bool buySide = (S == Side::Buy);
            auto& pool = getPool<S>();

            auto& priceLevel = pool.pool[priceId];
            if constexpr (std::is_same_v<Tag, TagExecute>)
            {
                notifyTrade(priceLevel.price, o.shares, S);
            }

            if (o.prev == INVALID_ID)  // i.e. we are the head
            {
                priceLevel.head = o.next;
            }
            else
            {
                orderPool_[o.prev].next = o.next;
            }

            if (o.next == INVALID_ID)
            {
                priceLevel.tail = o.prev;
            }
            else
            {
                orderPool_[o.next].prev = o.prev;
            }
            orderPool_.deallocate(id);
            orderToDetails_.erase(o.id);

            priceLevel.totalShares -= o.shares;
            Level& best = buySide ? bestBid_ : bestAsk_;
            bool wasTop = (priceLevel.price == best.price);
            if (priceLevel.totalShares == 0)
            {
                pool.levels.erase(priceLevel.price);
                pool.pool.deallocate(priceId);

                if (wasTop)
                {
                    best = getBest(pool);
                    if constexpr (buySide)
                    {
                        listener_->onTopBidChange(best.price, best.quantity);
                    }
                    else
                    {
                        listener_->onTopAskChange(best.price, best.quantity);
                    }
                }
            }
            else if (wasTop)
            {
                // Volume changed
                best.quantity = priceLevel.totalShares;
                if constexpr (buySide)
                {
                    listener_->onTopBidChange(best.price, best.quantity);
                }
                else
                {
                    listener_->onTopAskChange(best.price, best.quantity);
                }
            }
        }

        template<typename M>
        Level getBest(SizedPricePool<M>& pool)
        {
            auto it = pool.levels.begin();
            if (it == pool.levels.end()) [[unlikely]]
            {
                return Level {};
            }
            auto const& priceLevel = pool.pool[it->second];
            return {.price = priceLevel.price, .quantity = priceLevel.totalShares};
        }

        void notifyTrade(uint64_t price, uint32_t quantity, Side side)
        {
            listener_->onTrade(price, quantity, side);
        }

        bool isNewBest(uint64_t price, Level const& currentBest, Side side)
        {
            if (side == Side::Buy)
            {
                return price > currentBest.price;
            }
            return price < currentBest.price;
        }

        L* listener_ {};
        SizedPricePool<BidMap> buyPool_ {};
        SizedPricePool<AskMap> sellPool_ {};

        internal::ObjectPool<Order> orderPool_ {OrderPoolSize};
        absl::flat_hash_map<uint64_t, OrderDetails> orderToDetails_ {};

        // Cached versions of the best bid and ask
        Level bestBid_ {};
        Level bestAsk_ {};
    };
}  // namespace alpbook::nasdaq