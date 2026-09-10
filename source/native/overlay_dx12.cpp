#include "overlay_dx12.h"
#include "overlay_platform.h"
#include "overlay_hook.h"
#include "overlay_color.h"
#include "overlay_device_identity.h"
#include "present_counter.h"
#include "vsync_control.h"
#include "ui_input_coherence.h"
#include <backends/imgui_impl_dx12.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <memory>
#include <unordered_map>
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
    std::mutex mutex;
    uint64_t telemetryOwner = gNextTelemetryOwner.fetch_add(1, std::memory_order_relaxed);
    IDXGISwapChain3* swapchain = nullptr; // Weak: never extend the game's swapchain lifetime.
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    HMODULE creatorOwner = nullptr;
    void* applicationPresentEntry = nullptr;
    void* applicationPresent1Entry = nullptr;
    vsync_control::Chain vsync;
    uint64_t vsyncRevision = 1;
    bool vsyncRefresh = true;
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
    bool releaseInProgress = false;
    bool releaseInvalidated = false;
    bool retired = false;
    bool retirementFinalized = false;
    uint32_t activeCallbacks = 0;
    uint32_t activeReleases = 0;
    std::shared_ptr<State> pendingReplacement;

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
        std::vector<ComPtr<ID3D12Resource>> buffers(frames.size());
        for (UINT index = 0; index < buffers.size(); ++index)
            if (FAILED(swapchain->GetBuffer(index, IID_PPV_ARGS(&buffers[index])))) return false;
        for (UINT index = 0; index < buffers.size(); ++index)
        {
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
        if (retired || releaseInProgress) return RenderOutcome::eNoWrite;
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

std::mutex gStatesMutex;
std::unordered_map<void*, std::shared_ptr<State>> gStates;
// A timed-out queue cannot safely surrender its allocator or buffers. Retain
// that exceptional generation for process lifetime, including on Release.
std::vector<std::shared_ptr<State>> gRetained;
std::atomic<HMODULE> gSystemDxgi{nullptr};
thread_local uint32_t gPresentDepth = 0;
struct PresentScope;
thread_local PresentScope* gActivePresent = nullptr;
struct NativeReleaseScope;
thread_local NativeReleaseScope* gActiveNativeRelease = nullptr;

struct PresentScope
{
    void* object;
    const State* state = nullptr;
    PresentScope* previous;
    bool outer = true;
    PresentScope(void* value, const State* generation) noexcept
        : object(value), state(generation), previous(gActivePresent)
    {
        for (auto* active = previous; active; active = active->previous)
            if (active->object == object && active->state == state)
            {
                outer = false;
                break;
            }
        gActivePresent = this;
        ++gPresentDepth;
    }
    ~PresentScope()
    {
        gActivePresent = previous;
        --gPresentDepth;
    }
};

bool PresentActiveFor(void* object, const State* state) noexcept
{
    for (auto* present = gActivePresent; present; present = present->previous)
        if (present->object == object && present->state == state) return true;
    return false;
}

struct NativeReleaseScope
{
    IUnknown* object;
    const State* state;
    NativeReleaseScope* previous;
    NativeReleaseScope(IUnknown* value, const State* generation) noexcept
        : object(value), state(generation), previous(gActiveNativeRelease)
    {
        gActiveNativeRelease = this;
    }
    ~NativeReleaseScope() { gActiveNativeRelease = previous; }
};

bool SuppressCreateWhileInternal() noexcept
{
    // An application callback reached from the exact reserved native Release
    // may create an unrelated chain. Let that creator reach AttachSwapchain;
    // other overlay-owned recursive creation remains suppressed.
    return gInsideOverlay && !gActiveNativeRelease;
}

std::shared_ptr<State> Find(void* swapchain)
{
    std::lock_guard lock(gStatesMutex);
    const auto found = gStates.find(swapchain);
    return found == gStates.end() ? nullptr : found->second;
}

bool RegisterAttachedState(void* object, const std::shared_ptr<State>& candidate)
{
    if (!object || !candidate) return false;
    for (;;)
    {
        std::shared_ptr<State> predecessor;
        {
            std::lock_guard lock(gStatesMutex);
            const auto found = gStates.find(object);
            if (found == gStates.end())
            {
                if (gStates.size() >= 64) return false;
                gStates.emplace(object, candidate);
                return true;
            }
            if (found->second == candidate) return true;
            predecessor = found->second;
        }

        // A zero-returning Release can destroy an object and synchronously
        // create another one at the same address before the old hook regains
        // control. Queue that exact new generation behind the in-flight
        // predecessor; never let it inherit the predecessor's state.
        std::lock_guard predecessorLock(predecessor->mutex);
        std::lock_guard registryLock(gStatesMutex);
        const auto current = gStates.find(object);
        if (current == gStates.end()) continue;
        if (current->second != predecessor) continue;
        if (predecessor->retired)
        {
            current->second = candidate;
            return true;
        }
        if (!predecessor->releaseInProgress
            || predecessor->swapchain != object) return false;
        if (predecessor->pendingReplacement
            && predecessor->pendingReplacement != candidate) return false;
        predecessor->pendingReplacement = candidate;
        return true;
    }
}

void FinalizeRetiredState(const std::shared_ptr<State>& state)
{
    bool finalize = false;
    {
        std::lock_guard lock(state->mutex);
        if (state->retired && state->activeCallbacks == 0
            && state->activeReleases == 0
            && !state->retirementFinalized)
        {
            state->retirementFinalized = true;
            finalize = true;
        }
    }
    if (!finalize) return;

    // Retirement prevents new callbacks and the counters above prove every
    // leased Present/Release has returned. Teardown intentionally happens
    // without the state or registry lock: COM Release and platform shutdown
    // can execute arbitrary callbacks.
    const bool retain = !state->Shutdown(true);
    if (retain)
    {
        std::lock_guard lock(gStatesMutex);
        gRetained.push_back(state);
    }
}

class StateCallback
{
    std::shared_ptr<State> state_;
public:
    StateCallback(std::shared_ptr<State> state, void* object)
    {
        if (!state) return;
        std::lock_guard lock(state->mutex);
        if (state->retired || state->releaseInProgress
            || state->swapchain != object) return;
        ++state->activeCallbacks;
        state_ = std::move(state);
    }
    StateCallback(const StateCallback&) = delete;
    StateCallback& operator=(const StateCallback&) = delete;
    ~StateCallback()
    {
        if (!state_) return;
        bool finalize = false;
        {
            std::lock_guard lock(state_->mutex);
            if (state_->activeCallbacks != 0) --state_->activeCallbacks;
            finalize = state_->retired && state_->activeCallbacks == 0
                && state_->activeReleases == 0;
        }
        if (finalize) FinalizeRetiredState(state_);
    }
    const std::shared_ptr<State>& Get() const noexcept { return state_; }
};

bool IsPrincipal(IDXGISwapChain* chain, const std::shared_ptr<State>& state,
    bool requireUnique)
{
    if (!state) return false;
    const HWND foreground = GetForegroundWindow();
    auto score = [foreground](const auto& candidate) {
        return candidate->presentationArea.load(std::memory_order_relaxed)
            + (candidate->presentationWindow == foreground ? (1ull << 60) : 0ull);
    };
    std::lock_guard lock(gStatesMutex);
    const auto found = gStates.find(chain);
    if (found == gStates.end() || found->second != state) return false;
    for (const auto& [other, candidate] : gStates)
        if (other != chain && (score(candidate) > score(state)
            || (requireUnique && score(candidate) == score(state)))) return false;
    return true;
}

struct PresentPreparation
{
    bool tracked = false;
    RenderOutcome render = RenderOutcome::eNoWrite;
};

PresentPreparation BeforePresent(IDXGISwapChain* chain,
    const std::shared_ptr<State>& state, UINT flags, bool partialUpdate)
{
    PresentPreparation result{};
    if ((flags & DXGI_PRESENT_TEST) || !state) return result;
    // Only the principal game window contributes UI and presentation
    // telemetry. Auxiliary swapchains retain forwarding/lifetime hooks.
    if (!IsPrincipal(chain, state, false)) return result;
    result.tracked = true;
    gDxgiFrames.fetch_add(1, std::memory_order_relaxed);
    result.render = state->Render(partialUpdate);
    return result;
}

void AfterPresent(IDXGISwapChain* chain, const std::shared_ptr<State>& state)
{
    InternalScope internal;
    if (state)
    {
        uint64_t owner;
        {
            std::lock_guard lock(state->mutex);
            if (state->retired || state->swapchain != chain) return;
            owner = state->telemetryOwner;
        }
        UINT count = 0;
        // Streamline forwards this public method to its native swapchain.
        // One application Present may cause several native presentations.
        // A wrapper may release a temporary interface during this call. Do
        // not hold the state lock across external COM code.
        const bool available = chain->GetLastPresentCount(&count) == S_OK;
        std::lock_guard lock(state->mutex);
        if (!state->retired && state->swapchain == chain
            && state->telemetryOwner == owner)
            frame_telemetry::SamplePresentCounter(owner, count, available);
    }
}

HMODULE EntryOwner(void* entry) noexcept
{
    HMODULE owner = nullptr;
    return entry && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
        | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(entry), &owner) ? owner : nullptr;
}

