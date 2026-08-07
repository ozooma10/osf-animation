#requires -Version 5.1
<#
.SYNOPSIS
  Download and verify the official Tracy Windows viewer into the ignored build/tools directory.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$Version = '0.13.1'
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$installParent = Join-Path $repo 'build\tools\tracy'
$installRoot = Join-Path $installParent $Version
$existing = @(Get-ChildItem -LiteralPath $installRoot -Recurse -Filter 'tracy-profiler.exe' -File -ErrorAction SilentlyContinue)
if ($existing.Count -eq 1) {
    Write-Host "Tracy viewer already installed: $($existing[0].FullName)" -ForegroundColor Green
    exit 0
}
if (Test-Path -LiteralPath $installRoot) {
    throw "Incomplete or ambiguous Tracy installation at $installRoot. Remove that generated directory and rerun."
}

[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$headers = @{
    'Accept' = 'application/vnd.github+json'
    'User-Agent' = 'OSF-Animation-Profiling-Setup'
}
$release = Invoke-RestMethod -Uri "https://api.github.com/repos/wolfpld/tracy/releases/tags/v$Version" -Headers $headers
$assetName = "windows-$Version.zip"
$asset = @($release.assets | Where-Object { $_.name -eq $assetName })
if ($asset.Count -ne 1) { throw "Official Tracy release v$Version does not expose exactly one '$assetName' asset." }
$digest = [string]$asset[0].digest
if (-not $digest.StartsWith('sha256:', [StringComparison]::OrdinalIgnoreCase)) {
    throw "GitHub did not publish a SHA-256 digest for '$assetName'; refusing an unverified download."
}
$expectedHash = $digest.Substring(7).ToUpperInvariant()

$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ("osf-tracy-" + [Guid]::NewGuid().ToString('N'))
$archive = Join-Path $tempRoot $assetName
$extract = Join-Path $tempRoot 'extract'
New-Item -ItemType Directory -Force -Path $extract | Out-Null
try {
    Write-Host "Downloading official Tracy v$Version Windows viewer..." -ForegroundColor Cyan
    Invoke-WebRequest -Uri $asset[0].browser_download_url -Headers $headers -OutFile $archive
    $actualHash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToUpperInvariant()
    if ($actualHash -ne $expectedHash) {
        throw "Tracy asset hash mismatch. Expected $expectedHash, got $actualHash."
    }
    Expand-Archive -LiteralPath $archive -DestinationPath $extract
    $viewer = @(Get-ChildItem -LiteralPath $extract -Recurse -Filter 'tracy-profiler.exe' -File)
    if ($viewer.Count -ne 1) { throw "Verified Tracy archive contains $($viewer.Count) tracy-profiler.exe files; expected one." }

    New-Item -ItemType Directory -Force -Path $installParent | Out-Null
    Move-Item -LiteralPath $extract -Destination $installRoot
} finally {
    $tempFull = [IO.Path]::GetFullPath($tempRoot)
    $systemTemp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    if ($tempFull.StartsWith($systemTemp, [StringComparison]::OrdinalIgnoreCase) -and
        (Test-Path -LiteralPath $tempRoot)) {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force
    }
}

$installed = @(Get-ChildItem -LiteralPath $installRoot -Recurse -Filter 'tracy-profiler.exe' -File)
Write-Host "Tracy viewer installed and verified: $($installed[0].FullName)" -ForegroundColor Green
