module;

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

import alpbook.dispatch;

export module alpbook.itch.dispatch;

namespace alpbook::itch
{
    export using IdType = uint16_t;

    export struct DistributeRoundRobin
    {
    };
    export struct KeepUnassigned
    {
    };

    /// A simple strategy for mapping IDs to threads.
    export template<typename Tag = KeepUnassigned>
    class ArrayMapper
    {
        static constexpr uint32_t UNASSIGNED = std::numeric_limits<uint32_t>::max();
        static constexpr uint32_t TO_ASSIGN = UNASSIGNED - 1;
        static constexpr size_t MAX_LOCATE_ID = 65536;

      public:
        ArrayMapper() { lookupTable_.fill(UNASSIGNED); }

        void setThreadCount(uint32_t count)
        {
            threadCount_ = count;

            if constexpr (std::is_same_v<Tag, DistributeRoundRobin>)
            {
                distributeRoundRobin();
            }
            else
            {
                distributeAssigned();
            }
            lookupTable_[0] = UNASSIGNED - 1;
        }

        [[nodiscard]] uint32_t getWorkerIndex(IdType id) const noexcept { return lookupTable_[id]; }

        /// Assign can only be called prior to dispatch initialization.
        /// No-op for DistributeRoundRobin.
        void assign(IdType id) noexcept
        {
            if (threadCount_ != 0)
            {
                return;
            }
            lookupTable_[id] = TO_ASSIGN;
        }
        void assign(IdType id, uint32_t forceCore) noexcept
        {
            if (threadCount_ != 0)
            {
                return;
            }
            lookupTable_[id] = forceCore;
        }

        [[nodiscard]]
        std::vector<IdType> getIDsForThread(uint32_t core) const
        {
            // Simple linear scan is fine for cold initialization
            std::vector<IdType> assets;
            for (size_t id = 0; id < lookupTable_.size(); ++id)
            {
                if (lookupTable_[id] == core)
                {
                    assets.push_back(static_cast<IdType>(id));
                }
            }
            return assets;
        }

      private:
        void pin(size_t id, uint32_t core) { lookupTable_[id] = core; }

        void distributeRoundRobin()
        {
            for (size_t id = 0; id < MAX_LOCATE_ID; ++id)
            {
                pin(id, id % threadCount_);
            }
        }

        void distributeAssigned()
        {
            uint32_t nextAutoCore = 0;
            for (size_t id = 0; id < MAX_LOCATE_ID; ++id)
            {
                uint32_t val = lookupTable_[id];
                if (val == TO_ASSIGN)
                {
                    pin(id, nextAutoCore);
                    nextAutoCore = (nextAutoCore + 1) % threadCount_;
                }
                else if (val != UNASSIGNED)
                {
                    pin(id, val % threadCount_);
                }
            }
        }

        std::array<uint32_t, MAX_LOCATE_ID> lookupTable_ {};
        uint32_t threadCount_ = 0;
    };
}  // namespace alpbook::itch