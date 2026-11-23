#Requires -Version 7.2
<#
What every program and library of a package, and each update check the setup carries, is held to against the misuse
of its memory (build_common.ps1's Get-PeProtections, Get-PeProtectionNames, Get-ImageFileProtectionRefusals and
Get-ImageProtectionRefusals, and the table tools/image-protections.json), on made-up images that are never run; the
table's own rules; and that tools/package.ps1, tools/test_package.ps1, tools/build_setup.ps1 and the release audit
refuse what the table refuses. Every failed check is a "FAIL:" line, and the script then fails.
#>
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\build_common.ps1"

$failures = 0
function Check([bool] $Condition, [string] $Message) {
    # (Written to the host: a check inside a function whose output is dropped is still seen.)
    if (-not $Condition) { Write-Host "FAIL: $Message"; $script:failures++ }
}
$desktop = Split-Path -Parent $PSScriptRoot
$work = Join-Path $desktop "build/protection-tests/$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $work | Out-Null
$x64 = [uint16] 0x8664
$arm64 = [uint16] 0xAA64

# A made-up 64-bit Windows image, never run, built as a program that ships is: one section of code, its relocations, a
# load configuration whose GuardFlags say its code was built for CFG and lists its exception handlers' targets, a debug
# directory whose extended characteristics say CET_COMPAT, an unwind table of $Functions functions and $Signed PACIBSP
# instructions among its code (at $Skew bytes from where an instruction begins); its characteristics say ASLR, high
# entropy, DEP and CFG. $Lacking takes away what it names: aslr, highEntropyVa, dep, cfg (the characteristic),
# cfgCode (the load configuration's word of it), relocations, cet, ehContinuation.
function New-GuardedImage([uint16] $Machine = 0x8664, [string[]] $Lacking = @(), [int] $Signed = 0, [int] $Functions = 0, [int] $Skew = 0) {
    $code = 0x100 + 8 * $Functions
    $size = $code + 4 * ($Signed + 4) + $Skew
    $bytes = [byte[]]::new(0x200 + $size)
    $put = { param([int] $At, [long] $Value, [int] $Width) [Array]::Copy([BitConverter]::GetBytes($Value), 0, $bytes, $At, $Width) }
    $bytes[0] = 0x4D; $bytes[1] = 0x5A
    & $put 0x3C 0x40 4
    & $put 0x40 0x4550 4
    & $put 0x44 $Machine 2
    & $put 0x46 1 2
    & $put 0x54 240 2
    $optional = 0x58
    & $put $optional 0x20B 2
    $characteristics = 0
    foreach ($bit in @(@('aslr', 0x40), @('highEntropyVa', 0x20), @('dep', 0x100), @('cfg', 0x4000))) {
        if ($Lacking -cnotcontains $bit[0]) { $characteristics = $characteristics -bor $bit[1] }
    }
    & $put ($optional + 70) $characteristics 2
    & $put ($optional + 108) 16 4
    $rva = 0x1000
    $directory = { param([int] $Index, [long] $At, [long] $Size) & $put ($optional + 112 + 8 * $Index) ($rva + $At) 4; & $put ($optional + 116 + 8 * $Index) $Size 4 }
    # The load configuration, at 0 of the section: 148 bytes, its GuardFlags at 144.
    & $put 0x200 148 4
    $guard = 0
    if ($Lacking -cnotcontains 'cfgCode') { $guard = $guard -bor 0x100 }
    if ($Lacking -cnotcontains 'ehContinuation') { $guard = $guard -bor 0x400000 }
    & $put (0x200 + 144) $guard 4
    & $directory 10 0 148
    # The debug directory at 0xA0: one entry of type 20, its four bytes at 0xC0.
    & $put (0x200 + 0xA0 + 12) 20 4
    & $put (0x200 + 0xA0 + 16) 4 4
    & $put (0x200 + 0xA0 + 20) ($rva + 0xC0) 4
    & $put (0x200 + 0xA0 + 24) (0x200 + 0xC0) 4
    & $put (0x200 + 0xC0) $(if ($Lacking -ccontains 'cet') { 0 } else { 1 }) 4
    & $directory 6 0xA0 28
    if ($Lacking -cnotcontains 'relocations') { & $directory 5 0xC8 8 }
    if ($Functions) { & $directory 3 0x100 (8 * $Functions) }
    for ($index = 0; $index -lt $Signed; ++$index) { & $put (0x200 + $code + $Skew + 4 * $index) 0xD503237F 4 }
    $section = $optional + 240
    [Text.Encoding]::ASCII.GetBytes('.text').CopyTo($bytes, $section)
    & $put ($section + 8) $size 4
    & $put ($section + 12) $rva 4
    & $put ($section + 16) $size 4
    & $put ($section + 20) 0x200 4
    & $put ($section + 36) 0x60000020 4
    return $bytes
}
function Names([byte[]] $Bytes) { return (@(Get-PeProtectionNames (Get-PeProtections $Bytes)) -join ',') }

