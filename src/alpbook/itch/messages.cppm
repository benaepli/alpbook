module;

#include <array>
#include <cstdint>

import alpbook.book.nasdaq;

export module alpbook.itch.messages;

namespace alpbook::itch
{
    export enum class MessageClassification : std::uint8_t
    {
        /// Book events directly relate to order state.
        Order,

        /// System events are global changes.
        SystemEvent,

        /// Stock directories are sent before market open.
        StockDirectory,

        /// Asset-specific state changes.
        StockTradingAction,

        /// All other (irrelevant) message types.
        Ignored,
    };

    export constexpr auto STOCK_TICKER_LEN = 8UL;
    export using StockTicker = std::array<uint8_t, STOCK_TICKER_LEN>;

    /// Relevant fields in the stock directory message.
    export struct StockDirectory
    {
        StockTicker stock;

        /// Drop the message if it's inauthentic.
        bool authenticity;
    };

    export namespace events
    {
        struct StartOfMarket
        {
        };

        struct EndOfMarket
        {
        };

        struct StartOfSystem
        {
        };

        struct EndOfSystem
        {
        };

        struct StartOfMessages
        {
        };

        struct EndOfMessages
        {
        };
    }  // namespace events

    export enum class TradingState : uint8_t
    {
        Trading,
        Halt,
        Paused,
        Quotation,
    };

    export struct StockTradingAction
    {
        TradingState state;
    };

    export enum class ParseError : uint8_t
    {
        InsufficientData,
    };
}  // namespace alpbook::itch