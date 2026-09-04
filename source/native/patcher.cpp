#include "shared.h"
#include "midpoint_fix.h"
#include "dlssg_provider_policy.h"
#include "entry_detour.h"
#include "nvidia_mfg_policy.h"
#include "selective_ota_wrapper_policy.h"
#include "streamline_ota_policy.h"
#include "temporal_interval_trace.h"
#include "universal_route_policy.h"
#include "universal_wrapper_profile.h"

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
std::atomic<uint32_t> gDesiredMultiplier{2};
std::atomic<bool> gDesiredDynamicMode{false};
std::atomic<uint32_t> gDynamicTargetFrameRate{0};
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
std::atomic<bool> gGameFrameGenerationOn{false};
std::atomic<int32_t> gLastSetOptionsResult{static_cast<int32_t>(sl::Result::eErrorNotInitialized)};
std::atomic<int32_t> gLastGetStateResult{static_cast<int32_t>(sl::Result::eErrorNotInitialized)};
std::atomic<bool> gAppliedDynamicMode{false};
std::atomic<uint32_t> gAppliedMultiplier{0};
std::atomic<uint32_t> gAppliedDynamicTargetFrameRate{0};
std::atomic<bool> gAppliedDynamicExperimental56{false};
std::atomic<bool> gAppliedGeneratedOnlyDebug{false};
std::atomic<uint32_t> gActualFramesPresented{0};
std::atomic<uint32_t> gNumFramesToGenerateMax{0};
std::atomic<uint32_t> gDlssgStatus{0};
std::atomic<bool> gDynamicMfgSupported{false};
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
std::atomic<bool> gUiInputsReady{false};
std::atomic<bool> gAppliedUiRecompositionEnabled{false};
std::atomic<bool> gAppliedUiRecompositionForced{false};
std::atomic<uint64_t> gSetTagCalls{0};
std::atomic<uint64_t> gSetTagForFrameCalls{0};
std::atomic<uint32_t> gRealFpsMilli{0};
std::atomic<uint32_t> gDlssFpsMilli{0};
std::atomic<uint32_t> gFpsSampleWindowMs{0};
std::atomic<uint64_t> gFpsSampleTick{0};
std::atomic<uint64_t> gFpsOutputPresentTick{0};
std::atomic<bool> gLogReady{false};
std::mutex gStreamlineCallMutex;
std::mutex gLastOptionsMutex;
std::mutex gModuleMutex;
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
constexpr uint64_t kUiTagFreshnessMs = 2500;

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
    uint32_t multiplier = 2;
    bool dynamic = false;
    uint32_t dynamicTargetFrameRate = 0;
    bool dynamicExperimental56 = false;
    bool generatedOnlyDebug = false;
    bool intervalLogging = true;
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
    bool wrapperPatched = false;
    uint32_t compiledMaximumGeneratedFrames = 0;

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

struct UiResourceTagState
{
    bool active = false;
    sl::ResourceLifecycle lifecycle = sl::ResourceLifecycle::eOnlyValidNow;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t top = 0;
    uint32_t left = 0;
    uint32_t format = 0;
    uint64_t lastSeenTick = 0;
};

struct UiViewportTagState
{
    uint32_t viewport = UINT32_MAX;
    UiResourceTagState hudless{};
    UiResourceTagState uiAlpha{};
    UiResourceTagState uiColorAlpha{};
};

struct UiInputSnapshot
{
    bool hudless = false;
    bool uiAlpha = false;
    bool uiColorAlpha = false;
    bool dimensionsKnown = false;
    bool dimensionsMatch = false;
    bool ready = false;
    uint32_t hudlessWidth = 0;
    uint32_t hudlessHeight = 0;
    uint32_t uiWidth = 0;
    uint32_t uiHeight = 0;
    uint32_t uiFormat = 0;
    uint64_t oldestAgeMs = 0;
};

LastGameOptions gLastGameOptions;
std::vector<ModuleRecord> gModuleRecords;
std::array<ControlRouteRecord, kControlRouteCapacity> gControlRoutes{};
std::vector<UiViewportTagState> gUiViewportTags;
std::mutex gControlRouteMutex;
std::atomic<uint64_t> gNextModuleGeneration{1};
std::atomic<uint32_t> gActiveControlRouteSlot{UINT32_MAX};
std::atomic<uint32_t> gUniversalRouteFailure{
    static_cast<uint32_t>(UniversalRouteFailure::eNoActiveRoute)};
std::atomic<DWORD> gInternalControlBypassTlsIndex{TLS_OUT_OF_INDEXES};
LARGE_INTEGER gFpsCounterFrequency{};
LARGE_INTEGER gFpsWindowStart{};
uint64_t gFpsWindowOutputFrames = 0;
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
ControlRouteRecord* ActiveControlRoute() noexcept;
const char* ControlPathName(ControlEntryPath path) noexcept;
void SetUniversalRouteFailure(UniversalRouteFailure failure) noexcept;
bool ActiveSetterCovered(const ControlRouteRecord& route) noexcept;
bool StateEntryCovered(const ControlRouteRecord& route) noexcept;
bool TryInstallSetOptionsEntryDetour(HMODULE wrapper, void* resolvedTarget);
bool TryInstallGetStateEntryDetour(HMODULE wrapper, void* resolvedTarget);
uint32_t EnsureControlRoute(HMODULE wrapper, const std::wstring& path,
    uint64_t generation, bool wrapperPatched,
    uint32_t compiledMaximumGeneratedFrames);
bool InstallControlRouteEntries(uint32_t routeSlot);
bool InstallControlRouteLifecycleEntry(uint32_t routeSlot);
bool TryInstallSlInitEntryDetour(HMODULE interposer, void* resolvedTarget);
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
void InspectAlreadyLoadedModules();
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
    const uint32_t maximum = std::clamp(
        CurrentCapacityDecision().effectiveMaximumMultiplier,
        kMinimumMultiplier, kMaximumMultiplier);
    gSafeMaximumMultiplier.store(maximum, std::memory_order_release);
    return maximum;
}

uint32_t EffectiveMultiplier(const ControlConfig& control) noexcept
{
    return std::clamp(control.multiplier, kMinimumMultiplier,
        SafeMaximumMultiplier());
}

UiResourceTagState CaptureUiResourceTag(const sl::ResourceTag& tag, uint64_t tick)
{
    UiResourceTagState state{};
    if (!tag.resource || !tag.resource->native)
        return state;

    state.active = true;
    state.lifecycle = tag.lifecycle;
    state.lastSeenTick = tick;
    state.top = tag.extent.top;
    state.left = tag.extent.left;
    state.width = tag.extent.width != 0 ? tag.extent.width : tag.resource->width;
    state.height = tag.extent.height != 0 ? tag.extent.height : tag.resource->height;
    state.format = tag.resource->nativeFormat;
    return state;
}

bool UiTagFresh(const UiResourceTagState& state, uint64_t now)
{
    return state.active && state.lastSeenTick != 0 && now >= state.lastSeenTick
        && now - state.lastSeenTick <= kUiTagFreshnessMs;
}

UiInputSnapshot ReadUiInputSnapshot(uint32_t viewport)
{
    UiInputSnapshot snapshot{};
    const uint64_t now = GetTickCount64();
    std::lock_guard lock(gUiTagMutex);
    const auto found = std::find_if(gUiViewportTags.begin(), gUiViewportTags.end(),
        [&](const UiViewportTagState& state) { return state.viewport == viewport; });
    if (found == gUiViewportTags.end())
        return snapshot;

    snapshot.hudless = UiTagFresh(found->hudless, now);
    snapshot.uiAlpha = UiTagFresh(found->uiAlpha, now);
    snapshot.uiColorAlpha = UiTagFresh(found->uiColorAlpha, now);
    const UiResourceTagState* ui = snapshot.uiAlpha ? &found->uiAlpha
        : snapshot.uiColorAlpha ? &found->uiColorAlpha : nullptr;
    if (!snapshot.hudless || !ui)
        return snapshot;

    snapshot.hudlessWidth = found->hudless.width;
    snapshot.hudlessHeight = found->hudless.height;
    snapshot.uiWidth = ui->width;
    snapshot.uiHeight = ui->height;
    snapshot.uiFormat = ui->format;
    snapshot.dimensionsKnown = snapshot.hudlessWidth != 0
        && snapshot.hudlessHeight != 0 && snapshot.uiWidth != 0
        && snapshot.uiHeight != 0;
    snapshot.dimensionsMatch = snapshot.dimensionsKnown
        && found->hudless.top == ui->top && found->hudless.left == ui->left
        && snapshot.hudlessWidth == snapshot.uiWidth
        && snapshot.hudlessHeight == snapshot.uiHeight;

    const uint32_t colorWidth = gGameColorWidth.load(std::memory_order_relaxed);
    const uint32_t colorHeight = gGameColorHeight.load(std::memory_order_relaxed);
    if (snapshot.dimensionsMatch && colorWidth != 0 && colorHeight != 0)
    {
        snapshot.dimensionsMatch = snapshot.hudlessWidth == colorWidth
            && snapshot.hudlessHeight == colorHeight;
    }

    const uint64_t hudlessAge = now - found->hudless.lastSeenTick;
    const uint64_t uiAge = now - ui->lastSeenTick;
    snapshot.oldestAgeMs = std::max(hudlessAge, uiAge);
    snapshot.ready = snapshot.dimensionsMatch;
    return snapshot;
}

