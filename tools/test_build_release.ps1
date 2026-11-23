#Requires -Version 7.2
<#
The release build's checks, without building or fetching anything: the helpers the release scripts share
(build_common.ps1), and what tools/build_release.ps1 refuses before it starts (asked with -CheckOnly, of
made-up exports in a folder of their own). Every failed check is a "FAIL:" line, and the script then fails.
#>
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\build_common.ps1"

$failures = 0
function Check([bool] $Condition, [string] $Message) {
    # (Written to the host: a check inside a function whose output is dropped is still seen.)
    if (-not $Condition) { Write-Host "FAIL: $Message"; $script:failures++ }
}
function Refuses([scriptblock] $Action, [string] $Expected, [string] $Message) {
    try {
        & $Action | Out-Null
        Write-Host "FAIL: $Message (it was accepted)"
        $script:failures++
    } catch {
        if ($_.Exception.Message -notmatch $Expected) {
            Write-Host "FAIL: $Message (refused for another reason: $($_.Exception.Message))"
            $script:failures++
        }
    }
}

$desktop = Split-Path -Parent $PSScriptRoot
$work = Join-Path $desktop "build/release-tests/$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $work | Out-Null

# --- The public key the program pins.
$key = New-ThrowawayPublicKey
Check (Test-ReleasePublicKey $key) 'a key just made is taken'
Check (Test-ReleasePublicKey $key.ToUpperInvariant()) 'a key in capitals is taken'
Check ($key -cne (New-ThrowawayPublicKey)) 'two keys made are two'
$last = if ($key[-1] -eq '0') { '1' } else { '0' }
Check (-not (Test-ReleasePublicKey ($key.Substring(0, 127) + $last))) 'a point that is not on the curve is refused'
Check (-not (Test-ReleasePublicKey $key.Substring(0, 127))) '127 digits are refused'
Check (-not (Test-ReleasePublicKey ($key + "`n"))) 'a key with a line feed after it is refused'
Check (-not (Test-ReleasePublicKey ('g' + $key.Substring(1)))) 'a letter that is no digit is refused'
Check (-not (Test-ReleasePublicKey '')) 'nothing is no key'

# --- Where a program says its symbols are.
function New-FixtureImage([string] $Path, [string] $Symbols) {
    # A 64-bit Windows image of one section, whose debug directory holds one CodeView record naming $Symbols.
    $bytes = [byte[]]::new(0x400)
    $put = { param([int] $At, [long] $Value, [int] $Size) [BitConverter]::GetBytes($Value)[0..($Size - 1)].CopyTo($bytes, $At) }
    $bytes[0] = 0x4D; $bytes[1] = 0x5A
    & $put 0x3C 0x40 4
    & $put 0x40 0x00004550 4
    & $put 0x44 0x8664 2
    & $put 0x46 1 2
    & $put 0x54 240 2
    $optional = 0x58
    & $put $optional 0x20B 2
    & $put ($optional + 108) 16 4
    & $put ($optional + 112 + 48) 0x1000 4
    & $put ($optional + 112 + 52) 28 4
    $section = $optional + 240
    [Text.Encoding]::ASCII.GetBytes('.rdata').CopyTo($bytes, $section)
    & $put ($section + 8) 0x200 4
    & $put ($section + 12) 0x1000 4
    & $put ($section + 16) 0x200 4
    & $put ($section + 20) 0x200 4
    $name = [Text.Encoding]::UTF8.GetBytes($Symbols)
    & $put (0x200 + 12) 2 4
    & $put (0x200 + 16) (24 + $name.Length + 1) 4
    & $put (0x200 + 20) 0x101C 4
    & $put (0x200 + 24) 0x21C 4
    & $put 0x21C 0x53445352 4
    $name.CopyTo($bytes, 0x21C + 24)
    [IO.File]::WriteAllBytes($Path, $bytes)
}
New-FixtureImage "$work/bare.exe" 'qkvm-desktop.pdb'
New-FixtureImage "$work/full.exe" 'Q:\b\qkvm-desktop-0.1.0\src\build\publish\bin\qkvm-desktop.pdb'
New-FixtureImage "$work/relative.exe" 'bin/qkvm-desktop.pdb'
Check ((@(Get-PeDebugPaths "$work/bare.exe") -join '|') -ceq 'qkvm-desktop.pdb') 'the symbols a program names are read'
Check (Test-BareDebugPath "$work/bare.exe") 'a bare file name is taken'
Check (-not (Test-BareDebugPath "$work/full.exe")) 'a path with its drive and folders is refused'
Check (-not (Test-BareDebugPath "$work/relative.exe")) 'a path with a folder is refused'
[IO.File]::WriteAllText("$work/text.exe", 'not a program at all')
Refuses { Get-PeDebugPaths "$work/text.exe" } 'not a Windows image' 'a file that is no program is not read as one'

# --- The build folder in what ships.
$folder = 'Q:\b\made-up-build'
$ways = [ordered]@{
    'plain' = [Text.Encoding]::Latin1.GetBytes("x`0Q:\b\made-up-build\src\.deps\a.c`0")
    'slashes' = [Text.Encoding]::Latin1.GetBytes("q:/B/Made-Up-Build/src")
    'doubled' = [Text.Encoding]::Latin1.GetBytes('{"at": "Q:\\b\\made-up-build\\x"}')
    'wide' = [byte[]](@(7) + [Text.Encoding]::Unicode.GetBytes('Q:\b\made-up-build\x.pdb'))
    'wide-even' = [Text.Encoding]::Unicode.GetBytes('..Q:/b/made-up-build/x')
    'big' = [Text.Encoding]::BigEndianUnicode.GetBytes('Q:\b\made-up-build')
}
foreach ($way in $ways.Keys) {
    [IO.File]::WriteAllBytes("$work/$way.bin", $ways[$way])
    Check (Test-NamesFolder "$work/$way.bin" $folder) "the build folder is found ($way)"
}
[IO.File]::WriteAllBytes("$work/clean.bin", [Text.Encoding]::Latin1.GetBytes("\libdatachannel\deps\usrsctp\a.c`0Q:\b\made-up`0build"))
Check (-not (Test-NamesFolder "$work/clean.bin" $folder)) 'a file without the build folder passes'

# --- One digest for a folder.
foreach ($name in 'one', 'two') {
    New-Item -ItemType Directory -Path "$work/$name/sub" | Out-Null
    [IO.File]::WriteAllText("$work/$name/a.txt", 'a')
    [IO.File]::WriteAllText("$work/$name/sub/b.txt", 'b')
}
Check ((Get-TreeDigest "$work/one") -ceq (Get-TreeDigest "$work/two")) 'the same files make the same digest'
[IO.File]::WriteAllText("$work/two/sub/b.txt", 'c')
Check ((Get-TreeDigest "$work/one") -cne (Get-TreeDigest "$work/two")) 'a changed byte makes another digest'
[IO.File]::WriteAllText("$work/two/sub/b.txt", 'b')
Move-Item "$work/two/a.txt" "$work/two/z.txt"
Check ((Get-TreeDigest "$work/one") -cne (Get-TreeDigest "$work/two")) 'a renamed file makes another digest'

# --- The project's minimal FFmpeg, by the configuration line it keeps (what tools/build_ffmpeg.ps1 holds its build to,
# and tools/audit_release.ps1 the package's avcodec).
$ffmpeg = "--prefix=/ffmpeg --target-os=mingw32 --arch=x86_64 --enable-cross-compile --cross-prefix=x86_64-w64-mingw32- " +
    "--cc=x86_64-w64-mingw32-gcc-win32 --host-cc=x86_64-w64-mingw32-gcc-win32 --pkg-config=false --enable-shared " +
    "--disable-static --enable-version3 --disable-autodetect --disable-everything --disable-programs --disable-doc " +
    "--disable-network --disable-debug --disable-avformat --disable-avdevice --disable-avfilter --disable-swscale " +
    "--disable-swresample --enable-w32threads --enable-d3d11va --enable-zlib --enable-decoder='h264,mjpeg,png' " +
    "--enable-parser=h264 --enable-hwaccel='h264_d3d11va,h264_d3d11va2' --extra-cflags=-I../zlib " +
    "--extra-ldflags='-L../zlib -static-libgcc'"
$said = @(Get-FFmpegRefusals $ffmpeg)
Check ($said.Count -eq 0) "the configuration the build is made with is the minimal one ($($said -join '; '))"
$wrong = [ordered]@{
    'without --disable-everything' = @(($ffmpeg -replace ' --disable-everything', ''), 'does not say --disable-everything')
    'without --disable-autodetect' = @(($ffmpeg -replace ' --disable-autodetect', ''), 'does not say --disable-autodetect')
    'without --enable-version3' = @(($ffmpeg -replace ' --enable-version3', ''), 'does not say --enable-version3')
    'with everything later enabled' = @("$ffmpeg --enable-everything", 'enables --enable-everything')
    'with OpenSSL' = @("$ffmpeg --enable-openssl", 'enables --enable-openssl')
    'with GnuTLS' = @("$ffmpeg --enable-gnutls", 'enables --enable-gnutls')
    'with Mbed TLS' = @("$ffmpeg --enable-mbedtls", 'enables --enable-mbedtls')
    'with Schannel' = @("$ffmpeg --enable-schannel", 'enables --enable-schannel')
    'with a library of lib*' = @("$ffmpeg --enable-libdav1d", 'enables --enable-libdav1d')
    'with GPL parts' = @("$ffmpeg --enable-gpl", 'enables --enable-gpl')
    'with non-free parts' = @("$ffmpeg --enable-nonfree", 'enables --enable-nonfree')
    'with another decoder' = @(($ffmpeg -replace "h264,mjpeg,png", 'h264,mjpeg,png,hevc'), 'enables the decoder hevc')
    'with an outside decoder' = @("$ffmpeg --enable-decoder=libdav1d", 'enables the decoder libdav1d')
    'with another parser' = @("$ffmpeg --enable-parser=hevc", 'enables the parser hevc')
    'with another hardware decoding' = @("$ffmpeg --enable-hwaccel=h264_dxva2", 'enables the hwaccel h264_dxva2')
    'with something in capitals' = @("$ffmpeg --ENABLE-OPENSSL", 'enables --ENABLE-OPENSSL')
}
foreach ($case in $wrong.GetEnumerator()) {
    $said = @(Get-FFmpegRefusals $case.Value[0])
    Check (@($said | Where-Object { $_ -match [regex]::Escape($case.Value[1]) }).Count -eq 1) "a configuration $($case.Key) is refused ($($said -join '; '))"
}
Check (@(Get-FFmpegRefusals '').Count -gt 0) 'no configuration is no minimal one'
# (A word that only begins as one of them is not it.)
Check (@(@(Get-FFmpegRefusals ($ffmpeg -replace '--disable-everything', '--disable-everything-but')) -match 'does not say --disable-everything').Count -eq 1) 'a longer word is not --disable-everything'
# The ARM64 build (build_ffmpeg.ps1 -Arch arm64), held to the same rule and to its own: for ARM64, with FFmpeg's ARM64
# code (NEON among it) left on.
$ffmpegArm = $ffmpeg.Replace('--arch=x86_64', '--arch=aarch64').Replace('x86_64-w64-mingw32-gcc-win32', 'aarch64-w64-mingw32-clang').
    Replace('--cross-prefix=x86_64-w64-mingw32-', '--cross-prefix=aarch64-w64-mingw32-')
