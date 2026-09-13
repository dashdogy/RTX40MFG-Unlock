#pragma once
#include "build_variant.h"

#include <Windows.h>

#include <cstdint>

struct MfgUnlockReShadeSnapshot
{
    uint32_t structSize = sizeof(MfgUnlockReShadeSnapshot);
    uint32_t startupProfile = 0;
    uint32_t startupFailure = 0;
    uint32_t startupValidationMask = 0;
    BOOL centralHookInstalled = FALSE;
    BOOL startupSnapshotClean = FALSE;
    BOOL workerIdentityCertified = FALSE;
    BOOL bridgeReady = FALSE;
    BOOL gameFrameGenerationOn = FALSE;
    BOOL appliedFrameGenerationOn = FALSE;
    BOOL streamlineRebuildRequired = FALSE;
    BOOL dynamicMode = FALSE;
    BOOL appliedDynamicMode = FALSE;
    BOOL dynamicExperimental56 = FALSE;
    BOOL generatedOnlyDebug = FALSE;
    BOOL perSampleSynthesisReady = FALSE;
    BOOL highCapabilityPublicationAllowed = FALSE;
    uint32_t desiredMultiplier = 2;
    uint32_t appliedMultiplier = 0;
    uint32_t dynamicTargetFrameRate = 0;
    uint32_t actualFramesPresented = 0;
    uint32_t numFramesToGenerateMax = 0;
    int32_t lastSetOptionsResult = 0;
    int32_t lastGetStateResult = 0;
    uint64_t desiredRevision = 0;
    uint64_t appliedRevision = 0;
    char patchRoute[16]{};
    uint32_t backendAbiVersion = 0;
    BOOL transportEarlyHostCertified = FALSE;
    BOOL dllNotificationRegistered = FALSE;
    BOOL pluginLoaderHooksInstalled = FALSE;
    BOOL preEntryRouteInstalled = FALSE;
    BOOL selectedOwnerPublished = FALSE;
    uint64_t preEntryRouteAttempts = 0;
    uint64_t preEntryRouteSuccesses = 0;
    uint64_t preEntryRouteFailures = 0;
    uint32_t frontendClients = 0;
    BOOL followGameMode = TRUE;
    uint32_t statusProtocolVersion = 0;
    BOOL loaderCoreImported = FALSE;
    BOOL setOptionsEntryDetourCurrent = FALSE;
    BOOL setOptionsResolverFallbackActive = FALSE;
    uint64_t setOptionsResolverFallbackCalls = 0;
    BOOL setOptionsControlPathReady = FALSE;
    BOOL getStateEntryDetourCurrent = FALSE;
    BOOL ngxCreateEntryDetourCurrent = FALSE;
    BOOL ngxEvaluateEntryDetourCurrent = FALSE;
    BOOL backportReadyAtCreate = FALSE;
    BOOL pipelineMayPredateDetour = FALSE;
    BOOL setOptionsSeen = FALSE;
    BOOL setOptionsAccepted = FALSE;
    BOOL getStateSeen = FALSE;
    BOOL intervalLoggingEnabled = FALSE;
    BOOL intervalLogReady = FALSE;
    uint64_t intervalValidSamples = 0;
    uint64_t intervalInvalidSamples = 0;
    uint64_t intervalDroppedSamples = 0;
    int32_t intervalLastCount = 0;
    int32_t intervalLastIndex = 0;
    uint32_t intervalLastPositionNumerator = 0;
    uint32_t intervalLastPositionDenominator = 0;
    uint32_t realFpsMilli = 0;
    uint32_t dlssFpsMilli = 0;
    uint64_t fpsSampleAgeMs = 0;
    BOOL uiInputsReady = FALSE;
    BOOL uiRecompositionEnabled = FALSE;
    BOOL uiRecompositionForced = FALSE;
    BOOL appliedGeneratedOnlyDebug = FALSE;
    char intervalLogFile[64]{};
    BOOL mainResolverDiscoveryInstalled = FALSE;
    BOOL slInitEntryDetourCurrent = FALSE;
    BOOL slInitIatFallbackInstalled = FALSE;
    BOOL slInitResolverFallbackActive = FALSE;
    BOOL slInitControlPathReady = FALSE;
    uint64_t slInitCalls = 0;
    uint64_t slInitFlagsBefore = 0;
    uint64_t slInitFlagsAfter = 0;
    BOOL otaPreferencesForced = FALSE;
    BOOL otaPreferencesEnabledAtInit = FALSE;
    BOOL downloadedStreamlinePluginsEnabledAtInit = FALSE;
    BOOL otaProviderPreflightSupported = FALSE;
    BOOL otaForceSuppressed = FALSE;
    BOOL streamlineLoaderDiscoveryInstalled = FALSE;
    uint64_t streamlineLoaderDiscoveryCalls = 0;
    BOOL activeWrapperObserved = FALSE;
    BOOL activeWrapperUsesNvidiaOta = FALSE;
    uint32_t activeWrapperVersionMajor = 0;
    uint32_t activeWrapperVersionMinor = 0;
    uint32_t activeWrapperVersionBuild = 0;
    uint32_t activeWrapperVersionPrivate = 0;
    uint32_t wrapperCompiledMaximumGeneratedFrames = 0;
    uint32_t safeMaximumMultiplier = 2;
    BOOL requestedMultiplierLimited = FALSE;
    BOOL selectiveOtaDlssgWrapperRequested = FALSE;
    BOOL selectiveOtaDlssgWrapperCandidateReady = FALSE;
    uint32_t selectiveOtaDlssgWrapperFailure = 0;
    uint64_t selectiveOtaDlssgWrapperRedirectAttempts = 0;
    uint64_t selectiveOtaDlssgWrapperRedirectSuccesses = 0;
    uint64_t selectiveOtaDlssgWrapperFallbacks = 0;
    uint32_t selectiveOtaDlssgWrapperVersionMajor = 0;
    uint32_t selectiveOtaDlssgWrapperVersionMinor = 0;
    uint32_t selectiveOtaDlssgWrapperVersionBuild = 0;
    uint32_t selectiveOtaDlssgWrapperVersionPrivate = 0;
    BOOL nvidiaCompatibilityResolved = FALSE;
    int32_t nvidiaProfileStatus = -1;
    char nvidiaProfileName[128]{};
    // Protocol 40: 0 unknown, 2 native FG listed without MFG override, 4/6 listed MFG maximum.
    uint32_t nvidiaCompatibilityTier = 0;
    uint32_t nvidiaCompatibilityManifestEntries = 0;
    // Tier 2 uses the mod's conservative 2x bound; NVIDIA's MFG cell is blank.
    uint32_t nvidiaPolicyCeilingMultiplier = 0;
    uint32_t wrapperNativeMaximumMultiplier = 2;
    BOOL compatibilityFallback = FALSE;
    uint32_t compatibilityReason = 0;
    BOOL fullStreamlineOtaRequested = FALSE;
    BOOL fullStreamlineOtaEligible = FALSE;
    BOOL downloadedStreamlinePluginsForced = FALSE;
    uint32_t streamlineHostVersionMajor = 0;
    uint32_t streamlineHostVersionMinor = 0;
    uint32_t streamlineHostVersionBuild = 0;
    uint32_t streamlineHostVersionPrivate = 0;
    BOOL dynamicMfgSupportKnown = FALSE;
    BOOL dynamicMfgSupported = FALSE;
    char activeWrapperPath[512]{};
    uint64_t activeWrapperGeneration = 0;
    char activeControlPath[16]{};
    char activeStatePath[16]{};
    char activeControlDetour[16]{};
    char activeStateDetour[16]{};
    char activeProviderPath[512]{};
    uint32_t activeProviderVersionMajor = 0;
    uint32_t activeProviderVersionMinor = 0;
    uint32_t activeProviderVersionBuild = 0;
    uint32_t activeProviderVersionPrivate = 0;
    uint64_t activeProviderGeneration = 0;
    char providerSelectionSource[24]{};
    char providerCreateDetour[16]{};
    char providerEvaluateDetour[16]{};
    BOOL midpointReadyAtFirstCreate = FALSE;
    uint64_t activeLastCallRevision = 0;
    uint64_t activeLastAcceptedRevision = 0;
    uint32_t universalRouteFailure = 0;
    char universalRouteFailureReason[64]{};
    BOOL releaseEntryCurrent = FALSE;
    BOOL frameGenerationOffAccepted = FALSE;
    BOOL releaseObserved = FALSE;
    uint32_t dlssgPresetRequested = 2;
    BOOL dlssgPresetOverrideInstalled = FALSE;
    uint32_t dlssgPresetOverrideFailure = 0;
    uint64_t dlssgPresetReadCount = 0;
    BOOL ampereProgramReadyMfg = FALSE;
    uint32_t ampereKernelImage = 0;
    uint32_t ampereNativeCacheStatus = 0;
    uint32_t ampereCertifiedMaximum = 0;
    uint32_t ampereFailure = 0;
    uint64_t presetQueries = 0;

