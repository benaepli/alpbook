#include <vector>

#include <gtest/gtest.h>

#include "absl/numeric/int128.h"

import alpbook.book.nasdaq;
import alpbook.book.core;
import alpbook.common;

using alpbook::BookError;
using alpbook::Level;
using alpbook::price_t;
using alpbook::quantity_t;
using alpbook::Side;
using alpbook::nasdaq::AddOrder;
using alpbook::nasdaq::CancelOrder;
using alpbook::nasdaq::DecrementShares;
using alpbook::nasdaq::ExecuteOrder;
using alpbook::nasdaq::ReplaceOrder;

struct MockListener
{
    struct BidChange
    {
        price_t price;
        quantity_t quantity;
    };
    struct AskChange
    {
        price_t price;
        quantity_t quantity;
    };
    struct Trade
    {
        price_t price;
        quantity_t quantity;
        Side side;
    };

    std::vector<BidChange> bidChanges;
    std::vector<AskChange> askChanges;
    std::vector<Trade> trades;

    void onTopBidChange(price_t price, quantity_t quantity)
    {
        bidChanges.push_back({price, quantity});
    }

    void onTopAskChange(price_t price, quantity_t quantity)
    {
        askChanges.push_back({price, quantity});
    }

    void onTrade(price_t price, quantity_t quantity, Side side)
    {
        trades.push_back({price, quantity, side});
    }

    void reset()
    {
        bidChanges.clear();
        askChanges.clear();
        trades.clear();
    }

    size_t bidChangeCount() const { return bidChanges.size(); }
    size_t askChangeCount() const { return askChanges.size(); }
    size_t tradeCount() const { return trades.size(); }
};

namespace TestHelpers
{
    inline price_t makePrice(uint64_t p)
    {
        return price_t(p);
    }
    inline quantity_t makeQuantity(uint64_t q)
    {
        return quantity_t(q);
    }

    // Helper to compare uint128 values (works for values that fit in uint64_t)
    template<typename T>
    inline uint64_t toU64(T value)
    {
        return static_cast<uint64_t>(value);
    }

    void assertLevel(Level const& level, uint64_t expectedPrice, uint64_t expectedQty)
    {
        EXPECT_EQ(toU64(level.price), expectedPrice);
        EXPECT_EQ(toU64(level.quantity), expectedQty);
        EXPECT_TRUE(level.isValid());
    }

    void assertEmptyLevel(Level const& level)
    {
        EXPECT_EQ(toU64(level.quantity), 0u);
        EXPECT_FALSE(level.isValid());
    }

    void assertBidChanged(MockListener const& listener, size_t index, uint64_t price, uint64_t qty)
    {
        ASSERT_LT(index, listener.bidChanges.size());
        EXPECT_EQ(toU64(listener.bidChanges[index].price), price);
        EXPECT_EQ(toU64(listener.bidChanges[index].quantity), qty);
    }

    void assertAskChanged(MockListener const& listener, size_t index, uint64_t price, uint64_t qty)
    {
        ASSERT_LT(index, listener.askChanges.size());
        EXPECT_EQ(toU64(listener.askChanges[index].price), price);
        EXPECT_EQ(toU64(listener.askChanges[index].quantity), qty);
    }

    void assertTrade(
        MockListener const& listener, size_t index, uint64_t price, uint64_t qty, Side side)
    {
        ASSERT_LT(index, listener.trades.size());
        EXPECT_EQ(toU64(listener.trades[index].price), price);
        EXPECT_EQ(toU64(listener.trades[index].quantity), qty);
        EXPECT_EQ(listener.trades[index].side, side);
    }

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
}  // namespace TestHelpers

class NasdaqBookTest : public ::testing::Test
{
  protected:
    MockListener listener;
    std::unique_ptr<alpbook::nasdaq::Book<MockListener>> book;
    TestHelpers::OrderIdGenerator orderGen;

    void SetUp() override
    {
        book = std::make_unique<alpbook::nasdaq::Book<MockListener>>(listener);
    }

    void TearDown() override { book.reset(); }

    void addBuy(uint64_t price, uint32_t shares) { book->add(orderGen.createBuy(price, shares)); }

    void addSell(uint64_t price, uint32_t shares) { book->add(orderGen.createSell(price, shares)); }
};

