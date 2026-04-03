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

import alpdaq.internal;
import alpdaq.logging;
import alpdaq.system.state;
import alpbook.itch;

export module alpdaq.system;

namespace alpdaq
{
    using internal::Overloaded;

    export template<typename T>
    concept ItchSource = requires(T t) {
        t.poll([](ItchView const&) {}, [](SourceEvent const&) {});
        noexcept(t.poll());

        /// Force restarting must not block.
        t.forceRestart();
        noexcept(t.forceRestart());
    };

    template<SystemLogger Logger>
    struct SystemConfig
    {
        std::shared_ptr<Logger> logger;
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

    export template<typename Source, SystemLogger Logger>
        requires ItchSource<Source>
    class System
    {
      public:
        explicit System(Source source, SystemConfig<Logger> config) noexcept
            : source_(std::move(source))
            , config_(std::move(config))
        {
        }
        ~System() { stop(); }

        void stop()
        {
            if (running_.exchange(false))
            {
                config_.logger->logSystemStopped();
            }
        }

        std::expected<void, SystemError> run() noexcept
        {
            if (running_.exchange(true))
            {
                return std::unexpected(SystemError::AlreadyRunning);
            }

            config_.logger->logSystemStarted();

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
                    config_.logger->logFatalInconsistency();
                    clearOrderBooks();
                    config_.logger->logForceRestart();
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
                    Overloaded {
                        [this, &nextState](GapRecovery const&)
                        {
                            config_.logger->logGapRecovery();
                            nextState = SystemState::Recovery;
                        },
                        [this, &nextState](TotalRecovery const&)
                        {
                            config_.logger->logTotalRecovery();
                            nextState = SystemState::Recovery;
                        },
                        [this](SessionChanged const& e)
                        { config_.logger->logSessionChange(e.newSession); },
                        [this, &nextState](auto const&)
                        {
                            clearOrderBooks();
                            config_.logger->logForceRestart();
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
                    config_.logger->logFatalInconsistency();
                    clearOrderBooks();
                    config_.logger->logForceRestart();
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
                    Overloaded {
                        [this, &nextState](RecoveryComplete const&)
                        {
                            config_.logger->logRecoveryComplete();
                            nextState = SystemState::Live;
                        },
                        [this, &nextState](GapRecovery const&)
                        {
                            config_.logger->logGapRecovery();
                            gapRecovery();
                            nextState = SystemState::Recovery;
                        },
                        [this, &nextState](TotalRecovery const&)
                        {
                            config_.logger->logTotalRecovery();
                            clearOrderBooks();
                            nextState = SystemState::Recovery;
                        },
                        [this, &nextState](auto const&)
                        {
                            clearOrderBooks();
                            config_.logger->logForceRestart();
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
            config_.logger->rotateSession();
            clearOrderBooks();
            return SystemState::Waiting;
        }

        void normalDataHandler(ItchView const& view, SystemState& nextState) noexcept
        {
            auto result = processLiveMessage(view.payload);

            if (result == ProcessResult::FatalInconsistency) [[unlikely]]
            {
                config_.logger->logFatalInconsistency();
                clearOrderBooks();
                config_.logger->logForceRestart();
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
                Overloaded {
                    [this, &nextState](GapRecovery const&)
                    {
                        config_.logger->logGapRecovery();
                        gapRecovery();
                        nextState = SystemState::Recovery;
                    },
                    [this, &nextState](TotalRecovery const&)
                    {
                        config_.logger->logTotalRecovery();
                        clearOrderBooks();
                        nextState = SystemState::Recovery;
                    },
                    [this, &nextState](auto const&)
                    {
                        clearOrderBooks();
                        config_.logger->logForceRestart();
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

        SystemConfig<Logger> config_;
        SystemState state_ = SystemState::Waiting;

        absl::flat_hash_set<alpbook::itch::StockTicker> trackedStocks_;
        std::optional<DayState> dayState_ = std::nullopt;
    };
}  // namespace alpdaq