$said = @(Get-FFmpegRefusals $ffmpegArm -Arch arm64)
Check ($said.Count -eq 0) "the configuration the ARM64 build is made with is the minimal one ($($said -join '; '))"
$wrongArm = [ordered]@{
    'for x64' = @($ffmpeg, 'does not say --arch=aarch64')
    'for another system' = @(($ffmpegArm -replace '--target-os=mingw32', '--target-os=linux'), 'does not say --target-os=mingw32')
    'naming a second architecture' = @("$ffmpegArm --arch=x86_64", 'says --arch=x86_64')
    'without assembly' = @("$ffmpegArm --disable-asm", "ARM64 code off (--disable-asm)")
    'without inline assembly' = @("$ffmpegArm --disable-inline-asm", "ARM64 code off (--disable-inline-asm)")
    'without NEON' = @("$ffmpegArm --disable-neon", "ARM64 code off (--disable-neon)")
    'without ARMv8' = @("$ffmpegArm --disable-armv8", "ARM64 code off (--disable-armv8)")
    'unoptimised' = @("$ffmpegArm --disable-optimizations", "ARM64 code off (--disable-optimizations)")
    'without NEON, in capitals' = @("$ffmpegArm --DISABLE-NEON", "ARM64 code off (--DISABLE-NEON)")
    'with OpenSSL' = @("$ffmpegArm --enable-openssl", 'enables --enable-openssl')
    'without --disable-everything' = @(($ffmpegArm -replace ' --disable-everything', ''), 'does not say --disable-everything')
    # (Review arm 1, L2: the words that turned the ARM64 code off yet passed, and the compiler and its flags held.)
    'without SVE' = @("$ffmpegArm --disable-sve", "ARM64 code off (--disable-sve)")
    'without SVE2' = @("$ffmpegArm --disable-sve2", "ARM64 code off (--disable-sve2)")
    'without the choice at run time' = @("$ffmpegArm --disable-runtime-cpudetect", "ARM64 code off (--disable-runtime-cpudetect)")
    'for one processor' = @("$ffmpegArm --cpu=cortex-a53", 'says --cpu=cortex-a53')
    'with another compiler' = @(($ffmpegArm -replace '--cc=aarch64-w64-mingw32-clang', '--cc=aarch64-w64-mingw32-gcc'), 'does not say --cc=aarch64-w64-mingw32-clang alone')
    'with a second compiler' = @("$ffmpegArm --cc=/tmp/cc", 'does not say --cc=aarch64-w64-mingw32-clang alone')
    'with another prefix' = @(($ffmpegArm -replace '--cross-prefix=aarch64-w64-mingw32-', '--cross-prefix=/tmp/aarch64-w64-mingw32-'), 'does not say --cross-prefix=aarch64-w64-mingw32- alone')
    'with the vector code off by a flag' = @(($ffmpegArm -replace '--extra-cflags=-I\.\./zlib', "--extra-cflags='-I../zlib -mgeneral-regs-only'"), 'does not say --extra-cflags=-I../zlib alone')
    'with flags of the C++ compiler' = @("$ffmpegArm --extra-cxxflags=-O0", 'says --extra-cxxflags=-O0')
}
# (The line the ARM64 FFmpeg of 2026-11-19 was configured with, as its build.json keeps it, is the minimal one.)
$madeArm = "--prefix=/ffmpeg --target-os=mingw32 --arch=aarch64 --enable-cross-compile --cross-prefix=aarch64-w64-mingw32- " +
    "--cc=aarch64-w64-mingw32-clang --host-cc=aarch64-w64-mingw32-clang --pkg-config=false --enable-shared --disable-static " +
    "--enable-version3 --disable-autodetect --disable-everything --disable-programs --disable-doc --disable-network " +
    "--disable-debug --disable-avformat --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample " +
    "--enable-w32threads --enable-d3d11va --enable-zlib --enable-decoder='h264,mjpeg,png' --enable-parser=h264 " +
    "--enable-hwaccel='h264_d3d11va,h264_d3d11va2' --extra-cflags=-I../zlib --extra-ldflags='-L../zlib -static-libgcc'"
$said = @(Get-FFmpegRefusals $madeArm -Arch arm64)
Check ($said.Count -eq 0) "the ARM64 FFmpeg's own line passes ($($said -join '; '))"

# --- An ARM64 package holds the x64 package's files, path for path, but x64's vcruntime140_1.dll and a development
# FFmpeg's swresample (Get-PackagePairRefusals, for the release tools that make both; review arm 1, L3).
function Manifest([string] $Arch, [string[]] $Paths, [string] $Version = '0.2.0') {
    return [pscustomobject]@{ product = 'qKVM'; version = $Version; architecture = $Arch
        files = @($Paths | ForEach-Object { [pscustomobject]@{ path = $_; bytes = 1; sha256 = '0' } }) }
}
$shared = @('qkvm-desktop.exe', 'Qt6Core.dll', 'plugins/platforms/qwindows.dll', 'qml/QtQuick/qmldir', 'vcruntime140.dll', 'licenses/mingw-w64-runtime.txt')
$said = @(Get-PackagePairRefusals (Manifest 'x64' ($shared + 'vcruntime140_1.dll' + 'swresample-7.dll')) (Manifest 'arm64' $shared))
Check ($said.Count -eq 0) "an ARM64 package of the x64 one's files but vcruntime140_1.dll and swresample passes ($($said -join '; '))"
$said = @(Get-PackagePairRefusals (Manifest 'x64' ($shared + 'vcruntime140_1.dll')) (Manifest 'arm64' ($shared | Where-Object { $_ -ne 'plugins/platforms/qwindows.dll' })))
Check (@($said) -join '; ' -ceq 'the ARM64 package lacks plugins/platforms/qwindows.dll') "an ARM64 package that lacks a file of x64's is refused ($($said -join '; '))"
$said = @(Get-PackagePairRefusals (Manifest 'x64' $shared) (Manifest 'arm64' ($shared + 'plugins/tls/qopensslbackend.dll')))
Check (@($said) -join '; ' -ceq 'the ARM64 package holds plugins/tls/qopensslbackend.dll, which the x64 one does not') "an ARM64 package with a file more is refused ($($said -join '; '))"
$said = @(Get-PackagePairRefusals (Manifest 'x64' $shared) (Manifest 'arm64' ($shared + 'vcruntime140_1.dll')))
Check (@($said) -join '' -match 'holds vcruntime140_1\.dll') "an ARM64 package with x64's second part of the runtime is refused ($($said -join '; '))"
Check (@(Get-PackagePairRefusals (Manifest 'x64' $shared) (Manifest 'arm64' $shared '0.2.1')) -join '' -match 'two versions') 'packages of two versions are refused'
Check (@(Get-PackagePairRefusals (Manifest 'arm64' $shared) (Manifest 'x64' $shared)) -join '' -match 'not an x64 package and an ARM64 one') 'packages the wrong way round are refused'
foreach ($case in $wrongArm.GetEnumerator()) {
    $said = @(Get-FFmpegRefusals $case.Value[0] -Arch arm64)
    Check (@($said | Where-Object { $_ -match [regex]::Escape($case.Value[1]) }).Count -eq 1) "an ARM64 configuration $($case.Key) is refused ($($said -join '; '))"
}
# (And the x64 rule is the one it was: the ARM64 words are no part of it.)
Check (@(Get-FFmpegRefusals "$ffmpeg --disable-neon").Count -eq 0) 'the x64 rule does not ask for ARM64 code'
# --- A package's images are of its own kind, and import nothing it lacks (package.ps1 refuses to make one that is not
# so, and test_package.ps1 refuses one). Made-up images: a header, one section, an import table naming the libraries.
function New-MadeUpImage([uint16] $Machine, [string[]] $Imports = @()) {
    $bytes = [byte[]]::new(0x600)
    $bytes[0] = 0x4D; $bytes[1] = 0x5A
    [BitConverter]::GetBytes([int] 0x40).CopyTo($bytes, 0x3C)
    [BitConverter]::GetBytes([uint32] 0x00004550).CopyTo($bytes, 0x40)
    [BitConverter]::GetBytes($Machine).CopyTo($bytes, 0x44)
    [BitConverter]::GetBytes([uint16] 1).CopyTo($bytes, 0x46)
    [BitConverter]::GetBytes([uint16] 0xF0).CopyTo($bytes, 0x54)
    [BitConverter]::GetBytes([uint16] 0x20B).CopyTo($bytes, 0x58)
    [BitConverter]::GetBytes([uint32] 16).CopyTo($bytes, 0x58 + 108)
    [BitConverter]::GetBytes([uint32] 0x1000).CopyTo($bytes, 0x58 + 120)
    [BitConverter]::GetBytes([uint32] 0x200).CopyTo($bytes, 0x58 + 124)
    $section = 0x58 + 0xF0
    foreach ($at in 8, 16) { [BitConverter]::GetBytes([uint32] 0x400).CopyTo($bytes, $section + $at) }
    [BitConverter]::GetBytes([uint32] 0x1000).CopyTo($bytes, $section + 12)
    [BitConverter]::GetBytes([uint32] 0x200).CopyTo($bytes, $section + 20)
    $name = 20 * ($Imports.Count + 1)
    for ($index = 0; $index -lt $Imports.Count; ++$index) {
        [BitConverter]::GetBytes([uint32] (0x1000 + $name)).CopyTo($bytes, 0x200 + 20 * $index + 12)
        [Text.Encoding]::ASCII.GetBytes($Imports[$index]).CopyTo($bytes, 0x200 + $name)
        $name += $Imports[$index].Length + 1
    }
    return $bytes
}
$x64 = [uint16] 0x8664
$arm64 = [uint16] 0xAA64
Check ((Get-PeMachine (New-MadeUpImage $arm64)) -eq $arm64 -and (Get-PeMachine (New-MadeUpImage $x64)) -eq $x64) 'the machine of an image is read'
Check (@(Get-PeImports (New-MadeUpImage $arm64 @('kernel32.dll', 'b.dll'))) -join ',' -ceq 'kernel32.dll,b.dll') 'a made-up image says what it imports'
function New-MadeUpPackage([string] $Name, [hashtable] $Images) {
    $folder = Join-Path $work "package-$Name"
    foreach ($entry in $Images.GetEnumerator()) {
        $file = Join-Path $folder $entry.Key
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $file) | Out-Null
        [IO.File]::WriteAllBytes($file, (New-MadeUpImage $entry.Value[0] @($entry.Value | Select-Object -Skip 1)))
    }
    return $folder
}
$whole = New-MadeUpPackage 'arm64-whole' @{
    'qkvm-desktop.exe' = @($arm64, 'Qt6Core.dll', 'KERNEL32.dll', 'api-ms-win-crt-runtime-l1-1-0.dll', 'vcruntime140.dll')
    'Qt6Core.dll' = @($arm64, 'kernel32.dll'); 'vcruntime140.dll' = @($arm64)
    'plugins/platforms/qwindows.dll' = @($arm64, 'Qt6Core.dll', 'user32.dll') }
