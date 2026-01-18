module;

#include <cstdint>

import alpbook.dispatch;

export module alpbook.itch.dispatch;

namespace alpbook::itch
{
    constexpr auto MESSAGE_SLOT_SIZE = 64;

    using ItchSlot = MsgSlot<MESSAGE_SLOT_SIZE>;
    using IdType = std::uint16_t;

    export class ShiftMapper
    {
      public:
        ShiftMapper() = default;

        void setThreadCount(uint16_t threadCount) { mask_ = threadCount - 1; }
        [[nodiscard]] uint32_t getWorkerIndex(uint16_t id) const { return id & mask_; }

      private:
        uint32_t mask_ {};
    };
}  // namespace alpbook::itch