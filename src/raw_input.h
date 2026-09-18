#pragma once

#include <windows.h>

// Number of buttons reported to DirectInput consumers: left, right, middle,
// X1 and X2, in the order DIMOUSESTATE uses.
constexpr int kMouseButtonCount = 5;

// One frame worth of mouse movement, in the same units DirectInput reports:
// raw device counts for the axes and WHEEL_DELTA multiples for the wheel.
struct MouseSample {
    LONG x = 0;
    LONG y = 0;
    LONG wheel = 0;
    // Bit 0 is the left button, bit 4 is X2.
    ULONG buttons = 0;
};

namespace rawmouse {

// Returned by AcquireSlot when no accumulator is free.
constexpr int kInvalidSlot = -1;

// Starts the raw input thread. Safe to call repeatedly; only the first call
// does any work. Returns false when the thread or its message-only window
// could not be created, in which case the caller must keep using DirectInput.
bool Start();

// Claims an accumulator. Every device gets its own, because DirectInput gives
// every device its own copy of the movement: two devices reading one
// accumulator would take movement from each other, and the game would receive
// an arbitrary fraction of what the mouse reported.
int AcquireSlot();
void ReleaseSlot(int slot);

// Discards everything accumulated in one slot. Called when a device is
// acquired so that movement from before the acquisition is not delivered as
// one jump.
void Reset(int slot);

// Takes and clears the accumulated movement of one slot. Called once per
// frame by the DirectInput device proxy.
void Take(int slot, MouseSample* sample);

// Optional event signalled when new input arrives, for consumers that use
// IDirectInputDevice8::SetEventNotification. Pass nullptr to clear it.
void SetNotificationEvent(HANDLE event);


} // namespace rawmouse
