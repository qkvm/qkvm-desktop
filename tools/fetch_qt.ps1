<#
.SYNOPSIS
Fetches the Qt that QKVM Desktop is built with, and tells the build where it is.

.DESCRIPTION
    tools\fetch_qt.ps1 -Into D:\Qt

puts Qt 6.12.0 (open-source edition, for Microsoft's compiler, x64) into D:\Qt\6.12.0\msvc2022_64
and writes that place into local.cmake beside CMakeLists.txt, which the build reads and git does
not keep. Qt stays outside the repository because of its size.

Qt's own installer wants an account, and aqtinstall (3.3.0) still asks at the address Qt used
before 6.11. So this takes the archives from Qt's server itself: only the modules the program
links, each checked against the size and the SHA-256 written down below, so that what is built
with is what was there when this list was made.

A newer Qt means a new list. The server keeps each archive's sum beside it, as <archive>.sha256,
and the names of the archives in Updates.xml in the same folder.

    tools\fetch_qt.ps1 -Into D:\Qt -Downloads D:\kept

keeps the archives in D:\kept, where the next run takes them again after checking each one's size
and SHA-256 (tools/build_release.ps1 does this). Without it they are deleted once unpacked.
#>
param(
    [Parameter(Mandatory)] [string] $Into,
    [string] $Downloads = ''
)

. "$PSScriptRoot\common.ps1"

$Version = '6.12.0'
$Kit = 'msvc2022_64'
$Server = 'https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/qt6_6120/qt6_6120_msvc2022_64'
$Build = '6.12.0-0-202609280346'
$Tail = '-Windows-Windows_11_24H2-MSVC2022-Windows-Windows_11_24H2-X86_64.7z'

# Left out on purpose: Qt's documentation, its own translations, its tools for translating (until the
# program has a second language), the software OpenGL (the program draws with Direct3D), and every
# module that is not LGPL.
$Archives = @(
    @{ Package = 'qt.qt6.6120.win64_msvc2022_64'; Module = 'qtbase'; Bytes = 40184745
       Sha256 = '219d1b86def3be418e31d704572bc4acc91b2ec373c65595c9fb12b42bf098dc' }
    @{ Package = 'qt.qt6.6120.win64_msvc2022_64'; Module = 'qtsvg'; Bytes = 690561
       Sha256 = 'dfcad02224b71724e3314d0bc3ffc0b862dfa6e02bc4aadd39f1738f6c364c79' }
    @{ Package = 'qt.qt6.6120.win64_msvc2022_64'; Module = 'qtdeclarative'; Bytes = 199517162
       Sha256 = '258146e9972fadbc1162cda31c7d7ed7e627f5022b2a848f2edb84d9c2ce27db' }
    @{ Package = 'qt.qt6.6120.addons.qtwebsockets.win64_msvc2022_64'; Module = 'qtwebsockets'; Bytes = 320590
       Sha256 = '836eb7eafa8cb1d85e0ca6faf062269acafae68579add29551450a7064a8ffcf' }
)

$Root = Join-Path $Into "$Version\$Kit"
$keep = [bool] $Downloads
if (-not $keep) { $Downloads = Join-Path $Into '.download' }
New-Item -ItemType Directory -Force -Path $Downloads | Out-Null

Write-Host "Qt $Version ($Kit) into $Root"
foreach ($archive in $Archives) {
    $name = "$Build$($archive.Module)$Tail"
    $file = Join-Path $Downloads $name
    Get-Checked "$Server/$($archive.Package)/$name" $file $archive.Bytes $archive.Sha256
    Write-Host "  unpacking $($archive.Module)"
    Expand-Checked $file $Root
}

foreach ($needed in 'bin\Qt6Core.dll', 'bin\Qt6Quick.dll', 'bin\Qt6WebSockets.dll', 'bin\qmllint.exe', 'lib\cmake\Qt6\Qt6Config.cmake') {
    if (-not (Test-Path -LiteralPath (Join-Path $Root $needed))) { throw "After unpacking, $needed is not in $Root." }
}
if (-not $keep) { Remove-Item -LiteralPath $Downloads -Recurse -Force }

$local = Join-Path $DesktopRoot 'local.cmake'
$forCMake = (Resolve-Path -LiteralPath $Root).Path -replace '\\', '/'
Set-Content -LiteralPath $local -Encoding ascii -Value @(
    '# Written by tools/fetch_qt.ps1: where Qt is on this PC. Git does not keep this file.'
    "set(QKVM_QT_DIR `"$forCMake`")"
)
Write-Host "Qt is in $Root, and local.cmake says so."
