#include "reshade_frontend.h"

#if defined(MFG_UNLOCK_RESHADE_UI_CLIENT)
#include "backend_bridge.h"
#else
#include "reshade_bridge.h"
#endif

#include <imgui.h>
#include <reshade.hpp>

#include <Psapi.h>
#include <d3d12.h>
#include <dxgi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <string>

namespace
{
constexpr const char* kConfigSection = "RTX40MFG";
constexpr const char* kOverlayTitle = "DLSS MFG";
constexpr const char* kReShadeHomeWindow = "###home";

std::atomic<bool> gRegistered{false};
std::atomic<bool> gInitialized{false};
HMODULE gSelf = nullptr;
HMODULE gReShade = nullptr;
bool gDefaultDockInitialized = false;
bool gOverlayVisitedThisFrame = false;
bool gOverlayDockedThisFrame = false;
#if defined(MFG_UNLOCK_RESHADE_UI_CLIENT)
enum class BackendKind : uint32_t
{
    eNone = 0,
    eAbi = 1,
    eNativeFile = 2,
};

HMODULE gBackend = nullptr;
MfgUnlockBackendInterface gBackendInterface{};
std::atomic<bool> gBackendConnected{false};
std::atomic<BackendKind> gBackendKind{BackendKind::eNone};
std::atomic<bool> gControlCompanionBackend{false};
std::atomic<bool> gIntegratedUniversalBackend{false};
SRWLOCK gBackendConnectionLock = SRWLOCK_INIT;
std::wstring gNativeConfigPath;
std::wstring gNativeStatusPath;
std::wstring gCompanionConfigPath;
using MfgUnlockSampleFrameTelemetryFn = BOOL (WINAPI*)();
MfgUnlockSampleFrameTelemetryFn gSampleFrameTelemetry = nullptr;
std::atomic<uintptr_t> gTelemetrySwapchain{0};
std::atomic<uint64_t> gTelemetrySwapchainTick{0};

struct NativeMidpointUiState
{
    bool available = false;
    bool required = false;
    bool ready = false;
    bool publicRouteReady = false;
    BOOL adapterObserved = FALSE;
    BOOL backportEnabled = FALSE;
    BOOL backportApplied = FALSE;
    BOOL readyAtCreate = FALSE;
    BOOL fallbackActive = TRUE;
    uint32_t rawCount = 0;
    uint32_t uniqueOutputs = 0;
};

NativeMidpointUiState gNativeMidpoint{};

bool ConnectBackend() noexcept;
void MergeNativeMidpointStatus(
    MfgUnlockReShadeSnapshot& snapshot) noexcept;
#endif
SRWLOCK gPresentationQueueLock = SRWLOCK_INIT;
ID3D12CommandQueue* gPresentationQueue = nullptr;

int gMultiplier = 2;
bool gFollowGameMode = true;
bool gDynamicMode = false;
int gDynamicTargetFrameRate = 0;
bool gDynamicLockToRefreshRate = true;
int gDynamicCustomTargetFrameRate = 60;
bool gDynamicExperimental56 = false;
bool gGeneratedOnlyDebug = false;
bool gLastApplyAttempted = false;
bool gLastApplyAccepted = false;
bool gLastNativeConfigPersisted = true;

#if !defined(MFG_UNLOCK_RESHADE_UI_CLIENT)
extern "C" BOOL WINAPI MfgUnlockRegisterD3D12Device(
    ID3D12Device* device);
extern "C" BOOL WINAPI MfgUnlockRegisterD3D12Queue(
    ID3D12CommandQueue* queue);
extern "C" BOOL WINAPI MfgUnlockRegisterD3D12Swapchain(
    IDXGISwapChain* swapchain, IUnknown* presentationQueue);
extern "C" BOOL WINAPI MfgUnlockUnregisterD3D12Swapchain(
    IDXGISwapChain* swapchain);
#endif

BOOL BackendGetSnapshot(MfgUnlockReShadeSnapshot* snapshot) noexcept
{
#if defined(MFG_UNLOCK_RESHADE_UI_CLIENT)
    if (!ConnectBackend() || !gBackendInterface.getSnapshot
        || !gBackendInterface.getSnapshot(snapshot))
    {
        return FALSE;
    }
    if (gControlCompanionBackend.load(std::memory_order_acquire))
        MergeNativeMidpointStatus(*snapshot);
    return TRUE;
#else
    return MfgUnlockReShadeGetSnapshot(snapshot);
#endif
}

BOOL BackendApplyControl(uint32_t multiplier, BOOL dynamicMode,
    uint32_t dynamicTargetFrameRate, BOOL dynamicExperimental56,
    BOOL generatedOnlyDebug) noexcept
{
#if defined(MFG_UNLOCK_RESHADE_UI_CLIENT)
    return ConnectBackend() && gBackendInterface.applyControl
        ? gBackendInterface.applyControl(multiplier, dynamicMode,
            dynamicTargetFrameRate, dynamicExperimental56,
            generatedOnlyDebug) : FALSE;
#else
    return MfgUnlockReShadeApplyControl(multiplier, dynamicMode,
        dynamicTargetFrameRate, dynamicExperimental56,
        generatedOnlyDebug);
#endif
}

BOOL BackendRegisterDevice(ID3D12Device* device) noexcept
{
#if defined(MFG_UNLOCK_RESHADE_UI_CLIENT)
    return ConnectBackend() && gBackendInterface.registerD3D12Device
        ? gBackendInterface.registerD3D12Device(device) : FALSE;
#else
    return MfgUnlockRegisterD3D12Device(device);
#endif
}

BOOL BackendRegisterQueue(ID3D12CommandQueue* queue) noexcept
{
#if defined(MFG_UNLOCK_RESHADE_UI_CLIENT)
    return ConnectBackend() && gBackendInterface.registerD3D12Queue
        ? gBackendInterface.registerD3D12Queue(queue) : FALSE;
#else
    return MfgUnlockRegisterD3D12Queue(queue);
#endif
}

BOOL BackendRegisterSwapchain(IDXGISwapChain* swapchain,
    IUnknown* presentationQueue) noexcept
{
#if defined(MFG_UNLOCK_RESHADE_UI_CLIENT)
    return ConnectBackend() && gBackendInterface.registerD3D12Swapchain
        ? gBackendInterface.registerD3D12Swapchain(
            swapchain, presentationQueue) : FALSE;
#else
    return MfgUnlockRegisterD3D12Swapchain(
        swapchain, presentationQueue);
#endif
}

BOOL BackendUnregisterSwapchain(IDXGISwapChain* swapchain) noexcept
{
#if defined(MFG_UNLOCK_RESHADE_UI_CLIENT)
    return ConnectBackend() && gBackendInterface.unregisterD3D12Swapchain
        ? gBackendInterface.unregisterD3D12Swapchain(swapchain) : FALSE;
#else
    return MfgUnlockUnregisterD3D12Swapchain(swapchain);
#endif
}

#if defined(MFG_UNLOCK_RESHADE_UI_CLIENT)
bool BackendUsesNativeFile() noexcept
{
    return gBackendKind.load(std::memory_order_acquire)
        == BackendKind::eNativeFile;
}

bool FindJsonValue(const std::string& content, const char* name,
    size_t& offset) noexcept
{
    const std::string key = std::string("\"") + name + "\"";
    const size_t keyOffset = content.find(key);
    if (keyOffset == std::string::npos)
        return false;
    const size_t colon = content.find(':', keyOffset + key.size());
    if (colon == std::string::npos)
        return false;
    offset = content.find_first_not_of(" \t\r\n", colon + 1);
    return offset != std::string::npos;
}

bool ParseJsonBool(const std::string& content, const char* name,
    BOOL& value) noexcept
{
    size_t offset = 0;
    if (!FindJsonValue(content, name, offset))
        return false;
    if (content.compare(offset, 4, "true") == 0)
    {
        value = TRUE;
        return true;
    }
    if (content.compare(offset, 5, "false") == 0)
    {
        value = FALSE;
        return true;
    }
    return false;
}

template <typename T>
bool ParseJsonInteger(const std::string& content, const char* name,
    T& value) noexcept
{
    size_t offset = 0;
    if (!FindJsonValue(content, name, offset))
        return false;
    char* end = nullptr;
    const long long parsed = std::strtoll(content.c_str() + offset, &end, 10);
    if (!end || end == content.c_str() + offset)
        return false;
    value = static_cast<T>(parsed);
    return true;
}

bool ParseJsonString(const std::string& content, const char* name,
    std::string& value) noexcept
{
    size_t offset = 0;
    if (!FindJsonValue(content, name, offset)
        || content[offset] != '"')
    {
        return false;
    }
    value.clear();
    for (size_t index = offset + 1; index < content.size(); ++index)
    {
        const char current = content[index];
        if (current == '"')
            return true;
        if (current != '\\')
        {
            value.push_back(current);
            continue;
        }
        if (++index >= content.size())
            return false;
        switch (content[index])
        {
        case '"': value.push_back('"'); break;
        case '\\': value.push_back('\\'); break;
        case '/': value.push_back('/'); break;
        case 'b': value.push_back('\b'); break;
        case 'f': value.push_back('\f'); break;
        case 'n': value.push_back('\n'); break;
        case 'r': value.push_back('\r'); break;
        case 't': value.push_back('\t'); break;
        default: return false;
        }
    }
    return false;
}

bool ReadTextFile(const std::wstring& path, std::string& content) noexcept
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    const bool validSize = GetFileSizeEx(file, &size)
        && size.QuadPart > 0 && size.QuadPart <= 1024 * 1024;
    if (!validSize)
    {
        CloseHandle(file);
        return false;
    }
    content.resize(static_cast<size_t>(size.QuadPart));
    DWORD bytesRead = 0;
    const BOOL read = ReadFile(file, content.data(),
        static_cast<DWORD>(content.size()), &bytesRead, nullptr);
    CloseHandle(file);
    if (!read)
        return false;
    content.resize(bytesRead);
    return true;
}

bool ResolveNativeFilePaths() noexcept
{
    std::wstring executablePath(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executablePath.data(),
        static_cast<DWORD>(executablePath.size()));
    if (length == 0 || length >= executablePath.size())
        return false;
    executablePath.resize(length);
    const std::filesystem::path directory =
        std::filesystem::path(executablePath).parent_path();
#if defined(MFG_UNLOCK_V12_UNIVERSAL_UI)
    gNativeConfigPath = (directory
        / L"RTX40MFG-Universal.json").wstring();
    gNativeStatusPath = (directory
        / L"RTX40MFG-Universal.status.json").wstring();
#else
    if (GetModuleHandleW(L"RTX40MFG-Universal.asi"))
    {
        gNativeConfigPath = (directory
            / L"RTX40MFG-Universal.json").wstring();
        gNativeStatusPath = (directory
            / L"RTX40MFG-Universal.status.json").wstring();
    }
    else
    {
        gNativeConfigPath = (directory / L"plugins"
            / L"cyber_engine_tweaks" / L"mods" / L"RTX40MFG"
            / L"config.json").wstring();
        gNativeStatusPath = (directory / L"plugins"
            / L"cyber_engine_tweaks" / L"mods" / L"RTX40MFG"
            / L"bridge_status.json").wstring();
    }
#endif
    return true;
}

bool ResolveCompanionControlPath() noexcept
{
    std::wstring executablePath(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executablePath.data(),
        static_cast<DWORD>(executablePath.size()));
    if (length == 0 || length >= executablePath.size())
        return false;
    executablePath.resize(length);
    gCompanionConfigPath =
        (std::filesystem::path(executablePath).parent_path()
            / L"RTX40MFG-Bridge.json").wstring();
    return true;
}

