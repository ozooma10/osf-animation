[CmdletBinding()]
param(
    [string] $Python = 'C:\Python314\python.exe',
    [string] $Blender = 'C:\Program Files\Blender Foundation\Blender 5.1\blender.exe',
    [switch] $RebuildAnimations,
    [string] $DeployTo,
    [switch] $Package
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$payload = Join-Path $PSScriptRoot 'dist'
& $Python (Join-Path $PSScriptRoot 'tools\build_plugin.py')
if ($LASTEXITCODE -ne 0) { throw 'Plugin generation failed.' }
if ($RebuildAnimations) {
    & $Blender --background --factory-startup --disable-autoexec --python-exit-code 1 --python (Join-Path $PSScriptRoot 'tools\build_hug.py')
    if ($LASTEXITCODE -ne 0) { throw 'Animation generation failed.' }
}
$source = Join-Path $payload 'Scripts\Source'
& 'C:\Program Files (x86)\Steam\steamapps\common\Starfield\Tools\Papyrus Compiler\PapyrusCompiler.exe' $source "-i=$source;$repo\dist\Scripts\Source;C:\Modding\Starfield\PapyrusSource" "-o=$payload\Scripts" '-f=C:\Modding\Starfield\PapyrusSource\Starfield_Papyrus_Flags.flg' -all
if ($LASTEXITCODE -ne 0) { throw 'Papyrus compilation failed.' }
foreach ($name in 'EnterF','EnterM','HugF','HugM','ExitF','ExitM') {
    $clip = Join-Path $payload "OSF\Companions\$name.glb"
    if (!(Test-Path -LiteralPath $clip)) { throw "Missing $clip; use -RebuildAnimations with the local Contact v2 inputs." }
    & "$repo\build\windows\x64\releasedbg\osf-import-test.exe" $clip | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Native import rejected $clip" }
}
if ($DeployTo) {
    if (Get-Process Starfield -ErrorAction SilentlyContinue) { throw 'Close Starfield before deploying scripts or plugins.' }
    $target = [IO.Path]::GetFullPath($DeployTo)
    if ([IO.Path]::GetFileName($target) -ne 'OSF Companions Prototype') { throw 'DeployTo must name a dedicated OSF Companions Prototype mod directory.' }
    New-Item -ItemType Directory -Path $target -Force | Out-Null
    Get-ChildItem -LiteralPath $payload | Copy-Item -Destination $target -Recurse -Force
    Write-Output "Deployed: $target"
}
if ($Package) {
    $output = Join-Path $repo 'build\companion-hug\OSF Companions Prototype.zip'
    New-Item -ItemType Directory -Path (Split-Path $output) -Force | Out-Null
    Compress-Archive -Path "$payload\*",(Join-Path $PSScriptRoot 'README.md') -DestinationPath $output -Force
    $manifest = Get-ChildItem -LiteralPath $payload -Recurse -File | ForEach-Object {
        [ordered]@{path=[IO.Path]::GetRelativePath($payload,$_.FullName);bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
    }
    [ordered]@{archive=$output;sha256=(Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant();files=@($manifest)} |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path (Split-Path $output) 'package-manifest.json') -Encoding utf8
    Write-Output "Package: $output"
}
