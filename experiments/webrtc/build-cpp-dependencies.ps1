$ErrorActionPreference = 'Stop'
$env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$OutputEncoding = [Console]::OutputEncoding
$DesktopRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
Set-Location $DesktopRoot
. .\tools\common.ps1
Enter-BuildEnvironment
$trialRoot = Join-Path $DesktopRoot '.deps\webrtc-trial'
$prefix = Join-Path $trialRoot 'install'
$mbedBuild = Join-Path $trialRoot 'build-mbedtls'
# The source paths the libraries keep (__FILE__, in their logs and checks) begin below this folder: the folder they
# were built in is no part of what ships (/d1trimfile: takes it off).
$trim = "/d1trimfile:$trialRoot"
& {
cmake -S (Join-Path $trialRoot 'mbedtls-3.6.7') -B $mbedBuild -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL "-DCMAKE_C_FLAGS=/DMBEDTLS_SSL_DTLS_SRTP $trim" "-DCMAKE_INSTALL_PREFIX=$prefix" -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF -DUSE_STATIC_MBEDTLS_LIBRARY=ON -DUSE_SHARED_MBEDTLS_LIBRARY=OFF
if ($LASTEXITCODE -ne 0) { throw 'Mbed TLS configure failed' }
cmake --build $mbedBuild --parallel 2
if ($LASTEXITCODE -ne 0) { throw 'Mbed TLS build failed' }
cmake --install $mbedBuild
if ($LASTEXITCODE -ne 0) { throw 'Mbed TLS local install failed' }
$dataBuild = Join-Path $trialRoot 'build-libdatachannel'
cmake -S (Join-Path $trialRoot 'libdatachannel') -B $dataBuild -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl "-DCMAKE_C_FLAGS=/DMBEDTLS_SSL_DTLS_SRTP $trim" "-DCMAKE_CXX_FLAGS=/DWIN32 /D_WINDOWS /W3 /GR /EHsc /DMBEDTLS_SSL_DTLS_SRTP $trim" -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL "-DCMAKE_INSTALL_PREFIX=$prefix" "-DCMAKE_PREFIX_PATH=$prefix" -DUSE_MBEDTLS=ON -DUSE_GNUTLS=OFF -DUSE_NICE=OFF -DNO_MEDIA=OFF -DNO_WEBSOCKET=ON -DNO_EXAMPLES=ON -DNO_TESTS=ON -DPREFER_SYSTEM_LIB=OFF -DUSE_SYSTEM_SRTP=OFF -DUSE_SYSTEM_JUICE=OFF -DUSE_SYSTEM_USRSCTP=OFF -DUSE_SYSTEM_PLOG=OFF -DBUILD_SHARED_LIBS=ON
if ($LASTEXITCODE -ne 0) { throw 'libdatachannel configure failed' }
cmake --build $dataBuild --parallel 2
if ($LASTEXITCODE -ne 0) { throw 'libdatachannel build failed' }
cmake --install $dataBuild
if ($LASTEXITCODE -ne 0) { throw 'libdatachannel local install failed' }
Write-Output 'Pinned C++ dependencies built successfully.'
} *>&1 | Tee-Object -FilePath (Join-Path $trialRoot 'cpp-dependency-build.txt')
