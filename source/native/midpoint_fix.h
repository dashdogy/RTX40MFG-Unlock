#pragma once

#include <Windows.h>

#include <cstdint>

namespace midpoint_fix
{
using LogCallback = void (*)(const wchar_t* message);

void SetLogCallback(LogCallback callback) noexcept;
bool ObserveD3D12Device(void* device) noexcept;
bool ObserveVulkanPhysicalDevice(void* physicalDevice) noexcept;
bool PatchProvider(HMODULE module, const wchar_t* path) noexcept;
bool AdapterVerified() noexcept;
bool Ready() noexcept;
uint32_t FailureCode() noexcept;
}
