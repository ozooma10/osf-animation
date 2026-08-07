#requires -Version 5.1
<#
.SYNOPSIS
  Build and deploy the development-only Tracy DLL override, then restore normal xmake configuration.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$xmake = Get-Command xmake -ErrorAction Stop
$mods = $env:XSE_SF_MODS_PATH
if (-not $mods) { throw 'XSE_SF_MODS_PATH is not set; cannot locate the MO2 mods directory.' }
if (Get-Process -Name Starfield -ErrorAction SilentlyContinue) {
    throw 'Starfield is running. Close it before replacing the profiling override DLL/PDB.'
}

$baseDll = Join-Path $mods 'OSF Animation\SFSE\Plugins\OSF Animation.dll'
if (-not (Test-Path -LiteralPath $baseDll)) {
    throw "The normal OSF Animation DLL is missing: $baseDll"
}
$baseHashBefore = (Get-FileHash -LiteralPath $baseDll -Algorithm SHA256).Hash
$profileRoot = Join-Path $mods 'OSF Animation Profiling'
$deployedDll = Join-Path $profileRoot 'SFSE\Plugins\OSF Animation.dll'
$deployedPdb = [IO.Path]::ChangeExtension($deployedDll, 'pdb')
$builtDll = Join-Path $repo 'build\profile\windows\x64\releasedbg\OSF Animation.dll'
$marker = 'OSF_TRACY_PROFILE_BUILD'

function Invoke-Xmake([string[]] $Arguments, [string] $Failure) {
    & $xmake.Source @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Failure (exit $LASTEXITCODE)." }
}

function Contains-Marker([string] $Path, [string] $Text) {
    $ascii = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($Path))
    return $ascii.Contains($Text)
}

$failure = $null
Push-Location $repo
try {
    try {
        Invoke-Xmake @('f', '-c', '-m', 'releasedbg', '-P', '.', '-o', 'build/profile', '--osf_profiler=y', '-y') 'Profiling configuration failed'
        Invoke-Xmake @('-r', '-P', '.', '-y', 'OSF Animation Profiling') 'Profiling build failed'

        foreach ($required in @($builtDll, $deployedDll, $deployedPdb, (Join-Path $profileRoot 'meta.ini'))) {
            if (-not (Test-Path -LiteralPath $required)) { throw "Profiling output is missing: $required" }
        }
        if (-not (Contains-Marker $deployedDll $marker)) {
            throw "Deployed profiling DLL does not contain marker '$marker'."
        }
        if ((Get-FileHash -LiteralPath $builtDll -Algorithm SHA256).Hash -ne
            (Get-FileHash -LiteralPath $deployedDll -Algorithm SHA256).Hash) {
            throw 'Built and deployed profiling DLL hashes do not match.'
        }
        if ((Get-FileHash -LiteralPath $baseDll -Algorithm SHA256).Hash -ne $baseHashBefore) {
            throw 'The normal OSF Animation DLL changed during the profiling build.'
        }

        $allowed = @(
            'meta.ini',
            'SFSE/Plugins/OSF Animation.dll',
            'SFSE/Plugins/OSF Animation.pdb'
        )
        $rootPrefix = [IO.Path]::GetFullPath($profileRoot).TrimEnd('\') + '\'
        $actual = @(Get-ChildItem -LiteralPath $profileRoot -Recurse -File | ForEach-Object {
            $_.FullName.Substring($rootPrefix.Length).Replace('\', '/')
        })
        $unexpected = @($actual | Where-Object { $_ -notin $allowed })
        if ($unexpected) {
            throw "Profiling override contains unexpected file(s): $($unexpected -join ', ')"
        }
    } catch {
        $failure = $_
    } finally {
        try {
            Invoke-Xmake @('f', '-c', '-m', 'releasedbg', '-P', '.', '-o', 'build', '--osf_profiler=n', '-y') 'Could not restore normal xmake configuration'
        } catch {
            if ($failure) {
                Write-Error $_
            } else {
                $failure = $_
            }
        }
    }
} finally {
    Pop-Location
}

if ($failure) { throw $failure }
Write-Host 'Profiling override built and verified.' -ForegroundColor Green
Write-Host "  $deployedDll"
Write-Host 'Enable OSF Animation Profiling at higher conflict priority than OSF Animation, then restart Starfield.'
