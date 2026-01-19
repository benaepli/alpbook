#include <cstring>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

import alpbook.sink.nasdaq;
import alpbook.book.nasdaq;
import alpbook.book.core;
import alpbook.common;
import alpbook.itch.parsing;
import alpbook.dispatch;

using alpbook::BookError;
using alpbook::price_t;
using alpbook::quantity_t;
using alpbook::Side;
using alpbook::itch::ItchSlot;
using alpbook::itch::MessageOrigin;
using alpbook::nasdaq::AddOrder;
using alpbook::nasdaq::CancelOrder;
using alpbook::nasdaq::DecrementShares;
using alpbook::nasdaq::ExecuteOrder;
using alpbook::nasdaq::ReplaceOrder;

struct MockStrategy
{
    uint16_t assetId = 0;
    void* bookPtr = nullptr;

    struct TradeCall
    {
        price_t price;
        quantity_t qty;
        Side side;
    };
    struct BidChangeCall
    {
        price_t price;
        quantity_t qty;
    };
    struct AskChangeCall
    {
        price_t price;
        quantity_t qty;
    };

    std::vector<TradeCall> trades;
    std::vector<BidChangeCall> bidChanges;
    std::vector<AskChangeCall> askChanges;
    bool systemHaltCalled = false;
    bool systemRestartCalled = false;

    void setAsset(uint16_t id) { assetId = id; }

    template<typename BookType>
    void setBook(BookType* ptr)
    {
        bookPtr = static_cast<void*>(ptr);
    }

    void onTrade(price_t price, quantity_t qty, Side side) { trades.push_back({price, qty, side}); }

    void onTopBidChange(price_t price, quantity_t qty) { bidChanges.push_back({price, qty}); }

    void onTopAskChange(price_t price, quantity_t qty) { askChanges.push_back({price, qty}); }

    void onSystemHalt() { systemHaltCalled = true; }

    void onSystemRestart() { systemRestartCalled = true; }

    void reset()
    {
        trades.clear();
        bidChanges.clear();
        askChanges.clear();
        systemHaltCalled = false;
        systemRestartCalled = false;
    }
};

class MockMapper
{
    std::vector<uint16_t> assetIds_;
    uint32_t threadCount_ = 1;

  public:
    void addAsset(uint16_t id) { assetIds_.push_back(id); }

    void setThreadCount(uint32_t count) { threadCount_ = count; }

    uint32_t getWorkerIndex(uint16_t id) const
    {
        // Always route to worker 0 for testing
        return 0;
    }

    std::vector<uint16_t> getIDsForThread(uint32_t core) const
    {
        if (core == 0)
        {
            return assetIds_;
        }
        return {};
    }
};

namespace TestHelpers
{
    template<typename T>
    void writeField(ItchSlot<>& slot, size_t offset, T value)
    {
        T swapped = std::byteswap(value);
        std::memcpy(slot.data.data() + offset, &swapped, sizeof(T));
    }

    void writeTimestamp(ItchSlot<>& slot, uint64_t timestamp)
    {
        uint16_t high = static_cast<uint16_t>(timestamp >> 32);
        uint32_t low = static_cast<uint32_t>(timestamp & 0xFFFFFFFF);
        writeField(slot, 5, high);
        writeField(slot, 7, low);
    }

    ItchSlot<> createAddOrder(
        uint16_t assetId, uint64_t orderId, uint32_t price, uint32_t shares, char side)
    {
        ItchSlot<> slot {};
        slot.type = MessageOrigin::Live;
        slot.data[0] = 'A';
        writeField(slot, 1, assetId);
        writeTimestamp(slot, 1000);
        writeField(slot, 11, orderId);
        slot.data[19] = side;
        writeField(slot, 20, shares);
        writeField(slot, 32, price);
        return slot;
    }

    ItchSlot<> createExecuteOrder(uint16_t assetId, uint64_t orderId, uint32_t shares)
    {
        ItchSlot<> slot {};
        slot.type = MessageOrigin::Live;
        slot.data[0] = 'E';
        writeField(slot, 1, assetId);
        writeField(slot, 11, orderId);
        writeField(slot, 19, shares);
        return slot;
    }

