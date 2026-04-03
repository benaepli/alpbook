module;

#include <array>
#include <bit>
#include <concepts>
#include <cstring>
#include <expected>
#include <memory>
#include <span>
#include <variant>
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

    export using SessionId = std::array<uint8_t, 10>;

    export struct SessionChanged
    {
        SessionId newSession;
    };

    /// Gap recovery occurs if we don't need to restart from sequence number 1.
    export struct GapRecovery
    {
    };
    /// Total recovery indicates that all books should be cleared.
    export struct TotalRecovery
    {
    };
    export struct RecoveryComplete
    {
    };
    export struct FatalError
    {
    };

    /// If a given message produces a source event, the source event should be processed
    /// before any data corresponding to that message.
    export using SourceEvent =
        std::variant<SessionChanged, GapRecovery, TotalRecovery, RecoveryComplete, FatalError>;

    template<typename... Ts>
    struct Overload : Ts...
    {
        using Ts::operator()...;
    };
    template<class... Ts>
    Overload(Ts...) -> Overload<Ts...>;

    export struct ItchView
    {
        uint64_t sequenceNumber;
        std::span<std::byte const> payload;
    }; 

    export template<typename T>
    concept ItchSource = requires(T t) {
        t.poll([](ItchView const&) {}, [](SourceEvent const&) {});
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

        /// The last processed sequence number.
        uint64_t sequenceNumber = 0;
        absl::flat_hash_map<alpbook::itch::StockTicker, uint16_t> tickers;
    };

    enum class WaitResult : uint8_t
    {
        FatalInconsistency,
        StartupEvent,
    };

    enum class StartupResult : uint8_t
    {
        FatalInconsistency,
        LiveEvent,
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
        explicit System(Source source, SystemConfig<LogOutput> config) noexcept
            : source_(std::move(source))
            , config_(std::move(config))
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
        SystemState runWaiting() noexcept
        {
            SystemState nextState = SystemState::Waiting;
            auto handleData = [this, &nextState](ItchView const& view)
            {
                auto result = processWaitingMessage(view.payload);

                if (result == WaitResult::FatalInconsistency)
                {
                    clearOrderBooks();
                    source_.forceRestart();
                    nextState = SystemState::Recovery;
                }
                else if (result == WaitResult::StartupEvent)
                {
                    nextState = SystemState::Startup;
                }
            };

            auto handleEvent = [this, &nextState](SourceEvent const& event)
            {
                std::visit(
                    Overload {
                        [this, &nextState](GapRecovery const&)
                        { nextState = SystemState::Recovery; },
                        [this, &nextState](TotalRecovery const&)
                        { nextState = SystemState::Recovery; },
                        [this](SessionChanged const&)
                        {
                            // A session change is permitted in this state.
                        },
                        [this, &nextState](auto const&)
                        {
                            clearOrderBooks();
                            source_.forceRestart();
                            nextState = SystemState::Recovery;
                        },
                    },
                    event);
            };

            while (running_.load(std::memory_order_relaxed) && nextState == SystemState::Waiting)
                [[likely]]
            {
                source_.poll(handleData, handleEvent);
            }

            return nextState;
        }

        SystemState runStartup() noexcept
        {
            SystemState nextState = SystemState::Startup;
            auto handleData = [this, &nextState](ItchView const& view)
            {
                auto result = processStartupMessage(view.payload);

                if (result == StartupResult::FatalInconsistency) [[unlikely]]
                {
                    clearOrderBooks();
                    source_.forceRestart();
                    nextState = SystemState::Recovery;
                }
                else if (result == StartupResult::LiveEvent) [[unlikely]]
                {
                    nextState = SystemState::Live;
                }
            };

            auto handleEvent = [this, &nextState](SourceEvent const& event)
            { normalEventHandler(event, nextState); };

            while (running_.load(std::memory_order_relaxed) && nextState == SystemState::Startup)
                [[likely]]
            {
                source_.poll(handleData, handleEvent);
            }

            return nextState;
        }

        SystemState runLive() noexcept
        {
            SystemState nextState = SystemState::Live;
            auto handleData = [this, &nextState](ItchView const& view)
            { normalDataHandler(view, nextState); };

            auto handleEvent = [this, &nextState](SourceEvent const& event)
            { normalEventHandler(event, nextState); };

            while (running_.load(std::memory_order_relaxed) && nextState == SystemState::Live)
                [[likely]]
            {
                source_.poll(handleData, handleEvent);
            }
            return nextState;
        }

        SystemState runRecovery() noexcept
        {
            SystemState nextState = SystemState::Recovery;
            auto handleData = [this, &nextState](ItchView const& view)
            { normalDataHandler(view, nextState); };

            auto handleEvent = [this, &nextState](SourceEvent const& event)
            {
                std::visit(
                    Overload {
                        [&nextState](RecoveryComplete const&) { nextState = SystemState::Live; },
                        [this, &nextState](GapRecovery const&)
                        {
                            gapRecovery();
                            nextState = SystemState::Recovery;
                        },
                        [this, &nextState](TotalRecovery const&)
                        {
                            clearOrderBooks();
                            nextState = SystemState::Recovery;
                        },
                        [this, &nextState](auto const&)
                        {
                            clearOrderBooks();
                            source_.forceRestart();
                            nextState = SystemState::Recovery;
                        },
                    },
                    event);
            };

            while (running_.load(std::memory_order_relaxed) && nextState == SystemState::Recovery)
                [[likely]]
            {
                source_.poll(handleData, handleEvent);
            }
            return nextState;
        }

        SystemState runEndOfDay() noexcept
        {
            clearOrderBooks();
            return SystemState::Waiting;
        }

        void normalDataHandler(ItchView const& view, SystemState& nextState) noexcept
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
        }

        void normalEventHandler(SourceEvent const& event, SystemState& nextState) noexcept
        {
            std::visit(
                Overload {
                    [this, &nextState](GapRecovery const&)
                    {
                        gapRecovery();
                        nextState = SystemState::Recovery;
                    },
                    [this, &nextState](TotalRecovery const&)
                    {
                        clearOrderBooks();
                        nextState = SystemState::Recovery;
                    },
                    [this, &nextState](auto const&)
                    {
                        clearOrderBooks();
                        source_.forceRestart();
                        nextState = SystemState::Recovery;
                    },
                },
                event);
        }

        WaitResult processWaitingMessage(std::span<std::byte const> payload) noexcept;
        StartupResult processStartupMessage(std::span<std::byte const> payload) noexcept;
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