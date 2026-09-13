#pragma once
#include "overlay_slots.h"
#include <wrl/client.h>
#include <array>
namespace single_overlay::native {
inline bool InsideLoader() noexcept {
    using Fn = BOOLEAN (NTAPI*)();
    static auto fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlIsThreadWithinLoaderCallout"));
    return !fn || fn();
}
inline bool PinInterface(IUnknown* object, size_t last) noexcept {
    if (!object || last > 64) return false;
    auto** table = *reinterpret_cast<void***>(object);
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(table), &owner)
        || !slots::ImageSlot(owner, table) || !slots::ImageSlot(owner, table + last)) return false;
    for (size_t i = 0; i <= last; ++i) {
        HMODULE callable = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(table[i]), &callable) || !slots::ImageEntry(callable, table[i])) return false;
    }
    return true;
}
inline bool SystemDxgiObject(IUnknown* object, size_t last) noexcept {
    wchar_t path[MAX_PATH]{};
    if (!object || !GetSystemDirectoryW(path, MAX_PATH) || wcscat_s(path, L"\\dxgi.dll")) return false;
    const HMODULE dxgi = GetModuleHandleW(path);
    auto** table = *reinterpret_cast<void***>(object);
    return slots::ImageSlot(dxgi, table) && slots::ImageSlot(dxgi, table + last) && PinInterface(object, last);
}
// NVIDIA and ReShade's public COM unwrapping contracts. No private object offsets,
// no replacement of an application's interface, and bounded cycle detection.
template<class T> bool Unwrap(IUnknown* input, Microsoft::WRL::ComPtr<T>& output) noexcept {
    using Microsoft::WRL::ComPtr;
    constexpr GUID baseGuid = {0xadec44e2,0x61f0,0x45c3,{0xad,0x9f,0x1b,0x37,0x37,0x92,0x84,0xff}};
    // ReShade 6.8.0 source/com_utils.hpp: IID_UnwrappedObject. The returned
    // original owns an AddRef, as with any successful QueryInterface call.
    constexpr GUID reshadeGuid = {0x7f2c9a11,0x3b4e,0x4d6a,{0x81,0x2f,0x5e,0x9c,0xd3,0x7a,0x1b,0x42}};
    output.Reset();
    ComPtr<IUnknown> current = input;
    std::array<ComPtr<IUnknown>, 4> visited;
    for (size_t i = 0; current && i < visited.size(); ++i) {
        if (!PinInterface(current.Get(), 2) || FAILED(current.As(&visited[i]))) return false;
        for (size_t j = 0; j < i; ++j) if (visited[j].Get() == visited[i].Get()) return false;
        ComPtr<IUnknown> base;
        HRESULT hr = current->QueryInterface(baseGuid, reinterpret_cast<void**>(base.GetAddressOf()));
        if (hr == E_NOINTERFACE) {
            if (base) return false;
            hr = current->QueryInterface(reshadeGuid, reinterpret_cast<void**>(base.GetAddressOf()));
        }
        if (hr == E_NOINTERFACE && base) return false;
        if (hr == E_NOINTERFACE) return SUCCEEDED(current.As(&output)) && output;
        if (hr != S_OK || !base) return false;
        current = base;
    }
    return false;
}
}
