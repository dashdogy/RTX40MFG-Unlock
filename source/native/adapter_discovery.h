#pragma once
#include <Windows.h>
#include <d3d12.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cwchar>

namespace adapter_discovery
{
// NVAPI x64 ABI, verified against the pinned SDK's nvapi.h/interface table.
// Discovery never initializes CUDA and grants no execution/publication rights.
struct Architecture { uint32_t version, architecture, implementation, revision; };
struct LogicalGpu
{
    uint32_t version;
    void* osAdapter;
    uint32_t physicalCount;
    void* physical[64];
    uint32_t reserved[8];
};
static_assert(sizeof(Architecture) == 16 && sizeof(LogicalGpu) == 568);
static_assert(offsetof(LogicalGpu, osAdapter) == 8 && offsetof(LogicalGpu, physical) == 24);
using Initialize = int(__cdecl*)();
using Enumerate = int(__cdecl*)(void**, uint32_t*);
using GetArchitecture = int(__cdecl*)(void*, Architecture*);
using GetLogical = int(__cdecl*)(void*, void**);
using GetLogicalInfo = int(__cdecl*)(void*, LogicalGpu*);
struct Api { Initialize initialize{}; Enumerate enumerate{}; GetArchitecture architecture{}; GetLogical logical{}; GetLogicalInfo info{}; };

inline bool MatchAmpere(const Api& api, const LUID& wanted) noexcept
{
    if ((!wanted.LowPart && !wanted.HighPart) || !api.initialize || !api.enumerate
        || !api.architecture || !api.logical || !api.info || api.initialize() != 0) return false;
    std::array<void*, 64> physical{};
    uint32_t count = 0, matches = 0;
    bool ampere = false;
    if (api.enumerate(physical.data(), &count) != 0 || !count || count > physical.size()) return false;
    for (uint32_t i = 0; i < count; ++i)
    {
        if (!physical[i]) return false;
        for (uint32_t j = 0; j < i; ++j) if (physical[j] == physical[i]) return false;
        void* logical = nullptr;
        LUID found{};
        LogicalGpu data{};
        data.version = sizeof(data) | (1u << 16);
        data.osAdapter = &found;
        if (api.logical(physical[i], &logical) != 0 || !logical
            || api.info(logical, &data) != 0 || data.osAdapter != &found
            || data.physicalCount != 1 || data.physical[0] != physical[i]) return false;
        if (found.LowPart != wanted.LowPart || found.HighPart != wanted.HighPart) continue;
        Architecture architecture{sizeof(Architecture) | (2u << 16), 0, 0, 0};
        if (api.architecture(physical[i], &architecture) != 0) return false;
        ++matches;
        ampere = architecture.architecture == 0x170;
    }
    return matches == 1 && ampere;
}

inline bool OwnedCode(HMODULE module, const void* code) noexcept
{
    MEMORY_BASIC_INFORMATION memory{};
    if (!code || VirtualQuery(code, &memory, sizeof(memory)) != sizeof(memory)) return false;
    const DWORD protection = memory.Protect & 0xff;
    return memory.AllocationBase == module && memory.Type == MEM_IMAGE && memory.State == MEM_COMMIT
        && !(memory.Protect & (PAGE_GUARD | PAGE_NOACCESS))
        && (protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE);
}

struct ModuleReference
{
    HMODULE module{};
    ModuleReference() noexcept = default;
    ModuleReference(const ModuleReference&) = delete;
    ModuleReference& operator=(const ModuleReference&) = delete;
    ~ModuleReference() { if (module) FreeLibrary(module); }
    bool Acquire(const void* address) noexcept
    {
        return !module && address && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(address), &module);
    }
};

// Driver discovery runs inside native initialization. Keep stack use bounded;
// an unusually long or truncated module path is unavailable, never trusted.
constexpr size_t kPathCapacity = 1024;
inline bool CanonicalPath(const wchar_t* path, std::array<wchar_t, kPathCapacity>& canonical) noexcept
{
    const size_t length = path ? wcsnlen_s(path, kPathCapacity) : 0;
    if (length < 3 || length == kPathCapacity || path[1] != L':'
        || (path[2] != L'\\' && path[2] != L'/')) return false;
    // Loaded Windows driver paths need no relative, device, or UNC syntax.
    // Reject component aliases before normalization rather than admitting an
    // unexpected loader spelling beneath a trusted directory by coincidence.
    for (size_t start = 3, end = start; start < length; start = end + 1)
    {
        end = start;
        while (end < length && path[end] != L'\\' && path[end] != L'/') ++end;
        if (end == start || (end - start == 1 && path[start] == L'.')
            || (end - start == 2 && path[start] == L'.' && path[start + 1] == L'.')) return false;
    }
    const DWORD result = GetFullPathNameW(path, static_cast<DWORD>(canonical.size()), canonical.data(), nullptr);
    return result && result < canonical.size();
}

