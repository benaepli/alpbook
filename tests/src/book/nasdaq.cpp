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

using PolicyTypes = ::testing::Types<alpbook::nasdaq::PolicyTree, alpbook::nasdaq::PolicyHash>;

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

template<typename Policy>
class NasdaqBookTest : public ::testing::Test
{
  protected:
    MockListener listener;
    std::unique_ptr<alpbook::nasdaq::Book<Policy, MockListener>> book;
    TestHelpers::OrderIdGenerator orderGen;

    void SetUp() override
    {
        book = std::make_unique<alpbook::nasdaq::Book<Policy, MockListener>>(listener);
    }

    void TearDown() override { book.reset(); }

    void addBuy(uint64_t price, uint32_t shares)
    {
        this->book->add(this->orderGen.createBuy(price, shares));
    }

    void addSell(uint64_t price, uint32_t shares)
    {
        this->book->add(this->orderGen.createSell(price, shares));
    }
};

TYPED_TEST_SUITE(NasdaqBookTest, PolicyTypes);

TYPED_TEST(NasdaqBookTest, EmptyBookInitialization)
{
    auto bestBid = this->book->getBestBid();
    auto bestAsk = this->book->getBestAsk();

    TestHelpers::assertEmptyLevel(bestBid);
    TestHelpers::assertEmptyLevel(bestAsk);
    EXPECT_EQ(this->listener.bidChangeCount(), 0);
    EXPECT_EQ(this->listener.askChangeCount(), 0);
}

TYPED_TEST(NasdaqBookTest, AddSingleBuyOrder)
{
    this->addBuy(100, 50);

    auto bestBid = this->book->getBestBid();
    TestHelpers::assertLevel(bestBid, 100, 50);

    EXPECT_EQ(this->listener.bidChangeCount(), 1);
    TestHelpers::assertBidChanged(this->listener, 0, 100, 50);
}

TYPED_TEST(NasdaqBookTest, AddSingleSellOrder)
{
    this->addSell(101, 60);

    auto bestAsk = this->book->getBestAsk();
    TestHelpers::assertLevel(bestAsk, 101, 60);

    EXPECT_EQ(this->listener.askChangeCount(), 1);
    TestHelpers::assertAskChanged(this->listener, 0, 101, 60);
}

TYPED_TEST(NasdaqBookTest, AddMultipleOrdersAtSamePrice)
{
    this->addBuy(100, 50);
    this->listener.reset();

    this->addBuy(100, 30);

    auto bestBid = this->book->getBestBid();
    TestHelpers::assertLevel(bestBid, 100, 80);  // 50 + 30

    EXPECT_EQ(this->listener.bidChangeCount(), 1);
    TestHelpers::assertBidChanged(this->listener, 0, 100, 80);
}

TYPED_TEST(NasdaqBookTest, AddOrdersAtMultiplePriceLevels)
{
    this->addBuy(100, 50);
    this->addBuy(99, 40);
    this->addBuy(101, 30);

    auto bestBid = this->book->getBestBid();
    TestHelpers::assertLevel(bestBid, 101, 30);  // Best bid is highest price

    this->addSell(105, 20);
    this->addSell(106, 25);
    this->addSell(104, 15);

    auto bestAsk = this->book->getBestAsk();
    TestHelpers::assertLevel(bestAsk, 104, 15);  // Best ask is lowest price
}

TYPED_TEST(NasdaqBookTest, ExecuteFullOrder)
{
    this->addBuy(100, 50);
    uint64_t orderId = this->orderGen.lastId();

    this->listener.reset();
    auto result = this->book->execute(ExecuteOrder {orderId, 50});

    ASSERT_TRUE(result.has_value());

    // Order should be removed from book
    auto bestBid = this->book->getBestBid();
    TestHelpers::assertEmptyLevel(bestBid);

    // Should trigger trade and top bid change
    EXPECT_EQ(this->listener.tradeCount(), 1);
    TestHelpers::assertTrade(this->listener, 0, 100, 50, Side::Buy);

    EXPECT_EQ(this->listener.bidChangeCount(), 1);
    TestHelpers::assertBidChanged(this->listener, 0, 0, 0);
}

