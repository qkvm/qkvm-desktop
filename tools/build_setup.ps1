#Requires -Version 7.2
<#
.SYNOPSIS
Makes the program's setup: Inno Setup, with exactly the files of the packages it carries, x64's and Windows on Arm's,
and installs the PC's own.

.DESCRIPTION
    pwsh -File tools\build_setup.ps1 -PackageDirectory <x64 package>[,<arm64 package>] -Inno <Inno Setup's folder> -OutputDirectory <new folder> -UpdateCheck <x64 qkvm-update-check.exe>[,<arm64 one>] [-ScanTwin] [-PublishedFiles <list>]

One setup for both kinds of Windows (the owner, 2026-11-19: a person on Windows on Arm ends up with the ARM64 program
by every road): it carries x64's package and Windows on Arm's, and installs the ARM64 program where Windows is on Arm
(Inno Setup's IsArm64) and the x64 one everywhere else, wherever Windows runs x64 programs (x64compatible). A setup
of x64's package alone (a development build's: the gate's, before an ARM64 package is made there) installs on x64
Windows alone (x64os) and refuses Windows on Arm, as 0.1.0 to 0.1.2's did; no release carries one
(tools/release_files.ps1). Each package's update check is the build's own of its kind (-UpdateCheck, one for each
package, matched by their PE machine), unpacked on the PC of that kind alone. The same version in both packages.

Each package folder is one tools/package.ps1 made. Its package-manifest.json names every file in it with its size and
SHA-256: each is checked, and the folder must hold those files and the manifest and nothing else, with nothing
redirected in it; its architecture is x64 or arm64, and every program and library in it is a Windows image of that
kind. Each name must be one Inno Setup takes as it is: letters, digits, blanks and . _ + - only, in
folders of such names (a brace would be read as one of its constants, a quote would end a name). The Inno Setup
must be the one tools/fetch_inno.ps1 unpacks, by its tree's digest.

In <OutputDirectory>\inputs: packaging/qkvm-desktop.iss as committed, files.iss (one [Files] line for each file of
each package, at its own place under the program's folder, and one for each manifest, x64's with Check: not IsArm64
and Windows on Arm's with Check: IsArm64; x64's from its folder, named /DPackageDir, Windows on Arm's from its own,
/DArm64PackageDir), and, for each kind of Windows (x64, then Arm64; the functions of a kind the setup does not carry
say nothing): folders.iss (the package's folders, the only ones where the setup looks for what a setup ended halfway
left: PackageFoldersX64, PackageFoldersArm64), package.iss (every file of the package's, the program's own first: the
only ones the setup sets aside before its copy; and the size and SHA-256 of each, by which it takes a copy for whole:
PackageFilesX64 and the rest), earlier.iss (every file an earlier published version installed, of either kind, and
the package of this kind has not, which the setup takes for its own in a record of what an update set aside: the same
version's other package's files among them; and for each such version and kind, what it installed and this package
has not, which the setup removes after a whole copy, by its exact name, where that version of that kind was installed
in that very folder, and their folders, removed where that leaves them empty; from packaging/published-files.json, or
the list -PublishedFiles names, by the versions before this package's, and the other package of this version), the
marker of the update
contract (qkvm-desktop.installed: "setup" and a line feed), qKVM Setup's update checks (qkvm-update-check.exe, and
qkvm-update-check-arm64.exe where Windows on Arm's package is carried: the programs -UpdateCheck names, the build's
own of ui/win/update_check.cpp, which the setup carries and puts at the program's name while the earlier version is set
aside; each a Windows program of its package's kind, at most 4 MiB, outside the packages, and built with what
tools/image-protections.json asks of our own programs of that kind, read from its bytes) and the program's icon. Inno
Setup's compiler makes qKVM-Desktop-<version>-windows-setup.exe from them in <OutputDirectory>, with its words in
compile.txt and its own list of every file it put in, and the SHA-256 of each, in setup-files.txt. That list must be
each package's manifest, the manifests themselves, the marker and the update checks: nothing more and nothing less.
The setup's own manifest must ask for nothing more than the user who runs it (asInvoker). setup-record.json says what
went in and what came out, and which kinds of Windows the setup carries (architectures).

-ScanTwin makes the same setup again with nothing compressed, in <OutputDirectory>\scan, for the byte scan alone:
inside the setup the files and its own words are compressed, where no byte scan sees them; in the twin they are as
they are. It holds the same files (by the compiler's list) and is never handed out.

Nothing is run but the compiler: the setup itself is never started here.
#>
param(
    [Parameter(Mandatory)] [string[]] $PackageDirectory,
    [Parameter(Mandatory)] [string] $Inno,
    [Parameter(Mandatory)] [string] $OutputDirectory,
    [Parameter(Mandatory)] [string[]] $UpdateCheck,
    [switch] $ScanTwin,
    [string] $PublishedFiles = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\build_common.ps1"
. "$PSScriptRoot\release_common.ps1"
$desktop = Split-Path -Parent $PSScriptRoot
$PackageDirectory = @($PackageDirectory | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$UpdateCheck = @($UpdateCheck | ForEach-Object { $_ -split ',' } | Where-Object { $_ })

$Inno = (Resolve-Path -LiteralPath $Inno).Path.TrimEnd('\')
if ((Get-TreeDigest $Inno) -cne $InnoTreeDigest -or @(Get-ChildItem -LiteralPath $Inno -Recurse -File -Force).Count -ne $InnoTreeFiles) {
    throw "$Inno is not the pinned Inno Setup $InnoVersion (tools/fetch_inno.ps1 unpacks it)."
}

# Each package: its manifest, every file it names as it names it, and nothing else; of one kind of Windows, which every
# Windows image in it is for. x64's first, then Windows on Arm's.
$plain = '\A[A-Za-z0-9_][A-Za-z0-9._+ -]*\z'
$packages = [ordered]@{}
if (@($PackageDirectory).Count -lt 1 -or @($PackageDirectory).Count -gt 2) { throw 'Name x64''s package, and Windows on Arm''s where the setup carries it.' }
foreach ($folderGiven in $PackageDirectory) {
    $package = (Resolve-Path -LiteralPath $folderGiven).Path.TrimEnd('\')
    $manifestFile = Join-Path $package 'package-manifest.json'
    if (-not (Test-Path -LiteralPath $manifestFile -PathType Leaf)) { throw "$package has no package-manifest.json." }
    $manifest = Get-Content -LiteralPath $manifestFile -Raw | ConvertFrom-Json
    $kind = [string] $manifest.architecture
    if ($manifest.product -cne 'qKVM' -or @('x64', 'arm64') -cnotcontains $kind -or [string] $manifest.version -cnotmatch '\A\d+\.\d+\.\d+\z' -or -not @($manifest.files).Count) {
        throw 'The package manifest is not one tools/package.ps1 writes.'
    }
    if ($packages.Contains($kind)) { throw "Two packages of $kind are named: a setup carries one of each kind." }
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
        if ($path -match '\.(exe|dll)\z') {
            $image = Get-PeArchitecture ([IO.File]::ReadAllBytes($file))
            if ($image -cne 'no Windows program' -and $image -cne $kind) { throw "The package for $kind Windows holds a program for ${image}: $path" }
        }
    }
    foreach ($item in Get-ChildItem -LiteralPath $package -Recurse -Force) {
        $relative = [IO.Path]::GetRelativePath($package, $item.FullName).Replace('\', '/')
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "The package holds a redirected entry: $relative" }
        if (-not $item.PSIsContainer -and $relative -cne 'package-manifest.json' -and -not $named.Contains($relative)) {
            throw "The package holds a file that is not in its manifest: $relative"
        }
    }
    if (-not $named.Contains('qkvm-desktop.exe')) { throw 'The package has no qkvm-desktop.exe.' }
    $packages[$kind] = [pscustomobject]@{ Kind = $kind; Folder = $package; ManifestFile = $manifestFile; Manifest = $manifest
        Version = [string] $manifest.version; Entries = $entries; Named = $named }
}
if (-not $packages.Contains('x64')) { throw 'The setup carries x64''s package always: name it.' }
$kinds = @(@('x64', 'arm64') | Where-Object { $packages.Contains($_) })
$version = $packages['x64'].Version
if (@($packages.Values | Where-Object { $_.Version -cne $version }).Count) { throw 'The packages are of other versions: a setup carries one version.' }
$arm = $packages.Contains('arm64')

# The output folder: new, or empty, and apart from the packages.
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\')
if ((Test-Path -LiteralPath $OutputDirectory) -and @(Get-ChildItem -LiteralPath $OutputDirectory -Force).Count) {
    throw "$OutputDirectory is not empty: the setup is made in a new folder."
}
foreach ($one in $packages.Values) {
    if ($OutputDirectory.StartsWith("$($one.Folder)\", [StringComparison]::OrdinalIgnoreCase) -or $OutputDirectory -ieq $one.Folder -or
        $one.Folder.StartsWith("$OutputDirectory\", [StringComparison]::OrdinalIgnoreCase)) {
        throw 'The setup is made apart from the packages it carries.'
    }
}
$out = (New-Item -ItemType Directory -Force -Path $OutputDirectory).FullName
$inputs = (New-Item -ItemType Directory -Path (Join-Path $out 'inputs')).FullName
Copy-Item -LiteralPath (Join-Path $desktop 'packaging/qkvm-desktop.iss') -Destination $inputs
Copy-Item -LiteralPath (Join-Path $desktop 'ui/brand/qkvm.ico') -Destination $inputs
$markerBytes = [byte[]] @(0x73, 0x65, 0x74, 0x75, 0x70, 0x0A)
$marker = Join-Path $inputs 'qkvm-desktop.installed'
[IO.File]::WriteAllBytes($marker, $markerBytes)
# qKVM Setup's update checks, one for each package and of its kind: each a Windows program (MZ) of that kind, at most
# 4 MiB, a plain file outside the packages, built with what tools/image-protections.json asks of our own programs of
# that kind; carried as they are, x64's as qkvm-update-check.exe and Windows on Arm's as qkvm-update-check-arm64.exe.
if (@($UpdateCheck).Count -ne $kinds.Count) { throw "Name one update check for each package ($($kinds.Count))." }
$checks = [ordered]@{}
foreach ($given in $UpdateCheck) {
    $check = Get-Item -LiteralPath $given -Force -ErrorAction SilentlyContinue
    if (-not $check -or $check.PSIsContainer -or ($check.Attributes -band [IO.FileAttributes]::ReparsePoint) -or $check.Length -gt 4MB -or $check.Length -lt 2) {
        throw "$given is not a file of qKVM Setup's update check."
    }
    $checkBytes = [IO.File]::ReadAllBytes($check.FullName)
    if ($checkBytes[0] -ne 0x4D -or $checkBytes[1] -ne 0x5A) { throw "$given is not a Windows program." }
    foreach ($one in $packages.Values) {
        if ($check.FullName.StartsWith("$($one.Folder)\", [StringComparison]::OrdinalIgnoreCase)) { throw 'The update check is not one of the package''s files.' }
    }
    $kind = Get-PeArchitecture $checkBytes
    if (-not $packages.Contains($kind)) { throw "$given is an update check for $kind Windows, and the setup carries no package of that kind." }
    if ($checks.Contains($kind)) { throw "Two update checks for $kind are named." }
    $name = if ($kind -ceq 'arm64') { 'qkvm-update-check-arm64.exe' } else { 'qkvm-update-check.exe' }
    # (Built with what tools/image-protections.json asks of our own programs of its kind, by the name it is carried under,
    # read from its bytes, as every image of a package is: it runs on users' PCs. On ARM64 nearly all of its functions
    # are the static runtime's, which sign their returns themselves (267 of 310 signed in the rehearsal's build of
    # 2026-11-22, of a source of two functions), so its share passes whether or not its own code was built with
    # /guard:signret: its row's cfg, which qkvm_build gives with that switch, is what tells; review r2 of arm and armp, L1.)
    $unprotected = @(Get-ImageFileProtectionRefusals -File $check.FullName -Name $name -Arch $kind)
    if ($unprotected.Count) { throw "$given is not built with what its table asks: $($unprotected -join '; '). Build it with the release preset of its kind (qkvm_build's switches), as tools/build_release.ps1 does." }
    $file = Join-Path $inputs $name
    [IO.File]::WriteAllBytes($file, $checkBytes)
    $checks[$kind] = [pscustomobject]@{ Kind = $kind; Name = $name; File = $file; Bytes = $checkBytes.Length; Sha256 = Get-Sha256 $file }
}

# files.iss: each package's files, each at its own place, in ordinal order and its program's own file last (a start of
# it during the copy finds it beside the libraries it was built with, never a new program beside an earlier version's
# libraries); x64's for every PC but one on Arm, Windows on Arm's for one on Arm: so the two programs' files last of all.
$orderOf = @{}
foreach ($one in $packages.Values) {
    $order = [string[]] @(@($one.Entries | ForEach-Object { [string] $_.path }) + 'package-manifest.json' | Where-Object { $_ -cne 'qkvm-desktop.exe' })
    [Array]::Sort($order, [StringComparer]::Ordinal)
    $orderOf[$one.Kind] = $order
}
function Get-FileLine([string] $Kind, [string] $Path) {
    $relative = $Path -replace '/', '\'
    $folder = Split-Path -Parent $relative
    $source = if ($Kind -ceq 'arm64') { "{#Arm64PackageDir}\$relative" } else { $relative }
    $check = if ($Kind -ceq 'arm64') { 'IsArm64' } else { 'not IsArm64' }
    return 'Source: "{0}"; DestDir: "{{app}}{1}"; Flags: ignoreversion; Check: {2}' -f $source, $(if ($folder) { "\$folder" } else { '' }), $check
}
$lines = @(foreach ($kind in $kinds) { foreach ($path in $orderOf[$kind]) { Get-FileLine $kind $path } }) +
    @(foreach ($kind in $kinds) { Get-FileLine $kind 'qkvm-desktop.exe' })
Set-Content -LiteralPath (Join-Path $inputs 'files.iss') -Value $lines -Encoding ascii
# The function names of each kind (Inno Setup's IsArm64 chooses between them, packaging/qkvm-desktop.iss).
$suffixOf = @{ 'x64' = 'X64'; 'arm64' = 'Arm64' }
# folders.iss: each package's own folders, the program's folder itself as an empty name, where Inno Setup writes its
# temporary files and where the setup looks for what a setup ended halfway left. From the manifest, never from the
# folder, so that a program's folder chosen where other programs live is not walked. (A kind not carried: none.)
$folderLines = @('// Made by tools/build_setup.ps1 from each package-manifest.json: the folders of the package of each kind of Windows,',
    '// the program''s folder itself first as an empty name, each followed by |, as RemoveSetupLeftovers walks them.')
foreach ($kind in 'x64', 'arm64') {
    $folderLines += @("function PackageFolders$($suffixOf[$kind]): String;", 'begin', "  Result := '|';")
    if ($packages.Contains($kind)) {
        $packageFolders = [Collections.Generic.SortedSet[string]]::new([StringComparer]::Ordinal)
        [void] $packageFolders.Add('')
        foreach ($entry in $packages[$kind].Entries) { [void] $packageFolders.Add((Split-Path -Parent (([string] $entry.path) -replace '/', '\'))) }
        foreach ($folder in $packageFolders) { $folderLines += "  Result := Result + '$folder|';" }
    }
    $folderLines += @('end;', '')
}
Set-Content -LiteralPath (Join-Path $inputs 'folders.iss') -Value $folderLines -Encoding ascii
# package.iss: every file the setup puts in under the program's folder for each kind (files.iss's of that kind, but for
# the marker of the contract), the program's own first and then in files.iss's order: what the setup sets aside before
# its copy and puts back where the copy does not complete, and nothing else; and the bytes and the SHA-256 of each, in
# the same order, as the manifest gives them and the manifest's own (after a copy, the setup takes it for whole only
# where every file is there at its bytes, review i020-1 M1, by its SHA-256 too, review i020-2 L7). From the manifests,
# never from the folder.
$packageLines = @('// Made by tools/build_setup.ps1 from each package-manifest.json: every file the setup puts in under the program''s',
    '// folder for each kind of Windows, the program''s own first, each followed by |, as SetPackageAside sets them aside and',
    '// PutRight puts them back; and their bytes and SHA-256, in the same order.')
foreach ($kind in 'x64', 'arm64') {
    $names = @(); $sizes = @(); $sums = @()
    if ($packages.Contains($kind)) {
        $one = $packages[$kind]
        $bytesOf = @{}
        $sumOf = @{}
        foreach ($entry in $one.Entries) { $bytesOf[[string] $entry.path] = [long] $entry.bytes; $sumOf[[string] $entry.path] = ([string] $entry.sha256).ToLowerInvariant() }
        $bytesOf['package-manifest.json'] = (Get-Item -LiteralPath $one.ManifestFile).Length
        $sumOf['package-manifest.json'] = Get-Sha256 $one.ManifestFile
        foreach ($path in @('qkvm-desktop.exe') + @($orderOf[$kind])) {
            $names += $path -replace '/', '\'; $sizes += $bytesOf[$path]; $sums += $sumOf[$path]
        }
    }
    foreach ($list in @(@('PackageFiles', $names), @('PackageFileBytes', $sizes), @('PackageFileSha256', $sums))) {
        $packageLines += @('', "function $($list[0])$($suffixOf[$kind]): String;", 'begin', "  Result := '|';")
        foreach ($item in @($list[1])) { $packageLines += "  Result := Result + '$item|';" }
        $packageLines += 'end;'
    }
}
Set-Content -LiteralPath (Join-Path $inputs 'package.iss') -Value $packageLines -Encoding ascii
# earlier.iss: for each kind of Windows the setup installs, what versions of ours installed before (a licence, a library
# that went) and its package has not, by every version before this one in the committed list of what each published
# version installed (packaging/published-files.json: each version's published ZIP of each kind, the very package its
# setup was made from), and this version's package of the other kind (a folder that held x64's program and now takes
# Windows on Arm's, or the other way round). All of them once (EarlierFiles<kind>): names the setup takes for its own in
# a record of what an update set aside. And each version's own of each kind (EarlierFilesOf<kind>), with its folders, the
# deepest first, never the program's folder itself (EarlierFoldersOf<kind>): the setup removes each such file by its
# exact name after a whole copy, where that very version of that very kind was installed in that very folder (review
# i020-1, M4), and each such folder where that leaves it empty: never a pattern, never a walk.
if (-not $PublishedFiles) { $PublishedFiles = Join-Path $desktop 'packaging/published-files.json' }
$published = Get-Content -LiteralPath $PublishedFiles -Raw | ConvertFrom-Json
if ($published.schema -ne 2 -or -not @($published.versions).Count) { throw "$PublishedFiles is not a list of what each published version installed (schema 2)." }
$thisVersion = [version] $version
$seenVersions = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
# (What each version of each kind installed, by "<version> <kind>": the published versions before this one, and this
# version's packages as this setup carries them.)
$installedBy = [ordered]@{}
foreach ($release in @($published.versions)) {
    if ([string] $release.version -cnotmatch '\A\d+\.\d+\.\d+\z' -or -not @($release.builds).Count) { throw "$PublishedFiles has a version that is not one: $($release.version)" }
    if (-not $seenVersions.Add([string] $release.version)) { throw "$PublishedFiles names version $($release.version) twice." }
    $seenKinds = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($build in @($release.builds)) {
        if (@('x64', 'arm64') -cnotcontains [string] $build.architecture -or -not $seenKinds.Add([string] $build.architecture) -or -not @($build.files).Count) {
            throw "$PublishedFiles has a build of $($release.version) that is not one of x64 or arm64, or names one twice."
        }
        foreach ($path in @($build.files | ForEach-Object { [string] $_ })) {
            if (@($path.Split('/') | Where-Object { $_ -cnotmatch $plain -or $_.EndsWith('.') -or $_.EndsWith(' ') }).Count) {
                throw "$PublishedFiles names a file that cannot be named to Inno Setup as it is: $path"
            }
        }
        if ([version] [string] $release.version -lt $thisVersion) {
            $installedBy["$($release.version) $($build.architecture)"] = @($build.files | ForEach-Object { [string] $_ })
        }
    }
}
foreach ($one in $packages.Values) { $installedBy["$version $($one.Kind)"] = @($one.Entries | ForEach-Object { [string] $_.path }) }
$earlierLists = @{}
foreach ($kind in 'x64', 'arm64') {
    $earlier = [Collections.Generic.SortedSet[string]]::new([StringComparer]::Ordinal)
    $earlierNamed = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $earlierOf = [ordered]@{}
    if ($packages.Contains($kind)) {
        $mine = $packages[$kind].Named
        foreach ($at in $installedBy.Keys) {
            # (This version of this very kind is this package: nothing of it is an earlier one's.)
            if ($at -ceq "$version $kind") { continue }
            $own = [Collections.Generic.SortedSet[string]]::new([StringComparer]::Ordinal)
            $ownNamed = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
            foreach ($path in @($installedBy[$at])) {
                if ($path -in 'package-manifest.json', 'qkvm-desktop.installed' -or $mine.Contains($path)) { continue }
                if ($ownNamed.Add($path)) { [void] $own.Add($path) }
                if ($earlierNamed.Add($path)) { [void] $earlier.Add($path) }
            }
            $earlierOf[$at] = $own
        }
    }
    $earlierLists[$kind] = [pscustomobject]@{ All = $earlier; Of = $earlierOf }
}
function Get-FoldersBackwards($Names) {
    <# The folders of the names, a folder after every folder within it (ordinal order backwards), never the program's. #>
    $folders = [Collections.Generic.SortedSet[string]]::new([StringComparer]::Ordinal)
    foreach ($path in $Names) {
        $folder = Split-Path -Parent ($path -replace '/', '\')
        while ($folder) { [void] $folders.Add($folder); $folder = Split-Path -Parent $folder }
    }
    $backwards = [string[]] @($folders)
    [Array]::Sort($backwards, [StringComparer]::Ordinal)
    [Array]::Reverse($backwards)
    return , $backwards
}
$earlierLines = @('// Made by tools/build_setup.ps1 from packaging/published-files.json and the packages: for each kind of Windows, every',
    '// file a version of ours installed before this package (an earlier one of either kind, or this one of the other) and',
    '// this package has not, each followed by |, which a record of what an update set aside may name as the setup''s own;',
    '// and for each such version and kind, what it installed and this package has not, and their folders, the deepest',
    '// first, which RemoveEarlierFiles removes after a whole copy where that version of that kind was installed in that',
    '// very folder.')
foreach ($kind in 'x64', 'arm64') {
    $lists = $earlierLists[$kind]
    $suffix = $suffixOf[$kind]
    $earlierLines += @("function EarlierFiles$($suffix): String;", 'begin', "  Result := '|';")
    foreach ($path in $lists.All) { $earlierLines += "  Result := Result + '$($path -replace '/', '\')|';" }
    $earlierLines += @('end;', '', "function EarlierFilesOf$($suffix)(Version, Kind: String): String;", 'begin', "  Result := '|';")
    foreach ($at in $lists.Of.Keys) {
        $versionAt, $kindAt = $at.Split(' ')
        $earlierLines += "  if (Version = '$versionAt') and (Kind = '$kindAt') then begin"
        foreach ($path in $lists.Of[$at]) { $earlierLines += "    Result := Result + '$($path -replace '/', '\')|';" }
        $earlierLines += '  end;'
    }
    $earlierLines += @('end;', '', "function EarlierFoldersOf$($suffix)(Version, Kind: String): String;", 'begin', "  Result := '|';")
    foreach ($at in $lists.Of.Keys) {
        $versionAt, $kindAt = $at.Split(' ')
        $earlierLines += "  if (Version = '$versionAt') and (Kind = '$kindAt') then begin"
        foreach ($folder in (Get-FoldersBackwards $lists.Of[$at])) { $earlierLines += "    Result := Result + '$folder|';" }
        $earlierLines += '  end;'
    }
    $earlierLines += @('end;', '')
}
Set-Content -LiteralPath (Join-Path $inputs 'earlier.iss') -Value $earlierLines -Encoding ascii

# What the compiler is expected to put in: by where it is read from, its size and SHA-256.
$expected = @{}
$bytesOfKind = @{}
foreach ($one in $packages.Values) {
    $sum = [long] 0
    foreach ($entry in $one.Entries) {
        $expected[(Join-Path $one.Folder (([string] $entry.path) -replace '/', '\')).ToLowerInvariant()] = @{ Path = [string] $entry.path; Bytes = [long] $entry.bytes; Sha256 = [string] $entry.sha256; Kind = $one.Kind }
        $sum += [long] $entry.bytes
    }
    $expected[$one.ManifestFile.ToLowerInvariant()] = @{ Path = 'package-manifest.json'; Bytes = (Get-Item -LiteralPath $one.ManifestFile).Length; Sha256 = Get-Sha256 $one.ManifestFile; Kind = $one.Kind }
    $bytesOfKind[$one.Kind] = $sum + (Get-Item -LiteralPath $one.ManifestFile).Length
}
$expected[$marker.ToLowerInvariant()] = @{ Path = 'qkvm-desktop.installed'; Bytes = $markerBytes.Length; Sha256 = Get-Sha256 $marker; Kind = '' }
foreach ($one in $checks.Values) {
    $expected[$one.File.ToLowerInvariant()] = @{ Path = $one.Name; Bytes = $one.Bytes; Sha256 = $one.Sha256; Kind = $one.Kind }
}

$iscc = Join-Path $Inno 'ISCC.exe'
# (The bytes of every file it puts in on a PC of either kind, the more of the two, with the marker and the update check:
# in update mode the setup asks for that much room before it changes anything.)
$packageBytes = [long] (@($bytesOfKind.Values) | Measure-Object -Maximum).Maximum + $markerBytes.Length +
    [long] (@($checks.Values | ForEach-Object { $_.Bytes }) | Measure-Object -Maximum).Maximum
$name = Get-SetupName $version
$packageDefines = @("/DPackageDir=$($packages['x64'].Folder)") + @(if ($arm) { "/DArm64PackageDir=$($packages['arm64'].Folder)" })
function Invoke-Compile([string] $Into, [string[]] $Defines = @()) {
    <# Compiles the inputs into $Into, and checks the setup it made against what was expected. Its SHA-256. #>
    $said = & $iscc "/O$Into" "/DAppVersion=$version" @packageDefines "/DPackageBytes=$packageBytes" @Defines (Join-Path $inputs 'qkvm-desktop.iss') 2>&1 | ForEach-Object { "$_" }
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
    schema = 2
    version = $version
    architectures = $kinds
    setup = [ordered]@{ name = $name; bytes = (Get-Item -LiteralPath $setup).Length; sha256 = $sha256
        signature = [string] (Get-AuthenticodeSignature -LiteralPath $setup).Status; manifestLevel = 'asInvoker' }
    packages = @(foreach ($one in $packages.Values) {
        [ordered]@{ architecture = $one.Kind; folder = Split-Path -Leaf $one.Folder; manifestSha256 = Get-Sha256 $one.ManifestFile
            files = @($one.Entries).Count; bytes = $bytesOfKind[$one.Kind] } })
    roomAsked = $packageBytes
    updateChecks = @(foreach ($one in $checks.Values) { [ordered]@{ architecture = $one.Kind; name = $one.Name; bytes = $one.Bytes; sha256 = $one.Sha256 } })
    inno = [ordered]@{ version = $InnoVersion; tree = $InnoTreeDigest }
    script = [ordered]@{ sha256 = Get-Sha256 (Join-Path $inputs 'qkvm-desktop.iss'); files = Get-Sha256 (Join-Path $inputs 'files.iss')
        published = Get-Sha256 $PublishedFiles; earlier = @($earlierLists.Values | ForEach-Object { $_.All.Count } | Measure-Object -Sum).Sum }
    list = Get-Sha256 (Join-Path $out 'setup-files.txt')
    scanTwin = $twin
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $out 'setup-record.json') -Encoding utf8
$counted = @($packages.Values | ForEach-Object { @($_.Entries).Count + 1 } | Measure-Object -Sum).Sum
Write-Output "Setup: $setup" "SHA-256: $sha256" "For: $($kinds -join ', ')" "Files: $($counted + 1 + $checks.Count), each package's with its manifest, the marker and the update check$(if ($checks.Count -gt 1) { 's' })$(if ($twin) { '; and its uncompressed twin for the byte scan' })"
