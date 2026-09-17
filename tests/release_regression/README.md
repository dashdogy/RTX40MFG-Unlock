# Release regression harnesses

Use the pinned Windows x64 tools and SDK inputs in [BUILD.md](../../BUILD.md).
Build the release DLL first, then configure this separate project:

```powershell
cmake -S tests/release_regression -B build/release-tests `
    -G 'Visual Studio 17 2022' -A 'x64,version=10.0.22621.0' -T v143 `
    '-DCMAKE_VS_GLOBALS=VCToolsVersion=14.38.33130' `
    '-DSTREAMLINE_ROOT=C:/SDKs/streamline-sdk-v2.14.1' `
    '-DVULKAN_INCLUDE_DIR=C:/VulkanSDK/1.2.176.1/Include' `
    '-DVULKAN_LIBRARY=C:/VulkanSDK/1.2.176.1/Lib/vulkan-1.lib'
cmake --build build/release-tests --config Release --parallel 6
python tests/release_regression/run.py `
    --dll build/v1.3.3-hotfix.1/Release/RTXMFG.dll `
    --harness-dir build/release-tests/Release `
    --output build/release-test-results `
    --provider C:/Fixtures/nvngx_dlssg.dll
```

Supply an identified, supported DLSS-G provider for `--provider`. Its CPU
count/index gate is exercised using the normal publisher in the exact release
DLL. The runner verifies that the provider file remains unchanged. Without this
argument, that case is skipped and provider gate validation is incomplete.

The runner creates a separate directory per case, copies the exact DLL and
required fixtures, captures exit codes and hashes, and stops on failure.
`--only` accepts case names from `run.py` for focused reruns. It does not launch
games or replace installed files. WARP menu cases briefly use a foreground test
window; Vulkan cases require a compatible NVIDIA Vulkan device. Missing debug
or validation layers are reported in the individual logs.

Coverage includes Dynamic availability for legacy runtimes and range limits,
clean/default and saved controls, renamed loader forwarding
and duplicate ownership, observed wrapper controls, NVIDIA policy limits,
first-launch menu pixels and persistence across relaunch, WARP presentation and
layered swapchains, and Vulkan direct/dynamic/internal-Streamline/mixed routes.
Hotfix coverage also includes unaligned and writable/executable import tables,
HDR interface forwarding, locked status files and process identity, renderer DLL
imports and late loading, 64 KiB/128 KiB resolver threads, and log/status reuse
across launches. The first-launch case reuses its settings directory in a second process to
verify that the automatic menu does not reopen.

These tests establish synthetic rendering, forwarding and CPU gate behavior.
They do not establish live DLSS-G provider Evaluate output or ordered, distinct
generated frames in any game. RTX 30 GPU execution remains separately unverified.
