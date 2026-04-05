module;


#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <span>

export module alpdaq.network;

import alpbook.itch;

namespace alpdaq::network
{
    /// This struct represents the underlying ITCH/MoldUDP64 message.
    /// Invariant: an ITCH message must have at least 20 bytes. We do not check this
    /// in the associated helper functions.
    ///
    /// In its header, it contains: 10 bytes for the session, 8 bytes for the sequence number,
    /// and 2 bytes for the message count.
    export struct MoldItch
    {
        std::span<std::byte> message;

        bool valid() const noexcept { return message.size() >= 20; }

        std::array<uint8_t, 10> session() const noexcept
        {
            std::array<uint8_t, 10> s;
            std::memcpy(s.data(), message.data(), 10);
            return s;
        }

        uint64_t sequenceNumber() const noexcept
        {
            uint64_t val;
            std::memcpy(&val, message.data() + 10, sizeof(val));
            return std::byteswap(val);
        }

        uint16_t messageCount() const noexcept
        {
            std::uint16_t val;
            std::memcpy(&val, message.data() + 18, sizeof(val));
            return std::byteswap(val);
        }
    };
}  // namespace alpdaq::network