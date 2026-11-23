#Requires -Version 7.2
<#
.SYNOPSIS
Builds a runnable Windows folder and ZIP, x64's or Windows on ARM's, then checks startup without developer paths.
.DESCRIPTION
    tools/package.ps1
    tools/package.ps1 -SkipBuild
    tools/package.ps1 -Preset publish -SkipBuild     (what tools/build_release.ps1 does)
    tools/package.ps1 -Arch arm64 [-SkipBuild]       (the same for Windows on ARM, from the arm64-release preset)
    tools/package.ps1 -Arch arm64 -Preset publish -SkipBuild    (what tools/build_release.ps1 does for Windows on ARM)
Only existing approved dependencies are used. Nothing is installed. Each package has a new
folder under build/packages; an existing destination is never overwritten.

An ARM64 package is laid out as the x64 one, of the ARM64 kind (Qt, FFmpeg, WebRTC and the Visual C++ runtime). Made
on an x64 PC, its programs cannot start there: its startup checks (tools/test_package_archive.ps1 on its ZIP) are run
on Windows on ARM instead, and this says so (tools/build_release.ps1 says so of a release's, and the release's
instructions run them there). The release audit is run on it as on x64's.
#>
param([switch] $SkipBuild, [ValidateSet('release', 'publish')] [string] $Preset = 'release',
    [ValidateSet('x64', 'arm64')] [string] $Arch = 'x64')

