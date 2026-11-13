#Requires -Version 7.2
<#
The release audit on made-up packages: what it refuses outright (unrecorded, tampered, escaping, symbols), and the
gates of a beta (-Beta): passed only when every open gate is one the owner accepted, never with the notices or
FFmpeg open. Every failed check is a "FAIL:" line, and the script then fails.
#>
$ErrorActionPreference = 'Stop'
$failures = 0
function Check([bool] $Condition, [string] $Message) {
    # (Written to the host: a check inside a function whose output is dropped is still seen.)
    if (-not $Condition) { Write-Host "FAIL: $Message"; $script:failures++ }
}
$root = Join-Path (Split-Path -Parent $PSScriptRoot) "build/audit-tests/$([guid]::NewGuid().ToString('N'))"
$feed = 'https://download.qkvm.com/desktop/stable.json'
$key = 'ab' * 64

function New-Fixture([string] $Name, [hashtable] $Files) {
    $package = (New-Item -ItemType Directory -Path "$root/$Name" -Force).FullName
    foreach ($file in $Files.Keys) {
        $path = Join-Path $package $file
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
        [IO.File]::WriteAllBytes($path, [Text.Encoding]::Latin1.GetBytes($Files[$file]))
    }
    Save-Manifest $package
    return $package
}
function Save-Manifest([string] $Package) {
    $entries = @(Get-ChildItem -LiteralPath $Package -Recurse -File | Where-Object Name -ne 'package-manifest.json' | ForEach-Object {
        @{ path = [IO.Path]::GetRelativePath($Package, $_.FullName).Replace('\', '/'); bytes = $_.Length
           sha256 = (Get-FileHash -LiteralPath $_.FullName).Hash.ToLowerInvariant() }
    })
    @{ product = 'qKVM'; version = '0.1.0'; architecture = 'x64'; files = $entries } |
        ConvertTo-Json -Depth 4 | Set-Content -LiteralPath "$Package/package-manifest.json" -Encoding utf8
}
function Invoke-Audit([string] $Package, [string] $Name, [switch] $Beta, [switch] $RequireRelease, [switch] $MsixPayload, [string] $Setup = '') {
    $record = "$root/$Name.json"
    $refused = ''
    try {
        $withSetup = if ($Setup) { @{ SetupDirectory = $Setup } } else { @{} }
        & "$PSScriptRoot/audit_release.ps1" -PackageDirectory $Package -OutputFile $record -Beta:$Beta -RequireRelease:$RequireRelease -MsixPayload:$MsixPayload @withSetup | Out-Null
    } catch {
        $refused = "$($_.Exception.Message)"
    }
    $said = if (Test-Path -LiteralPath $record) { Get-Content -LiteralPath $record -Raw | ConvertFrom-Json } else { $null }
    return [pscustomobject]@{ Refused = $refused; Record = $said }
}
function Gate($Record, [string] $Id) { return @($Record.gates | Where-Object id -eq $Id)[0] }
# A made-up 64-bit Windows image, never run: one section with the import table of $Imports (and the delay-import table
# of $Delayed), and $Text after it. As the text a fixture's file is written from.
function New-Pe([string[]] $Imports = @('KERNEL32.dll'), [string] $Text = '', [string[]] $Delayed = @()) {
    $rva = 0x1000
    $importsAt = 0
    $delayedAt = ($Imports.Count + 1) * 20
    $namesAt = $delayedAt + ($Delayed.Count + 1) * 32
    $data = [Collections.Generic.List[byte]]::new([byte[]]::new($namesAt))
    $put = { param([int] $At, [uint32] $Value) $bytes = [BitConverter]::GetBytes($Value); for ($i = 0; $i -lt 4; ++$i) { $data[$At + $i] = $bytes[$i] } }
    for ($i = 0; $i -lt $Imports.Count; ++$i) {
        & $put ($importsAt + 20 * $i + 12) ($rva + $data.Count)
        $data.AddRange([Text.Encoding]::ASCII.GetBytes("$($Imports[$i])`0"))
    }
    for ($i = 0; $i -lt $Delayed.Count; ++$i) {
        & $put ($delayedAt + 32 * $i) 1
        & $put ($delayedAt + 32 * $i + 4) ($rva + $data.Count)
        $data.AddRange([Text.Encoding]::ASCII.GetBytes("$($Delayed[$i])`0"))
    }
    $image = [byte[]]::new(0x200 + $data.Count)
    $set = { param([int] $At, [long] $Value, [int] $Width) [Array]::Copy([BitConverter]::GetBytes($Value), 0, $image, $At, $Width) }
    $image[0] = 0x4D; $image[1] = 0x5A
    & $set 0x3C 0x40 4
    & $set 0x40 0x4550 4                  # PE
    & $set 0x44 0x8664 2                  # x64
    & $set 0x46 1 2                       # one section
    & $set 0x54 240 2                     # the optional header's size
    & $set 0x58 0x20B 2                   # PE32+
    & $set (0x58 + 108) 16 4              # data directories
    & $set (0x58 + 112 + 8) ($rva + $importsAt) 4
    & $set (0x58 + 112 + 12) (($Imports.Count + 1) * 20) 4
    if ($Delayed.Count) {
        & $set (0x58 + 112 + 13 * 8) ($rva + $delayedAt) 4
        & $set (0x58 + 112 + 13 * 8 + 4) (($Delayed.Count + 1) * 32) 4
    }
    $section = 0x58 + 240
    [Text.Encoding]::ASCII.GetBytes('.rdata').CopyTo($image, $section)
    & $set ($section + 8) $data.Count 4   # its size in memory
    & $set ($section + 12) $rva 4
    & $set ($section + 16) $data.Count 4  # and in the file
    & $set ($section + 20) 0x200 4
    $data.ToArray().CopyTo($image, 0x200)
    return [Text.Encoding]::Latin1.GetString($image) + $Text
}

# --- An incomplete, unsigned package.
$bare = New-Fixture 'bare' @{ 'qkvm-desktop.exe' = 'disposable unsigned fixture; never executed' }
$result = Invoke-Audit $bare 'bare'
Check (-not $result.Refused) "an incomplete package is audited, not refused ($($result.Refused))"
Check ($result.Record.integrityVerified -and -not $result.Record.productionReady -and -not $result.Record.requiredNoticesPresent) 'an unsigned incomplete package is not ready'
Check ($result.Record.profile -eq 'development') 'the audit without -Beta says so'
$result = Invoke-Audit $bare 'bare-beta' -Beta
Check ($result.Refused -match 'not a beta: notices, ffmpeg, update-channel') "an incomplete package is no beta ($($result.Refused))"
$result = Invoke-Audit $bare 'bare-release' -RequireRelease
Check ($result.Refused -match 'has not passed the production release gates') 'an incomplete package is no release'

# --- What is refused outright.
[IO.File]::WriteAllText("$bare/unrecorded.txt", 'extra')
Check ((Invoke-Audit $bare 'extra').Refused -match 'Unrecorded package file') 'an unrecorded file is refused'
Remove-Item -LiteralPath "$bare/unrecorded.txt"
[IO.File]::AppendAllText("$bare/qkvm-desktop.exe", 'tampered')
Check ((Invoke-Audit $bare 'tampered').Refused -match 'differ from the inventory') 'a changed file is refused'
@{ product = 'qKVM'; version = '0.1.0'; architecture = 'x64'; files = @(@{ path = '../outside.txt'; bytes = 0; sha256 = '0' }) } |
    ConvertTo-Json -Depth 4 | Set-Content -LiteralPath "$bare/package-manifest.json" -Encoding utf8
Check ((Invoke-Audit $bare 'traversal').Refused -match 'unsafe or duplicate path') 'a path out of the package is refused'

# --- A package that is a beta: every notice, every library named, the minimal FFmpeg, the feed and a key built in.
$notices = @('licenses/AGPL-3.0.txt', 'licenses/LGPL-3.0.txt', 'licenses/GPL-3.0.txt', 'licenses/Feather-MIT.txt',
    'licenses/Unicode-CLDR.txt', 'licenses/FFmpeg-LGPL-3.0.txt', 'licenses/libdatachannel-MPL-2.0.txt',
    'licenses/libjuice-MPL-2.0.txt', 'licenses/libsrtp-BSD.txt', 'licenses/usrsctp-BSD.txt', 'licenses/plog-MIT.txt',
    'licenses/Mbed-TLS-license.txt', 'licenses/Everest-notice.md', 'licenses/p256-m-integration-notice.md',
    'licenses/p256-m-notice.md', 'licenses/qt-sbom/qtbase-6.12.0.spdx.json',
    'licenses/qt-sbom/qtdeclarative-6.12.0.spdx.json', 'licenses/qt-sbom/qtsvg-6.12.0.spdx.json',
    'licenses/Qt-bundled-parts.md', 'licenses/zlib.txt', 'licenses/Inno-Setup-license.txt',
    'licenses/RemObjects-Pascal-Script-license.md')
$minimal = "`0--prefix=/ffmpeg --enable-shared --enable-version3 --disable-autodetect --disable-everything --enable-d3d11va --enable-zlib --enable-decoder='h264,mjpeg,png' --extra-ldflags='-L../zlib -static-libgcc'`0"
# What the MinGW-w64 compiler leaves in what it makes: its runtime's failure text, and its own mark.
$mingw = "`0Mingw-w64 runtime failure:`n`0`0GCC: (GNU) 13-win32`0"
# The project's FFmpeg as it is built: Windows' own libraries imported, the MinGW-w64 runtime linked in.
function Avcodec([string] $Configuration = $minimal) { return New-Pe @('KERNEL32.dll', 'msvcrt.dll') "fixture$Configuration$mingw" }
$listing = "| Qt | ``Qt6Core.dll`` |`n| FFmpeg | ``avcodec-63.dll`` |`n| The MinGW-w64 runtime, linked into avcodec |`n"
function Beta-Files([hashtable] $Change = @{}) {
    $files = @{
        'qkvm-desktop.exe' = New-Pe @('Qt6Core.dll', 'avcodec-63.dll', 'KERNEL32.dll', 'api-ms-win-crt-runtime-l1-1-0.dll') "`0$feed`0$key`0"
        'THIRD-PARTY.md' = $listing
        'Qt6Core.dll' = New-Pe @('KERNEL32.dll', 'USER32.dll') -Delayed @('UxTheme.dll')
        'avcodec-63.dll' = Avcodec
        'licenses/mingw-w64-runtime.txt' = 'notice'
    }
    foreach ($notice in $notices) { $files[$notice] = 'notice' }
    foreach ($name in $Change.Keys) {
        if ($null -eq $Change[$name]) { $files.Remove($name) } else { $files[$name] = $Change[$name] }
    }
    return $files
}
$beta = New-Fixture 'beta' (Beta-Files)
$result = Invoke-Audit $beta 'beta' -Beta
Check (-not $result.Refused) "a beta is taken as one ($($result.Refused))"
Check ($result.Record.profile -eq 'beta' -and $result.Record.betaAccepted) 'the record says it is a beta'
Check (-not (Gate $result.Record 'notices').open -and -not (Gate $result.Record 'ffmpeg').open) 'the notices and FFmpeg gates are closed by what the package holds'
foreach ($id in 'sources', 'signatures', 'installer', 'update-channel') {
    $gate = Gate $result.Record $id
    Check ($gate.open -and $gate.beta -match '^Owner, 2026-11-06 \(D') "the open gate $id carries the owner's reason"
}
Check ($result.Record.ffmpegConfiguration -match '--disable-everything') 'the record says how FFmpeg was configured'
Check ($result.Record.updateChannelBuiltIn) 'the record says the update channel is built in'
Check (-not $result.Record.productionReady) 'a beta is not production ready'
Check ((Invoke-Audit $beta 'beta-release' -RequireRelease).Refused -match 'production release gates') 'a beta is no release'

function Expect-NoBeta([string] $Name, [hashtable] $Change, [string] $Gate) {
    $package = New-Fixture $Name (Beta-Files $Change)
    $result = Invoke-Audit $package $Name -Beta
    Check ($result.Refused -match "not a beta: .*$Gate") "$Name is no beta ($($result.Refused))"
    Check ((Gate $result.Record $Gate).open) "$Name leaves the $Gate gate open"
    return $result
}
$found = Expect-NoBeta 'unlisted-library' @{ 'Qt6Gui.dll' = New-Pe } 'notices'
Check (@($found.Record.unlistedLibraries) -contains 'Qt6Gui.dll') 'the record names the library that is not listed'
Expect-NoBeta 'missing-notice' @{ 'licenses/plog-MIT.txt' = $null } 'notices' | Out-Null
# (zlib's, inside the project's own FFmpeg; and the parts inside Qt's files.)
Expect-NoBeta 'missing-zlib' @{ 'licenses/zlib.txt' = $null } 'notices' | Out-Null
Expect-NoBeta 'missing-qt-parts' @{ 'licenses/Qt-bundled-parts.md' = $null } 'notices' | Out-Null
# (Inno Setup's, whose setup and uninstall program the setup carries and leaves in the program's folder.)
Expect-NoBeta 'missing-inno-licence' @{ 'licenses/Inno-Setup-license.txt' = $null } 'notices' | Out-Null
# (And RemObjects Pascal Script's, which Inno Setup's runtime carries.)
Expect-NoBeta 'missing-pascal-script-licence' @{ 'licenses/RemObjects-Pascal-Script-license.md' = $null } 'notices' | Out-Null
Expect-NoBeta 'no-third-party' @{ 'THIRD-PARTY.md' = $null } 'notices' | Out-Null
Expect-NoBeta 'ffmpeg-with-a-library' @{ 'avcodec-63.dll' = Avcodec "`0--disable-everything --enable-libdav1d --enable-version3`0" } 'ffmpeg' | Out-Null
Expect-NoBeta 'ffmpeg-gpl' @{ 'avcodec-63.dll' = Avcodec "`0--disable-everything --enable-gpl --enable-version3`0" } 'ffmpeg' | Out-Null
Expect-NoBeta 'ffmpeg-everything' @{ 'avcodec-63.dll' = Avcodec "`0--prefix=/x --enable-decoder=h264 --enable-version3`0" } 'ffmpeg' | Out-Null
Expect-NoBeta 'ffmpeg-unknown' @{ 'avcodec-63.dll' = Avcodec 'fixture with no configuration in it' } 'ffmpeg' | Out-Null
# (Nothing found by itself on the machine it was built on, and no outside library under any name: the rule of
# build_common.ps1's Get-FFmpegRefusals, which test_build_release.ps1 tries case by case.)
$found = Expect-NoBeta 'ffmpeg-autodetect' @{ 'avcodec-63.dll' = Avcodec ($minimal -replace ' --disable-autodetect', '') } 'ffmpeg'
Check (@(@($found.Record.ffmpegRefusals) -match 'does not say --disable-autodetect').Count -eq 1) "the record says why FFmpeg is not the minimal build ($($found.Record.ffmpegRefusals))"
foreach ($outside in '--enable-openssl', '--enable-gnutls', '--enable-mbedtls', '--enable-decoder=libdav1d') {
    Expect-NoBeta "ffmpeg$($outside -replace '[^a-z0-9]', '-')" @{ 'avcodec-63.dll' = Avcodec ($minimal -replace ' --enable-zlib', " --enable-zlib $outside") } 'ffmpeg' | Out-Null
}
Expect-NoBeta 'no-feed' @{ 'qkvm-desktop.exe' = New-Pe @('KERNEL32.dll') "`0https://example.com/stable.json`0$key`0" } 'update-channel' | Out-Null
Expect-NoBeta 'no-key' @{ 'qkvm-desktop.exe' = New-Pe @('KERNEL32.dll') "`0$feed`0`0" } 'update-channel' | Out-Null

# --- What each library and program links, read from its own bytes rather than from its name: a runtime linked into
# it (the MinGW-w64 runtime inside the project's FFmpeg) has its notice in the package and its name in THIRD-PARTY.md,
# and every library it imports ships here or is Windows' own.
$linkedBeta = (Invoke-Audit $beta 'beta-links' -Beta).Record
$avcodecLinks = @($linkedBeta.links | Where-Object path -eq 'avcodec-63.dll')
Check ($avcodecLinks.Count -eq 1 -and @($avcodecLinks[0].runtimes) -contains 'MinGW-w64 runtime' -and
    @($avcodecLinks[0].imports) -contains 'msvcrt.dll') "the record says what avcodec links ($($avcodecLinks | ConvertTo-Json -Compress))"
$qtLinks = @($linkedBeta.links | Where-Object path -eq 'Qt6Core.dll')
Check ($qtLinks.Count -eq 1 -and @($qtLinks[0].imports) -contains 'UxTheme.dll' -and @($qtLinks[0].runtimes).Count -eq 0) 'a library''s delay-loaded imports are read too, and an MSVC library links no runtime of its own'
$found = Expect-NoBeta 'runtime-without-notice' @{ 'licenses/mingw-w64-runtime.txt' = $null } 'notices'
Check (@($found.Record.missingNotices) -contains 'licenses/mingw-w64-runtime.txt') 'the record names the notice of the runtime that is linked in'
$found = Expect-NoBeta 'runtime-not-listed' @{ 'THIRD-PARTY.md' = "| Qt | ``Qt6Core.dll`` |`n| FFmpeg | ``avcodec-63.dll`` |`n" } 'notices'
Check (@(@($found.Record.unlistedLinks) -match 'MinGW-w64 runtime.*avcodec-63\.dll').Count -gt 0) "the record names the runtime THIRD-PARTY.md does not, and where it is linked ($($found.Record.unlistedLinks))"
# (By the compiler's own mark alone, as a build keeps it that has no failure text of the runtime's; and in any library.)
Expect-NoBeta 'runtime-by-its-mark' @{ 'licenses/mingw-w64-runtime.txt' = $null
    'avcodec-63.dll' = New-Pe @('KERNEL32.dll') "fixture$minimal`0GCC: (GNU) 13-win32`0" } 'notices' | Out-Null
Expect-NoBeta 'runtime-by-its-failure-text' @{ 'licenses/mingw-w64-runtime.txt' = $null
    'avcodec-63.dll' = New-Pe @('KERNEL32.dll') "fixture$minimal`0Mingw-w64 runtime failure:`n`0" } 'notices' | Out-Null
Expect-NoBeta 'runtime-in-another-library' @{ 'licenses/mingw-w64-runtime.txt' = $null
    'avcodec-63.dll' = New-Pe @('KERNEL32.dll') "fixture$minimal"; 'Qt6Core.dll' = New-Pe @('KERNEL32.dll') $mingw } 'notices' | Out-Null
$noRuntime = New-Fixture 'no-runtime' (Beta-Files @{ 'licenses/mingw-w64-runtime.txt' = $null; 'avcodec-63.dll' = New-Pe @('KERNEL32.dll') "fixture$minimal" })
$result = Invoke-Audit $noRuntime 'no-runtime' -Beta
Check (-not $result.Refused) "where no runtime is linked in, no notice of one is asked for ($($result.Refused))"
$found = Expect-NoBeta 'imports-a-runtime' @{ 'Qt6Core.dll' = New-Pe @('KERNEL32.dll', 'libwinpthread-1.dll') } 'notices'
Check (@(@($found.Record.unlistedLinks) -match 'Qt6Core\.dll.*libwinpthread-1\.dll').Count -gt 0) "the record names the library imported that neither ships nor is Windows' ($($found.Record.unlistedLinks))"
Expect-NoBeta 'delay-loads-a-runtime' @{ 'Qt6Core.dll' = New-Pe @('KERNEL32.dll') -Delayed @('libgcc_s_seh-1.dll') } 'notices' | Out-Null
Expect-NoBeta 'program-imports-openssl' @{ 'qkvm-desktop.exe' = New-Pe @('KERNEL32.dll', 'libcrypto-3-x64.dll') "`0$feed`0$key`0" } 'notices' | Out-Null
$found = Expect-NoBeta 'not-an-image' @{ 'Qt6Core.dll' = 'a library that is no Windows image' } 'notices'
Check (@(@($found.Record.unlistedLinks) -match 'Qt6Core\.dll').Count -gt 0) 'a library whose links cannot be read is named'

# --- The notices of what this repository ships with its FFmpeg: the MinGW-w64 runtime's, as its licence asks, and
# THIRD-PARTY.md naming it, and libgcc under GCC's runtime exception.
$desktop = Split-Path -Parent $PSScriptRoot
$runtimeNotice = Join-Path $desktop 'licenses/mingw-w64-runtime.txt'
$noticeText = if (Test-Path -LiteralPath $runtimeNotice) { [IO.File]::ReadAllText($runtimeNotice) } else { '' }
Check ($noticeText -match 'avcodec-63\.dll and avutil-61\.dll' -and $noticeText -match 'ZPL' -and $noticeText -match 'Redistributions in binary form must reproduce the accompanying') 'the MinGW-w64 runtime''s notice is among the licences, with its conditions'
# And it is the runtime's own notice, unchanged, as the mingw-w64 v11.0.1 source has it
# (COPYING.MinGW-w64-runtime/COPYING.MinGW-w64-runtime.txt, SHA-256 e9b2dc02...): from its first line to the end.
$ownStart = $noticeText.Replace("`r`n", "`n").IndexOf("MinGW-w64 runtime licensing`n***************************`n")
$ownText = if ($ownStart -ge 0) { $noticeText.Replace("`r`n", "`n").Substring($ownStart) } else { '' }
$ownHash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($ownText))).ToLowerInvariant()
Check ($ownHash -eq 'e9b2dc02451ea29092a1f25fa0f3c07207ed421f1807dffb0c4e6dce69dee7bd') "the MinGW-w64 runtime's notice ends with the runtime's own notice from its source, unchanged (found $ownHash)"
$thirdParty = [IO.File]::ReadAllText((Join-Path $desktop 'THIRD-PARTY.md'))
Check ($thirdParty -match 'MinGW-w64 runtime' -and $thirdParty.Contains('licenses/mingw-w64-runtime.txt') -and
    $thirdParty -match 'libgcc' -and $thirdParty -match 'GCC Runtime Library Exception') 'THIRD-PARTY.md names the MinGW-w64 runtime and its notice, and libgcc under its exception'
