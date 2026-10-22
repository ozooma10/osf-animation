#requires -Version 7.2
# Focused background integration test. Uses a disposable harness save and the production DLL.
[CmdletBinding()]
param([string]$Harness = 'C:\Modding\Starfield\OSF Test Harness', [switch]$MenuPauses, [switch]$Interactive)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../../tools/ReleaseRules.ps1')
if ($Interactive -and -not $MenuPauses) { throw 'Interactive is only needed for menu pause checks.' }
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$runId = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$run = Join-Path $repo "build/loop-globals-game/$runId"
$payload = Join-Path $run 'payload'
$output = 'C:\Modding\Starfield\MO2\mods\OSF Testing Output'
$profile = 'C:\Modding\Starfield\MO2\profiles\OSF Testing'
$pluginList = Join-Path $profile 'plugins.txt'
$pluginName = 'OSFLoopGlobals.esm'
$entry = Join-Path $Harness 'Test-Starfield.ps1'
$files = @()
$started = $false
$pluginAdded = $false
$script:HarnessInvocation = 0
[IO.Directory]::CreateDirectory($payload) | Out-Null

function New-Bytes([scriptblock]$Write) {
    $s = [IO.MemoryStream]::new(); $w = [IO.BinaryWriter]::new($s)
    try { & $Write $w; return ,$s.ToArray() } finally { $w.Dispose(); $s.Dispose() }
}
function Text($w, [string]$s) { $w.Write([Text.Encoding]::UTF8.GetBytes($s)) }
function Part($w, [string]$tag, [byte[]]$bytes) { Text $w $tag; $w.Write([uint16]$bytes.Length); $w.Write($bytes) }
function Record($w, [string]$tag, [uint32]$id, [uint32]$flags, [byte[]]$bytes) {
    Text $w $tag; $w.Write([uint32]$bytes.Length); $w.Write($flags); $w.Write($id)
    $w.Write([uint32]0); $w.Write([uint16]582); $w.Write([uint16]0); $w.Write($bytes)
}
function Run-Harness([string[]]$Arguments) {
    $script:HarnessInvocation++
    & pwsh -NoProfile -File $entry @Arguments |
        Tee-Object -FilePath (Join-Path $run ("harness-$script:HarnessInvocation-" + $Arguments[1] + '.log')) |
        Where-Object { $_ -match '^(PASS:|FAIL:|Result:|Artifacts:)' } | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE) { throw "Harness failed: $Arguments" }
}

