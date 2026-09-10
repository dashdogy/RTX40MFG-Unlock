#include "overlay_vulkan.h"
#include "overlay_platform.h"
#include "overlay_hook.h"
#include "overlay_color_spv.h"
#include "ui_input_coherence.h"
#include <backends/imgui_impl_vulkan.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

namespace single_overlay::vulkan
{
namespace
{
std::atomic<PFN_vkGetInstanceProcAddr> gLoaderGipa{nullptr};
PFN_vkVoidFunction Wrap(const char* name, PFN_vkVoidFunction original);

struct Instance
{
    VkInstance handle = VK_NULL_HANDLE;
    uint32_t apiVersion = VK_API_VERSION_1_0;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
};
struct Device
{
    VkDevice handle = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    std::shared_ptr<Instance> instance;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    std::vector<VkQueueFamilyProperties> families;

    PFN_vkVoidFunction Get(const char* name) const
    {
        InternalScope internal;
        if (auto function = gdpa(handle, name)) return function;
        return instance->gipa(instance->handle, name);
    }
};
struct Queue
{
    VkQueue handle = VK_NULL_HANDLE;
    std::shared_ptr<Device> device;
    uint32_t family = UINT32_MAX;
    bool protectedQueue = false;
};
struct Surface
{
    HWND window = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
};

std::mutex gRegistryMutex;
std::unordered_map<VkInstance, std::shared_ptr<Instance>> gInstances;
std::unordered_map<VkPhysicalDevice, std::shared_ptr<Instance>> gPhysicalDevices;
std::unordered_map<VkDevice, std::shared_ptr<Device>> gDevices;
std::unordered_map<VkQueue, Queue> gQueues;
std::unordered_map<VkSurfaceKHR, Surface> gSurfaces;
// Only ordinary, locally created binary semaphores can be consumed and
// re-signaled around the UI draw. Imports can replace a semaphore's payload.
std::unordered_map<VkSemaphore, VkDevice> gNativeSemaphores;
std::atomic<bool> gSemaphoreCoverage{true};

#define MFG_VK_RENDER_FUNCTIONS(X) \
    X(GetSwapchainImagesKHR) X(CreateImageView) X(DestroyImageView) \
    X(CreateRenderPass) X(DestroyRenderPass) X(CreateFramebuffer) X(DestroyFramebuffer) \
    X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) \
    X(CreateFence) X(DestroyFence) X(GetFenceStatus) X(WaitForFences) X(ResetFences) \
    X(ResetCommandPool) \
    X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdBeginRenderPass) X(CmdEndRenderPass) \
    X(QueueSubmit) X(QueueWaitIdle) X(DeviceWaitIdle)

struct RenderFunctions
{
#define MFG_VK_FIELD(name) PFN_vk##name name = nullptr;
    MFG_VK_RENDER_FUNCTIONS(MFG_VK_FIELD)
#undef MFG_VK_FIELD
    bool Load(const Device& device)
    {
#define MFG_VK_LOAD(name) name = reinterpret_cast<PFN_vk##name>(device.Get("vk" #name)); if (!name) return false;
        MFG_VK_RENDER_FUNCTIONS(MFG_VK_LOAD)
#undef MFG_VK_LOAD
        return true;
    }
};

struct Frame
{
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool submitted = false;
};

struct State
{
    std::mutex mutex;
    std::shared_ptr<Device> device;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent{};
    VkImageUsageFlags usage = 0;
    VkSwapchainCreateFlagsKHR flags = 0;
    uint32_t layers = 1;
    uint32_t minImageCount = 2;
    VkSharingMode sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    std::vector<uint32_t> sharingFamilies;
    HWND window = nullptr;
    Queue queue;
    RenderFunctions vk;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    std::vector<Frame> frames;
    PlatformState platform;
    bool initialized = false;
    bool rendererInitialized = false;
    bool disabled = false;

    static PFN_vkVoidFunction LoadImGuiFunction(const char* name, void* user)
    {
        return static_cast<Device*>(user)->Get(name);
    }
    static void CheckResult(VkResult result)
    {
        if (result < 0) RecordFailure(L"Vulkan UI backend reported a graphics error");
    }

