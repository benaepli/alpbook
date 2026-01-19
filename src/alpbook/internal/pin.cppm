module;

#include <cstdint>
#include <expected>
#include <memory>

#include <hwloc.h>

export module alpbook.internal.pin;

export namespace alpbook::internal
{
    enum class PinError : uint8_t
    {
        InitializationFailed,
        NoSuchInterface,
        NoSuchCore,
        BindFailure,
        CoreCountError,
    };

    class Pinner
    {
      public:
        static std::expected<std::unique_ptr<Pinner>, PinError> create()
        {
            auto pinner = std::unique_ptr<Pinner>(new Pinner());
            if (hwloc_topology_init(&pinner->topology_) != 0)
            {
                return std::unexpected(PinError::InitializationFailed);
            }
            if (hwloc_topology_load(pinner->topology_) != 0)
            {
                hwloc_topology_destroy(pinner->topology_);
                return std::unexpected(PinError::InitializationFailed);
            }
            return pinner;
        }

        Pinner(Pinner const&) = delete;
        Pinner& operator=(Pinner const&) = delete;
        Pinner(Pinner&&) = delete;
        Pinner& operator=(Pinner&&) = delete;

        ~Pinner() { hwloc_topology_destroy(topology_); }

        std::expected<void, PinError> pinToCore(uint32_t coreIndex)
        {
            hwloc_obj_t core = hwloc_get_obj_by_type(topology_, HWLOC_OBJ_CORE, coreIndex);
            if (core == nullptr)
            {
                return std::unexpected(PinError::NoSuchCore);
            }
            // Pin to the first PU inside this Core to avoid hyperthreading
            int const puIndex = hwloc_bitmap_first(core->cpuset);
            return pinToPU(puIndex);
        }

        [[nodiscard]] std::expected<uint32_t, PinError> getCoreCount() const noexcept
        {
            int const result = hwloc_get_nbobjs_by_type(topology_, HWLOC_OBJ_CORE);
            if (result < 0)
            {
                return std::unexpected(PinError::CoreCountError);
            }
            return result;
        }

      private:
        Pinner() = default;

        std::expected<void, PinError> pinToPU(int puIndex) noexcept
        {
            hwloc_bitmap_t set = hwloc_bitmap_alloc();
            hwloc_bitmap_set(set, puIndex);

            if (hwloc_set_cpubind(topology_, set, HWLOC_CPUBIND_THREAD) < 0)
            {
                hwloc_bitmap_free(set);
                return std::unexpected(PinError::BindFailure);
            }
            hwloc_bitmap_free(set);
            return {};
        }

        hwloc_topology_t topology_;
    };
}  // namespace alpbook::internal