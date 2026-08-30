#include "shared.h"
#include "present_probe.h"
#include "ngx_output_probe.h"

#include <Windows.h>
#include <TlHelp32.h>
#include <winternl.h>
#include <d3d12.h>
#include <sl.h>
#include <sl_core_api.h>
#include <sl_dlss_g.h>
#include <nvsdk_ngx.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <intrin.h>
#include <mutex>
#include <share.h>
#include <string>
#include <vector>

namespace
{
enum class FeatureRecycleStage : uint32_t
{
    eIdle = 0,
    eWaitingForOffState = 1,
    eWaitingForDrain = 2,
    eReenabling = 3,
};

using D3D12CreateDeviceFn = HRESULT (WINAPI*)(
    IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);

FILE* gLog = nullptr;
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
std::atomic<PFun_slUpgradeInterface*> gOriginalUpgradeInterface{nullptr};
std::atomic<D3D12CreateDeviceFn> gOriginalD3D12CreateDevice{nullptr};
std::atomic<PFun_slSetTag*> gOriginalSetTag{nullptr};
std::atomic<PFun_slSetTagForFrame*> gOriginalSetTagForFrame{nullptr};
std::atomic<PFun_slDLSSGSetOptions*> gOriginalSetOptions{nullptr};
std::atomic<PFun_slDLSSGGetState*> gOriginalGetState{nullptr};
std::atomic<PFun_slFreeResources*> gOriginalFreeResources{nullptr};
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
std::atomic<uint64_t> gGetStateCalls{0};
std::atomic<uint64_t> gLiveReapplyCount{0};
std::atomic<uint64_t> gNotInitializedRetryCount{0};
std::atomic<bool> gStreamlineRebuildRequired{false};
std::atomic<uint64_t> gDeferredLifecycleRevision{0};
std::atomic<uint64_t> gLifecycleDeferralLoggedRevision{0};
std::atomic<uint64_t> gCleanEnableApplyCount{0};
std::atomic<uint64_t> gHostLifecycleResetCount{0};
std::atomic<bool> gCleanEnableBoundaryAvailable{true};
std::atomic<bool> gCleanEnableRetryPending{false};
std::atomic<uint64_t> gMissedCleanEnableCount{0};
std::atomic<uint32_t> gFeatureRecycleStage{
    static_cast<uint32_t>(FeatureRecycleStage::eIdle)};
std::atomic<uint64_t> gFeatureRecycleRevision{0};
std::atomic<uint64_t> gFeatureRecycleCount{0};
std::atomic<uint64_t> gFeatureRecycleFreeCalls{0};
std::atomic<int32_t> gFeatureRecycleLastFreeResult{
    static_cast<int32_t>(sl::Result::eErrorNotInitialized)};
std::atomic<bool> gFeatureRecycleOffStateObserved{false};
std::atomic<uint64_t> gFeatureRecycleStatePolls{0};
std::atomic<uint64_t> gFeatureRecycleFenceValue{0};
std::atomic<uint64_t> gFeatureRecycleFenceCompletedValue{0};
std::atomic<uint32_t> gFeatureRecycleOutstandingOutputs{0};
std::atomic<bool> gFeatureRecycleExplicitFreeSkipped{false};
std::atomic<bool> gNgxEvaluateLookupHookInstalled{false};
std::atomic<bool> gNgxEvaluateHookExposed{false};
std::atomic<bool> gNgxEvaluateSeen{false};
std::atomic<uint64_t> gNgxEvaluateCalls{0};
std::atomic<uint64_t> gNgxTemporalValidCount{0};
std::atomic<uint64_t> gNgxTemporalInvalidCount{0};
std::atomic<uint32_t> gNgxSeenCountMask{0};
std::atomic<uint32_t> gNgxSeenIndexMask{0};
std::atomic<int32_t> gNgxLastCountGetResult{
    static_cast<int32_t>(NVSDK_NGX_Result_FAIL_NotInitialized)};
std::atomic<int32_t> gNgxLastIndexGetResult{
    static_cast<int32_t>(NVSDK_NGX_Result_FAIL_NotInitialized)};
std::atomic<int32_t> gNgxLastRawCount{0};
std::atomic<int32_t> gNgxLastRawIndex{0};
std::atomic<int32_t> gNgxLastFrameIdGetResult{
    static_cast<int32_t>(NVSDK_NGX_Result_FAIL_NotInitialized)};
std::atomic<int32_t> gNgxLastOutputGetResult{
    static_cast<int32_t>(NVSDK_NGX_Result_FAIL_NotInitialized)};
std::atomic<uint64_t> gNgxOutputCompleteBatches{0};
std::atomic<uint64_t> gNgxOutputAliasedBatches{0};
std::atomic<uint32_t> gNgxLastOutputUniqueCount{0};
std::atomic<uint64_t> gNgxLastFrameId{0};
std::atomic<uintptr_t> gNgxLastOutputInterpolated{0};
std::atomic<bool> gNgxFullStateRepairActive{false};
std::atomic<uint64_t> gNgxFullStateForcedCount{0};
std::atomic<int32_t> gNgxLastDisableInterpolationGetResult{
    static_cast<int32_t>(NVSDK_NGX_Result_FAIL_NotInitialized)};
std::atomic<uintptr_t> gNgxLastDisableInterpolationResource{0};
std::atomic<uint64_t> gNgxDisableInterpolationResourcePreservedCount{0};
std::atomic<int32_t> gNgxLastResetGetResult{
    static_cast<int32_t>(NVSDK_NGX_Result_FAIL_NotInitialized)};
std::atomic<int32_t> gNgxLastRawReset{0};
std::atomic<uint64_t> gNgxResetPreservedCount{0};
std::atomic<bool> gForceFullNgxState{false};
std::atomic<bool> gDisableAutomaticFullNgxState{false};
std::atomic<bool> gExperimentalSm120Target{false};
std::atomic<bool> gExperimentalDl4rtSm120Path{false};
std::atomic<bool> gExperimentalTemporalPreEmphasis{false};
std::atomic<bool> gExperimentalTemporalPreEmphasisPatchActive{false};
std::atomic<bool> gPresentProbeEnvironmentEnabled{false};
std::atomic<bool> gNgxOutputProbeEnvironmentEnabled{false};
std::atomic<bool> gImmutableOutputEnvironmentEnabled{false};
std::atomic<uint64_t> gForcedBackbufferFrameId{1};
std::atomic<uint64_t> gNgxOutputBatchSequence{0};
std::atomic<bool> gDllNotificationRegistered{false};
std::atomic<bool> gLiveHookInstalled{false};
std::atomic<bool> gD3DDeviceHookInstalled{false};
std::atomic<bool> gD3D12CreateDeviceHookInstalled{false};
std::atomic<bool> gSlUpgradeInterfaceHookInstalled{false};
std::atomic<bool> gEarlyD3D12DeviceCaptured{false};
std::atomic<bool> gUiTagHookInstalled{false};
std::atomic<uint32_t> gLoadedWrapperCandidates{0};
std::atomic<uint32_t> gPatchedWrapperCandidates{0};
std::atomic<uint32_t> gLoadedNgxCandidates{0};
std::atomic<uint32_t> gPatchedNgxCandidates{0};
std::atomic<uint32_t> gLoadedNgxSynthesisCandidates{0};
std::atomic<uint32_t> gPatchedNgxSynthesisCandidates{0};
std::atomic<bool> gPerSampleSynthesisReady{false};
std::atomic<uint64_t> gSynthesisFallbackLoggedRevision{0};
std::atomic<uint32_t> gWrapperRouteBits{0};
std::atomic<uint32_t> gNgxRouteBits{0};
std::atomic<bool> gActiveWrapperObserved{false};
std::atomic<bool> gActiveWrapperPatched{false};
std::atomic<uintptr_t> gActiveWrapperBase{0};
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
std::atomic<bool> gLogReady{false};
std::mutex gStreamlineCallMutex;
std::mutex gLastOptionsMutex;
std::mutex gModuleMutex;
std::mutex gUiTagMutex;
std::mutex gNgxEvaluateRouteMutex;
std::mutex gNgxOutputBatchMutex;
std::mutex gNgxForceStateMutex;
std::wstring gConfigPath;
std::wstring gStatusPath;
std::wstring gExecutableDirectory;

constexpr uint32_t kRouteLocal = 1u;
constexpr uint32_t kRouteExternal = 2u;
constexpr uint32_t kMinimumMultiplier = 2u;
constexpr uint32_t kMaximumMultiplier = 6u;
constexpr uint8_t kStandardMaximumGeneratedFrames = 3u;
constexpr uint8_t kExperimentalMaximumGeneratedFrames = 5u;
constexpr uint64_t kNotInitializedRetryDelayMs = 500;
constexpr uint64_t kUiTagFreshnessMs = 2500;

struct ControlConfig
{
    uint32_t multiplier = 2;
    bool dynamic = false;
    uint32_t dynamicTargetFrameRate = 0;
    bool dynamicExperimental56 = false;
    bool generatedOnlyDebug = false;
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

struct FeatureRecycleContext
{
    FeatureRecycleStage stage = FeatureRecycleStage::eIdle;
    ControlSnapshot snapshot{};
    sl::ViewportHandle viewport{0u};
    sl::DLSSGOptions source{};
    uint64_t offSubmittedTick = 0;
};

struct ModuleRecord
{
    HMODULE module = nullptr;
    std::wstring path;
    bool wrapperExport = false;
    bool wrapperCandidate = false;
    bool wrapperPatched = false;
    uint8_t* wrapperMaximumImmediate = nullptr;
    bool ngxEvaluateLookupHooked = false;
    bool ngxExport = false;
    bool ngxCandidate = false;
    bool ngxPatched = false;
    bool ngxAdaSynthesisCandidate = false;
    bool ngxAdaSynthesisPatched = false;
    bool ngxMultiFrameMaximumCandidate = false;
    bool ngxMultiFrameMaximumPatched = false;
    bool ngxCachedDlssgImplementation = false;
    bool ngxTemporalPreEmphasisCandidate = false;
    bool ngxTemporalPreEmphasisPatched = false;
    bool inventoryLogged = false;
};

using GetProcAddressFn = FARPROC (WINAPI*)(HMODULE, LPCSTR);
using NgxD3D12EvaluateFeatureFn = NVSDK_NGX_Result (NVSDK_CONV*)(
    ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
    const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);

struct NgxEvaluateRoute
{
    HMODULE wrapper = nullptr;
    GetProcAddressFn originalGetProcAddress = nullptr;
    NgxD3D12EvaluateFeatureFn originalEvaluate = nullptr;
};

struct NgxOutputBatch
{
    const NVSDK_NGX_Handle* handle = nullptr;
    uint64_t sequence = 0;
    int count = 0;
    std::array<uintptr_t, kExperimentalMaximumGeneratedFrames> outputs{};
    uint32_t seenMask = 0;
};

struct NgxForceEpoch
{
    const NVSDK_NGX_Handle* handle = nullptr;
    int count = 0;
    uint32_t firstBatchSeenMask = 0;
    bool firstBatchComplete = false;
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
FeatureRecycleContext gFeatureRecycle;
std::vector<ModuleRecord> gModuleRecords;
std::vector<UiViewportTagState> gUiViewportTags;
std::vector<NgxEvaluateRoute> gNgxEvaluateRoutes;
std::vector<NgxOutputBatch> gNgxOutputBatches;
std::vector<NgxForceEpoch> gNgxForceEpochs;
LARGE_INTEGER gFpsCounterFrequency{};
LARGE_INTEGER gFpsWindowStart{};
uint64_t gFpsWindowRealFrames = 0;
uint64_t gFpsWindowPresentedFrames = 0;

void ObserveActiveWrapperProvider(void* function);
FARPROC WINAPI HookWrapperGetProcAddress(HMODULE module, LPCSTR functionName);
NVSDK_NGX_Result NVSDK_CONV HookNgxD3D12EvaluateFeature(
    ID3D12GraphicsCommandList* commandList, const NVSDK_NGX_Handle* featureHandle,
    const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback);

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

void ProbeLog(const wchar_t* message)
{
    Log(L"%s", message);
}

uint8_t RequestedMaximumGeneratedFrames(const ControlConfig& control)
{
    return control.dynamic && !control.dynamicExperimental56
        ? kStandardMaximumGeneratedFrames
        : kExperimentalMaximumGeneratedFrames;
}

uint8_t RequestedActiveGeneratedFrames(const ControlConfig& control)
{
    return control.dynamic
        ? RequestedMaximumGeneratedFrames(control)
        : static_cast<uint8_t>(std::clamp(
            control.multiplier, kMinimumMultiplier, kMaximumMultiplier) - 1u);
}

bool SetWrapperMaximum(ModuleRecord& record, uint8_t maximum)
{
    uint8_t* address = record.wrapperMaximumImmediate;
    if (!address || (*address != kStandardMaximumGeneratedFrames
        && *address != kExperimentalMaximumGeneratedFrames))
        return false;
    if (*address == maximum)
        return true;

    DWORD oldProtection = 0;
    if (!VirtualProtect(address, 1, PAGE_EXECUTE_READWRITE, &oldProtection))
    {
        Log(L"Streamline maximum update failed (%lu): %s",
            GetLastError(), record.path.c_str());
        return false;
    }
    *address = maximum;
    FlushInstructionCache(GetCurrentProcess(), address, 1);
    DWORD ignoredProtection = 0;
    const BOOL restored = VirtualProtect(address, 1, oldProtection, &ignoredProtection);
    if (!restored)
    {
        Log(L"Streamline maximum protection restore failed (%lu): %s",
            GetLastError(), record.path.c_str());
        return false;
    }

    Log(L"Streamline maximum updated: generatedFrames=%u multiplier=%ux path=%s",
        maximum, static_cast<uint32_t>(maximum) + 1, record.path.c_str());
    return true;
}

void ApplyWrapperMaximum(const ControlConfig& control)
{
    const uint8_t maximum = RequestedMaximumGeneratedFrames(control);
    std::lock_guard lock(gModuleMutex);
    for (auto& record : gModuleRecords)
    {
        if (record.wrapperPatched && record.wrapperMaximumImmediate)
            SetWrapperMaximum(record, maximum);
    }
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

void UpdateFpsTelemetry(uint32_t presentedFrames)
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
        gFpsWindowRealFrames = 0;
        gFpsWindowPresentedFrames = 0;
        return;
    }

    ++gFpsWindowRealFrames;
    gFpsWindowPresentedFrames += presentedFrames;
    const uint64_t elapsedTicks = static_cast<uint64_t>(
        now.QuadPart - gFpsWindowStart.QuadPart);
    const uint64_t minimumTicks = static_cast<uint64_t>(
        gFpsCounterFrequency.QuadPart) / 2;
    if (elapsedTicks < minimumTicks)
        return;

