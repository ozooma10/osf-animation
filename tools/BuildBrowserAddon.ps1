#requires -Version 7.2
[CmdletBinding()]
param([switch] $NoDeploy, [string] $TestReport = '')

$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
. (Join-Path $repo 'packaging/ReleaseValidation.ps1')
& pwsh -NoProfile -File (Join-Path $PSScriptRoot 'Test-SdkLock.ps1')
if ($LASTEXITCODE) { throw 'SDK lock failed.' }
& pwsh -NoProfile -File (Join-Path $PSScriptRoot 'Test-BrowserBoundary.ps1')
if ($LASTEXITCODE) { throw 'Browser boundary failed.' }
$before = Get-SmokeSourceIdentity $repo
$ui = Join-Path $repo 'extensions\browser\ui'
$view = Join-Path $repo 'build\osfui-animation-browser\SFSE\Plugins\OSF\UI\views\osf.animation'

if (-not (Get-Command npm.cmd -ErrorAction SilentlyContinue)) {
    throw 'npm is required to build OSF Animation Browser.'
}

# Native output is first built privately, including for ordinary addon deployment.
$savedMods = $env:XSE_SF_MODS_PATH
$savedGame = $env:XSE_SF_GAME_PATH
Push-Location $repo
try {
    $env:XSE_SF_MODS_PATH = Join-Path $repo ('build/packages/browser-native-' + [guid]::NewGuid().ToString('N'))
    $env:XSE_SF_GAME_PATH = $null
    & xmake f -y -p windows -a x64 -m releasedbg -o build --animation_smoke=n
    if ($LASTEXITCODE) { throw 'Browser native configuration failed.' }
    & xmake build -y -j4 'OSF Animation Browser'
    if ($LASTEXITCODE) { throw 'Browser native build failed.' }
} finally {
    $env:XSE_SF_MODS_PATH = $savedMods
    $env:XSE_SF_GAME_PATH = $savedGame
    Pop-Location
}

Push-Location $ui
try {
    & npm.cmd ci
    if ($LASTEXITCODE -ne 0) { throw 'npm ci failed.' }
    if ($TestReport) {
        if (Test-Path -LiteralPath $TestReport) { throw 'Browser test report already exists; use a fresh evidence path.' }
        & npm.cmd test -- --reporter=default --reporter=json "--outputFile=$TestReport"
        if ($LASTEXITCODE -ne 0) { throw 'Browser tests failed.' }
        & npm.cmd run check
        if ($LASTEXITCODE -ne 0) { throw 'Browser typecheck failed.' }
        & npm.cmd run build
        if ($LASTEXITCODE -ne 0) { throw 'Browser build failed.' }
    } else {
        & npm.cmd run verify
        if ($LASTEXITCODE -ne 0) { throw 'Browser verification failed.' }
    }
} finally {
    Pop-Location
}

if (-not (Test-Path -LiteralPath (Join-Path $view 'browser\manifest.json'))) {
    throw "Browser build is missing its manifest: $view"
}
$schema = Join-Path $repo 'build/osfui-animation-browser/SFSE/Plugins/OSF/Settings/schemas'
New-Item -ItemType Directory -Force -Path $schema | Out-Null
if (Test-Path -LiteralPath (Join-Path $schema 'osf.animation.json')) {
    Remove-Item -LiteralPath (Join-Path $schema 'osf.animation.json') -Force
}
Copy-Item -LiteralPath (Join-Path $repo 'extensions/browser/dist/SFSE/Plugins/OSF/Settings/schemas/osf.animation.browser.json') -Destination $schema -Force

Write-ReleaseBuildReceipt $repo 'browser' $before
if ($NoDeploy -or -not $env:XSE_SF_MODS_PATH) { return }

$mods = [IO.Path]::GetFullPath($env:XSE_SF_MODS_PATH).TrimEnd('\', '/')
$addon = [IO.Path]::GetFullPath((Join-Path $mods 'OSF Animation Browser'))
$prefix = $mods + [IO.Path]::DirectorySeparatorChar
if (-not $addon.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to deploy outside the configured mods directory: $addon"
}

$viewsRoot = Join-Path $addon 'SFSE\Plugins\OSF\UI\views'
$target = [IO.Path]::GetFullPath((Join-Path $viewsRoot 'osf.animation'))
$content = Join-Path $addon 'OSF'
if (-not $target.StartsWith($addon + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to replace a view outside the addon: $target"
}
if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Recurse -Force }
New-Item -ItemType Directory -Force -Path $viewsRoot, $content | Out-Null
Copy-Item -LiteralPath $view -Destination $viewsRoot -Recurse
foreach ($folder in 'vanilla','immersion') {
    # Update shipped content without deleting user-added packs in this mod.
    Copy-Item -LiteralPath (Join-Path $repo "dist/OSF/$folder") -Destination $content -Recurse -Force
}
$plugins = Join-Path $addon 'SFSE/Plugins'
$schemaTarget = Join-Path $plugins 'OSF/Settings/schemas'
New-Item -ItemType Directory -Force -Path $schemaTarget | Out-Null
if (Test-Path -LiteralPath (Join-Path $schemaTarget 'osf.animation.json')) {
    Remove-Item -LiteralPath (Join-Path $schemaTarget 'osf.animation.json') -Force
}
Copy-Item -LiteralPath (Join-Path $schema 'osf.animation.browser.json') -Destination $schemaTarget -Force
foreach ($name in 'OSF Animation Browser.dll','OSF Animation Browser.pdb') {
    Copy-Item -LiteralPath (Join-Path $repo "build/windows/x64/releasedbg/$name") -Destination $plugins -Force
}
$null = Test-ReleaseBuildReceipt $repo 'browser'
foreach ($file in (Get-ReleaseArtifacts $repo 'browser').files) {
    $relative = if ($file.path -ceq 'windows/x64/releasedbg/OSF Animation Browser.dll') { 'SFSE/Plugins/OSF Animation Browser.dll' }
        else { $file.path.Substring('osfui-animation-browser/'.Length) }
    if ((Get-FileHash -LiteralPath (Join-Path $addon $relative)).Hash -cne $file.sha256) { throw "Deployed hash mismatch: $relative" }
}
Write-Host "Deployed OSF Animation Browser to $addon"
