param(
    [Parameter(Mandatory = $true)][string]$NativeCacheRoot,
    [Parameter(Mandatory = $true)][string]$StreamlineRoot,
    [Parameter(Mandatory = $true)][string]$ImGuiRoot,
    [string]$VulkanIncludeDirectory = 'C:/VulkanSDK/1.2.176.1/Include',
    [string]$CMakeExecutable = 'C:/Program Files/CMake/bin/cmake.exe',
    [string]$VisualStudioInstance = 'C:/Program Files/Microsoft Visual Studio/2022/Professional',
    [string]$MsvcToolsVersion = '14.38.33130',
    [string]$WindowsSdkVersion = '10.0.22621.0',
    [string]$BuildDirectory = (Join-Path $PSScriptRoot '../../build/native')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-InputHash {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Expected,
        [switch]$NormalizeText
    )
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing pinned build input: $Path"
    }
    if ($NormalizeText) {
        $bytes = [Text.Encoding]::UTF8.GetBytes([IO.File]::ReadAllText($Path).Replace("`r`n", "`n"))
        $sha = [Security.Cryptography.SHA256]::Create()
        try { $actual = [BitConverter]::ToString($sha.ComputeHash($bytes)).Replace('-', '') }
        finally { $sha.Dispose() }
    } else {
        $actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    }
    if ($actual -ne $Expected) {
        throw "Pinned build input hash mismatch: $Path expected=$Expected actual=$actual"
    }
}

$sourceDirectory = $PSScriptRoot
$frozenCandidate = $NativeCacheRoot
$nativeCache = Join-Path $frozenCandidate 'native-cache'
$nativeCache3109 = Join-Path $frozenCandidate 'native-cache-3109'
$nativeCacheAdditional = Join-Path $frozenCandidate 'all-provider-layouts/native-cache-complete'
$msvcRoot = Join-Path $VisualStudioInstance "VC/Tools/MSVC/$MsvcToolsVersion"
$compiler = Join-Path $msvcRoot 'bin/Hostx64/x64/cl.exe'
$linker = Join-Path $msvcRoot 'bin/Hostx64/x64/link.exe'
$assembler = Join-Path $msvcRoot 'bin/Hostx64/x64/ml64.exe'
$windowsKits = 'C:/Program Files (x86)/Windows Kits/10'
$windowsHeader = Join-Path $windowsKits "Include/$WindowsSdkVersion/um/Windows.h"
$windowsKernelLibrary = Join-Path $windowsKits "Lib/$WindowsSdkVersion/um/x64/kernel32.lib"

