#requires -Version 5.1
<#
.SYNOPSIS
  Build OSF Animation and assemble the FOMOD release archive (same pipeline shape as
  OSF UI's tools/package.ps1: build -> stage -> verify -> zip + SHA-256).

.DESCRIPTION
  Flow:
    1. (unless -SkipBuild) xmake f -m <flavor> -P . && xmake -P .
       - the before_build hook rebuilds the browser view if ui/animation-browser is newer
       - the after_build hook also deploys to MO2, which is the normal dev flow
    2. stage the FOMOD tree (Core / Library / Immersion / fomod) from the authoritative
       sources: build/ (DLL + generated browser view), dist/ (scripts + scene packs)
    3. add the license docs a GPL distribution must carry
    4. verify every required file is present (hard fail, never a silent skip)
    5. zip -> packaging/out/OSF Animation v<version>[-tag].zip, print size + SHA-256

.PARAMETER Version
  Release version. Defaults to set_version(...) parsed from xmake.lua.
.PARAMETER Tag
  Optional suffix (e.g. 'beta' -> v1.1.0-beta). Also stamped into fomod/info.xml.
.PARAMETER DllFlavor
  xmake build mode. Default releasedbg (ship the flavor you tested in-game).
.PARAMETER SkipBuild
  Package the current build/ + dist/ without rebuilding.
.PARAMETER CompilePapyrus
  Recompile dist/Scripts/Source with the CK Papyrus compiler before staging (hard-fails
  if the compiler is missing). Without it, a .psc newer than its .pex only warns.
.PARAMETER IncludePdb
  Ship the .pdb next to the DLL (crash-log symbolication). Excluded by default.

.EXAMPLE
  packaging\build-archive.ps1
  # builds releasedbg, stages, verifies -> packaging\out\OSF Animation v1.0.0.zip
.EXAMPLE
  packaging\build-archive.ps1 -SkipBuild -Version 1.1.0 -Tag beta
#>
[CmdletBinding()]
param(
    [string] $Version,
    [string] $Tag = '',
    [ValidateSet('releasedbg', 'release')] [string] $DllFlavor = 'releasedbg',
    [switch] $SkipBuild,
    [switch] $CompilePapyrus,
    [switch] $IncludePdb,
    [string] $OutDir = "$PSScriptRoot\out"
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent

function Step($m) { Write-Host "==> $m" -ForegroundColor Cyan }
function Warn($m) { Write-Host "!!  $m" -ForegroundColor Yellow }
function Die($m)  { Write-Host "XX  $m" -ForegroundColor Red; exit 1 }

# --- version ---------------------------------------------------------------
if (-not $Version) {
    $m = Select-String -Path (Join-Path $repo 'xmake.lua') -Pattern 'set_version\("([^"]+)"\)' | Select-Object -First 1
    if (-not $m) { Die 'Could not parse set_version from xmake.lua; pass -Version explicitly.' }
    $Version = $m.Matches[0].Groups[1].Value
}
$verLabel = $Version
if ($Tag) { $verLabel += "-$Tag" }
Step "Packaging OSF Animation v$verLabel  (flavor=$DllFlavor)"

# --- build -----------------------------------------------------------------
# Pin -m and -P: a bare `xmake f -c` drops the mode and silently reconfigures to release.
if (-not $SkipBuild) {
    if (-not (Get-Command xmake -ErrorAction SilentlyContinue)) { Die 'xmake not found on PATH.' }
    Push-Location $repo
    try {
        Step "xmake f -m $DllFlavor -P ."
        xmake f -m $DllFlavor -P . -y
        if ($LASTEXITCODE -ne 0) { Die 'xmake config failed.' }
        Step 'xmake -P .   (also refreshes the browser view + deploys to MO2)'
        xmake -P . -y
        if ($LASTEXITCODE -ne 0) { Die 'Build failed.' }
    } finally { Pop-Location }
} else {
    Warn 'SkipBuild: packaging whatever is already in build/ + dist/ (browser view included, and possibly stale).'
}

$dll = Join-Path $repo "build\windows\x64\$DllFlavor\OSF Animation.dll"
$pdb = [IO.Path]::ChangeExtension($dll, 'pdb')
$dist = Join-Path $repo 'dist'
if (-not (Test-Path $dll)) { Die "DLL not found: $dll  (run xmake first; -DllFlavor $DllFlavor)" }

# --- Papyrus surface -------------------------------------------------------
# OSFTest (console smoke-test harness) is DEV-ONLY and deliberately excluded from release.
$apiScripts = @('OSF', 'OSFTypes', 'OSFAdvanced')
if ($CompilePapyrus) {
    $pc = 'C:\Program Files (x86)\Steam\steamapps\common\Starfield\Tools\Papyrus Compiler\PapyrusCompiler.exe'
    if (-not (Test-Path $pc)) { Die "-CompilePapyrus: compiler not found at $pc" }
    Step 'Compiling Papyrus (dist/Scripts/Source -> dist/Scripts)'
    & $pc (Join-Path $repo 'dist\Scripts\Source') `
        -i="$repo\dist\Scripts\Source;C:\Modding\Starfield\PapyrusSource" `
        -o="$repo\dist\Scripts" -f='C:\Modding\Starfield\PapyrusSource\Starfield_Papyrus_Flags.flg' -all
    if ($LASTEXITCODE -ne 0) { Die 'Papyrus compile failed.' }
}
foreach ($s in $apiScripts) {
    $pex = Join-Path $dist "Scripts\$s.pex"
    $psc = Join-Path $dist "Scripts\Source\$s.psc"
    if (-not (Test-Path $pex)) { Die "Missing $pex — compile Papyrus first (or pass -CompilePapyrus)." }
    if (-not (Test-Path $psc)) { Die "Missing $psc — consumers need the source to compile against." }
    # Stale .pex ships an API whose new natives fail to bind at link time — invisible until in-game.
    if ((Get-Item $psc).LastWriteTime -gt (Get-Item $pex).LastWriteTime) {
        Warn "$s.psc is newer than $s.pex — recompile (or re-run with -CompilePapyrus) before shipping."
    }
}

# --- stage Core ------------------------------------------------------------
$stage = Join-Path $env:TEMP "osfanim-pkg-$(Get-Random)"
$core = "$stage\Core"
@("$core\SFSE\Plugins", "$core\Scripts\Source") |
    ForEach-Object { New-Item -ItemType Directory -Force -Path $_ | Out-Null }

Step 'Staging Core (DLL + Papyrus API + browser view + internal scenes)'
Copy-Item $dll "$core\SFSE\Plugins\"
if ($IncludePdb) {
    if (-not (Test-Path $pdb)) { Die "-IncludePdb: $pdb not found." }
    Copy-Item $pdb "$core\SFSE\Plugins\"
}
foreach ($s in $apiScripts) {
    Copy-Item "$dist\Scripts\$s.pex" "$core\Scripts\"
    Copy-Item "$dist\Scripts\Source\$s.psc" "$core\Scripts\Source\"
}

# The scene-browser view (generated Vite production build — the xmake run above regenerates it
# from ui/animation-browser). OSF UI discovers views across the VFS at
# <OSFUI.dll dir>\OSFUI\views\<modId>\<viewName>\ — the view ships from THIS mod, no copy lives
# in OSF UI. Without it the browser/wheel UI simply doesn't exist. Copy the complete output
# (hashed fonts) to match the xmake deploy, minus the dev-only JS source map (below).
$viewSrc = Join-Path $repo 'build\views\osf.animation\browser'
if (-not (Test-Path "$viewSrc\index.html")) { Die "Browser view missing: $viewSrc (run npm run build in ui/animation-browser, or drop -SkipBuild)" }
$viewDst = "$core\SFSE\Plugins\OSFUI\views\osf.animation\browser"
New-Item -ItemType Directory -Force -Path $viewDst | Out-Null
Copy-Item "$viewSrc\*" $viewDst -Recurse
# .js.map is dev-only (no in-game devtools consume it) and bloats the archive — never ship it.
# Copy-Item -Recurse -Exclude misses files in subfolders, so prune the staged copy explicitly.
Get-ChildItem $viewDst -Recurse -Filter '*.map' | Remove-Item -Force

# License docs a distribution must carry (GPL-3.0 + modding exception + attribution).
# They live inside the plugin's own folder so the game's Data root stays clean.
$docDst = "$core\SFSE\Plugins\OSF Animation"
New-Item -ItemType Directory -Force -Path $docDst | Out-Null
foreach ($doc in 'LICENSE', 'EXCEPTIONS', 'THIRD_PARTY.md') {
    $src = Join-Path $repo $doc
    if (Test-Path $src) { Copy-Item $src $docDst } else { Warn "doc '$doc' not found — omitted." }
}

# Settings + hotkeys live in OSF UI's in-game settings menu (schema registered at
# runtime over the bridge) — no Data/OSF/settings.json ships anymore.
New-Item -ItemType Directory -Force -Path "$core\OSF" | Out-Null

# internal.osf.json -> Data/OSF/internal.osf.json. System scenes the framework needs at runtime:
# the player-only "solo" scene backs OSF.Health, plus the "pair" smoke-test. Always shipped so the
# health self-test works on a clean install (scenes are looked up by id, independent of filename).
Copy-Item "$dist\OSF\internal.osf.json" "$core\OSF\internal.osf.json"

# --- stage Animation Library (opt-in FOMOD group "Animation Library") ------
# The browsable vanilla + creature catalog: pure JSON packs that reference game-archive
# .af clips (no baked animation data, ~2.3 MB). The browser's library lane is empty without these.
$lib = "$stage\Library\OSF\vanilla"
New-Item -ItemType Directory -Force -Path $lib | Out-Null
$libPacks = @(Get-ChildItem "$dist\OSF\vanilla" -Filter '*.osf.json' -ErrorAction SilentlyContinue)
if ($libPacks.Count -lt 20) {
    Die "Animation library looks incomplete: $($libPacks.Count) packs in $dist\OSF\vanilla (expected the full vanilla+creature set; generate with tools/vanilla-packs)."
}
$libPacks | ForEach-Object { Copy-Item $_.FullName $lib }

# --- stage Immersion Actions (opt-in FOMOD group "Immersion Actions") ------
# Emote-wheel content: the player.emote.* scenes the wheel enumerates.
$imm = "$stage\Immersion\OSF\immersion"
New-Item -ItemType Directory -Force -Path $imm | Out-Null
if (-not (Test-Path "$dist\OSF\immersion\emotes.osf.json")) { Die "Immersion pack missing: $dist\OSF\immersion\emotes.osf.json" }
Copy-Item "$dist\OSF\immersion\emotes.osf.json" $imm

# --- fomod installer -------------------------------------------------------
# fomod/ (incl. fomod/images/ banner + plugin art referenced by ModuleConfig.xml) + stamp the version.
Copy-Item "$PSScriptRoot\fomod" "$stage\fomod" -Recurse
$infoPath = "$stage\fomod\info.xml"
$mver = ($verLabel -replace '[^0-9.].*$', '')   # 1.1.0-beta -> 1.1.0
(Get-Content $infoPath -Raw) -replace '<Version\b.*?</Version>', "<Version MachineVersion=`"$mver`">$verLabel</Version>" |
    Set-Content $infoPath -Encoding UTF8

# --- verify the staged payload ---------------------------------------------
Step 'Verifying staged payload'
$required = @(
    'Core\SFSE\Plugins\OSF Animation.dll',
    'Core\Scripts\OSF.pex', 'Core\Scripts\OSFTypes.pex', 'Core\Scripts\OSFAdvanced.pex',
    'Core\Scripts\Source\OSF.psc', 'Core\Scripts\Source\OSFTypes.psc', 'Core\Scripts\Source\OSFAdvanced.psc',
    'Core\SFSE\Plugins\OSFUI\views\osf.animation\browser\index.html',
    'Core\SFSE\Plugins\OSFUI\views\osf.animation\browser\manifest.json',  # OSF UI discovers the view by this
    'Core\SFSE\Plugins\OSFUI\views\osf.animation\browser\assets\browser.js',
    'Core\SFSE\Plugins\OSF Animation\LICENSE',      # GPL-3.0 text (required to distribute)
    'Core\SFSE\Plugins\OSF Animation\EXCEPTIONS',   # GPL 7 modding/linking exception
    'Core\OSF\internal.osf.json',
    'Immersion\OSF\immersion\emotes.osf.json',
    'fomod\ModuleConfig.xml', 'fomod\info.xml', 'fomod\images\header.jpg'
)
$missing = $required | Where-Object { -not (Test-Path (Join-Path $stage $_)) }
if ($missing) { Die ("Staged archive is missing required files:`n    " + ($missing -join "`n    ")) }

# The dev-only test harness must never ship.
if (Get-ChildItem $stage -Recurse -Include 'OSFTest.pex', 'OSFTest.psc' -ErrorAction SilentlyContinue) {
    Die 'OSFTest leaked into the stage — it is dev-only and must not ship.'
}

# JS source maps are dev-only; the staging copy above prunes them, so any survivor is a bug.
if (Get-ChildItem $stage -Recurse -Include '*.map' -ErrorAction SilentlyContinue) {
    Die 'A .map source map leaked into the stage — source maps are dev-only and must not ship.'
}

# Every <folder source="..."> the installer references must exist in the stage,
# or the FOMOD silently installs nothing for that option.
[xml]$mc = Get-Content (Join-Path $stage 'fomod\ModuleConfig.xml')
$fomodSources = @($mc.SelectNodes('//folder/@source') | ForEach-Object { $_.Value } | Select-Object -Unique)
$badSources = $fomodSources | Where-Object { -not (Test-Path (Join-Path $stage $_)) }
if ($badSources) { Die ("ModuleConfig.xml references missing folder(s): " + ($badSources -join ', ')) }

# --- zip -------------------------------------------------------------------
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$zip = Join-Path $OutDir "OSF Animation v$verLabel.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Step "Compressing -> $zip"
Compress-Archive -Path "$stage\*" -DestinationPath $zip -CompressionLevel Optimal
$fileCount = (Get-ChildItem $stage -Recurse -File).Count
Remove-Item $stage -Recurse -Force

# --- report ----------------------------------------------------------------
$sizeMB = [math]::Round((Get-Item $zip).Length / 1MB, 2)
$sha = (Get-FileHash $zip -Algorithm SHA256).Hash
Write-Host ''
Write-Host 'OK  Release archive ready' -ForegroundColor Green
Write-Host "    $zip"
$pdbNote = if ($IncludePdb) { 'PDB included' } else { 'no PDB (-IncludePdb to ship crash-log symbols)' }
Write-Host "    $sizeMB MB, $fileCount files ($DllFlavor, $($libPacks.Count) library packs, $pdbNote)"
Write-Host "    SHA256 $sha"
Write-Host ''
Write-Host '    FOMOD archive: install with MO2 / Vortex (Core is required; Animation Library'
Write-Host '    and Emote Wheel Content are the opt-in steps). Requires SFSE + OSF UI.'
