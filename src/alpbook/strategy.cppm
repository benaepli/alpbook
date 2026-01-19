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
        Book<B> && requires(T strategy, B* bookPtr, uint64_t price, uint32_t qty, Side side) {
            { strategy.setBook(bookPtr) } -> std::same_as<void>;
            { strategy.setAsset(uint16_t {}) } -> std::same_as<void>;

            { strategy.onTrade(price, qty, side) } -> std::same_as<void>;
            { strategy.onTopBidChange(price, qty) } -> std::same_as<void>;
            { strategy.onTopAskChange(price, qty) } -> std::same_as<void>;
            { strategy.onSystemHalt() } -> std::same_as<void>;
            { strategy.onSystemRestart() } -> std::same_as<void>;
        };

    export template<typename T, typename B>
    concept ExtendedStrategy = Strategy<T, B> && ExtendedBook<B>;
}  // namespace alpbook::strategy