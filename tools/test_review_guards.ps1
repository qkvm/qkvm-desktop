# Review-follow-up policy checks: synthetic evidence, no profile or network access.
#
#   tools\test_review_guards.ps1
#       What the files say: the gate's retry runs one program at a time, the gate and every build run
#       these guards and the margin step, the documents name the Credential Manager boundary, and the
#       WebRTC receiver calls its library only with none of its own locks held (M-D4).
#   tools\test_review_guards.ps1 -BuildDir <build folder> -Config <configuration> -Ctest <ctest>
#       All of the above, and what a build does (L-3): the limits CTest itself will apply there, read
#       from CTest. CTest runs this in every preset (CMakeLists.txt, test_review_guards), so taking the
#       guard step out of the gate is seen by every build's own tests, and a limit that a later line
#       changes, which the text never shows, is seen too.
param([string] $BuildDir = '', [string] $Config = '', [string] $Ctest = 'ctest')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$problems = [Collections.Generic.List[string]]::new()

# (Read with its block comments taken out: a line inside <# ... #> runs nothing. A line inside a branch
# that never runs, such as if ($false), is beyond what text can tell; every build's own test run is what
# sees the guards run at all.)
$gate = (Get-Content -LiteralPath "$root/tools/check.ps1" -Raw) -replace '(?s)<#.*?#>', ''
# The first run's results are new for each run: deleted before CTest writes them, and read by the margin
# step only where written after the run began (round 3, M1).
if ($gate -notmatch '(?m)^\s*Remove-Item -LiteralPath \$first -ErrorAction Ignore\s*$' -or
    $gate -notmatch '(?m)^\s*\$script:testsBegan = Get-Date\s*$' -or
    $gate -notmatch '(?m)^\s*ctest --preset \$preset --output-junit \$first\s*$') {
    $problems.Add('L-1: tools/check.ps1 no longer writes each run''s first results anew.')
}
# -j1 as an option of its own: -j16 is not it (L-3, G2).
if ($gate -notmatch '(?m)^\s*ctest --preset \$preset --rerun-failed --output-on-failure -j1(?![\w])') {
    $problems.Add('T-1: a retry must explicitly use -j1 even when CTEST_PARALLEL_LEVEL is 4.')
}
# The gate runs these guards and the margin step, on lines of their own that are not commented out
# (L-3, G5; round 2, M1).
if ($gate -notmatch "(?m)^\s*Step 'review follow-up guards' \{ & \(Join-Path \`$PSScriptRoot 'test_review_guards\.ps1'\) \}") {
    $problems.Add('L-3: tools/check.ps1 no longer runs the review follow-up guards.')
}
if ($gate -notmatch "(?m)^\s*Step `"time margins \`$preset`" \{ & \(Join-Path \`$PSScriptRoot 'test_time_margins\.ps1'\) -Preset \`$preset -Since \`$script:testsBegan \}") {
    $problems.Add('L-1: tools/check.ps1 no longer looks at how close each program came to its limit.')
}
# The gate fuzzes the readers of what a device sends, on a line of its own, for its bounded time and not as the canary
# proof (whose finding of a canary passes); the step fails on what a target finds, and the gate's preset is never the
# canaries' (FUZZ, 2026-11-17).
# The step stands among the gate's own (indented as they are, not in a branch of its own), runs fuzz.ps1 for its time,
# and fails where fuzz.ps1 ends well without its last line; and the gate fails where the step never ran at all, in
# whatever branch (outside review of FUZZ, L1).
$fuzzStepOfTheGate = '(?m)^ {8}Step ''fuzz'' \{\r?\n' +
    '\s*pwsh -NoProfile -File \(Join-Path \$PSScriptRoot ''fuzz\.ps1''\) -Minutes \d+ -FastCores \| ForEach-Object \{ \$_; \$last = \$_ \}\r?\n' +
    '\s*if \(\$LASTEXITCODE -eq 0 -and \$last -ne ''No fuzz target found anything\.''\) \{\r?\n' +
    '[^\r\n]*\r?\n' +
    '\s*\$global:LASTEXITCODE = 1\r?\n'
