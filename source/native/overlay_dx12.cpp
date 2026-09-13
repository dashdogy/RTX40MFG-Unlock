#include "overlay_dx12.h"
#include "overlay_build.h"
#include "overlay_install.h"
#include "overlay_platform.h"
#include "overlay_native.h"
#include "overlay_color.h"
#include "overlay_device_identity.h"
#include "present_counter.h"
#include "ui_input_coherence.h"
#include <backends/imgui_impl_dx12.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <memory>
#include <vector>

namespace single_overlay::dx12
{
using Microsoft::WRL::ComPtr;
namespace
{
constexpr UINT kSrvCount = 256;
std::atomic<uint64_t> gNextTelemetryOwner{1};
enum class RenderOutcome
{
    eNoWrite,
    eSubmitted,
    eSkippedPartialUpdate,
    eSkippedPartialUpdateWithResidual
};

struct Frame
{
    ComPtr<ID3D12Resource> exposedBuffer; // Retain the resource returned by this swapchain layer.
    ComPtr<ID3D12Resource> buffer;
    ComPtr<ID3D12CommandAllocator> allocator;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    UINT64 fenceValue = 0;
};

struct State;
thread_local const State* gRenderingState = nullptr;
struct RenderScope
{
    const State* previous;
    explicit RenderScope(const State* state) noexcept : previous(gRenderingState)
    {
        gRenderingState = state;
    }
    ~RenderScope() { gRenderingState = previous; }
};

struct State
{
    std::recursive_mutex mutex;
    uint64_t telemetryOwner = gNextTelemetryOwner.fetch_add(1, std::memory_order_relaxed);
    IDXGISwapChain3* swapchain = nullptr; // Alias owned by the enclosing Session.
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12DescriptorHeap> rtvs;
    ComPtr<ID3D12DescriptorHeap> srvs;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Fence> fence;
    HANDLE fenceEvent = nullptr;
    std::vector<Frame> frames;
    std::array<bool, kSrvCount> srvUsed{};
    UINT srvStep = 0;
    UINT64 nextFence = 1;
    PlatformState platform;
    HWND presentationWindow = nullptr;
    std::atomic<uint64_t> presentationArea{0};
    DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    bool colorSpaceObserved = false;
    bool initialized = false;
    bool disabled = false;
    bool rendererInitialized = false;
    bool overlayPixelsOutstanding = false;
    bool retired = false;

    void Disable()
    {
        disabled = true;
        platform.RetireInput();
    }

    bool Wait(UINT64 value)
    {
        if (!fence || value == 0 || fence->GetCompletedValue() >= value) return true;
        if (FAILED(device->GetDeviceRemovedReason())) return true;
        if (FAILED(fence->SetEventOnCompletion(value, fenceEvent))) return false;
        return WaitForSingleObject(fenceEvent, 3000) == WAIT_OBJECT_0;
    }

    bool Drain()
    {
        for (const auto& frame : frames) if (!Wait(frame.fenceValue)) return false;
        return true;
    }

    bool ReleaseBuffers()
    {
        InternalScope internal;
        if (!Drain())
        {
            Disable();
            RecordFailure(L"D3D12 UI drain timed out; live resources retained");
            return false;
        }
        for (auto& frame : frames)
        {
            frame.buffer.Reset();
            frame.exposedBuffer.Reset();
            frame.fenceValue = 0;
        }
        return true;
    }

    bool AcquireBuffers()
    {
        if (frames.empty() || !swapchain) return false;
        if (frames[0].buffer) return true;
        // Acquire the whole generation before publishing its descriptors. A
        // failed GetBuffer must not leave a partly reacquired generation.
        DXGI_SWAP_CHAIN_DESC1 desc{};
        ComPtr<IUnknown> expectedDevice;
        if (FAILED(swapchain->GetDesc1(&desc)) || desc.BufferCount != frames.size()
            || !DeviceIdentity(device.Get(), expectedDevice)) return false;
        std::vector<ComPtr<ID3D12Resource>> buffers(frames.size()), exposed(frames.size());
        for (UINT index = 0; index < buffers.size(); ++index)
        {
            ComPtr<ID3D12Device> resourceDevice;
            ComPtr<IUnknown> identity;
            if (FAILED(swapchain->GetBuffer(index, IID_PPV_ARGS(&exposed[index])))
                || !native::Unwrap(exposed[index].Get(), buffers[index])
                || !native::PinInterface(buffers[index].Get(), 14)
                || FAILED(buffers[index]->GetDevice(IID_PPV_ARGS(&resourceDevice)))
                || !DeviceIdentity(resourceDevice.Get(), identity)
                || identity != expectedDevice) return false;
            const auto resource = buffers[index]->GetDesc();
            if (resource.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D
                || resource.Width != desc.Width || resource.Height != desc.Height
                || resource.Format != desc.Format || resource.DepthOrArraySize != 1
                || resource.MipLevels != 1 || resource.SampleDesc.Count != 1
                || !(resource.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)) return false;
            for (UINT previous = 0; previous < index; ++previous)
                if (buffers[previous] == buffers[index]) return false;
        }
        for (UINT index = 0; index < buffers.size(); ++index)
        {
            frames[index].exposedBuffer = std::move(exposed[index]);
            frames[index].buffer = std::move(buffers[index]);
            device->CreateRenderTargetView(frames[index].buffer.Get(), nullptr, frames[index].rtv);
        }
        return true;
    }

