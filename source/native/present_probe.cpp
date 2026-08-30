#include "present_probe.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <share.h>
#include <vector>

namespace present_probe
{
namespace
{
using Microsoft::WRL::ComPtr;

constexpr GUID kStreamlineRetrieveBaseInterface = {
    0xadec44e2, 0x61f0, 0x45c3,
    { 0xad, 0x9f, 0x1b, 0x37, 0x37, 0x92, 0x84, 0xff }
};
constexpr size_t kFactoryVtableEntries = 32;
constexpr size_t kSwapchainVtableEntries = 41;
constexpr size_t kFactoryCreateSwapChainIndex = 10;
constexpr size_t kFactoryCreateSwapChainForHwndIndex = 15;
constexpr size_t kSwapchainPresentIndex = 8;
constexpr size_t kSwapchainPresent1Index = 22;
constexpr uint32_t kProbeWidth = 64;
constexpr uint32_t kProbeHeight = 36;
constexpr uint32_t kCopyWidth = 256;
constexpr uint32_t kCopyHeight = 144;
constexpr size_t kReadbackSlots = 32;
constexpr uint64_t kMaximumScheduledFrames = 12000;

using CreateFactoryFn = HRESULT (WINAPI*)(REFIID, void**);
using CreateFactory2Fn = HRESULT (WINAPI*)(UINT, REFIID, void**);
using FactoryCreateSwapChainFn = HRESULT (STDMETHODCALLTYPE*)(
    IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using FactoryCreateSwapChainForHwndFn = HRESULT (STDMETHODCALLTYPE*)(
    IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using SwapchainPresentFn = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using SwapchainPresent1Fn = HRESULT (STDMETHODCALLTYPE*)(
    IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

struct ProbeSlot
{
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ComPtr<ID3D12Resource> readback;
    uint64_t fenceValue = 0;
    uint64_t sequence = 0;
    int64_t qpc = 0;
    uint32_t backBufferIndex = 0;
    bool awaitingSubmission = false;
};

struct PendingPresent
{
    int64_t qpc = 0;
    uint32_t backBufferIndex = 0;
};

struct ProbeContext
{
    std::mutex mutex;
    bool initialized = false;
    bool unsupported = false;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Fence> fence;
    std::array<ProbeSlot, kReadbackSlots> slots{};
    uint64_t nextFenceValue = 1;
    uint64_t nextSequence = 0;
    uint64_t nextProcessedSequence = 0;
    uint64_t scheduled = 0;
    uint64_t captured = 0;
    uint64_t dropped = 0;
    uint32_t copyWidth = 0;
    uint32_t copyHeight = 0;
    uint32_t sourceLeft = 0;
    uint32_t sourceTop = 0;
    uint32_t bytesPerPixel = 0;
    uint32_t rowPitch = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    FILE* csv = nullptr;
    FILE* gray = nullptr;
    std::array<uint8_t, kProbeWidth * kProbeHeight> previousGray{};
    bool hasPreviousGray = false;
    std::deque<PendingPresent> pendingPresents;
};

struct FactoryHook
{
    ComPtr<IDXGIFactory7> factory;
    void** replacementVtable = nullptr;
    FactoryCreateSwapChainFn originalCreateSwapChain = nullptr;
    FactoryCreateSwapChainForHwndFn originalCreateSwapChainForHwnd = nullptr;
};

struct SwapchainHook
{
    ComPtr<IDXGISwapChain4> swapchain;
    ComPtr<ID3D12CommandQueue> queue;
    void** originalVtable = nullptr;
    void** replacementVtable = nullptr;
    SwapchainPresentFn originalPresent = nullptr;
    SwapchainPresent1Fn originalPresent1 = nullptr;
    ProbeContext probe;
};

std::atomic<CreateFactoryFn> gOriginalCreateFactory{nullptr};
std::atomic<CreateFactoryFn> gOriginalCreateFactory1{nullptr};
std::atomic<CreateFactory2Fn> gOriginalCreateFactory2{nullptr};
std::atomic<bool> gFactoryImportHookInstalled{false};
std::atomic<bool> gNativeFactoryHookInstalled{false};
std::atomic<bool> gNativeSwapchainHookInstalled{false};
std::atomic<bool> gEnabled{false};
std::atomic<bool> gDeferredReadbackEnabled{false};
std::atomic<uint64_t> gNativePresentCalls{0};
std::atomic<uint64_t> gScheduledFrames{0};
std::atomic<uint64_t> gCapturedFrames{0};
std::atomic<uint64_t> gDroppedFrames{0};
std::mutex gHookMutex;
// The probe owns COM references for the lifetime of the process.  Deliberately
// leak these two small registries so CRT shutdown never releases DXGI/D3D12
// objects after the graphics runtime has begun unloading.
auto* const gFactoryHooks =
    new std::vector<std::unique_ptr<FactoryHook>>();
auto* const gSwapchainHooks =
    new std::vector<std::unique_ptr<SwapchainHook>>();
LogCallback gLogCallback = nullptr;

void EmitLog(const wchar_t* message)
{
    if (gLogCallback)
        gLogCallback(message);
    else
    {
        OutputDebugStringW(L"[MfgUnlock] ");
        OutputDebugStringW(message);
        OutputDebugStringW(L"\n");
    }
}

uint32_t BytesPerPixel(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT:
        return 4;
    default:
        return 0;
    }
}

bool IsBgra(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return true;
    default:
        return false;
    }
}

bool IsRgb10(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_R10G10B10A2_TYPELESS
        || format == DXGI_FORMAT_R10G10B10A2_UNORM
        || format == DXGI_FORMAT_R10G10B10A2_UINT;
}

uint8_t LumaAt(const uint8_t* pixel, DXGI_FORMAT format)
{
    uint32_t red = 0;
    uint32_t green = 0;
    uint32_t blue = 0;
    if (IsRgb10(format))
    {
        uint32_t packed = 0;
        memcpy(&packed, pixel, sizeof(packed));
        red = (packed & 0x3ffu) >> 2;
        green = ((packed >> 10) & 0x3ffu) >> 2;
        blue = ((packed >> 20) & 0x3ffu) >> 2;
    }
    else if (IsBgra(format))
    {
        blue = pixel[0];
        green = pixel[1];
        red = pixel[2];
    }
    else
    {
        red = pixel[0];
        green = pixel[1];
        blue = pixel[2];
    }
    return static_cast<uint8_t>((54u * red + 183u * green + 19u * blue) >> 8);
}

bool HookImport(HMODULE module, const char* importedModule,
    const char* importedFunction, void* replacement, void*& original)
{
    auto* base = reinterpret_cast<uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE
        || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return false;
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress || !directory.Size)
        return false;

    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        base + directory.VirtualAddress);
    for (; descriptor->Name; ++descriptor)
    {
        const char* moduleName = reinterpret_cast<const char*>(base + descriptor->Name);
        if (_stricmp(moduleName, importedModule) != 0)
            continue;
        const DWORD originalRva = descriptor->OriginalFirstThunk
            ? descriptor->OriginalFirstThunk : descriptor->FirstThunk;
        auto* originalThunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + originalRva);
        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->FirstThunk);
        for (; originalThunk->u1.AddressOfData; ++originalThunk, ++thunk)
        {
            if (IMAGE_SNAP_BY_ORDINAL64(originalThunk->u1.Ordinal))
                continue;
            const auto* import = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                base + originalThunk->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(import->Name), importedFunction) != 0)
                continue;
            auto** slot = reinterpret_cast<void**>(&thunk->u1.Function);
            if (*slot == replacement)
                return true;
            DWORD oldProtection = 0;
            if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &oldProtection))
                return false;
            original = *slot;
            *slot = replacement;
            DWORD ignored = 0;
            const BOOL restored = VirtualProtect(
                slot, sizeof(*slot), oldProtection, &ignored);
            FlushInstructionCache(GetCurrentProcess(), slot, sizeof(*slot));
            return restored != FALSE;
        }
    }
    return false;
}

