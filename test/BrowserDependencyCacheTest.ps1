#requires -Version 7.2
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent
. "$repo/tools/WorkflowCache.ps1"
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile("$repo/tools/BuildBrowserAddon.ps1",[ref]$tokens,[ref]$errors)
$command=$ast.Find({param($n) $n -is [Management.Automation.Language.CommandAst] -and $n.GetCommandName() -eq 'Invoke-WorkflowCached'},$true)
$elements=$command.CommandElements
$selector=$null
for($i=0;$i -lt $elements.Count;$i++) {
    if($elements[$i] -is [Management.Automation.Language.CommandParameterAst] -and $elements[$i].ParameterName -eq 'Outputs') {
        $selector=$elements[$i+1].ScriptBlock.GetScriptBlock();break
    }
}
if(-not $selector){throw 'Dependency output selector missing'}
$ui=Join-Path ([IO.Path]::GetTempPath()) ('osf-npm-cache-'+[guid]::NewGuid().ToString('N'))
try {
    [IO.Directory]::CreateDirectory("$ui/node_modules/package")|Out-Null
    Set-Content "$ui/node_modules/package/index.js" 'installed'
    $original=Get-WorkflowFingerprint -Paths @(& $selector)
    [IO.Directory]::CreateDirectory("$ui/node_modules/.vite")|Out-Null
    Set-Content "$ui/node_modules/.vite/results.json" 'generated'
    if((Get-WorkflowFingerprint -Paths @(& $selector)) -cne $original){throw 'Generated Vite cache invalidated dependency installation'}
    Set-Content "$ui/node_modules/package/index.js" 'edited package'
    if((Get-WorkflowFingerprint -Paths @(& $selector)) -ceq $original){throw 'Changed package was not detected'}
    Remove-Item "$ui/node_modules/package/index.js"
    if((Get-WorkflowFingerprint -Paths @(& $selector)) -ceq $original){throw 'Missing package was not detected'}
    Write-Host 'PASS dependency cache: generated Vite cache ignored; edited/deleted package files invalidate reuse.'
} finally {
    $resolved=[IO.Path]::GetFullPath($ui)
    if(-not $resolved.StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()),[StringComparison]::OrdinalIgnoreCase)){throw 'Scratch path escaped temp directory.'}
    if(Test-Path -LiteralPath $resolved){Remove-Item -LiteralPath $resolved -Recurse -Force}
}
