#requires -Version 7.2
# Build and package privately; -SkipBuild requires a current production receipt.
[CmdletBinding()]
param(
    [ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+$')] [string] $Version,
    [ValidatePattern('^[A-Za-z0-9.-]*$')] [string] $Tag = '',
    [ValidateSet('releasedbg')] [string] $DllFlavor = 'releasedbg',
    [switch] $SkipBuild,
    [switch] $CompilePapyrus,
    [switch] $IncludePdb,
    [string] $OutDir = "$PSScriptRoot\out",
    [string] $ResultPath = ''
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'ReleaseValidation.ps1')
$OutDir = [IO.Path]::GetFullPath($OutDir)

function Step($m) { Write-Host "==> $m" -ForegroundColor Cyan }
function Warn($m) { Write-Host "!!  $m" -ForegroundColor Yellow }
function Die($m)  { throw $m }

if (-not $Version) {
    $m = Select-String -Path (Join-Path $repo 'xmake.lua') -Pattern 'set_version\("([^"]+)"\)' | Select-Object -First 1
    if (-not $m) { Die 'Could not parse set_version from xmake.lua; pass -Version explicitly.' }
    $Version = $m.Matches[0].Groups[1].Value
}
$verLabel = $Version
if ($Tag) { $verLabel += "-$Tag" }
Step "Packaging OSF Animation v$verLabel  (flavor=$DllFlavor)"

$dll = Join-Path $repo "build/windows/x64/releasedbg/OSF Animation.dll"
$pdb = [IO.Path]::ChangeExtension($dll, 'pdb')
$dist = Join-Path $repo 'dist'
# OSFTest is dev-only and never ships.
$apiScripts = @('OSF', 'OSFTypes', 'OSFAdvanced')
if ($CompilePapyrus) {
    $pc = 'C:\Program Files (x86)\Steam\steamapps\common\Starfield\Tools\Papyrus Compiler\PapyrusCompiler.exe'
    if (-not (Test-Path $pc)) { Die "-CompilePapyrus: compiler not found at $pc" }
    Step 'Compiling Papyrus (dist/Scripts/Source -> dist/Scripts)'
    # Keep options aligned with AGENTS.md and .vscode/tasks.json.
    & $pc (Join-Path $repo 'dist\Scripts\Source') `
        -i="$repo\dist\Scripts\Source;C:\Modding\Starfield\PapyrusSource" `
        -o="$repo\dist\Scripts" -f='C:\Modding\Starfield\PapyrusSource\Starfield_Papyrus_Flags.flg' -all
    if ($LASTEXITCODE -ne 0) { Die 'Papyrus compile failed.' }
}
Test-ReleasePapyrus $repo
if (-not $SkipBuild) {
    & (Join-Path $repo 'tools/BuildProduction.ps1')
}
$receipt = Test-ReleaseBuildReceipt $repo 'engine'
$actualVersion = [regex]::Match((Get-Content (Join-Path $repo 'xmake.lua') -Raw), 'set_version\("([^"\r\n]+)"\)').Groups[1].Value
if ($Version -cne $actualVersion) { throw 'Archive version must match the built plugin version.' }
$stage = Join-Path $repo ('build/packages/engine-' + [guid]::NewGuid().ToString('N'))
$core = "$stage\Core"
@("$core\SFSE\Plugins", "$core\Scripts\Source") |
    ForEach-Object { New-Item -ItemType Directory -Force -Path $_ | Out-Null }

Step 'Staging Core (DLL + Papyrus API + internal scenes)'
Copy-Item $dll "$core\SFSE\Plugins\"
if ($IncludePdb) {
    if (-not (Test-Path $pdb)) { Die "-IncludePdb: $pdb not found." }
    Copy-Item $pdb "$core\SFSE\Plugins\"
}
foreach ($s in $apiScripts) {
    Copy-Item "$dist\Scripts\$s.pex" "$core\Scripts\"
    Copy-Item "$dist\Scripts\Source\$s.psc" "$core\Scripts\Source\"
}

# License docs a GPL distribution must carry; kept in the plugin folder, off the Data root.
$docDst = "$core\SFSE\Plugins\OSF Animation"
New-Item -ItemType Directory -Force -Path $docDst | Out-Null
foreach ($doc in 'LICENSE', 'EXCEPTIONS', 'THIRD_PARTY.md') {
    $src = Join-Path $repo $doc
    if (Test-Path $src) { Copy-Item $src $docDst } else { Warn "doc '$doc' not found — omitted." }
}

# OSF Settings loads this schema at startup; never package player values.
$schemasDst = "$core\SFSE\Plugins\OSF\Settings\schemas"
New-Item -ItemType Directory -Force -Path $schemasDst | Out-Null
Copy-Item "$dist\SFSE\Plugins\OSF\Settings\schemas\osf.animation.json" $schemasDst
New-Item -ItemType Directory -Force -Path "$core\OSF" | Out-Null

# System scenes: "solo" backs OSF.Health, "pair" is the smoke test. Always shipped.
Copy-Item "$dist\OSF\internal.osf.json" "$core\OSF\internal.osf.json"

Copy-Item "$PSScriptRoot\fomod" "$stage\fomod" -Recurse
$infoPath = "$stage\fomod\info.xml"
$mver = ($verLabel -replace '[^0-9.].*$', '')   # 1.1.0-beta -> 1.1.0
(Get-Content $infoPath -Raw) -replace '<Version\b.*?</Version>', "<Version MachineVersion=`"$mver`">$verLabel</Version>" |
    Set-Content $infoPath -Encoding UTF8

Step 'Verifying staged payload'
$required = @(
    'Core\SFSE\Plugins\OSF Animation.dll',
    'Core\Scripts\OSF.pex', 'Core\Scripts\OSFTypes.pex', 'Core\Scripts\OSFAdvanced.pex',
    'Core\Scripts\Source\OSF.psc', 'Core\Scripts\Source\OSFTypes.psc', 'Core\Scripts\Source\OSFAdvanced.psc',
    'Core\SFSE\Plugins\OSF\Settings\schemas\osf.animation.json',
    'Core\SFSE\Plugins\OSF Animation\LICENSE',      # GPL-3.0 text (required to distribute)
    'Core\SFSE\Plugins\OSF Animation\EXCEPTIONS',   # GPL 7 modding/linking exception
    'Core\OSF\internal.osf.json',
    'fomod\ModuleConfig.xml', 'fomod\info.xml', 'fomod\images\header.jpg'
)
$missing = $required | Where-Object { -not (Test-Path (Join-Path $stage $_)) }
if ($missing) { Die ("Staged archive is missing required files:`n    " + ($missing -join "`n    ")) }
if (Test-Path "$core\SFSE\Plugins\OSF\UI\views\osf.animation") {
    Die 'The browser view leaked into the engine archive.'
}

if (Get-ChildItem $stage -Recurse -Include 'OSFTest.pex', 'OSFTest.psc' -ErrorAction SilentlyContinue) {
    Die 'OSFTest leaked into the stage — it is dev-only and must not ship.'
}

# A missing <folder source> makes the FOMOD silently install nothing for that option.
[xml]$mc = Get-Content (Join-Path $stage 'fomod\ModuleConfig.xml')
$fomodSources = @($mc.SelectNodes('//folder/@source') | ForEach-Object { $_.Value } | Select-Object -Unique)
$badSources = $fomodSources | Where-Object { -not (Test-Path (Join-Path $stage $_)) }
if ($badSources) { Die ("ModuleConfig.xml references missing folder(s): " + ($badSources -join ', ')) }

$allowed = @($required | ForEach-Object { $_.Replace('\','/') }) + @('Core/SFSE/Plugins/OSF Animation/THIRD_PARTY.md')
if ($IncludePdb) { $allowed += 'Core/SFSE/Plugins/OSF Animation.pdb' }
foreach ($file in Get-ReleaseFileHashes $stage) {
    if ($file.path -cnotin $allowed) { throw "Unexpected engine payload: $($file.path)" }
}
$null = Test-ReleaseBuildReceipt $repo 'engine'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$zip = Join-Path $OutDir "OSF Animation v$verLabel.zip"
Write-ReleaseArchive $stage $zip $receipt $ResultPath $repo
