#include "dlssg_preset.h"
#include "dlssg_preset_policy.h"
#include "dlssg_provider_policy.h"
#include "entry_detour.h"

#include <intrin.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <limits>
#include <mutex>
#include <utility>

namespace dlssg_preset
{
namespace
{
using GetSetting = int32_t (__cdecl*)(void*, void*, uint32_t, Setting*);
using QueryInterface = void* (__cdecl*)(uint32_t);
constexpr uint32_t kGetSettingInterface = 0x73BF8338u;
std::atomic<GetSetting> gOriginal{nullptr};
std::atomic<bool> gInstalled{false};
std::atomic<uint32_t> gFailure{0};
std::mutex gInstallMutex;
std::mutex gReadMutex;
std::mutex gSelectionMutex;
SelectionSnapshot gSelection;
bool gSelectionInitialized = false;
std::once_flag gInitialSelectionOnce;
std::atomic<InitialSelectionLoader> gInitialSelectionLoader{nullptr};
struct ProviderRead
{
    HMODULE module = nullptr;
    uint64_t count = 0;
    uint64_t overrides = 0;
    uint32_t value = 0;
    bool valueValid = false;
};
std::array<ProviderRead, 64> gReads{};
using CachedGetSetting = bool (*)(uint32_t, uint32_t*);
struct CachedReader
{
    std::atomic<HMODULE> module{nullptr};
    std::atomic<void*> target{nullptr};
    std::atomic<uint64_t> generation{0};
    std::atomic<uint64_t> reads{0};
    std::atomic<uint32_t> failure{0};
};
std::array<CachedReader, 64> gCachedReaders{};
std::mutex gCachedInstallMutex;

void RecordRead(HMODULE provider, bool valueValid, uint32_t value, bool overridden) noexcept
{
    std::lock_guard lock(gReadMutex);
    for (auto& read : gReads)
    {
        if (read.module && read.module != provider) continue;
        read.module = provider;
        ++read.count;
        read.overrides += overridden ? 1 : 0;
        read.value = valueValid ? value : 0;
        read.valueValid = valueValid;
        break;
    }
}

HMODULE Owner(const void* address) noexcept
{
    HMODULE module = nullptr;
    return address && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(address), &module)
        ? module : nullptr;
}

bool IsDriverImplementation(HMODULE implementation) noexcept
{
    wchar_t prefix[32768]{};
    const UINT count = GetSystemDirectoryW(prefix, static_cast<UINT>(std::size(prefix)));
    if (!count || count >= std::size(prefix) - 64
        || wcscat_s(prefix, L"\\DriverStore\\FileRepository\\"))
        return false;
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(implementation, path, static_cast<DWORD>(std::size(path)));
    const wchar_t* name = length && length < std::size(path) ? wcsrchr(path, L'\\') : nullptr;
    return name && _wcsicmp(name + 1, L"nvapi64_impl.dll") == 0
        && _wcsnicmp(path, prefix, wcslen(prefix)) == 0;
}

bool Writable(const void* pointer, size_t bytes) noexcept
{
    uintptr_t cursor = reinterpret_cast<uintptr_t>(pointer);
    if (!cursor || bytes > std::numeric_limits<uintptr_t>::max() - cursor)
        return false;
    const uintptr_t end = cursor + bytes;
    while (cursor < end)
    {
        MEMORY_BASIC_INFORMATION memory{};
        if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &memory, sizeof(memory))
            || memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS))
            || !(memory.Protect & (PAGE_READWRITE | PAGE_WRITECOPY
                | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            return false;
        const uintptr_t base = reinterpret_cast<uintptr_t>(memory.BaseAddress);
        if (memory.RegionSize > std::numeric_limits<uintptr_t>::max() - base)
            return false;
        const uintptr_t next = base + memory.RegionSize;
        if (next <= cursor) return false;
        cursor = next;
    }
    return true;
}

bool OwnedExecutable(HMODULE module, const void* address, size_t size) noexcept
{
    MEMORY_BASIC_INFORMATION memory{};
    const uintptr_t value = reinterpret_cast<uintptr_t>(address);
    if (!module || !address || !size || !VirtualQuery(address, &memory, sizeof(memory))
        || memory.AllocationBase != module || memory.Type != MEM_IMAGE
        || memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS))
        || !(memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
        return false;
    const uintptr_t region = reinterpret_cast<uintptr_t>(memory.BaseAddress);
    return value >= region && value - region < memory.RegionSize
        && size <= memory.RegionSize - (value - region);
}