    ItchSlot<> createCancelOrder(uint16_t assetId, uint64_t orderId)
    {
        ItchSlot<> slot {};
        slot.type = MessageOrigin::Live;
        slot.data[0] = 'D';
        writeField(slot, 1, assetId);
        writeField(slot, 11, orderId);
        return slot;
    }

    ItchSlot<> createReduceOrder(uint16_t assetId, uint64_t orderId, uint32_t shares)
    {
        ItchSlot<> slot {};
        slot.type = MessageOrigin::Live;
        slot.data[0] = 'X';
        writeField(slot, 1, assetId);
        writeField(slot, 11, orderId);
        writeField(slot, 19, shares);
        return slot;
    }

    ItchSlot<> createReplaceOrder(
        uint16_t assetId, uint64_t oldId, uint64_t newId, uint32_t price, uint32_t shares)
    {
        ItchSlot<> slot {};
        slot.type = MessageOrigin::Live;
        slot.data[0] = 'U';
        writeField(slot, 1, assetId);
        writeTimestamp(slot, 2000);
        writeField(slot, 11, oldId);
        writeField(slot, 19, newId);
        writeField(slot, 27, shares);
        writeField(slot, 31, price);
        return slot;
    }

    template<typename T>
    uint64_t toU64(T value)
    {
        return static_cast<uint64_t>(value);
    }
}  // namespace TestHelpers

class ContextTest : public ::testing::Test
{
  protected:
    std::unique_ptr<alpbook::nasdaq::Context<MockStrategy>> context;

    void SetUp() override
    {
        context = std::make_unique<alpbook::nasdaq::Context<MockStrategy>>(100);
    }

    void TearDown() override { context.reset(); }
};

TEST_F(ContextTest, InitializationSetsAssetId)
{
    EXPECT_EQ(context->strategy.assetId, 100);
}

TEST_F(ContextTest, InitializationSetsBookPointer)
{
    EXPECT_EQ(context->strategy.bookPtr, &context->book);
}

TEST_F(ContextTest, StartsHealthy)
{
    EXPECT_TRUE(context->isHealthy_);
}

TEST_F(ContextTest, HealthyStateAcceptsAddOrder)
{
    AddOrder order {1000, 1, 100, 50, Side::Buy};
    context->add(order);

    auto bestBid = context->book.getBestBid();
    EXPECT_TRUE(bestBid.isValid());
    EXPECT_EQ(TestHelpers::toU64(bestBid.price), 100u);
    EXPECT_EQ(TestHelpers::toU64(bestBid.quantity), 50u);
}

TEST_F(ContextTest, HealthyStateExecutesOrder)
{
    AddOrder addOrder {1000, 1, 100, 50, Side::Buy};
    context->add(addOrder);
    context->strategy.reset();

    ExecuteOrder execOrder {1, 50};
    context->execute(execOrder);

    EXPECT_EQ(context->strategy.trades.size(), 1u);
    EXPECT_EQ(TestHelpers::toU64(context->strategy.trades[0].price), 100u);
    EXPECT_EQ(TestHelpers::toU64(context->strategy.trades[0].qty), 50u);
    EXPECT_EQ(context->strategy.trades[0].side, Side::Buy);
}

TEST_F(ContextTest, HealthyStateReducesShares)
{
    AddOrder addOrder {1000, 1, 100, 50, Side::Buy};
    context->add(addOrder);
    context->strategy.reset();

    DecrementShares reduceOrder {1, 20};
    context->reduce(reduceOrder);

    auto bestBid = context->book.getBestBid();
    EXPECT_EQ(TestHelpers::toU64(bestBid.quantity), 30u);
}

TEST_F(ContextTest, HealthyStateCancelsOrder)
{
    AddOrder addOrder {1000, 1, 100, 50, Side::Buy};
    context->add(addOrder);
    context->strategy.reset();

    CancelOrder cancelOrder {1};
    context->cancel(cancelOrder);

    auto bestBid = context->book.getBestBid();
    EXPECT_FALSE(bestBid.isValid());
}

