# Windows PowerShell 5.1; can run on Windows 7 with WMF 5.1 installed.
# Controller remains outside AE so waiting cannot starve AE's idle hook.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$AfterFX,
    [Parameter(Mandatory=$true)][string]$Queue,
    [Parameter(Mandatory=$true)][string]$JobId,
    [Parameter(Mandatory=$true)][string]$Template,
    [Parameter(Mandatory=$true)][string]$Audio,
    [Parameter(Mandatory=$true)][string]$AudioItemName,
    [Parameter(Mandatory=$true)][string]$Composition,
    [Parameter(Mandatory=$true)][string]$SoundKeysLayer,
    [Parameter(Mandatory=$true)][string]$Profile,
    [Parameter(Mandatory=$true)][string]$PreparedProject,
    [Parameter(Mandatory=$true)][string]$OutputProject,
    [int]$EffectIndex=1,
    [double]$WorkStart=0,
    [Parameter(Mandatory=$true)][double]$WorkDuration,
    [int]$TimeoutSeconds=900
)
$ErrorActionPreference = 'Stop'
if ($JobId -notmatch '^[A-Za-z0-9_-]{1,64}$') { throw 'Invalid JobId' }
if ($TimeoutSeconds -lt 1 -or $WorkStart -lt 0 -or $WorkDuration -le 0 -or $EffectIndex -lt 1) { throw 'Invalid numeric option' }
function LocalPath([string]$p) {
    if ($p -notmatch '^[A-Za-z]:[\\/]') { throw "Use absolute local paths: $p" }
    return [IO.Path]::GetFullPath($p)
}
$AfterFX=LocalPath $AfterFX; $Queue=LocalPath $Queue; $Template=LocalPath $Template
$Audio=LocalPath $Audio; $Profile=LocalPath $Profile
$PreparedProject=LocalPath $PreparedProject; $OutputProject=LocalPath $OutputProject
foreach ($path in @($AfterFX,$Queue,$Template,$Audio,$Profile)) { if (!(Test-Path -LiteralPath $path)) { throw "Missing: $path" } }
if ([IO.File]::ReadAllText($Profile) -match '(?m)^[^#\r\n]*=UNRESOLVED\s*$') {
    throw 'Profile geometry still contains UNRESOLVED. No AfterFX process was launched.'
}
if ((Get-Process -Name AfterFX -ErrorAction SilentlyContinue)) {
    throw 'Close existing AfterFX processes before using this one-job dedicated-process launcher.'
}
if ((Get-ChildItem -LiteralPath $Queue -Filter '*.running')) { throw 'Queue contains an interrupted job; it will not be retried automatically.' }
if ((Get-ChildItem -LiteralPath $Queue -Filter '*.request')) { throw 'Queue already contains a pending job.' }
if ((Get-ChildItem -LiteralPath $Queue | Where-Object { $_.Name.StartsWith($JobId + '.', [StringComparison]::OrdinalIgnoreCase) })) { throw 'JobId already has artifacts. Use a new ID.' }
foreach ($path in @($PreparedProject,$OutputProject)) { if (Test-Path -LiteralPath $path) { throw "Refusing to overwrite: $path" } }
$inv = [Globalization.CultureInfo]::InvariantCulture
$fields = [ordered]@{
    job_id=$JobId; queue=$Queue; template=$Template; audio=$Audio; audio_item=$AudioItemName
    comp=$Composition; soundkeys_layer=$SoundKeysLayer; effect_index=$EffectIndex
    profile=$Profile; prepared=$PreparedProject; output=$OutputProject
    work_start=$WorkStart.ToString('R',$inv); work_duration=$WorkDuration.ToString('R',$inv)
}
$lines = foreach ($p in $fields.GetEnumerator()) {
    $v = [string]$p.Value
    if ($v -match '[\r\n\x00]' -or $v -ne $v.Trim()) { throw "Invalid protocol value: $($p.Key)" }
    "$($p.Key)=$v"
}
$config = Join-Path $Queue "$JobId.prep-config.txt"
[IO.File]::WriteAllText($config, ($lines -join "`n") + "`n", (New-Object Text.UTF8Encoding($false)))
$prep = Join-Path $PSScriptRoot 'PrepareAndSubmit.jsx'
$psi = New-Object Diagnostics.ProcessStartInfo
$psi.FileName = $AfterFX
$psi.Arguments = '-r "' + $prep + '"'
$psi.UseShellExecute = $false
$psi.EnvironmentVariables['SOUNDKEYS_BRIDGE_QUEUE'] = $Queue
$psi.EnvironmentVariables['SOUNDKEYS_BRIDGE_PREP_CONFIG'] = $config
$process = [Diagnostics.Process]::Start($psi)
$watch = [Diagnostics.Stopwatch]::StartNew()
$resultPath = Join-Path $Queue "$JobId.result.json"
$prepError = Join-Path $Queue "$JobId.prep-error.txt"
while ($watch.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
    if (Test-Path -LiteralPath $resultPath) {
        $result = Get-Content -Raw -LiteralPath $resultPath -Encoding UTF8 | ConvertFrom-Json
        if ($result.job_id -ne $JobId) { throw 'Mismatched result job_id' }
        if ($result.status -ne 'succeeded') { throw "AE job failed: $($result.code): $($result.message)" }
        if (!(Test-Path -LiteralPath $OutputProject)) { throw 'Success result exists but output project is missing' }
        $result
        return
    }
    if (Test-Path -LiteralPath $prepError) { throw (Get-Content -Raw -LiteralPath $prepError -Encoding UTF8) }
    if ($process.HasExited) { throw "AfterFX exited with code $($process.ExitCode); job state is unknown. No automatic retry." }
    Start-Sleep -Milliseconds 250
}
throw "Timed out. AfterFX may still be executing Sound Keys. No cancellation or retry was attempted. Inspect $Queue."
