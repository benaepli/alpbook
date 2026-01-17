module;

import alpbook.book.core;
import alpbook.common;
import alpbook.internal;

#include <cstdint>

#include "absl/container/flat_hash_map.h"

export module alpbook.book.nasdaq;

export import :state;

namespace alpbook::nasdaq
{
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

    export template<Listener L,
                    uint32_t PricePoolSize = DEFAULT_PRICE_LEVEL_POOL_SIZE,
                    uint32_t OrderPoolSize = DEFAULT_ORDER_POOL_SIZE>
    class Book
    {
      public:
        Book();
        Book(L& listener);

        void setListener(L& listener) noexcept { listener_ = &listener; }
        void clearListener() { listener_ = nullptr; }

        [[nodiscard]] Level getBestBid() const noexcept;
        [[nodiscard]] Level getBestAsk() const noexcept;
        [[nodiscard]] Level getBidLevel(int depth) const noexcept;
        [[nodiscard]] Level getAskLevel(int depth) const noexcept;

        [[nodiscard]] quantity_t getBuyVolumeAhead(price_t targetPrice) const noexcept;
        [[nodiscard]] quantity_t getSellVolumeAhead(price_t targetPrice) const noexcept;

        [[nodiscard]] quantity_t getBuyVolumeAheadByOrder(uint64_t orderID) const noexcept;
        [[nodiscard]] quantity_t getSellVolumeAheadByOrder(uint64_t orderID) const noexcept;

        void add(AddOrder order)
        {
            auto& pool = getPool(order.side);
            addImpl(pool, order.timestamp, order.id, order.price, order.shares, order.side);
        }

        void execute(ExecuteOrder order) { decrementImpl(order.id, order.shares); }
        void reduce(DecrementShares order) { decrementImpl(order.id, order.shares); }
        void cancel(CancelOrder order)
        {
            auto [id, priceId] = orderToDetails_[order.id];
            Order& o = orderPool_[id];
            auto& pool = getPool(o.side);
            cancelImpl(id, priceId, o, pool);
        }

        void replace(ReplaceOrder order)
        {
            auto [id, priceId] = orderToDetails_[order.oldId];
            Order o = std::move(orderPool_[id]);
            auto& pool = getPool(o.side);
            cancelImpl(id, priceId, o, pool);
            addImpl(pool, order.timestamp, order.newId, order.price, order.shares, o.side);
        }

      private:
        struct PricePool
        {
            internal::ObjectPool<PriceLevel> pool {PricePoolSize};
            LevelMap levels {};

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

        auto& getPool(Side side)
        {
            if (side == Side::Buy)
            {
                return buyPool_;
            }
            return sellPool_;
        }

        void addImpl(PricePool& pool,
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

        void decrementImpl(uint64_t orderId, uint32_t shares)
        {
            auto [id, priceId] = orderToDetails_[orderId];
            Order& o = orderPool_[id];
            auto& pool = getPool(o.side);
            if (o.shares == shares)
            {
                return cancelImpl(id, priceId, o, pool);
            }
            o.shares -= shares;
            auto& level = pool.pool[priceId];
            level.totalShares -= shares;
        }

        void cancelImpl(uint32_t id, uint32_t priceId, Order const& o, PricePool& pool)
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

        L* listener_ {};
        PricePool buyPool_;
        PricePool sellPool_;

        internal::ObjectPool<Order> orderPool_ {OrderPoolSize};
        absl::flat_hash_map<uint64_t, OrderDetails> orderToDetails_ {};
    };
}  // namespace alpbook::nasdaq