#include "ngx_output_probe.h"
#include "present_probe.h"

#include <dxgi1_6.h>
#include <sl.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <share.h>
#include <vector>

namespace ngx_output_probe
{
namespace
{
using Microsoft::WRL::ComPtr;

// Keep the extended interface tail intact: the runtime can QI these objects to
// newer ID3D12CommandQueue/ID3D12Device revisions after registration.
constexpr size_t kQueueVtableEntries = 32;
constexpr size_t kExecuteCommandListsIndex = 10;
constexpr size_t kDeviceVtableEntries = 128;
constexpr size_t kCreateCommandQueueIndex = 8;
constexpr size_t kReadbackSlots = 64;
constexpr size_t kImmutableSlots = 20;
constexpr uint64_t kQueueOnlyRetireLag = 8;
constexpr uint32_t kCopyWidth = 256;
constexpr uint32_t kCopyHeight = 144;
constexpr uint32_t kProbeWidth = 128;
constexpr uint32_t kProbeHeight = 72;
// Keep enough history to validate a live DLSS-G feature recycle after the
// sample has reached a stable 6x state.  Six thousand captures can be consumed
// in only a few seconds at 5 generated frames per engine frame.
constexpr uint64_t kMaximumCaptures = 20000;

using ExecuteCommandListsFn = void (STDMETHODCALLTYPE*)(
    ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using CreateCommandQueueFn = HRESULT (STDMETHODCALLTYPE*)(
    ID3D12Device*, const D3D12_COMMAND_QUEUE_DESC*, REFIID, void**);

struct ReadbackSlot
{
    ComPtr<ID3D12Resource> readback;
    ComPtr<ID3D12Resource> source;
    ComPtr<IUnknown> commandListIdentity;
    ID3D12GraphicsCommandList* commandList = nullptr;
    uint64_t fenceValue = 0;
    uint64_t sequence = 0;
    uint64_t batch = 0;
    uint64_t frameId = 0;
    int count = 0;
    int index = 0;
    CapturedOutputKind kind = CapturedOutputKind::eInterpolated;
    bool awaitingSubmission = false;
};

struct ImmutableSlot
{
    ComPtr<ID3D12Resource> texture;
    ComPtr<ID3D12Fence> streamlineFence;
    ComPtr<ID3D12Fence> streamlineFenceBaseline;
    ComPtr<IUnknown> commandListIdentity;
    D3D12_RESOURCE_DESC description{};
    ID3D12GraphicsCommandList* commandList = nullptr;
    uint64_t fenceValue = 0;
    uint64_t sequence = 0;
    uint64_t batch = 0;
    uint64_t reservationBatch = 0;
    uint64_t streamlineFenceValue = 0;
    uint64_t streamlineFenceBaselineValue = 0;
    int count = 0;
    int index = 0;
    int reservationIndex = 0;
    bool reserved = false;
    bool awaitingSubmission = false;
    bool streamlineFenceEligible = false;
};

struct QueueHook
{
    ComPtr<ID3D12CommandQueue> queue;
    void** replacementVtable = nullptr;
    ExecuteCommandListsFn originalExecute = nullptr;
};

struct DeviceHook
{
    ComPtr<ID3D12Device> device;
    void** replacementVtable = nullptr;
    CreateCommandQueueFn originalCreateCommandQueue = nullptr;
};

struct ProbeContext
{
    std::mutex mutex;
    bool initialized = false;
    bool unsupported = false;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Fence> fence;
    std::array<ReadbackSlot, kReadbackSlots> slots{};
    uint64_t nextSequence = 0;
    uint64_t nextFenceValue = 1;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    uint64_t rowBytes = 0;
    uint64_t readbackBytes = 0;
    uint32_t sourceLeft = 0;
    uint32_t sourceTop = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    FILE* csv = nullptr;
    FILE* realCsv = nullptr;
    FILE* gray = nullptr;
    uint64_t processedBatch = 0;
    int processedCount = 0;
    uint32_t processedMask = 0;
    std::array<uint64_t, 5> processedHashes{};
    uint64_t previousBatch = 0;
    int previousIndex = 0;
    std::vector<uint8_t> previousBytes;
};

struct ImmutableContext
{
    bool initialized = false;
    bool unsupported = false;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12Fence> latestStreamlineFence;
    std::array<ImmutableSlot, kImmutableSlots> slots{};
    uint64_t nextSequence = 0;
    uint64_t nextFenceValue = 1;
    uint64_t latestStreamlineFenceValue = 0;
};

ProbeContext gProbe;
ImmutableContext gImmutable;
std::mutex gQueueMutex;
std::vector<QueueHook> gQueueHooks;
std::mutex gDeviceMutex;
std::vector<DeviceHook> gDeviceHooks;
std::mutex gNativeInterfaceMutex;
PFun_slGetNativeInterface* gGetNativeInterface = nullptr;
std::atomic<bool> gEnabled{false};
std::atomic<bool> gImmutableEnabled{false};
std::atomic<bool> gQueueHookInstalled{false};
std::atomic<uint64_t> gScheduled{0};
std::atomic<uint64_t> gSubmitted{0};
std::atomic<uint64_t> gCaptured{0};
std::atomic<uint64_t> gDropped{0};
std::atomic<uint64_t> gCompleteBatches{0};
std::atomic<uint64_t> gDuplicateBatches{0};
std::atomic<uint64_t> gImmutablePrepared{0};
std::atomic<uint64_t> gImmutableSubmitted{0};
std::atomic<uint64_t> gImmutableRetired{0};
std::atomic<uint64_t> gImmutableDropped{0};
std::atomic<uint64_t> gImmutableReservationReclaims{0};
std::atomic<uint32_t> gImmutableAllocated{0};
std::atomic<bool> gImmutableExhaustionLogged{false};
std::atomic<bool> gImmutableReservationReclaimLogged{false};
std::atomic<bool> gNativeCommandListMatchLogged{false};
LogCallback gLogCallback = nullptr;

void EmitLog(const wchar_t* message)
{
    if (gLogCallback)
        gLogCallback(message);
    else
        OutputDebugStringW(message);
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

ComPtr<IUnknown> ResolveNativeIdentity(IUnknown* interfacePointer)
{
    ComPtr<IUnknown> identity;
    if (!interfacePointer)
        return identity;

    // Streamline can pass an SL proxy to NGX while ExecuteCommandLists receives
    // the underlying D3D12 object. Use NVIDIA's supported unwrapping API to
    // canonicalize both sides before comparing them. The API is documented as
    // not thread-safe, so serialize discovery and every invocation here.
    {
        std::lock_guard lock(gNativeInterfaceMutex);
        if (!gGetNativeInterface)
        {
            if (const HMODULE interposer = GetModuleHandleW(L"sl.interposer.dll"))
            {
                gGetNativeInterface = reinterpret_cast<PFun_slGetNativeInterface*>(
                    GetProcAddress(interposer, "slGetNativeInterface"));
            }
        }
        if (gGetNativeInterface)
        {
            void* nativeInterface = nullptr;
            if (gGetNativeInterface(interfacePointer, &nativeInterface)
                    == sl::Result::eOk
                && nativeInterface)
            {
                auto* nativeUnknown = static_cast<IUnknown*>(nativeInterface);
                nativeUnknown->QueryInterface(IID_PPV_ARGS(&identity));
                nativeUnknown->Release();
            }
        }
    }

    if (!identity)
        interfacePointer->QueryInterface(IID_PPV_ARGS(&identity));
    return identity;
}

enum class CommandListMatch
{
    eNone,
    ePointer,
    eNativeIdentity
};

template <typename Slot>
CommandListMatch MatchSubmittedCommandList(Slot& slot,
    UINT commandListCount, ID3D12CommandList* const* commandLists,
    std::vector<ComPtr<IUnknown>>& submittedIdentities,
    std::vector<uint8_t>& submittedIdentityResolved)
{
    if (!slot.commandList)
        return CommandListMatch::eNone;
    for (UINT i = 0; i < commandListCount; ++i)
    {
        if (commandLists[i] == slot.commandList)
            return CommandListMatch::ePointer;
    }

    if (!slot.commandListIdentity)
        return CommandListMatch::eNone;

    for (UINT i = 0; i < commandListCount; ++i)
    {
        if (!submittedIdentityResolved[i])
            continue;
        if (submittedIdentities[i]
            && submittedIdentities[i].Get() == slot.commandListIdentity.Get())
            return CommandListMatch::eNativeIdentity;
    }
    return CommandListMatch::eNone;
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

bool BuildGrayFrame(const std::vector<uint8_t>& packed,
    std::array<uint8_t, kProbeWidth * kProbeHeight>& gray)
{
    const uint32_t bytesPerPixel = BytesPerPixel(gProbe.format);
    const uint32_t copyWidth = gProbe.footprint.Footprint.Width;
    const uint32_t copyHeight = gProbe.footprint.Footprint.Height;
    if (!bytesPerPixel || copyWidth < kProbeWidth || copyHeight < kProbeHeight
        || gProbe.rowBytes < static_cast<uint64_t>(copyWidth) * bytesPerPixel
        || packed.size() < static_cast<size_t>(gProbe.rowBytes) * copyHeight)
        return false;

    for (uint32_t y = 0; y < kProbeHeight; ++y)
    {
        const uint32_t sourceY = y * copyHeight / kProbeHeight;
        const uint8_t* row = packed.data()
            + static_cast<size_t>(sourceY) * gProbe.rowBytes;
        for (uint32_t x = 0; x < kProbeWidth; ++x)
        {
            const uint32_t sourceX = x * copyWidth / kProbeWidth;
            gray[static_cast<size_t>(y) * kProbeWidth + x] = LumaAt(
                row + static_cast<size_t>(sourceX) * bytesPerPixel,
                gProbe.format);
        }
    }
    return true;
}

bool OpenProbeFiles()
{
    wchar_t tempDirectory[MAX_PATH]{};
    const DWORD length = GetTempPathW(
        static_cast<DWORD>(std::size(tempDirectory)), tempDirectory);
    if (!length || length >= std::size(tempDirectory))
        return false;
    wchar_t csvPath[MAX_PATH]{};
    wchar_t realCsvPath[MAX_PATH]{};
    wchar_t grayPath[MAX_PATH]{};
    swprintf_s(csvPath, L"%sMfgUnlock-ngx-output-%lu.csv", tempDirectory,
        static_cast<unsigned long>(GetCurrentProcessId()));
    swprintf_s(realCsvPath, L"%sMfgUnlock-ngx-real-%lu.csv", tempDirectory,
        static_cast<unsigned long>(GetCurrentProcessId()));
    swprintf_s(grayPath, L"%sMfgUnlock-ngx-output-%lu.gray", tempDirectory,
        static_cast<unsigned long>(GetCurrentProcessId()));
    gProbe.csv = _wfsopen(csvPath, L"w", _SH_DENYWR);
    gProbe.realCsv = _wfsopen(realCsvPath, L"w", _SH_DENYWR);
    gProbe.gray = _wfsopen(grayPath, L"wb", _SH_DENYWR);
    if (!gProbe.csv || !gProbe.realCsv || !gProbe.gray)
        return false;
    // The official sample's -maxFrames path terminates without giving the CRT
    // a reliable shutdown window.  Keep diagnostic rows and luma frames
    // unbuffered so the final temporal batch is never left partially written.
    setvbuf(gProbe.csv, nullptr, _IONBF, 0);
    setvbuf(gProbe.realCsv, nullptr, _IONBF, 0);
    setvbuf(gProbe.gray, nullptr, _IONBF, 0);
    fprintf(gProbe.csv,
        "sequence,batch,frameId,count,index,format,width,height,source,hash,"
        "nonZeroBytes,meanAbsByteDelta,changedBytes,uniqueSoFar,batchDuplicate,"
        "grayOffset,grayHash,grayWidth,grayHeight\n");
    fprintf(gProbe.realCsv,
        "sequence,batch,frameId,count,index,format,width,height,source,hash,"
        "nonZeroBytes,grayOffset,grayHash,grayWidth,grayHeight\n");
    fflush(gProbe.csv);
    fflush(gProbe.realCsv);
    wchar_t message[1024]{};
    swprintf_s(message, L"NGX output readback started: %s, %s and %s",
        csvPath, realCsvPath, grayPath);
    EmitLog(message);
    return true;
}

bool Initialize(ID3D12Resource* output)
{
    if (gProbe.initialized)
        return true;
    if (!output || FAILED(output->GetDevice(IID_PPV_ARGS(&gProbe.device))))
        return false;

    const D3D12_RESOURCE_DESC sourceDescription = output->GetDesc();
    if (sourceDescription.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D
        || sourceDescription.SampleDesc.Count != 1
        || sourceDescription.Width == 0 || sourceDescription.Height == 0)
    {
        gProbe.unsupported = true;
        return false;
    }

    D3D12_RESOURCE_DESC copyDescription = sourceDescription;
    copyDescription.Width = std::min<uint64_t>(kCopyWidth, sourceDescription.Width);
    copyDescription.Height = std::min<uint32_t>(kCopyHeight, sourceDescription.Height);
    copyDescription.DepthOrArraySize = 1;
    copyDescription.MipLevels = 1;
    copyDescription.Alignment = 0;
    copyDescription.SampleDesc.Count = 1;
    copyDescription.SampleDesc.Quality = 0;
    copyDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    copyDescription.Flags = D3D12_RESOURCE_FLAG_NONE;
    UINT rowCount = 0;
    gProbe.device->GetCopyableFootprints(&copyDescription, 0, 1, 0,
        &gProbe.footprint, &rowCount, &gProbe.rowBytes, &gProbe.readbackBytes);
    if (!gProbe.readbackBytes || !gProbe.rowBytes
        || FAILED(gProbe.device->CreateFence(
            0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gProbe.fence))))
        return false;

    gProbe.format = sourceDescription.Format;
    gProbe.sourceLeft = static_cast<uint32_t>(
        (sourceDescription.Width - copyDescription.Width) / 2);
    gProbe.sourceTop = (sourceDescription.Height - copyDescription.Height) / 2;
    if (!OpenProbeFiles())
        return false;

    gProbe.initialized = true;
    wchar_t message[256]{};
    swprintf_s(message,
        L"NGX output probe ready: format=%u source=%llux%u crop=%ux%u rowPitch=%u",
        static_cast<uint32_t>(gProbe.format),
        static_cast<unsigned long long>(sourceDescription.Width),
        sourceDescription.Height, gProbe.footprint.Footprint.Width,
        gProbe.footprint.Footprint.Height, gProbe.footprint.Footprint.RowPitch);
    EmitLog(message);
    return true;
}

bool EnsureReadback(ReadbackSlot& slot)
{
    if (slot.readback)
        return true;
    const D3D12_HEAP_PROPERTIES heap{
        D3D12_HEAP_TYPE_READBACK, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
        D3D12_MEMORY_POOL_UNKNOWN, 1, 1
    };
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = gProbe.readbackBytes;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return SUCCEEDED(gProbe.device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &description,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&slot.readback)));
}

bool SameDescription(const D3D12_RESOURCE_DESC& first,
    const D3D12_RESOURCE_DESC& second)
{
    return first.Dimension == second.Dimension
        && first.Alignment == second.Alignment
        && first.Width == second.Width
        && first.Height == second.Height
        && first.DepthOrArraySize == second.DepthOrArraySize
        && first.MipLevels == second.MipLevels
        && first.Format == second.Format
        && first.SampleDesc.Count == second.SampleDesc.Count
        && first.SampleDesc.Quality == second.SampleDesc.Quality
        && first.Layout == second.Layout
        && first.Flags == second.Flags;
}

bool InitializeImmutable(ID3D12Resource* output)
{
    if (gImmutable.initialized)
        return true;
    if (!output || FAILED(output->GetDevice(IID_PPV_ARGS(&gImmutable.device))))
        return false;
    const D3D12_RESOURCE_DESC description = output->GetDesc();
    if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D
        || description.SampleDesc.Count != 1
        || (description.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0)
    {
        gImmutable.unsupported = true;
        EmitLog(L"Immutable NGX output ring rejected an unsupported output resource");
        return false;
    }
    if (FAILED(gImmutable.device->CreateFence(
            0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gImmutable.fence))))
        return false;
    gImmutable.initialized = true;
    wchar_t message[256]{};
    swprintf_s(message,
        L"Immutable NGX output ring ready: format=%u size=%llux%u slots=%zu",
        static_cast<uint32_t>(description.Format),
        static_cast<unsigned long long>(description.Width), description.Height,
        gImmutable.slots.size());
    EmitLog(message);
    return true;
}

void ProcessCompletedImmutableSlots()
{
    const uint64_t queueCompleted = gImmutable.fence
        ? gImmutable.fence->GetCompletedValue() : 0;
    for (auto& slot : gImmutable.slots)
    {
        const bool queueComplete = slot.fenceValue != 0
            && queueCompleted >= slot.fenceValue;
        const bool streamlineComplete = slot.streamlineFence
            && slot.streamlineFenceValue != 0
            && slot.streamlineFence->GetCompletedValue()
                >= slot.streamlineFenceValue;
        const bool queueFallbackComplete = !slot.streamlineFence
            && slot.streamlineFenceEligible && queueComplete
            && gImmutable.nextSequence >= slot.sequence
            && gImmutable.nextSequence - slot.sequence >= kQueueOnlyRetireLag;
        if (!streamlineComplete && !queueFallbackComplete)
            continue;
        slot.commandList = nullptr;
        slot.commandListIdentity.Reset();
        slot.fenceValue = 0;
        slot.streamlineFence.Reset();
        slot.streamlineFenceBaseline.Reset();
        slot.streamlineFenceValue = 0;
        slot.streamlineFenceBaselineValue = 0;
        slot.sequence = 0;
        slot.batch = 0;
        slot.reservationBatch = 0;
        slot.count = 0;
        slot.index = 0;
        slot.reservationIndex = 0;
        slot.awaitingSubmission = false;
        slot.streamlineFenceEligible = false;
        if (!gImmutableEnabled.load(std::memory_order_acquire))
        {
            slot.texture.Reset();
            slot.description = {};
            gImmutableAllocated.fetch_sub(1, std::memory_order_relaxed);
        }
        gImmutableRetired.fetch_add(1, std::memory_order_relaxed);
    }
}

uint32_t ReleaseIdleImmutableTextures()
{
    uint32_t released = 0;
    for (auto& slot : gImmutable.slots)
    {
        if (slot.reserved || slot.reservationBatch != 0
            || slot.awaitingSubmission || slot.fenceValue != 0
            || slot.streamlineFenceValue != 0 || !slot.texture)
            continue;
        slot.texture.Reset();
        slot.description = {};
        slot.commandList = nullptr;
        slot.commandListIdentity.Reset();
        slot.streamlineFence.Reset();
        slot.streamlineFenceBaseline.Reset();
        slot.streamlineFenceValue = 0;
        slot.streamlineFenceBaselineValue = 0;
        slot.sequence = 0;
        slot.batch = 0;
        slot.reservationBatch = 0;
        slot.count = 0;
        slot.index = 0;
        slot.reservationIndex = 0;
        slot.streamlineFenceEligible = false;
        gImmutableAllocated.fetch_sub(1, std::memory_order_relaxed);
        ++released;
    }
    return released;
}

bool EnsureImmutableTexture(ImmutableSlot& slot,
    const D3D12_RESOURCE_DESC& description)
{
    if (slot.texture && SameDescription(slot.description, description))
        return true;
    if (slot.texture)
        gImmutableAllocated.fetch_sub(1, std::memory_order_relaxed);
    slot.texture.Reset();
    slot.description = {};
    const D3D12_HEAP_PROPERTIES heap{
        D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
        D3D12_MEMORY_POOL_UNKNOWN, 1, 1
    };
    if (FAILED(gImmutable.device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&slot.texture))))
        return false;
    slot.description = description;
    const uint32_t allocated =
        gImmutableAllocated.fetch_add(1, std::memory_order_relaxed) + 1;
    wchar_t message[160]{};
    swprintf_s(message, L"Immutable NGX texture pool allocated %u/%zu resources",
        allocated, gImmutable.slots.size());
    EmitLog(message);
    return true;
}

