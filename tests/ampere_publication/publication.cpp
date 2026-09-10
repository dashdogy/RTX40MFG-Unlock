#define MFG_PUBLICATION_CANDIDATE 1
#include "ampere_gpu.cpp"
#include "fixture.h"

namespace
{
enum class Fault { eNone, eWrite, eRestoreOnce, eRestoreForever, eForeignBeforeCas, eForeignAfterLast,
    ePayloadAfterLast, eWritableAfterLast, eBoundaryAfterLast };
publication_fixture::Fixture* active = nullptr;
Fault fault = Fault::eNone;
size_t faultSlot = 12;
bool fired = false;

BOOL WINAPI Protect(LPVOID address, SIZE_T bytes, DWORD protection, PDWORD previous)
{
    using namespace ampere_gpu;
    const uintptr_t value = reinterpret_cast<uintptr_t>(address);
    const uintptr_t table = active ? active->program.table : 0;
    const size_t count = active ? active->payloads.size() : 0;
    const size_t slot = value >= table && value < table + count * sizeof(uintptr_t)
        ? (value - table) / sizeof(uintptr_t) : SIZE_MAX;
    const bool writable = protection == PAGE_READWRITE || protection == PAGE_WRITECOPY;
    if (slot == faultSlot && fault == Fault::eWrite && writable && !fired)
    { fired = true; SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    if (!writable && ((fault == Fault::eRestoreOnce && slot == faultSlot && !fired)
            || (fault == Fault::eRestoreForever && (fired || slot == faultSlot))))
    { fired = true; SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    const BOOL result = VirtualProtect(address, bytes, protection, previous);
    if (!result || !active || fired) return result;
    if (fault == Fault::eForeignBeforeCas && slot == faultSlot && writable)
    {
        fired = true;
        *reinterpret_cast<uintptr_t*>(address) = active->program.originals[slot] + 8;
    }
    if (count && slot == count - 1 && !writable
        && (fault == Fault::eForeignAfterLast || fault == Fault::ePayloadAfterLast
            || fault == Fault::eWritableAfterLast || fault == Fault::eBoundaryAfterLast))
    {
        fired = true;
        DWORD ignored = 0;
        if (fault == Fault::eForeignAfterLast)
        {
            VirtualProtect(reinterpret_cast<void*>(table), sizeof(uintptr_t), PAGE_READWRITE, &ignored);
            *reinterpret_cast<uintptr_t*>(table) = active->program.originals[0] + 8;
            VirtualProtect(reinterpret_cast<void*>(table), sizeof(uintptr_t), PAGE_READONLY, &ignored);
        }
        else if (fault == Fault::eBoundaryAfterLast) publication_fixture::boundaryCurrent = false;
        else
        {
            auto* clone = reinterpret_cast<uint8_t*>(active->program.replacements[0]);
            VirtualProtect(clone, 64, PAGE_READWRITE, &ignored);
            clone[48] ^= 1;
            if (fault == Fault::ePayloadAfterLast) VirtualProtect(clone, 64, PAGE_READONLY, &ignored);
        }
    }
    return result;
}
}

int wmain(int argc, wchar_t** argv)
{
    using namespace ampere_gpu;
    using namespace publication_fixture;
    if (argc != 2 && argc != 3) return 2;
    if (argc == 3) faultSlot = static_cast<size_t>(_wcstoui64(argv[2], nullptr, 10));
    try
    {
        {
            Fixture fixture(argv[1]); active = &fixture;
            const auto result = PublishProgram(fixture.program, fixture.payloads, &Protect);
            Check(result.success && result.attempted && ProgramCurrent(fixture.program, 42), "complete registered-slot transaction admitted");
            Check(!ProgramCurrent(fixture.program, 43), "wrong adapter cannot reuse publication");
            fixture.program.boundary = {};
            Check(!ProgramCurrent(fixture.program, 42), "missing first-init proof cannot reuse publication");
        }
        for (unsigned missing = 0; missing != 5; ++missing)
        {
            Fixture fixture(argv[1]); active = &fixture;
            if (missing == 0) fixture.program.boundary.providerGeneration = 0;
            if (missing == 1) fixture.program.boundary.freshLoadToken = 0;
            if (missing == 2) fixture.program.boundary.beforeFirstPipelineCreate = false;
            if (missing == 3) fixture.program.boundary.earlyInitializationProven = false;
            if (missing == 4) fixture.program.boundary.stillCurrent = nullptr;
            const auto result = PublishProgram(fixture.program, fixture.payloads, &Protect);
            Check(!result.success && !result.attempted, "missing positive preparation proof rejects before mutation");
        }
        for (Fault mode : {Fault::eWrite, Fault::eRestoreOnce, Fault::eRestoreForever,
                Fault::eForeignBeforeCas, Fault::eForeignAfterLast, Fault::ePayloadAfterLast,
                Fault::eWritableAfterLast, Fault::eBoundaryAfterLast})
        {
            Fixture fixture(argv[1]); active = &fixture; fault = mode; fired = false;
            const auto result = PublishProgram(fixture.program, fixture.payloads, &Protect);
            printf("TRANSACTION_RESULT fault=%u fired=%u success=%u attempted=%u failedSlot=%zu\n",
                static_cast<unsigned>(mode), static_cast<unsigned>(fired), static_cast<unsigned>(result.success),
                static_cast<unsigned>(result.attempted), result.failedSlot);
            if (mode == Fault::eRestoreOnce && result.success)
            {
                // At slot zero the first COW publication can already leave the
                // image page in its exact original protection despite the
                // injected API failure. Require complete readback in that case.
                Check(fired && result.attempted && ProgramCurrent(fixture.program, 42),
                    "transient protection failure admits only a fully restored complete program");
                continue;
            }
            Check(fired && !result.success && result.attempted, "injected publication fault never admits partial program");
            const bool rollbackExpected = mode == Fault::eWrite || mode == Fault::eRestoreOnce
                || mode == Fault::ePayloadAfterLast || mode == Fault::eWritableAfterLast || mode == Fault::eBoundaryAfterLast;
            Check(result.rollbackComplete == rollbackExpected, "rollback status proves pointer and protection state");
            if (mode == Fault::eForeignBeforeCas || mode == Fault::eForeignAfterLast)
            {
                const size_t slot = mode == Fault::eForeignBeforeCas ? faultSlot : 0;
                uintptr_t current = 0;
                Check(SafeRead(fixture.program.table + slot * sizeof(uintptr_t), current)
                    && current == fixture.program.originals[slot] + 8, "CAS rollback preserves foreign writer");
            }
            printf("TRANSACTION fault=%u failedSlot=%zu rollbackVerified=%u\n",
                static_cast<unsigned>(mode), result.failedSlot, static_cast<unsigned>(result.rollbackComplete));
        }
        fault = Fault::eNone; fired = false;
        {
            Fixture fixture(argv[1]); active = &fixture;
            boundaryCurrent = false;
            const auto result = PublishProgram(fixture.program, fixture.payloads, &Protect);
            Check(!result.success && !result.attempted, "revoked live preparation ticket rejects before first mutation");
        }
        {
            Fixture fixture(argv[1]); active = &fixture;
            fixture.WriteClone(48, 0xee);
            const auto result = PublishProgram(fixture.program, fixture.payloads, &Protect);
            Check(!result.success && !result.attempted, "wrong payload rejected before first pointer is visible");
        }
        {
            Fixture fixture(argv[1]); active = &fixture;
            fixture.WriteClone(48, 0xee, false);
            const auto result = PublishProgram(fixture.program, fixture.payloads, &Protect);
            Check(!result.success && !result.attempted, "writable clone rejected before first pointer is visible");
        }
        active = nullptr;
        printf("AMPERE_PUBLICATION_PASSED fixtureAndBoundaryChecks=%u\n", checks);
        return 0;
    }
    catch (const std::exception& error) { active = nullptr; fprintf(stderr, "FAIL %s\n", error.what()); return 1; }
}
