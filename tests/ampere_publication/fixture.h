#pragma once
#include <cstdio>
#include <stdexcept>

namespace publication_fixture
{
inline unsigned checks = 0;
#if MFG_PUBLICATION_CANDIDATE
inline bool boundaryCurrent = true;
inline bool CurrentBoundary(HMODULE owner, const ampere_gpu::PreparationBoundary& boundary) noexcept
{ return owner && boundaryCurrent && boundary.Proven(); }
#endif
inline void Check(bool okay, const char* what)
{
    ++checks;
    if (!okay) throw std::runtime_error(what);
}

struct Fixture
{
    ampere_gpu::PublishedProgram program{};
#if MFG_PUBLICATION_COMPLETE
    ampere_native_cache::CompleteProgram payloads;
#else
    ampere_native_cache::Program payloads;
#endif
    explicit Fixture(const wchar_t* path)
    {
        using namespace ampere_gpu;
        // Map a provider as data. Its DllMain and GPU entry points never run.
        program.module = program.pinned = LoadLibraryExW(path, nullptr, DONT_RESOLVE_DLL_REFERENCES);
        uint32_t bytes = 0;
        dlssg_provider_policy::VersionTriplet version{};
        Check(program.module && ImageSize(program.module, bytes)
            && dlssg_provider_policy::ReadProviderVersion(path, version), "provider data mapping");
        const auto* profile = ProfileForVersion(version);
        Check(profile != nullptr, "explicit provider profile");
#if MFG_PUBLICATION_COMPLETE
        DescriptorDiscovery discovery{};
        const uintptr_t anchor = FindDescriptorEntry(program.module, bytes, *profile, &discovery);
        program.slotCount = discovery.registeredSlots;
        payloads.resize(program.slotCount);
#else
        const uintptr_t anchor = FindDescriptorEntry(program.module, bytes, *profile);
#endif
        Check(anchor != 0, "unique exact source descriptor table");
        program.table = anchor - profile->temporalSlot * sizeof(uintptr_t);
        program.luid = 42;
#if MFG_PUBLICATION_CANDIDATE
        boundaryCurrent = true;
        program.boundary = {7, 11, true, true, &CurrentBoundary};
        program.font = std::make_shared<FontProgram>();
        std::vector<uint8_t> fontPtx, fontNative;
        const bool fontPrepared = PrepareFont(program.module, bytes, *program.font, fontPtx, fontNative);
        if (!fontPrepared) printf("FONT_PREPARE_REJECTED stage=%s address=%p pages=%zu\n",
            program.font->stage, reinterpret_cast<void*>(program.font->address), program.font->pageCount);
        Check(fontPrepared,
            "unique owned real auxiliary font prepared");
        std::copy(fontNative.begin(), fontNative.end(), program.font->replacement.begin());
#endif
        for (size_t i = 0; i < payloads.size(); ++i)
        {
            Check(SafeRead(program.table + i * sizeof(uintptr_t), program.originals[i]), "original descriptor snapshot");
            payloads[i].assign(16, static_cast<uint8_t>(i + 1));
            const size_t total = kDescriptorBytes + payloads[i].size();
            auto* allocation = static_cast<uint8_t*>(VirtualAlloc(nullptr, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            Check(allocation != nullptr, "independent clone allocation");
            program.replacements[i] = reinterpret_cast<uintptr_t>(allocation);
            Check(SafeCopy(allocation, reinterpret_cast<const void*>(program.originals[i]), kDescriptorBytes), "original descriptor copied");
            const uintptr_t image = reinterpret_cast<uintptr_t>(allocation + kDescriptorBytes);
            const uint32_t imageBytes = static_cast<uint32_t>(payloads[i].size());
            memcpy(allocation + 8, &image, sizeof(image));
            memcpy(allocation + 16, &imageBytes, sizeof(imageBytes));
            memcpy(allocation + kDescriptorBytes, payloads[i].data(), imageBytes);
#if MFG_PUBLICATION_CANDIDATE
            program.allocationBytes[i] = static_cast<uint32_t>(total);
            memcpy(program.descriptors[i].data(), allocation, kDescriptorBytes);
#endif
            DWORD previous = 0;
            Check(VirtualProtect(allocation, total, PAGE_READONLY, &previous) != FALSE, "clone made immutable");
        }
    }
    ~Fixture()
    {
        using namespace ampere_gpu;
        // No provider code has executed; only this fixture can own readers.
        for (size_t i = 0; i < payloads.size(); ++i)
        {
            if (program.table && program.originals[i])
            {
                const uintptr_t address = program.table + i * sizeof(uintptr_t);
                uintptr_t current = 0;
                if (SafeRead(address, current) && current != program.originals[i])
                    PublishPointer(address, current, program.originals[i]);
                DWORD ignored = 0;
                VirtualProtect(reinterpret_cast<void*>(address), sizeof(uintptr_t), PAGE_READONLY, &ignored);
            }
            if (program.replacements[i]) VirtualFree(reinterpret_cast<void*>(program.replacements[i]), 0, MEM_RELEASE);
        }
        if (program.module) FreeLibrary(program.module);
    }
    void PublishFixture()
    {
        using namespace ampere_gpu;
#if MFG_PUBLICATION_CANDIDATE
        Check(WriteFont(*program.font, true, &VirtualProtect), "fixture font publication");
#endif
        for (size_t i = 0; i < payloads.size(); ++i)
            Check(PublishPointer(program.table + i * sizeof(uintptr_t), program.originals[i], program.replacements[i]).success,
                "fixture publication");
    }
    void WriteClone(size_t offset, uint8_t value, bool restore = true)
    {
        auto* address = reinterpret_cast<uint8_t*>(program.replacements[0]);
        DWORD previous = 0;
        Check(VirtualProtect(address, 64, PAGE_READWRITE, &previous) != FALSE, "fixture clone mutation protection");
        address[offset] = value;
        if (restore) Check(VirtualProtect(address, 64, PAGE_READONLY, &previous) != FALSE, "fixture clone mutation restoration");
    }
};
}