TYPED_TEST(NasdaqBookTest, ExecutePartialOrder)
{
    this->addBuy(100, 50);
    uint64_t orderId = this->orderGen.lastId();

    this->listener.reset();
    auto result = this->book->execute(ExecuteOrder {orderId, 20});

    ASSERT_TRUE(result.has_value());

    // Order should have 30 shares remaining
    auto bestBid = this->book->getBestBid();
    TestHelpers::assertLevel(bestBid, 100, 30);

    // Should trigger trade and top bid change
    EXPECT_EQ(this->listener.tradeCount(), 1);
    TestHelpers::assertTrade(this->listener, 0, 100, 20, Side::Buy);

    EXPECT_EQ(this->listener.bidChangeCount(), 1);
    TestHelpers::assertBidChanged(this->listener, 0, 100, 30);
}

TYPED_TEST(NasdaqBookTest, ExecuteInvalidOrderId)
{
    this->addBuy(100, 50);

    auto result = this->book->execute(ExecuteOrder {99999, 10});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), BookError::MissingId);
}

TYPED_TEST(NasdaqBookTest, ExecuteUpdatesTopOfBook)
{
    this->addBuy(100, 50);
    this->addBuy(99, 40);

    uint64_t topOrderId = this->orderGen.lastId() - 1;  // First order at price 100

    this->listener.reset();
    auto result = this->book->execute(ExecuteOrder {topOrderId, 50});

    ASSERT_TRUE(result.has_value());

    // Best bid should now be at price 99
    auto bestBid = this->book->getBestBid();
    TestHelpers::assertLevel(bestBid, 99, 40);

    EXPECT_EQ(this->listener.bidChangeCount(), 1);
    TestHelpers::assertBidChanged(this->listener, 0, 99, 40);
}

TYPED_TEST(NasdaqBookTest, CancelSingleOrder)
{
    this->addBuy(100, 50);
    uint64_t orderId = this->orderGen.lastId();

    this->listener.reset();
    auto result = this->book->cancel(CancelOrder {orderId});

    ASSERT_TRUE(result.has_value());

    auto bestBid = this->book->getBestBid();
    TestHelpers::assertEmptyLevel(bestBid);

    EXPECT_EQ(this->listener.bidChangeCount(), 1);
    TestHelpers::assertBidChanged(this->listener, 0, 0, 0);
}

TYPED_TEST(NasdaqBookTest, CancelInvalidId)
{
    this->addBuy(100, 50);

    auto result = this->book->cancel(CancelOrder {99999});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), BookError::MissingId);
}

TYPED_TEST(NasdaqBookTest, CancelUpdatesTopOfBook)
{
    this->addBuy(100, 50);
    uint64_t topOrderId = this->orderGen.lastId();
    this->addBuy(99, 40);

    this->listener.reset();
    auto result = this->book->cancel(CancelOrder {topOrderId});

    ASSERT_TRUE(result.has_value());

    auto bestBid = this->book->getBestBid();
    TestHelpers::assertLevel(bestBid, 99, 40);

    EXPECT_EQ(this->listener.bidChangeCount(), 1);
    TestHelpers::assertBidChanged(this->listener, 0, 99, 40);
}

TYPED_TEST(NasdaqBookTest, OnTopBidChangeWhenFirstBuyAdded)
{
    this->addBuy(100, 50);

    EXPECT_EQ(this->listener.bidChangeCount(), 1);
    TestHelpers::assertBidChanged(this->listener, 0, 100, 50);
}

TYPED_TEST(NasdaqBookTest, OnTopAskChangeWhenFirstSellAdded)
{
    this->addSell(101, 60);

    EXPECT_EQ(this->listener.askChangeCount(), 1);
    TestHelpers::assertAskChanged(this->listener, 0, 101, 60);
}

TYPED_TEST(NasdaqBookTest, OnTopBidChangeWhenBetterBidPriceAdded)
{
    this->addBuy(100, 50);
    this->listener.reset();

    this->addBuy(101, 30);

    EXPECT_EQ(this->listener.bidChangeCount(), 1);
    TestHelpers::assertBidChanged(this->listener, 0, 101, 30);
}

