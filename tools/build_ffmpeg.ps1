#Requires -Version 7.2
<#
.SYNOPSIS
Builds the project's own minimal FFmpeg into .deps\ffmpeg, in WSL, from pinned sources and a pinned toolchain.

.DESCRIPTION
    tools\build_ffmpeg.ps1
    tools\build_ffmpeg.ps1 -Downloads <folder> -Work <folder>      (what tools/build_release.ps1 does)
    tools\build_ffmpeg.ps1 -Arch arm64 [-Downloads <folder>]       (for Windows on ARM)
    tools\build_ffmpeg.ps1 -Arch <x64|arm64> -Downloads <folder> -Work <folder> -NoImportLibraries
                                              (a release's FFmpeg part: tools/build_release.ps1 -Part ffmpeg, with WSL)
    tools\build_ffmpeg.ps1 -Arch <x64|arm64> -ImportLibrariesOnly  (its build part: build_release.ps1 -Part build)

What is in it: the H.264, JPEG and PNG decoders, the H.264 parser, decoding on the graphics card through
Direct3D 11, and zlib (for PNG), as avcodec and avutil DLLs for Windows x64, LGPL version 3 or later
(--enable-version3), nothing GPL or non-free, no outside library, no program; everything else of FFmpeg
is disabled (--disable-everything).

From what, each checked against the size and SHA-256 written below before it is used:
- FFmpeg's release archive, as ffmpeg.org signs it (the signature was checked against FFmpeg's release key,
  FCF986EA15E6E293A5644F10B4322F04D67658D8, when the pin was made);
- zlib's release archive (the same bytes from GitHub and from zlib.net when the pin was made);
- a cross compiler and its tools, as Ubuntu 24.04 packages them, unpacked into the work folder (nothing is
  installed, in WSL or in Windows). WSL's Ubuntu 24.04 (or a distribution with its C library) runs them.

With -Arch arm64 the same sources, with the same options, become ARM64 DLLs, with FFmpeg's NEON code (its
ARM64 assembly, and the configuration's own checks of the assembler) on: Ubuntu packages no compiler for ARM64
Windows, so the cross compiler is llvm-mingw's (clang and lld, with mingw-w64's headers and runtime built for
ARM64), one archive of its release on GitHub for an x86_64 Ubuntu host, with the runtime of Windows' own
msvcrt.dll as Ubuntu's x64 compiler has it (the owner's choice, 2026-11-19); and Ubuntu's make, pinned below for
x64. Unpacked into the work folder and used in WSL as Ubuntu's packages are. The result goes where an ARM64
build looks for it (common.ps1, Get-DepsRoot), and says in build.json which of FFmpeg's ARM64 features it has;
a build without NEON is refused.

