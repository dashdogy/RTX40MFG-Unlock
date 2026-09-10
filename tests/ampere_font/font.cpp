#include "ampere_gpu.cpp"
#include "../ampere_publication/fixture.h"

namespace
{
using namespace ampere_gpu;
using namespace publication_fixture;
enum class Fault { eNone, ePageWrite, eForeignBeforeWrite, eForeignAfterTables, eBoundaryAfterFont, eRestoreForever };
Fixture* active = nullptr;
Fault fault = Fault::eNone;
bool fired = false;
BOOL WINAPI Protect(LPVOID address, SIZE_T bytes, DWORD protection, PDWORD previous)
{
    auto& f = *active->program.font;
    const uintptr_t at = reinterpret_cast<uintptr_t>(address);
    const bool fontPage = at >= f.pageStart && at < f.pageStart + f.pageCount * f.pageBytes;
    const bool writable = fontPage ? protection == PAGE_WRITECOPY
        : protection == PAGE_READWRITE || protection == PAGE_WRITECOPY;
    if (!fired && fontPage && writable && fault == Fault::ePageWrite && at == f.pageStart + f.pageBytes)
    { fired = true; return FALSE; }
    if (fontPage && !writable && fault == Fault::eRestoreForever)
    { fired = true; return FALSE; }
    const BOOL result = VirtualProtect(address, bytes, protection, previous);
    if (!result || fired) return result;
    if (fontPage && writable && fault == Fault::eForeignBeforeWrite)
    { fired = true; *reinterpret_cast<uint8_t*>(f.address) ^= 1; }
    if (fontPage && !writable && at == f.pageStart + (f.pageCount - 1) * f.pageBytes
        && fault == Fault::eBoundaryAfterFont)
    { fired = true; boundaryCurrent = false; }
    if (!writable && at == active->program.table + (active->program.slotCount - 1) * sizeof(uintptr_t)
        && fault == Fault::eForeignAfterTables)
    {
        fired = true;
        DWORD ignored = 0;
        VirtualProtect(reinterpret_cast<void*>(f.address), 8, PAGE_WRITECOPY, &ignored);
        *reinterpret_cast<uint8_t*>(f.address) ^= 1;
        VirtualProtect(reinterpret_cast<void*>(f.address), 8, f.protections[0], &ignored);
    }
    return result;
}
void RestoreOwnedFixture(FontProgram& font)
{
    // This process has never run any provider code and exclusively owns these
    // test mutations. Restore fixture bytes, including deliberate foreign edits.
    DWORD ignored = 0;
    Check(VirtualProtect(reinterpret_cast<void*>(font.address), kFontBytes, PAGE_WRITECOPY, &ignored), "fixture writable for cleanup");
    Check(SafeCopy(reinterpret_cast<void*>(font.address), font.original.data(), kFontBytes), "fixture bytes restored");
    for (size_t i = 0; i < font.pageCount; ++i)
        Check(VirtualProtect(reinterpret_cast<void*>(font.pageStart + i * font.pageBytes), font.pageBytes,
            font.protections[i], &ignored), "fixture protection restored");
}
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2) return 2;
    try
    {
        for (bool ptxMode : {false, true})
        {
            Fixture fixture(argv[1]); active = &fixture;
            auto& font = *fixture.program.font;
            std::array<uint8_t, 32> before{}, after{}, observed{};
            Check(SafeCopy(before.data(), reinterpret_cast<void*>(font.address - before.size()), before.size())
                && SafeCopy(after.data(), reinterpret_cast<void*>(font.address + kFontBytes), after.size()), "adjacent payload snapshot");
            if (ptxMode)
            {
                FontProgram fresh{};
                std::vector<uint8_t> ptx, native;
                uint32_t bytes = 0;
                Check(ImageSize(fixture.program.module, bytes) && PrepareFont(fixture.program.module, bytes, fresh, ptx, native),
                    "PTX font preparation from exact original");
                font.replacement.fill(0);
                std::copy(ptx.begin(), ptx.end(), font.replacement.begin());
            }
            const auto result = PublishProgram(fixture.program, fixture.payloads);
            if (!result.success) printf("FONT_TRANSACTION_REJECTED attempted=%u slot=%zu rollback=%u original=%u replacement=%u protect=%lu/%lu/%lu\n",
                result.attempted, result.failedSlot, result.rollbackComplete, FontCurrent(font, false), FontCurrent(font, true),
                font.protections[0], font.protections[1], font.protections[2]);
            if (!result.success)
                for (size_t i = 0; i < font.pageCount; ++i)
                {
                    MEMORY_BASIC_INFORMATION memory{};
                    VirtualQuery(reinterpret_cast<void*>(font.pageStart + i * font.pageBytes), &memory, sizeof(memory));
                    printf("FONT_PAGE %zu actual=%lu state=%lu type=%lu bytesOriginal=%u bytesReplacement=%u\n", i,
                        memory.Protect, memory.State, memory.Type, FontBytesEqual(font, false), FontBytesEqual(font, true));
                }
            Check(result.success && ProgramCurrent(fixture.program, 42), "complete font and FG transaction admitted");
            Check(FontCurrent(font, true), "all font bytes and every page protection verified");
            Check(SafeCopy(observed.data(), reinterpret_cast<void*>(font.address - before.size()), observed.size())
                && observed == before, "preceding resource unchanged");
            Check(SafeCopy(observed.data(), reinterpret_cast<void*>(font.address + kFontBytes), observed.size())
                && observed == after, "following resource unchanged");
            Check(WriteFont(font, false, &VirtualProtect) && FontCurrent(font, false), "font rollback verified");
            Check(!ProgramCurrent(fixture.program, 42), "missing font invalidates previously published FG program");
        }
        for (Fault mode : {Fault::ePageWrite, Fault::eForeignBeforeWrite, Fault::eForeignAfterTables,
                Fault::eBoundaryAfterFont, Fault::eRestoreForever})
        {
            Fixture fixture(argv[1]); active = &fixture; fault = mode; fired = false;
            // Faults exercise a distinct temporary/final permission transition.
            // The positive cases above use the provider's original COW pages.
            DWORD ignored = 0;
            Check(VirtualProtect(reinterpret_cast<void*>(fixture.program.font->address), kFontBytes,
                PAGE_READONLY, &ignored), "fixture font made read-only for protection-fault cases");
            fixture.program.font->protections.fill(PAGE_READONLY);
            const auto result = PublishProgram(fixture.program, fixture.payloads, &Protect);
            Check(fired && result.attempted && !result.success && !ProgramCurrent(fixture.program, 42),
                "font transaction fault blocks complete-program admission");
            const bool rollback = mode == Fault::ePageWrite || mode == Fault::eBoundaryAfterFont;
            Check(result.rollbackComplete == rollback, "font rollback result includes byte and protection verification");
            if (mode == Fault::eForeignBeforeWrite || mode == Fault::eForeignAfterTables)
            {
                uint8_t current = 0;
                const uint8_t expected = (mode == Fault::eForeignBeforeWrite
                    ? fixture.program.font->original[0] : fixture.program.font->replacement[0]) ^ 1;
                Check(SafeRead(fixture.program.font->address, current) && current == expected, "font rollback preserves foreign word");
            }
            RestoreOwnedFixture(*fixture.program.font);
        }
        fault = Fault::eNone; fired = false;
        {
            Fixture fixture(argv[1]); active = &fixture;
            auto saved = fixture.program.font;
            fixture.program.font.reset();
            const auto missing = PublishProgram(fixture.program, fixture.payloads);
            Check(!missing.attempted && !missing.success, "font required before any publication");
            fixture.program.font = saved;
            DWORD ignored = 0;
            VirtualProtect(reinterpret_cast<void*>(saved->address), 8, PAGE_WRITECOPY, &ignored);
            *reinterpret_cast<uint8_t*>(saved->address) ^= 1;
            VirtualProtect(reinterpret_cast<void*>(saved->address), 8, saved->protections[0], &ignored);
            const auto wrong = PublishProgram(fixture.program, fixture.payloads);
            Check(!wrong.attempted && !wrong.success, "modified font source rejected before any mutation");
            FontProgram fresh{};
            std::vector<uint8_t> ptx, native;
            uint32_t bytes = 0;
            Check(ImageSize(fixture.program.module, bytes)
                && !PrepareFont(fixture.program.module, bytes, fresh, ptx, native), "wrong auxiliary payload identity fails discovery");
            RestoreOwnedFixture(*saved);
            Check(!PrepareFont(fixture.program.module,
                static_cast<uint32_t>(saved->address - reinterpret_cast<uintptr_t>(fixture.program.module) + kFontBytes - 1),
                fresh, ptx, native), "font extending beyond the provider image rejected");
            const uintptr_t base = reinterpret_cast<uintptr_t>(fixture.program.module);
            IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
            Check(SafeRead(base, dos) && SafeRead(base + dos.e_lfanew, nt), "fixture section headers");
            const uintptr_t sections = base + dos.e_lfanew + offsetof(IMAGE_NT_HEADERS64, OptionalHeader)
                + nt.FileHeader.SizeOfOptionalHeader;
            uintptr_t duplicate = 0;
            for (size_t i = 0; i < nt.FileHeader.NumberOfSections; ++i)
            {
                IMAGE_SECTION_HEADER section{};
                Check(SafeRead(sections + i * sizeof(section), section), "fixture data section");
                const uintptr_t at = base + section.VirtualAddress;
                if (!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE)
                    && (section.Characteristics & IMAGE_SCN_MEM_WRITE) && section.Misc.VirtualSize >= kFontBytes
                    && (at + kFontBytes <= saved->address || at >= saved->address + kFontBytes))
                { duplicate = at; break; }
            }
            Check(duplicate != 0, "independent data-only duplicate fixture region");
            std::vector<uint8_t> old(kFontBytes);
            Check(SafeCopy(old.data(), reinterpret_cast<void*>(duplicate), old.size()), "duplicate region snapshot");
            Check(SafeCopy(reinterpret_cast<void*>(duplicate), saved->original.data(), kFontBytes), "duplicate font fixture placed");
            const bool duplicateRejected = !PrepareFont(fixture.program.module, bytes, fresh, ptx, native);
            Check(SafeCopy(reinterpret_cast<void*>(duplicate), old.data(), old.size()), "duplicate region restored");
            Check(duplicateRejected, "multiple exact font payloads rejected");
        }
        active = nullptr;
        printf("AMPERE_FONT_PASSED fixtureAndBoundaryChecks=%u\n", checks);
        return 0;
    }
    catch (const std::exception& error) { fprintf(stderr, "FAIL %s\n", error.what()); return 1; }
}
