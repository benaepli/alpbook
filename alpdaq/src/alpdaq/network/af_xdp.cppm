module;

#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string_view>

export module alpdaq.network.af_xdp;

import alpdaq.system;

namespace alpdaq::network
{
    export enum class ParseError : uint8_t
    {
        InvalidFormat,
        InvalidOctet,
        InvalidPort,
    };

    export struct IpV4Endpoint
    {
        std::array<uint8_t, 4> address;
        uint16_t port;

        static std::expected<IpV4Endpoint, ParseError> parse(std::string_view);
    };

    inline std::expected<IpV4Endpoint, ParseError> IpV4Endpoint::parse(std::string_view input)
    {
        std::array<uint8_t, 4> address {};
        char const* ptr = input.data();
        char const* const end = input.data() + input.size();

        for (uint8_t i = 0; i < 4; ++i)
        {
            if (ptr >= end)
            {
                return std::unexpected(ParseError::InvalidOctet);
            }

            uint16_t octet {};
            auto [next, ec] = std::from_chars(ptr, end, octet);

            if (ec != std::errc {} || octet > 255)
            {
                return std::unexpected(ParseError::InvalidOctet);
            }

            address[i] = static_cast<uint8_t>(octet);

            if (i < 3)
            {
                if (next >= end || *next != '.')
                {
                    return std::unexpected(ParseError::InvalidOctet);
                }
                ptr = next + 1;
            }
            else
            {
                ptr = next;
            }
        }

        if (ptr >= end || *ptr != ':')
        {
            return std::unexpected(ParseError::InvalidFormat);
        }
        ++ptr;

        if (ptr >= end)
        {
            return std::unexpected(ParseError::InvalidPort);
        }
        uint32_t port {};
        auto [portEnd, portEc] = std::from_chars(ptr, end, port);
        if (portEc != std::errc {} || port > 65535 || portEnd != end)
        {
            return std::unexpected(ParseError::InvalidPort);
        }
        
        return IpV4Endpoint {
            .address = address,
            .port = static_cast<uint16_t>(port),
        };
    }

    export struct MulticastFeed
    {
        IpV4Endpoint group;
        std::optional<std::array<uint8_t, 4>> source;
    };

    export struct AfXdpConfig
    {
        struct LiveConfig
        {
            std::string interface;
            uint32_t queueId;
            MulticastFeed feed;
            uint32_t frameSize = 2048;
            uint32_t fillRingSize = 4096;
            uint32_t rxRingSize = 4096;

            /// The number of descriptors peeked during a refill.
            uint32_t rxBatch = 32;
        };

        struct RecoveryConfig
        {
            uint32_t sqEntries = 256;
            std::optional<uint32_t> cqEntries;

            IpV4Endpoint rewindServer;
            IpV4Endpoint glimpseServer;
        };

        LiveConfig liveConfig;
        RecoveryConfig recoveryConfig;

        std::chrono::nanoseconds rewindTimeout {500'000};
        std::chrono::nanoseconds glimpseConnectTimeout {2000'000'000};

        uint64_t recoveryBufferSlots = 1 << 16;
    };

    struct alignas(std::hardware_constructive_interference_size) Slot
    {
        uint16_t len;
        std::array<std::byte, 62> data;
    };

    struct RecoveryBuffer
    {
        /// Indicates whether a given slot index is occupied.
        std::vector<bool> occupied;
        std::vector<Slot> slots;
        /// The expected sequence number of slots[0] if it exists.
        std::optional<uint64_t> base;
        /// The number of elements buffered. The base only changes if count == 0.
        uint64_t count;
    };

    enum class Phase : uint8_t
    {
        Streaming,
        GapRecovery,
        SnapshotRecovery
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

    export template<SourceLogger Logger>
    class AfXdpSource
    {
      public:
        AfXdpSource() = default;

        template<typename DataCb, typename EventCb>
        void poll(DataCb onData, EventCb onEvent) noexcept
        {
        }

        void forceRestart() noexcept {}

      private:
        AfXdpConfig config_;
        std::shared_ptr<Logger> logger_;

        uint64_t expected_ = 0;
        Phase phase_ = Phase::Streaming;
        bool restartRequested_ = false;

        RecoveryBuffer buffer_;
    };

    static_assert(ItchSource<AfXdpSource<NullSourceLogger>>);
}  // namespace alpdaq::network