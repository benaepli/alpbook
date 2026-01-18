module;

import alpbook.internal;

#include <cmath>
#include <concepts>
#include <expected>
#include <thread>

#include <readerwriterqueue.h>

export module alpbook.itch.dispatch;

namespace alpbook::itch
{
    constexpr auto MESSAGE_SLOT_SIZE = 64;

    struct alignas(std::hardware_destructive_interference_size) MsgSlot
    {
        std::array<uint8_t, MESSAGE_SLOT_SIZE> data;
    };

    template<typename T>
    concept PacketSink = std::default_initializable<T> && requires(T worker, MsgSlot data) {
        { worker.onMessage(data) } -> std::same_as<void>;

        // The parameter is the index (from 0 to the number of threads created).
        { worker.onThreadStart(uint32_t {}) } -> std::same_as<void>;
    };

    template<typename T>
    concept IDExtractor = requires(MsgSlot const& slot) {
        { T::extractID(slot) } -> std::convertible_to<uint16_t>;
    };

    enum class DispatchError : uint8_t
    {
        PinError,
    };

    template<PacketSink S, IDExtractor E>
    class Dispatcher
    {
      public:
        Dispatcher() = default;

        Dispatcher(Dispatcher const&) = delete;
        Dispatcher& operator=(Dispatcher const&) = delete;
        Dispatcher(Dispatcher&&) = delete;
        Dispatcher& operator=(Dispatcher&&) = delete;

        ~Dispatcher() = default;

        /// Initializes a dispatcher. maxWorkers being zero is equivalent to no limit.
        std::expected<void, DispatchError> init(uint32_t maxWorkers)
        {
            auto pinner = internal::Pinner::create();
            if (!pinner.has_value())
            {
                return std::unexpected(DispatchError::PinError);
            }
            pinner_ = std::move(*pinner);

            auto coresResult = pinner_->getCoreCount();
            if (!coresResult.has_value())
            {
                return std::unexpected(DispatchError::PinError);
            }

            uint32_t cores = *coresResult;
            if (maxWorkers != 0)
            {
                cores = std::min(cores, maxWorkers);
            }

            cores = std::bit_floor(cores);
            workerMask_ = cores - 1;

            workers_.reserve(cores);
            for (uint32_t i = 0; i < cores; i++)
            {
                workers_.push_back(Worker {
                    .core = i,
                });
                workers_[i].thread =
                    std::jthread([this, i](std::stop_token st) { workerLoop(st, i); });
            }
            return {};
        }

        /// Extracts the ID and routes the message to the correct worker queue.
        void dispatch(MsgSlot const& slot)
        {
            uint16_t id = E::extractID(slot);
            uint32_t threadIdx = id % workerMask_;
            workers_[threadIdx].queue.enqueue(slot);
        }

      private:
        void workerLoop(std::stop_token st, uint32_t core)
        {
            auto& queue = workers_[core].queue;
            pinner_->pinToCore(core);
            S sink;
            sink.onThreadStart(core);

            constexpr uint32_t busyThreshold = 2000;
            constexpr uint32_t relaxThreshold = 50000;

            internal::Backoff<busyThreshold, relaxThreshold> backoff;

            while (!st.stop_requested())
            {
                MsgSlot slot;
                if (queue.try_dequeue(slot))
                {
                    sink.onMessage(slot);
                    backoff.reset();
                }
                else
                {
                    backoff.pause();
                }
            }
        }

        std::unique_ptr<internal::Pinner> pinner_ {};

        struct alignas(std::hardware_destructive_interference_size) Worker
        {
            uint32_t core {};

            alignas(std::hardware_destructive_interference_size)
                moodycamel::ReaderWriterQueue<MsgSlot> queue;
            std::jthread thread;
        };
        uint32_t workerMask_ {};
        std::vector<Worker> workers_ {};
    };

}  // namespace alpbook::itch