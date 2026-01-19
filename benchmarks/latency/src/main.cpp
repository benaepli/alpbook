import alpbook_latency.strategy;
import alpbook.itch;
import alpbook_latency.sink;
import alpbook.dispatch;

#include <atomic>  // Added for atomic flag
#include <chrono>
#include <csignal>  // Added for signal handling
#include <cstdlib>  // Added for std::atoi
#include <iostream>
#include <string>
#include <thread>
#include <variant>  // Added for std::monostate

#include <immintrin.h>

import alpbook_latency.strategy;
import alpbook.itch;
import alpbook.internal.backoff;
import alpbook_latency.sink;
import alpbook.dispatch;
import alpbook.itch.reader;
import alpbook.internal.pin;

// Global flag to control execution state
std::atomic<bool> Running {true};

// Signal handler to capture Ctrl+C
void signal_handler(int)
{
    Running = false;
}

// Custom exception to break the reading loop cleanly
struct Interrupted : public std::exception
{
};

// Parse thread count from command-line arguments
int parseThreadCount(int argc, char** argv)
{
    int threadCount = 1;  // default
    for (int i = 3; i < argc; i++)
    {
        if (std::string(argv[i]) == "--threads" && i + 1 < argc)
        {
            threadCount = std::atoi(argv[i + 1]);
            break;
        }
    }
    return std::max(0, threadCount);
}

// Type trait to detect synchronous dispatcher
template<typename T>
struct IsSynchronous : std::false_type
{
};

template<typename Slot, typename F, typename E, typename M>
struct IsSynchronous<alpbook::Dispatcher<Slot, F, E, M, alpbook::SynchronousDispatch>>
    : std::true_type
{
};

template<typename T>
inline constexpr bool IsSynchronous_v = IsSynchronous<T>::value;

namespace alpbook_latency
{
    template<typename D>
    struct DispatchHandler
    {
        static constexpr bool IsSynchronous = IsSynchronous_v<D>;

        alpbook_latency::BenchmarkData* data;
        D& dispatcher;

        static constexpr int64_t TargetWaitNs = 1000;

        // Only use these members in async mode
        int64_t cyclesPerWait = 0;
        [[no_unique_address]] std::conditional_t<IsSynchronous,
                                                 std::monostate,
                                                 alpbook::internal::Backoff<0, 100000>> backoff;

        template<bool B>
        void handle(alpbook::itch::ItchSlot<B>& slot)
        {
            if (!Running) [[unlikely]]
            {
                throw Interrupted();
            }

            // Only wait in async mode
            if constexpr (!IsSynchronous)
            {
                if (cyclesPerWait == 0) [[unlikely]]
                {
                    // Calibration logic
                    int64_t startTsc = data->clock.rdtsc();
                    int64_t startNs = data->clock.tsc2ns(startTsc);

                    while (data->clock.tsc2ns(data->clock.rdtsc()) - startNs < TargetWaitNs)
                    {
                        _mm_pause();
                    }
                    cyclesPerWait = data->clock.rdtsc() - startTsc;
                }

                // Wait logic
                int64_t loopStart = data->clock.rdtsc();
                while ((data->clock.rdtsc() - loopStart) < cyclesPerWait)
                {
                    backoff.pause();
                }
                backoff.reset();
            }

            // Record timestamp and dispatch (same for both modes)
            if constexpr (B)
            {
                slot.dispatchTimestamp = data->clock.rdtsc();
            }
            dispatcher.dispatch(slot);
        }
    };
}  // namespace alpbook_latency