if ($gate -notmatch $fuzzStepOfTheGate) {
    $problems.Add('FUZZ: tools/check.ps1 no longer runs the fuzz targets among its steps (Step ''fuzz'', tools/fuzz.ps1 -Minutes n), or no longer fails where fuzz.ps1 does not end saying that nothing was found.')
}
if ($gate -notmatch "(?m)^\s*foreach \(\`$must in 'fuzz', 'review follow-up guards'\) \{\r?\n\s*if \(-not \(\`$steps \| Where-Object \{ \`$_ -like `"\`$\{must\}: \*`" \}\)\) \{\r?\n\s*\`$failed\.Add\(") {
    $problems.Add('FUZZ: tools/check.ps1 no longer fails where its fuzz step, or its guards, never ran.')
}
$fuzzStep = (Get-Content -LiteralPath "$root/tools/fuzz.ps1" -Raw) -replace '(?s)<#.*?#>', ''
# The gate's own branch: a target that exited otherwise than well, kept an input, or fuzzed nothing, fails the step; the
# step's end exits 1 on any failure, and says its last line only after that (outside review of FUZZ, L1 and L2).
$gateBranch = '(?m)^\s*\} elseif \(\$code -ne 0 -or \$kept\.Count -gt 0 -or -not \$read\.Inited -or \$read\.Runs -eq 0\) \{\r?\n\s*\$failed\+\+\s*$'
$theEnd = '(?m)^if \(\$failed -gt 0\) \{\r?\n(?:[^\r\n]*\r?\n){2}\s*exit 1\r?\n\}\r?\n' +
    'Write-Output \$\(if \(\$Canary\) \{ "[^"]*" \} else \{ ''No fuzz target found anything\.'' \}\)\r?\nexit 0\s*$'
if ($fuzzStep -notmatch $gateBranch -or $fuzzStep -notmatch $theEnd -or
    $fuzzStep -notmatch "\`$preset = if \(\`$Canary\) \{ 'fuzz-canary' \} else \{ 'fuzz' \}") {
    $problems.Add('FUZZ: tools/fuzz.ps1 no longer fails on a finding or on a target that fuzzed nothing, or no longer keeps the canaries to their own preset.')
}
# The gate's runs held to the fast cores where a PC has two kinds (L3), after the build; slow inputs said and no finding,
# the rest counted however old (L4).
if ($fuzzStep -notmatch '(?m)^if \(\$FastCores\) \{ Use-FastCores 6>&1 \}\s*$' -or
    $fuzzStep -notmatch '(?m)^\s*\$kept = @\(\$all \| Where-Object \{ \$_\.Name -notlike ''slow-unit-\*'' \}\)\s*$') {
    $problems.Add('FUZZ: tools/fuzz.ps1 no longer holds the gate''s runs to the fast cores, or no longer counts every finding but a slow input.')
}
# A target that cannot be started fails the step, and no other target's process stands in for it (L2): both where its
# seeds are read alone and where it is fuzzed.
$notStarted = '(?m)^\s*if \(\$process -isnot \[Diagnostics\.Process\]\) \{\r?\n(?:\s*#[^\r\n]*\r?\n)?\s*\$failed\+\+\s*$'
if ([regex]::Matches($fuzzStep, $notStarted).Count -lt 2 -or $fuzzStep -notmatch '-ErrorAction Stop') {
    $problems.Add('FUZZ: tools/fuzz.ps1 no longer fails where a target cannot be started.')
}
$fuzzPresets = Get-Content -LiteralPath "$root/CMakePresets.json" -Raw | ConvertFrom-Json
$gatePreset = @($fuzzPresets.configurePresets | Where-Object name -eq 'fuzz')
if ($gatePreset.Count -ne 1 -or $gatePreset[0].cacheVariables.QKVM_FUZZ -ne 'ON' -or $gatePreset[0].cacheVariables.QKVM_FUZZ_CANARY -ne 'OFF') {
    $problems.Add('FUZZ: the fuzz preset is not QKVM_FUZZ ON with QKVM_FUZZ_CANARY OFF in so many words.')
}
$top = Get-Content -LiteralPath "$root/CMakeLists.txt" -Raw
if ($top -notmatch '(?m)^add_test\(NAME test_review_guards\s') {
    $problems.Add('L-3: CMakeLists.txt no longer registers test_review_guards.')
}
foreach ($name in 'README.md','SPEC.md') {
    $doc = (Get-Content -LiteralPath "$root/$name" -Raw) -replace '\s+', ' '
    if ($doc -notmatch "no control through qKVM.s interface alone; with the Windows account, a Credential Manager Restore can replace the app password record and its retry count") {
        $problems.Add("N-1: $name does not name the Restore boundary precisely.")
    }
    if ($doc -notmatch 'restoring an older backup resets the wrong-try count') {
        $problems.Add("N-1: $name does not state the older-backup retry reset.")
    }
}
# M-D4, rounds 2 and 3 (L4, L3), over a list since M6 2b: in the files that speak to libdatachannel no
# lock is taken but through Held (the mailboxes' and the registries' alike), whose one member line stands
# once among them all, in rtc_seam.h; and every call into the library stands on a line of its own right
# after intoLibrary(), which counts the calls made under any of them for the test. No other file of the
# program includes the library or calls it (base, core/src, core/include, ui, engine, bench, input); the far
# ends of the tests in core/tests are not among them, and are named here as what they are.
$speaking = @('core/src/rtc_seam.h', 'core/src/webrtc_receiver.cpp', 'core/src/webrtc_offer.cpp')
$farEnds = @('core/tests/janus_by_hand.h', 'core/tests/film_sender.h', 'core/tests/jetkvm_peer_by_hand.h')
$heldMember = 'std::lock_guard<std::mutex> m_lock;'
$anyLock = '\b(lock_guard|unique_lock|scoped_lock|shared_lock)\b|\bstd::(try_)?lock\(|(\.|->)(try_)?lock(_shared)?(_for|_until)?\(|' +
    'QMutexLocker|QReadLocker|QWriteLocker|EnterCriticalSection|AcquireSRWLock'
