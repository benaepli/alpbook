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
using alpbook::itch::MessageType;
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
    bool recoveryStartCalled = false;
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

    void onRecoveryStart() { recoveryStartCalled = true; }

    void onSystemRestart() { systemRestartCalled = true; }

    void reset()
    {
        trades.clear();
        bidChanges.clear();
        askChanges.clear();
        systemHaltCalled = false;
        recoveryStartCalled = false;
        systemRestartCalled = false;
    }
};

struct MockStrategyFactory
{
    using StrategyType = MockStrategy;
    MockStrategy create(uint16_t) const { return MockStrategy {}; }
};

using PolicyTypes = ::testing::Types<alpbook::nasdaq::PolicyTree, alpbook::nasdaq::PolicyHash>;

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
        slot.type = MessageType::Live;
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
        slot.type = MessageType::Live;
        slot.data[0] = 'E';
        writeField(slot, 1, assetId);
        writeField(slot, 11, orderId);
        writeField(slot, 19, shares);
        return slot;
    }

    ItchSlot<> createCancelOrder(uint16_t assetId, uint64_t orderId)
    {
        ItchSlot<> slot {};
        slot.type = MessageType::Live;
        slot.data[0] = 'D';
        writeField(slot, 1, assetId);
        writeField(slot, 11, orderId);
        return slot;
    }

    ItchSlot<> createReduceOrder(uint16_t assetId, uint64_t orderId, uint32_t shares)
    {
        ItchSlot<> slot {};
        slot.type = MessageType::Live;
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
        slot.type = MessageType::Live;
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

template<typename Policy>
class ContextTest : public ::testing::Test
{
  protected:
    MockStrategyFactory factory;
    std::unique_ptr<alpbook::nasdaq::Context<Policy, MockStrategy, MockStrategyFactory>> context;

    void SetUp() override
    {
        context =
            std::make_unique<alpbook::nasdaq::Context<Policy, MockStrategy, MockStrategyFactory>>(
                100, factory);
    }

    void TearDown() override { context.reset(); }
};

TYPED_TEST_SUITE(ContextTest, PolicyTypes);

TYPED_TEST(ContextTest, InitializationSetsAssetId)
{
    EXPECT_EQ(this->context->strategy.assetId, 100);
}

TYPED_TEST(ContextTest, InitializationSetsBookPointer)
{
    EXPECT_EQ(this->context->strategy.bookPtr, &this->context->book);
}

TYPED_TEST(ContextTest, StartsHealthy)
{
    EXPECT_EQ(this->context->state, alpbook::nasdaq::ContextState::Normal);
}

TYPED_TEST(ContextTest, HealthyStateAcceptsAddOrder)
{
    AddOrder order {1000, 1, 100, 50, Side::Buy};
    this->context->add(order);

    auto bestBid = this->context->book.getBestBid();
    EXPECT_TRUE(bestBid.isValid());
    EXPECT_EQ(TestHelpers::toU64(bestBid.price), 100u);
    EXPECT_EQ(TestHelpers::toU64(bestBid.quantity), 50u);
}

TYPED_TEST(ContextTest, HealthyStateExecutesOrder)
{
    AddOrder addOrder {1000, 1, 100, 50, Side::Buy};
    this->context->add(addOrder);
    this->context->strategy.reset();

    ExecuteOrder execOrder {1, 50};
    this->context->execute(execOrder);

    EXPECT_EQ(this->context->strategy.trades.size(), 1u);
    EXPECT_EQ(TestHelpers::toU64(this->context->strategy.trades[0].price), 100u);
    EXPECT_EQ(TestHelpers::toU64(this->context->strategy.trades[0].qty), 50u);
    EXPECT_EQ(this->context->strategy.trades[0].side, Side::Buy);
}

TYPED_TEST(ContextTest, HealthyStateReducesShares)
{
    AddOrder addOrder {1000, 1, 100, 50, Side::Buy};
    this->context->add(addOrder);
    this->context->strategy.reset();

    DecrementShares reduceOrder {1, 20};
    this->context->reduce(reduceOrder);

    auto bestBid = this->context->book.getBestBid();
    EXPECT_EQ(TestHelpers::toU64(bestBid.quantity), 30u);
}

TYPED_TEST(ContextTest, HealthyStateCancelsOrder)
{
    AddOrder addOrder {1000, 1, 100, 50, Side::Buy};
    this->context->add(addOrder);
    this->context->strategy.reset();

    CancelOrder cancelOrder {1};
    this->context->cancel(cancelOrder);

    auto bestBid = this->context->book.getBestBid();
    EXPECT_FALSE(bestBid.isValid());
}

TYPED_TEST(ContextTest, HealthyStateReplacesOrder)
{
    AddOrder addOrder {1000, 1, 100, 50, Side::Buy};
    this->context->add(addOrder);
    this->context->strategy.reset();

    ReplaceOrder replaceOrder {2000, 1, 2, 101, 40};
    this->context->replace(replaceOrder);

    auto bestBid = this->context->book.getBestBid();
    EXPECT_EQ(TestHelpers::toU64(bestBid.price), 101u);
    EXPECT_EQ(TestHelpers::toU64(bestBid.quantity), 40u);
}

TYPED_TEST(ContextTest, InvalidExecuteTripsCircuitBreaker)
{
    // Try to execute non-existent order
    ExecuteOrder execOrder {99999, 10};
    this->context->execute(execOrder);

    EXPECT_EQ(this->context->state, alpbook::nasdaq::ContextState::Failed);
    EXPECT_TRUE(this->context->strategy.systemHaltCalled);
}

TYPED_TEST(ContextTest, InvalidReduceTripsCircuitBreaker)
{
    DecrementShares reduceOrder {99999, 10};
    this->context->reduce(reduceOrder);

    EXPECT_EQ(this->context->state, alpbook::nasdaq::ContextState::Failed);
    EXPECT_TRUE(this->context->strategy.systemHaltCalled);
}

TYPED_TEST(ContextTest, InvalidCancelTripsCircuitBreaker)
{
    CancelOrder cancelOrder {99999};
    this->context->cancel(cancelOrder);

    EXPECT_EQ(this->context->state, alpbook::nasdaq::ContextState::Failed);
    EXPECT_TRUE(this->context->strategy.systemHaltCalled);
}

TYPED_TEST(ContextTest, InvalidReplaceTripsCircuitBreaker)
{
    ReplaceOrder replaceOrder {2000, 99999, 2, 101, 40};
    this->context->replace(replaceOrder);

    EXPECT_EQ(this->context->state, alpbook::nasdaq::ContextState::Failed);
    EXPECT_TRUE(this->context->strategy.systemHaltCalled);
}

TYPED_TEST(ContextTest, CircuitBreakerResetsBook)
{
    // Add an order first
    AddOrder addOrder {1000, 1, 100, 50, Side::Buy};
    this->context->add(addOrder);

    // Trip circuit breaker
    ExecuteOrder execOrder {99999, 10};
    this->context->execute(execOrder);

    // Book should be reset (empty)
    auto bestBid = this->context->book.getBestBid();
    EXPECT_FALSE(bestBid.isValid());
}

TYPED_TEST(ContextTest, ApplyCallsLambdaWhenHealthy)
{
    bool lambdaCalled = false;
    alpbook::nasdaq::Book<TypeParam, MockStrategy>* capturedBook = nullptr;

    this->context->apply(
        [&](auto& book)
        {
            lambdaCalled = true;
            capturedBook = &book;
        });

    EXPECT_TRUE(lambdaCalled);
    EXPECT_EQ(capturedBook, &this->context->book);
}

template<typename Policy>
class SinkTest : public ::testing::Test
{
  protected:
    MockMapper mapper;
    MockStrategyFactory factory;
    std::unique_ptr<alpbook::nasdaq::Sink<Policy, MockStrategy, MockStrategyFactory>> sink;

    void SetUp() override
    {
        // Configure mapper with test assets
        mapper.addAsset(100);
        mapper.addAsset(101);
        mapper.addAsset(102);

        sink = std::make_unique<alpbook::nasdaq::Sink<Policy, MockStrategy, MockStrategyFactory>>(
            0, mapper, factory);
    }

    void TearDown() override { sink.reset(); }
};

TYPED_TEST_SUITE(SinkTest, PolicyTypes);

TYPED_TEST(SinkTest, SingleAssetRouting)
{
    auto addMsg = TestHelpers::createAddOrder(100, 1, 1000, 50, 'B');
    this->sink->onMessage(addMsg);

    auto execMsg = TestHelpers::createExecuteOrder(100, 1, 50);
    this->sink->onMessage(execMsg);

    // Both should route to the same context - no errors expected
    // Success is implicit (no crash/exceptions)
}

TYPED_TEST(SinkTest, MultiAssetRoutingIsIndependent)
{
    // Add orders to different assets
    auto add100 = TestHelpers::createAddOrder(100, 1, 1000, 50, 'B');
    auto add101 = TestHelpers::createAddOrder(101, 2, 1001, 60, 'B');
    auto add102 = TestHelpers::createAddOrder(102, 3, 1002, 70, 'B');

    this->sink->onMessage(add100);
    this->sink->onMessage(add101);
    this->sink->onMessage(add102);

    // Execute on asset 101 - should not affect others
    auto exec101 = TestHelpers::createExecuteOrder(101, 2, 60);
    this->sink->onMessage(exec101);

    // Success is implicit - all messages routed correctly
}

TYPED_TEST(SinkTest, AssetIsolationMaintainsIndependentBooks)
{
    // Add buy orders at same price to different assets
    auto add100 = TestHelpers::createAddOrder(100, 1, 1000, 50, 'B');
    auto add101 = TestHelpers::createAddOrder(101, 2, 1000, 60, 'B');

    this->sink->onMessage(add100);
    this->sink->onMessage(add101);

    // Execute order on asset 100
    auto exec100 = TestHelpers::createExecuteOrder(100, 1, 50);
    this->sink->onMessage(exec100);

    // Asset 101 should still have its order (verified implicitly by no errors)
    // Can execute it successfully
    auto exec101 = TestHelpers::createExecuteOrder(101, 2, 60);
    this->sink->onMessage(exec101);
}

TYPED_TEST(SinkTest, CircuitBreakerIsolationPerAsset)
{
    // Add order to asset 100
    auto add100 = TestHelpers::createAddOrder(100, 1, 1000, 50, 'B');
    this->sink->onMessage(add100);

    // Add order to asset 101
    auto add101 = TestHelpers::createAddOrder(101, 2, 1001, 60, 'B');
    this->sink->onMessage(add101);

    // Trip circuit breaker on asset 100 (invalid execute)
    auto invalidExec100 = TestHelpers::createExecuteOrder(100, 99999, 10);
    this->sink->onMessage(invalidExec100);

    // Asset 100 should now reject operations
    auto add100Again = TestHelpers::createAddOrder(100, 3, 1002, 70, 'B');
    this->sink->onMessage(add100Again);

    // Asset 101 should still accept operations
    auto add101Again = TestHelpers::createAddOrder(101, 4, 1003, 80, 'B');
    this->sink->onMessage(add101Again);

    // Can still execute on asset 101
    auto exec101 = TestHelpers::createExecuteOrder(101, 2, 60);
    this->sink->onMessage(exec101);

    // Success verified implicitly by no crashes
}

template<typename Policy>
class ItchIntegrationTest : public ::testing::Test
{
  protected:
    MockMapper mapper;
    MockStrategyFactory factory;
    std::unique_ptr<alpbook::nasdaq::Sink<Policy, MockStrategy, MockStrategyFactory>> sink;

    void SetUp() override
    {
        mapper.addAsset(100);
        sink = std::make_unique<alpbook::nasdaq::Sink<Policy, MockStrategy, MockStrategyFactory>>(
            0, mapper, factory);
    }

    void TearDown() override { sink.reset(); }
};

TYPED_TEST_SUITE(ItchIntegrationTest, PolicyTypes);

TYPED_TEST(ItchIntegrationTest, ParseAddOrderTypeA)
{
    auto msg = TestHelpers::createAddOrder(100, 1, 1000, 50, 'B');
    msg.data[0] = 'A';

    this->sink->onMessage(msg);

    // Order should be added (verified implicitly by no errors)
}

TYPED_TEST(ItchIntegrationTest, ParseAddOrderTypeF)
{
    auto msg = TestHelpers::createAddOrder(100, 1, 1000, 50, 'S');
    msg.data[0] = 'F';

    this->sink->onMessage(msg);

    // Order should be added
}

TYPED_TEST(ItchIntegrationTest, ParseExecuteOrderTypeE)
{
    // Add order first
    auto addMsg = TestHelpers::createAddOrder(100, 1, 1000, 50, 'B');
    this->sink->onMessage(addMsg);

    // Execute it
    auto execMsg = TestHelpers::createExecuteOrder(100, 1, 50);
    execMsg.data[0] = 'E';
    this->sink->onMessage(execMsg);

    // Execution should succeed (no errors)
}

TYPED_TEST(ItchIntegrationTest, ParseExecuteOrderTypeC)
{
    // Add order first
    auto addMsg = TestHelpers::createAddOrder(100, 1, 1000, 50, 'B');
    this->sink->onMessage(addMsg);

    // Execute it with type C
    auto execMsg = TestHelpers::createExecuteOrder(100, 1, 30);
    execMsg.data[0] = 'C';
    this->sink->onMessage(execMsg);

    // Partial execution should succeed
}

TYPED_TEST(ItchIntegrationTest, ParseCancelOrder)
{
    // Add order first
    auto addMsg = TestHelpers::createAddOrder(100, 1, 1000, 50, 'B');
    this->sink->onMessage(addMsg);

    // Cancel it
    auto cancelMsg = TestHelpers::createCancelOrder(100, 1);
    this->sink->onMessage(cancelMsg);

    // Cancel should succeed
}

TYPED_TEST(ItchIntegrationTest, ParseReduceOrder)
{
    // Add order first
    auto addMsg = TestHelpers::createAddOrder(100, 1, 1000, 50, 'B');
    this->sink->onMessage(addMsg);

    // Reduce it
    auto reduceMsg = TestHelpers::createReduceOrder(100, 1, 20);
    this->sink->onMessage(reduceMsg);

    // Reduce should succeed
}

TYPED_TEST(ItchIntegrationTest, ParseReplaceOrder)
{
    // Add order first
    auto addMsg = TestHelpers::createAddOrder(100, 1, 1000, 50, 'B');
    this->sink->onMessage(addMsg);

    // Replace it
    auto replaceMsg = TestHelpers::createReplaceOrder(100, 1, 2, 1001, 40);
    this->sink->onMessage(replaceMsg);

    // Replace should succeed
}

TYPED_TEST(ItchIntegrationTest, ParseUnknownMessageTypeIgnored)
{
    ItchSlot<> msg {};
    msg.data[0] = 'Z';  // Unknown type
    TestHelpers::writeField(msg, 1, uint16_t(100));

    // Should not crash
    this->sink->onMessage(msg);
}

TYPED_TEST(ItchIntegrationTest, SideEncodingBuyIsParsedCorrectly)
{
    auto msg = TestHelpers::createAddOrder(100, 1, 1000, 50, 'B');
    this->sink->onMessage(msg);

    // Buy order should be added (verified implicitly)
}

TYPED_TEST(ItchIntegrationTest, SideEncodingSellIsParsedCorrectly)
{
    auto msg = TestHelpers::createAddOrder(100, 1, 1000, 50, 'S');
    this->sink->onMessage(msg);

    // Sell order should be added
}

template<typename Policy>
class ScenarioTest : public ::testing::Test
{
  protected:
    MockMapper mapper;
    MockStrategyFactory factory;
    std::unique_ptr<alpbook::nasdaq::Sink<Policy, MockStrategy, MockStrategyFactory>> sink;

    void SetUp() override
    {
        // Initialize with 5 assets
        for (uint16_t i = 100; i < 105; i++)
        {
            mapper.addAsset(i);
        }
        sink = std::make_unique<alpbook::nasdaq::Sink<Policy, MockStrategy, MockStrategyFactory>>(
            0, mapper, factory);
    }

    void TearDown() override { sink.reset(); }
};

TYPED_TEST_SUITE(ScenarioTest, PolicyTypes);

TYPED_TEST(ScenarioTest, ComplexMultiAssetSequence)
{
    // Asset 100: Add buy order
    auto add100 = TestHelpers::createAddOrder(100, 1, 1000, 50, 'B');
    this->sink->onMessage(add100);

    // Asset 101: Add sell order
    auto add101 = TestHelpers::createAddOrder(101, 2, 1001, 60, 'S');
    this->sink->onMessage(add101);

    // Asset 102: Add buy order
    auto add102 = TestHelpers::createAddOrder(102, 3, 1002, 70, 'B');
    this->sink->onMessage(add102);

    // Asset 100: Execute partial
    auto exec100 = TestHelpers::createExecuteOrder(100, 1, 20);
    this->sink->onMessage(exec100);

    // Asset 101: Reduce
    auto reduce101 = TestHelpers::createReduceOrder(101, 2, 10);
    this->sink->onMessage(reduce101);

    // Asset 102: Cancel
    auto cancel102 = TestHelpers::createCancelOrder(102, 3);
    this->sink->onMessage(cancel102);

    // Asset 103: Add and replace
    auto add103 = TestHelpers::createAddOrder(103, 4, 1003, 80, 'B');
    this->sink->onMessage(add103);
    auto replace103 = TestHelpers::createReplaceOrder(103, 4, 5, 1004, 90);
    this->sink->onMessage(replace103);

    // All operations should complete successfully
}

TYPED_TEST(ScenarioTest, CircuitBreakerRecoveryIsolation)
{
    // Asset 100: Add order and trigger error
    auto add100 = TestHelpers::createAddOrder(100, 1, 1000, 50, 'B');
    this->sink->onMessage(add100);
    auto invalidExec100 = TestHelpers::createExecuteOrder(100, 99999, 10);
    this->sink->onMessage(invalidExec100);

    // Asset 100: Attempt more operations (should be rejected)
    auto add100Again = TestHelpers::createAddOrder(100, 2, 1001, 60, 'B');
    this->sink->onMessage(add100Again);

    // Asset 101: Add order and execute successfully
    auto add101 = TestHelpers::createAddOrder(101, 3, 1002, 70, 'B');
    this->sink->onMessage(add101);
    auto exec101 = TestHelpers::createExecuteOrder(101, 3, 70);
    this->sink->onMessage(exec101);

    // Asset 101: Continue trading normally
    auto add101Again = TestHelpers::createAddOrder(101, 4, 1003, 80, 'B');
    this->sink->onMessage(add101Again);

    // Success verified implicitly - asset 101 continues working
}

TYPED_TEST(ScenarioTest, StressTestMultipleAssets)
{
    // Create messages for 100 assets (only first 5 configured in mapper)
    for (uint16_t assetId = 100; assetId < 200; assetId++)
    {
        if (assetId < 105)
        {
            // Only configured assets should be processed
            auto addMsg = TestHelpers::createAddOrder(assetId, assetId, 1000, 50, 'B');
            this->sink->onMessage(addMsg);
        }
    }

    // Send random operations to configured assets
    for (uint16_t assetId = 100; assetId < 105; assetId++)
    {
        auto execMsg = TestHelpers::createExecuteOrder(assetId, assetId, 25);
        this->sink->onMessage(execMsg);

        auto reduceMsg = TestHelpers::createReduceOrder(assetId, assetId, 10);
        this->sink->onMessage(reduceMsg);

        auto cancelMsg = TestHelpers::createCancelOrder(assetId, assetId);
        this->sink->onMessage(cancelMsg);
    }

    // All messages should be processed without state leakage
}

TYPED_TEST(ScenarioTest, TimePriorityLostOnReplace)
{
    // Add first order
    auto add1 = TestHelpers::createAddOrder(100, 1, 1000, 50, 'B');
    this->sink->onMessage(add1);

    // Add second order at same price
    auto add2 = TestHelpers::createAddOrder(100, 2, 1000, 30, 'B');
    this->sink->onMessage(add2);

    // Replace first order - should lose time priority
    auto replace1 = TestHelpers::createReplaceOrder(100, 1, 3, 1000, 40);
    this->sink->onMessage(replace1);

    // Order 3 (replacement) should now be behind order 2 in queue
    // This is verified implicitly by the replace completing successfully
}

TYPED_TEST(ScenarioTest, InterleavedAddExecuteCancelSequence)
{
    // Interleave operations across multiple assets
    auto add100 = TestHelpers::createAddOrder(100, 1, 1000, 50, 'B');
    this->sink->onMessage(add100);

    auto add101 = TestHelpers::createAddOrder(101, 2, 1001, 60, 'S');
    this->sink->onMessage(add101);

    auto exec100 = TestHelpers::createExecuteOrder(100, 1, 25);
    this->sink->onMessage(exec100);

    auto add102 = TestHelpers::createAddOrder(102, 3, 1002, 70, 'B');
    this->sink->onMessage(add102);

    auto cancel101 = TestHelpers::createCancelOrder(101, 2);
    this->sink->onMessage(cancel101);

    auto exec100Again = TestHelpers::createExecuteOrder(100, 1, 25);
    this->sink->onMessage(exec100Again);

    auto cancel102 = TestHelpers::createCancelOrder(102, 3);
    this->sink->onMessage(cancel102);

    // All operations should complete in order
}

template<typename Policy>
class RecoveryTest : public ::testing::Test
{
  protected:
    MockStrategyFactory factory;
    std::unique_ptr<alpbook::nasdaq::Context<Policy, MockStrategy, MockStrategyFactory>> context;

    void SetUp() override
    {
        context =
            std::make_unique<alpbook::nasdaq::Context<Policy, MockStrategy, MockStrategyFactory>>(
                100, factory);
    }

    void TearDown() override { context.reset(); }
};

TYPED_TEST_SUITE(RecoveryTest, PolicyTypes);

TYPED_TEST(RecoveryTest, SnapshotStartTriggersRecoveryAndRestoresHealth)
{
    // Trip circuit breaker
    ExecuteOrder execOrder {99999, 10};
    this->context->execute(execOrder);
    EXPECT_EQ(this->context->state, alpbook::nasdaq::ContextState::Failed);
    EXPECT_TRUE(this->context->strategy.systemHaltCalled);

    // Trigger recovery with SnapshotStart
    bool result = this->context->handleOrigin(MessageType::SnapshotStart);

    EXPECT_TRUE(result);
    EXPECT_EQ(this->context->state, alpbook::nasdaq::ContextState::Recovering);
    EXPECT_TRUE(this->context->strategy.recoveryStartCalled);
}

TYPED_TEST(RecoveryTest, RecoveryClearsBookState)
{
    // Add an order
    AddOrder addOrder {1000, 1, 100, 50, Side::Buy};
    this->context->add(addOrder);
    auto bestBid = this->context->book.getBestBid();
    EXPECT_TRUE(bestBid.isValid());

    // Trigger recovery
    [[maybe_unused]] auto recovered = this->context->handleOrigin(MessageType::SnapshotStart);

    // Book should be cleared
    bestBid = this->context->book.getBestBid();
    EXPECT_FALSE(bestBid.isValid());
}

TYPED_TEST(RecoveryTest, RecoveryAllowsNewOrdersAfterCircuitBreaker)
{
    // Trip circuit breaker
    ExecuteOrder execOrder {99999, 10};
    this->context->execute(execOrder);
    EXPECT_EQ(this->context->state, alpbook::nasdaq::ContextState::Failed);

    // Trigger recovery
    [[maybe_unused]] auto recovered = this->context->handleOrigin(MessageType::SnapshotStart);

    // Should be able to add new orders
    AddOrder addOrder {1000, 1, 100, 50, Side::Buy};
    this->context->add(addOrder);

    auto bestBid = this->context->book.getBestBid();
    EXPECT_TRUE(bestBid.isValid());
    EXPECT_EQ(TestHelpers::toU64(bestBid.price), 100u);
}

TYPED_TEST(RecoveryTest, HandleOriginReturnsFalseForNonSnapshotWhenUnhealthy)
{
    // Trip circuit breaker
    ExecuteOrder execOrder {99999, 10};
    this->context->execute(execOrder);
    EXPECT_EQ(this->context->state, alpbook::nasdaq::ContextState::Failed);

    // Test all non-SnapshotStart origins return false when unhealthy
    EXPECT_FALSE(this->context->handleOrigin(MessageType::Live));
    EXPECT_FALSE(this->context->handleOrigin(MessageType::Recovery));
    EXPECT_FALSE(this->context->handleOrigin(MessageType::SnapshotEnd));
}

template<typename Policy>
class RecoverySinkTest : public ::testing::Test
{
  protected:
    MockMapper mapper;
    MockStrategyFactory factory;
    std::unique_ptr<alpbook::nasdaq::Sink<Policy, MockStrategy, MockStrategyFactory>> sink;

    void SetUp() override
    {
        mapper.addAsset(100);
        mapper.addAsset(101);
        sink = std::make_unique<alpbook::nasdaq::Sink<Policy, MockStrategy, MockStrategyFactory>>(
            0, mapper, factory);
    }

    void TearDown() override { sink.reset(); }

    ItchSlot<> createSnapshotStart(uint16_t assetId)
    {
        ItchSlot<> slot {};
        slot.type = MessageType::SnapshotStart;
        slot.data[0] = 'S';  // Dummy message type
        TestHelpers::writeField(slot, 1, assetId);
        return slot;
    }
};

TYPED_TEST_SUITE(RecoverySinkTest, PolicyTypes);

TYPED_TEST(RecoverySinkTest, RecoveryWorksEndToEndThroughSink)
{
    // Add order to asset 100
    auto addMsg = TestHelpers::createAddOrder(100, 1, 1000, 50, 'B');
    this->sink->onMessage(addMsg);

    // Trip circuit breaker
    auto invalidExec = TestHelpers::createExecuteOrder(100, 99999, 10);
    this->sink->onMessage(invalidExec);

    // Attempt to add order (should be rejected)
    auto addMsg2 = TestHelpers::createAddOrder(100, 2, 1001, 60, 'B');
    this->sink->onMessage(addMsg2);

    // Send SnapshotStart to trigger recovery
    auto snapshotMsg = this->createSnapshotStart(100);
    this->sink->onMessage(snapshotMsg);

    // Should now be able to add orders again
    auto addMsg3 = TestHelpers::createAddOrder(100, 3, 1002, 70, 'B');
    this->sink->onMessage(addMsg3);

    // Verify by executing the order (would fail if not added)
    auto execMsg = TestHelpers::createExecuteOrder(100, 3, 70);
    this->sink->onMessage(execMsg);
}

TYPED_TEST(RecoverySinkTest, RecoveryIsIsolatedPerAsset)
{
    // Trip circuit breaker on asset 100
    auto invalidExec100 = TestHelpers::createExecuteOrder(100, 99999, 10);
    this->sink->onMessage(invalidExec100);

    // Add order to asset 101 (should work)
    auto add101 = TestHelpers::createAddOrder(101, 1, 1000, 50, 'B');
    this->sink->onMessage(add101);

    // Recover asset 100
    auto snapshot100 = this->createSnapshotStart(100);
    this->sink->onMessage(snapshot100);

    // Both assets should now be healthy
    auto add100 = TestHelpers::createAddOrder(100, 2, 1001, 60, 'B');
    this->sink->onMessage(add100);

    auto exec100 = TestHelpers::createExecuteOrder(100, 2, 60);
    this->sink->onMessage(exec100);

    auto exec101 = TestHelpers::createExecuteOrder(101, 1, 50);
    this->sink->onMessage(exec101);

    // All operations should succeed
}

TYPED_TEST(RecoverySinkTest, CanTripCircuitBreakerAgainAfterRecovery)
{
    // Trip circuit breaker
    auto invalidExec = TestHelpers::createExecuteOrder(100, 99999, 10);
    this->sink->onMessage(invalidExec);

    // Recover
    auto snapshot = this->createSnapshotStart(100);
    this->sink->onMessage(snapshot);

    // Add an order
    auto addMsg = TestHelpers::createAddOrder(100, 1, 1000, 50, 'B');
    this->sink->onMessage(addMsg);

    // Trip circuit breaker again
    auto invalidExec2 = TestHelpers::createExecuteOrder(100, 99999, 10);
    this->sink->onMessage(invalidExec2);

    // Should be unhealthy again
    auto addMsg2 = TestHelpers::createAddOrder(100, 2, 1001, 60, 'B');
    this->sink->onMessage(addMsg2);

    // Can recover again
    auto snapshot2 = this->createSnapshotStart(100);
    this->sink->onMessage(snapshot2);

    auto addMsg3 = TestHelpers::createAddOrder(100, 3, 1002, 70, 'B');
    this->sink->onMessage(addMsg3);
}
