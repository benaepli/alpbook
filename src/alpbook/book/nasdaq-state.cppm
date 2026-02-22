module;

#include <concepts>
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

    template<typename T>
    concept BookStorage = requires(T s,
                                   uint64_t price,
                                   uint32_t depth,
                                   PriceLevel pl,
                                   PriceLevel (*visitor)(PriceLevel const&)) {
        { s.reset() } -> std::same_as<void>;

        { s.template getLevelAtDepth<Side::Buy>(depth) } -> std::same_as<Level>;
        { s.template getVolumeAhead<Side::Buy>(price) } -> std::convertible_to<uint64_t>;
        { s.template hasLevel<Side::Buy>(price) } -> std::same_as<bool>;
        { s.template getBest<Side::Buy>() } -> std::same_as<Level>;

        s.template insertNewLevel<Side::Buy>(price, pl);
        s.template eraseLevel<Side::Buy>(price);
        s.template updateLevel<Side::Buy>(price, visitor);
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
        auto const& getLevels() const
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

      private:
        // Generic helpers that work with any OrderBookTree type
        template<typename LevelMap>
        static Level getBestImpl(LevelMap const& levels)
        {
            auto it = levels.begin();
            if (it == levels.end())
            {
                return Level {};
            }
            return {it->first, it->second.totalShares};
        }

        template<typename LevelMap>
        static uint64_t getVolumeAheadImpl(LevelMap const& levels, uint64_t price)
        {
            auto it = levels.lower_bound(price);
            return levels.sum_exclusive(it);
        }

        template<typename LevelMap>
        static Level getLevelAtDepthImpl(LevelMap const& levels, uint32_t depth)
        {
            if (depth >= levels.size())
            {
                return Level {};
            }
            auto it = levels.find_index(depth);
            return {it->first, it->second.totalShares};
        }

      public:
        template<Side S>
        Level getBest() const
        {
            if constexpr (S == Side::Buy)
            {
                return getBestImpl(bidLevels_);
            }
            else
            {
                return getBestImpl(askLevels_);
            }
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
                                      [&](PriceLevel const& current) { return visitor(current); });
        }

        template<Side S>
        uint64_t getVolumeAhead(uint64_t price) const
        {
            if constexpr (S == Side::Buy)
            {
                return getVolumeAheadImpl(bidLevels_, price);
            }
            else
            {
                return getVolumeAheadImpl(askLevels_, price);
            }
        }

        template<Side S>
        Level getLevelAtDepth(uint32_t depth) const
        {
            if constexpr (S == Side::Buy)
            {
                return getLevelAtDepthImpl(bidLevels_, depth);
            }
            else
            {
                return getLevelAtDepthImpl(askLevels_, depth);
            }
        }
    };

    export template<typename Allocator, uint32_t LevelPoolSize = 50'000>
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
        internal::ObjectPool<PriceLevel, std::pmr::polymorphic_allocator<PriceLevel>> levelPool_;

        HashStorage(Allocator alloc)
            : bidData_(alloc)
            , askData_(alloc)
            , bidOrder_(alloc)
            , askOrder_(alloc)
            , levelPool_(LevelPoolSize, alloc)
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

      private:
        // Generic helpers for btree_set iteration
        template<typename OrderSet, typename DataMap>
        Level getBestImpl(OrderSet const& orderSet, DataMap const& dataMap) const
        {
            if (orderSet.empty())
            {
                return Level {};
            }

            uint64_t price = *orderSet.begin();
            uint32_t idx = dataMap.find(price)->second;
            return {price, getLevel(idx).totalShares};
        }

        template<typename OrderSet, typename DataMap, typename CompareLess>
        uint64_t getVolumeAheadImpl(OrderSet const& orderSet,
                                    DataMap const& dataMap,
                                    uint64_t price,
                                    CompareLess compareLess) const
        {
            uint64_t volume = 0;
            for (uint64_t p : orderSet)
            {
                if (p == price)
                {
                    break;
                }
                if (compareLess(p, price))
                {
                    break;
                }

                uint32_t idx = dataMap.find(p)->second;
                volume += levelPool_[idx].totalShares;
            }
            return volume;
        }

        template<typename OrderSet, typename DataMap>
        Level getLevelAtDepthImpl(OrderSet const& orderSet,
                                  DataMap const& dataMap,
                                  uint32_t depth) const
        {
            if (depth >= orderSet.size())
            {
                return Level {};
            }
            auto it = std::next(orderSet.begin(), depth);
            uint64_t price = *it;
            uint32_t idx = dataMap.find(price)->second;
            return {price, getLevel(idx).totalShares};
        }

      public:
        template<Side S>
        Level getBest() const
        {
            if constexpr (S == Side::Buy)
            {
                return getBestImpl(bidOrder_, bidData_);
            }
            else
            {
                return getBestImpl(askOrder_, askData_);
            }
        }

        template<Side S>
        ALPBOOK_INLINE bool hasLevel(uint64_t price) const
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
        ALPBOOK_INLINE void insertNewLevel(uint64_t price, PriceLevel const& level)
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
        ALPBOOK_INLINE void eraseLevel(uint64_t price)
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
            if constexpr (S == Side::Buy)
            {
                return getVolumeAheadImpl(bidOrder_,
                                          bidData_,
                                          price,
                                          [](uint64_t p, uint64_t target) { return p < target; });
            }
            else
            {
                return getVolumeAheadImpl(askOrder_,
                                          askData_,
                                          price,
                                          [](uint64_t p, uint64_t target) { return p > target; });
            }
        }

        template<Side S>
        Level getLevelAtDepth(uint32_t depth) const
        {
            if constexpr (S == Side::Buy)
            {
                return getLevelAtDepthImpl(bidOrder_, bidData_, depth);
            }
            else
            {
                return getLevelAtDepthImpl(askOrder_, askData_, depth);
            }
        }
    };
}  // namespace alpbook::nasdaq