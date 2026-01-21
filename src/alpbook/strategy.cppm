module;

#include <concepts>
#include <cstdint>

import alpbook.common;
import alpbook.book.core;

export module alpbook.strategy;

namespace alpbook::strategy
{
    export template<typename T, typename B>
    concept Strategy =
        requires(T strategy, B* bookPtr, uint64_t price, uint32_t qty, Side side) {
            { strategy.setBook(bookPtr) } -> std::same_as<void>;
            { strategy.setAsset(uint16_t {}) } -> std::same_as<void>;

            { strategy.onTrade(price, qty, side) } -> std::same_as<void>;
            { strategy.onTopBidChange(price, qty) } -> std::same_as<void>;
            { strategy.onTopAskChange(price, qty) } -> std::same_as<void>;

            { strategy.onSystemHalt() } -> std::same_as<void>;
            { strategy.onRecoveryStart() } -> std::same_as<void>;
            { strategy.onSystemRestart() } -> std::same_as<void>;
        };

    export template<typename T, typename B>
    concept ExtendedStrategy = Strategy<T, B> && ExtendedBook<B>;

    export template<typename F, typename S, typename B>
    concept StrategyFactory = std::copy_constructible<F> && requires(F factory, uint16_t assetId) {
        { factory.create(assetId) } -> std::same_as<S>;
        requires Strategy<S, B>;
    };
}  // namespace alpbook::strategy