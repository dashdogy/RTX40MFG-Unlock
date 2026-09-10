#include "ampere_cuda_program.h"
#include "ampere_policy.h"
#include <cstring>

namespace ampere_cuda_program
{
Result detail::ValidateUsing(const Api& api, ampere_native_cache::ProgramView program,
    EntryView entries, uint64_t wantedLuid, FontImage font) noexcept
{
    Result result{};
    if (!api.init || !api.count || !api.device || !api.capability || !api.luid
        || !api.current || !api.create || !api.pop || !api.destroy || !api.load
        || !api.function || !api.unload || !wantedLuid
        || program.size() < ampere_native_cache::kSlots || program.size() > ampere_native_cache::kMaximumSlots
        || entries.size() != program.size() || (font.payload && font.payload->empty())) return result;
    result.stage = Stage::eAdapter;
    int count = 0, selected = 0, major = 0, minor = 0;
    unsigned matches = 0, selectedNodes = 0;
    if ((result.error = api.init(0)) != 0 || (result.error = api.count(&count)) != 0
        || count <= 0 || count > 64) return result;
    for (int ordinal = 0; ordinal < count; ++ordinal)
    {
        int device = 0, deviceMajor = 0, deviceMinor = 0;
        uint64_t luid = 0; unsigned nodes = 0;
        if ((result.error = api.device(&device, ordinal)) != 0
            || (result.error = api.capability(&deviceMajor, &deviceMinor, device)) != 0
            || (result.error = api.luid(reinterpret_cast<char*>(&luid), &nodes, device)) != 0)
            return result;
        if (luid == wantedLuid)
        { ++matches; selected = device; major = deviceMajor; minor = deviceMinor; selectedNodes = nodes; }
    }
    if (!ampere_policy::AdapterMatches(wantedLuid, wantedLuid, matches, major, minor, selectedNodes)) return result;
    result.stage = Stage::eContext;
    void* previous = nullptr;
    void* context = nullptr;
    if ((result.error = api.current(&previous)) != 0) return result;
    if ((result.error = api.create(&context, 0, selected)) != 0 || !context) return result;
    // cuCtxCreate_v2 pushes its private context. Pop exactly that context on
    // cleanup; SetCurrent(previous) would replace the pushed stack entry and
    // leave a duplicate caller context on the stack.
    auto cleanup = [&]() noexcept {
        void* active = nullptr;
        const int queriedBeforePop = api.current(&active);
        if (queriedBeforePop || active != context)
        {
            result.stage = Stage::eCleanup;
            result.error = queriedBeforePop ? queriedBeforePop : 201;
            return;
        }
        void* popped = nullptr;
        int error = api.pop(&popped);
        if (error || popped != context)
        {
            result.stage = Stage::eCleanup; result.error = error ? error : 201;
            // An unexpected current context is not ours to destroy or replace.
            return;
        }
        error = api.destroy(context);
        void* current = nullptr;
        const int queried = api.current(&current);
        result.contextRestored = !error && !queried && current == previous;
        if (!result.contextRestored)
        { result.stage = Stage::eCleanup; result.error = error ? error : queried ? queried : 201; }
    };
    for (size_t i = 0; i < program.size() + (font.payload ? 1u : 0u); ++i)
    {
        const bool auxiliary = i == program.size();
        const auto& payload = auxiliary ? *font.payload : program[i];
        const char* entry = auxiliary ? "cuda_font_kernel" : entries[i].data();
        result.slot = static_cast<uint32_t>(i);
        result.stage = Stage::eModule;
        if (payload.empty() || (!auxiliary && (entries[i][0] == '\0' || entries[i].back() != '\0')))
        { result.error = 200; cleanup(); return result; }
        void* module = nullptr;
        result.error = api.load(&module, payload.data());
        if (result.error || !module)
        { if (!result.error) result.error = 200; cleanup(); return result; }
        void* function = nullptr;
        result.stage = Stage::eFunction;
        result.error = api.function(&function, module, entry);
        if (!result.error && !function) result.error = 500;
        if (!result.error && api.functionLoad) result.error = api.functionLoad(function);
        const int unloaded = api.unload(module);
        if (unloaded) { result.stage = Stage::eCleanup; result.error = unloaded; }
        if (result.error) { cleanup(); return result; }
    }
    result.stage = Stage::eReady;
    result.slot = UINT32_MAX;
    cleanup();
    return result;
}

Result Validate(ampere_native_cache::ProgramView program, EntryView entries, uint64_t luid, FontImage font) noexcept
{
    HMODULE module = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) return {};
    detail::Api api{};
#define MFG_CUDA_API(member, symbol) api.member = reinterpret_cast<decltype(api.member)>(GetProcAddress(module, symbol))
    MFG_CUDA_API(init, "cuInit");
    MFG_CUDA_API(count, "cuDeviceGetCount");
    MFG_CUDA_API(device, "cuDeviceGet");
    MFG_CUDA_API(capability, "cuDeviceComputeCapability");
    MFG_CUDA_API(luid, "cuDeviceGetLuid");
    MFG_CUDA_API(current, "cuCtxGetCurrent");
    MFG_CUDA_API(create, "cuCtxCreate_v2");
    MFG_CUDA_API(pop, "cuCtxPopCurrent_v2");
    MFG_CUDA_API(destroy, "cuCtxDestroy_v2");
    MFG_CUDA_API(load, "cuModuleLoadData");
    MFG_CUDA_API(function, "cuModuleGetFunction");
    MFG_CUDA_API(functionLoad, "cuFuncLoad");
    MFG_CUDA_API(unload, "cuModuleUnload");
#undef MFG_CUDA_API
    const auto result = detail::ValidateUsing(api, program, entries, luid, font);
    FreeLibrary(module);
    return result;
}

Result Validate(const ampere_native_cache::Program& program, const Entries& entries, uint64_t luid) noexcept
{
    return Validate(ampere_native_cache::ProgramView(program), EntryView(entries), luid);
}
Result detail::ValidateUsing(const Api& api, const ampere_native_cache::Program& program,
    const Entries& entries, uint64_t luid) noexcept
{
    return ValidateUsing(api, ampere_native_cache::ProgramView(program), EntryView(entries), luid);
}
}
