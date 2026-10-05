#requires -Version 7.2
[CmdletBinding()]
param(
    [ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+$')] [string] $Version,
    [ValidatePattern('^[A-Za-z0-9.-]*$')] [string] $Tag = '',
    [switch] $SkipBuild,
    [string] $OutDir = "$PSScriptRoot\out",
    [string] $ResultPath = ''
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'ReleaseValidation.ps1')
$OutDir = [IO.Path]::GetFullPath($OutDir)
if (-not $Version) {
    $match = Select-String -Path (Join-Path $repo 'xmake.lua') -Pattern 'set_version\("([^"]+)"\)' | Select-Object -First 1
    if (-not $match) { throw 'Could not read the engine version from xmake.lua.' }
    $Version = $match.Matches[0].Groups[1].Value
}
$label = if ($Tag) { "$Version-$Tag" } else { $Version }

if (-not $SkipBuild) {
    & (Join-Path $repo 'tools\BuildBrowserAddon.ps1') -NoDeploy
}

$receipt = Test-ReleaseBuildReceipt $repo 'browser'
$actualVersion = [regex]::Match((Get-Content (Join-Path $repo 'xmake.lua') -Raw), 'set_version\("([^"\r\n]+)"\)').Groups[1].Value
if ($Version -cne $actualVersion) { throw 'Browser archive version must match the engine source version.' }
$view = Join-Path $repo 'build\osfui-animation-browser\SFSE\Plugins\OSF\UI\views\osf.animation'
$browser = Join-Path $view 'browser'
foreach ($name in 'index.html', 'manifest.json') {
    if (-not (Test-Path -LiteralPath (Join-Path $browser $name))) { throw "Browser build missing $name." }
}

$stage = Join-Path $repo ('build/packages/browser-' + [guid]::NewGuid().ToString('N'))
$viewTarget = Join-Path $stage 'Browser\SFSE\Plugins\OSF\UI\views'
New-Item -ItemType Directory -Force -Path $viewTarget | Out-Null
Copy-Item -LiteralPath $view -Destination $viewTarget -Recurse

$docs = Join-Path $stage 'Browser\SFSE\Plugins\OSF Animation Browser'
New-Item -ItemType Directory -Force -Path $docs | Out-Null
foreach ($name in 'LICENSE', 'EXCEPTIONS', 'THIRD_PARTY.md') {
    $source = Join-Path $repo $name
    if ($name -ne 'THIRD_PARTY.md' -and -not (Test-Path -LiteralPath $source)) {
        throw "Required license document is missing: $name"
    }
    if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination $docs }
}

$packs = @(Get-ChildItem (Join-Path $repo 'dist\OSF\vanilla') -Filter '*.osf.json' -File)
if ($packs.Count -lt 20) { throw "Vanilla animation library is incomplete ($($packs.Count) packs)." }
$library = Join-Path $stage 'Library\OSF\vanilla'
New-Item -ItemType Directory -Force -Path $library | Out-Null
$packs | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $library }

$emotes = Join-Path $repo 'dist\OSF\immersion\emotes.osf.json'
if (-not (Test-Path -LiteralPath $emotes)) { throw 'The emote pack is missing.' }
$immersion = Join-Path $stage 'Immersion\OSF\immersion'
New-Item -ItemType Directory -Force -Path $immersion | Out-Null
Copy-Item -LiteralPath $emotes -Destination $immersion

$fomod = Join-Path $stage 'fomod'
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'browser-fomod') -Destination $fomod -Recurse
$images = Join-Path $fomod 'images'
New-Item -ItemType Directory -Force -Path $images | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'fomod\images\header.jpg') -Destination $images
$info = Join-Path $fomod 'info.xml'
$machineVersion = $Version -replace '[^0-9.].*$', ''
(Get-Content -LiteralPath $info -Raw) -replace '<Version\b.*?</Version>',
    "<Version MachineVersion=`"$machineVersion`">$label</Version>" |
    Set-Content -LiteralPath $info -Encoding UTF8

[xml]$config = Get-Content -LiteralPath (Join-Path $fomod 'ModuleConfig.xml')
$sources = @($config.SelectNodes('//folder/@source') | ForEach-Object { $_.Value } | Select-Object -Unique)
foreach ($source in $sources) {
    if (-not (Test-Path -LiteralPath (Join-Path $stage $source))) {
        throw "Installer references missing folder: $source"
    }
}
if (-not (Get-ChildItem (Join-Path $viewTarget 'osf.animation') -Recurse -Filter '*.js' -File)) {
    throw 'The browser has no generated JavaScript.'
}
if (Get-ChildItem $stage -Recurse -Filter '*.map' -File) { throw 'A source map leaked into the addon.' }

$null = Test-ReleaseBuildReceipt $repo 'browser'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$zip = Join-Path $OutDir "OSF Animation Browser v$label.zip"
Write-ReleaseArchive $stage $zip $receipt $ResultPath $repo
