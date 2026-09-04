#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace nvidia_mfg_policy
{
enum class Tier : uint32_t
{
    eUnknown = 0,
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

// NVIDIA's published title maximum is the policy ceiling. A structurally verified wrapper
// can lift a 2X/4X integration to the official 4X ceiling. A 6X title reaches
// 6X only with a wrapper natively laid out for five generated frames; otherwise
// it falls back to 4X before SetOptions or feature creation.
constexpr CapacityDecision DecideCapacity(Tier tier, bool wrapperPatched,
    uint32_t compiledMaximumGeneratedFrames) noexcept
{
    const uint32_t nativeMaximum = NativeMaximumMultiplier(
        compiledMaximumGeneratedFrames);
    CapacityDecision decision{};
    decision.wrapperNativeMaximumMultiplier = nativeMaximum;

    if (tier == Tier::eFourX)
    {
        const bool reachesOfficialCeiling = wrapperPatched
            || nativeMaximum >= 4u;
        decision.nvidiaCeilingMultiplier = 4u;
        decision.effectiveMaximumMultiplier = reachesOfficialCeiling
            ? 4u : nativeMaximum;
        decision.reason = reachesOfficialCeiling
            ? CapacityReason::eOfficialFourX
            : CapacityReason::eWrapperPatchUnavailable;
        decision.fallback = !reachesOfficialCeiling;
        return decision;
    }

    if (tier == Tier::eSixX)
    {
        decision.nvidiaCeilingMultiplier = 6u;
        if (nativeMaximum >= 6u)
        {
            decision.effectiveMaximumMultiplier = 6u;
            decision.reason = CapacityReason::eOfficialSixX;
        }
        else if (wrapperPatched)
        {
            decision.effectiveMaximumMultiplier = 4u;
            decision.reason = CapacityReason::eOfficialSixXWrapperFallback;
            decision.fallback = true;
        }
        else
        {
            decision.effectiveMaximumMultiplier = nativeMaximum;
            decision.reason = CapacityReason::eWrapperPatchUnavailable;
            decision.fallback = true;
        }
        return decision;
    }

    decision.effectiveMaximumMultiplier = nativeMaximum;
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
