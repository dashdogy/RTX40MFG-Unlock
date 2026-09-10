#include "reflex_control.h"
#include "entry_detour.h"

#include <sl_reflex.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <cwchar>
#include <limits>
#include <mutex>
#include <utility>

namespace reflex_control
{
namespace
{
constexpr size_t kRouteCount = 8;
constexpr size_t kEntryCount = 4;
constexpr size_t kNoRoute = kRouteCount;
using SetData = sl::Result (*)(const sl::BaseStructure*, sl::CommandBuffer*);
using Resolver = void* (*)(const char*);
constexpr std::array<entry_detour::Kind, kEntryCount> kKinds{
    entry_detour::Kind::eReflexSetOptions,
    entry_detour::Kind::eReflexSetData,
    entry_detour::Kind::eReflexSleep,
    entry_detour::Kind::eReflexGetState,
};
constexpr std::array<const char*, kEntryCount> kNames{
    "slReflexSetOptions", "slSetData", "slReflexSleep", "slReflexGetState",
};

struct ThreadStamp
{
    DWORD id = 0;
    uint64_t created = 0;
};

struct Route
{
    // Immutable after owner is published. An installed entry pins this image.
    std::atomic<HMODULE> owner{nullptr};
    uint64_t generation = 0;
    std::array<uint32_t, 4> version{};
    std::array<void*, kEntryCount> targets{};
    std::atomic<bool> stale{false};
    std::atomic_flag inCall = ATOMIC_FLAG_INIT;

