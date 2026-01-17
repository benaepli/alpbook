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

    export template<Listener L,
                    uint32_t PricePoolSize = DEFAULT_PRICE_LEVEL_POOL_SIZE,
                    uint32_t OrderPoolSize = DEFAULT_ORDER_POOL_SIZE>
    class Book
    {
        template<typename M>
        using SizedPricePool = PricePool<M, PricePoolSize>;

      public:
        Book() = default;
        Book(L& listener)
            : listener_(&listener)
        {
        }

        void setListener(L& listener) noexcept { listener_ = &listener; }
        void clearListener() { listener_ = nullptr; }

        [[nodiscard]] Level getBestBid() const noexcept { return getBest(buyPool_); }
        [[nodiscard]] Level getBestAsk() const noexcept { return getBest(sellPool_); }
        [[nodiscard]] Level getBidLevel(int depth) const noexcept;
        [[nodiscard]] Level getAskLevel(int depth) const noexcept;

        [[nodiscard]] quantity_t getBuyVolumeAhead(price_t targetPrice) const noexcept;
        [[nodiscard]] quantity_t getSellVolumeAhead(price_t targetPrice) const noexcept;

        [[nodiscard]] quantity_t getBuyVolumeAheadByOrder(uint64_t orderID) const noexcept;
        [[nodiscard]] quantity_t getSellVolumeAheadByOrder(uint64_t orderID) const noexcept;

        void add(AddOrder order)
        {
            withPool(
                order.side,
                [this, &order](auto& pool)
                {
                    addImpl(pool, order.timestamp, order.id, order.price, order.shares, order.side);
                });
        }

        std::expected<void, Error> execute(ExecuteOrder order)
        {
            return decrementImpl(order.id, order.shares);
        }
        std::expected<void, Error> reduce(DecrementShares order)
        {
            return decrementImpl(order.id, order.shares);
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
            withPool(o.side,
                     [this, id, priceId, &o](auto& pool) { cancelImpl(id, priceId, o, pool); });
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
            withPool(o.side,
                     [this, id, priceId, &o, &order](auto& pool)
                     {
                         cancelImpl(id, priceId, o, pool);
                         addImpl(
                             pool, order.timestamp, order.newId, order.price, order.shares, o.side);
                     });
            return {};
        }

      private:
        template<typename Func>
        decltype(auto) withPool(Side side, Func&& func)
        {
            if (side == Side::Buy)
            {
                return func(buyPool_);
            }
            return func(sellPool_);
        }

        template<typename M>
        void addImpl(SizedPricePool<M>& pool,
                     uint64_t timestamp,
                     uint64_t orderId,
                     uint64_t price,
                     uint32_t shares,
                     Side side)
        {
            auto priceId = pool.getOrDefault(price);
            PriceLevel& priceLevel = pool.pool[priceId];
            priceLevel.totalShares += shares;

            auto newId = orderPool_.allocate(timestamp, orderId, shares, side);

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

        std::expected<void, Error> decrementImpl(uint64_t orderId, uint32_t shares)
        {
            auto it = orderToDetails_.find(orderId);
            if (it == orderToDetails_.end()) [[unlikely]]
            {
                return std::unexpected(Error::MissingId);
            }
            auto [id, priceId] = it->second;
            Order& o = orderPool_[id];
            return withPool(
                o.side,
                [this, id, priceId, &o, shares](auto& pool) -> std::expected<void, Error>
                {
                    if (o.shares == shares)
                    {
                        cancelImpl(id, priceId, o, pool);
                        return {};
                    }
                    o.shares -= shares;
                    auto& level = pool.pool[priceId];
                    level.totalShares -= shares;
                    return {};
                });
        }

        template<typename M>
        void cancelImpl(uint32_t id, uint32_t priceId, Order const& o, SizedPricePool<M>& pool)
        {
            auto& priceLevel = pool.pool[priceId];
            priceLevel.totalShares -= o.shares;
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

            if (priceLevel.totalShares == 0)
            {
                pool.levels.erase(priceLevel.price);
                pool.pool.deallocate(priceId);
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
            if (listener_)
            {
                listener_->onTrade(price, quantity, side);
            }
        }

        L* listener_ {};
        SizedPricePool<BidMap> buyPool_ {};
        SizedPricePool<AskMap> sellPool_ {};

        internal::ObjectPool<Order> orderPool_ {OrderPoolSize};
        absl::flat_hash_map<uint64_t, OrderDetails> orderToDetails_ {};
    };
}  // namespace alpbook::nasdaq