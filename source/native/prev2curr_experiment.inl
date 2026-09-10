// Included by midpoint_fix.cpp so the experiment uses the same bounded reads,
// decompressor, image-ownership checks, and protected compare/exchange helper.
namespace
{
#include "prev2curr_profiles.h"

constexpr size_t kPrev2CurrSlot = 18;
Prev2CurrSnapshot gPrev2CurrState{};
void* gPrev2CurrAllocation = nullptr;

const Prev2CurrProfile* Prev2CurrProfileForTemporal(
    const TemporalProviderProfile* profile) noexcept
{
    if (profile == &k3109TemporalProfile)
        return &kPrev2Curr3109;
    if (profile == &kLegacySlot9TemporalProfile)
        return &kPrev2CurrLegacy;
    return nullptr;
}

bool BuildPrev2CurrFatbin(uint8_t* fatbin, size_t capacity,
    uint8_t* scratch, size_t scratchCapacity,
    const Prev2CurrProfile& profile, uint32_t& outputBytes) noexcept
{
    outputBytes = 0;
    if (!fatbin || !scratch || capacity < profile.sourceBytes
        || capacity < profile.outputBytes
        || profile.sm120RawBytes > scratchCapacity
        || scratchCapacity - profile.sm120RawBytes < 32
        || profile.sm89Offset < 120
        || profile.sm89Offset > profile.sourceBytes
        || profile.sourceBytes - profile.sm89Offset < 104
        || !Sha256Equals(fatbin, profile.sourceBytes, profile.sourceSha256))
        return false;

    const uint8_t* sm120 = fatbin + 16;
    uint8_t* sm89 = fatbin + profile.sm89Offset;
    if (ReadU32(fatbin) != 0xBA55ED50u || ReadU16(fatbin + 6) != 16
        || ReadU64(fatbin + 8) + 16 != profile.sourceBytes
        || ReadU16(sm120) != 1 || ReadU32(sm120 + 4) != 104
        || ReadU32(sm120 + 28) != 120
        || ReadU64(sm120 + 8) != profile.sm120PayloadBytes
        || ReadU32(sm120 + 16) != profile.sm120CompressedBytes
        || ReadU64(sm120 + 40) != 0x2041
        || ReadU64(sm120 + 56) != profile.sm120RawBytes
        || 120ull + profile.sm120PayloadBytes != profile.sm89Offset
        || profile.sm120CompressedBytes > profile.sm120PayloadBytes
        || ReadU16(sm89) != 1 || ReadU32(sm89 + 4) != 104
        || ReadU32(sm89 + 28) != 89 || ReadU64(sm89 + 40) != 0x2041)
        return false;
    if (!DecompressNvidiaLz(sm120 + 104, profile.sm120CompressedBytes,
            scratch, profile.sm120RawBytes)
        || !Sha256Equals(scratch, profile.sm120RawBytes,
            profile.sourcePtxSha256))
        return false;

    constexpr char oldTarget[] = ".target sm_120";
    constexpr char newTarget[] = ".target sm_89";
    constexpr char block[] = ".maxntid 256, 1, 1";
    const size_t target = FindUniqueBytes(scratch, profile.sm120RawBytes,
        oldTarget, sizeof(oldTarget) - 1);
    if (target == SIZE_MAX)
        return false;
    std::memcpy(scratch + target, newTarget, sizeof(newTarget) - 1);
    std::memmove(scratch + target + sizeof(newTarget) - 1,
        scratch + target + sizeof(oldTarget) - 1,
        profile.sm120RawBytes - target - (sizeof(oldTarget) - 1));
    size_t ptxBytes = profile.sm120RawBytes - 1;
    const size_t blockOffset = FindUniqueBytes(scratch, ptxBytes,
        block, sizeof(block) - 1);
    if (blockOffset == SIZE_MAX)
        return false;
    if (!Sha256Equals(scratch, ptxBytes, profile.outputPtxSha256))
        return false;

    const size_t paddedPtxBytes = (ptxBytes + 7) & ~size_t(7);
    const size_t total = profile.sm89Offset + 104 + paddedPtxBytes;
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
        return false;
    outputBytes = static_cast<uint32_t>(total);
    return true;
}
}