function Find-LibraryProblems([hashtable] $sources) {
    <# What the rule above finds in a set of sources: relative path to its lines. #>
    $found = [Collections.Generic.List[string]]::new()
    $members = 0
    foreach ($name in $speaking) {
        $lines = @($sources[$name])
        if ($lines.Count -eq 0) {
            $found.Add("M-D4: $name, which speaks to the library, is not there.")
            continue
        }
        $here = @($lines | Where-Object { $_.Trim() -eq $heldMember }).Count
        $members += $here
        if ($here -ne 0 -and $name -ne 'core/src/rtc_seam.h') {
            $found.Add("M-D4: $name has Held's lock member, which is rtc_seam.h's alone.")
        }
        $before = ''
        for ($at = 0; $at -lt $lines.Count; $at++) {
            $line = $lines[$at].Trim()
            if ($line -eq '' -or $line.StartsWith('//') -or $line.StartsWith('#include')) { continue }
            if ($line -match $anyLock -and $line -ne $heldMember) {
                $found.Add("M-D4: ${name}:$($at + 1) takes a lock that is not Held: $line")
            }
            $calls = [regex]::Matches($line, '\brtc[A-Z]\w*\(').Count
            if ($calls -gt 1) {
                $found.Add("M-D4: ${name}:$($at + 1) calls the library more than once on a line.")
            } elseif ($calls -eq 1 -and $before -ne 'intoLibrary();') {
                $found.Add("M-D4: ${name}:$($at + 1) calls the library without intoLibrary() just before.")
            }
            $before = $line
        }
    }
    if ($members -ne 1) {
        $found.Add("M-D4: Held's lock member stands $members times among the files that speak to the library, not once.")
    }
    foreach ($name in $sources.Keys | Where-Object { $speaking -notcontains $_ } | Sort-Object) {
        $lines = @($sources[$name])
        for ($at = 0; $at -lt $lines.Count; $at++) {
            $line = $lines[$at].Trim()
            if ($line -match '^#\s*include\s*[<"]rtc/') {
                $found.Add("M-D4: ${name}:$($at + 1) includes the WebRTC library, which only the files that speak to it may.")
            } elseif (($line -replace '//.*$', '') -match '\brtc[A-Z]\w*\(') {
                $found.Add("M-D4: ${name}:$($at + 1) calls the WebRTC library, which only the files that speak to it may.")
            }
        }
    }
    return , $found
}
$sources = @{}
foreach ($part in 'base', 'core/src', 'core/include', 'ui', 'engine', 'bench', 'input') {
    foreach ($file in Get-ChildItem -LiteralPath "$root/$part" -Recurse -File -Include *.h, *.cpp) {
        $sources[[IO.Path]::GetRelativePath($root, $file.FullName).Replace('\', '/')] = @(Get-Content -LiteralPath $file.FullName)
    }
}
foreach ($found in (Find-LibraryProblems $sources)) { $problems.Add($found) }
foreach ($name in $farEnds) {
    if (-not (Test-Path -LiteralPath "$root/$name")) { $problems.Add("M-D4: the far end $name named here is not there.") }
}
# The rule tried on a scratch copy of the sources, in memory: each of these is refused, or the guard is
# blind to it. (A call into the library from kvmd_live.cpp, the library included there, a call in the
# receiver without intoLibrary() before it, a second lock in the receiver, Held's member in the receiver.)
$receiverName = 'core/src/webrtc_receiver.cpp'
$tries = @(
    @{ Name = 'a call from kvmd_live.cpp'; File = 'core/src/kvmd_live.cpp'; Change = { param($l) $l + 'void scratch() { rtcCreatePeerConnection(nullptr); }' } }
    @{ Name = 'the library included in kvmd_live.cpp'; File = 'core/src/kvmd_live.cpp'; Change = { param($l) @('#include <rtc/rtc.h>') + $l } }
    @{ Name = 'a call from base'; File = 'base/include/qkvm/base/clock.h'; Change = { param($l) $l + 'inline void scratch() { rtcCleanup(); }' } }
    @{ Name = 'an intoLibrary() taken out'; File = $receiverName; Change = { param($l) $i = [array]::IndexOf(@($l | ForEach-Object Trim), 'intoLibrary();'); @($l[0..($i - 1)]) + @($l[($i + 1)..($l.Count - 1)]) } }
    @{ Name = 'a second lock'; File = $receiverName; Change = { param($l) $l + 'void scratch(std::mutex& m) { const std::lock_guard<std::mutex> other(m); }' } }
    @{ Name = 'Held''s member in the receiver'; File = $receiverName; Change = { param($l) $l + "    $heldMember" } }
)
foreach ($try in $tries) {
    if (-not $sources.ContainsKey($try.File)) {
        $problems.Add("M-D4: the guard does not look at $($try.File), which it is tried on.")
        continue
    }
    $copy = $sources.Clone()
    $copy[$try.File] = @(& $try.Change @($sources[$try.File]))
    if ((Find-LibraryProblems $copy).Count -eq 0) {
        $problems.Add("M-D4: the guard does not see $($try.Name) in a scratch copy.")
    }
}

if ($BuildDir) {
    # The screens programs' limits (ui/CMakeLists.txt says how each was come to): measured in Debug, and
    # Debug's borrowed for the address sanitizer (RelWithDebInfo) until they are measured there. The one
    # program they were is no more, and is not to come back unmeasured.
    $screens = @{
        test_ui_wall_screens = @{ Debug = 840; RelWithDebInfo = 840; Release = 600 }
        test_ui_bar_screens = @{ Debug = 1050; RelWithDebInfo = 1050; Release = 600 }
        test_ui_console_screens = @{ Debug = 1020; RelWithDebInfo = 1020; Release = 600 }
        test_ui_box_screens = @{ Debug = 700; RelWithDebInfo = 700; Release = 600 }
        # The two boxes' screens (PERF, 2026-11-15; ui/CMakeLists.txt).
        test_ui_own_files_screens_use = @{ Debug = 300; RelWithDebInfo = 300; Release = 600 }
        test_ui_own_files_screens_hold = @{ Debug = 280; RelWithDebInfo = 280; Release = 600 }
        test_ui_own_files_screens_room = @{ Debug = 270; RelWithDebInfo = 270; Release = 600 }
        test_ui_own_files_screens_looks = @{ Debug = 320; RelWithDebInfo = 320; Release = 600 }
    }
    $shown = & $Ctest --test-dir $BuildDir --show-only=json-v1 | ConvertFrom-Json
    if (@($shown.tests | Where-Object name -eq 'test_ui_screens').Count -ne 0) {
        $problems.Add('T-2/L-1: CTest has test_ui_screens again, which has no measured limit.')
    }
    if (@($shown.tests | Where-Object name -eq 'test_ui_own_files_screens').Count -ne 0) {
        $problems.Add('T-2/L-1: CTest has test_ui_own_files_screens again, the one program of 598 s of its 600.')
    }
    foreach ($name in $screens.Keys | Sort-Object) {
        $limits = $screens[$name]
        if (-not $limits.ContainsKey($Config)) {
            $problems.Add("No measured limit is known for $name in the configuration '$Config'.")
            continue
        }
        $howCome = if ($Config -eq 'RelWithDebInfo') { 'Debug''s, borrowed until measured here' } else { 'measured' }
        $test = @($shown.tests | Where-Object name -eq $name)
        $limit = @($test.properties | Where-Object name -eq 'TIMEOUT' | ForEach-Object value)
        if ($test.Count -ne 1 -or $limit.Count -ne 1 -or [double] $limit[0] -ne $limits[$Config]) {
            $problems.Add("T-2/L-1: CTest gives $name a limit of '$($limit -join ',')' s in $Config; $($howCome): $($limits[$Config]) s.")
        }
    }
}

# Rounds 6 and 7 of the smooth-video stage: whoever holds a stream's decoding lets go of it through
# letGoOfDecoding, which looks whether a packet waits only after letting go: a packet that came while
# it was held, whose wake found it taken, would lie there until the next one came. So the lock is
# only ever held by a unique_lock named `decoding`, never let go of by hand elsewhere, and in run()
# no return after it is held but through letGoOfDecoding. (PERF-2's stills' line: letGoOfDecoding gives
# back a card's place in the line first, then lets go of the decoding, then looks and hands in what waits
# through handIn, and does nothing more, to its closing brace; and handIn wakes the pool itself for every
# stream that is not a card's key frames. A place given back after the decoding is let go lets the next card
# and this one decode at once for a whole decoding, which a test sees only by chance: the review of abb20deb,
# L2.)
$stream = (Get-Content -LiteralPath "$root/engine/src/stream_core.cpp" -Raw) -replace '//[^\n]*', ''
if ($stream -notmatch '(?s)void StreamCore::letGoOfDecoding\(std::unique_lock<std::mutex>& decoding\)\s*\{\s*if \(keyFrameStills\(\)\) \{\s*m_gpu\.stillsGate\(\)\.leave\(m_member\);\s*\}\s*decoding\.unlock\(\);\s*bool more = false;\s*\{\s*const std::lock_guard guard\(m_inboxLock\);\s*more = !m_inbox\.empty\(\);\s*\}\s*if \(more\) \{\s*handIn\(\);\s*\}\s*\n\}(?=\n)') {
    $problems.Add('V-1: engine/src/stream_core.cpp letGoOfDecoding no longer gives back its place in the stills'' line, lets go, looks and hands in what waits, and nothing more.')
}
if ($stream -notmatch '(?s)void StreamCore::handIn\(bool fresh\)\s*\{\s*if \(gated\(\)\) \{\s*m_gpu\.stillsGate\(\)\.handIn\(m_member, fresh\);\s*\} else \{\s*m_gpu\.decodePool\(\)\.wake\(this\);\s*\}\s*\}') {
    $problems.Add('V-1: engine/src/stream_core.cpp handIn no longer wakes the pool itself for every stream but a card''s key frames.')
}
$holds = [regex]::Matches($stream, 'm_decodingLock')
$asUnique = [regex]::Matches($stream, 'std::unique_lock decoding\(m_decodingLock(, std::try_to_lock)?\);')
if ($holds.Count -ne $asUnique.Count) {
    $problems.Add('V-1: engine/src/stream_core.cpp takes m_decodingLock other than as a unique_lock named decoding.')
}
if ([regex]::Matches($stream, 'decoding\.unlock\(\)').Count -ne 1) {
    $problems.Add('V-1: engine/src/stream_core.cpp lets go of the decoding by hand outside letGoOfDecoding.')
}
# Every function of the file that takes the decoding ends with letGoOfDecoding, and returns early only
# through it (but where taking it did not succeed); no other file of the engine names the lock but
# the header that declares it, once.
$holders = 0
foreach ($function in [regex]::Matches($stream, '(?s)\n[^\n]*StreamCore::(\w+)\([^)]*\)[^\n{]*\n\{(.*?)\n\}(?=\n)')) {
    $body = $function.Groups[2].Value
    $taken = [regex]::Match($body, 'std::unique_lock decoding\(m_decodingLock(, std::try_to_lock)?\);')
    if (-not $taken.Success) { continue }
    $holders++
    $after = $body.Substring($taken.Index + $taken.Length) -replace '(?s)if \(!decoding\.owns_lock\(\)\) \{\s*return;\s*\}', ''
    if ([regex]::Matches($after, '(?<!letGoOfDecoding\(decoding\);\s*)return;').Count -ne 0) {
        $problems.Add("V-1: StreamCore::$($function.Groups[1].Value) returns, while it holds the decoding, other than through letGoOfDecoding.")
    }
    if ($after -notmatch 'letGoOfDecoding\(decoding\);\s*$') {
        $problems.Add("V-1: StreamCore::$($function.Groups[1].Value) does not end its hold of the decoding with letGoOfDecoding.")
    }
}
if ($holders -ne $asUnique.Count) {
    $problems.Add('V-1: engine/src/stream_core.cpp takes the decoding somewhere the guard does not see as a function of its own.')
}
foreach ($file in Get-ChildItem -LiteralPath "$root/engine" -Recurse -File -Include *.cpp,*.h) {
    $named = [regex]::Matches(((Get-Content -LiteralPath $file.FullName -Raw) -replace '//[^\n]*', ''), 'm_decodingLock').Count
    $expected = if ($file.Name -eq 'stream_core.cpp') { $named } elseif ($file.Name -eq 'stream_core.h') { 1 } else { 0 }
    if ($named -ne $expected) {
        $problems.Add("V-1: $($file.Name) names the decoding lock where only stream_core.cpp may take it.")
    }
}

# Every stall case has ports of its own (core/tests/signer_far_away.h): its table is tools/make_testdata.ps1's, which
# makes the certificates that name them, no port is two cases', and none is a port the profile fixtures' stand-in
# servers take (ui/tests/profile_maker.cpp), which the upgrade tests listen at again: the asker's case had 47391, and
# beside test_ui_upgrade_continuity one of the two failed (review arm 1, L7, 2026-11-21).
$stallTable = [ordered]@{}
foreach ($found in [regex]::Matches((Get-Content -LiteralPath "$root/core/tests/signer_far_away.h" -Raw),
        'inline constexpr Stalls \w+\{"stalls-([a-z0-9-]+)", \{(\d+), (\d+), (\d+)\}\};')) {
    $stallTable[$found.Groups[1].Value] = @(2..4 | ForEach-Object { [int] $found.Groups[$_].Value })
}
$madeTable = [ordered]@{}
$madeText = [regex]::Match((Get-Content -LiteralPath "$root/tools/make_testdata.ps1" -Raw), '(?s)\$stalls = \[ordered\] @\{(.*?)\}\r?\n').Groups[1].Value
foreach ($found in [regex]::Matches($madeText, "'([a-z0-9-]+)' = (\d+), (\d+), (\d+)")) {
    $madeTable[$found.Groups[1].Value] = @(2..4 | ForEach-Object { [int] $found.Groups[$_].Value })
}
$fixturePorts = @([regex]::Matches((Get-Content -LiteralPath "$root/ui/tests/profile_maker.cpp" -Raw),
        'constexpr quint16 (?:serverPort|screenServerPort) = (\d+);') | ForEach-Object { [int] $_.Groups[1].Value })
$stallPorts = @($stallTable.Values | ForEach-Object { $_ })
if ($stallTable.Count -lt 10 -or $fixturePorts.Count -ne 2) {
    $problems.Add('L7: the stall cases'' table (signer_far_away.h) or the fixtures'' servers'' ports (profile_maker.cpp) are no longer where this guard reads them.')
} elseif (($stallTable.Keys -join ',') -cne ($madeTable.Keys -join ',') -or
    @($stallTable.Keys | Where-Object { ($stallTable[$_] -join ',') -cne ($madeTable[$_] -join ',') }).Count) {
    $problems.Add('L7: the stall cases of core/tests/signer_far_away.h are not tools/make_testdata.ps1''s, name for name and port for port.')
} elseif (@($stallPorts | Group-Object | Where-Object Count -gt 1).Count) {
    $problems.Add("L7: a port is two stall cases': $(@($stallPorts | Group-Object | Where-Object Count -gt 1 | ForEach-Object Name) -join ', ').")
} elseif (@($stallPorts | Where-Object { $fixturePorts -contains $_ }).Count) {
    $problems.Add("L7: a stall case takes a port of the profile fixtures' stand-in servers: $(@($stallPorts | Where-Object { $fixturePorts -contains $_ }) -join ', ').")
}

if ($problems.Count) { $problems | Write-Output; exit 1 }
# (The screens programs' limits for the address sanitizer are Debug's, borrowed: said so.)
$limitsSaid = if ($Config -eq 'RelWithDebInfo') { 'the ones written down, the screens programs'' borrowed from Debug' } else { 'the measured ones' }
Write-Output ('Isolated retries, the guards in the gate and in every build, the margin step, the fuzz step, the Credential Manager ' +
    'boundary wording, the WebRTC files'' lock rule and the library''s boundary (tried on a scratch copy) and the decoding threads'' order passed' + $(if ($BuildDir) { "; the limits CTest applies in $Config are $limitsSaid." } else { '.' }))
