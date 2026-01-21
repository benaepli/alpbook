module;

#include <cstdint>
#include <memory>
#include <memory_resource>
#include <print>
#include <vector>

#include "alpbook/internal/hints.hpp"

import alpbook.strategy;
import alpbook.book.core;
import alpbook.book.nasdaq;
import alpbook.dispatch;
import alpbook.itch.parsing;

export module alpbook.sink.nasdaq;

namespace alpbook::nasdaq
{
    export enum class ContextState : uint8_t
    {
        Normal,
        Failed,
        Recovering,
    };

    export template<typename Policy, typename S, typename SF, bool Benchmark = false>
        requires strategy::Strategy<S, Book<Policy, S>>
        && strategy::StrategyFactory<SF, S, Book<Policy, S>>
    struct Context
    {
        using BookType = Book<Policy, S>;

        S strategy;
        BookType book;
        ContextState state = ContextState::Normal;

        Context(uint16_t id, SF& strategyFactory)
            : strategy(strategyFactory.create(id))
            , book(strategy)
        {
            strategy.setAsset(id);
            strategy.setBook(&book);
        }

        Context(Context&&) = delete;
        Context& operator=(Context&&) = delete;

        Context(Context const&) = delete;
        Context& operator=(Context const&) = delete;

        void add(AddOrder order)
        {
            if (state == ContextState::Recovering) [[unlikely]]
            {
                book.addUnordered(order);
            }
            else [[likely]]
            {
                book.add(order);
            }
        }

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

        /// Handles the origin of messages. Returns true if the message should be processed.
        ALPBOOK_INLINE bool handleOrigin(itch::MessageOrigin origin)
        {
            switch (origin)
            {
                [[likely]] case itch::MessageOrigin::Live:
                {
                    return state == ContextState::Normal;
                }
                case itch::MessageOrigin::Recovery:
                {
                    if (state != ContextState::Recovering) [[unlikely]]
                    {
                        tripCircuitBreaker(BookError::MissingId);  // TODO: improve handling
                        return false;
                    }
                    return true;
                }
                [[unlikely]] case itch::MessageOrigin::SnapshotStart:
                {
                    beginRecovery();
                    return true;
                }
                [[unlikely]] case itch::MessageOrigin::SnapshotEnd:
                {
                    if (state != ContextState::Recovering)
                    {
                        tripCircuitBreaker(BookError::MissingId);  // TODO: improve handling
                        return false;
                    }
                    endRecovery();
                    return true;
                }
                default:
                    return true;
            };
        }

        void jobStartedAt([[maybe_unused]] uint64_t tsc)
        {
            if constexpr (Benchmark)
            {
                strategy.jobStartedAt(tsc);
            }
        }

      private:
        ALPBOOK_COLD void tripCircuitBreaker(BookError e)
        {
            state = ContextState::Failed;
            strategy.onSystemHalt();
            book.reset();
        }

        ALPBOOK_COLD void beginRecovery()
        {
            book.reset();
            state = ContextState::Recovering;
            strategy.onRecoveryStart();
        }

        ALPBOOK_COLD void endRecovery()
        {
            state = ContextState::Normal;
            strategy.onSystemRestart();
        }
    };

    export template<typename Policy, typename S, typename SF, bool B = false>
        requires strategy::Strategy<S, Book<Policy, S>>
        && strategy::StrategyFactory<SF, S, Book<Policy, S>>
    class Sink
    {
        using Ctx = Context<Policy, S, SF, B>;

      public:
        template<typename Mapper>
            requires EnumerableMapper<Mapper, uint16_t>
        Sink(uint32_t coreIndex, Mapper const& mapper, SF strategyFactory)
            : strategyFactory_(std::move(strategyFactory))
            , storage_(std::make_unique<InternalStorage>())
        {
            contexts_.fill(nullptr);

            auto ids = mapper.getIDsForThread(coreIndex);
            for (auto id : ids)
            {
                contexts_[id] = storage_->allocator.template new_object<Ctx>(id, strategyFactory_);
            }
        }

        Sink(Sink&&) = default;
        Sink& operator=(Sink&&) = default;

        Sink(Sink const&) = delete;
        Sink& operator=(Sink const&) = delete;

        ~Sink()
        {
            if (!storage_)
            {
                return;
            }

            for (auto* ctx : contexts_)
            {
                if (ctx != nullptr)
                {
                    storage_->allocator.delete_object(ctx);
                }
            }
        }

        ALPBOOK_INLINE void onMessage(itch::ItchSlot<B> data)
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
        SF strategyFactory_;

        struct InternalStorage
        {
            std::pmr::unsynchronized_pool_resource pool_resource;
            std::pmr::polymorphic_allocator<Ctx> allocator;

            InternalStorage()
                : pool_resource(std::pmr::pool_options {.max_blocks_per_chunk = 1024,
                                                        .largest_required_pool_block = sizeof(Ctx)},
                                std::pmr::new_delete_resource())
                , allocator(&pool_resource)
            {
            }
        };

        std::unique_ptr<InternalStorage> storage_;
        std::array<Ctx*, 65536> contexts_ {};
    };
}  // namespace alpbook::nasdaq