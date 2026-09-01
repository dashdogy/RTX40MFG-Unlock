#pragma once

#include <Windows.h>

#include <array>
#include <cstdint>

namespace dlssg_provider_policy
{
struct VersionTriplet
{
    uint16_t major = 0;
    uint16_t minor = 0;
    uint16_t build = 0;
};

// Only provider builds which passed the complete immutable layout and payload
// contract belong here. The fourth file-version component is intentionally not
// part of eligibility because it did not change the validated provider layout.
inline constexpr std::array<VersionTriplet, 4> kSupportedVersions{{
    {310, 7, 0},
    {310, 7, 128},
    {310, 7, 129},
    {310, 8, 0},
}};

inline constexpr char kD3d12ImplementationExport[] =
    "NVSDK_NGX_D3D12_PopulateDeviceParameters_Impl";

constexpr bool IsSupportedVersion(VersionTriplet version) noexcept
{
    for (const auto supported : kSupportedVersions)
    {
        if (version.major == supported.major
            && version.minor == supported.minor
            && version.build == supported.build)
        {
            return true;
        }
    }
    return false;
}

static_assert(IsSupportedVersion({310, 7, 0}));
static_assert(IsSupportedVersion({310, 7, 128}));
static_assert(IsSupportedVersion({310, 7, 129}));
static_assert(IsSupportedVersion({310, 8, 0}));
static_assert(!IsSupportedVersion({310, 7, 1}));
static_assert(!IsSupportedVersion({310, 8, 1}));

bool ReadProviderVersion(
    const wchar_t* path, VersionTriplet& version) noexcept;
bool SupportedProviderVersionMatches(const wchar_t* path) noexcept;
bool IsDlssgImplementationModule(HMODULE module) noexcept;
bool IsSupportedProvider(HMODULE module, const wchar_t* path) noexcept;
}
