#Requires -Version 7.2
<#
What libdatachannel and Mbed TLS are built from, for both kinds, without building or fetching anything: a tree of
sources by its digest (build_common.ps1's Get-SourceTreeDigest), the pinned trees and archive ($WebRtcPins,
Get-WebRtcSourceRefusals), and the record of a build (New-WebRtcRecord, Get-WebRtcRecordRefusals), on made-up folders;
that experiments/webrtc/build-cpp-dependencies.ps1 refuses other sources before it does anything; and that the pins are
the ones the fetch and dependencies.json name. Every failed check is a "FAIL:" line, and the script then fails.
#>
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\build_common.ps1"

$failures = 0
function Check([bool] $Condition, [string] $Message) {
    # (Written to the host: a check inside a function whose output is dropped is still seen.)
    if (-not $Condition) { Write-Host "FAIL: $Message"; $script:failures++ }
}
$desktop = Split-Path -Parent $PSScriptRoot
$work = Join-Path $desktop "build/webrtc-tests/$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $work | Out-Null
function Write-Tree([string] $Folder, [hashtable] $Files) {
    foreach ($entry in $Files.GetEnumerator()) {
        $file = Join-Path $Folder $entry.Key
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $file) | Out-Null
        [IO.File]::WriteAllBytes($file, [Text.Encoding]::Latin1.GetBytes($entry.Value))
    }
    return $Folder
}

# --- A tree's digest: the same commit checked out with CRLF or LF line ends, and with git's own files or without, is
# one; any other byte, a file more or fewer, or a file moved, is another.
$lf = @{ 'CMakeLists.txt' = "project(x)`nadd_library(x x.c)`n"; 'src/x.c' = "int x;`n"; 'deps/a/LICENSE' = "notice`n"; 'data/blob.bin' = "`0`1`2" }
$crlf = @{ 'CMakeLists.txt' = "project(x)`r`nadd_library(x x.c)`r`n"; 'src/x.c' = "int x;`r`n"; 'deps/a/LICENSE' = "notice`r`n"; 'data/blob.bin' = "`0`1`2"
    '.git/HEAD' = 'ref: refs/heads/main'; 'deps/a/.git' = 'gitdir: ../../.git/modules/deps/a'; '.git/modules/deps/a/config' = '[core]' }
$digest = Get-SourceTreeDigest (Write-Tree "$work/lf" $lf)
Check ($digest -match '\A[0-9a-f]{64}\z') "a tree's digest is a SHA-256 ($digest)"
Check ((Get-SourceTreeDigest (Write-Tree "$work/crlf" $crlf)) -ceq $digest) 'a checkout with CRLF line ends and git''s own files has the digest of one with LF and none'
foreach ($change in @(
        @('a byte of a file', @{ 'src/x.c' = "int y;`n" }),
        @('a file more', @{ 'src/extra.c' = "int z;`n" }),
        @('a compiled script left beside the sources', @{ 'scripts/__pycache__/config.cpython-314.pyc' = "`0" }),
        @('a lone carriage return', @{ 'src/x.c' = "int x;`r" }))) {
    $other = $lf.Clone()
    foreach ($entry in $change[1].GetEnumerator()) { $other[$entry.Key] = $entry.Value }
    Check ((Get-SourceTreeDigest (Write-Tree "$work/changed-$([guid]::NewGuid().ToString('N'))" $other)) -cne $digest) "$($change[0]) gives another digest"
}
$fewer = $lf.Clone(); $fewer.Remove('deps/a/LICENSE')
Check ((Get-SourceTreeDigest (Write-Tree "$work/fewer" $fewer)) -cne $digest) 'a file fewer gives another digest'
$moved = $lf.Clone(); $moved.Remove('src/x.c'); $moved['src/y.c'] = "int x;`n"
Check ((Get-SourceTreeDigest (Write-Tree "$work/moved" $moved)) -cne $digest) 'a file moved gives another digest'