TEST_F(NasdaqBookTest, EmptyBookInitialization)
{
    auto bestBid = book->getBestBid();
    auto bestAsk = book->getBestAsk();

    TestHelpers::assertEmptyLevel(bestBid);
    TestHelpers::assertEmptyLevel(bestAsk);
    EXPECT_EQ(listener.bidChangeCount(), 0);
    EXPECT_EQ(listener.askChangeCount(), 0);
}

TEST_F(NasdaqBookTest, AddSingleBuyOrder)
{
    addBuy(100, 50);

    auto bestBid = book->getBestBid();
    TestHelpers::assertLevel(bestBid, 100, 50);

    EXPECT_EQ(listener.bidChangeCount(), 1);
    TestHelpers::assertBidChanged(listener, 0, 100, 50);
}

TEST_F(NasdaqBookTest, AddSingleSellOrder)
{
    addSell(101, 60);

    auto bestAsk = book->getBestAsk();
    TestHelpers::assertLevel(bestAsk, 101, 60);

    EXPECT_EQ(listener.askChangeCount(), 1);
    TestHelpers::assertAskChanged(listener, 0, 101, 60);
}

TEST_F(NasdaqBookTest, AddMultipleOrdersAtSamePrice)
{
    addBuy(100, 50);
    listener.reset();

    addBuy(100, 30);

    auto bestBid = book->getBestBid();
    TestHelpers::assertLevel(bestBid, 100, 80);  // 50 + 30

    EXPECT_EQ(listener.bidChangeCount(), 1);
    TestHelpers::assertBidChanged(listener, 0, 100, 80);
}

TEST_F(NasdaqBookTest, AddOrdersAtMultiplePriceLevels)
{
    addBuy(100, 50);
    addBuy(99, 40);
    addBuy(101, 30);

    auto bestBid = book->getBestBid();
    TestHelpers::assertLevel(bestBid, 101, 30);  // Best bid is highest price

    addSell(105, 20);
    addSell(106, 25);
    addSell(104, 15);

    auto bestAsk = book->getBestAsk();
    TestHelpers::assertLevel(bestAsk, 104, 15);  // Best ask is lowest price
}

TEST_F(NasdaqBookTest, ExecuteFullOrder)
{
    addBuy(100, 50);
    uint64_t orderId = orderGen.lastId();

    listener.reset();
    auto result = book->execute(ExecuteOrder {orderId, 50});

    ASSERT_TRUE(result.has_value());

    // Order should be removed from book
    auto bestBid = book->getBestBid();
    TestHelpers::assertEmptyLevel(bestBid);

    // Should trigger trade and top bid change
    EXPECT_EQ(listener.tradeCount(), 1);
    TestHelpers::assertTrade(listener, 0, 100, 50, Side::Buy);

    EXPECT_EQ(listener.bidChangeCount(), 1);
    TestHelpers::assertBidChanged(listener, 0, 0, 0);
}

TEST_F(NasdaqBookTest, ExecutePartialOrder)
{
    addBuy(100, 50);
    uint64_t orderId = orderGen.lastId();

    listener.reset();
    auto result = book->execute(ExecuteOrder {orderId, 20});

    ASSERT_TRUE(result.has_value());

    // Order should have 30 shares remaining
    auto bestBid = book->getBestBid();
    TestHelpers::assertLevel(bestBid, 100, 30);

    // Should trigger trade and top bid change
    EXPECT_EQ(listener.tradeCount(), 1);
    TestHelpers::assertTrade(listener, 0, 100, 20, Side::Buy);

    EXPECT_EQ(listener.bidChangeCount(), 1);
    TestHelpers::assertBidChanged(listener, 0, 100, 30);
}

TEST_F(NasdaqBookTest, ExecuteInvalidOrderId)
{
    addBuy(100, 50);

    auto result = book->execute(ExecuteOrder {99999, 10});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), BookError::MissingId);
}

TEST_F(NasdaqBookTest, ExecuteUpdatesTopOfBook)
{
    addBuy(100, 50);
    addBuy(99, 40);

    uint64_t topOrderId = orderGen.lastId() - 1;  // First order at price 100

    listener.reset();
    auto result = book->execute(ExecuteOrder {topOrderId, 50});

    ASSERT_TRUE(result.has_value());

    // Best bid should now be at price 99
    auto bestBid = book->getBestBid();
    TestHelpers::assertLevel(bestBid, 99, 40);

    EXPECT_EQ(listener.bidChangeCount(), 1);
    TestHelpers::assertBidChanged(listener, 0, 99, 40);
}

