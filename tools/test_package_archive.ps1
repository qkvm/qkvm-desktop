#Requires -Version 7.2
<# Checks the ZIP after extraction to a different directory, including two damaged-package cases. #>
param([Parameter(Mandatory)] [string] $Archive)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
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

# A program or symbols that are not handed out: setup, a bench program, a PDB.
Expect-ExtraFileToFail 'qkvm-setup.exe'
Expect-ExtraFileToFail 'qkvm-bench.exe'
Expect-ExtraFileToFail 'qkvm-desktop.pdb'
Expect-MissingFileToFail 'Qt6Quick.dll' 'Qt6Quick.dll is missing'
# This library is not on the preflight list: the running application must catch it through QML.
Expect-MissingFileToFail 'Qt6QuickLayouts.dll' 'failed its startup check'
& "$PSScriptRoot/test_package.ps1" -PackageDirectory $package
Write-Output "Relocated startup and damaged-package checks passed. Evidence folder: $place"
