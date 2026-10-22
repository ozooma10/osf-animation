#requires -Version 7.2
. (Join-Path $PSScriptRoot '../tools/SmokeBench.ps1')

function Get-ReleaseFileHashes([string]$Root) {
    @(Get-ChildItem -LiteralPath $Root -File -Recurse | Sort-Object FullName | ForEach-Object {
        @{ path=[IO.Path]::GetRelativePath($Root, $_.FullName).Replace('\','/'); sha256=(Get-FileHash -LiteralPath $_.FullName).Hash }
    })
}

function Assert-ProductionDll([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 2 -or $bytes[0] -ne 77 -or $bytes[1] -ne 90) { throw 'Invalid production DLL.' }
    if (Test-InstrumentedDll $Path) { throw 'Instrumented Animation DLL cannot be packaged.' }
}

function Get-ReleaseArtifacts([string]$Repository, [string]$Kind) {
    if ($Kind -eq 'engine') {
        $root = 'build/windows/x64/releasedbg'
        Assert-ProductionDll (Join-Path $Repository "$root/OSF Animation.dll")
        $files = @(Get-ReleaseFileHashes (Join-Path $Repository $root) | Where-Object path -in @('OSF Animation.dll','OSF Animation.pdb'))
    } elseif ($Kind -eq 'browser') {
        $root = 'build'
        $dll = 'windows/x64/releasedbg/OSF Animation Browser.dll'
        Assert-ProductionDll (Join-Path $Repository "$root/$dll")
        $view = 'osfui-animation-browser'
        $files = @(@{path=$dll; sha256=(Get-FileHash -LiteralPath (Join-Path $Repository "$root/$dll")).Hash}) +
            @(Get-ReleaseFileHashes (Join-Path $Repository "$root/$view") | ForEach-Object { @{path="$view/$($_.path)"; sha256=$_.sha256} })
    } else { throw "Unknown release kind: $Kind" }
    return @{root=$root; files=$files}
}

function Write-ReleaseBuildReceipt([string]$Repository, [string]$Kind, $Before) {
    $after = Get-SmokeSourceIdentity $Repository
    if ($Before.sha256 -cne $after.sha256) { throw 'Source changed during the build; no receipt written.' }
    $artifacts = Get-ReleaseArtifacts $Repository $Kind
    $root = $artifacts.root
    $files = $artifacts.files
    if (-not $files.Count) { throw 'Build produced no files.' }
    $receipt = @{ schemaVersion=1; kind=$Kind; source=$after; sdk=(Get-Content (Join-Path $Repository 'sdk-lock.json') -Raw | ConvertFrom-Json)
        configuration=$(if ($Kind -eq 'engine') { 'windows/x64/releasedbg; animation_smoke=n' } else { 'windows/x64/releasedbg; animation_smoke=n; vite/production' })
        root=$root; files=$files; createdAt=[DateTimeOffset]::UtcNow.ToString('o') }
    $receipt | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath (Join-Path $Repository "build/$Kind-receipt.json") -Encoding utf8NoBOM
}

function Test-ReleaseBuildReceipt([string]$Repository, [string]$Kind) {
    $path = Join-Path $Repository "build/$Kind-receipt.json"
    if (-not (Test-Path -LiteralPath $path)) { throw "No $Kind build receipt. Build through the packager or smoke bench before using -SkipBuild." }
    $receipt = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    $expectedRoot = if ($Kind -eq 'engine') { 'build/windows/x64/releasedbg' } else { 'build' }
    $expectedConfiguration = if ($Kind -eq 'engine') { 'windows/x64/releasedbg; animation_smoke=n' } else { 'windows/x64/releasedbg; animation_smoke=n; vite/production' }
    if ($receipt.schemaVersion -ne 1 -or $receipt.kind -cne $Kind -or $receipt.root -cne $expectedRoot -or $receipt.configuration -cne $expectedConfiguration -or
        $receipt.source.sha256 -cne (Get-SmokeSourceIdentity $Repository).sha256) { throw 'Build receipt does not match current source/configuration.' }
    $files = (Get-ReleaseArtifacts $Repository $Kind).files
    $actual = @($files | ForEach-Object { "$($_.path)=$($_.sha256)" })
    $expected = @($receipt.files | ForEach-Object { "$($_.path)=$($_.sha256)" })
    if (-not $expected.Count -or @(Compare-Object $expected $actual -CaseSensitive).Count) { throw 'Build output changed after its receipt was recorded.' }
    return $receipt
}

function Test-ReleasePapyrus([string]$Repository) {
    foreach ($name in @('OSF','OSFTypes','OSFAdvanced')) {
        $source = Get-Item -LiteralPath (Join-Path $Repository "dist/Scripts/Source/$name.psc")
        $compiled = Get-Item -LiteralPath (Join-Path $Repository "dist/Scripts/$name.pex")
        if ($source.LastWriteTimeUtc -gt $compiled.LastWriteTimeUtc) { throw "$name.psc is newer than its PEX. Recompile Papyrus before packaging." }
    }
    & python (Join-Path $Repository 'tools/verify-papyrus.py') $Repository
    if ($LASTEXITCODE) { throw 'Compiled Papyrus signature validation failed.' }
}

function Write-ReleaseArchive([string]$Stage, [string]$Archive, $BuildReceipt, [string]$ResultPath, [string]$Repository) {
    $files = @(Get-ReleaseFileHashes $Stage)
    foreach ($file in $files) {
        if (Test-ReleaseForbiddenEntry $file.path) { throw "Forbidden release entry: $($file.path)" }
    }
    foreach ($file in $BuildReceipt.files) {
        $relative = if ($BuildReceipt.kind -eq 'engine') { "Core/SFSE/Plugins/$($file.path)" }
            elseif ($file.path -ceq 'windows/x64/releasedbg/OSF Animation Browser.dll') { 'Browser/SFSE/Plugins/OSF Animation Browser.dll' }
            else { 'Browser/' + $file.path.Substring('osfui-animation-browser/'.Length) }
        if ($BuildReceipt.kind -eq 'engine' -and $file.path -eq 'OSF Animation.pdb' -and -not (Test-Path (Join-Path $Stage $relative))) { continue }
        $staged = @($files | Where-Object path -CEQ $relative)
        if ($staged.Count -ne 1 -or $staged[0].sha256 -cne $file.sha256) { throw "Staged build artifact mismatch: $relative" }
    }
    $partial = $Archive + '.partial'
    if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial }
    $zip = [IO.Compression.ZipFile]::Open($partial, 'Create')
    try {
        foreach ($file in $files) {
            $entry = $zip.CreateEntry($file.path, 'Optimal')
            $entry.LastWriteTime = [DateTimeOffset]::new(2000,1,1,0,0,0,[TimeSpan]::Zero)
            $entryInput = [IO.File]::OpenRead((Join-Path $Stage $file.path)); $entryOutput = $entry.Open()
            try { $entryInput.CopyTo($entryOutput) } finally { $entryOutput.Dispose(); $entryInput.Dispose() }
        }
    } finally { $zip.Dispose() }
    Test-ReleaseZipPayload $partial $files
    $current = Test-ReleaseBuildReceipt $Repository $BuildReceipt.kind
    if ($current.source.sha256 -cne $BuildReceipt.source.sha256) { throw 'Source changed during packaging.' }
    Move-Item -LiteralPath $partial -Destination $Archive -Force
    $hash = (Get-FileHash -LiteralPath $Archive).Hash
    $manifest = @{ schemaVersion=1; archive=[IO.Path]::GetFullPath($Archive); sha256=$hash; build=$BuildReceipt; files=$files }
    $manifestPath = $Archive + '.manifest.json'
    $manifest | ConvertTo-Json -Depth 25 | Set-Content -LiteralPath $manifestPath -Encoding utf8NoBOM
    "$hash  $([IO.Path]::GetFileName($Archive))" | Set-Content -LiteralPath ($Archive + '.sha256') -Encoding utf8NoBOM
    Test-ReleaseArchive $manifestPath
    if ($ResultPath) { [IO.File]::WriteAllText([IO.Path]::GetFullPath($ResultPath), $manifestPath) }
    Write-Host "Verified archive: $Archive"
    Write-Host "SHA256 $hash"
}

function Test-ReleaseArchive([string]$ManifestPath) {
    $manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
    if ((Get-FileHash -LiteralPath $manifest.archive).Hash -cne $manifest.sha256) { throw 'Archive checksum mismatch.' }
    Test-ReleaseZipPayload $manifest.archive $manifest.files
}

function Test-ReleaseZipPayload([string]$Archive, $Files) {
    $zip = [IO.Compression.ZipFile]::OpenRead($Archive)
    try {
        if ($zip.Entries.Count -ne @($Files).Count) { throw 'Archive inventory mismatch.' }
        foreach ($file in $Files) {
            $entry = $zip.GetEntry($file.path)
            if (-not $entry) { throw "Missing archive entry: $($file.path)" }
            $stream = $entry.Open()
            try { $hash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($stream)) } finally { $stream.Dispose() }
            if ($hash -cne $file.sha256) { throw "Archive entry mismatch: $($file.path)" }
        }
    } finally { $zip.Dispose() }
}
