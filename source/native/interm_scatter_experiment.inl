// Included inside midpoint_fix.cpp's private namespace, before the existing
// temporal builder. The original descriptor is published only once.
#ifndef MFG_UNLOCK_INTERM_SCATTER_EXPERIMENT
#define MFG_UNLOCK_INTERM_SCATTER_EXPERIMENT 0
#endif

#include "interm_scatter_profiles.h"

const IntermScatterProfile* IntermScatterProfileForTemporal(
    const TemporalProviderProfile* profile) noexcept
{
    if (profile == &k3109TemporalProfile)
        return &kIntermScatter3109;
    if (profile == &kLegacySlot9TemporalProfile)
        return &kIntermScatterLegacy;
    return nullptr;
}

const IntermScatterProfile* SelectedIntermScatterProfile(
    const TemporalProviderProfile& profile) noexcept
{
#if MFG_UNLOCK_INTERM_SCATTER_EXPERIMENT
    return IntermScatterProfileForTemporal(&profile);
#else
    (void)profile;
    return nullptr;
#endif
}

bool BuildIntermScatterFatbin(uint8_t* fatbin, size_t capacity,
    uint8_t* scratch, size_t scratchCapacity,
    const IntermScatterProfile& profile, uint32_t& outputBytes,
    Failure& failure) noexcept
{
    outputBytes = 0;
    failure = Failure::eTemporalLayout;
    if (!fatbin || !scratch || profile.sourceBytes < 120
        || capacity < profile.sourceBytes || capacity < profile.outputBytes
        || profile.sm120RawBytes < 16 || profile.sm120RawBytes > scratchCapacity
        || profile.sm89Offset < 120 || profile.sm89Offset > profile.sourceBytes
        || profile.sourceBytes - profile.sm89Offset < 104
        || !profile.sourceSha256 || !profile.sourcePtxSha256
        || !profile.outputSha256 || !profile.outputPtxSha256
        || !profile.parameterDeclaration || !profile.temporalLoad)
        return false;
    if (!Sha256Equals(fatbin, profile.sourceBytes, profile.sourceSha256))
    {
        failure = Failure::eSourceIdentity;
        return false;
    }

    const uint8_t* sm120 = fatbin + 16;
    uint8_t* sm89 = fatbin + profile.sm89Offset;
    if (ReadU32(fatbin) != 0xBA55ED50u || ReadU16(fatbin + 6) != 16
        || ReadU64(fatbin + 8) != profile.sourceBytes - 16ull
        || ReadU16(sm120) != 1 || ReadU32(sm120 + 4) != 104
        || ReadU32(sm120 + 28) != 120
        || ReadU64(sm120 + 8) != profile.sm120PayloadBytes
        || ReadU32(sm120 + 16) != profile.sm120CompressedBytes
        || ReadU64(sm120 + 40) != 0x2041
        || ReadU64(sm120 + 56) != profile.sm120RawBytes
        || 120ull + profile.sm120PayloadBytes != profile.sm89Offset
        || profile.sm120CompressedBytes == 0
        || profile.sm120CompressedBytes > profile.sm120PayloadBytes
        || ReadU16(sm89) != 1 || ReadU32(sm89 + 4) != 104
        || ReadU32(sm89 + 28) != 89 || ReadU64(sm89 + 40) != 0x2041)
        return false;
    if (!DecompressNvidiaLz(sm120 + 104, profile.sm120CompressedBytes,
            scratch, profile.sm120RawBytes))
    {
        failure = Failure::eDecompression;
        return false;
    }
    if (!Sha256Equals(scratch, profile.sm120RawBytes, profile.sourcePtxSha256))
    {
        failure = Failure::eSourceIdentity;
        return false;
    }

    constexpr char oldTarget[] = ".target sm_120";
    constexpr char newTarget[] = ".target sm_89";
    constexpr char block[] = ".maxntid 324, 1, 1";
    constexpr char complement[] = "sub.ftz.f32 %f4, %f27, %f1;";
    const size_t target = FindUniqueBytes(scratch, profile.sm120RawBytes,
        oldTarget, sizeof(oldTarget) - 1);
    if (target == SIZE_MAX
        || FindUniqueBytes(scratch, profile.sm120RawBytes,
            block, sizeof(block) - 1) == SIZE_MAX
        || FindUniqueBytes(scratch, profile.sm120RawBytes,
            profile.parameterDeclaration,
            std::strlen(profile.parameterDeclaration)) == SIZE_MAX
        || FindUniqueBytes(scratch, profile.sm120RawBytes,
            profile.temporalLoad, std::strlen(profile.temporalLoad)) == SIZE_MAX
        || FindUniqueBytes(scratch, profile.sm120RawBytes,
            complement, sizeof(complement) - 1) == SIZE_MAX)
        return false;

    // Keep all temporal, texture, scatter and floating-point operations intact.
    std::memcpy(scratch + target, newTarget, sizeof(newTarget) - 1);
    std::memmove(scratch + target + sizeof(newTarget) - 1,
        scratch + target + sizeof(oldTarget) - 1,
        profile.sm120RawBytes - target - (sizeof(oldTarget) - 1));
    const size_t ptxBytes = profile.sm120RawBytes - 1;
    if (!Sha256Equals(scratch, ptxBytes, profile.outputPtxSha256))
    {
        failure = Failure::eOutputIdentity;
        return false;
    }

    const size_t paddedPtxBytes = (ptxBytes + 7) & ~size_t(7);
    const size_t total = profile.sm89Offset + 104ull + paddedPtxBytes;
    if (total > capacity || total != profile.outputBytes)
        return false;
    std::memcpy(sm89 + 104, scratch, ptxBytes);
    std::memset(sm89 + 104 + ptxBytes, 0, paddedPtxBytes - ptxBytes);
    const uint64_t payloadBytes = paddedPtxBytes;
    const uint64_t outerBytes = total - 16;
    constexpr uint32_t zero32 = 0;
    constexpr uint64_t zero64 = 0;
    constexpr uint64_t uncompressedFlags = 0x41;
    std::memcpy(sm89 + 8, &payloadBytes, sizeof(payloadBytes));
    std::memcpy(sm89 + 16, &zero32, sizeof(zero32));
    std::memcpy(sm89 + 40, &uncompressedFlags, sizeof(uncompressedFlags));
    std::memcpy(sm89 + 56, &zero64, sizeof(zero64));
    std::memcpy(fatbin + 8, &outerBytes, sizeof(outerBytes));
    if (!Sha256Equals(fatbin, total, profile.outputSha256))
    {
        failure = Failure::eOutputIdentity;
        return false;
    }
    outputBytes = static_cast<uint32_t>(total);
    failure = Failure::eNone;
    return true;
}
