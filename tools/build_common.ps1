# What the release build's scripts share (build_release.ps1, build_ffmpeg.ps1, release_files.ps1, verify_release.ps1
# and their tests). Taken in with:  . "$PSScriptRoot\build_common.ps1"
# (The release key's own helpers are release_common.ps1's.)
# Nothing here reads the network, a profile or a secret.

Set-StrictMode -Version Latest

# Where the program asks for its updates, as the owner chose (D7). Built into every published copy for good.
$PublicUpdateFeed = 'https://download.qkvm.com/desktop/stable.json'

# Inno Setup, which makes the program's setup (the owner, 2026-11-08): tools/fetch_inno.ps1 unpacks it, and
# tools/build_setup.ps1 compiles with it and with nothing else. Its version, and the tree it unpacks to: Get-TreeDigest
# of it, and how many files it holds (first unpacked on 2026-11-09: the compiler, the setup's runtime Setup.e32 and its
# loaders, the compression libraries, the help, the examples and the translations).
$InnoVersion = '6.7.4'
$InnoTreeDigest = '2be5abb1b5f8480e8911419f78128fdc28febd2e1b079efb2a1708741b1b24c7'
$InnoTreeFiles = 119

# Where installed copies (the setup's) ask for their updates, beside the portable copies' feed (2026-11-08).
$SetupUpdateFeed = 'https://download.qkvm.com/desktop/stable-setup.json'
# And Windows on Arm's, beside them (0.2.0): every copy on Windows on Arm asks these, whatever its own kind.
$Arm64UpdateFeed = 'https://download.qkvm.com/desktop/stable-arm64.json'
$Arm64SetupUpdateFeed = 'https://download.qkvm.com/desktop/stable-arm64-setup.json'

function Get-SetupFeedRefusals {
    <#
    Why the payload of installed copies' feed of $Kind (stable-setup.json for x64, stable-arm64-setup.json for Windows on
    Arm: prepare_update_payload.ps1 -Setup -Arch, put in its envelope and signed by assemble_update_feed.ps1) is not the
    one of the setup of $Version that was built: no refusal when it is. It is one JSON object of exactly the payload's
    eleven fields, each once and of its kind, of qkvm-desktop's stable channel for windows-<kind>-setup; it offers
    $Version and names the setup by $Bytes and $Sha256 (the one setup, for both kinds); and it is
    current at $Now: published before it (with five minutes' grace), expiring after it, and within 31 days of its
    publication. Its fields come back too. Its signature is the caller's to check first, and so is the address it
    names: where it is uploaded, or a place of what is published.
    #>
    param([Parameter(Mandatory)] [byte[]] $Payload, [Parameter(Mandatory)] [string] $Version, [long] $Bytes,
        [string] $Sha256, [DateTime] $Now = [DateTime]::UtcNow, [ValidateSet('x64', 'arm64', IgnoreCase = $false)] [string] $Kind = 'x64')
    $fields = @('schema', 'product', 'platform', 'channel', 'version', 'published', 'expires', 'package', 'bytes', 'sha256', 'notes')
    $value = @{}
    try {
        $document = [Text.Json.JsonDocument]::Parse([Text.UTF8Encoding]::new($false, $true).GetString($Payload))
    } catch {
        return [pscustomobject]@{ Fields = $value; Refusals = @('it is no JSON') }
    }
    try {
        $root = $document.RootElement
        if ($root.ValueKind -ne [Text.Json.JsonValueKind]::Object) { return [pscustomobject]@{ Fields = $value; Refusals = @('it is no JSON object') } }
        $names = @($root.EnumerateObject() | ForEach-Object { $_.Name })
        if ($names.Count -ne $fields.Count -or @($fields | Where-Object { @($names -ceq $_).Count -ne 1 }).Count) {
            return [pscustomobject]@{ Fields = $value; Refusals = @("its fields are not exactly $($fields -join ', '), each once") }
        }
        foreach ($property in $root.EnumerateObject()) {
            $whole = [long] 0
            $value[$property.Name] = switch ($property.Value.ValueKind) {
                ([Text.Json.JsonValueKind]::Number) { if ($property.Value.TryGetInt64([ref] $whole)) { $whole } else { $null } }
                ([Text.Json.JsonValueKind]::String) { $property.Value.GetString() }
                default { $null }
            }
        }
    } finally {
        $document.Dispose()
    }
    $refusals = [Collections.Generic.List[string]]::new()
    if ($value.schema -ne 1 -or $value.product -cne 'qkvm-desktop' -or $value.channel -cne 'stable' -or
        $value.package -isnot [string] -or $value.notes -isnot [string]) {
        $refusals.Add('it is not a payload of schema 1 of qkvm-desktop''s stable channel')
    }
    if ($value.platform -cne "windows-$Kind-setup") { $refusals.Add("it is not of windows-$Kind-setup (it is of $($value.platform))") }
    if ($value.version -cne $Version) { $refusals.Add("it offers $($value.version), not $Version") }
    if ($value.bytes -isnot [long] -or $value.bytes -ne $Bytes -or $value.sha256 -cne $Sha256) { $refusals.Add('it names other bytes than the setup built') }
    $culture = [Globalization.CultureInfo]::InvariantCulture
    $styles = [Globalization.DateTimeStyles]::AdjustToUniversal -bor [Globalization.DateTimeStyles]::AssumeUniversal
    $published = [DateTime]::MinValue
    $expires = [DateTime]::MinValue
    if (-not ([DateTime]::TryParseExact("$($value.published)", "yyyy-MM-dd'T'HH:mm:ss'Z'", $culture, $styles, [ref] $published) -and
        [DateTime]::TryParseExact("$($value.expires)", "yyyy-MM-dd'T'HH:mm:ss'Z'", $culture, $styles, [ref] $expires))) {
        $refusals.Add('its dates are not UTC dates as the program reads them')
    } else {
        if ($expires -le $published -or $expires -gt $published.AddDays(31)) { $refusals.Add('it expires more than 31 days after its publication, or not after it') }
        if ($expires -le $Now -or $published -gt $Now.AddMinutes(5)) { $refusals.Add("it is not current (published $($value.published), expires $($value.expires))") }
    }
    return [pscustomobject]@{ Fields = $value; Refusals = @($refusals) }
}

function Get-SetupName {
    <# The name of the setup of $Version, as tools/build_setup.ps1 makes it and a release publishes it. #>
    param([Parameter(Mandatory)] [string] $Version)
    # (One setup for both kinds of Windows from 0.2.0: it installs the PC's own. 0.1.0 to 0.1.2's were
    # qKVM-Desktop-<version>-windows-x64-setup.exe, and the program takes any name that ends in -setup.exe.)
    return "qKVM-Desktop-$Version-windows-setup.exe"
}

function Get-ZipName {
    <# The name of the portable ZIP of $Version for $Kind of Windows (x64 or arm64), as tools/package.ps1 makes it. #>
    param([Parameter(Mandatory)] [string] $Version, [Parameter(Mandatory)] [ValidateSet('x64', 'arm64', IgnoreCase = $false)] [string] $Kind)
    return "qKVM-Desktop-$Version-windows-$Kind.zip"
}