template<class Family>
void* CallbackTarget(typename Family::Fn original) noexcept
{
    // The exact trampoline slot selects the entry. Another function in the
    // same interposer, or an object's changed vtable, is not interchangeable.
    std::lock_guard lock(Family::mutex);
    for (const auto& slot : Family::slots)
        if (slot.installed && slot.original.load(std::memory_order_acquire) == original)
            return slot.target;
    return nullptr;
}

void RefreshVsyncRegistration(const std::shared_ptr<State>& state, IDXGISwapChain* chain)
{
    vsync_control::Ownership ownership{};
    uint64_t revision = 0;
    ComPtr<ID3D12CommandQueue> queue;
    {
        std::lock_guard lock(state->mutex);
        if (!state->vsyncRefresh || state->disabled || state->swapchain != chain) return;
        revision = state->vsyncRevision;
        ownership.swapchain = reinterpret_cast<uintptr_t>(chain);
        ownership.creatorOwner = state->creatorOwner;
        ownership.presentEntry = state->applicationPresentEntry;
        ownership.present1Entry = state->applicationPresent1Entry;
        queue = state->queue;
    }
    InternalScope internal;
    auto** table = *reinterpret_cast<void***>(chain);
    if (table[8] == ownership.presentEntry && table[22] == ownership.present1Entry)
    {
        ownership.presentOwner = EntryOwner(table[8]);
        ownership.present1Owner = EntryOwner(table[22]);
    }
    // Streamline 2.14.1 DXGISwapChain::Present/Present1 invoke the FG before
    // hooks before forwarding to m_base. Identify that application proxy via
    // its COM contract, without reading a private object layout or unwrapping
    // and changing the native final-presentation entry.
    constexpr GUID streamlineSwapchain = {0xd3f0bbff, 0x3091, 0x4074,
        {0x9d, 0x9e, 0xb9, 0x9c, 0xe2, 0xe5, 0xcf, 0x9a}};
    ComPtr<IUnknown> proxy, queueIdentity, queueDeviceIdentity, swapDeviceIdentity;
    ComPtr<ID3D12Device> queueDevice, swapDevice;
    ownership.applicationWrapper = chain->QueryInterface(streamlineSwapchain,
            reinterpret_cast<void**>(proxy.GetAddressOf())) == S_OK
        && proxy.Get() == static_cast<IUnknown*>(chain);
    if (queue && queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT
        && SUCCEEDED(queue.As(&queueIdentity))
        && SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&queueDevice)))
        && SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&swapDevice)))
        && DeviceIdentity(queueDevice.Get(), queueDeviceIdentity)
        && DeviceIdentity(swapDevice.Get(), swapDeviceIdentity))
    {
        ownership.queueIdentity = reinterpret_cast<uintptr_t>(queueIdentity.Get());
        ownership.queueDeviceIdentity = reinterpret_cast<uintptr_t>(queueDeviceIdentity.Get());
        ownership.swapchainDeviceIdentity = reinterpret_cast<uintptr_t>(swapDeviceIdentity.Get());
        const LUID luid = queueDevice->GetAdapterLuid();
        const LUID swapLuid = swapDevice->GetAdapterLuid();
        if (luid.LowPart == swapLuid.LowPart && luid.HighPart == swapLuid.HighPart)
            memcpy(&ownership.adapterLuid, &luid, sizeof(luid));
    }
    std::lock_guard lock(state->mutex);
    if (state->vsyncRevision != revision || state->swapchain != chain
        || state->queue.Get() != queue.Get() || state->disabled) return;
    vsync_control::RegisterChain(state->vsync, ownership);
    state->vsyncRefresh = false;
}

