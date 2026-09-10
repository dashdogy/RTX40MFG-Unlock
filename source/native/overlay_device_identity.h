#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <array>

namespace single_overlay::dx12
{
// Streamline's documented QueryInterface contract returns an AddRef'd base
// interface. Its queue can expose a proxy device while its swapchain exposes
// the native device. Compare their native IUnknown identities without replacing
// the application's queue, device, swapchain, or lifecycle ownership.
inline bool DeviceIdentity(ID3D12Device* device,
    Microsoft::WRL::ComPtr<IUnknown>& identity) noexcept
{
    using Microsoft::WRL::ComPtr;
    constexpr GUID retrieveBase = {0xadec44e2, 0x61f0, 0x45c3,
        {0xad, 0x9f, 0x1b, 0x37, 0x37, 0x92, 0x84, 0xff}};
    identity.Reset();
    ComPtr<ID3D12Device> current = device;
    std::array<ComPtr<IUnknown>, 4> visited;
    for (size_t depth = 0; current && depth < visited.size(); ++depth)
    {
        if (FAILED(current.As(&visited[depth])) || !visited[depth]) return false;
        for (size_t previous = 0; previous < depth; ++previous)
            if (visited[previous].Get() == visited[depth].Get()) return false;
        ComPtr<IUnknown> base;
        const HRESULT result = current->QueryInterface(retrieveBase,
            reinterpret_cast<void**>(base.GetAddressOf()));
        if (result == E_NOINTERFACE)
        {
            identity = visited[depth];
            return true;
        }
        if (result != S_OK || !base) return false;
        ComPtr<ID3D12Device> native;
        if (FAILED(base.As(&native)) || !native) return false;
        current = native;
    }
    return false;
}
}