BOOL PersistCompanionControl() noexcept
{
    if (!gControlCompanionBackend.load(std::memory_order_acquire))
        return TRUE;
    if (gCompanionConfigPath.empty() && !ResolveCompanionControlPath())
        return FALSE;

    char content[320]{};
    const int length = std::snprintf(content, sizeof(content),
        "{\"version\":1,\"followGame\":%s,\"mode\":\"%s\","
        "\"multiplier\":%u,\"dynamicTargetFrameRate\":%u,"
        "\"dynamicExperimental56\":%s,\"generatedOnlyDebug\":%s}\n",
        gFollowGameMode ? "true" : "false",
        gDynamicMode ? "dynamic" : "fixed",
        static_cast<uint32_t>(std::clamp(gMultiplier, 2, 6)),
        static_cast<uint32_t>(
            std::clamp(gDynamicTargetFrameRate, 0, 1000)),
        gDynamicExperimental56 ? "true" : "false",
        gGeneratedOnlyDebug ? "true" : "false");
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(content))
        return FALSE;

    const std::wstring temporary = gCompanionConfigPath + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return FALSE;
    DWORD written = 0;
    const BOOL write = WriteFile(file, content, static_cast<DWORD>(length),
        &written, nullptr);
    const BOOL flushed = write && written == static_cast<DWORD>(length)
        && FlushFileBuffers(file);
    CloseHandle(file);
    if (!flushed || !MoveFileExW(temporary.c_str(),
            gCompanionConfigPath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(temporary.c_str());
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI NativeFileGetSnapshot(MfgUnlockReShadeSnapshot* snapshot)
{
    if (!snapshot || snapshot->structSize < sizeof(*snapshot))
        return FALSE;
    MfgUnlockReShadeSnapshot output{};
    output.followGameMode = TRUE;
    output.desiredMultiplier = static_cast<uint32_t>(
        std::clamp(gMultiplier, 2, 6));
    output.dynamicMode = gDynamicMode ? TRUE : FALSE;
    output.dynamicTargetFrameRate = static_cast<uint32_t>(
        std::clamp(gDynamicTargetFrameRate, 0, 1000));
    output.dynamicExperimental56 = gDynamicExperimental56 ? TRUE : FALSE;
    output.generatedOnlyDebug = gGeneratedOnlyDebug ? TRUE : FALSE;
    output.intervalLoggingEnabled = TRUE;
    output.frontendClients = 1;
    strcpy_s(output.patchRoute, "pending");

    std::string control;
    if (ReadTextFile(gNativeConfigPath, control))
    {
        BOOL followGame = FALSE;
        output.followGameMode = ParseJsonBool(
            control, "followGame", followGame) ? followGame : FALSE;
        ParseJsonInteger(control, "multiplier", output.desiredMultiplier);
        ParseJsonInteger(control, "dynamicTargetFrameRate",
            output.dynamicTargetFrameRate);
        ParseJsonBool(control, "dynamicExperimental56",
            output.dynamicExperimental56);
        ParseJsonBool(control, "generatedOnlyDebug",
            output.generatedOnlyDebug);
        std::string controlMode;
        if (ParseJsonString(control, "mode", controlMode))
            output.dynamicMode = controlMode == "dynamic" ? TRUE : FALSE;
    }

    std::string status;
    if (ReadTextFile(gNativeStatusPath, status))
    {
        ParseJsonBool(status, "followGame", output.followGameMode);
        ParseJsonInteger(status, "version",
            output.statusProtocolVersion);
        ParseJsonBool(status, "bridgeReady", output.bridgeReady);
        ParseJsonBool(status, "loaderCoreImported",
            output.loaderCoreImported);
        ParseJsonBool(status, "mainResolverDiscoveryInstalled",
            output.mainResolverDiscoveryInstalled);
        ParseJsonBool(status, "slInitEntryDetourCurrent",
            output.slInitEntryDetourCurrent);
        ParseJsonBool(status, "slInitIatFallbackInstalled",
            output.slInitIatFallbackInstalled);
        ParseJsonBool(status, "slInitResolverFallbackActive",
            output.slInitResolverFallbackActive);
        ParseJsonBool(status, "slInitControlPathReady",
            output.slInitControlPathReady);
        ParseJsonInteger(status, "slInitCalls", output.slInitCalls);
        ParseJsonInteger(status, "slInitFlagsBefore",
            output.slInitFlagsBefore);
        ParseJsonInteger(status, "slInitFlagsAfter",
            output.slInitFlagsAfter);
        ParseJsonBool(status, "otaPreferencesForced",
            output.otaPreferencesForced);
        ParseJsonBool(status, "otaPreferencesEnabledAtInit",
            output.otaPreferencesEnabledAtInit);
        ParseJsonBool(status, "downloadedStreamlinePluginsEnabledAtInit",
            output.downloadedStreamlinePluginsEnabledAtInit);
        ParseJsonBool(status, "otaProviderPreflightSupported",
            output.otaProviderPreflightSupported);
        ParseJsonBool(status, "otaForceSuppressed",
            output.otaForceSuppressed);
        ParseJsonBool(status, "nvidiaCompatibilityResolved",
            output.nvidiaCompatibilityResolved);
        ParseJsonInteger(status, "nvidiaProfileStatus",
            output.nvidiaProfileStatus);
        std::string nvidiaProfileName;
        if (ParseJsonString(status, "nvidiaProfileName",
                nvidiaProfileName))
        {
            strncpy_s(output.nvidiaProfileName,
                nvidiaProfileName.c_str(), _TRUNCATE);
        }
        ParseJsonInteger(status, "nvidiaCompatibilityTier",
            output.nvidiaCompatibilityTier);
        ParseJsonInteger(status, "nvidiaCompatibilityManifestEntries",
            output.nvidiaCompatibilityManifestEntries);
        ParseJsonInteger(status, "nvidiaPolicyCeilingMultiplier",
            output.nvidiaPolicyCeilingMultiplier);
        ParseJsonInteger(status, "wrapperNativeMaximumMultiplier",
            output.wrapperNativeMaximumMultiplier);
        ParseJsonBool(status, "compatibilityFallback",
            output.compatibilityFallback);
        ParseJsonInteger(status, "compatibilityReason",
            output.compatibilityReason);
        ParseJsonBool(status, "fullStreamlineOtaRequested",
            output.fullStreamlineOtaRequested);
        ParseJsonBool(status, "fullStreamlineOtaEligible",
            output.fullStreamlineOtaEligible);
        ParseJsonBool(status, "downloadedStreamlinePluginsForced",
            output.downloadedStreamlinePluginsForced);
        ParseJsonInteger(status, "streamlineHostVersionMajor",
            output.streamlineHostVersionMajor);
        ParseJsonInteger(status, "streamlineHostVersionMinor",
            output.streamlineHostVersionMinor);
        ParseJsonInteger(status, "streamlineHostVersionBuild",
            output.streamlineHostVersionBuild);
        ParseJsonInteger(status, "streamlineHostVersionPrivate",
            output.streamlineHostVersionPrivate);
        ParseJsonBool(status, "selectiveOtaDlssgWrapperRequested",
            output.selectiveOtaDlssgWrapperRequested);
        ParseJsonBool(status, "selectiveOtaDlssgWrapperCandidateReady",
            output.selectiveOtaDlssgWrapperCandidateReady);
        ParseJsonInteger(status, "selectiveOtaDlssgWrapperFailure",
            output.selectiveOtaDlssgWrapperFailure);
        ParseJsonInteger(status,
            "selectiveOtaDlssgWrapperRedirectAttempts",
            output.selectiveOtaDlssgWrapperRedirectAttempts);
        ParseJsonInteger(status,
            "selectiveOtaDlssgWrapperRedirectSuccesses",
            output.selectiveOtaDlssgWrapperRedirectSuccesses);
        ParseJsonInteger(status, "selectiveOtaDlssgWrapperFallbacks",
            output.selectiveOtaDlssgWrapperFallbacks);
        ParseJsonInteger(status, "selectiveOtaDlssgWrapperVersionMajor",
            output.selectiveOtaDlssgWrapperVersionMajor);
        ParseJsonInteger(status, "selectiveOtaDlssgWrapperVersionMinor",
            output.selectiveOtaDlssgWrapperVersionMinor);
        ParseJsonInteger(status, "selectiveOtaDlssgWrapperVersionBuild",
            output.selectiveOtaDlssgWrapperVersionBuild);
        ParseJsonInteger(status, "selectiveOtaDlssgWrapperVersionPrivate",
            output.selectiveOtaDlssgWrapperVersionPrivate);
        ParseJsonBool(status, "streamlineLoaderDiscoveryInstalled",
            output.streamlineLoaderDiscoveryInstalled);
        ParseJsonInteger(status, "streamlineLoaderDiscoveryCalls",
            output.streamlineLoaderDiscoveryCalls);
        ParseJsonBool(status, "setOptionsEntryDetourCurrent",
            output.setOptionsEntryDetourCurrent);
        ParseJsonBool(status, "setOptionsResolverFallbackActive",
            output.setOptionsResolverFallbackActive);
        ParseJsonInteger(status, "setOptionsResolverFallbackCalls",
            output.setOptionsResolverFallbackCalls);
        ParseJsonBool(status, "setOptionsControlPathReady",
            output.setOptionsControlPathReady);
        ParseJsonBool(status, "getStateEntryDetourCurrent",
            output.getStateEntryDetourCurrent);
        ParseJsonBool(status, "ngxCreateEntryDetourCurrent",
            output.ngxCreateEntryDetourCurrent);
        ParseJsonBool(status, "ngxEvaluateEntryDetourCurrent",
            output.ngxEvaluateEntryDetourCurrent);
        ParseJsonBool(status, "backportReadyAtCreate",
            output.backportReadyAtCreate);
        ParseJsonBool(status, "pipelineMayPredateDetour",
            output.pipelineMayPredateDetour);
        ParseJsonBool(status, "gameFrameGenerationOn",
            output.gameFrameGenerationOn);
        ParseJsonBool(status, "streamlineRebuildRequired",
            output.streamlineRebuildRequired);
        ParseJsonBool(status, "perSampleSynthesisReady",
            output.perSampleSynthesisReady);
        ParseJsonBool(status, "highCapabilityPublicationAllowed",
            output.highCapabilityPublicationAllowed);
        ParseJsonBool(status, "dllNotificationRegistered",
            output.dllNotificationRegistered);
        ParseJsonBool(status, "streamlinePluginLoaderHooksInstalled",
            output.pluginLoaderHooksInstalled);
        ParseJsonBool(status, "activeWrapperObserved",
            output.activeWrapperObserved);
        ParseJsonBool(status, "activeWrapperUsesNvidiaOta",
            output.activeWrapperUsesNvidiaOta);
        ParseJsonInteger(status, "activeWrapperVersionMajor",
            output.activeWrapperVersionMajor);
        ParseJsonInteger(status, "activeWrapperVersionMinor",
            output.activeWrapperVersionMinor);
        ParseJsonInteger(status, "activeWrapperVersionBuild",
            output.activeWrapperVersionBuild);
        ParseJsonInteger(status, "activeWrapperVersionPrivate",
            output.activeWrapperVersionPrivate);
        ParseJsonInteger(status,
            "wrapperCompiledMaximumGeneratedFrames",
            output.wrapperCompiledMaximumGeneratedFrames);
        ParseJsonInteger(status, "safeMaximumMultiplier",
            output.safeMaximumMultiplier);
        ParseJsonBool(status, "requestedMultiplierLimited",
            output.requestedMultiplierLimited);
        ParseJsonInteger(status, "multiplier", output.desiredMultiplier);
        ParseJsonInteger(status, "appliedMultiplier",
            output.appliedMultiplier);
        ParseJsonInteger(status, "dynamicTargetFrameRate",
            output.dynamicTargetFrameRate);
        ParseJsonInteger(status, "requestRevision",
            output.desiredRevision);
        ParseJsonInteger(status, "appliedRevision",
            output.appliedRevision);
        ParseJsonInteger(status, "actualFramesPresented",
            output.actualFramesPresented);
        ParseJsonInteger(status, "numFramesToGenerateMax",
            output.numFramesToGenerateMax);
        ParseJsonInteger(status, "setOptionsResult",
            output.lastSetOptionsResult);
        ParseJsonInteger(status, "getStateResult",
            output.lastGetStateResult);
        ParseJsonBool(status, "setOptionsSeen",
            output.setOptionsSeen);
        ParseJsonBool(status, "setOptionsAccepted",
            output.setOptionsAccepted);
        ParseJsonBool(status, "getStateSeen",
            output.getStateSeen);
        ParseJsonBool(status, "dynamicMfgSupportKnown",
            output.dynamicMfgSupportKnown);
        ParseJsonBool(status, "dynamicMfgSupported",
            output.dynamicMfgSupported);
        ParseJsonBool(status, "intervalLoggingEnabled",
            output.intervalLoggingEnabled);
        ParseJsonBool(status, "intervalLogReady",
            output.intervalLogReady);
        ParseJsonInteger(status, "intervalValidSamples",
            output.intervalValidSamples);
        ParseJsonInteger(status, "intervalInvalidSamples",
            output.intervalInvalidSamples);
        ParseJsonInteger(status, "intervalDroppedSamples",
            output.intervalDroppedSamples);
        ParseJsonInteger(status, "intervalLastCount",
            output.intervalLastCount);
        ParseJsonInteger(status, "intervalLastIndex",
            output.intervalLastIndex);
        ParseJsonInteger(status, "intervalLastPositionNumerator",
            output.intervalLastPositionNumerator);
        ParseJsonInteger(status, "intervalLastPositionDenominator",
            output.intervalLastPositionDenominator);
        ParseJsonInteger(status, "realFpsMilli", output.realFpsMilli);
        ParseJsonInteger(status, "dlssFpsMilli", output.dlssFpsMilli);
        ParseJsonInteger(status, "fpsSampleAgeMs", output.fpsSampleAgeMs);
        ParseJsonBool(status, "uiInputsReady", output.uiInputsReady);
        ParseJsonBool(status, "uiRecompositionEnabled",
            output.uiRecompositionEnabled);
        ParseJsonBool(status, "uiRecompositionForced",
            output.uiRecompositionForced);
        ParseJsonBool(status, "generatedOnlyDebug",
            output.generatedOnlyDebug);
        ParseJsonBool(status, "appliedGeneratedOnlyDebug",
            output.appliedGeneratedOnlyDebug);
        std::string intervalFile;
        if (ParseJsonString(status, "intervalLogFile", intervalFile))
            strncpy_s(output.intervalLogFile,
                intervalFile.c_str(), _TRUNCATE);
        std::string mode;
        if (ParseJsonString(status, "mode", mode))
            output.dynamicMode = mode == "dynamic" ? TRUE : FALSE;
        std::string appliedMode;
        if (ParseJsonString(status, "appliedMode", appliedMode))
        {
            output.appliedDynamicMode =
                appliedMode == "dynamic" ? TRUE : FALSE;
        }
        std::string route;
        if (ParseJsonString(status, "route", route))
            strncpy_s(output.patchRoute, route.c_str(), _TRUNCATE);
        std::string activeWrapperPath;
        if (ParseJsonString(status, "activeWrapperPath", activeWrapperPath))
            strncpy_s(output.activeWrapperPath,
                activeWrapperPath.c_str(), _TRUNCATE);
        ParseJsonInteger(status, "activeWrapperGeneration",
            output.activeWrapperGeneration);
        std::string activeControlPath;
        if (ParseJsonString(status, "activeControlPath", activeControlPath))
            strncpy_s(output.activeControlPath,
                activeControlPath.c_str(), _TRUNCATE);
        std::string activeStatePath;
        if (ParseJsonString(status, "activeStatePath", activeStatePath))
            strncpy_s(output.activeStatePath,
                activeStatePath.c_str(), _TRUNCATE);
        std::string activeControlDetour;
        if (ParseJsonString(status, "activeControlDetour",
                activeControlDetour))
            strncpy_s(output.activeControlDetour,
                activeControlDetour.c_str(), _TRUNCATE);
        std::string activeStateDetour;
        if (ParseJsonString(status, "activeStateDetour", activeStateDetour))
            strncpy_s(output.activeStateDetour,
                activeStateDetour.c_str(), _TRUNCATE);
        std::string activeProviderPath;
        if (ParseJsonString(status, "activeProviderPath", activeProviderPath))
            strncpy_s(output.activeProviderPath,
                activeProviderPath.c_str(), _TRUNCATE);
        ParseJsonInteger(status, "activeProviderVersionMajor",
            output.activeProviderVersionMajor);
        ParseJsonInteger(status, "activeProviderVersionMinor",
            output.activeProviderVersionMinor);
        ParseJsonInteger(status, "activeProviderVersionBuild",
            output.activeProviderVersionBuild);
        ParseJsonInteger(status, "activeProviderVersionPrivate",
            output.activeProviderVersionPrivate);
        ParseJsonInteger(status, "activeProviderGeneration",
            output.activeProviderGeneration);
        std::string providerSelectionSource;
        if (ParseJsonString(status, "providerSelectionSource",
                providerSelectionSource))
            strncpy_s(output.providerSelectionSource,
                providerSelectionSource.c_str(), _TRUNCATE);
        std::string providerCreateDetour;
        if (ParseJsonString(status, "providerCreateDetour",
                providerCreateDetour))
            strncpy_s(output.providerCreateDetour,
                providerCreateDetour.c_str(), _TRUNCATE);
        std::string providerEvaluateDetour;
        if (ParseJsonString(status, "providerEvaluateDetour",
                providerEvaluateDetour))
            strncpy_s(output.providerEvaluateDetour,
                providerEvaluateDetour.c_str(), _TRUNCATE);
        ParseJsonBool(status, "midpointReadyAtFirstCreate",
            output.midpointReadyAtFirstCreate);
        ParseJsonInteger(status, "activeLastCallRevision",
            output.activeLastCallRevision);
        ParseJsonInteger(status, "activeLastAcceptedRevision",
            output.activeLastAcceptedRevision);
        ParseJsonInteger(status, "universalRouteFailure",
            output.universalRouteFailure);
        std::string routeFailureReason;
        if (ParseJsonString(status, "universalRouteFailureReason",
                routeFailureReason))
            strncpy_s(output.universalRouteFailureReason,
                routeFailureReason.c_str(), _TRUNCATE);
        ParseJsonBool(status, "releaseEntryCurrent",
            output.releaseEntryCurrent);
        ParseJsonBool(status, "frameGenerationOffAccepted",
            output.frameGenerationOffAccepted);
        ParseJsonBool(status, "releaseObserved", output.releaseObserved);
        output.perSampleSynthesisReady = output.midpointReadyAtFirstCreate;
        output.highCapabilityPublicationAllowed =
            output.bridgeReady && output.midpointReadyAtFirstCreate;
    }
    *snapshot = output;
    return TRUE;
}

void MergeNativeMidpointStatus(
    MfgUnlockReShadeSnapshot& snapshot) noexcept
{
    NativeMidpointUiState state{};
    state.publicRouteReady = snapshot.bridgeReady != FALSE;
    state.required = snapshot.dynamicMode != FALSE
        || snapshot.desiredMultiplier > 2
        || snapshot.appliedMultiplier > 2;

    std::string status;
    BOOL rebuildRequired = FALSE;
    if (ReadTextFile(gNativeStatusPath, status))
    {
        const bool complete =
            ParseJsonBool(status, "adaAuthoritativeD3D12DeviceObserved",
                state.adapterObserved)
            && ParseJsonBool(status, "adaTemporalBackportEnabled",
                state.backportEnabled)
            && ParseJsonBool(status, "adaTemporalBackportApplied",
                state.backportApplied)
            && ParseJsonBool(status,
                "ngxFrameGenerationBackportReadyAtCreate",
                state.readyAtCreate)
            && ParseJsonBool(status, "synthesisFallbackActive",
                state.fallbackActive);
        state.available = complete;
        ParseJsonBool(status, "streamlineRebuildRequired",
            rebuildRequired);
        ParseJsonInteger(status, "ngxRawCount", state.rawCount);
        ParseJsonInteger(status, "ngxLastOutputUniqueCount",
            state.uniqueOutputs);
    }
    state.ready = state.available && state.adapterObserved
        && state.backportEnabled && state.backportApplied
        && state.readyAtCreate && !state.fallbackActive;
    gNativeMidpoint = state;
    // Midpoint/backport telemetry belongs to the optional research backend.
    // It must not override the public route reported by the control companion
    // or prevent the proven native ASI from serving ordinary MFG requests.
    (void)rebuildRequired;
}

BOOL WINAPI NativeFileApplyControl(uint32_t multiplier, BOOL dynamicMode,
    uint32_t dynamicTargetFrameRate, BOOL dynamicExperimental56,
    BOOL generatedOnlyDebug)
{
    const BOOL followGame = multiplier == 0 && dynamicMode == FALSE;
    uint32_t safeMaximumMultiplier = 2;
    std::string status;
    if (ReadTextFile(gNativeStatusPath, status))
        ParseJsonInteger(status, "safeMaximumMultiplier",
            safeMaximumMultiplier);
    safeMaximumMultiplier = std::clamp(
        safeMaximumMultiplier, 2u, 6u);
    multiplier = followGame
        ? 2u : std::clamp(multiplier, 2u, safeMaximumMultiplier);
    if (followGame || safeMaximumMultiplier < 6)
        dynamicExperimental56 = FALSE;
    dynamicTargetFrameRate = std::min(dynamicTargetFrameRate, 1000u);
    char content[384]{};
    const int length = std::snprintf(content, sizeof(content),
        "{\"followGame\":%s,\"mode\":\"%s\",\"multiplier\":%u,"
        "\"dynamicTargetFrameRate\":%u,"
        "\"dynamicExperimental56\":%s,"
        "\"generatedOnlyDebug\":%s,\"intervalLogging\":true,"
        "\"version\":11}\n",
        followGame ? "true" : "false",
        followGame ? "follow"
            : dynamicMode ? "dynamic" : "fixed",
        multiplier,
        dynamicTargetFrameRate, dynamicExperimental56 ? "true" : "false",
        generatedOnlyDebug ? "true" : "false");
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(content))
        return FALSE;

    std::error_code error;
    std::filesystem::create_directories(
        std::filesystem::path(gNativeConfigPath).parent_path(), error);
    if (error)
        return FALSE;
    const std::wstring temporary = gNativeConfigPath + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return FALSE;
    DWORD written = 0;
    const BOOL write = WriteFile(file, content, static_cast<DWORD>(length),
        &written, nullptr);
    const BOOL flushed = write && written == static_cast<DWORD>(length)
        && FlushFileBuffers(file);
    CloseHandle(file);
    if (!flushed || !MoveFileExW(temporary.c_str(), gNativeConfigPath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(temporary.c_str());
        return FALSE;
    }
    return TRUE;
}

void WINAPI NativeFileSetFrontendAttached(BOOL)
{
}

BOOL WINAPI NativeFileRegisterD3D12Device(ID3D12Device*)
{
    return TRUE;
}

BOOL WINAPI NativeFileRegisterD3D12Queue(ID3D12CommandQueue*)
{
    return TRUE;
}

BOOL WINAPI NativeFileRegisterD3D12Swapchain(
    IDXGISwapChain*, IUnknown*)
{
    return TRUE;
}

BOOL WINAPI NativeFileUnregisterD3D12Swapchain(IDXGISwapChain*)
{
    return TRUE;
}

bool ConnectBackend() noexcept
{
    if (gBackendConnected.load(std::memory_order_acquire))
        return true;

    AcquireSRWLockExclusive(&gBackendConnectionLock);
    if (gBackendConnected.load(std::memory_order_acquire))
    {
        ReleaseSRWLockExclusive(&gBackendConnectionLock);
        return true;
    }
#if defined(MFG_UNLOCK_V12_UNIVERSAL_UI)
    HMODULE core = GetModuleHandleW(L"RTX40MFGCore.dll");
    HMODULE shim = GetModuleHandleW(L"RTX40MFG.asi");
    if (!core || !shim || !GetProcAddress(core, "MfgUnlockCoreLoaded")
        || !ResolveNativeFilePaths())
    {
        ReleaseSRWLockExclusive(&gBackendConnectionLock);
        return false;
    }
    gBackend = core;
    gBackendInterface = {};
    gSampleFrameTelemetry = reinterpret_cast<
        MfgUnlockSampleFrameTelemetryFn>(GetProcAddress(
            core, "MfgUnlockSampleFrameTelemetry"));
    gBackendInterface.getSnapshot = &NativeFileGetSnapshot;
    gBackendInterface.applyControl = &NativeFileApplyControl;
    gBackendInterface.registerD3D12Device =
        &NativeFileRegisterD3D12Device;
    gBackendInterface.registerD3D12Queue =
        &NativeFileRegisterD3D12Queue;
    gBackendInterface.registerD3D12Swapchain =
        &NativeFileRegisterD3D12Swapchain;
    gBackendInterface.unregisterD3D12Swapchain =
        &NativeFileUnregisterD3D12Swapchain;
    gBackendInterface.setFrontendAttached =
        &NativeFileSetFrontendAttached;
    gBackendKind.store(BackendKind::eNativeFile,
        std::memory_order_release);
    gControlCompanionBackend.store(false, std::memory_order_release);
    gIntegratedUniversalBackend.store(true, std::memory_order_release);
    gBackendConnected.store(true, std::memory_order_release);
    ReleaseSRWLockExclusive(&gBackendConnectionLock);
    return true;
#endif
    HMODULE modules[1024]{};
    DWORD bytes = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), modules,
            sizeof(modules), &bytes))
    {
        ReleaseSRWLockExclusive(&gBackendConnectionLock);
        return false;
    }

    HMODULE backend = nullptr;
    HMODULE nativeBackend = nullptr;
    MfgUnlockBackendInterface interfaceTable{};
    ResolveNativeFilePaths();
    const DWORD count = std::min<DWORD>(bytes / sizeof(HMODULE),
        static_cast<DWORD>(std::size(modules)));
    for (DWORD index = 0; index < count; ++index)
    {
        HMODULE const candidate = modules[index];
        auto* const query = candidate
            ? reinterpret_cast<MfgUnlockBackendQueryInterfaceFn>(
                GetProcAddress(candidate,
                    "MfgUnlockBackendQueryInterface"))
            : nullptr;
        MfgUnlockBackendInterface candidateInterface{};
        if (!query
            || !query(MFG_UNLOCK_BACKEND_ABI_V3, &candidateInterface,
                sizeof(candidateInterface))
            || candidateInterface.structSize < sizeof(candidateInterface)
            || candidateInterface.abiVersion != MFG_UNLOCK_BACKEND_ABI_V3
            || !candidateInterface.getSnapshot
            || !candidateInterface.applyControl
            || !candidateInterface.registerD3D12Device
            || !candidateInterface.registerD3D12Queue
            || !candidateInterface.registerD3D12Swapchain
            || !candidateInterface.unregisterD3D12Swapchain
            || !candidateInterface.setFrontendAttached)
        {
            continue;
        }

        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(query), &pinned)
            || pinned != candidate)
        {
            continue;
        }
        backend = candidate;
        interfaceTable = candidateInterface;
        std::wstring backendPath(32768, L'\0');
        const DWORD backendPathLength = GetModuleFileNameW(candidate,
            backendPath.data(), static_cast<DWORD>(backendPath.size()));
        if (backendPathLength > 0
            && backendPathLength < backendPath.size())
        {
            backendPath.resize(backendPathLength);
        }
        else
        {
            backendPath.clear();
        }
        const bool controlCompanion = !backendPath.empty()
            && _wcsicmp(std::filesystem::path(backendPath).filename().c_str(),
                L"RTX40MFG-Bridge.asi") == 0;
        const bool integratedUniversal = !backendPath.empty()
            && _wcsicmp(std::filesystem::path(backendPath).filename().c_str(),
                L"RTX40MFG-Universal.asi") == 0;
        gControlCompanionBackend.store(
            controlCompanion, std::memory_order_release);
        gIntegratedUniversalBackend.store(
            integratedUniversal, std::memory_order_release);
        ResolveNativeFilePaths();
        break;

        // Unreachable after a valid ABI candidate; native discovery is done
        // below for candidates which do not expose the versioned interface.
    }
    if (!backend)
    {
        for (DWORD index = 0; index < count; ++index)
        {
            HMODULE const candidate = modules[index];
            std::wstring path(32768, L'\0');
            const DWORD pathLength = candidate
                ? GetModuleFileNameW(candidate, path.data(),
                    static_cast<DWORD>(path.size()))
                : 0;
            if (pathLength == 0 || pathLength >= path.size())
            {
                continue;
            }
            path.resize(pathLength);
            if (_wcsicmp(std::filesystem::path(path).filename().c_str(),
                    L"RTX40MFG.asi") != 0)
            {
                continue;
            }
            auto registerDevice = reinterpret_cast<
                MfgUnlockRegisterD3D12DeviceFn>(GetProcAddress(candidate,
                    "MfgUnlockRegisterD3D12Device"));
            auto registerQueue = reinterpret_cast<
                MfgUnlockRegisterD3D12QueueFn>(GetProcAddress(candidate,
                    "MfgUnlockRegisterD3D12Queue"));
            auto registerSwapchain = reinterpret_cast<
                MfgUnlockRegisterD3D12SwapchainFn>(GetProcAddress(candidate,
                    "MfgUnlockRegisterD3D12Swapchain"));
            auto unregisterSwapchain = reinterpret_cast<
                MfgUnlockUnregisterD3D12SwapchainFn>(GetProcAddress(candidate,
                    "MfgUnlockUnregisterD3D12Swapchain"));
            HMODULE pinned = nullptr;
            if (!registerDevice || !registerQueue || !registerSwapchain
                || !unregisterSwapchain || !ResolveNativeFilePaths()
                || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                        | GET_MODULE_HANDLE_EX_FLAG_PIN,
                    reinterpret_cast<LPCWSTR>(registerQueue), &pinned)
                || pinned != candidate)
            {
                continue;
            }
            nativeBackend = candidate;
            interfaceTable.getSnapshot = &NativeFileGetSnapshot;
            interfaceTable.applyControl = &NativeFileApplyControl;
            interfaceTable.registerD3D12Device = registerDevice;
            interfaceTable.registerD3D12Queue = registerQueue;
            interfaceTable.registerD3D12Swapchain = registerSwapchain;
            interfaceTable.unregisterD3D12Swapchain = unregisterSwapchain;
            interfaceTable.setFrontendAttached =
                &NativeFileSetFrontendAttached;
            break;
        }
        if (!nativeBackend)
        {
            ReleaseSRWLockExclusive(&gBackendConnectionLock);
            return false;
        }
    }

    gBackend = backend ? backend : nativeBackend;
    gBackendInterface = interfaceTable;
    gBackendKind.store(backend ? BackendKind::eAbi
        : BackendKind::eNativeFile, std::memory_order_release);
    gBackendConnected.store(true, std::memory_order_release);
    if (backend)
        gBackendInterface.setFrontendAttached(TRUE);
    ReleaseSRWLockExclusive(&gBackendConnectionLock);
    return true;
}

