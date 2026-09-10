#pragma once

#include <Windows.h>
#include <cstdint>

namespace vsync_control
{
enum class Mode : uint32_t { eGame = 0, eOff = 1, eOn = 2 };
enum class Failure : uint32_t
{
    eNone = 0,
    eRuntimeUnavailable,
    eNoApplicationSwapchain,
    eInterposerUnverified,
    eWrongOwner,
    eTargetChanged,
    eDeviceIdentityLost,
    eNotPrincipal,
    eNestedPresent,
    eTestPresent,
    eGenerationChanged,
    ePresentFailed,
    eConcurrentPresent,
};

struct Snapshot
{
    uint32_t requestedMode = 0;
    bool runtimeEligible = false;
    bool available = false;
    // A successful application Present submitted this request on the current
    // chain/configuration generation. This does not observe an NVCP override.
    bool matched = false;
    // The successful submission actually changed interval or tearing flags.
    bool overrideApplied = false;
    uint32_t originalInterval = 0;
    uint32_t submittedInterval = 0;
    uint32_t originalFlags = 0;
    uint32_t submittedFlags = 0;
    uint64_t ownerGeneration = 0;
    uint64_t configurationGeneration = 0;
    uint64_t runtimeGeneration = 0;
    Failure failure = Failure::eNoApplicationSwapchain;
    int32_t presentResult = 0;
};

// The backend supplies a monotonic epoch for its active wrapper/provider/
// publication proof. A zero epoch cannot authorize an override. Saved modes
// remain requested while the runtime is unavailable.
void Configure(uint32_t mode, bool runtimeEligible,
    uint64_t runtimeGeneration = 0) noexcept;
using RuntimeValidator = bool (*)() noexcept;
// Must read the current backend route/capability proof without I/O. Invoked
// outside this module's mutex, before both submission and completion.
void SetRuntimeValidator(RuntimeValidator validator) noexcept;
Snapshot ReadSnapshot() noexcept;
const char* FailureName(Failure failure) noexcept;

// Overlay-only ownership evidence. Device/queue identities are canonical COM
// identities and applicationWrapper is established by the Streamline proxy's
// QueryInterface contract. Native DXGI never satisfies this registration.
struct Ownership
{
    uintptr_t swapchain = 0;
    HMODULE creatorOwner = nullptr;
    HMODULE presentOwner = nullptr;
    HMODULE present1Owner = nullptr;
    void* presentEntry = nullptr;
    void* present1Entry = nullptr;
    uintptr_t queueIdentity = 0;
    uintptr_t queueDeviceIdentity = 0;
    uintptr_t swapchainDeviceIdentity = 0;
    uint64_t adapterLuid = 0;
    bool applicationWrapper = false;
};

struct Chain
{
    // Access only through this module; its mutex also protects the public
    // snapshot. The overlay owns this object for the swapchain generation.
    Ownership ownership{};
    uint64_t generation = 0;
    uint64_t inFlight = 0;
    bool registered = false;
    Failure failure = Failure::eNoApplicationSwapchain;
};

bool RegisterChain(Chain& chain, const Ownership& ownership) noexcept;
void InvalidateChain(Chain& chain,
    Failure failure = Failure::eGenerationChanged) noexcept;

struct Ticket
{
    uint64_t ownerGeneration = 0;
    uint64_t configurationGeneration = 0;
    uint64_t serial = 0;
    uint32_t originalInterval = 0;
    uint32_t submittedInterval = 0;
    uint32_t originalFlags = 0;
    uint32_t submittedFlags = 0;
    bool available = false;
    bool requestMatched = false;
    bool changed = false;
    bool tracked = false;
};

// callbackEntry is recovered from this exact HookFamily original trampoline;
// currentEntry is this object's current Present/Present1 vtable entry.
Ticket AdjustBeforePresent(Chain& chain, uintptr_t swapchain,
    void* callbackEntry, void* currentEntry, bool present1,
    bool principal, bool outer, uint32_t interval, uint32_t flags) noexcept;
void CompletePresent(Chain& chain, const Ticket& ticket, HRESULT result) noexcept;
}