void* FindCachedReader(HMODULE module) noexcept
{
    __try
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000) return nullptr;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC
            || !nt->FileHeader.NumberOfSections || nt->FileHeader.NumberOfSections > 96) return nullptr;
        const auto* sections = IMAGE_FIRST_SECTION(nt);
        const size_t imageSize = nt->OptionalHeader.SizeOfImage;
        if (imageSize < 4096 || imageSize > 1024u*1024u*1024u
            || reinterpret_cast<uintptr_t>(sections + nt->FileHeader.NumberOfSections) - base > imageSize) return nullptr;
        // mov ecx,FG preset key; call bool(uint32_t,uint32_t*); test al; jz.
        constexpr uint8_t prefix[]{0xb9,0xf1,0x1d,0xe4,0x10,0xe8};
        constexpr uint8_t suffix[]{0x84,0xc0,0x0f,0x84};
        constexpr uint8_t prologue[]{0x48,0x89,0x5c,0x24,0x10,0x89,0x4c,0x24,0x08,
            0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0xfa,0x8b,0xd9};
        void* selected = nullptr;
        for (unsigned i = 0; i != nt->FileHeader.NumberOfSections; ++i)
        {
            const auto& section = sections[i];
            if (!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE) || section.VirtualAddress >= imageSize) continue;
            const size_t length = std::min<size_t>(section.Misc.VirtualSize, imageSize-section.VirtualAddress);
            const auto* start = reinterpret_cast<const uint8_t*>(base+section.VirtualAddress);
            for (size_t offset = 0; offset+18 <= length; ++offset)
            {
                const auto* call = start+offset;
                if (memcmp(call, prefix, sizeof(prefix)) || memcmp(call+10, suffix, sizeof(suffix))) continue;
                int32_t callOffset = 0, branchOffset = 0;
                memcpy(&callOffset, call+6, 4); memcpy(&branchOffset, call+14, 4);
                const uintptr_t target = reinterpret_cast<uintptr_t>(call+10)+callOffset;
                const uintptr_t branch = reinterpret_cast<uintptr_t>(call+18)+branchOffset;
                if (target < base || target-base >= imageSize || sizeof(prologue) > imageSize-(target-base)
                    || !OwnedExecutable(module, call, 18)
                    || !OwnedExecutable(module, reinterpret_cast<void*>(target), sizeof(prologue))
                    || !OwnedExecutable(module, reinterpret_cast<void*>(branch), 1)
                    || memcmp(reinterpret_cast<void*>(target), prologue, sizeof(prologue))) return nullptr;
                DWORD64 owner = 0;
                const auto* function = RtlLookupFunctionEntry(target, &owner, nullptr);
                const uintptr_t pdata = reinterpret_cast<uintptr_t>(function);
                if (!function || owner != base || pdata < base || pdata-base >= imageSize
                    || sizeof(*function) > imageSize-(pdata-base) || Owner(function) != module
                    || base+function->BeginAddress != target
                    || function->EndAddress <= function->BeginAddress || function->EndAddress > imageSize
                    || function->UnwindData >= imageSize
                    || selected) return nullptr;
                selected = reinterpret_cast<void*>(target);
            }
        }
        return selected;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

template <size_t Index>
__declspec(noinline) bool HookCachedReader(uint32_t id, uint32_t* value) noexcept
{
    const void* caller = _ReturnAddress();
    auto& reader = gCachedReaders[Index];
    HMODULE provider = reader.module.load(std::memory_order_acquire);
    const auto state = entry_detour::ReadSnapshot(entry_detour::Kind::eDlssgCachedPresetRead, provider);
    auto original = reinterpret_cast<CachedGetSetting>(state.original);
    if (!original) return false;
    if (state.current && state.generation == reader.generation.load(std::memory_order_acquire)
        && state.target == reader.target.load(std::memory_order_acquire)
        && Owner(caller) == provider && id == kSettingId && Writable(value, sizeof(*value)))
    {
        const uint32_t preset = FreezeSelection();
        const bool overridden = preset == kPresetA || preset == kPresetB;
        bool result = true;
        if (overridden) *value = preset;
        else result = original(id, value);
        reader.reads.fetch_add(1, std::memory_order_relaxed);
        const bool valueValid = result && Writable(value, sizeof(*value));
        RecordRead(provider, valueValid, valueValid ? *value : 0, overridden);
        return result;
    }
    return original(id, value);
}