TYPED_TEST(NasdaqBookTest, OnTopBidChangeWhenSameBidPriceAdded)
{
    this->addBuy(100, 50);
    this->listener.reset();

    this->addBuy(100, 30);

    EXPECT_EQ(this->listener.bidChangeCount(), 1);
    TestHelpers::assertBidChanged(this->listener, 0, 100, 80);  // Quantity updated
}

TYPED_TEST(NasdaqBookTest, OnTopBidChangeWhenLastOrderAtBestBidCanceled)
{
    this->addBuy(100, 50);
    uint64_t orderId = this->orderGen.lastId();
    this->addBuy(99, 40);

    this->listener.reset();
    this->book->cancel(CancelOrder {orderId});

    EXPECT_EQ(this->listener.bidChangeCount(), 1);
    TestHelpers::assertBidChanged(this->listener, 0, 99, 40);
}

TYPED_TEST(NasdaqBookTest, OnTradeTriggeredOnExecute)
{
    this->addBuy(100, 50);
    uint64_t orderId = this->orderGen.lastId();

    this->listener.reset();
    this->book->execute(ExecuteOrder {orderId, 20});

    EXPECT_EQ(this->listener.tradeCount(), 1);
    TestHelpers::assertTrade(this->listener, 0, 100, 20, Side::Buy);
}

TYPED_TEST(NasdaqBookTest, GetBestBidAskReturnsCorrectValues)
{
    this->addBuy(100, 50);
    this->addSell(101, 60);

    auto bestBid = this->book->getBestBid();
    auto bestAsk = this->book->getBestAsk();

    TestHelpers::assertLevel(bestBid, 100, 50);
    TestHelpers::assertLevel(bestAsk, 101, 60);
}

TYPED_TEST(NasdaqBookTest, GetBestBidAskOnEmptyBook)
{
    auto bestBid = this->book->getBestBid();
    auto bestAsk = this->book->getBestAsk();

    TestHelpers::assertEmptyLevel(bestBid);
    TestHelpers::assertEmptyLevel(bestAsk);
}

TYPED_TEST(NasdaqBookTest, ReducePartialOrder)
{
    this->addBuy(100, 50);
    uint64_t orderId = this->orderGen.lastId();

    this->listener.reset();
    auto result = this->book->reduce(DecrementShares {orderId, 20});

    ASSERT_TRUE(result.has_value());

    auto bestBid = this->book->getBestBid();
    TestHelpers::assertLevel(bestBid, 100, 30);

    // Reduce should NOT trigger trade notification
    EXPECT_EQ(this->listener.tradeCount(), 0);

    // But should update top of book
    EXPECT_EQ(this->listener.bidChangeCount(), 1);
    TestHelpers::assertBidChanged(this->listener, 0, 100, 30);
}

TYPED_TEST(NasdaqBookTest, ReduceFullOrderBehavesLikeCancel)
{
    this->addBuy(100, 50);
    uint64_t orderId = this->orderGen.lastId();

    this->listener.reset();
    auto result = this->book->reduce(DecrementShares {orderId, 50});

    ASSERT_TRUE(result.has_value());

    auto bestBid = this->book->getBestBid();
    TestHelpers::assertEmptyLevel(bestBid);

    EXPECT_EQ(this->listener.tradeCount(), 0);
    EXPECT_EQ(this->listener.bidChangeCount(), 1);
}

TYPED_TEST(NasdaqBookTest, ReduceWithInvalidId)
{
    this->addBuy(100, 50);

    auto result = this->book->reduce(DecrementShares {99999, 10});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), BookError::MissingId);
}

TYPED_TEST(NasdaqBookTest, ReplaceAtSamePriceLosesTimePriority)
{
    this->addBuy(100, 50);
    uint64_t firstId = this->orderGen.lastId();
    this->addBuy(100, 30);
    uint64_t secondId = this->orderGen.lastId();

    // Replace first order - should move to end of queue
    this->listener.reset();
    uint64_t newId = this->orderGen.peekNextId();
    auto result = this->book->replace(ReplaceOrder {2000, firstId, newId, 100, 40});

    ASSERT_TRUE(result.has_value());

    // Total quantity should be 30 + 40 = 70
    auto bestBid = this->book->getBestBid();
    TestHelpers::assertLevel(bestBid, 100, 70);

    // Verify time priority: secondId should have 0 ahead, newId should have 30 ahead
    auto volumeAhead = this->book->getBuyVolumeAheadByOrder(newId);
    ASSERT_TRUE(volumeAhead.has_value());
    EXPECT_EQ(TestHelpers::toU64(volumeAhead.value()), 30u);
}

