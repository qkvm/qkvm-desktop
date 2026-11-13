#Requires -Version 7.6
<#
.SYNOPSIS
Signs a release payload or a SHA256SUMS file with qKVM Desktop's release key. Run by the owner only, in a console.
.DESCRIPTION
The file is a release payload (from prepare_update_payload.ps1: current, of qKVM Desktop, a package of its platform's
kind) or a SHA256SUMS file (sha256sum's lines); anything else is refused. The file is read once: what it says and its
SHA-256 are shown before the passphrase is asked for, and those bytes are signed, whatever lies in the file by then;
the SHA-256 of what was signed is said at the end. The signature is ECDSA P-256 over SHA-256 of the exact bytes, 64
bytes r || s (IEEE P1363), written to
a new file (FILE.sig unless -Output names another). It is checked against the pinned public key before it is written,
and the key must be the one that public key belongs to.
The passphrase is asked for in this console. It is never taken from the command line, a pipe or the environment, and
never printed or logged. Then: assemble_update_feed.ps1 makes the feed from a payload and its signature.
.EXAMPLE
pwsh -File tools/sign_release.ps1 -Key E:\qkvm-release-key\qkvm-desktop-release-key.p8 -PublicKey <128 hex> -File payload.json
#>
param(
    [Parameter(Mandatory)][string] $Key,
    [Parameter(Mandatory)][ValidatePattern('\A[0-9a-fA-F]{128}\z')][string] $PublicKey,
    [Parameter(Mandatory)][string] $File,
    [string] $Output = ''
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/release_common.ps1"

Assert-PersonAtConsole
$what = Get-SignableFile $File
$target = Assert-NewPlace $(if ($Output) { $Output } else { "$File.sig" })
Write-Host "Signing $($what.Said)."
Write-Host "SHA-256 of these bytes: $($what.Sha256)"
$passphrase = Read-ReleasePassphrase
try {
    $written = Invoke-SignRelease -Key $Key -PublicKey $PublicKey -What $what -Output $target -Passphrase $passphrase
} finally { $passphrase.Dispose() }
Write-Output "Signature: $written"
Write-Output "SHA-256 of what was signed: $($what.Sha256)"