TEST_F(NasdaqBookTest, CancelSingleOrder)
{
    addBuy(100, 50);
    uint64_t orderId = orderGen.lastId();

    listener.reset();
    auto result = book->cancel(CancelOrder {orderId});

    ASSERT_TRUE(result.has_value());

    auto bestBid = book->getBestBid();
    TestHelpers::assertEmptyLevel(bestBid);

    EXPECT_EQ(listener.bidChangeCount(), 1);
    TestHelpers::assertBidChanged(listener, 0, 0, 0);
}

TEST_F(NasdaqBookTest, CancelInvalidId)
{
    addBuy(100, 50);

    auto result = book->cancel(CancelOrder {99999});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), BookError::MissingId);
}

TEST_F(NasdaqBookTest, CancelUpdatesTopOfBook)
{
    addBuy(100, 50);
    uint64_t topOrderId = orderGen.lastId();
    addBuy(99, 40);

    listener.reset();
    auto result = book->cancel(CancelOrder {topOrderId});

    ASSERT_TRUE(result.has_value());

    auto bestBid = book->getBestBid();
    TestHelpers::assertLevel(bestBid, 99, 40);

    EXPECT_EQ(listener.bidChangeCount(), 1);
    TestHelpers::assertBidChanged(listener, 0, 99, 40);
}

TEST_F(NasdaqBookTest, OnTopBidChangeWhenFirstBuyAdded)
{
    addBuy(100, 50);

    EXPECT_EQ(listener.bidChangeCount(), 1);
    TestHelpers::assertBidChanged(listener, 0, 100, 50);
}

TEST_F(NasdaqBookTest, OnTopAskChangeWhenFirstSellAdded)
{
    addSell(101, 60);

    EXPECT_EQ(listener.askChangeCount(), 1);
    TestHelpers::assertAskChanged(listener, 0, 101, 60);
}

TEST_F(NasdaqBookTest, OnTopBidChangeWhenBetterBidPriceAdded)
{
    addBuy(100, 50);
    listener.reset();

    addBuy(101, 30);

    EXPECT_EQ(listener.bidChangeCount(), 1);
    TestHelpers::assertBidChanged(listener, 0, 101, 30);
}

TEST_F(NasdaqBookTest, OnTopBidChangeWhenSameBidPriceAdded)
{
    addBuy(100, 50);
    listener.reset();

    addBuy(100, 30);

    EXPECT_EQ(listener.bidChangeCount(), 1);
    TestHelpers::assertBidChanged(listener, 0, 100, 80);  // Quantity updated
}

TEST_F(NasdaqBookTest, OnTopBidChangeWhenLastOrderAtBestBidCanceled)
{
    addBuy(100, 50);
    uint64_t orderId = orderGen.lastId();
    addBuy(99, 40);

    listener.reset();
    book->cancel(CancelOrder {orderId});

    EXPECT_EQ(listener.bidChangeCount(), 1);
    TestHelpers::assertBidChanged(listener, 0, 99, 40);
}

TEST_F(NasdaqBookTest, OnTradeTriggeredOnExecute)
{
    addBuy(100, 50);
    uint64_t orderId = orderGen.lastId();

    listener.reset();
    book->execute(ExecuteOrder {orderId, 20});

    EXPECT_EQ(listener.tradeCount(), 1);
    TestHelpers::assertTrade(listener, 0, 100, 20, Side::Buy);
}

TEST_F(NasdaqBookTest, GetBestBidAskReturnsCorrectValues)
{
    addBuy(100, 50);
    addSell(101, 60);

    auto bestBid = book->getBestBid();
    auto bestAsk = book->getBestAsk();

    TestHelpers::assertLevel(bestBid, 100, 50);
    TestHelpers::assertLevel(bestAsk, 101, 60);
}

TEST_F(NasdaqBookTest, GetBestBidAskOnEmptyBook)
{
    auto bestBid = book->getBestBid();
    auto bestAsk = book->getBestAsk();

    TestHelpers::assertEmptyLevel(bestBid);
    TestHelpers::assertEmptyLevel(bestAsk);
}

TEST_F(NasdaqBookTest, ReducePartialOrder)
{
    addBuy(100, 50);
    uint64_t orderId = orderGen.lastId();

    listener.reset();
    auto result = book->reduce(DecrementShares {orderId, 20});

    ASSERT_TRUE(result.has_value());

    auto bestBid = book->getBestBid();
    TestHelpers::assertLevel(bestBid, 100, 30);

    // Reduce should NOT trigger trade notification
    EXPECT_EQ(listener.tradeCount(), 0);

    // But should update top of book
    EXPECT_EQ(listener.bidChangeCount(), 1);
    TestHelpers::assertBidChanged(listener, 0, 100, 30);
}

