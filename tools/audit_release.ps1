#Requires -Version 7.2
<#
.SYNOPSIS
Audits a built application folder and records remaining release blockers.
.DESCRIPTION
No installation, publishing, private profile reads or network requests. A successful
audit command means the inventory was examined, not that release was approved.
A package is of one kind of Windows, x64 or arm64, as its manifest says (tools/package.ps1 -Arch): every program and
library in it that is a Windows image is for that kind, read from its own PE header, or the package is refused.
-RequireRelease returns failure while any release gate remains open.
-Beta returns failure while a gate is open that the owner has not accepted for a beta
(every open gate is recorded with the owner's reason for accepting it); the
notices and FFmpeg gates are never accepted open.
-FFmpegRecord names the record of the release's FFmpeg part (tools/build_release.ps1 -Part ffmpeg): every FFmpeg DLL
in the package must be one its build for the package's kind made, by name, size and SHA-256, and every DLL of that
build must be in the package. Without it, or where anything differs, the ffmpeg-record gate is open, and it is never
accepted open: a release's audit is always given it.
-SetupDirectory names what tools/build_setup.ps1 made from this package and, where it carries it, the package of the
other kind (-SetupPackageDirectory names every package the setup carries, this one among them: x64's and Windows on
Arm's; this one alone where it is not given): the setup is refused unless the compiler's own list of what it put in
(setup-files.txt) is those packages' manifests, file for file and by SHA-256, with the manifests themselves and the
marker ("setup" and a line feed) and nothing else, and unless the setup's own manifest asks for nothing more than
whoever runs it (asInvoker). Its signature counts with the programs'; unsigned, it is accepted for a beta as the
owner said (2026-11-08). What is compressed inside it is not read here. Each update check it carries
(inputs\qkvm-update-check.exe, and inputs\qkvm-update-check-arm64.exe with Windows on Arm's package; each stands at the
program's name while an update has the earlier version set aside on a PC of its kind) is held to the program's rules:
a program for its kind of Windows, importing nothing but kernel32.dll and user32.dll, its symbols by a bare file name,
built with what tools/image-protections.json asks of our own programs of its kind (else the protections gate is open,
as for an image of the package), its signature counted, its bytes, SHA-256 and protections recorded.
And the setup itself, by its own bytes: the one its build's record (setup-record.json)
names, of this package's manifest, its update check, the compiler's list and the
pinned Inno Setup, whose own version resource says the package's version; a setup of
the same name from another build is refused.
#>
param(
    [Parameter(Mandatory)][string] $PackageDirectory,
    [Parameter(Mandatory)][string] $OutputFile,
    # The record of the WebRTC build the package's datachannel.dll is to be from (experiments/webrtc/build-cpp-dependencies.ps1
    # writes it beside its install folder); by default the one of this tree's own build for the package's kind.
    [string] $WebRtcRecord = '',
    [switch] $MsixPayload,
    [switch] $RequireRelease,
    [switch] $Beta,
    [string] $SetupDirectory = '',
    [string[]] $SetupPackageDirectory = @(),
    # The FFmpeg part's record (build_release.ps1 -Part ffmpeg): the package's FFmpeg DLLs are checked against it.
    [string] $FFmpegRecord = ''
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"
. "$PSScriptRoot\build_common.ps1"
. "$PSScriptRoot\release_common.ps1"
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
if ($manifest.product -ne 'qKVM' -or @('x64', 'arm64') -cnotcontains $manifest.architecture -or $manifest.files.Count -eq 0) {
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
    # (A Windows image of another kind than the package's is refused outright: an x64 library in a package for Arm would
    # be run under emulation, or not at all. One that is no Windows image is for the links below to name.)
    $kind = Get-PeArchitecture $bytes
    if ($kind -cne 'no Windows program' -and $kind -cne $manifest.architecture) {
        throw "The package for $($manifest.architecture) Windows holds a program for ${kind}: $($binary.path)"
    }
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
# Every program and library built with what tools/image-protections.json asks of the package's kind (ASLR, high
# entropy, DEP, CFG, and the guard of returns of its kind where its row asks it), each read from its own bytes, and what
# each has recorded (2026-11-21).
$protectionRefusals = @(Get-ImageProtectionRefusals $root ([string] $manifest.architecture))
$protections = @($binaries | ForEach-Object {
    $read = $null
    try { $read = Get-PeProtections ([IO.File]::ReadAllBytes((Join-Path $root $_.path))) } catch { }
    if ($read) { [ordered]@{ path = $_.path; has = @(Get-PeProtectionNames $read); signed = $read.Signed; functions = $read.Functions } }
})
# The package's datachannel.dll is the very one a build of the pinned WebRTC sources for its kind made, as that build's
# record names it (by default this tree's own build's for the package's kind), 2026-11-21. A package that holds none
# ships nothing of WebRTC.
$webrtcDll = Join-Path $root 'datachannel.dll'
$webrtcRecordSha256 = ''
$webrtcRefusals = @()
if (Test-Path -LiteralPath $webrtcDll -PathType Leaf) {
    if (-not $WebRtcRecord) {
        $kind = [string] $manifest.architecture
        $WebRtcRecord = Join-Path (Get-DepsRoot -Arch $kind) "$(if ($kind -eq 'x64') { 'webrtc-trial' } else { 'webrtc' })\$WebRtcRecordName"
    }
    if (Test-Path -LiteralPath $WebRtcRecord -PathType Leaf) {
        $webrtcRecordSha256 = Get-Sha256 $WebRtcRecord
        $built = $null
        try { $built = Get-Content -LiteralPath $WebRtcRecord -Raw | ConvertFrom-Json } catch { }
        $webrtcRefusals = @(Get-WebRtcRecordRefusals $built ([string] $manifest.architecture) $webrtcDll)
    } else {
        $webrtcRefusals = @("there is no record of the WebRTC build at $WebRtcRecord")
    }
}
$signatureChecks = @($programs | ForEach-Object {
    if (-not $seen.Contains($_)) { throw "Missing release executable: $_" }
    $signature = Get-AuthenticodeSignature -LiteralPath (Join-Path $root $_)
    [ordered]@{ file = $_; status = [string]$signature.Status }
})
# The setup made from this package and, where it carries it, the other kind's: exactly their files, their manifests, the
# marker and qKVM Setup's update check of each kind (a Windows program the setup carries and never installs as such);
# for the user alone.
$setup = $null
if ($SetupDirectory) {
    $setupRoot = (Resolve-Path -LiteralPath $SetupDirectory).Path.TrimEnd('\')
    $setupName = Get-SetupName ([string] $manifest.version)
    $setupFile = Join-Path $setupRoot $setupName
    if (-not (Test-Path -LiteralPath $setupFile -PathType Leaf)) { throw "The setup folder has no $setupName." }
    if ((Get-Item -LiteralPath $setupFile).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'The setup is redirected.' }
    # (Every package it carries, this one among them, each of its own kind; x64's always.)
    $setupPackages = [ordered]@{}
    $named = @($SetupPackageDirectory | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
    if (-not $named.Count) { $named = @($root) }
    foreach ($folder in $named) {
        $packageRoot = (Resolve-Path -LiteralPath $folder).Path.TrimEnd('\')
        $packageManifestFile = Join-Path $packageRoot 'package-manifest.json'
        $packageManifest = Get-Content -LiteralPath $packageManifestFile -Raw | ConvertFrom-Json
        $kind = [string] $packageManifest.architecture
        if (@('x64', 'arm64') -cnotcontains $kind -or $setupPackages.Contains($kind) -or [string] $packageManifest.version -cne [string] $manifest.version) {
            throw 'The packages the setup carries are not one of each kind, of this version.'
        }
        if ($packageRoot -ine $root) {
            # (The other kind's package, as its own audit holds it: every file by its manifest's size and SHA-256.)
            foreach ($entry in $packageManifest.files) {
                $file = Join-Path $packageRoot (([string] $entry.path) -replace '/', '\')
                if (-not (Test-Path -LiteralPath $file -PathType Leaf) -or (Get-Item -LiteralPath $file).Length -ne [long] $entry.bytes -or
                    (Get-FileHash -LiteralPath $file).Hash.ToLowerInvariant() -cne [string] $entry.sha256) {
                    throw "The $kind package the setup carries differs from its inventory: $($entry.path)"
                }
            }
        }
        $setupPackages[$kind] = [pscustomobject]@{ Kind = $kind; Root = $packageRoot; ManifestFile = $packageManifestFile; Manifest = $packageManifest }
    }
    if (-not $setupPackages.Contains('x64') -or @($setupPackages.Values | Where-Object { $_.Root -ieq $root }).Count -ne 1) {
        throw 'The setup carries x64''s package always, and this one among them.'
    }
    $markerBytes = [byte[]] @(0x73, 0x65, 0x74, 0x75, 0x70, 0x0A)
    $markerFile = Join-Path $setupRoot 'inputs\qkvm-desktop.installed'
    if (-not (Test-Path -LiteralPath $markerFile -PathType Leaf) -or
        (Get-FileHash -LiteralPath $markerFile).Hash.ToLowerInvariant() -cne [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($markerBytes)).ToLowerInvariant()) {
        throw 'The setup''s marker is not "setup" and a line feed.'
    }
    $wanted = @{}
    foreach ($one in $setupPackages.Values) {
        foreach ($entry in $one.Manifest.files) {
            $wanted[(Join-Path $one.Root ($entry.path -replace '/', '\')).ToLowerInvariant()] = @{ Path = $entry.path; Bytes = [long] $entry.bytes; Sha256 = [string] $entry.sha256 }
        }
        $wanted[$one.ManifestFile.ToLowerInvariant()] = @{ Path = 'package-manifest.json'; Bytes = (Get-Item -LiteralPath $one.ManifestFile).Length; Sha256 = (Get-FileHash -LiteralPath $one.ManifestFile).Hash.ToLowerInvariant() }
    }
    $wanted[$markerFile.ToLowerInvariant()] = @{ Path = 'qkvm-desktop.installed'; Bytes = $markerBytes.Length; Sha256 = (Get-FileHash -LiteralPath $markerFile).Hash.ToLowerInvariant() }
    # (Each update check stands at the program's own name while an update has the earlier version set aside on a PC of its
    # kind: it is held to the rules of the program that ships, read from its own bytes (review i020-2, L10). A program for
    # that kind of Windows that imports, at its start or when first called, nothing but Windows' kernel32.dll and
    # user32.dll, as it runs where nothing of the package's is; its symbols named by a bare file name; its signature
    # counted with the programs'.)
    $checks = [ordered]@{}
    foreach ($kind in @(@('x64', 'arm64') | Where-Object { $setupPackages.Contains($_) })) {
        $checkName = if ($kind -ceq 'arm64') { 'qkvm-update-check-arm64.exe' } else { 'qkvm-update-check.exe' }
        $checkFile = Join-Path $setupRoot "inputs\$checkName"
        if (-not (Test-Path -LiteralPath $checkFile -PathType Leaf) -or [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($checkFile), 0, 2) -cne 'MZ') {
            throw "The setup's update check for $kind is not a Windows program."
        }
        $checkBytes = [IO.File]::ReadAllBytes($checkFile)
        try {
            $checkImports = @(Get-PeImports -Bytes $checkBytes)
        } catch {
            throw "What the setup's update check for $kind links cannot be read ($($_.Exception.Message))."
        }
        if ((Get-PeArchitecture $checkBytes) -cne $kind) { throw "The setup's update check for $kind is not a program for $kind Windows." }
        $checkOthers = @($checkImports | Where-Object { $_ -notin 'kernel32.dll', 'user32.dll' })
        if ($checkOthers.Count) { throw "The setup's update check for $kind imports more than kernel32.dll and user32.dll: $($checkOthers -join ', ')" }
        if (-not (Test-BareDebugPath $checkFile)) { throw "The setup's update check for $kind names its symbols with a folder: $(@(Get-PeDebugPaths $checkFile) -join ', ')" }
        # (And built with what tools/image-protections.json asks of our own programs of its kind, by the name it is carried
        # under, read from its bytes, as every image of the package is: what it lacks opens the protections gate, which is
        # never accepted; on ARM64 CFG as well as the signed returns, which its static runtime's functions, nearly all of
        # it, give it either way. Review r2 of arm and armp, L1.)
        $checkLacks = @(Get-ImageFileProtectionRefusals -File $checkFile -Name $checkName -Arch $kind)
        $protectionRefusals += @($checkLacks | ForEach-Object { "the setup's $_" })
        $checkHas = @()
        try { $checkHas = @(Get-PeProtectionNames (Get-PeProtections $checkBytes)) } catch { }
        $checkSha256 = (Get-FileHash -LiteralPath $checkFile).Hash.ToLowerInvariant()
        $wanted[$checkFile.ToLowerInvariant()] = @{ Path = $checkName; Bytes = $checkBytes.Length; Sha256 = $checkSha256 }
        $checks[$kind] = [ordered]@{ architecture = $kind; name = $checkName; file = $checkFile; bytes = $checkBytes.Length; sha256 = $checkSha256; imports = $checkImports; has = $checkHas }
    }
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
    # (The setup that ships, by its own bytes (outside review of 0.2.0, 1.9): the very one its build's record names,
    # which tools/build_setup.ps1 writes beside it as it makes it, of these packages' manifests, of the update checks it
    # carries, of the compiler's list above and of the pinned Inno Setup; and its own version resource the package's.
    # A setup of the same name from another build, beside this build's other files, is refused: what is compressed in it
    # cannot be read here, so its bytes are held to the record of the build that read them.)
    $recordFile = Join-Path $setupRoot 'setup-record.json'
    if (-not (Test-Path -LiteralPath $recordFile -PathType Leaf)) { throw 'The setup folder has no setup-record.json, the record of the build that made the setup.' }
    $made = Get-Content -LiteralPath $recordFile -Raw | ConvertFrom-Json
    $setupSha256 = (Get-FileHash -LiteralPath $setupFile).Hash.ToLowerInvariant()
    if ([string] $made.setup.name -cne $setupName -or [string] $made.setup.sha256 -cne $setupSha256 -or [long] $made.setup.bytes -ne (Get-Item -LiteralPath $setupFile).Length) {
        throw "The setup is not the one its build's record names (setup-record.json): $setupName is another build's."
    }
    $kinds = @(@('x64', 'arm64') | Where-Object { $setupPackages.Contains($_) })
    if ($made.schema -ne 2 -or [string] $made.version -cne [string] $manifest.version -or (@($made.architectures) -join ',') -cne ($kinds -join ',') -or
        @($made.packages).Count -ne $kinds.Count -or
        @($setupPackages.Values | Where-Object { $one = $_; @($made.packages | Where-Object { $_.architecture -ceq $one.Kind -and [string] $_.manifestSha256 -ceq (Get-FileHash -LiteralPath $one.ManifestFile).Hash.ToLowerInvariant() }).Count -ne 1 }).Count) {
        throw 'The setup''s record names other packages than these.'
    }
    if (@($made.updateChecks).Count -ne $checks.Count -or
        @($checks.Values | Where-Object { $one = $_; @($made.updateChecks | Where-Object { $_.architecture -ceq $one.architecture -and [string] $_.sha256 -ceq $one.sha256 -and [long] $_.bytes -eq $one.bytes }).Count -ne 1 }).Count) {
        throw 'The setup''s record names other update checks than the ones it carries.'
    }
    if ([string] $made.list -cne (Get-FileHash -LiteralPath (Join-Path $setupRoot 'setup-files.txt')).Hash.ToLowerInvariant()) {
        throw 'The compiler''s list of what the setup holds is not the one its record names.'
    }
    if ([string] $made.inno.version -cne $InnoVersion) { throw "The setup was made with Inno Setup $($made.inno.version), not the pinned $InnoVersion." }
    $setupInfo = [Diagnostics.FileVersionInfo]::GetVersionInfo($setupFile)
    if ("$($setupInfo.ProductVersion)".Trim() -cne [string] $manifest.version -or "$($setupInfo.FileVersion)".Trim() -cne [string] $manifest.version) {
        throw "The setup says it is version $("$($setupInfo.ProductVersion)".Trim()), not the package's $($manifest.version)."
    }
    $setup = [ordered]@{ name = $setupName; bytes = (Get-Item -LiteralPath $setupFile).Length
        sha256 = (Get-FileHash -LiteralPath $setupFile).Hash.ToLowerInvariant(); files = $inSetup.Count; manifestLevel = $level
        architectures = $kinds
        updateChecks = @($checks.Values | ForEach-Object { [ordered]@{ architecture = $_.architecture; bytes = $_.bytes; sha256 = $_.sha256; imports = $_.imports; has = $_.has } }) }
    $signatureChecks += [ordered]@{ file = $setupName; status = [string] (Get-AuthenticodeSignature -LiteralPath $setupFile).Status }
    foreach ($one in $checks.Values) {
        $signatureChecks += [ordered]@{ file = $one.name; status = [string] (Get-AuthenticodeSignature -LiteralPath $one.file).Status }
    }
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
$ffmpegRefusals = @(Get-FFmpegRefusals $ffmpegConfiguration -Arch ([string] $manifest.architecture))
$ffmpegMinimal = $ffmpegRefusals.Count -eq 0
# ...and the very DLLs the release's FFmpeg part made for this kind, as its record names them (-FFmpegRecord): every
# FFmpeg library the package holds, by name, size and SHA-256, and none it lacks. Without the record this is not known.
$ffmpegLibraries = @($binaries | Where-Object { ($_.path.Split('/')[-1]) -match '^(avcodec|avutil|avformat|avfilter|avdevice|swresample|swscale|postproc)-\d+\.dll$' })
$ffmpegRecordSha256 = ''
if ($FFmpegRecord) {
    $ffmpegRecordSha256 = (Get-FileHash -LiteralPath $FFmpegRecord -Algorithm SHA256).Hash.ToLowerInvariant()
    $recordOfFFmpeg = $null
    try { $recordOfFFmpeg = Get-Content -LiteralPath $FFmpegRecord -Raw | ConvertFrom-Json } catch { }
    $ffmpegRecordRefusals = @(Get-FFmpegDllRefusals -Record $recordOfFFmpeg -Kind ([string] $manifest.architecture) -Dlls $ffmpegLibraries)
} else {
    $ffmpegRecordRefusals = @('no record of the FFmpeg part was given (-FFmpegRecord)')
}
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
    [ordered]@{ id = 'ffmpeg-record'; open = $ffmpegRecordRefusals.Count -gt 0
        text = 'FFmpeg''s DLLs are the ones the release''s FFmpeg part built for this kind of Windows, by name, size and SHA-256 as its record names them (build_release.ps1 -Part ffmpeg).'
        beta = '' }
    [ordered]@{ id = 'sources'; open = $true
        text = 'Exact corresponding source distributions, build instructions and public source availability are not verified by this audit.'
        beta = 'Owner, 2026-11-06 (D9, A10, A11): the source archive of the public commit and the third-party source bundle are release files (tools/release_files.ps1), and tools/verify_release.ps1 checks them as published.' }
    [ordered]@{ id = 'signatures'; open = @($signatureChecks | Where-Object status -ne 'Valid').Count -gt 0
        text = 'The programs are signed with a trusted code-signing certificate.'
        beta = 'Owner, 2026-11-06 (D1, D8): beta 1 has no certificate; the download page says what SmartScreen shows and how to check the SHA-256.' +
            $(if ($setup) { ' The setup and the update check it carries are unsigned as well until a certificate exists, and Windows warns before the setup runs (owner, 2026-11-08).' } else { '' }) }
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
    [ordered]@{ id = 'protections'; open = $protectionRefusals.Count -gt 0
        text = 'Every program and library, and each update check the setup carries, is built with what tools/image-protections.json asks of its kind of Windows: our own with ASLR, high entropy, DEP, CFG and the guard of returns of its kind, the others with what their x64 builds have.'
        beta = '' }
    [ordered]@{ id = 'webrtc'; open = $webrtcRefusals.Count -gt 0
        text = 'datachannel.dll is the one a build of the pinned libdatachannel and Mbed TLS sources for this kind of Windows made, by size and SHA-256 as that build''s record names it (experiments/webrtc/build-cpp-dependencies.ps1).'
        beta = '' }
)
$blockers = @($gates | Where-Object open | ForEach-Object { $_.text })
$refusedForBeta = @($gates | Where-Object { $_.open -and -not $_.beta } | ForEach-Object { $_.id })
$result = [ordered]@{
    schema = 2
    checkedAt = [DateTime]::UtcNow.ToString('o')
    version = $manifest.version
    architecture = $manifest.architecture
    profile = $(if ($Beta) { 'beta' } else { 'development' })
    packageManifestSha256 = (Get-FileHash -LiteralPath $manifestFile).Hash.ToLowerInvariant()
    integrityVerified = $true
    inventoriedFiles = $seen.Count
    binaries = $binaries
    requiredNoticesPresent = $missing.Count -eq 0
    missingNotices = $missing
    unlistedLibraries = $unlisted
    links = $links
    protections = $protections
    protectionRefusals = $protectionRefusals
    webrtcRecord = $(if ($webrtcRecordSha256) { [ordered]@{ file = $WebRtcRecord; sha256 = $webrtcRecordSha256 } } else { $null })
    webrtcRefusals = $webrtcRefusals
    unlistedLinks = $unlistedLinks
    ffmpegConfiguration = $ffmpegConfiguration
    ffmpegRefusals = $ffmpegRefusals
    ffmpegRecordSha256 = $ffmpegRecordSha256
    ffmpegRecordRefusals = $ffmpegRecordRefusals
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
