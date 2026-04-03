module;

#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include <tscns.h>

export module alpbook_latency.sink;

import alpbook;
import alpbook.sink.nasdaq;
import alpbook.strategy;
import alpbook_latency.strategy;

namespace alpbook_latency
{
    export class BenchmarkSinkFactory
    {
        BenchmarkStrategyFactory strategyFactory_;

      public:
        using SinkType = alpbook::nasdaq::
            Sink<alpbook::nasdaq::PolicyHash, BenchmarkStrategy, BenchmarkStrategyFactory, true>;

        explicit BenchmarkSinkFactory(BenchmarkStrategyFactory sf)
            : strategyFactory_(sf)
        {
        }

        template<typename Mapper>
        SinkType create(uint32_t core, Mapper const& mapper)
        {
            return SinkType(core, mapper, strategyFactory_);
        }
    };
}  // namespace alpbook_latency
