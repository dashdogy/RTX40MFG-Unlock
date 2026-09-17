#include "overlay_platform.h"
#include "overlay_hook.h"
#include "overlay_scaling.h"
#include "standalone_ui.h"
#include "single_hotkey.h"
#include "ui_host.h"
#include <backends/imgui_impl_win32.h>
#include <imgui_internal.h>
#include <Windowsx.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <unordered_map>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace single_overlay
{
namespace
{
struct InputMessage
{
    UINT message;
    WPARAM wparam;
    LPARAM lparam;
    ImGuiMouseSource mouseSource = ImGuiMouseSource_Mouse;
};
struct WindowInput
{
    WNDPROC original = nullptr;
    std::deque<InputMessage> messages;
    uint32_t clients = 0;
    uint32_t mouseButtons = 0;
    bool ownsCapture = false;
};
std::mutex gInputMutex;
std::unordered_map<HWND, WindowInput> gWindows;
std::atomic<HWND> gActiveWindow{nullptr};
MenuHotkey gMenuKey;
bool gShortcutSaveFailed = false; // Render-thread state, protected by gUiMutex.
bool gFirstLaunchChecked = false; // Shared across swapchain/context recreation.
bool gFirstLaunchPending = false;
bool gFirstLaunchSavePending = false;
std::atomic<bool> gWantMouse{false};
std::atomic<bool> gWantKeyboard{false};
RECT gPreviousClip{};
bool gHavePreviousClip = false;
std::mutex gClipMutex;
void DetachWindow(HWND window, bool releaseClient = true);

bool Foreground(HWND window) noexcept
{
    const HWND foreground = GetForegroundWindow();
    return window && foreground && (foreground == window
        || GetAncestor(foreground, GA_ROOT) == GetAncestor(window, GA_ROOT));
}

bool CaptureInput() noexcept
{
    return !gInsideOverlay && gVisible.load(std::memory_order_acquire)
        && Foreground(gActiveWindow.load(std::memory_order_acquire));
}

void SetVisible(bool visible, HWND window)
{
    InternalScope internal;
    // Serialize admission with retirement. A queued window callback must not
    // reopen a renderer that has already relinquished its input client.
    std::unique_lock inputLock(gInputMutex, std::defer_lock);
    if (visible)
    {
        inputLock.lock();
        const auto found = gWindows.find(window);
        if (found == gWindows.end() || !found->second.clients) return;
    }
    const bool previous = gVisible.exchange(visible, std::memory_order_acq_rel);
    if (!visible) gMenuKey.CancelBinding();
    gWantMouse.store(visible, std::memory_order_release);
    gWantKeyboard.store(visible, std::memory_order_release);
    if (visible && !previous)
    {
        gActiveWindow.store(window, std::memory_order_release);
        std::lock_guard clipLock(gClipMutex);
        gHavePreviousClip = GetClipCursor(&gPreviousClip) != FALSE;
        if (Foreground(window)) ClipCursor(nullptr);
    }
    else if (!visible && previous)
    {
        std::lock_guard clipLock(gClipMutex);
        if (gHavePreviousClip && Foreground(window)) ClipCursor(&gPreviousClip);
        gHavePreviousClip = false;
    }
}

bool MouseMessage(UINT message) noexcept
{
    return (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST)
        || message == WM_INPUT || message == WM_SETCURSOR;
}
bool KeyboardMessage(UINT message) noexcept
{
    return message == WM_KEYDOWN || message == WM_KEYUP
        || message == WM_CHAR || message == WM_UNICHAR;
}

int MouseButton(UINT message, WPARAM wparam, bool& down) noexcept
{
    down = false;
    switch (message)
    {
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: down = true; [[fallthrough]];
    case WM_LBUTTONUP: return 0;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: down = true; [[fallthrough]];
    case WM_RBUTTONUP: return 1;
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: down = true; [[fallthrough]];
    case WM_MBUTTONUP: return 2;
    case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK: down = true; [[fallthrough]];
    case WM_XBUTTONUP: return GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? 3 : 4;
    }
    return -1;
}

void UpdateMouseCapture(HWND window, UINT message, WPARAM wparam)
{
    bool down = false;
    const int index = MouseButton(message, wparam, down);
    const uint32_t button = index >= 0 ? 1u << index : 0;
    const bool cancel = message == WM_CANCELMODE || message == WM_KILLFOCUS
        || message == WM_NCDESTROY || (message == WM_ACTIVATEAPP && !wparam);
    if (!button && !cancel && message != WM_CAPTURECHANGED) return;

    bool acquire = false;
    bool release = false;
    {
        std::lock_guard lock(gInputMutex);
        const auto found = gWindows.find(window);
        if (found == gWindows.end()) return;
        auto& input = found->second;
        if (cancel || message == WM_CAPTURECHANGED)
        {
            constexpr UINT releases[]{WM_LBUTTONUP, WM_RBUTTONUP, WM_MBUTTONUP, WM_XBUTTONUP, WM_XBUTTONUP};
            for (unsigned index = 0; index < std::size(releases); ++index)
                if (input.mouseButtons & (1u << index))
                    input.messages.push_back({releases[index], index >= 3 ? MAKEWPARAM(0, index == 3 ? XBUTTON1 : XBUTTON2) : 0, 0});
        }
        if (message == WM_CAPTURECHANGED)
        {
            input.mouseButtons = 0;
            input.ownsCapture = false;
        }
        else if (cancel)
        {
            release = input.ownsCapture;
            input.mouseButtons = 0;
            input.ownsCapture = false;
        }
        else if (down && input.clients && gVisible.load(std::memory_order_acquire))
        {
            input.mouseButtons |= button;
            acquire = !input.ownsCapture;
        }
        else if (!down)
        {
            input.mouseButtons &= ~button;
            release = input.ownsCapture && !input.mouseButtons;
            if (release) input.ownsCapture = false;
        }
    }
    // SetCapture/ReleaseCapture belong to the HWND's thread. ImGui's queued
    // Win32 handler runs on the presentation thread, which may be different.
    // Never steal existing game capture, and never hold our mutex across an
    // API that can synchronously deliver WM_CAPTURECHANGED to this procedure.
    if (release && GetCapture() == window) ReleaseCapture();
    if (acquire && window == gActiveWindow.load(std::memory_order_acquire)
        && CaptureInput() && !GetCapture())
    {
        SetCapture(window);
        if (GetCapture() == window)
        {
            bool retained = false;
            {
                std::lock_guard lock(gInputMutex);
                const auto found = gWindows.find(window);
                if (found != gWindows.end() && found->second.clients)
                {
                    found->second.ownsCapture = true;
                    retained = true;
                }
            }
            if (!retained) ReleaseCapture();
        }
    }
}

void QueueWindowMessage(HWND window, const InputMessage& message)
{
    std::lock_guard lock(gInputMutex);
    const auto found = gWindows.find(window);
    if (found == gWindows.end() || !found->second.clients) return;
    auto& messages = found->second.messages;
    if (messages.size() >= 512)
    {
        messages.clear();
        messages.push_back({WM_KILLFOCUS, 0, 0});
        messages.push_back({WM_SETFOCUS, 0, 0});
    }
    messages.push_back(message);
}

bool RawMouseOmitsLegacyMessages(HWND window)
{
    // Query only on button/wheel transitions, not on high-rate motion packets.
    // Observing the registration leaves the game's input ownership unchanged.
    RAWINPUTDEVICE devices[64]{};
    UINT capacity = static_cast<UINT>(std::size(devices));
    const UINT count = GetRegisteredRawInputDevices(devices, &capacity, sizeof(RAWINPUTDEVICE));
    if (count == UINT(-1) || count > std::size(devices)) return false;
    for (UINT index = 0; index < count; ++index)
    {
        const auto& device = devices[index];
        if (device.usUsagePage == 0x01 && device.usUsage == 0x02
            && (device.dwFlags & RIDEV_NOLEGACY) == RIDEV_NOLEGACY
            && (!device.hwndTarget || device.hwndTarget == window))
            return true;
    }
    return false;
}

void QueueRawMouse(HWND window, const RAWMOUSE& mouse)
{
    if (!gVisible.load(std::memory_order_acquire)
        || window != gActiveWindow.load(std::memory_order_acquire)) return;
    // The desktop cursor already applies Windows pointer speed and absolute
    // device mapping. Keep its position ordered with this packet's buttons.
    POINT cursor{};
    if (GetCursorPos(&cursor) && ScreenToClient(window, &cursor))
        QueueWindowMessage(window, {WM_MOUSEMOVE, 0, MAKELPARAM(cursor.x, cursor.y)});
    if (!mouse.usButtonFlags || !RawMouseOmitsLegacyMessages(window)) return;
    constexpr UINT downMessages[]{WM_LBUTTONDOWN, WM_RBUTTONDOWN, WM_MBUTTONDOWN, WM_XBUTTONDOWN, WM_XBUTTONDOWN};
    constexpr UINT upMessages[]{WM_LBUTTONUP, WM_RBUTTONUP, WM_MBUTTONUP, WM_XBUTTONUP, WM_XBUTTONUP};
    for (unsigned index = 0; index < std::size(downMessages); ++index)
    {
        const WPARAM parameter = index >= 3 ? MAKEWPARAM(0, index == 3 ? XBUTTON1 : XBUTTON2) : 0;
        for (unsigned transition = 0; transition < 2; ++transition)
        {
            if (!(mouse.usButtonFlags & (1u << (index * 2 + transition)))) continue;
            const UINT message = transition ? upMessages[index] : downMessages[index];
            // Preserve the existing window-thread capture and cancellation path.
            UpdateMouseCapture(window, message, parameter);
            QueueWindowMessage(window, {message, parameter, 0});
        }
    }
    if (mouse.usButtonFlags & RI_MOUSE_WHEEL)
        QueueWindowMessage(window, {WM_MOUSEWHEEL, MAKEWPARAM(0, mouse.usButtonData), 0});
    if (mouse.usButtonFlags & RI_MOUSE_HWHEEL)
        QueueWindowMessage(window, {WM_MOUSEHWHEEL, MAKEWPARAM(0, mouse.usButtonData), 0});
}

void ReleaseRetiredRawMouse(HWND window, const RAWMOUSE& mouse)
{
    constexpr UINT releases[]{WM_LBUTTONUP, WM_RBUTTONUP, WM_MBUTTONUP, WM_XBUTTONUP, WM_XBUTTONUP};
    for (unsigned index = 0; index < std::size(releases); ++index)
        if (mouse.usButtonFlags & (2u << (index * 2)))
            UpdateMouseCapture(window, releases[index],
                index >= 3 ? MAKEWPARAM(0, index == 3 ? XBUTTON1 : XBUTTON2) : 0);
}

bool ProcessKey(HWND window, int key, bool down)
{
    const bool wasBinding = gMenuKey.Binding();
    const auto action = gMenuKey.Process(key, down);
    if (action == KeyAction::Toggle)
        SetVisible(!gVisible.load(std::memory_order_acquire), window);
    return wasBinding || action != KeyAction::None || key == gMenuKey.Key();
}

std::string KeyLabel()
{
    const int key = gMenuKey.Key();
    if (key == VK_BACK) return "Backspace";
    if (key == VK_OEM_PLUS) return "=";
    const UINT scan = MapVirtualKeyW(key, MAPVK_VK_TO_VSC_EX);
    const LONG parameter = LONG((scan & 0xFF) << 16) | (scan & 0xFF00 ? 1 << 24 : 0);
    wchar_t label[96]{};
    if (GetKeyNameTextW(parameter, label, static_cast<int>(std::size(label))))
    {
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, label, -1, nullptr, 0, nullptr, nullptr);
        std::string text(bytes, '\0');
        WideCharToMultiByte(CP_UTF8, 0, label, -1, text.data(), bytes, nullptr, nullptr);
        if (!text.empty()) text.pop_back();
        return text;
    }
    char fallback[16]{}; sprintf_s(fallback, "0x%02X", key); return fallback;
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    const LPARAM extra = GetMessageExtraInfo();
    const auto mouseSource = (extra & 0xFFFFFF80) == 0xFF515700 ? ImGuiMouseSource_Pen
        : (extra & 0xFFFFFF80) == 0xFF515780 ? ImGuiMouseSource_TouchScreen : ImGuiMouseSource_Mouse;
    WNDPROC original = nullptr;
    bool hasClient = false;
    bool drainRawCapture = false;
    {
        std::lock_guard lock(gInputMutex);
        const auto found = gWindows.find(window);
        if (found != gWindows.end())
        {
            original = found->second.original;
            hasClient = found->second.clients != 0;
            drainRawCapture = !hasClient && found->second.ownsCapture;
        }
    }
    if (!original) return DefWindowProcW(window, message, wparam, lparam);
    UpdateMouseCapture(window, message, wparam);
    if (hasClient && (message == WM_KEYDOWN || message == WM_KEYUP))
    {
        if (ProcessKey(window, static_cast<int>(wparam), message == WM_KEYDOWN)) return 0;
    }
    if (message == WM_INPUT && ((hasClient && Foreground(window)) || drainRawCapture))
    {
        // RIDEV_NOLEGACY suppresses ordinary keyboard and mouse messages.
        // Copy the packet now: its handle cannot outlive WM_INPUT cleanup.
        RAWINPUT input{};
        UINT bytes = sizeof(input);
        UINT copied = UINT(-1);
        {
            InternalScope internal;
            copied = GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT,
                &input, &bytes, sizeof(RAWINPUTHEADER));
        }
        if (copied <= sizeof(input) && copied >= sizeof(RAWINPUTHEADER)
            && input.header.dwSize <= copied)
        {
            if (hasClient && input.header.dwType == RIM_TYPEKEYBOARD
                && input.header.dwSize >= offsetof(RAWINPUT, data) + sizeof(RAWKEYBOARD)
                && ProcessKey(window, input.data.keyboard.VKey,
                    !(input.data.keyboard.Flags & RI_KEY_BREAK)))
                return DefWindowProcW(window, message, wparam, lparam);
            if (input.header.dwType == RIM_TYPEMOUSE
                && input.header.dwSize >= offsetof(RAWINPUT, data) + sizeof(RAWMOUSE))
            {
                if (drainRawCapture) ReleaseRetiredRawMouse(window, input.data.mouse);
                else QueueRawMouse(window, input.data.mouse);
            }
        }
    }
    if (!hasClient) DetachWindow(window, false);
    if (hasClient)
    {
        if ((gVisible.load(std::memory_order_acquire)
                && (MouseMessage(message) || KeyboardMessage(message)
                    || message == WM_NCMOUSEMOVE
                    || message == WM_SYSKEYDOWN || message == WM_SYSKEYUP))
            // These notifications are outside WM_MOUSEFIRST..WM_MOUSELAST.
            // The Win32 backend needs them even if the menu just closed,
            // to retire hover state and rearm tracking on the next entry.
            || message == WM_MOUSELEAVE || message == WM_NCMOUSELEAVE
            || message == WM_SETFOCUS || message == WM_KILLFOCUS)
            QueueWindowMessage(window, {message, wparam, lparam, mouseSource});
    }
    if (hasClient && CaptureInput())
    {
        if (message == WM_SETCURSOR && LOWORD(lparam) == HTCLIENT)
        {
            SetCursor(nullptr); // ImGui draws its own cursor at the game position.
            return TRUE;
        }
        if ((MouseMessage(message) && gWantMouse.load(std::memory_order_acquire))
            || (KeyboardMessage(message) && gWantKeyboard.load(std::memory_order_acquire)))
        {
            // WM_INPUT still requires the OS cleanup performed by DefWindowProc.
            return message == WM_INPUT
                ? DefWindowProcW(window, message, wparam, lparam) : 0;
        }
    }
    if (message == WM_ACTIVATEAPP && !wparam)
    {
        InternalScope internal;
        gMenuKey.ResetPressed();
        ClipCursor(nullptr);
    }
    if (message == WM_NCDESTROY)
    {
        if (gActiveWindow.load(std::memory_order_acquire) == window)
        {
            SetVisible(false, window);
            gActiveWindow.store(nullptr, std::memory_order_release);
        }
        std::lock_guard lock(gInputMutex);
        gWindows.erase(window);
    }
    return CallWindowProcW(original, window, message, wparam, lparam);
}