    bool Initialize(const Queue& presentQueue)
    {
        if (initialized)
            return queue.handle == presentQueue.handle && queue.device == presentQueue.device;
        if (presentQueue.device != device || presentQueue.protectedQueue
            || presentQueue.family >= device->families.size()
            || !(device->families[presentQueue.family].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            || !(usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) || layers != 1
            || (flags & VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR)
            || !extent.width || !extent.height || !window
            || !IsWindow(window)) return false;
        if (sharingMode == VK_SHARING_MODE_CONCURRENT
            && std::find(sharingFamilies.begin(), sharingFamilies.end(), presentQueue.family) == sharingFamilies.end())
            return false;
        int colorMode = 0;
        if (colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT) colorMode = 1;
        else if (colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT) colorMode = 2;
        else if (colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            if (format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_R8G8B8A8_SRGB) colorMode = 3;
        }
        else return false;
        queue = presentQueue;
        if (!vk.Load(*device)) return false;
        uint32_t count = 0;
        if (vk.GetSwapchainImagesKHR(device->handle, swapchain, &count, nullptr) != VK_SUCCESS
            || count < 2 || count > 16) return false;
        std::vector<VkImage> images(count);
        if (vk.GetSwapchainImagesKHR(device->handle, swapchain, &count, images.data()) != VK_SUCCESS) return false;
        if (!platform.Initialize(window)) return false;
        ContextScope current(platform.context);
        InternalScope internal;
        frames.resize(count);
        VkAttachmentDescription attachment{};
        attachment.format = format;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &reference;
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo renderInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        renderInfo.attachmentCount = 1;
        renderInfo.pAttachments = &attachment;
        renderInfo.subpassCount = 1;
        renderInfo.pSubpasses = &subpass;
        renderInfo.dependencyCount = 1;
        renderInfo.pDependencies = &dependency;
        if (vk.CreateRenderPass(device->handle, &renderInfo, nullptr, &renderPass) != VK_SUCCESS) return false;
        for (uint32_t index = 0; index < count; ++index)
        {
            auto& frame = frames[index];
            frame.image = images[index];
            VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            viewInfo.image = images[index];
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = format;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.layerCount = 1;
            if (vk.CreateImageView(device->handle, &viewInfo, nullptr, &frame.view) != VK_SUCCESS) return false;
            VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            framebuffer.renderPass = renderPass;
            framebuffer.attachmentCount = 1;
            framebuffer.pAttachments = &frame.view;
            framebuffer.width = extent.width;
            framebuffer.height = extent.height;
            framebuffer.layers = 1;
            if (vk.CreateFramebuffer(device->handle, &framebuffer, nullptr, &frame.framebuffer) != VK_SUCCESS) return false;
            VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pool.queueFamilyIndex = queue.family;
            if (vk.CreateCommandPool(device->handle, &pool, nullptr, &frame.pool) != VK_SUCCESS) return false;
            VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            allocation.commandPool = frame.pool;
            allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocation.commandBufferCount = 1;
            if (vk.AllocateCommandBuffers(device->handle, &allocation, &frame.commands) != VK_SUCCESS) return false;
            VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            if (vk.CreateFence(device->handle, &fenceInfo, nullptr, &frame.fence) != VK_SUCCESS) return false;
        }
        if (!ImGui_ImplVulkan_LoadFunctions(device->instance->apiVersion, &LoadImGuiFunction, device.get())) return false;
        ImGui_ImplVulkan_InitInfo info{};
        info.ApiVersion = device->instance->apiVersion;
        info.Instance = device->instance->handle;
        info.PhysicalDevice = device->physical;
        info.Device = device->handle;
        info.QueueFamily = queue.family;
        info.Queue = queue.handle;
        info.DescriptorPoolSize = 128;
        info.MinImageCount = std::clamp(minImageCount, 2u, count);
        info.ImageCount = count;
        info.PipelineInfoMain.RenderPass = renderPass;
        info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        info.CheckVkResultFn = &CheckResult;
        if (colorMode)
        {
            auto& shader = info.CustomShaderFragCreateInfo;
            shader.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            if (colorMode == 1) { shader.codeSize = sizeof(color::shader1); shader.pCode = color::shader1; }
            else if (colorMode == 2) { shader.codeSize = sizeof(color::shader2); shader.pCode = color::shader2; }
            else { shader.codeSize = sizeof(color::shader3); shader.pCode = color::shader3; }
        }
        rendererInitialized = ImGui_ImplVulkan_Init(&info);
        initialized = rendererInitialized;
        return initialized;
    }

    void Disable()
    {
        disabled = true;
        platform.RetireInput();
    }

    bool Drain()
    {
        if (!vk.WaitForFences || !queue.handle) return true;
        for (const auto& frame : frames)
        {
            if (!frame.submitted || !frame.fence) continue;
            const VkResult result = vk.WaitForFences(device->handle, 1, &frame.fence, VK_TRUE, 3000000000ull);
            if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST) return false;
        }
        // The UI owns only draw resources. It never gives an owned semaphore
        // to WSI, whose asynchronous lifetime is not covered by QueueWaitIdle.
        return true;
    }