# --- The pinned sources: each tree by its digest, and the archive beside them by its SHA-256 (made-up pins of made-up
# trees here; the real ones are $WebRtcPins).
$sources = "$work/sources"
Write-Tree "$sources/libdatachannel" $lf | Out-Null
Write-Tree "$sources/mbedtls-9.9.9" @{ 'library/ssl.c' = "int ssl;`n"; 'LICENSE' = "Apache`n" } | Out-Null
[IO.File]::WriteAllBytes("$sources/mbedtls-9.9.9.tar.bz2", [byte[]] @(66, 90, 104))
$pins = @{
    libdatachannel = @{ tree = $digest }
    mbedtls = @{ version = '9.9.9'; tree = (Get-SourceTreeDigest "$sources/mbedtls-9.9.9"); archiveSha256 = (Get-Sha256 "$sources/mbedtls-9.9.9.tar.bz2") }
}
$said = Get-WebRtcSourceRefusals $sources -Pins $pins
Check ($said.Refusals.Count -eq 0 -and $said.Trees.libdatachannel -ceq $digest -and $said.Trees.mbedtls -ceq $pins.mbedtls.tree) "the pinned sources pass, and their digests come back ($($said.Refusals -join '; '))"
[IO.File]::AppendAllText("$sources/libdatachannel/src/x.c", '// one more line')
$said = Get-WebRtcSourceRefusals $sources -Pins $pins
Check (@($said.Refusals | Where-Object { $_ -match '\Alibdatachannel is not the pinned tree' }).Count -eq 1 -and $said.Refusals.Count -eq 1) "a changed file of libdatachannel is refused ($($said.Refusals -join '; '))"
Write-Tree "$sources/libdatachannel" $lf | Out-Null
[IO.File]::WriteAllBytes("$sources/mbedtls-9.9.9.tar.bz2", [byte[]] @(66, 90, 104, 0))
Check (@((Get-WebRtcSourceRefusals $sources -Pins $pins).Refusals | Where-Object { $_ -match 'mbedtls-9\.9\.9\.tar\.bz2 is not the pinned archive' }).Count -eq 1) 'another Mbed TLS archive is refused'
Remove-Item -LiteralPath "$sources/mbedtls-9.9.9.tar.bz2"
Check ((Get-WebRtcSourceRefusals $sources -Pins $pins).Refusals.Count -eq 0) 'a copy of the trees without the archive passes, by the trees'
Remove-Item -LiteralPath "$sources/mbedtls-9.9.9" -Recurse
Check (@((Get-WebRtcSourceRefusals $sources -Pins $pins).Refusals | Where-Object { $_ -match '\Athere is no mbedtls-9\.9\.9 in' }).Count -eq 1) 'sources without Mbed TLS are refused'
Check ((Get-WebRtcSourceRefusals "$work/nothing-here").Refusals.Count -eq 2) 'a folder of neither is refused for both'

