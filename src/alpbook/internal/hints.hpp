#pragma once

#ifdef _MSC_VER
#    define ALPBOOK_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#    define ALPBOOK_INLINE inline __attribute__((always_inline))
#else
#    define ALPBOOK_INLINE inline
#endif

#if defined(__GNUC__) || defined(__clang__)
#    define ALPBOOK_COLD __attribute__((cold))
#else
#    define ALPBOOK_COLD
#endif
