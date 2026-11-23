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
A Setup release (-Setup): the package is qKVM's own setup (qKVM-Desktop-<version>-windows-setup.exe, one for both
kinds of Windows: it installs the PC's own), which must say the advertised version in its file version information
(read as data, never run). A copy that this setup installed takes only such a release, from its own feed
(stable-setup.json, beside stable.json), and runs it when the person asks. Each payload names one kind; -Portable and
-Setup are never given together.
-Arch names the kind of Windows the release is for (x64, the default, or arm64): the payload's platform says it
(windows-x64-portable, windows-arm64-setup), and a copy takes only a release for its PC's own kind (from stable.json and
stable-setup.json on x64, stable-arm64.json and stable-arm64-setup.json on Windows on Arm). A portable ZIP's one
qkvm-desktop.exe must be a program of that kind (its PE machine, read as data). The setup is the same file for both, and
-SetupRecord names the record build_setup.ps1 made of it (setup-record.json, in the run's setup folder): it must be the
record of that very setup (its version, size and SHA-256), and say that the setup carries the kind of -Arch (its
architectures), or nothing is prepared: a setup of x64's package alone, as the gate makes one under the same name,
installs nothing on Windows on Arm (review armp-1 L7).
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
    [switch] $TestPackage,
    [ValidateSet('x64', 'arm64')][string] $Arch = 'x64',
    [string] $SetupRecord = ''
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/installer_common.ps1"
. "$PSScriptRoot/release_common.ps1"

if ($Portable -and $Setup) { throw 'A payload names one kind of release: -Portable or -Setup, not both.' }
# The kind of release, said twice over: by the platform the program compares, and by the package's ending.
$ending = if ($Portable) { '.zip' } elseif ($Setup) { '-setup.exe' } else { '.msix' }
# (Letter for letter, small: ValidateSet takes ARM64 as arm64, which the program would refuse.)
$Arch = $Arch.ToLowerInvariant()
$platform = "windows-$Arch-$(if ($Portable) { 'portable' } elseif ($Setup) { 'setup' } else { 'msix' })"
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
    $kind = Get-PortableProgramArchitecture -Zip $packageFile.FullName
    if ($kind -cne $Arch) { throw "The ZIP's qkvm-desktop.exe is a program for $kind Windows, not for $Arch." }
    $said = Get-PortableProgramVersion -Zip $packageFile.FullName -Beside (Split-Path -Parent $destination)
    if ($said -cne $Version) { throw "The ZIP's qkvm-desktop.exe is version '$said', not the advertised $Version." }
} elseif ($Setup) {
    # (The setup's own record first: of this very setup, and carrying the kind the payload is for.)
    if (-not $SetupRecord -or -not (Test-Path -LiteralPath $SetupRecord -PathType Leaf)) {
        throw 'Name the setup''s record with -SetupRecord (setup-record.json, beside the setup in the run''s setup folder): a payload names a setup only of the kind it carries.'
    }
    $made = $null
    try { $made = Get-Content -LiteralPath $SetupRecord -Raw | ConvertFrom-Json } catch { $made = $null }
    if ($null -eq $made -or -not $made.PSObject.Properties['setup'] -or -not $made.PSObject.Properties['architectures'] -or
        [string] $made.setup.sha256 -cne (Get-FileHash -LiteralPath $packageFile.FullName -Algorithm SHA256).Hash.ToLowerInvariant() -or
        [long] $made.setup.bytes -ne $packageFile.Length -or [string] $made.version -cne $Version) {
        throw "$SetupRecord is not the record of this setup of $Version (build_setup.ps1's setup-record.json of it)."
    }
    if (@($made.architectures) -cnotcontains $Arch) {
        throw "The setup carries the program for $(@($made.architectures) -join ' and ') alone, not for ${Arch}: its record says so."
    }
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
    # (Some minutes before now, and so before it is signed: release_common.ps1, $PublishedBeforeMaking.)
    published = ($now - $PublishedBeforeMaking).ToString("yyyy-MM-dd'T'HH:mm:ss'Z'")
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
