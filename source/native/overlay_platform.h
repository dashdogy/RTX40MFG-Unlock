#pragma once
#include "single_overlay.h"
#include <imgui.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace single_overlay
{
inline std::recursive_mutex gUiMutex;
inline std::atomic<bool> gVisible{false};
inline std::atomic<uint32_t> gRenderFailures{0};
inline std::atomic<uint64_t> gDxgiFrames{0};
inline std::atomic<uint64_t> gVulkanFrames{0};
inline std::atomic<uint64_t> gRenderedFrames{0};
inline std::atomic<bool> gDxgiHooked{false};
inline std::atomic<bool> gVulkanHooked{false};
inline thread_local bool gInsideOverlay = false;

struct InternalScope
{
    bool previous = gInsideOverlay;
    InternalScope() noexcept { gInsideOverlay = true; }
    ~InternalScope() { gInsideOverlay = previous; }
};

struct ContextScope
{
    ImGuiContext* previous;
    explicit ContextScope(ImGuiContext* context) : previous(ImGui::GetCurrentContext())
    { ImGui::SetCurrentContext(context); }
    ~ContextScope() { ImGui::SetCurrentContext(previous); }
};

struct PlatformState
{
    ImGuiContext* context = nullptr;
    HWND window = nullptr;
    std::string iniPath;
    uint64_t firstFrame = 0;
    ImGuiStyle baseStyle;
    float uiScale = 1.0f;
    float layoutScale = 1.0f;
    bool layoutPrepared = false;
    bool inputWasVisible = false;
    bool inputAttached = false;
    bool Initialize(HWND hwnd);
    void Shutdown();
    void RetireInput();
    bool WantsFrame() const noexcept;
    bool NewFrame(uint32_t outputWidth, uint32_t outputHeight);
    void Draw();
};

void InstallInputHooks() noexcept;
void RecordFailure(const wchar_t* reason) noexcept;
}