TYPED_TEST(NasdaqBookTest, ReplaceAtDifferentPriceLevel)
{
    this->addBuy(100, 50);
    uint64_t orderId = this->orderGen.lastId();

    this->listener.reset();
    uint64_t newId = this->orderGen.peekNextId();
    auto result = this->book->replace(ReplaceOrder {2000, orderId, newId, 101, 40});

    ASSERT_TRUE(result.has_value());

    // Old price level should be gone
    auto oldBid = this->book->getBestBid();
    TestHelpers::assertLevel(oldBid, 101, 40);

    EXPECT_EQ(this->listener.bidChangeCount(), 2);  // Once for cancel, once for add
}

TYPED_TEST(NasdaqBookTest, ReplaceWithInvalidId)
{
    this->addBuy(100, 50);

    auto result = this->book->replace(ReplaceOrder {2000, 99999, 5000, 101, 40});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), BookError::MissingId);
}

TYPED_TEST(NasdaqBookTest, GetBuyVolumeAheadWithMultiplePriceLevels)
{
    this->addBuy(103, 50);
    this->addBuy(102, 40);
    this->addBuy(101, 30);
    this->addBuy(100, 20);

    auto volume = this->book->getBuyVolumeAhead(TestHelpers::makePrice(101));
    EXPECT_EQ(TestHelpers::toU64(volume), 90u);  // 50 + 40
}

TYPED_TEST(NasdaqBookTest, GetSellVolumeAheadWithMultiplePriceLevels)
{
    this->addSell(100, 20);
    this->addSell(101, 30);
    this->addSell(102, 40);
    this->addSell(103, 50);

    auto volume = this->book->getSellVolumeAhead(TestHelpers::makePrice(101));
    EXPECT_EQ(TestHelpers::toU64(volume), 20u);  // Only 100 is better
}

TYPED_TEST(NasdaqBookTest, GetBuyVolumeAheadByOrderVerifiesQueuePosition)
{
    this->addBuy(100, 50);
    this->addBuy(100, 30);
    uint64_t secondId = this->orderGen.lastId();
    this->addBuy(100, 20);
    uint64_t thirdId = this->orderGen.lastId();

    // Second order should have 50 ahead
    auto volumeAhead2 = this->book->getBuyVolumeAheadByOrder(secondId);
    ASSERT_TRUE(volumeAhead2.has_value());
    EXPECT_EQ(TestHelpers::toU64(volumeAhead2.value()), 50u);

    // Third order should have 50 + 30 = 80 ahead
    auto volumeAhead3 = this->book->getBuyVolumeAheadByOrder(thirdId);
    ASSERT_TRUE(volumeAhead3.has_value());
    EXPECT_EQ(TestHelpers::toU64(volumeAhead3.value()), 80u);
}

TYPED_TEST(NasdaqBookTest, GetSellVolumeAheadByOrderVerifiesQueuePosition)
{
    this->addSell(100, 50);
    this->addSell(100, 30);
    uint64_t secondId = this->orderGen.lastId();

    auto volumeAhead = this->book->getSellVolumeAheadByOrder(secondId);
    ASSERT_TRUE(volumeAhead.has_value());
    EXPECT_EQ(TestHelpers::toU64(volumeAhead.value()), 50u);
}

TYPED_TEST(NasdaqBookTest, VolumeQueryWithInvalidOrderId)
{
    this->addBuy(100, 50);

    auto result = this->book->getBuyVolumeAheadByOrder(99999);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), BookError::MissingId);
}

TYPED_TEST(NasdaqBookTest, GetBidLevelAtVariousDepths)
{
    this->addBuy(103, 50);
    this->addBuy(102, 40);
    this->addBuy(101, 30);

    auto level0 = this->book->getBidLevel(0);
    TestHelpers::assertLevel(level0, 103, 50);

    auto level1 = this->book->getBidLevel(1);
    TestHelpers::assertLevel(level1, 102, 40);

    auto level2 = this->book->getBidLevel(2);
    TestHelpers::assertLevel(level2, 101, 30);
}

