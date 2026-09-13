#include "single_module.h"
#include "build_variant.h"
#include "overlay_build.h"
#include "overlay_install.h"
#include "single_overlay.h"
#include "overlay_dxgi_proxy.h"
#include "proxy_generated/proxy_export_names.h"
#include "proxy_generated/renamable_names.h"

#include <delayimp.h>
#include <atomic>
#include <array>
#include <cwchar>
#include <cstring>

namespace
{
HMODULE gSelf = nullptr;
uint32_t gSelectedProxyFamily = UINT32_MAX;
HANDLE gLifetimeClaim = nullptr;
bool gOwnsBackend = false;
bool gDuplicate = false;
std::array<std::atomic<HMODULE>, kMfgProxyFamilyCount> gOriginalModules{};
std::atomic<FARPROC> gForwardTargets[kMfgProxyFamilyCount][kMfgProxyOrdinalLimit]{};
thread_local uint32_t gLoadingFamilies = 0;
thread_local uint32_t gCurrentLoadingFamily = UINT32_MAX;
thread_local const wchar_t* gLoadingPaths[kMfgProxyFamilyCount]{};
static_assert(kMfgProxyFamilyCount <= 32);

constexpr size_t kLongPathCapacity = 32768;

class LongPath
{
public:
    LongPath() noexcept
        : value_(static_cast<wchar_t*>(VirtualAlloc(nullptr,
            kLongPathCapacity * sizeof(wchar_t), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)))
    {}
    ~LongPath() noexcept
    {
        if (value_) VirtualFree(value_, 0, MEM_RELEASE);
    }
    LongPath(const LongPath&) = delete;
    LongPath& operator=(const LongPath&) = delete;
    explicit operator bool() const noexcept { return value_ != nullptr; }
    wchar_t* get() const noexcept { return value_; }

private:
    wchar_t* value_ = nullptr;
};

class ScopedFamilyLoad
{
public:
    explicit ScopedFamilyLoad(uint32_t family) noexcept
        : bit_(1u << family), previous_(gCurrentLoadingFamily)
    {
        gLoadingFamilies |= bit_;
        gCurrentLoadingFamily = family;
    }
    ~ScopedFamilyLoad() noexcept
    {
        gCurrentLoadingFamily = previous_;
        gLoadingFamilies &= ~bit_;
    }
    ScopedFamilyLoad(const ScopedFamilyLoad&) = delete;
    ScopedFamilyLoad& operator=(const ScopedFamilyLoad&) = delete;

private:
    uint32_t bit_;
    uint32_t previous_;
};

class ScopedLoadingPath
{
public:
    explicit ScopedLoadingPath(const wchar_t* path) noexcept
        : family_(gCurrentLoadingFamily)
    {
        if (family_ < kMfgProxyFamilyCount)
        {
            previous_ = gLoadingPaths[family_];
            gLoadingPaths[family_] = path;
        }
    }
    ~ScopedLoadingPath() noexcept
    {
        if (family_ < kMfgProxyFamilyCount)
            gLoadingPaths[family_] = previous_;
    }
    ScopedLoadingPath(const ScopedLoadingPath&) = delete;
    ScopedLoadingPath& operator=(const ScopedLoadingPath&) = delete;

private:
    uint32_t family_;
    const wchar_t* previous_ = nullptr;
};

constexpr const wchar_t* kLegacyCoreNames[] = {
    L"RTXMFGCore.dll", L"RTX40MFGCore.dll", L"RTX30MFGCore.dll",
    L"RTX30FGCore.dll", L"RTX30FG3xCore.dll"
};

bool LegacyInstallConflict(const wchar_t* modulePath) noexcept
{
    LongPath scratch;
    if (!scratch) return true;
    wchar_t* const path = scratch.get();
    if (wcscpy_s(path, kLongPathCapacity, modulePath)) return true;
    wchar_t* leaf = wcsrchr(path, L'\\');
    if (!leaf) return true;
    const size_t rootLength = static_cast<size_t>(leaf + 1 - path);

    // v1.2 does not participate in the process claim. A loaded-module check
    // alone therefore permits a later ASI import to activate a second backend.
    // Check only the documented installation roots and conventional ASI
    // subdirectories, before any core or overlay entry. Do not load, inspect
    // exports from, rename, or delete a legacy DLL to decide this policy.
    for (const wchar_t* directory : {L"", L"plugins\\", L"scripts\\", L"asi\\"})
    {
        for (const wchar_t* name : kLegacyCoreNames)
        {
            path[rootLength] = L'\0';
            if (wcscat_s(path, kLongPathCapacity, directory)
                || wcscat_s(path, kLongPathCapacity, name)) return true;
            const DWORD attributes = GetFileAttributesW(path);
            const DWORD error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
            if ((attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY))
                || (attributes == INVALID_FILE_ATTRIBUTES
                    && error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND))
            {
                single_module::Log(L"Mixed or unreadable legacy installation: new backend and menu disabled. "
                    L"Close the game and move the old core/ASI/UI files out of its loader directories.");
                single_module::Log(path);
                return true;
            }
        }
    }
    return false;
}

