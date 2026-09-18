#pragma once

#include <windows.h>

// Legacy mouse message suppression.
//
// RIDEV_NOLEGACY is what actually removes the stutter: the per-packet work
// Windows does to generate WM_MOUSEMOVE and friends for this process costs
// more than the raw input read itself, and it is done under the desktop input
// lock, so it serialises against the game thread rather than merely competing
// with it for CPU. The messages the front-end genuinely needs, the cursor
// position, the clicks, the wheel and the double clicks, are posted back to
// the game window from here instead.
namespace legacy {

// The game window, taken from SetCooperativeLevel.
void SetTargetWindow(HWND window);
HWND TargetWindow();

// Re-reads the client size and the double-click metrics of the target
// window. Called from the raw input thread's maintenance timer.
void RefreshTargetMetrics();

// Posts WM_MOUSEMOVE with where Windows has put the cursor, rate limited and
// only when the position changed. Called for every packet.
void TrackCursor();

// Posts the button and wheel messages for one packet's RI_MOUSE_* flags.
void PostButtons(USHORT flags, LONG wheel);

} // namespace legacy
