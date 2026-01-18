#include <random>
#include <vector>

#include <benchmark/benchmark.h>

import alpbook.book.nasdaq;
import alpbook.book.core;
import alpbook.common;

using alpbook::price_t;
using alpbook::quantity_t;
using alpbook::Side;
using alpbook::nasdaq::AddOrder;
using alpbook::nasdaq::CancelOrder;
using alpbook::nasdaq::ExecuteOrder;
using alpbook::nasdaq::ReplaceOrder;

namespace
{
    struct NoOpListener
    {
        void onTopBidChange(price_t, quantity_t) {}
        void onTopAskChange(price_t, quantity_t) {}
        void onTrade(price_t, quantity_t, Side) {}
    };

    using BenchBook = alpbook::nasdaq::Book<NoOpListener, 20000>;

    class OrderIdGenerator
    {
        uint64_t currentId_ = 1;
        uint64_t currentTimestamp_ = 1000;

      public:
        AddOrder createBuy(uint64_t price, uint32_t shares)
        {
            return {currentTimestamp_++, currentId_++, price, shares, Side::Buy};
        }

        AddOrder createSell(uint64_t price, uint32_t shares)
        {
            return {currentTimestamp_++, currentId_++, price, shares, Side::Sell};
        }

        uint64_t lastId() const { return currentId_ - 1; }
        uint64_t peekNextId() const { return currentId_; }
    };

    struct OrderDataset
    {
        std::vector<AddOrder> orders;
        std::vector<uint64_t> orderIds;
    };

    OrderDataset generateOrders(size_t count, uint64_t seed = 42)
    {
        OrderIdGenerator orderGen;
        std::mt19937 rng(seed);
        std::uniform_int_distribution<uint64_t> priceDist(90, 110);
        std::uniform_int_distribution<uint32_t> qtyDist(10, 100);
        std::bernoulli_distribution sideDist(0.5);

        OrderDataset dataset;
        dataset.orders.reserve(count);
        dataset.orderIds.reserve(count);

        for (size_t i = 0; i < count; ++i)
        {
            if (sideDist(rng))
                dataset.orders.push_back(orderGen.createBuy(priceDist(rng), qtyDist(rng)));
            else
                dataset.orders.push_back(orderGen.createSell(priceDist(rng), qtyDist(rng)));
            dataset.orderIds.push_back(orderGen.lastId());
        }

        return dataset;
    }

    size_t getBatchSize(benchmark::State& state, size_t maxBatch = 1000)
    {
        size_t bookSize = static_cast<size_t>(state.range(0));
        return (bookSize < maxBatch) ? bookSize : maxBatch;
    }

    void BM_Add(benchmark::State& state)
    {
        auto bookSize = static_cast<size_t>(state.range(0));
        size_t batchSize = 1000;

        auto dataset = generateOrders(bookSize + batchSize);

        NoOpListener listener;
        BenchBook book(listener);

        while (state.KeepRunningBatch(batchSize))
        {
            state.PauseTiming();
            book.reset();
            for (size_t i = 0; i < bookSize; ++i)
            {
                book.add(dataset.orders[i]);
            }
            state.ResumeTiming();

            for (size_t i = 0; i < batchSize; ++i)
            {
                book.add(dataset.orders[bookSize + i]);
            }
        }
    }
    BENCHMARK(BM_Add)->RangeMultiplier(10)->Range(100, 10000);

    void BM_Execute(benchmark::State& state)
    {
        auto bookSize = static_cast<size_t>(state.range(0));
        size_t batchSize = getBatchSize(state);

        auto dataset = generateOrders(bookSize);

        // Prepare execution payloads
        std::vector<ExecuteOrder> execs;
        execs.reserve(batchSize);
        for (size_t i = 0; i < batchSize; ++i)
        {
            execs.push_back({dataset.orderIds[i], 10});
        }

        NoOpListener listener;
        BenchBook book(listener);

        while (state.KeepRunningBatch(batchSize))
        {
            state.PauseTiming();
            book.reset();

            for (auto const& order : dataset.orders)
            {
                book.add(order);
            }
            state.ResumeTiming();
            for (auto const& txn : execs)
            {
                book.execute(txn);
            }
        }
    }
    BENCHMARK(BM_Execute)->RangeMultiplier(10)->Range(100, 10000);

    void BM_Cancel(benchmark::State& state)
    {
        auto bookSize = static_cast<size_t>(state.range(0));
        size_t batchSize = getBatchSize(state);

        auto dataset = generateOrders(bookSize);

        std::vector<CancelOrder> cancels;
        cancels.reserve(batchSize);
        for (size_t i = 0; i < batchSize; ++i)
        {
            cancels.push_back({dataset.orderIds[i]});
        }

        NoOpListener listener;
        BenchBook book(listener);

        while (state.KeepRunningBatch(batchSize))
        {
            state.PauseTiming();
            book.reset();
            for (auto const& order : dataset.orders)
            {
                book.add(order);
            }
            state.ResumeTiming();

            for (auto const& txn : cancels)
            {
                book.cancel(txn);
            }
        }
    }
    BENCHMARK(BM_Cancel)->RangeMultiplier(10)->Range(10, 10000);

