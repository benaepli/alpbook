module;

#include <expected>
#include <memory>
#include <span>
#include <vector>

#include <hwloc.h>

export module alpdaq.placement;

namespace alpdaq::placement
{
    export enum class PinError : uint8_t
    {
        InitializationFailed,
        BindFailure,
        CoreDiscoveryError
    };

    export enum class CoreType
    {
        Performance,
        Efficiency,
        Unknown,
    };

    export struct ProcessingUnit
    {
        uint32_t osIndex;
    };

    export struct PhysicalCore
    {
        uint32_t logicalId;
        CoreType coreType;
        std::vector<ProcessingUnit> pus;
    };

    export class Pinner
    {
      public:
        static std::expected<std::unique_ptr<Pinner>, PinError> create() noexcept
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
            if (auto err = pinner->discover(); !err.has_value())
            {
                return std::unexpected(err.error());
            }
            return pinner;
        }

        Pinner(Pinner const&) = delete;
        Pinner& operator=(Pinner const&) = delete;
        Pinner(Pinner&&) = delete;
        Pinner& operator=(Pinner&&) = delete;

        ~Pinner() { hwloc_topology_destroy(topology_); }

        std::expected<void, PinError> pinToPU(uint32_t puIndex) noexcept
        {
            hwloc_bitmap_t set = hwloc_bitmap_alloc();
            if (set == nullptr)
            {
                return std::unexpected(PinError::BindFailure);
            }
            hwloc_bitmap_set(set, puIndex);

            if (hwloc_set_cpubind(topology_, set, HWLOC_CPUBIND_THREAD) < 0)
            {
                hwloc_bitmap_free(set);
                return std::unexpected(PinError::BindFailure);
            }
            hwloc_bitmap_free(set);
            return {};
        }
        std::span<PhysicalCore const> getTopology() const { return std::span(cores_); }

      private:
        Pinner() = default;

        std::expected<void, PinError> discover() noexcept
        {
            int const coreCount = hwloc_get_nbobjs_by_type(topology_, HWLOC_OBJ_CORE);
            if (coreCount < 0)
            {
                return std::unexpected(PinError::CoreDiscoveryError);
            }
            for (uint32_t i = 0; i < coreCount; ++i)
            {
                hwloc_obj_t coreObj = hwloc_get_obj_by_type(topology_, HWLOC_OBJ_CORE, i);
                if (coreObj == nullptr)
                {
                    return std::unexpected(PinError::CoreDiscoveryError);
                }

                PhysicalCore core {.logicalId = i, .coreType = CoreType::Unknown};

                hwloc_obj_t puObj = nullptr;
                while (true)
                {
                    puObj = hwloc_get_next_obj_inside_cpuset_by_type(
                        topology_, coreObj->cpuset, HWLOC_OBJ_PU, puObj);
                    if (puObj == nullptr)
                    {
                        break;
                    }
                    core.pus.emplace_back(puObj->os_index);
                }

                cores_.push_back(std::move(core));
            }
            return {};
        }

        CoreType cpuKind(hwloc_obj_t coreObj) noexcept
        {
            int kindIndex = hwloc_cpukinds_get_by_cpuset(topology_, coreObj->cpuset, 0);
            if (kindIndex < 0)
            {
                return CoreType::Unknown;
            }
            int efficiency;
            if (hwloc_cpukinds_get_info(topology_, kindIndex, NULL, &efficiency, NULL, NULL, 0)
                != 0)
            {
                return CoreType::Unknown;
            }
            if (efficiency == 0)
            {
                return CoreType::Efficiency;
            }
            return CoreType::Performance;
        }

        hwloc_topology_t topology_;
        std::vector<PhysicalCore> cores_;
    };
}  // namespace alpdaq::placement