# (The Qt WebSockets bill of materials is no notice of the package: WebSockets is for the tests alone.)
Check (-not ((Invoke-Audit $beta 'no-websockets' -Beta).Record.missingNotices -match 'qtwebsockets')) 'Qt WebSockets is not asked for'

# --- One program in the portable ZIP; qKVM Setup beside it only in an MSIX; never a test or bench program.
$withSetup = New-Fixture 'with-setup' (Beta-Files @{ 'qkvm-setup.exe' = 'disposable unsigned fixture; never executed' })
Check ((Invoke-Audit $withSetup 'with-setup').Refused -match 'not handed out: qkvm-setup.exe') 'setup in the portable package is refused'
$result = Invoke-Audit $withSetup 'msix' -MsixPayload
Check (-not $result.Refused) "an MSIX's content carries setup ($($result.Refused))"
Check (@($result.Record.signatures.file) -contains 'qkvm-setup.exe') "an MSIX's setup is among the programs whose signatures count"
$noSetup = New-Fixture 'msix-without-setup' (Beta-Files)
Check ((Invoke-Audit $noSetup 'msix-without-setup' -MsixPayload).Refused -match 'Missing release executable: qkvm-setup.exe') 'an MSIX without setup is refused'
$bench = New-Fixture 'with-bench' (Beta-Files @{ 'bench/qkvm-bench.exe' = 'disposable fixture' })
Check ((Invoke-Audit $bench 'with-bench' -Beta).Refused -match 'not handed out: bench/qkvm-bench.exe') 'a bench program is refused'
$testProgram = New-Fixture 'with-test' (Beta-Files @{ 'test_engine_about.exe' = 'disposable fixture' })
Check ((Invoke-Audit $testProgram 'with-test' -MsixPayload).Refused -match 'not handed out: test_engine_about.exe') 'a test program is refused, in an MSIX too'

