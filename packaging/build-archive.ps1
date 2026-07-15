#requires -Version 5.1
<#
.SYNOPSIS
  Assemble the OSF Animation FOMOD release archive from build/ (DLL) + dist/ (scripts).
.EXAMPLE
  packaging\build-archive.ps1 -Version 0.2.0
  # -> packaging\out\OSF Animation v0.2.0.zip
.NOTES
  Run AFTER a verified `xmake` build and a Papyrus compile (so dist/Scripts/*.pex exist).
  Ship the flavor you tested in-game (default releasedbg); the .pdb is never included.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $Version,
    [ValidateSet('releasedbg', 'release')] [string] $DllFlavor = 'releasedbg',
    [string] $OutDir = "$PSScriptRoot\out"
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$dll  = Join-Path $repo "build\windows\x64\$DllFlavor\OSF Animation.dll"
$dist = Join-Path $repo 'dist'
if (-not (Test-Path $dll)) { throw "DLL not found: $dll  (run xmake first; -DllFlavor $DllFlavor)" }

# --- stage Core ---
$stage = Join-Path $env:TEMP "osfanim-pkg-$(Get-Random)"
$core = "$stage\Core"
@("$core\SFSE\Plugins", "$core\Scripts\Source") |
    ForEach-Object { New-Item -ItemType Directory -Force -Path $_ | Out-Null }

# Core: the DLL (no .pdb) + the public OSF API scripts/sources (consumers need the .psc to compile
# against). OSFTest (the console smoke-test harness) is DEV-ONLY and deliberately excluded from release.
Copy-Item $dll "$core\SFSE\Plugins\"
# OSFCompat is the non-public compat-natives script the DLL always binds; shipped in Core so the
# SAF shim (opt-in below) can resolve OSFCompat.* at runtime. Harmless when the shim isn't installed.
foreach ($s in @('OSF', 'OSFTypes', 'OSFAdvanced', 'OSFCompat')) {
    if (Test-Path "$dist\Scripts\$s.pex")        { Copy-Item "$dist\Scripts\$s.pex" "$core\Scripts\" }
    if (Test-Path "$dist\Scripts\Source\$s.psc") { Copy-Item "$dist\Scripts\Source\$s.psc" "$core\Scripts\Source\" }
}

# --- stage SAF Compatibility (opt-in FOMOD group) ---
# The SAF/SAFScript backwards-compat shim: pure Papyrus that delegates to OSF's natives.
# Installed only if the user picks the "SAF Compatibility" group; shares SAF's script names,
# so it's mutually exclusive with a standalone SAF install. Both .pex are required (an empty
# component would silently no-op), so fail loudly if the compile step was skipped.
$safc = "$stage\SafCompat"
New-Item -ItemType Directory -Force -Path "$safc\Scripts\Source" | Out-Null
foreach ($s in @('SAF', 'SAFScript')) {
    if (-not (Test-Path "$dist\Scripts\$s.pex")) {
        throw "SAF shim missing: $dist\Scripts\$s.pex (compile dist\Scripts\Source\$s.psc first)"
    }
    Copy-Item "$dist\Scripts\$s.pex" "$safc\Scripts\"
    Copy-Item "$dist\Scripts\Source\$s.psc" "$safc\Scripts\Source\"
}

# Settings + hotkeys live in OSF UI's in-game settings menu (schema registered at
# runtime over the bridge) — no Data/OSF/settings.json ships anymore.
New-Item -ItemType Directory -Force -Path "$core\OSF" | Out-Null

# internal.osf.json -> Data/OSF/internal.osf.json. System scenes the framework needs at runtime:
# the player-only "solo" scene backs OSF.Health, plus the "pair" smoke-test. Always shipped so the
# health self-test works on a clean install (scenes are looked up by id, independent of filename).
Copy-Item "$dist\OSF\internal.osf.json" "$core\OSF\internal.osf.json"

# fomod/ (incl. fomod/images/ banner + plugin art referenced by ModuleConfig.xml) + stamp the version into info.xml.
Copy-Item "$PSScriptRoot\fomod" "$stage\fomod" -Recurse
$infoPath = "$stage\fomod\info.xml"
$mver = ($Version -replace '[^0-9.].*$', '')   # 0.1.0-beta -> 0.1.0
(Get-Content $infoPath -Raw) -replace '<Version\b.*?</Version>', "<Version MachineVersion=`"$mver`">$Version</Version>" |
    Set-Content $infoPath -Encoding UTF8

# --- zip ---
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$zip = Join-Path $OutDir "OSF Animation v$Version.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path "$stage\*" -DestinationPath $zip
Remove-Item $stage -Recurse -Force

Write-Host "Built ($DllFlavor): $zip"
Get-ChildItem $OutDir -Filter "OSF Animation v$Version.zip" |
    Select-Object Name, @{ n = 'MB'; e = { [math]::Round($_.Length / 1MB, 2) } }
