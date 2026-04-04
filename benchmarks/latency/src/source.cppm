module;

#include <memory>
#include <utility>

export module benchmark.source;

import alpdaq.simulated.binary;
import benchmark.logger;

namespace benchmark
{
    export class BenchmarkSource
    {
      public:
        BenchmarkSource(alpdaq::simulated::BinaryItchSource source,
                        std::shared_ptr<BenchmarkLogger> logger) noexcept
            : source_(std::move(source))
            , logger_(std::move(logger))
        {
        }

        template<typename DataCb, typename EventCb>
        void poll(DataCb onData, EventCb onEvent) noexcept
        {
            source_.poll(
                [&](auto const& view) noexcept {
                    logger_->recordDispatch();
                    onData(view);
                },
                onEvent);
        }

        void forceRestart() noexcept { source_.forceRestart(); }

      private:
        alpdaq::simulated::BinaryItchSource source_;
        std::shared_ptr<BenchmarkLogger> logger_;
    };
}  // namespace benchmark
