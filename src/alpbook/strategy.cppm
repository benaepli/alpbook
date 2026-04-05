module;

#include <concepts>
#include <cstdint>

export module alpbook.strategy;

import alpbook.common;
import alpbook.book.core;

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

            { strategy.onHalt() } -> std::same_as<void>;
            { strategy.onResume() } -> std::same_as<void>;
        };

    export template<typename T, typename B>
    concept ExtendedStrategy = Strategy<T, B> && ExtendedBook<B>;
}  // namespace alpbook::strategy