template <size_t EntryCount>
void** CloneVtable(void* object)
{
    if (!object)
        return nullptr;
    auto** current = *reinterpret_cast<void***>(object);
    auto** replacement = static_cast<void**>(VirtualAlloc(
        nullptr, EntryCount * sizeof(void*), MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE));
    if (!replacement)
        return nullptr;
    memcpy(replacement, current, EntryCount * sizeof(void*));
    return replacement;
}

bool CommitVtable(void* object, void** replacement)
{
    if (!object || !replacement)
        return false;
    auto** objectVtable = reinterpret_cast<void**>(object);
    InterlockedExchangePointer(
        reinterpret_cast<void* volatile*>(objectVtable), replacement);
    return true;
}

bool SameComIdentity(IUnknown* first, IUnknown* second)
{
    if (!first || !second)
        return false;
    ComPtr<IUnknown> firstIdentity;
    ComPtr<IUnknown> secondIdentity;
    return SUCCEEDED(first->QueryInterface(IID_PPV_ARGS(&firstIdentity)))
        && SUCCEEDED(second->QueryInterface(IID_PPV_ARGS(&secondIdentity)))
        && firstIdentity.Get() == secondIdentity.Get();
}

SwapchainHook* FindSwapchainHook(IDXGISwapChain* swapchain)
{
    std::lock_guard lock(gHookMutex);
    for (auto& hook : *gSwapchainHooks)
    {
        if (SameComIdentity(hook->swapchain.Get(), swapchain))
            return hook.get();
    }
    return nullptr;
}

