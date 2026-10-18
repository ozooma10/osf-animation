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
    try { $null = & $Body } catch { $rejected = $true }
    Check $rejected $Message
}
try {
    # Packaging invokes the boundary check in an inherited StrictMode scope.
    & (Join-Path $PSScriptRoot '../tools/Test-BrowserBoundary.ps1')
    $checks++
    Test-SmokeNativeSummary '100% tests passed, 0 test(s) failed out of 8, spent 1s' 8
    $checks++
    Reject { Test-SmokeNativeSummary '100% tests passed, 0 test(s) failed out of 7, spent 1s' 8 } 'A missing suite passed.'
    Reject { Test-SmokeNativeSummary 'build succeeded' 8 } 'A build-only result passed.'
    Reject { Test-SmokeNativeSummary '87% tests passed, 1 test(s) failed out of 8, spent 1s' 8 } 'A failed suite passed.'
    $nativeLog = "[100%]: osf-example-test/default .... passed 0.031s`n100% tests passed, 0 test(s) failed out of 1, spent 1s"
    Test-SmokeNativeCases $nativeLog @('osf-example-test')
    $checks++
    Reject { Test-SmokeNativeCases $nativeLog @('osf-other-test') } 'Equal counts with wrong suite names passed.'
    Reject { Test-SmokeNativeCases ($nativeLog.Replace('.... passed','.... failed')) @('osf-example-test') } 'Contradictory XMake aggregate passed.'
    Reject { Test-SmokeNativeCases ($nativeLog + "`n[100%]: osf-example-test/default .... passed 0.031s") @('osf-example-test') } 'Duplicate suite evidence passed.'
    Check (@(Read-SmokeNativeCases ($nativeLog.Replace('.... passed','.... failed')))[0].status -eq 'failed') 'Failed native results were lost.'

    $browserStart = [DateTimeOffset]::UtcNow.AddSeconds(-3)
    $browserReceipt = @{
        success=$true; startTime=[DateTimeOffset]::UtcNow.AddSeconds(-2).ToUnixTimeMilliseconds()
        numTotalTests=1;numPassedTests=1;numFailedTests=0;numPendingTests=0;numTodoTests=0
        testResults=@(@{name=(Join-Path $scratch 'tests/example.test.ts');status='passed';assertionResults=@(
            @{fullName='restores state';status='passed';failureMessages=@();duration=3})})
    }
    Test-SmokeBrowserReceipt $browserReceipt $scratch @('tests/example.test.ts') $browserStart
    $checks++
    $browserJson = $browserReceipt | ConvertTo-Json -Depth 10
    function Reject-BrowserMutation([scriptblock]$Mutation, [string]$Message) {
        $copy = $browserJson | ConvertFrom-Json -AsHashtable
        & $Mutation $copy
        Reject { Test-SmokeBrowserReceipt $copy $scratch @('tests/example.test.ts') $browserStart } $Message
    }
    Reject-BrowserMutation { param($r) $r.success='true' } 'Truthy browser success passed.'
    Reject-BrowserMutation { param($r) $r.numPendingTests=1 } 'Skipped browser test passed.'
    Reject-BrowserMutation { param($r) $r.numTodoTests=1 } 'Todo browser test passed.'
    Reject-BrowserMutation { param($r) $r.startTime=1 } 'Stale browser test passed.'
    Reject-BrowserMutation { param($r) $r.testResults[0].assertionResults[0].status='failed' } 'Contradictory browser assertion passed.'
    Reject-BrowserMutation { param($r) $r.testResults[0].assertionResults=@() } 'Missing browser assertion passed.'
    Reject-BrowserMutation { param($r) $r.testResults[0].name=(Join-Path $scratch 'tests/wrong.test.ts') } 'Wrong browser test file passed.'
    Reject-BrowserMutation { param($r) $r.testResults+= $r.testResults[0] } 'Duplicate browser file passed.'
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
    $harnessPlan = @{protocolVersion=2;hosts=@('Released','Deployed');externalActions=@('quicksave','quickload','browser-imports');cases=@('live')}
    Test-SmokeHarnessPlan $harnessPlan @('live') 'Released'
    $checks++
    Reject { Test-SmokeHarnessPlan @{cases=@('live')} @('live') 'Released' } 'Old harness without input controller passed.'
    Reject { Test-SmokeHarnessPlan $harnessPlan @('different') 'Released' } 'Harness inventory drift passed.'
    Reject { Test-SmokeHarnessPlan $harnessPlan @('live') 'Unknown' } 'Unsupported harness host selection passed.'
    $stagingRepo = Join-Path $scratch 'browser-staging-repo'
    $viewPath = 'SFSE/Plugins/OSF/UI/views/osf.animation/browser'
    $productionView = Join-Path $stagingRepo ('build/osfui-animation-browser/' + $viewPath)
    foreach ($path in @($productionView,(Join-Path $stagingRepo 'test/smoke'),(Join-Path $stagingRepo 'dist/OSF/vanilla'),(Join-Path $stagingRepo 'dist/OSF/immersion'))) {
        [IO.Directory]::CreateDirectory($path) | Out-Null
    }
    $productionHtml = '<html><body><script src="/shared/osfui.js"></script></body></html>'
    [IO.File]::WriteAllText((Join-Path $productionView 'index.html'),$productionHtml)
    [IO.File]::WriteAllText((Join-Path $productionView 'manifest.json'),'{}')
    [IO.File]::WriteAllText((Join-Path $stagingRepo 'test/smoke/browser-observer.js'),'// private observer')
    foreach ($folder in @('vanilla','immersion')) { [IO.File]::WriteAllText((Join-Path $stagingRepo "dist/OSF/$folder/test.osf.json"),'{}') }
    $stagingMod = Join-Path $scratch 'runtime-browser'
    Stage-SmokeBrowser $stagingRepo $stagingMod
    Check ((Get-Content (Join-Path $stagingMod "$viewPath/index.html") -Raw).Contains('<script src="./smoke-observer.js"></script>')) 'Live browser observer was not injected.'
    Check (Test-Path (Join-Path $stagingMod "$viewPath/smoke-observer.js")) 'Observer script was not staged.'
    Check ([IO.File]::ReadAllText((Join-Path $productionView 'index.html')) -ceq $productionHtml) 'Observer modified the production document.'
    Check (-not (Test-Path (Join-Path $productionView 'smoke-observer.js'))) 'Private observer leaked into production assets.'
    Check ((Test-Path (Join-Path $stagingMod 'OSF/vanilla/test.osf.json')) -and (Test-Path (Join-Path $stagingMod 'OSF/immersion/test.osf.json'))) 'Shipped browser content was omitted from runtime staging.'
    Reject { Stage-SmokeBrowser $stagingRepo $stagingMod } 'Browser staging reused a previous view.'

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
        'Core/Scripts/OSFAdvanced.pex','Core/OSF/internal.osf.json','Core/SFSE/Plugins/OSF/Settings/schemas/osf.animation.engine.json','fomod/ModuleConfig.xml')
    $browserEntries = @('Browser/SFSE/Plugins/OSF Animation Browser.dll','Browser/SFSE/Plugins/OSF/UI/views/osf.animation/browser/index.html',
        'Browser/SFSE/Plugins/OSF/UI/views/osf.animation/browser/manifest.json','Browser/SFSE/Plugins/OSF/Settings/schemas/osf.animation.browser.json',
        'Immersion/OSF/immersion/emotes.osf.json','fomod/ModuleConfig.xml')
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
    $installArchives = Join-Path $scratch 'install-archives'
    [IO.Directory]::CreateDirectory($installArchives) | Out-Null
    function New-InstallArchive([string]$Path, $Entries) {
        $zip = [IO.Compression.ZipFile]::Open($Path,'Create')
        try { foreach ($key in $Entries.Keys) {
            $writer = [IO.StreamWriter]::new($zip.CreateEntry($key).Open())
            try { $writer.Write($Entries[$key]) } finally { $writer.Dispose() }
        } } finally { $zip.Dispose() }
    }
    $engineInstall = Join-Path $installArchives 'OSF Animation v1.zip'
    $browserInstall = Join-Path $installArchives 'OSF Animation Browser v1.zip'
    $enginePayload = @{'Core/SFSE/Plugins/engine.dll'='engine';'fomod/ModuleConfig.xml'='<config><requiredInstallFiles><folder source="Core" destination=""/></requiredInstallFiles></config>'}
    $browserPayload = @{'Browser/SFSE/Plugins/browser.dll'='browser';'Library/OSF/vanilla/library.json'='library';'Immersion/OSF/immersion/emotes.json'='emotes';
        'fomod/ModuleConfig.xml'='<config><requiredInstallFiles><folder source="Browser" destination=""/></requiredInstallFiles><installSteps><installStep><optionalFileGroups><group type="SelectExactlyOne"><plugins><plugin><files><folder source="Library" destination=""/></files></plugin><plugin/></plugins></group><group type="SelectExactlyOne"><plugins><plugin><files><folder source="Immersion" destination=""/></files></plugin><plugin/></plugins></group></optionalFileGroups></installStep></installSteps></config>'}
    New-InstallArchive $engineInstall $enginePayload
    New-InstallArchive $browserInstall $browserPayload
    $installRoot = Join-Path $scratch 'install-test'
    $installed = @(Test-SmokeInstall $installArchives $installRoot)
    Check ($installed.Count -eq 5 -and $installed[0].payloadFiles -eq 1 -and $installed[4].payloadFiles -eq 4) 'Installer choices did not produce the correct layouts.'
    Check (-not (Test-Path (Join-Path $installRoot 'browser-minimal/OSF/vanilla/library.json'))) 'Skipped library was installed.'
    Reject { Test-SmokeInstall $installArchives $installRoot } 'Existing install evidence was overwritten.'
    Remove-Item -LiteralPath $browserInstall
    $validConfig = $browserPayload['fomod/ModuleConfig.xml']
    $browserPayload['fomod/ModuleConfig.xml']='<config><requiredInstallFiles><folder source="Browser" destination=""/><folder source="Library" destination=""/><folder source="Immersion" destination=""/></requiredInstallFiles></config>'
    New-InstallArchive $browserInstall $browserPayload
    Reject { Test-SmokeInstall $installArchives (Join-Path $scratch 'required-library') } 'Required content was incorrectly treated as skippable.'
    Remove-Item -LiteralPath $browserInstall
    $browserPayload['fomod/ModuleConfig.xml']=$validConfig
    $browserPayload['Browser/SFSE/Plugins/engine.dll']='clobber engine'
    New-InstallArchive $browserInstall $browserPayload
    Reject { Test-SmokeInstall $installArchives (Join-Path $scratch 'collision-install') } 'Cross-product file collision passed.'
    Remove-Item -LiteralPath $browserInstall
    $browserPayload.Remove('Browser/SFSE/Plugins/engine.dll')
    New-InstallArchive $browserInstall $browserPayload
    Remove-Item -LiteralPath $engineInstall
    $enginePayload['Core/../escape.txt']='bad'
    New-InstallArchive $engineInstall $enginePayload
    Reject { Test-SmokeInstall $installArchives (Join-Path $scratch 'unsafe-install') } 'Archive traversal passed install validation.'
    Remove-Item -LiteralPath $engineInstall
    $enginePayload.Remove('Core/../escape.txt')
    $enginePayload['Core/OSF/clip-durations.json']='bad'
    New-InstallArchive $engineInstall $enginePayload
    Reject { Test-SmokeInstall $installArchives (Join-Path $scratch 'clobber-install') } 'Packaged player state passed install validation.'

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
    $report.nativeCases = @(Read-SmokeNativeCases $nativeLog)
    $report.browserCases = @(Read-SmokeBrowserCases $browserReceipt $scratch)
    Save-SmokeReport $report $scratch
    [xml]$junit=Get-Content -LiteralPath (Join-Path $scratch 'junit.xml') -Raw
    Check ($junit.testsuite.tests -eq '7') 'Individual native/browser tests were omitted from JUnit.'
    Check (@($junit.testsuite.testcase | Where-Object name -eq 'browser.tests/example.test.ts: restores state').Count -eq 1) 'Browser assertion identity was lost.'

    # Raw passing observations cannot become report passes when their receipt is rejected.
    # Use both dictionaries and deserialized objects, matching the three production readers.
    $unverifiedReport = [ordered]@{outcome='blocked';scope='offline+runtime';completeAutomatedSuite=$false;
        coverage=$coverage;unverified=@();stages=@(
            @{name='native';status='blocked';seconds=0;message='Unexpected suite inventory';log=$null},
            @{name='browser';status='blocked';seconds=0;message='Stale receipt';log=$null},
            @{name='runtime';status='blocked';seconds=0;message='Different DLL';log=$null});
        nativeCases=@(
            @{name='raw-pass';status='passed';message='Native observation';seconds=1;log=$null},
            @{name='raw-failure';status='failed';message='Assertion failed';seconds=1;log=$null});
        browserCases=@(@{name='raw-pass';status='passed';message='Browser observation';seconds=1;log=$null});
        runtimeCases=@(
            [pscustomobject]@{name='live';status='passed';message='Runtime observation'},
            [pscustomobject]@{name='pending';status='not-run';message='No observation'})}
    $rawEvidence = Join-Path $scratch 'raw-case-evidence.json'
    $unverifiedReport | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $rawEvidence
    $rawEvidenceHash = (Get-FileHash -LiteralPath $rawEvidence).Hash
    Set-SmokeUnverifiedCases $unverifiedReport @{name='native';status='failed';message='Another assertion failed'}
    Check ($unverifiedReport.nativeCases[0].status -eq 'passed' -and $unverifiedReport.nativeCases[1].status -eq 'failed') 'An executed test failure concealed observed partial passes.'
    Set-SmokeUnverifiedCases $unverifiedReport @{name='browser';status='passed';message='Validated'}
    Set-SmokeUnverifiedCases $unverifiedReport @{name='source-unchanged';status='blocked';message='Source changed'}
    Check ($unverifiedReport.browserCases[0].status -eq 'passed' -and $unverifiedReport.runtimeCases[0].status -eq 'passed') 'An unrelated or passing stage changed observed case results.'
    foreach ($blockedStage in $unverifiedReport.stages) {
        Set-SmokeUnverifiedCases $unverifiedReport $blockedStage
        $case = $unverifiedReport[$blockedStage.name + 'Cases'][0]
        Check ($case.status -eq 'blocked' -and $case.message.Contains('source receipt was not validated') -and
            $case.message.Contains($blockedStage.message) -and $case.message.Contains('observation')) "Unvalidated $($blockedStage.name) evidence remained a pass or lost its explanation."
        $firstMessage = $case.message
        Set-SmokeUnverifiedCases $unverifiedReport $blockedStage
        Check ($case.message -ceq $firstMessage) 'Repeated evidence invalidation changed the original observation.'
    }
    Check ($unverifiedReport.nativeCases[1].status -eq 'failed' -and $unverifiedReport.nativeCases[1].message -ceq 'Assertion failed' -and
        $unverifiedReport.runtimeCases[1].status -eq 'not-run') 'Evidence invalidation erased failures or fabricated observations.'
    Save-SmokeReport $unverifiedReport $scratch
    [xml]$junit = Get-Content -LiteralPath (Join-Path $scratch 'junit.xml') -Raw
    Check ($junit.testsuite.failures -eq '1' -and $junit.testsuite.errors -eq '6' -and $junit.testsuite.skipped -eq '1') 'JUnit advertised unvalidated observations as passing tests.'
    $markdown = Get-Content -LiteralPath (Join-Path $scratch 'report.md') -Raw
    Check ($markdown.Contains('| live | blocked |') -and $markdown.Contains('| incomplete |') -and
        -not $markdown.Contains('passed (boundaries below)')) 'Feature coverage advertised unvalidated runtime evidence as passed.'
    $savedReport = Get-Content -LiteralPath (Join-Path $scratch 'result.json') -Raw | ConvertFrom-Json
    Check ($savedReport.nativeCases[0].status -eq 'blocked' -and $savedReport.browserCases[0].status -eq 'blocked' -and
        $savedReport.runtimeCases[0].status -eq 'blocked') 'JSON retained raw passes from rejected receipts.'
    Check ((Get-FileHash -LiteralPath $rawEvidence).Hash -ceq $rawEvidenceHash) 'Reporting rewrote raw source evidence.'

    $releaseEntry = Join-Path $PSScriptRoot '../tools/test-release.ps1'
    $plan = & pwsh -NoProfile -File $releaseEntry -Plan | ConvertFrom-Json
    Check ($LASTEXITCODE -eq 0 -and $plan.runGame -is [bool] -and $plan.runGame) 'Release entrypoint omitted in-game testing by default.'
    Check (-not $plan.interactive) 'Release entrypoint inferred interactive permission.'
    $plan = & pwsh -NoProfile -File $releaseEntry -Interactive -Plan | ConvertFrom-Json
    Check ($LASTEXITCODE -eq 0 -and $plan.runGame -and $plan.interactive) 'Explicit interactive permission was not forwarded.'
    $unapprovedOutput = Join-Path $scratch 'unapproved-run'
    $rejectedRun = & pwsh -NoProfile -File $releaseEntry -OutputDirectory $unapprovedOutput 2>&1 | Out-String
    Check ($LASTEXITCODE -ne 0 -and $rejectedRun.Contains('requires explicit -Interactive') -and
        -not (Test-Path -LiteralPath $unapprovedOutput)) 'A game run without interactive permission reached build or staging.'
    $plan = & pwsh -NoProfile -File $releaseEntry -Offline -Plan -Hosts Deployed | ConvertFrom-Json
    Check ($LASTEXITCODE -eq 0 -and -not $plan.runGame -and $plan.hosts -eq 'Deployed') 'Offline scope/host selection was not forwarded.'
    Write-Host "Smoke bench contracts: $checks checks passed."
} finally {
    $resolved = [IO.Path]::GetFullPath($scratch)
    $prefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
    if (-not $resolved.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($resolved) -notlike 'osf-animation-bench-*') { throw 'Unsafe scratch cleanup path.' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