bool AttachWindow(HWND window)
{
    DWORD process = 0;
    if (!IsWindow(window) || !GetWindowThreadProcessId(window, &process)
        || process != GetCurrentProcessId()) return false;
    std::lock_guard lock(gInputMutex);
    auto found = gWindows.find(window);
    if (found != gWindows.end())
    {
        ++found->second.clients;
        return true;
    }
    SetLastError(ERROR_SUCCESS);
    const auto original = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window,
        GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WindowProc)));
    if (!original) return false;
    gWindows.emplace(window, WindowInput{original, {}, 1});
    return true;
}

void DetachWindow(HWND window, bool releaseClient)
{
    std::lock_guard lock(gInputMutex);
    const auto found = gWindows.find(window);
    if (found == gWindows.end()) return;
    if (releaseClient && found->second.clients) --found->second.clients;
    if (found->second.clients) return;
    // Input retirement must not depend on a GPU fence or ImGui destruction.
    // Keep another renderer's client on this HWND (or active HWND) intact.
    if (gActiveWindow.load(std::memory_order_acquire) == window)
    {
        SetVisible(false, window);
        gActiveWindow.store(nullptr, std::memory_order_release);
    }
    // A render-thread retirement cannot release window-thread capture. Keep
    // forwarding until button-up, cancellation or focus loss retires it there.
    if (found->second.ownsCapture) return;
    // Preserve a later subclass belonging to the game, ReShade or another mod.
    // Our pinned forwarding thunk remains safe in that chain without clients.
    if (reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC)) == &WindowProc)
    {
        SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(found->second.original));
        gWindows.erase(found);
    }
}

