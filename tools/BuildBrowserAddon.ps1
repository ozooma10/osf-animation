#requires -Version 5.1
[CmdletBinding()]
param([switch] $NoDeploy)

$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$ui = Join-Path $repo 'ui\animation-browser'
$view = Join-Path $repo 'build\osfui-animation-browser\SFSE\Plugins\OSF\UI\views\osf.animation'

if (-not (Get-Command npm.cmd -ErrorAction SilentlyContinue)) {
    throw 'npm is required to build OSF Animation Browser.'
}

Push-Location $ui
try {
    & npm.cmd ci
    if ($LASTEXITCODE -ne 0) { throw 'npm ci failed.' }
    & npm.cmd run verify
    if ($LASTEXITCODE -ne 0) { throw 'Browser verification failed.' }
} finally {
    Pop-Location
}

if (-not (Test-Path -LiteralPath (Join-Path $view 'browser\manifest.json'))) {
    throw "Browser build is missing its manifest: $view"
}

if ($NoDeploy -or -not $env:XSE_SF_MODS_PATH) { return }

$mods = [IO.Path]::GetFullPath($env:XSE_SF_MODS_PATH).TrimEnd('\', '/')
$addon = [IO.Path]::GetFullPath((Join-Path $mods 'OSF Animation Browser'))
$prefix = $mods + [IO.Path]::DirectorySeparatorChar
if (-not $addon.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to deploy outside the configured mods directory: $addon"
}

$viewsRoot = Join-Path $addon 'SFSE\Plugins\OSF\UI\views'
$target = Join-Path $viewsRoot 'osf.animation'
$content = Join-Path $addon 'OSF'
if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Recurse -Force }
if (Test-Path -LiteralPath $content) { Remove-Item -LiteralPath $content -Recurse -Force }
New-Item -ItemType Directory -Force -Path $viewsRoot, $content | Out-Null
Copy-Item -LiteralPath $view -Destination $viewsRoot -Recurse
Copy-Item -LiteralPath (Join-Path $repo 'dist\OSF\vanilla') -Destination $content -Recurse
Copy-Item -LiteralPath (Join-Path $repo 'dist\OSF\immersion') -Destination $content -Recurse
Write-Host "Deployed OSF Animation Browser to $addon"