bool HasLegacyConflict(HMODULE instance, const wchar_t* modulePath) noexcept
{
    for (const wchar_t* name : kLegacyCoreNames)
    {
        HMODULE legacy = GetModuleHandleW(name);
        if (legacy && legacy != instance && GetProcAddress(legacy, "MfgUnlockCoreLoaded"))
        {
            single_module::Log(L"Loaded legacy core retained; new backend and menu disabled.");
            single_module::Log(name);
            return true;
        }
    }
    if (LegacyInstallConflict(modulePath)) return true;

    // An explicitly loaded neutral DLL or ASI can live below the executable.
    // Also cover the executable's normal v1.2 installation and ASI directories.
    LongPath executableScratch;
    if (!executableScratch)
    {
        single_module::Log(L"Executable installation directory allocation failed; new backend and menu disabled.");
        return true;
    }
    wchar_t* const executablePath = executableScratch.get();
    const DWORD length = GetModuleFileNameW(nullptr, executablePath,
        static_cast<DWORD>(kLongPathCapacity));
    if (!length || length >= kLongPathCapacity)
    {
        single_module::Log(L"Executable installation directory unavailable; new backend and menu disabled.");
        return true;
    }
    const wchar_t* executableLeaf = wcsrchr(executablePath, L'\\');
    const wchar_t* moduleLeaf = wcsrchr(modulePath, L'\\');
    if (executableLeaf && moduleLeaf && executableLeaf - executablePath == moduleLeaf - modulePath
        && _wcsnicmp(executablePath, modulePath, executableLeaf - executablePath) == 0)
        return false;
    return LegacyInstallConflict(executablePath);
}

bool SelectProxyExports(HMODULE module, uint32_t family) noexcept
{
    // Customize only this loaded copy's own export-address table before any
    // importer can call it. The file stays identical under every supported name.
    // Matching name/ordinal exports share one thunk, and unrelated family names
    // have a null RVA so GetProcAddress reports them as absent.
    const auto base = reinterpret_cast<BYTE*>(module);
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 4096)
        return false;
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64
        || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
    const uint32_t size = nt->OptionalHeader.SizeOfImage;
    const auto directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (directory.VirtualAddress >= size || directory.Size < sizeof(IMAGE_EXPORT_DIRECTORY)
        || directory.Size > size - directory.VirtualAddress) return false;
    const auto exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + directory.VirtualAddress);
    constexpr uint32_t kLegacyLimit = 427, kNamedBase = 1024, kPublicLast = 4098;
    if (exports->Base != 1 || exports->NumberOfFunctions != kPublicLast
        || exports->AddressOfFunctions >= size
        || exports->NumberOfFunctions > (size - exports->AddressOfFunctions) / sizeof(DWORD)
        || (exports->AddressOfFunctions & 3u)) return false;
    auto table = reinterpret_cast<DWORD*>(base + exports->AddressOfFunctions);
    std::array<DWORD, kLegacyLimit> original{};
    for (uint32_t i = 0; i < kLegacyLimit; ++i)
    {
        const DWORD rva = table[i];
        if (!rva || rva >= size || (rva >= directory.VirtualAddress
            && rva - directory.VirtualAddress < directory.Size)) return false;
        original[i] = rva;
    }
    DWORD protection = 0;
    const SIZE_T bytes = exports->NumberOfFunctions * sizeof(DWORD);
    if (!VirtualProtect(table, bytes, PAGE_READWRITE, &protection)) return false;
    for (uint32_t ordinal = 1; ordinal <= kLegacyLimit; ++ordinal)
        table[ordinal - 1] = MfgProxyKnownOrdinal(family, ordinal) ? original[ordinal - 1] : 0;
    for (uint32_t index = 0; index < kMfgRenamedNameCount; ++index)
    {
        const uint32_t ordinal = family < kMfgProxyFamilyCount ? kMfgRenamedOrdinals[family][index] : 0;
        table[kNamedBase + index - 1] = ordinal && ordinal <= kLegacyLimit ? original[ordinal - 1] : 0;
    }
    bool verified = true;
    for (uint32_t ordinal = 1; ordinal <= kLegacyLimit; ++ordinal)
        verified &= table[ordinal - 1] == (MfgProxyKnownOrdinal(family, ordinal) ? original[ordinal - 1] : 0);
    for (uint32_t index = 0; index < kMfgRenamedNameCount; ++index)
    {
        const uint32_t ordinal = family < kMfgProxyFamilyCount ? kMfgRenamedOrdinals[family][index] : 0;
        verified &= table[kNamedBase + index - 1] == (ordinal && ordinal <= kLegacyLimit ? original[ordinal - 1] : 0);
    }
    DWORD ignored = 0;
    const bool restored = VirtualProtect(table, bytes, protection, &ignored) != FALSE;
    MEMORY_BASIC_INFORMATION memory{};
    return verified && restored && VirtualQuery(table, &memory, sizeof(memory))
        && memory.AllocationBase == module && memory.Protect == protection;
}

