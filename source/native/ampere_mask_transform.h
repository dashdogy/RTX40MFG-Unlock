#pragma once
#include <cstddef>
#include <cstdint>

namespace ampere_mask_transform
{
// Apply only to the exact verified slot-14 source fatbin and its decompressed
// SM89 PTX, before the caller retargets that PTX for real SM86 compilation.
// Source/output hashes cover the complete PTX. No occupancy hint is added.
// The source is read-only; ptxBytes changes only after successful validation.
bool Apply(const uint8_t* sourceFatbin, size_t sourceFatbinBytes,
    uint8_t* ptx, size_t& ptxBytes, size_t capacity) noexcept;
}
