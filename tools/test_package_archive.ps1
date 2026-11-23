#Requires -Version 7.2
<#
Checks the ZIP after extraction to a different directory, including its damaged-package cases.
-Evidence names a new file for the record of the checks, written once all of them have passed (build_common.ps1's
New-StartupEvidence): the tools that ran them, by SHA-256 (this script, tools/test_package.ps1, tools/build_common.ps1
and tools/image-protections.json of the folder it runs from, read before the checks), the ZIP by name, size and
SHA-256, its kind, and the kind of Windows they ran on. A release takes its ARM64 ZIP only with such a record made on
Windows on Arm (tools/release_files.ps1 -Arm64Startup, review armp-1 M3), by the tools of its own export (review r2 of
arm and armp, L2): run it from a copy of the export's tools folder, as it is.
#>
param([Parameter(Mandatory)] [string] $Archive, [string] $Evidence = '')

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\build_common.ps1"
if ($Evidence -and (Test-Path -LiteralPath $Evidence)) { throw "$Evidence is there already: the record of the checks is written to a new file." }
# (The tools that are about to run, as they are now.)
$tools = if ($Evidence) { Get-StartupCheckTools $PSScriptRoot }
$desktop = Split-Path -Parent $PSScriptRoot
$place = Join-Path $desktop "build/package-checks/$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $place | Out-Null
[IO.Compression.ZipFile]::ExtractToDirectory((Resolve-Path -LiteralPath $Archive).Path, $place)
$folders = @(Get-ChildItem -LiteralPath $place -Directory)
if ($folders.Count -ne 1) { throw 'The ZIP must contain one app folder.' }
$package = $folders[0].FullName
$prefix = $package + [IO.Path]::DirectorySeparatorChar
$manifest = Get-Content -LiteralPath "$package/package-manifest.json" -Raw | ConvertFrom-Json
$seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($entry in $manifest.files) {
    $file = [IO.Path]::GetFullPath((Join-Path $package $entry.path))
    if (-not $file.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -or -not $seen.Add($file)) {
        throw 'The package manifest contains an invalid or repeated file path.'
    }
    if (-not (Test-Path -LiteralPath $file -PathType Leaf) -or
        (Get-Item -LiteralPath $file).Length -ne $entry.bytes -or
        (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $entry.sha256) {
        throw "Package manifest mismatch: $($entry.path)"
    }
}
$actual = @(Get-ChildItem -LiteralPath $package -Recurse -File)
if ($actual.Count -ne $seen.Count + 1) { throw 'The ZIP has files missing from its manifest.' }
Write-Output "Extracted ZIP: $($seen.Count) file hashes matched."
& "$PSScriptRoot/test_package.ps1" -PackageDirectory $package

function Expect-MissingFileToFail {
    param([string] $RelativePath, [string] $ExpectedMessage)
    $file = [IO.Path]::GetFullPath((Join-Path $package $RelativePath))
    if (-not $file.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Test file leaves its package.' }
    $held = "$file.held-for-test"
    Move-Item -LiteralPath $file -Destination $held
    try {
        $failedAsExpected = $false
        try {
            & "$PSScriptRoot/test_package.ps1" -PackageDirectory $package
        } catch {
            if ($_.Exception.Message -notmatch $ExpectedMessage) { throw }
            $failedAsExpected = $true
            Write-Output "Expected rejection with $RelativePath removed: $($_.Exception.Message)"
        }
        if (-not $failedAsExpected) { throw "The package check missed the missing $RelativePath." }
    } finally {
        Move-Item -LiteralPath $held -Destination $file
    }
}

function Expect-ExtraFileToFail {
    param([string] $RelativePath)
    $file = [IO.Path]::GetFullPath((Join-Path $package $RelativePath))
    if (-not $file.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -or (Test-Path -LiteralPath $file)) {
        throw 'Test file leaves its package or is there already.'
    }
    [IO.File]::WriteAllText($file, 'disposable fixture; never executed')
    try {
        $failedAsExpected = $false
        try {
            & "$PSScriptRoot/test_package.ps1" -PackageDirectory $package
        } catch {
            if ($_.Exception.Message -notmatch 'which is not handed out') { throw }
            $failedAsExpected = $true
            Write-Output "Expected rejection with $RelativePath added: $($_.Exception.Message)"
        }
        if (-not $failedAsExpected) { throw "The package check missed the added $RelativePath." }
    } finally {
        # Only the exact disposable file made above is removed.
        Remove-Item -LiteralPath $file
    }
}

function Expect-ChangedImageToFail {
    param([string] $RelativePath, [int] $At, [byte[]] $Bytes, [string] $What, [string] $ExpectedMessage)
    $file = [IO.Path]::GetFullPath((Join-Path $package $RelativePath))
    if (-not $file.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Test file leaves its package.' }
    $kept = [IO.File]::ReadAllBytes($file)
    $changed = [byte[]] $kept.Clone()
    [Array]::Copy($Bytes, 0, $changed, $At, $Bytes.Length)
    [IO.File]::WriteAllBytes($file, $changed)
    try {
        $failedAsExpected = $false
        try {
            & "$PSScriptRoot/test_package.ps1" -PackageDirectory $package
        } catch {
            if ($_.Exception.Message -notmatch $ExpectedMessage) { throw }
            $failedAsExpected = $true
            Write-Output "Expected rejection with $RelativePath $What`: $($_.Exception.Message)"
        }
        if (-not $failedAsExpected) { throw "The package check missed $RelativePath $What." }
    } finally {
        [IO.File]::WriteAllBytes($file, $kept)
    }
}

# A program or symbols that are not handed out: setup, a bench program, a PDB.
Expect-ExtraFileToFail 'qkvm-setup.exe'
Expect-ExtraFileToFail 'qkvm-bench.exe'
Expect-ExtraFileToFail 'qkvm-desktop.pdb'
Expect-MissingFileToFail 'Qt6Quick.dll' 'Qt6Quick.dll is missing'
# This library is not on the preflight list: the running application must catch it through QML.
Expect-MissingFileToFail 'Qt6QuickLayouts.dll' 'failed its startup check'
# A library of the package's own made into one of the other kind (its header's machine), and one without its CFG (its
# characteristics' GUARD_CF): each refused before anything starts (review arm 1, L3 and M1).
$library = [IO.File]::ReadAllBytes((Join-Path $package 'avutil-61.dll'))
$other = if ([string] $manifest.architecture -eq 'arm64') { [uint16] 0x8664 } else { [uint16] 0xAA64 }
Expect-ChangedImageToFail 'avutil-61.dll' ([BitConverter]::ToInt32($library, 0x3C) + 4) ([BitConverter]::GetBytes($other)) 'made of the other kind' 'holds an image that is not for'
$qt = [IO.File]::ReadAllBytes((Join-Path $package 'Qt6Core.dll'))
$characteristicsAt = [BitConverter]::ToInt32($qt, 0x3C) + 24 + 70
$withoutCfg = [uint16] ([BitConverter]::ToUInt16($qt, $characteristicsAt) -band 0xBFFF)
Expect-ChangedImageToFail 'Qt6Core.dll' $characteristicsAt ([BitConverter]::GetBytes($withoutCfg)) 'without CFG' 'not built with what its table asks.*Qt6Core\.dll lacks cfg'
& "$PSScriptRoot/test_package.ps1" -PackageDirectory $package
Write-Output "Relocated startup and damaged-package checks passed. Evidence folder: $place"
if ($Evidence) {
    $made = New-StartupEvidence -Zip $Archive -Manifest $manifest -Tools $tools
    $made | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $Evidence -Encoding utf8
    Write-Output "The record of these checks: $Evidence ($($made.architecture) package, on $($made.windows.machine) Windows; SHA-256 of the ZIP $($made.sha256); made by the tools in $PSScriptRoot, $(@($tools.Keys | ForEach-Object { "$_ $($tools[$_].Substring(0, 12))" }) -join ', '))"
}
