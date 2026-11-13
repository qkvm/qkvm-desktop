#Requires -Version 7.2
<#
.SYNOPSIS
Measures one machine live in the program's own window: every
film, how many pictures reach the screen, how soon and how evenly.

.DESCRIPTION
    tools\measure_live.ps1 -Python <python with the app's requirements>
    tools\measure_live.ps1 -Python ... -Films pattern-2160p60      only these films (names as in .testdata)
    tools\measure_live.ps1 -Python ... -Decoding off               decoded in software (automatic, on, off)
    tools\measure_live.ps1 -Python ... -Card power-saving          on that graphics card (most-powerful,
                                                                   power-saving, windows)
    tools\measure_live.ps1 -Python ... -Seconds 60                 longer runs (30)
    tools\measure_live.ps1 -JetKvm                                 a JetKVM by hand, shown large (M6 2i)
    tools\measure_live.ps1 -JetKvm -Cards 16                       sixteen on the wall, the first shown large
    tools\measure_live.ps1 -JetKvm -Cards 64 -Wall                 sixty-four, the wall itself measured
    tools\measure_live.ps1 -JetKvm -FirstPicture                   how soon its first picture comes, after a
                                                                   JetKVM rested 0, 5.5 and 15 seconds (-Rests)

With -JetKvm no Python is needed and no film is chosen: so many JetKVMs by hand on this PC's loopback
(bench\jetkvm_stand_in.cpp, the release qkvm-jetkvm-stand-in, each sending .testdata\jetkvm-1080p60.h264)
are added to this PC's own wall in the rig ("own", QKVM_RIG_ADD, with the stand-in's made-up password),
and the first is shown large and measured as a film is (QKVM_RIG_LIVE_MEASURE); with -Wall, the wall
of them is measured as tools\stand_in_wall.py's is (QKVM_RIG_MEASURE: the processor, committed and
video memory, the window's frames). The wall settles a quarter of a second longer for each card, so
that all of them were added and have their pictures before anything is measured. Their summary is in
the same folder. With -FirstPicture no window is shown: for each rest, one JetKVM by hand whose first
picture comes so many milliseconds after the offer (as a real one's after it rested), and the release
qkvm-jetkvm-rig against it (bench\jetkvm_rig.cpp): a card's first ask to its first key frame, as the
connection's own steps say it and as a card that asks every 250 ms gets it, and a window's first live
picture on the connection the card holds.

Each film is played by tools\stand_in_live.py (a QKVM with live pictures, started here from the app's
folder beside this one, and ended after) at its own pace, and the release session rig shows its first
made-up machine large and measures it (QKVM_RIG_LIVE_MEASURE, bench\live_measure.h). Each run of this
script keeps what it measured in a folder of its own, build\measurements\live\<when>: a report and the
rig's and the stand-in's words for each film, and summary.md with them side by side against the
targets of SPEC.md section 9. A window is on the screen while it runs, and must not be covered or made
small: the program ends a live picture nobody can see. A run that was cut short (the window covered
while it measured), or whose window was not in front, says so in the summary.

What it measures is the PC it runs on. Before a number is written down, say what kind of PC that
was, without naming its parts.
#>
param(
    [string] $Python,
    [string[]] $Films = @('desktop-1080p30', 'pattern-1080p30', 'desktop-1080p60', 'pattern-1080p60', 'pattern-2160p60'),
    [ValidateSet('automatic', 'on', 'off')] [string] $Decoding = 'automatic',
    [ValidateSet('most-powerful', 'power-saving', 'windows')] [string] $Card = 'most-powerful',
    [int] $Seconds = 30,
    [int] $Port = 8794,
    [switch] $JetKvm,
    [ValidateRange(1, 128)] [int] $Cards = 1,
    [switch] $Wall,
    [switch] $FirstPicture,
    [int[]] $Rests = @(0, 5500, 15000)
)

. "$PSScriptRoot\common.ps1"
Enter-BuildEnvironment

if (-not $JetKvm -and -not $Python) { throw '-Python is needed (a Python with the app''s requirements), but for -JetKvm.' }
if (-not $JetKvm -and ($Wall -or $Cards -ne 1)) { throw '-Cards and -Wall are for -JetKvm.' }
if ($Port -in 8787, 8788, 8789, 8790, 8791, 8793) { throw "Port $Port is a QKVM's own, or another stand-in's: another one." }
$Rig = Join-Path $DesktopRoot 'build\release\bin\qkvm-session-rig.exe'
if (-not (Test-Path -LiteralPath $Rig)) { throw 'There is no release build yet. Build it first: tools\build.ps1 release' }
$JetKvmStandIn = Join-Path $DesktopRoot 'build\release\bin\qkvm-jetkvm-stand-in.exe'
if ($JetKvm -and -not (Test-Path -LiteralPath $JetKvmStandIn)) { throw 'There is no release JetKVM stand-in yet. Build it first: tools\build.ps1 release' }
$JetKvmRig = Join-Path $DesktopRoot 'build\release\bin\qkvm-jetkvm-rig.exe'
if ($FirstPicture -and -not (Test-Path -LiteralPath $JetKvmRig)) { throw 'There is no release JetKVM rig yet. Build it first: tools\build.ps1 release' }
if ($FirstPicture -and (-not $JetKvm -or $Wall -or $Cards -ne 1)) { throw '-FirstPicture is for -JetKvm alone, with one card.' }
$Data = Join-Path $DesktopRoot '.testdata'
if (-not (Test-Path -LiteralPath $Data)) { throw 'There are no test pictures yet. Make them first: tools\make_testdata.ps1' }
$App = Join-Path (Split-Path -Parent $DesktopRoot) 'app'
if (-not $JetKvm -and -not (Test-Path -LiteralPath (Join-Path $App 'qkvm'))) { throw "The app is not beside the desktop ($App): the stand-in is run from it." }
# (What a JetKVM by hand sends, and the name it is measured under.)
if ($JetKvm) { $Films = @('jetkvm-1080p60') }
# Every film is there before the first is played.
foreach ($film in $Films) {
    if (-not (Test-Path -LiteralPath (Join-Path $Data "$film.h264"))) { throw "No film $film in .testdata." }
}
$Out = Join-Path $DesktopRoot "build\measurements\live\$(Get-Date -Format 'yyyyMMdd-HHmmss')"
# (The rig is told where to write as "file,seconds,seconds": a comma in the path would break that.)
if ($Out.Contains(',')) { throw "The checkout's path has a comma in it, which the rig's switch cannot carry: $Out" }
New-Item -ItemType Directory -Force -Path $Out | Out-Null

# Whether anything still listens on the port, and the stand-in with what it started, ended.
function Test-Listening { [bool] (Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue) }
# The process this run started and what it started in turn (a venv's python.exe starts the real one; py
# with a venv starts the venv's, which starts the real one), a few steps down, each with when it began.
# A child is one that began after its parent: Windows goes on naming a parent that has ended by its
# number, and gives that number to another process later, whose "children" are then nobody's of this
# run. (The run holds the handle of the process it started from its start: that number stays its own.)
function Get-StandInProcesses([System.Diagnostics.Process] $server) {
    # (In whole microseconds, as Windows' list of processes says when each began: a child that began
    # in the same microsecond as its parent began after it, and not before.)
    $began = $server.StartTime
    $began = $began.AddTicks(-($began.Ticks % 10))
    $all = @([pscustomobject]@{ Id = [uint32] $server.Id; Began = $began })
    $next = $all
    for ($depth = 0; $depth -lt 4 -and $next.Count -gt 0; $depth++) {
        $next = @(foreach ($parent in $next) {
                Get-CimInstance Win32_Process -Filter "ParentProcessId=$($parent.Id)" -ErrorAction SilentlyContinue |
                    Where-Object { $null -ne $_.CreationDate -and $_.CreationDate -ge $parent.Began } |
                    ForEach-Object { [pscustomobject]@{ Id = [uint32] $_.ProcessId; Began = $_.CreationDate } }
            })
        $all += $next
    }
    return $all
}
# Whether what listens on the port is the stand-in this run started, and not something that took the
# port meanwhile.
function Test-StandInListening([System.Diagnostics.Process] $server) {
    $ours = @(Get-StandInProcesses $server | ForEach-Object Id)
    $owners = @(Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue | ForEach-Object OwningProcess)
    return $owners.Count -gt 0 -and @($owners | Where-Object { $_ -notin $ours }).Count -eq 0
}
function Stop-StandIn([System.Diagnostics.Process] $server) {
    # (What it started is ended first, the deepest first; each through a handle of its own, and only where
    # that handle is of the process that was found, by when it began: held, it keeps the number its own.)
    $all = @(Get-StandInProcesses $server)
    [array]::Reverse($all)
    foreach ($found in $all) {
        $process = if ($found.Id -eq $server.Id) { $server } else { Get-Process -Id $found.Id -ErrorAction SilentlyContinue }
        if ($null -eq $process) { continue }
        try {
            $null = $process.Handle
            if ($process.HasExited) { continue }
            if ($found.Id -ne $server.Id -and [math]::Abs(($process.StartTime - $found.Began).TotalMilliseconds) -ge 1) { continue }
            $process.Kill()
        } catch {
            continue  # (ended meanwhile, or not this run's to open)
        }
    }
    for ($i = 0; $i -lt 40 -and (Test-Listening); $i++) { Start-Sleep -Milliseconds 250 }
}

# The rig's switches of whoever runs this are put aside for the runs and given back after, and so is
# what tells Qt how to draw (the tests' platform without a screen, which a test run sets, draws
# nothing): one of theirs would change a run, and theirs are not to be changed.
$asideNames = @('QT_QPA_PLATFORM', 'QSG_RENDER_LOOP', 'QSG_RHI_BACKEND', 'QT_QPA_UPDATE_IDLE_TIME', 'QT_QUICK_BACKEND')
$theirs = @(Get-ChildItem -Path 'env:QKVM_RIG_*' -ErrorAction SilentlyContinue) + @($asideNames | ForEach-Object { Get-Item -Path "env:$_" -ErrorAction SilentlyContinue })
$theirs = @($theirs | Where-Object { $_ } | ForEach-Object { [pscustomobject]@{ Name = $_.Name; Value = $_.Value } })
$theirs | ForEach-Object { Remove-Item -Path "env:$($_.Name)" }
$theirTemp = @{ TMP = $env:TMP; TEMP = $env:TEMP; TMPDIR = $env:TMPDIR }
# What went wrong with a film, by its name: the others are measured all the same, and summed up.
$failed = @{}
# (With -JetKvm, the one run's name, and how long its wall settles: each card added and with its first picture
# before anything is measured, a quarter of a second each.)
$jetName = "jetkvm-1080p60-$Cards-$(if ($Wall) { 'wall' } else { 'large' })-$Decoding-$Card"
$jetSettle = 5 + [int] [math]::Ceiling($Cards / 4)
# So many JetKVMs by hand on this PC's loopback, each on a port of its own, said by the stand-in once it says it
# is ready: the process (its handle held) and their addresses, or why not. (It is one process, which ends by
# itself a while after the run was to.)
function Start-JetKvmStandIn([int] $count, [int] $restMs, [int] $forSeconds, [string] $log) {
    $process = Start-Process -FilePath $JetKvmStandIn -WindowStyle Hidden -PassThru -RedirectStandardOutput $log -RedirectStandardError "$log.err" `
        -ArgumentList @('--count', "$count", '--first-picture-after', "$restMs", '--seconds', "$forSeconds")
    try {
        $null = $process.Handle
    } catch {
        return @{ Process = $null; Problem = "the stand-in ended as it started ($(Split-Path -Leaf $log).err)" }
    }
    for ($i = 0; $i -lt 120 -and -not $process.HasExited -and -not (Select-String -LiteralPath $log -Pattern '^ready' -Quiet -ErrorAction SilentlyContinue); $i++) {
        Start-Sleep -Milliseconds 250
    }
    $found = @(Select-String -LiteralPath $log -Pattern '^jetkvm: (http://127\.0\.0\.1:\d+)$' -ErrorAction SilentlyContinue |
            ForEach-Object { $_.Matches[0].Groups[1].Value })
    if ($process.HasExited -or $found.Count -ne $count) {
        return @{ Process = $process; Problem = "the stand-in did not come up ($(Split-Path -Leaf $log).err)" }
    }
    return @{ Process = $process; Addresses = $found; Problem = $null }
}
function Stop-JetKvmStandIn($process) {
    if ($null -eq $process) { return }
    if (-not $process.HasExited) { $process.Kill() }
    $null = $process.WaitForExit(10000)
}
# With -FirstPicture, what the JetKVM rig measured after each rest, by the rest (as text: a number would be a place).
$firstPictures = [ordered] @{}
try {
    if ($JetKvm -and $FirstPicture) {
        foreach ($rest in $Rests) {
            $name = "jetkvm-first-picture-rested-$rest"
            $started = Start-JetKvmStandIn 1 $rest 150 (Join-Path $Out "$name.stand-in.txt")
            try {
                if ($started.Problem) { $firstPictures["$rest"] = @{ problem = $started.Problem }; continue }
                # (The stand-in's made-up password, as the tests' JetKVM by hand has it: no real one's.)
                $env:QKVM_RIG_KVM_PASSWORD = 'secret'
                $said = @(& $JetKvmRig $started.Addresses[0] --cards 1 --live 10 2>&1 | ForEach-Object { "$_" })
                Remove-Item -Path env:QKVM_RIG_KVM_PASSWORD -ErrorAction Ignore
                $said | Set-Content -LiteralPath (Join-Path $Out "$name.rig.txt") -Encoding utf8
                $numbers = @{}
                foreach ($line in $said) { if ($line -match '^measured: (\w+) (-?\d+)$') { $numbers[$Matches[1]] = [long] $Matches[2] } }
                if ($said -notcontains 'all came about') { $numbers['problem'] = "the rig said: $(@($said)[-1])" }
                $firstPictures["$rest"] = $numbers
                Write-Host "  rested ${rest} ms: $(if ($numbers.ContainsKey('problem')) { $numbers['problem'] } else { 'measured' })"
            } finally {
                Stop-JetKvmStandIn $started.Process
            }
        }
    } elseif ($JetKvm) {
        $film = 'jetkvm-1080p60'
        $started = Start-JetKvmStandIn $Cards 0 ($Seconds + $jetSettle + 150) (Join-Path $Out "$jetName.stand-in.txt")
        $addresses = $started.Addresses
        try {
            if ($started.Problem) {
                $failed[$film] = $started.Problem
            } else {
                $report = Join-Path $Out "$jetName.$(if ($Wall) { 'txt' } else { 'json' })"
                # (The stand-in's made-up password, as the tests' JetKVM by hand has it: no real one's.)
                $env:QKVM_RIG_ADD = $addresses -join ','
                $env:QKVM_RIG_KVM_PASSWORD = 'secret'
                $env:QKVM_RIG_DECODING = $Decoding
                $env:QKVM_RIG_CARD = $Card
                if ($Wall) {
                    $env:QKVM_RIG_MEASURE = "$report,$jetSettle,$Seconds"
                } else {
                    # (A JetKVM is named by where it is: its address and port.)
                    $env:QKVM_RIG_ENLARGE = ([uri] $addresses[0]).Authority
                    $env:QKVM_RIG_SIZE = '1600x1000'
                    $env:QKVM_RIG_LIVE_MEASURE = "$report,$jetSettle,$Seconds"
                }
                & $Rig own ($Seconds + $jetSettle + 95) 2>&1 | Set-Content -LiteralPath (Join-Path $Out "$jetName.rig.txt") -Encoding utf8
                Write-Host "  ${jetName}: $(if (Test-Path -LiteralPath $report) { 'measured' } else { 'no report' })"
            }
        } finally {
            Stop-JetKvmStandIn $started.Process
        }
    }
    foreach ($film in @($Films | Where-Object { -not $JetKvm })) {
        $file = Join-Path $Data "$film.h264"
        $name = "$film-$Decoding-$Card"
        $log = Join-Path $Out "$name.stand-in.txt"
        if (Test-Listening) { $failed[$film] = "port $Port was in use"; continue }
        # (The stand-in's own folder, thrown away when it is ended, is in this run's: Python takes the
        # first of TMPDIR, TEMP and TMP.)
        $tmp = Join-Path $Out 'tmp'
        New-Item -ItemType Directory -Force -Path $tmp | Out-Null
        $env:TMPDIR = $tmp
        $env:TEMP = $tmp
        $env:TMP = $tmp
        try {
            $server = Start-Process -FilePath $Python -ArgumentList @("`"$(Join-Path $PSScriptRoot 'stand_in_live.py')`"", "`"$file`"", '--port', "$Port", '--machines', '2') `
                -WorkingDirectory $App -RedirectStandardOutput $log -RedirectStandardError "$log.err" -WindowStyle Hidden -PassThru
            # (Its handle, held from here on, and when it began, read now: Start-Process hands back the
            # process by its number, and a number is another's once nobody holds the process it was.
            # One that has ended already is said as one that did not come up, and the next film goes on.)
            try {
                $null = $server.Handle
                $null = $server.StartTime
            } catch {
                $server = $null
            }
        } finally {
            foreach ($key in $theirTemp.Keys) { if ($null -eq $theirTemp[$key]) { Remove-Item -Path "env:$key" -ErrorAction Ignore } else { Set-Item -Path "env:$key" -Value $theirTemp[$key] } }
        }
        if ($null -eq $server) { $failed[$film] = "the stand-in ended as it started ($name.stand-in.txt.err)"; continue }
        try {
            for ($i = 0; $i -lt 60 -and -not (Test-StandInListening $server); $i++) {
                if ($server.HasExited) { break }
                Start-Sleep -Milliseconds 500
            }
            if (-not (Test-StandInListening $server)) { $failed[$film] = "the stand-in did not come up ($name.stand-in.txt.err)"; continue }
            $report = Join-Path $Out "$name.json"
            $env:QKVM_RIG_PASSWORD = 'demo-wall-password'
            $env:QKVM_RIG_ENLARGE = 'Machine 1'
            $env:QKVM_RIG_SIZE = '1600x1000'
            $env:QKVM_RIG_DECODING = $Decoding
            $env:QKVM_RIG_CARD = $Card
            $env:QKVM_RIG_LIVE_MEASURE = "$report,5,$Seconds"
            & $Rig "http://127.0.0.1:$Port" ($Seconds + 95) 2>&1 | Set-Content -LiteralPath (Join-Path $Out "$name.rig.txt") -Encoding utf8
            # (A rig that ended while it wrote leaves a report that is cut short: said, and the next film goes on.)
            $said = 'no report'
            if (Test-Path -LiteralPath $report) {
                try {
                    $said = if ((Get-Content -LiteralPath $report -Raw | ConvertFrom-Json).PSObject.Properties['problem']) { 'no live picture reached the screen' } else { 'measured' }
                } catch {
                    $said = 'a report that could not be read'
                }
            }
            Write-Host "  $film ($Decoding, $Card): $said"
        } finally {
            Stop-StandIn $server
        }
    }
} finally {
    Get-ChildItem -Path 'env:QKVM_RIG_*' -ErrorAction SilentlyContinue | ForEach-Object { Remove-Item -Path "env:$($_.Name)" }
    $theirs | ForEach-Object { Set-Item -Path "env:$($_.Name)" -Value $_.Value }
    foreach ($key in $theirTemp.Keys) { if ($null -eq $theirTemp[$key]) { Remove-Item -Path "env:$key" -ErrorAction Ignore } else { Set-Item -Path "env:$key" -Value $theirTemp[$key] } }
    Remove-Item -LiteralPath (Join-Path $Out 'tmp') -Recurse -Force -ErrorAction SilentlyContinue
}

# This run's films side by side, against the targets of SPEC.md section 9. (Places of arrivedToShown:
# up to 2, 4, 8, 12, 16, 33, 100 ms and slower; of unevenCounts: up to 2, 4, 8, 12, 17, 25, 34 ms and more.)
$lines = @(
    "| Film | Decoding | Card | Shown a second | Not shown | Arrived to shown, about ms | Within 2 ms | Within 4 ms | Delay changed by more than 8 ms | Monitor | Notes |",
    '|---|---|---|---|---|---|---|---|---|---|---|'
)
if ($JetKvm -and $FirstPicture) {
    # How soon a JetKVM's first picture comes, by how long it rested (plan 4, M1).
    $lines = @('| Rested, ms | Asked to its first key frame, ms | A card that asks every 250 ms has it after, ms | A window''s first live picture, ms | Live pictures in 10 s | Notes |',
        '|---|---|---|---|---|---|')
    foreach ($rest in $firstPictures.Keys) {
        $n = $firstPictures["$rest"]
        $value = { param($key) if ($n.ContainsKey($key)) { $n[$key] } else { '' } }
        $lines += "| $rest | $(& $value 'first_key_frame_ms') | $(& $value 'first_card_picture_ms') | $(& $value 'first_live_picture_ms') | $(& $value 'live_pictures_per_10s') | $(& $value 'problem') |"
    }
    $lines += ''
    $lines += 'The first key frame as the connection''s own steps say it (JetKvmConnection, the video''s diagnostics); a window''s first live picture on the connection the card already holds, from the window opening.'
    $lines | Set-Content -LiteralPath (Join-Path $Out 'summary.md') -Encoding utf8
    $lines | Write-Output
    "Kept in $Out"
    return
}
if ($JetKvm -and $Wall) {
    # A wall of JetKVMs: what the wall's own report says, against SPEC.md section 9's targets for a wall.
    $report = Join-Path $Out "$jetName.txt"
    $said = @{}
    if (Test-Path -LiteralPath $report) {
        foreach ($line in Get-Content -LiteralPath $report) {
            if ($line -match '^([^:]+): (.*)$') { $said[$Matches[1]] = $Matches[2] }
        }
    }
    $lines = @('| JetKVM cards | Decoding | Card | Of the PC''s processors | MB committed | MB of the cards'' own memory | Frames a second | Work for a frame | Notes |', '|---|---|---|---|---|---|---|---|---|')
    if ($failed.ContainsKey('jetkvm-1080p60')) {
        $lines += "| $Cards | $Decoding | $Card | $($failed['jetkvm-1080p60']) | | | | | |"
    } elseif (-not $said.ContainsKey('machines')) {
        $lines += "| $Cards | $Decoding | $Card | no report | | | | | |"
    } else {
        $notes = @()
        if ([int] $said['machines'] -ne $Cards) { $notes += "$($said['machines']) on the wall" }
        if ($said['the window was the one in front, at first and at last'] -ne 'yes, yes') { $notes += 'window not in front' }
        $lines += "| $Cards | $Decoding | $Card | $($said['percent of the PC''s processors']) % | $($said['MB committed']) | $($said['MB of the graphics cards'' own memory']) | $($said['frames a second']) | $($said['ms of work for a frame']) | $($notes -join ', ') |"
    }
    $lines += ''
    $lines += 'Targets (SPEC.md section 9): a wall of 64 under 5 % of the processors and under 300 MB committed.'
    $lines | Set-Content -LiteralPath (Join-Path $Out 'summary.md') -Encoding utf8
    $lines | Write-Output
    "Kept in $Out"
    return
}
foreach ($film in $Films) {
    $name = if ($JetKvm) { $jetName } else { "$film-$Decoding-$Card" }
    $report = Join-Path $Out "$name.json"
    if ($failed.ContainsKey($film)) { $lines += "| $film | $Decoding | $Card | $($failed[$film]) | | | | | | | |"; continue }
    if (-not (Test-Path -LiteralPath $report)) { $lines += "| $film | $Decoding | $Card | no report | | | | | | | |"; continue }
    try {
        $r = Get-Content -LiteralPath $report -Raw | ConvertFrom-Json
    } catch {
        $lines += "| $film | $Decoding | $Card | a report that could not be read | | | | | | | |"
        continue
    }
    if ($r.PSObject.Properties['problem']) { $lines += "| $film | $Decoding | $Card | $($r.problem) | | | | | | | |"; continue }
    $counts = @($r.arrivedToShown); $all = [math]::Max(1, ($counts | Measure-Object -Sum).Sum)
    $changes = @($r.unevenCounts); $changed = [math]::Max(1, ($changes | Measure-Object -Sum).Sum)
    $notes = @()
    if ($r.endedEarly) { $notes += 'cut short' }
    if (-not $r.windowActive) { $notes += 'window not in front' }
    if ($r.streamChanged) { $notes += 'stream changed' }
    $lines += ('| {0} | {1} | {2} | {3:N0} of {4:N0} | {5} of {6} | {7:N1} | {8:N1} % | {9:N1} % | {10:N1} % | {11:N0} Hz | {12} |' -f $film,
        $r.decoding, $Card, $r.shownPerSecond, $r.receivedPerSecond, ($r.replaced + $r.droppedBehind + $r.refused), $r.received,
        $r.delayMeanMs, (100 * $counts[0] / $all), (100 * ($counts[0] + $counts[1]) / $all),
        (100 * ((@($changes[3..7]) | Measure-Object -Sum).Sum) / $changed), $r.screenHz, ($notes -join ', '))
}
$lines += ''
$lines += 'Targets (SPEC.md section 9): on the card, a mean under 2 ms and 98 in 100 within 4 ms; in software at 1080p, under 4 ms;'
$lines += 'every picture that arrives shown; under 1 in 100 pictures taking more than 8 ms longer or shorter to the screen than the one before.'
$lines | Set-Content -LiteralPath (Join-Path $Out 'summary.md') -Encoding utf8
$lines | Write-Output
"Kept in $Out"
