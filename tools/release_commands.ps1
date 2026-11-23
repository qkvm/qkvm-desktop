#Requires -Version 7.2
<#
.SYNOPSIS
Says the commands that publish a release, for the owner to run by hand. Runs none of them.

.DESCRIPTION
    pwsh -File tools\release_commands.ps1 -Run <Root>\runs\<time> -Files <release files> -Bucket <R2 bucket>
        -Feed <stable.json> -SetupFeed <stable-setup.json> -Arm64Feed <stable-arm64.json>
        -Arm64SetupFeed <stable-arm64-setup.json> -Records <this PC's FFmpeg records> [-Notes <release notes file>]

For the release files of one run (tools/release_files.ps1), once the owner has signed SHA256SUMS
(tools/sign_release.ps1) and the four payloads (sign_release.ps1 takes them all at once, one passphrase) and
assembled the four feeds: stable.json and stable-arm64.json (the portable copies' of x64 and of Windows on Arm,
prepare_update_payload.ps1 -Arch x64 or arm64) and stable-setup.json and stable-arm64-setup.json (installed
copies', -Setup; the one setup for both kinds). First it checks, against the run's build record and the key the
program pins: each kind's ZIP and the setup are the ones built (the setup goes up as a Windows program,
application/vnd.microsoft.portable-executable); the FFmpeg they carry is the part this PC built and kept the record of
in -Records (build_common.ps1's Get-FFmpegKeptRefusals: the record the run names by its SHA-256 is one kept there, of the
run's commit, and another build of the same pins kept there made the same bytes, for each kind; and x64's, where it is
of 0.1.2's sources and toolchain, made the DLLs 0.1.2's release published, byte for byte), each ZIP's FFmpeg DLLs
and those its package manifest names are that record's, and that manifest is the one the run recorded and the setup
was made of; the ARM64 ZIP's start checks passed on Windows on Arm, for that very ZIP by its SHA-256, by the record
kept with the run (arm64-startup.json: tools/test_package_archive.ps1 -Evidence made it there, release_files.ps1
-Arm64Startup kept it), made by the tools of the run's export, each by the SHA-256 the run's build record names
(checks.arm64StartupTools); SHA256SUMS names exactly the release files, as they are, and
SHA256SUMS.sig is its signature; each portable feed is signed, current, of its kind's portable ZIP, and names this
version and that ZIP by size and SHA-256 at the address it is uploaded to; each installed copies' feed is signed,
current, of its kind's setup platform, and names this version and the setup by size and SHA-256 at the address it is
uploaded to (build_common.ps1's Get-SetupFeedRefusals). Only then does it say, in this order: the files' uploads to the
bucket behind download.qkvm.com (owner, D7; Cloudflare R2 by wrangler, at the version the sites are deployed
with); the GitHub release as a draft pre-release of the tag that is there, made as pypv (D4; release plan B11);
the draft downloaded as pypv into a new folder beside the release files (draft-v<version>; a draft's files are
not at its public address) and checked with the direct host's (tools/verify_release.ps1); the release published;
then the four feeds, so that no copy of the program is told of a release whose page is still a draft; and the
check of everything as published, the four feeds with it. Each versioned file is kept for good under desktop/<version>/ and
may be cached for a year; the feeds are never cached. Nothing is uploaded, signed or published here, and
nothing is asked of any service.
#>
param(
    [Parameter(Mandatory)] [string] $Run,
    [Parameter(Mandatory)] [string] $Files,
    [Parameter(Mandatory)] [ValidatePattern('\A[a-z0-9][a-z0-9-]{1,61}[a-z0-9]\z')] [string] $Bucket,
    [string] $Feed = '',
    [string] $SetupFeed = '',
    [string] $Arm64Feed = '',
    [string] $Arm64SetupFeed = '',
    [string] $Notes = '',
    # This PC's folder of the FFmpeg part's records (build_release.ps1 -Part ffmpeg -Records), kept from release to release.
    [string] $Records = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\build_common.ps1"
. "$PSScriptRoot\release_common.ps1"

$Run = (Resolve-Path -LiteralPath $Run).Path
$Files = (Resolve-Path -LiteralPath $Files).Path
$record = Get-Content -LiteralPath (Join-Path $Run 'build-record.json') -Raw | ConvertFrom-Json
$version = [string] $record.version
if ($record.trial) { throw 'This run is a trial: nothing of it is published. A release is built without -Trial, with the release key''s public half in the publish preset.' }
Assert-ReleaseFolder $Files $version -Signed
if (-not (Test-Path -LiteralPath (Join-Path $Files 'SHA256SUMS.sig'))) {
    throw 'SHA256SUMS is not signed yet: the owner signs it first (tools/sign_release.ps1).'
}
if (-not $Feed) {
    throw 'Sign the payload and assemble stable.json first, then name it with -Feed: the files are uploaded only with the feed that names them, once both are checked.'
}
$feedPath = (Resolve-Path -LiteralPath $Feed).Path
if (-not $SetupFeed) {
    throw 'Sign the setup''s payload and assemble stable-setup.json too, then name it with -SetupFeed: installed copies take their updates from it, and the setup is uploaded only with the feed that names it.'
}
$setupFeedPath = (Resolve-Path -LiteralPath $SetupFeed).Path
if (-not $Arm64Feed -or -not $Arm64SetupFeed) {
    throw 'Sign Windows on Arm''s two payloads too and assemble stable-arm64.json and stable-arm64-setup.json, then name them with -Arm64Feed and -Arm64SetupFeed: every copy on Windows on Arm takes its updates from them.'
}
$arm64FeedPath = (Resolve-Path -LiteralPath $Arm64Feed).Path
$arm64SetupFeedPath = (Resolve-Path -LiteralPath $Arm64SetupFeed).Path
if (@($feedPath, $setupFeedPath, $arm64FeedPath, $arm64SetupFeedPath | Sort-Object -Unique).Count -ne 4) { throw 'The four feeds are four files: one is named twice.' }

# What is uploaded is checked first, against the run and the key the program pins: a versioned file is kept for
# good under its name, and the feed is what every copy of the program takes.
$publicKey = [string] $record.updates.publicKey
if (-not (Test-ReleasePublicKey $publicKey)) { throw 'The build record names no key the program pins.' }
if ($record.schema -ne 2) { throw 'The run''s record is not of both kinds of Windows (schema 2).' }
$zipNames = [ordered]@{}
$builtOf = [ordered]@{}
foreach ($kind in 'x64', 'arm64') {
    $zipName = Get-ZipName $version $kind
    $built = @(@($record.packages) | Where-Object { [string] $_.architecture -ceq $kind })
    if ($built.Count -ne 1 -or $built[0].archive -cne $zipName) { throw "The run built no $kind ZIP named $zipName." }
    $zip = Join-Path $Files $zipName
    if ((Get-Item -LiteralPath $zip).Length -ne $built[0].bytes -or (Get-Sha256 $zip) -cne $built[0].sha256) {
        throw "$zipName is not the ZIP the run built: name with -Files the release files release_files.ps1 made of this run, as they are."
    }
    $zipNames[$kind] = $zipName
    $builtOf[$kind] = $built[0]
}
$setupName = Get-SetupName $version
if (-not $record.PSObject.Properties['setup'] -or -not $record.setup) { throw 'The run made no setup: its build record names none.' }
$setup = Join-Path $Files $setupName
if ((Get-Item -LiteralPath $setup).Length -ne $record.setup.bytes -or (Get-Sha256 $setup) -cne $record.setup.sha256) {
    throw "$setupName is not the setup the run built: name with -Files the release files release_files.ps1 made of this run, as they are."
}
# The FFmpeg that ships: the FFmpeg part this PC built, twice of the same pins, and kept the records of (review armp-1
# M1). The run took a part this PC kept, of its commit, reproduced; each ZIP's FFmpeg DLLs, read out of it, are the ones
# that record names for its kind, and so are those its package manifest names; and that manifest is the one the run
# recorded and the setup was made of (build_setup.ps1's record), so that the setup carries the same.
if (-not $Records) {
    throw 'Name this PC''s folder of FFmpeg records with -Records (build_release.ps1 -Part ffmpeg -Records): the FFmpeg that ships is held to the part this PC built and kept.'
}
$ffmpegKept = Get-FFmpegKeptRefusals -Records $Records -BuildRecord $record -RunCopy (Join-Path $Run $FFmpegRecordName)
# (What to do, said with every refusal of the FFmpeg that ships: nothing of such a run is published.)
$ffmpegTodo = "Nothing of this run is published. -Records names the folder this PC's FFmpeg part kept its records in (build_release.ps1 -Part ffmpeg; qkvm-desktop-records\ffmpeg beside its build folder unless told another); the run must be built on the build box from the part of this PC's second FFmpeg run of the same export, copied there as it is; and the release files are the run's own, as release_files.ps1 made them."
if ($ffmpegKept.Refusals.Count) { throw "The FFmpeg that ships is not the FFmpeg part this PC built and kept: $($ffmpegKept.Refusals -join '; '). $ffmpegTodo" }
$madeOf = @(if ($record.setup.PSObject.Properties['made'] -and $record.setup.made.PSObject.Properties['packages']) { $record.setup.made.packages })
foreach ($kind in 'x64', 'arm64') {
    $inZip = Get-ZipFFmpeg -Zip (Join-Path $Files $zipNames[$kind]) -Top "qKVM-Desktop-$version-windows-$kind"
    $refused = @(Get-FFmpegDllRefusals -Record $ffmpegKept.Record -Kind $kind -Dlls $inZip.Dlls | ForEach-Object { "$kind ZIP: $_" })
    $refused += @(Get-FFmpegDllRefusals -Record $ffmpegKept.Record -Kind $kind -Dlls $inZip.Listed | ForEach-Object { "$kind ZIP's package manifest: $_" })
    $built = $builtOf[$kind]
    if (-not $inZip.ManifestSha256 -or -not $built.PSObject.Properties['manifestSha256'] -or $inZip.ManifestSha256 -cne [string] $built.manifestSha256) {
        $refused += "$kind ZIP's package manifest is not the one the run recorded"
    }
    if (@($madeOf | Where-Object { [string] $_.architecture -ceq $kind -and [string] $_.manifestSha256 -ceq $inZip.ManifestSha256 }).Count -ne 1) {
        $refused += "$kind ZIP's package manifest is not the one the setup was made of"
    }
    if ($refused.Count) { throw "The FFmpeg that ships is not the FFmpeg part this PC built and kept: $($refused -join '; '). $ffmpegTodo" }
}
# The ARM64 package's start checks, which an x64 PC cannot run: passed on Windows on Arm for this very ZIP by its SHA-256,
# as the record kept with the run says (release_files.ps1 -Arm64Startup put it there; review armp-1 M3), made by the
# tools of the run's export, as the run's build record names them from the tree it built (review r2 of arm and armp, L2).
if (-not $record.PSObject.Properties['checks'] -or -not $record.checks.PSObject.Properties['arm64StartupTools']) {
    throw 'The run''s build record names no tools of the ARM64 start checks (a run of an earlier tools/build_release.ps1): build the run again.'
}
$started = @(Get-Arm64StartupRefusals -Evidence (Join-Path $Run $Arm64StartupName) -Name $zipNames['arm64'] -Bytes ([long] $builtOf['arm64'].bytes) -Sha256 ([string] $builtOf['arm64'].sha256) -Tools $record.checks.arm64StartupTools)
if ($started.Count) {
    throw "The ARM64 package's start checks have not passed on Windows on Arm for this run's ARM64 ZIP: $($started -join '; '). They are run there with tools\test_package_archive.ps1 -Archive <that ZIP> -Evidence <a new file>, from a copy of the export's tools folder of the run's commit ($($record.source.commit)), and the record is kept with the run by release_files.ps1 -Arm64Startup ($Arm64StartupName, copied back here with the run folder)."
}
$sumsBytes = [IO.File]::ReadAllBytes((Join-Path $Files 'SHA256SUMS'))
$listed = Test-Sha256Sums $sumsBytes
$released = @(Get-ReleaseFileNames $version | Where-Object { $_ -cne 'SHA256SUMS' } | Sort-Object -CaseSensitive)
if ((@($listed | Sort-Object -CaseSensitive) -join '|') -cne ($released -join '|')) { throw 'SHA256SUMS does not name exactly the release files.' }
foreach ($line in [Text.Encoding]::ASCII.GetString($sumsBytes).TrimEnd("`n").Split("`n")) {
    $hash, $name = $line -split '  ', 2
    if ((Get-Sha256 (Join-Path $Files $name)) -cne $hash) { throw "$name is not the file SHA256SUMS names." }
}
if (-not (Test-ReleaseSignature -Data $sumsBytes -Signature ([IO.File]::ReadAllBytes((Join-Path $Files 'SHA256SUMS.sig'))) -PublicKey $publicKey)) {
    throw 'SHA256SUMS.sig is not a signature of SHA256SUMS by the key the program pins.'
}
# Each kind's two feeds: the portable copies', of that kind's ZIP; installed copies', of the setup; each signed by the
# key the program pins, of this version, naming what is uploaded where it is uploaded.
$setupAddress = "https://download.qkvm.com/desktop/$version/$setupName"
$feedFiles = [ordered]@{ 'stable.json' = $feedPath; 'stable-setup.json' = $setupFeedPath; 'stable-arm64.json' = $arm64FeedPath; 'stable-arm64-setup.json' = $arm64SetupFeedPath }
$said = [ordered]@{}
foreach ($kind in 'x64', 'arm64') {
    $name = if ($kind -ceq 'x64') { 'stable.json' } else { 'stable-arm64.json' }
    # (What to do, said with each refusal of a feed: make it again of this run's file of that kind.)
    $portableTodo = "Make it again of this run: prepare_update_payload.ps1 -Portable -Arch $kind -Package <the release files' $($zipNames[$kind])> -PackageUrl https://download.qkvm.com/desktop/$version/$($zipNames[$kind]), signed with sign_release.ps1 and put in its envelope with assemble_update_feed.ps1."
    $setupTodo = "Make it again of this run: prepare_update_payload.ps1 -Setup -Arch $kind -Package <the release files' $setupName> -PackageUrl $setupAddress -SetupRecord <the run's setup\setup-record.json>, signed with sign_release.ps1 and put in its envelope with assemble_update_feed.ps1."
    try {
        $fields = Test-UpdatePayload -Bytes (Read-SignedFeed -Feed $feedFiles[$name] -PublicKey $publicKey)
    } catch {
        throw "$name will not do: $($_.Exception.Message) $portableTodo"
    }
    $address = "https://download.qkvm.com/desktop/$version/$($zipNames[$kind])"
    if ($fields.platform -cne "windows-$kind-portable") { throw "$name is of $($fields.platform), not of the $kind portable ZIP. $portableTodo" }
    if ($fields.version -cne $version) { throw "$name's feed offers $($fields.version), not $version. $portableTodo" }
    if ($fields.bytes -ne $builtOf[$kind].bytes -or $fields.sha256 -cne $builtOf[$kind].sha256) {
        throw "$name names a ZIP of other bytes: not the $kind ZIP. $portableTodo"
    }
    if ($fields.package -cne $address) { throw "$name names $($fields.package), which is not where it is uploaded ($address). $portableTodo" }
    $said[$name] = "$name is signed by it, current until $($fields.expires), and names $($zipNames[$kind]) ($($fields.bytes) bytes, SHA-256 $($fields.sha256)) at $address"
    $setupFeedName = if ($kind -ceq 'x64') { 'stable-setup.json' } else { 'stable-arm64-setup.json' }
    try {
        $setupPayload = Read-SignedFeed -Feed $feedFiles[$setupFeedName] -PublicKey $publicKey
    } catch {
        throw "$setupFeedName will not do: $($_.Exception.Message) $setupTodo"
    }
    $setupRead = Get-SetupFeedRefusals -Payload $setupPayload -Version $version -Bytes $record.setup.bytes -Sha256 $record.setup.sha256 -Kind $kind
    if ($setupRead.Refusals.Count) { throw "$setupFeedName will not do: $($setupRead.Refusals -join '; '). $setupTodo" }
    if ($setupRead.Fields.package -cne $setupAddress) {
        throw "$setupFeedName names $($setupRead.Fields.package), which is not where it is uploaded ($setupAddress). $setupTodo"
    }
    $said[$setupFeedName] = "$setupFeedName is signed by it, current until $($setupRead.Fields.expires), and names $setupName ($($setupRead.Fields.bytes) bytes, SHA-256 $($setupRead.Fields.sha256)) at $setupAddress"
}

$names = Get-ReleaseFileNames $version -Signed
$wrangler = 'npx --yes wrangler@4.152.0'
$lines = @(
    "# qKVM Desktop $version. Run these by hand, in this order, reading each answer.",
    "# Checked here: both ZIPs and the setup are the ones the run built; their FFmpeg is the part this PC built twice of the same pins and kept in $Records$(if ($ffmpegKept.Published) { ", x64's DLLs byte for byte those $($FFmpegPublished.release)'s release published from the same sources and toolchain" }); the ARM64 ZIP's start checks passed on Windows on Arm; SHA256SUMS names the five files as they are and is signed by the key the program pins; $($said.Values -join '; ').",
    '',
    "# 1. The files, kept for good at https://download.qkvm.com/desktop/$version/ (bucket $Bucket)."
)
foreach ($name in $names) {
    $type = if ($name -like '*.zip') { 'application/zip' } elseif ($name -like '*.exe') { 'application/vnd.microsoft.portable-executable' } elseif ($name -like '*.sig') { 'application/octet-stream' } else { 'text/plain; charset=utf-8' }
    $lines += "$wrangler r2 object put `"$Bucket/desktop/$version/$name`" --file `"$(Join-Path $Files $name)`" --content-type `"$type`" --cache-control `"public, max-age=31536000, immutable`" --remote"
}
$notesArgument = if ($Notes) { '--notes-file "' + (Resolve-Path -LiteralPath $Notes).Path + '"' } else { '--notes "qKVM Desktop ' + $version + ' beta"' }
$assets = ($names | ForEach-Object { '"' + (Join-Path $Files $_) + '"' }) -join ' '
# Where the draft is downloaded to be checked: a folder of its own beside the release files, which must hold the
# draft's files and nothing else (verify_release.ps1), so it must not be there yet.
$draft = Join-Path (Split-Path -Parent $Files) "draft-v$version"
if (Test-Path -LiteralPath $draft) { throw "$draft is there already: the draft is downloaded into a new folder, to hold its files alone." }
# (Its check names it with the direct host in one quoted word, as pwsh -File hands it over and verify_release.ps1
# splits it at its commas: a comma or a quote in the folder's path would make other places of it.)
if ($draft.Contains(',') -or $draft.Contains('"')) { throw "The folder for the draft ($draft) has a comma or a quote in its path: name -Files in another folder." }
$lines += @('',
    '# 2. The GitHub release, as pypv (by its token alone: the active login stays as it is), a draft pre-release until it is checked, of the tag already pushed (--verify-tag: never one GitHub would make).',
    '$env:GH_TOKEN = gh auth token --user pypv',
    "gh release create v$version --repo qkvm/qkvm-desktop --verify-tag --draft --prerelease --title `"qKVM Desktop $version beta`" $notesArgument $assets",
    'Remove-Item Env:GH_TOKEN',
    '',
    '# 3. The draft as GitHub holds it, downloaded as pypv (a draft''s files are not at its public address yet), checked with the files on the direct host before anything is published.',
    '$env:GH_TOKEN = gh auth token --user pypv',
    "gh release download v$version --repo qkvm/qkvm-desktop --dir `"$draft`"",
    'Remove-Item Env:GH_TOKEN',
    "pwsh -File tools\verify_release.ps1 -Run `"$Run`" -From `"$draft,https://download.qkvm.com/desktop/$version`" -RequireSignature",
    '',
    '# 4. The release published, as pypv, once the draft was checked.',
    '$env:GH_TOKEN = gh auth token --user pypv',
    "gh release edit v$version --repo qkvm/qkvm-desktop --verify-tag --tag v$version --draft=false",
    'Remove-Item Env:GH_TOKEN',
    '',
    '# 5. The feeds, last and never cached, so that no copy of the program is told of a release whose page is still a draft: x64 portable copies ask for https://download.qkvm.com/desktop/stable.json, installed copies for https://download.qkvm.com/desktop/stable-setup.json; every copy on Windows on Arm asks for stable-arm64.json or stable-arm64-setup.json beside them.'
)
foreach ($name in $feedFiles.Keys) {
    $lines += "$wrangler r2 object put `"$Bucket/desktop/$name`" --file `"$($feedFiles[$name])`" --content-type `"application/json`" --cache-control `"no-cache`" --remote"
}
$lines += @(
    '',
    '# 6. What is published, against what was built: the release at its public address, the direct host, and the four feeds.',
    "pwsh -File tools\verify_release.ps1 -Run `"$Run`" -From https://github.com/qkvm/qkvm-desktop/releases/download/v$version,https://download.qkvm.com/desktop/$version -Feed https://download.qkvm.com/desktop/stable.json -SetupFeed $SetupUpdateFeed -Arm64Feed $Arm64UpdateFeed -Arm64SetupFeed $Arm64SetupUpdateFeed -RequireSignature"
)
$lines
