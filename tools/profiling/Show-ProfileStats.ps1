#requires -Version 7.2
<#
.SYNOPSIS
  Summarize an OSF Animation profiler stats dump, optionally against a baseline dump.
.DESCRIPTION
  Reads <stamp>-<label>.stats.json written by the profiling override (docs/PROFILING.md). -Path may be
  a file or a directory; a directory selects its newest *.stats.json. -Compare adds per-scope deltas
  against a second dump (for before/after checks of an optimization).
.EXAMPLE
  .\tools\profiling\Show-ProfileStats.ps1 -Path "$([Environment]::GetFolderPath('MyDocuments'))\My Games\Starfield\SFSE\OSF\profiles"
.EXAMPLE
  .\tools\profiling\Show-ProfileStats.ps1 -Path after.stats.json -Compare before.stats.json -Category Anim -Top 15
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Path,
    [string]$Compare,
    [ValidateRange(1, 1000)][int]$Top = 25,
    [string]$Category,
    [ValidateSet('total', 'max', 'mean', 'count', 'allocs', 'perFrame')][string]$SortBy = 'total',
    [switch]$AsJson
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Resolve-StatsFile([string]$Candidate) {
    $full = [IO.Path]::GetFullPath($Candidate)
    if (Test-Path -LiteralPath $full -PathType Container) {
        $latest = Get-ChildItem -LiteralPath $full -Filter '*.stats.json' -File | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
        if (-not $latest) { throw "No *.stats.json in $full" }
        return $latest.FullName
    }
    if (-not (Test-Path -LiteralPath $full -PathType Leaf)) { throw "Stats file not found: $full" }
    return $full
}

function Read-Stats([string]$File) {
    $stats = Get-Content -LiteralPath $File -Raw | ConvertFrom-Json -AsHashtable
    if ($stats.schema -ne 1 -or -not $stats.ContainsKey('scopes')) { throw "Not an OSF profiler stats file (schema 1): $File" }
    return $stats
}

function Format-Ms([double]$Ns) { [Math]::Round($Ns / 1e6, 3) }
function Format-Us([double]$Ns) { [Math]::Round($Ns / 1e3, 2) }
function Format-Delta($Now, $Before) {
    if ($null -eq $Before -or $Before -eq 0) { return $null }
    [Math]::Round(100.0 * ($Now - $Before) / $Before, 1)
}

$file = Resolve-StatsFile $Path
$stats = Read-Stats $file
$baseline = $null
$baselineFile = $null
if ($Compare) {
    $baselineFile = Resolve-StatsFile $Compare
    $baseline = Read-Stats $baselineFile
}

$baselineScopes = @{}
if ($baseline) {
    foreach ($scope in $baseline.scopes) { $baselineScopes["$($scope.name)|$($scope.kind)"] = $scope }
}

$rows = foreach ($scope in $stats.scopes) {
    if ($Category -and $scope.category -ne $Category) { continue }
    $before = $baselineScopes["$($scope.name)|$($scope.kind)"]
    $row = [ordered]@{
        Name = $scope.name
        Kind = $scope.kind
        Count = $scope.count
        PerFrame = [Math]::Round([double]$scope.callsPerFrame, 2)
        TotalMs = Format-Ms $scope.totalNs
        MeanUs = Format-Us $scope.meanNs
        MaxUs = Format-Us $scope.maxNs
        AllocsPerCall = [Math]::Round([double]$scope.allocsPerCall, 2)
    }
    if ($baseline) {
        $row.DeltaTotalPct = if ($before) { Format-Delta $scope.totalNs $before.totalNs } else { 'new' }
        $row.DeltaMeanPct = if ($before) { Format-Delta $scope.meanNs $before.meanNs } else { 'new' }
    }
    [pscustomobject]$row
}
$sortKey = @{ total = 'TotalMs'; max = 'MaxUs'; mean = 'MeanUs'; count = 'Count'; allocs = 'AllocsPerCall'; perFrame = 'PerFrame' }[$SortBy]
$rows = @($rows | Sort-Object -Property $sortKey -Descending | Select-Object -First $Top)

if ($AsJson) {
    [ordered]@{ file = $file; baseline = $baselineFile; capture = $stats.capture; frames = $stats.frames; scopes = $rows; counters = $stats.counters } |
        ConvertTo-Json -Depth 8
    return
}