HMODULE LoadChainedOriginal(const wchar_t* path) noexcept
{
    ScopedLoadingPath loadingPath(path);
    HMODULE module = LoadLibraryExW(path, nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module)
        return nullptr;
    LongPath loadedScratch;
    if (!loadedScratch)
    {
        FreeLibrary(module);
        return nullptr;
    }
    wchar_t* const loaded = loadedScratch.get();
    const DWORD length = GetModuleFileNameW(module, loaded, static_cast<DWORD>(kLongPathCapacity));
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        reinterpret_cast<const BYTE*>(module) + dos->e_lfanew);
    // The OS loader has already validated the PE image. Reject another copy of
    // this payload so a mistakenly renamed proxy cannot form a forwarding loop.
    if (module == gSelf || !length || length >= kLongPathCapacity
        || _wcsicmp(path, loaded) != 0 || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64
        || GetProcAddress(module, "MfgUnlockSingleModuleQuery"))
    {
        FreeLibrary(module);
        return nullptr;
    }
    HMODULE pinned = nullptr;
    const bool valid = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
        | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(module), &pinned)
        && pinned == module;
    FreeLibrary(module);
    return valid ? module : nullptr;
}

HMODULE LoadProxyOriginal(uint32_t family) noexcept
{
    if (family >= kMfgProxyFamilyCount)
        return nullptr;
    HMODULE module = gOriginalModules[family].load(std::memory_order_acquire);
    if (module)
        return module;
    const auto& spec = kMfgProxyFamilies[family];
    LongPath pathScratch;
    if (!pathScratch)
        return nullptr;
    wchar_t* const path = pathScratch.get();
    const DWORD length = GetModuleFileNameW(gSelf, path, static_cast<DWORD>(kLongPathCapacity));
    if (!length || length >= kLongPathCapacity)
        return nullptr;
    wchar_t* leaf = wcsrchr(path, L'\\');
    if (!leaf || wcscpy_s(leaf + 1, kLongPathCapacity - (leaf + 1 - path), spec.chained))
        return nullptr;
    const DWORD attributes = GetFileAttributesW(path);
    HMODULE candidate = nullptr;
    if (attributes != INVALID_FILE_ATTRIBUTES)
    {
        // A present but invalid original is an error, never a silent fallback.
        if (!(attributes & FILE_ATTRIBUTE_DIRECTORY))
            candidate = LoadChainedOriginal(path);
    }
    else if (const DWORD error = GetLastError();
        spec.systemFallback && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND))
    {
        candidate = single_module::LoadSystemModule(spec.original);
    }
    if (!candidate)
        return nullptr;
    HMODULE expected = nullptr;
    if (gOriginalModules[family].compare_exchange_strong(expected, candidate,
        std::memory_order_release, std::memory_order_acquire))
        return candidate;
    return expected;
}

