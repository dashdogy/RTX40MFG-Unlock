#include "vsync_control.h"
#include "gpu_dispatch.h"

#include <dxgi1_6.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace vsync_control
{
namespace
{
std::mutex gMutex;
Snapshot gSnapshot;
bool gConfiguredEligible = false;
std::atomic<RuntimeValidator> gRuntimeValidator{nullptr};
uint64_t gNextGeneration = 1;
uint64_t gNextSerial = 1;
uint64_t gSnapshotSerial = 0;

struct OwnerRecord { HMODULE owner = nullptr; bool verified = false; };
std::mutex gOwnersMutex;
std::vector<OwnerRecord> gOwners;

bool ExecutableOwned(void* address, HMODULE owner) noexcept
{
    MEMORY_BASIC_INFORMATION memory{};
    return owner && address
        && VirtualQuery(address, &memory, sizeof(memory)) == sizeof(memory)
        && memory.AllocationBase == owner && memory.Type == MEM_IMAGE
        && memory.State == MEM_COMMIT
        && !(memory.Protect & (PAGE_GUARD | PAGE_NOACCESS))
        && (memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ
            | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}

bool ReadVersion(const wchar_t* path) noexcept
{
    DWORD ignored = 0;
    const DWORD bytes = GetFileVersionInfoSizeW(path, &ignored);
    if (!bytes || bytes > 1024u * 1024u) return false;
    std::vector<uint8_t> version(bytes);
    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT fixedBytes = 0;
    if (!GetFileVersionInfoW(path, 0, bytes, version.data())
        || !VerQueryValueW(version.data(), L"\\",
            reinterpret_cast<void**>(&fixed), &fixedBytes)
        || !fixed || fixedBytes < sizeof(*fixed)
        || fixed->dwSignature != VS_FFI_SIGNATURE) return false;
    const uint64_t actual = (uint64_t{fixed->dwFileVersionMS} << 32)
        | fixed->dwFileVersionLS;
    constexpr uint64_t minimum = (uint64_t{2} << 48)
        | (uint64_t{14} << 32) | (uint64_t{1} << 16);
    return actual >= minimum;
}

bool MatchingImageHeader(HMODULE owner, HANDLE file) noexcept
{
    // The open handle denies replacement while its signature and version are
    // checked. Also reject a different PE occupying the loaded image's path.
    IMAGE_DOS_HEADER dos{};
    DWORD read = 0;
    if (!ReadFile(file, &dos, sizeof(dos), &read, nullptr)
        || read != sizeof(dos) || dos.e_magic != IMAGE_DOS_SIGNATURE
        || dos.e_lfanew <= 0 || dos.e_lfanew > 1024 * 1024) return false;
    LARGE_INTEGER position{};
    position.QuadPart = dos.e_lfanew;
    IMAGE_NT_HEADERS64 disk{};
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)
        || !ReadFile(file, &disk, sizeof(disk), &read, nullptr)
        || read != sizeof(disk) || disk.Signature != IMAGE_NT_SIGNATURE
        || disk.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64
        || disk.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
    __try
    {
        const auto* base = reinterpret_cast<const uint8_t*>(owner);
        const auto* loadedDos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (loadedDos->e_magic != dos.e_magic || loadedDos->e_lfanew != dos.e_lfanew)
            return false;
        const auto* loaded = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos.e_lfanew);
        return loaded->Signature == disk.Signature
            && loaded->FileHeader.Machine == disk.FileHeader.Machine
            && loaded->FileHeader.TimeDateStamp == disk.FileHeader.TimeDateStamp
            && loaded->FileHeader.NumberOfSections == disk.FileHeader.NumberOfSections
            && loaded->OptionalHeader.SizeOfImage == disk.OptionalHeader.SizeOfImage
            && loaded->OptionalHeader.CheckSum == disk.OptionalHeader.CheckSum;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool VerifyInterposer(HMODULE owner) noexcept
{
    HMODULE pinned = nullptr;
    if (!owner || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(owner), &pinned)
        || pinned != owner) return false;
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(owner, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return false;
    const wchar_t* name = wcsrchr(path.data(), L'\\');
    if (_wcsicmp(name ? name + 1 : path.data(), L"sl.interposer.dll") != 0) return false;
    for (const char* entry : {"slInit", "slGetFeatureFunction", "slSetD3DDevice", "CreateDXGIFactory2"})
        if (!ExecutableOwned(reinterpret_cast<void*>(GetProcAddress(owner, entry)), owner)) return false;
    HANDLE file = CreateFileW(path.data(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    bool verified = MatchingImageHeader(owner, file) && ReadVersion(path.data());
    if (verified)
    {
        WINTRUST_FILE_INFO info{};
        info.cbStruct = sizeof(info);
        info.pcwszFilePath = path.data();
        info.hFile = file;
        WINTRUST_DATA trust{};
        trust.cbStruct = sizeof(trust);
        trust.dwUIChoice = WTD_UI_NONE;
        trust.fdwRevocationChecks = WTD_REVOKE_NONE;
        trust.dwUnionChoice = WTD_CHOICE_FILE;
        trust.pFile = &info;
        trust.dwStateAction = WTD_STATEACTION_VERIFY;
        // Registration is outside DllMain and happens once per pinned owner.
        // Trust verification must not open a UI or retrieve anything online.
        trust.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
        GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
        verified = WinVerifyTrust(nullptr, &action, &trust) == ERROR_SUCCESS;
        if (verified)
        {
            const auto* data = WTHelperProvDataFromStateData(trust.hWVTStateData);
            const auto* signer = data
                ? WTHelperGetProvSignerFromChain(const_cast<CRYPT_PROVIDER_DATA*>(data), 0, FALSE, 0)
                : nullptr;
            const CERT_CONTEXT* certificate = signer && signer->csCertChain
                ? signer->pasCertChain[0].pCert : nullptr;
            std::array<wchar_t, 256> commonName{};
            verified = certificate
                && CertGetNameStringW(certificate, CERT_NAME_ATTR_TYPE, 0,
                    const_cast<char*>(szOID_COMMON_NAME), commonName.data(),
                    static_cast<DWORD>(commonName.size())) > 1
                && wcscmp(commonName.data(), L"NVIDIA Corporation") == 0;
        }
        trust.dwStateAction = WTD_STATEACTION_CLOSE;
        WinVerifyTrust(nullptr, &action, &trust);
    }
    CloseHandle(file);
    return verified;
}

bool VerifiedInterposer(HMODULE owner) noexcept
{
    {
        std::lock_guard lock(gOwnersMutex);
        for (const auto& record : gOwners)
            if (record.owner == owner) return record.verified;
    }
    const bool verified = VerifyInterposer(owner);
    std::lock_guard lock(gOwnersMutex);
    for (const auto& record : gOwners)
        if (record.owner == owner) return record.verified;
    // A successfully pinned image cannot be unloaded or reused at this base.
    // Failed identities remain unavailable for this process as well.
    gOwners.push_back({owner, verified});
    return verified;
}

Failure CheckOwnership(const Ownership& value) noexcept
{
    if (!value.swapchain || !value.applicationWrapper)
        return Failure::eNoApplicationSwapchain;
    if (!value.creatorOwner || value.creatorOwner != value.presentOwner
        || value.creatorOwner != value.present1Owner)
        return Failure::eWrongOwner;
    if (!value.queueIdentity || !value.queueDeviceIdentity || !value.adapterLuid
        || value.queueDeviceIdentity != value.swapchainDeviceIdentity)
        return Failure::eDeviceIdentityLost;
    if (!ExecutableOwned(value.presentEntry, value.presentOwner)
        || !ExecutableOwned(value.present1Entry, value.present1Owner))
        return Failure::eTargetChanged;
    return VerifiedInterposer(value.creatorOwner)
        ? Failure::eNone : Failure::eInterposerUnverified;
}

bool SameOwnership(const Ownership& a, const Ownership& b) noexcept
{
    return a.swapchain == b.swapchain && a.creatorOwner == b.creatorOwner
        && a.presentOwner == b.presentOwner && a.present1Owner == b.present1Owner
        && a.presentEntry == b.presentEntry && a.present1Entry == b.present1Entry
        && a.queueIdentity == b.queueIdentity
        && a.queueDeviceIdentity == b.queueDeviceIdentity
        && a.swapchainDeviceIdentity == b.swapchainDeviceIdentity
        && a.adapterLuid == b.adapterLuid
        && a.applicationWrapper == b.applicationWrapper;
}

void ClearObserved(Failure failure) noexcept
{
    gSnapshot.available = false;
    gSnapshot.matched = false;
    gSnapshot.overrideApplied = false;
    gSnapshot.originalInterval = 0;
    gSnapshot.submittedInterval = 0;
    gSnapshot.originalFlags = 0;
    gSnapshot.submittedFlags = 0;
    gSnapshot.ownerGeneration = 0;
    gSnapshot.failure = failure;
    gSnapshot.presentResult = 0;
    gSnapshotSerial = 0;
}
}

void Configure(uint32_t mode, bool runtimeEligible, uint64_t runtimeGeneration) noexcept
{
    mode = mode <= static_cast<uint32_t>(Mode::eOn) ? mode : 0;
    runtimeEligible = runtimeEligible && runtimeGeneration != 0;
    std::lock_guard lock(gMutex);
    if (gSnapshot.requestedMode == mode && gConfiguredEligible == runtimeEligible
        && gSnapshot.runtimeGeneration == runtimeGeneration) return;
    gSnapshot.requestedMode = mode;
    gConfiguredEligible = runtimeEligible;
    gSnapshot.runtimeEligible = runtimeEligible;
    gSnapshot.runtimeGeneration = runtimeGeneration;
    ++gSnapshot.configurationGeneration;
    ClearObserved(runtimeEligible ? Failure::eNoApplicationSwapchain : Failure::eRuntimeUnavailable);
}

void SetRuntimeValidator(RuntimeValidator validator) noexcept
{
    gRuntimeValidator.store(validator, std::memory_order_release);
}

Snapshot ReadSnapshot() noexcept
{
    std::lock_guard lock(gMutex);
    return gSnapshot;
}

bool RegisterChain(Chain& chain, const Ownership& ownership) noexcept
{
    const Failure failure = CheckOwnership(ownership);
    std::lock_guard lock(gMutex);
    if (chain.registered && failure == Failure::eNone
        && SameOwnership(chain.ownership, ownership)) return true;
    if (chain.generation && gSnapshot.ownerGeneration == chain.generation)
        ClearObserved(failure == Failure::eNone ? Failure::eGenerationChanged : failure);
    chain.ownership = ownership;
    chain.generation = gNextGeneration++;
    chain.inFlight = 0;
    chain.failure = failure;
    chain.registered = failure == Failure::eNone;
    return chain.registered;
}

void InvalidateChain(Chain& chain, Failure failure) noexcept
{
    std::lock_guard lock(gMutex);
    if (chain.generation && gSnapshot.ownerGeneration == chain.generation)
        ClearObserved(failure);
    chain.generation = gNextGeneration++;
    chain.inFlight = 0;
    chain.registered = false;
    chain.failure = failure;
}

Ticket AdjustBeforePresent(Chain& chain, uintptr_t swapchain,
    void* callbackEntry, void* currentEntry, bool present1,
    bool principal, bool outer, uint32_t interval, uint32_t flags) noexcept
{
    Ticket ticket{};
    ticket.originalInterval = ticket.submittedInterval = interval;
    ticket.originalFlags = ticket.submittedFlags = flags;
    // Nested/test/auxiliary calls must never replace the application's status
    // or carry a policy ticket into a native generated-frame presentation.
    if (!outer || (flags & DXGI_PRESENT_TEST)) return ticket;
    const auto validator = gRuntimeValidator.load(std::memory_order_acquire);
    const bool runtimeCurrent = validator && validator();
    const uint64_t activeAdapter = gpu_dispatch::AdapterLuid();
    std::lock_guard lock(gMutex);
    if (!principal)
    {
        if (chain.generation && gSnapshot.ownerGeneration == chain.generation)
            ClearObserved(Failure::eNotPrincipal);
        return ticket;
    }
    if (chain.inFlight) return ticket;
    if (!chain.registered)
    {
        // A native/foreign swapchain can present on an independent NVIDIA
        // worker thread. It cannot acquire or overwrite an application's
        // policy ownership merely because its TLS Present depth is zero.
        if (!gSnapshot.ownerGeneration)
        {
            gSnapshot.available = false;
            gSnapshot.failure = chain.failure;
        }
        return ticket;
    }
    gSnapshot.runtimeEligible = gConfiguredEligible && runtimeCurrent;
    const void* expected = present1 ? chain.ownership.present1Entry : chain.ownership.presentEntry;
    Failure failure = chain.failure;
    if (chain.registered)
    {
        if (!swapchain || swapchain != chain.ownership.swapchain)
            failure = Failure::eNoApplicationSwapchain;
        else if (!expected || callbackEntry != expected || currentEntry != expected)
            failure = Failure::eTargetChanged;
        else if (!activeAdapter || chain.ownership.adapterLuid != activeAdapter)
            failure = Failure::eDeviceIdentityLost;
        else if (!gSnapshot.runtimeEligible)
            failure = Failure::eRuntimeUnavailable;
        else
            failure = Failure::eNone;
    }
    ticket.available = chain.registered && failure == Failure::eNone;
    ticket.requestMatched = ticket.available;
    if (ticket.available)
    {
        if (gSnapshot.requestedMode == static_cast<uint32_t>(Mode::eOn))
        {
            ticket.submittedInterval = 1;
            ticket.submittedFlags &= ~DXGI_PRESENT_ALLOW_TEARING;
        }
        else if (gSnapshot.requestedMode == static_cast<uint32_t>(Mode::eOff))
        {
            ticket.submittedInterval = 0;
        }
    }
    ticket.changed = ticket.submittedInterval != interval || ticket.submittedFlags != flags;
    ticket.ownerGeneration = chain.generation;
    ticket.configurationGeneration = gSnapshot.configurationGeneration;
    ticket.serial = gNextSerial++;
    ticket.tracked = chain.generation != 0;
    if (ticket.tracked) chain.inFlight = ticket.serial;
    gSnapshot.available = ticket.available;
    gSnapshot.matched = false;
    gSnapshot.overrideApplied = false;
    gSnapshot.originalInterval = interval;
    gSnapshot.submittedInterval = ticket.submittedInterval;
    gSnapshot.originalFlags = flags;
    gSnapshot.submittedFlags = ticket.submittedFlags;
    gSnapshot.ownerGeneration = chain.generation;
    gSnapshot.failure = failure;
    gSnapshot.presentResult = 0;
    gSnapshotSerial = ticket.serial;
    return ticket;
}

void CompletePresent(Chain& chain, const Ticket& ticket, HRESULT result) noexcept
{
    if (!ticket.tracked) return;
    const auto validator = gRuntimeValidator.load(std::memory_order_acquire);
    const bool runtimeCurrent = validator && validator();
    const uint64_t activeAdapter = gpu_dispatch::AdapterLuid();
    std::lock_guard lock(gMutex);
    if (chain.generation != ticket.ownerGeneration || chain.inFlight != ticket.serial) return;
    chain.inFlight = 0;
    if (gSnapshot.configurationGeneration != ticket.configurationGeneration
        || gSnapshot.ownerGeneration != ticket.ownerGeneration
        || gSnapshotSerial != ticket.serial) return;
    gSnapshot.presentResult = result;
    gSnapshot.runtimeEligible = gConfiguredEligible && runtimeCurrent;
    const bool adapterCurrent = activeAdapter && chain.ownership.adapterLuid == activeAdapter;
    gSnapshot.available = ticket.available && gSnapshot.runtimeEligible && adapterCurrent;
    gSnapshot.matched = result == S_OK && ticket.requestMatched && gSnapshot.available;
    gSnapshot.overrideApplied = gSnapshot.matched && ticket.changed;
    if (!gSnapshot.runtimeEligible) gSnapshot.failure = Failure::eRuntimeUnavailable;
    else if (!adapterCurrent) gSnapshot.failure = Failure::eDeviceIdentityLost;
    else if (result != S_OK) gSnapshot.failure = Failure::ePresentFailed;
}

const char* FailureName(Failure failure) noexcept
{
    switch (failure)
    {
    case Failure::eNone: return "none";
    case Failure::eRuntimeUnavailable: return "runtime-unavailable";
    case Failure::eNoApplicationSwapchain: return "application-swapchain-unproven";
    case Failure::eInterposerUnverified: return "interposer-unverified";
    case Failure::eWrongOwner: return "creator-owner-mismatch";
    case Failure::eTargetChanged: return "present-target-changed";
    case Failure::eDeviceIdentityLost: return "device-identity-lost";
    case Failure::eNotPrincipal: return "principal-swapchain-unproven";
    case Failure::eNestedPresent: return "nested-present";
    case Failure::eTestPresent: return "test-present";
    case Failure::eGenerationChanged: return "generation-changed";
    case Failure::ePresentFailed: return "present-not-accepted";
    case Failure::eConcurrentPresent: return "concurrent-present";
    }
    return "unknown";
}
}
