#requires -Version 7.2
<#
.SYNOPSIS
Build and exercise OSF Animation; retain JSON, Markdown, JUnit, logs and archives.
.DESCRIPTION
Offline by default. -RunGame adds the sibling harness's owned, disposable game
scenario. Neither mode deploys to the ordinary development mod. Exit codes:
0 requested checks passed, 1 assertion/build failure, 2 infrastructure blocked.
See docs/SMOKE_TESTING.md for coverage and the remaining acceptance boundaries.
#>
[CmdletBinding()]
param(
    [switch]$RunGame,
    [switch]$Plan,
    [string]$Harness = (Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) 'OSF Test Harness'),
    [string]$OutputDirectory = '',
    [ValidateRange(1,64)][int]$Jobs = 4
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'SmokeBench.ps1')
. (Join-Path $repo 'packaging/ReleaseValidation.ps1')
$coverage = @(Get-Content -LiteralPath (Join-Path $repo 'test/smoke/coverage.json') -Raw | ConvertFrom-Json)
$runtimeCases = @(Get-Content -LiteralPath (Join-Path $repo 'test/smoke/runtime-cases.json') -Raw | ConvertFrom-Json)
$suite = Join-Path $Harness 'Test-AnimationRelease.ps1'
$names = @('coverage','bench-contracts','sdk','production-build','native','browser','distribution',
    'runtime-build','runtime','restore-production-config','source-unchanged')