. "$PSScriptRoot\common.ps1"
. "$PSScriptRoot\build_common.ps1"
Enter-BuildEnvironment -Arch $Arch
$buildPreset = if ($Arch -eq 'arm64') { "arm64-$Preset" } else { $Preset }
# (Made on a PC that cannot run what it makes: an ARM64 package on x64.)
$canRun = $Arch -eq 'x64' -or $env:PROCESSOR_ARCHITECTURE -eq 'ARM64'
Push-Location $DesktopRoot
try {
    if (-not $SkipBuild) {
        cmake --preset $buildPreset
        if ($LASTEXITCODE -ne 0) { throw 'Release configuration failed.' }
        cmake --build --preset $buildPreset --target qkvm-desktop qkvm-setup qkvm-update-check
        if ($LASTEXITCODE -ne 0) { throw 'Release build failed.' }
    }
    $built = Join-Path $DesktopRoot "build/$buildPreset/bin"
    $program = Join-Path $built 'qkvm-desktop.exe'
    if (-not (Test-Path -LiteralPath $program)) { throw 'Build the release application first.' }
    $version = (Get-Item -LiteralPath $program).VersionInfo.ProductVersion
    if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'The executable has no valid product version.' }
    # The name the release keeps (the ZIP and the folder in it); each packaging in a folder of its own time.
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
    $name = "qKVM-Desktop-$version-windows-$Arch"
    $package = Join-Path $DesktopRoot "build/packages/$stamp/$name"
    if (Test-Path -LiteralPath $package) { throw "Package already exists: $package" }
    New-Item -ItemType Directory -Path $package | Out-Null
    # The one program that ships. qKVM Setup stays out of the portable ZIP while it can install nothing (owner,
    # D1): build_installer.ps1 takes it from the build for an MSIX.
    Copy-Item -LiteralPath $program -Destination $package

    $qt = Get-QtRoot -Arch $Arch
    # (On an x64 PC the ARM64 Qt's own tools, ARM64 programs, cannot run: it is deployed by the x64 Qt's tool, told
    # where the ARM64 one is by the x64 qtpaths reading the ARM64 Qt's target_qt.conf, as Qt's host-qtpaths.bat does
    # on the PC Qt was built on.)
    $deploy = @("$qt/bin/windeployqt.exe")
    if (-not $canRun) {
        $qtpaths = Join-Path $DesktopRoot "build/$buildPreset/qtpaths-for-deployment.bat"
        @('@echo off', "`"$(Get-QtRoot)\bin\qtpaths.exe`" -qtconf `"$qt\bin\target_qt.conf`" %*") |
            Set-Content -LiteralPath $qtpaths -Encoding ascii
        $deploy = @("$(Get-QtRoot)/bin/windeployqt.exe", '--qtpaths', $qtpaths)
    }
    # The program draws with Direct3D 11 alone (ui/main.cpp): Direct3D 12's shader compiler (dxcompiler, dxil)
    # is never loaded, and the HLSL compiler Qt may ask for (d3dcompiler_47) is Windows' own since 8.1.
    & $deploy[0] @($deploy | Select-Object -Skip 1) --release --dir $package --plugindir "$package/plugins" `
        --qmldir "$DesktopRoot/ui/qml" --qmlimport "$DesktopRoot/build/$buildPreset/qml" `
        --no-compiler-runtime --no-opengl-sw --no-translations --no-ffmpeg `
        --no-system-d3d-compiler --no-system-dxc-compiler `
        --skip-plugin-types qmltooling,qmllint,qmlls,generic `
        --exclude-plugins qopensslbackend,qcertonlybackend,qdirect2d,qminimal,qoffscreen `
        "$package/qkvm-desktop.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Qt runtime deployment failed.' }
    # Qt's tool does not collect our decoder or WebRTC dependencies.
    # FFmpeg: the project's own build has avcodec and avutil; the development one (fetch_ffmpeg.ps1) needs swresample too.
    $deps = Get-DepsRoot -Arch $Arch
    foreach ($dll in @('avcodec-63.dll', 'avutil-61.dll', 'swresample-7.dll')) {
        $from = "$deps/ffmpeg/bin/$dll"
        if ($dll -like 'swresample-*' -and -not (Test-Path -LiteralPath $from)) { continue }
        Copy-Item -LiteralPath $from -Destination $package
    }
    $webrtc = if ($Arch -eq 'x64') { "$deps/webrtc-trial/install" } else { "$deps/webrtc/install" }
    Copy-Item -LiteralPath "$webrtc/bin/datachannel.dll" -Destination $package
    # The datachannel.dll of the build its record names, of the pinned sources for this kind (written beside the install
    # folder by experiments/webrtc/build-cpp-dependencies.ps1): another is refused. A build of before 2026-11-21 has no
    # record: that is said, and the release audit's webrtc gate stays open, refusing a beta, until it is built anew.
    $webrtcRecord = Join-Path (Split-Path -Parent $webrtc) $WebRtcRecordName
    if (Test-Path -LiteralPath $webrtcRecord) {
        $webrtcRefused = @(Get-WebRtcRecordRefusals (Get-Content -LiteralPath $webrtcRecord -Raw | ConvertFrom-Json) $Arch "$package/datachannel.dll")
        if ($webrtcRefused.Count) { throw "The package's datachannel.dll is not the one its build's record names ($webrtcRecord): $($webrtcRefused -join '; ')." }
    } else {
        Write-Output "No record of the WebRTC build beside $webrtc (built before 2026-11-21): the release audit's webrtc gate stays open, refusing a beta, until tools/fetch_webrtc.ps1 builds it anew."
    }
    # Use only the official redistributable directory of the program's kind (x64 or ARM64), never compiler/debug DLLs;
    # and of it only the libraries for the program's kind of processor (the ARM64 folder holds an x64 vcruntime140_1.dll,
    # for emulated code, which no ARM64 image of the package imports: 2026-11-19).
    $crt = Join-Path $env:VCToolsRedistDir "$Arch/Microsoft.VC143.CRT"
    $machine = if ($Arch -eq 'arm64') { 0xAA64 } else { 0x8664 }
    $runtime = @(Get-ChildItem -LiteralPath $crt -File -Filter '*.dll' | Where-Object { (Get-PeMachine ([IO.File]::ReadAllBytes($_.FullName))) -eq $machine })
    if ($runtime.Count -eq 0) { throw "No redistributable runtime files found in $crt." }
    foreach ($dll in $runtime) { Copy-Item -LiteralPath $dll.FullName -Destination $package }
    @('[Paths]', 'Prefix=.', 'Plugins=plugins', 'QmlImports=qml', 'Translations=translations') |
        Set-Content -LiteralPath "$package/qt.conf" -Encoding utf8

    $licenses = New-Item -ItemType Directory -Path "$package/licenses"
    Copy-Item -LiteralPath "$DesktopRoot/THIRD-PARTY.md" -Destination $package
    foreach ($file in (Get-ChildItem -LiteralPath "$DesktopRoot/licenses" -File)) {
        Copy-Item -LiteralPath $file.FullName -Destination $licenses.FullName
    }
    # (The ARM64 package's own of them, in place of the x64 ones of the same name: the runtime its FFmpeg is linked
    # against is another build's.)
    if ($Arch -eq 'arm64') {
        foreach ($file in (Get-ChildItem -LiteralPath "$DesktopRoot/licenses/arm64" -File)) {
            Copy-Item -LiteralPath $file.FullName -Destination $licenses.FullName -Force
        }
    }
    Copy-Item -LiteralPath "$deps/ffmpeg/LICENSE.txt" -Destination "$licenses/FFmpeg-LGPL-3.0.txt"
    # (zlib is inside the project's own avcodec; the development FFmpeg has no notice of it beside it.)
    if (Test-Path -LiteralPath "$deps/ffmpeg/zlib-LICENSE.txt") {
        Copy-Item -LiteralPath "$deps/ffmpeg/zlib-LICENSE.txt" -Destination "$licenses/zlib.txt"
    }
    # (The sources' notices, the same for both kinds: an ARM64 build is made from x64's sources.)
    $transport = "$DesktopRoot/.deps/webrtc-trial"
    $notices = @{
        'libdatachannel/LICENSE' = 'libdatachannel-MPL-2.0.txt'
        'libdatachannel/deps/libjuice/LICENSE' = 'libjuice-MPL-2.0.txt'
        'libdatachannel/deps/libsrtp/LICENSE' = 'libsrtp-BSD.txt'
        'libdatachannel/deps/usrsctp/LICENSE.md' = 'usrsctp-BSD.txt'
        'libdatachannel/deps/plog/LICENSE' = 'plog-MIT.txt'
        'mbedtls-3.6.7/LICENSE' = 'Mbed-TLS-license.txt'
        'mbedtls-3.6.7/3rdparty/everest/README.md' = 'Everest-notice.md'
        'mbedtls-3.6.7/3rdparty/p256-m/README.md' = 'p256-m-integration-notice.md'
        'mbedtls-3.6.7/3rdparty/p256-m/p256-m/README.md' = 'p256-m-notice.md'
    }
    foreach ($entry in $notices.GetEnumerator()) {
        Copy-Item -LiteralPath (Join-Path $transport $entry.Key) -Destination (Join-Path $licenses $entry.Value)
    }
    $sbom = New-Item -ItemType Directory -Path "$licenses/qt-sbom"
    foreach ($module in @('qtbase', 'qtdeclarative', 'qtsvg')) {
        Copy-Item -LiteralPath "$qt/sbom/$module-6.12.0.spdx.json" -Destination $sbom.FullName
    }
    # What the person who unpacks it reads first. Plain ASCII, short; the version from the program itself.
    $windows = if ($Arch -eq 'arm64') { 'Windows 11 on ARM (ARM64)' } else { 'Windows 10 and 11 (x64)' }
    @"
qKVM Desktop $version beta, for $windows

qKVM Desktop shows the screens of the machines behind your KVMs and server
boards, and gives you their keyboard and mouse: on its own, or with a qKVM
server. This is a beta: try it where a mistake costs little, and tell us what
you find at https://github.com/qkvm/qkvm-desktop/issues

To start: extract the whole ZIP, then open qkvm-desktop.exe in that folder.
Keep the DLLs and the plugins and qml folders beside it. Nothing is installed;
your settings and saved passwords belong to your Windows account.

The program is not yet signed with a code-signing certificate, so Windows may
say it protected your PC: choose More info, then Run anyway. Check the ZIP
first against the SHA-256 published with it:
    Get-FileHash $name.zip

Settings, Updates looks for a newer version and shows you the checked file;
it does not install it.

Its source: https://github.com/qkvm/qkvm-desktop/tree/v$version
Its licence: GNU AGPL version 3 (licenses/AGPL-3.0.txt). The libraries that
ship with it, their licences and their sources: THIRD-PARTY.md and licenses/.
"@ | Set-Content -LiteralPath "$package/START-HERE.txt" -Encoding ascii

    # Every image of it of its kind, and every library one imports held by it or Windows' own: else no package.
    $refused = @(Get-PackageImageRefusals $package $Arch)
    if ($refused.Count) { throw "The package is not whole and of its kind: $($refused -join '; ')." }
    # And every image of it built with what tools/image-protections.json asks of its kind, read from its bytes.
    $unprotected = @(Get-ImageProtectionRefusals $package $Arch)
    if ($unprotected.Count) { throw "The package holds an image not built with what its table asks: $($unprotected -join '; ')." }
    if ($canRun) { & "$PSScriptRoot/test_package.ps1" -PackageDirectory $package -Arch $Arch }
    # The manifest records the actual deployed bytes after Qt has made its paths relocatable.
    $files = @(Get-ChildItem -LiteralPath $package -File -Recurse | Sort-Object FullName | ForEach-Object {
        [ordered]@{
            path = [IO.Path]::GetRelativePath($package, $_.FullName).Replace('\', '/')
            bytes = $_.Length
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    })
    [ordered]@{ product = 'qKVM'; version = $version; architecture = $Arch; files = $files } |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$package/package-manifest.json" -Encoding utf8
    $audit = Join-Path $DesktopRoot "build/release-audits/$stamp-$name.json"
    & "$PSScriptRoot/audit_release.ps1" -PackageDirectory $package -OutputFile $audit
    # Keep matching symbols privately, outside the distributable. They can contain
    # source paths and are never uploaded by packaging or by crash diagnostics.
    # (Setup's too, for the MSIX that build_installer.ps1 makes with it; and qKVM Setup's update check's, which
    # tools/build_setup.ps1 puts into the setup and never into this package.)
    $symbols = (New-Item -ItemType Directory -Path "$DesktopRoot/build/symbols/$stamp-$name").FullName
    $symbolFiles = @()
    foreach ($programName in @('qkvm-desktop', 'qkvm-setup', 'qkvm-update-check')) {
        $pdb = "$built/$programName.pdb"
        if (-not (Test-Path -LiteralPath $pdb)) { throw "Missing Release symbols: $programName" }
        Copy-Item -LiteralPath $pdb -Destination $symbols
        $symbolFiles += [ordered]@{
            program = "$programName.exe"
            programSha256 = (Get-FileHash -LiteralPath "$built/$programName.exe").Hash.ToLowerInvariant()
            symbols = "$programName.pdb"
            symbolsSha256 = (Get-FileHash -LiteralPath "$symbols/$programName.pdb").Hash.ToLowerInvariant()
        }
    }
    $symbolFiles | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath "$symbols/manifest.json" -Encoding utf8
    $archive = "$package.zip"
    & $WindowsTar -a -cf $archive -C (Split-Path -Parent $package) $name
    if ($LASTEXITCODE -ne 0) { throw 'The package ZIP could not be written.' }
    $hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $(Split-Path -Leaf $archive)" | Set-Content -LiteralPath "$archive.sha256" -Encoding ascii
    if ($canRun) {
        & "$PSScriptRoot/test_package_archive.ps1" -Archive $archive
    } else {
        Write-Output "Not started here (an $Arch package on an $($env:PROCESSOR_ARCHITECTURE) PC): run tools/test_package_archive.ps1 -Archive on Windows on ARM."
    }
    # (Each kind its own: the x64 one is what the gate's setup steps read.)
    $latest = if ($Arch -eq 'x64') { 'latest.json' } else { "latest-$Arch.json" }
    $made = [ordered]@{ folder = $package; archive = $archive; sha256 = $hash; files = $files.Count; audit = $audit; symbols = $symbols }
    # (A package that could not be started here says so, for whoever takes it on: its startup and damaged-package checks
    # are still to run where it can start, review arm 1, L3. x64's record is as it was.)
    if (-not $canRun) { $made['startupChecked'] = $false }
    $made | ConvertTo-Json | Set-Content -LiteralPath "$DesktopRoot/build/packages/$latest" -Encoding utf8
    Write-Output "Runnable package: $package" "ZIP: $archive" "SHA-256: $hash"
} finally {
    Pop-Location
}