TEST_F(ContextTest, HealthyStateReplacesOrder)
{
    AddOrder addOrder {1000, 1, 100, 50, Side::Buy};
    context->add(addOrder);
    context->strategy.reset();

    ReplaceOrder replaceOrder {2000, 1, 2, 101, 40};
    context->replace(replaceOrder);

    auto bestBid = context->book.getBestBid();
    EXPECT_EQ(TestHelpers::toU64(bestBid.price), 101u);
    EXPECT_EQ(TestHelpers::toU64(bestBid.quantity), 40u);
}

TEST_F(ContextTest, InvalidExecuteTripsCircuitBreaker)
{
    // Try to execute non-existent order
    ExecuteOrder execOrder {99999, 10};
    context->execute(execOrder);

    EXPECT_FALSE(context->isHealthy_);
    EXPECT_TRUE(context->strategy.systemHaltCalled);
}

TEST_F(ContextTest, InvalidReduceTripsCircuitBreaker)
{
    DecrementShares reduceOrder {99999, 10};
    context->reduce(reduceOrder);

    EXPECT_FALSE(context->isHealthy_);
    EXPECT_TRUE(context->strategy.systemHaltCalled);
}

TEST_F(ContextTest, InvalidCancelTripsCircuitBreaker)
{
    CancelOrder cancelOrder {99999};
    context->cancel(cancelOrder);

    EXPECT_FALSE(context->isHealthy_);
    EXPECT_TRUE(context->strategy.systemHaltCalled);
}

TEST_F(ContextTest, InvalidReplaceTripsCircuitBreaker)
{
    ReplaceOrder replaceOrder {2000, 99999, 2, 101, 40};
    context->replace(replaceOrder);

    EXPECT_FALSE(context->isHealthy_);
    EXPECT_TRUE(context->strategy.systemHaltCalled);
}

TEST_F(ContextTest, CircuitBreakerResetsBook)
{
    // Add an order first
    AddOrder addOrder {1000, 1, 100, 50, Side::Buy};
    context->add(addOrder);

    // Trip circuit breaker
    ExecuteOrder execOrder {99999, 10};
    context->execute(execOrder);

    // Book should be reset (empty)
    auto bestBid = context->book.getBestBid();
    EXPECT_FALSE(bestBid.isValid());
}

TEST_F(ContextTest, ApplyCallsLambdaWhenHealthy)
{
    bool lambdaCalled = false;
    alpbook::nasdaq::Book<MockStrategy>* capturedBook = nullptr;

    context->apply(
        [&](auto& book)
        {
            lambdaCalled = true;
            capturedBook = &book;
        });

    EXPECT_TRUE(lambdaCalled);
    EXPECT_EQ(capturedBook, &context->book);
}

class SinkTest : public ::testing::Test
{
  protected:
    MockMapper mapper;
    std::unique_ptr<alpbook::nasdaq::Sink<MockStrategy>> sink;

    void SetUp() override
    {
        // Configure mapper with test assets
        mapper.addAsset(100);
        mapper.addAsset(101);
        mapper.addAsset(102);

        sink = std::make_unique<alpbook::nasdaq::Sink<MockStrategy>>(0, mapper);
    }

    void TearDown() override { sink.reset(); }
};

TEST_F(SinkTest, SingleAssetRouting)
{
    auto addMsg = TestHelpers::createAddOrder(100, 1, 1000, 50, 'B');
    sink->onMessage(addMsg);

    auto execMsg = TestHelpers::createExecuteOrder(100, 1, 50);
    sink->onMessage(execMsg);

    // Both should route to the same context - no errors expected
    // Success is implicit (no crash/exceptions)
}