// Template function to run benchmark with either dispatcher type
template<typename DispatcherType>
int runBenchmark(std::string const& inputFile,
                 std::string const& outputFile,
                 alpbook_latency::BenchmarkData& sharedData,
                 alpbook_latency::BenchmarkSinkFactory& sinkFactory,
                 alpbook::itch::ArrayMapper<>& mapper,
                 uint32_t threadCount)
{
    try
    {
        DispatcherType dispatcher {mapper, sinkFactory};

        auto initResult = dispatcher.init(threadCount);
        if (!initResult.has_value())
        {
            std::cerr << "Failed to init dispatcher (pinning error?)\n";
            return 1;
        }
        uint32_t dispatcherCores = *initResult;

        // Pin main thread to a core that doesn't conflict with worker threads.
        // Use modulo to wrap around if needed (e.g., 4 % 8 = 4).
        auto mainPinner = alpbook::internal::Pinner::create();
        if (!mainPinner.has_value())
        {
            std::cerr << "Warning: Failed to initialize thread pinning for main thread.\n";
            std::cerr << "         Benchmark results may be unreliable.\n";
        }
        else
        {
            auto totalCores = (*mainPinner)->getCoreCount();
            if (!totalCores.has_value() || *totalCores == 0)
            {
                std::cerr << "Warning: Failed to query system core count.\n";
            }
            else
            {
                uint32_t mainCore = dispatcherCores % (*totalCores);
                auto pinResult = (*mainPinner)->pinToCore(mainCore);
                if (!pinResult.has_value())
                {
                    std::cerr << "Warning: Failed to pin main thread to core " << mainCore << ".\n";
                    std::cerr << "         Thread may migrate during execution.\n";
                }
                else
                {
                    std::cout << "Main thread pinned to core " << mainCore
                              << " (dispatcher uses cores 0-" << (dispatcherCores - 1) << ").\n";
                }
            }
        }

        std::cout << "Dispatcher initialized. Starting playback... (Ctrl+C to stop early)\n";
        alpbook_latency::DispatchHandler handler {&sharedData, dispatcher};
        auto result = alpbook::itch::readGzippedItch<decltype(handler), true>(inputFile, handler);

        if (!result.has_value())
        {
            std::cerr << "Error reading ITCH file: " << result.error() << "\n";
            return 1;
        }
        std::cout << "Playback complete.\n";
    }
    catch (Interrupted const&)
    {
        std::cerr << "\nInterrupted by user. Cleaning up...\n";
    }
    catch (std::exception const& e)
    {
        std::cerr << "\nUnexpected error: " << e.what() << "\n";
        return 1;
    }

    std::cout << "Waiting for queues to drain and threads to join...\n";
    std::this_thread::sleep_for(std::chrono::seconds(1));

    std::cout << "Saving results to " << outputFile << "...\n";
    sharedData.saveToCSV(outputFile);

    return 0;
}

int main(int argc, char** argv)
{
    std::signal(SIGINT, signal_handler);

    if (argc < 3)
    {
        std::cerr << "Usage: " << argv[0] << " <input_itch.gz> <output.csv> [--threads N]\n";
        std::cerr << "  --threads 0 or 1: Synchronous mode (no waiting)\n";
        std::cerr << "  --threads N > 1:  Async mode with N worker threads (1000ns wait)\n";
        std::cerr << "  Default: --threads 4\n";
        return 1;
    }

    std::string inputFile = argv[1];
    std::string outputFile = argv[2];
    int threadCount = parseThreadCount(argc, argv);

    // Setup shared objects
    alpbook_latency::BenchmarkData sharedData {};
    alpbook_latency::BenchmarkStrategyFactory factory(sharedData);
    alpbook_latency::BenchmarkSinkFactory sinkFactory(factory);
    alpbook::itch::ArrayMapper<> mapper {};
    for (uint16_t i = 1; i <= 100; i++)
    {
        mapper.assign(i);
    }

    using BenchSlot = alpbook::itch::ItchSlot<true>;
    using AsyncDispatcher = alpbook::Dispatcher<BenchSlot,
                                                alpbook_latency::BenchmarkSinkFactory,
                                                alpbook::itch::ItchExtractor,
                                                decltype(mapper)>;
    using SyncDispatcher = alpbook::Dispatcher<BenchSlot,
                                               alpbook_latency::BenchmarkSinkFactory,
                                               alpbook::itch::ItchExtractor,
                                               decltype(mapper),
                                               alpbook::SynchronousDispatch>;

    // Branch based on mode
    if (threadCount <= 1)
    {
        std::cout << "Running in synchronous mode (no waiting, single-threaded)\n";
        return runBenchmark<SyncDispatcher>(
            inputFile, outputFile, sharedData, sinkFactory, mapper, 1);
    }
    else
    {
        std::cout << "Running in async mode (" << threadCount
                  << " threads, 1000ns message spacing)\n";
        return runBenchmark<AsyncDispatcher>(
            inputFile, outputFile, sharedData, sinkFactory, mapper, threadCount);
    }
}