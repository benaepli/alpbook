module;

#include <concepts>

import alpbook.book.nasdaq;
import alpbook.itch.messages;

export module alpbook.itch.parsing:listener;

namespace alpbook::itch
{
    using namespace alpbook::nasdaq;

    export template<typename T>
    concept OrderListener = requires(T listener,
                                     AddOrder addMsg,
                                     ExecuteOrder execMsg,
                                     DecrementShares reduceMsg,
                                     CancelOrder cancelMsg,
                                     ReplaceOrder replaceMsg) {
        { listener.add(addMsg) } -> std::same_as<void>;
        { listener.execute(execMsg) } -> std::same_as<void>;
        { listener.reduce(reduceMsg) } -> std::same_as<void>;
        { listener.cancel(cancelMsg) } -> std::same_as<void>;
        { listener.replace(replaceMsg) } -> std::same_as<void>;
    };

    export template<typename T>
    concept SystemEventListener = requires(T listener,
                                           events::StartOfMessages startMessages,
                                           events::StartOfSystem startSystem,
                                           events::StartOfMarket startMarket,
                                           events::EndOfMarket endMarket,
                                           events::EndOfSystem endSystem,
                                           events::EndOfMessages endMessages) {
        { listener.startOfMessages(startMessages) } -> std::same_as<void>;
        { listener.startOfSystem(startSystem) } -> std::same_as<void>;
        { listener.startOfMarket(startMarket) } -> std::same_as<void>;
        { listener.endOfMarket(endMarket) } -> std::same_as<void>;
        { listener.endOfSystem(endSystem) } -> std::same_as<void>;
        { listener.endOfMessages(endMessages) } -> std::same_as<void>;
    };

}  // namespace alpbook::itch
