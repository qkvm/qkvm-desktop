#Requires -Version 7.2
<#
.SYNOPSIS
Audits a built application folder and records remaining release blockers.
.DESCRIPTION
No installation, publishing, private profile reads or network requests. A successful
audit command means the inventory was examined, not that release was approved.
-RequireRelease returns failure while any release gate remains open.
-Beta returns failure while a gate is open that the owner has not accepted for a beta
(every open gate is recorded with the owner's reason for accepting it); the
notices and FFmpeg gates are never accepted open.
-SetupDirectory names what tools/build_setup.ps1 made from this package: the setup
is refused unless the compiler's own list of what it put in (setup-files.txt) is
this package's manifest, file for file and by SHA-256, with the manifest itself
and the marker ("setup" and a line feed) and nothing else, and unless the
setup's own manifest asks for nothing more than whoever runs it (asInvoker). Its
signature counts with the programs'; unsigned, it is accepted for a beta as the
owner said (2026-11-08). What is compressed inside it is not read here.
#>
param(
    [Parameter(Mandatory)][string] $PackageDirectory,
    [Parameter(Mandatory)][string] $OutputFile,
    [switch] $MsixPayload,
    [switch] $RequireRelease,
    [switch] $Beta,
    [string] $SetupDirectory = ''
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\build_common.ps1"
if ($SetupDirectory -and $MsixPayload) { throw 'A setup is audited beside the portable package it was made from, never beside an MSIX.' }
$root = (Resolve-Path -LiteralPath $PackageDirectory).Path
if ((Get-Item -LiteralPath $root).Attributes -band [IO.FileAttributes]::ReparsePoint) {
    throw 'The package root must not be redirected.'
}
$manifestFile = Join-Path $root 'package-manifest.json'
$manifestInfo = Get-Item -LiteralPath $manifestFile
if ($manifestInfo.Length -gt 1048576 -or $manifestInfo.Attributes -band [IO.FileAttributes]::ReparsePoint) {
    throw 'The package inventory is redirected or too large.'
}
$manifest = Get-Content -LiteralPath $manifestFile -Raw | ConvertFrom-Json
if ($manifest.product -ne 'qKVM' -or $manifest.architecture -ne 'x64' -or $manifest.files.Count -eq 0) {
    throw 'The package inventory is invalid.'
}
$seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$binaries = @()
foreach ($entry in $manifest.files) {
    $parts = $entry.path.Split('/')
    if ([IO.Path]::IsPathRooted($entry.path) -or $entry.path.Contains('\') -or
        $entry.path.Contains(':') -or @($parts | Where-Object { $_ -in @('', '.', '..') }).Count -or
        -not $seen.Add($entry.path)) { throw 'The package inventory contains an unsafe or duplicate path.' }
    $file = $root
    foreach ($part in $parts) {
        $file = Join-Path $file $part
        $item = Get-Item -LiteralPath $file
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'The package contains a redirected path.' }
    }
    if ($item.PSIsContainer -or $item.Length -ne $entry.bytes -or
        (Get-FileHash -LiteralPath $file).Hash.ToLowerInvariant() -cne $entry.sha256) {
        throw "Package bytes differ from the inventory: $($entry.path)"
    }
    if ([IO.Path]::GetExtension($file) -in @('.exe', '.dll')) {
        $binaries += [ordered]@{ path = $entry.path; bytes = $entry.bytes; sha256 = $entry.sha256 }
    }
    # Symbols name the folders a program was built in: they are kept apart, never handed out.
    if ([IO.Path]::GetExtension($file) -ieq '.pdb') { throw "The package holds symbols: $($entry.path)" }
}
$all = @(Get-ChildItem -LiteralPath $root -Recurse -Force)
if (@($all | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) {
    throw 'The package contains a redirected entry.'
}
$actual = @($all | Where-Object { -not $_.PSIsContainer -and $_.FullName -ne $manifestFile })
$windowsMetadata = @('AppxManifest.xml','AppxBlockMap.xml','AppxSignature.p7x','[Content_Types].xml','AppxMetadata/CodeIntegrity.cat')
foreach ($file in $actual) {
    $relative = [IO.Path]::GetRelativePath($root, $file.FullName).Replace('\', '/')
    if ($MsixPayload -and $relative -cin $windowsMetadata) { continue }
    if (-not $seen.Contains($relative)) { throw "Unrecorded package file: $relative" }
}
$required = @('THIRD-PARTY.md','licenses/AGPL-3.0.txt','licenses/LGPL-3.0.txt','licenses/GPL-3.0.txt',
    'licenses/Feather-MIT.txt','licenses/Unicode-CLDR.txt','licenses/FFmpeg-LGPL-3.0.txt','licenses/libdatachannel-MPL-2.0.txt',
    'licenses/libjuice-MPL-2.0.txt','licenses/libsrtp-BSD.txt','licenses/usrsctp-BSD.txt','licenses/plog-MIT.txt',
    'licenses/Mbed-TLS-license.txt','licenses/Everest-notice.md','licenses/p256-m-integration-notice.md',
    'licenses/p256-m-notice.md','licenses/qt-sbom/qtbase-6.12.0.spdx.json',
    'licenses/qt-sbom/qtdeclarative-6.12.0.spdx.json','licenses/qt-sbom/qtsvg-6.12.0.spdx.json',
    'licenses/Qt-bundled-parts.md','licenses/zlib.txt','licenses/Inno-Setup-license.txt',
    'licenses/RemObjects-Pascal-Script-license.md')
# Every library that ships is named, by its file name, in THIRD-PARTY.md (with its licence and source there).
$inventory = if ($seen.Contains('THIRD-PARTY.md')) { Get-Content -LiteralPath (Join-Path $root 'THIRD-PARTY.md') -Raw } else { '' }
$unlisted = @($binaries | Where-Object { $_.path -like '*.dll' } | ForEach-Object { Split-Path -Leaf $_.path } |
    Where-Object { $inventory.IndexOf($_, [StringComparison]::OrdinalIgnoreCase) -lt 0 } | Sort-Object -Unique)
# And what each library and program links, read from its own bytes rather than from its name. A runtime linked into
# it (a compiler's start-up and support code: inside the file, with no file name of its own) has its notice in the
# package and its name in THIRD-PARTY.md; it is known by what it leaves in every file it is linked into. Every
# library a file imports, at its start or when first called (delay-loaded), ships here or is Windows' own.
$runtimes = @(
    # (mingw-w64-crt's pseudo-relocation failure text, and the mark GCC leaves of itself in each object it compiles.)
    [ordered]@{ name = 'MinGW-w64 runtime'; notice = 'licenses/mingw-w64-runtime.txt'
        marks = @('Mingw-w64 runtime failure:', 'GCC: (GNU) ') }
)
# Windows' own libraries that what ships is seen to import (Qt, the Microsoft runtime, the project's FFmpeg and
# libdatachannel, qKVM Setup), and Windows' API sets. Any other is a library the package needs and does not carry,
# whose licence nobody has read: added here only after a look at what it is.
$windowsLibraries = @('advapi32', 'authz', 'bcrypt', 'bcryptprimitives', 'comdlg32', 'crypt32', 'd3d9', 'd3d11',
    'd3d12', 'dnsapi', 'dwmapi', 'dwrite', 'dxgi', 'gdi32', 'icuuc', 'imm32', 'iphlpapi', 'kernel32', 'mpr', 'mscms',
    'msvcrt', 'ncrypt', 'netapi32', 'ntdll', 'ole32', 'oleaut32', 'secur32', 'setupapi', 'shell32', 'shlwapi',
    'uiautomationcore', 'user32', 'userenv', 'uxtheme', 'version', 'winhttp', 'winmm', 'wintrust', 'ws2_32',
    'wtsapi32') | ForEach-Object { "$_.dll" }
$shipped = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($binary in $binaries) { $null = $shipped.Add((Split-Path -Leaf $binary.path)) }
$links = @()
$unlistedLinks = @()
$linkedRuntimes = [ordered]@{}
foreach ($binary in $binaries) {
    $bytes = [IO.File]::ReadAllBytes((Join-Path $root $binary.path))
    try {
        $imports = @(Get-PeImports -Bytes $bytes)
    } catch {
        $unlistedLinks += "$($binary.path): what it links cannot be read ($($_.Exception.Message))"
        continue
    }
    $text = [Text.Encoding]::Latin1.GetString($bytes)
    $inside = @($runtimes | Where-Object { $runtime = $_; @($runtime.marks | Where-Object { $text.Contains($_) }).Count } |
        ForEach-Object { $_.name })
    foreach ($name in $inside) { $linkedRuntimes[$name] = @($linkedRuntimes[$name] | Where-Object { $_ }) + $binary.path }
    foreach ($import in $imports) {
        if (-not $shipped.Contains($import) -and $import -notin $windowsLibraries -and $import -notmatch '\A(api|ext)-ms-win-[a-z0-9-]+\.dll\z') {
            $unlistedLinks += "$($binary.path) imports $import, which neither ships nor is Windows' own"
        }
    }
    $links += [ordered]@{ path = $binary.path; imports = $imports; runtimes = $inside }
}
foreach ($runtime in $runtimes | Where-Object { $linkedRuntimes.Contains($_.name) }) {
    $required += $runtime.notice
    if ($inventory.IndexOf($runtime.name, [StringComparison]::OrdinalIgnoreCase) -lt 0) {
        $unlistedLinks += "$($runtime.name), linked into $($linkedRuntimes[$runtime.name] -join ', '), is not named in THIRD-PARTY.md"
    }
}
$missing = @($required | Where-Object { -not $seen.Contains($_) })
# The programs that ship: qkvm-desktop.exe in the portable ZIP, and qKVM Setup beside it in an MSIX. Any other is
# a test, bench or stand-in program, and is never handed out.
$programs = @('qkvm-desktop.exe') + @(if ($MsixPayload) { 'qkvm-setup.exe' })
foreach ($binary in $binaries | Where-Object { $_.path -like '*.exe' }) {
    if ($programs -cnotcontains $binary.path) { throw "The package holds a program that is not handed out: $($binary.path)" }
}
$signatureChecks = @($programs | ForEach-Object {
    if (-not $seen.Contains($_)) { throw "Missing release executable: $_" }
    $signature = Get-AuthenticodeSignature -LiteralPath (Join-Path $root $_)
    [ordered]@{ file = $_; status = [string]$signature.Status }
})
# The setup made from this package: exactly its files, the manifest and the marker; for the user alone.
$setup = $null
if ($SetupDirectory) {
    $setupRoot = (Resolve-Path -LiteralPath $SetupDirectory).Path.TrimEnd('\')
    $setupName = Get-SetupName ([string] $manifest.version)
    $setupFile = Join-Path $setupRoot $setupName
    if (-not (Test-Path -LiteralPath $setupFile -PathType Leaf)) { throw "The setup folder has no $setupName." }
    if ((Get-Item -LiteralPath $setupFile).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'The setup is redirected.' }
    $markerBytes = [byte[]] @(0x73, 0x65, 0x74, 0x75, 0x70, 0x0A)
    $markerFile = Join-Path $setupRoot 'inputs\qkvm-desktop.installed'
    if (-not (Test-Path -LiteralPath $markerFile -PathType Leaf) -or
        (Get-FileHash -LiteralPath $markerFile).Hash.ToLowerInvariant() -cne [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($markerBytes)).ToLowerInvariant()) {
        throw 'The setup''s marker is not "setup" and a line feed.'
    }
    $wanted = @{}
    foreach ($entry in $manifest.files) {
        $wanted[(Join-Path $root ($entry.path -replace '/', '\')).ToLowerInvariant()] = @{ Path = $entry.path; Bytes = [long] $entry.bytes; Sha256 = [string] $entry.sha256 }
    }
    $wanted[$manifestFile.ToLowerInvariant()] = @{ Path = 'package-manifest.json'; Bytes = $manifestInfo.Length; Sha256 = (Get-FileHash -LiteralPath $manifestFile).Hash.ToLowerInvariant() }
    $wanted[$markerFile.ToLowerInvariant()] = @{ Path = 'qkvm-desktop.installed'; Bytes = $markerBytes.Length; Sha256 = (Get-FileHash -LiteralPath $markerFile).Hash.ToLowerInvariant() }
    $inSetup = @{}
    foreach ($row in @(Get-Content -LiteralPath (Join-Path $setupRoot 'setup-files.txt') | ConvertFrom-Csv -Delimiter "`t")) {
        $source = ([string] $row.SourceFilename).ToLowerInvariant()
        if (-not $wanted.ContainsKey($source)) { throw "The setup holds a file the package does not have: $($row.SourceFilename)" }
        if ($inSetup.ContainsKey($source)) { throw "The setup holds $($wanted[$source].Path) twice." }
        $inSetup[$source] = $true
        if ([long] $row.OriginalSize -ne $wanted[$source].Bytes -or [string] $row.SHA256Sum -cne $wanted[$source].Sha256 -or [string] $row.Encrypted -cne 'no') {
            throw "The setup holds other bytes than the package's manifest names (or holds them encrypted) for $($wanted[$source].Path)."
        }
    }
    foreach ($source in $wanted.Keys | Where-Object { -not $inSetup.ContainsKey($_) }) { throw "The setup lacks $($wanted[$source].Path)." }
    $level = [regex]::Match((Get-PeManifest $setupFile), 'requestedExecutionLevel\s+level="([^"]+)"').Groups[1].Value
    if ($level -cne 'asInvoker') { throw "The setup's own manifest asks to run as $level, not as whoever runs it." }
    $setup = [ordered]@{ name = $setupName; bytes = (Get-Item -LiteralPath $setupFile).Length
        sha256 = (Get-FileHash -LiteralPath $setupFile).Hash.ToLowerInvariant(); files = $inSetup.Count; manifestLevel = $level }
    $signatureChecks += [ordered]@{ file = $setupName; status = [string] (Get-AuthenticodeSignature -LiteralPath $setupFile).Status }
}
function Read-Latin1([string] $Relative) {
    return [Text.Encoding]::Latin1.GetString([IO.File]::ReadAllBytes((Join-Path $root $Relative)))
}
# FFmpeg as it was configured, in its own words (the configuration line it carries): the project's minimal build
# disables everything and finds nothing by itself, enables nothing but what it names and no outside library under
# any name (build_common.ps1's Get-FFmpegRefusals, as build_ffmpeg.ps1 holds the build to it).
$decoder = @($binaries | Where-Object { $_.path -match '^avcodec-\d+\.dll$' })
$ffmpegConfiguration = ''
if ($decoder.Count -eq 1) {
    $found = [regex]::Match((Read-Latin1 $decoder[0].path), '(?<=\x00)--[a-z][^\x00]{0,8000}?--enable-version3[^\x00]{0,8000}')
    if ($found.Success) { $ffmpegConfiguration = $found.Value }
}
$ffmpegRefusals = @(Get-FFmpegRefusals $ffmpegConfiguration)
$ffmpegMinimal = $ffmpegRefusals.Count -eq 0
# The update channel the program carries: the public feed, and a 128-digit key beside it.
$program = Read-Latin1 'qkvm-desktop.exe'
$feed = 'https://download.qkvm.com/desktop/stable.json'
$channelBuiltIn = $program.Contains("$feed`0") -and $program -match '\x00[0-9a-fA-F]{128}\x00'
# Every gate, and whether it is open. An open gate is accepted for a beta only with the owner's reason, written
# here; the notices and FFmpeg are never accepted open. A caller cannot close a gate by passing a switch.
$gates = @(
    [ordered]@{ id = 'notices'; open = ($missing.Count -gt 0 -or $unlisted.Count -gt 0 -or $unlistedLinks.Count -gt 0)
        text = 'Every notice file is in the package; every library that ships, and every runtime linked into one, is named in THIRD-PARTY.md; and every library one imports ships or is Windows'' own.'
        beta = '' }
    [ordered]@{ id = 'ffmpeg'; open = -not $ffmpegMinimal
        text = 'FFmpeg is the project''s own minimal build (--disable-everything, --disable-autodetect, no outside library, LGPL).'
        beta = '' }
    [ordered]@{ id = 'sources'; open = $true
        text = 'Exact corresponding source distributions, build instructions and public source availability are not verified by this audit.'
        beta = 'Owner, 2026-11-06 (D9, A10, A11): the source archive of the public commit and the third-party source bundle are release files (tools/release_files.ps1), and tools/verify_release.ps1 checks them as published.' }
    [ordered]@{ id = 'signatures'; open = @($signatureChecks | Where-Object status -ne 'Valid').Count -gt 0
        text = 'The programs are signed with a trusted code-signing certificate.'
        beta = 'Owner, 2026-11-06 (D1, D8): beta 1 has no certificate; the download page says what SmartScreen shows and how to check the SHA-256.' +
            $(if ($setup) { ' The setup is unsigned as well until a certificate exists, and Windows warns before it runs (owner, 2026-11-08).' } else { '' }) }
    [ordered]@{ id = 'installer'; open = $true
        text = 'Signed install, update, rollback, interrupted recovery and uninstall/profile-retention acceptance on clean Windows 10 and 11.'
        beta = $(if ($setup) {
            'Owner, 2026-11-08: the per-user setup made with Inno Setup ships unsigned beside the portable ZIP; a first install, an upgrade, the update mode, a failed install, an uninstall that keeps the data and a reinstall are tried by tools/test_setup_vm.ps1 on clean Windows 10 and 11 before it is published.'
        } else {
            'Owner, 2026-11-06 (D1): beta 1 is a portable ZIP with no installer; it is tried on clean Windows 10 and 11 before it is published (B7).'
        }) }
    [ordered]@{ id = 'update-channel'; open = $true
        text = 'The production HTTPS update channel and release verification key are configured and qualified end to end.'
        beta = $(if ($channelBuiltIn) { 'Owner, 2026-11-06 (D6, D7): the program carries the public feed and a key; the owner signs offline, and the published feed is checked end to end on a clean machine (B12).' } else { '' }) }
)
$blockers = @($gates | Where-Object open | ForEach-Object { $_.text })
$refusedForBeta = @($gates | Where-Object { $_.open -and -not $_.beta } | ForEach-Object { $_.id })
$result = [ordered]@{
    schema = 2
    checkedAt = [DateTime]::UtcNow.ToString('o')
    version = $manifest.version
    profile = $(if ($Beta) { 'beta' } else { 'development' })
    packageManifestSha256 = (Get-FileHash -LiteralPath $manifestFile).Hash.ToLowerInvariant()
    integrityVerified = $true
    inventoriedFiles = $seen.Count
    binaries = $binaries
    requiredNoticesPresent = $missing.Count -eq 0
    missingNotices = $missing
    unlistedLibraries = $unlisted
    links = $links
    unlistedLinks = $unlistedLinks
    ffmpegConfiguration = $ffmpegConfiguration
    ffmpegRefusals = $ffmpegRefusals
    updateChannelBuiltIn = $channelBuiltIn
    signatures = $signatureChecks
    setup = $setup
    productionReady = $false
    gates = $gates
    betaAccepted = $refusedForBeta.Count -eq 0
    blockers = $blockers
}
$output = [IO.Path]::GetFullPath($OutputFile)
if ($output.StartsWith($root.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Keep the audit record outside the package whose hashes it verifies.'
}
[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($output)) | Out-Null
$result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $output -Encoding utf8
Write-Output "Release inventory verified: $($seen.Count) files; $($blockers.Count) release gates remain open. Record: $output"
if ($Beta) {
    foreach ($gate in $gates | Where-Object open) { Write-Output "  open: $($gate.id): $(if ($gate.beta) { "accepted for the beta. $($gate.beta)" } else { 'NOT ACCEPTED' })" }
    if ($refusedForBeta.Count) { throw "This package is not a beta: $($refusedForBeta -join ', ') open without the owner's acceptance." }
}
if ($RequireRelease) { throw 'This development package has not passed the production release gates. See the audit record.' }