void DisconnectBackend() noexcept
{
    if (gBackendConnected.exchange(false, std::memory_order_acq_rel)
        && gBackendKind.load(std::memory_order_acquire) == BackendKind::eAbi
        && gBackendInterface.setFrontendAttached)
    {
        gBackendInterface.setFrontendAttached(FALSE);
    }
    gBackendInterface = {};
    gBackend = nullptr;
    gSampleFrameTelemetry = nullptr;
    gTelemetrySwapchain.store(0, std::memory_order_release);
    gTelemetrySwapchainTick.store(0, std::memory_order_release);
    gControlCompanionBackend.store(false, std::memory_order_release);
    gNativeMidpoint = {};
    gBackendKind.store(BackendKind::eNone, std::memory_order_release);
}
#endif

bool IsD3D12(reshade::api::device* device) noexcept
{
    return device && device->get_api() == reshade::api::device_api::d3d12;
}

#if defined(MFG_UNLOCK_V12_UNIVERSAL_UI)
void OnFinishPresent(reshade::api::command_queue*,
    reshade::api::swapchain* swapchain)
{
    if (!gSampleFrameTelemetry || !swapchain
        || !IsD3D12(swapchain->get_device()))
    {
        return;
    }

    const uintptr_t candidate = reinterpret_cast<uintptr_t>(swapchain);
    const uint64_t now = GetTickCount64();
    uintptr_t selected = gTelemetrySwapchain.load(std::memory_order_acquire);
    if (selected != candidate)
    {
        const uint64_t lastTick = gTelemetrySwapchainTick.load(
            std::memory_order_acquire);
        if (selected != 0 && now >= lastTick && now - lastTick < 2000)
            return;
        if (!gTelemetrySwapchain.compare_exchange_strong(selected, candidate,
                std::memory_order_acq_rel, std::memory_order_acquire)
            && selected != candidate)
        {
            return;
        }
    }
    gTelemetrySwapchainTick.store(now, std::memory_order_release);
    gSampleFrameTelemetry();
}
#endif

