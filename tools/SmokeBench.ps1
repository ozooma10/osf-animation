# Shared smoke-bench evidence helpers. No build, deployment, or game actions.
Set-StrictMode -Version Latest

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
    # NUL delimiters preserve spaces, Unicode and unusual filenames. Include
    # untracked sources and deleted tracked paths; ignored build output is excluded.
    $paths = (Invoke-SmokeGit $Repository @('ls-files', '-z', '--cached', '--others', '--exclude-standard', '--deduplicate')).Split([char]0, [StringSplitOptions]::RemoveEmptyEntries)
    [Array]::Sort($paths, [StringComparer]::Ordinal)
    foreach ($relative in $paths) {
        $path = Join-Path $Repository $relative
        if (Test-Path -LiteralPath $path -PathType Container) {
            # Gitlinks are directories in ls-files. Hash their actual working
            # trees too, so an edited CommonLib cannot borrow old test evidence.
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

function Read-SmokeCaseResult([string]$Path, [DateTimeOffset]$StartedAt, [DateTimeOffset]$FinishedAt, [int]$ExitCode) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Missing scenario receipt: $Path" }
    $result = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    $expected = switch ($ExitCode) { 0 { 'passed' } 1 { 'failed' } 2 { 'blocked' } default { throw "Unexpected scenario exit code: $ExitCode" } }
    if ($result.scenario -cne 'AnimationSmoke' -or $result.outcome -cne $expected) { throw 'Scenario receipt disagrees with the requested scenario or exit code.' }
    # ConvertFrom-Json may return DateTime objects; Parse would stringify them
    # with local formatting, losing the offset and fractional seconds.
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

function Save-SmokeReport($Report, [string]$Directory) {
    [IO.File]::WriteAllText((Join-Path $Directory 'result.json'), ($Report | ConvertTo-Json -Depth 30))
    $lines = @('# OSF Animation smoke bench', '', "Outcome: **$($Report.outcome)**", '',
        "Scope: $($Report.scope)", "Complete automated suite: $($Report.completeAutomatedSuite)",
        '', '| Stage | Result | Seconds | Evidence / detail |', '| --- | --- | ---: | --- |')
    foreach ($stage in $Report.stages) {
        $link = if ($stage.log) { "[$($stage.name)](<$($stage.log)>) " } else { '' }
        $lines += "| $($stage.name) | $($stage.status) | $($stage.seconds) | $link$($stage.message.Replace('|','/').Replace("`n",' ')) |"
    }
    if ($Report.Contains('runtime') -and $Report.runtime) { $lines += @('', "Game suite receipt: [$($Report.runtime)](<$($Report.runtime)>)") }
    $lines += @('', '## Coverage', '', '| Feature | Native suites | Game cases |', '| --- | --- | --- |')
    foreach ($feature in $Report.coverage) { $lines += "| $($feature.feature) | $($feature.native -join ', ') | $($feature.runtime -join ', ') |" }
    $lines += @('', '## Not established by this run', '')
    foreach ($gate in $Report.unverified) { $lines += "- $gate" }
    [IO.File]::WriteAllLines((Join-Path $Directory 'report.md'), $lines)
    $settings = [Xml.XmlWriterSettings]::new(); $settings.Indent = $true
    $xml = [Xml.XmlWriter]::Create((Join-Path $Directory 'junit.xml'), $settings)
    try {
        $xml.WriteStartElement('testsuite'); $xml.WriteAttributeString('name', 'OSF Animation smoke bench')
        $xml.WriteAttributeString('tests', [string]$Report.stages.Count)
        $xml.WriteAttributeString('failures', [string]@($Report.stages | Where-Object status -eq 'failed').Count)
        $xml.WriteAttributeString('errors', [string]@($Report.stages | Where-Object status -eq 'blocked').Count)
        $xml.WriteAttributeString('skipped', [string]@($Report.stages | Where-Object status -in @('not-run','running')).Count)
        foreach ($stage in $Report.stages) {
            $xml.WriteStartElement('testcase'); $xml.WriteAttributeString('name', $stage.name)
            $xml.WriteAttributeString('time', ([double]$stage.seconds).ToString([Globalization.CultureInfo]::InvariantCulture))
            $element = switch ($stage.status) { failed { 'failure' } blocked { 'error' } not-run { 'skipped' } running { 'skipped' } default { '' } }
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
            if (@($names | Where-Object { $_ -match '(?i)(OSFTest\.(pex|psc)|AnimationTestHarness|smoke|\.map$|(^|/)values/)' }).Count) {
                throw "[failed] Development or player-state payload leaked into $($file.Name)."
            }
            $engine = $file.Name -notlike 'OSF Animation Browser*'
            $required = if ($engine) { @('Core/SFSE/Plugins/OSF Animation.dll','Core/Scripts/OSF.pex',
                'Core/Scripts/OSFTypes.pex','Core/Scripts/OSFAdvanced.pex','Core/OSF/internal.osf.json',
                'Core/SFSE/Plugins/OSF/Settings/schemas/osf.animation.json','fomod/ModuleConfig.xml') }
                else { @('Browser/SFSE/Plugins/OSF/UI/views/osf.animation/browser/index.html',
                    'Browser/SFSE/Plugins/OSF/UI/views/osf.animation/browser/manifest.json',
                    'Immersion/OSF/immersion/emotes.osf.json','fomod/ModuleConfig.xml') }
            foreach ($name in $required) { if ($name -cnotin $names) { throw "[failed] Archive is missing $name." } }
            if ($engine) {
                if (@($names | Where-Object { $_ -like '*UI/views/*' }).Count) { throw '[failed] Browser leaked into engine archive.' }
                $entry = @($zip.Entries | Where-Object { $_.FullName.Replace('\','/') -ceq 'Core/SFSE/Plugins/OSF Animation.dll' })[0]
                $stream = $entry.Open()
                try { $hash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($stream)) }
                finally { $stream.Dispose() }
                if ($hash -cne $ExpectedDllHash) { throw '[failed] Engine archive contains a different DLL from the production build.' }
            } elseif (@($names | Where-Object { $_ -like '*.dll' -or $_ -like '*.pex' }).Count) {
                throw '[failed] Native payload leaked into browser addon.'
            }
        } finally { $zip.Dispose() }
    }
}
