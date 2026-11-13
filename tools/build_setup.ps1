#Requires -Version 7.2
<#
.SYNOPSIS
Makes the program's setup from one package folder: Inno Setup, with exactly the package's files.

.DESCRIPTION
    pwsh -File tools\build_setup.ps1 -PackageDirectory <package folder> -Inno <Inno Setup's folder> -OutputDirectory <new folder> [-ScanTwin]

The package folder is one tools/package.ps1 made. Its package-manifest.json names every file in it with its size and
SHA-256: each is checked, and the folder must hold those files and the manifest and nothing else, with nothing
redirected in it. Each name must be one Inno Setup takes as it is: letters, digits, blanks and . _ + - only, in
folders of such names (a brace would be read as one of its constants, a quote would end a name). The Inno Setup
must be the one tools/fetch_inno.ps1 unpacks, by its tree's digest.

In <OutputDirectory>\inputs: packaging/qkvm-desktop.iss as committed, files.iss (one [Files] line for each file of the
package, at its own place under the program's folder, and one for the manifest), kept.iss (the package's files under
plugins and qml, the only ones the setup keeps in those two folders after a whole copy), folders.iss (the package's
folders, the only ones where the setup looks for what a setup ended halfway left), the marker of the update
contract (qkvm-desktop.installed: "setup" and a line feed) and the program's icon. Inno Setup's compiler makes
qKVM-Desktop-<version>-windows-x64-setup.exe from them in <OutputDirectory>, with its words in compile.txt and its
own list of every file it put in, and the SHA-256 of each, in setup-files.txt. That list must be the package's
manifest, the manifest itself and the marker: nothing more and nothing less. The setup's own manifest must ask for
nothing more than the user who runs it (asInvoker). setup-record.json says what went in and what came out.

