#Requires -Version 7.2
<#
.SYNOPSIS
Builds libFuzzer's targets of the readers of what a device or a server sends (DESIGN.md section 8 names them), and runs
each for a bounded time.

.DESCRIPTION
    tools\fuzz.ps1 [-Minutes 6] [-Seconds n] [-Only fuzz_rfb,fuzz_jpeg] [-NoBuild] [-FastCores]
    tools\fuzz.ps1 -Canary [-Smallest] [-Seconds n] [-Only ...] [-NoBuild]

The gate's fuzz step (tools\check.ps1, with -FastCores). The fuzz preset is configured and built (build\fuzz: the
address sanitizer's build with QKVM_FUZZ, its targets and their seeds, which the seed maker makes from the tests' own
inputs), on every processor. Then every target runs, as many at once as half the processors this PowerShell may use
(-FastCores: held first to the fast cores of a PC of two kinds, as the gate holds its tests, Use-FastCores), each for
as long as -Minutes allows over the rounds that takes (or -Seconds each), on a corpus of its own that grows from its
seeds and is kept from run to run (build\fuzz\fuzz\corpus\<seeds>). A corpus of more than 2000 inputs is merged first
(libFuzzer's -merge=1: the inputs that add nothing go), so that a target's start, which runs its whole corpus before
its time counts, does not grow from gate to gate.

Anything a target finds (a crash, a broken promise, an input that takes longer than -timeout, an allocation larger
than its MALLOC_MB, memory past -rss_limit_mb) is kept as libFuzzer writes it, in build\fuzz\fuzz\findings\<target>,
with the target's whole log in build\fuzz\fuzz\logs: the step fails, and says which and where, and which of them an
earlier run found (they stay, and fail every run, until they are taken away once each is a row of
test_core_fuzz_found and what it found is fixed: DESIGN.md section 8). An input slower than 25 seconds that is no
timeout (libFuzzer's slow-unit-*, -report_slow_units) is said, and kept, but is no finding: a busy PC makes them. What
each target did is said on its line: its runs and seconds, and libFuzzer's coverage of its reader (cov, the edges; ft,
the features) and corpus from its seeds to its end.

-Canary: the proof that each target reaches deep into its reader, rather than a run of the gate's. The fuzz-canary
preset is built instead (build\fuzz-canary), whose harnesses each end the run where their reader read an input to the
deep state each names (base/include/qkvm/base/fuzz_promise.h, reached). Each target's seeds are first read alone (none
may reach it), then it is fuzzed from them (-Smallest: from its one small seed instead) on a corpus made anew, until it
finds its canary or its time is up. A target passes where it found its canary, and nothing else, within its time: its
line says when, after how many runs, and which input (in build\fuzz-canary\fuzz\findings\<target>).

Exit 0: every target ran its time and found nothing (with -Canary: found its canary, and nothing else). 1: one found
something (with -Canary: not its canary, or nothing), or ended otherwise than libFuzzer's own end, or could not be
started, or fuzzed nothing (no INITED line in its log, or no run). 2: the build failed.
#>
param(
    [ValidateRange(1, 600)] [int] $Minutes = 6,
    [ValidateRange(0, 86400)] [int] $Seconds = 0,
    [string[]] $Only = @(),
    [switch] $NoBuild,
    [switch] $Canary,
    [switch] $Smallest,
    [switch] $FastCores)

. "$PSScriptRoot\common.ps1"
Enter-BuildEnvironment
$ErrorActionPreference = 'Continue'

if ($Smallest -and -not $Canary) { Write-Output '-Smallest is a way of the canary proof: give -Canary with it.'; exit 2 }
$preset = if ($Canary) { 'fuzz-canary' } else { 'fuzz' }
$build = Join-Path $DesktopRoot "build\$preset"
$fuzz = Join-Path $build 'fuzz'
Push-Location $DesktopRoot
try {
    if (-not $NoBuild) {
        $began = Get-Date
        cmake --preset $preset | Out-Null
        if ($LASTEXITCODE -ne 0) { Write-Output "The $preset preset could not be configured (exit $LASTEXITCODE)."; exit 2 }
        cmake --build --preset $preset
        if ($LASTEXITCODE -ne 0) { Write-Output "The fuzz targets could not be built (exit $LASTEXITCODE)."; exit 2 }
        Write-Output ("Built in {0:N0} s." -f ((Get-Date) - $began).TotalSeconds)
    }
} finally {
    Pop-Location
}

