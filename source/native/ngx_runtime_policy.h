#pragma once

#include "dlssg_provider_policy.h"
#include <Windows.h>

namespace ngx_runtime_policy
{
inline bool OwnedExport(HMODULE module, const char* name) noexcept
{
    const void* address = reinterpret_cast<const void*>(GetProcAddress(module, name));
    MEMORY_BASIC_INFORMATION memory{};
    return address && VirtualQuery(address, &memory, sizeof(memory))
        && memory.AllocationBase == module && memory.Type == MEM_IMAGE
        && memory.State == MEM_COMMIT
        && !(memory.Protect & (PAGE_GUARD | PAGE_NOACCESS))
        && (memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ
            | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}

inline bool IsRuntime(HMODULE module) noexcept
{
    if (!module || GetProcAddress(module, "NVSDK_NGX_GetAPIVersion")
        || GetProcAddress(module, "NVSDK_NGX_GetGPUArchitecture")
        || dlssg_provider_policy::IsDlssgImplementationModule(module))
        return false;
    const bool d3d12 = OwnedExport(module, "NVSDK_NGX_D3D12_CreateFeature")
        && OwnedExport(module, "NVSDK_NGX_D3D12_EvaluateFeature")
        && OwnedExport(module, "NVSDK_NGX_D3D12_GetFeatureRequirements")
        && OwnedExport(module, "NVSDK_NGX_D3D12_Init_ProjectID");
    const bool vulkan = (OwnedExport(module, "NVSDK_NGX_VULKAN_CreateFeature")
            || OwnedExport(module, "NVSDK_NGX_VULKAN_CreateFeature1"))
        && OwnedExport(module, "NVSDK_NGX_VULKAN_EvaluateFeature")
        && OwnedExport(module, "NVSDK_NGX_VULKAN_GetFeatureRequirements");
    const bool projectInit = OwnedExport(module, "NVSDK_NGX_VULKAN_Init_with_ProjectID")
        || OwnedExport(module, "NVSDK_NGX_VULKAN_Init_ProjectID")
        || OwnedExport(module, "NVSDK_NGX_VULKAN_Init_ProjectID_Ext");
    // An embedded SDK runtime may export only the application-ID Init family.
    // Require its owned capability and release contract as well; a provider
    // or a library forwarding an Init symbol is not a runtime identity.
    const bool applicationInit = (OwnedExport(module, "NVSDK_NGX_VULKAN_Init")
            || OwnedExport(module, "NVSDK_NGX_VULKAN_Init_Ext")
            || OwnedExport(module, "NVSDK_NGX_VULKAN_Init_Ext2"))
        && OwnedExport(module, "NVSDK_NGX_VULKAN_GetCapabilityParameters")
        && OwnedExport(module, "NVSDK_NGX_VULKAN_ReleaseFeature");
    return d3d12 || (vulkan && (projectInit || applicationInit));
}

inline bool IsRemixRuntime(HMODULE module) noexcept
{
    return IsRuntime(module) && OwnedExport(module, "remixapi_InitializeLibrary")
        && OwnedExport(module, "NVSDK_NGX_VULKAN_GetCapabilityParameters");
}
}
