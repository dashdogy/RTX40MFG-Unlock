#include "reshade_frontend.h"

#include <Windows.h>

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);
        return reshade_frontend::ProcessAttach(instance) ? TRUE : FALSE;
    }
    if (reason == DLL_PROCESS_DETACH)
        reshade_frontend::ProcessDetach(instance);
    return TRUE;
}
