#include <Windows.h>
#include <d3d12.h>
#include <sl.h>
#include <sl_dlss_g.h>
#include <nvsdk_ngx.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

extern "C" void WrapperSignature();

using NgxD3D12EvaluateFeatureFn = NVSDK_NGX_Result (NVSDK_CONV*)(
    ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
    const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);

namespace
{
std::atomic<uint32_t> gActualMultiplier{1};
std::atomic<bool> gTransientNotInitializedReturned{false};
std::atomic<bool> gUiRecompositionEnabled{false};
std::atomic<uint32_t> gOptionsFlags{0};
std::atomic<uint32_t> gOffSetCalls{0};
std::atomic<uint32_t> gRecycleOrderViolations{0};
std::atomic<uint64_t> gStateFenceValue{0};
std::once_flag gStateFenceOnce;
ID3D12Device* gStateFenceDevice = nullptr;
ID3D12Fence* gStateFence = nullptr;

class FakeParameters final : public NVSDK_NGX_Parameter
{
public:
    int count = 1;
    int index = 1;
    int countMax = 0;
    int mustCallEval = 0;
    ID3D12Resource* disableInterpolation = reinterpret_cast<ID3D12Resource*>(
        static_cast<uintptr_t>(0x3000));
    int notRenderingGameFrames = 0;
    int streamlineMode = 0;
    int reset = 0;
    int evalFlags = 0;
    unsigned long long frameId = 0;

