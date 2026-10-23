#requires -Version 7.2
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repository = Split-Path $PSScriptRoot -Parent
. (Join-Path $repository 'packaging/ReleaseValidation.ps1')
$scratch = Join-Path ([IO.Path]::GetTempPath()) ('osf-animation-release-' + [guid]::NewGuid().ToString('N'))
$checks = 0
function Check([bool]$Value, [string]$Message) {
    if (-not $Value) { throw $Message }
    $script:checks++
}
function Reject([scriptblock]$Body, [string]$Pattern) {
    try { & $Body } catch {
        Check ($_.Exception.Message -match $Pattern) "Wrong rejection: $_"
        return
    }
    throw "Expected rejection: $Pattern"
}
# Fingerprint collection is covered by SmokeBenchTest; isolate receipt validation here.
function Get-SmokeSourceIdentity([string]$Repository) {
    @{ sha256=(Get-FileHash (Join-Path $Repository 'source.txt')).Hash }
}
try {
    $output = Join-Path $scratch 'build/windows/x64/releasedbg'
    $stage = Join-Path $scratch 'stage'
    foreach ($directory in @($output, "$stage/Core/SFSE/Plugins", "$scratch/dist/Scripts/Source", "$scratch/src/Papyrus", "$scratch/tools")) {
        [IO.Directory]::CreateDirectory($directory) | Out-Null
    }
    Set-Content "$scratch/source.txt" 'initial'
    Set-Content "$scratch/sdk-lock.json" '{}'
    $dll = Join-Path $output 'OSF Animation.dll'
    [IO.File]::WriteAllText($dll, 'MZ production fixture')
    $before = Get-SmokeSourceIdentity $scratch
    Reject { Test-ReleaseBuildReceipt $scratch 'engine' } 'No engine build receipt'
    Write-ReleaseBuildReceipt $scratch 'engine' $before
    $receipt = Test-ReleaseBuildReceipt $scratch 'engine'
    Check ($receipt.files.Count -eq 1) 'The receipt did not bind the build output.'
    Set-Content "$scratch/source.txt" 'edited'
    Reject { Test-ReleaseBuildReceipt $scratch 'engine' } 'current source/configuration'
    Reject { Write-ReleaseBuildReceipt $scratch 'engine' $before } 'Source changed during the build'
    Set-Content "$scratch/source.txt" 'initial'
    [IO.File]::AppendAllText($dll, ' changed')
    Reject { Test-ReleaseBuildReceipt $scratch 'engine' } 'Build output changed'
    foreach ($marker in @('OSFAnimationTestHarness.json', 'OSFAnimationProfilingBuild')) {
        foreach ($encoding in @([Text.Encoding]::UTF8, [Text.Encoding]::Unicode)) {
            [IO.File]::WriteAllBytes($dll, ([byte[]](77,90) + $encoding.GetBytes($marker)))
            Reject { Assert-ProductionDll $dll } 'Instrumented Animation DLL'
        }
    }
    [IO.File]::WriteAllText($dll, 'MZ production fixture')
    Copy-Item -LiteralPath $dll -Destination "$stage/Core/SFSE/Plugins"
    $archive = Join-Path $scratch 'release.zip'
    $resultPath = Join-Path $scratch 'result.txt'
    Write-ReleaseArchive $stage $archive $receipt $resultPath $scratch
    Check ((Get-Content $resultPath) -ceq ($archive + '.manifest.json')) 'ResultPath did not identify the manifest.'
    Test-ReleaseArchive ($archive + '.manifest.json'); $checks++
    [IO.File]::AppendAllText("$stage/Core/SFSE/Plugins/OSF Animation.dll", ' changed')
    Reject { Write-ReleaseArchive $stage $archive $receipt '' $scratch } 'Staged build artifact mismatch'
    Test-ReleaseArchive ($archive + '.manifest.json'); $checks++ # Failed packaging preserves the last verified ZIP.
    Copy-Item -LiteralPath $dll -Destination "$stage/Core/SFSE/Plugins" -Force
    foreach ($forbidden in @('OSFTest.pex','OSFSettingsTestHarness.json','bundle.js.map','values/player.json')) {
        $path = Join-Path $stage $forbidden
        [IO.Directory]::CreateDirectory((Split-Path $path -Parent)) | Out-Null
        Set-Content $path 'forbidden'
        Reject { Write-ReleaseArchive $stage $archive $receipt '' $scratch } 'Forbidden release entry'
        Remove-Item -LiteralPath $path
    }
    [IO.File]::AppendAllText($archive, 'tampered')
    Reject { Test-ReleaseArchive ($archive + '.manifest.json') } 'Archive checksum mismatch'

    # A browser receipt binds both the native addon and the generated view.
    $browserDll = Join-Path $output 'OSF Animation Browser.dll'
    $viewRoot = 'osfui-animation-browser/SFSE/Plugins/OSF/UI/views/osf.animation/browser'
    $view = Join-Path $scratch "build/$viewRoot"
    $browserStage = Join-Path $scratch 'browser-stage'
    [IO.Directory]::CreateDirectory($view) | Out-Null
    [IO.File]::WriteAllText($browserDll, 'MZ browser fixture')
    Set-Content "$view/index.html" '<html>browser</html>'
    $schema = Join-Path $scratch 'build/osfui-animation-browser/SFSE/Plugins/OSF/Settings/schemas/osf.animation.browser.json'
    [IO.Directory]::CreateDirectory((Split-Path $schema)) | Out-Null
    Set-Content $schema '{"groups":{}}'
    Write-ReleaseBuildReceipt $scratch 'browser' $before
    $browserReceipt = Test-ReleaseBuildReceipt $scratch 'browser'
    Check ($browserReceipt.files.Count -eq 3) 'Browser receipt must bind the DLL, view, and owned settings schema.'
    Add-Content $schema 'changed'
    Reject { Test-ReleaseBuildReceipt $scratch 'browser' } 'Build output changed'
    Set-Content $schema '{"groups":{}}'
    [IO.File]::AppendAllText($browserDll, ' changed')
    Reject { Test-ReleaseBuildReceipt $scratch 'browser' } 'Build output changed'
    [IO.File]::WriteAllText($browserDll, 'MZ browser fixture')
    Add-Content "$view/index.html" 'changed'
    Reject { Test-ReleaseBuildReceipt $scratch 'browser' } 'Build output changed'
    Set-Content "$view/index.html" '<html>browser</html>'
    $browserPlugins = Join-Path $browserStage 'Browser/SFSE/Plugins'
    $browserViews = Join-Path $browserPlugins 'OSF/UI/views/osf.animation'
    [IO.Directory]::CreateDirectory($browserViews) | Out-Null
    Copy-Item $view $browserViews -Recurse
    $browserSchemas = Join-Path $browserPlugins 'OSF/Settings/schemas'
    [IO.Directory]::CreateDirectory($browserSchemas) | Out-Null
    Copy-Item $schema $browserSchemas
    $browserArchive = Join-Path $scratch 'browser.zip'
    Reject { Write-ReleaseArchive $browserStage $browserArchive $browserReceipt '' $scratch } 'Staged build artifact mismatch'
    Copy-Item $browserDll $browserPlugins
    Write-ReleaseArchive $browserStage $browserArchive $browserReceipt '' $scratch
    Test-ReleaseArchive ($browserArchive + '.manifest.json'); $checks++
    [IO.File]::AppendAllText("$browserPlugins/OSF Animation Browser.dll", ' changed')
    Reject { Write-ReleaseArchive $browserStage $browserArchive $browserReceipt '' $scratch } 'Staged build artifact mismatch'

    Copy-Item "$repository/tools/verify-papyrus.py" "$scratch/tools"
    Copy-Item "$repository/src/Papyrus/OSFScript.cpp" "$scratch/src/Papyrus"
    foreach ($name in @('OSF','OSFTypes','OSFAdvanced')) {
        Copy-Item "$repository/dist/Scripts/Source/$name.psc" "$scratch/dist/Scripts/Source"
        Copy-Item "$repository/dist/Scripts/$name.pex" "$scratch/dist/Scripts"
        (Get-Item "$scratch/dist/Scripts/Source/$name.psc").LastWriteTimeUtc = [DateTime]::UtcNow.AddDays(-1)
        (Get-Item "$scratch/dist/Scripts/$name.pex").LastWriteTimeUtc = [DateTime]::UtcNow
    }
    Test-ReleasePapyrus $scratch; $checks++
    (Get-Item "$scratch/dist/Scripts/Source/OSF.psc").LastWriteTimeUtc = [DateTime]::UtcNow.AddMinutes(1)
    Reject { Test-ReleasePapyrus $scratch } 'newer than its PEX'
    (Get-Item "$scratch/dist/Scripts/Source/OSF.psc").LastWriteTimeUtc = [DateTime]::UtcNow.AddDays(-1)
    Add-Content "$scratch/dist/Scripts/Source/OSF.psc" 'Int Function ExpectedMissingNative() Global Native'
    (Get-Item "$scratch/dist/Scripts/Source/OSF.psc").LastWriteTimeUtc = [DateTime]::UtcNow.AddDays(-1)
    Reject { Test-ReleasePapyrus $scratch } 'Compiled Papyrus signature validation failed'
    Write-Host "Release validation: $checks checks passed."
} finally {
    $resolved = [IO.Path]::GetFullPath($scratch)
    $prefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
    if (-not $resolved.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($resolved) -notlike 'osf-animation-release-*') { throw 'Unsafe scratch cleanup path.' }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
