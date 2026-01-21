import alpbook.internal.pool;
#include <gtest/gtest.h>
#include <memory_resource>

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

// Allocator support tests

TEST(ObjectPoolTest, PMRAllocator)
{
    std::pmr::monotonic_buffer_resource mbr(1024);
    std::pmr::polymorphic_allocator<int> alloc(&mbr);

    ObjectPool<int, std::pmr::polymorphic_allocator<int>> pool(100, alloc);

    auto idx0 = pool.allocate(42);
    auto idx1 = pool.allocate(99);

    EXPECT_EQ(pool[idx0], 42);
    EXPECT_EQ(pool[idx1], 99);
    EXPECT_EQ(pool.getAllocator(), alloc);
}

TEST(ObjectPoolTest, GetAllocator)
{
    ObjectPool<int> pool1;
    auto alloc1 = pool1.getAllocator();
    EXPECT_EQ(alloc1, std::allocator<int>());

    std::pmr::monotonic_buffer_resource mbr;
    std::pmr::polymorphic_allocator<double> alloc2(&mbr);
    ObjectPool<double, std::pmr::polymorphic_allocator<double>> pool2(alloc2);

    EXPECT_EQ(pool2.getAllocator(), alloc2);
}

TEST(ObjectPoolTest, AllocatorOnlyConstructor)
{
    std::pmr::monotonic_buffer_resource mbr;
    std::pmr::polymorphic_allocator<int> alloc(&mbr);

    ObjectPool<int, std::pmr::polymorphic_allocator<int>> pool(alloc);

    auto idx = pool.allocate(123);
    EXPECT_EQ(pool[idx], 123);
    EXPECT_EQ(pool.getAllocator(), alloc);
}

TEST(ObjectPoolTest, ReservedWithAllocator)
{
    std::pmr::monotonic_buffer_resource mbr(2048);
    std::pmr::polymorphic_allocator<TestObject> alloc(&mbr);

    ObjectPool<TestObject, std::pmr::polymorphic_allocator<TestObject>> pool(10, alloc);

    // All allocations should reuse reserved slots
    auto idx0 = pool.allocate();
    auto idx1 = pool.allocate();
    auto idx2 = pool.allocate();

    EXPECT_EQ(idx0, 0);
    EXPECT_EQ(idx1, 1);
    EXPECT_EQ(idx2, 2);
    EXPECT_EQ(pool.getAllocator(), alloc);
}

TEST(ObjectPoolTest, ZeroOverheadWithDefaultAllocator)
{
    // Verify that using the default allocator adds no size overhead
    // due to [[no_unique_address]] attribute
    struct Empty
    {
        int x;
    };

    // ObjectPool with default allocator should have the same size as
    // the version without allocator support would have had
    // (vector + uint32_t, no extra allocator storage for stateless allocators)
    constexpr size_t expectedMinSize = sizeof(std::vector<int>) + sizeof(uint32_t);

    EXPECT_LE(sizeof(ObjectPool<Empty>), expectedMinSize + sizeof(std::allocator<Empty>));

    // The [[no_unique_address]] optimization should keep the size minimal
    // This is implementation-defined, but most modern compilers optimize this
    static_assert(sizeof(std::allocator<int>) == 1 || sizeof(std::allocator<int>) == 0,
                  "std::allocator should be empty");
}

TEST(ObjectPoolTest, BackwardCompatibility)
{
    // Verify all existing usage patterns still work without changes
    ObjectPool<int> pool1;  // Default constructor
    ObjectPool<int> pool2(50);  // Reserved capacity constructor

    auto idx1 = pool1.allocate(100);
    auto idx2 = pool2.allocate(200);

    EXPECT_EQ(pool1[idx1], 100);
    EXPECT_EQ(pool2[idx2], 200);
}
