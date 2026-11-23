#Requires -Version 7.6
<#
.SYNOPSIS
Signs the update feeds again with new dates, so that no copy of qKVM says "update information expired". Run by the
owner only, in a console, before the feeds' expiry: the program refuses a feed more than 31 days old.
.DESCRIPTION
Each feed (stable.json, stable-setup.json, stable-arm64.json, stable-arm64-setup.json, as assemble_update_feed.ps1
wrote them) must be signed by the pinned public key already; its payload is kept field for field, but published is ten
minutes ago (so that a PC whose clock is a little behind takes it) and expires is -ValidDays from now (30 unless said).
Every feed's name, what it offers, its old expiry, its new dates and the SHA-256 of its new payload are shown first,
then the passphrase is asked for once for all of them (the owner, 2026-11-19); each new payload is signed with the
release key, its signature checked against the pinned public key, and each new feed written to a new file (-Output, in
the order of -Feed, one for each), all of them made before the first is written; where any one feed does not read, or
any one cannot be written, nothing is left. Nothing is uploaded.
The feeds are the set a release serves (release_common.ps1's Get-FeedSetRefusals, review armp-1 L6), or nothing is
signed: the four platforms once each, at one version, and a feed read from a file of a served name holds that name's
own kind; -Partial signs fewer on purpose (0.1.x's two, say), and the ones left out are named before the passphrase is
asked for, as each runs out on its own date. -Bucket names the bucket behind download.qkvm.com: then the upload of
each new feed under the name it is served at, never cached, is said at the end, as release_commands.ps1 says it.
The passphrase is asked for in this console. It is never taken from the command line, a pipe or the environment, and
never printed or logged.
.EXAMPLE
pwsh -File tools/refresh_feed.ps1 -Feed stable.json,stable-setup.json,stable-arm64.json,stable-arm64-setup.json -Key E:\qkvm-release-key\qkvm-desktop-release-key.p8 -PublicKey <128 hex> -Output new\stable.json,new\stable-setup.json,new\stable-arm64.json,new\stable-arm64-setup.json -Bucket <R2 bucket>
#>
param(
    [Parameter(Mandatory)][string[]] $Feed,
    [Parameter(Mandatory)][string] $Key,
    [Parameter(Mandatory)][ValidatePattern('\A[0-9a-fA-F]{128}\z')][string] $PublicKey,
    [Parameter(Mandatory)][string[]] $Output,
    [ValidateRange(1, 30)][int] $ValidDays = 30,
    [switch] $Partial,
    [ValidatePattern('\A([a-z0-9][a-z0-9-]{1,61}[a-z0-9])?\z')][string] $Bucket = ''
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/release_common.ps1"

Assert-PersonAtConsole
$Feed = @($Feed | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$Output = @($Output | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
if ($Feed.Count -lt 1 -or $Feed.Count -ne $Output.Count) { throw 'Name one -Output for each -Feed, in the same order. Nothing was signed.' }
$now = [DateTime]::UtcNow
$what = @(foreach ($one in $Feed) {
    try { Get-RefreshedPayload -Feed $one -PublicKey $PublicKey -ValidDays $ValidDays -Now $now }
    catch { throw "${one}: $($_.Exception.Message) Nothing was signed." }
})
$set = Get-FeedSetRefusals -What $what -Partial:$Partial
if (@($set.Refusals).Count) { throw "These feeds are not the set a release serves: $(@($set.Refusals) -join '; '). Nothing was signed." }
foreach ($place in $Output) { $null = Assert-NewPlace $place }
if (@($set.Missing).Count) { Write-Host "Not signed again now (-Partial): $(@($set.Missing) -join ', '). Each runs out on its own date." }
Write-Host "Signing again $($what.Count) feed$(if ($what.Count -ne 1) { 's' }), with one passphrase, for $ValidDays days from now:"
foreach ($one in $what) {
    Write-Host "  $($one.Name): qKVM Desktop $($one.Fields.version), $($one.Fields.platform), $($one.Fields.package) (it expires $($one.Expired); now valid until $($one.Fields.expires))."
    Write-Host "    SHA-256 of the new payload: $($one.Sha256)"
}
$passphrase = Read-ReleasePassphrase
try {
    $written = Invoke-RefreshFeeds -Key $Key -PublicKey $PublicKey -What $what -Outputs $Output -Passphrase $passphrase
} finally { $passphrase.Dispose() }
for ($at = 0; $at -lt $what.Count; $at++) {
    Write-Output "Refreshed feed: $($written[$at]), to be served as $(Get-FeedName $what[$at].Fields.platform)"
    Write-Output "SHA-256 of the payload signed: $($what[$at].Sha256)"
}
Write-Output "Valid until $($what[0].Fields.expires). Refresh them again before then."
if ($Bucket) {
    Write-Output '# The uploads, each under the name it is served at, never cached: run them by hand, reading each answer.'
    Get-FeedUploadLines -What $what -Written $written -Bucket $Bucket
} else {
    Write-Output 'Upload each new feed under the name it is served at, never cached (-Bucket says the commands).'
}
