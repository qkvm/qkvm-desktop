#Requires -Version 7.2
<# Builds the approved pinned libdatachannel/Mbed TLS stack in .deps, without a global install.
The same manifest and scripts used by the successful dependency trial are the source of truth.
-Arch arm64 builds the same sources for ARM64 (into .deps\arm64\webrtc, or where local.cmake's QKVM_DEPS_ARM64 says). #>
param([ValidateSet('x64', 'arm64')] [string] $Arch = 'x64')
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot '..\experiments\webrtc\fetch-cpp-dependencies.ps1')
if ($LASTEXITCODE -ne 0) { throw 'The pinned WebRTC sources could not be prepared.' }
& (Join-Path $PSScriptRoot '..\experiments\webrtc\build-cpp-dependencies.ps1') -Arch $Arch
if ($LASTEXITCODE -ne 0) { throw 'The pinned WebRTC dependencies could not be built.' }
