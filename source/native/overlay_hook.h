#pragma once
#include "single_module.h"
#include "third_party/minhook/include/MinHook.h"
#include "overlay_slots.h"
#include <array>
#include <atomic>
#include <mutex>
#include <utility>

namespace single_overlay
{
// Each distinct code entry has its own typed thunk and original pointer. This
// preserves ABI and forwarding across DXGI/Streamline/ReShade wrappers without
// guessing an original function from the object's current vtable.
template<class Tag, class R, class... Args>
struct HookFamily
{
    using Fn = R (WINAPI*)(Args...);
    struct Slot
    {
        void* target = nullptr;
        std::atomic<Fn> original{nullptr};
        bool installed = false;
    };
    static constexpr size_t kCapacity = 24;
    inline static std::array<Slot, kCapacity> slots{};
    inline static std::mutex mutex;

    template<size_t Index>
    static R WINAPI Entry(Args... args)
    {
        return Tag::Call(slots[Index].original.load(std::memory_order_acquire), args...);
    }
    template<size_t... Indices>
    static constexpr auto Entries(std::index_sequence<Indices...>)
    {
        return std::array<Fn, sizeof...(Indices)>{&Entry<Indices>...};
    }
    inline static constexpr auto entries = Entries(std::make_index_sequence<kCapacity>{});

