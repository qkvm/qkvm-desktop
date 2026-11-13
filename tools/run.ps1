#Requires -Version 7.2
<#
.SYNOPSIS
Starts the program as it was built.

.DESCRIPTION
    tools\run.ps1                the debug build
    tools\run.ps1 release

This developer launcher supplies Qt and FFmpeg from the build environment. For an executable
that runs directly from Explorer, use tools/package.ps1 and keep its complete app folder.
#>
param(
    [ValidateSet('debug', 'release', 'asan')] [string] $Preset = 'debug'
)

. "$PSScriptRoot\common.ps1"
Enter-BuildEnvironment

$program = Join-Path $DesktopRoot "build\$Preset\bin\qkvm-desktop.exe"
if (-not (Test-Path -LiteralPath $program)) { throw "There is no $Preset build yet. Build it first: tools\build.ps1 $Preset" }
& $program
