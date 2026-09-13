#include "fault_capture.h"
#if MFG_UNLOCK_GPU_FAULT_CAPTURE
#include "overlay_hook.h"
#include <d3d12.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstring>

namespace fault_capture
{
namespace
{
constexpr size_t kEvents = 512, kDevices = 8, kNodes = 64;
constexpr uint64_t kNormalLogEvents = 2048;
std::atomic<LogCallback> gLog{nullptr};
std::atomic<uint64_t> gCallId{0}, gDropped{0};
thread_local uint64_t gParent = 0;
SRWLOCK gEventLock = SRWLOCK_INIT, gDeviceLock = SRWLOCK_INIT;
enum class Stage : uint32_t { Begin, Return, Forward, Unwound };
struct Event
{
    uint64_t sequence = 0, id = 0, parent = 0, qpc = 0, generation = 0;
    uintptr_t owner = 0, caller = 0, command = 0, handleOrFeature = 0, parameters = 0, output = 0;
    uint32_t thread = 0, kind = 0, result = 0;
    Stage stage = Stage::Begin;
    bool outputReadable = false, callerPreserved = false;
};
std::array<Event, kEvents> gEvents{}, gCopy{}; // Worker copy stays off its stack.
uint64_t gSequence = 0, gLogged = 0;
struct Device
{
    ID3D12Device* device = nullptr;
    IUnknown* identity = nullptr;
    bool creationObserved = false, reported = false;
};
std::array<Device, kDevices> gDevices{};
std::atomic<HMODULE> gD3d12{nullptr}; // Retained for process lifetime.
std::atomic<HRESULT> gSettingsResult{E_PENDING};
INIT_ONCE gSettingsOnce = INIT_ONCE_STATIC_INIT, gWorkerOnce = INIT_ONCE_STATIC_INIT;

void Log(const wchar_t* format, ...) noexcept
{
    auto log = gLog.load(std::memory_order_acquire);
    if (!log) return;
    wchar_t line[2048]{};
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(line, _countof(line), _TRUNCATE, format, args);
    va_end(args);
    log(line);
}

template<class T> bool Read(const T* source, T& value) noexcept
{
    if (!source) return false;
    __try { value = *source; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void Name(const wchar_t* wide, const char* narrow, wchar_t (&out)[97]) noexcept
{
    size_t i = 0;
    for (; i != 96; ++i)
    {
        wchar_t c = 0;
        if (wide) { if (!Read(wide + i, c)) break; }
        else if (narrow) { char a = 0; if (!Read(narrow + i, a)) break; c = static_cast<unsigned char>(a); }
        else break;
        if (!c) break;
        out[i] = c < 32 || c == L'"' ? L'?' : c;
    }
    out[i] = 0;
}

void Add(Event event) noexcept
{
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    event.qpc = static_cast<uint64_t>(qpc.QuadPart);
    event.thread = GetCurrentThreadId();
    if (!TryAcquireSRWLockExclusive(&gEventLock))
    { gDropped.fetch_add(1, std::memory_order_relaxed); return; }
    event.sequence = ++gSequence;
    gEvents[(event.sequence - 1) % kEvents] = event;
    ReleaseSRWLockExclusive(&gEventLock);
}

Event Entry(const entry_detour::Snapshot& snapshot, const void* caller, const void* command,
    uintptr_t handleOrFeature, const void* parameters) noexcept
{
    Event event{};
    event.owner = reinterpret_cast<uintptr_t>(snapshot.owner);
    event.generation = snapshot.generation;
    event.kind = static_cast<uint32_t>(snapshot.kind);
    event.callerPreserved = snapshot.current && snapshot.forwarding;
    event.caller = reinterpret_cast<uintptr_t>(caller);
    event.command = reinterpret_cast<uintptr_t>(command);
    event.handleOrFeature = handleOrFeature;
    event.parameters = reinterpret_cast<uintptr_t>(parameters);
    return event;
}

void FlushEvents(bool failure) noexcept
{
    AcquireSRWLockExclusive(&gEventLock);
    gCopy = gEvents;
    const auto last = gSequence;
    ReleaseSRWLockExclusive(&gEventLock);
    const uint64_t oldest = last >= kEvents ? last - kEvents + 1 : 1;
    uint64_t first = failure ? oldest : std::max(oldest, gLogged + 1);
    if (failure || (gLogged < kNormalLogEvents && first > gLogged + 1))
        Log(L"GPU_CAPTURE NGX snapshot=%s oldest=%llu latest=%llu droppedContention=%llu overwritten=%llu",
            failure ? L"failure" : L"normal", oldest, last, gDropped.load(), last >= kEvents ? last - kEvents : 0);
    for (uint64_t i = first; i <= last; ++i)
    {
        if (!failure && i > kNormalLogEvents) break;
        const auto& e = gCopy[(i - 1) % kEvents];
        const wchar_t* stage = e.stage == Stage::Begin ? L"runtime-enter"
            : e.stage == Stage::Return ? L"runtime-return"
            : e.stage == Stage::Forward ? L"provider-forward-entry" : L"unwound-no-result";
        Log(L"GPU_CAPTURE NGX seq=%llu id=%llu parent=%llu tid=%u qpc=%llu stage=%s "
            L"kind=%u owner=%p generation=%llu caller=%p command=%p handleOrFeature=0x%llX "
            L"parameters=%p resultAvailable=%d result=0x%08X outputReadable=%d output=%p callerPreserved=%d",
            e.sequence, e.id, e.parent, e.thread, e.qpc, stage, e.kind,
            reinterpret_cast<void*>(e.owner), e.generation, reinterpret_cast<void*>(e.caller),
            reinterpret_cast<void*>(e.command), e.handleOrFeature, reinterpret_cast<void*>(e.parameters),
            e.stage == Stage::Return, e.result, e.outputReadable, reinterpret_cast<void*>(e.output),
            e.callerPreserved);
    }
    gLogged = last;
}

// Bounded, fault-tolerant reads: DRED nodes may contain stale object addresses.
// Never call methods on pObject, pCommandList or pCommandQueue from these lists.
void Breadcrumbs(const D3D12_AUTO_BREADCRUMB_NODE1* head) noexcept
{
    std::array<const void*, kNodes> seen{};
    size_t count = 0;
    for (; head && count < kNodes; ++count)
    {
        if (std::find(seen.begin(), seen.begin() + count, head) != seen.begin() + count)
        { Log(L"GPU_CAPTURE DRED breadcrumbs cycle=1"); break; }
        seen[count] = head;
        D3D12_AUTO_BREADCRUMB_NODE1 node{};
        if (!Read(head, node)) { Log(L"GPU_CAPTURE DRED breadcrumbs unreadableNode=%p", head); break; }
        UINT completed = 0;
        const bool readable = Read(node.pLastBreadcrumbValue, completed);
        wchar_t listName[97]{}, queueName[97]{};
        Name(node.pCommandListDebugNameW, node.pCommandListDebugNameA, listName);
        Name(node.pCommandQueueDebugNameW, node.pCommandQueueDebugNameA, queueName);
        Log(L"GPU_CAPTURE DRED breadcrumb node=%zu list=%p name=\"%s\" queue=%p queueName=\"%s\" "
            L"count=%u completedReadable=%d completed=%u", count, node.pCommandList, listName,
            node.pCommandQueue, queueName, node.BreadcrumbCount, readable, completed);
        if (readable && completed <= node.BreadcrumbCount && node.pCommandHistory)
        {
            // DRED retains the last 65536 operations of each command list.
            const uint64_t retained = node.BreadcrumbCount > 65536 ? node.BreadcrumbCount - 65536 : 0;
            const uint64_t begin = std::max(retained, completed > 8 ? uint64_t(completed - 8) : 0);
            const uint64_t end = std::min(uint64_t(node.BreadcrumbCount), begin + 32);
            for (uint64_t op = begin; op < end; ++op)
            {
                D3D12_AUTO_BREADCRUMB_OP value{};
                if (!Read(node.pCommandHistory + (op % 65536), value))
                { Log(L"GPU_CAPTURE DRED history unreadable=1"); break; }
                Log(L"GPU_CAPTURE DRED command index=%llu op=%u completedCounter=%u",
                    op, static_cast<uint32_t>(value), completed);
            }
        }
        for (UINT i = 0; node.pBreadcrumbContexts && i < std::min(node.BreadcrumbContextsCount, 32u); ++i)
        {
            D3D12_DRED_BREADCRUMB_CONTEXT context{};
            if (!Read(node.pBreadcrumbContexts + i, context)) break;
            wchar_t name[97]{}; Name(context.pContextString, nullptr, name);
            Log(L"GPU_CAPTURE DRED context index=%u text=\"%s\"", context.BreadcrumbIndex, name);
        }
        head = node.pNext;
    }
    Log(L"GPU_CAPTURE DRED breadcrumbs nodes=%zu truncated=%d", count, head != nullptr);
}

void Allocations(const D3D12_DRED_ALLOCATION_NODE1* head, const wchar_t* state) noexcept
{
    std::array<const void*, kNodes> seen{};
    size_t count = 0;
    for (; head && count < kNodes; ++count)
    {
        if (std::find(seen.begin(), seen.begin() + count, head) != seen.begin() + count)
        { Log(L"GPU_CAPTURE DRED allocation state=%s cycle=1", state); break; }
        seen[count] = head;
        D3D12_DRED_ALLOCATION_NODE1 node{};
        if (!Read(head, node)) { Log(L"GPU_CAPTURE DRED allocation state=%s unreadableNode=%p", state, head); break; }
        wchar_t name[97]{}; Name(node.ObjectNameW, node.ObjectNameA, name);
        Log(L"GPU_CAPTURE DRED allocation state=%s index=%zu type=%u object=%p name=\"%s\"",
            state, count, static_cast<uint32_t>(node.AllocationType), node.pObject, name);
        head = node.pNext;
    }
    Log(L"GPU_CAPTURE DRED allocations state=%s nodes=%zu truncated=%d", state, count, head != nullptr);
}

void Capture(Device& record, HRESULT reason) noexcept
{
    const LUID luid = record.device->GetAdapterLuid();
    Log(L"GPU_CAPTURE FAILURE device=%p identity=%p adapterLuid=%08X:%08X reason=0x%08X "
        L"creationObserved=%d settingsResult=0x%08X", record.device, record.identity,
        static_cast<uint32_t>(luid.HighPart), luid.LowPart, static_cast<uint32_t>(reason),
        record.creationObserved, static_cast<uint32_t>(gSettingsResult.load()));
    FlushEvents(true);
    ID3D12DeviceRemovedExtendedData1* dred = nullptr;
    HRESULT hr = record.device->QueryInterface(IID_PPV_ARGS(&dred));
    Log(L"GPU_CAPTURE DRED query1=0x%08X", static_cast<uint32_t>(hr));
    if (FAILED(hr) || !dred) return;
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs{};
    hr = dred->GetAutoBreadcrumbsOutput1(&breadcrumbs);
    Log(L"GPU_CAPTURE DRED breadcrumbsResult=0x%08X", static_cast<uint32_t>(hr));
    if (SUCCEEDED(hr)) Breadcrumbs(breadcrumbs.pHeadAutoBreadcrumbNode);
    ID3D12DeviceRemovedExtendedData2* dred2 = nullptr;
    hr = dred->QueryInterface(IID_PPV_ARGS(&dred2));
    Log(L"GPU_CAPTURE DRED query2=0x%08X", static_cast<uint32_t>(hr));
    if (SUCCEEDED(hr) && dred2)
    {
        D3D12_DRED_PAGE_FAULT_OUTPUT2 fault{};
        hr = dred2->GetPageFaultAllocationOutput2(&fault);
        Log(L"GPU_CAPTURE DRED deviceState=%u pageFaultResult=0x%08X VA=0x%016llX flags=%u",
            static_cast<uint32_t>(dred2->GetDeviceState()), static_cast<uint32_t>(hr), fault.PageFaultVA,
            static_cast<uint32_t>(fault.PageFaultFlags));
        if (SUCCEEDED(hr)) { Allocations(fault.pHeadExistingAllocationNode, L"existing"); Allocations(fault.pHeadRecentFreedAllocationNode, L"freed"); }
        dred2->Release();
    }
    else
    {
        D3D12_DRED_PAGE_FAULT_OUTPUT1 fault{};
        hr = dred->GetPageFaultAllocationOutput1(&fault);
        Log(L"GPU_CAPTURE DRED pageFaultResult=0x%08X VA=0x%016llX", static_cast<uint32_t>(hr), fault.PageFaultVA);
        if (SUCCEEDED(hr)) { Allocations(fault.pHeadExistingAllocationNode, L"existing"); Allocations(fault.pHeadRecentFreedAllocationNode, L"freed"); }
    }
    dred->Release();
    Log(L"GPU_CAPTURE END device=%p", record.device);
}

BOOL CALLBACK Configure(PINIT_ONCE, PVOID, PVOID*) noexcept
{
    HMODULE module = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    gD3d12.store(module, std::memory_order_release);
    using GetInterface = HRESULT (WINAPI*)(REFIID, void**);
    auto get = module ? reinterpret_cast<GetInterface>(GetProcAddress(module, "D3D12GetDebugInterface")) : nullptr;
    ID3D12DeviceRemovedExtendedDataSettings* settings = nullptr;
    const HRESULT hr = get ? get(IID_PPV_ARGS(&settings)) : E_NOINTERFACE;
    if (SUCCEEDED(hr) && settings)
    {
        settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        ID3D12DeviceRemovedExtendedDataSettings1* settings1 = nullptr;
        if (SUCCEEDED(settings->QueryInterface(IID_PPV_ARGS(&settings1))) && settings1)
        { settings1->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON); settings1->Release(); }
        settings->Release();
    }
    gSettingsResult.store(SUCCEEDED(hr) && !settings ? E_POINTER : hr, std::memory_order_release);
    Log(L"GPU_CAPTURE DRED settings=0x%08X breadcrumbs=forced pageFault=forced debugLayer=0 gpuValidation=0",
        static_cast<uint32_t>(gSettingsResult.load()));
    return TRUE;
}

using CreateFn = HRESULT (WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
struct CreateTag
{
    static HRESULT Call(CreateFn original, IUnknown* adapter, D3D_FEATURE_LEVEL level,
        REFIID iid, void** output) noexcept
    {
        InitOnceExecuteOnce(&gSettingsOnce, Configure, nullptr, nullptr);
        const HRESULT result = original ? original(adapter, level, iid, output) : E_POINTER;
        void* created = nullptr;
        if (SUCCEEDED(result) && Read(output, created) && created) ObserveDevice(created, true);
        Log(L"GPU_CAPTURE D3D12CreateDevice result=0x%08X adapter=%p level=0x%X output=%p "
            L"settingsBeforeCall=0x%08X", static_cast<uint32_t>(result), adapter,
            static_cast<uint32_t>(level), created, static_cast<uint32_t>(gSettingsResult.load()));
        return result;
    }
};
using CreateHook = single_overlay::HookFamily<CreateTag, HRESULT, IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**>;

void InstallCreateHook() noexcept
{
    auto module = gD3d12.load(std::memory_order_acquire);
    void* entry = module ? reinterpret_cast<void*>(GetProcAddress(module, "D3D12CreateDevice")) : nullptr;
    if (entry) CreateHook::Bind(entry, true);
    Log(L"GPU_CAPTURE D3D12CreateDevice hook=%d entry=%p", CreateHook::Installed(entry), entry);
}

DWORD WINAPI Worker(void*) noexcept
{
    LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
    Log(L"GPU_CAPTURE START version=1 pid=%u qpcFrequency=%lld events=%zu normalLogLimit=%llu devices=%zu "
        L"retention=process-lifetime scope=D3D12-Ada-runtime-results providerResults=entry-only",
        GetCurrentProcessId(), frequency.QuadPart, kEvents, kNormalLogEvents, kDevices);
    InitOnceExecuteOnce(&gSettingsOnce, Configure, nullptr, nullptr);
    InstallCreateHook();
    for (;;)
    {
        Sleep(100);
        FlushEvents(false);
        // Only this worker sets reported. Device pointers retain COM ownership;
        // no driver calls or logging occur while the device table is locked.
        for (size_t i = 0; i < kDevices; ++i)
        {
            AcquireSRWLockShared(&gDeviceLock);
            Device copy = gDevices[i];
            ReleaseSRWLockShared(&gDeviceLock);
            if (!copy.device || copy.reported) continue;
            const HRESULT reason = copy.device->GetDeviceRemovedReason();
            if (SUCCEEDED(reason)) continue;
            AcquireSRWLockExclusive(&gDeviceLock);
            gDevices[i].reported = true;
            ReleaseSRWLockExclusive(&gDeviceLock);
            Capture(copy, reason);
        }
    }
}

BOOL CALLBACK StartWorker(PINIT_ONCE, PVOID, PVOID*) noexcept
{
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&Worker), &owner)) return FALSE;
    HANDLE thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (!thread) { Log(L"GPU_CAPTURE workerStartFailed=%u", GetLastError()); return FALSE; }
    CloseHandle(thread);
    return TRUE;
}
}

