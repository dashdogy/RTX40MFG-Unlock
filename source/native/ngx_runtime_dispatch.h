#pragma once

#include "entry_detour.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>

namespace ngx_runtime_dispatch
{
enum class ReadResult : uint8_t { eUnrecognized, eUninitialized, eSelected, eInvalid };
struct Layout
{
    HMODULE runtime{};
    uintptr_t create = 0, helper = 0, pointer = 0;
    uint32_t imageBytes = 0, stride = 0, functionOffset = 0;
    std::array<uint8_t, 0x5d> createWindow{};
    std::array<uint8_t, 0x38> helperWindow{};
    std::array<uint8_t, 9> helperPrefix{};
    explicit operator bool() const noexcept { return runtime && pointer && stride; }
};
struct Selection
{
    HMODULE runtime{}, provider{};
    uintptr_t pointer = 0, table = 0, slot = 0, target = 0;
    explicit operator bool() const noexcept { return runtime && provider && target; }
};

inline bool Readable(uintptr_t address, size_t bytes, HMODULE owner = nullptr, bool executable = false) noexcept
{
    if (!address || !bytes || bytes > UINTPTR_MAX - address) return false;
    const uintptr_t end = address + bytes;
    while (address < end)
    {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<void*>(address), &memory, sizeof(memory)) != sizeof(memory)
            || memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS))
            || (owner && (memory.AllocationBase != owner || memory.Type != MEM_IMAGE))) return false;
        const DWORD protection = memory.Protect & 0xff;
        const bool read = protection == PAGE_READONLY || protection == PAGE_READWRITE || protection == PAGE_WRITECOPY
            || protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
        const bool execute = protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
        if (!read || (executable && !execute)) return false;
        const uintptr_t begin = reinterpret_cast<uintptr_t>(memory.BaseAddress);
        if (memory.RegionSize > UINTPTR_MAX - begin) return false;
        const uintptr_t next = begin + memory.RegionSize;
        if (next <= address) return false;
        address = next;
    }
    return true;
}
inline bool Inside(uintptr_t base, uint32_t size, uintptr_t address, size_t bytes) noexcept
{ return address >= base && address - base < size && bytes <= size - (address - base); }
inline bool AddRelative(uintptr_t next, int32_t displacement, uintptr_t& result) noexcept
{
    if (displacement < 0)
    {
        const auto distance = static_cast<uint32_t>(-static_cast<int64_t>(displacement));
        if (next < distance) return false;
        result = next - distance;
    }
    else
    {
        if (static_cast<uint32_t>(displacement) > UINTPTR_MAX - next) return false;
        result = next + static_cast<uint32_t>(displacement);
    }
    return true;
}
inline int32_t Signed32(const uint8_t* source) noexcept
{ int32_t value = 0; memcpy(&value, source, sizeof(value)); return value; }
template<size_t N> inline bool Bytes(const uint8_t* address, const uint8_t (&expected)[N]) noexcept
{ return memcmp(address, expected, N) == 0; }

