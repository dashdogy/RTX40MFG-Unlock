#include <Windows.h>
#include <sl.h>
#include <sl_dlss_g.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
constexpr wchar_t kConfigPath[] =
    L"plugins\\cyber_engine_tweaks\\mods\\RTX40MFG\\config.json";
constexpr wchar_t kStatusPath[] =
    L"plugins\\cyber_engine_tweaks\\mods\\RTX40MFG\\bridge_status.json";

bool WriteControl(const char* mode, uint32_t multiplier, uint32_t target,
    bool dynamicExperimental56 = false, bool generatedOnlyDebug = false)
{
    std::ofstream file(kConfigPath, std::ios::binary | std::ios::trunc);
    if (!file)
        return false;
    file << "{\"mode\":\"" << mode << "\",\"multiplier\":" << multiplier
         << ",\"dynamicTargetFrameRate\":" << target
         << ",\"dynamicExperimental56\":"
         << (dynamicExperimental56 ? "true" : "false")
         << ",\"generatedOnlyDebug\":"
         << (generatedOnlyDebug ? "true" : "false")
         << ",\"version\":7}\n";
    return file.good();
}

std::string ReadStatus()
{
    std::ifstream file(kStatusPath, std::ios::binary);
    std::ostringstream data;
    data << file.rdbuf();
    return data.str();
}

bool ReadJsonUnsigned(const std::string& json, const char* name, uint32_t& value)
{
    const std::string key = std::string("\"") + name + "\":";
    size_t offset = json.find(key);
    if (offset == std::string::npos)
        return false;
    offset += key.size();
    if (offset >= json.size() || json[offset] < '0' || json[offset] > '9')
        return false;
    uint64_t parsed = 0;
    while (offset < json.size() && json[offset] >= '0' && json[offset] <= '9')
    {
        parsed = parsed * 10 + static_cast<uint32_t>(json[offset] - '0');
        if (parsed > UINT32_MAX)
            return false;
        ++offset;
    }
    value = static_cast<uint32_t>(parsed);
    return true;
}

bool EnvironmentEnabled(const wchar_t* name)
{
    wchar_t value[8]{};
    const DWORD length = GetEnvironmentVariableW(name, value, _countof(value));
    return length > 0 && length < _countof(value) && value[0] != L'0';
}

std::wstring NgxPath()
{
    wchar_t path[32768]{};
    const DWORD length = GetEnvironmentVariableW(
        L"MFG_HARNESS_NGX_PATH", path, _countof(path));
    return length > 0 && length < _countof(path)
        ? std::wstring(path, length) : L"nvngx_dlssg.dll";
}

std::wstring WrapperPath()
{
    wchar_t path[32768]{};
    const DWORD length = GetEnvironmentVariableW(
        L"MFG_HARNESS_WRAPPER_PATH", path, _countof(path));
    return length > 0 && length < _countof(path)
        ? std::wstring(path, length) : L"sl.dlss_g.dll";
}

std::wstring AsiPath()
{
    wchar_t path[32768]{};
    const DWORD length = GetEnvironmentVariableW(
        L"MFG_HARNESS_ASI_PATH", path, _countof(path));
    return length > 0 && length < _countof(path)
        ? std::wstring(path, length) : L"RTX40MFG.asi";
}
}