TEST_F(NasdaqBookTest, ReduceFullOrderBehavesLikeCancel)
{
    addBuy(100, 50);
    uint64_t orderId = orderGen.lastId();

    listener.reset();
    auto result = book->reduce(DecrementShares {orderId, 50});

    ASSERT_TRUE(result.has_value());

    auto bestBid = book->getBestBid();
    TestHelpers::assertEmptyLevel(bestBid);

    EXPECT_EQ(listener.tradeCount(), 0);
    EXPECT_EQ(listener.bidChangeCount(), 1);
}

TEST_F(NasdaqBookTest, ReduceWithInvalidId)
{
    addBuy(100, 50);

    auto result = book->reduce(DecrementShares {99999, 10});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), BookError::MissingId);
}

TEST_F(NasdaqBookTest, ReplaceAtSamePriceLosesTimePriority)
{
    addBuy(100, 50);
    uint64_t firstId = orderGen.lastId();
    addBuy(100, 30);
    uint64_t secondId = orderGen.lastId();

    // Replace first order - should move to end of queue
    listener.reset();
    uint64_t newId = orderGen.peekNextId();
    auto result = book->replace(ReplaceOrder {2000, firstId, newId, 100, 40});

    ASSERT_TRUE(result.has_value());

    // Total quantity should be 30 + 40 = 70
    auto bestBid = book->getBestBid();
    TestHelpers::assertLevel(bestBid, 100, 70);

    // Verify time priority: secondId should have 0 ahead, newId should have 30 ahead
    auto volumeAhead = book->getBuyVolumeAheadByOrder(newId);
    ASSERT_TRUE(volumeAhead.has_value());
    EXPECT_EQ(TestHelpers::toU64(volumeAhead.value()), 30u);
}

TEST_F(NasdaqBookTest, ReplaceAtDifferentPriceLevel)
{
    addBuy(100, 50);
    uint64_t orderId = orderGen.lastId();

    listener.reset();
    uint64_t newId = orderGen.peekNextId();
    auto result = book->replace(ReplaceOrder {2000, orderId, newId, 101, 40});

    ASSERT_TRUE(result.has_value());

    // Old price level should be gone
    auto oldBid = book->getBestBid();
    TestHelpers::assertLevel(oldBid, 101, 40);

    EXPECT_EQ(listener.bidChangeCount(), 2);  // Once for cancel, once for add
}

TEST_F(NasdaqBookTest, ReplaceWithInvalidId)
{
    addBuy(100, 50);

    auto result = book->replace(ReplaceOrder {2000, 99999, 5000, 101, 40});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), BookError::MissingId);
}

TEST_F(NasdaqBookTest, GetBuyVolumeAheadWithMultiplePriceLevels)
{
    addBuy(103, 50);
    addBuy(102, 40);
    addBuy(101, 30);
    addBuy(100, 20);

    auto volume = book->getBuyVolumeAhead(TestHelpers::makePrice(101));
    EXPECT_EQ(TestHelpers::toU64(volume), 90u);  // 50 + 40
}

TEST_F(NasdaqBookTest, GetSellVolumeAheadWithMultiplePriceLevels)
{
    addSell(100, 20);
    addSell(101, 30);
    addSell(102, 40);
    addSell(103, 50);

    auto volume = book->getSellVolumeAhead(TestHelpers::makePrice(101));
    EXPECT_EQ(TestHelpers::toU64(volume), 20u);  // Only 100 is better
}

TEST_F(NasdaqBookTest, GetBuyVolumeAheadByOrderVerifiesQueuePosition)
{
    addBuy(100, 50);
    addBuy(100, 30);
    uint64_t secondId = orderGen.lastId();
    addBuy(100, 20);
    uint64_t thirdId = orderGen.lastId();

    // Second order should have 50 ahead
    auto volumeAhead2 = book->getBuyVolumeAheadByOrder(secondId);
    ASSERT_TRUE(volumeAhead2.has_value());
    EXPECT_EQ(TestHelpers::toU64(volumeAhead2.value()), 50u);

    // Third order should have 50 + 30 = 80 ahead
    auto volumeAhead3 = book->getBuyVolumeAheadByOrder(thirdId);
    ASSERT_TRUE(volumeAhead3.has_value());
    EXPECT_EQ(TestHelpers::toU64(volumeAhead3.value()), 80u);
}

