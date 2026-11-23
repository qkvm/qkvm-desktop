#Requires -Version 7.2
<#
.SYNOPSIS
Checks everything: how the code is set, every kind of build, every test, and what the linters say.

.DESCRIPTION
    tools\check.ps1

"It passes" means that this ran to its last line and that line said so. All it printed is kept in
build\check.txt: read the whole of it, not its end.

A step that fails does not stop the ones after it, so that one run shows everything that is wrong;
only the tests of a build that did not build are left out.

A test program that fails is run once more, by itself. Many of the tests wait for real time to
pass, and a PC that is busy with other work makes them late. One that passes at its second run has
not passed unseen: it is named where it happened and again just before the last line, and is looked
into. One that fails twice has failed.
#>

. "$PSScriptRoot\common.ps1"
Enter-BuildEnvironment

$Presets = 'debug', 'release', 'asan'
$Kept = Join-Path $DesktopRoot 'build\check.txt'
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Kept) | Out-Null

$failed = [System.Collections.Generic.List[string]]::new()
$secondRun = [System.Collections.Generic.List[string]]::new()
# Every step that ran and how it ended, said again before the last line: a step taken out of this
# file is seen missing there, not only by its absence (L-3).
$steps = [System.Collections.Generic.List[string]]::new()

function Step {
    <# Runs one step, says its name before it, and notes it when its last command did not end with 0. #>
    param([string] $Name, [scriptblock] $Do)
    Write-Output '' "=== $Name"
    $global:LASTEXITCODE = 0
    & $Do
    $passed = $LASTEXITCODE -eq 0
    $steps.Add("$Name`: $(if ($passed) { 'passed' } else { "failed ($LASTEXITCODE)" })")
    if (-not $passed) {
        $failed.Add($Name)
        Write-Output "*** $Name failed (exit code $LASTEXITCODE)"
    }
    return
}