TYPED_TEST(NasdaqBookTest, GetAskLevelAtVariousDepths)
{
    this->addSell(100, 20);
    this->addSell(101, 30);
    this->addSell(102, 40);

    auto level0 = this->book->getAskLevel(0);
    TestHelpers::assertLevel(level0, 100, 20);

    auto level1 = this->book->getAskLevel(1);
    TestHelpers::assertLevel(level1, 101, 30);

    auto level2 = this->book->getAskLevel(2);
    TestHelpers::assertLevel(level2, 102, 40);
}

TYPED_TEST(NasdaqBookTest, GetLevelBeyondAvailableDepth)
{
    this->addBuy(100, 50);
    this->addBuy(99, 40);

    auto level = this->book->getBidLevel(5);
    TestHelpers::assertEmptyLevel(level);
}

TYPED_TEST(NasdaqBookTest, CancelHeadOfQueue)
{
    this->addBuy(100, 50);
    uint64_t firstId = this->orderGen.lastId();
    this->addBuy(100, 30);
    this->addBuy(100, 20);

    this->listener.reset();
    auto result = this->book->cancel(CancelOrder {firstId});

    ASSERT_TRUE(result.has_value());

    // Total should be 30 + 20 = 50
    auto bestBid = this->book->getBestBid();
    TestHelpers::assertLevel(bestBid, 100, 50);
}

TYPED_TEST(NasdaqBookTest, CancelTailOfQueue)
{
    this->addBuy(100, 50);
    this->addBuy(100, 30);
    this->addBuy(100, 20);
    uint64_t lastId = this->orderGen.lastId();

    this->listener.reset();
    auto result = this->book->cancel(CancelOrder {lastId});

    ASSERT_TRUE(result.has_value());

    // Total should be 50 + 30 = 80
    auto bestBid = this->book->getBestBid();
    TestHelpers::assertLevel(bestBid, 100, 80);
}

TYPED_TEST(NasdaqBookTest, CancelMiddleOfQueue)
{
    this->addBuy(100, 50);
    this->addBuy(100, 30);
    uint64_t middleId = this->orderGen.lastId();
    this->addBuy(100, 20);

    this->listener.reset();
    auto result = this->book->cancel(CancelOrder {middleId});

    ASSERT_TRUE(result.has_value());

    // Total should be 50 + 20 = 70
    auto bestBid = this->book->getBestBid();
    TestHelpers::assertLevel(bestBid, 100, 70);
}

TYPED_TEST(NasdaqBookTest, CancelLastOrderAtPriceLevel)
{
    this->addBuy(100, 50);
    uint64_t orderId = this->orderGen.lastId();

    this->listener.reset();
    auto result = this->book->cancel(CancelOrder {orderId});

    ASSERT_TRUE(result.has_value());

    // Price level should be removed
    auto bestBid = this->book->getBestBid();
    TestHelpers::assertEmptyLevel(bestBid);
}

TYPED_TEST(NasdaqBookTest, EmptyBookQueries)
{
    auto volume = this->book->getBuyVolumeAhead(TestHelpers::makePrice(100));
    EXPECT_EQ(TestHelpers::toU64(volume), 0u);

    auto level = this->book->getBidLevel(0);
    TestHelpers::assertEmptyLevel(level);
}

TYPED_TEST(NasdaqBookTest, VolumeAheadWithNoOrders)
{
    auto volume = this->book->getBuyVolumeAhead(TestHelpers::makePrice(100));
    EXPECT_EQ(TestHelpers::toU64(volume), 0u);

    volume = this->book->getSellVolumeAhead(TestHelpers::makePrice(100));
    EXPECT_EQ(TestHelpers::toU64(volume), 0u);
}

