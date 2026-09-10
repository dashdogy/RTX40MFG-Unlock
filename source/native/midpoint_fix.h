#pragma once

#include <Windows.h>

#include <cstdint>

#ifndef MFG_UNLOCK_OUTPUT_PULL_MASK_ONLY
#define MFG_UNLOCK_OUTPUT_PULL_MASK_ONLY 0
#endif
#ifndef MFG_UNLOCK_OUTPUT_PULL_MASK_OCCUPANCY
#define MFG_UNLOCK_OUTPUT_PULL_MASK_OCCUPANCY 0
#endif

namespace midpoint_fix
{
using LogCallback = void (*)(const wchar_t* message);

void SetLogCallback(LogCallback callback) noexcept;
bool ObserveD3D12Device(void* device) noexcept;
bool ObserveVulkanPhysicalDevice(void* physicalDevice) noexcept;
bool PatchProvider(HMODULE module, const wchar_t* path) noexcept;
uint64_t AdapterLuid() noexcept;
bool AdapterVerified() noexcept;
bool Ready() noexcept;
uint32_t FailureCode() noexcept;

// Internal provenance, not part of the public options/control ABI. A known
// graphics API means the caller already established the matching Ada route.
enum class OutputPullMaskGraphicsApi : uint32_t
{
    eUnknown = 0,
    eD3D12 = 1,
    eVulkan = 2,
};

struct OutputPullMaskCreateBoundary
{
    OutputPullMaskGraphicsApi api = OutputPullMaskGraphicsApi::eUnknown;
    bool firstPipelineCreate = false;
    bool pipelineMayPredateDetour = true;
    bool earlyInitObserved = false;
};

enum class OutputPullMaskFailure : uint32_t
{
    eNone = 0,
    eDisabled,
    eCreateOwnership,
    eTemporalPublication,
    eProviderIdentity,
    eProfileNotCovered,
    eDescriptor,
    eSourceIdentity,
    eAllocation,
    eTransform,
    ePublication,
    ePublicationLost,
};

struct OutputPullMaskSnapshot
{
    bool enabled = MFG_UNLOCK_OUTPUT_PULL_MASK_ONLY != 0;
    bool occupancyHint = MFG_UNLOCK_OUTPUT_PULL_MASK_OCCUPANCY != 0;
    bool attempted = false;
    bool sourceVerified = false;
    bool ready = false;
    bool published = false;
    bool publicationAttempted = false;
    bool replacementWasVisible = false;
    bool unsafePublication = false;
    bool requiresRestart = false;
    bool firstPipelineCreate = false;
    bool pipelineMayPredateDetour = true;
    bool earlyInitObserved = false;
    OutputPullMaskFailure failure = OutputPullMaskFailure::eNone;
    OutputPullMaskGraphicsApi api = OutputPullMaskGraphicsApi::eUnknown;
    uint32_t sourceBytes = 0;
    uint32_t selectedBytes = 0;
    uint64_t adapterLuid = 0;
    uintptr_t provider = 0;
    uintptr_t descriptorEntry = 0;
    uintptr_t originalDescriptor = 0;
    uintptr_t selectedDescriptor = 0;
    uintptr_t originalFatbin = 0;
    uintptr_t selectedFatbin = 0;
    char sourceSha256[65]{};
    char selectedSha256[65]{};
    char selectedPtxSha256[65]{};
};

// Optional optimization: a verified fallback preserves temporal/MFG readiness.
// Unsafe publication revokes readiness and requires a restart; callers must
// honor requiresRestart even when this function returns false.
// First publication requires the first observed early Create boundary. After
// the first attempt, calls only validate the retained immutable publication.
// A caller may not infer ownership merely from a later Create or an Off/On UI.
bool PrepareOutputPullMaskForCreate(HMODULE module, const wchar_t* path,
    OutputPullMaskCreateBoundary boundary) noexcept;
OutputPullMaskSnapshot ReadOutputPullMaskSnapshot() noexcept;
bool OutputPullMaskRequiresRestart() noexcept;
}