# --- The setup made from a package (tools/build_setup.ps1), audited beside it: the compiler's list of what it put in is
# the package's manifest, the manifest itself and the marker, and nothing else; its own manifest asks for nothing more
# than the user; it is unsigned, as the beta's accepted gate says. (Its bytes are those of a small program of Windows
# that carries such a manifest, copied and never run: the audit reads its resources only.)
$markerBytes = [byte[]] @(0x73, 0x65, 0x74, 0x75, 0x70, 0x0A)
function New-SetupFixture([string] $Package, [string] $Name, [scriptblock] $Change = {}) {
    $setup = (New-Item -ItemType Directory -Path "$root/$Name" -Force).FullName
    New-Item -ItemType Directory -Path "$setup/inputs" | Out-Null
    Copy-Item -LiteralPath "$env:SystemRoot\System32\find.exe" -Destination "$setup/qKVM-Desktop-0.1.0-windows-x64-setup.exe"
    [IO.File]::WriteAllBytes("$setup/inputs/qkvm-desktop.installed", $markerBytes)
    $rows = [Collections.Generic.List[string]]::new()
    $rows.Add("Index`tSourceFilename`tTimeStamp`tVersion`tSHA256Sum`tOriginalSize`tFirstSlice`tLastSlice`tStartOffset`tChunkSuboffset`tChunkCompressedSize`tEncrypted`tISSigKeyID")
    $sources = @(Get-ChildItem -LiteralPath $Package -Recurse -File | ForEach-Object FullName) + "$setup\inputs\qkvm-desktop.installed"
    foreach ($source in $sources) {
        $rows.Add("$($rows.Count - 1)`t$($source -replace '/', '\')`t2026-11-08 00:00:00.000`t`t$((Get-FileHash -LiteralPath $source).Hash.ToLowerInvariant())`t$((Get-Item -LiteralPath $source).Length)`t1`t1`t0`t0`t1`tno`t")
    }
    & $Change ([pscustomobject]@{ Folder = $setup; Rows = $rows })
    Set-Content -LiteralPath "$setup/setup-files.txt" -Value $rows -Encoding utf8
    return $setup
}
function Edit-Row($Fixture, [string] $Like, [scriptblock] $To) {
    $at = -1
    for ($i = 1; $i -lt $Fixture.Rows.Count; ++$i) { if ($Fixture.Rows[$i].Contains($Like)) { $at = $i } }
    if ($at -lt 0) { throw "No row of $Like." }
    $Fixture.Rows[$at] = & $To $Fixture.Rows[$at]
}
$setupBeta = New-Fixture 'setup-beta' (Beta-Files)
$result = Invoke-Audit $setupBeta 'setup-beta' -Beta -Setup (New-SetupFixture $setupBeta 'setup-good')
Check (-not $result.Refused) "a beta with its setup is taken as one ($($result.Refused))"
if (-not $result.Refused) {
    Check ($result.Record.setup.name -eq 'qKVM-Desktop-0.1.0-windows-x64-setup.exe' -and $result.Record.setup.files -eq @(Get-ChildItem -LiteralPath $setupBeta -Recurse -File).Count + 1 -and
        $result.Record.setup.manifestLevel -eq 'asInvoker') "the record says what the setup holds and asks for ($($result.Record.setup | ConvertTo-Json -Compress))"
    Check (@($result.Record.signatures.file) -contains 'qKVM-Desktop-0.1.0-windows-x64-setup.exe') 'the setup is among the programs whose signatures count'
    Check ((Gate $result.Record 'installer').beta -match '^Owner, 2026-11-08: ' -and (Gate $result.Record 'signatures').beta -match 'setup') 'the open installer and signature gates say the owner accepted the unsigned setup'
}
function Expect-SetupRefusal([string] $Name, [scriptblock] $Change, [string] $Expected) {
    $setup = New-SetupFixture $setupBeta $Name $Change
    $result = Invoke-Audit $setupBeta $Name -Beta -Setup $setup
    Check ($result.Refused -match $Expected) "a setup $Name is refused ($($result.Refused))"
}
Expect-SetupRefusal 'with-a-file-more' { param($f) $f.Rows.Add("99`t$setupBeta\stray.dll`t2026-11-08 00:00:00.000`t`t$('0' * 64)`t1`t1`t1`t0`t0`t1`tno`t") } 'holds a file the package does not have'
Expect-SetupRefusal 'with-a-file-less' { param($f) $f.Rows.RemoveAt(@(1..($f.Rows.Count - 1) | Where-Object { $f.Rows[$_].Contains('Qt6Core.dll') })[0]) } 'lacks Qt6Core\.dll'
Expect-SetupRefusal 'with-other-bytes' { param($f) Edit-Row $f 'Qt6Core.dll' { param($row) $row -replace '\t[0-9a-f]{64}\t', "`t$('0' * 64)`t" } } 'other bytes .*Qt6Core\.dll'
Expect-SetupRefusal 'with-a-file-twice' { param($f) $f.Rows.Add(($f.Rows | Where-Object { $_.Contains('Qt6Core.dll') } | Select-Object -First 1)) } 'Qt6Core\.dll twice'
Expect-SetupRefusal 'with-another-marker' { param($f) [IO.File]::WriteAllText("$($f.Folder)/inputs/qkvm-desktop.installed", 'setup'); Edit-Row $f 'qkvm-desktop.installed' { param($row) $row -replace '\t[0-9a-f]{64}\t6\t', "`t$((Get-FileHash -LiteralPath "$($f.Folder)/inputs/qkvm-desktop.installed").Hash.ToLowerInvariant())`t5`t" } } 'marker'
Expect-SetupRefusal 'from-another-package' { param($f) for ($i = 1; $i -lt $f.Rows.Count; ++$i) { $f.Rows[$i] = $f.Rows[$i].Replace("$setupBeta\", "$setupBeta-other\") } } 'holds a file the package does not have'
Expect-SetupRefusal 'encrypted' { param($f) Edit-Row $f 'Qt6Core.dll' { param($row) $row -replace '\tno\t$', "`tyes`t" } } 'other bytes .*Qt6Core\.dll'
Expect-SetupRefusal 'not-there' { param($f) Remove-Item -LiteralPath "$($f.Folder)/qKVM-Desktop-0.1.0-windows-x64-setup.exe" } 'has no qKVM-Desktop-0\.1\.0-windows-x64-setup\.exe'
Expect-SetupRefusal 'for-an-administrator' { param($f)
    $file = "$($f.Folder)/qKVM-Desktop-0.1.0-windows-x64-setup.exe"
    $text = [Text.Encoding]::Latin1.GetString([IO.File]::ReadAllBytes($file))
    if (-not $text.Contains('level="asInvoker"')) { throw 'The fixture program has no level to change.' }
    [IO.File]::WriteAllBytes($file, [Text.Encoding]::Latin1.GetBytes($text.Replace('level="asInvoker"', 'level="asAdminxx"'))) } 'asks to run as asAdminxx'
Check ((Invoke-Audit $setupBeta 'setup-msix' -MsixPayload -Setup (New-SetupFixture $setupBeta 'setup-msix-folder')).Refused -match 'MSIX') 'a setup is not audited beside an MSIX'

# --- Symbols are never in a package, recorded or not.
$symbols = New-Fixture 'symbols' (Beta-Files @{ 'qkvm-desktop.pdb' = 'symbols' })
Check ((Invoke-Audit $symbols 'symbols').Refused -match 'holds symbols') 'a package with symbols is refused'
Check ((Invoke-Audit $symbols 'symbols-beta' -Beta).Refused -match 'holds symbols') 'a beta with symbols is refused'

# (A throw, not an exit: tools/check.ps1 calls this in its own PowerShell and counts what is thrown.)
if ($failures) { throw "$failures release audit check(s) failed." }
Remove-Item -LiteralPath $root -Recurse -Force
Write-Output 'Release audit checks passed: incomplete, unsigned, unrecorded, tampered, escaping and symbol-carrying packages are not released, and a beta is one only with every open gate accepted.'
