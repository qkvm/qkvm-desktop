#Requires -Version 7.2
<#
.SYNOPSIS
Puts together the files a release publishes, from one run of tools/build_release.ps1, and nothing else.

.DESCRIPTION
    pwsh -File tools\release_files.ps1 -Run <Root>\runs\<time> -Export <the export's folder> -Out <new folder>
        -Arm64Startup <the record of the ARM64 ZIP's start checks, made on Windows on Arm>

After the run, on the PC that made it (build_release.ps1 -Part build: its <Root>\src still holds the WebRTC
sources it built from, and its <Root>\downloads\ffmpeg the FFmpeg part's source archives), once the ARM64 ZIP's start
checks, which an x64 PC cannot run, have passed on Windows on Arm: tools/test_package_archive.ps1 -Archive <the run's
ARM64 ZIP> -Evidence <a new file> there, run from a copy of the export's tools folder, and that file brought back and
named with -Arm64Startup (review armp-1 M3). It must say the checks passed, on Windows on Arm, for the ARM64 package,
of exactly the run's ARM64 ZIP by name, size and SHA-256, made by the export's tools (tools/test_package_archive.ps1,
tools/test_package.ps1, tools/build_common.ps1 and tools/image-protections.json, each by its SHA-256, as the run's
build record names them too; review r2 of arm and armp, L2) (build_common.ps1's Get-Arm64StartupRefusals), or nothing
is made; it is kept in the run folder as
arm64-startup.json, which tools/release_commands.ps1 asks for in its turn. The release files are an
allowlist, and the folder holds those six and nothing more:
- qKVM-Desktop-<version>-windows-x64.zip and qKVM-Desktop-<version>-windows-arm64.zip, the run's ZIP of each kind,
  each checked against its build record;
- qKVM-Desktop-<version>-windows-setup.exe, the setup the run made from those ZIPs' package folders, which installs
  the PC's own kind (tools/build_setup.ps1), from where the run keeps it (setup/), checked against its build record,
  and only from a run whose record says the setup's static test passed;
- qKVM-Desktop-<version>-source.zip, the program's source: git archive of the public commit the run was built
  from (the export must be at that commit);
- qKVM-Desktop-<version>-third-party-sources.zip, the sources of what the build made of other projects: FFmpeg's
  and zlib's release archives (as the FFmpeg part's record names them, by size and SHA-256, the archives both kinds'
  FFmpeg was built from), Mbed TLS's, and libdatachannel with its submodules at their pinned commits, each
  checked against the record (a submodule moved off the commit libdatachannel records, "+" in git submodule
  status, is refused, and so is one the build uses that is not checked out, "-"; the one it never fetches is left
  out, as it was at the build) and against each kind's WebRTC build record (webrtc-record.json, the one that kind's
  beta audit held its datachannel.dll to: its commits and Mbed TLS archive those named), with a README that says what
  each is and how it was built (the build scripts are in the program's source). Each tree is written whole by git
  archive and unpacked whole by Windows' tar (either ending in an error stops everything, in its own words), and the
  bundle, as it is written, holds its archives by size and SHA-256 and libdatachannel as the tree each kind's build read,
  by its digest (build_common.ps1's Get-SourcesBundleRefusals), or it is removed and nothing is made of it; the refusal
  names the files where it differs from the checkout the build read. A bundle not yet so checked is removed however this
  ends (outside review 2, 5.1). So the release files are made of the last run built in its Root, before another is
  built there, with its audit-beta-<kind>.json and its two webrtc-record.json where the run left them;
- SHA256SUMS over the five, as sha256sum writes it. The owner signs it (tools/sign_release.ps1), which adds
  SHA256SUMS.sig.
Refused: a trial run, a run of a record before both kinds (schema 1), a run whose beta audit or byte scan did not
pass, a run without the record of its ARM64 ZIP's start checks passed on Windows on Arm by the export's tools, a run whose
build record names other tools of those checks than the export's, a ZIP that is not the one recorded or that holds symbols, a second program or latest.json, a run without a setup or whose setup is not the one recorded
or failed its test, an export at another commit, and an out folder with anything in it. Any other .exe, any .pdb
and latest.json are refused as release files by name.
The ZIPs, the setup, the source archive and SHA256SUMS are looked through by the private byte scan once more
(-Scanner, or the monorepo's beside this desktop folder): inside the setup only what is not compressed, as the
run's own scan saw all of it in the setup's uncompressed twin. The third-party archives are other projects' own
bytes, checked by hash. The run folder gets release-files.json: every release file with its size and SHA-256.
#>
param(
    [Parameter(Mandatory)] [string] $Run,
    [Parameter(Mandatory)] [string] $Export,
    [Parameter(Mandatory)] [string] $Out,
    [string] $Scanner = '',
    # The record tools/test_package_archive.ps1 -Evidence wrote on Windows on Arm, of the run's ARM64 ZIP.
    [string] $Arm64Startup = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\build_common.ps1"

$Run = (Resolve-Path -LiteralPath $Run).Path
$record = Get-Content -LiteralPath (Join-Path $Run 'build-record.json') -Raw | ConvertFrom-Json
$version = [string] $record.version
if ($record.schema -ne 2 -or $version -notmatch '^\d+\.\d+\.\d+$') { throw 'The build record is not one this script knows (a run of both kinds of Windows, schema 2): build the run again with the export''s tools/build_release.ps1.' }
if ($record.trial) { throw 'This run is a trial (built with a throwaway key): nothing of it is released. A release is built without -Trial, with the release key''s public half in the publish preset.' }
if ($record.checks.betaAudit -cne 'accepted') { throw "The run's beta audit did not pass: $($record.checks.betaRefused -join ', '). Its words are in the run's logs (9-audit-beta-<kind>.txt) and its records in the run folder (audit-beta-<kind>.json): put right what it refused, and build again." }
if ($record.checks.privateScan -cne 'clean') { throw "The run's private byte scan did not pass ($($record.checks.privateScan)). Its words are in the run's logs (10-scan.txt); a run whose scan was not run is built again with -Scanner." }

# Each kind's ZIP: the one recorded, holding one folder with one program and no symbols.
$kinds = @('x64', 'arm64')
$packaged = @($record.packages | ForEach-Object { [string] $_.architecture })
if ((@($packaged | Sort-Object -CaseSensitive) -join ',') -cne 'arm64,x64') { throw "The run is not of one package of each kind of Windows (it is of $($packaged -join ', '))." }
$zipNames = [ordered]@{}
foreach ($kind in $kinds) {
    $built = @($record.packages | Where-Object { [string] $_.architecture -ceq $kind })[0]
    $zipName = Get-ZipName $version $kind
    if ($built.archive -cne $zipName) { throw "The run's $kind ZIP is not named $zipName." }
    $zip = Join-Path $Run $zipName
    if (-not (Test-Path -LiteralPath $zip -PathType Leaf) -or (Get-Item -LiteralPath $zip).Length -ne $built.bytes -or (Get-Sha256 $zip) -cne $built.sha256) {
        throw "The $kind ZIP is not the one the build record names: take the run folder as build_release.ps1 left it."
    }
    Assert-ReleaseZip $zip "qKVM-Desktop-$version-windows-$kind"
    $zipNames[$kind] = $zipName
}
# The export, at the commit the run was built from.
$git = (Get-Command git -CommandType Application | Select-Object -First 1).Source
$exportPath = (Resolve-Path -LiteralPath $Export).Path
# (Every native command's exit code is read where it ends: a git or a tar that fails may still have said something, or
# written part of what it was asked for; outside review 2, 5.1.)
$head = "$(& $git -C $exportPath rev-parse HEAD)".Trim()
if ($LASTEXITCODE -ne 0) { throw "git could not say the export's commit (git exit $LASTEXITCODE): name with -Export the export's folder, the git repository tools/export_public.py made." }
if ($head -cne $record.source.commit) { throw "The export is at $head, the run was built from $($record.source.commit): name with -Export the export at that commit." }
$changes = @(& $git -C $exportPath status --porcelain --untracked-files=all)
if ($LASTEXITCODE -ne 0) { throw "git status of the export failed (git exit $LASTEXITCODE), so whether it has changes is not known: put the export right (git status in $exportPath says what is wrong), or export again." }
if ($changes.Count) { throw 'The export has changes that are not committed: put it back as committed (git status says what changed); a release is made of its commit alone.' }

# The ARM64 package's start checks, which an x64 PC cannot run: passed on Windows on Arm, for this very ZIP by its
# SHA-256 (review armp-1 M3), made by this export's tools, each by its SHA-256, as the run's build record names them too
# (review r2 of arm and armp, L2).
$exportTools = Join-Path $exportPath 'tools'
$tools = Get-StartupCheckTools $exportTools
if (-not $record.checks.PSObject.Properties['arm64StartupTools']) {
    throw 'The run''s build record names no tools of the ARM64 start checks (a run of an earlier tools/build_release.ps1): build the run again with this export.'
}
$recordTools = @(Get-StartupToolDifferences -Said $record.checks.arm64StartupTools -Wanted $tools)
if ($recordTools.Count) {
    throw "The run's build record names other tools of the ARM64 start checks than the export's ($($recordTools -join ', ') differ): the run was not built from this export's tree as it is."
}
$armBuilt = @($record.packages | Where-Object { [string] $_.architecture -ceq 'arm64' })[0]
$started = @(Get-Arm64StartupRefusals -Evidence $Arm64Startup -Name $zipNames['arm64'] -Bytes ([long] $armBuilt.bytes) -Sha256 ([string] $armBuilt.sha256) -Tools $tools)
if ($started.Count) {
    throw "The ARM64 package's start checks have not passed on Windows on Arm for this run's ARM64 ZIP: $($started -join '; '). Copy the export's tools folder ($exportTools) to Windows on Arm with that ZIP, run tools\test_package_archive.ps1 -Archive <that ZIP> -Evidence <a new file> from that copy there, and name that file here with -Arm64Startup."
}
$startedCopy = Join-Path $Run $Arm64StartupName
$startedFrom = (Resolve-Path -LiteralPath $Arm64Startup).Path
if ((Test-Path -LiteralPath $startedCopy) -and $startedFrom -ine $startedCopy -and (Get-Sha256 $startedCopy) -cne (Get-Sha256 $startedFrom)) {
    throw "$startedCopy is there already, and is another record of the ARM64 ZIP's start checks: name that one with -Arm64Startup, or find why there are two."
}

# The setup: the one the run made from its package and tested, where the run keeps it.
$setupName = Get-SetupName $version
if (-not $record.PSObject.Properties['setup'] -or -not $record.setup) { throw 'The run made no setup: its build record names none.' }
$tested = $record.checks.PSObject.Properties['setupTest']
if (-not $tested -or $tested.Value -cne 'passed') { throw "The run's setup's static test did not pass." }
if ($record.setup.name -cne $setupName) { throw "The run's setup is not named $setupName." }
if ($record.setup.file -cne "setup/$setupName") { throw "The build record names its setup not where the run keeps it (setup/$setupName)." }
$setup = Join-Path $Run "setup\$setupName"
if (-not (Test-Path -LiteralPath $setup -PathType Leaf) -or (Get-Item -LiteralPath $setup).Length -ne $record.setup.bytes -or
    (Get-Sha256 $setup) -cne $record.setup.sha256) {
    throw 'The setup is not the setup the build record names.'
}

if ((Test-Path -LiteralPath $Out) -and @(Get-ChildItem -LiteralPath $Out -Force).Count) { throw "$Out is not empty: name a new or empty folder with -Out." }
$Out = (New-Item -ItemType Directory -Force -Path $Out).FullName
$sourceName = "qKVM-Desktop-$version-source.zip"
$bundleName = "qKVM-Desktop-$version-third-party-sources.zip"

foreach ($zipName in $zipNames.Values) { Copy-Item -LiteralPath (Join-Path $Run $zipName) -Destination (Join-Path $Out $zipName) }
Copy-Item -LiteralPath $setup -Destination (Join-Path $Out $setupName)
# (Every git archive here as committed, byte for byte: never with line ends turned to Windows'.)
$asCommitted = @('-c', 'core.autocrlf=false', '-c', 'core.eol=lf')
$said = @(& $git -C $exportPath @asCommitted archive --format=zip "--prefix=qkvm-desktop-$version/" "--output=$(Join-Path $Out $sourceName)" $head 2>&1 | ForEach-Object { "$_" })
if ($LASTEXITCODE -ne 0) {
    throw "git archive of the public commit failed (git exit ${LASTEXITCODE}: $(@($said | Select-Object -First 3) -join ' / ')). If the disk of $Out is full, free room there and run this again, naming a new or empty -Out; otherwise git's words say what of the export it could not read."
}

# The third-party sources, from where the run took them, each held to what the build used (outside review 2, 5.1):
# FFmpeg's and zlib's archives to the FFmpeg part's record, by size and SHA-256, as the archives both kinds were built
# from; Mbed TLS's archive and libdatachannel with its submodules to the build record and to each kind's WebRTC build
# record, the one its beta audit held its datachannel.dll to; every tree unpacked whole; and the bundle, as it is written,
# held to all of that once more: its archives by size and SHA-256, its libdatachannel by its digest to the tree each
# kind's build read (build_common.ps1's Get-SourcesBundleRefusals).
$root = Split-Path -Parent (Split-Path -Parent $Run)
$staging = Join-Path $root "release-staging-$([guid]::NewGuid().ToString('N'))"
$tar = "$staging.tar"
$made = Join-Path $Out $bundleName
$bundleChecked = $false
$bundle = (New-Item -ItemType Directory -Path $staging).FullName
# (What to do where the sources are not those built, a sentence of its own: the run's <Root>\src is made anew by every
# run.)
$buildAgain = "Build the run again, and make its release files before another run is built in $root (each run makes $root\src anew)"
# (Where the disk the sources are written on may be full: that disk, and a fresh -Out, since the release files' copies
# are in this one already.)
$fullDisk = "If the disk of $root is full, free room there and run this again, naming a new or empty -Out."
# git archive of $Repository at $Commit, with $Prefix, into the tar: what it could not write stops everything, in its own
# words (outside review 2, 5.1).
function Write-SourcesTar([string] $Repository, [string] $Prefix, [string] $Commit, [string] $What) {
    $said = @(& $git -C $Repository @asCommitted archive --format=tar "--prefix=$Prefix" "--output=$tar" $Commit 2>&1 | ForEach-Object { "$_" })
    $ended = $LASTEXITCODE
    if ($ended -ne 0) {
        $words = @($said | Select-Object -First 3) -join ' / '
        throw "git archive could not write all of $What (git exit ${ended}: $words), and nothing is made of sources written in part. $fullDisk Otherwise git's words say what of the checkout it could not read. $buildAgain."
    }
}
# Windows' tar, unpacking $Tar into $Into: what it could not create stops everything, in its own words.
function Expand-Sources([string] $Tar, [string] $Into, [string] $What) {
    $said = @(& "$env:SystemRoot\System32\tar.exe" -xf $Tar -C $Into 2>&1 | ForEach-Object { "$_" })
    $ended = $LASTEXITCODE
    if ($ended -ne 0) {
        # (Each of its first lines with the file it names and why, the place it would have written left out, and a long
        # line cut in its middle.)
        $words = @($said | Select-Object -First 3 | ForEach-Object {
            $line = $_ -replace "Can't create '[^']*'", "Can't create it"
            if ($line.Length -gt 240) { $line.Substring(0, 120) + '...' + $line.Substring($line.Length - 100) } else { $line }
        }) -join ' / '
        throw "Windows' tar could not unpack all of $What (tar.exe exit ${ended}: $words), and nothing is made of sources unpacked in part. $fullDisk If tar cannot create one of the tree's files on Windows (a name longer than Windows takes), these sources cannot be made here as they were built: the WebRTC sources are to be pinned (build_common.ps1's `$WebRtcPins) at commits whose trees Windows can hold, and the release built again from them."
    }
}
try {
    $lines = @("Sources of the parts of other projects that qKVM Desktop $version builds and ships", '')
    $kept = @()
    # (FFmpeg's and zlib's, as the FFmpeg part's record names them, the archives each kind's FFmpeg was built from by its
    # pins: the build part copied them here from the part.)
    $ffmpegRecord = $record.dependencies.ffmpeg.record
    $ffmpegSources = @($ffmpegRecord.sources)
    if ((@($ffmpegSources | ForEach-Object { ([string] $_.archive) -replace '-[\d.]+\.tar\.\w+$', '' } | Sort-Object -CaseSensitive) -join ',') -cne 'ffmpeg,zlib') {
        throw 'The FFmpeg part''s record does not name FFmpeg''s and zlib''s archives, one each.'
    }
    foreach ($part in $ffmpegSources) {
        $pinned = "$($part.archive) $($part.bytes) $($part.sha256)"
        foreach ($kind in $kinds) {
            $builtOf = @(@($ffmpegRecord.builds) | Where-Object { [string] $_.architecture -ceq $kind })
            if ($builtOf.Count -ne 1 -or @([string] $builtOf[0].pins.ffmpeg, [string] $builtOf[0].pins.zlib) -cnotcontains $pinned) {
                throw "The FFmpeg part's record does not say its $kind FFmpeg was built from $($part.archive) as its sources name it ($pinned): the part is not of the sources offered. Build the FFmpeg part again (build_release.ps1 -Part ffmpeg, twice, with the same -Records), and the run from it."
            }
        }
        $file = Join-Path $root "downloads\ffmpeg\$($part.archive)"
        if (-not (Test-Path -LiteralPath $file -PathType Leaf) -or (Get-Item -LiteralPath $file).Length -ne [long] $part.bytes -or (Get-Sha256 $file) -cne $part.sha256) {
            throw "$($part.archive) is not the archive the build record names (in $root\downloads\ffmpeg, by its size and SHA-256). Build the run again from its FFmpeg part, and make its release files before another run is built in $root (each run's build part copies its FFmpeg part's archives there)."
        }
        Copy-Item -LiteralPath $file -Destination $bundle
        $kept += [ordered]@{ name = [string] $part.archive; bytes = [long] $part.bytes; sha256 = [string] $part.sha256 }
    }
    $webrtc = Join-Path $root 'src\.deps\webrtc-trial'
    $mbedtls = Join-Path $webrtc 'mbedtls-3.6.7.tar.bz2'
    if (-not (Test-Path -LiteralPath $mbedtls -PathType Leaf) -or (Get-Sha256 $mbedtls) -cne $record.dependencies.webrtc.mbedtls) { throw 'The Mbed TLS archive is not the one the build record names.' }
    Copy-Item -LiteralPath $mbedtls -Destination $bundle
    $kept += [ordered]@{ name = 'mbedtls-3.6.7.tar.bz2'; bytes = (Get-Item -LiteralPath $mbedtls).Length; sha256 = [string] $record.dependencies.webrtc.mbedtls }
    $datachannel = Join-Path $webrtc 'libdatachannel'
    $at = "$(& $git -C $datachannel rev-parse HEAD)".Trim()
    if ($LASTEXITCODE -ne 0) { throw "git could not say libdatachannel's commit in $datachannel (git exit $LASTEXITCODE). $buildAgain." }
    if ($at -cne $record.dependencies.webrtc.libdatachannel) { throw "libdatachannel is at $at, not at the recorded commit." }
    # Its submodules, each as libdatachannel records it and as the build record names it. The build fetches those the
    # WebRTC manifest names (experiments/webrtc/dependencies.json, its gitlinks, at the export's commit); any other is
    # not checked out, then or now, and nothing of it is built.
    $manifest = @(& $git -C $exportPath show "HEAD:experiments/webrtc/dependencies.json")
    if ($LASTEXITCODE -ne 0) { throw "git could not show experiments/webrtc/dependencies.json of the export's commit (git exit $LASTEXITCODE)." }
    $used = @(($manifest -join "`n") | ConvertFrom-Json | ForEach-Object { $_.cpp.gitlinks.PSObject.Properties.Name } | ForEach-Object { "deps/$_" })
    if (-not $used.Count) { throw 'The export names no submodules the build uses (experiments/webrtc/dependencies.json).' }
    $recorded = @{}
    foreach ($line in @($record.dependencies.webrtc.submodules)) {
        if ("$line" -cnotmatch '\A([-+U]?)([0-9a-f]{40}) (\S+)') { throw "The build record names a submodule as git does not: $line" }
        $recorded[$Matches[3]] = "$(if ($Matches[1]) { $Matches[1] } else { ' ' })$($Matches[2])"
    }
    $status = @(& $git -C $datachannel submodule status)
    if ($LASTEXITCODE -ne 0) { throw 'git submodule status of libdatachannel failed.' }
    $modules = @()
    foreach ($line in $status) {
        if ("$line" -cnotmatch '\A([ +\-U])([0-9a-f]{40}) (\S+)') { throw "git submodule status says what this does not know: $line" }
        $state, $commit, $path = $Matches[1], $Matches[2], $Matches[3]
        if ($state -cin '+', 'U') { throw "$path is not at the commit libdatachannel records ($state): its source is not the one built." }
        if ($state -ceq '-' -and $used -ccontains $path) { throw "$path is not checked out (-), and the build uses it." }
        if ($recorded[$path] -cne "$state$commit") {
            throw "$path is not as the build record names it (now $state$commit, recorded $(if ($recorded.ContainsKey($path)) { $recorded[$path] } else { 'nothing' }))."
        }
        $recorded.Remove($path)
        if ($state -ceq ' ') { $modules += [pscustomobject]@{ Path = $path; Commit = $commit } }
    }
    foreach ($path in $recorded.Keys) { throw "The build record names $path, which libdatachannel has not." }
    foreach ($path in $used) {
        if (@($modules | Where-Object Path -ceq $path).Count -ne 1) { throw "$path, which the build uses, is not checked out as it was." }
    }
    # Each kind's WebRTC build record (experiments/webrtc/build-cpp-dependencies.ps1 wrote it beside its install folder,
    # after holding the tree it read to the pins: build_common.ps1's New-WebRtcRecord), the one that kind's beta audit
    # held its package's datachannel.dll to (audit-beta-<kind>.json names it by its SHA-256): of the commits and the
    # Mbed TLS archive named above, and naming the tree that build read, which the bundle is held to below.
    $readBy = [ordered]@{}
    # (A field by its path, or nothing where any step of it is missing.)
    $field = {
        param($Object, [string[]] $Path)
        foreach ($name in $Path) {
            if ($null -eq $Object -or $Object -isnot [pscustomobject] -or -not $Object.PSObject.Properties[$name]) { return $null }
            $Object = $Object.$name
        }
        return $Object
    }
    foreach ($kind in $kinds) {
        $file = if ($kind -ceq 'x64') { Join-Path $webrtc $WebRtcRecordName } else { Join-Path $root "src\.deps\arm64\webrtc\$WebRtcRecordName" }
        $auditFile = Join-Path $Run "audit-beta-$kind.json"
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
            throw "There is no record of the $kind WebRTC build at ${file}: what it was built from is not known, so none of it is offered. $buildAgain."
        }
        $audited = $null
        try { $audited = (Get-Content -LiteralPath $auditFile -Raw | ConvertFrom-Json).webrtcRecord } catch { $audited = $null }
        if ($audited -isnot [pscustomobject] -or -not $audited.PSObject.Properties['sha256'] -or [string] $audited.sha256 -cne (Get-Sha256 $file)) {
            throw "$file is not the record of the $kind WebRTC build that the run's beta audit held its datachannel.dll to ($auditFile names another, or none). $buildAgain."
        }
        $said = $null
        try { $said = Get-Content -LiteralPath $file -Raw | ConvertFrom-Json } catch { $said = $null }
        $wrong = @()
        if ((& $field $said @('schema')) -ne 1 -or [string] (& $field $said @('part')) -cne 'webrtc') { $wrong += 'it is no record of a WebRTC build' } elseif ([string] (& $field $said @('architecture')) -cne $kind) { $wrong += "it is of a build for $(& $field $said @('architecture'))" }
        $commitSaid = [string] (& $field $said @('sources', 'libdatachannel', 'commit'))
        if ($commitSaid -cne $at) { $wrong += "it names libdatachannel at $commitSaid, the build record $at" }
        $gitlinks = & $field $said @('sources', 'libdatachannel', 'gitlinks')
        $linked = @(if ($gitlinks -is [pscustomobject]) { $gitlinks.PSObject.Properties | ForEach-Object { [pscustomobject]@{ Path = "deps/$($_.Name)"; Commit = [string] $_.Value } } })
        foreach ($link in $linked) {
            $checkedOut = @($modules | Where-Object Path -ceq $link.Path)
            if (-not $checkedOut.Count) { $wrong += "it names $($link.Path), which is not checked out" }
            elseif ($checkedOut[0].Commit -cne $link.Commit) { $wrong += "it names $($link.Path) at $($link.Commit), checked out at $($checkedOut[0].Commit)" }
        }
        foreach ($module in $modules) {
            if (-not @($linked | Where-Object Path -ceq $module.Path).Count) { $wrong += "it does not name $($module.Path), which is checked out" }
        }
        $mbedtlsSaid = [string] (& $field $said @('sources', 'mbedtls', 'archiveSha256'))
        if ($mbedtlsSaid -cne [string] $record.dependencies.webrtc.mbedtls) { $wrong += "it names Mbed TLS's archive $mbedtlsSaid, the build record $($record.dependencies.webrtc.mbedtls)" }
        $treeSaid = [string] (& $field $said @('sources', 'libdatachannel', 'tree'))
        if ($treeSaid -cnotmatch '\A[0-9a-f]{64}\z') { $wrong += 'it names no tree of libdatachannel' }
        if ($wrong.Count) {
            throw "The record of the $kind WebRTC build ($file) is not of the sources this run names: $($wrong -join '; '). $buildAgain."
        }
        $readBy["the $kind build"] = $treeSaid
    }

    # Unpacked whole, libdatachannel and each submodule it is built with, at its commit (as committed, byte for byte).
    Write-SourcesTar $datachannel 'libdatachannel/' HEAD 'libdatachannel'
    Expand-Sources $tar $bundle 'libdatachannel'
    foreach ($module in $modules) {
        $commit, $path = $module.Commit, $module.Path
        Write-SourcesTar (Join-Path $datachannel $path) "libdatachannel/$path/" $commit "libdatachannel/$path at $commit"
        Expand-Sources $tar $bundle "libdatachannel/$path at $commit"
        $kept += [ordered]@{ name = "libdatachannel/$path"; commit = $commit }
    }
    Remove-Item -LiteralPath $tar -ErrorAction SilentlyContinue
    $kept += [ordered]@{ name = 'libdatachannel'; commit = $at }
    foreach ($entry in $kept) {
        $lines += "$($entry.name): $(if ($entry.Contains('sha256')) { "SHA-256 $($entry.sha256)" } else { "commit $($entry.commit)" })"
    }
    $lines += @('',
        'FFmpeg is built with zlib, for x64 and for ARM64, by tools/build_ffmpeg.ps1 and tools/build_ffmpeg.sh, and',
        'libdatachannel with Mbed TLS, for both, by tools/fetch_webrtc.ps1 and experiments/webrtc, all in the program''s',
        'source archive of this release.',
        'Each part is under its own licence, in its own archive or folder here; THIRD-PARTY.md and licenses/ in the',
        'program say which.')
    Set-Content -LiteralPath (Join-Path $bundle 'README.txt') -Value $lines -Encoding ascii
    # The bundle as written: its archives by size and SHA-256, its libdatachannel the tree each kind's build read. Until
    # that check has passed, the bundle is removed however this ends (a ZIP cut short, a check that cannot read).
    $archives = @($kept | Where-Object { $_.Contains('sha256') } | ForEach-Object { [pscustomobject]@{ name = $_.name; bytes = $_.bytes; sha256 = $_.sha256 } })
    try {
        [IO.Compression.ZipFile]::CreateFromDirectory($bundle, $made)
        $inBundle = @(Get-SourcesBundleRefusals -Bundle $made -Archives $archives -Trees $readBy -Checkout $datachannel)
    } catch {
        throw "The third-party sources could not be written and checked ($($_.Exception.Message)), and nothing is left of them in $Out. If a disk is full, free room there; if a file was held open, close what holds it; then run this again, naming a new or empty -Out."
    }
    if ($inBundle.Count) {
        throw "The third-party sources made here are not what the build used, and were removed: $($inBundle -join '; '). $buildAgain; if the same refusal comes back, the checkout's commits do not give the tree the build read, and the release waits until they do."
    }
    $bundleChecked = $true
} finally {
    Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $tar -Force -ErrorAction SilentlyContinue
    if (-not $bundleChecked) { Remove-Item -LiteralPath $made -Force -ErrorAction SilentlyContinue }
}

# SHA256SUMS, as sha256sum writes it: the hash, two spaces, the name; one line each, a line feed after each.
$names = @($zipNames.Values) + @($setupName, $sourceName, $bundleName) | Sort-Object -CaseSensitive
$sums = ($names | ForEach-Object { "$(Get-Sha256 (Join-Path $Out $_))  $_`n" }) -join ''
[IO.File]::WriteAllBytes((Join-Path $Out 'SHA256SUMS'), [Text.Encoding]::ASCII.GetBytes($sums))
Assert-ReleaseFolder $Out $version

if (-not $Scanner) { $Scanner = Join-Path $PSScriptRoot '..\..\tools\scan_private.py' }
if (-not (Test-Path -LiteralPath $Scanner -PathType Leaf)) { throw 'The private byte scan is not there (-Scanner names it): nothing is released unscanned.' }
$python = (Get-Command python -CommandType Application | Select-Object -First 1).Source
& $python -I (Resolve-Path -LiteralPath $Scanner).Path (Join-Path $Out $zipNames['x64']) (Join-Path $Out $zipNames['arm64']) (Join-Path $Out $setupName) (Join-Path $Out $sourceName) (Join-Path $Out 'SHA256SUMS')
if ($LASTEXITCODE -ne 0) { throw 'The private byte scan refused the release files.' }

$files = @(Get-ChildItem -LiteralPath $Out -File | Sort-Object Name | ForEach-Object {
    [ordered]@{ name = $_.Name; bytes = $_.Length; sha256 = Get-Sha256 $_.FullName } })
# (The record of the ARM64 ZIP's start checks, kept with the run: release_commands.ps1 asks for it on the owner's PC.)
if ($startedFrom -ine $startedCopy) { Copy-Item -LiteralPath $startedFrom -Destination $startedCopy -Force }
[ordered]@{ version = $version; commit = $head; files = $files; arm64Startup = Get-Sha256 $startedCopy } | ConvertTo-Json -Depth 4 |
    Set-Content -LiteralPath (Join-Path $Run 'release-files.json') -Encoding utf8
Write-Output "Release files of qKVM Desktop ${version}: $Out" ($files | ForEach-Object { "  $($_.sha256)  $($_.name)" })
