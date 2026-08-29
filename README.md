# RTX 40 MFG Unlock for Cyberpunk 2077

Experimental Cyber Engine Tweaks mod that exposes fixed 2x/3x/4x and Dynamic
DLSS Frame Generation controls on RTX 40-series GPUs.

This is an unsupported research mod. Fixed 3x/4x and Dynamic can cause severe
artifacts, frozen presentation, black screens, extra latency, or crashes. Fixed
2x is NVIDIA's native RTX 40-series path.

## Repository layout

- `bin/` — installable ASI, CET Lua UI, and default configuration
- `source/native/` — native bridge source and CMake project
- `tests/live_control_harness/` — deterministic wrapper/NGX regression harness

## Requirements

- Cyberpunk 2077 with DLSS Frame Generation enabled
- Cyber Engine Tweaks with its ASI loader (`version.dll`)
- RTX 40-series GPU and a current NVIDIA driver

CET 1.37.1 was used during development. Other versions are not guaranteed.

## Install

Extract `bin` into the Cyberpunk 2077 game directory and merge folders. Open
the CET overlay and select Dynamic, 2x, 3x, or 4x. Changes apply live.

The installed files are:

```text
bin/x64/plugins/RTX40MFG.asi
bin/x64/plugins/cyber_engine_tweaks/mods/RTX40MFG/config.json
bin/x64/plugins/cyber_engine_tweaks/mods/RTX40MFG/init.lua
```

## How it works

CET loads the ASI during game startup. The bridge intercepts
`slGetFeatureFunction`, inspects modules actually mapped by Cyberpunk, and
identifies compatible Streamline and NGX images using exports plus exact code
signatures. It patches only their in-process memory; no game or NVIDIA DLL is
changed on disk and no NVIDIA cache path is assumed.

The bridge reports ready only after the active DLSS-G wrapper and a loaded NGX
module have both been verified and patched. It then adjusts Streamline options
and reports actual presentation telemetry to the CET UI.

## Porting the method

The native technique targets Streamline DLSS-G, not Cyberpunk's renderer. CET,
the Lua UI, install paths, and configuration path are Cyberpunk-specific. To
adapt it to another D3D12 game:

1. Load the native DLL early with that game's normal mod-loader or proxy-DLL
   mechanism.
2. Find how the executable obtains `slGetFeatureFunction`. Reuse
   `HookMainExecutableImport`, or replace only that entry hook if the game does
   not import it from `sl.interposer.dll`.
3. Keep `InspectAlreadyLoadedModules` and `RegisterDllNotification`. They find
   mapped candidates by exports rather than filenames or cache locations.
4. Re-derive `kWrapperPattern` and `kNgxPattern` for the exact Streamline/NGX
   versions in use. The included byte signatures are version-specific. Require
   one executable-section match and verify the original bytes before writing.
5. Keep `ObserveActiveWrapperProvider` and the readiness gate. A patched file
   or inactive mapped copy is not evidence that the game uses it.
6. Adapt `ResolveConfigPath` and replace the CET UI as needed. Preserve the
   game's requests to disable Frame Generation and respect the source
   `DLSSGOptions` structure version when copying fields.
7. Validate SetOptions acceptance and `slDLSSGGetState` actual-frame telemetry
   in one uninterrupted process. Test native 2x, live 3x/4x, focus changes,
   result codes 38/39, and an incompatible-signature case before distribution.

The harness shows the minimum expected call flow, deferred NGX loading, active
provider detection, live configuration changes, and actual-count reporting.

## Build

Visual Studio 2022, CMake 3.24+, and Streamline SDK 2.12.0 are required.

```powershell
cmake -S .\source\native -B .\build -G "Visual Studio 17 2022" -A x64 `
  -DSTREAMLINE_ROOT="C:\path\to\streamline-sdk-v2.12.0"
cmake --build .\build --config Release --parallel
```

Copy `build\Release\RTX40MFG.asi` into `bin\x64\plugins`.

## Diagnostics

Runtime logs are written to `%TEMP%\MfgUnlock-<process-id>.log`. Relevant lines
include the complete loaded-module paths, `Active DLSS-G wrapper provider`, and
`Bridge readiness changed`. `bridge_status.json` beside the Lua mod contains
the current request, result codes, and actual multiplier sample.

If the active modules are patched but the image freezes only while the game is
focused, the failure is in the unsupported presentation/pacing path rather than
module discovery. SetOptions result 0 confirms acceptance, not valid output.