-ScanTwin makes the same setup again with nothing compressed, in <OutputDirectory>\scan, for the byte scan alone:
inside the setup the files and its own words are compressed, where no byte scan sees them; in the twin they are as
they are. It holds the same files (by the compiler's list) and is never handed out.

Nothing is run but the compiler: the setup itself is never started here.
#>
param(
    [Parameter(Mandatory)] [string] $PackageDirectory,
    [Parameter(Mandatory)] [string] $Inno,
    [Parameter(Mandatory)] [string] $OutputDirectory,
    [switch] $ScanTwin
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\build_common.ps1"
$desktop = Split-Path -Parent $PSScriptRoot

$package = (Resolve-Path -LiteralPath $PackageDirectory).Path.TrimEnd('\')
$Inno = (Resolve-Path -LiteralPath $Inno).Path.TrimEnd('\')
if ((Get-TreeDigest $Inno) -cne $InnoTreeDigest -or @(Get-ChildItem -LiteralPath $Inno -Recurse -File -Force).Count -ne $InnoTreeFiles) {
    throw "$Inno is not the pinned Inno Setup $InnoVersion (tools/fetch_inno.ps1 unpacks it)."
}

# The package: its manifest, every file it names as it names it, and nothing else.
$manifestFile = Join-Path $package 'package-manifest.json'
if (-not (Test-Path -LiteralPath $manifestFile -PathType Leaf)) { throw "$package has no package-manifest.json." }
$manifest = Get-Content -LiteralPath $manifestFile -Raw | ConvertFrom-Json
$version = [string] $manifest.version
if ($manifest.product -cne 'qKVM' -or $manifest.architecture -cne 'x64' -or $version -cnotmatch '\A\d+\.\d+\.\d+\z' -or -not @($manifest.files).Count) {
    throw 'The package manifest is not one tools/package.ps1 writes.'
}
$plain = '\A[A-Za-z0-9_][A-Za-z0-9._+ -]*\z'
$named = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$entries = @($manifest.files | Sort-Object { [string] $_.path } -CaseSensitive)
foreach ($entry in $entries) {
    $path = [string] $entry.path
    if (@($path.Split('/') | Where-Object { $_ -cnotmatch $plain -or $_.EndsWith('.') -or $_.EndsWith(' ') }).Count) {
        throw "A file of the package cannot be named to Inno Setup as it is: $path"
    }
    if ($path -in 'package-manifest.json', 'qkvm-desktop.installed' -or -not $named.Add($path)) {
        throw "The package manifest names $path twice, or a file the setup makes itself."
    }
    $file = Join-Path $package ($path -replace '/', '\')
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "The package lacks $path, which its manifest names." }
    if ((Get-Item -LiteralPath $file).Length -ne [long] $entry.bytes -or (Get-Sha256 $file) -cne [string] $entry.sha256) {
        throw "The package holds a file that is not the bytes its manifest names: $path"
    }
}
foreach ($item in Get-ChildItem -LiteralPath $package -Recurse -Force) {
    $relative = [IO.Path]::GetRelativePath($package, $item.FullName).Replace('\', '/')
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "The package holds a redirected entry: $relative" }
    if (-not $item.PSIsContainer -and $relative -cne 'package-manifest.json' -and -not $named.Contains($relative)) {
        throw "The package holds a file that is not in its manifest: $relative"
    }
}

# The output folder: new, or empty, and apart from the package.
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\')
if ((Test-Path -LiteralPath $OutputDirectory) -and @(Get-ChildItem -LiteralPath $OutputDirectory -Force).Count) {
    throw "$OutputDirectory is not empty: the setup is made in a new folder."
}
if ($OutputDirectory.StartsWith("$package\", [StringComparison]::OrdinalIgnoreCase) -or $OutputDirectory -ieq $package) {
    throw 'The setup is made apart from the package it carries.'
}
$out = (New-Item -ItemType Directory -Force -Path $OutputDirectory).FullName
$inputs = (New-Item -ItemType Directory -Path (Join-Path $out 'inputs')).FullName
Copy-Item -LiteralPath (Join-Path $desktop 'packaging/qkvm-desktop.iss') -Destination $inputs
Copy-Item -LiteralPath (Join-Path $desktop 'ui/brand/qkvm.ico') -Destination $inputs
$markerBytes = [byte[]] @(0x73, 0x65, 0x74, 0x75, 0x70, 0x0A)
$marker = Join-Path $inputs 'qkvm-desktop.installed'
[IO.File]::WriteAllBytes($marker, $markerBytes)
# (In ordinal order, and the program's own file last: a start of it during the copy finds it beside the libraries it
# was built with, never a new program beside an earlier version's libraries.)
if (-not $named.Contains('qkvm-desktop.exe')) { throw 'The package has no qkvm-desktop.exe.' }
$order = [string[]] @(@($entries | ForEach-Object { [string] $_.path }) + 'package-manifest.json' | Where-Object { $_ -cne 'qkvm-desktop.exe' })
[Array]::Sort($order, [StringComparer]::Ordinal)
$lines = foreach ($path in @($order) + 'qkvm-desktop.exe') {
    $relative = $path -replace '/', '\'
    $folder = Split-Path -Parent $relative
    'Source: "{0}"; DestDir: "{{app}}{1}"; Flags: ignoreversion' -f $relative, $(if ($folder) { "\$folder" } else { '' })
}
Set-Content -LiteralPath (Join-Path $inputs 'files.iss') -Value $lines -Encoding ascii
# kept.iss: the package's own files under plugins and qml, which the setup keeps there after a whole copy (whatever
# else an earlier version left in those two folders it removes then). From the manifest, never from the folder.
$kept = @('// Made by tools/build_setup.ps1 from package-manifest.json: the files of the package under plugins and qml, each',
    '// between two | and in small letters, as RemoveStale compares them.', 'function KeptFiles: String;', 'begin', "  Result := '|';")
foreach ($path in @($entries | ForEach-Object { [string] $_.path } | Where-Object { $_ -cmatch '\A(plugins|qml)/' })) {
    $kept += "  Result := Result + '$(($path -replace '/', '\').ToLowerInvariant())|';"
}
$kept += 'end;'
Set-Content -LiteralPath (Join-Path $inputs 'kept.iss') -Value $kept -Encoding ascii
# folders.iss: the package's own folders, the program's folder itself as an empty name, where Inno Setup writes its
# temporary files and where the setup looks for what a setup ended halfway left. From the manifest, never from the
# folder, so that a program's folder chosen where other programs live is not walked.
$packageFolders = [Collections.Generic.SortedSet[string]]::new([StringComparer]::Ordinal)
[void] $packageFolders.Add('')
foreach ($entry in $entries) { [void] $packageFolders.Add((Split-Path -Parent (([string] $entry.path) -replace '/', '\'))) }
$folderLines = @('// Made by tools/build_setup.ps1 from package-manifest.json: the folders of the package, the program''s folder',
    '// itself first as an empty name, each followed by |, as RemoveSetupLeftovers walks them.', 'function PackageFolders: String;',
    'begin', "  Result := '|';")
foreach ($folder in $packageFolders) { $folderLines += "  Result := Result + '$folder|';" }
$folderLines += 'end;'
Set-Content -LiteralPath (Join-Path $inputs 'folders.iss') -Value $folderLines -Encoding ascii

# What the compiler is expected to put in: by where it is read from, its size and SHA-256.
$expected = @{}
foreach ($entry in $entries) {
    $expected[(Join-Path $package (([string] $entry.path) -replace '/', '\')).ToLowerInvariant()] = @{ Path = [string] $entry.path; Bytes = [long] $entry.bytes; Sha256 = [string] $entry.sha256 }
}
$expected[$manifestFile.ToLowerInvariant()] = @{ Path = 'package-manifest.json'; Bytes = (Get-Item -LiteralPath $manifestFile).Length; Sha256 = Get-Sha256 $manifestFile }
$expected[$marker.ToLowerInvariant()] = @{ Path = 'qkvm-desktop.installed'; Bytes = $markerBytes.Length; Sha256 = Get-Sha256 $marker }

$iscc = Join-Path $Inno 'ISCC.exe'
# (The bytes of every file it puts in: in update mode the setup asks for that much room before it changes anything.)
$packageBytes = [long] 0
foreach ($wanted in $expected.Values) { $packageBytes += $wanted.Bytes }
$name = Get-SetupName $version
function Invoke-Compile([string] $Into, [string[]] $Defines = @()) {
    <# Compiles the inputs into $Into, and checks the setup it made against what was expected. Its SHA-256. #>
    $said = & $iscc "/O$Into" "/DAppVersion=$version" "/DPackageDir=$package" "/DPackageBytes=$packageBytes" @Defines (Join-Path $inputs 'qkvm-desktop.iss') 2>&1 | ForEach-Object { "$_" }
    $ended = $LASTEXITCODE
    Set-Content -LiteralPath (Join-Path $Into 'compile.txt') -Value $said -Encoding utf8
    if ($ended -ne 0) { throw "Inno Setup's compiler failed (exit $ended): $((@($said) | Select-Object -Last 6) -join ' | ')" }
    $setup = Join-Path $Into $name
    if (-not (Test-Path -LiteralPath $setup -PathType Leaf)) { throw "The compiler made no $name." }
    $seen = @{}
    foreach ($row in @(Get-Content -LiteralPath (Join-Path $Into 'setup-files.txt') | ConvertFrom-Csv -Delimiter "`t")) {
        $source = ([string] $row.SourceFilename).ToLowerInvariant()
        if (-not $expected.ContainsKey($source)) { throw "The compiler put in a file that is not the package's: $($row.SourceFilename)" }
        $wanted = $expected[$source]
        if ($seen.ContainsKey($source)) { throw "The compiler put $($wanted.Path) in twice." }
        $seen[$source] = $true
        if ([long] $row.OriginalSize -ne $wanted.Bytes -or [string] $row.SHA256Sum -cne $wanted.Sha256 -or [string] $row.Encrypted -cne 'no') {
            throw "The compiler put in other bytes than the package's manifest names for $($wanted.Path)."
        }
    }
    foreach ($source in $expected.Keys | Where-Object { -not $seen.ContainsKey($_) }) { throw "The compiler left out $($expected[$source].Path)." }
    $level = [regex]::Match((Get-PeManifest $setup), 'requestedExecutionLevel\s+level="([^"]+)"').Groups[1].Value
    if ($level -cne 'asInvoker') { throw "The setup's own manifest asks to run as $level, not as whoever runs it." }
    return Get-Sha256 $setup
}

$sha256 = Invoke-Compile $out
$twin = $null
if ($ScanTwin) {
    $scan = (New-Item -ItemType Directory -Path (Join-Path $out 'scan')).FullName
    $twinSha256 = Invoke-Compile $scan @('/DScanTwin')
    $twin = [ordered]@{ name = "scan/$name"; bytes = (Get-Item -LiteralPath (Join-Path $scan $name)).Length; sha256 = $twinSha256 }
}
$setup = Join-Path $out $name
[ordered]@{
    schema = 1
    version = $version
    setup = [ordered]@{ name = $name; bytes = (Get-Item -LiteralPath $setup).Length; sha256 = $sha256
        signature = [string] (Get-AuthenticodeSignature -LiteralPath $setup).Status; manifestLevel = 'asInvoker' }
    package = [ordered]@{ folder = Split-Path -Leaf $package; manifestSha256 = Get-Sha256 $manifestFile; files = $entries.Count; bytes = $packageBytes }
    inno = [ordered]@{ version = $InnoVersion; tree = $InnoTreeDigest }
    script = [ordered]@{ sha256 = Get-Sha256 (Join-Path $inputs 'qkvm-desktop.iss'); files = Get-Sha256 (Join-Path $inputs 'files.iss') }
    list = Get-Sha256 (Join-Path $out 'setup-files.txt')
    scanTwin = $twin
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $out 'setup-record.json') -Encoding utf8
Write-Output "Setup: $setup" "SHA-256: $sha256" "Files: $($entries.Count + 2), the package's $($entries.Count), its manifest and the marker$(if ($twin) { '; and its uncompressed twin for the byte scan' })"
