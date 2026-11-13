param(
    [Parameter(Mandatory = $true)][string]$Ffmpeg,
    [Parameter(Mandatory = $true)][string]$Output
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $Ffmpeg -PathType Leaf)) { throw 'Existing FFmpeg executable not found.' }
if (Test-Path -LiteralPath $Output) { throw 'Choose a new fixture path; the experiment does not overwrite files.' }
# A moving generated picture. No capture device, network URL, or user content is read.
& $Ffmpeg -hide_banner -loglevel error -nostdin -n -f lavfi -i 'testsrc2=size=320x180:rate=30' `
    -frames:v 60 -an -c:v libopenh264 -profile:v 66 -b:v 600k -g 30 -bf 0 `
    -bsf:v 'h264_metadata=aud=insert' -f h264 $Output
if ($LASTEXITCODE -ne 0) { throw "Fixture generation failed: $LASTEXITCODE" }
