#include "ampere_cuda_program.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>
using namespace ampere_cuda_program;
namespace
{
std::vector<void*> stack;
int devices, capabilityMinor, loadError, functionError, unloadError, createError, failSlot;
bool changeContext, popError;
int materializeError;
unsigned materializations;
unsigned loads, functions, unloads, creates, pops, destroys;
size_t fontAt = SIZE_MAX;
int fontFunctionError = 0;
void* own = reinterpret_cast<void*>(0x7770);
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Reset()
{
    stack = {reinterpret_cast<void*>(0x1110), reinterpret_cast<void*>(0x2220)};
    devices = 1; capabilityMinor = 6; failSlot = 0;
    loadError = functionError = unloadError = createError = 0;
    changeContext = popError = false;
    loads = functions = unloads = creates = pops = destroys = 0;
    materializeError = 0; materializations = 0;
    fontAt = SIZE_MAX; fontFunctionError = 0;
}
int WINAPI Init(unsigned) { return 0; }
int WINAPI Count(int* n) { *n = devices; return 0; }
int WINAPI Device(int* value, int ordinal) { *value = ordinal; return 0; }
int WINAPI Capability(int* major, int* minor, int) { *major = 8; *minor = capabilityMinor; return 0; }
int WINAPI Luid(char* value, unsigned* nodes, int) { uint64_t luid = 42; std::memcpy(value, &luid, 8); *nodes = 1; return 0; }
int WINAPI Current(void** value) { *value = stack.empty() ? nullptr : stack.back(); return 0; }
int WINAPI Create(void** value, unsigned flags, int device)
{
    ++creates; Check(flags == 0 && device == 0, "matched device and ordinary context");
    if (createError) return createError;
    stack.push_back(own); *value = own; return 0;
}
int WINAPI Pop(void** value)
{
    ++pops; if (popError) return 201;
    Check(!stack.empty(), "context stack underflow"); *value = stack.back(); stack.pop_back(); return 0;
}
int WINAPI Destroy(void* value) { ++destroys; Check(value == own && stack.size() == 2, "only popped owned context destroyed"); return 0; }
int WINAPI Load(void** value, const void*)
{
    const unsigned slot = loads++;
    if (loadError && slot == static_cast<unsigned>(failSlot)) return loadError;
    *value = reinterpret_cast<void*>(0x9900 + slot); return 0;
}
int WINAPI Function(void** value, void*, const char* entry)
{
    const bool font = functions++ == fontAt;
    Check(std::strcmp(entry, font ? "cuda_font_kernel" : "main_kernel") == 0, "exact entry name");
    if (font && fontFunctionError) return fontFunctionError;
    if (changeContext) stack.push_back(reinterpret_cast<void*>(0x3330));
    if (functionError) return functionError;
    *value = reinterpret_cast<void*>(0x8800); return 0;
}
int WINAPI Unload(void*) { ++unloads; return unloadError; }
int WINAPI Materialize(void* function)
{
    ++materializations;
    Check(function == reinterpret_cast<void*>(0x8800), "resolved function materialized");
    return materializeError;
}
detail::Api Api() { return {Init,Count,Device,Capability,Luid,Current,Create,Pop,Destroy,Load,Function,nullptr,Unload}; }
}
int main()
{
    try
    {
        ampere_native_cache::Program program{}; Entries entries{};
        for (size_t i = 0; i < program.size(); ++i)
        { program[i] = {1,2,3,4}; std::strcpy(entries[i].data(), "main_kernel"); }
        Reset();
        auto result = detail::ValidateUsing(Api(), program, entries, 42);
        Check(result.Ready() && loads == 25 && functions == 25 && unloads == 25
            && creates == 1 && pops == 1 && destroys == 1 && stack.size() == 2, "complete program and exact nested context restore");
        Reset(); auto lazyApi = Api(); lazyApi.functionLoad = Materialize;
        result = detail::ValidateUsing(lazyApi, program, entries, 42);
        Check(result.Ready() && materializations == 25 && unloads == 25, "all lazy functions materialized when supported");
        Reset(); materializeError = 209;
        result = detail::ValidateUsing(lazyApi, program, entries, 42);
        Check(result.NativeImageUnsupported() && result.stage == Stage::eFunction
            && result.slot == 0 && materializations == 1 && unloads == 1 && stack.size() == 2,
            "lazy function image rejection unloads and restores context before fallback");
        Reset(); loadError = 209; failSlot = 7;
        result = detail::ValidateUsing(Api(), program, entries, 42);
        Check(result.NativeImageUnsupported() && result.slot == 7 && result.error == 209
            && loads == 8 && functions == 7 && unloads == 7 && stack.size() == 2,
            "unsupported native image reports exact slot and restores context before fallback");
        Reset(); functionError = 500;
        result = detail::ValidateUsing(Api(), program, entries, 42);
        Check(!result.Ready() && !result.NativeImageUnsupported() && unloads == 1
            && result.contextRestored && stack.size() == 2, "missing entry does not enable compatibility fallback");
        for (const int code : {2,201,700,719,999})
        {
            Reset(); loadError = code;
            result = detail::ValidateUsing(Api(), program, entries, 42);
            Check(!result.NativeImageUnsupported() && result.contextRestored, "OOM/context/device errors do not retry PTX");
        }
        Reset(); capabilityMinor = 9;
        Check(!detail::ValidateUsing(Api(),program,entries,42).Ready() && creates == 0, "Ada excluded from shipping Ampere validation");
        Reset(); devices = 2;
        Check(!detail::ValidateUsing(Api(),program,entries,42).Ready() && creates == 0, "duplicate LUID rejected");
        Reset(); Check(!detail::ValidateUsing(Api(),program,entries,43).Ready() && creates == 0, "wrong LUID rejected");
        Reset(); createError = 2;
        result = detail::ValidateUsing(Api(),program,entries,42);
        Check(!result.Ready() && destroys == 0 && pops == 0 && stack.size() == 2, "failed creation does not touch caller context");
        Reset(); unloadError = 201;
        result = detail::ValidateUsing(Api(),program,entries,42);
        Check(result.stage == Stage::eCleanup && !result.Ready() && !result.NativeImageUnsupported(), "cleanup failure cannot publish");
        Reset(); changeContext = true;
        result = detail::ValidateUsing(Api(),program,entries,42);
        Check(result.stage == Stage::eCleanup && !result.contextRestored && pops == 0 && destroys == 0
            && stack.back() == reinterpret_cast<void*>(0x3330), "foreign current context is not popped or destroyed");
        Reset(); popError = true;
        result = detail::ValidateUsing(Api(),program,entries,42);
        Check(result.stage == Stage::eCleanup && !result.contextRestored && destroys == 0, "failed pop retains owned context");
        Reset(); auto absent = Api(); absent.function = nullptr;
        Check(!detail::ValidateUsing(absent,program,entries,42).Ready() && creates == 0, "missing CUDA export fails closed");
        for (const size_t count : {25u,26u,27u,28u,33u,38u})
        {
            ampere_native_cache::CompleteProgram complete(count, {1,2,3,4});
            std::vector<Entry> completeEntries(count);
            for (auto& entry : completeEntries) std::strcpy(entry.data(), "main_kernel");
            Reset();
            result = detail::ValidateUsing(lazyApi, ampere_native_cache::ProgramView(complete), EntryView(completeEntries), 42);
            Check(result.Ready() && loads == count && functions == count && unloads == count
                && materializations == count && creates == 1 && pops == 1 && destroys == 1 && stack.size() == 2,
                "every registered slot materializes and nested caller context is restored");
            const std::vector<uint8_t> fontPayload{5,6,7,8};
            Reset(); fontAt = count;
            result = detail::ValidateUsing(lazyApi, ampere_native_cache::ProgramView(complete), EntryView(completeEntries), 42, {&fontPayload});
            Check(result.Ready() && loads == count + 1 && functions == count + 1 && unloads == count + 1
                && materializations == count + 1 && creates == 1 && pops == 1 && destroys == 1 && stack.size() == 2,
                "FG plus font materialize together in exactly one private context");
            Reset(); fontAt = count; failSlot = static_cast<int>(count); loadError = 209;
            result = detail::ValidateUsing(lazyApi, ampere_native_cache::ProgramView(complete), EntryView(completeEntries), 42, {&fontPayload});
            Check(result.NativeImageUnsupported() && result.slot == count && loads == count + 1
                && unloads == count && creates == 1 && pops == 1 && destroys == 1 && stack.size() == 2,
                "font image incompatibility cleans the shared context before complete-program fallback");
            Reset(); fontAt = count; fontFunctionError = 500;
            result = detail::ValidateUsing(lazyApi, ampere_native_cache::ProgramView(complete), EntryView(completeEntries), 42, {&fontPayload});
            Check(!result.Ready() && !result.NativeImageUnsupported() && result.slot == count
                && unloads == count + 1 && result.contextRestored && stack.size() == 2,
                "missing font symbol rejects complete program and restores caller context");
            Reset(); failSlot = static_cast<int>(count - 1); loadError = 209;
            result = detail::ValidateUsing(lazyApi, ampere_native_cache::ProgramView(complete), EntryView(completeEntries), 42);
            Check(result.NativeImageUnsupported() && result.slot == count - 1 && loads == count
                && functions == count - 1 && unloads == count - 1 && result.contextRestored && stack.size() == 2,
                "final registered native load failure cleans every prior module and restores context");
            Reset(); completeEntries.pop_back();
            result = detail::ValidateUsing(lazyApi, ampere_native_cache::ProgramView(complete), EntryView(completeEntries), 42);
            Check(!result.Ready() && creates == 0 && loads == 0, "incomplete entry-name array rejects before CUDA use");
        }
        for (const size_t count : {24u,39u})
        {
            ampere_native_cache::CompleteProgram complete(count, {1,2,3,4});
            std::vector<Entry> completeEntries(count);
            Reset();
            result = detail::ValidateUsing(Api(), ampere_native_cache::ProgramView(complete), EntryView(completeEntries), 42);
            Check(!result.Ready() && creates == 0 && loads == 0, "out-of-range complete program count rejects before CUDA use");
        }
        puts("CUDA_PROGRAM_VALIDATION_PASSED: full program, kernel errors, adapter identity, context stack and cleanup ownership");
        return 0;
    }
    catch (const std::exception& e) { fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
}
