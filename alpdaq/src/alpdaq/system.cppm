module;

#include <algorithm>
#include <array>
#include <bit>
#include <bitset>
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
import alpbook.book;
import alpdaq.system.container;

namespace alpdaq
{
    using internal::Overloaded;
    using namespace alpbook;

    export template<typename T>
    concept ItchSource = requires(T t) {
        t.poll([](ItchView const&) {}, [](SourceEvent const&) {});

        /// Force restarting must not block.
        t.forceRestart();
        noexcept(t.forceRestart());
    };

    export template<SystemLogger Logger>
    struct SystemConfig
    {
        std::shared_ptr<Logger> logger;
        std::vector<itch::StockTicker> stocks;
    };

    struct DayState
    {
        absl::flat_hash_map<itch::StockTicker, uint16_t> tickers;

        /// Bit n is set if ID n corresponds to a subscribed ticker.
        std::bitset<std::numeric_limits<uint16_t>::max() + 1> subscribed;

        void reset()
        {
            tickers.clear();
            subscribed.reset();
        }
    };

    enum class ItchSystemEvent : uint8_t
    {
        None,
        StartOfMessages,
        StartOfSystem,
        StartOfMarket,
        EndOfMarket,
        EndOfSystem,
        EndOfMessages,
    };

    ItchSystemEvent parseSystemEvent(std::span<std::byte const> payload) noexcept
    {
        struct Listener
        {
            ItchSystemEvent& result;
            void startOfMessages(itch::events::StartOfMessages) const
            {
                result = ItchSystemEvent::StartOfMessages;
            }
            void startOfSystem(itch::events::StartOfSystem) const
            {
                result = ItchSystemEvent::StartOfSystem;
            }
            void startOfMarket(itch::events::StartOfMarket) const
            {
                result = ItchSystemEvent::StartOfMarket;
            }
            void endOfMarket(itch::events::EndOfMarket) const
            {
                result = ItchSystemEvent::EndOfMarket;
            }
            void endOfSystem(itch::events::EndOfSystem) const
            {
                result = ItchSystemEvent::EndOfSystem;
            }
            void endOfMessages(itch::events::EndOfMessages) const
            {
                result = ItchSystemEvent::EndOfMessages;
            }
        };

        ItchSystemEvent result = ItchSystemEvent::None;
        Listener listener {result};
        itch::parseSystemEventMessage(payload, listener);
        return result;
    }

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

        /// Run is the primary entry point into the state machine.
        /// Only one thread can run at a time.
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
                    case SystemState::PreMarket:
                    {
                        state_ = runPreMarket();
                        break;
                    }
                    case SystemState::Live:
                    {
                        state_ = runLive();
                        break;
                    }
                    case SystemState::AfterMarket:
                    {
                        state_ = runAfterMarket();
                        break;
                    }
                    case SystemState::AfterSystemHours:
                    {
                        state_ = runAfterSystemHours();
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
        SystemState triggerInconsistency() noexcept
        {
            config_.logger->logInconsistency();
            clearOrderBooks();
            config_.logger->logForceRestart();
            source_.forceRestart();
            return SystemState::Recovery;
        }

        template<typename DataHandler, typename EventHandler>
        SystemState runStateLoop(SystemState currentState,
                                 DataHandler&& dataHandler,
                                 EventHandler&& eventHandler) noexcept
        {
            SystemState nextState = currentState;

            auto handleData = [&](ItchView const& view) { dataHandler(view.payload, nextState); };

            auto handleEvent = [&](SourceEvent const& event) { eventHandler(event, nextState); };

            while (running_.load(std::memory_order_relaxed) && nextState == currentState) [[likely]]
            {
                source_.poll(handleData, handleEvent);
            }

            return nextState;
        }

        SystemState runAwaitingSystemEvent(SystemState currentState,
                                           ItchSystemEvent expectedEvent,
                                           SystemState successState) noexcept
        {
            return runStateLoop(
                currentState,
                [&](std::span<std::byte const> payload, SystemState& nextState)
                {
                    auto classification = itch::classifyMessage(payload);

                    if (classification == itch::MessageClassification::SystemEvent)
                    {
                        if (parseSystemEvent(payload) == expectedEvent)
                        {
                            nextState = successState;
                        }
                        else
                        {
                            nextState = triggerInconsistency();
                        }
                    }
                },
                [this](SourceEvent const& event, SystemState& nextState)
                { normalEventHandler(event, nextState); });
        }

        SystemState runWaiting() noexcept
        {
            return runStateLoop(
                SystemState::Waiting,
                [this](std::span<std::byte const> payload, SystemState& nextState)
                {
                    if (payload.size() < itch::SYSTEM_EVENT_MESSAGE_SIZE) [[unlikely]]
                    {
                        nextState = triggerInconsistency();
                        return;
                    }

                    auto classification = itch::classifyMessage(payload);
                    if (classification == itch::MessageClassification::SystemEvent)
                    {
                        if (parseSystemEvent(payload) == ItchSystemEvent::StartOfMessages)
                        {
                            nextState = SystemState::Startup;
                        }
                        else
                        {
                            nextState = triggerInconsistency();
                        }
                    }
                    else if (classification != itch::MessageClassification::Ignored)
                    {
                        nextState = triggerInconsistency();
                    }
                },
                [this](SourceEvent const& event, SystemState& nextState)
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
                });
        }

