#include <Windows.h>
#include <filesystem>
#include <iostream>

int wmain(int argc, wchar_t** argv)
{
    if (argc != 3) return 1;
    const auto shim = LoadLibraryExW(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!shim)
    {
        std::cerr << "loader failed: " << GetLastError() << '\n';
        return 2;
    }
    const auto core = GetModuleHandleW(L"RTX40MFGCore.dll");
    wchar_t path[32768]{};
    if (!core || !GetModuleFileNameW(core, path, 32768)) return 3;
    std::error_code error;
    if (!std::filesystem::equivalent(path, argv[2], error) || error) return 4;
    const auto loaded = reinterpret_cast<BOOL(WINAPI*)()>(GetProcAddress(core, "MfgUnlockCoreLoaded"));
    if (!loaded || !loaded()) return 5;
    std::wcout << L"CORE_PATH=" << path << L'\n';
    std::cout << "V12_ORIGINAL_SHIM_HOTFIX_CORE_PASSED\n" << std::flush;
    // These modules own process-lifetime workers; exercise normal process exit.
    ExitProcess(0);
}