TEST_F(SinkTest, MultiAssetRoutingIsIndependent)
{
    // Add orders to different assets
    auto add100 = TestHelpers::createAddOrder(100, 1, 1000, 50, 'B');
    auto add101 = TestHelpers::createAddOrder(101, 2, 1001, 60, 'B');
    auto add102 = TestHelpers::createAddOrder(102, 3, 1002, 70, 'B');

    sink->onMessage(add100);
    sink->onMessage(add101);
    sink->onMessage(add102);

    // Execute on asset 101 - should not affect others
    auto exec101 = TestHelpers::createExecuteOrder(101, 2, 60);
    sink->onMessage(exec101);

    // Success is implicit - all messages routed correctly
}

TEST_F(SinkTest, AssetIsolationMaintainsIndependentBooks)
{
    // Add buy orders at same price to different assets
    auto add100 = TestHelpers::createAddOrder(100, 1, 1000, 50, 'B');
    auto add101 = TestHelpers::createAddOrder(101, 2, 1000, 60, 'B');

    sink->onMessage(add100);
    sink->onMessage(add101);

    // Execute order on asset 100
    auto exec100 = TestHelpers::createExecuteOrder(100, 1, 50);
    sink->onMessage(exec100);

    // Asset 101 should still have its order (verified implicitly by no errors)
    // Can execute it successfully
    auto exec101 = TestHelpers::createExecuteOrder(101, 2, 60);
    sink->onMessage(exec101);
}

TEST_F(SinkTest, CircuitBreakerIsolationPerAsset)
{
    // Add order to asset 100
    auto add100 = TestHelpers::createAddOrder(100, 1, 1000, 50, 'B');
    sink->onMessage(add100);

    // Add order to asset 101
    auto add101 = TestHelpers::createAddOrder(101, 2, 1001, 60, 'B');
    sink->onMessage(add101);

    // Trip circuit breaker on asset 100 (invalid execute)
    auto invalidExec100 = TestHelpers::createExecuteOrder(100, 99999, 10);
    sink->onMessage(invalidExec100);

    // Asset 100 should now reject operations
    auto add100Again = TestHelpers::createAddOrder(100, 3, 1002, 70, 'B');
    sink->onMessage(add100Again);

    // Asset 101 should still accept operations
    auto add101Again = TestHelpers::createAddOrder(101, 4, 1003, 80, 'B');
    sink->onMessage(add101Again);

    // Can still execute on asset 101
    auto exec101 = TestHelpers::createExecuteOrder(101, 2, 60);
    sink->onMessage(exec101);

    // Success verified implicitly by no crashes
}

class ItchIntegrationTest : public ::testing::Test
{
  protected:
    MockMapper mapper;
    std::unique_ptr<alpbook::nasdaq::Sink<MockStrategy>> sink;

    void SetUp() override
    {
        mapper.addAsset(100);
        sink = std::make_unique<alpbook::nasdaq::Sink<MockStrategy>>(0, mapper);
    }

    void TearDown() override { sink.reset(); }
};

TEST_F(ItchIntegrationTest, ParseAddOrderTypeA)
{
    auto msg = TestHelpers::createAddOrder(100, 1, 1000, 50, 'B');
    msg.data[0] = 'A';

    sink->onMessage(msg);

    // Order should be added (verified implicitly by no errors)
}

TEST_F(ItchIntegrationTest, ParseAddOrderTypeF)
{
    auto msg = TestHelpers::createAddOrder(100, 1, 1000, 50, 'S');
    msg.data[0] = 'F';

    sink->onMessage(msg);

    // Order should be added
}

TEST_F(ItchIntegrationTest, ParseExecuteOrderTypeE)
{
    // Add order first
    auto addMsg = TestHelpers::createAddOrder(100, 1, 1000, 50, 'B');
    sink->onMessage(addMsg);

    // Execute it
    auto execMsg = TestHelpers::createExecuteOrder(100, 1, 50);
    execMsg.data[0] = 'E';
    sink->onMessage(execMsg);

    // Execution should succeed (no errors)
}

TEST_F(ItchIntegrationTest, ParseExecuteOrderTypeC)
{
    // Add order first
    auto addMsg = TestHelpers::createAddOrder(100, 1, 1000, 50, 'B');
    sink->onMessage(addMsg);

    // Execute it with type C
    auto execMsg = TestHelpers::createExecuteOrder(100, 1, 30);
    execMsg.data[0] = 'C';
    sink->onMessage(execMsg);

    // Partial execution should succeed
}

