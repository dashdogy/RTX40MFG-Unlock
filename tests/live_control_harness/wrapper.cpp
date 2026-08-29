#include <Windows.h>
#include <sl.h>
#include <sl_dlss_g.h>

#include <atomic>
#include <string>

namespace
{
std::atomic<uint32_t> gActualMultiplier{1};

std::wstring NgxPath()
{
    wchar_t path[32768]{};
    const DWORD length = GetEnvironmentVariableW(
        L"MFG_HARNESS_NGX_PATH", path, _countof(path));
    return length > 0 && length < _countof(path)
        ? std::wstring(path, length) : L"nvngx_dlssg.dll";
}
}

extern "C" __declspec(dllexport) void* slGetPluginFunction(const char*)
{
    return nullptr;
}

extern "C" __declspec(dllexport) sl::Result FakeSetOptions(
    const sl::ViewportHandle&, const sl::DLSSGOptions& options)
{
    const std::wstring ngxPath = NgxPath();
    if (!GetModuleHandleW(ngxPath.c_str())
        && !LoadLibraryW(ngxPath.c_str()))
        return sl::Result::eErrorMissingOrInvalidAPI;
    if (options.mode == sl::DLSSGMode::eDynamic
        && options.structVersion < sl::kStructVersion5)
        return sl::Result::eErrorInvalidParameter;
    if (options.mode == sl::DLSSGMode::eOff)
        gActualMultiplier.store(0, std::memory_order_relaxed);
    else if (options.mode == sl::DLSSGMode::eDynamic)
        gActualMultiplier.store(4, std::memory_order_relaxed);
    else
        gActualMultiplier.store(options.numFramesToGenerate + 1, std::memory_order_relaxed);
    return options.mode == sl::DLSSGMode::eOn && options.numFramesToGenerate == 2
        ? sl::Result::eWarnOutOfVRAM : sl::Result::eOk;
}

extern "C" __declspec(dllexport) sl::Result FakeGetState(
    const sl::ViewportHandle&, sl::DLSSGState& state, const sl::DLSSGOptions*)
{
    state.status = sl::DLSSGStatus::eOk;
    state.numFramesActuallyPresented = gActualMultiplier.load(std::memory_order_relaxed);
    if (state.structVersion >= sl::kStructVersion2)
        state.numFramesToGenerateMax = 5;
    if (state.structVersion >= sl::kStructVersion4)
        state.bIsDynamicMFGSupported = sl::Boolean::eTrue;
    return sl::Result::eOk;
}
