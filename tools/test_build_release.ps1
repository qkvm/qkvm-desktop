#Requires -Version 7.2
<#
The release build's checks, without building or fetching anything: the helpers the release scripts share
(build_common.ps1), and what tools/build_release.ps1 refuses before it starts (asked with -CheckOnly, of
made-up exports in a folder of their own). Every failed check is a "FAIL:" line, and the script then fails.
#>
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\build_common.ps1"

$failures = 0
function Check([bool] $Condition, [string] $Message) {
    # (Written to the host: a check inside a function whose output is dropped is still seen.)
    if (-not $Condition) { Write-Host "FAIL: $Message"; $script:failures++ }
}
function Refuses([scriptblock] $Action, [string] $Expected, [string] $Message) {
    try {
        & $Action | Out-Null
        Write-Host "FAIL: $Message (it was accepted)"
        $script:failures++
    } catch {
        if ($_.Exception.Message -notmatch $Expected) {
            Write-Host "FAIL: $Message (refused for another reason: $($_.Exception.Message))"
            $script:failures++
        }
    }
}

$desktop = Split-Path -Parent $PSScriptRoot
$work = Join-Path $desktop "build/release-tests/$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $work | Out-Null

# --- The public key the program pins.
$key = New-ThrowawayPublicKey
Check (Test-ReleasePublicKey $key) 'a key just made is taken'
Check (Test-ReleasePublicKey $key.ToUpperInvariant()) 'a key in capitals is taken'
Check ($key -cne (New-ThrowawayPublicKey)) 'two keys made are two'
$last = if ($key[-1] -eq '0') { '1' } else { '0' }
Check (-not (Test-ReleasePublicKey ($key.Substring(0, 127) + $last))) 'a point that is not on the curve is refused'
Check (-not (Test-ReleasePublicKey $key.Substring(0, 127))) '127 digits are refused'
Check (-not (Test-ReleasePublicKey ($key + "`n"))) 'a key with a line feed after it is refused'
Check (-not (Test-ReleasePublicKey ('g' + $key.Substring(1)))) 'a letter that is no digit is refused'
Check (-not (Test-ReleasePublicKey '')) 'nothing is no key'

# --- Where a program says its symbols are.
function New-FixtureImage([string] $Path, [string] $Symbols) {
    # A 64-bit Windows image of one section, whose debug directory holds one CodeView record naming $Symbols.
    $bytes = [byte[]]::new(0x400)
    $put = { param([int] $At, [long] $Value, [int] $Size) [BitConverter]::GetBytes($Value)[0..($Size - 1)].CopyTo($bytes, $At) }
    $bytes[0] = 0x4D; $bytes[1] = 0x5A
    & $put 0x3C 0x40 4
    & $put 0x40 0x00004550 4
    & $put 0x44 0x8664 2
    & $put 0x46 1 2
    & $put 0x54 240 2
    $optional = 0x58
    & $put $optional 0x20B 2
    & $put ($optional + 108) 16 4
    & $put ($optional + 112 + 48) 0x1000 4
    & $put ($optional + 112 + 52) 28 4
    $section = $optional + 240
    [Text.Encoding]::ASCII.GetBytes('.rdata').CopyTo($bytes, $section)
    & $put ($section + 8) 0x200 4
    & $put ($section + 12) 0x1000 4
    & $put ($section + 16) 0x200 4
    & $put ($section + 20) 0x200 4
    $name = [Text.Encoding]::UTF8.GetBytes($Symbols)
    & $put (0x200 + 12) 2 4
    & $put (0x200 + 16) (24 + $name.Length + 1) 4
    & $put (0x200 + 20) 0x101C 4
    & $put (0x200 + 24) 0x21C 4
    & $put 0x21C 0x53445352 4
    $name.CopyTo($bytes, 0x21C + 24)
    [IO.File]::WriteAllBytes($Path, $bytes)
}
New-FixtureImage "$work/bare.exe" 'qkvm-desktop.pdb'
New-FixtureImage "$work/full.exe" 'Q:\b\qkvm-desktop-0.1.0\src\build\publish\bin\qkvm-desktop.pdb'
New-FixtureImage "$work/relative.exe" 'bin/qkvm-desktop.pdb'
Check ((@(Get-PeDebugPaths "$work/bare.exe") -join '|') -ceq 'qkvm-desktop.pdb') 'the symbols a program names are read'
Check (Test-BareDebugPath "$work/bare.exe") 'a bare file name is taken'
Check (-not (Test-BareDebugPath "$work/full.exe")) 'a path with its drive and folders is refused'
Check (-not (Test-BareDebugPath "$work/relative.exe")) 'a path with a folder is refused'
[IO.File]::WriteAllText("$work/text.exe", 'not a program at all')
Refuses { Get-PeDebugPaths "$work/text.exe" } 'not a Windows image' 'a file that is no program is not read as one'

