#include "nvidia_mfg_policy.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

namespace nvidia_mfg_policy
{
namespace
{
constexpr uint32_t kUnicodeStringMaximum = 2048;
using NvUnicodeString = uint16_t[kUnicodeStringMaximum];
using NvSessionHandle = void*;
using NvProfileHandle = void*;
using NvStatus = int32_t;
constexpr NvStatus kNvapiOk = 0;

#pragma pack(push, 4)
struct NvApplication
{
    uint32_t version = 0;
    uint32_t isPredefined = 0;
    NvUnicodeString appName{};
    NvUnicodeString userFriendlyName{};
    NvUnicodeString launcher{};
    NvUnicodeString fileInFolder{};
    uint32_t flags = 0;
    NvUnicodeString commandLine{};
};

struct NvProfile
{
    uint32_t version = 0;
    NvUnicodeString profileName{};
    uint32_t gpuSupport = 0;
    uint32_t isPredefined = 0;
    uint32_t numOfApps = 0;
    uint32_t numOfSettings = 0;
};
#pragma pack(pop)

static_assert(sizeof(NvApplication) == 20492u);
static_assert(sizeof(NvProfile) == 4116u);

constexpr uint32_t StructureVersion(size_t size, uint32_t version) noexcept
{
    return static_cast<uint32_t>(size) | (version << 16u);
}

using QueryInterfaceFn = void* (__cdecl*)(uint32_t);
using InitializeFn = NvStatus (__cdecl*)();
using CreateSessionFn = NvStatus (__cdecl*)(NvSessionHandle*);
using DestroySessionFn = NvStatus (__cdecl*)(NvSessionHandle);
using LoadSettingsFn = NvStatus (__cdecl*)(NvSessionHandle);
using FindApplicationFn = NvStatus (__cdecl*)(NvSessionHandle,
    const uint16_t*, NvProfileHandle*, NvApplication*);
using GetProfileInfoFn = NvStatus (__cdecl*)(NvSessionHandle,
    NvProfileHandle, NvProfile*);

constexpr uint32_t kInitializeId = 0x0150E828u;
constexpr uint32_t kCreateSessionId = 0x0694D52Eu;
constexpr uint32_t kDestroySessionId = 0xDAD9CFF8u;
constexpr uint32_t kLoadSettingsId = 0x375DBD6Bu;
constexpr uint32_t kFindApplicationId = 0xEEE566B2u;
constexpr uint32_t kGetProfileInfoId = 0x61CD6FD6u;

#include "nvidia_mfg_manifest.generated.h"

void CopyUnicode(const wchar_t* source, NvUnicodeString& destination)
{
    static_assert(sizeof(wchar_t) == sizeof(uint16_t));
    std::memset(destination, 0, sizeof(destination));
    if (!source)
        return;
    const size_t count = wcsnlen(source, kUnicodeStringMaximum - 1u);
    std::memcpy(destination, source, count * sizeof(wchar_t));
}

std::wstring ReadUnicode(const NvUnicodeString& source)
{
    static_assert(sizeof(wchar_t) == sizeof(uint16_t));
    const auto* wide = reinterpret_cast<const wchar_t*>(source);
    return std::wstring(wide, wcsnlen(wide, kUnicodeStringMaximum));
}
}

std::string NormalizeTitle(std::wstring_view title)
{
    std::string normalized;
    normalized.reserve(title.size());
    for (wchar_t value : title)
    {
        if (value >= L'A' && value <= L'Z')
            normalized.push_back(static_cast<char>(value - L'A' + L'a'));
        else if ((value >= L'a' && value <= L'z')
            || (value >= L'0' && value <= L'9'))
        {
            normalized.push_back(static_cast<char>(value));
        }
    }
    return normalized;
}

Tier FindTier(std::wstring_view profileName)
{
    const std::string key = NormalizeTitle(profileName);
    const auto found = std::lower_bound(kManifest.begin(), kManifest.end(), key,
        [](const ManifestEntry& entry, const std::string& value) {
            return std::string_view(entry.key) < std::string_view(value);
        });
    return found != kManifest.end() && key == found->key
        ? found->tier : Tier::eUnknown;
}

ProfileQuery IdentifyExecutable(const wchar_t* executablePath)
{
    ProfileQuery result{};
    if (!executablePath || !*executablePath)
        return result;

    HMODULE nvapi = LoadLibraryExW(L"nvapi64.dll", nullptr,
        LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!nvapi)
        return result;
    const auto query = reinterpret_cast<QueryInterfaceFn>(
        GetProcAddress(nvapi, "nvapi_QueryInterface"));
    if (!query)
    {
        FreeLibrary(nvapi);
        return result;
    }

    const auto initialize = reinterpret_cast<InitializeFn>(query(kInitializeId));
    const auto createSession = reinterpret_cast<CreateSessionFn>(
        query(kCreateSessionId));
    const auto destroySession = reinterpret_cast<DestroySessionFn>(
        query(kDestroySessionId));
    const auto loadSettings = reinterpret_cast<LoadSettingsFn>(
        query(kLoadSettingsId));
    const auto findApplication = reinterpret_cast<FindApplicationFn>(
        query(kFindApplicationId));
    const auto getProfileInfo = reinterpret_cast<GetProfileInfoFn>(
        query(kGetProfileInfoId));
    if (!initialize || !createSession || !destroySession || !loadSettings
        || !findApplication || !getProfileInfo)
    {
        FreeLibrary(nvapi);
        return result;
    }

    result.initializeStatus = initialize();
    if (result.initializeStatus != kNvapiOk)
    {
        FreeLibrary(nvapi);
        return result;
    }

    NvSessionHandle session = nullptr;
    result.createSessionStatus = createSession(&session);
    if (result.createSessionStatus != kNvapiOk || !session)
    {
        FreeLibrary(nvapi);
        return result;
    }

    result.loadSettingsStatus = loadSettings(session);
    if (result.loadSettingsStatus == kNvapiOk)
    {
        NvApplication application{};
        application.version = StructureVersion(sizeof(application), 4u);
        NvUnicodeString applicationPath{};
        CopyUnicode(executablePath, applicationPath);
        NvProfileHandle profile = nullptr;
        result.findApplicationStatus = findApplication(session,
            applicationPath, &profile, &application);
        if (result.findApplicationStatus == kNvapiOk && profile)
        {
            NvProfile profileInfo{};
            profileInfo.version = StructureVersion(sizeof(profileInfo), 1u);
            result.getProfileStatus = getProfileInfo(
                session, profile, &profileInfo);
            if (result.getProfileStatus == kNvapiOk)
            {
                result.profileName = ReadUnicode(profileInfo.profileName);
                result.tier = FindTier(result.profileName);
            }
        }
    }

    destroySession(session);
    // Do not call NvAPI_Unload: the host game may already be using NVAPI.
    FreeLibrary(nvapi);
    return result;
}

const char* TierName(Tier tier) noexcept
{
    switch (tier)
    {
    case Tier::eFourX: return "4X";
    case Tier::eSixX: return "6X";
    default: return "unknown";
    }
}

uint32_t ManifestEntryCount() noexcept
{
    return static_cast<uint32_t>(kManifest.size());
}

const char* ManifestFetchedDate() noexcept
{
    return kManifestFetchedDate;
}

const char* ManifestSha256() noexcept
{
    return kManifestSha256;
}
}
