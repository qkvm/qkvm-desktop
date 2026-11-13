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
    }
    $shown = & $Ctest --test-dir $BuildDir --show-only=json-v1 | ConvertFrom-Json
    if (@($shown.tests | Where-Object name -eq 'test_ui_screens').Count -ne 0) {
        $problems.Add('T-2/L-1: CTest has test_ui_screens again, which has no measured limit.')
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
# no return after it is held but through letGoOfDecoding.
$stream = (Get-Content -LiteralPath "$root/engine/src/stream_core.cpp" -Raw) -replace '//[^\n]*', ''
if ($stream -notmatch '(?s)void StreamCore::letGoOfDecoding\(std::unique_lock<std::mutex>& decoding\)\s*\{\s*decoding\.unlock\(\);\s*bool more = false;\s*\{\s*const std::lock_guard guard\(m_inboxLock\);\s*more = !m_inbox\.empty\(\);\s*\}\s*if \(more\) \{\s*m_gpu\.decodePool\(\)\.wake\(this\);') {
    $problems.Add('V-1: engine/src/stream_core.cpp letGoOfDecoding no longer lets go first and looks after.')
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

if ($problems.Count) { $problems | Write-Output; exit 1 }
# (The screens programs' limits for the address sanitizer are Debug's, borrowed: said so.)
$limitsSaid = if ($Config -eq 'RelWithDebInfo') { 'the ones written down, the screens programs'' borrowed from Debug' } else { 'the measured ones' }
Write-Output ('Isolated retries, the guards in the gate and in every build, the margin step, the Credential Manager ' +
    'boundary wording, the WebRTC files'' lock rule and the library''s boundary (tried on a scratch copy) and the decoding threads'' order passed' + $(if ($BuildDir) { "; the limits CTest applies in $Config are $limitsSaid." } else { '.' }))
