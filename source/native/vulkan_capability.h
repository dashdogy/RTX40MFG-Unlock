#pragma once

#include "protected_pointer.h"
#include <algorithm>
#include <array>
#include <mutex>

namespace vulkan_capability
{
struct Match
{
    uint8_t* threshold = nullptr;
    uint32_t rva = 0;
    uint32_t count = 0;
};

// The capability writer is separate from the Evaluate count validator.
// Recognize the native 1/5-frame decision, its decoded branch destination,
// and the RIP-relative DLSS-G parameter name. Never select an RVA by version.
inline Match Find(HMODULE module) noexcept
{
    Match result{};
    if (!module) return result;
    auto* base = reinterpret_cast<uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000)
        return result;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return result;
    const size_t imageSize = nt->OptionalHeader.SizeOfImage;
    auto inside = [imageSize](int64_t offset, size_t size) {
        return offset >= 0 && size <= imageSize && uint64_t(offset) <= imageSize - size;
    };
    const auto* section = IMAGE_FIRST_SECTION(nt);
    if (!inside(reinterpret_cast<const uint8_t*>(section) - base,
            size_t(nt->FileHeader.NumberOfSections) * sizeof(*section)))
        return result;
    constexpr std::array<uint8_t, 14> tail{
        0xBF, 1, 0, 0, 0, 0x49, 0x8B, 0x07, 0x44, 0x8B, 0xC7, 0x48, 0x8D, 0x15};
    constexpr char name[] = "DLSSG.MultiFrameCountMax";
    for (unsigned s = 0; s != nt->FileHeader.NumberOfSections; ++s)
    {
        const auto& entry = section[s];
        if (!(entry.Characteristics & IMAGE_SCN_MEM_EXECUTE) || entry.VirtualAddress >= imageSize)
            continue;
        const size_t length = std::min<size_t>(imageSize - entry.VirtualAddress,
            std::max(entry.Misc.VirtualSize, entry.SizeOfRawData));
        for (size_t offset = 0; offset + 17 <= length; ++offset)
        {
            const auto rva = entry.VirtualAddress + offset;
            auto* code = base + rva;
            if (code[0] != 0x81 || code[1] != 0xFD || (code[2] != 0xB0 && code[2] != 0x90)
                || code[3] != 1 || code[4] || code[5] || code[6] != 0x0F || code[7] != 0x8C
                || code[12] != 0xBF || code[13] != 5 || code[14] || code[15] || code[16])
                continue;
            int32_t branch = 0; memcpy(&branch, code + 8, sizeof(branch));
            const int64_t destination = int64_t(rva) + 12 + branch;
            if (!inside(destination, 18) || destination < int64_t(rva + 17)
                || destination + 18 > int64_t(entry.VirtualAddress + length)
                || memcmp(base + destination, tail.data(), tail.size()) != 0)
                continue;
            int32_t displacement = 0; memcpy(&displacement, base + destination + 14, sizeof(displacement));
            const int64_t stringRva = destination + 18 + displacement;
            if (!inside(stringRva, sizeof(name)) || memcmp(base + stringRva, name, sizeof(name)))
                continue;
            ++result.count;
            result.threshold = code + 2;
            result.rva = uint32_t(rva + 2);
        }
    }
    if (result.count != 1) result.threshold = nullptr;
    return result;
}

// Caller must retain and validate DLSS-G identity, the Vulkan adapter, entry
// generation, native Evaluate limit and temporal program before calling.
inline bool Publish(const Match& match) noexcept
{
    static std::mutex publicationMutex;
    std::lock_guard lock(publicationMutex);
    if (match.count != 1 || !match.threshold) return false;
    if (*match.threshold == 0x90) return true;
    if (*match.threshold != 0xB0) return false;
    DWORD old = 0;
    if (!VirtualProtect(match.threshold, 1, PAGE_EXECUTE_READWRITE, &old)) return false;
    const char observed = _InterlockedCompareExchange8(reinterpret_cast<volatile char*>(match.threshold),
        static_cast<char>(0x90), static_cast<char>(0xB0));
    const bool written = static_cast<uint8_t>(observed) == 0xB0 || static_cast<uint8_t>(observed) == 0x90;
    const bool flushed = FlushInstructionCache(GetCurrentProcess(), match.threshold, 1) != FALSE;
    const bool restored = protected_pointer::RestoreProtectionWithRetry(match.threshold, 1, old, &VirtualProtect);
    if (written && flushed && restored && *match.threshold == 0x90) return true;
    // Recover the original instruction if publication cannot be verified.
    DWORD ignored = 0;
    if (VirtualProtect(match.threshold, 1, PAGE_EXECUTE_READWRITE, &ignored))
    {
        _InterlockedCompareExchange8(reinterpret_cast<volatile char*>(match.threshold),
            static_cast<char>(0xB0), static_cast<char>(0x90));
        FlushInstructionCache(GetCurrentProcess(), match.threshold, 1);
        protected_pointer::RestoreProtectionWithRetry(match.threshold, 1, old, &VirtualProtect);
    }
    return false;
}
}
