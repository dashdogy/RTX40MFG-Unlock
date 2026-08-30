[CmdletBinding()]
param(
    [ValidateSet('control', 'preemphasis')]
    [string]$Mode = 'control',

    [ValidateRange(60, 5000)]
    [int]$MaxFrames = 180,

    [string]$OutputLog,

    [switch]$CaptureOutputs,

    [string]$CaptureDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$testsPath = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoPath = Split-Path -Parent $testsPath
$samplePath = Join-Path $repoPath 'outputs\tools\Streamline_Sample-v2.12.0\_bin'
$sampleExe = Join-Path $samplePath 'StreamlineSample.exe'
$traceConfig = Join-Path $repoPath 'test_runs\dl4rt-trace-fixed6-config.json'
$cdbPath = 'C:\Program Files\WindowsApps\Microsoft.WinDbg_1.2606.22001.0_x64__8wekyb3d8bbwe\amd64\cdb.exe'
$expectedImageHash = '20C18281E88F376BD6A0C1AB5ECFCB6F1BED931B928F956299D3489703B84CC3'

foreach ($requiredPath in @($sampleExe, $traceConfig, $cdbPath)) {
    if (-not (Test-Path -LiteralPath $requiredPath)) {
        throw "Required trace input was not found: $requiredPath"
    }
}

$runStamp = Get-Date -Format 'yyyyMMdd-HHmmss'
if ([string]::IsNullOrWhiteSpace($OutputLog)) {
    $OutputLog = Join-Path $repoPath (
        "test_runs\dl4rt-final-input-$Mode-sm89-$runStamp.log")
}
$OutputLog = [IO.Path]::GetFullPath($OutputLog)
$sampleStdout = [IO.Path]::ChangeExtension($OutputLog, '.sample.stdout.txt')
$sampleStderr = [IO.Path]::ChangeExtension($OutputLog, '.sample.stderr.txt')
if ($CaptureOutputs) {
    if ([string]::IsNullOrWhiteSpace($CaptureDirectory)) {
        $CaptureDirectory = [IO.Path]::ChangeExtension($OutputLog, $null) + '.capture'
    }
    $CaptureDirectory = [IO.Path]::GetFullPath($CaptureDirectory)
    New-Item -ItemType Directory -Path $CaptureDirectory -Force | Out-Null
}

$environment = [ordered]@{
    RTX40_MFG_CONFIG_PATH = $traceConfig
    RTX40_MFG_SAMPLE_PROBE = '1'
    RTX40_MFG_SAMPLE_EXPLICIT_QUEUE = '1'
    RTX40_MFG_NGX_OUTPUT_PROBE = $(if ($CaptureOutputs) { '1' } else { '0' })
    RTX40_MFG_PRESENT_PROBE = '0'
    RTX40_MFG_IMMUTABLE_OUTPUTS = '0'
    RTX40_MFG_UNSAFE_PRESENT_READBACK = '0'
    RTX40_MFG_DISABLE_FULL_NGX_STATE = '0'
    RTX40_MFG_FORCE_FULL_NGX_STATE = '0'
    RTX40_MFG_EXPERIMENTAL_SM120_TARGET = '0'
    RTX40_MFG_EXPERIMENTAL_DL4RT_SM120_PATH = '0'
    RTX40_MFG_EXPERIMENTAL_TEMPORAL_PREEMPHASIS = $(
        if ($Mode -eq 'preemphasis') { '1' } else { '0' })
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

$sampleProcess = $null
$samplePid = $null
try {
    $sampleProcess = Start-Process -FilePath $sampleExe -WorkingDirectory $samplePath `
        -ArgumentList $sampleArguments -PassThru `
        -RedirectStandardOutput $sampleStdout -RedirectStandardError $sampleStderr
    $samplePid = $sampleProcess.Id
    Write-Host "Started Streamline sample PID $samplePid in $Mode mode."

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
    $address = {
        param([UInt64]$RelativeVirtualAddress)
        '0x{0:x}' -f ($moduleBase + $RelativeVirtualAddress)
    }
    $timeReader = & $address 0x3D895
    $dl1Final = & $address 0x51D51
    $dl2TimeCopy = & $address 0x51EE6
    $dl2Final = & $address 0x51F48

    Write-Host ('Active cached image: {0}' -f $activeImagePath)
    Write-Host ('SHA-256: {0}' -f $activeImageHash)
    Write-Host ('Module base: 0x{0:x}' -f $moduleBase)
    Write-Host "Breakpoints: time=$timeReader dl1=$dl1Final copy=$dl2TimeCopy dl2=$dl2Final"

    $commands = [Collections.Generic.List[string]]::new()
    $commands.Add('sxi e06d7363')
    $commands.Add('r @$t0=0')
    $commands.Add('r @$t1=0')
    $commands.Add('r @$t2=0')
    $commands.Add('r @$t3=0')
    $commands.Add(
        '.printf "[DL4RT_TRACE] ACTIVE base=%p mode=' + $Mode +
        ' hash=' + $activeImageHash + '\\n", ' + ('0x{0:x}' -f $moduleBase))
    $commands.Add(
        'bp ' + $timeReader +
        ' ".if (@$t0 < 40) { .printf \"[DL4RT_TRACE] TIME_READER n=%d input=%p count=%u index=%u timeBits=%08x\\n\", @$t0, @r12, dwo(@r12+0x4f0), dwo(@r12+0x4ec), dwo(@r12+0x4f4); r @$t0=@$t0+1; }; gc"')
    $commands.Add(
        'bp ' + $dl1Final +
        ' ".if (@$t1 < 8) { .printf \"[DL4RT_TRACE] DL1_FINAL n=%d net=%p vtbl=%p target=%p\\n\", @$t1, @rcx, poi(@rcx), poi(poi(@rcx)+0x40); r @$t1=@$t1+1; }; gc"')
    $commands.Add(
        'bp ' + $dl2TimeCopy +
        ' ".if (@$t2 < 40) { .printf \"[DL4RT_TRACE] DL2_TIME_COPY n=%d wrapper=%p net=%p sourceBits=%08x oldDestBits=%08x\\n\", @$t2, @rbx, poi(@rbx+0x18), dwo(@rbp+0x50), dwo(poi(@rbx+0x18)+0x88); r @$t2=@$t2+1; }; gc"')
    $commands.Add(
        'bp ' + $dl2Final +
        ' ".if (@$t3 < 40) { .printf \"[DL4RT_TRACE] DL2_FINAL n=%d net=%p vtbl=%p target=%p timeBits=%08x rdx=%p r8=%p r9=%p arg5=%p\\n\", @$t3, @rcx, poi(@rcx), poi(poi(@rcx)+0x40), dwo(@rcx+0x88), @rdx, @r8, @r9, poi(@rsp+0x20); .if (@$t3 < 2) { .printf \"[DL4RT_TRACE] DL2_BINDINGS n=%d\\n\", @$t3; dq @rdx L6; dq @r8 L6; dq @r9 L6; dq poi(@rsp+0x20) L6; }; r @$t3=@$t3+1; }; gc"')
    $commands.Add('bl')
    $commands.Add('g')
    $debugCommands = $commands -join '; '

    & $cdbPath -p $samplePid -G -logo $OutputLog -c $debugCommands 2>&1 | Out-Null
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
                Write-Host "Stopped trace sample PID $samplePid."
            }
        }
    }
}