# name|program|longest input|most MB at once|seeds, one a line (CMakeLists.txt, qkvm_add_fuzzer).
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$targets = @(Get-Content -LiteralPath (Join-Path $fuzz 'fuzzers.txt') | Where-Object { $_ } | ForEach-Object {
        $name, $program, $longest, $most, $seeds = $_ -split '\|'
        [pscustomobject]@{ Name = $name; Program = $program; Longest = $longest; Most = $most; Seeds = $seeds }
    } | Where-Object { $Only.Count -eq 0 -or $Only -contains $_.Name })
if ($targets.Count -eq 0) { Write-Output 'No fuzz target to run.'; exit 1 }

# As many at once as half the processors this may use, and as many rounds as that takes within -Minutes. The gate's runs
# on the fast cores of a PC of two kinds, as its tests (Use-FastCores), and as the step's time was measured: on the box
# eight at once on its sixteen (outside review of FUZZ, L3; the build above had every core).
if ($FastCores) { Use-FastCores 6>&1 }
$mask = [long] [Diagnostics.Process]::GetCurrentProcess().ProcessorAffinity
$processors = 0
for ($bit = 0; $bit -lt 64; $bit++) { if ($mask -band ([long] 1 -shl $bit)) { $processors++ } }
$atOnce = [Math]::Max(1, [Math]::Floor($processors / 2))
$rounds = [Math]::Ceiling($targets.Count / $atOnce)
$each = if ($Seconds -gt 0) { $Seconds } else { [Math]::Max(30, [Math]::Floor($Minutes * 60 / $rounds)) }
$from = if ($Smallest) { 'smallest' } else { 'seeds' }
Write-Output "$($targets.Count) fuzz targets$(if ($Canary) { ", each after its canary from its $from" }), $atOnce at once on $processors processors: $each s each, in $rounds round(s)."

# The address sanitizer's runtime is beside the compiler; Qt's and FFmpeg's DLLs where CTest finds them.
$env:PATH = "$(Split-Path (Get-Command cl).Source);$(Get-QtRoot)\bin;$(Join-Path $DesktopRoot '.deps\ffmpeg\bin');$env:PATH"
foreach ($folder in 'corpus', 'findings', 'logs') { New-Item -ItemType Directory -Force -Path (Join-Path $fuzz $folder) | Out-Null }

function Read-Log([string[]] $said) {
    <# What libFuzzer said of a run: its runs, and its features (ft: MSVC's libFuzzer counts its inline 8-bit counters,
    and says no cov) and corpus at its start and at its end. #>
    $runs = $said | Select-String -Pattern 'stat::number_of_executed_units:\s*(\d+)' | Select-Object -Last 1
    $last = $said | Select-String -Pattern '^#(\d+)\s' | Select-Object -Last 1
    $runs = if ($runs) { [long] $runs.Matches[0].Groups[1].Value } elseif ($last) { [long] $last.Matches[0].Groups[1].Value } else { 0 }
    $inited = $said | Select-String -Pattern '^#\d+\s+INITED (?:cov: \d+ )?ft: (\d+) corp: (\d+)' | Select-Object -First 1
    $latest = $said | Select-String -Pattern '^#\d+\s+\w+\s+(?:cov: \d+ )?ft: (\d+) corp: (\d+)' | Select-Object -Last 1
    $grew = if ($inited -and $latest) {
        $a = $inited.Matches[0].Groups; $b = $latest.Matches[0].Groups
        'ft {0} to {1}, corpus {2} to {3}' -f $a[1].Value, $b[1].Value, $a[2].Value, $b[2].Value
    } else { 'no features said' }
    # (Inited: libFuzzer read its corpus and began. A target with no INITED line, or no run, did not fuzz at all.)
    [pscustomobject]@{ Runs = $runs; Grew = $grew; Inited = [bool] $inited }
}

