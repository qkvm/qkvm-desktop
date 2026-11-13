#Requires -Version 7.6
<#
.SYNOPSIS
Makes qKVM Desktop's release key. Run by the owner only, once, in a console window.
.DESCRIPTION
A new ECDSA P-256 key, written as an encrypted PKCS#8 file (PEM: PBES2, AES-256-CBC, its key made from the passphrase
by PBKDF2-HMAC-SHA256 with 600000 rounds) named qkvm-desktop-release-key.p8, into the folder named here, which must
not be inside a git repository. Nothing else is written, and nothing is sent anywhere.
The passphrase is asked for twice, in this console. It is never taken from the command line, a pipe or the
environment, and never printed or logged.
The public key is printed as the 128 hexadecimal characters the program pins (configure the release build with
-DQKVM_UPDATE_PUBLIC_KEY=...), with its SHA-256 fingerprint for the release record.
Keep two copies of the key file off this PC, and the passphrase apart from them. Without the key, or its passphrase,
no copy of qKVM takes another update: each would need its next version by hand.
.EXAMPLE
pwsh -File tools/release_key.ps1 -Directory E:\qkvm-release-key
#>
param([Parameter(Mandatory)][string] $Directory)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/release_common.ps1"

Assert-PersonAtConsole
$file = Assert-KeyPlace $Directory
Write-Host "A new release key will be written to $file"
Write-Host "Choose a passphrase of at least $ReleaseKeyShortest characters (a few words). It is asked for twice."
$passphrase = Read-ReleasePassphrase -New
try { $made = New-ReleaseKey -Directory $Directory -Passphrase $passphrase } finally { $passphrase.Dispose() }
Write-Output "Release key: $($made.File)"
Write-Output "Public key (QKVM_UPDATE_PUBLIC_KEY): $($made.PublicKey)"
Write-Output "Fingerprint (SHA-256 of the public key): $($made.Fingerprint)"
Write-Output 'Next: two copies of the key file off this PC, the passphrase kept apart from them, and this public key in the release build.'
