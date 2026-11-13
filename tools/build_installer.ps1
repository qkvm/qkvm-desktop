#Requires -Version 7.2
<# Builds and validates an MSIX and a setup folder. Never signs, installs or changes certificate trust.
Production inputs must already be signed by the configured public publisher. The private signing
key is never an input. -Development produces an explicitly unsigned, non-installable trial. #>
param(
    [Parameter(Mandatory)][string] $PackageDirectory,
    [switch] $Development
)
. "$PSScriptRoot/common.ps1"
. "$PSScriptRoot/installer_common.ps1"
Enter-BuildEnvironment
$package = (Resolve-Path -LiteralPath $PackageDirectory).Path
& "$PSScriptRoot/test_package.ps1" -PackageDirectory $package
$cache = Get-Content -LiteralPath "$DesktopRoot/build/release/CMakeCache.txt"
function Cache-Value([string] $Name) {
    $line = @($cache | Where-Object { $_.StartsWith("${Name}:STRING=") })
    if ($line.Count -ne 1) { throw "Missing installer build setting: $Name" }
    return ($line[0] -split '=', 2)[1]
}
$identity = Cache-Value 'QKVM_PACKAGE_NAME'
$publisher = Cache-Value 'QKVM_PACKAGE_PUBLISHER'
if ($Development) {
    if ($publisher) { throw 'Build the development trial with no production publisher configured.' }
    $identity = 'qKVM.Desktop.Development'
    $publisher = 'CN=qKVM Development'
} else {
    if (-not $publisher) { throw 'Configure the public Windows signing publisher before a production installer build.' }
    foreach ($program in @('qkvm-desktop.exe', 'qkvm-setup.exe')) {
        $signature = Get-AuthenticodeSignature -LiteralPath (Join-Path $package $program)
        if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -cne $publisher) {
            throw "$program must first be signed by the configured Windows publisher."
        }
    }
    if (-not (Cache-Value 'QKVM_UPDATE_FEED') -or -not (Cache-Value 'QKVM_UPDATE_PUBLIC_KEY')) {
        throw 'A production build needs its HTTPS update address and public verification key.'
    }
}
$version = (Get-Item -LiteralPath "$package/qkvm-desktop.exe").VersionInfo.ProductVersion
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'Invalid application version.' }
$place = Join-Path $DesktopRoot "build/installers/qKVM-$version-$(Get-Date -Format 'yyyyMMdd-HHmmss-fff')"
if (Test-Path -LiteralPath $place) { throw 'Installer output already exists.' }
$content = (New-Item -ItemType Directory -Path "$place/content").FullName
$setup = (New-Item -ItemType Directory -Path "$place/Setup").FullName
Copy-Item -Path "$package/*" -Destination $content -Recurse
# The portable ZIP carries no setup (owner, D1); an MSIX does, as the release build made it.
if (-not (Test-Path -LiteralPath "$content/qkvm-setup.exe")) {
    $built = "$DesktopRoot/build/release/bin/qkvm-setup.exe"
    if (-not (Test-Path -LiteralPath $built)) { throw 'The release build has no native setup helper.' }
    Copy-Item -LiteralPath $built -Destination $content
}
[ordered]@{ profileSchema = 1; version = $version } | ConvertTo-Json |
    Set-Content -LiteralPath "$content/qkvm-release.json" -Encoding utf8
# Use the existing product icon for the required Windows Start-menu images.
Add-Type -AssemblyName System.Drawing
$assets = (New-Item -ItemType Directory -Path "$content/Assets").FullName
$icon = [Drawing.Icon]::new("$DesktopRoot/ui/brand/qkvm.ico", 256, 256)
try {
    $source = $icon.ToBitmap()
    try {
        foreach ($size in @(44, 50, 150)) {
            $bitmap = [Drawing.Bitmap]::new($size, $size)
            $graphics = [Drawing.Graphics]::FromImage($bitmap)
            try {
                $graphics.Clear([Drawing.Color]::Transparent)
                $graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
                $graphics.DrawImage($source, 0, 0, $size, $size)
                $bitmap.Save("$assets/Logo$size.png", [Drawing.Imaging.ImageFormat]::Png)
            } finally { $graphics.Dispose(); $bitmap.Dispose() }
        }
    } finally { $source.Dispose() }
} finally { $icon.Dispose() }
$publisherXml = [Security.SecurityElement]::Escape($publisher)
$nameXml = [Security.SecurityElement]::Escape($identity)
$display = if ($Development) { 'qKVM Development' } else { 'qKVM' }
@"
<?xml version="1.0" encoding="utf-8"?>
<Package xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10"
 xmlns:uap="http://schemas.microsoft.com/appx/manifest/uap/windows10"
 xmlns:uap5="http://schemas.microsoft.com/appx/manifest/uap/windows10/5"
 xmlns:desktop6="http://schemas.microsoft.com/appx/manifest/desktop/windows10/6"
 xmlns:rescap="http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities"
 IgnorableNamespaces="uap uap5 desktop6 rescap">
 <Identity Name="$nameXml" Publisher="$publisherXml" Version="$version.0" ProcessorArchitecture="x64" />
 <Properties>
  <DisplayName>$display</DisplayName><PublisherDisplayName>qKVM</PublisherDisplayName><Logo>Assets\Logo50.png</Logo>
  <desktop6:RegistryWriteVirtualization>disabled</desktop6:RegistryWriteVirtualization>
  <desktop6:FileSystemWriteVirtualization>disabled</desktop6:FileSystemWriteVirtualization>
 </Properties>
 <Dependencies><TargetDeviceFamily Name="Windows.Desktop" MinVersion="10.0.19045.0" MaxVersionTested="10.0.26100.0" /></Dependencies>
 <Resources><Resource Language="en-us" /></Resources>
 <Applications>
  <Application Id="App" Executable="qkvm-desktop.exe" EntryPoint="Windows.FullTrustApplication">
   <uap:VisualElements DisplayName="$display" Description="KVM desktop console" BackgroundColor="transparent"
    Square44x44Logo="Assets\Logo44.png" Square150x150Logo="Assets\Logo150.png" />
   <Extensions>
    <uap5:Extension Category="windows.startupTask" Executable="qkvm-desktop.exe" EntryPoint="Windows.FullTrustApplication">
     <uap5:StartupTask TaskId="qKVMStartup" Enabled="false" DisplayName="$display" />
    </uap5:Extension>
   </Extensions>
  </Application>
 </Applications>
 <Capabilities><rescap:Capability Name="runFullTrust" /><rescap:Capability Name="unvirtualizedResources" /></Capabilities>