bool PinCallable(FARPROC target) noexcept
{
    MEMORY_BASIC_INFORMATION memory{};
    if (!target || !VirtualQuery(reinterpret_cast<const void*>(target), &memory, sizeof(memory))
        || memory.State != MEM_COMMIT || memory.Type != MEM_IMAGE || memory.AllocationBase == gSelf
        || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return false;
    const DWORD access = memory.Protect & 0xff;
    if (access != PAGE_EXECUTE && access != PAGE_EXECUTE_READ
        && access != PAGE_EXECUTE_READWRITE && access != PAGE_EXECUTE_WRITECOPY)
        return false;
    HMODULE owner = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(target), &owner) && owner == memory.AllocationBase;
}

FARPROC ResolveRecursiveTarget(uint32_t family, uint32_t ordinal) noexcept
{
    if (family >= kMfgProxyFamilyCount || !gLoadingPaths[family])
        return nullptr;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        gLoadingPaths[family], &module) || !module || module == gSelf)
        return nullptr;
    LongPath loadedScratch;
    if (!loadedScratch)
        return nullptr;
    wchar_t* const loaded = loadedScratch.get();
    const DWORD length = GetModuleFileNameW(module, loaded, static_cast<DWORD>(kLongPathCapacity));
    const auto base = reinterpret_cast<const BYTE*>(module);
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (!length || length >= kLongPathCapacity
        || _wcsicmp(gLoadingPaths[family], loaded) != 0
        || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 4096)
        return nullptr;
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64
        || GetProcAddress(module, "MfgUnlockSingleModuleQuery"))
        return nullptr;
    const char* name = MfgProxyExportName(family, ordinal);
    FARPROC target = GetProcAddress(module, name ? name : MAKEINTRESOURCEA(ordinal));
    MEMORY_BASIC_INFORMATION memory{};
    if (!target || !VirtualQuery(reinterpret_cast<const void*>(target), &memory, sizeof(memory))
        || memory.State != MEM_COMMIT || memory.Type != MEM_IMAGE || memory.AllocationBase != module
        || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return nullptr;
    const DWORD access = memory.Protect & 0xff;
    return access == PAGE_EXECUTE || access == PAGE_EXECUTE_READ
        || access == PAGE_EXECUTE_READWRITE || access == PAGE_EXECUTE_WRITECOPY ? target : nullptr;
}

FARPROC WINAPI DelayImportHook(unsigned event, PDelayLoadInfo info)
{
    if (event != dliNotePreLoadLibrary || !info || !info->szDll)
        return nullptr;
    // The version proxy must never import itself to read a provider version.
    // The documented delay-load notification supplies the explicit System32
    // image for these ordinary Windows APIs in every filename variant.
    if (_stricmp(info->szDll, "version.dll") == 0)
        return reinterpret_cast<FARPROC>(single_module::LoadSystemModule(L"version.dll"));
    if (_stricmp(info->szDll, "d3dcompiler_47.dll") == 0)
        return reinterpret_cast<FARPROC>(single_module::LoadSystemModule(L"d3dcompiler_47.dll"));
    if (_stricmp(info->szDll, "dxgi.dll") == 0)
        return reinterpret_cast<FARPROC>(single_module::LoadSystemModule(L"dxgi.dll"));
    return nullptr;
}

}

extern "C" const PfnDliHook __pfnDliNotifyHook2 = &DelayImportHook;

