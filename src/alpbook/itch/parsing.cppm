module;

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <variant>
#include <vector>

#include "alpbook/internal/hints.hpp"

export module alpbook.itch.parsing;

import alpbook.book.nasdaq;
import alpbook.common;
import alpbook.itch.messages;

export import :listener;

namespace alpbook::itch
{
    using ItchBytes = std::array<std::byte, 55>;

    export template<bool Benchmark = false>
    struct alignas(std::hardware_destructive_interference_size) ItchSlot
    {
        ItchBytes data;
        [[no_unique_address]] std::conditional_t<Benchmark, int64_t, std::monostate>
            dispatchTimestamp;
    };

    export inline constexpr size_t CLASSIFY_MESSAGE_SIZE = 1;
    export inline constexpr size_t PARSE_ID_SIZE = 3;
    export inline constexpr size_t SYSTEM_EVENT_MESSAGE_SIZE = 12;
    export inline constexpr size_t STOCK_TRADING_ACTION_MESSAGE_SIZE = 20;
    export inline constexpr size_t STOCK_DIRECTORY_MESSAGE_SIZE = 39;

    template<typename T>
        requires std::is_integral_v<T>
    T parseField(std::span<std::byte const> msg, size_t offset)
    {
        T val;
        std::memcpy(&val, msg.data() + offset, sizeof(T));
        return std::byteswap(val);
    }

    export uint16_t parseID(std::span<std::byte const> msg)
    {
        return parseField<uint16_t>(msg, 1);
    };

    uint64_t parseTimestamp(std::span<std::byte const> msg)
    {
        uint64_t const high = parseField<uint16_t>(msg, 5);
        uint64_t const low = parseField<uint32_t>(msg, 7);
        return (high << 32) | low;
    }

    export ALPBOOK_INLINE MessageClassification classifyMessage(std::span<std::byte const> msg)
    {
        char const msgType = std::to_integer<char>(msg[0]);
        [[likely]] if (msgType == 'A' || msgType == 'F' || msgType == 'E' || msgType == 'C'
                       || msgType == 'X' || msgType == 'D' || msgType == 'U')
        {
            return MessageClassification::Order;
        }
        if (msgType == 'R')
        {
            return MessageClassification::StockDirectory;
        }
        if (msgType == 'S')
        {
            return MessageClassification::SystemEvent;
        }
        if (msgType == 'H')
        {
            return MessageClassification::StockTradingAction;
        }
        return MessageClassification::Ignored;
    }

    /// Parse an ITCH system event message and dispatch to the listener.
    export template<SystemEventListener L>
    ALPBOOK_INLINE void parseSystemEventMessage(std::span<std::byte const> bytes,
                                                L& listener) noexcept
    {
        char const eventCode = std::to_integer<char>(bytes[11]);
        switch (eventCode)
        {
            case 'O':
                listener.startOfMessages(events::StartOfMessages {});
                break;
            case 'S':
                listener.startOfSystem(events::StartOfSystem {});
                break;
            case 'Q':
                listener.startOfMarket(events::StartOfMarket {});
                break;
            case 'M':
                listener.endOfMarket(events::EndOfMarket {});
                break;
            case 'E':
                listener.endOfSystem(events::EndOfSystem {});
                break;
            case 'C':
                listener.endOfMessages(events::EndOfMessages {});
                break;
            default:
                break;
        }
    }

    /// Parse an ITCH stock directory message into the struct representation.
    export ALPBOOK_INLINE StockDirectory parseStockDirectoryMessage(
        std::span<std::byte const> bytes) noexcept
    {
        StockTicker stock;
        std::memcpy(stock.data(), bytes.data() + 11, STOCK_TICKER_LEN);
        bool const authenticity = std::to_integer<char>(bytes[29]) == 'P';
        return StockDirectory {.stock = stock, .authenticity = authenticity};
    }

    /// Parse an ITCH stock trading action message into the struct representation.
    export ALPBOOK_INLINE StockTradingAction
    parseStockTradingActionMessage(std::span<std::byte const> bytes) noexcept
    {
        char const stateCode = std::to_integer<char>(bytes[19]);
        switch (stateCode)
        {
            case 'T':
            {
                return StockTradingAction {.state = TradingState::Trading};
            }
            case 'H':
            {
                return StockTradingAction {.state = TradingState::Halt};
            }
            case 'P':
            {
                return StockTradingAction {.state = TradingState::Paused};
            }
            case 'Q':
            {
                return StockTradingAction {.state = TradingState::Quotation};
            }
            default:
            {
                std::terminate();
            }
        }
    }

    /// Parse an ITCH order-related message and dispatch to the listener.
    /// Order-related messages are the ones directly consumed by order books
    /// and should be parsed after you know the correct locate ID.
    export template<OrderListener L>
    ALPBOOK_INLINE std::expected<void, ParseError> parseOrderMessage(
        std::span<std::byte const> bytes, L& listener) noexcept
    {
        char const msgType = std::to_integer<char>(bytes[0]);

        switch (msgType)
        {
            case 'A':
            case 'F':
            {
                if (bytes.size() < 36) [[unlikely]]
                {
                    return std::unexpected(ParseError::InsufficientData);
                }

                auto const timestamp = parseTimestamp(bytes);
                auto const id = parseField<uint64_t>(bytes, 11);
                auto const side = std::to_integer<char>(bytes[19]);
                auto const shares = parseField<uint32_t>(bytes, 20);
                auto const price = parseField<uint32_t>(bytes, 32);

                listener.add(AddOrder {.timestamp = timestamp,
                                       .id = id,
                                       .price = price,
                                       .shares = shares,
                                       .side = (side == 'B' ? Side::Buy : Side::Sell)});
                break;
            }

            case 'E':
            case 'C':
            {
                if (bytes.size() < 23) [[unlikely]]
                {
                    return std::unexpected(ParseError::InsufficientData);
                }

                auto const id = parseField<uint64_t>(bytes, 11);
                auto const shares = parseField<uint32_t>(bytes, 19);

                listener.execute(ExecuteOrder {.id = id, .shares = shares});
                break;
            }

            case 'X':
            {
                if (bytes.size() < 23) [[unlikely]]
                {
                    return std::unexpected(ParseError::InsufficientData);
                }

                auto const id = parseField<uint64_t>(bytes, 11);
                auto const shares = parseField<uint32_t>(bytes, 19);

                listener.reduce(DecrementShares {.id = id, .shares = shares});
                break;
            }

            case 'D':
            {
                if (bytes.size() < 19) [[unlikely]]
                {
                    return std::unexpected(ParseError::InsufficientData);
                }

                auto const id = parseField<uint64_t>(bytes, 11);

                listener.cancel(CancelOrder {.id = id});
                break;
            }

            case 'U':
            {
                if (bytes.size() < 35) [[unlikely]]
                {
                    return std::unexpected(ParseError::InsufficientData);
                }

                auto const timestamp = parseTimestamp(bytes);
                auto const oldId = parseField<uint64_t>(bytes, 11);
                auto const newId = parseField<uint64_t>(bytes, 19);
                auto const shares = parseField<uint32_t>(bytes, 27);
                auto const price = parseField<uint32_t>(bytes, 31);

                listener.replace(ReplaceOrder {.timestamp = timestamp,
                                               .oldId = oldId,
                                               .newId = newId,
                                               .price = price,
                                               .shares = shares});
                break;
            }

            default:
                break;
        }
        return {};
    }
}  // namespace alpbook::itch