    bool Shutdown()
    {
        if (!Drain())
        {
            Disable();
            RecordFailure(L"Vulkan UI drain timed out; GPU resources retained");
            return false;
        }
        std::lock_guard uiLock(gUiMutex);
        InternalScope internal;
        if (platform.context)
        {
            ContextScope current(platform.context);
            if (rendererInitialized)
            {
                ImGui_ImplVulkan_LoadFunctions(device->instance->apiVersion, &LoadImGuiFunction, device.get());
                ImGui_ImplVulkan_Shutdown();
            }
        }
        rendererInitialized = false;
        for (auto& frame : frames)
        {
            if (frame.framebuffer) vk.DestroyFramebuffer(device->handle, frame.framebuffer, nullptr);
            if (frame.view) vk.DestroyImageView(device->handle, frame.view, nullptr);
            if (frame.pool) vk.DestroyCommandPool(device->handle, frame.pool, nullptr);
            if (frame.fence) vk.DestroyFence(device->handle, frame.fence, nullptr);
        }
        frames.clear();
        if (renderPass) vk.DestroyRenderPass(device->handle, renderPass, nullptr);
        renderPass = VK_NULL_HANDLE;
        platform.Shutdown();
        initialized = false;
        return true;
    }

    // Wait at ALL_COMMANDS, draw, then re-signal the same proven native binary
    // semaphores. Present keeps its original wait set and owns their lifetime.
    bool Render(const Queue& presentQueue, uint32_t index,
        const VkPresentInfoKHR& present, VkResult& failure)
    {
        std::lock_guard stateLock(mutex);
        if (disabled) return false;
        std::lock_guard uiLock(gUiMutex);
        InternalScope internal;
        if (!Initialize(presentQueue))
        {
            Disable();
            RecordFailure(L"Vulkan UI initialization rejected for this swapchain");
            Shutdown();
            return false;
        }
        if (!platform.WantsFrame()) return false;
        if (index >= frames.size()) { Disable(); return false; }
        auto& frame = frames[index];
        if (frame.submitted && vk.WaitForFences(device->handle, 1, &frame.fence,
                VK_TRUE, 3000000000ull) != VK_SUCCESS)
        {
            Disable();
            RecordFailure(L"Vulkan UI frame fence timed out");
            return false;
        }
        ContextScope current(platform.context);
        // The pinned backend has one dispatch table. Serialize and select the
        // exact device before touching a context, including multiple devices.
        if (!ImGui_ImplVulkan_LoadFunctions(device->instance->apiVersion, &LoadImGuiFunction, device.get())) return false;
        ImGui_ImplVulkan_NewFrame();
        if (!platform.NewFrame(extent.width, extent.height)) return false;
        platform.Draw();
        if (vk.ResetCommandPool(device->handle, frame.pool, 0) != VK_SUCCESS) return false;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vk.BeginCommandBuffer(frame.commands, &begin) != VK_SUCCESS) return false;
        VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        pass.renderPass = renderPass;
        pass.framebuffer = frame.framebuffer;
        pass.renderArea.extent = extent;
        vk.CmdBeginRenderPass(frame.commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), frame.commands);
        vk.CmdEndRenderPass(frame.commands);
        if (vk.EndCommandBuffer(frame.commands) != VK_SUCCESS) return false;
        if (vk.ResetFences(device->handle, 1, &frame.fence) != VK_SUCCESS) return false;
        frame.submitted = false;
        std::vector<VkPipelineStageFlags> stages(present.waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = present.waitSemaphoreCount;
        submit.pWaitSemaphores = present.pWaitSemaphores;
        submit.pWaitDstStageMask = stages.data();
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &frame.commands;
        submit.signalSemaphoreCount = present.waitSemaphoreCount;
        submit.pSignalSemaphores = present.pWaitSemaphores;
        const VkResult result = vk.QueueSubmit(queue.handle, 1, &submit, frame.fence);
        if (result != VK_SUCCESS)
        {
            Disable();
            failure = result;
            RecordFailure(L"Vulkan UI submission failed");
            return false;
        }
        frame.submitted = true;
        gRenderedFrames.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
};

std::unordered_map<VkSwapchainKHR, std::shared_ptr<State>> gSwapchains;
std::vector<std::shared_ptr<State>> gRetained;

std::shared_ptr<Device> FindDevice(VkDevice handle)
{
    std::lock_guard lock(gRegistryMutex);
    const auto found = gDevices.find(handle);
    return found == gDevices.end() ? nullptr : found->second;
}

void RememberQueue(VkDevice handle, uint32_t family, bool isProtected, VkQueue queue)
{
    if (!queue) return;
    auto device = FindDevice(handle);
    if (!device || family >= device->families.size()) return;
    std::lock_guard lock(gRegistryMutex);
    if (gQueues.size() < 128)
        gQueues[queue] = Queue{queue, device, family, isProtected};
}

