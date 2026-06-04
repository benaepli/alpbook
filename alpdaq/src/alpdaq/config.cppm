module;

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <variant>
#include <vector>

#include <toml++/toml.hpp>

export module alpdaq.config;

import alpbook.itch;

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
            return NetworkSource {};
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
