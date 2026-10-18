#requires -Version 7.2
[CmdletBinding()]
param([Parameter(Mandatory)][string]$OutputRoot, [switch]$SkipPapyrus)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path $PSScriptRoot -Parent
$root = [IO.Path]::GetFullPath($OutputRoot)
$build = [IO.Path]::GetFullPath((Join-Path $repo 'build')).TrimEnd('\') + '\'
if (-not $root.StartsWith($build, [StringComparison]::OrdinalIgnoreCase)) { throw 'Smoke fixtures must be generated in the private build tree.' }
$assets = Join-Path $root 'OSF/Smoke'
[IO.Directory]::CreateDirectory($assets) | Out-Null
Copy-Item -LiteralPath (Join-Path $repo 'docs/examples/query-handoff/example.osf.json') -Destination (Join-Path $assets 'author-example.osf.json')
# A deliberately small authored head rotation, with no copied game asset.
$binary = [IO.MemoryStream]::new()
$writer = [IO.BinaryWriter]::new($binary)
foreach ($v in @(0,1, 0,0,0,1, 0,0,0.1305262,0.9914449)) { $writer.Write([single]$v) }
$bytes = $binary.ToArray(); $writer.Dispose(); $binary.Dispose()
$doc = @{
    asset=@{version='2.0'}; scene=0; scenes=@(@{nodes=@(0)})
    nodes=@(@{name='C_Head'})
    buffers=@(@{byteLength=$bytes.Length;uri='head.bin'})
    bufferViews=@(@{buffer=0;byteOffset=0;byteLength=8},@{buffer=0;byteOffset=8;byteLength=32})
    accessors=@(@{bufferView=0;componentType=5126;count=2;type='SCALAR';min=@(0);max=@(1)},@{bufferView=1;componentType=5126;count=2;type='VEC4'})
    animations=@(@{name='smoke';samplers=@(@{input=0;output=1;interpolation='LINEAR'});channels=@(@{sampler=0;target=@{node=0;path='rotation'}})})
}
[IO.File]::WriteAllBytes((Join-Path $assets 'head.bin'),$bytes)
$doc | ConvertTo-Json -Depth 15 | Set-Content (Join-Path $assets 'head.gltf') -Encoding utf8NoBOM
$doc.buffers[0].Remove('uri')
$json = [Text.Encoding]::UTF8.GetBytes(($doc | ConvertTo-Json -Depth 15 -Compress))
$padded = ($json.Length + 3) -band -4
$stream = [IO.MemoryStream]::new(); $writer = [IO.BinaryWriter]::new($stream)
$writer.Write([uint32]0x46546c67); $writer.Write([uint32]2); $writer.Write([uint32](28+$padded+$bytes.Length))
$writer.Write([uint32]$padded); $writer.Write([uint32]0x4e4f534a); $writer.Write($json)
for ($i=$json.Length;$i -lt $padded;$i++) { $writer.Write([byte]32) }
$writer.Write([uint32]$bytes.Length); $writer.Write([uint32]0x004e4942); $writer.Write($bytes)
$glb = $stream.ToArray(); $writer.Dispose(); $stream.Dispose()
[IO.File]::WriteAllBytes((Join-Path $assets 'head.glb'),$glb)
$stream = [IO.File]::Create((Join-Path $assets 'head-gzip.glb'))
$gzip = [IO.Compression.GZipStream]::new($stream,[IO.Compression.CompressionLevel]::Optimal)
try { $gzip.Write($glb) } finally { $gzip.Dispose(); $stream.Dispose() }
# Short quiet test tone. Wwise acceptance and audible output are reported separately.
$stream = [IO.File]::Create((Join-Path $assets 'tone.wav')); $writer = [IO.BinaryWriter]::new($stream)
try {
    $samples=24000
    $writer.Write([Text.Encoding]::ASCII.GetBytes('RIFF')); $writer.Write([uint32](36+$samples*2))
    $writer.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt ')); $writer.Write([uint32]16)
    $writer.Write([uint16]1); $writer.Write([uint16]1); $writer.Write([uint32]24000); $writer.Write([uint32]48000)
    $writer.Write([uint16]2); $writer.Write([uint16]16); $writer.Write([Text.Encoding]::ASCII.GetBytes('data')); $writer.Write([uint32]($samples*2))
    for($i=0;$i -lt $samples;$i++) { $writer.Write([int16](1500*[Math]::Sin(2*[Math]::PI*440*$i/24000))) }
} finally { $writer.Dispose(); $stream.Dispose() }
@{schema=1;pools=@(@{name='smoke';tags=@('osf-smoke');clips=@(@{spec='OSF/Smoke/tone.wav';text='OSF Animation audio test'})})} |
    ConvertTo-Json -Depth 10 | Set-Content (Join-Path $assets 'smoke.sounds.json') -Encoding utf8NoBOM
if (-not $SkipPapyrus) {
    $compiler='C:/Program Files (x86)/Steam/steamapps/common/Starfield/Tools/Papyrus Compiler/PapyrusCompiler.exe'
    $imports='C:/Modding/Starfield/PapyrusSource'
    $source=Join-Path $repo 'test/smoke/Scripts/Source'
    $scripts=Join-Path $root 'Scripts'; [IO.Directory]::CreateDirectory($scripts) | Out-Null
    & $compiler $source "-i=$source;$repo/dist/Scripts/Source;$imports" "-o=$scripts" "-f=$imports/Starfield_Papyrus_Flags.flg" -all
    if ($LASTEXITCODE -or -not (Test-Path (Join-Path $scripts 'OSFAnimationSmoke.pex'))) { throw 'Smoke Papyrus fixture compilation failed.' }
    $example=Join-Path $repo 'docs/examples/query-handoff'
    & $compiler $example "-i=$example;$repo/dist/Scripts/Source;$imports" "-o=$scripts" "-f=$imports/Starfield_Papyrus_Flags.flg" -all
    if ($LASTEXITCODE -or -not (Test-Path (Join-Path $scripts 'OSFQueryHandoffExample.pex'))) { throw 'Author example Papyrus compilation failed.' }
}
Write-Host 'Generated private GLTF, GLB, gzip, audio and Papyrus smoke fixtures.'