template <size_t... Index>
constexpr auto CachedThunks(std::index_sequence<Index...>) noexcept
{
    return std::array<CachedGetSetting, sizeof...(Index)>{&HookCachedReader<Index>...};
}
constexpr auto kCachedThunks = CachedThunks(std::make_index_sequence<64>{});

void InstallCachedReader(HMODULE module, uint64_t generation) noexcept
{
    std::lock_guard lock(gCachedInstallMutex);
    for (size_t index = 0; index != gCachedReaders.size(); ++index)
    {
        auto& reader = gCachedReaders[index];
        if (reader.module.load(std::memory_order_acquire) == module) return;
        if (reader.module.load(std::memory_order_acquire)) continue;
        void* target = FindCachedReader(module);
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(module), &pinned)
            || pinned != module) return;
        reader.target.store(target, std::memory_order_relaxed);
        reader.generation.store(generation, std::memory_order_relaxed);
        reader.failure.store(target ? 0 : 4, std::memory_order_relaxed);
        reader.module.store(module, std::memory_order_release);
        if (!target) return; // Record unknown layout; retain the public NVAPI route.
        entry_detour::InstallOptions options{};
        options.generation = generation;
        options.allowRelocated = true;
        void* original = nullptr;
        const bool installed = entry_detour::Install(entry_detour::Kind::eDlssgCachedPresetRead, module, target,
            reinterpret_cast<void*>(kCachedThunks[index]), original, options);
        const auto state = entry_detour::ReadSnapshot(entry_detour::Kind::eDlssgCachedPresetRead, module);
        reader.failure.store(installed && state.current ? 0 : 100u+static_cast<uint32_t>(state.failure),
            std::memory_order_release);
        return;
    }
}

__declspec(noinline) int32_t __cdecl HookGetSetting(
    void* session, void* profile, uint32_t id, Setting* setting) noexcept
{
    const void* caller = _ReturnAddress();
    const auto detour = entry_detour::ReadSnapshot(entry_detour::Kind::eDlssgPresetRead);
    GetSetting original = gOriginal.load(std::memory_order_acquire);
    // The registry publishes its original pointer before enabling the entry;
    // use it for calls racing publication of the fast-path pointer.
    if (!original) original = reinterpret_cast<GetSetting>(detour.original);
    if (!original) return -1;
    const HMODULE provider = Owner(caller);
    const bool eligible = detour.current && id == kSettingId
        && dlssg_provider_policy::IsSupportedRetainedProvider(provider)
        && Writable(setting, sizeof(*setting));
    // Freeze before entering the driver. A simultaneous config reload must
    // not change which process selection this first provider query receives.
    const uint32_t preset = eligible ? FreezeSelection() : kGameOrDriver;
    const int32_t result = original(session, profile, id, setting);
    if (!eligible || !Writable(setting, sizeof(*setting)) || !CompatibleRead(id, result, *setting))
        return result;
    // The result remains owned by the calling provider. Pin before recording
    // provenance so a later load at the same address cannot inherit it.
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(caller), &pinned)
        || pinned != provider)
        return result;
    const bool overridden = OverrideRead(id, result, *setting, preset);
    const bool valueValid = overridden || result == kOk;
    RecordRead(provider, valueValid, valueValid ? setting->current.dword : 0, overridden);
    return overridden ? kOk : result;
}

void WINAPI BeforeBootstrap(void*, uintptr_t, const void*, void*,
    uintptr_t, uintptr_t, entry_detour::Handle handle, const void*) noexcept
{
    const auto state = entry_detour::ReadSnapshot(handle);
    if (state.current && dlssg_provider_policy::IsSupportedRetainedProvider(state.owner))
        Prepare();
}
}