# --- The build folder in what ships.
$folder = 'Q:\b\made-up-build'
$ways = [ordered]@{
    'plain' = [Text.Encoding]::Latin1.GetBytes("x`0Q:\b\made-up-build\src\.deps\a.c`0")
    'slashes' = [Text.Encoding]::Latin1.GetBytes("q:/B/Made-Up-Build/src")
    'doubled' = [Text.Encoding]::Latin1.GetBytes('{"at": "Q:\\b\\made-up-build\\x"}')
    'wide' = [byte[]](@(7) + [Text.Encoding]::Unicode.GetBytes('Q:\b\made-up-build\x.pdb'))
    'wide-even' = [Text.Encoding]::Unicode.GetBytes('..Q:/b/made-up-build/x')
    'big' = [Text.Encoding]::BigEndianUnicode.GetBytes('Q:\b\made-up-build')
}
foreach ($way in $ways.Keys) {
    [IO.File]::WriteAllBytes("$work/$way.bin", $ways[$way])
    Check (Test-NamesFolder "$work/$way.bin" $folder) "the build folder is found ($way)"
}
[IO.File]::WriteAllBytes("$work/clean.bin", [Text.Encoding]::Latin1.GetBytes("\libdatachannel\deps\usrsctp\a.c`0Q:\b\made-up`0build"))
Check (-not (Test-NamesFolder "$work/clean.bin" $folder)) 'a file without the build folder passes'

# --- One digest for a folder.
foreach ($name in 'one', 'two') {
    New-Item -ItemType Directory -Path "$work/$name/sub" | Out-Null
    [IO.File]::WriteAllText("$work/$name/a.txt", 'a')
    [IO.File]::WriteAllText("$work/$name/sub/b.txt", 'b')
}
Check ((Get-TreeDigest "$work/one") -ceq (Get-TreeDigest "$work/two")) 'the same files make the same digest'
[IO.File]::WriteAllText("$work/two/sub/b.txt", 'c')
Check ((Get-TreeDigest "$work/one") -cne (Get-TreeDigest "$work/two")) 'a changed byte makes another digest'
[IO.File]::WriteAllText("$work/two/sub/b.txt", 'b')
Move-Item "$work/two/a.txt" "$work/two/z.txt"
Check ((Get-TreeDigest "$work/one") -cne (Get-TreeDigest "$work/two")) 'a renamed file makes another digest'

# --- The project's minimal FFmpeg, by the configuration line it keeps (what tools/build_ffmpeg.ps1 holds its build to,
# and tools/audit_release.ps1 the package's avcodec).
$ffmpeg = "--prefix=/ffmpeg --target-os=mingw32 --arch=x86_64 --enable-cross-compile --cross-prefix=x86_64-w64-mingw32- " +
    "--cc=x86_64-w64-mingw32-gcc-win32 --host-cc=x86_64-w64-mingw32-gcc-win32 --pkg-config=false --enable-shared " +
    "--disable-static --enable-version3 --disable-autodetect --disable-everything --disable-programs --disable-doc " +
    "--disable-network --disable-debug --disable-avformat --disable-avdevice --disable-avfilter --disable-swscale " +
    "--disable-swresample --enable-w32threads --enable-d3d11va --enable-zlib --enable-decoder='h264,mjpeg,png' " +
    "--enable-parser=h264 --enable-hwaccel='h264_d3d11va,h264_d3d11va2' --extra-cflags=-I../zlib " +
    "--extra-ldflags='-L../zlib -static-libgcc'"