    // All fields below are protected by gStateMutex, never held over a call.
    Snapshot state{};
    sl::ReflexOptions gameOptions{};
    ThreadStamp gameThread{};
    bool overrideOwned = false;
    bool uncertainOverride = false;
    bool concurrentCalls = false;
    uint64_t operation = 0;
    uint64_t availabilityTick = 0;
};

struct Desired
{
    uint32_t fps = 0;
    uint32_t us = 0;
    bool eligible = false;
    uint64_t revision = 0;
};

std::array<Route, kRouteCount> gRoutes{};
std::mutex gStateMutex;
std::mutex gDiscoveryMutex;
Desired gDesired{};
std::atomic<RuntimeValidator> gRuntimeValidator{nullptr};
size_t gSelected = kNoRoute;
Status gDiscoveryStatus = Status::eWaitingForModule;

struct ThreadContext
{
    unsigned depth = 0;
    const sl::ReflexOptions* forwardedOptions = nullptr;
};
thread_local std::array<ThreadContext, kRouteCount> gThread{};

struct CallScope
{
    ThreadContext& context;
    const sl::ReflexOptions* previous;
    explicit CallScope(size_t index) noexcept
        : context(gThread[index]), previous(context.forwardedOptions)
    {
        ++context.depth;
    }
    ~CallScope()
    {
        context.forwardedOptions = previous;
        --context.depth;
    }
};

struct CallLease
{
    Route& route;
    bool held;
    explicit CallLease(Route& value) noexcept
        : route(value), held(!route.inCall.test_and_set(std::memory_order_acquire)) {}
    ~CallLease()
    {
        if (held) route.inCall.clear(std::memory_order_release);
    }
};

bool ReadBytes(const void* source, void* destination, size_t bytes) noexcept
{
    __try
    {
        if (!source || !destination) return false;
        std::memcpy(destination, source, bytes);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool OwnedExecutable(HMODULE owner, const void* address) noexcept
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

bool ReadVersion(HMODULE module, std::array<uint32_t, 4>& version) noexcept
{
    const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(VS_VERSION_INFO), RT_VERSION);
    if (!resource) return false;
    const DWORD bytes = SizeofResource(module, resource);
    const HGLOBAL loaded = LoadResource(module, resource);
    const void* data = loaded ? LockResource(loaded) : nullptr;
    if (!data || bytes < sizeof(VS_FIXEDFILEINFO) || bytes > 65536) return false;
    void* value = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(data, L"\\", &value, &length)
        || !value || length < sizeof(VS_FIXEDFILEINFO)) return false;
    const uintptr_t start = reinterpret_cast<uintptr_t>(data);
    const uintptr_t pointer = reinterpret_cast<uintptr_t>(value);
    if (pointer < start || pointer - start > bytes
        || sizeof(VS_FIXEDFILEINFO) > bytes - (pointer - start)) return false;
    VS_FIXEDFILEINFO info{};
    if (!ReadBytes(value, &info, sizeof(info)) || info.dwSignature != VS_FFI_SIGNATURE)
        return false;
    version = {HIWORD(info.dwFileVersionMS), LOWORD(info.dwFileVersionMS),
        HIWORD(info.dwFileVersionLS), LOWORD(info.dwFileVersionLS)};
    // ReflexOptions v1 and these public entry signatures are unchanged in
    // the supported 2.14.0/2.14.1 plugins. Dynamic/V-Sync compatibility is a
    // separate runtime policy, not a prerequisite for a fixed-mode cap.
    return version[0] == 2 && version[1] == 14 && version[2] <= 1;
}

bool MatchingPath(HMODULE module, const wchar_t* expected) noexcept
{
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
    if (!length || length >= std::size(path) || !expected || _wcsicmp(path, expected))
        return false;
    // OTA plugins use opaque filenames. The resolver and its module-owned
    // Reflex entries establish feature identity below.
    return true;
}

void* Resolve(Resolver resolver, const char* name) noexcept
{
    __try { return resolver(name); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

ThreadStamp CurrentThread() noexcept
{
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return {};
    return {GetCurrentThreadId(), (static_cast<uint64_t>(created.dwHighDateTime) << 32)
        | created.dwLowDateTime};
}

bool SameThread(ThreadStamp left, ThreadStamp right) noexcept
{
    return left.id && left.created && left.id == right.id && left.created == right.created;
}

bool RuntimeEligible() noexcept
{
    const auto validator = gRuntimeValidator.load(std::memory_order_acquire);
    return !validator || validator();
}

Desired EffectiveDesired(bool runtimeEligible) noexcept
{
    // Caller holds gStateMutex; the externally supplied validator has already
    // run outside that lock.
    Desired desired = gDesired;
    desired.eligible &= runtimeEligible;
    return desired;
}

entry_detour::Snapshot Entry(size_t index, size_t kind) noexcept
{
    Route& route = gRoutes[index];
    const HMODULE owner = route.owner.load(std::memory_order_acquire);
    auto found = entry_detour::ReadSnapshot(kKinds[kind], owner);
    if (found.owner != owner || found.target != route.targets[kind]
        || found.generation != route.generation || !found.handle)
        return {};
    // Bind the final read to the exact immutable registry handle, not the
    // owner aggregate (which is only used to locate it during publication).
    const auto exact = entry_detour::ReadSnapshot(found.handle);
    return exact.owner == owner && exact.target == route.targets[kind]
        && exact.generation == route.generation ? exact : entry_detour::Snapshot{};
}

bool Current(size_t index, Snapshot* output = nullptr) noexcept
{
    uint32_t installed = 0, current = 0, failure = 0;
    bool covered = true;
    for (size_t i = 0; i < kEntryCount; ++i)
    {
        const auto entry = Entry(index, i);
        if (entry.installed) installed |= 1u << i;
        if (entry.current && entry.original && OwnedExecutable(entry.owner, entry.target))
            current |= 1u << i;
        covered &= entry.cachedPointersCovered;
        if (entry.failure != entry_detour::Failure::eNone)
            failure = static_cast<uint32_t>(entry.failure);
    }
    const bool stale = gRoutes[index].stale.load(std::memory_order_acquire);
    if (output)
    {
        output->installedHookMask = installed;
        output->currentHookMask = stale ? 0 : current;
        output->cachedPointersCovered = !stale && covered && current == eAllHooks;
        output->hookFailure = failure;
    }
    return !stale && current == eAllHooks && covered;
}

bool ReadOptions(const sl::BaseStructure* source, sl::ReflexOptions& copy) noexcept
{
    sl::ReflexOptions header{};
    if (!ReadBytes(source, &header, sizeof(sl::BaseStructure))
        || header.structType != sl::ReflexOptions::s_structType
        || header.structVersion != sl::kStructVersion1) return false;
    return ReadBytes(source, &copy, sizeof(copy))
        && copy.structType == header.structType && copy.structVersion == header.structVersion
        && copy.next == header.next;
}

// A bounded search only decides whether an unknown chain may contain options.
// Unknown prefix node sizes are never guessed or copied into a replacement.
bool ContainsOptions(const sl::BaseStructure* source) noexcept
{
    std::array<const sl::BaseStructure*, 16> seen{};
    for (size_t count = 0; source && count < seen.size(); ++count)
    {
        for (size_t i = 0; i < count; ++i)
            if (seen[i] == source) return true;
        seen[count] = source;
        sl::ReflexOptions header{};
        if (!ReadBytes(source, &header, sizeof(sl::BaseStructure))) return true;
        if (header.structType == sl::ReflexOptions::s_structType) return true;
        source = header.next;
    }
    return source != nullptr;
}

bool StateAvailability(const sl::ReflexState* state, bool& available) noexcept
{
    sl::ReflexOptions header{};
    if (!ReadBytes(state, &header, sizeof(sl::BaseStructure))
        || header.structType != sl::ReflexState::s_structType
        || header.structVersion < sl::kStructVersion1
        || header.structVersion > sl::kStructVersion2) return false;
    unsigned char value = 0;
    if (!ReadBytes(reinterpret_cast<const unsigned char*>(state)
            + offsetof(sl::ReflexState, lowLatencyAvailable), &value, sizeof(value))
        || value > 1) return false;
    available = value != 0;
    return true;
}

void RecordAvailability(size_t index, sl::Result result, const sl::ReflexState* state) noexcept
{
    bool available = false;
    const bool known = result == sl::Result::eOk && StateAvailability(state, available);
    std::lock_guard lock(gStateMutex);
    Route& route = gRoutes[index];
    route.state.lastGetStateResult = static_cast<uint32_t>(result);
    route.state.availabilityKnown = known;
    route.state.lowLatencyAvailable = known && available;
    route.availabilityTick = GetTickCount64();
}

bool QueryAvailability(size_t index) noexcept
{
    const auto entry = Entry(index, 3);
    if (!entry.current || !entry.original) return false;
    sl::ReflexState state{};
    const auto original = reinterpret_cast<PFun_slReflexGetState*>(entry.original);
    const sl::Result result = original(state);
    RecordAvailability(index, result, &state);
    std::lock_guard lock(gStateMutex);
    return gRoutes[index].state.availabilityKnown && gRoutes[index].state.lowLatencyAvailable;
}

bool Pending(const Route& route, const Desired& desired) noexcept
{
    if (!desired.fps || !desired.eligible)
        return route.overrideOwned || route.uncertainOverride;
    return !route.overrideOwned || !route.state.appliedKnown
        || route.state.appliedFrameLimitUs != desired.us
        || (route.state.availabilityKnown && !route.state.lowLatencyAvailable);
}

void Concurrent(size_t index) noexcept
{
    std::lock_guard lock(gStateMutex);
    Route& route = gRoutes[index];
    ++route.operation;
    route.concurrentCalls = true;
    route.state.appliedKnown = false;
    route.state.replaySafe = false;
    route.state.status = Status::eBusy;
}

struct Attempt
{
    Desired desired{};
    uint64_t operation = 0;
    bool modified = false;
};

Attempt Begin(size_t index, bool modified) noexcept
{
    std::lock_guard lock(gStateMutex);
    Route& route = gRoutes[index];
    Attempt attempt{gDesired, ++route.operation, modified};
    if (modified) route.uncertainOverride = true;
    gSelected = index;
    return attempt;
}

void Finish(size_t index, const Attempt& attempt, sl::Result result,
    const sl::ReflexOptions& submitted, const sl::ReflexOptions* game,
    bool replaySafe, ThreadStamp thread, bool replay, bool exclusive) noexcept
{
    std::lock_guard lock(gStateMutex);
    Route& route = gRoutes[index];
    route.state.lastSetResult = static_cast<uint32_t>(result);
    route.state.replayCalls += replay ? 1 : 0;
    if (result != sl::Result::eOk)
    {
        ++route.state.rejectedOptionCalls;
        if (attempt.modified || replay) route.state.appliedKnown = false;
        route.state.status = Status::eCallRejected;
        return;
    }
    ++route.state.acceptedOptionCalls;
    if (!exclusive || route.operation != attempt.operation)
    {
        route.uncertainOverride |= attempt.modified;
        route.state.appliedKnown = false;
        route.state.replaySafe = false;
        route.state.status = Status::eBusy;
        return;
    }
    if (game)
    {
        route.gameOptions = *game;
        route.gameOptions.next = nullptr; // Never retain a caller-owned chain.
        route.gameThread = thread;
        route.state.gameOptionsKnown = true;
        route.state.gameFrameLimitUs = game->frameLimitUs;
        route.state.replaySafe = replaySafe && thread.id && thread.created
            && !route.concurrentCalls;
    }
    route.state.appliedKnown = true;
    route.state.appliedFrameLimitUs = submitted.frameLimitUs;
    route.overrideOwned = attempt.modified;
    route.uncertainOverride = false;
    route.state.status = attempt.modified ? Status::eApplied : Status::eFollowGame;
    // A Configure callback may have changed the desired value during NVIDIA's
    // call. The accepted value remains evidence; Pending compares it to the
    // current request and schedules the next safe-boundary update/restoration.
}

sl::Result OptionsCall(size_t index, const sl::BaseStructure* input,
    sl::CommandBuffer* command, bool publicOptions) noexcept
{
    Route& route = gRoutes[index];
    const auto entry = Entry(index, publicOptions ? 0 : 1);
    if (!entry.original) return sl::Result::eErrorInvalidIntegration;
    const auto setOptions = reinterpret_cast<PFun_slReflexSetOptions*>(entry.original);
    const auto setData = reinterpret_cast<SetData>(entry.original);
    auto forward = [&](const sl::BaseStructure* options) {
        return publicOptions
            ? setOptions(*static_cast<const sl::ReflexOptions*>(options))
            : setData(options, command);
    };
    if (gThread[index].depth && input == gThread[index].forwardedOptions)
        return forward(input); // The public setter's normal slSetData call.

    sl::ReflexOptions game{};
    const bool known = ReadOptions(input, game);
    if (!known && !publicOptions && !ContainsOptions(input)) return forward(input);
    if (gThread[index].depth) Concurrent(index);
    CallLease lease(route);
    if (!lease.held) Concurrent(index);
    CallScope scope(index);
    if (!known)
    {
        const sl::Result result = forward(input);
        std::lock_guard lock(gStateMutex);
        ++route.operation;
        route.state.appliedKnown = false;
        route.state.replaySafe = false;
        route.state.status = Status::eUnknownShape;
        return result;
    }
    const bool current = Current(index);
    bool runtimeEligible = RuntimeEligible();
    bool wantsOverride = false, query = false;
    {
        std::lock_guard lock(gStateMutex);
        wantsOverride = gDesired.fps && gDesired.eligible && runtimeEligible;
        query = wantsOverride && (!route.state.availabilityKnown
            || !route.state.lowLatencyAvailable);
    }
    if (current && lease.held && query) QueryAvailability(index);
    runtimeEligible = RuntimeEligible();
    sl::ReflexOptions adjusted = game;
    bool modify = false;
    {
        std::lock_guard lock(gStateMutex);
        modify = current && lease.held && runtimeEligible && gDesired.fps && gDesired.eligible
            && route.state.availabilityKnown && route.state.lowLatencyAvailable;
        if (modify) adjusted.frameLimitUs = gDesired.us;
        else if (!current) route.state.status = Status::eStaleRoute;
    }
    const Attempt attempt = Begin(index, modify);
    const sl::BaseStructure* forwarded = modify ? &adjusted : input;
    scope.context.forwardedOptions = static_cast<const sl::ReflexOptions*>(forwarded);
    const sl::Result result = forward(forwarded);
    Finish(index, attempt, result, adjusted, &game,
        !game.next && (publicOptions || !command) && lease.held,
        CurrentThread(), false, lease.held);
    return result;
}

void ReplayAtSleep(size_t index) noexcept
{
    Route& route = gRoutes[index];
    if (!Current(index)) return;
    CallLease lease(route);
    if (!lease.held) { Concurrent(index); return; }
    const ThreadStamp thread = CurrentThread();
    bool runtimeEligible = RuntimeEligible();
    bool restore = false, query = false;
    {
        std::lock_guard lock(gStateMutex);
        const Desired desired = EffectiveDesired(runtimeEligible);
        if (!Pending(route, desired)) return;
        if (!route.state.gameOptionsKnown)
        {
            route.state.status = Status::eWaitingForGameOptions;
            return;
        }
        if (!route.state.replaySafe || !SameThread(route.gameThread, thread))
        {
            route.state.status = route.state.replaySafe ? Status::eWaitingForThread : Status::eUnknownShape;
            return;
        }
        restore = !desired.fps || !desired.eligible;
        query = !restore && (!route.availabilityTick
            || GetTickCount64() - route.availabilityTick >= 250);
    }
    CallScope scope(index);
    if (query) QueryAvailability(index);
    runtimeEligible = RuntimeEligible();
    sl::ReflexOptions adjusted{};
    bool modify = false;
    {
        std::lock_guard lock(gStateMutex);
        const Desired desired = EffectiveDesired(runtimeEligible);
        if (!Pending(route, desired) || !route.state.replaySafe
            || !SameThread(route.gameThread, thread)) return;
        modify = desired.fps && desired.eligible;
        if (modify && (!route.state.availabilityKnown || !route.state.lowLatencyAvailable))
        {
            route.state.status = route.state.availabilityKnown
                ? Status::eUnavailable : Status::eWaitingForAvailability;
            // Losing Reflex support never just abandons an owned cap. Try
            // restoring the last game options on this established thread.
            if (!route.overrideOwned && !route.uncertainOverride) return;
            modify = false;
        }
        adjusted = route.gameOptions;
        if (modify) adjusted.frameLimitUs = desired.us;
    }
    const auto entry = Entry(index, 0);
    if (!entry.current || !entry.original) return;
    const Attempt attempt = Begin(index, modify);
    scope.context.forwardedOptions = &adjusted;
    const auto original = reinterpret_cast<PFun_slReflexSetOptions*>(entry.original);
    const sl::Result result = original(adjusted);
    Finish(index, attempt, result, adjusted, nullptr, true, thread, true, true);
}

template <size_t Index>
sl::Result HookSetOptions(const sl::ReflexOptions& options) noexcept
{
    return OptionsCall(Index, &options, nullptr, true);
}

template <size_t Index>
sl::Result HookSetData(const sl::BaseStructure* inputs, sl::CommandBuffer* command) noexcept
{
    return OptionsCall(Index, inputs, command, false);
}

template <size_t Index>
sl::Result HookSleep(const sl::FrameToken& frame) noexcept
{
    const auto entry = Entry(Index, 2);
    if (!entry.original) return sl::Result::eErrorInvalidIntegration;
    if (!gThread[Index].depth) ReplayAtSleep(Index);
    CallScope scope(Index);
    return reinterpret_cast<PFun_slReflexSleep*>(entry.original)(frame);
}

template <size_t Index>
sl::Result HookGetState(sl::ReflexState& state) noexcept
{
    const auto entry = Entry(Index, 3);
    if (!entry.original) return sl::Result::eErrorInvalidIntegration;
    const bool outer = gThread[Index].depth == 0;
    CallLease lease(gRoutes[Index]);
    if (outer && !lease.held) Concurrent(Index);
    CallScope scope(Index);
    const auto result = reinterpret_cast<PFun_slReflexGetState*>(entry.original)(state);
    if (entry.current && !gRoutes[Index].stale.load(std::memory_order_acquire))
        RecordAvailability(Index, result, &state);
    return result;
}

template <size_t... Index>
auto Thunks(std::index_sequence<Index...>) noexcept
{
    return std::array<std::array<void*, kEntryCount>, sizeof...(Index)>{
        std::array<void*, kEntryCount>{reinterpret_cast<void*>(&HookSetOptions<Index>),
            reinterpret_cast<void*>(&HookSetData<Index>), reinterpret_cast<void*>(&HookSleep<Index>),
            reinterpret_cast<void*>(&HookGetState<Index>)}...};
}
const auto kThunks = Thunks(std::make_index_sequence<kRouteCount>{});
}

bool Configure(uint32_t frameLimitFps, bool eligible) noexcept
{
    if (frameLimitFps > 1000) return false;
    std::lock_guard lock(gStateMutex);
    if (gDesired.fps != frameLimitFps || gDesired.eligible != eligible)
    {
        gDesired.fps = frameLimitFps;
        gDesired.us = frameLimitFps ? (1000000u + frameLimitFps - 1) / frameLimitFps : 0;
        gDesired.eligible = eligible;
        ++gDesired.revision;
    }
    return true;
}

void SetRuntimeValidator(RuntimeValidator validator) noexcept
{
    gRuntimeValidator.store(validator, std::memory_order_release);
}

void ObserveModule(HMODULE module, const wchar_t* path, uint64_t generation) noexcept
{
    if (!module || !generation || !MatchingPath(module, path)) return;
    for (Route& route : gRoutes)
    {
        if (route.owner.load(std::memory_order_acquire) != module) continue;
        if (route.generation != generation) route.stale.store(true, std::memory_order_release);
        return;
    }
    // Reserve/publish immutable route metadata before enabling any entry. The
    // resolver runs without our locks, and returns only module-owned functions.
    HMODULE retained = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(module), &retained) || retained != module) return;
    struct ModuleLease
    {
        HMODULE module;
        ~ModuleLease() { FreeLibrary(module); }
    } lease{retained};
    const auto resolver = reinterpret_cast<Resolver>(GetProcAddress(module, "slGetPluginFunction"));
    if (!OwnedExecutable(module, reinterpret_cast<void*>(resolver))) return;
    std::array<void*, kEntryCount> targets{};
    for (size_t i = 0; i < targets.size(); ++i)
    {
        targets[i] = Resolve(resolver, kNames[i]);
        if (!OwnedExecutable(module, targets[i]))
        {
            if (i == 0) return; // A different Streamline feature is not Reflex.
            std::lock_guard lock(gStateMutex);
            if (gSelected == kNoRoute) gDiscoveryStatus = Status::eHookUnavailable;
            return;
        }
        for (size_t j = 0; j < i; ++j)
            if (targets[i] == targets[j]) return;
    }
    std::array<uint32_t, 4> version{};
    if (!ReadVersion(module, version))
    {
        std::lock_guard lock(gStateMutex);
        if (gSelected == kNoRoute) gDiscoveryStatus = Status::eUnsupportedModule;
        return;
    }
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(resolver), &pinned) || pinned != module) return;
    size_t index = kNoRoute;
    {
        std::lock_guard lock(gDiscoveryMutex);
        for (size_t i = 0; i < gRoutes.size(); ++i)
        {
            Route& route = gRoutes[i];
            const HMODULE existing = route.owner.load(std::memory_order_acquire);
            if (existing == module)
            {
                if (route.generation != generation || route.targets != targets)
                    route.stale.store(true, std::memory_order_release);
                return;
            }
            if (!existing && index == kNoRoute) index = i;
        }
        if (index == kNoRoute) return;
        Route& route = gRoutes[index];
        route.generation = generation;
        route.version = version;
        route.targets = targets;
        route.owner.store(module, std::memory_order_release);
    }
    {
        std::lock_guard lock(gStateMutex);
        if (gSelected == kNoRoute) gSelected = index;
    }
    entry_detour::InstallOptions options{};
    options.generation = generation;
    options.allowRelocated = true;
    for (size_t i = 0; i < kEntryCount; ++i)
    {
        void* original = nullptr;
        entry_detour::Install(kKinds[i], module, targets[i], kThunks[index][i], original, options);
    }
}

