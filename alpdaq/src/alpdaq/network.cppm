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
        std::span<std::byte const> message;

        [[nodiscard]] bool valid() const noexcept { return message.size() >= 20; }

        [[nodiscard]] std::array<uint8_t, 10> session() const noexcept
        {
            std::array<uint8_t, 10> s {};
            std::memcpy(s.data(), message.data(), 10);
            return s;
        }

        [[nodiscard]] uint64_t sequenceNumber() const noexcept
        {
            uint64_t val = 0;
            std::memcpy(&val, message.data() + 10, sizeof(val));
            return std::byteswap(val);
        }

        [[nodiscard]] uint16_t messageCount() const noexcept
        {
            std::uint16_t val = 0;
            std::memcpy(&val, message.data() + 18, sizeof(val));
            return std::byteswap(val);
        }
    };

    export template<typename T>
    concept SourceLogger = requires(T& t, uint64_t seq, uint16_t count) {
        /// A hole was detected: the messages in [seq, seq + count) are missing.
        t.logGapDetected(seq, count);
        /// A retransmission request was sent to the rewind server for [seq, seq + count).
        t.logRewindRequest(seq, count);
        /// An outstanding retransmission request timed out and is being retried.
        t.logRewindTimeout();
        /// A full (GLIMPSE) snapshot recovery has begun.
        t.logSnapshotStart();
        /// A snapshot completed; the live feed resumes from sequence number seq.
        t.logSnapshotComplete(seq);
        /// The recovery buffer overflowed, escalating gap recovery to total recovery.
        t.logBufferOverflow();
        /// An irrecoverable transport error occurred on the feed.
        t.logFeedError();
    };

    /// A no-op SourceLogger, useful as a default and in tests/benchmarks.
    export struct NullSourceLogger
    {
        void logGapDetected(uint64_t, uint16_t) noexcept {}
        void logRewindRequest(uint64_t, uint16_t) noexcept {}
        void logRewindTimeout() noexcept {}
        void logSnapshotStart() noexcept {}
        void logSnapshotComplete(uint64_t) noexcept {}
        void logBufferOverflow() noexcept {}
        void logFeedError() noexcept {}
    };
}  // namespace alpdaq::network