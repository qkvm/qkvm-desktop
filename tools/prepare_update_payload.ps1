#Requires -Version 7.2
<#
.SYNOPSIS
Prepares the exact public release bytes for offline signing. Does not sign, upload or install.
.DESCRIPTION
An MSIX release: the package and helper must already be signed by the release process. This command verifies
Windows trust, the configured product and publisher, and the advertised version.
A portable release (-Portable): the package is the ZIP that package.ps1 made. Its one qkvm-desktop.exe must say the
advertised version (read from a copy of that one file, never run). A portable copy of qKVM takes only such a release,
and shows the verified ZIP to the person; an installed (MSIX) copy takes only an MSIX.
A Setup release (-Setup): the package is qKVM's own setup (qKVM-Desktop-<version>-windows-x64-setup.exe), which must
say the advertised version in its file version information (read as data, never run). A copy that this setup
installed takes only such a release, from its own feed (stable-setup.json, beside stable.json), and runs it when the
person asks. Each payload names one kind; -Portable and -Setup are never given together.
Sign the output bytes with sign_release.ps1 (the offline P-256 release key, run by the owner) and pass the 64-byte
P1363 signature to assemble_update_feed.ps1. Never give this command a private key.
#>
param(
    [Parameter(Mandatory)][string] $Package,
    [Parameter(Mandatory)][ValidatePattern('\A(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})\z')][string] $Version,
    [Parameter(Mandatory)][uri] $PackageUrl,
    [Parameter(Mandatory)][string] $Notes,
    [Parameter(Mandatory)][string] $Output,
    [ValidateRange(1, 30)][int] $ValidDays = 7,
    [string] $InstallerHelper = '',
    [switch] $Portable,
    [switch] $Setup,
    [switch] $TestPackage
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/installer_common.ps1"
. "$PSScriptRoot/release_common.ps1"

if ($Portable -and $Setup) { throw 'A payload names one kind of release: -Portable or -Setup, not both.' }
# The kind of release, said twice over: by the platform the program compares, and by the package's ending.
$ending = if ($Portable) { '.zip' } elseif ($Setup) { '-setup.exe' } else { '.msix' }
$platform = if ($Portable) { 'windows-x64-portable' } elseif ($Setup) { 'windows-x64-setup' } else { 'windows-x64-msix' }
if (@($Version.Split('.') | Where-Object { [int]$_ -gt 65535 }).Count -gt 0) {
    throw 'Version components must be no greater than 65535.'
}
if (-not $PackageUrl.IsAbsoluteUri -or $PackageUrl.Scheme -cne 'https' -or
    $PackageUrl.UserInfo -or $PackageUrl.Query -or $PackageUrl.Fragment -or
    -not $PackageUrl.AbsolutePath.EndsWith($ending, [StringComparison]::OrdinalIgnoreCase) -or
    $PackageUrl.AbsoluteUri.Length -gt 2048) {
    throw "Use a direct HTTPS URL of the $ending package with no credentials, query or fragment."
}
if ($Notes.Length -gt 16000) { throw 'Release notes are limited to 16000 characters.' }
$packageFile = Get-Item -LiteralPath $Package
if ($packageFile.PSIsContainer -or -not $packageFile.Name.EndsWith($ending, [StringComparison]::OrdinalIgnoreCase) -or
    $packageFile.Length -lt 1 -or $packageFile.Length -gt 1GB) {
    throw "The package must be a nonempty $ending file no larger than 1 GiB."
}
$destination = [IO.Path]::GetFullPath($Output)
if ($TestPackage) {
    if (-not $PackageUrl.Host.EndsWith('.example.test', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Test packages are restricted to example.test subdomains.'
    }
} elseif ($Portable) {
    $said = Get-PortableProgramVersion -Zip $packageFile.FullName -Beside (Split-Path -Parent $destination)
    if ($said -cne $Version) { throw "The ZIP's qkvm-desktop.exe is version '$said', not the advertised $Version." }
} elseif ($Setup) {
    $said = Get-SetupVersion -Setup $packageFile.FullName
    if ($said -cne $Version -and $said -cne "$Version.0") { throw "The setup says version '$said', not the advertised $Version." }
} else {
    if (-not $InstallerHelper) { throw 'Supply the signed qKVM setup helper to verify the production package.' }
    foreach ($file in @($packageFile.FullName, $InstallerHelper)) {
        if ((Get-AuthenticodeSignature -LiteralPath $file).Status -ne 'Valid') { throw 'Release packages and their helper must have valid Windows signatures.' }
    }
    $checked = Invoke-InstallerCheck -Helper $InstallerHelper -Mode '--verify' -Package $packageFile.FullName
    if ($checked -cne "QKVM_INSTALLER_PACKAGE_OK $Version") { throw 'The release version does not match the verified installer.' }
}
$now = [DateTime]::UtcNow
$payload = [ordered]@{
    schema = 1
    product = 'qkvm-desktop'
    platform = $platform
    channel = 'stable'
    version = $Version
    published = $now.ToString("yyyy-MM-dd'T'HH:mm:ss'Z'")
    expires = $now.AddDays($ValidDays).ToString("yyyy-MM-dd'T'HH:mm:ss'Z'")
    package = $PackageUrl.AbsoluteUri
    bytes = $packageFile.Length
    sha256 = (Get-FileHash -LiteralPath $packageFile.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    notes = $Notes
}
$bytes = [Text.UTF8Encoding]::new($false).GetBytes(($payload | ConvertTo-Json -Compress))
$stream = [IO.File]::Open($destination, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
try { $stream.Write($bytes, 0, $bytes.Length) } finally { $stream.Dispose() }
Write-Output "Prepared unsigned $platform release payload: $destination"
Write-Output 'Sign these exact bytes with sign_release.ps1 (ECDSA P-256 / SHA-256, a 64-byte r || s signature).'