FactoryHook* FindFactoryHook(IDXGIFactory* factory)
{
    std::lock_guard lock(gHookMutex);
    for (auto& hook : *gFactoryHooks)
    {
        if (SameComIdentity(hook->factory.Get(), factory))
            return hook.get();
    }
    return nullptr;
}

ComPtr<ID3D12CommandQueue> RetrieveNativeQueue(IUnknown* device)
{
    ComPtr<ID3D12CommandQueue> queue;
    if (!device)
        return queue;

    ComPtr<IUnknown> base;
    if (SUCCEEDED(device->QueryInterface(
        kStreamlineRetrieveBaseInterface, reinterpret_cast<void**>(base.GetAddressOf()))))
    {
        base.As(&queue);
    }
    if (!queue)
        device->QueryInterface(IID_PPV_ARGS(&queue));
    return queue;
}

ComPtr<IDXGISwapChain4> RetrieveNativeSwapchain(IDXGISwapChain* swapchain)
{
    ComPtr<IDXGISwapChain4> native;
    if (!swapchain)
        return native;

    ComPtr<IUnknown> base;
    if (FAILED(swapchain->QueryInterface(
            kStreamlineRetrieveBaseInterface,
            reinterpret_cast<void**>(base.GetAddressOf()))))
        base = swapchain;
    if (base)
        base.As(&native);
    return native;
}

bool OpenProbeFiles(ProbeContext& probe)
{
    wchar_t tempDirectory[MAX_PATH]{};
    const DWORD length = GetTempPathW(_countof(tempDirectory), tempDirectory);
    if (length == 0 || length >= _countof(tempDirectory))
        return false;

    wchar_t csvPath[MAX_PATH]{};
    wchar_t grayPath[MAX_PATH]{};
    swprintf_s(csvPath, L"%sMfgUnlock-present-%lu.csv", tempDirectory,
        static_cast<unsigned long>(GetCurrentProcessId()));
    swprintf_s(grayPath, L"%sMfgUnlock-present-%lu.gray", tempDirectory,
        static_cast<unsigned long>(GetCurrentProcessId()));
    probe.csv = _wfsopen(csvPath, L"w", _SH_DENYWR);
    probe.gray = _wfsopen(grayPath, L"wb", _SH_DENYWR);
    if (!probe.csv || !probe.gray)
        return false;
    fprintf(probe.csv,
        "sequence,qpc,backBuffer,format,grayOffset,hash,meanAbsDelta,changedPixels\n");
    fflush(probe.csv);

    wchar_t message[768]{};
    swprintf_s(message,
        L"Native presentation content probe started: %s and %s",
        csvPath, grayPath);
    EmitLog(message);
    return true;
}

bool InitializeProbe(SwapchainHook& hook)
{
    ProbeContext& probe = hook.probe;
    DXGI_SWAP_CHAIN_DESC1 description{};
    if (!hook.queue || FAILED(hook.swapchain->GetDesc1(&description)))
        return false;
    probe.bytesPerPixel = BytesPerPixel(description.Format);
    if (!probe.bytesPerPixel || description.Width < kProbeWidth
        || description.Height < kProbeHeight)
    {
        wchar_t message[256]{};
        swprintf_s(message,
            L"Native presentation content probe unsupported: format=%u size=%ux%u",
            static_cast<uint32_t>(description.Format), description.Width,
            description.Height);
        EmitLog(message);
        probe.unsupported = true;
        return false;
    }

    probe.format = description.Format;
    probe.copyWidth = std::min<uint32_t>(kCopyWidth, description.Width);
    probe.copyHeight = std::min<uint32_t>(kCopyHeight, description.Height);
    probe.sourceLeft = (description.Width - probe.copyWidth) / 2;
    probe.sourceTop = (description.Height - probe.copyHeight) / 2;
    probe.rowPitch = (probe.copyWidth * probe.bytesPerPixel
        + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
        & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);

    if (FAILED(hook.queue->GetDevice(IID_PPV_ARGS(&probe.device)))
        || FAILED(probe.device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(&probe.fence))))
        return false;

    const uint64_t readbackSize = static_cast<uint64_t>(probe.rowPitch)
        * probe.copyHeight;
    const D3D12_HEAP_PROPERTIES heapProperties{
        D3D12_HEAP_TYPE_READBACK, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
        D3D12_MEMORY_POOL_UNKNOWN, 1, 1
    };
    D3D12_RESOURCE_DESC resourceDescription{};
    resourceDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDescription.Alignment = 0;
    resourceDescription.Width = readbackSize;
    resourceDescription.Height = 1;
    resourceDescription.DepthOrArraySize = 1;
    resourceDescription.MipLevels = 1;
    resourceDescription.Format = DXGI_FORMAT_UNKNOWN;
    resourceDescription.SampleDesc.Count = 1;
    resourceDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    for (auto& slot : probe.slots)
    {
        if (FAILED(probe.device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator)))
            || FAILED(probe.device->CreateCommandList(0,
                D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(), nullptr,
                IID_PPV_ARGS(&slot.commandList)))
            || FAILED(slot.commandList->Close())
            || FAILED(probe.device->CreateCommittedResource(
                &heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDescription,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&slot.readback))))
            return false;
    }
    if (!OpenProbeFiles(probe))
        return false;

    probe.initialized = true;
    wchar_t message[256]{};
    swprintf_s(message,
        L"Native presentation probe ready: format=%u crop=%ux%u output=%ux%u",
        static_cast<uint32_t>(probe.format), probe.copyWidth, probe.copyHeight,
        kProbeWidth, kProbeHeight);
    EmitLog(message);
    return true;
}