Assert-InputHash $CMakeExecutable '70FA92CE2AC9F54B0AE395B0B3790D9147EF2EBDBD7C4E0BB20852AAC581BAEA'
Assert-InputHash $compiler '6D23D795315737B52325B15308A72F59C54F68CB9812E6859B1113564FCCFF58'
Assert-InputHash $linker 'D78A3A29C27E7949A164386A92FC6EB51F8074E5AF248DC574196D49AEC93E3E'
Assert-InputHash $assembler 'B7F1B51EBA109E3AA1EE9C805D7FFEB08D9589578D6F487F1ACD6A8389796D43'
Assert-InputHash $windowsHeader 'B337D661D03A4ABEFB7B86A2742CE1AD5D19B57CD8B858BD13E7BBCC1DBEEAAA'
Assert-InputHash $windowsKernelLibrary '25346E02CFFCA92ABFF07000D54E1830FE0D8861C31A114EDA472547FE9F2F00'
Assert-InputHash (Join-Path $sourceDirectory 'ampere_font_native.inc') 'DB74E405A13D82EA9353EDBD840722D9923317FFD029E2F703E668B3D9F93441' -NormalizeText
Assert-InputHash (Join-Path $StreamlineRoot 'include/sl_dlss_g.h') '1FC18CBE004E280DF1F787276D08A1B28B8A8C4C65856FBAA659F56DFF6A915D'
Assert-InputHash (Join-Path $StreamlineRoot 'include/sl_reflex.h') '3B623A1189E04A686384D224A58C4AD9974C4E6E3204077676F6EC529475164C'
Assert-InputHash (Join-Path $StreamlineRoot 'include/sl_struct.h') '28DEDA67EA1A74371DD4F33FF4842B7DB03B2DB007001DA6DB06F6982297928D'
Assert-InputHash (Join-Path $StreamlineRoot 'include/sl.h') 'E1E81A7428D15B30DB37587E9469BD68A56D630D820A4ACCEC0AEE3B17E157DD'
Assert-InputHash (Join-Path $StreamlineRoot 'include/sl_appidentity.h') '1337385AC9867D66FA6BEB34C750E79AAF25A74A156A47E83F24937299DADE87'
Assert-InputHash (Join-Path $StreamlineRoot 'include/sl_consts.h') '17DE74AFDA2CB96204FF380464BED926F660D92015FE0304DFF4E329509C88B9'
Assert-InputHash (Join-Path $StreamlineRoot 'include/sl_core_api.h') '328DC3A2C1DEE579C200CA97E2D5B1EC38C893BE4BA2745EF5AD506AF7172E81'
Assert-InputHash (Join-Path $StreamlineRoot 'include/sl_core_types.h') 'F420CB052CE14489FC9FA0D933C3ED81A010DEB01FA13B1AA8D45DA1C5BA3234'
Assert-InputHash (Join-Path $StreamlineRoot 'include/sl_device_wrappers.h') 'AF7741305B1A468C3A83EFAA2005ECD53D2FFD7AADB8EC6868C9DCB4A8A8E3A1'
Assert-InputHash (Join-Path $StreamlineRoot 'include/sl_pcl.h') 'F43C5135FC8D5349CCD345E424F8CF1F61953A9F17E9897204B0741A3AB7B0FB'
Assert-InputHash (Join-Path $StreamlineRoot 'include/sl_result.h') '2A0F6C12863BDC00B38910A5EC85D1F083C5671EE817A920AFE626AC2A9100F7'
Assert-InputHash (Join-Path $StreamlineRoot 'include/sl_version.h') '48E9CB86F0FF6711304BDF3F7150F46466878BBEFC21F648773267BC6A326285'
Assert-InputHash (Join-Path $StreamlineRoot 'external/ngx-sdk/include/nvsdk_ngx.h') '96F21266A05EF06BB49F58C89DE1A7845F2D9B1E0E0E069FE03740B8EAF0344D'
Assert-InputHash (Join-Path $StreamlineRoot 'external/ngx-sdk/include/nvsdk_ngx_defs.h') 'A9481CBC78D53EE56639F0A573EC095C6C1EA1E5F296D139C20C42BF342C2E1B'
Assert-InputHash (Join-Path $StreamlineRoot 'external/ngx-sdk/include/nvsdk_ngx_helpers.h') '4569AA7566667D8C0D93C8E50DDD3F2C4FA1C89DA536FFDC2571BBDC97A539E9'
Assert-InputHash (Join-Path $StreamlineRoot 'external/ngx-sdk/include/nvsdk_ngx_helpers_cuda.h') '18F2FD3ADDA56124A975A9E5F7347E61CA2E25BD58E78B7922EDF6C5C0857281'
Assert-InputHash (Join-Path $StreamlineRoot 'external/ngx-sdk/include/nvsdk_ngx_helpers_d3d.h') '9B3C356CD2D04E7924C0B6B97FC1F1A37E5D30095A8C22133A5A08398CEF2C4C'
Assert-InputHash (Join-Path $StreamlineRoot 'external/ngx-sdk/include/nvsdk_ngx_params.h') '51A1F94116CF82F95C63A2C197371ED22E08BDDB09918D36CC6E2AC712753C80'
Assert-InputHash (Join-Path $ImGuiRoot 'imgui.h') '2CA314D19C7E1EA85687A9C1E37079E0CC823939A551B3E6B2453FF0AF9FBF13'
Assert-InputHash (Join-Path $ImGuiRoot 'imconfig.h') 'FB8E32B9AF9AA7DAD5EC5C5BC862537F5624CB39A814E6D3B36B4629A50B6599'
Assert-InputHash (Join-Path $ImGuiRoot 'imgui_internal.h') 'D34F90D1F5E6F364949AD1CC3E5562F7FB18781EC3988D1282A59528F39FF3D6'
Assert-InputHash (Join-Path $ImGuiRoot 'imstb_rectpack.h') 'BB53504995E983D54B1AE06EA727F0B39647E5E205B4BF7DA01343953974951C'
Assert-InputHash (Join-Path $ImGuiRoot 'imstb_textedit.h') '24A8DB00354AF8F4057417841635A1B6DFD8986F1608336FE6F10D6FD9769AAA'
Assert-InputHash (Join-Path $ImGuiRoot 'imstb_truetype.h') '37AA1D602706262BF94DA2F83EFAA8175EBC2202EDE13DA96B692FBCF8B4427B'
Assert-InputHash (Join-Path $ImGuiRoot 'backends/imgui_impl_dx12.h') '9127655FEEC0E45BFA197F7BAE96C21C81B813448DE83410CDF0DBADBD8B33B0'
Assert-InputHash (Join-Path $ImGuiRoot 'backends/imgui_impl_vulkan.h') 'F90DAB036827189E549A03A13414B013CE1F595B5ECF7B55787845444E2A92E9'
Assert-InputHash (Join-Path $ImGuiRoot 'backends/imgui_impl_win32.h') 'D71CFDBA658C65EADF90C47CD2B73F023674A57E4ADB577636D35002FE00AA9A'
Assert-InputHash (Join-Path $ImGuiRoot 'imgui.cpp') 'E97F4CB564B750108CC219372ECA8AB7CAC8DE892FC57B3A1932B0A170E1ED80'
Assert-InputHash (Join-Path $ImGuiRoot 'imgui_draw.cpp') 'FA3254D7D074E8FC27756CA64BFA63455CF72CD2579977FCA40C25DC692305EC'
Assert-InputHash (Join-Path $ImGuiRoot 'imgui_widgets.cpp') 'EE1EB52CCA7C5A0ACFF9A7B3E06833600B28F465E6BD44D2EF1887227D3637DE'
Assert-InputHash (Join-Path $ImGuiRoot 'imgui_tables.cpp') '66E8CA12A4C42B9C953152059B784F2DF0205AC8772A969123887A755C9D3FBC'
Assert-InputHash (Join-Path $ImGuiRoot 'backends/imgui_impl_win32.cpp') '76B8AFB8851888B4FAFD0B6BAE05335C1478AAE6B50C1B633B3BE036631203EE'
Assert-InputHash (Join-Path $ImGuiRoot 'backends/imgui_impl_dx12.cpp') '4932F938C6D48EDF91B45078CF0BDD0BF9AD46CC5D4B9A48873528046DA7C580'
Assert-InputHash (Join-Path $ImGuiRoot 'backends/imgui_impl_vulkan.cpp') 'AF53CFE867184C94C736B8BC620E17DDE8A61BAFA3F0B6F43B9532E065F08AEF'
Assert-InputHash (Join-Path $VulkanIncludeDirectory 'vulkan/vulkan.h') '44410C061C28016069A03E6EFDD642952DD24308A7FF35460B6F3B928EADA354'
Assert-InputHash (Join-Path $VulkanIncludeDirectory 'vulkan/vk_platform.h') '9FC121C475DE911518D1975352D3ED6320B3CDDD7D7E28B5E2D89B91E6BD9981'
Assert-InputHash (Join-Path $VulkanIncludeDirectory 'vulkan/vulkan_core.h') '0C7E4D78E18170758BC4EB15C78065237A0A083211EAE1DF89020E3C954AD79F'
Assert-InputHash (Join-Path $VulkanIncludeDirectory 'vulkan/vulkan_win32.h') '39F7E94FEA15B8391898DBEF6930C21AC13D59D4DE0298023B99D855068C06F6'
Assert-InputHash (Join-Path $nativeCache 'ampere_native_manifest.inc') '4C8765C79A4947A7F3A79EC98DB2DB7E3E95584621E853909BF5EDB2DA7D2C24'
Assert-InputHash (Join-Path $nativeCache3109 'ampere_native_manifest.inc') '90E3E797B5121B6FFBF52C1765FAC9683A9C1641363EE0CC669296754B4B2291'
Assert-InputHash (Join-Path $nativeCache 'manifest.json') 'B90F4131CD7E9E93C164FE2680C29475EFA2434DD6CF343C8713FCE4E2557E09'
Assert-InputHash (Join-Path $nativeCache3109 'manifest.json') 'D6D42F8BC002B29833EDE69E27DBC00E6E2913716BC6131DBAE19D02215E58D9'
Assert-InputHash (Join-Path $nativeCacheAdditional 'ampere_native_manifest_additional.inc') '1E0770EA6C85B4267E1068D16A723845D4AB949FA0BC142D2DFF2C24E88C9F64'
Assert-InputHash (Join-Path $nativeCacheAdditional 'additional-resources.json') '0007B3CCBC3D3B3ACE90DF4B438050013EE31DE8B6069AF1A39DCA5B7DAE7EDD'