if (-not (Test-Path -LiteralPath $OutputLog)) {
    throw "CDB did not create the expected trace log: $OutputLog"
}

$mfgLog = Join-Path $env:TEMP "MfgUnlock-$samplePid.log"
if (Test-Path -LiteralPath $mfgLog) {
    $mfgLogDestination = if ($CaptureOutputs) {
        Join-Path $CaptureDirectory "MfgUnlock-$samplePid.log"
    }
    else {
        [IO.Path]::ChangeExtension($OutputLog, '.mfg.log')
    }
    Copy-Item -LiteralPath $mfgLog -Destination $mfgLogDestination -Force
}

if ($CaptureOutputs) {
    Start-Sleep -Seconds 2
    $captureArtifacts = @(
        "MfgUnlock-ngx-output-$samplePid.csv",
        "MfgUnlock-ngx-output-$samplePid.gray",
        "MfgUnlock-ngx-real-$samplePid.csv"
    )
    foreach ($artifactName in $captureArtifacts) {
        $artifactPath = Join-Path $env:TEMP $artifactName
        if (-not (Test-Path -LiteralPath $artifactPath)) {
            throw "DL4RT trace did not produce capture artifact: $artifactPath"
        }
        Copy-Item -LiteralPath $artifactPath -Destination $CaptureDirectory -Force
    }
    Copy-Item -LiteralPath $sampleStdout,$sampleStderr `
        -Destination $CaptureDirectory -Force
    Write-Host "Output capture: $CaptureDirectory"
}

$markers = Select-String -LiteralPath $OutputLog -Pattern '\[DL4RT_TRACE\]' |
    ForEach-Object { $_.Line }
if (-not ($markers -match 'DL2_FINAL')) {
    throw "The debugger attached, but no final DL2 dispatch was captured. See $OutputLog"
}

Write-Host "Trace log: $OutputLog"
$markers | Select-Object -First 24