TEST_F(NasdaqBookTest, GetSellVolumeAheadByOrderVerifiesQueuePosition)
{
    addSell(100, 50);
    addSell(100, 30);
    uint64_t secondId = orderGen.lastId();

    auto volumeAhead = book->getSellVolumeAheadByOrder(secondId);
    ASSERT_TRUE(volumeAhead.has_value());
    EXPECT_EQ(TestHelpers::toU64(volumeAhead.value()), 50u);
}

TEST_F(NasdaqBookTest, VolumeQueryWithInvalidOrderId)
{
    addBuy(100, 50);

    auto result = book->getBuyVolumeAheadByOrder(99999);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), BookError::MissingId);
}

TEST_F(NasdaqBookTest, GetBidLevelAtVariousDepths)
{
    addBuy(103, 50);
    addBuy(102, 40);
    addBuy(101, 30);

    auto level0 = book->getBidLevel(0);
    TestHelpers::assertLevel(level0, 103, 50);

    auto level1 = book->getBidLevel(1);
    TestHelpers::assertLevel(level1, 102, 40);

    auto level2 = book->getBidLevel(2);
    TestHelpers::assertLevel(level2, 101, 30);
}

TEST_F(NasdaqBookTest, GetAskLevelAtVariousDepths)
{
    addSell(100, 20);
    addSell(101, 30);
    addSell(102, 40);

    auto level0 = book->getAskLevel(0);
    TestHelpers::assertLevel(level0, 100, 20);

    auto level1 = book->getAskLevel(1);
    TestHelpers::assertLevel(level1, 101, 30);

    auto level2 = book->getAskLevel(2);
    TestHelpers::assertLevel(level2, 102, 40);
}

TEST_F(NasdaqBookTest, GetLevelBeyondAvailableDepth)
{
    addBuy(100, 50);
    addBuy(99, 40);

    auto level = book->getBidLevel(5);
    TestHelpers::assertEmptyLevel(level);
}

TEST_F(NasdaqBookTest, CancelHeadOfQueue)
{
    addBuy(100, 50);
    uint64_t firstId = orderGen.lastId();
    addBuy(100, 30);
    addBuy(100, 20);

    listener.reset();
    auto result = book->cancel(CancelOrder {firstId});

    ASSERT_TRUE(result.has_value());

    // Total should be 30 + 20 = 50
    auto bestBid = book->getBestBid();
    TestHelpers::assertLevel(bestBid, 100, 50);
}

TEST_F(NasdaqBookTest, CancelTailOfQueue)
{
    addBuy(100, 50);
    addBuy(100, 30);
    addBuy(100, 20);
    uint64_t lastId = orderGen.lastId();

    listener.reset();
    auto result = book->cancel(CancelOrder {lastId});

    ASSERT_TRUE(result.has_value());

    // Total should be 50 + 30 = 80
    auto bestBid = book->getBestBid();
    TestHelpers::assertLevel(bestBid, 100, 80);
}

TEST_F(NasdaqBookTest, CancelMiddleOfQueue)
{
    addBuy(100, 50);
    addBuy(100, 30);
    uint64_t middleId = orderGen.lastId();
    addBuy(100, 20);

    listener.reset();
    auto result = book->cancel(CancelOrder {middleId});

    ASSERT_TRUE(result.has_value());

    // Total should be 50 + 20 = 70
    auto bestBid = book->getBestBid();
    TestHelpers::assertLevel(bestBid, 100, 70);
}

TEST_F(NasdaqBookTest, CancelLastOrderAtPriceLevel)
{
    addBuy(100, 50);
    uint64_t orderId = orderGen.lastId();

    listener.reset();
    auto result = book->cancel(CancelOrder {orderId});

    ASSERT_TRUE(result.has_value());

    // Price level should be removed
    auto bestBid = book->getBestBid();
    TestHelpers::assertEmptyLevel(bestBid);
}

TEST_F(NasdaqBookTest, EmptyBookQueries)
{
    auto volume = book->getBuyVolumeAhead(TestHelpers::makePrice(100));
    EXPECT_EQ(TestHelpers::toU64(volume), 0u);

    auto level = book->getBidLevel(0);
    TestHelpers::assertEmptyLevel(level);
}

TEST_F(NasdaqBookTest, VolumeAheadWithNoOrders)
{
    auto volume = book->getBuyVolumeAhead(TestHelpers::makePrice(100));
    EXPECT_EQ(TestHelpers::toU64(volume), 0u);

    volume = book->getSellVolumeAhead(TestHelpers::makePrice(100));
    EXPECT_EQ(TestHelpers::toU64(volume), 0u);
}