void ProcessCompletedSlot(ProbeContext& probe, ProbeSlot& slot)
{
    if (!slot.fenceValue
        || probe.fence->GetCompletedValue() < slot.fenceValue)
        return;

    uint8_t* mapped = nullptr;
    const D3D12_RANGE readRange{
        0, static_cast<SIZE_T>(probe.rowPitch) * probe.copyHeight
    };
    if (FAILED(slot.readback->Map(0, &readRange,
        reinterpret_cast<void**>(&mapped))) || !mapped)
        return;

    std::array<uint8_t, kProbeWidth * kProbeHeight> gray{};
    uint64_t hash = 1469598103934665603ull;
    uint64_t totalDelta = 0;
    uint32_t changedPixels = 0;
    for (uint32_t y = 0; y < kProbeHeight; ++y)
    {
        const uint32_t sourceY = y * probe.copyHeight / kProbeHeight;
        const uint8_t* row = mapped + static_cast<size_t>(sourceY) * probe.rowPitch;
        for (uint32_t x = 0; x < kProbeWidth; ++x)
        {
            const uint32_t sourceX = x * probe.copyWidth / kProbeWidth;
            const uint8_t value = LumaAt(
                row + static_cast<size_t>(sourceX) * probe.bytesPerPixel,
                probe.format);
            const size_t index = static_cast<size_t>(y) * kProbeWidth + x;
            gray[index] = value;
            hash ^= value;
            hash *= 1099511628211ull;
            if (probe.hasPreviousGray)
            {
                const uint32_t delta = value > probe.previousGray[index]
                    ? value - probe.previousGray[index]
                    : probe.previousGray[index] - value;
                totalDelta += delta;
                if (delta > 2)
                    ++changedPixels;
            }
        }
    }
    const D3D12_RANGE writeRange{0, 0};
    slot.readback->Unmap(0, &writeRange);

    const long long grayOffset = _ftelli64(probe.gray);
    fwrite(gray.data(), 1, gray.size(), probe.gray);
    const double meanAbsDelta = probe.hasPreviousGray
        ? static_cast<double>(totalDelta) / gray.size() : 0.0;
    fprintf(probe.csv, "%llu,%lld,%u,%u,%lld,%016llX,%.6f,%u\n",
        static_cast<unsigned long long>(slot.sequence),
        static_cast<long long>(slot.qpc), slot.backBufferIndex,
        static_cast<uint32_t>(probe.format), grayOffset,
        static_cast<unsigned long long>(hash), meanAbsDelta, changedPixels);
    probe.previousGray = gray;
    probe.hasPreviousGray = true;
    ++probe.captured;
    gCapturedFrames.fetch_add(1, std::memory_order_relaxed);
    if ((probe.captured & 63u) == 0)
    {
        fflush(probe.csv);
        fflush(probe.gray);
    }
    slot.fenceValue = 0;
}

void ProcessCompletedFrames(ProbeContext& probe)
{
    for (;;)
    {
        ProbeSlot* next = nullptr;
        for (auto& slot : probe.slots)
        {
            if (slot.fenceValue != 0
                && slot.sequence == probe.nextProcessedSequence)
            {
                next = &slot;
                break;
            }
        }
        if (!next || probe.fence->GetCompletedValue() < next->fenceValue)
            return;
        ProcessCompletedSlot(probe, *next);
        ++probe.nextProcessedSequence;
    }
}

