# Shared smoke-bench evidence helpers. No build, deployment, or game actions.
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'ReleaseRules.ps1')

function Invoke-SmokeGit([string]$Repository, [string[]]$Arguments) {
    $start = [Diagnostics.ProcessStartInfo]::new('git')
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    foreach ($argument in (@('-C', $Repository) + $Arguments)) { $start.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::Start($start)
    try {
        $errors = $process.StandardError.ReadToEndAsync()
        $output = $process.StandardOutput.ReadToEnd()
        $process.WaitForExit()
        if ($process.ExitCode) { throw "Git failed in ${Repository}: $($errors.GetAwaiter().GetResult())" }
        return $output
    } finally { $process.Dispose() }
}

function Get-SmokeSourceIdentity([string]$Repository) {
    $Repository = [IO.Path]::GetFullPath($Repository)
    $revision = (Invoke-SmokeGit $Repository @('rev-parse', 'HEAD')).Trim()
    $files = [Collections.Generic.List[object]]::new()
    # NUL-delimited; includes untracked and deleted paths, excludes ignored build output.
    $paths = (Invoke-SmokeGit $Repository @('ls-files', '-z', '--cached', '--others', '--exclude-standard', '--deduplicate')).Split([char]0, [StringSplitOptions]::RemoveEmptyEntries)
    [Array]::Sort($paths, [StringComparer]::Ordinal)
    foreach ($relative in $paths) {
        $path = Join-Path $Repository $relative
        if (Test-Path -LiteralPath $path -PathType Container) {
            # Gitlink: hash its working tree so an edited CommonLib can't reuse old evidence.
            $child = Get-SmokeSourceIdentity $path
            $files.Add([ordered]@{ path=$relative; sha256=$child.sha256; revision=$child.revision })
        } else {
            $hash = if (Test-Path -LiteralPath $path -PathType Leaf) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash } else { 'deleted' }
            $files.Add([ordered]@{ path=$relative; sha256=$hash })
        }
    }
    $bytes = [Text.Encoding]::UTF8.GetBytes((ConvertTo-Json -InputObject @($revision, $files.ToArray()) -Depth 10 -Compress))
    return [ordered]@{
        repository=$Repository; revision=$revision
        sha256=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes))
        dirty=-not [string]::IsNullOrWhiteSpace((Invoke-SmokeGit $Repository @('status', '--porcelain=v1', '--untracked-files=all', '--ignore-submodules=none')))
        files=$files.ToArray()
    }
}

function Test-SmokeNativeSummary([string]$Log, [int]$ExpectedCount) {
    $plain = [regex]::Replace($Log, '\x1B\[[0-?]*[ -/]*[@-~]', '')
    $summary = [regex]::Match($plain, '(?m)^100% tests passed, 0 test\(s\) failed out of (\d+),')
    if (-not $summary.Success -or [int]$summary.Groups[1].Value -ne $ExpectedCount) {
        throw '[blocked] XMake did not report the expected number of executed passing tests.'
    }
}

function Read-SmokeNativeCases([string]$Log) {
    $plain = [regex]::Replace($Log, '\x1B\[[0-?]*[ -/]*[@-~]', '')
    foreach ($match in [regex]::Matches($plain, '(?m)^\[\s*\d+%\]:\s+(osf-[a-z0-9-]+/[^\s]+)\s+[. ]*(passed|failed)\s+([0-9.]+)s\s*$')) {
        @{name=$match.Groups[1].Value; status=$match.Groups[2].Value; message='XMake executed suite';
            seconds=[double]::Parse($match.Groups[3].Value, [Globalization.CultureInfo]::InvariantCulture); log=$null}
    }
}

function Test-SmokeNativeCases([string]$Log, [string[]]$Targets) {
    Test-SmokeNativeSummary $Log $Targets.Count
    $cases = @(Read-SmokeNativeCases $Log)
    $expected = @($Targets | ForEach-Object { "$_/default" })
    if ($cases.Count -ne $expected.Count -or @(Compare-Object $expected @($cases.name) -CaseSensitive).Count -or
        @($cases | Where-Object status -ne 'passed').Count) {
        throw '[blocked] XMake suite identities do not match the registered tests.'
    }
}