bool SetRequested(uint32_t preset) noexcept
{
    if (!IsValidSelection(preset)) return false;
    std::lock_guard lock(gSelectionMutex);
    gSelectionInitialized = true;
    gSelection.requested = preset;
    if (!gSelection.selectionFrozen) gSelection.latched = preset;
    gSelection.restartRequired = gSelection.selectionFrozen && gSelection.requested != gSelection.latched;
    return true;
}

void SetInitialSelectionLoader(InitialSelectionLoader loader) noexcept
{
    gInitialSelectionLoader.store(loader, std::memory_order_release);
}

uint32_t FreezeSelection() noexcept
{
    std::call_once(gInitialSelectionOnce, [] {
        {
            std::lock_guard lock(gSelectionMutex);
            if (gSelectionInitialized) return;
        }
        const auto loader = gInitialSelectionLoader.load(std::memory_order_acquire);
        const uint32_t loaded = loader ? loader() : kPresetB;
        // The callback may wait on config I/O. A concurrent control update owns
        // the newer request and must win over the callback's older snapshot.
        std::lock_guard lock(gSelectionMutex);
        if (!gSelectionInitialized)
        {
            gSelection.requested = IsValidSelection(loaded) ? loaded : kPresetB;
            gSelection.latched = gSelection.requested;
            gSelectionInitialized = true;
        }
    });
    std::lock_guard lock(gSelectionMutex);
    gSelection.selectionFrozen = true;
    gSelection.restartRequired = gSelection.requested != gSelection.latched;
    return gSelection.latched;
}

SelectionSnapshot ReadSelection() noexcept
{
    std::lock_guard lock(gSelectionMutex);
    return gSelection;
}

void RecordProviderRead(HMODULE provider, bool valueValid, uint32_t value, bool overridden) noexcept
{
    if (provider) RecordRead(provider, valueValid, value, overridden);
}

bool Prepare() noexcept
{
    if (gInstalled.load(std::memory_order_acquire)) return ReadSnapshot(nullptr).installed;
    std::lock_guard lock(gInstallMutex);
    if (gInstalled.load(std::memory_order_acquire)) return ReadSnapshot(nullptr).installed;
    wchar_t path[32768]{};
    const UINT length = GetSystemDirectoryW(path, static_cast<UINT>(std::size(path)));
    if (!length || length >= std::size(path) - 32 || wcscat_s(path, L"\\nvapi64.dll"))
    {
        gFailure.store(1, std::memory_order_release);
        return false;
    }
    HMODULE module = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module)
    {
        gFailure.store(2, std::memory_order_release);
        return false;
    }
    wchar_t loaded[32768]{};
    const DWORD loadedLength = GetModuleFileNameW(module, loaded, static_cast<DWORD>(std::size(loaded)));
    const auto query = reinterpret_cast<QueryInterface>(GetProcAddress(module, "nvapi_QueryInterface"));
    void* target = loadedLength && loadedLength < std::size(loaded)
            && _wcsicmp(path, loaded) == 0 && Owner(reinterpret_cast<void*>(query)) == module
        ? query(kGetSettingInterface) : nullptr;
    const HMODULE implementation = Owner(target);
    HMODULE pinnedDispatcher = nullptr;
    HMODULE pinned = nullptr;
    if (!target || !implementation
        || (implementation != module && !IsDriverImplementation(implementation))
        || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(query), &pinnedDispatcher)
        || pinnedDispatcher != module
        || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(target), &pinned)
        || pinned != implementation)
    {
        FreeLibrary(module);
        gFailure.store(3, std::memory_order_release);
        return false;
    }
    FreeLibrary(module); // The hook's process-lifetime pin owns the image.
    entry_detour::InstallOptions options{};
    options.generation = 1; // The dispatcher and exact returned implementation are pinned.
    options.allowRelocated = true;
    void* original = nullptr;
    const bool installed = entry_detour::Install(entry_detour::Kind::eDlssgPresetRead,
        implementation, target, reinterpret_cast<void*>(&HookGetSetting), original, options);
    if (original) gOriginal.store(reinterpret_cast<GetSetting>(original), std::memory_order_release);
    const auto state = entry_detour::ReadSnapshot(entry_detour::Kind::eDlssgPresetRead);
    if (!installed || !state.current)
    {
        gFailure.store(100u + static_cast<uint32_t>(state.failure), std::memory_order_release);
        return false;
    }
    gFailure.store(0, std::memory_order_release);
    gInstalled.store(true, std::memory_order_release);
    return true;
}

