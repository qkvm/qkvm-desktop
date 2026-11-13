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
$env:PATH = "$prefix\bin;$DesktopRoot\.deps\ffmpeg\bin;$env:PATH"
$experiment = Join-Path $DesktopRoot 'experiments\webrtc\cpp'
$fixture = Join-Path $trialRoot 'moving-320x180.h264'
if (-not (Test-Path $fixture)) {
 & (Join-Path $experiment 'make_fixture.ps1') -Ffmpeg (Join-Path $DesktopRoot '.deps\ffmpeg\bin\ffmpeg.exe') -Output $fixture
}
$harnessBuild = Join-Path $trialRoot 'build-cpp-harness'
& {
 cmake -S $experiment -B $harnessBuild -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=cl "-DCMAKE_PREFIX_PATH=$prefix" "-DQKVM_FFMPEG_DIR=$DesktopRoot\.deps\ffmpeg" "-DQKVM_TRIAL_FIXTURE=$fixture"
 if ($LASTEXITCODE -ne 0) { throw 'Harness configure failed' }
 cmake --build $harnessBuild --parallel 2
 if ($LASTEXITCODE -ne 0) { throw 'Harness build failed' }
 ctest --test-dir $harnessBuild --output-on-failure -V
 if ($LASTEXITCODE -ne 0) { throw 'C++ receiver trial failed' }
} *>&1 | Tee-Object -FilePath (Join-Path $trialRoot 'cpp-trial.txt')