TYPED_TEST(NasdaqBookTest, TimePriorityFIFO)
{
    this->addBuy(100, 50);
    uint64_t firstId = this->orderGen.lastId();
    this->addBuy(100, 30);
    uint64_t secondId = this->orderGen.lastId();
    this->addBuy(100, 20);
    uint64_t thirdId = this->orderGen.lastId();

    // Verify FIFO ordering
    auto volume1 = this->book->getBuyVolumeAheadByOrder(firstId);
    ASSERT_TRUE(volume1.has_value());
    EXPECT_EQ(TestHelpers::toU64(volume1.value()), 0u);

    auto volume2 = this->book->getBuyVolumeAheadByOrder(secondId);
    ASSERT_TRUE(volume2.has_value());
    EXPECT_EQ(TestHelpers::toU64(volume2.value()), 50u);

    auto volume3 = this->book->getBuyVolumeAheadByOrder(thirdId);
    ASSERT_TRUE(volume3.has_value());
    EXPECT_EQ(TestHelpers::toU64(volume3.value()), 80u);
}

TYPED_TEST(NasdaqBookTest, PricePriorityBuyDescendingSellAscending)
{
    // Add buys at different prices
    this->addBuy(100, 50);
    this->addBuy(102, 40);
    this->addBuy(101, 30);

    // Best bid should be highest price
    auto bestBid = this->book->getBestBid();
    TestHelpers::assertLevel(bestBid, 102, 40);

    // Add sells at different prices
    this->addSell(105, 20);
    this->addSell(103, 30);
    this->addSell(104, 25);

    // Best ask should be lowest price
    auto bestAsk = this->book->getBestAsk();
    TestHelpers::assertLevel(bestAsk, 103, 30);
}

TYPED_TEST(NasdaqBookTest, MultiplePriceLevelsOnBothSides)
{
    // Build a realistic book
    this->addBuy(100, 50);
    this->addBuy(99, 40);
    this->addBuy(98, 30);

    this->addSell(101, 60);
    this->addSell(102, 70);
    this->addSell(103, 80);

    auto bestBid = this->book->getBestBid();
    auto bestAsk = this->book->getBestAsk();

    TestHelpers::assertLevel(bestBid, 100, 50);
    TestHelpers::assertLevel(bestAsk, 101, 60);

    // Verify depth queries
    auto bid1 = this->book->getBidLevel(1);
    TestHelpers::assertLevel(bid1, 99, 40);

    auto ask1 = this->book->getAskLevel(1);
    TestHelpers::assertLevel(ask1, 102, 70);
}

TYPED_TEST(NasdaqBookTest, InterleavedOperations)
{
    this->addBuy(100, 50);
    uint64_t id1 = this->orderGen.lastId();

    this->addSell(101, 60);

    this->addBuy(99, 40);
    uint64_t id2 = this->orderGen.lastId();

    // Execute partial
    this->book->execute(ExecuteOrder {id1, 20});

    // Cancel
    this->book->cancel(CancelOrder {id2});

    // Add more
    this->addBuy(101, 30);

    // Replace
    uint64_t newId = this->orderGen.peekNextId();
    this->book->replace(ReplaceOrder {3000, id1, newId, 100, 25});

    // Verify final state
    auto bestBid = this->book->getBestBid();
    TestHelpers::assertLevel(bestBid, 101, 30);

    auto bestAsk = this->book->getBestAsk();
    TestHelpers::assertLevel(bestAsk, 101, 60);
}

TYPED_TEST(NasdaqBookTest, BuildAndTearDownScenario)
{
    // Build up book
    this->addBuy(100, 50);
    uint64_t b1 = this->orderGen.lastId();
    this->addBuy(99, 40);
    uint64_t b2 = this->orderGen.lastId();
    this->addSell(101, 60);
    uint64_t s1 = this->orderGen.lastId();
    this->addSell(102, 70);
    uint64_t s2 = this->orderGen.lastId();

    // Verify state
    TestHelpers::assertLevel(this->book->getBestBid(), 100, 50);
    TestHelpers::assertLevel(this->book->getBestAsk(), 101, 60);

    // Tear down
    this->book->cancel(CancelOrder {b1});
    TestHelpers::assertLevel(this->book->getBestBid(), 99, 40);

    this->book->cancel(CancelOrder {s1});
    TestHelpers::assertLevel(this->book->getBestAsk(), 102, 70);

    this->book->cancel(CancelOrder {b2});
    TestHelpers::assertEmptyLevel(this->book->getBestBid());

    this->book->cancel(CancelOrder {s2});
    TestHelpers::assertEmptyLevel(this->book->getBestAsk());
}
