#Requires -Version 7.2
<#
.SYNOPSIS
Runs the measurements of step M1: walls of 16, 36 and 64 tiles, and one large picture, in the rig.

.DESCRIPTION
    tools\measure.ps1                       every run, on the adapter Windows chooses
    tools\measure.ps1 -Adapters 0, 1        the same on each of these adapters (qkvm-bench --adapters lists them)
    tools\measure.ps1 -Only wall            only the walls; or "one" for the single large picture
    tools\measure.ps1 -Tiles 64             only the walls of that many tiles
    tools\measure.ps1 -Seconds 30           longer runs (15)
    tools\measure.ps1 -CardDecoders 24      never more streams on the card than this (64)

Each run is the rig (bench) for a few seconds with one kind of picture and one way of decoding.
What each wrote is kept in build\measurements, one file a run, and build\measurements\summary.md
puts the last lines of all that are there side by side: it can be run in parts. A window is on the screen while it runs; it does not take the
keyboard, and it must not be covered: a run during which Windows said nobody could see the window
is marked.

What it measures is the PC it runs on. Before a number is written down, say what kind of PC
that was, without naming its parts.
#>
param(
    [int[]] $Adapters = @(-1),
    [ValidateSet('all', 'wall', 'one')] [string] $Only = 'all',
    [int[]] $Tiles = @(16, 36, 64),
    [int] $Seconds = 15,
    # One maker's driver stops for good when it is given a thirty-second stream to decode.
    # For such an adapter the walls are measured with fewer on the card and
    # the rest in software; each run's line says how many were where.
    [int] $CardDecoders = 64
)

. "$PSScriptRoot\common.ps1"
Enter-BuildEnvironment

$Bench = Join-Path $DesktopRoot 'build\release\bin\qkvm-bench.exe'
if (-not (Test-Path -LiteralPath $Bench)) { throw 'There is no release build yet. Build it first: tools\build.ps1 release' }
$Data = Join-Path $DesktopRoot '.testdata'
if (-not (Test-Path -LiteralPath $Data)) { throw 'There are no test pictures yet. Make them first: tools\make_testdata.ps1' }
$Out = Join-Path $DesktopRoot 'build\measurements'
New-Item -ItemType Directory -Force -Path $Out | Out-Null

# What a wall is made of: the name of the run, the sources its tiles take in turn, and how many
# pictures a second one tile gets.
$Pictures = @(
    @{ Name = 'jpeg-previews'; Sources = 'desktop-preview-480', 'pattern-preview-480'; PerSecond = 2; H264 = $false }
    @{ Name = '720p15-still'; Sources = , 'desktop-720p15.h264'; PerSecond = 15; H264 = $true }
    @{ Name = '720p15-moving'; Sources = , 'pattern-720p15.h264'; PerSecond = 15; H264 = $true }
    @{ Name = '1080p30-still'; Sources = , 'desktop-1080p30.h264'; PerSecond = 30; H264 = $true }
    @{ Name = '1080p30-moving'; Sources = , 'pattern-1080p30.h264'; PerSecond = 30; H264 = $true }
)

$ran = 0

function Invoke-Run {
    <# One run of the rig. What it wrote is kept under the run's name. #>
    param([string] $Name, [int] $Adapter, [string[]] $Arguments)
    $file = Join-Path $Out "$(if ($Adapter -ge 0) { "adapter$Adapter" } else { 'chosen' })-$Name.txt"
    $all = @('--data', $Data, '--seconds', $Seconds, '--report', $file) + $Arguments
    if ($Adapter -ge 0) { $all += '--adapter', $Adapter }
    Write-Host "  $Name"
    # A run that does not end by itself is ended, and said to have been: one that hangs must not
    # hold up the rest, and is a finding of its own.
    $run = Start-Process -FilePath $Bench -ArgumentList $all -NoNewWindow -PassThru
    if (-not $run.WaitForExit(($Seconds + 45) * 1000)) {
        $run.Kill()
        $run.WaitForExit()
        Add-Content -LiteralPath $file -Value 'ended hung=1'
    } elseif ($run.ExitCode -ne 0) {
        Add-Content -LiteralPath $file -Value "ended exit=$($run.ExitCode)"
    }
    $script:ran++
}

