module;

#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <toml++/toml.hpp>

export module alpdaq.config;

import alpbook.itch;
import alpdaq.network.af_xdp;

namespace alpdaq::config
{

    export struct PinDisabled
    {
    };

    export struct PinAuto
    {
    };

    export struct PinExact
    {
        uint32_t core;
    };

    export using PinPolicy = std::variant<PinDisabled, PinAuto, PinExact>;

    export struct NetworkSource
    {
        network::AfXdpConfig config;
    };

    export struct SimulatedSource
    {
        std::string inputFile;
    };

    export using DataSource = std::variant<NetworkSource, SimulatedSource>;

    export struct Config
    {
        PinPolicy mainThread;
        PinPolicy logThread;
        std::vector<alpbook::itch::StockTicker> stocks;
        DataSource dataSource;
        std::filesystem::path logDir;
    };

    auto parsePinPolicy(toml::node_view<toml::node const> node, std::string_view field)
        -> std::expected<PinPolicy, std::string>
    {
        if (!node)
        {
            return PinDisabled {};
        }
        if (auto const* str = node.as_string())
        {
            if (str->get() == "auto")
            {
                return PinAuto {};
            }
            if (str->get() == "none")
            {
                return PinDisabled {};
            }
            return std::unexpected(std::string("invalid value for ") + std::string(field)
                                   + ": expected integer, \"auto\", or \"none\"");
        }
        if (auto const* val = node.as_integer())
        {
            if (val->get() < 0)
            {
                return std::unexpected(std::string("negative core index for ")
                                       + std::string(field));
            }
            return PinExact {static_cast<uint32_t>(val->get())};
        }
        return std::unexpected(std::string("invalid type for ") + std::string(field)
                               + ": expected integer, \"auto\", or \"none\"");
    }

    std::string_view endpointError(network::ParseError e)
    {
        switch (e)
        {
            case network::ParseError::InvalidFormat:
                return "invalid format (expected a.b.c.d:port)";
            case network::ParseError::InvalidOctet:
                return "invalid address octet";
            case network::ParseError::InvalidPort:
                return "invalid port";
        }
        return "invalid endpoint";
    }

    auto parseEndpointField(toml::node_view<toml::node const> node, std::string_view field)
        -> std::expected<network::IpV4Endpoint, std::string>
    {
        if (!node)
        {
            return std::unexpected(std::format("missing {}", field));
        }
        auto const* str = node.as_string();
        if (str == nullptr)
        {
            return std::unexpected(std::format("{} must be a string", field));
        }
        auto ep = network::IpV4Endpoint::parse(str->get());
        if (!ep)
        {
            return std::unexpected(std::format("{}: {}", field, endpointError(ep.error())));
        }
        return *ep;
    }

    /// Parses a bare "a.b.c.d" IPv4 address (no port), as required for the optional
    /// multicast source filter.
    std::optional<std::array<uint8_t, 4>> parseIpV4Address(std::string_view input)
    {
        std::array<uint8_t, 4> address {};
        char const* ptr = input.data();
        char const* const end = input.data() + input.size();
        for (uint8_t i = 0; i < 4; ++i)
        {
            uint16_t octet {};
            auto [next, ec] = std::from_chars(ptr, end, octet);
            if (ec != std::errc {} || octet > 255)
            {
                return std::nullopt;
            }
            address[i] = static_cast<uint8_t>(octet);
            if (i < 3)
            {
                if (next >= end || *next != '.')
                {
                    return std::nullopt;
                }
                ptr = next + 1;
            }
            else if (next != end)
            {
                return std::nullopt;
            }
        }
        return address;
    }

    /// Applies an optional non-negative integer override, leaving the target untouched
    /// when the field is absent.
    auto applyUintOverride(toml::node_view<toml::node const> node,
                           std::string_view field,
                           uint32_t& target) -> std::expected<void, std::string>
    {
        if (!node)
        {
            return {};
        }
        auto const* val = node.as_integer();
        if (val == nullptr || val->get() < 0)
        {
            return std::unexpected(std::format("{} must be a non-negative integer", field));
        }
        target = static_cast<uint32_t>(val->get());
        return {};
    }

