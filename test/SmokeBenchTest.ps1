#requires -Version 7.2
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot '../tools/SmokeBench.ps1')
$scratch = Join-Path ([IO.Path]::GetTempPath()) ('osf-animation-bench-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($scratch) | Out-Null
$checks = 0
function Check([bool]$Value, [string]$Message) {
    if (-not $Value) { throw $Message }
    $script:checks++
}
function Reject([scriptblock]$Body, [string]$Message) {
    $rejected = $false
    try { & $Body } catch { $rejected = $true }
    Check $rejected $Message
}
try {
    Test-SmokeNativeSummary '100% tests passed, 0 test(s) failed out of 8, spent 1s' 8
    $checks++
    Reject { Test-SmokeNativeSummary '100% tests passed, 0 test(s) failed out of 7, spent 1s' 8 } 'A missing suite passed.'
    Reject { Test-SmokeNativeSummary 'build succeeded' 8 } 'A build-only result passed.'
    Reject { Test-SmokeNativeSummary '87% tests passed, 1 test(s) failed out of 8, spent 1s' 8 } 'A failed suite passed.'
    $coverage = @(@{feature='example';native=@('one','two');runtime=@('live');gap='visual check'})
    Test-SmokeCoverage $coverage @('one','two') @('live')
    $checks++
    Reject { Test-SmokeCoverage $coverage @('one','two','new') @('live') } 'Inventory drift passed.'
    Reject { Test-SmokeCoverage $coverage @('one','two') @('live','live') } 'Duplicate game cases passed.'
    Reject { Test-SmokeCoverage $coverage @('one','two') @('different') } 'An unknown game case passed.'
    $inventoryRepo=Join-Path $scratch 'inventory'
    [IO.Directory]::CreateDirectory((Join-Path $inventoryRepo 'src/Testing')) | Out-Null
    Set-Content -LiteralPath (Join-Path $inventoryRepo 'src/Testing/AnimationCases.h') -Value 'constexpr auto cases = { "live" };'
    Test-SmokeCaseInventory $inventoryRepo @('live')
    $checks++
    Reject { Test-SmokeCaseInventory $inventoryRepo @('different') } 'Native/JSON case-name drift passed.'
    Reject { Test-SmokeCaseInventory $inventoryRepo @('live','new') } 'A runtime case with no native assertion passed.'

    $start = [DateTimeOffset]::UtcNow.AddSeconds(-4)
    $finish = [DateTimeOffset]::UtcNow
    $casePath = Join-Path $scratch 'case.json'
    # Mirrors the sibling harness schema.
    $snapshot = @{schemaVersion=1;state='passed';runId='run-123';updatedAtUtcMs=$finish.AddMilliseconds(-500).ToUnixTimeMilliseconds();cases=@(@{name='live';status='passed'})}
    $case = @{
        scenario='AnimationSmoke';outcome='passed';startedAt=$start.AddMilliseconds(300).ToString('o');finishedAt=$finish.AddMilliseconds(-300).ToString('o')
        animationSourceHash='SOURCE';stagedDllSha256='DLL';animationRunId='run-123';animationSnapshot=$snapshot
        animationExpectedCases=@('live');animationStartedAt=$start.AddMilliseconds(400).ToString('o');animationFinishedAt=$finish.AddMilliseconds(-400).ToString('o')
    }
    $case | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $casePath
    $receipt = @{
        automatedPassed=$true;completeSuite=$true;outcome='passed';source=@{sha256='SOURCE'};stagedDllSha256='DLL'
        selectedCases=@('live');runtimeCases=@(@{name='live';status='passed'});snapshot=$snapshot;runId='run-123'
        startedAt=$start.AddMilliseconds(100).ToString('o');finishedAt=$finish.AddMilliseconds(-100).ToString('o')
        runs=@(@{name='animation-runtime';outcome='passed';exitCode=0;result=$casePath
            resultSha256=(Get-FileHash -LiteralPath $casePath).Hash;startedAt=$start.AddMilliseconds(200).ToString('o');finishedAt=$finish.AddMilliseconds(-200).ToString('o')})
    }
    Test-SmokeRuntimeReceipt $receipt @('live') 'SOURCE' 'DLL' $start $finish
    $checks++
    $receiptJson = $receipt | ConvertTo-Json -Depth 30
    $caseJson = $case | ConvertTo-Json -Depth 30
    function Reject-ReceiptMutation([scriptblock]$Mutation, [string]$Message) {
        # Independent copies prevent aliases between the aggregate and leaf snapshots.
        $mutatedReceipt = $receiptJson | ConvertFrom-Json -AsHashtable
        $mutatedCase = $caseJson | ConvertFrom-Json -AsHashtable
        & $Mutation $mutatedReceipt $mutatedCase
        $mutatedCase | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $casePath
        $mutatedReceipt.runs[0].resultSha256 = (Get-FileHash -LiteralPath $casePath).Hash
        Reject { Test-SmokeRuntimeReceipt $mutatedReceipt @('live') 'SOURCE' 'DLL' $start $finish } $Message
    }
    Reject { Test-SmokeRuntimeReceipt $receipt @('live') 'OTHER' 'DLL' $start $finish } 'Mismatched source passed.'
    Reject { Test-SmokeRuntimeReceipt $receipt @('live') 'SOURCE' 'OTHER' $start $finish } 'Mismatched DLL passed.'
    Reject { Test-SmokeRuntimeReceipt $receipt @('live') 'SOURCE' 'DLL' $start.AddSeconds(1) $finish } 'Stale evidence passed.'
    Reject-ReceiptMutation { param($r,$c) $r.runtimeCases[0].status = 'not-run' } 'Skipped case passed.'
    Reject-ReceiptMutation { param($r,$c) $r.automatedPassed = 'true' } 'Truthy string passed.'
    Reject-ReceiptMutation { param($r,$c) $r.selectedCases = @('live','live') } 'Duplicate selected cases passed.'
    Reject-ReceiptMutation { param($r,$c) $r.runtimeCases = @(@{id='live';status='passed'}) } 'Missing canonical case name passed.'
    Reject-ReceiptMutation { param($r,$c) $r.snapshot = $null } 'Missing aggregate native snapshot passed.'
    Reject-ReceiptMutation { param($r,$c) $c.Remove('animationSnapshot') } 'Missing leaf native snapshot passed.'
    Reject-ReceiptMutation { param($r,$c) $c.Remove('animationSourceHash') } 'Missing leaf source hash passed.'
    Reject-ReceiptMutation { param($r,$c) $c.animationSourceHash = 'OTHER' } 'Contradicted leaf source hash passed.'
    Reject-ReceiptMutation { param($r,$c) $c.stagedDllSha256 = 'OTHER' } 'Contradicted leaf DLL hash passed.'
    Reject-ReceiptMutation { param($r,$c) $c.animationRunId = 'another-run' } 'Contradicted leaf run identity passed.'
    Reject-ReceiptMutation { param($r,$c) $r.runId = '' } 'Missing runtime run identity passed.'
    Reject-ReceiptMutation { param($r,$c) $c.animationSnapshot.runId = 'another-run' } 'Contradicted native run identity passed.'
    Reject-ReceiptMutation { param($r,$c) $c.animationSnapshot.state = 'failed' } 'Contradicted native outcome passed.'
    Reject-ReceiptMutation { param($r,$c) $c.animationSnapshot.cases[0].status = 'failed' } 'Contradicted native case result passed.'
    Reject-ReceiptMutation { param($r,$c) $c.animationSnapshot.cases[0].name = 'another-case' } 'Unexpected native case passed.'
    Reject-ReceiptMutation { param($r,$c) $c.animationExpectedCases = @('another-case') } 'Contradicted leaf inventory passed.'
    Reject-ReceiptMutation { param($r,$c) $c.animationSnapshot.schemaVersion = 2 } 'Unknown native snapshot schema passed.'
    Reject-ReceiptMutation { param($r,$c) $c.animationSnapshot.updatedAtUtcMs = $start.AddSeconds(-1).ToUnixTimeMilliseconds() } 'Stale native snapshot passed.'
    Reject-ReceiptMutation { param($r,$c) $r.snapshot.updatedAtUtcMs++ } 'Contradicted aggregate snapshot timestamp passed.'
    Reject-ReceiptMutation { param($r,$c) $c.animationFinishedAt = $finish.AddSeconds(1).ToString('o') } 'Future native completion passed.'
    Reject-ReceiptMutation { param($r,$c) $r.finishedAt = $start.AddSeconds(-1).ToString('o') } 'Reversed aggregate timestamps passed.'
    Reject-ReceiptMutation { param($r,$c) $r.runs[0].finishedAt = $start.ToString('o') } 'Reversed run timestamps passed.'
    $caseJson | Set-Content -LiteralPath $casePath
    $receipt.runs[0].resultSha256 = (Get-FileHash -LiteralPath $casePath).Hash
    Add-Content -LiteralPath $casePath -Value ' '
    Reject { Test-SmokeRuntimeReceipt $receipt @('live') 'SOURCE' 'DLL' $start $finish } 'Changed evidence passed.'

    # Two archives isn't enough: each distinct product must exist.
    function New-TestArchive([string]$Path, [string[]]$Names) {
        $archive = [IO.Compression.ZipFile]::Open($Path, [IO.Compression.ZipArchiveMode]::Create)
        try {
            foreach ($name in $Names) {
                $stream = $archive.CreateEntry($name).Open()
                try { $stream.Write([Text.Encoding]::UTF8.GetBytes('fixture')) } finally { $stream.Dispose() }
            }
        } finally { $archive.Dispose() }
    }
    $engineEntries = @('Core/SFSE/Plugins/OSF Animation.dll','Core/Scripts/OSF.pex','Core/Scripts/OSFTypes.pex',
        'Core/Scripts/OSFAdvanced.pex','Core/OSF/internal.osf.json','Core/SFSE/Plugins/OSF/Settings/schemas/osf.animation.json','fomod/ModuleConfig.xml')
    $browserEntries = @('Browser/SFSE/Plugins/OSF Animation Browser.dll','Browser/SFSE/Plugins/OSF/UI/views/osf.animation/browser/index.html',
        'Browser/SFSE/Plugins/OSF/UI/views/osf.animation/browser/manifest.json','Immersion/OSF/immersion/emotes.osf.json','fomod/ModuleConfig.xml')
    $archiveHash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes('fixture')))
    $validArchives = Join-Path $scratch 'valid-archives'
    [IO.Directory]::CreateDirectory($validArchives) | Out-Null
    New-TestArchive (Join-Path $validArchives 'OSF Animation v1.zip') $engineEntries
    New-TestArchive (Join-Path $validArchives 'OSF Animation Browser v1.zip') $browserEntries
    Test-SmokeArchives $validArchives $archiveHash
    $checks++
    foreach ($product in @('OSF Animation','OSF Animation Browser')) {
        $duplicates = Join-Path $scratch ('duplicate-' + $product)
        [IO.Directory]::CreateDirectory($duplicates) | Out-Null
        foreach ($version in @(1,2)) {
            Copy-Item -LiteralPath (Join-Path $validArchives "$product v1.zip") -Destination (Join-Path $duplicates "$product v$version.zip")
        }
        Reject { Test-SmokeArchives $duplicates $archiveHash } "Two $product archives passed."
    }

    # Fingerprints must include actual dirty/untracked contents, not just HEAD.
    $repo = Join-Path $scratch 'repo with spaces'
    [IO.Directory]::CreateDirectory($repo) | Out-Null
    $null = Invoke-SmokeGit $repo @('init','-q')
    Set-Content -LiteralPath (Join-Path $repo 'tracked.txt') -Value 'first'
    $null = Invoke-SmokeGit $repo @('add','tracked.txt')
    $null = Invoke-SmokeGit $repo @('-c','user.name=Smoke Test','-c','user.email=smoke@localhost','-c','commit.gpgsign=false','commit','-qm','fixture')
    $before = Get-SmokeSourceIdentity $repo
    Set-Content -LiteralPath (Join-Path $repo 'tracked.txt') -Value 'second'
    $changed = Get-SmokeSourceIdentity $repo
    Check ($before.sha256 -cne $changed.sha256 -and $changed.dirty) 'Dirty tracked content was ignored.'
    Set-Content -LiteralPath (Join-Path $repo 'untracked.txt') -Value 'new'
    Check ($changed.sha256 -cne (Get-SmokeSourceIdentity $repo).sha256) 'Untracked content was ignored.'

    $report = [ordered]@{outcome='failed';scope='offline';completeAutomatedSuite=$false;coverage=$coverage;unverified=@('visual check');stages=@(
        @{name='pass';status='passed';seconds=0.1;message='';log=$null},
        @{name='fail';status='failed';seconds=0.1;message='a < b & c';log=$null},
        @{name='block';status='blocked';seconds=0;message='missing';log=$null},
        @{name='skip';status='not-run';seconds=0;message='dependency';log=$null})}
    Save-SmokeReport $report $scratch
    [xml]$junit = Get-Content -LiteralPath (Join-Path $scratch 'junit.xml') -Raw
    Check ($junit.testsuite.failures -eq '1' -and $junit.testsuite.errors -eq '1' -and $junit.testsuite.skipped -eq '1') 'JUnit concealed failed/blocked/skipped stages.'
    $report.runtimeCases=@(@{name='live';status='blocked';message='No live observation'})
    Save-SmokeReport $report $scratch
    [xml]$junit=Get-Content -LiteralPath (Join-Path $scratch 'junit.xml') -Raw
    Check ($junit.testsuite.tests -eq '5' -and $junit.testsuite.errors -eq '2') 'JUnit concealed a blocked game assertion.'
    $markdown=Get-Content -LiteralPath (Join-Path $scratch 'report.md') -Raw
    Check ($markdown.Contains('| live | blocked |') -and $markdown.Contains('| incomplete |')) 'Feature report treated an incomplete game case as passed.'
    Write-Host "Smoke bench contracts: $checks checks passed."
} finally {
    $resolved = [IO.Path]::GetFullPath($scratch)
    $prefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
    if (-not $resolved.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($resolved) -notlike 'osf-animation-bench-*') { throw 'Unsafe scratch cleanup path.' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
