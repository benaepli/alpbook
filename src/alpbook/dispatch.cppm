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
    export template<typename T>
    concept DispatchSlot = std::is_trivially_copyable_v<T>
        && (sizeof(T) % std::hardware_destructive_interference_size == 0)
        && (alignof(T) == std::hardware_destructive_interference_size);

    export template<typename Slot, typename T>
    concept PacketSink = DispatchSlot<Slot> && requires(T worker, Slot data) {
        { worker.onMessage(data) } -> std::same_as<void>;
    };

    export template<typename Slot, typename T>
    concept IDExtractor = DispatchSlot<Slot> && requires(Slot const& slot) {
        { T::extractID(slot) } -> std::convertible_to<uint16_t>;
    };

    export template<typename T, typename IdType = uint16_t>
    concept IDMapper = requires(T mapper, uint32_t threadCount, IdType id) {
        { mapper.setThreadCount(threadCount) } -> std::same_as<void>;
        { mapper.getWorkerIndex(id) } -> std::convertible_to<uint32_t>;
    };

    /// A factory for PacketSink. It must be thread-safe.
    export template<typename F, typename Slot, typename Mapper>
    concept SinkFactory = DispatchSlot<Slot> && std::copy_constructible<F>
        && requires(F factory, uint32_t core, Mapper const& m) {
               typename F::SinkType;
               { factory.create(core, m) } -> std::same_as<typename F::SinkType>;
               requires PacketSink<Slot, typename F::SinkType>;
           };

    export template<typename T, typename IdType = uint16_t>
    concept EnumerableMapper = IDMapper<T, IdType> && requires(T mapper, uint32_t core) {
        { mapper.getIDsForThread(core) } -> std::same_as<std::vector<IdType>>;
    };

    export enum class DispatchError : uint8_t
    {
        PinError,
    };

    /// Dispatches messages to workers based on templated policies.
    /// Thread counts are guaranteed to be a power of 2.
    export template<typename Slot, typename F, typename E, typename M>
        requires DispatchSlot<Slot> && SinkFactory<F, Slot, M> && IDExtractor<Slot, E>
        && IDMapper<M>
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
                worker->running.store(false, std::memory_order_release);
            }
            for (auto& worker : workers_)
            {
                if (worker->thread.joinable())
                {
                    worker->thread.join();
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
            if (cores == 0)
            {
                cores = 1;
            }

            mapper_->setThreadCount(cores);

            workers_.reserve(cores);
            for (uint32_t i = 0; i < cores; i++)
            {
                auto worker = std::make_unique<Worker>();
                worker->core = i;
                worker->running.store(true);
                workers_.push_back(std::move(worker));
            }
            for (uint32_t i = 0; i < cores; i++)
            {
                workers_[i]->thread = std::thread([this, i, fact = factory_, mapper = mapper_]
                                                  { workerLoop(i, fact, *mapper); });
            }
            return {};
        }

        /// Extracts the ID and routes the message to the correct worker queue.
        void dispatch(Slot const& slot)
        {
            uint16_t id = E::extractID(slot);
            uint32_t threadIdx = mapper_->getWorkerIndex(id);
            if (threadIdx != DROP_MSG)
            {
                workers_[threadIdx]->queue.enqueue(slot);
            }
        }

      private:
        void workerLoop(uint32_t core, F factory, M const& mapper)
        {
            auto& queue = workers_[core]->queue;
            auto& running = workers_[core]->running;
            pinner_->pinToCore(core);
            auto sink = factory.create(core, mapper);

            constexpr uint32_t busyThreshold = 2000;
            constexpr uint32_t relaxThreshold = 50000;

            internal::Backoff<busyThreshold, relaxThreshold> backoff;

            while (running.load(std::memory_order_relaxed))
            {
                Slot slot;
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
                moodycamel::ReaderWriterQueue<Slot> queue;
            std::thread thread;
        };
        std::vector<std::unique_ptr<Worker>> workers_ {};
    };

}  // namespace alpbook
