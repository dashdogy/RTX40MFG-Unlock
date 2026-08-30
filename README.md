# RTX 40 MFG Unlock for Cyberpunk 2077

Experimental Cyber Engine Tweaks mod providing fixed 2x through 6x and Dynamic
DLSS Frame Generation controls on RTX 40 series GPUs. The 5x and 6x modes are
especially experimental.

Dynamic defaults to a 4x ceiling. Its UI toggle allows experimental 5x and 6x.
UI recomposition is requested only when matching HUDless and UI buffers are tagged.
The panel reports rendered FPS and total DLSS output FPS.

This is an unsupported research mod. Modes above 2x may cause artifacts,
latency, frozen presentation, black screens, or crashes.

## Install

Requires Cyberpunk 2077, Cyber Engine Tweaks, an RTX 40 series GPU, and DLSS
Frame Generation enabled. CET 1.37.1 was used during development.

Extract `bin` into the Cyberpunk game directory, merge folders, then select a
mode from the CET overlay. Select the multiplier before launch when possible.
If Frame Generation is already active, change it Off and back On in the game
settings (or restart the game) so Streamline rebuilds its presentation swapchain
and generated-frame pool with the new shape.

## How it works

The ASI intercepts `slGetFeatureFunction`, watches modules actually loaded by
the game, and identifies Streamline and NGX candidates by exports and exact code
signatures. It patches only mapped process memory, never DLLs on disk, and does
not assume NVIDIA cache paths.

The bridge becomes ready only after the active DLSS G wrapper and loaded NGX
module are verified and patched. It then adjusts `slDLSSGSetOptions` and reads
actual presentation counts through `slDLSSGGetState`.
Multiplier-shape changes are submitted only on a clean DLSS-G enable. A live
change is reported as pending rather than recycling only the NGX feature inside
an already-created Streamline presentation swapchain.
For higher modes, it keeps the Ada backend in its supported single generated
frame configuration while the wrapper evaluates each requested temporal index.
An unknown backend safely falls back to 2x instead of presenting duplicate frames.
The generated frames only toggle isolates interpolated output for capture tests.

The approach targets Streamline DLSS G rather than Cyberpunk's renderer. In
another Streamline game, only the early DLL loading integration, UI, paths, and
version specific signatures would need to be adapted. The included harness
demonstrates the expected call flow and deferred NGX loading.

## Build

Requires Visual Studio 2022, CMake 3.24+, and Streamline SDK 2.12.0.

```powershell
cmake -S .\source\native -B .\build -G "Visual Studio 17 2022" -A x64 `
  -DSTREAMLINE_ROOT="C:\path\to\streamline-sdk-v2.12.0"
cmake --build .\build --config Release --parallel
```

Logs are written to the temporary directory and include the process ID.
