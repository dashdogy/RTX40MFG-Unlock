#pragma once
#include "ampere_native_cache.h"
#include <Windows.h>
#include <array>

namespace ampere_cuda_program
{
using Entry = std::array<char, 96>;
using Entries = std::array<Entry, ampere_native_cache::kSlots>;
using EntryView = std::span<const Entry>;
struct FontImage { const std::vector<uint8_t>* payload = nullptr; };
enum class Stage : uint32_t { eReady, eApi, eAdapter, eContext, eModule, eFunction, eCleanup };
struct Result
{
    Stage stage = Stage::eApi;
    int error = 0;
    uint32_t slot = UINT32_MAX;
    bool contextRestored = false;
    bool Ready() const noexcept { return stage == Stage::eReady && contextRestored; }
    bool NativeImageUnsupported() const noexcept
    {
        // Only explicit image/compiler compatibility errors permit AUTO to
        // consider the already validated PTX program. OOM, device loss and
        // context/cleanup failures must not trigger a second driver attempt.
        return contextRestored && (stage == Stage::eModule || stage == Stage::eFunction)
            && (error == 200 || error == 209 || error == 218 || error == 222);
    }
};

// A private context is used only to load and resolve the complete program.
// No kernel is launched, no primary context is retained/reset, and the caller's
// current context must be restored before a program can be published.
Result Validate(const ampere_native_cache::Program&, const Entries&, uint64_t luid) noexcept;
Result Validate(ampere_native_cache::ProgramView, EntryView, uint64_t luid, FontImage font = {}) noexcept;

namespace detail
{
struct Api
{
    int (WINAPI* init)(unsigned) = nullptr;
    int (WINAPI* count)(int*) = nullptr;
    int (WINAPI* device)(int*, int) = nullptr;
    int (WINAPI* capability)(int*, int*, int) = nullptr;
    int (WINAPI* luid)(char*, unsigned*, int) = nullptr;
    int (WINAPI* current)(void**) = nullptr;
    int (WINAPI* create)(void**, unsigned, int) = nullptr;
    int (WINAPI* pop)(void**) = nullptr;
    int (WINAPI* destroy)(void*) = nullptr;
    int (WINAPI* load)(void**, const void*) = nullptr;
    int (WINAPI* function)(void**, void*, const char*) = nullptr;
    int (WINAPI* functionLoad)(void*) = nullptr; // Optional on older CUDA drivers.
    int (WINAPI* unload)(void*) = nullptr;
};
Result ValidateUsing(const Api&, const ampere_native_cache::Program&, const Entries&, uint64_t luid) noexcept;
Result ValidateUsing(const Api&, ampere_native_cache::ProgramView, EntryView, uint64_t luid, FontImage font = {}) noexcept;
}
}
