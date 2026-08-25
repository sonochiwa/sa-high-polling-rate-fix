#pragma once

#include <windows.h>
#include <dinput.h>

#include "RawMouse.h"

namespace hprf {

// A DirectInput 8 mouse device that serves the game from the raw input
// accumulator instead of from DirectInput.
//
// The real device is still created and kept, but it is never acquired, so
// dinput8 never processes a single packet. Everything that only describes the
// device - capabilities, object info, device info - is answered by forwarding
// to it, which keeps the reported device identical to the one the game would
// have talked to.
//
// If the caller asks for a data format this class cannot serve, it acquires
// the real device and forwards the input calls as well. That path costs
// exactly what the unpatched game costs, and nothing behaves differently.
class RawMouseDevice final : public IDirectInputDevice8A {
public:
    RawMouseDevice(IDirectInputDevice8A* inner, bool unicode);

    /*** IUnknown ***/
    STDMETHOD(QueryInterface)(REFIID riid, LPVOID* object) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;

    /*** IDirectInputDevice8A ***/
    STDMETHOD(GetCapabilities)(LPDIDEVCAPS caps) override;
    STDMETHOD(EnumObjects)(LPDIENUMDEVICEOBJECTSCALLBACKA callback, LPVOID ref,
                           DWORD flags) override;
    STDMETHOD(GetProperty)(REFGUID guid, LPDIPROPHEADER header) override;
    STDMETHOD(SetProperty)(REFGUID guid, LPCDIPROPHEADER header) override;
    STDMETHOD(Acquire)() override;
    STDMETHOD(Unacquire)() override;
    STDMETHOD(GetDeviceState)(DWORD size, LPVOID data) override;
    STDMETHOD(GetDeviceData)(DWORD objectDataSize, LPDIDEVICEOBJECTDATA data,
                             LPDWORD inOut, DWORD flags) override;
    STDMETHOD(SetDataFormat)(LPCDIDATAFORMAT format) override;
    STDMETHOD(SetEventNotification)(HANDLE event) override;
    STDMETHOD(SetCooperativeLevel)(HWND window, DWORD flags) override;
    STDMETHOD(GetObjectInfo)(LPDIDEVICEOBJECTINSTANCEA instance, DWORD object,
                             DWORD how) override;
    STDMETHOD(GetDeviceInfo)(LPDIDEVICEINSTANCEA instance) override;
    STDMETHOD(RunControlPanel)(HWND owner, DWORD flags) override;
    STDMETHOD(Initialize)(HINSTANCE instance, DWORD version,
                          REFGUID guid) override;
    STDMETHOD(CreateEffect)(REFGUID guid, LPCDIEFFECT effect,
                            LPDIRECTINPUTEFFECT* created,
                            LPUNKNOWN outer) override;
    STDMETHOD(EnumEffects)(LPDIENUMEFFECTSCALLBACKA callback, LPVOID ref,
                           DWORD type) override;
    STDMETHOD(GetEffectInfo)(LPDIEFFECTINFOA info, REFGUID guid) override;
    STDMETHOD(GetForceFeedbackState)(LPDWORD state) override;
    STDMETHOD(SendForceFeedbackCommand)(DWORD command) override;
    STDMETHOD(EnumCreatedEffectObjects)(
        LPDIENUMCREATEDEFFECTOBJECTSCALLBACK callback, LPVOID ref,
        DWORD flags) override;
    STDMETHOD(Escape)(LPDIEFFESCAPE escape) override;
    STDMETHOD(Poll)() override;
    STDMETHOD(SendDeviceData)(DWORD objectDataSize,
                              LPCDIDEVICEOBJECTDATA data, LPDWORD inOut,
                              DWORD flags) override;
    STDMETHOD(EnumEffectsInFile)(LPCSTR fileName,
                                 LPDIENUMEFFECTSINFILECALLBACK callback,
                                 LPVOID ref, DWORD flags) override;
    STDMETHOD(WriteEffectToFile)(LPCSTR fileName, DWORD entries,
                                 LPDIFILEEFFECT fileEffect,
                                 DWORD flags) override;
    STDMETHOD(BuildActionMap)(LPDIACTIONFORMATA format, LPCSTR userName,
                              DWORD flags) override;
    STDMETHOD(SetActionMap)(LPDIACTIONFORMATA format, LPCSTR userName,
                            DWORD flags) override;
    STDMETHOD(GetImageInfo)(LPDIDEVICEIMAGEINFOHEADERA header) override;

private:
    ~RawMouseDevice();

    void ApplyCursorClip();
    void ReleaseCursorClip();
    HRESULT RefillBufferedData();

    // Buffered mode reports one event per axis and one per button change for
    // each frame, so a burst of packets between two reads is delivered as the
    // movement it adds up to.
    static constexpr DWORD kMaxBufferedEvents = 3 + kMouseButtonCount;

    LONG m_references = 1;
    IDirectInputDevice8A* m_inner = nullptr;
    bool m_unicode = false;
    bool m_passthrough = false;
    bool m_acquired = false;
    bool m_exclusive = false;
    bool m_dataFormatSet = false;
    bool m_cursorClipped = false;
    int m_slot = rawmouse::kInvalidSlot;
    DWORD m_stateSize = 0;
    DWORD m_bufferSize = 0;
    DWORD m_sequence = 0;
    ULONG m_reportedButtons = 0;
    HWND m_window = nullptr;
    HANDLE m_notificationEvent = nullptr;

    DIDEVICEOBJECTDATA m_buffered[kMaxBufferedEvents] = {};
    DWORD m_bufferedCount = 0;
    DWORD m_bufferedRead = 0;
};

}  // namespace hprf
