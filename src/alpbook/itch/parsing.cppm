module;

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <memory>
#include <variant>
#include <vector>

#include "alpbook/internal/hints.hpp"

import alpbook.book.nasdaq;
import alpbook.common;
import alpbook.itch.messages;

export module alpbook.itch.parsing;

export import :listener;

namespace alpbook::itch
{
    using ItchBytes = std::array<uint8_t, 55>;

    export template<bool Benchmark = false>
    struct alignas(std::hardware_destructive_interference_size) ItchSlot
    {
        ItchBytes data;
        [[no_unique_address]] std::conditional_t<Benchmark, int64_t, std::monostate>
            dispatchTimestamp;
    };

    template<typename T>
        requires std::is_integral_v<T>
    T parseField(ItchBytes const& msg, size_t offset)
    {
        T val;
        std::memcpy(&val, msg.data() + offset, sizeof(T));
        return std::byteswap(val);
    }

    export uint16_t parseID(ItchBytes const& msg)
    {
        return parseField<uint16_t>(msg.data, 1);
    };

    uint64_t parseTimestamp(ItchBytes const& msg)
    {
        uint64_t const high = parseField<uint16_t>(msg, 5);
        uint64_t const low = parseField<uint32_t>(msg, 7);
        return (high << 32) | low;
    }

    export ALPBOOK_INLINE MessageClassification classifyMessage(ItchBytes const& msg)
    {
        char const msgType = msg[0];
        [[likely]] if (msgType == 'A' || msgType == 'F' || msgType == 'E' || msgType == 'C'
                       || msgType == 'X' || msgType == 'D' || msgType == 'U')
        {
            return MessageClassification::Order;
        }
        if (msgType == 'R')
        {
            return MessageClassification::StockDirectory;
        }
        if (msgType == 'S')
        {
            return MessageClassification::SystemEvent;
        }
        if (msgType == 'H')
        {
            return MessageClassification::StockTradingAction;
        }
        return MessageClassification::Ignored;
    }

    /// Parse an ITCH order-related message and dispatch to the listener.
    /// Order-related messages are the ones directly consumed by order books
    /// and should be parsed after you know the correct locate ID.
    export template<OrderListener L>
    ALPBOOK_INLINE void parseOrderMessage(ItchBytes bytes, L& listener) noexcept
    {
        char const msgType = bytes[0];

        switch (msgType)
        {
            case 'A':
            case 'F':
            {
                auto const timestamp = parseTimestamp(bytes);
                auto const id = parseField<uint64_t>(bytes, 11);
                auto const side = static_cast<char>(bytes[19]);
                auto const shares = parseField<uint32_t>(bytes, 20);
                auto const price = parseField<uint32_t>(bytes, 32);

                listener.add(AddOrder {.timestamp = timestamp,
                                       .id = id,
                                       .price = price,
                                       .shares = shares,
                                       .side = (side == 'B' ? Side::Buy : Side::Sell)});
                break;
            }

            case 'E':
            case 'C':
            {
                auto const id = parseField<uint64_t>(bytes, 11);
                auto const shares = parseField<uint32_t>(bytes, 19);

                listener.execute(ExecuteOrder {.id = id, .shares = shares});
                break;
            }

            case 'X':
            {
                auto const id = parseField<uint64_t>(bytes, 11);
                auto const shares = parseField<uint32_t>(bytes, 19);

                listener.reduce(DecrementShares {.id = id, .shares = shares});
                break;
            }

            case 'D':
            {
                auto const id = parseField<uint64_t>(bytes, 11);

                listener.cancel(CancelOrder {.id = id});
                break;
            }

            case 'U':
            {
                auto const timestamp = parseTimestamp(bytes);
                auto const oldId = parseField<uint64_t>(bytes, 11);
                auto const newId = parseField<uint64_t>(bytes, 19);
                auto const shares = parseField<uint32_t>(bytes, 27);
                auto const price = parseField<uint32_t>(bytes, 31);

                listener.replace(ReplaceOrder {.timestamp = timestamp,
                                               .oldId = oldId,
                                               .newId = newId,
                                               .price = price,
                                               .shares = shares});
                break;
            }

            default:
                break;
        }
    }
}  // namespace alpbook::itch