</Package>
"@ | Set-Content -LiteralPath "$content/AppxManifest.xml" -Encoding utf8
# Signing the application changes its bytes. Record the final payload again, including the
# compatibility record and icons, after copying signed inputs. Windows owns its XML metadata.
if (-not $Development) {
    'qKVM installed application. Use the Start menu to open qKVM and Settings > Updates for updates. See THIRD-PARTY.md and licenses/ for bundled library notices and sources.' |
        Set-Content -LiteralPath "$content/START-HERE.txt" -Encoding utf8
}
$files = @(Get-ChildItem -LiteralPath $content -Recurse -File | Where-Object { $_.Name -notin @('package-manifest.json', 'AppxManifest.xml') } | Sort-Object FullName | ForEach-Object {
    [ordered]@{ path = [IO.Path]::GetRelativePath($content, $_.FullName).Replace('\', '/'); bytes = $_.Length;
        sha256 = (Get-FileHash -LiteralPath $_.FullName).Hash.ToLowerInvariant() }
})
[ordered]@{ product = 'qKVM'; version = $version; architecture = 'x64'; files = $files } |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$content/package-manifest.json" -Encoding utf8
$sdk = Join-Path $env:WindowsSdkDir "bin/$($env:WindowsSDKVersion.TrimEnd('\'))/x64"
$msix = "$setup/qkvm.msix"
& "$sdk/makeappx.exe" pack /d $content /p $msix /no
if ($LASTEXITCODE -ne 0) { throw 'Windows rejected the installer package.' }
Copy-Item -LiteralPath "$content/qkvm-setup.exe" -Destination $setup
# The helper inspects the actual MSIX through Windows' package reader. No deployment occurs.
Invoke-InstallerCheck -Helper "$setup/qkvm-setup.exe" -Mode '--inspect' -Package $msix
@'
@echo off
"%~dp0qkvm-setup.exe" --rollback
'@ | Set-Content -LiteralPath "$setup/Previous version.cmd" -Encoding ascii
@'
@echo off
"%~dp0qkvm-setup.exe" --recover
'@ | Set-Content -LiteralPath "$setup/Recover installation.cmd" -Encoding ascii
$status = if ($Development) { 'UNSIGNED DEVELOPMENT TRIAL: it cannot be installed. No certificate was trusted.' } else { 'AWAITING OFFLINE MSIX SIGNING: do not distribute until release verification passes.' }
@"
$status

After release signing, open qkvm-setup.exe to install. Setup always asks first.
Keep this folder: Previous version and Recover installation also work if qKVM will not start.
Settings, saved machines and Windows credentials are retained. No administrator is requested.
Setup verifies the Windows signature, retains the previous installer, journals each change,
and checks startup. A failed startup triggers restoration when a compatible prior version exists.
No installer, certificate, system setting, or existing user profile was changed by this build.
"@ | Set-Content -LiteralPath "$setup/START-HERE.txt" -Encoding utf8
$hash = (Get-FileHash -LiteralPath $msix -Algorithm SHA256).Hash.ToLowerInvariant()
$archive = ''
if ($Development) {
    $archive = "$place/Setup-UNSIGNED-DEVELOPMENT.zip"
    [IO.Compression.ZipFile]::CreateFromDirectory($setup, $archive)
}
[ordered]@{ folder = $setup; package = $msix; sha256 = $hash; version = $version;
    archive = $archive; identity = $identity; publisher = $publisher; development = [bool]$Development; signed = $false } |
    ConvertTo-Json | Set-Content -LiteralPath "$DesktopRoot/build/installers/latest.json" -Encoding utf8
Write-Output "Installer prepared: $setup" "Package SHA-256: $hash" $status
