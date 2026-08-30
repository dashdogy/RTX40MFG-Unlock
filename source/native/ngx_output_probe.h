#pragma once

#include <Windows.h>
#include <d3d12.h>
#include <nvsdk_ngx.h>

#include <cstdint>

namespace ngx_output_probe
{
using LogCallback = void (*)(const wchar_t* message);

struct Snapshot
{
    bool enabled = false;
    bool immutableEnabled = false;
    bool queueHookInstalled = false;
    uint64_t scheduled = 0;
    uint64_t submitted = 0;
    uint64_t captured = 0;
    uint64_t dropped = 0;
    uint64_t completeBatches = 0;
    uint64_t duplicateBatches = 0;
    uint64_t immutablePrepared = 0;
    uint64_t immutableSubmitted = 0;
    uint64_t immutableRetired = 0;
    uint64_t immutableDropped = 0;
    uint64_t immutableReservationReclaims = 0;
    uint32_t immutableAllocated = 0;
};

struct ImmutableOutput
{
    ID3D12Resource* resource = nullptr;
    uint64_t sequence = 0;
    uint32_t slot = UINT32_MAX;
    bool completesBatch = false;

    explicit operator bool() const { return resource != nullptr; }
};

struct FeatureDrainSnapshot
{
    bool hasStreamlineFence = false;
    bool streamlineFenceComplete = false;
    uint64_t streamlineFenceValue = 0;
    uint64_t streamlineFenceCompletedValue = 0;
    uint32_t outstandingImmutableOutputs = 0;
};

enum class CapturedOutputKind : uint8_t
{
    eInterpolated,
    eReal,
};

void Configure(bool enabled, bool immutableEnabled, LogCallback logCallback);
void SetImmutableEnabled(bool enabled);
bool RegisterDevice(ID3D12Device* device);
bool RegisterQueue(ID3D12CommandQueue* queue);
ImmutableOutput PrepareImmutableOutput(ID3D12GraphicsCommandList* commandList,
    ID3D12Resource* originalOutput, NVSDK_NGX_Parameter* parameters,
    uint64_t batch, int count, int index, bool completesBatch);
bool FinalizeImmutableOutput(ID3D12GraphicsCommandList* commandList,
    ID3D12Resource* originalOutput, const ImmutableOutput& immutable);
void CancelImmutableOutput(NVSDK_NGX_Parameter* parameters,
    ID3D12Resource* originalOutput, const ImmutableOutput& immutable);
void AbandonImmutableBatch(uint64_t batch);
void NotifyStreamlineCompletionFence(ID3D12Fence* fence, uint64_t value);
void BeginFeatureRecycle();
FeatureDrainSnapshot ReadFeatureDrainSnapshot();
bool FinishFeatureRecycle();
bool CaptureAfterEvaluate(ID3D12GraphicsCommandList* commandList,
    ID3D12Resource* output, const NVSDK_NGX_Handle* handle,
    uint64_t batch, uint64_t frameId, int count, int index,
    CapturedOutputKind kind = CapturedOutputKind::eInterpolated);
Snapshot ReadSnapshot();
}
