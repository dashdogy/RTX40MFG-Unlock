#include <cstdint>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <nvsdk_ngx_params.h>

struct ID3D12GraphicsCommandList;

namespace
{
std::atomic<uint32_t> gTemporalCalls{0};
std::atomic<uint32_t> gTemporalSequenceValid{1};
std::atomic<int32_t> gLastCount{0};
std::atomic<int32_t> gLastIndex{0};
std::mutex gTemporalMutex;
std::unordered_map<const NVSDK_NGX_Handle*, uint32_t> gSeenMaskByHandle;
std::unordered_map<const NVSDK_NGX_Handle*, uint32_t> gCompletedBatchesByHandle;
std::unordered_set<unsigned long long> gFrameIds;
}

extern "C" __declspec(dllexport) void NVSDK_NGX_D3D12_CreateFeature()
{
}

extern "C" __declspec(dllexport) uint32_t NVSDK_NGX_GetGPUArchitecture()
{
    return 0;
}

extern "C" __declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV
NVSDK_NGX_D3D12_EvaluateFeature(ID3D12GraphicsCommandList*,
    const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* parameters,
    PFN_NVSDK_NGX_ProgressCallback)
{
    int count = 0;
    int index = 0;
    int countMax = 0;
    int mustCallEval = 0;
    ID3D12Resource* disableInterpolation = nullptr;
    int notRenderingGameFrames = 1;
    int streamlineMode = 0;
    int reset = 0;
    int evalFlags = -1;
    unsigned long long frameId = 0;
    gTemporalCalls.fetch_add(1, std::memory_order_relaxed);
    bool valid = parameters
        && parameters->Get("DLSSG.MultiFrameCount", &count) == NVSDK_NGX_Result_Success
        && parameters->Get("DLSSG.MultiFrameIndex", &index) == NVSDK_NGX_Result_Success
        && parameters->Get("DLSSG.MultiFrameCountMax", &countMax)
            == NVSDK_NGX_Result_Success
        && parameters->Get("DLSSG.MustCallEval", &mustCallEval)
            == NVSDK_NGX_Result_Success
        && parameters->Get("DLSSG.OutputDisableInterpolation", &disableInterpolation)
            == NVSDK_NGX_Result_Success
        && parameters->Get("DLSSG.NotRenderingGameFrames", &notRenderingGameFrames)
            == NVSDK_NGX_Result_Success
        && parameters->Get("DLSSG.StreamlineMode", &streamlineMode)
            == NVSDK_NGX_Result_Success
        && parameters->Get("DLSSG.Reset", &reset) == NVSDK_NGX_Result_Success
        && parameters->Get("DLSSG.EvalFlags", &evalFlags)
            == NVSDK_NGX_Result_Success
        && parameters->Get("DLSSG.BackbufferFrameID", &frameId)
            == NVSDK_NGX_Result_Success;
    {
        std::lock_guard lock(gTemporalMutex);
        valid = valid && count == 5 && index >= 1 && index <= 5
            && countMax == 5 && mustCallEval == 1
            && disableInterpolation == reinterpret_cast<ID3D12Resource*>(
                static_cast<uintptr_t>(0x3000))
            && notRenderingGameFrames == 0
            && streamlineMode == 1 && evalFlags == 0 && frameId != 0;
        if (valid)
        {
            valid = gFrameIds.insert(frameId).second;
            valid = valid && reset == 0;
            uint32_t& seenMask = gSeenMaskByHandle[handle];
            const uint32_t bit = 1u << static_cast<uint32_t>(index - 1);
            valid = (seenMask & bit) == 0;
            if (valid)
            {
                seenMask |= bit;
                if (seenMask == 0x1fu)
                {
                    gSeenMaskByHandle.erase(handle);
                    ++gCompletedBatchesByHandle[handle];
                }
            }
        }
    }
    if (!valid)
        gTemporalSequenceValid.store(0, std::memory_order_relaxed);
    gLastCount.store(count, std::memory_order_relaxed);
    gLastIndex.store(index, std::memory_order_relaxed);
    return NVSDK_NGX_Result_Success;
}

extern "C" __declspec(dllexport) void FakeResetTemporalSequence()
{
    gTemporalCalls.store(0, std::memory_order_relaxed);
    gTemporalSequenceValid.store(1, std::memory_order_relaxed);
    gLastCount.store(0, std::memory_order_relaxed);
    gLastIndex.store(0, std::memory_order_relaxed);
    std::lock_guard lock(gTemporalMutex);
    gSeenMaskByHandle.clear();
    gCompletedBatchesByHandle.clear();
    gFrameIds.clear();
}

extern "C" __declspec(dllexport) uint32_t FakeTemporalSequenceValid()
{
    return gTemporalSequenceValid.load(std::memory_order_relaxed);
}

extern "C" __declspec(dllexport) uint32_t FakeTemporalCalls()
{
    return gTemporalCalls.load(std::memory_order_relaxed);
}
