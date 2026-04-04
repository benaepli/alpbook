import benchmark.logger;
import benchmark.source;
import benchmark.strategy;
import alpdaq.system;
import alpdaq.system.state;
import alpdaq.system.container.strategized;
import alpdaq.simulated.binary;
import alpbook.itch.messages;
import alpbook.book;

#include <algorithm>
#include <array>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace
{
    using Container = alpdaq::system::container::Strategized<alpbook::nasdaq::PolicyHash,
                                                             benchmark::BenchmarkStrategy,
                                                             benchmark::BenchmarkStrategyFactory>;

    using BenchmarkSystem = alpdaq::System<benchmark::BenchmarkSource,
                                           benchmark::BenchmarkLogger,
                                           Container>;

    std::atomic<BenchmarkSystem*> g_system {nullptr};

    void signalHandler(int)
    {
        if (auto* sys = g_system.load(std::memory_order_relaxed))
        {
            sys->stop();
        }
    }

    alpbook::itch::StockTicker parseTicker(char const* str)
    {
        alpbook::itch::StockTicker ticker {};
        ticker.fill(' ');
        auto len = std::min(std::strlen(str), alpbook::itch::STOCK_TICKER_LEN);
        std::copy_n(str, len, ticker.begin());
        return ticker;
    }
}  // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "Usage: " << argv[0] << " <itch_file> <output.csv> [tickers...]\n";
        return 1;
    }

    std::filesystem::path inputFile = argv[1];
    std::string outputFile = argv[2];

    std::vector<alpbook::itch::StockTicker> stocks;
    for (int i = 3; i < argc; ++i)
    {
        stocks.push_back(parseTicker(argv[i]));
    }

    auto logger = std::make_shared<benchmark::BenchmarkLogger>();

    alpdaq::SessionId session {};
    auto sourceResult = alpdaq::simulated::BinaryItchSource::open(inputFile, session);
    if (!sourceResult)
    {
        std::cerr << "Failed to open ITCH file: " << inputFile << "\n";
        return 1;
    }

    benchmark::BenchmarkSource source(std::move(*sourceResult), logger);

    benchmark::BenchmarkStrategyFactory factory(logger);
    Container container(factory);

    alpdaq::SystemConfig<benchmark::BenchmarkLogger> config {
        .logger = logger,
        .stocks = std::move(stocks),
    };

    BenchmarkSystem system(std::move(source), std::move(config), std::move(container));
    g_system.store(&system, std::memory_order_relaxed);
    std::signal(SIGINT, signalHandler);

    std::cout << "Starting latency benchmark...\n";

    auto result = system.run();

    g_system.store(nullptr, std::memory_order_relaxed);

    if (!result)
    {
        std::cout << "Replay complete.\n";
    }

    std::cout << "Saving results to " << outputFile << "...\n";
    logger->saveToCSV(outputFile);

    return 0;
}