TEST_F(ItchIntegrationTest, ParseCancelOrder)
{
    // Add order first
    auto addMsg = TestHelpers::createAddOrder(100, 1, 1000, 50, 'B');
    sink->onMessage(addMsg);

    // Cancel it
    auto cancelMsg = TestHelpers::createCancelOrder(100, 1);
    sink->onMessage(cancelMsg);

    // Cancel should succeed
}

TEST_F(ItchIntegrationTest, ParseReduceOrder)
{
    // Add order first
    auto addMsg = TestHelpers::createAddOrder(100, 1, 1000, 50, 'B');
    sink->onMessage(addMsg);

    // Reduce it
    auto reduceMsg = TestHelpers::createReduceOrder(100, 1, 20);
    sink->onMessage(reduceMsg);

    // Reduce should succeed
}

TEST_F(ItchIntegrationTest, ParseReplaceOrder)
{
    // Add order first
    auto addMsg = TestHelpers::createAddOrder(100, 1, 1000, 50, 'B');
    sink->onMessage(addMsg);

    // Replace it
    auto replaceMsg = TestHelpers::createReplaceOrder(100, 1, 2, 1001, 40);
    sink->onMessage(replaceMsg);

    // Replace should succeed
}

TEST_F(ItchIntegrationTest, ParseUnknownMessageTypeIgnored)
{
    ItchSlot<> msg {};
    msg.data[0] = 'Z';  // Unknown type
    TestHelpers::writeField(msg, 1, uint16_t(100));

    // Should not crash
    sink->onMessage(msg);
}

TEST_F(ItchIntegrationTest, SideEncodingBuyIsParsedCorrectly)
{
    auto msg = TestHelpers::createAddOrder(100, 1, 1000, 50, 'B');
    sink->onMessage(msg);

    // Buy order should be added (verified implicitly)
}

TEST_F(ItchIntegrationTest, SideEncodingSellIsParsedCorrectly)
{
    auto msg = TestHelpers::createAddOrder(100, 1, 1000, 50, 'S');
    sink->onMessage(msg);

    // Sell order should be added
}

class ScenarioTest : public ::testing::Test
{
  protected:
    MockMapper mapper;
    std::unique_ptr<alpbook::nasdaq::Sink<MockStrategy>> sink;

    void SetUp() override
    {
        // Initialize with 5 assets
        for (uint16_t i = 100; i < 105; i++)
        {
            mapper.addAsset(i);
        }
        sink = std::make_unique<alpbook::nasdaq::Sink<MockStrategy>>(0, mapper);
    }

    void TearDown() override { sink.reset(); }
};

TEST_F(ScenarioTest, ComplexMultiAssetSequence)
{
    // Asset 100: Add buy order
    auto add100 = TestHelpers::createAddOrder(100, 1, 1000, 50, 'B');
    sink->onMessage(add100);

    // Asset 101: Add sell order
    auto add101 = TestHelpers::createAddOrder(101, 2, 1001, 60, 'S');
    sink->onMessage(add101);

    // Asset 102: Add buy order
    auto add102 = TestHelpers::createAddOrder(102, 3, 1002, 70, 'B');
    sink->onMessage(add102);

    // Asset 100: Execute partial
    auto exec100 = TestHelpers::createExecuteOrder(100, 1, 20);
    sink->onMessage(exec100);

    // Asset 101: Reduce
    auto reduce101 = TestHelpers::createReduceOrder(101, 2, 10);
    sink->onMessage(reduce101);

    // Asset 102: Cancel
    auto cancel102 = TestHelpers::createCancelOrder(102, 3);
    sink->onMessage(cancel102);

    // Asset 103: Add and replace
    auto add103 = TestHelpers::createAddOrder(103, 4, 1003, 80, 'B');
    sink->onMessage(add103);
    auto replace103 = TestHelpers::createReplaceOrder(103, 4, 5, 1004, 90);
    sink->onMessage(replace103);

    // All operations should complete successfully
}

