#if defined(MFG_UNLOCK_SINGLE_MODULE_UI)
#include "single_module.h"
#include "single_overlay.h"
#include "overlay_application_imports.h"
#include "overlay_slots.h"
#include "caller_scoped_import.h"
#endif
#include "shared.h"
#include "build_variant.h"
#include "unified_control_paths.h"
#include "status_transport.h"
#include "ui_status_json.h"
#include "ui_input_coherence.h"
#include "ampere_backend.h"
#include "adapter_discovery.h"
#include "ngx_initialization.h"
#include "ngx_runtime_dispatch.h"
#include "ampere_gpu.h"
#include "ampere_policy.h"
#include "gpu_dispatch.h"
#include "gpu_selection_policy.h"
#include "midpoint_fix.h"
#include "early_provider_load.h"
#include "output_pull_experiment.h"
#include "prev2curr_experiment.h"
#include "output_pull_telemetry.h"
namespace gpu_backend = gpu_dispatch;
inline bool UseAmpere() noexcept { return gpu_dispatch::IsAmpere(); }

#include "dlssg_provider_policy.h"
#include "ngx_mfg_gate.h"
#include "reflex_control.h"
#include "vsync_control.h"
#include "ngx_runtime_policy.h"
#include "dlssg_preset.h"
#include "present_counter.h"
#include "entry_detour.h"
#include "protected_pointer.h"
#include "nvidia_mfg_policy.h"
#include "selective_ota_wrapper_policy.h"
#include "streamline_ota_policy.h"
#include "temporal_interval_trace.h"
#include "fault_capture.h"
#include "universal_route_policy.h"
#include "universal_wrapper_profile.h"
#include "vulkan_capability.h"

#include <Windows.h>
#include <TlHelp32.h>
#include <d3d12.h>
#include <winternl.h>
#include <sl.h>
#include <sl_dlss_g.h>
#include <nvsdk_ngx.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <cwchar>
#include <iterator>
#include <mutex>
#include <share.h>
#include <string>
#include <utility>
#include <vector>

namespace
{
// Stable prefix of sl::VulkanInfo v1-v3. The reference ABI is pointer-sized,
// so the universal core can observe the active VkPhysicalDevice without
// depending on Vulkan SDK headers or copying Streamline's helper-only types.
struct VulkanInfoPrefix
{
    sl::BaseStructure* next = nullptr;
    sl::StructType structType{};
    size_t structVersion = 0;
    void* device = nullptr;
    void* instance = nullptr;
    void* physicalDevice = nullptr;
};

using PFun_slSetVulkanInfoAbi = sl::Result(
    const VulkanInfoPrefix& info);

static_assert(offsetof(VulkanInfoPrefix, device) == 32);
static_assert(offsetof(VulkanInfoPrefix, physicalDevice) == 48);

FILE* gLog = nullptr;
std::atomic<bool> gDesiredFollowGame{true};
std::atomic<uint32_t> gDesiredMultiplier{2u};
std::atomic<bool> gDesiredDynamicMode{false};
std::atomic<uint32_t> gDynamicTargetFrameRate{0};
std::atomic<uint32_t> gDlssgPresetRequested{2};
std::atomic<uint32_t> gVsyncMode{0};
std::atomic<uint32_t> gReflexFrameLimitFps{0};
std::mutex gControlSnapshotMutex;
std::atomic<bool> gDynamicExperimental56{false};
std::atomic<bool> gGeneratedOnlyDebug{false};
std::atomic<uint64_t> gDesiredRevision{0};
std::atomic<uint64_t> gAppliedRevision{0};
std::atomic<uint64_t> gAttemptedRevision{0};
std::atomic<uint64_t> gLastAttemptTick{0};
std::atomic<bool> gControlReady{false};
std::atomic<PFun_slGetFeatureFunction*> gOriginalGetFeatureFunction{nullptr};
std::atomic<PFun_slSetD3DDevice*> gOriginalSetD3DDevice{nullptr};
std::atomic<PFun_slSetVulkanInfoAbi*> gOriginalSetVulkanInfo{nullptr};
std::atomic<PFun_slSetTag*> gOriginalSetTag{nullptr};
std::atomic<PFun_slSetTagForFrame*> gOriginalSetTagForFrame{nullptr};
std::atomic<PFun_slInit*> gOriginalSlInit{nullptr};
entry_detour::Handle gSlInitEntryHandle{};
using GetProcAddressFn = FARPROC (WINAPI*)(HMODULE, LPCSTR);
using LoadLibraryAFn = HMODULE (WINAPI*)(LPCSTR);
using LoadLibraryWFn = HMODULE (WINAPI*)(LPCWSTR);
using LoadLibraryExAFn = HMODULE (WINAPI*)(LPCSTR, HANDLE, DWORD);
using LoadLibraryExWFn = HMODULE (WINAPI*)(LPCWSTR, HANDLE, DWORD);
std::atomic<GetProcAddressFn> gOriginalSlCommonGetProcAddress{nullptr};
std::atomic<GetProcAddressFn> gOriginalMainGetProcAddress{nullptr};
std::atomic<LoadLibraryAFn> gOriginalStreamlineLoadLibraryA{nullptr};
std::atomic<LoadLibraryWFn> gOriginalStreamlineLoadLibraryW{nullptr};
std::atomic<LoadLibraryExAFn> gOriginalStreamlineLoadLibraryExA{nullptr};
std::atomic<LoadLibraryExWFn> gOriginalStreamlineLoadLibraryExW{nullptr};
std::atomic<bool> gSlCommonResolverDiscoveryInstalled{false};
std::atomic<bool> gStreamlineLoaderDiscoveryInstalled{false};
std::atomic<uint64_t> gStreamlineLoaderDiscoveryCalls{0};
std::atomic<uintptr_t> gRemixRuntimeBase{0};
std::atomic<bool> gMainResolverDiscoveryInstalled{false};
std::atomic<bool> gSlInitIatFallbackInstalled{false};
std::atomic<bool> gSlInitResolverFallbackActive{false};
std::atomic<uint64_t> gSlInitCalls{0};
std::atomic<uint64_t> gSlInitFlagsBefore{0};
std::atomic<uint64_t> gSlInitFlagsAfter{0};
std::atomic<bool> gOtaPreferencesForced{false};
std::atomic<bool> gDownloadedStreamlinePluginsForced{false};
std::atomic<bool> gOtaProviderPreflightSupported{false};
std::atomic<bool> gOtaForceSuppressed{false};
std::atomic<bool> gFullStreamlineOtaRequested{false};
std::atomic<bool> gFullStreamlineOtaEligible{false};
std::atomic<bool> gNvidiaCompatibilityResolved{false};
std::atomic<uint32_t> gNvidiaCompatibilityTier{0};
std::atomic<int32_t> gNvidiaProfileStatus{-1};
std::atomic<uint32_t> gStreamlineHostVersionMajor{0};
std::atomic<uint32_t> gStreamlineHostVersionMinor{0};
std::atomic<uint32_t> gStreamlineHostVersionBuild{0};
std::atomic<uint32_t> gStreamlineHostVersionPrivate{0};
std::atomic<bool> gSelectiveOtaDlssgWrapperRequested{false};
std::atomic<bool> gSelectiveOtaDlssgWrapperCandidateReady{false};
std::atomic<uint32_t> gSelectiveOtaDlssgWrapperFailure{0};
std::atomic<uint64_t> gSelectiveOtaDlssgWrapperRedirectAttempts{0};
std::atomic<uint64_t> gSelectiveOtaDlssgWrapperRedirectSuccesses{0};
std::atomic<uint64_t> gSelectiveOtaDlssgWrapperFallbacks{0};
std::atomic<uint32_t> gSelectiveOtaDlssgWrapperVersionMajor{0};
std::atomic<uint32_t> gSelectiveOtaDlssgWrapperVersionMinor{0};
std::atomic<uint32_t> gSelectiveOtaDlssgWrapperVersionBuild{0};
std::atomic<uint32_t> gSelectiveOtaDlssgWrapperVersionPrivate{0};
std::atomic<bool> gSetOptionsHookExposed{false};
std::atomic<bool> gGetStateHookExposed{false};
std::atomic<bool> gSetOptionsSeen{false};
std::atomic<bool> gGetStateSeen{false};
// This records the host's unadjusted request.  Keep it separate from the mode
// accepted after applying the mod control: an accepted mod-generated Off must
// not erase the host's continuing On intent.
std::atomic<bool> gGameFrameGenerationOn{false};
std::atomic<uint32_t> gGameFrameGenerationViewport{UINT32_MAX};
std::atomic<bool> gAppliedFrameGenerationOn{false};
std::atomic<uint64_t> gPresentationLifecycleEpoch{1};
std::atomic<int32_t> gLastSetOptionsResult{static_cast<int32_t>(sl::Result::eErrorNotInitialized)};
std::atomic<int32_t> gLastGetStateResult{static_cast<int32_t>(sl::Result::eErrorNotInitialized)};
std::atomic<bool> gAppliedDynamicMode{false};
std::atomic<uint32_t> gAppliedMultiplier{0};
std::atomic<float> gAppliedDynamicTargetFrameRate{0.0f};
std::atomic<bool> gAppliedDynamicTargetValid{false};
std::atomic<bool> gAppliedDynamicExperimental56{false};
std::atomic<bool> gAppliedGeneratedOnlyDebug{false};
std::atomic<uint32_t> gActualFramesPresented{0};
std::atomic<uint32_t> gNumFramesToGenerateMax{0};
std::atomic<uint32_t> gDlssgStatus{0};
std::atomic<bool> gDynamicMfgSupported{false};
std::atomic<bool> gDynamicMfgCapabilityKnown{false};
std::atomic<bool> gFgVsyncSupportKnown{false};
std::atomic<bool> gFgVsyncSupported{false};
std::atomic<uint64_t> gStateSampleTick{0};
std::atomic<uint64_t> gSetOptionsCalls{0};
std::atomic<uint64_t> gSetOptionsResolverFallbackCalls{0};
std::atomic<uint64_t> gGetStateCalls{0};
std::atomic<uint64_t> gGetStateResolverFallbackCalls{0};
std::atomic<uint64_t> gLiveReapplyCount{0};
std::atomic<uint64_t> gNotInitializedRetryCount{0};
std::atomic<uint64_t> gNgxCreateCalls{0};
std::atomic<uint64_t> gNgxFrameGenerationCreateCalls{0};
std::atomic<uint64_t> gNgxEvaluateCalls{0};
using NgxDispatchRoute = universal_route_policy::NgxDispatchRoute;
enum class NgxGraphicsApi : uint32_t
{
    eUnknown = 0,
    eD3D12 = 1,
    eVulkan = 2,
};
enum class NgxProviderSelectionSource : uint32_t
{
    eNone = 0,
    eProviderEntry = 1,
    eRuntimeCaller = 2,
    eRuntimeUniqueCandidate = 3,
    eRuntimeDispatchTable = 4,
};
std::atomic<uint32_t> gActiveNgxDispatchRoute{
    static_cast<uint32_t>(NgxDispatchRoute::ePending)};
std::atomic<uint32_t> gActiveNgxGraphicsApi{
    static_cast<uint32_t>(NgxGraphicsApi::eUnknown)};
std::atomic<bool> gVulkanAdapterVerified{false};
std::atomic<uint64_t> gActiveNgxCreateHandle{0};
std::atomic<uint64_t> gActiveNgxEvaluateHandle{0};
std::atomic<uintptr_t> gActiveNgxProviderBase{0};
std::atomic<uint64_t> gActiveNgxProviderGeneration{0};
std::atomic<uint32_t> gActiveNgxSelectionSource{0};
std::atomic<bool> gProviderChangedAfterCreate{false};
std::atomic<int32_t> gLastNgxCreateResult{0};
std::atomic<bool> gFrameGenerationCreateObserved{false};
std::atomic<bool> gFirstCreateMidpointReady{false};
std::atomic<bool> gBackportReadyAtCreate{false};
std::atomic<bool> gPipelineMayPredateDetour{false};
std::atomic<bool> gRestartRequired{false};
std::atomic<bool> gDllNotificationRegistered{false};
early_provider_load::Tracker<> gOutputPullMaskLoads;
struct OutputPullMaskInitEvidence
{
    entry_detour::Handle handle{};
    HMODULE owner = nullptr;
    uint64_t generation = 0;
};
std::mutex gOutputPullMaskInitMutex;
OutputPullMaskInitEvidence gOutputPullMaskSlInit;
OutputPullMaskInitEvidence gOutputPullMaskVulkanInit;
thread_local entry_detour::Snapshot gAmpereNativeInitBoundary{};
thread_local uint64_t gAmpereNativeInitTicket = 0;
std::atomic<bool> gModuleInventoryDirty{true};
std::atomic<bool> gLiveHookInstalled{false};
std::atomic<bool> gSetOptionsResolverFallbackActive{false};
std::atomic<bool> gGetStateResolverFallbackActive{false};
std::atomic<bool> gUiTagHookInstalled{false};
std::atomic<uint32_t> gLoadedWrapperCandidates{0};
std::atomic<uint32_t> gPatchedWrapperCandidates{0};
std::atomic<uint32_t> gLoadedNgxCandidates{0};
std::atomic<uint32_t> gPatchedNgxCandidates{0};
std::atomic<uint32_t> gWrapperRouteBits{0};
std::atomic<uint32_t> gNgxRouteBits{0};
std::atomic<bool> gActiveWrapperObserved{false};
std::atomic<bool> gActiveWrapperPatched{false};
std::atomic<uintptr_t> gActiveWrapperBase{0};
std::atomic<uint32_t> gWrapperCompiledMaximumGeneratedFrames{0};
std::atomic<uint32_t> gSafeMaximumMultiplier{2};
std::atomic<bool> gActiveWrapperUsesNvidiaOta{false};
std::atomic<uint32_t> gActiveWrapperVersionMajor{0};
std::atomic<uint32_t> gActiveWrapperVersionMinor{0};
std::atomic<uint32_t> gActiveWrapperVersionBuild{0};
std::atomic<uint32_t> gActiveWrapperVersionPrivate{0};
std::atomic<uint32_t> gLastOptionsViewport{UINT32_MAX};
std::atomic<uint32_t> gGameOptionsStructVersion{0};
std::atomic<uint32_t> gGameColorWidth{0};
std::atomic<uint32_t> gGameColorHeight{0};
std::atomic<uint32_t> gGameHudlessBufferFormat{0};
std::atomic<uint32_t> gGameUiBufferFormat{0};
std::atomic<bool> gGameUiRecompositionEnabled{false};
std::atomic<bool> gAppliedUiRecompositionEnabled{false};
std::atomic<bool> gAppliedUiRecompositionForced{false};
std::atomic<uint64_t> gSetTagCalls{0};
std::atomic<uint64_t> gSetTagForFrameCalls{0};
std::atomic<uint32_t> gRealFpsMilli{0};
std::atomic<uint32_t> gFpsOutputSource{0}; // 0 unavailable, 1 callback, 2 DXGI, 3 estimate.
std::atomic<uint32_t> gDlssFpsMilli{0};
std::atomic<uint32_t> gFpsSampleWindowMs{0};
std::atomic<uint64_t> gFpsSampleTick{0};
std::atomic<uint64_t> gFpsOutputPresentTick{0};
std::atomic<bool> gLogReady{false};
std::mutex gStreamlineCallMutex;
std::mutex gLastOptionsMutex;
std::mutex gModuleMutex;
std::mutex gImportPublicationMutex;
std::mutex gActiveControlRouteMutex;
std::mutex gUiTagMutex;
std::mutex gSelectiveOtaDlssgWrapperMutex;
std::once_flag gNvidiaCompatibilityOnce;
std::wstring gConfigPath;
std::wstring gStatusPath;
std::wstring gExecutableDirectory;
std::wstring gExecutablePath;
std::array<wchar_t, 32768> gExecutablePathBuffer{};
std::wstring gSelectiveOtaDlssgWrapperPath;
std::string gNvidiaProfileName;

constexpr uint32_t kRouteLocal = 1u;
constexpr uint32_t kRouteExternal = 2u;
constexpr uint32_t kMinimumMultiplier = 2u;
constexpr uint32_t kMaximumMultiplier = 6u;
constexpr uint64_t kNotInitializedRetryDelayMs = 500;

uint64_t PackEntryHandle(entry_detour::Handle handle) noexcept
{
    return (static_cast<uint64_t>(handle.serial) << 32)
        | static_cast<uint64_t>(handle.slot);
}

entry_detour::Handle UnpackEntryHandle(uint64_t value) noexcept
{
    if (value == 0)
        return {};
    return {static_cast<uint32_t>(value),
        static_cast<uint32_t>(value >> 32)};
}

enum class SelectiveOtaDlssgWrapperFailure : uint32_t
{
    eNone = 0,
    eProviderUnsupported = 1,
    eLoaderDiscoveryUnavailable = 2,
    eProgramDataUnavailable = 3,
    eNoCompatibleCandidate = 4,
};

struct ControlConfig
{
    bool followGame = true;
    uint32_t multiplier = 2u;
    bool dynamic = false;
    uint32_t dynamicTargetFrameRate = 0;
    uint32_t dlssgPreset = 2;
    uint32_t vsyncMode = 0;
    uint32_t reflexFrameLimitFps = 0;
    bool dynamicExperimental56 = false;
    bool generatedOnlyDebug = false;
    bool intervalLogging = gpu_dispatch::IsAda() || UseAmpere();
    bool selectiveOtaDlssgWrapper = false;
};

struct ControlSnapshot
{
    ControlConfig control{};
    uint64_t revision = 0;
};

struct LastGameOptions
{
    sl::ViewportHandle viewport{0u};
    sl::DLSSGOptions options{};
    bool valid = false;
};

struct RetainedFeatureIdentity
{
    uintptr_t handle = 0;
    uintptr_t runtime = 0;
    uint64_t runtimeGeneration = 0;
    uintptr_t provider = 0;
    uint64_t providerGeneration = 0;
    uintptr_t wrapper = 0;
    uint64_t wrapperGeneration = 0;
    uint64_t publication = 0;
    uint64_t adapterLuid = 0;
    uint32_t generatedFrameCapacity = 0;
    uint64_t lifetime = 0;
    uint64_t createAttemptEpoch = 0;

    explicit operator bool() const noexcept
    {
        return handle != 0 && runtime != 0 && runtimeGeneration != 0
            && provider != 0 && providerGeneration != 0 && wrapper != 0
            && wrapperGeneration != 0 && publication != 0
            && generatedFrameCapacity != 0 && lifetime != 0;
    }
};

struct AcceptedOffEvidence
{
    RetainedFeatureIdentity feature{};
    uint32_t routeSlot = UINT32_MAX;
    uint32_t viewport = UINT32_MAX;
    uint64_t revision = 0;
};

using PFun_slSetDataInternal = sl::Result(
    const sl::BaseStructure* inputs, sl::CommandBuffer* commandBuffer);
using PFun_slGetDataInternal = sl::Result(
    const sl::BaseStructure* inputs, sl::BaseStructure* outputs,
    sl::CommandBuffer* commandBuffer);

using ControlEntryPath = universal_route_policy::Path;
using UniversalRouteFailure = universal_route_policy::Failure;

struct ModuleRecord
{
    HMODULE module = nullptr;
    std::wstring path;
    uint64_t generation = 0;
    uint64_t freshLoadToken = 0;
    uint32_t controlRouteSlot = UINT32_MAX;
    bool wrapperExport = false;
    bool wrapperCandidate = false;
    bool wrapperPatched = false;
    uint32_t wrapperCompiledMaximumGeneratedFrames = 0;
    bool ngxExport = false;
    bool ngxD3D12Export = false;
    bool ngxVulkanExport = false;
    bool ngxCandidate = false;
    bool ngxPatched = false;
    bool ngxTemporalPatched = false;
    bool ngxRuntimeExport = false;
    bool ngxRuntimeD3D12Export = false;
    bool ngxRuntimeVulkanExport = false;
    bool createResolverDiscoveryHooked = false;
    bool inventoryLogged = false;
};

struct FileVersion
{
    uint32_t major = 0;
    uint32_t minor = 0;
    uint32_t build = 0;
    uint32_t privatePart = 0;
};

constexpr size_t kControlRouteCapacity = 16;

struct ControlRouteRecord
{
    std::atomic<bool> claimed{false};
    HMODULE wrapper = nullptr;
    uint64_t generation = 0;
    std::wstring path;
    FileVersion version{};
    std::atomic<bool> wrapperPatched{false};
    std::atomic<uint32_t> compiledMaximumGeneratedFrames{0};

    entry_detour::Handle publicSetHandle{};
    entry_detour::Handle publicGetHandle{};
    entry_detour::Handle internalSetHandle{};
    entry_detour::Handle internalGetHandle{};
    entry_detour::Handle freeResourcesHandle{};
    std::atomic<PFun_slDLSSGSetOptions*> publicSetOriginal{nullptr};
    std::atomic<PFun_slDLSSGGetState*> publicGetOriginal{nullptr};
    std::atomic<PFun_slSetDataInternal*> internalSetOriginal{nullptr};
    std::atomic<PFun_slGetDataInternal*> internalGetOriginal{nullptr};
    std::atomic<PFun_slFreeResources*> freeResourcesOriginal{nullptr};
    std::atomic<bool> publicSetResolverFallback{false};
    std::atomic<bool> publicGetResolverFallback{false};
    std::atomic<bool> lifecycleInstallAttempted{false};

    std::atomic<uint32_t> activeSetterPath{
        static_cast<uint32_t>(ControlEntryPath::eNone)};
    std::atomic<uint32_t> activeStatePath{
        static_cast<uint32_t>(ControlEntryPath::eNone)};
    std::atomic<uint64_t> setterCalls{0};
    std::atomic<uint64_t> stateCalls{0};
    std::atomic<uint64_t> lastCallTick{0};
    std::atomic<uint64_t> lastCallRevision{0};
    std::atomic<uint64_t> lastAcceptedRevision{0};
    std::atomic<bool> structureCompatible{true};
    std::atomic<bool> frameGenerationOffAccepted{false};
    std::atomic<bool> releaseObserved{false};
    std::atomic<uint64_t> releaseCalls{0};
};

using UiInputSnapshot = ui_input_coherence::Snapshot;

LastGameOptions gLastGameOptions;
RetainedFeatureIdentity gRetainedFrameGenerationFeature;
RetainedFeatureIdentity gFreshRecreatedFrameGenerationFeature;
AcceptedOffEvidence gAcceptedOffEvidence;
std::mutex gFrameGenerationLifetimeMutex;
std::atomic<uint64_t> gFrameGenerationCreateAttemptEpoch{0};
std::vector<ModuleRecord> gModuleRecords;
std::array<ControlRouteRecord, kControlRouteCapacity> gControlRoutes{};
ui_input_coherence::Tracker gUiInputTracker;
std::mutex gControlRouteMutex;
std::atomic<uint64_t> gNextModuleGeneration{1};
std::atomic<uint32_t> gActiveControlRouteSlot{UINT32_MAX};
std::atomic<uint32_t> gUniversalRouteFailure{
    static_cast<uint32_t>(UniversalRouteFailure::eNoActiveRoute)};
std::atomic<DWORD> gInternalControlBypassTlsIndex{TLS_OUT_OF_INDEXES};
LARGE_INTEGER gFpsCounterFrequency{};
LARGE_INTEGER gFpsWindowStart{};
uint64_t gFpsWindowOutputFrames = 0;
bool gFpsWindowOutputAvailable = false;
frame_telemetry::PresentCounter gFpsPresentCounter;
std::array<uint64_t, temporal_interval_trace::kFirstSampleHandleCapacity>
    gFpsWindowFirstSamples{};
bool gFpsTelemetryActive = false;
LARGE_INTEGER gIntervalFpsWindowStart{};
std::array<uint64_t, temporal_interval_trace::kFirstSampleHandleCapacity>
    gIntervalFpsWindowFirstSamples{};
std::mutex gFpsTelemetryMutex;

HMODULE ModuleFromAddress(const void* address);
std::wstring LoadedModulePath(HMODULE module);
bool UsesNvidiaOtaCache(const std::wstring& path);
void RecomputeModuleStateLocked();
void ObserveAmpereFeatureLifetime(
    const ampere_backend::FeatureLifetimeEvent& event) noexcept;
void EnsureAmpereFeatureLifetimeObserver() noexcept;
ControlRouteRecord* ActiveControlRoute() noexcept;
const char* ControlPathName(ControlEntryPath path) noexcept;
void SetUniversalRouteFailure(UniversalRouteFailure failure) noexcept;
bool ActiveSetterCovered(const ControlRouteRecord& route) noexcept;
bool SetterEntryCovered(const ControlRouteRecord& route) noexcept;
bool StateEntryCovered(const ControlRouteRecord& route) noexcept;
bool TryInstallSetOptionsEntryDetour(HMODULE wrapper, void* resolvedTarget);
bool TryInstallGetStateEntryDetour(HMODULE wrapper, void* resolvedTarget);
uint32_t EnsureControlRoute(HMODULE wrapper, const std::wstring& path,
    uint64_t generation, bool wrapperPatched,
    uint32_t compiledMaximumGeneratedFrames);
bool InstallControlRouteEntries(uint32_t routeSlot);
bool InstallControlRouteLifecycleEntry(uint32_t routeSlot);
bool TryInstallSlInitEntryDetour(HMODULE interposer, void* resolvedTarget);
void WINAPI BeforeNgxD3D12EvaluateFeature(void*, uintptr_t, const void*, void*, uintptr_t, uintptr_t, entry_detour::Handle, const void*) noexcept;
void WINAPI BeforeNgxRuntimeD3D12EvaluateFeature(void*, uintptr_t, const void*, void*, uintptr_t, uintptr_t, entry_detour::Handle, const void*) noexcept;
bool TryInstallNgxCreateEntryDetour(HMODULE provider, const std::wstring& path,
    uint64_t generation);
bool TryInstallNgxEvaluateEntryDetour(
    HMODULE provider, const std::wstring& path, uint64_t generation);
bool TryInstallNgxRuntimeCreateEntryDetour(
    HMODULE runtime, const std::wstring& path, uint64_t generation);
bool TryInstallNgxRuntimeEvaluateEntryDetour(
    HMODULE runtime, const std::wstring& path, uint64_t generation);
bool TryInstallNgxVulkanCreateEntryDetours(
    HMODULE provider, const std::wstring& path, uint64_t generation);
bool TryInstallNgxVulkanEvaluateEntryDetour(
    HMODULE provider, const std::wstring& path, uint64_t generation);
bool TryInstallNgxRuntimeVulkanCreateEntryDetours(
    HMODULE runtime, const std::wstring& path, uint64_t generation);
bool TryInstallNgxRuntimeVulkanEvaluateEntryDetour(
    HMODULE runtime, const std::wstring& path, uint64_t generation);
bool TryInstallNgxVulkanAdapterEntryDetours(
    HMODULE module, const std::wstring& path, uint64_t generation);
bool InstallSlCommonResolverDiscovery(
    HMODULE module, const std::wstring& path);
bool InstallStreamlineLoaderDiscovery(
    HMODULE module, const std::wstring& path);
ModuleRecord InspectLoadedModule(HMODULE, const std::wstring&);
void InspectAlreadyLoadedModules();
ampere_gpu::PreparationBoundary ResolveAmperePreparationBoundary(HMODULE, uint64_t) noexcept;
bool AmperePreparationStillCurrent(HMODULE, const ampere_gpu::PreparationBoundary&) noexcept;
void PrepareNgxInitialization(void* device, const entry_detour::Snapshot& entry,
    const void* caller, bool firstInit) noexcept;
bool OutputPullMaskEarlyInitProven(HMODULE provider, NgxGraphicsApi api) noexcept;
void ObserveOutputPullMaskSlInitResult(
    const entry_detour::Snapshot& before, bool succeeded) noexcept;
void ObserveOutputPullMaskVulkanInit(entry_detour::Handle handle) noexcept;

sl::Result InvokeAmpereDeviceSetup(PFun_slSetD3DDevice* original, void* device)
{
    sl::Result result = sl::Result::eErrorNotInitialized;
    ampere_backend::BeginStartup();
    __try
    {
        InspectAlreadyLoadedModules();
        result = original(device);
        InspectAlreadyLoadedModules();
    }
    __finally { ampere_backend::EndStartup(result == sl::Result::eOk); }
    return result;
}

sl::Result InvokeAmpereInit(PFun_slInit* original, const sl::Preferences& preferences, uint64_t version)
{
    ampere_backend::BeginInit();
    __try { return original(preferences, version); }
    __finally { ampere_backend::EndInit(); }
}

void ConfigureSelectiveOtaDlssgWrapper(bool requested,
    bool providerSupported, bool loaderDiscoveryReady);
std::wstring SelectiveOtaDlssgWrapperRedirectPath(
    const std::wstring& requestedPath);
sl::Result HookSlDLSSGGetState(const sl::ViewportHandle& viewport,
    sl::DLSSGState& state, const sl::DLSSGOptions* options);
ModuleRecord InspectLoadedModule(
    HMODULE module, const std::wstring& suppliedPath);
FileVersion ReadFileVersion(const std::wstring& path);
uint64_t ModuleGeneration(HMODULE module) noexcept;

void Log(const wchar_t* format, ...)
{
    wchar_t message[2048]{};
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(message, _countof(message), _TRUNCATE, format, args);
    va_end(args);

    OutputDebugStringW(L"[MfgUnlock] ");
    OutputDebugStringW(message);
    OutputDebugStringW(L"\n");
    if (gLog)
    {
        fwprintf_s(gLog, L"%s\n", message);
        fflush(gLog);
    }
}

void MidpointLog(const wchar_t* message)
{
    Log(L"%s", message ? message : L"");
}

int32_t NvidiaProfileStatus(
    const nvidia_mfg_policy::ProfileQuery& query) noexcept
{
    if (query.initializeStatus != 0)
        return query.initializeStatus;
    if (query.createSessionStatus != 0)
        return query.createSessionStatus;
    if (query.loadSettingsStatus != 0)
        return query.loadSettingsStatus;
    if (query.findApplicationStatus != 0)
        return query.findApplicationStatus;
    return query.getProfileStatus;
}

void ResolveNvidiaCompatibilityPolicy()
{
    std::call_once(gNvidiaCompatibilityOnce, [] {
        const nvidia_mfg_policy::ProfileQuery query =
            nvidia_mfg_policy::IdentifyExecutable(gExecutablePath.c_str());
        gNvidiaProfileName = nvidia_mfg_policy::NormalizeTitle(
            query.profileName);
        gNvidiaProfileStatus.store(
            NvidiaProfileStatus(query), std::memory_order_relaxed);
        gNvidiaCompatibilityTier.store(
            static_cast<uint32_t>(query.tier), std::memory_order_relaxed);
        gNvidiaCompatibilityResolved.store(
            query.getProfileStatus == 0 && !query.profileName.empty(),
            std::memory_order_release);
    });
}

nvidia_mfg_policy::CapacityDecision CurrentCapacityDecision() noexcept
{
    const bool activeObserved =
        gActiveWrapperObserved.load(std::memory_order_acquire);
    const bool wrapperPatched = activeObserved
        && gActiveWrapperPatched.load(std::memory_order_acquire);
    return nvidia_mfg_policy::DecideCapacity(
        static_cast<nvidia_mfg_policy::Tier>(
            gNvidiaCompatibilityTier.load(std::memory_order_acquire)),
        wrapperPatched,
        activeObserved
            ? gWrapperCompiledMaximumGeneratedFrames.load(
                std::memory_order_acquire)
            : 0u);
}

uint32_t SafeMaximumMultiplier() noexcept
{
    const uint32_t runtimeMaximum = UseAmpere()
        ? (ampere_backend::Ready()
            && ampere_backend::StartupMaximumGeneratedFrames() == ampere_policy::kMaximumGeneratedFrames
            && ampere_backend::CertifiedMaximumGeneratedFrames() == ampere_policy::kMaximumGeneratedFrames
            ? ampere_policy::kMaximumGeneratedFrames + 1u : 2u)
        : gpu_dispatch::IsAda() ? std::clamp(
            CurrentCapacityDecision().effectiveMaximumMultiplier,
            kMinimumMultiplier, kMaximumMultiplier) : 2u;
    const uint32_t maximum = nvidia_mfg_policy::LimitToPolicy(
        static_cast<nvidia_mfg_policy::Tier>(
            gNvidiaCompatibilityTier.load(std::memory_order_acquire)), runtimeMaximum);
    gSafeMaximumMultiplier.store(maximum, std::memory_order_release);
    return maximum;
}

uint32_t EffectiveMultiplier(const ControlConfig& control) noexcept
{
    if (UseAmpere())
    {
        return std::clamp(control.multiplier, 1u, SafeMaximumMultiplier());
    }
    else
    {
        return std::clamp(control.multiplier, kMinimumMultiplier,
            SafeMaximumMultiplier());
    }
}

ui_input_coherence::Generation CurrentUiInputGeneration() noexcept
{
    const auto* route = ActiveControlRoute();
    const bool current = route && route->structureCompatible.load(std::memory_order_acquire);
    return {current ? reinterpret_cast<uintptr_t>(route->wrapper) : 0,
        current ? route->generation : 0,
        gActiveNgxProviderBase.load(std::memory_order_acquire),
        gActiveNgxProviderGeneration.load(std::memory_order_acquire),
        gFrameGenerationCreateAttemptEpoch.load(std::memory_order_acquire),
        gPresentationLifecycleEpoch.load(std::memory_order_acquire),
        ui_input_coherence::resourceEpoch.load(std::memory_order_acquire)};
}

void InvalidateUiInputEvidence(uint32_t viewport = UINT32_MAX) noexcept
{
    ui_input_coherence::InvalidateResources();
    std::lock_guard lock(gUiTagMutex);
    gUiInputTracker.Invalidate(viewport);
}

ui_input_coherence::Resource CaptureUiResourceTag(const sl::ResourceTag& tag,
    uint64_t tick) noexcept
{
    ui_input_coherence::Resource state{};
    state.observed = true;
    state.lastSeenTick = tick;
    if (!tag.resource || !tag.resource->native) return state;
    const auto& resource = *tag.resource;
    state.active = true;
    state.native = reinterpret_cast<uintptr_t>(resource.native);
    state.view = reinterpret_cast<uintptr_t>(resource.view);
    state.type = static_cast<uint32_t>(resource.type);
    state.lifecycle = static_cast<uint32_t>(tag.lifecycle);
    state.top = tag.extent.top;
    state.left = tag.extent.left;
    state.width = tag.extent.width ? tag.extent.width : resource.width;
    state.height = tag.extent.height ? tag.extent.height : resource.height;
    state.format = resource.nativeFormat;
    // Do not dereference a retained resource later or infer native bounds from
    // an address. Unknown descriptions remain observations, never ready input.
    state.shapeKnown = tag.structType == sl::ResourceTag::s_structType
        && tag.structVersion == sl::kStructVersion1
        && resource.structType == sl::Resource::s_structType
        && resource.structVersion == sl::kStructVersion1
        && resource.type == sl::ResourceType::eTex2d && state.format
        && tag.lifecycle >= sl::ResourceLifecycle::eOnlyValidNow
        && tag.lifecycle <= sl::ResourceLifecycle::eValidUntilEvaluate
        && resource.width && resource.height && state.width && state.height
        && state.left <= resource.width && state.width <= resource.width - state.left
        && state.top <= resource.height && state.height <= resource.height - state.top;
    return state;
}

UiInputSnapshot ReadUiInputSnapshot(uint32_t viewport)
{
    const auto generation = CurrentUiInputGeneration();
    const bool active = viewport == gLastOptionsViewport.load(std::memory_order_acquire);
    std::lock_guard lock(gUiTagMutex);
    auto snapshot = gUiInputTracker.Read(viewport, generation, GetTickCount64(),
        active ? gGameColorWidth.load(std::memory_order_relaxed) : 0,
        active ? gGameColorHeight.load(std::memory_order_relaxed) : 0,
        active ? gGameHudlessBufferFormat.load(std::memory_order_relaxed) : 0,
        active ? gGameUiBufferFormat.load(std::memory_order_relaxed) : 0);
    if (generation != CurrentUiInputGeneration())
    {
        snapshot.ready = false;
        snapshot.generationCurrent = false;
        snapshot.failure = ui_input_coherence::Failure::eGenerationChanged;
    }
    return snapshot;
}

struct UiTagBatch
{
    uint32_t viewport = UINT32_MAX;
    bool valid = false;
    bool framed = false;
    uint32_t frame = 0;
    uint64_t tick = 0;
    ui_input_coherence::Generation generation{};
    std::array<ui_input_coherence::Resource, 3> resources{};
};

UiTagBatch PrepareUiResourceTags(const sl::ViewportHandle& viewport,
    const sl::ResourceTag* tags, uint32_t numTags, bool framed, uint32_t frame)
{
    UiTagBatch batch{};
    if (!tags || !numTags || numTags > 1024) return batch;
    batch.viewport = static_cast<uint32_t>(viewport);
    batch.framed = framed;
    batch.frame = frame;
    batch.tick = GetTickCount64();
    batch.generation = CurrentUiInputGeneration();
    // Copy bounded metadata while the caller's descriptors are valid. No COM
    // references, caller chains, or FrameToken pointers survive this call.
    for (uint32_t index = 0; index < numTags; ++index)
    {
        const auto& tag = tags[index];
        size_t slot = 3;
        if (tag.type == sl::kBufferTypeHUDLessColor) slot = 0;
        else if (tag.type == sl::kBufferTypeUIAlpha) slot = 1;
        else if (tag.type == sl::kBufferTypeUIColorAndAlpha) slot = 2;
        if (slot != 3)
        {
            batch.resources[slot] = CaptureUiResourceTag(tag, batch.tick);
            batch.valid = true;
        }
    }
    // A new explicitly framed tagging call advances the observed frame even
    // if it contains only other inputs, so previous-frame UI cannot linger.
    batch.valid = batch.valid || framed;
    return batch;
}

void RecordUiResourceTags(const UiTagBatch& batch)
{
    if (!batch.valid) return;
    {
        std::lock_guard lock(gUiTagMutex);
        if (batch.generation != CurrentUiInputGeneration())
        {
            gUiInputTracker.Invalidate(batch.viewport);
            return;
        }
        auto* entry = gUiInputTracker.Begin(batch.viewport, batch.framed,
            batch.frame, batch.generation, batch.tick);
        if (!entry) return;
        for (size_t slot = 0; slot < batch.resources.size(); ++slot)
            if (batch.resources[slot].observed)
                gUiInputTracker.Observe(*entry,
                    static_cast<ui_input_coherence::Kind>(slot), batch.resources[slot]);
    }
}

bool DlssgStateAvailable(sl::Result result) noexcept
{
    return result == sl::Result::eOk
        || result == sl::Result::eWarnOutOfVRAM;
}

void ResetFpsTelemetry() noexcept
{
    gFpsWindowStart = {};
    gFpsWindowOutputFrames = 0;
    gFpsWindowOutputAvailable = false;
    gFpsOutputSource.store(0, std::memory_order_relaxed);
    const temporal_interval_trace::Snapshot intervalTrace =
        temporal_interval_trace::ReadSnapshot();
    for (size_t index = 0;
         index < temporal_interval_trace::kFirstSampleHandleCapacity;
         ++index)
    {
        gFpsWindowFirstSamples[index] =
            intervalTrace.firstSampleCounters[index].samples;
    }
    gRealFpsMilli.store(0, std::memory_order_relaxed);
    gDlssFpsMilli.store(0, std::memory_order_relaxed);
    gFpsSampleWindowMs.store(0, std::memory_order_relaxed);
    gFpsSampleTick.store(0, std::memory_order_release);
}

void UpdateFpsTelemetryForOutputPresent(uint32_t outputFrames,
    bool outputAvailable, uint32_t outputSource)
{
    LARGE_INTEGER now{};
    if (!QueryPerformanceCounter(&now))
        return;
    if (gFpsCounterFrequency.QuadPart == 0
        && !QueryPerformanceFrequency(&gFpsCounterFrequency))
        return;
    if (gFpsWindowStart.QuadPart == 0 || now.QuadPart <= gFpsWindowStart.QuadPart)
    {
        gFpsWindowStart = now;
        gFpsWindowOutputFrames = 0;
        gFpsWindowOutputAvailable = outputAvailable;
        const temporal_interval_trace::Snapshot intervalTrace =
            temporal_interval_trace::ReadSnapshot();
        for (size_t index = 0;
             index < temporal_interval_trace::kFirstSampleHandleCapacity;
             ++index)
        {
            gFpsWindowFirstSamples[index] =
                intervalTrace.firstSampleCounters[index].samples;
        }
        return;
    }

    gFpsWindowOutputFrames += outputFrames;
    gFpsWindowOutputAvailable &= outputAvailable;
    const uint64_t elapsedTicks = static_cast<uint64_t>(
        now.QuadPart - gFpsWindowStart.QuadPart);
    const uint64_t minimumTicks = static_cast<uint64_t>(
        gFpsCounterFrequency.QuadPart) / 2;
    if (elapsedTicks < minimumTicks)
        return;

    const temporal_interval_trace::Snapshot intervalTrace =
        temporal_interval_trace::ReadSnapshot();
    uint64_t realFrames = 0;
    for (size_t index = 0;
         index < temporal_interval_trace::kFirstSampleHandleCapacity;
         ++index)
    {
        const uint64_t current =
            intervalTrace.firstSampleCounters[index].samples;
        const uint64_t previous = gFpsWindowFirstSamples[index];
        if (current >= previous)
            realFrames = std::max(realFrames, current - previous);
        gFpsWindowFirstSamples[index] = current;
    }

    const uint64_t frequency = static_cast<uint64_t>(gFpsCounterFrequency.QuadPart);
    const auto rateMilli = [&](uint64_t frames) {
        return frame_telemetry::RateMilli(frames, frequency, elapsedTicks);
    };
    gRealFpsMilli.store(rateMilli(realFrames), std::memory_order_relaxed);
    gDlssFpsMilli.store(gFpsWindowOutputAvailable
        ? rateMilli(gFpsWindowOutputFrames) : 0u,
        std::memory_order_relaxed);
    gFpsOutputSource.store(gFpsWindowOutputAvailable ? outputSource : 0u,
        std::memory_order_relaxed);
    gFpsSampleWindowMs.store(static_cast<uint32_t>(
        std::min<uint64_t>(UINT32_MAX,
            (elapsedTicks * 1000u + frequency / 2u) / frequency)),
        std::memory_order_relaxed);
    gFpsSampleTick.store(GetTickCount64(), std::memory_order_release);
    gFpsWindowStart = now;
    gFpsWindowOutputFrames = 0;
    gFpsWindowOutputAvailable = outputAvailable;
}

void ResetIntervalFpsWindow(
    const temporal_interval_trace::Snapshot& intervalTrace,
    const LARGE_INTEGER& now) noexcept
{
    gIntervalFpsWindowStart = now;
    for (size_t index = 0;
         index < temporal_interval_trace::kFirstSampleHandleCapacity;
         ++index)
    {
        gIntervalFpsWindowFirstSamples[index] =
            intervalTrace.firstSampleCounters[index].samples;
    }
}

void UpdateFpsTelemetryWithoutPresentCallback()
{
    std::lock_guard telemetryLock(gFpsTelemetryMutex);
    const uint64_t nowTick = GetTickCount64();
    if (!gAppliedFrameGenerationOn.load(std::memory_order_acquire))
    {
        if (gRealFpsMilli.load(std::memory_order_relaxed) != 0
            || gDlssFpsMilli.load(std::memory_order_relaxed) != 0)
        {
            ResetFpsTelemetry();
        }
        gFpsTelemetryActive = false;
        gFpsOutputPresentTick.store(0, std::memory_order_release);
        gIntervalFpsWindowStart = {};
        gIntervalFpsWindowFirstSamples.fill(0);
        return;
    }

    const uint64_t presentTick = gFpsOutputPresentTick.load(
        std::memory_order_acquire);
    const bool presentSamplerCurrent = presentTick != 0
        && nowTick >= presentTick && nowTick - presentTick <= 2000;
    const temporal_interval_trace::Snapshot intervalTrace =
        temporal_interval_trace::ReadSnapshot();
    LARGE_INTEGER now{};
    if (!intervalTrace.initialized || !intervalTrace.enabled
        || !QueryPerformanceCounter(&now))
    {
        return;
    }

    if (presentSamplerCurrent)
    {
        ResetIntervalFpsWindow(intervalTrace, now);
        return;
    }

    // No ReShade final-present callback is available in the CET-only package.
    // Count index-1 temporal requests as real input frames, then scale by the
    // runtime-reported presentation multiplier. This preserves correct real
    // cadence and provides a useful DLSS output estimate without adding a
    // process-wide DXGI Present hook solely for UI telemetry.
    if (gFpsTelemetryActive)
    {
        gFpsTelemetryActive = false;
        gFpsWindowStart = {};
        gFpsWindowOutputFrames = 0;
    }
    if (gFpsCounterFrequency.QuadPart == 0
        && !QueryPerformanceFrequency(&gFpsCounterFrequency))
    {
        return;
    }
    if (gIntervalFpsWindowStart.QuadPart == 0
        || now.QuadPart <= gIntervalFpsWindowStart.QuadPart)
    {
        ResetIntervalFpsWindow(intervalTrace, now);
        gRealFpsMilli.store(0, std::memory_order_relaxed);
        gDlssFpsMilli.store(0, std::memory_order_relaxed);
        gFpsSampleWindowMs.store(0, std::memory_order_relaxed);
        gFpsSampleTick.store(0, std::memory_order_release);
        return;
    }

    const uint64_t elapsedTicks = static_cast<uint64_t>(
        now.QuadPart - gIntervalFpsWindowStart.QuadPart);
    const uint64_t minimumTicks = static_cast<uint64_t>(
        gFpsCounterFrequency.QuadPart) / 2;
    if (elapsedTicks < minimumTicks)
        return;

    uint64_t realFrames = 0;
    for (size_t index = 0;
         index < temporal_interval_trace::kFirstSampleHandleCapacity;
         ++index)
    {
        const uint64_t current =
            intervalTrace.firstSampleCounters[index].samples;
        const uint64_t previous = gIntervalFpsWindowFirstSamples[index];
        if (current >= previous)
            realFrames = std::max(realFrames, current - previous);
    }

#if !defined(MFG_UNLOCK_SINGLE_MODULE_UI)
    uint32_t presentedMultiplier = gActualFramesPresented.load(
        std::memory_order_relaxed);
    if (presentedMultiplier < kMinimumMultiplier
        && intervalTrace.lastCount >= 1 && intervalTrace.lastCount <= 5)
    {
        presentedMultiplier = static_cast<uint32_t>(
            intervalTrace.lastCount + 1);
    }
    presentedMultiplier = std::clamp(
        presentedMultiplier, kMinimumMultiplier, kMaximumMultiplier);
#endif
    const uint64_t frequency = static_cast<uint64_t>(
        gFpsCounterFrequency.QuadPart);
    const auto rateMilli = [&](uint64_t frames) {
        return static_cast<uint32_t>(std::min<uint64_t>(UINT32_MAX,
            (frames * frequency * 1000u + elapsedTicks / 2u) / elapsedTicks));
    };
    gRealFpsMilli.store(rateMilli(realFrames), std::memory_order_relaxed);
#if defined(MFG_UNLOCK_SINGLE_MODULE_UI)
    // The selected mode and the latest GetState result are not output counts
    // for this time window. Keep the real rate if DXGI telemetry is unavailable.
    gDlssFpsMilli.store(0, std::memory_order_relaxed);
    gFpsOutputSource.store(0, std::memory_order_relaxed);
#else
    gDlssFpsMilli.store(rateMilli(realFrames * presentedMultiplier),
        std::memory_order_relaxed);
    gFpsOutputSource.store(3, std::memory_order_relaxed);
#endif
    gFpsSampleWindowMs.store(static_cast<uint32_t>(
        std::min<uint64_t>(UINT32_MAX,
            (elapsedTicks * 1000u + frequency / 2u) / frequency)),
        std::memory_order_relaxed);
    gFpsSampleTick.store(nowTick, std::memory_order_release);
    ResetIntervalFpsWindow(intervalTrace, now);
}

void RecordDlssgStateResult(
    sl::Result result, const sl::DLSSGState& state)
{
    gGetStateCalls.fetch_add(1, std::memory_order_relaxed);
    gGetStateSeen.store(true, std::memory_order_release);
    gLastGetStateResult.store(static_cast<int32_t>(result), std::memory_order_relaxed);
    if (!DlssgStateAvailable(result))
    {
        gFgVsyncSupportKnown.store(false, std::memory_order_release);
        gFgVsyncSupported.store(false, std::memory_order_release);
        return;
    }

    const uint32_t previous =
        gActualFramesPresented.exchange(state.numFramesActuallyPresented,
            std::memory_order_relaxed);
    gDlssgStatus.store(static_cast<uint32_t>(state.status), std::memory_order_relaxed);
    if (state.structVersion >= sl::kStructVersion2)
        gNumFramesToGenerateMax.store(
            UseAmpere() ? std::min(state.numFramesToGenerateMax, kMaximumMultiplier - 1u)
                : state.numFramesToGenerateMax, std::memory_order_relaxed);
    bool dynamicChanged = false;
    if (state.structVersion < sl::kStructVersion4)
    {
        gFgVsyncSupportKnown.store(false, std::memory_order_release);
        gFgVsyncSupported.store(false, std::memory_order_release);
    }
    if (state.structVersion >= sl::kStructVersion4)
    {
        // Dynamic V-Sync additionally requires the verified 2.14.1 runtime
        // and D3D12 route; this field alone remains a general FG capability.
        const bool vsyncKnown = state.bIsVsyncSupportAvailable == sl::Boolean::eTrue
            || state.bIsVsyncSupportAvailable == sl::Boolean::eFalse;
        gFgVsyncSupported.store(vsyncKnown
            && state.bIsVsyncSupportAvailable == sl::Boolean::eTrue,
            std::memory_order_relaxed);
        gFgVsyncSupportKnown.store(vsyncKnown, std::memory_order_release);
        const bool known = state.bIsDynamicMFGSupported == sl::Boolean::eTrue
            || state.bIsDynamicMFGSupported == sl::Boolean::eFalse;
        const bool supported = known
            && state.bIsDynamicMFGSupported == sl::Boolean::eTrue;
        dynamicChanged = gDynamicMfgSupported.exchange(supported,
            std::memory_order_relaxed) != supported;
        dynamicChanged = (gDynamicMfgCapabilityKnown.exchange(known,
            std::memory_order_release) != known) || dynamicChanged;
    }
    gStateSampleTick.store(GetTickCount64(), std::memory_order_release);
    bool capacityChanged = false;
    if (UseAmpere())
    {
        static std::atomic<uint32_t> previousCapacity{0};
        const uint32_t capacity = ampere_backend::CertifiedMaximumGeneratedFrames();
        capacityChanged = previousCapacity.exchange(capacity) != capacity;
    }
    if (capacityChanged || dynamicChanged)
    {
        // A bootstrap clamp or unsupported Dynamic request is not the desired
        // final mode. Reapply after the owned allocation/capability changes on
        // either supported GPU family, including late Ada initialization.
        gAppliedRevision.store(0, std::memory_order_release);
        gAttemptedRevision.store(0, std::memory_order_release);
    }

    if (!UseAmpere() && previous != state.numFramesActuallyPresented)
        Log(L"DLSS-G state sample: frames presented since prior query=%u "
            L"(maximum generated per real frame=%u, status=%u)",
            state.numFramesActuallyPresented,
            gNumFramesToGenerateMax.load(std::memory_order_relaxed),
            static_cast<uint32_t>(state.status));
}

bool DynamicMfgCapabilityKnown() noexcept
{
    return gDynamicMfgCapabilityKnown.load(std::memory_order_acquire);
}

bool DynamicMfgSupported() noexcept
{
    return DynamicMfgCapabilityKnown()
        && nvidia_mfg_policy::DynamicRangeFits(
            gNumFramesToGenerateMax.load(std::memory_order_acquire), SafeMaximumMultiplier())
        && gActiveNgxGraphicsApi.load(std::memory_order_acquire)
            != static_cast<uint32_t>(NgxGraphicsApi::eVulkan)
        && (!UseAmpere() || SafeMaximumMultiplier() == ampere_policy::kMaximumGeneratedFrames + 1u)
        && gDynamicMfgSupported.load(std::memory_order_acquire);
}

std::wstring ParentPath(const std::wstring& path)
{
    const auto separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? std::wstring{} : path.substr(0, separator);
}

std::wstring JoinPath(const std::wstring& left, const std::wstring& right)
{
    if (left.empty())
        return right;
    if (left.back() == L'\\' || left.back() == L'/')
        return left + right;
    return left + L"\\" + right;
}

uint32_t ClassifyLoadedRoute(const std::wstring& path)
{
    if (!gExecutableDirectory.empty()
        && _wcsicmp(ParentPath(path).c_str(), gExecutableDirectory.c_str()) == 0)
        return kRouteLocal;
    return kRouteExternal;
}

bool AdapterVerifiedForApi(NgxGraphicsApi api) noexcept
{
    if (UseAmpere())
    {
        return api == NgxGraphicsApi::eD3D12 && gpu_backend::AdapterVerified();
    }
    return gpu_backend::AdapterVerified()
        && (api != NgxGraphicsApi::eVulkan
            || gVulkanAdapterVerified.load(std::memory_order_acquire));
}

entry_detour::Snapshot EffectiveNgxDetour(
    entry_detour::Kind providerKind,
    entry_detour::Kind runtimeKind, bool create)
{
    const entry_detour::Handle activeHandle = UnpackEntryHandle(
        create ? gActiveNgxCreateHandle.load(std::memory_order_acquire)
               : gActiveNgxEvaluateHandle.load(std::memory_order_acquire));
    if (activeHandle)
        return entry_detour::ReadSnapshot(activeHandle);
    // Aggregate discovery is useful only before a provider is selected. Once
    // the first FG Create identifies a provider, never borrow coverage from a
    // different provider merely because it exposes the same operation.
    if (gActiveNgxProviderBase.load(std::memory_order_acquire) != 0)
        return {};
    const entry_detour::Snapshot provider =
        entry_detour::ReadSnapshot(providerKind);
    const entry_detour::Snapshot runtime =
        entry_detour::ReadSnapshot(runtimeKind);
    const NgxDispatchRoute route = static_cast<NgxDispatchRoute>(
        gActiveNgxDispatchRoute.load(std::memory_order_acquire));
    if (route == NgxDispatchRoute::eProvider)
        return provider;
    if (route == NgxDispatchRoute::eRuntime)
        return runtime;
    if (runtime.current)
        return runtime;
    return provider;
}

entry_detour::Snapshot EffectiveNgxCreateDetour()
{
    const NgxGraphicsApi api = static_cast<NgxGraphicsApi>(
        gActiveNgxGraphicsApi.load(std::memory_order_acquire));
    if (api == NgxGraphicsApi::eVulkan)
    {
        entry_detour::Snapshot create = EffectiveNgxDetour(
            entry_detour::Kind::eNgxVulkanCreateFeature,
            entry_detour::Kind::eNgxRuntimeVulkanCreateFeature, true);
        return create.current ? create : EffectiveNgxDetour(
            entry_detour::Kind::eNgxVulkanCreateFeature1,
            entry_detour::Kind::eNgxRuntimeVulkanCreateFeature1, true);
    }
    entry_detour::Snapshot d3d12 = EffectiveNgxDetour(
        entry_detour::Kind::eNgxD3D12CreateFeature,
        entry_detour::Kind::eNgxRuntimeD3D12CreateFeature, true);
    if (api == NgxGraphicsApi::eD3D12 || d3d12.current)
        return d3d12;
    entry_detour::Snapshot vulkan = EffectiveNgxDetour(
        entry_detour::Kind::eNgxVulkanCreateFeature,
        entry_detour::Kind::eNgxRuntimeVulkanCreateFeature, true);
    return vulkan.current ? vulkan : EffectiveNgxDetour(
        entry_detour::Kind::eNgxVulkanCreateFeature1,
        entry_detour::Kind::eNgxRuntimeVulkanCreateFeature1, true);
}

entry_detour::Snapshot EffectiveNgxEvaluateDetour()
{
    const NgxGraphicsApi api = static_cast<NgxGraphicsApi>(
        gActiveNgxGraphicsApi.load(std::memory_order_acquire));
    if (api == NgxGraphicsApi::eVulkan)
    {
        return EffectiveNgxDetour(
            entry_detour::Kind::eNgxVulkanEvaluateFeature,
            entry_detour::Kind::eNgxRuntimeVulkanEvaluateFeature, false);
    }
    entry_detour::Snapshot d3d12 = EffectiveNgxDetour(
        entry_detour::Kind::eNgxD3D12EvaluateFeature,
        entry_detour::Kind::eNgxRuntimeD3D12EvaluateFeature, false);
    return api == NgxGraphicsApi::eD3D12 || d3d12.current ? d3d12
        : EffectiveNgxDetour(
            entry_detour::Kind::eNgxVulkanEvaluateFeature,
            entry_detour::Kind::eNgxRuntimeVulkanEvaluateFeature, false);
}

bool BridgeReady()
{
    ControlRouteRecord* control = ActiveControlRoute();
    if (control
        && !control->structureCompatible.load(std::memory_order_acquire))
    {
        // Preserve the more specific unknown-version or malformed-chain code
        // published by the adapter which rejected this route.
        return false;
    }
    const entry_detour::Snapshot create = EffectiveNgxCreateDetour();
    const entry_detour::Snapshot evaluate = EffectiveNgxEvaluateDetour();
    const bool awaitingOptions = control
        && control->activeSetterPath.load(std::memory_order_acquire)
            == static_cast<uint32_t>(ControlEntryPath::eNone)
        && control->setterCalls.load(std::memory_order_acquire) == 0
        && gNgxFrameGenerationCreateCalls.load(std::memory_order_acquire) == 0
        && gActiveNgxProviderBase.load(std::memory_order_acquire) == 0;
    const universal_route_policy::Readiness state{
        control != nullptr,
        control && control->wrapperPatched,
        control && control->structureCompatible.load(
            std::memory_order_acquire),
        control && (awaitingOptions ? SetterEntryCovered(*control)
                                   : ActiveSetterCovered(*control)),
        control && StateEntryCovered(*control),
        gActiveNgxProviderBase.load(std::memory_order_acquire) != 0,
        gProviderChangedAfterCreate.load(std::memory_order_acquire),
        create.current,
        evaluate.current,
        AdapterVerifiedForApi(static_cast<NgxGraphicsApi>(
            gActiveNgxGraphicsApi.load(std::memory_order_acquire))),
        gpu_backend::Ready()
            && (!gpu_dispatch::IsAda() || ngx_mfg_gate::Ready(reinterpret_cast<HMODULE>(
                gActiveNgxProviderBase.load(std::memory_order_acquire))))
            && gFirstCreateMidpointReady.load(std::memory_order_acquire),
        awaitingOptions,
    };
    const UniversalRouteFailure failure =
        universal_route_policy::EvaluateReadiness(state);
    SetUniversalRouteFailure(failure);
    return failure == UniversalRouteFailure::eNone;
}

const char* PatchRouteName()
{
    const uintptr_t remix = gRemixRuntimeBase.load(std::memory_order_acquire);
    if (remix && remix != UINTPTR_MAX && !ActiveControlRoute())
        return "remix"; // Detection only; Remix owns FG options and allocation.
    if (!BridgeReady())
        return "pending";

    ControlRouteRecord* control = ActiveControlRoute();
    if (!control)
        return "pending";
    return ClassifyLoadedRoute(control->path) == kRouteLocal
        ? "local" : "external";
}

bool IsRegularFile(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool FindJsonValue(const std::string& content, const char* name, size_t& value)
{
    const std::string key = std::string("\"") + name + "\"";
    const auto keyOffset = content.find(key);
    if (keyOffset == std::string::npos)
        return false;
    const auto colon = content.find(':', keyOffset + key.size());
    if (colon == std::string::npos)
        return false;
    value = content.find_first_not_of(" \t\r\n", colon + 1);
    return value != std::string::npos;
}

bool TryParseUnsigned(const std::string& content, const char* name,
    uint32_t minimum, uint32_t maximum, uint32_t& value)
{
    size_t offset = 0;
    if (!FindJsonValue(content, name, offset) || content[offset] < '0' || content[offset] > '9')
        return false;

    uint64_t parsed = 0;
    size_t end = offset;
    while (end < content.size() && content[end] >= '0' && content[end] <= '9')
    {
        parsed = parsed * 10 + static_cast<uint32_t>(content[end] - '0');
        if (parsed > maximum)
            return false;
        ++end;
    }
    if (parsed < minimum || parsed > maximum || !ui_status_json::ValueEnd(content, end))
        return false;
    value = static_cast<uint32_t>(parsed);
    return true;
}

bool TryParseBoolean(const std::string& content, const char* name, bool& value)
{
    size_t offset = 0;
    if (!FindJsonValue(content, name, offset))
        return false;
    if (content.compare(offset, 4, "true") == 0)
    {
        value = true;
        return true;
    }
    if (content.compare(offset, 5, "false") == 0)
    {
        value = false;
        return true;
    }
    return false;
}

bool TryParseControl(const char* data, size_t size, ControlConfig& control)
{
    if (!data || size == 0)
        return false;

    const std::string content(data, size);
    if (!ui_status_json::CompleteObject(content))
        return false;
    ControlConfig parsed{};
    size_t followGameOffset = 0;
    if (FindJsonValue(content, "followGame", followGameOffset))
    {
        if (!TryParseBoolean(content, "followGame", parsed.followGame))
            return false;
    }
    else
    {
        // Configs written before Follow game support represented an explicit
        // fixed/dynamic override. Preserve that meaning during migration.
        parsed.followGame = false;
    }
    if (!TryParseUnsigned(content, "multiplier",
        1u, kMaximumMultiplier, parsed.multiplier))
        return false;

    size_t modeOffset = 0;
    if (FindJsonValue(content, "mode", modeOffset))
    {
        if (content.compare(modeOffset, 9, "\"dynamic\"") == 0)
            parsed.dynamic = true;
        else if (content.compare(modeOffset, 8, "\"follow\"") == 0
            && parsed.followGame)
            parsed.dynamic = false;
        else if (content.compare(modeOffset, 7, "\"fixed\"") != 0)
            return false;
    }

    if (parsed.followGame)
        parsed.dynamic = false;

    size_t targetOffset = 0;
    if (FindJsonValue(content, "dynamicTargetFrameRate", targetOffset)
        && !TryParseUnsigned(content, "dynamicTargetFrameRate", 0, 1000,
            parsed.dynamicTargetFrameRate))
        return false;

    size_t presetOffset = 0;
    if (FindJsonValue(content, "dlssgPreset", presetOffset)
        && !TryParseUnsigned(content, "dlssgPreset", 0, 2, parsed.dlssgPreset))
        return false;

    size_t experimentalOffset = 0;
    size_t pacingOffset = 0;
    if (FindJsonValue(content, "vsyncMode", pacingOffset)
        && !TryParseUnsigned(content, "vsyncMode", 0, 2, parsed.vsyncMode))
        return false;
    if (FindJsonValue(content, "reflexFrameLimitFps", pacingOffset)
        && !TryParseUnsigned(content, "reflexFrameLimitFps", 0, 1000,
            parsed.reflexFrameLimitFps))
        return false;
    if (FindJsonValue(content, "dynamicExperimental56", experimentalOffset)
        && !TryParseBoolean(content, "dynamicExperimental56",
            parsed.dynamicExperimental56))
        return false;

    size_t intervalLoggingOffset = 0;
    bool legacyIntervalLogging = gpu_dispatch::IsAda() || UseAmpere();
    if (FindJsonValue(content, "intervalLogging", intervalLoggingOffset)
        && !TryParseBoolean(content, "intervalLogging",
            legacyIntervalLogging))
        return false;
    // Both selected backends retain temporal-request tracing for FPS telemetry.
    // Accept legacy false settings without allowing them to disable collection.
    parsed.intervalLogging = gpu_dispatch::IsAda() || UseAmpere()
        || legacyIntervalLogging;

    size_t generatedOnlyOffset = 0;
    if (FindJsonValue(content, "generatedOnlyDebug", generatedOnlyOffset)
        && !TryParseBoolean(content, "generatedOnlyDebug",
            parsed.generatedOnlyDebug))
        return false;

    size_t selectiveWrapperOffset = 0;
    if (FindJsonValue(content, "selectiveOtaDlssgWrapper",
            selectiveWrapperOffset)
        && !TryParseBoolean(content, "selectiveOtaDlssgWrapper",
            parsed.selectiveOtaDlssgWrapper))
    {
        return false;
    }
    // Protocol 18 retires single-wrapper redirection. Keep accepting the old
    // key so existing configs migrate without being rejected, but never arm it.
    parsed.selectiveOtaDlssgWrapper = false;

    if (UseAmpere())
    {
        if (!ampere_policy::ControlValid(parsed.multiplier, parsed.dynamic,
                parsed.dynamicExperimental56, parsed.generatedOnlyDebug)) return false;
    }
    parsed.dynamicExperimental56 = false;
    control = parsed;
    return true;
}

bool ReadControlFile(const std::wstring& path, ControlConfig& control)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    std::array<char, 4096> buffer{};
    DWORD bytesRead = 0;
    const BOOL read = ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr);
    CloseHandle(file);
    return read && TryParseControl(buffer.data(), bytesRead, control);
}

bool ReadLastWriteTime(const std::wstring& path, FILETIME& writeTime)
{
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes))
        return false;
    writeTime = attributes.ftLastWriteTime;
    return true;
}

ControlConfig ReadInitialControl()
{
    ControlConfig control{};
    wchar_t value[16]{};
    const DWORD length = GetEnvironmentVariableW(
        MFG_ENV_PREFIX_W L"ACTIVE_MULTIPLIER", value, _countof(value));
    if (length == 1 && value[0] >= L'2' && value[0] <= L'6')
    {
        control.multiplier = static_cast<uint32_t>(value[0] - L'0');
        control.followGame = false; // Preserve an explicit environment override.
    }

    ControlConfig fileControl{};
    if (ReadControlFile(gConfigPath, fileControl))
        return fileControl;

#if defined(MFG_UNLOCK_UNIVERSAL_CONFIG)
    // A dedicated CET install keeps its live control inside CET's mandatory
    // per-mod filesystem sandbox. Preserve an existing executable-side choice
    // as the first-launch fallback; the next CET selection is written to the
    // sandbox-local file watched by this core.
    const std::wstring executableConfig = JoinPath(
        gExecutableDirectory, MFG_CONFIG_W);
    if (_wcsicmp(gConfigPath.c_str(), executableConfig.c_str()) != 0
        && ReadControlFile(executableConfig, fileControl))
    {
        return fileControl;
    }
#endif
    return control;
}

uint32_t ReadInitialDlssgPreset() noexcept
{
    // Called at the first provider settings query if the worker has not yet
    // published control. Registering this callback in DllMain performs no I/O.
    try { return ReadInitialControl().dlssgPreset; }
    catch (...) { return 2; }
}

std::wstring ResolveConfigPath(HMODULE instance, const std::wstring& executableDirectory)
{
#if MFG_UNLOCK_RUNTIME_GPU_SELECTION
    // The integrated UI owns this transport; an old CET script is not a
    // consumer of the unified filenames and must not redirect the worker.
    return unified_control_paths::Config(executableDirectory);
#endif
    if (UseAmpere())
    {
        return JoinPath(executableDirectory, MFG_CONFIG_W);
    }
    std::wstring explicitPath(32768, L'\0');
    const DWORD explicitLength = GetEnvironmentVariableW(
        MFG_CONFIG_ENV_W, explicitPath.data(),
        static_cast<DWORD>(explicitPath.size()));
    if (explicitLength > 0 && explicitLength < explicitPath.size())
    {
        explicitPath.resize(explicitLength);
        return explicitPath;
    }

#if defined(MFG_UNLOCK_UNIVERSAL_CONFIG)
#if !(MFG_UNLOCK_OUTPUT_PULL_EXPERIMENT || MFG_UNLOCK_OUTPUT_PULL_TELEMETRY)
    // The isolated OutputPull builds pair with the executable-side ReShade UI.
    // Preserve the stock CET autodetection only outside this experiment.
    const std::wstring cetDirectory = JoinPath(executableDirectory,
        L"plugins\\cyber_engine_tweaks\\mods\\RTX40MFG");
    if (IsRegularFile(JoinPath(cetDirectory, L"init.lua")))
    {
        return JoinPath(cetDirectory, MFG_CONFIG_W);
    }
#endif
    return JoinPath(executableDirectory, MFG_CONFIG_W);
#else
    const std::wstring cetPath = JoinPath(executableDirectory,
        L"plugins\\cyber_engine_tweaks\\mods\\RTX40MFG\\config.json");
    if (IsRegularFile(cetPath))
        return cetPath;

    std::wstring modulePath(32768, L'\0');
    const DWORD moduleLength = GetModuleFileNameW(instance,
        modulePath.data(), static_cast<DWORD>(modulePath.size()));
    modulePath.resize(moduleLength < modulePath.size() ? moduleLength : 0);
    const std::wstring legacyPath = JoinPath(
        ParentPath(ParentPath(modulePath)), L"config.json");
    return IsRegularFile(legacyPath) ? legacyPath : cetPath;
#endif
}

std::wstring ResolveStatusPath(const std::wstring& configPath,
    const std::wstring& executableDirectory)
{
#if MFG_UNLOCK_RUNTIME_GPU_SELECTION
    return unified_control_paths::Status(configPath, executableDirectory);
#endif
    if (UseAmpere())
    {
        return JoinPath(executableDirectory, MFG_STATUS_W);
    }
    std::wstring explicitPath(32768, L'\0');
    const DWORD explicitLength = GetEnvironmentVariableW(
        MFG_STATUS_ENV_W, explicitPath.data(),
        static_cast<DWORD>(explicitPath.size()));
    if (explicitLength > 0 && explicitLength < explicitPath.size())
    {
        explicitPath.resize(explicitLength);
        return explicitPath;
    }

#if defined(MFG_UNLOCK_UNIVERSAL_CONFIG)
    const std::wstring universalConfig = JoinPath(
        executableDirectory, MFG_CONFIG_W);
    const std::wstring cetConfig = JoinPath(executableDirectory,
        L"plugins\\cyber_engine_tweaks\\mods\\RTX40MFG\\"
        MFG_CONFIG_W);
    if (_wcsicmp(configPath.c_str(), universalConfig.c_str()) == 0
        || _wcsicmp(configPath.c_str(), cetConfig.c_str()) == 0)
    {
        return JoinPath(ParentPath(configPath),
            MFG_STATUS_W);
    }
#endif
    return JoinPath(ParentPath(configPath), L"bridge_status.json");
}

uint64_t StoreControl(const ControlConfig& control)
{
    std::lock_guard controlLock(gControlSnapshotMutex);
    gDesiredFollowGame.store(control.followGame, std::memory_order_relaxed);
    gDesiredMultiplier.store(control.multiplier, std::memory_order_relaxed);
    gDesiredDynamicMode.store(control.dynamic, std::memory_order_relaxed);
    gDynamicTargetFrameRate.store(control.dynamicTargetFrameRate, std::memory_order_relaxed);
    gDlssgPresetRequested.store(control.dlssgPreset, std::memory_order_relaxed);
    gVsyncMode.store(control.vsyncMode, std::memory_order_relaxed);
    gReflexFrameLimitFps.store(control.reflexFrameLimitFps, std::memory_order_relaxed);
    dlssg_preset::SetRequested(control.dlssgPreset);
    gDynamicExperimental56.store(control.dynamicExperimental56, std::memory_order_relaxed);
    gGeneratedOnlyDebug.store(control.generatedOnlyDebug,
        std::memory_order_relaxed);
    temporal_interval_trace::SetEnabled(gpu_dispatch::IsAda() || UseAmpere()
        || control.intervalLogging);
    const uint64_t revision = gDesiredRevision.fetch_add(1, std::memory_order_release) + 1;
    gControlReady.store(true, std::memory_order_release);
    return revision;
}

ControlSnapshot ReadControlSnapshot()
{
    std::lock_guard controlLock(gControlSnapshotMutex);
    ControlSnapshot snapshot{};
    for (;;)
    {
        const uint64_t before = gDesiredRevision.load(std::memory_order_acquire);
        snapshot.control.followGame =
            gDesiredFollowGame.load(std::memory_order_relaxed);
        snapshot.control.multiplier = gDesiredMultiplier.load(std::memory_order_relaxed);
        snapshot.control.dynamic = gDesiredDynamicMode.load(std::memory_order_relaxed);
        snapshot.control.dynamicTargetFrameRate =
            gDynamicTargetFrameRate.load(std::memory_order_relaxed);
        snapshot.control.dlssgPreset =
            gDlssgPresetRequested.load(std::memory_order_relaxed);
        snapshot.control.vsyncMode = gVsyncMode.load(std::memory_order_relaxed);
        snapshot.control.reflexFrameLimitFps =
            gReflexFrameLimitFps.load(std::memory_order_relaxed);
        snapshot.control.dynamicExperimental56 =
            gDynamicExperimental56.load(std::memory_order_relaxed);
        snapshot.control.generatedOnlyDebug =
            gGeneratedOnlyDebug.load(std::memory_order_relaxed);
        snapshot.control.intervalLogging = temporal_interval_trace::Enabled();
        const uint64_t after = gDesiredRevision.load(std::memory_order_acquire);
        if (before == after)
        {
            snapshot.revision = after;
            return snapshot;
        }
    }
}

void PublishLiveBridge(const ControlConfig& control)
{
    wchar_t multiplier[2]{ static_cast<wchar_t>(L'0' + std::clamp(
        control.multiplier, kMinimumMultiplier, kMaximumMultiplier)), L'\0' };
    wchar_t target[16]{};
    swprintf_s(target, L"%u", control.dynamicTargetFrameRate);
    SetEnvironmentVariableW(MFG_ENV_PREFIX_W L"ACTIVE_MULTIPLIER", multiplier);
    SetEnvironmentVariableW(MFG_ENV_PREFIX_W L"ACTIVE_MODE",
        control.followGame ? L"follow"
            : control.dynamic ? L"dynamic" : L"fixed");
    SetEnvironmentVariableW(MFG_ENV_PREFIX_W L"FOLLOW_GAME",
        control.followGame ? L"1" : L"0");
    SetEnvironmentVariableW(MFG_ENV_PREFIX_W L"DYNAMIC_TARGET", target);
    SetEnvironmentVariableW(MFG_ENV_PREFIX_W L"DYNAMIC_EXPERIMENTAL_56",
        control.dynamicExperimental56 ? L"1" : L"0");
    SetEnvironmentVariableW(MFG_ENV_PREFIX_W L"GENERATED_ONLY_DEBUG",
        control.generatedOnlyDebug ? L"1" : L"0");
    SetEnvironmentVariableW(MFG_ENV_PREFIX_W L"INTERVAL_LOGGING", L"1");
    SetEnvironmentVariableW(MFG_ENV_PREFIX_W L"AUTO_BRIDGE", L"1");
}

void PublishPatchRoute()
{
    const char* route = PatchRouteName();
    wchar_t wideRoute[16]{};
    MultiByteToWideChar(CP_UTF8, 0, route, -1, wideRoute, _countof(wideRoute));
    SetEnvironmentVariableW(MFG_ENV_PREFIX_W L"PATCH_ROUTE", wideRoute);
}

uint64_t UnixTimeSeconds()
{
    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    ULARGE_INTEGER ticks{};
    ticks.LowPart = time.dwLowDateTime;
    ticks.HighPart = time.dwHighDateTime;
    constexpr uint64_t kWindowsToUnixEpoch = 116444736000000000ULL;
    return (ticks.QuadPart - kWindowsToUnixEpoch) / 10000000ULL;
}

std::string JsonEscapeWide(const std::wstring& source)
{
    if (source.empty())
        return {};
    const int byteCount = WideCharToMultiByte(CP_UTF8,
        WC_ERR_INVALID_CHARS, source.data(), static_cast<int>(source.size()),
        nullptr, 0, nullptr, nullptr);
    if (byteCount <= 0)
        return {};
    std::string utf8(static_cast<size_t>(byteCount), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            source.data(), static_cast<int>(source.size()), utf8.data(),
            byteCount, nullptr, nullptr) != byteCount)
        return {};

    std::string escaped;
    escaped.reserve(utf8.size() + 16);
    constexpr char hex[] = "0123456789ABCDEF";
    for (const unsigned char value : utf8)
    {
        switch (value)
        {
        case '\\': escaped += "\\\\"; break;
        case '"': escaped += "\\\""; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (value < 0x20)
            {
                escaped += "\\u00";
                escaped.push_back(hex[value >> 4]);
                escaped.push_back(hex[value & 0x0F]);
            }
            else
            {
                escaped.push_back(static_cast<char>(value));
            }
            break;
        }
    }
    return escaped;
}

const char* ControlDetourMethod(const ControlRouteRecord* route,
    bool setter) noexcept
{
    if (!route)
        return "none";
    const ControlEntryPath path = static_cast<ControlEntryPath>(
        (setter ? route->activeSetterPath : route->activeStatePath).load(
            std::memory_order_acquire));
    if (path == ControlEntryPath::eResolver)
        return "resolver";
    entry_detour::Handle handle{};
    if (setter)
        handle = path == ControlEntryPath::eInternal
            ? route->internalSetHandle : route->publicSetHandle;
    else
        handle = path == ControlEntryPath::eInternal
            ? route->internalGetHandle : route->publicGetHandle;
    return entry_detour::MethodName(
        entry_detour::ReadSnapshot(handle).method);
}

struct PresentationRuntimeProof
{
    uintptr_t wrapper = 0;
    uint64_t wrapperGeneration = 0;
    uintptr_t provider = 0;
    uint64_t providerGeneration = 0;
    uint64_t createHandle = 0;
    uint64_t evaluateHandle = 0;
    uint64_t lifecycleEpoch = 0;
    bool operator==(const PresentationRuntimeProof&) const = default;
};

std::mutex gPresentationProofMutex;
PresentationRuntimeProof gPresentationProof{};
uint64_t gPresentationRuntimeEpoch = 0;
bool gPresentationProviderEligible = false;

PresentationRuntimeProof CurrentPresentationProof() noexcept
{
    const auto* route = ActiveControlRoute();
    return {route ? reinterpret_cast<uintptr_t>(route->wrapper) : 0,
        route ? route->generation : 0,
        gActiveNgxProviderBase.load(std::memory_order_acquire),
        gActiveNgxProviderGeneration.load(std::memory_order_acquire),
        gActiveNgxCreateHandle.load(std::memory_order_acquire),
        gActiveNgxEvaluateHandle.load(std::memory_order_acquire),
        gPresentationLifecycleEpoch.load(std::memory_order_acquire)};
}

bool FrameGenerationRuntimeCurrent() noexcept
{
    // Callback-time validation performs no filesystem or NVIDIA calls. The
    // worker binds its version check to this exact selected route generation.
    const auto* route = ActiveControlRoute();
    if (!route || gActiveNgxGraphicsApi.load(std::memory_order_acquire)
            == static_cast<uint32_t>(NgxGraphicsApi::eUnknown)
        || !gAppliedFrameGenerationOn.load(std::memory_order_acquire)
        || gRestartRequired.load(std::memory_order_acquire)
        || !BridgeReady()) return false;
    const auto current = CurrentPresentationProof();
    if (current.wrapper != reinterpret_cast<uintptr_t>(route->wrapper)
        || current.wrapperGeneration != route->generation) return false;
    std::lock_guard lock(gPresentationProofMutex);
    return current.provider && current.providerGeneration
        && current.createHandle && current.evaluateHandle
        && current == gPresentationProof;
}

bool PresentationRuntimeCurrent() noexcept
{
    const auto* route = ActiveControlRoute();
    if (!route || route->version.major != 2 || route->version.minor != 14
        || route->version.build != 1 || !FrameGenerationRuntimeCurrent()) return false;
    std::lock_guard lock(gPresentationProofMutex);
    return gPresentationProviderEligible;
}

bool ReflexRuntimeCurrent() noexcept
{
    // The manual cap is suspended for both requested and game-owned Dynamic
    // mode. Configure retains its saved value and the next proven Reflex
    // callback restores the game's own options before any later re-enable.
    if (gDesiredDynamicMode.load(std::memory_order_acquire)
        || gAppliedDynamicMode.load(std::memory_order_acquire)
        || !FrameGenerationRuntimeCurrent()) return false;
    // Fixed D3D12 limiting uses the stable public Reflex API; it does not
    // depend on the private Dynamic/V-Sync implementation or provider 310.9.1.
    // Keep the existing Vulkan compatibility boundary intact.
    return gActiveNgxGraphicsApi.load(std::memory_order_acquire)
            == static_cast<uint32_t>(NgxGraphicsApi::eD3D12)
        || PresentationRuntimeCurrent();
}

bool VsyncRuntimeCurrent() noexcept
{
    // The optional V-Sync override is omitted from this release. Retain its
    // saved field and ABI slots without authorizing a presentation mutation.
    return false;
}

void UpdatePresentationPolicy(const ControlConfig& control)
{
    const auto current = CurrentPresentationProof();
    bool changed = false;
    {
        std::lock_guard lock(gPresentationProofMutex);
        changed = current != gPresentationProof;
    }
    if (changed)
    {
        const auto version = ReadFileVersion(LoadedModulePath(
            reinterpret_cast<HMODULE>(current.provider)));
        std::lock_guard lock(gPresentationProofMutex);
        gPresentationProviderEligible = current.provider != 0
            && current.providerGeneration != 0 && version.major == 310
            && version.minor == 9 && version.build == 1;
        gPresentationProof = current;
        ++gPresentationRuntimeEpoch;
    }
    reflex_control::Configure(control.reflexFrameLimitFps, ReflexRuntimeCurrent());
    vsync_control::Configure(0, false, gPresentationRuntimeEpoch);
}

void DiscoverReflexModule()
{
    // This function is called only by the normal worker, after loader discovery
    // has returned. Never install this transport from a LoadLibrary callback.
    std::vector<ModuleRecord> modules;
    {
        std::lock_guard lock(gModuleMutex);
        for (const auto& record : gModuleRecords)
        {
            // This export identifies plugin candidates, including NVIDIA OTA
            // filenames. ObserveModule then requires the owned Reflex entries.
            if (record.wrapperExport) modules.push_back(record);
        }
    }
    for (const auto& record : modules)
        reflex_control::ObserveModule(record.module, record.path.c_str(), record.generation);
}

bool WriteBridgeStatus(const ControlConfig& control, DWORD pid)
{
    if (gStatusPath.empty())
        return false;

    UpdateFpsTelemetryWithoutPresentCallback();
    const uint32_t uiViewport = gLastOptionsViewport.load(std::memory_order_acquire);
    const UiInputSnapshot uiInputs = ReadUiInputSnapshot(uiViewport);
    const bool bridgeReady = BridgeReady();
    const char* route = PatchRouteName();
    const uint64_t desiredRevision = gDesiredRevision.load(std::memory_order_acquire);
    const uint64_t appliedRevision = gAppliedRevision.load(std::memory_order_acquire);
    const bool setOptionsSeen = gSetOptionsSeen.load(std::memory_order_acquire);
    const bool getStateSeen = gGetStateSeen.load(std::memory_order_acquire);
    const bool gameFrameGenerationOn =
        gGameFrameGenerationOn.load(std::memory_order_acquire);
    const bool appliedFrameGenerationOn =
        gAppliedFrameGenerationOn.load(std::memory_order_acquire);
    const int32_t setOptionsResult =
        gLastSetOptionsResult.load(std::memory_order_relaxed);
    const int32_t getStateResult =
        gLastGetStateResult.load(std::memory_order_relaxed);
    const bool setOptionsAccepted = setOptionsResult == static_cast<int32_t>(sl::Result::eOk)
        || setOptionsResult == static_cast<int32_t>(sl::Result::eWarnOutOfVRAM);
    const bool applied = appliedFrameGenerationOn && appliedRevision != 0
        && setOptionsAccepted;
    const bool pending = gameFrameGenerationOn && desiredRevision != appliedRevision;
    const uint64_t stateTick = gStateSampleTick.load(std::memory_order_acquire);
    const uint64_t nowTick = GetTickCount64();
    const uint64_t stateAgeMs = stateTick == 0 || nowTick < stateTick
        ? 0 : nowTick - stateTick;
    const uint64_t fpsTick = gFpsSampleTick.load(std::memory_order_acquire);
    const uint64_t fpsAgeMs = fpsTick == 0 || nowTick < fpsTick
        ? 0 : nowTick - fpsTick;
    const entry_detour::Snapshot setOptionsDetour =
        entry_detour::ReadSnapshot(entry_detour::Kind::eDlssgSetOptions);
    const entry_detour::Snapshot createDetour = EffectiveNgxCreateDetour();
    const entry_detour::Snapshot evaluateDetour = EffectiveNgxEvaluateDetour();
    const entry_detour::Snapshot getStateDetour =
        entry_detour::ReadSnapshot(entry_detour::Kind::eDlssgGetState);
    const entry_detour::Snapshot slInitDetour =
        entry_detour::ReadSnapshot(entry_detour::Kind::eSlInit);
    const bool slInitIatFallback =
        gSlInitIatFallbackInstalled.load(std::memory_order_acquire);
    const bool slInitResolverFallback =
        gSlInitResolverFallbackActive.load(std::memory_order_acquire);
    const bool slInitControlPathReady = slInitDetour.current
        || slInitIatFallback || slInitResolverFallback
        || gMainResolverDiscoveryInstalled.load(
            std::memory_order_acquire);
    const uint64_t slInitCalls =
        gSlInitCalls.load(std::memory_order_acquire);
    const uint64_t slInitFlagsAfter =
        gSlInitFlagsAfter.load(std::memory_order_acquire);
    const uint64_t allowOtaMask =
        static_cast<uint64_t>(sl::PreferenceFlags::eAllowOTA);
    const uint64_t downloadedPluginsMask = static_cast<uint64_t>(
        sl::PreferenceFlags::eLoadDownloadedPlugins);
    static_assert(allowOtaMask == streamline_ota_policy::kAllowOta);
    static_assert(downloadedPluginsMask
        == streamline_ota_policy::kLoadDownloadedPlugins);
    const uint32_t safeMaximumMultiplier = SafeMaximumMultiplier();
    const bool requestedMultiplierLimited =
        !control.followGame && !control.dynamic
        && control.multiplier > safeMaximumMultiplier;
    const bool setOptionsResolverFallbackActive =
        gSetOptionsResolverFallbackActive.load(std::memory_order_acquire);
    const uint64_t setOptionsResolverFallbackCalls =
        gSetOptionsResolverFallbackCalls.load(std::memory_order_acquire);
    const bool setOptionsControlPathReady = setOptionsDetour.current
        || (setOptionsResolverFallbackActive
            && setOptionsResolverFallbackCalls > 0);
    const temporal_interval_trace::Snapshot intervalTrace =
        temporal_interval_trace::ReadSnapshot();
    const auto preset = dlssg_preset::ReadSnapshot(reinterpret_cast<HMODULE>(
        gActiveNgxProviderBase.load(std::memory_order_acquire)));
    const bool restartRequired =
        gRestartRequired.load(std::memory_order_acquire);
    const nvidia_mfg_policy::CapacityDecision compatibilityCapacity =
        CurrentCapacityDecision();
    const uint32_t nvidiaCompatibilityTier =
        gNvidiaCompatibilityTier.load(std::memory_order_acquire);

    const ControlRouteRecord* activeControl = ActiveControlRoute();
    const ControlEntryPath activeSetterPath = activeControl
        ? static_cast<ControlEntryPath>(activeControl->activeSetterPath.load(
            std::memory_order_acquire)) : ControlEntryPath::eNone;
    const ControlEntryPath activeStatePath = activeControl
        ? static_cast<ControlEntryPath>(activeControl->activeStatePath.load(
            std::memory_order_acquire)) : ControlEntryPath::eNone;
    const std::string activeWrapperPath = JsonEscapeWide(
        activeControl ? activeControl->path : std::wstring{});
    const HMODULE activeProvider = reinterpret_cast<HMODULE>(
        gActiveNgxProviderBase.load(std::memory_order_acquire));
    const std::wstring activeProviderPathWide = activeProvider
        ? LoadedModulePath(activeProvider) : std::wstring{};
    const std::string activeProviderPath =
        JsonEscapeWide(activeProviderPathWide);
    const FileVersion activeProviderVersion =
        ReadFileVersion(activeProviderPathWide);
    const uint32_t selectionSource = gActiveNgxSelectionSource.load(
        std::memory_order_acquire);
    const char* selectionSourceName = selectionSource
            == static_cast<uint32_t>(
                NgxProviderSelectionSource::eProviderEntry)
        ? "provider-entry" : selectionSource
            == static_cast<uint32_t>(
                NgxProviderSelectionSource::eRuntimeCaller)
        ? "runtime-caller" : selectionSource
            == static_cast<uint32_t>(
                NgxProviderSelectionSource::eRuntimeUniqueCandidate)
        ? "runtime-unique-candidate" : selectionSource
            == static_cast<uint32_t>(NgxProviderSelectionSource::eRuntimeDispatchTable)
        ? "runtime-dispatch-table" : "none";
    const UniversalRouteFailure routeFailure =
        static_cast<UniversalRouteFailure>(gUniversalRouteFailure.load(
            std::memory_order_acquire));
    const entry_detour::Snapshot releaseDetour = activeControl
        ? entry_detour::ReadSnapshot(activeControl->freeResourcesHandle)
        : entry_detour::Snapshot{};

    char json[16384]{};
    const int length = sprintf_s(json,
        "{\"version\":" MFG_STATUS_VERSION ",\"pid\":%lu,\"processBirth\":%llu,\"heartbeat\":%llu,\"route\":\"%s\","
        "\"bridgeReady\":%s,\"liveHookInstalled\":%s,"
        "\"loaderCoreImported\":true,"
        "\"nvidiaCompatibilityResolved\":%s,"
        "\"nvidiaProfileStatus\":%d,"
        "\"nvidiaProfileName\":\"%s\","
        "\"nvidiaCompatibilityTier\":%u,"
        "\"nvidiaCompatibilityManifestEntries\":%u,"
        "\"nvidiaPolicyCeilingMultiplier\":%u,"
        "\"wrapperNativeMaximumMultiplier\":%u,"
        "\"compatibilityFallback\":%s,"
        "\"compatibilityReason\":%u,"
        "\"fullStreamlineOtaRequested\":%s,"
        "\"fullStreamlineOtaEligible\":%s,"
        "\"downloadedStreamlinePluginsForced\":%s,"
        "\"streamlineHostVersionMajor\":%u,"
        "\"streamlineHostVersionMinor\":%u,"
        "\"streamlineHostVersionBuild\":%u,"
        "\"streamlineHostVersionPrivate\":%u,"
        "\"mainResolverDiscoveryInstalled\":%s,"
        "\"slInitEntryDetourInstalled\":%s,"
        "\"slInitEntryDetourCurrent\":%s,"
        "\"slInitCachedPointersCovered\":%s,"
        "\"slInitEntryDetourFailure\":%u,"
        "\"slInitEntryRva\":%u,"
        "\"slInitIatFallbackInstalled\":%s,"
        "\"slInitResolverFallbackActive\":%s,"
        "\"slInitControlPathReady\":%s,"
        "\"slInitCalls\":%llu,"
        "\"slInitFlagsBefore\":%llu,\"slInitFlagsAfter\":%llu,"
        "\"otaPreferencesForced\":%s,\"otaPreferencesEnabledAtInit\":%s,"
        "\"downloadedStreamlinePluginsEnabledAtInit\":%s,"
        "\"otaProviderPreflightSupported\":%s,\"otaForceSuppressed\":%s,"
        "\"selectiveOtaDlssgWrapperRequested\":%s,"
        "\"selectiveOtaDlssgWrapperCandidateReady\":%s,"
        "\"selectiveOtaDlssgWrapperFailure\":%u,"
        "\"selectiveOtaDlssgWrapperRedirectAttempts\":%llu,"
        "\"selectiveOtaDlssgWrapperRedirectSuccesses\":%llu,"
        "\"selectiveOtaDlssgWrapperFallbacks\":%llu,"
        "\"selectiveOtaDlssgWrapperVersionMajor\":%u,"
        "\"selectiveOtaDlssgWrapperVersionMinor\":%u,"
        "\"selectiveOtaDlssgWrapperVersionBuild\":%u,"
        "\"selectiveOtaDlssgWrapperVersionPrivate\":%u,"
        "\"slCommonResolverDiscoveryInstalled\":%s,"
        "\"streamlineLoaderDiscoveryInstalled\":%s,"
        "\"streamlineLoaderDiscoveryCalls\":%llu,"
        "\"setOptionsEntryDetourInstalled\":%s,"
        "\"setOptionsEntryDetourCurrent\":%s,"
        "\"setOptionsCachedPointersCovered\":%s,"
        "\"setOptionsEntryDetourFailure\":%u,"
        "\"setOptionsEntryRva\":%u,"
        "\"setOptionsResolverFallbackActive\":%s,"
        "\"setOptionsResolverFallbackCalls\":%llu,"
        "\"setOptionsControlPathReady\":%s,"
        "\"getStateEntryDetourInstalled\":%s,"
        "\"getStateEntryDetourCurrent\":%s,"
        "\"getStateCachedPointersCovered\":%s,"
        "\"getStateEntryDetourFailure\":%u,"
        "\"getStateEntryRva\":%u,"
        "\"ngxCreateEntryDetourInstalled\":%s,"
        "\"ngxCreateEntryDetourCurrent\":%s,"
        "\"ngxCreateCachedPointersCovered\":%s,"
        "\"ngxCreateEntryDetourFailure\":%u,"
        "\"ngxCreateEntryRva\":%u,"
        "\"ngxEvaluateEntryDetourInstalled\":%s,"
        "\"ngxEvaluateEntryDetourCurrent\":%s,"
        "\"ngxEvaluateCachedPointersCovered\":%s,"
        "\"ngxEvaluateEntryDetourFailure\":%u,"
        "\"ngxEvaluateEntryRva\":%u,"
        "\"ngxCreateCalls\":%llu,"
        "\"ngxFrameGenerationCreateCalls\":%llu,"
        "\"ngxEvaluateCalls\":%llu,"
        "\"lastNgxCreateResultAvailable\":false,"
        "\"lastNgxCreateResult\":%d,"
        "\"frameGenerationCreateObserved\":%s,"
        "\"backportReadyAtCreate\":%s,"
        "\"pipelineMayPredateDetour\":%s,"
        "\"restartRequired\":%s,"
        "\"streamlineRebuildRequired\":%s,"
        "\"uiTagHookInstalled\":%s,"
        "\"activeWrapperObserved\":%s,\"activeWrapperPatched\":%s,"
        "\"activeWrapperUsesNvidiaOta\":%s,"
        "\"activeWrapperVersionMajor\":%u,"
        "\"activeWrapperVersionMinor\":%u,"
        "\"activeWrapperVersionBuild\":%u,"
        "\"activeWrapperVersionPrivate\":%u,"
        "\"wrapperCompiledMaximumGeneratedFrames\":%u,"
        "\"safeMaximumMultiplier\":%u,"
        "\"loadedWrapperCandidates\":%u,\"patchedWrapperCandidates\":%u,"
        "\"loadedNgxCandidates\":%u,\"patchedNgxCandidates\":%u,"
        "\"followGame\":%s,\"mode\":\"%s\",\"multiplier\":%u,"
        "\"dynamicTargetFrameRate\":%u,"
        "\"dynamicExperimental56\":%s,\"generatedOnlyDebug\":%s,"
        "\"forcedMaximumMultiplier\":%u,"
        "\"dlssgPresetRequested\":%u,\"dlssgPresetOverrideInstalled\":%s,"
        "\"dlssgPresetOverrideFailure\":%u,\"dlssgPresetReadCount\":%llu,"
        "\"dlssgCachedPresetInstalled\":%s,\"dlssgCachedPresetFailure\":%u,"
        "\"dlssgCachedPresetRva\":%u,\"dlssgCachedPresetReadCount\":%llu,"
        "\"requestedMultiplierLimited\":%s,"
        "\"intervalLoggingEnabled\":%s,\"intervalLogReady\":%s,"
        "\"intervalValidSamples\":%llu,\"intervalInvalidSamples\":%llu,"
        "\"intervalDroppedSamples\":%llu,\"intervalSeenCountMask\":%u,"
        "\"intervalSeenIndexMask\":%u,\"intervalLastCount\":%d,"
        "\"intervalLastIndex\":%d,\"intervalLastPositionNumerator\":%u,"
        "\"intervalLastPositionDenominator\":%u,"
        "\"intervalLogFile\":\"%ls\","
        "\"requestRevision\":%llu,\"appliedRevision\":%llu,"
        "\"applied\":%s,\"pending\":%s,\"gameFrameGenerationOn\":%s,"
        "\"appliedFrameGenerationOn\":%s,"
        "\"appliedMode\":\"%s\",\"appliedMultiplier\":%u,"
        "\"appliedDynamicTargetFrameRate\":%.3f,"
        "\"appliedDynamicTargetValid\":%s,"
        "\"appliedDynamicExperimental56\":%s,"
        "\"appliedGeneratedOnlyDebug\":%s,\"setOptionsSeen\":%s,"
        "\"setOptionsAccepted\":%s,"
        "\"setOptionsResult\":%d,\"getStateSeen\":%s,\"getStateResult\":%d,"
        "\"actualFramesPresented\":%u,\"numFramesToGenerateMax\":%u,"
        "\"realFpsMilli\":%u,\"dlssFpsMilli\":%u,"
        "\"fpsOutputSource\":%u,"
        "\"fpsSampleWindowMs\":%u,\"fpsSampleAgeMs\":%llu,"
        "\"dlssgStatus\":%u,\"dynamicMfgSupportKnown\":%s,"
        "\"dynamicMfgSupported\":%s,"
        "\"fgVsyncSupportKnown\":%s,\"fgVsyncSupported\":%s,"
        "\"gameOptionsStructVersion\":%u,\"gameUiRecompositionEnabled\":%s,"
        "\"gameHudlessBufferFormat\":%u,\"gameUiBufferFormat\":%u,"
        "\"hudlessTagActive\":%s,\"uiAlphaTagActive\":%s,"
        "\"uiColorAlphaTagActive\":%s,\"uiDimensionsKnown\":%s,"
        "\"uiDimensionsMatch\":%s,\"uiInputsReady\":%s,"
        "\"uiRecompositionEnabled\":%s,\"uiRecompositionForced\":%s,"
        "\"hudlessWidth\":%u,\"hudlessHeight\":%u,"
        "\"uiWidth\":%u,\"uiHeight\":%u,\"uiTagFormat\":%u,"
        "\"uiTagAgeMs\":%llu,\"setTagCalls\":%llu,"
        "\"setTagForFrameCalls\":%llu,"
        "\"stateSampleAgeMs\":%llu,\"setOptionsCalls\":%llu,"
        "\"getStateCalls\":%llu,\"liveReapplyCount\":%llu,"
        "\"notInitializedRetryCount\":%llu,"
        "\"activeWrapperPath\":\"%s\","
        "\"activeWrapperGeneration\":%llu,"
        "\"activeControlPath\":\"%s\","
        "\"activeStatePath\":\"%s\","
        "\"activeControlDetour\":\"%s\","
        "\"activeStateDetour\":\"%s\","
        "\"activeProviderPath\":\"%s\","
        "\"activeProviderVersionMajor\":%u,"
        "\"activeProviderVersionMinor\":%u,"
        "\"activeProviderVersionBuild\":%u,"
        "\"activeProviderVersionPrivate\":%u,"
        "\"activeProviderGeneration\":%llu,"
        "\"providerSelectionSource\":\"%s\","
        "\"providerCreateDetour\":\"%s\","
        "\"providerEvaluateDetour\":\"%s\","
        "\"midpointReadyAtFirstCreate\":%s,"
        "\"activeLastCallRevision\":%llu,"
        "\"activeLastAcceptedRevision\":%llu,"
        "\"universalRouteFailure\":%u,"
        "\"universalRouteFailureReason\":\"%s\","
        "\"releaseEntryCurrent\":%s,"
        "\"frameGenerationOffAccepted\":%s,"
        "\"releaseObserved\":%s}\n",
        static_cast<unsigned long>(pid),
        static_cast<unsigned long long>(diagnostic_paths::ProcessBirth()),
        static_cast<unsigned long long>(UnixTimeSeconds()), route,
        bridgeReady ? "true" : "false",
        (gLiveHookInstalled.load(std::memory_order_relaxed)
            || gOriginalGetFeatureFunction.load(std::memory_order_acquire)) ? "true" : "false",
        gNvidiaCompatibilityResolved.load(
            std::memory_order_acquire) ? "true" : "false",
        gNvidiaProfileStatus.load(std::memory_order_relaxed),
        gNvidiaProfileName.c_str(),
        nvidiaCompatibilityTier,
        nvidia_mfg_policy::ManifestEntryCount(),
        compatibilityCapacity.nvidiaCeilingMultiplier,
        compatibilityCapacity.wrapperNativeMaximumMultiplier,
        compatibilityCapacity.fallback ? "true" : "false",
        static_cast<uint32_t>(compatibilityCapacity.reason),
        gFullStreamlineOtaRequested.load(
            std::memory_order_relaxed) ? "true" : "false",
        gFullStreamlineOtaEligible.load(
            std::memory_order_relaxed) ? "true" : "false",
        gDownloadedStreamlinePluginsForced.load(
            std::memory_order_relaxed) ? "true" : "false",
        gStreamlineHostVersionMajor.load(std::memory_order_relaxed),
        gStreamlineHostVersionMinor.load(std::memory_order_relaxed),
        gStreamlineHostVersionBuild.load(std::memory_order_relaxed),
        gStreamlineHostVersionPrivate.load(std::memory_order_relaxed),
        gMainResolverDiscoveryInstalled.load(
            std::memory_order_relaxed) ? "true" : "false",
        slInitDetour.installed ? "true" : "false",
        slInitDetour.current ? "true" : "false",
        slInitDetour.cachedPointersCovered ? "true" : "false",
        static_cast<uint32_t>(slInitDetour.failure),
        slInitDetour.targetRva,
        slInitIatFallback ? "true" : "false",
        slInitResolverFallback ? "true" : "false",
        slInitControlPathReady ? "true" : "false",
        static_cast<unsigned long long>(slInitCalls),
        static_cast<unsigned long long>(
            gSlInitFlagsBefore.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(slInitFlagsAfter),
        gOtaPreferencesForced.load(
            std::memory_order_relaxed) ? "true" : "false",
        slInitCalls > 0 && (slInitFlagsAfter & allowOtaMask) != 0
            ? "true" : "false",
        slInitCalls > 0
                && (slInitFlagsAfter & downloadedPluginsMask) != 0
            ? "true" : "false",
        gOtaProviderPreflightSupported.load(
            std::memory_order_relaxed) ? "true" : "false",
        gOtaForceSuppressed.load(
            std::memory_order_relaxed) ? "true" : "false",
        gSelectiveOtaDlssgWrapperRequested.load(
            std::memory_order_relaxed) ? "true" : "false",
        gSelectiveOtaDlssgWrapperCandidateReady.load(
            std::memory_order_relaxed) ? "true" : "false",
        gSelectiveOtaDlssgWrapperFailure.load(
            std::memory_order_relaxed),
        static_cast<unsigned long long>(
            gSelectiveOtaDlssgWrapperRedirectAttempts.load(
                std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            gSelectiveOtaDlssgWrapperRedirectSuccesses.load(
                std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            gSelectiveOtaDlssgWrapperFallbacks.load(
                std::memory_order_relaxed)),
        gSelectiveOtaDlssgWrapperVersionMajor.load(
            std::memory_order_relaxed),
        gSelectiveOtaDlssgWrapperVersionMinor.load(
            std::memory_order_relaxed),
        gSelectiveOtaDlssgWrapperVersionBuild.load(
            std::memory_order_relaxed),
        gSelectiveOtaDlssgWrapperVersionPrivate.load(
            std::memory_order_relaxed),
        gSlCommonResolverDiscoveryInstalled.load(
            std::memory_order_relaxed) ? "true" : "false",
        gStreamlineLoaderDiscoveryInstalled.load(
            std::memory_order_relaxed) ? "true" : "false",
        static_cast<unsigned long long>(
            gStreamlineLoaderDiscoveryCalls.load(std::memory_order_relaxed)),
        setOptionsDetour.installed ? "true" : "false",
        setOptionsDetour.current ? "true" : "false",
        setOptionsDetour.cachedPointersCovered ? "true" : "false",
        static_cast<uint32_t>(setOptionsDetour.failure),
        setOptionsDetour.targetRva,
        setOptionsResolverFallbackActive ? "true" : "false",
        static_cast<unsigned long long>(setOptionsResolverFallbackCalls),
        setOptionsControlPathReady ? "true" : "false",
        getStateDetour.installed ? "true" : "false",
        getStateDetour.current ? "true" : "false",
        getStateDetour.cachedPointersCovered ? "true" : "false",
        static_cast<uint32_t>(getStateDetour.failure),
        getStateDetour.targetRva,
        createDetour.installed ? "true" : "false",
        createDetour.current ? "true" : "false",
        createDetour.cachedPointersCovered ? "true" : "false",
        static_cast<uint32_t>(createDetour.failure),
        createDetour.targetRva,
        evaluateDetour.installed ? "true" : "false",
        evaluateDetour.current ? "true" : "false",
        evaluateDetour.cachedPointersCovered ? "true" : "false",
        static_cast<uint32_t>(evaluateDetour.failure),
        evaluateDetour.targetRva,
        static_cast<unsigned long long>(
            gNgxCreateCalls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            gNgxFrameGenerationCreateCalls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            gNgxEvaluateCalls.load(std::memory_order_relaxed)),
        gLastNgxCreateResult.load(std::memory_order_relaxed),
        gFrameGenerationCreateObserved.load(
            std::memory_order_relaxed) ? "true" : "false",
        gBackportReadyAtCreate.load(
            std::memory_order_relaxed) ? "true" : "false",
        gPipelineMayPredateDetour.load(
            std::memory_order_relaxed) ? "true" : "false",
        restartRequired ? "true" : "false",
        restartRequired ? "true" : "false",
        gUiTagHookInstalled.load(std::memory_order_relaxed) ? "true" : "false",
        gActiveWrapperObserved.load(std::memory_order_relaxed) ? "true" : "false",
        gActiveWrapperPatched.load(std::memory_order_relaxed) ? "true" : "false",
        gActiveWrapperUsesNvidiaOta.load(
            std::memory_order_relaxed) ? "true" : "false",
        gActiveWrapperVersionMajor.load(std::memory_order_relaxed),
        gActiveWrapperVersionMinor.load(std::memory_order_relaxed),
        gActiveWrapperVersionBuild.load(std::memory_order_relaxed),
        gActiveWrapperVersionPrivate.load(std::memory_order_relaxed),
        gWrapperCompiledMaximumGeneratedFrames.load(
            std::memory_order_relaxed),
        safeMaximumMultiplier,
        gLoadedWrapperCandidates.load(std::memory_order_relaxed),
        gPatchedWrapperCandidates.load(std::memory_order_relaxed),
        gLoadedNgxCandidates.load(std::memory_order_relaxed),
        gPatchedNgxCandidates.load(std::memory_order_relaxed),
        control.followGame ? "true" : "false",
        control.followGame ? "follow"
            : control.dynamic ? "dynamic" : "fixed",
        control.multiplier,
        control.dynamicTargetFrameRate,
        control.dynamicExperimental56 ? "true" : "false",
        control.generatedOnlyDebug ? "true" : "false",
        safeMaximumMultiplier,
        control.dlssgPreset, preset.installed ? "true" : "false", preset.failure,
        static_cast<unsigned long long>(preset.providerReads),
        preset.cachedReaderInstalled ? "true" : "false", preset.cachedReaderFailure,
        preset.cachedReaderRva, static_cast<unsigned long long>(preset.cachedReaderReads),
        requestedMultiplierLimited ? "true" : "false",
        intervalTrace.enabled ? "true" : "false",
        intervalTrace.logReady ? "true" : "false",
        static_cast<unsigned long long>(intervalTrace.validSamples),
        static_cast<unsigned long long>(intervalTrace.invalidSamples),
        static_cast<unsigned long long>(intervalTrace.droppedSamples),
        intervalTrace.seenCountMask, intervalTrace.seenIndexMask,
        intervalTrace.lastCount, intervalTrace.lastIndex,
        intervalTrace.lastPositionNumerator,
        intervalTrace.lastPositionDenominator,
        temporal_interval_trace::FileName(),
        static_cast<unsigned long long>(desiredRevision),
        static_cast<unsigned long long>(appliedRevision),
        applied ? "true" : "false", pending ? "true" : "false",
        gameFrameGenerationOn ? "true" : "false",
        appliedFrameGenerationOn ? "true" : "false",
        gAppliedDynamicMode.load(std::memory_order_relaxed) ? "dynamic" : "fixed",
        gAppliedMultiplier.load(std::memory_order_relaxed),
        static_cast<double>(gAppliedDynamicTargetFrameRate.load(std::memory_order_relaxed)),
        gAppliedDynamicTargetValid.load(std::memory_order_relaxed) ? "true" : "false",
        gAppliedDynamicExperimental56.load(std::memory_order_relaxed) ? "true" : "false",
        gAppliedGeneratedOnlyDebug.load(std::memory_order_relaxed) ? "true" : "false",
        setOptionsSeen ? "true" : "false",
        setOptionsAccepted ? "true" : "false", setOptionsResult,
        getStateSeen ? "true" : "false", getStateResult,
        gActualFramesPresented.load(std::memory_order_relaxed),
        gNumFramesToGenerateMax.load(std::memory_order_relaxed),
        gRealFpsMilli.load(std::memory_order_relaxed),
        gDlssFpsMilli.load(std::memory_order_relaxed),
        gFpsOutputSource.load(std::memory_order_relaxed),
        gFpsSampleWindowMs.load(std::memory_order_relaxed),
        static_cast<unsigned long long>(fpsAgeMs),
        gDlssgStatus.load(std::memory_order_relaxed),
        DynamicMfgCapabilityKnown() ? "true" : "false",
        (UseAmpere() ? DynamicMfgSupported() : gDynamicMfgSupported.load(std::memory_order_relaxed)) ? "true" : "false",
        gFgVsyncSupportKnown.load(std::memory_order_relaxed) ? "true" : "false",
        gFgVsyncSupported.load(std::memory_order_relaxed) ? "true" : "false",
        gGameOptionsStructVersion.load(std::memory_order_relaxed),
        gGameUiRecompositionEnabled.load(std::memory_order_relaxed) ? "true" : "false",
        gGameHudlessBufferFormat.load(std::memory_order_relaxed),
        gGameUiBufferFormat.load(std::memory_order_relaxed),
        uiInputs.hudless ? "true" : "false",
        uiInputs.uiAlpha ? "true" : "false",
        uiInputs.uiColorAlpha ? "true" : "false",
        uiInputs.dimensionsKnown ? "true" : "false",
        uiInputs.dimensionsMatch ? "true" : "false",
        uiInputs.ready ? "true" : "false",
        gAppliedUiRecompositionEnabled.load(std::memory_order_relaxed) ? "true" : "false",
        gAppliedUiRecompositionForced.load(std::memory_order_relaxed) ? "true" : "false",
        uiInputs.hudlessWidth, uiInputs.hudlessHeight,
        uiInputs.uiWidth, uiInputs.uiHeight, uiInputs.uiFormat,
        static_cast<unsigned long long>(uiInputs.oldestAgeMs),
        static_cast<unsigned long long>(gSetTagCalls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            gSetTagForFrameCalls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(stateAgeMs),
        static_cast<unsigned long long>(gSetOptionsCalls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(gGetStateCalls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(gLiveReapplyCount.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            gNotInitializedRetryCount.load(std::memory_order_relaxed)),
        activeWrapperPath.c_str(),
        static_cast<unsigned long long>(
            activeControl ? activeControl->generation : 0),
        ControlPathName(activeSetterPath),
        ControlPathName(activeStatePath),
        ControlDetourMethod(activeControl, true),
        ControlDetourMethod(activeControl, false),
        activeProviderPath.c_str(),
        activeProviderVersion.major, activeProviderVersion.minor,
        activeProviderVersion.build, activeProviderVersion.privatePart,
        static_cast<unsigned long long>(
            gActiveNgxProviderGeneration.load(std::memory_order_acquire)),
        selectionSourceName,
        activeProvider ? entry_detour::MethodName(createDetour.method)
                       : "none",
        activeProvider ? entry_detour::MethodName(evaluateDetour.method)
                       : "none",
        gFirstCreateMidpointReady.load(
            std::memory_order_acquire) ? "true" : "false",
        static_cast<unsigned long long>(activeControl
            ? activeControl->lastCallRevision.load(
                std::memory_order_acquire) : 0),
        static_cast<unsigned long long>(activeControl
            ? activeControl->lastAcceptedRevision.load(
                std::memory_order_acquire) : 0),
        static_cast<uint32_t>(routeFailure),
        universal_route_policy::FailureName(routeFailure),
        releaseDetour.current ? "true" : "false",
        activeControl && activeControl->frameGenerationOffAccepted.load(
            std::memory_order_acquire) ? "true" : "false",
        activeControl && activeControl->releaseObserved.load(
            std::memory_order_acquire) ? "true" : "false");
    if (length <= 0)
        return false;

    // ReShade reads concurrently. Publish a complete replacement so a reader
    // sees the previous snapshot until the new one is ready, never a truncated
    // or partially written status file.
    std::string serialized(json, static_cast<size_t>(length));
    const size_t gpuClosing = serialized.rfind('}');
    if (gpuClosing == std::string::npos) return false;
    char gpu[192]{};
    sprintf_s(gpu, ",\"product\":\"RTXMFG\",\"gpuFamily\":%u,\"gpuAdapterLuid\":%llu,\"gpuSelectionFailure\":%u",
        static_cast<uint32_t>(gpu_dispatch::Selected()),
        static_cast<unsigned long long>(gpu_dispatch::AdapterLuid()), gpu_dispatch::FailureCode());
    serialized.insert(gpuClosing, gpu);
    char presetStatus[512]{};
    sprintf_s(presetStatus, ",\"dlssgPresetLatched\":%u,"
        "\"dlssgPresetSelectionFrozen\":%s,\"dlssgPresetRestartRequired\":%s,"
        "\"dlssgPresetObserved\":%u,\"dlssgPresetObservedValid\":%s,"
        "\"dlssgPresetOverrideReadCount\":%llu",
        preset.latched, preset.selectionFrozen ? "true" : "false",
        preset.restartRequired ? "true" : "false", preset.observedPreset,
        preset.observedPresetValid ? "true" : "false",
        static_cast<unsigned long long>(preset.overrideReads));
    serialized.insert(serialized.rfind('}'), presetStatus);
    const auto vsync = vsync_control::ReadSnapshot();
    const auto reflex = reflex_control::ReadSnapshot();
    char pacing[2048]{};
    sprintf_s(pacing, ",\"vsyncMode\":%u,\"reflexFrameLimitFps\":%u,"
        "\"dynamicVsyncAvailable\":%s,\"vsyncControlAvailable\":%s,"
        "\"vsyncOverrideApplied\":%s,\"vsyncPresentationObserved\":%s,"
        "\"vsyncOriginalInterval\":%u,\"vsyncSubmittedInterval\":%u,\"vsyncFailure\":%u,"
        "\"reflexControlAvailable\":%s,\"reflexAppliedKnown\":%s,"
        "\"reflexAppliedFrameLimitUs\":%u,\"reflexLimitPending\":%s,"
        "\"reflexRestorePending\":%s,\"reflexStatus\":%u,\"reflexLastResult\":%u,"
        "\"reflexHookMask\":%u,\"reflexModuleVersionMajor\":%u,"
        "\"reflexModuleVersionMinor\":%u,\"reflexModuleVersionPatch\":%u,"
        "\"reflexModuleGeneration\":%llu",
        control.vsyncMode, control.reflexFrameLimitFps,
        VsyncRuntimeCurrent() && DynamicMfgSupported() ? "true" : "false",
        vsync.available ? "true" : "false", vsync.overrideApplied ? "true" : "false",
        vsync.matched ? "true" : "false", vsync.originalInterval, vsync.submittedInterval,
        static_cast<uint32_t>(vsync.failure),
        reflex.eligible && reflex.cachedPointersCovered && reflex.availabilityKnown
            && reflex.lowLatencyAvailable ? "true" : "false",
        reflex.appliedKnown ? "true" : "false", reflex.appliedFrameLimitUs,
        reflex.pending ? "true" : "false", reflex.restorePending ? "true" : "false",
        static_cast<uint32_t>(reflex.status), reflex.lastSetResult,
        reflex.currentHookMask, reflex.moduleVersionMajor, reflex.moduleVersionMinor,
        reflex.moduleVersionPatch, static_cast<unsigned long long>(reflex.moduleGeneration));
    serialized.insert(serialized.rfind('}'), pacing);
    if (UseAmpere())
    {
        const size_t closing = serialized.rfind('}');
        if (closing == std::string::npos) return false;
        const auto diagnostic = ampere_backend::Diagnostics();
        char ampere[2048]{};
        sprintf_s(ampere, ",\"ampereProgramReadyMfg\":%s,"
            "\"ampereFailure\":%u,\"presetQueries\":%llu,"
            "\"ampereEvaluations\":%llu,\"ampereRejections\":%llu,"
            "\"ampereNativeMaximum\":%u,\"ampereLastCreateCount\":%d,"
            "\"ampereNativeMaximumReadResult\":%u,\"ampereStartupMaximum\":%u,\"amperePresentationBuffers\":%u,"
            "\"ampereCreatedFeatures\":%llu,\"ampereSubmittedBatches\":%llu,"
            "\"ampereKernelImage\":%u,\"ampereNativeCacheStatus\":%u,\"ampereCertifiedMaximum\":%u,"
            "\"ampereFirstFailure\":%u,"
            "\"ampereFirstNgxResult\":%u,"
            "\"amperePrimaryFailure\":%u,"
            "\"amperePrimaryNgxResult\":%u,"
            "\"ampereLastFailure\":%u,"
            "\"ampereLastNgxResult\":%u,"
            "\"ampereCreateAttempts\":%llu,"
            "\"ampereCreateBlockedBeforeProvider\":%llu,"
            "\"ampereEvaluateAttempts\":%llu,"
            "\"amperePreparationStage\":%u,\"ampereStartupFailureMask\":%u,"
            "\"ampereCandidateVersionMajor\":%u,"
            "\"ampereCandidateVersionMinor\":%u,"
            "\"ampereCandidateVersionBuild\":%u,\"ampereLegacySinglePreset\":%s",
            ampere_backend::Ready() ? "true" : "false", ampere_backend::FailureCode(),
            static_cast<unsigned long long>(ampere_backend::PresetQueries()),
            static_cast<unsigned long long>(ampere_backend::Evaluations()),
            static_cast<unsigned long long>(ampere_backend::Rejections()),
            ampere_backend::NativeMaximumGeneratedFrames(), ampere_backend::LastCreateCount(),
            ampere_backend::NativeMaximumReadResult(), ampere_backend::StartupMaximumGeneratedFrames(), ampere_backend::PresentationBuffers(),
            static_cast<unsigned long long>(ampere_backend::CreatedFeatures()),
            static_cast<unsigned long long>(ampere_backend::SubmittedBatches()),
            ampere_gpu::KernelImage(), ampere_gpu::NativeCacheStatus(),
            ampere_backend::CertifiedMaximumGeneratedFrames(),
            diagnostic.first.code,
            diagnostic.first.ngxResult,
            diagnostic.primary.code,
            diagnostic.primary.ngxResult,
            diagnostic.last.code,
            diagnostic.last.ngxResult,
            static_cast<unsigned long long>(diagnostic.createAttempts),
            static_cast<unsigned long long>(diagnostic.createBlockedBeforeProvider),
            static_cast<unsigned long long>(diagnostic.evaluateAttempts),
            diagnostic.preparationStage, diagnostic.startupFailureMask,
            static_cast<uint32_t>(diagnostic.candidateVersion >> 32),
            static_cast<uint32_t>((diagnostic.candidateVersion >> 16) & 0xffffu),
            static_cast<uint32_t>(diagnostic.candidateVersion & 0xffffu),
            ampere_backend::LegacySinglePreset() ? "true" : "false");
        serialized.insert(closing, ampere);
    }
    const auto publication=status_transport::Publish(gStatusPath,serialized);
    static DWORD lastPrimaryError=ERROR_SUCCESS,lastFallbackError=ERROR_SUCCESS;
    static bool lastFallback=false;
    if(publication.primaryError!=lastPrimaryError||publication.fallbackError!=lastFallbackError
        ||publication.fallback!=lastFallback){
        Log(L"MFG_STATUS_TRANSPORT published=%d fallback=%d primaryError=%lu fallbackError=%lu path=%s",
            publication.published,publication.fallback,publication.primaryError,publication.fallbackError,
            (publication.fallback?status_transport::MemoryName(gStatusPath):gStatusPath).c_str());
        lastPrimaryError=publication.primaryError;lastFallbackError=publication.fallbackError;
        lastFallback=publication.fallback;
    }
    return publication.published;
}

using ChainFindStatus = universal_route_policy::StructureStatus;

struct BaseStructureFields
{
    sl::BaseStructure* next = nullptr;
    sl::StructType type{};
    size_t version = 0;
};

bool ReadBaseStructureFields(const sl::BaseStructure* source,
    BaseStructureFields& fields) noexcept
{
    if (!source)
        return false;
    __try
    {
        fields.next = source->next;
        fields.type = source->structType;
        fields.version = source->structVersion;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

template <typename T>
ChainFindStatus FindStructBounded(const sl::BaseStructure* chain,
    const T*& result, size_t& version) noexcept
{
    constexpr size_t kMaximumChainNodes = 32;
    const sl::BaseStructure* visited[kMaximumChainNodes]{};
    result = nullptr;
    version = 0;
    for (size_t index = 0; chain && index < kMaximumChainNodes; ++index)
    {
        for (size_t prior = 0; prior < index; ++prior)
        {
            if (visited[prior] == chain)
                return ChainFindStatus::eMalformed;
        }
        visited[index] = chain;
        BaseStructureFields fields{};
        if (!ReadBaseStructureFields(chain, fields))
            return ChainFindStatus::eMalformed;
        if (fields.type == T::s_structType)
        {
            if (UseAmpere())
            {
                // Validate the tail too: the complete chain is forwarded after
                // our fixed options prefix, so a later cycle must not escape.
                if (!result)
                {
                    result = static_cast<const T*>(chain);
                    version = fields.version;
                }
            }
            else
            {
                result = static_cast<const T*>(chain);
                version = fields.version;
                return ChainFindStatus::eFound;
            }
        }
        chain = fields.next;
    }
    return chain ? ChainFindStatus::eMalformed
                 : result ? ChainFindStatus::eFound : ChainFindStatus::eNotFound;
}

bool IsSupportedOptionsVersion(size_t version) noexcept
{
    static_assert(sl::kStructVersion1 == 1);
    static_assert(sl::kStructVersion5 == 5);
    return universal_route_policy::IsSupportedOptionsVersion(version);
}

bool IsSupportedStateVersion(size_t version) noexcept
{
    static_assert(sl::kStructVersion1 == 1);
    static_assert(sl::kStructVersion4 == 4);
    return universal_route_policy::IsSupportedStateVersion(version);
}

ControlRouteRecord* ControlRouteAt(uint32_t slot) noexcept
{
    if (slot >= gControlRoutes.size()
        || !gControlRoutes[slot].claimed.load(std::memory_order_acquire))
        return nullptr;
    return &gControlRoutes[slot];
}

ControlRouteRecord* ActiveControlRoute() noexcept
{
    return ControlRouteAt(
        gActiveControlRouteSlot.load(std::memory_order_acquire));
}

const char* ControlPathName(ControlEntryPath path) noexcept
{
    switch (path)
    {
    case ControlEntryPath::ePublic: return "public";
    case ControlEntryPath::eInternal: return "internal";
    case ControlEntryPath::eResolver: return "resolver";
    default: return "none";
    }
}

bool IsAcceptedControlResult(sl::Result result) noexcept
{
    return universal_route_policy::IsAcceptedResult(
        static_cast<int32_t>(result),
        static_cast<int32_t>(sl::Result::eOk),
        static_cast<int32_t>(sl::Result::eWarnOutOfVRAM));
}

sl::Result HostControlResult(sl::Result result) noexcept
{
    // Match the adjusted-options path at every validated DLSS-G boundary,
    // including the initial native enable before BridgeReady. SL_FAILED
    // treats all nonzero results as errors, including this accepted warning.
    // Record the raw result before returning; real errors remain unchanged.
    return result == sl::Result::eWarnOutOfVRAM ? sl::Result::eOk : result;
}

void ClearAppliedDynamicTelemetry() noexcept
{
    gAppliedDynamicTargetValid.store(false, std::memory_order_release);
    gAppliedDynamicMode.store(false, std::memory_order_relaxed);
    gAppliedDynamicTargetFrameRate.store(0.0f, std::memory_order_relaxed);
    gAppliedDynamicExperimental56.store(false, std::memory_order_relaxed);
}

bool SameRetainedFeature(const RetainedFeatureIdentity& left,
    const RetainedFeatureIdentity& right) noexcept
{
    return left.handle == right.handle && left.runtime == right.runtime
        && left.runtimeGeneration == right.runtimeGeneration
        && left.provider == right.provider
        && left.providerGeneration == right.providerGeneration
        && left.wrapper == right.wrapper
        && left.wrapperGeneration == right.wrapperGeneration
        && left.publication == right.publication
        && left.adapterLuid == right.adapterLuid
        && left.generatedFrameCapacity == right.generatedFrameCapacity
        && left.lifetime == right.lifetime
        && left.createAttemptEpoch == right.createAttemptEpoch;
}

bool SameObservedFeature(const RetainedFeatureIdentity& feature,
    const ampere_backend::FeatureLifetimeEvent& event) noexcept
{
    return feature.handle == event.handle
        && feature.lifetime == event.lifetime
        && feature.runtime == event.runtime
        && feature.runtimeGeneration == event.runtimeGeneration
        && feature.provider == event.provider
        && feature.providerGeneration == event.providerGeneration
        && feature.wrapper == event.wrapper
        && feature.wrapperGeneration == event.wrapperGeneration
        && feature.publication == event.publication
        && feature.adapterLuid == event.adapterLuid
        && feature.generatedFrameCapacity == event.generatedFrameCapacity
        && feature.createAttemptEpoch == event.createAttemptToken;
}

bool AmpereFeatureEntriesCurrent(uintptr_t runtimeOwner, uint64_t runtimeGeneration,
    uintptr_t providerOwner, uint64_t providerGeneration) noexcept
{
    if (!runtimeOwner || !runtimeGeneration || !providerOwner || !providerGeneration)
        return false;
    const auto runtime = entry_detour::ReadSnapshot(
        entry_detour::Kind::eNgxRuntimeD3D12CreateFeature,
        reinterpret_cast<HMODULE>(runtimeOwner));
    const auto provider = entry_detour::ReadSnapshot(
        entry_detour::Kind::eNgxD3D12CreateFeature,
        reinterpret_cast<HMODULE>(providerOwner));
    if (!runtime.current || runtime.currentEntries != 1 || runtime.generation != runtimeGeneration
        || !provider.current || provider.currentEntries != 1 || provider.generation != providerGeneration)
        return false;
    const auto selected = entry_detour::ReadSnapshot(UnpackEntryHandle(
        gActiveNgxCreateHandle.load(std::memory_order_acquire)));
    if (!selected.current) return false;
    // A runtime may select its provider before forwarding, or the nested
    // concrete provider may select the route. Both entries remain required;
    // the selected kind determines which exact immutable handle must match.
    if (selected.kind == entry_detour::Kind::eNgxRuntimeD3D12CreateFeature)
        return selected.handle == runtime.handle;
    if (selected.kind == entry_detour::Kind::eNgxD3D12CreateFeature)
        return selected.handle == provider.handle;
    return false;
}

bool RetainedFeatureMatchesRuntime(const RetainedFeatureIdentity& feature,
    const ControlRouteRecord& route) noexcept
{
    if (!feature || feature.wrapper != reinterpret_cast<uintptr_t>(route.wrapper)
        || feature.wrapperGeneration != route.generation
        || feature.provider
            != gActiveNgxProviderBase.load(std::memory_order_acquire)
        || feature.providerGeneration
            != gActiveNgxProviderGeneration.load(std::memory_order_acquire)
        || feature.createAttemptEpoch
            != gFrameGenerationCreateAttemptEpoch.load(std::memory_order_acquire)
        || !gFrameGenerationCreateObserved.load(std::memory_order_acquire))
        return false;
    if (!AmpereFeatureEntriesCurrent(feature.runtime, feature.runtimeGeneration,
            feature.provider, feature.providerGeneration))
        return false;
    if (!UseAmpere())
        return false;
    return feature.publication == ampere_gpu::Publication()
        && feature.adapterLuid == ampere_gpu::AdapterLuid();
}

uint32_t ControlRouteSlot(const ControlRouteRecord& route) noexcept
{
    const ptrdiff_t slot = &route - gControlRoutes.data();
    return slot >= 0 && static_cast<size_t>(slot) < gControlRoutes.size()
        ? static_cast<uint32_t>(slot) : UINT32_MAX;
}

void RecordGameFrameGenerationIntent(const sl::ViewportHandle& viewport,
    bool enabled) noexcept
{
    gGameFrameGenerationViewport.store(
        static_cast<uint32_t>(viewport), std::memory_order_release);
    gGameFrameGenerationOn.store(enabled, std::memory_order_release);
}

bool ControlRequestsEnabled(const ControlSnapshot& snapshot,
    const sl::DLSSGOptions& source) noexcept
{
    if (!gGameFrameGenerationOn.load(std::memory_order_acquire)
        || source.mode == sl::DLSSGMode::eOff)
        return false;
    if (!UseAmpere())
        return true;
    return snapshot.control.followGame
        || snapshot.control.multiplier > 1;
}

bool CanReenableRetainedFeatureLocked(ControlRouteRecord& route,
    const sl::ViewportHandle& viewport, const ControlSnapshot& snapshot,
    const sl::DLSSGOptions& source,
    RetainedFeatureIdentity* expectedLiveFeature = nullptr) noexcept
{
    if (expectedLiveFeature)
        *expectedLiveFeature = {};
    if (!ControlRequestsEnabled(snapshot, source))
        return true;
    // Ada retains its existing Streamline control contract.  The opaque
    // handle/capacity observer below belongs only to the experimental Ampere
    // backend; applying it to Ada would reject every live On revision.
    if (!UseAmpere())
        return true;
    const uint32_t routeSlot = ControlRouteSlot(route);
    if (routeSlot == UINT32_MAX
        || !RetainedFeatureMatchesRuntime(
            gRetainedFrameGenerationFeature, route))
        return false;
    if (!gAppliedFrameGenerationOn.load(std::memory_order_acquire))
    {
        const bool acceptedOff = gAcceptedOffEvidence.routeSlot == routeSlot
            && gAcceptedOffEvidence.viewport
                == static_cast<uint32_t>(viewport)
            && SameRetainedFeature(gAcceptedOffEvidence.feature,
                gRetainedFrameGenerationFeature);
        const bool freshRecreation = SameRetainedFeature(
            gFreshRecreatedFrameGenerationFeature,
            gRetainedFrameGenerationFeature);
        if (!acceptedOff && !freshRecreation)
            return false;
    }
    const uint32_t requiredMultiplier = snapshot.control.followGame
        ? std::max(source.numFramesToGenerate + 1u, 2u)
        : EffectiveMultiplier(snapshot.control);
    if (gRetainedFrameGenerationFeature.generatedFrameCapacity + 1u
        < requiredMultiplier)
        return false;
    if (expectedLiveFeature)
        *expectedLiveFeature = gRetainedFrameGenerationFeature;
    return true;
}

bool CanReenableRetainedFeature(ControlRouteRecord& route,
    const sl::ViewportHandle& viewport, const ControlSnapshot& snapshot,
    const sl::DLSSGOptions& source,
    RetainedFeatureIdentity* expectedLiveFeature = nullptr) noexcept
{
    std::unique_lock<std::mutex> lifetimeLock(gFrameGenerationLifetimeMutex, std::defer_lock);
    if (UseAmpere()) lifetimeLock.lock();
    return CanReenableRetainedFeatureLocked(route, viewport, snapshot, source, expectedLiveFeature);
}

bool RetainedFeatureStillExactForLiveOnLocked(ControlRouteRecord& route,
    const sl::ViewportHandle& viewport,
    const RetainedFeatureIdentity& expected) noexcept
{
    const uint32_t routeSlot = ControlRouteSlot(route);
    if (!expected || routeSlot == UINT32_MAX
        || !SameRetainedFeature(expected, gRetainedFrameGenerationFeature)
        || !RetainedFeatureMatchesRuntime(
            gRetainedFrameGenerationFeature, route))
        return false;
    if (gAppliedFrameGenerationOn.load(std::memory_order_acquire))
        return true;
    const bool acceptedOff = gAcceptedOffEvidence.routeSlot == routeSlot
        && gAcceptedOffEvidence.viewport == static_cast<uint32_t>(viewport)
        && SameRetainedFeature(
            expected, gAcceptedOffEvidence.feature);
    return acceptedOff || SameRetainedFeature(
        expected, gFreshRecreatedFrameGenerationFeature);
}

void RecordAcceptedSetOptionsLifecycleLocked(ControlRouteRecord& route,
    const sl::ViewportHandle& viewport, bool enabled) noexcept
{
    route.lastAcceptedRevision.store(
        gDesiredRevision.load(std::memory_order_acquire),
        std::memory_order_release);
    if (gAppliedFrameGenerationOn.exchange(
            enabled, std::memory_order_acq_rel) != enabled)
        gPresentationLifecycleEpoch.fetch_add(1, std::memory_order_acq_rel);
    route.frameGenerationOffAccepted.store(!enabled,
        std::memory_order_release);
    if (enabled)
    {
        route.releaseObserved.store(false, std::memory_order_release);
        gAcceptedOffEvidence = {};
        gFreshRecreatedFrameGenerationFeature = {};
    }
    else
    {
        InvalidateUiInputEvidence(static_cast<uint32_t>(viewport));
        gFreshRecreatedFrameGenerationFeature = {};
        if (!UseAmpere())
        {
            // Ada has no opaque NGX-feature observer. Preserve only the exact
            // accepted route/viewport Off boundary: live Off -> On remains
            // supported until a matching slFreeResources succeeds, at which
            // point lifetime ownership is unprovable and restart is required.
            const uint32_t routeSlot = ControlRouteSlot(route);
            gAcceptedOffEvidence = routeSlot == UINT32_MAX
                ? AcceptedOffEvidence{}
                : AcceptedOffEvidence{{}, routeSlot,
                    static_cast<uint32_t>(viewport),
                    gDesiredRevision.load(std::memory_order_acquire)};
        }
        else
        {
            const uint32_t routeSlot = ControlRouteSlot(route);
            if (routeSlot != UINT32_MAX
                && RetainedFeatureMatchesRuntime(
                    gRetainedFrameGenerationFeature, route))
            {
                gAcceptedOffEvidence = {gRetainedFrameGenerationFeature,
                    routeSlot, static_cast<uint32_t>(viewport),
                    gDesiredRevision.load(std::memory_order_acquire)};
            }
            else
            {
                // Off was accepted, but no exact retained feature is available
                // for a later live Ampere On. Preserve the stopped state and
                // require a known recreation boundary instead of guessing from
                // route-wide flags.
                gAcceptedOffEvidence = {};
                gRestartRequired.store(true, std::memory_order_release);
            }
        }
        ClearAppliedDynamicTelemetry();
        gFgVsyncSupportKnown.store(false, std::memory_order_release);
        gFgVsyncSupported.store(false, std::memory_order_release);
    }
}

void RecordSetOptionsLifecycle(ControlRouteRecord& route,
    const sl::ViewportHandle& viewport, bool enabled, sl::Result result) noexcept
{
    if (!IsAcceptedControlResult(result))
        return;
    std::lock_guard lifetimeLock(gFrameGenerationLifetimeMutex);
    RecordAcceptedSetOptionsLifecycleLocked(route, viewport, enabled);
}

void ResetReleasedPipelineStateLocked(ControlRouteRecord& route) noexcept
{
    InvalidateUiInputEvidence();
    gAppliedUiRecompositionEnabled.store(false, std::memory_order_release);
    gAppliedUiRecompositionForced.store(false, std::memory_order_release);
    gRetainedFrameGenerationFeature = {};
    gFreshRecreatedFrameGenerationFeature = {};
    gAcceptedOffEvidence = {};
    gPresentationLifecycleEpoch.fetch_add(1, std::memory_order_acq_rel);
    gFgVsyncSupportKnown.store(false, std::memory_order_release);
    gFgVsyncSupported.store(false, std::memory_order_release);
    gDynamicMfgCapabilityKnown.store(false, std::memory_order_release);
    gDynamicMfgSupported.store(false, std::memory_order_release);
    ClearAppliedDynamicTelemetry();
    route.frameGenerationOffAccepted.store(false,
        std::memory_order_release);
    route.releaseObserved.store(true, std::memory_order_release);
    gAppliedFrameGenerationOn.store(false, std::memory_order_release);
    // Applied/attempted revisions are scoped to the released native feature.
    // A later exact Create must receive the current request even when the
    // config revision itself did not change across recreation.
    gAppliedRevision.store(0, std::memory_order_release);
    gAttemptedRevision.store(0, std::memory_order_release);
    gFrameGenerationCreateObserved.store(false, std::memory_order_release);
    gFirstCreateMidpointReady.store(false, std::memory_order_release);
    gBackportReadyAtCreate.store(false, std::memory_order_release);
    gPipelineMayPredateDetour.store(false, std::memory_order_release);
    gRestartRequired.store(false, std::memory_order_release);
    gProviderChangedAfterCreate.store(false, std::memory_order_release);
    gActiveNgxDispatchRoute.store(
        static_cast<uint32_t>(NgxDispatchRoute::ePending),
        std::memory_order_release);
    gActiveNgxGraphicsApi.store(
        static_cast<uint32_t>(NgxGraphicsApi::eUnknown),
        std::memory_order_release);
    gActiveNgxCreateHandle.store(0, std::memory_order_release);
    gActiveNgxEvaluateHandle.store(0, std::memory_order_release);
    gActiveNgxProviderBase.store(0, std::memory_order_release);
    gActiveNgxProviderGeneration.store(0, std::memory_order_release);
    gActiveNgxSelectionSource.store(0, std::memory_order_release);
    SetUniversalRouteFailure(UniversalRouteFailure::eProviderNotSelected);
    Log(L"Active FG pipeline released after accepted Off; the next On/Create "
        L"will select one coherent wrapper/provider route");
}

void ResetReleasedPipelineState(ControlRouteRecord& route) noexcept
{
    std::lock_guard lifetimeLock(gFrameGenerationLifetimeMutex);
    ResetReleasedPipelineStateLocked(route);
}

ControlRouteRecord* RouteForObservedFeature(
    const ampere_backend::FeatureLifetimeEvent& event) noexcept
{
    for (auto& route : gControlRoutes)
    {
        if (route.claimed.load(std::memory_order_acquire)
            && reinterpret_cast<uintptr_t>(route.wrapper) == event.wrapper
            && route.generation == event.wrapperGeneration)
            return &route;
    }
    return nullptr;
}

void ObserveAmpereFeatureLifetime(
    const ampere_backend::FeatureLifetimeEvent& event) noexcept
{
    if (!UseAmpere() || !event.handle || !event.lifetime || !event.runtime
        || !event.runtimeGeneration || !event.provider
        || !event.providerGeneration || !event.wrapper
        || !event.wrapperGeneration || !event.publication
        || !event.generatedFrameCapacity || !event.createAttemptToken)
        return;

    ControlRouteRecord* route = RouteForObservedFeature(event);
    if (!route)
    {
        gRestartRequired.store(true, std::memory_order_release);
        return;
    }

    std::lock_guard lifetimeLock(gFrameGenerationLifetimeMutex);
    if (event.phase == ampere_backend::FeatureLifetimePhase::eCreated)
    {
        // Older queued completions cannot adopt a newer in-flight operation's
        // epoch even when NGX reuses the same numeric handle and modules.
        if (event.createAttemptToken != gFrameGenerationCreateAttemptEpoch.load(
                std::memory_order_acquire))
            return;
        if (!AmpereFeatureEntriesCurrent(event.runtime, event.runtimeGeneration,
                event.provider, event.providerGeneration)
            || event.provider
                != gActiveNgxProviderBase.load(std::memory_order_acquire)
            || event.providerGeneration
                != gActiveNgxProviderGeneration.load(std::memory_order_acquire)
            || event.publication != ampere_gpu::Publication()
            || event.adapterLuid != ampere_gpu::AdapterLuid())
        {
            gRestartRequired.store(true, std::memory_order_release);
            return;
        }

        // The backend snapshots mutations under its lifecycle lock and drains
        // them in strict FIFO order after releasing it. Keep the monotonic
        // lifetime comparison as a defensive identity gate for delayed or
        // reentrant observer delivery and numeric handle reuse.
        if (gRetainedFrameGenerationFeature
            && gRetainedFrameGenerationFeature.runtime == event.runtime
            && gRetainedFrameGenerationFeature.runtimeGeneration
                == event.runtimeGeneration
            && gRetainedFrameGenerationFeature.lifetime > event.lifetime)
            return;

        const bool replacesAcceptedOff = gRetainedFrameGenerationFeature
            && SameRetainedFeature(gAcceptedOffEvidence.feature,
                gRetainedFrameGenerationFeature);
        const bool followsExactRelease = route->releaseObserved.load(
            std::memory_order_acquire);
        gRetainedFrameGenerationFeature = {
            event.handle, event.runtime, event.runtimeGeneration,
            event.provider, event.providerGeneration,
            event.wrapper, event.wrapperGeneration,
            event.publication, event.adapterLuid,
            event.generatedFrameCapacity, event.lifetime,
            event.createAttemptToken};
        gFreshRecreatedFrameGenerationFeature = followsExactRelease
            ? gRetainedFrameGenerationFeature : RetainedFeatureIdentity{};
        gAcceptedOffEvidence = {};
        route->releaseObserved.store(false, std::memory_order_release);
        gFrameGenerationCreateObserved.store(true, std::memory_order_release);
        if ((!gRestartRequired.load(std::memory_order_acquire)
                || replacesAcceptedOff || followsExactRelease)
            && !gProviderChangedAfterCreate.load(std::memory_order_acquire))
            gRestartRequired.store(false, std::memory_order_release);
        return;
    }

    if (event.phase != ampere_backend::FeatureLifetimePhase::eReleased
        || !SameObservedFeature(gRetainedFrameGenerationFeature, event))
        return;

    if (gRetainedFrameGenerationFeature.createAttemptEpoch
        != gFrameGenerationCreateAttemptEpoch.load(std::memory_order_acquire))
    {
        // A newer Create has begun after this exact release mutation but before
        // observer delivery. Retire only the old feature-scoped state: clearing
        // the active dispatch/provider selection here would clobber the newer
        // in-flight pre-Create publication. FIFO delivery lets its successful
        // Created event consume releaseObserved and certify a fresh lifetime;
        // if it fails, restart remains required.
        gRetainedFrameGenerationFeature = {};
        gFreshRecreatedFrameGenerationFeature = {};
        gAcceptedOffEvidence = {};
        gPresentationLifecycleEpoch.fetch_add(1, std::memory_order_acq_rel);
        gFgVsyncSupportKnown.store(false, std::memory_order_release);
        gFgVsyncSupported.store(false, std::memory_order_release);
        gDynamicMfgCapabilityKnown.store(false, std::memory_order_release);
        gDynamicMfgSupported.store(false, std::memory_order_release);
        ClearAppliedDynamicTelemetry();
        route->frameGenerationOffAccepted.store(false,
            std::memory_order_release);
        route->releaseObserved.store(true, std::memory_order_release);
        gAppliedFrameGenerationOn.store(false, std::memory_order_release);
        gAppliedRevision.store(0, std::memory_order_release);
        gAttemptedRevision.store(0, std::memory_order_release);
        gRestartRequired.store(true, std::memory_order_release);
        return;
    }
    ResetReleasedPipelineStateLocked(*route);
}

void EnsureAmpereFeatureLifetimeObserver() noexcept
{
    // Storing the same callback is idempotent and avoids a publication window
    // between a separate once flag and the observer pointer itself.
    ampere_backend::SetFeatureLifetimeCallback(
        &ObserveAmpereFeatureLifetime);
}

void SetUniversalRouteFailure(UniversalRouteFailure failure) noexcept
{
    gUniversalRouteFailure.store(static_cast<uint32_t>(failure),
        std::memory_order_release);
}

// The caller holds gActiveControlRouteMutex through slot and metadata publication.
bool PublishActiveControlRoute(ControlRouteRecord& route)
{
    if (UseAmpere())
    {
        ampere_backend::ObserveActiveWrapper(route.wrapper, route.generation);
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(route.wrapper);
    const uintptr_t previous = gActiveWrapperBase.exchange(
        base, std::memory_order_acq_rel);
    gActiveWrapperPatched.store(route.wrapperPatched,
        std::memory_order_release);
    gActiveWrapperObserved.store(true, std::memory_order_release);
    gActiveWrapperUsesNvidiaOta.store(
        UsesNvidiaOtaCache(route.path), std::memory_order_release);
    gActiveWrapperVersionMajor.store(route.version.major,
        std::memory_order_release);
    gActiveWrapperVersionMinor.store(route.version.minor,
        std::memory_order_release);
    gActiveWrapperVersionBuild.store(route.version.build,
        std::memory_order_release);
    gActiveWrapperVersionPrivate.store(route.version.privatePart,
        std::memory_order_release);
    gWrapperCompiledMaximumGeneratedFrames.store(
        route.compiledMaximumGeneratedFrames, std::memory_order_release);
    if (previous != base)
    {
        gFgVsyncSupportKnown.store(false, std::memory_order_release);
        gFgVsyncSupported.store(false, std::memory_order_release);
        gDynamicMfgCapabilityKnown.store(false, std::memory_order_release);
        gDynamicMfgSupported.store(false, std::memory_order_release);
    }
    return previous != base;
}

bool ActivateControlRoute(uint32_t slot, ControlEntryPath path,
    bool setter) noexcept
{
    ControlRouteRecord* route = ControlRouteAt(slot);
    if (!route)
        return false;
    std::unique_lock publicationLock(gActiveControlRouteMutex);
    uint32_t active = gActiveControlRouteSlot.load(
        std::memory_order_acquire);
    if (active == UINT32_MAX)
    {
        gActiveControlRouteSlot.compare_exchange_strong(
            active, slot, std::memory_order_acq_rel,
            std::memory_order_acquire);
        active = gActiveControlRouteSlot.load(std::memory_order_acquire);
    }
    if (active != slot)
    {
        ControlRouteRecord* activeRoute = ControlRouteAt(active);
        const universal_route_policy::Identity activeIdentity{
            reinterpret_cast<uintptr_t>(
                activeRoute ? activeRoute->wrapper : nullptr),
            activeRoute ? activeRoute->generation : 0};
        const universal_route_policy::Identity candidateIdentity{
            reinterpret_cast<uintptr_t>(route->wrapper), route->generation};
        universal_route_policy::Lifecycle lifecycle{};
        lifecycle.frameGenerationOn = gAppliedFrameGenerationOn.load(
            std::memory_order_acquire);
        lifecycle.pipelineCreated = gFrameGenerationCreateObserved.load(
            std::memory_order_acquire);
        if (!universal_route_policy::CanActivateRoute(
                activeIdentity, candidateIdentity, lifecycle))
        {
            SetUniversalRouteFailure(
                UniversalRouteFailure::eWrapperChangedAfterCreate);
            gRestartRequired.store(true, std::memory_order_release);
            return false;
        }
        gActiveControlRouteSlot.store(slot, std::memory_order_release);
    }

    if (setter)
    {
        route->activeSetterPath.store(static_cast<uint32_t>(path),
            std::memory_order_release);
        route->setterCalls.fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
        route->activeStatePath.store(static_cast<uint32_t>(path),
            std::memory_order_release);
        route->stateCalls.fetch_add(1, std::memory_order_relaxed);
    }
    route->lastCallTick.store(GetTickCount64(), std::memory_order_release);
    route->lastCallRevision.store(
        gDesiredRevision.load(std::memory_order_acquire),
        std::memory_order_release);
    const bool changed = PublishActiveControlRoute(*route);
    publicationLock.unlock();
    if (changed)
    {
        Log(L"Active DLSS-G wrapper selected by real call: generation=%llu "
            L"patched=%d compiledMaximum=%u version=%u.%u.%u.%u path=%s",
            static_cast<unsigned long long>(route->generation),
            route->wrapperPatched.load(), route->compiledMaximumGeneratedFrames.load(),
            route->version.major, route->version.minor, route->version.build,
            route->version.privatePart, route->path.c_str());
    }
    if (!route->wrapperPatched)
    {
        SetUniversalRouteFailure(
            UniversalRouteFailure::eActiveWrapperUnpatched);
        // A real public call still establishes game intent and accepted FG
        // state. BridgeReady keeps every override disabled for this route.
        // Do not patch generic lifecycle entries merely to observe it.
        return route->structureCompatible.load(std::memory_order_acquire);
    }
    InstallControlRouteLifecycleEntry(slot);
    return route->structureCompatible.load(std::memory_order_acquire);
}

void InvalidateControlRoute(uint32_t slot,
    UniversalRouteFailure failure) noexcept
{
    if (ControlRouteRecord* route = ControlRouteAt(slot))
        route->structureCompatible.store(false, std::memory_order_release);
    if (slot == gActiveControlRouteSlot.load(std::memory_order_acquire))
        InvalidateUiInputEvidence();
    SetUniversalRouteFailure(failure);
}

bool ControlEntryCurrent(entry_detour::Handle handle) noexcept
{
    return handle && entry_detour::ReadSnapshot(handle).current;
}

template <typename Function>
Function* EntryOriginal(const std::atomic<Function*>& published,
    entry_detour::Handle handle) noexcept
{
    if (Function* original = published.load(std::memory_order_acquire))
        return original;
    return handle ? reinterpret_cast<Function*>(
        entry_detour::ReadSnapshot(handle).original) : nullptr;
}

bool ActiveSetterCovered(const ControlRouteRecord& route) noexcept
{
    return universal_route_policy::IsCovered(
        static_cast<ControlEntryPath>(route.activeSetterPath.load(
            std::memory_order_acquire)),
        ControlEntryCurrent(route.publicSetHandle),
        ControlEntryCurrent(route.internalSetHandle),
        route.publicSetResolverFallback.load(std::memory_order_acquire)
            && route.publicSetOriginal.load(std::memory_order_acquire));
}

bool StateEntryCovered(const ControlRouteRecord& route) noexcept
{
    return universal_route_policy::HasCoveredEntry(
        ControlEntryCurrent(route.publicGetHandle),
        ControlEntryCurrent(route.internalGetHandle),
        route.publicGetResolverFallback.load(std::memory_order_acquire)
            && route.publicGetOriginal.load(std::memory_order_acquire));
}

bool SetterEntryCovered(const ControlRouteRecord& route) noexcept
{
    return universal_route_policy::HasCoveredEntry(
        ControlEntryCurrent(route.publicSetHandle),
        ControlEntryCurrent(route.internalSetHandle),
        route.publicSetResolverFallback.load(std::memory_order_acquire)
            && route.publicSetOriginal.load(std::memory_order_acquire));
}

sl::DLSSGOptions CopyKnownOptions(const sl::DLSSGOptions& source, bool preserveNext)
{
    sl::DLSSGOptions copy{};
    copy.next = preserveNext ? source.next : nullptr;
    copy.structType = source.structType;
    copy.structVersion = source.structVersion;
    copy.mode = source.mode;
    copy.numFramesToGenerate = source.numFramesToGenerate;
    copy.flags = source.flags;
    copy.dynamicResWidth = source.dynamicResWidth;
    copy.dynamicResHeight = source.dynamicResHeight;
    copy.numBackBuffers = source.numBackBuffers;
    copy.mvecDepthWidth = source.mvecDepthWidth;
    copy.mvecDepthHeight = source.mvecDepthHeight;
    copy.colorWidth = source.colorWidth;
    copy.colorHeight = source.colorHeight;
    copy.colorBufferFormat = source.colorBufferFormat;
    copy.mvecBufferFormat = source.mvecBufferFormat;
    copy.depthBufferFormat = source.depthBufferFormat;
    copy.hudLessBufferFormat = source.hudLessBufferFormat;
    copy.uiBufferFormat = source.uiBufferFormat;
    copy.onErrorCallback = source.onErrorCallback;
    if (source.structVersion >= sl::kStructVersion2)
        copy.bReserved15 = source.bReserved15;
    if (source.structVersion >= sl::kStructVersion3)
        copy.queueParallelismMode = source.queueParallelismMode;
    if (source.structVersion >= sl::kStructVersion4)
        copy.enableUserInterfaceRecomposition = source.enableUserInterfaceRecomposition;
    if (source.structVersion >= sl::kStructVersion5)
        copy.dynamicTargetFrameRate = source.dynamicTargetFrameRate;
    return copy;
}

sl::DLSSGOptions BuildAdjustedOptions(
    const sl::DLSSGOptions& source, const ControlSnapshot& snapshot,
    bool preserveNext)
{
    sl::DLSSGOptions adjusted = CopyKnownOptions(source, preserveNext);
    if (!snapshot.control.followGame && snapshot.control.dynamic
        && DynamicMfgSupported())
    {
        // The injected object is a complete v5 structure even when the game
        // supplied an older prefix, so the active wrapper can consume the
        // dynamic target without reading beyond the game's allocation.
        adjusted.structVersion = sl::kStructVersion5;
        adjusted.mode = sl::DLSSGMode::eDynamic;
        adjusted.dynamicTargetFrameRate =
            static_cast<float>(snapshot.control.dynamicTargetFrameRate);
    }
    else if (!snapshot.control.followGame && !snapshot.control.dynamic)
    {
        adjusted.mode = sl::DLSSGMode::eOn;
        adjusted.numFramesToGenerate =
            EffectiveMultiplier(snapshot.control) - 1;
    }
    if (UseAmpere())
    {
        if (snapshot.control.multiplier == 1 || source.mode == sl::DLSSGMode::eOff)
            adjusted.mode = sl::DLSSGMode::eOff;
        else if (snapshot.control.dynamic && DynamicMfgSupported())
        {
            adjusted.structVersion = sl::kStructVersion5;
            adjusted.mode = sl::DLSSGMode::eDynamic;
        }
        else
            adjusted.mode = sl::DLSSGMode::eOn;
        adjusted.numFramesToGenerate = std::max(EffectiveMultiplier(snapshot.control), 2u) - 1u;
        adjusted.dynamicTargetFrameRate = adjusted.mode == sl::DLSSGMode::eDynamic
            ? static_cast<float>(snapshot.control.dynamicTargetFrameRate) : 0.0f;
        adjusted.flags = static_cast<sl::DLSSGFlags>(static_cast<uint32_t>(adjusted.flags)
            & ~static_cast<uint32_t>(sl::DLSSGFlags::eShowOnlyInterpolatedFrame));
    }
    if (snapshot.control.generatedOnlyDebug)
    {
        adjusted.flags = static_cast<sl::DLSSGFlags>(
            static_cast<uint32_t>(adjusted.flags)
            | static_cast<uint32_t>(
                sl::DLSSGFlags::eShowOnlyInterpolatedFrame));
    }
    // UI recomposition changes provider allocations. Preserve the game's
    // versioned option exactly; tagged-input metadata cannot authorize it.
    return adjusted;
}

void CaptureGameOptions(
    const sl::ViewportHandle& viewport, const sl::DLSSGOptions& options)
{
    const uint32_t capturedViewport = static_cast<uint32_t>(viewport);
    if (capturedViewport != gLastOptionsViewport.load(std::memory_order_acquire)
        || options.colorWidth != gGameColorWidth.load(std::memory_order_relaxed)
        || options.colorHeight != gGameColorHeight.load(std::memory_order_relaxed)
        || options.hudLessBufferFormat != gGameHudlessBufferFormat.load(std::memory_order_relaxed)
        || options.uiBufferFormat != gGameUiBufferFormat.load(std::memory_order_relaxed))
        InvalidateUiInputEvidence(capturedViewport);
    {
        std::lock_guard lock(gLastOptionsMutex);
        gLastGameOptions.viewport = viewport;
        gLastGameOptions.options = CopyKnownOptions(options, false);
        gLastGameOptions.valid = true;
    }
    const uint32_t viewportValue = static_cast<uint32_t>(viewport);
    gLastOptionsViewport.store(viewportValue, std::memory_order_release);
    gGameOptionsStructVersion.store(
        static_cast<uint32_t>(options.structVersion), std::memory_order_relaxed);
    gGameColorWidth.store(options.colorWidth, std::memory_order_relaxed);
    gGameColorHeight.store(options.colorHeight, std::memory_order_relaxed);
    gGameHudlessBufferFormat.store(options.hudLessBufferFormat, std::memory_order_relaxed);
    gGameUiBufferFormat.store(options.uiBufferFormat, std::memory_order_relaxed);
    gGameUiRecompositionEnabled.store(options.structVersion >= sl::kStructVersion4
        && options.enableUserInterfaceRecomposition == sl::Boolean::eTrue,
        std::memory_order_relaxed);
}

bool ReadLastGameOptions(
    const sl::ViewportHandle& viewport, sl::DLSSGOptions& options)
{
    std::lock_guard lock(gLastOptionsMutex);
    if (!gLastGameOptions.valid
        || static_cast<uint32_t>(gLastGameOptions.viewport)
            != static_cast<uint32_t>(viewport))
        return false;
    options = gLastGameOptions.options;
    return true;
}

void RecordAppliedControl(const ControlSnapshot& snapshot, sl::Result result,
    bool liveReapply, bool uiRecompositionEnabled, bool uiRecompositionForced,
    uint32_t effectiveMultiplier, bool effectiveDynamicMode,
    bool effectiveDynamicExperimental56, bool dynamicOverrideApplied,
    float effectiveDynamicTargetFrameRate, bool effectiveDynamicTargetValid)
{
    gSetOptionsSeen.store(true, std::memory_order_release);
    gLastSetOptionsResult.store(static_cast<int32_t>(result), std::memory_order_relaxed);
    gLastAttemptTick.store(GetTickCount64(), std::memory_order_relaxed);
    gAttemptedRevision.store(snapshot.revision, std::memory_order_release);
    // eWarnOutOfVRAM is emitted after Streamline accepts work when DXGI reports
    // no remaining budget. Keep the raw warning for telemetry, but do not leave
    // a successfully submitted multiplier permanently marked as pending.
    if (result != sl::Result::eOk && result != sl::Result::eWarnOutOfVRAM)
        return;

    const uint64_t previous = gAppliedRevision.load(std::memory_order_acquire);
    gAppliedDynamicMode.store(effectiveDynamicMode, std::memory_order_relaxed);
    gAppliedMultiplier.store(effectiveMultiplier, std::memory_order_relaxed);
    gAppliedDynamicTargetFrameRate.store(
        effectiveDynamicTargetValid ? effectiveDynamicTargetFrameRate : 0.0f,
        std::memory_order_relaxed);
    gAppliedDynamicTargetValid.store(effectiveDynamicTargetValid,
        std::memory_order_relaxed);
    gAppliedDynamicExperimental56.store(
        effectiveDynamicExperimental56, std::memory_order_relaxed);
    gAppliedGeneratedOnlyDebug.store(
        snapshot.control.generatedOnlyDebug, std::memory_order_relaxed);
    gAppliedUiRecompositionEnabled.store(
        uiRecompositionEnabled, std::memory_order_relaxed);
    gAppliedUiRecompositionForced.store(
        uiRecompositionForced, std::memory_order_relaxed);
    const bool desiredApplied = snapshot.control.followGame
        || (snapshot.control.dynamic
            ? dynamicOverrideApplied && effectiveDynamicMode
            : effectiveMultiplier == snapshot.control.multiplier);
    gAppliedRevision.store(desiredApplied ? snapshot.revision : 0, std::memory_order_release);
    if (!desiredApplied) gAttemptedRevision.store(0, std::memory_order_release);
    if (liveReapply)
        gLiveReapplyCount.fetch_add(1, std::memory_order_relaxed);

    if (previous == snapshot.revision)
        return;
    if (snapshot.control.followGame)
        Log(L"%s follow-game options: dynamic=%d multiplier=%ux result=%d",
            liveReapply ? L"Live-reapplied" : L"Applied",
            effectiveDynamicMode, effectiveMultiplier,
            static_cast<int>(result));
    else if (snapshot.control.dynamic && !dynamicOverrideApplied)
        Log(L"%s dynamic MFG request not applied: capability=%s; "
            L"preserved game options (%s %ux), result=%d",
            liveReapply ? L"Live-reapplied" : L"Applied",
            DynamicMfgCapabilityKnown() ? L"unsupported" : L"checking",
            effectiveDynamicMode ? L"dynamic" : L"fixed",
            effectiveMultiplier, static_cast<int>(result));
    else if (effectiveDynamicMode)
        Log(L"%s dynamic MFG: target=%u FPS experimental56=%d max=%ux result=%d",
            liveReapply ? L"Live-reapplied" : L"Applied",
            snapshot.control.dynamicTargetFrameRate,
            effectiveDynamicExperimental56,
            SafeMaximumMultiplier(),
            static_cast<int>(result));
    else
        Log(L"%s fixed multiplier: %ux, result=%d",
            liveReapply ? L"Live-reapplied" : L"Applied",
            effectiveMultiplier, static_cast<int>(result));
    Log(L"UI recomposition: enabled=%d forced=%d inputsReady=%d "
        L"gameEnabled=%d optionsVersion=%u hudlessFormat=%u uiFormat=%u",
        uiRecompositionEnabled, uiRecompositionForced,
        ReadUiInputSnapshot(gLastOptionsViewport.load(std::memory_order_acquire)).ready,
        gGameUiRecompositionEnabled.load(std::memory_order_relaxed),
        gGameOptionsStructVersion.load(std::memory_order_relaxed),
        gGameHudlessBufferFormat.load(std::memory_order_relaxed),
        gGameUiBufferFormat.load(std::memory_order_relaxed));
}

uint32_t InternalControlBypassDepth() noexcept
{
    const DWORD index = gInternalControlBypassTlsIndex.load(
        std::memory_order_acquire);
    if (index == TLS_OUT_OF_INDEXES)
        return 0;
    return static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(TlsGetValue(index)));
}

class ScopedInternalControlBypass
{
public:
    ScopedInternalControlBypass() noexcept
    {
        index_ = gInternalControlBypassTlsIndex.load(
            std::memory_order_acquire);
        if (index_ == TLS_OUT_OF_INDEXES)
            return;
        previousDepth_ = reinterpret_cast<uintptr_t>(TlsGetValue(index_));
        active_ = TlsSetValue(index_, reinterpret_cast<void*>(
            previousDepth_ + 1)) != FALSE;
    }
    ~ScopedInternalControlBypass()
    {
        if (active_)
        {
            TlsSetValue(index_,
                reinterpret_cast<void*>(previousDepth_));
        }
    }

    ScopedInternalControlBypass(const ScopedInternalControlBypass&) = delete;
    ScopedInternalControlBypass& operator=(
        const ScopedInternalControlBypass&) = delete;

private:
    DWORD index_ = TLS_OUT_OF_INDEXES;
    uintptr_t previousDepth_ = 0;
    bool active_ = false;
};

sl::Result CallRouteSetOptions(ControlRouteRecord& route,
    const sl::ViewportHandle& viewport,
    const sl::DLSSGOptions& options) noexcept
{
    if (auto* original = EntryOriginal(
            route.publicSetOriginal, route.publicSetHandle))
    {
        ScopedInternalControlBypass bypass;
        return original(viewport, options);
    }
    auto* internal = EntryOriginal(
        route.internalSetOriginal, route.internalSetHandle);
    if (!internal)
        return sl::Result::eErrorNotInitialized;
    sl::ViewportHandle viewportCopy{
        static_cast<uint32_t>(viewport)};
    sl::DLSSGOptions optionsCopy = CopyKnownOptions(options, false);
    viewportCopy.next = &optionsCopy;
    ScopedInternalControlBypass bypass;
    return internal(&viewportCopy, nullptr);
}

sl::Result CallRouteGetState(ControlRouteRecord& route,
    const sl::ViewportHandle& viewport, sl::DLSSGState& state,
    const sl::DLSSGOptions* options) noexcept
{
    if (auto* original = EntryOriginal(
            route.publicGetOriginal, route.publicGetHandle))
    {
        ScopedInternalControlBypass bypass;
        return original(viewport, state, options);
    }
    auto* internal = EntryOriginal(
        route.internalGetOriginal, route.internalGetHandle);
    if (!internal)
        return sl::Result::eErrorNotInitialized;
    sl::ViewportHandle viewportCopy{
        static_cast<uint32_t>(viewport)};
    sl::DLSSGOptions optionsCopy{};
    if (options && IsSupportedOptionsVersion(options->structVersion))
    {
        optionsCopy = CopyKnownOptions(*options, false);
        viewportCopy.next = &optionsCopy;
    }
    ScopedInternalControlBypass bypass;
    return internal(&viewportCopy, &state, nullptr);
}

bool RouteHasStateFunction(const ControlRouteRecord& route) noexcept
{
    return EntryOriginal(route.publicGetOriginal, route.publicGetHandle)
        || EntryOriginal(route.internalGetOriginal, route.internalGetHandle);
}

template <typename SubmitOptions, typename QueryState>
sl::Result SubmitAdjustedOptionsImpl(
    ControlRouteRecord& route, const sl::ViewportHandle& viewport,
    const sl::DLSSGOptions& source, const ControlSnapshot& snapshot,
    bool liveReapply, SubmitOptions&& submitOptions,
    QueryState&& queryState, bool hasState,
    const RetainedFeatureIdentity* expectedLiveFeature = nullptr)
{
    const bool dynamicRequested = !snapshot.control.followGame
        && snapshot.control.dynamic;
    if (dynamicRequested && !DynamicMfgCapabilityKnown() && hasState)
    {
        sl::DLSSGState state{};
        const sl::Result stateResult = queryState(state, &source);
        RecordDlssgStateResult(stateResult, state);
    }

    sl::DLSSGOptions adjusted = BuildAdjustedOptions(
        source, snapshot, !liveReapply);
    bool dynamicOverrideApplied = dynamicRequested && DynamicMfgSupported();
    sl::Result result = submitOptions(adjusted);
    sl::Result acceptedResult = result;
    if ((result == sl::Result::eOk || result == sl::Result::eWarnOutOfVRAM)
        && hasState
        && (!liveReapply
            || (dynamicRequested && !DynamicMfgCapabilityKnown())))
    {
        sl::DLSSGState state{};
        const sl::Result stateResult = queryState(state, &adjusted);
        RecordDlssgStateResult(stateResult, state);
    }

    // A capability query can become valid only after the game's original
    // options initialize DLSS-G. Apply Dynamic immediately once that query
    // explicitly confirms support; otherwise leave the game options intact.
    if ((result == sl::Result::eOk || result == sl::Result::eWarnOutOfVRAM)
        && dynamicRequested && !dynamicOverrideApplied
        && DynamicMfgSupported())
    {
        const sl::DLSSGOptions acceptedFallback = adjusted;
        adjusted = BuildAdjustedOptions(
            source, snapshot, !liveReapply);
        dynamicOverrideApplied = true;
        result = submitOptions(adjusted);
        if (IsAcceptedControlResult(result))
            acceptedResult = result;
        else
        {
            // The startup fallback was accepted even if the first Dynamic
            // request failed. Keep that actual state separate from the latest
            // submission error; neither the requested target nor an older
            // accepted target describes the active options in this case.
            adjusted = acceptedFallback;
            dynamicOverrideApplied = false;
        }
    }

    const bool preserveGameControl = snapshot.control.followGame
        || (dynamicRequested && !dynamicOverrideApplied);
    const uint32_t effectiveMultiplier =
        [&]() {
            if (UseAmpere()) return adjusted.mode == sl::DLSSGMode::eOff ? 1u : adjusted.numFramesToGenerate + 1u;
            return preserveGameControl
        ? std::clamp(std::min(source.numFramesToGenerate,
                kMaximumMultiplier - 1u) + 1u,
            kMinimumMultiplier, kMaximumMultiplier)
        : EffectiveMultiplier(snapshot.control);
        }();
    const bool effectiveDynamicMode =
        [&]() {
            if (UseAmpere()) return adjusted.mode == sl::DLSSGMode::eDynamic;
            return preserveGameControl
        ? source.mode == sl::DLSSGMode::eDynamic
            || source.mode == sl::DLSSGMode::eAuto
        : snapshot.control.dynamic;
        }();
    const bool effectiveDynamicExperimental56 = false;
    const bool uiRecompositionEnabled = adjusted.structVersion >= sl::kStructVersion4
        && adjusted.enableUserInterfaceRecomposition == sl::Boolean::eTrue;
    const bool effectiveDynamicTargetValid = adjusted.mode == sl::DLSSGMode::eDynamic
        && std::isfinite(adjusted.dynamicTargetFrameRate)
        && adjusted.dynamicTargetFrameRate >= 0.0f;
    std::unique_lock<std::mutex> liveFeatureLock;
    if (liveReapply && UseAmpere()
        && adjusted.mode != sl::DLSSGMode::eOff
        && IsAcceptedControlResult(acceptedResult))
    {
        liveFeatureLock = std::unique_lock<std::mutex>(
            gFrameGenerationLifetimeMutex);
        if (!expectedLiveFeature
            || !RetainedFeatureStillExactForLiveOnLocked(
                route, viewport, *expectedLiveFeature))
        {
            gSetOptionsSeen.store(true, std::memory_order_release);
            gLastSetOptionsResult.store(static_cast<int32_t>(result),
                std::memory_order_relaxed);
            gLastAttemptTick.store(GetTickCount64(),
                std::memory_order_relaxed);
            gAttemptedRevision.store(snapshot.revision,
                std::memory_order_release);
            gAppliedRevision.store(0, std::memory_order_release);
            if (gAppliedFrameGenerationOn.exchange(
                    false, std::memory_order_acq_rel))
            {
                gPresentationLifecycleEpoch.fetch_add(
                    1, std::memory_order_acq_rel);
            }
            ClearAppliedDynamicTelemetry();
            gFgVsyncSupportKnown.store(false, std::memory_order_release);
            gFgVsyncSupported.store(false, std::memory_order_release);
            gRestartRequired.store(true, std::memory_order_release);
            Log(L"Accepted live On revision %llu left pending: retained "
                L"feature changed during the setter call",
                static_cast<unsigned long long>(snapshot.revision));
            return HostControlResult(result);
        }
    }
    RecordAppliedControl(snapshot, acceptedResult, liveReapply,
        uiRecompositionEnabled, false,
        effectiveMultiplier, effectiveDynamicMode,
        effectiveDynamicExperimental56, dynamicOverrideApplied,
        adjusted.dynamicTargetFrameRate, effectiveDynamicTargetValid);
    if (result != acceptedResult)
    {
        gLastSetOptionsResult.store(static_cast<int32_t>(result), std::memory_order_relaxed);
        gAttemptedRevision.store(snapshot.revision, std::memory_order_release);
    }
    if (IsAcceptedControlResult(acceptedResult))
    {
        if (liveFeatureLock.owns_lock())
        {
            RecordAcceptedSetOptionsLifecycleLocked(route, viewport,
                adjusted.mode != sl::DLSSGMode::eOff);
        }
        else
        {
            RecordSetOptionsLifecycle(route, viewport,
                adjusted.mode != sl::DLSSGMode::eOff, acceptedResult);
        }
        // The lifecycle helper also serves unsnapshotted game calls. Restore
        // the exact revision accepted by this adjusted submission in case a
        // newer config arrived while the native setter was running.
        route.lastAcceptedRevision.store(snapshot.revision,
            std::memory_order_release);
    }
    return HostControlResult(result);
}

sl::Result SubmitAdjustedOptions(
    ControlRouteRecord& route, const sl::ViewportHandle& viewport,
    const sl::DLSSGOptions& source, const ControlSnapshot& snapshot,
    bool liveReapply,
    const RetainedFeatureIdentity* expectedLiveFeature = nullptr)
{
    const bool hasState = RouteHasStateFunction(route);
    return SubmitAdjustedOptionsImpl(route, viewport, source, snapshot,
        liveReapply,
        [&](const sl::DLSSGOptions& adjusted) {
            return CallRouteSetOptions(route, viewport, adjusted);
        },
        [&](sl::DLSSGState& state, const sl::DLSSGOptions* options) {
            return CallRouteGetState(route, viewport, state, options);
        }, hasState, expectedLiveFeature);
}

void ReapplyPendingControl(const sl::ViewportHandle& viewport)
{
    if (!gControlReady.load(std::memory_order_acquire)
        || !gGameFrameGenerationOn.load(std::memory_order_acquire)
        || gGameFrameGenerationViewport.load(std::memory_order_acquire)
            != static_cast<uint32_t>(viewport)
        || gRestartRequired.load(std::memory_order_acquire)
        || !BridgeReady())
        return;

    const ControlSnapshot snapshot = ReadControlSnapshot();
    if (snapshot.revision == 0
        || snapshot.revision == gAppliedRevision.load(std::memory_order_acquire))
        return;
    if (!snapshot.control.followGame && snapshot.control.dynamic
        && !DynamicMfgSupported())
        return;
    // Keep an unavailable fixed request pending without repeatedly submitting
    // a lower fallback or labelling that fallback as the requested revision.
    if (!snapshot.control.followGame && !snapshot.control.dynamic
        && snapshot.control.multiplier > SafeMaximumMultiplier())
        return;

    const uint64_t attemptedRevision =
        gAttemptedRevision.load(std::memory_order_acquire);
    bool retryNotInitialized = false;
    if (snapshot.revision == attemptedRevision)
    {
        const int32_t result = gLastSetOptionsResult.load(std::memory_order_relaxed);
        if (result != static_cast<int32_t>(sl::Result::eErrorNotInitialized))
            return;
        const uint64_t now = GetTickCount64();
        const uint64_t previousAttempt =
            gLastAttemptTick.load(std::memory_order_relaxed);
        if (now < previousAttempt
            || now - previousAttempt < kNotInitializedRetryDelayMs)
            return;
        retryNotInitialized = true;
    }

    ControlRouteRecord* route = ActiveControlRoute();
    sl::DLSSGOptions source{};
    if (!route || !ReadLastGameOptions(viewport, source))
        return;
    RetainedFeatureIdentity expectedLiveFeature{};
    std::unique_lock<std::mutex> lifetimeLock(gFrameGenerationLifetimeMutex, std::defer_lock);
    if (UseAmpere())
    {
        lifetimeLock.lock();
        const uint64_t token = gFrameGenerationCreateAttemptEpoch.load(std::memory_order_acquire);
        const auto pending = ampere_backend::FeatureCreatePendingStatus(token);
        if (pending == ampere_backend::FeatureCreatePendingState::eCurrent
            || pending == ampere_backend::FeatureCreatePendingState::eReadBusy)
            return;
        if (pending == ampere_backend::FeatureCreatePendingState::eInvalid)
        {
            gRestartRequired.store(true, std::memory_order_release);
            Log(L"Live On revision %llu blocked: unfinished feature lifecycle proof is no longer exact",
                static_cast<unsigned long long>(snapshot.revision));
            return;
        }
    }
    if (!CanReenableRetainedFeatureLocked(*route, viewport, snapshot, source,
            &expectedLiveFeature))
    {
        // The exact epoch/entry publication uses this same lifetime lock.
        // A newer Create cannot slip between this decision and its latch.
        gRestartRequired.store(true, std::memory_order_release);
        Log(L"Live On revision %llu blocked: retained feature identity, "
            L"capacity, provider, publication, or generation is no longer exact",
            static_cast<unsigned long long>(snapshot.revision));
        return;
    }
    if (lifetimeLock.owns_lock()) lifetimeLock.unlock();
    if (retryNotInitialized)
    {
        const uint64_t retry =
            gNotInitializedRetryCount.fetch_add(1, std::memory_order_relaxed) + 1;
        Log(L"Retrying request revision %llu after Streamline result 21 (retry %llu)",
            static_cast<unsigned long long>(snapshot.revision),
            static_cast<unsigned long long>(retry));
    }

    gSetOptionsCalls.fetch_add(1, std::memory_order_relaxed);
    const sl::Result result =
        SubmitAdjustedOptions(*route, viewport, source, snapshot, true,
            &expectedLiveFeature);
    if (result != sl::Result::eOk)
        Log(L"Live reapply failed for request revision %llu: result=%d",
            static_cast<unsigned long long>(snapshot.revision), static_cast<int>(result));
}

bool SameMfgControl(const ControlConfig& left,
    const ControlConfig& right) noexcept
{
    return left.followGame == right.followGame
        && left.multiplier == right.multiplier
        && left.dynamic == right.dynamic
        && left.dynamicTargetFrameRate == right.dynamicTargetFrameRate
        && left.dlssgPreset == right.dlssgPreset
        && left.vsyncMode == right.vsyncMode
        && left.reflexFrameLimitFps == right.reflexFrameLimitFps
        && left.dynamicExperimental56 == right.dynamicExperimental56
        && left.generatedOnlyDebug == right.generatedOnlyDebug;
}

HMODULE ModuleFromAddress(const void* address)
{
    MEMORY_BASIC_INFORMATION memory{};
    return address
        && VirtualQuery(address, &memory, sizeof(memory)) == sizeof(memory)
        ? static_cast<HMODULE>(memory.AllocationBase) : nullptr;
}

void PublishProviderCreateState(HMODULE provider, bool backportReady)
{
    std::lock_guard lock(gModuleMutex);
    for (auto& record : gModuleRecords)
    {
        if (record.module != provider)
            continue;
        record.ngxTemporalPatched = backportReady;
        if (UseAmpere())
        {
            record.ngxPatched = backportReady;
        }
        break;
    }
    RecomputeModuleStateLocked();
}

bool ExportsNgxD3D12Route(HMODULE module) noexcept
{
    return module
        && GetProcAddress(module, "NVSDK_NGX_D3D12_CreateFeature")
        && GetProcAddress(module, "NVSDK_NGX_D3D12_EvaluateFeature")
        && GetProcAddress(module, "NVSDK_NGX_D3D12_GetFeatureRequirements");
}

bool ExportsNgxVulkanRoute(HMODULE module) noexcept
{
    return module
        && (GetProcAddress(module, "NVSDK_NGX_VULKAN_CreateFeature")
            || GetProcAddress(module, "NVSDK_NGX_VULKAN_CreateFeature1"))
        && GetProcAddress(module, "NVSDK_NGX_VULKAN_EvaluateFeature")
        && GetProcAddress(module, "NVSDK_NGX_VULKAN_GetFeatureRequirements");
}

bool IsNgxRuntimeModule(HMODULE module) noexcept
{
    return ngx_runtime_policy::IsRuntime(module);
}

const wchar_t* NgxDispatchRouteName(NgxDispatchRoute route) noexcept
{
    return route == NgxDispatchRoute::eRuntime ? L"runtime" : L"provider";
}

const wchar_t* NgxGraphicsApiName(NgxGraphicsApi api) noexcept
{
    switch (api)
    {
    case NgxGraphicsApi::eD3D12: return L"D3D12";
    case NgxGraphicsApi::eVulkan: return L"Vulkan";
    default: return L"unknown";
    }
}

bool CanResolveNgxDispatchRoute(NgxDispatchRoute route) noexcept
{
    const auto active = static_cast<NgxDispatchRoute>(
        gActiveNgxDispatchRoute.load(std::memory_order_acquire));
    return universal_route_policy::ShouldInspectNgxCreate(
        true, active, route);
}

bool CommitNgxDispatchRoute(NgxDispatchRoute route) noexcept
{
    uint32_t expected = static_cast<uint32_t>(NgxDispatchRoute::ePending);
    const uint32_t requested = static_cast<uint32_t>(route);
    if (gActiveNgxDispatchRoute.compare_exchange_strong(
            expected, requested, std::memory_order_acq_rel,
            std::memory_order_acquire))
        return true;
    return universal_route_policy::CanCommitNgxDispatchRoute(true,
        static_cast<NgxDispatchRoute>(expected), route);
}

bool CommitNgxGraphicsApi(NgxGraphicsApi api) noexcept
{
    uint32_t expected = static_cast<uint32_t>(NgxGraphicsApi::eUnknown);
    const uint32_t requested = static_cast<uint32_t>(api);
    if (gActiveNgxGraphicsApi.compare_exchange_strong(
            expected, requested, std::memory_order_acq_rel,
            std::memory_order_acquire))
        return true;
    if (expected == requested)
        return true;
    gProviderChangedAfterCreate.store(true, std::memory_order_release);
    gRestartRequired.store(true, std::memory_order_release);
    Log(L"Rejected graphics API change after FG pipeline selection: "
        L"active=%s observed=%s; restart required",
        NgxGraphicsApiName(static_cast<NgxGraphicsApi>(expected)),
        NgxGraphicsApiName(api));
    return false;
}

bool ResolveUniqueHookedProvider(NgxGraphicsApi api, HMODULE& provider,
    std::wstring* path) noexcept
{
    HMODULE candidate = nullptr;
    std::wstring candidatePath;
    uint32_t candidateCount = 0;
    {
        std::lock_guard lock(gModuleMutex);
        for (const auto& record : gModuleRecords)
        {
            if (!record.ngxExport || !record.ngxCandidate
                || !record.ngxPatched)
                continue;
            const entry_detour::Kind createKind =
                api == NgxGraphicsApi::eVulkan
                ? entry_detour::Kind::eNgxVulkanCreateFeature
                : entry_detour::Kind::eNgxD3D12CreateFeature;
            const entry_detour::Kind create1Kind =
                api == NgxGraphicsApi::eVulkan
                ? entry_detour::Kind::eNgxVulkanCreateFeature1
                : entry_detour::Kind::eCount;
            const entry_detour::Kind evaluateKind =
                api == NgxGraphicsApi::eVulkan
                ? entry_detour::Kind::eNgxVulkanEvaluateFeature
                : entry_detour::Kind::eNgxD3D12EvaluateFeature;
            const entry_detour::Snapshot create =
                entry_detour::ReadSnapshot(createKind, record.module);
            const entry_detour::Snapshot create1 =
                create1Kind != entry_detour::Kind::eCount
                ? entry_detour::ReadSnapshot(create1Kind, record.module)
                : entry_detour::Snapshot{};
            const entry_detour::Snapshot evaluate =
                entry_detour::ReadSnapshot(evaluateKind, record.module);
            const entry_detour::Snapshot selectedCreate = create.current
                ? create : create1;
            if (!selectedCreate.current || !evaluate.current
                || selectedCreate.generation != record.generation
                || evaluate.generation != record.generation)
                continue;
            ++candidateCount;
            candidate = record.module;
            if (path)
                candidatePath = record.path;
        }
    }
    if (universal_route_policy::ResolveProvider(false, candidateCount)
        != universal_route_policy::ProviderResolution::eUniqueCandidate)
        return false;
    provider = candidate;
    if (path)
        *path = std::move(candidatePath);
    return true;
}

bool ResolveMidpointProvider(NgxDispatchRoute route, NgxGraphicsApi api,
    const entry_detour::Snapshot& detour, const void* originalCaller,
    HMODULE& provider, std::wstring* path,
    NgxProviderSelectionSource& source)
{
    provider = nullptr;
    if (path)
        path->clear();
    source = NgxProviderSelectionSource::eNone;
    if (route == NgxDispatchRoute::eProvider)
    {
        provider = detour.owner;
        if (dlssg_provider_policy::IsSupportedRetainedProvider(provider))
        {
            if (path)
                *path = LoadedModulePath(provider);
            source = NgxProviderSelectionSource::eProviderEntry;
            return true;
        }
        return false;
    }

    if (UseAmpere() && api == NgxGraphicsApi::eD3D12)
    {
        const auto create = detour.kind == entry_detour::Kind::eNgxRuntimeD3D12CreateFeature
            ? detour : entry_detour::ReadSnapshot(entry_detour::Kind::eNgxRuntimeD3D12CreateFeature, detour.owner);
        ngx_runtime_dispatch::Selection selected{};
        const auto result = ngx_runtime_dispatch::Read(create, NVSDK_NGX_Feature_FrameGeneration, selected);
        if (result == ngx_runtime_dispatch::ReadResult::eInvalid) return false;
        if (result == ngx_runtime_dispatch::ReadResult::eSelected)
        {
            const auto entry = entry_detour::ReadSnapshot(entry_detour::Kind::eNgxD3D12CreateFeature, selected.provider);
            if (!entry.current || entry.generation != ModuleGeneration(selected.provider)
                || entry.target != reinterpret_cast<void*>(selected.target)
                || !ngx_runtime_dispatch::StillCurrent(selected)
                || !dlssg_provider_policy::IsSupportedRetainedProvider(selected.provider)) return false;
            provider = selected.provider;
            if (path) *path = LoadedModulePath(provider);
            source = NgxProviderSelectionSource::eRuntimeDispatchTable;
            return true;
        }
    }

    // Prefer the preserved caller when the runtime really was entered by a
    // DLSS-G provider. Some NGX runtimes instead receive the call from a
    // runtime-owned dispatcher. In that case, fall back only when discovery
    // has exactly one supported, patched provider with current Create and
    // Evaluate entry detours. Zero or multiple candidates remain fail-closed.
    provider = ModuleFromAddress(originalCaller);
    if (dlssg_provider_policy::IsSupportedRetainedProvider(provider))
    {
        if (path)
            *path = LoadedModulePath(provider);
        source = NgxProviderSelectionSource::eRuntimeCaller;
        return true;
    }
    provider = nullptr;
    if (path)
        path->clear();
    if (!ResolveUniqueHookedProvider(api, provider, path))
        return false;
    source = NgxProviderSelectionSource::eRuntimeUniqueCandidate;
    return true;
}

void BeforeNgxCreateFeatureForRoute(NgxDispatchRoute route,
    NgxGraphicsApi api, void* commandContext, NVSDK_NGX_Feature feature,
    const NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** handle,
    entry_detour::Handle entryHandle, const void* originalCaller) noexcept
{
    (void)parameters;
    (void)handle;

    const bool frameGeneration = feature == NVSDK_NGX_Feature_FrameGeneration;
    // The shared NGX runtime also carries DLSS Super Resolution and other
    // feature traffic. Those calls must reach NVIDIA without MFG logging,
    // route publication, provider selection, or descriptor work.
    if (!frameGeneration)
        return;
    if (UseAmpere())
        EnsureAmpereFeatureLifetimeObserver();
    if (!CanResolveNgxDispatchRoute(route))
        return;
    const uint64_t observedCall = gNgxCreateCalls.fetch_add(
        1, std::memory_order_relaxed) + 1;
    if (observedCall <= 16)
    {
        Log(L"NGX_CREATE observed=%llu dispatch=%s feature=%u "
            L"api=%s frameGeneration=%d",
            static_cast<unsigned long long>(observedCall),
            NgxDispatchRouteName(route), static_cast<uint32_t>(feature),
            NgxGraphicsApiName(api), frameGeneration);
    }

    const uint64_t call = gNgxFrameGenerationCreateCalls.fetch_add(
        1, std::memory_order_relaxed) + 1;
    const entry_detour::Snapshot detour =
        entry_detour::ReadSnapshot(entryHandle);
    HMODULE provider = nullptr;
    std::wstring path;
    NgxProviderSelectionSource selectionSource =
        NgxProviderSelectionSource::eNone;
    const bool providerAccepted = detour.current
        && ResolveMidpointProvider(route, api, detour, originalCaller,
            provider, &path, selectionSource);

    if (!providerAccepted)
    {
        // In particular, do not let an ambiguous shared-runtime observation
        // poison this Create. The nested concrete provider entry can still
        // claim the pending route before NVIDIA executes the implementation.
        if (call == 1 || (call & (call - 1)) == 0)
        {
            Log(L"MFG_CREATE pre-call=%llu dispatch=%s api=%s "
                L"providerAccepted=0 "
                L"routeCommitted=0 adapterVerified=%d "
                L"backportReadyAtCreate=0 targetRva=0x%X "
                L"callerPreserved=%d",
                static_cast<unsigned long long>(call),
                NgxDispatchRouteName(route), NgxGraphicsApiName(api),
                AdapterVerifiedForApi(api),
                detour.targetRva, detour.forwarding);
        }
        return;
    }

    if (!CommitNgxGraphicsApi(api))
        return;

    const uintptr_t providerBase = reinterpret_cast<uintptr_t>(provider);
    uintptr_t activeProvider = gActiveNgxProviderBase.load(
        std::memory_order_acquire);
    if (activeProvider == 0)
    {
        gActiveNgxProviderBase.compare_exchange_strong(
            activeProvider, providerBase, std::memory_order_acq_rel,
            std::memory_order_acquire);
        activeProvider = gActiveNgxProviderBase.load(
            std::memory_order_acquire);
    }
    if (universal_route_policy::SelectProvider(activeProvider, providerBase)
        == universal_route_policy::ProviderSelection::eRejectedChange)
    {
        gProviderChangedAfterCreate.store(true, std::memory_order_release);
        gRestartRequired.store(true, std::memory_order_release);
        gBackportReadyAtCreate.store(false, std::memory_order_release);
        Log(L"Rejected provider change after FG pipeline selection: "
            L"active=%p observed=%p; recreate required",
            reinterpret_cast<void*>(activeProvider), provider);
        return;
    }
    if (!CommitNgxDispatchRoute(route))
        return;

    // A post-call lifecycle observer publishes the opaque handle only after
    // native Create succeeds.  This pre-call epoch prevents an older Release
    // completion from clearing route state after a newer Create has begun.
    const uint64_t providerGeneration = ModuleGeneration(provider);
    const entry_detour::Kind evaluateKind = api == NgxGraphicsApi::eVulkan
        ? route == NgxDispatchRoute::eRuntime
            ? entry_detour::Kind::eNgxRuntimeVulkanEvaluateFeature
            : entry_detour::Kind::eNgxVulkanEvaluateFeature
        : route == NgxDispatchRoute::eRuntime
            ? entry_detour::Kind::eNgxRuntimeD3D12EvaluateFeature
            : entry_detour::Kind::eNgxD3D12EvaluateFeature;
    const entry_detour::Snapshot evaluate =
        entry_detour::ReadSnapshot(evaluateKind, detour.owner);
    bool firstPipelineCreate = false;
    {
        std::unique_lock<std::mutex> lifetimeLock(gFrameGenerationLifetimeMutex, std::defer_lock);
        if (UseAmpere()) lifetimeLock.lock();
        uint64_t createAttemptToken =
            gFrameGenerationCreateAttemptEpoch.fetch_add(
                1, std::memory_order_acq_rel) + 1;
        if (!createAttemptToken)
        {
            createAttemptToken =
                gFrameGenerationCreateAttemptEpoch.fetch_add(
                    1, std::memory_order_acq_rel) + 1;
        }
        if (UseAmpere())
        {
            ampere_backend::SetCurrentFeatureCreateAttemptToken(
                createAttemptToken);
        }
        firstPipelineCreate =
            !gFrameGenerationCreateObserved.exchange(
                true, std::memory_order_acq_rel);
        gActiveNgxProviderGeneration.store(providerGeneration, std::memory_order_release);
        gActiveNgxSelectionSource.store(static_cast<uint32_t>(selectionSource),
            std::memory_order_release);
        gActiveNgxCreateHandle.store(PackEntryHandle(entryHandle),
            std::memory_order_release);
        if (evaluate.current)
        {
            gActiveNgxEvaluateHandle.store(
                PackEntryHandle(evaluate.handle), std::memory_order_release);
        }
    }
    if (selectionSource
        == NgxProviderSelectionSource::eRuntimeUniqueCandidate
        && firstPipelineCreate)
    {
        Log(L"Runtime Create caller was not a provider; selected the one "
            L"fully covered DLSS-G provider: path=%s", path.c_str());
    }

    // The loader-friendly route cannot assume that Streamline exposed
    // slSetD3DDevice before this call. The command list is authoritative for
    // the device that will create the FG feature, so validate that adapter at
    // the last safe pre-create point.
    if (api == NgxGraphicsApi::eD3D12
        && !gpu_backend::AdapterVerified() && commandContext)
    {
        auto* commandList = static_cast<ID3D12GraphicsCommandList*>(
            commandContext);
        ID3D12Device* device = nullptr;
        if (SUCCEEDED(commandList->GetDevice(
                __uuidof(ID3D12Device), reinterpret_cast<void**>(&device)))
            && device)
        {
            gpu_backend::ObserveD3D12Device(device);
            device->Release();
        }
    }
    if (gpu_dispatch::IsAda())
    {
        dlssg_preset::Prepare();
        InspectLoadedModule(provider, path);
    }
    const bool adapterVerified = AdapterVerifiedForApi(api);
    // Temporal publication alone is insufficient: the provider must also
    // accept the multi-frame count/index before the first feature compiles.
    const bool deviceGateReady = !gpu_dispatch::IsAda() || ngx_mfg_gate::Ready(provider);
    bool ready = adapterVerified && deviceGateReady
        && gpu_backend::PatchProvider(provider, path.c_str());
#if MFG_UNLOCK_OUTPUT_PULL_MASK_ONLY
    if (ready && gpu_dispatch::IsAda())
    {
        const midpoint_fix::OutputPullMaskCreateBoundary boundary{
            api == NgxGraphicsApi::eD3D12
                ? midpoint_fix::OutputPullMaskGraphicsApi::eD3D12
                : api == NgxGraphicsApi::eVulkan
                    ? midpoint_fix::OutputPullMaskGraphicsApi::eVulkan
                    : midpoint_fix::OutputPullMaskGraphicsApi::eUnknown,
            firstPipelineCreate,
            gPipelineMayPredateDetour.load(std::memory_order_acquire),
            OutputPullMaskEarlyInitProven(provider, api)};
        midpoint_fix::PrepareOutputPullMaskForCreate(
            provider, path.c_str(), boundary);
        const auto mask = midpoint_fix::ReadOutputPullMaskSnapshot();
        // An unsupported optional optimization retains the temporal route.
        // Uncertain publication/protection ownership must never be hidden by
        // the readiness value captured before the optional publisher ran.
        if (mask.requiresRestart)
        {
            ready = false;
            gRestartRequired.store(true, std::memory_order_release);
        }
        else
        {
            ready = gpu_backend::Ready();
        }
    }
#endif
#if (MFG_UNLOCK_OUTPUT_PULL_EXPERIMENT || MFG_UNLOCK_OUTPUT_PULL_TELEMETRY)
    const bool outputPullReady = ready && api == NgxGraphicsApi::eD3D12
        && midpoint_fix::PrepareOutputPullForCreate(provider, path.c_str());
    const bool prev2CurrReady = outputPullReady
        && midpoint_fix::PreparePrev2CurrForCreate(provider, path.c_str());
    output_pull_telemetry::RecordCreate(outputPullReady, prev2CurrReady);
#endif
    gBackportReadyAtCreate.store(ready, std::memory_order_release);
    if (firstPipelineCreate)
        gFirstCreateMidpointReady.store(ready, std::memory_order_release);
    if (provider)
        PublishProviderCreateState(provider, ready);
    if (ready)
    {
        gPipelineMayPredateDetour.store(false, std::memory_order_release);
        gRestartRequired.store(false, std::memory_order_release);
    }
    if (call == 1 || (call & (call - 1)) == 0)
    {
        Log(L"MFG_CREATE pre-call=%llu dispatch=%s api=%s "
            L"providerAccepted=%d "
            L"routeCommitted=1 adapterVerified=%d backportReadyAtCreate=%d "
            L"targetRva=0x%X callerPreserved=%d",
            static_cast<unsigned long long>(call),
            NgxDispatchRouteName(route), NgxGraphicsApiName(api),
            providerAccepted,
            adapterVerified, ready, detour.targetRva, detour.forwarding);
    }
}

void WINAPI BeforeNgxD3D12CreateFeature(void* commandList,
    uintptr_t feature, const void* parameters, void* handle,
    uintptr_t, uintptr_t,
    entry_detour::Handle entryHandle,
    const void* originalCaller) noexcept
{
    BeforeNgxCreateFeatureForRoute(NgxDispatchRoute::eProvider,
        NgxGraphicsApi::eD3D12, commandList,
        static_cast<NVSDK_NGX_Feature>(feature),
        static_cast<const NVSDK_NGX_Parameter*>(parameters),
        static_cast<NVSDK_NGX_Handle**>(handle), entryHandle,
        originalCaller);
}

void WINAPI BeforeNgxRuntimeD3D12CreateFeature(void* commandList,
    uintptr_t feature, const void* parameters, void* handle,
    uintptr_t, uintptr_t,
    entry_detour::Handle entryHandle,
    const void* originalCaller) noexcept
{
    BeforeNgxCreateFeatureForRoute(NgxDispatchRoute::eRuntime,
        NgxGraphicsApi::eD3D12, commandList,
        static_cast<NVSDK_NGX_Feature>(feature),
        static_cast<const NVSDK_NGX_Parameter*>(parameters),
        static_cast<NVSDK_NGX_Handle**>(handle), entryHandle,
        originalCaller);
}

bool TryInstallNgxCreateEntryDetour(
    HMODULE provider, const std::wstring& path, uint64_t generation)
{
    return ampere_backend::InstallRoute(provider, path.c_str(), generation, false,
        &BeforeNgxD3D12CreateFeature, &BeforeNgxD3D12EvaluateFeature);
}

void BeforeNgxEvaluateFeatureForRoute(NgxDispatchRoute route,
    NgxGraphicsApi api, void* commandList, uintptr_t handleValue,
    const void* parameters,
    void* callback, entry_detour::Handle entryHandle,
    const void* originalCaller) noexcept
{
    const uint32_t active = gActiveNgxDispatchRoute.load(
        std::memory_order_acquire);
    if (gActiveNgxGraphicsApi.load(std::memory_order_acquire)
        != static_cast<uint32_t>(api))
        return;

    if (UseAmpere())
    {
        // The Ampere gate invokes this only inside an owned runtime operation.
        // Check the pinned entry identity without reopening the provider file on
        // every frame. Optional interval collection remains disabled by default.
        const auto verified = entry_detour::ReadSnapshot(entryHandle);
        if (api != NgxGraphicsApi::eD3D12 || route != NgxDispatchRoute::eProvider
            || !verified.current || verified.kind != entry_detour::Kind::eNgxD3D12EvaluateFeature
            || verified.owner != ampere_gpu::Provider()
            || reinterpret_cast<uintptr_t>(verified.owner) != gActiveNgxProviderBase.load(std::memory_order_acquire)
            || verified.generation != gActiveNgxProviderGeneration.load(std::memory_order_acquire))
            return;
        const auto providerEvaluate = entry_detour::ReadSnapshot(
            entry_detour::Kind::eNgxD3D12EvaluateFeature, verified.owner);
        const auto selected = entry_detour::ReadSnapshot(UnpackEntryHandle(
            gActiveNgxEvaluateHandle.load(std::memory_order_acquire)));
        if (!providerEvaluate.current || providerEvaluate.currentEntries != 1
            || providerEvaluate.handle != entryHandle || !selected.current)
            return;
        if (active == static_cast<uint32_t>(NgxDispatchRoute::eProvider))
        {
            if (selected.kind != entry_detour::Kind::eNgxD3D12EvaluateFeature
                || selected.handle != entryHandle)
                return;
        }
        else if (active == static_cast<uint32_t>(NgxDispatchRoute::eRuntime))
        {
            // Selection belongs to the outer runtime, while this observation
            // still comes only from the admitted nested provider entry. Never
            // record the outer callback or replace the selected route handle.
            const auto runtimeEvaluate = entry_detour::ReadSnapshot(
                entry_detour::Kind::eNgxRuntimeD3D12EvaluateFeature, selected.owner);
            const auto create = entry_detour::ReadSnapshot(UnpackEntryHandle(
                gActiveNgxCreateHandle.load(std::memory_order_acquire)));
            if (selected.kind != entry_detour::Kind::eNgxRuntimeD3D12EvaluateFeature
                || selected.owner != ModuleFromAddress(originalCaller)
                || !runtimeEvaluate.current || runtimeEvaluate.currentEntries != 1
                || runtimeEvaluate.handle != selected.handle
                || !create.current || create.kind != entry_detour::Kind::eNgxRuntimeD3D12CreateFeature
                || create.owner != selected.owner || create.generation != selected.generation
                || !AmpereFeatureEntriesCurrent(reinterpret_cast<uintptr_t>(selected.owner), selected.generation,
                    reinterpret_cast<uintptr_t>(verified.owner), verified.generation))
                return;
        }
        else return;
        temporal_interval_trace::Record(reinterpret_cast<const NVSDK_NGX_Handle*>(handleValue),
            static_cast<const NVSDK_NGX_Parameter*>(parameters), gpu_backend::Ready());
        gNgxEvaluateCalls.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    else
    {
        if (active != static_cast<uint32_t>(route))
            return;
        // Evaluate has no feature-id argument. Prefer the selected provider as the
        // caller. Some games (including AFOP) enter the shared runtime through a
        // runtime-owned dispatcher even though Create safely resolved one unique
        // covered DLSS-G provider. For that shape, defer admission until the
        // namespaced temporal parameters independently identify this as FG; an SR
        // or other DLSS 5 Evaluate remains invisible to this telemetry path.
        const bool callerIsSelectedProvider =
            reinterpret_cast<uintptr_t>(ModuleFromAddress(originalCaller))
            == gActiveNgxProviderBase.load(std::memory_order_acquire);
        const bool selectedByUniqueCandidate =
            gActiveNgxSelectionSource.load(std::memory_order_acquire)
            == static_cast<uint32_t>(
                NgxProviderSelectionSource::eRuntimeUniqueCandidate);
        if (route == NgxDispatchRoute::eRuntime
            && !callerIsSelectedProvider && !selectedByUniqueCandidate)
            return;

        const entry_detour::Snapshot detour =
            entry_detour::ReadSnapshot(entryHandle);
        HMODULE observedProvider = nullptr;
        NgxProviderSelectionSource selectionSource =
            NgxProviderSelectionSource::eNone;
        const bool providerResolved = detour.current
            && ResolveMidpointProvider(route, api, detour, originalCaller,
                observedProvider, nullptr, selectionSource);
        if (!providerResolved || reinterpret_cast<uintptr_t>(observedProvider)
                != gActiveNgxProviderBase.load(std::memory_order_acquire))
            return;

        uint64_t activeEvaluate = gActiveNgxEvaluateHandle.load(
            std::memory_order_acquire);
        const uint64_t requested = PackEntryHandle(entryHandle);
        if (activeEvaluate == 0)
        {
            gActiveNgxEvaluateHandle.compare_exchange_strong(
                activeEvaluate, requested, std::memory_order_acq_rel,
                std::memory_order_acquire);
        }
        if (gActiveNgxEvaluateHandle.load(std::memory_order_acquire)
            != requested)
            return;
        (void)commandList;
        (void)callback;
        const auto* handle = reinterpret_cast<const NVSDK_NGX_Handle*>(
            handleValue);
        const auto* ngxParameters = static_cast<const NVSDK_NGX_Parameter*>(
            parameters);
        if (route == NgxDispatchRoute::eRuntime
            && !callerIsSelectedProvider)
        {
            const bool validTemporalSample =
                temporal_interval_trace::RecordIfValidTemporalSample(
                    handle, ngxParameters, gpu_backend::Ready());
            if (!universal_route_policy::CanInspectRuntimeEvaluate(false,
                    selectedByUniqueCandidate, validTemporalSample))
                return;
        }
        else
        {
            temporal_interval_trace::Record(
                handle, ngxParameters, gpu_backend::Ready());
        }
        gNgxEvaluateCalls.fetch_add(1, std::memory_order_relaxed);
    }
#if MFG_UNLOCK_OUTPUT_PULL_TELEMETRY
    output_pull_telemetry::RecordEvaluate(handle);
#endif
}

void WINAPI BeforeNgxD3D12EvaluateFeature(void* commandList,
    uintptr_t handle, const void* parameters, void* callback,
    uintptr_t, uintptr_t,
    entry_detour::Handle entryHandle,
    const void* originalCaller) noexcept
{
    BeforeNgxEvaluateFeatureForRoute(NgxDispatchRoute::eProvider,
        NgxGraphicsApi::eD3D12, commandList, handle, parameters, callback,
        entryHandle,
        originalCaller);
}

void WINAPI BeforeNgxRuntimeD3D12EvaluateFeature(void* commandList,
    uintptr_t handle, const void* parameters, void* callback,
    uintptr_t, uintptr_t,
    entry_detour::Handle entryHandle,
    const void* originalCaller) noexcept
{
    BeforeNgxEvaluateFeatureForRoute(NgxDispatchRoute::eRuntime,
        NgxGraphicsApi::eD3D12, commandList, handle, parameters, callback,
        entryHandle,
        originalCaller);
}

bool TryInstallNgxEvaluateEntryDetour(
    HMODULE provider, const std::wstring& path, uint64_t generation)
{
    return ampere_backend::InstallRoute(provider, path.c_str(), generation, false,
        &BeforeNgxD3D12CreateFeature, &BeforeNgxD3D12EvaluateFeature);
}

bool TryInstallNgxRuntimeCreateEntryDetour(
    HMODULE runtime, const std::wstring& path, uint64_t generation)
{
    if (!IsNgxRuntimeModule(runtime)) return false;
    return ampere_backend::InstallRoute(runtime, path.c_str(), generation, true,
        &BeforeNgxRuntimeD3D12CreateFeature, &BeforeNgxRuntimeD3D12EvaluateFeature);
}

bool TryInstallNgxRuntimeEvaluateEntryDetour(
    HMODULE runtime, const std::wstring& path, uint64_t generation)
{
    if (!IsNgxRuntimeModule(runtime)) return false;
    return ampere_backend::InstallRoute(runtime, path.c_str(), generation, true,
        &BeforeNgxRuntimeD3D12CreateFeature, &BeforeNgxRuntimeD3D12EvaluateFeature);
}

void WINAPI BeforeNgxVulkanCreateFeature(void* commandBuffer,
    uintptr_t feature, const void* parameters, void* handle,
    uintptr_t, uintptr_t,
    entry_detour::Handle entryHandle,
    const void* originalCaller) noexcept
{
    BeforeNgxCreateFeatureForRoute(NgxDispatchRoute::eProvider,
        NgxGraphicsApi::eVulkan, commandBuffer,
        static_cast<NVSDK_NGX_Feature>(feature),
        static_cast<const NVSDK_NGX_Parameter*>(parameters),
        static_cast<NVSDK_NGX_Handle**>(handle), entryHandle,
        originalCaller);
}

void WINAPI BeforeNgxRuntimeVulkanCreateFeature(void* commandBuffer,
    uintptr_t feature, const void* parameters, void* handle,
    uintptr_t, uintptr_t,
    entry_detour::Handle entryHandle,
    const void* originalCaller) noexcept
{
    BeforeNgxCreateFeatureForRoute(NgxDispatchRoute::eRuntime,
        NgxGraphicsApi::eVulkan, commandBuffer,
        static_cast<NVSDK_NGX_Feature>(feature),
        static_cast<const NVSDK_NGX_Parameter*>(parameters),
        static_cast<NVSDK_NGX_Handle**>(handle), entryHandle,
        originalCaller);
}

// CreateFeature1 places VkDevice, VkCommandBuffer, feature and parameters in
// the four register arguments; the output handle is the untouched fifth stack
// argument. The forwarding relay preserves it while this pre-call reads only
// the feature and parameters.
void WINAPI BeforeNgxVulkanCreateFeature1(void*, uintptr_t commandBuffer,
    const void* featureValue, void* parameters, uintptr_t, uintptr_t,
    entry_detour::Handle entryHandle,
    const void* originalCaller) noexcept
{
    BeforeNgxCreateFeatureForRoute(NgxDispatchRoute::eProvider,
        NgxGraphicsApi::eVulkan,
        reinterpret_cast<void*>(commandBuffer),
        static_cast<NVSDK_NGX_Feature>(
            reinterpret_cast<uintptr_t>(featureValue)),
        static_cast<const NVSDK_NGX_Parameter*>(parameters), nullptr,
        entryHandle, originalCaller);
}

void WINAPI BeforeNgxRuntimeVulkanCreateFeature1(void*,
    uintptr_t commandBuffer, const void* featureValue, void* parameters,
    uintptr_t, uintptr_t,
    entry_detour::Handle entryHandle,
    const void* originalCaller) noexcept
{
    BeforeNgxCreateFeatureForRoute(NgxDispatchRoute::eRuntime,
        NgxGraphicsApi::eVulkan,
        reinterpret_cast<void*>(commandBuffer),
        static_cast<NVSDK_NGX_Feature>(
            reinterpret_cast<uintptr_t>(featureValue)),
        static_cast<const NVSDK_NGX_Parameter*>(parameters), nullptr,
        entryHandle, originalCaller);
}

NVSDK_NGX_Result NVSDK_CONV RejectUnsupportedVulkanCall(void*, uintptr_t, const void*, void*) noexcept
{
    return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
}

bool WINAPI VulkanCreateGate(void* argument1, uintptr_t argument2, const void* argument3, void* argument4,
    uintptr_t argument5, uintptr_t argument6, entry_detour::Handle handle, const void* caller) noexcept
{
    const auto entry = entry_detour::ReadSnapshot(handle);
    if (!entry.current) return false;
    const bool version1 = entry.kind == entry_detour::Kind::eNgxVulkanCreateFeature1
        || entry.kind == entry_detour::Kind::eNgxRuntimeVulkanCreateFeature1;
    const uintptr_t feature = version1 ? reinterpret_cast<uintptr_t>(argument3) : argument2;
    if (!gpu_dispatch::AllowVulkanCreate(gpu_dispatch::Selected(),
            feature == NVSDK_NGX_Feature_FrameGeneration,
            gVulkanAdapterVerified.load(std::memory_order_acquire)))
        return false;
#if MFG_UNLOCK_OUTPUT_PULL_MASK_ONLY
    // A forwarding gate runs before its pre-call callback. Run preparation
    // inside this gate so an unsafe publication rejects this same native call.
    entry_detour::ForwardPreCall before = nullptr;
    switch (entry.kind)
    {
    case entry_detour::Kind::eNgxVulkanCreateFeature:
        before = &BeforeNgxVulkanCreateFeature; break;
    case entry_detour::Kind::eNgxRuntimeVulkanCreateFeature:
        before = &BeforeNgxRuntimeVulkanCreateFeature; break;
    case entry_detour::Kind::eNgxVulkanCreateFeature1:
        before = &BeforeNgxVulkanCreateFeature1; break;
    case entry_detour::Kind::eNgxRuntimeVulkanCreateFeature1:
        before = &BeforeNgxRuntimeVulkanCreateFeature1; break;
    default:
        return false;
    }
    before(argument1, argument2, argument3, argument4,
        argument5, argument6, handle, caller);
    if (feature == NVSDK_NGX_Feature_FrameGeneration
        && midpoint_fix::OutputPullMaskRequiresRestart())
        return false;
#endif
    return true;
}

bool WINAPI VulkanEvaluateGate(void*, uintptr_t, const void*, void*,
    uintptr_t, uintptr_t, entry_detour::Handle handle, const void*) noexcept
{
    const auto entry = entry_detour::ReadSnapshot(handle);
    return entry.current
#if MFG_UNLOCK_OUTPUT_PULL_MASK_ONLY
        && (entry.kind != entry_detour::Kind::eNgxVulkanEvaluateFeature
            || !midpoint_fix::OutputPullMaskRequiresRestart())
#endif
        && gpu_dispatch::AllowVulkanEvaluate(gpu_dispatch::Selected(),
        entry.kind == entry_detour::Kind::eNgxVulkanEvaluateFeature,
        gVulkanAdapterVerified.load(std::memory_order_acquire));
}

bool InstallNgxVulkanCreateEntry(HMODULE owner,
    const std::wstring& path, uint64_t generation, const char* exportName,
    entry_detour::Kind kind, entry_detour::ForwardPreCall callback,
    bool featureIsSecondArgument)
{
    void* target = reinterpret_cast<void*>(
        GetProcAddress(owner, exportName));
    if (!target)
        return false;
    entry_detour::InstallOptions options{};
    options.generation = generation;
    options.allowRelocated = true;
    (void)featureIsSecondArgument; // The gate decodes both public Create ABIs.
#if MFG_UNLOCK_OUTPUT_PULL_MASK_ONLY
    callback = nullptr; // The gate prepares exactly once, then checks publication.
#endif
    void* trampoline = nullptr;
    entry_detour::Handle handle{};
    const bool installed = entry_detour::InstallForwarding(kind, owner,
        target, callback, trampoline, options, &handle,
        &VulkanCreateGate, reinterpret_cast<void*>(&RejectUnsupportedVulkanCall));
    const entry_detour::Snapshot state = entry_detour::ReadSnapshot(handle);
    Log(L"NGX Vulkan %hs entry detour: installed=%d current=%d "
        L"cachedPointersCovered=%d method=%hs failure=%u "
        L"targetRva=0x%X path=%s", exportName, installed, state.current,
        state.cachedPointersCovered, entry_detour::MethodName(state.method),
        static_cast<uint32_t>(state.failure), state.targetRva, path.c_str());
    return installed;
}

bool TryInstallNgxVulkanCreateEntryDetours(
    HMODULE provider, const std::wstring& path, uint64_t generation)
{
    if (!provider
        || !dlssg_provider_policy::IsSupportedProvider(
            provider, path.c_str()))
        return false;
    const bool create = InstallNgxVulkanCreateEntry(provider, path,
        generation, "NVSDK_NGX_VULKAN_CreateFeature",
        entry_detour::Kind::eNgxVulkanCreateFeature,
        &BeforeNgxVulkanCreateFeature, true);
    const bool create1 = InstallNgxVulkanCreateEntry(provider, path,
        generation, "NVSDK_NGX_VULKAN_CreateFeature1",
        entry_detour::Kind::eNgxVulkanCreateFeature1,
        &BeforeNgxVulkanCreateFeature1, false);
    return create || create1;
}

bool TryInstallNgxRuntimeVulkanCreateEntryDetours(
    HMODULE runtime, const std::wstring& path, uint64_t generation)
{
    if (!IsNgxRuntimeModule(runtime))
        return false;
    const bool create = InstallNgxVulkanCreateEntry(runtime, path,
        generation, "NVSDK_NGX_VULKAN_CreateFeature",
        entry_detour::Kind::eNgxRuntimeVulkanCreateFeature,
        &BeforeNgxRuntimeVulkanCreateFeature, true);
    const bool create1 = InstallNgxVulkanCreateEntry(runtime, path,
        generation, "NVSDK_NGX_VULKAN_CreateFeature1",
        entry_detour::Kind::eNgxRuntimeVulkanCreateFeature1,
        &BeforeNgxRuntimeVulkanCreateFeature1, false);
    return create || create1;
}

void WINAPI BeforeNgxVulkanEvaluateFeature(void* commandBuffer,
    uintptr_t handle, const void* parameters, void* callback,
    uintptr_t, uintptr_t,
    entry_detour::Handle entryHandle,
    const void* originalCaller) noexcept
{
    BeforeNgxEvaluateFeatureForRoute(NgxDispatchRoute::eProvider,
        NgxGraphicsApi::eVulkan, commandBuffer, handle, parameters,
        callback, entryHandle, originalCaller);
}

void WINAPI BeforeNgxRuntimeVulkanEvaluateFeature(void* commandBuffer,
    uintptr_t handle, const void* parameters, void* callback,
    uintptr_t, uintptr_t,
    entry_detour::Handle entryHandle,
    const void* originalCaller) noexcept
{
    BeforeNgxEvaluateFeatureForRoute(NgxDispatchRoute::eRuntime,
        NgxGraphicsApi::eVulkan, commandBuffer, handle, parameters,
        callback, entryHandle, originalCaller);
}

bool InstallNgxVulkanEvaluateEntry(HMODULE owner,
    const std::wstring& path, uint64_t generation,
    entry_detour::Kind kind, entry_detour::ForwardPreCall callback)
{
    void* target = reinterpret_cast<void*>(GetProcAddress(
        owner, "NVSDK_NGX_VULKAN_EvaluateFeature"));
    if (!target)
        return false;
    entry_detour::InstallOptions options{};
    options.generation = generation;
    options.allowRelocated = true;
    void* trampoline = nullptr;
    entry_detour::Handle handle{};
    const bool installed = entry_detour::InstallForwarding(kind, owner,
        target, callback, trampoline, options, &handle,
        &VulkanEvaluateGate, reinterpret_cast<void*>(&RejectUnsupportedVulkanCall));
    const entry_detour::Snapshot state = entry_detour::ReadSnapshot(handle);
    Log(L"NGX Vulkan EvaluateFeature entry detour: installed=%d current=%d "
        L"cachedPointersCovered=%d method=%hs failure=%u "
        L"targetRva=0x%X path=%s", installed, state.current,
        state.cachedPointersCovered, entry_detour::MethodName(state.method),
        static_cast<uint32_t>(state.failure), state.targetRva, path.c_str());
    return installed;
}

bool TryInstallNgxVulkanEvaluateEntryDetour(
    HMODULE provider, const std::wstring& path, uint64_t generation)
{
    return provider
        && dlssg_provider_policy::IsSupportedProvider(
            provider, path.c_str())
        && InstallNgxVulkanEvaluateEntry(provider, path, generation,
            entry_detour::Kind::eNgxVulkanEvaluateFeature,
            &BeforeNgxVulkanEvaluateFeature);
}

bool TryInstallNgxRuntimeVulkanEvaluateEntryDetour(
    HMODULE runtime, const std::wstring& path, uint64_t generation)
{
    return IsNgxRuntimeModule(runtime)
        && InstallNgxVulkanEvaluateEntry(runtime, path, generation,
            entry_detour::Kind::eNgxRuntimeVulkanEvaluateFeature,
            &BeforeNgxRuntimeVulkanEvaluateFeature);
}

void PrepareVulkanProviderCapabilities(entry_detour::Handle handle)
{
    const auto entry = entry_detour::ReadSnapshot(handle);
    if (!entry.current || !gpu_dispatch::IsAda()
        || !gVulkanAdapterVerified.load(std::memory_order_acquire)
        || !dlssg_provider_policy::IsSupportedRetainedProvider(entry.owner))
        return;
    // Only the provider actually entered by NGX may select the temporal
    // program. Passive discovery can also see an unused bundled provider
    // while NGX selects a different NVIDIA override image.
    const auto path = LoadedModulePath(entry.owner);
    const auto record = InspectLoadedModule(entry.owner, path);
    const auto create = entry_detour::ReadSnapshot(
        entry_detour::Kind::eNgxVulkanCreateFeature, entry.owner);
    const auto evaluate = entry_detour::ReadSnapshot(
        entry_detour::Kind::eNgxVulkanEvaluateFeature, entry.owner);
    if (!record.ngxPatched || !create.current || !evaluate.current
        || entry.generation != record.generation
        || create.generation != record.generation || evaluate.generation != record.generation
        || !gpu_backend::PatchProvider(entry.owner, path.c_str()))
        return;
    const auto capability = vulkan_capability::Find(entry.owner);
    if (capability.threshold && *capability.threshold != 0x90)
    {
        const bool published = vulkan_capability::Publish(capability);
        Log(L"Vulkan DLSS-G capability maximum: published=%d generatedFrames=5 "
            L"targetRva=0x%X path=%s", published, capability.rva, path.c_str());
    }
}

void WINAPI BeforeNgxVulkanInit(void*, uintptr_t, const void*,
    void* physicalDevice, uintptr_t, uintptr_t, entry_detour::Handle handle,
    const void*) noexcept
{
    const bool verified = physicalDevice
        && gpu_backend::ObserveVulkanPhysicalDevice(physicalDevice);
    gVulkanAdapterVerified.store(verified, std::memory_order_release);
    if (verified)
    {
        if (gpu_dispatch::IsAda()) dlssg_preset::Prepare();
        gModuleInventoryDirty.store(true, std::memory_order_release);
        InspectAlreadyLoadedModules();
        ObserveOutputPullMaskVulkanInit(handle);
        PrepareVulkanProviderCapabilities(handle);
    }
}

void WINAPI BeforeNgxVulkanProjectInit(void*, uintptr_t, const void*,
    void*, uintptr_t, uintptr_t physicalDevice, entry_detour::Handle handle,
    const void*) noexcept
{
    const bool verified = physicalDevice
        && gpu_backend::ObserveVulkanPhysicalDevice(
            reinterpret_cast<void*>(physicalDevice));
    gVulkanAdapterVerified.store(verified, std::memory_order_release);
    if (verified)
    {
        if (gpu_dispatch::IsAda()) dlssg_preset::Prepare();
        gModuleInventoryDirty.store(true, std::memory_order_release);
        InspectAlreadyLoadedModules();
        ObserveOutputPullMaskVulkanInit(handle);
        PrepareVulkanProviderCapabilities(handle);
    }
}

bool TryInstallNgxVulkanAdapterEntryDetours(HMODULE module,
    const std::wstring& path, uint64_t generation)
{
    if (UseAmpere())
    {
        return false;
    }
    if (!module || !ExportsNgxVulkanRoute(module))
        return false;
    struct AdapterEntry
    {
        const char* name;
        entry_detour::Kind kind;
        entry_detour::ForwardPreCall callback;
    };
    const AdapterEntry entries[] = {
        {"NVSDK_NGX_VULKAN_Init",
            entry_detour::Kind::eNgxVulkanAdapterInit,
            &BeforeNgxVulkanInit},
        {"NVSDK_NGX_VULKAN_Init_Ext",
            entry_detour::Kind::eNgxVulkanAdapterInit,
            &BeforeNgxVulkanInit},
        {"NVSDK_NGX_VULKAN_Init_Ext2",
            entry_detour::Kind::eNgxVulkanAdapterInit,
            &BeforeNgxVulkanInit},
        {"NVSDK_NGX_VULKAN_Init_with_ProjectID",
            entry_detour::Kind::eNgxVulkanAdapterInit,
            &BeforeNgxVulkanProjectInit},
        {"NVSDK_NGX_VULKAN_Init_ProjectID",
            entry_detour::Kind::eNgxVulkanAdapterInit,
            &BeforeNgxVulkanProjectInit},
        {"NVSDK_NGX_VULKAN_Init_ProjectID_Ext",
            entry_detour::Kind::eNgxVulkanAdapterInit,
            &BeforeNgxVulkanProjectInit},
    };
    bool installedAny = false;
    for (const AdapterEntry& entry : entries)
    {
        void* target = reinterpret_cast<void*>(
            GetProcAddress(module, entry.name));
        if (!target)
            continue;
        // Some providers alias the base D3D12 and Vulkan Init exports. Their
        // fourth arguments have different meanings, so observe the base
        // Vulkan entry only when it is a distinct implementation. Ext/Ext2
        // and project-ID entries carry an unambiguous VkPhysicalDevice.
        if (std::strcmp(entry.name, "NVSDK_NGX_VULKAN_Init") == 0
            && target == reinterpret_cast<void*>(GetProcAddress(
                module, "NVSDK_NGX_D3D12_Init")))
            continue;
        entry_detour::InstallOptions options{};
        options.generation = generation;
        options.allowRelocated = true;
        void* trampoline = nullptr;
        entry_detour::Handle handle{};
        const bool installed = entry_detour::InstallForwarding(
            entry.kind, module, target, entry.callback, trampoline,
            options, &handle);
        const entry_detour::Snapshot state =
            entry_detour::ReadSnapshot(handle);
        Log(L"NGX Vulkan adapter entry %hs: installed=%d current=%d "
            L"method=%hs failure=%u targetRva=0x%X path=%s",
            entry.name, installed, state.current,
            entry_detour::MethodName(state.method),
            static_cast<uint32_t>(state.failure), state.targetRva,
            path.c_str());
        installedAny = installedAny || installed;
    }
    return installedAny;
}

bool CopySupportedOptions(const sl::DLSSGOptions* source,
    sl::DLSSGOptions& destination, bool preserveNext) noexcept
{
    if (!source)
        return false;
    __try
    {
        if (source->structType != sl::DLSSGOptions::s_structType
            || !IsSupportedOptionsVersion(source->structVersion))
            return false;
        destination = CopyKnownOptions(*source, preserveNext);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool ReadViewportValue(const sl::ViewportHandle* viewport,
    uint32_t& value) noexcept
{
    if (!viewport)
        return false;
    __try
    {
        value = static_cast<uint32_t>(*viewport);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

sl::DLSSGOptions AmpereFixedOptions(const sl::DLSSGOptions& source, bool preserveNext)
{
    return BuildAdjustedOptions(source, ReadControlSnapshot(), preserveNext);
}


sl::Result PassPublicSet(ControlRouteRecord& route,
    const sl::ViewportHandle& viewport,
    const sl::DLSSGOptions& options) noexcept
{
    auto* original = EntryOriginal(
        route.publicSetOriginal, route.publicSetHandle);
    if (!original)
        return sl::Result::eErrorNotInitialized;
    ScopedInternalControlBypass bypass;
    if (UseAmpere())
    {
        sl::DLSSGOptions known{};
        const sl::DLSSGOptions* found = nullptr;
        size_t version = 0;
        if (FindStructBounded(&options, found, version) != ChainFindStatus::eFound || found != &options)
            return sl::Result::eErrorInvalidParameter;
        if (!CopySupportedOptions(&options, known, true)) return sl::Result::eErrorInvalidParameter;
        const sl::DLSSGOptions fixed = AmpereFixedOptions(known, true);
        return original(viewport, fixed);
    }
    else
    {
        return original(viewport, options);
    }
}

sl::Result HandlePublicSetOptions(uint32_t routeSlot,
    ControlEntryPath path, const sl::ViewportHandle& viewport,
    const sl::DLSSGOptions& options)
{
    ControlRouteRecord* route = ControlRouteAt(routeSlot);
    if (!route || !EntryOriginal(
            route->publicSetOriginal, route->publicSetHandle))
        return sl::Result::eErrorNotInitialized;

    BaseStructureFields fields{};
    if (!ReadBaseStructureFields(&options, fields)
        || fields.type != sl::DLSSGOptions::s_structType)
    {
        InvalidateControlRoute(routeSlot,
            UniversalRouteFailure::eMalformedStructureChain);
        return PassPublicSet(*route, viewport, options);
    }
    if (!IsSupportedOptionsVersion(fields.version))
    {
        InvalidateControlRoute(routeSlot,
            UniversalRouteFailure::eUnknownOptionsVersion);
        return PassPublicSet(*route, viewport, options);
    }
    if (!ActivateControlRoute(routeSlot, path, true))
        return PassPublicSet(*route, viewport, options);

    std::lock_guard callLock(gStreamlineCallMutex);
    gSetOptionsCalls.fetch_add(1, std::memory_order_relaxed);
    if (path == ControlEntryPath::eResolver)
    {
        gSetOptionsResolverFallbackCalls.fetch_add(
            1, std::memory_order_release);
    }

    const bool enabled = options.mode == sl::DLSSGMode::eOn
        || options.mode == sl::DLSSGMode::eAuto
        || options.mode == sl::DLSSGMode::eDynamic;
    RecordGameFrameGenerationIntent(viewport, enabled);
    if (!enabled)
    {
        gSetOptionsSeen.store(true, std::memory_order_release);
        const sl::Result result = PassPublicSet(*route, viewport, options);
        gLastSetOptionsResult.store(static_cast<int32_t>(result),
            std::memory_order_relaxed);
        RecordSetOptionsLifecycle(*route, viewport, false, result);
        return HostControlResult(result);
    }

    CaptureGameOptions(viewport, options);
    if (!gControlReady.load(std::memory_order_acquire)
        || !BridgeReady())
    {
        const sl::Result result = PassPublicSet(*route, viewport, options);
        gSetOptionsSeen.store(true, std::memory_order_release);
        gLastSetOptionsResult.store(static_cast<int32_t>(result),
            std::memory_order_relaxed);
        RecordSetOptionsLifecycle(*route, viewport, true, result);
        if (route->wrapperPatched && IsAcceptedControlResult(result)
            && !gFrameGenerationCreateObserved.load(
                std::memory_order_acquire)
            && (!UseAmpere() || !ampere_backend::EarlyInitObserved())
            )
        {
            gPipelineMayPredateDetour.store(true,
                std::memory_order_release);
            gRestartRequired.store(true, std::memory_order_release);
        }
        if (!IsAcceptedControlResult(result) || !BridgeReady())
            return HostControlResult(result);
    }

    return SubmitAdjustedOptions(*route, viewport, options,
        ReadControlSnapshot(), false);
}

sl::Result HandlePublicGetState(uint32_t routeSlot, ControlEntryPath path,
    const sl::ViewportHandle& viewport, sl::DLSSGState& state,
    const sl::DLSSGOptions* options)
{
    ControlRouteRecord* route = ControlRouteAt(routeSlot);
    if (!route)
        return sl::Result::eErrorNotInitialized;
    auto* original = EntryOriginal(
        route->publicGetOriginal, route->publicGetHandle);
    if (!original)
        return sl::Result::eErrorNotInitialized;

    const auto passThrough = [&](bool record = true) {
        if (UseAmpere())
        {
            const sl::DLSSGState* known = nullptr;
            size_t version = 0;
            if (FindStructBounded(&state, known, version) != ChainFindStatus::eFound
                || !IsSupportedStateVersion(version)) return sl::Result::eErrorInvalidParameter;
            sl::DLSSGOptions query{};
            if (options)
            {
                const sl::DLSSGOptions* found = nullptr;
                if (FindStructBounded(options, found, version) != ChainFindStatus::eFound
                    || !CopySupportedOptions(options, query, true)) return sl::Result::eErrorInvalidParameter;
                query = AmpereFixedOptions(query, true);
            }
            const auto result = original(viewport, state, options ? &query : nullptr);
            if (DlssgStateAvailable(result) && state.structVersion >= sl::kStructVersion2)
            {
                state.numFramesToGenerateMax = std::min(state.numFramesToGenerateMax, ampere_policy::kMaximumGeneratedFrames);
            }
            if (record) RecordDlssgStateResult(result, state);
            return result;
        }
        else
        {
            return original(viewport, state, options);
        }
    };

    BaseStructureFields stateFields{};
    if (!ReadBaseStructureFields(&state, stateFields)
        || stateFields.type != sl::DLSSGState::s_structType)
    {
        InvalidateControlRoute(routeSlot,
            UniversalRouteFailure::eMalformedStructureChain);
        ScopedInternalControlBypass bypass;
        return passThrough();
    }
    if (!IsSupportedStateVersion(stateFields.version))
    {
        InvalidateControlRoute(routeSlot,
            UniversalRouteFailure::eUnknownStateVersion);
        ScopedInternalControlBypass bypass;
        return passThrough();
    }
    if (options)
    {
        BaseStructureFields optionFields{};
        if (!ReadBaseStructureFields(options, optionFields)
            || optionFields.type != sl::DLSSGOptions::s_structType)
        {
            InvalidateControlRoute(routeSlot,
                UniversalRouteFailure::eMalformedStructureChain);
            ScopedInternalControlBypass bypass;
            return passThrough();
        }
        if (!IsSupportedOptionsVersion(optionFields.version))
        {
            InvalidateControlRoute(routeSlot,
                UniversalRouteFailure::eUnknownOptionsVersion);
            ScopedInternalControlBypass bypass;
            return passThrough();
        }
    }
    if (!ActivateControlRoute(routeSlot, path, false))
    {
        ScopedInternalControlBypass bypass;
        return passThrough();
    }
    std::lock_guard callLock(gStreamlineCallMutex);
    if (path == ControlEntryPath::eResolver)
    {
        gGetStateResolverFallbackCalls.fetch_add(
            1, std::memory_order_release);
    }
    ReapplyPendingControl(viewport);
    ScopedInternalControlBypass bypass;
    const sl::Result result = passThrough(false);
    RecordDlssgStateResult(result, state);
    return HostControlResult(result);
}

sl::Result HandleInternalSetData(uint32_t routeSlot,
    const sl::BaseStructure* inputs, sl::CommandBuffer* commandBuffer)
{
    ControlRouteRecord* route = ControlRouteAt(routeSlot);
    if (!route)
        return sl::Result::eErrorNotInitialized;
    auto* original = EntryOriginal(
        route->internalSetOriginal, route->internalSetHandle);
    if (!original)
        return sl::Result::eErrorNotInitialized;
    if (InternalControlBypassDepth() != 0)
        return original(inputs, commandBuffer);
    const auto passThrough = [&]() {
        ScopedInternalControlBypass bypass;
        if (UseAmpere())
        {
            const sl::DLSSGOptions* requested = nullptr;
            const sl::ViewportHandle* selectedViewport = nullptr;
            size_t optionsVersion = 0, viewportVersion = 0;
            const auto shape = FindStructBounded(inputs, requested, optionsVersion);
            if (shape == ChainFindStatus::eMalformed) return sl::Result::eErrorInvalidParameter;
            if (shape == ChainFindStatus::eFound)
            {
                sl::DLSSGOptions known{};
                uint32_t viewportValue = 0;
                if (!CopySupportedOptions(requested, known, false)
                    || FindStructBounded(inputs, selectedViewport, viewportVersion) != ChainFindStatus::eFound
                    || !ReadViewportValue(selectedViewport, viewportValue)) return sl::Result::eErrorInvalidParameter;
                sl::DLSSGOptions fixed = AmpereFixedOptions(known, false);
                sl::ViewportHandle viewportCopy{viewportValue};
                viewportCopy.next = &fixed;
                fixed.next = const_cast<sl::BaseStructure*>(inputs);
                return original(&viewportCopy, commandBuffer);
            }
        }
        return original(inputs, commandBuffer);
    };

    const sl::DLSSGOptions* options = nullptr;
    const sl::ViewportHandle* viewport = nullptr;
    size_t optionsVersion = 0;
    size_t viewportVersion = 0;
    const ChainFindStatus optionsStatus = FindStructBounded(
        inputs, options, optionsVersion);
    const ChainFindStatus viewportStatus = FindStructBounded(
        inputs, viewport, viewportVersion);
    const universal_route_policy::AdapterAction action =
        universal_route_policy::ClassifyInternalSet(
            optionsStatus, viewportStatus, optionsVersion);
    if (action == universal_route_policy::AdapterAction::ePassThrough)
        return passThrough();
    if (action == universal_route_policy::AdapterAction::eRejectMalformed)
    {
        InvalidateControlRoute(routeSlot,
            UniversalRouteFailure::eMalformedStructureChain);
        return passThrough();
    }
    if (action
        == universal_route_policy::AdapterAction::eRejectUnknownOptions)
    {
        InvalidateControlRoute(routeSlot,
            UniversalRouteFailure::eUnknownOptionsVersion);
        return passThrough();
    }
    sl::DLSSGOptions source{};
    uint32_t viewportValue = 0;
    if (!CopySupportedOptions(options, source, false)
        || !ReadViewportValue(viewport, viewportValue))
    {
        InvalidateControlRoute(routeSlot,
            UniversalRouteFailure::eMalformedStructureChain);
        return passThrough();
    }
    if (!ActivateControlRoute(
            routeSlot, ControlEntryPath::eInternal, true))
        return passThrough();
    std::lock_guard callLock(gStreamlineCallMutex);
    gSetOptionsCalls.fetch_add(1, std::memory_order_relaxed);
    const sl::ViewportHandle publicViewport{viewportValue};
    const bool enabled = source.mode == sl::DLSSGMode::eOn
        || source.mode == sl::DLSSGMode::eAuto
        || source.mode == sl::DLSSGMode::eDynamic;
    RecordGameFrameGenerationIntent(publicViewport, enabled);
    if (!enabled)
    {
        gSetOptionsSeen.store(true, std::memory_order_release);
        const sl::Result result = passThrough();
        gLastSetOptionsResult.store(static_cast<int32_t>(result),
            std::memory_order_relaxed);
        RecordSetOptionsLifecycle(*route, publicViewport, false, result);
        return HostControlResult(result);
    }

    CaptureGameOptions(publicViewport, source);
    if (!gControlReady.load(std::memory_order_acquire)
        || !BridgeReady())
    {
        const sl::Result result = passThrough();
        gSetOptionsSeen.store(true, std::memory_order_release);
        gLastSetOptionsResult.store(static_cast<int32_t>(result),
            std::memory_order_relaxed);
        RecordSetOptionsLifecycle(*route, publicViewport, true, result);
        if (!IsAcceptedControlResult(result) || !BridgeReady())
            return HostControlResult(result);
    }

    auto submit = [&](const sl::DLSSGOptions& adjustedSource) {
        sl::ViewportHandle viewportCopy{viewportValue};
        sl::DLSSGOptions adjusted = CopyKnownOptions(
            adjustedSource, false);
        viewportCopy.next = &adjusted;
        adjusted.next = const_cast<sl::BaseStructure*>(inputs);
        ScopedInternalControlBypass bypass;
        return original(&viewportCopy, commandBuffer);
    };
    auto query = [&](sl::DLSSGState& state,
                         const sl::DLSSGOptions* queryOptions) {
        return CallRouteGetState(
            *route, publicViewport, state, queryOptions);
    };
    return SubmitAdjustedOptionsImpl(*route, publicViewport, source,
        ReadControlSnapshot(), false, submit, query,
        RouteHasStateFunction(*route));
}

sl::Result HandleInternalGetData(uint32_t routeSlot,
    const sl::BaseStructure* inputs, sl::BaseStructure* outputs,
    sl::CommandBuffer* commandBuffer)
{
    ControlRouteRecord* route = ControlRouteAt(routeSlot);
    if (!route)
        return sl::Result::eErrorNotInitialized;
    auto* original = EntryOriginal(
        route->internalGetOriginal, route->internalGetHandle);
    if (!original)
        return sl::Result::eErrorNotInitialized;
    if (InternalControlBypassDepth() != 0)
        return original(inputs, outputs, commandBuffer);
    const auto passThrough = [&](bool record = true) {
        ScopedInternalControlBypass bypass;
        if (UseAmpere())
        {
            const sl::DLSSGState* known = nullptr;
            size_t version = 0;
            const auto shape = FindStructBounded(outputs, known, version);
            if (shape == ChainFindStatus::eMalformed || (shape == ChainFindStatus::eFound
                && !IsSupportedStateVersion(version))) return sl::Result::eErrorInvalidParameter;
            const auto result = original(inputs, outputs, commandBuffer);
            if (known)
            {
                auto& state = *const_cast<sl::DLSSGState*>(known);
                if (DlssgStateAvailable(result) && state.structVersion >= sl::kStructVersion2)
                    state.numFramesToGenerateMax = std::min(state.numFramesToGenerateMax, ampere_policy::kMaximumGeneratedFrames);
                if (record) RecordDlssgStateResult(result, state);
            }
            return result;
        }
        else
        {
            return original(inputs, outputs, commandBuffer);
        }
    };

    const sl::DLSSGState* stateView = nullptr;
    const sl::DLSSGOptions* optionsView = nullptr;
    const sl::ViewportHandle* viewport = nullptr;
    size_t stateVersion = 0;
    size_t viewportVersion = 0;
    const ChainFindStatus stateStatus = FindStructBounded(
        outputs, stateView, stateVersion);
    const ChainFindStatus viewportStatus = FindStructBounded(
        inputs, viewport, viewportVersion);
    size_t optionsVersion = 0;
    const ChainFindStatus optionsStatus = FindStructBounded(
        inputs, optionsView, optionsVersion);
    const universal_route_policy::AdapterAction action =
        universal_route_policy::ClassifyInternalGet(
            stateStatus, viewportStatus, stateVersion,
            optionsStatus, optionsVersion);
    if (action == universal_route_policy::AdapterAction::ePassThrough)
        return passThrough();
    if (action == universal_route_policy::AdapterAction::eRejectMalformed)
    {
        InvalidateControlRoute(routeSlot,
            UniversalRouteFailure::eMalformedStructureChain);
        return passThrough();
    }
    if (action
        == universal_route_policy::AdapterAction::eRejectUnknownState)
    {
        InvalidateControlRoute(routeSlot,
            UniversalRouteFailure::eUnknownStateVersion);
        return passThrough();
    }
    if (action
        == universal_route_policy::AdapterAction::eRejectUnknownOptions)
    {
        InvalidateControlRoute(routeSlot,
            UniversalRouteFailure::eUnknownOptionsVersion);
        return passThrough();
    }
    uint32_t viewportValue = 0;
    if (!ReadViewportValue(viewport, viewportValue))
    {
        InvalidateControlRoute(routeSlot,
            UniversalRouteFailure::eMalformedStructureChain);
        return passThrough();
    }
    if (!ActivateControlRoute(
            routeSlot, ControlEntryPath::eInternal, false))
        return passThrough();
    std::lock_guard callLock(gStreamlineCallMutex);
    const sl::ViewportHandle publicViewport{viewportValue};
    ReapplyPendingControl(publicViewport);
    const sl::Result result = passThrough(false);
    auto& mutableState = *const_cast<sl::DLSSGState*>(stateView);
    RecordDlssgStateResult(result, mutableState);
    return HostControlResult(result);
}

sl::Result HandleFreeResources(uint32_t routeSlot, sl::Feature feature,
    const sl::ViewportHandle& viewport)
{
    ControlRouteRecord* route = ControlRouteAt(routeSlot);
    if (!route)
        return sl::Result::eErrorNotInitialized;
    auto* original = EntryOriginal(
        route->freeResourcesOriginal, route->freeResourcesHandle);
    if (!original)
        return sl::Result::eErrorNotInitialized;
    AcceptedOffEvidence expected{};
    if (feature == sl::kFeatureDLSS_G)
    {
        std::lock_guard lifetimeLock(gFrameGenerationLifetimeMutex);
        if (gAcceptedOffEvidence.routeSlot == routeSlot
            && gAcceptedOffEvidence.viewport
                == static_cast<uint32_t>(viewport))
            expected = gAcceptedOffEvidence;
    }
    const sl::Result result = original(feature, viewport);
    route->releaseCalls.fetch_add(1, std::memory_order_relaxed);
    if (feature == sl::kFeatureDLSS_G && IsAcceptedControlResult(result))
    {
        InvalidateUiInputEvidence(static_cast<uint32_t>(viewport));
        if (static_cast<uint32_t>(viewport)
            == gLastOptionsViewport.load(std::memory_order_acquire))
        {
            gAppliedUiRecompositionEnabled.store(false, std::memory_order_release);
            gAppliedUiRecompositionForced.store(false, std::memory_order_release);
        }
    }
    if (expected.routeSlot != UINT32_MAX
        && IsAcceptedControlResult(result))
    {
        if (!expected.feature)
        {
            // Ada exposes no exact post-ReleaseFeature identity. The matching
            // accepted free is therefore a one-way safety boundary even if an
            // On call raced with the native free: do not reapply to a possibly
            // released lifetime from polling.
            gAppliedRevision.store(0, std::memory_order_release);
            gAttemptedRevision.store(0, std::memory_order_release);
            gRestartRequired.store(true, std::memory_order_release);
            Log(L"Matching Ada DLSS-G resources were freed after accepted "
                L"Off; exact feature recreation is unavailable, restart required");
            return result;
        }
        std::lock_guard lifetimeLock(gFrameGenerationLifetimeMutex);
        if (SameRetainedFeature(expected.feature,
                gRetainedFrameGenerationFeature)
            && !route->releaseObserved.load(std::memory_order_acquire))
        {
            // slFreeResources success is not itself proof that this exact NGX
            // feature reached ReleaseFeature.  A matching post-call observer
            // may already have retired it from inside the native call; absent
            // that evidence, keep ownership and require a known recreation.
            gRestartRequired.store(true, std::memory_order_release);
            Log(L"Matching DLSS-G resources were freed without an exact "
                L"ReleaseFeature boundary; retained feature %p remains "
                L"restricted until restart/recreation",
                reinterpret_cast<void*>(expected.feature.handle));
        }
    }
    return result;
}

template <size_t Slot>
sl::Result PublicSetThunk(const sl::ViewportHandle& viewport,
    const sl::DLSSGOptions& options)
{
    ControlRouteRecord* route = ControlRouteAt(static_cast<uint32_t>(Slot));
    const ControlEntryPath path = route
            && route->publicSetResolverFallback.load(
                std::memory_order_acquire)
        ? ControlEntryPath::eResolver : ControlEntryPath::ePublic;
    return HandlePublicSetOptions(static_cast<uint32_t>(Slot),
        path, viewport, options);
}

template <size_t Slot>
sl::Result PublicGetThunk(const sl::ViewportHandle& viewport,
    sl::DLSSGState& state, const sl::DLSSGOptions* options)
{
    ControlRouteRecord* route = ControlRouteAt(static_cast<uint32_t>(Slot));
    const ControlEntryPath path = route
            && route->publicGetResolverFallback.load(
                std::memory_order_acquire)
        ? ControlEntryPath::eResolver : ControlEntryPath::ePublic;
    return HandlePublicGetState(static_cast<uint32_t>(Slot),
        path, viewport, state, options);
}

template <size_t Slot>
sl::Result InternalSetThunk(const sl::BaseStructure* inputs,
    sl::CommandBuffer* commandBuffer)
{
    return HandleInternalSetData(
        static_cast<uint32_t>(Slot), inputs, commandBuffer);
}

template <size_t Slot>
sl::Result InternalGetThunk(const sl::BaseStructure* inputs,
    sl::BaseStructure* outputs, sl::CommandBuffer* commandBuffer)
{
    return HandleInternalGetData(static_cast<uint32_t>(Slot),
        inputs, outputs, commandBuffer);
}

template <size_t Slot>
sl::Result FreeResourcesThunk(sl::Feature feature,
    const sl::ViewportHandle& viewport)
{
    return HandleFreeResources(static_cast<uint32_t>(Slot),
        feature, viewport);
}

template <size_t... Slots>
constexpr auto MakePublicSetThunks(std::index_sequence<Slots...>)
{
    return std::array<PFun_slDLSSGSetOptions*, sizeof...(Slots)>{
        &PublicSetThunk<Slots>...};
}

template <size_t... Slots>
constexpr auto MakePublicGetThunks(std::index_sequence<Slots...>)
{
    return std::array<PFun_slDLSSGGetState*, sizeof...(Slots)>{
        &PublicGetThunk<Slots>...};
}

template <size_t... Slots>
constexpr auto MakeInternalSetThunks(std::index_sequence<Slots...>)
{
    return std::array<PFun_slSetDataInternal*, sizeof...(Slots)>{
        &InternalSetThunk<Slots>...};
}

template <size_t... Slots>
constexpr auto MakeInternalGetThunks(std::index_sequence<Slots...>)
{
    return std::array<PFun_slGetDataInternal*, sizeof...(Slots)>{
        &InternalGetThunk<Slots>...};
}

template <size_t... Slots>
constexpr auto MakeFreeResourcesThunks(std::index_sequence<Slots...>)
{
    return std::array<PFun_slFreeResources*, sizeof...(Slots)>{
        &FreeResourcesThunk<Slots>...};
}

constexpr auto gPublicSetThunks = MakePublicSetThunks(
    std::make_index_sequence<kControlRouteCapacity>{});
constexpr auto gPublicGetThunks = MakePublicGetThunks(
    std::make_index_sequence<kControlRouteCapacity>{});
constexpr auto gInternalSetThunks = MakeInternalSetThunks(
    std::make_index_sequence<kControlRouteCapacity>{});
constexpr auto gInternalGetThunks = MakeInternalGetThunks(
    std::make_index_sequence<kControlRouteCapacity>{});
constexpr auto gFreeResourcesThunks = MakeFreeResourcesThunks(
    std::make_index_sequence<kControlRouteCapacity>{});

uint32_t EnsureControlRoute(HMODULE wrapper, const std::wstring& path,
    uint64_t generation, bool wrapperPatched,
    uint32_t compiledMaximumGeneratedFrames)
{
    if (!wrapper)
        return UINT32_MAX;
    std::lock_guard lock(gControlRouteMutex);
    for (uint32_t slot = 0; slot < gControlRoutes.size(); ++slot)
    {
        ControlRouteRecord& route = gControlRoutes[slot];
        if (route.claimed.load(std::memory_order_acquire)
            && route.wrapper == wrapper && route.generation == generation)
        {
            route.compiledMaximumGeneratedFrames.store(compiledMaximumGeneratedFrames, std::memory_order_release);
            route.wrapperPatched.store(wrapperPatched, std::memory_order_release);
            return slot;
        }
    }
    for (uint32_t slot = 0; slot < gControlRoutes.size(); ++slot)
    {
        ControlRouteRecord& route = gControlRoutes[slot];
        if (route.claimed.load(std::memory_order_acquire))
            continue;
        route.wrapper = wrapper;
        route.generation = generation;
        route.path = path;
        route.version = ReadFileVersion(path);
        route.wrapperPatched = wrapperPatched;
        route.compiledMaximumGeneratedFrames =
            compiledMaximumGeneratedFrames;
        route.claimed.store(true, std::memory_order_release);
        return slot;
    }
    return UINT32_MAX;
}

bool InstallControlRouteEntries(uint32_t routeSlot)
{
    ControlRouteRecord* route = ControlRouteAt(routeSlot);
    if (!route || !route->wrapperPatched)
        return false;
    using GetPluginFunctionFn = void* (*)(const char*);
    auto* resolver = reinterpret_cast<GetPluginFunctionFn>(
        GetProcAddress(route->wrapper, "slGetPluginFunction"));
    if (!resolver)
        return false;

    void* publicSet = resolver("slDLSSGSetOptions");
    void* publicGet = resolver("slDLSSGGetState");
    entry_detour::InstallOptions relocated{};
    relocated.generation = route->generation;
    relocated.allowRelocated = true;
    entry_detour::InstallOptions hotpatch{};
    hotpatch.generation = route->generation;

    // Public DLSS-G entries are the narrowest ABI boundary and cover pointers
    // which the host cached before this module was discovered. Do not relocate
    // the generic internal or lifecycle entries on every merely discovered
    // wrapper: those functions also participate in plugin startup. They are
    // installed only as a missing-public-entry fallback, while lifecycle is
    // delayed until a real call selects this wrapper.
    void* trampoline = nullptr;
    bool publicSetCurrent = ControlEntryCurrent(route->publicSetHandle);
    if (!publicSetCurrent && publicSet)
    {
        HMODULE owner = ModuleFromAddress(publicSet);
        if (owner)
        {
            publicSetCurrent = entry_detour::Install(
                entry_detour::Kind::eDlssgSetOptions, owner, publicSet,
                reinterpret_cast<void*>(gPublicSetThunks[routeSlot]),
                trampoline, hotpatch, &route->publicSetHandle);
            if (!publicSetCurrent)
            {
                publicSetCurrent = entry_detour::Install(
                    entry_detour::Kind::eDlssgSetOptions, owner, publicSet,
                    reinterpret_cast<void*>(gPublicSetThunks[routeSlot]),
                    trampoline, relocated, &route->publicSetHandle);
            }
            if (publicSetCurrent)
            {
                route->publicSetResolverFallback.store(false,
                    std::memory_order_release);
                route->publicSetOriginal.store(
                    reinterpret_cast<PFun_slDLSSGSetOptions*>(trampoline),
                    std::memory_order_release);
                gSetOptionsHookExposed.store(true,
                    std::memory_order_release);
            }
        }
    }

    trampoline = nullptr;
    bool publicGetCurrent = ControlEntryCurrent(route->publicGetHandle);
    if (!publicGetCurrent && publicGet)
    {
        HMODULE owner = ModuleFromAddress(publicGet);
        if (owner)
        {
            publicGetCurrent = entry_detour::Install(
                entry_detour::Kind::eDlssgGetState, owner, publicGet,
                reinterpret_cast<void*>(gPublicGetThunks[routeSlot]),
                trampoline, hotpatch, &route->publicGetHandle);
            if (!publicGetCurrent)
            {
                publicGetCurrent = entry_detour::Install(
                    entry_detour::Kind::eDlssgGetState, owner, publicGet,
                    reinterpret_cast<void*>(gPublicGetThunks[routeSlot]),
                    trampoline, relocated, &route->publicGetHandle);
            }
            if (publicGetCurrent)
            {
                route->publicGetResolverFallback.store(false,
                    std::memory_order_release);
                route->publicGetOriginal.store(
                    reinterpret_cast<PFun_slDLSSGGetState*>(trampoline),
                    std::memory_order_release);
                gGetStateHookExposed.store(true,
                    std::memory_order_release);
            }
        }
    }

    const bool setResolverCovered =
        route->publicSetResolverFallback.load(std::memory_order_acquire)
        && route->publicSetOriginal.load(std::memory_order_acquire);
    const bool getResolverCovered =
        route->publicGetResolverFallback.load(std::memory_order_acquire)
        && route->publicGetOriginal.load(std::memory_order_acquire);

    bool internalSetCurrent = ControlEntryCurrent(route->internalSetHandle);
    if (!internalSetCurrent
        && universal_route_policy::NeedsInternalFallback(
            publicSetCurrent || setResolverCovered))
    {
        void* internalSet = resolver("slSetData");
        if (internalSet)
        {
            trampoline = nullptr;
            HMODULE owner = ModuleFromAddress(internalSet);
            internalSetCurrent = owner && entry_detour::Install(
                entry_detour::Kind::eSlSetData, owner, internalSet,
                reinterpret_cast<void*>(gInternalSetThunks[routeSlot]),
                trampoline, relocated, &route->internalSetHandle);
            if (internalSetCurrent)
            {
                route->internalSetOriginal.store(
                    reinterpret_cast<PFun_slSetDataInternal*>(trampoline),
                    std::memory_order_release);
                gSetOptionsHookExposed.store(true,
                    std::memory_order_release);
            }
        }
    }

    bool internalGetCurrent = ControlEntryCurrent(route->internalGetHandle);
    if (!internalGetCurrent
        && universal_route_policy::NeedsInternalFallback(
            publicGetCurrent || getResolverCovered))
    {
        void* internalGet = resolver("slGetData");
        if (internalGet)
        {
            trampoline = nullptr;
            HMODULE owner = ModuleFromAddress(internalGet);
            internalGetCurrent = owner && entry_detour::Install(
                entry_detour::Kind::eSlGetData, owner, internalGet,
                reinterpret_cast<void*>(gInternalGetThunks[routeSlot]),
                trampoline, relocated, &route->internalGetHandle);
            if (internalGetCurrent)
            {
                route->internalGetOriginal.store(
                    reinterpret_cast<PFun_slGetDataInternal*>(trampoline),
                    std::memory_order_release);
                gGetStateHookExposed.store(true,
                    std::memory_order_release);
            }
        }
    }

    const bool activeRoute =
        gActiveControlRouteSlot.load(std::memory_order_acquire) == routeSlot;
    if (universal_route_policy::CanInstallLifecycleEntry(activeRoute))
        InstallControlRouteLifecycleEntry(routeSlot);

    const entry_detour::Snapshot publicSetState =
        entry_detour::ReadSnapshot(route->publicSetHandle);
    const entry_detour::Snapshot publicGetState =
        entry_detour::ReadSnapshot(route->publicGetHandle);
    const entry_detour::Snapshot internalSetState =
        entry_detour::ReadSnapshot(route->internalSetHandle);
    const entry_detour::Snapshot internalGetState =
        entry_detour::ReadSnapshot(route->internalGetHandle);
    const entry_detour::Snapshot releaseState =
        entry_detour::ReadSnapshot(route->freeResourcesHandle);
    Log(L"DLSS-G wrapper route generation=%llu public=%d/%d (%hs/%hs) "
        L"internalFallback=%d/%d (%hs/%hs) release=%d (%hs) active=%d "
        L"path=%s",
        static_cast<unsigned long long>(route->generation),
        publicSetCurrent, publicGetCurrent,
        entry_detour::MethodName(publicSetState.method),
        entry_detour::MethodName(publicGetState.method),
        internalSetCurrent, internalGetCurrent,
        entry_detour::MethodName(internalSetState.method),
        entry_detour::MethodName(internalGetState.method),
        releaseState.current, entry_detour::MethodName(releaseState.method),
        activeRoute, route->path.c_str());
    return (publicSetCurrent || internalSetCurrent || setResolverCovered)
        && (publicGetCurrent || internalGetCurrent || getResolverCovered);
}

bool InstallControlRouteLifecycleEntry(uint32_t routeSlot)
{
    ControlRouteRecord* route = ControlRouteAt(routeSlot);
    if (!route
        || !route->wrapperPatched
        || !route->structureCompatible.load(std::memory_order_acquire)
        || !universal_route_policy::CanInstallLifecycleEntry(
            gActiveControlRouteSlot.load(std::memory_order_acquire)
                == routeSlot))
        return false;
    if (ControlEntryCurrent(route->freeResourcesHandle))
        return true;

    bool expected = false;
    if (!route->lifecycleInstallAttempted.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel,
            std::memory_order_acquire))
        return ControlEntryCurrent(route->freeResourcesHandle);

    using GetPluginFunctionFn = void* (*)(const char*);
    auto* resolver = reinterpret_cast<GetPluginFunctionFn>(
        GetProcAddress(route->wrapper, "slGetPluginFunction"));
    void* freeResources = resolver ? resolver("slFreeResources") : nullptr;
    entry_detour::InstallOptions relocated{};
    relocated.generation = route->generation;
    relocated.allowRelocated = true;
    void* trampoline = nullptr;
    bool freeResourcesCurrent = false;
    if (freeResources)
    {
        HMODULE owner = ModuleFromAddress(freeResources);
        freeResourcesCurrent = owner && entry_detour::Install(
            entry_detour::Kind::eSlFreeResources, owner, freeResources,
            reinterpret_cast<void*>(gFreeResourcesThunks[routeSlot]),
            trampoline, relocated, &route->freeResourcesHandle);
        if (freeResourcesCurrent)
        {
            route->freeResourcesOriginal.store(
                reinterpret_cast<PFun_slFreeResources*>(trampoline),
                std::memory_order_release);
        }
    }
    const entry_detour::Snapshot releaseState =
        entry_detour::ReadSnapshot(route->freeResourcesHandle);
    Log(L"DLSS-G lifecycle route generation=%llu installed=%d current=%d "
        L"method=%hs failure=%u path=%s",
        static_cast<unsigned long long>(route->generation),
        freeResourcesCurrent, releaseState.current,
        entry_detour::MethodName(releaseState.method),
        static_cast<uint32_t>(releaseState.failure), route->path.c_str());
    return freeResourcesCurrent;
}

uint32_t FindControlRouteForTarget(void* target) noexcept
{
    HMODULE owner = ModuleFromAddress(target);
    if (!owner)
        return UINT32_MAX;
    for (uint32_t slot = 0; slot < gControlRoutes.size(); ++slot)
    {
        ControlRouteRecord* route = ControlRouteAt(slot);
        if (!route)
            continue;
        for (entry_detour::Handle handle : {route->publicSetHandle,
                 route->publicGetHandle, route->internalSetHandle,
                 route->internalGetHandle, route->freeResourcesHandle})
        {
            const entry_detour::Snapshot state =
                entry_detour::ReadSnapshot(handle);
            if (state.owner == owner || state.target == target)
                return slot;
        }
        if (route->wrapper == owner)
            return slot;
    }
    return UINT32_MAX;
}

bool TryInstallSetOptionsEntryDetour(HMODULE wrapper, void*)
{
    for (uint32_t slot = 0; slot < gControlRoutes.size(); ++slot)
    {
        ControlRouteRecord* route = ControlRouteAt(slot);
        if (route && route->wrapper == wrapper)
            return InstallControlRouteEntries(slot)
                && (ControlEntryCurrent(route->publicSetHandle)
                    || ControlEntryCurrent(route->internalSetHandle));
    }
    return false;
}

bool TryInstallGetStateEntryDetour(HMODULE wrapper, void*)
{
    for (uint32_t slot = 0; slot < gControlRoutes.size(); ++slot)
    {
        ControlRouteRecord* route = ControlRouteAt(slot);
        if (route && route->wrapper == wrapper)
            return InstallControlRouteEntries(slot)
                && (ControlEntryCurrent(route->publicGetHandle)
                    || ControlEntryCurrent(route->internalGetHandle));
    }
    return false;
}

// Compatibility entry points are retained for binaries which resolve the
// symbols directly. Resolver fallbacks below always publish a typed route thunk.
sl::Result HookSlDLSSGSetOptions(
    const sl::ViewportHandle& viewport, const sl::DLSSGOptions& options)
{
    ControlRouteRecord* route = ActiveControlRoute();
    return route ? HandlePublicSetOptions(
        gActiveControlRouteSlot.load(std::memory_order_acquire),
        ControlEntryPath::eResolver, viewport, options)
        : sl::Result::eErrorNotInitialized;
}

sl::Result HookSlDLSSGGetState(const sl::ViewportHandle& viewport,
    sl::DLSSGState& state, const sl::DLSSGOptions* options)
{
    ControlRouteRecord* route = ActiveControlRoute();
    return route ? HandlePublicGetState(
        gActiveControlRouteSlot.load(std::memory_order_acquire),
        ControlEntryPath::eResolver, viewport, state, options)
        : sl::Result::eErrorNotInitialized;
}

uint32_t ResolvedDlssgControlRoute(void* resolved)
{
    HMODULE owner = ModuleFromAddress(resolved);
    MEMORY_BASIC_INFORMATION memory{};
    if (!owner || !VirtualQuery(resolved, &memory, sizeof(memory))
        || memory.AllocationBase != owner || memory.State != MEM_COMMIT
        || memory.Type != MEM_IMAGE || (memory.Protect & PAGE_GUARD)
        || !(memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ
            | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
        || !GetProcAddress(owner, "slGetPluginFunction"))
        return UINT32_MAX;
    const uint32_t slot = FindControlRouteForTarget(resolved);
    if (slot != UINT32_MAX)
        return slot;

    // The successful, feature-specific lookup proves callable identity. Its
    // owner must remain loaded for every published typed forwarding pointer.
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(resolved), &pinned) || pinned != owner)
        return UINT32_MAX;
    const ModuleRecord discovered = InspectLoadedModule(owner, LoadedModulePath(owner));
    if (discovered.controlRouteSlot != UINT32_MAX)
        return discovered.controlRouteSlot;
    if (!discovered.wrapperExport || !discovered.generation)
        return UINT32_MAX;
    // Unknown wrapper layouts get resolver forwarding only: no executable
    // entry patch, maximum claim, lifecycle patch, or MFG authorization.
    return EnsureControlRoute(owner, discovered.path, discovered.generation, false, 0);
}

sl::Result HookSlGetFeatureFunction(
    sl::Feature feature, const char* functionName, void*& function)
{
    auto* original = gOriginalGetFeatureFunction.load(std::memory_order_acquire);
    if (!original)
        return sl::Result::eErrorNotInitialized;

    const sl::Result result = original(feature, functionName, function);
    if (result != sl::Result::eOk || feature != sl::kFeatureDLSS_G)
        return result;
    if (function && functionName && strcmp(functionName, "slDLSSGSetOptions") == 0)
    {
        void* const resolved = function;
        const uint32_t routeSlot = ResolvedDlssgControlRoute(resolved);
        ControlRouteRecord* route = ControlRouteAt(routeSlot);
        if (route)
        {
            InstallControlRouteEntries(routeSlot);
            const bool publicEntry =
                ControlEntryCurrent(route->publicSetHandle);
            const bool internalEntry =
                ControlEntryCurrent(route->internalSetHandle);
            if (!publicEntry && !internalEntry
                && resolved != reinterpret_cast<void*>(
                    gPublicSetThunks[routeSlot]))
            {
                auto* candidate = reinterpret_cast<PFun_slDLSSGSetOptions*>(resolved);
                auto* expected = static_cast<PFun_slDLSSGSetOptions*>(nullptr);
                if (!route->publicSetOriginal.compare_exchange_strong(expected, candidate,
                        std::memory_order_acq_rel) && expected != candidate)
                    return result;
                route->publicSetResolverFallback.store(
                    true, std::memory_order_release);
                function = reinterpret_cast<void*>(
                    gPublicSetThunks[routeSlot]);
                gSetOptionsHookExposed.store(true,
                    std::memory_order_release);
                gSetOptionsResolverFallbackActive.store(
                    true, std::memory_order_release);
            }
        }
    }
    else if (function && functionName && strcmp(functionName, "slDLSSGGetState") == 0)
    {
        void* const resolved = function;
        const uint32_t routeSlot = ResolvedDlssgControlRoute(resolved);
        ControlRouteRecord* route = ControlRouteAt(routeSlot);
        if (route)
        {
            InstallControlRouteEntries(routeSlot);
            const bool publicEntry =
                ControlEntryCurrent(route->publicGetHandle);
            const bool internalEntry =
                ControlEntryCurrent(route->internalGetHandle);
            if (!publicEntry && !internalEntry
                && resolved != reinterpret_cast<void*>(
                    gPublicGetThunks[routeSlot]))
            {
                auto* candidate = reinterpret_cast<PFun_slDLSSGGetState*>(resolved);
                auto* expected = static_cast<PFun_slDLSSGGetState*>(nullptr);
                if (!route->publicGetOriginal.compare_exchange_strong(expected, candidate,
                        std::memory_order_acq_rel) && expected != candidate)
                    return result;
                route->publicGetResolverFallback.store(
                    true, std::memory_order_release);
                function = reinterpret_cast<void*>(
                    gPublicGetThunks[routeSlot]);
                gGetStateHookExposed.store(true,
                    std::memory_order_release);
                gGetStateResolverFallbackActive.store(
                    true, std::memory_order_release);
            }
        }
    }
    return result;
}

sl::Result HookSlSetTag(const sl::ViewportHandle& viewport,
    const sl::ResourceTag* tags, uint32_t numTags, sl::CommandBuffer* cmdBuffer)
{
    auto* original = gOriginalSetTag.load(std::memory_order_acquire);
    if (!original)
        return sl::Result::eErrorNotInitialized;
    const auto batch = PrepareUiResourceTags(viewport, tags, numTags, false, 0);
    const sl::Result result = original(viewport, tags, numTags, cmdBuffer);
    gSetTagCalls.fetch_add(1, std::memory_order_relaxed);
    if (result == sl::Result::eOk)
        RecordUiResourceTags(batch);
    return result;
}

sl::Result HookSlSetTagForFrame(const sl::FrameToken& frame,
    const sl::ViewportHandle& viewport, const sl::ResourceTag* tags,
    uint32_t numTags, sl::CommandBuffer* cmdBuffer)
{
    auto* original = gOriginalSetTagForFrame.load(std::memory_order_acquire);
    if (!original)
        return sl::Result::eErrorNotInitialized;
    const auto batch = PrepareUiResourceTags(viewport, tags, numTags, true,
        static_cast<uint32_t>(frame));
    const sl::Result result = original(frame, viewport, tags, numTags, cmdBuffer);
    gSetTagForFrameCalls.fetch_add(1, std::memory_order_relaxed);
    if (result == sl::Result::eOk)
        RecordUiResourceTags(batch);
    return result;
}

void InspectAlreadyLoadedModules();

sl::Result HookSlSetD3DDevice(void* device)
{
    auto* original = gOriginalSetD3DDevice.load(std::memory_order_acquire);
    if (!original)
        return sl::Result::eErrorNotInitialized;
    InvalidateUiInputEvidence();
    if (gpu_backend::ObserveD3D12Device(device))
    {
        gModuleInventoryDirty.store(true, std::memory_order_release);
        if (gpu_dispatch::IsAda())
        {
            dlssg_preset::Prepare();
            temporal_interval_trace::SetEnabled(true);
            InspectAlreadyLoadedModules();
        }
        else if (UseAmpere())
        {
            temporal_interval_trace::SetEnabled(true);
        }
    }
    if (UseAmpere())
    {
        return InvokeAmpereDeviceSetup(original, device);
    }
    else
    {
        return original(device);
    }
}

sl::Result HookSlSetVulkanInfo(const VulkanInfoPrefix& info)
{
    auto* original = gOriginalSetVulkanInfo.load(std::memory_order_acquire);
    if (!original)
        return sl::Result::eErrorNotInitialized;
    InvalidateUiInputEvidence();
    void* physicalDevice = nullptr;
    __try
    {
        if (info.structVersion >= sl::kStructVersion1
            && info.structVersion <= sl::kStructVersion3)
            physicalDevice = info.physicalDevice;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        physicalDevice = nullptr;
    }
    const bool verified = physicalDevice
        && gpu_backend::ObserveVulkanPhysicalDevice(physicalDevice);
    gVulkanAdapterVerified.store(verified, std::memory_order_release);
    if (verified)
    {
        if (gpu_dispatch::IsAda()) dlssg_preset::Prepare();
        gModuleInventoryDirty.store(true, std::memory_order_release);
        InspectAlreadyLoadedModules();
    }
    return original(info);
}

bool HookModuleImport(HMODULE module, const char* importedModule,
    const char* importedFunction, void* replacement, void*& original)
{
    // VirtualProtect applies to an entire page, including neighbouring IAT
    // slots. Serialize discovery writers through protection restoration.
    // No loader calls or external callbacks run while this mutex is held.
    std::lock_guard publicationLock(gImportPublicationMutex);
    auto* base = reinterpret_cast<uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE
        || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return false;

    const auto& importDirectory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!importDirectory.VirtualAddress || !importDirectory.Size)
        return false;

    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        base + importDirectory.VirtualAddress);
    for (; descriptor->Name; ++descriptor)
    {
        const char* moduleName = reinterpret_cast<const char*>(base + descriptor->Name);
        if (_stricmp(moduleName, importedModule) != 0)
            continue;

        const DWORD originalRva = descriptor->OriginalFirstThunk
            ? descriptor->OriginalFirstThunk : descriptor->FirstThunk;
        auto* originalThunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + originalRva);
        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->FirstThunk);
        for (; originalThunk->u1.AddressOfData; ++originalThunk, ++thunk)
        {
            if (IMAGE_SNAP_BY_ORDINAL64(originalThunk->u1.Ordinal))
                continue;
            const auto* import = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                base + originalThunk->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(import->Name), importedFunction) != 0)
                continue;

            auto** slot = reinterpret_cast<void**>(&thunk->u1.Function);
            auto* current = protected_pointer::ReadPointer(
                reinterpret_cast<uintptr_t>(slot));
            if (current == replacement)
                return true;

            MEMORY_BASIC_INFORMATION memory{};
            if (VirtualQuery(slot, &memory, sizeof(memory)) != sizeof(memory)
                || memory.AllocationBase != module || memory.Type != MEM_IMAGE
                || memory.State != MEM_COMMIT)
                return false;
            const auto published = protected_pointer::ReplaceProtectedPointer(
                reinterpret_cast<uintptr_t>(slot),
                reinterpret_cast<uintptr_t>(current),
                reinterpret_cast<uintptr_t>(replacement),
                &VirtualProtect, PAGE_READWRITE);
            if (published.replacementWasPublished)
                original = current;
            return published.disposition
                == protected_pointer::PublishDisposition::ePublishedRestored;
        }
    }
    return false;
}

bool HookMainExecutableImport(const char* importedModule,
    const char* importedFunction, void* replacement, void*& original)
{
    return HookModuleImport(GetModuleHandleW(nullptr), importedModule,
        importedFunction, replacement, original);
}

bool ModuleFileNameEquals(const std::wstring& path,
    const wchar_t* expected)
{
    const size_t separator = path.find_last_of(L"\\/");
    const wchar_t* name = separator == std::wstring::npos
        ? path.c_str() : path.c_str() + separator + 1;
    return expected && _wcsicmp(name, expected) == 0;
}

std::wstring WidePathFromAnsi(const char* path)
{
    if (!path)
        return {};
    const int count = MultiByteToWideChar(
        CP_ACP, 0, path, -1, nullptr, 0);
    if (count <= 1)
        return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    if (!MultiByteToWideChar(CP_ACP, 0, path, -1,
            result.data(), count))
    {
        return {};
    }
    result.pop_back();
    return result;
}

std::string AnsiPathFromWide(const std::wstring& path)
{
    if (path.empty())
        return {};
    const int count = WideCharToMultiByte(
        CP_ACP, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (count <= 1)
        return {};
    std::string result(static_cast<size_t>(count), '\0');
    if (!WideCharToMultiByte(CP_ACP, 0, path.c_str(), -1,
            result.data(), count, nullptr, nullptr))
    {
        return {};
    }
    result.pop_back();
    return result;
}

void RecordSelectiveOtaDlssgWrapperResult(
    const std::wstring& requestedPath, const std::wstring& redirectPath,
    HMODULE module, DWORD redirectError, bool fallback)
{
    gSelectiveOtaDlssgWrapperRedirectAttempts.fetch_add(
        1, std::memory_order_relaxed);
    if (module && !fallback)
    {
        gSelectiveOtaDlssgWrapperRedirectSuccesses.fetch_add(
            1, std::memory_order_relaxed);
        Log(L"Selective DLSS-G wrapper redirect loaded NVIDIA OTA candidate: "
            L"requested=%s selected=%s", requestedPath.c_str(),
            redirectPath.c_str());
    }
    else if (fallback)
    {
        gSelectiveOtaDlssgWrapperFallbacks.fetch_add(
            1, std::memory_order_relaxed);
        Log(L"Selective DLSS-G wrapper redirect failed (%lu); "
            L"falling back to game wrapper: requested=%s selected=%s",
            redirectError, requestedPath.c_str(), redirectPath.c_str());
    }
}

void InspectStreamlineLoadedModule(HMODULE module)
{
    if (!module)
        return;
    gStreamlineLoaderDiscoveryCalls.fetch_add(1, std::memory_order_relaxed);
    InspectLoadedModule(module, LoadedModulePath(module));
}

HMODULE WINAPI HookStreamlineLoadLibraryA(LPCSTR fileName)
{
    auto* original = gOriginalStreamlineLoadLibraryA.load(
        std::memory_order_acquire);
    if (!original || original == &HookStreamlineLoadLibraryA)
        return nullptr;
    const std::wstring requested = WidePathFromAnsi(fileName);
    const std::wstring redirect =
        SelectiveOtaDlssgWrapperRedirectPath(requested);
    HMODULE module = nullptr;
    if (!redirect.empty())
    {
        const std::string redirected = AnsiPathFromWide(redirect);
        if (!redirected.empty())
            module = original(redirected.c_str());
        const DWORD redirectError = module ? ERROR_SUCCESS : GetLastError();
        if (!module)
            module = original(fileName);
        RecordSelectiveOtaDlssgWrapperResult(requested, redirect, module,
            redirectError, redirectError != ERROR_SUCCESS);
    }
    else
    {
        module = original(fileName);
    }
    InspectStreamlineLoadedModule(module);
    return module;
}

HMODULE WINAPI HookStreamlineLoadLibraryW(LPCWSTR fileName)
{
    auto* original = gOriginalStreamlineLoadLibraryW.load(
        std::memory_order_acquire);
    if (!original || original == &HookStreamlineLoadLibraryW)
        return nullptr;
    const std::wstring requested = fileName ? fileName : L"";
    const std::wstring redirect =
        SelectiveOtaDlssgWrapperRedirectPath(requested);
    HMODULE module = redirect.empty() ? original(fileName)
        : original(redirect.c_str());
    if (!redirect.empty())
    {
        const DWORD redirectError = module ? ERROR_SUCCESS : GetLastError();
        if (!module)
            module = original(fileName);
        RecordSelectiveOtaDlssgWrapperResult(requested, redirect, module,
            redirectError, redirectError != ERROR_SUCCESS);
    }
    InspectStreamlineLoadedModule(module);
    return module;
}

HMODULE WINAPI HookStreamlineLoadLibraryExA(
    LPCSTR fileName, HANDLE file, DWORD flags)
{
    auto* original = gOriginalStreamlineLoadLibraryExA.load(
        std::memory_order_acquire);
    if (!original || original == &HookStreamlineLoadLibraryExA)
        return nullptr;
    const std::wstring requested = WidePathFromAnsi(fileName);
    const std::wstring redirect =
        SelectiveOtaDlssgWrapperRedirectPath(requested);
    HMODULE module = nullptr;
    if (!redirect.empty())
    {
        const std::string redirected = AnsiPathFromWide(redirect);
        if (!redirected.empty())
            module = original(redirected.c_str(), file, flags);
        const DWORD redirectError = module ? ERROR_SUCCESS : GetLastError();
        if (!module)
            module = original(fileName, file, flags);
        RecordSelectiveOtaDlssgWrapperResult(requested, redirect, module,
            redirectError, redirectError != ERROR_SUCCESS);
    }
    else
    {
        module = original(fileName, file, flags);
    }
    InspectStreamlineLoadedModule(module);
    return module;
}

HMODULE WINAPI HookStreamlineLoadLibraryExW(
    LPCWSTR fileName, HANDLE file, DWORD flags)
{
    auto* original = gOriginalStreamlineLoadLibraryExW.load(
        std::memory_order_acquire);
    if (!original || original == &HookStreamlineLoadLibraryExW)
        return nullptr;
    const std::wstring requested = fileName ? fileName : L"";
    const std::wstring redirect =
        SelectiveOtaDlssgWrapperRedirectPath(requested);
    HMODULE module = redirect.empty() ? original(fileName, file, flags)
        : original(redirect.c_str(), file, flags);
    if (!redirect.empty())
    {
        const DWORD redirectError = module ? ERROR_SUCCESS : GetLastError();
        if (!module)
            module = original(fileName, file, flags);
        RecordSelectiveOtaDlssgWrapperResult(requested, redirect, module,
            redirectError, redirectError != ERROR_SUCCESS);
    }
    InspectStreamlineLoadedModule(module);
    return module;
}

template <typename Function>
bool PreserveLoaderImportOriginal(std::atomic<Function>& destination,
    void* original, Function hook, const wchar_t* functionName,
    const std::wstring& path)
{
    if (!original)
        return destination.load(std::memory_order_acquire) != nullptr;
    auto* candidate = reinterpret_cast<Function>(original);
    if (candidate == hook)
        return destination.load(std::memory_order_acquire) != nullptr;
    Function expected = nullptr;
    if (!destination.compare_exchange_strong(expected, candidate,
            std::memory_order_acq_rel, std::memory_order_acquire)
        && expected != candidate)
    {
        Log(L"Streamline loader discovery skipped conflicting %s chain: %s",
            functionName, path.c_str());
        return false;
    }
    return true;
}

bool InstallStreamlineLoaderDiscovery(
    HMODULE module, const std::wstring& path)
{
    if (!module)
        return false;

    bool installed = false;
    bool newlyHooked = false;
    void* original = nullptr;
    // A failed protection restoration can leave the hook visible. Retain its
    // callable original independently of whether publication verified success.
    bool published = HookModuleImport(module, "KERNEL32.dll", "LoadLibraryA",
        reinterpret_cast<void*>(&HookStreamlineLoadLibraryA), original);
    if (original || published)
    {
        const bool chainReady = PreserveLoaderImportOriginal(
            gOriginalStreamlineLoadLibraryA, original,
            &HookStreamlineLoadLibraryA, L"LoadLibraryA", path);
        installed = (published && chainReady) || installed;
        newlyHooked = (published && chainReady && original != nullptr) || newlyHooked;
    }
    original = nullptr;
    published = HookModuleImport(module, "KERNEL32.dll", "LoadLibraryW",
        reinterpret_cast<void*>(&HookStreamlineLoadLibraryW), original);
    if (original || published)
    {
        const bool chainReady = PreserveLoaderImportOriginal(
            gOriginalStreamlineLoadLibraryW, original,
            &HookStreamlineLoadLibraryW, L"LoadLibraryW", path);
        installed = (published && chainReady) || installed;
        newlyHooked = (published && chainReady && original != nullptr) || newlyHooked;
    }
    original = nullptr;
    published = HookModuleImport(module, "KERNEL32.dll", "LoadLibraryExA",
        reinterpret_cast<void*>(&HookStreamlineLoadLibraryExA), original);
    if (original || published)
    {
        const bool chainReady = PreserveLoaderImportOriginal(
            gOriginalStreamlineLoadLibraryExA, original,
            &HookStreamlineLoadLibraryExA, L"LoadLibraryExA", path);
        installed = (published && chainReady) || installed;
        newlyHooked = (published && chainReady && original != nullptr) || newlyHooked;
    }
    original = nullptr;
    published = HookModuleImport(module, "KERNEL32.dll", "LoadLibraryExW",
        reinterpret_cast<void*>(&HookStreamlineLoadLibraryExW), original);
    if (original || published)
    {
        const bool chainReady = PreserveLoaderImportOriginal(
            gOriginalStreamlineLoadLibraryExW, original,
            &HookStreamlineLoadLibraryExW, L"LoadLibraryExW", path);
        installed = (published && chainReady) || installed;
        newlyHooked = (published && chainReady && original != nullptr) || newlyHooked;
    }
    if (installed)
        gStreamlineLoaderDiscoveryInstalled.store(true,
            std::memory_order_release);
    if (newlyHooked)
    {
        Log(L"Streamline loader-return discovery installed: %s",
            path.c_str());
    }
    return installed;
}

sl::Result HookSlInit(const sl::Preferences& preferences,
    uint64_t sdkVersion)
{
    fault_capture::BeforeSlInit();
    auto* original = EntryOriginal(gOriginalSlInit, gSlInitEntryHandle);
    if (!original || original == &HookSlInit)
        return sl::Result::eErrorNotInitialized;
#if defined(MFG_UNLOCK_SINGLE_MODULE_UI)
    // Recheck application factory imports at the existing early boundary.
    // The UI creates no graphics objects and modifies no native/Streamline tables.
    single_overlay::BeforeStreamlineInit();
#endif
    const auto maskInitEntry = entry_detour::ReadSnapshot(gSlInitEntryHandle);
    {
        std::lock_guard lock(gOutputPullMaskInitMutex);
        gOutputPullMaskSlInit = {};
        gOutputPullMaskVulkanInit = {};
    }

    InvalidateUiInputEvidence();
    gAppliedUiRecompositionEnabled.store(false, std::memory_order_release);
    gAppliedUiRecompositionForced.store(false, std::memory_order_release);
    ampere_backend::ObserveEarlyInit();

    sl::Preferences adjusted = preferences;
    const uint64_t before = static_cast<uint64_t>(adjusted.flags);
    static_assert(static_cast<uint64_t>(sl::PreferenceFlags::eAllowOTA)
        == streamline_ota_policy::kAllowOta);
    static_assert(static_cast<uint64_t>(
        sl::PreferenceFlags::eLoadDownloadedPlugins)
        == streamline_ota_policy::kLoadDownloadedPlugins);
    const std::wstring providerPath = JoinPath(
        gExecutableDirectory, L"nvngx_dlssg.dll");
    const bool providerPreflight =
        dlssg_provider_policy::SupportedProviderVersionMatches(
            providerPath.c_str());
    HMODULE interposer = GetModuleHandleW(L"sl.interposer.dll");
    const std::wstring interposerPath = interposer
        ? LoadedModulePath(interposer) : std::wstring{};
#if defined(MFG_UNLOCK_SINGLE_MODULE_UI)
    // Publish the existing resolver gateway before slInit can cache Vulkan
    // entry points inside the interposer. The worker may not have seen it yet.
    if (interposer)
        InstallSlCommonResolverDiscovery(interposer, interposerPath);
#endif
    if (interposer
        && !gStreamlineLoaderDiscoveryInstalled.load(
            std::memory_order_acquire))
    {
        // A resolver-return slInit detour can beat the background module scan.
        // Install discovery synchronously at the last point before the real
        // slInit starts selecting and loading its plug-in set.
        InstallStreamlineLoaderDiscovery(interposer, interposerPath);
    }
    const bool loaderDiscovery =
        gStreamlineLoaderDiscoveryInstalled.load(std::memory_order_acquire);
    ResolveNvidiaCompatibilityPolicy();
    const auto tier = static_cast<nvidia_mfg_policy::Tier>(
        gNvidiaCompatibilityTier.load(std::memory_order_acquire));
    const bool officialSixX = tier == nvidia_mfg_policy::Tier::eSixX;
    FileVersion hostVersion{};
    if (interposer)
        hostVersion = ReadFileVersion(interposerPath);
    const bool coherentHost = hostVersion.major == 2u
        && hostVersion.minor >= 10u;
    gStreamlineHostVersionMajor.store(
        hostVersion.major, std::memory_order_relaxed);
    gStreamlineHostVersionMinor.store(
        hostVersion.minor, std::memory_order_relaxed);
    gStreamlineHostVersionBuild.store(
        hostVersion.build, std::memory_order_relaxed);
    gStreamlineHostVersionPrivate.store(
        hostVersion.privatePart, std::memory_order_relaxed);
    // Selectively redirecting only sl.dlss_g crossed Streamline ABI families
    // and caused startup failures. Retire that route: official 6X titles opt
    // into Streamline's coherent full-set OTA selection instead.
    ConfigureSelectiveOtaDlssgWrapper(false,
        providerPreflight, loaderDiscovery);
    const streamline_ota_policy::Result policy =
        streamline_ota_policy::Apply(
            before, gpu_dispatch::IsAda() && officialSixX, coherentHost, loaderDiscovery);
    const uint64_t after = policy.flags;
    adjusted.flags = static_cast<sl::PreferenceFlags>(after);
    gSlInitFlagsBefore.store(before, std::memory_order_release);
    gSlInitFlagsAfter.store(after, std::memory_order_release);
    gOtaPreferencesForced.store(
        policy.allowOtaForced || policy.loadDownloadedPluginsForced,
        std::memory_order_release);
    gDownloadedStreamlinePluginsForced.store(
        policy.loadDownloadedPluginsForced, std::memory_order_release);
    gOtaProviderPreflightSupported.store(
        providerPreflight, std::memory_order_release);
    gOtaForceSuppressed.store(
        policy.fullOtaSuppressed,
        std::memory_order_release);
    gFullStreamlineOtaRequested.store(
        policy.fullOtaRequested, std::memory_order_release);
    gFullStreamlineOtaEligible.store(
        policy.fullOtaRequested && !policy.fullOtaSuppressed,
        std::memory_order_release);
    const uint64_t call =
        gSlInitCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    Log(L"Streamline slInit call=%llu sdkVersion=0x%llX "
        L"flagsBefore=0x%llX flagsAfter=0x%llX ngxOtaAllowed=%d "
        L"downloadedStreamlinePlugins=%d "
        L"loaderDiscovery=%d fullOtaRequested=%d fullOtaEligible=%d "
        L"profile=%hs nvidiaTier=%hs hostVersion=%u.%u.%u.%u "
        L"providerPreflight=%d",
        static_cast<unsigned long long>(call),
        static_cast<unsigned long long>(sdkVersion),
        static_cast<unsigned long long>(before),
        static_cast<unsigned long long>(after),
        (after & streamline_ota_policy::kAllowOta) != 0,
        (after & streamline_ota_policy::kLoadDownloadedPlugins) != 0,
        loaderDiscovery,
        policy.fullOtaRequested,
        policy.fullOtaRequested && !policy.fullOtaSuppressed,
        gNvidiaProfileName.c_str(), nvidia_mfg_policy::TierName(tier),
        hostVersion.major, hostVersion.minor, hostVersion.build,
        hostVersion.privatePart, providerPreflight);
    const sl::Result result = InvokeAmpereInit(original, adjusted, sdkVersion);
    Log(L"Streamline slInit original result=%u call=%llu", static_cast<unsigned>(result),
        static_cast<unsigned long long>(call));
    ObserveOutputPullMaskSlInitResult(maskInitEntry, result == sl::Result::eOk);
    if (result == sl::Result::eOk)
    {
        // Streamline may load feature or NGX provider modules before returning.
        // Inspect them synchronously so already-resolved NGX pointers enter the
        // code-entry detours before the game can create its first FG pipeline.
        HMODULE common = GetModuleHandleW(L"sl.common.dll");
        if (common)
        {
            InstallSlCommonResolverDiscovery(
                common, LoadedModulePath(common));
        }
        InspectAlreadyLoadedModules();
    }
    return result;
}

bool TryInstallSlInitEntryDetour(HMODULE interposer, void* resolvedTarget)
{
    if (!interposer)
        return false;
    const std::wstring path = LoadedModulePath(interposer);
    if (!ModuleFileNameEquals(path, L"sl.interposer.dll"))
        return false;
    void* target = resolvedTarget ? resolvedTarget
        : reinterpret_cast<void*>(GetProcAddress(interposer, "slInit"));
    if (!target || ModuleFromAddress(target) != interposer)
        return false;

    void* trampoline = nullptr;
    const bool installed = entry_detour::Install(
        entry_detour::Kind::eSlInit, interposer, target,
        reinterpret_cast<void*>(&HookSlInit), trampoline,
        entry_detour::InstallOptions{}, &gSlInitEntryHandle);
    if (trampoline)
    {
        gOriginalSlInit.store(reinterpret_cast<PFun_slInit*>(trampoline),
            std::memory_order_release);
    }
    const entry_detour::Snapshot state = entry_detour::ReadSnapshot(
        entry_detour::Kind::eSlInit);
    Log(L"Streamline slInit entry detour: installed=%d current=%d "
        L"cachedPointersCovered=%d failure=%u targetRva=0x%X path=%s",
        installed, state.current, state.cachedPointersCovered,
        static_cast<uint32_t>(state.failure), state.targetRva,
        path.c_str());
    return installed;
}

FARPROC WINAPI HookMainGetProcAddress(HMODULE module, LPCSTR functionName)
{
    GetProcAddressFn original = gOriginalMainGetProcAddress.load(
        std::memory_order_acquire);
    if (!original)
        return nullptr;
    FARPROC resolved = original(module, functionName);
    resolved = fault_capture::ResolveProc(module, functionName, resolved);
    if (resolved && reinterpret_cast<uintptr_t>(functionName) > 0xFFFFu
        && ngx_initialization::IsEntry(functionName))
        InspectLoadedModule(module, LoadedModulePath(module));
    if (resolved && gpu_dispatch::IsAda()) dlssg_preset::PrepareForExport(module, functionName);
#if defined(MFG_UNLOCK_SINGLE_MODULE_UI)
    if (resolved) single_overlay::application_imports::ArmModule(module);
    resolved = single_overlay::ResolveProc(module, functionName, resolved);
#endif
    if (!resolved || !functionName
        || reinterpret_cast<uintptr_t>(functionName) <= 0xFFFFu
        || !ModuleFileNameEquals(LoadedModulePath(module),
            L"sl.interposer.dll"))
    {
        return resolved;
    }

    if (std::strcmp(functionName, "slGetFeatureFunction") == 0)
    {
        auto* candidate = reinterpret_cast<PFun_slGetFeatureFunction*>(resolved);
        if (candidate == &HookSlGetFeatureFunction)
            return resolved;
        if (ModuleFromAddress(reinterpret_cast<void*>(resolved)) != module)
            return resolved;
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(resolved), &pinned) || pinned != module)
            return resolved;
        auto* expected = static_cast<PFun_slGetFeatureFunction*>(nullptr);
        if (!gOriginalGetFeatureFunction.compare_exchange_strong(expected, candidate,
                std::memory_order_acq_rel) && expected != candidate)
            return resolved;
        if (!expected)
            Log(L"Streamline feature-function dynamic gateway installed: path=%s",
                LoadedModulePath(module).c_str());
        return reinterpret_cast<FARPROC>(&HookSlGetFeatureFunction);
    }

    if (std::strcmp(functionName, "slSetD3DDevice") == 0)
    {
        auto* candidate = reinterpret_cast<PFun_slSetD3DDevice*>(resolved);
        if (candidate != &HookSlSetD3DDevice) gOriginalSetD3DDevice.store(candidate);
        return reinterpret_cast<FARPROC>(&HookSlSetD3DDevice);
    }

    if (std::strcmp(functionName, "slSetVulkanInfo") == 0)
    {
        auto* candidate = reinterpret_cast<PFun_slSetVulkanInfoAbi*>(
            resolved);
        if (candidate != &HookSlSetVulkanInfo)
            gOriginalSetVulkanInfo.store(candidate,
                std::memory_order_release);
        return reinterpret_cast<FARPROC>(&HookSlSetVulkanInfo);
    }
    if (std::strcmp(functionName, "slInit") != 0)
        return resolved;

    if (TryInstallSlInitEntryDetour(module,
            reinterpret_cast<void*>(resolved)))
    {
        return resolved;
    }

    auto* candidate = reinterpret_cast<PFun_slInit*>(resolved);
    if (candidate != &HookSlInit)
        gOriginalSlInit.store(candidate, std::memory_order_release);
    gSlInitResolverFallbackActive.store(true, std::memory_order_release);
    return reinterpret_cast<FARPROC>(&HookSlInit);
}

bool InstallMainResolverDiscovery()
{
    void* original = nullptr;
    bool installed = HookMainExecutableImport("KERNEL32.dll",
        "GetProcAddress", reinterpret_cast<void*>(&HookMainGetProcAddress),
        original);
#if defined(MFG_UNLOCK_SINGLE_MODULE_UI)
    if (!installed)
    {
        // GTA Enhanced has four-byte-aligned x64 imports. Preserve the strict
        // pointer publisher and the IAT; intercept only main-executable callers
        // at the identified public resolver entry instead.
        HMODULE executable = GetModuleHandleW(nullptr);
        single_overlay::slots::VisitImports(executable, [&](const char* name, void** slot) {
            if (strcmp(name, "GetProcAddress")) return true;
            void* target = nullptr;
            memcpy(&target, slot, sizeof(target));
            if (!caller_scoped_import::Eligible(executable, slot, target, name)) return true;
            void* trampoline = nullptr;
            const auto replacement = reinterpret_cast<void*>(&HookMainGetProcAddress);
            if (caller_scoped_import::Prepare(executable, slot, target,replacement,name,trampoline)) {
                gOriginalMainGetProcAddress.store(reinterpret_cast<GetProcAddressFn>(trampoline),std::memory_order_release);
                installed = caller_scoped_import::Activate(target,replacement);
                if (installed) original = trampoline;
            }
            return false;
        });
    }
#endif
    if (original)
    {
        const auto candidate = reinterpret_cast<GetProcAddressFn>(original);
        GetProcAddressFn expected = nullptr;
        if (!gOriginalMainGetProcAddress.compare_exchange_strong(
                expected, candidate, std::memory_order_acq_rel,
                std::memory_order_acquire)
            && expected != candidate)
        {
            return false;
        }
    }
    gMainResolverDiscoveryInstalled.store(installed,
        std::memory_order_release);
    return installed;
}

bool InstallSlInitIatFallback()
{
    void* original = nullptr;
    const bool installed = HookMainExecutableImport("sl.interposer.dll",
        "slInit", reinterpret_cast<void*>(&HookSlInit), original);
    if (original)
    {
        auto* candidate = reinterpret_cast<PFun_slInit*>(original);
        if (candidate != &HookSlInit)
            gOriginalSlInit.store(candidate, std::memory_order_release);
    }
    gSlInitIatFallbackInstalled.store(installed,
        std::memory_order_release);
    return installed;
}

bool InstallSlInitControlPath()
{
    const bool resolverDiscovery = InstallMainResolverDiscovery();
    HMODULE interposer = GetModuleHandleW(L"sl.interposer.dll");
    const bool entryInstalled = interposer
        && TryInstallSlInitEntryDetour(interposer, nullptr);
    const bool iatFallback = entryInstalled
        ? false : InstallSlInitIatFallback();
    return entryInstalled || iatFallback || resolverDiscovery;
}

FARPROC WINAPI HookSlCommonGetProcAddress(
    HMODULE module, LPCSTR functionName)
{
    GetProcAddressFn original = gOriginalSlCommonGetProcAddress.load(
        std::memory_order_acquire);
    if (!original)
        return nullptr;
    FARPROC resolved = original(module, functionName);
#if defined(MFG_UNLOCK_SINGLE_MODULE_UI)
    // Vulkan is often resolved inside Streamline rather than the executable.
    // Reuse its established resolver boundary without wrapping internal DXGI
    // factories or changing shared Streamline/native method implementations.
    if (functionName && reinterpret_cast<uintptr_t>(functionName) > 0xFFFFu
        && functionName[0] == 'v' && functionName[1] == 'k')
        resolved = single_overlay::ResolveProc(module, functionName, resolved);
#endif
    if (resolved && reinterpret_cast<uintptr_t>(functionName) > 0xFFFFu
        && ngx_initialization::IsEntry(functionName))
        InspectLoadedModule(module, LoadedModulePath(module));
    if (resolved && gpu_dispatch::IsAda()) dlssg_preset::PrepareForExport(module, functionName);
    if (!resolved || !functionName
        || reinterpret_cast<uintptr_t>(functionName) <= 0xFFFFu)
        return resolved;
    const bool create = std::strcmp(functionName,
        "NVSDK_NGX_D3D12_CreateFeature") == 0;
    const bool evaluate = std::strcmp(functionName,
        "NVSDK_NGX_D3D12_EvaluateFeature") == 0;
    const bool ampereStartup = true
        && (std::strcmp(functionName, "NVSDK_NGX_D3D12_GetFeatureRequirements") == 0
            || std::strcmp(functionName, "NVSDK_NGX_D3D12_GetCapabilityParameters") == 0);
    const bool vulkan = std::strcmp(functionName,
            "NVSDK_NGX_VULKAN_CreateFeature") == 0
        || std::strcmp(functionName,
            "NVSDK_NGX_VULKAN_CreateFeature1") == 0
        || std::strcmp(functionName,
            "NVSDK_NGX_VULKAN_EvaluateFeature") == 0
        || std::strcmp(functionName,
            "NVSDK_NGX_VULKAN_Init") == 0
        || std::strcmp(functionName,
            "NVSDK_NGX_VULKAN_Init_Ext") == 0
        || std::strcmp(functionName,
            "NVSDK_NGX_VULKAN_Init_Ext2") == 0
        || std::strcmp(functionName,
            "NVSDK_NGX_VULKAN_Init_with_ProjectID") == 0
        || std::strcmp(functionName,
            "NVSDK_NGX_VULKAN_Init_ProjectID") == 0
        || std::strcmp(functionName,
            "NVSDK_NGX_VULKAN_Init_ProjectID_Ext") == 0;
    if (!create && !evaluate && !vulkan && !ampereStartup)
        return resolved;

    // This resolver interception is discovery only. The raw function pointer
    // is returned unchanged after the implementation entry itself has been
    // atomically detoured, so already-cached and newly-resolved pointers take
    // the same route.
    const std::wstring path = LoadedModulePath(module);
    const ModuleRecord record = InspectLoadedModule(module, path);
    if (record.ngxExport)
    {
        TryInstallNgxCreateEntryDetour(module, path, record.generation);
        TryInstallNgxEvaluateEntryDetour(module, path, record.generation);
        TryInstallNgxVulkanCreateEntryDetours(
            module, path, record.generation);
        TryInstallNgxVulkanEvaluateEntryDetour(
            module, path, record.generation);
        TryInstallNgxVulkanAdapterEntryDetours(
            module, path, record.generation);
    }
    if (record.ngxRuntimeExport)
    {
        TryInstallNgxRuntimeCreateEntryDetour(
            module, path, record.generation);
        TryInstallNgxRuntimeEvaluateEntryDetour(
            module, path, record.generation);
        TryInstallNgxRuntimeVulkanCreateEntryDetours(
            module, path, record.generation);
        TryInstallNgxRuntimeVulkanEvaluateEntryDetour(
            module, path, record.generation);
        TryInstallNgxVulkanAdapterEntryDetours(
            module, path, record.generation);
    }
    return resolved;
}

bool InstallSlCommonResolverDiscovery(
    HMODULE module, const std::wstring& path)
{
    if (!module)
        return false;
    void* original = nullptr;
    const bool installed = HookModuleImport(module, "KERNEL32.dll",
        "GetProcAddress", reinterpret_cast<void*>(&HookSlCommonGetProcAddress),
        original);
    if (original)
    {
        GetProcAddressFn expected = nullptr;
        const auto candidate = reinterpret_cast<GetProcAddressFn>(original);
        if (!gOriginalSlCommonGetProcAddress.compare_exchange_strong(
                expected, candidate, std::memory_order_acq_rel,
                std::memory_order_acquire)
            && expected != candidate)
            return false;
    }
    if (installed)
    {
        gSlCommonResolverDiscoveryInstalled.store(
            true, std::memory_order_release);
        if (original)
        {
            Log(L"Streamline NGX resolver discovery installed: %s",
                path.c_str());
        }
        std::lock_guard lock(gModuleMutex);
        for (auto& record : gModuleRecords)
        {
            if (record.module == module)
            {
                record.createResolverDiscoveryHooked = true;
                break;
            }
        }
    }
    return installed;
}

bool InstallFeatureFunctionHook()
{
    void* original = nullptr;
    const bool installed = HookMainExecutableImport("sl.interposer.dll",
        "slGetFeatureFunction", reinterpret_cast<void*>(&HookSlGetFeatureFunction), original);
    if (original)
    {
        gOriginalGetFeatureFunction.store(
            reinterpret_cast<PFun_slGetFeatureFunction*>(original),
            std::memory_order_release);
    }
    return installed;
}

bool InstallD3DDeviceHook()
{
    void* original = nullptr;
    const bool installed = HookMainExecutableImport("sl.interposer.dll",
        "slSetD3DDevice", reinterpret_cast<void*>(&HookSlSetD3DDevice), original);
    if (original)
    {
        gOriginalSetD3DDevice.store(
            reinterpret_cast<PFun_slSetD3DDevice*>(original),
            std::memory_order_release);
    }
    return installed;
}

bool InstallVulkanInfoHook()
{
    void* original = nullptr;
    const bool installed = HookMainExecutableImport("sl.interposer.dll",
        "slSetVulkanInfo", reinterpret_cast<void*>(&HookSlSetVulkanInfo),
        original);
    if (original)
    {
        gOriginalSetVulkanInfo.store(
            reinterpret_cast<PFun_slSetVulkanInfoAbi*>(original),
            std::memory_order_release);
    }
    return installed;
}

bool InstallUiTagHooks()
{
    void* legacyOriginal = nullptr;
    const bool legacyInstalled = HookMainExecutableImport("sl.interposer.dll",
        "slSetTag", reinterpret_cast<void*>(&HookSlSetTag), legacyOriginal);
    if (legacyOriginal)
    {
        gOriginalSetTag.store(reinterpret_cast<PFun_slSetTag*>(legacyOriginal),
            std::memory_order_release);
    }

    void* frameOriginal = nullptr;
    const bool frameInstalled = HookMainExecutableImport("sl.interposer.dll",
        "slSetTagForFrame", reinterpret_cast<void*>(&HookSlSetTagForFrame), frameOriginal);
    if (frameOriginal)
    {
        gOriginalSetTagForFrame.store(
            reinterpret_cast<PFun_slSetTagForFrame*>(frameOriginal),
            std::memory_order_release);
    }

    const bool installed = legacyInstalled || frameInstalled;
    gUiTagHookInstalled.store(installed, std::memory_order_release);
    return installed;
}

static constexpr std::array<uint8_t, 3> kWrapperOriginal{ 0x0F, 0x42, 0xD1 };
static constexpr std::array<uint8_t, 3> kWrapperReplacement{ 0x90, 0x90, 0x90 };

struct PatternPatchResult
{
    bool candidate = false;
    bool patched = false;
    uint8_t* match = nullptr;
    uint32_t profileMaximum = 0;
};

const IMAGE_NT_HEADERS64* ImageHeaders(HMODULE module)
{
    const auto* base = reinterpret_cast<const uint8_t*>(module);
    if (!base)
        return nullptr;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0
        || static_cast<size_t>(dos->e_lfanew) > 1024 * 1024)
        return nullptr;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE
        || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return nullptr;
    return nt;
}

bool RvaRangeIsValid(const IMAGE_NT_HEADERS64* nt, DWORD rva, size_t size)
{
    return nt && rva < nt->OptionalHeader.SizeOfImage
        && size <= static_cast<size_t>(nt->OptionalHeader.SizeOfImage - rva);
}

bool ModuleExportsFunction(HMODULE module, const char* expected)
{
    const auto* nt = ImageHeaders(module);
    if (!nt || !expected)
        return false;

    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!directory.VirtualAddress
        || !RvaRangeIsValid(nt, directory.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY)))
        return false;

    const auto* base = reinterpret_cast<const uint8_t*>(module);
    const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(
        base + directory.VirtualAddress);
    const size_t namesSize = static_cast<size_t>(exports->NumberOfNames) * sizeof(DWORD);
    if (!exports->AddressOfNames
        || !RvaRangeIsValid(nt, exports->AddressOfNames, namesSize))
        return false;

    const auto* names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
    for (DWORD index = 0; index < exports->NumberOfNames; ++index)
    {
        const DWORD nameRva = names[index];
        if (!RvaRangeIsValid(nt, nameRva, 1))
            continue;
        const char* name = reinterpret_cast<const char*>(base + nameRva);
        const size_t remaining = nt->OptionalHeader.SizeOfImage - nameRva;
        const size_t length = strnlen_s(name, remaining);
        if (length < remaining && strcmp(name, expected) == 0)
            return true;
    }
    return false;
}

PatternPatchResult PatchNgxMfgGate(HMODULE module, const std::wstring& path)
{
    const bool wasReady = ngx_mfg_gate::Ready(module);
    const auto result = ngx_mfg_gate::Patch(module, gpu_dispatch::IsAda()
        && (gpu_backend::AdapterVerified() || gVulkanAdapterVerified.load(std::memory_order_acquire)));
    if (result.patched && !wasReady)
        Log(L"NGX MFG count/index gate restored with verified atomic publication at RVA 0x%zX: %s",
            static_cast<size_t>(result.match + 2 - reinterpret_cast<uint8_t*>(module)), path.c_str());
    return {result.candidate, result.patched, result.match};
}

PatternPatchResult PatchUniqueWrapperMaximumPattern(
    HMODULE module, const std::wstring& path)
{
    const auto* base = reinterpret_cast<const uint8_t*>(module);
    const auto* nt = ImageHeaders(module);
    if (!nt)
        return {};

    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    uint8_t* match = nullptr;
    size_t matchCount = 0;
    for (unsigned index = 0; index < nt->FileHeader.NumberOfSections;
         ++index, ++section)
    {
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0
            || section->VirtualAddress >= nt->OptionalHeader.SizeOfImage)
        {
            continue;
        }
        auto* begin = const_cast<uint8_t*>(
            base + section->VirtualAddress);
        const size_t available =
            nt->OptionalHeader.SizeOfImage - section->VirtualAddress;
        const size_t size = std::min<size_t>(available,
            std::max<size_t>(section->Misc.VirtualSize,
                section->SizeOfRawData));
        for (size_t offset = 0;
             offset + universal_wrapper_profile::kPatternSize <= size;
             ++offset)
        {
            if (!universal_wrapper_profile::Matches(
                    begin + offset, size - offset))
            {
                continue;
            }
            match = begin + offset;
            ++matchCount;
        }
    }

    if (matchCount == 0)
        return {};
    if (matchCount != 1 || !match)
    {
        Log(L"Streamline maximum: expected one 1/3/5 profile, found %zu: %s",
            matchCount, path.c_str());
        return {true, false, nullptr};
    }

    uint32_t compiledMaximum = 0;
    memcpy(&compiledMaximum,
        match + universal_wrapper_profile::kMaximumOffset,
        sizeof(compiledMaximum));
    if (!universal_wrapper_profile::IsSupportedMaximum(compiledMaximum))
    {
        Log(L"Streamline maximum: unsupported compiled value %u: %s",
            compiledMaximum, path.c_str());
        return {true, false, match};
    }

    if (!gpu_dispatch::IsAda())
    {
        return {true, UseAmpere() && memcmp(match + universal_wrapper_profile::kPatchOffset,
            kWrapperOriginal.data(), kWrapperOriginal.size()) == 0, match, compiledMaximum};
    }
    uint8_t* address = match + universal_wrapper_profile::kPatchOffset;
    if (memcmp(address, kWrapperReplacement.data(),
            kWrapperReplacement.size()) == 0)
    {
        Log(L"Streamline maximum: already patched at RVA 0x%zX "
            L"(compiled=%u): %s",
            static_cast<size_t>(address
                - const_cast<uint8_t*>(base)),
            compiledMaximum, path.c_str());
        return {true, true, match, compiledMaximum};
    }
    if (memcmp(address, kWrapperOriginal.data(),
            kWrapperOriginal.size()) != 0)
    {
        Log(L"Streamline maximum: matched context but original bytes differ: %s",
            path.c_str());
        return {true, false, match, compiledMaximum};
    }
    DWORD oldProtection = 0;
    if (!VirtualProtect(address, kWrapperReplacement.size(),
            PAGE_EXECUTE_READWRITE, &oldProtection))
    {
        Log(L"Streamline maximum: VirtualProtect failed (%lu): %s",
            GetLastError(), path.c_str());
        return {true, false, match, compiledMaximum};
    }
    memcpy(address, kWrapperReplacement.data(),
        kWrapperReplacement.size());
    FlushInstructionCache(GetCurrentProcess(), address,
        kWrapperReplacement.size());
    DWORD ignoredProtection = 0;
    const BOOL restored = VirtualProtect(address,
        kWrapperReplacement.size(), oldProtection, &ignoredProtection);
    if (!restored)
    {
        Log(L"Streamline maximum: protection restore failed (%lu): %s",
            GetLastError(), path.c_str());
        return {true, false, match, compiledMaximum};
    }

    Log(L"Streamline maximum: patched RVA 0x%zX (compiled=%u): %s",
        static_cast<size_t>(address - const_cast<uint8_t*>(base)),
        compiledMaximum, path.c_str());
    return {true, true, match, compiledMaximum};
}

std::wstring LoadedModulePath(HMODULE module)
{
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(module, path.data(),
        static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size())
        return {};
    path.resize(length);
    return path;
}

uint64_t ModuleGeneration(HMODULE module) noexcept
{
    if (!module)
        return 0;
    std::lock_guard lock(gModuleMutex);
    const auto record = std::find_if(gModuleRecords.begin(),
        gModuleRecords.end(), [&](const ModuleRecord& candidate) {
            return candidate.module == module;
        });
    return record == gModuleRecords.end() ? 0 : record->generation;
}

ampere_gpu::PreparationBoundary ResolveAmperePreparationBoundary(HMODULE provider, uint64_t generation) noexcept
{
    if (!provider || !generation || !UseAmpere()
        || !gDllNotificationRegistered.load(std::memory_order_acquire)
        || !ampere_backend::BeforeFirstFeatureCreate()
        || gFrameGenerationCreateAttemptEpoch.load(std::memory_order_acquire)) return {};
    uint64_t loadToken = 0;
    bool nativeOwnerCurrent = false;
    const auto native = gAmpereNativeInitBoundary;
    if (native.current && (!gAmpereNativeInitTicket
        || ngx_initialization::PreparationTicket() != gAmpereNativeInitTicket)) return {};
    {
        std::lock_guard lock(gModuleMutex);
        for (const auto& record : gModuleRecords)
        {
            if (record.module == provider && record.generation == generation && record.ngxExport)
                loadToken = record.freshLoadToken;
            if (native.current && record.module == native.owner && record.generation == native.generation
                && (record.ngxExport || record.ngxRuntimeD3D12Export)
                && gOutputPullMaskLoads.Matches(reinterpret_cast<uintptr_t>(record.module), record.freshLoadToken))
                nativeOwnerCurrent = true;
        }
    }
    const auto module = reinterpret_cast<uintptr_t>(provider);
    if (!gOutputPullMaskLoads.Matches(module, loadToken)) return {};
    bool proven = false;
    if (nativeOwnerCurrent)
    {
        const auto current = entry_detour::ReadSnapshot(native.handle);
        proven = current.current && current.owner == native.owner && current.generation == native.generation
            && current.target == native.target && current.original == native.original
            && (current.kind == entry_detour::Kind::eNgxD3D12Init
                || current.kind == entry_detour::Kind::eNgxD3D12InitProject);
    }
    if (!proven)
    {
        OutputPullMaskInitEvidence init;
        { std::lock_guard lock(gOutputPullMaskInitMutex); init = gOutputPullMaskSlInit; }
        const auto current = entry_detour::ReadSnapshot(init.handle);
        proven = init.owner && current.current && current.kind == entry_detour::Kind::eSlInit
            && current.owner == init.owner && current.generation == init.generation;
    }
    // Positive initialization evidence is paired with an actual map event and
    // the current inventory generation; counters alone never authorize a write.
    if (!proven || !gOutputPullMaskLoads.Matches(module, loadToken)) return {};
    return {generation, loadToken, true, true, &AmperePreparationStillCurrent};
}

bool AmperePreparationStillCurrent(HMODULE provider, const ampere_gpu::PreparationBoundary& before) noexcept
{
    if (!before.Proven()) return false;
    const auto current = ResolveAmperePreparationBoundary(provider, before.providerGeneration);
    return current.Proven() && current.providerGeneration == before.providerGeneration
        && current.freshLoadToken == before.freshLoadToken;
}

void PrepareNgxInitialization(void* device, const entry_detour::Snapshot& entry,
    const void*, bool firstInit) noexcept
{
    if (gpu_dispatch::IsAda() || gpu_dispatch::Selected() == gpu_dispatch::Family::eConflict) return;
    const auto ticket = ngx_initialization::PreparationTicket();
    if (!firstInit || !ticket || !entry.current || !device
        || ModuleGeneration(entry.owner) != entry.generation
        || !gOutputPullMaskLoads.Current(reinterpret_cast<uintptr_t>(entry.owner))) return;
    if (!ampere_backend::BeginNativePreparation()) return;
    __try
    {
        // Reserve the nonblocking lifecycle scope before taking GPU selection
        // or adapter locks. These observations never publish a provider program.
        if (adapter_discovery::IsAmpereD3D12Device(device)
            && gpu_backend::ObserveD3D12Device(device) && UseAmpere()
            && ngx_initialization::PreparationTicket() == ticket)
        {
            gAmpereNativeInitBoundary = entry;
            gAmpereNativeInitTicket = ticket;
            gModuleInventoryDirty.store(true, std::memory_order_release);
            InspectAlreadyLoadedModules();
        }
    }
    __finally
    {
        gAmpereNativeInitBoundary = {};
        gAmpereNativeInitTicket = 0;
        ampere_backend::EndNativePreparation();
    }
}

void ObserveOutputPullMaskSlInitResult(
    const entry_detour::Snapshot& before, bool succeeded) noexcept
{
    const auto current = entry_detour::ReadSnapshot(before.handle);
    const bool valid = succeeded && before.current && before.owner
        && before.kind == entry_detour::Kind::eSlInit && current.current
        && current.handle == before.handle && current.owner == before.owner
        && current.generation == before.generation && current.original == before.original;
    std::lock_guard lock(gOutputPullMaskInitMutex);
    gOutputPullMaskSlInit = valid
        ? OutputPullMaskInitEvidence{before.handle, before.owner, before.generation}
        : OutputPullMaskInitEvidence{};
}

void ObserveOutputPullMaskVulkanInit(entry_detour::Handle handle) noexcept
{
    const auto entry = entry_detour::ReadSnapshot(handle);
    const bool valid = entry.current && entry.owner && entry.generation
        && entry.kind == entry_detour::Kind::eNgxVulkanAdapterInit
        && ModuleGeneration(entry.owner) == entry.generation;
    std::lock_guard lock(gOutputPullMaskInitMutex);
    gOutputPullMaskVulkanInit = valid
        ? OutputPullMaskInitEvidence{handle, entry.owner, entry.generation}
        : OutputPullMaskInitEvidence{};
}

bool OutputPullMaskEarlyInitProven(HMODULE provider, NgxGraphicsApi api) noexcept
{
    if (!provider || !gpu_dispatch::IsAda()
        || (api != NgxGraphicsApi::eD3D12 && api != NgxGraphicsApi::eVulkan)
        || !gDllNotificationRegistered.load(std::memory_order_acquire))
        return false;
    uint64_t generation = 0;
    uint64_t loadToken = 0;
    {
        std::lock_guard lock(gModuleMutex);
        const auto record = std::find_if(gModuleRecords.begin(), gModuleRecords.end(),
            [&](const ModuleRecord& candidate) { return candidate.module == provider; });
        if (record == gModuleRecords.end() || !record->ngxExport)
            return false;
        generation = record->generation;
        loadToken = record->freshLoadToken;
    }
    const uintptr_t module = reinterpret_cast<uintptr_t>(provider);
    if (!generation || generation != gActiveNgxProviderGeneration.load(std::memory_order_acquire)
        || !gOutputPullMaskLoads.Matches(module, loadToken))
        return false;
    OutputPullMaskInitEvidence slInit;
    OutputPullMaskInitEvidence vulkanInit;
    {
        std::lock_guard lock(gOutputPullMaskInitMutex);
        slInit = gOutputPullMaskSlInit;
        vulkanInit = gOutputPullMaskVulkanInit;
    }
    const auto proofCurrent = [](const OutputPullMaskInitEvidence& proof,
        entry_detour::Kind kind) noexcept {
        const auto current = entry_detour::ReadSnapshot(proof.handle);
        return proof.owner && current.current && current.kind == kind
            && current.handle == proof.handle && current.owner == proof.owner
            && current.generation == proof.generation;
    };
    // An observed call counter is insufficient: only successful slInit with
    // the same retained entry grants evidence. Nested in-progress calls skip.
    if (proofCurrent(slInit, entry_detour::Kind::eSlInit))
        return gOutputPullMaskLoads.Matches(module, loadToken);
    if (api != NgxGraphicsApi::eVulkan
        || !proofCurrent(vulkanInit, entry_detour::Kind::eNgxVulkanAdapterInit))
        return false;
    if (vulkanInit.owner == provider)
        return vulkanInit.generation == generation
            && gOutputPullMaskLoads.Matches(module, loadToken);
    // Shared runtime Init is useful only for the exact current runtime Create
    // route. An unrelated runtime or provider cannot donate init evidence.
    const auto create = entry_detour::ReadSnapshot(UnpackEntryHandle(
        gActiveNgxCreateHandle.load(std::memory_order_acquire)));
    return create.current && create.owner == vulkanInit.owner
        && create.generation == vulkanInit.generation
        && (create.kind == entry_detour::Kind::eNgxRuntimeVulkanCreateFeature
            || create.kind == entry_detour::Kind::eNgxRuntimeVulkanCreateFeature1)
        && ModuleGeneration(vulkanInit.owner) == vulkanInit.generation
        && gOutputPullMaskLoads.Matches(module, loadToken);
}

bool UsesNvidiaOtaCache(const std::wstring& path)
{
    std::wstring normalized = path;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
        [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
    return normalized.find(L"\\nvidia\\ngx\\models\\")
        != std::wstring::npos;
}

FileVersion ReadFileVersion(const std::wstring& path)
{
    FileVersion version{};
    DWORD ignored = 0;
    const DWORD bytes = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!bytes)
        return version;
    std::vector<uint8_t> data(bytes);
    if (!GetFileVersionInfoW(path.c_str(), 0, bytes, data.data()))
        return version;
    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT fixedBytes = 0;
    if (!VerQueryValueW(data.data(), L"\\",
            reinterpret_cast<void**>(&fixed), &fixedBytes)
        || !fixed || fixedBytes < sizeof(*fixed)
        || fixed->dwSignature != 0xFEEF04BDu)
        return version;
    version.major = HIWORD(fixed->dwFileVersionMS);
    version.minor = LOWORD(fixed->dwFileVersionMS);
    version.build = HIWORD(fixed->dwFileVersionLS);
    version.privatePart = LOWORD(fixed->dwFileVersionLS);
    return version;
}

bool ReadDiskWrapperProfile(const std::wstring& path,
    uint32_t& compiledMaximum)
{
    compiledMaximum = 0;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER size{};
    const bool validSize = GetFileSizeEx(file, &size)
        && size.QuadPart >= static_cast<LONGLONG>(sizeof(IMAGE_DOS_HEADER))
        && size.QuadPart <= 32ll * 1024ll * 1024ll;
    if (!validSize)
    {
        CloseHandle(file);
        return false;
    }

    std::vector<uint8_t> bytes(static_cast<size_t>(size.QuadPart));
    DWORD bytesRead = 0;
    const BOOL read = ReadFile(file, bytes.data(),
        static_cast<DWORD>(bytes.size()), &bytesRead, nullptr);
    CloseHandle(file);
    if (!read || bytesRead != bytes.size())
        return false;

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0
        || static_cast<size_t>(dos->e_lfanew)
            > bytes.size() - sizeof(IMAGE_NT_HEADERS64))
    {
        return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        bytes.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE
        || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64
        || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
    {
        return false;
    }
    const auto* sections = IMAGE_FIRST_SECTION(nt);
    const size_t sectionTableOffset = reinterpret_cast<const uint8_t*>(sections)
        - bytes.data();
    const size_t sectionTableBytes = static_cast<size_t>(
        nt->FileHeader.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER);
    if (sectionTableOffset > bytes.size()
        || sectionTableBytes > bytes.size() - sectionTableOffset)
    {
        return false;
    }

    size_t matchCount = 0;
    uint32_t matchedMaximum = 0;
    for (unsigned index = 0; index < nt->FileHeader.NumberOfSections;
         ++index)
    {
        const IMAGE_SECTION_HEADER& section = sections[index];
        if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0
            || section.PointerToRawData >= bytes.size())
        {
            continue;
        }
        const size_t available = bytes.size() - section.PointerToRawData;
        const size_t sectionSize = std::min<size_t>(
            section.SizeOfRawData, available);
        const uint8_t* begin = bytes.data() + section.PointerToRawData;
        for (size_t offset = 0;
             offset + universal_wrapper_profile::kPatternSize <= sectionSize;
             ++offset)
        {
            if (!universal_wrapper_profile::Matches(
                    begin + offset, sectionSize - offset))
            {
                continue;
            }
            uint32_t maximum = 0;
            memcpy(&maximum,
                begin + offset
                    + universal_wrapper_profile::kMaximumOffset,
                sizeof(maximum));
            matchedMaximum = maximum;
            ++matchCount;
        }
    }
    if (matchCount != 1)
        return false;
    compiledMaximum = matchedMaximum;
    return true;
}

bool FindSelectiveOtaDlssgWrapper(std::wstring& selectedPath,
    FileVersion& selectedVersion,
    SelectiveOtaDlssgWrapperFailure& failure)
{
    std::wstring programData(32768, L'\0');
    const DWORD length = GetEnvironmentVariableW(
        L"ProgramData", programData.data(),
        static_cast<DWORD>(programData.size()));
    if (length == 0 || length >= programData.size())
    {
        failure = SelectiveOtaDlssgWrapperFailure::eProgramDataUnavailable;
        return false;
    }
    programData.resize(length);

    const std::wstring versionsRoot = JoinPath(
        programData,
        L"NVIDIA\\NGX\\models\\sl_dlss_g_0\\versions");
    WIN32_FIND_DATAW versionEntry{};
    HANDLE versions = FindFirstFileW(
        JoinPath(versionsRoot, L"*").c_str(), &versionEntry);
    if (versions == INVALID_HANDLE_VALUE)
    {
        failure = SelectiveOtaDlssgWrapperFailure::eNoCompatibleCandidate;
        return false;
    }

    bool found = false;
    do
    {
        if ((versionEntry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0
            || wcscmp(versionEntry.cFileName, L".") == 0
            || wcscmp(versionEntry.cFileName, L"..") == 0)
        {
            continue;
        }
        const std::wstring filesRoot = JoinPath(JoinPath(
            versionsRoot, versionEntry.cFileName), L"files");
        WIN32_FIND_DATAW fileEntry{};
        HANDLE files = FindFirstFileW(
            JoinPath(filesRoot, L"*").c_str(), &fileEntry);
        if (files == INVALID_HANDLE_VALUE)
            continue;
        do
        {
            if ((fileEntry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                continue;
            const std::wstring path = JoinPath(filesRoot, fileEntry.cFileName);
            const FileVersion version = ReadFileVersion(path);
            uint32_t compiledMaximum = 0;
            if (!ReadDiskWrapperProfile(path, compiledMaximum))
                continue;
            const selective_ota_wrapper_policy::Version policyVersion{
                version.major, version.minor, version.build,
                version.privatePart};
            if (!selective_ota_wrapper_policy::IsCompatibleCandidate(
                    policyVersion, compiledMaximum))
            {
                continue;
            }
            const selective_ota_wrapper_policy::Version currentVersion{
                selectedVersion.major, selectedVersion.minor,
                selectedVersion.build, selectedVersion.privatePart};
            if (!found || selective_ota_wrapper_policy::IsNewer(
                    policyVersion, currentVersion))
            {
                found = true;
                selectedPath = path;
                selectedVersion = version;
            }
        } while (FindNextFileW(files, &fileEntry));
        FindClose(files);
    } while (FindNextFileW(versions, &versionEntry));
    FindClose(versions);

    failure = found ? SelectiveOtaDlssgWrapperFailure::eNone
        : SelectiveOtaDlssgWrapperFailure::eNoCompatibleCandidate;
    return found;
}

void ConfigureSelectiveOtaDlssgWrapper(bool requested,
    bool providerSupported, bool loaderDiscoveryReady)
{
    gSelectiveOtaDlssgWrapperRequested.store(
        requested, std::memory_order_release);
    gSelectiveOtaDlssgWrapperCandidateReady.store(
        false, std::memory_order_release);
    gSelectiveOtaDlssgWrapperVersionMajor.store(0,
        std::memory_order_relaxed);
    gSelectiveOtaDlssgWrapperVersionMinor.store(0,
        std::memory_order_relaxed);
    gSelectiveOtaDlssgWrapperVersionBuild.store(0,
        std::memory_order_relaxed);
    gSelectiveOtaDlssgWrapperVersionPrivate.store(0,
        std::memory_order_relaxed);
    {
        std::lock_guard lock(gSelectiveOtaDlssgWrapperMutex);
        gSelectiveOtaDlssgWrapperPath.clear();
    }
    if (!requested)
    {
        gSelectiveOtaDlssgWrapperFailure.store(
            static_cast<uint32_t>(
                SelectiveOtaDlssgWrapperFailure::eNone),
            std::memory_order_release);
        return;
    }
    if (!providerSupported)
    {
        gSelectiveOtaDlssgWrapperFailure.store(
            static_cast<uint32_t>(SelectiveOtaDlssgWrapperFailure::
                eProviderUnsupported), std::memory_order_release);
        Log(L"Selective DLSS-G wrapper route suppressed: provider "
            L"preflight failed");
        return;
    }
    if (!loaderDiscoveryReady)
    {
        gSelectiveOtaDlssgWrapperFailure.store(
            static_cast<uint32_t>(SelectiveOtaDlssgWrapperFailure::
                eLoaderDiscoveryUnavailable), std::memory_order_release);
        Log(L"Selective DLSS-G wrapper route suppressed: loader "
            L"interception unavailable before slInit");
        return;
    }

    std::wstring path;
    FileVersion version{};
    SelectiveOtaDlssgWrapperFailure failure =
        SelectiveOtaDlssgWrapperFailure::eNoCompatibleCandidate;
    if (!FindSelectiveOtaDlssgWrapper(path, version, failure))
    {
        gSelectiveOtaDlssgWrapperFailure.store(
            static_cast<uint32_t>(failure), std::memory_order_release);
        Log(L"Selective DLSS-G wrapper route unavailable: failure=%u",
            static_cast<uint32_t>(failure));
        return;
    }

    {
        std::lock_guard lock(gSelectiveOtaDlssgWrapperMutex);
        gSelectiveOtaDlssgWrapperPath = path;
    }
    gSelectiveOtaDlssgWrapperVersionMajor.store(
        version.major, std::memory_order_relaxed);
    gSelectiveOtaDlssgWrapperVersionMinor.store(
        version.minor, std::memory_order_relaxed);
    gSelectiveOtaDlssgWrapperVersionBuild.store(
        version.build, std::memory_order_relaxed);
    gSelectiveOtaDlssgWrapperVersionPrivate.store(
        version.privatePart, std::memory_order_relaxed);
    gSelectiveOtaDlssgWrapperFailure.store(
        static_cast<uint32_t>(SelectiveOtaDlssgWrapperFailure::eNone),
        std::memory_order_release);
    gSelectiveOtaDlssgWrapperCandidateReady.store(
        true, std::memory_order_release);
    Log(L"Selective DLSS-G wrapper candidate ready: version=%u.%u.%u.%u "
        L"compiledMaximum=5 path=%s", version.major, version.minor,
        version.build, version.privatePart, path.c_str());
}

std::wstring SelectiveOtaDlssgWrapperRedirectPath(
    const std::wstring& requestedPath)
{
    if (!gSelectiveOtaDlssgWrapperCandidateReady.load(
            std::memory_order_acquire)
        || !ModuleFileNameEquals(requestedPath, L"sl.dlss_g.dll"))
    {
        return {};
    }

    if (requestedPath.find_first_of(L"\\/") != std::wstring::npos)
    {
        std::wstring absolutePath(32768, L'\0');
        const DWORD length = GetFullPathNameW(requestedPath.c_str(),
            static_cast<DWORD>(absolutePath.size()),
            absolutePath.data(), nullptr);
        if (length == 0 || length >= absolutePath.size())
            return {};
        absolutePath.resize(length);
        if (_wcsicmp(ParentPath(absolutePath).c_str(),
                gExecutableDirectory.c_str()) != 0)
        {
            return {};
        }
    }

    std::lock_guard lock(gSelectiveOtaDlssgWrapperMutex);
    return gSelectiveOtaDlssgWrapperPath;
}

class ScopedModuleReference
{
public:
    explicit ScopedModuleReference(HMODULE module) noexcept
    {
        HMODULE retained = nullptr;
        if (module
            && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                reinterpret_cast<LPCWSTR>(module), &retained)
            && retained == module)
        {
            module_ = retained;
        }
        else if (retained)
        {
            FreeLibrary(retained);
        }
    }

    ~ScopedModuleReference()
    {
        if (module_)
            FreeLibrary(module_);
    }

    ScopedModuleReference(const ScopedModuleReference&) = delete;
    ScopedModuleReference& operator=(const ScopedModuleReference&) = delete;

    explicit operator bool() const noexcept { return module_ != nullptr; }
    HMODULE get() const noexcept { return module_; }

private:
    HMODULE module_ = nullptr;
};

void RecomputeModuleStateLocked()
{
    uint32_t wrapperCandidates = 0;
    uint32_t patchedWrappers = 0;
    uint32_t ngxCandidates = 0;
    uint32_t patchedNgx = 0;
    uint32_t wrapperRouteBits = 0;
    uint32_t ngxRouteBits = 0;
    uint32_t activeCompiledMaximum = 0;
    const uintptr_t activeWrapper =
        gActiveWrapperBase.load(std::memory_order_acquire);
    for (const auto& record : gModuleRecords)
    {
        if (record.wrapperCandidate)
            ++wrapperCandidates;
        if (record.wrapperPatched)
        {
            ++patchedWrappers;
            wrapperRouteBits |= ClassifyLoadedRoute(record.path);
        }
        if (record.wrapperCompiledMaximumGeneratedFrames != 0)
        {
            if (reinterpret_cast<uintptr_t>(record.module)
                == activeWrapper)
            {
                activeCompiledMaximum =
                    record.wrapperCompiledMaximumGeneratedFrames;
            }
        }
        if (record.ngxCandidate)
            ++ngxCandidates;
        if (record.ngxPatched && record.ngxTemporalPatched)
        {
            ++patchedNgx;
            ngxRouteBits |= ClassifyLoadedRoute(record.path);
        }
    }
    gLoadedWrapperCandidates.store(wrapperCandidates, std::memory_order_release);
    gPatchedWrapperCandidates.store(patchedWrappers, std::memory_order_release);
    gLoadedNgxCandidates.store(ngxCandidates, std::memory_order_release);
    gPatchedNgxCandidates.store(patchedNgx, std::memory_order_release);
    gWrapperRouteBits.store(wrapperRouteBits, std::memory_order_release);
    gNgxRouteBits.store(ngxRouteBits, std::memory_order_release);
    // Candidate discovery is not activity. Do not publish a capacity from an
    // inactive local/OTA wrapper; the UI remains in "detecting" until a real
    // setter/state call selects one generation.
    const uint32_t selectedCompiledMaximum = activeWrapper != 0
        ? activeCompiledMaximum : 0u;
    gWrapperCompiledMaximumGeneratedFrames.store(
        selectedCompiledMaximum, std::memory_order_release);
    gSafeMaximumMultiplier.store(
        std::clamp(CurrentCapacityDecision().effectiveMaximumMultiplier,
            kMinimumMultiplier, kMaximumMultiplier),
        std::memory_order_release);
}

void LogModuleInventory(const ModuleRecord& record)
{
    if (!record.wrapperExport && !record.ngxExport
        && !record.ngxRuntimeExport)
        return;
    Log(L"Loaded module: wrapperExport=%d wrapperCandidate=%d wrapperPatched=%d "
        L"wrapperCompiledMaximum=%u "
        L"ngxExport=%d d3d12=%d vulkan=%d ngxCandidate=%d ngxPatched=%d "
        L"midpointPatched=%d ngxRuntimeExport=%d runtimeD3D12=%d "
        L"runtimeVulkan=%d path=%s",
        record.wrapperExport, record.wrapperCandidate, record.wrapperPatched,
        record.wrapperCompiledMaximumGeneratedFrames,
        record.ngxExport, record.ngxD3D12Export, record.ngxVulkanExport,
        record.ngxCandidate, record.ngxPatched,
        record.ngxTemporalPatched, record.ngxRuntimeExport,
        record.ngxRuntimeD3D12Export, record.ngxRuntimeVulkanExport,
        record.path.c_str());
}

ModuleRecord InspectLoadedModule(HMODULE module, const std::wstring& suppliedPath)
{
    if (!module)
        return {};
    // Toolhelp snapshots do not retain module references. NVIDIA's transient
    // NGX model DLLs can unload between snapshot enumeration and the pattern
    // scan, leaving a stale executable-section pointer. Acquire a loader
    // reference before reading any PE data and keep it for the full inspection.
    ScopedModuleReference retained(module);
    if (!retained)
        return {};
    module = retained.get();
    const std::wstring livePath = LoadedModulePath(module);
    const std::wstring path = livePath.empty() ? suppliedPath : livePath;
    if (ModuleFileNameEquals(path, L"sl.interposer.dll"))
    {
        TryInstallSlInitEntryDetour(module, nullptr);
        InstallStreamlineLoaderDiscovery(module, path);
        InstallSlCommonResolverDiscovery(module, path);
    }
    if (ModuleFileNameEquals(path, L"sl.common.dll"))
    {
        InstallStreamlineLoaderDiscovery(module, path);
        InstallSlCommonResolverDiscovery(module, path);
    }
    ModuleRecord snapshot{};
    bool logInventory = false;
    {
        std::lock_guard lock(gModuleMutex);
        const auto existing = std::find_if(gModuleRecords.begin(), gModuleRecords.end(),
            [&](const ModuleRecord& record) {
                return record.module == module
                    && _wcsicmp(record.path.c_str(), path.c_str()) == 0;
            });
        if (existing != gModuleRecords.end())
        {
            if (existing->wrapperCandidate && !existing->wrapperPatched)
            {
                const PatternPatchResult result =
                    PatchUniqueWrapperMaximumPattern(
                        module, path);
                existing->wrapperCandidate = result.candidate;
                existing->wrapperPatched = result.patched;
                existing->wrapperCompiledMaximumGeneratedFrames =
                    result.profileMaximum;
                if (existing->wrapperCandidate)
                {
                    existing->controlRouteSlot = EnsureControlRoute(
                        existing->module, existing->path,
                        existing->generation, existing->wrapperPatched,
                        existing->wrapperCompiledMaximumGeneratedFrames);
                }
                RecomputeModuleStateLocked();
            }
            if (gpu_dispatch::IsAda() && existing->ngxExport && !existing->ngxPatched)
            {
                const auto result = PatchNgxMfgGate(module, path);
                existing->ngxCandidate = result.candidate;
                existing->ngxPatched = result.patched;
                RecomputeModuleStateLocked();
            }
            if (gLogReady.load(std::memory_order_acquire) && !existing->inventoryLogged)
            {
                existing->inventoryLogged = true;
                logInventory = true;
            }
            snapshot = *existing;
        }
        else
        {
            ModuleRecord record{};
            record.module = module;
            record.path = path;
            record.generation = gNextModuleGeneration.fetch_add(
                1, std::memory_order_relaxed);
            record.freshLoadToken = gOutputPullMaskLoads.Current(
                reinterpret_cast<uintptr_t>(module));
            record.wrapperExport = ModuleExportsFunction(module, "slGetPluginFunction");
            record.ngxD3D12Export = ExportsNgxD3D12Route(module);
            record.ngxVulkanExport = ExportsNgxVulkanRoute(module);
            record.ngxExport =
                dlssg_provider_policy::IsDlssgImplementationModule(module)
                && (record.ngxD3D12Export || record.ngxVulkanExport)
                && ModuleExportsFunction(
                    module, "NVSDK_NGX_GetGPUArchitecture");
            record.ngxRuntimeExport = IsNgxRuntimeModule(module);
            record.ngxRuntimeD3D12Export = record.ngxRuntimeExport
                && record.ngxD3D12Export;
            record.ngxRuntimeVulkanExport = record.ngxRuntimeExport
                && record.ngxVulkanExport;
            if (!record.wrapperExport && !record.ngxExport
                && !record.ngxRuntimeExport)
                return record;
            // Wrapper and provider eligibility are independent. A game-local
            // or NVIDIA-cache wrapper which has exactly one recognized native
            // 1/3/5-frame clamp is patched immediately. Unknown or ambiguous
            // wrapper layouts remain untouched; the separately loaded NGX
            // provider still has to pass its own identity/version checks.
            if (record.wrapperExport)
            {
                const PatternPatchResult result =
                    PatchUniqueWrapperMaximumPattern(module, path);
                record.wrapperCandidate = result.candidate;
                record.wrapperPatched = result.patched;
                record.wrapperCompiledMaximumGeneratedFrames =
                    result.profileMaximum;
                if (record.wrapperCandidate)
                {
                    record.controlRouteSlot = EnsureControlRoute(
                        record.module, record.path, record.generation,
                        record.wrapperPatched,
                        record.wrapperCompiledMaximumGeneratedFrames);
                }
            }
            if (record.ngxExport)
            {
                const PatternPatchResult result = gpu_dispatch::IsAda()
                    ? PatchNgxMfgGate(module, path)
                    : PatternPatchResult{true, false, nullptr};
                record.ngxCandidate = result.candidate;
                record.ngxPatched = result.patched;
            }
            record.inventoryLogged = gLogReady.load(std::memory_order_acquire);
            logInventory = record.inventoryLogged;
            gModuleRecords.push_back(record);
            RecomputeModuleStateLocked();
            snapshot = record;
        }
    }
    // A Streamline process can contain an on-disk wrapper plus a later NVIDIA
    // override wrapper. Only the uniquely signature-verified DLSS-G candidate
    // is safe to pin and detour during passive discovery. The generic
    // slGetPluginFunction export is shared by inactive feature modules and is
    // not proof that their returned DLSS-G entry belongs to the active route.
    ngx_initialization::ObserveModule(snapshot.module, snapshot.generation, &PrepareNgxInitialization);
    ampere_backend::SetPreparationBoundaryResolver(&ResolveAmperePreparationBoundary);
    ampere_backend::ObserveModule(snapshot.module, snapshot.path.c_str(), snapshot.generation,
        snapshot.wrapperCandidate, snapshot.ngxExport, snapshot.ngxRuntimeExport);

    if (snapshot.wrapperExport)
    {
        // NVIDIA OTA plugins keep their Streamline identity but use opaque
        // cache filenames. Continue discovery structurally through every
        // Streamline plugin so the next provider is patched on loader return,
        // before that plugin can resolve and cache its NGX entry points.
        InstallStreamlineLoaderDiscovery(snapshot.module, snapshot.path);
        InstallSlCommonResolverDiscovery(snapshot.module, snapshot.path);
    }
    if (snapshot.wrapperCandidate)
    {
        InstallControlRouteEntries(snapshot.controlRouteSlot);
    }
    if (snapshot.ngxExport)
    {
        TryInstallNgxCreateEntryDetour(
            snapshot.module, snapshot.path, snapshot.generation);
        TryInstallNgxEvaluateEntryDetour(
            snapshot.module, snapshot.path, snapshot.generation);
        TryInstallNgxVulkanCreateEntryDetours(
            snapshot.module, snapshot.path, snapshot.generation);
        TryInstallNgxVulkanEvaluateEntryDetour(
            snapshot.module, snapshot.path, snapshot.generation);
        TryInstallNgxVulkanAdapterEntryDetours(
            snapshot.module, snapshot.path, snapshot.generation);
        if (gpu_dispatch::IsAda()) dlssg_preset::ObserveProvider(snapshot.module, snapshot.generation);
        if (logInventory)
        {
            const auto preset = dlssg_preset::ReadSnapshot(snapshot.module);
            Log(L"DLSS-G cached preset reader: installed=%d failure=%u "
                L"targetRva=0x%X path=%s", preset.cachedReaderInstalled,
                preset.cachedReaderFailure, preset.cachedReaderRva, snapshot.path.c_str());
        }
    }
    if (snapshot.ngxRuntimeExport)
    {
        // Embedded NGX hosts load providers themselves. Cover their loader
        // return and resolver boundary before they cache provider functions.
        InstallStreamlineLoaderDiscovery(snapshot.module, snapshot.path);
        InstallSlCommonResolverDiscovery(snapshot.module, snapshot.path);
        if (ngx_runtime_policy::IsRemixRuntime(snapshot.module))
        {
            HMODULE pinned = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                    | GET_MODULE_HANDLE_EX_FLAG_PIN,
                    reinterpret_cast<LPCWSTR>(snapshot.module), &pinned)
                && pinned == snapshot.module)
            {
                const uintptr_t base = reinterpret_cast<uintptr_t>(pinned);
                uintptr_t previous = 0;
                if (gRemixRuntimeBase.compare_exchange_strong(previous, base,
                        std::memory_order_acq_rel))
                    Log(L"RTX Remix embedded Vulkan NGX runtime detected; "
                        L"FG controls and resource capacity remain game-managed: %s",
                        snapshot.path.c_str());
                else if (previous != base)
                    gRemixRuntimeBase.store(UINTPTR_MAX, std::memory_order_release);
            }
        }
        TryInstallNgxRuntimeCreateEntryDetour(
            snapshot.module, snapshot.path, snapshot.generation);
        TryInstallNgxRuntimeEvaluateEntryDetour(
            snapshot.module, snapshot.path, snapshot.generation);
        TryInstallNgxRuntimeVulkanCreateEntryDetours(
            snapshot.module, snapshot.path, snapshot.generation);
        TryInstallNgxRuntimeVulkanEvaluateEntryDetour(
            snapshot.module, snapshot.path, snapshot.generation);
        TryInstallNgxVulkanAdapterEntryDetours(
            snapshot.module, snapshot.path, snapshot.generation);
    }
    if (logInventory)
        LogModuleInventory(snapshot);
    return snapshot;
}

void FlushModuleInventoryToLog()
{
    std::vector<ModuleRecord> records;
    {
        std::lock_guard lock(gModuleMutex);
        for (auto& record : gModuleRecords)
        {
            if (!record.inventoryLogged)
            {
                record.inventoryLogged = true;
                records.push_back(record);
            }
        }
    }
    for (const auto& record : records)
        LogModuleInventory(record);
}

void RemoveLoadedModule(const ModuleRecord& expected)
{
    const HMODULE module = expected.module;
    if (!module || !expected.generation)
        return;
    // Toolhelp is a point-in-time inventory. Inspection can load and register
    // more modules, so absence from that inventory is not unload evidence.
    // Take the loader reference outside gModuleMutex to avoid lock inversion.
    ScopedModuleReference retained(module);
    if (retained && _wcsicmp(LoadedModulePath(module).c_str(),
            expected.path.c_str()) == 0)
        return;
    {
        std::lock_guard lock(gModuleMutex);
        const auto found = std::find_if(gModuleRecords.begin(), gModuleRecords.end(),
            [&](const ModuleRecord& record) {
                return record.module == module
                    && record.generation == expected.generation
                    && _wcsicmp(record.path.c_str(), expected.path.c_str()) == 0;
            });
        if (found == gModuleRecords.end())
            return;
        gModuleRecords.erase(found);
        RecomputeModuleStateLocked();
        // A newer observation at the same address owns its own route state.
        if (std::any_of(gModuleRecords.begin(), gModuleRecords.end(),
                [&](const ModuleRecord& record) { return record.module == module; }))
            return;
    }
    // A route may be selected after inventory retirement releases gModuleMutex.
    // Retire its slot and metadata as one transaction only for this generation.
    std::lock_guard publicationLock(gActiveControlRouteMutex);
    if (ControlRouteRecord* route = ActiveControlRoute();
        route && route->wrapper == module && route->generation == expected.generation)
    {
        gActiveControlRouteSlot.store(UINT32_MAX,
            std::memory_order_release);
        SetUniversalRouteFailure(UniversalRouteFailure::eNoActiveRoute);
        gActiveWrapperPatched.store(false, std::memory_order_release);
        gActiveWrapperObserved.store(false, std::memory_order_release);
        gActiveWrapperBase.store(0, std::memory_order_release);
        gActiveWrapperUsesNvidiaOta.store(false, std::memory_order_release);
        gActiveWrapperVersionMajor.store(0, std::memory_order_release);
        gActiveWrapperVersionMinor.store(0, std::memory_order_release);
        gActiveWrapperVersionBuild.store(0, std::memory_order_release);
        gActiveWrapperVersionPrivate.store(0, std::memory_order_release);
    }
}

void InspectAlreadyLoadedModules()
{
    // Only consider observations which predate the snapshot for retirement.
    std::vector<ModuleRecord> previousRecords;
    {
        std::lock_guard lock(gModuleMutex);
        previousRecords = gModuleRecords;
    }
    HANDLE snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        Log(L"Could not enumerate loaded modules (%lu)", GetLastError());
        return;
    }

    std::vector<HMODULE> loadedModules;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry))
    {
        do
        {
            HMODULE module = reinterpret_cast<HMODULE>(entry.modBaseAddr);
            loadedModules.push_back(module);
#if defined(MFG_UNLOCK_SINGLE_MODULE_UI)
            single_overlay::application_imports::ArmModule(module);
#endif
            InspectLoadedModule(module, entry.szExePath);
            entry.dwSize = sizeof(entry);
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);

    for (const ModuleRecord& record : previousRecords)
    {
        if (std::find(loadedModules.begin(), loadedModules.end(),
                record.module) == loadedModules.end())
            RemoveLoadedModule(record);
    }
}

struct MfgLdrDllLoadedNotificationData
{
    ULONG flags;
    const UNICODE_STRING* fullDllName;
    const UNICODE_STRING* baseDllName;
    PVOID dllBase;
    ULONG sizeOfImage;
};

union MfgLdrDllNotificationData
{
    MfgLdrDllLoadedNotificationData loaded;
    MfgLdrDllLoadedNotificationData unloaded;
};

using MfgLdrDllNotificationFunction = void (CALLBACK*)(
    ULONG reason, const MfgLdrDllNotificationData* data, void* context);
using LdrRegisterDllNotificationFn = NTSTATUS (NTAPI*)(
    ULONG flags, MfgLdrDllNotificationFunction callback, void* context, void** cookie);

void CALLBACK OnDllNotification(
    ULONG reason, const MfgLdrDllNotificationData* data, void*)
{
    static constexpr ULONG kDllLoaded = 1;
    static constexpr ULONG kDllUnloaded = 2;
    if (data && (reason == kDllLoaded || reason == kDllUnloaded))
    {
#if MFG_UNLOCK_OUTPUT_PULL_MASK_ONLY || MFG_UNLOCK_RUNTIME_GPU_SELECTION || MFG_UNLOCK_AMPERE_MFG
        const uintptr_t module = reinterpret_cast<uintptr_t>(
            reason == kDllLoaded ? data->loaded.dllBase : data->unloaded.dllBase);
        if (reason == kDllLoaded)
            gOutputPullMaskLoads.Loaded(module);
        else
            gOutputPullMaskLoads.Unloaded(module);
#endif
        gModuleInventoryDirty.store(true, std::memory_order_release);
    }
}

bool RegisterDllNotification()
{
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto* registerNotification = ntdll ? reinterpret_cast<LdrRegisterDllNotificationFn>(
        GetProcAddress(ntdll, "LdrRegisterDllNotification")) : nullptr;
    if (!registerNotification)
        return false;

    void* cookie = nullptr;
    const NTSTATUS status = registerNotification(0, &OnDllNotification, nullptr, &cookie);
    const bool registered = status >= 0 && cookie != nullptr;
    gDllNotificationRegistered.store(registered, std::memory_order_release);
    return registered;
}

DWORD WINAPI PatchWorker(void* context)
{
    EnsureAmpereFeatureLifetimeObserver();
    const DWORD pid = GetCurrentProcessId();
    wchar_t tempDirectory[MAX_PATH]{};
    const DWORD tempLength = GetTempPathW(_countof(tempDirectory), tempDirectory);
    if (!tempLength || tempLength >= _countof(tempDirectory))
        tempDirectory[0] = L'\0';
    const std::wstring logPath=diagnostic_paths::RuntimeLog();
    if (!logPath.empty())
    {
        gLog = _wfsopen(logPath.c_str(), L"w, ccs=UTF-8", _SH_DENYWR);
    }
    gLogReady.store(gLog != nullptr, std::memory_order_release);
#if defined(MFG_UNLOCK_SINGLE_MODULE_UI)
    single_module::DrainLog(&MidpointLog);
#endif
    fault_capture::Initialize(&MidpointLog);

    const std::wstring mappingName = MfgUnlockObjectName(L"Status", pid);
    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mappingName.c_str());
    auto* shared = mapping ? static_cast<MfgUnlockStatus*>(
        MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(MfgUnlockStatus))) : nullptr;

    const std::wstring eventName = MfgUnlockObjectName(L"Ready", pid);
    HANDLE readyEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName.c_str());

    std::wstring executablePath(32768, L'\0');
    const DWORD executableLength = GetModuleFileNameW(nullptr,
        executablePath.data(), static_cast<DWORD>(executablePath.size()));
    executablePath.resize(executableLength < executablePath.size()
        ? executableLength : 0);
    const std::wstring executableDirectory = ParentPath(executablePath);
    if (gExecutablePath.empty())
        gExecutablePath = executablePath;
    ResolveNvidiaCompatibilityPolicy();
    if (gConfigPath.empty())
    {
        gConfigPath = ResolveConfigPath(
            static_cast<HMODULE>(context), executableDirectory);
    }
    gStatusPath = ResolveStatusPath(gConfigPath, executableDirectory);
    // The first atomic publication replaces the previous launch's snapshot.
    // Never delete a file another live process may still own.
    temporal_interval_trace::Initialize(tempDirectory, executablePath.c_str());
#if MFG_UNLOCK_OUTPUT_PULL_TELEMETRY
    output_pull_telemetry::Initialize(tempDirectory, pid);
#endif
    const ControlConfig initialControl = ReadInitialControl();
    StoreControl(initialControl);
    FILETIME configWriteTime{};
    ReadLastWriteTime(gConfigPath, configWriteTime);
    Log(L"Initial control: mode=%s multiplier=%ux dynamicTarget=%u FPS "
        L"dynamicExperimental56=%d generatedOnlyDebug=%d "
        L"intervalLogging=%d selectiveOtaDlssgWrapper=%d; config: %s",
        initialControl.followGame ? L"follow"
            : initialControl.dynamic ? L"dynamic" : L"fixed",
        initialControl.multiplier,
        initialControl.dynamicTargetFrameRate, initialControl.dynamicExperimental56,
        initialControl.generatedOnlyDebug, initialControl.intervalLogging,
        initialControl.selectiveOtaDlssgWrapper,
        gConfigPath.c_str());
    if (initialControl.intervalLogging)
    {
        Log(L"NGX temporal-request interval trace enabled: %s",
            temporal_interval_trace::FilePath());
    }

    Log(L"Patch worker started for PID %lu", static_cast<unsigned long>(pid));
    Log(L"Diagnostics reused per executable: log=%s status=%s processBirth=%llu executable=%s",
        logPath.c_str(),gStatusPath.c_str(),static_cast<unsigned long long>(diagnostic_paths::ProcessBirth()),executablePath.c_str());
#if defined(MFG_UNLOCK_SINGLE_MODULE_UI)
    Log(L"RTXMFG build=1.3.3-hotfix.2 outputPullMask=%d occupancyHint=%d "
        L"uiInputs=framed-observations uiRecomposition=game-managed",
        MFG_UNLOCK_OUTPUT_PULL_MASK_ONLY,
        MFG_UNLOCK_OUTPUT_PULL_MASK_OCCUPANCY);
#endif
#if MFG_UNLOCK_OUTPUT_PULL_MASK_ONLY
    // Publication can precede worker/log startup. Preserve the one-time result
    // here as well as at Create so early failures remain locally diagnosable.
    const auto maskStartup = midpoint_fix::ReadOutputPullMaskSnapshot();
    Log(L"OUTPUT_PULL_MASK startup attempted=%d ready=%d published=%d "
        L"failure=%u publicationAttempted=%d replacementWasVisible=%d "
        L"unsafePublication=%d requiresRestart=%d firstCreate=%d "
        L"mayPredate=%d earlyInit=%d sourceSha256=%hs "
        L"selectedSha256=%hs",
        maskStartup.attempted, maskStartup.ready, maskStartup.published,
        static_cast<unsigned>(maskStartup.failure),
        maskStartup.publicationAttempted, maskStartup.replacementWasVisible,
        maskStartup.unsafePublication, maskStartup.requiresRestart,
        maskStartup.firstPipelineCreate, maskStartup.pipelineMayPredateDetour,
        maskStartup.earlyInitObserved,
        maskStartup.sourceSha256, maskStartup.selectedSha256);
#endif
    Log(L"NVIDIA compatibility profile: resolved=%d status=%d profile=%hs "
        L"tier=%hs manifestEntries=%u fetched=%hs sha256=%hs",
        gNvidiaCompatibilityResolved.load(std::memory_order_acquire),
        gNvidiaProfileStatus.load(std::memory_order_relaxed),
        gNvidiaProfileName.c_str(),
        nvidia_mfg_policy::TierName(
            static_cast<nvidia_mfg_policy::Tier>(
                gNvidiaCompatibilityTier.load(std::memory_order_relaxed))),
        nvidia_mfg_policy::ManifestEntryCount(),
        nvidia_mfg_policy::ManifestFetchedDate(),
        nvidia_mfg_policy::ManifestSha256());
    Log(L"Early DLL notification registered: %d",
        gDllNotificationRegistered.load(std::memory_order_acquire));
    Log(L"Streamline slInit control path: entryCurrent=%d iatFallback=%d "
        L"resolverDiscovery=%d calls=%llu",
        entry_detour::ReadSnapshot(
            entry_detour::Kind::eSlInit).current,
        gSlInitIatFallbackInstalled.load(std::memory_order_acquire),
        gMainResolverDiscoveryInstalled.load(std::memory_order_acquire),
        static_cast<unsigned long long>(
            gSlInitCalls.load(std::memory_order_acquire)));
    const bool liveHookInstalled = InstallFeatureFunctionHook();
    gLiveHookInstalled.store(liveHookInstalled, std::memory_order_release);
    Log(L"Streamline feature-function interception installed: %d", liveHookInstalled);
    Log(L"Streamline D3D device interception installed: %d",
        InstallD3DDeviceHook());
    Log(L"Streamline Vulkan info interception installed: %d",
        InstallVulkanInfoHook());
    const bool uiTagHookInstalled = InstallUiTagHooks();
    Log(L"Streamline UI tag interception installed: %d", uiTagHookInstalled);
    InspectAlreadyLoadedModules();
    FlushModuleInventoryToLog();
    Log(L"Loaded-module discovery initialized: ready=%d route=%hs wrappers=%u/%u ngx=%u/%u",
        BridgeReady(), PatchRouteName(),
        gPatchedWrapperCandidates.load(std::memory_order_relaxed),
        gLoadedWrapperCandidates.load(std::memory_order_relaxed),
        gPatchedNgxCandidates.load(std::memory_order_relaxed),
        gLoadedNgxCandidates.load(std::memory_order_relaxed));

    if (shared)
    {
        shared->magic = kMfgUnlockStatusMagic;
        shared->win32Error = liveHookInstalled ? ERROR_SUCCESS : ERROR_PROC_NOT_FOUND;
        shared->wrapperPatchCount = static_cast<LONG>(
            gPatchedWrapperCandidates.load(std::memory_order_relaxed));
        shared->ngxPatchCount = static_cast<LONG>(
            gPatchedNgxCandidates.load(std::memory_order_relaxed));
        if (!logPath.empty())
            wcsncpy_s(shared->logPath, logPath.c_str(), _TRUNCATE);
        InterlockedExchange(&shared->state,
            BridgeReady() ? 1 : liveHookInstalled ? 0 : -1);
    }
    if (readyEvent)
        SetEvent(readyEvent);

    if (shared)
        UnmapViewOfFile(shared);
    if (mapping)
        CloseHandle(mapping);
    if (readyEvent)
        CloseHandle(readyEvent);
    PublishPatchRoute();
    PublishLiveBridge(initialControl);
    ControlConfig activeControl = initialControl;
    reflex_control::SetRuntimeValidator(&ReflexRuntimeCurrent);
    vsync_control::SetRuntimeValidator(&VsyncRuntimeCurrent);
    DiscoverReflexModule();
    UpdatePresentationPolicy(activeControl);
    if (!WriteBridgeStatus(activeControl, pid))
        Log(L"Could not publish CET bridge status file: %s", gStatusPath.c_str());

    // The active frontend writes its selected config path when the user changes
    // the mode. Watch it off the presenting thread and atomically publish
    // changes for the SetOptions hook.
    uint32_t heartbeatTicks = 0;
    uint32_t inventoryTicks = 0;
    bool previousReady = BridgeReady();
    std::string previousRoute = PatchRouteName();
    for (;;)
    {
        Sleep(100);
#if defined(MFG_UNLOCK_SINGLE_MODULE_UI)
        single_module::DrainLog(&MidpointLog);
#endif
        ++inventoryTicks;
        if (gModuleInventoryDirty.exchange(false, std::memory_order_acq_rel))
        {
            inventoryTicks = 0;
            InspectAlreadyLoadedModules();
            DiscoverReflexModule();
        }
        FILETIME latestWriteTime{};
        if (ReadLastWriteTime(gConfigPath, latestWriteTime)
            && CompareFileTime(&latestWriteTime, &configWriteTime) != 0)
        {
            configWriteTime = latestWriteTime;
            ControlConfig control{};
            if (!ReadControlFile(gConfigPath, control))
            {
                Log(L"Ignored an invalid live control config update");
            }
            else
            {
                const bool mfgChanged = !SameMfgControl(activeControl, control);
                activeControl = control;
                if (mfgChanged)
                    StoreControl(activeControl);
                else
                    temporal_interval_trace::SetEnabled(gpu_dispatch::IsAda() || UseAmpere()
                        || activeControl.intervalLogging);
                PublishLiveBridge(activeControl);
                WriteBridgeStatus(activeControl, pid);
                Log(L"Live control requested: mode=%s multiplier=%ux dynamicTarget=%u FPS "
                    L"dynamicExperimental56=%d generatedOnlyDebug=%d "
                    L"intervalLogging=%d mfgChanged=%d",
                    activeControl.followGame ? L"follow"
                        : activeControl.dynamic ? L"dynamic" : L"fixed",
                    activeControl.multiplier,
                    activeControl.dynamicTargetFrameRate,
                    activeControl.dynamicExperimental56,
                    activeControl.generatedOnlyDebug,
                    activeControl.intervalLogging, mfgChanged);
            }
        }

        UpdatePresentationPolicy(activeControl);
        temporal_interval_trace::Flush();
#if MFG_UNLOCK_OUTPUT_PULL_TELEMETRY
        output_pull_telemetry::Flush();
#endif

        const bool ready = BridgeReady();
        const std::string route = PatchRouteName();
        if (ready != previousReady || route != previousRoute)
        {
            previousReady = ready;
            previousRoute = route;
            PublishPatchRoute();
            WriteBridgeStatus(activeControl, pid);
            Log(L"Bridge readiness changed: ready=%d route=%hs wrappers=%u/%u ngx=%u/%u",
                ready, route.c_str(),
                gPatchedWrapperCandidates.load(std::memory_order_relaxed),
                gLoadedWrapperCandidates.load(std::memory_order_relaxed),
                gPatchedNgxCandidates.load(std::memory_order_relaxed),
                gLoadedNgxCandidates.load(std::memory_order_relaxed));
        }

        if (++heartbeatTicks >= 10)
        {
            WriteBridgeStatus(activeControl, pid);
            heartbeatTicks = 0;
        }
    }
}
}

BOOL SampleFrameTelemetry(uint64_t owner, uint32_t presentCount,
    bool outputAvailable, bool cumulative)
{
    gFpsOutputPresentTick.store(GetTickCount64(), std::memory_order_release);
    std::lock_guard telemetryLock(gFpsTelemetryMutex);
    if (!gAppliedFrameGenerationOn.load(std::memory_order_acquire))
    {
        if (gFpsTelemetryActive)
            ResetFpsTelemetry();
        gFpsTelemetryActive = false;
        gFpsPresentCounter = {};
        return FALSE;
    }

    const temporal_interval_trace::Snapshot intervalTrace =
        temporal_interval_trace::ReadSnapshot();
    if (!intervalTrace.initialized || !intervalTrace.enabled)
    {
        if (gFpsTelemetryActive)
            ResetFpsTelemetry();
        gFpsTelemetryActive = false;
        gFpsPresentCounter = {};
        return FALSE;
    }

    if (!gFpsTelemetryActive)
    {
        ResetFpsTelemetry();
        gFpsPresentCounter = {};
        gFpsTelemetryActive = true;
    }

    if (cumulative)
    {
        const auto delta = gFpsPresentCounter.Sample(owner, presentCount,
            GetTickCount64(), outputAvailable);
        if (delta.resetWindow) ResetFpsTelemetry();
        UpdateFpsTelemetryForOutputPresent(delta.frames, delta.available, 2u);
    }
    else
        UpdateFpsTelemetryForOutputPresent(1u, outputAvailable, 1u);
    return TRUE;
}

void frame_telemetry::SamplePresentCounter(
    uint64_t owner, uint32_t count, bool available)
{
    SampleFrameTelemetry(owner, count, available, true);
}

extern "C" __declspec(dllexport) BOOL WINAPI MfgUnlockSampleFrameTelemetry()
{
#if defined(MFG_UNLOCK_SINGLE_MODULE_UI)
    // A Vulkan/application callback alone does not count generated presents.
    return SampleFrameTelemetry(0, 0, false, true);
#else
    return SampleFrameTelemetry(0, 0, true, false);
#endif
}

extern "C" __declspec(dllexport) BOOL WINAPI MfgUnlockCoreLoaded()
{
    return TRUE;
}

#if defined(MFG_UNLOCK_SINGLE_MODULE_UI)
extern "C" BOOL WINAPI MfgUnlockCoreEntry(HINSTANCE instance, DWORD reason, LPVOID)
#else
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
#endif
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        const DWORD bypassTlsIndex = TlsAlloc();
        if (bypassTlsIndex == TLS_OUT_OF_INDEXES)
            return FALSE;
        gInternalControlBypassTlsIndex.store(
            bypassTlsIndex, std::memory_order_release);
        // Dynamic TLS does not require thread attach notifications. Some host
        // loader states can still reject this optimization; DllMain's frame is
        // deliberately kept small so an unexpected notification remains safe.
        DisableThreadLibraryCalls(instance);
        gpu_backend::SetLogCallback(&MidpointLog);
        gExecutablePathBuffer.fill(L'\0');
        GetModuleFileNameW(nullptr, gExecutablePathBuffer.data(),
            static_cast<DWORD>(gExecutablePathBuffer.size()));
        gExecutablePathBuffer.back() = L'\0';
        gExecutablePath = gExecutablePathBuffer.data();
        gExecutableDirectory = ParentPath(gExecutablePathBuffer.data());
        // The slInit detour runs before the worker thread in early loaders.
        // Resolve the startup-only wrapper policy now so HookSlInit can read
        // the same control file before Streamline selects its plugins.
        gConfigPath = ResolveConfigPath(instance, gExecutableDirectory);
        dlssg_preset::SetInitialSelectionLoader(&ReadInitialDlssgPreset);
        if (HMODULE interposer = GetModuleHandleW(L"sl.interposer.dll"))
        {
            const std::wstring path = LoadedModulePath(interposer);
            InstallStreamlineLoaderDiscovery(interposer, path);
            InstallSlCommonResolverDiscovery(interposer, path);
        }
        InstallSlInitControlPath();
        gLiveHookInstalled.store(InstallFeatureFunctionHook(), std::memory_order_release);
        InstallD3DDeviceHook();
        InstallVulkanInfoHook();
        InstallUiTagHooks();
        // The imported core is initialized before the loader-facing ASI's
        // DllMain. Install the bounded entry hooks synchronously when their
        // modules already exist, closing the worker-start race for pointers
        // which Streamline cached before this plugin was discovered.
        if (HMODULE common = GetModuleHandleW(L"sl.common.dll"))
        {
            InstallSlCommonResolverDiscovery(
                common, LoadedModulePath(common));
        }
        if (HMODULE provider = GetModuleHandleW(L"nvngx_dlssg.dll"))
        {
            InspectLoadedModule(provider, LoadedModulePath(provider));
        }
        if (HMODULE runtime = GetModuleHandleW(L"_nvngx.dll"))
        {
            InspectLoadedModule(runtime, LoadedModulePath(runtime));
        }
        RegisterDllNotification();
        HANDLE thread = CreateThread(nullptr, 0, PatchWorker, instance, 0, nullptr);
        if (thread)
            CloseHandle(thread);
    }
    return TRUE;
}