bool PreparePrev2CurrForCreate(HMODULE module, const wchar_t* path) noexcept
{
    std::lock_guard lock(gMutex);
    // Both previous publications must still belong to this retained provider.
    uintptr_t outputPullDescriptor = 0;
    const bool outputPullCurrent = module && gOutputPullState.ready
        && gOutputPullState.provider == reinterpret_cast<uintptr_t>(module)
        && IsReadOnlyImageAddress(module, gOutputPullState.descriptorEntry)
        && SafeRead(gOutputPullState.descriptorEntry, outputPullDescriptor)
        && outputPullDescriptor == gOutputPullState.selectedDescriptor;
    if (!outputPullCurrent || !module || module != gProvider || module != gPinnedProvider
        || !gReady.load(std::memory_order_acquire)
        || !gAdapterVerified.load(std::memory_order_acquire)
        || gPublishedAdapterLuid == 0
        || gAdapterLuid.load(std::memory_order_acquire) != gPublishedAdapterLuid)
    {
        if (gPrev2CurrState.attempted)
        {
            gPrev2CurrState.ready = false;
            gPrev2CurrState.failure = 8;
        }
        return false;
    }
    if (gPrev2CurrState.attempted)
    {
        uintptr_t observed = 0;
        const bool current = gPrev2CurrState.ready
            && gPrev2CurrState.provider == reinterpret_cast<uintptr_t>(module)
            && IsReadOnlyImageAddress(module, gPrev2CurrState.descriptorEntry)
            && SafeRead(gPrev2CurrState.descriptorEntry, observed)
            && observed == gPrev2CurrState.selectedDescriptor;
        if (!current && gPrev2CurrState.ready)
        {
            gPrev2CurrState.ready = false;
            gPrev2CurrState.failure = 8;
        }
        return current;
    }
    gPrev2CurrState.attempted = true;
    gPrev2CurrState.provider = reinterpret_cast<uintptr_t>(module);
    auto fail = [&](uint32_t failure) noexcept {
        gPrev2CurrState.failure = failure;
        gPrev2CurrState.ready = false;
        Log(L"PREV2CURR variant=%hs ready=0 failure=%u provider=%s",
            MFG_UNLOCK_PREV2CURR_EXPERIMENT ? "candidate" : "baseline",
            failure, path ? path : L"(unknown)");
        return false;
    };
    dlssg_provider_policy::VersionTriplet version{};
    if (!path || !dlssg_provider_policy::ReadProviderVersion(path, version))
        return fail(1);
    const TemporalProviderProfile* temporal = ProfileForVersion(version);
    const Prev2CurrProfile* profile = Prev2CurrProfileForTemporal(temporal);
    if (!profile)
        return fail(2);
    const uintptr_t base = reinterpret_cast<uintptr_t>(module);
    uint32_t imageSize = 0;
    if (!ImageSize(module, imageSize) || base > UINTPTR_MAX - imageSize
        || gDescriptorEntry < base + temporal->temporalSlot * sizeof(uintptr_t))
        return fail(3);
    const uintptr_t end = base + imageSize;
    const uintptr_t table = gDescriptorEntry
        - temporal->temporalSlot * sizeof(uintptr_t);
    const uintptr_t entry = table + kPrev2CurrSlot * sizeof(uintptr_t);
    uintptr_t original = 0;
    if (entry < base || entry > end - sizeof(uintptr_t)
        || !IsReadOnlyImageAddress(module, entry)
        || !SafeRead(entry, original)
        || !DescriptorMatches(base, end, original, kPrev2CurrSlot, *temporal))
        return fail(3);
    std::array<uint8_t, kDescriptorBytes> descriptor{};
    if (!SafeCopy(descriptor.data(), reinterpret_cast<const void*>(original),
            descriptor.size()))
        return fail(3);
    // The retargeted program is certified only for this initialized shape.
    if (ReadU32(descriptor.data() + 32) != 256
        || ReadU32(descriptor.data() + 36) != 1
        || ReadU32(descriptor.data() + 40) != 1)
        return fail(3);
    uintptr_t source = 0;
    uint32_t supplied = 0;
    std::memcpy(&source, descriptor.data() + 8, sizeof(source));
    std::memcpy(&supplied, descriptor.data() + 16, sizeof(supplied));
    if (profile->sourceBytes > imageSize || source < base
        || source > end - profile->sourceBytes
        || (supplied != 0 && supplied != profile->sourceBytes))
        return fail(4);

    constexpr size_t allocationBytes = kDescriptorBytes
        + kOutputCapacity + kScratchCapacity;
    uint8_t* allocation = static_cast<uint8_t*>(VirtualAlloc(nullptr,
        allocationBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!allocation)
        return fail(5);
    uint8_t* fatbin = allocation + kDescriptorBytes;
    uint8_t* scratch = fatbin + kOutputCapacity;
    std::memcpy(allocation, descriptor.data(), descriptor.size());
    if (!SafeCopy(fatbin, reinterpret_cast<const void*>(source),
            profile->sourceBytes)
        || !Sha256Equals(fatbin, profile->sourceBytes, profile->sourceSha256))
    {
        VirtualFree(allocation, 0, MEM_RELEASE);
        return fail(4);
    }
    gPrev2CurrState.sourceVerified = true;
    gPrev2CurrState.sourceBytes = profile->sourceBytes;
    gPrev2CurrState.descriptorEntry = entry;
    gPrev2CurrState.originalDescriptor = original;
    gPrev2CurrState.originalFatbin = source;
    strcpy_s(gPrev2CurrState.sourceSha256, profile->sourceSha256);

#if MFG_UNLOCK_PREV2CURR_EXPERIMENT
    uint32_t outputBytes = 0;
    if (!BuildPrev2CurrFatbin(fatbin, kOutputCapacity, scratch,
            kScratchCapacity, *profile, outputBytes))
    {
        VirtualFree(allocation, 0, MEM_RELEASE);
        return fail(6);
    }
    const uintptr_t selected = reinterpret_cast<uintptr_t>(allocation);
    const uintptr_t selectedFatbin = reinterpret_cast<uintptr_t>(fatbin);
    std::memcpy(allocation + 8, &selectedFatbin, sizeof(selectedFatbin));
    std::memcpy(allocation + 16, &outputBytes, sizeof(outputBytes));
    DWORD oldProtection = 0;
    if (!VirtualProtect(allocation, allocationBytes, PAGE_READONLY,
            &oldProtection)
        || !gAdapterVerified.load(std::memory_order_acquire)
        || gAdapterLuid.load(std::memory_order_acquire) != gPublishedAdapterLuid)
    {
        VirtualFree(allocation, 0, MEM_RELEASE);
        return fail(7);
    }
    const PublishResult result = PublishPointer(entry, original, selected);
    if (!result.success)
    {
        if (!result.replacementWasVisible)
            VirtualFree(allocation, 0, MEM_RELEASE);
        else
            gPrev2CurrAllocation = allocation; // May already be cached.
        return fail(7);
    }
    gPrev2CurrAllocation = allocation; // Retained until process teardown.
    gPrev2CurrState.selectedDescriptor = selected;
    gPrev2CurrState.selectedFatbin = selectedFatbin;
    gPrev2CurrState.activeBytes = outputBytes;
    gPrev2CurrState.published = true;
    strcpy_s(gPrev2CurrState.selectedSha256, profile->outputSha256);
    strcpy_s(gPrev2CurrState.selectedPtxSha256, profile->outputPtxSha256);
#else
    VirtualFree(allocation, 0, MEM_RELEASE);
    gPrev2CurrState.selectedDescriptor = original;
    gPrev2CurrState.selectedFatbin = source;
    gPrev2CurrState.activeBytes = profile->sourceBytes;
    strcpy_s(gPrev2CurrState.selectedSha256, profile->sourceSha256);
#endif
    gPrev2CurrState.ready = true;
    Log(L"PREV2CURR variant=%hs ready=1 published=%d slot=18 "
        L"entryRva=0x%zX sourceSha256=%hs selectedSha256=%hs "
        L"minBlocksPerSm=%u provider=%s",
        MFG_UNLOCK_PREV2CURR_EXPERIMENT ? "candidate" : "baseline",
        gPrev2CurrState.published, static_cast<size_t>(entry - base),
        profile->sourceSha256, gPrev2CurrState.selectedSha256,
        0u, path);
    return true;
}

Prev2CurrSnapshot ReadPrev2CurrSnapshot() noexcept
{
    std::lock_guard lock(gMutex);
    return gPrev2CurrState;
}