$unverified = @($coverage | ForEach-Object { "$($_.feature): $($_.gap)" })
if (-not $RunGame) { $unverified = @('In-game assertions skipped. Use -RunGame with the local OSF Test Harness.') + $unverified }
if ($Plan) {
    [ordered]@{ stages=$names; runGame=[bool]$RunGame; runtimeCases=$runtimeCases; coverage=$coverage;
        unverified=$unverified; runtimeRunner=$suite } | ConvertTo-Json -Depth 12
    exit 0
}
$directory = if ($OutputDirectory) { [IO.Path]::GetFullPath($OutputDirectory) } else {
    Join-Path $repo ('build/smoke/' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff') + '-' + [guid]::NewGuid().ToString('N').Substring(0,8))
}
if (Test-Path -LiteralPath $directory) { throw 'Use a new output directory; previous evidence is never overwritten.' }
# Prevent report output becoming a changing part of its own source fingerprint.
$relativeOutput = [IO.Path]::GetRelativePath($repo, $directory)
if (-not $relativeOutput.StartsWith('..') -and -not [IO.Path]::IsPathRooted($relativeOutput) -and
    $relativeOutput -notmatch '^build[\\/]smoke([\\/]|$)') {
    throw 'Inside the repository, output must live under build/smoke. External output directories are also supported.'
}
[IO.Directory]::CreateDirectory($directory) | Out-Null
$report = [ordered]@{
    startedUtc=[DateTime]::UtcNow.ToString('o'); finishedUtc=$null; outcome='running'
    scope=$(if ($RunGame) { 'offline+runtime' } else { 'offline' }); completeAutomatedSuite=$false; automatedPassed=$false
    source=$null; runtime=$null; productionDllSha256=$null; runtimeDllSha256=$null
    coverage=$coverage; unverified=$unverified
    stages=@($names | ForEach-Object { [ordered]@{ name=$_; status='not-run'; seconds=0; message=''; log=$null } })
}
$exitCode = 2
$lock = $null
$identity = $null
$oldMods = $env:XSE_SF_MODS_PATH
$oldGame = $env:XSE_SF_GAME_PATH
$productionMods = Join-Path $directory 'production'
$productionMod = Join-Path $productionMods 'OSF Animation'
$runtimeMods = Join-Path $directory 'runtime-staging'
$runtimeMod = Join-Path $runtimeMods 'OSF Animation'

function Get-Stage([string]$Name) { @($report.stages | Where-Object name -eq $Name)[0] }
function Test-StagesPassed([string[]]$Dependencies) {
    @($Dependencies | Where-Object { (Get-Stage $_).status -ne 'passed' }).Count -eq 0
}
function Invoke-SmokeStage([string]$Name, [scriptblock]$Body, [string[]]$Dependencies = @()) {
    $script:stage = Get-Stage $Name
    if (-not (Test-StagesPassed $Dependencies)) {
        $script:stage.message = 'Prerequisite did not pass: ' + ($Dependencies -join ', ')
        Save-SmokeReport $report $directory
        return
    }
    $script:stage.status = 'running'
    $watch = [Diagnostics.Stopwatch]::StartNew()
    Write-Host "Smoke bench: $Name"
    Save-SmokeReport $report $directory
    try { & $Body; $script:stage.status = 'passed' }
    catch {
        $script:stage.status = if ($_.Exception.Message.StartsWith('[failed]')) { 'failed' } else { 'blocked' }
        $script:stage.message = $_.Exception.Message
        Write-Warning "$Name`: $($_.Exception.Message)"
    } finally {
        $script:stage.seconds = [Math]::Round($watch.Elapsed.TotalSeconds, 3)
        Save-SmokeReport $report $directory
    }
}
function Invoke-SmokeCommand([string]$Program, [string[]]$Arguments, [switch]$Runtime) {
    $script:stage.log = Join-Path $directory ($script:stage.name + '.log')
    if (-not (Get-Command $Program -ErrorAction SilentlyContinue)) { throw "[blocked] Missing command: $Program" }
    & $Program @Arguments 2>&1 | Tee-Object -FilePath $script:stage.log -Append | Out-Host
    if ($LASTEXITCODE) {
        $kind = if ($Runtime -and $LASTEXITCODE -eq 2) { 'blocked' } else { 'failed' }
        throw "[$kind] $Program returned $LASTEXITCODE; see $($script:stage.log)"
    }
}
Push-Location $repo
try {
    [IO.Directory]::CreateDirectory((Join-Path $repo 'build')) | Out-Null
    $lock = [IO.File]::Open((Join-Path $repo 'build/smoke.lock'), 'OpenOrCreate', 'ReadWrite', 'None')
    $identity = Get-SmokeSourceIdentity $repo
    $identity | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $directory 'source.json')
    $report.source = [ordered]@{ revision=$identity.revision; sha256=$identity.sha256; dirty=$identity.dirty }
    Invoke-SmokeStage 'coverage' {
        $targets = @(Get-SmokeNativeTargets $repo)
        Test-SmokeCoverage $coverage $targets $runtimeCases
        $script:stage.message = "$($targets.Count) native suites; $($runtimeCases.Count) game assertions; $($coverage.Count) feature areas"
    }
    Invoke-SmokeStage 'bench-contracts' {
        Invoke-SmokeCommand 'pwsh' @('-NoProfile','-File',(Join-Path $repo 'test/SmokeBenchTest.ps1'))
        Invoke-SmokeCommand 'pwsh' @('-NoProfile','-File',(Join-Path $repo 'test/ReleaseValidationTest.ps1'))
        if ($RunGame) {
            $harnessTests = Join-Path $Harness 'tests/AnimationSmoke.Tests.ps1'
            if (-not (Test-Path -LiteralPath $harnessTests)) { throw '[blocked] Animation harness contract tests are missing; pass -Harness.' }
            Invoke-SmokeCommand 'pwsh' @('-NoProfile','-File',$harnessTests)
        }
    }
    Invoke-SmokeStage 'sdk' {
        Invoke-SmokeCommand 'pwsh' @('-NoProfile','-File',(Join-Path $PSScriptRoot 'Test-SdkLock.ps1'))
        $script:stage.message = 'Vendored OSF UI / OSF Settings SDK files match sdk-lock.json'
    }
    Invoke-SmokeStage 'production-build' {
        $env:XSE_SF_MODS_PATH = $productionMods
        $env:XSE_SF_GAME_PATH = $null
        Invoke-SmokeCommand 'xmake' @('f','-y','-m','releasedbg','-o','build','--animation_smoke=n')
        Invoke-SmokeCommand 'xmake' @('-y',"-j$Jobs")
        Invoke-SmokeCommand 'xmake' @('install','-y','-o',$productionMod,'OSF Animation')
        $report.productionDllSha256 = (Get-FileHash -LiteralPath (Join-Path $productionMod 'SFSE/Plugins/OSF Animation.dll')).Hash
        Write-ReleaseBuildReceipt $repo 'engine' $identity
        $script:stage.message = 'releasedbg production DLL built and installed to private staging'
    }
    Invoke-SmokeStage 'native' {
        Invoke-SmokeCommand 'xmake' @('test',"-j$Jobs",'-v')
        Test-SmokeNativeSummary (Get-Content -LiteralPath $script:stage.log -Raw) @(Get-SmokeNativeTargets $repo).Count
    } @('coverage','production-build')
    Invoke-SmokeStage 'browser' {
        Invoke-SmokeCommand 'pwsh' @('-NoProfile','-File',(Join-Path $PSScriptRoot 'BuildBrowserAddon.ps1'),'-NoDeploy')
        $script:stage.message = 'npm ci, browser tests, typecheck, and production build'
    }
    Invoke-SmokeStage 'distribution' {
        $archives = Join-Path $directory 'archives'
        Invoke-SmokeCommand 'pwsh' @('-NoProfile','-File',(Join-Path $repo 'packaging/build-archive.ps1'),'-SkipBuild','-OutDir',$archives)
        Invoke-SmokeCommand 'pwsh' @('-NoProfile','-File',(Join-Path $repo 'packaging/build-browser-addon.ps1'),'-SkipBuild','-OutDir',$archives)
        Test-SmokeArchives $archives $report.productionDllSha256
        Get-ChildItem -LiteralPath $archives -Filter '*.zip' -File | Get-FileHash |
            Select-Object Path,Hash | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $directory 'archive-hashes.json')
        $script:stage.message = 'Engine and browser ZIP contents verified; production DLL hash matches'
    } @('production-build','browser')
    if ($RunGame) {
        Invoke-SmokeStage 'runtime-build' {
            if (-not (Test-Path -LiteralPath $suite)) { throw '[blocked] Animation runtime harness is missing; pass -Harness.' }
            $env:XSE_SF_MODS_PATH = $runtimeMods
            # Separate output keeps instrumented builds out of build/windows, which the packager uses.
            Invoke-SmokeCommand 'xmake' @('f','-y','-m','releasedbg','-o','build/smoke-native','--animation_smoke=y')
            Invoke-SmokeCommand 'xmake' @('-y',"-j$Jobs")
            Invoke-SmokeCommand 'xmake' @('install','-y','-o',$runtimeMod,'OSF Animation')
            Copy-Item -LiteralPath (Join-Path $repo 'test/smoke/data/OSF') -Destination $runtimeMod -Recurse -Force
            $report.runtimeDllSha256 = (Get-FileHash -LiteralPath (Join-Path $runtimeMod 'SFSE/Plugins/OSF Animation.dll')).Hash
        } @('coverage','bench-contracts','production-build','native','browser','distribution')
        Invoke-SmokeStage 'runtime' {
            $report.runtime = Join-Path $directory 'runtime-result.json'
            $started = [DateTimeOffset]::UtcNow
            Invoke-SmokeCommand 'pwsh' @('-NoProfile','-File',$suite,'-AnimationProject',$repo,
                '-StagedModRoot',$runtimeMod,'-SourceHash',$identity.sha256,'-ResultPath',$report.runtime) -Runtime
            $receipt = Get-Content -LiteralPath $report.runtime -Raw | ConvertFrom-Json
            Test-SmokeRuntimeReceipt $receipt $runtimeCases $identity.sha256 $report.runtimeDllSha256 $started ([DateTimeOffset]::UtcNow)
            $script:stage.message = "$($runtimeCases.Count) fresh in-game assertions passed"
        } @('runtime-build')
    } else {
        (Get-Stage 'runtime-build').message = 'Use -RunGame to build the isolated test DLL.'
        (Get-Stage 'runtime').message = 'Use -RunGame for owned disposable game scenarios.'
    }
} catch {
    $report.unverified += $_.Exception.Message
    (Get-Stage 'source-unchanged').status = 'blocked'
    (Get-Stage 'source-unchanged').message = $_.Exception.Message
    Write-Warning $_.Exception.Message
} finally {
    if ($lock) {
        Invoke-SmokeStage 'restore-production-config' {
            $env:XSE_SF_MODS_PATH = $oldMods
            $env:XSE_SF_GAME_PATH = $oldGame
            Invoke-SmokeCommand 'xmake' @('f','-y','-m','releasedbg','-o','build','--animation_smoke=n')
        }
        if ($identity) {
            Invoke-SmokeStage 'source-unchanged' {
                if ((Get-SmokeSourceIdentity $repo).sha256 -cne $identity.sha256) {
                    throw '[blocked] Source changed during the bench; rerun against fixed content.'
                }
            }
        }
    }
    $env:XSE_SF_MODS_PATH = $oldMods
    $env:XSE_SF_GAME_PATH = $oldGame
    $required = if ($RunGame) { $names } else { @($names | Where-Object { $_ -notin @('runtime-build','runtime') }) }
    $exitCode = if (@($report.stages | Where-Object status -eq 'failed').Count) { 1 }
        elseif (@($report.stages | Where-Object { $_.name -in $required -and $_.status -ne 'passed' }).Count) { 2 } else { 0 }
    $report.automatedPassed = $exitCode -eq 0
    $report.completeAutomatedSuite = $report.automatedPassed -and [bool]$RunGame
    $report.outcome = if ($exitCode -eq 2) { 'blocked' } elseif ($exitCode -eq 1) { 'failed' }
        elseif ($RunGame) { 'automated-passed-acceptance-gaps-open' } else { 'offline-passed' }
    $report.finishedUtc = [DateTime]::UtcNow.ToString('o')
    Save-SmokeReport $report $directory
    if ($lock) { $lock.Dispose() }
    Pop-Location
    Write-Host "Smoke report: $(Join-Path $directory 'report.md')"
}
exit $exitCode
