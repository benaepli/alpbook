module;

import alpbook.common;

#include <cstdint>

#include <absl/container/btree_map.h>

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

    /// Maps from a price to a PriceLevel on the sell side (ascending order).
    using AskMap = absl::btree_map<uint64_t, PriceLevel>;

    /// Maps from a price to a PriceLevel on the buy side (descending order).
    using BidMap = absl::btree_map<uint64_t, PriceLevel, std::greater<>>;
}  // namespace alpbook::nasdaq