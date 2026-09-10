#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace early_provider_load
{
// Windows serializes these notifications under its loader lock. The callback
// only records actual map/unmap events: module enumeration and LoadLibrary
// returns (including refcount-only returns) cannot create a fresh-load token.
// Storage never wraps. Exhaustion permanently removes this optional proof.
template <size_t Capacity = 1024>
class Tracker
{
    static_assert(Capacity > 0 && Capacity < UINT32_MAX);
    static_assert(std::atomic<uintptr_t>::is_always_lock_free);
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
    static_assert(std::atomic<uint32_t>::is_always_lock_free);
    static_assert(std::atomic<bool>::is_always_lock_free);

    struct Entry
    {
        std::atomic<uintptr_t> module{0};
        std::atomic<bool> loaded{false};
        std::atomic<uint64_t> token{0};
    };
    std::array<Entry, Capacity> entries_{};
    std::atomic<uint32_t> count_{0};
    std::atomic<bool> overflow_{false};

public:
    void Loaded(uintptr_t module) noexcept
    {
        if (!module || overflow_.load(std::memory_order_acquire))
            return;
        const uint32_t index = count_.fetch_add(1, std::memory_order_acq_rel);
        if (index >= Capacity)
        {
            overflow_.store(true, std::memory_order_release);
            return;
        }
        auto& entry = entries_[index];
        entry.module.store(module, std::memory_order_relaxed);
        entry.loaded.store(true, std::memory_order_relaxed);
        entry.token.store(static_cast<uint64_t>(index) + 1, std::memory_order_release);
    }

    void Unloaded(uintptr_t module) noexcept
    {
        if (!module)
            return;
        const uint32_t count = count_.load(std::memory_order_acquire);
        const size_t limit = count < Capacity ? count : Capacity;
        for (size_t index = 0; index < limit; ++index)
        {
            auto& entry = entries_[index];
            if (entry.token.load(std::memory_order_acquire)
                && entry.module.load(std::memory_order_relaxed) == module)
                entry.loaded.store(false, std::memory_order_release);
        }
    }

    uint64_t Current(uintptr_t module) const noexcept
    {
        if (!module || overflow_.load(std::memory_order_acquire))
            return 0;
        const uint32_t count = count_.load(std::memory_order_acquire);
        if (count > Capacity)
            return 0;
        uint64_t found = 0;
        for (size_t index = 0; index < count; ++index)
        {
            const auto& entry = entries_[index];
            const uint64_t token = entry.token.load(std::memory_order_acquire);
            // A notification is still publishing. Do not infer that its
            // absent module field means it concerns a different provider.
            if (!token)
                return 0;
            if (entry.module.load(std::memory_order_relaxed) == module)
                found = entry.loaded.load(std::memory_order_acquire) ? token : 0;
        }
        return count == count_.load(std::memory_order_acquire)
                && !overflow_.load(std::memory_order_acquire)
            ? found : 0;
    }

    bool Matches(uintptr_t module, uint64_t token) const noexcept
    {
        return token != 0 && Current(module) == token;
    }

    bool Overflowed() const noexcept
    {
        return overflow_.load(std::memory_order_acquire);
    }
};
}
