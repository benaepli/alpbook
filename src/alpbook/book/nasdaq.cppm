module;

import alpbook.book.core;
import alpbook.common;
import alpbook.internal;

#include <array>
#include <cstdint>
#include <expected>
#include <memory_resource>

#include <sys/mman.h>

#include "absl/container/flat_hash_map.h"
#include "alpbook/internal/hints.hpp"

export module alpbook.book.nasdaq;

export import :state;
namespace alpbook::nasdaq
{
    export constexpr auto DEFAULT_ORDER_POOL_SIZE = 500'000;
    export constexpr auto DEFAULT_BUFFER_POOL_SIZE = 64 << 20;  // 64MB default buffer size

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
        uint64_t price;
    };

    struct TagChange
    {
    };
    struct TagExecute
    {
    };

    template<size_t BufferPoolSize>
    struct TreeBuffer
    {
        TreeBuffer()
        {
            madvise(buffer.data(), buffer.size(), MADV_HUGEPAGE);

            // To prevent page faults at runtime
            std::byte volatile* raw = buffer.data();
            for (size_t i = 0; i < buffer.size(); i += 4096)
            {
                raw[i] = std::byte {1};
            }
        }

        std::array<std::byte, BufferPoolSize> buffer {};
    };

    export template<Listener L,
                    uint32_t OrderPoolSize = DEFAULT_ORDER_POOL_SIZE,
                    size_t BufferPoolSize = DEFAULT_BUFFER_POOL_SIZE>
    class Book
    {
        using PmrAllocator = std::pmr::polymorphic_allocator<std::byte>;

      public:
        Book(L& listener)
            : listener_(&listener)
            , monotonicResource_(buffer_.buffer.data(), buffer_.buffer.size())
            , poolResource_(&monotonicResource_)
            , bidLevels_(PmrAllocator(&poolResource_))
            , askLevels_(PmrAllocator(&poolResource_))
            , orderToDetails_(PmrAllocator(&poolResource_))
        {
            orderToDetails_.reserve(OrderPoolSize);
        }

        Book(Book const&) = delete;
        Book& operator=(Book const&) = delete;
        Book(Book&&) = delete;
        Book& operator=(Book&&) = delete;

        using OrderMap = absl::flat_hash_map<
            uint64_t,
            OrderDetails,
            absl::container_internal::hash_default_hash<uint64_t>,
            absl::container_internal::hash_default_eq<uint64_t>,
            std::pmr::polymorphic_allocator<std::pair<uint64_t const, OrderDetails>>>;

        void setListener(L& listener) noexcept { listener_ = &listener; }

        void reset() noexcept
        {
            bidLevels_.clear();
            askLevels_.clear();
            orderToDetails_.clear();
            orderPool_.reset();
            bestBid_ = {};
            bestAsk_ = {};
        }

        [[nodiscard]] Level getBestBid() const noexcept { return bestBid_; }
        [[nodiscard]] Level getBestAsk() const noexcept { return bestAsk_; }

        /// Returns the bid level at the given depth. The time complexity is O(depth), so clients
        /// should use this carefully.
        [[nodiscard]] Level getBidLevel(uint32_t depth) const noexcept
        {
            return getLevelAtDepth(bidLevels_, depth);
        }

        /// Returns the ask level at the given depth. The time complexity is O(depth), so clients
        /// should use this carefully.
        [[nodiscard]] Level getAskLevel(uint32_t depth) const noexcept
        {
            return getLevelAtDepth(askLevels_, depth);
        }

        /// Returns the total buy volume strictly better than the given price.
        [[nodiscard]] quantity_t getBuyVolumeAhead(price_t targetPrice) const noexcept
        {
            auto it = bidLevels_.lower_bound(static_cast<uint64_t>(targetPrice));
            return quantity_t(bidLevels_.sum_exclusive(it));
        }

        /// Returns the total sell volume strictly better than the given price.
        [[nodiscard]] quantity_t getSellVolumeAhead(price_t targetPrice) const noexcept
        {
            auto it = askLevels_.lower_bound(static_cast<uint64_t>(targetPrice));
            return quantity_t(askLevels_.sum_exclusive(it));
        }

        /// Returns the total volume ahead of a specific order. Time complexity is O(log N + M)
        /// where N is the number of price levels and M is the order's position in its queue.
        [[nodiscard]] std::expected<quantity_t, BookError> getBuyVolumeAheadByOrder(
            uint64_t orderID) const noexcept
        {
            return getVolumeAheadByOrderImpl(bidLevels_, orderID);
        }

        /// Returns the total volume ahead of a specific order. Time complexity is O(log N + M)
        /// where N is the number of price levels and M is the order's position in its queue.
        [[nodiscard]] std::expected<quantity_t, BookError> getSellVolumeAheadByOrder(
            uint64_t orderID) const noexcept
        {
            return getVolumeAheadByOrderImpl(askLevels_, orderID);
        }

        /// Adds an order. Assumes that this function is called in the correct order by ID.
        ALPBOOK_INLINE void add(AddOrder order)
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

        /// Adds an order during recovery. Uses the order ID to search for the correct place to
        /// insert.
        void addUnordered(AddOrder order)
        {
            if (order.side == Side::Buy)
            {
                addUnorderedImpl<Side::Buy>(order.timestamp, order.id, order.price, order.shares);
            }
            else
            {
                addUnorderedImpl<Side::Sell>(order.timestamp, order.id, order.price, order.shares);
            }
        }

        std::expected<void, BookError> execute(ExecuteOrder order)
        {
            return decrementImpl(order.id, order.shares, TagExecute {});
        }
        std::expected<void, BookError> reduce(DecrementShares order)
        {
            return decrementImpl(order.id, order.shares, TagChange {});
        }

        ALPBOOK_INLINE std::expected<void, BookError> cancel(CancelOrder order)
        {
            auto it = orderToDetails_.find(order.id);
            if (it == orderToDetails_.end()) [[unlikely]]
            {
                return std::unexpected(BookError::MissingId);
            }
            auto [id, price] = it->second;
            Order& o = orderPool_[id];

            if (o.side == Side::Buy)
            {
                cancelImpl<Side::Buy>(id, price, o, TagChange {});
            }
            else
            {
                cancelImpl<Side::Sell>(id, price, o, TagChange {});
            }
            return {};
        }

        /// Replace is equivalent to cancelling an order and inserting a new one.
        /// Replaced orders lose time priority.
        ALPBOOK_INLINE std::expected<void, BookError> replace(ReplaceOrder order)
        {
            auto it = orderToDetails_.find(order.oldId);
            if (it == orderToDetails_.end()) [[unlikely]]
            {
                return std::unexpected(BookError::MissingId);
            }
            auto [id, price] = it->second;
            Order o = std::move(orderPool_[id]);
            if (o.side == Side::Buy)
            {
                cancelImpl<Side::Buy>(id, price, o, TagChange {});
                addImpl<Side::Buy>(order.timestamp, order.newId, order.price, order.shares);
            }
            else
            {
                cancelImpl<Side::Sell>(id, price, o, TagChange {});
                addImpl<Side::Sell>(order.timestamp, order.newId, order.price, order.shares);
            }
            return {};
        }

      private:
        template<Side S>
        auto& getLevels()
        {
            if constexpr (S == Side::Buy)
            {
                return bidLevels_;
            }
            else
            {
                return askLevels_;
            }
        }

        template<Side S>
        void updateBestAfterAdd(uint64_t price, uint32_t shares)
        {
            constexpr bool buySide = (S == Side::Buy);
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
        }

        template<Side S>
        ALPBOOK_INLINE void addImpl(uint64_t timestamp,
                                    uint64_t orderId,
                                    uint64_t price,
                                    uint32_t shares)
        {
            auto& levels = getLevels<S>();

            auto newId = orderPool_.allocate(timestamp, orderId, shares, INVALID_ID, INVALID_ID, S);

            if (levels.contains(price))
            {
                levels.update_key(price,
                                  [&](PriceLevel const& level)
                                  {
                                      PriceLevel updated = level;
                                      updated.totalShares += shares;

                                      // Link new order to end of queue
                                      orderPool_[newId].prev = level.tail;
                                      if (level.tail != INVALID_ID)
                                      {
                                          orderPool_[level.tail].next = newId;
                                      }
                                      updated.tail = newId;
                                      return updated;
                                  });
            }
            else
            {
                PriceLevel newLevel {.head = newId, .tail = newId, .totalShares = shares};
                levels.insert_v(price, newLevel);
            }

            updateBestAfterAdd<S>(price, shares);
            orderToDetails_[orderId] = {.orderId = newId, .price = price};
        }

        template<Side S>
        void addUnorderedImpl(uint64_t timestamp, uint64_t orderId, uint64_t price, uint32_t shares)
        {
            auto& levels = getLevels<S>();
            auto newPoolId =
                orderPool_.allocate(timestamp, orderId, shares, INVALID_ID, INVALID_ID, S);

            if (!levels.contains(price))
            {
                PriceLevel newLevel {.head = newPoolId, .tail = newPoolId, .totalShares = shares};
                levels.insert_v(price, newLevel);
            }
            else
            {
                levels.update_key(price,
                                  [&](PriceLevel const& level)
                                  {
                                      PriceLevel updated = level;
                                      updated.totalShares += shares;

                                      // Search from the back for insertion point
                                      uint32_t current = level.tail;
                                      uint32_t insertAfter = INVALID_ID;

                                      while (current != INVALID_ID)
                                      {
                                          if (orderPool_[current].id < orderId)
                                          {
                                              insertAfter = current;
                                              break;
                                          }
                                          current = orderPool_[current].prev;
                                      }

                                      if (insertAfter == INVALID_ID)
                                      {
                                          // New order is the new HEAD (smallest ID)
                                          orderPool_[newPoolId].next = updated.head;
                                          if (updated.head != INVALID_ID)
                                          {
                                              orderPool_[updated.head].prev = newPoolId;
                                          }
                                          updated.head = newPoolId;
                                      }
                                      else
                                      {
                                          // Insert after the found node
                                          uint32_t nextNode = orderPool_[insertAfter].next;
                                          orderPool_[newPoolId].prev = insertAfter;
                                          orderPool_[newPoolId].next = nextNode;
                                          orderPool_[insertAfter].next = newPoolId;
                                          if (nextNode != INVALID_ID)
                                          {
                                              orderPool_[nextNode].prev = newPoolId;
                                          }
                                          else
                                          {
                                              updated.tail = newPoolId;  // New tail
                                          }
                                      }
                                      return updated;
                                  });
            }

            updateBestAfterAdd<S>(price, shares);
            orderToDetails_[orderId] = {.orderId = newPoolId, .price = price};
        }

        template<typename Tag>
        ALPBOOK_INLINE std::expected<void, BookError> decrementImpl(uint64_t orderId,
                                                                    uint32_t shares,
                                                                    Tag tag)
        {
            auto it = orderToDetails_.find(orderId);
            if (it == orderToDetails_.end()) [[unlikely]]
            {
                return std::unexpected(BookError::MissingId);
            }
            auto [id, price] = it->second;
            Order& o = orderPool_[id];

            if (o.side == Side::Buy)
            {
                decrementInner<Side::Buy>(id, price, o, shares, tag);
            }
            else
            {
                decrementInner<Side::Sell>(id, price, o, shares, tag);
            }
            return {};
        }

        template<Side S, typename Tag>
        ALPBOOK_INLINE void decrementInner(
            uint64_t id, uint64_t price, Order& o, uint32_t shares, Tag tag)
        {
            constexpr bool buySide = (S == Side::Buy);
            auto& levels = getLevels<S>();

            if (o.shares == shares)
            {
                cancelImpl<S>(id, price, o, tag);
                return;
            }
            o.shares -= shares;

            uint32_t newTotalShares = 0;
            levels.update_key(price,
                              [&](PriceLevel const& level)
                              {
                                  PriceLevel updated = level;
                                  updated.totalShares -= shares;
                                  newTotalShares = updated.totalShares;
                                  return updated;
                              });

            if constexpr (std::is_same_v<Tag, TagExecute>)
            {
                notifyTrade(price, shares, S);
            }

            Level& best = buySide ? bestBid_ : bestAsk_;
            if (o.shares > 0 && price == best.price)
            {
                best.quantity = newTotalShares;
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
        ALPBOOK_INLINE void cancelImpl(uint32_t id, uint64_t price, Order const& o, Tag tag)
        {
            constexpr bool buySide = (S == Side::Buy);
            auto& levels = getLevels<S>();

            if constexpr (std::is_same_v<Tag, TagExecute>)
            {
                notifyTrade(price, o.shares, S);
            }

            if (o.prev != INVALID_ID)
            {
                orderPool_[o.prev].next = o.next;
            }
            if (o.next != INVALID_ID)
            {
                orderPool_[o.next].prev = o.prev;
            }

            // Get current level info to determine if we should erase or update
            auto it = levels.find(price);
            uint32_t oldTotal = it->second.totalShares;
            uint32_t newTotal = oldTotal - o.shares;

            Level& best = buySide ? bestBid_ : bestAsk_;
            bool wasTop = (price == best.price);

            if (newTotal == 0)
            {
                levels.erase_key(price);

                if (wasTop)
                {
                    best = getBest(levels);
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
            else
            {
                // Update level with new total and updated head/tail pointers
                levels.update_key(price,
                                  [&](PriceLevel const& level)
                                  {
                                      PriceLevel updated = level;
                                      updated.totalShares = newTotal;
                                      if (o.prev == INVALID_ID)  // was head
                                      {
                                          updated.head = o.next;
                                      }
                                      if (o.next == INVALID_ID)  // was tail
                                      {
                                          updated.tail = o.prev;
                                      }
                                      return updated;
                                  });

                if (wasTop)
                {
                    best.quantity = newTotal;
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

            orderPool_.deallocate(id);
            orderToDetails_.erase(o.id);
        }

        template<typename M>
        Level getBest(M const& levels) const
        {
            auto it = levels.begin();
            if (it == levels.end()) [[unlikely]]
            {
                return Level {};
            }
            return {.price = it->first, .quantity = it->second.totalShares};
        }

        template<typename M>
        Level getLevelAtDepth(M const& levels, uint32_t depth) const
        {
            auto it = levels.begin();
            if (it == levels.end()) [[unlikely]]
            {
                return Level {};
            }
            for (uint32_t i = 0; i < depth; i++)
            {
                ++it;
                if (it == levels.end()) [[unlikely]]
                {
                    return Level {};
                }
            }
            return {.price = it->first, .quantity = it->second.totalShares};
        }

        template<typename M>
        [[nodiscard]] std::expected<quantity_t, BookError> getVolumeAheadByOrderImpl(
            M const& levels, uint64_t orderID) const noexcept
        {
            auto it = orderToDetails_.find(orderID);
            if (it == orderToDetails_.end()) [[unlikely]]
            {
                return std::unexpected(BookError::MissingId);
            }

            auto [id, orderPrice] = it->second;
            auto levelIt = levels.find(orderPrice);
            if (levelIt == levels.end()) [[unlikely]]
            {
                return std::unexpected(BookError::MissingId);
            }
            auto const& level = levelIt->second;

            uint64_t volume = levels.sum_exclusive(levelIt);

            uint32_t currentId = level.head;
            while (currentId != INVALID_ID && currentId != id)
            {
                Order const& o = orderPool_[currentId];
                volume += o.shares;
                currentId = o.next;
            }
            return quantity_t(volume);
        }

        void notifyTrade(uint64_t price, uint32_t quantity, Side side)
        {
            listener_->onTrade(price, quantity, side);
        }

        bool isNewBest(uint64_t price, Level const& currentBest, Side side)
        {
            if (!currentBest.isValid())
            {
                return true;
            }
            if (side == Side::Buy)
            {
                return price > currentBest.price;
            }
            return price < currentBest.price;
        }

        L* listener_ {};

        // Memory resources for allocation
        alignas(std::max_align_t) TreeBuffer<BufferPoolSize> buffer_ {};
        std::pmr::monotonic_buffer_resource monotonicResource_;
        std::pmr::unsynchronized_pool_resource poolResource_;

        BidMap<PmrAllocator> bidLevels_;
        AskMap<PmrAllocator> askLevels_;

        internal::ObjectPool<Order> orderPool_ {OrderPoolSize};
        OrderMap orderToDetails_ {};

        // Cached versions of the best bid and ask
        Level bestBid_ {};
        Level bestAsk_ {};
    };
}  // namespace alpbook::nasdaq