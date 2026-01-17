import alpbook.internal.pool;
#include <gtest/gtest.h>

using alpbook::internal::ObjectPool;

struct TestObject
{
    int value = 42;
};

TEST(ObjectPoolTest, BasicAllocationAndAccess)
{
    ObjectPool<int> pool;

    // Allocate with initial values using structured bindings
    auto idx0 = pool.allocate(100);
    auto idx1 = pool.allocate(200);
    auto idx2 = pool.allocate(300);

    EXPECT_EQ(idx0, 0);
    EXPECT_EQ(idx1, 1);
    EXPECT_EQ(idx2, 2);

    EXPECT_EQ(pool[idx0], 100);
    EXPECT_EQ(pool[idx1], 200);
    EXPECT_EQ(pool[idx2], 300);

    // Test modification via operator[]
    pool[idx0] = 999;
    EXPECT_EQ(pool[idx0], 999);
}

TEST(ObjectPoolTest, DeallocationAndReuse)
{
    ObjectPool<int> pool;

    auto idx0 = pool.allocate(0);
    auto idx1 = pool.allocate(0);
    auto idx2 = pool.allocate(0);

    // Deallocate in order: 0, 1, 2
    pool.deallocate(idx0);
    pool.deallocate(idx1);
    pool.deallocate(idx2);

    // Should reuse in LIFO order: 2, 1, 0
    auto reused0 = pool.allocate(0);
    auto reused1 = pool.allocate(0);
    auto reused2 = pool.allocate(0);

    EXPECT_EQ(reused0, 2);
    EXPECT_EQ(reused1, 1);
    EXPECT_EQ(reused2, 0);
}

TEST(ObjectPoolTest, ReservedCapacity)
{
    ObjectPool<int> pool(5);

    // All allocations should succeed without growing
    auto idx0 = pool.allocate(10);
    auto idx1 = pool.allocate(20);
    auto idx2 = pool.allocate(30);
    auto idx3 = pool.allocate(40);
    auto idx4 = pool.allocate(50);

    EXPECT_EQ(idx0, 0);
    EXPECT_EQ(idx1, 1);
    EXPECT_EQ(idx2, 2);
    EXPECT_EQ(idx3, 3);
    EXPECT_EQ(idx4, 4);

    EXPECT_EQ(pool[idx0], 10);
    EXPECT_EQ(pool[idx1], 20);
    EXPECT_EQ(pool[idx0], 10);
}

TEST(ObjectPoolTest, StructWithDefaultConstructor)
{
    ObjectPool<TestObject> pool;
    auto idx = pool.allocate();

    // Verify default construction
    EXPECT_EQ(pool[idx].value, 42);

    // Verify modification
    pool[idx].value = 999;
    EXPECT_EQ(pool[idx].value, 999);
}

struct ComplexObject
{
    int a;
    double b;

    ComplexObject(int a, double b)
        : a(a)
        , b(b)
    {
    }
};

TEST(ObjectPoolTest, PerfectForwarding)
{
    ObjectPool<ComplexObject> pool;

    auto idx = pool.allocate(42, 3.14);
    EXPECT_EQ(pool[idx].a, 42);
    EXPECT_EQ(pool[idx].b, 3.14);
}