$said = @(Get-PackageImageRefusals $whole 'arm64')
Check ($said.Count -eq 0) "an ARM64 package of ARM64 images that import what it holds or Windows has passes ($($said -join '; '))"
$wrongKind = New-MadeUpPackage 'arm64-x64' @{ 'qkvm-desktop.exe' = @($arm64); 'vcruntime140_1.dll' = @($x64) }
$said = @(Get-PackageImageRefusals $wrongKind 'arm64')
Check (@($said | Where-Object { $_ -match 'vcruntime140_1\.dll is a program for x64, not arm64' }).Count -eq 1) "an x64 image in an ARM64 package is refused ($($said -join '; '))"
$wrongKind = New-MadeUpPackage 'x64-arm64' @{ 'qkvm-desktop.exe' = @($x64); 'plugins/tls/a.dll' = @($arm64) }
$said = @(Get-PackageImageRefusals $wrongKind 'x64')
Check (@($said | Where-Object { $_ -match 'plugins/tls/a\.dll is a program for arm64, not x64' }).Count -eq 1) "an ARM64 image in an x64 package is refused ($($said -join '; '))"
$lacking = New-MadeUpPackage 'arm64-lacking' @{ 'qkvm-desktop.exe' = @($arm64, 'Qt6Gui.dll', 'msvcp140.dll') }
$said = @(Get-PackageImageRefusals $lacking 'arm64')
Check (@($said | Where-Object { $_ -match 'qkvm-desktop\.exe imports Qt6Gui\.dll' }).Count -eq 1) "an import the package lacks is refused ($($said -join '; '))"
# (The Visual C++ runtime is the package's to carry, wherever this PC's Windows has one of its own.)
Check (@($said | Where-Object { $_ -match 'imports msvcp140\.dll' }).Count -eq 1) "a Visual C++ runtime import the package lacks is refused though Windows here has one ($($said -join '; '))"
Check (@(Get-PackageImageRefusals $whole 'x64').Count -eq 4) 'every ARM64 image of a package said to be x64 is refused'
# (The kinds alone, as the startup checks look: what is lacking is the program's start to find, but a wrong kind not.)
Check (@(Get-PackageImageRefusals $lacking 'arm64' -KindsOnly).Count -eq 0) 'the kinds alone leave a lacking import be'
Check (@(Get-PackageImageRefusals (Join-Path $work 'package-arm64-x64') 'arm64' -KindsOnly).Count -eq 1) 'the kinds alone refuse a wrong kind'
# The package's own checks (test_package.ps1) refuse an image of the other kind before they start anything: a made-up
# ARM64 package with every file they ask for, and the x64 vcruntime140_1.dll of the Build Tools' ARM64 folder beside.
$images = @{}
foreach ($one in 'qkvm-desktop.exe', 'Qt6Quick.dll', 'Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Network.dll', 'Qt6Qml.dll', 'Qt6Svg.dll',
    'Qt6QuickTemplates2.dll', 'avcodec-63.dll', 'avutil-61.dll', 'datachannel.dll', 'msvcp140.dll', 'msvcp140_atomic_wait.dll',
    'vcruntime140.dll', 'plugins/platforms/qwindows.dll', 'plugins/tls/qschannelbackend.dll') { $images[$one] = @($arm64) }
$images['vcruntime140_1.dll'] = @($x64)
$checked = New-MadeUpPackage 'arm64-checked' $images
foreach ($text in 'qml/QtQuick/qmldir', 'qml/QtQuick/Templates/qmldir', 'qt.conf') {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent (Join-Path $checked $text)) | Out-Null
    Set-Content -LiteralPath (Join-Path $checked $text) -Value 'made up'
}
Refuses { & "$PSScriptRoot\test_package.ps1" -PackageDirectory $checked -Arch arm64 } 'holds an image that is not for arm64.*vcruntime140_1\.dll is a program for x64' 'the package checks refuse an x64 image in an ARM64 package'
Refuses { & "$PSScriptRoot\test_package.ps1" -PackageDirectory $checked -Arch x64 } 'holds an image that is not for x64.*is a program for arm64' 'the package checks refuse ARM64 images in an x64 package'

# build_ffmpeg.ps1 holds what it built to this rule, for the kind it built, and to nothing of its own.
$builder = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'build_ffmpeg.ps1'))
Check ($builder -match '\$refused = @\(Get-FFmpegRefusals \$configuration -Arch \$Arch\)' -and $builder -match 'if \(\$refused\.Count\) \{ throw') 'build_ffmpeg.ps1 refuses what the rule refuses'

