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
    enum class SystemState : uint8_t
    {
        /// Waiting to process the first message.
        Waiting,

        /// Startup: in this state we process primarily just directory mapping messages and system
        /// events.
        Startup,

        Live,

        Recovery,

        EndOfDay,
    };

    export enum class SourceEvent : uint8_t
    {
        /// Gap recovery occurs if we don't need to restart from sequence number 1.
        GapRecovery,
        /// Total recovery indicates that all books should be cleared.
        TotalRecovery,
        RecoveryComplete,
        FatalError,
    };

    struct ItchView
    {
        uint64_t sequenceNumber;
        std::span<std::byte const> payload;
    };

    export template<typename T>
    concept ItchSource = requires(T t) {
        t.poll(std::declval<void (*)(ItchView)>(), std::declval<void (*)(SourceEvent)>());
        noexcept(t.poll());

        /// Force restarting must not block.
        t.forceRestart();
        noexcept(t.forceRestart());
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

        uint64_t session;

        /// The last processed sequence number.
        uint64_t sequenceNumber = 0;
        absl::flat_hash_map<alpbook::itch::StockTicker, uint16_t> tickers;
    };

    enum class ProcessResult : uint8_t
    {
        Ok,
        FatalInconsistency,
        EndOfDayEvent,
    };

    enum class SystemError
    {
        AlreadyRunning,
    };

    export template<typename Source, logging::OutputSink LogOutput>
        requires ItchSource<Source>
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

            while (running_.load(std::memory_order_relaxed))
            {
                switch (state_)
                {
                    case SystemState::Waiting:
                        state_ = runWaiting();
                        break;
                    case SystemState::Startup:
                    {
                        state_ = runStartup();
                        break;
                    }
                    case SystemState::Live:
                    {
                        state_ = runLive();
                        break;
                    }
                    case SystemState::Recovery:
                    {
                        state_ = runRecovery();
                        break;
                    }
                    case SystemState::EndOfDay:
                    {
                        state_ = runEndOfDay();
                        break;
                    }
                }
            }
            return {};
        }

      private:
        SystemState runWaiting() noexcept;
        SystemState runStartup() noexcept;
        SystemState runLive() noexcept
        {
            SystemState nextState = SystemState::Live;
            auto handleData = [this, &nextState](ItchView view)
            {
                auto result = processLiveMessage(view.payload);

                if (result == ProcessResult::FatalInconsistency) [[unlikely]]
                {
                    clearOrderBooks();
                    source_.forceRestart();
                    nextState = SystemState::Recovery;
                }
                else if (result == ProcessResult::EndOfDayEvent) [[unlikely]]
                {
                    nextState = SystemState::EndOfDay;
                }
            };

            auto handleEvent = [this, &nextState](SourceEvent event)
            {
                if (event == SourceEvent::GapRecovery)
                {
                    gapRecovery();
                    nextState = SystemState::Recovery;
                }
                else if (event == SourceEvent::TotalRecovery)
                {
                    clearOrderBooks();
                    nextState = SystemState::Recovery;
                }
                else
                {
                    clearOrderBooks();
                    source_.forceRestart();
                    nextState = SystemState::Recovery;
                }
            };

            while (running_.load(std::memory_order_relaxed) && nextState == SystemState::Live)
            {
                source_.poll(handleData, handleEvent);
            }
        }
        SystemState runRecovery() noexcept;
        SystemState runEndOfDay() noexcept;

        ProcessResult processLiveMessage(std::span<std::byte const> payload) noexcept;
        void gapRecovery() noexcept;
        void clearOrderBooks() noexcept;

        std::atomic<bool> running_ {false};

        Source source_;

        SystemConfig<LogOutput> config_;
        SystemState state_ = SystemState::Waiting;

        absl::flat_hash_set<alpbook::itch::StockTicker> trackedStocks_;
        std::optional<DayState> dayState_ = std::nullopt;
    };
}  // namespace alpdaq