module;

#include <cstdint>
#include <memory>

export module benchmark.strategy;

import alpbook.common;
import alpbook.strategy;
import alpbook.book;
import alpdaq.system.container.strategized;
import benchmark.logger;

namespace benchmark
{
    using namespace alpbook;

    export class BenchmarkStrategy
    {
      public:
        explicit BenchmarkStrategy(std::shared_ptr<BenchmarkLogger> logger)
            : logger_(std::move(logger))
        {
        }

        template<typename B>
        void setBook(B*)
        {
        }
        void setAsset(uint16_t) {}

        void onTrade(price_t, quantity_t, Side) { logger_->recordStrategy(); }
        void onTopBidChange(price_t, quantity_t) { logger_->recordStrategy(); }
        void onTopAskChange(price_t, quantity_t) { logger_->recordStrategy(); }

        void onHalt() {}
        void onResume() {}

      private:
        std::shared_ptr<BenchmarkLogger> logger_;
    };

    using BenchmarkBook = nasdaq::Book<nasdaq::PolicyHash, BenchmarkStrategy>;

    static_assert(strategy::Strategy<BenchmarkStrategy, BenchmarkBook>);

    export class BenchmarkStrategyFactory
    {
      public:
        explicit BenchmarkStrategyFactory(std::shared_ptr<BenchmarkLogger> logger)
            : logger_(std::move(logger))
        {
        }

        BenchmarkStrategy create(uint16_t) const { return BenchmarkStrategy(logger_); }

      private:
        std::shared_ptr<BenchmarkLogger> logger_;
    };

    static_assert(alpdaq::system::container::
                      StrategyFactory<BenchmarkStrategyFactory, BenchmarkStrategy, BenchmarkBook>);
}  // namespace benchmark
