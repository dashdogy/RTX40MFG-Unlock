// Included by midpoint_fix.cpp. Reuse its exact temporal provider ownership,
// bounded reads, decompressor, SHA-256 and protected compare/exchange helper.
#if MFG_UNLOCK_OUTPUT_PULL_MASK_OCCUPANCY && !MFG_UNLOCK_OUTPUT_PULL_MASK_ONLY
#error OutputPull mask occupancy requires the mask-only path
#endif
#if MFG_UNLOCK_OUTPUT_PULL_MASK_ONLY && MFG_UNLOCK_OUTPUT_PULL_EXPERIMENT
#error Two OutputPull publishers cannot be enabled together
#endif

namespace
{
#include "output_pull_mask_transform.inl"

constexpr size_t kOutputPullMaskSlot = 14;
constexpr size_t kOutputPullMaskAllocationBytes = kDescriptorBytes
    + kOutputCapacity + kScratchCapacity;

OutputPullMaskSnapshot gOutputPullMaskState{};
void* gOutputPullMaskAllocation = nullptr;
std::array<uint8_t, kDescriptorBytes> gOutputPullMaskDescriptor{};

const OutputPullMaskProfile* MaskProfileForTemporal(
    const TemporalProviderProfile* profile) noexcept
{
    if (profile == &k3109TemporalProfile)
        return &kOutputPullMask3109;
    if (profile == &kLegacySlot9TemporalProfile)
        return &kOutputPullMaskLegacy;
    return nullptr;
}

bool BuildOutputPullMaskFatbin(uint8_t* fatbin, size_t capacity,
    uint8_t* scratch, size_t scratchCapacity,
    const OutputPullMaskProfile& profile, bool occupancyHint,
    uint32_t& outputBytes) noexcept
{
    outputBytes = 0;
    const size_t variant = occupancyHint ? 1 : 0;
    if (!fatbin || !scratch || profile.sourceBytes > capacity
        || profile.outputBytes[variant] > capacity
        || profile.sm89RawBytes > scratchCapacity
        || profile.sm89Offset < 120 || profile.sm89Offset > profile.sourceBytes
        || profile.sourceBytes - profile.sm89Offset < 104
        || !Sha256Equals(fatbin, profile.sourceBytes, profile.sourceSha256))
        return false;
    const uint8_t* sm120 = fatbin + 16;
    uint8_t* sm89 = fatbin + profile.sm89Offset;
    if (ReadU32(fatbin) != 0xBA55ED50u || ReadU16(fatbin + 6) != 16
        || ReadU64(fatbin + 8) + 16 != profile.sourceBytes
        || ReadU16(sm120) != 1 || ReadU32(sm120 + 4) != 104
        || ReadU32(sm120 + 28) != 120 || ReadU64(sm120 + 40) != 0x2041
        || ReadU64(sm120 + 8) != profile.sm89Offset - 120
        || ReadU16(sm89) != 1 || ReadU32(sm89 + 4) != 104
        || ReadU32(sm89 + 28) != 89 || ReadU64(sm89 + 40) != 0x2041
        || ReadU64(sm89 + 8) != profile.sm89PayloadBytes
        || ReadU32(sm89 + 16) != profile.sm89CompressedBytes
        || ReadU64(sm89 + 56) != profile.sm89RawBytes
        || profile.sm89CompressedBytes > profile.sm89PayloadBytes
        || profile.sm89PayloadBytes > profile.sourceBytes - profile.sm89Offset - 104)
        return false;
    const size_t cubinOffset = profile.sm89Offset + 104
        + static_cast<size_t>(profile.sm89PayloadBytes);
    if (cubinOffset > profile.sourceBytes || profile.sourceBytes - cubinOffset < 64)
        return false;
    const uint8_t* cubin = fatbin + cubinOffset;
    if (ReadU16(cubin) != 2 || ReadU32(cubin + 4) != 64
        || ReadU32(cubin + 28) != 89
        || ReadU64(cubin + 8) != profile.sourceBytes - cubinOffset - 64)
        return false;
    if (!DecompressNvidiaLz(sm89 + 104, profile.sm89CompressedBytes,
            scratch, profile.sm89RawBytes))
        return false;
    uint32_t ptxBytes = 0;
    if (!RewriteOutputPullMaskPtx(scratch, scratchCapacity, profile,
            occupancyHint, ptxBytes))
        return false;
    const size_t paddedBytes = (static_cast<size_t>(ptxBytes) + 7) & ~size_t(7);
    const size_t total = profile.sm89Offset + 104 + paddedBytes;
    if (total > capacity || total != profile.outputBytes[variant])
        return false;
    std::memcpy(sm89 + 104, scratch, ptxBytes);
    std::memset(sm89 + 104 + ptxBytes, 0, paddedBytes - ptxBytes);
    const uint64_t payloadBytes = paddedBytes;
    const uint64_t outerBytes = total - 16;
    constexpr uint32_t zero32 = 0;
    constexpr uint64_t zero64 = 0;
    constexpr uint64_t uncompressedFlags = 0x41;
    std::memcpy(sm89 + 8, &payloadBytes, sizeof(payloadBytes));
    std::memcpy(sm89 + 16, &zero32, sizeof(zero32));
    std::memcpy(sm89 + 40, &uncompressedFlags, sizeof(uncompressedFlags));
    std::memcpy(sm89 + 56, &zero64, sizeof(zero64));
    // Ending the outer image here excludes the original Ada cubin. Keeping it
    // would let native code bypass the changed PTX. The SM120 image is intact.
    std::memcpy(fatbin + 8, &outerBytes, sizeof(outerBytes));
    if (!Sha256Equals(fatbin, total, profile.outputSha256[variant]))
        return false;
    outputBytes = static_cast<uint32_t>(total);
    return true;
}

bool MaskTemporalPublicationCurrent(HMODULE module) noexcept
{
    uintptr_t observed = 0;
    return module && module == gProvider && gPinnedProvider == module
        && gReady.load(std::memory_order_acquire)
        && gAdapterVerified.load(std::memory_order_acquire)
        && gPublishedAdapterLuid != 0
        && gAdapterLuid.load(std::memory_order_acquire) == gPublishedAdapterLuid
        && IsReadOnlyImageAddress(module, gDescriptorEntry)
        && SafeRead(gDescriptorEntry, observed)
        && observed == gReplacementDescriptor;
}

bool MaskPublicationCurrent(HMODULE module) noexcept
{
    uintptr_t observed = 0;
    std::array<uint8_t, kDescriptorBytes> descriptor{};
    MEMORY_BASIC_INFORMATION memory{};
    return gOutputPullMaskState.ready && gOutputPullMaskState.published
        && MaskTemporalPublicationCurrent(module)
        && gOutputPullMaskState.provider == reinterpret_cast<uintptr_t>(module)
        && gOutputPullMaskState.adapterLuid == gPublishedAdapterLuid
        && gOutputPullMaskAllocation
        && VirtualQuery(gOutputPullMaskAllocation, &memory, sizeof(memory)) == sizeof(memory)
        && memory.State == MEM_COMMIT && memory.Type == MEM_PRIVATE
        && memory.AllocationBase == gOutputPullMaskAllocation
        && memory.Protect == PAGE_READONLY
        && memory.RegionSize >= kOutputPullMaskAllocationBytes
        && IsReadOnlyImageAddress(module, gOutputPullMaskState.descriptorEntry)
        && SafeRead(gOutputPullMaskState.descriptorEntry, observed)
        && observed == gOutputPullMaskState.selectedDescriptor
        && SafeCopy(descriptor.data(), reinterpret_cast<const void*>(observed), descriptor.size())
        && descriptor == gOutputPullMaskDescriptor
        && Sha256Equals(reinterpret_cast<const uint8_t*>(gOutputPullMaskState.selectedFatbin),
            gOutputPullMaskState.selectedBytes, gOutputPullMaskState.selectedSha256);
}

void MarkMaskPublicationUnsafe() noexcept
{
    gOutputPullMaskState.ready = false;
    gOutputPullMaskState.unsafePublication = true;
    gOutputPullMaskState.requiresRestart = true;
    gOutputPullMaskUnsafe.store(true, std::memory_order_release);
    gReady.store(false, std::memory_order_release);
    SetFailure(Failure::ePublication);
}
}

