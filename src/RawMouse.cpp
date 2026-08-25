#include "RawMouse.h"

namespace hprf {
namespace rawmouse {
namespace {

const wchar_t kWindowClassName[] = L"HighPollingRateFixRawInput";
constexpr UINT_PTR kMaintenanceTimerId = 1;
constexpr UINT kMaintenanceTimerInterval = 1000;

// More than two DirectInput mouse consumers in one process has never been
// observed; the extra slots exist so that an unexpected one still gets real
// input instead of falling back.
constexpr int kMaxDevices = 8;

// One accumulator per device. The raw input thread adds each packet to every
// live accumulator with interlocked operations, and each device takes and
// clears its own once per frame, so no two threads ever wait for each other.
// That is the whole point of the plugin: DirectInput serialises its own
// per-packet bookkeeping against the caller of GetDeviceState, and at 8000 Hz
// the game thread pays for it.
//
// Every device needs its own, because DirectInput gives every device its own
// copy of the movement. Sharing one would let two consumers take movement from
// each other, and the game would receive an arbitrary fraction of what the
// mouse reported.
//
// The accumulators are static and never freed. The raw input thread touches
// only this array, never a device object, so a device being destroyed while a
// packet is being distributed cannot leave the thread reading freed memory.
struct Accumulator {
    volatile LONG active;
    volatile LONG x;
    volatile LONG y;
    volatile LONG wheel;
    // Buttons currently held, and buttons seen down at any point since the
    // last Take(). The latch is what stops a click shorter than a frame from
    // being lost, which becomes likely once the device reports every 125
    // microseconds.
    volatile LONG buttonsDown;
    volatile LONG buttonsLatched;
};

Accumulator g_accumulators[kMaxDevices];

LONGLONG g_ticksPerSecond = 0;
volatile LONG g_started = 0;
HANDLE g_thread = nullptr;
HWND g_window = nullptr;
HANDLE g_readyEvent = nullptr;
bool g_windowReady = false;

// Written and read only on the raw input thread.
bool g_haveAbsolutePosition = false;
LONG g_lastAbsoluteX = 0;
LONG g_lastAbsoluteY = 0;

volatile LONG g_notificationEvent = 0;

// Legacy mouse message suppression.
//
// RIDEV_NOLEGACY is what actually removes the stutter: the per-packet work
// Windows does to generate WM_MOUSEMOVE and friends for this process costs
// more than the raw input read itself, and it is done under the desktop input
// lock, so it serialises against the game thread rather than merely competing
// with it for CPU.
//
// It cannot be used on its own here. With a message-only window as the target
// and no RIDEV_INPUTSINK, setting the flag stops the raw stream arriving at
// all: a click read back through the device returns 0x00 instead of 0x80. A
// message-only window never holds focus, and without INPUTSINK the delivery is
// focus-driven. Pairing the two makes delivery unconditional, and the focus
// rule the game needs is then applied here instead.
volatile LONG g_targetWindow = 0;
bool g_foreground = false;
LONGLONG g_lastForegroundCheckTicks = 0;

// The cursor position for the pause menu and the map. Suppressing legacy
// messages removes the WM_MOUSEMOVE they read it from, but it does not stop
// Windows moving the real cursor, so the position is read back from the system
// rather than integrated from the raw deltas here. Windows has already applied
// the pointer speed and the acceleration curve by then, so the menu feels the
// way the desktop does.
LONG g_clientWidth = 0;
LONG g_clientHeight = 0;
LONG g_lastPostedX = -1;
LONG g_lastPostedY = -1;
LONGLONG g_lastCursorPostTicks = 0;

// Double clicks are a message of their own, not something a listener derives
// from two WM_LBUTTONDOWNs, so suppressing legacy messages removes them too.
// Windows' rule is reproduced here: a second press counts as a double click
// when it lands inside a rectangle centred on the first one, within the
// double-click time, and only if the window class asked for double clicks with
// CS_DBLCLKS. A third press starts over.
bool g_classWantsDoubleClicks = false;
DWORD g_doubleClickTime = 500;
LONG g_doubleClickWidth = 4;
LONG g_doubleClickHeight = 4;
DWORD g_lastPressTicks[kMouseButtonCount] = {};
POINT g_lastPressPosition[kMouseButtonCount] = {};

// The MK_ flags Windows would have put in wParam, kept in step with the button
// transitions so the synthesised messages carry the state the real ones would.
WPARAM g_mouseKeyFlags = 0;

// 500 posts a second matches the frame rate this game reaches and is a small
// fraction of the packet rate that caused the problem. Nothing is posted at
// all unless the position actually changed.
constexpr LONGLONG kCursorPostIntervalMicros = 2000;
// Alt-tab does not need to be noticed faster than this.
constexpr LONGLONG kForegroundCheckIntervalMicros = 32000;

LONGLONG MicrosSince(LONGLONG ticks, LONGLONG* now) {
    LARGE_INTEGER counter = {};
    if (!g_ticksPerSecond || !QueryPerformanceCounter(&counter)) {
        return -1;
    }
    *now = counter.QuadPart;
    return (counter.QuadPart - ticks) * 1000000 / g_ticksPerSecond;
}

// INPUTSINK delivers packets whether or not the game has focus, so the focus
// rule DirectInput applied has to be reapplied here: input that arrives while
// the player is in another window must not turn the camera.
bool GameIsForeground() {
    LONGLONG now = 0;
    LONGLONG elapsed = MicrosSince(g_lastForegroundCheckTicks, &now);
    if (elapsed < 0) {
        return true;
    }
    if (g_lastForegroundCheckTicks &&
        elapsed < kForegroundCheckIntervalMicros) {
        return g_foreground;
    }
    g_lastForegroundCheckTicks = now;

    HWND foreground = GetForegroundWindow();
    DWORD processId = 0;
    g_foreground = foreground != nullptr &&
                   GetWindowThreadProcessId(foreground, &processId) != 0 &&
                   processId == GetCurrentProcessId();
    return g_foreground;
}

void RefreshTargetMetrics() {
    HWND window = reinterpret_cast<HWND>(
        InterlockedCompareExchange(&g_targetWindow, 0, 0));
    if (!window || !IsWindow(window)) {
        return;
    }

    RECT client = {};
    if (GetClientRect(window, &client)) {
        g_clientWidth = client.right - client.left;
        g_clientHeight = client.bottom - client.top;
    }

    g_classWantsDoubleClicks =
        (GetClassLongPtrW(window, GCL_STYLE) & CS_DBLCLKS) != 0;
    g_doubleClickTime = GetDoubleClickTime();
    g_doubleClickWidth = GetSystemMetrics(SM_CXDOUBLECLK);
    g_doubleClickHeight = GetSystemMetrics(SM_CYDOUBLECLK);
}

// GetAsyncKeyState rather than GetKeyState: this runs on the raw input thread,
// which has its own input queue and no idea what the game thread's keyboard
// state is.
WPARAM CurrentKeyFlags() {
    WPARAM flags = g_mouseKeyFlags;
    if (GetAsyncKeyState(VK_SHIFT) < 0) {
        flags |= MK_SHIFT;
    }
    if (GetAsyncKeyState(VK_CONTROL) < 0) {
        flags |= MK_CONTROL;
    }
    return flags;
}

LONG Distance(LONG a, LONG b) {
    LONG difference = a - b;
    return difference < 0 ? -difference : difference;
}

LONG Clamp(LONG value, LONG low, LONG high) {
    if (value < low) {
        return low;
    }
    return value > high ? high : value;
}

// Where Windows has put the cursor, in the game window's client coordinates.
bool CursorPosition(HWND window, POINT* client) {
    POINT point = {};
    if (!GetCursorPos(&point) || !ScreenToClient(window, &point)) {
        return false;
    }
    client->x = Clamp(point.x, 0, g_clientWidth > 0 ? g_clientWidth - 1 : 0);
    client->y = Clamp(point.y, 0, g_clientHeight > 0 ? g_clientHeight - 1 : 0);
    return true;
}

// Called for every packet, so the work is kept out of it: the position is only
// read when the rate limit allows a post, and nothing is sent if the cursor
// has not actually moved since the last one.
void TrackCursor() {
    HWND window = reinterpret_cast<HWND>(
        InterlockedCompareExchange(&g_targetWindow, 0, 0));
    if (!window || g_clientWidth <= 0 || g_clientHeight <= 0) {
        return;
    }

    LONGLONG now = 0;
    LONGLONG elapsed = MicrosSince(g_lastCursorPostTicks, &now);
    if (elapsed < 0) {
        return;
    }
    if (g_lastCursorPostTicks && elapsed < kCursorPostIntervalMicros) {
        return;
    }
    g_lastCursorPostTicks = now;

    POINT position = {};
    if (!CursorPosition(window, &position)) {
        return;
    }
    if (position.x == g_lastPostedX && position.y == g_lastPostedY) {
        return;
    }
    g_lastPostedX = position.x;
    g_lastPostedY = position.y;

    PostMessageW(window, WM_MOUSEMOVE, CurrentKeyFlags(),
                 MAKELPARAM(static_cast<WORD>(position.x),
                            static_cast<WORD>(position.y)));
}

// The button messages go the same way as the cursor, and for the same reason:
// anything that listens for clicks rather than reading the DirectInput device
// would otherwise never see one. Transitions are rare, so each is posted as it
// happens with no rate limiting.
void PostLegacyButtons(USHORT flags, LONG wheel) {
    if (!flags) {
        return;
    }

    HWND window = reinterpret_cast<HWND>(
        InterlockedCompareExchange(&g_targetWindow, 0, 0));
    POINT cursor = {};
    if (!window || !CursorPosition(window, &cursor)) {
        return;
    }

    LPARAM position = MAKELPARAM(static_cast<WORD>(cursor.x),
                                 static_cast<WORD>(cursor.y));

    struct ButtonMessage {
        USHORT downFlag;
        USHORT upFlag;
        UINT downMessage;
        UINT upMessage;
        UINT doubleClickMessage;
        WPARAM keyFlag;
        // Non-zero for the side buttons, which say which one they are in the
        // high word of wParam.
        WORD extraButton;
    };

    static const ButtonMessage kButtons[kMouseButtonCount] = {
        {RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, WM_LBUTTONDOWN,
         WM_LBUTTONUP, WM_LBUTTONDBLCLK, MK_LBUTTON, 0},
        {RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, WM_RBUTTONDOWN,
         WM_RBUTTONUP, WM_RBUTTONDBLCLK, MK_RBUTTON, 0},
        {RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP,
         WM_MBUTTONDOWN, WM_MBUTTONUP, WM_MBUTTONDBLCLK, MK_MBUTTON, 0},
        {RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, WM_XBUTTONDOWN,
         WM_XBUTTONUP, WM_XBUTTONDBLCLK, MK_XBUTTON1, XBUTTON1},
        {RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, WM_XBUTTONDOWN,
         WM_XBUTTONUP, WM_XBUTTONDBLCLK, MK_XBUTTON2, XBUTTON2},
    };

    DWORD ticks = GetTickCount();

    for (int index = 0; index < kMouseButtonCount; ++index) {
        const ButtonMessage& button = kButtons[index];

        // Windows includes the button in wParam on the way down and excludes
        // it on the way up, so the flags are updated before each post.
        if (flags & button.downFlag) {
            g_mouseKeyFlags |= button.keyFlag;

            bool isDoubleClick =
                g_classWantsDoubleClicks && g_lastPressTicks[index] != 0 &&
                ticks - g_lastPressTicks[index] <= g_doubleClickTime &&
                Distance(cursor.x, g_lastPressPosition[index].x) <=
                    g_doubleClickWidth / 2 &&
                Distance(cursor.y, g_lastPressPosition[index].y) <=
                    g_doubleClickHeight / 2;

            WPARAM wParam = button.extraButton
                                ? MAKEWPARAM(CurrentKeyFlags(),
                                             button.extraButton)
                                : CurrentKeyFlags();
            PostMessageW(window,
                         isDoubleClick ? button.doubleClickMessage
                                       : button.downMessage,
                         wParam, position);

            // A third press must start a new pair rather than produce a second
            // double click.
            g_lastPressTicks[index] = isDoubleClick ? 0 : ticks;
            g_lastPressPosition[index] = cursor;
        }

        if (flags & button.upFlag) {
            g_mouseKeyFlags &= ~button.keyFlag;
            WPARAM wParam = button.extraButton
                                ? MAKEWPARAM(CurrentKeyFlags(),
                                             button.extraButton)
                                : CurrentKeyFlags();
            PostMessageW(window, button.upMessage, wParam, position);
        }
    }

    // WM_MOUSEWHEEL is the one that carries screen coordinates.
    if ((flags & RI_MOUSE_WHEEL) && wheel) {
        POINT point = cursor;
        if (ClientToScreen(window, &point)) {
            PostMessageW(window, WM_MOUSEWHEEL,
                         MAKEWPARAM(CurrentKeyFlags(),
                                    static_cast<SHORT>(wheel)),
                         MAKELPARAM(static_cast<WORD>(point.x),
                                    static_cast<WORD>(point.y)));
        }
    }
}

void SignalNotification() {
    HANDLE event = reinterpret_cast<HANDLE>(
        InterlockedCompareExchange(&g_notificationEvent, 0, 0));
    if (event) {
        SetEvent(event);
    }
}

void AccumulateButton(Accumulator& slot, USHORT flags, USHORT downFlag,
                      USHORT upFlag, int bit) {
    if (flags & downFlag) {
        InterlockedOr(&slot.buttonsDown, 1L << bit);
        InterlockedOr(&slot.buttonsLatched, 1L << bit);
    }
    if (flags & upFlag) {
        InterlockedAnd(&slot.buttonsDown, ~(1L << bit));
    }
}

void AccumulateMouse(const RAWMOUSE& mouse) {
    // INPUTSINK keeps the packets coming while the player is in another
    // window. Dropping them here is what stops the camera turning behind their
    // back, and it is also the cheapest possible handling of a packet nobody
    // wants.
    if (!GameIsForeground()) {
        return;
    }

    LONG deltaX = 0;
    LONG deltaY = 0;

    if (mouse.usFlags & MOUSE_MOVE_ABSOLUTE) {
        // Tablets, touch digitisers and remote desktop sessions report an
        // absolute position in a normalised 0..65535 range. DirectInput hands
        // the game relative counts, so the position is differentiated here.
        bool virtualDesktop = (mouse.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
        int width = GetSystemMetrics(virtualDesktop ? SM_CXVIRTUALSCREEN
                                                    : SM_CXSCREEN);
        int height = GetSystemMetrics(virtualDesktop ? SM_CYVIRTUALSCREEN
                                                     : SM_CYSCREEN);
        LONG absoluteX = MulDiv(mouse.lLastX, width, 65535);
        LONG absoluteY = MulDiv(mouse.lLastY, height, 65535);
        if (g_haveAbsolutePosition) {
            deltaX = absoluteX - g_lastAbsoluteX;
            deltaY = absoluteY - g_lastAbsoluteY;
        }
        g_lastAbsoluteX = absoluteX;
        g_lastAbsoluteY = absoluteY;
        g_haveAbsolutePosition = true;
    } else {
        g_haveAbsolutePosition = false;
        deltaX = mouse.lLastX;
        deltaY = mouse.lLastY;
    }

    USHORT flags = mouse.usButtonFlags;
    LONG wheel = (flags & RI_MOUSE_WHEEL)
                     ? static_cast<SHORT>(mouse.usButtonData)
                     : 0;

    for (int index = 0; index < kMaxDevices; ++index) {
        Accumulator& slot = g_accumulators[index];
        if (!slot.active) {
            continue;
        }

        if (deltaX) {
            InterlockedExchangeAdd(&slot.x, deltaX);
        }
        if (deltaY) {
            InterlockedExchangeAdd(&slot.y, deltaY);
        }
        if (wheel) {
            InterlockedExchangeAdd(&slot.wheel, wheel);
        }
        if (flags) {
            AccumulateButton(slot, flags, RI_MOUSE_LEFT_BUTTON_DOWN,
                             RI_MOUSE_LEFT_BUTTON_UP, 0);
            AccumulateButton(slot, flags, RI_MOUSE_RIGHT_BUTTON_DOWN,
                             RI_MOUSE_RIGHT_BUTTON_UP, 1);
            AccumulateButton(slot, flags, RI_MOUSE_MIDDLE_BUTTON_DOWN,
                             RI_MOUSE_MIDDLE_BUTTON_UP, 2);
            AccumulateButton(slot, flags, RI_MOUSE_BUTTON_4_DOWN,
                             RI_MOUSE_BUTTON_4_UP, 3);
            AccumulateButton(slot, flags, RI_MOUSE_BUTTON_5_DOWN,
                             RI_MOUSE_BUTTON_5_UP, 4);
        }
    }

    TrackCursor();
    PostLegacyButtons(flags, wheel);
}

// Reads one WM_INPUT payload.
void ReadRawInput(HRAWINPUT handle) {
    alignas(8) BYTE storage[sizeof(RAWINPUT) + 16] = {};
    UINT size = sizeof(storage);
    UINT copied = GetRawInputData(handle, RID_INPUT, storage, &size,
                                  sizeof(RAWINPUTHEADER));
    if (copied == static_cast<UINT>(-1) || copied < sizeof(RAWINPUTHEADER)) {
        return;
    }

    const RAWINPUT* input = reinterpret_cast<const RAWINPUT*>(storage);
    if (input->header.dwType != RIM_TYPEMOUSE) {
        return;
    }

    AccumulateMouse(input->data.mouse);
}

bool RegisterForRawInput(HWND window) {
    RAWINPUTDEVICE device = {};
    device.usUsagePage = 0x01;  // Generic desktop controls.
    device.usUsage = 0x02;      // Mouse.
    // The two flags go together or neither does: see the note above.
    device.dwFlags = RIDEV_NOLEGACY | RIDEV_INPUTSINK;
    device.hwndTarget = window;
    return RegisterRawInputDevices(&device, 1, sizeof(device)) != FALSE;
}

// Windows keeps one raw mouse registration per process. Another plugin loaded
// after this one can take the registration away, which would leave the game
// with no mouse input at all, so it is checked once a second and reclaimed
// when it is gone.
void EnsureRegistration(HWND window) {
    UINT count = 0;
    if (GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE)) ==
            static_cast<UINT>(-1) &&
        GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return;
    }