# --- What an image's bytes say.
$all = 'aslr,highEntropyVa,dep,cfg,cet,ehContinuation'
Check ((Names (New-GuardedImage)) -ceq $all) "an x64 image built with every protection says so ($(Names (New-GuardedImage)))"
foreach ($one in 'aslr', 'highEntropyVa', 'dep', 'cfg', 'cet', 'ehContinuation') {
    $said = Names (New-GuardedImage -Lacking $one)
    # (High entropy is ASLR's: without ASLR there is none.)
    $gone = @($one) + @(if ($one -ceq 'aslr') { 'highEntropyVa' })
    Check ($said -ceq (($all -split ',' | Where-Object { $gone -cnotcontains $_ }) -join ',')) "an image without $one is read without it, and with the others ($said)"
}
# (ASLR needs its relocations, and high entropy needs ASLR; CFG needs code built for it as well as the characteristic.)
Check ((Names (New-GuardedImage -Lacking 'relocations')) -ceq 'dep,cfg,cet,ehContinuation') 'an image without its relocations has no ASLR, high entropy or not'
Check ((Names (New-GuardedImage -Lacking 'aslr')) -cnotmatch 'highEntropyVa') 'high entropy without ASLR is no protection'
Check ((Names (New-GuardedImage -Lacking 'cfgCode')) -cnotmatch '\bcfg\b') 'the characteristic of CFG without the code built for it is no CFG'
# ARM64's signed returns: two in three of its functions, counted at the start of an instruction, never on x64.
$read = Get-PeProtections (New-GuardedImage $arm64 -Signed 90 -Functions 100)
Check ($read.Signed -eq 90 -and $read.Functions -eq 100) "the signed returns and the functions of an ARM64 image are counted ($($read.Signed) of $($read.Functions))"
Check ((Names (New-GuardedImage $arm64 -Signed 90 -Functions 100)) -cmatch ',pac$') 'an ARM64 image whose functions sign their returns has pac'
Check ((Names (New-GuardedImage $arm64 -Signed 67 -Functions 100)) -cmatch ',pac$') 'two in three are enough (67 of 100)'
Check ((Names (New-GuardedImage $arm64 -Signed 66 -Functions 100)) -cnotmatch 'pac') 'fewer than two in three are not (66 of 100)'
Check ((Names (New-GuardedImage $arm64 -Signed 53 -Functions 47000)) -cnotmatch 'pac') 'a program built without /guard:signret, its runtime''s few pieces only, has no pac'
Check ((Get-PeProtections (New-GuardedImage $arm64 -Signed 90 -Functions 100 -Skew 2)).Signed -eq 0) 'the bytes of PACIBSP where no instruction begins are not counted'
Check ((Get-PeProtections (New-GuardedImage $x64 -Signed 90 -Functions 100)).Signed -eq 0) 'an x64 image has no signed returns, whatever its bytes'
Check ((Names (New-GuardedImage $arm64 -Signed 0 -Functions 0)) -cnotmatch 'pac') 'an ARM64 image with no unwind table has no pac'
$threw = $false
try { Get-PeProtections ([Text.Encoding]::ASCII.GetBytes('MZ but no Windows image after it')) | Out-Null } catch { $threw = $true }
Check $threw 'bytes that are no Windows image are refused'