    void BM_Replace(benchmark::State& state)
    {
        NoOpListener listener;
        BenchBook book(listener);
        OrderIdGenerator orderGen;

        std::mt19937 rng(42);
        std::uniform_int_distribution<uint64_t> priceDist(90, 110);
        std::uniform_int_distribution<uint32_t> qtyDist(10, 100);

        auto bookSize = static_cast<size_t>(state.range(0));
        std::vector<uint64_t> orderIds;
        orderIds.reserve(bookSize);

        for (size_t i = 0; i < bookSize; ++i)
        {
            book.add(orderGen.createBuy(priceDist(rng), qtyDist(rng)));
            orderIds.push_back(orderGen.lastId());
        }

        size_t idx = 0;
        for (auto _ : state)
        {
            auto oldId = orderIds[idx % bookSize];
            auto newId = orderGen.peekNextId();
            book.replace(ReplaceOrder {1000, oldId, newId, priceDist(rng), qtyDist(rng)});
            orderIds[idx % bookSize] = newId;
            ++idx;
        }
    }
    BENCHMARK(BM_Replace)->RangeMultiplier(10)->Range(10, 10000);

    void BM_GetBestBid(benchmark::State& state)
    {
        NoOpListener listener;
        BenchBook book(listener);
        OrderIdGenerator orderGen;

        std::mt19937 rng(42);
        std::uniform_int_distribution<uint64_t> priceDist(90, 110);
        std::uniform_int_distribution<uint32_t> qtyDist(10, 100);

        auto bookSize = static_cast<size_t>(state.range(0));
        for (size_t i = 0; i < bookSize; ++i)
        {
            book.add(orderGen.createBuy(priceDist(rng), qtyDist(rng)));
        }

        for (auto _ : state)
        {
            benchmark::DoNotOptimize(book.getBestBid());
        }
    }
    BENCHMARK(BM_GetBestBid)->RangeMultiplier(10)->Range(10, 100000);

    void BM_GetBidLevel(benchmark::State& state)
    {
        NoOpListener listener;
        alpbook::nasdaq::Book<NoOpListener> book(listener);
        OrderIdGenerator orderGen;

        std::mt19937 rng(42);
        std::uniform_int_distribution<uint64_t> priceDist(1, 1000);
        std::uniform_int_distribution<uint32_t> qtyDist(10, 100);

        auto bookSize = static_cast<size_t>(state.range(0));
        for (size_t i = 0; i < bookSize; ++i)
        {
            book.add(orderGen.createBuy(priceDist(rng), qtyDist(rng)));
        }

        uint32_t depth = 0;
        for (auto _ : state)
        {
            benchmark::DoNotOptimize(book.getBidLevel(depth % 10));
            ++depth;
        }
    }
    BENCHMARK(BM_GetBidLevel)->RangeMultiplier(10)->Range(10, 100000);

    void BM_GetBuyVolumeAhead(benchmark::State& state)
    {
        NoOpListener listener;
        alpbook::nasdaq::Book<NoOpListener> book(listener);
        OrderIdGenerator orderGen;

        std::mt19937 rng(42);
        std::uniform_int_distribution<uint64_t> priceDist(90, 110);
        std::uniform_int_distribution<uint32_t> qtyDist(10, 100);

        auto bookSize = static_cast<size_t>(state.range(0));
        for (size_t i = 0; i < bookSize; ++i)
        {
            book.add(orderGen.createBuy(priceDist(rng), qtyDist(rng)));
        }

        for (auto _ : state)
        {
            benchmark::DoNotOptimize(book.getBuyVolumeAhead(price_t(100)));
        }
    }
    BENCHMARK(BM_GetBuyVolumeAhead)->RangeMultiplier(10)->Range(10, 100000);

    void BM_GetBuyVolumeAheadByOrder(benchmark::State& state)
    {
        NoOpListener listener;
        BenchBook book(listener);
        OrderIdGenerator orderGen;

        std::mt19937 rng(42);
        std::uniform_int_distribution<uint64_t> priceDist(90, 110);
        std::uniform_int_distribution<uint32_t> qtyDist(10, 100);

        auto bookSize = static_cast<size_t>(state.range(0));
        std::vector<uint64_t> orderIds;
        orderIds.reserve(bookSize);

        for (size_t i = 0; i < bookSize; ++i)
        {
            book.add(orderGen.createBuy(priceDist(rng), qtyDist(rng)));
            orderIds.push_back(orderGen.lastId());
        }

        size_t idx = 0;
        for (auto _ : state)
        {
            benchmark::DoNotOptimize(book.getBuyVolumeAheadByOrder(orderIds[idx % bookSize]));
            ++idx;
        }
    }
    BENCHMARK(BM_GetBuyVolumeAheadByOrder)->RangeMultiplier(10)->Range(10, 100000);
}  // namespace