template <typename T>
T* NativeObject(reshade::api::api_object* object) noexcept
{
    return object ? reinterpret_cast<T*>(static_cast<uintptr_t>(
        object->get_native())) : nullptr;
}

void StorePresentationQueue(ID3D12CommandQueue* queue) noexcept
{
    if (queue)
        queue->AddRef();
    AcquireSRWLockExclusive(&gPresentationQueueLock);
    ID3D12CommandQueue* const previous = gPresentationQueue;
    gPresentationQueue = queue;
    ReleaseSRWLockExclusive(&gPresentationQueueLock);
    if (previous)
        previous->Release();
}

void RemovePresentationQueue(ID3D12CommandQueue* queue) noexcept
{
    ID3D12CommandQueue* removed = nullptr;
    AcquireSRWLockExclusive(&gPresentationQueueLock);
    if (!queue || gPresentationQueue == queue)
    {
        removed = gPresentationQueue;
        gPresentationQueue = nullptr;
    }
    ReleaseSRWLockExclusive(&gPresentationQueueLock);
    if (removed)
        removed->Release();
}

ID3D12CommandQueue* RetainPresentationQueue(
    reshade::api::device* expectedDevice) noexcept
{
    ID3D12CommandQueue* queue = nullptr;
    AcquireSRWLockShared(&gPresentationQueueLock);
    queue = gPresentationQueue;
    if (queue)
        queue->AddRef();
    ReleaseSRWLockShared(&gPresentationQueueLock);
    if (!queue || !IsD3D12(expectedDevice))
        return queue;

    ID3D12Device* queueDevice = nullptr;
    IUnknown* queueIdentity = nullptr;
    IUnknown* expectedIdentity = nullptr;
    ID3D12Device* const nativeExpected =
        NativeObject<ID3D12Device>(expectedDevice);
    const bool sameDevice = nativeExpected
        && SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&queueDevice)))
        && SUCCEEDED(queueDevice->QueryInterface(
            IID_PPV_ARGS(&queueIdentity)))
        && SUCCEEDED(nativeExpected->QueryInterface(
            IID_PPV_ARGS(&expectedIdentity)))
        && queueIdentity == expectedIdentity;
    if (expectedIdentity)
        expectedIdentity->Release();
    if (queueIdentity)
        queueIdentity->Release();
    if (queueDevice)
        queueDevice->Release();
    if (!sameDevice)
    {
        queue->Release();
        queue = nullptr;
    }
    return queue;
}

