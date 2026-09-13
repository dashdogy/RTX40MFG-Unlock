#include "ampere_backend.h"
#include "fault_capture.h"
#include "ngx_runtime_dispatch.h"
#if MFG_UNLOCK_RUNTIME_GPU_SELECTION
#include "gpu_dispatch.h"
#endif
#include "ampere_patterns.h"
#include "ampere_host_contracts.h"
#include "ampere_policy.h"
#include "dlssg_provider_policy.h"
#include "dlssg_preset.h"
#if MFG_UNLOCK_OUTPUT_PULL_MASK_ONLY
#include "midpoint_fix.h"
#endif
#include "protected_pointer.h"
#include "third_party/minhook/src/hde/hde64.h"
#include <d3d12.h>
#include <dxgi.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include <intrin.h>
#include <psapi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

namespace ampere_backend
{
namespace
{
using namespace ampere_patterns;
constexpr size_t kRoutes = 8;
constexpr auto kRejected = NVSDK_NGX_Result_FAIL_FeatureNotSupported;
using CreateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, const NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using EvaluateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
using ReleaseFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
using RequirementsFn = NVSDK_NGX_Result(NVSDK_CONV*)(IDXGIAdapter*, const NVSDK_NGX_FeatureDiscoveryInfo*, NVSDK_NGX_FeatureRequirement*);
using ParametersFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
using PresetFn = bool(*)(uint32_t, uint32_t*);

struct Image
{
    HMODULE module{};
    uintptr_t base{};
    uint32_t size{};
    const IMAGE_NT_HEADERS64* nt{};
    bool Open(HMODULE value) noexcept
    {
        module = value; base = reinterpret_cast<uintptr_t>(value);
        __try
        {
            const auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(value);
            if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000) return false;
            nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
            size = nt->OptionalHeader.SizeOfImage;
            const uintptr_t sectionEnd = reinterpret_cast<uintptr_t>(IMAGE_FIRST_SECTION(nt) + nt->FileHeader.NumberOfSections);
            return size >= 4096 && size < 1024u * 1024u * 1024u && sectionEnd >= base && sectionEnd - base <= size;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool Contains(uintptr_t address, size_t bytes = 1) const noexcept
    { return address >= base && address - base < size && bytes <= size - (address - base); }
    bool Executable(uintptr_t address) const noexcept
    {
        MEMORY_BASIC_INFORMATION m{};
        return Contains(address) && VirtualQuery(reinterpret_cast<void*>(address), &m, sizeof(m)) == sizeof(m)
            && m.AllocationBase == module && m.Type == MEM_IMAGE && m.State == MEM_COMMIT
            && (m.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0
            && ((m.Protect & 0xff) == PAGE_EXECUTE_READ || (m.Protect & 0xff) == PAGE_EXECUTE);
    }
};

std::recursive_mutex gCalls;
std::mutex gInstall;
std::atomic<LogCallback> gLog{nullptr};
std::atomic<bool> gFatal{false}, gStartupComplete{false};
std::atomic<bool> gEarlyInit{false};
std::atomic<bool> gD3D12Confirmed{false};
std::atomic<uint32_t> gFailure{100};
std::atomic<uint32_t> gFatalFailure{0};
std::atomic<uint64_t> gPresetQueries{0}, gEvaluations{0}, gRejections{0};
struct LegacyPresetRouteProof
{
    std::atomic<HMODULE> provider{nullptr};
    std::atomic<uint64_t> generation{0};
};
std::array<LegacyPresetRouteProof, kRoutes> gLegacyPresetRoutes{};
std::atomic<uint32_t> gNativeMaximum{0};
std::atomic<uint32_t> gNativeMaximumResult{static_cast<uint32_t>(NVSDK_NGX_Result_FAIL_NotInitialized)};
std::atomic<uint32_t> gStartupMaximum{0}, gPresentationBuffers{0};
std::atomic<uint32_t> gCertifiedMaximum{0};
std::atomic<uint32_t> gStartupFailureMask{0};
std::atomic<uintptr_t> gActiveWrapper{0};
std::atomic<uint64_t> gActiveWrapperGeneration{0};
std::atomic_flag gWrapperWriter = ATOMIC_FLAG_INIT;
std::atomic<int32_t> gLastCreateCount{-1};
std::atomic<uint64_t> gCreatedFeatures{0}, gSubmittedBatches{0};
std::atomic<uint64_t> gCreateAttempts{0}, gCreateBlockedBeforeProvider{0}, gEvaluateAttempts{0};
std::atomic<bool> gPipelineCreateObserved{false};
std::atomic<uint64_t> gCandidateProviderVersion{0};
ampere_diagnostics::Failures gFailures;
thread_local unsigned gStartupDepth = 0;
thread_local unsigned gPreparationDepth = 0;
std::atomic<PreparationBoundaryResolver> gPreparationBoundaryResolver{nullptr};
thread_local unsigned gInitDepth = 0;
thread_local unsigned gInitLockDepth = 0;
thread_local unsigned gCreateDepth = 0;
void Report(const wchar_t* text) noexcept { if (auto log = gLog.load()) log(text); }
bool Reject(uint32_t failure) noexcept
{
    if (failure == 151 && ampere_gpu::FailureCode())
        gFailures.Record(ampere_gpu::FailureCode());
    gFailures.Record(failure);
    gCertifiedMaximum.store(0, std::memory_order_release);
    if (gFatal.load())
    {
        uint32_t empty = 0;
        gFatalFailure.compare_exchange_strong(empty, failure);
        if (empty) { gRejections.fetch_add(1); return false; }
    }
    if (gFailure.exchange(failure) != failure)
    {
        wchar_t message[96]{};
        swprintf_s(message, L"Ampere boundary rejected: failure=%u", failure);
        Report(message);
    }
    gRejections.fetch_add(1);
    return false;
}

bool Pin(HMODULE module) noexcept
{
    HMODULE pinned = nullptr;
    return module && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(module), &pinned) && pinned == module;
}

// Signature masks permit relocation operands, not opcode or layout changes.
// Decode the whole window and require every relative call/LEA and branch to
// belong to the same image. No driver version or fixed RVA grants admission.
bool DecodedWindow(const Image& image, uintptr_t address, size_t length) noexcept
{
    size_t offset = 0;
    while (offset < length)
    {
        hde64s ins{};
        const unsigned n = hde64_disasm(reinterpret_cast<void*>(address + offset), &ins);
        if (!n || (ins.flags & F_ERROR) || n > length - offset) return false;
        const auto* bytes = reinterpret_cast<const uint8_t*>(address + offset);
        if (ins.opcode == 0xe8)
        {
            int32_t displacement = 0; memcpy(&displacement, bytes + n - 4, 4);
            if (!image.Executable(address + offset + n + displacement)) return false;
        }
        if (ins.opcode == 0x8d && ins.modrm_mod == 0 && ins.modrm_rm == 5)
        {
            int32_t displacement = 0; memcpy(&displacement, bytes + n - 4, 4);
            if (!image.Contains(address + offset + n + displacement)) return false;
        }
        offset += n;
    }
    return offset == length;
}

uintptr_t FindPattern(const Image& image, const uint8_t* pattern, const uint8_t* mask,
    size_t length, size_t branchOffset = SIZE_MAX, uint32_t minimum = 0, uint32_t maximum = 0,
    unsigned* matches = nullptr) noexcept
{
    if (matches) *matches = 0;
    uintptr_t match = 0;
    const auto* sections = IMAGE_FIRST_SECTION(image.nt);
    for (unsigned i = 0; i < image.nt->FileHeader.NumberOfSections; ++i)
    {
        const auto& s = sections[i];
        if (!(s.Characteristics & IMAGE_SCN_MEM_EXECUTE) || s.VirtualAddress >= image.size) continue;
        const uint32_t begin = std::max<uint32_t>(s.VirtualAddress, minimum);
        const uint32_t end = static_cast<uint32_t>(std::min<uint64_t>(
            std::min<uint64_t>(image.size, maximum ? maximum : image.size),
            uint64_t{s.VirtualAddress} + std::max(s.Misc.VirtualSize, s.SizeOfRawData)));
        for (uint64_t offset = begin; offset + length <= end; ++offset)
        {
            const auto* candidate = reinterpret_cast<const uint8_t*>(image.base + offset);
            bool same = true;
            for (size_t j = 0; same && j < length; ++j) same = !mask[j] || candidate[j] == pattern[j];
            if (!same || !DecodedWindow(image, image.base + offset, length)) continue;
            if (branchOffset != SIZE_MAX)
            {
                const int displacement = static_cast<int8_t>(candidate[branchOffset + 1]);
                if (displacement <= 0 || !image.Executable(image.base + offset + branchOffset + 2 + displacement)) continue;
            }
            if (matches) ++*matches;
            if (match) return 0;
            match = image.base + offset;
        }
    }
    return match;
}

bool UnwindRoot(const Image& image, RUNTIME_FUNCTION function, RUNTIME_FUNCTION& root) noexcept
{
    for (unsigned depth = 0; depth != 8; ++depth)
    {
        const uintptr_t unwind = image.base + function.UnwindData;
        if (!image.Contains(unwind, 4)) return false;
        const auto* bytes = reinterpret_cast<const uint8_t*>(unwind);
        if ((bytes[0] & 7u) != 1u) return false;
        const unsigned flags = bytes[0] >> 3;
        if (!(flags & UNW_FLAG_CHAININFO)) { root = function; return true; }
        if (flags & (UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER)) return false;
        const size_t offset = (4u + 2u * bytes[2] + 3u) & ~size_t{3};
        if (!image.Contains(unwind, offset + sizeof(RUNTIME_FUNCTION))) return false;
        RUNTIME_FUNCTION parent{};
        memcpy(&parent, bytes + offset, sizeof(parent));
        DWORD64 owner = 0;
        const auto* actual = RtlLookupFunctionEntry(image.base + parent.BeginAddress, &owner, nullptr);
        if (!actual || owner != image.base
            || !image.Contains(reinterpret_cast<uintptr_t>(actual), sizeof(*actual))
            || memcmp(actual, &parent, sizeof(parent))) return false;
        function = parent;
    }
    return false;
}

bool FunctionBounds(const Image& image, uintptr_t entry, uint32_t& begin, uint32_t& end) noexcept
{
    DWORD64 owner = 0;
    const auto* function = RtlLookupFunctionEntry(entry, &owner, nullptr);
    if (!function || owner != image.base
        || !image.Contains(reinterpret_cast<uintptr_t>(function), sizeof(*function))
        || function->BeginAddress >= function->EndAddress
        || function->EndAddress > image.size
        || image.base + function->BeginAddress != entry
        || function->EndAddress - function->BeginAddress > 0x4000
        || !image.Executable(entry) || !image.Executable(image.base + function->EndAddress - 1))
        return false;
    begin = function->BeginAddress;
    end = function->EndAddress;
    RUNTIME_FUNCTION root{};
    if (!UnwindRoot(image, *function, root) || root.BeginAddress != begin) return false;
    // MSVC splits one function's unwind coverage when a nonvolatile register
    // is saved only on one branch. Adjacent entries with an exact chained
    // unwind parent are continuations, not independent exported functions.
    for (unsigned fragments = 0; fragments != 32; ++fragments)
    {
        owner = 0;
        const auto* next = RtlLookupFunctionEntry(image.base + end, &owner, nullptr);
        if (!next || owner != image.base
            || !image.Contains(reinterpret_cast<uintptr_t>(next), sizeof(*next))
            || next->BeginAddress != end) return true;
        RUNTIME_FUNCTION nextRoot{};
        if (!UnwindRoot(image, *next, nextRoot)) return false;
        if (memcmp(&root, &nextRoot, sizeof(root))) return true;
        if (next->EndAddress <= end || next->EndAddress > image.size
            || next->EndAddress - begin > 0x4000
            || !image.Executable(image.base + next->EndAddress - 1)) return false;
        end = next->EndAddress;
    }
    return false;
}

// NGX may outline Create's architecture validation into a helper far from
// its public export. Inspect only that function and its decoded direct callees;
// neighboring API implementations and unrelated image-wide matches grant no
// coverage. Resolve before installing our Create entry detour, then retain the
// owned addresses for the scoped startup/Create patches.
uintptr_t FindCreateValidation(const Image& image, uintptr_t create) noexcept
{
    uint32_t begin = 0, end = 0;
    if (!FunctionBounds(image, create, begin, end)) return 0;
    std::array<uintptr_t, 65> functions{};
    functions[0] = create;
    size_t count = 1;
    for (uintptr_t cursor = create; cursor < image.base + end;)
    {
        hde64s ins{};
        const unsigned length = hde64_disasm(reinterpret_cast<void*>(cursor), &ins);
        if (!length || (ins.flags & F_ERROR) || length > image.base + end - cursor) return 0;
        if (ins.opcode == 0xe8)
        {
            int32_t displacement = 0;
            memcpy(&displacement, reinterpret_cast<void*>(cursor + length - 4), sizeof(displacement));
            const uintptr_t target = cursor + length + displacement;
            uint32_t childBegin = 0, childEnd = 0;
            if (FunctionBounds(image, target, childBegin, childEnd)
                && std::find(functions.begin(), functions.begin() + count, target) == functions.begin() + count)
            {
                if (count == functions.size()) return 0;
                functions[count++] = target;
            }
        }
        cursor += length;
    }
    uintptr_t selected = 0;
    for (size_t i = 0; i < count; ++i)
    {
        if (!FunctionBounds(image, functions[i], begin, end)) return 0;
        unsigned matches = 0;
        const uintptr_t candidate = FindPattern(image, kAmpereNgxCreateValidationPattern.data(),
            kAmpereNgxCreateValidationPatternMask.data(), kAmpereNgxCreateValidationPattern.size(),
            kAmpereNgxCreateValidationBranchOffset, begin, end, &matches);
        if (matches > 1 || (candidate && selected)) return 0;
        if (candidate) selected = candidate;
    }
    return selected;
}

struct Patch
{
    uintptr_t address = 0;
    uint8_t original = 0;
    bool active = false;
    bool privatePage = false;
    bool Set(bool enable) noexcept
    {
        if (!address) return false;
        if (active == enable) return *reinterpret_cast<const uint8_t*>(address) == (enable ? 0xeb : original);
        const uint8_t expected = enable ? original : 0xeb;
        const uint8_t replacement = enable ? 0xeb : original;
        // An entry detour can already have made this image page private.
        // Re-arming WRITECOPY on such a page fails on Windows; establish its
        // working-set ownership before selecting the private-page route.
        PSAPI_WORKING_SET_EX_INFORMATION working{};
        working.VirtualAddress = reinterpret_cast<void*>(address);
        const bool alreadyPrivate = QueryWorkingSetEx(GetCurrentProcess(), &working, sizeof(working))
            && working.VirtualAttributes.Valid && !working.VirtualAttributes.Shared;
        const auto result = protected_pointer::ReplaceProtectedBytes(address,
            &expected, &replacement, 1, PAGE_EXECUTE_READ, &VirtualProtect,
            &FlushInstructionCache, privatePage || alreadyPrivate ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_WRITECOPY);
        privatePage |= result.replacementWasPublished;
        if (result.disposition != protected_pointer::PublishDisposition::ePublishedRestored)
        { gFatal.store(true); return Reject(110); }
        active = enable;
        return true;
    }
};

struct Route
{
    HMODULE module{};
    uint64_t generation{};
    bool runtime = false;
    bool prepared = false;
    bool runtimeInspected = false;
    bool deviceGateReady = false;
    bool legacyPresetReady = false;
    uint32_t nativeMaximum = 0;
    uint32_t startupMaximum = 0;
    bool wrapperInspected = false;
    bool candidateLogged = false;
    ampere_wrapper::Layout wrapperLayout{};
    entry_detour::Handle create{}, evaluate{}, release{}, requirements{}, parameters{}, preset{};
    entry_detour::ForwardPreCall beforeCreate{}, beforeEvaluate{};
    Patch metadata{}, validation{}, availability{};
    std::array<ampere_policy::Feature, 16> features{};
    uint64_t nextFeatureLifetime = 0;
    struct OtherFeature
    {
        enum class State : uint8_t { eFree, eCreating, eActive, eReleasing };
        uintptr_t handle = 0;
        uint64_t lifetime = 0;
        uint32_t activeCalls = 0;
        State state = State::eFree;

