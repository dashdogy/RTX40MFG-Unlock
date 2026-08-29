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

bool WriteControl(const char* mode, uint32_t multiplier, uint32_t target)
{
    std::ofstream file(kConfigPath, std::ios::binary | std::ios::trunc);
    if (!file)
        return false;
    file << "{\"mode\":\"" << mode << "\",\"multiplier\":" << multiplier
         << ",\"dynamicTargetFrameRate\":" << target << ",\"version\":5}\n";
    return file.good();
}

std::string ReadStatus()
{
    std::ifstream file(kStatusPath, std::ios::binary);
    std::ostringstream data;
    data << file.rdbuf();
    return data.str();
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

    if (!WriteControl("dynamic", 4, 120))
        return 19;
    Sleep(400);
    sl::DLSSGState dynamicState{};
    if (getState(viewport, dynamicState, &options) != sl::Result::eOk
        || dynamicState.numFramesActuallyPresented != 4)
        return 20;
    Sleep(1200);
    const std::string dynamicStatus = ReadStatus();
    puts(dynamicStatus.c_str());
    if (dynamicStatus.find("\"mode\":\"dynamic\"") == std::string::npos
        || dynamicStatus.find("\"appliedMode\":\"dynamic\"") == std::string::npos
        || dynamicStatus.find("\"appliedDynamicTargetFrameRate\":120") == std::string::npos
        || dynamicStatus.find("\"setOptionsResult\":0") == std::string::npos)
        return 21;

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

    puts("LIVE_CONTROL_DYNAMIC_AND_VRAM_WARNING_HARNESS_OK");
    if (transientNotInitialized)
        puts("TRANSIENT_ERROR_21_RETRY_HARNESS_OK");
    return 0;
}