void RefreshUiInputReadiness(uint32_t viewport)
{
    if (viewport == UINT32_MAX)
        return;
    const UiInputSnapshot snapshot = ReadUiInputSnapshot(viewport);
    const bool previous = gUiInputsReady.exchange(snapshot.ready, std::memory_order_acq_rel);
    if (previous == snapshot.ready)
        return;

    if (gControlReady.load(std::memory_order_acquire))
        gDesiredRevision.fetch_add(1, std::memory_order_release);
    Log(L"UI inputs changed: ready=%d viewport=%u hudless=%d uiAlpha=%d "
        L"uiColorAlpha=%d dimensionsKnown=%d dimensionsMatch=%d "
        L"hudless=%ux%u ui=%ux%u",
        snapshot.ready, viewport, snapshot.hudless, snapshot.uiAlpha,
        snapshot.uiColorAlpha, snapshot.dimensionsKnown, snapshot.dimensionsMatch,
        snapshot.hudlessWidth, snapshot.hudlessHeight,
        snapshot.uiWidth, snapshot.uiHeight);
}

void CaptureUiResourceTags(const sl::ViewportHandle& viewport,
    const sl::ResourceTag* tags, uint32_t numTags)
{
    if (!tags || numTags == 0 || numTags > 1024)
        return;

    const uint32_t viewportValue = static_cast<uint32_t>(viewport);
    const uint64_t tick = GetTickCount64();
    bool relevant = false;
    {
        std::lock_guard lock(gUiTagMutex);
        auto found = std::find_if(gUiViewportTags.begin(), gUiViewportTags.end(),
            [&](const UiViewportTagState& state) { return state.viewport == viewportValue; });
        if (found == gUiViewportTags.end())
        {
            gUiViewportTags.push_back({});
            found = std::prev(gUiViewportTags.end());
            found->viewport = viewportValue;
        }

        for (uint32_t index = 0; index < numTags; ++index)
        {
            const sl::ResourceTag& tag = tags[index];
            UiResourceTagState* destination = nullptr;
            if (tag.type == sl::kBufferTypeHUDLessColor)
                destination = &found->hudless;
            else if (tag.type == sl::kBufferTypeUIAlpha)
                destination = &found->uiAlpha;
            else if (tag.type == sl::kBufferTypeUIColorAndAlpha)
                destination = &found->uiColorAlpha;
            if (!destination)
                continue;
            *destination = CaptureUiResourceTag(tag, tick);
            relevant = true;
        }
    }

    const uint32_t activeViewport = gLastOptionsViewport.load(std::memory_order_acquire);
    if (relevant && (activeViewport == UINT32_MAX || activeViewport == viewportValue))
        RefreshUiInputReadiness(viewportValue);
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

void UpdateFpsTelemetryForOutputPresent()
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

    ++gFpsWindowOutputFrames;
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
        return static_cast<uint32_t>(std::min<uint64_t>(UINT32_MAX,
            (frames * frequency * 1000u + elapsedTicks / 2u) / elapsedTicks));
    };
    gRealFpsMilli.store(rateMilli(realFrames), std::memory_order_relaxed);
    gDlssFpsMilli.store(rateMilli(gFpsWindowOutputFrames),
        std::memory_order_relaxed);
    gFpsSampleWindowMs.store(static_cast<uint32_t>(
        std::min<uint64_t>(UINT32_MAX,
            (elapsedTicks * 1000u + frequency / 2u) / frequency)),
        std::memory_order_relaxed);
    gFpsSampleTick.store(GetTickCount64(), std::memory_order_release);
    gFpsWindowStart = now;
    gFpsWindowOutputFrames = 0;
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
    if (!gGameFrameGenerationOn.load(std::memory_order_acquire))
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
    const uint64_t frequency = static_cast<uint64_t>(
        gFpsCounterFrequency.QuadPart);
    const auto rateMilli = [&](uint64_t frames) {
        return static_cast<uint32_t>(std::min<uint64_t>(UINT32_MAX,
            (frames * frequency * 1000u + elapsedTicks / 2u) / elapsedTicks));
    };
    gRealFpsMilli.store(rateMilli(realFrames), std::memory_order_relaxed);
    gDlssFpsMilli.store(rateMilli(realFrames * presentedMultiplier),
        std::memory_order_relaxed);
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
        return;

    const uint32_t previous =
        gActualFramesPresented.exchange(state.numFramesActuallyPresented,
            std::memory_order_relaxed);
    gDlssgStatus.store(static_cast<uint32_t>(state.status), std::memory_order_relaxed);
    if (state.structVersion >= sl::kStructVersion2)
        gNumFramesToGenerateMax.store(
            state.numFramesToGenerateMax, std::memory_order_relaxed);
    if (state.structVersion >= sl::kStructVersion4)
        gDynamicMfgSupported.store(
            state.bIsDynamicMFGSupported == sl::Boolean::eTrue,
            std::memory_order_relaxed);
    gStateSampleTick.store(GetTickCount64(), std::memory_order_release);

    if (previous != state.numFramesActuallyPresented)
        Log(L"DLSS-G state sample: frames presented since prior query=%u "
            L"(maximum generated per real frame=%u, status=%u)",
            state.numFramesActuallyPresented,
            gNumFramesToGenerateMax.load(std::memory_order_relaxed),
            static_cast<uint32_t>(state.status));
}

bool DynamicMfgCapabilityKnown() noexcept
{
    return gStateSampleTick.load(std::memory_order_acquire) != 0;
}

bool DynamicMfgSupported() noexcept
{
    return DynamicMfgCapabilityKnown()
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
    return midpoint_fix::AdapterVerified()
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
    const universal_route_policy::Readiness state{
        control != nullptr,
        control && control->wrapperPatched,
        control && control->structureCompatible.load(
            std::memory_order_acquire),
        control && ActiveSetterCovered(*control),
        control && StateEntryCovered(*control),
        gActiveNgxProviderBase.load(std::memory_order_acquire) != 0,
        gProviderChangedAfterCreate.load(std::memory_order_acquire),
        create.current,
        evaluate.current,
        AdapterVerifiedForApi(static_cast<NgxGraphicsApi>(
            gActiveNgxGraphicsApi.load(std::memory_order_acquire))),
        midpoint_fix::Ready()
            && gFirstCreateMidpointReady.load(std::memory_order_acquire),
    };
    const UniversalRouteFailure failure =
        universal_route_policy::EvaluateReadiness(state);
    SetUniversalRouteFailure(failure);
    return failure == UniversalRouteFailure::eNone;
}

const char* PatchRouteName()
{
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
    if (parsed < minimum || parsed > maximum)
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
        kMinimumMultiplier, kMaximumMultiplier, parsed.multiplier))
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

    size_t experimentalOffset = 0;
    if (FindJsonValue(content, "dynamicExperimental56", experimentalOffset)
        && !TryParseBoolean(content, "dynamicExperimental56",
            parsed.dynamicExperimental56))
        return false;

    size_t intervalLoggingOffset = 0;
    bool legacyIntervalLogging = true;
    if (FindJsonValue(content, "intervalLogging", intervalLoggingOffset)
        && !TryParseBoolean(content, "intervalLogging",
            legacyIntervalLogging))
        return false;
    // Protocol 18 release tracing is always active. Accept the retired setting
    // so existing files remain valid, but never let it disable diagnostics.
    parsed.intervalLogging = true;

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
        L"RTX40_MFG_ACTIVE_MULTIPLIER", value, _countof(value));
    if (length == 1 && value[0] >= L'2' && value[0] <= L'6')
        control.multiplier = static_cast<uint32_t>(value[0] - L'0');

    ControlConfig fileControl{};
    if (ReadControlFile(gConfigPath, fileControl))
        return fileControl;

#if defined(MFG_UNLOCK_UNIVERSAL_CONFIG)
    // A dedicated CET install keeps its live control inside CET's mandatory
    // per-mod filesystem sandbox. Preserve an existing executable-side choice
    // as the first-launch fallback; the next CET selection is written to the
    // sandbox-local file watched by this core.
    const std::wstring executableConfig = JoinPath(
        gExecutableDirectory, L"RTX40MFG-Universal.json");
    if (_wcsicmp(gConfigPath.c_str(), executableConfig.c_str()) != 0
        && ReadControlFile(executableConfig, fileControl))
    {
        return fileControl;
    }
#endif
    return control;
}

