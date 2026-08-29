#include <Windows.h>
#include <sl.h>
#include <cstring>
#include <string>

namespace
{
std::wstring WrapperPath()
{
    wchar_t path[32768]{};
    const DWORD length = GetEnvironmentVariableW(
        L"MFG_HARNESS_WRAPPER_PATH", path, _countof(path));
    return length > 0 && length < _countof(path)
        ? std::wstring(path, length) : L"sl.dlss_g.dll";
}

HMODULE LoadHarnessModule(const wchar_t* name)
{
    HMODULE module = GetModuleHandleW(name);
    return module ? module : LoadLibraryW(name);
}
}

SL_API sl::Result slGetFeatureFunction(
    sl::Feature feature, const char* functionName, void*& function)
{
    function = nullptr;
    if (feature != sl::kFeatureDLSS_G || !functionName)
        return sl::Result::eErrorFeatureNotSupported;

    const std::wstring wrapperPath = WrapperPath();
    HMODULE wrapper = LoadHarnessModule(wrapperPath.c_str());
    if (!wrapper)
        return sl::Result::eErrorMissingOrInvalidAPI;
    if (std::strcmp(functionName, "slDLSSGSetOptions") == 0)
        function = reinterpret_cast<void*>(GetProcAddress(wrapper, "FakeSetOptions"));
    else if (std::strcmp(functionName, "slDLSSGGetState") == 0)
        function = reinterpret_cast<void*>(GetProcAddress(wrapper, "FakeGetState"));
    return function ? sl::Result::eOk : sl::Result::eErrorFeatureMissing;
}