# --- The setup, as build_release.ps1 makes it from its run's own package (it cannot be built here: what it runs, and
# in what order, is read from it). Inno Setup is unpacked anew in the build folder, the setup and its uncompressed twin
# are made from the moved package, the setup's static test runs, the beta audit is given the setup, the byte scan and
# the build folder's check look through both, and the record says the setup.
$releaseBuild = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'build_release.ps1')).Replace("`r`n", "`n")
function Check-Order([string] $Text, [string] $Label, [Collections.Specialized.OrderedDictionary] $Steps) {
    # Each step's words, as they stand, after the one before.
    $at = 0
    foreach ($step in $Steps.GetEnumerator()) {
        $found = $Text.IndexOf($step.Value, $at, [StringComparison]::Ordinal)
        Check ($found -ge 0) "build_release.ps1, ${Label}: $($step.Key), in its order"
        if ($found -ge 0) { $at = $found + $step.Value.Length }
    }
}
# The FFmpeg part: each kind built anew, without import libraries; the record checked against every earlier one of the
# same pins before it is written, the part held to it, and the record kept for the next release.
Check-Order $releaseBuild 'the FFmpeg part' ([ordered]@{
    'the earlier records are read before anything is built' = '$earlier += [pscustomobject]@{ Name = $item.Name; Record = $was }'
    'it stops before building where it is only asked' = 'if ($CheckOnly) {'
    'each kind''s work folder is made anew' = 'if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }'
    'each kind is built without import libraries' = "Invoke-Clean `"`$step-ffmpeg-`$kind`" @('-File', `"`$src\tools\build_ffmpeg.ps1`", '-Arch', `$kind, '-Downloads', `"`$downloads\ffmpeg`", '-Work', `$work, '-NoImportLibraries')"
    'nothing in the part names the build folder' = 'if (Test-NamesFolder $file.FullName $Root) { throw "$([IO.Path]::GetRelativePath($partFolder'
    'the record is made of the part' = '$record = New-FFmpegRecord -Part $partFolder -Version $version -Commit $commit -Tree $tree -Scripts $scripts'
    'it is held to the earlier records of the same pins' = '$reproduced = Get-FFmpegReproductionRefusals -Record $record -Earlier $earlier'
    'and stops where any one differs' = 'if ($reproduced.Refusals.Count) {'
    'where it is not the same bytes, the release stops' = "Rename-Item -LiteralPath `$partFolder -NewName 'not-reproduced'"
    'only then is the record written' = '$record | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $recordFile'
    'the part is held to its record' = '$self = Get-FFmpegPartRefusals -Folder $partFolder -Commit $commit -Tree $tree'
    'the record is kept for the next release' = 'Copy-Item -LiteralPath $recordFile -Destination $kept'
})
Check ($releaseBuild.Contains("throw `"FFmpeg was not built as before from the same pins, and the release stops here:")) 'a build that is not the same bytes as before stops the release'
# The build part: the FFmpeg part held to its record before anything is built, and its copies again; the import
# libraries made, both kinds built and packaged, the setup made of both packages, each kind's beta audit given the
# FFmpeg part's record.
Check-Order $releaseBuild 'the build part' ([ordered]@{
    'the FFmpeg part is held to its record' = '$ffmpegPart = Get-FFmpegPartRefusals -Folder $FFmpeg -Commit $commit -Tree $tree'
    'and refused where anything is not as it says' = 'if ($ffmpegPart.Refusals.Count) { throw "The FFmpeg part in $FFmpeg is not taken:'
    'before anything is built' = 'if ($CheckOnly) {'
    'its copies are held to the record once more' = '$again = @(Compare-FolderToFiles $ffmpegIn[$kind] @($build.files) $kind)'
    'and refused where they are not' = 'if ($again.Count) { throw'
    'so are the sources'' copies' = 'throw "The copy of $($source.archive) is not the archive the FFmpeg part''s record names."'
    'Qt for x64' = "Invoke-Clean '1-qt' @('-File', `"`$src\tools\fetch_qt.ps1`", '-Into', `$qt, '-Downloads', `"`$downloads\qt`")"
    'and for ARM64' = "'-Arch', 'arm64')"
    'the import libraries are made from the part''s export lists' = "@('-File', `"`$src\tools\build_ffmpeg.ps1`", '-Arch', `$kind, '-ImportLibrariesOnly')"
    'the ARM64 build runs x64''s tool' = "'-DQKVM_HOST_TOOLS=`$src\build\publish\bin'"
    'x64 is packaged from its publish build' = "Invoke-Clean '5-package-x64' @('-File', `"`$src\tools\package.ps1`", '-Preset', 'publish', '-SkipBuild')"
    'and ARM64 from its own' = "Invoke-Clean '5-package-arm64' @('-File', `"`$src\tools\package.ps1`", '-Arch', 'arm64', '-Preset', 'publish', '-SkipBuild')"
    'the packages are moved into the run' = 'Move-Item -LiteralPath $made.folder -Destination $package'
    'each program carries the feed and the key' = 'if (-not $programBytes.Contains("$carried`0")) { throw'
    # (Once both packages are made: the ARM64 one holds x64's files path for path, but what only one kind has; review
    # arm 1, L3, which left the call to the release build.)
    'the ARM64 package is held to the x64 one''s files' = '$pair = @(Get-PackagePairRefusals -X64 (Read-PackageManifest $packages[''x64'']) -Arm64 (Read-PackageManifest $packages[''arm64'']))'
    'and refused where it is not' = 'if ($pair.Count) { throw "The ARM64 package is not the x64 one''s files of its kind:'
    'Inno Setup is unpacked in the build folder' = "Invoke-Clean '6-inno' @('-File', `"`$src\tools\fetch_inno.ps1`", '-Into', `$inno, '-Downloads', `"`$downloads\inno`")"
    'the setup is made of both packages, with both update checks and its twin' = "Invoke-Clean '7-setup' @('-File', `"`$src\tools\build_setup.ps1`", '-PackageDirectory', `$bothPackages, '-Inno', (Join-Path `$inno `$InnoVersion), '-OutputDirectory', `$setupWork, '-UpdateCheck', `$bothChecks, '-ScanTwin')"
    'the setup''s static test runs on it' = "Invoke-Clean '8-setup-test' @('-File', `"`$src\tools\test_setup.ps1`", '-SetupDirectory', `$setupWork, '-PackageDirectory', `$bothPackages, '-Inno', (Join-Path `$inno `$InnoVersion))"
    'each kind''s beta audit is given the setup and the FFmpeg part''s record' = 'audit_release.ps1" -PackageDirectory $packages[$kind] -OutputFile $auditFile -Beta -SetupDirectory $setupWork -SetupPackageDirectory $bothPackages -FFmpegRecord $ffmpegRecordFile'
    'Inno Setup''s tree is checked once more before the scan' = 'if ((Get-TreeDigest $innoTree) -cne $InnoTreeDigest -or @(Get-ChildItem -LiteralPath $innoTree -Recurse -File -Force).Count -ne $InnoTreeFiles) {'
    'the byte scan takes only Inno Setup''s two programs as theirs' = "`$theirs = @('--public', (Join-Path `$innoTree 'Setup.e32'), '--public', (Join-Path `$innoTree 'SetupLdr.e32'))"
    'the byte scan looks through both kinds, the setup and its twin' = '@theirs $packages[''x64''] $zips[''x64''] $packages[''arm64''] $zips[''arm64''] $setupFile $twinFile'
})
$presetsOfBuild = [regex]::Match($releaseBuild, '\$presetOf = \[ordered\]@\{ x64 = ''publish''; arm64 = ''arm64-publish'' \}').Success
Check $presetsOfBuild 'build_release.ps1 builds x64 with the publish preset and ARM64 with arm64-publish'
Check ($releaseBuild.Contains('foreach ($old in $src, $qt, $temp, $inno)')) 'build_release.ps1 unpacks Inno Setup anew at every run'
Check ($releaseBuild -match 'foreach \(\$file in \$setupFile, \$twinFile\) \{\s+if \(Test-NamesFolder \$file \$Root\)') 'build_release.ps1 refuses a setup that names the folder it was built in'
Check ($releaseBuild.Contains('setup = [ordered]@{ name = $setupName; file = "setup/$setupName"') -and $releaseBuild.Contains("setupTest = 'passed'")) 'the build record names the setup and its test'
Check ($releaseBuild.Contains('record = $ffmpegPart.Record') -and $releaseBuild.Contains('recordSha256 = Get-Sha256 $ffmpegRecordFile')) 'the build record holds the FFmpeg part''s record'
# (And the tools of the ARM64 start checks in the tree it built, by SHA-256, which release_files.ps1 and
# release_commands.ps1 hold the record of those checks to; review r2 of arm and armp, L2.)
Check ($releaseBuild.Contains('arm64StartupTools = Get-StartupCheckTools "$src\tools"')) 'the build record names the tools of the ARM64 start checks of the tree it built'

# --- Every native command's end is read (outside review 2, 5.1). The build of each kind, a script given to a child
# PowerShell: as build_release.ps1 makes it of its words, it reads, and after its build it asks the tools what they are
# by Get-BuildToolVersions.
$parsed = [Management.Automation.Language.Parser]::ParseInput($releaseBuild, [ref] $null, [ref] $null)
$given = @($parsed.FindAll({ param($node) $node -is [Management.Automation.Language.AssignmentStatementAst] -and "$($node.Left)" -ceq '$build' -and
    $node.Right -is [Management.Automation.Language.CommandExpressionAst] -and $node.Right.Expression -is [Management.Automation.Language.ExpandableStringExpressionAst] }, $true))
Check ($given.Count -eq 1) "build_release.ps1 makes one build script for the child ($($given.Count))"
if ($given.Count -eq 1) {
    $childScript = & {
        $src = 'D:\rel\src'; $kind = 'arm64'; $buildPreset = 'arm64-publish'; $keyArgument = "'-DQKVM_UPDATE_PUBLIC_KEY=$('a' * 128)'"
        $hostTools = "'-DQKVM_HOST_TOOLS=D:\rel\src\build\publish\bin'"; $versions = 'D:\rel\runs\1\tools-arm64.json'
        & ([scriptblock]::Create($given[0].Right.Extent.Text))
    }
    $errors = $null
    $null = [Management.Automation.Language.Parser]::ParseInput($childScript, [ref] $null, [ref] $errors)
    Check (@($errors).Count -eq 0) "the build script build_release.ps1 gives the child reads as PowerShell ($(@($errors) -join '; '))"
    $built = $childScript.IndexOf("if (`$LASTEXITCODE -ne 0) { throw 'The arm64-publish build failed.' }", [StringComparison]::Ordinal)
    $asked = $childScript.IndexOf(". 'D:\rel\src\tools\build_common.ps1'`nGet-BuildToolVersions | ConvertTo-Json | Set-Content -LiteralPath 'D:\rel\runs\1\tools-arm64.json' -Encoding utf8", [StringComparison]::Ordinal)
    Check ($built -ge 0 -and $asked -gt $built) 'once built, the child asks the tools what they are with Get-BuildToolVersions, and writes their words'
}
Check ($releaseBuild -match '\$release = @\(& wsl\.exe -d Ubuntu -e cat /etc/os-release 2>\$null\)\n\s+if \(\$LASTEXITCODE -ne 0\) \{ throw "WSL''s Ubuntu, which built FFmpeg, did not say what it is') 'the FFmpeg part reads how WSL ended when it asks what Ubuntu built it'
# What built a release, as each tool says it (Get-BuildToolVersions), of stand-ins for Visual Studio's tools: a tool that
# ends in an error, or says nothing of what it is, stops the build; the compiler and the linker, given nothing to do,
# are read by their words alone.
$toolsSaid = [ordered]@{ cl = @('Microsoft (R) C/C++ Optimizing Compiler Version 19.44.35229 for x64', 'usage: cl [ option... ] filename... [ /link linkoption... ]')
    link = @('Microsoft (R) Incremental Linker Version 14.44.35229.0'); cmake = @('cmake version 3.31.6-msvc6', '', 'CMake suite maintained and supported by Kitware')
    ninja = @('1.12.1'); git = @('git version 2.55.0.windows.3'); vswhere = @('17.14.37710.0') }
$toolsEnd = [ordered]@{ cl = 2; link = 1100; cmake = 0; ninja = 0; git = 0; vswhere = 0 }
function Get-StandInVersions([Collections.IDictionary] $Said, [Collections.IDictionary] $Ends) {
    $environment = @{ VCToolsVersion = '14.44.35207'; WindowsSDKVersion = '10.0.26100.0\'; VCToolsRedistDir = 'C:\VS\VC\Redist\MSVC\14.44.35112\'; QKVM_BUILD_ENVIRONMENT = 'C:\VS' }
    $saved = @{}
    foreach ($name in $environment.Keys) { $saved[$name] = [Environment]::GetEnvironmentVariable($name); [Environment]::SetEnvironmentVariable($name, $environment[$name]) }
    try {
        # (Each stand-in says its words and ends as it is told; functions are found before programs of the same name.)
        function cl { $Said.cl; $global:LASTEXITCODE = $Ends.cl }
        function link { $Said.link; $global:LASTEXITCODE = $Ends.link }
        function cmake { $Said.cmake; $global:LASTEXITCODE = $Ends.cmake }
        function ninja { $Said.ninja; $global:LASTEXITCODE = $Ends.ninja }
        function git { $Said.git; $global:LASTEXITCODE = $Ends.git }
        return Get-BuildToolVersions -VsWhere { $Said.vswhere; $global:LASTEXITCODE = $Ends.vswhere }
    } finally {
        # (One that was not there is taken away again: PowerShell would hand .NET an empty text for $null.)
        foreach ($name in $saved.Keys) {
            if ($null -eq $saved[$name]) { Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue } else { [Environment]::SetEnvironmentVariable($name, $saved[$name]) }
        }
        $global:LASTEXITCODE = 0
    }
}
$versions = $null
try { $versions = Get-StandInVersions $toolsSaid $toolsEnd } catch { Write-Host "(Get-BuildToolVersions: $($_.Exception.Message))" }
Check ($null -ne $versions) 'build_common.ps1 asks the tools that built a release what they are, the compiler and the linker whatever their end (Get-BuildToolVersions)'
Check (($versions | ConvertTo-Json -Compress) -ceq (([ordered]@{ visualStudio = '17.14.37710.0'; msvcTools = '14.44.35207'
    compiler = 'Microsoft (R) C/C++ Optimizing Compiler Version 19.44.35229 for x64'; linker = 'Microsoft (R) Incremental Linker Version 14.44.35229.0'
    windowsSdk = '10.0.26100.0'; vcRedist = '14.44.35112'; cmake = 'cmake version 3.31.6-msvc6'; ninja = '1.12.1'; git = 'git version 2.55.0.windows.3'
    powershell = $PSVersionTable.PSVersion.ToString() }) | ConvertTo-Json -Compress)) "each tool is recorded by its own words, the compiler and the linker whatever their end ($($versions | ConvertTo-Json -Compress))"
foreach ($tool in 'vswhere', 'cmake', 'ninja', 'git') {
    $ends = [ordered]@{}; foreach ($one in $toolsEnd.GetEnumerator()) { $ends[$one.Key] = $one.Value }
    $ends[$tool] = 1
    Refuses { Get-StandInVersions $toolsSaid $ends } "\A$tool ended in an error \(exit 1\) when asked what it is: the build record would not say what built this\.\z" "a $tool that ends in an error stops the build"
}
foreach ($tool in 'vswhere', 'cl', 'link', 'cmake', 'ninja', 'git') {
    $said = [ordered]@{}; foreach ($one in $toolsSaid.GetEnumerator()) { $said[$one.Key] = $one.Value }
    $said[$tool] = @(if ($tool -cin 'cl', 'link') { 'usage: it, with nothing else' })
    Refuses { Get-StandInVersions $said $toolsEnd } "\A$tool said nothing of what it is: the build record would not say what built this\.\z" "a $tool that says nothing of what it is stops the build"
}
# Inno Setup copied out of its low folder (fetch_inno.ps1): each file and folder held to its label as icacls says it,
# and an icacls that ends in an error stops the unpacking, since an item it could not read is not known to be other than
# low (outside review 2, 5.1). A stand-in for icacls here, and Windows' own icacls reading a folder made here.
$lowWork = Join-Path $work 'low'
New-Item -ItemType Directory -Force -Path (Join-Path $lowWork 'sub') | Out-Null
foreach ($name in 'ISCC.exe', 'Setup.e32', 'sub\Default.isl') { [IO.File]::WriteAllText((Join-Path $lowWork $name), 'made-up') }
function Get-StandInLow([hashtable] $Labels, [hashtable] $Ends = @{}) {
    # (Each item's lines as icacls says them, its label among them where $Labels names one; its end as $Ends says.)
    $standIn = {
        param([string] $Path)
        $leaf = Split-Path -Leaf $Path
        "$Path NT AUTHORITY\SYSTEM:(I)(F)"
        if ($Labels.ContainsKey($leaf)) { "    $($Labels[$leaf])" }
        $global:LASTEXITCODE = $(if ($Ends.ContainsKey($leaf)) { $Ends[$leaf] } else { 0 })
    }
    return @(Get-LowIntegrityItems -Folder $lowWork -Icacls $standIn)
}
if (-not (Get-Command Get-LowIntegrityItems -ErrorAction SilentlyContinue)) {
    Check $false 'build_common.ps1 says which files and folders are marked low integrity, reading how icacls ends (Get-LowIntegrityItems)'
} else {
    $none = @(Get-StandInLow @{ 'ISCC.exe' = 'Mandatory Label\Medium Mandatory Level:(NW)' })
    Check ($none.Count -eq 0) "nothing is said low that icacls labels otherwise ($($none -join ', '))"
    $one = @(Get-StandInLow @{ 'Default.isl' = 'Mandatory Label\Low Mandatory Level:(NW)' })
    Check ($one.Count -eq 1 -and $one[0] -ceq [IO.Path]::GetFullPath((Join-Path $lowWork 'sub\Default.isl'))) "a file icacls labels low is said, by its path ($($one -join ', '))"
    Refuses { Get-StandInLow @{} @{ 'Setup.e32' = 5 } } '\Aicacls could not say the integrity of .*Setup\.e32 \(icacls exit 5' 'an icacls that ends in an error stops the unpacking'
    $real = @(Get-LowIntegrityItems -Folder $lowWork)
    Check ($real.Count -eq 0) "Windows' icacls says no file made here is low ($($real -join ', '))"
}
Check ([IO.File]::ReadAllText((Join-Path $PSScriptRoot 'fetch_inno.ps1')).Contains('$low = @(Get-LowIntegrityItems -Folder $Root -Icacls $icacls)')) 'fetch_inno.ps1 holds Inno Setup, copied out, to its labels with Get-LowIntegrityItems'

# --- The publish preset as committed here.
$preset = Get-PublishPreset $desktop
Check ($preset.Feed -ceq $PublicUpdateFeed) 'the publish preset asks the public feed'
Check (-not $preset.PublicKey -or (Test-ReleasePublicKey $preset.PublicKey)) 'the publish preset holds no key yet, or one that can be pinned'
Check ((Get-ProjectVersion $desktop) -match '^\d+\.\d+\.\d+$') 'the tree says its version in three numbers'

# --- What build_release.ps1 refuses before it begins.
$git = (Get-Command git -CommandType Application | Select-Object -First 1).Source
function New-FixtureExport([string] $Name, [string] $Key = '', [string] $Feed = $PublicUpdateFeed, [string[]] $Extra = @(), [string] $Inherits = 'release',
    [string[]] $ArmInherits = @('publish', 'arm64'), [switch] $ArmCache) {
    $folder = Join-Path $work $Name
    New-Item -ItemType Directory -Path "$folder/tools" | Out-Null
    [IO.File]::WriteAllText("$folder/CMakeLists.txt", "cmake_minimum_required(VERSION 3.25)`nproject(QkvmDesktop VERSION 0.1.0 LANGUAGES CXX)`n")
    $presets = [ordered]@{ version = 6; configurePresets = @(
        [ordered]@{ name = 'release'; cacheVariables = @{ CMAKE_BUILD_TYPE = 'Release' } }
        [ordered]@{ name = 'publish'; inherits = $Inherits; cacheVariables = [ordered]@{ QKVM_UPDATE_FEED = $Feed; QKVM_UPDATE_PUBLIC_KEY = $Key } }
        [ordered]@{ name = 'arm64'; hidden = $true; cacheVariables = @{ QKVM_ARCH = 'arm64' } }
        [ordered]@{ name = 'arm64-publish'; inherits = $ArmInherits }
    ) }
    if ($ArmCache) { $presets.configurePresets[-1]['cacheVariables'] = @{ QKVM_UPDATE_FEED = 'https://example.com/stable.json' } }
    [IO.File]::WriteAllText("$folder/CMakePresets.json", ($presets | ConvertTo-Json -Depth 5))
    foreach ($file in @('tools/build_release.ps1', 'tools/package.ps1') + $Extra) {
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent "$folder/$file") | Out-Null
        [IO.File]::WriteAllText("$folder/$file", "# made-up`n")
    }
    & $git -C $folder init -q 2>&1 | Out-Null
    & $git -C $folder add -A 2>&1 | Out-Null
    & $git -C $folder -c user.name=fixture -c user.email=fixture@example.invalid -c core.autocrlf=false commit -q -m fixture 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "The made-up export $Name could not be committed." }
    return $folder
}
function Invoke-Check([string[]] $Arguments) {
    $said = & (Get-Process -Id $PID).Path -NoProfile -NonInteractive -File "$PSScriptRoot/build_release.ps1" @Arguments -CheckOnly 2>&1
    $ended = $LASTEXITCODE
    # (An error is drawn in colours and wrapped to the window, each further line after a "|": read as one line of words.)
    $text = [regex]::Replace((($said | ForEach-Object { "$_" }) -join "`n"), '\x1b\[[0-9;]*[A-Za-z]', '')
    return [pscustomobject]@{ Ended = $ended; Said = [regex]::Replace($text, '\r?\n\s*\|\s*', ' ') }
}
function Expect-Build([string[]] $Arguments, [string] $Expected, [string] $Message) {
    $result = Invoke-Check $Arguments
    Check ($result.Ended -eq 0 -and $result.Said -match $Expected) "$Message (exit $($result.Ended): $($result.Said))"
}
function Expect-Refusal([string[]] $Arguments, [string] $Expected, [string] $Message) {
    $result = Invoke-Check $Arguments
    Check ($result.Ended -ne 0 -and $result.Said -match $Expected) "$Message (exit $($result.Ended): $($result.Said))"
}


# --- The FFmpeg part of a release (build_release.ps1 -Part ffmpeg, on a PC with WSL) and what the build part takes of it
# (-Part build, on the PC that builds the release): its record names the commit and tree, the pins of each kind's
# sources, toolchain and scripts, and every file of each kind's FFmpeg, each DLL among them, by size and SHA-256; the
# build part takes exactly that, of its own commit, or nothing.
function New-FFmpegPart([string] $Name, [string] $Commit = ('1' * 40), [string] $Tree = ('2' * 40), [string] $Toolchain = 'made-up-compiler_1.0_amd64.deb') {
    $part = Join-Path $work "ffmpeg-part-$Name"
    New-Item -ItemType Directory -Path "$part/sources" | Out-Null
    $archives = [ordered]@{}
    foreach ($archive in 'ffmpeg-9.0.2.tar.xz', 'zlib-1.3.2.tar.gz') {
        [IO.File]::WriteAllText("$part/sources/$archive", "made-up source of $archive")
        $archives[$archive] = [ordered]@{ archive = $archive; bytes = (Get-Item -LiteralPath "$part/sources/$archive").Length; sha256 = Get-Sha256 "$part/sources/$archive" }
    }
    foreach ($kind in 'x64', 'arm64') {
        foreach ($file in 'bin/avcodec-63.dll', 'bin/avutil-61.dll', 'def/avcodec-63.def', 'def/avutil-61.def', 'include/libavcodec/avcodec.h', 'LICENSE.txt') {
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent "$part/$kind/$file") | Out-Null
            [IO.File]::WriteAllText("$part/$kind/$file", "made-up $file for $kind")
        }
        [ordered]@{ ffmpeg = $archives['ffmpeg-9.0.2.tar.xz']; zlib = $archives['zlib-1.3.2.tar.gz']
            toolchain = @([ordered]@{ package = "$kind-$Toolchain"; bytes = 1234; sha256 = 'a' * 64 }); configuration = '--prefix=/ffmpeg' } |
            ConvertTo-Json -Depth 4 | Set-Content -LiteralPath "$part/$kind/build.json" -Encoding utf8
    }
    $made = New-FFmpegRecord -Part $part -Version '0.1.0' -Commit $Commit -Tree $Tree -Scripts @("build_ffmpeg.ps1 $('b' * 64)", "build_ffmpeg.sh $('c' * 64)") -Wsl 'made-up'
    $made | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $part $FFmpegRecordName) -Encoding utf8
    return $part
}
function Read-Record([string] $Part) { return Get-Content -LiteralPath (Join-Path $Part $FFmpegRecordName) -Raw | ConvertFrom-Json }
function Save-Record([string] $Part, $Record) { $Record | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $Part $FFmpegRecordName) -Encoding utf8 }
function Copy-Part([string] $From, [string] $Name) {
    $to = Join-Path $work "ffmpeg-part-$Name"
    Copy-Item -LiteralPath $From -Destination $to -Recurse
    return $to
}
$c1 = '1' * 40
$t1 = '2' * 40
$part = New-FFmpegPart 'good'
$record = Read-Record $part
Check ($record.source.commit -ceq $c1 -and $record.source.tree -ceq $t1 -and $record.part -ceq 'ffmpeg') 'the FFmpeg part''s record names the commit and the tree it was built from'
Check ((@($record.builds | ForEach-Object architecture) -join ',') -ceq 'x64,arm64') 'it holds a build of each kind'
$x64 = @($record.builds | Where-Object architecture -ceq 'x64')[0]
Check ((@($x64.dlls | ForEach-Object name) -join ',') -ceq 'bin/avcodec-63.dll,bin/avutil-61.dll') 'it names each DLL'
$avcodec = Get-Item -LiteralPath "$part/x64/bin/avcodec-63.dll"
Check (@($x64.dlls | Where-Object { $_.name -ceq 'bin/avcodec-63.dll' -and $_.bytes -eq $avcodec.Length -and
    $_.sha256 -ceq (Get-FileHash -LiteralPath $avcodec.FullName).Hash.ToLowerInvariant() }).Count -eq 1) 'each DLL by its size and SHA-256'