    if (count != 0 && count <= 32) {
        RAWINPUTDEVICE devices[32] = {};
        UINT stored = count;
        if (GetRegisteredRawInputDevices(devices, &stored,
                                         sizeof(RAWINPUTDEVICE)) !=
            static_cast<UINT>(-1)) {
            for (UINT index = 0; index < stored; ++index) {
                if (devices[index].usUsagePage == 0x01 &&
                    devices[index].usUsage == 0x02 &&
                    devices[index].hwndTarget == window) {
                    return;
                }
            }
        }
    }

    RegisterForRawInput(window);
}

LRESULT CALLBACK RawInputWndProc(HWND window, UINT message, WPARAM wParam,
                                 LPARAM lParam) {
    switch (message) {
        case WM_INPUT:
            ReadRawInput(reinterpret_cast<HRAWINPUT>(lParam));
            return 0;
        case WM_TIMER:
            if (wParam == kMaintenanceTimerId) {
                EnsureRegistration(window);
                RefreshTargetMetrics();
                return 0;
            }
            break;
        default:
            break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

DWORD WINAPI RawInputThread(LPVOID) {
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &RawInputWndProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = kWindowClassName;
    RegisterClassExW(&windowClass);

    g_window = CreateWindowExW(0, kWindowClassName, L"", 0, 0, 0, 0, 0,
                               HWND_MESSAGE, nullptr, windowClass.hInstance,
                               nullptr);
    if (!g_window) {
        SetEvent(g_readyEvent);
        return 0;
    }

    if (!RegisterForRawInput(g_window)) {
        DestroyWindow(g_window);
        g_window = nullptr;
        SetEvent(g_readyEvent);
        return 0;
    }

    SetTimer(g_window, kMaintenanceTimerId, kMaintenanceTimerInterval, nullptr);

    g_windowReady = true;
    SetEvent(g_readyEvent);

    // The thread runs at normal priority on purpose. It wakes once per packet,
    // and above normal would make it preempt the game thread thousands of
    // times a second for a latency gain the game cannot observe, because it
    // reads the accumulator once per frame.
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_INPUT) {
            ReadRawInput(reinterpret_cast<HRAWINPUT>(message.lParam));
            // Drain the rest of the burst without going back through
            // GetMessage for each packet.
            MSG queued;
            while (PeekMessageW(&queued, nullptr, WM_INPUT, WM_INPUT,
                                PM_REMOVE)) {
                ReadRawInput(reinterpret_cast<HRAWINPUT>(queued.lParam));
            }
            SignalNotification();
            continue;
        }

        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return 0;
}

}  // namespace

bool Start() {
    if (InterlockedCompareExchange(&g_started, 1, 0) != 0) {
        return g_windowReady;
    }

    LARGE_INTEGER frequency = {};
    g_ticksPerSecond =
        QueryPerformanceFrequency(&frequency) ? frequency.QuadPart : 0;

    g_readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_readyEvent) {
        return false;
    }

