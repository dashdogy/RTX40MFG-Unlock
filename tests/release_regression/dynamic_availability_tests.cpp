#include "ui_dynamic_mfg.h"
#include <cstdio>
#include <cstring>
using namespace ui_dynamic_mfg;

int main()
{
    unsigned checks=0, failures=0;
    auto check=[&](const MfgUnlockReShadeSnapshot& state, Availability expected, const char* reason) {
        ++checks;
        const bool passed=Classify(state)==expected;
        std::printf("%s %s\n",passed?"PASS":"FAIL",reason);
        if(!passed) ++failures;
    };
    MfgUnlockReShadeSnapshot state{};
    state.gpuFamily=1; state.safeMaximumMultiplier=4; state.numFramesToGenerateMax=3;
    state.activeWrapperObserved=TRUE; state.activeWrapperVersionMajor=2;
    state.activeWrapperVersionMinor=8; state.bridgeReady=TRUE;
    check(state,Availability::Unavailable,"Avatar 2.8/4x does not wait for an absent Dynamic field");
    state.bridgeReady=FALSE;
    check(state,Availability::Unavailable,"legacy runtime remains unavailable during bridge transitions");
    state.activeWrapperVersionMinor=10;
    check(state,Availability::Unavailable,"observed Streamline 2.10 predates Dynamic mode");
    state.activeWrapperObserved=FALSE;
    check(state,Availability::Checking,"unobserved module version cannot veto active runtime");
    state.activeWrapperObserved=TRUE; state.activeWrapperVersionMajor=0;
    check(state,Availability::Checking,"missing wrapper version remains unresolved");
    state.activeWrapperVersionMajor=2; state.activeWrapperVersionMinor=14;
    check(state,Availability::Checking,"newer 4x runtime still needs a capability answer");
    state.dynamicMfgSupportKnown=TRUE;
    check(state,Availability::Unavailable,"explicit unsupported answer survives an unavailable bridge");
    state.bridgeReady=TRUE; state.dynamicMfgSupported=TRUE;
    check(state,Availability::Supported,"reported Dynamic range that fits 4x remains supported");
    state.numFramesToGenerateMax=5;
    check(state,Availability::Unavailable,"6x Dynamic range cannot fit a 4x cap");
    state.dynamicMfgSupportKnown=FALSE;
    check(state,Availability::Unavailable,"known range restriction wins before capability discovery");
    state.safeMaximumMultiplier=6;
    check(state,Availability::Checking,"a lifted cap still requires positive Dynamic discovery");
    state.dynamicMfgSupportKnown=TRUE;
    check(state,Availability::Supported,"supported 6x Dynamic remains available");
    state.numFramesToGenerateMax=0;
    check(state,Availability::Checking,"missing range cannot enable Dynamic");
    state.numFramesToGenerateMax=0xffffffffu;
    check(state,Availability::Unavailable,"invalid range cannot overflow into availability");
    state.gpuFamily=2; state.numFramesToGenerateMax=3; state.safeMaximumMultiplier=4;
    check(state,Availability::Unavailable,"Ampere retains its full-capacity Dynamic restriction");
    state.numFramesToGenerateMax=5; state.safeMaximumMultiplier=6;
    check(state,Availability::Supported,"Ampere supported full-capacity Dynamic is retained");
    const bool labels=std::strcmp(Label(Availability::Unavailable),"Dynamic (unavailable)")==0
        && std::strcmp(Label(Availability::Checking),"Dynamic (checking...)")==0
        && std::strcmp(Label(Availability::Supported),"Dynamic")==0;
    ++checks; if(!labels) ++failures;
    std::printf("%s shared menu labels match all three availability states\n",labels?"PASS":"FAIL");
    std::printf("COMPLETE Dynamic availability checks=%u failures=%u\n",checks,failures);
    return failures?1:0;
}
