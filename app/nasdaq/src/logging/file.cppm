module;

#include <filesystem>
#include <fstream>
#include <variant>

import alpdaq.logging.logger;
import alpdaq.logging.messages;

export module alpdaq.logging.file;

namespace alpdaq::logging
{
    export template<typename F, typename Data>
    concept FailureHandler = requires(F f, Message<Data> msg) {
        { f(msg) } -> std::same_as<void>;
    };

    export using SystemData = std::variant<std::monostate>;

    export template<typename F>
    concept SystemFailureHandler = FailureHandler<F, SystemData>;

    export template<SystemFailureHandler OnFailure>
    struct FileOutput
    {
        std::filesystem::path path;
        OnFailure onFailure;
        std::ofstream stream;

        FileOutput& operator<<(Message<SystemData> msg) noexcept
        {
            if (!stream.is_open())
            {
                onFailure(msg);
                return *this;
            }

            // TODO: actually write logged messages
            stream << "TODO" << "\n";

            if (stream.fail())
            {
                onFailure(msg);
                stream.clear();
            }
            return *this;
        }

        static void rotate() noexcept
        {
            // No-op.
        }
    };

    static_assert(OutputSink<FileOutput<void (*)(Message<SystemData>)>, SystemData>);
}  // namespace alpdaq::logging