The import libraries the program links with are made by Microsoft's own lib tool from the DLLs' export
lists, which stay in the tree (def). lib stamps each with the time it was made, so they are other bytes at every
making: -NoImportLibraries leaves them out, and the tree is then the same bytes at every build of the same pins (a
release's FFmpeg part, checked against the record of an earlier build of them); -ImportLibrariesOnly makes them, and
nothing else, in a tree that is there already (a release's build part, on a PC without WSL). The archives of the
sources are kept in the downloads folder: they are the source offered with the release (tools/release_files.ps1).
.deps\ffmpeg\build.json says what was built from what.
#>
param(
    [string] $Downloads = '',
    [string] $Work = '',
    [string] $Distribution = 'Ubuntu',
    [ValidateSet('x64', 'arm64')] [string] $Arch = 'x64',
    [switch] $NoImportLibraries,
    [switch] $ImportLibrariesOnly
)

. "$PSScriptRoot\common.ps1"
. "$PSScriptRoot\build_common.ps1"

$FFmpeg = @{ Name = 'ffmpeg-9.0.2.tar.xz'; Url = 'https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz'; Bytes = 12040788
    Sha256 = '8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e' }
$Zlib = @{ Name = 'zlib-1.3.2.tar.gz'; Url = 'https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz'
    Bytes = 1502830; Sha256 = 'bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16' }
$Ubuntu = 'http://archive.ubuntu.com/ubuntu/pool'
$Toolchain = @(
    @{ Name = 'gcc-mingw-w64-x86-64-win32_13.2.0-6ubuntu1+26.1_amd64.deb'; Path = 'universe/g/gcc-mingw-w64'; Bytes = 35402818
       Sha256 = '80e1430303e1a619fc9cfc66225893d81486e52801e1916b712d8111d100ae38' }
    @{ Name = 'gcc-mingw-w64-x86-64-win32-runtime_13.2.0-6ubuntu1+26.1_amd64.deb'; Path = 'universe/g/gcc-mingw-w64'; Bytes = 14185558
       Sha256 = '7ac09bc7f283f1148c8e1697373db069c1d4364a9f5fa58bb66c8a93760b8b65' }
    @{ Name = 'gcc-mingw-w64-base_13.2.0-6ubuntu1+26.1_amd64.deb'; Path = 'universe/g/gcc-mingw-w64'; Bytes = 188046
       Sha256 = 'a30c07e807e9874ea93909448c5d6d37f9569ebe60ad32de8e490aad198538a3' }
    @{ Name = 'binutils-mingw-w64-x86-64_2.41.90.20240122-1ubuntu1+11.4_amd64.deb'; Path = 'universe/b/binutils-mingw-w64'; Bytes = 6053282
       Sha256 = '28bfabbf64a68813a56360bca056072af9ff71ef4090a4b8a8a085afcda3e4dd' }
    @{ Name = 'mingw-w64-x86-64-dev_11.0.1-3build1_all.deb'; Path = 'universe/m/mingw-w64'; Bytes = 3786378
       Sha256 = '018480a62907fcca19c6bf2975ae01f5c92c23dee8c3cf7a8355c10bf7d42c6f' }
    @{ Name = 'mingw-w64-common_11.0.1-3build1_all.deb'; Path = 'universe/m/mingw-w64'; Bytes = 5580230
       Sha256 = '4a42fd30fbc3838c58c929d8a4882682fc12408bb7a0e6cafa913c0f0a57c187' }
    @{ Name = 'make_4.3-4.1build2_amd64.deb'; Path = 'main/m/make-dfsg'; Bytes = 179752
       Sha256 = '1fe6a815b56c7b6e9ce4086a363f09444bbd0a0d30e230c453d0b78e44b57a99' }
    @{ Name = 'nasm_2.16.01-1build1_amd64.deb'; Path = 'universe/n/nasm'; Bytes = 458884
       Sha256 = '22eede0f2dd62343b0298182f62f7485704fe02f166395b02c92a8883377e0b3' }
    @{ Name = 'libmpc3_1.3.1-1build1_amd64.deb'; Path = 'main/m/mpclib3'; Bytes = 54520
       Sha256 = '34af1193734f40723c7fc03d855d98c107d8b62bd2fa17dae8b09d2369cdd45e' }
    @{ Name = 'libisl23_0.26-3build1_amd64.deb'; Path = 'main/i/isl'; Bytes = 679596
       Sha256 = '24b1aa0e1772080fd23df7bed10aadcbbe16b3181b77d3a013c14dfaf9848541' }
)
# For ARM64: llvm-mingw 20261104 (LLVM 23.1.5), its msvcrt kind for an x86_64 Ubuntu host, the SHA-256 GitHub publishes
# for it (build_ffmpeg.sh checks the archive against it again before it unpacks it); and make as for x64.
$LlvmMingw = @{ Name = 'llvm-mingw-20261104-msvcrt-ubuntu-22.04-x86_64.tar.xz'; Bytes = 83338104
    Url = 'https://github.com/mstorsjo/llvm-mingw/releases/download/20261104/llvm-mingw-20261104-msvcrt-ubuntu-22.04-x86_64.tar.xz'
    Sha256 = '6e3342e2e0dfa56e9178f3c34b198f278bd5e603db4a759455d1439cc2230630' }
$Parts = if ($Arch -eq 'arm64') { @($LlvmMingw) + @($Toolchain | Where-Object { $_.Name -like 'make_*' }) } else { $Toolchain }
if ($NoImportLibraries -and $ImportLibrariesOnly) { throw '-NoImportLibraries and -ImportLibrariesOnly ask for opposite things.' }
$Root = Join-Path (Get-DepsRoot -Arch $Arch) 'ffmpeg'

function New-ImportLibraries {
    # The program is linked by Microsoft's linker: import libraries of Microsoft's own making, from each DLL's exports.
    Enter-BuildEnvironment
    New-Item -ItemType Directory -Force -Path "$Root\lib" | Out-Null
    foreach ($library in 'avcodec', 'avutil') {
        $dll = @(Get-ChildItem -LiteralPath "$Root\bin" -Filter "$library-*.dll")
        if ($dll.Count -ne 1) { throw "There is not one $library DLL in $Root\bin." }
        $definition = @(Get-ChildItem -LiteralPath "$Root\def" -Filter "$library-*.def")
        if ($definition.Count -ne 1) { throw "There is not one export list of $library in $Root\def." }
        & lib.exe /nologo "/machine:$Arch" "/def:$($definition[0].FullName)" "/name:$($dll[0].Name)" "/out:$Root\lib\$library.lib" | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "lib.exe could not make $library.lib." }
        Remove-Item -LiteralPath "$Root\lib\$library.exp" -ErrorAction SilentlyContinue
    }
}
if ($ImportLibrariesOnly) {
    if (-not (Test-Path -LiteralPath (Join-Path $Root 'build.json'))) { throw "There is no FFmpeg in $Root to make import libraries for." }
    New-ImportLibraries
    Write-Host "The import libraries of the FFmpeg in $Root are made."
    return
}

if (-not $Downloads) { $Downloads = Join-Path $DesktopRoot '.deps\downloads\ffmpeg' }
if (-not $Work) { $Work = if ($Arch -eq 'arm64') { Join-Path (Get-DepsRoot -Arch arm64) 'ffmpeg-build' } else { Join-Path $DesktopRoot '.deps\ffmpeg-build' } }
$Downloads = [IO.Path]::GetFullPath($Downloads)
$Work = [IO.Path]::GetFullPath($Work)
New-Item -ItemType Directory -Force -Path $Downloads, $Work | Out-Null

Write-Host "FFmpeg 9.0.2 (minimal$(if ($Arch -eq 'arm64') { ', ARM64' })) built in $Work"
Get-Checked $FFmpeg.Url (Join-Path $Downloads $FFmpeg.Name) $FFmpeg.Bytes $FFmpeg.Sha256
Get-Checked $Zlib.Url (Join-Path $Downloads $Zlib.Name) $Zlib.Bytes $Zlib.Sha256
foreach ($package in $Parts) {
    $from = if ($package.ContainsKey('Url')) { $package.Url } else { "$Ubuntu/$($package.Path)/$($package.Name)" }
    Get-Checked $from (Join-Path $Downloads $package.Name) $package.Bytes $package.Sha256
}

function ConvertTo-Wsl([string] $Path) {
    $said = & wsl.exe -d $Distribution -e wslpath -a ($Path -replace '\\', '/')
    if ($LASTEXITCODE -ne 0 -or -not $said) { throw "WSL ($Distribution) cannot reach $Path." }
    return "$said".Trim()
}
# The script as bash reads it: with Unix line ends, whatever a checkout or an archive made of them.
$script = Join-Path $Work 'build_ffmpeg.sh'
[IO.File]::WriteAllText($script, [IO.File]::ReadAllText("$PSScriptRoot\build_ffmpeg.sh").Replace("`r`n", "`n"))
$arguments = @((ConvertTo-Wsl $script), $Arch, (ConvertTo-Wsl $Work),
    (ConvertTo-Wsl (Join-Path $Downloads $FFmpeg.Name)), (ConvertTo-Wsl (Join-Path $Downloads $Zlib.Name)))
$arguments += @($Parts | ForEach-Object { ConvertTo-Wsl (Join-Path $Downloads $_.Name) })
$said = & wsl.exe -d $Distribution -e bash --noprofile --norc @arguments 2>&1 | ForEach-Object { "$_" }
$ended = $LASTEXITCODE
$said | Write-Host
if ($ended -ne 0) { throw "The FFmpeg build in WSL failed (exit $ended)." }

# What it needs of Windows: nothing but Windows' own DLLs and each other (not the cross compiler's runtime).
$windows = '^(KERNEL32|msvcrt|USER32|ADVAPI32|bcrypt|ole32|OLEAUT32|SHELL32|avutil-\d+|avcodec-\d+)\.dll$'
foreach ($line in $said | Where-Object { $_ -match '^(avcodec|avutil)-\d+\.dll: ' }) {
    $needs = @(($line -split ': ', 2)[1] -split ' ' | Where-Object { $_ })
    $foreign = @($needs | Where-Object { $_ -notmatch $windows })
    if ($foreign.Count) { throw "$(($line -split ':')[0]) needs $($foreign -join ', '), which is not Windows'." }
}

if (Test-Path -LiteralPath $Root) { Remove-Item -LiteralPath $Root -Recurse -Force }
Copy-Item -LiteralPath (Join-Path $Work 'out\ffmpeg') -Destination $Root -Recurse
foreach ($unwanted in @(Get-ChildItem -LiteralPath $Root -Recurse -File -Include '*.dll.a', '*.a', '*.pc')) {
    Remove-Item -LiteralPath $unwanted.FullName
}
# (And the folders they were in, left empty: a tree compared file for file holds no folder without a file.)
foreach ($empty in @(Get-ChildItem -LiteralPath $Root -Recurse -Directory | Sort-Object { $_.FullName.Length } -Descending)) {
    if (-not @(Get-ChildItem -LiteralPath $empty.FullName -Force).Count) { Remove-Item -LiteralPath $empty.FullName }
}
Copy-Item -LiteralPath (Join-Path $Work 'out\def') -Destination (Join-Path $Root 'def') -Recurse
if (-not $NoImportLibraries) { New-ImportLibraries }

$needs = @('include\libavcodec\avcodec.h', 'include\libavutil\hwcontext_d3d11va.h', 'LICENSE.txt', 'zlib-LICENSE.txt')
if (-not $NoImportLibraries) { $needs += @('lib\avcodec.lib', 'lib\avutil.lib') }
foreach ($needed in $needs) {
    if (-not (Test-Path -LiteralPath (Join-Path $Root $needed))) { throw "After the build, $needed is not in $Root." }
}
# What the build says of itself, in the configuration line it carries; and nothing of where it was made.
$avutil = @(Get-ChildItem -LiteralPath "$Root\bin" -Filter 'avutil-*.dll')[0].FullName
$configuration = [regex]::Match([Text.Encoding]::Latin1.GetString([IO.File]::ReadAllBytes($avutil)),
    '(?<=\x00)--prefix=[^\x00]*').Value
foreach ($wanted in '--enable-d3d11va', '--enable-zlib') {
    if ($configuration -notmatch [regex]::Escape($wanted)) { throw "The build's configuration does not say $wanted." }
}
# (The rule the release audit holds the package's avcodec to: everything disabled, nothing found by itself, LGPL
# version 3, and nothing enabled but what build_ffmpeg.sh names.)
$refused = @(Get-FFmpegRefusals $configuration -Arch $Arch)
if ($refused.Count) { throw "The build is not the minimal one: $($refused -join '; ')." }
# ARM64: programs for ARM64 Windows, with FFmpeg's NEON code built in (wall tiles decode in software: without it, at
# a fraction of the pace). configure's own findings, as build_ffmpeg.sh kept them from config.h.
$features = $null
if ($Arch -eq 'arm64') {
    foreach ($file in Get-ChildItem -LiteralPath "$Root\bin" -File -Filter '*.dll') {
        $bytes = [IO.File]::ReadAllBytes($file.FullName)
        if ([BitConverter]::ToUInt16($bytes, [BitConverter]::ToInt32($bytes, 0x3C) + 4) -ne 0xAA64) { throw "$($file.Name) is not a program for ARM64 Windows." }
    }
    $features = [ordered]@{}
    foreach ($line in Get-Content -LiteralPath (Join-Path $Work 'out\features.txt')) {
        if ($line -match '^#define (\w+) (\d+)$') { $features[$Matches[1]] = [int] $Matches[2] }
    }
    foreach ($needed in 'ARCH_AARCH64', 'HAVE_NEON', 'HAVE_ARMV8', 'HAVE_NEON_EXTERNAL') {
        if ($features[$needed] -ne 1) { throw "The ARM64 build has no $needed (configure's config.h): FFmpeg's NEON code is not in it." }
    }
}
$wslWork = ConvertTo-Wsl $Work
foreach ($file in Get-ChildItem -LiteralPath "$Root\bin" -File) {
    foreach ($folder in $Work, $wslWork, $DesktopRoot) {
        if (Test-NamesFolder $file.FullName $folder) { throw "$($file.Name) names the folder it was built in." }
    }
}

$record = [ordered]@{
    ffmpeg = [ordered]@{ archive = $FFmpeg.Name; bytes = $FFmpeg.Bytes; sha256 = $FFmpeg.Sha256 }
    zlib = [ordered]@{ archive = $Zlib.Name; bytes = $Zlib.Bytes; sha256 = $Zlib.Sha256 }
    toolchain = @($Parts | ForEach-Object { [ordered]@{ package = $_.Name; bytes = $_.Bytes; sha256 = $_.Sha256 } })
    wsl = $Distribution
    configuration = $configuration
    # (The DLLs and their export lists: the import libraries made from them are other bytes at every making.)
    files = @(Get-ChildItem -LiteralPath "$Root\bin", "$Root\def" -File | Sort-Object Name | ForEach-Object {
        [ordered]@{ file = $_.Name; bytes = $_.Length; sha256 = Get-Sha256 $_.FullName } })
}
if ($features) { $record['architecture'] = $Arch; $record['features'] = $features }
$record | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$Root\build.json" -Encoding utf8
Write-Host "FFmpeg is in ${Root}: H.264, JPEG and PNG, Direct3D 11 decoding, zlib; LGPL version 3 or later$(if ($features) { '; ARM64, NEON on' })."