$said = @(Get-FFmpegRefusals $ffmpeg)
Check ($said.Count -eq 0) "the configuration the build is made with is the minimal one ($($said -join '; '))"
$wrong = [ordered]@{
    'without --disable-everything' = @(($ffmpeg -replace ' --disable-everything', ''), 'does not say --disable-everything')
    'without --disable-autodetect' = @(($ffmpeg -replace ' --disable-autodetect', ''), 'does not say --disable-autodetect')
    'without --enable-version3' = @(($ffmpeg -replace ' --enable-version3', ''), 'does not say --enable-version3')
    'with everything later enabled' = @("$ffmpeg --enable-everything", 'enables --enable-everything')
    'with OpenSSL' = @("$ffmpeg --enable-openssl", 'enables --enable-openssl')
    'with GnuTLS' = @("$ffmpeg --enable-gnutls", 'enables --enable-gnutls')
    'with Mbed TLS' = @("$ffmpeg --enable-mbedtls", 'enables --enable-mbedtls')
    'with Schannel' = @("$ffmpeg --enable-schannel", 'enables --enable-schannel')
    'with a library of lib*' = @("$ffmpeg --enable-libdav1d", 'enables --enable-libdav1d')
    'with GPL parts' = @("$ffmpeg --enable-gpl", 'enables --enable-gpl')
    'with non-free parts' = @("$ffmpeg --enable-nonfree", 'enables --enable-nonfree')
    'with another decoder' = @(($ffmpeg -replace "h264,mjpeg,png", 'h264,mjpeg,png,hevc'), 'enables the decoder hevc')
    'with an outside decoder' = @("$ffmpeg --enable-decoder=libdav1d", 'enables the decoder libdav1d')
    'with another parser' = @("$ffmpeg --enable-parser=hevc", 'enables the parser hevc')
    'with another hardware decoding' = @("$ffmpeg --enable-hwaccel=h264_dxva2", 'enables the hwaccel h264_dxva2')
    'with something in capitals' = @("$ffmpeg --ENABLE-OPENSSL", 'enables --ENABLE-OPENSSL')
}
foreach ($case in $wrong.GetEnumerator()) {
    $said = @(Get-FFmpegRefusals $case.Value[0])
    Check (@($said | Where-Object { $_ -match [regex]::Escape($case.Value[1]) }).Count -eq 1) "a configuration $($case.Key) is refused ($($said -join '; '))"
}
Check (@(Get-FFmpegRefusals '').Count -gt 0) 'no configuration is no minimal one'
# (A word that only begins as one of them is not it.)
Check (@(@(Get-FFmpegRefusals ($ffmpeg -replace '--disable-everything', '--disable-everything-but')) -match 'does not say --disable-everything').Count -eq 1) 'a longer word is not --disable-everything'
# build_ffmpeg.ps1 holds what it built to this rule, and to nothing of its own.
$builder = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'build_ffmpeg.ps1'))
Check ($builder -match '\$refused = @\(Get-FFmpegRefusals \$configuration\)' -and $builder -match 'if \(\$refused\.Count\) \{ throw') 'build_ffmpeg.ps1 refuses what the rule refuses'

# --- The setup, as build_release.ps1 makes it from its run's own package (it cannot be built here: what it runs, and
# in what order, is read from it). Inno Setup is unpacked anew in the build folder, the setup and its uncompressed twin
# are made from the moved package, the setup's static test runs, the beta audit is given the setup, the byte scan and
# the build folder's check look through both, and the record says the setup.
$releaseBuild = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'build_release.ps1'))
$order = [ordered]@{
    'the package is moved into the run' = 'Move-Item -LiteralPath \$made\.folder -Destination \$package'
    'Inno Setup is unpacked in the build folder' = "Invoke-Clean '\d+-inno' @\('-File', `"\`$src\\tools\\fetch_inno\.ps1`", '-Into', \`$inno, '-Downloads', `"\`$downloads\\inno`"\)"
    'the setup is made from the run''s package, with its twin' = "Invoke-Clean '\d+-setup' @\('-File', `"\`$src\\tools\\build_setup\.ps1`", '-PackageDirectory', \`$package, '-Inno', \(Join-Path \`$inno \`$InnoVersion\), '-OutputDirectory', \`$setupWork, '-ScanTwin'\)"
    'the setup''s static test runs on it' = "Invoke-Clean '\d+-setup-test' @\('-File', `"\`$src\\tools\\test_setup\.ps1`", '-SetupDirectory', \`$setupWork, '-PackageDirectory', \`$package, '-Inno', \(Join-Path \`$inno \`$InnoVersion\)\)"
    'the beta audit is given the setup' = 'audit_release\.ps1" -PackageDirectory \$package -OutputFile \(Join-Path \$run ''audit-beta\.json''\) -Beta -SetupDirectory \$setupWork'
    'Inno Setup''s tree is checked once more before the scan' = "(?m)^\s+\`$innoTree = Join-Path \`$inno \`$InnoVersion\r?\n\s+if \(\(Get-TreeDigest \`$innoTree\) -cne \`$InnoTreeDigest -or @\(Get-ChildItem -LiteralPath \`$innoTree -Recurse -File -Force\)\.Count -ne \`$InnoTreeFiles\) \{\r?\n\s+throw "
    'the byte scan takes only Inno Setup''s two programs as theirs' = "(?m)^\s+\`$theirs = @\('--public', \(Join-Path \`$innoTree 'Setup\.e32'\), '--public', \(Join-Path \`$innoTree 'SetupLdr\.e32'\)\)\r?$"
    'the byte scan looks through the setup and its twin' = '\$python -I \(Resolve-Path -LiteralPath \$Scanner\)\.Path @theirs \$package \$zip \$setupFile \$twinFile'
}
$at = 0
foreach ($step in $order.GetEnumerator()) {
    $found = [regex]::Match($releaseBuild.Substring($at), $step.Value)
    Check $found.Success "build_release.ps1: $($step.Key), in its order"
    if ($found.Success) { $at += $found.Index + $found.Length }
}
Check ($releaseBuild -match 'foreach \(\$old in \$src, \$qt, \$temp, \$inno\)') 'build_release.ps1 unpacks Inno Setup anew at every run'
Check ($releaseBuild -match 'foreach \(\$file in \$setupFile, \$twinFile\) \{\s+if \(Test-NamesFolder \$file \$Root\)') 'build_release.ps1 refuses a setup that names the folder it was built in'
Check ($releaseBuild -match 'setup = \[ordered\]@\{ name = \$setupName; file = "setup/\$setupName"' -and $releaseBuild -match 'setupTest = ''passed''') 'the build record names the setup and its test'

