#pragma once

#include <Windows.h>

#include <cstdint>

struct IDXGISwapChain;
struct IUnknown;
struct ID3D12CommandList;
struct ID3D12CommandQueue;

namespace present_probe
{
using LogCallback = void (*)(const wchar_t* message);

struct Snapshot
{
    bool factoryImportHookInstalled = false;
    bool nativeFactoryHookInstalled = false;
    bool nativeSwapchainHookInstalled = false;
    bool enabled = false;
    uint64_t nativePresentCalls = 0;
    uint64_t scheduledFrames = 0;
    uint64_t capturedFrames = 0;
    uint64_t droppedFrames = 0;
};

bool Install(HMODULE executable, LogCallback logCallback);
bool RegisterSwapchain(IDXGISwapChain* swapchain, IUnknown* presentationQueue);
bool UnregisterSwapchain(IDXGISwapChain* swapchain);
uint32_t PrepareQueueCaptures(ID3D12CommandQueue* queue,
    ID3D12CommandList** commandLists, uint32_t capacity);
void CompleteQueueCaptures(ID3D12CommandQueue* queue);
void SetEnabled(bool enabled);
Snapshot ReadSnapshot();
}
