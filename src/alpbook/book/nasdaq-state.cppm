module;

#include <cstdint>
#include <utility>

#include <absl/container/btree_set.h>
#include <absl/container/flat_hash_map.h>
#include <bpptree/bpptree.hpp>
#include <bpptree/indexed.hpp>
#include <bpptree/ordered.hpp>
#include <bpptree/summed.hpp>

#include "alpbook/internal/hints.hpp"

import alpbook.common;
import alpbook.internal.pool;

export module alpbook.book.nasdaq:state;

namespace alpbook::nasdaq
{
    constexpr auto INVALID_ID = std::numeric_limits<uint32_t>::max();

    struct Order
    {
        uint64_t timestamp;
        uint64_t id;

        uint32_t shares;
        uint32_t next = INVALID_ID;
        uint32_t prev = INVALID_ID;
        Side side;
    };

    struct PriceLevel
    {
        uint32_t head = INVALID_ID;
        uint32_t tail = INVALID_ID;
        uint32_t totalShares = 0;
    };

    /// Extractor for BppTree Summed mixin - extracts totalShares from a price-level pair
    struct VolumeExtractor
    {
        // Overload for pair (used during iteration/summing)
        uint64_t operator()(std::pair<uint64_t, PriceLevel> const& val) const noexcept
        {
            return val.second.totalShares;
        }

        // Overload for separate key/value args (used during insert operations)
        uint64_t operator()(uint64_t /*key*/, PriceLevel const& val) const noexcept
        {
            return val.totalShares;
        }
    };

    template<typename Compare, typename Allocator = std::allocator<void>>
    using OrderBookTree =
        bpptree::BppTree<std::pair<uint64_t, PriceLevel>, 512, 512, 16, true, Allocator>::
            template mixins<bpptree::OrderedBuilder<>::compare<Compare>,
                            bpptree::IndexedBuilder<>,
                            bpptree::SummedBuilder<VolumeExtractor>>::Transient;

    /// Maps from a price to a PriceLevel on the sell side (ascending order).
    template<typename Allocator = std::allocator<void>>
    using AskMap = OrderBookTree<bpptree::detail::MinComparator, Allocator>;

    /// Maps from a price to a PriceLevel on the buy side (descending order).
    template<typename Allocator = std::allocator<void>>
    using BidMap = OrderBookTree<bpptree::detail::MaxComparator, Allocator>;

    /// Uses an augmented B+ Tree for O(log N) range sums. Most operations are O(log N).
    export struct PolicyTree
    {
    };

    /// Uses a BTree for ordering information and a hash map for updates. O(1) for most operations,
    /// but O(n) for querying volume information.
    export struct PolicyHash
    {
    };

    export template<typename Allocator>
    struct TreeStorage
    {
        using BidStore = BidMap<Allocator>;
        using AskStore = AskMap<Allocator>;

        BidStore bidLevels_;
        AskStore askLevels_;
        Allocator alloc_;
        TreeStorage(Allocator alloc)
            : bidLevels_(alloc)
            , askLevels_(alloc)
            , alloc_(alloc)
        {
        }

        void reset()
        {
            bidLevels_.clear();
            askLevels_.clear();
        }

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
        Level getBest() const
        {
            auto const& levels = (S == Side::Buy ? bidLevels_ : askLevels_);
            auto it = levels.begin();
            if (it == levels.end())
            {
                return Level {};
            }
            return {it->first, it->second.totalShares};
        }

        template<Side S>
        bool hasLevel(uint64_t price) const
        {
            return getLevels<S>().contains(price);
        }

        template<Side S>
        void insertNewLevel(uint64_t price, PriceLevel const& level)
        {
            getLevels<S>().insert_v(price, level);
        }

        template<Side S>
        void eraseLevel(uint64_t price)
        {
            getLevels<S>().erase_key(price);
        }

        template<Side S, typename Visitor>
        ALPBOOK_INLINE void updateLevel(uint64_t price, Visitor&& visitor)
        {
            getLevels<S>().update_key(price,
                                      [&](PriceLevel const& current)
                                      {
                                          PriceLevel copy = current;
                                          visitor(copy);
                                          return copy;
                                      });
        }

        template<Side S>
        uint64_t getVolumeAhead(uint64_t price) const
        {
            auto const& levels = (S == Side::Buy ? bidLevels_ : askLevels_);
            auto it = levels.lower_bound(price);
            return levels.sum_exclusive(it);
        }

