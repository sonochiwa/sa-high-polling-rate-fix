#include "raw_input.h"

#include "clock.h"
#include "legacy_messages.h"

namespace rawmouse {
namespace {

const wchar_t kWindowClassName[] = L"HighPollingRateFixRawInput";

// The window class the NVIDIA App overlay registers raw mouse input to while
// its in-game overlay is open. The name is a constant in nvspcap.dll, the
// part of the overlay that runs inside the game; the class is registered with
// the game's module handle, so the class name is the only thing that tells
// the window apart from any other in the process.
const wchar_t kDriverOverlayClassName[] =
    L"{F54CC444-0903-46F8-AF1E-105BE5E18F46}";
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

volatile LONG g_started = 0;
HANDLE g_thread = nullptr;
HWND g_window = nullptr;
HANDLE g_readyEvent = nullptr;
bool g_windowReady = false;
volatile LONG g_notificationEvent = 0;

// Written and read only on the raw input thread.
bool g_haveAbsolutePosition = false;
LONG g_lastAbsoluteX = 0;
LONG g_lastAbsoluteY = 0;

// INPUTSINK delivers packets whether or not the game has focus, so the focus
// rule DirectInput applied has to be reapplied here.
bool g_foreground = false;
LONGLONG g_lastForegroundCheckTicks = 0;
// Alt-tab does not need to be noticed faster than this.
constexpr LONGLONG kForegroundCheckIntervalMicros = 32000;

bool GameIsForeground() {
    LONGLONG now = 0;
    LONGLONG elapsed = clock::MicrosSince(g_lastForegroundCheckTicks, &now);
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

    legacy::TrackCursor();
    legacy::PostButtons(flags, wheel);
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
    // Generic desktop controls, mouse.
    device.usUsagePage = 0x01;
    device.usUsage = 0x02;
    // NOLEGACY and INPUTSINK go together or neither does: with a message-only
    // window as the target, NOLEGACY alone stops the raw stream arriving at
    // all, because a message-only window never holds focus and delivery is
    // focus-driven without INPUTSINK.
    device.dwFlags = RIDEV_NOLEGACY | RIDEV_INPUTSINK;
    device.hwndTarget = window;
    return RegisterRawInputDevices(&device, 1, sizeof(device)) != FALSE;
}

bool HeldByDriverOverlay(HWND target) {
    if (!target || !IsWindow(target) || !GetModuleHandleW(L"nvspcap.dll")) {
        return false;
    }
    wchar_t name[64] = {};
    GetClassNameW(target, name, static_cast<int>(sizeof(name) / sizeof(name[0])));
    return lstrcmpW(name, kDriverOverlayClassName) == 0;
}

// Windows keeps one raw mouse registration per process. Another plugin loaded
// after this one can take the registration away, which would leave the game
// with no mouse input at all, so it is checked once a second and reclaimed
// when it is gone.
//
// The NVIDIA App overlay is the exception. While it is open it moves the
// registration to a window of its own and checks once a second that it still
// has it; reclaiming it here made the two take it from each other every
// second, and the overlay's cursor stopped for a second at a time while the
// game's camera moved instead. The overlay hands the registration back when
// it closes, so it is simply left alone while it holds it.
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
                    (devices[index].hwndTarget == window ||
                     HeldByDriverOverlay(devices[index].hwndTarget))) {
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
                legacy::RefreshTargetMetrics();
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


} // namespace

bool Start() {
    if (InterlockedCompareExchange(&g_started, 1, 0) != 0) {
        return g_windowReady;
    }

    clock::Initialize();

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

} // namespace rawmouse
