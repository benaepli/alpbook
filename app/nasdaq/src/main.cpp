#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <variant>

#include <toml++/toml.hpp>

import alpdaq;
import alpdaq.network.af_xdp;
import alpdaq.system.container.strategized;
import alpbook.book;
import nasdaq.strategy;

namespace
{
    namespace logging = alpdaq::logging;
    namespace network = alpdaq::network;
    namespace config = alpdaq::config;

    using FileLogger = logging::FileLogger<logging::LogFailureHandler>;
    using LoggerCore =
        logging::Logger<logging::FileOutput<logging::LogFailureHandler>, logging::LogData>;
    using AfXdpSource = network::AfXdpSource<FileLogger>;
    using Source = alpdaq::MovableSource<AfXdpSource>;
    using Container = alpdaq::system::container::
        Strategized<alpbook::nasdaq::PolicyHash, app::EmptyStrategy, app::EmptyStrategyFactory>;
    using AppSystem = alpdaq::System<Source, FileLogger, Container>;

    constexpr size_t LOG_QUEUE_SIZE = 1 << 16;

    std::atomic<AppSystem*> g_system {nullptr};

    void signalHandler(int)
    {
        if (auto* sys = g_system.load(std::memory_order_relaxed))
        {
            sys->stop();
        }
    }

    void onLogFailure(logging::Message<logging::LogData>)
    {
        std::fputs("alpdaq: log write failed\n", stderr);
    }

    /// Resolves a pin policy to a target PU index, or std::nullopt when pinning is disabled
    /// or no PU is available.
    std::optional<uint32_t> resolvePin(config::PinPolicy const& policy,
                                       alpdaq::placement::Pinner const& pinner)
    {
        if (auto const* exact = std::get_if<config::PinExact>(&policy))
        {
            return exact->core;
        }
        if (std::holds_alternative<config::PinAuto>(policy))
        {
            auto topology = pinner.getTopology();
            if (topology.empty() || topology[0].pus.empty())
            {
                return std::nullopt;
            }
            return topology[0].pus[0].osIndex;
        }
        return std::nullopt;
    }

    std::string_view openErrorString(network::OpenError error)
    {
        switch (error)
        {
            case network::OpenError::InvalidConfig:
                return "invalid AF_XDP configuration";
            case network::OpenError::InterfaceNotFound:
                return "interface not found";
            case network::OpenError::InsufficientPrivileges:
                return "insufficient privileges (AF_XDP requires CAP_NET_RAW/root)";
            case network::OpenError::UmemFailed:
                return "UMEM allocation failed";
            case network::OpenError::XdpSocketFailed:
                return "XDP socket setup failed";
            case network::OpenError::MulticastJoinFailed:
                return "multicast join failed";
            case network::OpenError::UringInitFailed:
                return "io_uring init failed";
            case network::OpenError::SocketSetupFailed:
                return "recovery socket setup failed";
        }
        return "unknown error";
    }
}  // namespace

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        std::cerr << "Usage: alpdaq <config.toml>\n";
        return 1;
    }

    toml::table tbl;
    try
    {
        tbl = toml::parse_file(argv[1]);
    }
    catch (toml::parse_error const& e)
    {
        std::cerr << "Config parse error: " << e << "\n";
        return 1;
    }

    auto parsed = config::parse(tbl);
    if (!parsed)
    {
        std::cerr << "Config error: " << parsed.error() << "\n";
        return 1;
    }
    auto cfg = std::move(*parsed);

    if (!std::holds_alternative<config::NetworkSource>(cfg.dataSource))
    {
        std::cerr << "Config error: the alpdaq binary requires data.source = \"network\"\n";
        return 1;
    }
    auto afXdpConfig = std::get<config::NetworkSource>(cfg.dataSource).config;
    auto const interface = afXdpConfig.liveConfig.interface;
    auto const queueId = afXdpConfig.liveConfig.queueId;

    auto pinnerResult = alpdaq::placement::Pinner::create();
    if (!pinnerResult)
    {
        std::cerr << "Failed to initialize CPU pinner\n";
        return 1;
    }
    auto pinner = std::move(*pinnerResult);

    std::error_code ec;
    std::filesystem::create_directories(cfg.logDir, ec);
    auto logPath = cfg.logDir / "alpdaq.log";

    auto core = std::make_shared<LoggerCore>(LOG_QUEUE_SIZE,
                                             logging::FileOutput {
                                                 .path = logPath,
                                                 .onFailure = &onLogFailure,
                                                 .stream = std::ofstream(logPath),
                                             });
    auto logger = std::make_shared<FileLogger>(core);

    auto logPin = resolvePin(cfg.logThread, *pinner);
    std::thread logThread(
        [core, logPin, &pinner]
        {
            if (logPin)
            {
                pinner->pinToPU(*logPin);
            }
            core->run();
        });

    auto stopLogger = [&]
    {
        core->stop();
        logThread.join();
    };

    auto sourceResult = AfXdpSource::open(std::move(afXdpConfig), logger);
    if (!sourceResult)
    {
        std::cerr << "Failed to open AF_XDP source: " << openErrorString(sourceResult.error())
                  << "\n";
        stopLogger();
        return 1;
    }
    Source source(std::move(*sourceResult));

    Container container(app::EmptyStrategyFactory {});

    alpdaq::SystemConfig systemConfig {
        .logger = logger,
        .stocks = std::move(cfg.stocks),
    };

    AppSystem system(std::move(source), std::move(systemConfig), std::move(container));
    g_system.store(&system, std::memory_order_relaxed);
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    if (auto mainPin = resolvePin(cfg.mainThread, *pinner))
    {
        pinner->pinToPU(*mainPin);
    }

    std::cout << "alpdaq starting (interface=" << interface << ", queue=" << queueId << ")...\n";

    auto result = system.run();

    g_system.store(nullptr, std::memory_order_relaxed);
    stopLogger();

    if (!result)
    {
        std::cerr << "alpdaq terminated with a fatal error\n";
        return 1;
    }
    return 0;
}
