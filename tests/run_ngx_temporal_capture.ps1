[CmdletBinding()]
param(
    [ValidateSet('control', 'preemphasis')]
    [string]$Mode = 'control',

    [ValidateRange(60, 5000)]
    [int]$MaxFrames = 100,

    [string]$RunDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$testsPath = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoPath = Split-Path -Parent $testsPath
$samplePath = Join-Path $repoPath 'outputs\tools\Streamline_Sample-v2.12.0\_bin'
$sampleExe = Join-Path $samplePath 'StreamlineSample.exe'
$traceConfig = Join-Path $repoPath 'test_runs\dl4rt-trace-fixed6-config.json'

foreach ($requiredPath in @($sampleExe, $traceConfig)) {
    if (-not (Test-Path -LiteralPath $requiredPath)) {
        throw "Required capture input was not found: $requiredPath"
    }
}

if ([string]::IsNullOrWhiteSpace($RunDirectory)) {
    $runStamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $RunDirectory = Join-Path $repoPath (
        "test_runs\streamline-final-dl4rt-$Mode-fixed6-$runStamp")
}
$RunDirectory = [IO.Path]::GetFullPath($RunDirectory)
New-Item -ItemType Directory -Path $RunDirectory -Force | Out-Null

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
    RTX40_MFG_EXPERIMENTAL_TEMPORAL_PREEMPHASIS = $(
        if ($Mode -eq 'preemphasis') { '1' } else { '0' })
}
foreach ($entry in $environment.GetEnumerator()) {
    [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, 'Process')
}
$environment | ConvertTo-Json | Set-Content -LiteralPath (
    Join-Path $RunDirectory 'environment.json') -Encoding UTF8

$sampleArguments = @(
    '-Reflex_mode', '1',
    '-Reflex_fpsCap', '20',
    '-DLSSG_on',
    '-DLSSG_numFrameToGenerate', '5',
    '-maxFrames', $MaxFrames.ToString()
)
$sampleArguments -join ' ' | Set-Content -LiteralPath (
    Join-Path $RunDirectory 'sample-command-line.txt') -Encoding UTF8

$stdoutPath = Join-Path $RunDirectory 'sample-stdout.txt'
$stderrPath = Join-Path $RunDirectory 'sample-stderr.txt'
$sampleProcess = Start-Process -FilePath $sampleExe -WorkingDirectory $samplePath `
    -ArgumentList $sampleArguments -PassThru `
    -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
$samplePid = $sampleProcess.Id
Write-Host "Started Streamline sample PID $samplePid in $Mode mode."

if (-not $sampleProcess.WaitForExit(300000)) {
    $liveProcess = Get-Process -Id $samplePid -ErrorAction SilentlyContinue
    if ($liveProcess -and $liveProcess.Path -ieq $sampleExe) {
        Stop-Process -Id $samplePid -Force
    }
    throw 'The Streamline temporal capture exceeded its five-minute timeout.'
}
if ($sampleProcess.ExitCode -ne 0) {
    throw "Streamline sample exited with code $($sampleProcess.ExitCode)."
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
        throw "Capture artifact was not produced: $artifactPath"
    }
    Copy-Item -LiteralPath $artifactPath -Destination $RunDirectory -Force
}

$captureLog = Join-Path $RunDirectory "MfgUnlock-$samplePid.log"
if ($Mode -eq 'preemphasis') {
    $patchEvidence = Select-String -LiteralPath $captureLog -Pattern (
        'ngxCachedDlssgImplementation=1.*temporalPreEmphasisPatched=1')
    if (-not $patchEvidence) {
        throw 'The output capture completed without proof that the cached DLSS-G backend was patched.'
    }
}

Write-Host "Capture directory: $RunDirectory"
Get-ChildItem -LiteralPath $RunDirectory -File |
    Select-Object Name, Length, LastWriteTime
