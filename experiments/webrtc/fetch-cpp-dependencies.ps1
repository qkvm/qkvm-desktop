$ErrorActionPreference = 'Stop'
$env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$OutputEncoding = [Console]::OutputEncoding
$env:GIT_TERMINAL_PROMPT = '0'
$DesktopRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$trialRoot = Join-Path $DesktopRoot '.deps\webrtc-trial'
[IO.Directory]::CreateDirectory($trialRoot) | Out-Null
$git = (Get-Command git -CommandType Application -ErrorAction Stop).Source
$source = Join-Path $trialRoot 'libdatachannel'
if (-not (Test-Path $source)) {
    & $git -c credential.helper= clone --filter=blob:none --no-checkout https://github.com/paullouisageneau/libdatachannel.git $source
    if ($LASTEXITCODE -ne 0) { throw 'libdatachannel clone failed' }
}
& $git -C $source checkout --detach 6b1e2e620f1e37f0eafeee702eaea0043cb305fd
if ($LASTEXITCODE -ne 0) { throw 'libdatachannel pin checkout failed' }
& $git -C $source -c credential.helper= submodule update --init --depth 1 deps/libjuice deps/libsrtp deps/usrsctp deps/plog
if ($LASTEXITCODE -ne 0) { throw 'Pinned submodule fetch failed' }
& $git -C $source rev-parse HEAD
& $git -C $source submodule status
$archive = Join-Path $trialRoot 'mbedtls-3.6.7.tar.bz2'
if (-not (Test-Path $archive)) { Invoke-WebRequest -Uri 'https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2' -OutFile $archive }
$hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
if ($hash -ne 'a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6') { throw 'Mbed TLS archive hash mismatch' }
Write-Output "Verified Mbed TLS SHA-256 $hash"
if (-not (Test-Path (Join-Path $trialRoot 'mbedtls-3.6.7'))) {
    & "$env:SystemRoot\System32\tar.exe" -xf $archive -C $trialRoot
    if ($LASTEXITCODE -ne 0) { throw 'Mbed TLS extraction failed' }
}
Write-Output 'Dependency sources ready.'
