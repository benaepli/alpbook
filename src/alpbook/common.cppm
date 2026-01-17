module;

#include "absl/numeric/int128.h"

export module alpbook.common;

export namespace alpbook
{
    using price_t = absl::uint128;
    using quantity_t = absl::uint128;

    // Compile-time information about the units of an exchange.
    struct SymbolConfig
    {
        uint64_t priceScale;
        uint64_t qtyScale;
    };

    constexpr SymbolConfig NASDAQ_SYMBOL_CONFIG = {
        .priceScale = 10'000,
        .qtyScale = 1,
    };

    struct SymbolRules
    {
        int64_t minTick;
        int64_t minQty;
        int64_t maxQty;
    };

    enum class Side : uint8_t
    {
        Buy,
        Sell,
    };

    struct Level
    {
        price_t price {};
        quantity_t quantity {};

        [[nodiscard]] bool isValid() const noexcept { return quantity != 0; }
    };
}  // namespace alpbook