# --- The publish preset as committed here.
$preset = Get-PublishPreset $desktop
Check ($preset.Feed -ceq $PublicUpdateFeed) 'the publish preset asks the public feed'
Check (-not $preset.PublicKey -or (Test-ReleasePublicKey $preset.PublicKey)) 'the publish preset holds no key yet, or one that can be pinned'
Check ((Get-ProjectVersion $desktop) -match '^\d+\.\d+\.\d+$') 'the tree says its version in three numbers'

# --- What build_release.ps1 refuses before it begins.
$git = (Get-Command git -CommandType Application | Select-Object -First 1).Source
function New-FixtureExport([string] $Name, [string] $Key = '', [string] $Feed = $PublicUpdateFeed, [string[]] $Extra = @(), [string] $Inherits = 'release') {
    $folder = Join-Path $work $Name
    New-Item -ItemType Directory -Path "$folder/tools" | Out-Null
    [IO.File]::WriteAllText("$folder/CMakeLists.txt", "cmake_minimum_required(VERSION 3.25)`nproject(QkvmDesktop VERSION 0.1.0 LANGUAGES CXX)`n")
    $presets = [ordered]@{ version = 6; configurePresets = @(
        [ordered]@{ name = 'release'; cacheVariables = @{ CMAKE_BUILD_TYPE = 'Release' } }
        [ordered]@{ name = 'publish'; inherits = $Inherits; cacheVariables = [ordered]@{ QKVM_UPDATE_FEED = $Feed; QKVM_UPDATE_PUBLIC_KEY = $Key } }
    ) }
    [IO.File]::WriteAllText("$folder/CMakePresets.json", ($presets | ConvertTo-Json -Depth 5))
    foreach ($file in @('tools/build_release.ps1', 'tools/package.ps1') + $Extra) {
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent "$folder/$file") | Out-Null
        [IO.File]::WriteAllText("$folder/$file", "# made-up`n")
    }
    & $git -C $folder init -q 2>&1 | Out-Null
    & $git -C $folder add -A 2>&1 | Out-Null
    & $git -C $folder -c user.name=fixture -c user.email=fixture@example.invalid -c core.autocrlf=false commit -q -m fixture 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "The made-up export $Name could not be committed." }
    return $folder
}
function Invoke-Check([string[]] $Arguments) {
    $said = & (Get-Process -Id $PID).Path -NoProfile -NonInteractive -File "$PSScriptRoot/build_release.ps1" @Arguments -CheckOnly 2>&1
    return [pscustomobject]@{ Ended = $LASTEXITCODE; Said = ($said | ForEach-Object { "$_" }) -join "`n" }
}
function Expect-Build([string[]] $Arguments, [string] $Expected, [string] $Message) {
    $result = Invoke-Check $Arguments
    Check ($result.Ended -eq 0 -and $result.Said -match $Expected) "$Message (exit $($result.Ended): $($result.Said))"
}
function Expect-Refusal([string[]] $Arguments, [string] $Expected, [string] $Message) {
    $result = Invoke-Check $Arguments
    Check ($result.Ended -ne 0 -and $result.Said -match $Expected) "$Message (exit $($result.Ended): $($result.Said))"
}

