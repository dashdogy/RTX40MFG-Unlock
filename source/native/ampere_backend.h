#pragma once
#include "ampere_gpu.h"
#include "entry_detour.h"
#include "ampere_diagnostics.h"

namespace ampere_backend
{
using LogCallback = ampere_gpu::LogCallback;
enum class FeatureLifetimePhase : uint32_t
{
    eCreated = 1,
    eReleased = 2,
};
struct FeatureLifetimeEvent
{
    FeatureLifetimePhase phase{};
    uintptr_t handle = 0;
    uint64_t lifetime = 0;
    uintptr_t runtime = 0;
    uint64_t runtimeGeneration = 0;
    uintptr_t provider = 0;
    uint64_t providerGeneration = 0;
    uintptr_t wrapper = 0;
    uint64_t wrapperGeneration = 0;
    uint64_t publication = 0;
    uint64_t adapterLuid = 0;
    uint32_t generatedFrameCapacity = 0;
    // Token supplied by the exact pre-Create callback and retained with the
    // successful feature. It is never sampled later during event delivery.
    uint64_t createAttemptToken = 0;
};
using FeatureLifetimeCallback = void(*)(const FeatureLifetimeEvent&) noexcept;
using PreparationBoundaryResolver = ampere_gpu::PreparationBoundary(*)(HMODULE, uint64_t) noexcept;
void SetPreparationBoundaryResolver(PreparationBoundaryResolver) noexcept;
void SetLogCallback(LogCallback) noexcept;
// Notifications are immutable snapshots queued in native mutation order and
// delivered by the calling threads after gCalls is released. A release can
// therefore be nested inside an outer Streamline free call without reordering
// Created/Released delivery or requiring a permanent dispatch worker.
void SetFeatureLifetimeCallback(FeatureLifetimeCallback) noexcept;
// Valid only for the current thread's in-progress FG Create callback.
void SetCurrentFeatureCreateAttemptToken(uint64_t token) noexcept;
enum class FeatureCreatePendingState : uint8_t { eNone, eCurrent, eReadBusy, eInvalid };
// Read-only and nonblocking. Busy defers control until a later snapshot; it
// never certifies a feature. None still requires all retained-feature checks.
FeatureCreatePendingState FeatureCreatePendingStatus(uint64_t token) noexcept;
bool FeatureCreatePendingCurrent(uint64_t token) noexcept;
bool ObserveD3D12Device(void*) noexcept;
bool ObserveVulkanPhysicalDevice(void*) noexcept;
bool PatchProvider(HMODULE, const wchar_t*) noexcept;
bool AdapterVerified() noexcept;
bool Ready() noexcept;
uint32_t FailureCode() noexcept;
ampere_diagnostics::Snapshot Diagnostics() noexcept;
bool EarlyInitObserved() noexcept;
uint64_t PresetQueries() noexcept;
// Current retained provider exposes only the legacy preset value 1. This is
// capability evidence; a real successful Create records the observed value.
bool LegacySinglePreset() noexcept;
uint64_t Evaluations() noexcept;
uint64_t Rejections() noexcept;
uint32_t NativeMaximumGeneratedFrames() noexcept;
uint32_t NativeMaximumReadResult() noexcept;
uint32_t StartupMaximumGeneratedFrames() noexcept;
uint32_t PresentationBuffers() noexcept;
uint32_t CertifiedMaximumGeneratedFrames() noexcept;
void ObserveActiveWrapper(HMODULE, uint64_t generation) noexcept;
int32_t LastCreateCount() noexcept;
uint64_t CreatedFeatures() noexcept;
uint64_t SubmittedBatches() noexcept;

// All module work occurs on loader return or ordinary calls, never from the
// loader notification callback. Startup is serialized with feature creation.
void BeginStartup() noexcept;
void ObserveEarlyInit() noexcept;
void BeginInit() noexcept;
void EndInit() noexcept;
bool BeginNativePreparation() noexcept;
void EndNativePreparation() noexcept;
bool BeforeFirstFeatureCreate() noexcept;
void EndStartup(bool accepted) noexcept;
void ObserveModule(HMODULE, const wchar_t*, uint64_t generation,
    bool wrapper, bool provider, bool runtime) noexcept;
bool InstallRoute(HMODULE, const wchar_t*, uint64_t generation, bool runtime,
    entry_detour::ForwardPreCall beforeCreate,
    entry_detour::ForwardPreCall beforeEvaluate) noexcept;
}
