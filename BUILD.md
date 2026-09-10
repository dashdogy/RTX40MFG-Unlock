# Building v1.3.2

The release target is `RTXMFGUnified`, producing `Release/RTXMFG.dll`.
Use Windows x64, Visual Studio 2022/MSVC 14.38.33130, Windows SDK
10.0.22621.0, CMake 4.3.3, and Python 3.10 or newer.

Required external inputs:

- Streamline SDK **2.14.1**, as used by the font-kernel candidate.
- Dear ImGui revision `3912b3d9a9c1b3f17431aebafd86d2f40ee6e59c`
  from ReShade 6.8.0, including its backends.
- Vulkan SDK 1.2.176.1 headers and `glslangValidator.exe`.
- The validated SM86 kernel cache, with this directory layout:

```text
native-cache/ampere_native_manifest.inc
native-cache/manifest.json
native-cache/RTX30MFG-Kernels/*.fatbin
native-cache-3109/ampere_native_manifest.inc
native-cache-3109/manifest.json
native-cache-3109/RTX30MFG-Kernels/*.fatbin
all-provider-layouts/native-cache-complete/ampere_native_manifest_additional.inc
all-provider-layouts/native-cache-complete/additional-resources.json
all-provider-layouts/native-cache-complete/RTX30MFG-Kernels/*.fatbin
```

Kernel caches are separately prepared build inputs and are not included in this
source tree. A source checkout alone cannot reproduce the DLL without them.
The build script pins the tools, SDK headers and manifests by SHA-256; CMake
also verifies every embedded kernel's size and hash. No provider DLL is modified.

With those inputs available, run from the repository root, supplying their
actual paths and a fresh build directory:

```powershell
./source/native/build.ps1 `
    -StreamlineRoot 'C:/SDKs/streamline-sdk-v2.14.1' `
    -ImGuiRoot 'C:/SDKs/reshade-6.8.0/deps/imgui' `
    -NativeCacheRoot 'C:/BuildInputs/rtxmfg-sm86' `
    -BuildDirectory "$PWD/build/v1.3.2"
```

The proxy dispatchers, Vulkan shader arrays and adapted ImGui DX12 backend are
generated in the build directory. Their normalized SHA-256 values must match
`source/native/generated_sha256.json`. The fixed export map is recorded in
`proxy_exports.txt`; generation does not scan DLLs on the build machine.
Third-party notices remain embedded in the release DLL.

The focused font/publication checks are in `tests/ampere_font`. Configure that
project separately with `MFG_PROVIDER_FIXTURE` pointing to the audited 310.9.1
provider and `MFG_FONT_PROVIDER_LIST` pointing to a text list of audited provider
paths, then build Release and run CTest. These checks use data-only provider
images and mock CUDA calls. They do not establish live provider execution or
correct final presentation in a game.
