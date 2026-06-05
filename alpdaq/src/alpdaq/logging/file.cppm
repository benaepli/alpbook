module;

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <print>
#include <string_view>
#include <variant>

export module alpdaq.logging.file;

import alpdaq.internal;
import alpdaq.logging.logger;
import alpdaq.logging.messages;
import alpdaq.network;
import alpdaq.system.state;

namespace alpdaq::logging
{
    // System-level events (driven by alpdaq::SystemLogger).
    export struct PreMarketStarted
    {
    };
    export struct LiveStarted
    {
    };
    export struct AfterMarketStarted
    {
    };
    export struct AfterSystemHoursStarted
    {
    };
    export struct EndOfDayStarted
    {
    };
    export struct GapRecoveryStarted
    {
    };
    export struct TotalRecoveryStarted
    {
    };
    export struct RecoveryCompleted
    {
    };
    export struct ForceRestartTriggered
    {
    };
    export struct SessionChange
    {
        SessionId id;
    };
    export struct SessionRotated
    {
    };
    export struct InconsistencyDetected
    {
    };
    export struct FatalInconsistency
    {
    };
    export struct SystemStarted
    {
    };
    export struct SystemStopped
    {
    };

    // Source-level events (driven by alpdaq::network::SourceLogger).
    export struct GapDetected
    {
        uint64_t seq;
        uint16_t count;
    };
    export struct RewindRequested
    {
        uint64_t seq;
        uint16_t count;
    };
    export struct RewindTimedOut
    {
    };
    export struct SnapshotStarted
    {
    };
    export struct SnapshotCompleted
    {
        uint64_t seq;
    };
    export struct BufferOverflowed
    {
    };
    export struct FeedError
    {
    };

    export using LogData = std::variant<PreMarketStarted,
                                        LiveStarted,
                                        AfterMarketStarted,
                                        AfterSystemHoursStarted,
                                        EndOfDayStarted,
                                        GapRecoveryStarted,
                                        TotalRecoveryStarted,
                                        RecoveryCompleted,
                                        ForceRestartTriggered,
                                        SessionChange,
                                        SessionRotated,
                                        InconsistencyDetected,
                                        FatalInconsistency,
                                        SystemStarted,
                                        SystemStopped,
                                        GapDetected,
                                        RewindRequested,
                                        RewindTimedOut,
                                        SnapshotStarted,
                                        SnapshotCompleted,
                                        BufferOverflowed,
                                        FeedError>;

    constexpr std::string_view levelTag(Level level)
    {
        switch (level)
        {
            case Level::Info:
                return "INFO";
            case Level::Warn:
                return "WARN";
            case Level::Error:
                return "ERROR";
        }
        return "UNKNOWN";
    }

    export void writeMessage(std::ostream& os, Message<LogData> const& msg)
    {
        using internal::Overloaded;
        auto tag = levelTag(msg.level);
        std::visit(
            Overloaded {
                [&](PreMarketStarted const&) { std::print(os, "[{}] pre-market started\n", tag); },
                [&](LiveStarted const&) { std::print(os, "[{}] live started\n", tag); },
                [&](AfterMarketStarted const&)
                { std::print(os, "[{}] after-market started\n", tag); },
                [&](AfterSystemHoursStarted const&)
                { std::print(os, "[{}] after-system-hours started\n", tag); },
                [&](EndOfDayStarted const&) { std::print(os, "[{}] end-of-day started\n", tag); },
                [&](GapRecoveryStarted const&)
                { std::print(os, "[{}] gap recovery started\n", tag); },
                [&](TotalRecoveryStarted const&)
                { std::print(os, "[{}] total recovery started\n", tag); },
                [&](RecoveryCompleted const&) { std::print(os, "[{}] recovery completed\n", tag); },
                [&](ForceRestartTriggered const&)
                { std::print(os, "[{}] force restart triggered\n", tag); },
                [&](SessionChange const& e)
                {
                    std::print(os,
                               "[{}] session changed: "
                               "{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}\n",
                               tag,
                               e.id[0],
                               e.id[1],
                               e.id[2],
                               e.id[3],
                               e.id[4],
                               e.id[5],
                               e.id[6],
                               e.id[7],
                               e.id[8],
                               e.id[9]);
                },
                [&](SessionRotated const&) { std::print(os, "[{}] session rotated\n", tag); },
                [&](InconsistencyDetected const&)
                { std::print(os, "[{}] inconsistency detected\n", tag); },
                [&](FatalInconsistency const&)
                { std::print(os, "[{}] fatal inconsistency detected\n", tag); },
                [&](SystemStarted const&) { std::print(os, "[{}] system started\n", tag); },
                [&](SystemStopped const&) { std::print(os, "[{}] system stopped\n", tag); },
                [&](GapDetected const& e)
                { std::print(os, "[{}] gap detected: seq={} count={}\n", tag, e.seq, e.count); },
                [&](RewindRequested const& e)
                { std::print(os, "[{}] rewind requested: seq={} count={}\n", tag, e.seq, e.count); },
                [&](RewindTimedOut const&) { std::print(os, "[{}] rewind timed out\n", tag); },
                [&](SnapshotStarted const&)
                { std::print(os, "[{}] snapshot recovery started\n", tag); },
                [&](SnapshotCompleted const& e)
                { std::print(os, "[{}] snapshot complete: resume seq={}\n", tag, e.seq); },
                [&](BufferOverflowed const&)
                { std::print(os, "[{}] recovery buffer overflow\n", tag); },
                [&](FeedError const&) { std::print(os, "[{}] feed error\n", tag); },
            },
            msg.data);
    }