function Start-Target([string] $program, [string[]] $arguments, [string] $log, [switch] $Wait) {
    <# A target started, its log beside it; nothing (and why, said) where it could not be started: an exe missing under
    -NoBuild, or one an antivirus holds. Never the process of a target before (outside review of FUZZ, L2). #>
    try {
        return Start-Process -FilePath $program -ArgumentList $arguments -NoNewWindow -PassThru -Wait:$Wait `
            -RedirectStandardError $log -RedirectStandardOutput "$log.out" -ErrorAction Stop
    } catch {
        Write-Output "$(Split-Path -Leaf $program): could not be started ($($_.Exception.Message))"
        return $null
    }
}

$failed = 0
if ($Canary) {
    # Each target's seeds alone first, without a mutation: none may reach its canary, or finding it proves nothing.
    foreach ($target in $targets) {
        $alone = Join-Path $fuzz "logs\$($target.Name)-seeds.log"
        # (What it found kept beside the run's own findings, never where this was started.)
        $kept = Join-Path $fuzz "findings\$($target.Name)-seeds"
        Remove-Item -LiteralPath $kept -Recurse -Force -ErrorAction Ignore
        New-Item -ItemType Directory -Force -Path $kept | Out-Null
        $process = Start-Target $target.Program @('-runs=0', "-max_len=$($target.Longest)",
            "-malloc_limit_mb=$($target.Most)", "-artifact_prefix=$kept\", (Join-Path $fuzz "$from\$($target.Seeds)")) $alone -Wait
        if ($process -isnot [Diagnostics.Process]) {
            $failed++
            $process | Where-Object { $_ -is [string] }
            continue
        }
        if ($process.ExitCode -ne 0) {
            $failed++
            Write-Output "$($target.Name): its $from alone end otherwise than well (exit $($process.ExitCode)); log $alone"
            Get-Content -LiteralPath $alone | Select-String -Pattern 'the canary found|a promise broken|ERROR: ' |
                Select-Object -First 4 | ForEach-Object { "      $($_.Line)" }
        }
    }
    if ($failed -gt 0) { Write-Output "$failed target(s) reach their canary, or fail, on their $from alone."; exit 1 }
}

# A corpus kept from run to run, merged where it has grown past its most: libFuzzer runs the whole of it at a target's
# start, before -max_total_time counts, so a corpus that only grows makes the step longer at every gate (outside review
# of FUZZ, L4). Where the merge does not end well, the corpus stays as it is, and the run says what it finds in it.
$mostCorpus = 2000
if (-not $Canary) {
    foreach ($target in $targets) {
        $corpus = Join-Path $fuzz "corpus\$($target.Seeds)"
        $had = @(Get-ChildItem -LiteralPath $corpus -File -ErrorAction SilentlyContinue).Count
        if ($had -le $mostCorpus) { continue }
        $merged = "$corpus.merged"
        Remove-Item -LiteralPath $merged -Recurse -Force -ErrorAction Ignore
        New-Item -ItemType Directory -Force -Path $merged | Out-Null
        $began = Get-Date
        $process = Start-Target $target.Program @('-merge=1', "-max_len=$($target.Longest)",
            "-malloc_limit_mb=$($target.Most)", '-rss_limit_mb=2560', '-timeout=25', $merged, $corpus) `
            (Join-Path $fuzz "logs\$($target.Name)-merge.log") -Wait
        $has = @(Get-ChildItem -LiteralPath $merged -File -ErrorAction SilentlyContinue).Count
        if ($process -is [Diagnostics.Process] -and $process.ExitCode -eq 0 -and $has -gt 0) {
            Remove-Item -LiteralPath $corpus -Recurse -Force
            Move-Item -LiteralPath $merged -Destination $corpus
            Write-Output ('{0}: its corpus merged from {1} inputs to {2} in {3:N0} s.' -f $target.Name, $had, $has, ((Get-Date) - $began).TotalSeconds)
        } else {
            Remove-Item -LiteralPath $merged -Recurse -Force -ErrorAction Ignore
            Write-Output "$($target.Name): its corpus of $had inputs could not be merged, and is kept as it is."
        }
    }
}

