#pragma once
// The shared panel only needs these settings helpers and its opaque callback
// parameter. No ReShade code, API table, add-on registration or DLL is linked.
#include <Windows.h>
#include "build_variant.h"
#include <string>
#include <type_traits>
#include <cstdlib>
#include <cerrno>
#include <limits>

namespace standalone_ui
{
inline std::wstring Wide(const char* text)
{
    if (!text) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, result.data(), count);
    result.resize(count-1);
    return result;
}
inline std::wstring SettingsPath(const char* filename)
{
    wchar_t executable[32768]{};
    const DWORD count = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    if (!count || count >= std::size(executable)) return {};
    std::wstring path(executable, count);
    const size_t end = path.find_last_of(L"\\/");
    if (end == std::wstring::npos) return {};
    path.resize(end+1);
    path += Wide(filename);
    return path;
}
}

namespace reshade
{
namespace api { struct effect_runtime; }

template<class T>
bool get_config_value(api::effect_runtime*, const char* section,
    const char* key, T& value)
{
    const auto own = standalone_ui::SettingsPath(MFG_PRODUCT "-UI.ini");
    if (own.empty()) return false;
    const auto wideSection = standalone_ui::Wide(section);
    const auto wideKey = standalone_ui::Wide(key);
    wchar_t text[128]{};
    DWORD count = GetPrivateProfileStringW(wideSection.c_str(), wideKey.c_str(), L"", text, static_cast<DWORD>(std::size(text)), own.c_str());
    if (!count)
    {
        // Read-only migration preserves the former panel's UI-only custom
        // target preference. The backend JSON remains authoritative for mode.
        const auto previous = standalone_ui::SettingsPath("ReShade.ini");
        count = GetPrivateProfileStringW(wideSection.c_str(), wideKey.c_str(), L"", text, static_cast<DWORD>(std::size(text)), previous.c_str());
    }
    if (!count || count >= std::size(text)-1) return false;
    wchar_t* end = nullptr;
    errno = 0;
    const long long parsed = wcstoll(text, &end, 10);
    if (end == text || *end || errno == ERANGE
        || parsed < static_cast<long long>(std::numeric_limits<T>::lowest())
        || parsed > static_cast<long long>(std::numeric_limits<T>::max())) return false;
    value = static_cast<T>(parsed);
    return true;
}

template<class T>
bool set_config_value(api::effect_runtime*, const char* section,
    const char* key, T value)
{
    const auto path = standalone_ui::SettingsPath(MFG_PRODUCT "-UI.ini");
    if (path.empty()) return false;
    const auto text = std::to_wstring(static_cast<int>(value));
    return WritePrivateProfileStringW(standalone_ui::Wide(section).c_str(),
        standalone_ui::Wide(key).c_str(), text.c_str(), path.c_str()) != FALSE;
}
}
