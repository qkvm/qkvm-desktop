#Requires -Version 7.2
<#
.SYNOPSIS
Puts together the files a release publishes, from one run of tools/build_release.ps1, and nothing else.

.DESCRIPTION
    pwsh -File tools\release_files.ps1 -Run <Root>\runs\<time> -Export <the export's folder> -Out <new folder>

Right after the run (its <Root>\src still holds the WebRTC sources it built from). The release files are an
allowlist, and the folder holds those five and nothing more:
- qKVM-Desktop-<version>-windows-x64.zip, the run's ZIP, checked against its build record;
- qKVM-Desktop-<version>-windows-x64-setup.exe, the setup the run made from that ZIP's package folder
  (tools/build_setup.ps1), from where the run keeps it (setup/), checked against its build record, and only from
  a run whose record says the setup's static test passed;
- qKVM-Desktop-<version>-source.zip, the program's source: git archive of the public commit the run was built
  from (the export must be at that commit);
- qKVM-Desktop-<version>-third-party-sources.zip, the sources of what the build made of other projects: FFmpeg's
  and zlib's release archives, Mbed TLS's, and libdatachannel with its submodules at their pinned commits, each
  checked against the record (a submodule moved off the commit libdatachannel records, "+" in git submodule
  status, is refused, and so is one the build uses that is not checked out, "-"; the one it never fetches is left
  out, as it was at the build), with a README that says what each is and how it was built (the build scripts
  are in the program's source);
- SHA256SUMS over the four, as sha256sum writes it. The owner signs it (tools/sign_release.ps1), which adds
  SHA256SUMS.sig.
Refused: a trial run, a run whose beta audit or byte scan did not pass, a ZIP that is not the one recorded or
that holds symbols, a second program or latest.json, a run without a setup or whose setup is not the one recorded
or failed its test, an export at another commit, and an out folder with anything in it. Any other .exe, any .pdb
and latest.json are refused as release files by name.
The ZIP, the setup, the source archive and SHA256SUMS are looked through by the private byte scan once more
(-Scanner, or the monorepo's beside this desktop folder): inside the setup only what is not compressed, as the
run's own scan saw all of it in the setup's uncompressed twin. The third-party archives are other projects' own
bytes, checked by hash. The run folder gets release-files.json: every release file with its size and SHA-256.
#>
param(
    [Parameter(Mandatory)] [string] $Run,
    [Parameter(Mandatory)] [string] $Export,
    [Parameter(Mandatory)] [string] $Out,
    [string] $Scanner = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\build_common.ps1"

$Run = (Resolve-Path -LiteralPath $Run).Path
$record = Get-Content -LiteralPath (Join-Path $Run 'build-record.json') -Raw | ConvertFrom-Json
$version = [string] $record.version
if ($record.schema -ne 1 -or $version -notmatch '^\d+\.\d+\.\d+$') { throw 'The build record is not one this script knows.' }
if ($record.trial) { throw 'This run is a trial (built with a throwaway key): nothing of it is released.' }
if ($record.checks.betaAudit -cne 'accepted') { throw "The run's beta audit did not pass: $($record.checks.betaRefused -join ', ')." }
if ($record.checks.privateScan -cne 'clean') { throw "The run's private byte scan did not pass ($($record.checks.privateScan))." }

# The ZIP: the one recorded, holding one folder with one program and no symbols.
$zipName = "qKVM-Desktop-$version-windows-x64.zip"
if ($record.package.archive -cne $zipName) { throw "The run's ZIP is not named $zipName." }
$zip = Join-Path $Run $zipName
if ((Get-Item -LiteralPath $zip).Length -ne $record.package.bytes -or (Get-Sha256 $zip) -cne $record.package.sha256) {
    throw 'The ZIP is not the one the build record names.'
}
Assert-ReleaseZip $zip "qKVM-Desktop-$version-windows-x64"

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

# The export, at the commit the run was built from.
$git = (Get-Command git -CommandType Application | Select-Object -First 1).Source
$exportPath = (Resolve-Path -LiteralPath $Export).Path
$head = (& $git -C $exportPath rev-parse HEAD).Trim()
if ($head -cne $record.source.commit) { throw "The export is at $head, the run was built from $($record.source.commit)." }
if (@(& $git -C $exportPath status --porcelain --untracked-files=all).Count) { throw 'The export has changes that are not committed.' }

if ((Test-Path -LiteralPath $Out) -and @(Get-ChildItem -LiteralPath $Out -Force).Count) { throw "$Out is not empty." }
$Out = (New-Item -ItemType Directory -Force -Path $Out).FullName
$sourceName = "qKVM-Desktop-$version-source.zip"
$bundleName = "qKVM-Desktop-$version-third-party-sources.zip"

Copy-Item -LiteralPath $zip -Destination (Join-Path $Out $zipName)
Copy-Item -LiteralPath $setup -Destination (Join-Path $Out $setupName)
# (Every git archive here as committed, byte for byte: never with line ends turned to Windows'.)
$asCommitted = @('-c', 'core.autocrlf=false', '-c', 'core.eol=lf')
& $git -C $exportPath @asCommitted archive --format=zip "--prefix=qkvm-desktop-$version/" "--output=$(Join-Path $Out $sourceName)" $head
if ($LASTEXITCODE -ne 0) { throw 'git archive of the public commit failed.' }

# The third-party sources, from where the run took them, each checked against the record.
$root = Split-Path -Parent (Split-Path -Parent $Run)
$staging = Join-Path $root "release-staging-$([guid]::NewGuid().ToString('N'))"
$bundle = (New-Item -ItemType Directory -Path $staging).FullName
try {
    $lines = @("Sources of the parts of other projects that qKVM Desktop $version builds and ships", '')
    $kept = @()
    foreach ($part in @($record.dependencies.ffmpeg.ffmpeg, $record.dependencies.ffmpeg.zlib)) {
        $file = Join-Path $root "downloads\ffmpeg\$($part.archive)"
        if ((Get-Sha256 $file) -cne $part.sha256) { throw "$($part.archive) is not the archive the build record names." }
        Copy-Item -LiteralPath $file -Destination $bundle
        $kept += [ordered]@{ name = $part.archive; sha256 = $part.sha256 }
    }
    $webrtc = Join-Path $root 'src\.deps\webrtc-trial'
    $mbedtls = Join-Path $webrtc 'mbedtls-3.6.7.tar.bz2'
    if ((Get-Sha256 $mbedtls) -cne $record.dependencies.webrtc.mbedtls) { throw 'The Mbed TLS archive is not the one the build record names.' }
    Copy-Item -LiteralPath $mbedtls -Destination $bundle
    $kept += [ordered]@{ name = 'mbedtls-3.6.7.tar.bz2'; sha256 = $record.dependencies.webrtc.mbedtls }
    $datachannel = Join-Path $webrtc 'libdatachannel'
    $at = (& $git -C $datachannel rev-parse HEAD).Trim()
    if ($at -cne $record.dependencies.webrtc.libdatachannel) { throw "libdatachannel is at $at, not at the recorded commit." }
    $tar = "$staging.tar"
    & $git -C $datachannel @asCommitted archive --format=tar '--prefix=libdatachannel/' "--output=$tar" HEAD
    if ($LASTEXITCODE -ne 0) { throw 'git archive of libdatachannel failed.' }
    & "$env:SystemRoot\System32\tar.exe" -xf $tar -C $bundle
    # Its submodules, each as libdatachannel records it and as the build record names it. The build fetches those the
    # WebRTC manifest names (experiments/webrtc/dependencies.json, its gitlinks, at the export's commit); any other is
    # not checked out, then or now, and nothing of it is built.
    $used = @((& $git -C $exportPath show "HEAD:experiments/webrtc/dependencies.json") -join "`n" | ConvertFrom-Json |
        ForEach-Object { $_.cpp.gitlinks.PSObject.Properties.Name } | ForEach-Object { "deps/$_" })
    if ($LASTEXITCODE -ne 0 -or -not $used.Count) { throw 'The export names no submodules the build uses (experiments/webrtc/dependencies.json).' }
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
    foreach ($module in $modules) {
        $commit, $path = $module.Commit, $module.Path
        & $git -C (Join-Path $datachannel $path) @asCommitted archive --format=tar "--prefix=libdatachannel/$path/" "--output=$tar" $commit
        if ($LASTEXITCODE -ne 0) { throw "git archive of $path failed." }
        & "$env:SystemRoot\System32\tar.exe" -xf $tar -C $bundle
        $kept += [ordered]@{ name = "libdatachannel/$path"; commit = $commit }
    }
    Remove-Item -LiteralPath $tar -ErrorAction SilentlyContinue
    $kept += [ordered]@{ name = 'libdatachannel'; commit = $at }
    foreach ($entry in $kept) {
        $lines += "$($entry.name): $(if ($entry.Contains('sha256')) { "SHA-256 $($entry.sha256)" } else { "commit $($entry.commit)" })"
    }
    $lines += @('',
        'FFmpeg is built with zlib by tools/build_ffmpeg.ps1 and tools/build_ffmpeg.sh, and libdatachannel with Mbed',
        'TLS by tools/fetch_webrtc.ps1 and experiments/webrtc, all in the program''s source archive of this release.',
        'Each part is under its own licence, in its own archive or folder here; THIRD-PARTY.md and licenses/ in the',
        'program say which.')
    Set-Content -LiteralPath (Join-Path $bundle 'README.txt') -Value $lines -Encoding ascii
    [IO.Compression.ZipFile]::CreateFromDirectory($bundle, (Join-Path $Out $bundleName))
} finally {
    Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
}

# SHA256SUMS, as sha256sum writes it: the hash, two spaces, the name; one line each, a line feed after each.
$names = @($zipName, $setupName, $sourceName, $bundleName) | Sort-Object -CaseSensitive
$sums = ($names | ForEach-Object { "$(Get-Sha256 (Join-Path $Out $_))  $_`n" }) -join ''
[IO.File]::WriteAllBytes((Join-Path $Out 'SHA256SUMS'), [Text.Encoding]::ASCII.GetBytes($sums))
Assert-ReleaseFolder $Out $version

if (-not $Scanner) { $Scanner = Join-Path $PSScriptRoot '..\..\tools\scan_private.py' }
if (-not (Test-Path -LiteralPath $Scanner -PathType Leaf)) { throw 'The private byte scan is not there (-Scanner names it): nothing is released unscanned.' }
$python = (Get-Command python -CommandType Application | Select-Object -First 1).Source
& $python -I (Resolve-Path -LiteralPath $Scanner).Path (Join-Path $Out $zipName) (Join-Path $Out $setupName) (Join-Path $Out $sourceName) (Join-Path $Out 'SHA256SUMS')
if ($LASTEXITCODE -ne 0) { throw 'The private byte scan refused the release files.' }

$files = @(Get-ChildItem -LiteralPath $Out -File | Sort-Object Name | ForEach-Object {
    [ordered]@{ name = $_.Name; bytes = $_.Length; sha256 = Get-Sha256 $_.FullName } })
[ordered]@{ version = $version; commit = $head; files = $files } | ConvertTo-Json -Depth 4 |
    Set-Content -LiteralPath (Join-Path $Run 'release-files.json') -Encoding utf8
Write-Output "Release files of qKVM Desktop ${version}: $Out" ($files | ForEach-Object { "  $($_.sha256)  $($_.name)" })
