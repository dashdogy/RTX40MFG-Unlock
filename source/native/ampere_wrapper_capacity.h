#pragma once
#include <Windows.h>
#include <array>
#include <cstdint>
#include "ampere_mfg_limits.h"

namespace ampere_wrapper
{
enum class Profile : uint32_t { eUnknown, eCompact, eExtended };
// Read-only allocation view. The private layout is admitted by independently
// matched startup, swap-chain, buffer-loop and NGX creation code windows.
struct Layout
{
    HMODULE module{};
    uintptr_t contextSlot = 0;
    uintptr_t createBegin = 0, createEnd = 0;
    Profile profile = Profile::eUnknown;
};
struct Capacity
{
    uintptr_t context = 0, swapchain = 0;
    uintptr_t buffersBegin = 0, buffersEnd = 0, buffersCapacity = 0;
    std::array<uintptr_t, ampere_mfg::kPresentationBuffers> buffers{};
    bool operator==(const Capacity&) const = default;
};
// Failure reporting only. Observed fields do not certify an allocation.
struct Observation
{
    Capacity capacity{};
    uint32_t maximum = 0, bufferCount = 0;
    bool contextReadable = false, buffersReadable = false;
};
bool Discover(HMODULE, Layout&) noexcept;
bool BeforeSwapchain(const Layout&) noexcept;
bool Read(const Layout&, Capacity&) noexcept;
bool Matches(const Layout&, const Capacity&) noexcept;
Observation InspectFailure(const Layout&) noexcept;
bool CreationOnStack(const Layout&) noexcept;
}