    static void AllocateSrv(ImGui_ImplDX12_InitInfo* info,
        D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
    {
        auto& state = *static_cast<State*>(info->UserData);
        *cpu = {}; *gpu = {};
        for (UINT index = 0; index < kSrvCount; ++index)
        {
            if (state.srvUsed[index]) continue;
            state.srvUsed[index] = true;
            *cpu = state.srvs->GetCPUDescriptorHandleForHeapStart();
            *gpu = state.srvs->GetGPUDescriptorHandleForHeapStart();
            cpu->ptr += static_cast<SIZE_T>(index) * state.srvStep;
            gpu->ptr += static_cast<UINT64>(index) * state.srvStep;
            return;
        }
        state.Disable();
        RecordFailure(L"D3D12 UI descriptor pool exhausted");
    }

    static void FreeSrv(ImGui_ImplDX12_InitInfo* info,
        D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE)
    {
        auto& state = *static_cast<State*>(info->UserData);
        const auto base = state.srvs->GetCPUDescriptorHandleForHeapStart().ptr;
        if (cpu.ptr >= base && state.srvStep && (cpu.ptr-base) % state.srvStep == 0)
        {
            const auto index = (cpu.ptr-base) / state.srvStep;
            if (index < kSrvCount) state.srvUsed[index] = false;
        }
    }

    bool Initialize()
    {
        if (initialized) return true;
        DXGI_SWAP_CHAIN_DESC1 desc{};
        HWND window = nullptr;
        if (!swapchain || FAILED(swapchain->GetDesc1(&desc))
            || FAILED(swapchain->GetHwnd(&window)) || !window
            || desc.BufferCount < 2 || desc.BufferCount > 16
            || desc.Stereo || desc.SampleDesc.Count != 1
            || (desc.SwapEffect != DXGI_SWAP_EFFECT_FLIP_DISCARD
                && desc.SwapEffect != DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL)
            || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT
            || device->GetNodeCount() != 1) return false;
        if (!colorSpaceObserved && desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT)
            colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
        if (colorSpace != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709
            && colorSpace != DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
            && colorSpace != DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) return false;
        if (!platform.Initialize(window)) return false;
        ContextScope current(platform.context);
        InternalScope internal;
        frames.resize(desc.BufferCount);

        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
        rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvDesc.NumDescriptors = desc.BufferCount;
        if (FAILED(device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvs)))) return false;
        const UINT rtvStep = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
        for (UINT index = 0; index < desc.BufferCount; ++index)
        {
            auto& frame = frames[index];
            if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                    IID_PPV_ARGS(&frame.allocator)))) return false;
            frame.rtv = rtv;
            rtv.ptr += rtvStep;
        }
        if (!AcquireBuffers()) return false;
        D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
        srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvDesc.NumDescriptors = kSrvCount;
        srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&srvs)))
            || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))
            || FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                frames[0].allocator.Get(), nullptr, IID_PPV_ARGS(&commands)))) return false;
        if (FAILED(commands->Close())) return false;
        fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!fenceEvent) return false;
        srvStep = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        ImGui_ImplDX12_InitInfo info{};
        info.Device = device.Get();
        info.CommandQueue = queue.Get();
        info.NumFramesInFlight = static_cast<int>(desc.BufferCount);
        info.RTVFormat = desc.Format;
        info.SrvDescriptorHeap = srvs.Get();
        info.UserData = this;
        info.SrvDescriptorAllocFn = &AllocateSrv;
        info.SrvDescriptorFreeFn = &FreeSrv;
        rendererInitialized = ImGui_ImplDX12_Init(&info);
        if (!rendererInitialized) return false;
        initialized = true;
        return true;
    }

    bool Shutdown(bool destroyPlatform)
    {
        InternalScope releasingResources;
        if (!ReleaseBuffers()) return false;
        std::lock_guard uiLock(gUiMutex);
        if (platform.context)
        {
            ContextScope current(platform.context);
            InternalScope internal;
            if (rendererInitialized) ImGui_ImplDX12_Shutdown();
        }
        rendererInitialized = false;
        commands.Reset();
        frames.clear();
        rtvs.Reset();
        srvs.Reset();
        fence.Reset();
        if (fenceEvent) CloseHandle(fenceEvent);
        fenceEvent = nullptr;
        srvUsed.fill(false);
        nextFence = 1;
        initialized = false;
        overlayPixelsOutstanding = false;
        if (destroyPlatform) platform.Shutdown();
        return true;
    }

    RenderOutcome Render(bool partialUpdate = false)
    {
        std::lock_guard stateLock(mutex);
        RenderScope rendering(this);
        if (retired) return RenderOutcome::eNoWrite;
        if (partialUpdate)
        {
            // Dirty/scroll presentation reconstructs unspecified pixels from
            // prior frames. Without a retained clean background for every
            // backbuffer, drawing or widening damage would corrupt that
            // contract. Never submit overlay work for this call. A prior UI
            // write remains explicitly outstanding until lifecycle teardown;
            // it is not reclassified as reconstructed application content.
            return overlayPixelsOutstanding
                ? RenderOutcome::eSkippedPartialUpdateWithResidual
                : RenderOutcome::eSkippedPartialUpdate;
        }
        if (disabled) return RenderOutcome::eNoWrite;
        std::lock_guard uiLock(gUiMutex);
        InternalScope internal;
        if (!Initialize())
        {
            Disable();
            RecordFailure(L"D3D12 UI initialization rejected for this swapchain");
            Shutdown(true);
            return RenderOutcome::eNoWrite;
        }
        if (!platform.WantsFrame()) return RenderOutcome::eNoWrite;
        if (!AcquireBuffers())
        {
            Disable();
            RecordFailure(L"D3D12 UI backbuffer reacquisition failed");
            Shutdown(true);
            return RenderOutcome::eNoWrite;
        }
        const UINT index = swapchain->GetCurrentBackBufferIndex();
        if (index >= frames.size()) { Disable(); return RenderOutcome::eNoWrite; }
        auto& frame = frames[index];
        if (!Wait(frame.fenceValue))
        {
            Disable();
            RecordFailure(L"D3D12 UI frame fence timed out");
            return RenderOutcome::eNoWrite;
        }
        ContextScope current(platform.context);
        ImGui_ImplDX12_SetOutputColorSpace(colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 ? 1
            : colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ? 2 : 0);
        ImGui_ImplDX12_NewFrame();
        if (!ImGui_ImplDX12_IsReady())
        {
            Disable();
            RecordFailure(L"D3D12 UI pipeline initialization failed");
            return RenderOutcome::eNoWrite;
        }
        const auto output = frame.buffer->GetDesc();
        if (!platform.NewFrame(static_cast<uint32_t>(output.Width), output.Height))
            return RenderOutcome::eNoWrite;
        platform.Draw();
        ImDrawData* drawData = ImGui::GetDrawData();
        if (!drawData || drawData->CmdListsCount == 0 || drawData->TotalVtxCount == 0)
            return RenderOutcome::eNoWrite;
        if (FAILED(frame.allocator->Reset())
            || FAILED(commands->Reset(frame.allocator.Get(), nullptr)))
        {
            Disable();
            RecordFailure(L"D3D12 UI command reset failed");
            return RenderOutcome::eNoWrite;
        }
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = frame.buffer.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commands->ResourceBarrier(1, &barrier);
        commands->OMSetRenderTargets(1, &frame.rtv, FALSE, nullptr);
        ID3D12DescriptorHeap* heap = srvs.Get();
        commands->SetDescriptorHeaps(1, &heap);
        ImGui_ImplDX12_RenderDrawData(drawData, commands.Get());
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        commands->ResourceBarrier(1, &barrier);
        if (FAILED(commands->Close())) { Disable(); return RenderOutcome::eNoWrite; }
        ID3D12CommandList* list = commands.Get();
        queue->ExecuteCommandLists(1, &list);
        // ExecuteCommandLists has no result. From this point pixel writes may
        // have reached the queue even if the following fence signal fails.
        overlayPixelsOutstanding = true;
        const UINT64 value = nextFence++;
        // Once submitted, retain every referenced resource until this exact
        // queue's fence completes. An unsuccessful Signal poisons the state.
        frame.fenceValue = value;
        if (FAILED(queue->Signal(fence.Get(), value)))
        {
            Disable();
            RecordFailure(L"D3D12 UI fence signal failed");
            return RenderOutcome::eSubmitted;
        }
        gRenderedFrames.fetch_add(1, std::memory_order_relaxed);
        return RenderOutcome::eSubmitted;
    }
};


} // namespace