Check (@($x64.files).Count -eq 7 -and @($x64.files | Where-Object path -ceq 'build.json').Count -eq 1) 'and every file of the build, its build.json too'
Check ($x64.pins.ffmpeg -cmatch '^ffmpeg-9\.0\.2\.tar\.xz \d+ [0-9a-f]{64}$' -and @($x64.pins.toolchain)[0] -cmatch '^x64-made-up-compiler_1\.0_amd64\.deb 1234 a{64}$' -and
    @($x64.pins.scripts).Count -eq 2) 'and the pins of its sources, its toolchain and the scripts that build it'
Check ($x64.pinsDigest -ceq (Get-FFmpegPinsDigest $x64.pins 'x64') -and $x64.pinsDigest -cne @($record.builds)[1].pinsDigest) 'one digest for each kind''s pins'
Check ((@($record.sources | ForEach-Object archive) -join ',') -ceq 'ffmpeg-9.0.2.tar.xz,zlib-1.3.2.tar.gz') 'and the source archives'
$said = Get-FFmpegPartRefusals -Folder $part -Commit $c1 -Tree $t1
Check (@($said.Refusals).Count -eq 0) "the FFmpeg part as it was made is taken ($(@($said.Refusals) -join '; '))"

function Expect-PartRefused([string] $Folder, [string] $Expected, [string] $Message, [string] $Commit = $c1, [string] $Tree = $t1) {
    $refusals = @((Get-FFmpegPartRefusals -Folder $Folder -Commit $Commit -Tree $Tree).Refusals)
    Check (@($refusals -match $Expected).Count -ge 1) "$Message ($($refusals -join '; '))"
}
Expect-PartRefused $part 'another commit' 'an FFmpeg part of another commit is refused' -Commit ('3' * 40)
Expect-PartRefused $part 'another tree' 'an FFmpeg part of another tree is refused' -Tree ('4' * 40)
$one = Copy-Part $part 'changed-dll'
[IO.File]::AppendAllText("$one/arm64/bin/avcodec-63.dll", 'x')
Expect-PartRefused $one '^arm64: bin/avcodec-63\.dll is not the file the record names' 'a DLL of another size is refused'
$one = Copy-Part $part 'same-size'
[IO.File]::WriteAllText("$one/x64/bin/avutil-61.dll", 'made-up bin/avutil-61.dll for x65')
Expect-PartRefused $one '^x64: bin/avutil-61\.dll is not the file the record names' 'a DLL of the same size and another SHA-256 is refused'
$one = Copy-Part $part 'extra-dll'
[IO.File]::WriteAllText("$one/x64/bin/swresample-6.dll", 'not made by the part')
Expect-PartRefused $one '^x64: bin/swresample-6\.dll is not in the record' 'a DLL that is not in the record is refused'
$one = Copy-Part $part 'missing-dll'
Remove-Item -LiteralPath "$one/x64/bin/avutil-61.dll"
Expect-PartRefused $one '^x64: bin/avutil-61\.dll, which the record names, is not there' 'a DLL the record names that is not there is refused'
$one = Copy-Part $part 'dll-unnamed'
$changed = Read-Record $one
$changed.builds[1].dlls = @($changed.builds[1].dlls[0])
Save-Record $one $changed
Expect-PartRefused $one '^arm64: the record holds a DLL among its files that it does not name' 'a record that names a DLL among its files and not as a DLL is refused'
$one = Copy-Part $part 'dll-edited'
$changed = Read-Record $one
$changed.builds[0].dlls[0].sha256 = 'd' * 64
Save-Record $one $changed
Expect-PartRefused $one '^x64: the record''s bin/avcodec-63\.dll is not among its files as it says' 'a record whose DLL is not its file is refused'
$one = Copy-Part $part 'one-kind'
$changed = Read-Record $one
$changed.builds = @($changed.builds[0])
Save-Record $one $changed
Expect-PartRefused $one 'not of one build for each kind of Windows' 'a record of one kind alone is refused'
$one = Copy-Part $part 'twice-x64'
$changed = Read-Record $one
$changed.builds = @($changed.builds[0], $changed.builds[0])
Save-Record $one $changed
Expect-PartRefused $one 'not of one build for each kind of Windows' 'a record of x64 twice is refused'
$one = Copy-Part $part 'pins-edited'
$changed = Read-Record $one
$changed.builds[1].pins.toolchain = @("arm64-other-compiler_2.0_amd64.deb 1234 $('a' * 64)")
Save-Record $one $changed
Expect-PartRefused $one '^arm64: its pins are not the ones its digest says' 'a record whose pins are not its digest''s is refused'
$one = Copy-Part $part 'source-changed'
[IO.File]::AppendAllText("$one/sources/zlib-1.3.2.tar.gz", 'x')
Expect-PartRefused $one '^sources: zlib-1\.3\.2\.tar\.gz is not the file the record names' 'a source archive of other bytes is refused'
$one = Copy-Part $part 'source-of-another'
$changed = Read-Record $one
$changed.builds[0].pins.ffmpeg = "ffmpeg-9.0.1.tar.xz 1 $('e' * 64)"
$changed.builds[0].pinsDigest = Get-FFmpegPinsDigest $changed.builds[0].pins 'x64'
Save-Record $one $changed
Expect-PartRefused $one '^x64: its source ffmpeg-9\.0\.1\.tar\.xz is not among the record''s sources' 'a build of sources the part does not hold is refused'
$one = Copy-Part $part 'stray'
[IO.File]::WriteAllText("$one/notes.txt", 'left here')
Expect-PartRefused $one '^notes\.txt is not in the record' 'anything beside the record, the two kinds and the sources is refused'
$one = Copy-Part $part 'no-record'
Remove-Item -LiteralPath (Join-Path $one $FFmpegRecordName)
Expect-PartRefused $one 'there is no ffmpeg-record\.json' 'a part without its record is refused'
$one = Copy-Part $part 'not-json'
[IO.File]::WriteAllText((Join-Path $one $FFmpegRecordName), 'not JSON')
Expect-PartRefused $one 'is no JSON' 'a record that is no JSON is refused'
$one = Copy-Part $part 'not-ffmpeg'
$changed = Read-Record $one
$changed.part = 'build'
Save-Record $one $changed
Expect-PartRefused $one 'not the record of an FFmpeg part' 'a record of something else is refused'

