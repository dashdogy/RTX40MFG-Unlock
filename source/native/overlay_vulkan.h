#pragma once
#include <Windows.h>
namespace single_overlay::vulkan
{
void Install(HMODULE module) noexcept;
FARPROC Resolve(HMODULE module, LPCSTR name, FARPROC original) noexcept;
}
