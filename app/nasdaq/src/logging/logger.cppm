module;

#include <expected>
#include <filesystem>
#include <fstream>
#include <utility>
#include <variant>

#include "readerwriterqueue.h"

import alpdaq.logging.messages;
import alpbook.internal.backoff;

export module alpdaq.logging.logger;

namespace alpdaq::logging
{
    using namespace alpbook;
    constexpr uint32_t BUSY_THRESHOLD = 10000;
    constexpr uint32_t RELAX_THRESHOLD = std::numeric_limits<uint32_t>::max();

    export enum class LoggerError
    {
        AlreadyRunning,
        NotRunning,
        StoppedRunning,
    };

    export template<typename T>
    concept OutputSink = requires(T& t, Message msg) {
        { t << msg } -> std::same_as<T&>;
        noexcept(t << msg);
    };

    struct StopSignal
    {
    };

    struct alignas(std::hardware_destructive_interference_size) LogEntry
    {
        std::variant<StopSignal, Message> entry;
    };

    export template<OutputSink O>
    class Logger
    {
      public:
        explicit Logger(size_t bufferSize, O output)
            : queue_(bufferSize)
            , output_(std::move(output))
        {
        }

        ~Logger() { std::ignore = stop(); };

        Logger(Logger const&) = delete;
        Logger& operator=(Logger const&) = delete;
        Logger(Logger&&) = delete;
        Logger& operator=(Logger&&) = delete;

        /// Run the logging loop.
        std::expected<void, LoggerError> run() noexcept
        {
            if (running_.exchange(true))
            {
                return std::unexpected(LoggerError::AlreadyRunning);
            }

            internal::Backoff<BUSY_THRESHOLD, RELAX_THRESHOLD> backoff {};
            while (true)
            {
                LogEntry top;
                if (!queue_.try_dequeue(top))
                {
                    backoff.pause();
                    continue;
                }

                backoff.reset();
                if (std::holds_alternative<StopSignal>(top.entry))
                {
                    break;
                }

                if (Message* msg = std::get_if<Message>(&top.entry))
                {
                    output_ << *msg;
                }
            }
            return {};
        }

        /// Stop the logging loop. Keep in mind that all logging requests after this call is made
        /// will either fail or simply be ignored.
        std::expected<void, LoggerError> stop() noexcept
        {
            if (!running_.exchange(false))
            {
                return std::unexpected(LoggerError::NotRunning);
            }
            enqueueUnchecked(LogEntry {.entry = StopSignal {}});
            return {};
        }

        /// Tries to log a specific message and returns true if we have capacity.
        /// Delivery is best-effort for performance: if you call stop() then call this,
        /// there is a possibility this function may succeed without producing any logs.
        bool tryEnqueueUnchecked(Message m) noexcept
        {
            return queue_.try_enqueue(LogEntry {.entry = m});
        }

      private:
        void enqueueUnchecked(LogEntry m) noexcept
        {
            internal::Backoff<BUSY_THRESHOLD, RELAX_THRESHOLD> backoff {};
            while (true)
            {
                if (queue_.try_enqueue(m))
                {
                    return;
                }
                backoff.pause();
            }
        }

        moodycamel::ReaderWriterQueue<LogEntry> queue_;
        O output_;

        std::atomic<bool> running_ {false};
    };

    template<typename F>
    concept FailureHandler = requires(F f, Message msg) {
        { f(msg) } -> std::same_as<void>;
    };

    template<FailureHandler OnFailure>
    struct FileOutput
    {
        std::filesystem::path path;
        OnFailure onFailure;
        std::ofstream stream;

        // Must be noexcept to satisfy the OutputSink concept
        FileOutput& operator<<(Message msg) noexcept
        {
            if (!stream.is_open())
            {
                onFailure(msg);
                return *this;
            }

            // TODO: actually write logged messages
            stream << "TODO" << "\n";

            if (stream.fail())
            {
                onFailure(msg);
                stream.clear();
            }
            return *this;
        }
    };

    static_assert(OutputSink<FileOutput<void (*)(Message)>>);
}  // namespace alpdaq::logging