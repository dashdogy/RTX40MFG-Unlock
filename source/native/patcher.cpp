#include "shared.h"

#include <Windows.h>
#include <TlHelp32.h>
#include <winternl.h>
#include <sl.h>
#include <sl_dlss_g.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <share.h>
#include <string>
#include <vector>

namespace
{
FILE* gLog = nullptr;
std::atomic<uint32_t> gDesiredMultiplier{2};
std::atomic<bool> gDesiredDynamicMode{false};
std::atomic<uint32_t> gDynamicTargetFrameRate{0};
std::atomic<uint64_t> gDesiredRevision{0};
std::atomic<uint64_t> gAppliedRevision{0};
std::atomic<uint64_t> gAttemptedRevision{0};
std::atomic<bool> gControlReady{false};
std::atomic<PFun_slGetFeatureFunction*> gOriginalGetFeatureFunction{nullptr};
std::atomic<PFun_slDLSSGSetOptions*> gOriginalSetOptions{nullptr};
std::atomic<PFun_slDLSSGGetState*> gOriginalGetState{nullptr};
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
std::atomic<uint32_t> gActualFramesPresented{0};
std::atomic<uint32_t> gNumFramesToGenerateMax{0};
std::atomic<uint32_t> gDlssgStatus{0};
std::atomic<bool> gDynamicMfgSupported{false};
std::atomic<uint64_t> gStateSampleTick{0};
std::atomic<uint64_t> gSetOptionsCalls{0};
std::atomic<uint64_t> gGetStateCalls{0};
std::atomic<uint64_t> gLiveReapplyCount{0};
std::atomic<bool> gDllNotificationRegistered{false};
std::atomic<bool> gLiveHookInstalled{false};
std::atomic<uint32_t> gLoadedWrapperCandidates{0};
std::atomic<uint32_t> gPatchedWrapperCandidates{0};
std::atomic<uint32_t> gLoadedNgxCandidates{0};
std::atomic<uint32_t> gPatchedNgxCandidates{0};
std::atomic<uint32_t> gWrapperRouteBits{0};
std::atomic<uint32_t> gNgxRouteBits{0};
std::atomic<bool> gActiveWrapperObserved{false};
std::atomic<bool> gActiveWrapperPatched{false};
std::atomic<uintptr_t> gActiveWrapperBase{0};
std::atomic<bool> gLogReady{false};
std::mutex gStreamlineCallMutex;
std::mutex gLastOptionsMutex;
std::mutex gModuleMutex;
std::wstring gConfigPath;
std::wstring gStatusPath;
std::wstring gExecutableDirectory;

constexpr uint32_t kRouteLocal = 1u;
constexpr uint32_t kRouteExternal = 2u;

struct ControlConfig
{
    uint32_t multiplier = 2;
    bool dynamic = false;
    uint32_t dynamicTargetFrameRate = 0;
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

struct ModuleRecord
{
    HMODULE module = nullptr;
    std::wstring path;
    bool wrapperExport = false;
    bool wrapperCandidate = false;
    bool wrapperPatched = false;
    bool ngxExport = false;
    bool ngxCandidate = false;
    bool ngxPatched = false;
    bool inventoryLogged = false;
};

LastGameOptions gLastGameOptions;
std::vector<ModuleRecord> gModuleRecords;

void ObserveActiveWrapperProvider(void* function);

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

bool TryParseControl(const char* data, size_t size, ControlConfig& control)
{
    if (!data || size == 0)
        return false;

    const std::string content(data, size);
    ControlConfig parsed{};
    if (!TryParseUnsigned(content, "multiplier", 2, 4, parsed.multiplier))
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
    if (length == 1 && value[0] >= L'2' && value[0] <= L'4')
        control.multiplier = static_cast<uint32_t>(value[0] - L'0');

    ControlConfig fileControl{};
    return ReadControlFile(gConfigPath, fileControl) ? fileControl : control;
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
    gDesiredMultiplier.store(control.multiplier, std::memory_order_relaxed);
    gDesiredDynamicMode.store(control.dynamic, std::memory_order_relaxed);
    gDynamicTargetFrameRate.store(control.dynamicTargetFrameRate, std::memory_order_relaxed);
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
    wchar_t multiplier[2]{ static_cast<wchar_t>(L'0' + std::clamp(control.multiplier, 2u, 4u)), L'\0' };
    wchar_t target[16]{};
    swprintf_s(target, L"%u", control.dynamicTargetFrameRate);
    SetEnvironmentVariableW(L"RTX40_MFG_ACTIVE_MULTIPLIER", multiplier);
    SetEnvironmentVariableW(L"RTX40_MFG_ACTIVE_MODE", control.dynamic ? L"dynamic" : L"fixed");
    SetEnvironmentVariableW(L"RTX40_MFG_DYNAMIC_TARGET", target);
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

    char json[3072]{};
    const int length = sprintf_s(json,
        "{\"version\":4,\"pid\":%lu,\"heartbeat\":%llu,\"route\":\"%s\","
        "\"bridgeReady\":%s,\"liveHookInstalled\":%s,"
        "\"activeWrapperObserved\":%s,\"activeWrapperPatched\":%s,"
        "\"loadedWrapperCandidates\":%u,\"patchedWrapperCandidates\":%u,"
        "\"loadedNgxCandidates\":%u,\"patchedNgxCandidates\":%u,"
        "\"mode\":\"%s\",\"multiplier\":%u,\"dynamicTargetFrameRate\":%u,"
        "\"requestRevision\":%llu,\"appliedRevision\":%llu,"
        "\"applied\":%s,\"pending\":%s,\"gameFrameGenerationOn\":%s,"
        "\"appliedMode\":\"%s\",\"appliedMultiplier\":%u,"
        "\"appliedDynamicTargetFrameRate\":%u,\"setOptionsSeen\":%s,"
        "\"setOptionsAccepted\":%s,"
        "\"setOptionsResult\":%d,\"getStateSeen\":%s,\"getStateResult\":%d,"
        "\"actualFramesPresented\":%u,\"numFramesToGenerateMax\":%u,"
        "\"dlssgStatus\":%u,\"dynamicMfgSupported\":%s,"
        "\"stateSampleAgeMs\":%llu,\"setOptionsCalls\":%llu,"
        "\"getStateCalls\":%llu,\"liveReapplyCount\":%llu}\n",
        static_cast<unsigned long>(pid),
        static_cast<unsigned long long>(UnixTimeSeconds()), route,
        bridgeReady ? "true" : "false",
        gLiveHookInstalled.load(std::memory_order_relaxed) ? "true" : "false",
        gActiveWrapperObserved.load(std::memory_order_relaxed) ? "true" : "false",
        gActiveWrapperPatched.load(std::memory_order_relaxed) ? "true" : "false",
        gLoadedWrapperCandidates.load(std::memory_order_relaxed),
        gPatchedWrapperCandidates.load(std::memory_order_relaxed),
        gLoadedNgxCandidates.load(std::memory_order_relaxed),
        gPatchedNgxCandidates.load(std::memory_order_relaxed),
        control.dynamic ? "dynamic" : "fixed", control.multiplier,
        control.dynamicTargetFrameRate,
        static_cast<unsigned long long>(desiredRevision),
        static_cast<unsigned long long>(appliedRevision),
        applied ? "true" : "false", pending ? "true" : "false",
        gameFrameGenerationOn ? "true" : "false",
        gAppliedDynamicMode.load(std::memory_order_relaxed) ? "dynamic" : "fixed",
        gAppliedMultiplier.load(std::memory_order_relaxed),
        gAppliedDynamicTargetFrameRate.load(std::memory_order_relaxed),
        setOptionsSeen ? "true" : "false",
        setOptionsAccepted ? "true" : "false", setOptionsResult,
        getStateSeen ? "true" : "false", getStateResult,
        gActualFramesPresented.load(std::memory_order_relaxed),
        gNumFramesToGenerateMax.load(std::memory_order_relaxed),
        gDlssgStatus.load(std::memory_order_relaxed),
        gDynamicMfgSupported.load(std::memory_order_relaxed) ? "true" : "false",
        static_cast<unsigned long long>(stateAgeMs),
        static_cast<unsigned long long>(gSetOptionsCalls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(gGetStateCalls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(gLiveReapplyCount.load(std::memory_order_relaxed)));
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
    const sl::DLSSGOptions& source, const ControlSnapshot& snapshot, bool preserveNext)
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
            std::clamp(snapshot.control.multiplier, 2u, 4u) - 1;
    }
    return adjusted;
}

void CaptureGameOptions(
    const sl::ViewportHandle& viewport, const sl::DLSSGOptions& options)
{
    std::lock_guard lock(gLastOptionsMutex);
    gLastGameOptions.viewport = viewport;
    gLastGameOptions.options = CopyKnownOptions(options, false);
    gLastGameOptions.valid = true;
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

void RecordAppliedControl(const ControlSnapshot& snapshot, sl::Result result, bool liveReapply)
{
    gSetOptionsSeen.store(true, std::memory_order_release);
    gLastSetOptionsResult.store(static_cast<int32_t>(result), std::memory_order_relaxed);
    gAttemptedRevision.store(snapshot.revision, std::memory_order_release);
    // eWarnOutOfVRAM is emitted after Streamline accepts work when DXGI reports
    // no remaining budget. Keep the raw warning for telemetry, but do not leave
    // a successfully submitted multiplier permanently marked as pending.
    if (result != sl::Result::eOk && result != sl::Result::eWarnOutOfVRAM)
        return;

    const uint64_t previous = gAppliedRevision.load(std::memory_order_acquire);
    gAppliedDynamicMode.store(snapshot.control.dynamic, std::memory_order_relaxed);
    gAppliedMultiplier.store(snapshot.control.multiplier, std::memory_order_relaxed);
    gAppliedDynamicTargetFrameRate.store(
        snapshot.control.dynamicTargetFrameRate, std::memory_order_relaxed);
    gAppliedRevision.store(snapshot.revision, std::memory_order_release);
    if (liveReapply)
        gLiveReapplyCount.fetch_add(1, std::memory_order_relaxed);

    if (previous == snapshot.revision)
        return;
    if (snapshot.control.dynamic)
        Log(L"%s dynamic MFG: target=%u FPS, result=%d",
            liveReapply ? L"Live-reapplied" : L"Applied",
            snapshot.control.dynamicTargetFrameRate, static_cast<int>(result));
    else
        Log(L"%s fixed multiplier: %ux, result=%d",
            liveReapply ? L"Live-reapplied" : L"Applied",
            snapshot.control.multiplier, static_cast<int>(result));
}

sl::Result SubmitAdjustedOptions(
    PFun_slDLSSGSetOptions* original, const sl::ViewportHandle& viewport,
    const sl::DLSSGOptions& source, const ControlSnapshot& snapshot, bool liveReapply)
{
    const sl::DLSSGOptions adjusted =
        BuildAdjustedOptions(source, snapshot, !liveReapply);
    const sl::Result result = original(viewport, adjusted);
    RecordAppliedControl(snapshot, result, liveReapply);
    // Cyberpunk treats every non-zero Result as a hard failure. Result 39 is a
    // warning rather than a rejected options update, so preserve it in the
    // bridge status while returning success to the host.
    return result == sl::Result::eWarnOutOfVRAM ? sl::Result::eOk : result;
}

void ReapplyPendingControl(const sl::ViewportHandle& viewport)
{
    if (!gControlReady.load(std::memory_order_acquire)
        || !gGameFrameGenerationOn.load(std::memory_order_acquire)
        || !BridgeReady())
        return;

    const ControlSnapshot snapshot = ReadControlSnapshot();
    if (snapshot.revision == 0
        || snapshot.revision == gAppliedRevision.load(std::memory_order_acquire)
        || snapshot.revision == gAttemptedRevision.load(std::memory_order_acquire))
        return;

    auto* original = gOriginalSetOptions.load(std::memory_order_acquire);
    sl::DLSSGOptions source{};
    if (!original || !ReadLastGameOptions(viewport, source))
        return;

    gSetOptionsCalls.fetch_add(1, std::memory_order_relaxed);
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
    gSetOptionsCalls.fetch_add(1, std::memory_order_relaxed);

    const bool enabled = options.mode == sl::DLSSGMode::eOn
        || options.mode == sl::DLSSGMode::eAuto
        || options.mode == sl::DLSSGMode::eDynamic;
    gGameFrameGenerationOn.store(enabled, std::memory_order_release);
    if (!enabled)
    {
        gSetOptionsSeen.store(true, std::memory_order_release);
        const sl::Result result = original(viewport, options);
        gLastSetOptionsResult.store(static_cast<int32_t>(result), std::memory_order_relaxed);
        return result;
    }

    CaptureGameOptions(viewport, options);
    if (!gControlReady.load(std::memory_order_acquire))
    {
        const sl::Result result = original(viewport, options);
        gSetOptionsSeen.store(true, std::memory_order_release);
        gLastSetOptionsResult.store(static_cast<int32_t>(result), std::memory_order_relaxed);
        return result;
    }

    if (!BridgeReady())
    {
        const sl::Result result = original(viewport, options);
        gSetOptionsSeen.store(true, std::memory_order_release);
        gLastSetOptionsResult.store(static_cast<int32_t>(result), std::memory_order_relaxed);
        if (result != sl::Result::eOk || !BridgeReady())
            return result;
        Log(L"Active DLSS-G modules became ready during the native options call; applying saved control");
    }

    const ControlSnapshot snapshot = ReadControlSnapshot();
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
    gGetStateCalls.fetch_add(1, std::memory_order_relaxed);
    gGetStateSeen.store(true, std::memory_order_release);
    gLastGetStateResult.store(static_cast<int32_t>(result), std::memory_order_relaxed);
    if (result != sl::Result::eOk)
        return result;

    const uint32_t previous =
        gActualFramesPresented.exchange(state.numFramesActuallyPresented, std::memory_order_relaxed);
    gDlssgStatus.store(static_cast<uint32_t>(state.status), std::memory_order_relaxed);
    if (state.structVersion >= sl::kStructVersion2)
        gNumFramesToGenerateMax.store(state.numFramesToGenerateMax, std::memory_order_relaxed);
    if (state.structVersion >= sl::kStructVersion4)
        gDynamicMfgSupported.store(
            state.bIsDynamicMFGSupported == sl::Boolean::eTrue, std::memory_order_relaxed);
    gStateSampleTick.store(GetTickCount64(), std::memory_order_release);

    if (previous != state.numFramesActuallyPresented)
        Log(L"DLSS-G actual presentation count: %ux (maximum generated frames=%u, status=%u)",
            state.numFramesActuallyPresented,
            gNumFramesToGenerateMax.load(std::memory_order_relaxed),
            static_cast<uint32_t>(state.status));
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

bool HookMainExecutableImport(const char* importedModule, const char* importedFunction)
{
    HMODULE module = GetModuleHandleW(nullptr);
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
            if (current == reinterpret_cast<void*>(&HookSlGetFeatureFunction))
                return true;

            DWORD oldProtection = 0;
            if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &oldProtection))
                return false;
            gOriginalGetFeatureFunction.store(
                reinterpret_cast<PFun_slGetFeatureFunction*>(current),
                std::memory_order_release);
            *slot = reinterpret_cast<void*>(&HookSlGetFeatureFunction);
            DWORD ignoredProtection = 0;
            const BOOL restored = VirtualProtect(
                slot, sizeof(*slot), oldProtection, &ignoredProtection);
            FlushInstructionCache(GetCurrentProcess(), slot, sizeof(*slot));
            return restored != FALSE;
        }
    }
    return false;
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

struct PatternPatchResult
{
    bool candidate = false;
    bool patched = false;
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
        return {true, false};
    }

