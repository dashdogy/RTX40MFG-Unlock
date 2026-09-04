#pragma once

#include "reshade_bridge.h"

#include <Windows.h>

#include <cstdint>

struct ID3D12CommandQueue;
struct ID3D12Device;
struct IDXGISwapChain;
struct IUnknown;

// V3 is an exact fail-closed contract. Change the version whenever this table
// or the snapshot contract changes; clients must reject unknown versions.
constexpr uint32_t MFG_UNLOCK_BACKEND_ABI_V3 = 0x00030000u;

using MfgUnlockGetSnapshotFn = BOOL (WINAPI*)(MfgUnlockReShadeSnapshot*);
using MfgUnlockApplyControlFn = BOOL (WINAPI*)(uint32_t, BOOL, uint32_t,
    BOOL, BOOL);
using MfgUnlockRegisterD3D12DeviceFn = BOOL (WINAPI*)(ID3D12Device*);
using MfgUnlockRegisterD3D12QueueFn = BOOL (WINAPI*)(ID3D12CommandQueue*);
using MfgUnlockRegisterD3D12SwapchainFn = BOOL (WINAPI*)(
    IDXGISwapChain*, IUnknown*);
using MfgUnlockUnregisterD3D12SwapchainFn = BOOL (WINAPI*)(IDXGISwapChain*);
using MfgUnlockSetFrontendAttachedFn = void (WINAPI*)(BOOL);

struct MfgUnlockBackendInterface
{
    uint32_t structSize = sizeof(MfgUnlockBackendInterface);
    uint32_t abiVersion = MFG_UNLOCK_BACKEND_ABI_V3;
    MfgUnlockGetSnapshotFn getSnapshot = nullptr;
    MfgUnlockApplyControlFn applyControl = nullptr;
    MfgUnlockRegisterD3D12DeviceFn registerD3D12Device = nullptr;
    MfgUnlockRegisterD3D12QueueFn registerD3D12Queue = nullptr;
    MfgUnlockRegisterD3D12SwapchainFn registerD3D12Swapchain = nullptr;
    MfgUnlockUnregisterD3D12SwapchainFn unregisterD3D12Swapchain = nullptr;
    MfgUnlockSetFrontendAttachedFn setFrontendAttached = nullptr;
};

using MfgUnlockBackendQueryInterfaceFn = BOOL (WINAPI*)(
    uint32_t requestedAbiVersion, MfgUnlockBackendInterface* output,
    uint32_t outputSize);

#if defined(MFG_UNLOCK_BACKEND_BRIDGE)
extern "C" __declspec(dllexport) BOOL WINAPI
MfgUnlockBackendQueryInterface(uint32_t requestedAbiVersion,
    MfgUnlockBackendInterface* output, uint32_t outputSize);
#endif
