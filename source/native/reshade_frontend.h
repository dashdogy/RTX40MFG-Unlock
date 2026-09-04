#pragma once

#include <Windows.h>

namespace reshade_frontend
{
#if defined(MFG_UNLOCK_RESHADE_ADDON)
bool ProcessAttach(HMODULE self) noexcept;
void ProcessDetach(HMODULE self) noexcept;
#else
inline bool ProcessAttach(HMODULE) noexcept { return true; }
inline void ProcessDetach(HMODULE) noexcept {}
#endif
}