namespace single_module
{
HMODULE Self() noexcept { return gSelf; }
bool OwnsBackend() noexcept { return gOwnsBackend; }

HMODULE LoadSystemModule(const wchar_t* basename) noexcept
{
    if (!basename || wcschr(basename, L'\\') || wcschr(basename, L'/'))
        return nullptr;
    LongPath pathScratch;
    if (!pathScratch)
        return nullptr;
    wchar_t* const path = pathScratch.get();
    const UINT count = GetSystemDirectoryW(path, static_cast<UINT>(kLongPathCapacity));
    if (!count || count >= kLongPathCapacity - 256)
        return nullptr;
    if (wcscat_s(path, kLongPathCapacity, L"\\")
        || wcscat_s(path, kLongPathCapacity, basename))
        return nullptr;
    ScopedLoadingPath loadingPath(path);
    HMODULE module = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module || module == gSelf)
    {
        if (module) FreeLibrary(module);
        return nullptr;
    }
    LongPath loadedScratch;
    if (!loadedScratch)
    {
        FreeLibrary(module);
        return nullptr;
    }
    wchar_t* const loaded = loadedScratch.get();
    const DWORD length = GetModuleFileNameW(module, loaded,
        static_cast<DWORD>(kLongPathCapacity));
    if (!length || length >= kLongPathCapacity || _wcsicmp(path, loaded) != 0)
    {
        FreeLibrary(module);
        return nullptr;
    }
    // Original entry pointers can be cached by the game for its whole life.
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(module), &pinned) || pinned != module)
    {
        FreeLibrary(module);
        return nullptr;
    }
    FreeLibrary(module); // The permanent pin owns the lifetime now.
    return module;
}

namespace {
struct UiLogLine { std::atomic<bool> ready{false}; wchar_t text[512]{}; };
UiLogLine gUiLog[64];
std::atomic<uint32_t> gUiLogNext{0};
uint32_t gUiLogRead = 0; // Only the existing backend worker drains this queue.
}
void DrainLog(void (*sink)(const wchar_t*)) noexcept
{
    while (gUiLogRead < std::size(gUiLog) && gUiLog[gUiLogRead].ready.load(std::memory_order_acquire))
        sink(gUiLog[gUiLogRead++].text);
}
void Log(const wchar_t* text) noexcept
{
    const auto index = gUiLogNext.fetch_add(1, std::memory_order_relaxed);
    if (index < std::size(gUiLog))
    {
        wcsncpy_s(gUiLog[index].text, text ? text : L"(null)", _TRUNCATE);
        gUiLog[index].ready.store(true, std::memory_order_release);
    }
    OutputDebugStringW(L"[" MFG_PRODUCT_W L" single] ");
    OutputDebugStringW(text ? text : L"(null)");
    OutputDebugStringW(L"\n");
}
}

extern "C" FARPROC WINAPI MfgProxyResolve(uint32_t family, uint32_t ordinal) noexcept
{
    const DWORD savedError = GetLastError();
    if (family < kMfgProxyFamilyCount && ordinal < kMfgProxyOrdinalLimit)
    {
        FARPROC target = gForwardTargets[family][ordinal].load(std::memory_order_acquire);
        const uint32_t bit = 1u << family;
        if (!target && (gLoadingFamilies & bit))
            target = ResolveRecursiveTarget(family, ordinal);
        if (!target && !(gLoadingFamilies & bit))
        {
            // Never hold a proxy-owned gate around LoadLibraryExW. Independent
            // attempts may converge on the same pinned module and callable;
            // a caller already holding the loader lock can always progress.
            ScopedFamilyLoad loading(family);
            if (HMODULE module = LoadProxyOriginal(family))
            {
                const char* name = MfgProxyExportName(family, ordinal);
                target = GetProcAddress(module, name ? name : MAKEINTRESOURCEA(ordinal));
                if (!PinCallable(target))
                    target = nullptr;
                if (target)
                {
                    FARPROC expected = nullptr;
                    if (!gForwardTargets[family][ordinal].compare_exchange_strong(expected, target,
                        std::memory_order_release, std::memory_order_acquire))
                        target = expected;
                }
            }
        }
        if (target)
        {
            SetLastError(savedError);
            return target;
        }
        wchar_t message[512]{};
        swprintf_s(message, L"Cannot forward %s export #%u. Check the original %s beside this loader%s.",
            kMfgProxyFamilies[family].original, ordinal, kMfgProxyFamilies[family].chained,
            kMfgProxyFamilies[family].systemFallback ? L" or the Windows System32 DLL" : L" (required)");
        single_module::Log(message);
    }
    const ULONG_PTR detail[] = { family, ordinal };
    RaiseException(0xC0000139u, EXCEPTION_NONCONTINUABLE, 2, detail);
    return nullptr;
}

