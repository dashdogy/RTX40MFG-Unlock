#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include "overlay_native.h"

namespace single_overlay::dx12
{
// Streamline and ReShade QueryInterface contracts return an AddRef'd base
// interface. Its queue can expose a proxy device while its swapchain exposes
// the native device. Compare their native IUnknown identities without replacing
// the application's queue, device, swapchain, or lifecycle ownership.
inline bool DeviceIdentity(ID3D12Device* device,
    Microsoft::WRL::ComPtr<IUnknown>& identity) noexcept
{
    using Microsoft::WRL::ComPtr;
    identity.Reset();
    ComPtr<ID3D12Device> unwrapped;
    return native::Unwrap(device, unwrapped)
        && SUCCEEDED(unwrapped.As(&identity)) && identity;
}
}
