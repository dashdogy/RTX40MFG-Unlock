#include "gpu_dispatch.h"
#include "fault_capture.h"
#include "gpu_selection_policy.h"
#include "ampere_backend.h"
#include <d3d12.h>
#include <atomic>
#include <mutex>

namespace gpu_dispatch
{
namespace
{
std::mutex gSelectionMutex;
Selection gSelection;
std::atomic<Family> gFamily{Family::eUnknown};
std::atomic<uint64_t> gLuid{0};
std::atomic<midpoint_fix::LogCallback> gLog{nullptr};

bool Publish(Family candidate, uint64_t luid) noexcept
{
    const Family previous = gSelection.family;
    const bool accepted = gSelection.Observe(candidate, luid);
    gLuid.store(gSelection.luid, std::memory_order_release);
    gFamily.store(gSelection.family, std::memory_order_release);
    if (previous != gSelection.family)
    {
        wchar_t message[192]{};
        swprintf_s(message, L"Unified GPU selection: family=%s adapterLuid=0x%016llX",
            gSelection.family == Family::eAda ? L"Ada (SM89)"
            : gSelection.family == Family::eAmpere ? L"Ampere (SM86, experimental)"
            : L"conflict; restart required", gSelection.luid);
        if (auto log = gLog.load(std::memory_order_acquire)) log(message);
    }
    return accepted;
}
bool DeviceIdentity(void* source, ID3D12Device*& device, uint64_t& luid) noexcept
{
    device = nullptr;
    luid = 0;
    if (!source) return false;
    __try
    {
        if (FAILED(static_cast<IUnknown*>(source)->QueryInterface(__uuidof(ID3D12Device),
                reinterpret_cast<void**>(&device))) || !device) return false;
        const LUID identity = device->GetAdapterLuid();
        luid = static_cast<uint64_t>(identity.LowPart)
            | (static_cast<uint64_t>(static_cast<uint32_t>(identity.HighPart)) << 32);
        return luid != 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}

Family Selected() noexcept { return gFamily.load(std::memory_order_acquire); }
bool IsAda() noexcept { return Selected() == Family::eAda; }
bool IsAmpere() noexcept { return Selected() == Family::eAmpere; }
uint64_t AdapterLuid() noexcept { return gLuid.load(std::memory_order_acquire); }
void SetLogCallback(midpoint_fix::LogCallback callback) noexcept
{
    gLog.store(callback, std::memory_order_release);
    midpoint_fix::SetLogCallback(callback);
    ampere_backend::SetLogCallback(callback);
}
bool ObserveD3D12Device(void* source) noexcept
{
    fault_capture::ObserveDevice(source);
    std::lock_guard lock(gSelectionMutex);
    ID3D12Device* device = nullptr;
    uint64_t luid = 0;
    const bool identity = DeviceIdentity(source, device, luid);
    if (!identity || !gSelection.Allows(luid))
    {
        if (device) device->Release();
        return Publish(Family::eUnknown, luid);
    }
    Family candidate = Family::eUnknown;
    if (gSelection.family != Family::eAmpere && midpoint_fix::ObserveD3D12Device(device))
        candidate = Family::eAda;
    else if (gSelection.family != Family::eAda && ampere_backend::ObserveD3D12Device(device))
        candidate = Family::eAmpere;
    device->Release();
    return Publish(candidate, luid);
}
bool ObserveVulkanPhysicalDevice(void* device) noexcept
{
    std::lock_guard lock(gSelectionMutex);
    if (gSelection.family == Family::eAmpere || gSelection.family == Family::eConflict)
        return false;
    const bool verified = midpoint_fix::ObserveVulkanPhysicalDevice(device);
    return Publish(verified ? Family::eAda : Family::eUnknown,
        verified ? midpoint_fix::AdapterLuid() : 0);
}
bool AdapterVerified() noexcept
{
    return IsAda() ? midpoint_fix::AdapterVerified()
        : IsAmpere() && ampere_backend::AdapterVerified();
}
bool Ready() noexcept
{
    return IsAda() ? midpoint_fix::Ready() : IsAmpere() && ampere_backend::Ready();
}
bool PatchProvider(HMODULE module, const wchar_t* path) noexcept
{
    return IsAda() ? midpoint_fix::PatchProvider(module, path)
        : IsAmpere() && ampere_backend::PatchProvider(module, path);
}
uint32_t FailureCode() noexcept
{
    return IsAda() ? midpoint_fix::FailureCode()
        : IsAmpere() ? ampere_backend::FailureCode()
        : Selected() == Family::eConflict ? 201u : 200u;
}
}