        template<Side S>
        Level getLevelAtDepth(uint32_t depth) const
        {
            auto const& levels = (S == Side::Buy ? bidLevels_ : askLevels_);
            if (depth >= levels.size())
            {
                return Level {};
            }
            auto it = levels.find_index(depth);
            return {it->first, it->second.totalShares};
        }
    };

    export template<typename Allocator>
    struct HashStorage
    {
        using LevelMap = absl::flat_hash_map<uint64_t,
                                             uint32_t,
                                             absl::container_internal::hash_default_hash<uint64_t>,
                                             absl::container_internal::hash_default_eq<uint64_t>,
                                             Allocator>;

        using BidSet = absl::btree_set<uint64_t, std::greater<uint64_t>, Allocator>;
        using AskSet = absl::btree_set<uint64_t, std::less<uint64_t>, Allocator>;

        LevelMap bidData_;
        LevelMap askData_;
        BidSet bidOrder_;
        AskSet askOrder_;
        internal::ObjectPool<PriceLevel> levelPool_;

        HashStorage(Allocator alloc, uint32_t poolSize = 50'000)
            : bidData_(alloc)
            , askData_(alloc)
            , bidOrder_(alloc)
            , askOrder_(alloc)
            , levelPool_(poolSize)
        {
        }

        void reset()
        {
            bidData_.clear();
            askData_.clear();
            bidOrder_.clear();
            askOrder_.clear();
            levelPool_.reset();
        }

        PriceLevel& getLevel(uint32_t idx) { return levelPool_[idx]; }
        PriceLevel const& getLevel(uint32_t idx) const { return levelPool_[idx]; }

        template<Side S>
        Level getBest() const
        {
            auto const& set = (S == Side::Buy ? bidOrder_ : askOrder_);
            if (set.empty())
            {
                return Level {};
            }

            uint64_t price = *set.begin();

            uint32_t idx = (S == Side::Buy ? bidData_ : askData_).find(price)->second;
            return {price, getLevel(idx).totalShares};
        }

        template<Side S>
        bool hasLevel(uint64_t price) const
        {
            if constexpr (S == Side::Buy)
            {
                return bidData_.contains(price);
            }
            else
            {
                return askData_.contains(price);
            }
        }

        template<Side S>
        void insertNewLevel(uint64_t price, PriceLevel const& level)
        {
            auto idx = levelPool_.allocate();
            levelPool_[idx] = level;

            if constexpr (S == Side::Buy)
            {
                bidData_.emplace(price, idx);
                bidOrder_.insert(price);
            }
            else
            {
                askData_.emplace(price, idx);
                askOrder_.insert(price);
            }
        }

        template<Side S>
        void eraseLevel(uint64_t price)
        {
            auto& data = (S == Side::Buy ? bidData_ : askData_);

            auto it = data.find(price);
            if (it != data.end())
            {
                levelPool_.deallocate(it->second);
                data.erase(it);

                if constexpr (S == Side::Buy)
                {
                    bidOrder_.erase(price);
                }
                else
                {
                    askOrder_.erase(price);
                }
            }
        }

        template<Side S, typename Visitor>
        ALPBOOK_INLINE void updateLevel(uint64_t price, Visitor&& visitor)
        {
            auto& data = (S == Side::Buy ? bidData_ : askData_);
            auto it = data.find(price);

            if (it != data.end())
            {
                PriceLevel& currentLevel = levelPool_[it->second];
                currentLevel = visitor(currentLevel);
            }
        }

        template<Side S>
        uint64_t getVolumeAhead(uint64_t price) const
        {
            uint64_t volume = 0;
            auto const& orderSet = (S == Side::Buy ? bidOrder_ : askOrder_);
            auto const& dataMap = (S == Side::Buy ? bidData_ : askData_);

            for (uint64_t p : orderSet)
            {
                if (p == price)
                {
                    break;
                }

                if constexpr (S == Side::Buy)
                {
                    if (p < price)
                    {
                        break;
                    }
                }
                else
                {
                    if (p > price)
                    {
                        break;
                    }
                }

                uint32_t idx = dataMap.find(p)->second;
                volume += levelPool_[idx].totalShares;
            }

            return volume;
        }

        template<Side S>
        Level getLevelAtDepth(uint32_t depth) const
        {
            auto const& orderSet = (S == Side::Buy ? bidOrder_ : askOrder_);
            if (depth >= orderSet.size())
            {
                return Level {};
            }
            auto it = std::next(orderSet.begin(), depth);
            uint64_t price = *it;
            uint32_t idx = (S == Side::Buy ? bidData_ : askData_).find(price)->second;
            return {price, getLevel(idx).totalShares};
        }
    };
}  // namespace alpbook::nasdaq