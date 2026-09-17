#pragma once
#include <Windows.h>

namespace single_overlay::application_imports {
// Loaded application renderer DLLs only. No loading, graphics creation, entry
// patching, waiting, or work in loader-notification callbacks.
void ArmModule(HMODULE module) noexcept;
void ArmStartupDependencies() noexcept;
}
