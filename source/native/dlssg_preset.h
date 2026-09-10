#pragma once

#include <Windows.h>
#include <cstdint>

namespace dlssg_preset
{
struct SelectionSnapshot
{
    uint32_t requested = 2;
    uint32_t latched = 2;
    bool selectionFrozen = false;
    bool restartRequired = false;
};

struct Snapshot
{
    bool installed = false;
    uint32_t failure = 0;
    uint64_t providerReads = 0;
    bool cachedReaderInstalled = false;
    uint32_t cachedReaderFailure = 0;
    uint32_t cachedReaderRva = 0;
    uint64_t cachedReaderReads = 0;
    uint32_t requested = 2;
    uint32_t latched = 2;
    bool selectionFrozen = false;
    bool restartRequired = false;
    uint32_t observedPreset = 0;
    bool observedPresetValid = false;
    uint64_t overrideReads = 0;
};

// 0 leaves game/driver reads untouched; 1 selects A and 2 selects B. A process
// keeps one selection after its first relevant provider query. Later requests
// are saved for restart, without replacing cached settings or live programs.
bool SetRequested(uint32_t preset) noexcept;
uint32_t FreezeSelection() noexcept;
SelectionSnapshot ReadSelection() noexcept;
using InitialSelectionLoader = uint32_t (*)() noexcept;
// Assign before enabling hooks. Only FreezeSelection calls this loader, once,
// outside the state lock and only if SetRequested has not already initialized it.
void SetInitialSelectionLoader(InitialSelectionLoader loader) noexcept;
// Query-result evidence only. The caller owns a validated, process-retained
// exact provider route and has established output-buffer ownership.
void RecordProviderRead(HMODULE provider, bool valueValid, uint32_t value, bool overridden) noexcept;

// Call at a normal initialization/API boundary, outside DllMain. Installs only
// a process-memory read hook; no NVIDIA profile set/save API is used.
bool Prepare() noexcept;
void PrepareForExport(HMODULE module, const char* name) noexcept;
// Bounded entry discovery; no NVIDIA initialization is invoked here.
void ObserveProvider(HMODULE module, uint64_t generation) noexcept;
Snapshot ReadSnapshot(HMODULE provider) noexcept;
}