    g_thread = CreateThread(nullptr, 0, &RawInputThread, nullptr, 0, nullptr);
    if (!g_thread) {
        CloseHandle(g_readyEvent);
        g_readyEvent = nullptr;
        return false;
    }

    WaitForSingleObject(g_readyEvent, 5000);
    return g_windowReady;
}

int AcquireSlot() {
    for (int index = 0; index < kMaxDevices; ++index) {
        if (InterlockedCompareExchange(&g_accumulators[index].active, 1, 0) ==
            0) {
            Reset(index);
            InterlockedExchange(&g_accumulators[index].buttonsDown, 0);
            return index;
        }
    }
    return kInvalidSlot;
}

void ReleaseSlot(int slot) {
    if (slot < 0 || slot >= kMaxDevices) {
        return;
    }
    InterlockedExchange(&g_accumulators[slot].active, 0);
}

void Reset(int slot) {
    if (slot < 0 || slot >= kMaxDevices) {
        return;
    }

    Accumulator& accumulator = g_accumulators[slot];
    InterlockedExchange(&accumulator.x, 0);
    InterlockedExchange(&accumulator.y, 0);
    InterlockedExchange(&accumulator.wheel, 0);
    InterlockedExchange(&accumulator.buttonsLatched, 0);
}

void Take(int slot, MouseSample* sample) {
    if (slot < 0 || slot >= kMaxDevices) {
        *sample = MouseSample();
        return;
    }

    Accumulator& accumulator = g_accumulators[slot];
    sample->x = InterlockedExchange(&accumulator.x, 0);
    sample->y = InterlockedExchange(&accumulator.y, 0);
    sample->wheel = InterlockedExchange(&accumulator.wheel, 0);

    // Held buttons are read before the latch is taken. A press that lands
    // between the two reads is still covered: either it is already visible in
    // the held state, or it stays in the latch for this call.
    LONG held = InterlockedCompareExchange(&accumulator.buttonsDown, 0, 0);
    LONG latched = InterlockedExchange(&accumulator.buttonsLatched, 0);
    sample->buttons = static_cast<ULONG>(held | latched);
}

void SetNotificationEvent(HANDLE event) {
    InterlockedExchange(&g_notificationEvent,
                        static_cast<LONG>(reinterpret_cast<LONG_PTR>(event)));
}

void SetTargetWindow(HWND window) {
    InterlockedExchange(&g_targetWindow,
                        static_cast<LONG>(reinterpret_cast<LONG_PTR>(window)));
    g_lastPostedX = -1;
    g_lastPostedY = -1;
    RefreshTargetMetrics();
}

}  // namespace rawmouse
}  // namespace hprf
