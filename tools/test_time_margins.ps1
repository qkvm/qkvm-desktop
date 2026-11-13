# How close each test program came to the limit CTest gave it, in one preset's first run (L-1).
#
#   tools\test_time_margins.ps1 -Preset debug|release|asan [-Since <when that run began>]
#
# Reads the first run's results that tools/check.ps1 keeps (build\<preset>\Testing\first-attempt.xml,
# CTest's JUnit output of the run before any program is run again, deleted before each run; CTest's own
# LastTest.log was seen with its first blocks written over, so it is not read) and the limits CTest
# applies (ctest --show-only=json-v1). Results older than -Since are refused (an earlier run's would pass
# for this one's). Every program CTest knows must be in the results, with a limit and a time, and the
# review guards must be among them (they are a test of every build: CMakeLists.txt). A program that used
# more than two thirds of its limit is named; one that used more than 85% fails this step: that is where
# programs began to run out of time when the PC was busy, before any code was wrong (Debug's screens at
# 588.83 s of 600, ASAN's at 507.90). Anything it cannot read fails the step, not the gate.
param([Parameter(Mandatory)] [string] $Preset, [Nullable[datetime]] $Since = $null)
$ErrorActionPreference = 'Stop'
try {
    $root = Split-Path -Parent $PSScriptRoot
    $build = Join-Path $root "build\$Preset"
    $results = Join-Path $build 'Testing\first-attempt.xml'
    if (-not (Test-Path -LiteralPath $results)) {
        Write-Output "No first-run results for $Preset ($results)."
        exit 1
    }
    # (In UTC: a run across the end of summer time is not taken for an older one.)
    if ($null -ne $Since -and (Get-Item -LiteralPath $results).LastWriteTimeUtc -lt $Since.ToUniversalTime()) {
        Write-Output "The first-run results for $Preset are older than this run ($((Get-Item -LiteralPath $results).LastWriteTimeUtc) UTC before $($Since.ToUniversalTime()) UTC)."
        exit 1
    }
    # (Names as CTest writes them, letter case included.)
    $limits = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::Ordinal)
    $guard = $null
    foreach ($test in (ctest --test-dir $build --show-only=json-v1 | ConvertFrom-Json).tests) {
        $limit = @($test.properties | Where-Object name -eq 'TIMEOUT' | ForEach-Object value)
        $limits[$test.name] = if ($limit.Count -eq 1) { [double] $limit[0] } else { $null }
        if ($test.name -ceq 'test_review_guards') { $guard = $test }
    }
    # The review guards are a test of every build, run as they are written: with this build's folder, and
    # with nothing that would let them be skipped, expected to fail, or switched off (round 3, L1).
    $guardProperties = @(if ($null -ne $guard) { $guard.properties | ForEach-Object { $_.name } })
    $guardCommand = if ($null -ne $guard) { $guard.command -join ' ' } else { '' }
    $loose = @(@('DISABLED', 'SKIP_RETURN_CODE', 'SKIP_REGULAR_EXPRESSION', 'WILL_FAIL') | Where-Object { $guardProperties -contains $_ })
    if ($null -eq $guard -or $loose.Count -gt 0 -or $guardCommand -notmatch 'test_review_guards\.ps1 -BuildDir ') {
        Write-Output "$Preset does not run test_review_guards as written (properties: $($guardProperties -join ', '); command: $guardCommand)."
        exit 1
    }
    [xml] $junit = Get-Content -LiteralPath $results -Raw
    $guardCase = @($junit.SelectNodes("//testcase[@name='test_review_guards']"))
    if ($guardCase.Count -ne 1 -or $guardCase[0].GetAttribute('status') -ne 'run' -or
        $guardCase[0].SelectNodes('failure|skipped|error').Count -gt 0) {
        Write-Output "test_review_guards did not run and pass in $Preset's first run."
        exit 1
    }
    $rows = [Collections.Generic.List[object]]::new()
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $broken = [Collections.Generic.List[string]]::new()
    foreach ($case in $junit.SelectNodes('//testcase')) {
        $name = $case.GetAttribute('name')
        $time = $case.GetAttribute('time')
        if ($name -eq '' -or $time -eq '' -or -not $limits.ContainsKey($name) -or $null -eq $limits[$name] -or -not $seen.Add($name)) {
            $broken.Add($(if ($name -eq '') { '(a case without a name)' } else { $name }))
            continue
        }
        $rows.Add([pscustomobject]@{ Program = $name; Seconds = [double] $time; Limit = [double] $limits[$name] })
    }
    $missing = @($limits.Keys | Where-Object { -not $seen.Contains($_) })
    if ($rows.Count -eq 0 -or $broken.Count -gt 0 -or $missing.Count -gt 0) {
        Write-Output "$Preset`: $($rows.Count) programs read, $($limits.Count) known to CTest; unreadable, unknown, without a limit or twice: $($broken -join ', '); missing from the results: $($missing -join ', ')."
        exit 1
    }
    $over = 0
    foreach ($row in $rows) {
        $row | Add-Member -NotePropertyName Share -NotePropertyValue ([math]::Round(100 * $row.Seconds / $row.Limit))
        if ($row.Seconds / $row.Limit -gt 0.85) { $over++ }
    }
    $closest = $rows | Sort-Object Share -Descending | Select-Object -First 1
    Write-Output "$Preset`: $($rows.Count) programs; closest to its limit: $($closest.Program) at $($closest.Seconds) of $($closest.Limit) s ($($closest.Share)%)."
    foreach ($row in @($rows | Where-Object Share -gt 66 | Sort-Object Share -Descending)) {
        Write-Output ("  {0}: {1} of {2} s ({3}%){4}" -f $row.Program, $row.Seconds, $row.Limit, $row.Share,
            $(if ($row.Share -gt 85) { ' : over 85%, give it a measured limit' } else { '' }))
    }
    exit ([int]($over -gt 0))
} catch {
    Write-Output "The margins of $Preset could not be read: $_"
    exit 1
}
