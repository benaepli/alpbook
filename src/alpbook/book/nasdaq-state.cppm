module;

import alpbook.common;

#include <cstdint>
#include <utility>

#include <bpptree/bpptree.hpp>
#include <bpptree/ordered.hpp>
#include <bpptree/summed.hpp>

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
        typename bpptree::BppTree<std::pair<uint64_t, PriceLevel>, 512, 512, 16, true, Allocator>
            ::template mixins<
                bpptree::OrderedBuilder<>::compare<Compare>,
                bpptree::SummedBuilder<VolumeExtractor>>::Transient;

    /// Maps from a price to a PriceLevel on the sell side (ascending order).
    template<typename Allocator = std::allocator<void>>
    using AskMap = OrderBookTree<bpptree::detail::MinComparator, Allocator>;

    /// Maps from a price to a PriceLevel on the buy side (descending order).
    template<typename Allocator = std::allocator<void>>
    using BidMap = OrderBookTree<bpptree::detail::MaxComparator, Allocator>;
}  // namespace alpbook::nasdaq