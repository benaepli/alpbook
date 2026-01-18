module;

#include <cmath>
#include <concepts>
#include <expected>
#include <thread>

#include <readerwriterqueue.h>

import alpbook.internal;

export module alpbook.dispatch;

namespace alpbook
{
    export template<size_t SlotSize>
    struct alignas(std::hardware_destructive_interference_size) MsgSlot
    {
        std::array<uint8_t, SlotSize> data;
    };

    export template<size_t SlotSize, typename T>
    concept PacketSink = requires(T worker, MsgSlot<SlotSize> data) {
        { worker.onMessage(data) } -> std::same_as<void>;
    };

    /// A factory for PacketSink. It must be thread-safe.
    export template<typename F, size_t SlotSize>
    concept SinkFactory = std::copy_constructible<F> && requires(F factory, uint32_t coreIndex) {
        typename F::SinkType;
        { factory.create(coreIndex) } -> std::same_as<typename F::SinkType>;
        requires PacketSink<SlotSize, typename F::SinkType>;
    };

    export template<size_t SlotSize, typename T>
    concept IDExtractor = requires(MsgSlot<SlotSize> const& slot) {
        { T::extractID(slot) } -> std::convertible_to<uint16_t>;
    };

    export template<typename T, typename IDType = uint16_t>
    concept IDMapper = requires(T mapper, uint32_t threadCount, IDType id) {
        { mapper.setThreadCount(threadCount) } -> std::same_as<void>;
        { mapper.getWorkerIndex(id) } -> std::convertible_to<uint32_t>;
    };

    export enum class DispatchError : uint8_t
    {
        PinError,
    };

    /// Dispatches messages to workers based on templated policies.
    /// Thread counts are guaranteed to be a power of 2.
    export template<size_t SlotSize, typename F, typename E, typename M>
        requires SinkFactory<F, SlotSize> && IDExtractor<SlotSize, E> && IDMapper<M>
    class Dispatcher
    {
        using Sink = F::SinkType;

        static constexpr uint32_t DROP_MSG = std::numeric_limits<uint32_t>::max();

      public:
        explicit Dispatcher(M& mapper, F factory)
            : mapper_(&mapper)
            , factory_(std::move(factory))
        {
        }

        void setMapper(M& mapper) { mapper_ = &mapper; }

        Dispatcher(Dispatcher const&) = delete;
        Dispatcher& operator=(Dispatcher const&) = delete;
        Dispatcher(Dispatcher&&) = delete;
        Dispatcher& operator=(Dispatcher&&) = delete;

        ~Dispatcher()
        {
            for (auto& worker : workers_)
            {
                worker.running.store(false, std::memory_order_release);
            }
            for (auto& worker : workers_)
            {
                if (worker.thread.joinable())
                {
                    worker.thread.join();
                }
            }
        }

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

            workers_.reserve(cores);
            for (uint32_t i = 0; i < cores; i++)
            {
                workers_.push_back(Worker {
                    .core = i,
                    .running = true,
                });
                workers_[i].thread =
                    std::thread([this, i, fact = factory_] { workerLoop(i, fact); });
            }
            mapper_->setThreadCount(cores);
            return {};
        }

        /// Extracts the ID and routes the message to the correct worker queue.
        void dispatch(MsgSlot<SlotSize> const& slot)
        {
            uint16_t id = E::extractID(slot);
            uint32_t threadIdx = mapper_->getWorkerIndex(id);
            if (threadIdx != DROP_MSG)
            {
                workers_[threadIdx].queue.enqueue(slot);
            }
        }

      private:
        void workerLoop(uint32_t core, F factory)
        {
            auto& queue = workers_[core].queue;
            auto& running = workers_[core].running;
            pinner_->pinToCore(core);
            auto sink = factory.create(core);

            constexpr uint32_t busyThreshold = 2000;
            constexpr uint32_t relaxThreshold = 50000;

            internal::Backoff<busyThreshold, relaxThreshold> backoff;

            while (running.load(std::memory_order_relaxed))
            {
                MsgSlot<SlotSize> slot;
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

        M* mapper_ = nullptr;
        F factory_;
        std::unique_ptr<internal::Pinner> pinner_ {};

        struct alignas(std::hardware_destructive_interference_size) Worker
        {
            uint32_t core {};
            std::atomic<bool> running {false};

            alignas(std::hardware_destructive_interference_size)
                moodycamel::ReaderWriterQueue<MsgSlot<SlotSize>> queue;
            std::thread thread;
        };
        std::vector<Worker> workers_ {};
    };

}  // namespace alpbook
