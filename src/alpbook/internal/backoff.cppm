module;

#include <atomic>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64)
#    include <immintrin.h>
#endif

export module alpbook.internal.backoff;

export namespace alpbook::internal
{
    template<uint32_t BusyLimit,
             uint32_t SpinLimit,
             uint32_t YieldLimit = std::numeric_limits<uint32_t>::max(),
             std::chrono::duration WaitDuration = std::chrono::microseconds(1)>
    class Backoff
    {
        uint32_t count = 0;

      public:
        void pause()
        {
            if (count < BusyLimit)
            {
                // Nothing.
            }
            else if (count < SpinLimit)
            {
#if defined(__x86_64__) || defined(_M_X64)
                _mm_pause();
#elif defined(__arm__) || defined(__aarch64__)
                asm volatile("yield" ::: "memory");
#endif
            }
            else if (count < YieldLimit)
            {
                std::this_thread::yield();
            }
            else
            {
                std::this_thread::sleep_for(WaitDuration);
            }
            count++;
        }

        void reset() { count = 0; }
    };
}  // namespace alpbook::internal