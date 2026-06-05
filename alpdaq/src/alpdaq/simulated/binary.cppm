module;

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>

export module alpdaq.simulated.binary;

import alpbook.itch;
import alpdaq.system.state;

namespace alpdaq::simulated
{
    export class BinaryItchSource
    {
      public:
        static std::expected<BinaryItchSource, alpbook::itch::StreamStatus> open(
            std::filesystem::path const& path, SessionId session)
        {
            auto stream = alpbook::itch::ItchStream<>::open(path);
            if (!stream)
            {
                return std::unexpected(stream.error());
            }
            return BinaryItchSource(std::move(*stream), path, session);
        }

        template<typename DataCb, typename EventCb>
        void poll(DataCb onData, EventCb onEvent) noexcept
        {
            if (failed_) [[unlikely]]
            {
                onEvent(SourceEvent {FatalError {}});
                return;
            }

            if (needsSessionEvent_) [[unlikely]]
            {
                needsSessionEvent_ = false;
                onEvent(SourceEvent {SessionChanged {.newSession = session_}});
                return;
            }

            auto result = stream_.next();
            if (!result)
            {
                failed_ = true;
                onEvent(SourceEvent {FatalError {}});
                return;
            }

            auto& slot = *result;
            ItchView view {
                .payload = std::span<std::byte const>(slot.get().data),
            };
            onData(view);
        }

        void forceRestart() noexcept
        {
            needsSessionEvent_ = true;

            auto stream = alpbook::itch::ItchStream<>::open(path_);
            if (stream)
            {
                stream_ = std::move(*stream);
            }
            else
            {
                failed_ = true;
            }
        }

      private:
        BinaryItchSource(alpbook::itch::ItchStream<> stream,
                         std::filesystem::path path,
                         SessionId session) noexcept
            : stream_(std::move(stream))
            , path_(std::move(path))
            , session_(session)
        {
        }

        alpbook::itch::ItchStream<> stream_;
        std::filesystem::path path_;
        SessionId session_;
        bool needsSessionEvent_ = true;
        bool failed_ = false;
    };
}  // namespace alpdaq::simulated
