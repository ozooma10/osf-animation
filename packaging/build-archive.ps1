#requires -Version 5.1
<#
.SYNOPSIS
  Assemble the OSF Animation FOMOD release archive from build/ (DLL) + dist/ (scripts).
.EXAMPLE
  packaging\build-archive.ps1 -Version 0.1.0
  # -> packaging\out\OSF Animation v0.1.0.zip
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

# Core: the DLL (no .pdb) + the OSF API scripts/sources. A consumer needs the .psc to compile
# against; OSFTest is the console smoke-test harness referenced by docs/GETTING_STARTED.md.
Copy-Item $dll "$core\SFSE\Plugins\"
foreach ($s in @('OSF', 'OSFTypes', 'OSFTest')) {
    if (Test-Path "$dist\Scripts\$s.pex")        { Copy-Item "$dist\Scripts\$s.pex" "$core\Scripts\" }
    if (Test-Path "$dist\Scripts\Source\$s.psc") { Copy-Item "$dist\Scripts\Source\$s.psc" "$core\Scripts\Source\" }
}

# fomod/ + stamp the version into info.xml.
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
