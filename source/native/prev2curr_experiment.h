#pragma once

#include <Windows.h>
#include <cstdint>

#ifndef MFG_UNLOCK_PREV2CURR_EXPERIMENT
#define MFG_UNLOCK_PREV2CURR_EXPERIMENT 0
#endif

namespace midpoint_fix
{
// Private experimental telemetry. This is not part of the public control ABI.
struct Prev2CurrSnapshot
{
    bool candidate = MFG_UNLOCK_PREV2CURR_EXPERIMENT != 0;
    bool attempted = false;
    bool sourceVerified = false;
    bool ready = false;
    bool published = false;
    uint32_t failure = 0;
    uint32_t sourceBytes = 0;
    uint32_t activeBytes = 0;
    uintptr_t provider = 0;
    uintptr_t descriptorEntry = 0;
    uintptr_t originalDescriptor = 0;
    uintptr_t selectedDescriptor = 0;
    uintptr_t originalFatbin = 0;
    uintptr_t selectedFatbin = 0;
    char sourceSha256[65]{};
    char selectedSha256[65]{};
    char selectedPtxSha256[65]{};
};

// Called only at the already-verified, pre-CreateFeature provider boundary.
// The selected program remains fixed for the lifetime of this process.
bool PreparePrev2CurrForCreate(HMODULE module, const wchar_t* path) noexcept;
Prev2CurrSnapshot ReadPrev2CurrSnapshot() noexcept;
}
