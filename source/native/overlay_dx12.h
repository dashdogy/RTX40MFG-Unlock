#pragma once
#include <Windows.h>
namespace single_overlay::dx12
{
void Install(HMODULE module) noexcept;
FARPROC Resolve(HMODULE module, LPCSTR name, FARPROC original) noexcept;
}
