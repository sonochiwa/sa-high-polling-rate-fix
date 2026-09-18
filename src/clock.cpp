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
    return (counter.QuadPart - ticks) * 1000000 / g_ticksPerSecond;
}

} // namespace clock