std::wstring ResolveConfigPath(HMODULE instance, const std::wstring& executableDirectory)
{
    std::wstring explicitPath(32768, L'\0');
    const DWORD explicitLength = GetEnvironmentVariableW(
        L"RTX40_MFG_CONFIG_PATH", explicitPath.data(),
        static_cast<DWORD>(explicitPath.size()));
    if (explicitLength > 0 && explicitLength < explicitPath.size())
    {
        explicitPath.resize(explicitLength);
        return explicitPath;
    }

#if defined(MFG_UNLOCK_UNIVERSAL_CONFIG)
    const std::wstring cetDirectory = JoinPath(executableDirectory,
        L"plugins\\cyber_engine_tweaks\\mods\\RTX40MFG");
    if (IsRegularFile(JoinPath(cetDirectory, L"init.lua")))
    {
        return JoinPath(cetDirectory, L"RTX40MFG-Universal.json");
    }
    return JoinPath(executableDirectory, L"RTX40MFG-Universal.json");
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
    std::wstring explicitPath(32768, L'\0');
    const DWORD explicitLength = GetEnvironmentVariableW(
        L"RTX40_MFG_STATUS_PATH", explicitPath.data(),
        static_cast<DWORD>(explicitPath.size()));
    if (explicitLength > 0 && explicitLength < explicitPath.size())
    {
        explicitPath.resize(explicitLength);
        return explicitPath;
    }

#if defined(MFG_UNLOCK_UNIVERSAL_CONFIG)
    const std::wstring universalConfig = JoinPath(
        executableDirectory, L"RTX40MFG-Universal.json");
    const std::wstring cetConfig = JoinPath(executableDirectory,
        L"plugins\\cyber_engine_tweaks\\mods\\RTX40MFG\\"
        L"RTX40MFG-Universal.json");
    if (_wcsicmp(configPath.c_str(), universalConfig.c_str()) == 0
        || _wcsicmp(configPath.c_str(), cetConfig.c_str()) == 0)
    {
        return JoinPath(ParentPath(configPath),
            L"RTX40MFG-Universal.status.json");
    }
#endif
    return JoinPath(ParentPath(configPath), L"bridge_status.json");
}

uint64_t StoreControl(const ControlConfig& control)
{
    gDesiredFollowGame.store(control.followGame, std::memory_order_relaxed);
    gDesiredMultiplier.store(control.multiplier, std::memory_order_relaxed);
    gDesiredDynamicMode.store(control.dynamic, std::memory_order_relaxed);
    gDynamicTargetFrameRate.store(control.dynamicTargetFrameRate, std::memory_order_relaxed);
    gDynamicExperimental56.store(control.dynamicExperimental56, std::memory_order_relaxed);
    gGeneratedOnlyDebug.store(control.generatedOnlyDebug,
        std::memory_order_relaxed);
    temporal_interval_trace::SetEnabled(true);
    const uint64_t revision = gDesiredRevision.fetch_add(1, std::memory_order_release) + 1;
    gControlReady.store(true, std::memory_order_release);
    return revision;
}

ControlSnapshot ReadControlSnapshot()
{
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
    SetEnvironmentVariableW(L"RTX40_MFG_ACTIVE_MULTIPLIER", multiplier);
    SetEnvironmentVariableW(L"RTX40_MFG_ACTIVE_MODE",
        control.followGame ? L"follow"
            : control.dynamic ? L"dynamic" : L"fixed");
    SetEnvironmentVariableW(L"RTX40_MFG_FOLLOW_GAME",
        control.followGame ? L"1" : L"0");
    SetEnvironmentVariableW(L"RTX40_MFG_DYNAMIC_TARGET", target);
    SetEnvironmentVariableW(L"RTX40_MFG_DYNAMIC_EXPERIMENTAL_56",
        control.dynamicExperimental56 ? L"1" : L"0");
    SetEnvironmentVariableW(L"RTX40_MFG_GENERATED_ONLY_DEBUG",
        control.generatedOnlyDebug ? L"1" : L"0");
    SetEnvironmentVariableW(L"RTX40_MFG_INTERVAL_LOGGING", L"1");
    SetEnvironmentVariableW(L"RTX40_MFG_AUTO_BRIDGE", L"1");
}

