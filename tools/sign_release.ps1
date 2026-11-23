#Requires -Version 7.6
<#
.SYNOPSIS
Signs release payloads and a SHA256SUMS file with qKVM Desktop's release key, all with one passphrase. Run by the owner
only, in a console.
.DESCRIPTION
Each file is a release payload (from prepare_update_payload.ps1: current, of qKVM Desktop, a package of its platform's
kind) or a SHA256SUMS file (sha256sum's lines); where any one is anything else, or is named twice, nothing is signed.
Each file is read once: every file's name, what it says and the SHA-256 of its bytes are shown before the passphrase
is asked for, once for all of them (the owner, 2026-11-19), and those bytes are signed, whatever lies in the files by
then; the SHA-256 of what was signed is said of each at the end. Each signature is ECDSA P-256 over SHA-256 of the exact
bytes, 64 bytes r || s (IEEE P1363), checked against the pinned public key; all are made and checked before the first
is written, each to a new file (FILE.sig; -Output names another, for one file alone), and where any one cannot be
written none is left. The key must be the one that public key belongs to.
The passphrase is asked for in this console. It is never taken from the command line, a pipe or the environment, and
never printed or logged. Then: assemble_update_feed.ps1 makes each feed from a payload and its signature.
.EXAMPLE
pwsh -File tools/sign_release.ps1 -Key E:\qkvm-release-key\qkvm-desktop-release-key.p8 -PublicKey <128 hex> -File SHA256SUMS,payload-portable.json,payload-setup.json
#>
param(
    [Parameter(Mandatory)][string] $Key,
    [Parameter(Mandatory)][ValidatePattern('\A[0-9a-fA-F]{128}\z')][string] $PublicKey,
    [Parameter(Mandatory)][string[]] $File,
    [string] $Output = ''
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/release_common.ps1"

Assert-PersonAtConsole
$File = @($File | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
if ($Output -and $File.Count -ne 1) { throw '-Output names the signature of one file alone; several are signed to FILE.sig each. Nothing was signed.' }
$what = Get-SignableFiles $File
$outputs = @(if ($Output) { $Output } else { $what | ForEach-Object { "$($_.File).sig" } })
foreach ($place in $outputs) { $null = Assert-NewPlace $place }
Write-Host "Signing $($what.Count) file$(if ($what.Count -ne 1) { 's' }), with one passphrase:"
foreach ($one in $what) {
    Write-Host "  $($one.Name): $($one.Said)."
    Write-Host "    SHA-256 of these bytes: $($one.Sha256)"
}
$passphrase = Read-ReleasePassphrase
try {
    $written = Invoke-SignReleaseFiles -Key $Key -PublicKey $PublicKey -What $what -Outputs $outputs -Passphrase $passphrase
} finally { $passphrase.Dispose() }
for ($at = 0; $at -lt $what.Count; $at++) {
    Write-Output "Signature: $($written[$at])"
    Write-Output "SHA-256 of what was signed: $($what[$at].Sha256)"
}
