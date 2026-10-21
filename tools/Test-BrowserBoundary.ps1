#requires -Version 7.2
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path $PSScriptRoot -Parent
$public = @('OSFSceneAPI.h','OSFEngineAPI.h','OSFActorState.h','OSFCatalog.h','OSFClipScene.h','OSFUI.h','OSFSettings.h','OSFSettings_Diagnostics.h')
$source = Join-Path $repo 'extensions/browser/src'
foreach ($file in Get-ChildItem -LiteralPath $source -Recurse -File | Where-Object Extension -in '.cpp','.h') {
    foreach ($match in [regex]::Matches([IO.File]::ReadAllText($file.FullName), '(?m)^\s*#\s*include\s*"([^"]+)"')) {
        $include = $match.Groups[1].Value
        if ($include -match '(^|[\\/])\.\.([\\/]|$)') { throw "Private relative include: $include" }
        if (Test-Path -LiteralPath (Join-Path $source $include)) { continue }
        if (Test-Path -LiteralPath (Join-Path $repo "shared/$include")) { continue }
        if ($include -in $public -or $include -like 'RE/*' -or $include -like 'REX/*' -or $include -like 'SFSE/*' -or $include -like 'REL/*') { continue }
        throw "Browser includes a private engine file: $($file.Name): $include"
    }
}
$build = [IO.File]::ReadAllText((Join-Path $repo 'xmake.lua'))
if ($build -notmatch 'includes\("extensions/browser"\)' -or $build -match 'target\("OSF Animation Browser"\)') {
    throw 'The root build must include the Browser extension target from its own directory.'
}
$extensionBuild = [IO.File]::ReadAllText((Join-Path $repo 'extensions/browser/xmake.lua'))
$target = [regex]::Match($extensionBuild, '(?s)target\("OSF Animation Browser"\)(.*?)(?=\ntarget\(|\z)').Groups[1].Value
if ($target -notmatch 'add_files\("src/\*\*\.cpp"\)' -or
    $target -match 'add_(files|deps|links)\([^\r\n]*("\.\./|"OSF Animation")' -or
    $target -match 'add_includedirs\([^\r\n]*"\.\./\.\./src"') { throw 'Browser target includes or links engine implementation.' }
$core = [regex]::Match($build, '(?s)target\("OSF Animation"\)(.*?)(?=\ntarget\(|\z)').Groups[1].Value
if ($core -match 'add_files\([^\r\n]*extensions/') { throw 'Browser implementation leaked into engine target.' }
$private = @(Get-ChildItem -LiteralPath (Join-Path $repo 'src') -Recurse -File |
    Where-Object { $_.Extension -in '.cpp','.h' -and $_.FullName -notlike '*\Testing\*' -and $_.Name -notin @('OSFUI.h','BrowserForwarder.h') })
foreach ($file in $private) {
    if ([IO.File]::ReadAllText($file.FullName) -match '#include "(API/(BrowserSession|UIBridge)|Serialization/WheelPins|(?:API/)?OSFUI|Util/KeywordLabel)') {
        throw "Engine depends on browser integration: $($file.FullName)"
    }
    if ([IO.File]::ReadAllText($file.FullName) -match 'clip-durations\.json|kUiDragScale|SetUiCursorVisible|ClipDurationCatalog|player\.emote\.|derivedDebugAnimation|MakeAnchorInFrontOfView') {
        throw "Browser cache/input policy leaked into engine: $($file.FullName)"
    }
}
$coreSchema = Get-Content (Join-Path $repo 'dist/SFSE/Plugins/OSF/Settings/schemas/osf.animation.engine.json') -Raw | ConvertFrom-Json
$browserSchema = Get-Content (Join-Path $repo 'extensions/browser/dist/SFSE/Plugins/OSF/Settings/schemas/osf.animation.browser.json') -Raw | ConvertFrom-Json
$coreKeys = @($coreSchema.groups.PSObject.Properties.Value | ForEach-Object { $_.key })
$browserKeys = @($browserSchema.groups.PSObject.Properties.Value | ForEach-Object { $_.key })
$coreHotkeys = $coreSchema.PSObject.Properties['hotkeys']
$browserHotkeys = $browserSchema.PSObject.Properties['hotkeys']
if (($coreHotkeys -and $coreHotkeys.Value) -or @($coreKeys | Where-Object { $_ -like 'browser.*' -or $_ -like 'launch.*' }).Count -or
    @($browserKeys | Where-Object { $_ -in $coreKeys }).Count -or
    -not $browserHotkeys -or -not $browserHotkeys.Value.PSObject.Properties['openBrowser'] -or
    -not $browserHotkeys.Value.PSObject.Properties['openWheel']) {
    throw 'Engine and browser schemas must have separate settings ownership and retain browser hotkey IDs.'
}
# The view retains its picked cast/placement on hide. Keep the native invalidation
# boundary at world replacement; UI tests exercise reopen, rescan and reset replay.
$bridge = [IO.File]::ReadAllText((Join-Path $source 'API/UIBridge.cpp'))
$hidden = [regex]::Match($bridge, '(?s)void OnHidden\(\)(.*?)(?=\n\t\tvoid OnRequestClose)').Value
$reset = [regex]::Match($bridge, '(?s)if \(event == EngineEvent::WorldReset\)(.*?)(?=} else if)').Value
if (-not $hidden -or -not $reset -or $hidden -match 'Clear\w*Tokens\(' -or $reset -notmatch 'UIBridgeWorld::ClearWorldTokens\(\)') {
    throw 'Picked browser references must survive hiding and be invalidated on world replacement.'
}
Write-Host 'Browser boundary: public headers only, independent target, no engine-to-browser implementation dependency; picks have world lifetime.'
