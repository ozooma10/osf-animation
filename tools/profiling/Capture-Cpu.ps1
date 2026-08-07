#requires -Version 5.1
#requires -RunAsAdministrator
<#
.SYNOPSIS
  Capture a symbolized CPU-sampling trace of a running Starfield session.
#>
[CmdletBinding()]
param(
    [string] $Label = 'cpu',
    [switch] $NoOpen
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$wpr = Get-Command wpr.exe -ErrorAction Stop
$wpa = Get-Command wpa.exe -ErrorAction Stop
$starfield = @(Get-Process -Name Starfield -ErrorAction SilentlyContinue)
if ($starfield.Count -ne 1) {
    throw "Expected exactly one running Starfield process; found $($starfield.Count)."
}

$safeLabel = ($Label -replace '[^A-Za-z0-9._-]', '-').Trim('-')
if (-not $safeLabel) { $safeLabel = 'cpu' }
$captureDir = Join-Path $repo 'build\profiles'
New-Item -ItemType Directory -Force -Path $captureDir | Out-Null
$timestamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$etl = Join-Path $captureDir "$timestamp-$safeLabel.etl"
$session = 'OSFAnimationCpu'

$status = (& $wpr.Source -status -instancename $session 2>&1 | Out-String)
if ($LASTEXITCODE -eq 0 -and $status -match 'recording is in progress') {
    throw "WPR session '$session' is already running. Stop or cancel it before starting another capture."
}

$started = $false
try {
    Write-Host 'Starting WPR CPU.Verbose capture...' -ForegroundColor Cyan
    & $wpr.Source -start CPU.Verbose -filemode -instancename $session
    if ($LASTEXITCODE -ne 0) { throw "wpr -start failed with exit code $LASTEXITCODE." }
    $started = $true

    Write-Host 'Reproduce the behavior in Starfield for 10-20 seconds.' -ForegroundColor Green
    Read-Host 'Press Enter to stop and save the capture'

    & $wpr.Source -stop $etl "OSF Animation CPU profile: $safeLabel" -compress -instancename $session
    if ($LASTEXITCODE -ne 0) { throw "wpr -stop failed with exit code $LASTEXITCODE." }
    $started = $false
    if (-not (Test-Path -LiteralPath $etl)) {
        throw "WPR reported success but did not create the requested trace: $etl"
    }
    if ((Get-Item -LiteralPath $etl).Length -le 0) {
        throw "WPR created an empty trace and WPA will not be launched: $etl"
    }
} catch {
    if ($started) {
        & $wpr.Source -cancel -instancename $session | Out-Null
    }
    throw
}

$mods = $env:XSE_SF_MODS_PATH
$symbolPaths = @()
if ($mods) {
    $symbolPaths += Join-Path $mods 'OSF Animation Profiling\SFSE\Plugins'
    $symbolPaths += Join-Path $mods 'OSF Animation\SFSE\Plugins'
}
$symbolPaths += Join-Path $repo 'build\profile\windows\x64\releasedbg'
$symbolPaths += Join-Path $repo 'build\windows\x64\releasedbg'
$symbolPaths = @($symbolPaths | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -Unique)
$symbolCache = Join-Path $repo 'build\symbols'
New-Item -ItemType Directory -Force -Path $symbolCache | Out-Null
$symbolPaths += "srv*$symbolCache*https://msdl.microsoft.com/download/symbols"
$env:_NT_SYMBOL_PATH = $symbolPaths -join ';'
$env:_NT_SYMCACHE_PATH = Join-Path $repo 'build\symcache'

Write-Host "Saved $etl" -ForegroundColor Green
Write-Host "WPA symbol path: $env:_NT_SYMBOL_PATH"
if (-not $NoOpen) {
    Start-Process -FilePath $wpa.Source -ArgumentList ('"{0}"' -f $etl)
}