void ResetSlot(ReadbackSlot& slot)
{
    slot.source.Reset();
    slot.commandList = nullptr;
    slot.commandListIdentity.Reset();
    slot.fenceValue = 0;
    slot.awaitingSubmission = false;
}

void RecordBatchHash(const ReadbackSlot& slot, uint64_t hash,
    uint32_t& uniqueSoFar, bool& batchDuplicate)
{
    if (gProbe.processedBatch != slot.batch
        || gProbe.processedCount != slot.count || slot.index == 1)
    {
        gProbe.processedBatch = slot.batch;
        gProbe.processedCount = slot.count;
        gProbe.processedMask = 0;
        gProbe.processedHashes.fill(0);
    }
    const uint32_t target = static_cast<uint32_t>(slot.index - 1);
    if (target < gProbe.processedHashes.size())
    {
        gProbe.processedHashes[target] = hash;
        gProbe.processedMask |= 1u << target;
    }
    uniqueSoFar = 0;
    for (int current = 0; current < slot.count; ++current)
    {
        if ((gProbe.processedMask & (1u << current)) == 0)
            continue;
        bool unique = true;
        for (int previous = 0; previous < current; ++previous)
        {
            if ((gProbe.processedMask & (1u << previous)) != 0
                && gProbe.processedHashes[previous] == gProbe.processedHashes[current])
            {
                unique = false;
                break;
            }
        }
        if (unique)
            ++uniqueSoFar;
    }
    const uint32_t expectedMask = (1u << static_cast<uint32_t>(slot.count)) - 1u;
    if (gProbe.processedMask == expectedMask)
    {
        batchDuplicate = uniqueSoFar != static_cast<uint32_t>(slot.count);
        gCompleteBatches.fetch_add(1, std::memory_order_relaxed);
        if (batchDuplicate)
            gDuplicateBatches.fetch_add(1, std::memory_order_relaxed);
    }
}

