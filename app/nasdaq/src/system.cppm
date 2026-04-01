module;

#include <array>
#include <bit>
#include <concepts>
#include <cstring>
#include <expected>
#include <memory>
#include <span>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

import alpdaq.logging;
import alpbook.itch;

export module alpdaq.system;

namespace alpdaq
{
    enum class SystemState
    {
        /// Waiting to process the first message.
        Waiting,

        /// Startup: in this state we process primarily just directory mapping messages and system
        /// events.
        Startup,

        Halt,

        Recovery,
        Live,

        EndOfDay,
    };

    enum class SourceError
    {
    };

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
            uint16_t val;
            std::memcpy(&val, message.data() + 18, sizeof(val));
            return std::byteswap(val);
        }
    };

    /// A source must return a sequence of bytes corresponding to an ITCH batch.
    export template<typename T, bool B>
    concept ItchSource = requires(T t) {
        { t.poll() } -> std::same_as<std::expected<MoldItch, SourceError>>;
        noexcept(t.poll());
    };

    template<logging::OutputSink O>
    struct SystemConfig
    {
        std::shared_ptr<logging::Logger<O>> logger;
        std::vector<alpbook::itch::StockTicker> stocks;
    };

    struct DayState
    {
        constexpr static auto DISPATCH_ARRAY_SIZE = std::numeric_limits<uint16_t>::max();

        uint64_t sessionId;

        /// The last processed sequence number.
        uint64_t sequenceNumber = 0;
        absl::flat_hash_map<alpbook::itch::StockTicker, uint16_t> tickers;
    };

    enum class SystemError
    {
        AlreadyRunning,
    };

    export template<typename Source, logging::OutputSink LogOutput, bool B>
        requires ItchSource<Source, B>
    class System
    {
      public:
        explicit System(Source&& source, SystemConfig<LogOutput> config) noexcept
            : source_(source)
            , config_(config)
        {
        }
        ~System() { stop(); }

        void stop() { running_.exchange(false); }

        std::expected<void, SystemError> run() noexcept
        {
            if (running_.exchange(true))
            {
                return std::unexpected(SystemError::AlreadyRunning);
            }

            runNormal();
            return {};
        }

      private:
        void runNormal() noexcept
        {
            while (running_.load(std::memory_order_relaxed))
            {
                auto result = source_.poll();
                if (!result)
                {
                    continue;
                }

                auto& mold = *result;
                if (!mold.valid())
                {
                    continue;
                }
            }
        }

        std::atomic<bool> running_ {false};

        Source source_;

        SystemConfig<LogOutput> config_;
        SystemState state_ = SystemState::Waiting;

        absl::flat_hash_set<alpbook::itch::StockTicker> trackedStocks_;
        std::optional<DayState> dayState_ = std::nullopt;
    };
}  // namespace alpdaq