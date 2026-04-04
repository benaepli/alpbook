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
        absl::flat_hash_map<itch::StockTicker, uint16_t> tickers;

        /// Bit n is set if ID n corresponds to a subscribed ticker.
        std::bitset<std::numeric_limits<uint16_t>::max() + 1> subscribed;

        void reset()
        {
            tickers.clear();
            subscribed.reset();
        }
    };

    enum class WaitResult : uint8_t
    {
        Inconsistency,
        StartupEvent,
    };

    enum class StartupResult : uint8_t
    {
        Ok,
        Inconsistency,
        PreMarketEvent,
    };

    enum class PreMarketResult : uint8_t
    {
        Ok,
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
        /// runWaiting mostly only allows for one message: moving to the startup state.
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

        /// runStartup handles moving to the pre-market state and updating stock directories.
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
                else if (result == StartupResult::PreMarketEvent) [[unlikely]]
                {
                    config_.logger->logPreMarket();
                    container_.onPreMarket();
                    nextState = SystemState::PreMarket;
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

        SystemState runPreMarket() noexcept
        {
            SystemState nextState = SystemState::PreMarket;
            auto handleData = [this, &nextState](ItchView const& view)
            {
                auto result = processPreMarketMessage(view.payload);

                if (result == PreMarketResult::Inconsistency) [[unlikely]]
                {
                    config_.logger->logInconsistency();
                    clearOrderBooks();
                    config_.logger->logForceRestart();
                    source_.forceRestart();
                    nextState = SystemState::Recovery;
                }
                else if (result == PreMarketResult::LiveEvent) [[unlikely]]
                {
                    nextState = SystemState::Live;
                }
            };

            auto handleEvent = [this, &nextState](SourceEvent const& event)
            { normalEventHandler(event, nextState); };

            while (running_.load(std::memory_order_relaxed) && nextState == SystemState::PreMarket)
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
            dayState_.reset();
            return SystemState::Waiting;
        }

        /// The data handler for the normal (live and recovery) case.
        /// Calls processLiveMessage to update state appropriately.
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

        /// The event handler for the normal (live and recovery) case.
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
            auto const action = itch::parseStockTradingActionMessage(payload);
            container_.onTradingAction(assetId, action.state);
            return true;
        }

        WaitResult processWaitingMessage(std::span<std::byte const> payload) noexcept
        {
            if (payload.size() < itch::SYSTEM_EVENT_MESSAGE_SIZE) [[unlikely]]
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
            if (payload.size() < itch::SYSTEM_EVENT_MESSAGE_SIZE) [[unlikely]]
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
                    if (!handleStockDirectory(payload))
                    {
                        return StartupResult::Inconsistency;
                    }
                    return StartupResult::Ok;
                }
                case itch::MessageClassification::StockTradingAction:
                {
                    if (!handleStockTradingAction(payload))
                    {
                        return StartupResult::Inconsistency;
                    }
                    return StartupResult::Ok;
                }
                case itch::MessageClassification::SystemEvent:
                {
                    StartupResult result = StartupResult::Inconsistency;
                    struct Listener
                    {
                        StartupResult& result;
                        void startOfMessages(itch::events::StartOfMessages) const
                        {
                            result = StartupResult::Inconsistency;
                        }
                        void startOfSystem(itch::events::StartOfSystem) const
                        {
                            result = StartupResult::PreMarketEvent;
                        }
                        void startOfMarket(itch::events::StartOfMarket) const
                        {
                            result = StartupResult::Inconsistency;
                        }
                        void endOfMarket(itch::events::EndOfMarket) const
                        {
                            result = StartupResult::Inconsistency;
                        }
                        void endOfSystem(itch::events::EndOfSystem) const
                        {
                            result = StartupResult::Inconsistency;
                        }
                        void endOfMessages(itch::events::EndOfMessages) const
                        {
                            result = StartupResult::Inconsistency;
                        }
                    };

                    Listener listener {result};
                    itch::parseSystemEventMessage<Listener>(payload, listener);
                    return result;
                }
                case itch::MessageClassification::Ignored:
                {
                    break;
                }
            }

            return StartupResult::Ok;
        }

        PreMarketResult processPreMarketMessage(std::span<std::byte const> payload) noexcept
        {
            if (payload.size() < itch::SYSTEM_EVENT_MESSAGE_SIZE) [[unlikely]]
            {
                return PreMarketResult::Inconsistency;
            }

            itch::MessageClassification const classification = itch::classifyMessage(payload);
            switch (classification)
            {
                case itch::MessageClassification::Order:
                case itch::MessageClassification::StockDirectory:
                {
                    return PreMarketResult::Inconsistency;
                }
                case itch::MessageClassification::StockTradingAction:
                {
                    if (!handleStockTradingAction(payload)) [[unlikely]]
                    {
                        return PreMarketResult::Inconsistency;
                    }
                    return PreMarketResult::Ok;
                }
                case itch::MessageClassification::SystemEvent:
                {
                    PreMarketResult result = PreMarketResult::Inconsistency;
                    struct Listener
                    {
                        PreMarketResult& result;
                        void startOfMessages(itch::events::StartOfMessages) const
                        {
                            result = PreMarketResult::Inconsistency;
                        }
                        void startOfSystem(itch::events::StartOfSystem) const
                        {
                            result = PreMarketResult::Inconsistency;
                        }
                        void startOfMarket(itch::events::StartOfMarket) const
                        {
                            result = PreMarketResult::LiveEvent;
                        }
                        void endOfMarket(itch::events::EndOfMarket) const
                        {
                            result = PreMarketResult::Inconsistency;
                        }
                        void endOfSystem(itch::events::EndOfSystem) const
                        {
                            result = PreMarketResult::Inconsistency;
                        }
                        void endOfMessages(itch::events::EndOfMessages) const
                        {
                            result = PreMarketResult::Inconsistency;
                        }
                    };

                    Listener listener {result};
                    itch::parseSystemEventMessage<Listener>(payload, listener);
                    return result;
                }
                case itch::MessageClassification::Ignored:
                {
                    return PreMarketResult::Ok;
                }
            }

            return PreMarketResult::Inconsistency;
        }

        ProcessResult processLiveMessage(std::span<std::byte const> payload) noexcept;
        static void gapRecovery() noexcept
        {
            // For now: no action needed
        }
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