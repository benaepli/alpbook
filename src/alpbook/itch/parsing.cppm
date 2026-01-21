module;

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <memory>
#include <variant>
#include <vector>

import alpbook.book.nasdaq;
import alpbook.dispatch;
import alpbook.common;

export module alpbook.itch.parsing;

export import :listener;

namespace alpbook::itch
{
    export enum class MessageOrigin : uint8_t
    {
        Live = 0,
        Recovery = 1,
        SnapshotStart = 2,
        SnapshotEnd = 3
    };

    using ItchBytes = std::array<uint8_t, 55>;

    export template<bool Benchmark = false>
    struct alignas(std::hardware_destructive_interference_size) ItchSlot
    {
        MessageOrigin type;
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

    export struct ItchExtractor
    {
        template<bool B>
        static uint16_t extractID(ItchSlot<B> const& msg)
        {
            return parseField<uint16_t>(msg.data, 1);
        }
    };

    static_assert(DispatchSlot<ItchSlot<true>>);
    static_assert(IDExtractor<ItchSlot<>, ItchExtractor>);

    uint64_t parseTimestamp(ItchBytes const& msg)
    {
        uint64_t const high = parseField<uint16_t>(msg, 5);
        uint64_t const low = parseField<uint32_t>(msg, 7);
        return (high << 32) | low;
    }

    /// Parse an ITCH message and dispatch to the listener.
    export template<OrderListener L>
    [[gnu::always_inline]] void parse(ItchBytes bytes, L& listener) noexcept
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