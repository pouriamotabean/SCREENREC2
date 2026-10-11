#include "Timing.h"
#include <thread>
#include <chrono>

#ifdef _WIN32
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <mmsystem.h>

namespace Timing
{
    int64_t now()
    {
        LARGE_INTEGER v;
        QueryPerformanceCounter (&v);
        return (int64_t) v.QuadPart;
    }

    int64_t frequency()
    {
        static const int64_t f = []
        {
            LARGE_INTEGER v;
            QueryPerformanceFrequency (&v);
            return (int64_t) v.QuadPart;
        }();
        return f;
    }

    HighResTimerScope::HighResTimerScope()  { timeBeginPeriod (1); }
    HighResTimerScope::~HighResTimerScope() { timeEndPeriod (1); }
}
#else
namespace Timing
{
    int64_t now()
    {
        return (int64_t) std::chrono::duration_cast<std::chrono::nanoseconds> (
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    int64_t frequency() { return 1000000000LL; }
    HighResTimerScope::HighResTimerScope()  {}
    HighResTimerScope::~HighResTimerScope() {}
}
#endif

namespace Timing
{
    double toSeconds (int64_t ticks)      { return (double) ticks / (double) frequency(); }
    int64_t fromSeconds (double seconds)  { return (int64_t) (seconds * (double) frequency()); }

    void sleepUntil (int64_t target)
    {
        for (;;)
        {
            const double remaining = toSeconds (target - now());
            if (remaining <= 0.0)
                return;

            if (remaining > 0.003)
                std::this_thread::sleep_for (std::chrono::microseconds ((int64_t) ((remaining - 0.002) * 1.0e6)));
            else
                std::this_thread::yield();
        }
    }
}