void PublishPatchRoute()
{
    const char* route = PatchRouteName();
    wchar_t wideRoute[16]{};
    MultiByteToWideChar(CP_UTF8, 0, route, -1, wideRoute, _countof(wideRoute));
    SetEnvironmentVariableW(L"RTX40_MFG_PATCH_ROUTE", wideRoute);
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

bool WriteBridgeStatus(const ControlConfig& control, DWORD pid)
{
    if (gStatusPath.empty())
        return false;

    UpdateFpsTelemetryWithoutPresentCallback();
    const uint32_t uiViewport = gLastOptionsViewport.load(std::memory_order_acquire);
    RefreshUiInputReadiness(uiViewport);
    const UiInputSnapshot uiInputs = ReadUiInputSnapshot(uiViewport);
    const bool bridgeReady = BridgeReady();
    const char* route = PatchRouteName();
    const uint64_t desiredRevision = gDesiredRevision.load(std::memory_order_acquire);
    const uint64_t appliedRevision = gAppliedRevision.load(std::memory_order_acquire);
    const bool setOptionsSeen = gSetOptionsSeen.load(std::memory_order_acquire);
    const bool getStateSeen = gGetStateSeen.load(std::memory_order_acquire);
    const bool gameFrameGenerationOn =
        gGameFrameGenerationOn.load(std::memory_order_acquire);
    const int32_t setOptionsResult =
        gLastSetOptionsResult.load(std::memory_order_relaxed);
    const int32_t getStateResult =
        gLastGetStateResult.load(std::memory_order_relaxed);
    const bool setOptionsAccepted = setOptionsResult == static_cast<int32_t>(sl::Result::eOk)
        || setOptionsResult == static_cast<int32_t>(sl::Result::eWarnOutOfVRAM);
    const bool applied = gameFrameGenerationOn && appliedRevision != 0
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
        !control.followGame
        && (control.multiplier > safeMaximumMultiplier
            || (control.dynamic && control.dynamicExperimental56
                && safeMaximumMultiplier < kMaximumMultiplier));
    const bool setOptionsResolverFallbackActive =
        gSetOptionsResolverFallbackActive.load(std::memory_order_acquire);
    const uint64_t setOptionsResolverFallbackCalls =
        gSetOptionsResolverFallbackCalls.load(std::memory_order_acquire);
    const bool setOptionsControlPathReady = setOptionsDetour.current
        || (setOptionsResolverFallbackActive
            && setOptionsResolverFallbackCalls > 0);
    const temporal_interval_trace::Snapshot intervalTrace =
        temporal_interval_trace::ReadSnapshot();
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
        ? "runtime-unique-candidate" : "none";
    const UniversalRouteFailure routeFailure =
        static_cast<UniversalRouteFailure>(gUniversalRouteFailure.load(
            std::memory_order_acquire));
    const entry_detour::Snapshot releaseDetour = activeControl
        ? entry_detour::ReadSnapshot(activeControl->freeResourcesHandle)
        : entry_detour::Snapshot{};

    char json[16384]{};
    const int length = sprintf_s(json,
        "{\"version\":18,\"pid\":%lu,\"heartbeat\":%llu,\"route\":\"%s\","
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
        "\"requestedMultiplierLimited\":%s,"
        "\"intervalLoggingEnabled\":%s,\"intervalLogReady\":%s,"
        "\"intervalValidSamples\":%llu,\"intervalInvalidSamples\":%llu,"
        "\"intervalDroppedSamples\":%llu,\"intervalSeenCountMask\":%u,"
        "\"intervalSeenIndexMask\":%u,\"intervalLastCount\":%d,"
        "\"intervalLastIndex\":%d,\"intervalLastPositionNumerator\":%u,"
        "\"intervalLastPositionDenominator\":%u,"
        "\"intervalLogFile\":\"MfgUnlock-intervals-%lu.csv\","
        "\"requestRevision\":%llu,\"appliedRevision\":%llu,"
        "\"applied\":%s,\"pending\":%s,\"gameFrameGenerationOn\":%s,"
        "\"appliedMode\":\"%s\",\"appliedMultiplier\":%u,"
        "\"appliedDynamicTargetFrameRate\":%u,"
        "\"appliedDynamicExperimental56\":%s,"
        "\"appliedGeneratedOnlyDebug\":%s,\"setOptionsSeen\":%s,"
        "\"setOptionsAccepted\":%s,"
        "\"setOptionsResult\":%d,\"getStateSeen\":%s,\"getStateResult\":%d,"
        "\"actualFramesPresented\":%u,\"numFramesToGenerateMax\":%u,"
        "\"realFpsMilli\":%u,\"dlssFpsMilli\":%u,"
        "\"fpsSampleWindowMs\":%u,\"fpsSampleAgeMs\":%llu,"
        "\"dlssgStatus\":%u,\"dynamicMfgSupportKnown\":%s,"
        "\"dynamicMfgSupported\":%s,"
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
        static_cast<unsigned long long>(UnixTimeSeconds()), route,
        bridgeReady ? "true" : "false",
        gLiveHookInstalled.load(std::memory_order_relaxed) ? "true" : "false",
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
        static_cast<unsigned long>(pid),
        static_cast<unsigned long long>(desiredRevision),
        static_cast<unsigned long long>(appliedRevision),
        applied ? "true" : "false", pending ? "true" : "false",
        gameFrameGenerationOn ? "true" : "false",
        gAppliedDynamicMode.load(std::memory_order_relaxed) ? "dynamic" : "fixed",
        gAppliedMultiplier.load(std::memory_order_relaxed),
        gAppliedDynamicTargetFrameRate.load(std::memory_order_relaxed),
        gAppliedDynamicExperimental56.load(std::memory_order_relaxed) ? "true" : "false",
        gAppliedGeneratedOnlyDebug.load(std::memory_order_relaxed) ? "true" : "false",
        setOptionsSeen ? "true" : "false",
        setOptionsAccepted ? "true" : "false", setOptionsResult,
        getStateSeen ? "true" : "false", getStateResult,
        gActualFramesPresented.load(std::memory_order_relaxed),
        gNumFramesToGenerateMax.load(std::memory_order_relaxed),
        gRealFpsMilli.load(std::memory_order_relaxed),
        gDlssFpsMilli.load(std::memory_order_relaxed),
        gFpsSampleWindowMs.load(std::memory_order_relaxed),
        static_cast<unsigned long long>(fpsAgeMs),
        gDlssgStatus.load(std::memory_order_relaxed),
        DynamicMfgCapabilityKnown() ? "true" : "false",
        gDynamicMfgSupported.load(std::memory_order_relaxed) ? "true" : "false",
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

    HANDLE file = CreateFileW(gStatusPath.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    DWORD written = 0;
    const BOOL result = WriteFile(file, json, static_cast<DWORD>(length), &written, nullptr);
    CloseHandle(file);
    return result && written == static_cast<DWORD>(length);
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
            result = static_cast<const T*>(chain);
            version = fields.version;
            return ChainFindStatus::eFound;
        }
        chain = fields.next;
    }
    return chain ? ChainFindStatus::eMalformed
                 : ChainFindStatus::eNotFound;
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

void RecordSetOptionsLifecycle(ControlRouteRecord& route, bool enabled,
    sl::Result result) noexcept
{
    if (!IsAcceptedControlResult(result))
        return;
    route.lastAcceptedRevision.store(
        gDesiredRevision.load(std::memory_order_acquire),
        std::memory_order_release);
    gGameFrameGenerationOn.store(enabled, std::memory_order_release);
    route.frameGenerationOffAccepted.store(!enabled,
        std::memory_order_release);
    if (enabled)
        route.releaseObserved.store(false, std::memory_order_release);
}

void ResetReleasedPipelineState(ControlRouteRecord& route) noexcept
{
    route.frameGenerationOffAccepted.store(false,
        std::memory_order_release);
    route.releaseObserved.store(true, std::memory_order_release);
    gGameFrameGenerationOn.store(false, std::memory_order_release);
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

void SetUniversalRouteFailure(UniversalRouteFailure failure) noexcept
{
    gUniversalRouteFailure.store(static_cast<uint32_t>(failure),
        std::memory_order_release);
}

void PublishActiveControlRoute(ControlRouteRecord& route)
{
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
        Log(L"Active DLSS-G wrapper selected by real call: generation=%llu "
            L"patched=%d compiledMaximum=%u version=%u.%u.%u.%u path=%s",
            static_cast<unsigned long long>(route.generation),
            route.wrapperPatched, route.compiledMaximumGeneratedFrames,
            route.version.major, route.version.minor, route.version.build,
            route.version.privatePart, route.path.c_str());
    }
}

bool ActivateControlRoute(uint32_t slot, ControlEntryPath path,
    bool setter) noexcept
{
    ControlRouteRecord* route = ControlRouteAt(slot);
    if (!route)
        return false;
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
        lifecycle.frameGenerationOn = gGameFrameGenerationOn.load(
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
    PublishActiveControlRoute(*route);
    InstallControlRouteLifecycleEntry(slot);
    if (!route->wrapperPatched)
    {
        SetUniversalRouteFailure(
            UniversalRouteFailure::eActiveWrapperUnpatched);
        return false;
    }
    return route->structureCompatible.load(std::memory_order_acquire);
}

void InvalidateControlRoute(uint32_t slot,
    UniversalRouteFailure failure) noexcept
{
    if (ControlRouteRecord* route = ControlRouteAt(slot))
        route->structureCompatible.store(false, std::memory_order_release);
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
    bool preserveNext, bool enableUiRecomposition)
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
    if (snapshot.control.generatedOnlyDebug)
    {
        adjusted.flags = static_cast<sl::DLSSGFlags>(
            static_cast<uint32_t>(adjusted.flags)
            | static_cast<uint32_t>(
                sl::DLSSGFlags::eShowOnlyInterpolatedFrame));
    }
    if (enableUiRecomposition)
    {
        adjusted.structVersion = std::max<size_t>(
            adjusted.structVersion, sl::kStructVersion4);
        adjusted.enableUserInterfaceRecomposition = sl::Boolean::eTrue;
    }
    return adjusted;
}

void CaptureGameOptions(
    const sl::ViewportHandle& viewport, const sl::DLSSGOptions& options)
{
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
    RefreshUiInputReadiness(viewportValue);
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
    bool effectiveDynamicExperimental56, bool dynamicOverrideApplied)
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
        snapshot.control.dynamicTargetFrameRate, std::memory_order_relaxed);
    gAppliedDynamicExperimental56.store(
        effectiveDynamicExperimental56, std::memory_order_relaxed);
    gAppliedGeneratedOnlyDebug.store(
        snapshot.control.generatedOnlyDebug, std::memory_order_relaxed);
    gAppliedUiRecompositionEnabled.store(
        uiRecompositionEnabled, std::memory_order_relaxed);
    gAppliedUiRecompositionForced.store(
        uiRecompositionForced, std::memory_order_relaxed);
    gAppliedRevision.store(snapshot.revision, std::memory_order_release);
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
        gUiInputsReady.load(std::memory_order_relaxed),
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
    QueryState&& queryState, bool hasState)
{
    const UiInputSnapshot uiInputs = ReadUiInputSnapshot(
        static_cast<uint32_t>(viewport));
    const bool gameUiRecomposition = source.structVersion >= sl::kStructVersion4
        && source.enableUserInterfaceRecomposition == sl::Boolean::eTrue;
    const bool forceUiRecomposition = uiInputs.ready && !gameUiRecomposition;
    const bool dynamicRequested = !snapshot.control.followGame
        && snapshot.control.dynamic;
    if (dynamicRequested && !DynamicMfgCapabilityKnown() && hasState)
    {
        sl::DLSSGState state{};
        const sl::Result stateResult = queryState(state, &source);
        RecordDlssgStateResult(stateResult, state);
    }

    sl::DLSSGOptions adjusted = BuildAdjustedOptions(
        source, snapshot, !liveReapply, uiInputs.ready);
    bool dynamicOverrideApplied = dynamicRequested && DynamicMfgSupported();
    sl::Result result = submitOptions(adjusted);
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
        adjusted = BuildAdjustedOptions(
            source, snapshot, !liveReapply, uiInputs.ready);
        dynamicOverrideApplied = true;
        result = submitOptions(adjusted);
    }

    const bool preserveGameControl = snapshot.control.followGame
        || (dynamicRequested && !dynamicOverrideApplied);
    const uint32_t effectiveMultiplier = preserveGameControl
        ? std::clamp(std::min(source.numFramesToGenerate,
                kMaximumMultiplier - 1u) + 1u,
            kMinimumMultiplier, SafeMaximumMultiplier())
        : EffectiveMultiplier(snapshot.control);
    const bool effectiveDynamicMode = preserveGameControl
        ? source.mode == sl::DLSSGMode::eDynamic
            || source.mode == sl::DLSSGMode::eAuto
        : snapshot.control.dynamic;
    const bool effectiveDynamicExperimental56 =
        dynamicOverrideApplied
        && snapshot.control.dynamicExperimental56
        && SafeMaximumMultiplier() >= kMaximumMultiplier;
    const bool uiRecompositionEnabled = adjusted.structVersion >= sl::kStructVersion4
        && adjusted.enableUserInterfaceRecomposition == sl::Boolean::eTrue;
    RecordAppliedControl(snapshot, result, liveReapply,
        uiRecompositionEnabled, forceUiRecomposition,
        effectiveMultiplier, effectiveDynamicMode,
        effectiveDynamicExperimental56, dynamicOverrideApplied);
    if (result == sl::Result::eOk || result == sl::Result::eWarnOutOfVRAM)
    {
        route.lastAcceptedRevision.store(snapshot.revision,
            std::memory_order_release);
        RecordSetOptionsLifecycle(route, true, result);
    }
    // Some hosts treat every non-zero Result as a hard failure. Result 39 is a
    // warning rather than a rejected options update, so preserve it in the
    // bridge status while returning success to the host.
    return result == sl::Result::eWarnOutOfVRAM ? sl::Result::eOk : result;
}

