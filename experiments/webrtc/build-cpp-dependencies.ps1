# -Arch arm64: the same sources (fetched by fetch-cpp-dependencies.ps1 into .deps\webrtc-trial, as for x64), built with
# the same options for ARM64 into the ARM64 libraries' folder (tools\common.ps1, Get-DepsRoot: .deps\arm64\webrtc).
# -Sources: where those sources are, when not in .deps\webrtc-trial (a copy of the same pinned checkout and archive).
# Whichever: nothing is built unless they are the pinned sources, tree for tree by their digests (tools\build_common.ps1,
# $WebRtcPins and Get-WebRtcSourceRefusals), and what was built from and what was made is written beside the install
# folder (webrtc-record.json, New-WebRtcRecord), which tools\package.ps1 and the release audit hold a package to.
param([ValidateSet('x64', 'arm64')] [string] $Arch = 'x64', [string] $Sources = '')
$ErrorActionPreference = 'Stop'
$env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$OutputEncoding = [Console]::OutputEncoding
$DesktopRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
Set-Location $DesktopRoot
. .\tools\common.ps1
. .\tools\build_common.ps1
$trialRoot = if ($Sources) { [IO.Path]::GetFullPath($Sources) } else { Join-Path $DesktopRoot '.deps\webrtc-trial' }
# (Before anything else is done or made: a tree that is not the pinned one is refused here, whoever gave it.)
$checked = Get-WebRtcSourceRefusals $trialRoot
if ($checked.Refusals.Count) { throw "Not the pinned WebRTC sources, so nothing is built: $($checked.Refusals -join '; ')." }
Write-Output "The pinned sources: libdatachannel's tree $($checked.Trees.libdatachannel), Mbed TLS's $($checked.Trees.mbedtls)."
Enter-BuildEnvironment -Arch $Arch
$builtRoot = if ($Arch -eq 'x64') { Join-Path $DesktopRoot '.deps\webrtc-trial' } else { Join-Path (Get-DepsRoot -Arch $Arch) 'webrtc' }
[IO.Directory]::CreateDirectory($builtRoot) | Out-Null
$prefix = Join-Path $builtRoot 'install'
$mbedBuild = Join-Path $builtRoot 'build-mbedtls'
# The source paths the libraries keep (__FILE__, in their logs and checks) begin below this folder: the folder they
# were built in is no part of what ships (/d1trimfile: takes it off).
$trim = "/d1trimfile:$trialRoot"
# (Built for ARM64 on an x64 PC, CMake is told so: it then runs nothing it builds.)
$cross = @()
if ($Arch -eq 'arm64' -and $env:PROCESSOR_ARCHITECTURE -ne 'ARM64') { $cross = @('-DCMAKE_SYSTEM_NAME=Windows', '-DCMAKE_SYSTEM_PROCESSOR=ARM64') }
# (Mbed TLS's configuration runs its Python scripts, which would leave their compiled forms in the sources' folders:
# the sources stay as they were checked, for the next build's check.)
$env:PYTHONDONTWRITEBYTECODE = '1'
& {
cmake -S (Join-Path $trialRoot 'mbedtls-3.6.7') -B $mbedBuild -G Ninja @cross -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL "-DCMAKE_C_FLAGS=/DMBEDTLS_SSL_DTLS_SRTP $trim" "-DCMAKE_INSTALL_PREFIX=$prefix" -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF -DUSE_STATIC_MBEDTLS_LIBRARY=ON -DUSE_SHARED_MBEDTLS_LIBRARY=OFF
if ($LASTEXITCODE -ne 0) { throw 'Mbed TLS configure failed' }
cmake --build $mbedBuild --parallel 2
if ($LASTEXITCODE -ne 0) { throw 'Mbed TLS build failed' }
cmake --install $mbedBuild
if ($LASTEXITCODE -ne 0) { throw 'Mbed TLS local install failed' }
$dataBuild = Join-Path $builtRoot 'build-libdatachannel'
cmake -S (Join-Path $trialRoot 'libdatachannel') -B $dataBuild -G Ninja @cross -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl "-DCMAKE_C_FLAGS=/DMBEDTLS_SSL_DTLS_SRTP $trim" "-DCMAKE_CXX_FLAGS=/DWIN32 /D_WINDOWS /W3 /GR /EHsc /DMBEDTLS_SSL_DTLS_SRTP $trim" -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL "-DCMAKE_INSTALL_PREFIX=$prefix" "-DCMAKE_PREFIX_PATH=$prefix" -DUSE_MBEDTLS=ON -DUSE_GNUTLS=OFF -DUSE_NICE=OFF -DNO_MEDIA=OFF -DNO_WEBSOCKET=ON -DNO_EXAMPLES=ON -DNO_TESTS=ON -DPREFER_SYSTEM_LIB=OFF -DUSE_SYSTEM_SRTP=OFF -DUSE_SYSTEM_JUICE=OFF -DUSE_SYSTEM_USRSCTP=OFF -DUSE_SYSTEM_PLOG=OFF -DBUILD_SHARED_LIBS=ON
if ($LASTEXITCODE -ne 0) { throw 'libdatachannel configure failed' }
cmake --build $dataBuild --parallel 2
if ($LASTEXITCODE -ne 0) { throw 'libdatachannel build failed' }
cmake --install $dataBuild
if ($LASTEXITCODE -ne 0) { throw 'libdatachannel local install failed' }
Write-Output 'Pinned C++ dependencies built successfully.'
} *>&1 | Tee-Object -FilePath (Join-Path $builtRoot 'cpp-dependency-build.txt')
# What was built from and what was made, beside the install folder: the pins, the trees as checked above, this script,
# the commit of the tree it ran in (where that is a checkout), and every library made.
$commit = ''
if (Test-Path -LiteralPath (Join-Path $DesktopRoot '..\.git')) {
    $commit = "$(git -C $DesktopRoot rev-parse HEAD 2>$null)".Trim()
    if ($LASTEXITCODE -ne 0) { $commit = '' }
}
New-WebRtcRecord -Arch $Arch -Trees $checked.Trees -Install $prefix -Script $PSCommandPath -Commit $commit |
    ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $builtRoot $WebRtcRecordName) -Encoding utf8
Write-Output "Recorded in $(Join-Path $builtRoot $WebRtcRecordName)."