if (-not (Test-Path -LiteralPath (Join-Path $sourceDirectory 'CMakeLists.txt') -PathType Leaf)) {
    throw "Corrected source snapshot is incomplete: $sourceDirectory"
}
if (Test-Path -LiteralPath $BuildDirectory) {
    throw "Use a fresh dedicated build directory: $BuildDirectory"
}

$arguments = @(
    '-S', $sourceDirectory,
    '-B', $BuildDirectory,
    '-G', 'Visual Studio 17 2022',
    '-A', "x64,version=$WindowsSdkVersion",
    '-T', 'v143',
    "-DCMAKE_GENERATOR_INSTANCE=$VisualStudioInstance",
    "-DCMAKE_VS_GLOBALS=VCToolsVersion=$MsvcToolsVersion",
    "-DSTREAMLINE_ROOT=$StreamlineRoot",
    "-DIMGUI_ROOT=$ImGuiRoot",
    "-DVULKAN_INCLUDE_DIR=$VulkanIncludeDirectory",
    "-DGLSLANG_VALIDATOR=$(Join-Path $VulkanIncludeDirectory ../Bin/glslangValidator.exe)",
    '-DMFG_UNLOCK_RUNTIME_GPU_SELECTION=ON',
    '-DMFG_UNLOCK_BUILD_SINGLE_MODULE=ON',
    '-DMFG_UNLOCK_OUTPUT_PULL_MASK_ONLY=ON',
    '-DMFG_UNLOCK_OUTPUT_PULL_MASK_OCCUPANCY=OFF',
    '-DMFG_UNLOCK_OUTPUT_PULL_EXPERIMENT=OFF',
    '-DMFG_UNLOCK_OUTPUT_PULL_TELEMETRY=OFF',
    '-DMFG_UNLOCK_PREV2CURR_EXPERIMENT=OFF',
    '-DMFG_UNLOCK_INTERM_SCATTER_EXPERIMENT=OFF',
    '-DMFG_AMPERE_EMBEDDED_KERNELS=ON',
    '-DMFG_AMPERE_KERNEL_IMAGE=AUTO',
    "-DMFG_AMPERE_NATIVE_MANIFEST=$(Join-Path $nativeCache 'ampere_native_manifest.inc')",
    "-DMFG_AMPERE_KERNEL_DIRECTORY=$(Join-Path $nativeCache 'RTX30MFG-Kernels')",
    "-DMFG_AMPERE_NATIVE_MANIFEST_3109=$(Join-Path $nativeCache3109 'ampere_native_manifest.inc')",
    "-DMFG_AMPERE_KERNEL_DIRECTORY_3109=$(Join-Path $nativeCache3109 'RTX30MFG-Kernels')",
    "-DMFG_AMPERE_NATIVE_MANIFEST_ADDITIONAL=$(Join-Path $nativeCacheAdditional 'ampere_native_manifest_additional.inc')",
    "-DMFG_AMPERE_NATIVE_RESOURCES_ADDITIONAL=$(Join-Path $nativeCacheAdditional 'additional-resources.json')",
    "-DMFG_AMPERE_KERNEL_DIRECTORY_ADDITIONAL=$(Join-Path $nativeCacheAdditional 'RTX30MFG-Kernels')"
)