sl::Result SubmitAdjustedOptions(
    ControlRouteRecord& route, const sl::ViewportHandle& viewport,
    const sl::DLSSGOptions& source, const ControlSnapshot& snapshot,
    bool liveReapply)
{
    const bool hasState = RouteHasStateFunction(route);
    return SubmitAdjustedOptionsImpl(route, viewport, source, snapshot,
        liveReapply,
        [&](const sl::DLSSGOptions& adjusted) {
            return CallRouteSetOptions(route, viewport, adjusted);
        },
        [&](sl::DLSSGState& state, const sl::DLSSGOptions* options) {
            return CallRouteGetState(route, viewport, state, options);
        }, hasState);
}

void ReapplyPendingControl(const sl::ViewportHandle& viewport)
{
    if (!gControlReady.load(std::memory_order_acquire)
        || !gGameFrameGenerationOn.load(std::memory_order_acquire)
        || !BridgeReady())
        return;

    const ControlSnapshot snapshot = ReadControlSnapshot();
    if (snapshot.revision == 0
        || snapshot.revision == gAppliedRevision.load(std::memory_order_acquire))
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
        SubmitAdjustedOptions(*route, viewport, source, snapshot, true);
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
    const bool d3d12Runtime = ExportsNgxD3D12Route(module)
        && GetProcAddress(module, "NVSDK_NGX_D3D12_Init_ProjectID");
    const bool vulkanRuntime = ExportsNgxVulkanRoute(module)
        && (GetProcAddress(module, "NVSDK_NGX_VULKAN_Init_with_ProjectID")
            || GetProcAddress(
                module, "NVSDK_NGX_VULKAN_Init_ProjectID")
            || GetProcAddress(
                module, "NVSDK_NGX_VULKAN_Init_ProjectID_Ext"));
    return module
        && (d3d12Runtime || vulkanRuntime)
        && !GetProcAddress(module, "NVSDK_NGX_GetAPIVersion")
        && !GetProcAddress(module, "NVSDK_NGX_GetGPUArchitecture")
        && !dlssg_provider_policy::IsDlssgImplementationModule(module);
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
    std::wstring& path) noexcept
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
            candidatePath = record.path;
        }
    }
    if (universal_route_policy::ResolveProvider(false, candidateCount)
        != universal_route_policy::ProviderResolution::eUniqueCandidate)
        return false;
    provider = candidate;
    path = std::move(candidatePath);
    return true;
}

bool ResolveMidpointProvider(NgxDispatchRoute route, NgxGraphicsApi api,
    const entry_detour::Snapshot& detour, const void* originalCaller,
    HMODULE& provider, std::wstring& path,
    NgxProviderSelectionSource& source)
{
    provider = nullptr;
    path.clear();
    source = NgxProviderSelectionSource::eNone;
    if (route == NgxDispatchRoute::eProvider)
    {
        provider = detour.owner;
        path = LoadedModulePath(provider);
        if (provider && dlssg_provider_policy::IsSupportedProvider(
                provider, path.c_str()))
        {
            source = NgxProviderSelectionSource::eProviderEntry;
            return true;
        }
        return false;
    }

    // Prefer the preserved caller when the runtime really was entered by a
    // DLSS-G provider. Some NGX runtimes instead receive the call from a
    // runtime-owned dispatcher. In that case, fall back only when discovery
    // has exactly one supported, patched provider with current Create and
    // Evaluate entry detours. Zero or multiple candidates remain fail-closed.
    provider = ModuleFromAddress(originalCaller);
    path = LoadedModulePath(provider);
    if (provider && dlssg_provider_policy::IsSupportedProvider(
            provider, path.c_str()))
    {
        source = NgxProviderSelectionSource::eRuntimeCaller;
        return true;
    }
    provider = nullptr;
    path.clear();
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
            provider, path, selectionSource);

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
                L"callerPreserved=1",
                static_cast<unsigned long long>(call),
                NgxDispatchRouteName(route), NgxGraphicsApiName(api),
                AdapterVerifiedForApi(api),
                detour.targetRva);
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

    const bool firstPipelineCreate =
        !gFrameGenerationCreateObserved.exchange(
            true, std::memory_order_acq_rel);
    gActiveNgxProviderGeneration.store(
        ModuleGeneration(provider), std::memory_order_release);
    gActiveNgxSelectionSource.store(static_cast<uint32_t>(selectionSource),
        std::memory_order_release);
    gActiveNgxCreateHandle.store(PackEntryHandle(entryHandle),
        std::memory_order_release);
    const entry_detour::Kind evaluateKind = api == NgxGraphicsApi::eVulkan
        ? route == NgxDispatchRoute::eRuntime
            ? entry_detour::Kind::eNgxRuntimeVulkanEvaluateFeature
            : entry_detour::Kind::eNgxVulkanEvaluateFeature
        : route == NgxDispatchRoute::eRuntime
            ? entry_detour::Kind::eNgxRuntimeD3D12EvaluateFeature
            : entry_detour::Kind::eNgxD3D12EvaluateFeature;
    const entry_detour::Snapshot evaluate =
        entry_detour::ReadSnapshot(evaluateKind, detour.owner);
    if (evaluate.current)
    {
        gActiveNgxEvaluateHandle.store(
            PackEntryHandle(evaluate.handle), std::memory_order_release);
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
        && !midpoint_fix::AdapterVerified() && commandContext)
    {
        auto* commandList = static_cast<ID3D12GraphicsCommandList*>(
            commandContext);
        ID3D12Device* device = nullptr;
        if (SUCCEEDED(commandList->GetDevice(
                __uuidof(ID3D12Device), reinterpret_cast<void**>(&device)))
            && device)
        {
            midpoint_fix::ObserveD3D12Device(device);
            device->Release();
        }
    }
    const bool adapterVerified = AdapterVerifiedForApi(api);
    const bool ready = adapterVerified
        && midpoint_fix::PatchProvider(provider, path.c_str());
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
            L"targetRva=0x%X callerPreserved=1",
            static_cast<unsigned long long>(call),
            NgxDispatchRouteName(route), NgxGraphicsApiName(api),
            providerAccepted,
            adapterVerified, ready, detour.targetRva);
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
    if (!provider
        || !dlssg_provider_policy::IsSupportedProvider(
            provider, path.c_str()))
        return false;
    void* target = reinterpret_cast<void*>(GetProcAddress(
        provider, "NVSDK_NGX_D3D12_CreateFeature"));
    if (!target)
        return false;

    void* trampoline = nullptr;
    entry_detour::Handle detourHandle{};
    entry_detour::InstallOptions installOptions{};
    installOptions.generation = generation;
    installOptions.allowRelocated = true;
    installOptions.filterForwardArg2 = true;
    installOptions.requiredForwardArg2 = static_cast<uintptr_t>(
        NVSDK_NGX_Feature_FrameGeneration);
    const bool installed = entry_detour::InstallForwarding(
        entry_detour::Kind::eNgxD3D12CreateFeature,
        provider, target, &BeforeNgxD3D12CreateFeature,
        trampoline, installOptions, &detourHandle);
    const entry_detour::Snapshot state =
        entry_detour::ReadSnapshot(detourHandle);
    Log(L"NGX CreateFeature entry detour: installed=%d current=%d "
        L"cachedPointersCovered=%d method=%hs failure=%u "
        L"targetRva=0x%X path=%s",
        installed, state.current, state.cachedPointersCovered,
        entry_detour::MethodName(state.method),
        static_cast<uint32_t>(state.failure), state.targetRva, path.c_str());
    return installed;
}