struct Session : State
{
    ComPtr<IDXGISwapChain3> renderingChain;
    bool resizing = false;
    bool wasStillDrawing = false;
    uint32_t presents = 0;
};
namespace {
std::mutex gSessionsMutex;
std::array<Session*, 64> gSessions{};
bool Principal(Session* session) noexcept
{
    const HWND foreground = GetForegroundWindow();
    auto score = [foreground](Session* s) {
        return s->presentationArea.load(std::memory_order_relaxed)
            + (s->presentationWindow == foreground ? (1ull << 60) : 0);
    };
    std::lock_guard lock(gSessionsMutex);
    for (auto* other : gSessions)
        if (other && other != session && score(other) > score(session)) return false;
    return true;
}
}

Session* CreateSession(IDXGISwapChain* chain, IUnknown* presentedQueue) noexcept
{
    if (!chain || !presentedQueue || native::InsideLoader()) return nullptr;
    InternalScope internal;
    try {
        ComPtr<IDXGISwapChain3> nativeChain, renderingChain;
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12Device> device, swapDevice, nativeDevice;
        ComPtr<IUnknown> queueIdentity, swapIdentity;
        if (FAILED(chain->QueryInterface(IID_PPV_ARGS(&renderingChain)))
            || !native::PinInterface(renderingChain.Get(), 39)
            || !native::Unwrap(chain, nativeChain) || !native::SystemDxgiObject(nativeChain.Get(), 39)
            || !native::Unwrap(presentedQueue, queue)
            || !native::PinInterface(queue.Get(), 18)
            || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT
            || FAILED(queue->GetDevice(IID_PPV_ARGS(&device)))
            || !native::Unwrap(device.Get(), nativeDevice)
            || !native::PinInterface(nativeDevice.Get(), 43)
            || FAILED(renderingChain->GetDevice(IID_PPV_ARGS(&swapDevice)))
            || !DeviceIdentity(nativeDevice.Get(), queueIdentity)
            || !DeviceIdentity(swapDevice.Get(), swapIdentity)
            || queueIdentity.Get() != swapIdentity.Get()) {
            RecordFailure(L"MFG_PROXY_UI renderer unavailable: native swapchain/queue/device ownership not established");
            return nullptr;
        }
        const LUID a = nativeDevice->GetAdapterLuid(), b = swapDevice->GetAdapterLuid();
        if (a.HighPart != b.HighPart || a.LowPart != b.LowPart) return nullptr;
        auto session = std::make_unique<Session>();
        DXGI_SWAP_CHAIN_DESC1 desc{};
        if (FAILED(renderingChain->GetHwnd(&session->presentationWindow)) || !session->presentationWindow
            || FAILED(renderingChain->GetDesc1(&desc))) return nullptr;
        // Keep the returned swapchain's semantics paired with its creation
        // queue. Streamline may expose off-screen application buffers, a
        // different buffer count/index, and another native presentation queue.
        // The native chain above proves the base's identity; it is never used
        // to obtain rendering buffers or bypass this layer's frame indexing.
        session->renderingChain = renderingChain;
        session->swapchain = renderingChain.Get();
        session->queue = queue;
        session->device = nativeDevice;
        session->presentationArea.store(uint64_t(desc.Width) * desc.Height);
        {
            std::lock_guard lock(gSessionsMutex);
            auto slot = std::find(gSessions.begin(), gSessions.end(), nullptr);
            if (slot == gSessions.end()) return nullptr;
            *slot = session.get();
        }
        single_module::Log(L"MFG_PROXY_UI renderer registered layer=creation-return buffers=layer-local index=layer-local queue=creation-argument device=matched");
        return session.release();
    } catch (...) {
        RecordFailure(L"MFG_PROXY_UI renderer allocation failed; original presentation retained");
        return nullptr;
    }
}