# --- The table: our own programs have every protection of their kind, and another's library keeps on ARM64 what its x64
# build has but the shadow stack, which ARM64 has not; where ARM64's guard of returns does not stand in for it, its row
# says why.
$table = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'image-protections.json') -Raw | ConvertFrom-Json
$known = @($table.protections.PSObject.Properties.Name)
$rows = @($table.images)
foreach ($row in $rows) {
    foreach ($kind in 'x64', 'arm64') {
        $unknown = @(@($row.$kind) | Where-Object { $known -cnotcontains $_ })
        Check ($unknown.Count -eq 0) "the row of $($row.of) asks only for protections the table names ($($unknown -join ', '))"
    }
    Check (@($row.x64) -cnotcontains 'pac' -and @($row.arm64) -cnotcontains 'cet') "the row of $($row.of) asks of each kind what it can have"
    $kept = @(@($row.x64) | Where-Object { $_ -cne 'cet' -and @($row.arm64) -cnotcontains $_ })
    Check ($kept.Count -eq 0) "$($row.of) keeps on ARM64 what it has on x64 ($($kept -join ', ') lacking)"
    if (@($row.x64) -ccontains 'cet' -and @($row.arm64) -cnotcontains 'pac') {
        Check ([bool] $row.PSObject.Properties['note'] -and $row.note -match 'ARM64') "the row of $($row.of) says why ARM64 has no guard of its returns where x64 has one"
    }
}
$ours = @($rows | Where-Object { @($_.names) -ccontains 'qkvm-desktop.exe' })
Check ($ours.Count -eq 1 -and $rows.IndexOf($ours[0]) -eq 0) 'our own programs are the first row'
if ($ours.Count) {
    Check ((@($ours[0].x64) | Sort-Object) -join ',' -ceq 'aslr,cet,cfg,dep,highEntropyVa') "our own programs have every protection on x64 ($(@($ours[0].x64) -join ', '))"
    Check ((@($ours[0].arm64) | Sort-Object) -join ',' -ceq 'aslr,cfg,dep,highEntropyVa,pac') "our own programs have every protection on ARM64 ($(@($ours[0].arm64) -join ', '))"
    foreach ($program in 'qkvm-setup.exe', 'qkvm-update-check.exe', 'qkvm-update-check-arm64.exe') {
        Check (@($ours[0].names) -ccontains $program) "$program is one of our own programs"
    }
}
# (Every image the 0.2.0 packages were seen to hold, x64's and ARM64's, has its row; read 2026-11-21.)
$seen = @('avcodec-63.dll', 'avutil-61.dll', 'swresample-7.dll', 'concrt140.dll', 'datachannel.dll', 'msvcp140.dll', 'msvcp140_1.dll',
    'msvcp140_2.dll', 'msvcp140_atomic_wait.dll', 'msvcp140_codecvt_ids.dll', 'vccorlib140.dll', 'vcruntime140.dll',
    'vcruntime140_1.dll', 'vcruntime140_threads.dll', 'qkvm-desktop.exe', 'Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Network.dll',
    'Qt6OpenGL.dll', 'Qt6Qml.dll', 'Qt6QmlMeta.dll', 'Qt6QmlModels.dll', 'Qt6QmlWorkerScript.dll', 'Qt6Quick.dll',
    'Qt6QuickLayouts.dll', 'Qt6QuickShapes.dll', 'Qt6QuickTemplates2.dll', 'Qt6Svg.dll', 'plugins/iconengines/qsvgicon.dll',
    'plugins/imageformats/qgif.dll', 'plugins/networkinformation/qnetworklistmanager.dll', 'plugins/platforms/qwindows.dll',
    'plugins/tls/qschannelbackend.dll', 'qml/QtQml/Models/modelsplugin.dll', 'qml/QtQuick/Templates/qtquicktemplates2plugin.dll')
foreach ($path in $seen) {
    Check (@($rows | Where-Object { $row = $_; @(@($row.names) | Where-Object { $path -like $_ }).Count }).Count -ge 1) "the table has a row for $path"
}