void OnInitDevice(reshade::api::device* device)
{
    if (IsD3D12(device))
        BackendRegisterDevice(NativeObject<ID3D12Device>(device));
}

void OnInitCommandQueue(reshade::api::command_queue* queue)
{
    if (!queue || !IsD3D12(queue->get_device()))
        return;
    ID3D12CommandQueue* const native =
        NativeObject<ID3D12CommandQueue>(queue);
    BackendRegisterQueue(native);
    if ((queue->get_type() & reshade::api::command_queue_type::graphics)
        != reshade::api::command_queue_type{})
    {
        StorePresentationQueue(native);
    }
}

void OnDestroyCommandQueue(reshade::api::command_queue* queue)
{
    if (queue && IsD3D12(queue->get_device()))
        RemovePresentationQueue(NativeObject<ID3D12CommandQueue>(queue));
}

void OnInitSwapchain(reshade::api::swapchain* swapchain, bool)
{
    if (!swapchain || !IsD3D12(swapchain->get_device()))
        return;
    ID3D12CommandQueue* const queue =
        RetainPresentationQueue(swapchain->get_device());
    if (queue)
    {
        BackendRegisterSwapchain(
            NativeObject<IDXGISwapChain>(swapchain), queue);
        queue->Release();
    }
}

void OnDestroySwapchain(reshade::api::swapchain* swapchain, bool)
{
    if (swapchain && IsD3D12(swapchain->get_device()))
    {
        BackendUnregisterSwapchain(
            NativeObject<IDXGISwapChain>(swapchain));
    }
}

void EnsureDefaultOverlayDock(reshade::api::effect_runtime*)
{
    const bool overlayVisited = gOverlayVisitedThisFrame;
    const bool overlayDocked = gOverlayDockedThisFrame;
    gOverlayVisitedThisFrame = false;
    gOverlayDockedThisFrame = false;

    if (!overlayVisited || overlayDocked || gDefaultDockInitialized)
        return;

    // ReShade invokes this event after registered overlay windows. Re-open the
    // already-created Home window to discover its public dock ID, then append
    // one empty Begin/End to our window with that ID queued. Dear ImGui
    // explicitly supports multiple Begin calls for one window in a frame.
    // This avoids depending on ReShade's private DockBuilder node numbering.
    ImGui::Begin(kReShadeHomeWindow, nullptr,
        ImGuiWindowFlags_NoFocusOnAppearing);
    const ImGuiID homeDockId = ImGui::GetWindowDockID();
    ImGui::End();
    if (homeDockId == 0)
        return;

    ImGui::SetNextWindowDockID(homeDockId, ImGuiCond_Always);
    ImGui::Begin(kOverlayTitle, nullptr,
        ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::End();
}

void RegisterRuntimeEvents()
{
#if defined(MFG_UNLOCK_V12_UNIVERSAL_UI)
    reshade::register_event<reshade::addon_event::finish_present>(
        OnFinishPresent);
#else
    reshade::register_event<reshade::addon_event::init_device>(OnInitDevice);
    reshade::register_event<reshade::addon_event::init_command_queue>(
        OnInitCommandQueue);
    reshade::register_event<reshade::addon_event::destroy_command_queue>(
        OnDestroyCommandQueue);
    reshade::register_event<reshade::addon_event::init_swapchain>(
        OnInitSwapchain);
    reshade::register_event<reshade::addon_event::destroy_swapchain>(
        OnDestroySwapchain);
#endif
    reshade::register_event<reshade::addon_event::reshade_overlay>(
        EnsureDefaultOverlayDock);
}

void UnregisterRuntimeEvents()
{
    reshade::unregister_event<reshade::addon_event::reshade_overlay>(
        EnsureDefaultOverlayDock);
#if defined(MFG_UNLOCK_V12_UNIVERSAL_UI)
    reshade::unregister_event<reshade::addon_event::finish_present>(
        OnFinishPresent);
#else
    reshade::unregister_event<reshade::addon_event::destroy_swapchain>(
        OnDestroySwapchain);
    reshade::unregister_event<reshade::addon_event::init_swapchain>(
        OnInitSwapchain);
    reshade::unregister_event<reshade::addon_event::destroy_command_queue>(
        OnDestroyCommandQueue);
    reshade::unregister_event<reshade::addon_event::init_command_queue>(
        OnInitCommandQueue);
    reshade::unregister_event<reshade::addon_event::init_device>(OnInitDevice);
    RemovePresentationQueue(nullptr);
#endif
}

bool ExistingMfgBackendPresent(HMODULE self) noexcept
{
    HMODULE modules[1024]{};
    DWORD bytes = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), modules,
            sizeof(modules), &bytes))
    {
        return true;
    }

    const DWORD count = std::min<DWORD>(bytes / sizeof(HMODULE),
        static_cast<DWORD>(std::size(modules)));
    for (DWORD index = 0; index < count; ++index)
    {
        HMODULE module = modules[index];
        if (!module || module == self)
            continue;
        if (GetProcAddress(module, "MfgUnlockRegisterD3D12Queue")
            || GetProcAddress(module, "MfgUnlockReShadeGetSnapshot"))
        {
            return true;
        }
    }
    return false;
}

void PersistSettings(reshade::api::effect_runtime* runtime)
{
    reshade::set_config_value(runtime, kConfigSection,
        "FollowGameMode", gFollowGameMode);
    reshade::set_config_value(runtime, kConfigSection,
        "Multiplier", gMultiplier);
    reshade::set_config_value(runtime, kConfigSection,
        "DynamicMode", gDynamicMode);
    reshade::set_config_value(runtime, kConfigSection,
        "DynamicTargetFrameRate", gDynamicTargetFrameRate);
    reshade::set_config_value(runtime, kConfigSection,
        "DynamicLockToRefreshRate", gDynamicLockToRefreshRate);
    reshade::set_config_value(runtime, kConfigSection,
        "DynamicCustomTargetFrameRate", gDynamicCustomTargetFrameRate);
    reshade::set_config_value(runtime, kConfigSection,
        "DynamicExperimental56", gDynamicExperimental56);
    reshade::set_config_value(runtime, kConfigSection,
        "GeneratedOnlyDebug", gGeneratedOnlyDebug);
}

void ApplySettings(reshade::api::effect_runtime* runtime, bool persist)
{
    int safeMaximumMultiplier = 6;
#if defined(MFG_UNLOCK_V12_UNIVERSAL_UI)
    MfgUnlockReShadeSnapshot capacity{};
    if (BackendGetSnapshot(&capacity))
    {
        safeMaximumMultiplier = std::clamp(
            static_cast<int>(capacity.safeMaximumMultiplier), 2, 6);
    }
#endif
    gMultiplier = std::clamp(gMultiplier, 2, safeMaximumMultiplier);
    if (gFollowGameMode || safeMaximumMultiplier < 6)
        gDynamicExperimental56 = false;
    gDynamicCustomTargetFrameRate = std::clamp(
        gDynamicCustomTargetFrameRate, 1, 1000);
    gDynamicTargetFrameRate = gDynamicLockToRefreshRate
        ? 0 : gDynamicCustomTargetFrameRate;
    const BOOL accepted = BackendApplyControl(
        gFollowGameMode ? 0u : static_cast<uint32_t>(gMultiplier),
        !gFollowGameMode && gDynamicMode ? TRUE : FALSE,
        static_cast<uint32_t>(gDynamicTargetFrameRate),
        gDynamicExperimental56 ? TRUE : FALSE,
        gGeneratedOnlyDebug ? TRUE : FALSE);
    if (persist)
    {
#if defined(MFG_UNLOCK_RESHADE_UI_CLIENT)
#if defined(MFG_UNLOCK_V12_UNIVERSAL_UI)
        const BOOL companionConfigPersisted = TRUE;
        const BOOL nativeConfigPersisted = accepted;
#else
        const BOOL companionConfigPersisted =
            PersistCompanionControl();
        const BOOL nativeConfigPersisted =
            !gIntegratedUniversalBackend.load(std::memory_order_acquire)
            || (ResolveNativeFilePaths()
                && NativeFileApplyControl(
                    static_cast<uint32_t>(gMultiplier),
                    !gFollowGameMode && gDynamicMode ? TRUE : FALSE,
                    static_cast<uint32_t>(gDynamicTargetFrameRate),
                    gDynamicExperimental56 ? TRUE : FALSE,
                    gGeneratedOnlyDebug ? TRUE : FALSE));
#endif
        gLastNativeConfigPersisted = companionConfigPersisted != FALSE
            && nativeConfigPersisted != FALSE;
#else
        gLastNativeConfigPersisted = true;
#endif
        gLastApplyAttempted = true;
        gLastApplyAccepted = accepted != FALSE;
        PersistSettings(runtime);
    }
}