std::string IniFilename()
{
    wchar_t executable[32768]{};
    if (!GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)))) return {};
    std::wstring path(executable);
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    path.resize(slash+1);
    path += L"RTX40MFG-UI.ini";
    const UINT key = GetPrivateProfileIntW(L"Overlay", L"ToggleKey", kDefaultMenuKey, path.c_str());
    gMenuKey.Load(static_cast<int>(key));
    // Dear ImGui rewrites its entire INI. Keep layout separate from the
    // panel's preferences so saving window positions cannot discard them.
    path.resize(slash+1);
    path += L"RTX40MFG-UI-layout.ini";
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, path.data(), static_cast<int>(path.size()),
        nullptr, 0, nullptr, nullptr);
    std::string utf8(bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), utf8.data(), bytes, nullptr, nullptr);
    return utf8;
}

bool SystemKey(int key) noexcept
{
    return key == VK_LWIN || key == VK_RWIN || key == VK_MENU
        || key == VK_LMENU || key == VK_RMENU || key == VK_TAB;
}
struct AsyncKeyTag
{
    static SHORT Call(SHORT (WINAPI* original)(int), int key)
    { return CaptureInput() && !SystemKey(key) ? 0 : original(key); }
};
struct KeyTag
{
    static SHORT Call(SHORT (WINAPI* original)(int), int key)
    { return CaptureInput() && !SystemKey(key) ? 0 : original(key); }
};
struct KeyboardTag
{
    static BOOL Call(BOOL (WINAPI* original)(PBYTE), PBYTE keys)
    {
        const BOOL result = original(keys);
        if (result && keys && CaptureInput())
            for (int key = 0; key < 256; ++key) if (!SystemKey(key)) keys[key] = 0;
        return result;
    }
};
struct CursorPositionTag
{
    static BOOL Call(BOOL (WINAPI* original)(int, int), int x, int y)
    { return CaptureInput() ? TRUE : original(x, y); }
};
struct ClipTag
{
    static BOOL Call(BOOL (WINAPI* original)(const RECT*), const RECT* clip)
    {
        if (!CaptureInput()) return original(clip);
        std::lock_guard clipLock(gClipMutex);
        if (clip) { gPreviousClip = *clip; gHavePreviousClip = true; }
        else gHavePreviousClip = false;
        return original(nullptr);
    }
};
}