    uint32_t ampereFirstFailure = 0;
    uint32_t ampereFirstNgxResult = 0;
    uint32_t amperePrimaryFailure = 0;
    uint32_t amperePrimaryNgxResult = 0;
    uint32_t ampereLastFailure = 0;
    uint32_t ampereLastNgxResult = 0;
    uint64_t ampereCreateAttempts = 0;
    uint64_t ampereCreateBlockedBeforeProvider = 0;
    uint64_t ampereEvaluateAttempts = 0;
    uint32_t amperePreparationStage = 0;
    uint32_t ampereStartupFailureMask = 0;
    uint32_t ampereCandidateVersionMajor = 0;
    uint32_t ampereCandidateVersionMinor = 0;
    uint32_t ampereCandidateVersionBuild = 0;
    uint64_t ampereCreatedFeatures = 0;
    uint64_t ampereEvaluations = 0;
    uint32_t gpuFamily = 0;
    uint64_t gpuAdapterLuid = 0;
    uint32_t gpuSelectionFailure = 0;

    uint32_t dlssgPresetLatched = 2;
    BOOL dlssgPresetSelectionFrozen = FALSE;
    BOOL dlssgPresetRestartRequired = FALSE;
    uint32_t dlssgPresetObserved = 0;
    BOOL dlssgPresetObservedValid = FALSE;
    uint64_t dlssgPresetOverrideReadCount = 0;
    float appliedDynamicTargetFrameRate = 0.0f;
    BOOL appliedDynamicTargetValid = FALSE;
    BOOL fgVsyncSupportKnown = FALSE;
    BOOL fgVsyncSupported = FALSE;