function Read-SmokeBrowserCases($Receipt, [string]$Root) {
    foreach ($file in $Receipt.testResults) {
        $relative = [IO.Path]::GetRelativePath($Root, $file.name).Replace('\','/')
        foreach ($case in $file.assertionResults) {
            @{name=($relative + ': ' + $case.fullName); status=$case.status;
                message=($case.failureMessages -join "`n"); seconds=([double]$case.duration / 1000); log=$null}
        }
    }
}

function Test-SmokeBrowserReceipt($Receipt, [string]$Root, [string[]]$ExpectedFiles, [DateTimeOffset]$StartedAt) {
    if ($Receipt.success -isnot [bool] -or -not $Receipt.success -or $Receipt.numTotalTests -le 0 -or
        $Receipt.numPassedTests -ne $Receipt.numTotalTests -or $Receipt.numFailedTests -ne 0 -or
        $Receipt.numPendingTests -ne 0 -or $Receipt.numTodoTests -ne 0 -or
        [long]$Receipt.startTime -lt $StartedAt.ToUnixTimeMilliseconds() -or
        [long]$Receipt.startTime -gt [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()) {
        throw '[blocked] Browser receipt is stale, incomplete, or contains skipped/failed tests.'
    }
    $files = @($Receipt.testResults | ForEach-Object { [IO.Path]::GetRelativePath($Root, $_.name).Replace('\','/') })
    if ($files.Count -ne $ExpectedFiles.Count -or @(Compare-Object $files $ExpectedFiles -CaseSensitive).Count -or
        @($Receipt.testResults | Where-Object { $_.status -ne 'passed' -or -not $_.assertionResults.Count }).Count) {
        throw '[blocked] Browser test file inventory does not match the executed receipt.'
    }
    $cases = @(Read-SmokeBrowserCases $Receipt $Root)
    if ($cases.Count -ne $Receipt.numTotalTests -or @($cases | Where-Object status -ne 'passed').Count) {
        throw '[blocked] Browser assertions contradict the aggregate receipt.'
    }
}

function Read-SmokeCaseResult([string]$Path, [DateTimeOffset]$StartedAt, [DateTimeOffset]$FinishedAt, [int]$ExitCode) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Missing scenario receipt: $Path" }
    $result = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    $expected = switch ($ExitCode) { 0 { 'passed' } 1 { 'failed' } 2 { 'blocked' } default { throw "Unexpected scenario exit code: $ExitCode" } }
    if ($result.scenario -cne 'AnimationSmoke' -or $result.outcome -cne $expected) { throw 'Scenario receipt disagrees with the requested scenario or exit code.' }
    # Cast, not Parse: ConvertFrom-Json may yield DateTime, and Parse loses offset/fractions.
    $start = [DateTimeOffset]$result.startedAt
    $finish = [DateTimeOffset]$result.finishedAt
    if ($start -lt $StartedAt -or $finish -lt $start -or $finish -gt $FinishedAt) { throw 'Scenario receipt is stale or has invalid timestamps.' }
    return $result
}

function Test-SmokePassedCases($Cases, [string[]]$ExpectedCases) {
    if (@($Cases).Count -ne $ExpectedCases.Count -or
        @($Cases.name | Sort-Object -Unique).Count -ne $ExpectedCases.Count) {
        throw 'Runtime snapshot has missing or duplicate cases.'
    }
    foreach ($case in $Cases) {
        if ($case.name -cnotin $ExpectedCases -or $case.status -cne 'passed') {
            throw "Runtime case did not pass: $($case.name)"
        }
    }
}

function Test-SmokePassedSnapshot($Snapshot, [string[]]$ExpectedCases, [string]$RunId) {
    if (-not $Snapshot -or $Snapshot.schemaVersion -ne 1 -or $Snapshot.state -cne 'passed' -or
        $Snapshot.runId -cne $RunId) { throw 'Runtime snapshot has a mismatched schema, state, or run identity.' }
    Test-SmokePassedCases $Snapshot.cases $ExpectedCases
}

function Test-SmokeRuntimeReceipt($Receipt, [string[]]$ExpectedCases, [string]$SourceHash,
    [string]$DllHash, [DateTimeOffset]$StartedAt, [DateTimeOffset]$FinishedAt) {
    if ($Receipt.automatedPassed -isnot [bool] -or -not $Receipt.automatedPassed -or
        $Receipt.completeSuite -isnot [bool] -or -not $Receipt.completeSuite -or
        $Receipt.outcome -cne 'passed') { throw 'Runtime receipt is not a complete passing suite.' }
    if ($Receipt.source.sha256 -cne $SourceHash) { throw 'Runtime receipt tested different source content.' }
    if ($Receipt.stagedDllSha256 -cne $DllHash) { throw 'Runtime receipt tested a different DLL.' }
    if (-not $ExpectedCases.Count -or @($ExpectedCases | Sort-Object -Unique).Count -ne $ExpectedCases.Count) { throw 'Expected runtime case inventory is empty or duplicated.' }
    if (@($Receipt.selectedCases).Count -ne $ExpectedCases.Count -or
        @(Compare-Object @($ExpectedCases | Sort-Object) @($Receipt.selectedCases | Sort-Object) -CaseSensitive).Count -or
        @($Receipt.runs).Count -ne 1) { throw 'Runtime receipt has missing, duplicate, or unexpected cases/evidence.' }
    if ($Receipt.runId -isnot [string] -or [string]::IsNullOrWhiteSpace($Receipt.runId)) { throw 'Runtime receipt has no run identity.' }
    Test-SmokePassedCases $Receipt.runtimeCases $ExpectedCases
    Test-SmokePassedSnapshot $Receipt.snapshot $ExpectedCases $Receipt.runId
    $receiptStart = [DateTimeOffset]$Receipt.startedAt
    $receiptFinish = [DateTimeOffset]$Receipt.finishedAt
    if ($receiptStart -lt $StartedAt -or $receiptFinish -lt $receiptStart -or $receiptFinish -gt $FinishedAt) {
        throw 'Runtime receipt is stale or has invalid timestamps.'
    }
    foreach ($run in $Receipt.runs) {
        if ($run.name -cne 'animation-runtime' -or $run.outcome -cne 'passed' -or $run.exitCode -ne 0 -or
            -not $run.result -or -not (Test-Path -LiteralPath $run.result -PathType Leaf)) { throw "Runtime case lacks passing evidence: $($run.name)" }
        $runStart = [DateTimeOffset]$run.startedAt
        $runFinish = [DateTimeOffset]$run.finishedAt
        if ($runStart -lt $receiptStart -or $runFinish -lt $runStart -or $runFinish -gt $receiptFinish) { throw 'Runtime run has invalid timestamp bounds.' }
        $evidence = Read-SmokeCaseResult $run.result $runStart $runFinish $run.exitCode
        if ((Get-FileHash -LiteralPath $run.result).Hash -cne $run.resultSha256) { throw "Runtime case has changed evidence: $($run.name)" }
        if ($evidence.animationSourceHash -cne $SourceHash -or $evidence.stagedDllSha256 -cne $DllHash -or
            $evidence.animationRunId -cne $Receipt.runId) { throw 'Scenario evidence disagrees with runtime source, DLL, or run identity.' }
        Test-SmokePassedSnapshot $evidence.animationSnapshot $ExpectedCases $Receipt.runId
        if (@($evidence.animationExpectedCases).Count -ne $ExpectedCases.Count -or
            @(Compare-Object @($ExpectedCases | Sort-Object) @($evidence.animationExpectedCases | Sort-Object) -CaseSensitive).Count) {
            throw 'Scenario evidence tested a different native case inventory.'
        }
        $animationStart = [DateTimeOffset]$evidence.animationStartedAt
        $animationFinish = [DateTimeOffset]$evidence.animationFinishedAt
        if ($animationStart -lt [DateTimeOffset]$evidence.startedAt -or $animationFinish -lt $animationStart -or
            $animationFinish -gt [DateTimeOffset]$evidence.finishedAt) { throw 'Native case evidence has invalid timestamp bounds.' }
        $snapshotTime = [long]$evidence.animationSnapshot.updatedAtUtcMs
        if ($snapshotTime -lt $animationStart.ToUnixTimeMilliseconds() -or $snapshotTime -gt $animationFinish.ToUnixTimeMilliseconds() -or
            [long]$Receipt.snapshot.updatedAtUtcMs -ne $snapshotTime) { throw 'Native snapshot is stale or disagrees with the aggregate snapshot.' }
    }
}

function Set-SmokeUnverifiedCases($Report, $Stage) {
    if ($Stage.status -ne 'blocked') { return }
    $kind = switch ($Stage.name) { native { 'nativeCases' } browser { 'browserCases' } runtime { 'runtimeCases' } }
    if (-not $kind -or -not $Report.Contains($kind)) { return }
    # Cases are imported before validation so genuine failures remain visible. A blocked
    # stage never establishes its raw passing observations; retain those in the source
    # receipt/log, but mark the report unverified. Failed stages keep observed partial passes.
    foreach ($case in $Report[$kind]) {
        if ($case.status -eq 'passed') {
            $case.status = 'blocked'
            $case.message = "Unverified: $($Stage.name) source receipt was not validated. $($Stage.message) Original observation: $($case.message)"
        }
    }
}

function Save-SmokeReport($Report, [string]$Directory) {
    [IO.File]::WriteAllText((Join-Path $Directory 'result.json'), ($Report | ConvertTo-Json -Depth 30))
    $lines = @('# OSF Animation smoke bench', '', "Outcome: **$($Report.outcome)**", '',
        "Scope: $($Report.scope)", "Complete automated suite: $($Report.completeAutomatedSuite)",
        '', '| Stage | Result | Seconds | Evidence / detail |', '| --- | --- | ---: | --- |')
    foreach ($stage in $Report.stages) {
        $link = if ($stage.log) { "[$($stage.name)](<$($stage.log)>) " } else { '' }
        $lines += "| $($stage.name) | $($stage.status) | $($stage.seconds) | $link$($stage.message.Replace('|','/').Replace("`n",' ')) |"
    }
    $offline = @(foreach ($kind in @('nativeCases','browserCases')) {
        if ($Report.Contains($kind)) { foreach ($case in $Report[$kind]) {
            @{name=($kind.Replace('Cases','') + '.' + $case.name);status=$case.status;message=$case.message;seconds=$case.seconds;log=$case.log}
        } }
    })
    if ($offline.Count) {
        $lines += @('', '## Offline assertions', '', '| Test | Result | Seconds |', '| --- | --- | ---: |')
        foreach ($case in $offline) { $lines += "| $($case.name.Replace('|','/')) | $($case.status) | $($case.seconds) |" }
    }
    if ($Report.Contains('runtime') -and $Report.runtime) { $lines += @('', "Game suite receipt: [$($Report.runtime)](<$($Report.runtime)>)") }
    $cases = @(if ($Report.Contains('runtimeCases')) { $Report.runtimeCases })
    if ($Report.Contains('hosts')) { $lines += @('', "Host selection: **$($Report.hosts)** (exact DLL hashes are retained in the game receipt).") }
    if ($cases.Count) {
        $lines += @('', '## In-game assertions', '', '| Case | Result | Observation |', '| --- | --- | --- |')
        foreach ($case in $cases) { $lines += "| $($case.name) | $($case.status) | $($case.message.Replace('|','/').Replace("`n",' ')) |" }
    }
    $lines += @('', '## Coverage', '', '| Feature | Native suites | Game cases | Automatic game result |', '| --- | --- | --- | --- |')
    foreach ($feature in $Report.coverage) {
        $mapped=@($cases | Where-Object name -in $feature.runtime)
        $status=if (-not $feature.runtime.Count) { 'no live assertion' }
            elseif (@($mapped | Where-Object status -eq 'failed').Count) { 'failed' }
            elseif ($mapped.Count -ne $feature.runtime.Count -or @($mapped | Where-Object status -ne 'passed').Count) { 'incomplete' }
            else { 'passed (boundaries below)' }
        $lines += "| $($feature.feature) | $($feature.native -join ', ') | $($feature.runtime -join ', ') | $status |"
    }
    $lines += @('', '## Not established by this run', '')
    foreach ($gate in $Report.unverified) { $lines += "- $gate" }
    [IO.File]::WriteAllLines((Join-Path $Directory 'report.md'), $lines)
    $settings = [Xml.XmlWriterSettings]::new(); $settings.Indent = $true
    $xml = [Xml.XmlWriter]::Create((Join-Path $Directory 'junit.xml'), $settings)
    try {
        $xml.WriteStartElement('testsuite'); $xml.WriteAttributeString('name', 'OSF Animation smoke bench')
        $entries=@($Report.stages) + $offline + @($cases | ForEach-Object { @{name=('game.'+$_.name);status=$_.status;message=$_.message;seconds=0;log=$null} })
        $xml.WriteAttributeString('tests', [string]$entries.Count)
        $xml.WriteAttributeString('failures', [string]@($entries | Where-Object status -eq 'failed').Count)
        $xml.WriteAttributeString('errors', [string]@($entries | Where-Object status -eq 'blocked').Count)
        $xml.WriteAttributeString('skipped', [string]@($entries | Where-Object status -notin @('passed','failed','blocked')).Count)
        foreach ($stage in $entries) {
            $xml.WriteStartElement('testcase'); $xml.WriteAttributeString('name', $stage.name)
            $xml.WriteAttributeString('time', ([double]$stage.seconds).ToString([Globalization.CultureInfo]::InvariantCulture))
            $element = switch ($stage.status) { failed { 'failure' } blocked { 'error' } passed { '' } default { 'skipped' } }
            if ($element) { $xml.WriteElementString($element, $stage.message) }
            if ($stage.log) { $xml.WriteElementString('system-out', $stage.log) }
            $xml.WriteEndElement()
        }
        $xml.WriteEndElement()
    } finally { $xml.Dispose() }
}

function Get-SmokeNativeTargets([string]$Repository) {
    $source = Get-Content -LiteralPath (Join-Path $Repository 'xmake.lua') -Raw
    foreach ($match in [regex]::Matches($source, '(?ms)^target\("([^"]+)"\)(.*?)(?=^target\(|\z)')) {
        if ($match.Groups[2].Value -match '\badd_tests\(') { $match.Groups[1].Value }
    }
}

function Test-SmokeCaseInventory([string]$Repository, [string[]]$RuntimeCases) {
    $header=Get-Content -LiteralPath (Join-Path $Repository 'src/Testing/AnimationCases.h') -Raw
    $compiled=@([regex]::Matches($header,'"([a-z][a-z0-9.-]+)"') | ForEach-Object { $_.Groups[1].Value })
    if ($compiled.Count -ne $RuntimeCases.Count -or @(Compare-Object $compiled $RuntimeCases -CaseSensitive).Count) {
        throw '[failed] Compiled runtime case inventory differs from runtime-cases.json.'
    }
}

function Test-SmokeHarnessPlan($Plan, [string[]]$Cases, [string]$Hosts) {
    $planData = $Plan | ConvertTo-Json -Depth 12 | ConvertFrom-Json -AsHashtable
    if (-not $planData.ContainsKey('protocolVersion') -or $planData.protocolVersion -ne 2 -or
        -not $planData.ContainsKey('hosts') -or $Hosts -cnotin $planData.hosts -or
        -not $planData.ContainsKey('externalActions') -or
        @(@('quicksave','quickload','browser-imports') | Where-Object { $_ -cnotin $planData.externalActions }).Count) {
        throw '[blocked] Animation harness is outdated: protocol 2 with host staging and save/load/browser input support is required.'
    }
    if ($planData.cases.Count -ne $Cases.Count -or @(Compare-Object $planData.cases $Cases -CaseSensitive).Count) {
        throw '[blocked] Runtime harness and Animation case inventories differ.'
    }
}

function Stage-SmokeBrowser([string]$Repository, [string]$RuntimeMod) {
    $viewRelative = 'SFSE/Plugins/OSF/UI/views/osf.animation'
    $builtView = Join-Path $Repository ('build/osfui-animation-browser/' + $viewRelative)
    $stagedView = Join-Path $RuntimeMod $viewRelative
    if (Test-Path -LiteralPath $stagedView) { throw '[blocked] Browser smoke staging requires a fresh view destination.' }
    foreach ($required in @('browser/index.html','browser/manifest.json')) {
        if (-not (Test-Path -LiteralPath (Join-Path $builtView $required))) { throw "[blocked] Built browser is missing $required." }
    }
    $observer = Join-Path $Repository 'test/smoke/browser-observer.js'
    if (-not (Test-Path -LiteralPath $observer)) { throw '[blocked] Browser smoke observer is missing.' }
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($stagedView)) | Out-Null
    Copy-Item -LiteralPath $builtView -Destination $stagedView -Recurse
    $index = Join-Path $stagedView 'browser/index.html'
    $html = [IO.File]::ReadAllText($index)
    if ([regex]::Matches($html,'</body>').Count -ne 1 -or $html.Contains('smoke-observer.js')) {
        throw '[blocked] Unexpected browser document; cannot install private observer.'
    }
    Copy-Item -LiteralPath $observer -Destination (Join-Path $stagedView 'browser/smoke-observer.js')
    [IO.File]::WriteAllText($index, $html.Replace('</body>', '<script src="./smoke-observer.js"></script></body>'))
    $content = Join-Path $RuntimeMod 'OSF'
    [IO.Directory]::CreateDirectory($content) | Out-Null
    foreach ($folder in @('vanilla','immersion')) {
        Copy-Item -LiteralPath (Join-Path $Repository "dist/OSF/$folder") -Destination $content -Recurse
    }
}

function Test-SmokeCoverage($Coverage, [string[]]$NativeTargets, [string[]]$RuntimeCases) {
    $mapped = @($Coverage.native | Sort-Object -Unique)
    if (-not $NativeTargets.Count -or @(Compare-Object $NativeTargets $mapped).Count) {
        throw '[failed] Native suite inventory and test/smoke/coverage.json disagree.'
    }
    if (-not $RuntimeCases.Count -or @($RuntimeCases | Sort-Object -Unique).Count -ne $RuntimeCases.Count) {
        throw '[failed] Runtime case inventory is empty or duplicated.'
    }
    if (@(Compare-Object @($Coverage.runtime | Sort-Object -Unique) $RuntimeCases).Count) {
        throw '[failed] Runtime case inventory and coverage map disagree.'
    }
    foreach ($row in $Coverage) {
        if (-not $row.feature -or -not $row.gap) { throw '[failed] Each feature needs a name and explicit acceptance boundary.' }
    }
}

function Test-SmokeArchives([string]$Directory, [string]$ExpectedDllHash) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archives = @(Get-ChildItem -LiteralPath $Directory -Filter '*.zip' -File)
    if ($archives.Count -ne 2 -or
        @($archives | Where-Object Name -cmatch '^OSF Animation v.+\.zip$').Count -ne 1 -or
        @($archives | Where-Object Name -cmatch '^OSF Animation Browser v.+\.zip$').Count -ne 1) {
        throw '[failed] Expected exactly one fresh engine archive and one fresh browser archive.'
    }
    foreach ($file in $archives) {
        $zip = [IO.Compression.ZipFile]::OpenRead($file.FullName)
        try {
            $names = @($zip.Entries | ForEach-Object { $_.FullName.Replace('\','/') })
            if (@($names | Where-Object { $_ -like '*/schemas/osf.animation.json' }).Count) {
                throw '[failed] Obsolete combined settings schema leaked into archive.'
            }
            if (@($names | Where-Object { Test-ReleaseForbiddenEntry $_ }).Count) {
                throw "[failed] Development or player-state payload leaked into $($file.Name)."
            }
            $engine = $file.Name -notlike 'OSF Animation Browser*'
            $required = if ($engine) { @('Core/SFSE/Plugins/OSF Animation.dll','Core/Scripts/OSF.pex',
                'Core/Scripts/OSFTypes.pex','Core/Scripts/OSFAdvanced.pex','Core/OSF/internal.osf.json',
                'Core/SFSE/Plugins/OSF/Settings/schemas/osf.animation.engine.json','Core/SFSE/Plugins/OSF/Settings/schemas/osf.animation.align.json','fomod/ModuleConfig.xml') }
                else { @('Browser/SFSE/Plugins/OSF Animation Browser.dll','Browser/SFSE/Plugins/OSF/UI/views/osf.animation/browser/index.html',
                    'Browser/SFSE/Plugins/OSF/Settings/schemas/osf.animation.browser.json',
                    'Browser/SFSE/Plugins/OSF/UI/views/osf.animation/browser/manifest.json',
                    'Immersion/OSF/immersion/emotes.osf.json','fomod/ModuleConfig.xml') }
            foreach ($name in $required) { if ($name -cnotin $names) { throw "[failed] Archive is missing $name." } }
            if ($engine) {
                if (@($names | Where-Object { $_ -like '*UI/views/*' -or $_ -like '*OSF Animation Browser.dll' -or $_ -like '*/schemas/osf.animation.browser.json' }).Count) { throw '[failed] Browser leaked into engine archive.' }
                $entry = @($zip.Entries | Where-Object { $_.FullName.Replace('\','/') -ceq 'Core/SFSE/Plugins/OSF Animation.dll' })[0]
                $stream = $entry.Open()
                try { $hash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($stream)) }
                finally { $stream.Dispose() }
                if ($hash -cne $ExpectedDllHash) { throw '[failed] Engine archive contains a different DLL from the production build.' }
            } elseif (@($names | Where-Object { ($_ -like '*.dll' -and $_ -cne 'Browser/SFSE/Plugins/OSF Animation Browser.dll') -or $_ -like '*.pex' }).Count) {
                throw '[failed] Engine or foreign native payload leaked into browser addon.'
            }
        } finally { $zip.Dispose() }
    }
}

# Exercise the installer folder choices and repeated payload overlays in a fresh
# sandbox. This checks file layout/preservation, not MO2 or in-game migration.
function Test-SmokeInstall([string]$ArchiveDirectory, [string]$Destination) {
    if (Test-Path -LiteralPath $Destination) { throw '[blocked] Install test requires a fresh destination.' }
    $archives = @(Get-ChildItem -LiteralPath $ArchiveDirectory -Filter '*.zip' -File)
    $engine = @($archives | Where-Object Name -cmatch '^OSF Animation v.+\.zip$')
    $browser = @($archives | Where-Object Name -cmatch '^OSF Animation Browser v.+\.zip$')
    if ($engine.Count -ne 1 -or $browser.Count -ne 1) { throw '[failed] Install test needs both product archives.' }
    $variants = @(
        @{name='engine-only';roots=@()}, @{name='browser-minimal';roots=@('Browser')},
        @{name='browser-library';roots=@('Browser','Library')}, @{name='browser-emotes';roots=@('Browser','Immersion')},
        @{name='browser-all';roots=@('Browser','Library','Immersion')})
    foreach ($variant in $variants) {
        $root = [IO.Path]::GetFullPath((Join-Path $Destination $variant.name))
        $prefix = $root.TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
        $sentinels = @('OSF/user/custom.osf.json','OSF/clip-durations.json',
            'SFSE/Plugins/OSF/Settings/values/osf.animation.browser.json','SFSE/Plugins/OSF/Settings/schemas/another-mod.json')
        foreach ($relative in $sentinels) {
            $path = Join-Path $root $relative
            [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path)) | Out-Null
            [IO.File]::WriteAllText($path, 'preserve:' + $relative)
        }
        $expected = @{}
        foreach ($pass in 1..2) {
            $written = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
            foreach ($product in @(@{file=$engine[0];roots=@('Core')},@{file=$browser[0];roots=$variant.roots})) {
                if (-not $product.roots.Count) { continue }
                $zip = [IO.Compression.ZipFile]::OpenRead($product.file.FullName)
                try {
                    $configEntry = $zip.GetEntry('fomod/ModuleConfig.xml')
                    if (-not $configEntry) { throw '[failed] Missing FOMOD installer.' }
                    $reader = [IO.StreamReader]::new($configEntry.Open())
                    try { [xml]$config = $reader.ReadToEnd() } finally { $reader.Dispose() }
                    if ($config.SelectNodes('//file').Count) { throw '[failed] Installer file rules changed; update the install matrix.' }
                    $required = @($config.SelectNodes('/config/requiredInstallFiles/folder') | ForEach-Object { $_.source })
                    $requiredRoot = if ($product.roots -contains 'Core') { 'Core' } else { 'Browser' }
                    if ($required.Count -ne 1 -or $required[0] -cne $requiredRoot) { throw '[failed] Installer required folders changed.' }
                    $groups = @($config.SelectNodes('/config/installSteps/installStep/optionalFileGroups/group'))
                    $optionalRoots = @(foreach ($group in $groups) {
                        if ($group.type -cne 'SelectExactlyOne') { throw '[failed] Installer selection policy changed.' }
                        $choices = @($group.SelectNodes('plugins/plugin'))
                        $withFiles = @($choices | Where-Object { $_.SelectNodes('files/folder').Count -eq 1 })
                        $withoutFiles = @($choices | Where-Object { $_.SelectNodes('files/*').Count -eq 0 })
                        if ($choices.Count -ne 2 -or $withFiles.Count -ne 1 -or $withoutFiles.Count -ne 1) { throw '[failed] Installer requires one include and one skip choice per content group.' }
                        $withFiles[0].SelectSingleNode('files/folder').source
                    })
                    if (($requiredRoot -eq 'Core' -and $groups.Count) -or ($requiredRoot -eq 'Browser' -and
                        ($groups.Count -ne 2 -or @(Compare-Object @('Library','Immersion') $optionalRoots -CaseSensitive).Count))) {
                        throw '[failed] Installer optional content choices changed.'
                    }
                    $sources = @($config.SelectNodes('//folder') | ForEach-Object {
                        if ($_.destination) { throw '[failed] Installer destination changed; update the install tests.' }
                        $_.source
                    })
                    $allowed = @(if ($product.roots -contains 'Core') { 'Core' } else { 'Browser'; 'Library'; 'Immersion' })
                    if ($sources.Count -ne $allowed.Count -or @(Compare-Object $sources $allowed -CaseSensitive).Count) {
                        throw '[failed] Installer choices changed; update the install matrix.'
                    }
                    foreach ($folder in $product.roots) {
                        $entries = @($zip.Entries | Where-Object { $_.FullName.StartsWith($folder + '/', [StringComparison]::Ordinal) -and $_.Name })
                        if (-not $entries.Count) { throw "[failed] Empty installer folder: $folder" }
                        foreach ($entry in $entries) {
                            $relative = $entry.FullName.Substring($folder.Length + 1)
                            if ([IO.Path]::IsPathRooted($relative) -or $relative -match '(^|[/\\])\.\.([/\\]|$)|:') { throw '[failed] Unsafe archive path.' }
                            $path = [IO.Path]::GetFullPath((Join-Path $root $relative))
                            if (-not $path.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)) { throw '[failed] Archive path escapes install sandbox.' }
                            if (-not $written.Add($path)) { throw '[failed] Installer payloads collide at a destination path.' }
                            if ($relative -in $sentinels) { throw '[failed] Archive overwrites player/other-mod data.' }
                            [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path)) | Out-Null
                            $inputStream = $entry.Open(); $outputStream = [IO.File]::Create($path)
                            try { $inputStream.CopyTo($outputStream) } finally { $inputStream.Dispose(); $outputStream.Dispose() }
                            $inputStream = $entry.Open()
                            try { $expected[$relative]=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($inputStream)) }
                            finally { $inputStream.Dispose() }
                        }
                    }
                } finally { $zip.Dispose() }
            }
            foreach ($relative in $expected.Keys) {
                if ((Get-FileHash -LiteralPath (Join-Path $root $relative)).Hash -cne $expected[$relative]) { throw '[failed] Installed payload hash mismatch.' }
            }
            foreach ($relative in $sentinels) {
                if ([IO.File]::ReadAllText((Join-Path $root $relative)) -cne ('preserve:' + $relative)) { throw '[failed] Reinstall changed retained data.' }
            }
        }
        $installed = @(Get-ChildItem -LiteralPath $root -File -Recurse)
        if ($installed.Count -ne $expected.Count + $sentinels.Count) { throw '[failed] Unexpected installed files.' }
        @{name=$variant.name;status='passed';payloadFiles=$expected.Count;overlays=2;preserved=$sentinels;directory=$root}
    }
}