bool PlatformState::Initialize(HWND hwnd)
{
    if (context)
    {
        if (window != hwnd || !inputAttached) return false;
        ContextScope current(context);
        InternalScope internal;
        // The pinned docking renderer's Shutdown destroys platform viewport
        // data too, even with extra viewports disabled. Rebind Win32 after a
        // renderer recreation while retaining this context and its UI state.
        if (!ImGui::GetMainViewport()->PlatformUserData)
        {
            ImGui_ImplWin32_Shutdown();
            return ImGui_ImplWin32_Init(hwnd);
        }
        return true;
    }
    if (!AttachWindow(hwnd)) return false;
    inputAttached = true;
    window = hwnd;
    ImGuiContext* previous = ImGui::GetCurrentContext();
    context = ImGui::CreateContext();
    ImGui::SetCurrentContext(previous);
    ContextScope current(context);
    InternalScope internal;
    ImGui::StyleColorsDark();
    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
    iniPath = IniFilename();
    io.IniFilename = iniPath.empty() ? nullptr : iniPath.c_str();
    io.LogFilename = nullptr;
    io.MouseDrawCursor = true;
    ImFontConfig fontConfig;
    fontConfig.Flags |= ImFontFlags_NoLoadError;
    wchar_t windowsPath[32768]{};
    const UINT windowsLength = GetWindowsDirectoryW(windowsPath, static_cast<UINT>(std::size(windowsPath)));
    if (windowsLength && windowsLength < std::size(windowsPath))
    {
        // Use the installed Windows UI font. No font file is redistributed.
        const auto fontPath = (std::filesystem::path(windowsPath)/L"Fonts"/L"segoeui.ttf").u8string();
        io.FontDefault = io.Fonts->AddFontFromFileTTF(
            reinterpret_cast<const char*>(fontPath.c_str()), 16.0f, &fontConfig);
    }
    if (!io.FontDefault)
    {
        fontConfig.SizePixels = 16.0f;
        io.FontDefault = io.Fonts->AddFontDefault(&fontConfig);
    }
    if (!ImGui_ImplWin32_Init(hwnd))
    {
        ImGui::DestroyContext(context);
        context = nullptr;
        RetireInput();
        window = nullptr;
        return false;
    }
    baseStyle = ImGui::GetStyle();
    uiScale = std::clamp(ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd), 0.75f, 3.0f);
    layoutScale = uiScale; // Migrate an older DPI-only saved window once.
    standalone_ui::Initialize(single_module::Self());
    wchar_t savedScale[32]{};
    const auto preferences = standalone_ui::SettingsPath("RTX40MFG-UI.ini");
    if (!gFirstLaunchChecked)
    {
        gFirstLaunchChecked = true;
        gFirstLaunchPending = !preferences.empty() && !GetPrivateProfileIntW(
            L"Overlay", L"FirstLaunchMenuShown", 0, preferences.c_str());
    }
    GetPrivateProfileStringW(L"Overlay", L"LayoutScale", L"", savedScale,
        static_cast<DWORD>(std::size(savedScale)), preferences.c_str());
    wchar_t* scaleEnd = nullptr;
    const float parsedScale = wcstof(savedScale, &scaleEnd);
    if (scaleEnd != savedScale && *scaleEnd == L'\0' && std::isfinite(parsedScale)
        && parsedScale >= 0.125f && parsedScale <= 32.0f)
        layoutScale = parsedScale;
    layoutPrepared = false;
    ImGui::GetStyle() = baseStyle;
    ImGui::GetStyle().ScaleAllSizes(uiScale);
    ImGui::GetStyle().FontScaleDpi = uiScale;
    ImGui::GetStyle().MouseCursorScale = 1.0f;
    firstFrame = GetTickCount64();
    if (!gVisible.load(std::memory_order_acquire))
        gActiveWindow.store(hwnd, std::memory_order_release);
    return true;
}

