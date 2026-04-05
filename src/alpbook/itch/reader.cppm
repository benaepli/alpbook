module;

#include <bit>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

#include "zlib.h"

export module alpbook.itch.reader;

import alpbook.itch.parsing;

namespace alpbook::itch
{
    export enum class StreamStatus : uint8_t
    {
        EndOfFile,
        ReadError
    };

    export template<bool Benchmark = false>
    class ItchStream
    {
      public:
        static std::expected<ItchStream, StreamStatus> open(std::filesystem::path const& path)
        {
            gzFile file = gzopen(path.c_str(), "rb");
            if (!file)
            {
                return std::unexpected(StreamStatus::ReadError);
            }
            return ItchStream(file);
        }

        std::expected<std::reference_wrapper<ItchSlot<Benchmark>>, StreamStatus> next()
        {
            int bytesRead = gzread(file_, &msgLenBigEndian_, sizeof(msgLenBigEndian_));
            if (bytesRead < static_cast<int>(sizeof(msgLenBigEndian_)))
            {
                return std::unexpected(StreamStatus::EndOfFile);
            }

            uint16_t msgLen = std::byteswap(msgLenBigEndian_);
            uint16_t bytesToRead = std::min(static_cast<size_t>(msgLen), slot_.data.size());
            bytesRead = gzread(file_, slot_.data.data(), bytesToRead);
            if (bytesRead < bytesToRead)
            {
                return std::unexpected(StreamStatus::ReadError);
            }

            if (msgLen > bytesToRead)
            {
                gzseek(file_, msgLen - bytesToRead, SEEK_CUR);
            }

            return slot_;
        }

        ItchStream(ItchStream const&) = delete;
        ItchStream& operator=(ItchStream const&) = delete;

        ItchStream(ItchStream&& other) noexcept
            : file_(other.file_)
            , slot_(other.slot_)
            , msgLenBigEndian_(other.msgLenBigEndian_)
        {
            other.file_ = nullptr;
        }

        ItchStream& operator=(ItchStream&& other) noexcept
        {
            if (this != &other)
            {
                if (file_)
                {
                    gzclose(file_);
                }
                file_ = other.file_;
                slot_ = other.slot_;
                msgLenBigEndian_ = other.msgLenBigEndian_;
                other.file_ = nullptr;
            }
            return *this;
        }

        ~ItchStream()
        {
            if (file_)
            {
                gzclose(file_);
            }
        }

      private:
        explicit ItchStream(gzFile file)
            : file_(file)
        {
        }

        gzFile file_ = nullptr;
        ItchSlot<Benchmark> slot_ {};
        uint16_t msgLenBigEndian_ = 0;
    };
}  // namespace alpbook::itch
