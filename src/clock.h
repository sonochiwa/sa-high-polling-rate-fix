#pragma once

#include <windows.h>

namespace clock {

// Reads the performance counter frequency. Called once before any
// MicrosSince.
void Initialize();

// Microseconds since `ticks`, storing the current counter in `now`. Returns
// -1 when the performance counter is unavailable.
LONGLONG MicrosSince(LONGLONG ticks, LONGLONG* now);

} // namespace clock