void PlatformState::RetireInput()
{
    if (!inputAttached) return;
    inputAttached = false;
    // DetachWindow retains only the forwarding thunk while window-thread
    // button-up/cancellation is still needed to release owned mouse capture.
    DetachWindow(window);
}

void PlatformState::Shutdown()
{
    RetireInput();
    if (!context) return;
    ImGuiContext* previous = ImGui::GetCurrentContext();
    if (previous == context) previous = nullptr;
    ImGui::SetCurrentContext(context);
    InternalScope internal;
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext(context);
    context = nullptr;
    ImGui::SetCurrentContext(previous);
    window = nullptr;
}

bool PlatformState::WantsFrame() const noexcept
{
    // The edge state is shared with window/raw messages, so polling cannot
    // toggle twice for a single press. It also covers custom input pumps.
    if (context && inputAttached && Foreground(window))
    {
        InternalScope internal;
        if (gFirstLaunchPending)
        {
            SetVisible(true, window);
            if (gVisible.load(std::memory_order_acquire))
            {
                gFirstLaunchPending = false;
                gFirstLaunchSavePending = true;
            }
        }
        if (gMenuKey.Binding())
        {
            for (int key = VK_BACK; key < 255 && gMenuKey.Binding(); ++key)
                if (key == VK_ESCAPE || MenuHotkey::Bindable(key))
                    ProcessKey(window, key, (GetAsyncKeyState(key) & 0x8000) != 0);
        }
        else
        {
            const int key = gMenuKey.Key();
            ProcessKey(window, key, (GetAsyncKeyState(key) & 0x8000) != 0);
        }
    }
    return context && inputAttached && IsWindow(window) && (gVisible.load(std::memory_order_acquire)
        || GetTickCount64() - firstFrame < 7000);
}