    void Set(const char* name, unsigned long long value) override
    {
        if (strcmp(name, "DLSSG.BackbufferFrameID") == 0)
            frameId = value;
    }
    void Set(const char*, float) override {}
    void Set(const char*, double) override {}
    void Set(const char*, unsigned int) override {}
    void Set(const char* name, int value) override
    {
        if (strcmp(name, "DLSSG.MultiFrameCount") == 0)
            count = value;
        else if (strcmp(name, "DLSSG.MultiFrameIndex") == 0)
            index = value;
        else if (strcmp(name, "DLSSG.MultiFrameCountMax") == 0)
            countMax = value;
        else if (strcmp(name, "DLSSG.MustCallEval") == 0)
            mustCallEval = value;
        else if (strcmp(name, "DLSSG.OutputDisableInterpolation") == 0)
            disableInterpolation = nullptr;
        else if (strcmp(name, "DLSSG.NotRenderingGameFrames") == 0)
            notRenderingGameFrames = value;
        else if (strcmp(name, "DLSSG.StreamlineMode") == 0)
            streamlineMode = value;
        else if (strcmp(name, "DLSSG.Reset") == 0)
            reset = 9;
        else if (strcmp(name, "DLSSG.EvalFlags") == 0)
            evalFlags = value;
    }
    void Set(const char*, ID3D11Resource*) override {}
    void Set(const char*, ID3D12Resource*) override {}
    void Set(const char*, void*) override {}
    NVSDK_NGX_Result Get(const char* name, unsigned long long* value) const override
    {
        if (strcmp(name, "DLSSG.BackbufferFrameID") == 0)
        {
            *value = frameId;
            return NVSDK_NGX_Result_Success;
        }
        return NVSDK_NGX_Result_FAIL_UnsupportedParameter;
    }
    NVSDK_NGX_Result Get(const char*, float*) const override
        { return NVSDK_NGX_Result_FAIL_UnsupportedParameter; }
    NVSDK_NGX_Result Get(const char*, double*) const override
        { return NVSDK_NGX_Result_FAIL_UnsupportedParameter; }
    NVSDK_NGX_Result Get(const char*, unsigned int*) const override
        { return NVSDK_NGX_Result_FAIL_UnsupportedParameter; }
    NVSDK_NGX_Result Get(const char* name, int* value) const override
    {
        if (strcmp(name, "DLSSG.MultiFrameCount") == 0)
            *value = count;
        else if (strcmp(name, "DLSSG.MultiFrameIndex") == 0)
            *value = index;
        else if (strcmp(name, "DLSSG.MultiFrameCountMax") == 0)
            *value = countMax;
        else if (strcmp(name, "DLSSG.MustCallEval") == 0)
            *value = mustCallEval;
        else if (strcmp(name, "DLSSG.NotRenderingGameFrames") == 0)
            *value = notRenderingGameFrames;
        else if (strcmp(name, "DLSSG.StreamlineMode") == 0)
            *value = streamlineMode;
        else if (strcmp(name, "DLSSG.Reset") == 0)
            *value = reset;
        else if (strcmp(name, "DLSSG.EvalFlags") == 0)
            *value = evalFlags;
        else
            return NVSDK_NGX_Result_FAIL_UnsupportedParameter;
        return NVSDK_NGX_Result_Success;
    }
    NVSDK_NGX_Result Get(const char*, ID3D11Resource**) const override
        { return NVSDK_NGX_Result_FAIL_UnsupportedParameter; }
    NVSDK_NGX_Result Get(const char* name, ID3D12Resource** value) const override
    {
        if (strcmp(name, "DLSSG.OutputDisableInterpolation") == 0)
        {
            *value = disableInterpolation;
            return NVSDK_NGX_Result_Success;
        }
        if (strcmp(name, "DLSSG.OutputInterpolated") == 0)
        {
            *value = reinterpret_cast<ID3D12Resource*>(
                static_cast<uintptr_t>(0x1000 + index * 0x100));
            return NVSDK_NGX_Result_Success;
        }
        if (strcmp(name, "DLSSG.OutputReal") == 0)
        {
            *value = reinterpret_cast<ID3D12Resource*>(
                static_cast<uintptr_t>(0x2000));
            return NVSDK_NGX_Result_Success;
        }
        return NVSDK_NGX_Result_FAIL_UnsupportedParameter;
    }
    NVSDK_NGX_Result Get(const char*, void**) const override
        { return NVSDK_NGX_Result_FAIL_UnsupportedParameter; }
    void Reset() override {}
};

uint32_t MaximumGeneratedFrames()
{
    const auto* signature = reinterpret_cast<const uint8_t*>(&WrapperSignature);
    return signature[0] == 0xBA ? signature[1] : 0;
}

bool EnvironmentEnabled(const wchar_t* name)
{
    wchar_t value[8]{};
    const DWORD length = GetEnvironmentVariableW(name, value, _countof(value));
    return length > 0 && length < _countof(value) && value[0] != L'0';
}

std::wstring NgxPath()
{
    wchar_t path[32768]{};
    const DWORD length = GetEnvironmentVariableW(
        L"MFG_HARNESS_NGX_PATH", path, _countof(path));
    return length > 0 && length < _countof(path)
        ? std::wstring(path, length) : L"nvngx_dlssg.dll";
}

uint32_t FreeResourcesCalls()
{
    HMODULE interposer = GetModuleHandleW(L"sl.interposer.dll");
    if (!interposer)
        return 0;
    using CallsFn = uint32_t();
    auto* calls = reinterpret_cast<CallsFn*>(
        GetProcAddress(interposer, "FakeFreeResourcesCalls"));
    return calls ? calls() : 0;
}

void EnsureStateFence()
{
    std::call_once(gStateFenceOnce, [] {
        if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                IID_PPV_ARGS(&gStateFenceDevice))))
            return;
        // These harness-only objects intentionally live until process exit.
        // Cross-DLL static COM destruction races with the injected ASI worker
        // and is unrelated to the lifecycle behavior being tested.
        gStateFenceDevice->CreateFence(
            0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gStateFence));
    });
}
}

extern "C" __declspec(dllexport) void* slGetPluginFunction(const char*)
{
    return nullptr;
}

extern "C" __declspec(dllexport) sl::Result FakeSetOptions(
    const sl::ViewportHandle&, const sl::DLSSGOptions& options)
{
    gUiRecompositionEnabled.store(options.structVersion >= sl::kStructVersion4
        && options.enableUserInterfaceRecomposition == sl::Boolean::eTrue,
        std::memory_order_relaxed);
    gOptionsFlags.store(static_cast<uint32_t>(options.flags), std::memory_order_relaxed);
    const std::wstring ngxPath = NgxPath();
    if (!GetModuleHandleW(ngxPath.c_str())
        && !LoadLibraryW(ngxPath.c_str()))
        return sl::Result::eErrorMissingOrInvalidAPI;
    if (EnvironmentEnabled(L"MFG_HARNESS_TRANSIENT_21")
        && !gTransientNotInitializedReturned.exchange(true, std::memory_order_relaxed))
        return sl::Result::eErrorNotInitialized;
    if (options.mode == sl::DLSSGMode::eDynamic
        && options.structVersion < sl::kStructVersion5)
        return sl::Result::eErrorInvalidParameter;
    if (options.mode == sl::DLSSGMode::eOff)
    {
        gOffSetCalls.fetch_add(1, std::memory_order_relaxed);
        gActualMultiplier.store(0, std::memory_order_relaxed);
    }
    else if (options.mode == sl::DLSSGMode::eDynamic)
    {
        gActualMultiplier.store(MaximumGeneratedFrames() + 1, std::memory_order_relaxed);
    }
    else
    {
        gActualMultiplier.store(options.numFramesToGenerate + 1, std::memory_order_relaxed);
    }
    return options.mode == sl::DLSSGMode::eOn && options.numFramesToGenerate == 2
        ? sl::Result::eWarnOutOfVRAM : sl::Result::eOk;
}

