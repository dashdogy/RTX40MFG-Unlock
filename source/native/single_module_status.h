#pragma once
#include <Windows.h>
#include <cstdint>

struct MfgSingleModuleStatus
{
    uint32_t size;
    uint32_t version;
    uint32_t backendOwner;
    uint32_t duplicateSuppressed;
    uint32_t dxgiHooked;
    uint32_t vulkanHooked;
    uint32_t overlayVisible;
    uint32_t renderFailures;
    uint32_t shortcutVirtualKey;
    uint32_t shortcutBindingActive;
    uint64_t dxgiFrames;
    uint64_t vulkanFrames;
    uint64_t renderedFrames;
};
using MfgSingleModuleQueryFn = BOOL (WINAPI*)(MfgSingleModuleStatus*);