TEST_F(NasdaqBookTest, TimePriorityFIFO)
{
    addBuy(100, 50);
    uint64_t firstId = orderGen.lastId();
    addBuy(100, 30);
    uint64_t secondId = orderGen.lastId();
    addBuy(100, 20);
    uint64_t thirdId = orderGen.lastId();

    // Verify FIFO ordering
    auto volume1 = book->getBuyVolumeAheadByOrder(firstId);
    ASSERT_TRUE(volume1.has_value());
    EXPECT_EQ(TestHelpers::toU64(volume1.value()), 0u);

    auto volume2 = book->getBuyVolumeAheadByOrder(secondId);
    ASSERT_TRUE(volume2.has_value());
    EXPECT_EQ(TestHelpers::toU64(volume2.value()), 50u);

    auto volume3 = book->getBuyVolumeAheadByOrder(thirdId);
    ASSERT_TRUE(volume3.has_value());
    EXPECT_EQ(TestHelpers::toU64(volume3.value()), 80u);
}

TEST_F(NasdaqBookTest, PricePriorityBuyDescendingSellAscending)
{
    // Add buys at different prices
    addBuy(100, 50);
    addBuy(102, 40);
    addBuy(101, 30);

    // Best bid should be highest price
    auto bestBid = book->getBestBid();
    TestHelpers::assertLevel(bestBid, 102, 40);

    // Add sells at different prices
    addSell(105, 20);
    addSell(103, 30);
    addSell(104, 25);

    // Best ask should be lowest price
    auto bestAsk = book->getBestAsk();
    TestHelpers::assertLevel(bestAsk, 103, 30);
}

TEST_F(NasdaqBookTest, MultiplePriceLevelsOnBothSides)
{
    // Build a realistic book
    addBuy(100, 50);
    addBuy(99, 40);
    addBuy(98, 30);

    addSell(101, 60);
    addSell(102, 70);
    addSell(103, 80);

    auto bestBid = book->getBestBid();
    auto bestAsk = book->getBestAsk();

    TestHelpers::assertLevel(bestBid, 100, 50);
    TestHelpers::assertLevel(bestAsk, 101, 60);

    // Verify depth queries
    auto bid1 = book->getBidLevel(1);
    TestHelpers::assertLevel(bid1, 99, 40);

    auto ask1 = book->getAskLevel(1);
    TestHelpers::assertLevel(ask1, 102, 70);
}

TEST_F(NasdaqBookTest, InterleavedOperations)
{
    addBuy(100, 50);
    uint64_t id1 = orderGen.lastId();

    addSell(101, 60);

    addBuy(99, 40);
    uint64_t id2 = orderGen.lastId();

    // Execute partial
    book->execute(ExecuteOrder {id1, 20});

    // Cancel
    book->cancel(CancelOrder {id2});

    // Add more
    addBuy(101, 30);

    // Replace
    uint64_t newId = orderGen.peekNextId();
    book->replace(ReplaceOrder {3000, id1, newId, 100, 25});

    // Verify final state
    auto bestBid = book->getBestBid();
    TestHelpers::assertLevel(bestBid, 101, 30);

    auto bestAsk = book->getBestAsk();
    TestHelpers::assertLevel(bestAsk, 101, 60);
}

TEST_F(NasdaqBookTest, BuildAndTearDownScenario)
{
    // Build up book
    addBuy(100, 50);
    uint64_t b1 = orderGen.lastId();
    addBuy(99, 40);
    uint64_t b2 = orderGen.lastId();
    addSell(101, 60);
    uint64_t s1 = orderGen.lastId();
    addSell(102, 70);
    uint64_t s2 = orderGen.lastId();

    // Verify state
    TestHelpers::assertLevel(book->getBestBid(), 100, 50);
    TestHelpers::assertLevel(book->getBestAsk(), 101, 60);

    // Tear down
    book->cancel(CancelOrder {b1});
    TestHelpers::assertLevel(book->getBestBid(), 99, 40);

    book->cancel(CancelOrder {s1});
    TestHelpers::assertLevel(book->getBestAsk(), 102, 70);

    book->cancel(CancelOrder {b2});
    TestHelpers::assertEmptyLevel(book->getBestBid());

    book->cancel(CancelOrder {s2});
    TestHelpers::assertEmptyLevel(book->getBestAsk());
}
