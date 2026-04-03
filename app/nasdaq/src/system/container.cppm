module;

#include <concepts>
#include <cstdint>
#include <vector>

export module alpdaq.system.container;

import alpbook.book.nasdaq;
import alpbook.itch.messages;

namespace alpdaq::system
{
    using namespace alpbook;

    export template<typename T>
    concept StrategyContainer = requires(T container,
                                         std::vector<itch::StockTicker> const& tickers,
                                         uint16_t assetId,
                                         itch::StockTicker ticker,
                                         nasdaq::AddOrder addMsg,
                                         nasdaq::ExecuteOrder execMsg,
                                         nasdaq::DecrementShares reduceMsg,
                                         nasdaq::CancelOrder cancelMsg,
                                         nasdaq::ReplaceOrder replaceMsg,
                                         itch::TradingState tradingState) {
        { container.init(tickers) } -> std::same_as<void>;
        { container.onStockDirectory(assetId, ticker) } -> std::same_as<void>;

        { container.add(assetId, addMsg) } -> std::same_as<void>;
        { container.execute(assetId, execMsg) } -> std::same_as<void>;
        { container.reduce(assetId, reduceMsg) } -> std::same_as<void>;
        { container.cancel(assetId, cancelMsg) } -> std::same_as<void>;
        { container.replace(assetId, replaceMsg) } -> std::same_as<void>;

        { container.onTradingAction(assetId, tradingState) } -> std::same_as<void>;
        { container.clearAll() } -> std::same_as<void>;
    };
}  // namespace alpdaq::system
