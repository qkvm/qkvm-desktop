# What the scripts in this folder share. They take it in with:  . "$PSScriptRoot\common.ps1"

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
# Windows PowerShell draws a progress bar for every block of a download, and is many times slower for it.
$ProgressPreference = 'SilentlyContinue'

# The folder all of QKVM Desktop is in.
$DesktopRoot = Split-Path -Parent $PSScriptRoot

# Windows' own tar reads .7z and .zip. Named in full because Git puts another tar first on the path, which reads neither.
$WindowsTar = Join-Path $env:SystemRoot 'System32\tar.exe'

function Test-IsWhatWasRecorded {
    param([string] $File, [long] $Bytes, [string] $Sha256)
    if (-not (Test-Path -LiteralPath $File)) { return $false }
    if ((Get-Item -LiteralPath $File).Length -ne $Bytes) { return $false }
    return (Get-FileHash -LiteralPath $File -Algorithm SHA256).Hash -eq $Sha256
}

function Get-Checked {
    <#
    Brings $Url to $File, unless that file is there already and right, and stops unless what came
    has the size and the SHA-256 the caller wrote down. Nothing that fails this is left on the disk
    to be unpacked by mistake.
    #>
    param([string] $Url, [string] $File, [long] $Bytes, [string] $Sha256)
    if (Test-IsWhatWasRecorded $File $Bytes $Sha256) { return }
    Write-Host ("  fetching {0} ({1:N0} MB)" -f (Split-Path -Leaf $File), ($Bytes / 1MB))
    Invoke-WebRequest -Uri $Url -OutFile $File -UseBasicParsing
    if (-not (Test-IsWhatWasRecorded $File $Bytes $Sha256)) {
        $came = (Get-Item -LiteralPath $File).Length
        Remove-Item -LiteralPath $File -Force
        throw "What came from $Url is not what was recorded ($came bytes came, $Bytes were expected, or the SHA-256 differs). Nothing of it was kept."
    }
}

function Expand-Checked {
    <# Unpacks an archive that Get-Checked has passed into $Into. $Strip leaves out that many leading folders. #>
    param([string] $File, [string] $Into, [int] $Strip = 0)
    New-Item -ItemType Directory -Force -Path $Into | Out-Null
    & $WindowsTar -xf $File -C $Into --strip-components $Strip
    if ($LASTEXITCODE -ne 0) {
        throw "Windows' tar could not unpack $File (exit code $LASTEXITCODE). The tar of Windows 10 cannot read .7z; Windows 11's can."
    }
}

function Get-LocalSetting {
    <# What local.cmake says of $Name (set(<Name> "<value>")), with backslashes; nothing where it says nothing. #>
    param([Parameter(Mandatory)] [string] $Name)
    $local = Join-Path $DesktopRoot 'local.cmake'
    if (-not (Test-Path -LiteralPath $local)) { return $null }
    $line = Select-String -LiteralPath $local -Pattern ('^set\(' + [regex]::Escape($Name) + ' "(.+)"\)$') | Select-Object -First 1
    if (-not $line) { return $null }
    return $line.Matches[0].Groups[1].Value -replace '/', '\'
}

function Get-QtRoot {
    <#
    Where Qt is on this PC, as tools\fetch_qt.ps1 wrote it into local.cmake: the x64 Qt, or with -Arch arm64 the ARM64
    one (tools\fetch_qt.ps1 -Arch arm64).
    #>
    param([ValidateSet('x64', 'arm64')] [string] $Arch = 'x64')
    $local = Join-Path $DesktopRoot 'local.cmake'
    if (-not (Test-Path -LiteralPath $local)) { throw 'Qt is not known on this PC yet. Run tools\fetch_qt.ps1 first (README.md, Building).' }
    if ($Arch -eq 'arm64') {
        $root = Get-LocalSetting 'QKVM_QT_DIR_ARM64'
        if (-not $root) { throw 'local.cmake does not say where the ARM64 Qt is. Run tools\fetch_qt.ps1 -Arch arm64.' }
        return $root
    }
    $root = Get-LocalSetting 'QKVM_QT_DIR'
    if (-not $root) { throw 'local.cmake does not say where Qt is. Run tools\fetch_qt.ps1 again.' }
    return $root
}

