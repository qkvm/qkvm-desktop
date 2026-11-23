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

function New-Fixture([string] $Name, [hashtable] $Files, [string] $Architecture = 'x64') {
    $package = (New-Item -ItemType Directory -Path "$root/$Name" -Force).FullName
    foreach ($file in $Files.Keys) {
        $path = Join-Path $package $file
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
        [IO.File]::WriteAllBytes($path, [Text.Encoding]::Latin1.GetBytes($Files[$file]))
    }
    Save-Manifest $package $Architecture
    return $package
}
function Save-Manifest([string] $Package, [string] $Architecture = 'x64') {
    $entries = @(Get-ChildItem -LiteralPath $Package -Recurse -File | Where-Object Name -ne 'package-manifest.json' | ForEach-Object {
        @{ path = [IO.Path]::GetRelativePath($Package, $_.FullName).Replace('\', '/'); bytes = $_.Length
           sha256 = (Get-FileHash -LiteralPath $_.FullName).Hash.ToLowerInvariant() }
    })
    @{ product = 'qKVM'; version = '0.1.0'; architecture = $Architecture; files = $entries } |
        ConvertTo-Json -Depth 4 | Set-Content -LiteralPath "$Package/package-manifest.json" -Encoding utf8
}
$ffmpegLibrary = '^(avcodec|avutil|avformat|avfilter|avdevice|swresample|swscale|postproc)-\d+\.dll$'
function New-FFmpegRecordOf([string] $Package, [string] $Name, [string] $Kind = '', [string[]] $Kinds = @()) {
    # A made-up record of a release's FFmpeg part whose build of the package's kind (or $Kind) made the very FFmpeg DLLs
    # the package holds, as build_release.ps1 -Part ffmpeg names them (bin/<name>, size, SHA-256); with $Kinds, a build
    # of each of those instead, the same DLLs in each.
    $manifest = Get-Content -LiteralPath "$Package/package-manifest.json" -Raw | ConvertFrom-Json
    if (-not $Kind) { $Kind = [string] $manifest.architecture }
    if (-not $Kinds.Count) { $Kinds = @($Kind) }
    $dlls = @($manifest.files | Where-Object { $_.path.Split('/')[-1] -match $ffmpegLibrary } | ForEach-Object {
        [ordered]@{ name = "bin/$($_.path.Split('/')[-1])"; bytes = $_.bytes; sha256 = $_.sha256 } })
    $file = "$root/$Name-ffmpeg-record.json"
    [ordered]@{ schema = 1; product = 'qKVM Desktop'; part = 'ffmpeg'
        builds = @($Kinds | ForEach-Object { [ordered]@{ architecture = $_; dlls = $dlls } }) } |
        ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $file -Encoding utf8
    return $file
}
# (Every audit here is given the record of an FFmpeg part that made the package's own FFmpeg DLLs, unless the case says
# another record, or none with '-'.)
function Invoke-Audit([string] $Package, [string] $Name, [switch] $Beta, [switch] $RequireRelease, [switch] $MsixPayload, [string] $Setup = '', [string[]] $SetupPackages = @(), [string] $FFmpegRecord = '') {
    $record = "$root/$Name.json"
    $refused = ''
    try {
        $withSetup = if ($Setup) { @{ SetupDirectory = $Setup } } else { @{} }
        if ($SetupPackages.Count) { $withSetup['SetupPackageDirectory'] = $SetupPackages }
        if (-not $FFmpegRecord -and (Test-Path -LiteralPath "$Package/package-manifest.json")) {
            try { $FFmpegRecord = New-FFmpegRecordOf $Package $Name } catch { $FFmpegRecord = '-' }
        }
        if ($FFmpegRecord -and $FFmpegRecord -cne '-') { $withSetup['FFmpegRecord'] = $FFmpegRecord }
        & "$PSScriptRoot/audit_release.ps1" -PackageDirectory $Package -OutputFile $record -Beta:$Beta -RequireRelease:$RequireRelease -MsixPayload:$MsixPayload @withSetup | Out-Null
    } catch {
        $refused = "$($_.Exception.Message)"
    }
    $said = if (Test-Path -LiteralPath $record) { Get-Content -LiteralPath $record -Raw | ConvertFrom-Json } else { $null }
    return [pscustomobject]@{ Refused = $refused; Record = $said }
}
function Gate($Record, [string] $Id) { return @($Record.gates | Where-Object id -eq $Id)[0] }
# A made-up 64-bit Windows image, never run: one section with the import table of $Imports (and the delay-import table
# of $Delayed), and $Text after it, for the machine $Machine (x64's unless said: 0xAA64 is ARM64's). As the text a
# fixture's file is written from.
function New-Pe([string[]] $Imports = @('KERNEL32.dll'), [string] $Text = '', [string[]] $Delayed = @(), [uint16] $Machine = 0x8664) {
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
    # (Built as every image that ships must be, tools/image-protections.json: its relocations, a load configuration whose
    # GuardFlags say CFG and EH continuation, extended characteristics that say CET_COMPAT, and for an ARM64 one its four
    # functions signing their returns. Protect-Pe takes some away.)
    while ($data.Count % 4) { $data.Add(0) }
    $guardsAt = $data.Count
    $data.AddRange([byte[]]::new(148 + 28 + 4 + 8 + 32 + 16))
    & $put $guardsAt 148
    & $put ($guardsAt + 144) 0x400100
    & $put ($guardsAt + 148 + 12) 20
    & $put ($guardsAt + 148 + 16) 4
    & $put ($guardsAt + 148 + 20) ($rva + $guardsAt + 176)
    & $put ($guardsAt + 148 + 24) (0x200 + $guardsAt + 176)
    & $put ($guardsAt + 176) 1
    for ($i = 0; $i -lt 4; ++$i) { & $put ($guardsAt + 220 + 4 * $i) 0xD503237Fu }
    $image = [byte[]]::new(0x200 + $data.Count)
    $set = { param([int] $At, [long] $Value, [int] $Width) [Array]::Copy([BitConverter]::GetBytes($Value), 0, $image, $At, $Width) }
    $image[0] = 0x4D; $image[1] = 0x5A
    & $set 0x3C 0x40 4
    & $set 0x40 0x4550 4                  # PE
    & $set 0x44 $Machine 2                # x64, or what is asked
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
    & $set (0x58 + 70) 0x4160 2           # ASLR, high entropy, DEP, CFG
    foreach ($directory in @(@(3, 188, 32), @(5, 180, 8), @(6, 148, 28), @(10, 0, 148))) {
        & $set (0x58 + 112 + 8 * $directory[0]) ($rva + $guardsAt + $directory[1]) 4
        & $set (0x58 + 112 + 8 * $directory[0] + 4) $directory[2] 4
    }
    $section = 0x58 + 240
    [Text.Encoding]::ASCII.GetBytes('.rdata').CopyTo($image, $section)
    & $set ($section + 8) $data.Count 4   # its size in memory
    & $set ($section + 12) $rva 4
    & $set ($section + 16) $data.Count 4  # and in the file
    & $set ($section + 20) 0x200 4
    & $set ($section + 36) 0x60000020 4   # code, run and read
    $data.ToArray().CopyTo($image, 0x200)
    return [Text.Encoding]::Latin1.GetString($image) + $Text
}
# A made-up image of New-Pe's with protections taken away: of its characteristics aslr, highEntropyVa, dep, cfg; cet,
# its extended characteristics' word (where its debug directory's entry points); pac, its four functions' PACIBSP (the
# words after its unwind table).
function Protect-Pe([string] $Pe, [string[]] $Without) {
    $bytes = [Text.Encoding]::Latin1.GetBytes($Pe)
    $characteristics = [BitConverter]::ToUInt16($bytes, 0x58 + 70)
    foreach ($bit in @(@('aslr', 0x40), @('highEntropyVa', 0x20), @('dep', 0x100), @('cfg', 0x4000))) {
        if ($Without -ccontains $bit[0]) { $characteristics = $characteristics -band -bnot $bit[1] }
    }
    [BitConverter]::GetBytes([uint16] $characteristics).CopyTo($bytes, 0x58 + 70)
    # (Where a directory of the one section is in the file: its place in memory, less the section's, from 0x200.)
    $inFile = { param([int] $Index) [BitConverter]::ToInt32($bytes, 0x58 + 112 + 8 * $Index) - 0x1000 + 0x200 }
    if ($Without -ccontains 'cet') { [Array]::Clear($bytes, [BitConverter]::ToInt32($bytes, (& $inFile 6) + 24), 4) }
    if ($Without -ccontains 'pac') { [Array]::Clear($bytes, (& $inFile 3) + 32, 16) }
    return [Text.Encoding]::Latin1.GetString($bytes)
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
# (Windows on Arm's, as build_ffmpeg.ps1 -Arch arm64 makes it: the same line, of ARM64 for mingw32.)
$minimalArm = $minimal.Replace('--prefix=/ffmpeg ', '--prefix=/ffmpeg --target-os=mingw32 --arch=aarch64 --cross-prefix=aarch64-w64-mingw32- --cc=aarch64-w64-mingw32-clang --extra-cflags=-I../zlib ')
# The project's FFmpeg as it is built: Windows' own libraries imported, the MinGW-w64 runtime linked in; of its
# machine's kind unless a configuration is given.
function Avcodec([string] $Configuration = '', [uint16] $Machine = 0x8664) {
    if (-not $Configuration) { $Configuration = if ($Machine -eq 0xAA64) { $minimalArm } else { $minimal } }
    return New-Pe @('KERNEL32.dll', 'msvcrt.dll') "fixture$Configuration$mingw" -Machine $Machine
}
$listing = "| Qt | ``Qt6Core.dll`` |`n| FFmpeg | ``avcodec-63.dll`` |`n| The MinGW-w64 runtime, linked into avcodec |`n"
function Beta-Files([hashtable] $Change = @{}, [uint16] $Machine = 0x8664) {
    $files = @{
        'qkvm-desktop.exe' = New-Pe @('Qt6Core.dll', 'avcodec-63.dll', 'KERNEL32.dll', 'api-ms-win-crt-runtime-l1-1-0.dll') "`0$feed`0$key`0" -Machine $Machine
        'THIRD-PARTY.md' = $listing
        'Qt6Core.dll' = New-Pe @('KERNEL32.dll', 'USER32.dll') -Delayed @('UxTheme.dll') -Machine $Machine
        'avcodec-63.dll' = Avcodec -Machine $Machine
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
Check ($result.Record.architecture -ceq 'x64') "the record says the package is for x64 ($($result.Record.architecture))"

# --- A package for Windows on Arm (0.2.0): the same rules, of ARM64 programs. No package holds a Windows image of
# another kind than its manifest says, and a manifest names x64 or arm64, letter for letter.
$armBeta = New-Fixture 'arm-beta' (Beta-Files -Machine 0xAA64) 'arm64'
$result = Invoke-Audit $armBeta 'arm-beta' -Beta
Check (-not $result.Refused) "a beta for Windows on Arm is taken as one ($($result.Refused))"
Check ($result.Record.betaAccepted -and $result.Record.architecture -ceq 'arm64') 'the record says the package is a beta for arm64'
Check (-not (Gate $result.Record 'notices').open -and -not (Gate $result.Record 'ffmpeg').open -and $result.Record.updateChannelBuiltIn) 'a beta for Arm has its notices, its FFmpeg and its channel as an x64 one'
# (Its FFmpeg is held to Windows on Arm's rule: x64's configuration in an ARM64 avcodec is not the ARM64 build.)
$result = Invoke-Audit (New-Fixture 'arm-with-x64-ffmpeg' (Beta-Files @{ 'avcodec-63.dll' = Avcodec -Configuration $minimal -Machine 0xAA64 } 0xAA64) 'arm64') 'arm-with-x64-ffmpeg' -Beta
Check ($result.Refused -match 'not a beta: ffmpeg' -and (Gate $result.Record 'ffmpeg').open) "an Arm package whose FFmpeg says x64's configuration is no beta ($($result.Refused))"
$mixed = [ordered]@{
    'arm-with-an-x64-library' = @((Beta-Files @{ 'Qt6Core.dll' = New-Pe @('KERNEL32.dll') } 0xAA64), 'arm64', 'holds a program for x64: Qt6Core\.dll')
    'arm-with-an-x64-program' = @((Beta-Files @{ 'qkvm-desktop.exe' = New-Pe @('KERNEL32.dll') "`0$feed`0$key`0" } 0xAA64), 'arm64', 'holds a program for x64: qkvm-desktop\.exe')
    'x64-with-an-arm-library' = @((Beta-Files @{ 'avcodec-63.dll' = Avcodec -Machine 0xAA64 }), 'x64', 'holds a program for arm64: avcodec-63\.dll')
    'arm-said-of-x64' = @((Beta-Files), 'arm64', 'for arm64 Windows holds a program for x64')
    'x64-said-of-arm' = @((Beta-Files -Machine 0xAA64), 'x64', 'for x64 Windows holds a program for arm64')
    'x64-with-an-x86-library' = @((Beta-Files @{ 'Qt6Core.dll' = New-Pe @('KERNEL32.dll') -Machine 0x014C }), 'x64', 'holds a program for machine 0x014C: Qt6Core\.dll')
    'arm-in-capitals' = @((Beta-Files -Machine 0xAA64), 'ARM64', 'inventory is invalid')
    'arm-of-another-name' = @((Beta-Files -Machine 0xAA64), 'aarch64', 'inventory is invalid')
}
foreach ($case in $mixed.GetEnumerator()) {
    $package = New-Fixture $case.Key $case.Value[0] $case.Value[1]
    $refused = (Invoke-Audit $package $case.Key -Beta).Refused
    Check ($refused -match $case.Value[2]) "$($case.Key) is refused ($refused)"
}

# --- FFmpeg's DLLs are the ones the release's FFmpeg part made (build_release.ps1 -Part ffmpeg, on a PC with WSL; the
# package is made on the build box): by name, size and SHA-256 as the part's record names them for the package's kind,
# none more and none fewer. Without the record this is not known, and a beta is not one; the gate is never accepted.
$result = Invoke-Audit $beta 'record-own' -Beta
Check (-not (Gate $result.Record 'ffmpeg-record').open -and $result.Record.ffmpegRecordSha256 -match '^[0-9a-f]{64}$') "a package of the FFmpeg its part made passes ($($result.Refused))"
$result = Invoke-Audit $beta 'record-none' -Beta -FFmpegRecord '-'
Check ($result.Refused -match 'not a beta: ffmpeg-record' -and (Gate $result.Record 'ffmpeg-record').open -and -not (Gate $result.Record 'ffmpeg-record').beta) "without the FFmpeg part's record no package is a beta ($($result.Refused))"
Check (@(@($result.Record.ffmpegRecordRefusals) -match 'no record of the FFmpeg part was given').Count -eq 1) 'the record says the FFmpeg part''s record was not given'
$otherDecoder = New-Fixture 'other-decoder' (Beta-Files @{ 'avcodec-63.dll' = Avcodec -Configuration ($minimal + ' ') })
$withAvutil = New-Fixture 'with-avutil' (Beta-Files @{ 'avutil-61.dll' = New-Pe @('KERNEL32.dll') })
$bothKinds = New-FFmpegRecordOf $armBeta 'both-kinds' -Kinds @('x64', 'arm64')
$recordCases = [ordered]@{
    'record-of-another-avcodec' = @($beta, (New-FFmpegRecordOf $otherDecoder 'another-avcodec'), 'avcodec-63\.dll is not the DLL the FFmpeg part made for x64')
    'record-of-another-kind' = @($beta, (New-FFmpegRecordOf $beta 'arm-only' -Kind 'arm64'), 'the record holds no build for x64')
    'record-without-avutil' = @($withAvutil, (New-FFmpegRecordOf $beta 'avcodec-only'), 'avutil-61\.dll is not a DLL the FFmpeg part made for x64')
    'record-with-avutil' = @($beta, (New-FFmpegRecordOf $withAvutil 'with-avutil'), 'avutil-61\.dll, which the FFmpeg part made for x64, is not in the package')
    'record-of-arm-for-x64' = @($beta, $bothKinds, 'avcodec-63\.dll is not the DLL the FFmpeg part made for x64')
    'record-not-json' = @($beta, "$root/not-json.json", 'there is no record of an FFmpeg part')
}
[IO.File]::WriteAllText("$root/not-json.json", 'not JSON')
foreach ($case in $recordCases.GetEnumerator()) {
    $result = Invoke-Audit $case.Value[0] $case.Key -Beta -FFmpegRecord $case.Value[1]
    Check ($result.Refused -match 'ffmpeg-record' -and (Gate $result.Record 'ffmpeg-record').open -and
        @($result.Record.ffmpegRecordRefusals) -match $case.Value[2]) "$($case.Key) is refused ($($result.Refused); $(@($result.Record.ffmpegRecordRefusals) -join '; '))"
}
$result = Invoke-Audit $armBeta 'record-both-for-arm' -Beta -FFmpegRecord $bothKinds
Check (-not (Gate $result.Record 'ffmpeg-record').open) "an Arm package is held to the record's build for arm64 ($($result.Refused))"

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

# --- Every program and library built with what tools/image-protections.json asks of its kind, read from its own bytes
# (build_common.ps1's Get-ImageProtectionRefusals, which test_image_protections.ps1 tries case by case): else no beta,
# and the gate is never accepted open. What each has is recorded.
$protectedBeta = Invoke-Audit $beta 'beta-protections' -Beta
Check (-not $protectedBeta.Refused -and -not (Gate $protectedBeta.Record 'protections').open) "a beta's images have every protection their rows ask ($($protectedBeta.Refused))"
$protected = @($protectedBeta.Record.protections | Where-Object path -eq 'Qt6Core.dll')
Check ($protected.Count -eq 1 -and (@($protected[0].has) -join ',') -ceq 'aslr,highEntropyVa,dep,cfg,cet,ehContinuation') "the record says what each image has ($($protected | ConvertTo-Json -Compress))"
$found = Expect-NoBeta 'program-without-cfg' @{ 'qkvm-desktop.exe' = Protect-Pe (New-Pe @('Qt6Core.dll', 'avcodec-63.dll', 'KERNEL32.dll') "`0$feed`0$key`0") 'cfg' } 'protections'
Check (@(@($found.Record.protectionRefusals) -match '\Aqkvm-desktop\.exe lacks cfg').Count -eq 1) "the record says what the program lacks ($($found.Record.protectionRefusals))"
Check (-not (Gate $found.Record 'protections').beta) 'the protections gate is never accepted open'
Expect-NoBeta 'qt-without-dep' @{ 'Qt6Core.dll' = Protect-Pe (New-Pe @('KERNEL32.dll')) 'dep' } 'protections' | Out-Null
Expect-NoBeta 'ffmpeg-without-aslr' @{ 'avcodec-63.dll' = Protect-Pe (Avcodec) 'aslr' } 'protections' | Out-Null
Expect-NoBeta 'library-of-no-row' @{ 'plugins2/other.dll' = New-Pe @('KERNEL32.dll') } 'protections' | Out-Null

# --- datachannel.dll is the one a build of the pinned WebRTC sources for the package's kind made, by size and SHA-256 as
# that build's record names it (the webrtc gate, never accepted open; test_webrtc_sources.ps1 tries the record's own
# rules). Audited without -Beta here: the webrtc gate alone is looked at. A package that holds none ships nothing of it.
Check (-not (Gate $protectedBeta.Record 'webrtc').open) 'a package without datachannel.dll leaves the webrtc gate shut'
$channel = New-Pe @('KERNEL32.dll') 'made-up datachannel'
$withChannel = New-Fixture 'with-datachannel' (Beta-Files @{ 'datachannel.dll' = $channel })
function New-WebRtcRecordOf([string] $Name, [string] $Arch, [string] $Library) {
    # (A made-up build's record, of the pinned sources, that made $Library as its datachannel.dll.)
    $install = "$root/$Name-install"
    New-Item -ItemType Directory -Force -Path "$install/bin" | Out-Null
    [IO.File]::WriteAllBytes("$install/bin/datachannel.dll", [Text.Encoding]::Latin1.GetBytes($Library))
    $file = "$root/$Name-webrtc-record.json"
    & {
        . "$PSScriptRoot\build_common.ps1"
        New-WebRtcRecord -Arch $Arch -Trees @{ libdatachannel = $WebRtcPins.libdatachannel.tree; mbedtls = $WebRtcPins.mbedtls.tree } -Install $install `
            -Script "$PSScriptRoot\..\experiments\webrtc\build-cpp-dependencies.ps1" | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $file -Encoding utf8
    }
    return $file
}
function Invoke-AuditOfWebRtc([string] $Name, [string] $Record) {
    $out = "$root/$Name.json"
    & "$PSScriptRoot/audit_release.ps1" -PackageDirectory $withChannel -OutputFile $out -WebRtcRecord $Record | Out-Null
    return Get-Content -LiteralPath $out -Raw | ConvertFrom-Json
}
$said = Invoke-AuditOfWebRtc 'webrtc-own' (New-WebRtcRecordOf 'own' 'x64' $channel)
Check (-not (Gate $said 'webrtc').open -and $said.webrtcRecord.sha256 -match '\A[0-9a-f]{64}\z') "a datachannel.dll its build's record names shuts the webrtc gate ($($said.webrtcRefusals))"
foreach ($case in @(
        @('webrtc-another-library', (New-WebRtcRecordOf 'another' 'x64' 'another library'), 'not this one, by its size and SHA-256'),
        @('webrtc-other-kind', (New-WebRtcRecordOf 'arm' 'arm64' $channel), 'of a build for arm64, not x64'),
        @('webrtc-no-record', "$root/no-such-record.json", 'there is no record of the WebRTC build'))) {
    $said = Invoke-AuditOfWebRtc $case[0] $case[1]
    $gate = Gate $said 'webrtc'
    Check ($gate.open -and -not $gate.beta -and @(@($said.webrtcRefusals) -match $case[2]).Count -eq 1) "$($case[0]) leaves the webrtc gate open, never accepted ($($said.webrtcRefusals))"
}

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
# than the user; it is unsigned, as the beta's accepted gate says; and it is itself the one its build's record names
# (outside review of 0.2.0, 1.9). A real setup, made by tools/build_setup.ps1 from the made-up package with the pinned
# Inno Setup (never run: the audit reads its bytes and resources only), and never a stand-in program of Windows'. Each
# case is that one build, changed alone in its own folder and put back after, so that the compiler's list still names
# that folder's own inputs and only the one thing a case changes differs.
$markerBytes = [byte[]] @(0x73, 0x65, 0x74, 0x75, 0x70, 0x0A)
$innoVersion = [regex]::Match([IO.File]::ReadAllText("$PSScriptRoot\build_common.ps1"), "(?m)^\`$InnoVersion = '([^']+)'").Groups[1].Value
$inno = Join-Path (Split-Path -Parent $PSScriptRoot) ".deps\inno\$innoVersion"
if (-not $innoVersion -or -not (Test-Path -LiteralPath (Join-Path $inno 'ISCC.exe'))) { throw "There is no Inno Setup in ${inno}: run tools\fetch_inno.ps1 first." }
function Build-Setup([string] $Package, [string] $Name, [byte[]] $Check = $null, [string] $ArmPackage = '', [byte[]] $ArmCheck = $null) {
    <# A setup made from $Package into a new folder $Name by tools/build_setup.ps1, carrying $Check as its update check (by
    default a made-up image of Windows' own calls alone, never run, as the real one imports kernel32.dll and user32.dll
    alone). Its folder. #>
    if (-not $Check) { $Check = [Text.Encoding]::Latin1.GetBytes((New-Pe @('KERNEL32.dll', 'USER32.dll'))) }
    $checkFile = "$root/$Name-update-check.exe"
    [IO.File]::WriteAllBytes($checkFile, $Check)
    $out = "$root/$Name"
    $packages = $Package
    $checks = $checkFile
    # (And Windows on Arm's package, with its own update check, where it is named: one setup for both.)
    if ($ArmPackage) {
        if (-not $ArmCheck) { $ArmCheck = [Text.Encoding]::Latin1.GetBytes((New-Pe @('KERNEL32.dll', 'USER32.dll') -Machine 0xAA64)) }
        $armCheckFile = "$root/$Name-update-check-arm64.exe"
        [IO.File]::WriteAllBytes($armCheckFile, $ArmCheck)
        $packages = "$Package,$ArmPackage"
        $checks = "$checkFile,$armCheckFile"
    }
    $said = & (Get-Process -Id $PID).Path -NoProfile -NonInteractive -File "$PSScriptRoot/build_setup.ps1" -PackageDirectory $packages -Inno $inno -OutputDirectory $out -UpdateCheck $checks 2>&1
    if ($LASTEXITCODE -ne 0) { throw "tools/build_setup.ps1 could not make $Name (exit $LASTEXITCODE): $(($said | ForEach-Object { "$_" }) -join ' | ')" }
    return (Resolve-Path -LiteralPath $out).Path
}
$setupBeta = New-Fixture 'setup-beta' (Beta-Files)
$setupMade = Build-Setup $setupBeta 'setup-made'
$setupKept = "$root/setup-made-as-built"
Copy-Item -LiteralPath $setupMade -Destination $setupKept -Recurse
function New-SetupFixture([string] $Package, [string] $Name, [scriptblock] $Change = {}) {
    <# The one build, as it was made, in its own folder, changed by $Change (the folder, and the compiler's list of what it
    put in as rows, written back only where changed). The folder. #>
    Remove-Item -LiteralPath $setupMade -Recurse -Force
    Copy-Item -LiteralPath $setupKept -Destination $setupMade -Recurse
    $list = "$setupMade/setup-files.txt"
    $before = @(Get-Content -LiteralPath $list)
    $rows = [Collections.Generic.List[string]]::new([string[]] $before)
    & $Change ([pscustomobject]@{ Folder = $setupMade; Rows = $rows })
    if (($rows -join "`n") -cne ($before -join "`n")) { Set-Content -LiteralPath $list -Value $rows -Encoding utf8 }
    return $setupMade
}
function Edit-Record($Fixture, [scriptblock] $Change) {
    <# The build's record (setup-record.json) changed by $Change. #>
    $file = "$($Fixture.Folder)/setup-record.json"
    $record = Get-Content -LiteralPath $file -Raw | ConvertFrom-Json
    & $Change $record
    $record | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $file -Encoding utf8
}
function Edit-Row($Fixture, [string] $Like, [scriptblock] $To) {
    $at = -1
    for ($i = 1; $i -lt $Fixture.Rows.Count; ++$i) { if ($Fixture.Rows[$i].Contains($Like)) { $at = $i } }
    if ($at -lt 0) { throw "No row of $Like." }
    $Fixture.Rows[$at] = & $To $Fixture.Rows[$at]
}
$result = Invoke-Audit $setupBeta 'setup-beta' -Beta -Setup (New-SetupFixture $setupBeta 'setup-good')
Check (-not $result.Refused) "a beta with its setup is taken as one ($($result.Refused))"
if (-not $result.Refused) {
    Check ($result.Record.setup.name -eq 'qKVM-Desktop-0.1.0-windows-setup.exe' -and $result.Record.setup.files -eq @(Get-ChildItem -LiteralPath $setupBeta -Recurse -File).Count + 2 -and
        $result.Record.setup.manifestLevel -eq 'asInvoker') "the record says what the setup holds and asks for ($($result.Record.setup | ConvertTo-Json -Compress))"
    Check (@($result.Record.signatures.file) -contains 'qKVM-Desktop-0.1.0-windows-setup.exe') 'the setup is among the programs whose signatures count'
    # (Its update check, which stands at the program's name while an update has the earlier version set aside, is held to
    # the release's rules as the program is, and named by its bytes; review i020-2, L10.)
    $checkMade = "$setupMade/inputs/qkvm-update-check.exe"
    Check (@($result.Record.signatures.file) -contains 'qkvm-update-check.exe') 'the setup''s update check is among the programs whose signatures count'
    $checkSaid = @($result.Record.setup.updateChecks)
    Check ($checkSaid.Count -eq 1 -and $checkSaid[0].architecture -eq 'x64' -and $checkSaid[0].sha256 -eq (Get-FileHash -LiteralPath $checkMade).Hash.ToLowerInvariant() -and $checkSaid[0].bytes -eq (Get-Item -LiteralPath $checkMade).Length -and
        (@($checkSaid[0].imports) -join ',') -eq 'KERNEL32.dll,USER32.dll' -and (@($result.Record.setup.architectures) -join ',') -eq 'x64') "the record names the update check by its kind, its bytes, its SHA-256 and what it imports ($($result.Record.setup | ConvertTo-Json -Compress))"
    Check ($checkSaid.Count -eq 1 -and (@($checkSaid[0].has) -join ',') -ceq 'aslr,highEntropyVa,dep,cfg,cet,ehContinuation' -and -not (Gate $result.Record 'protections').open) "the record says what the update check is built with, and it shuts the protections gate ($($result.Record.setup | ConvertTo-Json -Compress))"
    Check ((Gate $result.Record 'installer').beta -match '^Owner, 2026-11-08: ' -and (Gate $result.Record 'signatures').beta -match 'setup') 'the open installer and signature gates say the owner accepted the unsigned setup'
}
function Expect-SetupRefusal([string] $Name, [scriptblock] $Change, [string] $Expected) {
    $setup = New-SetupFixture $setupBeta $Name $Change
    $result = Invoke-Audit $setupBeta $Name -Beta -Setup $setup
    Check ($result.Refused -match $Expected) "a setup $Name is refused ($($result.Refused))"
}
Expect-SetupRefusal 'without-its-update-check' { param($f) Remove-Item -LiteralPath "$($f.Folder)/inputs/qkvm-update-check.exe" } 'update check for x64 is not a Windows program'
Expect-SetupRefusal 'with-an-update-check-of-text' { param($f) [IO.File]::WriteAllText("$($f.Folder)/inputs/qkvm-update-check.exe", 'not a program') } 'update check for x64 is not a Windows program'
# (The update check held to the program's rules: x64, Windows' own kernel32.dll and user32.dll alone, its symbols by a
# bare file name; review i020-2, L10.)
function Set-UpdateCheck($Fixture, [byte[]] $Bytes) {
    <# The fixture's update check made these bytes, its row in the compiler's list with them. #>
    $file = "$($Fixture.Folder)/inputs/qkvm-update-check.exe"
    [IO.File]::WriteAllBytes($file, $Bytes)
    Edit-Row $Fixture 'qkvm-update-check.exe' { param($row) $row -replace '\t[0-9a-f]{64}\t\d+\t', "`t$((Get-FileHash -LiteralPath $file).Hash.ToLowerInvariant())`t$($Bytes.Length)`t" }
}
function New-PeWithSymbols([string] $Symbols) {
    <# A made-up 64-bit Windows image of one section and no imports, whose debug directory names $Symbols (as
    tools/test_build_release.ps1's), built as our own programs must be (tools/image-protections.json): its relocations
    kept, ASLR of high entropy, DEP, CFG with its load configuration's word of it, and its debug directory's second entry
    saying CET_COMPAT. #>
    $bytes = [byte[]]::new(0x400)
    $put = { param([int] $At, [long] $Value, [int] $Size) [BitConverter]::GetBytes($Value)[0..($Size - 1)].CopyTo($bytes, $At) }
    $bytes[0] = 0x4D; $bytes[1] = 0x5A
    & $put 0x3C 0x40 4
    & $put 0x40 0x00004550 4
    & $put 0x44 0x8664 2
    & $put 0x46 1 2
    & $put 0x54 240 2
    & $put 0x58 0x20B 2
    & $put (0x58 + 70) 0x4160 2
    & $put (0x58 + 108) 16 4
    & $put (0x58 + 112 + 40) 0x11A0 4
    & $put (0x58 + 112 + 44) 8 4
    & $put (0x58 + 112 + 48) 0x1000 4
    & $put (0x58 + 112 + 52) 56 4
    & $put (0x58 + 112 + 80) 0x1100 4
    & $put (0x58 + 112 + 84) 148 4
    $section = 0x58 + 240
    [Text.Encoding]::ASCII.GetBytes('.rdata').CopyTo($bytes, $section)
    & $put ($section + 8) 0x200 4
    & $put ($section + 12) 0x1000 4
    & $put ($section + 16) 0x200 4
    & $put ($section + 20) 0x200 4
    $name = [Text.Encoding]::UTF8.GetBytes($Symbols)
    & $put (0x200 + 12) 2 4
    & $put (0x200 + 16) (24 + $name.Length + 1) 4
    & $put (0x200 + 20) 0x1040 4
    & $put (0x200 + 24) 0x240 4
    & $put (0x21C + 12) 20 4
    & $put (0x21C + 16) 4 4
    & $put (0x21C + 20) 0x1038 4
    & $put (0x21C + 24) 0x238 4
    & $put 0x238 1 4
    & $put 0x240 0x53445352 4
    $name.CopyTo($bytes, 0x240 + 24)
    & $put 0x300 148 4
    & $put (0x300 + 144) 0x400100 4
    return $bytes
}
$armCheck = [Text.Encoding]::Latin1.GetBytes((New-Pe @('KERNEL32.dll', 'USER32.dll')))
$armCheck[0x44] = 0x64; $armCheck[0x45] = 0xAA
Expect-SetupRefusal 'with-an-update-check-that-imports-more' { param($f) Set-UpdateCheck $f ([Text.Encoding]::Latin1.GetBytes((New-Pe @('KERNEL32.dll', 'USER32.dll', 'msvcrt.dll')))) } 'update check for x64 imports more than kernel32\.dll and user32\.dll: msvcrt\.dll'
Expect-SetupRefusal 'with-an-update-check-that-delay-loads-more' { param($f) Set-UpdateCheck $f ([Text.Encoding]::Latin1.GetBytes((New-Pe @('KERNEL32.dll') -Delayed @('Qt6Core.dll')))) } 'update check for x64 imports more than kernel32\.dll and user32\.dll: Qt6Core\.dll'
Expect-SetupRefusal 'with-an-update-check-for-arm64' { param($f) Set-UpdateCheck $f $armCheck } 'update check for x64 is not a program for x64 Windows'
Expect-SetupRefusal 'with-an-update-check-of-32-bits' { param($f) $bytes = [Text.Encoding]::Latin1.GetBytes((New-Pe @('KERNEL32.dll'))); $bytes[0x58] = 0x0B; $bytes[0x59] = 0x01; Set-UpdateCheck $f $bytes } 'what the setup''s update check for x64 links cannot be read'
Expect-SetupRefusal 'with-an-update-check-whose-symbols-name-a-folder' { param($f) Set-UpdateCheck $f (New-PeWithSymbols 'Q:\b\src\build\publish\bin\qkvm-update-check.pdb') } 'update check for x64 names its symbols with a folder'
$bareCheck = Build-Setup $setupBeta 'setup-check-bare-symbols' (New-PeWithSymbols 'qkvm-update-check.pdb')
$result = Invoke-Audit $setupBeta 'setup-check-bare-symbols' -Beta -Setup $bareCheck
Check (-not $result.Refused) "an update check whose symbols are named by a bare file name is taken ($($result.Refused))"
# (And each update check built with what tools/image-protections.json asks of our own programs of its kind, read from its
# bytes, as every image of a package is: one that lacks a protection opens the protections gate, never accepted, and the
# record says what it lacks; review r2 of arm and armp, L1. tools/build_setup.ps1 makes no such setup, so the case is
# made as a setup of an earlier build_setup.ps1, or one made by hand, would carry it: the update check, its row in the
# compiler's list and its build's record changed together.)
function Set-CheckOf([string] $Folder, [string] $Name, [byte[]] $Bytes) {
    $file = "$Folder/inputs/$Name"
    [IO.File]::WriteAllBytes($file, $Bytes)
    $sha256 = (Get-FileHash -LiteralPath $file).Hash.ToLowerInvariant()
    $list = "$Folder/setup-files.txt"
    $rows = @(Get-Content -LiteralPath $list)
    $at = @(for ($i = 1; $i -lt $rows.Count; ++$i) { if ($rows[$i] -match "[\\/]inputs[\\/]$([regex]::Escape($Name))\t") { $i } })
    if ($at.Count -ne 1) { throw "No one row of $Name." }
    $rows[$at[0]] = $rows[$at[0]] -replace '\t[0-9a-f]{64}\t\d+\t', "`t$sha256`t$($Bytes.Length)`t"
    Set-Content -LiteralPath $list -Value $rows -Encoding utf8
    Edit-Record ([pscustomobject]@{ Folder = $Folder }) { param($r)
        foreach ($one in @($r.updateChecks | Where-Object { $_.name -ceq $Name })) { $one.sha256 = $sha256; $one.bytes = $Bytes.Length }
        $r.list = (Get-FileHash -LiteralPath $list).Hash.ToLowerInvariant() }
}
function Expect-UnprotectedCheck([string] $Name, [string] $Setup, [string[]] $Packages, [string] $Expected) {
    $result = Invoke-Audit $setupBeta $Name -Beta -Setup $Setup -SetupPackages $Packages
    Check ($result.Refused -match 'not a beta: .*protections' -and $null -ne $result.Record -and (Gate $result.Record 'protections').open -and
        -not (Gate $result.Record 'protections').beta -and @(@($result.Record.protectionRefusals) -match $Expected).Count -eq 1) "a setup $Name is no beta, and the record says why ($($result.Refused); $(if ($result.Record) { @($result.Record.protectionRefusals) -join '; ' }))"
}
$withoutCet = New-SetupFixture $setupBeta 'check-without-cet' { param($f) Set-CheckOf $f.Folder 'qkvm-update-check.exe' ([Text.Encoding]::Latin1.GetBytes((Protect-Pe (New-Pe @('KERNEL32.dll', 'USER32.dll')) 'cet'))) }
Expect-UnprotectedCheck 'whose-update-check-lacks-cet' $withoutCet @($setupBeta) "\Athe setup's qkvm-update-check\.exe lacks cet, which every image of qKVM Desktop's own has on x64\z"
$withoutCfg = New-SetupFixture $setupBeta 'check-without-cfg' { param($f) Set-CheckOf $f.Folder 'qkvm-update-check.exe' ([Text.Encoding]::Latin1.GetBytes((Protect-Pe (New-Pe @('KERNEL32.dll', 'USER32.dll')) 'cfg'))) }
Expect-UnprotectedCheck 'whose-update-check-lacks-cfg' $withoutCfg @($setupBeta) "\Athe setup's qkvm-update-check\.exe lacks cfg, which every image of qKVM Desktop's own has on x64\z"
Expect-SetupRefusal 'with-a-file-more' { param($f) $f.Rows.Add("99`t$setupBeta\stray.dll`t2026-11-08 00:00:00.000`t`t$('0' * 64)`t1`t1`t1`t0`t0`t1`tno`t") } 'holds a file the package does not have'
Expect-SetupRefusal 'with-a-file-less' { param($f) $f.Rows.RemoveAt(@(1..($f.Rows.Count - 1) | Where-Object { $f.Rows[$_].Contains('Qt6Core.dll') })[0]) } 'lacks Qt6Core\.dll'
Expect-SetupRefusal 'with-other-bytes' { param($f) Edit-Row $f 'Qt6Core.dll' { param($row) $row -replace '\t[0-9a-f]{64}\t', "`t$('0' * 64)`t" } } 'other bytes .*Qt6Core\.dll'
Expect-SetupRefusal 'with-a-file-twice' { param($f) $f.Rows.Add(($f.Rows | Where-Object { $_.Contains('Qt6Core.dll') } | Select-Object -First 1)) } 'Qt6Core\.dll twice'
Expect-SetupRefusal 'with-another-marker' { param($f) [IO.File]::WriteAllText("$($f.Folder)/inputs/qkvm-desktop.installed", 'setup'); Edit-Row $f 'qkvm-desktop.installed' { param($row) $row -replace '\t[0-9a-f]{64}\t6\t', "`t$((Get-FileHash -LiteralPath "$($f.Folder)/inputs/qkvm-desktop.installed").Hash.ToLowerInvariant())`t5`t" } } 'marker'
Expect-SetupRefusal 'from-another-package' { param($f) for ($i = 1; $i -lt $f.Rows.Count; ++$i) { $f.Rows[$i] = $f.Rows[$i].Replace("$setupBeta\", "$setupBeta-other\") } } 'holds a file the package does not have'
Expect-SetupRefusal 'encrypted' { param($f) Edit-Row $f 'Qt6Core.dll' { param($row) $row -replace '\tno\t$', "`tyes`t" } } 'other bytes .*Qt6Core\.dll'
Expect-SetupRefusal 'not-there' { param($f) Remove-Item -LiteralPath "$($f.Folder)/qKVM-Desktop-0.1.0-windows-setup.exe" } 'has no qKVM-Desktop-0\.1\.0-windows-setup\.exe'
Expect-SetupRefusal 'for-an-administrator' { param($f)
    $file = "$($f.Folder)/qKVM-Desktop-0.1.0-windows-setup.exe"
    $text = [Text.Encoding]::Latin1.GetString([IO.File]::ReadAllBytes($file))
    if (-not $text.Contains('level="asInvoker"')) { throw 'The fixture program has no level to change.' }
    [IO.File]::WriteAllBytes($file, [Text.Encoding]::Latin1.GetBytes($text.Replace('level="asInvoker"', 'level="asAdminxx"'))) } 'asks to run as asAdminxx'
Check ((Invoke-Audit $setupBeta 'setup-msix' -MsixPayload -Setup (New-SetupFixture $setupBeta 'setup-msix-folder')).Refused -match 'MSIX') 'a setup is not audited beside an MSIX'
# (Outside review of 0.2.0, 1.9: the setup that ships is itself the one its build's record names, of this package, its
# update check, the compiler's list and the pinned Inno Setup, and its own version resource says the package's version.
# A setup of the same name and version from another build (another package's), beside this build's other files, is
# refused: before, the audit read only its manifest level from it.)
$otherPackage = New-Fixture 'setup-beta-other' (Beta-Files @{ 'licenses/another-build.txt' = 'what makes this build another' })
$otherSetup = Build-Setup $otherPackage 'setup-other-build'
Expect-SetupRefusal 'of-another-build' { param($f) Copy-Item -LiteralPath "$otherSetup/qKVM-Desktop-0.1.0-windows-setup.exe" -Destination "$($f.Folder)/qKVM-Desktop-0.1.0-windows-setup.exe" -Force } 'not the one its build''s record names'
Expect-SetupRefusal 'without-its-record' { param($f) Remove-Item -LiteralPath "$($f.Folder)/setup-record.json" } 'has no setup-record\.json'
Expect-SetupRefusal 'whose-record-names-another-package' { param($f) Edit-Record $f { param($r) $r.packages[0].manifestSha256 = '0' * 64 } } 'record names other packages'
Expect-SetupRefusal 'whose-record-names-another-version' { param($f) Edit-Record $f { param($r) $r.version = '0.1.1' } } 'record names other packages'
Expect-SetupRefusal 'whose-record-names-another-kind' { param($f) Edit-Record $f { param($r) $r.architectures = @('x64', 'arm64') } } 'record names other packages'
Expect-SetupRefusal 'whose-record-names-another-update-check' { param($f) Edit-Record $f { param($r) $r.updateChecks[0].sha256 = '0' * 64 } } 'record names other update checks'
Expect-SetupRefusal 'whose-list-is-not-the-one-its-record-names' { param($f) $last = $f.Rows[$f.Rows.Count - 1]; $f.Rows.RemoveAt($f.Rows.Count - 1); $f.Rows.Insert(1, $last) } 'list of what the setup holds is not the one its record names'
Expect-SetupRefusal 'made-with-another-inno-setup' { param($f) Edit-Record $f { param($r) $r.inno.version = '6.0.0' } } 'not the pinned'
$laterPackage = New-Fixture 'setup-beta-later' (Beta-Files)
$laterManifest = Get-Content -LiteralPath "$laterPackage/package-manifest.json" -Raw | ConvertFrom-Json
$laterManifest.version = '0.1.1'
$laterManifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath "$laterPackage/package-manifest.json" -Encoding utf8
$laterExe = "$(Build-Setup $laterPackage 'setup-later')/qKVM-Desktop-0.1.1-windows-setup.exe"
Expect-SetupRefusal 'that says another version itself' { param($f)
    Copy-Item -LiteralPath $laterExe -Destination "$($f.Folder)/qKVM-Desktop-0.1.0-windows-setup.exe" -Force
    Edit-Record $f { param($r) $r.setup.sha256 = (Get-FileHash -LiteralPath $laterExe).Hash.ToLowerInvariant(); $r.setup.bytes = (Get-Item -LiteralPath $laterExe).Length } } 'says it is version 0\.1\.1'

# --- One setup for both kinds of Windows (0.2.0): audited beside each package it carries, both named; each update check
# of its own kind; never beside one package alone.
$armSetupBeta = New-Fixture 'setup-beta-arm' (Beta-Files -Machine 0xAA64) 'arm64'
$bothSetup = Build-Setup $setupBeta 'setup-both' -ArmPackage $armSetupBeta
$both = @($setupBeta, $armSetupBeta)
$result = Invoke-Audit $setupBeta 'setup-both-x64' -Beta -Setup $bothSetup -SetupPackages $both
Check (-not $result.Refused) "x64's package is taken beside one setup of both kinds ($($result.Refused))"
if (-not $result.Refused) {
    Check ((@($result.Record.setup.architectures) -join ',') -eq 'x64,arm64' -and @($result.Record.setup.updateChecks).Count -eq 2 -and
        @($result.Record.signatures.file) -contains 'qkvm-update-check-arm64.exe') "the record says the setup carries both kinds, with an update check of each ($($result.Record.setup | ConvertTo-Json -Compress))"
}
$result = Invoke-Audit $armSetupBeta 'setup-both-arm' -Beta -Setup $bothSetup -SetupPackages $both
Check (-not $result.Refused) "Windows on Arm's package is taken beside the same setup ($($result.Refused))"
Check ((Invoke-Audit $setupBeta 'setup-both-alone' -Beta -Setup $bothSetup).Refused -match 'holds a file the package does not have') 'a setup of both kinds beside x64''s package alone is refused'
Check ((Invoke-Audit $armSetupBeta 'setup-both-arm-alone' -Beta -Setup $bothSetup -SetupPackages @($armSetupBeta)).Refused -match 'carries x64''s package always') 'a setup beside Windows on Arm''s package alone is refused'
Check ((Invoke-Audit $setupBeta 'setup-both-x64-twice' -Beta -Setup $bothSetup -SetupPackages @($setupBeta, $setupBeta)).Refused -match 'one of each kind') 'a setup beside two packages of one kind is refused'
$armWrongCheck = Build-Setup $setupBeta 'setup-both-wrong-check' -ArmPackage $armSetupBeta -ArmCheck ([Text.Encoding]::Latin1.GetBytes((New-Pe @('KERNEL32.dll', 'USER32.dll') -Machine 0xAA64 -Delayed @('msvcrt.dll'))))
Check ((Invoke-Audit $setupBeta 'setup-both-wrong-check' -Beta -Setup $armWrongCheck -SetupPackages $both).Refused -match 'update check for arm64 imports more') 'Windows on Arm''s update check is held to the program''s rules too'
# (And to the row of our own programs for ARM64, where its share of signed returns cannot tell whether its own code was
# built with /guard:signret, as most of its functions are the static runtime's: CFG, which qkvm_build gives with that
# switch, is asked too. In the audit of either package beside the setup; review r2 of arm and armp, L1.)
$bothChecked = Build-Setup $setupBeta 'setup-both-check-protections' -ArmPackage $armSetupBeta
$bothCheckedKept = "$root/setup-both-check-protections-as-built"
Copy-Item -LiteralPath $bothChecked -Destination $bothCheckedKept -Recurse
foreach ($case in @(@('cfg', 'lacks cfg,'), @('pac', 'lacks pac,'))) {
    Remove-Item -LiteralPath $bothChecked -Recurse -Force
    Copy-Item -LiteralPath $bothCheckedKept -Destination $bothChecked -Recurse
    Set-CheckOf $bothChecked 'qkvm-update-check-arm64.exe' ([Text.Encoding]::Latin1.GetBytes((Protect-Pe (New-Pe @('KERNEL32.dll', 'USER32.dll') -Machine 0xAA64) $case[0])))
    Expect-UnprotectedCheck "of-both-whose-arm64-update-check-lacks-$($case[0])" $bothChecked $both "\Athe setup's qkvm-update-check-arm64\.exe $($case[1]) which every image of qKVM Desktop's own has on arm64\z"
    $result = Invoke-Audit $armSetupBeta "setup-both-arm-check-without-$($case[0])" -Beta -Setup $bothChecked -SetupPackages $both
    Check ($result.Refused -match 'not a beta: .*protections') "the ARM64 package's audit beside that setup refuses it too ($($result.Refused))"
}
[IO.File]::AppendAllText("$armSetupBeta/Qt6Core.dll", 'tampered')
Check ((Invoke-Audit $setupBeta 'setup-both-tampered' -Beta -Setup $bothSetup -SetupPackages $both).Refused -match 'arm64 package the setup carries differs from its inventory: Qt6Core\.dll') 'a setup beside an Arm package changed since is refused'

# --- Symbols are never in a package, recorded or not.
$symbols = New-Fixture 'symbols' (Beta-Files @{ 'qkvm-desktop.pdb' = 'symbols' })
Check ((Invoke-Audit $symbols 'symbols').Refused -match 'holds symbols') 'a package with symbols is refused'
Check ((Invoke-Audit $symbols 'symbols-beta' -Beta).Refused -match 'holds symbols') 'a beta with symbols is refused'

# (A throw, not an exit: tools/check.ps1 calls this in its own PowerShell and counts what is thrown.)
if ($failures) { throw "$failures release audit check(s) failed." }
Remove-Item -LiteralPath $root -Recurse -Force
Write-Output 'Release audit checks passed: incomplete, unsigned, unrecorded, tampered, escaping and symbol-carrying packages are not released, and a beta is one only with every open gate accepted.'
