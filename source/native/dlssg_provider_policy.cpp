#include "dlssg_provider_policy.h"

#include <winver.h>

namespace dlssg_provider_policy
{
bool ReadProviderVersion(
    const wchar_t* path, VersionTriplet& version) noexcept
{
    version = {};
    if (!path || !*path)
        return false;

    DWORD ignored = 0;
    const DWORD versionBytes = GetFileVersionInfoSizeW(path, &ignored);
    if (!versionBytes)
        return false;

    void* const versionData = VirtualAlloc(nullptr, versionBytes,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!versionData)
        return false;

    VS_FIXEDFILEINFO* fixedInfo = nullptr;
    UINT fixedInfoBytes = 0;
    const bool versionRead = GetFileVersionInfoW(
            path, 0, versionBytes, versionData)
        && VerQueryValueW(versionData, L"\\",
            reinterpret_cast<void**>(&fixedInfo), &fixedInfoBytes)
        && fixedInfo && fixedInfoBytes >= sizeof(VS_FIXEDFILEINFO)
        && fixedInfo->dwSignature == VS_FFI_SIGNATURE;
    if (versionRead)
    {
        version.major = HIWORD(fixedInfo->dwFileVersionMS);
        version.minor = LOWORD(fixedInfo->dwFileVersionMS);
        version.build = HIWORD(fixedInfo->dwFileVersionLS);
    }
    VirtualFree(versionData, 0, MEM_RELEASE);
    return versionRead;
}

bool SupportedProviderVersionMatches(const wchar_t* path) noexcept
{
    VersionTriplet version{};
    return ReadProviderVersion(path, version)
        && IsSupportedVersion(version);
}

bool IsDlssgImplementationModule(HMODULE module) noexcept
{
    return module && GetProcAddress(module, kD3d12ImplementationExport);
}

bool IsSupportedProvider(HMODULE module, const wchar_t* path) noexcept
{
    // Feature identity is structural. Once that is established, the embedded
    // provider version is the only eligibility input; delivery path, filename,
    // hash, and the versions of unrelated DLSS siblings are irrelevant.
    return IsDlssgImplementationModule(module)
        && SupportedProviderVersionMatches(path);
}
}
