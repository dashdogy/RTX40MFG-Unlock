#include "ampere_font_native.inc"
constexpr char kFontSourceSha256[] =
    "C6CC4129606AF4A3931C98BF7F7405EB9F5469F6522DA914FE7F3E5B39A224A1";

bool FontBytesEqual(const FontProgram& font, bool replacement) noexcept
{
    if (!font.address) return false;
    __try
    {
        return memcmp(reinterpret_cast<const void*>(font.address),
            replacement ? font.replacement.data() : font.original.data(), kFontBytes) == 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool FontPageProtectionMatches(const FontProgram& font, size_t i) noexcept
{
    MEMORY_BASIC_INFORMATION memory{};
    const uintptr_t page = font.pageStart + i * font.pageBytes;
    if (VirtualQuery(reinterpret_cast<void*>(page), &memory, sizeof(memory)) != sizeof(memory)
        || memory.State != MEM_COMMIT || memory.Type != MEM_IMAGE || memory.AllocationBase != font.module)
        return false;
    if (memory.Protect != PAGE_READONLY && memory.Protect != PAGE_READWRITE && memory.Protect != PAGE_WRITECOPY)
        return false;
    if (memory.Protect == font.protections[i]) return true;
    // Windows reports a privatized WRITECOPY image page as READWRITE, even
    // after VirtualProtect(WRITECOPY). Accept that transition only when the
    // working set confirms the page is private; no extra permission is granted.
    if (font.protections[i] != PAGE_WRITECOPY || memory.Protect != PAGE_READWRITE) return false;
    uint8_t resident = 0;
    if (!SafeRead(page, resident)) return false;
    PSAPI_WORKING_SET_EX_INFORMATION working{};
    working.VirtualAddress = reinterpret_cast<void*>(page);
    return QueryWorkingSetEx(GetCurrentProcess(), &working, sizeof(working))
        && working.VirtualAttributes.Valid && !working.VirtualAttributes.Shared;
}
bool RestoreFontPage(const FontProgram& font, size_t i,
    protected_pointer::ProtectMemoryFn protect) noexcept
{
    for (unsigned retry = 0; retry != 2; ++retry)
    {
        DWORD ignored = 0;
        protect(reinterpret_cast<void*>(font.pageStart + i * font.pageBytes),
            font.pageBytes, font.protections[i], &ignored);
        if (FontPageProtectionMatches(font, i)) return true;
    }
    return false;
}
bool FontCurrent(const FontProgram& font, bool replacement) noexcept
{
    if (!font.module || !font.address || !font.pageBytes
        || !font.pageCount || font.pageCount > font.protections.size()) return false;
    for (size_t i = 0; i < font.pageCount; ++i)
    {
        if (!FontPageProtectionMatches(font, i)) return false;
    }
    return FontBytesEqual(font, replacement);
}

bool PrepareFont(HMODULE module, uint32_t imageSize, FontProgram& font,
    std::vector<uint8_t>& ptx, std::vector<uint8_t>& native)
{
    const uintptr_t base = reinterpret_cast<uintptr_t>(module);
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!SafeRead(base, dos) || dos.e_lfanew <= 0
        || static_cast<size_t>(dos.e_lfanew) > imageSize
        || sizeof(nt) > imageSize - static_cast<size_t>(dos.e_lfanew)
        || !SafeRead(base + dos.e_lfanew, nt)) return false;
    const size_t sectionOffset = static_cast<size_t>(dos.e_lfanew)
        + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
    if (sectionOffset > imageSize || !nt.FileHeader.NumberOfSections
        || nt.FileHeader.NumberOfSections > (imageSize - sectionOffset) / sizeof(IMAGE_SECTION_HEADER)) return false;
    uintptr_t found = 0;
    font.stage = "unique-source";
    // The exact auxiliary program, not a version-derived RVA or whole-DLL
    // hash, authorizes this private mutation. Reject duplicate matches.
    for (size_t s = 0; s < nt.FileHeader.NumberOfSections; ++s)
    {
        IMAGE_SECTION_HEADER section{};
        if (!SafeRead(base + sectionOffset + s * sizeof(section), section)) return false;
        if (!(section.Characteristics & IMAGE_SCN_MEM_READ)
            || (section.Characteristics & IMAGE_SCN_MEM_EXECUTE)
            || section.VirtualAddress >= imageSize) continue;
        const size_t bytes = std::min<size_t>(imageSize - section.VirtualAddress,
            std::max<size_t>(section.Misc.VirtualSize, section.SizeOfRawData));
        if (bytes < kFontBytes) continue;
        for (size_t off = 0; off <= bytes - kFontBytes; off += 8)
        {
            const uintptr_t address = base + section.VirtualAddress + off;
            uint32_t magic = 0;
            if (!SafeRead(address, magic)) return false;
            if (magic != 0xBA55ED50) continue;
            uint64_t payloadBytes = 0;
            if (!SafeRead(address + 8, payloadBytes) || payloadBytes != kFontBytes - 16) continue;
            if (!SafeCopy(font.original.data(), reinterpret_cast<void*>(address), kFontBytes)
                || !Sha256Equals(font.original.data(), kFontBytes, kFontSourceSha256)) continue;
            if (found) return false;
            found = address;
        }
    }
    if (!found || !SafeCopy(font.original.data(), reinterpret_cast<void*>(found), kFontBytes)) return false;
    font.module = module; font.address = found;
    font.stage = "pages";
    // Providers place this auxiliary fatbin in non-executable .data. Preserve
    // each page's actual access semantics; adjacent provider globals can write.
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    font.pageBytes = system.dwPageSize;
    if (font.pageBytes != 4096) return false;
    font.pageStart = found & ~(font.pageBytes - 1);
    font.pageCount = (found + kFontBytes - font.pageStart + font.pageBytes - 1) / font.pageBytes;
    if (font.pageCount > font.protections.size()) return false;
    for (size_t i = 0; i < font.pageCount; ++i)
    {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<void*>(font.pageStart + i * font.pageBytes), &memory, sizeof(memory)) != sizeof(memory)) return false;
        font.protections[i] = memory.Protect;
    }
    if (!FontCurrent(font, false)
        || !Sha256Equals(font.original.data(), kFontBytes, kFontSourceSha256)) return false;
    Sm89PtxMetadata metadata{};
    font.stage = "PTX-metadata";
    if (!FindUniqueSm89Ptx(found, kFontBytes, metadata)) return false;
    ptx.resize(kFontBytes);
    std::vector<uint8_t> scratch(metadata.compressedSize);
    uint32_t outputBytes = 0;
    StripFailure failure{};
    font.stage = "PTX-transform";
    if (!BuildAmpereSm86Fatbin(found, kFontBytes, metadata, ptx.data(), ptx.size(),
            scratch.data(), scratch.size(), outputBytes, failure, nullptr, false)) return false;
    ptx.resize(outputBytes);
    native.assign(kFontNative.begin(), kFontNative.end());
    font.stage = "prepared";
    return !ptx.empty() && ptx.size() <= kFontBytes && native.size() <= kFontBytes;
}