    export template<typename F, typename Data>
    concept FailureHandler = requires(F f, Message<Data> msg) {
        { f(msg) } -> std::same_as<void>;
    };

    export using LogFailureHandler = void (*)(Message<LogData>);

    export template<FailureHandler<LogData> OnFailure>
    struct FileOutput
    {
        std::filesystem::path path;
        OnFailure onFailure;
        std::ofstream stream;

        FileOutput& operator<<(Message<LogData> msg) noexcept
        {
            if (!stream.is_open())
            {
                onFailure(msg);
                return *this;
            }

            writeMessage(stream, msg);

            if (stream.fail())
            {
                onFailure(msg);
                stream.clear();
            }
            return *this;
        }

        void rotate() noexcept { stream.flush(); }
    };

    static_assert(OutputSink<FileOutput<LogFailureHandler>, LogData>);

    /// A file-backed asynchronous logger satisfying both alpdaq::SystemLogger and
    /// alpdaq::network::SourceLogger, so a single instance can serve the System and the
    /// AfXdpSource over one queue / one log file.
    export template<FailureHandler<LogData> OnFailure>
    class FileLogger
    {
      public:
        explicit FileLogger(std::shared_ptr<Logger<FileOutput<OnFailure>, LogData>> logger)
            : logger_(std::move(logger))
        {
        }

        // SystemLogger
        void logPreMarket() { logger_->tryEnqueueUnchecked({Level::Info, PreMarketStarted {}}); }

        void logLive() { logger_->tryEnqueueUnchecked({Level::Info, LiveStarted {}}); }

        void logAfterMarket()
        {
            logger_->tryEnqueueUnchecked({Level::Info, AfterMarketStarted {}});
        }

        void logAfterSystemHours()
        {
            logger_->tryEnqueueUnchecked({Level::Info, AfterSystemHoursStarted {}});
        }

        void logEndOfDay() { logger_->tryEnqueueUnchecked({Level::Info, EndOfDayStarted {}}); }

        void logGapRecovery() { logger_->tryEnqueueUnchecked({Level::Warn, GapRecoveryStarted {}}); }

        void logTotalRecovery()
        {
            logger_->tryEnqueueUnchecked({Level::Warn, TotalRecoveryStarted {}});
        }

        void logRecoveryComplete()
        {
            logger_->tryEnqueueUnchecked({Level::Info, RecoveryCompleted {}});
        }

        void logForceRestart()
        {
            logger_->tryEnqueueUnchecked({Level::Error, ForceRestartTriggered {}});
        }

        void logInconsistency()
        {
            logger_->tryEnqueueUnchecked({Level::Warn, InconsistencyDetected {}});
        }

        void logFatalInconsistency()
        {
            logger_->tryEnqueueUnchecked({Level::Error, FatalInconsistency {}});
        }

        void logSessionChange(SessionId id)
        {
            logger_->tryEnqueueUnchecked({Level::Info, SessionChange {id}});
        }

        void logSystemStarted() { logger_->tryEnqueueUnchecked({Level::Info, SystemStarted {}}); }

        void logSystemStopped() { logger_->tryEnqueueUnchecked({Level::Info, SystemStopped {}}); }

        void rotateSession() { logger_->flushSession(); }

        // SourceLogger (invoked from AfXdpSource::poll, which is noexcept).
        void logGapDetected(uint64_t seq, uint16_t count) noexcept
        {
            logger_->tryEnqueueUnchecked({Level::Warn, GapDetected {seq, count}});
        }

        void logRewindRequest(uint64_t seq, uint16_t count) noexcept
        {
            logger_->tryEnqueueUnchecked({Level::Info, RewindRequested {seq, count}});
        }

        void logRewindTimeout() noexcept
        {
            logger_->tryEnqueueUnchecked({Level::Warn, RewindTimedOut {}});
        }

        void logSnapshotStart() noexcept
        {
            logger_->tryEnqueueUnchecked({Level::Warn, SnapshotStarted {}});
        }

        void logSnapshotComplete(uint64_t seq) noexcept
        {
            logger_->tryEnqueueUnchecked({Level::Info, SnapshotCompleted {seq}});
        }

        void logBufferOverflow() noexcept
        {
            logger_->tryEnqueueUnchecked({Level::Error, BufferOverflowed {}});
        }

        void logFeedError() noexcept
        {
            logger_->tryEnqueueUnchecked({Level::Error, FeedError {}});
        }

      private:
        std::shared_ptr<Logger<FileOutput<OnFailure>, LogData>> logger_;
    };

    static_assert(alpdaq::SystemLogger<FileLogger<LogFailureHandler>>);
    static_assert(alpdaq::network::SourceLogger<FileLogger<LogFailureHandler>>);
}  // namespace alpdaq::logging
