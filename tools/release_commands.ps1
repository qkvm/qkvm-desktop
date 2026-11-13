#Requires -Version 7.2
<#
.SYNOPSIS
Says the commands that publish a release, for the owner to run by hand. Runs none of them.

.DESCRIPTION
    pwsh -File tools\release_commands.ps1 -Run <Root>\runs\<time> -Files <release files> -Bucket <R2 bucket>
        -Feed <stable.json> -SetupFeed <stable-setup.json> [-Notes <release notes file>]

For the release files of one run (tools/release_files.ps1), once the owner has signed SHA256SUMS
(tools/sign_release.ps1), signed the payloads and assembled stable.json (the portable copies' feed) and
stable-setup.json (installed copies', prepare_update_payload.ps1 -Setup). First it checks, against the run's build
record and the key the program pins: the ZIP and the setup are the ones built (the setup goes up as a Windows
program, application/vnd.microsoft.portable-executable); SHA256SUMS names exactly the release files, as
they are, and SHA256SUMS.sig is its signature; stable.json is signed, current, of the portable ZIP, and names
this version and this ZIP by size and SHA-256 at the address it is uploaded to; stable-setup.json is signed,
current, of the setup, and names this version and this setup by size and SHA-256 at the address it is uploaded
to (build_common.ps1's Get-SetupFeedRefusals). Only then does it say, in this order: the files' uploads to the
bucket behind download.qkvm.com (owner, D7; Cloudflare R2 by wrangler, at the version the sites are deployed
with); the GitHub release as a draft pre-release of the tag that is there, made as pypv (D4; release plan B11);
the draft downloaded as pypv into a new folder beside the release files (draft-v<version>; a draft's files are
not at its public address) and checked with the direct host's (tools/verify_release.ps1); the release published;
then the two feeds, so that no copy of the program is told of a release whose page is still a draft; and the
check of everything as published, both feeds with it. Each versioned file is kept for good under desktop/<version>/ and
may be cached for a year; the feeds are never cached. Nothing is uploaded, signed or published here, and
nothing is asked of any service.
#>
param(
    [Parameter(Mandatory)] [string] $Run,
    [Parameter(Mandatory)] [string] $Files,
    [Parameter(Mandatory)] [ValidatePattern('\A[a-z0-9][a-z0-9-]{1,61}[a-z0-9]\z')] [string] $Bucket,
    [string] $Feed = '',
    [string] $SetupFeed = '',
    [string] $Notes = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\build_common.ps1"
. "$PSScriptRoot\release_common.ps1"

$Run = (Resolve-Path -LiteralPath $Run).Path
$Files = (Resolve-Path -LiteralPath $Files).Path
$record = Get-Content -LiteralPath (Join-Path $Run 'build-record.json') -Raw | ConvertFrom-Json
$version = [string] $record.version
if ($record.trial) { throw 'This run is a trial: nothing of it is published.' }
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

# What is uploaded is checked first, against the run and the key the program pins: a versioned file is kept for
# good under its name, and the feed is what every copy of the program takes.
$publicKey = [string] $record.updates.publicKey
if (-not (Test-ReleasePublicKey $publicKey)) { throw 'The build record names no key the program pins.' }
$zipName = "qKVM-Desktop-$version-windows-x64.zip"
$zip = Join-Path $Files $zipName
if ((Get-Item -LiteralPath $zip).Length -ne $record.package.bytes -or (Get-Sha256 $zip) -cne $record.package.sha256) {
    throw "$zipName is not the ZIP the run built."
}
$setupName = Get-SetupName $version
if (-not $record.PSObject.Properties['setup'] -or -not $record.setup) { throw 'The run made no setup: its build record names none.' }
$setup = Join-Path $Files $setupName
if ((Get-Item -LiteralPath $setup).Length -ne $record.setup.bytes -or (Get-Sha256 $setup) -cne $record.setup.sha256) {
    throw "$setupName is not the setup the run built."
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
try {
    $fields = Test-UpdatePayload -Bytes (Read-SignedFeed -Feed $feedPath -PublicKey $publicKey)
} catch {
    throw "stable.json will not do: $($_.Exception.Message)"
}
$address = "https://download.qkvm.com/desktop/$version/$zipName"
if ($fields.platform -cne 'windows-x64-portable') { throw "stable.json is of $($fields.platform), not of the portable ZIP." }
if ($fields.version -cne $version) { throw "stable.json's feed offers $($fields.version), not $version." }
if ($fields.bytes -ne $record.package.bytes -or $fields.sha256 -cne $record.package.sha256) {
    throw 'stable.json names a ZIP of other bytes: not this ZIP.'
}
if ($fields.package -cne $address) { throw "stable.json names $($fields.package), which is not where it is uploaded ($address)." }
# Installed copies' feed: signed by the same key, of this setup, where it is uploaded.
try {
    $setupPayload = Read-SignedFeed -Feed $setupFeedPath -PublicKey $publicKey
} catch {
    throw "stable-setup.json will not do: $($_.Exception.Message)"
}
$setupRead = Get-SetupFeedRefusals -Payload $setupPayload -Version $version -Bytes $record.setup.bytes -Sha256 $record.setup.sha256
if ($setupRead.Refusals.Count) { throw "stable-setup.json will not do: $($setupRead.Refusals -join '; ')." }
$setupAddress = "https://download.qkvm.com/desktop/$version/$setupName"
if ($setupRead.Fields.package -cne $setupAddress) {
    throw "stable-setup.json names $($setupRead.Fields.package), which is not where it is uploaded ($setupAddress)."
}

$names = Get-ReleaseFileNames $version -Signed
$wrangler = 'npx --yes wrangler@4.152.0'
$lines = @(
    "# qKVM Desktop $version. Run these by hand, in this order, reading each answer.",
    "# Checked here: the ZIP and the setup are the ones the run built; SHA256SUMS names the four files as they are and is signed by the key the program pins; stable.json is signed by it, current until $($fields.expires), and names $zipName ($($fields.bytes) bytes, SHA-256 $($fields.sha256)) at $address; stable-setup.json is signed by it, current until $($setupRead.Fields.expires), and names $setupName ($($setupRead.Fields.bytes) bytes, SHA-256 $($setupRead.Fields.sha256)) at $setupAddress.",
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
    '# 5. The feeds, last and never cached, so that no copy of the program is told of a release whose page is still a draft: portable copies ask for https://download.qkvm.com/desktop/stable.json, installed copies for https://download.qkvm.com/desktop/stable-setup.json.',
    "$wrangler r2 object put `"$Bucket/desktop/stable.json`" --file `"$feedPath`" --content-type `"application/json`" --cache-control `"no-cache`" --remote",
    "$wrangler r2 object put `"$Bucket/desktop/stable-setup.json`" --file `"$setupFeedPath`" --content-type `"application/json`" --cache-control `"no-cache`" --remote",
    '',
    '# 6. What is published, against what was built: the release at its public address, the direct host, and both feeds.',
    "pwsh -File tools\verify_release.ps1 -Run `"$Run`" -From https://github.com/qkvm/qkvm-desktop/releases/download/v$version,https://download.qkvm.com/desktop/$version -Feed https://download.qkvm.com/desktop/stable.json -SetupFeed $SetupUpdateFeed -RequireSignature"
)
$lines