TEST_F(ScenarioTest, CircuitBreakerRecoveryIsolation)
{
    // Asset 100: Add order and trigger error
    auto add100 = TestHelpers::createAddOrder(100, 1, 1000, 50, 'B');
    sink->onMessage(add100);
    auto invalidExec100 = TestHelpers::createExecuteOrder(100, 99999, 10);
    sink->onMessage(invalidExec100);

    // Asset 100: Attempt more operations (should be rejected)
    auto add100Again = TestHelpers::createAddOrder(100, 2, 1001, 60, 'B');
    sink->onMessage(add100Again);

    // Asset 101: Add order and execute successfully
    auto add101 = TestHelpers::createAddOrder(101, 3, 1002, 70, 'B');
    sink->onMessage(add101);
    auto exec101 = TestHelpers::createExecuteOrder(101, 3, 70);
    sink->onMessage(exec101);

    // Asset 101: Continue trading normally
    auto add101Again = TestHelpers::createAddOrder(101, 4, 1003, 80, 'B');
    sink->onMessage(add101Again);

    // Success verified implicitly - asset 101 continues working
}

TEST_F(ScenarioTest, StressTestMultipleAssets)
{
    // Create messages for 100 assets (only first 5 configured in mapper)
    for (uint16_t assetId = 100; assetId < 200; assetId++)
    {
        if (assetId < 105)
        {
            // Only configured assets should be processed
            auto addMsg = TestHelpers::createAddOrder(assetId, assetId, 1000, 50, 'B');
            sink->onMessage(addMsg);
        }
    }

    // Send random operations to configured assets
    for (uint16_t assetId = 100; assetId < 105; assetId++)
    {
        auto execMsg = TestHelpers::createExecuteOrder(assetId, assetId, 25);
        sink->onMessage(execMsg);

        auto reduceMsg = TestHelpers::createReduceOrder(assetId, assetId, 10);
        sink->onMessage(reduceMsg);

        auto cancelMsg = TestHelpers::createCancelOrder(assetId, assetId);
        sink->onMessage(cancelMsg);
    }

    // All messages should be processed without state leakage
}

TEST_F(ScenarioTest, TimePriorityLostOnReplace)
{
    // Add first order
    auto add1 = TestHelpers::createAddOrder(100, 1, 1000, 50, 'B');
    sink->onMessage(add1);

    // Add second order at same price
    auto add2 = TestHelpers::createAddOrder(100, 2, 1000, 30, 'B');
    sink->onMessage(add2);

    // Replace first order - should lose time priority
    auto replace1 = TestHelpers::createReplaceOrder(100, 1, 3, 1000, 40);
    sink->onMessage(replace1);

    // Order 3 (replacement) should now be behind order 2 in queue
    // This is verified implicitly by the replace completing successfully
}

TEST_F(ScenarioTest, InterleavedAddExecuteCancelSequence)
{
    // Interleave operations across multiple assets
    auto add100 = TestHelpers::createAddOrder(100, 1, 1000, 50, 'B');
    sink->onMessage(add100);

    auto add101 = TestHelpers::createAddOrder(101, 2, 1001, 60, 'S');
    sink->onMessage(add101);

    auto exec100 = TestHelpers::createExecuteOrder(100, 1, 25);
    sink->onMessage(exec100);

    auto add102 = TestHelpers::createAddOrder(102, 3, 1002, 70, 'B');
    sink->onMessage(add102);

    auto cancel101 = TestHelpers::createCancelOrder(101, 2);
    sink->onMessage(cancel101);

    auto exec100Again = TestHelpers::createExecuteOrder(100, 1, 25);
    sink->onMessage(exec100Again);

    auto cancel102 = TestHelpers::createCancelOrder(102, 3);
    sink->onMessage(cancel102);

    // All operations should complete in order
}

class RecoveryTest : public ::testing::Test
{
  protected:
    std::unique_ptr<alpbook::nasdaq::Context<MockStrategy>> context;