Snapshot ReadSnapshot() noexcept
{
    Snapshot result{};
    size_t selected = kNoRoute;
    const bool runtimeEligible = RuntimeEligible();
    {
        std::lock_guard lock(gStateMutex);
        selected = gSelected;
        const Desired desired = EffectiveDesired(runtimeEligible);
        if (selected != kNoRoute)
        {
            const Route& route = gRoutes[selected];
            result = route.state;
            result.pending = Pending(route, desired);
            result.restorePending = (!desired.fps || !desired.eligible
                    || (route.state.availabilityKnown && !route.state.lowLatencyAvailable))
                && (route.overrideOwned || route.uncertainOverride);
            result.moduleGeneration = route.generation;
            result.moduleVersionMajor = route.version[0];
            result.moduleVersionMinor = route.version[1];
            result.moduleVersionPatch = route.version[2];
            result.moduleVersionBuild = route.version[3];
            if (result.restorePending && result.status != Status::eCallRejected
                && result.status != Status::eWaitingForThread && result.status != Status::eUnknownShape)
                result.status = Status::eRestorePending;
            else if (!desired.eligible && desired.fps && !result.restorePending)
                result.status = Status::eIneligible;
            else if (result.pending && (result.status == Status::eFollowGame
                || result.status == Status::eApplied))
                result.status = !result.gameOptionsKnown ? Status::eWaitingForGameOptions
                    : !result.availabilityKnown ? Status::eWaitingForAvailability
                    : !result.lowLatencyAvailable ? Status::eUnavailable : Status::eBusy;
        }
        else
        {
            result.pending = gDesired.fps != 0;
            result.status = !result.pending ? Status::eFollowGame
                : !desired.eligible ? Status::eIneligible : gDiscoveryStatus;
        }
        result.requestedFrameLimitFps = gDesired.fps;
        result.requestedFrameLimitUs = gDesired.us;
        result.eligible = desired.eligible;
    }
    if (selected != kNoRoute && !Current(selected, &result))
    {
        result.appliedKnown = false;
        result.status = gRoutes[selected].stale.load(std::memory_order_acquire)
            ? Status::eStaleRoute : Status::eHookUnavailable;
    }
    return result;
}

const char* StatusName(Status status) noexcept
{
    switch (status)
    {
    case Status::eFollowGame: return "follow-game";
    case Status::eWaitingForModule: return "waiting-for-reflex";
    case Status::eUnsupportedModule: return "unsupported-reflex-version";
    case Status::eHookUnavailable: return "reflex-hook-unavailable";
    case Status::eIneligible: return "runtime-ineligible";
    case Status::eWaitingForGameOptions: return "waiting-for-game-options";
    case Status::eWaitingForThread: return "waiting-for-options-thread";
    case Status::eWaitingForAvailability: return "waiting-for-reflex-state";
    case Status::eUnavailable: return "reflex-unavailable";
    case Status::eApplied: return "accepted";
    case Status::eRestorePending: return "restore-pending";
    case Status::eCallRejected: return "reflex-call-rejected";
    case Status::eUnknownShape: return "options-not-replayable";
    case Status::eStaleRoute: return "stale-reflex-route";
    case Status::eBusy: return "pending-safe-boundary";
    }
    return "unknown";
}
}