Push-Location $DesktopRoot
try {
    & {
        Write-Output "QKVM Desktop, checked on $(Get-Date -Format 'yyyy-MM-dd HH:mm') with the tools of $env:QKVM_BUILD_ENVIRONMENT"
        $parts = 'base', 'engine', 'input', 'bmc', 'rfb', 'core', 'ui', 'bench' | Where-Object { Test-Path -LiteralPath $_ }
        $cpp = @(Get-ChildItem -Path $parts -Recurse -File -Include *.h, *.cpp | ForEach-Object FullName)
        $qml = @(Get-ChildItem -Path $parts -Recurse -File -Include *.qml | ForEach-Object FullName)

        # The engine's tests draw the pictures this makes. It makes only what is not there yet.
        Step 'test pictures' {
            try {
                & (Join-Path $PSScriptRoot 'make_testdata.ps1') 6>&1
                $global:LASTEXITCODE = 0
            } catch {
                Write-Output "$_"
                $global:LASTEXITCODE = 1
            }
        }

        # A sign that draws nothing is seen by nobody who reads the file: none is written as itself.
        # (And the check for them is tried first: one that refuses nothing would always pass.)
        Step 'signs nobody can see' {
            python (Join-Path $PSScriptRoot 'test_check_signs.py')
            if ($LASTEXITCODE -eq 0) { python (Join-Path $PSScriptRoot 'check_signs.py') }
        }

        # Windows takes a command line of at most 32,767 characters, and the tree's files outgrew it on
        # 2026-11-15 (34,452 with 0.1.2 merged into 0.2.0; PowerShell then says only that the program
        # "failed to run"). So they are given a hundred at a time, and a part that fails fails the step.
        Step 'clang-format' {
            clang-format --version
            $worst = 0
            for ($at = 0; $at -lt $cpp.Count; $at += 100) {
                clang-format --dry-run --Werror $cpp[$at..([Math]::Min($at + 99, $cpp.Count - 1))]
                if ($LASTEXITCODE -ne 0) { $worst = $LASTEXITCODE }
            }
            $global:LASTEXITCODE = $worst
        }

        Step 'update release tools' {
            try {
                & (Join-Path $PSScriptRoot 'test_update_tools.ps1')
                $global:LASTEXITCODE = 0
            } catch {
                Write-Output "$_"
                $global:LASTEXITCODE = 1
            }
        }

        # The release key's tools, with throwaway keys only (each made in a temporary folder and removed).
        Step 'release signing tools' {
            try {
                & (Join-Path $PSScriptRoot 'test_release_signing.ps1')
                $global:LASTEXITCODE = 0
            } catch {
                Write-Output "$_"
                $global:LASTEXITCODE = 1
            }
        }

        Step 'review follow-up guards' { & (Join-Path $PSScriptRoot 'test_review_guards.ps1') }

        Step 'release audit tools' {
            try {
                # (Its setups are made by tools/build_setup.ps1 with the pinned Inno Setup, fetched once into .deps and
                # unpacked there, never installed, as the Windows setup step below has it; outside review of 0.2.0, 1.9.)
                & (Join-Path $PSScriptRoot 'fetch_inno.ps1') 6>&1
                & (Join-Path $PSScriptRoot 'test_release_audit.ps1')
                $global:LASTEXITCODE = 0
            } catch {
                Write-Output "$_"
                $global:LASTEXITCODE = 1
            }
        }

        # What the release build refuses before it begins (it builds and fetches nothing here).
        Step 'release build checks' { pwsh -NoProfile -File (Join-Path $PSScriptRoot 'test_build_release.ps1') }
        # What each program and library that ships is built with against the misuse of its memory (made-up images).
        Step 'image protection checks' { pwsh -NoProfile -File (Join-Path $PSScriptRoot 'test_image_protections.ps1') }
        # What libdatachannel and Mbed TLS are built from, and the record of a build (made-up trees; nothing built).
        Step 'WebRTC source checks' { pwsh -NoProfile -File (Join-Path $PSScriptRoot 'test_webrtc_sources.ps1') }
        # The release files of a run, and the check of what is published (made-up runs; a stand-in host on 127.0.0.1).
        Step 'release files checks' { pwsh -NoProfile -File (Join-Path $PSScriptRoot 'test_release_files.ps1') }
        Step 'published release checks' { pwsh -NoProfile -File (Join-Path $PSScriptRoot 'test_verify_release.ps1') }
        Step 'release commands checks' { pwsh -NoProfile -File (Join-Path $PSScriptRoot 'test_release_commands.ps1') }

        Step 'qmlformat' {
            $unset = 0
            foreach ($file in $qml) {
                $asSet = ((qmlformat $file) -join "`n").TrimEnd()
                $asIs = ((Get-Content -LiteralPath $file -Raw) -replace "`r`n", "`n").TrimEnd()
                if ($asSet -cne $asIs) {
                    Write-Output "$file is not set the way qmlformat sets it"
                    $unset++
                }
            }
            $global:LASTEXITCODE = $unset
        }

        # QML takes a property whose name begins with "on" and a capital for the handler of a signal,
        # and says so only when the file is loaded, in words that do not point here. qmllint says
        # nothing. It has cost an afternoon twice.
        Step 'qml names' {
            $taken = @($qml | ForEach-Object { Select-String -LiteralPath $_ -Pattern '\bproperty\s+\S+\s+on[A-Z]\w*' -CaseSensitive })
            foreach ($found in $taken) {
                Write-Output "$($found.Path):$($found.LineNumber): a property may not be called on-something: $($found.Line.Trim())"
            }
            $global:LASTEXITCODE = $taken.Count
        }

        # A line of text takes what it is given for a page where it is written like one ("<b>",
        # "<img src=...>"), unless it is told that it is letters. What the program shows comes from
        # servers and devices: every line of text is told. (ui/tests/test_wall_screens.cpp holds the
        # screens to it; this holds a line that no test happens to draw.)
        Step 'qml plain text' {
            $loose = 0
            foreach ($file in $qml) {
                $lines = @(Get-Content -LiteralPath $file)
                for ($at = 0; $at -lt $lines.Count; $at++) {
                    # "Text {", "contentItem: Text {", "component Heading: Text {": wherever one begins.
                    if ($lines[$at] -notmatch '(^|\s)Text \{\s*$') { continue }
                    $deep = 0
                    $told = $false
                    for ($in = $at; $in -lt $lines.Count; $in++) {
                        $deep += @($lines[$in].ToCharArray() | Where-Object { $_ -eq '{' }).Count
                        if ($deep -eq 1 -and $lines[$in] -match '^\s*textFormat: Text\.PlainText\s*$') { $told = $true }
                        $deep -= @($lines[$in].ToCharArray() | Where-Object { $_ -eq '}' }).Count
                        if ($deep -le 0) { break }
                    }
                    if (-not $told) {
                        Write-Output "${file}:$($at + 1): a line of text that is not told its text is letters (textFormat: Text.PlainText)"
                        $loose++
                    }
                }
            }
            $global:LASTEXITCODE = $loose
        }

        $built = @()
        foreach ($preset in $Presets) {
            Step "configure $preset" { cmake --preset $preset }
            Step "build $preset" { cmake --build --preset $preset }
            if ($failed -notcontains "configure $preset" -and $failed -notcontains "build $preset") {
                $built += $preset
                Step "test $preset" {
                    # On the fast cores, where the PC has two kinds: on the slow ones tests that
                    # are right run out of their time (common.ps1 says what was seen). The builds
                    # have all of them.
                    Use-FastCores 6>&1
                    try {
                        # The first run's results, written apart from any run again (the margin step reads
                        # them, and only ones written after this run began); and its log, kept for whoever
                        # reads the evidence.
                        $first = Join-Path $DesktopRoot "build\$preset\Testing\first-attempt.xml"
                        Remove-Item -LiteralPath $first -ErrorAction Ignore
                        $script:testsBegan = Get-Date
                        ctest --preset $preset --output-junit $first
                        $ended = $LASTEXITCODE
                        $log = "build\$preset\Testing\Temporary\LastTest.log"
                        if (Test-Path -LiteralPath $log) {
                            Copy-Item -LiteralPath $log -Destination "build\$preset\Testing\first-attempt.log" -Force
                        }
                        $global:LASTEXITCODE = $ended
                        if ($LASTEXITCODE -ne 0) {
                            $noted = "build\$preset\Testing\Temporary\LastTestsFailed.log"
                            $again = @(if (Test-Path -LiteralPath $noted) { Get-Content -LiteralPath $noted | ForEach-Object { ($_ -split ':', 2)[1] } })
                            Write-Output "*** failed in ${preset}, and run once more by itself: $($again -join ', ')"
                            ctest --preset $preset --rerun-failed --output-on-failure -j1
                            if ($LASTEXITCODE -eq 0) { foreach ($name in $again) { $secondRun.Add("$name ($preset)") } }
                        }
                    } finally {
                        $ended = $LASTEXITCODE
                        Use-FastCores -Not
                        $global:LASTEXITCODE = $ended
                    }
                }
                # How close each program came to its limit in the first run (L-1).
                Step "time margins $preset" { & (Join-Path $PSScriptRoot 'test_time_margins.ps1') -Preset $preset -Since $script:testsBegan }
            }
        }

        # libFuzzer's targets of the readers of what a device or a server sends, each for a bounded time from the seeds
        # the tests' own inputs make (tools/fuzz.ps1: the fuzz preset's own build on every core, then half the fast
        # cores at once, as the tests are held to them and as the step was measured, six minutes over the rounds that
        # takes): anything one finds fails the step, its input kept (DESIGN.md section 8). It passes only where
        # fuzz.ps1 ended well and said, as its last line, that every target ran its time and found nothing (outside
        # review of FUZZ, L1 and L3).
        Step 'fuzz' {
            pwsh -NoProfile -File (Join-Path $PSScriptRoot 'fuzz.ps1') -Minutes 6 -FastCores | ForEach-Object { $_; $last = $_ }
            if ($LASTEXITCODE -eq 0 -and $last -ne 'No fuzz target found anything.') {
                Write-Output '*** fuzz.ps1 ended well without saying that every target ran its time and found nothing'
                $global:LASTEXITCODE = 1
            }
        }

        if ($built -contains 'release') {
            Step 'Windows runnable package' {
                try {
                    & (Join-Path $PSScriptRoot 'package.ps1') -SkipBuild
                    $global:LASTEXITCODE = 0
                } catch {
                    Write-Output "$_"
                    $global:LASTEXITCODE = 1
                }
            }
            if ($failed -notcontains 'Windows runnable package') {
                # The setup made from the same package: Inno Setup fetched once into .deps and unpacked there in
                # its portable mode, never installed (tools/fetch_inno.ps1); the setup and its uncompressed twin
                # (tools/build_setup.ps1); and its static test, which never runs it (tools/test_setup.ps1).
                Step 'Windows setup' {
                    try {
                        & (Join-Path $PSScriptRoot 'fetch_inno.ps1') 6>&1
                        . (Join-Path $PSScriptRoot 'build_common.ps1')
                        $inno = Join-Path $DesktopRoot ".deps\inno\$InnoVersion"
                        $package = Get-Content -LiteralPath 'build/packages/latest.json' -Raw | ConvertFrom-Json
                        $setup = Join-Path $DesktopRoot "build\setups\$(Get-Date -Format 'yyyyMMdd-HHmmss-fff')"
                        # (With qKVM Setup's update check, this release build's own: the setup carries it.)
                        $updateCheck = Join-Path $DesktopRoot 'build\release\bin\qkvm-update-check.exe'
                        pwsh -NoProfile -File (Join-Path $PSScriptRoot 'build_setup.ps1') -PackageDirectory $package.folder -Inno $inno -OutputDirectory $setup -UpdateCheck $updateCheck -ScanTwin
                        if ($LASTEXITCODE -ne 0) { throw "The setup could not be made (exit $LASTEXITCODE)." }
                        pwsh -NoProfile -File (Join-Path $PSScriptRoot 'test_setup.ps1') -SetupDirectory $setup -PackageDirectory $package.folder -Inno $inno
                    } catch {
                        Write-Output "$_"
                        $global:LASTEXITCODE = 1
                    }
                }
                Step 'Windows installer' {
                    try {
                        $package = Get-Content -LiteralPath 'build/packages/latest.json' -Raw | ConvertFrom-Json
                        & (Join-Path $PSScriptRoot 'build_installer.ps1') -PackageDirectory $package.folder -Development
                        $installer = Get-Content -LiteralPath 'build/installers/latest.json' -Raw | ConvertFrom-Json
                        & (Join-Path $PSScriptRoot 'test_installer.ps1') -SetupDirectory $installer.folder
                        $global:LASTEXITCODE = 0
                    } catch {
                        Write-Output "$_"
                        $global:LASTEXITCODE = 1
                    }
                }
            }
        }

        if ($built -contains 'debug') {
            # Qt writes down for each QML module how it is to be linted; a warning counts as a failure here.
            Step 'qmllint' {
                $worst = 0
                $lists = Get-ChildItem -Path build\debug -Recurse -File -Filter *.rsp |
                    Where-Object { $_.Directory.Name -eq 'qmllint' -and $_.Name -notmatch '_(json|module)\.rsp$' }
                if (-not $lists) { Write-Output 'No QML module was found to lint.'; $worst = 1 }
                foreach ($list in $lists) {
                    qmllint --max-warnings 0 "@$($list.FullName)"
                    if ($LASTEXITCODE -ne 0) { $worst = $LASTEXITCODE }
                }
                $global:LASTEXITCODE = $worst
            }
            # Our own files only: what Qt generates beside them is Qt's to keep clean. Eight files at
            # a time, each looked at by a clang-tidy of its own (one after the other they took half
            # of the whole check's time); what was said of each is told in the files' own order.
            Step 'clang-tidy' {
                clang-tidy --version
                $looked = $cpp | Where-Object { $_ -like '*.cpp' } | ForEach-Object -ThrottleLimit 8 -Parallel {
                    $said = clang-tidy -p build\debug --quiet $_ 2>&1
                    [pscustomobject]@{ File = $_; Said = $said; Ended = $LASTEXITCODE }
                }
                $worst = 0
                foreach ($one in ($looked | Sort-Object File)) {
                    Write-Output "--- $($one.File)"
                    $one.Said
                    if ($one.Ended -ne 0) { $worst = $one.Ended }
                }
                Write-Output "$(@($looked).Count) files looked at."
                $global:LASTEXITCODE = $worst
            }
        }

        # The steps that must have run, wherever a later edit puts them (in a branch that never runs, say): one that never
        # ran fails the check (outside review of FUZZ, L1).
        foreach ($must in 'fuzz', 'review follow-up guards') {
            if (-not ($steps | Where-Object { $_ -like "${must}: *" })) {
                $failed.Add("$must (never ran)")
                Write-Output "*** $must never ran"
            }
        }
        Write-Output '' 'Steps:'
        $steps | ForEach-Object { Write-Output "  $_" }
        Write-Output ''
        if ($failed.Count -gt 0) {
            Write-Output "FAILED: $($failed -join ', ')"
        } else {
            if ($secondRun.Count -gt 0) {
                Write-Output "Passed only at a second run, and to be looked into: $($secondRun -join ', ')"
            }
            Write-Output 'All of it passed.'
        }
    } 2>&1 | Tee-Object -FilePath $Kept
} finally {
    Pop-Location
}

Write-Host "Kept in $Kept"
exit ([int]($failed.Count -gt 0))
