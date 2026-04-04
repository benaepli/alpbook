module;

#include <cstdint>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <tscns.h>

export module benchmark.logger;

import alpdaq.system.state;

namespace benchmark
{
    export class BenchmarkLogger
    {
      public:
        BenchmarkLogger() { clock_.init(); }

        void recordDispatch() noexcept { pendingDispatch_ = clock_.rdtsc(); }

        void recordStrategy() noexcept { samples_.emplace_back(pendingDispatch_, clock_.rdtsc()); }

        void saveToCSV(std::string const& path) const
        {
            std::ofstream file(path);
            file << "latency_ns\n";
            for (auto const& [dispatchTsc, strategyTsc] : samples_)
            {
                auto dispatchNs = clock_.tsc2ns(dispatchTsc);
                auto strategyNs = clock_.tsc2ns(strategyTsc);
                file << (strategyNs - dispatchNs) << "\n";
            }
        }

        void logPreMarket() {}
        void logGapRecovery() {}
        void logTotalRecovery() {}
        void logRecoveryComplete() {}
        void logInconsistency() {}
        void logForceRestart() {}
        void logFatalInconsistency() {}
        void logSessionChange(alpdaq::SessionId) {}
        void logSystemStarted() {}
        void logSystemStopped() {}
        void rotateSession() {}

      private:
        TSCNS clock_;
        int64_t pendingDispatch_ = 0;
        std::vector<std::pair<int64_t, int64_t>> samples_;
    };
}  // namespace benchmark
