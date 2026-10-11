#pragma once
#include <cstdint>

// One shared high-resolution clock (Windows QueryPerformanceCounter) used by
// the audio thread, the screen-capture thread and the GUI, so audio and video
// can be placed on exactly the same timeline.
namespace Timing
{
    int64_t now();                  // raw counter ticks
    int64_t frequency();            // ticks per second
    double  toSeconds (int64_t ticks);
    int64_t fromSeconds (double seconds);

    // Sleep until the given counter value (coarse sleep, then short spin).
    void sleepUntil (int64_t target);

    // Raise the Windows timer resolution to 1 ms while an instance is alive.
    struct HighResTimerScope
    {
        HighResTimerScope();
        ~HighResTimerScope();
    };
}
