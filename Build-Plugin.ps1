# Build the OSF Animation content plugin: compile the Papyrus scripts and
# (re)build OSFAnimation.esm from the Spriggit YAML source in Plugin\.
#
# The C++ DLL is built by xmake; this covers the Papyrus + .esm side, which
# xmake does not. After running this, a normal `xmake` deploys the .esm and
# .pex into MO2\mods\OSF Animation (the .esm copy is wired into xmake.lua).
#
#   .\Build-Plugin.ps1              # compile scripts + build the .esm
#   .\Build-Plugin.ps1 -NoCompile   # only rebuild the .esm from Plugin\
#   .\Build-Plugin.ps1 -NoPlugin    # only compile the scripts
param(
    [switch]$NoCompile,
    [switch]$NoPlugin
)

$ErrorActionPreference = "Stop"
$Root = $PSScriptRoot

$Compiler   = "C:\Program Files (x86)\Steam\steamapps\common\Starfield\Tools\Papyrus Compiler\PapyrusCompiler.exe"
$Flags      = "C:\Modding\Starfield\PapyrusSource\Starfield_Papyrus_Flags.flg"
$SourceDir  = Join-Path $Root "dist\Scripts\Source"
$ScriptOut  = Join-Path $Root "dist\Scripts"
# OSF's own sources (OSF.psc / OSFTypes.psc, imported by the manager) + the base game sources.
$Imports    = @($SourceDir, "C:\Modding\Starfield\PapyrusSource") -join ";"

$PluginSrc  = Join-Path $Root "Plugin"
$Esm        = Join-Path $Root "OSFAnimation.esm"
# Starfield is a separated-master game: Spriggit needs the Data folder to resolve masters.
$DataFolder = "C:\Program Files (x86)\Steam\steamapps\common\Starfield\Data"

function Find-Spriggit {
    $cmd = Get-Command spriggit -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($p in @(
        "$env:USERPROFILE\.dotnet\tools\spriggit.exe",
        "$env:LOCALAPPDATA\Spriggit\Spriggit.CLI.exe",
        "C:\Modding\Starfield\Tools\Spriggit\Spriggit.CLI.exe"
    )) { if (Test-Path -LiteralPath $p) { return $p } }
    return $null
}

if (-not $NoCompile) {
    if (-not (Test-Path -LiteralPath $Compiler)) { throw "Papyrus compiler not found: $Compiler" }
    Write-Host "Compiling Papyrus scripts..."
    & $Compiler $SourceDir "-i=$Imports" "-o=$ScriptOut" "-f=$Flags" -all 2>&1 | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { throw "Papyrus compile failed" }
}

if (-not $NoPlugin) {
    $Spriggit = Find-Spriggit
    if (-not $Spriggit) { throw "Spriggit CLI not found (looked in PATH, dotnet tools, Tools\Spriggit)." }
    Write-Host "Building OSFAnimation.esm with Spriggit ($Spriggit)..."
    if (Test-Path -LiteralPath $Esm) { Remove-Item -LiteralPath $Esm -Force }
    & $Spriggit deserialize -i $PluginSrc -o $Esm -d "$DataFolder" 2>&1 | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { throw "Spriggit deserialize failed" }
}

Write-Host "Build-Plugin complete. Run 'xmake' (with the game closed) to deploy the .esm + scripts + DLL."
