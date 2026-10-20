#requires -Version 7.2
[CmdletBinding()]
param([Parameter(Mandatory)][string]$OutputRoot)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$root = [IO.Path]::GetFullPath($OutputRoot)
$build = [IO.Path]::GetFullPath((Join-Path $repo 'build')).TrimEnd('\') + '\'
if (-not $root.StartsWith($build,[StringComparison]::OrdinalIgnoreCase)) { throw 'D3 fixtures require a private build output.' }
$assets = Join-Path $root 'OSF/D3'
[IO.Directory]::CreateDirectory($assets) | Out-Null
# Eight-second, almost-inert head rotation. No copied game motion or sound.
$stream = [IO.MemoryStream]::new()
$writer = [IO.BinaryWriter]::new($stream)
foreach ($v in @(0,8, 0,0,0,1, 0,0,0.0087265,0.9999619)) { $writer.Write([single]$v) }
$bytes = $stream.ToArray(); $writer.Dispose(); $stream.Dispose()
$doc = @{
    asset=@{version='2.0'};scene=0;scenes=@(@{nodes=@(0)});nodes=@(@{name='C_Head'})
    buffers=@(@{byteLength=$bytes.Length;uri='motion.bin'})
    bufferViews=@(@{buffer=0;byteOffset=0;byteLength=8},@{buffer=0;byteOffset=8;byteLength=32})
    accessors=@(@{bufferView=0;componentType=5126;count=2;type='SCALAR';min=@(0);max=@(8)},
        @{bufferView=1;componentType=5126;count=2;type='VEC4'})
    animations=@(@{name='d3';samplers=@(@{input=0;output=1;interpolation='LINEAR'});
        channels=@(@{sampler=0;target=@{node=0;path='rotation'}})})
}
[IO.File]::WriteAllBytes((Join-Path $assets 'motion.bin'),$bytes)
$doc | ConvertTo-Json -Depth 15 | Set-Content (Join-Path $assets 'motion.gltf') -Encoding utf8NoBOM
function New-D3Tone([int]$Samples,[double]$Frequency) {
$stream = [IO.MemoryStream]::new(); $writer = [IO.BinaryWriter]::new($stream)
try {
    $writer.Write([Text.Encoding]::ASCII.GetBytes('RIFF')); $writer.Write([uint32](36+$samples*2))
    $writer.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt ')); $writer.Write([uint32]16)
    $writer.Write([uint16]1); $writer.Write([uint16]1); $writer.Write([uint32]24000); $writer.Write([uint32]48000)
    $writer.Write([uint16]2); $writer.Write([uint16]16)
    $writer.Write([Text.Encoding]::ASCII.GetBytes('data')); $writer.Write([uint32]($samples*2))
    for ($i=0;$i -lt $samples;$i++) {
        $envelope=[Math]::Min(1,$i/120.0)*[Math]::Min(1,($samples-$i)/120.0)
        $writer.Write([int16](700*$envelope*[Math]::Sin(2*[Math]::PI*$Frequency*$i/24000)))
    }
    $tone=$stream.ToArray()
} finally { $writer.Dispose(); $stream.Dispose() }
return ,$tone
}
$tone=New-D3Tone 2400 660
foreach($channel in @(@('voice',660),@('contact',880),@('other',1100))) {
    [IO.File]::WriteAllBytes((Join-Path $assets ($channel[0]+'.wav')),(New-D3Tone 192000 $channel[1]))
}
$scenes = @()
foreach ($route in @('lane','beat')) {
    [IO.Directory]::CreateDirectory((Join-Path $assets $route)) | Out-Null
    $marks=@()
    for ($i=0;$i -lt 32;$i++) {
        $file="$route/$('{0:d2}' -f $i).wav"
        [IO.File]::WriteAllBytes((Join-Path $assets $file),$tone)
        $mark=@{at=($i+0.5)/32;role='lead';repeat='loop'}
        if ($route -eq 'lane') { $mark.spec="OSF/D3/$file" } else { $mark.id="d3:OSF/D3/$file" }
        $marks+=,$mark
    }
    $node=@{id='main';stages=@(@{clips=@('OSF/D3/motion.gltf:d3');loops=0});loops=0;timer=8.1;
        edges=@(@{to='$end';when='timer'})}
    $node[$(if($route -eq 'lane'){'sound'}else{'cue'})]=$marks
    $scenes+=@{id="osf.d3.$route";tags=@('osf-d3');stripActors=$false;lockPlayer=$false;placement='followActor';
        roles=@(@{name='lead';gender='any'});entry='main';nodes=@($node)}
    $mark=@{at=0.05;role='lead'}
    [IO.File]::WriteAllBytes((Join-Path $assets "$route/cancel.wav"),$tone)
    if($route -eq 'lane'){$mark.spec="OSF/D3/$route/cancel.wav"}else{$mark.id="d3:OSF/D3/$route/cancel.wav"}
    $node=@{id='main';stages=@(@{clips=@('OSF/D3/motion.gltf:d3');loops=0});loops=0;
        edges=@(@{id='authored';label='Authored voice';to='authored';when='advance'})}
    $node[$(if($route -eq 'lane'){'sound'}else{'cue'})]=@($mark)
    $authored=@{id='authored';stages=@(@{clips=@('OSF/D3/motion.gltf:d3');loops=0});loops=0;
        sound=@(@{at='enter';role='lead';spec='OSF/D3/authored.wav'})}
    $scenes+=@{id="osf.d3.cancel-$route";tags=@('osf-d3');stripActors=$false;lockPlayer=$false;placement='followActor';
        roles=@(@{name='lead';gender='any'});entry='main';nodes=@($node,$authored)}
}
[IO.File]::WriteAllBytes((Join-Path $assets 'authored.wav'),$tone)
$scenes+=@{id='osf.d3.silent';tags=@('osf-d3');stripActors=$false;lockPlayer=$false;placement='followActor';
    roles=@(@{name='lead';gender='any'});stages=@(@{clips=@('OSF/D3/motion.gltf:d3');loops=0})}
@{schema=1;name='Private D3 audio comparison';unlisted=$true;camera='none';placement='followActor';
    playerInputLock=$false;hideApparel=$false;clearHeldItems=$false;fade=$false;sceneControls=$false;
    scenes=$scenes} | ConvertTo-Json -Depth 20 | Set-Content (Join-Path $assets 'd3.osf.json') -Encoding utf8NoBOM
Write-Output "Generated D3 fixtures in $assets"