void ProcessCompletedSlot(ReadbackSlot& slot)
{
    if (!slot.fenceValue
        || !gProbe.fence
        || gProbe.fence->GetCompletedValue() < slot.fenceValue)
        return;

    uint8_t* mapped = nullptr;
    const D3D12_RANGE readRange{0, static_cast<SIZE_T>(gProbe.readbackBytes)};
    if (FAILED(slot.readback->Map(
        0, &readRange, reinterpret_cast<void**>(&mapped))) || !mapped)
    {
        ResetSlot(slot);
        return;
    }

    uint64_t hash = 1469598103934665603ull;
    uint64_t nonZeroBytes = 0;
    uint64_t totalDelta = 0;
    uint64_t changedBytes = 0;
    std::vector<uint8_t> packed;
    packed.reserve(static_cast<size_t>(gProbe.rowBytes)
        * gProbe.footprint.Footprint.Height);
    for (uint32_t rowIndex = 0;
        rowIndex < gProbe.footprint.Footprint.Height; ++rowIndex)
    {
        const uint8_t* row = mapped
            + static_cast<size_t>(rowIndex) * gProbe.footprint.Footprint.RowPitch;
        for (uint64_t byteIndex = 0; byteIndex < gProbe.rowBytes; ++byteIndex)
        {
            const uint8_t value = row[byteIndex];
            packed.push_back(value);
            hash ^= value;
            hash *= 1099511628211ull;
            nonZeroBytes += value != 0;
        }
    }
    const D3D12_RANGE writeRange{0, 0};
    slot.readback->Unmap(0, &writeRange);

    const bool comparable = gProbe.previousBatch == slot.batch
        && gProbe.previousIndex + 1 == slot.index
        && gProbe.previousBytes.size() == packed.size();
    if (comparable)
    {
        for (size_t i = 0; i < packed.size(); ++i)
        {
            const uint32_t delta = packed[i] > gProbe.previousBytes[i]
                ? packed[i] - gProbe.previousBytes[i]
                : gProbe.previousBytes[i] - packed[i];
            totalDelta += delta;
            changedBytes += delta != 0;
        }
    }
    const double meanDelta = comparable && !packed.empty()
        ? static_cast<double>(totalDelta) / packed.size() : 0.0;

    std::array<uint8_t, kProbeWidth * kProbeHeight> gray{};
    long long grayOffset = -1;
    uint64_t grayHash = 0;
    if (gProbe.gray && BuildGrayFrame(packed, gray))
    {
        grayHash = 1469598103934665603ull;
        for (const uint8_t value : gray)
        {
            grayHash ^= value;
            grayHash *= 1099511628211ull;
        }
        grayOffset = _ftelli64(gProbe.gray);
        if (fwrite(gray.data(), 1, gray.size(), gProbe.gray) != gray.size())
            grayOffset = -1;
    }

    uint32_t uniqueSoFar = 0;
    bool batchDuplicate = false;
    const bool interpolated = slot.kind == CapturedOutputKind::eInterpolated;
    if (interpolated)
        RecordBatchHash(slot, hash, uniqueSoFar, batchDuplicate);
    if (interpolated && gProbe.csv)
    {
        fprintf(gProbe.csv,
            "%llu,%llu,%llu,%d,%d,%u,%u,%u,0x%p,%016llX,%llu,%.9f,%llu,%u,%d,%lld,%016llX,%u,%u\n",
            static_cast<unsigned long long>(slot.sequence),
            static_cast<unsigned long long>(slot.batch),
            static_cast<unsigned long long>(slot.frameId), slot.count, slot.index,
            static_cast<uint32_t>(gProbe.format), gProbe.footprint.Footprint.Width,
            gProbe.footprint.Footprint.Height, slot.source.Get(),
            static_cast<unsigned long long>(hash),
            static_cast<unsigned long long>(nonZeroBytes), meanDelta,
            static_cast<unsigned long long>(changedBytes), uniqueSoFar,
            batchDuplicate, grayOffset,
            static_cast<unsigned long long>(grayHash),
            kProbeWidth, kProbeHeight);
        if ((slot.sequence & 31u) == 0 || batchDuplicate)
            fflush(gProbe.csv);
    }
    else if (!interpolated && gProbe.realCsv)
    {
        fprintf(gProbe.realCsv,
            "%llu,%llu,%llu,%d,%d,%u,%u,%u,0x%p,%016llX,%llu,%lld,%016llX,%u,%u\n",
            static_cast<unsigned long long>(slot.sequence),
            static_cast<unsigned long long>(slot.batch),
            static_cast<unsigned long long>(slot.frameId), slot.count, slot.index,
            static_cast<uint32_t>(gProbe.format), gProbe.footprint.Footprint.Width,
            gProbe.footprint.Footprint.Height, slot.source.Get(),
            static_cast<unsigned long long>(hash),
            static_cast<unsigned long long>(nonZeroBytes), grayOffset,
            static_cast<unsigned long long>(grayHash),
            kProbeWidth, kProbeHeight);
        if ((slot.sequence & 31u) == 0)
            fflush(gProbe.realCsv);
    }
    if (batchDuplicate)
    {
        wchar_t message[256]{};
        swprintf_s(message,
            L"NGX duplicate output batch: batch=%llu count=%d unique=%u",
            static_cast<unsigned long long>(slot.batch), slot.count, uniqueSoFar);
        EmitLog(message);
    }
    if (interpolated)
    {
        gProbe.previousBatch = slot.batch;
        gProbe.previousIndex = slot.index;
        gProbe.previousBytes = std::move(packed);
    }
    if ((slot.sequence & 31u) == 0 && gProbe.gray)
        fflush(gProbe.gray);
    gCaptured.fetch_add(1, std::memory_order_relaxed);
    ResetSlot(slot);
}

