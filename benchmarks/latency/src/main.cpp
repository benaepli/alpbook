import alpbook_latency.strategy;
import alpbook.itch;
import alpbook_latency.sink;
import alpbook.dispatch;

#include <atomic>  // Added for atomic flag
#include <chrono>
#include <csignal>  // Added for signal handling
#include <iostream>
#include <string>
#include <thread>

#include <immintrin.h>

import alpbook_latency.strategy;
import alpbook.itch;
import alpbook.internal.backoff;
import alpbook_latency.sink;
import alpbook.dispatch;
import alpbook.itch.reader;

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

namespace alpbook_latency
{
    template<typename D>
    struct DispatchHandler
    {
        alpbook_latency::BenchmarkData* data;
        D& dispatcher;

        // Configuration: How many nanoseconds to wait between messages.
        static constexpr int64_t TargetWaitNs = 1000;

        int64_t cyclesPerWait = 0;
        alpbook::internal::Backoff<0, 100000> backoff;

        template<bool B>
        void handle(alpbook::itch::ItchSlot<B>& slot)
        {
            if (!Running) [[unlikely]]
            {
                throw Interrupted();
            }

            if (cyclesPerWait == 0) [[unlikely]]
            {
                int64_t startTsc = data->clock.rdtsc();
                int64_t startNs = data->clock.tsc2ns(startTsc);

                while (data->clock.tsc2ns(data->clock.rdtsc()) - startNs < TargetWaitNs)
                {
                    _mm_pause();
                }
                cyclesPerWait = data->clock.rdtsc() - startTsc;
            }

            int64_t loopStart = data->clock.rdtsc();
            while ((data->clock.rdtsc() - loopStart) < cyclesPerWait)
            {
                backoff.pause();
            }
            backoff.reset();

            if constexpr (B)
            {
                slot.dispatchTimestamp = data->clock.rdtsc();
            }
            dispatcher.dispatch(slot);
        }
    };
}  // namespace alpbook_latency

// Usage: ./benchmark <itch_file.gz> <output_latencies.csv>
int main(int argc, char** argv)
{
    std::signal(SIGINT, signal_handler);

    if (argc < 3)
    {
        std::cerr << "Usage: " << argv[0] << " <input_itch.gz> <output.csv>\n";
        return 1;
    }

    std::string inputFile = argv[1];
    std::string outputFile = argv[2];

    alpbook_latency::BenchmarkData sharedData {};
    alpbook_latency::BenchmarkStrategyFactory factory(sharedData);
    alpbook_latency::BenchmarkSinkFactory sinkFactory(factory);
    alpbook::itch::ArrayMapper<> mapper {};
    for (uint16_t i = 1; i <= 100; i++)
    {
        mapper.assign(i);
    }

    using BenchSlot = alpbook::itch::ItchSlot<true>;

    try
    {
        alpbook::Dispatcher<BenchSlot,
                            alpbook_latency::BenchmarkSinkFactory,
                            alpbook::itch::ItchExtractor,
                            decltype(mapper)>
            dispatcher {mapper, sinkFactory};

        auto initResult = dispatcher.init(4);
        if (!initResult.has_value())
        {
            std::cerr << "Failed to init dispatcher (pinning error?)\n";
            return 1;
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