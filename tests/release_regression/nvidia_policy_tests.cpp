#include "nvidia_mfg_policy.h"
#include <cstdio>
#include <cwchar>
using namespace nvidia_mfg_policy;
static unsigned checks = 0, failures = 0;
static void Check(bool ok, const char* message) {
    ++checks; if (!ok) { ++failures; std::printf("FAIL %s\n", message); }
}
int wmain(int argc, wchar_t** argv) {
    Check(FindTier(L"Marvel's Spider-Man Remastered") == Tier::eNoMfgOverride, "Remastered is FG-only in NVIDIA table");
    Check(FindTier(L"Marvel's Spider-Man: Miles Morales") == Tier::eNoMfgOverride, "Miles Morales is FG-only");
    Check(FindTier(L"Marvel's Spider-Man 2") == Tier::eFourX, "Spider-Man 2 has 4x ceiling");
    Check(FindTier(L"Cyberpunk 2077") == Tier::eSixX, "Cyberpunk has 6x ceiling");
    Check(FindTier(L"Definitely Unknown Title 6725") == Tier::eUnknown, "unmatched title stays unknown");
    Check(FindTier(L"Marvel's Spider-Man") == Tier::eUnknown, "partial title cannot borrow sequel policy");
    Check(FindTier(L"Alan Wake Remastered") == Tier::eUnknown, "SR-only row does not imply FG or MFG");
    Check(FindTier(L"MARVEL'S SPIDER-MAN 2") == Tier::eFourX, "exact normalized title matches");
    for (Tier tier : {Tier::eUnknown, Tier::eNoMfgOverride, Tier::eFourX, Tier::eSixX}) {
        for (bool patched : {false, true}) {
            for (unsigned compiled : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 0xffffffffu}) {
                const auto result = DecideCapacity(tier, patched, compiled);
                Check(result.effectiveMaximumMultiplier <= NativeMaximumMultiplier(compiled), "policy cannot expand compiled bound");
                Check(result.nvidiaCeilingMultiplier == 0 || result.effectiveMaximumMultiplier <= result.nvidiaCeilingMultiplier, "override never exceeds per-game ceiling");
                Check(patched || result.effectiveMaximumMultiplier == 2, "unpatched route cannot advertise higher modes");
                Check(result.fallback == (result.nvidiaCeilingMultiplier > result.effectiveMaximumMultiplier), "fallback reports runtime restriction separately");
                Check(LimitToPolicy(tier, result.effectiveMaximumMultiplier) == result.effectiveMaximumMultiplier, "shared backend bound is idempotent");
            }
        }
    }
    Check(DecideCapacity(Tier::eFourX, true, 1).effectiveMaximumMultiplier == 2, "patched 2x clamp cannot manufacture 4x capacity");
    Check(DecideCapacity(Tier::eSixX, true, 3).effectiveMaximumMultiplier == 4, "6x title with 4x wrapper retains 4x runtime bound");
    Check(DecideCapacity(Tier::eFourX, true, 5).effectiveMaximumMultiplier == 4, "6x runtime respects 4x title policy");
    Check(DecideCapacity(Tier::eSixX, true, 5).effectiveMaximumMultiplier == 6, "6x remains available to capable listed title");
    Check(DecideCapacity(Tier::eUnknown, true, 5).effectiveMaximumMultiplier == 6, "unknown native MFG integration retains native bound");
    Check(LimitToPolicy(Tier::eNoMfgOverride, 6) == 2, "native-FG-only title does not acquire override through alternate backend");
    Check(!DynamicRangeFits(5, 4), "6x Dynamic runtime cannot bypass 4x policy");
    Check(!DynamicRangeFits(3, 2), "4x Dynamic runtime cannot bypass FG-only fallback");
    Check(!DynamicRangeFits(0, 6), "unknown Dynamic range stays unavailable");
    Check(!DynamicRangeFits(0xffffffffu, 6), "invalid Dynamic maximum cannot overflow into eligibility");
    Check(DynamicRangeFits(3, 4), "4x Dynamic range fits 4x limit");
    Check(DynamicRangeFits(5, 6), "6x Dynamic range fits 6x limit");
    Check(!DynamicRangeFits(5, 2), "temporarily unavailable runtime cannot enable saved Dynamic request");
    // Optional live read-only identity check; never binds SetSetting/SaveSettings.
    if (argc == 2) {
        const auto query = IdentifyExecutable(argv[1]);
        Check(query.getProfileStatus == 0 && query.tier == Tier::eNoMfgOverride, "installed Spider-Man executable resolves to FG-only policy");
        std::wprintf(L"PROFILE %ls tier=%u status=%d\n", query.profileName.c_str(), unsigned(query.tier), query.getProfileStatus);
    }
    std::printf("COMPLETE NVIDIA policy %u checks %u failures manifest=%u\n", checks, failures, ManifestEntryCount());
    return failures ? 1 : 0;
}