void LoadSettings()
{
    gDefaultDockInitialized = false;
    reshade::get_config_value(nullptr, kConfigSection,
        "DefaultDockInitialized", gDefaultDockInitialized);

    MfgUnlockReShadeSnapshot snapshot{};
    const bool snapshotLoaded = BackendGetSnapshot(&snapshot) != FALSE;
    if (snapshotLoaded)
    {
        gFollowGameMode = snapshot.followGameMode != FALSE;
        gMultiplier = static_cast<int>(snapshot.desiredMultiplier);
        gDynamicMode = snapshot.dynamicMode != FALSE;
        gDynamicTargetFrameRate = static_cast<int>(
            snapshot.dynamicTargetFrameRate);
        gDynamicLockToRefreshRate = gDynamicTargetFrameRate == 0;
        if (!gDynamicLockToRefreshRate)
        {
            gDynamicCustomTargetFrameRate = gDynamicTargetFrameRate;
        }
        else
        {
            reshade::get_config_value(nullptr, kConfigSection,
                "DynamicCustomTargetFrameRate",
                gDynamicCustomTargetFrameRate);
        }
        gDynamicCustomTargetFrameRate = std::clamp(
            gDynamicCustomTargetFrameRate, 1, 1000);
        gDynamicExperimental56 = snapshot.dynamicExperimental56 != FALSE;
        gGeneratedOnlyDebug = snapshot.generatedOnlyDebug != FALSE;
    }

#if defined(MFG_UNLOCK_V12_UNIVERSAL_UI)
    // The early-loaded core owns the V1.2 Release control file. ReShade may
    // initialize much later, so stale ReShade.ini values must not overwrite
    // the request which was already consumed during Streamline startup.
    if (snapshotLoaded)
        return;
#endif

    reshade::get_config_value(nullptr, kConfigSection,
        "FollowGameMode", gFollowGameMode);
    reshade::get_config_value(nullptr, kConfigSection,
        "Multiplier", gMultiplier);
    reshade::get_config_value(nullptr, kConfigSection,
        "DynamicMode", gDynamicMode);
    reshade::get_config_value(nullptr, kConfigSection,
        "DynamicTargetFrameRate", gDynamicTargetFrameRate);
    gDynamicLockToRefreshRate = gDynamicTargetFrameRate == 0;
    if (!gDynamicLockToRefreshRate)
        gDynamicCustomTargetFrameRate = gDynamicTargetFrameRate;
    reshade::get_config_value(nullptr, kConfigSection,
        "DynamicLockToRefreshRate", gDynamicLockToRefreshRate);
    reshade::get_config_value(nullptr, kConfigSection,
        "DynamicCustomTargetFrameRate", gDynamicCustomTargetFrameRate);
    gDynamicCustomTargetFrameRate = std::clamp(
        gDynamicCustomTargetFrameRate, 1, 1000);
    gDynamicTargetFrameRate = gDynamicLockToRefreshRate
        ? 0 : gDynamicCustomTargetFrameRate;
    reshade::get_config_value(nullptr, kConfigSection,
        "DynamicExperimental56", gDynamicExperimental56);
    reshade::get_config_value(nullptr, kConfigSection,
        "GeneratedOnlyDebug", gGeneratedOnlyDebug);
    ApplySettings(nullptr, false);
#if defined(MFG_UNLOCK_RESHADE_UI_CLIENT)
    // Migrate existing ReShade-only settings to the early companion file.
    // The bridge consumes this on the next public DLSS-G lookup, even when
    // ReShade itself initializes after the feature allocation boundary.
    gLastNativeConfigPersisted = PersistCompanionControl() != FALSE;
#endif
}

const char* StreamlineResultLabel(int32_t result) noexcept
{
    switch (result)
    {
    case 0:
        return "ok";
    case 34:
        return "feature failed to load";
    case 38:
        return "invalid state/capacity";
    case 39:
        return "accepted; low VRAM warning";
    default:
        return "see Streamline result";
    }
}

const char* YesNo(BOOL value) noexcept
{
    return value ? "yes" : "no";
}

