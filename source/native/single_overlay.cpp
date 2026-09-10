#include "single_overlay.h"
#include "overlay_platform.h"
#include "overlay_dx12.h"
#include "overlay_vulkan.h"
#include <array>
#include <cwchar>
#include <cstring>

namespace single_overlay
{
namespace
{
bool ModuleName(HMODULE module, const wchar_t* expected) noexcept
{
    if (!module) return false;
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
    if (!length || length >= std::size(path)) return false;
    const wchar_t* slash = wcsrchr(path, L'\\');
    return _wcsicmp(slash ? slash+1 : path, expected) == 0;
}
}

void InstallKnownModules() noexcept
{
    if (!single_module::OwnsBackend()) return;
    InternalScope internal;
    if (HMODULE dxgi = GetModuleHandleW(L"dxgi.dll")) dx12::Install(dxgi);
    wchar_t system[32768]{};
    const UINT length = GetSystemDirectoryW(system, static_cast<UINT>(std::size(system)));
    if (length && length < std::size(system)-32 && !wcscat_s(system, L"\\dxgi.dll"))
        if (HMODULE dxgi = GetModuleHandleW(system)) dx12::Install(dxgi);
    if (HMODULE vk = GetModuleHandleW(L"vulkan-1.dll")) vulkan::Install(vk);
    InstallInputHooks();
}

FARPROC ResolveProc(HMODULE module, LPCSTR name, FARPROC original) noexcept
{
    if (gInsideOverlay || !single_module::OwnsBackend() || !name
        || reinterpret_cast<uintptr_t>(name) <= 0xFFFFu || !original) return original;
    if (strncmp(name, "CreateDXGIFactory", 17) == 0 && ModuleName(module, L"dxgi.dll"))
        return dx12::Resolve(module, name, original);
    if (name[0] == 'v' && name[1] == 'k' && ModuleName(module, L"vulkan-1.dll"))
        return vulkan::Resolve(module, name, original);
    return original;
}
}