        OtherFeature() = default;
        OtherFeature(uintptr_t value) noexcept { *this = value; }
        OtherFeature& operator=(uintptr_t value) noexcept
        {
            handle = value;
            lifetime = value ? 1 : 0;
            activeCalls = 0;
            state = !value ? State::eFree
                : value == UINTPTR_MAX ? State::eCreating : State::eActive;
            return *this;
        }
        friend bool operator==(const OtherFeature& feature, uintptr_t value) noexcept
        { return feature.handle == value && (value == 0 || feature.state != State::eFree); }
        explicit operator bool() const noexcept
        { return state != State::eFree && handle != 0; }
        bool Free() const noexcept { return state == State::eFree; }
        bool Active(uintptr_t value) const noexcept
        { return state == State::eActive && handle == value && lifetime != 0; }
        void Clear() noexcept { *this = {}; }
    };
    std::vector<OtherFeature> otherFeatures;
    uint64_t nextOtherFeatureLifetime = 0;
};
std::array<Route, kRoutes> gRoutes{};
struct CreateAttemptProof
{
    entry_detour::Handle runtimeCreate{}, providerCreate{};
    uintptr_t runtime = 0, provider = 0, wrapper = 0;
    uint64_t runtimeGeneration = 0, providerGeneration = 0, wrapperGeneration = 0;
    uint64_t token = 0, publication = 0, luid = 0;
    ampere_wrapper::Layout layout{};
    ampere_wrapper::Capacity capacity{};
    bool active = false, valid = false;
};
std::mutex gCreateAttemptMutex;
std::array<CreateAttemptProof, 64> gCreateAttemptsInProgress{};
thread_local size_t gCurrentCreateAttempt = SIZE_MAX;

bool CreateAttemptProofCurrent(const CreateAttemptProof& proof) noexcept
{
    if (!Ready() || !proof.valid || !proof.token || !proof.runtime || !proof.provider || !proof.wrapper
        || !proof.runtimeGeneration || !proof.providerGeneration || !proof.wrapperGeneration
        || !proof.publication || !proof.luid) return false;
    const auto runtime = entry_detour::ReadSnapshot(proof.runtimeCreate);
    const auto provider = entry_detour::ReadSnapshot(proof.providerCreate);
    return runtime.current && runtime.kind == entry_detour::Kind::eNgxRuntimeD3D12CreateFeature
        && reinterpret_cast<uintptr_t>(runtime.owner) == proof.runtime
        && runtime.generation == proof.runtimeGeneration
        && provider.current && provider.kind == entry_detour::Kind::eNgxD3D12CreateFeature
        && reinterpret_cast<uintptr_t>(provider.owner) == proof.provider
        && provider.generation == proof.providerGeneration
        && proof.provider == reinterpret_cast<uintptr_t>(ampere_gpu::Provider())
        && proof.publication == ampere_gpu::Publication() && proof.luid == ampere_gpu::AdapterLuid()
        && proof.wrapper == gActiveWrapper.load(std::memory_order_acquire)
        && proof.wrapperGeneration == gActiveWrapperGeneration.load(std::memory_order_acquire)
        && proof.layout.module == reinterpret_cast<HMODULE>(proof.wrapper)
        && ampere_wrapper::Matches(proof.layout, proof.capacity);
}

// Native calls remain serialized by gCalls. These bounded snapshots let a
// reentrant/control thread distinguish a proven unfinished Create from stale
// retained ownership without acquiring gCalls under a Streamline callback.
struct CreateAttemptScope
{
    size_t slot = SIZE_MAX;
    explicit CreateAttemptScope(const Route& runtime) noexcept
    {
        std::lock_guard lock(gCreateAttemptMutex);
        if (gCurrentCreateAttempt != SIZE_MAX) return;
        for (size_t i = 0; i < gCreateAttemptsInProgress.size(); ++i)
        {
            auto& proof = gCreateAttemptsInProgress[i];
            if (proof.active) continue;
            proof = {};
            proof.runtimeCreate = runtime.create;
            proof.runtime = reinterpret_cast<uintptr_t>(runtime.module);
            proof.runtimeGeneration = runtime.generation;
            proof.publication = ampere_gpu::Publication();
            proof.luid = ampere_gpu::AdapterLuid();
            proof.active = proof.valid = true;
            slot = gCurrentCreateAttempt = i;
            break;
        }
    }
    CreateAttemptScope(const CreateAttemptScope&) = delete;
    CreateAttemptScope& operator=(const CreateAttemptScope&) = delete;
    ~CreateAttemptScope() { Finish(); }
    explicit operator bool() const noexcept { return slot != SIZE_MAX; }
    void BindCapacity(const Route& wrapper, const ampere_wrapper::Capacity& capacity) noexcept
    {
        std::lock_guard lock(gCreateAttemptMutex);
        if (slot == SIZE_MAX) return;
        auto& proof = gCreateAttemptsInProgress[slot];
        proof.wrapper = reinterpret_cast<uintptr_t>(wrapper.module);
        proof.wrapperGeneration = wrapper.generation;
        proof.layout = wrapper.wrapperLayout;
        proof.capacity = capacity;
    }
    CreateAttemptProof Read() const noexcept
    {
        std::lock_guard lock(gCreateAttemptMutex);
        return slot == SIZE_MAX ? CreateAttemptProof{} : gCreateAttemptsInProgress[slot];
    }
    void Finish() noexcept
    {
        std::lock_guard lock(gCreateAttemptMutex);
        if (slot == SIZE_MAX) return;
        gCreateAttemptsInProgress[slot] = {};
        if (gCurrentCreateAttempt == slot) gCurrentCreateAttempt = SIZE_MAX;
        slot = SIZE_MAX;
    }
};

void AbortCurrentCreateAttempt() noexcept
{
    std::lock_guard lock(gCreateAttemptMutex);
    if (gCurrentCreateAttempt == SIZE_MAX) return;
    gCreateAttemptsInProgress[gCurrentCreateAttempt] = {};
    gCurrentCreateAttempt = SIZE_MAX;
}

bool BindCurrentCreateProvider(const Route& provider) noexcept
{
    std::lock_guard lock(gCreateAttemptMutex);
    if (gCurrentCreateAttempt == SIZE_MAX) return false;
    auto& proof = gCreateAttemptsInProgress[gCurrentCreateAttempt];
    if (!proof.active || !proof.valid) return false;
    const auto owner = reinterpret_cast<uintptr_t>(provider.module);
    if (proof.provider && (proof.provider != owner || proof.providerGeneration != provider.generation
        || proof.providerCreate != provider.create))
    { proof.valid = false; return false; }
    const uint64_t publication = ampere_gpu::Publication();
    if (ampere_gpu::Provider() != provider.module || !publication
        || proof.luid != ampere_gpu::AdapterLuid()
        || (proof.provider && proof.publication != publication))
    { proof.valid = false; return false; }
    // The first concrete provider binding selects one of the already-published
    // candidates. After that selection its immutable program cannot change.
    proof.publication = publication;
    proof.provider = owner;
    proof.providerGeneration = provider.generation;
    proof.providerCreate = provider.create;
    return true;
}

uint64_t CurrentCreateAttemptToken() noexcept
{
    std::lock_guard lock(gCreateAttemptMutex);
    if (gCurrentCreateAttempt == SIZE_MAX) return 0;
    const auto& proof = gCreateAttemptsInProgress[gCurrentCreateAttempt];
    return proof.active && proof.valid ? proof.token : 0;
}

struct PendingFeatureLifetimeEvent
{
    FeatureLifetimeEvent event{};
    CreateAttemptProof create{};
};
std::atomic<FeatureLifetimeCallback> gFeatureLifetimeCallback{nullptr};
std::mutex gFeatureLifetimeDeliveryMutex;
std::deque<PendingFeatureLifetimeEvent> gPendingFeatureLifetimeEvents;
PendingFeatureLifetimeEvent gDeliveringFeatureLifetimeEvent{};
bool gFeatureLifetimeDeliveryActive = false;
thread_local Route* gCallingRuntime = nullptr;
thread_local Route* gCreatingProvider = nullptr;
thread_local ngx_runtime_dispatch::Selection gCreatingDispatch{};
thread_local Route* gCallingWrapper = nullptr;
thread_local ampere_wrapper::Capacity gCreatingCapacity{};
thread_local ampere_policy::Feature* gCallingFeature = nullptr;
thread_local unsigned gEvaluateDepth = 0, gReleaseDepth = 0;
thread_local bool gProviderEvaluated = false;
struct EvaluationInput
{
    int count = 0, index = 0;
    uint64_t frame = 0;
    ID3D12Resource* output = nullptr;
    bool operator==(const EvaluationInput&) const = default;
};
struct EvaluationReads
{
    uint32_t count = 0, index = 0, frame = 0, output = 0;
    bool exception = false;
};
thread_local EvaluationInput gEvaluationInput{};

NVSDK_NGX_Result InvokeCreate(Route& route, CreateFn original,
    ID3D12GraphicsCommandList* command, NVSDK_NGX_Feature feature,
    const NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** output, bool& restored, Route*& provider,
    Route& wrapper, const ampere_wrapper::Capacity& capacity, uint64_t& createAttemptToken)
{
    NVSDK_NGX_Result result = kRejected;
    bool completed = false;
    createAttemptToken = 0;
    ++gCreateDepth;
    gCallingRuntime = &route; gCreatingProvider = nullptr;
    gCallingWrapper = &wrapper; gCreatingCapacity = capacity;
    __try
    {
        const auto dispatch = route.runtime ? ngx_runtime_dispatch::Read(entry_detour::ReadSnapshot(route.create),
            static_cast<uint32_t>(feature), gCreatingDispatch) : ngx_runtime_dispatch::ReadResult::eUnrecognized;
        if (dispatch == ngx_runtime_dispatch::ReadResult::eInvalid)
            Reject(ampere_diagnostics::kRuntimeDispatchFailure);
        else result = original(command, feature, parameters, output);
        createAttemptToken = CurrentCreateAttemptToken();
        completed = true;
    }
    __finally
    {
        --gCreateDepth;
        provider = gCreatingProvider;
        gCallingRuntime = nullptr; gCreatingProvider = nullptr;
        gCreatingDispatch = {};
        gCallingWrapper = nullptr; gCreatingCapacity = {};
        restored = !route.runtime || route.validation.Set(false);
        // /EHsc does not unwind an outer C++ scope for a caught SEH exception.
        if (!completed) AbortCurrentCreateAttempt();
    }
    return result;
}

NVSDK_NGX_Result InvokeRelease(Route& route, ReleaseFn original, NVSDK_NGX_Handle* handle,
    ampere_policy::Feature& feature)
{
    gCallingRuntime = &route; gCallingFeature = &feature; ++gReleaseDepth;
    __try { return original(handle); }
    __finally { --gReleaseDepth; gCallingRuntime = nullptr; gCallingFeature = nullptr; }
}

NVSDK_NGX_Result InvokeEvaluate(Route& route, EvaluateFn original, ID3D12GraphicsCommandList* command,
    const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback,
    ampere_policy::Feature& feature, bool& providerExecuted)
{
    gCallingRuntime = &route; gCallingFeature = &feature; ++gEvaluateDepth; gProviderEvaluated = false;
    NVSDK_NGX_Result result = kRejected;
    __try { result = original(command, handle, parameters, callback); return result; }
    __finally
    {
        providerExecuted = gProviderEvaluated;
        if (result != NVSDK_NGX_Result_Success || !providerExecuted) feature.batch.failed = true;
        gEvaluationInput = {};
        --gEvaluateDepth; gCallingRuntime = nullptr; gCallingFeature = nullptr;
    }
}

template<class F> F Original(entry_detour::Handle handle) noexcept
{
    const auto state = entry_detour::ReadSnapshot(handle);
    return state.current ? reinterpret_cast<F>(state.original) : nullptr;
}

template<class F> F RouteOriginal(const Route& route, entry_detour::Handle handle) noexcept
{
    const auto state = entry_detour::ReadSnapshot(handle);
    // Entry owners and trampolines are pinned for the process lifetime. Take
    // the exact route/generation snapshot while gCalls protects route metadata,
    // before neutral forwarding drops the Ampere lifecycle lock.
    return state.current && state.owner == route.module && state.generation == route.generation
        ? reinterpret_cast<F>(state.original) : nullptr;
}

bool RouteEntryCurrent(const Route& route, entry_detour::Handle handle) noexcept
{
    const auto state = entry_detour::ReadSnapshot(handle);
    return route.module && route.generation && state.current
        && state.owner == route.module && state.generation == route.generation;
}

size_t ReserveOtherFeature(Route& route) noexcept
{
    // Reserve before calling native Create: it may reenter Create and grow the
    // vector. A stable index, rather than an iterator, survives that growth.
    uint64_t lifetime = ++route.nextOtherFeatureLifetime;
    if (!lifetime) lifetime = ++route.nextOtherFeatureLifetime;
    const auto free = std::find_if(route.otherFeatures.begin(), route.otherFeatures.end(),
        [](const Route::OtherFeature& feature) { return feature.Free(); });
    if (free != route.otherFeatures.end())
    {
        const size_t index = static_cast<size_t>(free - route.otherFeatures.begin());
        free->handle = 0;
        free->lifetime = lifetime;
        free->activeCalls = 0;
        free->state = Route::OtherFeature::State::eCreating;
        return index;
    }
    try
    {
        route.otherFeatures.emplace_back();
        auto& feature = route.otherFeatures.back();
        feature.lifetime = lifetime;
        feature.state = Route::OtherFeature::State::eCreating;
    }
    catch (...) { return SIZE_MAX; }
    return route.otherFeatures.size() - 1;
}

struct OtherFeatureLease
{
    size_t route = SIZE_MAX;
    HMODULE module = nullptr;
    uint64_t generation = 0;
    size_t slot = SIZE_MAX;
    uint64_t lifetime = 0;
    uintptr_t handle = 0;
};

OtherFeatureLease SnapshotOtherFeature(size_t routeIndex, const Route& route,
    size_t slot, uintptr_t handle = 0) noexcept
{
    return {routeIndex, route.module, route.generation, slot,
        slot < route.otherFeatures.size() ? route.otherFeatures[slot].lifetime : 0,
        handle};
}

Route::OtherFeature* CurrentOtherFeature(const OtherFeatureLease& lease) noexcept
{
    if (lease.route >= gRoutes.size()) return nullptr;
    auto& route = gRoutes[lease.route];
    if (route.module != lease.module || route.generation != lease.generation
        || lease.slot >= route.otherFeatures.size()) return nullptr;
    auto& feature = route.otherFeatures[lease.slot];
    return feature.lifetime == lease.lifetime && lease.lifetime ? &feature : nullptr;
}

size_t FindActiveOtherFeature(const Route& route, uintptr_t handle) noexcept
{
    if (!handle) return SIZE_MAX;
    size_t found = SIZE_MAX;
    for (size_t i = 0; i != route.otherFeatures.size(); ++i)
    {
        const auto& feature = route.otherFeatures[i];
        // An old Release may still be executing while a nested Create receives
        // the same numeric handle. Do not send another operation for that value
        // until the old native completion has reconciled its exact lifetime.
        if (feature.state == Route::OtherFeature::State::eReleasing
            && feature.handle == handle) return SIZE_MAX;
        if (!feature.Active(handle)) continue;
        if (found != SIZE_MAX) return SIZE_MAX;
        found = i;
    }
    return found;
}

FeatureLifetimeEvent LifetimeEvent(FeatureLifetimePhase phase,
    const ampere_policy::Feature& feature) noexcept
{
    return {phase, feature.handle, feature.lifetime, feature.owner, feature.generation,
        feature.provider, feature.providerGeneration, feature.wrapper,
        feature.wrapperGeneration, feature.publication, feature.luid,
        feature.createdGeneratedFrames, feature.createAttemptToken};
}

void NotifyFeatureLifetime(const FeatureLifetimeEvent& event) noexcept
{
    if (const auto callback = gFeatureLifetimeCallback.load(std::memory_order_acquire))
    {
        __try { callback(event); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

bool QueueFeatureLifetime(const FeatureLifetimeEvent& event,
    const CreateAttemptProof& create = {}) noexcept
{
    // Callers enqueue while gCalls still serializes the native state change.
    // Delivery is deliberately separate: an observer may call back into a
    // Streamline path and must never inherit the Ampere lifecycle lock.
    try
    {
        std::lock_guard lock(gFeatureLifetimeDeliveryMutex);
        gPendingFeatureLifetimeEvents.push_back({event, create});
        return true;
    }
    catch (...)
    {
        gFatal.store(true, std::memory_order_release);
        Reject(172);
        return false;
    }
}

void DrainFeatureLifetimeEvents() noexcept
{
    std::unique_lock lock(gFeatureLifetimeDeliveryMutex);
    if (gFeatureLifetimeDeliveryActive)
        return;
    gFeatureLifetimeDeliveryActive = true;
    while (!gPendingFeatureLifetimeEvents.empty())
    {
        const PendingFeatureLifetimeEvent pending =
            gPendingFeatureLifetimeEvents.front();
        gPendingFeatureLifetimeEvents.pop_front();
        gDeliveringFeatureLifetimeEvent = pending;
        lock.unlock();
        NotifyFeatureLifetime(pending.event);
        lock.lock();
        gDeliveringFeatureLifetimeEvent = {};
    }
    gFeatureLifetimeDeliveryActive = false;
}

uintptr_t CreatedHandle(NVSDK_NGX_Result result, NVSDK_NGX_Handle** output) noexcept
{
    if (result != NVSDK_NGX_Result_Success) return 0;
    __try { return output && *output ? reinterpret_cast<uintptr_t>(*output) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool ParameterCount(const NVSDK_NGX_Parameter* parameters, int& count) noexcept
{
    if (!parameters) return false;
    __try { return parameters->Get("DLSSG.MultiFrameCount", &count) == NVSDK_NGX_Result_Success; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void ObserveCreationCount(const NVSDK_NGX_Parameter* parameters) noexcept
{
    int count = -1;
    const bool read = ParameterCount(parameters, count);
    gLastCreateCount.store(read ? count : -1, std::memory_order_relaxed);
}

Route* ActiveWrapper() noexcept
{
    const uint64_t generation = gActiveWrapperGeneration.load(std::memory_order_acquire);
    const auto module = reinterpret_cast<HMODULE>(gActiveWrapper.load(std::memory_order_acquire));
    if (!generation || generation != gActiveWrapperGeneration.load(std::memory_order_acquire)) return nullptr;
    for (auto& route : gRoutes)
        if (route.module == module && route.generation == generation && route.wrapperLayout.module == module) return &route;
    return nullptr;
}
bool FeatureCapacityCurrent(const ampere_policy::Feature& feature) noexcept
{
    auto* wrapper = ActiveWrapper();
    return wrapper && reinterpret_cast<uintptr_t>(wrapper->module) == feature.wrapper
        && wrapper->generation == feature.wrapperGeneration
        && ampere_wrapper::Matches(wrapper->wrapperLayout, feature.capacity);
}

bool ReadEvaluationInput(const NVSDK_NGX_Parameter* parameters, EvaluationInput& input,
    EvaluationReads* observation = nullptr) noexcept
{
    input = {};
    EvaluationReads reads{};
    if (observation) *observation = reads;
    if (!parameters) return false;
    __try
    {
        // The compiled wrapper uses Set(void*) for both opaque values.
        // In this MSVC ABI that setter is vtable +0x00, not the first
        // overload in source declaration order. Integer reads fail in NGX.
        void* frame = nullptr;
        void* output = nullptr;
        reads.count = parameters->Get("DLSSG.MultiFrameCount", &input.count);
        reads.index = parameters->Get("DLSSG.MultiFrameIndex", &input.index);
        reads.frame = parameters->Get("DLSSG.BackbufferFrameID", &frame);
        reads.output = parameters->Get("DLSSG.OutputInterpolated", &output);
        if (observation) *observation = reads;
        input.frame = reinterpret_cast<uintptr_t>(frame);
        input.output = static_cast<ID3D12Resource*>(output);
        return reads.count == NVSDK_NGX_Result_Success && reads.index == NVSDK_NGX_Result_Success
            && reads.frame == NVSDK_NGX_Result_Success && reads.output == NVSDK_NGX_Result_Success
            && ampere_policy::EvaluationCountValid(input.count) && input.output != nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        reads.exception = true;
        if (observation) *observation = reads;
        return false;
    }
}

void ReportEvaluationRejection(const Route& route, const ampere_policy::Feature& feature,
    const EvaluationInput& input, const EvaluationReads& reads, uint32_t mask,
    uint64_t publication, uint64_t luid) noexcept
{
    // One bounded snapshot per poisoned feature, before the failed flag is set.
    // It explains rejection; none of these observations relax an admission gate.
    wchar_t message[1024]{};
    swprintf_s(message, L"Ampere first evaluation rejection: mask=0x%02X "
        L"get(count/index/frame/output)=%08X/%08X/%08X/%08X exception=%u "
        L"count=%d index=%d frame=%llu output=%p "
        L"batch(started/submitted/failed)=%u/%u/%u previousFrame=%llu firstOutput=%p "
        L"depth(create/evaluate/release)=%u/%u/%u ready(d3d/adapter/program/startup/fatal)=%u/%u/%u/%u/%u",
        mask, reads.count, reads.index, reads.frame, reads.output, static_cast<unsigned>(reads.exception),
        input.count, input.index, input.frame, input.output,
        static_cast<unsigned>(feature.batch.started), feature.batch.submitted, static_cast<unsigned>(feature.batch.failed),
        feature.batch.frame, reinterpret_cast<void*>(feature.batch.outputs[0]), gCreateDepth, gEvaluateDepth, gReleaseDepth,
        static_cast<unsigned>(gD3D12Confirmed.load()), static_cast<unsigned>(AdapterVerified()),
        static_cast<unsigned>(ampere_gpu::Ready()), static_cast<unsigned>(gStartupComplete.load()),
        static_cast<unsigned>(gFatal.load()));
    Report(message);
    swprintf_s(message, L"Ampere first evaluation ownership: handle=%p runtime=%p/%p generation=%llu/%llu "
        L"program=0x%llX/0x%llX luid=0x%llX/0x%llX createdCount=%u "
        L"wrapper=%p/%p generation=%llu/%llu (stored/current)",
        reinterpret_cast<void*>(feature.handle), reinterpret_cast<void*>(feature.owner), route.module,
        feature.generation, route.generation, feature.publication, publication, feature.luid, luid,
        feature.createdGeneratedFrames, reinterpret_cast<void*>(feature.wrapper),
        reinterpret_cast<void*>(gActiveWrapper.load()), feature.wrapperGeneration, gActiveWrapperGeneration.load());
    Report(message);
    if (const auto* wrapper = ActiveWrapper())
    {
        const auto observed = ampere_wrapper::InspectFailure(wrapper->wrapperLayout);
        const auto& current = observed.capacity;
        const auto& stored = feature.capacity;
        swprintf_s(message, L"Ampere first evaluation allocation: readable(context/buffers)=%u/%u maximum=%u count=%u "
            L"context=%p/%p swapchain=%p/%p vector=%p:%p:%p/%p:%p:%p "
            L"buffers=%p:%p:%p:%p:%p:%p/%p:%p:%p:%p:%p:%p (stored/current)",
            static_cast<unsigned>(observed.contextReadable), static_cast<unsigned>(observed.buffersReadable),
            observed.maximum, observed.bufferCount,
            reinterpret_cast<void*>(stored.context), reinterpret_cast<void*>(current.context),
            reinterpret_cast<void*>(stored.swapchain), reinterpret_cast<void*>(current.swapchain),
            reinterpret_cast<void*>(stored.buffersBegin), reinterpret_cast<void*>(stored.buffersEnd),
            reinterpret_cast<void*>(stored.buffersCapacity), reinterpret_cast<void*>(current.buffersBegin),
            reinterpret_cast<void*>(current.buffersEnd), reinterpret_cast<void*>(current.buffersCapacity),
            reinterpret_cast<void*>(stored.buffers[0]), reinterpret_cast<void*>(stored.buffers[1]),
            reinterpret_cast<void*>(stored.buffers[2]), reinterpret_cast<void*>(stored.buffers[3]),
            reinterpret_cast<void*>(stored.buffers[4]), reinterpret_cast<void*>(stored.buffers[5]),
            reinterpret_cast<void*>(current.buffers[0]), reinterpret_cast<void*>(current.buffers[1]),
            reinterpret_cast<void*>(current.buffers[2]), reinterpret_cast<void*>(current.buffers[3]),
            reinterpret_cast<void*>(current.buffers[4]), reinterpret_cast<void*>(current.buffers[5]));
        Report(message);
    }
}

bool OutputMatchesDevice(ID3D12Resource* output, ID3D12Device* device, D3D12_RESOURCE_DESC& desc) noexcept
{
    ID3D12Device* owner = nullptr;
    bool valid = false;
    __try
    {
        if (output && device && SUCCEEDED(output->GetDevice(__uuidof(ID3D12Device),
                reinterpret_cast<void**>(&owner))) && owner == device)
        {
            desc = output->GetDesc();
            valid = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D
                && desc.Width && desc.Height && desc.DepthOrArraySize == 1
                && desc.MipLevels == 1 && desc.SampleDesc.Count == 1
                && desc.Format != DXGI_FORMAT_UNKNOWN;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { valid = false; }
    if (owner) owner->Release();
    return valid;
}

void ReportEvaluationSubmissionFailure(const Route& route, const ampere_policy::Feature& feature,
    ID3D12GraphicsCommandList* command, const EvaluationInput& input, const EvaluationReads& reads,
    const D3D12_RESOURCE_DESC& outputDesc, NVSDK_NGX_Result result, bool providerEntered) noexcept
{
    // The caller poisons this exact feature after a failed native submission;
    // later calls cannot reach this diagnostic again before a new Create.
    // Keep all additional COM queries off the successful evaluation path.
    if (result == NVSDK_NGX_Result_Success && providerEntered) return;
    uint32_t commandType = UINT32_MAX;
    HRESULT deviceResult = E_POINTER, removedReason = E_PENDING;
    ID3D12Device* device = nullptr;
    uintptr_t deviceIdentity = 0;
    bool deviceMatches = false, removedReasonRead = false, queryException = false;
    __try
    {
        if (command)
        {
            commandType = static_cast<uint32_t>(command->GetType());
            deviceResult = command->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device));
            deviceIdentity = reinterpret_cast<uintptr_t>(device);
            deviceMatches = SUCCEEDED(deviceResult) && device && deviceIdentity == feature.device;
            if (deviceMatches)
            {
                removedReason = device->GetDeviceRemovedReason();
                removedReasonRead = true;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { queryException = true; }
    __try { if (device) device->Release(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { queryException = true; }

    wchar_t message[1024]{};
    swprintf_s(message, L"Ampere first evaluation submission failure: result=0x%08X providerEntered=%u "
        L"count=%d index=%d frame=%llu output=%p boundary=%u",
        static_cast<uint32_t>(result), static_cast<unsigned>(providerEntered), input.count, input.index,
        input.frame, input.output, FailureCode());
    Report(message);
    swprintf_s(message, L"Ampere evaluation failure device: command=%p type=%u getDevice=0x%08X "
        L"device=%p expectedDevice=%p matches=%u removedReasonRead=%u removedReason=0x%08X exception=%u "
        L"luid=0x%llX handle=%p lifetime=%llu runtime=%p/%llu provider=%p/%llu wrapper=%p/%llu publication=0x%llX",
        command, commandType, static_cast<uint32_t>(deviceResult), reinterpret_cast<void*>(deviceIdentity),
        reinterpret_cast<void*>(feature.device), static_cast<unsigned>(deviceMatches),
        static_cast<unsigned>(removedReasonRead), static_cast<uint32_t>(removedReason), static_cast<unsigned>(queryException),
        feature.luid, reinterpret_cast<void*>(feature.handle), feature.lifetime, route.module, route.generation,
        reinterpret_cast<void*>(feature.provider), feature.providerGeneration,
        reinterpret_cast<void*>(feature.wrapper), feature.wrapperGeneration, feature.publication);
    Report(message);
    // Reuse the descriptor and parameter-read results already validated before
    // native Evaluate. Optional input pointers are not dereferenced, resource
    // state is not inferred, and no GPU readback or synchronization is started.
    swprintf_s(message, L"Ampere evaluation failure output: resource=%p dimension=%u size=%llux%u "
        L"array=%u mips=%u format=%u sampleCount=%u sampleQuality=%u layout=%u flags=0x%X "
        L"get(count/index/frame/output)=%08X/%08X/%08X/%08X",
        input.output, static_cast<unsigned>(outputDesc.Dimension), outputDesc.Width, outputDesc.Height,
        outputDesc.DepthOrArraySize, outputDesc.MipLevels, static_cast<unsigned>(outputDesc.Format),
        outputDesc.SampleDesc.Count, outputDesc.SampleDesc.Quality, static_cast<unsigned>(outputDesc.Layout),
        static_cast<unsigned>(outputDesc.Flags), reads.count, reads.index, reads.frame, reads.output);
    Report(message);
}

bool WritablePresetValue(const uint32_t* value) noexcept
{
    uintptr_t cursor = reinterpret_cast<uintptr_t>(value);
    if (!cursor || cursor > UINTPTR_MAX - sizeof(*value)) return false;
    const uintptr_t end = cursor + sizeof(*value);
    while (cursor < end)
    {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &memory, sizeof(memory)) != sizeof(memory)
            || memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        const DWORD protection = memory.Protect & 0xff;
        if (protection != PAGE_READWRITE && protection != PAGE_WRITECOPY
            && protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY) return false;
        const uintptr_t base = reinterpret_cast<uintptr_t>(memory.BaseAddress);
        if (memory.RegionSize > UINTPTR_MAX - base || base + memory.RegionSize <= cursor) return false;
        cursor = (std::min)(end, base + memory.RegionSize);
    }
    return true;
}

bool AccessPresetValue(uint32_t* value, uint32_t& observation, bool write) noexcept
{
    if (!WritablePresetValue(value)) return false;
    __try
    {
        if (write) *value = observation;
        else observation = *value;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

template<size_t I> __declspec(noinline) bool Preset(uint32_t setting, uint32_t* value)
{
    const void* caller = _ReturnAddress();
    PresetFn original = nullptr;
    HMODULE provider = nullptr;
    bool eligible = false;
    {
        std::lock_guard lock(gCalls);
        const Route& route = gRoutes[I];
        original = RouteOriginal<PresetFn>(route, route.preset);
        provider = route.module;
        eligible = AdapterVerified() && !gFatal.load();
    }
    if (!original) return false;
    MEMORY_BASIC_INFORMATION owner{};
    const bool providerCaller = provider
        && VirtualQuery(caller, &owner, sizeof(owner)) == sizeof(owner)
        && owner.AllocationBase == provider && owner.Type == MEM_IMAGE && owner.State == MEM_COMMIT
        && (owner.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0
        && ((owner.Protect & 0xff) == PAGE_EXECUTE_READ || (owner.Protect & 0xff) == PAGE_EXECUTE);
    if (setting != ampere_policy::kPresetSetting || !eligible || !providerCaller || !WritablePresetValue(value))
        return original(setting, value);

    // Route metadata is no longer locked when the first read loads config or
    // calls the provider. Image ownership avoids acquiring the loader lock.
    uint32_t observed = dlssg_preset::FreezeSelection();
    const bool overridden = observed != 0;
    bool result = false;
    if (overridden)
    {
        if (!AccessPresetValue(value, observed, true)) return original(setting, value);
        result = true;
    }
    else
    {
        result = original(setting, value);
        if (result ? !AccessPresetValue(value, observed, false) : !WritablePresetValue(value)) return result;
    }
    dlssg_preset::RecordProviderRead(provider, result, result ? observed : 0, overridden);
    gPresetQueries.fetch_add(1, std::memory_order_relaxed);
    return result;
}

#if MFG_UNLOCK_RUNTIME_GPU_SELECTION
void ObserveCreateDevice(ID3D12GraphicsCommandList* command) noexcept
{
    if (!command) return;
    ID3D12Device* device = nullptr;
    __try
    {
        if (SUCCEEDED(command->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device))) && device)
            gpu_dispatch::ObserveD3D12Device(device);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (device) device->Release();
}
#endif

template<size_t I> NVSDK_NGX_Result NVSDK_CONV Create(ID3D12GraphicsCommandList* command,
    NVSDK_NGX_Feature feature, const NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** output)
{
    std::unique_lock lock(gCalls);
    const bool frameGeneration = feature == NVSDK_NGX_Feature_FrameGeneration;
    if (frameGeneration) gPipelineCreateObserved.store(true, std::memory_order_release);
    auto original = RouteOriginal<CreateFn>(gRoutes[I], gRoutes[I].create);
#if MFG_UNLOCK_RUNTIME_GPU_SELECTION
    // The command list identifies the actual device even when a game does not
    // expose slSetD3DDevice. No backend is chosen by GPU name or enumeration order.
    if (frameGeneration) ObserveCreateDevice(command);
    if (!gpu_dispatch::IsAmpere())
    {
        if (!original) return NVSDK_NGX_Result_FAIL_NotInitialized;
        if (frameGeneration && gpu_dispatch::Selected() == gpu_dispatch::Family::eConflict) return kRejected;
        if (frameGeneration || gpu_dispatch::IsAda())
        {
            const auto before = frameGeneration && gpu_dispatch::IsAda() ? gRoutes[I].beforeCreate : nullptr;
            const auto entry = gRoutes[I].create;
            lock.unlock();
            if (before) before(command, static_cast<uintptr_t>(feature), parameters, output,
                0, 0, entry, _ReturnAddress());
#if MFG_UNLOCK_OUTPUT_PULL_MASK_ONLY
            if (frameGeneration && midpoint_fix::OutputPullMaskRequiresRestart()) return kRejected;
#endif
            // Native NGX may wait for a provider callback on another thread.
            // Ada/unmodified calls must not inherit Ampere's TLS lifecycle lock.
            fault_capture::NgxCall call(entry, _ReturnAddress(), command,
                static_cast<uintptr_t>(feature), parameters);
            const auto result = original(command, feature, parameters, output);
            call.Complete(static_cast<uint32_t>(result), reinterpret_cast<void* const*>(output));
            return result;
        }
        // During pre-device startup, retain non-FG handles for Ampere's later
        // independent SR/RR pass-through. No parameter keys are used as identity.
    }
#endif
    if (frameGeneration) gCreateAttempts.fetch_add(1, std::memory_order_relaxed);
    if (!original)
    {
        if (frameGeneration)
        {
            gCreateBlockedBeforeProvider.fetch_add(1, std::memory_order_relaxed);
            gFailures.Record(ampere_diagnostics::kCreateSubmissionFailure, NVSDK_NGX_Result_FAIL_NotInitialized);
        }
        return NVSDK_NGX_Result_FAIL_NotInitialized;
    }
    if (feature != NVSDK_NGX_Feature_FrameGeneration)
    {
        // SR/RR can reuse a parameter object containing FG keys. Establish
        // their identity at Create rather than guessing from Evaluate options.
        // The installed entry pins its owner and trampoline. Reserve an exact
        // route/generation/lifetime before dropping gCalls; a nested Create may
        // grow the vector or return a numerically reused handle.
        OtherFeatureLease reservation{};
        {
            auto& route = gRoutes[I];
            if (!route.runtime || !output) { Reject(126); return kRejected; }
            const size_t slot = ReserveOtherFeature(route);
            if (slot == SIZE_MAX) { Reject(126); return kRejected; }
            reservation = SnapshotOtherFeature(I, route, slot);
        }
        lock.unlock();
        const auto result = original(command, feature, parameters, output);
        const uintptr_t created = CreatedHandle(result, output);
        lock.lock();
        if (auto* reserved = CurrentOtherFeature(reservation);
            reserved && reserved->state == Route::OtherFeature::State::eCreating)
        {
            if (result == NVSDK_NGX_Result_Success && created)
            {
                reserved->handle = created;
                reserved->state = Route::OtherFeature::State::eActive;
            }
            else reserved->Clear();
        }
        return result;
    }
    Route& route = gRoutes[I];
    struct Attempt
    {
        bool forwarded = false;
        ~Attempt() { if (!forwarded) gCreateBlockedBeforeProvider.fetch_add(1, std::memory_order_relaxed); }
    } attempt;
    if (!output || !command || gCreateDepth || gEvaluateDepth || gReleaseDepth
        || gCurrentCreateAttempt != SIZE_MAX
        || !gStartupComplete.load() || gFatal.load()) { Reject(120); return kRejected; }
    gCertifiedMaximum.store(0, std::memory_order_release);
    *output = nullptr;
    ID3D12Device* device = nullptr;
    if (FAILED(command->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device))) || !device)
    { Reject(121); return kRejected; }
    const uintptr_t deviceIdentity = reinterpret_cast<uintptr_t>(device);
    const LUID luid = device->GetAdapterLuid();
    device->Release();
    uint64_t packed = 0; memcpy(&packed, &luid, sizeof(packed));
    if (!AdapterVerified() || packed != ampere_gpu::AdapterLuid()) { Reject(121); return kRejected; }
    CreateAttemptScope createAttempt(route);
    if (!createAttempt) { Reject(120); return kRejected; }
    if (route.beforeCreate) route.beforeCreate(command, static_cast<uintptr_t>(feature), parameters, output,
        0, 0, route.create, _ReturnAddress());
    // MultiFrameCount is an evaluation input in the target wrapper, not its
    // creation/allocation contract. Observe it without rewriting or gating on it.
    ObserveCreationCount(parameters);
    if (!Ready() || route.startupMaximum != ampere_policy::kMaximumGeneratedFrames
        || !parameters) { Reject(122); return kRejected; }
    auto* wrapper = ActiveWrapper();
    ampere_wrapper::Capacity capacity{};
    const bool callerProven = wrapper && ampere_wrapper::CreationOnStack(wrapper->wrapperLayout);
    const bool buffersProven = wrapper && ampere_wrapper::Read(wrapper->wrapperLayout, capacity);
    if (!callerProven || !buffersProven)
    {
        wchar_t message[160]{};
        swprintf_s(message, L"Ampere MFG allocation rejected: wrapper=%p generation=%llu caller=%u buffers=%u",
            wrapper ? wrapper->module : nullptr, wrapper ? wrapper->generation : 0,
            static_cast<unsigned>(callerProven), static_cast<unsigned>(buffersProven));
        Report(message);
        gPresentationBuffers.store(0); Reject(171); return kRejected;
    }
    createAttempt.BindCapacity(*wrapper, capacity);
    gPresentationBuffers.store(static_cast<uint32_t>(capacity.buffers.size()));
    auto free = std::find_if(route.features.begin(), route.features.end(), [](const auto& f) { return !f.handle; });
    if (free == route.features.end()) { Reject(123); return kRejected; }
    if (route.runtime && (!route.validation.address || !route.validation.Set(true)))
    { Reject(152); return kRejected; }
    bool restored = false;
    Route* provider = nullptr;
    attempt.forwarded = true;
    uint64_t createAttemptToken = 0;
    const NVSDK_NGX_Result result = InvokeCreate(route, original, command, feature, parameters, output,
        restored, provider, *wrapper, capacity, createAttemptToken);
    FeatureLifetimeEvent createdEvent{};
    bool notifyCreated = false;
    if (result == NVSDK_NGX_Result_Success && *output)
    {
        uint64_t lifetime = ++route.nextFeatureLifetime;
        if (!lifetime) lifetime = ++route.nextFeatureLifetime;
        *free = {reinterpret_cast<uintptr_t>(*output), reinterpret_cast<uintptr_t>(route.module),
            route.generation, ampere_gpu::Publication(), packed, false};
        free->lifetime = lifetime;
        free->provider = provider ? reinterpret_cast<uintptr_t>(provider->module) : 0;
        free->providerGeneration = provider ? provider->generation : 0;
        free->device = deviceIdentity;
        free->createdGeneratedFrames = ampere_policy::kMaximumGeneratedFrames;
        free->wrapper = reinterpret_cast<uintptr_t>(wrapper->module);
        free->wrapperGeneration = wrapper->generation;
        free->capacity = capacity;
        free->createAttemptToken = createAttemptToken;
        if (!restored || !provider || !createAttemptToken
            || (!provider->legacyPresetReady && gPresetQueries.load() == 0)
            || !FeatureCapacityCurrent(*free))
        {
            createAttempt.Finish();
            gFatal.store(true);
            Reject(125);
            if (auto release = Original<ReleaseFn>(route.release))
                free->Release(InvokeRelease(route, release, *output, *free) == NVSDK_NGX_Result_Success);
            *output = nullptr;
            return kRejected;
        }
        gCreatedFeatures.fetch_add(1);
        // The owned legacy Create contract accepts only its sole value 1.
        // A successful real Create establishes the read; discovery alone does
        // not increment query counters or claim the modern B preset was used.
        if (provider->legacyPresetReady)
            dlssg_preset::RecordProviderRead(provider->module, true, 1, false);
        gFailures.Recover();
        gFailure.store(0, std::memory_order_release);
        gCertifiedMaximum.store(ampere_policy::kMaximumGeneratedFrames, std::memory_order_release);
        Report(L"Ampere MFG Create accepted: active wrapper owns six presentation buffers; exact feature tracked");
        createdEvent = LifetimeEvent(FeatureLifetimePhase::eCreated, *free);
        notifyCreated = QueueFeatureLifetime(createdEvent, createAttempt.Read());
    }
    else if (result == NVSDK_NGX_Result_Success) { Reject(124); return kRejected; }
    else gFailures.Record(ampere_diagnostics::kCreateSubmissionFailure, static_cast<uint32_t>(result));
    // Queued delivery owns its immutable proof before another native Create
    // can begin. Reentrant delivery must get a fresh TLS operation, not ours.
    createAttempt.Finish();
    if (notifyCreated)
    {
        lock.unlock();
        DrainFeatureLifetimeEvents();
    }
    return result;
}

template<size_t I> NVSDK_NGX_Result NVSDK_CONV Evaluate(ID3D12GraphicsCommandList* command,
    const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback)
{
    std::unique_lock lock(gCalls);
    auto original = RouteOriginal<EvaluateFn>(gRoutes[I], gRoutes[I].evaluate);
    if (!original) return NVSDK_NGX_Result_FAIL_NotInitialized;
#if MFG_UNLOCK_RUNTIME_GPU_SELECTION
    if (!gpu_dispatch::IsAmpere())
    {
        if (gpu_dispatch::Selected() == gpu_dispatch::Family::eConflict) return kRejected;
        const auto before = gpu_dispatch::IsAda() ? gRoutes[I].beforeEvaluate : nullptr;
        const auto entry = gRoutes[I].evaluate;
        lock.unlock();
        if (before) before(command, reinterpret_cast<uintptr_t>(handle), parameters,
            reinterpret_cast<void*>(callback), 0, 0, entry, _ReturnAddress());
        fault_capture::NgxCall call(entry, _ReturnAddress(), command,
            reinterpret_cast<uintptr_t>(handle), parameters);
        const auto result = original(command, handle, parameters, callback);
        call.Complete(static_cast<uint32_t>(result));
        return result;
    }
#endif
    const uintptr_t handleValue = reinterpret_cast<uintptr_t>(handle);
    size_t frameGenerationSlot = SIZE_MAX;
    {
        const auto& route = gRoutes[I];
        for (size_t slot = 0; slot != route.features.size(); ++slot)
        {
            if (route.features[slot].handle && route.features[slot].handle == handleValue)
            { frameGenerationSlot = slot; break; }
        }
    }
    if (frameGenerationSlot == SIZE_MAX)
    {
        OtherFeatureLease lease{};
        {
            auto& route = gRoutes[I];
            const size_t slot = FindActiveOtherFeature(route, handleValue);
            if (slot != SIZE_MAX && route.otherFeatures[slot].activeCalls != UINT32_MAX)
            {
                ++route.otherFeatures[slot].activeCalls;
                lease = SnapshotOtherFeature(I, route, slot, handleValue);
            }
        }
        if (lease.slot != SIZE_MAX)
        {
            lock.unlock();
            const auto result = original(command, handle, parameters, callback);
            lock.lock();
            if (auto* current = CurrentOtherFeature(lease);
                current && current->Active(lease.handle) && current->activeCalls)
                --current->activeCalls;
            return result;
        }
        Reject(130); return kRejected;
    }
    Route& route = gRoutes[I];
    auto* found = &route.features[frameGenerationSlot];
    gEvaluateAttempts.fetch_add(1, std::memory_order_relaxed);
    EvaluationInput input{};
    EvaluationReads reads{};
    const uint64_t publication = ampere_gpu::Publication(), adapter = ampere_gpu::AdapterLuid();
    const bool inputsValid = ReadEvaluationInput(parameters, input, &reads);
    const uint32_t rejected = ((gCreateDepth || gEvaluateDepth || gReleaseDepth) ? 0x01u : 0)
        | (!Ready() ? 0x02u : 0)
        | (!inputsValid ? 0x04u : 0)
        | (!FeatureCapacityCurrent(*found) ? 0x08u : 0)
        | (found->createdGeneratedFrames != ampere_policy::kMaximumGeneratedFrames ? 0x10u : 0)
        | (!found->batch.Accepts(input.frame, input.count, input.index, reinterpret_cast<uintptr_t>(input.output)) ? 0x20u : 0)
        | (!found->Matches(reinterpret_cast<uintptr_t>(handle), reinterpret_cast<uintptr_t>(route.module),
            route.generation, publication, adapter) ? 0x40u : 0);
    if (rejected)
    {
        gCertifiedMaximum.store(0, std::memory_order_release);
        if (!found->batch.failed) ReportEvaluationRejection(route, *found, input, reads, rejected, publication, adapter);
        found->batch.failed = true; Reject(131); return kRejected;
    }
    ID3D12Device* device = nullptr;
    if (!command || FAILED(command->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device))) || !device)
    { found->batch.failed = true; Reject(132); return kRejected; }
    const LUID luid = device->GetAdapterLuid();
    D3D12_RESOURCE_DESC desc{};
    const bool outputValid = reinterpret_cast<uintptr_t>(device) == found->device
        && OutputMatchesDevice(input.output, device, desc)
        && (!found->outputWidth || (found->outputWidth == desc.Width
            && found->outputHeight == desc.Height && found->outputFormat == desc.Format));
    device->Release();
    uint64_t packed = 0; memcpy(&packed, &luid, sizeof(packed));
    if (packed != found->luid || !outputValid)
    { found->batch.failed = true; Reject(132); return kRejected; }
    found->outputWidth = desc.Width; found->outputHeight = desc.Height; found->outputFormat = desc.Format;
    if (route.beforeEvaluate) route.beforeEvaluate(command, reinterpret_cast<uintptr_t>(handle), parameters,
        reinterpret_cast<void*>(callback), 0, 0, route.evaluate, _ReturnAddress());
    bool providerExecuted = false;
    gEvaluationInput = input;
    const auto result = InvokeEvaluate(route, original, command, handle, parameters, callback, *found, providerExecuted);
    if (result != NVSDK_NGX_Result_Success || !providerExecuted)
    {
        if (result != NVSDK_NGX_Result_Success)
            gFailures.Record(ampere_diagnostics::kEvaluationSubmissionFailure, static_cast<uint32_t>(result));
        ReportEvaluationSubmissionFailure(route, *found, command, input, reads, desc, result, providerExecuted);
    }
    if (!FeatureCapacityCurrent(*found))
    { found->batch.failed = true; Reject(171); return kRejected; }
    found->batch.Complete(input.frame, input.count, input.index, reinterpret_cast<uintptr_t>(input.output),
        result == NVSDK_NGX_Result_Success && providerExecuted);
    if (result == NVSDK_NGX_Result_Success)
    {
        if (!providerExecuted) { Reject(134); return kRejected; }
        if (found->batch.failed) { Reject(137); return kRejected; }
        found->evaluated = true; gEvaluations.fetch_add(1);
        gCertifiedMaximum.store(ampere_policy::kMaximumGeneratedFrames, std::memory_order_release);
        if (input.index == input.count) gSubmittedBatches.fetch_add(1);
    }
    // Preserve genuine failures. There is no output copy, forged success, or
    // reuse of a possibly stale/black OutputReal surface in this build.
    return result;
}

template<size_t I> NVSDK_NGX_Result NVSDK_CONV Release(NVSDK_NGX_Handle* handle)
{
    std::unique_lock lock(gCalls);
    auto original = RouteOriginal<ReleaseFn>(gRoutes[I], gRoutes[I].release);
    if (!original) return NVSDK_NGX_Result_FAIL_NotInitialized;
    const uintptr_t handleValue = reinterpret_cast<uintptr_t>(handle);
#if MFG_UNLOCK_RUNTIME_GPU_SELECTION
    if (!gpu_dispatch::IsAmpere())
    {
        OtherFeatureLease lease{};
        const auto entry = gRoutes[I].release;
        {
            auto& route = gRoutes[I];
            const size_t slot = FindActiveOtherFeature(route, handleValue);
            if (slot != SIZE_MAX && route.otherFeatures[slot].activeCalls == 0)
            {
                route.otherFeatures[slot].state = Route::OtherFeature::State::eReleasing;
                lease = SnapshotOtherFeature(I, route, slot, handleValue);
            }
        }
        lock.unlock();
        fault_capture::NgxCall call(entry, _ReturnAddress(), nullptr, handleValue, nullptr);
        const auto result = original(handle);
        call.Complete(static_cast<uint32_t>(result));
        if (lease.slot != SIZE_MAX)
        {
            lock.lock();
            if (auto* current = CurrentOtherFeature(lease);
                current && current->state == Route::OtherFeature::State::eReleasing
                    && current->handle == lease.handle)
            {
                if (result == NVSDK_NGX_Result_Success) current->Clear();
                else current->state = Route::OtherFeature::State::eActive;
            }
        }
        return result;
    }
#endif
    Route& route = gRoutes[I];
    for (auto& f : route.features)
    {
        if (f.handle && f.handle == handleValue)
        {
            if (gCreateDepth || gEvaluateDepth || gReleaseDepth) { Reject(136); return kRejected; }
            if (!f.Matches(handleValue, reinterpret_cast<uintptr_t>(route.module),
                    route.generation, ampere_gpu::Publication(), ampere_gpu::AdapterLuid()))
            { Reject(135); return kRejected; }
            const auto result = InvokeRelease(route, original, handle, f);
            const FeatureLifetimeEvent event = LifetimeEvent(FeatureLifetimePhase::eReleased, f);
            if (f.Release(result == NVSDK_NGX_Result_Success))
            {
                gCertifiedMaximum.store(0, std::memory_order_release);
                gPresentationBuffers.store(0, std::memory_order_release);
                const bool notifyReleased = QueueFeatureLifetime(event);
                lock.unlock();
                if (notifyReleased)
                    DrainFeatureLifetimeEvents();
            }
            return result;
        }
    }
    OtherFeatureLease lease{};
    {
        const size_t slot = FindActiveOtherFeature(route, handleValue);
        if (slot == SIZE_MAX || route.otherFeatures[slot].activeCalls)
        { Reject(136); return kRejected; }
        route.otherFeatures[slot].state = Route::OtherFeature::State::eReleasing;
        lease = SnapshotOtherFeature(I, route, slot, handleValue);
    }
    // No reference or iterator into the growable table crosses this native
    // boundary. A concurrent Create can safely grow it or reuse the number.
    lock.unlock();
    const auto result = original(handle);
    lock.lock();
    if (auto* current = CurrentOtherFeature(lease);
        current && current->state == Route::OtherFeature::State::eReleasing
            && current->handle == lease.handle)
    {
        if (result == NVSDK_NGX_Result_Success) current->Clear();
        else current->state = Route::OtherFeature::State::eActive;
    }
    return result;
}

NVSDK_NGX_Result NVSDK_CONV RejectProviderCall(void*, uintptr_t, const void*, void*) noexcept { return kRejected; }

bool WINAPI ProviderGate(void* arg1, uintptr_t arg2, const void* arg3, void* arg4,
    uintptr_t arg5, uintptr_t arg6, entry_detour::Handle entry, const void* caller) noexcept
{
    const auto attemptedEntry = entry_detour::ReadSnapshot(entry);
    if (attemptedEntry.current && attemptedEntry.kind == entry_detour::Kind::eNgxD3D12CreateFeature
        && arg2 == NVSDK_NGX_Feature_FrameGeneration)
        gPipelineCreateObserved.store(true, std::memory_order_release);
#if MFG_UNLOCK_RUNTIME_GPU_SELECTION
    std::unique_lock callLock(gCalls);
    const auto selectedEntry = entry_detour::ReadSnapshot(entry);
    if (selectedEntry.kind == entry_detour::Kind::eNgxD3D12CreateFeature
        && arg2 == NVSDK_NGX_Feature_FrameGeneration)
        ObserveCreateDevice(static_cast<ID3D12GraphicsCommandList*>(arg1));
    if (!gpu_dispatch::IsAmpere())
    {
        const auto selectedRoute = std::find_if(gRoutes.begin(), gRoutes.end(), [&](const Route& candidate) {
            return !candidate.runtime && candidate.module == selectedEntry.owner
                && candidate.generation == selectedEntry.generation; });
        if (!selectedEntry.current || selectedRoute == gRoutes.end()) return false;
        if (gpu_dispatch::Selected() == gpu_dispatch::Family::eConflict)
            return selectedEntry.kind == entry_detour::Kind::eAmpereRelease;
        entry_detour::ForwardPreCall before = nullptr;
        if (gpu_dispatch::IsAda())
        {
            if (selectedEntry.kind == entry_detour::Kind::eNgxD3D12CreateFeature
                && arg2 == NVSDK_NGX_Feature_FrameGeneration && selectedRoute->beforeCreate)
                before = selectedRoute->beforeCreate;
            else if (selectedEntry.kind == entry_detour::Kind::eNgxD3D12EvaluateFeature && selectedRoute->beforeEvaluate)
                before = selectedRoute->beforeEvaluate;
        }
        callLock.unlock();
        if (before) before(arg1, arg2, arg3, arg4, arg5, arg6, entry, caller);
#if MFG_UNLOCK_OUTPUT_PULL_MASK_ONLY
        if (((selectedEntry.kind == entry_detour::Kind::eNgxD3D12CreateFeature
                && arg2 == NVSDK_NGX_Feature_FrameGeneration)
            || selectedEntry.kind == entry_detour::Kind::eNgxD3D12EvaluateFeature)
            && midpoint_fix::OutputPullMaskRequiresRestart()) return false;
#endif
        fault_capture::ProviderForward(entry, caller, arg1, arg2, arg3);
        return true;
    }
#endif
    // Tail forwarding keeps NVIDIA's real runtime caller on the stack.
    // Only a nested call belonging to our tracked runtime operation is admitted.
    HMODULE callerModule = nullptr;
    if (!gCallingRuntime || !caller || !GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(caller), &callerModule)
        || callerModule != gCallingRuntime->module) return Reject(160);
    const auto snapshot = entry_detour::ReadSnapshot(entry);
    auto found = std::find_if(gRoutes.begin(), gRoutes.end(), [&](const Route& route) {
        return !route.runtime && route.module == snapshot.owner && route.generation == snapshot.generation; });
    if (!snapshot.current || found == gRoutes.end() || !found->prepared) return Reject(161);
    auto& route = *found;
    if (snapshot.kind == entry_detour::Kind::eNgxD3D12CreateFeature)
    {
        ngx_runtime_dispatch::Selection selected{};
        const auto dispatch = ngx_runtime_dispatch::Read(entry_detour::ReadSnapshot(gCallingRuntime->create),
            NVSDK_NGX_Feature_FrameGeneration, selected);
        if (dispatch == ngx_runtime_dispatch::ReadResult::eInvalid
            || (gCreatingDispatch && (!ngx_runtime_dispatch::StillCurrent(gCreatingDispatch)
                || gCreatingDispatch.provider != snapshot.owner
                || gCreatingDispatch.target != reinterpret_cast<uintptr_t>(snapshot.target)))
            || (dispatch == ngx_runtime_dispatch::ReadResult::eSelected
                && (selected.provider != snapshot.owner || selected.target != reinterpret_cast<uintptr_t>(snapshot.target)
                    || !ngx_runtime_dispatch::StillCurrent(selected))))
            return Reject(ampere_diagnostics::kRuntimeDispatchFailure);
        if (!gCreateDepth || arg2 != NVSDK_NGX_Feature_FrameGeneration || !Ready() || !arg3
            || !gCallingWrapper || !ampere_wrapper::Matches(gCallingWrapper->wrapperLayout, gCreatingCapacity)
            || (gCreatingProvider && gCreatingProvider != &route)) return Reject(162);
        if (!ampere_gpu::BindProvider(route.module)) return Reject(161);
        if (!BindCurrentCreateProvider(route)) return Reject(162);
        gCreatingProvider = &route;
        if (route.beforeCreate) route.beforeCreate(arg1, arg2, arg3, arg4, arg5, arg6, entry, caller);
        return true;
    }
    auto* feature = gCallingFeature;
    if (route.module != ampere_gpu::Provider() || !feature
        || feature->provider != reinterpret_cast<uintptr_t>(route.module)
        || feature->providerGeneration != route.generation) return Reject(163);
    if (snapshot.kind == entry_detour::Kind::eNgxD3D12EvaluateFeature)
    {
        EvaluationInput input{};
        if (!gEvaluateDepth || !arg2 || !Ready() || gProviderEvaluated || !FeatureCapacityCurrent(*feature)
            || !ReadEvaluationInput(static_cast<const NVSDK_NGX_Parameter*>(arg3), input)
            || !(input == gEvaluationInput)
            || (feature->providerHandle && feature->providerHandle != arg2)) return Reject(164);
        // The inner alias is learned only inside an exact, successfully
        // created outer handle's synchronous runtime call, never from options.
        feature->providerHandle = arg2;
        gProviderEvaluated = true;
        if (route.beforeEvaluate) route.beforeEvaluate(arg1, arg2, arg3, arg4, arg5, arg6, entry, caller);
        return true;
    }
    if (snapshot.kind == entry_detour::Kind::eAmpereRelease)
        return gReleaseDepth && arg1 && (!feature->providerHandle
            || feature->providerHandle == reinterpret_cast<uintptr_t>(arg1)) ? true : Reject(165);
    return Reject(166);
}

template<size_t I> NVSDK_NGX_Result NVSDK_CONV Requirements(IDXGIAdapter* adapter,
    const NVSDK_NGX_FeatureDiscoveryInfo* discovery, NVSDK_NGX_FeatureRequirement* requirements)
{
    std::unique_lock lock(gCalls);
    auto original = RouteOriginal<RequirementsFn>(gRoutes[I], gRoutes[I].requirements);
    if (!original) return NVSDK_NGX_Result_FAIL_NotInitialized;
    // Streamline asks this during slInit, before a D3D12 device exists.
    // Verify only the adapter supplied to this FG query. Actual device LUID
    // confirmation remains mandatory before program publication and Create.
    const bool scoped =
#if MFG_UNLOCK_RUNTIME_GPU_SELECTION
        !gpu_dispatch::IsAda() && gpu_dispatch::Selected() != gpu_dispatch::Family::eConflict &&
#endif
        (gInitDepth || gStartupDepth) && requirements && discovery
        && discovery->FeatureID == NVSDK_NGX_Feature_FrameGeneration
        && ampere_gpu::ObserveAdapter(adapter) && !gFatal.load();
    if (!scoped)
    {
        // Non-FG and ordinary capability discovery is pure pass-through. It
        // must not serialize native code with feature callbacks on gCalls.
        lock.unlock();
        return original(adapter, discovery, requirements);
    }
    auto& route = gRoutes[I];
    if (scoped && route.metadata.address) route.metadata.Set(true);
    const auto result = original(adapter, discovery, requirements);
    if (scoped && route.metadata.active) route.metadata.Set(false);
    if (!scoped || gFatal.load()) return result;
    if (result == NVSDK_NGX_Result_Success
        && (requirements->FeatureSupported == NVSDK_NGX_FeatureSupportResult_AdapterUnsupported
            || requirements->FeatureSupported == NVSDK_NGX_FeatureSupportResult_Supported))
    {
        requirements->FeatureSupported = NVSDK_NGX_FeatureSupportResult_Supported;
        requirements->MinHWArchitecture = 0;
        return result;
    }
    if (ampere_gpu::Ready()
        && (result == NVSDK_NGX_Result_FAIL_FeatureNotSupported || result == NVSDK_NGX_Result_FAIL_FeatureNotFound))
    {
        // The actual eligible provider and its full SM86 program are present.
        // Restrict the requirements override to this verified FG startup.
        *requirements = {};
        requirements->FeatureSupported = NVSDK_NGX_FeatureSupportResult_Supported;
        return NVSDK_NGX_Result_Success;
    }
    // Do not invent a feature when the provider is absent or failed to load.
    return result;
}

bool PublishStartupCapabilities(Route& route, NVSDK_NGX_Parameter* parameters) noexcept
{
    if (!parameters || gStartupComplete.load() || gCreatedFeatures.load()) return Reject(170);
    // This is an early capability bootstrap, not permission to create FG.
    // A recognized wrapper must still be before swap-chain allocation; the
    // actual caller, generation and allocated buffers are checked at Create.
    bool earlyWrapper = false;
    for (const auto& candidate : gRoutes)
        earlyWrapper |= candidate.wrapperLayout.module && ampere_wrapper::BeforeSwapchain(candidate.wrapperLayout);
    if (!earlyWrapper) return Reject(170);
    __try
    {
        unsigned int maximum = 0, available = 0;
        const auto nativeResult = parameters->Get("DLSSG.MultiFrameCountMax", &maximum);
        if (!route.startupMaximum)
        {
            route.nativeMaximum = nativeResult == NVSDK_NGX_Result_Success ? maximum : 0;
            gNativeMaximum.store(route.nativeMaximum);
            gNativeMaximumResult.store(nativeResult);
        }
        if (nativeResult == NVSDK_NGX_Result_Success && maximum > 5) return Reject(154);
        // Ampere's one-frame (or absent, when unavailable) capability is the
        // reason for this bootstrap. It is not evidence of buffer allocation.
        parameters->Set("FrameGeneration.Available", 1u);
        parameters->Set("DLSSG.MultiFrameCountMax", ampere_policy::kMaximumGeneratedFrames);
        if (parameters->Get("FrameGeneration.Available", &available) != NVSDK_NGX_Result_Success
            || parameters->Get("DLSSG.MultiFrameCountMax", &maximum) != NVSDK_NGX_Result_Success
            || available != 1 || maximum != ampere_policy::kMaximumGeneratedFrames) return Reject(154);
        route.startupMaximum = maximum;
        gStartupMaximum.store(maximum);
        wchar_t message[180]{};
        swprintf_s(message, L"Ampere MFG capability bootstrap: nativeGet=0x%08X nativeMax=%u startupMax=%u; allocation still required at Create",
            static_cast<unsigned>(gNativeMaximumResult.load()), route.nativeMaximum, maximum);
        Report(message);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return Reject(154); }
}

template<size_t I> NVSDK_NGX_Result NVSDK_CONV Parameters(NVSDK_NGX_Parameter** output)
{
    std::unique_lock lock(gCalls);
    auto original = RouteOriginal<ParametersFn>(gRoutes[I], gRoutes[I].parameters);
    if (!original) return NVSDK_NGX_Result_FAIL_NotInitialized;
    const bool publishStartup = output && gStartupDepth
        && ampere_gpu::Ready() && AdapterVerified() && !gFatal.load()
#if MFG_UNLOCK_RUNTIME_GPU_SELECTION
        && gpu_dispatch::IsAmpere()
#endif
        ;
    if (!publishStartup)
    {
        // Capability pass-through is itself external native work. Snapshot the
        // pinned route/generation above, then release gCalls so a provider can
        // synchronously wait for another intercepted operation.
        lock.unlock();
        return original(output);
    }
    const auto result = original(output);
    if (result == NVSDK_NGX_Result_Success && *output)
    {
        if (!PublishStartupCapabilities(gRoutes[I], *output))
        {
            const uint32_t failure = gFailure.load();
            gFatal.store(true);
            Reject(failure); // Preserve the first fatal bootstrap reason.
        }
    }
    return result;
}

#define AMPERE_THUNKS(F) F<0>, F<1>, F<2>, F<3>, F<4>, F<5>, F<6>, F<7>
constexpr std::array<CreateFn, kRoutes> kCreates{AMPERE_THUNKS(Create)};
constexpr std::array<EvaluateFn, kRoutes> kEvaluates{AMPERE_THUNKS(Evaluate)};
constexpr std::array<ReleaseFn, kRoutes> kReleases{AMPERE_THUNKS(Release)};
constexpr std::array<RequirementsFn, kRoutes> kRequirements{AMPERE_THUNKS(Requirements)};
constexpr std::array<ParametersFn, kRoutes> kParameters{AMPERE_THUNKS(Parameters)};
constexpr std::array<PresetFn, kRoutes> kPresets{AMPERE_THUNKS(Preset)};
#undef AMPERE_THUNKS

Route* FindRoute(HMODULE module, uint64_t generation) noexcept
{
    for (auto& route : gRoutes) if (route.module == module && route.generation == generation) return &route;
    for (auto& route : gRoutes) if (!route.module) { route.module = module; route.generation = generation; return &route; }
    return nullptr;
}

bool InstallExport(Route& route, const char* name, entry_detour::Kind kind, void* hook, entry_detour::Handle& handle) noexcept
{
    if (handle) return entry_detour::ReadSnapshot(handle).current;
    void* target = reinterpret_cast<void*>(GetProcAddress(route.module, name));
    if (!target) return false;
    void* trampoline = nullptr;
    entry_detour::InstallOptions options{}; options.generation = route.generation; options.allowRelocated = true;
    if (!route.runtime && (kind == entry_detour::Kind::eNgxD3D12CreateFeature
            || kind == entry_detour::Kind::eNgxD3D12EvaluateFeature || kind == entry_detour::Kind::eAmpereRelease))
        return entry_detour::InstallForwarding(kind, route.module, target, nullptr, trampoline,
            options, &handle, &ProviderGate, reinterpret_cast<void*>(&RejectProviderCall));
    return entry_detour::Install(kind, route.module, target, hook, trampoline, options, &handle);
}

bool ReadOnlyImageSpan(const Image& image, uintptr_t address, size_t bytes) noexcept
{
    MEMORY_BASIC_INFORMATION memory{};
    if (!image.Contains(address, bytes)
        || VirtualQuery(reinterpret_cast<const void*>(address), &memory, sizeof(memory)) != sizeof(memory)
        || memory.AllocationBase != image.module || memory.Type != MEM_IMAGE || memory.State != MEM_COMMIT
        || memory.Protect != PAGE_READONLY) return false;
    const uintptr_t region = reinterpret_cast<uintptr_t>(memory.BaseAddress);
    return address >= region && address - region < memory.RegionSize
        && bytes <= memory.RegionSize - (address - region);
}

uintptr_t HostRelativeTarget(uintptr_t start, size_t offset) noexcept
{
    int32_t displacement = 0;
    memcpy(&displacement, reinterpret_cast<const void*>(start + offset), 4);
    return start + offset + 4 + displacement;
}

bool OwnedCfgDispatch(const Image& image, uintptr_t slot) noexcept
{
    const auto& directory = image.nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
    constexpr size_t required = offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardCFDispatchFunctionPointer) + sizeof(uint64_t);
    if (!directory.VirtualAddress || directory.Size < required
        || !ReadOnlyImageSpan(image, image.base + directory.VirtualAddress, required)) return false;
    const auto* config = reinterpret_cast<const IMAGE_LOAD_CONFIG_DIRECTORY64*>(image.base + directory.VirtualAddress);
    if (config->Size < required || config->GuardCFDispatchFunctionPointer != slot
        || !ReadOnlyImageSpan(image, slot, sizeof(uintptr_t))) return false;
    uintptr_t callable = 0;
    memcpy(&callable, reinterpret_cast<const void*>(slot), sizeof(callable));
    MEMORY_BASIC_INFORMATION memory{};
    if (!callable || VirtualQuery(reinterpret_cast<const void*>(callable), &memory, sizeof(memory)) != sizeof(memory)
        || memory.Type != MEM_IMAGE || memory.State != MEM_COMMIT
        || (memory.Protect != PAGE_EXECUTE_READ && memory.Protect != PAGE_EXECUTE)) return false;
    // Windows may replace the provider's default CFG dispatcher with its own
    // system-image implementation. Bind the declared immutable slot and retain
    // the actual executable owner without substituting a callable.
    return Pin(static_cast<HMODULE>(memory.AllocationBase));
}

template<size_t N> bool ValidateHostOperands(const Image& image, uintptr_t start,
    const std::array<ampere_host_contracts::Operand, N>& operands) noexcept
{
    using Kind = ampere_host_contracts::OperandKind;
    uintptr_t logger = 0;
    for (const auto& operand : operands)
    {
        const uintptr_t target = HostRelativeTarget(start, operand.offset);
        if (operand.kind == Kind::eLogger)
        {
            uint32_t begin = 0, end = 0;
            if ((logger && target != logger) || !FunctionBounds(image, target, begin, end)) return false;
            logger = target;
            continue;
        }
        if (operand.kind == Kind::eCfgSlot && !OwnedCfgDispatch(image, target)) return false;
        const size_t bytes = operand.anchor ? strlen(operand.anchor) + 1
            : operand.kind == Kind::eCfgSlot ? sizeof(uintptr_t) : 4;
        if (!ReadOnlyImageSpan(image, target, bytes)
            || (operand.anchor && memcmp(reinterpret_cast<const void*>(target), operand.anchor, bytes))) return false;
    }
    return logger != 0;
}

template<size_t N, size_t R> uintptr_t FindHostContract(const Image& image,
    const std::array<uint8_t, N>& body, size_t bytes, size_t ownerOffset, size_t ownerBytes,
    const std::array<ampere_host_contracts::Operand, R>& operands, unsigned& matches) noexcept
{
    std::array<uint8_t, N> mask{};
    mask.fill(255);
    for (const auto& operand : operands)
    {
        if (operand.offset + 4 > bytes) return 0;
        memset(mask.data() + operand.offset, 0, 4);
    }
    unsigned found = 0;
    const uintptr_t start = FindPattern(image, body.data(), mask.data(), bytes, SIZE_MAX, 0, 0, &found);
    matches += found;
    uint32_t begin = 0, end = 0;
    if (!start || start < image.base + ownerOffset
        || !FunctionBounds(image, start - ownerOffset, begin, end) || end - begin != ownerBytes
        || ownerOffset + bytes > ownerBytes || !ValidateHostOperands(image, start, operands)) return 0;
    return start;
}

bool LegacyPresetContract(const Image& image, unsigned& matches) noexcept
{
    uintptr_t selected = 0;
    for (const auto& profile : ampere_host_contracts::kPresets)
        if (const auto found = FindHostContract(image, profile.body, profile.bytes,
                profile.ownerOffset, profile.ownerBytes, profile.operands, matches)) selected = found;
    return selected != 0 && matches == 1;
}

void RetainLegacyPresetContract(size_t index, const Route& route) noexcept
{
    if (index >= gLegacyPresetRoutes.size()) return;
    auto& proof = gLegacyPresetRoutes[index];
    const bool known = route.prepared && route.legacyPresetReady && !route.runtime
        && route.module && route.generation;
    proof.generation.store(0);
    proof.provider.store(known ? route.module : nullptr);
    if (known) proof.generation.store(route.generation);
}

bool RoutePresetCurrent(const Route& route) noexcept
{
    if (!route.prepared || route.runtime || !route.module || !route.generation) return false;
    if (route.preset) return RouteEntryCurrent(route, route.preset);
    if (!route.legacyPresetReady || !RouteEntryCurrent(route, route.create)) return false;
    // Admission belongs to this retained route, independently of which prepared
    // provider is currently selected for status or later feature creation.
    for (size_t i = 0; i < gRoutes.size(); ++i)
    {
        if (&gRoutes[i] != &route) continue;
        const auto& proof = gLegacyPresetRoutes[i];
        const uint64_t generation = proof.generation.load();
        return generation == route.generation && proof.provider.load() == route.module
            && generation == proof.generation.load();
    }
    return false;
}

bool InstallPreset(Route& route, const Image& image) noexcept
{
    if (route.preset) return entry_detour::ReadSnapshot(route.preset).current;
    if (route.legacyPresetReady) return true;
    // mov ecx,DLSS-FG preset ID; call settings reader; test al,al; jz rel32.
    constexpr uint8_t pattern[]{0xb9,0xf1,0x1d,0xe4,0x10,0xe8,0,0,0,0,0x84,0xc0,0x0f,0x84,0,0,0,0};
    constexpr uint8_t mask[]{255,255,255,255,255,255,0,0,0,0,255,255,255,255,0,0,0,0};
    unsigned matches = 0;
    const uintptr_t match = FindPattern(image, pattern, mask, sizeof(pattern), SIZE_MAX, 0, 0, &matches);
    unsigned legacyMatches = 0;
    const bool legacy = LegacyPresetContract(image, legacyMatches);
    if (legacy && matches == 0 && Pin(image.module))
    {
        route.legacyPresetReady = true;
        Report(L"Ampere FG preset contract: provider accepts sole legacy preset 1; modern preset selection is unavailable");
        return true;
    }
    if (legacyMatches) return Reject(140);
    if (!match)
    {
        wchar_t detail[192]{};
        swprintf_s(detail, L"Ampere FG preset discovery rejected: decoded call-site matches=%u (%s); module=%p generation=%llu",
            matches, matches ? L"ambiguous" : L"missing", route.module, route.generation);
        Report(detail);
        return Reject(140);
    }
    int32_t displacement = 0; memcpy(&displacement, reinterpret_cast<void*>(match + 6), 4);
    const uintptr_t target = match + 10 + displacement;
    // The known reader takes uint32_t ID in ecx, uint32_t* in rdx and returns
    // bool in al. Require its complete argument-capture prologue, independently
    // of the eligibility version. This rejects different private ABIs.
    constexpr uint8_t prologue[]{0x48,0x89,0x5c,0x24,0x10,0x89,0x4c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0xfa,0x8b,0xd9};
    if (!image.Executable(target) || memcmp(reinterpret_cast<void*>(target), prologue, sizeof(prologue)) != 0) return Reject(141);
    void* trampoline = nullptr;
    entry_detour::InstallOptions options{}; options.generation = route.generation; options.allowRelocated = true;
    const size_t i = &route - gRoutes.data();
    if (!entry_detour::Install(entry_detour::Kind::eAmperePreset, route.module, reinterpret_cast<void*>(target),
            reinterpret_cast<void*>(kPresets[i]), trampoline, options, &route.preset)) return Reject(142);
    uint32_t privateFlags = 0;
    auto reader = Original<PresetFn>(route.preset);
    if (!reader || (reader(0x10e41df6u, &privateFlags) && (privateFlags & 4u)))
    { gFatal.store(true); return Reject(143); }
    Report(L"Ampere FG preset query hook installed; selection is fixed at the first query; driver profiles unchanged");
    return true;
}

uintptr_t DirectMaximumDeviceGate(const Image& image, unsigned& matches,
    uint8_t* nativeMaximum = nullptr) noexcept
{
    // Providers validate MultiFrameCount against a direct maximum of three
    // or five. Require the complete ComputeAndValidateTimeFactor body: nonzero
    // count, supported/unsupported device branches, index bounds, flags and
    // time-factor stores. Only image-relative data/call operands may move.
    // This profile neither changes the neural factory nor grants eligibility.
    constexpr uint8_t body[]{
        0x48,0x83,0xec,0x38,0x44,0x8b,0x81,0x18,0x05,0x00,0x00,0x4c,0x8b,0xc9,0x45,0x85,
        0xc0,0x75,0x29,0x4c,0x8d,0x0d,0x00,0x00,0x00,0x00,0xba,0x7a,0x01,0x00,0x00,0x4c,
        0x8d,0x05,0x00,0x00,0x00,0x00,0x48,0x8d,0x0d,0x00,0x00,0x00,0x00,0xe8,0x00,0x00,
        0x00,0x00,0xb8,0x05,0x00,0xd0,0xba,0x48,0x83,0xc4,0x38,0xc3,0x84,0xd2,0x0f,0x84,
        0x93,0x00,0x00,0x00,0x41,0x83,0xf8,0x05,0x76,0x1e,0xc7,0x44,0x24,0x28,0x05,0x00,
        0x00,0x00,0x4c,0x8d,0x0d,0x00,0x00,0x00,0x00,0x44,0x89,0x44,0x24,0x20,0xba,0x83,
        0x01,0x00,0x00,0xe9,0xf3,0x00,0x00,0x00,0x8b,0x89,0x14,0x05,0x00,0x00,0x41,0x80,
        0xb9,0x10,0x05,0x00,0x00,0x00,0x41,0x8d,0x40,0x01,0x41,0x0f,0x44,0xc0,0x85,0xc9,
        0x0f,0x84,0xc1,0x00,0x00,0x00,0x3b,0xc8,0x0f,0x87,0xb9,0x00,0x00,0x00,0x0f,0x94,
        0xc0,0x0f,0x57,0xc0,0x41,0x88,0x81,0x21,0x05,0x00,0x00,0x0f,0x57,0xc9,0x41,0x83,
        0xf8,0x01,0xf3,0x49,0x0f,0x2a,0xc0,0x0f,0x97,0xc0,0x41,0x88,0x81,0x20,0x05,0x00,
        0x00,0x8b,0xc1,0xf3,0x0f,0x58,0x05,0x00,0x00,0x00,0x00,0xf3,0x48,0x0f,0x2a,0xc8,
        0xb8,0x01,0x00,0x00,0x00,0xf3,0x0f,0x5e,0xc8,0xf3,0x41,0x0f,0x11,0x89,0x1c,0x05,
        0x00,0x00,0x48,0x83,0xc4,0x38,0xc3,0x41,0x83,0xf8,0x01,0x74,0x2e,0x44,0x89,0x44,
        0x24,0x20,0x4c,0x8d,0x0d,0x00,0x00,0x00,0x00,0x4c,0x8d,0x05,0x00,0x00,0x00,0x00,
        0xba,0x8c,0x01,0x00,0x00,0x48,0x8d,0x0d,0x00,0x00,0x00,0x00,0xe8,0x00,0x00,0x00,
        0x00,0xb8,0x05,0x00,0xd0,0xba,0x48,0x83,0xc4,0x38,0xc3,0x8b,0x89,0x14,0x05,0x00,
        0x00,0x83,0xf9,0x01,0x0f,0x84,0x54,0xff,0xff,0xff,0x89,0x4c,0x24,0x20,0x4c,0x8d,
        0x0d,0x00,0x00,0x00,0x00,0x48,0x8d,0x0d,0x00,0x00,0x00,0x00,0xba,0x91,0x01,0x00,
        0x00,0x4c,0x8d,0x05,0x00,0x00,0x00,0x00,0xe8,0x00,0x00,0x00,0x00,0xb8,0x05,0x00,
        0xd0,0xba,0x48,0x83,0xc4,0x38,0xc3,0x89,0x44,0x24,0x28,0x4c,0x8d,0x0d,0x00,0x00,
        0x00,0x00,0x89,0x4c,0x24,0x20,0xba,0x9a,0x01,0x00,0x00,0x4c,0x8d,0x05,0x00,0x00,
        0x00,0x00,0x48,0x8d,0x0d,0x00,0x00,0x00,0x00,0xe8,0x00,0x00,0x00,0x00,0xb8,0x05,
        0x00,0xd0,0xba,0x48,0x83,0xc4,0x38,0xc3,
    };
    constexpr size_t operands[]{0x16,0x22,0x29,0x2e,0x55,0xb7,0xe5,0xec,0xf8,
        0xfd,0x121,0x128,0x134,0x139,0x14e,0x15e,0x165,0x16a};
    std::array<uint8_t, sizeof(body)> mask{};
    mask.fill(255);
    for (const size_t offset : operands) memset(mask.data() + offset, 0, 4);
    struct Layout { uint32_t countField, zeroLine; uint8_t maximum; };
    constexpr Layout layouts[]{ {0x518,0x17a,5}, {0x500,0x122,3},
        {0x500,0x11f,3}, {0x658,0x11f,3} };
    uintptr_t start = 0;
    uint8_t selectedMaximum = 0;
    matches = 0;
    for (const auto& layout : layouts)
    {
        std::array<uint8_t, sizeof(body)> exact{};
        std::copy(std::begin(body), std::end(body), exact.begin());
        const auto set = [&exact](size_t offset, uint32_t value) noexcept
        { memcpy(exact.data() + offset, &value, sizeof(value)); };
        set(0x07, layout.countField);
        set(0x6a, layout.countField - 4);
        set(0x71, layout.countField - 8);
        set(0x97, layout.countField + 9);
        set(0xad, layout.countField + 8);
        set(0xce, layout.countField + 4);
        set(0x10d, layout.countField - 4);
        exact[0x47] = layout.maximum;
        set(0x4e, layout.maximum);
        set(0x1b, layout.zeroLine);
        set(0x5f, layout.zeroLine + 9);
        set(0xf1, layout.zeroLine + 0x12);
        set(0x12d, layout.zeroLine + 0x17);
        set(0x157, layout.zeroLine + 0x20);
        unsigned found = 0;
        const uintptr_t candidate = FindPattern(image, exact.data(), mask.data(), exact.size(), SIZE_MAX, 0, 0, &found);
        matches += found;
        if (candidate) { start = candidate; selectedMaximum = layout.maximum; }
    }
    uint32_t begin = 0, end = 0;
    if (!start || matches != 1 || !FunctionBounds(image, start, begin, end) || end - begin != sizeof(body)) return 0;
    const auto target = [start](size_t offset) noexcept
    {
        int32_t displacement = 0;
        memcpy(&displacement, reinterpret_cast<const void*>(start + offset), 4);
        return start + offset + 4 + displacement;
    };
    // Every masked operand still belongs to this exact image; data must be
    // immutable. All four failure paths use the same owned logging function.
    const uintptr_t logger = target(0x2e);
    uint32_t loggerBegin = 0, loggerEnd = 0;
    if (!FunctionBounds(image, logger, loggerBegin, loggerEnd)) return 0;
    const auto readonlyData = [&image](uintptr_t address, size_t bytes) noexcept
    {
        MEMORY_BASIC_INFORMATION memory{};
        if (!image.Contains(address, bytes)
            || VirtualQuery(reinterpret_cast<const void*>(address), &memory, sizeof(memory)) != sizeof(memory)
            || memory.AllocationBase != image.module || memory.Type != MEM_IMAGE || memory.State != MEM_COMMIT
            || memory.Protect != PAGE_READONLY) return false;
        const uintptr_t region = reinterpret_cast<uintptr_t>(memory.BaseAddress);
        return address >= region && address - region < memory.RegionSize
            && bytes <= memory.RegionSize - (address - region);
    };
    for (const size_t offset : operands)
    {
        const uintptr_t address = target(offset);
        if (offset == 0x2e || offset == 0xfd || offset == 0x139 || offset == 0x16a)
        { if (address != logger) return 0; continue; }
        if (!readonlyData(address, 4)) return 0;
    }
    constexpr char functionName[] = "EndpointCoreInputs::ComputeAndValidateTimeFactor";
    for (const size_t offset : {size_t{0x22}, size_t{0xec}, size_t{0x134}, size_t{0x15e}})
        if (!readonlyData(target(offset), sizeof(functionName))
            || memcmp(reinterpret_cast<const void*>(target(offset)), functionName, sizeof(functionName))) return 0;
    uint32_t denominatorOffset = 0;
    memcpy(&denominatorOffset, reinterpret_cast<const void*>(target(0xb7)), sizeof(denominatorOffset));
    if (denominatorOffset != 0x3f800000u) return 0; // index / (generatedCount + 1.0f)
    // All control-flow operands are exact in the body above. In particular,
    // JZ +0x93 selects the count==1 route; fall-through retains the discovered
    // bounded count, index checks and output writes in this owning function.
    if (nativeMaximum) *nativeMaximum = selectedMaximum;
    return start + 0x3c;
}

uintptr_t InlineMaximumDeviceGate(const Image& image, unsigned& matches) noexcept
{
    using Kind = ampere_host_contracts::OperandKind;
    constexpr std::array<ampere_host_contracts::Operand, 4> tailOperands{{
        {0x0c,Kind::eData,nullptr},
        {0x18,Kind::eData,"EndpointCore::ValidateMultiFrameCount"},
        {0x1f,Kind::eData,nullptr}, {0x24,Kind::eLogger,nullptr},
    }};
    uintptr_t selected = 0;
    for (const auto& profile : ampere_host_contracts::kInlineCounts)
    {
        const uintptr_t start = FindHostContract(image, profile.body, profile.body.size(),
            profile.ownerOffset, profile.ownerBytes, profile.operands, matches);
        if (!start) continue;
        const uintptr_t tail = start + profile.tailOffset;
        if (profile.ownerOffset + profile.tailOffset + profile.tail.size() > profile.ownerBytes
            || !image.Executable(tail) || !image.Executable(tail + profile.tail.size() - 1)) continue;
        bool same = true;
        for (size_t i = 0; i < profile.tail.size(); ++i)
        {
            bool operand = false;
            for (const auto& relative : tailOperands)
                operand |= i >= relative.offset && i < relative.offset + 4;
            if (!operand && reinterpret_cast<const uint8_t*>(tail)[i] != profile.tail[i]) { same = false; break; }
        }
        if (!same || !DecodedWindow(image, tail, profile.tail.size())
            || !ValidateHostOperands(image, tail, tailOperands)
            || HostRelativeTarget(tail, 0x24) != HostRelativeTarget(start, 0x2c)
            || HostRelativeTarget(tail, 0x18) != HostRelativeTarget(start, 0x1b)) continue;
        // Both final index checks retain the exact shared error tail. The
        // architecture query remains intact; only its following policy branch
        // is eligible for replacement. Count/index bounds remain mandatory.
        selected = start;
    }
    return matches == 1 ? selected : 0;
}

struct DeviceGateWrite
{
    uintptr_t address = 0;
    std::array<uint8_t, 2> expected{}, replacement{};
    size_t bytes = 0;
};
struct DeviceGatePlan
{
    std::array<DeviceGateWrite, 3> writes{};
    size_t count = 0;
    uint8_t nativeMaximum = 0;
    bool direct = false, inlined = false;
};

bool FindDeviceGatePlan(const Image& image, DeviceGatePlan& plan) noexcept
{
    constexpr uint8_t pattern[]{0x84,0xd2,0x0f,0x84,0x03,0x01,0,0,0xbe,5,0,0,0};
    constexpr uint8_t mask[]{255,255,255,255,255,255,255,255,255,255,255,255,255};
    unsigned modernMatches = 0, directMatches = 0, inlineMatches = 0;
    uint8_t directMaximum = 0;
    const uintptr_t modern = FindPattern(image, pattern, mask, sizeof(pattern), SIZE_MAX, 0, 0, &modernMatches);
    const uintptr_t direct = DirectMaximumDeviceGate(image, directMatches, &directMaximum);
    const uintptr_t inlined = InlineMaximumDeviceGate(image, inlineMatches);
    if ((!modern && !direct && !inlined) || modernMatches + directMatches + inlineMatches != 1) return false;
    plan = {};
    plan.nativeMaximum = inlined ? 3 : direct ? directMaximum : 5;
    plan.direct = direct != 0;
    plan.inlined = inlined != 0;
    if (plan.nativeMaximum == 3)
    {
        const uintptr_t comparison = inlined ? inlined + 0x4f : direct + 0x0b;
        const uintptr_t diagnostic = inlined ? inlined + 0x56 : direct + 0x12;
        // The full owning contracts prove these scalar immediates and their
        // zero upper bytes. Retain the bounded count/index checks and the
        // per-Evaluate output contract; six buffers are independently required
        // at Create and every Evaluate before reaching this provider path.
        plan.writes[plan.count++] = {comparison,{3,0},{5,0},1};
        plan.writes[plan.count++] = {diagnostic,{3,0},{5,0},1};
    }
    if (inlined)
    {
        // Retain the real device query and conditional opcode. Changing its
        // one-byte displacement to zero makes either outcome reach the same
        // bounded count path, without an unaligned two-byte publication.
        plan.writes[plan.count++] = {inlined + 0x4c,{0x22,0},{0,0},1};
    }
    else
    {
        const uintptr_t match = modern ? modern : direct;
        if (!image.Executable(match + 8 + (modern ? 0x103 : 0x93))) return false;
        // JMP +4 skips only the old JZ's relative operand.
        plan.writes[plan.count++] = {match + 2,{0x0f,0x84},{0xeb,0x04},2};
    }
    return true;
}

bool PublishDeviceGatePlan(const Image& image, const DeviceGatePlan& plan,
    protected_pointer::ProtectMemoryFn protectMemory = &VirtualProtect,
    protected_pointer::FlushInstructionCacheFn flush = &FlushInstructionCache) noexcept
{
    if (!plan.count || plan.count > plan.writes.size()) return false;
    for (size_t i = 0; i < plan.count; ++i)
    {
        const auto& write = plan.writes[i];
        if (!image.Contains(write.address, write.bytes) || !image.Executable(write.address)
            || (write.bytes != 1 && write.bytes != 2) || (write.address & (write.bytes - 1))
            || memcmp(reinterpret_cast<const void*>(write.address), write.expected.data(), write.bytes)) return false;
    }
    const auto replace = [protectMemory, flush](const DeviceGateWrite& write, bool restore) noexcept
    {
        PSAPI_WORKING_SET_EX_INFORMATION working{};
        working.VirtualAddress = reinterpret_cast<void*>(write.address);
        const bool privatePage = QueryWorkingSetEx(GetCurrentProcess(), &working, sizeof(working))
            && working.VirtualAttributes.Valid && !working.VirtualAttributes.Shared;
        return protected_pointer::ReplaceProtectedBytes(write.address,
            restore ? write.replacement.data() : write.expected.data(),
            restore ? write.expected.data() : write.replacement.data(), write.bytes,
            PAGE_EXECUTE_READ, protectMemory, flush, privatePage ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_WRITECOPY);
    };
    // Count and diagnostic limits are published first, the device branch last.
    // The caller proves the pre-first-Create boundary and retains its owner.
    for (size_t i = 0; i < plan.count; ++i)
    {
        const auto result = replace(plan.writes[i], false);
        if (result.disposition == protected_pointer::PublishDisposition::ePublishedRestored) continue;
        bool restored = true;
        for (size_t undo = i + 1; undo != 0; --undo)
        {
            const auto& write = plan.writes[undo - 1];
            if (!memcmp(reinterpret_cast<const void*>(write.address), write.expected.data(), write.bytes)
                && protected_pointer::ProtectionMatches(write.address, PAGE_EXECUTE_READ, write.bytes)) continue;
            restored &= replace(write, true).disposition == protected_pointer::PublishDisposition::ePublishedRestored;
        }
        Report(restored ? L"Ampere FG host policy publication failed; complete original contract restored"
            : L"Ampere FG host policy publication failed; rollback incomplete, provider remains blocked");
        return false;
    }
    return true;
}

bool DeviceGate(const Image& image) noexcept
{
    DeviceGatePlan plan{};
    if (!FindDeviceGatePlan(image, plan)) return Reject(144);
    if (!Pin(image.module)) return Reject(145);
    if (!PublishDeviceGatePlan(image, plan)) { gFatal.store(true); return Reject(146); }
    if (plan.nativeMaximum == 3)
        Report(L"Ampere FG host policy: native generated maximum 3 extended to bounded 5; index/resource validation retained");
    else if (plan.direct)
        Report(L"Ampere FG device policy: direct maximum 5 profile published; count/index bounds and time-factor stores verified");
    return true;
}
} // namespace

void SetLogCallback(LogCallback callback) noexcept { gLog.store(callback); ampere_gpu::SetLogCallback(callback); }
void SetFeatureLifetimeCallback(FeatureLifetimeCallback callback) noexcept
{ gFeatureLifetimeCallback.store(callback, std::memory_order_release); }
void SetCurrentFeatureCreateAttemptToken(uint64_t token) noexcept
{
    std::lock_guard lock(gCreateAttemptMutex);
    if (gCurrentCreateAttempt == SIZE_MAX) return;
    auto& proof = gCreateAttemptsInProgress[gCurrentCreateAttempt];
    if (!proof.active || !proof.valid) return;
    if (!token || (proof.token && proof.token != token))
    { proof.valid = false; return; }
    proof.token = token;
}

FeatureCreatePendingState FeatureCreatePendingStatus(uint64_t token) noexcept
{
    using State = FeatureCreatePendingState;
    if (!token) return State::eNone;
    CreateAttemptProof proof{};
    {
        std::unique_lock lock(gCreateAttemptMutex, std::try_to_lock);
        if (!lock.owns_lock()) return State::eReadBusy;
        for (const auto& candidate : gCreateAttemptsInProgress)
        {
            if (!candidate.active || candidate.token != token) continue;
            if (proof.active) return State::eInvalid;
            proof = candidate;
        }
    }
    if (proof.active)
    {
        if (!proof.valid) return State::eInvalid;
        // The scoped runtime operation may publish its token before it has
        // reached the provider/capacity boundary. Its current runtime entry
        // proves unfinished work only; defer without certifying a feature.
        if (!proof.provider || !proof.wrapper)
        {
            const auto runtime = entry_detour::ReadSnapshot(proof.runtimeCreate);
            return Ready() && runtime.current && runtime.kind == entry_detour::Kind::eNgxRuntimeD3D12CreateFeature
                && reinterpret_cast<uintptr_t>(runtime.owner) == proof.runtime
                && runtime.generation == proof.runtimeGeneration ? State::eReadBusy : State::eInvalid;
        }
        return CreateAttemptProofCurrent(proof) ? State::eCurrent : State::eInvalid;
    }
    bool released = false;
    FeatureLifetimeEvent selected{};
    {
        std::unique_lock lock(gFeatureLifetimeDeliveryMutex, std::try_to_lock);
        if (!lock.owns_lock()) return State::eReadBusy;
        bool invalid = false;
        const auto select = [&](const PendingFeatureLifetimeEvent& pending) {
            const auto& event = pending.event;
            const auto& candidate = pending.create;
            if ((event.phase != FeatureLifetimePhase::eCreated && event.phase != FeatureLifetimePhase::eReleased)
                || event.createAttemptToken != token)
                return false;
            if (!event.handle || !event.lifetime || !event.runtime || !event.runtimeGeneration
                || !event.provider || !event.providerGeneration || !event.wrapper || !event.wrapperGeneration
                || !event.publication || !event.adapterLuid
                || event.generatedFrameCapacity != ampere_policy::kMaximumGeneratedFrames)
            { invalid = true; return false; }
            if (selected.handle)
            {
                // A successful Release can queue behind this exact Created
                // notification. Preserve FIFO and wait for both transitions;
                // another handle/lifetime may never reuse its operation token.
                if (released || event.phase != FeatureLifetimePhase::eReleased
                    || selected.handle != event.handle || selected.lifetime != event.lifetime
                    || selected.runtime != event.runtime || selected.runtimeGeneration != event.runtimeGeneration
                    || selected.provider != event.provider || selected.providerGeneration != event.providerGeneration
                    || selected.wrapper != event.wrapper || selected.wrapperGeneration != event.wrapperGeneration
                    || selected.publication != event.publication || selected.adapterLuid != event.adapterLuid)
                { invalid = true; return false; }
            }
            if (event.phase == FeatureLifetimePhase::eReleased)
            {
                // This immutable event exists only after a successful native
                // Release. Its old capacity need not survive teardown; wait
                // for the observer to retire that exact retained identity.
                selected = event;
                released = true;
                return true;
            }
            if (event.createAttemptToken != candidate.token
                || event.runtime != candidate.runtime || event.runtimeGeneration != candidate.runtimeGeneration
                || event.provider != candidate.provider || event.providerGeneration != candidate.providerGeneration
                || event.wrapper != candidate.wrapper || event.wrapperGeneration != candidate.wrapperGeneration
                || event.publication != candidate.publication || event.adapterLuid != candidate.luid
                || event.generatedFrameCapacity != ampere_policy::kMaximumGeneratedFrames)
            { invalid = true; return false; }
            selected = event;
            proof = candidate;
            return true;
        };
        bool found = select(gDeliveringFeatureLifetimeEvent);
        size_t inspected = 0;
        for (const auto& pending : gPendingFeatureLifetimeEvents)
        {
            if (++inspected > 64) return State::eInvalid;
            found = select(pending) || found;
        }
        if (invalid) return State::eInvalid;
        if (!found) return State::eNone;
    }
    if (released)
    {
        const auto runtime = entry_detour::ReadSnapshot(entry_detour::Kind::eNgxRuntimeD3D12CreateFeature,
            reinterpret_cast<HMODULE>(selected.runtime));
        const auto provider = entry_detour::ReadSnapshot(entry_detour::Kind::eNgxD3D12CreateFeature,
            reinterpret_cast<HMODULE>(selected.provider));
        return runtime.current && runtime.currentEntries == 1 && runtime.generation == selected.runtimeGeneration
            && provider.current && provider.currentEntries == 1 && provider.generation == selected.providerGeneration
            && selected.provider == reinterpret_cast<uintptr_t>(ampere_gpu::Provider())
            && selected.publication == ampere_gpu::Publication() && selected.adapterLuid == ampere_gpu::AdapterLuid()
            && selected.wrapper == gActiveWrapper.load(std::memory_order_acquire)
            && selected.wrapperGeneration == gActiveWrapperGeneration.load(std::memory_order_acquire)
            ? State::eCurrent : State::eInvalid;
    }
    return CreateAttemptProofCurrent(proof) ? State::eCurrent : State::eInvalid;
}

bool FeatureCreatePendingCurrent(uint64_t token) noexcept
{ return FeatureCreatePendingStatus(token) == FeatureCreatePendingState::eCurrent; }
bool ObserveD3D12Device(void* device) noexcept
{
    const bool verified = ampere_gpu::ObserveD3D12Device(device);
    gD3D12Confirmed.store(verified);
    return verified;
}
bool ObserveVulkanPhysicalDevice(void*) noexcept { return false; }
bool AdapterVerified() noexcept { return ampere_gpu::AdapterVerified(); }
bool Ready() noexcept { return gD3D12Confirmed.load() && AdapterVerified() && ampere_gpu::Ready() && gStartupComplete.load() && !gFatal.load(); }
uint32_t FailureCode() noexcept
{
    if (const auto fatal = gFatalFailure.load()) return fatal;
    if (const auto primary = gFailures.Primary().code) return primary;
    return ampere_gpu::FailureCode() ? ampere_gpu::FailureCode() : gFailure.load();
}
ampere_diagnostics::Snapshot Diagnostics() noexcept
{
    return {gFailures.First(), gFailures.Primary(), gFailures.Last(),
        gCreateAttempts.load(), gCreateBlockedBeforeProvider.load(), gEvaluateAttempts.load(),
        gCandidateProviderVersion.load(), ampere_gpu::PreparationStage(), gStartupFailureMask.load()};
}
bool EarlyInitObserved() noexcept { return gEarlyInit.load(std::memory_order_acquire); }
uint64_t PresetQueries() noexcept { return gPresetQueries.load(); }
bool LegacySinglePreset() noexcept
{
    if (gFatal.load()) return false;
    const HMODULE provider = ampere_gpu::Provider();
    if (!provider) return false;
    const auto current = entry_detour::ReadSnapshot(entry_detour::Kind::eNgxD3D12CreateFeature, provider);
    if (!current.current || current.currentEntries != 1 || current.owner != provider || !current.generation) return false;
    // Every retained route has its own immutable capability proof. Preparing
    // another provider must not replace the currently selected program's
    // status, and a later Bind may select either already-prepared program.
    for (const auto& proof : gLegacyPresetRoutes)
    {
        const uint64_t generation = proof.generation.load();
        if (generation != current.generation || proof.provider.load() != provider
            || generation != proof.generation.load()) continue;
        return provider == ampere_gpu::Provider() && !gFatal.load();
    }
    return false;
}
uint64_t Evaluations() noexcept { return gEvaluations.load(); }
uint64_t Rejections() noexcept { return gRejections.load(); }
uint32_t NativeMaximumGeneratedFrames() noexcept { return gNativeMaximum.load(); }
uint32_t NativeMaximumReadResult() noexcept { return gNativeMaximumResult.load(); }
uint32_t StartupMaximumGeneratedFrames() noexcept { return gStartupMaximum.load(); }
uint32_t PresentationBuffers() noexcept { return gPresentationBuffers.load(); }
uint32_t CertifiedMaximumGeneratedFrames() noexcept { return gCertifiedMaximum.load(std::memory_order_acquire); }
void ObserveActiveWrapper(HMODULE module, uint64_t generation) noexcept
{
    // The control path may hold its own locks. Publish without taking gCalls
    // so a concurrent NGX callback cannot introduce a lock-order inversion.
    const uintptr_t base = reinterpret_cast<uintptr_t>(module);
    if (gActiveWrapper.load() == base && gActiveWrapperGeneration.load() == generation) return;
    if (gWrapperWriter.test_and_set(std::memory_order_acquire)) return;
    gCertifiedMaximum.store(0, std::memory_order_release);
    gPresentationBuffers.store(0, std::memory_order_release);
    gActiveWrapperGeneration.store(0, std::memory_order_release);
    gActiveWrapper.store(base, std::memory_order_release);
    gActiveWrapperGeneration.store(generation, std::memory_order_release);
    gWrapperWriter.clear(std::memory_order_release);
}
int32_t LastCreateCount() noexcept { return gLastCreateCount.load(); }
uint64_t CreatedFeatures() noexcept { return gCreatedFeatures.load(); }
uint64_t SubmittedBatches() noexcept { return gSubmittedBatches.load(); }

bool PatchProvider(HMODULE module, const wchar_t* path) noexcept
{
    std::lock_guard callLock(gCalls);
    if (!AdapterVerified() || gFatal.load() || (!gStartupDepth && !gStartupComplete.load())) return false;
    for (const auto& route : gRoutes)
        if (route.module == module && RoutePresetCurrent(route)
            && RouteEntryCurrent(route, route.create) && RouteEntryCurrent(route, route.evaluate)
            && RouteEntryCurrent(route, route.release) && ampere_gpu::PreparedProvider(module))
            return ampere_gpu::PatchProvider(module, path);
    return false;
}

void ObserveEarlyInit() noexcept { gEarlyInit.store(true); }
void SetPreparationBoundaryResolver(PreparationBoundaryResolver resolver) noexcept
{ gPreparationBoundaryResolver.store(resolver, std::memory_order_release); }
void BeginInit() noexcept
{
    ++gInitDepth;
    // Unknown/Ada slInit is a native call and can wait on another thread's NGX
    // callbacks. Unknown Ampere requirement overrides acquire gCalls inside
    // Requirements/ObserveModule, covering each actual mutation and restore.
    // Remember the depth that acquired a persistent Ampere lock: selection can
    // change inside slInit, and nested scopes must unwind the original owner.
    if (!gInitLockDepth
#if MFG_UNLOCK_RUNTIME_GPU_SELECTION
        && gpu_dispatch::IsAmpere()
#endif
        )
    {
        gCalls.lock();
        gInitLockDepth = gInitDepth;
    }
}
void EndInit() noexcept
{
    if (!gInitDepth) return;
    std::lock_guard lock(gCalls);
    for (auto& route : gRoutes)
    {
        if (route.availability.active) route.availability.Set(false);
        if (route.metadata.active) route.metadata.Set(false);
    }
    if (gInitLockDepth == gInitDepth)
    {
        gInitLockDepth = 0;
        gCalls.unlock();
    }
    --gInitDepth;
}
bool BeforeFirstFeatureCreate() noexcept
{ return !gPipelineCreateObserved.load(std::memory_order_acquire) && !gCreateAttempts.load() && !gCreatedFeatures.load(); }
bool BeginNativePreparation() noexcept
{
    // Init forwarding owns its short pre-call boundary lock. Never wait for
    // another thread that may already hold gCalls around native startup and
    // require this same forwarding entry. Missing the early window fails closed.
    if (!gCalls.try_lock()) return false;
    if (!BeforeFirstFeatureCreate() || gStartupComplete.load() || gFatal.load())
    { gCalls.unlock(); return false; }
    ++gPreparationDepth;
    return true;
}
void EndNativePreparation() noexcept
{
    if (!gPreparationDepth) return;
    --gPreparationDepth;
    gCalls.unlock();
}
void BeginStartup() noexcept
{
    gCalls.lock(); ++gStartupDepth;
    if (GetModuleHandleW(L"RTX30MFGCore.dll") || GetModuleHandleW(L"RTX40MFGCore.dll"))
    { gFatal.store(true); Reject(156); }
    if (!gEarlyInit.load()) { gFatal.store(true); Reject(155); }
}
void EndStartup(bool accepted) noexcept
{
    for (auto& route : gRoutes)
    {
        if (route.availability.active) route.availability.Set(false);
        if (route.metadata.active) route.metadata.Set(false);
    }
    bool runtimeCovered = false, providerCovered = false;
    for (const auto& route : gRoutes)
    {
        const bool covered = route.prepared && route.create && route.evaluate && route.release
            && RouteEntryCurrent(route, route.create)
            && RouteEntryCurrent(route, route.evaluate)
            && RouteEntryCurrent(route, route.release);
        runtimeCovered |= covered && route.runtime;
        providerCovered |= covered && RoutePresetCurrent(route)
            && ampere_gpu::PreparedProvider(route.module);
    }
    const uint32_t missing = (!accepted ? 0x01u : 0)
        | (!runtimeCovered ? 0x02u : 0) | (!providerCovered ? 0x04u : 0)
        | (gStartupMaximum.load() != ampere_policy::kMaximumGeneratedFrames ? 0x08u : 0)
        | (!gD3D12Confirmed.load() ? 0x10u : 0) | (!AdapterVerified() ? 0x20u : 0)
        | (!ampere_gpu::Ready() ? 0x40u : 0) | (gFatal.load() ? 0x80u : 0);
    const bool ready = missing == 0;
    gStartupFailureMask.store(missing, std::memory_order_release);
    gStartupComplete.store(ready);
    if (ready) { gFailure.store(0); gFailures.Recover(); }
    else
    {
        Reject(ampere_diagnostics::kStartupAdmissionFailure);
        wchar_t detail[320]{};
        swprintf_s(detail, L"Ampere startup admission incomplete: mask=0x%02X slStartupAccepted=%u runtimeCovered=%u providerCovered=%u "
            L"startupMaximum=%u d3d12=%u adapter=%u program=%u fatal=%u",
            missing, static_cast<unsigned>(accepted), static_cast<unsigned>(runtimeCovered), static_cast<unsigned>(providerCovered),
            gStartupMaximum.load(), static_cast<unsigned>(gD3D12Confirmed.load()), static_cast<unsigned>(AdapterVerified()),
            static_cast<unsigned>(ampere_gpu::Ready()), static_cast<unsigned>(gFatal.load()));
        Report(detail);
    }
    Report(ready ? L"Ampere startup complete; temporary gates restored; MFG program prepared"
        : L"Ampere startup incomplete; temporary gates restored; MFG admission unavailable");
    --gStartupDepth; gCalls.unlock();
}

bool InspectRuntimeLayout(Route& route, const Image& image, const wchar_t* path) noexcept
{
    if (!route.runtimeInspected)
    {
        route.runtimeInspected = true;
        const uintptr_t create = reinterpret_cast<uintptr_t>(GetProcAddress(route.module, "NVSDK_NGX_D3D12_CreateFeature"));
        const uintptr_t metadata = FindPattern(image, kAmpereNgxMetadataPattern.data(), kAmpereNgxMetadataPatternMask.data(),
            kAmpereNgxMetadataPattern.size(), kAmpereNgxMetadataBranchOffset);
        const uintptr_t validation = FindCreateValidation(image, create);
        const bool pinned = metadata && validation && Pin(route.module);
        if (pinned)
        {
            route.metadata = {metadata + kAmpereNgxMetadataBranchOffset, kAmpereNgxMetadataOriginal};
            route.validation = {validation + kAmpereNgxCreateValidationBranchOffset, kAmpereNgxCreateValidationOriginal};
        }
        wchar_t detail[2048]{};
        swprintf_s(detail, L"Ampere NGX interface: metadata=0x%X createValidation=0x%X modulePinned=%u role=%s; discovery=%s path=%.1500ls",
            metadata ? static_cast<uint32_t>(metadata - image.base) : 0,
            validation ? static_cast<uint32_t>(validation - image.base) : 0, static_cast<unsigned>(pinned),
            pinned ? L"verified-runtime" : L"native-forwarding-unhooked",
            !metadata ? L"metadata-signature-missing-or-ambiguous" : !validation ? L"create-call-path-unrecognized"
                : !pinned ? L"module-pin-failed" : L"decoded-create-call-path", path ? path : L"");
        Report(detail);
    }
    return route.metadata.address && route.validation.address;
}

void ObserveModule(HMODULE module, const wchar_t* path, uint64_t generation,
    bool wrapper, bool provider, bool runtime) noexcept
{
#if MFG_UNLOCK_RUNTIME_GPU_SELECTION
    if (gpu_dispatch::IsAda() || gpu_dispatch::Selected() == gpu_dispatch::Family::eConflict) return;
#endif
    if (!module || !generation || (!wrapper && !provider && !runtime) || gFatal.load()) return;
    std::lock_guard callLock(gCalls);
    std::lock_guard lock(gInstall);
    Image image{};
    if (!image.Open(module)) return;
    Route* route = FindRoute(module, generation);
    if (!route) { Reject(150); return; }
    route->runtime = runtime;
    const size_t index = route - gRoutes.data();
    if (provider && !route->candidateLogged)
    {
        route->candidateLogged = true;
        dlssg_provider_policy::VersionTriplet version{};
        const bool read = dlssg_provider_policy::ReadProviderVersion(path, version);
        gCandidateProviderVersion.store(read ? (uint64_t{version.major} << 32)
            | (uint64_t{version.minor} << 16) | version.build : 0, std::memory_order_release);
        wchar_t detail[2048]{};
        swprintf_s(detail, L"Ampere provider candidate: version=%u.%u.%u versionRead=%u eligible=%u generation=%llu path=%.1600ls",
            version.major, version.minor, version.build, static_cast<unsigned>(read),
            static_cast<unsigned>(dlssg_provider_policy::IsSupportedProvider(module, path)), generation, path ? path : L"");
        Report(detail);
    }
    if (runtime) InspectRuntimeLayout(*route, image, path);
    if (wrapper && !route->wrapperInspected)
    {
        route->wrapperInspected = true;
        ampere_wrapper::Layout layout{};
        if (ampere_wrapper::Discover(module, layout) && Pin(module))
        {
            route->wrapperLayout = layout;
            Report(L"Ampere MFG wrapper allocation layout recognized; waiting for early capability bootstrap and actual buffers");
        }
        else
            Report(L"Ampere MFG wrapper allocation layout unrecognized; startup capability bootstrap remains blocked");
    }
    if (provider && !route->prepared && (gStartupDepth || gPreparationDepth)
        && BeforeFirstFeatureCreate()
        && gD3D12Confirmed.load() && AdapterVerified())
    {
        if (!dlssg_provider_policy::IsSupportedProvider(module, path)) { Reject(3); return; }
        const auto resolveBoundary = gPreparationBoundaryResolver.load(std::memory_order_acquire);
        auto boundary = resolveBoundary ? resolveBoundary(module, generation) : ampere_gpu::PreparationBoundary{};
        // Prove the early owner before installing a preset reader or changing
        // the private device gate, not merely before descriptor publication.
        if (!boundary.Proven()) { Reject(15); return; }
        if (!InstallPreset(*route, image)) return;
        boundary = resolveBoundary(module, generation);
        if (!boundary.Proven()) { Reject(15); return; }
        if (!route->deviceGateReady)
            route->deviceGateReady = DeviceGate(image);
        boundary = resolveBoundary(module, generation);
        if (!route->deviceGateReady || !ampere_gpu::PatchProvider(module, path, boundary)) { Reject(151); return; }
        route->prepared = true;
        RetainLegacyPresetContract(index, *route);
    }
    if (runtime && !route->prepared && (gInitDepth || gStartupDepth || gPreparationDepth))
    {
        // Owned NGX exports also occur in game-local intermediaries. Discovery
        // of one is not a failed driver admission; leave its forwarding intact.
        if (!route->metadata.address || !route->validation.address) return;
        if (!InstallExport(*route, "NVSDK_NGX_D3D12_GetFeatureRequirements", entry_detour::Kind::eAmpereRequirements,
                reinterpret_cast<void*>(kRequirements[index]), route->requirements)
            || !InstallExport(*route, "NVSDK_NGX_D3D12_GetCapabilityParameters", entry_detour::Kind::eAmpereParameters,
                reinterpret_cast<void*>(kParameters[index]), route->parameters)) { Reject(153); return; }
        route->prepared = true;
    }
    if (wrapper && (gInitDepth || gStartupDepth) && !route->availability.address)
    {
        const uintptr_t modern = FindPattern(image, kAmpereSlDlssgAvailabilityPattern.data(), kAmpereSlDlssgAvailabilityPatternMask.data(),
            kAmpereSlDlssgAvailabilityPattern.size(), kAmpereSlDlssgAvailabilityBranchOffset);
        const uintptr_t legacy = FindPattern(image, kAmpereSlDlssgAvailabilityLegacyPattern.data(), kAmpereSlDlssgAvailabilityLegacyPatternMask.data(),
            kAmpereSlDlssgAvailabilityLegacyPattern.size(), kAmpereSlDlssgAvailabilityLegacyBranchOffset);
        if ((modern != 0) != (legacy != 0))
        {
            if (!Pin(module)) return;
            route->availability = {(modern ? modern + kAmpereSlDlssgAvailabilityBranchOffset
                : legacy + kAmpereSlDlssgAvailabilityLegacyBranchOffset), kAmpereSlDlssgAvailabilityOriginal};
        }
    }
    if (gStartupDepth && gD3D12Confirmed.load() && AdapterVerified())
    {
        if (route->metadata.address) route->metadata.Set(true);
        if (route->availability.address) route->availability.Set(true);
    }
}

bool InstallRoute(HMODULE module, const wchar_t* path, uint64_t generation, bool runtime,
    entry_detour::ForwardPreCall beforeCreate, entry_detour::ForwardPreCall beforeEvaluate) noexcept
{
    std::lock_guard callLock(gCalls);
    std::lock_guard lock(gInstall);
    if (!module || !generation) return false;
#if MFG_UNLOCK_RUNTIME_GPU_SELECTION
    if (!runtime && !dlssg_provider_policy::IsSupportedProvider(module, path)) return false;
#endif
    auto* route = FindRoute(module, generation);
    if (!route) return false;
    route->runtime = runtime;
    // Prove the private driver layout before installing any mutating callback.
    // A native intermediary continues to the separately verified runtime. If
    // that runtime is absent, startup and provider admission still fail closed.
    // Selected Ada callbacks are native pass-throughs and need no Ampere layout.
    if (runtime
#if MFG_UNLOCK_RUNTIME_GPU_SELECTION
        && !gpu_dispatch::IsAda()
#endif
        )
    {
        Image image{};
        if (!image.Open(module) || !InspectRuntimeLayout(*route, image, path)) return false;
    }
    if (beforeCreate) route->beforeCreate = beforeCreate;
    if (beforeEvaluate) route->beforeEvaluate = beforeEvaluate;
    const size_t i = route - gRoutes.data();
    if (runtime && (!InstallExport(*route, "NVSDK_NGX_D3D12_GetFeatureRequirements", entry_detour::Kind::eAmpereRequirements,
            reinterpret_cast<void*>(kRequirements[i]), route->requirements)
        || !InstallExport(*route, "NVSDK_NGX_D3D12_GetCapabilityParameters", entry_detour::Kind::eAmpereParameters,
            reinterpret_cast<void*>(kParameters[i]), route->parameters))) return false;
    // Install Release and Evaluate first. No successful FG Create can escape
    // before its exact handle lifetime and evaluation boundary are covered.
    if (!InstallExport(*route, "NVSDK_NGX_D3D12_ReleaseFeature", entry_detour::Kind::eAmpereRelease,
            reinterpret_cast<void*>(kReleases[i]), route->release)
        || !InstallExport(*route, "NVSDK_NGX_D3D12_EvaluateFeature", runtime ? entry_detour::Kind::eNgxRuntimeD3D12EvaluateFeature : entry_detour::Kind::eNgxD3D12EvaluateFeature,
            reinterpret_cast<void*>(kEvaluates[i]), route->evaluate)) return false;
    return InstallExport(*route, "NVSDK_NGX_D3D12_CreateFeature", runtime ? entry_detour::Kind::eNgxRuntimeD3D12CreateFeature : entry_detour::Kind::eNgxD3D12CreateFeature,
        reinterpret_cast<void*>(kCreates[i]), route->create);
}
} // namespace ampere_backend
