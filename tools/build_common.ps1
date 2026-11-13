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

# Where installed copies (the setup's) ask for their updates, beside the portable copies' feed (builder UPD, 2026-11-08).
$SetupUpdateFeed = 'https://download.qkvm.com/desktop/stable-setup.json'

function Get-SetupFeedRefusals {
    <#
    Why the payload of installed copies' feed (stable-setup.json: prepare_update_payload.ps1 -Setup, put in its envelope
    and signed by assemble_update_feed.ps1) is not the one of the setup of $Version that was built: no refusal when it
    is. It is one JSON object of exactly the payload's eleven fields, each once and of its kind, of qkvm-desktop's
    stable channel for windows-x64-setup; it offers $Version and names the setup by $Bytes and $Sha256; and it is
    current at $Now: published before it (with five minutes' grace), expiring after it, and within 31 days of its
    publication. Its fields come back too. Its signature is the caller's to check first, and so is the address it
    names: where it is uploaded, or a place of what is published.
    #>
    param([Parameter(Mandatory)] [byte[]] $Payload, [Parameter(Mandatory)] [string] $Version, [long] $Bytes,
        [string] $Sha256, [DateTime] $Now = [DateTime]::UtcNow)
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
    if ($value.platform -cne 'windows-x64-setup') { $refusals.Add("it is not of windows-x64-setup (it is of $($value.platform))") }
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
    return "qKVM-Desktop-$Version-windows-x64-setup.exe"
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

function Get-FFmpegRefusals {
    <#
    Why an FFmpeg configuration line, as FFmpeg keeps it in its libraries, is not the project's minimal build: nothing
    when it is. The minimal build disables everything and finds nothing by itself on the machine it is built on
    (--disable-everything, --disable-autodetect), is LGPL version 3 (--enable-version3), and enables only what
    tools/build_ffmpeg.sh names: no outside library under any name (lib*, openssl, gnutls, mbedtls, schannel and the
    rest), nothing GPL or non-free, no decoder, parser or hardware decoding but its own.
    #>
    param([AllowEmptyString()] [string] $Configuration)
    # (Its words as configure was given them: a word in single quotes, as FFmpeg writes one with a comma or a blank,
    # is one word.)
    $words = @([regex]::Matches($Configuration, "(?:[^\s']|'[^']*')+") | ForEach-Object { $_.Value.Replace("'", '') })
    $refused = @()
    foreach ($needed in '--disable-everything', '--disable-autodetect', '--enable-version3') {
        if ($words -cnotcontains $needed) { $refused += "it does not say $needed" }
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

function Get-ReleaseFileNames {
    <#
    The files a release publishes (tools/release_files.ps1), and nothing else: the portable ZIP, the setup, the source,
    the third-party sources and SHA256SUMS; and the owner's signature of SHA256SUMS.
    #>
    param([Parameter(Mandatory)] [string] $Version, [switch] $Signed)
    $names = @("qKVM-Desktop-$Version-windows-x64.zip", (Get-SetupName $Version), "qKVM-Desktop-$Version-source.zip",
        "qKVM-Desktop-$Version-third-party-sources.zip", 'SHA256SUMS')
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
