#include <windows.h>
#include <dinput.h>

#include <new>

#include "MinHook.h"
#include "MouseDevice.h"
#include "RawMouse.h"

namespace hprf {
namespace {

constexpr int kVtableCreateDevice = 3;

using DirectInput8CreateFn = HRESULT(WINAPI*)(HINSTANCE instance,
                                              DWORD version, REFIID riid,
                                              LPVOID* out, LPUNKNOWN outer);
using CreateDeviceAFn = HRESULT(STDMETHODCALLTYPE*)(
    IDirectInput8A* self, REFGUID guid, LPDIRECTINPUTDEVICE8A* device,
    LPUNKNOWN outer);
using CreateDeviceWFn = HRESULT(STDMETHODCALLTYPE*)(
    IDirectInput8W* self, REFGUID guid, LPDIRECTINPUTDEVICE8W* device,
    LPUNKNOWN outer);

DirectInput8CreateFn g_originalDirectInput8Create = nullptr;
CreateDeviceAFn g_originalCreateDeviceA = nullptr;
CreateDeviceWFn g_originalCreateDeviceW = nullptr;

// Wraps a freshly created system mouse device. The real device is kept alive
// inside the proxy, so the reference the caller was given is transferred, not
// released. If anything is unavailable the caller keeps the unmodified
// DirectInput device and the game behaves exactly as it would without the
// plugin.
void* WrapMouseDevice(void* device, bool unicode) {
    if (!rawmouse::Start()) {
        return device;
    }

    auto* inner = static_cast<IDirectInputDevice8A*>(device);
    auto* proxy = new (std::nothrow) RawMouseDevice(inner, unicode);
    if (!proxy) {
        return device;
    }

    return static_cast<IDirectInputDevice8A*>(proxy);
}

bool ShouldWrap(REFGUID guid, LPUNKNOWN outer) {
    // Aggregation is not supported by the proxy, and only the system mouse is
    // interesting; keyboards and game controllers report at rates that never
    // caused the problem this plugin exists for.
    return outer == nullptr && IsEqualGUID(guid, GUID_SysMouse);
}

HRESULT STDMETHODCALLTYPE HookedCreateDeviceA(IDirectInput8A* self,
                                              REFGUID guid,
                                              LPDIRECTINPUTDEVICE8A* device,
                                              LPUNKNOWN outer) {
    HRESULT result = g_originalCreateDeviceA(self, guid, device, outer);
    if (FAILED(result) || !device || !*device || !ShouldWrap(guid, outer)) {
        return result;
    }

    *device =
        static_cast<LPDIRECTINPUTDEVICE8A>(WrapMouseDevice(*device, false));
    return result;
}

HRESULT STDMETHODCALLTYPE HookedCreateDeviceW(IDirectInput8W* self,
                                              REFGUID guid,
                                              LPDIRECTINPUTDEVICE8W* device,
                                              LPUNKNOWN outer) {
    HRESULT result = g_originalCreateDeviceW(self, guid, device, outer);
    if (FAILED(result) || !device || !*device || !ShouldWrap(guid, outer)) {
        return result;
    }

    *device =
        static_cast<LPDIRECTINPUTDEVICE8W>(WrapMouseDevice(*device, true));
    return result;
}

void* VtableEntry(void* object, int index) {
    void** vtable = *reinterpret_cast<void***>(object);
    return vtable[index];
}

void HookCreateDevice(void* object, bool unicode) {
    if (unicode ? g_originalCreateDeviceW != nullptr
                : g_originalCreateDeviceA != nullptr) {
        return;
    }

    void* target = VtableEntry(object, kVtableCreateDevice);
    void* detour = unicode ? reinterpret_cast<void*>(&HookedCreateDeviceW)
                           : reinterpret_cast<void*>(&HookedCreateDeviceA);
    void* original = nullptr;

    if (MH_CreateHook(target, detour, &original) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        return;
    }

    if (unicode) {
        g_originalCreateDeviceW = reinterpret_cast<CreateDeviceWFn>(original);
    } else {
        g_originalCreateDeviceA = reinterpret_cast<CreateDeviceAFn>(original);
    }
}

HRESULT WINAPI HookedDirectInput8Create(HINSTANCE instance, DWORD version,
                                        REFIID riid, LPVOID* out,
                                        LPUNKNOWN outer) {
    HRESULT result =
        g_originalDirectInput8Create(instance, version, riid, out, outer);
    if (FAILED(result) || !out || !*out) {
        return result;
    }

    if (IsEqualIID(riid, IID_IDirectInput8A)) {
        HookCreateDevice(*out, false);
    } else if (IsEqualIID(riid, IID_IDirectInput8W)) {
        HookCreateDevice(*out, true);
    }

    return result;
}

DWORD WINAPI Initialize(LPVOID) {
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        return 0;
    }

    HMODULE dinput8 = LoadLibraryW(L"dinput8.dll");
    if (!dinput8) {
        return 0;
    }

    auto create = reinterpret_cast<DirectInput8CreateFn>(
        GetProcAddress(dinput8, "DirectInput8Create"));
    if (!create) {
        return 0;
    }

    if (MH_CreateHook(reinterpret_cast<void*>(create),
                      reinterpret_cast<void*>(&HookedDirectInput8Create),
                      reinterpret_cast<void**>(&g_originalDirectInput8Create)) ==
        MH_OK) {
        MH_EnableHook(reinterpret_cast<void*>(create));
    }
    return 0;
}

// The plugin hooks a dinput8 export and a dinput8 vtable entry, and hands the
// game a device object that lives in this module. None of that can be
// withdrawn safely once the game holds the device, so the module is pinned and
// a FreeLibrary on it becomes a no-op.
void PinSelf() {
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN |
                           GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(&PinSelf), &pinned);
}

}  // namespace
}  // namespace hprf

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) {
        return TRUE;
    }

    DisableThreadLibraryCalls(instance);
    hprf::PinSelf();

    // DllMain runs under the loader lock and the initialization below loads
    // dinput8 and starts a thread, so it is moved off this call.
    HANDLE thread =
        CreateThread(nullptr, 0, &hprf::Initialize, nullptr, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    }

    return TRUE;
}
