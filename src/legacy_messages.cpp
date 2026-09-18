#include "legacy_messages.h"

#include "clock.h"
#include "raw_input.h"

namespace legacy {
namespace {

volatile LONG g_targetWindow = 0;

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

HWND CurrentTargetWindow() {
    return reinterpret_cast<HWND>(InterlockedCompareExchange(&g_targetWindow, 0, 0));
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

} // namespace

void SetTargetWindow(HWND window) {
    InterlockedExchange(&g_targetWindow, static_cast<LONG>(reinterpret_cast<LONG_PTR>(window)));
    g_lastPostedX = -1;
    g_lastPostedY = -1;
    RefreshTargetMetrics();
}

HWND TargetWindow() {
    return CurrentTargetWindow();
}

void RefreshTargetMetrics() {
    HWND window = CurrentTargetWindow();
    if (!window || !IsWindow(window)) {
        return;
    }

    RECT client = {};
    if (GetClientRect(window, &client)) {
        g_clientWidth = client.right - client.left;
        g_clientHeight = client.bottom - client.top;
    }

    g_classWantsDoubleClicks = (GetClassLongPtrW(window, GCL_STYLE) & CS_DBLCLKS) != 0;
    g_doubleClickTime = GetDoubleClickTime();
    g_doubleClickWidth = GetSystemMetrics(SM_CXDOUBLECLK);
    g_doubleClickHeight = GetSystemMetrics(SM_CYDOUBLECLK);
}

// Called for every packet, so the work is kept out of it: the position is only
// read when the rate limit allows a post, and nothing is sent if the cursor
// has not actually moved since the last one.
void TrackCursor() {
    HWND window = CurrentTargetWindow();
    if (!window || g_clientWidth <= 0 || g_clientHeight <= 0) {
        return;
    }

    LONGLONG now = 0;
    LONGLONG elapsed = clock::MicrosSince(g_lastCursorPostTicks, &now);
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
                 MAKELPARAM(static_cast<WORD>(position.x), static_cast<WORD>(position.y)));
}

// The button messages go the same way as the cursor, and for the same reason:
// anything that listens for clicks rather than reading the DirectInput device
// would otherwise never see one. Transitions are rare, so each is posted as it
// happens with no rate limiting.
void PostButtons(USHORT flags, LONG wheel) {
    if (!flags) {
        return;
    }

    HWND window = CurrentTargetWindow();
    POINT cursor = {};
    if (!window || !CursorPosition(window, &cursor)) {
        return;
    }

    LPARAM position = MAKELPARAM(static_cast<WORD>(cursor.x), static_cast<WORD>(cursor.y));

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
        {RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, WM_LBUTTONDOWN, WM_LBUTTONUP, WM_LBUTTONDBLCLK,
         MK_LBUTTON, 0},
        {RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, WM_RBUTTONDOWN, WM_RBUTTONUP, WM_RBUTTONDBLCLK,
         MK_RBUTTON, 0},
        {RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, WM_MBUTTONDOWN, WM_MBUTTONUP, WM_MBUTTONDBLCLK,
         MK_MBUTTON, 0},
        {RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, WM_XBUTTONDOWN, WM_XBUTTONUP, WM_XBUTTONDBLCLK, MK_XBUTTON1,
         XBUTTON1},
        {RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, WM_XBUTTONDOWN, WM_XBUTTONUP, WM_XBUTTONDBLCLK, MK_XBUTTON2,
         XBUTTON2},
    };

    DWORD ticks = GetTickCount();

    for (int index = 0; index < kMouseButtonCount; ++index) {
        const ButtonMessage& button = kButtons[index];

        // Windows includes the button in wParam on the way down and excludes
        // it on the way up, so the flags are updated before each post.
        if (flags & button.downFlag) {
            g_mouseKeyFlags |= button.keyFlag;

            bool isDoubleClick = g_classWantsDoubleClicks && g_lastPressTicks[index] != 0 &&
                                 ticks - g_lastPressTicks[index] <= g_doubleClickTime &&
                                 Distance(cursor.x, g_lastPressPosition[index].x) <= g_doubleClickWidth / 2 &&
                                 Distance(cursor.y, g_lastPressPosition[index].y) <= g_doubleClickHeight / 2;

            WPARAM wParam = button.extraButton ? MAKEWPARAM(CurrentKeyFlags(), button.extraButton) : CurrentKeyFlags();
            PostMessageW(window, isDoubleClick ? button.doubleClickMessage : button.downMessage, wParam, position);

            // A third press must start a new pair rather than produce a second
            // double click.
            g_lastPressTicks[index] = isDoubleClick ? 0 : ticks;
            g_lastPressPosition[index] = cursor;
        }

        if (flags & button.upFlag) {
            g_mouseKeyFlags &= ~button.keyFlag;
            WPARAM wParam = button.extraButton ? MAKEWPARAM(CurrentKeyFlags(), button.extraButton) : CurrentKeyFlags();
            PostMessageW(window, button.upMessage, wParam, position);
        }
    }

    // WM_MOUSEWHEEL is the one that carries screen coordinates.
    if ((flags & RI_MOUSE_WHEEL) && wheel) {
        POINT point = cursor;
        if (ClientToScreen(window, &point)) {
            PostMessageW(window, WM_MOUSEWHEEL, MAKEWPARAM(CurrentKeyFlags(), static_cast<SHORT>(wheel)),
                         MAKELPARAM(static_cast<WORD>(point.x), static_cast<WORD>(point.y)));
        }
    }
}

} // namespace legacy
