#requires -Version 7.2
<#
.SYNOPSIS
Verify vendored OSF UI / OSF Settings SDK files against sdk-lock.json.
.DESCRIPTION
Always checks each vendored file's hash (CRLF normalized to LF) against the lock.
When a sibling source checkout is present, also checks that the locked tag still
resolves to the locked commit and that the file at that commit matches the lock.
A sibling whose current SDK differs from the lock is reported, not failed.
Exit codes: 0 passed, 1 drift or provenance mismatch, 2 sources required but missing.
#>
[CmdletBinding()]
param(
    # Fail (exit 2) instead of skipping when a sibling source checkout is missing.
    [switch]$RequireSources
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path $PSScriptRoot -Parent
$lock = Get-Content -LiteralPath (Join-Path $repo 'sdk-lock.json') -Raw | ConvertFrom-Json

function Get-NormalizedHash([byte[]]$Bytes) {
    $lf = [Collections.Generic.List[byte]]::new($Bytes.Length)
    foreach ($b in $Bytes) { if ($b -ne 13) { $lf.Add($b) } }
    [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($lf.ToArray())).ToLowerInvariant()
}

# Raw git stdout bytes, or $null when git fails (missing tag, commit, or path).
function Invoke-GitBytes([string]$Repository, [string[]]$Arguments) {
    $start = [Diagnostics.ProcessStartInfo]::new('git')
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    foreach ($argument in (@('-C', $Repository) + $Arguments)) { $start.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::Start($start)
    try {
        $errors = $process.StandardError.ReadToEndAsync()
        $buffer = [IO.MemoryStream]::new()
        $process.StandardOutput.BaseStream.CopyTo($buffer)
        $process.WaitForExit()
        $null = $errors.GetAwaiter().GetResult()
        if ($process.ExitCode) { return $null }
        return $buffer.ToArray()
    } finally { $process.Dispose() }
}

$failures = [Collections.Generic.List[string]]::new()
$notes = [Collections.Generic.List[string]]::new()
$missingSources = [Collections.Generic.List[string]]::new()

foreach ($file in $lock.files) {
    $path = Join-Path $repo $file.path
    if (-not (Test-Path -LiteralPath $path)) { $failures.Add("$($file.path): missing"); continue }
    $actual = Get-NormalizedHash ([IO.File]::ReadAllBytes($path))
    if ($actual -ne $file.sha256) {
        $failures.Add("$($file.path): hash $actual does not match the lock; re-vendor from $($file.source) $($lock.sources.($file.source).tag) or update sdk-lock.json")
    }
}

foreach ($property in $lock.sources.PSObject.Properties) {
    $name = $property.Name
    $source = $property.Value
    $sibling = [IO.Path]::GetFullPath((Join-Path $repo $source.sibling))
    if (-not (Test-Path -LiteralPath (Join-Path $sibling '.git'))) {
        $missingSources.Add("${name}: no checkout at $sibling; provenance not checked")
        continue
    }
    $tagged = Invoke-GitBytes $sibling @('rev-parse', '--verify', '--quiet', "$($source.tag)^{commit}")
    if ($null -eq $tagged) {
        $notes.Add("${name}: tag $($source.tag) not present in $sibling; checked the locked commit only")
    } elseif ([Text.Encoding]::ASCII.GetString($tagged).Trim() -ne $source.commit) {
        $failures.Add("${name}: tag $($source.tag) resolves to $([Text.Encoding]::ASCII.GetString($tagged).Trim()), lock says $($source.commit)")
    }
    foreach ($file in @($lock.files | Where-Object source -eq $name)) {
        $blob = Invoke-GitBytes $sibling @('cat-file', 'blob', "$($source.commit):$($file.sourcePath)")
        if ($null -eq $blob) {
            $failures.Add("$($file.path): $($file.sourcePath) not found at $name $($source.commit)")
        } elseif ((Get-NormalizedHash $blob) -ne $file.sha256) {
            $failures.Add("$($file.path): lock hash does not match $($file.sourcePath) at $name $($source.commit)")
        }
        $current = Join-Path $sibling $file.sourcePath
        if ((Test-Path -LiteralPath $current) -and (Get-NormalizedHash ([IO.File]::ReadAllBytes($current))) -ne $file.sha256) {
            $notes.Add("$($file.path): sibling $name working copy of $($file.sourcePath) differs from the locked release (newer SDK?)")
        }
    }
}

foreach ($note in $notes) { Write-Host "note: $note" }
foreach ($missing in $missingSources) { Write-Host "skipped: $missing" }
foreach ($failure in $failures) { Write-Host "FAILED: $failure" }
if ($failures.Count) { exit 1 }
if ($RequireSources -and $missingSources.Count) { exit 2 }
Write-Host "SDK lock: $(@($lock.files).Count) vendored file(s) match $(@($lock.sources.PSObject.Properties | ForEach-Object { "$($_.Name) $($_.Value.tag)" }) -join ', ')"
exit 0
