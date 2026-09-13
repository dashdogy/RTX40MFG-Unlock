#pragma once
#include <Windows.h>
#include <cstdint>

// Version 1 ends at renderedFrames (64 bytes). Version 2 appends the bounded
// overlay coordinator state. Version 3 appends owned-proxy counters. The
// 64-byte and 80-byte contracts remain supported without writing past them.
struct MfgSingleModuleStatus
{
    uint32_t size;
    uint32_t version;
    uint32_t backendOwner;
    uint32_t duplicateSuppressed;
    uint32_t dxgiHooked;
    uint32_t vulkanHooked;
    uint32_t overlayVisible;
    uint32_t renderFailures;
    uint32_t shortcutVirtualKey;
    uint32_t shortcutBindingActive;
    uint64_t dxgiFrames;
    uint64_t vulkanFrames;
    uint64_t renderedFrames;
    uint32_t overlayInstallState;   // single_overlay::install::State
    uint32_t overlayInstallReason;  // single_overlay::install::Reason
    uint32_t overlayActivatedTargets;
    uint32_t overlaySuspendedThreads;
    uint64_t factoryGatewayCalls;
    uint64_t factoryWrappersCreated;
    uint64_t swapchainWrappersCreated;
    uint64_t internalFactoryCallsSkipped;
    uint32_t liveFactoryWrappers;
    uint32_t liveSwapchainWrappers;
};
constexpr uint32_t kMfgSingleModuleStatusSizeV1 = 64;
constexpr uint32_t kMfgSingleModuleStatusSizeV2 = 80;
static_assert(sizeof(MfgSingleModuleStatus) == 120);
using MfgSingleModuleQueryFn = BOOL (WINAPI*)(MfgSingleModuleStatus*);
