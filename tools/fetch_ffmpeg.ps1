<#
.SYNOPSIS
Fetches a development FFmpeg into .deps\ffmpeg: for working on the program, and for the encoders
tools\make_testdata.ps1 makes its clips with. What is handed out is built with the project's own
(tools\build_ffmpeg.ps1, which tools\build_release.ps1 runs).

.DESCRIPTION
    tools\fetch_ffmpeg.ps1

An LGPL build, as DLLs with their headers and import libraries, from the builds that FFmpeg's own
download page points to. It is pinned to one build and checked against that build's size and
SHA-256, and then asked what it is: a build that carries GPL or non-free parts, or that cannot
decode on the graphics card through Direct3D 11, is refused.

The last build of a month is pinned because the builder keeps those for two years, and the daily
ones for two weeks. The release's own FFmpeg has nothing in it but what is used (DESIGN.md
section 9).

    tools\fetch_ffmpeg.ps1 -Downloads D:\kept

keeps the archive in D:\kept, where the next run takes it again after checking its size and SHA-256.
#>
param([string] $Downloads = '')

. "$PSScriptRoot\common.ps1"

$Release = 'autobuild-2026-09-30-13-08'
$Name = 'ffmpeg-n9.0.2-17-g2a571b6068-win64-lgpl-shared-9.0.zip'
$Bytes = 76972461
$Sha256 = '7157177b8a6cb2174c1650ba8c71b363f2c78cba5330f88c4c02cf5b2b880646'

$Deps = Join-Path $DesktopRoot '.deps'
$Root = Join-Path $Deps 'ffmpeg'
New-Item -ItemType Directory -Force -Path $Deps | Out-Null
$keep = [bool] $Downloads
if ($keep) { New-Item -ItemType Directory -Force -Path $Downloads | Out-Null } else { $Downloads = $Deps }
$zip = Join-Path $Downloads $Name

Write-Host "FFmpeg ($Name) into $Root"
Get-Checked "https://github.com/BtbN/FFmpeg-Builds/releases/download/$Release/$Name" $zip $Bytes $Sha256
if (Test-Path -LiteralPath $Root) { Remove-Item -LiteralPath $Root -Recurse -Force }
# The archive holds one folder with the build's name; what is in it goes straight into .deps\ffmpeg.
Expand-Checked $zip $Root -Strip 1

foreach ($needed in 'bin\ffmpeg.exe', 'include\libavcodec\avcodec.h', 'include\libavutil\hwcontext_d3d11va.h', 'lib\avcodec.lib', 'lib\avutil.lib') {
    if (-not (Test-Path -LiteralPath (Join-Path $Root $needed))) { throw "After unpacking, $needed is not in $Root." }
}

# What the build says of itself. Asked of the build, not read from its name.
$ffmpeg = Join-Path $Root 'bin\ffmpeg.exe'
$configuration = (& $ffmpeg -hide_banner -buildconf) -join "`n"
foreach ($refused in '--enable-gpl', '--enable-nonfree') {
    if ($configuration -match [regex]::Escape($refused)) { throw "This FFmpeg was built with $refused. Only an LGPL build may ship with the program." }
}
$onTheCard = (& $ffmpeg -hide_banner -hwaccels) -join "`n"
if ($onTheCard -notmatch '(?m)^d3d11va\s*$') { throw 'This FFmpeg cannot decode through Direct3D 11 (d3d11va is not among its hwaccels).' }
$decoders = (& $ffmpeg -hide_banner -decoders) -join "`n"
foreach ($decoder in 'h264', 'mjpeg', 'png') {
    if ($decoders -notmatch "(?m)^\s*V\S*\s+$decoder\s") { throw "This FFmpeg has no $decoder decoder." }
}

if (-not $keep) { Remove-Item -LiteralPath $zip -Force }
Write-Host "FFmpeg is in ${Root}: LGPL, with Direct3D 11 decoding, H.264, JPEG and PNG."
