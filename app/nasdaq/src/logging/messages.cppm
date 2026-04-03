module;

#include <cstdint>
#include <variant>

export module alpdaq.logging.messages;

namespace alpdaq::logging
{
    export enum class Level : uint8_t
    {
        Info,
        Warn,
        Error
    };

    export template<typename Data>
    struct Message
    {
        Level level;
        Data data;
    };
}  // namespace alpdaq::logging