void ProcessCompletedSlots()
{
    for (auto& slot : gProbe.slots)
        ProcessCompletedSlot(slot);
}

QueueHook* FindQueueHook(ID3D12CommandQueue* queue)
{
    for (auto& hook : gQueueHooks)
    {
        if (SameComIdentity(hook.queue.Get(), queue))
            return &hook;
    }
    return nullptr;
}

DeviceHook* FindDeviceHook(ID3D12Device* device)
{
    for (auto& hook : gDeviceHooks)
    {
        if (SameComIdentity(hook.device.Get(), device))
            return &hook;
    }
    return nullptr;
}

void STDMETHODCALLTYPE HookExecuteCommandLists(ID3D12CommandQueue* queue,
    UINT commandListCount, ID3D12CommandList* const* commandLists)
{
    ExecuteCommandListsFn original = nullptr;
    {
        std::lock_guard lock(gQueueMutex);
        if (QueueHook* hook = FindQueueHook(queue))
            original = hook->originalExecute;
    }
    if (!original)
        return;

    std::array<ID3D12CommandList*, 32> presentationCaptures{};
    const uint32_t presentationCaptureCount =
        present_probe::PrepareQueueCaptures(queue,
            presentationCaptures.data(),
            static_cast<uint32_t>(presentationCaptures.size()));
    if (presentationCaptureCount != 0)
    {
        std::vector<ID3D12CommandList*> combined;
        combined.reserve(presentationCaptureCount + commandListCount);
        combined.insert(combined.end(), presentationCaptures.begin(),
            presentationCaptures.begin() + presentationCaptureCount);
        combined.insert(combined.end(), commandLists,
            commandLists + commandListCount);
        original(queue, static_cast<UINT>(combined.size()), combined.data());
        present_probe::CompleteQueueCaptures(queue);
    }
    else
    {
        original(queue, commandListCount, commandLists);
    }

    std::vector<ComPtr<IUnknown>> submittedIdentities(commandListCount);
    std::vector<uint8_t> submittedIdentityResolved(commandListCount, 0);
    bool resolveSubmittedIdentities =
        gEnabled.load(std::memory_order_acquire)
        || gImmutableEnabled.load(std::memory_order_acquire);
    if (!resolveSubmittedIdentities)
    {
        std::lock_guard pendingLock(gProbe.mutex);
        resolveSubmittedIdentities = std::any_of(
            gProbe.slots.begin(), gProbe.slots.end(),
            [](const ReadbackSlot& slot) { return slot.awaitingSubmission; })
            || std::any_of(gImmutable.slots.begin(), gImmutable.slots.end(),
                [](const ImmutableSlot& slot) {
                    return slot.awaitingSubmission;
                });
    }
    if (resolveSubmittedIdentities)
    {
        // Do not enter Streamline while holding the ring mutex. SetOptions and
        // queue execution can run on different Cyberpunk threads.
        for (UINT i = 0; i < commandListCount; ++i)
        {
            submittedIdentities[i] = ResolveNativeIdentity(commandLists[i]);
            submittedIdentityResolved[i] = 1;
        }
    }
    std::lock_guard lock(gProbe.mutex);
    if (gProbe.initialized && gProbe.fence)
    {
        ProcessCompletedSlots();
        bool matched = false;
        for (auto& slot : gProbe.slots)
        {
            if (!slot.awaitingSubmission || !slot.commandList)
                continue;
            matched |= MatchSubmittedCommandList(slot, commandListCount,
                commandLists, submittedIdentities,
                submittedIdentityResolved) != CommandListMatch::eNone;
        }
        if (matched)
        {
            const uint64_t fenceValue = gProbe.nextFenceValue++;
            if (SUCCEEDED(queue->Signal(gProbe.fence.Get(), fenceValue)))
            {
                for (auto& slot : gProbe.slots)
                {
                    if (!slot.awaitingSubmission || !slot.commandList)
                        continue;
                    if (MatchSubmittedCommandList(slot, commandListCount,
                            commandLists, submittedIdentities,
                            submittedIdentityResolved)
                        != CommandListMatch::eNone)
                    {
                        slot.awaitingSubmission = false;
                        slot.fenceValue = fenceValue;
                        gSubmitted.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        }
    }

    // The immutable ring is mutated by the NGX evaluation threads as well as
    // queue submission. Keep all slot matching and retirement under the same
    // mutex; Cyberpunk records and submits these lists concurrently.
    if (!gImmutable.initialized || !gImmutable.fence)
        return;
    ProcessCompletedImmutableSlots();
    bool immutableMatched = false;
    for (auto& slot : gImmutable.slots)
    {
        if (!slot.awaitingSubmission || !slot.commandList)
            continue;
        immutableMatched |= MatchSubmittedCommandList(slot, commandListCount,
            commandLists, submittedIdentities,
            submittedIdentityResolved) != CommandListMatch::eNone;
    }
    if (!immutableMatched)
        return;
    const uint64_t immutableFenceValue = gImmutable.nextFenceValue++;
    if (FAILED(queue->Signal(gImmutable.fence.Get(), immutableFenceValue)))
        return;
    for (auto& slot : gImmutable.slots)
    {
        if (!slot.awaitingSubmission || !slot.commandList)
            continue;
        const CommandListMatch match = MatchSubmittedCommandList(slot,
            commandListCount, commandLists, submittedIdentities,
            submittedIdentityResolved);
        if (match != CommandListMatch::eNone)
        {
            slot.awaitingSubmission = false;
            slot.fenceValue = immutableFenceValue;
            gImmutableSubmitted.fetch_add(1, std::memory_order_relaxed);
            if (match == CommandListMatch::eNativeIdentity
                && !gNativeCommandListMatchLogged.exchange(
                    true, std::memory_order_acq_rel))
            {
                EmitLog(L"Matched NGX work to queue submission through "
                    L"Streamline native command-list identity");
            }
        }
    }
}

HRESULT STDMETHODCALLTYPE HookCreateCommandQueue(ID3D12Device* device,
    const D3D12_COMMAND_QUEUE_DESC* description, REFIID interfaceId,
    void** commandQueue)
{
    CreateCommandQueueFn original = nullptr;
    {
        std::lock_guard lock(gDeviceMutex);
        if (DeviceHook* hook = FindDeviceHook(device))
            original = hook->originalCreateCommandQueue;
    }
    if (!original)
        return E_FAIL;
    const HRESULT result = original(device, description, interfaceId, commandQueue);
    if (SUCCEEDED(result) && commandQueue && *commandQueue)
    {
        ComPtr<ID3D12CommandQueue> queue;
        reinterpret_cast<IUnknown*>(*commandQueue)->QueryInterface(
            IID_PPV_ARGS(&queue));
        if (queue)
            RegisterQueue(queue.Get());
    }
    return result;
}
}

bool QueueFeaturesEnabled()
{
    return gEnabled.load(std::memory_order_acquire)
        || gImmutableEnabled.load(std::memory_order_acquire);
}

void Configure(bool enabled, bool immutableEnabled, LogCallback logCallback)
{
    if (logCallback)
        gLogCallback = logCallback;
    gEnabled.store(enabled, std::memory_order_release);
    gImmutableEnabled.store(immutableEnabled, std::memory_order_release);
    EmitLog(enabled
        ? L"NGX output content probe enabled"
        : L"NGX output content probe disabled");
    EmitLog(immutableEnabled
        ? L"Immutable NGX output ring enabled"
        : L"Immutable NGX output ring disabled");
}

void SetImmutableEnabled(bool enabled)
{
    const bool previous = gImmutableEnabled.exchange(
        enabled, std::memory_order_acq_rel);
    uint32_t releasedTextures = 0;
    if (previous && !enabled)
    {
        std::lock_guard lock(gProbe.mutex);
        for (auto& slot : gImmutable.slots)
        {
            slot.reservationBatch = 0;
            slot.reservationIndex = 0;
        }
        ProcessCompletedImmutableSlots();
        releasedTextures = ReleaseIdleImmutableTextures();
    }
    else if (!previous && enabled)
    {
        // Streamline may reuse the same fence object while restarting its
        // completion values when frame generation is re-enabled.  A baseline
        // retained from the previous MFG epoch would make every lower value
        // look stale and strand the new pool.  Begin the new MFG epoch without
        // a baseline; also unblock any old slot that was waiting to acquire
        // its first completion fence when the mode changed.
        std::lock_guard lock(gProbe.mutex);
        gImmutableExhaustionLogged.store(false, std::memory_order_release);
        gImmutable.latestStreamlineFence.Reset();
        gImmutable.latestStreamlineFenceValue = 0;
        for (auto& slot : gImmutable.slots)
        {
            if (!slot.streamlineFenceEligible || slot.streamlineFence)
                continue;
            slot.streamlineFenceBaseline.Reset();
            slot.streamlineFenceBaselineValue = 0;
        }
    }
    if (previous != enabled)
    {
        if (enabled)
            gImmutableReservationReclaimLogged.store(
                false, std::memory_order_release);
        EmitLog(enabled
            ? L"Immutable NGX output ring enabled"
            : L"Immutable NGX output ring disabled");
        if (!enabled && releasedTextures != 0)
        {
            wchar_t message[160]{};
            swprintf_s(message,
                L"Released %u idle immutable textures after leaving MFG",
                releasedTextures);
            EmitLog(message);
        }
    }
}

bool RegisterDevice(ID3D12Device* device)
{
    // Registration happens from slSetD3DDevice, before the game creates its
    // queues and before a live 2x -> MFG control change can enable the ring.
    // Keep the hook dormant when diagnostics are disabled, but install it now
    // so the existing graphics queue can be observed later.
    if (!device)
        return false;
    std::lock_guard lock(gDeviceMutex);
    if (FindDeviceHook(device))
        return true;
    DeviceHook hook{};
    hook.device = device;
    hook.replacementVtable = CloneVtable<kDeviceVtableEntries>(device);
    if (!hook.replacementVtable)
        return false;
    hook.originalCreateCommandQueue = reinterpret_cast<CreateCommandQueueFn>(
        hook.replacementVtable[kCreateCommandQueueIndex]);
    hook.replacementVtable[kCreateCommandQueueIndex] =
        reinterpret_cast<void*>(&HookCreateCommandQueue);
    if (!CommitVtable(device, hook.replacementVtable))
        return false;
    gDeviceHooks.push_back(std::move(hook));
    EmitLog(L"NGX output probe hooked D3D12 command-queue creation");
    return true;
}

bool RegisterQueue(ID3D12CommandQueue* queue)
{
    if (!queue)
        return false;
    std::lock_guard lock(gQueueMutex);
    if (FindQueueHook(queue))
        return true;
    QueueHook hook{};
    hook.queue = queue;
    hook.replacementVtable = CloneVtable<kQueueVtableEntries>(queue);
    if (!hook.replacementVtable)
        return false;
    hook.originalExecute = reinterpret_cast<ExecuteCommandListsFn>(
        hook.replacementVtable[kExecuteCommandListsIndex]);
    hook.replacementVtable[kExecuteCommandListsIndex] =
        reinterpret_cast<void*>(&HookExecuteCommandLists);
    if (!CommitVtable(queue, hook.replacementVtable))
        return false;
    gQueueHooks.push_back(std::move(hook));
    gQueueHookInstalled.store(true, std::memory_order_release);
    EmitLog(L"NGX output probe hooked a D3D12 command queue");
    return true;
}

ImmutableOutput PrepareImmutableOutput(ID3D12GraphicsCommandList* commandList,
    ID3D12Resource* originalOutput, NVSDK_NGX_Parameter* parameters,
    uint64_t batch, int count, int index, bool completesBatch)
{
    ImmutableOutput result{};
    if (!gImmutableEnabled.load(std::memory_order_acquire)
        || !commandList || !originalOutput || !parameters || batch == 0
        || count < 1 || count > 5 || index < 1 || index > count)
        return result;

    const ComPtr<IUnknown> commandListIdentity =
        ResolveNativeIdentity(commandList);
    std::lock_guard lock(gProbe.mutex);
    if (gImmutable.unsupported || !InitializeImmutable(originalOutput))
    {
        gImmutableDropped.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    ProcessCompletedImmutableSlots();

    const D3D12_RESOURCE_DESC description = originalOutput->GetDesc();
    const auto idle = [](const ImmutableSlot& candidate) {
        return !candidate.reserved && !candidate.awaitingSubmission
            && candidate.fenceValue == 0
            && candidate.streamlineFenceValue == 0
            && candidate.sequence == 0
            && candidate.reservationBatch == 0;
    };
    uint32_t slotIndex = UINT32_MAX;
    if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D
        || description.SampleDesc.Count != 1
        || (description.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0)
    {
        gImmutableDropped.fetch_add(1, std::memory_order_relaxed);
        return result;
    }

    const auto reserveMissingBatchIndices = [&]() {
        std::array<bool, 5> represented{};
        for (const auto& candidate : gImmutable.slots)
        {
            if (candidate.reservationBatch == batch
                && candidate.reservationIndex >= 1
                && candidate.reservationIndex <= count)
            {
                represented[static_cast<size_t>(
                    candidate.reservationIndex - 1)] = true;
            }
            if (candidate.batch == batch && candidate.sequence != 0
                && candidate.index >= 1 && candidate.index <= count)
            {
                represented[static_cast<size_t>(candidate.index - 1)] = true;
            }
        }

        std::array<int, 5> missingIndices{};
        uint32_t missingCount = 0;
        for (int outputIndex = 1; outputIndex <= count; ++outputIndex)
        {
            if (!represented[static_cast<size_t>(outputIndex - 1)])
                missingIndices[missingCount++] = outputIndex;
        }
        if (missingCount == 0)
            return true;

        std::array<uint32_t, 5> reservationSlots{};
        std::array<bool, kImmutableSlots> selected{};
        uint32_t reservationCount = 0;
        // Reuse storage of the same shape first, but do not allocate committed
        // 4K textures merely to hold a logical reservation.  The texture is
        // created lazily only when that output index is actually evaluated.
        for (uint32_t candidate = 0;
            candidate < gImmutable.slots.size()
                && reservationCount < missingCount;
            ++candidate)
        {
            const ImmutableSlot& candidateSlot = gImmutable.slots[candidate];
            if (!idle(candidateSlot) || !candidateSlot.texture
                || !SameDescription(candidateSlot.description, description))
                continue;
            reservationSlots[reservationCount++] = candidate;
            selected[candidate] = true;
        }
        for (uint32_t candidate = 0;
            candidate < gImmutable.slots.size()
                && reservationCount < missingCount;
            ++candidate)
        {
            if (selected[candidate] || !idle(gImmutable.slots[candidate]))
                continue;
            reservationSlots[reservationCount++] = candidate;
            selected[candidate] = true;
        }
        if (reservationCount != missingCount)
            return false;

        for (uint32_t reservation = 0;
            reservation < reservationCount; ++reservation)
        {
            ImmutableSlot& reservationSlot =
                gImmutable.slots[reservationSlots[reservation]];
            reservationSlot.reservationBatch = batch;
            reservationSlot.reservationIndex = missingIndices[reservation];
        }
        return true;
    };

    const auto currentIndexReserved = [&]() {
        return std::any_of(gImmutable.slots.begin(), gImmutable.slots.end(),
            [&](const ImmutableSlot& candidate) {
                return candidate.reservationBatch == batch
                    && candidate.reservationIndex == index;
            });
    };

    if (!currentIndexReserved() && !reserveMissingBatchIndices())
    {
        // Cyberpunk can briefly expose several handles which only evaluate
        // output 1/5 after a feature recreation.  Their unused reservations
        // contain no GPU work and must not consume the entire ring.  Reclaim
        // those logical slots under pressure and make any already-submitted
        // outputs from the partial batches eligible for normal fence/lag based
        // retirement.  If a reclaimed batch resumes later it simply reserves
        // its remaining indices again.
        std::array<uint64_t, kImmutableSlots> reclaimedBatches{};
        uint32_t reclaimedBatchCount = 0;
        uint32_t reclaimedReservations = 0;
        for (auto& candidate : gImmutable.slots)
        {
            if (candidate.reservationBatch == 0
                || candidate.reservationBatch == batch
                || candidate.reserved || candidate.awaitingSubmission
                || candidate.fenceValue != 0
                || candidate.streamlineFenceValue != 0
                || candidate.sequence != 0)
            {
                continue;
            }
            const uint64_t reclaimedBatch = candidate.reservationBatch;
            if (std::find(reclaimedBatches.begin(),
                    reclaimedBatches.begin() + reclaimedBatchCount,
                    reclaimedBatch)
                == reclaimedBatches.begin() + reclaimedBatchCount)
            {
                reclaimedBatches[reclaimedBatchCount++] = reclaimedBatch;
            }
            candidate.reservationBatch = 0;
            candidate.reservationIndex = 0;
            ++reclaimedReservations;
        }
        for (uint32_t reclaimed = 0;
            reclaimed < reclaimedBatchCount; ++reclaimed)
        {
            for (auto& candidate : gImmutable.slots)
            {
                if (candidate.batch != reclaimedBatches[reclaimed]
                    || candidate.sequence == 0 || candidate.reserved)
                    continue;
                candidate.streamlineFenceEligible = true;
                candidate.streamlineFenceBaseline =
                    gImmutable.latestStreamlineFence;
                candidate.streamlineFenceBaselineValue =
                    gImmutable.latestStreamlineFenceValue;
            }
        }
        if (reclaimedReservations != 0)
        {
            gImmutableReservationReclaims.fetch_add(
                reclaimedReservations, std::memory_order_relaxed);
            if (!gImmutableReservationReclaimLogged.exchange(
                    true, std::memory_order_acq_rel))
            {
                wchar_t message[256]{};
                swprintf_s(message,
                    L"Reclaimed %u unused immutable reservations from %u "
                    L"partial batches under ring pressure",
                    reclaimedReservations, reclaimedBatchCount);
                EmitLog(message);
            }
            ProcessCompletedImmutableSlots();
        }
        reserveMissingBatchIndices();
    }

    // Under extreme pressure preserve the current generated output even when
    // the rest of its batch cannot be reserved atomically.  Later evaluations
    // will retry the full missing-index reservation after completed work retires.
    if (!currentIndexReserved())
    {
        const auto fallback = std::find_if(gImmutable.slots.begin(),
            gImmutable.slots.end(), idle);
        if (fallback != gImmutable.slots.end())
        {
            fallback->reservationBatch = batch;
            fallback->reservationIndex = index;
        }
    }
    for (uint32_t candidate = 0;
        candidate < gImmutable.slots.size(); ++candidate)
    {
        const ImmutableSlot& candidateSlot = gImmutable.slots[candidate];
        if (candidateSlot.reservationBatch == batch
            && candidateSlot.reservationIndex == index)
        {
            slotIndex = candidate;
            break;
        }
    }
    if (slotIndex == UINT32_MAX)
    {
        if (!gImmutableExhaustionLogged.exchange(
                true, std::memory_order_acq_rel))
        {
            const uint64_t queueCompleted = gImmutable.fence
                ? gImmutable.fence->GetCompletedValue() : 0;
            const uint64_t latestStreamlineCompleted =
                gImmutable.latestStreamlineFence
                ? gImmutable.latestStreamlineFence->GetCompletedValue() : 0;
            wchar_t message[512]{};
            swprintf_s(message,
                L"Immutable pool exhausted: batch=%llu output=%d/%d complete=%u "
                L"nextSequence=%llu queueCompleted=%llu latestStreamline=%p "
                L"latestTarget=%llu latestCompleted=%llu",
                static_cast<unsigned long long>(batch), index, count,
                completesBatch ? 1u : 0u,
                static_cast<unsigned long long>(gImmutable.nextSequence),
                static_cast<unsigned long long>(queueCompleted),
                gImmutable.latestStreamlineFence.Get(),
                static_cast<unsigned long long>(
                    gImmutable.latestStreamlineFenceValue),
                static_cast<unsigned long long>(latestStreamlineCompleted));
            EmitLog(message);
            for (uint32_t diagnosticIndex = 0;
                diagnosticIndex < gImmutable.slots.size(); ++diagnosticIndex)
            {
                const ImmutableSlot& diagnosticSlot =
                    gImmutable.slots[diagnosticIndex];
                const uint64_t streamlineCompleted =
                    diagnosticSlot.streamlineFence
                    ? diagnosticSlot.streamlineFence->GetCompletedValue() : 0;
                swprintf_s(message,
                    L"Immutable slot %u: sequence=%llu batch=%llu output=%d/%d "
                    L"reservation=%llu:%d reserved=%u awaiting=%u eligible=%u "
                    L"queueTarget=%llu "
                    L"streamline=%p streamlineTarget=%llu streamlineCompleted=%llu "
                    L"baseline=%p baselineValue=%llu",
                    diagnosticIndex,
                    static_cast<unsigned long long>(diagnosticSlot.sequence),
                    static_cast<unsigned long long>(diagnosticSlot.batch),
                    diagnosticSlot.index, diagnosticSlot.count,
                    static_cast<unsigned long long>(
                        diagnosticSlot.reservationBatch),
                    diagnosticSlot.reservationIndex,
                    diagnosticSlot.reserved ? 1u : 0u,
                    diagnosticSlot.awaitingSubmission ? 1u : 0u,
                    diagnosticSlot.streamlineFenceEligible ? 1u : 0u,
                    static_cast<unsigned long long>(diagnosticSlot.fenceValue),
                    diagnosticSlot.streamlineFence.Get(),
                    static_cast<unsigned long long>(
                        diagnosticSlot.streamlineFenceValue),
                    static_cast<unsigned long long>(streamlineCompleted),
                    diagnosticSlot.streamlineFenceBaseline.Get(),
                    static_cast<unsigned long long>(
                        diagnosticSlot.streamlineFenceBaselineValue));
                EmitLog(message);
            }
        }
        gImmutableDropped.fetch_add(1, std::memory_order_relaxed);
        return result;
    }

    const uint64_t sequence = gImmutable.nextSequence + 1;
    ImmutableSlot& slot = gImmutable.slots[slotIndex];
    if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D
        || description.SampleDesc.Count != 1
        || (description.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0
        || !EnsureImmutableTexture(slot, description))
    {
        for (auto& reservationSlot : gImmutable.slots)
        {
            if (reservationSlot.reservationBatch != batch)
                continue;
            reservationSlot.reservationBatch = 0;
            reservationSlot.reservationIndex = 0;
        }
        gImmutableDropped.fetch_add(1, std::memory_order_relaxed);
        return result;
    }

    gImmutable.nextSequence = sequence;
    slot.commandList = commandList;
    slot.commandListIdentity = commandListIdentity;
    slot.fenceValue = 0;
    slot.sequence = sequence;
    slot.batch = batch;
    slot.reservationBatch = 0;
    slot.count = count;
    slot.index = index;
    slot.reservationIndex = 0;
    slot.streamlineFence.Reset();
    slot.streamlineFenceBaseline.Reset();
    slot.streamlineFenceValue = 0;
    slot.streamlineFenceBaselineValue = 0;
    slot.reserved = true;
    slot.awaitingSubmission = false;
    slot.streamlineFenceEligible = false;
    parameters->Set("DLSSG.OutputInterpolated", slot.texture.Get());

    result.resource = slot.texture.Get();
    result.sequence = sequence;
    result.slot = slotIndex;
    result.completesBatch = completesBatch;
    gImmutablePrepared.fetch_add(1, std::memory_order_relaxed);
    return result;
}

bool FinalizeImmutableOutput(ID3D12GraphicsCommandList* commandList,
    ID3D12Resource* originalOutput, const ImmutableOutput& immutable)
{
    if (!commandList || !originalOutput || !immutable
        || immutable.slot >= gImmutable.slots.size())
        return false;

    std::lock_guard lock(gProbe.mutex);
    ImmutableSlot& slot = gImmutable.slots[immutable.slot];
    if (!slot.reserved || slot.sequence != immutable.sequence
        || slot.texture.Get() != immutable.resource
        || slot.commandList != commandList)
        return false;

    D3D12_RESOURCE_BARRIER beforeCopy[3]{};
    beforeCopy[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    beforeCopy[0].UAV.pResource = immutable.resource;
    beforeCopy[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    beforeCopy[1].Transition.pResource = immutable.resource;
    beforeCopy[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    beforeCopy[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    beforeCopy[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    beforeCopy[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    beforeCopy[2].Transition.pResource = originalOutput;
    beforeCopy[2].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    beforeCopy[2].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    beforeCopy[2].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    commandList->ResourceBarrier(static_cast<UINT>(std::size(beforeCopy)), beforeCopy);
    commandList->CopyResource(originalOutput, immutable.resource);

    D3D12_RESOURCE_BARRIER afterCopy[2]{beforeCopy[1], beforeCopy[2]};
    std::swap(afterCopy[0].Transition.StateBefore,
        afterCopy[0].Transition.StateAfter);
    std::swap(afterCopy[1].Transition.StateBefore,
        afterCopy[1].Transition.StateAfter);
    commandList->ResourceBarrier(static_cast<UINT>(std::size(afterCopy)), afterCopy);

    slot.reserved = false;
    slot.awaitingSubmission = true;
    if (immutable.completesBatch)
    {
        for (auto& batchSlot : gImmutable.slots)
        {
            if (batchSlot.reservationBatch == slot.batch)
            {
                batchSlot.reservationBatch = 0;
                batchSlot.reservationIndex = 0;
            }
            // Streamline can submit each generated output before it evaluates
            // the last missing output in the batch.  Those earlier slots already
            // have a queue fence and awaitingSubmission is therefore false;
            // excluding them here strands four of five 6x outputs forever.
            // Mark every still-live slot from the completed batch eligible
            // for the next Streamline completion fence.
            if (batchSlot.batch != slot.batch || batchSlot.sequence == 0
                || batchSlot.reserved)
                continue;
            batchSlot.streamlineFenceEligible = true;
            batchSlot.streamlineFenceBaseline = gImmutable.latestStreamlineFence;
            batchSlot.streamlineFenceBaselineValue =
                gImmutable.latestStreamlineFenceValue;
        }
    }
    return true;
}

void CancelImmutableOutput(NVSDK_NGX_Parameter* parameters,
    ID3D12Resource* originalOutput, const ImmutableOutput& immutable)
{
    if (!immutable || immutable.slot >= gImmutable.slots.size())
        return;
    if (parameters && originalOutput)
        parameters->Set("DLSSG.OutputInterpolated", originalOutput);

    std::lock_guard lock(gProbe.mutex);
    ImmutableSlot& slot = gImmutable.slots[immutable.slot];
    if (!slot.reserved || slot.sequence != immutable.sequence
        || slot.texture.Get() != immutable.resource)
        return;
    const uint64_t canceledBatch = slot.batch;
    slot.commandList = nullptr;
    slot.commandListIdentity.Reset();
    slot.sequence = 0;
    slot.batch = 0;
    slot.reservationBatch = 0;
    slot.streamlineFence.Reset();
    slot.streamlineFenceBaseline.Reset();
    slot.streamlineFenceValue = 0;
    slot.streamlineFenceBaselineValue = 0;
    slot.count = 0;
    slot.index = 0;
    slot.reservationIndex = 0;
    slot.reserved = false;
    slot.awaitingSubmission = false;
    slot.streamlineFenceEligible = false;
    slot.fenceValue = 0;
    for (auto& batchSlot : gImmutable.slots)
    {
        if (batchSlot.reservationBatch == canceledBatch)
        {
            batchSlot.reservationBatch = 0;
            batchSlot.reservationIndex = 0;
        }
        if (batchSlot.batch != canceledBatch || batchSlot.sequence == 0
            || batchSlot.reserved)
            continue;
        batchSlot.streamlineFenceEligible = true;
        batchSlot.streamlineFenceBaseline = gImmutable.latestStreamlineFence;
        batchSlot.streamlineFenceBaselineValue =
            gImmutable.latestStreamlineFenceValue;
    }
    gImmutableDropped.fetch_add(1, std::memory_order_relaxed);
}

void AbandonImmutableBatch(uint64_t batch)
{
    if (batch == 0)
        return;
    std::lock_guard lock(gProbe.mutex);
    if (!gImmutable.initialized)
        return;
    for (auto& slot : gImmutable.slots)
    {
        if (slot.reservationBatch == batch)
        {
            slot.reservationBatch = 0;
            slot.reservationIndex = 0;
        }
        if (slot.batch != batch || slot.sequence == 0 || slot.reserved)
            continue;
        slot.streamlineFenceEligible = true;
        slot.streamlineFenceBaseline = gImmutable.latestStreamlineFence;
        slot.streamlineFenceBaselineValue =
            gImmutable.latestStreamlineFenceValue;
    }
}

void NotifyStreamlineCompletionFence(ID3D12Fence* fence, uint64_t value)
{
    if (!fence || value == 0)
        return;
    std::lock_guard lock(gProbe.mutex);
    const bool sameLatestFence = gImmutable.latestStreamlineFence
        && SameComIdentity(gImmutable.latestStreamlineFence.Get(), fence);
    const uint64_t previousLatestValue = gImmutable.latestStreamlineFenceValue;
    const uint64_t completedValue = fence->GetCompletedValue();
    const bool timelineReset = sameLatestFence
        && value < previousLatestValue && completedValue < previousLatestValue;
    if (timelineReset)
    {
        uint32_t reboundSlots = 0;
        for (auto& slot : gImmutable.slots)
        {
            if (slot.sequence == 0)
                continue;
            if (slot.streamlineFence
                && SameComIdentity(slot.streamlineFence.Get(), fence))
            {
                slot.streamlineFence.Reset();
                slot.streamlineFenceValue = 0;
                ++reboundSlots;
            }
            if (slot.streamlineFenceBaseline
                && SameComIdentity(slot.streamlineFenceBaseline.Get(), fence))
            {
                slot.streamlineFenceBaseline.Reset();
                slot.streamlineFenceBaselineValue = 0;
            }
        }
        wchar_t message[256]{};
        swprintf_s(message,
            L"Streamline completion fence timeline reset: previous=%llu "
            L"reported=%llu completed=%llu reboundSlots=%u",
            static_cast<unsigned long long>(previousLatestValue),
            static_cast<unsigned long long>(value),
            static_cast<unsigned long long>(completedValue), reboundSlots);
        EmitLog(message);
    }
    if (!sameLatestFence || timelineReset
        || value >= gImmutable.latestStreamlineFenceValue)
    {
        gImmutable.latestStreamlineFence = fence;
        gImmutable.latestStreamlineFenceValue = value;
    }
    for (auto& slot : gImmutable.slots)
    {
        if (slot.reserved || !slot.streamlineFenceEligible
            || slot.streamlineFenceValue != 0
            || (!slot.awaitingSubmission && slot.fenceValue == 0))
            continue;
        const bool sameBaselineFence = slot.streamlineFenceBaseline
            && SameComIdentity(slot.streamlineFenceBaseline.Get(), fence);
        if (sameBaselineFence && value <= slot.streamlineFenceBaselineValue)
            continue;
        slot.streamlineFence = fence;
        slot.streamlineFenceValue = value;
    }
    ProcessCompletedImmutableSlots();
}

void BeginFeatureRecycle()
{
    // Stop allocating replacement outputs while the old DLSS-G epoch drains.
    // Existing submitted textures remain alive until their Streamline fence is
    // complete; SetImmutableEnabled(false) only releases already-idle storage.
    SetImmutableEnabled(false);
}

FeatureDrainSnapshot ReadFeatureDrainSnapshot()
{
    FeatureDrainSnapshot snapshot{};
    std::lock_guard lock(gProbe.mutex);
    if (gImmutable.initialized)
        ProcessCompletedImmutableSlots();

    if (gImmutable.latestStreamlineFence
        && gImmutable.latestStreamlineFenceValue != 0)
    {
        snapshot.hasStreamlineFence = true;
        snapshot.streamlineFenceValue = gImmutable.latestStreamlineFenceValue;
        snapshot.streamlineFenceCompletedValue =
            gImmutable.latestStreamlineFence->GetCompletedValue();
        snapshot.streamlineFenceComplete =
            snapshot.streamlineFenceCompletedValue != UINT64_MAX
            && snapshot.streamlineFenceCompletedValue
                >= snapshot.streamlineFenceValue;
    }

    for (const auto& slot : gImmutable.slots)
    {
        if (slot.reserved || slot.reservationBatch != 0
            || slot.awaitingSubmission || slot.fenceValue != 0
            || slot.streamlineFenceValue != 0 || slot.sequence != 0)
        {
            ++snapshot.outstandingImmutableOutputs;
        }
    }
    return snapshot;
}

bool FinishFeatureRecycle()
{
    std::lock_guard lock(gProbe.mutex);
    if (gImmutable.initialized)
        ProcessCompletedImmutableSlots();
    for (const auto& slot : gImmutable.slots)
    {
        if (slot.reserved || slot.reservationBatch != 0
            || slot.awaitingSubmission || slot.fenceValue != 0
            || slot.streamlineFenceValue != 0 || slot.sequence != 0)
        {
            return false;
        }
    }

    ReleaseIdleImmutableTextures();
    gImmutable.latestStreamlineFence.Reset();
    gImmutable.latestStreamlineFenceValue = 0;
    gImmutableExhaustionLogged.store(false, std::memory_order_release);
    return true;
}

bool CaptureAfterEvaluate(ID3D12GraphicsCommandList* commandList,
    ID3D12Resource* output, const NVSDK_NGX_Handle* handle,
    uint64_t batch, uint64_t frameId, int count, int index,
    CapturedOutputKind kind)
{
    const bool indexValid = kind == CapturedOutputKind::eReal
        ? index == 0 : index >= 1 && index <= count;
    if (!gEnabled.load(std::memory_order_acquire)
        || !commandList || !output || !handle
        || count < 1 || count > 5 || !indexValid)
        return false;
    const ComPtr<IUnknown> commandListIdentity =
        ResolveNativeIdentity(commandList);
    std::lock_guard lock(gProbe.mutex);
    if (gProbe.unsupported || !Initialize(output))
        return false;
    ProcessCompletedSlots();
    if (gScheduled.load(std::memory_order_relaxed) >= kMaximumCaptures)
        return false;
    ReadbackSlot& slot = gProbe.slots[gProbe.nextSequence % gProbe.slots.size()];
    if (slot.awaitingSubmission || slot.fenceValue != 0 || !EnsureReadback(slot))
    {
        gDropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const D3D12_RESOURCE_DESC description = output->GetDesc();
    if (description.Format != gProbe.format
        || description.Width < gProbe.footprint.Footprint.Width
        || description.Height < gProbe.footprint.Footprint.Height)
        return false;

    D3D12_RESOURCE_BARRIER uav{};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = output;
    commandList->ResourceBarrier(1, &uav);
    D3D12_RESOURCE_BARRIER transition{};
    transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    transition.Transition.pResource = output;
    transition.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    transition.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    transition.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    commandList->ResourceBarrier(1, &transition);

    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = slot.readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = gProbe.footprint;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = output;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;
    const D3D12_BOX sourceBox{
        gProbe.sourceLeft, gProbe.sourceTop, 0,
        gProbe.sourceLeft + gProbe.footprint.Footprint.Width,
        gProbe.sourceTop + gProbe.footprint.Footprint.Height, 1
    };
    commandList->CopyTextureRegion(
        &destination, 0, 0, 0, &source, &sourceBox);
    std::swap(transition.Transition.StateBefore, transition.Transition.StateAfter);
    commandList->ResourceBarrier(1, &transition);

    slot.source = output;
    slot.commandList = commandList;
    slot.commandListIdentity = commandListIdentity;
    slot.sequence = gProbe.nextSequence++;
    slot.batch = batch;
    slot.frameId = frameId;
    slot.count = count;
    slot.index = index;
    slot.kind = kind;
    slot.awaitingSubmission = true;
    gScheduled.fetch_add(1, std::memory_order_relaxed);
    return true;
}

Snapshot ReadSnapshot()
{
    Snapshot snapshot{};
    snapshot.enabled = gEnabled.load(std::memory_order_relaxed);
    snapshot.immutableEnabled = gImmutableEnabled.load(std::memory_order_relaxed);
    snapshot.queueHookInstalled = gQueueHookInstalled.load(std::memory_order_relaxed);
    snapshot.scheduled = gScheduled.load(std::memory_order_relaxed);
    snapshot.submitted = gSubmitted.load(std::memory_order_relaxed);
    snapshot.captured = gCaptured.load(std::memory_order_relaxed);
    snapshot.dropped = gDropped.load(std::memory_order_relaxed);
    snapshot.completeBatches = gCompleteBatches.load(std::memory_order_relaxed);
    snapshot.duplicateBatches = gDuplicateBatches.load(std::memory_order_relaxed);
    snapshot.immutablePrepared = gImmutablePrepared.load(std::memory_order_relaxed);
    snapshot.immutableSubmitted = gImmutableSubmitted.load(std::memory_order_relaxed);
    snapshot.immutableRetired = gImmutableRetired.load(std::memory_order_relaxed);
    snapshot.immutableDropped = gImmutableDropped.load(std::memory_order_relaxed);
    snapshot.immutableReservationReclaims =
        gImmutableReservationReclaims.load(std::memory_order_relaxed);
    snapshot.immutableAllocated = gImmutableAllocated.load(std::memory_order_relaxed);
    return snapshot;
}
}