void PrepareForExport(HMODULE module, const char* name) noexcept
{
    if (name && reinterpret_cast<uintptr_t>(name) > 0xFFFFu
        && std::strncmp(name, "NVSDK_NGX_", 10) == 0
        && dlssg_provider_policy::IsSupportedRetainedProvider(module))
    {
        // The host can resolve and call Init before the discovery worker sees
        // the image. Cover the cached reader at this same API boundary. Its
        // process-lifetime pin gives this private hook one immutable generation.
        InstallCachedReader(module, 1);
        Prepare();
    }
}

void ObserveProvider(HMODULE module, uint64_t generation) noexcept
{
    if (!generation || !dlssg_provider_policy::IsSupportedRetainedProvider(module))
        return;
    InstallCachedReader(module, generation);
    constexpr const char* names[] = {
        "NVSDK_NGX_D3D12_Init", "NVSDK_NGX_D3D12_Init_Ext",
        "NVSDK_NGX_D3D12_GetFeatureRequirements", "NVSDK_NGX_VULKAN_GetFeatureRequirements",
        "NVSDK_NGX_D3D12_PopulateDeviceParameters_Impl", "NVSDK_NGX_VULKAN_PopulateDeviceParameters_Impl"};
    constexpr const char* vulkanInitializers[] = {
        "NVSDK_NGX_VULKAN_Init", "NVSDK_NGX_VULKAN_Init_Ext", "NVSDK_NGX_VULKAN_Init_Ext2"};
    for (const char* name : names)
    {
        void* target = reinterpret_cast<void*>(GetProcAddress(module, name));
        if (!target || Owner(target) != module) continue;
        bool adapterEntry = false;
        for (const char* vulkanName : vulkanInitializers)
            adapterEntry |= target == reinterpret_cast<void*>(GetProcAddress(module, vulkanName));
        // Some unused legacy exports share a stub with Vulkan Init. The
        // existing Vulkan adapter pre-call prepares the preset hook as well.
        if (adapterEntry) continue;
        entry_detour::InstallOptions options{};
        options.generation = generation;
        options.allowRelocated = true;
        void* original = nullptr;
        entry_detour::InstallForwarding(entry_detour::Kind::eDlssgPresetBootstrap,
            module, target, &BeforeBootstrap, original, options);
    }
}

Snapshot ReadSnapshot(HMODULE provider) noexcept
{
    const auto state = entry_detour::ReadSnapshot(entry_detour::Kind::eDlssgPresetRead);
    Snapshot result{state.current, gFailure.load(std::memory_order_acquire), 0};
    const auto selection = ReadSelection();
    result.requested = selection.requested;
    result.latched = selection.latched;
    result.selectionFrozen = selection.selectionFrozen;
    result.restartRequired = selection.restartRequired;
    if (state.installed && !state.current)
        result.failure = 100u + static_cast<uint32_t>(entry_detour::Failure::eHookConflict);
    if (!provider) return result;
    for (const auto& reader : gCachedReaders)
    {
        if (reader.module.load(std::memory_order_acquire) != provider) continue;
        const auto cached = entry_detour::ReadSnapshot(entry_detour::Kind::eDlssgCachedPresetRead, provider);
        result.cachedReaderInstalled = cached.current
            && cached.generation == reader.generation.load(std::memory_order_acquire)
            && cached.target == reader.target.load(std::memory_order_acquire);
        result.cachedReaderFailure = reader.failure.load(std::memory_order_acquire);
        if (cached.installed && !cached.current)
            result.cachedReaderFailure = 100u+static_cast<uint32_t>(entry_detour::Failure::eHookConflict);
        result.cachedReaderRva = cached.targetRva;
        result.cachedReaderReads = reader.reads.load(std::memory_order_acquire);
        result.installed |= result.cachedReaderInstalled;
        if (result.cachedReaderInstalled) result.failure = 0;
        break;
    }
    std::lock_guard lock(gReadMutex);
    for (const auto& read : gReads)
        if (read.module == provider)
        {
            result.providerReads = read.count;
            result.overrideReads = read.overrides;
            result.observedPreset = read.value;
            result.observedPresetValid = read.valueValid;
            break;
        }
    return result;
}
}