inline bool Discover(HMODULE module, const void* target, Layout& output) noexcept
{
    output = {};
    const uintptr_t base = reinterpret_cast<uintptr_t>(module), create = reinterpret_cast<uintptr_t>(target);
    if (!Readable(base, sizeof(IMAGE_DOS_HEADER), module)) return false;
    __try
    {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < sizeof(IMAGE_DOS_HEADER)
            || dos->e_lfanew > 0x100000 || static_cast<uint32_t>(dos->e_lfanew) > UINTPTR_MAX - base) return false;
        const uintptr_t ntAddress = base + dos->e_lfanew;
        if (!Readable(ntAddress, sizeof(IMAGE_NT_HEADERS64), module)) return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(ntAddress);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64
            || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
        const uint32_t size = nt->OptionalHeader.SizeOfImage;
        if (size < 4096 || size > 1024u * 1024u * 1024u || size > UINTPTR_MAX - base
            || !Inside(base, size, create, 0xe1) || !Readable(create, 0xe1, module, true)) return false;
        const auto* code = reinterpret_cast<const uint8_t*>(create);
        constexpr uint8_t load[]{0x48,0x8b,0x05}, test[]{0x48,0x85,0xc0,0x75,0x36}, store[]{0x48,0x89,0x05};
        // The separately decoded helper call follows this exact argument setup.
        constexpr uint8_t callSetup[]{0x48,0x89,0x7c,0x24,0x28,0x4c,0x8b,0xce,0x4d,0x8b,0xc6,
            0x48,0x89,0x6c,0x24,0x20,0x41,0x8b,0xd7,0x48,0x8b,0xc8,0xe8};
        if (!Bytes(code+0x84, load) || !Bytes(code+0x8b, test) || !Bytes(code+0xbf, store)
            || !Bytes(code+0xc6, callSetup)) return false;
        uintptr_t pointer = 0, storedPointer = 0, helper = 0;
        if (!AddRelative(create+0x8b, Signed32(code+0x87), pointer)
            || !AddRelative(create+0xc6, Signed32(code+0xc2), storedPointer) || pointer != storedPointer
            || pointer % alignof(uintptr_t) || !Inside(base, size, pointer, sizeof(uintptr_t))
            || !Readable(pointer, sizeof(uintptr_t), module)
            || !AddRelative(create+0xe1, Signed32(code+0xdd), helper)
            || !Inside(base, size, helper, 0x178) || !Readable(helper, 0x178, module, true)) return false;
        const auto* body = reinterpret_cast<const uint8_t*>(helper);
        constexpr uint8_t prefix[]{0x4d,0x8b,0xf8,0x48,0x63,0xea,0x48,0x8b,0xf1};
        constexpr uint8_t scale[]{0x48,0x69,0xc7}, compare[]{0x48,0x83,0xbc,0x30};
        constexpr uint8_t branch[]{0x00,0x74,0xb5,0x48,0x69,0xcd}, fetch[]{0x48,0x8b,0x84,0x31};
        constexpr uint8_t invoke[]{0x4d,0x8b,0xce,0x4d,0x8b,0xc7,0x8b,0xd5,0x48,0x8b,0x8c,0x24,0x98,0,0,0,0xff,0x15};
        if (!Bytes(body+0x18, prefix) || !Bytes(body+0x140, scale) || !Bytes(body+0x147, compare)
            || !Bytes(body+0x14f, branch) || !Bytes(body+0x159, fetch) || !Bytes(body+0x161, invoke)) return false;
        const int32_t stride = Signed32(body+0x143), offset = Signed32(body+0x14b);
        if (stride != Signed32(body+0x155) || offset != Signed32(body+0x15d)
            || stride < 8 || stride > 0x400 || offset < 0 || offset > 0x10000
            || stride % 8 || offset % 8) return false;
        output.runtime = module; output.create = create; output.helper = helper;
        output.pointer = pointer; output.imageBytes = size;
        output.stride = static_cast<uint32_t>(stride); output.functionOffset = static_cast<uint32_t>(offset);
        memcpy(output.createWindow.data(), code+0x84, output.createWindow.size());
        memcpy(output.helperWindow.data(), body+0x140, output.helperWindow.size());
        memcpy(output.helperPrefix.data(), body+0x18, output.helperPrefix.size());
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { output = {}; return false; }
}
inline bool Current(const Layout& layout) noexcept
{
    if (!layout || !Readable(layout.create+0x84, layout.createWindow.size(), layout.runtime, true)
        || !Readable(layout.helper+0x140, layout.helperWindow.size(), layout.runtime, true)
        || !Readable(layout.helper+0x18, layout.helperPrefix.size(), layout.runtime, true)
        || !Readable(layout.pointer, sizeof(uintptr_t), layout.runtime)) return false;
    __try
    {
        return memcmp(reinterpret_cast<void*>(layout.create+0x84), layout.createWindow.data(), layout.createWindow.size()) == 0
            && memcmp(reinterpret_cast<void*>(layout.helper+0x140), layout.helperWindow.data(), layout.helperWindow.size()) == 0
            && memcmp(reinterpret_cast<void*>(layout.helper+0x18), layout.helperPrefix.data(), layout.helperPrefix.size()) == 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
inline bool Pointer(uintptr_t address, uintptr_t& value) noexcept
{
    if (address % alignof(uintptr_t) || !Readable(address, sizeof(uintptr_t))) return false;
    __try { value = *reinterpret_cast<const volatile uintptr_t*>(address); MemoryBarrier(); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
inline bool ExecutableOwner(uintptr_t target, HMODULE& owner) noexcept
{
    MEMORY_BASIC_INFORMATION memory{};
    if (!Readable(target, 1, nullptr, true)
        || VirtualQuery(reinterpret_cast<void*>(target), &memory, sizeof(memory)) != sizeof(memory)
        || memory.Type != MEM_IMAGE) return false;
    owner = static_cast<HMODULE>(memory.AllocationBase);
    return owner != nullptr;
}
inline ReadResult Read(const Layout& layout, uint32_t feature, Selection& selection) noexcept
{
    selection = {};
    if (!layout) return ReadResult::eUnrecognized;
    if (feature >= 0x13 || !Current(layout)) return ReadResult::eInvalid;
    uintptr_t table = 0;
    if (!Pointer(layout.pointer, table)) return ReadResult::eInvalid;
    if (!table) return ReadResult::eUninitialized;
    const uintptr_t offset = uintptr_t{feature} * layout.stride + layout.functionOffset;
    if (offset > UINTPTR_MAX - table) return ReadResult::eInvalid;
    const uintptr_t slot = table + offset;
    uintptr_t target = 0, currentTable = 0, currentTarget = 0;
    HMODULE provider = nullptr;
    if (!Pointer(slot, target)) return ReadResult::eInvalid;
    if (!target) return ReadResult::eUninitialized;
    if (!ExecutableOwner(target, provider) || !Pointer(layout.pointer, currentTable) || currentTable != table
        || !Pointer(slot, currentTarget) || currentTarget != target) return ReadResult::eInvalid;
    selection = {layout.runtime, provider, layout.pointer, table, slot, target};
    return ReadResult::eSelected;
}
inline bool StillCurrent(const Selection& selection) noexcept
{
    uintptr_t table = 0, target = 0;
    HMODULE owner = nullptr;
    return selection && Readable(selection.pointer, sizeof(uintptr_t), selection.runtime)
        && Pointer(selection.pointer, table) && table == selection.table
        && Pointer(selection.slot, target) && target == selection.target
        && ExecutableOwner(target, owner) && owner == selection.provider;
}

struct CachedLayout { entry_detour::Handle entry{}; HMODULE owner{}; uint64_t generation = 0; Layout layout{}; };
inline std::array<CachedLayout, 16> gLayouts{};
inline std::mutex gMutex;
inline ReadResult Read(const entry_detour::Snapshot& create, uint32_t feature, Selection& selection) noexcept
{
    selection = {};
    if (!create.current || create.kind != entry_detour::Kind::eNgxRuntimeD3D12CreateFeature
        || !create.owner || !create.generation || !create.handle) return ReadResult::eUnrecognized;
    Layout layout{};
    {
        std::lock_guard lock(gMutex);
        CachedLayout* found = nullptr;
        for (auto& cached : gLayouts) if (cached.entry == create.handle) { found = &cached; break; }
        if (!found)
        {
            for (auto& cached : gLayouts) if (!cached.entry) { found = &cached; break; }
            if (!found) return ReadResult::eUnrecognized;
            found->entry = create.handle; found->owner = create.owner; found->generation = create.generation;
            Discover(create.owner, create.target, found->layout);
        }
        if (found->owner != create.owner || found->generation != create.generation) return ReadResult::eInvalid;
        layout = found->layout;
    }
    const auto current = entry_detour::ReadSnapshot(create.handle);
    if (!current.current || current.owner != create.owner || current.generation != create.generation
        || current.target != create.target) return ReadResult::eInvalid;
    return Read(layout, feature, selection);
}
} // namespace ngx_runtime_dispatch