foreach ($adapter in $Adapters) {
    Write-Host "Adapter $(if ($adapter -ge 0) { $adapter } else { 'that Windows chooses' })"
    if ($Only -in 'all', 'wall') {
        # Not "$tiles": to PowerShell that is the parameter $Tiles, a list.
        foreach ($count in $Tiles) {
            foreach ($picture in $Pictures) {
                # A JPEG is always decoded in software. H.264 is tried all on the card, all in
                # software, and half and half.
                $ways = if ($picture.H264) { 'card', 'software', 'mixed' } else { , 'software' }
                foreach ($way in $ways) {
                    $arguments = @('--tiles', $count, '--size', '1920x1080', '--at', '80,80')
                    foreach ($source in $picture.Sources) { $arguments += '--source', $source }
                    $arguments += switch ($way) {
                        'card' { '--decoding', 'card', '--card-decoders', [Math]::Min($count, $CardDecoders) }
                        'software' { '--decoding', 'software' }
                        'mixed' { '--decoding', 'auto', '--large', [int]($count / 2), '--card-decoders', [Math]::Min($count, $CardDecoders) }
                    }
                    Invoke-Run "wall-$count-$($picture.Name)-$way" $adapter $arguments
                }
            }
        }
    }
    if ($Only -in 'all', 'one') {
        # One large picture, as the controlled machine's is: without a frame, and each picture shown
        # the moment it is there.
        foreach ($way in 'card', 'software') {
            foreach ($clip in 'desktop-2160p60', 'pattern-2160p60', 'desktop-1080p30', 'pattern-1080p30') {
                foreach ($pace in 'at-once', 'monitor') {
                    $arguments = @('--tiles', 1, '--source', "$clip.h264", '--decoding', $way, '--size', '3840x2160', '--at', '0,0', '--borderless')
                    if ($pace -eq 'at-once') { $arguments += '--at-once' }
                    Invoke-Run "one-$clip-$way-$pace" $adapter $arguments
                }
            }
        }
    }
}

# Side by side, from every report that is in the folder. A column is left empty where a run did not say.
$lines = @(
    "Processor: percent of all $([Environment]::ProcessorCount) logical processors together. Each run's own file says when it was made."
    ''
    '| run | wanted/s | shown/s | processor % | card decoding % | card drawing % | card MB | committed MB | on card | in software | decode ms | arrived to shown ms | worst ms | dropped | replaced | refused | failed | unseen | lost |'
    '|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|'
)
foreach ($file in Get-ChildItem -LiteralPath $Out -Filter *.txt | Sort-Object { $_.BaseName -replace '(\d+)', { $_.Value.PadLeft(4, '0') } }) {
    $said = @{}
    $content = @(Get-Content -LiteralPath $file.FullName)
    foreach ($pair in (($content | Where-Object { $_ -like 'summary *' } | Select-Object -Last 1) -split ' ' | Where-Object { $_ -like '*=*' })) {
        $key, $value = $pair -split '=', 2
        $said[$key] = $value
    }
    $cells = @(
        $file.BaseName, $said['wanted_s'], $said['shown_s'], $said['processor'], $said['decoding'], $said['drawing'], $said['card_mb_most'],
        $said['committed_mb_most'], $said['on_card'], $said['in_software'], $said['decode_ms'], $said['arrived_to_shown_ms'], $said['worst_ms'],
        $said['dropped'], $said['replaced'], $said['refused'], $said['streams_failed'], $said['unseen'], $said['recovered']
    )
    $lines += '| ' + ($cells -join ' | ') + ' |' + $(if ($content | Where-Object { $_ -like 'ended *' }) { ' did not end well' })
}
$table = Join-Path $Out 'summary.md'
Set-Content -LiteralPath $table -Value $lines -Encoding utf8
Write-Host "$ran runs. All that are there, side by side: $table"