bool ExchangeFontWord(uintptr_t address, uint64_t expected, uint64_t replacement) noexcept
{
    __try
    {
        return static_cast<uint64_t>(InterlockedCompareExchange64(
            reinterpret_cast<volatile LONG64*>(address), static_cast<LONG64>(replacement),
            static_cast<LONG64>(expected))) == expected;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool WriteFont(FontProgram& font, bool replacement,
    protected_pointer::ProtectMemoryFn protect) noexcept
{
    if (!protect || !font.module || !font.address || !font.pageBytes
        || !font.pageCount || font.pageCount > font.protections.size() || (font.address & 7)
        || (replacement && !FontCurrent(font, false))) return false;
    size_t changed = 0;
    bool writable = true;
    for (size_t i = 0; i < font.pageCount; ++i)
    {
        const uintptr_t page = font.pageStart + i * font.pageBytes;
        MEMORY_BASIC_INFORMATION memory{};
        DWORD old = 0;
        if (VirtualQuery(reinterpret_cast<void*>(page), &memory, sizeof(memory)) != sizeof(memory)
            || memory.State != MEM_COMMIT || memory.Type != MEM_IMAGE
            || memory.AllocationBase != font.module)
        { writable = false; break; }
        // MEM_IMAGE remains MEM_IMAGE after COW. WRITECOPY is valid for both
        // shared and already private image pages; no executable page is touched.
        if (!protect(reinterpret_cast<void*>(page), font.pageBytes, PAGE_WRITECOPY, &old))
        { writable = false; break; }
        changed = i + 1;
    }
    bool copied = false;
    size_t publishedWords = 0;
    // Recheck the complete expected value after making every page writable.
    // A foreign pre-publication edit is never overwritten.
    if (writable && (!replacement || FontBytesEqual(font, false)))
    {
        copied = true;
        for (size_t offset = 0; offset < kFontBytes; offset += sizeof(uint64_t))
        {
            const uint64_t original = ReadU64(font.original.data() + offset);
            const uint64_t target = ReadU64(font.replacement.data() + offset);
            const bool exchanged = ExchangeFontWord(font.address + offset,
                replacement ? original : target, replacement ? target : original);
            if (exchanged) { if (replacement) ++publishedWords; continue; }
            uint64_t current = 0;
            if (!replacement && SafeRead(font.address + offset, current) && current == original) continue;
            copied = false;
            // Rollback visits every word and restores only our expected value.
            // A concurrent foreign word is preserved and admission stays closed.
            if (replacement) break;
        }
        if (!copied && replacement)
            for (size_t i = 0; i < publishedWords; ++i)
                ExchangeFontWord(font.address + i * sizeof(uint64_t),
                    ReadU64(font.replacement.data() + i * sizeof(uint64_t)),
                    ReadU64(font.original.data() + i * sizeof(uint64_t)));
    }
    std::atomic_thread_fence(std::memory_order_seq_cst);
    bool restored = true;
    for (size_t i = 0; i < changed; ++i)
        if (!RestoreFontPage(font, i, protect)) restored = false;
    const bool success = copied && restored && FontCurrent(font, replacement);
    if (!success && replacement && publishedWords) WriteFont(font, false, protect);
    return success;
}