bool PrepareOutputPullMaskForCreate(HMODULE module, const wchar_t* path,
    OutputPullMaskCreateBoundary boundary) noexcept
{
#if !MFG_UNLOCK_OUTPUT_PULL_MASK_ONLY
    (void)module;
    (void)path;
    (void)boundary;
    return false;
#else
    std::lock_guard lock(gMutex);
    if (gOutputPullMaskState.attempted)
    {
        // A later Create is never permission to replace compiled code. Once a
        // first attempt failed, or a publication was lost, it stays closed.
        if (!gOutputPullMaskState.ready)
            return false;
        const bool current = boundary.api == gOutputPullMaskState.api
            && MaskPublicationCurrent(module);
        if (!current)
        {
            MarkMaskPublicationUnsafe();
            gOutputPullMaskState.failure = OutputPullMaskFailure::ePublicationLost;
            Log(L"OUTPUT_PULL_MASK ready=0 failure=%u reason=publication-lost",
                static_cast<unsigned>(gOutputPullMaskState.failure));
        }
        return current;
    }
    gOutputPullMaskState.attempted = true;
    gOutputPullMaskState.firstPipelineCreate = boundary.firstPipelineCreate;
    gOutputPullMaskState.pipelineMayPredateDetour = boundary.pipelineMayPredateDetour;
    gOutputPullMaskState.earlyInitObserved = boundary.earlyInitObserved;
    gOutputPullMaskState.api = boundary.api;
    gOutputPullMaskState.provider = reinterpret_cast<uintptr_t>(module);
    auto fail = [&](OutputPullMaskFailure failure) noexcept {
        gOutputPullMaskState.failure = failure;
        gOutputPullMaskState.ready = false;
        Log(L"OUTPUT_PULL_MASK variant=%hs ready=0 published=%d failure=%u "
            L"replacementWasVisible=%d unsafePublication=%d requiresRestart=%d "
            L"firstCreate=%d mayPredate=%d earlyInitObserved=%d provider=%s",
            MFG_UNLOCK_OUTPUT_PULL_MASK_OCCUPANCY ? "mask-occupancy" : "mask-only",
            gOutputPullMaskState.published, static_cast<unsigned>(failure),
            gOutputPullMaskState.replacementWasVisible,
            gOutputPullMaskState.unsafePublication, gOutputPullMaskState.requiresRestart,
            boundary.firstPipelineCreate, boundary.pipelineMayPredateDetour,
            boundary.earlyInitObserved,
            path ? path : L"(unknown)");
        return false;
    };
    if ((boundary.api != OutputPullMaskGraphicsApi::eD3D12
            && boundary.api != OutputPullMaskGraphicsApi::eVulkan)
        || !boundary.firstPipelineCreate || boundary.pipelineMayPredateDetour
        || !boundary.earlyInitObserved)
        return fail(OutputPullMaskFailure::eCreateOwnership);
    if (!MaskTemporalPublicationCurrent(module))
        return fail(OutputPullMaskFailure::eTemporalPublication);
    std::array<wchar_t, 32768> loadedPath{};
    const DWORD pathBytes = GetModuleFileNameW(module, loadedPath.data(),
        static_cast<DWORD>(loadedPath.size()));
    dlssg_provider_policy::VersionTriplet version{};
    if (pathBytes == 0 || pathBytes >= loadedPath.size()
        || !dlssg_provider_policy::ReadProviderVersion(loadedPath.data(), version))
        return fail(OutputPullMaskFailure::eProviderIdentity);
    const auto* temporal = ProfileForVersion(version);
    const auto* profile = MaskProfileForTemporal(temporal);
    if (!profile)
        return fail(OutputPullMaskFailure::eProfileNotCovered);
    const uintptr_t base = reinterpret_cast<uintptr_t>(module);
    uint32_t imageBytes = 0;
    if (!ImageSize(module, imageBytes) || imageBytes < kDescriptorBytes
        || base > UINTPTR_MAX - imageBytes
        || gDescriptorEntry < base + temporal->temporalSlot * sizeof(uintptr_t))
        return fail(OutputPullMaskFailure::eDescriptor);
    const uintptr_t end = base + imageBytes;
    const uintptr_t table = gDescriptorEntry - temporal->temporalSlot * sizeof(uintptr_t);
    if (table < base || table > end - kKernelCount * sizeof(uintptr_t))
        return fail(OutputPullMaskFailure::eDescriptor);
    const uintptr_t entry = table + kOutputPullMaskSlot * sizeof(uintptr_t);
    uintptr_t original = 0;
    std::array<uint8_t, kDescriptorBytes> descriptor{};
    if (!IsReadOnlyImageAddress(module, entry) || !SafeRead(entry, original)
        || !DescriptorMatches(base, end, original, kOutputPullMaskSlot, *temporal)
        || !SafeCopy(descriptor.data(), reinterpret_cast<const void*>(original), descriptor.size())
        || ReadU64(descriptor.data() + 32) != 256
        || ReadU64(descriptor.data() + 40) != 1)
        return fail(OutputPullMaskFailure::eDescriptor);
    const uintptr_t source = static_cast<uintptr_t>(ReadU64(descriptor.data() + 8));
    const uint32_t supplied = ReadU32(descriptor.data() + 16);
    if (profile->sourceBytes > imageBytes || source < base
        || source > end - profile->sourceBytes
        || (supplied != 0 && supplied != profile->sourceBytes))
        return fail(OutputPullMaskFailure::eSourceIdentity);
    auto* allocation = static_cast<uint8_t*>(VirtualAlloc(nullptr,
        kOutputPullMaskAllocationBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!allocation)
        return fail(OutputPullMaskFailure::eAllocation);
    uint8_t* fatbin = allocation + kDescriptorBytes;
    uint8_t* scratch = fatbin + kOutputCapacity;
    if (!SafeCopy(fatbin, reinterpret_cast<const void*>(source), profile->sourceBytes)
        || !Sha256Equals(fatbin, profile->sourceBytes, profile->sourceSha256))
    {
        VirtualFree(allocation, 0, MEM_RELEASE);
        return fail(OutputPullMaskFailure::eSourceIdentity);
    }
    gOutputPullMaskState.sourceVerified = true;
    gOutputPullMaskState.sourceBytes = profile->sourceBytes;
    gOutputPullMaskState.adapterLuid = gPublishedAdapterLuid;
    gOutputPullMaskState.descriptorEntry = entry;
    gOutputPullMaskState.originalDescriptor = original;
    gOutputPullMaskState.originalFatbin = source;
    strcpy_s(gOutputPullMaskState.sourceSha256, profile->sourceSha256);
    constexpr bool occupancyHint = MFG_UNLOCK_OUTPUT_PULL_MASK_OCCUPANCY != 0;
    constexpr size_t variant = occupancyHint ? 1 : 0;
    uint32_t outputBytes = 0;
    if (!BuildOutputPullMaskFatbin(fatbin, kOutputCapacity, scratch, kScratchCapacity,
            *profile, occupancyHint, outputBytes))
    {
        VirtualFree(allocation, 0, MEM_RELEASE);
        return fail(OutputPullMaskFailure::eTransform);
    }
    const uintptr_t selected = reinterpret_cast<uintptr_t>(allocation);
    const uintptr_t selectedFatbin = reinterpret_cast<uintptr_t>(fatbin);
    std::memcpy(allocation, descriptor.data(), descriptor.size());
    std::memcpy(allocation + 8, &selectedFatbin, sizeof(selectedFatbin));
    std::memcpy(allocation + 16, &outputBytes, sizeof(outputBytes));
    DWORD oldProtection = 0;
    if (!VirtualProtect(allocation, kOutputPullMaskAllocationBytes, PAGE_READONLY,
            &oldProtection) || !MaskTemporalPublicationCurrent(module))
    {
        VirtualFree(allocation, 0, MEM_RELEASE);
        return fail(OutputPullMaskFailure::ePublication);
    }
    MEMORY_BASIC_INFORMATION entryMemory{};
    if (VirtualQuery(reinterpret_cast<const void*>(entry), &entryMemory,
            sizeof(entryMemory)) != sizeof(entryMemory)
        || entryMemory.State != MEM_COMMIT || entryMemory.Type != MEM_IMAGE
        || entryMemory.AllocationBase != module || !IsReadOnlyImageAddress(module, entry))
    {
        VirtualFree(allocation, 0, MEM_RELEASE);
        return fail(OutputPullMaskFailure::ePublication);
    }
    // Retain candidate identities even if a protection/rollback failure makes
    // the final selection indeterminate. Optional fallback is allowed only
    // after readback proves the original pointer and exact protection restored.
    gOutputPullMaskState.selectedDescriptor = selected;
    gOutputPullMaskState.selectedFatbin = selectedFatbin;
    gOutputPullMaskState.selectedBytes = outputBytes;
    strcpy_s(gOutputPullMaskState.selectedSha256, profile->outputSha256[variant]);
    strcpy_s(gOutputPullMaskState.selectedPtxSha256, profile->outputPtxSha256[variant]);
    gOutputPullMaskState.publicationAttempted = true;
    const PublishResult published = PublishPointer(entry, original, selected);
    gOutputPullMaskState.replacementWasVisible = published.replacementWasVisible;
    if (!published.success)
    {
        uintptr_t restoredPointer = 0;
        const bool restored = IsReadOnlyImageAddress(module, entry)
            && ProtectionMatches(entry, entryMemory.Protect)
            && SafeRead(entry, restoredPointer) && restoredPointer == original;
        if (!published.replacementWasVisible && restored)
        {
            VirtualFree(allocation, 0, MEM_RELEASE);
            gOutputPullMaskState.selectedDescriptor = 0;
            gOutputPullMaskState.selectedFatbin = 0;
        }
        else
            gOutputPullMaskAllocation = allocation; // Visible or uncertain: never release it.
        if (!restored)
            MarkMaskPublicationUnsafe();
        return fail(OutputPullMaskFailure::ePublication);
    }
    gOutputPullMaskAllocation = allocation; // Provider and bytes live until process teardown.
    std::memcpy(gOutputPullMaskDescriptor.data(), allocation, gOutputPullMaskDescriptor.size());
    gOutputPullMaskState.selectedDescriptor = selected;
    gOutputPullMaskState.selectedFatbin = selectedFatbin;
    gOutputPullMaskState.selectedBytes = outputBytes;
    gOutputPullMaskState.published = true;
    gOutputPullMaskState.ready = true;
    strcpy_s(gOutputPullMaskState.selectedSha256, profile->outputSha256[variant]);
    strcpy_s(gOutputPullMaskState.selectedPtxSha256, profile->outputPtxSha256[variant]);
    if (!MaskPublicationCurrent(module))
    {
        MarkMaskPublicationUnsafe();
        return fail(OutputPullMaskFailure::ePublicationLost);
    }
    Log(L"OUTPUT_PULL_MASK variant=%hs ready=1 published=1 slot=14 api=%u "
        L"firstCreate=1 mayPredate=0 earlyInitObserved=1 entryRva=0x%zX sourceSha256=%hs "
        L"selectedSha256=%hs selectedPtxSha256=%hs minBlocksPerSm=%u "
        L"sm89CubinRemoved=1 provider=%s",
        occupancyHint ? "mask-occupancy" : "mask-only", static_cast<unsigned>(boundary.api),
        static_cast<size_t>(entry - base), profile->sourceSha256,
        profile->outputSha256[variant], profile->outputPtxSha256[variant],
        occupancyHint ? 6u : 0u, loadedPath.data());
    return true;
#endif
}

OutputPullMaskSnapshot ReadOutputPullMaskSnapshot() noexcept
{
    std::lock_guard lock(gMutex);
    return gOutputPullMaskState;
}

bool OutputPullMaskRequiresRestart() noexcept
{
#if MFG_UNLOCK_OUTPUT_PULL_MASK_ONLY
    return gOutputPullMaskUnsafe.load(std::memory_order_acquire);
#else
    return false;
#endif
}