$capture = $stats.capture
# ConvertFrom-Json turns ISO-8601 strings into DateTime; print them back as UTC ISO-8601.
$startedAt = $capture.startedAt
if ($startedAt -is [datetime]) { $startedAt = $startedAt.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ss.fffZ') }
Write-Host "$($stats.plugin) $($stats.dllVersion) on Starfield $($stats.gameVersion)" -ForegroundColor Cyan
Write-Host "  dump '$($stats.label)' ($($stats.mode), $($capture.reason)): $([Math]::Round($capture.durationMs / 1000, 1)) s from $startedAt"
Write-Host "  $file"
if ($baselineFile) { Write-Host "  compared with $baselineFile" }

$frames = $stats.frames
$frameTable = @(
    [pscustomobject]@{ Measure = 'Frame ms'; Mean = [Math]::Round($frames.frameMs.mean, 3); P50 = [Math]::Round($frames.frameMs.p50, 3); P95 = [Math]::Round($frames.frameMs.p95, 3); P99 = [Math]::Round($frames.frameMs.p99, 3); Max = [Math]::Round($frames.frameMs.max, 3) }
    [pscustomobject]@{ Measure = 'OSF ms/frame'; Mean = [Math]::Round($frames.osfMsPerFrame.mean, 3); P50 = [Math]::Round($frames.osfMsPerFrame.p50, 3); P95 = [Math]::Round($frames.osfMsPerFrame.p95, 3); P99 = [Math]::Round($frames.osfMsPerFrame.p99, 3); Max = [Math]::Round($frames.osfMsPerFrame.max, 3) }
)
if ($baseline) {
    $b = $baseline.frames
    $frameTable += [pscustomobject]@{ Measure = 'Baseline frame ms'; Mean = [Math]::Round($b.frameMs.mean, 3); P50 = [Math]::Round($b.frameMs.p50, 3); P95 = [Math]::Round($b.frameMs.p95, 3); P99 = [Math]::Round($b.frameMs.p99, 3); Max = [Math]::Round($b.frameMs.max, 3) }
    $frameTable += [pscustomobject]@{ Measure = 'Baseline OSF ms/frame'; Mean = [Math]::Round($b.osfMsPerFrame.mean, 3); P50 = [Math]::Round($b.osfMsPerFrame.p50, 3); P95 = [Math]::Round($b.osfMsPerFrame.p95, 3); P99 = [Math]::Round($b.osfMsPerFrame.p99, 3); Max = [Math]::Round($b.osfMsPerFrame.max, 3) }
}
Write-Host ''
Write-Host "Frames: $($frames.count) timed, $($frames.gaps) gap(s) totalling $([Math]::Round($frames.gapMs)) ms; OSF share of frame mean $([Math]::Round(100 * $frames.osfShareOfFrame.mean, 2))%, p95 $([Math]::Round(100 * $frames.osfShareOfFrame.p95, 2))%" -ForegroundColor Cyan
$frameTable | Format-Table -AutoSize | Out-Host

Write-Host "Scopes (top $Top by $SortBy$(if ($Category) { ", category $Category" }))" -ForegroundColor Cyan
if ($rows.Count) { $rows | Format-Table -AutoSize | Out-Host } else { Write-Host '  (none)' }

$counters = @($stats.counters | Where-Object { -not $Category -or $_.name -like "$Category.*" })
if ($counters.Count) {
    Write-Host 'Counters and gauges' -ForegroundColor Cyan
    $counters | ForEach-Object {
        if ($_.kind -eq 'counter') {
            [pscustomobject]@{ Name = $_.name; Kind = 'counter'; Total = $_.total; Samples = $_.increments; Min = $null; Max = $null; Last = $null }
        } else {
            [pscustomobject]@{ Name = $_.name; Kind = 'gauge'; Total = $null; Samples = $_.samples; Min = $_.min; Max = $_.max; Last = $_.last }
        }
    } | Format-Table -AutoSize | Out-Host
}

$warnings = @()
if ($capture.ringWrapped) { $warnings += 'A trace ring wrapped: the timeline starts later than the window on at least one thread (raise ringEvents).' }
if ($capture.tornEvents) { $warnings += "$($capture.tornEvents) trace event(s) were mid-write during capture and skipped." }
if ($capture.droppedDescs) { $warnings += "$($capture.droppedDescs) instrumented site(s) exceeded the descriptor registry and were not recorded." }
if ($capture.droppedThreads) { $warnings += "$($capture.droppedThreads) thread(s) exceeded the thread registry and were not recorded." }
foreach ($warning in $warnings) { Write-Warning $warning }
