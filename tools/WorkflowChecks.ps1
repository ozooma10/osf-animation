. (Join-Path $PSScriptRoot 'WorkflowCache.ps1')
. (Join-Path $PSScriptRoot 'SmokeBench.ps1')
function Invoke-AnimationContractChecks([string]$Repository, [string]$Harness, [bool]$IncludeHarness, [string]$Log, [switch]$Force) {
    $name = if ($IncludeHarness) { 'bench-contracts-runtime' } else { 'bench-contracts-offline' }
    $cacheLog = Join-Path $Repository "build/workflow-cache/$name.log"
    $receipt = Invoke-WorkflowCached -Stamp (Join-Path $Repository "build/workflow-cache/$name.json") -Force:$Force -Fingerprint {
        $identities = @((Get-SmokeSourceIdentity $Repository).sha256, [string]$IncludeHarness)
        if ($IncludeHarness) { $identities += (Get-SmokeSourceIdentity $Harness).sha256 }
        Get-WorkflowFingerprint -Paths @((Get-Command git).Source) -Values $identities
    } -Outputs { @($cacheLog) } -Action {
        if (Test-Path -LiteralPath $cacheLog) { Remove-Item -LiteralPath $cacheLog -Force }
        $tests = @("$Repository/test/SmokeBenchTest.ps1", "$Repository/test/ReleaseValidationTest.ps1", "$Repository/test/BrowserDependencyCacheTest.ps1")
        if ($IncludeHarness) { $tests += Join-Path $Harness 'tests/AnimationSmoke.Tests.ps1' }
        foreach ($test in $tests) {
            & pwsh -NoProfile -File $test 2>&1 | Tee-Object -FilePath $cacheLog -Append | Out-Host
            if ($LASTEXITCODE) { throw "[failed] $test returned $LASTEXITCODE" }
        }
    }
    Copy-Item -LiteralPath $cacheLog -Destination $Log -Force
    Write-Host "Workflow contracts tested $($receipt.completedUtc)"
}
