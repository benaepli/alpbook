module;

import alpbook.common;

#include <concepts>
#include <cstdint>
#include <type_traits>

export module alpbook.book.core;

namespace alpbook
{
    export template<typename T>
    concept Listener = requires(T l, price_t price, quantity_t quantity) {
        l.onTopBidChange(price, quantity);
        l.onTopAskChange(price, quantity);
        l.onTrade(price, quantity, Side {});
    };

    export template<typename T>
    concept Book = requires(T b, int depth, price_t targetPrice) {
        { b.getBestBid() } -> std::same_as<Level>;
        { b.getBestAsk() } -> std::same_as<Level>;
        { b.getBidLevel(depth) } -> std::same_as<Level>;
        { b.getAskLevel(depth) } -> std::same_as<Level>;

        { b.getBuyVolumeAhead(targetPrice) } -> std::same_as<quantity_t>;
        { b.getSellVolumeAhead(targetPrice) } -> std::same_as<quantity_t>;
    };

    export template<typename T>
    concept ExtendedBook = Book<T> && requires(T b, uint64_t orderID) {
        { b.getBuyVolumeAheadByOrder(orderID) } -> std::same_as<quantity_t>;
        { b.getSellVolumeAheadByOrder(orderID) } -> std::same_as<quantity_t>;
    };
}  // namespace alpbook