extern "C" FARPROC WINAPI MfgProxyResolveRenamed(uint32_t kind, uint32_t key) noexcept
{
    const uint32_t family = gSelectedProxyFamily;
    uint32_t ordinal = 0;
    if (family < kMfgProxyFamilyCount)
    {
        if (kind == 0 && MfgProxyKnownOrdinal(family, key)) ordinal = key;
        else if (kind == 1 && key < kMfgRenamedNameCount) ordinal = kMfgRenamedOrdinals[family][key];
    }
    // Unsupported names/ordinals never resolve against a different DLL family.
    return MfgProxyResolve(ordinal ? family : UINT32_MAX, ordinal);
}

extern "C" __declspec(dllexport) BOOL WINAPI
MfgUnlockSingleModuleQuery(MfgSingleModuleStatus* status)
{
    // Version-1 consumers pass the 64-byte layout and receive populated
    // version-1 fields; version-2 consumers also receive the bounded
    // coordinator state. Any other size is rejected as before.
    if (!status || (status->size != sizeof(*status)
            && status->size != kMfgSingleModuleStatusSizeV1
            && status->size != kMfgSingleModuleStatusSizeV2))
        return FALSE;
    const uint32_t requested = status->size;
    MfgSingleModuleStatus value{};
    value.size = requested;
    value.version = requested == kMfgSingleModuleStatusSizeV2 ? 2 : 3;
    value.backendOwner = gOwnsBackend;
    value.duplicateSuppressed = gDuplicate;
    single_overlay::ReadStatus(value);
    single_overlay::proxy::ReadStatus(value);
    value.overlayInstallState = static_cast<uint32_t>(single_overlay::install::Current());
    value.overlayInstallReason = static_cast<uint32_t>(single_overlay::install::UnavailableReason());
    value.overlayActivatedTargets = single_overlay::install::ActivatedTargetCount();
    value.overlaySuspendedThreads = single_overlay::install::SuspendedThreadCount();
    memcpy(status, &value, requested);
    if (requested == kMfgSingleModuleStatusSizeV1)
        status->version = 1;
    return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;
    gSelf = instance;
    // Process attach is serialized by the loader. Keep this buffer off the
    // stack: even ignored thread notifications otherwise reserve its 64 KiB
    // before the reason check, overflowing small driver-thread stacks.
    static wchar_t modulePath[32768]{};
    const DWORD pathLength = GetModuleFileNameW(instance, modulePath, static_cast<DWORD>(std::size(modulePath)));
    if (!pathLength || pathLength >= std::size(modulePath)) return FALSE;
    const wchar_t* basename = wcsrchr(modulePath, L'\\');
    basename = basename ? basename + 1 : modulePath;
    for (uint32_t i = 0; i < kMfgProxyFamilyCount; ++i)
        if (_wcsicmp(basename, kMfgProxyFamilies[i].original) == 0)
        {
            gSelectedProxyFamily = i;
            break;
        }
    if (!SelectProxyExports(instance, gSelectedProxyFamily)) return FALSE;
    DisableThreadLibraryCalls(instance);

    wchar_t claim[96]{};
    swprintf_s(claim, L"Local\\RTX40MFG-SingleModule-%lu", GetCurrentProcessId());
    gLifetimeClaim = CreateMutexW(nullptr, FALSE, claim);
    if (!gLifetimeClaim)
        return FALSE;
    gDuplicate = GetLastError() == ERROR_ALREADY_EXISTS;
    // Forwarding remains available after suppression. Claim-aware integrated
    // copies honor the mutex; unmodified split cores require the install guard.
    if (!gDuplicate) gDuplicate = HasLegacyConflict(instance, modulePath);

    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(instance), &pinned) || pinned != instance)
    {
        CloseHandle(gLifetimeClaim);
        gLifetimeClaim = nullptr;
        return FALSE;
    }
    if (gDuplicate)
    {
        single_module::Log(L"Backend/menu initialization suppressed by the process claim or legacy installation guard; proxy forwarding remains active.");
        return TRUE;
    }
    gOwnsBackend = true;
    if (!MfgUnlockCoreEntry(instance, reason, reserved))
    {
        gOwnsBackend = false;
        return FALSE;
    }
    // Bounded application import publication only; no UI discovery worker.
    single_overlay::InstallKnownModules();
    return TRUE;
}