# --- A fresh build of the same pins is the same bytes as every earlier one, or the release stops: each kind is compared
# with every earlier build of its kind and of the same pins (their digest, worked out anew), and with no other.
$again = Read-Record (New-FFmpegPart 'again')
$said = Get-FFmpegReproductionRefusals -Record $again -Earlier @([pscustomobject]@{ Name = 'first.json'; Record = $record })
Check (@($said.Refusals).Count -eq 0 -and (@($said.Matched) -join ',') -ceq 'first.json (x64),first.json (arm64)') "a build of the same pins that is byte for byte the earlier one is taken, of each kind ($(@($said.Refusals) -join '; '))"
$one = New-FFmpegPart 'differs'
[IO.File]::AppendAllText("$one/arm64/bin/avcodec-63.dll", 'x')
$differs = New-FFmpegRecord -Part $one -Version '0.1.0' -Commit $c1 -Tree $t1 -Scripts @("build_ffmpeg.ps1 $('b' * 64)", "build_ffmpeg.sh $('c' * 64)")
$said = Get-FFmpegReproductionRefusals -Record $differs -Earlier @([pscustomobject]@{ Name = 'first.json'; Record = $record })
Check (@($said.Refusals).Count -eq 1 -and @($said.Refusals)[0] -cmatch '^arm64: not byte for byte the build of the same pins recorded in first\.json \(bin/avcodec-63\.dll differ\)$' -and
    (@($said.Matched) -join ',') -ceq 'first.json (x64)') "a DLL of other bytes from the same pins stops the release, and says which ($(@($said.Refusals) -join '; '))"
$one = New-FFmpegPart 'fewer'
Remove-Item -LiteralPath "$one/x64/def/avutil-61.def"
$fewer = New-FFmpegRecord -Part $one -Version '0.1.0' -Commit $c1 -Tree $t1 -Scripts @("build_ffmpeg.ps1 $('b' * 64)", "build_ffmpeg.sh $('c' * 64)")
$said = Get-FFmpegReproductionRefusals -Record $fewer -Earlier @([pscustomobject]@{ Name = 'first.json'; Record = $record })
Check (@($said.Refusals -match '^x64: not byte for byte .*def/avutil-61\.def').Count -eq 1) 'a build of the same pins with a file fewer stops the release'
$one = New-FFmpegPart 'other-pins' -Toolchain 'other-compiler_2.0_amd64.deb'
[IO.File]::AppendAllText("$one/x64/bin/avcodec-63.dll", 'x')
$other = New-FFmpegRecord -Part $one -Version '0.1.0' -Commit $c1 -Tree $t1 -Scripts @("build_ffmpeg.ps1 $('b' * 64)", "build_ffmpeg.sh $('c' * 64)")
$said = Get-FFmpegReproductionRefusals -Record $other -Earlier @([pscustomobject]@{ Name = 'first.json'; Record = $record })
Check (@($said.Refusals).Count -eq 0 -and @($said.Matched).Count -eq 0) 'a build of other pins is not compared'
$otherScript = New-FFmpegRecord -Part $one -Version '0.1.0' -Commit $c1 -Tree $t1 -Scripts @("build_ffmpeg.ps1 $('b' * 64)", "build_ffmpeg.sh $('f' * 64)")
Check ($otherScript.builds[0].pinsDigest -cne $other.builds[0].pinsDigest) 'another script that builds it is other pins'
$stated = Read-Record (New-FFmpegPart 'stated-digest')
$stated.builds[0].pinsDigest = $other.builds[0].pinsDigest
$said = Get-FFmpegReproductionRefusals -Record $differs -Earlier @([pscustomobject]@{ Name = 'stated.json'; Record = $stated })
Check (@($said.Refusals).Count -eq 1 -and @($said.Matched).Count -eq 1) 'an earlier record is matched by the digest of its pins worked out anew, not by the one it states'
$said = Get-FFmpegReproductionRefusals -Record $again -Earlier @([pscustomobject]@{ Name = 'a.json'; Record = $record }, [pscustomobject]@{ Name = 'b.json'; Record = (Read-Record (New-FFmpegPart 'b')) })
Check (@($said.Matched).Count -eq 4) 'every earlier record of the same pins is compared'