    uint8_t* address = match + patch.patchOffset;
    if (memcmp(address, patch.replacement, patch.patchSize) == 0)
    {
        Log(L"%s: already patched at RVA 0x%zX: %s", patch.label,
            static_cast<size_t>(address - const_cast<uint8_t*>(base)), path.c_str());
        return {true, true};
    }
    if (memcmp(address, patch.original, patch.patchSize) != 0)
    {
        Log(L"%s: matched context but original bytes differ: %s", patch.label, path.c_str());
        return {true, false};
    }

    DWORD oldProtection = 0;
    if (!VirtualProtect(address, patch.patchSize, PAGE_EXECUTE_READWRITE, &oldProtection))
    {
        Log(L"%s: VirtualProtect failed (%lu): %s", patch.label, GetLastError(), path.c_str());
        return {true, false};
    }
    memcpy(address, patch.replacement, patch.patchSize);
    FlushInstructionCache(GetCurrentProcess(), address, patch.patchSize);
    DWORD ignoredProtection = 0;
    const BOOL restored = VirtualProtect(address, patch.patchSize, oldProtection, &ignoredProtection);
    if (!restored)
    {
        Log(L"%s: protection restore failed (%lu): %s", patch.label, GetLastError(), path.c_str());
        return {true, false};
    }

