#include "dlssg_provider_policy.h"
#include "data_image.h"

int wmain(int argc, wchar_t** argv)
{
    using namespace dlssg_provider_policy;
    HMODULE module = nullptr;
    try
    {
        if (argc != 4) throw std::runtime_error("usage: policy fixture.dll dlssg|sibling accept|reject");
        const bool identity = std::wstring(argv[2]) == L"dlssg";
        const bool accepted = std::wstring(argv[3]) == L"accept";
        Require(identity || std::wstring(argv[2]) == L"sibling", "explicit expected identity");
        Require(accepted || std::wstring(argv[3]) == L"reject", "explicit expected admission");
        Require(IsSupportedVersion({310, 9, 0}), "baseline remains admitted");
        Require(IsSupportedVersion({310, 9, 1}) == bool(EXPECT_31091_SUPPORTED), "new triplet matches baseline or hotfix expectation");
        Require(!IsSupportedVersion({310, 9, 2}), "unknown 310.9.2 remains rejected");
        Require(HasDlssgExportIdentity(true, false), "DLSS-G export identity admitted");
        Require(!HasDlssgExportIdentity(false, false) && !HasDlssgExportIdentity(true, true)
            && !HasDlssgExportIdentity(false, true), "missing and sibling export identities rejected");
        VersionTriplet version{};
        Require(ReadProviderVersion(argv[1], version), "actual embedded version read");
        module = LoadLibraryExW(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
        Require(module != nullptr, "map actual fixture without imports or DllMain");
        Require(IsDlssgImplementationModule(module) == identity, "actual fixture export identity result");
        Require(IsSupportedProvider(module, argv[1]) == accepted, "actual provider admission result");
        std::cout << "version=" << version.major << '.' << version.minor << '.' << version.build
            << " admitted=" << accepted << "\nPROVIDER_IDENTITY_AUDIT_PASSED\n";
        FreeLibrary(module);
        return 0;
    }
    catch (const std::exception& error)
    {
        if (module) FreeLibrary(module);
        std::cerr << "PROVIDER_IDENTITY_AUDIT_FAILED: " << error.what() << '\n';
        return 1;
    }
}
