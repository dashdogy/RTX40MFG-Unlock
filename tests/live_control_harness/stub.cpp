#include <Windows.h>
#include <sl.h>
#include <sl_core_api.h>
#include <atomic>
#include <cstring>
#include <string>

namespace
{
std::atomic<uint32_t> gFreeResourcesCalls{0};
std::atomic<uint32_t> gFreeResourcesOrderViolations{0};

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

SL_API sl::Result slFreeResources(
    sl::Feature feature, const sl::ViewportHandle&)
{
    if (feature != sl::kFeatureDLSS_G)
        return sl::Result::eErrorFeatureNotSupported;
    const std::wstring wrapperPath = WrapperPath();
    HMODULE wrapper = GetModuleHandleW(wrapperPath.c_str());
    using ActualMultiplierFn = uint32_t();
    auto* actualMultiplier = wrapper
        ? reinterpret_cast<ActualMultiplierFn*>(
            GetProcAddress(wrapper, "FakeActualMultiplier"))
        : nullptr;
    if (!actualMultiplier || actualMultiplier() != 0)
        gFreeResourcesOrderViolations.fetch_add(1, std::memory_order_relaxed);
    gFreeResourcesCalls.fetch_add(1, std::memory_order_relaxed);
    return sl::Result::eOk;
}

extern "C" __declspec(dllexport) uint32_t FakeFreeResourcesCalls()
{
    return gFreeResourcesCalls.load(std::memory_order_relaxed);
}

extern "C" __declspec(dllexport) uint32_t FakeFreeResourcesOrderViolations()
{
    return gFreeResourcesOrderViolations.load(std::memory_order_relaxed);
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

SL_API sl::Result slSetTag(const sl::ViewportHandle&, const sl::ResourceTag*,
    uint32_t, sl::CommandBuffer*)
{
    return sl::Result::eOk;
}

SL_API sl::Result slSetTagForFrame(const sl::FrameToken&,
    const sl::ViewportHandle&, const sl::ResourceTag*, uint32_t,
    sl::CommandBuffer*)
{
    return sl::Result::eOk;
}