& $CMakeExecutable @arguments
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed: $LASTEXITCODE" }

function Read-CacheValue([string]$Name) {
    $prefix = $Name + ':'
    $line = Get-Content -LiteralPath (Join-Path $BuildDirectory 'CMakeCache.txt') |
        Where-Object { $_.StartsWith($prefix, [StringComparison]::Ordinal) } |
        Select-Object -First 1
    if (-not $line -or $line.IndexOf('=') -lt 0) { return $null }
    return $line.Substring($line.IndexOf('=') + 1)
}
function Assert-CacheValue([string]$Name, [string]$Expected) {
    $actual = Read-CacheValue $Name
    if ($actual -cne $Expected) {
        throw "Configured toolchain mismatch: $Name expected=$Expected actual=$actual"
    }
}
function Assert-CachePath([string]$Name, [string]$Expected) {
    $actual = Read-CacheValue $Name
    if (-not $actual -or -not [IO.Path]::GetFullPath($actual).Equals(
            [IO.Path]::GetFullPath($Expected), [StringComparison]::OrdinalIgnoreCase)) {
        throw "Configured toolchain path mismatch: $Name expected=$Expected actual=$actual"
    }
}

Assert-CacheValue 'CMAKE_GENERATOR' 'Visual Studio 17 2022'
Assert-CachePath 'CMAKE_GENERATOR_INSTANCE' $VisualStudioInstance
Assert-CacheValue 'CMAKE_GENERATOR_TOOLSET' 'v143'
Assert-CacheValue 'CMAKE_GENERATOR_PLATFORM' "x64,version=$WindowsSdkVersion"
Assert-CacheValue 'CMAKE_VS_GLOBALS' "VCToolsVersion=$MsvcToolsVersion"
Assert-CachePath 'CMAKE_ASM_MASM_COMPILER' $assembler
Assert-CachePath 'MFG_AMPERE_NATIVE_MANIFEST_ADDITIONAL' (Join-Path $nativeCacheAdditional 'ampere_native_manifest_additional.inc')
Assert-CachePath 'MFG_AMPERE_NATIVE_RESOURCES_ADDITIONAL' (Join-Path $nativeCacheAdditional 'additional-resources.json')
Assert-CachePath 'MFG_AMPERE_KERNEL_DIRECTORY_ADDITIONAL' (Join-Path $nativeCacheAdditional 'RTX30MFG-Kernels')
Assert-CacheValue 'MFG_UNLOCK_OUTPUT_PULL_MASK_ONLY' 'ON'
Assert-CacheValue 'MFG_UNLOCK_OUTPUT_PULL_MASK_OCCUPANCY' 'OFF'
foreach ($legacyExperiment in @('MFG_UNLOCK_OUTPUT_PULL_EXPERIMENT', 'MFG_UNLOCK_OUTPUT_PULL_TELEMETRY',
    'MFG_UNLOCK_PREV2CURR_EXPERIMENT', 'MFG_UNLOCK_INTERM_SCATTER_EXPERIMENT')) {
    Assert-CacheValue $legacyExperiment 'OFF'
}

