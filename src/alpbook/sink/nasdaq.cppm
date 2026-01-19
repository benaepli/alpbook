module;

#include <cstdint>
#include <memory>
#include <vector>

import alpbook.strategy;
import alpbook.book.core;
import alpbook.book.nasdaq;
import alpbook.dispatch;
import alpbook.itch.parsing;

export module alpbook.sink.nasdaq;

namespace alpbook::nasdaq
{
    export template<typename S, bool Benchmark = false>
        requires strategy::Strategy<S, Book<S>>
    struct Context
    {
        using BookType = Book<S>;

        S strategy {};
        BookType book;

        bool isHealthy_ = true;

        Context(uint16_t id)
            : book(strategy)
        {
            strategy.setAsset(id);
            strategy.setBook(&book);
        }

        void add(AddOrder order) { book.add(order); }

        void execute(ExecuteOrder order)
        {
            auto result = book.execute(order);
            if (!result.has_value()) [[unlikely]]
            {
                tripCircuitBreaker(result.error());
            }
        }

        void reduce(DecrementShares order)
        {
            auto result = book.reduce(order);
            if (!result.has_value()) [[unlikely]]
            {
                tripCircuitBreaker(result.error());
            }
        }

        void cancel(CancelOrder order)
        {
            auto result = book.cancel(order);
            if (!result.has_value()) [[unlikely]]
            {
                tripCircuitBreaker(result.error());
            }
        }

        void replace(ReplaceOrder order)
        {
            auto result = book.replace(order);
            if (!result.has_value()) [[unlikely]]
            {
                tripCircuitBreaker(result.error());
            }
        }

        void apply(auto const& func) { func(book); }

        [[nodiscard]] bool handleOrigin(itch::MessageOrigin origin)
        {
            if (origin == itch::MessageOrigin::SnapshotStart) [[unlikely]]
            {
                recover();
                return true;
            }
            return isHealthy_;
        }

        void jobStartedAt([[maybe_unused]] uint64_t tsc)
        {
            if constexpr (Benchmark)
            {
                strategy.jobStartedAt(tsc);
            }
        }

      private:
        void tripCircuitBreaker(BookError e)
        {
            isHealthy_ = false;
            strategy.onSystemHalt();
            book.reset();
        }

        void recover()
        {
            book.reset();
            isHealthy_ = true;
            strategy.onSystemRestart();
        }
    };

    export template<typename S, bool B = false>
        requires strategy::Strategy<S, Book<S>>
    class Sink
    {
        using Ctx = Context<S, B>;

      public:
        template<typename Mapper>
            requires EnumerableMapper<Mapper, uint16_t>
        Sink(uint32_t coreIndex, Mapper const& mapper)
        {
            auto ids = mapper.getIDsForThread(coreIndex);
            for (auto id : ids)
            {
                contexts_[id] = std::make_unique<Ctx>(id);
            }
        }

        void onMessage(itch::ItchSlot<B> data)
        {
            auto id = itch::ItchExtractor::extractID(data);
            auto& context = *contexts_[id];

            if (!context.handleOrigin(data.type)) [[unlikely]]
            {
                return;
            }

            if constexpr (B)
            {
                context.jobStartedAt(data.dispatchTimestamp);
            }
            itch::parse(data.data, context);
        }

      private:
        std::array<std::unique_ptr<Ctx>, 65536> contexts_;
    };
}  // namespace alpbook::nasdaq