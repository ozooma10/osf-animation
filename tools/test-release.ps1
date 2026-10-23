#requires -Version 7.2
<#
.SYNOPSIS
Run all automated release checks, including the disposable game scenario.
.DESCRIPTION
Full testing is the default. -Offline explicitly omits in-game assertions and
reports incomplete coverage. Shares the smoke bench, lock, receipts and exit codes.
Game execution requires explicit -Interactive for keyboard, mouse and focus control.
#>
[CmdletBinding()]
param(
    [switch]$Offline,
    [switch]$PrepareOnly,
    [switch]$Interactive,
    [switch]$Plan,
    [switch]$ForceVerification,
    [ValidateSet('Released','Deployed')][string]$Hosts = 'Released',
    [string]$Harness = (Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) 'OSF Test Harness'),
    [string]$OutputDirectory = '',
    [ValidateRange(1,64)][int]$Jobs = 4
)
$ErrorActionPreference = 'Stop'
if ($Offline -and $PrepareOnly) { throw '-Offline and -PrepareOnly are mutually exclusive.' }
$arguments = @('-NoProfile','-File',(Join-Path $PSScriptRoot 'test-smoke.ps1'),'-Hosts',$Hosts,'-Harness',$Harness,'-Jobs',"$Jobs")
if ($PrepareOnly) { $arguments += '-PrepareOnly' } elseif (-not $Offline) { $arguments += '-RunGame' }
if ($Interactive) { $arguments += '-Interactive' }
if ($ForceVerification) { $arguments += '-ForceVerification' }
if ($Plan) { $arguments += '-Plan' }
if ($OutputDirectory) { $arguments += @('-OutputDirectory',$OutputDirectory) }
& pwsh @arguments
exit $LASTEXITCODE
