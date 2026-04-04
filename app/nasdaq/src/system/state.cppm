module;

#include <array>
#include <concepts>
#include <cstdint>
#include <span>
#include <variant>

export module alpdaq.system.state;

namespace alpdaq
{
    export enum class SystemState : uint8_t
    {
        /// Waiting to process the first message.
        Waiting,

        /// Startup: in this state we process primarily just directory mapping messages and system
        /// events.
        Startup,

        /// Phase between start of system hours and start of market hours.
        PreMarket,

        Live,

        Recovery,

        EndOfDay,

        /// Terminate immediately flushes all logs and attempts to shut down.
        /// This happens upon an irrecoverable I/O error.
        Terminate,
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

    export template<typename T>
    concept SystemLogger = requires(T& t, SessionId id) {
        t.logPreMarket();
        t.logGapRecovery();
        t.logTotalRecovery();
        t.logRecoveryComplete();
        t.logForceRestart();
        t.logFatalInconsistency();
        t.logSessionChange(id);
        t.logSystemStarted();
        t.logSystemStopped();
        t.rotateSession();
    };

    export struct ItchView
    {
        uint64_t sequenceNumber;
        std::span<std::byte const> payload;
    };
}  // namespace alpdaq
