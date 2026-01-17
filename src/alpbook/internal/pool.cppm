module;

#include <concepts>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

export module alpbook.internal.pool;

export namespace alpbook::internal
{
    /// A pool for at most std::numeric_limits<uint32_t>::max() objects.
    /// The memory addresses of each object in the pool are not stable,
    /// but the returned identifiers are. Objects are constructed with
    /// provided arguments using perfect forwarding.
    /// Objects must be trivially relocatable and trivially destructible.
    template<typename T>
        requires std::is_trivially_destructible_v<T>
    class ObjectPool
    {
      public:
        ObjectPool() noexcept
            : firstFree_(0)
        {
        }

        explicit ObjectPool(uint32_t reserved)
            : firstFree_(0)
            , slots_(reserved)
        {
            for (auto i = 0; i < reserved; i++)
            {
                slots_[i].nextFree = i + 1;
            }
        }

        ObjectPool(ObjectPool const&) = delete;
        ObjectPool& operator=(ObjectPool const&) = delete;
        ObjectPool(ObjectPool&&) = delete;
        ObjectPool& operator=(ObjectPool&&) = delete;

        template<typename... Args>
        uint32_t allocate(Args&&... args)
        {
            auto size = static_cast<uint32_t>(slots_.size());
            if (firstFree_ == size)
            {
                slots_.push_back(Slot(0));
                std::construct_at(&slots_[size].data, std::forward<Args>(args)...);
                firstFree_ = size + 1;
                return size;
            }
            auto result = firstFree_;
            firstFree_ = slots_[firstFree_].nextFree;
            std::construct_at(&slots_[result].data, std::forward<Args>(args)...);
            return result;
        }

        void deallocate(uint32_t index) noexcept
        {
            slots_[index].nextFree = firstFree_;
            firstFree_ = index;
        }

        void reset() noexcept
        {
            auto size = static_cast<uint32_t>(slots_.size());
            firstFree_ = 0;

            // Rebuild free list: each slot points to the next
            for (uint32_t i = 0; i < size; i++)
            {
                slots_[i].nextFree = i + 1;
            }
        }

        T& operator[](uint32_t index) noexcept { return slots_[index].data; }
        T const& operator[](uint32_t index) const noexcept { return slots_[index].data; }

      private:
        union Slot
        {
            T data;
            uint32_t nextFree;

            Slot()
                : nextFree(0)
            {
            }
            Slot(uint32_t free)
                : nextFree(free)
            {
            }
            ~Slot() {}
            Slot(Slot&& other) noexcept
                : nextFree(other.nextFree)
            {
            }
        };

        std::vector<Slot> slots_;
        uint32_t firstFree_;
    };
}  // namespace alpbook::internal