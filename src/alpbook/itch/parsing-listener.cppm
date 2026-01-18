module;

import alpbook.book.nasdaq;

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
}  // namespace alpbook::itch
