# RTX 40 MFG Unlock for Cyberpunk 2077

Experimental Cyber Engine Tweaks mod providing fixed 2x through 6x and Dynamic
DLSS Frame Generation controls on RTX 40 series GPUs. The 5x and 6x modes are
especially experimental.

Dynamic defaults to a 4x ceiling. Its UI toggle allows experimental 5x and 6x.
UI recomposition is requested only when matching HUDless and UI buffers are tagged.
The panel reports rendered FPS and total DLSS output FPS.

Version 1.1 restores the preserved D157 runtime and separates DLSS-G feature
identity from version eligibility. A loaded module must expose the DLSS-G-specific
`NVSDK_NGX_D3D12_PopulateDeviceParameters_Impl` export before its version is
considered. The tested provider versions are `310.7.0.*`, `310.7.128.*`,
`310.7.129.*`, and `310.8.0.*`; other `310.7.x` builds are not accepted
implicitly.

Streamline 2.12 and 2.13 module layouts are recognized independently of the
DLSS-G provider version. Updating `nvngx_dlssg.dll` does not require matching
versions of `nvngx_dlss.dll`, `nvngx_dlssd.dll`, `nvngx_dlssnr.dll`,
`nvngx_deepdvc.dll`, or the other Streamline DLLs.

This is an unsupported research mod. Modes above 2x may cause artifacts,
latency, frozen presentation, black screens, or crashes.

## Install

Requires Cyberpunk 2077, Cyber Engine Tweaks, an RTX 40 series GPU, and DLSS
Frame Generation enabled. CET 1.37.1 was used during development.

Extract `bin` into the Cyberpunk game directory, merge folders, then select a
mode from the CET overlay. Select the multiplier before launch when possible.
If Frame Generation is already active, toggle it Off and On (or restart the
game) so Streamline rebuilds the feature with the requested shape. The release
ZIP does not include `config.json`, so installing it preserves the selected
mode.

The v1.1 provider matrix covered 17 DLSS-G binaries: the five binaries in the
four supported version triplets passed and all twelve others failed closed.
Each passing provider was also checked beside renamed DLSS, DLSSD, DLSSNR, and
DeepDVC siblings; only DLSS-G was admitted. These compatibility checks and FPS
counters do not by themselves prove final-present image quality.

## How it works

The ASI intercepts `slGetFeatureFunction`, watches modules actually loaded by
the game, and identifies Streamline and NGX candidates by exports and exact code
signatures. DLSS-G feature identity is established before the supported-version
gate, preventing same-version DLSS-family siblings from being scanned as the
Frame Generation provider. It patches only mapped process memory, never DLLs on
disk, and does not assume NVIDIA cache paths.

The D157 (v1.0) fix targets Ada’s midpoint compaction bug: at higher multipliers, generated samples collapse toward the middle of the frame interval instead of occupying their requested temporal positions, producing near duplicate frames. It backports the corrected slot-9 temporal program used by Blackwell in process memory so each generated sample is evaluated at its own evenly spaced position between rendered frames. If the active adapter, provider version, or layout cannot be verified, the patch fails closed to native 2x.

The bridge becomes ready only after the active DLSS G wrapper and loaded NGX
module are verified and patched. It then adjusts `slDLSSGSetOptions` and reads
actual presentation counts through `slDLSSGGetState`.

The approach targets Streamline DLSS G rather than Cyberpunk's renderer. In
another Streamline game, adapt the early DLL loading integration, UI and config
paths, and game/provider specific signatures.

## Build

Requires Visual Studio 2022, CMake 3.24+, and Streamline SDK 2.12.0.

```powershell
cmake -S .\source\native -B .\build -G "Visual Studio 17 2022" -A x64 `
  -DSTREAMLINE_ROOT="C:\path\to\streamline-sdk-v2.12.0"
cmake --build .\build --config Release --parallel
```

The native build writes `build\Release\RTX40MFG.asi`. The CET UI and its
FPS/status client are tracked at
`bin\x64\plugins\cyber_engine_tweaks\mods\RTX40MFG\init.lua`. Breakpoint and
deep-kernel research diagnostics are disabled in the normal build.

Logs are written to the temporary directory and include the process ID.
