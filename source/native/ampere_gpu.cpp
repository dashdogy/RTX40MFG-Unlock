#include "ampere_gpu.h"
#include "dlssg_provider_policy.h"
#include "protected_pointer.h"
#include "ampere_native_cache.h"
#include "ampere_cuda_program.h"
#include "ampere_diagnostics.h"
#include "adapter_discovery.h"
#include "ampere_mask_transform.h"

#include <bcrypt.h>
#include <d3d12.h>
#include <dxgi.h>
#include <psapi.h>
#include "ampere_policy.h"
#include <vector>
#include <string_view>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <span>
#include <memory>

namespace ampere_gpu
{
namespace
{
constexpr size_t kKernelCount = 25;
constexpr size_t kMaximumKernelCount = ampere_native_cache::kMaximumSlots;
constexpr size_t kDescriptorBytes = 48;
constexpr size_t kNameBytes = 96;
constexpr uint64_t kMaximumFatbinBytes = 64ull * 1024ull * 1024ull;
#include "ampere_font_program.h"
// Architecture-independent temporal inputs match Universal v1.2. Only SM89
// PTX is translated; no SM120 entry is retained in the Ampere program.
constexpr size_t kTemporalGrowthAllowance = 512;
struct TemporalProviderProfile
{
    size_t temporalSlot = 0;
    bool namedKernelDescriptors = false;
    uint64_t sourceFatbinBytes = 0;
    const char* sourceFatbinSha256 = nullptr;
    const char* sourcePtxSha256 = nullptr;
    const char* parameter = nullptr;
    const char* currentScale = nullptr;
    const char* previousScale = nullptr;
    bool extraRegisters = false;
};
constexpr TemporalProviderProfile kEarlyTemporalProfile{
    12, false, 197560,
    "25E24B204A58BB5E2757199B1C856B5FDE7D26B9967FFEF82FD794BDE4BCBC67",
    "946A4CCE7B2E43BD0F6228EBCC99F85B9EF03F64D4A5F6D19097FA502BF912CB",
    "main_kernel_param_5", "%f4012", "%f4010", true};
constexpr TemporalProviderProfile LegacyTemporalProfile(size_t temporalSlot)
{
    return {temporalSlot, false, 98408,
        "5A8E0284AAB8AC14FC82B0504BBEEF25D2FCE1D13A1C11D8BDB3F91FEE8145FC",
        "46C05996A2EF199BBE39378681734ED5EE757655DE70410D9900D85BA91222F1",
        "main_kernel_param_0+32", "%f136", "%f134", false};
}
constexpr auto kLegacySlot11TemporalProfile = LegacyTemporalProfile(11);
constexpr auto kLegacySlot10TemporalProfile = LegacyTemporalProfile(10);
constexpr auto kLegacySlot9TemporalProfile = LegacyTemporalProfile(9);
constexpr TemporalProviderProfile k3109TemporalProfile{
    9, true, 98704,
    "FBA7599CC9CDC1EED947AD5ADB94436052E8C17AD7FD268C2590AB36A0C731E9",
    "D1EB7A60C57915238AA228511DA16386F95AAAA5A0D1C1B7903F1DA8FB9519B4",
    "Kernel_EstimateIntermMvecsScatter_param_0+32", "%f136", "%f134", false};

constexpr std::array<const char*, kMaximumKernelCount> k3109KernelNames{{
    "BlendCandidatesFused",
    "Copy4Channel",
    "DL1Net_Input",
    "DL1Net_Output",
    "DetectMenus",
    "DetectMenusHudless",
    "Distortion",
    "DownsampleDistortion",
    "DownsampleRGBAndEstimateUIFused",
    "EstimateIntermMvecsScatter",
    "EstimatePrev2CurrScatter",
    "InitMvecQualityMask",
    "InputMvecProcessing",
    "MenuDetectionOutput",
    "OutputPull",
    "OutputPullMiddle",
    "OutputPush",
    "OutputPushFine",
    "Prev2CurrUnpackPull",
    "Prev2CurrPullMiddle",
    "Prev2CurrPush",
    "Prev2CurrPushFine",
    "Scatter3d",
    "WarpBlendingWeights",
    "ZeroBuffer",
    "CaptureBuffer",
    "CaptureResource",
    "DbgIntermMotion",
    "RateBox",
    "Vis_Copy",
    "Vis_IsDynamic",
    "Vis_Alignment",
    "Vis_Thumbnail",
    "Vis_MotionVectorResample",
    "Vis_CopyArbitrarySize",
    "Vis_UI",
    "Vis_UIHint",
    "Vis_DrawVectors",
}};

constexpr const TemporalProviderProfile* ProfileForVersion(
    dlssg_provider_policy::VersionTriplet version) noexcept
{
    if (version.major == 310
        && ((version.minor == 1 && version.build == 0)
            || (version.minor == 2
                && (version.build == 0 || version.build == 1))
            || (version.minor == 3 && version.build == 0)))
        return &kEarlyTemporalProfile;
    if (version.major == 310
        && ((version.minor == 4 && version.build == 0)
            || (version.minor == 5
                && (version.build == 0 || version.build == 2
                    || version.build == 3))))
        return &kLegacySlot11TemporalProfile;
    if (version.major == 310 && version.minor == 6 && version.build == 0)
        return &kLegacySlot10TemporalProfile;
    // 310.9.1 preserves all 25 source payloads and their native-cache mapping.
    if (version.major == 310 && version.minor == 9
        && (version.build == 0 || version.build == 1))
        return &k3109TemporalProfile;
    if (version.major == 310
        && ((version.minor == 7
                && (version.build == 0 || version.build == 128
                    || version.build == 129))
            || (version.minor == 8 && version.build == 0)))
        return &kLegacySlot9TemporalProfile;
    return nullptr;
}

static_assert([] {
    for (const auto version : dlssg_provider_policy::kSupportedVersions)
        if (!ProfileForVersion(version)) return false;
    return ProfileForVersion({0, 0, 0}) == nullptr
        && ProfileForVersion({310, 9, 2}) == nullptr;
}(), "Every eligible DLSS-G version needs an explicit Ampere temporal profile");

static_assert(kEarlyTemporalProfile.temporalSlot == 12);
static_assert(kLegacySlot11TemporalProfile.temporalSlot == 11);
static_assert(kLegacySlot10TemporalProfile.temporalSlot == 10);
static_assert(kLegacySlot9TemporalProfile.temporalSlot == 9);
static_assert(k3109TemporalProfile.temporalSlot == 9);

enum class Failure : uint32_t
{
    eNone = 0,
    eAdapterUnavailable = 1,
    eAdapterNotAmpere = 2,
    eProviderVersion = 3,
    eProviderLayout = 4,
    eSourceIdentity = 5,
    eDecompression = 6,
    eTemporalLayout = 7,
    eOutputIdentity = 8,
    eAllocation = 9,
    ePublication = 10,
    eRestartRequired = 11,
    eProviderNotReady = 12,
    eNativeProgram = 13,
    eCudaProgram = 14,
    eEarlyPreparation = 15,
    eFontProgram = 16,
};

struct PublishResult
{
    bool success = false;
    bool replacementWasVisible = false;
};

std::atomic<LogCallback> gLogCallback{nullptr};
std::atomic<bool> gAdapterVerified{false};
std::atomic<bool> gReady{false};
std::atomic<uint32_t> gKernelImage{0};
std::atomic<uint32_t> gNativeCacheStatus{0};
using Preparation = ampere_diagnostics::Preparation;
std::atomic<Preparation> gPreparation{Preparation::ePending};
std::atomic<uint32_t> gFailure{
    static_cast<uint32_t>(Failure::eAdapterUnavailable)};
std::atomic<uint64_t> gAdapterLuid{0};
std::mutex gMutex;
HMODULE gProvider = nullptr;
HMODULE gPinnedProvider = nullptr;
uint64_t gPublishedAdapterLuid = 0;
uintptr_t gDescriptorTable = 0;
std::array<uintptr_t, kMaximumKernelCount> gOriginalDescriptors{};
std::array<uintptr_t, kMaximumKernelCount> gReplacementDescriptors{};
bool gPublicationFailed = false;
struct PublishedProgram
{
    HMODULE module = nullptr;
    HMODULE pinned = nullptr;
    uint64_t luid = 0;
    uintptr_t table = 0;
    std::array<uintptr_t, kMaximumKernelCount> originals{}, replacements{};
    uint32_t kernelImage = 0, cacheStatus = 0;
    PreparationBoundary boundary{};
    std::array<uint32_t, kMaximumKernelCount> allocationBytes{};
    std::array<std::array<uint8_t, kDescriptorBytes>, kMaximumKernelCount> descriptors{};
    size_t slotCount = kKernelCount;
    std::shared_ptr<FontProgram> font;
};
// Discovery may see a local provider and several OTA candidates before NGX
// chooses one. Each owns a separate immutable program. The first real nested
// provider Create binds the process; discovery alone cannot select or revoke it.
std::array<PublishedProgram, 16> gPrograms{};
std::atomic<const PublishedProgram*> gSelectedProgram{nullptr};
size_t gProgramCount = 0;
bool gProviderBound = false;

void Log(const wchar_t* format, ...) noexcept
{
    const LogCallback callback = gLogCallback.load(std::memory_order_acquire);
    if (!callback)
        return;
    wchar_t message[1024]{};
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(message, _countof(message), _TRUNCATE, format, args);
    va_end(args);
    callback(message);
}

bool SafeCopy(void* destination, const void* source, size_t bytes) noexcept
{
    if (!destination || !source || bytes == 0)
        return false;
    __try
    {
        std::memcpy(destination, source, bytes);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

template <typename T>
bool SafeRead(uintptr_t address, T& value) noexcept
{
    return SafeCopy(&value, reinterpret_cast<const void*>(address),
        sizeof(value));
}

uint16_t ReadU16(const uint8_t* bytes) noexcept
{
    uint16_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

uint32_t ReadU32(const uint8_t* bytes) noexcept
{
    uint32_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

uint64_t ReadU64(const uint8_t* bytes) noexcept
{
    uint64_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

bool Sha256Equals(const uint8_t* bytes, size_t count,
    const char* expected) noexcept
{
    if (!bytes || count == 0 || count > ULONG_MAX || !expected
        || std::strlen(expected) != 64)
        return false;

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    std::array<uint8_t, 32> digest{};
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm,
        BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status >= 0)
    {
        status = BCryptHash(algorithm, nullptr, 0,
            const_cast<PUCHAR>(bytes), static_cast<ULONG>(count),
            digest.data(), static_cast<ULONG>(digest.size()));
    }
    if (algorithm)
        BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0)
        return false;

    static constexpr char kDigits[] = "0123456789ABCDEF";
    for (size_t index = 0; index < digest.size(); ++index)
    {
        if (expected[index * 2] != kDigits[digest[index] >> 4]
            || expected[index * 2 + 1] != kDigits[digest[index] & 0x0F])
            return false;
    }
    return true;
}

bool ImageSize(HMODULE module, uint32_t& imageSize) noexcept
{
    imageSize = 0;
    const uintptr_t base = reinterpret_cast<uintptr_t>(module);
    IMAGE_DOS_HEADER dos{};
    if (!base || !SafeRead(base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE
        || dos.e_lfanew <= 0
        || static_cast<uintptr_t>(dos.e_lfanew) > UINTPTR_MAX - base)
        return false;
    IMAGE_NT_HEADERS64 nt{};
    if (!SafeRead(base + static_cast<uintptr_t>(dos.e_lfanew), nt)
        || nt.Signature != IMAGE_NT_SIGNATURE
        || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC
        || nt.OptionalHeader.SizeOfImage < sizeof(nt))
        return false;
    imageSize = nt.OptionalHeader.SizeOfImage;
    return true;
}

bool IsReadOnlyImageAddress(HMODULE module, uintptr_t address) noexcept
{
    MEMORY_BASIC_INFORMATION memory{};
    if (!module || !address
        || VirtualQuery(reinterpret_cast<const void*>(address), &memory,
            sizeof(memory)) != sizeof(memory)
        || memory.State != MEM_COMMIT || memory.Type != MEM_IMAGE
        || memory.AllocationBase != module
        || (memory.Protect & (PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE))
        || address < reinterpret_cast<uintptr_t>(memory.BaseAddress)
        || memory.RegionSize < sizeof(uintptr_t)
        || address - reinterpret_cast<uintptr_t>(memory.BaseAddress)
            > memory.RegionSize - sizeof(uintptr_t))
        return false;
    const DWORD protection = memory.Protect & 0xFFu;
    return protection == PAGE_READONLY || protection == PAGE_EXECUTE_READ;
}

bool ImmutableCloneCurrent(const PublishedProgram& program, size_t slot,
    const std::vector<uint8_t>* payload = nullptr) noexcept
{
    if (program.slotCount < kKernelCount || program.slotCount > kMaximumKernelCount
        || slot >= program.slotCount) return false;
    const uintptr_t clone = program.replacements[slot];
    const size_t bytes = program.allocationBytes[slot];
    MEMORY_BASIC_INFORMATION memory{};
    if (!clone || bytes <= kDescriptorBytes
        || VirtualQuery(reinterpret_cast<const void*>(clone), &memory, sizeof(memory)) != sizeof(memory)
        || memory.AllocationBase != reinterpret_cast<const void*>(clone)
        || memory.BaseAddress != reinterpret_cast<const void*>(clone)
        || memory.State != MEM_COMMIT || memory.Type != MEM_PRIVATE
        || memory.Protect != PAGE_READONLY || memory.RegionSize < bytes)
        return false;
    std::array<uint8_t, kDescriptorBytes> observed{};
    if (!SafeCopy(observed.data(), reinterpret_cast<const void*>(clone), observed.size())
        || observed != program.descriptors[slot]) return false;
    uintptr_t image = 0;
    uint32_t imageBytes = 0;
    memcpy(&image, observed.data() + 8, sizeof(image));
    memcpy(&imageBytes, observed.data() + 16, sizeof(imageBytes));
    if (image != clone + kDescriptorBytes || imageBytes != bytes - kDescriptorBytes)
        return false;
    if (!payload) return true;
    if (payload->size() != imageBytes) return false;
    __try
    {
        return memcmp(reinterpret_cast<const void*>(image), payload->data(), imageBytes) == 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

PublishedProgram* FindPublishedProgram(HMODULE module) noexcept
{
    for (size_t i = 0; i < gProgramCount; ++i)
        if (gPrograms[i].module == module) return &gPrograms[i];
    return nullptr;
}

bool ProgramCurrent(const PublishedProgram& program, uint64_t luid) noexcept
{
    if (!luid || program.luid != luid || !program.boundary.Proven()
        || !program.font || program.font->module != program.module || !FontCurrent(*program.font, true)
        || program.slotCount < kKernelCount || program.slotCount > kMaximumKernelCount) return false;
    for (size_t i = 0; i < program.slotCount; ++i)
    {
        uintptr_t observed = 0;
        if (!SafeRead(program.table + i * sizeof(uintptr_t), observed)
            || observed != program.replacements[i]
            || !IsReadOnlyImageAddress(program.module, program.table + i * sizeof(uintptr_t))
            || !ImmutableCloneCurrent(program, i))
            return false;
    }
    return true;
}

void SelectProgram(const PublishedProgram& program) noexcept
{
    gProvider = program.module;
    gPinnedProvider = program.pinned;
    gPublishedAdapterLuid = program.luid;
    gDescriptorTable = program.table;
    gOriginalDescriptors = program.originals;
    gReplacementDescriptors = program.replacements;
    gKernelImage.store(program.kernelImage, std::memory_order_release);
    gNativeCacheStatus.store(program.cacheStatus, std::memory_order_release);
    gSelectedProgram.store(&program, std::memory_order_release);
    gReady.store(true, std::memory_order_release);
}

bool KernelIdentityMatches(const TemporalProviderProfile& profile, size_t slot,
    const char* descriptorName, const char* entryName) noexcept
{
    if (!descriptorName || !entryName || slot >= kMaximumKernelCount)
        return false;
    if (!profile.namedKernelDescriptors)
    {
        return std::strcmp(descriptorName, "dlfg_kernel") == 0
            && std::strcmp(entryName, "main_kernel") == 0;
    }

    constexpr char kEntryPrefix[] = "Kernel_";
    const char* const expected = k3109KernelNames[slot];
    return expected && std::strcmp(descriptorName, expected) == 0
        && std::strncmp(entryName, kEntryPrefix,
            sizeof(kEntryPrefix) - 1) == 0
        && std::strcmp(entryName + sizeof(kEntryPrefix) - 1, expected) == 0;
}

enum class DescriptorFailure : uint32_t
{
    eNone, eBounds, eUnreadable, eName, eHeader, eSize, eTemporalIdentity, eRegistration
};
struct DescriptorCheck
{
    DescriptorFailure failure = DescriptorFailure::eBounds;
    bool named = false;
    uint32_t supplied = 0;
    uint64_t bytes = 0;
};
struct DescriptorDiscovery
{
    uint32_t matches = 0, namedReferences = 0, validSlots = 0;
    size_t failedSlot = SIZE_MAX;
    uintptr_t candidate = 0;
    size_t registeredSlots = 0;
    uintptr_t registration = 0;
    DescriptorCheck check{};
};
const wchar_t* DescriptorFailureName(DescriptorFailure failure) noexcept
{
    switch (failure)
    {
    case DescriptorFailure::eNone: return L"none";
    case DescriptorFailure::eBounds: return L"descriptor-bounds";
    case DescriptorFailure::eUnreadable: return L"descriptor-unreadable";
    case DescriptorFailure::eName: return L"kernel-name-or-entry";
    case DescriptorFailure::eHeader: return L"fatbin-header-or-ownership";
    case DescriptorFailure::eSize: return L"fatbin-size";
    case DescriptorFailure::eTemporalIdentity: return L"temporal-source-identity";
    case DescriptorFailure::eRegistration: return L"registration-loop-contract";
    default: return L"unknown";
    }
}

bool DescriptorMatches(uintptr_t moduleBase, uintptr_t moduleEnd,
    uintptr_t descriptor, size_t slot,
    const TemporalProviderProfile& profile, DescriptorCheck* detail = nullptr) noexcept
{
    DescriptorCheck result{};
    struct ReportCheck { DescriptorCheck* output; DescriptorCheck& result;
        ~ReportCheck() { if (output) *output = result; } } report{detail, result};
    if (!moduleBase || moduleEnd <= moduleBase
        || descriptor < moduleBase || moduleEnd - moduleBase < kDescriptorBytes
        || descriptor > moduleEnd - kDescriptorBytes)
        return false;

    std::array<uint8_t, kDescriptorBytes> bytes{};
    result.failure = DescriptorFailure::eUnreadable;
    if (!SafeCopy(bytes.data(), reinterpret_cast<const void*>(descriptor),
            bytes.size()))
        return false;
    uintptr_t name = 0;
    uintptr_t fatbin = 0;
    uintptr_t entry = 0;
    uint32_t suppliedSize = 0;
    std::memcpy(&name, bytes.data(), sizeof(name));
    std::memcpy(&fatbin, bytes.data() + 0x08, sizeof(fatbin));
    std::memcpy(&suppliedSize, bytes.data() + 0x10, sizeof(suppliedSize));
    std::memcpy(&entry, bytes.data() + 0x18, sizeof(entry));

    char descriptorName[kNameBytes]{};
    char entryName[kNameBytes]{};
    result.failure = DescriptorFailure::eName;
    if (!name || !entry || !fatbin || moduleEnd - moduleBase < kNameBytes
        || name < moduleBase || name > moduleEnd - kNameBytes
        || entry < moduleBase || entry > moduleEnd - kNameBytes
        || fatbin < moduleBase || fatbin > moduleEnd - 16
        || !SafeCopy(descriptorName, reinterpret_cast<const void*>(name),
            sizeof(descriptorName) - 1)
        || !SafeCopy(entryName, reinterpret_cast<const void*>(entry),
            sizeof(entryName) - 1)
        || !KernelIdentityMatches(profile, slot, descriptorName, entryName))
        return false;
    result.named = true;
    result.supplied = suppliedSize;
    result.failure = DescriptorFailure::eHeader;

    std::array<uint8_t, 16> header{};
    if (!SafeCopy(header.data(), reinterpret_cast<const void*>(fatbin),
            header.size())
        || ReadU32(header.data()) != 0xBA55ED50u)
        return false;
    const uint16_t headerSize = ReadU16(header.data() + 6);
    const uint64_t payloadSize = ReadU64(header.data() + 8);
    const uint64_t totalSize = static_cast<uint64_t>(headerSize) + payloadSize;
    result.bytes = totalSize;
    result.failure = DescriptorFailure::eSize;
    if (headerSize < header.size() || totalSize < headerSize
        || totalSize > kMaximumFatbinBytes
        || totalSize > static_cast<uint64_t>(moduleEnd - fatbin)
        || (suppliedSize != 0 && suppliedSize != totalSize))
        return false;
    result.failure = DescriptorFailure::eTemporalIdentity;
    if (slot == profile.temporalSlot && (totalSize != profile.sourceFatbinBytes
        || !Sha256Equals(reinterpret_cast<const uint8_t*>(fatbin),
            static_cast<size_t>(totalSize), profile.sourceFatbinSha256))) return false;
    result.failure = DescriptorFailure::eNone;
    return true;
}

bool DescriptorTableMatches(uintptr_t moduleBase, uintptr_t moduleEnd,
    uintptr_t table, const TemporalProviderProfile& profile,
    DescriptorDiscovery* detail = nullptr, size_t slotCount = kKernelCount) noexcept
{
    if (slotCount < kKernelCount || slotCount > kMaximumKernelCount) return false;
    const size_t kTableBytes = slotCount * sizeof(uintptr_t);
    if (!table || moduleEnd <= moduleBase || table < moduleBase
        || moduleEnd - moduleBase < kTableBytes || table > moduleEnd - kTableBytes)
        return false;
    bool valid = true;
    for (size_t slot = 0; slot < slotCount; ++slot)
    {
        uintptr_t descriptor = 0;
        DescriptorCheck check{};
        if (!SafeRead(table + slot * sizeof(uintptr_t), descriptor)
            || !DescriptorMatches(moduleBase, moduleEnd, descriptor, slot,
                profile, &check))
        {
            if (!detail) return false;
            if (valid) { detail->failedSlot = slot; detail->check = check; }
            valid = false;
        }
        else if (detail) ++detail->validSlots;
    }
    return valid;
}

#include "ampere_registration_profiles.inl"

bool RegistrationCountMatches(const TemporalProviderProfile& profile, size_t count) noexcept
{
    if (profile.temporalSlot == 12) return count == 28 || count == 33;
    if (profile.temporalSlot == 11) return count == 27 || count == 38;
    if (profile.temporalSlot == 10) return count == 26;
    return profile.temporalSlot == 9 && (count == 25 || count == 38);
}

bool ImageMemorySpan(uintptr_t base, uintptr_t address, size_t bytes, DWORD protection) noexcept
{
    if (!bytes || address > UINTPTR_MAX - bytes) return false;
    const uintptr_t end = address + bytes;
    for (uintptr_t at = address; at < end;)
    {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<const void*>(at), &memory, sizeof(memory)) != sizeof(memory)
            || memory.State != MEM_COMMIT || memory.Type != MEM_IMAGE
            || reinterpret_cast<uintptr_t>(memory.AllocationBase) != base
            || memory.Protect != protection) return false;
        const uintptr_t begin = reinterpret_cast<uintptr_t>(memory.BaseAddress);
        if (begin > at || memory.RegionSize == 0 || begin > UINTPTR_MAX - memory.RegionSize
            || begin + memory.RegionSize <= at) return false;
        at = std::min(end, begin + memory.RegionSize);
    }
    return true;
}

bool ImageSectionSpan(uintptr_t base, uint32_t imageSize, size_t sections,
    uint16_t sectionCount, uintptr_t address, size_t bytes, bool executable) noexcept
{
    if (!bytes || address < base || bytes > imageSize || address - base > imageSize - bytes)
        return false;
    for (uint16_t i = 0; i < sectionCount; ++i)
    {
        IMAGE_SECTION_HEADER section{};
        if (!SafeRead(base + sections + i * sizeof(section), section)) return false;
        if (!(section.Characteristics & IMAGE_SCN_MEM_READ)
            || (section.Characteristics & IMAGE_SCN_MEM_WRITE)
            || bool(section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != executable
            || section.VirtualAddress >= imageSize) continue;
        const size_t size = std::min<size_t>(imageSize - section.VirtualAddress,
            std::max<size_t>(section.Misc.VirtualSize, section.SizeOfRawData));
        const uintptr_t begin = base + section.VirtualAddress;
        if (address >= begin && bytes <= size && address - begin <= size - bytes)
            return ImageMemorySpan(base, address, bytes, executable ? PAGE_EXECUTE_READ : PAGE_READONLY);
    }
    return false;
}

bool DecodeImageRelative(uintptr_t instructionBase, size_t displacementOffset,
    int32_t displacement, uintptr_t imageBase, uint32_t imageSize, uintptr_t& target) noexcept
{
    const uint64_t offset = instructionBase - imageBase + displacementOffset + sizeof(displacement);
    if (instructionBase < imageBase || offset > imageSize) return false;
    const int64_t relative = static_cast<int64_t>(offset) + displacement;
    if (relative < 0 || static_cast<uint64_t>(relative) >= imageSize) return false;
    target = imageBase + static_cast<uintptr_t>(relative);
    return true;
}

uintptr_t FindDescriptorEntry(HMODULE module, uint32_t imageSize,
    const TemporalProviderProfile& profile, DescriptorDiscovery* detail = nullptr) noexcept
{
    if (!module || imageSize < kKernelCount * sizeof(uintptr_t))
        return 0;
    const uintptr_t base = reinterpret_cast<uintptr_t>(module);
    if (base > UINTPTR_MAX - imageSize)
        return 0;
    const uintptr_t end = base + imageSize;

    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!SafeRead(base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE
        || dos.e_lfanew <= 0
        || static_cast<uintptr_t>(dos.e_lfanew) > UINTPTR_MAX - base
        || !SafeRead(base + static_cast<uintptr_t>(dos.e_lfanew), nt)
        || nt.Signature != IMAGE_NT_SIGNATURE
        || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return 0;
    const size_t sectionOffset = static_cast<size_t>(dos.e_lfanew)
        + offsetof(IMAGE_NT_HEADERS64, OptionalHeader)
        + nt.FileHeader.SizeOfOptionalHeader;
    if (sectionOffset > imageSize
        || nt.FileHeader.NumberOfSections
            > (imageSize - sectionOffset) / sizeof(IMAGE_SECTION_HEADER))
        return 0;

    // The temporal anchor alone proves only a prefix. Bind that table to the
    // provider's complete, unwind-owned registration loop before choosing its
    // extent. Earlier releases register 26-28 slots; development builds can
    // register 33 or 38. Nothing is inferred from adjacent pointers or a version.
    const auto& directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (directory.Size == 0 || directory.Size % sizeof(RUNTIME_FUNCTION)
        || directory.Size / sizeof(RUNTIME_FUNCTION) > 65536
        || !ImageSectionSpan(base, imageSize, sectionOffset, nt.FileHeader.NumberOfSections,
            base + directory.VirtualAddress, directory.Size, false)) return 0;
    uintptr_t match = 0;
    for (size_t index = 0; index < directory.Size / sizeof(RUNTIME_FUNCTION); ++index)
    {
        RUNTIME_FUNCTION function{};
        if (!SafeRead(base + directory.VirtualAddress + index * sizeof(function), function)) return 0;
        if (function.EndAddress <= function.BeginAddress || function.EndAddress > imageSize) continue;
        const size_t bytes = function.EndAddress - function.BeginAddress;
        const uintptr_t begin = base + function.BeginAddress;
        for (const auto& pattern : kRegistrationPatterns)
        {
            if (bytes != pattern.bytes.size()
                || !ImageSectionSpan(base, imageSize, sectionOffset, nt.FileHeader.NumberOfSections,
                    begin, bytes, true)) continue;
            DWORD64 owner = 0, endOwner = 0;
            const auto* const expectedFunction = reinterpret_cast<const RUNTIME_FUNCTION*>(
                base + directory.VirtualAddress + index * sizeof(function));
            const auto* const runtimeFunction = RtlLookupFunctionEntry(begin, &owner, nullptr);
            const auto* const endFunction = RtlLookupFunctionEntry(begin + bytes - 1, &endOwner, nullptr);
            if (owner != base || endOwner != base || runtimeFunction != expectedFunction
                || endFunction != expectedFunction
                || !ImageSectionSpan(base, imageSize, sectionOffset, nt.FileHeader.NumberOfSections,
                    base + function.UnwindData, pattern.unwind.size(), false)) continue;
            std::array<uint8_t, 24> unwind{};
            if (pattern.unwind.size() != unwind.size()
                || !SafeCopy(unwind.data(), reinterpret_cast<const void*>(base + function.UnwindData), unwind.size())
                || !std::equal(unwind.begin(), unwind.end(), pattern.unwind.begin())) continue;
            std::array<uint8_t, sizeof(kRegistrationBody0)> observed{};
            if (!SafeCopy(observed.data(), reinterpret_cast<const void*>(begin), bytes)) return 0;
            bool shape = true;
            for (size_t at = 0; shape && at < bytes; ++at)
            {
                const auto relative = [&](const auto& offsets) {
                    return std::any_of(offsets.begin(), offsets.end(), [&](size_t offset) {
                        return at >= offset && at - offset < sizeof(int32_t); }); };
                if (relative(pattern.imageOperands) || relative(pattern.calls)
                    || std::find(pattern.counts.begin(), pattern.counts.end(), at) != pattern.counts.end()) continue;
                shape = observed[at] == pattern.bytes[at];
            }
            if (!shape) continue;
            const size_t count = observed[pattern.counts.back()];
            if (!RegistrationCountMatches(profile, count)
                || std::any_of(pattern.counts.begin(), pattern.counts.end(), [&](size_t at) {
                    return observed[at] != count; })) continue;
            uintptr_t table = 0;
            for (const auto offset : pattern.imageOperands)
            {
                int32_t displacement = 0; memcpy(&displacement, observed.data() + offset, sizeof(displacement));
                uintptr_t target = 0;
                if (!DecodeImageRelative(begin, offset, displacement, base, imageSize, target)
                    || !ImageSectionSpan(base, imageSize, sectionOffset, nt.FileHeader.NumberOfSections,
                        target, sizeof(uintptr_t), false)) { shape = false; break; }
                if (offset == pattern.tableOffset) table = target;
            }
            for (const auto offset : pattern.calls)
            {
                int32_t displacement = 0; memcpy(&displacement, observed.data() + offset, sizeof(displacement));
                uintptr_t target = 0;
                if (!DecodeImageRelative(begin, offset, displacement, base, imageSize, target)
                    || !ImageSectionSpan(base, imageSize, sectionOffset, nt.FileHeader.NumberOfSections,
                        target, 1, true)) { shape = false; break; }
                DWORD64 callOwner = 0;
                const auto* const callFunction = RtlLookupFunctionEntry(target, &callOwner, nullptr);
                const uintptr_t callRecord = reinterpret_cast<uintptr_t>(callFunction);
                const uintptr_t directoryBegin = base + directory.VirtualAddress;
                RUNTIME_FUNCTION callBounds{};
                if (callOwner != base || callRecord < directoryBegin
                    || callRecord - directoryBegin > directory.Size - sizeof(callBounds)
                    || (callRecord - directoryBegin) % sizeof(callBounds)
                    || !SafeRead(callRecord, callBounds) || base + callBounds.BeginAddress != target
                    || callBounds.EndAddress <= callBounds.BeginAddress
                    || !ImageSectionSpan(base, imageSize, sectionOffset, nt.FileHeader.NumberOfSections,
                        target, callBounds.EndAddress - callBounds.BeginAddress, true))
                    { shape = false; break; }
            }
            if (!shape || !table || table % alignof(uintptr_t)
                || !ImageSectionSpan(base, imageSize, sectionOffset, nt.FileHeader.NumberOfSections,
                    table, count * sizeof(uintptr_t), false)) continue;
            DescriptorDiscovery candidate{};
            const bool tableValid = DescriptorTableMatches(base, end, table, profile, &candidate, count);
            if (detail)
            {
                ++detail->namedReferences;
                if (!detail->candidate || candidate.validSlots > detail->validSlots)
                {
                    detail->candidate = table;
                    detail->validSlots = candidate.validSlots;
                    detail->failedSlot = candidate.failedSlot;
                    detail->check = candidate.check;
                    detail->registeredSlots = count;
                    detail->registration = begin;
                }
            }
            if (!tableValid) continue;
            if (detail) ++detail->matches;
            if (match)
                return 0;
            match = table + profile.temporalSlot * sizeof(uintptr_t);
        }
    }
    if (!match && detail && !detail->candidate) detail->check.failure = DescriptorFailure::eRegistration;
    return match;
}

constexpr char kSm89PtxTarget[] = ".target sm_89";
constexpr char kSm86PtxTarget[] = ".target sm_86";
enum class StripFailure { eNone, eAmpereSm89PtxMetadataInvalid,
    eAmpereSm86SizeOverflow, eAmpereSm89PtxDecompressionFailed,
    eAmpereSm89TargetMismatch, eTemporalIdentityOrLayout, eMaskIdentityOrLayout };
bool DecompressNvidiaLz(const uint8_t* input, size_t inputSize,
    uint8_t* output, size_t outputSize) noexcept
{
    if (!input || !output || inputSize == 0 || outputSize == 0)
        return false;
    size_t inputOffset = 0;
    size_t outputOffset = 0;
    while (inputOffset < inputSize)
    {
        const uint8_t token = input[inputOffset++];
        size_t literalBytes = token >> 4;
        if (literalBytes == 15)
        {
            uint8_t extension = 0;
            do
            {
                if (inputOffset >= inputSize)
                    return false;
                extension = input[inputOffset++];
                if (literalBytes > SIZE_MAX - extension)
                    return false;
                literalBytes += extension;
            } while (extension == 0xFF);
        }
        if (literalBytes > inputSize - inputOffset
            || literalBytes > outputSize - outputOffset)
            return false;
        std::memcpy(output + outputOffset, input + inputOffset, literalBytes);
        inputOffset += literalBytes;
        outputOffset += literalBytes;
        if (inputOffset == inputSize)
            break;
        if (inputSize - inputOffset < 2)
            return false;
        const size_t backOffset = static_cast<size_t>(input[inputOffset])
            | (static_cast<size_t>(input[inputOffset + 1]) << 8);
        inputOffset += 2;
        if (backOffset == 0 || backOffset > outputOffset)
            return false;
        size_t matchBytes = 4 + (token & 0x0F);
        if ((token & 0x0F) == 15)
        {
            uint8_t extension = 0;
            do
            {
                if (inputOffset >= inputSize)
                    return false;
                extension = input[inputOffset++];
                if (matchBytes > SIZE_MAX - extension)
                    return false;
                matchBytes += extension;
            } while (extension == 0xFF);
        }
        if (matchBytes > outputSize - outputOffset)
            return false;
        for (size_t index = 0; index < matchBytes; ++index)
            output[outputOffset + index] =
                output[outputOffset + index - backOffset];
        outputOffset += matchBytes;
    }
    return inputOffset == inputSize && outputOffset == outputSize;
}

size_t FindUniqueBytes(const uint8_t* bytes, size_t count,
    const char* marker, size_t markerBytes) noexcept
{
    if (!bytes || !marker || markerBytes == 0 || markerBytes > count)
        return SIZE_MAX;
    size_t match = SIZE_MAX;
    for (size_t offset = 0; offset + markerBytes <= count; ++offset)
    {
        if (std::memcmp(bytes + offset, marker, markerBytes) != 0)
            continue;
        if (match != SIZE_MAX)
            return SIZE_MAX;
        match = offset;
    }
    return match;
}

struct Sm89PtxMetadata
{
    uint16_t outerHeaderSize = 0;
    uint64_t entryOffset = 0;
    uint32_t imageHeaderSize = 0;
    uint64_t paddedPayloadSize = 0;
    uint32_t compressedSize = 0;
    uint64_t flags = 0;
    uint64_t decompressedSize = 0;
    size_t rawPtxBytes = 0;
};

bool FindUniqueSm89Ptx(uintptr_t fatbin, uint64_t totalSize,
    Sm89PtxMetadata& metadata) noexcept
{
    if (!fatbin || totalSize < 16 || totalSize > kMaximumFatbinBytes)
        return false;
    std::array<uint8_t, 16> outer{};
    if (!SafeCopy(outer.data(), reinterpret_cast<const void*>(fatbin),
            outer.size())
        || ReadU32(outer.data()) != 0xBA55ED50u)
        return false;
    const uint16_t outerHeaderSize = ReadU16(outer.data() + 6);
    if (outerHeaderSize < outer.size() || outerHeaderSize > totalSize
        || ReadU64(outer.data() + 8) != totalSize - outerHeaderSize)
        return false;

    uint64_t offset = outerHeaderSize;
    uint32_t matches = 0;
    while (offset + 64 <= totalSize)
    {
        std::array<uint8_t, 64> header{};
        if (!SafeCopy(header.data(), reinterpret_cast<const void*>(
                fatbin + static_cast<uintptr_t>(offset)), header.size()))
            return false;
        const uint16_t kind = ReadU16(header.data());
        const uint32_t imageHeaderSize = ReadU32(header.data() + 4);
        const uint64_t paddedPayloadSize = ReadU64(header.data() + 8);
        const uint32_t architecture = ReadU32(header.data() + 28);
        if (imageHeaderSize < header.size()
            || imageHeaderSize > totalSize - offset
            || paddedPayloadSize > totalSize - offset - imageHeaderSize)
            return false;
        const uint64_t entrySize = imageHeaderSize + paddedPayloadSize;
        if (kind == 1 && architecture == 89)
        {
            ++matches;
            metadata.outerHeaderSize = outerHeaderSize;
            metadata.entryOffset = offset;
            metadata.imageHeaderSize = imageHeaderSize;
            metadata.paddedPayloadSize = paddedPayloadSize;
            metadata.compressedSize = ReadU32(header.data() + 16);
            metadata.flags = ReadU64(header.data() + 40);
            metadata.decompressedSize = ReadU64(header.data() + 56);
        }
        offset += entrySize;
    }
    if (offset != totalSize || matches != 1)
        return false;
    const bool compressed = (metadata.flags & 0x2000u) != 0;
    if (compressed)
    {
        if (metadata.compressedSize == 0
            || metadata.compressedSize > metadata.paddedPayloadSize
            || metadata.decompressedSize == 0
            || metadata.decompressedSize > kMaximumFatbinBytes)
            return false;
        metadata.rawPtxBytes = static_cast<size_t>(metadata.decompressedSize);
    }
    else
    {
        if (metadata.paddedPayloadSize == 0
            || metadata.paddedPayloadSize > kMaximumFatbinBytes)
            return false;
        metadata.rawPtxBytes = metadata.compressedSize != 0
                && metadata.compressedSize <= metadata.paddedPayloadSize
            ? metadata.compressedSize
            : static_cast<size_t>(metadata.paddedPayloadSize);
    }
    return true;
}

bool CorrectTemporalPtx(uint8_t* ptx, size_t& bytes, size_t capacity,
    const TemporalProviderProfile& profile) noexcept
{
    if (!ptx || bytes > capacity || !profile.sourcePtxSha256 || !profile.parameter
        || !profile.currentScale || !profile.previousScale
        || !Sha256Equals(ptx, bytes, profile.sourcePtxSha256)) return false;
    try
    {
        std::string source(reinterpret_cast<const char*>(ptx), bytes);
        if (profile.extraRegisters)
        {
            constexpr char before[] = ".reg .f32 %f<4010>;";
            constexpr char after[] = ".reg .f32 %f<4013>;";
            const size_t declaration = FindUniqueBytes(ptx, bytes, before, sizeof(before) - 1);
            if (declaration == SIZE_MAX) return false;
            source.replace(declaration, sizeof(before) - 1, after);
        }
        constexpr char label[] = "$L__BB0_3:";
        const size_t join = FindUniqueBytes(ptx, bytes, label, sizeof(label) - 1);
        if (join == SIZE_MAX) return false;
        const size_t newline = source.find('\n', join + sizeof(label) - 1);
        if (newline == std::string::npos) return false;
        const size_t insertion = newline + 1;
        constexpr char midpoint[] = "0f3F000000";
        constexpr char multiply[] = "mul.ftz.f32 ";
        std::array<size_t, 104> positions{};
        size_t count = 0, search = 0;
        while ((search = source.find(midpoint, search)) != std::string::npos)
        {
            const size_t marker = search;
            search += sizeof(midpoint) - 1;
            if (search >= bytes || source[search] != ';') continue;
            const size_t previousLine = source.rfind('\n', marker);
            const size_t line = previousLine == std::string::npos ? 0 : previousLine + 1;
            if (source.compare(line, sizeof(multiply) - 1, multiply) != 0) continue;
            if (count == positions.size() || marker <= insertion) return false;
            positions[count++] = marker;
        }
        if (count != positions.size()) return false;
        const char* one = profile.extraRegisters ? "%f4011" : "%f135";
        std::string result;
        result.reserve(bytes + kTemporalGrowthAllowance);
        result.append(source, 0, insertion);
        result += "ld.param.f32 " + std::string(profile.previousScale) + ", [" + profile.parameter + "];\r\n";
        result += "mov.f32 " + std::string(one) + ", 0f3F800000;\r\n";
        result += "sub.ftz.f32 " + std::string(profile.currentScale) + ", " + one + ", " + profile.previousScale + ";\r\n";
        size_t cursor = insertion;
        for (size_t i = 0; i < positions.size(); ++i)
        {
            result.append(source, cursor, positions[i] - cursor);
            result += i < 52 ? profile.currentScale : profile.previousScale;
            cursor = positions[i] + sizeof(midpoint) - 1;
        }
        result.append(source, cursor, source.size() - cursor);
        if (result.size() > capacity) return false;
        memcpy(ptx, result.data(), result.size());
        bytes = result.size();
        return true;
    }
    catch (...) { return false; }
}

bool BuildAmpereSm86Fatbin(uintptr_t sourceFatbin, uint64_t sourceTotalSize,
    const Sm89PtxMetadata& metadata, uint8_t* output,
    size_t outputCapacity, uint8_t* scratch, size_t scratchCapacity,
    uint32_t& outputBytes, StripFailure& failure,
    const TemporalProviderProfile* temporal = nullptr, bool outputPullMask = false) noexcept
{
    failure = StripFailure::eAmpereSm89PtxMetadataInvalid;
    if (!sourceFatbin || !output || metadata.outerHeaderSize < 16
        || metadata.imageHeaderSize < 64 || metadata.rawPtxBytes == 0
        || metadata.entryOffset > sourceTotalSize
        || metadata.imageHeaderSize > sourceTotalSize - metadata.entryOffset
        || metadata.paddedPayloadSize > sourceTotalSize
            - metadata.entryOffset - metadata.imageHeaderSize)
        return false;
    size_t rawBytes = metadata.rawPtxBytes;
    size_t paddedRawBytes = (rawBytes + 7u) & ~size_t{7};
    if (metadata.rawPtxBytes > SIZE_MAX - 7u
        || metadata.outerHeaderSize > SIZE_MAX - metadata.imageHeaderSize
        || metadata.outerHeaderSize + metadata.imageHeaderSize
            > SIZE_MAX - paddedRawBytes)
    {
        failure = StripFailure::eAmpereSm86SizeOverflow;
        return false;
    }
    size_t finalSize = metadata.outerHeaderSize
        + metadata.imageHeaderSize + paddedRawBytes;
    if (finalSize > outputCapacity || finalSize > UINT32_MAX)
    {
        failure = StripFailure::eAmpereSm86SizeOverflow;
        return false;
    }
    if (!SafeCopy(output, reinterpret_cast<const void*>(sourceFatbin),
            metadata.outerHeaderSize)
        || !SafeCopy(output + metadata.outerHeaderSize,
            reinterpret_cast<const void*>(sourceFatbin
                + static_cast<uintptr_t>(metadata.entryOffset)),
            metadata.imageHeaderSize))
        return false;

    uint8_t* const destinationPtx = output + metadata.outerHeaderSize
        + metadata.imageHeaderSize;
    const uintptr_t sourcePayload = sourceFatbin
        + static_cast<uintptr_t>(metadata.entryOffset)
        + metadata.imageHeaderSize;
    if ((metadata.flags & 0x2000u) != 0)
    {
        if (!scratch || scratchCapacity < metadata.compressedSize
            || !SafeCopy(scratch,
                reinterpret_cast<const void*>(sourcePayload),
                metadata.compressedSize)
            || !DecompressNvidiaLz(scratch, metadata.compressedSize,
                destinationPtx, metadata.rawPtxBytes))
        {
            failure = StripFailure::eAmpereSm89PtxDecompressionFailed;
            return false;
        }
    }
    else if (!SafeCopy(destinationPtx,
        reinterpret_cast<const void*>(sourcePayload),
        metadata.rawPtxBytes))
    {
        return false;
    }

    if (temporal)
    {
        failure = StripFailure::eTemporalIdentityOrLayout;
        if (sourceTotalSize != temporal->sourceFatbinBytes
            || !Sha256Equals(reinterpret_cast<const uint8_t*>(sourceFatbin),
                static_cast<size_t>(sourceTotalSize), temporal->sourceFatbinSha256)
            || !CorrectTemporalPtx(destinationPtx, rawBytes,
                outputCapacity - metadata.outerHeaderSize - metadata.imageHeaderSize, *temporal))
            return false;
        paddedRawBytes = (rawBytes + 7u) & ~size_t{7};
        finalSize = metadata.outerHeaderSize + metadata.imageHeaderSize + paddedRawBytes;
        if (finalSize > outputCapacity || finalSize > UINT32_MAX) return false;
    }
    if (outputPullMask)
    {
        failure = StripFailure::eMaskIdentityOrLayout;
        if (!ampere_mask_transform::Apply(reinterpret_cast<const uint8_t*>(sourceFatbin),
                static_cast<size_t>(sourceTotalSize), destinationPtx, rawBytes,
                outputCapacity - metadata.outerHeaderSize - metadata.imageHeaderSize)) return false;
        paddedRawBytes = (rawBytes + 7u) & ~size_t{7};
        finalSize = metadata.outerHeaderSize + metadata.imageHeaderSize + paddedRawBytes;
        if (finalSize > outputCapacity || finalSize > UINT32_MAX) return false;
    }
    const size_t targetOffset = FindUniqueBytes(destinationPtx,
        rawBytes, kSm89PtxTarget,
        sizeof(kSm89PtxTarget) - 1);
    if (targetOffset == SIZE_MAX)
    {
        failure = StripFailure::eAmpereSm89TargetMismatch;
        return false;
    }
    // Retargeting is permitted only for PTX the Ampere compiler can consume.
    // In particular this is not FP8 emulation or a Blackwell SASS transplant.
    const std::string_view ptx(reinterpret_cast<const char*>(destinationPtx),
        rawBytes);
    for (const char* unsupported : {".e4m3", ".e5m2", "wgmma.", "tcgen05.", "mma.sp::ordered_metadata", "sm_90", "sm_120"})
    {
        if (ptx.find(unsupported) != std::string_view::npos)
            return false;
    }
    std::memcpy(destinationPtx + targetOffset, kSm86PtxTarget,
        sizeof(kSm86PtxTarget) - 1);
    std::memset(destinationPtx + rawBytes, 0,
        paddedRawBytes - rawBytes);

    uint8_t* const destinationEntry = output + metadata.outerHeaderSize;
    const uint64_t paddedRawBytes64 = paddedRawBytes;
    const uint32_t actualPayloadBytes =
        (metadata.flags & 0x2000u) != 0
            ? 0u : static_cast<uint32_t>(rawBytes);
    const uint32_t sm86 = 86;
    const uint64_t uncompressedFlags = metadata.flags & ~uint64_t{0x2000};
    const uint64_t zero64 = 0;
    std::memcpy(destinationEntry + 8, &paddedRawBytes64,
        sizeof(paddedRawBytes64));
    std::memcpy(destinationEntry + 16, &actualPayloadBytes,
        sizeof(actualPayloadBytes));
    std::memcpy(destinationEntry + 28, &sm86, sizeof(sm86));
    std::memcpy(destinationEntry + 40, &uncompressedFlags,
        sizeof(uncompressedFlags));
    std::memcpy(destinationEntry + 56, &zero64, sizeof(zero64));
    const uint64_t payloadBytes = finalSize - metadata.outerHeaderSize;
    std::memcpy(output + 8, &payloadBytes, sizeof(payloadBytes));
    outputBytes = static_cast<uint32_t>(finalSize);
    failure = StripFailure::eNone;
    return true;
}

#include "ampere_font_program.inl"

bool ProtectionMatches(uintptr_t address, DWORD expected) noexcept
{
    MEMORY_BASIC_INFORMATION memory{};
    constexpr DWORD mask = 0xffu | PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE;
    return VirtualQuery(reinterpret_cast<void*>(address), &memory, sizeof(memory)) == sizeof(memory)
        && memory.State == MEM_COMMIT && (memory.Protect & mask) == (expected & mask);
}
PublishResult PublishPointer(uintptr_t address, uintptr_t expected,
    uintptr_t replacement, protected_pointer::ProtectMemoryFn protect = &VirtualProtect) noexcept
{
    // The shared primitive never writes an expected value before its CAS,
    // so another writer cannot be overwritten while materializing a page.
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(reinterpret_cast<void*>(address), &memory, sizeof(memory)) != sizeof(memory)
        || memory.State != MEM_COMMIT) return {};
    DWORD writable = PAGE_READWRITE;
    if (memory.Type == MEM_IMAGE)
    {
        PSAPI_WORKING_SET_EX_INFORMATION working{};
        working.VirtualAddress = reinterpret_cast<void*>(address);
        if (!QueryWorkingSetEx(GetCurrentProcess(), &working, sizeof(working))
            || !working.VirtualAttributes.Valid) return {};
        // Windows can report WRITECOPY when RW was requested for a shared
        // image. After the first CAS privatizes it, use the private RW route.
        if (working.VirtualAttributes.Shared) writable = PAGE_WRITECOPY;
    }
    const auto result = protected_pointer::ReplaceProtectedPointer(address,
        expected, replacement, protect, writable);
    return {result.disposition == protected_pointer::PublishDisposition::ePublishedRestored,
        result.replacementWasPublished};
}

struct ProgramPublicationResult
{
    bool success = false;
    bool attempted = false;
    bool rollbackComplete = false;
    size_t failedSlot = SIZE_MAX;
};

ProgramPublicationResult PublishProgram(PublishedProgram& program,
    ampere_native_cache::ProgramView payloads,
    protected_pointer::ProtectMemoryFn protect = &VirtualProtect) noexcept
{
    ProgramPublicationResult result{};
    std::array<DWORD, kMaximumKernelCount> protections{};
    if (!protect || !program.boundary.Current(program.module)
        || program.slotCount < kKernelCount || program.slotCount > kMaximumKernelCount
        || payloads.size() != program.slotCount || !program.font
        || program.font->module != program.module
        || !FontCurrent(*program.font, false)) return result;
    // Recheck every original and complete immutable clone before exposing the
    // first pointer. A full second readback is required before admission.
    for (size_t i = 0; i < program.slotCount; ++i)
    {
        result.failedSlot = i;
        const uintptr_t address = program.table + i * sizeof(uintptr_t);
        uintptr_t observed = 0;
        if (!IsReadOnlyImageAddress(program.module, address)
            || !SafeRead(address, observed) || observed != program.originals[i]
            || !protected_pointer::QueryProtection(address, protections[i], sizeof(uintptr_t))
            || !ImmutableCloneCurrent(program, i, &payloads[i])) return result;
    }
    // CUDA loading and preflight may reenter native initialization. A copied
    // first-init flag is insufficient after the host's live ticket is revoked.
    if (!program.boundary.Current(program.module)) return result;
    size_t attempted = 0;
    result.attempted = true;
    result.failedSlot = SIZE_MAX;
    const bool fontPublished = WriteFont(*program.font, true, protect);
    for (size_t i = 0; fontPublished && program.boundary.Current(program.module) && i < program.slotCount; ++i)
    {
        result.attempted = true;
        attempted = i + 1;
        result.failedSlot = i;
        if (!PublishPointer(program.table + i * sizeof(uintptr_t),
                program.originals[i], program.replacements[i], protect).success) break;
        if (attempted != program.slotCount) continue;
        result.success = true;
        for (size_t slot = 0; slot < program.slotCount; ++slot)
        {
            uintptr_t observed = 0;
            const uintptr_t address = program.table + slot * sizeof(uintptr_t);
            if (!SafeRead(address, observed) || observed != program.replacements[slot]
                || !IsReadOnlyImageAddress(program.module, address)
                || !protected_pointer::ProtectionMatches(address, protections[slot], sizeof(uintptr_t))
                || !ImmutableCloneCurrent(program, slot, &payloads[slot]))
            { result.success = false; result.failedSlot = slot; break; }
        }
        if (result.success && (!program.boundary.Current(program.module)
            || !FontCurrent(*program.font, true))) result.success = false;
    }
    if (result.success) { result.failedSlot = SIZE_MAX; return result; }
    // Storage is retained after any attempt. CAS rollback preserves a foreign
    // writer, and original page protection is restored independently of the
    // pointer result. Every uncertain transaction permanently blocks admission.
    for (size_t i = 0; i < attempted; ++i)
    {
        const uintptr_t address = program.table + i * sizeof(uintptr_t);
        uintptr_t observed = 0;
        if (SafeRead(address, observed) && observed == program.replacements[i])
            PublishPointer(address, program.replacements[i], program.originals[i], protect);
        protected_pointer::RestoreProtectionWithRetry(reinterpret_cast<void*>(address),
            sizeof(uintptr_t), protections[i], protect);
    }
    if (fontPublished) WriteFont(*program.font, false, protect);
    // Protection restoration is independent of byte ownership. Preserve any
    // foreign content and restore each page's original access semantics.
    for (size_t i = 0; i < program.font->pageCount; ++i)
        RestoreFontPage(*program.font, i, protect);
    result.rollbackComplete = FontCurrent(*program.font, false);
    for (size_t i = 0; i < attempted; ++i)
    {
        const uintptr_t address = program.table + i * sizeof(uintptr_t);
        uintptr_t observed = 0;
        if (!SafeRead(address, observed) || observed != program.originals[i]
            || !protected_pointer::ProtectionMatches(address, protections[i], sizeof(uintptr_t)))
            result.rollbackComplete = false;
    }
    return result;
}

uint64_t PackLuid(const LUID& luid) noexcept
{
    return static_cast<uint64_t>(luid.LowPart)
        | (static_cast<uint64_t>(static_cast<uint32_t>(luid.HighPart)) << 32);
}

void SetFailure(Failure failure) noexcept;
bool VerifyAmpereAdapter(const LUID&, int&, int&) noexcept;
void RejectAdapterUnavailable() noexcept
{
    std::lock_guard lock(gMutex);
    gAdapterVerified.store(false, std::memory_order_release);
    gReady.store(false, std::memory_order_release);
    SetFailure(Failure::eAdapterUnavailable);
}

bool ObserveAdapterLuid(const LUID& luid, const wchar_t* api) noexcept
{
    int major = 0;
    int minor = 0;
    const bool verified = VerifyAmpereAdapter(luid, major, minor);
    const uint64_t packedLuid = PackLuid(luid);
    {
        std::lock_guard lock(gMutex);
        if (gProvider && gPublishedAdapterLuid != packedLuid)
        {
            gAdapterVerified.store(false, std::memory_order_release);
            gReady.store(false, std::memory_order_release);
            SetFailure(Failure::eRestartRequired);
            Log(L"Ampere SM86 provider requires restart after an adapter change");
            return false;
        }
        gAdapterLuid.store(packedLuid, std::memory_order_release);
        gAdapterVerified.store(verified, std::memory_order_release);
        if (!verified)
        {
            gReady.store(false, std::memory_order_release);
            SetFailure(major == 0 && minor == 0
                    ? Failure::eAdapterUnavailable : Failure::eAdapterNotAmpere);
        }
        else if (!gProvider) SetFailure(Failure::eNone);
    }
    Log(L"Ampere %s adapter verification: luid=0x%016llX "
        L"capability=%d.%d verified=%d", api ? api : L"unknown",
        static_cast<unsigned long long>(packedLuid), major, minor, verified);
    return verified;
}

bool VerifyAmpereAdapter(const LUID& activeLuid, int& major, int& minor) noexcept
{
    using CuInit = int (WINAPI*)(unsigned int);
    using CuDeviceGetCount = int (WINAPI*)(int*);
    using CuDeviceGet = int (WINAPI*)(int*, int);
    using CuDeviceComputeCapability = int (WINAPI*)(int*, int*, int);
    using CuDeviceGetLuid = int (WINAPI*)(char*, unsigned int*, int);

    HMODULE cuda = LoadLibraryExW(L"nvcuda.dll", nullptr,
        LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!cuda)
        return false;
    auto* initialize = reinterpret_cast<CuInit>(GetProcAddress(cuda, "cuInit"));
    auto* getCount = reinterpret_cast<CuDeviceGetCount>(
        GetProcAddress(cuda, "cuDeviceGetCount"));
    auto* getDevice = reinterpret_cast<CuDeviceGet>(
        GetProcAddress(cuda, "cuDeviceGet"));
    auto* getCapability = reinterpret_cast<CuDeviceComputeCapability>(
        GetProcAddress(cuda, "cuDeviceComputeCapability"));
    auto* getLuid = reinterpret_cast<CuDeviceGetLuid>(
        GetProcAddress(cuda, "cuDeviceGetLuid"));
    int count = 0;
    bool complete = initialize && getCount && getDevice && getCapability
        && getLuid && initialize(0) == 0 && getCount(&count) == 0 && count > 0;
    uint32_t matches = 0;
    unsigned int matchedNodeMask = 0;
    for (int ordinal = 0; complete && ordinal < count; ++ordinal)
    {
        int device = 0;
        int candidateMajor = 0;
        int candidateMinor = 0;
        std::array<char, sizeof(LUID)> rawLuid{};
        unsigned int nodeMask = 0;
        if (getDevice(&device, ordinal) != 0
            || getCapability(&candidateMajor, &candidateMinor, device) != 0
            || getLuid(rawLuid.data(), &nodeMask, device) != 0)
        {
            complete = false;
            break;
        }
        LUID candidateLuid{};
        std::memcpy(&candidateLuid, rawLuid.data(), sizeof(candidateLuid));
        if (candidateLuid.LowPart == activeLuid.LowPart
            && candidateLuid.HighPart == activeLuid.HighPart)
        {
            ++matches;
            matchedNodeMask = nodeMask;
            major = candidateMajor;
            minor = candidateMinor;
        }
    }
    FreeLibrary(cuda);
    return complete && ampere_policy::AdapterMatches(PackLuid(activeLuid),
        PackLuid(activeLuid), matches, major, minor, matchedNodeMask);
}

void SetFailure(Failure failure) noexcept
{
    const auto value = static_cast<uint32_t>(failure);
    if (gFailure.exchange(value, std::memory_order_acq_rel) != value && value)
        Log(L"Ampere program preparation deferred/rejected: failure=%u stage=%u (%hs)", value,
            static_cast<uint32_t>(gPreparation.load()),
            ampere_diagnostics::PreparationName(static_cast<uint32_t>(gPreparation.load())));
}

} // namespace

void SetLogCallback(LogCallback callback) noexcept
{
    gLogCallback.store(callback, std::memory_order_release);
}

bool ObserveD3D12Device(void* device) noexcept
{
    if (!device)
    {
        RejectAdapterUnavailable();
        return false;
    }
    ID3D12Device* d3d12 = nullptr;
    if (FAILED(reinterpret_cast<IUnknown*>(device)->QueryInterface(
            __uuidof(ID3D12Device), reinterpret_cast<void**>(&d3d12))))
    {
        RejectAdapterUnavailable();
        return false;
    }
    const LUID luid = d3d12->GetAdapterLuid();
    d3d12->Release();
    return ObserveAdapterLuid(luid, L"D3D12");
}

bool ObserveAdapter(void* adapter) noexcept
{
    if (!adapter) return false;
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(static_cast<IDXGIAdapter*>(adapter)->GetDesc(&desc))) return false;
    return desc.VendorId == 0x10de && adapter_discovery::IsAmpere(desc.AdapterLuid);
}

bool PatchProvider(HMODULE module, const wchar_t* path, const PreparationBoundary& boundary) noexcept
{
    std::lock_guard lock(gMutex);
    if (!module || !gAdapterVerified.load() || gPublicationFailed) return false;
    const uint64_t luid = gAdapterLuid.load();
    if (const auto* program = FindPublishedProgram(module))
    {
        gPreparation.store(Preparation::eRevalidation);
        const bool current = ProgramCurrent(*program, luid);
        if (module == gProvider)
        {
            gReady.store(current, std::memory_order_release);
            if (!current && gProviderBound) gPublicationFailed = true;
        }
        if (!current) SetFailure(Failure::eRestartRequired);
        else gPreparation.store(Preparation::eReady);
        return current;
    }
    auto fail = [&](Failure code) { SetFailure(code); return false; };
    // No additional publication may be introduced once a real FG feature has
    // bound its provider. Already prepared candidates remain immutable.
    if (gProviderBound || gProgramCount == gPrograms.size())
        return fail(Failure::eRestartRequired);
    if (!boundary.Current(module)) return fail(Failure::eEarlyPreparation);
    gPreparation.store(Preparation::eEligibility);
    if (!dlssg_provider_policy::IsSupportedProvider(module, path)) return fail(Failure::eProviderVersion);
    dlssg_provider_policy::VersionTriplet version{};
    gPreparation.store(Preparation::eVersion);
    if (!dlssg_provider_policy::ReadProviderVersion(path, version)) return fail(Failure::eProviderVersion);
    gPreparation.store(Preparation::eProfile);
    const auto* profile = ProfileForVersion(version);
    if (!profile) return fail(Failure::eProviderLayout);
    uint32_t size = 0;
    gPreparation.store(Preparation::eImage);
    if (!ImageSize(module, size)) return fail(Failure::eProviderLayout);
    gPreparation.store(Preparation::eDescriptorTable);
    DescriptorDiscovery discovery{};
    const uintptr_t anchor = FindDescriptorEntry(module, size, *profile, &discovery);
    if (!anchor)
    {
        Log(L"Ampere descriptor discovery rejected: provider=%p version=%u.%u.%u "
            L"matches=%u namedReferences=%u candidateRva=0x%zX validSlots=%u/%zu "
            L"failedSlot=%d reason=%s supplied=%u headerBytes=%llu path=%s",
            module, version.major, version.minor, version.build, discovery.matches,
            discovery.namedReferences, discovery.candidate ? discovery.candidate - reinterpret_cast<uintptr_t>(module) : 0,
            discovery.validSlots, discovery.registeredSlots,
            discovery.failedSlot == SIZE_MAX ? -1 : static_cast<int>(discovery.failedSlot), DescriptorFailureName(discovery.check.failure),
            discovery.check.supplied, discovery.check.bytes, path ? path : L"(unknown)");
        return fail(Failure::eProviderLayout);
    }
    const uintptr_t table = anchor - profile->temporalSlot * sizeof(uintptr_t);
    const size_t slotCount = discovery.registeredSlots;
    if (!RegistrationCountMatches(*profile, slotCount)) return fail(Failure::eProviderLayout);
    HMODULE pinned = nullptr;
    gPreparation.store(Preparation::eModuleLifetime);
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(module), &pinned) || pinned != module)
        return fail(Failure::eProviderLayout);

    std::array<uintptr_t, kMaximumKernelCount> originals{}, clones{};
    PublishedProgram candidate{};
    candidate.module = module; candidate.pinned = pinned;
    candidate.luid = luid; candidate.table = table; candidate.boundary = boundary;
    candidate.slotCount = slotCount;
    ampere_native_cache::CompleteProgram ptxProgram, nativeProgram;
    std::vector<uint8_t> fontPtx, fontNative;
    std::vector<ampere_cuda_program::Entry> entries;
    bool useNative = false;
    auto discard = [&]() { for (auto clone : clones) if (clone) VirtualFree(reinterpret_cast<void*>(clone), 0, MEM_RELEASE); };
    // Build the complete program before publishing any slot. This function is
    // called under the Create serialization lock, before the first registration.
    try
    {
        gPreparation.store(Preparation::eFontProgram);
        candidate.font = std::make_shared<FontProgram>();
        if (!PrepareFont(module, size, *candidate.font, fontPtx, fontNative))
        {
            Log(L"Ampere auxiliary font rejected: stage=%S RVA=0x%zX; unique owned source payload or SM86 transform unavailable; provider=%s",
                candidate.font->stage, candidate.font->address ? candidate.font->address - reinterpret_cast<uintptr_t>(module) : 0,
                path ? path : L"(unknown)");
            return fail(Failure::eFontProgram);
        }
        ptxProgram.resize(slotCount);
        entries.resize(slotCount);
        for (size_t i = 0; i < slotCount; ++i)
        {
            gPreparation.store(Preparation::eKernelDescriptor);
            const uintptr_t entry = table + i * sizeof(uintptr_t);
            uintptr_t fatbin = 0;
            uintptr_t kernelEntry = 0;
            uint32_t supplied = 0;
            if (!IsReadOnlyImageAddress(module, entry)
                || !SafeRead(entry, originals[i])
                || !SafeRead(originals[i] + 8, fatbin)
                || !SafeRead(originals[i] + 16, supplied) || !supplied
                || !SafeRead(originals[i] + 24, kernelEntry)
                || !SafeCopy(entries[i].data(), reinterpret_cast<const void*>(kernelEntry), entries[i].size() - 1))
            { discard(); return fail(Failure::eProviderNotReady); }
            Sm89PtxMetadata meta{};
            gPreparation.store(Preparation::ePtxIdentity);
            if (!FindUniqueSm89Ptx(fatbin, supplied, meta))
            { discard(); return fail(Failure::eSourceIdentity); }
            const auto* temporal = i == profile->temporalSlot ? profile : nullptr;
            const size_t capacity = meta.outerHeaderSize + meta.imageHeaderSize
                + ((meta.rawPtxBytes + 7) & ~size_t{7}) + (temporal ? kTemporalGrowthAllowance : 0);
            ptxProgram[i].resize(capacity);
            std::vector<uint8_t> scratch(meta.compressedSize);
            uint32_t outputSize = 0;
            StripFailure reason{};
            gPreparation.store(Preparation::ePtxTransform);
            if (!BuildAmpereSm86Fatbin(fatbin, supplied, meta, ptxProgram[i].data(),
                    capacity, scratch.data(), scratch.size(), outputSize, reason, temporal,
                    i == 14 && profile->temporalSlot == 9))
            { discard(); return fail(Failure::eDecompression); }
            ptxProgram[i].resize(outputSize);
        }
        gPreparation.store(Preparation::eNativeCache);
        const auto mode = ampere_native_cache::ConfiguredMode();
        if (mode != ampere_native_cache::Mode::ePtx)
        {
            const auto result = ampere_native_cache::LoadConfiguredProgram(ptxProgram, nativeProgram);
            gNativeCacheStatus.store(static_cast<uint32_t>(result), std::memory_order_release);
            useNative = result == ampere_native_cache::Status::eLoaded;
            if (!useNative && mode == ampere_native_cache::Mode::eCubin)
                return fail(Failure::eNativeProgram);
        }
        gPreparation.store(Preparation::eCudaValidation);
        auto validated = ampere_cuda_program::Validate(useNative ? nativeProgram : ptxProgram, entries, luid,
            {useNative ? &fontNative : &fontPtx});
        if (!validated.Ready())
        {
            Log(L"Ampere CUDA program validation: image=%s stage=%u slot=%u result=%d contextRestored=%u provider=%s",
                useNative ? L"native" : L"PTX", static_cast<uint32_t>(validated.stage), validated.slot,
                validated.error, static_cast<unsigned>(validated.contextRestored), path ? path : L"(unknown)");
            if (!useNative || mode != ampere_native_cache::Mode::eAuto || !validated.NativeImageUnsupported())
                return fail(Failure::eCudaProgram);
            // Compatibility fallback is decided for the complete program,
            // before any descriptor is published and before the first Create.
            // It is never attempted after provider evaluation or device loss.
            validated = ampere_cuda_program::Validate(ptxProgram, entries, luid, {&fontPtx});
            if (!validated.Ready())
            {
                Log(L"Ampere CUDA PTX fallback rejected: stage=%u slot=%u result=%d contextRestored=%u provider=%s",
                    static_cast<uint32_t>(validated.stage), validated.slot, validated.error,
                    static_cast<unsigned>(validated.contextRestored), path ? path : L"(unknown)");
                return fail(Failure::eCudaProgram);
            }
            useNative = false;
            gNativeCacheStatus.store(static_cast<uint32_t>(ampere_native_cache::Status::eDriverRejected));
            Log(L"Ampere CUDA native image unsupported; complete validated PTX program selected before publication");
        }
        // Choose one complete program before any publication; never mix native
        // and PTX slots, or change the program while a feature can be using it.
        if (!boundary.Current(module)) return fail(Failure::eEarlyPreparation);
        const auto& selectedFont = useNative ? fontNative : fontPtx;
        std::copy(selectedFont.begin(), selectedFont.end(), candidate.font->replacement.begin());
        gPreparation.store(Preparation::eDescriptorClone);
        const auto& selected = useNative ? nativeProgram : ptxProgram;
        for (size_t i = 0; i < slotCount; ++i)
        {
            const uint32_t outputSize = static_cast<uint32_t>(selected[i].size());
            const size_t allocationSize = kDescriptorBytes + outputSize;
            auto* allocation = static_cast<uint8_t*>(VirtualAlloc(nullptr,
                allocationSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            if (!allocation) { discard(); return fail(Failure::eAllocation); }
            clones[i] = reinterpret_cast<uintptr_t>(allocation);
            if (!SafeCopy(allocation, reinterpret_cast<void*>(originals[i]), kDescriptorBytes))
            { discard(); return fail(Failure::eProviderLayout); }
            memcpy(allocation + kDescriptorBytes, selected[i].data(), outputSize);
            const uintptr_t output = clones[i] + kDescriptorBytes;
            memcpy(allocation + 8, &output, sizeof(output));
            memcpy(allocation + 16, &outputSize, sizeof(outputSize));
            DWORD previous = 0;
            if (!VirtualProtect(allocation, allocationSize, PAGE_READONLY, &previous))
            { discard(); return fail(Failure::ePublication); }
            candidate.allocationBytes[i] = static_cast<uint32_t>(allocationSize);
            if (!SafeCopy(candidate.descriptors[i].data(), allocation, kDescriptorBytes))
            { discard(); return fail(Failure::ePublication); }
        }
    }
    catch (...) { discard(); return fail(Failure::eAllocation); }
    gPreparation.store(Preparation::ePublication);
    candidate.originals = originals; candidate.replacements = clones;
    candidate.kernelImage = useNative ? 2u : 1u;
    candidate.cacheStatus = gNativeCacheStatus.load();
    const auto result = PublishProgram(candidate, useNative ? nativeProgram : ptxProgram);
    if (!result.success)
    {
        if (!result.attempted) discard();
        else { gPublicationFailed = true; gReady.store(false, std::memory_order_release); }
        Log(L"Ampere program publication rejected: slot=%u attempted=%u rollbackVerified=%u",
            static_cast<uint32_t>(result.failedSlot), static_cast<unsigned>(result.attempted),
            static_cast<unsigned>(result.rollbackComplete));
        return fail(Failure::ePublication);
    }
    auto& program = gPrograms[gProgramCount++];
    program = std::move(candidate);
    if (!gProvider) SelectProgram(program);
    gPreparation.store(Preparation::eReady);
    SetFailure(Failure::eNone);
    Log(L"Ampere auxiliary font published: SM86 %s bytes=%zu sourceBytes=%zu RVA=0x%zX; shared CUDA preflight and publication transaction",
        useNative ? L"native" : L"PTX", useNative ? fontNative.size() : fontPtx.size(), kFontBytes,
        program.font->address - reinterpret_cast<uintptr_t>(module));
    Log(L"Ampere SM86 program published: %zu %s slots, variable temporal positions; cache=%u; table RVA=0x%zX; registration RVA=0x%zX",
        slotCount, useNative ? L"native" : L"PTX", gNativeCacheStatus.load(), table - reinterpret_cast<uintptr_t>(module),
        discovery.registration - reinterpret_cast<uintptr_t>(module));
    return true;
}
bool PreparedProvider(HMODULE module) noexcept
{
    std::lock_guard lock(gMutex);
    const auto* program = FindPublishedProgram(module);
    return gAdapterVerified.load() && !gPublicationFailed && program
        && ProgramCurrent(*program, gAdapterLuid.load());
}
bool BindProvider(HMODULE module) noexcept
{
    std::lock_guard lock(gMutex);
    if (!gAdapterVerified.load() || gPublicationFailed
        || (gProviderBound && module != gProvider)) return false;
    const auto* program = FindPublishedProgram(module);
    if (!program || !ProgramCurrent(*program, gAdapterLuid.load()))
    {
        if (gProviderBound && module == gProvider)
        {
            gReady.store(false, std::memory_order_release);
            gPublicationFailed = true;
            SetFailure(Failure::eRestartRequired);
        }
        return false;
    }
    const bool firstBinding = !gProviderBound;
    SelectProgram(*program);
    gProviderBound = true;
    gPreparation.store(Preparation::eReady);
    SetFailure(Failure::eNone);
    if (firstBinding)
        Log(L"Ampere real provider Create bound immutable program: provider=%p publication=0x%llX image=%u cache=%u",
            module, static_cast<unsigned long long>(program->table), program->kernelImage, program->cacheStatus);
    return true;
}
bool AdapterVerified() noexcept { return gAdapterVerified.load(std::memory_order_acquire); }
bool Ready() noexcept { return gReady.load(std::memory_order_acquire); }
uint32_t FailureCode() noexcept { return gFailure.load(std::memory_order_acquire); }
uint32_t PreparationStage() noexcept { return static_cast<uint32_t>(gPreparation.load(std::memory_order_acquire)); }
uint32_t KernelImage() noexcept
{
    const auto* program = gSelectedProgram.load(std::memory_order_acquire);
    return Ready() && program ? program->kernelImage : 0;
}
uint32_t NativeCacheStatus() noexcept
{
    const auto* program = gSelectedProgram.load(std::memory_order_acquire);
    return program ? program->cacheStatus : gNativeCacheStatus.load(std::memory_order_acquire);
}
uint64_t AdapterLuid() noexcept { return gAdapterLuid.load(std::memory_order_acquire); }
uint64_t Publication() noexcept
{
    const auto* program = gSelectedProgram.load(std::memory_order_acquire);
    return Ready() && program ? program->table : 0;
}
HMODULE Provider() noexcept
{
    const auto* program = gSelectedProgram.load(std::memory_order_acquire);
    return Ready() && program ? program->module : nullptr;
}
} // namespace ampere_gpu
