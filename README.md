# Universal RTX 40 MFG Unlocker

Universal DLSS Multi Frame Generation enabler for supported Windows x64 games
on NVIDIA GeForce RTX 40 Series GPUs. V1.2 adds Follow game, fixed 2X through
the verified maximum, and Dynamic controls to games that already provide
Streamline DLSS Frame Generation.

The same build supports DirectX 12 and Vulkan. It combines NVIDIA's listed
maximum for each game with the active Streamline wrapper capacity, exposing up to 6X
when both allow it and falling back to a supported lower maximum. It does not
add DLSS Frame Generation to games that do not already support it.

This is unsupported research software. Higher multipliers and Vulkan support
remain experimental and may cause artifacts, frozen presentation, black
screens, or crashes.

## V1.2 changes

1. Added one Universal build for DirectX 12 and Vulkan games.
2. Added live fixed and Dynamic controls through the ReShade menu.
3. Added automatic game capability limits, Ada temporal correction, and telemetry.

If this mod helps you, [help me get through university on Ko-fi](https://ko-fi.com/dashdogy).

## Install

Requires Windows x64, an RTX 40 Series GPU, a game with working Streamline DLSS
Frame Generation, [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader),
and [ReShade](https://reshade.me/) with extension support for the menu.

Close the game and copy these files beside its real executable:

```text
RTX40MFGCore.dll
RTX40MFG.asi
RTX40MFG-UI.addon64
```

Install Ultimate ASI Loader under a supported proxy name that the game loads
early. Merge the supplied `global.ini` values into the matching UAL proxy file,
such as `dinput8.ini` or `version.ini`; do not overwrite unrelated loader
settings. Remove legacy
`RTX40MFG-Universal.asi` and `RTX40MFG-Bridge.asi` files before upgrading, but
keep `RTX40MFG-Universal.json` to preserve the selected mode.

Do not replace or bundle a game's Streamline or NVIDIA DLLs. An NVIDIA App OTA
override may supply the active Frame Generation wrapper and provider separately.

### DirectX 12

Install ReShade for DirectX 10/11/12. ReShade normally owns `dxgi.dll`, so give
Ultimate ASI Loader a different supported proxy name that the game imports at
startup, commonly `dinput8.dll` or `version.dll`. Never install both loaders
under the same proxy filename.

### Vulkan

Select Vulkan in the ReShade installer. Use Ultimate ASI Loader under an early
proxy unrelated to DirectX that the game imports, such as `dinput8.dll`,
`version.dll`, or `winmm.dll`; a Vulkan game may never load `dxgi.dll` or
`d3d12.dll`. The mod files are otherwise identical to the DirectX 12 install.

The ASI must load before the first Frame Generation pipeline is created. If the
menu asks for recreation, toggle Frame Generation Off and On or restart with an
earlier loader proxy.

### Frozen image or black screen above 2x

If selecting a multiplier above 2x freezes the image while the game keeps
running, try the following before deleting settings or reinstalling:

1. Close the game.
2. Open the game's profile in the NVIDIA App. Under **DLSS Override - Model
   Presets**, set **Frame Generation** to **Preset B** and apply the change.
3. Restart the game and test the desired multiplier.

Users in [issue #6](https://github.com/dashdogy/RTX40MFG-Unlock/issues/6)
reported that forcing Preset B restored presentation in Crimson Desert and
Star Wars: Zero Company. Cyberpunk was also in the original report, but its
recovery was not separately confirmed. This is a reported workaround; the
underlying cause and correct output at every multiplier remain unverified.
File removal and rebooting had mixed results, so preserve your configuration
when trying the preset change.

### ReShade status flicker

Flickering status in the ReShade panel can persist after Preset B resolves the
frozen image. Treat it as a separate symptom. A user
[suggested using Ultimate ASI Loader's `dinput8.dll` proxy](https://github.com/dashdogy/RTX40MFG-Unlock/issues/6#issuecomment-5545273216)
for the flicker, but the affected recipient has not confirmed that fix.
If testing a different UAL proxy, fully close the game and back up the current
loader setup first; preserve proxy files belonging to other mods.

If either problem persists, include the game and mod version, GPU and driver,
Frame Generation preset, selected multiplier, UAL proxy filename (if used),
and relevant logs in the bug report. State whether the game image freezes,
the ReShade status flickers, or both.

## Usage

Open ReShade and select the **DLSS MFG** tab. Choose Follow game, a fixed
multiplier, or Dynamic when the active stack supports it. Changes are sent
immediately; no Apply button is required. The normal panel shows real and DLSS
output FPS, while detailed route and interval telemetry stays under Debug.

## How it works

`RTX40MFG.asi` imports `RTX40MFGCore.dll`, loading the core before the ASI entry
point. The core intercepts the active Streamline and NGX function paths before
feature creation, validates the adapter, wrapper, provider, and requested
capacity, then publishes the Ada temporal correction and forwards the call.

Only verified code and data inside the process are changed; NVIDIA files on disk are
left untouched. Unknown ownership, layout, timing, or capacity fails closed.
FPS counters and telemetry are diagnostic and do not by themselves prove
correct final presentation frame spacing.

Logs are written to `%TEMP%\MfgUnlock-<PID>.log` and temporal samples to
`%TEMP%\MfgUnlock-intervals-<PID>.csv`.

## Cyberpunk 2077

The dedicated Cyberpunk 2077 mod is still available on [Nexus Mods](https://www.nexusmods.com/cyberpunk2077/mods/33286).

## Build

Requires Visual Studio 2022, CMake 3.24+, Streamline SDK 2.12.0, and compatible
ReShade and ImGui source trees.

```powershell
cmake -S .\source\native -B .\build -G "Visual Studio 17 2022" -A x64 `
  -DSTREAMLINE_ROOT="C:\path\to\streamline" `
  -DMFG_UNLOCK_BUILD_UNIVERSAL_UI=ON `
  -DRESHADE_ROOT="C:\path\to\reshade" `
  -DIMGUI_ROOT="C:\path\to\reshade\deps\imgui"
cmake --build .\build --config Release --parallel --target `
  RTX40MFGCore RTX40MFGAuto RTX40MFGReShadeUI
```

## License

Original code in this repository is licensed under the [MIT License](LICENSE).
Reuse and redistribution are permitted provided the copyright and license
notice are retained. NVIDIA Streamline, NGX, ReShade, Ultimate ASI Loader, and
other external components remain subject to their respective terms. The
bundled MinHook source retains its own license in
the [MinHook license](source/native/third_party/minhook/LICENSE.txt).
