[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$evidence = Join-Path $repo 'build\vanilla-pairs-audition'
$receiptPath = Join-Path $evidence 'validation.json'
if (-not (Test-Path -LiteralPath $receiptPath)) { throw 'Run verify.py before deploying.' }
$receipt = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json
if ($receipt.status -ne 'passed') { throw 'Pack validation did not pass.' }
$modsRoot = $env:XSE_SF_MODS_PATH
if ([string]::IsNullOrWhiteSpace($modsRoot)) { throw 'XSE_SF_MODS_PATH is not configured.' }
$modRoot = [IO.Path]::GetFullPath((Join-Path $modsRoot 'OSF Animation'))
if (-not (Test-Path -LiteralPath (Join-Path $modRoot 'SFSE\Plugins\OSF Animation.dll'))) {
    throw "OSF Animation is not installed at $modRoot"
}
$allowlist = @('hunter-walter.osf.json', 'survivor-victim.osf.json', 'the-key.osf.json')
$copies = foreach ($name in $allowlist) {
    $relative = 'OSF/Auditions/VanillaPairs/' + $name
    $source = Join-Path (Join-Path $PSScriptRoot 'Data') $relative
    $target = [IO.Path]::GetFullPath((Join-Path $modRoot $relative))
    if (-not $target.StartsWith($modRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Deployment target escaped the configured mod.'
    }
    $hash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
    if ($hash -ne $receipt.payload_sha256.$relative) { throw "Source changed after validation: $source" }
    [pscustomobject]@{ source = $source; target = $target; sha256 = $hash; name = $name }
}
$lock = $null
try {
    $lockPath = 'C:\Modding\Starfield\OSF Test Harness\state\runner.lock'
    try { $lock = [IO.File]::Open($lockPath, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None) }
    catch [IO.IOException] {
        Write-Output 'Deployment deferred: another harness run owns the shared runner lock. No mod files changed.'
        exit 3
    }
    if (Get-Process -Name Starfield -ErrorAction SilentlyContinue) { throw 'Close Starfield before deploying audition content.' }
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
    $backup = Join-Path $evidence ('backup-' + $stamp)
    foreach ($copy in $copies) {
        if (Test-Path -LiteralPath $copy.target) {
            if ((Get-FileHash -LiteralPath $copy.target -Algorithm SHA256).Hash -eq $copy.sha256) { continue }
            [IO.Directory]::CreateDirectory($backup) | Out-Null
            Copy-Item -LiteralPath $copy.target -Destination (Join-Path $backup $copy.name)
        }
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($copy.target)) | Out-Null
        Copy-Item -LiteralPath $copy.source -Destination $copy.target -Force
    }
    foreach ($copy in $copies) {
        if ((Get-FileHash -LiteralPath $copy.target -Algorithm SHA256).Hash -ne $copy.sha256) {
            throw "Deployed hash mismatch: $($copy.target)"
        }
    }
    [ordered]@{ status = 'deployed'; time = (Get-Date).ToString('o'); files = @($copies); backup = $backup } |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $evidence 'deployment.json') -Encoding utf8
    Write-Output "Deployed and hash-verified all three audition files in $modRoot."
}
finally {
    if ($null -ne $lock) { $lock.Dispose() }
}