# --- The build refuses sources that are not the pinned ones before it does anything: it enters no build environment,
# makes no folder and writes no record (a made-up folder of the right names, given as -Sources).
$arm64Deps = Join-Path $desktop '.deps\arm64'
$hadDeps = Test-Path -LiteralPath $arm64Deps
$fake = Write-Tree "$work/fake" @{ 'libdatachannel/CMakeLists.txt' = "project(libdatachannel)`n"; 'mbedtls-3.6.7/CMakeLists.txt' = "project(mbedtls)`n" }
$said = & pwsh -NoProfile -File (Join-Path $desktop 'experiments\webrtc\build-cpp-dependencies.ps1') -Arch arm64 -Sources $fake 2>&1 | Out-String
# (As one line: PowerShell folds an error's words into the width of its window.)
$said = $said -replace '\s*\r?\n\s*(\|\s*)?', ' '
Check ($LASTEXITCODE -ne 0 -and $said -match 'Not the pinned WebRTC sources, so nothing is built: libdatachannel is not the pinned tree.*mbedtls-3\.6\.7 is not the pinned tree') "the build refuses sources that are not the pinned ones ($($said.Trim()))"
Check ($said -notmatch 'Visual Studio|vcvars|cmake' -and (Test-Path -LiteralPath $arm64Deps) -eq $hadDeps) 'and does nothing before it refuses'
$builder = [IO.File]::ReadAllText((Join-Path $desktop 'experiments\webrtc\build-cpp-dependencies.ps1'))
Check ($builder -match '(?s)\$checked = Get-WebRtcSourceRefusals \$trialRoot\r?\nif \(\$checked\.Refusals\.Count\) \{ throw .*Enter-BuildEnvironment') 'the build checks its sources, whichever they are, before its environment'
Check ($builder -match '(?s)PYTHONDONTWRITEBYTECODE = ''1''.*cmake -S') 'the build leaves no compiled script in the sources it checked'
Check ($builder -match 'New-WebRtcRecord -Arch \$Arch -Trees \$checked\.Trees -Install \$prefix') 'the build records what it built from and what it made'
$fetcher = [IO.File]::ReadAllText((Join-Path $desktop 'experiments\webrtc\fetch-cpp-dependencies.ps1'))
Check ($fetcher -match '(?s)if \(Test-Path -LiteralPath \$unpacked\) \{ Remove-Item -LiteralPath \$unpacked -Recurse -Force \}\r?\n& "\$env:SystemRoot\\System32\\tar\.exe" -xf \$archive') 'the fetch unpacks Mbed TLS anew from its checked archive every time'

# --- A build's record: of its kind, of the pinned sources, naming the datachannel.dll it made.
$install = "$work/install"
Write-Tree $install @{ 'bin/datachannel.dll' = 'made-up library'; 'lib/datachannel.lib' = 'made-up import library'; 'include/rtc/rtc.h' = '' } | Out-Null
$pinnedTrees = @{ libdatachannel = $WebRtcPins.libdatachannel.tree; mbedtls = $WebRtcPins.mbedtls.tree }
$record = New-WebRtcRecord -Arch arm64 -Trees $pinnedTrees -Install $install -Script (Join-Path $desktop 'experiments\webrtc\build-cpp-dependencies.ps1') -Commit 'abc'
$file = "$work/$WebRtcRecordName"
$record | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $file -Encoding utf8
function Read-Record { return Get-Content -LiteralPath $file -Raw | ConvertFrom-Json }
$read = Read-Record
Check (@($read.built.name) -join ',' -ceq 'bin/datachannel.dll,lib/datachannel.lib' -and $read.commit -ceq 'abc' -and $read.script.sha256 -match '\A[0-9a-f]{64}\z') "the record names what the build made, its script and its commit ($($read.built.name -join ','))"
$dll = "$install/bin/datachannel.dll"
$said = @(Get-WebRtcRecordRefusals $read arm64 $dll)
Check ($said.Count -eq 0) "the record of a build of the pinned sources passes for the DLL it made ($($said -join '; '))"
Check (@(Get-WebRtcRecordRefusals $read x64 $dll) -join '' -match 'of a build for arm64, not x64') 'a record of the other kind is refused'
[IO.File]::WriteAllText("$work/other.dll", 'another library')
Check (@(Get-WebRtcRecordRefusals $read arm64 "$work/other.dll") -join '' -match 'not this one, by its size and SHA-256') 'another datachannel.dll is refused'
Check (@(Get-WebRtcRecordRefusals $read arm64 "$work/none.dll") -join '' -match 'there is no') 'no datachannel.dll is refused'
foreach ($change in @(
        @('libdatachannel commit', { param($r) $r.sources.libdatachannel.commit = '0' * 40 }),
        @('libdatachannel tree', { param($r) $r.sources.libdatachannel.tree = '0' * 64 }),
        @('libjuice commit', { param($r) $r.sources.libdatachannel.gitlinks.libjuice = '0' * 40 }),
        @('plog commit', { param($r) $r.sources.libdatachannel.gitlinks.PSObject.Properties.Remove('plog') }),
        @('Mbed TLS archive', { param($r) $r.sources.mbedtls.archiveSha256 = '0' * 64 }),
        @('Mbed TLS tree', { param($r) $r.sources.mbedtls.tree = '0' * 64 }))) {
    $changed = Read-Record
    & $change[1] $changed
    $said = @(Get-WebRtcRecordRefusals $changed arm64 $dll)
    Check ($said.Count -eq 1 -and $said[0] -ceq "its $($change[0]) is not the pinned one") "a record whose $($change[0]) is another is refused ($($said -join '; '))"
}
$changed = Read-Record; $changed.built = @($changed.built | Where-Object name -cne 'bin/datachannel.dll')
Check (@(Get-WebRtcRecordRefusals $changed arm64 $dll) -join '' -match 'not this one') 'a record that names no datachannel.dll is refused'
foreach ($not in @($null, [pscustomobject] @{ schema = 1; part = 'ffmpeg' }, [pscustomobject] @{ schema = 2; part = 'webrtc' })) {
    Check (@(Get-WebRtcRecordRefusals $not arm64 $dll) -join '' -ceq 'it is no record of a WebRTC build') 'what is no record of a WebRTC build is refused'
}
# (tools/package.ps1 makes no package whose datachannel.dll is not the one its build's record names.)
$packager = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'package.ps1'))
Check ($packager -match '\$webrtcRefused = @\(Get-WebRtcRecordRefusals \(Get-Content -LiteralPath \$webrtcRecord -Raw \| ConvertFrom-Json\) \$Arch "\$package/datachannel\.dll"\)\r?\n\s+if \(\$webrtcRefused\.Count\) \{ throw') 'package.ps1 holds the datachannel.dll it packs to its build''s record'