    static HMODULE OriginalOwner(Fn original) noexcept
    {
        // Trampolines are private allocations. Recover the actual installed
        // image entry that owns this exact callback, never the current vtable.
        std::lock_guard lock(mutex);
        for (const auto& slot : slots)
        {
            if (!slot.installed || slot.original.load(std::memory_order_acquire) != original) continue;
            HMODULE owner = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                    | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(slot.target), &owner))
                return owner; // Bind already pinned this image for callback lifetime.
        }
        return nullptr;
    }

    static bool Installed(void* target = nullptr) noexcept
    {
        std::lock_guard lock(mutex);
        for (const auto& slot : slots)
            if (slot.installed && (!target || slot.target == target)) return true;
        return false;
    }

    static bool PublishForwarderOriginal(void* target, void* trampoline) noexcept
    {
        if (!target || !trampoline) return false;
        std::lock_guard lock(mutex);
        for (auto& slot : slots) if (slot.target == target) {
            slot.original.store(reinterpret_cast<Fn>(trampoline), std::memory_order_release);
            return true;
        }
        return false;
    }

    static Fn Bind(void* target, bool install) noexcept
    {
        if (!target) return nullptr;
        for (auto entry : entries)
            if (reinterpret_cast<void*>(entry) == target) return entry;
        MEMORY_BASIC_INFORMATION memory{};
        if (!VirtualQuery(target, &memory, sizeof(memory))
            || memory.Type != MEM_IMAGE || memory.State != MEM_COMMIT
            || !(memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ
                | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
            || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            return reinterpret_cast<Fn>(target);
        HMODULE owner = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(target), &owner)
            || owner != memory.AllocationBase)
            return reinterpret_cast<Fn>(target);

        std::lock_guard lock(mutex);
        size_t index = kCapacity;
        for (size_t i = 0; i < kCapacity; ++i)
            if (slots[i].target == target) { index = i; break; }
        if (index == kCapacity)
        {
            for (size_t i = 0; i < kCapacity; ++i)
                if (!slots[i].target) { index = i; break; }
            if (index == kCapacity) return reinterpret_cast<Fn>(target);
            slots[index].target = target;
            slots[index].original.store(reinterpret_cast<Fn>(target), std::memory_order_release);
        }
        auto& slot = slots[index];
        if (install && !slot.installed)
        {
            const auto init = MH_Initialize();
            if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
                return entries[index];
            void* original = nullptr;
            if (MH_CreateHook(target, reinterpret_cast<void*>(entries[index]), &original) == MH_OK)
            {
                slot.original.store(reinterpret_cast<Fn>(original), std::memory_order_release);
                if (MH_EnableHook(target) == MH_OK)
                    slot.installed = true;
                else
                {
                    MH_RemoveHook(target);
                    slot.original.store(reinterpret_cast<Fn>(target), std::memory_order_release);
                }
            }
        }
        return entries[index];
    }

    // Bounded-installation support: prepare a trampoline without enabling the
    // detour. The trampoline is published before any activation so a callback
    // racing an activation always forwards through a valid original. Returns
    // the thunk for the prepared slot, or null when the target cannot be
    // prepared (occupied capacity, creation failure, or already installed).
    static void* Prepare(void* target) noexcept
    {
        if (!target) return nullptr;
        std::lock_guard lock(mutex);
        size_t index = kCapacity;
        for (size_t i = 0; i < kCapacity; ++i)
            if (slots[i].target == target)
            {
                if (slots[i].installed) return nullptr;
                index = i;
                break;
            }
        if (index == kCapacity)
        {
            for (size_t i = 0; i < kCapacity; ++i)
                if (!slots[i].target) { index = i; break; }
            if (index == kCapacity) return nullptr;
            slots[index].target = target;
        }
        auto& slot = slots[index];
        if (!slot.original.load(std::memory_order_acquire)
            || slot.original.load(std::memory_order_acquire) == reinterpret_cast<Fn>(target))
        {
            const auto init = MH_Initialize();
            if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return nullptr;
            void* original = nullptr;
            if (MH_CreateHook(target, reinterpret_cast<void*>(entries[index]), &original) != MH_OK
                || !original)
                return nullptr;
            slot.original.store(reinterpret_cast<Fn>(original), std::memory_order_release);
        }
        return entries[index];
    }

    // Prepare an immutable typed forwarder. The original public entry remains
    // callable even if Steam subsequently changes its own inline hook.
    static void* Forwarder(void* target) noexcept
    {
        if (!target) return nullptr;
        std::lock_guard lock(mutex);
        for (size_t i = 0; i < kCapacity; ++i)
            if (slots[i].target == target) return reinterpret_cast<void*>(entries[i]);
        for (size_t i = 0; i < kCapacity; ++i)
            if (!slots[i].target)
            {
                slots[i].target = target;
                slots[i].original.store(reinterpret_cast<Fn>(target), std::memory_order_release);
                slots[i].installed = true;
                return reinterpret_cast<void*>(entries[i]);
            }
        return nullptr;
    }

    static bool PrepareSlot(HMODULE owner, void** address, single_overlay::slots::Batch& batch) noexcept
    {
        if (!single_overlay::slots::ImageSlot(owner, address)) return false;
        void* original = protected_pointer::ReadPointer(reinterpret_cast<uintptr_t>(address));
        if (!single_overlay::slots::ImageEntry(owner, original)) return false;
        return batch.Add(owner, address, original, Forwarder(original));
    }

    // After a verified all-or-nothing activation of every prepared slot,
    // publish the installed state. Until this runs, callbacks never fire
    // because no detour is enabled; afterwards every thunk's original is the
    // trampoline published by Prepare.
    static size_t MarkAllPreparedActivated() noexcept
    {
        std::lock_guard lock(mutex);
        size_t activated = 0;
        for (auto& slot : slots)
            if (slot.target && !slot.installed)
            {
                slot.installed = true;
                ++activated;
            }
        return activated;
    }

    // True when this exact code entry is one of this family's installed
    // inline-detour targets. Used to verify that an observed object really
    // uses the implementations that were activated, without patching
    // anything new for it.
    static bool OwnsInstalledTarget(void* target) noexcept
    {
        if (!target) return false;
        std::lock_guard lock(mutex);
        for (size_t i = 0; i < kCapacity; ++i)
            if (slots[i].installed && reinterpret_cast<void*>(entries[i]) == target) return true;
        return false;
    }
};
}