void QueuePresentedFrame(SwapchainHook& hook, UINT flags,
    uint32_t backBufferIndex, int64_t qpc)
{
    if (!gDeferredReadbackEnabled.load(std::memory_order_acquire)
        || (flags & DXGI_PRESENT_TEST) != 0)
        return;

    std::lock_guard lock(hook.probe.mutex);
    ProbeContext& probe = hook.probe;
    if (probe.pendingPresents.size() >= kReadbackSlots)
    {
        ++probe.dropped;
        gDroppedFrames.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    probe.pendingPresents.push_back({qpc, backBufferIndex});
}

uint32_t PrepareQueueCapturesForHook(SwapchainHook& hook,
    ID3D12CommandList** commandLists, uint32_t capacity)
{
    if (!commandLists || capacity == 0)
        return 0;

    std::lock_guard lock(hook.probe.mutex);
    ProbeContext& probe = hook.probe;
    if (probe.unsupported)
        return 0;
    if (!probe.initialized && !InitializeProbe(hook))
    {
        probe.unsupported = true;
        EmitLog(L"Deferred presentation content probe initialization failed");
        return 0;
    }
    ProcessCompletedFrames(probe);

    uint32_t prepared = 0;
    while (prepared < capacity && !probe.pendingPresents.empty()
        && probe.scheduled < kMaximumScheduledFrames)
    {
        const PendingPresent pending = probe.pendingPresents.front();
        ProbeSlot& slot = probe.slots[probe.nextSequence % probe.slots.size()];
        if (slot.fenceValue != 0 || slot.awaitingSubmission)
        {
            probe.pendingPresents.pop_front();
            ++probe.dropped;
            gDroppedFrames.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        ComPtr<ID3D12Resource> backBuffer;
        if (FAILED(hook.swapchain->GetBuffer(
                pending.backBufferIndex, IID_PPV_ARGS(&backBuffer))))
        {
            probe.pendingPresents.pop_front();
            ++probe.dropped;
            gDroppedFrames.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        const D3D12_RESOURCE_DESC backBufferDescription = backBuffer->GetDesc();
        if (backBufferDescription.Format != probe.format
            || FAILED(slot.allocator->Reset())
            || FAILED(slot.commandList->Reset(slot.allocator.Get(), nullptr)))
        {
            probe.pendingPresents.pop_front();
            ++probe.dropped;
            gDroppedFrames.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        D3D12_RESOURCE_BARRIER before{};
        before.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        before.Transition.pResource = backBuffer.Get();
        before.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        before.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        before.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        slot.commandList->ResourceBarrier(1, &before);

        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = slot.readback.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint.Footprint.Format = probe.format;
        destination.PlacedFootprint.Footprint.Width = probe.copyWidth;
        destination.PlacedFootprint.Footprint.Height = probe.copyHeight;
        destination.PlacedFootprint.Footprint.Depth = 1;
        destination.PlacedFootprint.Footprint.RowPitch = probe.rowPitch;
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = backBuffer.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.SubresourceIndex = 0;
        const D3D12_BOX sourceBox{
            probe.sourceLeft, probe.sourceTop, 0,
            probe.sourceLeft + probe.copyWidth,
            probe.sourceTop + probe.copyHeight, 1
        };
        slot.commandList->CopyTextureRegion(
            &destination, 0, 0, 0, &source, &sourceBox);
        std::swap(before.Transition.StateBefore, before.Transition.StateAfter);
        slot.commandList->ResourceBarrier(1, &before);
        if (FAILED(slot.commandList->Close()))
        {
            probe.pendingPresents.pop_front();
            ++probe.dropped;
            gDroppedFrames.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        slot.sequence = probe.nextSequence++;
        slot.qpc = pending.qpc;
        slot.backBufferIndex = pending.backBufferIndex;
        slot.awaitingSubmission = true;
        commandLists[prepared++] = slot.commandList.Get();
        probe.pendingPresents.pop_front();
        ++probe.scheduled;
        gScheduledFrames.fetch_add(1, std::memory_order_relaxed);
    }
    return prepared;
}

void CompleteQueueCapturesForHook(SwapchainHook& hook)
{
    std::lock_guard lock(hook.probe.mutex);
    ProbeContext& probe = hook.probe;
    if (!probe.initialized || !probe.fence)
        return;
    bool any = false;
    for (const auto& slot : probe.slots)
        any |= slot.awaitingSubmission;
    if (!any)
        return;

    const uint64_t fenceValue = probe.nextFenceValue++;
    if (FAILED(hook.queue->Signal(probe.fence.Get(), fenceValue)))
        return;
    for (auto& slot : probe.slots)
    {
        if (!slot.awaitingSubmission)
            continue;
        slot.awaitingSubmission = false;
        slot.fenceValue = fenceValue;
    }
}

HRESULT STDMETHODCALLTYPE HookSwapchainPresent(
    IDXGISwapChain* swapchain, UINT syncInterval, UINT flags)
{
    gNativePresentCalls.fetch_add(1, std::memory_order_relaxed);
    SwapchainHook* hook = FindSwapchainHook(swapchain);
    if (!hook || !hook->originalPresent)
        return E_FAIL;
    const uint32_t backBufferIndex = hook->swapchain->GetCurrentBackBufferIndex();
    const HRESULT result = hook->originalPresent(swapchain, syncInterval, flags);
    if (SUCCEEDED(result))
    {
        LARGE_INTEGER qpc{};
        QueryPerformanceCounter(&qpc);
        QueuePresentedFrame(*hook, flags, backBufferIndex, qpc.QuadPart);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE HookSwapchainPresent1(
    IDXGISwapChain1* swapchain, UINT syncInterval, UINT flags,
    const DXGI_PRESENT_PARAMETERS* parameters)
{
    gNativePresentCalls.fetch_add(1, std::memory_order_relaxed);
    SwapchainHook* hook = FindSwapchainHook(swapchain);
    if (!hook || !hook->originalPresent1)
        return E_FAIL;
    const uint32_t backBufferIndex = hook->swapchain->GetCurrentBackBufferIndex();
    const HRESULT result = hook->originalPresent1(
        swapchain, syncInterval, flags, parameters);
    if (SUCCEEDED(result))
    {
        LARGE_INTEGER qpc{};
        QueryPerformanceCounter(&qpc);
        QueuePresentedFrame(*hook, flags, backBufferIndex, qpc.QuadPart);
    }
    return result;
}

bool HookNativeSwapchain(IDXGISwapChain* swapchain, IUnknown* device)
{
    if (!swapchain)
        return false;

    // The sample owns the Streamline proxy while the DLSS-G plugin presents
    // through its native base swapchain.  Always unwrap first so this hook
    // observes the actual flips and never re-enters the interposer.
    ComPtr<IDXGISwapChain4> swapchain4 = RetrieveNativeSwapchain(swapchain);
    if (!swapchain4)
        return false;
    ComPtr<ID3D12CommandQueue> queue = RetrieveNativeQueue(device);
    if (!queue)
        return false;

    std::lock_guard lock(gHookMutex);
    for (auto& existing : *gSwapchainHooks)
    {
        if (SameComIdentity(existing->swapchain.Get(), swapchain4.Get()))
            return true;
    }

    auto hook = std::make_unique<SwapchainHook>();
    hook->swapchain = swapchain4;
    hook->queue = queue;
    hook->originalVtable = *reinterpret_cast<void***>(swapchain4.Get());
    hook->replacementVtable = CloneVtable<kSwapchainVtableEntries>(swapchain4.Get());
    if (!hook->replacementVtable)
        return false;
    hook->originalPresent = reinterpret_cast<SwapchainPresentFn>(
        hook->replacementVtable[kSwapchainPresentIndex]);
    hook->originalPresent1 = reinterpret_cast<SwapchainPresent1Fn>(
        hook->replacementVtable[kSwapchainPresent1Index]);
    hook->replacementVtable[kSwapchainPresentIndex] =
        reinterpret_cast<void*>(&HookSwapchainPresent);
    hook->replacementVtable[kSwapchainPresent1Index] =
        reinterpret_cast<void*>(&HookSwapchainPresent1);
    if (!CommitVtable(swapchain4.Get(), hook->replacementVtable))
        return false;
    gSwapchainHooks->push_back(std::move(hook));
    gNativeSwapchainHookInstalled.store(true, std::memory_order_release);
    EmitLog(L"Native DXGI presentation hook installed");
    return true;
}

HRESULT STDMETHODCALLTYPE HookFactoryCreateSwapChain(
    IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* description,
    IDXGISwapChain** swapchain)
{
    FactoryHook* hook = FindFactoryHook(factory);
    if (!hook || !hook->originalCreateSwapChain)
        return E_FAIL;
    const HRESULT result = hook->originalCreateSwapChain(
        factory, device, description, swapchain);
    if (SUCCEEDED(result) && swapchain && *swapchain)
        HookNativeSwapchain(*swapchain, device);
    return result;
}

HRESULT STDMETHODCALLTYPE HookFactoryCreateSwapChainForHwnd(
    IDXGIFactory2* factory, IUnknown* device, HWND window,
    const DXGI_SWAP_CHAIN_DESC1* description,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDescription,
    IDXGIOutput* restrictToOutput, IDXGISwapChain1** swapchain)
{
    FactoryHook* hook = FindFactoryHook(factory);
    if (!hook || !hook->originalCreateSwapChainForHwnd)
        return E_FAIL;
    const HRESULT result = hook->originalCreateSwapChainForHwnd(
        factory, device, window, description, fullscreenDescription,
        restrictToOutput, swapchain);
    if (SUCCEEDED(result) && swapchain && *swapchain)
        HookNativeSwapchain(*swapchain, device);
    return result;
}

void HookNativeFactory(IUnknown* exposedFactory)
{
    if (!exposedFactory)
        return;
    ComPtr<IUnknown> base;
    if (FAILED(exposedFactory->QueryInterface(
        kStreamlineRetrieveBaseInterface,
        reinterpret_cast<void**>(base.GetAddressOf()))))
        base = exposedFactory;

    ComPtr<IDXGIFactory7> factory;
    if (FAILED(base.As(&factory)))
        return;

    std::lock_guard lock(gHookMutex);
    for (auto& existing : *gFactoryHooks)
    {
        if (SameComIdentity(existing->factory.Get(), factory.Get()))
            return;
    }
    auto hook = std::make_unique<FactoryHook>();
    hook->factory = factory;
    hook->replacementVtable = CloneVtable<kFactoryVtableEntries>(factory.Get());
    if (!hook->replacementVtable)
        return;
    hook->originalCreateSwapChain = reinterpret_cast<FactoryCreateSwapChainFn>(
        hook->replacementVtable[kFactoryCreateSwapChainIndex]);
    hook->originalCreateSwapChainForHwnd =
        reinterpret_cast<FactoryCreateSwapChainForHwndFn>(
            hook->replacementVtable[kFactoryCreateSwapChainForHwndIndex]);
    hook->replacementVtable[kFactoryCreateSwapChainIndex] =
        reinterpret_cast<void*>(&HookFactoryCreateSwapChain);
    hook->replacementVtable[kFactoryCreateSwapChainForHwndIndex] =
        reinterpret_cast<void*>(&HookFactoryCreateSwapChainForHwnd);
    if (!CommitVtable(factory.Get(), hook->replacementVtable))
        return;
    gFactoryHooks->push_back(std::move(hook));
    gNativeFactoryHookInstalled.store(true, std::memory_order_release);
    EmitLog(L"Native DXGI factory hook installed");
}

HRESULT WINAPI HookCreateFactory(REFIID interfaceId, void** factory)
{
    CreateFactoryFn original = gOriginalCreateFactory.load(std::memory_order_acquire);
    if (!original)
        return E_FAIL;
    const HRESULT result = original(interfaceId, factory);
    if (SUCCEEDED(result) && factory && *factory)
        HookNativeFactory(static_cast<IUnknown*>(*factory));
    return result;
}

HRESULT WINAPI HookCreateFactory1(REFIID interfaceId, void** factory)
{
    CreateFactoryFn original = gOriginalCreateFactory1.load(std::memory_order_acquire);
    if (!original)
        return E_FAIL;
    const HRESULT result = original(interfaceId, factory);
    if (SUCCEEDED(result) && factory && *factory)
        HookNativeFactory(static_cast<IUnknown*>(*factory));
    return result;
}

HRESULT WINAPI HookCreateFactory2(UINT flags, REFIID interfaceId, void** factory)
{
    CreateFactory2Fn original = gOriginalCreateFactory2.load(std::memory_order_acquire);
    if (!original)
        return E_FAIL;
    const HRESULT result = original(flags, interfaceId, factory);
    if (SUCCEEDED(result) && factory && *factory)
        HookNativeFactory(static_cast<IUnknown*>(*factory));
    return result;
}

template <typename Function>
bool InstallOneImport(HMODULE executable, const char* moduleName,
    const char* functionName, void* replacement,
    std::atomic<Function>& originalStorage)
{
    void* original = nullptr;
    const bool installed = HookImport(
        executable, moduleName, functionName, replacement, original);
    if (original)
        originalStorage.store(reinterpret_cast<Function>(original),
            std::memory_order_release);
    return installed;
}
}

bool Install(HMODULE executable, LogCallback logCallback)
{
    if (logCallback)
        gLogCallback = logCallback;
    bool installed = false;
    installed |= InstallOneImport(executable, "sl.interposer.dll",
        "CreateDXGIFactory", reinterpret_cast<void*>(&HookCreateFactory),
        gOriginalCreateFactory);
    installed |= InstallOneImport(executable, "sl.interposer.dll",
        "CreateDXGIFactory1", reinterpret_cast<void*>(&HookCreateFactory1),
        gOriginalCreateFactory1);
    installed |= InstallOneImport(executable, "sl.interposer.dll",
        "CreateDXGIFactory2", reinterpret_cast<void*>(&HookCreateFactory2),
        gOriginalCreateFactory2);
    gFactoryImportHookInstalled.store(installed, std::memory_order_release);
    return installed;
}

bool RegisterSwapchain(IDXGISwapChain* swapchain, IUnknown* presentationQueue)
{
    if (!gEnabled.load(std::memory_order_acquire)
        || !swapchain || !presentationQueue)
        return false;
    return HookNativeSwapchain(swapchain, presentationQueue);
}

bool UnregisterSwapchain(IDXGISwapChain* swapchain)
{
    ComPtr<IDXGISwapChain4> native = RetrieveNativeSwapchain(swapchain);
    if (!native)
        return false;

    std::lock_guard lock(gHookMutex);
    const auto found = std::find_if(gSwapchainHooks->begin(), gSwapchainHooks->end(),
        [&](const std::unique_ptr<SwapchainHook>& hook) {
            return SameComIdentity(hook->swapchain.Get(), native.Get());
        });
    if (found == gSwapchainHooks->end())
        return true;

    SwapchainHook& hook = **found;
    if (*reinterpret_cast<void***>(hook.swapchain.Get()) == hook.replacementVtable)
        CommitVtable(hook.swapchain.Get(), hook.originalVtable);
    if (hook.probe.csv)
        fclose(hook.probe.csv);
    if (hook.probe.gray)
        fclose(hook.probe.gray);
    if (hook.replacementVtable)
        VirtualFree(hook.replacementVtable, 0, MEM_RELEASE);
    gSwapchainHooks->erase(found);
    gNativeSwapchainHookInstalled.store(
        !gSwapchainHooks->empty(), std::memory_order_release);
    EmitLog(L"Native DXGI presentation hook unregistered before swapchain release");
    return true;
}

uint32_t PrepareQueueCaptures(ID3D12CommandQueue* queue,
    ID3D12CommandList** commandLists, uint32_t capacity)
{
    if (!gDeferredReadbackEnabled.load(std::memory_order_acquire)
        || !queue || !commandLists || capacity == 0)
        return 0;
    std::lock_guard lock(gHookMutex);
    for (auto& hook : *gSwapchainHooks)
    {
        if (SameComIdentity(hook->queue.Get(), queue))
            return PrepareQueueCapturesForHook(*hook, commandLists, capacity);
    }
    return 0;
}

void CompleteQueueCaptures(ID3D12CommandQueue* queue)
{
    if (!gDeferredReadbackEnabled.load(std::memory_order_acquire) || !queue)
        return;
    std::lock_guard lock(gHookMutex);
    for (auto& hook : *gSwapchainHooks)
    {
        if (SameComIdentity(hook->queue.Get(), queue))
        {
            CompleteQueueCapturesForHook(*hook);
            return;
        }
    }
}

void SetEnabled(bool enabled)
{
    const bool previous = gEnabled.exchange(enabled, std::memory_order_acq_rel);
    wchar_t unsafeReadback[8]{};
    const DWORD unsafeLength = GetEnvironmentVariableW(
        L"RTX40_MFG_UNSAFE_PRESENT_READBACK", unsafeReadback,
        static_cast<DWORD>(std::size(unsafeReadback)));
    const bool deferredReadback = enabled && unsafeLength == 1
        && unsafeReadback[0] == L'1';
    gDeferredReadbackEnabled.store(deferredReadback, std::memory_order_release);
    if (previous != enabled)
        EmitLog(enabled
            ? L"Native presentation content probe enabled"
            : L"Native presentation content probe disabled");
    if (enabled && !deferredReadback)
        EmitLog(L"Native presentation pixel readback remains disabled (safe mode)");
}

Snapshot ReadSnapshot()
{
    Snapshot snapshot{};
    snapshot.factoryImportHookInstalled =
        gFactoryImportHookInstalled.load(std::memory_order_relaxed);
    snapshot.nativeFactoryHookInstalled =
        gNativeFactoryHookInstalled.load(std::memory_order_relaxed);
    snapshot.nativeSwapchainHookInstalled =
        gNativeSwapchainHookInstalled.load(std::memory_order_relaxed);
    snapshot.enabled = gEnabled.load(std::memory_order_relaxed);
    snapshot.nativePresentCalls = gNativePresentCalls.load(std::memory_order_relaxed);
    snapshot.scheduledFrames = gScheduledFrames.load(std::memory_order_relaxed);
    snapshot.capturedFrames = gCapturedFrames.load(std::memory_order_relaxed);
    snapshot.droppedFrames = gDroppedFrames.load(std::memory_order_relaxed);
    return snapshot;
}
}
