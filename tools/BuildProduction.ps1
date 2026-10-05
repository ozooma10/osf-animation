#requires -Version 7.2
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
. (Join-Path $repo 'packaging/ReleaseValidation.ps1')
& pwsh -NoProfile -File (Join-Path $PSScriptRoot 'Test-SdkLock.ps1')
if ($LASTEXITCODE) { throw 'SDK lock failed.' }
$before = Get-SmokeSourceIdentity $repo
$mods = $env:XSE_SF_MODS_PATH
$game = $env:XSE_SF_GAME_PATH
Push-Location $repo
try {
    $env:XSE_SF_MODS_PATH = Join-Path $repo ('build/packages/staging-' + [guid]::NewGuid().ToString('N'))
    $env:XSE_SF_GAME_PATH = $null
    & xmake f -y -p windows -a x64 -m releasedbg -o build --animation_smoke=n
    if ($LASTEXITCODE) { throw 'Production configuration failed.' }
    & xmake build -y -j4 'OSF Animation'
    if ($LASTEXITCODE) { throw 'Production build failed.' }
    Write-ReleaseBuildReceipt $repo 'engine' $before
} finally {
    $env:XSE_SF_MODS_PATH = $mods
    $env:XSE_SF_GAME_PATH = $game
    Pop-Location
}