    auto parseNetworkSource(toml::table const& tbl) -> std::expected<NetworkSource, std::string>
    {
        auto const net = tbl["network"];
        if (!net)
        {
            return std::unexpected(std::string("missing [network] section"));
        }
        if (net.as_table() == nullptr)
        {
            return std::unexpected(std::string("[network] must be a table"));
        }

        network::AfXdpConfig config {};
        auto& live = config.liveConfig;
        auto& recovery = config.recoveryConfig;

        auto const* ifaceStr = net["interface"].as_string();
        if (ifaceStr == nullptr || ifaceStr->get().empty())
        {
            return std::unexpected(std::string("network.interface must be a non-empty string"));
        }
        live.interface = std::string(ifaceStr->get());

        auto const* queueVal = net["queue_id"].as_integer();
        if (queueVal == nullptr || queueVal->get() < 0)
        {
            return std::unexpected(std::string("network.queue_id must be a non-negative integer"));
        }
        live.queueId = static_cast<uint32_t>(queueVal->get());

        auto group = parseEndpointField(net["multicast_group"], "network.multicast_group");
        if (!group)
        {
            return std::unexpected(std::move(group.error()));
        }
        live.feed.group = *group;

        if (auto const src = net["multicast_source"])
        {
            auto const* srcStr = src.as_string();
            if (srcStr == nullptr)
            {
                return std::unexpected(std::string("network.multicast_source must be a string"));
            }
            auto addr = parseIpV4Address(srcStr->get());
            if (!addr)
            {
                return std::unexpected(
                    std::string("network.multicast_source: invalid IPv4 address"));
            }
            live.feed.source = *addr;
        }

        auto rewind = parseEndpointField(net["rewind_server"], "network.rewind_server");
        if (!rewind)
        {
            return std::unexpected(std::move(rewind.error()));
        }
        recovery.rewindServer = *rewind;

        auto glimpse = parseEndpointField(net["glimpse_server"], "network.glimpse_server");
        if (!glimpse)
        {
            return std::unexpected(std::move(glimpse.error()));
        }
        recovery.glimpseServer = *glimpse;

        auto const* userStr = net["glimpse_username"].as_string();
        if (userStr == nullptr)
        {
            return std::unexpected(std::string("network.glimpse_username must be a string"));
        }
        recovery.glimpseUsername = std::string(userStr->get());

        auto const* passStr = net["glimpse_password"].as_string();
        if (passStr == nullptr)
        {
            return std::unexpected(std::string("network.glimpse_password must be a string"));
        }
        recovery.glimpsePassword = std::string(passStr->get());

        // Optional tuning overrides; absent fields keep AfXdpConfig's defaults.
        for (auto [node, field, target] :
             {std::tuple {net["frame_size"], "network.frame_size", &live.frameSize},
              std::tuple {net["fill_ring_size"], "network.fill_ring_size", &live.fillRingSize},
              std::tuple {net["rx_ring_size"], "network.rx_ring_size", &live.rxRingSize},
              std::tuple {net["rx_batch"], "network.rx_batch", &live.rxBatch},
              std::tuple {net["sq_entries"], "network.sq_entries", &recovery.sqEntries}})
        {
            if (auto e = applyUintOverride(node, field, *target); !e)
            {
                return std::unexpected(std::move(e.error()));
            }
        }

        if (auto const cq = net["cq_entries"])
        {
            auto const* val = cq.as_integer();
            if (val == nullptr || val->get() < 0)
            {
                return std::unexpected(
                    std::string("network.cq_entries must be a non-negative integer"));
            }
            recovery.cqEntries = static_cast<uint32_t>(val->get());
        }

        if (auto const slots = net["recovery_buffer_slots"])
        {
            auto const* val = slots.as_integer();
            if (val == nullptr || val->get() <= 0)
            {
                return std::unexpected(
                    std::string("network.recovery_buffer_slots must be a positive integer"));
            }
            config.recoveryBufferSlots = static_cast<uint64_t>(val->get());
        }

        if (auto const rt = net["rewind_timeout_ns"])
        {
            auto const* val = rt.as_integer();
            if (val == nullptr || val->get() < 0)
            {
                return std::unexpected(
                    std::string("network.rewind_timeout_ns must be a non-negative integer"));
            }
            config.rewindTimeout = std::chrono::nanoseconds(val->get());
        }

        if (auto const gt = net["glimpse_connect_timeout_ns"])
        {
            auto const* val = gt.as_integer();
            if (val == nullptr || val->get() < 0)
            {
                return std::unexpected(std::string(
                    "network.glimpse_connect_timeout_ns must be a non-negative integer"));
            }
            config.glimpseConnectTimeout = std::chrono::nanoseconds(val->get());
        }

        return NetworkSource {std::move(config)};
    }

