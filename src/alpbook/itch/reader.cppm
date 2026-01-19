module;

#include <bit>
#include <concepts>
#include <expected>
#include <filesystem>
#include <string>

#include "zlib.h"

import alpbook.book.nasdaq;
import alpbook.itch.parsing;

export module alpbook.itch.reader;

namespace alpbook::itch
{
    template<typename H, bool B>
    concept DirectHandler = requires(H h, ItchSlot<B>& slot) {
        { h.handle(slot) } -> std::same_as<void>;
    };

    export template<typename H, bool B>
        requires DirectHandler<H, B>
    std::expected<void, std::string> readGzippedItch(std::filesystem::path const& path, H& handler)
    {
        gzFile file = gzopen(path.c_str(), "rb");
        if (!file)
        {
            return std::unexpected("Failed to open GZIP file: " + path.string());
        }
        ItchSlot<B> slot;
        slot.type = MessageOrigin::Live;

        uint16_t msgLenBigEndian = 0;

        while (true)
        {
            // Read the 2-byte length prefix (standard ITCH file format)
            int bytesRead = gzread(file, &msgLenBigEndian, sizeof(msgLenBigEndian));
            if (bytesRead < static_cast<int>(sizeof(msgLenBigEndian)))
            {
                break;
            }
            uint16_t msgLen = std::byteswap(msgLenBigEndian);
            // We only read what fits in our fixed 55-byte buffer
            uint16_t bytesToRead = std::min(static_cast<size_t>(msgLen), slot.data.size());
            bytesRead = gzread(file, slot.data.data(), bytesToRead);
            if (bytesRead < bytesToRead)
            {
                break;
            }

            if (msgLen > bytesToRead)
            {
                gzseek(file, msgLen - bytesToRead, SEEK_CUR);
            }
            handler.handle(slot);
        }

        gzclose(file);
        return {};
    }
}  // namespace alpbook::itch