#pragma once
#include "single_module.h"

namespace single_overlay
{
void ArmFactoryGateway() noexcept;
// The existing backend boundary rechecks application imports only.
void BeforeStreamlineInit() noexcept;
void InstallKnownModules() noexcept;
FARPROC ResolveProc(HMODULE module, LPCSTR name, FARPROC original) noexcept;
void ReadStatus(MfgSingleModuleStatus& status) noexcept;
}
