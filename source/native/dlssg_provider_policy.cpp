#include "dlssg_provider_policy.h"

#include <winver.h>

#include <atomic>

namespace dlssg_provider_policy
{
namespace
{
// Published slots own one loader reference and are never cleared. As with
// installed entry detours, cached callers can outlive module discovery. The
// retained image therefore cannot be replaced by a later load at the same base.
std::array<std::atomic<HMODULE>, 64> gRetainedProviders{};
}

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
    return module && HasDlssgExportIdentity(
        GetProcAddress(module, kD3d12ImplementationExport) != nullptr,
        GetProcAddress(module, kDirectSrImplementationExport) != nullptr);
}

bool IsSupportedProvider(HMODULE module, const wchar_t* path) noexcept
{
    // Feature identity is structural. Once that is established, the embedded
    // provider version is the only eligibility input; delivery path, filename,
    // hash, and the versions of unrelated DLSS siblings are irrelevant.
    return IsDlssgImplementationModule(module)
        && SupportedProviderVersionMatches(path);
}

namespace
{
// Keep the large cold-path buffer and its stack probing out of cached calls.
__declspec(noinline) bool RetainSupportedProvider(HMODULE module) noexcept
{
    HMODULE retained = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(module), &retained))
        return false;
    if (retained != module || !IsDlssgImplementationModule(retained))
    {
        FreeLibrary(retained);
        return false;
    }

    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(retained, path,
        static_cast<DWORD>(std::size(path)));
    const bool supported = length != 0 && length < std::size(path)
        && IsSupportedProvider(retained, path);
    if (supported)
    {
        for (auto& slot : gRetainedProviders)
        {
            HMODULE empty = nullptr;
            if (slot.compare_exchange_strong(empty, retained,
                    std::memory_order_acq_rel, std::memory_order_acquire))
                return true; // This slot now owns the reference.
            if (empty == retained)
                break; // A racing validator already retained this image.
        }
    }
    // Rejected providers are not cached. A full cache falls back to validation
    // on each call; it never changes eligibility or publishes an unowned image.
    FreeLibrary(retained);
    return supported;
}
}

bool IsSupportedRetainedProvider(HMODULE module) noexcept
{
    if (!module)
        return false;
    for (const auto& slot : gRetainedProviders)
    {
        const HMODULE cached = slot.load(std::memory_order_acquire);
        if (cached == module)
            return IsDlssgImplementationModule(module);
        if (!cached)
            break;
    }
    return RetainSupportedProvider(module);
}
}
