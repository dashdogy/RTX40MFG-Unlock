#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace nvidia_mfg_policy
{
enum class Tier : uint32_t
{
    eUnknown = 0,
    // Native FG is listed, but NVIDIA publishes no MFG override for this title.
    eNoMfgOverride = 2,
    eFourX = 4,
    eSixX = 6,
};

enum class CapacityReason : uint32_t
{
    eUnknownTitleNativeCapacity = 0,
    eOfficialFourX = 1,
    eOfficialSixX = 2,
    eOfficialSixXWrapperFallback = 3,
    eWrapperPatchUnavailable = 4,
    eNoListedMfgOverride = 5,
};

struct CapacityDecision
{
    uint32_t nvidiaCeilingMultiplier = 0;
    uint32_t wrapperNativeMaximumMultiplier = 2;
    uint32_t effectiveMaximumMultiplier = 2;
    CapacityReason reason = CapacityReason::eUnknownTitleNativeCapacity;
    bool fallback = false;
};

struct ProfileQuery
{
    Tier tier = Tier::eUnknown;
    std::wstring profileName;
    int32_t initializeStatus = -1;
    int32_t createSessionStatus = -1;
    int32_t loadSettingsStatus = -1;
    int32_t findApplicationStatus = -1;
    int32_t getProfileStatus = -1;
};

constexpr bool IsRecognizedCompiledMaximum(uint32_t generatedFrames) noexcept
{
    return generatedFrames == 1u || generatedFrames == 3u
        || generatedFrames == 5u;
}

constexpr uint32_t NativeMaximumMultiplier(
    uint32_t compiledMaximumGeneratedFrames) noexcept
{
    return IsRecognizedCompiledMaximum(compiledMaximumGeneratedFrames)
        ? compiledMaximumGeneratedFrames + 1u : 2u;
}

constexpr uint32_t PolicyCeiling(Tier tier) noexcept
{
    switch (tier)
    {
    case Tier::eNoMfgOverride: return 2u; // Conservative mod fallback, not an NVIDIA 2x listing.
    case Tier::eFourX: return 4u;
    case Tier::eSixX: return 6u;
    default: return 0u; // Unknown must not invent an NVIDIA maximum.
    }
}

constexpr uint32_t LimitToPolicy(Tier tier, uint32_t runtimeMaximum) noexcept
{
    const uint32_t ceiling = PolicyCeiling(tier);
    return ceiling != 0u && ceiling < runtimeMaximum ? ceiling : runtimeMaximum;
}

// Streamline Dynamic ignores numFramesToGenerate. Only offer it when the
// reported runtime range already fits the policy/capacity bound. This does
// not turn the reported maximum into allocation or lifetime evidence.
constexpr bool DynamicRangeFits(uint32_t maximumGeneratedFrames,
    uint32_t allowedMultiplier) noexcept
{
    return allowedMultiplier >= 2u && allowedMultiplier <= 6u
        && maximumGeneratedFrames != 0u
        && maximumGeneratedFrames < allowedMultiplier;
}

// The published per-title limit and the wrapper limit are independent bounds.
// Patching a clamp does not establish larger allocation/ring capacity. This
// decision never lifts the compiled limit; BridgeReady and lifetime checks
// still govern whether an override can be applied to a particular session.
constexpr CapacityDecision DecideCapacity(Tier tier, bool wrapperPatched,
    uint32_t compiledMaximumGeneratedFrames) noexcept
{
    const uint32_t nativeMaximum = NativeMaximumMultiplier(
        compiledMaximumGeneratedFrames);
    CapacityDecision decision{};
    decision.wrapperNativeMaximumMultiplier = nativeMaximum;
    decision.nvidiaCeilingMultiplier = PolicyCeiling(tier);
    const uint32_t runtimeMaximum = wrapperPatched ? nativeMaximum : 2u;
    decision.effectiveMaximumMultiplier = LimitToPolicy(tier, runtimeMaximum);
    decision.fallback = decision.nvidiaCeilingMultiplier > decision.effectiveMaximumMultiplier;

    if (tier == Tier::eNoMfgOverride)
    {
        decision.reason = CapacityReason::eNoListedMfgOverride;
        return decision;
    }

    if (tier == Tier::eFourX)
    {
        decision.reason = !decision.fallback
            ? CapacityReason::eOfficialFourX
            : CapacityReason::eWrapperPatchUnavailable;
        return decision;
    }

    if (tier == Tier::eSixX)
    {
        if (!decision.fallback)
        {
            decision.reason = CapacityReason::eOfficialSixX;
        }
        else if (decision.effectiveMaximumMultiplier == 4u)
        {
            decision.reason = CapacityReason::eOfficialSixXWrapperFallback;
        }
        else
        {
            decision.reason = CapacityReason::eWrapperPatchUnavailable;
        }
        return decision;
    }

    decision.reason = CapacityReason::eUnknownTitleNativeCapacity;
    return decision;
}

std::string NormalizeTitle(std::wstring_view title);
Tier FindTier(std::wstring_view profileName);
ProfileQuery IdentifyExecutable(const wchar_t* executablePath);
const char* TierName(Tier tier) noexcept;

uint32_t ManifestEntryCount() noexcept;
const char* ManifestFetchedDate() noexcept;
const char* ManifestSha256() noexcept;
}