# --- The pins are the ones the fetch takes and experiments/webrtc/dependencies.json names.
$named = (Get-Content -LiteralPath (Join-Path $desktop 'experiments\webrtc\dependencies.json') -Raw | ConvertFrom-Json).cpp
Check ($named.libdatachannel.commit -ceq $WebRtcPins.libdatachannel.commit -and $named.libdatachannel.version -ceq $WebRtcPins.libdatachannel.version) 'libdatachannel''s pin is dependencies.json''s'
foreach ($module in $WebRtcPins.libdatachannel.gitlinks.GetEnumerator()) {
    Check ($named.gitlinks.($module.Key) -ceq $module.Value) "$($module.Key)'s pin is dependencies.json's"
}
Check (@($named.gitlinks.PSObject.Properties.Name).Count -eq $WebRtcPins.libdatachannel.gitlinks.Count) 'every submodule dependencies.json names is pinned'
Check ($named.mbedtls.sha256 -ceq $WebRtcPins.mbedtls.archiveSha256 -and $named.mbedtls.version -ceq $WebRtcPins.mbedtls.version) 'Mbed TLS''s pin is dependencies.json''s'
Check ($fetcher.Contains("checkout --detach $($WebRtcPins.libdatachannel.commit)") -and $fetcher.Contains("'$($WebRtcPins.mbedtls.archiveSha256)'")) 'the fetch takes the pinned commit and checks the pinned archive'
Check ($fetcher -match 'submodule update --init --depth 1 deps/libjuice deps/libsrtp deps/usrsctp deps/plog') 'the fetch takes the four pinned submodules'

if ($failures) { throw "$failures WebRTC source check(s) failed." }
Remove-Item -LiteralPath $work -Recurse -Force
Write-Output 'WebRTC source checks passed: a tree by its digest whatever its line ends, the pinned trees and archive, the build that refuses anything else first, and its record held to what it made.'