function Get-DepsRoot {
    <#
    Where the libraries of others are that a build for $Arch links: .deps for x64; for ARM64 .deps\arm64, or where
    local.cmake says (QKVM_DEPS_ARM64), as CMakeLists.txt finds them.
    #>
    param([ValidateSet('x64', 'arm64')] [string] $Arch = 'x64')
    if ($Arch -eq 'x64') { return (Join-Path $DesktopRoot '.deps') }
    $root = Get-LocalSetting 'QKVM_DEPS_ARM64'
    if ($root) { return $root }
    return (Join-Path $DesktopRoot '.deps\arm64')
}

function Enter-BuildEnvironment {
    <#
    Makes this PowerShell what Visual Studio's "x64 Native Tools" prompt is, and a little more:
    Microsoft's compiler, the CMake, Ninja and clang tools that come with it, and the DLLs of Qt and
    FFmpeg, all on the path. Does nothing when it was done before in the same PowerShell.

    With -Arch arm64, the compiler makes ARM64 programs: Visual Studio's "x64_arm64 Cross Tools" on an x64 PC (where
    the DLLs of the ARM64 Qt and FFmpeg cannot be loaded, and the x64 Qt's tools are on the path instead), its "ARM64
    Native Tools" on Windows on ARM. A PowerShell that entered one stays in it: another is refused.

    $Versions is which Visual Studio will do: Qt 6.12 is built with the compiler of 2022 and
    refuses an older one.
    #>
    param([string] $Versions = '[17.0,18.0)', [ValidateSet('x64', 'arm64')] [string] $Arch = 'x64')
    if ($env:QKVM_BUILD_ENVIRONMENT) {
        $entered = if ($env:QKVM_BUILD_ARCH) { $env:QKVM_BUILD_ARCH } else { 'x64' }
        if ($entered -ne $Arch) { throw "This PowerShell has the build's environment for $entered already: start a new one for $Arch." }
        return
    }
    $onArm = $env:PROCESSOR_ARCHITECTURE -eq 'ARM64'
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vs = $null
    $part = if ($Arch -eq 'arm64') { 'Microsoft.VisualStudio.Component.VC.Tools.ARM64' } else { 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64' }
    if (Test-Path -LiteralPath $vswhere) {
        $vs = & $vswhere -latest -products * -version $Versions -requires $part -property installationPath
    }
    if (-not $vs) { throw "No Build Tools for Visual Studio 2022 with the C++ tools for $Arch is installed (README.md, Building)." }

    # vcvars64.bat sets up a cmd of its own; what it set there is read back and taken over here.
    $batch = if ($Arch -eq 'x64') { 'vcvars64.bat' } elseif ($onArm) { 'vcvarsarm64.bat' } else { 'vcvarsamd64_arm64.bat' }
    $vcvars = Join-Path $vs "VC\Auxiliary\Build\$batch"
    $lines = & $env:ComSpec /d /c "`"$vcvars`" >nul 2>&1 && set"
    if ($LASTEXITCODE -ne 0) { throw "$batch of $vs failed (exit code $LASTEXITCODE)." }
    foreach ($line in $lines) {
        $name, $value = $line -split '=', 2
        if ($name -and $null -ne $value) { Set-Item -LiteralPath "Env:$name" -Value $value }
    }

    # clang-format and clang-tidy come with the "C++ Clang tools" part, which does not put them on the path.
    $llvm = Join-Path $vs "VC\Tools\Llvm\$(if ($onArm) { 'ARM64' } else { 'x64' })\bin"
    if ($Arch -eq 'arm64' -and -not $onArm) {
        # (A cross-build: only the x64 Qt's tools can run here.)
        $env:PATH = "$llvm;$(Get-QtRoot)\bin;$env:PATH"
    } else {
        $ffmpeg = Join-Path (Get-DepsRoot -Arch $Arch) 'ffmpeg\bin'
        $env:PATH = "$llvm;$(Get-QtRoot -Arch $Arch)\bin;$ffmpeg;$env:PATH"
    }
    $env:QKVM_BUILD_ENVIRONMENT = $vs
    $env:QKVM_BUILD_ARCH = $Arch
}

# What this PowerShell may run on, kept while Use-FastCores holds it to less; nothing: it is not held.
if (-not (Test-Path -LiteralPath Variable:global:QkvmAllCores)) { $global:QkvmAllCores = $null }

function Use-FastCores {
    <#
    Holds this PowerShell, and everything it starts from here on, to the fast cores of a PC that
    has two kinds of them (Intel's P and E cores), and says so; with -Not, gives it all of them
    again. Does nothing on a PC whose cores are all of one kind.

    For running the tests, not for building. Windows runs what is not in front of whoever sits at
    the PC on the slow cores when it sees fit, and a test program is half as slow again there. The
    made-up KVMs of the tests speak TLS on the test's own thread; under the address sanitizer, on a
    slow core, three lines are not let in within the second and a half the tests give one, and
    programs that are right fail with "unreachable" (seen on 2026-10-13: the same programs passed
    held to the fast cores and failed held to the slow ones, six runs of six).
    #>
    param([switch] $Not)
    $me = [Diagnostics.Process]::GetCurrentProcess()
    if ($Not) {
        if ($global:QkvmAllCores) { $me.ProcessorAffinity = $global:QkvmAllCores; $global:QkvmAllCores = $null }
        return
    }
    if ($global:QkvmAllCores) { return }
    if (-not ('Qkvm.Cores' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace Qkvm {
    public static class Cores {
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool GetLogicalProcessorInformationEx(int relationship, IntPtr buffer, ref uint length);

        // The logical processors of the cores of the highest efficiency class (which is Windows'
        // word for the fastest kind), as a mask, of the first group of processors only; and all
        // of that group. One entry for each core: its size at 4, its class at 9, its processors
        // at 32 and their group at 40 (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, for a core).
        public static long Fast(out long all) {
            all = 0;
            uint length = 0;
            GetLogicalProcessorInformationEx(0, IntPtr.Zero, ref length);
            if (length == 0) { return 0; }
            IntPtr buffer = Marshal.AllocHGlobal((int)length);
            try {
                if (!GetLogicalProcessorInformationEx(0, buffer, ref length)) { return 0; }
                int best = -1;
                long fast = 0;
                for (long at = 0; at + 48 <= length;) {
                    IntPtr one = new IntPtr(buffer.ToInt64() + at);
                    int size = Marshal.ReadInt32(one, 4);
                    if (size <= 0) { break; }
                    int kind = Marshal.ReadByte(one, 9);
                    long processors = Marshal.ReadInt64(one, 32);
                    if (Marshal.ReadInt16(one, 40) == 0) {
                        all |= processors;
                        if (kind > best) { best = kind; fast = processors; }
                        else if (kind == best) { fast |= processors; }
                    }
                    at += size;
                }
                return fast;
            } finally {
                Marshal.FreeHGlobal(buffer);
            }
        }
    }
}
'@
    }
    $all = [long] 0
    $fast = [Qkvm.Cores]::Fast([ref] $all)
    if ($fast -eq 0 -or $fast -eq $all) { return }
    $global:QkvmAllCores = $me.ProcessorAffinity
    # (Only what this PowerShell may run on to begin with.)
    $held = [long] $global:QkvmAllCores -band $fast
    if ($held -eq 0) { $global:QkvmAllCores = $null; return }
    $me.ProcessorAffinity = [IntPtr] $held
    Write-Host ('The tests are held to the fast cores of this PC (processors 0x{0:X} of 0x{1:X}).' -f $held, $all)
}
