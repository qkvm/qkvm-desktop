#Requires -Version 7.2
<# Builds the approved pinned libdatachannel/Mbed TLS stack in .deps, without a global install.
The same manifest and scripts used by the successful dependency trial are the source of truth. #>
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot '..\experiments\webrtc\fetch-cpp-dependencies.ps1')
if ($LASTEXITCODE -ne 0) { throw 'The pinned WebRTC sources could not be prepared.' }
& (Join-Path $PSScriptRoot '..\experiments\webrtc\build-cpp-dependencies.ps1')
if ($LASTEXITCODE -ne 0) { throw 'The pinned WebRTC dependencies could not be built.' }