# --- A package's images, each held to its row.
function New-Package([string] $Name, [hashtable] $Images) {
    $folder = Join-Path $work $Name
    foreach ($entry in $Images.GetEnumerator()) {
        $file = Join-Path $folder $entry.Key
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $file) | Out-Null
        [IO.File]::WriteAllBytes($file, [byte[]] $entry.Value)
    }
    return $folder
}
function Whole([uint16] $Machine, [hashtable] $Change = @{}) {
    # Every row's images as each must be: on ARM64 our own and the runtime's signing their returns, Qt's not.
    $signed = if ($Machine -eq $arm64) { @{ Signed = 9; Functions = 10 } } else { @{} }
    $images = @{
        'qkvm-desktop.exe' = New-GuardedImage $Machine @signed
        'vcruntime140.dll' = New-GuardedImage $Machine @signed
        'msvcp140_atomic_wait.dll' = New-GuardedImage $Machine @signed
        'Qt6Core.dll' = New-GuardedImage $Machine -Lacking 'ehContinuation'
        'plugins/platforms/qwindows.dll' = New-GuardedImage $Machine -Lacking 'ehContinuation'
        'qml/QtQuick/qtquick2plugin.dll' = New-GuardedImage $Machine -Lacking 'ehContinuation'
        'avcodec-63.dll' = New-GuardedImage $Machine -Lacking 'cfg', 'cfgCode', 'cet', 'ehContinuation'
        'datachannel.dll' = New-GuardedImage $Machine -Lacking 'cfg', 'cfgCode', 'cet', 'ehContinuation'
    }
    foreach ($name in $Change.Keys) {
        if ($null -eq $Change[$name]) { $images.Remove($name) } else { $images[$name] = $Change[$name] }
    }
    return $images
}
foreach ($kind in @(@('x64', $x64), @('arm64', $arm64))) {
    $said = @(Get-ImageProtectionRefusals (New-Package "whole-$($kind[0])" (Whole $kind[1])) $kind[0])
    Check ($said.Count -eq 0) "a $($kind[0]) package whose every image has what its row asks passes ($($said -join '; '))"
}
function Expect-Refused([string] $Name, [string] $Arch, [hashtable] $Images, [string] $Expected) {
    $said = @(Get-ImageProtectionRefusals (New-Package $Name $Images) $Arch)
    Check (@($said | Where-Object { $_ -match $Expected }).Count -eq 1 -and $said.Count -eq 1) "$Name is refused, and for that alone ($($said -join '; '))"
}
foreach ($one in 'aslr', 'highEntropyVa', 'dep', 'cfg', 'cet') {
    $lacks = if ($one -ceq 'aslr') { 'aslr, highEntropyVa' } else { $one }
    Expect-Refused "x64-program-without-$one" 'x64' (Whole $x64 @{ 'qkvm-desktop.exe' = New-GuardedImage -Lacking $one }) "\Aqkvm-desktop\.exe lacks $lacks, which every image of qKVM Desktop's own has on x64\z"
}
Expect-Refused 'x64-qt-without-cet' 'x64' (Whole $x64 @{ 'Qt6Core.dll' = New-GuardedImage -Lacking 'ehContinuation', 'cet' }) '\AQt6Core\.dll lacks cet, which every image of Qt has on x64\z'
Expect-Refused 'x64-plugin-without-cfg' 'x64' (Whole $x64 @{ 'plugins/platforms/qwindows.dll' = New-GuardedImage -Lacking 'cfgCode' }) '\Aplugins/platforms/qwindows\.dll lacks cfg, which every image of Qt has'
Expect-Refused 'x64-runtime-without-eh' 'x64' (Whole $x64 @{ 'vcruntime140.dll' = New-GuardedImage -Lacking 'ehContinuation' }) '\Avcruntime140\.dll lacks ehContinuation, which every image of the Visual C\+\+ runtime has on x64\z'
Expect-Refused 'x64-ffmpeg-without-dep' 'x64' (Whole $x64 @{ 'avcodec-63.dll' = New-GuardedImage -Lacking 'cfg', 'dep' }) '\Aavcodec-63\.dll lacks dep, which every image of FFmpeg'
Expect-Refused 'x64-webrtc-without-aslr' 'x64' (Whole $x64 @{ 'datachannel.dll' = New-GuardedImage -Lacking 'relocations' }) '\Adatachannel\.dll lacks aslr, highEntropyVa, which every image of libdatachannel'
Expect-Refused 'arm64-program-without-pac' 'arm64' (Whole $arm64 @{ 'qkvm-desktop.exe' = New-GuardedImage $arm64 -Signed 53 -Functions 47000 }) "\Aqkvm-desktop\.exe lacks pac, which every image of qKVM Desktop's own has on arm64\z"
Expect-Refused 'arm64-program-without-cfg' 'arm64' (Whole $arm64 @{ 'qkvm-desktop.exe' = New-GuardedImage $arm64 -Lacking 'cfg' -Signed 9 -Functions 10 }) '\Aqkvm-desktop\.exe lacks cfg,'
Expect-Refused 'arm64-runtime-without-pac' 'arm64' (Whole $arm64 @{ 'msvcp140_atomic_wait.dll' = New-GuardedImage $arm64 }) '\Amsvcp140_atomic_wait\.dll lacks pac, which every image of the Visual C\+\+ runtime has on arm64\z'
Expect-Refused 'arm64-qml-without-cfg' 'arm64' (Whole $arm64 @{ 'qml/QtQuick/qtquick2plugin.dll' = New-GuardedImage $arm64 -Lacking 'cfg' }) '\Aqml/QtQuick/qtquick2plugin\.dll lacks cfg, which every image of Qt has on arm64\z'
Expect-Refused 'unknown-library' 'x64' (Whole $x64 @{ 'plugins2/other.dll' = New-GuardedImage }) '\Aplugins2/other\.dll is named by no row of the table of protections'
Expect-Refused 'unreadable-library' 'x64' (Whole $x64 @{ 'Qt6Gui.dll' = [Text.Encoding]::ASCII.GetBytes('no image') }) '\AQt6Gui\.dll''s protections cannot be read'
# (Names in capitals or not: an image's row is found by its name as Windows finds the file.)
$capitals = Whole $x64
$capitals.Remove('qkvm-desktop.exe')
$capitals['QKVM-DESKTOP.EXE'] = New-GuardedImage -Lacking 'cet'
$said = @(Get-ImageProtectionRefusals (New-Package 'capitals' $capitals) 'x64')
Check (@($said | Where-Object { $_ -match "QKVM-DESKTOP\.EXE lacks cet, which every image of qKVM Desktop's own" }).Count -eq 1) "a name in capitals is held to its row ($($said -join '; '))"

