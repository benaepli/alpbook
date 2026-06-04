module;

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cerrno>
#include <charconv>
#include <chrono>
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
import alpdaq.system.state;
import alpdaq.network;

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
            /// SoupBinTCP login credentials. Username is space-padded to 6 bytes;
            /// password to 10 bytes.
            std::string glimpseUsername;
            std::string glimpsePassword;
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
        uint64_t count = 0;
        /// (highest seq seen + 1) - *base.
        uint64_t span = 0;

        /// Begin an episode at the first missing sequence.
        void openEpisode(uint64_t baseSeq) noexcept
        {
            assert(count == 0 && !base);
            base = baseSeq;
            span = 0;
        }

        /// Store the message at `seq` into the window. Returns false iff the slot index
        /// is past the window. Duplicates are dropped.
        [[nodiscard]] bool insert(uint64_t seq, std::span<std::byte const> payload) noexcept
        {
            assert(base && seq >= *base);
            if (payload.size() > slots.front().data.size())
            {
                return false;
            }
            uint64_t const idx = seq - *base;
            if (idx >= slots.size())
            {
                return false;
            }
            if (occupied[idx])
            {
                return true;
            }
            auto& slot = slots[idx];
            slot.len = static_cast<uint16_t>(payload.size());
            std::memcpy(slot.data.data(), payload.data(), payload.size());
            occupied[idx] = true;
            ++count;
            if (idx + 1 > span)
            {
                span = idx + 1;
            }
            return true;
        }

        /// If the slot at `expected` is occupied, return its payload (valid until the next
        /// insert at the same index), clear the bit, and decrement count. std::nullopt on a hole.
        std::optional<std::span<std::byte const>> tryDeliver(uint64_t expected) noexcept
        {
            assert(base && expected >= *base);
            uint64_t const idx = expected - *base;
            assert(idx < slots.size());

            if (!occupied[idx])
            {
                return std::nullopt;
            }
            occupied[idx] = false;
            --count;
            auto const& slot = slots[idx];
            return std::span<std::byte const>(slot.data.data(), slot.len);
        }

        /// End an episode after delivery has cleared everything.
        void closeEpisode() noexcept
        {
            assert(count == 0);
            base = std::nullopt;
            span = 0;
        }

        /// Abort an episode on escalation. Bulk-clears
        /// the dirty range so the bitmap returns to its between-episodes state.
        void abortEpisode() noexcept
        {
            std::fill(
                occupied.begin(), occupied.begin() + static_cast<std::ptrdiff_t>(span), false);
            count = 0;
            base = std::nullopt;
            span = 0;
        }
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
            LoggingIn,
            Snapshot,
            Done,
        };

        Stage stage = Stage::Idle;
        /// Sequence number to assign to the next incoming SoupBinTCP data packet.
        uint64_t nextSnapshotSeq = 0;
        std::vector<std::byte> rxBuffer;
        size_t rxLength = 0;
    };

    /// A whole MoldUDP64 packet ready.
    struct FullMessage
    {
        std::array<uint8_t, 10> session;
        uint64_t sequenceNumber;
        uint16_t messageCount;
        /// Points beyond the 20-byte header into UMEM.
        std::span<std::byte const> messageArea;
    };

    struct LiveMessage
    {
        uint64_t sequenceNumber;
        std::span<std::byte const> payload;
    };

    struct MoldMessage
    {
        std::span<std::byte const> payload;
        /// Offset of the following message within the area.
        size_t next;
    };

    /// Decodes the ITCH message at an offset. Returns std::nullopt if the area is truncated at or
    /// after offset.
    [[nodiscard]] inline std::optional<MoldMessage> decodeMoldMessage(
        std::span<std::byte const> area, size_t offset) noexcept
    {
        if (offset + 2 > area.size())
        {
            return std::nullopt;
        }
        uint16_t lenBe = 0;
        std::memcpy(&lenBe, area.data() + offset, sizeof(lenBe));
        uint16_t const len = std::byteswap(lenBe);
        if (offset + 2 + len > area.size())
        {
            return std::nullopt;
        }
        return MoldMessage {
            .payload = std::span(area.data() + offset + 2, len),
            .next = offset + 2 + static_cast<size_t>(len),
        };
    }

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
            if (restartRequested_)
            {
                restartRequested_ = false;
                enterSnapshotRecovery();
                return;
            }
            if (recoveryCompletePending_)
            {
                recoveryCompletePending_ = false;
                onEvent(SourceEvent {RecoveryComplete {}});
                return;
            }
            if (sessionChangePending_)
            {
                sessionChangePending_ = false;
                onEvent(SourceEvent {SessionChanged {currentSession_}});
                return;
            }

            serviceUringCq();

            switch (phase_)
            {
                case Phase::Streaming:
                    pollStreaming(onData, onEvent);
                    break;
                case Phase::GapRecovery:
                    pollGapRecovery(onData, onEvent);
                    break;
                case Phase::SnapshotRecovery:
                    pollSnapshotRecovery(onData, onEvent);
                    break;
            }
        }

        void forceRestart() noexcept { restartRequested_ = true; }

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
                    || live.rxBatch > live.rxRingSize || config.recoveryBufferSlots == 0
                    || config.recoveryConfig.sqEntries == 0)
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
            buffer_.span = 0;

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

            // Sizing to sqEntries should, for all intents and purposes, be sufficient.
            rewindTxPool_.assign(recovery.sqEntries, {});
            rewindTxIdx_ = 0;

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

            // Pre-allocate the recv buffer and build the heartbeat.
            glimpse_.rxBuffer.resize(16384);
            glimpse_.rxLength = 0;
            {
                uint16_t const hbLen = std::byteswap(static_cast<uint16_t>(1));
                std::memcpy(glimpseHeartbeatTxBuf_.data(), &hbLen, 2);
                glimpseHeartbeatTxBuf_[2] = std::byte {'R'};
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

        /// Parse ETH (optionally one 802.1Q VLAN tag) + IPv4 (no options) + UDP from a
        /// captured frame and return a span over the UDP payload. Returns std::nullopt on any
        /// validation failure.
        static std::optional<std::span<std::byte const>> parseUdpPayload(std::byte const* frame,
                                                                         std::size_t len) noexcept
        {
            constexpr std::size_t ethHeaderSize = 14;
            if (len < ethHeaderSize)
            {
                return std::nullopt;
            }

            uint16_t ethertype = 0;
            std::memcpy(&ethertype, frame + 12, 2);
            ethertype = std::byteswap(ethertype);

            std::size_t offset = ethHeaderSize;
            if (ethertype == 0x8100)
            {
                if (len < ethHeaderSize + 4)
                {
                    return std::nullopt;
                }
                std::memcpy(&ethertype, frame + offset + 2, 2);
                ethertype = std::byteswap(ethertype);
                offset += 4;
            }
            if (ethertype != 0x0800)
            {
                return std::nullopt;
            }

            if (len < offset + 20)
            {
                return std::nullopt;
            }
            uint8_t const ihl = std::to_integer<uint8_t>(frame[offset]) & 0x0F;
            if (ihl != 5)
            {
                return std::nullopt;
            }
            uint8_t const proto = std::to_integer<uint8_t>(frame[offset + 9]);
            if (proto != 17)
            {
                return std::nullopt;
            }
            offset += 20;

            if (len < offset + 8)
            {
                return std::nullopt;
            }
            uint16_t udpLen = 0;
            std::memcpy(&udpLen, frame + offset + 4, 2);
            udpLen = std::byteswap(udpLen);
            if (udpLen < 8)
            {
                return std::nullopt;
            }
            std::size_t const payloadLen = static_cast<std::size_t>(udpLen) - 8;
            offset += 8;
            if (len < offset + payloadLen)
            {
                return std::nullopt;
            }
            return std::span(frame + offset, payloadLen);
        }

        /// Recycles the previously-completed batch (if any) and peeks a fresh one from
        /// the RX ring. On batch recycle: releases the RX descriptors and pushes their
        /// frame addresses back into the fill ring so the kernel can refill them. Returns
        /// true iff at least one descriptor is now available at the cursor.
        bool refillBatch() noexcept
        {
            if (rxCursor_.processed != rxCursor_.available)
            {
                return rxCursor_.available > 0;
            }
            if (rxCursor_.available > 0)
            {
                uint32_t fillIdx = 0;
                uint32_t const reserved =
                    xsk_ring_prod__reserve(&fillRing_, rxCursor_.available, &fillIdx);
                for (uint32_t i = 0; i < reserved; ++i)
                {
                    xdp_desc const* desc = xsk_ring_cons__rx_desc(&rxRing_, rxCursor_.idx + i);
                    *xsk_ring_prod__fill_addr(&fillRing_, fillIdx + i) = desc->addr;
                }
                if (reserved > 0)
                {
                    xsk_ring_prod__submit(&fillRing_, reserved);
                }
                xsk_ring_cons__release(&rxRing_, rxCursor_.available);
            }
            rxCursor_.idx = 0;
            rxCursor_.processed = 0;
            rxCursor_.available =
                xsk_ring_cons__peek(&rxRing_, config_.liveConfig.rxBatch, &rxCursor_.idx);
            return rxCursor_.available > 0;
        }

        /// Peek the next available MoldUDP packet without consuming it. Refills the cursor
        /// batch when needed. Silently skips frames that fail validation, heartbeats
        /// (count == 0), and end-of-session markers (count == 0xFFFF) by advancing past
        /// the descriptor and retrying. Returns std::nullopt iff the ring is empty.
        std::optional<FullMessage> peekPacket() noexcept
        {
            while (true)
            {
                if (!refillBatch())
                {
                    return std::nullopt;
                }

                xdp_desc const* desc =
                    xsk_ring_cons__rx_desc(&rxRing_, rxCursor_.idx + rxCursor_.processed);
                std::byte const* frame = umemArea_.data() + desc->addr;
                auto udpPayload = parseUdpPayload(frame, desc->len);
                if (!udpPayload)
                {
                    advancePacket();
                    continue;
                }

                MoldItch const mold {.message = *udpPayload};
                if (!mold.valid())
                {
                    advancePacket();
                    continue;
                }
                uint16_t const count = mold.messageCount();
                if (count == 0 || count == 0xFFFF)
                {
                    advancePacket();
                    continue;
                }

                return FullMessage {
                    .session = mold.session(),
                    .sequenceNumber = mold.sequenceNumber(),
                    .messageCount = count,
                    .messageArea = udpPayload->subspan(20),
                };
            }
        }

        /// Release the current descriptor (the one most recently returned by peekPacket).
        /// The batch's RX descriptors are released and the fill ring repopulated only
        /// when the batch is exhausted, on the next refillBatch().
        void advancePacket() noexcept
        {
            assert(rxCursor_.processed < rxCursor_.available);

            ++rxCursor_.processed;
            rxCursor_.msgOffset = 0;
            rxCursor_.msgRemain = 0;
        }

        /// Pop one ITCH message from currentPacket_ at the cursor's position. Returns
        /// std::nullopt (and abandons the packet) if its area is truncated before the
        /// message the cursor claims. The returned payload span is valid until the next
        /// advancePacket() (or popLiveMessage()).
        std::optional<LiveMessage> popLiveMessage() noexcept
        {
            assert(currentPacket_ && rxCursor_.msgRemain > 0);

            auto const& packet = *currentPacket_;
            auto const decoded = decodeMoldMessage(packet.messageArea, rxCursor_.msgOffset);
            if (!decoded)
            {
                currentPacket_.reset();
                advancePacket();
                return std::nullopt;
            }

            uint64_t const seq =
                packet.sequenceNumber + (packet.messageCount - rxCursor_.msgRemain);
            rxCursor_.msgOffset = decoded->next;
            --rxCursor_.msgRemain;
            if (rxCursor_.msgRemain == 0)
            {
                currentPacket_.reset();
                advancePacket();
            }
            return LiveMessage {.sequenceNumber = seq, .payload = decoded->payload};
        }

        static constexpr uint64_t URING_REWIND_SEND = 1;
        static constexpr uint64_t URING_REWIND_RECV = 2;
        static constexpr uint64_t URING_GLIMPSE_CONNECT = 3;
        static constexpr uint64_t URING_GLIMPSE_SEND = 4;
        static constexpr uint64_t URING_GLIMPSE_RECV = 5;

        /// Submits a MoldUDP rewind request for [seq, seq + count) via io_uring on
        /// rewindFd_.
        void submitRewindRequest(uint64_t seq, uint16_t count) noexcept
        {
            auto& buf = rewindTxPool_[rewindTxIdx_];
            rewindTxIdx_ = static_cast<uint32_t>((rewindTxIdx_ + 1) % rewindTxPool_.size());

            std::memcpy(buf.data(), currentSession_.data(), 10);
            uint64_t const seqBe = std::byteswap(seq);
            std::memcpy(buf.data() + 10, &seqBe, 8);
            uint16_t const countBe = std::byteswap(count);
            std::memcpy(buf.data() + 18, &countBe, 2);

            io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
            if (sqe == nullptr)
            {
                logger_->logFeedError();
                return;
            }
            io_uring_prep_send(sqe, rewindFd_, buf.data(), buf.size(), 0);
            sqe->user_data = URING_REWIND_SEND;
            io_uring_submit(&ring_);

            lastRequest_ = std::chrono::steady_clock::now();
            logger_->logRewindRequest(seq, count);
        }

        /// Posts a recv on rewindFd_ via io_uring if one is not already outstanding.
        void ensureRewindRecvPosted() noexcept
        {
            if (rewindRecvPosted_)
            {
                return;
            }
            io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
            if (sqe == nullptr)
            {
                logger_->logFeedError();
                return;
            }
            io_uring_prep_recv(sqe, rewindFd_, rewindRxBuf_.data(), rewindRxBuf_.size(), 0);
            sqe->user_data = URING_REWIND_RECV;
            io_uring_submit(&ring_);
            rewindRecvPosted_ = true;
        }

        /// Resets the source to a clean SnapshotRecovery starting state. Used by
        /// forceRestart() and by overflow / session-change escalations.
        void enterSnapshotRecovery() noexcept
        {
            currentPacket_.reset();
            if (buffer_.base)
            {
                buffer_.abortEpisode();
            }
            expected_ = 0;
            currentSession_ = {};
            rxCursor_.msgOffset = 0;
            rxCursor_.msgRemain = 0;
            recoveryCompletePending_ = false;
            sessionChangePending_ = false;
            totalRecoveryEmitted_ = false;

            if (glimpseFd_ >= 0)
            {
                ::close(glimpseFd_);
                glimpseFd_ = -1;
            }
            glimpse_.stage = GlimpseState::Stage::Idle;
            glimpse_.nextSnapshotSeq = 0;
            glimpse_.rxLength = 0;
            glimpseRecvPosted_ = false;

            phase_ = Phase::SnapshotRecovery;
        }

        /// Walks the MoldUDP packet pkt, inserting every message into the recovery
        /// buffer. On insert overflow, logs and escalates to SnapshotRecovery; returns
        /// false in that case (caller should return too).
        bool bufferEntirePacket(FullMessage const& pkt) noexcept
        {
            currentPacket_ = pkt;
            rxCursor_.msgOffset = 0;
            rxCursor_.msgRemain = pkt.messageCount;
            while (rxCursor_.msgRemain > 0)
            {
                auto const msg = popLiveMessage();
                if (!msg)
                {
                    break;
                }
                // Drop messages below the episode base.
                if (buffer_.base && msg->sequenceNumber < *buffer_.base)
                {
                    continue;
                }
                if (!buffer_.insert(msg->sequenceNumber, msg->payload))
                {
                    logger_->logBufferOverflow();
                    enterSnapshotRecovery();
                    return false;
                }
            }
            return true;
        }

        /// Scans the buffer's occupied bitmap and submits a fresh rewind request for
        /// every unoccupied run in [0, span). Splits runs larger than 0xFFFF.
        void rescanAndResubmit() noexcept
        {
            if (!buffer_.base)
            {
                return;
            }
            uint64_t i = 0;
            while (i < buffer_.span)
            {
                if (buffer_.occupied[i])
                {
                    ++i;
                    continue;
                }
                uint64_t const runStart = i;
                while (i < buffer_.span && !buffer_.occupied[i])
                {
                    ++i;
                }
                uint64_t const length = i - runStart;
                uint64_t off = 0;
                while (off < length)
                {
                    uint16_t const chunk =
                        static_cast<uint16_t>(std::min<uint64_t>(length - off, 0xFFFFu));
                    submitRewindRequest(*buffer_.base + runStart + off, chunk);
                    off += chunk;
                }
            }
        }

        /// Drains the io_uring completion queue, dispatching each completion by tag.
        void serviceUringCq() noexcept
        {
            io_uring_cqe* cqe = nullptr;
            while (io_uring_peek_cqe(&ring_, &cqe) == 0)
            {
                uint64_t const tag = cqe->user_data;
                int const res = cqe->res;
                io_uring_cqe_seen(&ring_, cqe);

                switch (tag)
                {
                    case URING_REWIND_RECV:
                        handleRewindRecv(res);
                        break;
                    case URING_REWIND_SEND:
                        handleRewindSend(res);
                        break;
                    case URING_GLIMPSE_CONNECT:
                        handleGlimpseConnect(res);
                        break;
                    case URING_GLIMPSE_SEND:
                        handleGlimpseSend(res);
                        break;
                    case URING_GLIMPSE_RECV:
                        handleGlimpseRecv(res);
                        break;
                }
            }
        }

        /// Handles a rewind recv completion: ingests the response (only while in
        /// GapRecovery with an open buffer) and re-posts the recv.
        void handleRewindRecv(int res) noexcept
        {
            rewindRecvPosted_ = false;
            if (res < 0)
            {
                logger_->logFeedError();
            }
            else if (res > 0 && phase_ == Phase::GapRecovery && buffer_.base)
            {
                ingestRewindResponse(std::span(rewindRxBuf_.data(), static_cast<size_t>(res)));
            }
            ensureRewindRecvPosted();
        }

        /// Handles a rewind send completion. A negative res is a feed error.
        void handleRewindSend(int res) noexcept
        {
            if (res < 0)
            {
                logger_->logFeedError();
            }
        }

        /// Parses a MoldUDP64 rewind response and inserts each ITCH message into the
        /// recovery buffer. On buffer overflow, logs and escalates to SnapshotRecovery.
        void ingestRewindResponse(std::span<std::byte const> packet) noexcept
        {
            MoldItch const mold {.message = packet};
            if (!mold.valid())
            {
                return;
            }
            uint16_t const count = mold.messageCount();
            if (count == 0 || count == 0xFFFF)
            {
                return;
            }

            uint64_t const firstSeq = mold.sequenceNumber();
            auto const area = packet.subspan(20);
            size_t offset = 0;
            for (uint16_t i = 0; i < count; ++i)
            {
                auto const decoded = decodeMoldMessage(area, offset);
                if (!decoded)
                {
                    break;
                }
                if (!buffer_.insert(firstSeq + i, decoded->payload))
                {
                    logger_->logBufferOverflow();
                    enterSnapshotRecovery();
                    break;
                }
                offset = decoded->next;
            }
        }

        /// Creates a TCP socket for GLIMPSE and submits an io_uring connect to the
        /// configured server. On failure, logs and leaves stage=Idle so retry can
        /// happen via forceRestart.
        void startGlimpseConnect() noexcept
        {
            glimpseFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
            if (glimpseFd_ < 0)
            {
                logger_->logFeedError();
                return;
            }
            glimpseConnectAddr_ = {};
            glimpseConnectAddr_.sin_family = AF_INET;
            glimpseConnectAddr_.sin_port = htons(config_.recoveryConfig.glimpseServer.port);
            std::memcpy(&glimpseConnectAddr_.sin_addr.s_addr,
                        config_.recoveryConfig.glimpseServer.address.data(),
                        config_.recoveryConfig.glimpseServer.address.size());

            io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
            if (sqe == nullptr)
            {
                logger_->logFeedError();
                ::close(glimpseFd_);
                glimpseFd_ = -1;
                return;
            }
            io_uring_prep_connect(sqe,
                                  glimpseFd_,
                                  reinterpret_cast<sockaddr*>(&glimpseConnectAddr_),
                                  sizeof(glimpseConnectAddr_));
            sqe->user_data = URING_GLIMPSE_CONNECT;
            io_uring_submit(&ring_);
            glimpse_.stage = GlimpseState::Stage::Connecting;
        }

        /// Builds the SoupBinTCP Login Request into glimpseLoginTxBuf_ and submits a
        /// send via io_uring.
        void sendGlimpseLogin() noexcept
        {
            auto& buf = glimpseLoginTxBuf_;
            uint16_t const lenBe = std::byteswap(static_cast<uint16_t>(47));
            std::memcpy(buf.data(), &lenBe, 2);
            buf[2] = std::byte {'L'};

            auto fillField = [&buf](size_t pos, size_t width, std::string const& src)
            {
                size_t const copyLen = std::min(src.size(), width);
                for (size_t i = 0; i < copyLen; ++i)
                {
                    buf[pos + i] = static_cast<std::byte>(src[i]);
                }
                for (size_t i = copyLen; i < width; ++i)
                {
                    buf[pos + i] = std::byte {' '};
                }
            };
            fillField(3, 6, config_.recoveryConfig.glimpseUsername);
            fillField(9, 10, config_.recoveryConfig.glimpsePassword);
            // Requested session: all spaces is server's current session.
            for (size_t i = 19; i < 29; ++i)
            {
                buf[i] = std::byte {' '};
            }
            // Requested sequence: "0" right-justified in 20 ASCII bytes is snapshot.
            for (size_t i = 29; i < 48; ++i)
            {
                buf[i] = std::byte {' '};
            }
            buf[48] = std::byte {'0'};

            io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
            if (sqe == nullptr)
            {
                logger_->logFeedError();
                return;
            }
            io_uring_prep_send(sqe, glimpseFd_, buf.data(), buf.size(), 0);
            sqe->user_data = URING_GLIMPSE_SEND;
            io_uring_submit(&ring_);
        }

        /// Submits the pre-built client heartbeat frame as a response.
        void sendGlimpseHeartbeat() noexcept
        {
            if (glimpseFd_ < 0)
            {
                return;
            }
            io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
            if (sqe == nullptr)
            {
                logger_->logFeedError();
                return;
            }
            io_uring_prep_send(
                sqe, glimpseFd_, glimpseHeartbeatTxBuf_.data(), glimpseHeartbeatTxBuf_.size(), 0);
            sqe->user_data = URING_GLIMPSE_SEND;
            io_uring_submit(&ring_);
        }

        /// Posts a recv on glimpseFd_ if one is not already outstanding. Buffer pointer
        /// is data() + rxLength (stable across the recv's lifetime since rxBuffer is
        /// pre-sized and never reallocated, and ingest's memmove only runs after
        /// completion).
        void ensureGlimpseRecvPosted() noexcept
        {
            if (glimpseRecvPosted_ || glimpseFd_ < 0)
            {
                return;
            }
            if (glimpse_.rxLength >= glimpse_.rxBuffer.size())
            {
                return;
            }
            io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
            if (sqe == nullptr)
            {
                logger_->logFeedError();
                return;
            }
            io_uring_prep_recv(sqe,
                               glimpseFd_,
                               glimpse_.rxBuffer.data() + glimpse_.rxLength,
                               glimpse_.rxBuffer.size() - glimpse_.rxLength,
                               0);
            sqe->user_data = URING_GLIMPSE_RECV;
            io_uring_submit(&ring_);
            glimpseRecvPosted_ = true;
        }

        /// Handles a GLIMPSE connect completion: on success, sends the Login Request
        /// and posts the first recv; on failure, logs and leaves stalled.
        void handleGlimpseConnect(int res) noexcept
        {
            if (res < 0)
            {
                logger_->logFeedError();
                return;
            }
            sendGlimpseLogin();
            glimpse_.stage = GlimpseState::Stage::LoggingIn;
            ensureGlimpseRecvPosted();
        }

        /// Handles a GLIMPSE send completion. A negative res is a feed error.
        void handleGlimpseSend(int res) noexcept
        {
            if (res < 0)
            {
                logger_->logFeedError();
            }
        }

        /// Appends new bytes to rxBuffer, ingests any complete SoupBin frames,
        /// and re-posts the recv if the connection is still alive.
        void handleGlimpseRecv(int res) noexcept
        {
            glimpseRecvPosted_ = false;
            if (res < 0)
            {
                logger_->logFeedError();
                return;
            }
            if (res == 0)
            {
                // Peer closed.
                return;
            }
            glimpse_.rxLength += static_cast<size_t>(res);
            ingestSoupBinFrames();
            if (glimpse_.stage != GlimpseState::Stage::Done && glimpseFd_ >= 0)
            {
                ensureGlimpseRecvPosted();
            }
        }

        /// Walks the rxBuffer, dispatching every complete SoupBinTCP frame to
        /// handleSoupBinFrame and sliding any partial tail back to the start.
        void ingestSoupBinFrames() noexcept
        {
            size_t offset = 0;
            while (offset + 2 <= glimpse_.rxLength)
            {
                uint16_t lenBe = 0;
                std::memcpy(&lenBe, glimpse_.rxBuffer.data() + offset, 2);
                uint16_t const frameLen = std::byteswap(lenBe);
                if (frameLen == 0)
                {
                    offset += 2;
                    continue;
                }
                if (offset + 2 + frameLen > glimpse_.rxLength)
                {
                    break;
                }
                uint8_t const type = std::to_integer<uint8_t>(glimpse_.rxBuffer[offset + 2]);
                auto const payload = std::span<std::byte const>(
                    glimpse_.rxBuffer.data() + offset + 3, static_cast<size_t>(frameLen - 1));
                handleSoupBinFrame(type, payload);
                offset += 2 + frameLen;
            }
            if (offset > 0)
            {
                if (offset < glimpse_.rxLength)
                {
                    std::memmove(glimpse_.rxBuffer.data(),
                                 glimpse_.rxBuffer.data() + offset,
                                 glimpse_.rxLength - offset);
                }
                glimpse_.rxLength -= offset;
            }
        }

        /// Dispatches a SoupBinTCP frame by type.
        void handleSoupBinFrame(uint8_t type, std::span<std::byte const> payload) noexcept
        {
            switch (type)
            {
                case 'L':  // Login Accepted
                    handleLoginAccepted(payload);
                    break;
                case 'J':  // Login Rejected
                    logger_->logFeedError();
                    break;
                case 'S':  // Sequenced Data
                    if (glimpse_.stage == GlimpseState::Stage::Snapshot && buffer_.base)
                    {
                        if (!buffer_.insert(glimpse_.nextSnapshotSeq, payload))
                        {
                            logger_->logBufferOverflow();
                        }
                        else
                        {
                            ++glimpse_.nextSnapshotSeq;
                        }
                    }
                    break;
                case 'H':  // Server Heartbeat
                    sendGlimpseHeartbeat();
                    break;
                case 'Z':  // End of Session
                    glimpse_.stage = GlimpseState::Stage::Done;
                    rescanAndResubmit();
                    logger_->logSnapshotComplete(glimpse_.nextSnapshotSeq);
                    if (glimpseFd_ >= 0)
                    {
                        ::close(glimpseFd_);
                        glimpseFd_ = -1;
                    }
                    break;
                case '+':
                default:
                    break;
            }
        }

        /// Parses a login accepted message,
        /// anchors the recovery buffer at the snapshot's first sequence, flips into
        /// GapRecovery, and defers the SessionChanged event to the next poll.
        void handleLoginAccepted(std::span<std::byte const> payload) noexcept
        {
            if (payload.size() < 30)
            {
                logger_->logFeedError();
                return;
            }
            std::array<uint8_t, 10> session;
            std::memcpy(session.data(), payload.data(), 10);

            char buf[21] = {};
            for (size_t i = 0; i < 20; ++i)
            {
                buf[i] = static_cast<char>(std::to_integer<uint8_t>(payload[10 + i]));
            }
            char const* p = buf;
            while (p < buf + 20 && *p == ' ')
            {
                ++p;
            }
            uint64_t startSeq = 0;
            std::from_chars(p, buf + 20, startSeq);

            currentSession_ = session;
            expected_ = startSeq;
            glimpse_.nextSnapshotSeq = startSeq;

            if (buffer_.base)
            {
                buffer_.abortEpisode();
            }
            buffer_.openEpisode(startSeq);

            sessionChangePending_ = true;
            glimpse_.stage = GlimpseState::Stage::Snapshot;
            phase_ = Phase::GapRecovery;
            logger_->logSnapshotStart();
        }

        template<typename DataCb, typename EventCb>
        void pollStreaming(DataCb onData, EventCb onEvent) noexcept
        {
            if (!currentPacket_)
            {
                auto pkt = peekPacket();
                if (!pkt)
                {
                    return;
                }

                if (pkt->session != currentSession_)
                {
                    currentSession_ = pkt->session;
                    onEvent(SourceEvent {SessionChanged {pkt->session}});
                    return;
                }

                if (expected_ != 0 && pkt->sequenceNumber != expected_)
                {
                    uint64_t const totalMissing = pkt->sequenceNumber - expected_;
                    logger_->logGapDetected(
                        expected_,
                        static_cast<uint16_t>(std::min<uint64_t>(totalMissing, 0xFFFFu)));

                    buffer_.openEpisode(expected_);

                    uint64_t off = 0;
                    while (off < totalMissing)
                    {
                        uint16_t const chunk =
                            static_cast<uint16_t>(std::min<uint64_t>(totalMissing - off, 0xFFFFu));
                        submitRewindRequest(expected_ + off, chunk);
                        off += chunk;
                    }
                    ensureRewindRecvPosted();

                    if (!bufferEntirePacket(*pkt))
                    {
                        return;
                    }

                    phase_ = Phase::GapRecovery;
                    onEvent(SourceEvent {GapRecovery {}});
                    return;
                }

                expected_ = pkt->sequenceNumber;
                currentPacket_ = *pkt;
                rxCursor_.msgOffset = 0;
                rxCursor_.msgRemain = pkt->messageCount;
            }

            auto const msg = popLiveMessage();
            if (!msg)
            {
                return;
            }
            expected_ = msg->sequenceNumber + 1;
            onData(ItchView {.sequenceNumber = msg->sequenceNumber, .payload = msg->payload});
        }

        template<typename DataCb, typename EventCb>
        void pollGapRecovery(DataCb onData, EventCb /*onEvent*/) noexcept
        {
            assert(buffer_.base);

            // First, we try to drain.
            if (auto const payload = buffer_.tryDeliver(expected_))
            {
                uint64_t const seq = expected_++;
                if (buffer_.count == 0)
                {
                    buffer_.closeEpisode();
                    phase_ = Phase::Streaming;
                    recoveryCompletePending_ = true;
                }
                onData(ItchView {.sequenceNumber = seq, .payload = *payload});
                return;
            }

            // Then we buffer one live packet if available and detect new gaps.
            if (auto pkt = peekPacket())
            {
                if (pkt->session != currentSession_)
                {
                    enterSnapshotRecovery();
                    return;
                }

                uint64_t const writeFrontier = *buffer_.base + buffer_.span;
                if (pkt->sequenceNumber > writeFrontier)
                {
                    uint64_t const newGapLength = pkt->sequenceNumber - writeFrontier;
                    uint64_t off = 0;
                    while (off < newGapLength)
                    {
                        uint16_t const chunk =
                            static_cast<uint16_t>(std::min<uint64_t>(newGapLength - off, 0xFFFFu));
                        submitRewindRequest(writeFrontier + off, chunk);
                        off += chunk;
                    }
                    logger_->logGapDetected(
                        writeFrontier,
                        static_cast<uint16_t>(std::min<uint64_t>(newGapLength, 0xFFFFu)));
                }

                if (!bufferEntirePacket(*pkt))
                {
                    return;
                }
            }

            // Timeout for runs.
            if (buffer_.base)
            {
                auto const now = std::chrono::steady_clock::now();
                if (now - lastRequest_ > config_.rewindTimeout)
                {
                    rescanAndResubmit();
                    logger_->logRewindTimeout();
                }
            }
        }

        /// SnapshotRecovery body. Emits TotalRecovery once on entry, kicks off the
        /// GLIMPSE connect, then idles. Actual work is done in the receive handler.
        template<typename DataCb, typename EventCb>
        void pollSnapshotRecovery(DataCb /*onData*/, EventCb onEvent) noexcept
        {
            if (!totalRecoveryEmitted_)
            {
                totalRecoveryEmitted_ = true;
                onEvent(SourceEvent {TotalRecovery {}});
                return;
            }
            if (glimpse_.stage == GlimpseState::Stage::Idle)
            {
                startGlimpseConnect();
            }
        }

        AfXdpConfig config_;
        std::shared_ptr<Logger> logger_;

        uint64_t expected_ = 0;
        Phase phase_ = Phase::Streaming;
        bool restartRequested_ = false;
        /// Set when GapRecovery's drain completes; the next poll emits RecoveryComplete.
        bool recoveryCompletePending_ = false;
        /// Set when handleLoginAccepted updates currentSession_.
        bool sessionChangePending_ = false;
        /// Single-shot gate so we emit TotalRecovery exactly once per snapshot episode.
        bool totalRecoveryEmitted_ = false;
        /// Session bytes of the most recently emitted SessionChanged event. All-zero
        /// before the first packet ever.
        std::array<uint8_t, 10> currentSession_ {};

        RecoveryBuffer buffer_;

        xsk_umem* umem_ = nullptr;
        xsk_socket* xsk_ = nullptr;
        xsk_ring_prod fillRing_ {};
        xsk_ring_cons rxRing_ {};
        xsk_ring_cons compRing_ {};
        std::span<std::byte> umemArea_;
        int joinFd_ = -1;

        RxCursor rxCursor_;
        /// The packet whose messages we are currently iterating. Engaged iff msgRemain > 0;
        /// popLiveMessage clears it when the last message in the packet is consumed.
        std::optional<FullMessage> currentPacket_;

        io_uring ring_ {};
        bool uringReady_ = false;
        /// A UDP file descriptor for rewinds, always open and kept idle.
        int rewindFd_ = -1;
        /// A TCP file descriptor for GLIMPSE recovery. Created lazily.
        int glimpseFd_ = -1;
        /// True iff a recv on rewindFd_ is currently outstanding in io_uring.
        bool rewindRecvPosted_ = false;
        /// Receive buffer for rewind responses; sized for one MoldUDP packet.
        std::array<std::byte, 2048> rewindRxBuf_ {};
        /// Stable backing storage for in-flight rewind request packets.
        std::vector<std::array<std::byte, 20>> rewindTxPool_;
        uint32_t rewindTxIdx_ = 0;
        /// True iff a recv on glimpseFd_ is currently outstanding in io_uring.
        bool glimpseRecvPosted_ = false;
        std::array<std::byte, 49> glimpseLoginTxBuf_ {};
        std::array<std::byte, 3> glimpseHeartbeatTxBuf_ {};
        /// Stable sockaddr for the io_uring connect to the GLIMPSE server.
        sockaddr_in glimpseConnectAddr_ {};

        std::chrono::steady_clock::time_point lastRequest_;
        GlimpseState glimpse_;
    };

    static_assert(ItchSource<AfXdpSource<NullSourceLogger>>);
}  // namespace alpdaq::network