void Initialize(LogCallback log) noexcept
{
    gLog.store(log, std::memory_order_release);
    InitOnceExecuteOnce(&gWorkerOnce, StartWorker, nullptr, nullptr);
}
void BeforeSlInit() noexcept
{
    // Called outside DllMain. Device creation remains the authoritative ordering
    // observation; late discovery never claims retroactive DRED coverage.
    InitOnceExecuteOnce(&gSettingsOnce, Configure, nullptr, nullptr);
    InstallCreateHook();
}
FARPROC ResolveProc(HMODULE module, LPCSTR name, FARPROC resolved) noexcept
{
    if (resolved && module && module == gD3d12.load(std::memory_order_acquire)
        && reinterpret_cast<uintptr_t>(name) > 0xffff && std::strcmp(name, "D3D12CreateDevice") == 0)
        return reinterpret_cast<FARPROC>(CreateHook::Bind(reinterpret_cast<void*>(resolved), true));
    return resolved;
}
namespace
{
bool DeviceInterfaces(void* source, ID3D12Device*& device, IUnknown*& identity) noexcept
{
    __try
    {
        return SUCCEEDED(static_cast<IUnknown*>(source)->QueryInterface(IID_PPV_ARGS(&device))) && device
            && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&identity))) && identity;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}
