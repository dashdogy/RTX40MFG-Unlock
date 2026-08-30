[CmdletBinding()]
param(
    [double]$MinimumScalar = -4.0,

    [double]$MaximumScalar = 5.0,

    [ValidateRange(0.000001, 1000.0)]
    [double]$Step = 0.25,

    [single[]]$ScalarValues,

    [ValidateRange(60, 5000)]
    [int]$MaxFrames = 360,

    [string]$RunDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$requestedValues = [Collections.Generic.List[single]]::new()
if ($null -ne $ScalarValues -and $ScalarValues.Count -gt 0) {
    foreach ($value in $ScalarValues) {
        if ([single]::IsNaN($value) -or [single]::IsInfinity($value)) {
            throw 'Every explicit scalar value must be finite.'
        }
        $requestedValues.Add($value)
    }
}
else {
    if ([double]::IsNaN($MinimumScalar) -or [double]::IsInfinity($MinimumScalar) -or
        [double]::IsNaN($MaximumScalar) -or [double]::IsInfinity($MaximumScalar)) {
        throw 'Scalar bounds must be finite.'
    }
    if ($MaximumScalar -lt $MinimumScalar) {
        throw 'MaximumScalar must be greater than or equal to MinimumScalar.'
    }
    $rangeCount = [int][Math]::Floor(
        (($MaximumScalar - $MinimumScalar) / $Step) + 1e-9) + 1
    for ($index = 0; $index -lt $rangeCount; ++$index) {
        $requestedValues.Add([single]($MinimumScalar + ($index * $Step)))
    }
}

$scalarCount = $requestedValues.Count
if ($scalarCount -lt 1 -or $scalarCount -gt 97) {
    throw "The requested sweep contains $scalarCount values; the supported range is 1 through 97."
}

$scalars = [Collections.Generic.List[object]]::new()
for ($index = 0; $index -lt $scalarCount; ++$index) {
    $value = $requestedValues[$index]
    $bits = [BitConverter]::ToUInt32([BitConverter]::GetBytes($value), 0)
    $scalars.Add([ordered]@{
        index = $index
        value = $value
        bits = ('{0:X8}' -f $bits)
    })
}

$testsPath = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoPath = Split-Path -Parent $testsPath
$samplePath = Join-Path $repoPath 'outputs\tools\Streamline_Sample-v2.12.0\_bin'
$sampleExe = Join-Path $samplePath 'StreamlineSample.exe'
$traceConfig = Join-Path $repoPath 'test_runs\dl4rt-trace-fixed6-config.json'
$cdbPath = 'C:\Program Files\WindowsApps\Microsoft.WinDbg_1.2606.22001.0_x64__8wekyb3d8bbwe\amd64\cdb.exe'
$expectedImageHash = '20C18281E88F376BD6A0C1AB5ECFCB6F1BED931B928F956299D3489703B84CC3'

foreach ($requiredPath in @($sampleExe, $traceConfig, $cdbPath)) {
    if (-not (Test-Path -LiteralPath $requiredPath)) {
        throw "Required sweep input was not found: $requiredPath"
    }
}

if ([string]::IsNullOrWhiteSpace($RunDirectory)) {
    $runStamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $RunDirectory = Join-Path $repoPath "test_runs\dl4rt-scalar-sweep-sm89-$runStamp"
}
$RunDirectory = [IO.Path]::GetFullPath($RunDirectory)
New-Item -ItemType Directory -Path $RunDirectory -Force | Out-Null

$traceLog = Join-Path $RunDirectory 'dl4rt-scalar-sweep.cdb.log'
$sampleStdout = Join-Path $RunDirectory 'sample.stdout.txt'
$sampleStderr = Join-Path $RunDirectory 'sample.stderr.txt'
$mapPath = Join-Path $RunDirectory 'scalar-map.json'

$environment = [ordered]@{
    RTX40_MFG_CONFIG_PATH = $traceConfig
    RTX40_MFG_SAMPLE_PROBE = '1'
    RTX40_MFG_SAMPLE_EXPLICIT_QUEUE = '1'
    RTX40_MFG_NGX_OUTPUT_PROBE = '1'
    RTX40_MFG_PRESENT_PROBE = '0'
    RTX40_MFG_IMMUTABLE_OUTPUTS = '0'
    RTX40_MFG_UNSAFE_PRESENT_READBACK = '0'
    RTX40_MFG_DISABLE_FULL_NGX_STATE = '0'
    RTX40_MFG_FORCE_FULL_NGX_STATE = '0'
    RTX40_MFG_EXPERIMENTAL_SM120_TARGET = '0'
    RTX40_MFG_EXPERIMENTAL_DL4RT_SM120_PATH = '0'
    RTX40_MFG_EXPERIMENTAL_TEMPORAL_PREEMPHASIS = '0'
}
foreach ($entry in $environment.GetEnumerator()) {
    [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, 'Process')
}

$sampleArguments = @(
    '-Reflex_mode', '1',
    '-Reflex_fpsCap', '20',
    '-DLSSG_on',
    '-DLSSG_numFrameToGenerate', '5',
    '-maxFrames', $MaxFrames.ToString()
)

$metadata = [ordered]@{
    schema = 1
    createdAt = (Get-Date).ToString('o')
    gpuPath = 'sm89-dl2'
    activeImageSha256Expected = $expectedImageHash
    generatedCount = 5
    maxFrames = $MaxFrames
    assignment = 'scalarIndex = zeroBasedDl2DispatchNumber modulo scalarCount'
    rotationNote = 'A scalar count coprime with five rotates every scalar through every native output index.'
    scalarCount = $scalars.Count
    scalars = $scalars
    environment = $environment
    sampleArguments = $sampleArguments
}
$metadata | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $mapPath -Encoding UTF8

$sampleProcess = $null
$samplePid = $null
try {
    $sampleProcess = Start-Process -FilePath $sampleExe -WorkingDirectory $samplePath `
        -ArgumentList $sampleArguments -PassThru `
        -RedirectStandardOutput $sampleStdout -RedirectStandardError $sampleStderr
    $samplePid = $sampleProcess.Id
    Write-Host "Started Streamline sample PID $samplePid for a $($scalars.Count)-point scalar sweep."

    $activeModule = $null
    $moduleDeadline = [DateTime]::UtcNow.AddSeconds(30)
    while ([DateTime]::UtcNow -lt $moduleDeadline -and -not $activeModule) {
        $liveProcess = Get-Process -Id $samplePid -ErrorAction SilentlyContinue
        if (-not $liveProcess) {
            break
        }
        try {
            $activeModule = $liveProcess.Modules | Where-Object {
                $_.FileName -match '\\NVIDIA\\NGX\\models\\dlssg\\' -and
                $_.FileName -like '*.bin'
            } | Select-Object -First 1
        }
        catch {
            $activeModule = $null
        }
        if (-not $activeModule) {
            Start-Sleep -Milliseconds 100
        }
    }
    if (-not $activeModule) {
        throw 'The active cached DLSS-G image was not observed before the sample exited.'
    }

    $activeImagePath = $activeModule.FileName
    $activeImageHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $activeImagePath).Hash
    if ($activeImageHash -ne $expectedImageHash) {
        throw "Unsupported cached DLSS-G image hash $activeImageHash at $activeImagePath"
    }

    $moduleBase = [UInt64]$activeModule.BaseAddress.ToInt64()
    $dl2Final = '0x{0:x}' -f ($moduleBase + 0x51F48)

    $metadata.activeImage = $activeImagePath
    $metadata.activeImageSha256 = $activeImageHash
    $metadata.moduleBase = ('0x{0:X}' -f $moduleBase)
    $metadata.dl2Final = $dl2Final
    $metadata.samplePid = $samplePid
    $metadata | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $mapPath -Encoding UTF8

    Write-Host ('Active cached image: {0}' -f $activeImagePath)
    Write-Host ('SHA-256: {0}' -f $activeImageHash)
    Write-Host ('Final DL2 breakpoint: {0}' -f $dl2Final)

    $overrideBlocks = [Collections.Generic.List[string]]::new()
    foreach ($scalar in $scalars) {
        $overrideBlocks.Add(
            '.if (@$t1 == ' + $scalar.index + ') { ed @rcx+0x88 0x' + $scalar.bits + ' }')
    }

    $commands = [Collections.Generic.List[string]]::new()
    $commands.Add('sxi e06d7363')
    $commands.Add('r @$t0=0')
    $commands.Add('r @$t1=0')
    $commands.Add('r @$t2=0')
    $commands.Add(
        '.printf "[DL4RT_SWEEP] ACTIVE base=%p scalarCount=' + $scalars.Count +
        ' hash=' + $activeImageHash + '\\n", ' + ('0x{0:x}' -f $moduleBase))
    $breakpointCommands = [Collections.Generic.List[string]]::new()
    $breakpointCommands.Add('r @$t1=@$t0 % ' + $scalars.Count)
    $breakpointCommands.Add('r @$t2=dwo(@rcx+0x88)')
    foreach ($block in $overrideBlocks) {
        $breakpointCommands.Add($block)
    }
    $breakpointCommands.Add(
        '.printf "[DL4RT_SWEEP] n=%u slot=%u scalarIndex=%u nativeBits=%08x injectedBits=%08x\\n", @$t0, @$t0 % 5, @$t1, @$t2, dwo(@rcx+0x88)')
    $breakpointCommands.Add('r @$t0=@$t0+1')
    $breakpointCommands.Add('gc')
    $dispatchProgram = $breakpointCommands -join '; '
    if ($scalars.Count -le 8) {
        $commands.Add(
            'bp ' + $dl2Final + ' "' + ($dispatchProgram -replace '"', '\"') + '"')
    }
    else {
        # CDB silently drops very long breakpoint command strings. Keep the
        # breakpoint itself short and execute the generated dispatch program
        # from a no-space temporary path on every hit.
        $dispatchScriptPath = Join-Path $env:TEMP "dl4rt-sweep-dispatch-$samplePid.cdb"
        $dispatchProgram | Set-Content -LiteralPath $dispatchScriptPath `
            -Encoding ASCII -NoNewline
        Copy-Item -LiteralPath $dispatchScriptPath -Destination (
            Join-Path $RunDirectory 'dispatch-commands.cdb') -Force
        $commands.Add('bp ' + $dl2Final + ' "$<' + $dispatchScriptPath + '"')
    }
    $commands.Add('g')
    $debugCommands = $commands -join '; '

    & $cdbPath -p $samplePid -G -logo $traceLog -c $debugCommands 2>&1 | Out-Null
    $cdbExitCode = $LASTEXITCODE
    Write-Host "CDB exited with code $cdbExitCode."
    if ($cdbExitCode -ne 0) {
        throw "CDB failed with exit code $cdbExitCode."
    }
}
finally {
    if ($samplePid) {
        $liveProcess = Get-Process -Id $samplePid -ErrorAction SilentlyContinue
        if ($liveProcess) {
            $resolvedPath = $null
            try {
                $resolvedPath = $liveProcess.Path
            }
            catch {
                $resolvedPath = $null
            }
            if ($resolvedPath -and $resolvedPath -ieq $sampleExe) {
                Stop-Process -Id $samplePid -Force -ErrorAction SilentlyContinue
                Write-Host "Stopped sweep sample PID $samplePid."
            }
        }
    }
}

if (-not (Test-Path -LiteralPath $traceLog)) {
    throw "CDB did not create the expected sweep log: $traceLog"
}

Start-Sleep -Seconds 2
$expectedArtifacts = @(
    "MfgUnlock-$samplePid.log",
    "MfgUnlock-ngx-output-$samplePid.csv",
    "MfgUnlock-ngx-output-$samplePid.gray",
    "MfgUnlock-ngx-real-$samplePid.csv"
)
foreach ($artifactName in $expectedArtifacts) {
    $artifactPath = Join-Path $env:TEMP $artifactName
    if (-not (Test-Path -LiteralPath $artifactPath)) {
        throw "Scalar sweep did not produce capture artifact: $artifactPath"
    }
    Copy-Item -LiteralPath $artifactPath -Destination $RunDirectory -Force
}

$markers = @(Select-String -LiteralPath $traceLog -Pattern '^\[DL4RT_SWEEP\] n=' |
    ForEach-Object { $_.Line })
if (-not $markers) {
    throw "The debugger attached, but no final DL2 scalar overrides were recorded. See $traceLog"
}

$outputCsv = Get-ChildItem -LiteralPath $RunDirectory -Filter 'MfgUnlock-ngx-output-*.csv' |
    Select-Object -First 1
$capturedRows = if ($outputCsv) { (Import-Csv -LiteralPath $outputCsv.FullName).Count } else { 0 }
$preAttachRows = $capturedRows - $markers.Count
if ($preAttachRows -lt 0 -or ($preAttachRows % 5) -ne 0) {
    throw "Cannot align $capturedRows captured rows with $($markers.Count) traced dispatches."
}
$metadata.capturedGeneratedRows = $capturedRows
$metadata.tracedDl2Dispatches = $markers.Count
$metadata.preAttachGeneratedRows = $preAttachRows
$metadata.preAttachGeneratedBatches = [int]($preAttachRows / 5)
$metadata | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $mapPath -Encoding UTF8
Write-Host "Recorded $($markers.Count) DL2 overrides and $capturedRows generated-frame rows."
Write-Host "Excluded $preAttachRows pre-attach rows from scalar-response analysis."
Write-Host "Scalar map: $mapPath"
Write-Host "Capture directory: $RunDirectory"
$markers | Select-Object -First ([Math]::Min(12, $markers.Count))
