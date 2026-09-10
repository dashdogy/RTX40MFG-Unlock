#pragma once
#include <algorithm>
#include <cstdint>

namespace frame_telemetry
{
struct PresentDelta
{
    uint32_t frames = 0;
    bool resetWindow = false;
    bool available = false;
};

// Sample the cumulative DXGI count, including native presents made between
// application callbacks. The owner is a lifetime generation, never a pointer.
class PresentCounter
{
public:
    PresentDelta Sample(uint64_t owner, uint32_t count,
        uint64_t tick, bool available) noexcept
    {
        const bool discontinuity = owner != owner_ || tick < tick_
            || tick - tick_ > 2000;
        owner_ = owner;
        tick_ = tick;
        if (!available || owner == 0)
        {
            const bool reset = haveCount_ || discontinuity;
            haveCount_ = false;
            return {0, reset, false};
        }
        const uint32_t delta = count - count_; // UINT wrap is intentional.
        const bool reset = !haveCount_ || discontinuity || delta >= 0x80000000u;
        count_ = count;
        haveCount_ = true;
        return {reset ? 0u : delta, reset, true};
    }

private:
    uint64_t owner_ = 0;
    uint64_t tick_ = 0;
    uint32_t count_ = 0;
    bool haveCount_ = false;
};

inline uint32_t RateMilli(uint64_t frames, uint64_t frequency,
    uint64_t elapsedTicks) noexcept
{
    if (!frequency || !elapsedTicks) return 0;
    const long double value = static_cast<long double>(frames)
        * frequency * 1000.0L / elapsedTicks;
    return static_cast<uint32_t>(std::min<long double>(UINT32_MAX, value + 0.5L));
}

// Internal single-module call; the existing exported no-argument ABI is retained.
void SamplePresentCounter(uint64_t owner, uint32_t count, bool available);
}