# --- qKVM Setup's two update checks, which the setup carries outside the packages and puts at the program's name while
# an update has the earlier version set aside: each held to its row, by the name it is carried under, as every image of
# a package is (Get-ImageFileProtectionRefusals; review r2 of arm and armp, L1). On ARM64 its many pieces of the static
# runtime sign their returns whether or not its own code was built with /guard:signret, so its row's cfg, which
# qkvm_build gives with that switch, is what tells: one without CFG is refused although its share of signed returns is
# enough.
function Check-File([byte[]] $Bytes) {
    $file = Join-Path $work "check-$([guid]::NewGuid().ToString('N')).exe"
    [IO.File]::WriteAllBytes($file, $Bytes)
    return $file
}
function Expect-CheckRefused([string] $Name, [string] $Arch, [byte[]] $Bytes, [string] $Expected, [string] $Message) {
    $said = @(Get-ImageFileProtectionRefusals -File (Check-File $Bytes) -Name $Name -Arch $Arch)
    Check ($said.Count -eq 1 -and $said[0] -match $Expected) "$Message ($($said -join '; '))"
}
$said = @(Get-ImageFileProtectionRefusals -File (Check-File (New-GuardedImage)) -Name 'qkvm-update-check.exe' -Arch x64)
Check ($said.Count -eq 0) "an x64 update check built as our own programs are passes ($($said -join '; '))"
$said = @(Get-ImageFileProtectionRefusals -File (Check-File (New-GuardedImage $arm64 -Signed 9 -Functions 10)) -Name 'qkvm-update-check-arm64.exe' -Arch arm64)
Check ($said.Count -eq 0) "an ARM64 update check built as our own programs are passes ($($said -join '; '))"
Expect-CheckRefused 'qkvm-update-check.exe' 'x64' (New-GuardedImage -Lacking 'cet') "\Aqkvm-update-check\.exe lacks cet, which every image of qKVM Desktop's own has on x64\z" 'an x64 update check without CET_COMPAT is refused'
Expect-CheckRefused 'qkvm-update-check.exe' 'x64' (New-GuardedImage -Lacking 'cfgCode') "\Aqkvm-update-check\.exe lacks cfg, which every image of qKVM Desktop's own has on x64\z" 'an x64 update check without CFG is refused'
Expect-CheckRefused 'qkvm-update-check.exe' 'x64' (New-GuardedImage -Lacking 'relocations') "\Aqkvm-update-check\.exe lacks aslr, highEntropyVa, which every image" 'an x64 update check without its relocations is refused'
Expect-CheckRefused 'qkvm-update-check-arm64.exe' 'arm64' (New-GuardedImage $arm64 -Lacking 'cfg' -Signed 9 -Functions 10) "\Aqkvm-update-check-arm64\.exe lacks cfg, which every image of qKVM Desktop's own has on arm64\z" 'an ARM64 update check without CFG is refused, its signed returns enough or not'
Expect-CheckRefused 'qkvm-update-check-arm64.exe' 'arm64' (New-GuardedImage $arm64 -Signed 53 -Functions 47000) "\Aqkvm-update-check-arm64\.exe lacks pac, which every image of qKVM Desktop's own has on arm64\z" 'an ARM64 update check without its signed returns is refused'
Expect-CheckRefused 'qkvm-update-check.exe' 'x64' ([Text.Encoding]::ASCII.GetBytes('MZ but no image')) "\Aqkvm-update-check\.exe's protections cannot be read" 'an update check whose protections cannot be read is refused'
Expect-CheckRefused 'another-check.exe' 'x64' (New-GuardedImage) '\Aanother-check\.exe is named by no row of the table of protections' 'a program carried under a name no row names is refused'
# (Those who hold them to it: tools/build_setup.ps1 makes no setup of one it refuses, and the release audit opens its
# protections gate, never accepted, for a setup that carries one; test_setup.ps1 and test_release_audit.ps1 try both.)
$setupMaker = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'build_setup.ps1'))
Check ($setupMaker -match '\$unprotected = @\(Get-ImageFileProtectionRefusals -File \$check\.FullName -Name \$name -Arch \$kind\)\r?\n\s+if \(\$unprotected\.Count\) \{ throw') 'build_setup.ps1 makes no setup of an update check the table refuses'
$auditText = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'audit_release.ps1'))
Check ($auditText -match '\$checkLacks = @\(Get-ImageFileProtectionRefusals -File \$checkFile -Name \$checkName -Arch \$kind\)\r?\n\s+\$protectionRefusals \+= ') 'the release audit holds each update check of the setup to the table, in the protections gate'