$good = New-FixtureExport 'good' -Key $key
$commit = (& $git -C $good rev-parse HEAD).Trim()
$newRoot = Join-Path $work 'root-new'
Expect-Build @('-Export', $good, '-Root', $newRoot) "Would build qKVM Desktop 0\.1\.0 from $commit" 'an export with a committed key is built'
Check (-not (Test-Path -LiteralPath $newRoot)) 'asking leaves no build folder behind'
$unset = New-FixtureExport 'unset'
Expect-Build @('-Export', $unset, '-Root', $newRoot, '-Trial') 'Would build .*\(TRIAL\)' 'a trial is built with no key in the preset'
Expect-Refusal @('-Export', $unset, '-Root', $newRoot) 'holds no public key yet' 'without a key in the preset nothing is built'
$wrongKey = New-FixtureExport 'wrong-key' -Key ($key.Substring(0, 127) + $last)
Expect-Refusal @('-Export', $wrongKey, '-Root', $newRoot) 'not 128 hexadecimal digits of a point' 'a key that is no point is refused'
$wrongFeed = New-FixtureExport 'wrong-feed' -Key $key -Feed 'https://example.com/stable.json'
Expect-Refusal @('-Export', $wrongFeed, '-Root', $newRoot) 'feed is not' 'another feed is refused'
Expect-Refusal @('-Export', $wrongFeed, '-Root', $newRoot, '-Trial') 'feed is not' 'another feed is refused in a trial too'
$wrongBase = New-FixtureExport 'wrong-base' -Key $key -Inherits 'debug'
Expect-Refusal @('-Export', $wrongBase, '-Root', $newRoot) 'must inherit the release preset' 'a publish preset on another base is refused'
[IO.File]::AppendAllText("$good/CMakeLists.txt", "# changed`n")
Expect-Refusal @('-Export', $good, '-Root', $newRoot) 'not committed' 'a changed file is refused'
& $git -C $good checkout -q -- CMakeLists.txt
[IO.File]::WriteAllText("$good/stray.txt", 'left here')
Expect-Refusal @('-Export', $good, '-Root', $newRoot) 'not committed' 'a file left in the export is refused'
Remove-Item -LiteralPath "$good/stray.txt"
Expect-Refusal @('-Export', "$good/tools", '-Root', $newRoot) 'not the top of a git repository' 'a folder inside a repository is refused'
Expect-Refusal @('-Export', $work, '-Root', $newRoot) 'not the top of a git repository|no CMakeLists' 'a folder that is no export is refused'
$notDesktop = New-FixtureExport 'not-desktop' -Key $key
& $git -C $notDesktop rm -q tools/build_release.ps1 2>&1 | Out-Null
& $git -C $notDesktop -c user.name=fixture -c user.email=fixture@example.invalid commit -q -m less 2>&1 | Out-Null
Expect-Refusal @('-Export', $notDesktop, '-Root', $newRoot) 'no tools/build_release.ps1' 'a tree that is not the desktop''s is refused'
$withDeps = New-FixtureExport 'with-deps' -Key $key -Extra @('.deps/ffmpeg/x.txt')
Expect-Refusal @('-Export', $withDeps, '-Root', $newRoot) 'never committed' 'an export that holds this PC''s dependencies is refused'
$withLocal = New-FixtureExport 'with-local' -Key $key -Extra @('local.cmake')
Expect-Refusal @('-Export', $withLocal, '-Root', $newRoot) 'never committed' 'an export that holds local.cmake is refused'
$foreign = Join-Path $work 'foreign-root'
New-Item -ItemType Directory -Path $foreign | Out-Null
[IO.File]::WriteAllText("$foreign/somebody.txt", 'not the script''s')
Expect-Refusal @('-Export', $good, '-Root', $foreign) 'did not make' 'a folder with something else in it is never used'
$ours = Join-Path $work 'our-root'
New-Item -ItemType Directory -Path "$ours/src" | Out-Null
[IO.File]::WriteAllText("$ours/.qkvm-release-root", 'made by build_release.ps1')
Expect-Build @('-Export', $good, '-Root', $ours) 'Would build' 'a folder the script made before is used again'
Expect-Refusal @('-Export', $good, '-Root', "$good/build-here") 'must be apart' 'a build folder inside the export is refused'

if ($failures) { Write-Output "$failures release build check(s) failed."; exit 1 }
Remove-Item -LiteralPath $work -Recurse -Force
Write-Output 'Release build checks passed: keys, symbols paths, digests, the publish preset, and the exports and folders that are refused.'
