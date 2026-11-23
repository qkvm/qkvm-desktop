#Requires -Version 7.2
<#
.SYNOPSIS
Checks the runnable folder with only Windows on PATH, without opening a user profile.
-WithSetup: the content of an MSIX, which carries qKVM Setup beside the program (tools/test_installer.ps1).
-Arch: the kind of processor its images are for (x64 or arm64); by default what its manifest says, else x64.
#>
param([Parameter(Mandatory)] [string] $PackageDirectory, [switch] $WithSetup, [string] $Arch = '')

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\build_common.ps1"
$package = (Resolve-Path -LiteralPath $PackageDirectory).Path
if (-not $Arch) {
    $manifestFile = Join-Path $package 'package-manifest.json'
    $Arch = if (Test-Path -LiteralPath $manifestFile) { [string] (Get-Content -LiteralPath $manifestFile -Raw | ConvertFrom-Json).architecture } else { 'x64' }
}
if ($Arch -notin 'x64', 'arm64') { throw "The runnable package says it is for $Arch." }
# One program ships in the portable ZIP, qkvm-desktop.exe: no setup while it can install nothing (owner, D1), no test,
# bench or stand-in program, and no symbols. An MSIX carries qKVM Setup as well.
$programs = @('qkvm-desktop.exe') + @(if ($WithSetup) { 'qkvm-setup.exe' })
foreach ($file in Get-ChildItem -LiteralPath $package -Recurse -File) {
    $relative = [IO.Path]::GetRelativePath($package, $file.FullName).Replace('\', '/')
    if (($file.Extension -ieq '.exe' -and $programs -cnotcontains $relative) -or $file.Extension -ieq '.pdb') {
        throw "The runnable package holds $relative, which is not handed out."
    }
}
foreach ($program in $programs) {
    if (-not (Test-Path -LiteralPath (Join-Path $package $program) -PathType Leaf)) {
        throw "The runnable package is incomplete: $program is missing from $package."
    }
}
# Fail before launching an incomplete program, so Windows does not show a missing-DLL dialog.
foreach ($required in @(
    'qkvm-desktop.exe', 'Qt6Quick.dll', 'Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Network.dll',
    'Qt6Qml.dll', 'Qt6Svg.dll', 'Qt6QuickTemplates2.dll',
    'avcodec-63.dll', 'avutil-61.dll', 'datachannel.dll',
    'msvcp140.dll', 'msvcp140_atomic_wait.dll', 'vcruntime140.dll',
    'plugins/platforms/qwindows.dll', 'plugins/tls/qschannelbackend.dll',
    'qml/QtQuick/qmldir', 'qml/QtQuick/Templates/qmldir', 'qt.conf'
) + @(
    # (x64's runtime of the second kind of exception handling; ARM64 has none: the Build Tools' ARM64 runtime folder's
    # vcruntime140_1.dll is an x64 one, for emulated code.)
    if ($Arch -eq 'x64') { 'vcruntime140_1.dll' }
) + @(
    # The development FFmpeg's avcodec asks for swresample; the project's own does not.
    $avcodec = Join-Path $package 'avcodec-63.dll'
    if ((Test-Path -LiteralPath $avcodec) -and [Text.Encoding]::Latin1.GetString([IO.File]::ReadAllBytes($avcodec)).Contains("swresample-7.dll`0")) {
        'swresample-7.dll'
    }
)) {
    if (-not (Test-Path -LiteralPath (Join-Path $package $required) -PathType Leaf)) {
        throw "The runnable package is incomplete: $required is missing from $package."
    }
}
# Every program and library of it is of its kind (an x64 one in an ARM64 package, or the other way round, is never
# loaded and is no part of it), 2026-11-19. (What a library it lacks would mean is the program's own start to find.)
$otherKind = @(Get-PackageImageRefusals $package $Arch -KindsOnly)
if ($otherKind.Count) { throw "The runnable package holds an image that is not for ${Arch}: $($otherKind -join '; ')." }
# And each is built with what tools/image-protections.json asks of its kind (ASLR, DEP, CFG and the guard of returns of
# its kind where its row asks them), read from its bytes, 2026-11-21.
$unprotected = @(Get-ImageProtectionRefusals $package $Arch)
if ($unprotected.Count) { throw "The runnable package holds an image not built with what its table asks: $($unprotected -join '; ')." }

$start = [Diagnostics.ProcessStartInfo]::new()
$start.FileName = Join-Path $package 'qkvm-desktop.exe'
$start.ArgumentList.Add('--check-package')
$start.WorkingDirectory = $env:SystemRoot
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
# No Qt, compiler, FFmpeg, QML, injected platform or developer-cache fallback.
foreach ($key in @($start.Environment.Keys)) {
    if ($key -match '^(QT_|QML|QSG_|QKVM_|QV4_)') { [void] $start.Environment.Remove($key) }
}
$start.Environment['PATH'] = "$env:SystemRoot\System32;$env:SystemRoot"
$start.Environment['QT_FORCE_STDERR_LOGGING'] = '1'
$start.Environment['QML_DISABLE_DISK_CACHE'] = '1'

$checks = @(
    @{ Program = 'qkvm-desktop.exe'; Argument = '--check-package'; Marker = 'QKVM_PACKAGE_OK'; Poison = @{} },
    @{ Program = 'qkvm-desktop.exe'; Argument = '--check-package'; Marker = 'QKVM_PACKAGE_OK'; Poison = @{ 'qt_qpa_platform' = 'not-a-platform' } },
    @{ Program = 'qkvm-desktop.exe'; Argument = '--check-package'; Marker = 'QKVM_PACKAGE_OK'; Poison = @{ 'QV4_MAX_CALL_DEPTH' = '1'; 'QV4_CRASH_ON_STACKOVERFLOW' = '1' } },
    @{ Program = 'qkvm-desktop.exe'; Argument = '--check-package'; Marker = 'QKVM_PACKAGE_OK'; Poison = @{ 'QT_FATAL_WARNINGS' = '1'; 'QT_FATAL_CRITICALS' = '1' } }
)
if ($WithSetup) { $checks += @{ Program = 'qkvm-setup.exe'; Argument = '--check-helper'; Marker = 'QKVM_SETUP_OK'; Poison = @{} } }
foreach ($check in $checks) {
    $start.FileName = Join-Path $package $check.Program
    $start.ArgumentList.Clear()
    $start.ArgumentList.Add($check.Argument)
    foreach ($key in @($start.Environment.Keys)) {
        if ($key -match '^(QT_|QML|QSG_|QKVM_|QV4_)') { [void] $start.Environment.Remove($key) }
    }
    foreach ($key in $check.Poison.Keys) { $start.Environment[$key] = $check.Poison[$key] }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    try {
        if (-not $process.Start()) { throw 'The packaged application could not start.' }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(30000)) {
            $process.Kill($true)
            $process.WaitForExit()
            Write-Output ($stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult()).Trim()
            throw 'The packaged application did not finish its startup check within 30 seconds.'
        }
        $said = $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult()
        Write-Output $said.Trim()
        if ($process.ExitCode -ne 0 -or $said -notmatch $check.Marker) {
            throw "The packaged application failed its startup check (exit $($process.ExitCode))."
        }
        Write-Output "$($check.Program) passed with developer paths removed, poisoned overrides=$($check.Poison.Keys -join ','), and no user profile opened."
    } finally {
        $process.Dispose()
    }
}
