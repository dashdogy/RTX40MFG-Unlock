#pragma once

#include <cstdint>

namespace selective_ota_wrapper_policy
{
struct Version
{
    uint32_t major = 0;
    uint32_t minor = 0;
    uint32_t build = 0;
    uint32_t privatePart = 0;
};

// Streamline 2.11 is the first NVIDIA OTA wrapper family in the local cache
// whose binary is natively compiled for five generated frames. Keep this
// experiment on that closest family rather than silently moving an older game
// integration to a newer ABI.
constexpr bool IsCompatibleCandidate(
    const Version& version, uint32_t compiledMaximum) noexcept
{
    return version.major == 2u && version.minor == 11u
        && compiledMaximum == 5u;
}

constexpr bool IsNewer(const Version& candidate,
    const Version& current) noexcept
{
    if (candidate.major != current.major)
        return candidate.major > current.major;
    if (candidate.minor != current.minor)
        return candidate.minor > current.minor;
    if (candidate.build != current.build)
        return candidate.build > current.build;
    return candidate.privatePart > current.privatePart;
}
}