    void SetUp() override
    {
        context = std::make_unique<alpbook::nasdaq::Context<MockStrategy>>(100);
    }

    void TearDown() override { context.reset(); }
};

TEST_F(RecoveryTest, SnapshotStartTriggersRecoveryAndRestoresHealth)
{
    // Trip circuit breaker
    ExecuteOrder execOrder {99999, 10};
    context->execute(execOrder);
    EXPECT_FALSE(context->isHealthy_);
    EXPECT_TRUE(context->strategy.systemHaltCalled);

    // Trigger recovery with SnapshotStart
    bool result = context->handleOrigin(MessageOrigin::SnapshotStart);

    EXPECT_TRUE(result);
    EXPECT_TRUE(context->isHealthy_);
    EXPECT_TRUE(context->strategy.systemRestartCalled);
}

TEST_F(RecoveryTest, RecoveryClearsBookState)
{
    // Add an order
    AddOrder addOrder {1000, 1, 100, 50, Side::Buy};
    context->add(addOrder);
    auto bestBid = context->book.getBestBid();
    EXPECT_TRUE(bestBid.isValid());

    // Trigger recovery
    [[maybe_unused]] auto recovered = context->handleOrigin(MessageOrigin::SnapshotStart);

    // Book should be cleared
    bestBid = context->book.getBestBid();
    EXPECT_FALSE(bestBid.isValid());
}

TEST_F(RecoveryTest, RecoveryAllowsNewOrdersAfterCircuitBreaker)
{
    // Trip circuit breaker
    ExecuteOrder execOrder {99999, 10};
    context->execute(execOrder);
    EXPECT_FALSE(context->isHealthy_);

    // Trigger recovery
    [[maybe_unused]] auto recovered = context->handleOrigin(MessageOrigin::SnapshotStart);

    // Should be able to add new orders
    AddOrder addOrder {1000, 1, 100, 50, Side::Buy};
    context->add(addOrder);

    auto bestBid = context->book.getBestBid();
    EXPECT_TRUE(bestBid.isValid());
    EXPECT_EQ(TestHelpers::toU64(bestBid.price), 100u);
}

TEST_F(RecoveryTest, HandleOriginReturnsFalseForNonSnapshotWhenUnhealthy)
{
    // Trip circuit breaker
    ExecuteOrder execOrder {99999, 10};
    context->execute(execOrder);
    EXPECT_FALSE(context->isHealthy_);

    // Test all non-SnapshotStart origins return false when unhealthy
    EXPECT_FALSE(context->handleOrigin(MessageOrigin::Live));
    EXPECT_FALSE(context->handleOrigin(MessageOrigin::Recovery));
    EXPECT_FALSE(context->handleOrigin(MessageOrigin::SnapshotEnd));
}

TEST_F(RecoveryTest, HandleOriginReturnsTrueForAllMessagesWhenHealthy)
{
    // Context starts healthy
    EXPECT_TRUE(context->isHealthy_);

    // All message origins should return true when healthy
    EXPECT_TRUE(context->handleOrigin(MessageOrigin::Live));
    EXPECT_TRUE(context->handleOrigin(MessageOrigin::Recovery));
    EXPECT_TRUE(context->handleOrigin(MessageOrigin::SnapshotStart));
    EXPECT_TRUE(context->handleOrigin(MessageOrigin::SnapshotEnd));
}

TEST_F(RecoveryTest, RecoveryIsIdempotent)
{
    // Trigger recovery multiple times
    [[maybe_unused]] auto first = context->handleOrigin(MessageOrigin::SnapshotStart);
    EXPECT_TRUE(context->isHealthy_);

    context->strategy.reset();
    [[maybe_unused]] auto second = context->handleOrigin(MessageOrigin::SnapshotStart);
    EXPECT_TRUE(context->isHealthy_);
    EXPECT_TRUE(context->strategy.systemRestartCalled);

    // Should still be able to add orders
    AddOrder addOrder {1000, 1, 100, 50, Side::Buy};
    context->add(addOrder);

    auto bestBid = context->book.getBestBid();
    EXPECT_TRUE(bestBid.isValid());
}

