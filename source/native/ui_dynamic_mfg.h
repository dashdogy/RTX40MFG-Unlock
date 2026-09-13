#pragma once
#include "nvidia_mfg_policy.h"
#include "reshade_bridge.h"

namespace ui_dynamic_mfg
{
enum class Availability { Checking, Unavailable, Supported };

inline Availability Classify(const MfgUnlockReShadeSnapshot& state) noexcept
{
    // These public Streamline APIs predate eDynamic and its state field:
    // https://github.com/NVIDIA-RTX/Streamline/blob/v2.10.0/include/sl_dlss_g.h
    // Only the observed callable owner's version establishes this negative.
    // A loaded but inactive sibling or an unknown version proves nothing.
    const bool legacyRuntime = state.activeWrapperObserved
        && state.activeWrapperVersionMajor == 2
        && state.activeWrapperVersionMinor <= 10;
    const bool rangeFits = nvidia_mfg_policy::DynamicRangeFits(
        state.numFramesToGenerateMax, state.safeMaximumMultiplier);
    const bool rangeBlocked = state.numFramesToGenerateMax != 0 && !rangeFits;
    const bool ampereLimit = state.gpuFamily == 2 && state.safeMaximumMultiplier < 6;

    // A known rejection or limit is already conclusive, even before a newer
    // capability query arrives or while the control bridge is unavailable.
    if (legacyRuntime || rangeBlocked || ampereLimit
        || (state.dynamicMfgSupportKnown && !state.dynamicMfgSupported))
        return Availability::Unavailable;
    if (!state.dynamicMfgSupportKnown || !rangeFits)
        return Availability::Checking;
    return Availability::Supported;
}

inline const char* Label(Availability value) noexcept
{
    switch (value)
    {
    case Availability::Supported: return "Dynamic";
    case Availability::Unavailable: return "Dynamic (unavailable)";
    default: return "Dynamic (checking...)";
    }
}
}
