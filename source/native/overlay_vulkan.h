#pragma once
#include <Windows.h>
namespace single_overlay::vulkan
{
// Public import/resolver gateways only. Never detour the loader or driver.
FARPROC Resolve(HMODULE module, LPCSTR name, FARPROC original) noexcept;
}
