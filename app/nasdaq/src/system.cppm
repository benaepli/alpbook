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

export module alpdaq.system;

import alpdaq.internal;
import alpdaq.logging;
import alpdaq.system.state;
import alpbook.itch;
import alpdaq.system.container;

namespace alpdaq
{
    using internal::Overloaded;
    using namespace alpbook;

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
        std::vector<itch::StockTicker> stocks;
    };

    struct DayState
    {
        constexpr static auto DISPATCH_ARRAY_SIZE = std::numeric_limits<uint16_t>::max();

        /// The last processed sequence number.
        uint64_t sequenceNumber = 0;
        absl::flat_hash_map<itch::StockTicker, uint16_t> tickers;
    };

    enum class WaitResult : uint8_t
    {
        Inconsistency,
        StartupEvent,
    };

    enum class StartupResult : uint8_t
    {
        Inconsistency,
        LiveEvent,
    };

    enum class ProcessResult : uint8_t
    {
        Ok,
        Inconsistency,
        EndOfDayEvent,
    };

    enum class SystemError
    {
        AlreadyRunning,
        FatalError,
    };

    export template<typename Source, SystemLogger Logger, typename Container>
        requires ItchSource<Source> && system::StrategyContainer<Container>
    class System
    {
      public:
        explicit System(Source source, SystemConfig<Logger> config, Container container) noexcept
            : source_(std::move(source))
            , config_(std::move(config))
            , container_(std::move(container))
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
            container_.init(config_.stocks);

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
                    case SystemState::Terminate:
                    {
                        config_.logger->rotateSession();
                        return std::unexpected(SystemError::FatalError);
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

                if (result == WaitResult::Inconsistency)
                {
                    config_.logger->logInconsistency();
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
                        [this, &nextState](FatalError const&)
                        {
                            clearOrderBooks();
                            nextState = SystemState::Terminate;
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

                if (result == StartupResult::Inconsistency) [[unlikely]]
                {
                    config_.logger->logInconsistency();
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
                        [this, &nextState](FatalError const&)
                        {
                            clearOrderBooks();
                            nextState = SystemState::Terminate;
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
            container_.clearAll();
            return SystemState::Waiting;
        }

        void normalDataHandler(ItchView const& view, SystemState& nextState) noexcept
        {
            auto result = processLiveMessage(view.payload);

            if (result == ProcessResult::Inconsistency) [[unlikely]]
            {
                config_.logger->logInconsistency();
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
                        container_.clearAll();
                        nextState = SystemState::Recovery;
                    },
                    [this, &nextState](FatalError const&)
                    {
                        clearOrderBooks();
                        nextState = SystemState::Terminate;
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

        WaitResult processWaitingMessage(std::span<std::byte const> payload) noexcept
        {
            if (payload.size() <= itch::SYSTEM_EVENT_MESSAGE_SIZE) [[unlikely]]
            {
                return WaitResult::Inconsistency;
            }

            itch::MessageClassification const classification = itch::classifyMessage(payload);
            switch (classification)
            {
                case itch::MessageClassification::Order:
                case itch::MessageClassification::StockDirectory:
                case itch::MessageClassification::StockTradingAction:
                case itch::MessageClassification::Ignored:
                {
                    return WaitResult::Inconsistency;
                }
                case itch::MessageClassification::SystemEvent:
                {
                    break;
                }
            }

            WaitResult result = WaitResult::Inconsistency;
            struct Listener
            {
                WaitResult& result;
                void startOfMessages(itch::events::StartOfMessages) const
                {
                    result = WaitResult::StartupEvent;
                }
                void startOfSystem(itch::events::StartOfSystem) const
                {
                    result = WaitResult::Inconsistency;
                }
                void startOfMarket(itch::events::StartOfMarket) const
                {
                    result = WaitResult::Inconsistency;
                }
                void endOfMarket(itch::events::EndOfMarket) const
                {
                    result = WaitResult::Inconsistency;
                }
                void endOfSystem(itch::events::EndOfSystem) const
                {
                    result = WaitResult::Inconsistency;
                }
                void endOfMessages(itch::events::EndOfMessages) const
                {
                    result = WaitResult::Inconsistency;
                }
            };

            Listener listener {result};
            itch::parseSystemEventMessage<Listener>(payload, listener);
            return result;
        }
        StartupResult processStartupMessage(std::span<std::byte const> payload) noexcept
        {
            if (payload.size() <= itch::SYSTEM_EVENT_MESSAGE_SIZE) [[unlikely]]
            {
                return StartupResult::Inconsistency;
            }

            itch::MessageClassification const classification = itch::classifyMessage(payload);
            switch (classification)
            {
                case itch::MessageClassification::Order:
                {
                    return StartupResult::Inconsistency;
                }
                case itch::MessageClassification::StockDirectory:
                {
                    // TODO: parse stock directory, register with container
                    break;
                }
                case itch::MessageClassification::StockTradingAction:
                {
                    // TODO: parse trading action, dispatch to container
                    break;
                }
                case itch::MessageClassification::SystemEvent:
                {
                    // TODO: parse system event, startOfSystem → LiveEvent
                    break;
                }
                case itch::MessageClassification::Ignored:
                {
                    break;
                }
            }

            return StartupResult::Inconsistency;
        }
        ProcessResult processLiveMessage(std::span<std::byte const> payload) noexcept;
        static void gapRecovery() noexcept
        {
            // For now: no action needed
        }
        void clearOrderBooks() noexcept { container_.clearAll(); }

        std::atomic<bool> running_ {false};

        Source source_;

        SystemConfig<Logger> config_;
        SystemState state_ = SystemState::Waiting;

        Container container_;
        std::optional<DayState> dayState_ = std::nullopt;
    };
}  // namespace alpdaq