    const uint64_t frequency = static_cast<uint64_t>(gFpsCounterFrequency.QuadPart);
    const auto rateMilli = [&](uint64_t frames) {
        return static_cast<uint32_t>(std::min<uint64_t>(UINT32_MAX,
            (frames * frequency * 1000u + elapsedTicks / 2u) / elapsedTicks));
    };
    gRealFpsMilli.store(rateMilli(gFpsWindowRealFrames), std::memory_order_relaxed);
    gDlssFpsMilli.store(
        rateMilli(gFpsWindowPresentedFrames), std::memory_order_relaxed);
    gFpsSampleWindowMs.store(static_cast<uint32_t>(
        std::min<uint64_t>(UINT32_MAX,
            (elapsedTicks * 1000u + frequency / 2u) / frequency)),
        std::memory_order_relaxed);
    gFpsSampleTick.store(GetTickCount64(), std::memory_order_release);
    gFpsWindowStart = now;
    gFpsWindowRealFrames = 0;
    gFpsWindowPresentedFrames = 0;
}

void RecordDlssgStateResult(
    sl::Result result, const sl::DLSSGState& state, bool fpsFrameSample)
{
    gGetStateCalls.fetch_add(1, std::memory_order_relaxed);
    gGetStateSeen.store(true, std::memory_order_release);
    gLastGetStateResult.store(static_cast<int32_t>(result), std::memory_order_relaxed);
    if (result != sl::Result::eOk)
    {
        if (fpsFrameSample)
            UpdateFpsTelemetry(0);
        return;
    }

    const uint32_t previous =
        gActualFramesPresented.exchange(state.numFramesActuallyPresented,
            std::memory_order_relaxed);
    if (fpsFrameSample)
        UpdateFpsTelemetry(state.numFramesActuallyPresented);
    gDlssgStatus.store(static_cast<uint32_t>(state.status), std::memory_order_relaxed);
    if (state.structVersion >= sl::kStructVersion2)
        gNumFramesToGenerateMax.store(
            state.numFramesToGenerateMax, std::memory_order_relaxed);
    if (state.structVersion >= sl::kStructVersion3
        && state.inputsProcessingCompletionFence
        && state.lastPresentInputsProcessingCompletionFenceValue != 0)
    {
        ngx_output_probe::NotifyStreamlineCompletionFence(
            static_cast<ID3D12Fence*>(state.inputsProcessingCompletionFence),
            state.lastPresentInputsProcessingCompletionFenceValue);
    }
    if (state.structVersion >= sl::kStructVersion4)
        gDynamicMfgSupported.store(
            state.bIsDynamicMFGSupported == sl::Boolean::eTrue,
            std::memory_order_relaxed);
    gStateSampleTick.store(GetTickCount64(), std::memory_order_release);

    if (gFeatureRecycleStage.load(std::memory_order_acquire)
            == static_cast<uint32_t>(FeatureRecycleStage::eWaitingForOffState)
        && state.numFramesActuallyPresented <= 1)
    {
        gFeatureRecycleOffStateObserved.store(true, std::memory_order_release);
    }

    if (previous != state.numFramesActuallyPresented)
        Log(L"DLSS-G actual presentation count: %ux (maximum generated frames=%u, status=%u)",
            state.numFramesActuallyPresented,
            gNumFramesToGenerateMax.load(std::memory_order_relaxed),
            static_cast<uint32_t>(state.status));
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

bool BridgeReady()
{
    return gLiveHookInstalled.load(std::memory_order_acquire)
        && gSetOptionsHookExposed.load(std::memory_order_acquire)
        && gActiveWrapperObserved.load(std::memory_order_acquire)
        && gActiveWrapperPatched.load(std::memory_order_acquire)
        && gPatchedNgxCandidates.load(std::memory_order_acquire) > 0;
}

bool PerSampleSynthesisReady()
{
    return gPerSampleSynthesisReady.load(std::memory_order_acquire);
}

bool RequiresPerSampleSynthesis(const ControlConfig& control)
{
    return control.dynamic || control.multiplier > kMinimumMultiplier;
}

ControlSnapshot EffectiveControlSnapshot(const ControlSnapshot& requested)
{
    ControlSnapshot effective = requested;
    if (RequiresPerSampleSynthesis(requested.control) && !PerSampleSynthesisReady())
    {
        effective.control.dynamic = false;
        effective.control.multiplier = kMinimumMultiplier;
        effective.control.dynamicExperimental56 = false;
    }
    return effective;
}

const char* PatchRouteName()
{
    if (!BridgeReady())
        return "pending";

    const uint32_t wrapperBits = gWrapperRouteBits.load(std::memory_order_acquire);
    const uint32_t ngxBits = gNgxRouteBits.load(std::memory_order_acquire);
    const uint32_t common = wrapperBits & ngxBits;
    if ((common & (kRouteLocal | kRouteExternal)) == (kRouteLocal | kRouteExternal))
        return "both";
    if ((common & kRouteLocal) != 0)
        return "local";
    if ((common & kRouteExternal) != 0)
        return "external";
    return "mixed";
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
    if (!TryParseUnsigned(content, "multiplier",
        kMinimumMultiplier, kMaximumMultiplier, parsed.multiplier))
        return false;

    size_t modeOffset = 0;
    if (FindJsonValue(content, "mode", modeOffset))
    {
        if (content.compare(modeOffset, 9, "\"dynamic\"") == 0)
            parsed.dynamic = true;
        else if (content.compare(modeOffset, 7, "\"fixed\"") != 0)
            return false;
    }

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

    size_t generatedOnlyOffset = 0;
    if (FindJsonValue(content, "generatedOnlyDebug", generatedOnlyOffset)
        && !TryParseBoolean(content, "generatedOnlyDebug",
            parsed.generatedOnlyDebug))
        return false;

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
    return ReadControlFile(gConfigPath, fileControl) ? fileControl : control;
}

bool ReadEnvironmentFlag(const wchar_t* name)
{
    wchar_t value[8]{};
    const DWORD length = GetEnvironmentVariableW(
        name, value, static_cast<DWORD>(std::size(value)));
    return length == 1 && value[0] == L'1';
}

std::wstring ResolveConfigPath(HMODULE instance, const std::wstring& executableDirectory)
{
    wchar_t explicitPath[32768]{};
    const DWORD explicitLength = GetEnvironmentVariableW(
        L"RTX40_MFG_CONFIG_PATH", explicitPath, _countof(explicitPath));
    if (explicitLength > 0 && explicitLength < _countof(explicitPath))
        return std::wstring(explicitPath, explicitLength);

    const std::wstring cetPath = JoinPath(executableDirectory,
        L"plugins\\cyber_engine_tweaks\\mods\\RTX40MFG\\config.json");
    if (IsRegularFile(cetPath))
        return cetPath;

    wchar_t modulePath[32768]{};
    GetModuleFileNameW(instance, modulePath, _countof(modulePath));
    const std::wstring legacyPath = JoinPath(ParentPath(ParentPath(modulePath)), L"config.json");
    return IsRegularFile(legacyPath) ? legacyPath : cetPath;
}

uint64_t StoreControl(const ControlConfig& control)
{
    // Do not alter the active wrapper's allocation maximum from this watcher
    // thread. Multiplier-shape changes are applied on the serialized render
    // path only after DLSS-G has entered Off and its work has drained.
    gDesiredMultiplier.store(control.multiplier, std::memory_order_relaxed);
    gDesiredDynamicMode.store(control.dynamic, std::memory_order_relaxed);
    gDynamicTargetFrameRate.store(control.dynamicTargetFrameRate, std::memory_order_relaxed);
    gDynamicExperimental56.store(control.dynamicExperimental56, std::memory_order_relaxed);
    gGeneratedOnlyDebug.store(control.generatedOnlyDebug, std::memory_order_relaxed);
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
        snapshot.control.multiplier = gDesiredMultiplier.load(std::memory_order_relaxed);
        snapshot.control.dynamic = gDesiredDynamicMode.load(std::memory_order_relaxed);
        snapshot.control.dynamicTargetFrameRate =
            gDynamicTargetFrameRate.load(std::memory_order_relaxed);
        snapshot.control.dynamicExperimental56 =
            gDynamicExperimental56.load(std::memory_order_relaxed);
        snapshot.control.generatedOnlyDebug =
            gGeneratedOnlyDebug.load(std::memory_order_relaxed);
        const uint64_t after = gDesiredRevision.load(std::memory_order_acquire);
        if (before == after)
        {
            snapshot.revision = after;
            return snapshot;
        }
    }
}

ControlSnapshot ReadAppliedControlSnapshot()
{
    ControlSnapshot snapshot{};
    snapshot.revision = gAppliedRevision.load(std::memory_order_acquire);
    snapshot.control.dynamic =
        gAppliedDynamicMode.load(std::memory_order_relaxed);
    snapshot.control.multiplier =
        gAppliedMultiplier.load(std::memory_order_relaxed);
    snapshot.control.dynamicTargetFrameRate =
        gAppliedDynamicTargetFrameRate.load(std::memory_order_relaxed);
    snapshot.control.dynamicExperimental56 =
        gAppliedDynamicExperimental56.load(std::memory_order_relaxed);
    snapshot.control.generatedOnlyDebug =
        gAppliedGeneratedOnlyDebug.load(std::memory_order_relaxed);
    return snapshot;
}

void PublishLiveBridge(const ControlConfig& control)
{
    wchar_t multiplier[2]{ static_cast<wchar_t>(L'0' + std::clamp(
        control.multiplier, kMinimumMultiplier, kMaximumMultiplier)), L'\0' };
    wchar_t target[16]{};
    swprintf_s(target, L"%u", control.dynamicTargetFrameRate);
    SetEnvironmentVariableW(L"RTX40_MFG_ACTIVE_MULTIPLIER", multiplier);
    SetEnvironmentVariableW(L"RTX40_MFG_ACTIVE_MODE", control.dynamic ? L"dynamic" : L"fixed");
    SetEnvironmentVariableW(L"RTX40_MFG_DYNAMIC_TARGET", target);
    SetEnvironmentVariableW(L"RTX40_MFG_DYNAMIC_EXPERIMENTAL_56",
        control.dynamicExperimental56 ? L"1" : L"0");
    SetEnvironmentVariableW(L"RTX40_MFG_GENERATED_ONLY_DEBUG",
        control.generatedOnlyDebug ? L"1" : L"0");
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

bool WriteBridgeStatus(const ControlConfig& control, DWORD pid)
{
    if (gStatusPath.empty())
        return false;

    const uint32_t uiViewport = gLastOptionsViewport.load(std::memory_order_acquire);
    RefreshUiInputReadiness(uiViewport);
    const UiInputSnapshot uiInputs = ReadUiInputSnapshot(uiViewport);
    const bool bridgeReady = BridgeReady();
    const bool perSampleSynthesisReady = PerSampleSynthesisReady();
    const bool synthesisFallbackActive =
        RequiresPerSampleSynthesis(control) && !perSampleSynthesisReady;
    const uint32_t effectiveMaximumMultiplier = synthesisFallbackActive
        ? kMinimumMultiplier
        : static_cast<uint32_t>(RequestedMaximumGeneratedFrames(control)) + 1;
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
    const uint32_t featureRecycleStage =
        gFeatureRecycleStage.load(std::memory_order_acquire);
    const bool featureRecycleActive = featureRecycleStage
        != static_cast<uint32_t>(FeatureRecycleStage::eIdle);
    const bool applied = gameFrameGenerationOn && appliedRevision != 0
        && setOptionsAccepted && !featureRecycleActive;
    const bool pending = gameFrameGenerationOn
        && (desiredRevision != appliedRevision || featureRecycleActive);
    const uint64_t stateTick = gStateSampleTick.load(std::memory_order_acquire);
    const uint64_t nowTick = GetTickCount64();
    const uint64_t stateAgeMs = stateTick == 0 || nowTick < stateTick
        ? 0 : nowTick - stateTick;
    const uint64_t fpsTick = gFpsSampleTick.load(std::memory_order_acquire);
    const uint64_t fpsAgeMs = fpsTick == 0 || nowTick < fpsTick
        ? 0 : nowTick - fpsTick;
    const present_probe::Snapshot presentProbe = present_probe::ReadSnapshot();
    const ngx_output_probe::Snapshot ngxOutputProbe =
        ngx_output_probe::ReadSnapshot();

    char json[8192]{};
    const int length = sprintf_s(json,
        "{\"version\":23,\"pid\":%lu,\"heartbeat\":%llu,\"route\":\"%s\","
        "\"bridgeReady\":%s,\"liveHookInstalled\":%s,"
        "\"d3dDeviceHookInstalled\":%s,"
        "\"d3d12CreateDeviceHookInstalled\":%s,"
        "\"slUpgradeInterfaceHookInstalled\":%s,"
        "\"earlyD3D12DeviceCaptured\":%s,"
        "\"uiTagHookInstalled\":%s,"
        "\"ngxEvaluateLookupHookInstalled\":%s,"
        "\"ngxEvaluateHookExposed\":%s,\"ngxEvaluateSeen\":%s,"
        "\"presentFactoryImportHookInstalled\":%s,"
        "\"presentNativeFactoryHookInstalled\":%s,"
        "\"presentNativeSwapchainHookInstalled\":%s,"
        "\"presentProbeEnabled\":%s,"
        "\"nativePresentCalls\":%llu,"
        "\"presentProbeScheduledFrames\":%llu,"
        "\"presentProbeCapturedFrames\":%llu,"
        "\"presentProbeDroppedFrames\":%llu,"
        "\"ngxOutputProbeEnabled\":%s,"
        "\"ngxOutputQueueHookInstalled\":%s,"
        "\"ngxOutputProbeScheduled\":%llu,"
        "\"ngxOutputProbeSubmitted\":%llu,"
        "\"ngxOutputProbeCaptured\":%llu,"
        "\"ngxOutputProbeDropped\":%llu,"
        "\"ngxOutputProbeCompleteBatches\":%llu,"
        "\"ngxOutputProbeDuplicateBatches\":%llu,"
        "\"ngxImmutableOutputsEnabled\":%s,"
        "\"ngxImmutablePrepared\":%llu,"
        "\"ngxImmutableSubmitted\":%llu,"
        "\"ngxImmutableRetired\":%llu,"
        "\"ngxImmutableDropped\":%llu,"
        "\"ngxImmutableReservationReclaims\":%llu,"
        "\"ngxImmutableAllocated\":%u,"
        "\"activeWrapperObserved\":%s,\"activeWrapperPatched\":%s,"
        "\"loadedWrapperCandidates\":%u,\"patchedWrapperCandidates\":%u,"
        "\"loadedNgxCandidates\":%u,\"patchedNgxCandidates\":%u,"
        "\"loadedNgxSynthesisCandidates\":%u,"
        "\"patchedNgxSynthesisCandidates\":%u,"
        "\"perSampleSynthesisReady\":%s,\"synthesisFallbackActive\":%s,"
        "\"mode\":\"%s\",\"multiplier\":%u,\"dynamicTargetFrameRate\":%u,"
        "\"dynamicExperimental56\":%s,\"generatedOnlyDebug\":%s,"
        "\"forcedMaximumMultiplier\":%u,"
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
        "\"dlssgStatus\":%u,\"dynamicMfgSupported\":%s,"
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
        "\"streamlineLifecyclePolicy\":\"clean-enable\","
        "\"streamlineRebuildRequired\":%s,"
        "\"deferredLifecycleRevision\":%llu,"
        "\"cleanEnableApplyCount\":%llu,"
        "\"hostLifecycleResetCount\":%llu,"
        "\"cleanEnableBoundaryAvailable\":%s,"
        "\"cleanEnableRetryPending\":%s,"
        "\"missedCleanEnableCount\":%llu,"
        "\"featureRecycleActive\":%s,\"featureRecycleStage\":%u,"
        "\"featureRecycleRevision\":%llu,\"featureRecycleCount\":%llu,"
        "\"featureRecycleFreeCalls\":%llu,\"featureRecycleLastFreeResult\":%d,"
        "\"featureRecycleOffStateObserved\":%s,"
        "\"featureRecycleStatePolls\":%llu,"
        "\"featureRecycleFenceValue\":%llu,"
        "\"featureRecycleFenceCompletedValue\":%llu,"
        "\"featureRecycleOutstandingOutputs\":%u,"
        "\"featureRecycleExplicitFreeSkipped\":%s,"
        "\"ngxEvaluateCalls\":%llu,"
        "\"ngxTemporalValidCount\":%llu,\"ngxTemporalInvalidCount\":%llu,"
        "\"ngxSeenCountMask\":%u,\"ngxSeenIndexMask\":%u,"
        "\"ngxCountGetResult\":%d,\"ngxIndexGetResult\":%d,"
        "\"ngxRawCount\":%d,\"ngxRawIndex\":%d,"
        "\"ngxFrameIdGetResult\":%d,\"ngxOutputGetResult\":%d,"
        "\"ngxOutputCompleteBatches\":%llu,"
        "\"ngxOutputAliasedBatches\":%llu,"
        "\"ngxFullStateRepairActive\":%s,"
        "\"ngxFullStateForcedCount\":%llu,"
        "\"ngxDisableInterpolationGetResult\":%d,"
        "\"ngxDisableInterpolationResource\":\"0x%llX\","
        "\"ngxDisableInterpolationResourcePreservedCount\":%llu,"
        "\"ngxResetGetResult\":%d,\"ngxRawReset\":%d,"
        "\"ngxResetPreservedCount\":%llu,"
        "\"ngxLastOutputUniqueCount\":%u,\"ngxLastFrameId\":%llu,"
        "\"ngxLastOutputInterpolated\":\"0x%llX\"}\n",
        static_cast<unsigned long>(pid),
        static_cast<unsigned long long>(UnixTimeSeconds()), route,
        bridgeReady ? "true" : "false",
        gLiveHookInstalled.load(std::memory_order_relaxed) ? "true" : "false",
        gD3DDeviceHookInstalled.load(std::memory_order_relaxed) ? "true" : "false",
        gD3D12CreateDeviceHookInstalled.load(std::memory_order_relaxed)
            ? "true" : "false",
        gSlUpgradeInterfaceHookInstalled.load(std::memory_order_relaxed)
            ? "true" : "false",
        gEarlyD3D12DeviceCaptured.load(std::memory_order_relaxed)
            ? "true" : "false",
        gUiTagHookInstalled.load(std::memory_order_relaxed) ? "true" : "false",
        gNgxEvaluateLookupHookInstalled.load(std::memory_order_relaxed)
            ? "true" : "false",
        gNgxEvaluateHookExposed.load(std::memory_order_relaxed) ? "true" : "false",
        gNgxEvaluateSeen.load(std::memory_order_relaxed) ? "true" : "false",
        presentProbe.factoryImportHookInstalled ? "true" : "false",
        presentProbe.nativeFactoryHookInstalled ? "true" : "false",
        presentProbe.nativeSwapchainHookInstalled ? "true" : "false",
        presentProbe.enabled ? "true" : "false",
        static_cast<unsigned long long>(presentProbe.nativePresentCalls),
        static_cast<unsigned long long>(presentProbe.scheduledFrames),
        static_cast<unsigned long long>(presentProbe.capturedFrames),
        static_cast<unsigned long long>(presentProbe.droppedFrames),
        ngxOutputProbe.enabled ? "true" : "false",
        ngxOutputProbe.queueHookInstalled ? "true" : "false",
        static_cast<unsigned long long>(ngxOutputProbe.scheduled),
        static_cast<unsigned long long>(ngxOutputProbe.submitted),
        static_cast<unsigned long long>(ngxOutputProbe.captured),
        static_cast<unsigned long long>(ngxOutputProbe.dropped),
        static_cast<unsigned long long>(ngxOutputProbe.completeBatches),
        static_cast<unsigned long long>(ngxOutputProbe.duplicateBatches),
        ngxOutputProbe.immutableEnabled ? "true" : "false",
        static_cast<unsigned long long>(ngxOutputProbe.immutablePrepared),
        static_cast<unsigned long long>(ngxOutputProbe.immutableSubmitted),
        static_cast<unsigned long long>(ngxOutputProbe.immutableRetired),
        static_cast<unsigned long long>(ngxOutputProbe.immutableDropped),
        static_cast<unsigned long long>(
            ngxOutputProbe.immutableReservationReclaims),
        ngxOutputProbe.immutableAllocated,
        gActiveWrapperObserved.load(std::memory_order_relaxed) ? "true" : "false",
        gActiveWrapperPatched.load(std::memory_order_relaxed) ? "true" : "false",
        gLoadedWrapperCandidates.load(std::memory_order_relaxed),
        gPatchedWrapperCandidates.load(std::memory_order_relaxed),
        gLoadedNgxCandidates.load(std::memory_order_relaxed),
        gPatchedNgxCandidates.load(std::memory_order_relaxed),
        gLoadedNgxSynthesisCandidates.load(std::memory_order_relaxed),
        gPatchedNgxSynthesisCandidates.load(std::memory_order_relaxed),
        perSampleSynthesisReady ? "true" : "false",
        synthesisFallbackActive ? "true" : "false",
        control.dynamic ? "dynamic" : "fixed", control.multiplier,
        control.dynamicTargetFrameRate,
        control.dynamicExperimental56 ? "true" : "false",
        control.generatedOnlyDebug ? "true" : "false",
        effectiveMaximumMultiplier,
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
        gStreamlineRebuildRequired.load(std::memory_order_relaxed)
            ? "true" : "false",
        static_cast<unsigned long long>(
            gDeferredLifecycleRevision.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            gCleanEnableApplyCount.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            gHostLifecycleResetCount.load(std::memory_order_relaxed)),
        gCleanEnableBoundaryAvailable.load(std::memory_order_relaxed)
            ? "true" : "false",
        gCleanEnableRetryPending.load(std::memory_order_relaxed)
            ? "true" : "false",
        static_cast<unsigned long long>(
            gMissedCleanEnableCount.load(std::memory_order_relaxed)),
        featureRecycleActive ? "true" : "false", featureRecycleStage,
        static_cast<unsigned long long>(
            gFeatureRecycleRevision.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            gFeatureRecycleCount.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            gFeatureRecycleFreeCalls.load(std::memory_order_relaxed)),
        gFeatureRecycleLastFreeResult.load(std::memory_order_relaxed),
        gFeatureRecycleOffStateObserved.load(std::memory_order_relaxed)
            ? "true" : "false",
        static_cast<unsigned long long>(
            gFeatureRecycleStatePolls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            gFeatureRecycleFenceValue.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            gFeatureRecycleFenceCompletedValue.load(std::memory_order_relaxed)),
        gFeatureRecycleOutstandingOutputs.load(std::memory_order_relaxed),
        gFeatureRecycleExplicitFreeSkipped.load(std::memory_order_relaxed)
            ? "true" : "false",
        static_cast<unsigned long long>(gNgxEvaluateCalls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            gNgxTemporalValidCount.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            gNgxTemporalInvalidCount.load(std::memory_order_relaxed)),
        gNgxSeenCountMask.load(std::memory_order_relaxed),
        gNgxSeenIndexMask.load(std::memory_order_relaxed),
        gNgxLastCountGetResult.load(std::memory_order_relaxed),
        gNgxLastIndexGetResult.load(std::memory_order_relaxed),
        gNgxLastRawCount.load(std::memory_order_relaxed),
        gNgxLastRawIndex.load(std::memory_order_relaxed),
        gNgxLastFrameIdGetResult.load(std::memory_order_relaxed),
        gNgxLastOutputGetResult.load(std::memory_order_relaxed),
        static_cast<unsigned long long>(
            gNgxOutputCompleteBatches.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            gNgxOutputAliasedBatches.load(std::memory_order_relaxed)),
        gNgxFullStateRepairActive.load(std::memory_order_relaxed)
            ? "true" : "false",
        static_cast<unsigned long long>(
            gNgxFullStateForcedCount.load(std::memory_order_relaxed)),
        gNgxLastDisableInterpolationGetResult.load(std::memory_order_relaxed),
        static_cast<unsigned long long>(
            gNgxLastDisableInterpolationResource.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            gNgxDisableInterpolationResourcePreservedCount.load(
                std::memory_order_relaxed)),
        gNgxLastResetGetResult.load(std::memory_order_relaxed),
        gNgxLastRawReset.load(std::memory_order_relaxed),
        static_cast<unsigned long long>(
            gNgxResetPreservedCount.load(std::memory_order_relaxed)),
        gNgxLastOutputUniqueCount.load(std::memory_order_relaxed),
        static_cast<unsigned long long>(
            gNgxLastFrameId.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            gNgxLastOutputInterpolated.load(std::memory_order_relaxed)));
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

sl::DLSSGOptions CopyKnownOptions(const sl::DLSSGOptions& source, bool preserveNext)
{
    sl::DLSSGOptions copy{};
    copy.next = preserveNext ? source.next : nullptr;
    copy.structType = source.structType;
    copy.structVersion = std::clamp<size_t>(
        source.structVersion, sl::kStructVersion1, sl::kStructVersion5);
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
    if (snapshot.control.dynamic)
    {
        // The injected object is a complete v5 structure even when Cyberpunk supplied
        // an older prefix, so the active wrapper can consume the dynamic target
        // without reading beyond the game's allocation.
        adjusted.structVersion = sl::kStructVersion5;
        adjusted.mode = sl::DLSSGMode::eDynamic;
        adjusted.dynamicTargetFrameRate =
            static_cast<float>(snapshot.control.dynamicTargetFrameRate);
    }
    else
    {
        adjusted.mode = sl::DLSSGMode::eOn;
        adjusted.numFramesToGenerate =
            std::clamp(snapshot.control.multiplier,
            kMinimumMultiplier, kMaximumMultiplier) - 1;
    }
    if (snapshot.control.generatedOnlyDebug)
    {
        adjusted.flags = static_cast<sl::DLSSGFlags>(
            static_cast<uint32_t>(adjusted.flags)
            | static_cast<uint32_t>(sl::DLSSGFlags::eShowOnlyInterpolatedFrame));
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

bool AcceptedSetOptionsResult(sl::Result result)
{
    return result == sl::Result::eOk || result == sl::Result::eWarnOutOfVRAM;
}

sl::Result HostSetOptionsResult(sl::Result result)
{
    // Cyberpunk treats every non-zero Result as a hard failure. Result 39 is a
    // warning rather than a rejected options update, so retain it in telemetry
    // while reporting success to the host.
    return result == sl::Result::eWarnOutOfVRAM ? sl::Result::eOk : result;
}

PFun_slFreeResources* ResolveFreeResources()
{
    auto* cached = gOriginalFreeResources.load(std::memory_order_acquire);
    if (cached)
        return cached;

    constexpr std::array<const wchar_t*, 3> moduleNames{
        L"sl.interposer.dll", L"sl.common.dll", L"sl.api.dll"};
    for (const wchar_t* moduleName : moduleNames)
    {
        HMODULE module = GetModuleHandleW(moduleName);
        if (!module)
            continue;
        auto* resolved = reinterpret_cast<PFun_slFreeResources*>(
            GetProcAddress(module, "slFreeResources"));
        if (!resolved)
            continue;
        gOriginalFreeResources.store(resolved, std::memory_order_release);
        return resolved;
    }
    return nullptr;
}

bool ControlNeedsFeatureRecycle(const ControlSnapshot& target)
{
    if (gAppliedRevision.load(std::memory_order_acquire) == 0)
        return false;
    ControlConfig applied{};
    applied.dynamic = gAppliedDynamicMode.load(std::memory_order_relaxed);
    applied.multiplier = gAppliedMultiplier.load(std::memory_order_relaxed);
    applied.dynamicTargetFrameRate =
        gAppliedDynamicTargetFrameRate.load(std::memory_order_relaxed);
    applied.dynamicExperimental56 =
        gAppliedDynamicExperimental56.load(std::memory_order_relaxed);
    applied.generatedOnlyDebug =
        gAppliedGeneratedOnlyDebug.load(std::memory_order_relaxed);
    return applied.dynamic != target.control.dynamic
        || RequestedActiveGeneratedFrames(applied)
            != RequestedActiveGeneratedFrames(target.control);
}

void DeferLifecycleControl(const ControlSnapshot& target)
{
    gStreamlineRebuildRequired.store(true, std::memory_order_release);
    gDeferredLifecycleRevision.store(target.revision, std::memory_order_relaxed);
    if (gLifecycleDeferralLoggedRevision.exchange(
            target.revision, std::memory_order_acq_rel) == target.revision)
        return;

    Log(L"Deferred multiplier-shape revision %llu until a clean DLSS-G enable; "
        L"the active Streamline presentation swapchain remains unchanged",
        static_cast<unsigned long long>(target.revision));
}

void ResetNgxFeatureEpoch()
{
    std::vector<uint64_t> abandonedBatches;
    {
        std::lock_guard lock(gNgxOutputBatchMutex);
        abandonedBatches.reserve(gNgxOutputBatches.size());
        for (const auto& batch : gNgxOutputBatches)
            abandonedBatches.push_back(batch.sequence);
        gNgxOutputBatches.clear();
    }
    for (const uint64_t batch : abandonedBatches)
        ngx_output_probe::AbandonImmutableBatch(batch);
    {
        std::lock_guard lock(gNgxForceStateMutex);
        gNgxForceEpochs.clear();
    }
    Log(L"Reset NGX temporal epoch for recreated DLSS-G feature (%zu partial batches)",
        abandonedBatches.size());
}

sl::DLSSGOptions BuildRecycleOffOptions(const sl::DLSSGOptions& source)
{
    sl::DLSSGOptions off = CopyKnownOptions(source, false);
    off.mode = sl::DLSSGMode::eOff;
    off.flags = static_cast<sl::DLSSGFlags>(
        static_cast<uint32_t>(off.flags)
        & ~static_cast<uint32_t>(sl::DLSSGFlags::eRetainResourcesWhenOff));
    return off;
}

sl::Result BeginFeatureRecycle(PFun_slDLSSGSetOptions* original,
    const sl::ViewportHandle& viewport, const sl::DLSSGOptions& source,
    const ControlSnapshot& requested)
{
    const ControlSnapshot target = EffectiveControlSnapshot(requested);
    const sl::DLSSGOptions off = BuildRecycleOffOptions(source);
    gSetOptionsCalls.fetch_add(1, std::memory_order_relaxed);
    const sl::Result result = original(viewport, off);
    gSetOptionsSeen.store(true, std::memory_order_release);
    gLastSetOptionsResult.store(static_cast<int32_t>(result), std::memory_order_relaxed);
    gLastAttemptTick.store(GetTickCount64(), std::memory_order_relaxed);
    if (!AcceptedSetOptionsResult(result))
    {
        gAttemptedRevision.store(target.revision, std::memory_order_release);
        Log(L"DLSS-G feature recycle could not submit Off for revision %llu: result=%d",
            static_cast<unsigned long long>(target.revision),
            static_cast<int>(result));
        return HostSetOptionsResult(result);
    }

    gFeatureRecycle.stage = FeatureRecycleStage::eWaitingForOffState;
    gFeatureRecycle.snapshot = target;
    gFeatureRecycle.viewport = viewport;
    gFeatureRecycle.source = CopyKnownOptions(source, false);
    gFeatureRecycle.offSubmittedTick = GetTickCount64();
    gFeatureRecycleStage.store(
        static_cast<uint32_t>(FeatureRecycleStage::eWaitingForOffState),
        std::memory_order_release);
    gFeatureRecycleRevision.store(target.revision, std::memory_order_relaxed);
    gFeatureRecycleOffStateObserved.store(false, std::memory_order_release);
    gFeatureRecycleFenceValue.store(0, std::memory_order_relaxed);
    gFeatureRecycleFenceCompletedValue.store(0, std::memory_order_relaxed);
    gFeatureRecycleOutstandingOutputs.store(0, std::memory_order_relaxed);
    gFeatureRecycleExplicitFreeSkipped.store(false, std::memory_order_relaxed);
    gFeatureRecycleLastFreeResult.store(
        static_cast<int32_t>(sl::Result::eErrorNotInitialized),
        std::memory_order_relaxed);
    ngx_output_probe::BeginFeatureRecycle();
    Log(L"DLSS-G feature recycle started for revision %llu: Off accepted, "
        L"retain-resources flag cleared",
        static_cast<unsigned long long>(target.revision));
    return HostSetOptionsResult(result);
}

void RecordAppliedControl(const ControlSnapshot& snapshot, sl::Result result,
    bool liveReapply, bool uiRecompositionEnabled, bool uiRecompositionForced)
{
    gSetOptionsSeen.store(true, std::memory_order_release);
    gLastSetOptionsResult.store(static_cast<int32_t>(result), std::memory_order_relaxed);
    gLastAttemptTick.store(GetTickCount64(), std::memory_order_relaxed);
    gAttemptedRevision.store(snapshot.revision, std::memory_order_release);
    // eWarnOutOfVRAM is emitted after Streamline accepts work when DXGI reports
    // no remaining budget. Keep the raw warning for telemetry, but do not leave
    // a successfully submitted multiplier permanently marked as pending.
    if (result != sl::Result::eOk && result != sl::Result::eWarnOutOfVRAM)
    {
        if (result == sl::Result::eErrorNotInitialized
            && gCleanEnableBoundaryAvailable.load(std::memory_order_acquire))
            gCleanEnableRetryPending.store(true, std::memory_order_release);
        return;
    }

    const uint64_t previous = gAppliedRevision.load(std::memory_order_acquire);
    const bool cleanEnable =
        gCleanEnableBoundaryAvailable.exchange(false, std::memory_order_acq_rel);
    gCleanEnableRetryPending.store(false, std::memory_order_release);
    gGameFrameGenerationOn.store(true, std::memory_order_release);
    gAppliedDynamicMode.store(snapshot.control.dynamic, std::memory_order_relaxed);
    gAppliedMultiplier.store(snapshot.control.multiplier, std::memory_order_relaxed);
    gAppliedDynamicTargetFrameRate.store(
        snapshot.control.dynamicTargetFrameRate, std::memory_order_relaxed);
    gAppliedDynamicExperimental56.store(
        snapshot.control.dynamicExperimental56, std::memory_order_relaxed);
    gAppliedGeneratedOnlyDebug.store(
        snapshot.control.generatedOnlyDebug, std::memory_order_relaxed);
    present_probe::SetEnabled(snapshot.control.generatedOnlyDebug
        || gPresentProbeEnvironmentEnabled.load(std::memory_order_acquire));
    // Streamline owns the generated-frame surfaces and their presentation
    // fences.  The immutable replacement ring is now diagnostic-only: making
    // it part of normal >2x operation creates a second lifetime system which
    // cannot safely outlive Streamline's swapchain queue in every integration.
    ngx_output_probe::SetImmutableEnabled(
        gImmutableOutputEnvironmentEnabled.load(std::memory_order_acquire));
    gAppliedUiRecompositionEnabled.store(
        uiRecompositionEnabled, std::memory_order_relaxed);
    gAppliedUiRecompositionForced.store(
        uiRecompositionForced, std::memory_order_relaxed);
    gAppliedRevision.store(snapshot.revision, std::memory_order_release);
    if (previous == 0 && cleanEnable)
        gCleanEnableApplyCount.fetch_add(1, std::memory_order_relaxed);
    if (snapshot.revision
        == gDesiredRevision.load(std::memory_order_acquire))
    {
        gStreamlineRebuildRequired.store(false, std::memory_order_release);
        gDeferredLifecycleRevision.store(0, std::memory_order_relaxed);
    }
    if (liveReapply)
        gLiveReapplyCount.fetch_add(1, std::memory_order_relaxed);

    if (previous == snapshot.revision)
        return;
    if (snapshot.control.dynamic)
        Log(L"%s dynamic MFG: target=%u FPS experimental56=%d max=%ux result=%d",
            liveReapply ? L"Live-reapplied" : L"Applied",
            snapshot.control.dynamicTargetFrameRate,
            snapshot.control.dynamicExperimental56,
            static_cast<uint32_t>(RequestedMaximumGeneratedFrames(snapshot.control)) + 1,
            static_cast<int>(result));
    else
        Log(L"%s fixed multiplier: %ux, result=%d",
            liveReapply ? L"Live-reapplied" : L"Applied",
            snapshot.control.multiplier, static_cast<int>(result));
    Log(L"UI recomposition: enabled=%d forced=%d inputsReady=%d "
        L"gameEnabled=%d optionsVersion=%u hudlessFormat=%u uiFormat=%u",
        uiRecompositionEnabled, uiRecompositionForced,
        gUiInputsReady.load(std::memory_order_relaxed),
        gGameUiRecompositionEnabled.load(std::memory_order_relaxed),
        gGameOptionsStructVersion.load(std::memory_order_relaxed),
        gGameHudlessBufferFormat.load(std::memory_order_relaxed),
        gGameUiBufferFormat.load(std::memory_order_relaxed));
    Log(L"Generated-only diagnostic: enabled=%d",
        snapshot.control.generatedOnlyDebug);
}

void RecordUnadjustedEnableResult(sl::Result result)
{
    gSetOptionsSeen.store(true, std::memory_order_release);
    gLastSetOptionsResult.store(static_cast<int32_t>(result), std::memory_order_relaxed);
    gLastAttemptTick.store(GetTickCount64(), std::memory_order_relaxed);
    if (AcceptedSetOptionsResult(result))
    {
        const bool consumedCleanEnable =
            gCleanEnableBoundaryAvailable.exchange(false, std::memory_order_acq_rel);
        gCleanEnableRetryPending.store(false, std::memory_order_release);
        gGameFrameGenerationOn.store(true, std::memory_order_release);
        if (consumedCleanEnable)
            gMissedCleanEnableCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (result == sl::Result::eErrorNotInitialized
        && gCleanEnableBoundaryAvailable.load(std::memory_order_acquire))
    {
        if (gControlReady.load(std::memory_order_acquire))
            gAttemptedRevision.store(
                gDesiredRevision.load(std::memory_order_acquire),
                std::memory_order_release);
        gCleanEnableRetryPending.store(true, std::memory_order_release);
    }
}

sl::Result SubmitAdjustedOptions(
    PFun_slDLSSGSetOptions* original, const sl::ViewportHandle& viewport,
    const sl::DLSSGOptions& source, const ControlSnapshot& snapshot, bool liveReapply)
{
    const ControlSnapshot effectiveSnapshot = EffectiveControlSnapshot(snapshot);
    const bool synthesisFallback = effectiveSnapshot.control.dynamic
            != snapshot.control.dynamic
        || effectiveSnapshot.control.multiplier != snapshot.control.multiplier;
    if (synthesisFallback
        && gSynthesisFallbackLoggedRevision.exchange(
            snapshot.revision, std::memory_order_acq_rel) != snapshot.revision)
    {
        Log(L"Requested MFG mode requires the per-sample synthesis patch; "
            L"falling back to fixed 2x for this NVIDIA DLL revision");
    }

    ApplyWrapperMaximum(effectiveSnapshot.control);
    const UiInputSnapshot uiInputs = ReadUiInputSnapshot(
        static_cast<uint32_t>(viewport));
    const bool gameUiRecomposition = source.structVersion >= sl::kStructVersion4
        && source.enableUserInterfaceRecomposition == sl::Boolean::eTrue;
    const bool forceUiRecomposition = uiInputs.ready && !gameUiRecomposition;
    const sl::DLSSGOptions adjusted = BuildAdjustedOptions(
        source, effectiveSnapshot, !liveReapply, uiInputs.ready);
    const bool uiRecompositionEnabled = adjusted.structVersion >= sl::kStructVersion4
        && adjusted.enableUserInterfaceRecomposition == sl::Boolean::eTrue;
    gSetOptionsCalls.fetch_add(1, std::memory_order_relaxed);
    const sl::Result result = original(viewport, adjusted);
    RecordAppliedControl(effectiveSnapshot, result, liveReapply,
        uiRecompositionEnabled, forceUiRecomposition);
    if (!liveReapply
        && (result == sl::Result::eOk || result == sl::Result::eWarnOutOfVRAM))
    {
        auto* getState = gOriginalGetState.load(std::memory_order_acquire);
        if (getState)
        {
            sl::DLSSGState state{};
            const sl::Result stateResult = getState(viewport, state, &adjusted);
            RecordDlssgStateResult(stateResult, state, true);
        }
        else
        {
            UpdateFpsTelemetry(0);
        }
    }
    return HostSetOptionsResult(result);
}

bool ProgressFeatureRecycle(const sl::ViewportHandle& viewport)
{
    if (gFeatureRecycle.stage == FeatureRecycleStage::eIdle)
        return false;
    if (static_cast<uint32_t>(gFeatureRecycle.viewport)
        != static_cast<uint32_t>(viewport))
        return true;

    const ControlSnapshot latest = EffectiveControlSnapshot(ReadControlSnapshot());
    if (latest.revision != 0
        && latest.revision != gFeatureRecycle.snapshot.revision)
    {
        Log(L"DLSS-G feature recycle retargeted from revision %llu to %llu",
            static_cast<unsigned long long>(gFeatureRecycle.snapshot.revision),
            static_cast<unsigned long long>(latest.revision));
        gFeatureRecycle.snapshot = latest;
        gFeatureRecycleRevision.store(latest.revision, std::memory_order_relaxed);
    }
    sl::DLSSGOptions newestSource{};
    if (ReadLastGameOptions(viewport, newestSource))
        gFeatureRecycle.source = newestSource;

    auto* original = gOriginalSetOptions.load(std::memory_order_acquire);
    if (!original)
        return true;

    const auto tryReenable = [&]() {
        if (gFeatureRecycle.stage == FeatureRecycleStage::eReenabling
            && gLastSetOptionsResult.load(std::memory_order_relaxed)
                == static_cast<int32_t>(sl::Result::eErrorNotInitialized))
        {
            const uint64_t now = GetTickCount64();
            const uint64_t previous =
                gLastAttemptTick.load(std::memory_order_relaxed);
            if (now >= previous && now - previous < kNotInitializedRetryDelayMs)
                return;
            gNotInitializedRetryCount.fetch_add(1, std::memory_order_relaxed);
        }

        const uint64_t revision = gFeatureRecycle.snapshot.revision;
        const sl::Result enableResult = SubmitAdjustedOptions(
            original, viewport, gFeatureRecycle.source,
            gFeatureRecycle.snapshot, true);
        if (enableResult == sl::Result::eOk)
        {
            gFeatureRecycleCount.fetch_add(1, std::memory_order_relaxed);
            gFeatureRecycle.stage = FeatureRecycleStage::eIdle;
            gFeatureRecycleStage.store(
                static_cast<uint32_t>(FeatureRecycleStage::eIdle),
                std::memory_order_release);
            Log(L"DLSS-G feature recycle completed for revision %llu",
                static_cast<unsigned long long>(revision));
            return;
        }
        if (enableResult == sl::Result::eErrorNotInitialized)
        {
            Log(L"DLSS-G feature recycle re-enable will retry for revision %llu: result=21",
                static_cast<unsigned long long>(revision));
            return;
        }

        Log(L"DLSS-G feature recycle re-enable failed for revision %llu: result=%d",
            static_cast<unsigned long long>(revision),
            static_cast<int>(enableResult));
        gFeatureRecycle.stage = FeatureRecycleStage::eIdle;
        gFeatureRecycleStage.store(
            static_cast<uint32_t>(FeatureRecycleStage::eIdle),
            std::memory_order_release);
    };

    if (gFeatureRecycle.stage == FeatureRecycleStage::eReenabling)
    {
        tryReenable();
        return true;
    }

    if (gFeatureRecycle.stage == FeatureRecycleStage::eWaitingForOffState)
    {
        // Some integrations (Cyberpunk included) stop calling GetState after
        // initialization.  Poll the official state entry point ourselves while
        // host On calls are suppressed, using the same Off options that began
        // the recycle.  Otherwise an accepted Off request can wait forever for
        // a callback that the host never makes.
        if (!gFeatureRecycleOffStateObserved.load(std::memory_order_acquire))
        {
            auto* getState = gOriginalGetState.load(std::memory_order_acquire);
            if (getState)
            {
                sl::DLSSGState state{};
                const sl::DLSSGOptions off =
                    BuildRecycleOffOptions(gFeatureRecycle.source);
                gFeatureRecycleStatePolls.fetch_add(1, std::memory_order_relaxed);
                const sl::Result stateResult = getState(viewport, state, &off);
                RecordDlssgStateResult(stateResult, state, false);
            }
        }
        if (!gFeatureRecycleOffStateObserved.load(std::memory_order_acquire))
            return true;
        gFeatureRecycle.stage = FeatureRecycleStage::eWaitingForDrain;
        gFeatureRecycleStage.store(
            static_cast<uint32_t>(FeatureRecycleStage::eWaitingForDrain),
            std::memory_order_release);
        Log(L"DLSS-G feature recycle observed the Off state; waiting for GPU drain");
    }

    const ngx_output_probe::FeatureDrainSnapshot drain =
        ngx_output_probe::ReadFeatureDrainSnapshot();
    gFeatureRecycleFenceValue.store(
        drain.streamlineFenceValue, std::memory_order_relaxed);
    gFeatureRecycleFenceCompletedValue.store(
        drain.streamlineFenceCompletedValue, std::memory_order_relaxed);
    gFeatureRecycleOutstandingOutputs.store(
        drain.outstandingImmutableOutputs, std::memory_order_relaxed);
    if (drain.outstandingImmutableOutputs != 0
        || (drain.hasStreamlineFence && !drain.streamlineFenceComplete))
        return true;
    if (!ngx_output_probe::FinishFeatureRecycle())
        return true;

    gFeatureRecycle.stage = FeatureRecycleStage::eReenabling;
    gFeatureRecycleStage.store(
        static_cast<uint32_t>(FeatureRecycleStage::eReenabling),
        std::memory_order_release);

    if (drain.hasStreamlineFence && drain.streamlineFenceComplete)
    {
        auto* freeResources = ResolveFreeResources();
        if (freeResources)
        {
            gFeatureRecycleFreeCalls.fetch_add(1, std::memory_order_relaxed);
            const sl::Result freeResult = freeResources(
                sl::kFeatureDLSS_G, viewport);
            gFeatureRecycleLastFreeResult.store(
                static_cast<int32_t>(freeResult), std::memory_order_relaxed);
            Log(L"Explicitly freed drained DLSS-G resources for revision %llu: result=%d",
                static_cast<unsigned long long>(gFeatureRecycle.snapshot.revision),
                static_cast<int>(freeResult));
        }
        else
        {
            gFeatureRecycleExplicitFreeSkipped.store(true, std::memory_order_relaxed);
            Log(L"slFreeResources was unavailable after the DLSS-G drain; "
                L"the Off call already released resources because retention was cleared");
        }
    }
    else
    {
        // The default Off behavior still frees DLSS-G resources. Do not invoke
        // slFreeResources without a completion fence: NVIDIA documents that as
        // unsafe while an evaluation command list may still be pending.
        gFeatureRecycleExplicitFreeSkipped.store(true, std::memory_order_relaxed);
        Log(L"No Streamline completion fence was exposed; relying on drained Off "
            L"resource release and skipping unsafe explicit slFreeResources");
    }

    ResetNgxFeatureEpoch();
    tryReenable();
    return true;
}

void ReapplyPendingControl(const sl::ViewportHandle& viewport)
{
    const bool gameEnabled =
        gGameFrameGenerationOn.load(std::memory_order_acquire);
    const bool cleanEnableRetry =
        gCleanEnableRetryPending.load(std::memory_order_acquire);
    if (!gControlReady.load(std::memory_order_acquire)
        || (!gameEnabled && !cleanEnableRetry) || !BridgeReady())
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

    auto* original = gOriginalSetOptions.load(std::memory_order_acquire);
    sl::DLSSGOptions source{};
    if (!original || !ReadLastGameOptions(viewport, source))
        return;
    if (retryNotInitialized)
    {
        const uint64_t retry =
            gNotInitializedRetryCount.fetch_add(1, std::memory_order_relaxed) + 1;
        Log(L"Retrying request revision %llu after Streamline result 21 (retry %llu)",
            static_cast<unsigned long long>(snapshot.revision),
            static_cast<unsigned long long>(retry));
    }

    const ControlSnapshot effective = EffectiveControlSnapshot(snapshot);
    if (!gCleanEnableBoundaryAvailable.load(std::memory_order_acquire)
        && gAppliedRevision.load(std::memory_order_acquire) == 0)
    {
        // The host's first On call was accepted before the bridge could adjust
        // it.  There is an active Streamline presentation swapchain, but no
        // trustworthy applied shape to maintain.  Never reinterpret this as a
        // fresh feature simply because our applied revision is still zero.
        DeferLifecycleControl(snapshot);
        return;
    }
    if (ControlNeedsFeatureRecycle(effective))
    {
        // A live feature-only recycle leaves Streamline's presentation queue,
        // off-screen buffers, and swapchain lifetime configured for the old
        // multiplier.  Defer shape changes until the host performs a genuine
        // Off -> On lifecycle boundary (or the process starts fresh).
        DeferLifecycleControl(snapshot);
        return;
    }
    const sl::Result result =
        SubmitAdjustedOptions(original, viewport, source, snapshot, true);
    if (result != sl::Result::eOk)
        Log(L"Live reapply failed for request revision %llu: result=%d",
            static_cast<unsigned long long>(snapshot.revision), static_cast<int>(result));
}

sl::Result HookSlDLSSGSetOptions(
    const sl::ViewportHandle& viewport, const sl::DLSSGOptions& options)
{
    auto* original = gOriginalSetOptions.load(std::memory_order_acquire);
    if (!original)
        return sl::Result::eErrorNotInitialized;

    std::lock_guard callLock(gStreamlineCallMutex);

    const bool enabled = options.mode == sl::DLSSGMode::eOn
        || options.mode == sl::DLSSGMode::eAuto
        || options.mode == sl::DLSSGMode::eDynamic;
    const bool previouslyEnabled =
        gGameFrameGenerationOn.load(std::memory_order_acquire);
    if (!enabled)
    {
        if (previouslyEnabled
            || gFeatureRecycle.stage != FeatureRecycleStage::eIdle)
        {
            gFeatureRecycle.stage = FeatureRecycleStage::eIdle;
            gFeatureRecycleStage.store(
                static_cast<uint32_t>(FeatureRecycleStage::eIdle),
                std::memory_order_release);
            ngx_output_probe::BeginFeatureRecycle();
            ResetNgxFeatureEpoch();
        }
        gSetOptionsSeen.store(true, std::memory_order_release);
        gSetOptionsCalls.fetch_add(1, std::memory_order_relaxed);
        const sl::DLSSGOptions off = BuildRecycleOffOptions(options);
        const sl::Result result = original(viewport, off);
        gLastSetOptionsResult.store(static_cast<int32_t>(result), std::memory_order_relaxed);
        if (AcceptedSetOptionsResult(result))
        {
            // The next host On call is a clean creation boundary. Forget the
            // previous applied shape so the saved target is submitted directly
            // instead of entering the retired feature-only recycle path.
            gAppliedRevision.store(0, std::memory_order_release);
            gAttemptedRevision.store(0, std::memory_order_release);
            gAppliedMultiplier.store(0, std::memory_order_relaxed);
            gHostLifecycleResetCount.fetch_add(1, std::memory_order_relaxed);
            gGameFrameGenerationOn.store(false, std::memory_order_release);
            gCleanEnableBoundaryAvailable.store(true, std::memory_order_release);
            gCleanEnableRetryPending.store(false, std::memory_order_release);
            gStreamlineRebuildRequired.store(false, std::memory_order_release);
            gDeferredLifecycleRevision.store(0, std::memory_order_relaxed);
        }
        return result;
    }

    CaptureGameOptions(viewport, options);
    if (!gControlReady.load(std::memory_order_acquire))
    {
        gSetOptionsCalls.fetch_add(1, std::memory_order_relaxed);
        const sl::Result result = original(viewport, options);
        RecordUnadjustedEnableResult(result);
        return result;
    }

    if (!BridgeReady())
    {
        gSetOptionsCalls.fetch_add(1, std::memory_order_relaxed);
        const sl::Result result = original(viewport, options);
        RecordUnadjustedEnableResult(result);
        // If this call loaded NGX and completed bridge discovery, the native
        // feature and presentation swapchain were still created from the
        // unadjusted options.  Applying a second shape here would already be a
        // mid-feature mutation, so wait for a genuine host Off -> On instead.
        return result;
    }

    const ControlSnapshot snapshot = ReadControlSnapshot();
    const ControlSnapshot effective = EffectiveControlSnapshot(snapshot);
    if (!gCleanEnableBoundaryAvailable.load(std::memory_order_acquire)
        && gAppliedRevision.load(std::memory_order_acquire) == 0)
    {
        DeferLifecycleControl(snapshot);
        gSetOptionsCalls.fetch_add(1, std::memory_order_relaxed);
        const sl::Result result = original(viewport, options);
        RecordUnadjustedEnableResult(result);
        return result;
    }
    if (snapshot.revision
            != gAppliedRevision.load(std::memory_order_acquire)
        && ControlNeedsFeatureRecycle(effective))
    {
        DeferLifecycleControl(snapshot);
        const ControlSnapshot applied = ReadAppliedControlSnapshot();
        if (applied.revision != 0)
            return SubmitAdjustedOptions(original, viewport, options, applied, false);
    }
    return SubmitAdjustedOptions(original, viewport, options, snapshot, false);
}

sl::Result HookSlDLSSGGetState(
    const sl::ViewportHandle& viewport, sl::DLSSGState& state,
    const sl::DLSSGOptions* options)
{
    auto* original = gOriginalGetState.load(std::memory_order_acquire);
    if (!original)
        return sl::Result::eErrorNotInitialized;

    std::lock_guard callLock(gStreamlineCallMutex);
    ReapplyPendingControl(viewport);
    const sl::Result result = original(viewport, state, options);
    RecordDlssgStateResult(result, state, false);
    return result;
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
        ObserveActiveWrapperProvider(function);
        auto* setOptions = reinterpret_cast<PFun_slDLSSGSetOptions*>(function);
        if (setOptions != &HookSlDLSSGSetOptions)
            gOriginalSetOptions.store(setOptions, std::memory_order_release);
        function = reinterpret_cast<void*>(&HookSlDLSSGSetOptions);
        if (!gSetOptionsHookExposed.exchange(true))
            Log(L"Intercepted slDLSSGSetOptions for live multiplier control");
    }
    else if (function && functionName && strcmp(functionName, "slDLSSGGetState") == 0)
    {
        ObserveActiveWrapperProvider(function);
        auto* getState = reinterpret_cast<PFun_slDLSSGGetState*>(function);
        if (getState != &HookSlDLSSGGetState)
            gOriginalGetState.store(getState, std::memory_order_release);
        function = reinterpret_cast<void*>(&HookSlDLSSGGetState);
        if (!gGetStateHookExposed.exchange(true))
            Log(L"Intercepted slDLSSGGetState for render-thread reapply and actual telemetry");
    }
    return result;
}

bool RegisterObservedD3D12Device(
    void* d3dInterface, const wchar_t* source, bool early)
{
    if (!d3dInterface)
        return false;

    ID3D12Device* device = nullptr;
    if (FAILED(reinterpret_cast<IUnknown*>(d3dInterface)->QueryInterface(
            IID_PPV_ARGS(&device))))
        return false;

    const bool registered = ngx_output_probe::RegisterDevice(device);
    device->Release();
    if (registered && early
        && !gEarlyD3D12DeviceCaptured.exchange(true, std::memory_order_acq_rel))
    {
        Log(L"Captured early D3D12 device through %s before queue creation", source);
    }
    return registered;
}

HRESULT WINAPI HookD3D12CreateDevice(IUnknown* adapter,
    D3D_FEATURE_LEVEL minimumFeatureLevel, REFIID interfaceId, void** device)
{
    const auto original =
        gOriginalD3D12CreateDevice.load(std::memory_order_acquire);
    if (!original)
        return E_FAIL;

    const HRESULT result = original(
        adapter, minimumFeatureLevel, interfaceId, device);
    if (SUCCEEDED(result) && device && *device)
        RegisterObservedD3D12Device(*device, L"D3D12CreateDevice", true);
    return result;
}

sl::Result HookSlUpgradeInterface(void** baseInterface)
{
    // Cyberpunk can create its native graphics queue before slSetD3DDevice.
    // Capture the base device immediately before Streamline replaces it with
    // a proxy, then capture the proxy as well so both submission layers are
    // observable.
    void* const base = baseInterface ? *baseInterface : nullptr;
    RegisterObservedD3D12Device(base, L"slUpgradeInterface(base)", true);

    auto* original = gOriginalUpgradeInterface.load(std::memory_order_acquire);
    const sl::Result result = original
        ? original(baseInterface) : sl::Result::eErrorNotInitialized;
    if (baseInterface && *baseInterface && *baseInterface != base)
    {
        RegisterObservedD3D12Device(
            *baseInterface, L"slUpgradeInterface(proxy)", true);
    }
    return result;
}

sl::Result HookSlSetD3DDevice(void* d3dDevice)
{
    if (RegisterObservedD3D12Device(
            d3dDevice, L"slSetD3DDevice", false))
        Log(L"Captured Streamline D3D12 device for late queue discovery");

    auto* original = gOriginalSetD3DDevice.load(std::memory_order_acquire);
    return original ? original(d3dDevice) : sl::Result::eErrorNotInitialized;
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
            *slot = replacement;
            DWORD ignoredProtection = 0;
            const BOOL restored = VirtualProtect(
                slot, sizeof(*slot), oldProtection, &ignoredProtection);
            FlushInstructionCache(GetCurrentProcess(), slot, sizeof(*slot));
            return restored != FALSE;
        }
    }
    return false;
}

bool HookMainExecutableImport(const char* importedModule, const char* importedFunction,
    void* replacement, void*& original)
{
    return HookModuleImport(GetModuleHandleW(nullptr), importedModule,
        importedFunction, replacement, original);
}

HMODULE ModuleFromAddress(const void* address)
{
    if (!address)
        return nullptr;
    MEMORY_BASIC_INFORMATION memory{};
    return VirtualQuery(address, &memory, sizeof(memory)) == sizeof(memory)
        ? static_cast<HMODULE>(memory.AllocationBase) : nullptr;
}

bool InstallNgxEvaluateLookupHook(ModuleRecord& record)
{
    if (!record.wrapperExport)
        return false;

    void* original = nullptr;
    const bool installed = HookModuleImport(record.module, "KERNEL32.dll",
        "GetProcAddress", reinterpret_cast<void*>(&HookWrapperGetProcAddress), original);
    if (!installed)
        return false;

    std::lock_guard lock(gNgxEvaluateRouteMutex);
    auto route = std::find_if(gNgxEvaluateRoutes.begin(), gNgxEvaluateRoutes.end(),
        [&](const NgxEvaluateRoute& candidate) {
            return candidate.wrapper == record.module;
        });
    if (route == gNgxEvaluateRoutes.end())
    {
        NgxEvaluateRoute added{};
        added.wrapper = record.module;
        added.originalGetProcAddress = reinterpret_cast<GetProcAddressFn>(original);
        gNgxEvaluateRoutes.push_back(added);
    }
    else if (original)
    {
        route->originalGetProcAddress = reinterpret_cast<GetProcAddressFn>(original);
    }
    record.ngxEvaluateLookupHooked = true;
    gNgxEvaluateLookupHookInstalled.store(true, std::memory_order_release);
    return true;
}

FARPROC WINAPI HookWrapperGetProcAddress(HMODULE module, LPCSTR functionName)
{
    const HMODULE caller = ModuleFromAddress(_ReturnAddress());
    GetProcAddressFn original = nullptr;
    {
        std::lock_guard lock(gNgxEvaluateRouteMutex);
        const auto route = std::find_if(gNgxEvaluateRoutes.begin(), gNgxEvaluateRoutes.end(),
            [&](const NgxEvaluateRoute& candidate) {
                return candidate.wrapper == caller;
            });
        if (route != gNgxEvaluateRoutes.end())
            original = route->originalGetProcAddress;
    }
    if (!original)
        return nullptr;

    FARPROC function = original(module, functionName);
    if (reinterpret_cast<uintptr_t>(functionName) <= 0xFFFFu
        || !functionName
        || strcmp(functionName, "NVSDK_NGX_D3D12_EvaluateFeature") != 0
        || !function)
        return function;

    bool firstExposure = false;
    {
        std::lock_guard lock(gNgxEvaluateRouteMutex);
        const auto route = std::find_if(gNgxEvaluateRoutes.begin(), gNgxEvaluateRoutes.end(),
            [&](const NgxEvaluateRoute& candidate) {
                return candidate.wrapper == caller;
            });
        if (route == gNgxEvaluateRoutes.end())
            return function;
        firstExposure = route->originalEvaluate == nullptr;
        route->originalEvaluate = reinterpret_cast<NgxD3D12EvaluateFeatureFn>(function);
    }

    gNgxEvaluateHookExposed.store(true, std::memory_order_release);
    if (firstExposure)
        Log(L"Intercepted NVSDK_NGX_D3D12_EvaluateFeature for temporal-index validation");
    return reinterpret_cast<FARPROC>(&HookNgxD3D12EvaluateFeature);
}

NVSDK_NGX_Result NVSDK_CONV HookNgxD3D12EvaluateFeature(
    ID3D12GraphicsCommandList* commandList, const NVSDK_NGX_Handle* featureHandle,
    const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback)
{
    void* returnAddress = _ReturnAddress();
    const HMODULE caller = ModuleFromAddress(returnAddress);
    const uintptr_t callerRva = caller
        ? reinterpret_cast<uintptr_t>(returnAddress)
            - reinterpret_cast<uintptr_t>(caller)
        : 0;
    const uint64_t call = gNgxEvaluateCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    gNgxEvaluateSeen.store(true, std::memory_order_release);

    int rawCount = 0;
    int rawIndex = 0;
    int rawMultiFrameCountMax = 0;
    int rawMustCallEval = 0;
    ID3D12Resource* outputDisableInterpolation = nullptr;
    int rawNotRenderingGameFrames = 0;
    int rawStreamlineMode = 0;
    int rawReset = 0;
    int rawEvalFlags = 0;
    unsigned long long rawFrameId = 0;
    ID3D12Resource* outputInterpolated = nullptr;
    ID3D12Resource* outputReal = nullptr;
    NVSDK_NGX_Result countResult = NVSDK_NGX_Result_FAIL_InvalidParameter;
    NVSDK_NGX_Result indexResult = NVSDK_NGX_Result_FAIL_InvalidParameter;
    NVSDK_NGX_Result maxResult = NVSDK_NGX_Result_FAIL_InvalidParameter;
    NVSDK_NGX_Result mustCallResult = NVSDK_NGX_Result_FAIL_InvalidParameter;
    NVSDK_NGX_Result disableInterpolationResult = NVSDK_NGX_Result_FAIL_InvalidParameter;
    NVSDK_NGX_Result notRenderingResult = NVSDK_NGX_Result_FAIL_InvalidParameter;
    NVSDK_NGX_Result streamlineModeResult = NVSDK_NGX_Result_FAIL_InvalidParameter;
    NVSDK_NGX_Result resetResult = NVSDK_NGX_Result_FAIL_InvalidParameter;
    NVSDK_NGX_Result evalFlagsResult = NVSDK_NGX_Result_FAIL_InvalidParameter;
    NVSDK_NGX_Result frameIdResult = NVSDK_NGX_Result_FAIL_InvalidParameter;
    NVSDK_NGX_Result outputResult = NVSDK_NGX_Result_FAIL_InvalidParameter;
    NVSDK_NGX_Result outputRealResult = NVSDK_NGX_Result_FAIL_InvalidParameter;
    if (parameters)
    {
        countResult = parameters->Get("DLSSG.MultiFrameCount", &rawCount);
        indexResult = parameters->Get("DLSSG.MultiFrameIndex", &rawIndex);
        maxResult = parameters->Get(
            "DLSSG.MultiFrameCountMax", &rawMultiFrameCountMax);
        mustCallResult = parameters->Get("DLSSG.MustCallEval", &rawMustCallEval);
        disableInterpolationResult = parameters->Get(
            "DLSSG.OutputDisableInterpolation", &outputDisableInterpolation);
        notRenderingResult = parameters->Get(
            "DLSSG.NotRenderingGameFrames", &rawNotRenderingGameFrames);
        streamlineModeResult = parameters->Get(
            "DLSSG.StreamlineMode", &rawStreamlineMode);
        resetResult = parameters->Get("DLSSG.Reset", &rawReset);
        evalFlagsResult = parameters->Get("DLSSG.EvalFlags", &rawEvalFlags);
        frameIdResult = parameters->Get("DLSSG.BackbufferFrameID", &rawFrameId);
        outputResult = parameters->Get(
            "DLSSG.OutputInterpolated", &outputInterpolated);
        outputRealResult = parameters->Get("DLSSG.OutputReal", &outputReal);
    }

    const bool dynamic = gAppliedDynamicMode.load(std::memory_order_acquire);
    const uint32_t appliedMultiplier = gAppliedMultiplier.load(std::memory_order_acquire);
    const uint32_t expectedCount = !dynamic && appliedMultiplier >= 2
        && appliedMultiplier <= kMaximumMultiplier ? appliedMultiplier - 1 : 0;
    const bool countValid = countResult == NVSDK_NGX_Result_Success
        && rawCount >= 1 && rawCount <= static_cast<int>(kExperimentalMaximumGeneratedFrames)
        && (expectedCount == 0 || rawCount == static_cast<int>(expectedCount));
    const bool indexValid = indexResult == NVSDK_NGX_Result_Success
        && rawIndex >= 1 && rawIndex <= rawCount;
    const bool temporalParametersValid = countValid && indexValid;
    unsigned long long forcedFrameId = 0;
    int preservedReset = rawReset;
    const bool automaticFullStateRepair = dynamic
        || appliedMultiplier > kMinimumMultiplier;
    const bool forceFullState = temporalParametersValid
        && parameters
        && ((automaticFullStateRepair
                && !gDisableAutomaticFullNgxState.load(std::memory_order_acquire))
            || gForceFullNgxState.load(std::memory_order_acquire));
    gNgxFullStateRepairActive.store(
        forceFullState, std::memory_order_relaxed);
    if (forceFullState)
    {
        gNgxFullStateForcedCount.fetch_add(1, std::memory_order_relaxed);
        auto* mutableParameters = const_cast<NVSDK_NGX_Parameter*>(parameters);
        forcedFrameId = gForcedBackbufferFrameId.fetch_add(
            1, std::memory_order_relaxed);
        mutableParameters->Set("DLSSG.BackbufferFrameID", forcedFrameId);
        mutableParameters->Set("DLSSG.MultiFrameCountMax",
            static_cast<int>(kExperimentalMaximumGeneratedFrames));
        mutableParameters->Set("DLSSG.MustCallEval", 1);
        // OutputDisableInterpolation is a D3D12 resource written by NGX and
        // consumed asynchronously by Streamline.  Replacing it with scalar 0
        // destroys NVIDIA's reset/warm-up suppression signal and causes the
        // history-seeding batch to be presented as repeated generated frames.
        // Preserve the native resource exactly as supplied by Streamline.
        if (disableInterpolationResult == NVSDK_NGX_Result_Success
            && outputDisableInterpolation)
        {
            gNgxDisableInterpolationResourcePreservedCount.fetch_add(
                1, std::memory_order_relaxed);
        }
        mutableParameters->Set("DLSSG.NotRenderingGameFrames", 0);
        // Reset is the namespaced DLSSG.Reset input owned by Streamline.  Do
        // not synthesize generic Reset or hold reset across all five temporal
        // evaluations: either corrupts NVIDIA's per-frame history boundary.
        mutableParameters->Set("DLSSG.StreamlineMode", 1);
        mutableParameters->Set("DLSSG.EvalFlags", 0);
        if (resetResult == NVSDK_NGX_Result_Success)
            gNgxResetPreservedCount.fetch_add(1, std::memory_order_relaxed);
    }
    NgxD3D12EvaluateFeatureFn original = nullptr;
    {
        std::lock_guard lock(gNgxEvaluateRouteMutex);
        auto route = std::find_if(gNgxEvaluateRoutes.begin(), gNgxEvaluateRoutes.end(),
            [&](const NgxEvaluateRoute& candidate) {
                return candidate.wrapper == caller;
            });
        if (route == gNgxEvaluateRoutes.end() && gNgxEvaluateRoutes.size() == 1)
            route = gNgxEvaluateRoutes.begin();
        if (route != gNgxEvaluateRoutes.end())
            original = route->originalEvaluate;
    }

    gNgxLastCountGetResult.store(static_cast<int32_t>(countResult), std::memory_order_relaxed);
    gNgxLastIndexGetResult.store(static_cast<int32_t>(indexResult), std::memory_order_relaxed);
    gNgxLastRawCount.store(rawCount, std::memory_order_relaxed);
    gNgxLastRawIndex.store(rawIndex, std::memory_order_relaxed);
    gNgxLastFrameIdGetResult.store(
        static_cast<int32_t>(frameIdResult), std::memory_order_relaxed);
    gNgxLastOutputGetResult.store(
        static_cast<int32_t>(outputResult), std::memory_order_relaxed);
    gNgxLastDisableInterpolationGetResult.store(
        static_cast<int32_t>(disableInterpolationResult),
        std::memory_order_relaxed);
    gNgxLastDisableInterpolationResource.store(
        reinterpret_cast<uintptr_t>(outputDisableInterpolation),
        std::memory_order_relaxed);
    gNgxLastResetGetResult.store(
        static_cast<int32_t>(resetResult), std::memory_order_relaxed);
    gNgxLastRawReset.store(rawReset, std::memory_order_relaxed);
    gNgxLastFrameId.store(rawFrameId, std::memory_order_relaxed);
    gNgxLastOutputInterpolated.store(
        reinterpret_cast<uintptr_t>(outputInterpolated), std::memory_order_relaxed);
    if (temporalParametersValid)
    {
        gNgxTemporalValidCount.fetch_add(1, std::memory_order_relaxed);
        gNgxSeenCountMask.fetch_or(1u << static_cast<uint32_t>(rawCount),
            std::memory_order_relaxed);
        gNgxSeenIndexMask.fetch_or(1u << static_cast<uint32_t>(rawIndex - 1),
            std::memory_order_relaxed);
    }
    else
    {
        gNgxTemporalInvalidCount.fetch_add(1, std::memory_order_relaxed);
    }

    uint64_t outputBatchSequence = 0;
    bool outputBatchComplete = false;
    std::vector<uint64_t> abandonedOutputBatchSequences;
    if (temporalParametersValid
        && outputResult == NVSDK_NGX_Result_Success
        && outputInterpolated)
    {
        std::lock_guard lock(gNgxOutputBatchMutex);
        // BackbufferFrameID is not part of every native NGX parameter block.
        // Cyberpunk alternates multiple handles and can deliver the five output
        // indices out of order.  Match each index to the oldest incomplete batch
        // for that handle which has not seen it yet.  Repeated indices naturally
        // open another in-flight batch.
        for (auto candidate = gNgxOutputBatches.begin();
            candidate != gNgxOutputBatches.end();)
        {
            if (candidate->handle == featureHandle
                && candidate->count != rawCount)
            {
                abandonedOutputBatchSequences.push_back(candidate->sequence);
                candidate = gNgxOutputBatches.erase(candidate);
            }
            else
            {
                ++candidate;
            }
        }
        const uint32_t slot = static_cast<uint32_t>(rawIndex - 1);
        const uint32_t slotMask = 1u << slot;
        auto outputBatchIt = std::find_if(gNgxOutputBatches.begin(),
            gNgxOutputBatches.end(), [&](const NgxOutputBatch& candidate) {
                return candidate.handle == featureHandle
                    && candidate.count == rawCount
                    && (candidate.seenMask & slotMask) == 0;
            });
        if (outputBatchIt == gNgxOutputBatches.end())
        {
            size_t pendingForHandle = static_cast<size_t>(std::count_if(
                gNgxOutputBatches.begin(), gNgxOutputBatches.end(),
                [&](const NgxOutputBatch& candidate) {
                    return candidate.handle == featureHandle
                        && candidate.count == rawCount;
                }));
            if (pendingForHandle >= 4)
            {
                auto oldest = std::find_if(gNgxOutputBatches.begin(),
                    gNgxOutputBatches.end(), [&](const NgxOutputBatch& candidate) {
                        return candidate.handle == featureHandle
                            && candidate.count == rawCount;
                    });
                if (oldest != gNgxOutputBatches.end())
                {
                    abandonedOutputBatchSequences.push_back(oldest->sequence);
                    gNgxOutputBatches.erase(oldest);
                }
            }
            gNgxOutputBatches.push_back({});
            outputBatchIt = std::prev(gNgxOutputBatches.end());
            outputBatchIt->handle = featureHandle;
            outputBatchIt->sequence = gNgxOutputBatchSequence.fetch_add(
                1, std::memory_order_relaxed) + 1;
            outputBatchIt->count = rawCount;
        }
        NgxOutputBatch& outputBatch = *outputBatchIt;

        outputBatchSequence = outputBatch.sequence;
        outputBatch.outputs[slot] =
            reinterpret_cast<uintptr_t>(outputInterpolated);
        outputBatch.seenMask |= slotMask;
        const uint32_t expectedMask = (1u << static_cast<uint32_t>(rawCount)) - 1u;
        outputBatchComplete = outputBatch.seenMask == expectedMask;
        if (outputBatchComplete)
        {
            uint32_t uniqueCount = 0;
            for (int index = 0; index < rawCount; ++index)
            {
                bool first = true;
                for (int previous = 0; previous < index; ++previous)
                {
                    if (outputBatch.outputs[previous]
                        == outputBatch.outputs[index])
                    {
                        first = false;
                        break;
                    }
                }
                if (first)
                    ++uniqueCount;
            }
            gNgxLastOutputUniqueCount.store(uniqueCount, std::memory_order_relaxed);
            gNgxOutputCompleteBatches.fetch_add(1, std::memory_order_relaxed);
            if (uniqueCount != static_cast<uint32_t>(rawCount))
                gNgxOutputAliasedBatches.fetch_add(1, std::memory_order_relaxed);
            if (call <= 128)
            {
                Log(L"NGX output batch: handle=%p frame=%llu count=%d "
                    L"uniqueOutputs=%u aliased=%d",
                    featureHandle, rawFrameId, rawCount, uniqueCount,
                    uniqueCount != static_cast<uint32_t>(rawCount));
            }
            gNgxOutputBatches.erase(outputBatchIt);
        }
    }
    for (const uint64_t abandonedBatch : abandonedOutputBatchSequences)
        ngx_output_probe::AbandonImmutableBatch(abandonedBatch);

    if (call <= 128)
    {
        Log(L"NGX temporal sample #%llu: callerRva=0x%llX handle=%p count=%d index=%d "
            L"frame=%llu output=%p outputReal=%p getCount=0x%08X "
            L"getIndex=0x%08X getFrame=0x%08X getOutput=0x%08X "
            L"getOutputReal=0x%08X expectedCount=%u valid=%d",
            static_cast<unsigned long long>(call),
            static_cast<unsigned long long>(callerRva),
            featureHandle, rawCount, rawIndex,
            rawFrameId, outputInterpolated, outputReal,
            static_cast<uint32_t>(countResult), static_cast<uint32_t>(indexResult),
            static_cast<uint32_t>(frameIdResult), static_cast<uint32_t>(outputResult),
            static_cast<uint32_t>(outputRealResult),
            expectedCount, temporalParametersValid);
        Log(L"NGX full state #%llu: max=%d mustCallEval=%d disableInterpolation=%p "
            L"notRendering=%d streamlineMode=%d reset=%d evalFlags=%d "
            L"frameObserved=%llu frameForced=%llu resetPreserved=%d force=%d results="
            L"[%08X,%08X,%08X,%08X,%08X,%08X,%08X]",
            static_cast<unsigned long long>(call), rawMultiFrameCountMax,
            rawMustCallEval, outputDisableInterpolation,
            rawNotRenderingGameFrames, rawStreamlineMode, rawReset, rawEvalFlags,
            rawFrameId, forcedFrameId, preservedReset, forceFullState,
            static_cast<uint32_t>(maxResult),
            static_cast<uint32_t>(mustCallResult),
            static_cast<uint32_t>(disableInterpolationResult),
            static_cast<uint32_t>(notRenderingResult),
            static_cast<uint32_t>(streamlineModeResult),
            static_cast<uint32_t>(resetResult),
            static_cast<uint32_t>(evalFlagsResult));
    }

    const ngx_output_probe::ImmutableOutput immutableOutput =
        temporalParametersValid
            && outputResult == NVSDK_NGX_Result_Success
            && outputInterpolated && parameters
        ? ngx_output_probe::PrepareImmutableOutput(commandList,
            outputInterpolated, const_cast<NVSDK_NGX_Parameter*>(parameters),
            outputBatchSequence, rawCount, rawIndex, outputBatchComplete)
        : ngx_output_probe::ImmutableOutput{};
    if (immutableOutput && call <= 128)
    {
        Log(L"NGX immutable output #%llu: original=%p immutable=%p slot=%u sequence=%llu",
            static_cast<unsigned long long>(call), outputInterpolated,
            immutableOutput.resource, immutableOutput.slot,
            static_cast<unsigned long long>(immutableOutput.sequence));
    }

    const NVSDK_NGX_Result evaluateResult = original
        ? original(commandList, featureHandle, parameters, callback)
        : NVSDK_NGX_Result_FAIL_NotInitialized;
    if (gExperimentalTemporalPreEmphasis.load(std::memory_order_relaxed)
        && temporalParametersValid && rawCount > 1 && call <= 128)
    {
        // The environment-gated module patch replaces NGX's internal
        // index/(count+1) calculation with index-count/2.  Keep the public
        // MultiFrameIndex untouched so its normal range validation and all
        // downstream indexing continue to see [1, count].
        const float nativeRatio = static_cast<float>(rawIndex)
            / static_cast<float>(rawCount + 1);
        const float suppliedRatio = static_cast<float>(rawIndex)
            - static_cast<float>(rawCount) * 0.5f;
        Log(L"NGX temporal pre-emphasis #%llu: count=%d nativeIndex=%d "
            L"nativeRatio=%.6f suppliedRatio=%.6f patchActive=%d evaluate=0x%08X",
            static_cast<unsigned long long>(call), rawCount, rawIndex,
            nativeRatio, suppliedRatio,
            gExperimentalTemporalPreEmphasisPatchActive.load(
                std::memory_order_relaxed),
            static_cast<uint32_t>(evaluateResult));
    }
    ID3D12Resource* capturedOutput = outputInterpolated;
    if (immutableOutput)
    {
        if (evaluateResult == NVSDK_NGX_Result_Success
            && ngx_output_probe::FinalizeImmutableOutput(
                commandList, outputInterpolated, immutableOutput))
        {
            capturedOutput = immutableOutput.resource;
        }
        else
        {
            ngx_output_probe::CancelImmutableOutput(
                const_cast<NVSDK_NGX_Parameter*>(parameters),
                outputInterpolated, immutableOutput);
        }
    }
    if (evaluateResult == NVSDK_NGX_Result_Success
        && temporalParametersValid
        && capturedOutput
        && outputBatchSequence != 0)
    {
        if (rawIndex == 1
            && outputRealResult == NVSDK_NGX_Result_Success
            && outputReal)
        {
            ngx_output_probe::CaptureAfterEvaluate(commandList, outputReal,
                featureHandle, outputBatchSequence,
                forceFullState ? forcedFrameId : rawFrameId,
                rawCount, 0, ngx_output_probe::CapturedOutputKind::eReal);
        }
        ngx_output_probe::CaptureAfterEvaluate(commandList, capturedOutput,
            featureHandle, outputBatchSequence,
            forceFullState ? forcedFrameId : rawFrameId,
            rawCount, rawIndex);
    }
    return evaluateResult;
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

bool InstallEarlyD3D12Hooks()
{
    void* createOriginal = nullptr;
    bool createInstalled = HookMainExecutableImport("sl.interposer.dll",
        "D3D12CreateDevice", reinterpret_cast<void*>(&HookD3D12CreateDevice),
        createOriginal);
    if (!createInstalled)
    {
        createInstalled = HookMainExecutableImport("d3d12.dll",
            "D3D12CreateDevice", reinterpret_cast<void*>(&HookD3D12CreateDevice),
            createOriginal);
    }
    if (createOriginal)
    {
        gOriginalD3D12CreateDevice.store(
            reinterpret_cast<D3D12CreateDeviceFn>(createOriginal),
            std::memory_order_release);
    }
    gD3D12CreateDeviceHookInstalled.store(
        createInstalled, std::memory_order_release);

    void* upgradeOriginal = nullptr;
    const bool upgradeInstalled = HookMainExecutableImport("sl.interposer.dll",
        "slUpgradeInterface", reinterpret_cast<void*>(&HookSlUpgradeInterface),
        upgradeOriginal);
    if (upgradeOriginal)
    {
        gOriginalUpgradeInterface.store(
            reinterpret_cast<PFun_slUpgradeInterface*>(upgradeOriginal),
            std::memory_order_release);
    }
    gSlUpgradeInterfaceHookInstalled.store(
        upgradeInstalled, std::memory_order_release);
    return createInstalled || upgradeInstalled;
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
    gD3DDeviceHookInstalled.store(installed, std::memory_order_release);
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

static constexpr std::array<uint8_t, 10> kWrapperPattern{
    0xBA, 0x05, 0x00, 0x00, 0x00, 0x3B, 0xCA, 0x0F, 0x42, 0xD1
};
static constexpr std::array<uint8_t, 3> kWrapperOriginal{ 0x0F, 0x42, 0xD1 };
static constexpr std::array<uint8_t, 3> kWrapperReplacement{ 0x90, 0x90, 0x90 };
static const PatternPatch kWrapperPatch{
    L"Streamline maximum", kWrapperPattern.data(), kWrapperPattern.size(), 7,
    kWrapperOriginal.data(), kWrapperReplacement.data(), kWrapperOriginal.size()
};

static constexpr std::array<uint8_t, 13> kNgxPattern{
    0x84, 0xD2, 0x0F, 0x84, 0x03, 0x01, 0x00, 0x00, 0xBE, 0x05, 0x00, 0x00, 0x00
};
static constexpr std::array<uint8_t, 6> kNgxOriginal{ 0x0F, 0x84, 0x03, 0x01, 0x00, 0x00 };
static constexpr std::array<uint8_t, 6> kNgxReplacement{ 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
static const PatternPatch kNgxPatch{
    L"NGX device support", kNgxPattern.data(), kNgxPattern.size(), 2,
    kNgxOriginal.data(), kNgxReplacement.data(), kNgxOriginal.size()
};

// Feature initialization stores a dedicated multi-frame capability byte after
// comparing the real GPU architecture with Blackwell (0x1B0).  Patch only that
// stored capability.  Spoofing the architecture export itself changes other
// architecture-dependent paths and crashes Ada during renderer startup.
static constexpr std::array<uint8_t, 15> kNgxAdaSynthesisPattern{
    0x3D, 0xB0, 0x01, 0x00, 0x00,
    0x0F, 0x93, 0xC0,
    0x88, 0x47, 0x28,
    0x40, 0x88, 0x77, 0x29
};
static constexpr std::array<uint8_t, 3> kNgxAdaSynthesisOriginal{
    0x0F, 0x93, 0xC0
};
static constexpr std::array<uint8_t, 3> kNgxAdaSynthesisReplacement{
    0xB0, 0x01, 0x90
};
static const PatternPatch kNgxAdaSynthesisPatch{
    L"NGX architecture capability", kNgxAdaSynthesisPattern.data(),
    kNgxAdaSynthesisPattern.size(), 5, kNgxAdaSynthesisOriginal.data(),
    kNgxAdaSynthesisReplacement.data(), kNgxAdaSynthesisOriginal.size()
};

// Parameter population independently clamps DLSSG.MultiFrameCountMax to one
// below Blackwell.  Lower only this parameter's threshold to Ada (0x190).
// The global architecture export must retain the real GPU architecture because
// other architecture-dependent backend paths are not safe to spoof.
static constexpr std::array<uint8_t, 17> kNgxMultiFrameMaximumPattern{
    0x81, 0xFD, 0xB0, 0x01, 0x00, 0x00,
    0x0F, 0x8C, 0x9F, 0x00, 0x00, 0x00,
    0xBF, 0x05, 0x00, 0x00, 0x00
};
static constexpr std::array<uint8_t, 6> kNgxMultiFrameMaximumOriginal{
    0x81, 0xFD, 0xB0, 0x01, 0x00, 0x00
};
static constexpr std::array<uint8_t, 6> kNgxMultiFrameMaximumReplacement{
    0x81, 0xFD, 0x90, 0x01, 0x00, 0x00
};
static const PatternPatch kNgxMultiFrameMaximumPatch{
    L"NGX multi-frame parameter maximum", kNgxMultiFrameMaximumPattern.data(),
    kNgxMultiFrameMaximumPattern.size(), 0,
    kNgxMultiFrameMaximumOriginal.data(),
    kNgxMultiFrameMaximumReplacement.data(),
    kNgxMultiFrameMaximumOriginal.size()
};

// NGX selects its embedded DL4RT target from the real GPU architecture.  This
// opt-in diagnostic redirects only the Ada (0x190) branch from the sm89 target
// block to the adjacent sm120 target block.  It is intentionally environment
// gated: sm120 code may be rejected by an Ada driver/device, and this test is
// only meant to establish whether the remaining boundary is model/kernel
// selection rather than Streamline parameter propagation.
static constexpr std::array<uint8_t, 21> kNgxExperimentalSm120Pattern{
    0x83, 0xE9, 0x10, 0x74, 0x75,
    0x83, 0xE9, 0x10, 0x74, 0x60,
    0x83, 0xE9, 0x10, 0x74, 0x4B,
    0x44, 0x88, 0x3A, 0x83, 0xF9, 0x20
};
static constexpr std::array<uint8_t, 1> kNgxExperimentalSm120Original{ 0x4B };
static constexpr std::array<uint8_t, 1> kNgxExperimentalSm120Replacement{ 0x2B };
static const PatternPatch kNgxExperimentalSm120Patch{
    L"NGX experimental Ada-to-sm120 target", kNgxExperimentalSm120Pattern.data(),
    kNgxExperimentalSm120Pattern.size(), 14,
    kNgxExperimentalSm120Original.data(),
    kNgxExperimentalSm120Replacement.data(),
    kNgxExperimentalSm120Original.size()
};

// EndpointDL4RTWrapper selects separate DL1/DL2 network implementations for
// SM < 89, SM == 89, and SM > 89.  Lowering the equality threshold by one sends
// Ada's reported SM 89 through the same constructors selected on Blackwell.
// Keep both sites gated and DLSS-G-scoped; these constructors can still reject
// Ada if the selected graph or kernels require Blackwell hardware.
static constexpr std::array<uint8_t, 11> kNgxDl4rtDl1Sm120Pattern{
    0xB9, 0x88, 0x14, 0x00, 0x00,
    0x83, 0xF8, 0x59,
    0x7E, 0x21, 0xE8
};
static constexpr std::array<uint8_t, 11> kNgxDl4rtDl2Sm120Pattern{
    0xB9, 0x60, 0x20, 0x00, 0x00,
    0x83, 0xF8, 0x59,
    0x7E, 0x21, 0xE8
};
static constexpr std::array<uint8_t, 1> kNgxDl4rtSm89Original{ 0x59 };
static constexpr std::array<uint8_t, 1> kNgxDl4rtSm120Replacement{ 0x58 };
static const PatternPatch kNgxDl4rtDl1Sm120Patch{
    L"NGX experimental DL4RT DL1 Blackwell path",
    kNgxDl4rtDl1Sm120Pattern.data(), kNgxDl4rtDl1Sm120Pattern.size(), 7,
    kNgxDl4rtSm89Original.data(), kNgxDl4rtSm120Replacement.data(), 1
};
static const PatternPatch kNgxDl4rtDl2Sm120Patch{
    L"NGX experimental DL4RT DL2 Blackwell path",
    kNgxDl4rtDl2Sm120Pattern.data(), kNgxDl4rtDl2Sm120Pattern.size(), 7,
    kNgxDl4rtSm89Original.data(), kNgxDl4rtSm120Replacement.data(), 1
};

// ComputeAndValidateTimeFactor normally stores:
//
//   temporalRatio = MultiFrameIndex / (MultiFrameCount + 1)
//
// The Ada multi-frame output probe behaves as if that value is contracted
// around 0.5 once more.  This opt-in sample experiment pre-emphasizes the
// internal float without changing or bypassing the validated public indices:
//
//   temporalRatio = MultiFrameIndex - MultiFrameCount / 2
//
// For count=1 this remains 0.5.  For count=5 it supplies
// [-1.5, -0.5, 0.5, 1.5, 2.5].  The replacement is the same size as NVIDIA's
// original ratio-calculation block and preserves the following success path.
static constexpr std::array<uint8_t, 44> kNgxTemporalPreEmphasisPattern{
    0x0F, 0x57, 0xC9,
    0xF3, 0x48, 0x0F, 0x2A, 0xC9,
    0x41, 0x8B, 0xC0,
    0x0F, 0x57, 0xC0,
    0xF3, 0x48, 0x0F, 0x2A, 0xC0,
    0xF3, 0x0F, 0x58, 0x05, 0xA7, 0x97, 0x03, 0x00,
    0xF3, 0x0F, 0x5E, 0xC8,
    0xF3, 0x0F, 0x11, 0x8B, 0xF4, 0x04, 0x00, 0x00,
    0xB8, 0x01, 0x00, 0x00, 0x00
};
static constexpr std::array<uint8_t, 39> kNgxTemporalPreEmphasisOriginal{
    0x0F, 0x57, 0xC9,
    0xF3, 0x48, 0x0F, 0x2A, 0xC9,
    0x41, 0x8B, 0xC0,
    0x0F, 0x57, 0xC0,
    0xF3, 0x48, 0x0F, 0x2A, 0xC0,
    0xF3, 0x0F, 0x58, 0x05, 0xA7, 0x97, 0x03, 0x00,
    0xF3, 0x0F, 0x5E, 0xC8,
    0xF3, 0x0F, 0x11, 0x8B, 0xF4, 0x04, 0x00, 0x00
};
static constexpr std::array<uint8_t, 39> kNgxTemporalPreEmphasisReplacement{
    0x8D, 0x0C, 0x09,                         // lea ecx,[rcx+rcx]
    0x44, 0x29, 0xC1,                         // sub ecx,r8d
    0x0F, 0x57, 0xC9,                         // xorps xmm1,xmm1
    0xF3, 0x0F, 0x2A, 0xC9,                   // cvtsi2ss xmm1,ecx
    0xB8, 0x00, 0x00, 0x00, 0x3F,             // mov eax,0.5f
    0x66, 0x0F, 0x6E, 0xC0,                   // movd xmm0,eax
    0xF3, 0x0F, 0x59, 0xC8,                   // mulss xmm1,xmm0
    0xF3, 0x0F, 0x11, 0x8B, 0xF4, 0x04, 0x00, 0x00,
    0x90, 0x90, 0x90, 0x90, 0x90
};
static const PatternPatch kNgxTemporalPreEmphasisPatch{
    L"NGX experimental temporal-ratio pre-emphasis",
    kNgxTemporalPreEmphasisPattern.data(),
    kNgxTemporalPreEmphasisPattern.size(), 0,
    kNgxTemporalPreEmphasisOriginal.data(),
    kNgxTemporalPreEmphasisReplacement.data(),
    kNgxTemporalPreEmphasisOriginal.size()
};

// NVIDIA's downloaded DLSS-G implementation contains the same function with
// a 0x20 RVA shift.  The RIP-relative address inside the matched block is
// therefore 0x20 smaller even though the replacement itself is identical.
// Keep this separate from the exported/front DLL signature so the experimental
// patch remains exact and fails closed if NVIDIA changes either image.
static constexpr std::array<uint8_t, 44> kNgxCachedTemporalPreEmphasisPattern{
    0x0F, 0x57, 0xC9,
    0xF3, 0x48, 0x0F, 0x2A, 0xC9,
    0x41, 0x8B, 0xC0,
    0x0F, 0x57, 0xC0,
    0xF3, 0x48, 0x0F, 0x2A, 0xC0,
    0xF3, 0x0F, 0x58, 0x05, 0x87, 0x97, 0x03, 0x00,
    0xF3, 0x0F, 0x5E, 0xC8,
    0xF3, 0x0F, 0x11, 0x8B, 0xF4, 0x04, 0x00, 0x00,
    0xB8, 0x01, 0x00, 0x00, 0x00
};
static constexpr std::array<uint8_t, 39> kNgxCachedTemporalPreEmphasisOriginal{
    0x0F, 0x57, 0xC9,
    0xF3, 0x48, 0x0F, 0x2A, 0xC9,
    0x41, 0x8B, 0xC0,
    0x0F, 0x57, 0xC0,
    0xF3, 0x48, 0x0F, 0x2A, 0xC0,
    0xF3, 0x0F, 0x58, 0x05, 0x87, 0x97, 0x03, 0x00,
    0xF3, 0x0F, 0x5E, 0xC8,
    0xF3, 0x0F, 0x11, 0x8B, 0xF4, 0x04, 0x00, 0x00
};
static const PatternPatch kNgxCachedTemporalPreEmphasisPatch{
    L"NGX cached-backend temporal-ratio pre-emphasis",
    kNgxCachedTemporalPreEmphasisPattern.data(),
    kNgxCachedTemporalPreEmphasisPattern.size(), 0,
    kNgxCachedTemporalPreEmphasisOriginal.data(),
    kNgxTemporalPreEmphasisReplacement.data(),
    kNgxCachedTemporalPreEmphasisOriginal.size()
};

struct PatternPatchResult
{
    bool candidate = false;
    bool patched = false;
    uint8_t* match = nullptr;
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

std::wstring LoadedModulePath(HMODULE module)
{
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(module, path, _countof(path));
    return length > 0 && length < _countof(path)
        ? std::wstring(path, length) : std::wstring{};
}

void RecomputeModuleStateLocked()
{
    uint32_t wrapperCandidates = 0;
    uint32_t patchedWrappers = 0;
    uint32_t ngxCandidates = 0;
    uint32_t patchedNgx = 0;
    uint32_t ngxSynthesisCandidates = 0;
    uint32_t patchedNgxSynthesis = 0;
    bool perSampleSynthesisReady = false;
    bool temporalPreEmphasisBackendReady = false;
    uint32_t wrapperRouteBits = 0;
    uint32_t ngxRouteBits = 0;
    for (const auto& record : gModuleRecords)
    {
        if (record.wrapperCandidate)
            ++wrapperCandidates;
        if (record.wrapperPatched)
        {
            ++patchedWrappers;
            wrapperRouteBits |= ClassifyLoadedRoute(record.path);
        }
        if (record.ngxCandidate)
            ++ngxCandidates;
        if (record.ngxPatched)
        {
            ++patchedNgx;
            ngxRouteBits |= ClassifyLoadedRoute(record.path);
        }
        if (record.ngxAdaSynthesisCandidate)
            ++ngxSynthesisCandidates;
        if (record.ngxAdaSynthesisPatched)
            ++patchedNgxSynthesis;
        if (record.ngxPatched && record.ngxAdaSynthesisPatched
            && record.ngxMultiFrameMaximumPatched)
            perSampleSynthesisReady = true;
        if (record.ngxCachedDlssgImplementation
            && record.ngxTemporalPreEmphasisPatched)
            temporalPreEmphasisBackendReady = true;
    }
    gLoadedWrapperCandidates.store(wrapperCandidates, std::memory_order_release);
    gPatchedWrapperCandidates.store(patchedWrappers, std::memory_order_release);
    gLoadedNgxCandidates.store(ngxCandidates, std::memory_order_release);
    gPatchedNgxCandidates.store(patchedNgx, std::memory_order_release);
    gLoadedNgxSynthesisCandidates.store(
        ngxSynthesisCandidates, std::memory_order_release);
    gPatchedNgxSynthesisCandidates.store(
        patchedNgxSynthesis, std::memory_order_release);
    gExperimentalTemporalPreEmphasisPatchActive.store(
        temporalPreEmphasisBackendReady, std::memory_order_release);
    const bool wasSynthesisReady = gPerSampleSynthesisReady.exchange(
        perSampleSynthesisReady, std::memory_order_acq_rel);
    if (perSampleSynthesisReady && !wasSynthesisReady)
    {
        // A high-multiplier request may have been safely submitted as 2x while
        // the OTA backend was still loading. Let the next render-thread state
        // call reapply that same request now that the guarded patch is ready.
        gAppliedRevision.store(0, std::memory_order_release);
        gAttemptedRevision.store(0, std::memory_order_release);
    }
    gWrapperRouteBits.store(wrapperRouteBits, std::memory_order_release);
    gNgxRouteBits.store(ngxRouteBits, std::memory_order_release);
}

void LogModuleInventory(const ModuleRecord& record)
{
    if (!record.wrapperExport && !record.ngxExport
        && !record.ngxCachedDlssgImplementation)
        return;
    Log(L"Loaded module: wrapperExport=%d wrapperCandidate=%d wrapperPatched=%d "
        L"ngxEvaluateLookupHooked=%d "
        L"ngxExport=%d ngxCandidate=%d ngxPatched=%d "
        L"ngxAdaSynthesisCandidate=%d ngxAdaSynthesisPatched=%d "
        L"ngxMultiFrameMaximumCandidate=%d ngxMultiFrameMaximumPatched=%d "
        L"ngxCachedDlssgImplementation=%d temporalPreEmphasisCandidate=%d "
        L"temporalPreEmphasisPatched=%d path=%s",
        record.wrapperExport, record.wrapperCandidate, record.wrapperPatched,
        record.ngxEvaluateLookupHooked,
        record.ngxExport, record.ngxCandidate, record.ngxPatched,
        record.ngxAdaSynthesisCandidate, record.ngxAdaSynthesisPatched,
        record.ngxMultiFrameMaximumCandidate,
        record.ngxMultiFrameMaximumPatched,
        record.ngxCachedDlssgImplementation,
        record.ngxTemporalPreEmphasisCandidate,
        record.ngxTemporalPreEmphasisPatched,
        record.path.c_str());
}

bool IsCachedDlssgImplementationPath(const std::wstring& path)
{
    std::wstring normalized = path;
    std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
        [](wchar_t character) {
            return static_cast<wchar_t>(std::towlower(character));
        });
    return normalized.ends_with(L".bin")
        && normalized.find(L"\\nvidia\\ngx\\models\\dlssg\\")
            != std::wstring::npos;
}

ModuleRecord InspectLoadedModule(HMODULE module, const std::wstring& suppliedPath)
{
    if (!module)
        return {};
    const std::wstring path = suppliedPath.empty() ? LoadedModulePath(module) : suppliedPath;
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
            record.wrapperExport = ModuleExportsFunction(module, "slGetPluginFunction");
            record.ngxExport = ModuleExportsFunction(module, "NVSDK_NGX_D3D12_CreateFeature")
                && ModuleExportsFunction(module, "NVSDK_NGX_GetGPUArchitecture");
            record.ngxCachedDlssgImplementation =
                IsCachedDlssgImplementationPath(path);
            if (!record.wrapperExport && !record.ngxExport
                && !record.ngxCachedDlssgImplementation)
                return record;
            if (record.wrapperExport)
            {
                const PatternPatchResult result =
                    PatchUniqueExecutablePattern(module, path, kWrapperPatch);
                record.wrapperCandidate = result.candidate;
                record.wrapperPatched = result.patched;
                if (result.patched && result.match)
                {
                    record.wrapperMaximumImmediate = result.match + 1;
                    SetWrapperMaximum(record,
                        RequestedMaximumGeneratedFrames(ReadControlSnapshot().control));
                }
                InstallNgxEvaluateLookupHook(record);
            }
            if (record.ngxExport)
            {
                const PatternPatchResult result =
                    PatchUniqueExecutablePattern(module, path, kNgxPatch);
                record.ngxCandidate = result.candidate;
                record.ngxPatched = result.patched;

                const PatternPatchResult synthesisResult =
                    PatchUniqueExecutablePattern(module, path, kNgxAdaSynthesisPatch);
                record.ngxAdaSynthesisCandidate = synthesisResult.candidate;
                record.ngxAdaSynthesisPatched = synthesisResult.patched;

                const PatternPatchResult maximumResult =
                    PatchUniqueExecutablePattern(module, path,
                        kNgxMultiFrameMaximumPatch);
                record.ngxMultiFrameMaximumCandidate = maximumResult.candidate;
                record.ngxMultiFrameMaximumPatched = maximumResult.patched;

                // The architecture-to-target helper is shared by several NGX
                // features.  Apply this experiment only to a module that also
                // matched both DLSS-G-specific multi-frame patches.
                if (gExperimentalSm120Target.load(std::memory_order_acquire)
                    && synthesisResult.candidate
                    && maximumResult.candidate)
                {
                    PatchUniqueExecutablePattern(module, path,
                        kNgxExperimentalSm120Patch);
                }
                if (gExperimentalDl4rtSm120Path.load(std::memory_order_acquire)
                    && synthesisResult.candidate
                    && maximumResult.candidate)
                {
                    PatchUniqueExecutablePattern(module, path,
                        kNgxDl4rtDl1Sm120Patch);
                    PatchUniqueExecutablePattern(module, path,
                        kNgxDl4rtDl2Sm120Patch);
                }
                if (gExperimentalTemporalPreEmphasis.load(
                        std::memory_order_acquire)
                    && synthesisResult.candidate
                    && maximumResult.candidate)
                {
                    const PatternPatchResult temporalPreEmphasisResult =
                        PatchUniqueExecutablePattern(module, path,
                            kNgxTemporalPreEmphasisPatch);
                    record.ngxTemporalPreEmphasisCandidate =
                        temporalPreEmphasisResult.candidate;
                    record.ngxTemporalPreEmphasisPatched =
                        temporalPreEmphasisResult.patched;
                }

            }
            if (record.ngxCachedDlssgImplementation
                && gExperimentalTemporalPreEmphasis.load(
                    std::memory_order_acquire))
            {
                const PatternPatchResult temporalPreEmphasisResult =
                    PatchUniqueExecutablePattern(module, path,
                        kNgxCachedTemporalPreEmphasisPatch);
                record.ngxTemporalPreEmphasisCandidate =
                    temporalPreEmphasisResult.candidate;
                record.ngxTemporalPreEmphasisPatched =
                    temporalPreEmphasisResult.patched;
            }
            record.inventoryLogged = gLogReady.load(std::memory_order_acquire);
            logInventory = record.inventoryLogged;
            gModuleRecords.push_back(record);
            RecomputeModuleStateLocked();
            snapshot = record;
        }
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
    {
        std::lock_guard lock(gNgxEvaluateRouteMutex);
        gNgxEvaluateRoutes.erase(std::remove_if(
            gNgxEvaluateRoutes.begin(), gNgxEvaluateRoutes.end(),
            [&](const NgxEvaluateRoute& route) { return route.wrapper == module; }),
            gNgxEvaluateRoutes.end());
        gNgxEvaluateLookupHookInstalled.store(!gNgxEvaluateRoutes.empty(),
            std::memory_order_release);
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(module);
    if (gActiveWrapperBase.load(std::memory_order_acquire) == base)
    {
        gActiveWrapperPatched.store(false, std::memory_order_release);
        gActiveWrapperObserved.store(false, std::memory_order_release);
        gActiveWrapperBase.store(0, std::memory_order_release);
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

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry))
    {
        do
        {
            InspectLoadedModule(reinterpret_cast<HMODULE>(entry.modBaseAddr), entry.szExePath);
            entry.dwSize = sizeof(entry);
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
}

void ObserveActiveWrapperProvider(void* function)
{
    if (!function)
        return;
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(function, &memory, sizeof(memory)) != sizeof(memory)
        || !memory.AllocationBase)
        return;

    HMODULE module = static_cast<HMODULE>(memory.AllocationBase);
    const ModuleRecord record = InspectLoadedModule(module, LoadedModulePath(module));
    if (!record.wrapperExport)
        return;

    const uintptr_t base = reinterpret_cast<uintptr_t>(module);
    const uintptr_t previous = gActiveWrapperBase.exchange(base, std::memory_order_acq_rel);
    gActiveWrapperPatched.store(record.wrapperPatched, std::memory_order_release);
    gActiveWrapperObserved.store(true, std::memory_order_release);
    if (previous != base)
        Log(L"Active DLSS-G wrapper provider: patched=%d path=%s",
            record.wrapperPatched, record.path.c_str());
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
    if (!data)
        return;

    if (reason == kDllUnloaded)
    {
        RemoveLoadedModule(static_cast<HMODULE>(data->unloaded.dllBase));
        return;
    }
    if (reason != kDllLoaded || !data->loaded.dllBase)
        return;

    std::wstring path;
    if (data->loaded.fullDllName && data->loaded.fullDllName->Buffer)
        path.assign(data->loaded.fullDllName->Buffer,
            data->loaded.fullDllName->Length / sizeof(wchar_t));
    InspectLoadedModule(static_cast<HMODULE>(data->loaded.dllBase), path);
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
    gForceFullNgxState.store(
        ReadEnvironmentFlag(L"RTX40_MFG_FORCE_FULL_NGX_STATE"),
        std::memory_order_release);
    gDisableAutomaticFullNgxState.store(
        ReadEnvironmentFlag(L"RTX40_MFG_DISABLE_FULL_NGX_STATE"),
        std::memory_order_release);
    gExperimentalSm120Target.store(
        ReadEnvironmentFlag(L"RTX40_MFG_EXPERIMENTAL_SM120_TARGET"),
        std::memory_order_release);
    gExperimentalDl4rtSm120Path.store(
        ReadEnvironmentFlag(L"RTX40_MFG_EXPERIMENTAL_DL4RT_SM120_PATH"),
        std::memory_order_release);
    gExperimentalTemporalPreEmphasis.store(
        ReadEnvironmentFlag(L"RTX40_MFG_EXPERIMENTAL_TEMPORAL_PREEMPHASIS"),
        std::memory_order_release);
    gPresentProbeEnvironmentEnabled.store(
        ReadEnvironmentFlag(L"RTX40_MFG_PRESENT_PROBE"),
        std::memory_order_release);
    gNgxOutputProbeEnvironmentEnabled.store(
        ReadEnvironmentFlag(L"RTX40_MFG_NGX_OUTPUT_PROBE"),
        std::memory_order_release);
    gImmutableOutputEnvironmentEnabled.store(
        ReadEnvironmentFlag(L"RTX40_MFG_IMMUTABLE_OUTPUTS"),
        std::memory_order_release);
    present_probe::SetEnabled(
        gPresentProbeEnvironmentEnabled.load(std::memory_order_acquire));
    ngx_output_probe::Configure(
        gNgxOutputProbeEnvironmentEnabled.load(std::memory_order_acquire),
        gImmutableOutputEnvironmentEnabled.load(std::memory_order_acquire),
        &ProbeLog);
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

    wchar_t executablePath[32768]{};
    GetModuleFileNameW(nullptr, executablePath, _countof(executablePath));
    const std::wstring executableDirectory = ParentPath(executablePath);
    gConfigPath = ResolveConfigPath(static_cast<HMODULE>(context), executableDirectory);
    gStatusPath = JoinPath(ParentPath(gConfigPath), L"bridge_status.json");
    DeleteFileW(gStatusPath.c_str());
    const ControlConfig initialControl = ReadInitialControl();
    StoreControl(initialControl);
    FILETIME configWriteTime{};
    ReadLastWriteTime(gConfigPath, configWriteTime);
    Log(L"Initial control: mode=%s multiplier=%ux dynamicTarget=%u FPS "
        L"dynamicExperimental56=%d generatedOnlyDebug=%d; config: %s",
        initialControl.dynamic ? L"dynamic" : L"fixed", initialControl.multiplier,
        initialControl.dynamicTargetFrameRate, initialControl.dynamicExperimental56,
        initialControl.generatedOnlyDebug,
        gConfigPath.c_str());

    Log(L"Patch worker started for PID %lu", static_cast<unsigned long>(pid));
    Log(L"Sample diagnostics: forceFullNgxState=%d disableAutomaticFullNgxState=%d "
        L"experimentalSm120Target=%d experimentalDl4rtSm120Path=%d "
        L"experimentalTemporalPreEmphasis=%d presentProbe=%d ngxOutputProbe=%d "
        L"immutableOutputs=%d",
        gForceFullNgxState.load(std::memory_order_relaxed),
        gDisableAutomaticFullNgxState.load(std::memory_order_relaxed),
        gExperimentalSm120Target.load(std::memory_order_relaxed),
        gExperimentalDl4rtSm120Path.load(std::memory_order_relaxed),
        gExperimentalTemporalPreEmphasis.load(std::memory_order_relaxed),
        gPresentProbeEnvironmentEnabled.load(std::memory_order_relaxed),
        gNgxOutputProbeEnvironmentEnabled.load(std::memory_order_relaxed),
        gImmutableOutputEnvironmentEnabled.load(std::memory_order_relaxed));
    Log(L"Early DLL notification registered: %d",
        gDllNotificationRegistered.load(std::memory_order_acquire));
    const bool liveHookInstalled = InstallFeatureFunctionHook();
    gLiveHookInstalled.store(liveHookInstalled, std::memory_order_release);
    Log(L"Streamline feature-function interception installed: %d", liveHookInstalled);
    Log(L"Early D3D12 creation interception installed: create=%d upgrade=%d",
        gD3D12CreateDeviceHookInstalled.load(std::memory_order_acquire),
        gSlUpgradeInterfaceHookInstalled.load(std::memory_order_acquire));
    Log(L"Streamline D3D12 device interception installed: %d",
        gD3DDeviceHookInstalled.load(std::memory_order_acquire));
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

    // CET writes config.json when the user changes the mode. Watch it off
    // the presenting thread and atomically publish changes for the SetOptions hook.
    uint32_t heartbeatTicks = 0;
    bool previousReady = BridgeReady();
    std::string previousRoute = PatchRouteName();
    for (;;)
    {
        Sleep(100);
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
                activeControl = control;
                StoreControl(activeControl);
                PublishLiveBridge(activeControl);
                WriteBridgeStatus(activeControl, pid);
                Log(L"Live control requested: mode=%s multiplier=%ux dynamicTarget=%u FPS "
                    L"dynamicExperimental56=%d generatedOnlyDebug=%d",
                    activeControl.dynamic ? L"dynamic" : L"fixed", activeControl.multiplier,
                    activeControl.dynamicTargetFrameRate,
                    activeControl.dynamicExperimental56,
                    activeControl.generatedOnlyDebug);
            }
        }

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
MfgUnlockRegisterD3D12Queue(ID3D12CommandQueue* queue)
{
    return ngx_output_probe::RegisterQueue(queue) ? TRUE : FALSE;
}

extern "C" __declspec(dllexport) BOOL WINAPI
MfgUnlockRegisterD3D12Device(ID3D12Device* device)
{
    return ngx_output_probe::RegisterDevice(device) ? TRUE : FALSE;
}

extern "C" __declspec(dllexport) BOOL WINAPI
MfgUnlockRegisterD3D12Swapchain(
    IDXGISwapChain* swapchain, IUnknown* presentationQueue)
{
    return present_probe::RegisterSwapchain(swapchain, presentationQueue)
        ? TRUE : FALSE;
}

extern "C" __declspec(dllexport) BOOL WINAPI
MfgUnlockUnregisterD3D12Swapchain(IDXGISwapChain* swapchain)
{
    return present_probe::UnregisterSwapchain(swapchain) ? TRUE : FALSE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);
        // This must be available before DLL-load notifications inspect NGX.
        gExperimentalSm120Target.store(
            ReadEnvironmentFlag(L"RTX40_MFG_EXPERIMENTAL_SM120_TARGET"),
            std::memory_order_release);
        gExperimentalDl4rtSm120Path.store(
            ReadEnvironmentFlag(L"RTX40_MFG_EXPERIMENTAL_DL4RT_SM120_PATH"),
            std::memory_order_release);
        gExperimentalTemporalPreEmphasis.store(
            ReadEnvironmentFlag(L"RTX40_MFG_EXPERIMENTAL_TEMPORAL_PREEMPHASIS"),
            std::memory_order_release);
        wchar_t executablePath[32768]{};
        GetModuleFileNameW(nullptr, executablePath, _countof(executablePath));
        gExecutableDirectory = ParentPath(executablePath);
        InstallEarlyD3D12Hooks();
        InstallD3DDeviceHook();
        gLiveHookInstalled.store(InstallFeatureFunctionHook(), std::memory_order_release);
        InstallUiTagHooks();
        // The official Streamline sample recreates its swapchain through the
        // interposer.  Do not alter that path for parameter-only control runs;
        // opt the sample into presentation probing explicitly.
        if (!ReadEnvironmentFlag(L"RTX40_MFG_SAMPLE_PROBE")
            && ReadEnvironmentFlag(L"RTX40_MFG_PRESENT_PROBE"))
        {
            present_probe::Install(GetModuleHandleW(nullptr), &ProbeLog);
        }
        RegisterDllNotification();
        HANDLE thread = CreateThread(nullptr, 0, PatchWorker, instance, 0, nullptr);
        if (thread)
            CloseHandle(thread);
    }
    return TRUE;
}