void BeforeNgxEvaluateFeatureForRoute(NgxDispatchRoute route,
    NgxGraphicsApi api, void* commandList, uintptr_t handleValue,
    const void* parameters,
    void* callback, entry_detour::Handle entryHandle,
    const void* originalCaller) noexcept
{
    const uint32_t active = gActiveNgxDispatchRoute.load(
        std::memory_order_acquire);
    if (active != static_cast<uint32_t>(route))
        return;
    if (gActiveNgxGraphicsApi.load(std::memory_order_acquire)
        != static_cast<uint32_t>(api))
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
    std::wstring observedPath;
    NgxProviderSelectionSource selectionSource =
        NgxProviderSelectionSource::eNone;
    const bool providerResolved = detour.current
        && ResolveMidpointProvider(route, api, detour, originalCaller,
            observedProvider, observedPath, selectionSource);
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
                handle, ngxParameters, midpoint_fix::Ready());
        if (!universal_route_policy::CanInspectRuntimeEvaluate(false,
                selectedByUniqueCandidate, validTemporalSample))
            return;
    }
    else
    {
        temporal_interval_trace::Record(
            handle, ngxParameters, midpoint_fix::Ready());
    }
    gNgxEvaluateCalls.fetch_add(1, std::memory_order_relaxed);
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
    if (!provider
        || !dlssg_provider_policy::IsSupportedProvider(
            provider, path.c_str()))
        return false;
    void* target = reinterpret_cast<void*>(GetProcAddress(
        provider, "NVSDK_NGX_D3D12_EvaluateFeature"));
    if (!target)
        return false;

    void* trampoline = nullptr;
    entry_detour::Handle detourHandle{};
    entry_detour::InstallOptions installOptions{};
    installOptions.generation = generation;
    installOptions.allowRelocated = true;
    const bool installed = entry_detour::InstallForwarding(
        entry_detour::Kind::eNgxD3D12EvaluateFeature,
        provider, target, &BeforeNgxD3D12EvaluateFeature, trampoline,
        installOptions, &detourHandle);
    const entry_detour::Snapshot state =
        entry_detour::ReadSnapshot(detourHandle);
    Log(L"NGX EvaluateFeature entry detour: installed=%d current=%d "
        L"cachedPointersCovered=%d method=%hs failure=%u "
        L"targetRva=0x%X path=%s",
        installed, state.current, state.cachedPointersCovered,
        entry_detour::MethodName(state.method),
        static_cast<uint32_t>(state.failure), state.targetRva, path.c_str());
    return installed;
}

bool TryInstallNgxRuntimeCreateEntryDetour(
    HMODULE runtime, const std::wstring& path, uint64_t generation)
{
    if (!IsNgxRuntimeModule(runtime))
        return false;
    void* target = reinterpret_cast<void*>(GetProcAddress(
        runtime, "NVSDK_NGX_D3D12_CreateFeature"));
    if (!target)
        return false;

    void* trampoline = nullptr;
    entry_detour::Handle detourHandle{};
    entry_detour::InstallOptions installOptions{};
    installOptions.generation = generation;
    installOptions.allowRelocated = true;
    installOptions.filterForwardArg2 = true;
    installOptions.requiredForwardArg2 = static_cast<uintptr_t>(
        NVSDK_NGX_Feature_FrameGeneration);
    const bool installed = entry_detour::InstallForwarding(
        entry_detour::Kind::eNgxRuntimeD3D12CreateFeature,
        runtime, target, &BeforeNgxRuntimeD3D12CreateFeature, trampoline,
        installOptions, &detourHandle);
    const entry_detour::Snapshot state =
        entry_detour::ReadSnapshot(detourHandle);
    Log(L"NGX runtime CreateFeature entry detour: installed=%d current=%d "
        L"cachedPointersCovered=%d method=%hs failure=%u "
        L"targetRva=0x%X path=%s",
        installed, state.current, state.cachedPointersCovered,
        entry_detour::MethodName(state.method),
        static_cast<uint32_t>(state.failure), state.targetRva, path.c_str());
    return installed;
}

bool TryInstallNgxRuntimeEvaluateEntryDetour(
    HMODULE runtime, const std::wstring& path, uint64_t generation)
{
    if (!IsNgxRuntimeModule(runtime))
        return false;
    void* target = reinterpret_cast<void*>(GetProcAddress(
        runtime, "NVSDK_NGX_D3D12_EvaluateFeature"));
    if (!target)
        return false;

    void* trampoline = nullptr;
    entry_detour::Handle detourHandle{};
    entry_detour::InstallOptions installOptions{};
    installOptions.generation = generation;
    installOptions.allowRelocated = true;
    const bool installed = entry_detour::InstallForwarding(
        entry_detour::Kind::eNgxRuntimeD3D12EvaluateFeature,
        runtime, target, &BeforeNgxRuntimeD3D12EvaluateFeature, trampoline,
        installOptions, &detourHandle);
    const entry_detour::Snapshot state =
        entry_detour::ReadSnapshot(detourHandle);
    Log(L"NGX runtime EvaluateFeature entry detour: installed=%d current=%d "
        L"cachedPointersCovered=%d method=%hs failure=%u "
        L"targetRva=0x%X path=%s",
        installed, state.current, state.cachedPointersCovered,
        entry_detour::MethodName(state.method),
        static_cast<uint32_t>(state.failure), state.targetRva, path.c_str());
    return installed;
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
    options.filterForwardArg2 = featureIsSecondArgument;
    options.requiredForwardArg2 = static_cast<uintptr_t>(
        NVSDK_NGX_Feature_FrameGeneration);
    void* trampoline = nullptr;
    entry_detour::Handle handle{};
    const bool installed = entry_detour::InstallForwarding(kind, owner,
        target, callback, trampoline, options, &handle);
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
        target, callback, trampoline, options, &handle);
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

void WINAPI BeforeNgxVulkanInit(void*, uintptr_t, const void*,
    void* physicalDevice, uintptr_t, uintptr_t, entry_detour::Handle,
    const void*) noexcept
{
    const bool verified = physicalDevice
        && midpoint_fix::ObserveVulkanPhysicalDevice(physicalDevice);
    gVulkanAdapterVerified.store(verified, std::memory_order_release);
    if (verified)
        gModuleInventoryDirty.store(true, std::memory_order_release);
}

void WINAPI BeforeNgxVulkanProjectInit(void*, uintptr_t, const void*,
    void*, uintptr_t, uintptr_t physicalDevice, entry_detour::Handle,
    const void*) noexcept
{
    const bool verified = physicalDevice
        && midpoint_fix::ObserveVulkanPhysicalDevice(
            reinterpret_cast<void*>(physicalDevice));
    gVulkanAdapterVerified.store(verified, std::memory_order_release);
    if (verified)
        gModuleInventoryDirty.store(true, std::memory_order_release);
}

