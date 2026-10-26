#include "clock.h"

namespace clock {
namespace {

LONGLONG g_ticksPerSecond = 0;

} // namespace

void Initialize() {
    LARGE_INTEGER frequency = {};
    g_ticksPerSecond = QueryPerformanceFrequency(&frequency) ? frequency.QuadPart : 0;
}

LONGLONG MicrosSince(LONGLONG ticks, LONGLONG* now) {
    LARGE_INTEGER counter = {};
    if (!g_ticksPerSecond || !QueryPerformanceCounter(&counter)) {
        return -1;
    }
    *now = counter.QuadPart;
    // Whole seconds and the remainder are converted apart: multiplying the
    // full difference by a million overflows once it spans about ten days of
    // a 10 MHz counter, and a zero `ticks` spans the whole system uptime.
    LONGLONG elapsed = counter.QuadPart - ticks;
    return elapsed / g_ticksPerSecond * 1000000 +
           elapsed % g_ticksPerSecond * 1000000 / g_ticksPerSecond;
}

} // namespace clock