function Get-PeManifest {
    <#
    The manifest a Windows program carries (its resource of kind 24, number 1), as text; nothing where it has none.
    The file is mapped by Windows as data and its resources read: it is never run.
    #>
    param([Parameter(Mandatory)] [string] $File)
    if (-not ('Qkvm.PeResources' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace Qkvm {
    public static class PeResources {
        const uint AsDataFile = 0x00000002, AsImageResource = 0x00000020;
        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        static extern IntPtr LoadLibraryExW(string name, IntPtr file, uint flags);
        [DllImport("kernel32.dll")] static extern bool FreeLibrary(IntPtr module);
        [DllImport("kernel32.dll")] static extern IntPtr FindResourceW(IntPtr module, IntPtr name, IntPtr type);
        [DllImport("kernel32.dll")] static extern IntPtr LoadResource(IntPtr module, IntPtr found);
        [DllImport("kernel32.dll")] static extern IntPtr LockResource(IntPtr loaded);
        [DllImport("kernel32.dll")] static extern uint SizeofResource(IntPtr module, IntPtr found);

        public static byte[] Read(string file, int type, int number) {
            IntPtr module = LoadLibraryExW(file, IntPtr.Zero, AsDataFile | AsImageResource);
            if (module == IntPtr.Zero) { throw new InvalidOperationException("Windows cannot read the resources of " + file + " (" + Marshal.GetLastWin32Error() + ")."); }
            try {
                IntPtr found = FindResourceW(module, (IntPtr)number, (IntPtr)type);
                if (found == IntPtr.Zero) { return null; }
                IntPtr at = LockResource(LoadResource(module, found));
                byte[] bytes = new byte[SizeofResource(module, found)];
                Marshal.Copy(at, bytes, 0, bytes.Length);
                return bytes;
            } finally {
                FreeLibrary(module);
            }
        }
    }
}
'@
    }
    $bytes = [Qkvm.PeResources]::Read((Resolve-Path -LiteralPath $File).Path, 24, 1)
    if ($null -eq $bytes) { return '' }
    return [Text.Encoding]::UTF8.GetString($bytes)
}

function Test-ReleasePublicKey {
    <#
    Whether $Hex is a public key the program can pin: 128 hexadecimal digits, the X and then the Y of a point
    that is on the P-256 curve (Windows' own cryptography refuses a point that is not). Says nothing of whose it is.
    #>
    param([AllowEmptyString()] [string] $Hex)
    # (\z: a line feed after the digits is not a key's.)
    if ($Hex -cnotmatch '\A[0-9a-fA-F]{128}\z') { return $false }
    try {
        $parameters = [Security.Cryptography.ECParameters]::new()
        $parameters.Curve = [Security.Cryptography.ECCurve+NamedCurves]::nistP256
        $point = [Security.Cryptography.ECPoint]::new()
        $point.X = [Convert]::FromHexString($Hex.Substring(0, 64))
        $point.Y = [Convert]::FromHexString($Hex.Substring(64))
        $parameters.Q = $point
        $key = [Security.Cryptography.ECDsa]::Create($parameters)
        $key.Dispose()
        return $true
    } catch {
        return $false
    }
}

function New-ThrowawayPublicKey {
    <#
    The public half of a P-256 key made here and dropped at once: its private half is never written anywhere,
    so a program that pins it takes no update from anybody. For a trial build only, never for one handed out.
    #>
    $key = [Security.Cryptography.ECDsa]::Create([Security.Cryptography.ECCurve+NamedCurves]::nistP256)
    try {
        $public = $key.ExportParameters($false)
        return ([Convert]::ToHexString($public.Q.X) + [Convert]::ToHexString($public.Q.Y)).ToLowerInvariant()
    } finally {
        $key.Dispose()
    }
}

function Get-PublishPreset {
    <#
    The publish preset of the CMakePresets.json in $SourceRoot: its feed and its public key, as committed. Throws
    when there is none, or when it inherits anything but the release preset.
    #>
    param([Parameter(Mandatory)] [string] $SourceRoot)
    $file = Join-Path $SourceRoot 'CMakePresets.json'
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "There is no CMakePresets.json in $SourceRoot." }
    $presets = Get-Content -LiteralPath $file -Raw | ConvertFrom-Json
    $publish = @($presets.configurePresets | Where-Object { $_.name -ceq 'publish' })
    if ($publish.Count -ne 1) { throw 'CMakePresets.json has no publish preset (or more than one).' }
    $preset = $publish[0]
    if ($preset.inherits -cne 'release') { throw 'The publish preset must inherit the release preset and nothing else.' }
    $variables = $preset.cacheVariables
    $names = @($variables.PSObject.Properties.Name)
    foreach ($name in 'QKVM_UPDATE_FEED', 'QKVM_UPDATE_PUBLIC_KEY') {
        if ($names -cnotcontains $name) { throw "The publish preset does not say $name." }
    }
    # (And Windows on Arm's, which takes both from it and nothing of its own: the one feed and key for both kinds.)
    $arm = @($presets.configurePresets | Where-Object { $_.name -ceq 'arm64-publish' })
    if ($arm.Count -ne 1) { throw 'CMakePresets.json has no arm64-publish preset (or more than one).' }
    if ((@($arm[0].inherits) -join ',') -cne 'publish,arm64' -or $arm[0].PSObject.Properties['cacheVariables']) {
        throw 'The arm64-publish preset must inherit the publish preset and the arm64 one, in that order, and say nothing of its own.'
    }
    return [pscustomobject]@{ Feed = [string] $variables.QKVM_UPDATE_FEED; PublicKey = [string] $variables.QKVM_UPDATE_PUBLIC_KEY }
}

function Get-ProjectVersion {
    <# The version CMakeLists.txt in $SourceRoot gives the program: three numbers, as every machine reads it (D2). #>
    param([Parameter(Mandatory)] [string] $SourceRoot)
    $line = Select-String -LiteralPath (Join-Path $SourceRoot 'CMakeLists.txt') -Pattern '^project\(QkvmDesktop VERSION (\d+\.\d+\.\d+) ' |
        Select-Object -First 1
    if (-not $line) { throw "The CMakeLists.txt in $SourceRoot says no version of qKVM Desktop." }
    return $line.Matches[0].Groups[1].Value
}

function Get-BuildToolVersions {
    <#
    What built a release, as each tool says it, for its build record's tools (build_release.ps1 asks it in each kind's
    build environment, after Enter-BuildEnvironment): Visual Studio, its C++ tools, compiler and linker, the Windows SDK,
    the runtime that ships, CMake, Ninja, git and PowerShell. A tool that ends in an error, or says nothing of what it
    is, stops the build (outside review 2, 5.1). The compiler and the linker, given nothing to do, say their name and
    version first and then how they are used: of them only those words are read, not how such a call ends.
    $VsWhere is Visual Studio's vswhere.exe, as it is installed.
    #>
    param($VsWhere = (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'))
    $first = {
        param([string] $Name, [scriptblock] $What, [string] $Like = '.', [switch] $AnyEnd)
        # (All it says, to its end, before its first line of the kind is taken: a pipeline stopped at that line would
        # stop the tool too, and it would never say how it ended.)
        $all = @(& $What 2>&1 | ForEach-Object { "$_".Trim() })
        if (-not $AnyEnd -and $LASTEXITCODE -ne 0) { throw "$Name ended in an error (exit $LASTEXITCODE) when asked what it is: the build record would not say what built this." }
        $line = @($all | Where-Object { $_ -match $Like } | Select-Object -First 1)
        if (-not $line.Count -or -not $line[0]) { throw "$Name said nothing of what it is: the build record would not say what built this." }
        return $line[0]
    }
    return [ordered]@{
        visualStudio = & $first 'vswhere' { & $VsWhere -path $env:QKVM_BUILD_ENVIRONMENT -property installationVersion }
        msvcTools = $env:VCToolsVersion
        compiler = & $first 'cl' { cl } 'Version' -AnyEnd
        linker = & $first 'link' { link } 'Version' -AnyEnd
        windowsSdk = $env:WindowsSDKVersion.TrimEnd('\')
        vcRedist = Split-Path -Leaf $env:VCToolsRedistDir.TrimEnd('\')
        cmake = & $first 'cmake' { cmake --version }
        ninja = & $first 'ninja' { ninja --version }
        git = & $first 'git' { git --version }
        powershell = $PSVersionTable.PSVersion.ToString()
    }
}

function Get-LowIntegrityItems {
    <#
    The files and folders under $Folder that Windows marks low integrity, by their paths, as icacls says each one's label
    (tools/fetch_inno.ps1, once Inno Setup is copied out of its low folder). An icacls that ends in an error stops this:
    an item it could not read is not known to be other than low (outside review 2, 5.1). $Icacls is Windows' icacls.exe.
    #>
    param([Parameter(Mandatory)] [string] $Folder, $Icacls = (Join-Path $env:SystemRoot 'System32\icacls.exe'))
    $low = @()
    foreach ($item in @(Get-ChildItem -LiteralPath $Folder -Recurse -Force)) {
        $said = @(& $Icacls $item.FullName 2>&1 | ForEach-Object { "$_" })
        if ($LASTEXITCODE -ne 0) {
            throw "icacls could not say the integrity of $($item.FullName) (icacls exit ${LASTEXITCODE}: $(($said | Select-Object -First 2) -join ' / ')), so it is not known to be other than low: nothing of this unpacking is taken. Run it again; if icacls fails again, find why it cannot read that file's label."
        }
        if (($said -join "`n") -match 'Low Mandatory Level') { $low += $item.FullName }
    }
    return $low
}

function Get-PeDebugPaths {
    <#
    The paths a Windows program or library names for its symbols (the CodeView records of its debug directory):
    none for one built without, a bare file name for one linked with /PDBALTPATH:%_PDB%. Throws on a file that is
    not a 64-bit Windows image.
    #>
    param([Parameter(Mandatory)] [string] $File)
    $bytes = [IO.File]::ReadAllBytes($File)
    if ($bytes.Length -lt 0x40 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) { throw "$File is not a Windows image." }
    $pe = [BitConverter]::ToInt32($bytes, 0x3C)
    if ($pe -lt 0 -or $pe + 24 -gt $bytes.Length -or [BitConverter]::ToUInt32($bytes, $pe) -ne 0x00004550) {
        throw "$File is not a Windows image."
    }
    $sections = [BitConverter]::ToUInt16($bytes, $pe + 6)
    $optionalSize = [BitConverter]::ToUInt16($bytes, $pe + 20)
    $optional = $pe + 24
    if ([BitConverter]::ToUInt16($bytes, $optional) -ne 0x20B) { throw "$File is not a 64-bit Windows image." }
    $directories = [BitConverter]::ToUInt32($bytes, $optional + 108)
    if ($directories -le 6) { return @() }
    $debugRva = [BitConverter]::ToUInt32($bytes, $optional + 112 + 6 * 8)
    $debugSize = [BitConverter]::ToUInt32($bytes, $optional + 112 + 6 * 8 + 4)
    if ($debugRva -eq 0 -or $debugSize -eq 0) { return @() }
    $table = $optional + $optionalSize
    $at = -1
    for ($index = 0; $index -lt $sections; ++$index) {
        $section = $table + 40 * $index
        $virtualSize = [BitConverter]::ToUInt32($bytes, $section + 8)
        $virtualAddress = [BitConverter]::ToUInt32($bytes, $section + 12)
        $rawPointer = [BitConverter]::ToUInt32($bytes, $section + 20)
        if ($debugRva -ge $virtualAddress -and $debugRva -lt $virtualAddress + $virtualSize) {
            $at = [long] $rawPointer + ($debugRva - $virtualAddress)
        }
    }
    if ($at -lt 0 -or $at + $debugSize -gt $bytes.Length) { throw "The debug directory of $File is outside the file." }
    $paths = @()
    for ($entry = 0; $entry + 28 -le $debugSize; $entry += 28) {
        $type = [BitConverter]::ToUInt32($bytes, $at + $entry + 12)
        $dataSize = [BitConverter]::ToUInt32($bytes, $at + $entry + 16)
        $dataPointer = [BitConverter]::ToUInt32($bytes, $at + $entry + 24)
        if ($type -ne 2 -or $dataSize -lt 25 -or [long] $dataPointer + $dataSize -gt $bytes.Length) { continue }
        if ([BitConverter]::ToUInt32($bytes, $dataPointer) -ne 0x53445352) { continue } # RSDS
        $name = $dataPointer + 24
        $end = [Array]::IndexOf($bytes, [byte] 0, $name)
        if ($end -lt 0 -or $end -gt $dataPointer + $dataSize) { $end = $dataPointer + $dataSize }
        $paths += [Text.Encoding]::UTF8.GetString($bytes, $name, $end - $name)
    }
    return $paths
}

function Get-PeImports {
    <#
    The libraries a 64-bit Windows program or library imports, by the names its import table and its delay-import
    table give them, each once as written. Throws on bytes that are no such image, or whose tables lie outside them.
    #>
    param([Parameter(Mandatory)] [byte[]] $Bytes)
    if ($Bytes.Length -lt 0x40 -or $Bytes[0] -ne 0x4D -or $Bytes[1] -ne 0x5A) { throw 'It is not a Windows image.' }
    $pe = [BitConverter]::ToInt32($Bytes, 0x3C)
    if ($pe -lt 0 -or [long] $pe + 24 -gt $Bytes.Length -or [BitConverter]::ToUInt32($Bytes, $pe) -ne 0x00004550) {
        throw 'It is not a Windows image.'
    }
    $sections = [BitConverter]::ToUInt16($Bytes, $pe + 6)
    $optional = $pe + 24
    $table = $optional + [BitConverter]::ToUInt16($Bytes, $pe + 20)
    if ([long] $optional + 112 + 14 * 8 -gt $Bytes.Length -or [BitConverter]::ToUInt16($Bytes, $optional) -ne 0x20B) {
        throw 'It is not a 64-bit Windows image.'
    }
    if ([long] $table + 40 * $sections -gt $Bytes.Length) { throw 'Its section table lies outside it.' }
    $places = @(for ($index = 0; $index -lt $sections; ++$index) {
        $section = $table + 40 * $index
        [pscustomobject]@{
            Address = [long] [BitConverter]::ToUInt32($Bytes, $section + 12)
            Size = [long] [Math]::Max([BitConverter]::ToUInt32($Bytes, $section + 8), [BitConverter]::ToUInt32($Bytes, $section + 16))
            Raw = [long] [BitConverter]::ToUInt32($Bytes, $section + 20)
        }
    })
    # (Where in the file a place of the image lies, or a throw where it is in none of its sections.)
    $offsetOf = {
        param([long] $Rva, [int] $Length)
        foreach ($place in $places) {
            if ($Rva -ge $place.Address -and $Rva -lt $place.Address + $place.Size) {
                $offset = $place.Raw + ($Rva - $place.Address)
                if ($offset + $Length -le $Bytes.Length) { return $offset }
            }
        }
        throw 'An import table lies outside it.'
    }
    $directories = [BitConverter]::ToUInt32($Bytes, $optional + 108)
    $names = [Collections.Generic.List[string]]::new()
    # The import table (descriptors of 20 bytes, the name at 12) and the delay-import table (32 bytes, the name at 4),
    # each ended by a descriptor of zeros.
    foreach ($kind in @(@{ Index = 1; Size = 20; Name = 12 }, @{ Index = 13; Size = 32; Name = 4 })) {
        if ($directories -le $kind.Index) { continue }
        $rva = [BitConverter]::ToUInt32($Bytes, $optional + 112 + 8 * $kind.Index)
        if ($rva -eq 0) { continue }
        for ($count = 0; ; ++$count) {
            if ($count -gt 4096) { throw 'An import table has no end.' }
            $descriptor = & $offsetOf ($rva + [long] $kind.Size * $count) $kind.Size
            $nameRva = [BitConverter]::ToUInt32($Bytes, $descriptor + $kind.Name)
            if ($nameRva -eq 0) { break }
            $name = & $offsetOf $nameRva 1
            $end = [Array]::IndexOf($Bytes, [byte] 0, [int] $name, [int] [Math]::Min(260, $Bytes.Length - $name))
            if ($end -le $name) { throw 'An imported name has no end.' }
            $names.Add([Text.Encoding]::ASCII.GetString($Bytes, $name, $end - $name))
        }
    }
    return @($names | Select-Object -Unique)
}

function Get-PeMachine {
    <# The kind of processor a Windows image is for, as its header says (0x8664 x64, 0xAA64 ARM64); a throw for bytes that
    are no Windows image. #>
    param([Parameter(Mandatory)] [byte[]] $Bytes)
    if ($Bytes.Length -lt 0x40 -or $Bytes[0] -ne 0x4D -or $Bytes[1] -ne 0x5A) { throw 'It is not a Windows image.' }
    $pe = [BitConverter]::ToInt32($Bytes, 0x3C)
    if ($pe -lt 0 -or [long] $pe + 6 -gt $Bytes.Length -or [BitConverter]::ToUInt32($Bytes, $pe) -ne 0x00004550) {
        throw 'It is not a Windows image.'
    }
    return [BitConverter]::ToUInt16($Bytes, $pe + 4)
}

function Get-PackageImageRefusals {
    <#
    Why the images of a package folder are not all of its kind and whole: nothing when they are. Every program and
    library in it (.exe, .dll, anywhere in the folder) is for the package's kind of processor ($Arch, x64 or arm64; the
    Build Tools' ARM64 runtime folder holds an x64 vcruntime140_1.dll for emulated code, which an ARM64 package has no
    use for, 2026-11-19), and every library one of them imports, at its start or when first called, is one the package
    holds (by name, anywhere in it), or Windows' own: an API set (api-ms-win-*, ext-ms-win-*) or a library of this PC's
    Windows folder that is not the Visual C++ runtime, which the package carries itself. -KindsOnly: the kinds alone (a
    package's startup checks, which leave a library taken away to the program's own start to find).
    #>
    param([Parameter(Mandatory)] [string] $Folder, [Parameter(Mandatory)] [ValidateSet('x64', 'arm64')] [string] $Arch,
        [switch] $KindsOnly)
    $root = (Resolve-Path -LiteralPath $Folder).Path
    $machines = @{ [uint16] 0x8664 = 'x64'; [uint16] 0xAA64 = 'arm64' }
    $images = @(Get-ChildItem -LiteralPath $root -Recurse -File | Where-Object { $_.Extension -in '.exe', '.dll' })
    $held = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($image in $images) { $null = $held.Add($image.Name) }
    $windows = Join-Path $env:SystemRoot 'System32'
    $refused = @()
    foreach ($image in $images) {
        $relative = [IO.Path]::GetRelativePath($root, $image.FullName).Replace('\', '/')
        $bytes = [IO.File]::ReadAllBytes($image.FullName)
        try { $machine = Get-PeMachine $bytes } catch { $refused += "$relative is no Windows image"; continue }
        $kind = if ($machines.ContainsKey([uint16] $machine)) { $machines[[uint16] $machine] } else { '0x{0:X4}' -f $machine }
        if ($kind -ne $Arch) { $refused += "$relative is a program for $kind, not $Arch"; continue }
        if ($KindsOnly) { continue }
        try { $imports = @(Get-PeImports -Bytes $bytes) } catch { $refused += "$relative's imports cannot be read ($($_.Exception.Message))"; continue }
        foreach ($import in $imports) {
            if ($held.Contains($import) -or $import -match '\A(api|ext)-ms-win-[a-z0-9-]+\.dll\z') { continue }
            $runtime = $import -match '\A(vcruntime|msvcp|concrt|vccorlib)\d'
            if (-not $runtime -and (Test-Path -LiteralPath (Join-Path $windows $import) -PathType Leaf)) { continue }
            $refused += "$relative imports $import, which the package does not hold$(if (-not $runtime) { ' and Windows does not have' })"
        }
    }
    return $refused
}

function Get-PackagePairRefusals {
    <#
    Why a package for Windows on ARM ($Arm64, its package-manifest.json read back) does not hold the files the x64 package
    of the same build ($X64) holds, path for path: nothing when it does. They differ only where the two kinds' parts do:
    x64's vcruntime140_1.dll (the second part of x64's runtime, which ARM64 has not), and a development FFmpeg's
    swresample-7.dll in either (the project's own FFmpeg has none). Both are of one version and of their kinds. For the
    release tools, which make both (review arm 1, L3: windeployqt left out a plugin, or a file the x64 package has went
    missing, and only the startup check would have found it).
    #>
    param([Parameter(Mandatory)] $X64, [Parameter(Mandatory)] $Arm64)
    $refusals = @()
    if ([string] $X64.architecture -cne 'x64' -or [string] $Arm64.architecture -cne 'arm64') { return @('they are not an x64 package and an ARM64 one') }
    if ([string] $X64.version -cne [string] $Arm64.version) { $refusals += "they are of two versions ($($X64.version), $($Arm64.version))" }
    $allowed = { param([string] $Path, [bool] $IsX64) ($IsX64 -and $Path -ceq 'vcruntime140_1.dll') -or $Path -cmatch '\Aswresample-\d+\.dll\z' }
    $x64Paths = @($X64.files | ForEach-Object { [string] $_.path } | Where-Object { -not (& $allowed $_ $true) })
    $armPaths = @($Arm64.files | ForEach-Object { [string] $_.path } | Where-Object { -not (& $allowed $_ $false) })
    foreach ($path in $x64Paths | Where-Object { $armPaths -cnotcontains $_ }) { $refusals += "the ARM64 package lacks $path" }
    foreach ($path in $armPaths | Where-Object { $x64Paths -cnotcontains $_ }) { $refusals += "the ARM64 package holds $path, which the x64 one does not" }
    return $refusals
}

function Get-PeProtections {
    <#
    What a 64-bit Windows image is built with against the misuse of its memory, read from its own bytes (it is never
    loaded): Machine (0x8664 x64, 0xAA64 ARM64); Aslr, its place chosen anew at each start (DYNAMIC_BASE, and its
    relocations kept); HighEntropyVa, that place chosen from the whole 64-bit room; Dep, nothing run from its data
    (NX_COMPAT); Cfg, every indirect call checked against the image's own table of what may be called (GUARD_CF, and
    its load configuration says the code was built for it); Cet, fit for x64's shadow stack, by which the processor
    checks every return against a copy Windows keeps (CET_COMPAT, of its extended characteristics); EhContinuation, its
    exception handlers' targets listed for that stack; Signed of Functions: how many of its functions sign their return
    address with ARM64's pointer authentication as they begin (PACIBSP, which /guard:signret puts at the start of each
    function that saves its return address), of how many functions its unwind table names (ARM64 only; 0 for x64).
    Throws on bytes that are no such image.
    #>
    param([Parameter(Mandatory)] [byte[]] $Bytes)
    if ($Bytes.Length -lt 0x40 -or $Bytes[0] -ne 0x4D -or $Bytes[1] -ne 0x5A) { throw 'It is not a Windows image.' }
    $pe = [BitConverter]::ToInt32($Bytes, 0x3C)
    if ($pe -lt 0 -or [long] $pe + 24 -gt $Bytes.Length -or [BitConverter]::ToUInt32($Bytes, $pe) -ne 0x00004550) {
        throw 'It is not a Windows image.'
    }
    $machine = [BitConverter]::ToUInt16($Bytes, $pe + 4)
    $sections = [BitConverter]::ToUInt16($Bytes, $pe + 6)
    $fileCharacteristics = [BitConverter]::ToUInt16($Bytes, $pe + 22)
    $optional = $pe + 24
    if ([long] $optional + 112 -gt $Bytes.Length -or [BitConverter]::ToUInt16($Bytes, $optional) -ne 0x20B) {
        throw 'It is not a 64-bit Windows image.'
    }
    $table = $optional + [BitConverter]::ToUInt16($Bytes, $pe + 20)
    if ([long] $table + 40 * $sections -gt $Bytes.Length) { throw 'Its section table lies outside it.' }
    $characteristics = [BitConverter]::ToUInt16($Bytes, $optional + 70)
    $count = [BitConverter]::ToUInt32($Bytes, $optional + 108)
    # (A data directory's place and size; nothing where the image has fewer.)
    $directory = {
        param([int] $Index)
        if ($count -le $Index -or [long] $optional + 120 + 8 * $Index -gt $Bytes.Length) { return @([uint32] 0, [uint32] 0) }
        return @([BitConverter]::ToUInt32($Bytes, $optional + 112 + 8 * $Index), [BitConverter]::ToUInt32($Bytes, $optional + 116 + 8 * $Index))
    }
    $places = @(for ($index = 0; $index -lt $sections; ++$index) {
        $section = $table + 40 * $index
        [pscustomobject]@{
            Address = [long] [BitConverter]::ToUInt32($Bytes, $section + 12)
            Size = [long] [Math]::Max([BitConverter]::ToUInt32($Bytes, $section + 8), [BitConverter]::ToUInt32($Bytes, $section + 16))
            Raw = [long] [BitConverter]::ToUInt32($Bytes, $section + 20)
            RawSize = [long] [BitConverter]::ToUInt32($Bytes, $section + 16)
            Flags = [BitConverter]::ToUInt32($Bytes, $section + 36)
        }
    })
    # (Where in the file a place of the image lies: -1 where it is in none of its sections.)
    $offsetOf = {
        param([long] $Rva, [int] $Length)
        foreach ($place in $places) {
            if ($Rva -ge $place.Address -and $Rva -lt $place.Address + $place.Size) {
                $offset = $place.Raw + ($Rva - $place.Address)
                if ($offset + $Length -le $Bytes.Length) { return $offset }
            }
        }
        return -1
    }
    # The relocations (directory 5) kept, which a place chosen anew needs.
    $relocations = (& $directory 5)[1] -gt 0 -and ($fileCharacteristics -band 0x0001) -eq 0
    $aslr = ($characteristics -band 0x0040) -ne 0 -and $relocations
    # The load configuration (directory 10): its GuardFlags (at 144) where it is that long.
    $guardFlags = [uint32] 0
    $load = & $directory 10
    if ($load[0] -ne 0) {
        $at = & $offsetOf $load[0] 4
        if ($at -ge 0) {
            $size = [BitConverter]::ToUInt32($Bytes, $at)
            if ($size -ge 148 -and $at + 148 -le $Bytes.Length) { $guardFlags = [BitConverter]::ToUInt32($Bytes, $at + 144) }
        }
    }
    # The debug directory (directory 6): its extended characteristics (an entry of type 20), whose first bit is CET_COMPAT.
    $extended = [uint32] 0
    $debug = & $directory 6
    if ($debug[0] -ne 0) {
        $at = & $offsetOf $debug[0] 28
        for ($entry = 0; $at -ge 0 -and $entry + 28 -le $debug[1] -and $at + $entry + 28 -le $Bytes.Length; $entry += 28) {
            $pointer = [BitConverter]::ToUInt32($Bytes, $at + $entry + 24)
            if ([BitConverter]::ToUInt32($Bytes, $at + $entry + 12) -eq 20 -and [BitConverter]::ToUInt32($Bytes, $at + $entry + 16) -ge 4 -and
                [long] $pointer + 4 -le $Bytes.Length) {
                $extended = [BitConverter]::ToUInt32($Bytes, $pointer)
            }
        }
    }
    # ARM64's signed returns: the PACIBSP instructions in its code, and the functions of its unwind table (directory 3,
    # eight bytes a function on ARM64).
    $signed = 0
    $functions = 0
    if ($machine -eq 0xAA64) {
        $functions = [int] ((& $directory 3)[1] / 8)
        $text = [Text.Encoding]::Latin1.GetString($Bytes)
        $word = [Text.Encoding]::Latin1.GetString([byte[]] @(0x7F, 0x23, 0x03, 0xD5))
        foreach ($place in $places | Where-Object { ($_.Flags -band 0x20000020) -ne 0 }) {
            $end = [Math]::Min($place.Raw + $place.RawSize, $Bytes.Length)
            $from = $place.Raw
            while ($from -lt $end) {
                $found = $text.IndexOf($word, [int] $from, [int] ($end - $from), [StringComparison]::Ordinal)
                if ($found -lt 0 -or $found + 4 -gt $end) { break }
                if (($found - $place.Raw) % 4 -eq 0) { ++$signed; $from = $found + 4 } else { $from = $found + 1 }
            }
        }
    }
    return [pscustomobject]@{
        Machine = $machine
        Aslr = $aslr
        HighEntropyVa = $aslr -and ($characteristics -band 0x0020) -ne 0
        Dep = ($characteristics -band 0x0100) -ne 0
        Cfg = ($characteristics -band 0x4000) -ne 0 -and ($guardFlags -band 0x100) -ne 0
        Cet = ($extended -band 0x1) -ne 0
        EhContinuation = ($guardFlags -band 0x400000) -ne 0
        Signed = $signed
        Functions = $functions
    }
}

function Get-PeProtectionNames {
    <#
    The protections of tools/image-protections.json an image has, by their names there, from what Get-PeProtections read
    of it: pac where it is an ARM64 image and at least two in three of its functions sign their returns (an image with
    /guard:signret signs some 85 in 100, the leaf functions that keep no return address on the stack being the rest;
    one without, its runtime's few pieces only).
    #>
    param([Parameter(Mandatory)] $Protections)
    $names = @()
    foreach ($pair in @(@('aslr', 'Aslr'), @('highEntropyVa', 'HighEntropyVa'), @('dep', 'Dep'), @('cfg', 'Cfg'), @('cet', 'Cet'),
            @('ehContinuation', 'EhContinuation'))) {
        if ($Protections.($pair[1])) { $names += $pair[0] }
    }
    if ($Protections.Machine -eq 0xAA64 -and $Protections.Functions -gt 0 -and 3 * [long] $Protections.Signed -ge 2 * [long] $Protections.Functions) {
        $names += 'pac'
    }
    return $names
}

function Get-ImageFileProtectionRefusals {
    <#
    Why one program or library ($File), shipped as $Name (its path in what ships, with / between folders), is not built
    with what tools/image-protections.json asks of its kind ($Arch, x64 or arm64): nothing when it is. Its row is the first
    whose names match $Name, in capitals or not, and it must have every protection that row asks for $Arch, read from its
    own bytes. An image no row names is refused, and so is one whose protections cannot be read. For every image of a
    package folder (Get-ImageProtectionRefusals), and for qKVM Setup's two update checks, which the setup carries outside
    the packages and puts at the program's name while an update has the earlier version set aside (tools/build_setup.ps1
    and the release audit, review r2 of arm and armp, L1). -Rows: the table's rows, read already; -Table: another table
    (the tests').
    #>
    param([Parameter(Mandatory)] [string] $File, [Parameter(Mandatory)] [string] $Name, [Parameter(Mandatory)] [ValidateSet('x64', 'arm64')] [string] $Arch,
        [string] $Table = (Join-Path $PSScriptRoot 'image-protections.json'), [object[]] $Rows = @())
    if (-not $Rows.Count) { $Rows = @((Get-Content -LiteralPath $Table -Raw | ConvertFrom-Json).images) }
    $row = @($Rows | Where-Object { $names = @($_.names); @($names | Where-Object { $Name -like $_ }).Count } | Select-Object -First 1)
    if (-not $row.Count) { return @("$Name is named by no row of the table of protections (tools/image-protections.json)") }
    try {
        $has = @(Get-PeProtectionNames (Get-PeProtections ([IO.File]::ReadAllBytes($File))))
    } catch {
        return @("$Name's protections cannot be read ($($_.Exception.Message))")
    }
    $lacking = @(@($row[0].$Arch) | Where-Object { $has -cnotcontains $_ })
    if ($lacking.Count) { return @("$Name lacks $($lacking -join ', '), which every image of $($row[0].of) has on $Arch") }
    return @()
}

function Get-ImageProtectionRefusals {
    <#
    Why the programs and libraries of a package folder are not each built with what tools/image-protections.json asks of
    their kind ($Arch, x64 or arm64): nothing when they are. Every .exe and .dll anywhere in the folder is held to its row
    by its path in the folder (Get-ImageFileProtectionRefusals). -Table: another table (the tests').
    #>
    param([Parameter(Mandatory)] [string] $Folder, [Parameter(Mandatory)] [ValidateSet('x64', 'arm64')] [string] $Arch,
        [string] $Table = (Join-Path $PSScriptRoot 'image-protections.json'))
    $rows = @((Get-Content -LiteralPath $Table -Raw | ConvertFrom-Json).images)
    $root = (Resolve-Path -LiteralPath $Folder).Path
    $refused = @()
    foreach ($image in @(Get-ChildItem -LiteralPath $root -Recurse -File | Where-Object { $_.Extension -in '.exe', '.dll' } | Sort-Object FullName)) {
        $relative = [IO.Path]::GetRelativePath($root, $image.FullName).Replace('\', '/')
        $refused += @(Get-ImageFileProtectionRefusals -File $image.FullName -Name $relative -Arch $Arch -Rows $rows)
    }
    return $refused
}

# The sources libdatachannel and Mbed TLS are built from for both kinds (experiments/webrtc/dependencies.json, fetched by
# experiments/webrtc/fetch-cpp-dependencies.ps1): libdatachannel's commit and its four submodules' as its tree pins them,
# Mbed TLS's archive by its SHA-256, and each tree as it is built from by its digest (Get-SourceTreeDigest), taken on
# 2026-11-21 from a checkout of those commits and from that archive unpacked. experiments/webrtc/build-cpp-dependencies.ps1
# builds from nothing else, and writes what it built from beside what it built (New-WebRtcRecord).
$WebRtcPins = [ordered]@{
    libdatachannel = [ordered]@{ version = '0.24.6'; commit = '6b1e2e620f1e37f0eafeee702eaea0043cb305fd'
        gitlinks = [ordered]@{ libjuice = 'b89c792e3612faf2f12cf35bcc56857313a06be3'; libsrtp = '24b3bf8f19b6f5ab4cd2bcceb4f4064efca86fd5'
            usrsctp = 'fec583d54493f879d2ae44a743423bf8a04371ab'; plog = '94899e0b926ac1b0f4750bfbd495167b4a6ae9ef' }
        tree = '3372157adc302e14081e1ea013ea79a4844638c830ae0343d9c870c81d2954a3' }
    mbedtls = [ordered]@{ version = '3.6.7'; archiveSha256 = 'a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6'
        tree = '703dadc104095f4e6cc19897e4d64de50e129ef334dee61fab2457e6bdd93f03' }
}
# (Where the record of a build of them is, beside its install folder.)
$WebRtcRecordName = 'webrtc-record.json'

function Get-SourceTreeDigest {
    <#
    One SHA-256 for a tree of sources as a build reads them: of the sorted lines "<sha256> <relative path>" of every file
    in it, as Get-TreeDigest, but with git's own files left out (any .git, folder or file, at any depth: what a checkout
    keeps of its history, which the build does not read) and each file's line ends taken as LF (a checkout by a git whose
    core.autocrlf is true writes CRLF, as Git for Windows does by default: the same commit then reads the same). Any other
    byte of any file, a file more or a file fewer, gives another digest.
    #>
    param([Parameter(Mandatory)] [string] $Folder)
    return Get-SourceLinesDigest @(Get-SourceTreeLines $Folder)
}

function ConvertTo-SourceTreeLine {
    <#
    The line Get-SourceTreeDigest takes of one file, at $Relative (with /) in its tree: "<sha256> <relative path>" of its
    $Bytes with their line ends taken as LF. Nothing for one of git's own (a .git, folder or file, at any depth).
    #>
    param([Parameter(Mandatory)] [string] $Relative, [Parameter(Mandatory)] [AllowEmptyCollection()] [byte[]] $Bytes)
    if (@($Relative.Split('/') | Where-Object { $_ -ieq '.git' }).Count) { return }
    if ([Array]::IndexOf($Bytes, [byte] 13) -ge 0) {
        $text = [Text.Encoding]::Latin1.GetString($Bytes).Replace("`r`n", "`n")
        $Bytes = [Text.Encoding]::Latin1.GetBytes($text)
    }
    return '{0} {1}' -f [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($Bytes)).ToLowerInvariant(), $Relative
}

function Get-SourceTreeLines {
    <# Get-SourceTreeDigest's lines of the tree in $Folder, one for each file but git's own (ConvertTo-SourceTreeLine). #>
    param([Parameter(Mandatory)] [string] $Folder)
    $root = (Resolve-Path -LiteralPath $Folder).Path
    return @(Get-ChildItem -LiteralPath $root -Recurse -File -Force | ForEach-Object {
        ConvertTo-SourceTreeLine ([IO.Path]::GetRelativePath($root, $_.FullName).Replace('\', '/')) ([IO.File]::ReadAllBytes($_.FullName))
    })
}

function Get-SourceLinesDigest {
    <# The digest of a tree from its lines (ConvertTo-SourceTreeLine), in any order: of the lines sorted, each ended by LF. #>
    param([AllowEmptyCollection()] [string[]] $Lines = @())
    $sorted = @($Lines | Sort-Object -CaseSensitive)
    $text = [Text.Encoding]::UTF8.GetBytes(($sorted -join "`n") + "`n")
    return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($text)).ToLowerInvariant()
}

function Get-SourcesBundleRefusals {
    <#
    Why the third-party sources $Bundle, a ZIP as tools/release_files.ps1 writes it, are not what the build used: none
    when they are. At its top it holds README.txt and each of $Archives (each with its name, bytes and sha256) by its
    size and SHA-256, each once, and libdatachannel/, and nothing else; under libdatachannel/ nothing of git's own, and
    its files, by Get-SourceTreeDigest's rules, are the tree that each of $Trees (who read it, to the digest that one's
    record names) was built from. Where they are not, how they differ from $Checkout, the checkout the build read, is said:
    the files the bundle lacks, those it holds more, and those of other bytes (outside review 2, 5.1).
    #>
    param([Parameter(Mandatory)] [string] $Bundle, [Parameter(Mandatory)] [object[]] $Archives,
        [Parameter(Mandatory)] [Collections.IDictionary] $Trees, [Parameter(Mandatory)] [string] $Checkout)
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $refusals = [Collections.Generic.List[string]]::new()
    $prefix = 'libdatachannel/'
    $lines = [Collections.Generic.List[string]]::new()
    $seen = [Collections.Generic.Dictionary[string, string]]::new([StringComparer]::Ordinal)
    $hash = [Security.Cryptography.SHA256]::Create()
    $zip = [IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $Bundle).Path)
    try {
        foreach ($entry in $zip.Entries) {
            $name = $entry.FullName.Replace('\', '/')
            # (A folder's own entry, as an empty one has: a submodule the build does not use.)
            if ($name.EndsWith('/')) { continue }
            $stream = $entry.Open()
            try {
                $memory = [IO.MemoryStream]::new()
                $stream.CopyTo($memory)
                $bytes = $memory.ToArray()
            } finally {
                $stream.Dispose()
            }
            if ($name.StartsWith($prefix, [StringComparison]::Ordinal)) {
                $line = ConvertTo-SourceTreeLine $name.Substring($prefix.Length) $bytes
                if ($null -eq $line) { $refusals.Add("it holds $name, which is git's own and no source"); continue }
                $lines.Add($line)
                continue
            }
            if ($seen.ContainsKey($name)) { $refusals.Add("it holds $name twice"); continue }
            $seen[$name] = "$($bytes.Length) $([Convert]::ToHexString($hash.ComputeHash($bytes)).ToLowerInvariant())"
        }
    } finally {
        $zip.Dispose()
        $hash.Dispose()
    }
    if (-not $seen.ContainsKey('README.txt')) { $refusals.Add('it holds no README.txt') }
    $named = @($Archives | ForEach-Object { [string] $_.name })
    foreach ($archive in $Archives) {
        if (-not $seen.ContainsKey([string] $archive.name)) { $refusals.Add("it holds no $($archive.name)"); continue }
        if ($seen[[string] $archive.name] -cne "$([long] $archive.bytes) $($archive.sha256)") {
            $refusals.Add("its $($archive.name) is not the archive the build used (its size or its SHA-256 differs)")
        }
    }
    foreach ($name in $seen.Keys) {
        if ($name -cne 'README.txt' -and $named -cnotcontains $name) { $refusals.Add("it holds $name, which is none of the sources") }
    }
    $digest = Get-SourceLinesDigest @($lines)
    $differs = $false
    foreach ($tree in $Trees.GetEnumerator()) {
        if ($digest -cne [string] $tree.Value) {
            $refusals.Add("its libdatachannel, with its submodules, is not the tree $($tree.Key) was built from (its digest is $digest, the one that build's record names $($tree.Value))")
            $differs = $true
        }
    }
    if ($differs) {
        # (Which files, against the checkout the build read: by path, each file's line holding its SHA-256.)
        $mapOf = {
            param([string[]] $Of)
            $map = [Collections.Generic.Dictionary[string, string]]::new([StringComparer]::Ordinal)
            foreach ($one in $Of) { $at = $one.IndexOf(' '); $map[$one.Substring($at + 1)] = $one.Substring(0, $at) }
            return , $map
        }
        $inBundle = & $mapOf @($lines)
        $inCheckout = & $mapOf @(Get-SourceTreeLines $Checkout)
        $some = {
            param([string[]] $Names)
            $sorted = @($Names | Sort-Object -CaseSensitive)
            $shown = ($sorted | Select-Object -First 5) -join ', '
            if ($sorted.Count -gt 5) { $shown += " and $($sorted.Count - 5) more" }
            return $shown
        }
        $lacks = @($inCheckout.Keys | Where-Object { -not $inBundle.ContainsKey($_) })
        $more = @($inBundle.Keys | Where-Object { -not $inCheckout.ContainsKey($_) })
        $other = @($inBundle.Keys | Where-Object { $inCheckout.ContainsKey($_) -and $inCheckout[$_] -cne $inBundle[$_] })
        $how = @()
        if ($lacks.Count) { $how += "it lacks $(& $some $lacks)" }
        if ($more.Count) { $how += "it holds $(& $some $more), which the checkout has not" }
        if ($other.Count) { $how += "it holds $(& $some $other) with other bytes than the checkout's" }
        if ($how.Count) {
            $refusals.Add("where it differs from the checkout the build read ($Checkout): $($how -join '; ')")
        } else {
            $refusals.Add("it is the checkout the build read ($Checkout) file for file: that checkout is no longer the tree the build read, and was changed after the build, so the run is to be built again")
        }
    }
    return @($refusals)
}

function Get-WebRtcSourceRefusals {
    <#
    Why the folder $Sources does not hold the pinned sources of libdatachannel and Mbed TLS ($WebRtcPins, or -Pins): its
    libdatachannel (with its four submodules) and its mbedtls-<version> are each the pinned tree by its digest
    (Get-SourceTreeDigest), and the Mbed TLS archive beside them, where there is one, the pinned archive by its SHA-256.
    Nothing when they are. The two trees' digests come back too, for the record of the build.
    #>
    param([Parameter(Mandatory)] [string] $Sources, $Pins = $WebRtcPins)
    $refusals = [Collections.Generic.List[string]]::new()
    $found = [ordered]@{ libdatachannel = ''; mbedtls = '' }
    $root = [IO.Path]::GetFullPath($Sources)
    $trees = [ordered]@{ libdatachannel = 'libdatachannel'; mbedtls = "mbedtls-$($Pins.mbedtls.version)" }
    foreach ($tree in $trees.GetEnumerator()) {
        $folder = Join-Path $root $tree.Value
        if (-not (Test-Path -LiteralPath $folder -PathType Container)) { $refusals.Add("there is no $($tree.Value) in $root"); continue }
        $found[$tree.Key] = Get-SourceTreeDigest $folder
        if ($found[$tree.Key] -cne $Pins[$tree.Key].tree) {
            $refusals.Add("$($tree.Value) is not the pinned tree (its digest is $($found[$tree.Key]), the pin $($Pins[$tree.Key].tree))")
        }
    }
    $archive = Join-Path $root "mbedtls-$($Pins.mbedtls.version).tar.bz2"
    if ((Test-Path -LiteralPath $archive -PathType Leaf) -and (Get-Sha256 $archive) -cne $Pins.mbedtls.archiveSha256) {
        $refusals.Add("mbedtls-$($Pins.mbedtls.version).tar.bz2 is not the pinned archive")
    }
    return [pscustomobject]@{ Refusals = @($refusals); Trees = $found }
}

function New-WebRtcRecord {
    <#
    The record of a build of libdatachannel and Mbed TLS for $Arch from the pinned sources (whose trees' digests the build
    measured, $Trees): those pins, the script that built them and its SHA-256, the commit of the tree it ran in where it is
    a checkout (else nothing), and every library the build made in $Install's bin and lib, by name, size and SHA-256.
    Written beside the install folder as webrtc-record.json; tools/package.ps1 and the release audit hold a package's
    datachannel.dll to it (Get-WebRtcRecordRefusals).
    #>
    param([Parameter(Mandatory)] [ValidateSet('x64', 'arm64')] [string] $Arch, [Parameter(Mandatory)] $Trees,
        [Parameter(Mandatory)] [string] $Install, [Parameter(Mandatory)] [string] $Script, [string] $Commit = '')
    $made = @(foreach ($folder in 'bin', 'lib') {
        $at = Join-Path $Install $folder
        if (Test-Path -LiteralPath $at) {
            Get-ChildItem -LiteralPath $at -File | Where-Object { $_.Extension -in '.dll', '.lib' } | Sort-Object Name | ForEach-Object {
                [ordered]@{ name = "$folder/$($_.Name)"; bytes = $_.Length; sha256 = Get-Sha256 $_.FullName }
            }
        }
    })
    return [ordered]@{
        schema = 1; product = 'qKVM Desktop'; part = 'webrtc'; architecture = $Arch
        sources = [ordered]@{
            libdatachannel = [ordered]@{ version = $WebRtcPins.libdatachannel.version; commit = $WebRtcPins.libdatachannel.commit
                gitlinks = $WebRtcPins.libdatachannel.gitlinks; tree = $Trees.libdatachannel }
            mbedtls = [ordered]@{ version = $WebRtcPins.mbedtls.version; archiveSha256 = $WebRtcPins.mbedtls.archiveSha256; tree = $Trees.mbedtls }
        }
        script = [ordered]@{ name = 'experiments/webrtc/build-cpp-dependencies.ps1'; sha256 = Get-Sha256 $Script }
        commit = $(if ($Commit) { $Commit } else { $null })
        built = $made
    }
}

function Get-WebRtcRecordRefusals {
    <#
    Why the record of a WebRTC build ($Record, as New-WebRtcRecord wrote it and ConvertFrom-Json read it back) is not of a
    build for $Arch from the pinned sources ($WebRtcPins: libdatachannel's commit, its four submodules' and its tree's
    digest; Mbed TLS's archive and its tree's digest) that made the datachannel.dll $Dll is, by its size and SHA-256.
    Nothing when it is.
    #>
    param($Record, [Parameter(Mandatory)] [ValidateSet('x64', 'arm64')] [string] $Arch, [Parameter(Mandatory)] [string] $Dll)
    $refusals = [Collections.Generic.List[string]]::new()
    # (A field by its path, or nothing where any step of it is missing.)
    $field = {
        param($Object, [string[]] $Path)
        foreach ($name in $Path) {
            if ($null -eq $Object -or $Object -isnot [pscustomobject] -or -not $Object.PSObject.Properties[$name]) { return $null }
            $Object = $Object.$name
        }
        return $Object
    }
    if ((& $field $Record @('schema')) -ne 1 -or [string] (& $field $Record @('part')) -cne 'webrtc') { return @('it is no record of a WebRTC build') }
    if ([string] (& $field $Record @('architecture')) -cne $Arch) { $refusals.Add("it is of a build for $(& $field $Record @('architecture')), not $Arch") }
    $pinned = [ordered]@{
        'libdatachannel commit' = @(@('sources', 'libdatachannel', 'commit'), $WebRtcPins.libdatachannel.commit)
        'libdatachannel tree' = @(@('sources', 'libdatachannel', 'tree'), $WebRtcPins.libdatachannel.tree)
        'Mbed TLS archive' = @(@('sources', 'mbedtls', 'archiveSha256'), $WebRtcPins.mbedtls.archiveSha256)
        'Mbed TLS tree' = @(@('sources', 'mbedtls', 'tree'), $WebRtcPins.mbedtls.tree)
    }
    foreach ($module in $WebRtcPins.libdatachannel.gitlinks.GetEnumerator()) {
        $pinned["$($module.Key) commit"] = @(@('sources', 'libdatachannel', 'gitlinks', $module.Key), $module.Value)
    }
    foreach ($pin in $pinned.GetEnumerator()) {
        if ([string] (& $field $Record $pin.Value[0]) -cne $pin.Value[1]) { $refusals.Add("its $($pin.Key) is not the pinned one") }
    }
    $made = @(@(& $field $Record @('built')) | Where-Object { [string] (& $field $_ @('name')) -ceq 'bin/datachannel.dll' })
    if (-not (Test-Path -LiteralPath $Dll -PathType Leaf)) {
        $refusals.Add("there is no $Dll")
    } elseif ($made.Count -ne 1 -or [string] (& $field $made[0] @('bytes')) -cne [string] (Get-Item -LiteralPath $Dll).Length -or
        [string] (& $field $made[0] @('sha256')) -cne (Get-Sha256 $Dll)) {
        $refusals.Add('the datachannel.dll it names is not this one, by its size and SHA-256')
    }
    return @($refusals)
}

function Get-FFmpegRefusals {
    <#
    Why an FFmpeg configuration line, as FFmpeg keeps it in its libraries, is not the project's minimal build: nothing
    when it is. The minimal build disables everything and finds nothing by itself on the machine it is built on
    (--disable-everything, --disable-autodetect), is LGPL version 3 (--enable-version3), and enables only what
    tools/build_ffmpeg.sh names: no outside library under any name (lib*, openssl, gnutls, mbedtls, schannel and the
    rest), nothing GPL or non-free, no decoder, parser or hardware decoding but its own.
    With -Arch arm64, the build for Windows on ARM: also for ARM64 (--arch=aarch64, for mingw32) and with FFmpeg's
    ARM64 code (its NEON and its other assembly) not turned off: wall tiles decode in software there.
    #>
    param([AllowEmptyString()] [string] $Configuration, [ValidateSet('x64', 'arm64')] [string] $Arch = 'x64')
    # (Its words as configure was given them: a word in single quotes, as FFmpeg writes one with a comma or a blank,
    # is one word.)
    $words = @([regex]::Matches($Configuration, "(?:[^\s']|'[^']*')+") | ForEach-Object { $_.Value.Replace("'", '') })
    $refused = @()
    foreach ($needed in '--disable-everything', '--disable-autodetect', '--enable-version3') {
        if ($words -cnotcontains $needed) { $refused += "it does not say $needed" }
    }
    if ($Arch -eq 'arm64') {
        foreach ($needed in '--arch=aarch64', '--target-os=mingw32') {
            if ($words -cnotcontains $needed) { $refused += "it does not say $needed" }
        }
        foreach ($word in $words | Where-Object { $_ -match '\A--arch=' -and $_ -cne '--arch=aarch64' }) { $refused += "it says $word" }
        # (The words of configure that turn the ARM64 code off: all assembly, or one of its kinds, or the choice at run
        # time of what the processor has.)
        $off = '\A--disable-(asm|inline-asm|neon|armv8|vfp|optimizations|arm-crc|dotprod|i8mm|sve|sve2|runtime-cpudetect)\z'
        foreach ($word in $words | Where-Object { $_ -imatch $off }) { $refused += "it turns FFmpeg's ARM64 code off ($word)" }
        # And the words build_ffmpeg.sh gives, and no other of their kind: llvm-mingw's compiler for ARM64 Windows by
        # its own name, no processor chosen (which would leave out what another has), and no flags of the compiler's but
        # where zlib is (one could turn the vector code off: -mgeneral-regs-only, +nosimd), review arm 1, L2.
        foreach ($named in @(@('--cc=', '--cc=aarch64-w64-mingw32-clang'), @('--cross-prefix=', '--cross-prefix=aarch64-w64-mingw32-'),
                @('--extra-cflags=', '--extra-cflags=-I../zlib'))) {
            $given = @($words | Where-Object { $_.StartsWith($named[0], [StringComparison]::OrdinalIgnoreCase) })
            if ($given.Count -ne 1 -or $given[0] -cne $named[1]) { $refused += "it does not say $($named[1]) alone (it says $($given -join ' '))" }
        }
        foreach ($word in $words | Where-Object { $_ -imatch '\A--(cpu|extra-cxxflags|extra-asflags)=' }) { $refused += "it says $word, which the ARM64 build does not" }
    }
    $plain = @('--enable-shared', '--enable-version3', '--enable-cross-compile', '--enable-w32threads', '--enable-d3d11va',
        '--enable-zlib')
    $lists = @{ decoder = @('h264', 'mjpeg', 'png'); parser = @('h264'); hwaccel = @('h264_d3d11va', 'h264_d3d11va2') }
    foreach ($word in $words | Where-Object { $_ -match '\A--enable-' }) {
        if ($plain -ccontains $word) { continue }
        if ($word -cmatch '\A--enable-(decoder|parser|hwaccel)=(.+)\z') {
            $kind = $Matches[1]
            foreach ($one in $Matches[2].Split(',')) {
                if ($lists[$kind] -cnotcontains $one) { $refused += "it enables the $kind $one, which the minimal build does not" }
            }
            continue
        }
        $refused += "it enables $word, which the minimal build does not"
    }
    return $refused
}

function Test-BareDebugPath {
    <# Whether every symbols path $File names is a bare file name: no folder, no drive, nothing of where it was built. #>
    param([Parameter(Mandatory)] [string] $File)
    foreach ($path in @(Get-PeDebugPaths $File)) {
        if ($path -match '[\\/:]') { return $false }
    }
    return $true
}

function Test-NamesFolder {
    <#
    Whether $File holds the path $Folder anywhere: in capitals or not, with its folder signs as \, \\ or /, as
    UTF-8 text or UTF-16 (either way round). What a build leaves of where it was made.
    #>
    param([Parameter(Mandatory)] [string] $File, [Parameter(Mandatory)] [string] $Folder)
    $plain = { param([string] $text) $text.Replace('/', '\').Replace('\\', '\') }
    $wanted = & $plain $Folder.TrimEnd('\', '/')
    $bytes = [IO.File]::ReadAllBytes($File)
    $readings = @([Text.Encoding]::Latin1.GetString($bytes))
    foreach ($encoding in [Text.Encoding]::Unicode, [Text.Encoding]::BigEndianUnicode) {
        $readings += $encoding.GetString($bytes)
        if ($bytes.Length -gt 1) { $readings += $encoding.GetString($bytes, 1, $bytes.Length - 1) }
    }
    foreach ($reading in $readings) {
        if ((& $plain $reading).IndexOf($wanted, [StringComparison]::OrdinalIgnoreCase) -ge 0) { return $true }
    }
    return $false
}

function Get-TreeDigest {
    <#
    One SHA-256 for a whole folder: of the sorted lines "<sha256> <relative path>" of every file in it. Two
    folders with the same files, byte for byte and by name, have the same digest.
    #>
    param([Parameter(Mandatory)] [string] $Folder)
    $root = (Resolve-Path -LiteralPath $Folder).Path
    $lines = @(Get-ChildItem -LiteralPath $root -Recurse -File -Force | ForEach-Object {
        $relative = [IO.Path]::GetRelativePath($root, $_.FullName).Replace('\', '/')
        '{0} {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $relative
    } | Sort-Object -CaseSensitive)
    $text = [Text.Encoding]::UTF8.GetBytes(($lines -join "`n") + "`n")
    return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($text)).ToLowerInvariant()
}

# The FFmpeg part of a release build (build_release.ps1 -Part ffmpeg, on a PC with WSL) is a folder of exactly: its
# record (this name), one FFmpeg tree for each kind of Windows (x64, arm64: as tools/build_ffmpeg.ps1 made it, with its
# build.json and its export lists, without import libraries) and sources (FFmpeg's and zlib's release archives). The
# build part (build_release.ps1 -Part build, on the build box) takes it only as the record names it.
$FFmpegRecordName = 'ffmpeg-record.json'
$FFmpegKinds = @('x64', 'arm64')
# Where the FFmpeg part keeps its records from release to release unless told another (build_release.ps1 -Records):
# beside its build folder, in the folder of the release roots, which are kept there and never cleaned, the owner's own
# (the coordinator, 2026-11-22): <the folder of -Root>\qkvm-desktop-records\ffmpeg. Each release's record is also kept
# in the private repository, desktop-notes/releases/<version>/ffmpeg-record.json.
$FFmpegRecordsBeside = 'qkvm-desktop-records\ffmpeg'
function Get-FFmpegRecordsFolder([string] $Root) {
    return Join-Path (Split-Path -Parent ([IO.Path]::GetFullPath($Root).TrimEnd('\'))) $FFmpegRecordsBeside
}

function Get-FolderFiles {
    <# Every file under $Folder: its path from there (with /), its size and its SHA-256. #>
    param([Parameter(Mandatory)] [string] $Folder)
    $root = (Resolve-Path -LiteralPath $Folder).Path
    return @(Get-ChildItem -LiteralPath $root -Recurse -File -Force | ForEach-Object {
        [pscustomobject][ordered]@{ path = [IO.Path]::GetRelativePath($root, $_.FullName).Replace('\', '/'); bytes = $_.Length
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
    } | Sort-Object -Property path -CaseSensitive)
}

function Get-FFmpegPinsDigest {
    <#
    One SHA-256 for what an FFmpeg build of $Kind is made from ($Pins: its FFmpeg and zlib archives, each part of its
    toolchain, and the two scripts that build it, each as "<name> <bytes> <sha256>" or "<script> <sha256>"). Two builds
    of the same digest are made from the same bytes by the same steps, and must be the same bytes.
    #>
    param([Parameter(Mandatory)] $Pins, [Parameter(Mandatory)] [string] $Kind)
    $lines = @("kind $Kind", "ffmpeg $($Pins.ffmpeg)", "zlib $($Pins.zlib)") + @(@($Pins.toolchain) | ForEach-Object { "toolchain $_" }) +
        @(@($Pins.scripts) | ForEach-Object { "script $_" })
    $text = [Text.Encoding]::UTF8.GetBytes(($lines -join "`n") + "`n")
    return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($text)).ToLowerInvariant()
}

function New-FFmpegRecord {
    <#
    The record of the FFmpeg part in $Part, as build_release.ps1 -Part ffmpeg writes it beside them: the commit and tree
    it was built from; for each kind, the pins of its sources and toolchain (from its build.json) and of the scripts
    ($Scripts: "<script> <sha256>" each) with their digest, FFmpeg's configuration line, each DLL by name, size and
    SHA-256, and every file of its tree the same way; and the source archives in sources/.
    #>
    param([Parameter(Mandatory)] [string] $Part, [Parameter(Mandatory)] [string] $Version, [Parameter(Mandatory)] [string] $Commit,
        [Parameter(Mandatory)] [string] $Tree, [Parameter(Mandatory)] [string[]] $Scripts, [string] $Wsl = '')
    $said = { param($one) "$($one.archive) $($one.bytes) $($one.sha256)" }
    $builds = foreach ($kind in $FFmpegKinds) {
        $made = Get-Content -LiteralPath (Join-Path $Part "$kind\build.json") -Raw | ConvertFrom-Json
        $pins = [ordered]@{ ffmpeg = & $said $made.ffmpeg; zlib = & $said $made.zlib
            toolchain = @($made.toolchain | ForEach-Object { "$($_.package) $($_.bytes) $($_.sha256)" }); scripts = @($Scripts) }
        $files = @(Get-FolderFiles (Join-Path $Part $kind))
        $build = [ordered]@{ architecture = $kind; pins = $pins; pinsDigest = Get-FFmpegPinsDigest $pins $kind
            configuration = [string] $made.configuration
            dlls = @($files | Where-Object { $_.path -like '*.dll' } | ForEach-Object { [ordered]@{ name = $_.path; bytes = $_.bytes; sha256 = $_.sha256 } })
            files = $files }
        if ($made.PSObject.Properties['features']) { $build['features'] = $made.features }
        $build
    }
    return [ordered]@{
        schema = 1
        product = 'qKVM Desktop'
        part = 'ffmpeg'
        version = $Version
        builtAt = [DateTime]::UtcNow.ToString('o')
        source = [ordered]@{ commit = $Commit; tree = $Tree }
        wsl = $Wsl
        sources = @(Get-FolderFiles (Join-Path $Part 'sources') | ForEach-Object { [ordered]@{ archive = $_.path; bytes = $_.bytes; sha256 = $_.sha256 } })
        builds = @($builds)
        reproduces = @()
    }
}

function Compare-FolderToFiles {
    <# Why the files under $Folder are not exactly $Recorded (path, bytes, sha256 each), each said with $Label: none when they are. #>
    param([Parameter(Mandatory)] [string] $Folder, [object[]] $Recorded = @(), [Parameter(Mandatory)] [string] $Label)
    if (-not (Test-Path -LiteralPath $Folder -PathType Container)) { return @("${Label}: there is no folder $Folder") }
    $wanted = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::Ordinal)
    foreach ($one in $Recorded) { $wanted[[string] $one.path] = $one }
    $refusals = @()
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($one in Get-FolderFiles $Folder) {
        $null = $seen.Add($one.path)
        if (-not $wanted.ContainsKey($one.path)) { $refusals += "${Label}: $($one.path) is not in the record"; continue }
        $was = $wanted[$one.path]
        if ($one.bytes -ne [long] $was.bytes -or $one.sha256 -cne [string] $was.sha256) {
            $refusals += "${Label}: $($one.path) is not the file the record names (its size or its SHA-256 differs)"
        }
    }
    foreach ($path in $wanted.Keys) {
        if (-not $seen.Contains($path)) { $refusals += "${Label}: $path, which the record names, is not there" }
    }
    return $refusals
}

function Get-FFmpegPartRefusals {
    <#
    Why the FFmpeg part in $Folder is not one the build part takes for the export at $Commit (and $Tree): none when it
    is. Its record must be of that commit and tree, of one build for each kind, each build's pins those its digest says
    and each DLL among its files; and the folder must hold exactly what the record names, every file by size and
    SHA-256: the record, each kind's tree and the sources, nothing more and nothing less. The record comes back too.
    #>
    param([Parameter(Mandatory)] [string] $Folder, [Parameter(Mandatory)] [string] $Commit, [Parameter(Mandatory)] [string] $Tree)
    $answer = { param($record, $refusals) [pscustomobject]@{ Record = $record; Refusals = @($refusals) } }
    $file = Join-Path $Folder $FFmpegRecordName
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { return & $answer $null @("there is no $FFmpegRecordName in $Folder") }
    try { $record = Get-Content -LiteralPath $file -Raw | ConvertFrom-Json } catch { return & $answer $null @("$FFmpegRecordName is no JSON") }
    if ($record.schema -ne 1 -or $record.part -cne 'ffmpeg' -or -not $record.PSObject.Properties['builds'] -or
        -not $record.PSObject.Properties['source'] -or -not $record.PSObject.Properties['sources']) {
        return & $answer $null @("$FFmpegRecordName is not the record of an FFmpeg part")
    }
    $refusals = [Collections.Generic.List[string]]::new()
    if ([string] $record.source.commit -cne $Commit) { $refusals.Add("the record is of another commit ($($record.source.commit)); the export is at $Commit") }
    if ([string] $record.source.tree -cne $Tree) { $refusals.Add("the record is of another tree ($($record.source.tree)); the export's is $Tree") }
    $kinds = @($record.builds | ForEach-Object { [string] $_.architecture })
    if ((@($kinds | Sort-Object -CaseSensitive) -join ',') -cne 'arm64,x64') {
        $refusals.Add("the record is not of one build for each kind of Windows, x64 and arm64 (it is of $($kinds -join ', '))")
        return & $answer $record $refusals
    }
    $sources = @($record.sources | ForEach-Object { "$($_.archive) $($_.bytes) $($_.sha256)" })
    foreach ($build in $record.builds) {
        $kind = [string] $build.architecture
        if ((Get-FFmpegPinsDigest $build.pins $kind) -cne [string] $build.pinsDigest) { $refusals.Add("${kind}: its pins are not the ones its digest says") }
        foreach ($archive in $build.pins.ffmpeg, $build.pins.zlib) {
            if ($sources -cnotcontains [string] $archive) { $refusals.Add("${kind}: its source $(([string] $archive).Split(' ')[0]) is not among the record's sources") }
        }
        $files = @($build.files)
        $dlls = @($build.dlls)
        if (-not $dlls.Count) { $refusals.Add("${kind}: the record names no DLL") }
        foreach ($dll in $dlls) {
            if (@($files | Where-Object { $_.path -ceq [string] $dll.name -and [long] $_.bytes -eq [long] $dll.bytes -and $_.sha256 -ceq [string] $dll.sha256 }).Count -ne 1) {
                $refusals.Add("${kind}: the record's $($dll.name) is not among its files as it says")
            }
        }
        if (@($files | Where-Object { $_.path -like '*.dll' }).Count -ne $dlls.Count) { $refusals.Add("${kind}: the record holds a DLL among its files that it does not name") }
        foreach ($one in @(Compare-FolderToFiles (Join-Path $Folder $kind) $files $kind)) { $refusals.Add($one) }
    }
    $named = @($record.sources | ForEach-Object { [pscustomobject]@{ path = [string] $_.archive; bytes = $_.bytes; sha256 = $_.sha256 } })
    foreach ($one in @(Compare-FolderToFiles (Join-Path $Folder 'sources') $named 'sources')) { $refusals.Add($one) }
    foreach ($item in Get-ChildItem -LiteralPath $Folder -Force) {
        if (@($FFmpegKinds + 'sources' + $FFmpegRecordName) -cnotcontains $item.Name) { $refusals.Add("$($item.Name) is not in the record") }
    }
    return & $answer $record $refusals
}

# x64's FFmpeg as 0.1.2's release published it (2026-11-15), the one build of these sources made elsewhere and at
# another time than a release's own two (review r2 of arm and armp, L4): the sources and toolchain 0.1.2's
# tools/build_ffmpeg.ps1 pinned, as that release's build record names them (each "<name> <bytes> <sha256>"), and the
# two DLLs of its x64 ZIP (qKVM-Desktop-0.1.2-windows-x64.zip, SHA-256 9bb0573717f1495c... as its SHA256SUMS names it),
# read out of it. A build of x64 from those very sources and toolchain makes those very DLLs, or it is not built as
# 0.1.2's was. The scripts are not among them: 0.2.0's changed for ARM64, and x64's configuration stayed 0.1.2's.
$FFmpegPublished = [ordered]@{
    release = '0.1.2'
    ffmpeg = 'ffmpeg-9.0.2.tar.xz 12040788 8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e'
    zlib = 'zlib-1.3.2.tar.gz 1502830 bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16'
    toolchain = @(
        'gcc-mingw-w64-x86-64-win32_13.2.0-6ubuntu1+26.1_amd64.deb 35402818 80e1430303e1a619fc9cfc66225893d81486e52801e1916b712d8111d100ae38'
        'gcc-mingw-w64-x86-64-win32-runtime_13.2.0-6ubuntu1+26.1_amd64.deb 14185558 7ac09bc7f283f1148c8e1697373db069c1d4364a9f5fa58bb66c8a93760b8b65'
        'gcc-mingw-w64-base_13.2.0-6ubuntu1+26.1_amd64.deb 188046 a30c07e807e9874ea93909448c5d6d37f9569ebe60ad32de8e490aad198538a3'
        'binutils-mingw-w64-x86-64_2.41.90.20240122-1ubuntu1+11.4_amd64.deb 6053282 28bfabbf64a68813a56360bca056072af9ff71ef4090a4b8a8a085afcda3e4dd'
        'mingw-w64-x86-64-dev_11.0.1-3build1_all.deb 3786378 018480a62907fcca19c6bf2975ae01f5c92c23dee8c3cf7a8355c10bf7d42c6f'
        'mingw-w64-common_11.0.1-3build1_all.deb 5580230 4a42fd30fbc3838c58c929d8a4882682fc12408bb7a0e6cafa913c0f0a57c187'
        'make_4.3-4.1build2_amd64.deb 179752 1fe6a815b56c7b6e9ce4086a363f09444bbd0a0d30e230c453d0b78e44b57a99'
        'nasm_2.16.01-1build1_amd64.deb 458884 22eede0f2dd62343b0298182f62f7485704fe02f166395b02c92a8883377e0b3'
        'libmpc3_1.3.1-1build1_amd64.deb 54520 34af1193734f40723c7fc03d855d98c107d8b62bd2fa17dae8b09d2369cdd45e'
        'libisl23_0.26-3build1_amd64.deb 679596 24b1aa0e1772080fd23df7bed10aadcbbe16b3181b77d3a013c14dfaf9848541')
    dlls = [ordered]@{ 'avcodec-63.dll' = '2177024 27e3c113ed2fce172bdbac4227b28c9c3d8ad0898455cf4dcf13b345aeb92abf'
        'avutil-61.dll' = '1040896 605e96998a3570e1d7547eada6ab94d81a1777a0145fccf59000d0950bfb16fe' }
}

function Get-FFmpegPublishedRefusals {
    <#
    Why the x64 build of the FFmpeg part's $Record is not what 0.1.2's release published ($FFmpegPublished): where its
    FFmpeg and zlib archives and every part of its toolchain are 0.1.2's (the toolchain in any order), its DLLs must be
    0.1.2's two, each by name, size and SHA-256, none fewer and none more; each one that differs is named. None where they
    are, or where its sources or toolchain are not 0.1.2's (then nothing is compared). Applies says whether they were
    compared. Windows on Arm's build is never compared: 0.2.0 was its first.
    #>
    param([Parameter(Mandatory)] $Record, $Published = $FFmpegPublished)
    $answer = { param([bool] $applies, $refusals) [pscustomobject]@{ Applies = $applies; Refusals = @($refusals) } }
    $x64 = @(@($Record.builds) | Where-Object { [string] $_.architecture -ceq 'x64' })
    if ($x64.Count -ne 1) { return & $answer $false @() }
    $pins = $x64[0].pins
    $toolchain = (@(@($pins.toolchain) | ForEach-Object { [string] $_ }) | Sort-Object -CaseSensitive) -join '|'
    if ([string] $pins.ffmpeg -cne $Published.ffmpeg -or [string] $pins.zlib -cne $Published.zlib -or
        $toolchain -cne ((@($Published.toolchain) | Sort-Object -CaseSensitive) -join '|')) {
        return & $answer $false @()
    }
    $of = "$($Published.release)'s release"
    $made = [Collections.Generic.Dictionary[string, string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($dll in @($x64[0].dlls)) { $made[([string] $dll.name).Split('/')[-1]] = "$([long] $dll.bytes) $([string] $dll.sha256)" }
    $refusals = @()
    foreach ($name in $Published.dlls.Keys) {
        if (-not $made.ContainsKey($name)) {
            $refusals += "x64: $name, which $of published from the same sources and toolchain, was not made"
        } elseif ($made[$name] -cne $Published.dlls[$name]) {
            $refusals += "x64: $name is not the one $of published from the same sources and toolchain (its size or its SHA-256 differs: made $($made[$name]), published $($Published.dlls[$name]))"
        }
    }
    foreach ($name in @($made.Keys | Where-Object { -not $Published.dlls.Contains($_) })) {
        $refusals += "x64: $name was made, and $of published none of that name from the same sources and toolchain"
    }
    return & $answer $true $refusals
}

function Get-FFmpegReproductionRefusals {
    <#
    Why the FFmpeg part's $Record is not what the earlier records ($Earlier: Name and Record each) say a build of the same
    pins makes: for each kind, every earlier build of that kind whose pins have the same digest (worked out anew from
    them) must have made the same files, byte for byte (each by path, size and SHA-256). None when it is; the earlier
    builds it matched come back as Matched ("<name> (<kind>)"). A build of other pins is not compared. And x64's build,
    where it is of 0.1.2's sources and toolchain, must have made the DLLs 0.1.2's release published
    (Get-FFmpegPublishedRefusals): Published says it did (review r2 of arm and armp, L4).
    #>
    param([Parameter(Mandatory)] $Record, [object[]] $Earlier = @())
    $refusals = @()
    $matched = @()
    $lines = { param($build) @(@($build.files) | ForEach-Object { "$($_.path) $($_.bytes) $($_.sha256)" } | Sort-Object -CaseSensitive) }
    foreach ($build in @($Record.builds)) {
        $kind = [string] $build.architecture
        $digest = Get-FFmpegPinsDigest $build.pins $kind
        $now = & $lines $build
        foreach ($old in $Earlier) {
            foreach ($was in @($old.Record.builds | Where-Object { [string] $_.architecture -ceq $kind -and (Get-FFmpegPinsDigest $_.pins $kind) -ceq $digest })) {
                $then = & $lines $was
                $differ = @(Compare-Object -ReferenceObject $then -DifferenceObject $now -CaseSensitive | ForEach-Object { ($_.InputObject -split ' ')[0] } |
                    Sort-Object -Unique -CaseSensitive)
                if ($differ.Count) {
                    $refusals += "${kind}: not byte for byte the build of the same pins recorded in $($old.Name) ($($differ -join ', ') differ)"
                } else {
                    $matched += "$($old.Name) ($kind)"
                }
            }
        }
    }
    $published = Get-FFmpegPublishedRefusals -Record $Record
    $refusals += @($published.Refusals)
    return [pscustomobject]@{ Refusals = @($refusals); Matched = @($matched); Published = $published.Applies -and -not @($published.Refusals).Count }
}

function Get-FFmpegDllRefusals {
    <#
    Why the FFmpeg DLLs a package of $Kind holds ($Dlls: path, bytes and sha256 of each) are not the ones the FFmpeg
    part's $Record (build_release.ps1 -Part ffmpeg) made for that kind: none when they are, each by name, size and SHA-256,
    and none more or fewer.
    #>
    param($Record, [Parameter(Mandatory)] [string] $Kind, [object[]] $Dlls = @())
    if ($null -eq $Record -or $Record.part -cne 'ffmpeg' -or -not $Record.PSObject.Properties['builds']) { return @('there is no record of an FFmpeg part') }
    $build = @($Record.builds | Where-Object { [string] $_.architecture -ceq $Kind })
    if ($build.Count -ne 1) { return @("the record holds no build for $Kind") }
    $refusals = @()
    $recorded = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::Ordinal)
    foreach ($dll in @($build[0].dlls)) { $recorded[([string] $dll.name).Split('/')[-1]] = $dll }
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($dll in $Dlls) {
        $name = ([string] $dll.path).Replace('\', '/').Split('/')[-1]
        $null = $seen.Add($name)
        if (-not $recorded.ContainsKey($name)) { $refusals += "$name is not a DLL the FFmpeg part made for $Kind"; continue }
        if ([long] $dll.bytes -ne [long] $recorded[$name].bytes -or [string] $dll.sha256 -cne [string] $recorded[$name].sha256) {
            $refusals += "$name is not the DLL the FFmpeg part made for $Kind (its size or its SHA-256 differs)"
        }
    }
    foreach ($name in $recorded.Keys) { if (-not $seen.Contains($name)) { $refusals += "$name, which the FFmpeg part made for $Kind, is not in the package" } }
    return $refusals
}

# FFmpeg's libraries, by the names a package holds them under (audit_release.ps1 looks for the same).
$FFmpegLibraryPattern = '^(avcodec|avutil|avformat|avfilter|avdevice|swresample|swscale|postproc)-\d+\.dll$'

function Get-ZipFFmpeg {
    <#
    What a release ZIP holds of FFmpeg, read out of it as data (nothing is unpacked or run): every FFmpeg library in it
    (Dlls: its path in the ZIP, size and SHA-256 each, wherever it stands), and the package manifest of its one folder
    $Top (ManifestSha256, '' where there is none) with the FFmpeg libraries that manifest names (Listed, the same way).
    #>
    param([Parameter(Mandatory)] [string] $Zip, [Parameter(Mandatory)] [string] $Top)
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $dlls = @()
    $manifestSha256 = ''
    $listed = @()
    $archive = [IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $Zip).Path)
    try {
        foreach ($entry in $archive.Entries) {
            $name = $entry.FullName
            $isManifest = $name -ceq "$Top/package-manifest.json"
            if (-not $isManifest -and $name.Split('/')[-1] -notmatch $FFmpegLibraryPattern) { continue }
            $held = [IO.MemoryStream]::new()
            $stream = $entry.Open()
            try { $stream.CopyTo($held) } finally { $stream.Dispose() }
            $bytes = $held.ToArray()
            $sha256 = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant()
            if (-not $isManifest) { $dlls += [pscustomobject]@{ path = $name; bytes = [long] $bytes.Length; sha256 = $sha256 }; continue }
            $manifestSha256 = $sha256
            $text = [Text.Encoding]::UTF8.GetString($bytes)
            $manifest = $null
            try { $manifest = $text.TrimStart([char] 0xFEFF) | ConvertFrom-Json } catch { $manifest = $null }
            if ($null -ne $manifest -and $manifest.PSObject.Properties['files']) {
                $listed = @(@($manifest.files) | Where-Object { ([string] $_.path).Split('/')[-1] -match $FFmpegLibraryPattern } | ForEach-Object {
                    [pscustomobject]@{ path = [string] $_.path; bytes = [long] $_.bytes; sha256 = ([string] $_.sha256).ToLowerInvariant() } })
            }
        }
    } finally {
        $archive.Dispose()
    }
    return [pscustomobject]@{ Dlls = @($dlls); ManifestSha256 = $manifestSha256; Listed = @($listed) }
}

function Get-FFmpegKeptRefusals {
    <#
    Why the FFmpeg part that a release build took ($BuildRecord: its build-record.json, schema 2) is not one this PC
    built and kept the record of in $Records (the folder build_release.ps1 -Part ffmpeg keeps them in, from release to
    release, never cleared): none when it is (review armp-1 M1). The folder holds such records alone; exactly one of
    them is the record the build record names by its SHA-256 (dependencies.ffmpeg.recordSha256); the run's own copy of it
    ($RunCopy, the run's ffmpeg-record.json) is that file byte for byte, and the record the build record holds names the
    same files and sources; it is of the run's commit and tree; and for each kind another record kept there, not a copy
    of it, is of a build of the same pins (their digest worked out anew) that made the same files byte for byte, and
    none of the same pins made other bytes: a release builds FFmpeg twice. The kept record comes back too (Record), for
    the FFmpeg DLLs that ship to be held to.
    #>
    param([Parameter(Mandatory)] [string] $Records, [Parameter(Mandatory)] $BuildRecord, [Parameter(Mandatory)] [string] $RunCopy)
    $answer = { param($record, $refusals) [pscustomobject]@{ Record = $record; Refusals = @($refusals); Published = $false } }
    if (-not (Test-Path -LiteralPath $Records -PathType Container)) { return & $answer $null @("there is no folder of FFmpeg records $Records") }
    $ffmpeg = $null
    if ($BuildRecord.PSObject.Properties['dependencies'] -and $BuildRecord.dependencies.PSObject.Properties['ffmpeg']) { $ffmpeg = $BuildRecord.dependencies.ffmpeg }
    if ($null -eq $ffmpeg -or -not $ffmpeg.PSObject.Properties['recordSha256'] -or -not $ffmpeg.PSObject.Properties['record'] -or
        [string] $ffmpeg.recordSha256 -cnotmatch '\A[0-9a-f]{64}\z' -or -not $BuildRecord.PSObject.Properties['source']) {
        return & $answer $null @('the build record names no record of an FFmpeg part by its SHA-256')
    }
    $named = [string] $ffmpeg.recordSha256
    $kept = @()
    foreach ($item in Get-ChildItem -LiteralPath $Records -Force) {
        $was = $null
        if (-not $item.PSIsContainer -and $item.Extension -ieq '.json') {
            try { $was = Get-Content -LiteralPath $item.FullName -Raw | ConvertFrom-Json } catch { $was = $null }
        }
        if ($null -eq $was -or -not $was.PSObject.Properties['part'] -or $was.part -cne 'ffmpeg' -or -not $was.PSObject.Properties['builds'] -or
            -not $was.PSObject.Properties['source'] -or -not $was.PSObject.Properties['sources']) {
            return & $answer $null @("$($item.Name) is not the record of an FFmpeg part: the folder of records holds them alone")
        }
        $kept += [pscustomobject]@{ Name = $item.Name; Sha256 = Get-Sha256 $item.FullName; Record = $was }
    }
    $mine = @($kept | Where-Object { $_.Sha256 -ceq $named })
    if (-not $mine.Count) { return & $answer $null @("the record the run names ($named) is not a record this PC kept in $Records") }
    $record = $mine[0].Record
    $refusals = [Collections.Generic.List[string]]::new()
    if (-not (Test-Path -LiteralPath $RunCopy -PathType Leaf) -or (Get-Sha256 $RunCopy) -cne $named) {
        $refusals.Add("the run's copy of the FFmpeg part's record is not the record the build record names ($RunCopy)")
    }
    $linesOf = { param($build) @(@($build.files) | ForEach-Object { "$($_.path) $($_.bytes) $($_.sha256)" } | Sort-Object -CaseSensitive) -join '|' }
    $buildOf = { param($of, [string] $kind) @(@($of.builds) | Where-Object { [string] $_.architecture -ceq $kind }) }
    $held = $ffmpeg.record
    foreach ($kind in $FFmpegKinds) {
        $here = @(& $buildOf $record $kind)
        $there = @(if ($null -ne $held -and $held.PSObject.Properties['builds']) { & $buildOf $held $kind })
        if ($here.Count -ne 1 -or $there.Count -ne 1 -or (& $linesOf $here[0]) -cne (& $linesOf $there[0])) {
            $refusals.Add("the record the build record holds is not the kept record it names ($kind)")
        }
    }
    $sourcesOf = { param($of) @(@($of.sources) | ForEach-Object { "$($_.archive) $($_.bytes) $($_.sha256)" } | Sort-Object -CaseSensitive) -join '|' }
    if ($null -eq $held -or -not $held.PSObject.Properties['sources'] -or (& $sourcesOf $held) -cne (& $sourcesOf $record)) {
        $refusals.Add('the record the build record holds is not the kept record it names (its sources)')
    }
    if ([string] $record.source.commit -cne [string] $BuildRecord.source.commit) { $refusals.Add("the kept record is of another commit ($($record.source.commit)); the run was built from $($BuildRecord.source.commit)") }
    if ([string] $record.source.tree -cne [string] $BuildRecord.source.tree) { $refusals.Add("the kept record is of another tree ($($record.source.tree)); the run's is $($BuildRecord.source.tree)") }
    # (Another build of the same pins, kept here: never the same file under another name.)
    $others = @($kept | Where-Object { $_.Sha256 -cne $named } | ForEach-Object { [pscustomobject]@{ Name = $_.Name; Record = $_.Record } })
    $reproduced = Get-FFmpegReproductionRefusals -Record $record -Earlier $others
    foreach ($one in $reproduced.Refusals) { $refusals.Add($one) }
    $unproved = @($FFmpegKinds | Where-Object { $kind = $_; -not @($reproduced.Matched | Where-Object { "$_" -clike "* ($kind)" }).Count })
    if ($unproved.Count) { $refusals.Add("no other build of its pins kept here made the same bytes, for $($unproved -join ', '): the release builds FFmpeg twice") }
    # (Published: x64's DLLs are those 0.1.2's release published from the same sources and toolchain, as said.)
    return [pscustomobject]@{ Record = $record; Refusals = @($refusals); Published = [bool] $reproduced.Published }
}

# The record of the ARM64 ZIP's start checks, made on Windows on Arm (tools/test_package_archive.ps1 -Evidence), as a run
# keeps it (tools/release_files.ps1 -Arm64Startup puts it there; release_commands.ps1 asks for it): review armp-1 M3.
$Arm64StartupName = 'arm64-startup.json'

function Get-NativeMachine {
    <# The kind of Windows this PC is, as Windows says it whatever this process runs as (IsWow64Process2's native
    machine): 'arm64' or 'x64'; '' where Windows has no such call or says another. #>
    if (-not ('QkvmNativeMachine' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class QkvmNativeMachine {
    [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool IsWow64Process2(IntPtr process, out ushort processMachine, out ushort nativeMachine);
    public static int Native() {
        try { ushort process, native; return IsWow64Process2(GetCurrentProcess(), out process, out native) ? native : 0; }
        catch (EntryPointNotFoundException) { return 0; }
    }
}
'@
    }
    switch ([QkvmNativeMachine]::Native()) { 0xAA64 { return 'arm64' } 0x8664 { return 'x64' } default { return '' } }
}

# The files of the desktop's tools folder the ARM64 start checks run: tools/test_package_archive.ps1, the package's
# checks it calls (tools/test_package.ps1), what both take in (tools/build_common.ps1) and the table of protections they
# hold each image to (tools/image-protections.json). The record of those checks names each by its SHA-256, and a release
# takes only a record made by its own export's (review r2 of arm and armp, L2).
$StartupCheckTools = @('test_package_archive.ps1', 'test_package.ps1', 'build_common.ps1', 'image-protections.json')

function Get-StartupCheckTools {
    <# The SHA-256 of each file of $StartupCheckTools in the tools folder $Folder, by its name in the desktop's tree
    (tools/<name>), in that order; a throw where one is not there. #>
    param([Parameter(Mandatory)] [string] $Folder)
    $tools = [ordered]@{}
    foreach ($name in $StartupCheckTools) {
        $file = Join-Path $Folder $name
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "There is no $name in ${Folder}: it is not the desktop's tools folder." }
        $tools["tools/$name"] = Get-Sha256 $file
    }
    return $tools
}

function Get-StartupToolDifferences {
    <# The tools of the start checks by which $Said (a record's tools: an object read from JSON, or a dictionary, of a
    name to a SHA-256) is not $Wanted: each name either lacks, or names with another SHA-256 or names more. None when
    they are the same. #>
    param($Said, [Parameter(Mandatory)] $Wanted)
    $read = {
        param($Of)
        $map = [Collections.Generic.Dictionary[string, string]]::new([StringComparer]::Ordinal)
        if ($Of -is [Collections.IDictionary]) { foreach ($key in $Of.Keys) { $map["$key"] = "$($Of[$key])" } }
        elseif ($Of -is [pscustomobject]) { foreach ($property in $Of.PSObject.Properties) { $map[$property.Name] = "$($property.Value)" } }
        return , $map
    }
    $said = & $read $Said
    $wanted = & $read $Wanted
    return @(@($wanted.Keys | Where-Object { -not $said.ContainsKey($_) -or $said[$_] -cne $wanted[$_] }) +
        @($said.Keys | Where-Object { -not $wanted.ContainsKey($_) }))
}

function New-StartupEvidence {
    <#
    The record of a package's start checks that passed (tools/test_package_archive.ps1 -Evidence writes it once every
    check has): the tools that ran them ($Tools: Get-StartupCheckTools of the folder they ran from, read before they
    ran), the ZIP by name, size and SHA-256, the package's kind and version as its manifest ($Manifest) says them, and the
    kind of Windows they ran on (Get-NativeMachine) and that this process ran as.
    #>
    param([Parameter(Mandatory)] [string] $Zip, [Parameter(Mandatory)] $Manifest, [Parameter(Mandatory)] $Tools)
    $file = Get-Item -LiteralPath $Zip
    $system = Get-CimInstance -ClassName Win32_OperatingSystem -ErrorAction SilentlyContinue
    return [ordered]@{
        schema = 1
        product = 'qKVM Desktop'
        check = 'tools/test_package_archive.ps1'
        tools = $Tools
        passed = $true
        archive = $file.Name
        bytes = $file.Length
        sha256 = Get-Sha256 $file.FullName
        architecture = [string] $Manifest.architecture
        version = [string] $Manifest.version
        windows = [ordered]@{ machine = Get-NativeMachine; process = ([Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture).ToString().ToLowerInvariant()
            caption = $(if ($system) { [string] $system.Caption } else { '' }); version = $(if ($system) { [string] $system.Version } else { '' }) }
        at = [DateTime]::UtcNow.ToString('o')
    }
}

function Get-Arm64StartupRefusals {
    <#
    Why $Evidence does not show that the ARM64 package's start checks passed on Windows on Arm for exactly the ZIP named
    $Name of $Bytes and $Sha256 (a run's ARM64 ZIP), made by the run's export's tools ($Tools, Get-StartupCheckTools of
    them): none when it does (review armp-1 M3; review r2 of arm and armp, L2). It must be a record of
    tools/test_package_archive.ps1 (New-StartupEvidence's, schema 1) that says the checks passed, made on Windows on
    Arm, of an ARM64 package, of that ZIP by name, size and SHA-256, and that names exactly those tools by their SHA-256.
    #>
    param([string] $Evidence = '', [Parameter(Mandatory)] [string] $Name, [long] $Bytes, [string] $Sha256 = '', [Parameter(Mandatory)] $Tools)
    if (-not $Evidence -or -not (Test-Path -LiteralPath $Evidence -PathType Leaf)) { return @("there is no record of the ARM64 package's start checks") }
    $said = $null
    try { $said = Get-Content -LiteralPath $Evidence -Raw | ConvertFrom-Json } catch { return @("$Evidence is no JSON") }
    $has = { param($name) $null -ne $said -and $null -ne $said.PSObject.Properties[$name] }
    if (-not (& $has 'schema') -or $said.schema -ne 1 -or -not (& $has 'check') -or [string] $said.check -cne 'tools/test_package_archive.ps1') {
        return @("$Evidence is not a record of tools/test_package_archive.ps1")
    }
    $refusals = @()
    if (-not (& $has 'passed') -or $said.passed -isnot [bool] -or -not $said.passed) { $refusals += 'its record does not say the checks passed' }
    $machine = if ((& $has 'windows') -and $null -ne $said.windows.PSObject.Properties['machine']) { [string] $said.windows.machine } else { '' }
    if ($machine -cne 'arm64') { $refusals += "they were not made on Windows on Arm (on $(if ($machine) { $machine } else { 'a PC it does not name' }))" }
    if (-not (& $has 'architecture') -or [string] $said.architecture -cne 'arm64') { $refusals += 'its record is not of an ARM64 package' }
    if (-not (& $has 'archive') -or [string] $said.archive -cne $Name) { $refusals += "its record is not of $Name" }
    elseif (-not (& $has 'bytes') -or -not (& $has 'sha256') -or [long] $said.bytes -ne $Bytes -or [string] $said.sha256 -cne $Sha256) {
        $refusals += 'its record is not of this ZIP (its size or its SHA-256 differs)'
    }
    if (-not (& $has 'tools')) {
        $refusals += 'its record does not name the tools that made it (an earlier tools/test_package_archive.ps1 made it)'
    } else {
        $other = @(Get-StartupToolDifferences -Said $said.tools -Wanted $Tools)
        if ($other.Count) { $refusals += "they were made by other tools than this run's export's ($($other -join ', ') differ)" }
    }
    return $refusals
}

function Get-ReleaseFileNames {
    <#
    The files a release publishes (tools/release_files.ps1), and nothing else: the portable ZIP of each kind of Windows
    (x64, and ARM64 for Windows on Arm), the one setup for both, the source, the third-party sources and SHA256SUMS; and
    the owner's signature of SHA256SUMS.
    #>
    param([Parameter(Mandatory)] [string] $Version, [switch] $Signed)
    $names = @((Get-ZipName $Version 'x64'), (Get-ZipName $Version 'arm64'), (Get-SetupName $Version),
        "qKVM-Desktop-$Version-source.zip", "qKVM-Desktop-$Version-third-party-sources.zip", 'SHA256SUMS')
    if ($Signed) { $names += 'SHA256SUMS.sig' }
    return $names
}

function Assert-ReleaseFolder {
    <#
    Throws unless $Folder holds the release files of $Version and nothing else: no folder, no other file, and by name
    never a program (.exe) but the setup of $Version, never symbols (.pdb) or a latest.json (which holds this PC's
    paths). With -Signed the owner's SHA256SUMS.sig may be there too.
    #>
    param([Parameter(Mandatory)] [string] $Folder, [Parameter(Mandatory)] [string] $Version, [switch] $Signed)
    $allowed = Get-ReleaseFileNames $Version -Signed:$Signed
    $present = @(Get-ChildItem -LiteralPath $Folder -Force)
    foreach ($item in $present) {
        if ($item.PSIsContainer) { throw "A release holds no folder: $($item.Name)" }
        if (($item.Extension -ieq '.exe' -and $item.Name -cne (Get-SetupName $Version)) -or $item.Extension -ieq '.pdb' -or
            $item.Name -ieq 'latest.json') {
            throw "Never a release file: $($item.Name)"
        }
        if ($allowed -cnotcontains $item.Name) { throw "Not a release file: $($item.Name)" }
    }
    foreach ($name in $allowed | Where-Object { $_ -cne 'SHA256SUMS.sig' }) {
        if (@($present | Where-Object Name -ceq $name).Count -ne 1) { throw "The release lacks $name." }
    }
}

function Assert-ReleaseZip {
    <#
    Throws unless the ZIP holds one folder, $Top, with qkvm-desktop.exe the one program in it, and no symbols, no
    latest.json and no path that leaves the folder.
    #>
    param([Parameter(Mandatory)] [string] $Zip, [Parameter(Mandatory)] [string] $Top)
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $Zip).Path)
    try {
        $program = $false
        foreach ($entry in $archive.Entries) {
            $name = $entry.FullName
            $parts = $name.TrimEnd('/').Split('/')
            if (-not $name.StartsWith("$Top/", [StringComparison]::Ordinal) -or $name.Contains('\') -or $name.Contains(':') -or
                @($parts | Where-Object { $_ -in '', '.', '..' }).Count) {
                throw "The ZIP holds a path outside its one folder: $name"
            }
            $leaf = $parts[-1]
            if ($leaf -like '*.pdb' -or $leaf -ieq 'latest.json') { throw "The ZIP holds $name, which is never handed out." }
            if ($leaf -like '*.exe') {
                if ($name -cne "$Top/qkvm-desktop.exe") { throw "The ZIP holds a program that is not handed out: $name" }
                $program = $true
            }
        }
        if (-not $program) { throw 'The ZIP holds no qkvm-desktop.exe.' }
    } finally {
        $archive.Dispose()
    }
}

function Get-Sha256 {
    param([Parameter(Mandatory)] [string] $File)
    return (Get-FileHash -LiteralPath $File -Algorithm SHA256).Hash.ToLowerInvariant()
}
