#pragma once

#include <cstdint>
#include "ampere_wrapper_capacity.h"

namespace ampere_policy
{
constexpr uint32_t kPresetSetting = 0x10E41DF1u;
constexpr uint32_t kPresetB = 2;
constexpr uint32_t kMaximumGeneratedFrames = ampere_mfg::kMaximumGeneratedFrames;

constexpr bool EvaluationCountValid(int count) noexcept
{
    return count >= 1 && count <= static_cast<int>(kMaximumGeneratedFrames);
}

// One native render interval owns a fixed count of distinct submissions.
// Counts may change only between complete intervals, within the same proven
// six-buffer allocation. This covers fixed modes and native Dynamic batches.
// This records successful submissions, not completed GPU work or presentation.
struct Batch
{
    uint64_t frame = 0;
    std::array<uintptr_t, kMaximumGeneratedFrames> outputs{};
    uint32_t generated = 0;
    uint32_t submitted = 0;
    bool started = false;
    bool failed = false;

    bool Accepts(uint64_t value, int count, int index, uintptr_t output) const noexcept
    {
        if (failed || !output || !EvaluationCountValid(count) || index < 1 || index > count) return false;
        if (!started) return index == 1;
        if (submitted == generated)
            return index == 1 && value != frame;
        if (value != frame || count != generated || index != submitted + 1) return false;
        for (uint32_t i = 0; i < submitted; ++i)
            if (output == outputs[i]) return false;
        return true;
    }
    void Complete(uint64_t value, int count, int index, uintptr_t output, bool succeeded) noexcept
    {
        if (!succeeded || !Accepts(value, count, index, output)) { failed = true; return; }
        if (index == 1) { frame = value; generated = static_cast<uint32_t>(count); outputs = {}; started = true; }
        outputs[static_cast<size_t>(index - 1)] = output;
        submitted = static_cast<uint32_t>(index);
    }
};

constexpr bool AdapterMatches(uint64_t d3dLuid, uint64_t cudaLuid,
    unsigned matches, int major, int minor, unsigned nodeMask) noexcept
{
    return d3dLuid != 0 && d3dLuid == cudaLuid && matches == 1
        && major == 8 && minor == 6 && nodeMask != 0
        && (nodeMask & (nodeMask - 1)) == 0;
}

constexpr bool ControlValid(unsigned multiplier, bool dynamic,
    bool experimental, bool generatedOnly) noexcept
{
    return multiplier >= 1 && multiplier <= kMaximumGeneratedFrames + 1
        && (!dynamic || multiplier != 1) && !experimental && !generatedOnly;
}

// Public NGX handles are opaque. No EndpointCore offsets, inferred allocation
// size, or unobserved release are accepted as a substitute for this identity.
struct Feature
{
    uintptr_t handle = 0;
    uintptr_t owner = 0;
    uint64_t generation = 0;
    uint64_t publication = 0;
    uint64_t luid = 0;
    bool evaluated = false;
    uintptr_t provider = 0;
    uint64_t providerGeneration = 0;
    uintptr_t providerHandle = 0;
    uint32_t createdGeneratedFrames = 0;
    uintptr_t device = 0; // Identity only; NGX owns the device lifetime.
    uint64_t outputWidth = 0;
    uint32_t outputHeight = 0, outputFormat = 0;
    Batch batch{};
    uintptr_t wrapper = 0;
    uint64_t wrapperGeneration = 0;
    ampere_wrapper::Capacity capacity{};
    // Distinguishes successive native feature lifetimes when NGX reuses the
    // same opaque handle and all module/publication identities are unchanged.
    // Kept after the original aggregate fields so policy fixtures remain exact.
    uint64_t lifetime = 0;
    uint64_t createAttemptToken = 0;

    bool Matches(uintptr_t value, uintptr_t module, uint64_t load,
        uint64_t program, uint64_t adapter) const noexcept
    {
        return handle && value == handle && owner == module && generation == load
            && publication == program && luid == adapter;
    }
    bool Release(bool succeeded) noexcept
    {
        if (!handle || !succeeded) return false;
        *this = {};
        return true;
    }
};
}
