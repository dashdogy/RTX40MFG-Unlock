#pragma once

#include <cstdint>

namespace streamline_ota_policy
{
constexpr uint64_t kAllowOta = 1ull << 3;
constexpr uint64_t kLoadDownloadedPlugins = 1ull << 6;

struct Result
{
    uint64_t flags = 0;
    bool allowOtaForced = false;
    bool loadDownloadedPluginsForced = false;
    bool fullOtaRequested = false;
    bool fullOtaSuppressed = false;
};

constexpr Result Apply(uint64_t flags, bool officialSixX,
    bool coherentHost, bool loaderDiscoveryReady) noexcept
{
    const bool enable = officialSixX && coherentHost
        && loaderDiscoveryReady;
    const uint64_t adjusted = enable
        ? flags | kAllowOta | kLoadDownloadedPlugins : flags;
    return {
        adjusted,
        enable && (flags & kAllowOta) == 0,
        enable && (flags & kLoadDownloadedPlugins) == 0,
        officialSixX,
        officialSixX && !enable,
    };
}
}