        SystemState runStartup() noexcept
        {
            return runStateLoop(
                SystemState::Startup,
                [this](std::span<std::byte const> payload, SystemState& nextState)
                {
                    if (payload.size() < itch::SYSTEM_EVENT_MESSAGE_SIZE) [[unlikely]]
                    {
                        nextState = triggerInconsistency();
                        return;
                    }

                    auto classification = itch::classifyMessage(payload);
                    switch (classification)
                    {
                        case itch::MessageClassification::StockDirectory:
                        {
                            if (!handleStockDirectory(payload))
                            {
                                nextState = triggerInconsistency();
                            }
                            break;
                        }
                        case itch::MessageClassification::StockTradingAction:
                        {
                            if (!handleStockTradingAction(payload))
                            {
                                nextState = triggerInconsistency();
                            }
                            break;
                        }
                        case itch::MessageClassification::SystemEvent:
                        {
                            if (parseSystemEvent(payload) == ItchSystemEvent::StartOfSystem)
                            {
                                config_.logger->logPreMarket();
                                container_.onPreMarket();
                                nextState = SystemState::PreMarket;
                            }
                            else
                            {
                                nextState = triggerInconsistency();
                            }
                            break;
                        }
                        case itch::MessageClassification::Order:
                        {
                            nextState = triggerInconsistency();
                            break;
                        }
                        case itch::MessageClassification::Ignored:
                        {
                            break;
                        }
                    }
                },
                [this](SourceEvent const& event, SystemState& nextState)
                { normalEventHandler(event, nextState); });
        }

        SystemState runPreMarket() noexcept
        {
            return runStateLoop(
                SystemState::PreMarket,
                [this](std::span<std::byte const> payload, SystemState& nextState)
                {
                    if (payload.size() < itch::SYSTEM_EVENT_MESSAGE_SIZE) [[unlikely]]
                    {
                        nextState = triggerInconsistency();
                        return;
                    }

                    auto classification = itch::classifyMessage(payload);
                    switch (classification)
                    {
                        case itch::MessageClassification::StockTradingAction:
                        {
                            if (!handleStockTradingAction(payload)) [[unlikely]]
                            {
                                nextState = triggerInconsistency();
                            }
                            break;
                        }
                        case itch::MessageClassification::SystemEvent:
                        {
                            if (parseSystemEvent(payload) == ItchSystemEvent::StartOfMarket)
                            {
                                container_.resumeTrading();
                                nextState = SystemState::Live;
                            }
                            else
                            {
                                nextState = triggerInconsistency();
                            }
                            break;
                        }
                        case itch::MessageClassification::Order:
                        case itch::MessageClassification::StockDirectory:
                        {
                            nextState = triggerInconsistency();
                            break;
                        }
                        case itch::MessageClassification::Ignored:
                        {
                            break;
                        }
                    }
                },
                [this](SourceEvent const& event, SystemState& nextState)
                { normalEventHandler(event, nextState); });
        }

        SystemState runLive() noexcept
        {
            return runStateLoop(
                SystemState::Live,
                [this](std::span<std::byte const> payload, SystemState& nextState)
                { normalDataHandler(payload, nextState); },
                [this](SourceEvent const& event, SystemState& nextState)
                { normalEventHandler(event, nextState); });
        }

        SystemState runAfterMarket() noexcept
        {
            return runAwaitingSystemEvent(SystemState::AfterMarket,
                                          ItchSystemEvent::EndOfSystem,
                                          SystemState::AfterSystemHours);
        }

        SystemState runAfterSystemHours() noexcept
        {
            return runAwaitingSystemEvent(SystemState::AfterSystemHours,
                                          ItchSystemEvent::EndOfMessages,
                                          SystemState::EndOfDay);
        }