$running = [System.Collections.Generic.List[object]]::new()
$ended = [System.Collections.Generic.List[object]]::new()
foreach ($target in $targets) {
    while ($running.Count -ge $atOnce) {
        $done = @($running | Where-Object { $_.Process.HasExited })
        if ($done.Count -eq 0) { Start-Sleep -Milliseconds 500; continue }
        foreach ($one in $done) { [void] $running.Remove($one); $ended.Add($one) }
    }
    $corpus = Join-Path $fuzz "corpus\$($target.Seeds)"
    $findings = Join-Path $fuzz "findings\$($target.Name)"
    $log = Join-Path $fuzz "logs\$($target.Name).log"
    if ($Canary) {
        # (A corpus made anew: what an earlier proof found is not to be begun from.)
        Remove-Item -LiteralPath $corpus, $findings -Recurse -Force -ErrorAction Ignore
    }
    New-Item -ItemType Directory -Force -Path $corpus, $findings | Out-Null
    # (-len_control=0: libFuzzer would otherwise mutate within a length that grows from the shortest seed's over the
    # run, cutting every longer seed to it, so that a short run never mutates a whole update, JPEG or stream; each
    # target's own longest, MAX_LEN, holds it instead. Seen 2026-11-17: no canary of rfb, rfb_inflate and redfish within
    # 120 s at "lim: 362" and "lim: 914", their seeds being longer.)
    # (-report_slow_units=25: an input as slow as that is said; one slower than -timeout is a finding.)
    $arguments = @($corpus, (Join-Path $fuzz "$from\$($target.Seeds)"), "-max_total_time=$each", '-timeout=25',
        '-report_slow_units=25', '-rss_limit_mb=2560', "-malloc_limit_mb=$($target.Most)", "-max_len=$($target.Longest)",
        '-len_control=0', '-print_final_stats=1', "-artifact_prefix=$findings\")
    $process = Start-Target $target.Program $arguments $log
    if ($process -isnot [Diagnostics.Process]) {
        # (Not started: it fails the step, and takes no place among those running.)
        $failed++
        $process | Where-Object { $_ -is [string] }
        continue
    }
    $running.Add([pscustomobject]@{ Target = $target; Process = $process; Log = $log; Findings = $findings; Began = Get-Date })
}
foreach ($one in $running) { $one.Process.WaitForExit(); $ended.Add($one) }

foreach ($one in ($ended | Sort-Object { $_.Target.Name })) {
    $one.Process.WaitForExit()
    $code = $one.Process.ExitCode
    $said = @(Get-Content -LiteralPath $one.Log -ErrorAction SilentlyContinue)
    $read = Read-Log $said
    $took = [int] ($one.Process.ExitTime - $one.Process.StartTime).TotalSeconds
    # (What it kept: its findings, and its slow inputs, which a busy PC makes, said and no finding: L4.)
    $all = @(Get-ChildItem -LiteralPath $one.Findings -File -ErrorAction SilentlyContinue)
    $kept = @($all | Where-Object { $_.Name -notlike 'slow-unit-*' })
    $slow = @($all | Where-Object { $_.Name -like 'slow-unit-*' -and $_.LastWriteTime -ge $one.Began })
    $earlier = @($kept | Where-Object { $_.LastWriteTime -lt $one.Began } | Sort-Object LastWriteTime)
    $line = '{0,-22} {1,12:N0} runs in {2,4} s, exit {3}; {4}' -f $one.Target.Name, $read.Runs, $took, $code, $read.Grew
    if ($slow.Count -gt 0) { $line += "; $($slow.Count) slow input(s), no finding, in $($one.Findings)" }
    if ($Canary) {
        $found = @($said | Select-String -Pattern '^the canary found: (.*)$')
        $other = @($said | Select-String -Pattern 'a promise broken|ERROR: AddressSanitizer|ERROR: libFuzzer: (?!deadly signal)')
        if ($found.Count -eq 1 -and $other.Count -eq 0 -and $kept.Count -eq 1) {
            Write-Output "$line`n      canary found: $($found[0].Matches[0].Groups[1].Value) ($($kept[0].FullName))"
        } else {
            $failed++
            Write-Output "$line  *** NO CANARY$(if ($other.Count) { ', and something else found' }): log $($one.Log)"
            $other | Select-Object -First 4 | ForEach-Object { "      $($_.Line)" }
        }
    } elseif ($code -ne 0 -or $kept.Count -gt 0 -or -not $read.Inited -or $read.Runs -eq 0) {
        $failed++
        # (One that ran nothing, or never began on its corpus, fuzzed nothing: no pass, outside review of FUZZ, L2.)
        if (-not $read.Inited -or $read.Runs -eq 0) {
            Write-Output "$line  *** DID NOT FUZZ: no INITED line, or no run; log $($one.Log)"
        }
        if ($code -ne 0 -or $kept.Count -gt 0) {
            Write-Output "$line  *** FOUND: $($kept.Count) input(s) in $($one.Findings); log $($one.Log)"
        }
        if ($earlier.Count -gt 0) {
            # (Kept from a run before: they fail every run until they are taken away, once each is a row of
            # test_core_fuzz_found and what it found is fixed. Said so, not taken for a new finding: L4.)
            Write-Output ("      {0} of them from an earlier run, the first of {1:yyyy-MM-dd HH:mm}: {2}" -f $earlier.Count,
                $earlier[0].LastWriteTime, (($earlier | Select-Object -First 3 | ForEach-Object Name) -join ', '))
        }
        $said | Select-String -Pattern 'ERROR: |a promise broken|SUMMARY: |deadly signal|Test unit written' |
            Select-Object -First 8 | ForEach-Object { "      $($_.Line)" }
    } else {
        Write-Output $line
    }
}
if ($failed -gt 0) {
    if ($Canary) { Write-Output "$failed fuzz target(s) did not find their canary alone within their time." }
    else { Write-Output "$failed fuzz target(s) found something, or did not fuzz: what and where is said above." }
    exit 1
}
Write-Output $(if ($Canary) { "Every fuzz target found its canary from its $from, and nothing else." } else { 'No fuzz target found anything.' })
exit 0
