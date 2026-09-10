#pragma once
#include "entry_detour.h"
#include <array>
#include <atomic>
#include <mutex>
#include <cstring>

namespace ngx_initialization
{
using Prepare = void(*)(void* device, const entry_detour::Snapshot& entry,
    const void* caller, bool firstInit) noexcept;
inline std::atomic<Prepare> gPrepare{};
struct Owner { HMODULE module{}; uint64_t generation{}; bool initObserved = false;
    std::array<entry_detour::Handle, 3> entries{}; };
inline std::array<Owner, 8> gOwners{};
inline std::mutex gInstall;
inline std::recursive_mutex gBoundaryCalls;
inline thread_local bool gPreparing = false;
inline thread_local uint64_t gPreparationEpoch = 0;
inline std::atomic<uint64_t> gEntryEpoch{0};
inline uint64_t NextEntryEpoch() noexcept
{
    uint64_t value = gEntryEpoch.load(std::memory_order_acquire);
    do { if (value == UINT64_MAX) return 0; }
    while (!gEntryEpoch.compare_exchange_weak(value, value + 1,
        std::memory_order_acq_rel, std::memory_order_acquire));
    return value + 1;
}
inline uint64_t PreparationTicket() noexcept
{
    return gPreparing && gPreparationEpoch && gPreparationEpoch != UINT64_MAX
        && gEntryEpoch.load(std::memory_order_acquire) == gPreparationEpoch ? gPreparationEpoch : 0;
}
inline constexpr std::array<const char*, 3> kNames{
    "NVSDK_NGX_D3D12_Init", "NVSDK_NGX_D3D12_Init_Ext", "NVSDK_NGX_D3D12_Init_ProjectID"};
inline bool IsEntry(const char* name) noexcept
{
    if (!name) return false;
    for (const auto* entry : kNames) if (!strcmp(name, entry)) return true;
    return false;
}
inline void InvokePreparation(Prepare prepare, void* device,
    const entry_detour::Snapshot& snapshot, const void* caller, bool firstInit, uint64_t epoch) noexcept
{
    gPreparing = true;
    gPreparationEpoch = epoch;
    __try { prepare(device, snapshot, caller, firstInit); }
    __finally { gPreparationEpoch = 0; gPreparing = false; }
}
inline void WINAPI Before(void*, uintptr_t, const void* device, void*, uintptr_t projectDevice,
    uintptr_t, entry_detour::Handle handle, const void* caller) noexcept
{
    const DWORD error = GetLastError();
    // Even suppressed/reentrant or contending native Init calls revoke a
    // previously observed pre-native window. The epoch never wraps or resets.
    const uint64_t epoch = NextEntryEpoch();
    std::lock_guard boundary(gBoundaryCalls);
    const auto snapshot = entry_detour::ReadSnapshot(handle);
    bool firstInit = false;
    bool owned = false;
    {
        std::lock_guard lock(gInstall);
        for (auto& owner : gOwners)
            if (owner.module == snapshot.owner && owner.generation == snapshot.generation)
                for (const auto entry : owner.entries)
                    if (entry == handle)
                    {
                        owned = true;
                        firstInit = !owner.initObserved;
                        owner.initObserved = true;
                    }
    }
    if (!gPreparing && owned && snapshot.current)
    {
        const bool project = snapshot.kind == entry_detour::Kind::eNgxD3D12InitProject;
        if (const auto prepare = gPrepare.load(std::memory_order_acquire))
        {
            InvokePreparation(prepare, project ? reinterpret_cast<void*>(projectDevice) : const_cast<void*>(device),
                snapshot, caller, firstInit && epoch != 0, epoch);
        }
    }
    SetLastError(error);
}
inline void ObserveModule(HMODULE module, uint64_t generation, Prepare prepare) noexcept
{
    if (!module || !generation || !prepare) return;
    std::array<void*, 3> targets{};
    bool found = false;
    for (size_t i = 0; i < targets.size(); ++i)
    {
        targets[i] = reinterpret_cast<void*>(GetProcAddress(module, kNames[i]));
        HMODULE owner = nullptr;
        if (!targets[i] || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(targets[i]), &owner) || owner != module) targets[i] = nullptr;
        found |= targets[i] != nullptr;
    }
    if (!found) return;
    std::lock_guard lock(gInstall);
    size_t index = 0;
    for (; index < gOwners.size(); ++index) if (gOwners[index].module == module) break;
    if (index == gOwners.size())
    {
        for (index = 0; index < gOwners.size(); ++index) if (!gOwners[index].module) break;
        if (index == gOwners.size()) return;
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(module), &pinned) || pinned != module) return;
        gOwners[index].module = module;
        gOwners[index].generation = generation;
    }
    // Pinned owners are never recycled, and an inventory-generation mismatch
    // cannot reuse their forwarding observer as fresh initialization evidence.
    if (gOwners[index].generation != generation) return;
    gPrepare.store(prepare, std::memory_order_release);
    auto& owner = gOwners[index];
    for (size_t i = 0; i < targets.size(); ++i)
    {
        if (!targets[i] || owner.entries[i]) continue;
        // Some libraries alias Init and Init_Ext. One forwarding observer is
        // sufficient for those two identical device-argument positions.
        if (i == 1 && targets[1] == targets[0]) continue;
        if (i == 2 && (targets[2] == targets[0] || targets[2] == targets[1])) continue;
        entry_detour::InstallOptions options{};
        options.generation = owner.generation; options.allowRelocated = true;
        void* original = nullptr;
        entry_detour::InstallForwarding(i == 2 ? entry_detour::Kind::eNgxD3D12InitProject : entry_detour::Kind::eNgxD3D12Init,
            module, targets[i], &Before, original, options, &owner.entries[i]);
    }
}
}
