// Compile the actual candidate implementation, without runtime admission bypasses.
#include "midpoint_fix.cpp"
#include "data_image.h"

int wmain(int argc, wchar_t** argv)
{
    using namespace midpoint_fix;
    try
    {
        if (argc != 3) throw std::runtime_error("usage: audit provider.dll output.fatbin");
        Require(ProfileForVersion({310, 9, 0}) == &k3109TemporalProfile, "baseline version binds exact profile");
        Require((ProfileForVersion({310, 9, 1}) == &k3109TemporalProfile) == bool(EXPECT_31091_SUPPORTED), "310.9.1 profile selection matches expected baseline or hotfix");
        Require(ProfileForVersion({310, 9, 2}) == nullptr, "unknown 310.9.2 rejected");
        Require(ProfileForVersion({311, 9, 1}) == nullptr, "wrong major rejected");
        Require(dlssg_provider_policy::IsSupportedVersion({310, 9, 1}) == bool(EXPECT_31091_SUPPORTED), "shared provider eligibility matches expected baseline or hotfix");
        DataImage image(argv[1]);
        const uintptr_t begin = reinterpret_cast<uintptr_t>(image.base);
        const auto module = reinterpret_cast<HMODULE>(image.base);
        uint32_t imageBytes = 0;
        Require(ImageSize(module, imageBytes) && imageBytes == image.bytes, "source PE image bounds");
        dlssg_provider_policy::VersionTriplet version{};
        Require(dlssg_provider_policy::ReadProviderVersion(argv[1], version), "actual fixture version read");
        const auto* selected = ProfileForVersion(version);
        const bool newVersion = version.major == 310 && version.minor == 9 && version.build == 1;
        Require(selected == ((newVersion && !EXPECT_31091_SUPPORTED) ? nullptr : &k3109TemporalProfile), "actual fixture selects expected profile");
        // Baseline audit uses the known contract explicitly to separate a policy
        // rejection from structural or payload incompatibility. Shipping is unchanged.
        const auto& profile = k3109TemporalProfile;
        const uintptr_t entry = FindDescriptorEntry(module, imageBytes, profile);
        Require(entry != 0, "unique complete named 25-slot table discovered");
        auto* table = reinterpret_cast<uintptr_t*>(entry - profile.temporalSlot * sizeof(uintptr_t));
        auto* descriptor = reinterpret_cast<uint8_t*>(table[profile.temporalSlot]);
        const auto* source = reinterpret_cast<const uint8_t*>(ReadU64(descriptor + 8));
        std::cout << std::hex << "table_rva=0x" << reinterpret_cast<uintptr_t>(table) - begin
            << " slot9_entry_rva=0x" << entry - begin
            << " slot9_descriptor_rva=0x" << reinterpret_cast<uintptr_t>(descriptor) - begin
            << " slot9_fatbin_rva=0x" << reinterpret_cast<uintptr_t>(source) - begin << std::dec << '\n';
        Require(Sha256Equals(source, static_cast<size_t>(profile.sourceFatbinBytes), profile.sourceFatbinSha256), "exact source fatbin SHA256");
        std::vector<uint8_t> fatbin(kOutputCapacity);
        std::vector<uint8_t> scratch(kScratchCapacity);
        std::memcpy(fatbin.data(), source, static_cast<size_t>(profile.sourceFatbinBytes));
        uint32_t outputBytes = 0;
        Failure failure{};
        Require(BuildTemporalFatbin(fatbin.data(), scratch.data(), profile, outputBytes, failure), "unchanged production temporal transformation");
        Require(Sha256Equals(fatbin.data(), outputBytes, profile.outputFatbinSha256), "exact expected output fatbin SHA256");
        Require(outputBytes > profile.sourceFatbinBytes && outputBytes <= kOutputCapacity
            && outputBytes % 8 == 0, "output layout fits the aligned bounded allocation");
        std::cout << "output_bytes=" << outputBytes << '\n';
        std::ofstream output(std::filesystem::path(argv[2]), std::ios::binary);
        output.write(reinterpret_cast<const char*>(fatbin.data()), outputBytes);
        Require(output.good(), "transformed artifact written");
        output.close();

        const uintptr_t originalEntry = table[9];
        std::swap(table[8], table[9]);
        Require(FindDescriptorEntry(module, imageBytes, profile) == 0, "swapped kernel slots rejected");
        std::swap(table[8], table[9]);
        auto* name = reinterpret_cast<char*>(ReadU64(descriptor));
        const char originalName = name[0];
        name[0] = '!';
        Require(FindDescriptorEntry(module, imageBytes, profile) == 0, "wrong midpoint descriptor name rejected");
        name[0] = originalName;
        auto* entryName = reinterpret_cast<char*>(ReadU64(descriptor + 24));
        const char originalKernel = entryName[0];
        entryName[0] = '!';
        Require(FindDescriptorEntry(module, imageBytes, profile) == 0, "wrong CUDA entry name rejected");
        entryName[0] = originalKernel;
        table[9] = begin + imageBytes;
        Require(FindDescriptorEntry(module, imageBytes, profile) == 0, "out-of-image descriptor rejected");
        table[9] = originalEntry;
        auto* sourceMutable = const_cast<uint8_t*>(source);
        sourceMutable[200] ^= 1;
        Require(FindDescriptorEntry(module, imageBytes, profile) == 0, "mutated source fatbin rejected by discovery");
        sourceMutable[200] ^= 1;
        Require(FindDescriptorEntry(module, imageBytes, profile) == entry, "original mapping restored");
        auto wrong = profile;
        wrong.sourcePtxSha256 = "0000000000000000000000000000000000000000000000000000000000000000";
        std::memcpy(fatbin.data(), source, static_cast<size_t>(profile.sourceFatbinBytes));
        Require(!BuildTemporalFatbin(fatbin.data(), scratch.data(), wrong, outputBytes, failure)
            && failure == Failure::eSourceIdentity, "wrong decompressed PTX identity rejected");
        std::memcpy(fatbin.data(), source, static_cast<size_t>(profile.sourceFatbinBytes));
        fatbin[16 + 28] = 119;
        Require(!BuildTemporalFatbin(fatbin.data(), scratch.data(), profile, outputBytes, failure), "wrong architecture layout rejected");

        auto* sections = IMAGE_FIRST_SECTION(image.nt);
        const auto count = image.nt->FileHeader.NumberOfSections;
        auto* extra = sections + count;
        Require(reinterpret_cast<uint8_t*>(extra + 1) <= image.base + image.nt->OptionalHeader.SizeOfHeaders, "duplicate fixture section header capacity");
        std::memset(extra, 0, sizeof(*extra));
        std::memcpy(extra->Name, ".audit", 6);
        extra->VirtualAddress = imageBytes;
        extra->Misc.VirtualSize = 4096;
        extra->Characteristics = IMAGE_SCN_MEM_READ | IMAGE_SCN_CNT_INITIALIZED_DATA;
        std::memcpy(image.base + imageBytes, table, 25 * sizeof(uintptr_t));
        image.nt->FileHeader.NumberOfSections++;
        image.nt->OptionalHeader.SizeOfImage += 4096;
        Require(FindDescriptorEntry(module, imageBytes + 4096, profile) == 0, "ambiguous duplicate registration table rejected");
        std::cout << "ADA_PROVIDER_AUDIT_PASSED\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "AUDIT_FAILED: " << error.what() << '\n';
        return 1;
    }
}
