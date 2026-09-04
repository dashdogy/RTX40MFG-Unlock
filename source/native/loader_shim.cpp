#include <Windows.h>

extern "C" __declspec(dllimport) BOOL WINAPI MfgUnlockCoreLoaded();

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);
        // A normal, non-delay import of RTX40MFGCore.dll guarantees that the
        // core's process-attach path ran before this loader-facing ASI entry.
        return MfgUnlockCoreLoaded();
    }
    return TRUE;
}
