#pragma once
#include <atomic>
#include <cstdint>

namespace ampere_diagnostics
{
inline constexpr uint32_t kCreateSubmissionFailure = 172;
inline constexpr uint32_t kEvaluationSubmissionFailure = 173;
inline constexpr uint32_t kStartupAdmissionFailure = 174;
inline constexpr uint32_t kRuntimeDispatchFailure = 176;

enum class Preparation : uint32_t
{
    ePending, eEligibility, eVersion, eProfile, eImage, eDescriptorTable,
    eModuleLifetime, eKernelDescriptor, ePtxIdentity, ePtxTransform,
    eNativeCache, eDescriptorClone, ePublication, eRevalidation, eReady,
    eCudaValidation, eFontProgram
};
constexpr const char* PreparationName(uint32_t stage) noexcept
{
    switch (static_cast<Preparation>(stage))
    {
    case Preparation::ePending: return "pending";
    case Preparation::eEligibility: return "provider eligibility";
    case Preparation::eVersion: return "embedded version";
    case Preparation::eProfile: return "temporal profile";
    case Preparation::eImage: return "provider PE image";
    case Preparation::eDescriptorTable: return "kernel descriptor table";
    case Preparation::eModuleLifetime: return "provider module lifetime";
    case Preparation::eKernelDescriptor: return "kernel descriptor ownership";
    case Preparation::ePtxIdentity: return "PTX identity";
    case Preparation::ePtxTransform: return "PTX transform";
    case Preparation::eNativeCache: return "native cache";
    case Preparation::eDescriptorClone: return "descriptor clone";
    case Preparation::ePublication: return "kernel publication";
    case Preparation::eRevalidation: return "publication revalidation";
    case Preparation::eReady: return "program prepared";
    case Preparation::eCudaValidation: return "CUDA program loadability";
    case Preparation::eFontProgram: return "auxiliary font program";
    default: return "unknown";
    }
}

struct Failure
{
    uint32_t code = 0;
    uint32_t ngxResult = 0;
};

// Keep each code and its NGX result in one publication. Later guard refusals
// must not replace the operation which originally failed. Recovery clears the
// active failure, while the first failure remains available for this run.
class Failures
{
    std::atomic<uint64_t> first_{0}, primary_{0}, last_{0};
    static constexpr Failure Decode(uint64_t value) noexcept
    { return {static_cast<uint32_t>(value), static_cast<uint32_t>(value >> 32)}; }
public:
    void Record(uint32_t code, uint32_t ngxResult = 0) noexcept
    {
        if (!code) return;
        const uint64_t value = (uint64_t{ngxResult} << 32) | code;
        uint64_t empty = 0;
        first_.compare_exchange_strong(empty, value, std::memory_order_acq_rel);
        empty = 0;
        primary_.compare_exchange_strong(empty, value, std::memory_order_acq_rel);
        last_.store(value, std::memory_order_release);
    }
    void Recover() noexcept
    {
        primary_.store(0, std::memory_order_release);
        last_.store(0, std::memory_order_release);
    }
    Failure First() const noexcept { return Decode(first_.load(std::memory_order_acquire)); }
    Failure Primary() const noexcept { return Decode(primary_.load(std::memory_order_acquire)); }
    Failure Last() const noexcept { return Decode(last_.load(std::memory_order_acquire)); }
};

struct Snapshot
{
    Failure first{}, primary{}, last{};
    uint64_t createAttempts = 0;
    uint64_t createBlockedBeforeProvider = 0;
    uint64_t evaluateAttempts = 0;
    uint64_t candidateVersion = 0;
    uint32_t preparationStage = 0;
    uint32_t startupFailureMask = 0;
};

constexpr const char* FailureName(uint32_t code) noexcept
{
    switch (code)
    {
    case 0: return "none";
    case 3: return "provider version is not eligible";
    case 4: return "provider kernel layout could not be validated";
    case 5: return "provider PTX identity could not be validated";
    case 6: return "provider PTX transform failed";
    case 10: return "kernel publication failed";
    case 11: return "published provider changed; restart required";
    case 13: return "native kernel program unavailable";
    case 14: return "CUDA rejected the kernel program before publication";
    case 15: return "early provider load or initialization proof unavailable";
    case 16: return "auxiliary font identity or Ampere transform unavailable";
    case 120: return "Create rejected before startup admission";
    case 131: return "evaluation inputs or feature lifetime rejected";
    case 140: return "FG preset call site could not be validated";
    case 141: return "FG preset reader layout could not be validated";
    case 142: return "FG preset hook installation failed";
    case 151: return "provider preparation failed";
    case 152: return "NGX runtime layout could not be validated";
    case 155: return "early Streamline initialization was not intercepted";
    case 170: return "early wrapper allocation could not be established";
    case 171: return "wrapper buffer ownership could not be established";
    case kCreateSubmissionFailure: return "NGX Create failed";
    case kEvaluationSubmissionFailure: return "NGX evaluation failed";
    case kStartupAdmissionFailure: return "Streamline startup did not establish MFG admission";
    case kRuntimeDispatchFailure: return "NGX runtime dispatch target changed or is invalid";
    default: return "compatibility or ownership check failed";
    }
}
}