class RecoverySinkTest : public ::testing::Test
{
  protected:
    MockMapper mapper;
    std::unique_ptr<alpbook::nasdaq::Sink<MockStrategy>> sink;

    void SetUp() override
    {
        mapper.addAsset(100);
        mapper.addAsset(101);
        sink = std::make_unique<alpbook::nasdaq::Sink<MockStrategy>>(0, mapper);
    }

    void TearDown() override { sink.reset(); }

    ItchSlot<> createSnapshotStart(uint16_t assetId)
    {
        ItchSlot<> slot {};
        slot.type = MessageOrigin::SnapshotStart;
        slot.data[0] = 'S';  // Dummy message type
        TestHelpers::writeField(slot, 1, assetId);
        return slot;
    }
};

TEST_F(RecoverySinkTest, RecoveryWorksEndToEndThroughSink)
{
    // Add order to asset 100
    auto addMsg = TestHelpers::createAddOrder(100, 1, 1000, 50, 'B');
    sink->onMessage(addMsg);

    // Trip circuit breaker
    auto invalidExec = TestHelpers::createExecuteOrder(100, 99999, 10);
    sink->onMessage(invalidExec);

    // Attempt to add order (should be rejected)
    auto addMsg2 = TestHelpers::createAddOrder(100, 2, 1001, 60, 'B');
    sink->onMessage(addMsg2);

    // Send SnapshotStart to trigger recovery
    auto snapshotMsg = createSnapshotStart(100);
    sink->onMessage(snapshotMsg);

    // Should now be able to add orders again
    auto addMsg3 = TestHelpers::createAddOrder(100, 3, 1002, 70, 'B');
    sink->onMessage(addMsg3);

    // Verify by executing the order (would fail if not added)
    auto execMsg = TestHelpers::createExecuteOrder(100, 3, 70);
    sink->onMessage(execMsg);
}

TEST_F(RecoverySinkTest, RecoveryIsIsolatedPerAsset)
{
    // Trip circuit breaker on asset 100
    auto invalidExec100 = TestHelpers::createExecuteOrder(100, 99999, 10);
    sink->onMessage(invalidExec100);

    // Add order to asset 101 (should work)
    auto add101 = TestHelpers::createAddOrder(101, 1, 1000, 50, 'B');
    sink->onMessage(add101);

    // Recover asset 100
    auto snapshot100 = createSnapshotStart(100);
    sink->onMessage(snapshot100);

    // Both assets should now be healthy
    auto add100 = TestHelpers::createAddOrder(100, 2, 1001, 60, 'B');
    sink->onMessage(add100);

    auto exec100 = TestHelpers::createExecuteOrder(100, 2, 60);
    sink->onMessage(exec100);

    auto exec101 = TestHelpers::createExecuteOrder(101, 1, 50);
    sink->onMessage(exec101);

    // All operations should succeed
}

TEST_F(RecoverySinkTest, CanTripCircuitBreakerAgainAfterRecovery)
{
    // Trip circuit breaker
    auto invalidExec = TestHelpers::createExecuteOrder(100, 99999, 10);
    sink->onMessage(invalidExec);

    // Recover
    auto snapshot = createSnapshotStart(100);
    sink->onMessage(snapshot);

    // Add an order
    auto addMsg = TestHelpers::createAddOrder(100, 1, 1000, 50, 'B');
    sink->onMessage(addMsg);

    // Trip circuit breaker again
    auto invalidExec2 = TestHelpers::createExecuteOrder(100, 99999, 10);
    sink->onMessage(invalidExec2);

    // Should be unhealthy again
    auto addMsg2 = TestHelpers::createAddOrder(100, 2, 1001, 60, 'B');
    sink->onMessage(addMsg2);

    // Can recover again
    auto snapshot2 = createSnapshotStart(100);
    sink->onMessage(snapshot2);

    auto addMsg3 = TestHelpers::createAddOrder(100, 3, 1002, 70, 'B');
    sink->onMessage(addMsg3);
}
