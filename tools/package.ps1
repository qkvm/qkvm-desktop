#Requires -Version 7.2
<#
.SYNOPSIS
Builds a runnable Windows x64 folder and ZIP, then checks startup without developer paths.
.DESCRIPTION
    tools/package.ps1
    tools/package.ps1 -SkipBuild
    tools/package.ps1 -Preset publish -SkipBuild     (what tools/build_release.ps1 does)
Only existing approved dependencies are used. Nothing is installed. Each package has a new
folder under build/packages; an existing destination is never overwritten.
#>
param([switch] $SkipBuild, [ValidateSet('release', 'publish')] [string] $Preset = 'release')

. "$PSScriptRoot\common.ps1"
Enter-BuildEnvironment
Push-Location $DesktopRoot
try {
    if (-not $SkipBuild) {
        cmake --preset $Preset
        if ($LASTEXITCODE -ne 0) { throw 'Release configuration failed.' }
        cmake --build --preset $Preset --target qkvm-desktop qkvm-setup
        if ($LASTEXITCODE -ne 0) { throw 'Release build failed.' }
    }
    $built = Join-Path $DesktopRoot "build/$Preset/bin"
    $program = Join-Path $built 'qkvm-desktop.exe'
    if (-not (Test-Path -LiteralPath $program)) { throw 'Build the release application first.' }
    $version = (Get-Item -LiteralPath $program).VersionInfo.ProductVersion
    if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'The executable has no valid product version.' }
    # The name the release keeps (the ZIP and the folder in it); each packaging in a folder of its own time.
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
    $name = "qKVM-Desktop-$version-windows-x64"
    $package = Join-Path $DesktopRoot "build/packages/$stamp/$name"
    if (Test-Path -LiteralPath $package) { throw "Package already exists: $package" }
    New-Item -ItemType Directory -Path $package | Out-Null
    # The one program that ships. qKVM Setup stays out of the portable ZIP while it can install nothing (owner,
    # D1): build_installer.ps1 takes it from the build for an MSIX.
    Copy-Item -LiteralPath $program -Destination $package

    $qt = Get-QtRoot
    # The program draws with Direct3D 11 alone (ui/main.cpp): Direct3D 12's shader compiler (dxcompiler, dxil)
    # is never loaded, and the HLSL compiler Qt may ask for (d3dcompiler_47) is Windows' own since 8.1.
    & "$qt/bin/windeployqt.exe" --release --dir $package --plugindir "$package/plugins" `
        --qmldir "$DesktopRoot/ui/qml" --qmlimport "$DesktopRoot/build/$Preset/qml" `
        --no-compiler-runtime --no-opengl-sw --no-translations --no-ffmpeg `
        --no-system-d3d-compiler --no-system-dxc-compiler `
        --skip-plugin-types qmltooling,qmllint,qmlls,generic `
        --exclude-plugins qopensslbackend,qcertonlybackend,qdirect2d,qminimal,qoffscreen `
        "$package/qkvm-desktop.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Qt runtime deployment failed.' }
    # Qt's tool does not collect our decoder or WebRTC dependencies.
    # FFmpeg: the project's own build has avcodec and avutil; the development one (fetch_ffmpeg.ps1) needs swresample too.
    foreach ($dll in @('avcodec-63.dll', 'avutil-61.dll', 'swresample-7.dll')) {
        $from = "$DesktopRoot/.deps/ffmpeg/bin/$dll"
        if ($dll -like 'swresample-*' -and -not (Test-Path -LiteralPath $from)) { continue }
        Copy-Item -LiteralPath $from -Destination $package
    }
    Copy-Item -LiteralPath "$DesktopRoot/.deps/webrtc-trial/install/bin/datachannel.dll" -Destination $package
    # Use only the official x64 redistributable directory, never compiler/debug DLLs.
    $crt = Join-Path $env:VCToolsRedistDir 'x64/Microsoft.VC143.CRT'
    $runtime = @(Get-ChildItem -LiteralPath $crt -File -Filter '*.dll')
    if ($runtime.Count -eq 0) { throw "No redistributable runtime files found in $crt." }
    foreach ($dll in $runtime) { Copy-Item -LiteralPath $dll.FullName -Destination $package }
    @('[Paths]', 'Prefix=.', 'Plugins=plugins', 'QmlImports=qml', 'Translations=translations') |
        Set-Content -LiteralPath "$package/qt.conf" -Encoding utf8

    $licenses = New-Item -ItemType Directory -Path "$package/licenses"
    Copy-Item -LiteralPath "$DesktopRoot/THIRD-PARTY.md" -Destination $package
    foreach ($file in (Get-ChildItem -LiteralPath "$DesktopRoot/licenses" -File)) {
        Copy-Item -LiteralPath $file.FullName -Destination $licenses.FullName
    }
    Copy-Item -LiteralPath "$DesktopRoot/.deps/ffmpeg/LICENSE.txt" -Destination "$licenses/FFmpeg-LGPL-3.0.txt"
    # (zlib is inside the project's own avcodec; the development FFmpeg has no notice of it beside it.)
    if (Test-Path -LiteralPath "$DesktopRoot/.deps/ffmpeg/zlib-LICENSE.txt") {
        Copy-Item -LiteralPath "$DesktopRoot/.deps/ffmpeg/zlib-LICENSE.txt" -Destination "$licenses/zlib.txt"
    }
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
    @"
qKVM Desktop $version beta, for Windows 10 and 11 (x64)

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
    Get-FileHash qKVM-Desktop-$version-windows-x64.zip

Settings, Updates looks for a newer version and shows you the checked file;
it does not install it.

Its source: https://github.com/qkvm/qkvm-desktop/tree/v$version
Its licence: GNU AGPL version 3 (licenses/AGPL-3.0.txt). The libraries that
ship with it, their licences and their sources: THIRD-PARTY.md and licenses/.
"@ | Set-Content -LiteralPath "$package/START-HERE.txt" -Encoding ascii

    & "$PSScriptRoot/test_package.ps1" -PackageDirectory $package
    # The manifest records the actual deployed bytes after Qt has made its paths relocatable.
    $files = @(Get-ChildItem -LiteralPath $package -File -Recurse | Sort-Object FullName | ForEach-Object {
        [ordered]@{
            path = [IO.Path]::GetRelativePath($package, $_.FullName).Replace('\', '/')
            bytes = $_.Length
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    })
    [ordered]@{ product = 'qKVM'; version = $version; architecture = 'x64'; files = $files } |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$package/package-manifest.json" -Encoding utf8
    $audit = Join-Path $DesktopRoot "build/release-audits/$stamp-$name.json"
    & "$PSScriptRoot/audit_release.ps1" -PackageDirectory $package -OutputFile $audit
    # Keep matching symbols privately, outside the distributable. They can contain
    # source paths and are never uploaded by packaging or by crash diagnostics.
    # (Setup's too, for the MSIX that build_installer.ps1 makes with it.)
    $symbols = (New-Item -ItemType Directory -Path "$DesktopRoot/build/symbols/$stamp-$name").FullName
    $symbolFiles = @()
    foreach ($programName in @('qkvm-desktop', 'qkvm-setup')) {
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
    & "$PSScriptRoot/test_package_archive.ps1" -Archive $archive
    [ordered]@{ folder = $package; archive = $archive; sha256 = $hash; files = $files.Count; audit = $audit; symbols = $symbols } |
        ConvertTo-Json | Set-Content -LiteralPath "$DesktopRoot/build/packages/latest.json" -Encoding utf8
    Write-Output "Runnable package: $package" "ZIP: $archive" "SHA-256: $hash"
} finally {
    Pop-Location
}
