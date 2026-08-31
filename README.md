# RTX 40 MFG Unlock for Cyberpunk 2077

Experimental Cyber Engine Tweaks mod providing fixed 2x through 6x and Dynamic
DLSS Frame Generation controls on RTX 40 series GPUs. The 5x and 6x modes are
especially experimental.

Dynamic defaults to a 4x ceiling. Its UI toggle allows experimental 5x and 6x.
UI recomposition is requested only when matching HUDless and UI buffers are tagged.
The panel reports rendered FPS and total DLSS output FPS.

Version 1.0.3 adds the D157 fixed-capacity and temporal fix, supports the tested
DLSS-G provider versions `310.7.0.*`, `310.7.128.*`, `310.7.129.*`, and
`310.8.0.*`.

This is an unsupported research mod. Modes above 2x may cause artifacts,
latency, frozen presentation, black screens, or crashes.

## Install

Requires Cyberpunk 2077, Cyber Engine Tweaks, an RTX 40 series GPU, and DLSS
Frame Generation enabled. CET 1.37.1 was used during development.

Extract `bin` into the Cyberpunk game directory, merge folders, then select a
mode from the CET overlay. Multiplier changes apply live after the first clean
Frame Generation enable. If the panel says `Re-enable Frame Generation`, toggle
it Off and On (or restart the game). The release ZIP does not include
`config.json`, so installing it preserves the selected mode.

## How it works

The ASI intercepts `slGetFeatureFunction`, watches modules actually loaded by
the game, and identifies Streamline and NGX candidates by exports and exact code
signatures. It patches only mapped process memory, never DLLs on disk, and does
not assume NVIDIA cache paths.

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
