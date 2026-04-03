module;

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <print>
#include <string_view>
#include <variant>

import alpdaq.internal;
import alpdaq.logging.logger;
import alpdaq.logging.messages;
import alpdaq.system.state;

export module alpdaq.logging.file;

namespace alpdaq::logging
{
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
    export struct FatalInconsistency
    {
    };
    export struct SystemStarted
    {
    };
    export struct SystemStopped
    {
    };

    export using SystemData = std::variant<GapRecoveryStarted,
                                           TotalRecoveryStarted,
                                           RecoveryCompleted,
                                           ForceRestartTriggered,
                                           SessionChange,
                                           SessionRotated,
                                           FatalInconsistency,
                                           SystemStarted,
                                           SystemStopped>;

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

    export void writeSystemMessage(std::ostream& os, Message<SystemData> const& msg)
    {
        using internal::Overloaded;
        auto tag = levelTag(msg.level);
        std::visit(
            Overloaded {
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
                [&](FatalInconsistency const&)
                { std::print(os, "[{}] fatal inconsistency detected\n", tag); },
                [&](SystemStarted const&) { std::print(os, "[{}] system started\n", tag); },
                [&](SystemStopped const&) { std::print(os, "[{}] system stopped\n", tag); },
            },
            msg.data);
    }

    export template<typename F, typename Data>
    concept FailureHandler = requires(F f, Message<Data> msg) {
        { f(msg) } -> std::same_as<void>;
    };

    export using SystemFailureHandler = void (*)(Message<SystemData>);

    export template<FailureHandler<SystemData> OnFailure>
    struct FileOutput
    {
        std::filesystem::path path;
        OnFailure onFailure;
        std::ofstream stream;

        FileOutput& operator<<(Message<SystemData> msg) noexcept
        {
            if (!stream.is_open())
            {
                onFailure(msg);
                return *this;
            }

            writeSystemMessage(stream, msg);

            if (stream.fail())
            {
                onFailure(msg);
                stream.clear();
            }
            return *this;
        }

        static void rotate() noexcept
        {
            // No-op.
        }
    };

    static_assert(OutputSink<FileOutput<SystemFailureHandler>, SystemData>);

    export template<FailureHandler<SystemData> OnFailure>
    class SystemFileLogger
    {
      public:
        explicit SystemFileLogger(std::shared_ptr<Logger<FileOutput<OnFailure>, SystemData>> logger)
            : logger_(std::move(logger))
        {
        }

        void logGapRecovery()
        {
            logger_->tryEnqueueUnchecked({Level::Warn, GapRecoveryStarted {}});
        }

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

      private:
        std::shared_ptr<Logger<FileOutput<OnFailure>, SystemData>> logger_;
    };

    static_assert(alpdaq::SystemLogger<SystemFileLogger<SystemFailureHandler>>);
}  // namespace alpdaq::logging
