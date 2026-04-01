module;

#include <variant>

export module alpdaq.logging.messages;

namespace alpdaq::logging
{
    export using Data = std::variant<std::monostate>;

    export struct Message
    {
        Data data;
    };
}  // namespace alpdaq::logging
