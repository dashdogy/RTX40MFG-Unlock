#pragma once
#include <Windows.h>
#include "build_variant.h"
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <string>

namespace diagnostic_paths {
inline uint64_t ProcessBirth() noexcept {
    static const uint64_t birth=[] {
        FILETIME created{},exited{},kernel{},user{};
        if(!GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user))return uint64_t(0);
        return (uint64_t(created.dwHighDateTime)<<32)|created.dwLowDateTime;
    }();
    return birth;
}
inline uint64_t PathHash(const std::wstring& path) noexcept {
    uint64_t hash=14695981039346656037ull;
    for(wchar_t ch:path) {
        if(ch==L'/')ch=L'\\';
        if(ch>=L'A'&&ch<=L'Z')ch+=L'a'-L'A';
        hash^=uint16_t(ch);hash*=1099511628211ull;
    }
    return hash;
}
inline std::wstring Executable() {
    std::wstring path(32768,L'\0');
    const DWORD count=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
    if(!count||count>=path.size())return {};
    path.resize(count);return path;
}
inline std::wstring FileName(const std::wstring& executable,const wchar_t* suffix=L".log") {
    if(executable.empty())return {};
    auto stem=std::filesystem::path(executable).stem().wstring();
    if(stem.size()>40)stem.resize(40);
    for(auto& ch:stem) {
        if(ch>=L'A'&&ch<=L'Z')ch+=L'a'-L'A';
        if(!((ch>=L'a'&&ch<=L'z')||(ch>=L'0'&&ch<=L'9')||ch==L'-'||ch==L'_'))ch=L'_';
    }
    wchar_t identity[24]{};
    swprintf_s(identity,L"-%016llX",static_cast<unsigned long long>(PathHash(executable)));
    return MFG_LOG_PREFIX_W L"-"+stem+identity+suffix;
}
inline std::wstring RuntimeLog() {
    std::wstring directory(32768,L'\0');
    const DWORD count=GetTempPathW(static_cast<DWORD>(directory.size()),directory.data());
    if(!count||count>=directory.size())return {};
    directory.resize(count);
    const auto name=FileName(Executable());
    return name.empty()?std::wstring{}:(std::filesystem::path(directory)/name).wstring();
}
}
