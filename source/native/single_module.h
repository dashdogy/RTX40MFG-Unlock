#pragma once
#include <Windows.h>
#include <cstdint>
#include "single_module_status.h"

namespace single_module
{
HMODULE Self() noexcept;
bool OwnsBackend() noexcept;
HMODULE LoadSystemModule(const wchar_t* basename) noexcept;
void Log(const wchar_t* text) noexcept;
}

extern "C" BOOL WINAPI MfgUnlockCoreEntry(HINSTANCE, DWORD, LPVOID);
extern "C" __declspec(dllexport) BOOL WINAPI MfgUnlockSampleFrameTelemetry();
