#Requires -Version 7.2
<# Run after offline signing, before producing a public feed. Never installs a package. #>
param([Parameter(Mandatory)][string] $SetupDirectory)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/installer_common.ps1"
$setup = (Resolve-Path -LiteralPath $SetupDirectory).Path
$publisher = ''
foreach ($name in @('qkvm.msix', 'qkvm-setup.exe')) {
    $signature = Get-AuthenticodeSignature -LiteralPath (Join-Path $setup $name)
    if ($signature.Status -ne 'Valid') { throw "$name has no valid Windows signature." }
    if ($publisher -and $publisher -cne $signature.SignerCertificate.Subject) { throw 'Installer publisher mismatch.' }
    $publisher = $signature.SignerCertificate.Subject
}
$helper = Join-Path $setup 'qkvm-setup.exe'
$package = Join-Path $setup 'qkvm.msix'
Invoke-InstallerCheck -Helper $helper -Mode '--verify' -Package $package
$auditContent = Join-Path (Split-Path -Parent $setup) "release-audit-$([guid]::NewGuid().ToString('N'))"
[IO.Compression.ZipFile]::ExtractToDirectory($package, $auditContent)
& "$PSScriptRoot/audit_release.ps1" -PackageDirectory $auditContent -OutputFile "$auditContent.json" -MsixPayload -RequireRelease
$hash = (Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash.ToLowerInvariant()
"$hash  qkvm.msix" | Set-Content -LiteralPath "$setup/qkvm.msix.sha256" -Encoding ascii
@'
Signed qKVM Setup. Windows signature and package identity checks passed.

Open qkvm-setup.exe to install. Setup always asks first; quit qKVM from its tray
before continuing. Open qKVM from the Start menu after installation finishes.
Keep this folder: Previous version and Recover installation can be used even
when qKVM cannot start. Your saved machines and settings stay in place.
'@ | Set-Content -LiteralPath "$setup/START-HERE.txt" -Encoding utf8
$archive = Join-Path (Split-Path -Parent $setup) "qKVM-Setup-$($hash.Substring(0, 12)).zip"
[IO.Compression.ZipFile]::CreateFromDirectory($setup, $archive)
[ordered]@{ folder = $setup; package = $package; sha256 = $hash; publisher = $publisher;
    archive = $archive; archiveSha256 = (Get-FileHash -LiteralPath $archive).Hash.ToLowerInvariant(); signed = $true } |
    ConvertTo-Json | Set-Content -LiteralPath "$archive.verified.json" -Encoding utf8
Write-Output 'Windows signatures, product identity, architecture and profile compatibility passed.'
Write-Output "Verified installer SHA-256: $hash"
