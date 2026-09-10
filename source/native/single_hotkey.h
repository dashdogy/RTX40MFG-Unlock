#pragma once
#include <Windows.h>
#include <array>
#include <atomic>

namespace single_overlay
{
inline constexpr int kDefaultMenuKey = VK_BACK;

enum class KeyAction { None, Toggle, Bound, Cancelled };

class MenuHotkey
{
    std::atomic<int> key_{kDefaultMenuKey};
    std::atomic<bool> binding_{false};
    std::atomic<int> pendingSave_{0};
    std::array<std::atomic<bool>, 256> down_{};
public:
    static bool Bindable(int key) noexcept
    {
        return key >= VK_BACK && key < 255 && key != VK_ESCAPE
            && key != VK_SHIFT && key != VK_CONTROL && key != VK_MENU
            && key != VK_LSHIFT && key != VK_RSHIFT
            && key != VK_LCONTROL && key != VK_RCONTROL
            && key != VK_LMENU && key != VK_RMENU
            && key != VK_LWIN && key != VK_RWIN;
    }
    int Key() const noexcept { return key_.load(std::memory_order_acquire); }
    bool Binding() const noexcept { return binding_.load(std::memory_order_acquire); }
    void BeginBinding() noexcept { binding_.store(true, std::memory_order_release); }
    void CancelBinding() noexcept { binding_.store(false, std::memory_order_release); }
    bool Load(int key) noexcept
    {
        if (!Bindable(key)) return false;
        key_.store(key, std::memory_order_release);
        return true;
    }
    int TakePendingSave() noexcept { return pendingSave_.exchange(0, std::memory_order_acq_rel); }
    void ResetPressed() noexcept
    { for (auto& down : down_) down.store(false, std::memory_order_release); }
    KeyAction Process(int key, bool down) noexcept
    {
        if (key < 0 || key >= 256) return KeyAction::None;
        const bool previous = down_[key].exchange(down, std::memory_order_acq_rel);
        if (!down || previous) return KeyAction::None;
        if (Binding())
        {
            if (key == VK_ESCAPE) { CancelBinding(); return KeyAction::Cancelled; }
            if (!Bindable(key)) return KeyAction::None;
            key_.store(key, std::memory_order_release);
            pendingSave_.store(key, std::memory_order_release);
            CancelBinding();
            return KeyAction::Bound;
        }
        return key == Key() ? KeyAction::Toggle : KeyAction::None;
    }
};
}
