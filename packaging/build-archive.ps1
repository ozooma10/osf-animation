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
foreach ($s in @('OSF', 'OSFTypes', 'OSFAdvanced')) {
    if (Test-Path "$dist\Scripts\$s.pex")        { Copy-Item "$dist\Scripts\$s.pex" "$core\Scripts\" }
    if (Test-Path "$dist\Scripts\Source\$s.psc") { Copy-Item "$dist\Scripts\Source\$s.psc" "$core\Scripts\Source\" }
}

# The scene-browser view (committed Vite production build). OSF UI discovers views across
# the VFS at <OSFUI.dll dir>\OSFUI\views\<modId>\<viewName>\ — the view ships from THIS mod,
# no copy lives in OSF UI. Without it the browser/wheel UI simply doesn't exist, so fail
# loudly. Copy the complete output (hashed fonts, source map) to match the xmake deploy.
$viewSrc = Join-Path $repo 'views\osf.animation\browser'
if (-not (Test-Path "$viewSrc\index.html")) { throw "Browser view missing: $viewSrc (run npm run build in ui/animation-browser)" }
$viewDst = "$core\SFSE\Plugins\OSFUI\views\osf.animation\browser"
New-Item -ItemType Directory -Force -Path $viewDst | Out-Null
Copy-Item "$viewSrc\*" $viewDst -Recurse

# Settings + hotkeys live in OSF UI's in-game settings menu (schema registered at
# runtime over the bridge) — no Data/OSF/settings.json ships anymore.
New-Item -ItemType Directory -Force -Path "$core\OSF" | Out-Null

# internal.osf.json -> Data/OSF/internal.osf.json. System scenes the framework needs at runtime:
# the player-only "solo" scene backs OSF.Health, plus the "pair" smoke-test. Always shipped so the
# health self-test works on a clean install (scenes are looked up by id, independent of filename).
Copy-Item "$dist\OSF\internal.osf.json" "$core\OSF\internal.osf.json"

# --- stage Animation Library (opt-in FOMOD group "Animation Library") ---
# The browsable vanilla + creature catalog: pure JSON packs that reference game-archive
# .af clips (no baked animation data, ~2.3 MB). The browser's library lane is empty without
# these, so fail loudly if the generated packs are missing.
$lib = "$stage\Library\OSF\vanilla"
New-Item -ItemType Directory -Force -Path $lib | Out-Null
$libPacks = Get-ChildItem "$dist\OSF\vanilla" -Filter '*.osf.json' -ErrorAction SilentlyContinue
if (-not $libPacks) { throw "Animation library packs missing: $dist\OSF\vanilla\*.osf.json (generate with tools/vanilla-packs first)" }
$libPacks | ForEach-Object { Copy-Item $_.FullName $lib }

# --- stage Immersion Actions (opt-in FOMOD group "Immersion Actions") ---
# Emote-wheel content: the player.emote.* scenes the wheel enumerates. Required for the
# group to be non-empty, so fail loudly if absent.
$imm = "$stage\Immersion\OSF\immersion"
New-Item -ItemType Directory -Force -Path $imm | Out-Null
if (-not (Test-Path "$dist\OSF\immersion\emotes.osf.json")) {
    throw "Immersion pack missing: $dist\OSF\immersion\emotes.osf.json"
}
Copy-Item "$dist\OSF\immersion\emotes.osf.json" $imm

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