    uint32_t vsyncMode = 0; // 0 game, 1 off, 2 on; application request only.
    uint32_t reflexFrameLimitFps = 0; // 0 preserves the game's limiter.
    BOOL dynamicVsyncAvailable = FALSE;
    BOOL vsyncControlAvailable = FALSE;
    BOOL vsyncOverrideApplied = FALSE;
    BOOL vsyncPresentationObserved = FALSE;
    uint32_t vsyncOriginalInterval = 0;
    uint32_t vsyncSubmittedInterval = 0;
    uint32_t vsyncFailure = 0;
    BOOL reflexControlAvailable = FALSE;
    BOOL reflexAppliedKnown = FALSE;
    uint32_t reflexAppliedFrameLimitUs = 0;
    BOOL reflexLimitPending = FALSE;
    BOOL reflexRestorePending = FALSE;
    uint32_t reflexStatus = 0;
    int32_t reflexLastResult = 0;
    uint32_t reflexHookMask = 0;
    uint32_t reflexModuleVersionMajor = 0;
    uint32_t reflexModuleVersionMinor = 0;
    uint32_t reflexModuleVersionPatch = 0;
    uint64_t reflexModuleGeneration = 0;

    BOOL ampereLegacySinglePreset = FALSE;

};

#if defined(MFG_UNLOCK_RESHADE_ADDON) || defined(MFG_UNLOCK_BACKEND_BRIDGE)
extern "C" __declspec(dllexport) BOOL WINAPI
MfgUnlockReShadeGetSnapshot(MfgUnlockReShadeSnapshot* snapshot);

extern "C" __declspec(dllexport) BOOL WINAPI
MfgUnlockReShadeApplyControl(uint32_t multiplier, BOOL dynamicMode,
    uint32_t dynamicTargetFrameRate, BOOL dynamicExperimental56,
    BOOL generatedOnlyDebug);
#endif