void DestroySession(Session* session) noexcept
{
    if (!session) return;
    {
        std::lock_guard lock(gSessionsMutex);
        for (auto& slot : gSessions) if (slot == session) slot = nullptr;
    }
    session->retired = true;
    session->platform.RetireInput();
    // A wrapper holds a method lease across every forwarded callback. Its
    // final release proves no other method can still access this session.
    // A failed GPU drain retains the entire generation, including the native
    // chain, queue and allocator, for process lifetime without new allocation.
    if (!session->Shutdown(true)) {
        RecordFailure(L"MFG_PROXY_UI retiring generation retained after GPU drain failure");
        return;
    }
    delete session;
}

bool BeginPresent(Session* session, UINT flags, bool partialUpdate) noexcept
{
    if (!session || gInsideOverlay) return false;
    std::lock_guard lock(session->mutex);
    if (session->resizing || session->retired) return false;
    ++session->presents;
    if (session->presents != 1) return true; // Concurrent calls forward; only one UI submission can own a frame.
    if ((flags & DXGI_PRESENT_TEST) || ((flags & DXGI_PRESENT_DO_NOT_WAIT) && session->wasStillDrawing)) return true;
    if (!Principal(session)) return true;
    gDxgiFrames.fetch_add(1, std::memory_order_relaxed);
#if MFG_UNLOCK_OVERLAY_MENU_DRAW && !MFG_UNLOCK_OVERLAY_SKIP_GPU_WORK
    if (!install::InputReady()) return true; // A concurrent creator never waits on input publication.
    try { session->Render(partialUpdate); }
    catch (...) { session->Disable(); RecordFailure(L"MFG_PROXY_UI render exception; UI disabled for this generation"); }
#endif
    return true;
}
void EndPresent(Session* session, HRESULT result, UINT flags) noexcept
{
    if (!session) return;
    std::lock_guard lock(session->mutex);
    if (result==S_OK && !(flags & DXGI_PRESENT_TEST) && Principal(session)) {
        InternalScope internal;
        UINT count=0;
        const bool available=SUCCEEDED(session->swapchain->GetLastPresentCount(&count));
        frame_telemetry::SamplePresentCounter(session->telemetryOwner,count,available);
    }
    if (!(flags & DXGI_PRESENT_TEST)) session->wasStillDrawing = result == DXGI_ERROR_WAS_STILL_DRAWING;
    if (session->presents) --session->presents;
}
bool BeginResize(Session* session) noexcept
{
    if (!session) return true;
    std::lock_guard lock(session->mutex);
    if (session->resizing || session->presents) return false;
    session->resizing = true;
    ui_input_coherence::InvalidateResources();
    if (!session->Shutdown(false)) { session->resizing = false; return false; }
    return true;
}
void EndResize(Session* session, HRESULT result) noexcept
{
    if (!session) return;
    std::lock_guard lock(session->mutex);
    DXGI_SWAP_CHAIN_DESC1 desc{};
    if (SUCCEEDED(result) && SUCCEEDED(session->swapchain->GetDesc1(&desc)))
        session->presentationArea.store(uint64_t(desc.Width) * desc.Height);
    session->telemetryOwner=gNextTelemetryOwner.fetch_add(1,std::memory_order_relaxed);
    session->resizing = false;
    session->wasStillDrawing = false;
}
void ObserveColorSpace(Session* session, DXGI_COLOR_SPACE_TYPE color) noexcept
{
    if (!session) return;
    std::lock_guard lock(session->mutex);
    session->colorSpace = color;
    session->colorSpaceObserved = true;
}
void VerifyResizeQueues(Session* session, UINT count, IUnknown* const* queues) noexcept
{
    if (!session || !queues) return;
    std::lock_guard lock(session->mutex);
    DXGI_SWAP_CHAIN_DESC1 desc{};
    if (FAILED(session->swapchain->GetDesc1(&desc))) { session->Disable(); return; }
    if (!count) count = desc.BufferCount;
    if (count == 0 || count > 16 || count != desc.BufferCount) { session->Disable(); return; }
    for (UINT i = 0; i < count; ++i) {
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<IUnknown> a, b;
        if (!native::Unwrap(queues[i], queue) || FAILED(queue.As(&a))
            || FAILED(session->queue.As(&b)) || a.Get() != b.Get()) {
            session->Disable();
            RecordFailure(L"MFG_PROXY_UI ResizeBuffers1 changed presentation queue; rendering disabled until recreation");
            return;
        }
    }
}
} // namespace single_overlay::dx12
