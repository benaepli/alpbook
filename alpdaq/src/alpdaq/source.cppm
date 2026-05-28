module;

#include <memory>
#include <utility>

export module alpdaq.source;

import alpdaq.system.state;

namespace alpdaq
{
    export template<typename T>
    class MovableSource
    {
      public:
        explicit MovableSource(std::unique_ptr<T> ptr) noexcept
            : ptr_(std::move(ptr))
        {
        }

        template<typename DataCb, typename EventCb>
        void poll(DataCb onData, EventCb onEvent) noexcept
        {
            ptr_->poll(std::forward<DataCb>(onData), std::forward<EventCb>(onEvent));
        }

        void forceRestart() noexcept { ptr_->forceRestart(); }

      private:
        std::unique_ptr<T> ptr_;
    };
}  // namespace alpdaq
