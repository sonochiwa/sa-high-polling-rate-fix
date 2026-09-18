#include "mouse_device.h"

#include "legacy_messages.h"

namespace {

// c_dfDIMouse describes seven objects and c_dfDIMouse2 eleven. Checking the
// object count together with the state size keeps a private data format that
// happens to be the same size from being served with the standard layout.
constexpr DWORD kMouseObjectCount = 7;
constexpr DWORD kMouse2ObjectCount = 11;

bool IsStandardMouseFormat(LPCDIDATAFORMAT format) {
    if (!format || format->dwSize != sizeof(DIDATAFORMAT) ||
        (format->dwFlags & DIDF_ABSAXIS) != 0) {
        return false;
    }

    if (format->dwDataSize == sizeof(DIMOUSESTATE)) {
        return format->dwNumObjs == kMouseObjectCount;
    }
    if (format->dwDataSize == sizeof(DIMOUSESTATE2)) {
        return format->dwNumObjs == kMouse2ObjectCount;
    }
    return false;
}

DWORD ButtonCountForStateSize(DWORD stateSize) {
    DWORD available = stateSize == sizeof(DIMOUSESTATE) ? 4u : 8u;
    return available < kMouseButtonCount ? available : kMouseButtonCount;
}

} // namespace

RawMouseDevice::RawMouseDevice(IDirectInputDevice8A* inner, bool unicode)
    : m_inner(inner), m_unicode(unicode) {
    m_slot = rawmouse::AcquireSlot();
    if (m_slot == rawmouse::kInvalidSlot) {
        m_passthrough = true;
    }
}

RawMouseDevice::~RawMouseDevice() {
    ReleaseCursorClip();
    rawmouse::ReleaseSlot(m_slot);
    m_slot = rawmouse::kInvalidSlot;
    if (m_notificationEvent) {
        rawmouse::SetNotificationEvent(nullptr);
    }
    if (m_inner) {
        m_inner->Unacquire();
        m_inner->Release();
        m_inner = nullptr;
    }
}

