#Requires -Version 7.2
<#
.SYNOPSIS
Verifies an offline release signature using only its public key and assembles the update feed.
.DESCRIPTION
No private keys, publication, certificate installation, package execution or network requests.
The payload must be a current release payload as prepare_update_payload.ps1 writes it. The output must be served
directly at the HTTPS feed address its kind is read from: stable.json, the one compiled into the desktop app, for a
portable (or MSIX) release; stable-setup.json, beside it, for a Setup release. The command says which.
#>
param(
    [Parameter(Mandatory)][string] $Payload,
    [Parameter(Mandatory)][string] $Signature,
    [Parameter(Mandatory)][ValidatePattern('\A[0-9a-fA-F]{128}\z')][string] $PublicKey,
    [Parameter(Mandatory)][string] $Output
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/release_common.ps1"

$payloadFile = Get-Item -LiteralPath $Payload
$signatureFile = Get-Item -LiteralPath $Signature
if ($payloadFile.PSIsContainer -or $payloadFile.Length -lt 1 -or $payloadFile.Length -gt 70KB -or
    $signatureFile.PSIsContainer -or $signatureFile.Length -ne 64) {
    throw 'Expected a bounded payload and a 64-byte P1363 signature.'
}
$bytes = [IO.File]::ReadAllBytes($payloadFile.FullName)
$fields = Test-UpdatePayload -Bytes $bytes
$signatureBytes = [IO.File]::ReadAllBytes($signatureFile.FullName)
$publicBytes = [Convert]::FromHexString($PublicKey)
$point = [Security.Cryptography.ECPoint]::new()
$point.X = $publicBytes[0..31]
$point.Y = $publicBytes[32..63]
$parameters = [Security.Cryptography.ECParameters]::new()
$parameters.Curve = [Security.Cryptography.ECCurve+NamedCurves]::nistP256
$parameters.Q = $point
$verifier = [Security.Cryptography.ECDsa]::Create()
try {
    $verifier.ImportParameters($parameters)
    if (-not $verifier.VerifyData($bytes, $signatureBytes, [Security.Cryptography.HashAlgorithmName]::SHA256,
            [Security.Cryptography.DSASignatureFormat]::IeeeP1363FixedFieldConcatenation)) {
        throw 'The release signature does not match this payload and pinned public key.'
    }
} finally { $verifier.Dispose() }
$envelope = [ordered]@{
    payload = [Convert]::ToBase64String($bytes)
    signature = [Convert]::ToBase64String($signatureBytes)
} | ConvertTo-Json -Compress
$encoded = [Text.UTF8Encoding]::new($false).GetBytes($envelope)
if ($encoded.Length -gt 96KB) { throw 'The signed update feed exceeds the desktop size limit.' }
$destination = [IO.Path]::GetFullPath($Output)
$stream = [IO.File]::Open($destination, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
try { $stream.Write($encoded, 0, $encoded.Length) } finally { $stream.Dispose() }
Write-Output "Verified and assembled public update feed: $destination"
Write-Output "Serve it as $(Get-FeedName $fields.platform) ($($fields.platform), qKVM Desktop $($fields.version))."
