#include <Windows.h>
#include <sl.h>
#include <sl_dlss_g.h>

#include <cstdio>
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
    bool dynamicExperimental56 = false)
{
    std::ofstream file(kConfigPath, std::ios::binary | std::ios::trunc);
    if (!file)
        return false;
    file << "{\"mode\":\"" << mode << "\",\"multiplier\":" << multiplier
         << ",\"dynamicTargetFrameRate\":" << target
         << ",\"dynamicExperimental56\":"
         << (dynamicExperimental56 ? "true" : "false")
         << ",\"version\":6}\n";
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
}

int wmain()
{
    DWORD ignored = 0;
    const DWORD versionSize = GetFileVersionInfoSizeW(L"AsiLiveControlHarness.exe", &ignored);
    wprintf_s(L"VERSION proxy import active; metadata size=%lu\n", versionSize);
    if (!WriteControl("fixed", 2, 0))
    {
        fputs("could not initialize config\n", stderr);
        return 10;
    }

    Sleep(2000);
    const bool transientNotInitialized =
        EnvironmentEnabled(L"MFG_HARNESS_TRANSIENT_21");
    if (transientNotInitialized && !LoadLibraryW(NgxPath().c_str()))
    {
        fputs("could not preload NGX stub\n", stderr);
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
    auto* uiRecompositionEnabled =
        reinterpret_cast<FakeUiRecompositionEnabledFn*>(GetProcAddress(
            wrapperModule, "FakeUiRecompositionEnabled"));
    if (!uiRecompositionEnabled)
        return 36;
    const sl::ViewportHandle viewport{0u};
    sl::DLSSGOptions options{};
    options.structVersion = sl::kStructVersion3;
    options.mode = sl::DLSSGMode::eOn;
    options.numFramesToGenerate = 1;
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
    if (initialState.numFramesActuallyPresented != 2)
        return 14;

    if (!WriteControl("fixed", 4, 0))
        return 15;
    Sleep(400);

    // Cyberpunk does not call SetOptions again here. The ASI must notice the
    // pending request from this render-thread GetState call and reapply it.
    sl::DLSSGState updatedState{};
    if (getState(viewport, updatedState, &options) != sl::Result::eOk)
        return 16;
    printf_s("after GetState-only switch actual=%u max=%u\n",
        updatedState.numFramesActuallyPresented,
        updatedState.numFramesToGenerateMax);
    if (updatedState.numFramesActuallyPresented != 4
        || updatedState.numFramesToGenerateMax != 5)
        return 17;

    Sleep(1200);
    const std::string status = ReadStatus();
    puts(status.c_str());
    const std::string expectedLiveReapplyCount = transientNotInitialized
        ? "\"liveReapplyCount\":2" : "\"liveReapplyCount\":1";
    const std::string expectedRetryCount = transientNotInitialized
        ? "\"notInitializedRetryCount\":1"
        : "\"notInitializedRetryCount\":0";
    if (status.find("\"actualFramesPresented\":4") == std::string::npos
        || status.find("\"pending\":false") == std::string::npos
        || status.find(expectedLiveReapplyCount) == std::string::npos
        || status.find(expectedRetryCount) == std::string::npos)
        return 18;

    if (!WriteControl("fixed", 5, 0))
        return 25;
    Sleep(400);
    sl::DLSSGState fiveXState{};
    if (getState(viewport, fiveXState, &options) != sl::Result::eOk)
        return 26;
    printf_s("experimental 5x actual=%u max-generated=%u\n",
        fiveXState.numFramesActuallyPresented,
        fiveXState.numFramesToGenerateMax);
    if (fiveXState.numFramesActuallyPresented != 5
        || fiveXState.numFramesToGenerateMax != 5)
        return 27;

    if (!WriteControl("fixed", 6, 0))
        return 28;
    Sleep(400);
    sl::DLSSGState sixXState{};
    if (getState(viewport, sixXState, &options) != sl::Result::eOk)
        return 29;
    printf_s("experimental 6x actual=%u max-generated=%u\n",
        sixXState.numFramesActuallyPresented,
        sixXState.numFramesToGenerateMax);
    if (sixXState.numFramesActuallyPresented != 6
        || sixXState.numFramesToGenerateMax != 5)
        return 30;
    Sleep(1200);
    const std::string experimentalStatus = ReadStatus();
    puts(experimentalStatus.c_str());
    if (experimentalStatus.find("\"appliedMultiplier\":6") == std::string::npos
        || experimentalStatus.find("\"actualFramesPresented\":6") == std::string::npos
        || experimentalStatus.find("\"setOptionsResult\":0") == std::string::npos
        || experimentalStatus.find("\"pending\":false") == std::string::npos)
        return 31;

    if (!WriteControl("dynamic", 4, 120))
        return 19;
    Sleep(400);
    sl::DLSSGState dynamicState{};
    if (getState(viewport, dynamicState, &options) != sl::Result::eOk
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
    if (getState(viewport, experimentalDynamicState, &options) != sl::Result::eOk
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
    if (getState(viewport, warningState, &options) != sl::Result::eOk
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
