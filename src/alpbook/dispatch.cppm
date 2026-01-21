module;

#include <cmath>
#include <concepts>
#include <expected>
#include <limits>
#include <new>
#include <optional>
#include <print>
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

    export struct SynchronousDispatch
    {
    };

    /// Dispatches messages to workers based on templated policies.
    /// Thread counts are guaranteed to be a power of 2.
    export template<typename Slot, typename F, typename E, typename M, typename Policy = void>
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
            if (workers_ptr_)
            {
                for (uint32_t i = 0; i < workerCount_; ++i)
                {
                    workers_ptr_[i].running.store(false, std::memory_order_release);
                }
                for (uint32_t i = 0; i < workerCount_; ++i)
                {
                    if (workers_ptr_[i].thread.joinable())
                    {
                        workers_ptr_[i].thread.join();
                    }
                }
                for (uint32_t i = 0; i < workerCount_; ++i)
                {
                    workers_ptr_[i].~Worker();
                }
                ::operator delete[](workers_ptr_, std::align_val_t {alignof(Worker)});
            }
        }

        /// Initializes a dispatcher. maxWorkers being zero is equivalent to no limit.
        std::expected<uint32_t, DispatchError> init(uint32_t maxWorkers)
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

            workerCount_ = cores;
            workers_ptr_ = static_cast<Worker*>(::operator new[](
                sizeof(Worker) * workerCount_, std::align_val_t {alignof(Worker)}));

            for (uint32_t i = 0; i < workerCount_; ++i)
            {
                new (&workers_ptr_[i]) Worker();
                workers_ptr_[i].core = i;
                workers_ptr_[i].running.store(true);
            }
            for (uint32_t i = 0; i < workerCount_; ++i)
            {
                workers_ptr_[i].thread = std::thread([this, i, fact = factory_, mapper = mapper_]
                                                     { workerLoop(i, fact, *mapper); });
            }
            return cores;
        }

        /// Extracts the ID and routes the message to the correct worker queue.
        void dispatch(Slot const& slot)
        {
            uint16_t id = E::extractID(slot);
            uint32_t threadIdx = mapper_->getWorkerIndex(id);
            if (threadIdx != DROP_MSG)
            {
                workers_ptr_[threadIdx].queue.enqueue(slot);
            }
        }

      private:
        void workerLoop(uint32_t core, F factory, M const& mapper)
        {
            auto& queue = workers_ptr_[core].queue;
            auto& running = workers_ptr_[core].running;
            pinner_->pinToCore(core);
            auto sink = factory.create(core, mapper);

            constexpr uint32_t busyThreshold = 10000;
            constexpr uint32_t relaxThreshold = std::numeric_limits<uint32_t>::max();

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
                moodycamel::ReaderWriterQueue<Slot> queue {16384};
            std::thread thread;
        };
        Worker* workers_ptr_ = nullptr;
        uint32_t workerCount_ = 0;
    };

    /// Specialization for single-threaded, immediate dispatch.
    export template<typename Slot, typename F, typename E, typename M>
        requires DispatchSlot<Slot> && SinkFactory<F, Slot, M> && IDExtractor<Slot, E>
        && IDMapper<M>
    class Dispatcher<Slot, F, E, M, SynchronousDispatch>
    {
        using Sink = F::SinkType;
        static constexpr uint32_t DROP_MSG = std::numeric_limits<uint32_t>::max();

      public:
        explicit Dispatcher(M& mapper, F factory)
            : mapper_(&mapper)
            , factory_(std::move(factory))
        {
        }

        // No-op or update internal pointer
        void setMapper(M& mapper) { mapper_ = &mapper; }

        Dispatcher(Dispatcher const&) = delete;
        Dispatcher& operator=(Dispatcher const&) = delete;
        Dispatcher(Dispatcher&&) = default;
        Dispatcher& operator=(Dispatcher&&) = default;
        ~Dispatcher() = default;

        /// Initializes the single sink.
        /// Ignores maxWorkers (treats it as 1) and Pinning logic. Returns 0.
        std::expected<uint32_t, DispatchError> init(uint32_t /*maxWorkers*/)
        {
            // Treat as single core (index 0)
            mapper_->setThreadCount(1);

            // Create the sink immediately for the current thread (core 0)
            sink_ = factory_.create(0, *mapper_);
            return 0;
        }

        /// Immediately processes the message on the calling thread.
        void dispatch(Slot const& slot)
        {
            uint16_t id = E::extractID(slot);

            // We still check the mapper to ensure the ID is valid for this "worker"
            if (mapper_->getWorkerIndex(id) != DROP_MSG)
            {
                // Direct function call instead of enqueueing
                sink_->onMessage(slot);
            }
        }

      private:
        M* mapper_ = nullptr;
        F factory_;
        // We hold the sink directly instead of a list of Workers
        std::optional<Sink> sink_;
    };

}  // namespace alpbook
