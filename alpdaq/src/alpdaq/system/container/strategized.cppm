module;

#include <algorithm>
#include <array>
#include <concepts>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <utility>
#include <vector>

export module alpdaq.system.container.strategized;

import alpbook.strategy;
import alpbook.book;
import alpbook.itch.messages;

namespace alpdaq::system::container
{
    using namespace alpbook;

    export template<typename F, typename S, typename B>
    concept StrategyFactory = std::copy_constructible<F> && requires(F factory, uint16_t assetId) {
        { factory.create(assetId) } -> std::same_as<S>;
        requires strategy::Strategy<S, B>;
    };

    export template<typename Policy, typename S, typename SF>
        requires strategy::Strategy<S, nasdaq::Book<Policy, S>>
        && StrategyFactory<SF, S, nasdaq::Book<Policy, S>>
    class Strategized
    {
        using BookType = nasdaq::Book<Policy, S>;

        struct Context
        {
            S strategy;
            BookType book;
            bool totalSuspend_ = true;
            bool exchangeHalt_ = false;
            bool integrityFailure_ = false;

            Context(SF& factory, uint16_t assetId)
                : strategy(factory.create(assetId))
                , book(strategy)
            {
                strategy.setAsset(assetId);
                strategy.setBook(&book);
            }

            Context(Context const&) = delete;
            Context& operator=(Context const&) = delete;
            Context(Context&&) = delete;
            Context& operator=(Context&&) = delete;

            bool isHalted() const { return totalSuspend_ || exchangeHalt_ || integrityFailure_; }

            void suspendTrading()
            {
                bool wasHalted = isHalted();
                totalSuspend_ = true;
                if (!wasHalted)
                {
                    strategy.onHalt();
                }
            }

            void resumeTrading()
            {
                totalSuspend_ = false;
                if (!isHalted())
                {
                    strategy.onResume();
                }
            }

            void onTradingAction(itch::TradingState state)
            {
                if (state == itch::TradingState::Halt)
                {
                    bool wasHalted = isHalted();
                    exchangeHalt_ = true;
                    if (!wasHalted)
                    {
                        strategy.onHalt();
                    }
                }
                else
                {
                    exchangeHalt_ = false;
                    if (!isHalted())
                    {
                        strategy.onResume();
                    }
                }
            }

            void tripCircuitBreaker()
            {
                bool wasHalted = isHalted();
                integrityFailure_ = true;
                if (!wasHalted)
                {
                    strategy.onHalt();
                }
            }

            void add(nasdaq::AddOrder msg) { book.add(msg); }

            void execute(nasdaq::ExecuteOrder msg)
            {
                auto result = book.execute(msg);
                if (!result) [[unlikely]]
                {
                    tripCircuitBreaker();
                }
            }

            void reduce(nasdaq::DecrementShares msg)
            {
                auto result = book.reduce(msg);
                if (!result) [[unlikely]]
                {
                    tripCircuitBreaker();
                }
            }

            void cancel(nasdaq::CancelOrder msg)
            {
                auto result = book.cancel(msg);
                if (!result) [[unlikely]]
                {
                    tripCircuitBreaker();
                }
            }

            void replace(nasdaq::ReplaceOrder msg)
            {
                auto result = book.replace(msg);
                if (!result) [[unlikely]]
                {
                    tripCircuitBreaker();
                }
            }
        };

        using Ctx = Context;

        struct InternalStorage
        {
            std::pmr::unsynchronized_pool_resource pool_resource;
            std::pmr::polymorphic_allocator<Ctx> allocator;

            InternalStorage()
                : pool_resource(std::pmr::pool_options {.max_blocks_per_chunk = 4,
                                                        .largest_required_pool_block = sizeof(Ctx)},
                                std::pmr::new_delete_resource())
                , allocator(&pool_resource)
            {
            }
        };

      public:
        Strategized(SF factory)
            : factory_(std::move(factory))
        {
        }

        ~Strategized() { clearAll(); }

        Strategized(Strategized&& other) noexcept
            : factory_(std::move(other.factory_))
            , subscribed_(std::move(other.subscribed_))
            , storage_(std::move(other.storage_))
            , contexts_(other.contexts_)
        {
            other.contexts_.fill(nullptr);
        }

        Strategized& operator=(Strategized&& other) noexcept
        {
            if (this != &other)
            {
                clearAll();
                factory_ = std::move(other.factory_);
                subscribed_ = std::move(other.subscribed_);
                storage_ = std::move(other.storage_);
                contexts_ = other.contexts_;
                other.contexts_.fill(nullptr);
            }
            return *this;
        }

        Strategized(Strategized const&) = delete;
        Strategized& operator=(Strategized const&) = delete;

        void init(std::vector<itch::StockTicker> const& tickers)
        {
            subscribed_ = tickers;
            storage_ = std::make_unique<InternalStorage>();
        }

        void onPreMarket() {}

        void onStockDirectory(uint16_t assetId, itch::StockTicker ticker)
        {
            if (std::ranges::find(subscribed_, ticker) == subscribed_.end())
                return;

            auto* ptr = storage_->allocator.allocate(1);
            std::construct_at(ptr, factory_, assetId);
            contexts_[assetId] = ptr;
        }

        void add(uint16_t assetId, nasdaq::AddOrder msg) { contexts_[assetId]->add(msg); }
        void execute(uint16_t assetId, nasdaq::ExecuteOrder msg)
        {
            contexts_[assetId]->execute(msg);
        }
        void reduce(uint16_t assetId, nasdaq::DecrementShares msg)
        {
            contexts_[assetId]->reduce(msg);
        }
        void cancel(uint16_t assetId, nasdaq::CancelOrder msg) { contexts_[assetId]->cancel(msg); }
        void replace(uint16_t assetId, nasdaq::ReplaceOrder msg)
        {
            contexts_[assetId]->replace(msg);
        }

        void suspendTrading()
        {
            for (auto* ctx : contexts_)
            {
                if (ctx)
                {
                    ctx->suspendTrading();
                }
            }
        }

        void resumeTrading()
        {
            for (auto* ctx : contexts_)
            {
                if (ctx)
                {
                    ctx->resumeTrading();
                }
            }
        }

        void onTradingAction(uint16_t assetId, itch::TradingState state)
        {
            contexts_[assetId]->onTradingAction(state);
        }

        void clearAll()
        {
            if (!storage_)
            {
                return;
            }

            for (auto*& ctx : contexts_)
            {
                if (ctx)
                {
                    ctx->suspendTrading();
                    std::destroy_at(ctx);
                    storage_->allocator.deallocate(ctx, 1);
                    ctx = nullptr;
                }
            }
        }

      private:
        SF factory_;
        std::vector<itch::StockTicker> subscribed_;
        std::unique_ptr<InternalStorage> storage_;
        std::array<Ctx*, 65536> contexts_ {};
    };
}  // namespace alpdaq::system::container
