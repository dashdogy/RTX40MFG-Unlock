#include <Windows.h>
#include <sl.h>
#include <sl_dlss_g.h>

#include <atomic>
#include <string>

extern "C" void WrapperSignature();

namespace
{
std::atomic<uint32_t> gActualMultiplier{1};
std::atomic<bool> gTransientNotInitializedReturned{false};
std::atomic<bool> gUiRecompositionEnabled{false};

uint32_t MaximumGeneratedFrames()
{
    const auto* signature = reinterpret_cast<const uint8_t*>(&WrapperSignature);
    return signature[0] == 0xBA ? signature[1] : 0;
}

bool EnvironmentEnabled(const wchar_t* name)
{
    wchar_t value[8]{};
    const DWORD length = GetEnvironmentVariableW(name, value, _countof(value));
    return length > 0 && length < _countof(value) && value[0] != L'0';
}

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
    gUiRecompositionEnabled.store(options.structVersion >= sl::kStructVersion4
        && options.enableUserInterfaceRecomposition == sl::Boolean::eTrue,
        std::memory_order_relaxed);
    const std::wstring ngxPath = NgxPath();
    if (!GetModuleHandleW(ngxPath.c_str())
        && !LoadLibraryW(ngxPath.c_str()))
        return sl::Result::eErrorMissingOrInvalidAPI;
    if (EnvironmentEnabled(L"MFG_HARNESS_TRANSIENT_21")
        && !gTransientNotInitializedReturned.exchange(true, std::memory_order_relaxed))
        return sl::Result::eErrorNotInitialized;
    if (options.mode == sl::DLSSGMode::eDynamic
        && options.structVersion < sl::kStructVersion5)
        return sl::Result::eErrorInvalidParameter;
    if (options.mode == sl::DLSSGMode::eOff)
        gActualMultiplier.store(0, std::memory_order_relaxed);
    else if (options.mode == sl::DLSSGMode::eDynamic)
        gActualMultiplier.store(MaximumGeneratedFrames() + 1, std::memory_order_relaxed);
    else
        gActualMultiplier.store(options.numFramesToGenerate + 1, std::memory_order_relaxed);
    return options.mode == sl::DLSSGMode::eOn && options.numFramesToGenerate == 2
        ? sl::Result::eWarnOutOfVRAM : sl::Result::eOk;
}

extern "C" __declspec(dllexport) uint32_t FakeUiRecompositionEnabled()
{
    return gUiRecompositionEnabled.load(std::memory_order_relaxed) ? 1u : 0u;
}

extern "C" __declspec(dllexport) sl::Result FakeGetState(
    const sl::ViewportHandle&, sl::DLSSGState& state, const sl::DLSSGOptions*)
{
    state.status = sl::DLSSGStatus::eOk;
    state.numFramesActuallyPresented = gActualMultiplier.load(std::memory_order_relaxed);
    if (state.structVersion >= sl::kStructVersion2)
        state.numFramesToGenerateMax = MaximumGeneratedFrames();
    if (state.structVersion >= sl::kStructVersion4)
        state.bIsDynamicMFGSupported = sl::Boolean::eTrue;
    return sl::Result::eOk;
}