bool TryInstallNgxVulkanAdapterEntryDetours(HMODULE module,
    const std::wstring& path, uint64_t generation)
{
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

sl::Result PassPublicSet(ControlRouteRecord& route,
    const sl::ViewportHandle& viewport,
    const sl::DLSSGOptions& options) noexcept
{
    auto* original = EntryOriginal(
        route.publicSetOriginal, route.publicSetHandle);
    if (!original)
        return sl::Result::eErrorNotInitialized;
    ScopedInternalControlBypass bypass;
    return original(viewport, options);
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
    if (!enabled)
    {
        gSetOptionsSeen.store(true, std::memory_order_release);
        const sl::Result result = PassPublicSet(*route, viewport, options);
        gLastSetOptionsResult.store(static_cast<int32_t>(result),
            std::memory_order_relaxed);
        RecordSetOptionsLifecycle(*route, false, result);
        return result;
    }

    CaptureGameOptions(viewport, options);
    if (!gControlReady.load(std::memory_order_acquire)
        || !BridgeReady())
    {
        const sl::Result result = PassPublicSet(*route, viewport, options);
        gSetOptionsSeen.store(true, std::memory_order_release);
        gLastSetOptionsResult.store(static_cast<int32_t>(result),
            std::memory_order_relaxed);
        RecordSetOptionsLifecycle(*route, true, result);
        if (IsAcceptedControlResult(result)
            && !gFrameGenerationCreateObserved.load(
                std::memory_order_acquire))
        {
            gPipelineMayPredateDetour.store(true,
                std::memory_order_release);
            gRestartRequired.store(true, std::memory_order_release);
        }
        if (!IsAcceptedControlResult(result) || !BridgeReady())
            return result;
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

    BaseStructureFields stateFields{};
    if (!ReadBaseStructureFields(&state, stateFields)
        || stateFields.type != sl::DLSSGState::s_structType)
    {
        InvalidateControlRoute(routeSlot,
            UniversalRouteFailure::eMalformedStructureChain);
        ScopedInternalControlBypass bypass;
        return original(viewport, state, options);
    }
    if (!IsSupportedStateVersion(stateFields.version))
    {
        InvalidateControlRoute(routeSlot,
            UniversalRouteFailure::eUnknownStateVersion);
        ScopedInternalControlBypass bypass;
        return original(viewport, state, options);
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
            return original(viewport, state, options);
        }
        if (!IsSupportedOptionsVersion(optionFields.version))
        {
            InvalidateControlRoute(routeSlot,
                UniversalRouteFailure::eUnknownOptionsVersion);
            ScopedInternalControlBypass bypass;
            return original(viewport, state, options);
        }
    }
    if (!ActivateControlRoute(routeSlot, path, false))
    {
        ScopedInternalControlBypass bypass;
        return original(viewport, state, options);
    }
    std::lock_guard callLock(gStreamlineCallMutex);
    if (path == ControlEntryPath::eResolver)
    {
        gGetStateResolverFallbackCalls.fetch_add(
            1, std::memory_order_release);
    }
    ReapplyPendingControl(viewport);
    ScopedInternalControlBypass bypass;
    const sl::Result result = original(viewport, state, options);
    RecordDlssgStateResult(result, state);
    return result;
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
    if (!enabled)
    {
        gSetOptionsSeen.store(true, std::memory_order_release);
        const sl::Result result = passThrough();
        gLastSetOptionsResult.store(static_cast<int32_t>(result),
            std::memory_order_relaxed);
        RecordSetOptionsLifecycle(*route, false, result);
        return result;
    }

    CaptureGameOptions(publicViewport, source);
    if (!gControlReady.load(std::memory_order_acquire)
        || !BridgeReady())
    {
        const sl::Result result = passThrough();
        gSetOptionsSeen.store(true, std::memory_order_release);
        gLastSetOptionsResult.store(static_cast<int32_t>(result),
            std::memory_order_relaxed);
        RecordSetOptionsLifecycle(*route, true, result);
        if (!IsAcceptedControlResult(result) || !BridgeReady())
            return result;
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
    const auto passThrough = [&]() {
        ScopedInternalControlBypass bypass;
        return original(inputs, outputs, commandBuffer);
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
    const sl::Result result = passThrough();
    auto& mutableState = *const_cast<sl::DLSSGState*>(stateView);
    RecordDlssgStateResult(result, mutableState);
    return result;
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
    const sl::Result result = original(feature, viewport);
    route->releaseCalls.fetch_add(1, std::memory_order_relaxed);
    if (gActiveControlRouteSlot.load(std::memory_order_acquire) == routeSlot
        && feature == sl::kFeatureDLSS_G
        && IsAcceptedControlResult(result)
        && route->frameGenerationOffAccepted.load(
            std::memory_order_acquire))
    {
        ResetReleasedPipelineState(*route);
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
            return slot;
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
    if (!route)
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

sl::Result HookSlGetFeatureFunction(
    sl::Feature feature, const char* functionName, void*& function)
{
    auto* original = gOriginalGetFeatureFunction.load(std::memory_order_acquire);
    if (!original)
        return sl::Result::eErrorNotInitialized;

    const sl::Result result = original(feature, functionName, function);
    if (function && functionName && strcmp(functionName, "slDLSSGSetOptions") == 0)
    {
        void* const resolved = function;
        uint32_t routeSlot = FindControlRouteForTarget(resolved);
        if (routeSlot == UINT32_MAX)
        {
            HMODULE owner = ModuleFromAddress(resolved);
            const ModuleRecord discovered = InspectLoadedModule(
                owner, LoadedModulePath(owner));
            routeSlot = discovered.controlRouteSlot;
        }
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
                route->publicSetOriginal.store(
                    reinterpret_cast<PFun_slDLSSGSetOptions*>(resolved),
                    std::memory_order_release);
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
        uint32_t routeSlot = FindControlRouteForTarget(resolved);
        if (routeSlot == UINT32_MAX)
        {
            HMODULE owner = ModuleFromAddress(resolved);
            const ModuleRecord discovered = InspectLoadedModule(
                owner, LoadedModulePath(owner));
            routeSlot = discovered.controlRouteSlot;
        }
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
                route->publicGetOriginal.store(
                    reinterpret_cast<PFun_slDLSSGGetState*>(resolved),
                    std::memory_order_release);
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
    const sl::Result result = original(viewport, tags, numTags, cmdBuffer);
    gSetTagCalls.fetch_add(1, std::memory_order_relaxed);
    if (result == sl::Result::eOk)
        CaptureUiResourceTags(viewport, tags, numTags);
    return result;
}

sl::Result HookSlSetTagForFrame(const sl::FrameToken& frame,
    const sl::ViewportHandle& viewport, const sl::ResourceTag* tags,
    uint32_t numTags, sl::CommandBuffer* cmdBuffer)
{
    auto* original = gOriginalSetTagForFrame.load(std::memory_order_acquire);
    if (!original)
        return sl::Result::eErrorNotInitialized;
    const sl::Result result = original(frame, viewport, tags, numTags, cmdBuffer);
    gSetTagForFrameCalls.fetch_add(1, std::memory_order_relaxed);
    if (result == sl::Result::eOk)
        CaptureUiResourceTags(viewport, tags, numTags);
    return result;
}

sl::Result HookSlSetD3DDevice(void* device)
{
    auto* original = gOriginalSetD3DDevice.load(std::memory_order_acquire);
    if (!original)
        return sl::Result::eErrorNotInitialized;
    if (midpoint_fix::ObserveD3D12Device(device))
        gModuleInventoryDirty.store(true, std::memory_order_release);
    return original(device);
}

sl::Result HookSlSetVulkanInfo(const VulkanInfoPrefix& info)
{
    auto* original = gOriginalSetVulkanInfo.load(std::memory_order_acquire);
    if (!original)
        return sl::Result::eErrorNotInitialized;
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
        && midpoint_fix::ObserveVulkanPhysicalDevice(physicalDevice);
    gVulkanAdapterVerified.store(verified, std::memory_order_release);
    if (verified)
        gModuleInventoryDirty.store(true, std::memory_order_release);
    return original(info);
}

bool HookModuleImport(HMODULE module, const char* importedModule,
    const char* importedFunction, void* replacement, void*& original)
{
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
            auto* current = *slot;
            if (current == replacement)
                return true;

            DWORD oldProtection = 0;
            if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &oldProtection))
                return false;
            original = current;
            InterlockedExchangePointer(
                reinterpret_cast<void* volatile*>(slot), replacement);
            DWORD ignoredProtection = 0;
            const BOOL restored = VirtualProtect(
                slot, sizeof(*slot), oldProtection, &ignoredProtection);
            FlushInstructionCache(GetCurrentProcess(), slot, sizeof(*slot));
            return restored != FALSE;
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
    if (HookModuleImport(module, "KERNEL32.dll", "LoadLibraryA",
            reinterpret_cast<void*>(&HookStreamlineLoadLibraryA), original))
    {
        installed = PreserveLoaderImportOriginal(
            gOriginalStreamlineLoadLibraryA, original,
            &HookStreamlineLoadLibraryA, L"LoadLibraryA", path) || installed;
        newlyHooked = original != nullptr || newlyHooked;
    }
    original = nullptr;
    if (HookModuleImport(module, "KERNEL32.dll", "LoadLibraryW",
            reinterpret_cast<void*>(&HookStreamlineLoadLibraryW), original))
    {
        installed = PreserveLoaderImportOriginal(
            gOriginalStreamlineLoadLibraryW, original,
            &HookStreamlineLoadLibraryW, L"LoadLibraryW", path) || installed;
        newlyHooked = original != nullptr || newlyHooked;
    }
    original = nullptr;
    if (HookModuleImport(module, "KERNEL32.dll", "LoadLibraryExA",
            reinterpret_cast<void*>(&HookStreamlineLoadLibraryExA), original))
    {
        installed = PreserveLoaderImportOriginal(
            gOriginalStreamlineLoadLibraryExA, original,
            &HookStreamlineLoadLibraryExA, L"LoadLibraryExA", path) || installed;
        newlyHooked = original != nullptr || newlyHooked;
    }
    original = nullptr;
    if (HookModuleImport(module, "KERNEL32.dll", "LoadLibraryExW",
            reinterpret_cast<void*>(&HookStreamlineLoadLibraryExW), original))
    {
        installed = PreserveLoaderImportOriginal(
            gOriginalStreamlineLoadLibraryExW, original,
            &HookStreamlineLoadLibraryExW, L"LoadLibraryExW", path) || installed;
        newlyHooked = original != nullptr || newlyHooked;
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
    auto* original = EntryOriginal(gOriginalSlInit, gSlInitEntryHandle);
    if (!original || original == &HookSlInit)
        return sl::Result::eErrorNotInitialized;

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
            before, officialSixX, coherentHost, loaderDiscovery);
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
    const sl::Result result = original(adjusted, sdkVersion);
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
    if (!resolved || !functionName
        || reinterpret_cast<uintptr_t>(functionName) <= 0xFFFFu
        || !ModuleFileNameEquals(LoadedModulePath(module),
            L"sl.interposer.dll"))
    {
        return resolved;
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
    const bool installed = HookMainExecutableImport("KERNEL32.dll",
        "GetProcAddress", reinterpret_cast<void*>(&HookMainGetProcAddress),
        original);
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
    if (!resolved || !functionName
        || reinterpret_cast<uintptr_t>(functionName) <= 0xFFFFu)
        return resolved;
    const bool create = std::strcmp(functionName,
        "NVSDK_NGX_D3D12_CreateFeature") == 0;
    const bool evaluate = std::strcmp(functionName,
        "NVSDK_NGX_D3D12_EvaluateFeature") == 0;
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
    if (!create && !evaluate && !vulkan)
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

struct PatternPatch
{
    const wchar_t* label;
    const uint8_t* pattern;
    size_t patternSize;
    size_t patchOffset;
    const uint8_t* original;
    const uint8_t* replacement;
    size_t patchSize;
};

static constexpr std::array<uint8_t, 3> kWrapperOriginal{ 0x0F, 0x42, 0xD1 };
static constexpr std::array<uint8_t, 3> kWrapperReplacement{ 0x90, 0x90, 0x90 };

static constexpr std::array<uint8_t, 13> kNgxPattern{
    0x84, 0xD2, 0x0F, 0x84, 0x03, 0x01, 0x00, 0x00, 0xBE, 0x05, 0x00, 0x00, 0x00
};
static constexpr std::array<uint8_t, 6> kNgxOriginal{ 0x0F, 0x84, 0x03, 0x01, 0x00, 0x00 };
static constexpr std::array<uint8_t, 6> kNgxReplacement{ 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
static const PatternPatch kNgxPatch{
    L"NGX device support", kNgxPattern.data(), kNgxPattern.size(), 2,
    kNgxOriginal.data(), kNgxReplacement.data(), kNgxOriginal.size()
};

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

PatternPatchResult PatchUniqueExecutablePattern(
    HMODULE module, const std::wstring& path, const PatternPatch& patch)
{
    const auto* base = reinterpret_cast<const uint8_t*>(module);
    const auto* nt = ImageHeaders(module);
    if (!nt)
        return {};

    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    uint8_t* match = nullptr;
    size_t matchCount = 0;
    for (unsigned index = 0; index < nt->FileHeader.NumberOfSections; ++index, ++section)
    {
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0)
            continue;
        auto* begin = const_cast<uint8_t*>(base + section->VirtualAddress);
        if (section->VirtualAddress >= nt->OptionalHeader.SizeOfImage)
            continue;
        const size_t available = nt->OptionalHeader.SizeOfImage - section->VirtualAddress;
        const size_t size = std::min<size_t>(available,
            std::max<size_t>(section->Misc.VirtualSize, section->SizeOfRawData));
        if (size < patch.patternSize)
            continue;
        const size_t suffixOffset = patch.patchOffset + patch.patchSize;
        for (size_t offset = 0; offset + patch.patternSize <= size; ++offset)
        {
            const bool prefixMatches = patch.patchOffset == 0
                || memcmp(begin + offset, patch.pattern, patch.patchOffset) == 0;
            const bool suffixMatches = suffixOffset == patch.patternSize
                || memcmp(begin + offset + suffixOffset, patch.pattern + suffixOffset,
                    patch.patternSize - suffixOffset) == 0;
            const auto* candidate = begin + offset + patch.patchOffset;
            const bool patchBytesMatch = memcmp(candidate, patch.original, patch.patchSize) == 0
                || memcmp(candidate, patch.replacement, patch.patchSize) == 0;
            if (prefixMatches && suffixMatches && patchBytesMatch)
            {
                match = begin + offset;
                ++matchCount;
            }
        }
    }

    if (matchCount == 0)
        return {};
    if (matchCount != 1 || !match)
    {
        Log(L"%s: expected one code pattern, found %zu: %s", patch.label, matchCount, path.c_str());
        return {true, false, nullptr};
    }

    uint8_t* address = match + patch.patchOffset;
    if (memcmp(address, patch.replacement, patch.patchSize) == 0)
    {
        Log(L"%s: already patched at RVA 0x%zX: %s", patch.label,
            static_cast<size_t>(address - const_cast<uint8_t*>(base)), path.c_str());
        return {true, true, match};
    }
    if (memcmp(address, patch.original, patch.patchSize) != 0)
    {
        Log(L"%s: matched context but original bytes differ: %s", patch.label, path.c_str());
        return {true, false, match};
    }

    DWORD oldProtection = 0;
    if (!VirtualProtect(address, patch.patchSize, PAGE_EXECUTE_READWRITE, &oldProtection))
    {
        Log(L"%s: VirtualProtect failed (%lu): %s", patch.label, GetLastError(), path.c_str());
        return {true, false, match};
    }
    memcpy(address, patch.replacement, patch.patchSize);
    FlushInstructionCache(GetCurrentProcess(), address, patch.patchSize);
    DWORD ignoredProtection = 0;
    const BOOL restored = VirtualProtect(address, patch.patchSize, oldProtection, &ignoredProtection);
    if (!restored)
    {
        Log(L"%s: protection restore failed (%lu): %s", patch.label, GetLastError(), path.c_str());
        return {true, false, match};
    }

    Log(L"%s: patched RVA 0x%zX: %s", patch.label,
        static_cast<size_t>(address - const_cast<uint8_t*>(base)), path.c_str());
    return {true, true, match};
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
                if (existing->wrapperCandidate
                    && existing->controlRouteSlot == UINT32_MAX)
                {
                    existing->controlRouteSlot = EnsureControlRoute(
                        existing->module, existing->path,
                        existing->generation, existing->wrapperPatched,
                        existing->wrapperCompiledMaximumGeneratedFrames);
                }
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
                const PatternPatchResult result =
                    PatchUniqueExecutablePattern(module, path, kNgxPatch);
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
    }
    if (snapshot.ngxRuntimeExport)
    {
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

void RemoveLoadedModule(HMODULE module)
{
    if (!module)
        return;
    {
        std::lock_guard lock(gModuleMutex);
        gModuleRecords.erase(std::remove_if(gModuleRecords.begin(), gModuleRecords.end(),
            [&](const ModuleRecord& record) { return record.module == module; }),
            gModuleRecords.end());
        RecomputeModuleStateLocked();
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(module);
    if (ControlRouteRecord* route = ActiveControlRoute();
        route && route->wrapper == module)
    {
        gActiveControlRouteSlot.store(UINT32_MAX,
            std::memory_order_release);
        SetUniversalRouteFailure(UniversalRouteFailure::eNoActiveRoute);
    }
    if (gActiveWrapperBase.load(std::memory_order_acquire) == base)
    {
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
            InspectLoadedModule(module, entry.szExePath);
            entry.dwSize = sizeof(entry);
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);

    std::vector<HMODULE> removedModules;
    {
        std::lock_guard lock(gModuleMutex);
        for (const ModuleRecord& record : gModuleRecords)
        {
            if (std::find(loadedModules.begin(), loadedModules.end(),
                    record.module) == loadedModules.end())
                removedModules.push_back(record.module);
        }
    }
    for (HMODULE module : removedModules)
        RemoveLoadedModule(module);
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
        gModuleInventoryDirty.store(true, std::memory_order_release);
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
    const DWORD pid = GetCurrentProcessId();
    wchar_t tempDirectory[MAX_PATH]{};
    DWORD tempLength = GetTempPathW(_countof(tempDirectory), tempDirectory);
    std::wstring logPath;
    if (tempLength > 0 && tempLength < _countof(tempDirectory))
    {
        wchar_t logName[64]{};
        swprintf_s(logName, L"MfgUnlock-%lu.log", static_cast<unsigned long>(pid));
        logPath = JoinPath(tempDirectory, logName);
        gLog = _wfsopen(logPath.c_str(), L"w, ccs=UTF-8", _SH_DENYWR);
    }
    gLogReady.store(gLog != nullptr, std::memory_order_release);

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
    DeleteFileW(gStatusPath.c_str());
    temporal_interval_trace::Initialize(tempDirectory, pid);
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
        ++inventoryTicks;
        if (gModuleInventoryDirty.exchange(false, std::memory_order_acq_rel))
        {
            inventoryTicks = 0;
            InspectAlreadyLoadedModules();
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
                    temporal_interval_trace::SetEnabled(true);
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

        temporal_interval_trace::Flush();

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

extern "C" __declspec(dllexport) BOOL WINAPI
MfgUnlockSampleFrameTelemetry()
{
    gFpsOutputPresentTick.store(GetTickCount64(), std::memory_order_release);
    std::lock_guard telemetryLock(gFpsTelemetryMutex);
    if (!gGameFrameGenerationOn.load(std::memory_order_acquire))
    {
        if (gFpsTelemetryActive)
            ResetFpsTelemetry();
        gFpsTelemetryActive = false;
        return FALSE;
    }

    const temporal_interval_trace::Snapshot intervalTrace =
        temporal_interval_trace::ReadSnapshot();
    if (!intervalTrace.initialized || !intervalTrace.enabled)
    {
        if (gFpsTelemetryActive)
            ResetFpsTelemetry();
        gFpsTelemetryActive = false;
        return FALSE;
    }

    if (!gFpsTelemetryActive)
    {
        ResetFpsTelemetry();
        gFpsTelemetryActive = true;
    }

    UpdateFpsTelemetryForOutputPresent();
    return TRUE;
}

extern "C" __declspec(dllexport) BOOL WINAPI MfgUnlockCoreLoaded()
{
    return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
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
        midpoint_fix::SetLogCallback(&MidpointLog);
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
