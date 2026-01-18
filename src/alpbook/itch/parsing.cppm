module;

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

import alpbook.book.nasdaq;
import alpbook.dispatch;
import alpbook.common;

export module alpbook.itch.parsing;

export import :listener;

namespace alpbook::itch
{
    export struct alignas(std::hardware_destructive_interference_size) ItchSlot
    {
        std::array<uint8_t, 64> data;
    };

    template<typename T>
        requires std::is_integral_v<T>
    T parseField(ItchSlot const& msg, size_t offset)
    {
        T val;
        std::memcpy(&val, msg.data.data() + offset, sizeof(T));
        return std::byteswap(val);
    }

    export struct ItchExtractor
    {
        static uint16_t extractID(ItchSlot const& msg) { return parseField<uint16_t>(msg, 1); }
    };

    static_assert(DispatchSlot<ItchSlot>);
    static_assert(IDExtractor<ItchSlot, ItchExtractor>);

    inline uint64_t parseTimestamp(ItchSlot const& msg)
    {
        uint64_t const high = parseField<uint16_t>(msg, 5);
        uint64_t const low = parseField<uint32_t>(msg, 7);
        return (high << 32) | low;
    }

    /// Parse an ITCH message and dispatch to the listener.
    export template<OrderListener L>
    void parse(ItchSlot slot, L& listener) noexcept
    {
        char const msgType = slot.data[0];

        switch (msgType)
        {
            case 'A':
            case 'F':
            {
                auto const timestamp = parseTimestamp(slot);
                auto const id = parseField<uint64_t>(slot, 11);
                auto const side = static_cast<char>(slot.data[19]);
                auto const shares = parseField<uint32_t>(slot, 20);
                auto const price = parseField<uint32_t>(slot, 32);

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
                auto const id = parseField<uint64_t>(slot, 11);
                auto const shares = parseField<uint32_t>(slot, 19);

                listener.execute(ExecuteOrder {.id = id, .shares = shares});
                break;
            }

            case 'X':
            {
                auto const id = parseField<uint64_t>(slot, 11);
                auto const shares = parseField<uint32_t>(slot, 19);

                listener.reduce(DecrementShares {.id = id, .shares = shares});
                break;
            }

            case 'D':
            {
                auto const id = parseField<uint64_t>(slot, 11);

                listener.cancel(CancelOrder {.id = id});
                break;
            }

            case 'U':
            {
                auto const timestamp = parseTimestamp(slot);
                auto const oldId = parseField<uint64_t>(slot, 11);
                auto const newId = parseField<uint64_t>(slot, 19);
                auto const shares = parseField<uint32_t>(slot, 27);
                auto const price = parseField<uint32_t>(slot, 31);

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