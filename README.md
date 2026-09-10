# Universal RTXMFG — v1.3.2

DLSS Multi Frame Generation controls for Windows x64 games on RTX 40 series GPUs,
with experimental RTX 30 support. v1.3.2 combines the backend and menu into one
DLL, with fixed multipliers up to 6x and Dynamic MFG where supported.

**RTX 30 series support is very early and experimental. It may not work in some
games or configurations, even when the menu loads.**

The game must already have a Streamline DLSS Frame Generation integration.
Available modes depend on the GPU and game.

If this mod helps you, [help me get through university on Ko-fi](https://ko-fi.com/dashdogy).

## Install

The v1.3.2 download contains **`RTXMFG.dll`**. The menu is built in; no separate
ReShade or external loader installation is needed.

1. Close the game. If upgrading from a split release, remove the old mod
   components as described below first.
2. Download `RTXMFG-v1.3.2.zip` from [Releases](https://github.com/dashdogy/RTX40MFG-Unlock/releases)
   and extract it.
3. Rename `RTXMFG.dll` to **one** supported filename below that the game loads
   early. Place it beside the **actual game executable**, not the launcher.
4. Launch the game, enable DLSS Frame Generation where available, and press
   **Backspace** to open the menu.

Install only one copy. Do not overwrite a DLL belonging to the game or another
mod; choose another suitable name if it is already occupied. Leaving the file
named `RTXMFG.dll` does not make the game load it automatically.

### Supported filenames

```text
version.dll
dinput8.dll
winmm.dll
d3d9.dll
d3d10.dll
d3d11.dll
d3d12.dll
dxgi.dll
dsound.dll
wininet.dll
winhttp.dll
binkw64.dll
bink2w64.dll
xinput1_1.dll
xinput1_2.dll
xinput1_3.dll
xinput1_4.dll
xinput9_1_0.dll
xinputuap.dll
```

The correct name depends on the game. A supported name must actually be loaded
before Frame Generation starts; Vulkan games may not load DirectX proxy names.

**For Bink games:** back up the game's original DLL first, then rename
`binkw64.dll` to `binkw64Hooked.dll`, or `bink2w64.dll` to
`bink2w64Hooked.dll`. Give RTXMFG the original filename and keep both files
beside the executable. These routes require the game's original Bink DLL.

## Upgrade

When upgrading from v1.2, move `RTX40MFGCore.dll`, `RTX40MFG.asi`, and
`RTX40MFG-UI.addon64` to a backup outside the game's loading directories.
Also remove older Universal/Bridge mod components or corresponding RTX 30 split
files, if installed. Preserve unrelated mods and their loaders.

v1.3.2 saves settings in `RTXMFG-Universal.json`. Older RTX40MFG/RTX30MFG settings
are not imported automatically; reselect your preferences in the menu.

For later single-DLL updates, replace only the installed RTXMFG DLL, keeping
its chosen filename and your settings.

## Usage

Press **Backspace** to open or close the menu. The hotkey can be changed there.

- Choose **Follow game**, a fixed multiplier, or **Dynamic** when available.
- Dynamic can target the display refresh rate or a custom FPS value.
- Select **Game/driver**, **Preset A**, or **Preset B** where supported.
  Preset changes after initialization require a restart.
- **Limit FPS with Reflex** provides a separate cap where supported.
  The manual cap is unavailable while Dynamic is selected or active.

Check the status line for pending changes or restart instructions.
Use the game or driver settings for V-Sync.

## Compatibility and troubleshooting

RTX 40 has DirectX 12 and experimental Vulkan paths. RTX 30 support is
**very early, experimental, and DirectX 12 only**. Some games or configurations
may not work at all. Dynamic MFG is unavailable on Vulkan.
Compatibility varies by game; artifacts, freezes, black screens and crashes
remain possible.

If the menu does not appear, check the executable folder, chosen proxy name,
and any leftover split components. Fully close the game before changing files.

For frozen presentation, some users reported that **Preset B** helped in
[issue #6](https://github.com/dashdogy/RTX40MFG-Unlock/issues/6). Restart after
changing presets. This is a reported workaround, not a fix for every game.

For [bug reports](https://github.com/dashdogy/RTX40MFG-Unlock/issues), include
the game, GPU, driver, proxy filename, multiplier and preset, plus the matching
`%TEMP%\RTXMFG-<PID>.log` and `RTXMFG-Universal.<PID>.status.json`.

## Uninstall

Close the game and remove the renamed RTXMFG DLL. Restore any original DLL
you renamed to a `Hooked` filename. Keep `RTXMFG-Universal.json` if you want
to retain your settings.

## Links and licence

The dedicated Cyberpunk 2077 mod is also available on
[Nexus Mods](https://www.nexusmods.com/cyberpunk2077/mods/33286).

Original project code is licensed under the [MIT License](LICENSE).
For source builds, see [BUILD.md](BUILD.md).

Third-party components retain their respective licences and notices.
