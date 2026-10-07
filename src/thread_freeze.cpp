#include "thread_freeze.h"

#include <windows.h>

#include <cwchar>

namespace {

constexpr DWORD kWaitMs = 10000;

} // namespace

ThreadFreezeLock::ThreadFreezeLock() : mutex(nullptr), owned(false) {
    wchar_t name[64]{};
    swprintf_s(name, L"Local\\GtaSaMinHookFreeze-%lu", GetCurrentProcessId());
    mutex = CreateMutexW(nullptr, FALSE, name);
    if (mutex) {
        const DWORD wait = WaitForSingleObject(mutex, kWaitMs);
        owned = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
    }
}

ThreadFreezeLock::~ThreadFreezeLock() {
    if (owned) {
        ReleaseMutex(mutex);
    }
    if (mutex) {
        CloseHandle(mutex);
    }
}