bool PlatformState::NewFrame(uint32_t outputWidth, uint32_t outputHeight)
{
    InternalScope internal;
    const bool visible = gVisible.load(std::memory_order_acquire);
    if (visible && !inputWasVisible)
    {
        ImGui::GetIO().ClearInputKeys();
        ImGui::GetIO().ClearInputMouse();
    }
    inputWasVisible = visible;
    std::deque<InputMessage> messages;
    {
        std::lock_guard lock(gInputMutex);
        const auto found = gWindows.find(window);
        if (found != gWindows.end()) messages.swap(found->second.messages);
    }
    for (const auto& input : messages)
    {
        bool down = false;
        const int button = MouseButton(input.message, input.wparam, down);
        if (button >= 0)
        {
            // Native capture was handled synchronously by WindowProc. Calling
            // the Win32 backend's button handler here would attempt capture on
            // the wrong thread, or release capture that belongs to the game.
            ImGui::GetIO().AddMouseSourceEvent(input.mouseSource);
            ImGui::GetIO().AddMouseButtonEvent(button, down);
        }
        else ImGui_ImplWin32_WndProcHandler(window, input.message, input.wparam, input.lparam);
    }
    ImGui_ImplWin32_NewFrame();
    auto& io = ImGui::GetIO();
    const auto scaling = CalculateOverlayScaling(outputWidth, outputHeight,
        io.DisplaySize.x, io.DisplaySize.y, ImGui_ImplWin32_GetDpiScaleForHwnd(window));
    if (!scaling.valid) return false;
    io.DisplayFramebufferScale = ImVec2(scaling.framebufferX, scaling.framebufferY);
    if (scaling.logical != uiScale)
    {
        // Start from unscaled metrics so repeated resolution changes cannot
        // accumulate rounding damage to padding, borders or control sizes.
        uiScale = scaling.logical;
        ImGui::GetStyle() = baseStyle;
        ImGui::GetStyle().ScaleAllSizes(uiScale);
        ImGui::GetStyle().FontScaleDpi = uiScale;
    }
    // Cursor size is independent of the panel's resolution scaling. Undo
    // framebuffer magnification as well, so virtualized client sizes do not
    // enlarge the software cursor at the final output resolution.
    ImGui::GetStyle().MouseCursorScale = 1.0f
        / std::max(scaling.framebufferX, scaling.framebufferY);
    ImGui::NewFrame();
    return true;
}

