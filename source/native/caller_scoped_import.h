#pragma once
#include <Windows.h>

// Unaligned application imports remain untouched. The fallback changes only
// an identified public system entry, and dispatches to the replacement only
// when the original return address belongs to the main executable.
namespace caller_scoped_import {
bool Eligible(HMODULE importer, void** slot, void* expected, const char* name) noexcept;
// Prepare is transparent. Publish the returned trampoline into the typed
// forwarder's original pointer before Activate; never recurse via the entry.
bool Prepare(HMODULE importer, void** slot, void* expected, void* replacement, const char* name, void*& original) noexcept;
bool Activate(void* expected, void* replacement) noexcept;
bool Deactivate(void* expected, void* replacement) noexcept;
}
