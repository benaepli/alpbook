module;

#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include <tscns.h>

import alpbook;

export module alpbook_latency.strategy;

namespace alpbook_latency
{
    export struct BenchmarkData
    {
        std::mutex mutex;
        std::vector<int64_t> latencies;
        TSCNS clock;

        BenchmarkData() { clock.init(); }

        void saveToCSV(std::string const& filename)
        {
            std::lock_guard lock(mutex);
            std::ofstream file(filename);
            file << "latency_ns\n";
            for (auto v : latencies)
            {
                file << v << "\n";
            }
        }
    };

    export class BenchmarkStrategy
    {
      public:
        static constexpr size_t MAX_LATENCIES = 1U << 20;
        static constexpr size_t WARMUP_THRESHOLD = 10000;

        BenchmarkStrategy(BenchmarkData* data)
            : data_(data)
        {
            latencies_.reserve(MAX_LATENCIES);
            for (size_t i = 0; i < MAX_LATENCIES; ++i)
            {
                latencies_.push_back(0);
            }
            latencies_.clear();
        }

        ~BenchmarkStrategy()
        {
            std::lock_guard lock(data_->mutex);
            if (latencies_.size() < MAX_LATENCIES)
            {
                data_->latencies.insert(
                    data_->latencies.end(), latencies_.begin(), latencies_.end());
            }
            else
            {
                data_->latencies.insert(
                    data_->latencies.end(), latencies_.begin() + cursor_, latencies_.end());
                data_->latencies.insert(
                    data_->latencies.end(), latencies_.begin(), latencies_.begin() + cursor_);
            }
        }

        template<typename B>
        void setBook(B*)
        {
        }
        void setAsset(uint16_t) {}
        void onTrade(alpbook::price_t, alpbook::quantity_t, alpbook::Side) { record(); }
        void onTopBidChange(alpbook::price_t, alpbook::quantity_t) { record(); }
        void onTopAskChange(alpbook::price_t, alpbook::quantity_t) { record(); }
        void onSystemHalt() {}
        void onRecoveryStart() {}
        void onSystemRestart() {}

        void jobStartedAt(uint64_t tsc) { lastTsc_ = tsc; }

      private:
        void record()
        {
            if (lastTsc_ == 0)
            {
                return;
            }
            if (messagesSeen_ < WARMUP_THRESHOLD)
            {
                messagesSeen_++;
                return;
            }

            int64_t currentTsc = data_->clock.rdtsc();

            int64_t startNs = data_->clock.tsc2ns(lastTsc_);
            int64_t endNs = data_->clock.tsc2ns(currentTsc);
            int64_t latency = endNs - startNs;

            if (latencies_.size() < MAX_LATENCIES)
            {
                latencies_.push_back(latency);
            }
            else
            {
                latencies_[cursor_] = latency;
                cursor_ = (cursor_ + 1) & (MAX_LATENCIES - 1);
            }
        }

        BenchmarkData* data_;
        int64_t lastTsc_ = 0;
        std::vector<int64_t> latencies_;
        size_t cursor_ = 0;
        size_t messagesSeen_ = 0;
    };

    static_assert(alpbook::strategy::Strategy<
                  BenchmarkStrategy,
                  alpbook::nasdaq::Book<alpbook::nasdaq::PolicyHash, BenchmarkStrategy>>);

    export class BenchmarkStrategyFactory
    {
      public:
        using StrategyType = BenchmarkStrategy;

        BenchmarkStrategyFactory(BenchmarkData& data)
            : data_(&data)
        {
        }

        BenchmarkStrategy create(uint16_t) const { return BenchmarkStrategy(data_); }

      private:
        BenchmarkData* data_;
    };

    static_assert(alpbook::strategy::StrategyFactory<
                  BenchmarkStrategyFactory,
                  BenchmarkStrategy,
                  alpbook::nasdaq::Book<alpbook::nasdaq::PolicyHash, BenchmarkStrategy>>);

}  // namespace alpbook_latency