# --- Those who hold a package to the table: tools/package.ps1 makes none it refuses, tools/test_package.ps1 refuses one
# before it starts anything (a made-up ARM64 package with every file it asks for, our program without its signed
# returns), and the release audit's protections gate, never accepted open (test_release_audit.ps1 tries it).
$packager = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'package.ps1'))
Check ($packager -match '\$unprotected = @\(Get-ImageProtectionRefusals \$package \$Arch\)\r?\n\s+if \(\$unprotected\.Count\) \{ throw') 'package.ps1 makes no package the table refuses'
$required = @('qkvm-desktop.exe', 'Qt6Quick.dll', 'Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Network.dll', 'Qt6Qml.dll', 'Qt6Svg.dll',
    'Qt6QuickTemplates2.dll', 'avcodec-63.dll', 'avutil-61.dll', 'datachannel.dll', 'msvcp140.dll', 'msvcp140_atomic_wait.dll',
    'vcruntime140.dll', 'plugins/platforms/qwindows.dll', 'plugins/tls/qschannelbackend.dll')
$checked = Whole $arm64
foreach ($one in $required | Where-Object { -not $checked.ContainsKey($_) }) {
    $checked[$one] = if ($one -like 'Qt6*' -or $one -like 'plugins/*') { New-GuardedImage $arm64 } elseif ($one -like 'av*') { New-GuardedImage $arm64 -Lacking 'cfg' } else { New-GuardedImage $arm64 -Signed 9 -Functions 10 }
}
$checked['qkvm-desktop.exe'] = New-GuardedImage $arm64 -Signed 1 -Functions 10
$folder = New-Package 'arm64-checked' $checked
foreach ($text in 'qml/QtQuick/qmldir', 'qml/QtQuick/Templates/qmldir', 'qt.conf') {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent (Join-Path $folder $text)) | Out-Null
    Set-Content -LiteralPath (Join-Path $folder $text) -Value 'made up'
}
$refusal = ''
try { & "$PSScriptRoot\test_package.ps1" -PackageDirectory $folder -Arch arm64 | Out-Null } catch { $refusal = "$($_.Exception.Message)" }
Check ($refusal -match 'not built with what its table asks.*qkvm-desktop\.exe lacks pac') "the package checks refuse an ARM64 program without its signed returns ($refusal)"
$auditor = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'audit_release.ps1'))
Check ($auditor -match '\$protectionRefusals = @\(Get-ImageProtectionRefusals \$root \(\[string\] \$manifest\.architecture\)\)' -and
    $auditor -match "id = 'protections'; open = \`$protectionRefusals\.Count -gt 0") 'the release audit holds the package to the table, in a gate of its own'

if ($failures) { throw "$failures image protection check(s) failed." }
Remove-Item -LiteralPath $work -Recurse -Force
Write-Output 'Image protection checks passed: each image is read for ASLR, high entropy, DEP, CFG, the shadow stack, EH continuation and ARM64''s signed returns, and held to its row of the table by the package, its checks, the setup''s build and the audit.'