vsync_control::Ticket PrepareVsyncPresent(const std::shared_ptr<State>& state,
    IDXGISwapChain* chain, void* callbackEntry, bool present1, bool tracked,
    bool outer, UINT sync, UINT flags)
{
    vsync_control::Ticket unchanged{};
    unchanged.originalInterval = unchanged.submittedInterval = sync;
    unchanged.originalFlags = unchanged.submittedFlags = flags;
    if (!state || !outer || (flags & DXGI_PRESENT_TEST)) return unchanged;
    const bool principal = tracked && IsPrincipal(chain, state, true);
    if (principal) RefreshVsyncRegistration(state, chain);
    std::lock_guard lock(state->mutex);
    if (state->disabled || state->swapchain != chain)
    {
        vsync_control::InvalidateChain(state->vsync, vsync_control::Failure::eDeviceIdentityLost);
        return unchanged;
    }
    auto** table = *reinterpret_cast<void***>(chain);
    return vsync_control::AdjustBeforePresent(state->vsync,
        reinterpret_cast<uintptr_t>(chain), callbackEntry, table[present1 ? 22 : 8],
        present1, principal, outer, sync, flags);
}

struct PresentTag
{
    using Fn = HRESULT (WINAPI*)(IDXGISwapChain*, UINT, UINT);
    static HRESULT Call(Fn original, IDXGISwapChain* chain, UINT sync, UINT flags)
    {
        if (gInsideOverlay) return original(chain, sync, flags);
        StateCallback callback(Find(chain), chain);
        const auto& state = callback.Get();
        PresentScope present(chain, state.get());
        const auto preparation = present.outer
            ? BeforePresent(chain, state, flags, false) : PresentPreparation{};
        const auto ticket = PrepareVsyncPresent(state, chain,
            CallbackTarget<HookFamily<PresentTag, HRESULT, IDXGISwapChain*, UINT, UINT>>(original),
            false, preparation.tracked, present.outer, sync, flags);
        const HRESULT result = original(chain, ticket.submittedInterval, ticket.submittedFlags);
        if (state) vsync_control::CompletePresent(state->vsync, ticket, result);
        if (preparation.tracked && result == S_OK)
            AfterPresent(chain, state);
        return result;
    }
};
struct Present1Tag
{
    using Fn = HRESULT (WINAPI*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
    static HRESULT Call(Fn original, IDXGISwapChain1* chain, UINT sync, UINT flags,
        const DXGI_PRESENT_PARAMETERS* parameters)
    {
        if (gInsideOverlay) return original(chain, sync, flags, parameters);
        StateCallback callback(Find(chain), chain);
        const auto& state = callback.Get();
        PresentScope present(chain, state.get());
        const bool partialUpdate = parameters
            && (parameters->DirtyRectsCount != 0 || parameters->pScrollRect
                || parameters->pScrollOffset);
        const auto preparation = present.outer
            ? BeforePresent(chain, state, flags, partialUpdate) : PresentPreparation{};
        const auto ticket = PrepareVsyncPresent(state, chain,
            CallbackTarget<HookFamily<Present1Tag, HRESULT, IDXGISwapChain1*, UINT, UINT,
                const DXGI_PRESENT_PARAMETERS*>>(original),
            true, preparation.tracked, present.outer, sync, flags);
        // Present1 metadata is the application's reconstruction contract. A
        // partial/scroll call never receives overlay writes, and all paths
        // forward the exact structure and pointees supplied by the caller.
        const HRESULT result = original(chain, ticket.submittedInterval, ticket.submittedFlags,
            parameters);
        if (state) vsync_control::CompletePresent(state->vsync, ticket, result);
        if (preparation.tracked && result == S_OK)
            AfterPresent(chain, state);
        return result;
    }
};

bool PrepareResize(void* chain)
{
    if (auto state = Find(chain))
    {
        std::lock_guard lock(state->mutex);
        if (state->retired || state->releaseInProgress
            || state->swapchain != chain) return false;
        ui_input_coherence::InvalidateResources();
        vsync_control::InvalidateChain(state->vsync);
        ++state->vsyncRevision;
        state->vsyncRefresh = true;
        state->telemetryOwner = gNextTelemetryOwner.fetch_add(1, std::memory_order_relaxed);
        return state->Shutdown(false);
    }
    return true;
}

struct ResizeTag
{
    using Fn = HRESULT (WINAPI*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
    static HRESULT Call(Fn original, IDXGISwapChain* chain, UINT count, UINT width,
        UINT height, DXGI_FORMAT format, UINT flags)
    {
        if (gInsideOverlay) return original(chain, count, width, height, format, flags);
        InternalScope internal;
        if (!PrepareResize(chain)) return DXGI_ERROR_WAS_STILL_DRAWING;
        const HRESULT result = original(chain, count, width, height, format, flags);
        if (SUCCEEDED(result)) if (auto state = Find(chain))
        {
            DXGI_SWAP_CHAIN_DESC1 desc{};
            if (SUCCEEDED(state->swapchain->GetDesc1(&desc)))
                state->presentationArea.store(uint64_t(desc.Width)*desc.Height, std::memory_order_relaxed);
        }
        return result;
    }
};
struct Resize1Tag
{
    using Fn = HRESULT (WINAPI*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT,
        const UINT*, IUnknown* const*);
    static HRESULT Call(Fn original, IDXGISwapChain3* chain, UINT count, UINT width,
        UINT height, DXGI_FORMAT format, UINT flags, const UINT* nodes, IUnknown* const* queues)
    {
        if (gInsideOverlay) return original(chain, count, width, height, format, flags, nodes, queues);
        InternalScope internal;
        if (!PrepareResize(chain)) return DXGI_ERROR_WAS_STILL_DRAWING;
        const HRESULT result = original(chain, count, width, height, format, flags, nodes, queues);
        if (SUCCEEDED(result))
        {
            if (auto state = Find(chain))
            {
                DXGI_SWAP_CHAIN_DESC1 desc{};
                ComPtr<ID3D12CommandQueue> replacement;
                ComPtr<IUnknown> deviceIdentity;
                bool proven = SUCCEEDED(chain->GetDesc1(&desc)) && desc.BufferCount >= 2
                    && desc.BufferCount <= 16;
                if (queues) proven = proven && DeviceIdentity(state->device.Get(), deviceIdentity);
                ComPtr<IUnknown> queueIdentity;
                for (UINT index = 0; queues && proven && index < desc.BufferCount; ++index)
                {
                    ComPtr<ID3D12CommandQueue> candidate;
                    ComPtr<ID3D12Device> candidateDevice;
                    ComPtr<IUnknown> identity, candidateDeviceIdentity;
                    proven = queues[index] && (!nodes || nodes[index] <= 1)
                        && SUCCEEDED(queues[index]->QueryInterface(IID_PPV_ARGS(&candidate)))
                        && candidate->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT
                        && SUCCEEDED(candidate->GetDevice(IID_PPV_ARGS(&candidateDevice)))
                        && DeviceIdentity(candidateDevice.Get(), candidateDeviceIdentity)
                        && candidateDeviceIdentity.Get() == deviceIdentity.Get()
                        && SUCCEEDED(candidate.As(&identity));
                    if (!proven) break;
                    if (!replacement) { replacement = candidate; queueIdentity = identity; }
                    else proven = identity.Get() == queueIdentity.Get();
                }
                std::lock_guard lock(state->mutex);
                if (proven)
                {
                    if (replacement) state->queue = replacement;
                    state->presentationArea.store(uint64_t(desc.Width)*desc.Height, std::memory_order_relaxed);
                }
                else
                {
                    state->Disable();
                    RecordFailure(L"ResizeBuffers1 presentation queue ownership could not be proven");
                }
            }
        }
        return result;
    }
};

struct ColorSpaceTag
{
    using Fn = HRESULT (WINAPI*)(IDXGISwapChain3*, DXGI_COLOR_SPACE_TYPE);
    static HRESULT Call(Fn original, IDXGISwapChain3* chain, DXGI_COLOR_SPACE_TYPE color)
    {
        if (gInsideOverlay) return original(chain, color);
        InternalScope internal;
        const HRESULT result = original(chain, color);
        if (SUCCEEDED(result)) if (auto state = Find(chain))
        {
            std::lock_guard lock(state->mutex);
            state->colorSpace = color;
            state->colorSpaceObserved = true;
        }
        return result;
    }
};

struct ReleaseReservation
{
    std::shared_ptr<State> state;
    bool drain = false;
};

ReleaseReservation ReserveRelease(const std::shared_ptr<State>& state,
    IUnknown* object, bool activePresent)
{
    ReleaseReservation reservation{};
    if (!state) return reservation;
    std::lock_guard lock(state->mutex);
    if (state->retired || state->swapchain != object) return reservation;
    const bool first = state->activeReleases++ == 0;
    if (first) state->releaseInvalidated = false;
    state->releaseInProgress = true;
    reservation.state = state;
    if (first && !activePresent)
    {
        vsync_control::InvalidateChain(state->vsync);
        ++state->vsyncRevision;
        state->vsyncRefresh = true;
        state->releaseInvalidated = true;
        // Existing Present leases own their renderer generation until they
        // return. Otherwise drop backbuffer references before native Release
        // so they cannot artificially keep the swapchain alive.
        reservation.drain = state->activeCallbacks == 0;
    }
    return reservation;
}

void CompleteRelease(IUnknown* object, const ReleaseReservation& reservation,
    ULONG refs)
{
    if (!reservation.state) return;
    const auto& state = reservation.state;
    bool remove = false;
    bool finalize = false;
    std::shared_ptr<State> replacement;
    std::shared_ptr<State> rejectedReplacement;
    {
        std::lock_guard lock(state->mutex);
        if (refs == 0 && !state->retired && state->swapchain == object)
        {
            ui_input_coherence::InvalidateResources();
            if (!state->releaseInvalidated)
            {
                vsync_control::InvalidateChain(state->vsync);
                ++state->vsyncRevision;
                state->vsyncRefresh = true;
                state->releaseInvalidated = true;
            }
            state->retired = true;
            state->swapchain = nullptr;
            replacement = std::move(state->pendingReplacement);
            remove = true;
        }
        if (state->activeReleases != 0) --state->activeReleases;
        if (!state->retired && state->activeReleases == 0)
        {
            state->releaseInProgress = false;
            state->releaseInvalidated = false;
            rejectedReplacement = std::move(state->pendingReplacement);
        }
        finalize = state->retired && state->activeCallbacks == 0
            && state->activeReleases == 0;
    }

    // Replace/remove only this exact generation. A candidate queued during
    // external Release becomes visible only after the predecessor's native
    // refcount reached zero. If the predecessor survived, the candidate is
    // released outside both locks and the old generation remains authoritative.
    if (remove)
    {
        std::lock_guard lock(gStatesMutex);
        const auto found = gStates.find(object);
        if (found != gStates.end() && found->second == state)
        {
            if (replacement) found->second = replacement;
            else gStates.erase(found);
        }
    }
    if (rejectedReplacement)
        RecordFailure(L"D3D12 UI replacement generation rejected because predecessor survived Release");
    if (finalize) FinalizeRetiredState(state);
}

struct ReleaseTag
{
    using Fn = ULONG (WINAPI*)(IUnknown*);
    static ULONG Call(Fn original, IUnknown* object)
    {
        auto state = Find(object);
        if (!state) return original(object);
        // A wrapper may balance a temporary interface for the exact object
        // whose Present is on this thread. Do not drain that object's active
        // renderer. An unrelated object receives ordinary retirement even
        // when this thread is inside another Present or overlay callback.
        const bool activePresent = PresentActiveFor(object, state.get());
        // Render can query this exact wrapper while holding its state mutex.
        // A balanced temporary-interface Release from that internal query
        // belongs to the existing Present lease; preserve direct forwarding
        // instead of recursively acquiring the state mutex. Other objects and
        // generations still take the full reservation/retirement path below.
        if (gInsideOverlay && activePresent && gRenderingState == state.get())
            return original(object);
        // Backbuffers can retain their parent swapchain. Drain and drop only
        // those references before forwarding Release. The COM result then
        // tells us whether actual destruction occurred; no speculative refcount
        // probe or hidden swapchain reference is needed. Surviving objects keep
        // their renderer, descriptors and allocators for the next Present.
        const auto reservation = ReserveRelease(state, object, activePresent);
        if (!reservation.state) return original(object);
        // The reservation blocks new rendering and proves there are no
        // unleased users when drain is requested. ReleaseBuffers and native
        // Release deliberately run outside every state/registry lock.
        if (reservation.drain) state->ReleaseBuffers();
        ULONG refs;
        {
            InternalScope internal;
            NativeReleaseScope releasing(object, state.get());
            refs = original(object);
        }
        CompleteRelease(object, reservation, refs);
        return refs;
    }
};

void AttachSwapchain(IDXGISwapChain* chain, IUnknown* presentationQueue, HMODULE creatorOwner)
{
    if (!chain || !presentationQueue) return;
    InternalScope internal;
    auto** table = *reinterpret_cast<void***>(chain);
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(table[8]), &owner)
        || (owner != gSystemDxgi.load(std::memory_order_acquire)
            && (!creatorOwner || owner != creatorOwner))) return;
    // Native DXGI and an image-owned wrapper returned by the exact hooked
    // creator are supported. Public COM queue/device identity and complete
    // lifecycle interception below remain mandatory in both cases.
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Device> queueDevice;
    ComPtr<ID3D12Device> swapDevice;
    ComPtr<IDXGISwapChain3> chain3;
    ComPtr<IUnknown> queueIdentity;
    ComPtr<IUnknown> swapIdentity;
    if (FAILED(presentationQueue->QueryInterface(IID_PPV_ARGS(&queue)))
        || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT
        || FAILED(queue->GetDevice(IID_PPV_ARGS(&queueDevice)))
        || FAILED(chain->GetDevice(IID_PPV_ARGS(&swapDevice)))
        || !DeviceIdentity(queueDevice.Get(), queueIdentity) || !DeviceIdentity(swapDevice.Get(), swapIdentity)
        || queueIdentity.Get() != swapIdentity.Get()
        || FAILED(chain->QueryInterface(IID_PPV_ARGS(&chain3)))) return;
    auto state = std::make_shared<State>();
    DXGI_SWAP_CHAIN_DESC1 desc{};
    if (FAILED(chain3->GetHwnd(&state->presentationWindow)) || !state->presentationWindow
        || FAILED(chain3->GetDesc1(&desc))) return;
    state->presentationArea.store(uint64_t(desc.Width)*desc.Height, std::memory_order_relaxed);
    state->swapchain = chain3.Get();
    state->queue = queue;
    state->device = queueDevice;
    state->creatorOwner = creatorOwner;
    // Refuse differing aliases instead of retaining a stale weak pointer.
    if (static_cast<void*>(chain) != static_cast<void*>(chain3.Get())) return;
    table = *reinterpret_cast<void***>(chain3.Get());
    state->applicationPresentEntry = table[8];
    state->applicationPresent1Entry = table[22];
    HookFamily<PresentTag, HRESULT, IDXGISwapChain*, UINT, UINT>::Bind(table[8], true);
    HookFamily<Present1Tag, HRESULT, IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*>::Bind(table[22], true);
    HookFamily<ResizeTag, HRESULT, IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT>::Bind(table[13], true);
    HookFamily<Resize1Tag, HRESULT, IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT,
        const UINT*, IUnknown* const*>::Bind(table[39], true);
    HookFamily<ColorSpaceTag, HRESULT, IDXGISwapChain3*, DXGI_COLOR_SPACE_TYPE>::Bind(table[38], true);
    HookFamily<ReleaseTag, ULONG, IUnknown*>::Bind(table[2], true);
    if (!HookFamily<PresentTag, HRESULT, IDXGISwapChain*, UINT, UINT>::Installed(table[8])
        || !HookFamily<Present1Tag, HRESULT, IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*>::Installed(table[22])
        || !HookFamily<ResizeTag, HRESULT, IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT>::Installed(table[13])
        || !HookFamily<Resize1Tag, HRESULT, IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT,
            const UINT*, IUnknown* const*>::Installed(table[39])
        || !HookFamily<ReleaseTag, ULONG, IUnknown*>::Installed(table[2])
        || !HookFamily<ColorSpaceTag, HRESULT, IDXGISwapChain3*, DXGI_COLOR_SPACE_TYPE>::Installed(table[38]))
    {
        RecordFailure(L"D3D12 UI lifecycle interception is incomplete; swapchain not retained");
        return;
    }
    if (!RegisterAttachedState(chain3.Get(), state))
        RecordFailure(L"D3D12 UI swapchain generation registration was ambiguous");
    else if (Find(chain3.Get()) == state)
        RefreshVsyncRegistration(state, chain);
}

