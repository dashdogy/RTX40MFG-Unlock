#pragma once

#include <Windows.h>

#include <cstdint>
#include <cwchar>
#include <string>

constexpr uint32_t kMfgUnlockStatusMagic = 0x5547464Du; // "MFGU"

struct MfgUnlockStatus
{
    uint32_t magic;
    volatile LONG state; // 0 pending, 1 success, -1 failure
    DWORD win32Error;
    LONG wrapperPatchCount;
    LONG ngxPatchCount;
    wchar_t logPath[MAX_PATH];
};

inline std::wstring MfgUnlockObjectName(const wchar_t* kind, DWORD pid)
{
    wchar_t buffer[96]{};
    swprintf_s(buffer, L"Local\\MfgUnlock%s-%lu", kind, static_cast<unsigned long>(pid));
    return buffer;
}
