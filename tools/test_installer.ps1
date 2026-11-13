#Requires -Version 7.2
<# Exercises a real unsigned MSIX without registering it or changing certificate trust. #>
param([Parameter(Mandatory)][string] $SetupDirectory)
. "$PSScriptRoot/common.ps1"
. "$PSScriptRoot/installer_common.ps1"
Enter-BuildEnvironment
$setup = (Resolve-Path -LiteralPath $SetupDirectory).Path
$helper = Join-Path $setup 'qkvm-setup.exe'
$package = Join-Path $setup 'qkvm.msix'
$root = (New-Item -ItemType Directory -Path "$DesktopRoot/build/installer-checks/$([guid]::NewGuid().ToString('N'))").FullName
$content = Join-Path $root 'extracted'
[IO.Compression.ZipFile]::ExtractToDirectory($package, $content)
$manifest = Get-Content -LiteralPath "$content/package-manifest.json" -Raw | ConvertFrom-Json
$prefix = $content + [IO.Path]::DirectorySeparatorChar
$seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($entry in $manifest.files) {
    $file = [IO.Path]::GetFullPath((Join-Path $content $entry.path))
    if (-not $file.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -or -not $seen.Add($file)) {
        throw 'An installer payload path is invalid or repeated.'
    }
    if (-not (Test-Path -LiteralPath $file -PathType Leaf) -or
        (Get-Item -LiteralPath $file).Length -ne $entry.bytes -or
        (Get-FileHash -LiteralPath $file).Hash -ne $entry.sha256) {
        throw "Installer payload mismatch: $($entry.path)"
    }
}
if ((Get-FileHash -LiteralPath $helper).Hash -ne (Get-FileHash -LiteralPath "$content/qkvm-setup.exe").Hash) {
    throw 'The external and packaged setup helpers differ.'
}
Write-Output "Installer payload: $($seen.Count) file hashes matched."
& "$PSScriptRoot/test_package.ps1" -PackageDirectory $content -WithSetup
Invoke-InstallerCheck -Helper $helper -Mode '--inspect' -Package $package
$xml = [xml](Get-Content -LiteralPath "$content/AppxManifest.xml" -Raw)
$startup = $xml.SelectSingleNode("//*[local-name()='StartupTask']")
if ($startup.TaskId -cne 'qKVMStartup' -or $startup.Enabled -cne 'false') { throw 'Startup must be opt-in and owned by Windows.' }
foreach ($setting in @('RegistryWriteVirtualization', 'FileSystemWriteVirtualization')) {
    if ($xml.SelectSingleNode("//*[local-name()='$setting']").InnerText -cne 'disabled') { throw 'The package must preserve the existing profile location.' }
}
function Expect-InstallerRefusal([string] $Mode, [string] $File, [string] $Message) {
    try { Invoke-InstallerCheck -Helper $helper -Mode $Mode -Package $File | Out-Null }
    catch {
        if ($_.Exception.Message -notmatch $Message) { throw }
        Write-Output "Expected installer rejection: $($_.Exception.Message)"
        return
    }
    throw 'An invalid installer was accepted.'
}
if ((Get-AuthenticodeSignature -LiteralPath $package).Status -eq 'NotSigned') {
    Expect-InstallerRefusal '--verify' $package 'Windows does not trust this installer signature'
}
# These are disposable repacked fixtures. Windows' package reader, not a mock, rejects them.
$sdk = Join-Path $env:WindowsSdkDir "bin/$($env:WindowsSDKVersion.TrimEnd('\'))/x64/makeappx.exe"
# MakeAppx regenerates these metadata files; remove only those exact extracted files.
foreach ($name in @('AppxBlockMap.xml', '[Content_Types].xml', 'AppxSignature.p7x')) {
    $file = Join-Path $content $name
    if (Test-Path -LiteralPath $file) { Remove-Item -LiteralPath $file }
}
$release = Get-Content -LiteralPath "$content/qkvm-release.json" -Raw | ConvertFrom-Json
$release.profileSchema = 2
$release | ConvertTo-Json | Set-Content -LiteralPath "$content/qkvm-release.json" -Encoding utf8
$incompatible = Join-Path $root 'incompatible.msix'
& $sdk pack /d $content /p $incompatible /no
if ($LASTEXITCODE -ne 0) { throw 'Could not build the incompatible-profile fixture.' }
Expect-InstallerRefusal '--inspect' $incompatible 'profile migration'
$release.profileSchema = 1
$release | ConvertTo-Json | Set-Content -LiteralPath "$content/qkvm-release.json" -Encoding utf8
$xml.Package.Identity.Name = 'qKVM.OtherProduct'
$xml.Save("$content/AppxManifest.xml")
$foreign = Join-Path $root 'foreign.msix'
& $sdk pack /d $content /p $foreign /no
if ($LASTEXITCODE -ne 0) { throw 'Could not build the foreign-product fixture.' }
Expect-InstallerRefusal '--inspect' $foreign 'publisher and product'
Write-Output "Installer structure, payload, startup, profile, trust and wrong-product checks passed. Evidence: $root"