void AttachFactory(IUnknown* unknown);
struct CreateChainTag
{
    using Fn = HRESULT (WINAPI*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
    static HRESULT Call(Fn original, IDXGIFactory* factory, IUnknown* queue,
        DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** chain)
    {
        if (SuppressCreateWhileInternal()) return original(factory, queue, desc, chain);
        const HMODULE owner = HookFamily<CreateChainTag, HRESULT, IDXGIFactory*, IUnknown*,
            DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**>::OriginalOwner(original);
        HRESULT result;
        { InternalScope internal; result = original(factory, queue, desc, chain); }
        if (SUCCEEDED(result) && chain) AttachSwapchain(*chain, queue, owner);
        return result;
    }
};
struct CreateHwndTag
{
    using Fn = HRESULT (WINAPI*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
    static HRESULT Call(Fn original, IDXGIFactory2* factory, IUnknown* queue, HWND window,
        const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,
        IDXGIOutput* output, IDXGISwapChain1** chain)
    {
        if (SuppressCreateWhileInternal())
            return original(factory, queue, window, desc, fullscreen, output, chain);
        const HMODULE owner = HookFamily<CreateHwndTag, HRESULT, IDXGIFactory2*, IUnknown*, HWND,
            const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
            IDXGISwapChain1**>::OriginalOwner(original);
        HRESULT result;
        { InternalScope internal; result = original(factory, queue, window, desc, fullscreen, output, chain); }
        if (SUCCEEDED(result) && chain) AttachSwapchain(*chain, queue, owner);
        return result;
    }
};
struct CreateCoreWindowTag
{
    using Fn = HRESULT (WINAPI*)(IDXGIFactory2*, IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*,
        IDXGIOutput*, IDXGISwapChain1**);
    static HRESULT Call(Fn original, IDXGIFactory2* factory, IUnknown* queue, IUnknown* window,
        const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* output, IDXGISwapChain1** chain)
    {
        if (SuppressCreateWhileInternal()) return original(factory, queue, window, desc, output, chain);
        const HMODULE owner = HookFamily<CreateCoreWindowTag, HRESULT, IDXGIFactory2*, IUnknown*,
            IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**>::OriginalOwner(original);
        HRESULT result;
        { InternalScope internal; result = original(factory, queue, window, desc, output, chain); }
        if (SUCCEEDED(result) && chain) AttachSwapchain(*chain, queue, owner);
        return result;
    }
};
struct CreateCompositionTag
{
    using Fn = HRESULT (WINAPI*)(IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*,
        IDXGIOutput*, IDXGISwapChain1**);
    static HRESULT Call(Fn original, IDXGIFactory2* factory, IUnknown* queue,
        const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* output, IDXGISwapChain1** chain)
    {
        if (SuppressCreateWhileInternal()) return original(factory, queue, desc, output, chain);
        const HMODULE owner = HookFamily<CreateCompositionTag, HRESULT, IDXGIFactory2*, IUnknown*,
            const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**>::OriginalOwner(original);
        HRESULT result;
        { InternalScope internal; result = original(factory, queue, desc, output, chain); }
        if (SUCCEEDED(result) && chain) AttachSwapchain(*chain, queue, owner);
        return result;
    }
};

void AttachFactory(IUnknown* unknown)
{
    if (!unknown) return;
    InternalScope internal;
    ComPtr<IDXGIFactory> factory;
    if (FAILED(unknown->QueryInterface(IID_PPV_ARGS(&factory)))) return;
    auto** table = *reinterpret_cast<void***>(factory.Get());
    HookFamily<CreateChainTag, HRESULT, IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**>::Bind(table[10], true);
    ComPtr<IDXGIFactory2> factory2;
    if (FAILED(factory.As(&factory2))) return;
    table = *reinterpret_cast<void***>(factory2.Get());
    HookFamily<CreateHwndTag, HRESULT, IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**>::Bind(table[15], true);
    HookFamily<CreateCoreWindowTag, HRESULT, IDXGIFactory2*, IUnknown*, IUnknown*,
        const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**>::Bind(table[16], true);
    HookFamily<CreateCompositionTag, HRESULT, IDXGIFactory2*, IUnknown*,
        const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**>::Bind(table[24], true);
}

void ObserveNativeFactory()
{
    wchar_t path[32768]{};
    const UINT length = GetSystemDirectoryW(path, static_cast<UINT>(std::size(path)));
    if (!length || length > std::size(path)-32 || wcscat_s(path, L"\\dxgi.dll")) return;
    HMODULE system = GetModuleHandleW(path);
    if (!system) return;
    gSystemDxgi.store(system, std::memory_order_release);
    using Create = HRESULT (WINAPI*)(REFIID, void**);
    const auto create = reinterpret_cast<Create>(GetProcAddress(system, "CreateDXGIFactory1"));
    if (!create) return;
    InternalScope internal;
    ComPtr<IDXGIFactory> factory;
    if (SUCCEEDED(create(IID_PPV_ARGS(&factory)))) AttachFactory(factory.Get());
}

struct FactoryTag
{
    using Fn = HRESULT (WINAPI*)(REFIID, void**);
    static HRESULT Call(Fn original, REFIID iid, void** factory)
    {
        const HRESULT result = original(iid, factory);
        if (!gInsideOverlay && SUCCEEDED(result) && factory)
        {
            ObserveNativeFactory();
            AttachFactory(static_cast<IUnknown*>(*factory));
        }
        return result;
    }
};
struct Factory2Tag
{
    using Fn = HRESULT (WINAPI*)(UINT, REFIID, void**);
    static HRESULT Call(Fn original, UINT flags, REFIID iid, void** factory)
    {
        const HRESULT result = original(flags, iid, factory);
        if (!gInsideOverlay && SUCCEEDED(result) && factory)
        {
            ObserveNativeFactory();
            AttachFactory(static_cast<IUnknown*>(*factory));
        }
        return result;
    }
};
}

FARPROC Resolve(HMODULE, LPCSTR name, FARPROC original) noexcept
{
    if (!name || reinterpret_cast<uintptr_t>(name) <= 0xFFFFu || !original) return original;
    if (strcmp(name, "CreateDXGIFactory") == 0 || strcmp(name, "CreateDXGIFactory1") == 0)
        return reinterpret_cast<FARPROC>(HookFamily<FactoryTag, HRESULT, REFIID, void**>::Bind(reinterpret_cast<void*>(original), true));
    if (strcmp(name, "CreateDXGIFactory2") == 0)
        return reinterpret_cast<FARPROC>(HookFamily<Factory2Tag, HRESULT, UINT, REFIID, void**>::Bind(reinterpret_cast<void*>(original), true));
    return original;
}

void Install(HMODULE module) noexcept
{
    if (!module) return;
    for (const char* name : {"CreateDXGIFactory", "CreateDXGIFactory1", "CreateDXGIFactory2"})
    {
        const auto original = GetProcAddress(module, name);
        if (original)
        {
            Resolve(module, name, original);
        }
    }
    gDxgiHooked.store(HookFamily<FactoryTag, HRESULT, REFIID, void**>::Installed()
        || HookFamily<Factory2Tag, HRESULT, UINT, REFIID, void**>::Installed(), std::memory_order_release);
}
}