void DrawSettings(reshade::api::effect_runtime* runtime)
{
    gOverlayVisitedThisFrame = true;
    gOverlayDockedThisFrame = ImGui::IsWindowDocked();
    if (gOverlayDockedThisFrame && !gDefaultDockInitialized)
    {
        gDefaultDockInitialized = true;
        reshade::set_config_value(runtime, kConfigSection,
            "DefaultDockInitialized", true);
    }

    MfgUnlockReShadeSnapshot snapshot{};
    if (!BackendGetSnapshot(&snapshot))
    {
#if defined(MFG_UNLOCK_RESHADE_UI_CLIENT)
        ImGui::TextUnformatted("RTX40MFG backend is not loaded.");
        ImGui::TextWrapped("The UI loaded correctly. Install "
            "RTX40MFG.asi through Ultimate ASI Loader and fully "
            "restart the game.");
#else
        ImGui::TextUnformatted("Native bridge snapshot is unavailable.");
#endif
        return;
    }

    const int safeMaximumMultiplier = std::clamp(
        static_cast<int>(snapshot.safeMaximumMultiplier), 2, 6);
    const bool dynamicCapabilityKnown =
        snapshot.dynamicMfgSupportKnown != FALSE;
    const bool dynamicSupported = dynamicCapabilityKnown
        && snapshot.dynamicMfgSupported != FALSE;
    if (snapshot.nvidiaCompatibilityResolved)
    {
        if (snapshot.nvidiaCompatibilityTier == 4
            || snapshot.nvidiaCompatibilityTier == 6)
        {
            ImGui::Text("NVIDIA-listed maximum: %uX",
                snapshot.nvidiaCompatibilityTier);
        }
        else
        {
            ImGui::TextUnformatted(
                "NVIDIA-listed maximum: not listed");
        }
    }
    else
    {
        ImGui::TextUnformatted("NVIDIA-listed maximum: unavailable");
    }
    if (snapshot.activeWrapperObserved)
    {
        ImGui::Text("Available maximum: %uX%s", safeMaximumMultiplier,
            snapshot.compatibilityFallback ? " (safe fallback)" : "");
    }
    else
    {
        ImGui::TextUnformatted("Available maximum: detecting active wrapper");
    }
    ImGui::Text("Frame Generation: %s",
        snapshot.gameFrameGenerationOn ? "On" : "Off");
    const bool fpsSampleCurrent = snapshot.realFpsMilli > 0
        && snapshot.dlssFpsMilli > 0
        && snapshot.fpsSampleAgeMs <= 2000;
    if (snapshot.gameFrameGenerationOn && fpsSampleCurrent)
    {
        ImGui::Text("FPS: %.1f real | %.1f DLSS",
            static_cast<double>(snapshot.realFpsMilli) / 1000.0,
            static_cast<double>(snapshot.dlssFpsMilli) / 1000.0);
    }
#if defined(MFG_UNLOCK_V12_UNIVERSAL_UI)
    else if (snapshot.gameFrameGenerationOn && gSampleFrameTelemetry)
    {
        ImGui::TextUnformatted("FPS: measuring...");
    }
#endif
    if (snapshot.statusProtocolVersion < 18)
    {
        ImGui::TextWrapped("Update RTX40MFG.asi and RTX40MFGCore.dll "
            "together; the loaded core uses an older protocol.");
    }
    else if (!snapshot.bridgeReady)
    {
        ImGui::TextUnformatted("Waiting for an active DLSS-G pipeline.");
    }
    if (snapshot.streamlineRebuildRequired
        || snapshot.pipelineMayPredateDetour)
    {
        ImGui::TextWrapped("Toggle Frame Generation Off then On, or restart "
            "the game, to recreate the pipeline with this selection.");
    }

    ImGui::Separator();
    bool settingsChanged = false;
    static const char* fixedModes[] = {
        "2x", "3x", "4x", "5x", "6x"
    };
    if (gDynamicMode && dynamicCapabilityKnown && !dynamicSupported)
    {
        gFollowGameMode = true;
        gDynamicMode = false;
        gDynamicExperimental56 = false;
        settingsChanged = true;
    }

    const char* dynamicLabel = dynamicSupported
        ? "Dynamic"
        : dynamicCapabilityKnown
            ? "Dynamic (unavailable)"
            : "Dynamic (checking...)";
    const char* currentMode = gFollowGameMode
        ? "Follow game"
        : gDynamicMode
            ? dynamicLabel
            : fixedModes[
                std::clamp(gMultiplier, 2, safeMaximumMultiplier) - 2];
    if (ImGui::BeginCombo("MFG mode", currentMode))
    {
        if (ImGui::Selectable("Follow game", gFollowGameMode))
        {
            gFollowGameMode = true;
            gDynamicMode = false;
            settingsChanged = true;
        }
        for (int multiplier = 2; multiplier <= safeMaximumMultiplier;
             ++multiplier)
        {
            const bool selected = !gFollowGameMode && !gDynamicMode
                && gMultiplier == multiplier;
            if (ImGui::Selectable(fixedModes[multiplier - 2], selected))
            {
                gFollowGameMode = false;
                gDynamicMode = false;
                gMultiplier = multiplier;
                settingsChanged = true;
            }
        }
        ImGui::BeginDisabled(!dynamicSupported);
        if (ImGui::Selectable(dynamicLabel, gDynamicMode)
            && dynamicSupported)
        {
            gFollowGameMode = false;
            gDynamicMode = true;
            settingsChanged = true;
        }
        ImGui::EndDisabled();
        ImGui::EndCombo();
    }
    if (dynamicCapabilityKnown && !dynamicSupported)
    {
        ImGui::TextUnformatted(
            "Dynamic mode is not supported by this game.");
    }
    if (!gFollowGameMode && gDynamicMode && dynamicSupported)
    {
        if (ImGui::Checkbox("Lock target to refresh rate",
                &gDynamicLockToRefreshRate))
        {
            settingsChanged = true;
        }
        if (!gDynamicLockToRefreshRate)
        {
            ImGui::DragInt("Custom target FPS",
                &gDynamicCustomTargetFrameRate, 1.0f, 1, 1000);
            if (ImGui::IsItemDeactivatedAfterEdit())
                settingsChanged = true;
        }
        if (safeMaximumMultiplier >= 6)
        {
            if (ImGui::Checkbox("Enable Dynamic 5X/6X (experimental)",
                    &gDynamicExperimental56))
            {
                settingsChanged = true;
            }
        }
    }
    if (gLastApplyAttempted
        && (!gLastApplyAccepted || !gLastNativeConfigPersisted))
    {
        if (gLastApplyAccepted)
            ImGui::TextUnformatted(
                "Request accepted, but the configuration was not saved.");
        else
            ImGui::TextUnformatted(
                "The selection could not be applied or saved.");
    }

    ImGui::Separator();
#if defined(MFG_UNLOCK_V12_UNIVERSAL_UI)
    if (ImGui::CollapsingHeader("Debug"))
    {
        if (ImGui::Checkbox("Generated frames only",
                &gGeneratedOnlyDebug))
        {
            settingsChanged = true;
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Active route");
        ImGui::Text("Core protocol: %u | Bridge: %s",
            snapshot.statusProtocolVersion,
            snapshot.bridgeReady ? "ready" : "not ready");
        ImGui::Text("Fail-closed reason: %s (%u)",
            snapshot.universalRouteFailureReason[0]
                ? snapshot.universalRouteFailureReason : "none",
            snapshot.universalRouteFailure);
        ImGui::TextWrapped("Wrapper: %s",
            snapshot.activeWrapperPath[0]
                ? snapshot.activeWrapperPath : "waiting for a real call");
        ImGui::Text("Wrapper version: %u.%u.%u.%u | generation: %llu",
            snapshot.activeWrapperVersionMajor,
            snapshot.activeWrapperVersionMinor,
            snapshot.activeWrapperVersionBuild,
            snapshot.activeWrapperVersionPrivate,
            static_cast<unsigned long long>(
                snapshot.activeWrapperGeneration));
        ImGui::Text("Control: %s (%s) | State: %s (%s)",
            snapshot.activeControlPath[0]
                ? snapshot.activeControlPath : "none",
            snapshot.activeControlDetour[0]
                ? snapshot.activeControlDetour : "none",
            snapshot.activeStatePath[0]
                ? snapshot.activeStatePath : "none",
            snapshot.activeStateDetour[0]
                ? snapshot.activeStateDetour : "none");

        ImGui::Separator();
        ImGui::TextUnformatted("Active provider");
        ImGui::TextWrapped("Provider: %s",
            snapshot.activeProviderPath[0]
                ? snapshot.activeProviderPath : "waiting for FG Create");
        ImGui::Text("Provider version: %u.%u.%u.%u | generation: %llu",
            snapshot.activeProviderVersionMajor,
            snapshot.activeProviderVersionMinor,
            snapshot.activeProviderVersionBuild,
            snapshot.activeProviderVersionPrivate,
            static_cast<unsigned long long>(
                snapshot.activeProviderGeneration));
        ImGui::Text("Selected by: %s | Create: %s | Evaluate: %s",
            snapshot.providerSelectionSource[0]
                ? snapshot.providerSelectionSource : "none",
            snapshot.providerCreateDetour[0]
                ? snapshot.providerCreateDetour : "none",
            snapshot.providerEvaluateDetour[0]
                ? snapshot.providerEvaluateDetour : "none");
        ImGui::Text("Midpoint ready at first Create: %s",
            YesNo(snapshot.midpointReadyAtFirstCreate));

        ImGui::Separator();
        ImGui::TextUnformatted("Control and lifecycle");
        ImGui::Text("Requested/applied revision: %llu/%llu",
            static_cast<unsigned long long>(snapshot.desiredRevision),
            static_cast<unsigned long long>(snapshot.appliedRevision));
        ImGui::Text("Route last call/accepted revision: %llu/%llu",
            static_cast<unsigned long long>(
                snapshot.activeLastCallRevision),
            static_cast<unsigned long long>(
                snapshot.activeLastAcceptedRevision));
        ImGui::Text("FG: %s | Off accepted: %s | Release observed: %s",
            snapshot.gameFrameGenerationOn ? "on" : "off",
            YesNo(snapshot.frameGenerationOffAccepted),
            YesNo(snapshot.releaseObserved));
        ImGui::Text("Release entry: %s | Recreate required: %s",
            snapshot.releaseEntryCurrent ? "covered" : "unavailable",
            YesNo(snapshot.streamlineRebuildRequired));
        ImGui::Text("NVIDIA max: %s | Available max: %s",
            snapshot.nvidiaCompatibilityTier == 4
                    || snapshot.nvidiaCompatibilityTier == 6
                ? (snapshot.nvidiaCompatibilityTier == 6 ? "6X" : "4X")
                : "not listed",
            snapshot.activeWrapperObserved
                ? (safeMaximumMultiplier == 6 ? "6X"
                    : safeMaximumMultiplier == 5 ? "5X"
                    : safeMaximumMultiplier == 4 ? "4X"
                    : safeMaximumMultiplier == 3 ? "3X" : "2X")
                : "detecting");
        if (snapshot.getStateSeen && snapshot.lastGetStateResult == 0)
        {
            ImGui::Text("Runtime: %u presented | max %u generated",
                snapshot.actualFramesPresented,
                snapshot.numFramesToGenerateMax);
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Temporal interval trace (always on)");
        ImGui::Text("Log: %s | Samples: %llu valid, %llu invalid, %llu dropped",
            snapshot.intervalLogReady ? "ready" : "opening",
            static_cast<unsigned long long>(snapshot.intervalValidSamples),
            static_cast<unsigned long long>(snapshot.intervalInvalidSamples),
            static_cast<unsigned long long>(snapshot.intervalDroppedSamples));
        if (snapshot.intervalValidSamples > 0)
        {
            ImGui::Text("Last interval: count %d | index %d | position %u/%u",
                snapshot.intervalLastCount, snapshot.intervalLastIndex,
                snapshot.intervalLastPositionNumerator,
                snapshot.intervalLastPositionDenominator);
        }
        if (snapshot.intervalLogFile[0])
            ImGui::Text("Trace: %%TEMP%%\\%s", snapshot.intervalLogFile);
    }
#else
    if (ImGui::CollapsingHeader("Debug"))
    {
        if (ImGui::Checkbox("Generated frames only",
                &gGeneratedOnlyDebug))
        {
            settingsChanged = true;
        }
        ImGui::Separator();
        ImGui::TextUnformatted("Core and bridge");
        ImGui::Text("Core loaded: %s | Protocol: %u",
            YesNo(snapshot.loaderCoreImported),
            snapshot.statusProtocolVersion);
        ImGui::Text("Bridge: %s | Route: %s",
            snapshot.bridgeReady ? "connected" : "waiting",
            snapshot.patchRoute[0] ? snapshot.patchRoute : "pending");
#if defined(MFG_UNLOCK_RESHADE_UI_CLIENT) \
    && !defined(MFG_UNLOCK_V12_UNIVERSAL_UI)
        const BackendKind backendKind = gBackendKind.load(
            std::memory_order_acquire);
        const char* backendLabel = backendKind == BackendKind::eAbi
            ? "exported ABI" : backendKind == BackendKind::eNativeFile
                ? "status file" : "none";
        ImGui::Text("UI backend: %s | Companion: %s | Integrated: %s",
            backendLabel,
            gControlCompanionBackend.load(std::memory_order_acquire)
                ? "yes" : "no",
            gIntegratedUniversalBackend.load(std::memory_order_acquire)
                ? "yes" : "no");
#endif
#if !defined(MFG_UNLOCK_V12_UNIVERSAL_UI)
        ImGui::Text("Startup: profile %u | failure %u | mask 0x%08X",
            snapshot.startupProfile,
            snapshot.startupFailure,
            snapshot.startupValidationMask);
        ImGui::Text("Central hook: %s | clean snapshot: %s | worker: %s",
            YesNo(snapshot.centralHookInstalled),
            YesNo(snapshot.startupSnapshotClean),
            YesNo(snapshot.workerIdentityCertified));
        ImGui::Text("Early host: %s | DLL notify: %s | loader hooks: %s",
            YesNo(snapshot.transportEarlyHostCertified),
            YesNo(snapshot.dllNotificationRegistered),
            YesNo(snapshot.pluginLoaderHooksInstalled));
        ImGui::Text("Pre-entry route: %s | owner published: %s",
            YesNo(snapshot.preEntryRouteInstalled),
            YesNo(snapshot.selectedOwnerPublished));
        ImGui::Text("Pre-entry attempts: %llu | successes: %llu | failures: %llu",
            static_cast<unsigned long long>(snapshot.preEntryRouteAttempts),
            static_cast<unsigned long long>(snapshot.preEntryRouteSuccesses),
            static_cast<unsigned long long>(snapshot.preEntryRouteFailures));
#endif
        ImGui::Text("Main resolver: %s | loader discovery: %s (%llu calls)",
            YesNo(snapshot.mainResolverDiscoveryInstalled),
            YesNo(snapshot.streamlineLoaderDiscoveryInstalled),
            static_cast<unsigned long long>(
                snapshot.streamlineLoaderDiscoveryCalls));

        ImGui::Separator();
        ImGui::TextUnformatted("Compatibility");
        ImGui::Text("NVIDIA profile: %s | status: %d | resolved: %s",
            snapshot.nvidiaProfileName[0]
                ? snapshot.nvidiaProfileName : "not available",
            snapshot.nvidiaProfileStatus,
            YesNo(snapshot.nvidiaCompatibilityResolved));
        if (snapshot.nvidiaCompatibilityTier == 4
            || snapshot.nvidiaCompatibilityTier == 6)
        {
            ImGui::Text("NVIDIA-listed maximum: %uX",
                snapshot.nvidiaCompatibilityTier);
        }
        else
        {
            ImGui::TextUnformatted(
                "NVIDIA-listed maximum: not listed");
        }
        ImGui::Text("Manifest entries: %u | Policy maximum: %uX",
            snapshot.nvidiaCompatibilityManifestEntries,
            snapshot.nvidiaPolicyCeilingMultiplier);
        ImGui::Text("Available maximum: %uX | Wrapper maximum: %uX",
            safeMaximumMultiplier,
            snapshot.wrapperNativeMaximumMultiplier);
        ImGui::Text("Wrapper compiled output: %u generated frames",
            snapshot.wrapperCompiledMaximumGeneratedFrames);
        if (snapshot.activeWrapperObserved)
        {
            ImGui::Text("Wrapper: %s %u.%u.%u.%u",
                snapshot.activeWrapperUsesNvidiaOta
                    ? "NVIDIA OTA" : "game-provided",
                snapshot.activeWrapperVersionMajor,
                snapshot.activeWrapperVersionMinor,
                snapshot.activeWrapperVersionBuild,
                snapshot.activeWrapperVersionPrivate);
        }
        else
        {
            ImGui::TextUnformatted("Wrapper: waiting");
        }
        ImGui::Text("Fallback: %s | reason: %u | request limited: %s",
            YesNo(snapshot.compatibilityFallback),
            snapshot.compatibilityReason,
            YesNo(snapshot.requestedMultiplierLimited));

        ImGui::Separator();
        ImGui::TextUnformatted("Control and lifecycle");
    ImGui::Text("Dynamic capability: %s",
        !dynamicCapabilityKnown ? "checking"
            : dynamicSupported ? "supported" : "unsupported");
    if (snapshot.followGameMode)
    {
        ImGui::Text("Requested: follow game  Applied: %s %ux",
            snapshot.appliedRevision != 0
                ? (snapshot.appliedDynamicMode ? "dynamic" : "fixed")
                : "none",
            snapshot.appliedMultiplier);
    }
    else
    {
        ImGui::Text("Requested: %s %ux  Applied: %s %ux",
            snapshot.dynamicMode ? "dynamic" : "fixed",
            snapshot.desiredMultiplier,
            snapshot.appliedRevision != 0
                ? (snapshot.appliedDynamicMode ? "dynamic" : "fixed")
                : "none",
            snapshot.appliedMultiplier);
    }
    if (snapshot.dynamicTargetFrameRate == 0)
    {
        ImGui::TextUnformatted(
            "Dynamic target: refresh rate (wire value 0)");
    }
    else
    {
        ImGui::Text("Dynamic target: %u FPS",
            snapshot.dynamicTargetFrameRate);
    }
    ImGui::Text("Desired revision: %llu | Applied revision: %llu",
        static_cast<unsigned long long>(snapshot.desiredRevision),
        static_cast<unsigned long long>(snapshot.appliedRevision));
    ImGui::Text("Dynamic 5X/6X: %s | Generated-only desired/applied: %s/%s",
        YesNo(snapshot.dynamicExperimental56),
        YesNo(snapshot.generatedOnlyDebug),
        YesNo(snapshot.appliedGeneratedOnlyDebug));
    ImGui::Text("UI refresh lock: %s | custom target: %d FPS",
        gDynamicLockToRefreshRate ? "yes" : "no",
        gDynamicCustomTargetFrameRate);
    ImGui::Text("Last UI apply: attempted %s | accepted %s | saved %s",
        gLastApplyAttempted ? "yes" : "no",
        gLastApplyAccepted ? "yes" : "no",
        gLastNativeConfigPersisted ? "yes" : "no");
    ImGui::Text("FG on: %s | rebuild: %s | pipeline predates detour: %s",
        YesNo(snapshot.gameFrameGenerationOn),
        YesNo(snapshot.streamlineRebuildRequired),
        YesNo(snapshot.pipelineMayPredateDetour));

    ImGui::Separator();
    ImGui::TextUnformatted("Hooks and synthesis");
    ImGui::Text("SetOptions entry: %s | control path: %s | fallback: %s",
        YesNo(snapshot.setOptionsEntryDetourCurrent),
        YesNo(snapshot.setOptionsControlPathReady),
        YesNo(snapshot.setOptionsResolverFallbackActive));
    ImGui::Text("SetOptions fallback calls: %llu | seen: %s | accepted: %s",
        static_cast<unsigned long long>(
            snapshot.setOptionsResolverFallbackCalls),
        YesNo(snapshot.setOptionsSeen),
        YesNo(snapshot.setOptionsAccepted));
    ImGui::Text("GetState entry: %s | Create entry: %s | Evaluate entry: %s",
        YesNo(snapshot.getStateEntryDetourCurrent),
        YesNo(snapshot.ngxCreateEntryDetourCurrent),
        YesNo(snapshot.ngxEvaluateEntryDetourCurrent));
    ImGui::Text("Create midpoint ready: %s | per-sample synthesis: %s",
        YesNo(snapshot.backportReadyAtCreate),
        YesNo(snapshot.perSampleSynthesisReady));
    ImGui::Text("High-capability publication allowed: %s",
        YesNo(snapshot.highCapabilityPublicationAllowed));
#if defined(MFG_UNLOCK_RESHADE_UI_CLIENT) \
    && !defined(MFG_UNLOCK_V12_UNIVERSAL_UI)
    ImGui::Text("Midpoint source: available %s | required %s | ready %s",
        gNativeMidpoint.available ? "yes" : "no",
        gNativeMidpoint.required ? "yes" : "no",
        gNativeMidpoint.ready ? "yes" : "no");
    ImGui::Text("Midpoint route: public %s | adapter %s | fallback %s",
        gNativeMidpoint.publicRouteReady ? "yes" : "no",
        YesNo(gNativeMidpoint.adapterObserved),
        YesNo(gNativeMidpoint.fallbackActive));
    ImGui::Text("Midpoint backport: enabled %s | applied %s | at Create %s",
        YesNo(gNativeMidpoint.backportEnabled),
        YesNo(gNativeMidpoint.backportApplied),
        YesNo(gNativeMidpoint.readyAtCreate));
    ImGui::Text("Midpoint outputs: raw %u | unique %u",
        gNativeMidpoint.rawCount,
        gNativeMidpoint.uniqueOutputs);
#endif

    ImGui::Separator();
    ImGui::TextUnformatted("Runtime");
    if (snapshot.getStateSeen && snapshot.lastGetStateResult == 0)
    {
        ImGui::Text("NGX last sample: %u frames since prior state query",
            snapshot.actualFramesPresented);
        ImGui::Text("Capability: up to %ux (%u generated per real frame)",
            snapshot.numFramesToGenerateMax + 1,
            snapshot.numFramesToGenerateMax);
    }
    else
    {
        ImGui::TextUnformatted("NGX reported: waiting for active FG state");
    }
    if (snapshot.setOptionsSeen)
    {
        ImGui::Text("SetOptions: %d (%s)",
            snapshot.lastSetOptionsResult,
            StreamlineResultLabel(snapshot.lastSetOptionsResult));
    }
    if (snapshot.getStateSeen)
    {
        ImGui::Text("GetState: %d (%s)",
            snapshot.lastGetStateResult,
            StreamlineResultLabel(snapshot.lastGetStateResult));
    }

    const char* uiState = snapshot.uiRecompositionForced
        ? "enabled (forced)" : snapshot.uiRecompositionEnabled
            ? "enabled" : snapshot.uiInputsReady
                ? "inputs ready" : "game-managed";
    ImGui::Text("UI recomposition: %s", uiState);
    ImGui::Text("UI inputs: %s | enabled: %s | forced: %s",
        YesNo(snapshot.uiInputsReady),
        YesNo(snapshot.uiRecompositionEnabled),
        YesNo(snapshot.uiRecompositionForced));

    ImGui::Separator();
    ImGui::TextUnformatted("Streamline and OTA");
    ImGui::Text("Host: %u.%u.%u.%u | slInit calls: %llu",
        snapshot.streamlineHostVersionMajor,
        snapshot.streamlineHostVersionMinor,
        snapshot.streamlineHostVersionBuild,
        snapshot.streamlineHostVersionPrivate,
        static_cast<unsigned long long>(snapshot.slInitCalls));
    ImGui::Text("slInit entry: %s | IAT fallback: %s | resolver: %s",
        YesNo(snapshot.slInitEntryDetourCurrent),
        YesNo(snapshot.slInitIatFallbackInstalled),
        YesNo(snapshot.slInitResolverFallbackActive));
    ImGui::Text("slInit control: %s | flags: 0x%llX -> 0x%llX",
        YesNo(snapshot.slInitControlPathReady),
        static_cast<unsigned long long>(snapshot.slInitFlagsBefore),
        static_cast<unsigned long long>(snapshot.slInitFlagsAfter));
    ImGui::Text("Full OTA requested/eligible: %s/%s | plugins forced: %s",
        YesNo(snapshot.fullStreamlineOtaRequested),
        YesNo(snapshot.fullStreamlineOtaEligible),
        YesNo(snapshot.downloadedStreamlinePluginsForced));
    ImGui::Text("OTA preference forced/at init: %s/%s",
        YesNo(snapshot.otaPreferencesForced),
        YesNo(snapshot.otaPreferencesEnabledAtInit));
    ImGui::Text("Downloaded plugins at init: %s | preflight: %s | suppressed: %s",
        YesNo(snapshot.downloadedStreamlinePluginsEnabledAtInit),
        YesNo(snapshot.otaProviderPreflightSupported),
        YesNo(snapshot.otaForceSuppressed));
#if !defined(MFG_UNLOCK_V12_UNIVERSAL_UI)
    ImGui::Text("Legacy selective OTA requested/candidate: %s/%s | failure: %u",
        YesNo(snapshot.selectiveOtaDlssgWrapperRequested),
        YesNo(snapshot.selectiveOtaDlssgWrapperCandidateReady),
        snapshot.selectiveOtaDlssgWrapperFailure);
    ImGui::Text("Legacy redirects: %llu attempts | %llu successes | %llu fallbacks",
        static_cast<unsigned long long>(
            snapshot.selectiveOtaDlssgWrapperRedirectAttempts),
        static_cast<unsigned long long>(
            snapshot.selectiveOtaDlssgWrapperRedirectSuccesses),
        static_cast<unsigned long long>(
            snapshot.selectiveOtaDlssgWrapperFallbacks));
    ImGui::Text("Legacy candidate: %u.%u.%u.%u",
        snapshot.selectiveOtaDlssgWrapperVersionMajor,
        snapshot.selectiveOtaDlssgWrapperVersionMinor,
        snapshot.selectiveOtaDlssgWrapperVersionBuild,
        snapshot.selectiveOtaDlssgWrapperVersionPrivate);
#endif

    ImGui::Separator();
    ImGui::TextUnformatted("Interval trace (always on)");
    ImGui::Text("Log ready: %s", YesNo(snapshot.intervalLogReady));
    ImGui::Text("Samples: %llu valid | %llu invalid | %llu dropped",
        static_cast<unsigned long long>(snapshot.intervalValidSamples),
        static_cast<unsigned long long>(snapshot.intervalInvalidSamples),
        static_cast<unsigned long long>(snapshot.intervalDroppedSamples));
    if (snapshot.intervalValidSamples > 0)
    {
        ImGui::Text("Last: count %d | index %d | position %u/%u",
            snapshot.intervalLastCount,
            snapshot.intervalLastIndex,
            snapshot.intervalLastPositionNumerator,
            snapshot.intervalLastPositionDenominator);
    }
    if (!snapshot.ngxEvaluateEntryDetourCurrent)
    {
        ImGui::TextUnformatted("Status: Evaluate hook unavailable");
    }
    else if (snapshot.intervalValidSamples == 0)
    {
        ImGui::TextUnformatted(snapshot.intervalLogReady
            ? "Status: waiting for FG Evaluate"
            : "Status: opening log");
    }
    if (snapshot.intervalLogFile[0])
    {
        ImGui::Text("Trace file: %%TEMP%%\\%s",
            snapshot.intervalLogFile);
    }
    }
#endif

    if (settingsChanged)
        ApplySettings(runtime, true);
}
}

extern "C" __declspec(dllexport) const char* AUTHOR = "dashdogy";

#if defined(MFG_UNLOCK_RESHADE_UI_CLIENT)
extern "C" __declspec(dllexport) const char* NAME =
    "Universal RTX 40 MFG Unlock V1.2";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Universal DLSS Multi Frame Generation enabler for supported games on "
    "NVIDIA GeForce RTX 40 Series GPUs.";
#else
extern "C" __declspec(dllexport) const char* NAME =
    "DLSS MFG Unlock - ReShade Early Load";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Universal DLSS Multi Frame Generation enabler for supported games on "
    "NVIDIA GeForce RTX 40 Series GPUs.";
#endif

extern "C" __declspec(dllexport) bool AddonInit(
    HMODULE addonModule, HMODULE reshadeModule)
{
    if (addonModule != gSelf || reshadeModule != gReShade
        || !gRegistered.load(std::memory_order_acquire))
    {
        return false;
    }
    bool expected = false;
    if (!gInitialized.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel, std::memory_order_acquire))
    {
        return true;
    }
    gOverlayVisitedThisFrame = false;
    gOverlayDockedThisFrame = false;
    RegisterRuntimeEvents();
    LoadSettings();
    reshade::register_overlay(kOverlayTitle, DrawSettings);
    return true;
}