# --- x64's FFmpeg of 0.1.2's sources and toolchain is the FFmpeg 0.1.2's release published, byte for byte: its two DLLs
# by size and SHA-256 (build_common.ps1's $FFmpegPublished, read out of 0.1.2's x64 ZIP), or the release stops, saying
# which DLL differs (review r2 of arm and armp, L4). Its scripts are not compared, and a build of other sources or
# another toolchain is not compared with 0.1.2's at all.
function Set-X64($Record, [string[]] $Toolchain = @($FFmpegPublished.toolchain), [hashtable] $Dlls = @{}) {
    <# The record's x64 build made of 0.1.2's sources and $Toolchain, with 0.1.2's DLLs but for $Dlls (a name to
    "<bytes> <sha256>", or to $null for none). #>
    $x64 = @($Record.builds | Where-Object architecture -ceq 'x64')[0]
    $x64.pins.ffmpeg = $FFmpegPublished.ffmpeg
    $x64.pins.zlib = $FFmpegPublished.zlib
    $x64.pins.toolchain = @($Toolchain)
    $made = [ordered]@{}
    foreach ($name in $FFmpegPublished.dlls.Keys) { $made[$name] = $FFmpegPublished.dlls[$name] }
    foreach ($name in $Dlls.Keys) { if ($null -eq $Dlls[$name]) { $made.Remove($name) } else { $made[$name] = $Dlls[$name] } }
    $x64.dlls = @($made.Keys | ForEach-Object { [pscustomobject]@{ name = "bin/$_"; bytes = [long] ($made[$_] -split ' ')[0]; sha256 = ($made[$_] -split ' ')[1] } })
    return $Record
}
$as012 = Set-X64 (Read-Record $part)
$said = Get-FFmpegPublishedRefusals -Record $as012
Check ($said.Applies -and @($said.Refusals).Count -eq 0) "x64's FFmpeg of 0.1.2's sources and toolchain, with 0.1.2's DLLs, is taken ($(@($said.Refusals) -join '; '))"
$said = Get-FFmpegPublishedRefusals -Record (Set-X64 (Read-Record $part) -Toolchain @(@($FFmpegPublished.toolchain)[-1..-10]))
Check ($said.Applies -and @($said.Refusals).Count -eq 0) "its toolchain named in another order is the same toolchain ($(@($said.Refusals) -join '; '))"
$said = Get-FFmpegPublishedRefusals -Record (Set-X64 (Read-Record $part) -Dlls @{ 'avcodec-63.dll' = "2177024 $('0' * 64)" })
Check ($said.Applies -and @($said.Refusals).Count -eq 1 -and @($said.Refusals)[0] -match '\Ax64: avcodec-63\.dll is not the one 0\.1\.2''s release published from the same sources and toolchain') "an avcodec of other bytes than 0.1.2's is refused, by its name ($(@($said.Refusals) -join '; '))"
$said = Get-FFmpegPublishedRefusals -Record (Set-X64 (Read-Record $part) -Dlls @{ 'avutil-61.dll' = "1040897 $('605e96998a3570e1d7547eada6ab94d81a1777a0145fccf59000d0950bfb16fe')" })
Check (@($said.Refusals).Count -eq 1 -and @($said.Refusals)[0] -match '\Ax64: avutil-61\.dll is not the one') "an avutil of another size is refused ($(@($said.Refusals) -join '; '))"
$said = Get-FFmpegPublishedRefusals -Record (Set-X64 (Read-Record $part) -Dlls @{ 'avutil-61.dll' = $null })
Check (@($said.Refusals).Count -eq 1 -and @($said.Refusals)[0] -match '\Ax64: avutil-61\.dll, which 0\.1\.2''s release published from the same sources and toolchain, was not made') "a build that lacks one of 0.1.2's DLLs is refused ($(@($said.Refusals) -join '; '))"
$said = Get-FFmpegPublishedRefusals -Record (Set-X64 (Read-Record $part) -Dlls @{ 'swresample-6.dll' = "1 $('1' * 64)" })
Check (@($said.Refusals).Count -eq 1 -and @($said.Refusals)[0] -match '\Ax64: swresample-6\.dll was made, and 0\.1\.2''s release published none') "a build that makes a DLL more than 0.1.2's is refused ($(@($said.Refusals) -join '; '))"
$said = Get-FFmpegPublishedRefusals -Record (Set-X64 (Read-Record $part) -Toolchain (@($FFmpegPublished.toolchain)[0..8] + "make_4.4_amd64.deb 1 $('2' * 64)") -Dlls @{ 'avcodec-63.dll' = "1 $('0' * 64)" })
Check (-not $said.Applies -and @($said.Refusals).Count -eq 0) "a build of another toolchain is not compared with 0.1.2's ($(@($said.Refusals) -join '; '))"
$said = Get-FFmpegPublishedRefusals -Record (Read-Record $part)
Check (-not $said.Applies -and @($said.Refusals).Count -eq 0) 'nor one of other sources and another toolchain'
foreach ($source in 'ffmpeg', 'zlib') {
    $otherSource = Set-X64 (Read-Record $part) -Dlls @{ 'avcodec-63.dll' = "1 $('0' * 64)" }
    @($otherSource.builds | Where-Object architecture -ceq 'x64')[0].pins.$source = "$source-9.9.9.tar.xz 1 $('3' * 64)"
    $said = Get-FFmpegPublishedRefusals -Record $otherSource
    Check (-not $said.Applies -and @($said.Refusals).Count -eq 0) "nor one of another $source archive with 0.1.2's toolchain ($(@($said.Refusals) -join '; '))"
}
# (Only x64's: Windows on Arm's FFmpeg was first built for 0.2.0.)
$armOf012 = Read-Record $part
$arm = @($armOf012.builds | Where-Object architecture -ceq 'arm64')[0]
$arm.pins.ffmpeg = $FFmpegPublished.ffmpeg; $arm.pins.zlib = $FFmpegPublished.zlib; $arm.pins.toolchain = @($FFmpegPublished.toolchain)
Check (-not (Get-FFmpegPublishedRefusals -Record $armOf012).Applies) 'an ARM64 build is not compared with 0.1.2''s x64 DLLs'
# (Where the reproduction is checked, in the FFmpeg part and against this PC's kept records in release_commands.ps1.)
$said = Get-FFmpegReproductionRefusals -Record (Set-X64 (Read-Record $part) -Dlls @{ 'avcodec-63.dll' = "2177024 $('0' * 64)" })
Check (@($said.Refusals -match '\Ax64: avcodec-63\.dll is not the one 0\.1\.2''s release published').Count -eq 1 -and -not $said.Published) "the reproduction check holds x64 to 0.1.2's published DLLs ($(@($said.Refusals) -join '; '))"
$said = Get-FFmpegReproductionRefusals -Record (Set-X64 (Read-Record $part))
Check (@($said.Refusals).Count -eq 0 -and $said.Published) "and says so where they are the same ($(@($said.Refusals) -join '; '))"
# (The pins held are today's tools/build_ffmpeg.ps1's for x64: 0.2.0's x64 FFmpeg is compared with 0.1.2's.)
$ffmpegScript = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'build_ffmpeg.ps1'))
$pinned = @([regex]::Matches($ffmpegScript, "Name = '([^']+)'[^}]*?Bytes = (\d+)[^}]*?Sha256 = '([0-9a-f]{64})'", 'Singleline') | ForEach-Object { "$($_.Groups[1].Value) $($_.Groups[2].Value) $($_.Groups[3].Value)" })
$x64Pinned = @($pinned | Where-Object { $_ -notlike 'llvm-mingw-*' })
Check ($x64Pinned.Count -eq 12 -and $x64Pinned[0] -ceq $FFmpegPublished.ffmpeg -and $x64Pinned[1] -ceq $FFmpegPublished.zlib -and
    ((@($x64Pinned[2..11]) | Sort-Object -CaseSensitive) -join '|') -ceq ((@($FFmpegPublished.toolchain) | Sort-Object -CaseSensitive) -join '|')) "tools/build_ffmpeg.ps1 builds x64 from 0.1.2's sources and toolchain, so 0.2.0's x64 FFmpeg is held to 0.1.2's DLLs ($($x64Pinned.Count) pins)"