void RememberDevice(VkPhysicalDevice physical, VkDevice handle)
{
    std::shared_ptr<Instance> instance;
    {
        std::lock_guard lock(gRegistryMutex);
        const auto found = gPhysicalDevices.find(physical);
        if (found != gPhysicalDevices.end()) instance = found->second;
    }
    if (!instance || !handle) return;
    InternalScope internal;
    auto device = std::make_shared<Device>();
    device->handle = handle;
    device->physical = physical;
    device->instance = instance;
    device->gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(instance->gipa(instance->handle, "vkGetDeviceProcAddr"));
    auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        instance->gipa(instance->handle, "vkGetPhysicalDeviceQueueFamilyProperties"));
    if (!device->gdpa || !properties) return;
    uint32_t count = 0;
    properties(physical, &count, nullptr);
    if (!count || count > 64) return;
    device->families.resize(count);
    properties(physical, &count, device->families.data());
    device->families.resize(count);
    std::lock_guard lock(gRegistryMutex);
    if (gDevices.size() < 32) gDevices[handle] = std::move(device);
}

struct GipaTag
{
    static PFN_vkVoidFunction Call(PFN_vkGetInstanceProcAddr original, VkInstance instance, const char* name)
    {
        const auto result = original(instance, name);
        return gInsideOverlay ? result : Wrap(name, result);
    }
};
struct GdpaTag
{
    static PFN_vkVoidFunction Call(PFN_vkGetDeviceProcAddr original, VkDevice device, const char* name)
    {
        const auto result = original(device, name);
        return gInsideOverlay ? result : Wrap(name, result);
    }
};
struct CreateInstanceTag
{
    static VkResult Call(PFN_vkCreateInstance original, const VkInstanceCreateInfo* info,
        const VkAllocationCallbacks* allocator, VkInstance* output)
    {
        const VkResult result = original(info, allocator, output);
        if (!gInsideOverlay && result == VK_SUCCESS && output && info)
        {
            auto instance = std::make_shared<Instance>();
            instance->handle = *output;
            instance->gipa = gLoaderGipa.load(std::memory_order_acquire);
            if (info->pApplicationInfo && info->pApplicationInfo->apiVersion)
                instance->apiVersion = info->pApplicationInfo->apiVersion;
            std::lock_guard lock(gRegistryMutex);
            if (instance->gipa && gInstances.size() < 16) gInstances[*output] = std::move(instance);
        }
        return result;
    }
};
struct EnumeratePhysicalTag
{
    static VkResult Call(PFN_vkEnumeratePhysicalDevices original, VkInstance instance,
        uint32_t* count, VkPhysicalDevice* physical)
    {
        const VkResult result = original(instance, count, physical);
        if (!gInsideOverlay && (result == VK_SUCCESS || result == VK_INCOMPLETE) && physical && count)
        {
            std::lock_guard lock(gRegistryMutex);
            const auto found = gInstances.find(instance);
            if (found != gInstances.end() && *count < 128)
                for (uint32_t index = 0; index < *count; ++index)
                    gPhysicalDevices[physical[index]] = found->second;
        }
        return result;
    }
};
struct CreateDeviceTag
{
    static VkResult Call(PFN_vkCreateDevice original, VkPhysicalDevice physical,
        const VkDeviceCreateInfo* info, const VkAllocationCallbacks* allocator, VkDevice* output)
    {
        const VkResult result = original(physical, info, allocator, output);
        if (!gInsideOverlay && result == VK_SUCCESS && output) RememberDevice(physical, *output);
        return result;
    }
};
struct GetQueueTag
{
    static void Call(PFN_vkGetDeviceQueue original, VkDevice device, uint32_t family, uint32_t index, VkQueue* output)
    {
        original(device, family, index, output);
        if (!gInsideOverlay && output) RememberQueue(device, family, false, *output);
    }
};
struct GetQueue2Tag
{
    static void Call(PFN_vkGetDeviceQueue2 original, VkDevice device,
        const VkDeviceQueueInfo2* info, VkQueue* output)
    {
        original(device, info, output);
        if (!gInsideOverlay && output && info)
            RememberQueue(device, info->queueFamilyIndex, (info->flags & VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT) != 0, *output);
    }
};
struct CreateSemaphoreTag
{
    static VkResult Call(PFN_vkCreateSemaphore original, VkDevice device,
        const VkSemaphoreCreateInfo* info, const VkAllocationCallbacks* allocator, VkSemaphore* output)
    {
        const VkResult result = original(device, info, allocator, output);
        if (!gInsideOverlay && result == VK_SUCCESS && info && output)
        {
            std::lock_guard lock(gRegistryMutex);
            if (!info->pNext && !info->flags && gDevices.contains(device) && gNativeSemaphores.size() < 8192)
                gNativeSemaphores[*output] = device;
        }
        return result;
    }
};
struct DestroySemaphoreTag
{
    static void Call(PFN_vkDestroySemaphore original, VkDevice device,
        VkSemaphore semaphore, const VkAllocationCallbacks* allocator)
    {
        if (!gInsideOverlay)
        {
            std::lock_guard lock(gRegistryMutex);
            gNativeSemaphores.erase(semaphore);
        }
        original(device, semaphore, allocator);
    }
};
struct ImportSemaphoreTag
{
    static VkResult Call(PFN_vkImportSemaphoreWin32HandleKHR original, VkDevice device,
        const VkImportSemaphoreWin32HandleInfoKHR* info)
    {
        if (!gInsideOverlay && info)
        {
            std::lock_guard lock(gRegistryMutex);
            gNativeSemaphores.erase(info->semaphore);
        }
        return original(device, info);
    }
};
struct CreateSurfaceTag
{
    static VkResult Call(PFN_vkCreateWin32SurfaceKHR original, VkInstance instance,
        const VkWin32SurfaceCreateInfoKHR* info, const VkAllocationCallbacks* allocator, VkSurfaceKHR* output)
    {
        const VkResult result = original(instance, info, allocator, output);
        if (!gInsideOverlay && result == VK_SUCCESS && output && info)
        {
            std::lock_guard lock(gRegistryMutex);
            if (gSurfaces.size() < 64) gSurfaces[*output] = Surface{info->hwnd, instance};
        }
        return result;
    }
};
struct CreateSwapchainTag
{
    static VkResult Call(PFN_vkCreateSwapchainKHR original, VkDevice handle,
        const VkSwapchainCreateInfoKHR* info, const VkAllocationCallbacks* allocator, VkSwapchainKHR* output)
    {
        if (gInsideOverlay || !info) return original(handle, info, allocator, output);
        auto device = FindDevice(handle);
        VkSwapchainCreateInfoKHR adjusted = *info;
        if (device && !(adjusted.imageUsage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
        {
            InternalScope internal;
            auto capabilities = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
                device->instance->gipa(device->instance->handle, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
            VkSurfaceCapabilitiesKHR support{};
            if (capabilities && capabilities(device->physical, info->surface, &support) == VK_SUCCESS
                && (support.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
                adjusted.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        }
        VkResult result;
        {
            // Windows Vulkan drivers may build an internal DXGI swapchain.
            // It belongs to Vulkan WSI, not the application's D3D12 renderer.
            InternalScope internal;
            result = original(handle, &adjusted, allocator, output);
        }
        if (result == VK_SUCCESS && output && device)
        {
            ui_input_coherence::InvalidateResources();
            auto state = std::make_shared<State>();
            state->device = device;
            state->swapchain = *output;
            state->surface = info->surface;
            state->format = info->imageFormat;
            state->colorSpace = info->imageColorSpace;
            state->extent = info->imageExtent;
            state->usage = adjusted.imageUsage;
            state->flags = info->flags;
            state->layers = info->imageArrayLayers;
            state->minImageCount = info->minImageCount;
            state->sharingMode = info->imageSharingMode;
            if (info->queueFamilyIndexCount <= 64 && info->queueFamilyIndexCount && info->pQueueFamilyIndices)
                state->sharingFamilies.assign(info->pQueueFamilyIndices, info->pQueueFamilyIndices+info->queueFamilyIndexCount);
            std::lock_guard lock(gRegistryMutex);
            const auto surface = gSurfaces.find(info->surface);
            if (surface != gSurfaces.end() && surface->second.instance == device->instance->handle)
                state->window = surface->second.window;
            if (gSwapchains.size() < 64) gSwapchains[*output] = std::move(state);
        }
        return result;
    }
};

struct PresentTag
{
    static VkResult Call(PFN_vkQueuePresentKHR original, VkQueue handle, const VkPresentInfoKHR* info)
    {
        if (gInsideOverlay || !info || !info->swapchainCount || !info->pSwapchains || !info->pImageIndices)
            return original(handle, info);
        Queue queue;
        std::shared_ptr<State> selected;
        uint32_t selectedIndex = 0;
        bool nativeWaits = gSemaphoreCoverage.load(std::memory_order_acquire)
            && info->waitSemaphoreCount > 0 && info->waitSemaphoreCount <= 64 && info->pWaitSemaphores;
        {
            std::lock_guard lock(gRegistryMutex);
            const auto foundQueue = gQueues.find(handle);
            if (foundQueue != gQueues.end()) queue = foundQueue->second;
            if (queue.device && nativeWaits)
                for (uint32_t index = 0; index < info->waitSemaphoreCount; ++index)
                {
                    const auto semaphore = gNativeSemaphores.find(info->pWaitSemaphores[index]);
                    if (semaphore == gNativeSemaphores.end() || semaphore->second != queue.device->handle)
                    { nativeWaits = false; break; }
                }
            if (queue.device)
                for (uint32_t index = 0; index < info->swapchainCount; ++index)
                {
                    const auto found = gSwapchains.find(info->pSwapchains[index]);
                    if (found != gSwapchains.end() && found->second->device == queue.device)
                    {
                        selected = found->second;
                        selectedIndex = index;
                        break;
                    }
                }
        }
        VkResult renderFailure = VK_SUCCESS;
        bool supportedPresent = true;
        // Preserve arbitrary extensions verbatim. Rendering with incremental
        // regions or device-group masks needs additional image ownership proof.
        for (auto* next = static_cast<const VkBaseInStructure*>(info->pNext); next; next = next->pNext)
            if (next->sType == VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR
                || next->sType == VK_STRUCTURE_TYPE_DEVICE_GROUP_PRESENT_INFO_KHR)
                supportedPresent = false;
        if (selected && supportedPresent && nativeWaits)
        {
            gVulkanFrames.fetch_add(1, std::memory_order_relaxed);
            selected->Render(queue, info->pImageIndices[selectedIndex], *info, renderFailure);
        }
        if (renderFailure != VK_SUCCESS)
        {
            if (info->pResults)
                std::fill_n(info->pResults, info->swapchainCount, renderFailure);
            return renderFailure;
        }
        // The device dispatch may re-enter the loader entry. Only one layer
        // may consume or substitute this call's original semaphore set.
        InternalScope internal;
        const VkResult result = original(handle, info);
        if (selected && (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR))
            MfgUnlockSampleFrameTelemetry();
        return result;
    }
};

void ForgetSwapchain(VkDevice device, VkSwapchainKHR swapchain)
{
    std::shared_ptr<State> state;
    {
        std::lock_guard lock(gRegistryMutex);
        const auto found = gSwapchains.find(swapchain);
        if (found != gSwapchains.end() && found->second->device->handle == device)
        {
            state = found->second;
            gSwapchains.erase(found);
        }
    }
    if (state)
    {
        ui_input_coherence::InvalidateResources();
        std::lock_guard lock(state->mutex);
        if (!state->Shutdown())
        {
            std::lock_guard registryLock(gRegistryMutex);
            gRetained.push_back(std::move(state));
        }
    }
}
struct DestroySwapchainTag
{
    static void Call(PFN_vkDestroySwapchainKHR original, VkDevice device,
        VkSwapchainKHR swapchain, const VkAllocationCallbacks* allocator)
    {
        if (!gInsideOverlay) ForgetSwapchain(device, swapchain);
        original(device, swapchain, allocator);
    }
};
struct DestroyDeviceTag
{
    static void Call(PFN_vkDestroyDevice original, VkDevice device, const VkAllocationCallbacks* allocator)
    {
        if (!gInsideOverlay)
        {
            std::vector<VkSwapchainKHR> chains;
            {
                std::lock_guard lock(gRegistryMutex);
                for (const auto& [chain, state] : gSwapchains)
                    if (state->device->handle == device) chains.push_back(chain);
            }
            for (auto chain : chains) ForgetSwapchain(device, chain);
            {
                std::lock_guard lock(gRegistryMutex);
                std::erase_if(gQueues, [&](const auto& row) { return row.second.device->handle == device; });
                std::erase_if(gNativeSemaphores, [&](const auto& row) { return row.second == device; });
                gDevices.erase(device);
            }
        }
        original(device, allocator);
    }
};
struct DestroySurfaceTag
{
    static void Call(PFN_vkDestroySurfaceKHR original, VkInstance instance,
        VkSurfaceKHR surface, const VkAllocationCallbacks* allocator)
    {
        if (!gInsideOverlay)
        {
            std::lock_guard lock(gRegistryMutex);
            gSurfaces.erase(surface);
        }
        original(instance, surface, allocator);
    }
};
struct DestroyInstanceTag
{
    static void Call(PFN_vkDestroyInstance original, VkInstance instance, const VkAllocationCallbacks* allocator)
    {
        if (!gInsideOverlay)
        {
            std::lock_guard lock(gRegistryMutex);
            std::erase_if(gPhysicalDevices, [&](const auto& row) { return row.second->handle == instance; });
            gInstances.erase(instance);
        }
        original(instance, allocator);
    }
};

PFN_vkVoidFunction Wrap(const char* name, PFN_vkVoidFunction original)
{
    if (!name || !original) return original;
    // A missed destroy/import could leave a stale or replaced payload marked
    // native. Disable this route if either callable cannot be intercepted.
#define MFG_VK_SEMAPHORE_WRAP(nameLiteral, Tag, Result, ...) \
    if (strcmp(name, nameLiteral) == 0) { \
        const auto wrapped = reinterpret_cast<PFN_vkVoidFunction>( \
            HookFamily<Tag, Result, __VA_ARGS__>::Bind(reinterpret_cast<void*>(original), false)); \
        if (wrapped == original) gSemaphoreCoverage.store(false, std::memory_order_release); \
        return wrapped; }
    MFG_VK_SEMAPHORE_WRAP("vkDestroySemaphore", DestroySemaphoreTag, void, VkDevice, VkSemaphore, const VkAllocationCallbacks*)
    MFG_VK_SEMAPHORE_WRAP("vkImportSemaphoreWin32HandleKHR", ImportSemaphoreTag, VkResult, VkDevice, const VkImportSemaphoreWin32HandleInfoKHR*)
#undef MFG_VK_SEMAPHORE_WRAP
#define MFG_VK_WRAP(nameLiteral, Tag, Result, ...) \
    if (strcmp(name, nameLiteral) == 0) return reinterpret_cast<PFN_vkVoidFunction>( \
        HookFamily<Tag, Result, __VA_ARGS__>::Bind(reinterpret_cast<void*>(original), false));
    MFG_VK_WRAP("vkGetInstanceProcAddr", GipaTag, PFN_vkVoidFunction, VkInstance, const char*)
    MFG_VK_WRAP("vkGetDeviceProcAddr", GdpaTag, PFN_vkVoidFunction, VkDevice, const char*)
    MFG_VK_WRAP("vkCreateInstance", CreateInstanceTag, VkResult, const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*)
    MFG_VK_WRAP("vkEnumeratePhysicalDevices", EnumeratePhysicalTag, VkResult, VkInstance, uint32_t*, VkPhysicalDevice*)
    MFG_VK_WRAP("vkCreateDevice", CreateDeviceTag, VkResult, VkPhysicalDevice, const VkDeviceCreateInfo*, const VkAllocationCallbacks*, VkDevice*)
    MFG_VK_WRAP("vkGetDeviceQueue", GetQueueTag, void, VkDevice, uint32_t, uint32_t, VkQueue*)
    MFG_VK_WRAP("vkGetDeviceQueue2", GetQueue2Tag, void, VkDevice, const VkDeviceQueueInfo2*, VkQueue*)
    MFG_VK_WRAP("vkCreateSemaphore", CreateSemaphoreTag, VkResult, VkDevice, const VkSemaphoreCreateInfo*, const VkAllocationCallbacks*, VkSemaphore*)
    MFG_VK_WRAP("vkCreateWin32SurfaceKHR", CreateSurfaceTag, VkResult, VkInstance, const VkWin32SurfaceCreateInfoKHR*, const VkAllocationCallbacks*, VkSurfaceKHR*)
    MFG_VK_WRAP("vkCreateSwapchainKHR", CreateSwapchainTag, VkResult, VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*, VkSwapchainKHR*)
    MFG_VK_WRAP("vkQueuePresentKHR", PresentTag, VkResult, VkQueue, const VkPresentInfoKHR*)
    MFG_VK_WRAP("vkDestroySwapchainKHR", DestroySwapchainTag, void, VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*)
    MFG_VK_WRAP("vkDestroyDevice", DestroyDeviceTag, void, VkDevice, const VkAllocationCallbacks*)
    MFG_VK_WRAP("vkDestroySurfaceKHR", DestroySurfaceTag, void, VkInstance, VkSurfaceKHR, const VkAllocationCallbacks*)
    MFG_VK_WRAP("vkDestroyInstance", DestroyInstanceTag, void, VkInstance, const VkAllocationCallbacks*)
#undef MFG_VK_WRAP
    return original;
}
}

FARPROC Resolve(HMODULE, LPCSTR name, FARPROC original) noexcept
{
    if (!name || reinterpret_cast<uintptr_t>(name) <= 0xFFFFu || !original) return original;
    if (strcmp(name, "vkGetInstanceProcAddr") == 0)
    {
        const auto target = reinterpret_cast<PFN_vkGetInstanceProcAddr>(original);
        gLoaderGipa.store(target, std::memory_order_release);
        return reinterpret_cast<FARPROC>(HookFamily<GipaTag, PFN_vkVoidFunction, VkInstance, const char*>::Bind(reinterpret_cast<void*>(original), true));
    }
    if (strcmp(name, "vkGetDeviceProcAddr") == 0)
        return reinterpret_cast<FARPROC>(HookFamily<GdpaTag, PFN_vkVoidFunction, VkDevice, const char*>::Bind(reinterpret_cast<void*>(original), true));
    return reinterpret_cast<FARPROC>(Wrap(name, reinterpret_cast<PFN_vkVoidFunction>(original)));
}

void Install(HMODULE module) noexcept
{
    if (!module) return;
    const auto gipa = GetProcAddress(module, "vkGetInstanceProcAddr");
    if (!gipa) return;
    Resolve(module, "vkGetInstanceProcAddr", gipa);
    if (auto gdpa = GetProcAddress(module, "vkGetDeviceProcAddr")) Resolve(module, "vkGetDeviceProcAddr", gdpa);
    // Direct imports do not pass through a resolver. Cover the loader's own
    // public entries as well as every pointer returned by GIPA/GDPA.
#define MFG_VK_INSTALL(name, Tag, Result, ...) \
    HookFamily<Tag, Result, __VA_ARGS__>::Bind(reinterpret_cast<void*>(GetProcAddress(module, name)), true);
    MFG_VK_INSTALL("vkCreateInstance", CreateInstanceTag, VkResult, const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*)
    MFG_VK_INSTALL("vkEnumeratePhysicalDevices", EnumeratePhysicalTag, VkResult, VkInstance, uint32_t*, VkPhysicalDevice*)
    MFG_VK_INSTALL("vkCreateDevice", CreateDeviceTag, VkResult, VkPhysicalDevice, const VkDeviceCreateInfo*, const VkAllocationCallbacks*, VkDevice*)
    MFG_VK_INSTALL("vkGetDeviceQueue", GetQueueTag, void, VkDevice, uint32_t, uint32_t, VkQueue*)
    MFG_VK_INSTALL("vkGetDeviceQueue2", GetQueue2Tag, void, VkDevice, const VkDeviceQueueInfo2*, VkQueue*)
    MFG_VK_INSTALL("vkCreateSemaphore", CreateSemaphoreTag, VkResult, VkDevice, const VkSemaphoreCreateInfo*, const VkAllocationCallbacks*, VkSemaphore*)
    MFG_VK_INSTALL("vkDestroySemaphore", DestroySemaphoreTag, void, VkDevice, VkSemaphore, const VkAllocationCallbacks*)
    MFG_VK_INSTALL("vkImportSemaphoreWin32HandleKHR", ImportSemaphoreTag, VkResult, VkDevice, const VkImportSemaphoreWin32HandleInfoKHR*)
    MFG_VK_INSTALL("vkCreateWin32SurfaceKHR", CreateSurfaceTag, VkResult, VkInstance, const VkWin32SurfaceCreateInfoKHR*, const VkAllocationCallbacks*, VkSurfaceKHR*)
    MFG_VK_INSTALL("vkCreateSwapchainKHR", CreateSwapchainTag, VkResult, VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*, VkSwapchainKHR*)
    MFG_VK_INSTALL("vkQueuePresentKHR", PresentTag, VkResult, VkQueue, const VkPresentInfoKHR*)
    MFG_VK_INSTALL("vkDestroySwapchainKHR", DestroySwapchainTag, void, VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*)
    MFG_VK_INSTALL("vkDestroyDevice", DestroyDeviceTag, void, VkDevice, const VkAllocationCallbacks*)
    MFG_VK_INSTALL("vkDestroySurfaceKHR", DestroySurfaceTag, void, VkInstance, VkSurfaceKHR, const VkAllocationCallbacks*)
    MFG_VK_INSTALL("vkDestroyInstance", DestroyInstanceTag, void, VkInstance, const VkAllocationCallbacks*)
#undef MFG_VK_INSTALL
    const auto destroySemaphore = GetProcAddress(module, "vkDestroySemaphore");
    const auto importSemaphore = GetProcAddress(module, "vkImportSemaphoreWin32HandleKHR");
    if (!destroySemaphore || !HookFamily<DestroySemaphoreTag, void, VkDevice, VkSemaphore,
            const VkAllocationCallbacks*>::Installed(reinterpret_cast<void*>(destroySemaphore))
        || (importSemaphore && !HookFamily<ImportSemaphoreTag, VkResult, VkDevice,
            const VkImportSemaphoreWin32HandleInfoKHR*>::Installed(reinterpret_cast<void*>(importSemaphore))))
        gSemaphoreCoverage.store(false, std::memory_order_release);
    gVulkanHooked.store(HookFamily<GipaTag, PFN_vkVoidFunction, VkInstance, const char*>::Installed(), std::memory_order_release);
}
}
