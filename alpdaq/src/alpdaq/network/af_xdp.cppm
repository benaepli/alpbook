module;

#include <array>
#include <bit>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include <arpa/inet.h>
#include <bpf/libbpf.h>
#include <liburing.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <xdp/libxdp.h>
#include <xdp/xsk.h>

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

    struct RxCursor
    {
        uint32_t idx = 0;
        uint32_t available = 0;
        uint32_t processed = 0;
        uint32_t msgOffset = 0;
        uint32_t msgRemain = 0;
    };

    struct GlimpseState
    {
        enum class Stage : uint8_t
        {
            Idle,
            Connecting,
            Snapshot
        };

        Stage stage = Stage::Idle;
        uint64_t joinSeq = 0;
        std::vector<std::byte> rxBuffer;
        size_t rxLength = 0;
    };

    export enum class OpenError : uint8_t
    {
        InvalidConfig,
        InterfaceNotFound,
        InsufficientPrivileges,
        UmemFailed,
        XdpSocketFailed,
        MulticastJoinFailed,
        UringInitFailed,
        SocketSetupFailed,
    };

    export template<SourceLogger Logger>
    class AfXdpSource
    {
      public:
        AfXdpSource() = default;
        ~AfXdpSource() { teardown(); }

        AfXdpSource(AfXdpSource const&) = delete;
        AfXdpSource& operator=(AfXdpSource const&) = delete;
        AfXdpSource(AfXdpSource&&) = delete;
        AfXdpSource& operator=(AfXdpSource&&) = delete;

        /// Initializes the source.
        static std::expected<std::unique_ptr<AfXdpSource>, OpenError> open(
            AfXdpConfig config, std::shared_ptr<Logger> logger)
        {
            auto source = std::make_unique<AfXdpSource>();
            if (auto result = source->init(std::move(config), std::move(logger)); !result)
            {
                return std::unexpected(result.error());
            }
            return source;
        }

        template<typename DataCb, typename EventCb>
        void poll(DataCb onData, EventCb onEvent) noexcept
        {
        }

        void forceRestart() noexcept {}

      private:
        /// Maps a positive errno to the closest OpenError, falling back to the caller's category.
        static OpenError classifyErrno(int err, OpenError fallback) noexcept
        {
            switch (err)
            {
                case EPERM:
                case EACCES:
                    return OpenError::InsufficientPrivileges;
                case ENODEV:
                case ENXIO:
                    return OpenError::InterfaceNotFound;
                default:
                    return fallback;
            }
        }

        std::expected<void, OpenError> init(AfXdpConfig config, std::shared_ptr<Logger> logger)
        {
            {
                auto const& live = config.liveConfig;
                if (!std::has_single_bit(live.frameSize) || live.frameSize < 2048
                    || !std::has_single_bit(live.fillRingSize)
                    || !std::has_single_bit(live.rxRingSize) || live.rxBatch == 0
                    || live.rxBatch > live.rxRingSize || config.recoveryBufferSlots == 0)
                {
                    return std::unexpected(OpenError::InvalidConfig);
                }
            }

            config_ = std::move(config);
            logger_ = std::move(logger);

            auto const& live = config_.liveConfig;
            auto const& recovery = config_.recoveryConfig;

            buffer_.slots.resize(config_.recoveryBufferSlots);
            buffer_.occupied.assign(config_.recoveryBufferSlots, false);
            buffer_.base = std::nullopt;
            buffer_.count = 0;

            // UMEM: one shared frame pool large enough for the fill and RX rings.
            uint64_t const frameCount = static_cast<uint64_t>(live.fillRingSize) + live.rxRingSize;
            size_t const umemBytes = frameCount * live.frameSize;
            void* area = ::mmap(
                nullptr, umemBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (area == MAP_FAILED)
            {
                return std::unexpected(classifyErrno(errno, OpenError::UmemFailed));
            }
            umemArea_ = std::span(static_cast<std::byte*>(area), umemBytes);

            xsk_umem_config umemConfig {};
            umemConfig.fill_size = live.fillRingSize;
            umemConfig.comp_size = live.rxRingSize;
            umemConfig.frame_size = live.frameSize;
            umemConfig.frame_headroom = 0;
            umemConfig.flags = 0;
            if (int err = xsk_umem__create(
                    &umem_, umemArea_.data(), umemBytes, &fillRing_, &compRing_, &umemConfig);
                err != 0)
            {
                return std::unexpected(classifyErrno(-err, OpenError::UmemFailed));
            }

            // libxdp attaches the default redirect program.
            xsk_socket_config xskConfig {};
            xskConfig.rx_size = live.rxRingSize;
            xskConfig.tx_size = 0;
            xskConfig.xdp_flags = 0;
            xskConfig.bind_flags = 0;
            if (int err = xsk_socket__create(&xsk_,
                                             live.interface.c_str(),
                                             live.queueId,
                                             umem_,
                                             &rxRing_,
                                             nullptr,
                                             &xskConfig);
                err != 0)
            {
                return std::unexpected(classifyErrno(-err, OpenError::XdpSocketFailed));
            }

            // Hand the kernel its RX buffers (the first fillRingSize frames of the pool).
            uint32_t fillIdx = 0;
            if (xsk_ring_prod__reserve(&fillRing_, live.fillRingSize, &fillIdx)
                != live.fillRingSize)
            {
                return std::unexpected(OpenError::XdpSocketFailed);
            }
            for (uint32_t i = 0; i < live.fillRingSize; ++i)
            {
                *xsk_ring_prod__fill_addr(&fillRing_, fillIdx + i) =
                    static_cast<uint64_t>(i) * live.frameSize;
            }
            xsk_ring_prod__submit(&fillRing_, live.fillRingSize);

            // Multicast membership: held open so the switch/NIC forwards the feed; the data is
            // redirected to the AF_XDP socket, so this fd is never read.
            joinFd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
            if (joinFd_ < 0)
            {
                return std::unexpected(classifyErrno(errno, OpenError::MulticastJoinFailed));
            }
            int reuse = 1;
            ::setsockopt(joinFd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

            sockaddr_in bindAddr {};
            bindAddr.sin_family = AF_INET;
            bindAddr.sin_port = htons(live.feed.group.port);
            bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
            if (::bind(joinFd_, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) < 0)
            {
                return std::unexpected(classifyErrno(errno, OpenError::MulticastJoinFailed));
            }

            auto toInAddr = [](std::array<uint8_t, 4> const& octets)
            {
                in_addr addr {};
                std::memcpy(&addr.s_addr, octets.data(), octets.size());
                return addr;
            };

            ip_mreq_source mreq {};
            mreq.imr_multiaddr = toInAddr(live.feed.group.address);
            mreq.imr_interface.s_addr = htonl(INADDR_ANY);

            int membershipOpt = IP_ADD_MEMBERSHIP;
            socklen_t mreqLen = sizeof(ip_mreq);
            if (live.feed.source)
            {
                mreq.imr_sourceaddr = toInAddr(*live.feed.source);
                membershipOpt = IP_ADD_SOURCE_MEMBERSHIP;
                mreqLen = sizeof(ip_mreq_source);
            }
            if (::setsockopt(joinFd_, IPPROTO_IP, membershipOpt, &mreq, mreqLen) < 0)
            {
                return std::unexpected(classifyErrno(errno, OpenError::MulticastJoinFailed));
            }

            // io_uring for the recovery transports.
            io_uring_params params {};
            if (recovery.cqEntries)
            {
                params.flags = IORING_SETUP_CQSIZE;
                params.cq_entries = *recovery.cqEntries;
            }
            if (int err = io_uring_queue_init_params(recovery.sqEntries, &ring_, &params); err < 0)
            {
                return std::unexpected(classifyErrno(-err, OpenError::UringInitFailed));
            }
            uringReady_ = true;

            // Rewind request socket connected.
            rewindFd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
            if (rewindFd_ < 0)
            {
                return std::unexpected(classifyErrno(errno, OpenError::SocketSetupFailed));
            }
            sockaddr_in rewindAddr {};
            rewindAddr.sin_family = AF_INET;
            rewindAddr.sin_port = htons(recovery.rewindServer.port);
            std::memcpy(&rewindAddr.sin_addr.s_addr,
                        recovery.rewindServer.address.data(),
                        recovery.rewindServer.address.size());
            if (::connect(rewindFd_, reinterpret_cast<sockaddr*>(&rewindAddr), sizeof(rewindAddr))
                < 0)
            {
                return std::unexpected(classifyErrno(errno, OpenError::SocketSetupFailed));
            }

            return {};
        }

        /// Releases every resource init() may have acquired in reverse order.
        void teardown() noexcept
        {
            if (rewindFd_ >= 0)
            {
                ::close(rewindFd_);
                rewindFd_ = -1;
            }
            if (glimpseFd_ >= 0)
            {
                ::close(glimpseFd_);
                glimpseFd_ = -1;
            }
            if (joinFd_ >= 0)
            {
                ::close(joinFd_);
                joinFd_ = -1;
            }
            if (uringReady_)
            {
                io_uring_queue_exit(&ring_);
                uringReady_ = false;
            }
            if (xsk_ != nullptr)
            {
                xsk_socket__delete(xsk_);
                xsk_ = nullptr;
            }
            if (umem_ != nullptr)
            {
                xsk_umem__delete(umem_);
                umem_ = nullptr;
            }
            if (!umemArea_.empty())
            {
                ::munmap(umemArea_.data(), umemArea_.size());
                umemArea_ = {};
            }
        }

        AfXdpConfig config_;
        std::shared_ptr<Logger> logger_;

        uint64_t expected_ = 0;
        Phase phase_ = Phase::Streaming;
        bool restartRequested_ = false;

        RecoveryBuffer buffer_;

        xsk_umem* umem_ = nullptr;
        xsk_socket* xsk_ = nullptr;
        xsk_ring_prod fillRing_ {};
        xsk_ring_cons rxRing_ {};
        xsk_ring_cons compRing_ {};
        std::span<std::byte> umemArea_;
        int joinFd_ = -1;

        RxCursor rxCursor_;

        io_uring ring_ {};
        bool uringReady_ = false;
        /// A UDP file descriptor for rewinds, always open and kept idle.
        int rewindFd_ = -1;
        /// A TCP file descriptor for GLIMPSE recovery. Created lazily.
        int glimpseFd_ = -1;

        std::chrono::steady_clock::time_point lastRequest_;
        GlimpseState glimpse_;
    };

    static_assert(ItchSource<AfXdpSource<NullSourceLogger>>);
}  // namespace alpdaq::network