function Read-ProbeJson([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    $stream = [IO.FileStream]::new($Path, 'Open', 'Read', [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
    $reader = [IO.StreamReader]::new($stream)
    try { $reader.ReadToEnd() | ConvertFrom-Json } finally { $reader.Dispose() }
}
function Invoke-PauseProbe([string]$Command = 'probe-playback', [hashtable]$Arguments = @{}) {
    $id = [guid]::NewGuid().ToString('N')
    $request = @{id=$id;sessionId=$script:ProbeSession.sessionId;expiresAtUtcMs=[DateTimeOffset]::UtcNow.AddSeconds(10).ToUnixTimeMilliseconds();command=$Command;args=$Arguments}
    $temporary = Join-Path $script:ProbeMailbox 'request.tmp'
    $request | ConvertTo-Json -Depth 5 | Set-Content $temporary -Encoding utf8NoBOM
    [IO.File]::Move($temporary, (Join-Path $script:ProbeMailbox 'request.json'), $true)
    $timer = [Diagnostics.Stopwatch]::StartNew()
    do {
        $response = Read-ProbeJson (Join-Path $script:ProbeMailbox "responses/$id.json")
        if ($response) {
            if ($response.sessionId -ne $script:ProbeSession.sessionId -or $response.id -ne $id -or -not $response.ok) { throw "Pause probe rejected: $($response.error)" }
            $response | ConvertTo-Json -Depth 8 -Compress | Add-Content (Join-Path $run 'pause-observations.jsonl')
            return $response.data
        }
        Start-Sleep -Milliseconds 50
    } while ($timer.Elapsed.TotalSeconds -lt 10)
    throw "Pause probe timed out: $Command; do not retry a submitted menu change."
}
function Wait-PauseProbe([scriptblock]$Predicate, [string]$Description) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    do {
        $state = Invoke-PauseProbe
        if (& $Predicate $state) { return $state }
        Start-Sleep -Milliseconds 100
    } while ($timer.Elapsed.TotalSeconds -lt 8)
    throw "Timed out: $Description"
}
function Set-ProbeMenu([string]$Name, [bool]$Open) {
    if (-not $Interactive) {
        $null = Invoke-PauseProbe 'probe-menu' @{name=$Name;open=$Open}
        return
    }
    $key = if ($Name -eq 'PauseMenu') { 27 } elseif ($Name -eq 'InventoryMenu' -and $Open) { 73 } else { 9 }
    Run-Harness @('-Action','Input','-Interactive','-VirtualKey',"$key")
    if (-not $Open) {
        $closed = Wait-PauseProbe { param($s) $Name -notin $s.menus } "$Name closes after its Back key"
        # Inventory may return to its DataMenu parent. Close only that observed
        # parent from this test's initially menu-free gameplay state.
        if ($Name -eq 'InventoryMenu' -and 'DataMenu' -in $closed.menus) {
            Run-Harness @('-Action','Input','-Interactive','-VirtualKey','9')
        }
    }
}
function Test-MenuPauses {
    $script:ProbeMailbox = Join-Path $run 'mailbox'
    $script:ProbeSession = Read-ProbeJson (Join-Path $script:ProbeMailbox 'session.json')
    $owner = Read-ProbeJson (Join-Path $Harness 'state/owner.json')
    if (-not $script:ProbeSession -or $script:ProbeSession.pid -ne $owner.pid -or $script:ProbeSession.processStartFileTime -ne $owner.processStartFileTime -or $script:ProbeSession.launchToken -ne $runId) { throw 'Pause probe handshake does not match the owned test game.' }
    $testMenus = if ($Interactive) { @('PauseMenu','DataMenu','InventoryMenu') } else { @('PauseMenu','DataMenu') }
    $results = foreach ($timing in @('loops','timer')) {
        foreach ($menu in $testMenus) {
            $sceneId = "osf.global.pause-$timing"
            $ownedMenu = $false
            $samples = @()
            try {
                Run-Harness @('-Action','Command','-ConsoleCommand',('cgf "OSFLoopGlobals.BeginPauseCase" "' + $sceneId + '"'))
                $before = Wait-PauseProbe { param($s) $s.sceneId -eq $sceneId -and $s.playback -and $s.playback.time -gt 0.15 } 'Papyrus scene starts and advances'
                Set-ProbeMenu $menu $true
                $ownedMenu = $true
                $paused = Wait-PauseProbe { param($s) $menu -in $s.menus -and $s.pauseRequestCount -gt 0 } "$menu opens and pauses simulation"
                $timer = [Diagnostics.Stopwatch]::StartNew()
                do {
                    Start-Sleep -Milliseconds 250
                    $samples += Invoke-PauseProbe
                } while ($timer.Elapsed.TotalSeconds -lt 4)
                Set-ProbeMenu $menu $false
                $ownedMenu = $false
                $resumed = Wait-PauseProbe { param($s) $menu -notin $s.menus -and $s.pauseRequestCount -eq 0 } "$menu closes and resumes simulation"
                Start-Sleep -Milliseconds 350
                $progress = Invoke-PauseProbe
                Start-Sleep -Seconds 3 # Beyond the resume watchdog grace; check the retained handle again.
                $after = Invoke-PauseProbe
                $frozen = $paused.handle -eq $before.handle -and $paused.playback -and @($samples | Where-Object {
                    $_.handle -ne $before.handle -or -not $_.playback -or $menu -notin $_.menus -or $_.pauseRequestCount -eq 0 -or
                    [Math]::Abs($_.playback.time - $paused.playback.time) -gt 0.03
                }).Count -eq 0
                $continued = $after.handle -eq $before.handle -and $after.playback -and $after.playback.id -eq $before.playback.id -and
                    $progress.playback -and [Math]::Abs($progress.playback.time - $resumed.playback.time) -gt 0.03
                $caseResult = [pscustomobject]@{menu=$menu;timing=$timing;frozen=[bool]$frozen;resumed=[bool]$continued;passed=([bool]$frozen -and [bool]$continued);before=$before;paused=$paused;after=$after;samples=$samples.Count}
                $caseResult | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $run "pause-$timing-$menu.json")
                $caseResult
            } finally {
                if ($ownedMenu) { Set-ProbeMenu $menu $false }
                Run-Harness @('-Action','Command','-ConsoleCommand','cgf "OSFLoopGlobals.EndPauseCase"')
                $null = Wait-PauseProbe { param($s) $s.handle -eq 0 -and -not $s.playback } 'Owned scene stops'
            }
        }
    }
    $report = @{runId=$runId;interactive=[bool]$Interactive;testDllSha256=(Get-FileHash $dll).Hash;cases=@($results);passed=(@($results | Where-Object {-not $_.passed}).Count -eq 0)}
    $report | ConvertTo-Json -Depth 10 | Set-Content (Join-Path $run 'pause-result.json')
    $results | Format-Table menu,timing,frozen,resumed,passed
    if (-not $report.passed) { throw 'Menu pause regression reproduced; inspect pause-result.json and animation.log.' }
}

try {
    if (Get-Process Starfield -ErrorAction SilentlyContinue) { throw 'A game is already running; no session was changed.' }
    if (Get-Process ModOrganizer -ErrorAction SilentlyContinue) {
        $selected = Get-Content 'C:\Modding\Starfield\MO2\ModOrganizer.ini' | Where-Object { $_ -match '^selected_profile=' } | Select-Object -First 1
        if ($selected -ne 'selected_profile=@ByteArray(OSF Testing)') { throw 'Idle MO2 must be on the owned OSF Testing profile or closed normally.' }
    }
    $marker = Get-Content (Join-Path $profile '.osf-test-harness.json') -Raw | ConvertFrom-Json
    if ($marker.harnessRoot -ne $Harness) { throw 'Test profile is not owned by this harness.' }
    if ((Get-Content $pluginList) -match '^\*?OSFLoopGlobals\.esm$') { throw 'Test plugin is already listed.' }
    $dll = Join-Path $repo $(if ($MenuPauses) { 'build/smoke-native/windows/x64/releasedbg/OSF Animation.dll' } else { 'build/windows/x64/releasedbg/OSF Animation.dll' })
    if ((Test-InstrumentedDll $dll) -ne [bool]$MenuPauses) {
        throw 'Menu tests require the private smoke DLL; loop tests require the production DLL.'
    }
    Run-Harness @('-Action','Initialize','-SkipBuild')
    & pwsh -NoProfile -File (Join-Path $repo 'tools/BuildSmokeFixtures.ps1') -OutputRoot $payload -SkipPapyrus
    if ($LASTEXITCODE) { throw 'Asset generation failed.' }
    $header = New-Bytes { param($w)
        Part $w 'HEDR' (New-Bytes { param($b) $b.Write([single]0.96); $b.Write([uint32]1); $b.Write([uint32]0x801) })
        Part $w 'CNAM' ([Text.Encoding]::ASCII.GetBytes("OSF loop global test`0"))
        Part $w 'MAST' ([Text.Encoding]::ASCII.GetBytes("Starfield.esm`0"))
    }
    $global = New-Bytes { param($w)
        Part $w 'EDID' ([Text.Encoding]::ASCII.GetBytes("OSFLoopGlobalsCount`0"))
        Part $w 'FNAM' ([byte[]]@(102))
        Part $w 'FLTV' ([BitConverter]::GetBytes([single]2))
    }
    $record = New-Bytes { param($w) Record $w 'GLOB' 0x01000800 0 $global }
    $plugin = New-Bytes { param($w)
        Record $w 'TES4' 0 1 $header
        Text $w 'GRUP'; $w.Write([uint32](24 + $record.Length)); Text $w 'GLOB'
        $w.Write([byte[]]::new(12)); $w.Write($record)
    }
    [IO.File]::WriteAllBytes((Join-Path $payload $pluginName), $plugin)
    $cases = @(
        @{id='live';ref='OSFLoopGlobals.esm|0x800'},
        @{id='changed';ref='OSFLoopGlobals.esm|0x800'},
        @{id='graph';ref='OSFLoopGlobals.esm|0x800'},
        @{id='scaled';ref='OSFLoopGlobals.esm|0x800'},
        @{id='missing-plugin';ref='OSFLoopGlobalsMissing.esm|0x800'},
        @{id='missing-form';ref='OSFLoopGlobals.esm|0x801'},
        @{id='invalid-value';ref='OSFLoopGlobals.esm|0x800'}
    )
    $scenes = foreach ($case in $cases) {
        $stage = @{loops=@{global=$case.ref;default=3};clips=@('OSF/Smoke/head.glb');cue=@(@{at=0.5;id='loop';repeat='loop'})}
        if ($case.id -eq 'graph') {
            @{id="osf.global.$($case.id)";entry='count';nodes=@(@{id='count';loops=$stage.loops;stages=@(@{loops=0;clips=$stage.clips});cue=$stage.cue;edges=@(@{to='$end';when='loops'})})}
        } else { @{id="osf.global.$($case.id)";stages=@($stage)} }
    }
    foreach ($timing in @('loops','timer')) {
        $stage = @{clips=@('OSF/Smoke/head.glb');loops=10}
        if ($timing -eq 'timer') { $stage.loops=0; $stage.timer=10 }
        $scenes += @{id="osf.global.pause-$timing";lockPlayer=$true;stages=@($stage)}
    }
    @{schema=1;unlisted=$true;camera='none';placement='followActor';lockPlayer=$false;stripActors=$false;roles=@(@{name='player'});scenes=@($scenes)} |
        ConvertTo-Json -Depth 20 | Set-Content (Join-Path $payload 'OSF/Smoke/loop-globals.osf.json') -Encoding utf8NoBOM
    $scripts = Join-Path $payload 'Scripts'
    [IO.Directory]::CreateDirectory($scripts) | Out-Null
    Copy-Item (Join-Path $repo 'dist/Scripts/*.pex') $scripts
    $imports = 'C:\Modding\Starfield\PapyrusSource'
    & 'C:\Program Files (x86)\Steam\steamapps\common\Starfield\Tools\Papyrus Compiler\PapyrusCompiler.exe' $PSScriptRoot "-i=$PSScriptRoot;$repo/dist/Scripts/Source;$imports" "-o=$scripts" "-f=$imports/Starfield_Papyrus_Flags.flg" -all
    if ($LASTEXITCODE) { throw 'Papyrus test compilation failed.' }
    [IO.Directory]::CreateDirectory((Join-Path $payload 'SFSE/Plugins')) | Out-Null
    Copy-Item $dll (Join-Path $payload 'SFSE/Plugins/OSF Animation.dll')
    if ($MenuPauses) {
        @{stateDir=(Join-Path $run 'mailbox');launchToken=$runId} | ConvertTo-Json | Set-Content (Join-Path $payload 'SFSE/Plugins/OSFAnimationTestHarness.json') -Encoding utf8NoBOM
    }
    $manifest = @(Get-ChildItem $payload -Recurse -File | ForEach-Object {
        @{relative=[IO.Path]::GetRelativePath($payload,$_.FullName);sha256=(Get-FileHash $_.FullName).Hash}
    })
    $manifest | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $run 'payload-manifest.json')
    foreach ($file in $manifest) {
        if (Test-Path -LiteralPath (Join-Path $output $file.relative)) { throw "Would overwrite existing private output: $($file.relative)" }
    }
    foreach ($file in $manifest) {
        $dest = Join-Path $output $file.relative
        [IO.Directory]::CreateDirectory((Split-Path $dest -Parent)) | Out-Null
        Copy-Item (Join-Path $payload $file.relative) $dest
        $files += $dest
    }
    Copy-Item $pluginList (Join-Path $run 'plugins-before.txt')
    Add-Content $pluginList "`n*$pluginName"
    $pluginAdded = $true
    $started = $true
    $startArgs = @('-Action','Start','-SkipBuild')
    if ($Interactive) { $startArgs += '-Interactive' }
    Run-Harness $startArgs
    if ($MenuPauses) { Test-MenuPauses; return }
    Run-Harness @('-Action','Command','-ConsoleCommand',('cgf "OSFLoopGlobals.Run" "' + $runId + '"'),'-ResultPath',(Join-Path $run 'command.json'))
    $logRoot = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'My Games/Starfield/Logs/Script'
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $raw = ''
    while ($timer.Elapsed.TotalSeconds -lt 150) {
        $log = Get-ChildItem $logRoot -Recurse -File -Filter 'OSFLoopGlobals.0.log' | Select-Object -First 1
        if ($log) {
            $raw = Get-Content $log.FullName -Raw
            if ($raw.Contains("DONE|$runId") -or ($raw.Contains("RUN|$runId") -and $raw.Contains('FAIL|'))) { break }
        }
        Start-Sleep -Seconds 1
    }
    $raw | Set-Content (Join-Path $run 'papyrus.log')
    if (-not $raw.Contains("DONE|$runId") -or $raw.Contains('FAIL|')) { throw 'Papyrus test did not finish successfully; inspect papyrus.log.' }
    $results = foreach ($case in $cases) {
        $id = "osf.global.$($case.id)"
        $escaped = [regex]::Escape($id)
        $begin = [regex]::Match($raw, "CASE\|$escaped\|(\d+)\|(\d+)")
        if (-not $begin.Success) { throw "Missing start evidence: $id" }
        $handle = $begin.Groups[1].Value
        $expected = [int]$begin.Groups[2].Value
        $observed = [regex]::Matches($raw, "EVENT\|$escaped\|$handle\|4\|loop\|").Count
        $completed = [regex]::Matches($raw, "EVENT\|$escaped\|$handle\|16\|\|0(?:\r?\n|$)").Count -eq 1
        @{scene=$id;expected=$expected;observed=$observed;completed=$completed;passed=($observed -eq $expected -and $completed)}
    }
    $report = @{runId=$runId;productionDllSha256=(Get-FileHash $dll).Hash;cases=@($results);passed=(@($results | Where-Object {-not $_.passed}).Count -eq 0)}
    $report | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $run 'result.json')
    $results | ForEach-Object { [pscustomobject]$_ } | Format-Table scene,expected,observed,completed,passed
    if (-not $report.passed) { throw 'Loop count assertions failed.' }
} finally {
    if ($started) { Run-Harness @('-Action','Stop') }
    if ($started) {
        Copy-Item -LiteralPath (Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'My Games/Starfield/SFSE/Logs/OSF Animation.log') -Destination (Join-Path $run 'animation.log') -ErrorAction SilentlyContinue
    }
    if (($started -or $files.Count) -and (Get-Process Starfield -ErrorAction SilentlyContinue)) { throw 'Game is still running; private payload retained for safe cleanup.' }
    if ($pluginAdded) {
        foreach ($list in @($pluginList, (Join-Path $profile 'loadorder.txt'))) {
            if (Test-Path -LiteralPath $list) {
                $lines = @(Get-Content $list | Where-Object { $_ -notmatch '^\*?OSFLoopGlobals\.esm$' })
                [IO.File]::WriteAllLines($list, $lines)
            }
        }
    }
    foreach ($file in $files) {
        $resolved = [IO.Path]::GetFullPath($file)
        if (-not $resolved.StartsWith($output + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Cleanup escaped private output.' }
        Remove-Item -LiteralPath $resolved
    }
    Write-Host "Evidence: $run"
}