extern "C" __declspec(dllexport) void AddonUninit(
    HMODULE addonModule, HMODULE reshadeModule)
{
    if (addonModule != gSelf || reshadeModule != gReShade)
        return;
    if (gInitialized.exchange(false, std::memory_order_acq_rel))
    {
        reshade::unregister_overlay(kOverlayTitle, DrawSettings);
        UnregisterRuntimeEvents();
    }
    if (gRegistered.exchange(false, std::memory_order_acq_rel))
        reshade::unregister_addon(addonModule, reshadeModule);
#if defined(MFG_UNLOCK_RESHADE_UI_CLIENT)
    DisconnectBackend();
#endif
}

namespace reshade_frontend
{
bool ProcessAttach(HMODULE self) noexcept
{
#if defined(MFG_UNLOCK_RESHADE_UI_CLIENT)
    // Backend discovery is intentionally non-fatal. ReShade may initialize
    // before a misconfigured or delayed ASI transport; keeping the UI loaded
    // preserves a visible diagnostic surface and permits a later connection.
    if (!self)
        return false;
    if (!reshade::register_addon(self))
    {
        return false;
    }
    HMODULE reshadeModule = reshade::internal::get_reshade_module_handle();
    if (!reshadeModule)
    {
        reshade::unregister_addon(self, reshadeModule);
        return false;
    }
#else
    if (!self || ExistingMfgBackendPresent(self))
        return false;
    if (!reshade::register_addon(self))
        return false;

    HMODULE reshadeModule = reshade::internal::get_reshade_module_handle();
    if (!reshadeModule
        || !central_feature_hook::ConfigureReShadeEarlyLoad(reshadeModule))
    {
        reshade::unregister_addon(self, reshadeModule);
        return false;
    }
#endif
    gSelf = self;
    gReShade = reshadeModule;
    gRegistered.store(true, std::memory_order_release);
    return true;
}

void ProcessDetach(HMODULE self) noexcept
{
    if (self != gSelf)
        return;
    if (gInitialized.exchange(false, std::memory_order_acq_rel))
    {
        reshade::unregister_overlay(kOverlayTitle, DrawSettings);
        UnregisterRuntimeEvents();
    }
    if (gRegistered.exchange(false, std::memory_order_acq_rel))
        reshade::unregister_addon(self, gReShade);
#if defined(MFG_UNLOCK_RESHADE_UI_CLIENT)
    DisconnectBackend();
#endif
}
}