void ObserveDevice(void* source, bool creationObserved) noexcept
{
    if (!source) return;
    ID3D12Device* device = nullptr;
    IUnknown* identity = nullptr;
    if (!DeviceInterfaces(source, device, identity))
    { if (identity) identity->Release(); if (device) device->Release(); return; }
    size_t slot = kDevices;
    bool retained = false;
    AcquireSRWLockExclusive(&gDeviceLock);
    for (size_t i = 0; i < kDevices; ++i)
        if (gDevices[i].identity == identity)
        { slot = i; gDevices[i].creationObserved |= creationObserved; break; }
    if (slot == kDevices)
        for (size_t i = 0; i < kDevices; ++i)
            if (!gDevices[i].device)
            { slot = i; gDevices[i] = {device, identity, creationObserved, false}; retained = true; break; }
    ReleaseSRWLockExclusive(&gDeviceLock);
    if (retained) Log(L"GPU_CAPTURE device=%p identity=%p slot=%zu creationObserved=%d", device, identity, slot, creationObserved);
    else { identity->Release(); device->Release(); }
    if (slot == kDevices) Log(L"GPU_CAPTURE deviceCapacityExceeded=1");
}

NgxCall::NgxCall(entry_detour::Handle entry, const void* caller, const void* command,
    uintptr_t handleOrFeature, const void* parameters) noexcept
{
    id_ = gCallId.fetch_add(1, std::memory_order_relaxed) + 1;
    parent_ = gParent;
    entry_ = entry_detour::ReadSnapshot(entry);
    caller_ = caller; command_ = command; parameters_ = parameters; handleOrFeature_ = handleOrFeature;
    auto event = Entry(entry_, caller_, command_, handleOrFeature_, parameters_);
    event.id = id_; event.parent = parent_; event.stage = Stage::Begin;
    Add(event);
    gParent = id_;
}
void NgxCall::Complete(uint32_t result, void* const* output) noexcept
{
    auto event = Entry(entry_, caller_, command_, handleOrFeature_, parameters_);
    event.id = id_; event.parent = parent_; event.stage = Stage::Return; event.result = result;
    // NVSDK_NGX_Result_Success == 1. Never dereference an output on failure.
    void* created = nullptr;
    if (result == 1 && output)
    { event.outputReadable = Read(output, created); event.output = reinterpret_cast<uintptr_t>(created); }
    Add(event); completed_ = true;
}
NgxCall::~NgxCall()
{
    if (!completed_) { Event event{}; event.id = id_; event.parent = parent_; event.stage = Stage::Unwound; Add(event); }
    gParent = parent_;
}
void ProviderForward(entry_detour::Handle entry, const void* caller, const void* command,
    uintptr_t handleOrFeature, const void* parameters) noexcept
{
    const auto snapshot = entry_detour::ReadSnapshot(entry);
    const bool release = snapshot.kind == entry_detour::Kind::eAmpereRelease;
    auto event = Entry(snapshot, caller, release ? nullptr : command,
        release ? reinterpret_cast<uintptr_t>(command) : handleOrFeature, release ? nullptr : parameters);
    event.id = gCallId.fetch_add(1, std::memory_order_relaxed) + 1;
    event.parent = gParent; event.stage = Stage::Forward; Add(event);
}
}
#elif MFG_UNLOCK_NGX_CREATE_RESULT_DIAGNOSTICS
#include "ngx_create_result_lite.inc"
#endif
