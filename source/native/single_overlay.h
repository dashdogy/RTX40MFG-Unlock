#pragma once
#include "single_module.h"

namespace single_overlay
{
// Entry interception only: these never create graphics objects or an ImGui
// context. Graphics initialization happens on a real presentation callback.
void InstallKnownModules() noexcept;
FARPROC ResolveProc(HMODULE module, LPCSTR name, FARPROC original) noexcept;
void ReadStatus(MfgSingleModuleStatus& status) noexcept;
}