    Log(L"%s: patched RVA 0x%zX: %s", patch.label,
        static_cast<size_t>(address - const_cast<uint8_t*>(base)), path.c_str());
    return {true, true};
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
    }
    gLoadedWrapperCandidates.store(wrapperCandidates, std::memory_order_release);
    gPatchedWrapperCandidates.store(patchedWrappers, std::memory_order_release);
    gLoadedNgxCandidates.store(ngxCandidates, std::memory_order_release);
    gPatchedNgxCandidates.store(patchedNgx, std::memory_order_release);
    gWrapperRouteBits.store(wrapperRouteBits, std::memory_order_release);
    gNgxRouteBits.store(ngxRouteBits, std::memory_order_release);
}

void LogModuleInventory(const ModuleRecord& record)
{
    if (!record.wrapperExport && !record.ngxExport)
        return;
    Log(L"Loaded module: wrapperExport=%d wrapperCandidate=%d wrapperPatched=%d "
        L"ngxExport=%d ngxCandidate=%d ngxPatched=%d path=%s",
        record.wrapperExport, record.wrapperCandidate, record.wrapperPatched,
        record.ngxExport, record.ngxCandidate, record.ngxPatched,
        record.path.c_str());
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
            if (!record.wrapperExport && !record.ngxExport)
                return record;
            if (record.wrapperExport)
            {
                const PatternPatchResult result =
                    PatchUniqueExecutablePattern(module, path, kWrapperPatch);
                record.wrapperCandidate = result.candidate;
                record.wrapperPatched = result.patched;
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
    Log(L"Initial control: mode=%s multiplier=%ux dynamicTarget=%u FPS; config: %s",
        initialControl.dynamic ? L"dynamic" : L"fixed", initialControl.multiplier,
        initialControl.dynamicTargetFrameRate, gConfigPath.c_str());

    Log(L"Patch worker started for PID %lu", static_cast<unsigned long>(pid));
    Log(L"Early DLL notification registered: %d",
        gDllNotificationRegistered.load(std::memory_order_acquire));
    const bool liveHookInstalled = HookMainExecutableImport(
        "sl.interposer.dll", "slGetFeatureFunction");
    gLiveHookInstalled.store(liveHookInstalled, std::memory_order_release);
    Log(L"Streamline feature-function interception installed: %d", liveHookInstalled);
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
                Log(L"Live control requested: mode=%s multiplier=%ux dynamicTarget=%u FPS",
                    activeControl.dynamic ? L"dynamic" : L"fixed", activeControl.multiplier,
                    activeControl.dynamicTargetFrameRate);
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

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);
        wchar_t executablePath[32768]{};
        GetModuleFileNameW(nullptr, executablePath, _countof(executablePath));
        gExecutableDirectory = ParentPath(executablePath);
        gLiveHookInstalled.store(HookMainExecutableImport(
            "sl.interposer.dll", "slGetFeatureFunction"), std::memory_order_release);
        RegisterDllNotification();
        HANDLE thread = CreateThread(nullptr, 0, PatchWorker, instance, 0, nullptr);
        if (thread)
            CloseHandle(thread);
    }
    return TRUE;
}
