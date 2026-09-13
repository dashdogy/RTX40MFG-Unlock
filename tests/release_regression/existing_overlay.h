#pragma once
#include "common.h"
#include "fixture_api.h"
namespace harness {
struct ExistingOverlay {
    HMODULE module=nullptr;
    using Install=BOOL(WINAPI*)();
    using Reset=void(WINAPI*)();
    using Count=unsigned long long(WINAPI*)(unsigned);
    Install intact=nullptr, restored=nullptr;
    Reset reset=nullptr;
    Count count=nullptr;
    void (WINAPI* readPresent)(FixturePresent*)=nullptr;
    bool Load() {
        wchar_t executable[32768]{};
        GetModuleFileNameW(nullptr,executable,32768);
        const auto path=std::filesystem::path(executable).parent_path()/L"ExistingOverlayFixture.dll";
        module=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if(!module)return false;
        auto install=reinterpret_cast<Install>(GetProcAddress(module,"OverlayFixtureInstall"));
        intact=reinterpret_cast<Install>(GetProcAddress(module,"OverlayFixtureCodeIntact"));
        restored=reinterpret_cast<Install>(GetProcAddress(module,"OverlayFixtureSlotsRestored"));
        reset=reinterpret_cast<Reset>(GetProcAddress(module,"OverlayFixtureResetCounts"));
        count=reinterpret_cast<Count>(GetProcAddress(module,"OverlayFixtureCount"));
        readPresent=reinterpret_cast<decltype(readPresent)>(GetProcAddress(module,"OverlayFixtureReadPresent"));
        return install&&intact&&restored&&reset&&count&&readPresent&&install();
    }
};
}