    auto parseDataSource(toml::table const& tbl) -> std::expected<DataSource, std::string>
    {
        auto const data = tbl["data"];
        if (!data)
        {
            return std::unexpected(std::string("missing [data] section"));
        }
        auto const source = data["source"];
        if (!source)
        {
            return std::unexpected(std::string("missing data.source"));
        }
        auto const* sourceStr = source.as_string();
        if (sourceStr == nullptr)
        {
            return std::unexpected(std::string("data.source must be a string"));
        }

        if (sourceStr->get() == "network")
        {
            auto net = parseNetworkSource(tbl);
            if (!net)
            {
                return std::unexpected(std::move(net.error()));
            }
            return std::move(*net);
        }
        if (sourceStr->get() == "simulated")
        {
            auto const inputFile = data["input_file"];
            if (!inputFile)
            {
                return std::unexpected(
                    std::string("data.input_file is required when source is \"simulated\""));
            }
            auto const* fileStr = inputFile.as_string();
            if (fileStr == nullptr || fileStr->get().empty())
            {
                return std::unexpected(std::string("data.input_file must be a non-empty string"));
            }
            return SimulatedSource {std::string(fileStr->get())};
        }

        return std::unexpected(std::string("unknown data.source: \"")
                               + std::string(sourceStr->get())
                               + "\", expected \"network\" or \"simulated\"");
    }

    auto parseStocks(toml::table const& tbl)
        -> std::expected<std::vector<alpbook::itch::StockTicker>, std::string>
    {
        auto const stocks = tbl["stocks"];
        if (!stocks)
        {
            return std::unexpected(std::string("missing stocks list"));
        }
        auto const* arr = stocks.as_array();
        if (arr == nullptr)
        {
            return std::unexpected(std::string("stocks must be an array"));
        }
        if (arr->empty())
        {
            return std::unexpected(std::string("stocks list must not be empty"));
        }

        std::vector<alpbook::itch::StockTicker> result;
        result.reserve(arr->size());
        for (auto const& elem : *arr)
        {
            auto const* str = elem.as_string();
            if (str == nullptr)
            {
                return std::unexpected(std::string("each stock must be a string"));
            }
            if (str->get().size() > alpbook::itch::STOCK_TICKER_LEN)
            {
                return std::unexpected(std::format("stock ticker \"{}\" exceeds max length {}",
                                                   str->get(),
                                                   alpbook::itch::STOCK_TICKER_LEN));
            }
            alpbook::itch::StockTicker ticker {};
            ticker.fill(' ');
            std::copy(str->get().begin(), str->get().end(), ticker.begin());
            result.push_back(ticker);
        }
        return result;
    }

    export auto parse(toml::table const& tbl) -> std::expected<Config, std::string>
    {
        auto const pinning = tbl["pinning"];

        auto mainPin = parsePinPolicy(pinning["main_thread"], "pinning.main_thread");
        if (!mainPin)
        {
            return std::unexpected(std::move(mainPin.error()));
        }

        auto logPin = parsePinPolicy(pinning["log_thread"], "pinning.log_thread");
        if (!logPin)
        {
            return std::unexpected(std::move(logPin.error()));
        }

        auto stocks = parseStocks(tbl);
        if (!stocks)
        {
            return std::unexpected(std::move(stocks.error()));
        }

        auto dataSource = parseDataSource(tbl);
        if (!dataSource)
        {
            return std::unexpected(std::move(dataSource.error()));
        }

        auto const logDir = tbl["log_dir"];
        if (!logDir)
        {
            return std::unexpected(std::string("missing log_dir"));
        }
        auto const* logDirStr = logDir.as_string();
        if (logDirStr == nullptr || logDirStr->get().empty())
        {
            return std::unexpected(std::string("log_dir must be a non-empty string"));
        }

        return Config {
            .mainThread = std::move(*mainPin),
            .logThread = std::move(*logPin),
            .stocks = std::move(*stocks),
            .dataSource = std::move(*dataSource),
            .logDir = std::filesystem::path(logDirStr->get()),
        };
    }
}  // namespace alpdaq::config
