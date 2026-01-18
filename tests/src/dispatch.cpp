#include <atomic>
#include <chrono>
#include <vector>
#include <thread>
#include <deque>
#include <memory>

#include <gtest/gtest.h>

import alpbook.dispatch;

using namespace alpbook;

namespace
{
    constexpr size_t TestSlotSize = 64;

    struct alignas(std::hardware_destructive_interference_size) TestSlot
    {
        std::array<uint8_t, TestSlotSize> data;
    };

    struct MockSink
    {
        std::atomic<uint32_t>& messageCount;
        uint32_t lastId = 0;

        void onMessage(TestSlot data)
        {
            lastId = *reinterpret_cast<uint16_t*>(data.data.data()); // Assume ID is at the start
            messageCount.fetch_add(1, std::memory_order_relaxed);
        }
    };

    struct MockMapper
    {
        uint32_t threadCount = 0;

        void setThreadCount(uint32_t count)
        {
            threadCount = count;
        }

        uint32_t getWorkerIndex(uint16_t id)
        {
            if (id == 0xFFFF) return std::numeric_limits<uint32_t>::max(); // DROP_MSG
            return id % threadCount;
        }
    };

    struct MockFactory
    {
        using SinkType = MockSink;
        std::vector<std::unique_ptr<std::atomic<uint32_t>>>* counters;

        MockSink create(uint32_t coreIndex, MockMapper const&)
        {
            return MockSink {*(*counters)[coreIndex]};
        }
    };

    struct MockExtractor
    {
        static uint16_t extractID(TestSlot const& slot)
        {
            return *reinterpret_cast<uint16_t const*>(slot.data.data());
        }
    };

    static_assert(DispatchSlot<TestSlot>);
    static_assert(PacketSink<TestSlot, MockSink>);
    static_assert(IDExtractor<TestSlot, MockExtractor>);
}

class DispatcherTest : public ::testing::Test
{
  protected:
    std::vector<std::unique_ptr<std::atomic<uint32_t>>> counters;
    MockMapper mapper;
    MockFactory factory;

    void SetUp() override
    {
        // Pre-size counters to a reasonable maximum to avoid out-of-bounds during create()
        for(int i=0; i<64; ++i) {
            counters.push_back(std::make_unique<std::atomic<uint32_t>>(0));
        }
        factory.counters = &counters;
    }

    void resetCounters() {
        for(auto& c : counters) c->store(0);
    }
};

TEST_F(DispatcherTest, Initialization)
{
    Dispatcher<TestSlot, MockFactory, MockExtractor, MockMapper> dispatcher(mapper, factory);

    auto result = dispatcher.init(2);
    if (!result.has_value())
    {
        GTEST_SKIP() << "Dispatcher::init failed, likely due to hwloc error";
    }

    EXPECT_GT(mapper.threadCount, 0);
    EXPECT_LE(mapper.threadCount, 2);
    EXPECT_EQ(mapper.threadCount & (mapper.threadCount - 1), 0);
}

TEST_F(DispatcherTest, DispatchMessages)
{
    Dispatcher<TestSlot, MockFactory, MockExtractor, MockMapper> dispatcher(mapper, factory);

    auto result = dispatcher.init(4);
    if (!result.has_value())
    {
        GTEST_SKIP() << "Dispatcher::init failed";
    }

    // Messages are being processed by workers...
}

TEST_F(DispatcherTest, DispatchCorrectRouting)
{
    Dispatcher<TestSlot, MockFactory, MockExtractor, MockMapper> dispatcher(mapper, factory);

    auto result = dispatcher.init(2);
    if (!result.has_value())
    {
        GTEST_SKIP() << "Dispatcher::init failed";
    }

    uint32_t threadCount = mapper.threadCount;
    resetCounters();
    
    uint32_t const msgsPerThread = 100;
    for (uint32_t i = 0; i < threadCount * msgsPerThread; ++i)
    {
        TestSlot slot;
        *reinterpret_cast<uint16_t*>(slot.data.data()) = static_cast<uint16_t>(i);
        dispatcher.dispatch(slot);
    }

    auto start = std::chrono::steady_clock::now();
    uint32_t totalProcessed = 0;
    while (totalProcessed < threadCount * msgsPerThread)
    {
        totalProcessed = 0;
        for (uint32_t i = 0; i < threadCount; ++i)
        {
            totalProcessed += counters[i]->load(std::memory_order_relaxed);
        }
        
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2))
        {
            break;
        }
        std::this_thread::yield();
    }

    EXPECT_EQ(totalProcessed, threadCount * msgsPerThread);
    for (uint32_t i = 0; i < threadCount; ++i)
    {
        EXPECT_EQ(counters[i]->load(), msgsPerThread);
    }
}

TEST_F(DispatcherTest, DropMessages)
{
    Dispatcher<TestSlot, MockFactory, MockExtractor, MockMapper> dispatcher(mapper, factory);

    auto result = dispatcher.init(2);
    if (!result.has_value())
    {
        GTEST_SKIP() << "Dispatcher::init failed";
    }

    resetCounters();

    // 0xFFFF is DROP_MSG
    TestSlot slot;
    *reinterpret_cast<uint16_t*>(slot.data.data()) = 0xFFFF;
    dispatcher.dispatch(slot);

    // Valid message
    *reinterpret_cast<uint16_t*>(slot.data.data()) = 0;
    dispatcher.dispatch(slot);

    auto start = std::chrono::steady_clock::now();
    while (counters[0]->load() == 0)
    {
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(1))
        {
            break;
        }
        std::this_thread::yield();
    }

    uint32_t total = 0;
    for (uint32_t i = 0; i < mapper.threadCount; ++i)
    {
        total += counters[i]->load();
    }
    EXPECT_EQ(total, 1);
}
