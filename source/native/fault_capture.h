#pragma once
#include "entry_detour.h"

#ifndef MFG_UNLOCK_GPU_FAULT_CAPTURE
#define MFG_UNLOCK_GPU_FAULT_CAPTURE 0
#endif

#ifndef MFG_UNLOCK_NGX_CREATE_RESULT_DIAGNOSTICS
#define MFG_UNLOCK_NGX_CREATE_RESULT_DIAGNOSTICS 0
#endif

namespace fault_capture
{
using LogCallback = void (*)(const wchar_t*);
#if MFG_UNLOCK_GPU_FAULT_CAPTURE || MFG_UNLOCK_NGX_CREATE_RESULT_DIAGNOSTICS
void Initialize(LogCallback log) noexcept;
void BeforeSlInit() noexcept;
void ObserveDevice(void* device, bool creationObserved = false) noexcept;
FARPROC ResolveProc(HMODULE module, LPCSTR name, FARPROC resolved) noexcept;
void ProviderForward(entry_detour::Handle entry, const void* caller,
    const void* command, uintptr_t handleOrFeature, const void* parameters) noexcept;

// Records the result of the EXISTING ordinary runtime call. Provider entries
// keep their assembly tail-forwarding ABI and only report entry observations.
class NgxCall
{
public:
    NgxCall(entry_detour::Handle entry, const void* caller, const void* command,
        uintptr_t handleOrFeature, const void* parameters) noexcept;
    ~NgxCall();
    void Complete(uint32_t result, void* const* output = nullptr) noexcept;
    NgxCall(const NgxCall&) = delete;
    NgxCall& operator=(const NgxCall&) = delete;
private:
    uint64_t id_ = 0, parent_ = 0;
    entry_detour::Snapshot entry_{};
    const void* caller_ = nullptr;
    const void* command_ = nullptr;
    const void* parameters_ = nullptr;
    uintptr_t handleOrFeature_ = 0;
    bool completed_ = false;
};
#else
inline void Initialize(LogCallback) noexcept {}
inline void BeforeSlInit() noexcept {}
inline void ObserveDevice(void*, bool = false) noexcept {}
inline FARPROC ResolveProc(HMODULE, LPCSTR, FARPROC resolved) noexcept { return resolved; }
inline void ProviderForward(entry_detour::Handle, const void*, const void*, uintptr_t, const void*) noexcept {}
class NgxCall
{
public:
    NgxCall(entry_detour::Handle, const void*, const void*, uintptr_t, const void*) noexcept {}
    void Complete(uint32_t, void* const* = nullptr) noexcept {}
};
#endif
}
