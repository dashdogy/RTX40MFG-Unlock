#pragma once
#include <Windows.h>
#include <string>
#include <filesystem>
#include "build_variant.h"

namespace unified_control_paths
{
inline std::wstring EnvironmentPath(const wchar_t* name)
{
    std::wstring value(32768, L'\0');
    const DWORD length = GetEnvironmentVariableW(name, value.data(),
        static_cast<DWORD>(value.size()));
    if (!length || length >= value.size()) return {};
    value.resize(length);
    return value;
}

inline std::wstring Beside(const std::wstring& directory, const wchar_t* name)
{
    return directory + ((!directory.empty() &&
        (directory.back() == L'\\' || directory.back() == L'/')) ? L"" : L"\\") + name;
}

inline std::wstring Resolve(const std::wstring& directory, const std::wstring& path)
{
    // Environment overrides have one stable base in both the early backend
    // and the later UI, regardless of changes to the process working directory.
    return (std::filesystem::path(directory) / std::filesystem::path(path))
        .lexically_normal().wstring();
}

inline std::wstring Config(const std::wstring& executableDirectory)
{
    const auto explicitPath = EnvironmentPath(MFG_CONFIG_ENV_W);
    return explicitPath.empty() ? Beside(executableDirectory, MFG_CONFIG_W)
        : Resolve(executableDirectory, explicitPath);
}

inline std::wstring ProcessStatusPath(const std::wstring& path, DWORD processId)
{
    if (path.empty() || !processId) return {};
    // A game and a helper loaded from the same directory can both own a
    // backend. Their transient status (and its .tmp publication) must not
    // replace each other. Settings remain shared across process restarts.
    const std::filesystem::path requested(path);
    const std::wstring name = requested.filename().wstring();
    constexpr wchar_t suffix[] = L".status.json";
    const size_t insertion = name.ends_with(suffix)
        ? name.size() - (std::size(suffix) - 1)
        : requested.stem().wstring().size();
    return (requested.parent_path() / (name.substr(0, insertion) + L"."
        + std::to_wstring(processId) + name.substr(insertion))).wstring();
}

inline std::wstring Status(const std::wstring& configPath,
    const std::wstring& executableDirectory, DWORD processId = GetCurrentProcessId())
{
    const auto explicitPath = EnvironmentPath(MFG_STATUS_ENV_W);
    if (!explicitPath.empty())
        return ProcessStatusPath(Resolve(executableDirectory, explicitPath), processId);
    const auto separator = configPath.find_last_of(L"\\/");
    return ProcessStatusPath(Beside(separator == std::wstring::npos ? executableDirectory
        : configPath.substr(0, separator + 1), MFG_STATUS_W), processId);
}
}
