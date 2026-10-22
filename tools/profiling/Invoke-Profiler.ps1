#requires -Version 7.2
<#
.SYNOPSIS
  Send a command to the OSF Animation profiler override running in Starfield and wait for its result.
.DESCRIPTION
  Writes <Documents>\My Games\Starfield\SFSE\OSF\profiles\command.json (atomically) and waits for the
  game to acknowledge it in status.json. The game reads commands about once a second from its frame
  hook, and Starfield pauses while unfocused, so focus the game window (or pass -Focus).

  Commands:
    start   reset the window and record in -Mode stats|trace (default stats)
    stop    stop recording (frames are still timed); data is kept until start/reset
    dump    write <stamp>-<label>.stats.json (+ .trace.json.gz in trace mode); -Reset starts a new window
    mark    add a labelled instant to the trace timeline
    reset   start a new capture window without changing the mode
    status  only report the current state
  See docs/PROFILING.md.
.EXAMPLE
  .\tools\profiling\Invoke-Profiler.ps1 -Command start -Mode trace -Label orbit -Focus
.EXAMPLE
  .\tools\profiling\Invoke-Profiler.ps1 -Command dump -Label orbit -Show
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('start', 'stop', 'dump', 'mark', 'reset', 'status')][string]$Command,
    [ValidateSet('stats', 'trace')][string]$Mode,
    [string]$Label,
    [ValidateSet('on', 'off')][string]$AutoDump,
    [ValidateRange(1024, 4194304)][int]$RingEvents,
    [switch]$Reset,
    [switch]$Focus,
    [switch]$Show,
    [ValidateRange(1, 600)][int]$TimeoutSec = 60,
    [string]$Directory = (Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'My Games\Starfield\SFSE\OSF\profiles')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$game = @(Get-Process -Name Starfield -ErrorAction SilentlyContinue)
if ($game.Count -ne 1) { throw "Expected exactly one running Starfield process; found $($game.Count)." }
if (-not (Test-Path -LiteralPath $Directory -PathType Container)) {
    throw "No profiler directory at $Directory. Is the 'OSF Animation Profiling' override enabled in MO2 (the SFSE log says '[Profile] OSFAnimationProfilingBuild')?"
}

$statusPath = Join-Path $Directory 'status.json'
function Read-Status {
    for ($attempt = 0; $attempt -lt 5; $attempt++) {
        try {
            if (-not (Test-Path -LiteralPath $statusPath)) { return $null }
            return Get-Content -LiteralPath $statusPath -Raw | ConvertFrom-Json -AsHashtable
        } catch { Start-Sleep -Milliseconds 50 }  # replaced mid-read
    }
    return $null
}

$before = Read-Status
$previousSequence = if ($before) { [int64]$before.sequence } else { -1 }
$previousDumpAt = if ($before -and $before.lastDump) { [string]$before.lastDump.at } else { '' }

$request = [ordered]@{ command = $Command }
if ($Mode) { $request.mode = $Mode }
if ($Label) { $request.label = $Label }
if ($AutoDump) { $request.autoDump = $AutoDump -eq 'on' }
if ($RingEvents) { $request.ringEvents = $RingEvents }
if ($Reset) { $request.reset = $true }

$commandPath = Join-Path $Directory 'command.json'
if (Test-Path -LiteralPath $commandPath) { throw "A previous command is still waiting in $commandPath. Focus the game, or delete the file." }
$temporary = "$commandPath.tmp"
$request | ConvertTo-Json -Compress | Set-Content -LiteralPath $temporary -Encoding utf8NoBOM
Move-Item -LiteralPath $temporary -Destination $commandPath -Force

if ($Focus) {
    $null = (New-Object -ComObject WScript.Shell).AppActivate($game[0].Id)
} else {
    Write-Host 'Waiting for the game to read the command. Starfield pauses while unfocused: switch to it (or use -Focus).' -ForegroundColor DarkGray
}

$deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSec)
$status = $null
do {
    Start-Sleep -Milliseconds 250
    if (-not (Get-Process -Id $game[0].Id -ErrorAction SilentlyContinue)) { throw 'Starfield exited before acknowledging the command.' }
    $current = Read-Status
    if ($current -and [int64]$current.sequence -gt $previousSequence -and $current.lastCommand -and $current.lastCommand.command -eq $Command) {
        $status = $current
    }
} while (-not $status -and [DateTime]::UtcNow -lt $deadline)
if (-not $status) {
    throw "No acknowledgement within $TimeoutSec s. Is the game focused and in gameplay (not on a loading screen)? command.json is left in place."
}
if (-not $status.lastCommand.ok) { throw "Profiler rejected '$Command': $($status.lastCommand.error)" }

if ($Command -eq 'dump') {
    # The dump is serialized on a background thread; wait for its completion record.
    while (-not ($status.lastDump -and [string]$status.lastDump.at -ne $previousDumpAt) -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 250
        $current = Read-Status
        if ($current) { $status = $current }
    }
    if (-not ($status.lastDump -and [string]$status.lastDump.at -ne $previousDumpAt)) { throw "The dump did not finish within $TimeoutSec s; check the SFSE log for '[Profile]'." }
    if ($status.lastDump.error) { throw "Dump failed: $($status.lastDump.error)" }
}

Write-Host "Profiler: mode $($status.mode), label '$($status.label)', auto-dump $(if ($status.autoDump) { 'on' } else { 'off' }), ring $($status.ringEvents) events/thread" -ForegroundColor Green
Write-Host "  build $($status.build) $($status.dllVersion), game $($status.gameVersion), window since $($status.captureStartedAt)"
if ($status.lastDump) {
    Write-Host "  last dump '$($status.lastDump.label)' ($($status.lastDump.reason)) at $($status.lastDump.at)"
    if ($status.lastDump.stats) { Write-Host "    stats: $($status.lastDump.stats)" }
    if ($status.lastDump.trace) { Write-Host "    trace: $($status.lastDump.trace)  (open in https://ui.perfetto.dev)" }
}
if ($Show -and $Command -eq 'dump' -and $status.lastDump.stats) {
    & (Join-Path $PSScriptRoot 'Show-ProfileStats.ps1') -Path $status.lastDump.stats
}
