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
    template<typename S>
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

        void add(AddOrder order)
        {
            if (!isHealthy_) [[unlikely]]
            {
                return;
            }
            book.add(order);
        }

        void execute(ExecuteOrder order)
        {
            if (!isHealthy_) [[unlikely]]
            {
                return;
            }
            auto result = book.execute(order);
            if (!result.has_value()) [[unlikely]]
            {
                tripCircuitBreaker(result.error());
            }
        }

        void reduce(DecrementShares order)
        {
            if (!isHealthy_) [[unlikely]]
            {
                return;
            }
            auto result = book.reduce(order);
            if (!result.has_value()) [[unlikely]]
            {
                tripCircuitBreaker(result.error());
            }
        }

        void cancel(CancelOrder order)
        {
            if (!isHealthy_) [[unlikely]]
            {
                return;
            }
            auto result = book.cancel(order);
            if (!result.has_value()) [[unlikely]]
            {
                tripCircuitBreaker(result.error());
            }
        }

        void replace(ReplaceOrder order)
        {
            if (!isHealthy_) [[unlikely]]
            {
                return;
            }
            auto result = book.replace(order);
            if (!result.has_value()) [[unlikely]]
            {
                tripCircuitBreaker(result.error());
            }
        }

        void apply(auto const& func)
        {
            if (!isHealthy_) [[unlikely]]
            {
                return;
            }
            func(book);
        }

      private:
        void tripCircuitBreaker(BookError e)
        {
            isHealthy_ = false;
            strategy.onSystemHalt();
            book.reset();
        }
    };

    template<typename S>
        requires strategy::Strategy<S, Book<S>>
    class Sink
    {
        using Ctx = Context<S>;

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

        void onMessage(itch::ItchSlot data)
        {
            auto id = itch::ItchExtractor::extractID(data);
            itch::parse(data, *contexts_[id]);
        }

      private:
        std::array<std::unique_ptr<Ctx>, 65536> contexts_;
    };
}  // namespace alpbook::nasdaq