void PlatformState::Draw()
{
    if (const int key = gMenuKey.TakePendingSave())
    {
        const auto path = standalone_ui::SettingsPath("RTX40MFG-UI.ini");
        const auto value = std::to_wstring(key);
        gShortcutSaveFailed = path.empty() || !WritePrivateProfileStringW(
            L"Overlay", L"ToggleKey", value.c_str(), path.c_str());
    }
    bool open = gVisible.load(std::memory_order_acquire);
    if (open)
    {
        const bool rescaleLayout = !layoutPrepared || std::abs(layoutScale-uiScale) > 0.0001f;
        const auto display = ImGui::GetIO().DisplaySize;
        const float margin = 16.0f*uiScale;
        const ImVec2 maximum(std::max(32.0f, display.x-2.0f*margin), std::max(32.0f, display.y-2.0f*margin));
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(std::min(360.0f*uiScale, maximum.x), std::min(180.0f*uiScale, maximum.y)), maximum);
        if (rescaleLayout)
        {
            ImVec2 size(570.0f*uiScale, 610.0f*uiScale), position(24.0f*uiScale, 24.0f*uiScale);
            const float ratio = uiScale/layoutScale;
            if (const ImGuiWindow* panel = ImGui::FindWindowByName("DLSS MFG"))
            {
                size = ImVec2(panel->SizeFull.x*ratio, panel->SizeFull.y*ratio);
                position = ImVec2(panel->Pos.x*ratio, panel->Pos.y*ratio);
            }
            else if (const ImGuiWindowSettings* saved = ImGui::FindWindowSettingsByID(ImHashStr("DLSS MFG")))
            {
                size = ImVec2(float(saved->Size.x)*ratio, float(saved->Size.y)*ratio);
                position = ImVec2(float(saved->Pos.x)*ratio, float(saved->Pos.y)*ratio);
            }
            size.x = std::clamp(size.x, std::min(360.0f*uiScale, maximum.x), maximum.x);
            size.y = std::clamp(size.y, std::min(180.0f*uiScale, maximum.y), maximum.y);
            position.x = std::clamp(position.x, 0.0f, std::max(0.0f, display.x-size.x));
            position.y = std::clamp(position.y, 0.0f, std::max(0.0f, display.y-size.y));
            ImGui::SetNextWindowSize(size, ImGuiCond_Always);
            ImGui::SetNextWindowPos(position, ImGuiCond_Always);
        }
        if (ImGui::Begin("DLSS MFG", &open))
        {
            ImGui::Text("Menu key: %s", KeyLabel().c_str());
            ImGui::SameLine();
            if (gMenuKey.Binding())
            {
                ImGui::TextUnformatted("Press a key... Esc cancels");
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) gMenuKey.CancelBinding();
            }
            else if (ImGui::Button("Change key")) gMenuKey.BeginBinding();
            if (gShortcutSaveFailed)
                ImGui::TextWrapped("The new shortcut works for this session, but could not be saved.");
            ImGui::Separator();
            standalone_ui::Draw();
        }
        ImGui::End();
        if (gFirstLaunchSavePending)
        {
            // Persist only after the full menu has reached Draw, never merely
            // on DLL load or a background/splash swapchain attachment. A failed
            // write leaves the next launch eligible, without per-frame I/O.
            gFirstLaunchSavePending = false;
            const auto preferences = standalone_ui::SettingsPath("RTX40MFG-UI.ini");
            if (!preferences.empty()) WritePrivateProfileStringW(
                L"Overlay", L"FirstLaunchMenuShown", L"1", preferences.c_str());
        }
        if (rescaleLayout)
        {
            layoutScale = uiScale;
            layoutPrepared = true;
            // Save the dimensions with their coordinate scale, so relaunching
            // at a different resolution rescales a saved panel exactly once.
            if (ImGui::GetIO().IniFilename)
            {
                ImGui::SaveIniSettingsToDisk(ImGui::GetIO().IniFilename);
                const auto preferences = standalone_ui::SettingsPath("RTX40MFG-UI.ini");
                const auto value = std::to_wstring(layoutScale);
                WritePrivateProfileStringW(L"Overlay", L"LayoutScale", value.c_str(), preferences.c_str());
            }
        }
        if (!open) SetVisible(false, window);
    }
    else if (GetTickCount64() - firstFrame < 7000)
    {
        ImGui::SetNextWindowPos(ImVec2(16.0f*uiScale, 16.0f*uiScale), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.85f);
        if (ImGui::Begin("##RTX40MFG startup", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize
            | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings))
        {
            ImGui::Text("RTX40MFG  |  %s opens DLSS MFG", KeyLabel().c_str());
        }
        ImGui::End();
    }
    ImGui::Render();
    gWantMouse.store(open, std::memory_order_release);
    gWantKeyboard.store(open, std::memory_order_release);
}