inline bool NvapiOwnerPath(const wchar_t* systemDirectory, const wchar_t* candidate,
    bool systemStubOnly = false) noexcept
{
    std::array<wchar_t, kPathCapacity> system{}, path{};
    if (!CanonicalPath(systemDirectory, system) || !CanonicalPath(candidate, path)) return false;
    const size_t baseLength = wcslen(system.data()), pathLength = wcslen(path.data());
    if (baseLength + 1 >= pathLength || path[baseLength] != L'\\'
        || CompareStringOrdinal(system.data(), static_cast<int>(baseLength), path.data(),
            static_cast<int>(baseLength), TRUE) != CSTR_EQUAL) return false;
    const wchar_t* relative = path.data() + baseLength + 1;
    if (_wcsicmp(relative, L"nvapi64.dll") == 0) return true;
    if (systemStubOnly) return false;
    constexpr wchar_t repository[] = L"DriverStore\\FileRepository\\";
    constexpr size_t prefixLength = std::size(repository) - 1;
    if (wcslen(relative) <= prefixLength
        || _wcsnicmp(relative, repository, prefixLength) != 0) return false;
    const wchar_t* leaf = wcsrchr(relative + prefixLength, L'\\');
    return leaf && leaf > relative + prefixLength && _wcsicmp(leaf + 1, L"nvapi64_impl.dll") == 0;
}

using QueryInterface = void*(__cdecl*)(uint32_t);
inline bool OwnerFactoryMatches(HMODULE owner, uint32_t id, const void* target) noexcept
{
    if (!OwnedCode(owner, target)) return false;
    const auto query = reinterpret_cast<QueryInterface>(GetProcAddress(owner, "nvapi_QueryInterface"));
    return OwnedCode(owner, reinterpret_cast<void*>(query)) && query(id) == target;
}

inline bool IsAmpere(const LUID& luid) noexcept
{
    std::array<wchar_t, kPathCapacity> systemDirectory{}, stubPath{}, loadedPath{};
    const UINT systemLength = GetSystemDirectoryW(systemDirectory.data(), static_cast<UINT>(systemDirectory.size()));
    if (!systemLength || systemLength >= systemDirectory.size()
        || swprintf_s(stubPath.data(), stubPath.size(), L"%ls\\nvapi64.dll", systemDirectory.data()) < 0) return false;
    ModuleReference dispatcher;
    dispatcher.module = LoadLibraryExW(stubPath.data(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dispatcher.module) return false;
    const DWORD stubLength = GetModuleFileNameW(dispatcher.module, loadedPath.data(), static_cast<DWORD>(loadedPath.size()));
    if (!stubLength || stubLength >= loadedPath.size()
        || !NvapiOwnerPath(systemDirectory.data(), loadedPath.data(), true)) return false;
    const auto query = reinterpret_cast<QueryInterface>(GetProcAddress(dispatcher.module, "nvapi_QueryInterface"));
    if (!OwnedCode(dispatcher.module, reinterpret_cast<void*>(query))) return false;
    constexpr std::array<uint32_t, 5> ids{0x0150e828, 0xe5ac921f, 0xd8265d24, 0xadd604d1, 0x842b066e};
    std::array<void*, ids.size()> targets{};
    std::array<ModuleReference, ids.size()> owners{};
    for (size_t i = 0; i < ids.size(); ++i)
    {
        targets[i] = query(ids[i]);
        if (!owners[i].Acquire(targets[i])) return false;
        const DWORD ownerLength = GetModuleFileNameW(owners[i].module, loadedPath.data(), static_cast<DWORD>(loadedPath.size()));
        // Recent drivers split the trusted System32 dispatcher and its actual
        // factory-owned code in the driver store. Retain that exact owner and
        // prove both factories agree, without accepting an arbitrary RX image.
        if (!ownerLength || ownerLength >= loadedPath.size()
            || !NvapiOwnerPath(systemDirectory.data(), loadedPath.data())
            || !OwnerFactoryMatches(owners[i].module, ids[i], targets[i])
            || !OwnedCode(dispatcher.module, reinterpret_cast<void*>(query)) || query(ids[i]) != targets[i]) return false;
    }
    const Api api{reinterpret_cast<Initialize>(targets[0]), reinterpret_cast<Enumerate>(targets[1]),
        reinterpret_cast<GetArchitecture>(targets[2]), reinterpret_cast<GetLogical>(targets[3]),
        reinterpret_cast<GetLogicalInfo>(targets[4])};
    return MatchAmpere(api, luid);
}
inline bool IsAmpereD3D12Device(void* source) noexcept
{
    if (!source) return false;
    ID3D12Device* device = nullptr;
    LUID luid{};
    bool identified = false;
    __try
    {
        if (SUCCEEDED(static_cast<IUnknown*>(source)->QueryInterface(__uuidof(ID3D12Device),
                reinterpret_cast<void**>(&device))) && device)
        {
            luid = device->GetAdapterLuid();
            identified = luid.LowPart || luid.HighPart;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { identified = false; device = nullptr; }
    if (device) device->Release();
    // A probe on another family must not initialize CUDA or commit the global
    // adapter selection. Authoritative execution checks remain a later step.
    return identified && IsAmpere(luid);
}
}
