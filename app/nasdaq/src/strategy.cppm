module;

#include <cstdint>

export module nasdaq.strategy;

import alpbook.common;
import alpbook.strategy;
import alpbook.book;
import alpdaq.system.container.strategized;

namespace app
{
    using namespace alpbook;

    /// A do-nothing strategy: it satisfies the Strategy concept but reacts to nothing.
    export class EmptyStrategy
    {
      public:
        template<typename B>
        void setBook(B*)
        {
        }
        void setAsset(uint16_t) {}

        void onTrade(price_t, quantity_t, Side) {}
        void onTopBidChange(price_t, quantity_t) {}
        void onTopAskChange(price_t, quantity_t) {}

        void onHalt() {}
        void onResume() {}
    };

    using EmptyBook = nasdaq::Book<nasdaq::PolicyHash, EmptyStrategy>;

    static_assert(strategy::Strategy<EmptyStrategy, EmptyBook>);

    export class EmptyStrategyFactory
    {
      public:
        EmptyStrategy create(uint16_t) const { return EmptyStrategy {}; }
    };

    static_assert(alpdaq::system::container::
                      StrategyFactory<EmptyStrategyFactory, EmptyStrategy, EmptyBook>);
}  // namespace app
