#Requires -Version 7.6
<#
.SYNOPSIS
Signs the update feed again with new dates, so that no copy of qKVM says "update information expired". Run by the
owner only, in a console, before the feed's expiry: the program refuses a feed more than 31 days old.
.DESCRIPTION
The feed (stable.json, or a Setup release's stable-setup.json, as assemble_update_feed.ps1 wrote it; each is refreshed
by itself) must be signed by the pinned public key already; its
payload is kept field for field, but published is now and expires is -ValidDays later (30 unless said). The new
payload is signed with the release key, the signature checked against the pinned public key, and the new feed written
to a new file (-Output). Nothing is uploaded.
The passphrase is asked for in this console. It is never taken from the command line, a pipe or the environment, and
never printed or logged.
.EXAMPLE
pwsh -File tools/refresh_feed.ps1 -Feed stable.json -Key E:\qkvm-release-key\qkvm-desktop-release-key.p8 -PublicKey <128 hex> -Output stable-new.json
#>
param(
    [Parameter(Mandatory)][string] $Feed,
    [Parameter(Mandatory)][string] $Key,
    [Parameter(Mandatory)][ValidatePattern('\A[0-9a-fA-F]{128}\z')][string] $PublicKey,
    [Parameter(Mandatory)][string] $Output,
    [ValidateRange(1, 30)][int] $ValidDays = 30
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/release_common.ps1"

Assert-PersonAtConsole
$was = Read-RefreshableFeed -Feed $Feed -PublicKey $PublicKey
$null = Assert-NewPlace $Output
Write-Host "Signing again: qKVM Desktop $($was.version), $($was.platform), $($was.package) (it expires $($was.expires)), for $ValidDays days from now."
$passphrase = Read-ReleasePassphrase
try {
    $now = Invoke-RefreshFeed -Feed $Feed -Key $Key -PublicKey $PublicKey -Output $Output -ValidDays $ValidDays -Passphrase $passphrase
} finally { $passphrase.Dispose() }
Write-Output "Refreshed feed: $([IO.Path]::GetFullPath($Output)), to be served as $(Get-FeedName $was.platform)"
Write-Output "Valid until $($now.expires). Refresh it again before then."