$compilerMetadata = Get-ChildItem -LiteralPath (Join-Path $BuildDirectory 'CMakeFiles') `
    -Recurse -File -Filter 'CMakeCXXCompiler.cmake' | Select-Object -First 1
if (-not $compilerMetadata) { throw 'CMake did not emit C++ compiler metadata.' }
$compilerText = Get-Content -LiteralPath $compilerMetadata.FullName -Raw
$compilerRecord = 'set(CMAKE_CXX_COMPILER "' + $compiler.Replace('\', '/') + '")'
$linkerRecord = 'set(CMAKE_LINKER "' + $linker.Replace('\', '/') + '")'
if (-not $compilerText.Contains($compilerRecord) `
    -or -not $compilerText.Contains($linkerRecord) `
    -or -not $compilerText.Contains('set(CMAKE_CXX_COMPILER_VERSION "19.38.33135.0")') `
    -or -not $compilerText.Contains('set(CMAKE_CXX_COMPILER_ARCHITECTURE_ID "x64")')) {
    throw "Configured compiler metadata is not the recorded MSVC x64 toolchain: $($compilerMetadata.FullName)"
}

& $CMakeExecutable --build $BuildDirectory --config Release --parallel --target RTXMFGUnified
if ($LASTEXITCODE -ne 0) { throw "Native build failed: $LASTEXITCODE" }

$dll = Join-Path $BuildDirectory 'Release/RTXMFG.dll'
if (-not (Test-Path -LiteralPath $dll -PathType Leaf)) {
    throw "Unified build did not produce the expected DLL: $dll"
}
Get-FileHash -LiteralPath $dll -Algorithm SHA256
