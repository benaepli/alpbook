module;

#include <expected>
#include <utility>
#include <variant>

#include "readerwriterqueue.h"

export module alpdaq.logging.logger;

import alpdaq.logging.messages;
import alpbook.internal.backoff;

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

    export template<typename T, typename Data>
    concept OutputSink = requires(T& t, Message<Data> msg) {
        { t << msg } -> std::same_as<T&>;
        noexcept(t << msg);

        /// Rotate serves a synchronization point. It indicates that flush() has been called
        /// and all messages prior to that flush() have been processed.
        { t.rotate() } -> std::same_as<void>;
        noexcept(t.rotate());
    };

    struct StopSignal
    {
    };

    struct FlushSignal
    {
        std::atomic<bool>* isFlushed;
    };

    template<typename Data>
    struct alignas(std::hardware_destructive_interference_size) LogEntry
    {
        std::variant<StopSignal, FlushSignal, Message<Data>> entry;
    };

    export template<typename O, typename Data>
        requires OutputSink<O, Data>
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
            bool draining = false;

            while (true)
            {
                LogEntry<Data> top;
                if (!queue_.try_dequeue(top))
                {
                    if (draining)
                    {
                        break;
                    }
                    backoff.pause();
                    continue;
                }

                backoff.reset();
                if (std::holds_alternative<StopSignal>(top.entry))
                {
                    draining = true;
                    continue;
                }

                if (FlushSignal* flush = std::get_if<FlushSignal>(&top.entry))
                {
                    output_.rotate();
                    flush->isFlushed->store(true, std::memory_order_release);
                    continue;
                }

                if (Message<Data>* msg = std::get_if<Message<Data>>(&top.entry))
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
            enqueueUnchecked(LogEntry<Data> {.entry = StopSignal {}});
            return {};
        }

        /// Tries to log a specific message and returns true if we have capacity.
        /// Delivery is best-effort for performance: if you call stop() then call this,
        /// there is a possibility this function may succeed without producing any logs.
        bool tryEnqueueUnchecked(Message<Data> m) noexcept
        {
            return queue_.try_enqueue(LogEntry<Data> {.entry = m});
        }

        void flushSession() noexcept
        {
            if (!running_.load(std::memory_order_acquire))
            {
                return;
            };

            std::atomic isFlushed {false};
            enqueueUnchecked(LogEntry<Data> {.entry = FlushSignal {&isFlushed}});

            internal::Backoff<BUSY_THRESHOLD, RELAX_THRESHOLD> backoff {};

            while (!isFlushed.load(std::memory_order_acquire))
            {
                backoff.pause();
            }
        }

      private:
        void enqueueUnchecked(LogEntry<Data> m) noexcept
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

        moodycamel::ReaderWriterQueue<LogEntry<Data>> queue_;
        O output_;

        std::atomic<bool> running_ {false};
    };
}  // namespace alpdaq::logging