int wmain()
{
    DWORD ignored = 0;
    const DWORD versionSize = GetFileVersionInfoSizeW(L"AsiLiveControlHarness.exe", &ignored);
    wprintf_s(L"VERSION proxy import active; metadata size=%lu\n", versionSize);
    std::error_code directoryError;
    std::filesystem::create_directories(
        L"plugins\\cyber_engine_tweaks\\mods\\RTX40MFG", directoryError);
    if (directoryError)
    {
        fputs("could not initialize config directory\n", stderr);
        return 9;
    }
    // The production policy must apply the >2x shape on the first clean enable,
    // before Streamline creates its presentation swapchain and frame pool.
    if (!WriteControl("fixed", 6, 0))
    {
        fputs("could not initialize config\n", stderr);
        return 10;
    }

    const std::wstring asiPath = AsiPath();
    if (!LoadLibraryW(asiPath.c_str()))
    {
        fwprintf_s(stderr, L"could not load ASI (%lu): %s\n",
            GetLastError(), asiPath.c_str());
        return 11;
    }

    Sleep(2000);
    const bool transientNotInitialized =
        EnvironmentEnabled(L"MFG_HARNESS_TRANSIENT_21");
    const bool expectSynthesisFallback =
        EnvironmentEnabled(L"MFG_HARNESS_EXPECT_SYNTHESIS_FALLBACK");
    const bool expectMissedCleanEnable =
        EnvironmentEnabled(L"MFG_HARNESS_EXPECT_MISSED_CLEAN_ENABLE");
    // A clean-start >2x apply requires the loaded NGX backend to be identified
    // before the host's first accepted DLSS-G On call.
    if (!expectMissedCleanEnable && !LoadLibraryW(NgxPath().c_str()))
    {
        fputs("could not preload NGX stub for clean enable\n", stderr);
        return 11;
    }

    void* setAddress = nullptr;
    void* getAddress = nullptr;
    const sl::Result setLookup = slGetFeatureFunction(
        sl::kFeatureDLSS_G, "slDLSSGSetOptions", setAddress);
    const sl::Result getLookup = slGetFeatureFunction(
        sl::kFeatureDLSS_G, "slDLSSGGetState", getAddress);
    if (setLookup != sl::Result::eOk || getLookup != sl::Result::eOk
        || !setAddress || !getAddress)
    {
        printf_s("function lookup failed: set=%d get=%d\n",
            static_cast<int>(setLookup), static_cast<int>(getLookup));
        return 11;
    }

    auto* setOptions = reinterpret_cast<PFun_slDLSSGSetOptions*>(setAddress);
    auto* getState = reinterpret_cast<PFun_slDLSSGGetState*>(getAddress);
    const std::wstring wrapperPath = WrapperPath();
    HMODULE wrapperModule = GetModuleHandleW(wrapperPath.c_str());
    if (!wrapperModule)
        wrapperModule = LoadLibraryW(wrapperPath.c_str());
    if (!wrapperModule)
        return 35;
    using FakeUiRecompositionEnabledFn = uint32_t();
    using FakeOptionsFlagsFn = uint32_t();
    using FakeCounterFn = uint32_t();
    using FakeRunTemporalSequenceFn = uint32_t(uint32_t);
    using FakeRunInterleavedTemporalSequencesFn = uint32_t();
    auto* uiRecompositionEnabled =
        reinterpret_cast<FakeUiRecompositionEnabledFn*>(GetProcAddress(
            wrapperModule, "FakeUiRecompositionEnabled"));
    if (!uiRecompositionEnabled)
        return 36;
    auto* optionsFlags = reinterpret_cast<FakeOptionsFlagsFn*>(GetProcAddress(
        wrapperModule, "FakeOptionsFlags"));
    if (!optionsFlags)
        return 47;
    auto* actualMultiplier = reinterpret_cast<FakeCounterFn*>(GetProcAddress(
        wrapperModule, "FakeActualMultiplier"));
    if (!actualMultiplier)
        return 55;
    auto* offSetCalls = reinterpret_cast<FakeCounterFn*>(GetProcAddress(
        wrapperModule, "FakeOffSetCalls"));
    auto* recycleOrderViolations = reinterpret_cast<FakeCounterFn*>(GetProcAddress(
        wrapperModule, "FakeRecycleOrderViolations"));
    HMODULE interposerModule = GetModuleHandleW(L"sl.interposer.dll");
    auto* freeResourcesCalls = interposerModule
        ? reinterpret_cast<FakeCounterFn*>(GetProcAddress(
            interposerModule, "FakeFreeResourcesCalls"))
        : nullptr;
    auto* freeResourcesOrderViolations = interposerModule
        ? reinterpret_cast<FakeCounterFn*>(GetProcAddress(
            interposerModule, "FakeFreeResourcesOrderViolations"))
        : nullptr;
    if (!offSetCalls || !recycleOrderViolations || !freeResourcesCalls
        || !freeResourcesOrderViolations)
        return 52;
    auto* runTemporalSequence =
        reinterpret_cast<FakeRunTemporalSequenceFn*>(GetProcAddress(
            wrapperModule, "FakeRunTemporalSequence"));
    auto* runInterleavedTemporalSequences =
        reinterpret_cast<FakeRunInterleavedTemporalSequencesFn*>(GetProcAddress(
            wrapperModule, "FakeRunInterleavedTemporalSequences"));
    if (!runTemporalSequence || !runInterleavedTemporalSequences)
        return 48;
    const sl::ViewportHandle viewport{0u};
    sl::DLSSGOptions options{};
    options.structVersion = sl::kStructVersion3;
    options.mode = sl::DLSSGMode::eOn;
    options.numFramesToGenerate = 1;
    const auto readSettledState = [&](uint32_t expected,
        sl::DLSSGState& state) {
        sl::Result result = sl::Result::eErrorNotInitialized;
        for (uint32_t attempt = 0; attempt < 12; ++attempt)
        {
            state = {};
            result = getState(viewport, state, &options);
            if (result != sl::Result::eOk
                || state.numFramesActuallyPresented == expected)
                return result;
            Sleep(25);
        }
        return result;
    };
    const auto applyAfterHostLifecycle = [&](uint32_t expected,
        sl::DLSSGState& state) {
        sl::DLSSGOptions off = options;
        off.mode = sl::DLSSGMode::eOff;
        if (setOptions(viewport, off) != sl::Result::eOk
            || actualMultiplier() != 0)
            return false;
        Sleep(25);
        if (setOptions(viewport, options) != sl::Result::eOk)
            return false;
        return readSettledState(expected, state) == sl::Result::eOk
            && state.numFramesActuallyPresented == expected;
    };
    const sl::Result initialSetResult = setOptions(viewport, options);
    if ((!transientNotInitialized && initialSetResult != sl::Result::eOk)
        || (transientNotInitialized
            && initialSetResult != sl::Result::eErrorNotInitialized))
        return 12;
    if (transientNotInitialized)
    {
        sl::DLSSGState cooldownState{};
        if (getState(viewport, cooldownState, &options) != sl::Result::eOk)
            return 13;
        printf_s("before retry cooldown actual=%u\n",
            cooldownState.numFramesActuallyPresented);
        if (cooldownState.numFramesActuallyPresented != 1)
            return 14;
        Sleep(700);
    }

    sl::DLSSGState initialState{};
    if (getState(viewport, initialState, &options) != sl::Result::eOk)
        return 13;
    printf_s("initial actual=%u\n", initialState.numFramesActuallyPresented);
    const uint32_t expectedInitialFrames =
        (expectSynthesisFallback || expectMissedCleanEnable) ? 2u : 6u;
    if (initialState.numFramesActuallyPresented != expectedInitialFrames)
        return 14;

    if (expectMissedCleanEnable)
    {
        Sleep(1200);
        const std::string missedStatus = ReadStatus();
        puts(missedStatus.c_str());
        if (missedStatus.find("\"version\":23") == std::string::npos
            || missedStatus.find("\"multiplier\":6") == std::string::npos
            || missedStatus.find("\"appliedMultiplier\":0") == std::string::npos
            || missedStatus.find("\"actualFramesPresented\":2") == std::string::npos
            || missedStatus.find("\"pending\":true") == std::string::npos
            || missedStatus.find("\"streamlineRebuildRequired\":true") == std::string::npos
            || missedStatus.find("\"cleanEnableApplyCount\":0") == std::string::npos
            || missedStatus.find("\"missedCleanEnableCount\":1") == std::string::npos)
            return 59;
        sl::DLSSGState recoveredState{};
        if (!applyAfterHostLifecycle(6, recoveredState))
            return 60;
        Sleep(1200);
        const std::string recoveredStatus = ReadStatus();
        if (recoveredStatus.find("\"appliedMultiplier\":6") == std::string::npos
            || recoveredStatus.find("\"pending\":false") == std::string::npos
            || recoveredStatus.find("\"streamlineRebuildRequired\":false") == std::string::npos
            || recoveredStatus.find("\"cleanEnableApplyCount\":1") == std::string::npos
            || recoveredStatus.find("\"hostLifecycleResetCount\":1") == std::string::npos)
            return 61;
        puts("MISSED_CLEAN_ENABLE_DEFER_AND_RECOVER_HARNESS_OK");
        return 0;
    }

    if (!WriteControl("fixed", 4, 0, false, true))
        return 15;
    Sleep(400);

    // A live shape update must not mutate Streamline's already-created
    // presentation swapchain. GetState should discover and defer the request,
    // leaving the cold-start shape active until the host performs Off -> On.
    const uint32_t expectedUpdatedFrames = expectedInitialFrames;
    sl::DLSSGState updatedState{};
    if (readSettledState(expectedUpdatedFrames, updatedState) != sl::Result::eOk)
        return 16;
    printf_s("after deferred GetState-only switch actual=%u max=%u\n",
        updatedState.numFramesActuallyPresented,
        updatedState.numFramesToGenerateMax);
    if (updatedState.numFramesActuallyPresented != expectedUpdatedFrames
        || updatedState.numFramesToGenerateMax != 5)
        return 17;

    Sleep(1200);
    const std::string deferredStatus = ReadStatus();
    puts(deferredStatus.c_str());
    if (expectSynthesisFallback)
    {
        if (deferredStatus.find("\"perSampleSynthesisReady\":false") == std::string::npos
            || deferredStatus.find("\"synthesisFallbackActive\":true") == std::string::npos
            || deferredStatus.find("\"multiplier\":4") == std::string::npos
            || deferredStatus.find("\"appliedMultiplier\":2") == std::string::npos
            || deferredStatus.find("\"actualFramesPresented\":2") == std::string::npos
            || deferredStatus.find("\"forcedMaximumMultiplier\":2") == std::string::npos
            || deferredStatus.find("\"streamlineRebuildRequired\":false") == std::string::npos)
            return 49;
        puts("LIVE_CONTROL_SYNTHESIS_FALLBACK_HARNESS_OK");
        return 0;
    }
    if (deferredStatus.find("\"actualFramesPresented\":6") == std::string::npos
        || deferredStatus.find("\"appliedMultiplier\":6") == std::string::npos
        || deferredStatus.find("\"pending\":true") == std::string::npos
        || deferredStatus.find("\"version\":23") == std::string::npos
        || deferredStatus.find("\"streamlineLifecyclePolicy\":\"clean-enable\"") == std::string::npos
        || deferredStatus.find("\"streamlineRebuildRequired\":true") == std::string::npos
        || deferredStatus.find("\"cleanEnableApplyCount\":1") == std::string::npos
        || deferredStatus.find("\"hostLifecycleResetCount\":0") == std::string::npos
        || deferredStatus.find("\"featureRecycleCount\":0") == std::string::npos
        || deferredStatus.find("\"ngxImmutableOutputsEnabled\":false") == std::string::npos)
        return 18;

    sl::DLSSGState fourXState{};
    if (!applyAfterHostLifecycle(4, fourXState)
        || fourXState.numFramesToGenerateMax != 5
        || (optionsFlags() & static_cast<uint32_t>(
            sl::DLSSGFlags::eShowOnlyInterpolatedFrame)) == 0)
        return 18;
    Sleep(1200);
    const std::string status = ReadStatus();
    puts(status.c_str());
    const std::string expectedLiveReapplyCount = transientNotInitialized
        ? "\"liveReapplyCount\":1" : "\"liveReapplyCount\":0";
    const std::string expectedRetryCount = transientNotInitialized
        ? "\"notInitializedRetryCount\":1"
        : "\"notInitializedRetryCount\":0";
    if (status.find("\"actualFramesPresented\":4") == std::string::npos
        || status.find("\"pending\":false") == std::string::npos
        || status.find("\"version\":23") == std::string::npos
        || status.find("\"streamlineRebuildRequired\":false") == std::string::npos
        || status.find("\"cleanEnableApplyCount\":2") == std::string::npos
        || status.find("\"hostLifecycleResetCount\":1") == std::string::npos
        || status.find("\"featureRecycleCount\":0") == std::string::npos
        || status.find("\"featureRecycleFreeCalls\":0") == std::string::npos
        || status.find("\"featureRecycleActive\":false") == std::string::npos
        || status.find("\"generatedOnlyDebug\":true") == std::string::npos
        || status.find("\"appliedGeneratedOnlyDebug\":true") == std::string::npos
        || status.find(expectedLiveReapplyCount) == std::string::npos
        || status.find(expectedRetryCount) == std::string::npos)
        return 18;

    if (!WriteControl("fixed", 5, 0))
        return 25;
    Sleep(400);
    // Repeated host On calls are not a creation boundary; they must preserve
    // the old 4x shape instead of silently rebuilding only the NGX feature.
    if (setOptions(viewport, options) != sl::Result::eOk
        || actualMultiplier() != 4)
        return 57;
    Sleep(1100);
    const std::string setOptionsOnlyStatus = ReadStatus();
    if (setOptionsOnlyStatus.find("\"streamlineRebuildRequired\":true")
            == std::string::npos
        || setOptionsOnlyStatus.find("\"appliedMultiplier\":4")
            == std::string::npos)
        return 58;
    sl::DLSSGState fiveXState{};
    if (!applyAfterHostLifecycle(5, fiveXState))
        return 26;
    printf_s("clean-enable 5x actual=%u max-generated=%u\n",
        fiveXState.numFramesActuallyPresented,
        fiveXState.numFramesToGenerateMax);
    if (fiveXState.numFramesActuallyPresented != 5
        || fiveXState.numFramesToGenerateMax != 5
        || (optionsFlags() & static_cast<uint32_t>(
            sl::DLSSGFlags::eShowOnlyInterpolatedFrame)) != 0)
        return 27;

    if (!WriteControl("fixed", 6, 0))
        return 28;
    Sleep(400);
    sl::DLSSGState sixXState{};
    if (!applyAfterHostLifecycle(6, sixXState))
        return 29;
    printf_s("experimental 6x actual=%u max-generated=%u\n",
        sixXState.numFramesActuallyPresented,
        sixXState.numFramesToGenerateMax);
    if (sixXState.numFramesActuallyPresented != 6
        || sixXState.numFramesToGenerateMax != 5)
        return 30;

    HMODULE ngxModule = GetModuleHandleW(NgxPath().c_str());
    if (!ngxModule)
        ngxModule = LoadLibraryW(NgxPath().c_str());
    using FakeResetTemporalSequenceFn = void();
    using FakeTemporalSequenceValidFn = uint32_t();
    using FakeTemporalCallsFn = uint32_t();
    auto* resetTemporalSequence = reinterpret_cast<FakeResetTemporalSequenceFn*>(
        GetProcAddress(ngxModule, "FakeResetTemporalSequence"));
    auto* temporalSequenceValid = reinterpret_cast<FakeTemporalSequenceValidFn*>(
        GetProcAddress(ngxModule, "FakeTemporalSequenceValid"));
    auto* temporalCalls = reinterpret_cast<FakeTemporalCallsFn*>(
        GetProcAddress(ngxModule, "FakeTemporalCalls"));
    if (!resetTemporalSequence || !temporalSequenceValid || !temporalCalls)
        return 49;
    resetTemporalSequence();
    if (runTemporalSequence(5) != 5
        || temporalCalls() != 5 || temporalSequenceValid() != 1)
        return 50;
    if (runInterleavedTemporalSequences() != 10
        || temporalCalls() != 15 || temporalSequenceValid() != 1)
        return 51;
    if (runInterleavedTemporalSequences() != 10
        || temporalCalls() != 25 || temporalSequenceValid() != 1)
        return 54;
    Sleep(1200);
    const std::string experimentalStatus = ReadStatus();
    puts(experimentalStatus.c_str());
    if (experimentalStatus.find("\"appliedMultiplier\":6") == std::string::npos
        || experimentalStatus.find("\"actualFramesPresented\":6") == std::string::npos
        || experimentalStatus.find("\"ngxEvaluateSeen\":true") == std::string::npos
        || experimentalStatus.find("\"ngxTemporalValidCount\":25") == std::string::npos
        || experimentalStatus.find("\"ngxTemporalInvalidCount\":0") == std::string::npos
        || experimentalStatus.find("\"ngxSeenCountMask\":32") == std::string::npos
        || experimentalStatus.find("\"ngxSeenIndexMask\":31") == std::string::npos
        || experimentalStatus.find("\"ngxOutputCompleteBatches\":5") == std::string::npos
        || experimentalStatus.find("\"ngxOutputAliasedBatches\":0") == std::string::npos
        || experimentalStatus.find("\"ngxFullStateRepairActive\":true") == std::string::npos
        || experimentalStatus.find("\"ngxFullStateForcedCount\":25") == std::string::npos
        || experimentalStatus.find("\"ngxDisableInterpolationGetResult\":1") == std::string::npos
        || experimentalStatus.find("\"ngxDisableInterpolationResource\":\"0x3000\"") == std::string::npos
        || experimentalStatus.find("\"ngxDisableInterpolationResourcePreservedCount\":25") == std::string::npos
        || experimentalStatus.find("\"ngxResetGetResult\":1") == std::string::npos
        || experimentalStatus.find("\"ngxRawReset\":0") == std::string::npos
        || experimentalStatus.find("\"ngxResetPreservedCount\":25") == std::string::npos
        || experimentalStatus.find("\"ngxLastOutputUniqueCount\":5") == std::string::npos
        || experimentalStatus.find("\"setOptionsResult\":0") == std::string::npos
        || experimentalStatus.find("\"pending\":false") == std::string::npos)
        return 31;

    if (!WriteControl("dynamic", 4, 120))
        return 19;
    Sleep(400);
    sl::DLSSGState dynamicState{};
    if (!applyAfterHostLifecycle(4, dynamicState)
        || dynamicState.numFramesActuallyPresented != 4
        || dynamicState.numFramesToGenerateMax != 3)
        return 20;
    Sleep(1200);
    const std::string dynamicStatus = ReadStatus();
    puts(dynamicStatus.c_str());
    if (dynamicStatus.find("\"mode\":\"dynamic\"") == std::string::npos
        || dynamicStatus.find("\"appliedMode\":\"dynamic\"") == std::string::npos
        || dynamicStatus.find("\"appliedDynamicTargetFrameRate\":120") == std::string::npos
        || dynamicStatus.find("\"dynamicExperimental56\":false") == std::string::npos
        || dynamicStatus.find("\"forcedMaximumMultiplier\":4") == std::string::npos
        || dynamicStatus.find("\"setOptionsResult\":0") == std::string::npos)
        return 21;

    if (!WriteControl("dynamic", 4, 240, true))
        return 32;
    Sleep(400);
    sl::DLSSGState experimentalDynamicState{};
    if (!applyAfterHostLifecycle(6, experimentalDynamicState)
        || experimentalDynamicState.numFramesActuallyPresented != 6
        || experimentalDynamicState.numFramesToGenerateMax != 5)
        return 33;
    Sleep(1200);
    const std::string experimentalDynamicStatus = ReadStatus();
    puts(experimentalDynamicStatus.c_str());
    if (experimentalDynamicStatus.find("\"dynamicExperimental56\":true") == std::string::npos
        || experimentalDynamicStatus.find("\"appliedDynamicExperimental56\":true") == std::string::npos
        || experimentalDynamicStatus.find("\"forcedMaximumMultiplier\":6") == std::string::npos
        || experimentalDynamicStatus.find("\"actualFramesPresented\":6") == std::string::npos)
        return 34;

    if (!WriteControl("fixed", 3, 0))
        return 22;
    Sleep(400);
    sl::DLSSGState warningState{};
    if (!applyAfterHostLifecycle(3, warningState)
        || warningState.numFramesActuallyPresented != 3)
        return 23;
    Sleep(1200);
    const std::string warningStatus = ReadStatus();
    puts(warningStatus.c_str());
    if (warningStatus.find("\"appliedMultiplier\":3") == std::string::npos
        || warningStatus.find("\"setOptionsAccepted\":true") == std::string::npos
        || warningStatus.find("\"setOptionsResult\":39") == std::string::npos
        || warningStatus.find("\"pending\":false") == std::string::npos)
        return 24;
    if (offSetCalls() != 6 || freeResourcesCalls() != 0
        || recycleOrderViolations() != 0
        || freeResourcesOrderViolations() != 0
        || warningStatus.find("\"cleanEnableApplyCount\":7") == std::string::npos
        || warningStatus.find("\"hostLifecycleResetCount\":6") == std::string::npos
        || warningStatus.find("\"streamlineRebuildRequired\":false") == std::string::npos
        || warningStatus.find("\"featureRecycleCount\":0") == std::string::npos
        || warningStatus.find("\"featureRecycleFreeCalls\":0") == std::string::npos
        || warningStatus.find("\"featureRecycleExplicitFreeSkipped\":false")
            == std::string::npos)
        return 53;

    sl::Extent fullExtent{};
    fullExtent.width = 1920;
    fullExtent.height = 1080;
    sl::Resource hudless{sl::ResourceType::eTex2d,
        reinterpret_cast<void*>(static_cast<uintptr_t>(0x1000))};
    hudless.width = 1920;
    hudless.height = 1080;
    hudless.nativeFormat = 28;
    sl::Resource uiAlpha{sl::ResourceType::eTex2d,
        reinterpret_cast<void*>(static_cast<uintptr_t>(0x2000))};
    uiAlpha.width = 1920;
    uiAlpha.height = 1080;
    uiAlpha.nativeFormat = 61;
    sl::ResourceTag hudlessTag{&hudless, sl::kBufferTypeHUDLessColor,
        sl::ResourceLifecycle::eValidUntilPresent, &fullExtent};
    sl::ResourceTag uiAlphaTag{&uiAlpha, sl::kBufferTypeUIAlpha,
        sl::ResourceLifecycle::eValidUntilPresent, &fullExtent};
    if (slSetTag(viewport, &hudlessTag, 1, nullptr) != sl::Result::eOk
        || uiRecompositionEnabled() != 0)
        return 37;
    sl::Extent mismatchedExtent{};
    mismatchedExtent.width = 1280;
    mismatchedExtent.height = 720;
    sl::ResourceTag mismatchedUiTag{&uiAlpha, sl::kBufferTypeUIAlpha,
        sl::ResourceLifecycle::eValidUntilPresent, &mismatchedExtent};
    if (slSetTag(viewport, &mismatchedUiTag, 1, nullptr) != sl::Result::eOk
        || uiRecompositionEnabled() != 0)
        return 44;
    if (slSetTag(viewport, &uiAlphaTag, 1, nullptr) != sl::Result::eOk)
        return 38;
    Sleep(100);
    sl::DLSSGState uiEnabledState{};
    if (getState(viewport, uiEnabledState, &options) != sl::Result::eOk
        || uiRecompositionEnabled() != 1)
        return 39;
    Sleep(1200);
    const std::string uiEnabledStatus = ReadStatus();
    puts(uiEnabledStatus.c_str());
    if (uiEnabledStatus.find("\"uiTagHookInstalled\":true") == std::string::npos
        || uiEnabledStatus.find("\"hudlessTagActive\":true") == std::string::npos
        || uiEnabledStatus.find("\"uiAlphaTagActive\":true") == std::string::npos
        || uiEnabledStatus.find("\"uiDimensionsMatch\":true") == std::string::npos
        || uiEnabledStatus.find("\"uiInputsReady\":true") == std::string::npos
        || uiEnabledStatus.find("\"uiRecompositionEnabled\":true") == std::string::npos
        || uiEnabledStatus.find("\"uiRecompositionForced\":true") == std::string::npos)
        return 40;

    sl::ResourceTag clearUiTag{nullptr, sl::kBufferTypeUIAlpha,
        sl::ResourceLifecycle::eValidUntilPresent};
    if (slSetTag(viewport, &clearUiTag, 1, nullptr) != sl::Result::eOk)
        return 41;
    Sleep(100);
    sl::DLSSGState uiDisabledState{};
    if (getState(viewport, uiDisabledState, &options) != sl::Result::eOk
        || uiRecompositionEnabled() != 0)
        return 42;
    Sleep(1200);
    const std::string uiDisabledStatus = ReadStatus();
    puts(uiDisabledStatus.c_str());
    if (uiDisabledStatus.find("\"uiInputsReady\":false") == std::string::npos
        || uiDisabledStatus.find("\"uiRecompositionEnabled\":false") == std::string::npos
        || uiDisabledStatus.find("\"uiRecompositionForced\":false") == std::string::npos)
        return 43;

    for (uint32_t sample = 0; sample < 70; ++sample)
    {
        Sleep(10);
        if (setOptions(viewport, options) != sl::Result::eOk)
            return 45;
    }
    Sleep(1200);
    const std::string fpsStatus = ReadStatus();
    puts(fpsStatus.c_str());
    uint32_t realFpsMilli = 0;
    uint32_t dlssFpsMilli = 0;
    uint32_t fpsAgeMs = UINT32_MAX;
    if (!ReadJsonUnsigned(fpsStatus, "realFpsMilli", realFpsMilli)
        || !ReadJsonUnsigned(fpsStatus, "dlssFpsMilli", dlssFpsMilli)
        || !ReadJsonUnsigned(fpsStatus, "fpsSampleAgeMs", fpsAgeMs)
        || realFpsMilli == 0 || dlssFpsMilli < realFpsMilli * 28u / 10u
        || dlssFpsMilli > realFpsMilli * 32u / 10u || fpsAgeMs > 2000)
        return 46;

    puts("LIVE_CONTROL_DYNAMIC_UI_AND_FPS_TELEMETRY_HARNESS_OK");
    if (transientNotInitialized)
        puts("TRANSIENT_ERROR_21_RETRY_HARNESS_OK");
    return 0;
}