extern "C" __declspec(dllexport) uint32_t FakeUiRecompositionEnabled()
{
    return gUiRecompositionEnabled.load(std::memory_order_relaxed) ? 1u : 0u;
}

extern "C" __declspec(dllexport) uint32_t FakeOptionsFlags()
{
    return gOptionsFlags.load(std::memory_order_relaxed);
}

extern "C" __declspec(dllexport) uint32_t FakeActualMultiplier()
{
    return gActualMultiplier.load(std::memory_order_relaxed);
}

extern "C" __declspec(dllexport) uint32_t FakeOffSetCalls()
{
    return gOffSetCalls.load(std::memory_order_relaxed);
}

extern "C" __declspec(dllexport) uint32_t FakeRecycleOrderViolations()
{
    return gRecycleOrderViolations.load(std::memory_order_relaxed);
}

extern "C" __declspec(dllexport) uint32_t FakeRunTemporalSequence(
    uint32_t evaluations)
{
    HMODULE ngx = GetModuleHandleW(NgxPath().c_str());
    if (!ngx)
        return 0;
    auto* evaluate = reinterpret_cast<NgxD3D12EvaluateFeatureFn>(
        GetProcAddress(ngx, "NVSDK_NGX_D3D12_EvaluateFeature"));
    if (!evaluate)
        return 0;

    FakeParameters parameters;
    uint32_t completed = 0;
    for (; completed < evaluations; ++completed)
    {
        parameters.count = static_cast<int>(evaluations);
        parameters.index = static_cast<int>(completed + 1);
        if (evaluate(nullptr, nullptr, &parameters, nullptr) != NVSDK_NGX_Result_Success)
            break;
    }
    return completed;
}

extern "C" __declspec(dllexport) uint32_t FakeRunInterleavedTemporalSequences()
{
    HMODULE ngx = GetModuleHandleW(NgxPath().c_str());
    if (!ngx)
        return 0;
    auto* evaluate = reinterpret_cast<NgxD3D12EvaluateFeatureFn>(
        GetProcAddress(ngx, "NVSDK_NGX_D3D12_EvaluateFeature"));
    if (!evaluate)
        return 0;

    FakeParameters parameters[2];
    const auto handles = std::array{
        reinterpret_cast<const NVSDK_NGX_Handle*>(static_cast<uintptr_t>(1)),
        reinterpret_cast<const NVSDK_NGX_Handle*>(static_cast<uintptr_t>(2))};
    uint32_t completed = 0;
    for (int index = 5; index >= 1; --index)
    {
        for (uint32_t handle = 0; handle < handles.size(); ++handle)
        {
            parameters[handle].count = 5;
            parameters[handle].index = index;
            if (evaluate(nullptr, handles[handle], &parameters[handle], nullptr)
                != NVSDK_NGX_Result_Success)
                return completed;
            ++completed;
        }
    }
    return completed;
}

extern "C" __declspec(dllexport) sl::Result FakeGetState(
    const sl::ViewportHandle&, sl::DLSSGState& state, const sl::DLSSGOptions*)
{
    state.status = sl::DLSSGStatus::eOk;
    state.numFramesActuallyPresented = gActualMultiplier.load(std::memory_order_relaxed);
    if (state.structVersion >= sl::kStructVersion2)
        state.numFramesToGenerateMax = MaximumGeneratedFrames();
    if (state.structVersion >= sl::kStructVersion4)
        state.bIsDynamicMFGSupported = sl::Boolean::eTrue;
    if (state.structVersion >= sl::kStructVersion3)
    {
        EnsureStateFence();
        if (gStateFence)
        {
            const uint64_t value =
                gStateFenceValue.fetch_add(1, std::memory_order_relaxed) + 1;
            gStateFence->Signal(value);
            state.inputsProcessingCompletionFence = gStateFence;
            state.lastPresentInputsProcessingCompletionFenceValue = value;
        }
    }
    return sl::Result::eOk;
}
