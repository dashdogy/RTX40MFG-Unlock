#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>

namespace entry_detour
{
// A Kind describes the ABI at an entry, not a unique process-wide slot.
// Multiple implementations of the same Kind may be installed concurrently.
enum class Kind : uint32_t
{
    eNgxD3D12CreateFeature = 0,
    eDlssgSetOptions = 1,
    eDlssgGetState = 2,
    eNgxD3D12EvaluateFeature = 3,
    eSlInit = 4,
    eNgxRuntimeD3D12CreateFeature = 5,
    eNgxRuntimeD3D12EvaluateFeature = 6,
    eSlSetData = 7,
    eSlGetData = 8,
    eSlFreeResources = 9,
    eNgxVulkanCreateFeature = 10,
    eNgxVulkanCreateFeature1 = 11,
    eNgxVulkanEvaluateFeature = 12,
    eNgxRuntimeVulkanCreateFeature = 13,
    eNgxRuntimeVulkanCreateFeature1 = 14,
    eNgxRuntimeVulkanEvaluateFeature = 15,
    eNgxVulkanAdapterInit = 16,
    eCount,
};

enum class Method : uint32_t
{
    eUnavailable = 0,
    eHotpatch = 1,
    eRelocated = 2,
};

enum class Failure : uint32_t
{
    eNone = 0,
    eInvalidArgument = 1,
    eRegistryFull = 2,
    eUnsupportedMitigation = 3,
    eOwnerMismatch = 4,
    eUnsupportedEntry = 5,
    eEntryNotHotpatchable = 6,
    eTargetPinFailed = 7,
    eRelayAllocationFailed = 8,
    eProtectionChangeFailed = 9,
    eAtomicPublicationFailed = 10,
    eProtectionRestoreFailed = 11,
    ePostWriteVerificationFailed = 12,
    eHookConflict = 13,
    eRelocatorInitializeFailed = 14,
    eRelocatorCreateFailed = 15,
    eRelocatorEnableFailed = 16,
    eForwardRelayFailed = 17,
};

// Handles remain valid for the lifetime of the process. Installed entries pin
// their owner because already-cached function pointers may outlive discovery.
struct Handle
{
    uint32_t slot = UINT32_MAX;
    uint32_t serial = 0;

    explicit operator bool() const noexcept
    {
        return slot != UINT32_MAX && serial != 0;
    }

    friend bool operator==(Handle left, Handle right) noexcept
    {
        return left.slot == right.slot && left.serial == right.serial;
    }
};

struct InstallOptions
{
    // Monotonic load generation assigned by module discovery. It is part of
    // the registry key, so a stale discovery record cannot impersonate a new
    // module instance at a recycled base address.
    uint64_t generation = 0;

    // When false, installation is limited to the atomic hotpatch-entry form.
    // When true, a pinned MinHook/HDE64 relocation fallback is permitted after
    // the strict hotpatch profile has been rejected.
    bool allowRelocated = false;

    // Optional fast-path filter for forwarding detours. When enabled, calls
    // whose second register argument does not match this value jump directly
    // to the original trampoline without entering the pre-call callback. This
    // keeps shared NGX runtime entries transparent to unrelated features.
    bool filterForwardArg2 = false;
    uintptr_t requiredForwardArg2 = 0;
};

struct Snapshot
{
    bool installed = false;
    bool current = false;
    bool cachedPointersCovered = false;
    Failure failure = Failure::eNone;
    Method method = Method::eUnavailable;
    Kind kind = Kind::eCount;
    Handle handle{};
    HMODULE owner = nullptr;
    void* target = nullptr;
    void* original = nullptr;
    uint32_t targetRva = 0;
    uint64_t generation = 0;
    uint32_t matchingEntries = 0;
    uint32_t currentEntries = 0;
};

// Installs one code-entry detour keyed by kind + target + owner + generation.
// The returned trampoline is unique to this target. With allowRelocated=false
// this uses the two-byte atomic hotpatch publication, covering cached pointers.
bool Install(Kind kind, HMODULE owner, void* target, void* hook,
    void*& originalTrampoline, const InstallOptions& options,
    Handle* installedHandle = nullptr) noexcept;

// Compatibility overload for callers which only need one strict hotpatch
// entry. New routing code should retain the returned Handle and query it.
bool Install(Kind kind, HMODULE owner, void* target, void* hook,
    void*& originalTrampoline) noexcept;

// Provider/runtime forwarding preserves the original caller return address.
// The callback receives the four original register arguments, the first two
// stack arguments, the exact detour handle and that caller address. The
// assembly dispatcher then tail-forwards through this entry's own trampoline.
using ForwardPreCall = void (WINAPI*)(void* arg1, uintptr_t arg2,
    const void* arg3, void* arg4, uintptr_t arg5, uintptr_t arg6,
    Handle handle,
    const void* originalCaller) noexcept;

bool InstallForwarding(Kind kind, HMODULE owner, void* target,
    ForwardPreCall preCall, void*& originalTrampoline,
    const InstallOptions& options, Handle* installedHandle = nullptr) noexcept;

Snapshot ReadSnapshot(Handle handle) noexcept;

// Aggregate compatibility view. It reports the newest current entry of this
// kind and includes counts; readiness decisions should use the exact Handle.
Snapshot ReadSnapshot(Kind kind) noexcept;

// Owner-scoped aggregate used to pair Create/Evaluate entries from the same
// provider or runtime module before the first Evaluate call occurs.
Snapshot ReadSnapshot(Kind kind, HMODULE owner) noexcept;

size_t RegistryCapacity() noexcept;
const char* MethodName(Method method) noexcept;
const char* FailureName(Failure failure) noexcept;
}