Check ($releaseBuild.Contains("Write-Output `$(if (`$reproduced.Published) { `"x64's FFmpeg DLLs are byte for byte the ones")) 'the FFmpeg part says whether x64''s DLLs are 0.1.2''s published ones'

# --- The FFmpeg DLLs a package holds, against the part's record (audit_release.ps1 -FFmpegRecord).
$dllsOf = { param($kind) @(Get-ChildItem -LiteralPath "$part/$kind/bin" -File | ForEach-Object { [pscustomobject]@{ path = $_.Name; bytes = $_.Length; sha256 = Get-Sha256 $_.FullName } }) }
Check (@(Get-FFmpegDllRefusals -Record $record -Kind 'x64' -Dlls (& $dllsOf 'x64')).Count -eq 0) 'a package of the DLLs the part made for its kind passes'
Check (@(Get-FFmpegDllRefusals -Record $record -Kind 'x64' -Dlls (& $dllsOf 'arm64')) -match 'avcodec-63\.dll is not the DLL the FFmpeg part made for x64').Count -eq 1 'another kind''s DLLs are refused'
Check (@(Get-FFmpegDllRefusals -Record $record -Kind 'x64' -Dlls @((& $dllsOf 'x64')[0])) -match 'avutil-61\.dll, which the FFmpeg part made for x64, is not in the package').Count -eq 1 'a DLL fewer is refused'
$more = @(& $dllsOf 'x64') + [pscustomobject]@{ path = 'swresample-6.dll'; bytes = 1; sha256 = 'f' * 64 }
Check (@(Get-FFmpegDllRefusals -Record $record -Kind 'x64' -Dlls $more) -match '^swresample-6\.dll is not a DLL the FFmpeg part made for x64$').Count -eq 1 'a DLL the part did not make is refused'
Check (@(Get-FFmpegDllRefusals -Record $null -Kind 'x64' -Dlls (& $dllsOf 'x64')) -match 'no record').Count -eq 1 'no record is refused'

# --- What build_release.ps1 refuses before it begins.
$parts = @{}
function Get-PartFor([string] $Export) {
    # A made-up FFmpeg part of the export's own commit and tree, the second build of its pins: it reproduced the first,
    # for each kind, as the release's must (its record says so).
    $at = (& $git -C $Export rev-parse HEAD).Trim()
    if (-not $parts.ContainsKey($at)) {
        $made = New-FFmpegPart "for-$($at.Substring(0, 12))" -Commit $at -Tree (& $git -C $Export rev-parse 'HEAD^{tree}').Trim()
        $again = Read-Record $made
        $again.reproduces = @('ffmpeg-0.1.0-first.json (x64)', 'ffmpeg-0.1.0-first.json (arm64)')
        Save-Record $made $again
        $parts[$at] = $made
    }
    return $parts[$at]
}
function Build-Part([string] $Export) { return @('-Part', 'build', '-FFmpeg', (Get-PartFor $Export)) }

$good = New-FixtureExport 'good' -Key $key
$commit = (& $git -C $good rev-parse HEAD).Trim()
$newRoot = Join-Path $work 'root-new'
$records = Join-Path $work 'ffmpeg-records'
Expect-Build (@('-Export', $good, '-Root', $newRoot) + (Build-Part $good)) "Would build qKVM Desktop 0\.1\.0 for x64 and arm64 from $commit in .*, with the FFmpeg part in " 'an export with a committed key is built, of both kinds, with the FFmpeg part of its commit'
Check (-not (Test-Path -LiteralPath $newRoot)) 'asking leaves no build folder behind'
Expect-Refusal @('-Export', $good, '-Root', $newRoot) 'Part' 'nothing is built without saying which part'
Expect-Refusal @('-Part', 'build', '-Export', $good, '-Root', $newRoot) 'name its folder with -FFmpeg' 'the build part takes FFmpeg from the FFmpeg part alone'
Expect-Refusal (@('-Export', $good, '-Root', $newRoot, '-Part', 'build', '-FFmpeg', $part)) 'is not taken: the record is of another commit' 'the build part refuses an FFmpeg part of another commit'
$tampered = Copy-Part (Get-PartFor $good) 'good-tampered'
[IO.File]::AppendAllText("$tampered/arm64/bin/avutil-61.dll", 'x')
Expect-Refusal (@('-Export', $good, '-Root', $newRoot, '-Part', 'build', '-FFmpeg', $tampered)) 'arm64: bin/avutil-61\.dll is not the file the record names' 'the build part refuses an FFmpeg part with a DLL of another hash'
$extra = Copy-Part (Get-PartFor $good) 'good-extra'
[IO.File]::WriteAllText("$extra/x64/bin/avformat-63.dll", 'x')
Expect-Refusal (@('-Export', $good, '-Root', $newRoot, '-Part', 'build', '-FFmpeg', $extra)) 'x64: bin/avformat-63\.dll is not in the record' 'the build part refuses an FFmpeg DLL that is not in the record'
Expect-Refusal (@('-Export', $good, '-Root', $newRoot, '-Records', $records) + (Build-Part $good)) '-Records is the FFmpeg part''s' 'the build part keeps no records'
# (The first build of its pins, for a kind: the release builds FFmpeg twice, and takes the second, which reproduced the
# first byte for byte, review armp-1 M1. A trial takes any part of its commit: it is never handed out.)
$first = Copy-Part (Get-PartFor $good) 'good-first'
$once = Read-Record $first
$once.reproduces = @()
Save-Record $first $once
Expect-Refusal (@('-Export', $good, '-Root', $newRoot, '-Part', 'build', '-FFmpeg', $first)) 'reproduces no earlier build of its pins for x64, arm64' 'the build part refuses the first build of FFmpeg''s pins'
$half = Copy-Part (Get-PartFor $good) 'good-half'
$once = Read-Record $half
$once.reproduces = @('ffmpeg-0.1.0-first.json (x64)', 'ffmpeg-0.1.0-first.json (x64-other)')
Save-Record $half $once
Expect-Refusal (@('-Export', $good, '-Root', $newRoot, '-Part', 'build', '-FFmpeg', $half)) 'reproduces no earlier build of its pins for arm64:' 'the build part refuses a part that reproduced an earlier build of one kind alone'
Expect-Build (@('-Export', $good, '-Root', $newRoot, '-Part', 'build', '-FFmpeg', $first, '-Trial')) 'Would build qKVM Desktop 0\.1\.0 for x64 and arm64 .*\(TRIAL\)' 'a trial takes the first build of FFmpeg''s pins'
$inSrc = Join-Path $work 'root-with-part'
New-Item -ItemType Directory -Path "$inSrc/src" | Out-Null
[IO.File]::WriteAllText("$inSrc/.qkvm-release-root", 'made by build_release.ps1')
Copy-Item -LiteralPath (Get-PartFor $good) -Destination "$inSrc/src/part" -Recurse
Expect-Refusal @('-Export', $good, '-Root', $inSrc, '-Part', 'build', '-FFmpeg', "$inSrc/src/part") 'which every run makes anew' 'an FFmpeg part where the run makes its tree anew is refused'
Expect-Refusal (@('-Export', $good, '-Root', (Join-Path $work 'root,with-comma')) + (Build-Part $good)) 'holds a comma' 'a build folder with a comma in its path is refused'

# The FFmpeg part: of both kinds, with no key, against the earlier records, which are its records and nothing else.
Expect-Build @('-Part', 'ffmpeg', '-Export', $good, '-Root', $newRoot, '-Records', $records) "Would build FFmpeg for x64 and arm64 of qKVM Desktop 0\.1\.0 from $commit in .*, against 0 earlier record" 'the FFmpeg part is built for both kinds'
Check (-not (Test-Path -LiteralPath $records)) 'asking makes no folder of records'
# (Its records' folder unless told another: beside the build folder, in the folder of the owner's release roots, which
# are kept there and never cleaned (the coordinator, 2026-11-22); never inside the build folder.)
$beside = Join-Path (Split-Path -Parent $newRoot) 'qkvm-desktop-records\ffmpeg'
Check ((Get-FFmpegRecordsFolder $newRoot) -ceq $beside -and (Get-FFmpegRecordsFolder "$newRoot\") -ceq $beside) "the FFmpeg part's records are kept beside its build folder unless told another ($(Get-FFmpegRecordsFolder $newRoot))"
Expect-Build @('-Part', 'ffmpeg', '-Export', $good, '-Root', $newRoot) "against 0 earlier record\(s\) in $([regex]::Escape($beside))\." 'the FFmpeg part keeps its record for the next release beside its build folder, unless told another'
Check (-not (Test-Path -LiteralPath $beside)) 'asking makes no folder of records there'
Expect-Refusal (@('-Export', $good, '-Root', $newRoot, '-Records', $beside) + (Build-Part $good)) '-Records is the FFmpeg part''s' 'the build part keeps no records, its default folder named neither'
Check ($releaseBuild.Contains('desktop-notes/releases/$version/ffmpeg-record.json')) 'the FFmpeg part says that its record goes into the private repository with the release'
Expect-Refusal @('-Part', 'ffmpeg', '-Export', $good, '-Root', $newRoot, '-Records', $records, '-Trial') '-Trial is the build part''s' 'a trial is the build part''s'
Expect-Refusal @('-Part', 'ffmpeg', '-Export', $good, '-Root', $newRoot, '-Records', $records, '-FFmpeg', $part) '-FFmpeg names the FFmpeg part for the build part' 'the FFmpeg part takes no FFmpeg part'
New-Item -ItemType Directory -Path $records | Out-Null
Copy-Item -LiteralPath (Join-Path $part $FFmpegRecordName) -Destination "$records/ffmpeg-0.1.0-first.json"
Copy-Item -LiteralPath (Join-Path (New-FFmpegPart 'second') $FFmpegRecordName) -Destination "$records/ffmpeg-0.1.0-second.json"
Expect-Build @('-Part', 'ffmpeg', '-Export', $good, '-Root', $newRoot, '-Records', $records) 'against 2 earlier record' 'the FFmpeg part reads every earlier record'
[IO.File]::WriteAllText("$records/notes.txt", 'left here')
Expect-Refusal @('-Part', 'ffmpeg', '-Export', $good, '-Root', $newRoot, '-Records', $records) 'notes\.txt is not the record of an FFmpeg part' 'a folder of records holds records alone'
Remove-Item -LiteralPath "$records/notes.txt"
[IO.File]::WriteAllText("$records/broken.json", '{ "part": "build", "builds": [] }')
Expect-Refusal @('-Part', 'ffmpeg', '-Export', $good, '-Root', $newRoot, '-Records', $records) 'broken\.json is not the record of an FFmpeg part' 'a record of something else among them is refused'
Remove-Item -LiteralPath "$records/broken.json"
$unset = New-FixtureExport 'unset'
Expect-Build @('-Part', 'ffmpeg', '-Export', $unset, '-Root', $newRoot, '-Records', $records) 'Would build FFmpeg' 'the FFmpeg part needs no key'

# The build part's export, as before: its key, its feed, its presets, the tree and the folder.
Expect-Build (@('-Export', $unset, '-Root', $newRoot, '-Trial') + (Build-Part $unset)) 'Would build .*\(TRIAL\)' 'a trial is built with no key in the preset'
Expect-Refusal (@('-Export', $unset, '-Root', $newRoot) + (Build-Part $unset)) 'holds no public key yet' 'without a key in the preset nothing is built'
$wrongKey = New-FixtureExport 'wrong-key' -Key ($key.Substring(0, 127) + $last)
Expect-Refusal (@('-Export', $wrongKey, '-Root', $newRoot) + (Build-Part $wrongKey)) 'not 128 hexadecimal digits of a point' 'a key that is no point is refused'
$wrongFeed = New-FixtureExport 'wrong-feed' -Key $key -Feed 'https://example.com/stable.json'
Expect-Refusal (@('-Export', $wrongFeed, '-Root', $newRoot) + (Build-Part $wrongFeed)) 'feed is not' 'another feed is refused'
Expect-Refusal (@('-Export', $wrongFeed, '-Root', $newRoot, '-Trial') + (Build-Part $wrongFeed)) 'feed is not' 'another feed is refused in a trial too'
$wrongBase = New-FixtureExport 'wrong-base' -Key $key -Inherits 'debug'
Expect-Refusal (@('-Export', $wrongBase, '-Root', $newRoot) + (Build-Part $wrongBase)) 'must inherit the release preset' 'a publish preset on another base is refused'
$wrongArm = New-FixtureExport 'wrong-arm' -Key $key -ArmInherits @('arm64', 'publish')
Expect-Refusal (@('-Export', $wrongArm, '-Root', $newRoot) + (Build-Part $wrongArm)) 'arm64-publish preset must inherit the publish preset and the arm64 one, in that order' 'an arm64-publish preset of the other order is refused'
$armOwn = New-FixtureExport 'arm-own' -Key $key -ArmCache
Expect-Refusal (@('-Export', $armOwn, '-Root', $newRoot) + (Build-Part $armOwn)) 'in that order, and say nothing of its own' 'an arm64-publish preset with a feed of its own is refused'
[IO.File]::AppendAllText("$good/CMakeLists.txt", "# changed`n")
Expect-Refusal (@('-Export', $good, '-Root', $newRoot) + (Build-Part $good)) 'not committed' 'a changed file is refused'
& $git -C $good checkout -q -- CMakeLists.txt
[IO.File]::WriteAllText("$good/stray.txt", 'left here')
Expect-Refusal (@('-Export', $good, '-Root', $newRoot) + (Build-Part $good)) 'not committed' 'a file left in the export is refused'
Remove-Item -LiteralPath "$good/stray.txt"
Expect-Refusal (@('-Export', "$good/tools", '-Root', $newRoot) + (Build-Part $good)) 'not the top of a git repository' 'a folder inside a repository is refused'
Expect-Refusal (@('-Export', $work, '-Root', $newRoot) + (Build-Part $good)) 'not the top of a git repository|no CMakeLists' 'a folder that is no export is refused'
$notDesktop = New-FixtureExport 'not-desktop' -Key $key
& $git -C $notDesktop rm -q tools/build_release.ps1 2>&1 | Out-Null
& $git -C $notDesktop -c user.name=fixture -c user.email=fixture@example.invalid commit -q -m less 2>&1 | Out-Null
Expect-Refusal (@('-Export', $notDesktop, '-Root', $newRoot) + (Build-Part $notDesktop)) 'no tools/build_release.ps1' 'a tree that is not the desktop''s is refused'
$withDeps = New-FixtureExport 'with-deps' -Key $key -Extra @('.deps/ffmpeg/x.txt')
Expect-Refusal (@('-Export', $withDeps, '-Root', $newRoot) + (Build-Part $withDeps)) 'never committed' 'an export that holds this PC''s dependencies is refused'
$withLocal = New-FixtureExport 'with-local' -Key $key -Extra @('local.cmake')
Expect-Refusal (@('-Export', $withLocal, '-Root', $newRoot) + (Build-Part $withLocal)) 'never committed' 'an export that holds local.cmake is refused'
$foreign = Join-Path $work 'foreign-root'
New-Item -ItemType Directory -Path $foreign | Out-Null
[IO.File]::WriteAllText("$foreign/somebody.txt", 'not the script''s')
Expect-Refusal (@('-Export', $good, '-Root', $foreign) + (Build-Part $good)) 'did not make' 'a folder with something else in it is never used'
$ours = Join-Path $work 'our-root'
New-Item -ItemType Directory -Path "$ours/src" | Out-Null
[IO.File]::WriteAllText("$ours/.qkvm-release-root", 'made by build_release.ps1')
Expect-Build (@('-Export', $good, '-Root', $ours) + (Build-Part $good)) 'Would build' 'a folder the script made before is used again'
Expect-Refusal (@('-Export', $good, '-Root', "$good/build-here") + (Build-Part $good)) 'must be apart' 'a build folder inside the export is refused'

if ($failures) { Write-Output "$failures release build check(s) failed."; exit 1 }
Remove-Item -LiteralPath $work -Recurse -Force
Write-Output 'Release build checks passed: keys, symbols paths, digests, the publish presets, the FFmpeg part and what the build part takes of it, and the exports and folders that are refused.'