HRESULT STDMETHODCALLTYPE RawMouseDevice::QueryInterface(REFIID riid,
                                                         LPVOID* object) {
    if (!object) {
        return E_POINTER;
    }

    // The ANSI and Unicode device interfaces have the same vtable layout and
    // differ only in the string types of the descriptive calls, all of which
    // are forwarded to the real device unchanged. Handing out this object for
    // either identifier is therefore safe, and the Unicode identifiers are
    // accepted only because a caller that created the Unicode DirectInput
    // object will ask for them.
    if (riid == IID_IUnknown || riid == IID_IDirectInputDeviceA ||
        riid == IID_IDirectInputDevice2A || riid == IID_IDirectInputDevice7A ||
        riid == IID_IDirectInputDevice8A || riid == IID_IDirectInputDeviceW ||
        riid == IID_IDirectInputDevice2W || riid == IID_IDirectInputDevice7W ||
        riid == IID_IDirectInputDevice8W) {
        AddRef();
        *object = this;
        return S_OK;
    }

    *object = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE RawMouseDevice::AddRef() {
    return static_cast<ULONG>(InterlockedIncrement(&m_references));
}

ULONG STDMETHODCALLTYPE RawMouseDevice::Release() {
    LONG remaining = InterlockedDecrement(&m_references);
    if (remaining == 0) {
        delete this;
        return 0;
    }
    return static_cast<ULONG>(remaining);
}

HRESULT STDMETHODCALLTYPE RawMouseDevice::GetCapabilities(LPDIDEVCAPS caps) {
    return m_inner->GetCapabilities(caps);
}

HRESULT STDMETHODCALLTYPE RawMouseDevice::EnumObjects(
    LPDIENUMDEVICEOBJECTSCALLBACKA callback, LPVOID ref, DWORD flags) {
    return m_inner->EnumObjects(callback, ref, flags);
}

HRESULT STDMETHODCALLTYPE RawMouseDevice::GetProperty(REFGUID guid,
                                                      LPDIPROPHEADER header) {
    if (&guid == &DIPROP_BUFFERSIZE && header &&
        header->dwSize == sizeof(DIPROPDWORD)) {
        reinterpret_cast<LPDIPROPDWORD>(header)->dwData = m_bufferSize;
        return DI_OK;
    }
    return m_inner->GetProperty(guid, header);
}

HRESULT STDMETHODCALLTYPE RawMouseDevice::SetProperty(REFGUID guid,
                                                      LPCDIPROPHEADER header) {
    if (&guid == &DIPROP_BUFFERSIZE) {
        if (!header || header->dwSize != sizeof(DIPROPDWORD)) {
            return DIERR_INVALIDPARAM;
        }
        m_bufferSize = reinterpret_cast<LPCDIPROPDWORD>(header)->dwData;
        m_bufferedCount = 0;
        m_bufferedRead = 0;
        return DI_OK;
    }

    if (&guid == &DIPROP_AXISMODE) {
        if (!header || header->dwSize != sizeof(DIPROPDWORD)) {
            return DIERR_INVALIDPARAM;
        }
        // Raw input reports movement, not a position, so absolute axis mode
        // cannot be served from the accumulator.
        DWORD mode = reinterpret_cast<LPCDIPROPDWORD>(header)->dwData;
        if (mode == DIPROPAXISMODE_ABS) {
            m_passthrough = true;
        }
    }

    return m_inner->SetProperty(guid, header);
}

HRESULT STDMETHODCALLTYPE RawMouseDevice::Acquire() {
    if (m_passthrough) {
        return m_inner->Acquire();
    }

    if (!m_dataFormatSet) {
        return DIERR_INVALIDPARAM;
    }
    if (m_acquired) {
        return S_FALSE;
    }

    rawmouse::Reset(m_slot);
    m_reportedButtons = 0;
    m_bufferedCount = 0;
    m_bufferedRead = 0;
    m_acquired = true;
    ApplyCursorClip();
    return DI_OK;
}

HRESULT STDMETHODCALLTYPE RawMouseDevice::Unacquire() {
    if (m_passthrough) {
        return m_inner->Unacquire();
    }

    ReleaseCursorClip();
    if (!m_acquired) {
        return DI_NOEFFECT;
    }
    m_acquired = false;
    return DI_OK;
}

HRESULT STDMETHODCALLTYPE RawMouseDevice::GetDeviceState(DWORD size,
                                                         LPVOID data) {
    if (m_passthrough) {
        return m_inner->GetDeviceState(size, data);
    }

    if (!data) {
        return E_POINTER;
    }
    if (!m_dataFormatSet || size != m_stateSize) {
        return DIERR_INVALIDPARAM;
    }
    if (!m_acquired) {
        return DIERR_NOTACQUIRED;
    }

    MouseSample sample;
    rawmouse::Take(m_slot, &sample);
    m_reportedButtons = sample.buttons;

    ZeroMemory(data, size);
    DIMOUSESTATE2* state = static_cast<DIMOUSESTATE2*>(data);
    state->lX = sample.x;
    state->lY = sample.y;
    // Raw input already reports the wheel in WHEEL_DELTA multiples, which is
    // the unit DirectInput uses for lZ.
    state->lZ = sample.wheel;

    DWORD buttons = ButtonCountForStateSize(m_stateSize);
    for (DWORD index = 0; index < buttons; ++index) {
        state->rgbButtons[index] =
            (sample.buttons & (1UL << index)) ? 0x80 : 0x00;
    }

    return DI_OK;
}

HRESULT RawMouseDevice::RefillBufferedData() {
    m_bufferedCount = 0;
    m_bufferedRead = 0;

    MouseSample sample;
    rawmouse::Take(m_slot, &sample);

    auto append = [this](DWORD offset, DWORD value) {
        if (m_bufferedCount >= kMaxBufferedEvents) {
            return;
        }
        DIDEVICEOBJECTDATA& entry = m_buffered[m_bufferedCount++];
        entry = {};
        entry.dwOfs = offset;
        entry.dwData = value;
        entry.dwTimeStamp = GetTickCount();
        entry.dwSequence = ++m_sequence;
    };

    if (sample.x) {
        append(DIMOFS_X, static_cast<DWORD>(sample.x));
    }
    if (sample.y) {
        append(DIMOFS_Y, static_cast<DWORD>(sample.y));
    }
    if (sample.wheel) {
        append(DIMOFS_Z, static_cast<DWORD>(sample.wheel));
    }

    DWORD buttons = ButtonCountForStateSize(m_stateSize);
    for (DWORD index = 0; index < buttons; ++index) {
        ULONG mask = 1UL << index;
        if ((sample.buttons & mask) == (m_reportedButtons & mask)) {
            continue;
        }
        append(DIMOFS_BUTTON0 + index, (sample.buttons & mask) ? 0x80 : 0x00);
    }
    m_reportedButtons = sample.buttons;

    return DI_OK;
}

HRESULT STDMETHODCALLTYPE RawMouseDevice::GetDeviceData(
    DWORD objectDataSize, LPDIDEVICEOBJECTDATA data, LPDWORD inOut,
    DWORD flags) {
    if (m_passthrough) {
        return m_inner->GetDeviceData(objectDataSize, data, inOut, flags);
    }

    if (!inOut) {
        return E_POINTER;
    }
    if (objectDataSize != sizeof(DIDEVICEOBJECTDATA) &&
        objectDataSize != sizeof(DIDEVICEOBJECTDATA_DX3)) {
        return DIERR_INVALIDPARAM;
    }
    if (!m_acquired) {
        return DIERR_NOTACQUIRED;
    }
    if (m_bufferSize == 0) {
        return DIERR_NOTBUFFERED;
    }

    if (m_bufferedRead >= m_bufferedCount) {
        RefillBufferedData();
    }

    DWORD available = m_bufferedCount - m_bufferedRead;
    DWORD requested = *inOut;

    if (!data) {
        // A null buffer means the caller is flushing or counting.
        *inOut = requested == INFINITE ? available
                                       : (requested < available ? requested
                                                                : available);
        if ((flags & DIGDD_PEEK) == 0) {
            m_bufferedRead += *inOut;
        }
        return DI_OK;
    }

    DWORD copied = requested < available ? requested : available;
    for (DWORD index = 0; index < copied; ++index) {
        memcpy(reinterpret_cast<BYTE*>(data) + index * objectDataSize,
               &m_buffered[m_bufferedRead + index], objectDataSize);
    }
    if ((flags & DIGDD_PEEK) == 0) {
        m_bufferedRead += copied;
    }
    *inOut = copied;
    return DI_OK;
}

HRESULT STDMETHODCALLTYPE RawMouseDevice::SetDataFormat(
    LPCDIDATAFORMAT format) {
    HRESULT result = m_inner->SetDataFormat(format);
    if (FAILED(result)) {
        return result;
    }

    if (IsStandardMouseFormat(format)) {
        m_stateSize = format->dwDataSize;
        m_dataFormatSet = true;
    } else {
        m_passthrough = true;
    }

    return result;
}

HRESULT STDMETHODCALLTYPE RawMouseDevice::SetEventNotification(HANDLE event) {
    m_notificationEvent = event;
    rawmouse::SetNotificationEvent(event);
    return m_inner->SetEventNotification(event);
}

HRESULT STDMETHODCALLTYPE RawMouseDevice::SetCooperativeLevel(HWND window,
                                                              DWORD flags) {
    HRESULT result = m_inner->SetCooperativeLevel(window, flags);
    if (FAILED(result)) {
        return result;
    }

    m_window = window;
    m_exclusive = (flags & DISCL_EXCLUSIVE) != 0;
    legacy::SetTargetWindow(window);
    if (m_acquired) {
        ReleaseCursorClip();
        ApplyCursorClip();
    }
    return result;
}

HRESULT STDMETHODCALLTYPE RawMouseDevice::GetObjectInfo(
    LPDIDEVICEOBJECTINSTANCEA instance, DWORD object, DWORD how) {
    return m_inner->GetObjectInfo(instance, object, how);
}

HRESULT STDMETHODCALLTYPE RawMouseDevice::GetDeviceInfo(
    LPDIDEVICEINSTANCEA instance) {
    return m_inner->GetDeviceInfo(instance);
}

HRESULT STDMETHODCALLTYPE RawMouseDevice::RunControlPanel(HWND owner,
                                                          DWORD flags) {
    return m_inner->RunControlPanel(owner, flags);
}

HRESULT STDMETHODCALLTYPE RawMouseDevice::Initialize(HINSTANCE instance,
                                                     DWORD version,
                                                     REFGUID guid) {
    return m_inner->Initialize(instance, version, guid);
}

HRESULT STDMETHODCALLTYPE RawMouseDevice::CreateEffect(
    REFGUID guid, LPCDIEFFECT effect, LPDIRECTINPUTEFFECT* created,
    LPUNKNOWN outer) {
    return m_inner->CreateEffect(guid, effect, created, outer);
}

HRESULT STDMETHODCALLTYPE RawMouseDevice::EnumEffects(
    LPDIENUMEFFECTSCALLBACKA callback, LPVOID ref, DWORD type) {
    return m_inner->EnumEffects(callback, ref, type);
}

HRESULT STDMETHODCALLTYPE RawMouseDevice::GetEffectInfo(LPDIEFFECTINFOA info,
                                                        REFGUID guid) {
    return m_inner->GetEffectInfo(info, guid);
}

HRESULT STDMETHODCALLTYPE RawMouseDevice::GetForceFeedbackState(
    LPDWORD state) {
    return m_inner->GetForceFeedbackState(state);
}

HRESULT STDMETHODCALLTYPE RawMouseDevice::SendForceFeedbackCommand(
    DWORD command) {
    return m_inner->SendForceFeedbackCommand(command);
}

HRESULT STDMETHODCALLTYPE RawMouseDevice::EnumCreatedEffectObjects(
    LPDIENUMCREATEDEFFECTOBJECTSCALLBACK callback, LPVOID ref, DWORD flags) {
    return m_inner->EnumCreatedEffectObjects(callback, ref, flags);
}

HRESULT STDMETHODCALLTYPE RawMouseDevice::Escape(LPDIEFFESCAPE escape) {
    return m_inner->Escape(escape);
}

HRESULT STDMETHODCALLTYPE RawMouseDevice::Poll() {
    if (m_passthrough) {
        return m_inner->Poll();
    }
    if (!m_acquired) {
        return DIERR_NOTACQUIRED;
    }
    // A mouse is not a polled device; the accumulator is always current.
    return DI_NOEFFECT;
}

HRESULT STDMETHODCALLTYPE RawMouseDevice::SendDeviceData(
    DWORD objectDataSize, LPCDIDEVICEOBJECTDATA data, LPDWORD inOut,
    DWORD flags) {
    return m_inner->SendDeviceData(objectDataSize, data, inOut, flags);
}

HRESULT STDMETHODCALLTYPE RawMouseDevice::EnumEffectsInFile(
    LPCSTR fileName, LPDIENUMEFFECTSINFILECALLBACK callback, LPVOID ref,
    DWORD flags) {
    return m_inner->EnumEffectsInFile(fileName, callback, ref, flags);
}

HRESULT STDMETHODCALLTYPE RawMouseDevice::WriteEffectToFile(
    LPCSTR fileName, DWORD entries, LPDIFILEEFFECT fileEffect, DWORD flags) {
    return m_inner->WriteEffectToFile(fileName, entries, fileEffect, flags);
}

HRESULT STDMETHODCALLTYPE RawMouseDevice::BuildActionMap(
    LPDIACTIONFORMATA format, LPCSTR userName, DWORD flags) {
    return m_inner->BuildActionMap(format, userName, flags);
}

HRESULT STDMETHODCALLTYPE RawMouseDevice::SetActionMap(
    LPDIACTIONFORMATA format, LPCSTR userName, DWORD flags) {
    // Action mapping drives the device through DirectInput's own data path,
    // which the accumulator cannot serve.
    m_passthrough = true;
    return m_inner->SetActionMap(format, userName, flags);
}

HRESULT STDMETHODCALLTYPE RawMouseDevice::GetImageInfo(
    LPDIDEVICEIMAGEINFOHEADERA header) {
    return m_inner->GetImageInfo(header);
}

void RawMouseDevice::ApplyCursorClip() {
    // DirectInput confines the cursor to the window while an exclusive mouse
    // device is acquired. The real device is never acquired here, so the same
    // confinement is applied directly; without it a click in exclusive
    // fullscreen could land outside the game.
    if (!m_exclusive || !m_window || m_cursorClipped) {
        return;
    }
    if (!IsWindow(m_window) || GetForegroundWindow() != m_window) {
        return;
    }

    RECT client = {};
    if (!GetClientRect(m_window, &client)) {
        return;
    }

    POINT topLeft = {client.left, client.top};
    POINT bottomRight = {client.right, client.bottom};
    if (!ClientToScreen(m_window, &topLeft) ||
        !ClientToScreen(m_window, &bottomRight)) {
        return;
    }

    RECT screen = {topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
    if (ClipCursor(&screen)) {
        m_cursorClipped = true;
    }
}

void RawMouseDevice::ReleaseCursorClip() {
    if (!m_cursorClipped) {
        return;
    }
    ClipCursor(nullptr);
    m_cursorClipped = false;
}