        SystemState runRecovery() noexcept
        {
            return runStateLoop(
                SystemState::Recovery,
                [this](std::span<std::byte const> payload, SystemState& nextState)
                { normalDataHandler(payload, nextState); },
                [this](SourceEvent const& event, SystemState& nextState)
                {
                    std::visit(
                        Overloaded {
                            [this, &nextState](RecoveryComplete const&)
                            {
                                config_.logger->logRecoveryComplete();
                                container_.resumeTrading();
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
                });
        }

        SystemState runEndOfDay() noexcept
        {
            config_.logger->rotateSession();
            container_.clearAll();
            dayState_.reset();
            return SystemState::Waiting;
        }

        void normalDataHandler(std::span<std::byte const> payload, SystemState& nextState) noexcept
        {
            auto result = processLiveMessage(payload);

            if (result == ProcessResult::Inconsistency) [[unlikely]]
            {
                nextState = triggerInconsistency();
            }
            else if (result == ProcessResult::EndOfDayEvent) [[unlikely]]
            {
                nextState = SystemState::AfterMarket;
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

        bool isSubscribed(uint32_t id) const noexcept { return dayState_.subscribed.test(id); }

        /// handleStockDirectory parses a stock directory message and updates the container
        /// and state appropriately. Returns true on success.
        bool handleStockDirectory(std::span<std::byte const> payload) noexcept
        {
            if (payload.size() < itch::STOCK_DIRECTORY_MESSAGE_SIZE) [[unlikely]]
            {
                return false;
            }
            auto const assetId = itch::parseID(payload);
            auto const dir = itch::parseStockDirectoryMessage(payload);

            // A bijective mapping.
            if (dayState_.tickers.contains(dir.stock) || dayState_.subscribed.test(assetId))
                [[unlikely]]
            {
                return false;
            }

            if (std::ranges::find(config_.stocks, dir.stock) != config_.stocks.end())
            {
                dayState_.tickers.emplace(dir.stock, assetId);
                dayState_.subscribed.set(assetId);
            }

            container_.onStockDirectory(assetId, dir.stock);
            return true;
        }

        /// Parses a stock trading action and updates the container. Returns true on success.
        bool handleStockTradingAction(std::span<std::byte const> payload) noexcept
        {
            if (payload.size() < itch::STOCK_TRADING_ACTION_MESSAGE_SIZE) [[unlikely]]
            {
                return false;
            }

            auto const assetId = itch::parseID(payload);
            if (!isSubscribed(assetId))
            {
                return true;
            }

            auto const action = itch::parseStockTradingActionMessage(payload);
            container_.onTradingAction(assetId, action.state);
            return true;
        }

        ProcessResult handleOrderMessage(std::span<std::byte const> payload) noexcept
        {
            auto const assetId = itch::parseID(payload);
            if (!isSubscribed(assetId))
            {
                return ProcessResult::Ok;
            }

            struct Listener
            {
                Container& container;
                uint16_t assetId;

                void add(nasdaq::AddOrder msg) { container.add(assetId, msg); }
                void execute(nasdaq::ExecuteOrder msg) { container.execute(assetId, msg); }
                void reduce(nasdaq::DecrementShares msg) { container.reduce(assetId, msg); }
                void cancel(nasdaq::CancelOrder msg) { container.cancel(assetId, msg); }
                void replace(nasdaq::ReplaceOrder msg) { container.replace(assetId, msg); }
            };

            Listener listener {container_, assetId};
            auto result = itch::parseOrderMessage(payload, listener);

            if (!result) [[unlikely]]
            {
                return ProcessResult::Inconsistency;
            }

            return ProcessResult::Ok;
        }

        ProcessResult processLiveMessage(std::span<std::byte const> payload) noexcept
        {
            auto classification = itch::classifyMessage(payload);

            switch (classification)
            {
                case itch::MessageClassification::Order:
                {
                    return handleOrderMessage(payload);
                }
                case itch::MessageClassification::SystemEvent:
                {
                    if (parseSystemEvent(payload) == ItchSystemEvent::EndOfMarket)
                    {
                        return ProcessResult::EndOfDayEvent;
                    }
                    return ProcessResult::Inconsistency;
                }
                case itch::MessageClassification::StockTradingAction:
                {
                    if (!handleStockTradingAction(payload)) [[unlikely]]
                    {
                        return ProcessResult::Inconsistency;
                    }
                    return ProcessResult::Ok;
                }
                case itch::MessageClassification::StockDirectory:
                {
                    return ProcessResult::Inconsistency;
                }
                case itch::MessageClassification::Ignored:
                {
                    return ProcessResult::Ok;
                }
            }

            return ProcessResult::Ok;
        }
        void gapRecovery() noexcept { container_.suspendTrading(); }
        void clearOrderBooks() noexcept
        {
            container_.clearAll();
            dayState_.reset();
        }

        std::atomic<bool> running_ {false};

        Source source_;

        SystemConfig<Logger> config_;
        SystemState state_ = SystemState::Waiting;

        Container container_;
        DayState dayState_;
    };
}  // namespace alpdaq