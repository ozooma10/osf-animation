#requires -Version 7.2
<#
.SYNOPSIS
  Build and deploy the development-only profiler override DLL, then restore the normal xmake configuration.
.DESCRIPTION
  Configures `--profiling=y` into build/profile, builds only the engine target, and deploys exactly
  meta.ini + OSF Animation.dll + .pdb to "<mods>\OSF Animation Profiling". Enable that MO2 mod at a
  higher conflict priority than "OSF Animation"; it inherits every other file from the normal mod.
  The normal mod is never written, and the script verifies its DLL hash is unchanged.
  See docs/PROFILING.md.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
. (Join-Path $repo 'tools/ReleaseRules.ps1')

$xmake = Get-Command xmake -ErrorAction Stop
$mods = $env:XSE_SF_MODS_PATH
if (-not $mods) { throw 'XSE_SF_MODS_PATH is not set; cannot locate the MO2 mods directory.' }
if (Get-Process -Name Starfield -ErrorAction SilentlyContinue) {
    throw 'Starfield is running. Close it before replacing the profiling override DLL/PDB.'
}

$baseDll = Join-Path $mods 'OSF Animation\SFSE\Plugins\OSF Animation.dll'
$baseHashBefore = if (Test-Path -LiteralPath $baseDll) { (Get-FileHash -LiteralPath $baseDll).Hash } else { $null }
$profileRoot = Join-Path $mods 'OSF Animation Profiling'
$deployedDll = Join-Path $profileRoot 'SFSE\Plugins\OSF Animation.dll'
$deployedPdb = [IO.Path]::ChangeExtension($deployedDll, 'pdb')
$builtDll = Join-Path $repo 'build\profile\windows\x64\releasedbg\OSF Animation.dll'

function Invoke-Xmake([string[]]$Arguments, [string]$Failure) {
    & $xmake.Source @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Failure (exit $LASTEXITCODE)." }
}

$failure = $null
Push-Location $repo
try {
    try {
        Invoke-Xmake @('f', '-y', '-p', 'windows', '-a', 'x64', '-m', 'releasedbg', '-o', 'build/profile',
            '--profiling=y', '--animation_smoke=n') 'Profiling configuration failed'
        # Only the engine target: a plain `xmake` would also rebuild and deploy the Browser DLL.
        Invoke-Xmake @('build', '-y', 'OSF Animation') 'Profiling build failed'

        foreach ($required in @($builtDll, $deployedDll, $deployedPdb, (Join-Path $profileRoot 'meta.ini'))) {
            if (-not (Test-Path -LiteralPath $required)) { throw "Profiling output is missing: $required" }
        }
        if (-not (Test-InstrumentedDll $deployedDll)) {
            throw 'The deployed DLL does not carry the profiling build marker.'
        }
        if ((Get-FileHash -LiteralPath $builtDll).Hash -ne (Get-FileHash -LiteralPath $deployedDll).Hash) {
            throw 'Built and deployed profiling DLL hashes differ.'
        }
        $baseHashAfter = if (Test-Path -LiteralPath $baseDll) { (Get-FileHash -LiteralPath $baseDll).Hash } else { $null }
        if ($baseHashAfter -ne $baseHashBefore) {
            throw 'The normal OSF Animation DLL changed during the profiling build.'
        }

        $allowed = @('meta.ini', 'SFSE/Plugins/OSF Animation.dll', 'SFSE/Plugins/OSF Animation.pdb')
        $prefix = [IO.Path]::GetFullPath($profileRoot).TrimEnd('\') + '\'
        $actual = @(Get-ChildItem -LiteralPath $profileRoot -Recurse -File | ForEach-Object {
            $_.FullName.Substring($prefix.Length).Replace('\', '/')
        })
        $unexpected = @($actual | Where-Object { $_ -notin $allowed })
        if ($unexpected.Count) { throw "Profiling override contains unexpected file(s): $($unexpected -join ', ')" }
    } catch {
        $failure = $_
    } finally {
        try {
            Invoke-Xmake @('f', '-y', '-p', 'windows', '-a', 'x64', '-m', 'releasedbg', '-o', 'build',
                '--profiling=n', '--animation_smoke=n') 'Could not restore the normal xmake configuration'
        } catch {
            if ($failure) { Write-Error $_ -ErrorAction Continue } else { $failure = $_ }
        }
    }
} finally {
    Pop-Location
}

if ($failure) { throw $failure }
Write-Host 'Profiling override built and verified.' -ForegroundColor Green
Write-Host "  $deployedDll"
Write-Host 'In MO2, enable "OSF Animation Profiling" below (higher priority than) "OSF Animation", then start the game.'
Write-Host 'Disable it again after profiling. See docs/PROFILING.md.'
