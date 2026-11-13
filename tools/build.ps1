#Requires -Version 7.2
<#
.SYNOPSIS
Builds one kind of build.

.DESCRIPTION
    tools\build.ps1              the debug build
    tools\build.ps1 release      what is handed out
    tools\build.ps1 asan         with the address sanitizer

The quick way while working. tools\check.ps1 is what says that all is well.
#>
param(
    [ValidateSet('debug', 'release', 'asan')] [string] $Preset = 'debug'
)

. "$PSScriptRoot\common.ps1"
Enter-BuildEnvironment

Push-Location $DesktopRoot
try {
    cmake --preset $Preset
    if ($LASTEXITCODE -eq 0) { cmake --build --preset $Preset }
} finally {
    Pop-Location
}
exit $LASTEXITCODE