FARPROC ResolveBoundedInput(HMODULE module, LPCSTR name, FARPROC original) noexcept
{
    if (!name || !original || reinterpret_cast<uintptr_t>(name) <= 0xFFFFu) return original;
    // Only wrap the public entry from the actual pinned System32 user32 image.
    wchar_t path[MAX_PATH]{};
    wchar_t system[MAX_PATH]{};
    if (!GetModuleFileNameW(module, path, MAX_PATH) || !GetSystemDirectoryW(system, MAX_PATH)) return original;
    wcscat_s(system, L"\\user32.dll");
    if (_wcsicmp(path, system) || !slots::ImageEntry(module, reinterpret_cast<void*>(original))) return original;
    void* thunk = nullptr;
    if (!strcmp(name, "GetAsyncKeyState")) thunk = HookFamily<AsyncKeyTag, SHORT, int>::Forwarder(reinterpret_cast<void*>(original));
    else if (!strcmp(name, "GetKeyState")) thunk = HookFamily<KeyTag, SHORT, int>::Forwarder(reinterpret_cast<void*>(original));
    else if (!strcmp(name, "GetKeyboardState")) thunk = HookFamily<KeyboardTag, BOOL, PBYTE>::Forwarder(reinterpret_cast<void*>(original));
    else if (!strcmp(name, "SetCursorPos")) thunk = HookFamily<CursorPositionTag, BOOL, int, int>::Forwarder(reinterpret_cast<void*>(original));
    else if (!strcmp(name, "ClipCursor")) thunk = HookFamily<ClipTag, BOOL, const RECT*>::Forwarder(reinterpret_cast<void*>(original));
    return thunk ? reinterpret_cast<FARPROC>(thunk) : original;
}

bool PublishBoundedInputOriginal(const char* name, void* target, void* trampoline) noexcept
{
    if (!strcmp(name,"GetAsyncKeyState")) return HookFamily<AsyncKeyTag,SHORT,int>::PublishForwarderOriginal(target,trampoline);
    if (!strcmp(name,"GetKeyState")) return HookFamily<KeyTag,SHORT,int>::PublishForwarderOriginal(target,trampoline);
    if (!strcmp(name,"GetKeyboardState")) return HookFamily<KeyboardTag,BOOL,PBYTE>::PublishForwarderOriginal(target,trampoline);
    if (!strcmp(name,"SetCursorPos")) return HookFamily<CursorPositionTag,BOOL,int,int>::PublishForwarderOriginal(target,trampoline);
    if (!strcmp(name,"ClipCursor")) return HookFamily<ClipTag,BOOL,const RECT*>::PublishForwarderOriginal(target,trampoline);
    return false;
}

bool PrepareBoundedInput(HMODULE user32, slots::Batch& batch) noexcept
{
    HMODULE executable = GetModuleHandleW(nullptr);
    return slots::VisitImports(executable, [&](const char* name, void** address) {
        FARPROC original = GetProcAddress(user32, name);
        FARPROC replacement = ResolveBoundedInput(user32, name, original);
        if (replacement == original) return true;
        if (*address != reinterpret_cast<void*>(original))
        {
            single_module::Log(L"D3D12 UI: foreign input IAT slot retained; window messages remain available");
            return true;
        }
        const bool added=batch.Add(executable, address, reinterpret_cast<void*>(original), reinterpret_cast<void*>(replacement), name, &PublishBoundedInputOriginal);
        if (!added) {
            MEMORY_BASIC_INFORMATION memory{};VirtualQuery(address,&memory,sizeof(memory));
            wchar_t line[256]{};
            swprintf_s(line,L"MFG_PROXY_UI input preparation failed symbol=%hs slotRva=0x%llX protection=0x%lX",name,
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address)-reinterpret_cast<uintptr_t>(executable)),memory.Protect);
            single_module::Log(line);
        }
        return added;
    });
}

void OnBoundedInputActivated() noexcept
{
    HookFamily<AsyncKeyTag, SHORT, int>::MarkAllPreparedActivated();
    HookFamily<KeyTag, SHORT, int>::MarkAllPreparedActivated();
    HookFamily<KeyboardTag, BOOL, PBYTE>::MarkAllPreparedActivated();
    HookFamily<CursorPositionTag, BOOL, int, int>::MarkAllPreparedActivated();
    HookFamily<ClipTag, BOOL, const RECT*>::MarkAllPreparedActivated();
}

void RecordFailure(const wchar_t* reason) noexcept
{
    if (gRenderFailures.fetch_add(1, std::memory_order_relaxed) < 12)
        single_module::Log(reason);
}

void ReadStatus(MfgSingleModuleStatus& status) noexcept
{
    status.dxgiHooked = gDxgiHooked.load();
    status.vulkanHooked = gVulkanHooked.load();
    status.overlayVisible = gVisible.load();
    status.renderFailures = gRenderFailures.load();
    status.shortcutVirtualKey = static_cast<uint32_t>(gMenuKey.Key());
    status.shortcutBindingActive = gMenuKey.Binding();
    status.dxgiFrames = gDxgiFrames.load();
    status.vulkanFrames = gVulkanFrames.load();
    status.renderedFrames = gRenderedFrames.load();
}
}
