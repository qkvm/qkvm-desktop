# What the release tools share. They take it in with:  . "$PSScriptRoot/release_common.ps1"

# The version a portable ZIP's program says it is: its one qkvm-desktop.exe, at the ZIP's top or in its one folder
# (package.ps1 writes "<name>/qkvm-desktop.exe"). That one entry is copied out into a new folder in $Beside, its
# version read from the copy's resources (Windows maps it as data; it is never run), and the folder removed. Nothing
# else of the ZIP is unpacked.
function Get-PortableProgramVersion {
    param([Parameter(Mandatory)][string] $Zip, [Parameter(Mandatory)][string] $Beside)
    try {
        $archive = [IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $Zip).Path)
    } catch {
        throw 'The package is not a ZIP that can be read.'
    }
    $look = Join-Path $Beside ('.version-' + [Guid]::NewGuid().ToString('N'))
    try {
        $programs = @($archive.Entries | Where-Object { $_.FullName -cmatch '\A([^/\\]+[/\\])?qkvm-desktop\.exe\z' })
        if ($programs.Count -ne 1) {
            throw 'A portable ZIP holds exactly one qkvm-desktop.exe, at its top or in its one folder.'
        }
        New-Item -ItemType Directory -Path $look | Out-Null
        $copy = Join-Path $look 'qkvm-desktop.exe'
        [IO.Compression.ZipFileExtensions]::ExtractToFile($programs[0], $copy)
        return [string] [Diagnostics.FileVersionInfo]::GetVersionInfo($copy).ProductVersion
    } finally {
        $archive.Dispose()
        if (Test-Path -LiteralPath $look) { Remove-Item -LiteralPath $look -Recurse -Force }
    }
}

# The kind of Windows a program is for, by its PE header's machine, read from its bytes (never run): 'x64' (0x8664),
# 'arm64' (0xAA64), or what else it says ('machine 0x....', or 'no Windows program').
function Get-PeArchitecture([byte[]] $Bytes) {
    if ($null -eq $Bytes -or $Bytes.Length -lt 0x40 -or $Bytes[0] -ne 0x4D -or $Bytes[1] -ne 0x5A) { return 'no Windows program' }
    $at = [BitConverter]::ToInt32($Bytes, 0x3C)
    if ($at -lt 0 -or $at + 6 -gt $Bytes.Length -or $Bytes[$at] -ne 0x50 -or $Bytes[$at + 1] -ne 0x45 -or
        $Bytes[$at + 2] -ne 0 -or $Bytes[$at + 3] -ne 0) { return 'no Windows program' }
    $machine = [BitConverter]::ToUInt16($Bytes, $at + 4)
    switch ($machine) {
        0x8664 { return 'x64' }
        0xAA64 { return 'arm64' }
    }
    return 'machine 0x{0:X4}' -f $machine
}

# The kind of Windows a portable ZIP's one qkvm-desktop.exe is for (Get-PeArchitecture), read from that one entry's
# bytes in the ZIP: nothing is unpacked or run.
function Get-PortableProgramArchitecture {
    param([Parameter(Mandatory)][string] $Zip)
    try {
        $archive = [IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $Zip).Path)
    } catch {
        throw 'The package is not a ZIP that can be read.'
    }
    try {
        $programs = @($archive.Entries | Where-Object { $_.FullName -cmatch '\A([^/\\]+[/\\])?qkvm-desktop\.exe\z' })
        if ($programs.Count -ne 1 -or $programs[0].Length -gt 256MB) {
            throw 'A portable ZIP holds exactly one qkvm-desktop.exe, at its top or in its one folder.'
        }
        $stream = $programs[0].Open()
        try {
            $read = [IO.MemoryStream]::new()
            $stream.CopyTo($read)
            return Get-PeArchitecture $read.ToArray()
        } finally { $stream.Dispose() }
    } finally { $archive.Dispose() }
}

# --- The release key, and what is signed with it: run by the owner only (release_key.ps1, sign_release.ps1,
# refresh_feed.ps1). The key is made, held and used here and nowhere else; tests use throwaway keys only. ---

# The one name the key's file has: the exporter and check_staged.py refuse it with any extension, and .p8 itself.
$ReleaseKeyName = 'qkvm-desktop-release-key.p8'
# PBKDF2-HMAC-SHA256 rounds that make the AES-256 key of the key file from the passphrase.
$ReleaseKeyRounds = 600000
$ReleaseKeyShortest = 16
$ReleaseKeyCurve = '1.2.840.10045.3.1.7'

# The passphrase is typed by a person, into this console, when asked: it is never read from a pipe, a file, the
# command line or the environment. A run whose input is not a console (a script, a pipe, an agent) is refused first.
function Assert-PersonAtConsole {
    if ([Console]::IsInputRedirected) {
        throw 'Run this in a console window, and type the passphrase there when asked. It is never read from a pipe, a file, the command line or the environment.'
    }
}

function Test-InsideRepository([string] $Folder) {
    $at = [IO.DirectoryInfo]::new([IO.Path]::GetFullPath($Folder))
    while ($null -ne $at) {
        if (Test-Path -LiteralPath (Join-Path $at.FullName '.git')) { return $true }
        $at = $at.Parent
    }
    return $false
}

# Where a new key goes: a folder that is there, inside no git repository, with no key in it yet.
function Assert-KeyPlace([string] $Directory) {
    $full = [IO.Path]::GetFullPath($Directory)
    if (-not (Test-Path -LiteralPath $full -PathType Container)) { throw "There is no folder $full. Make it first." }
    if (Test-InsideRepository $full) {
        throw "$full is inside a git repository. Keep the release key outside every repository."
    }
    $file = Join-Path $full $ReleaseKeyName
    if (Test-Path -LiteralPath $file) { throw "$file is there already. It was left as it is." }
    return $file
}

# A new file with these bytes, never over one that is there.
function Write-NewFile([string] $Path, [byte[]] $Bytes) {
    $stream = [IO.File]::Open([IO.Path]::GetFullPath($Path), [IO.FileMode]::CreateNew, [IO.FileAccess]::Write,
        [IO.FileShare]::None)
    try { $stream.Write($Bytes, 0, $Bytes.Length) } finally { $stream.Dispose() }
}

function Assert-NewPlace([string] $Path) {
    $full = [IO.Path]::GetFullPath($Path)
    if (Test-Path -LiteralPath $full) { throw "$full is there already. It was left as it is." }
    return $full
}

# The passphrase as letters for the one call that needs them, wiped afterwards. Its letters never become a .NET
# string here (a string cannot be wiped). $With gets the letters as its one argument.
function Use-Passphrase([Security.SecureString] $Passphrase, [scriptblock] $With) {
    $letters = [char[]]::new($Passphrase.Length)
    $held = [Runtime.InteropServices.Marshal]::SecureStringToGlobalAllocUnicode($Passphrase)
    try {
        [Runtime.InteropServices.Marshal]::Copy($held, $letters, 0, $letters.Length)
        $came = & $With $letters
        return , $came
    } finally {
        [Runtime.InteropServices.Marshal]::ZeroFreeGlobalAllocUnicode($held)
        [Array]::Clear($letters, 0, $letters.Length)
    }
}

# Asked for in the console, never shown (each letter is a star). A new key's is asked twice and must be long.
function Read-ReleasePassphrase([switch] $New) {
    $first = Read-Host -AsSecureString -Prompt 'Passphrase of the release key'
    if (-not $New) { return $first }
    if ($first.Length -lt $ReleaseKeyShortest) {
        $first.Dispose()
        throw "Use a passphrase of at least $ReleaseKeyShortest characters (a few words). Nothing was made."
    }
    $again = Read-Host -AsSecureString -Prompt 'The same passphrase again'
    try {
        $same = Use-Passphrase $first {
            param($one)
            Use-Passphrase $again {
                param($other)
                if ($one.Length -ne $other.Length) { return $false }
                $differ = 0
                for ($at = 0; $at -lt $one.Length; $at++) { $differ = $differ -bor ([int] $one[$at] -bxor [int] $other[$at]) }
                return $differ -eq 0
            }
        }
    } finally { $again.Dispose() }
    if (-not $same) {
        $first.Dispose()
        throw 'The two passphrases differ. Nothing was made.'
    }
    return $first
}

function Get-PublicKeyText([byte[]] $Point) {
    return [Convert]::ToHexString($Point).ToLowerInvariant()
}

# A new P-256 key, written as an encrypted PKCS#8 file (PEM: PBES2, PBKDF2-HMAC-SHA256, AES-256-CBC) and opened again
# with the passphrase before this says it is made. Says the key file, the public key the program pins (X then Y, 128
# hexadecimal characters) and its SHA-256 fingerprint.
function New-ReleaseKey {
    param([Parameter(Mandatory)][string] $Directory, [Parameter(Mandatory)][Security.SecureString] $Passphrase)
    $file = Assert-KeyPlace $Directory
    if ($Passphrase.Length -lt $ReleaseKeyShortest) {
        throw "Use a passphrase of at least $ReleaseKeyShortest characters (a few words). Nothing was made."
    }
    $made = [Security.Cryptography.ECDsa]::Create([Security.Cryptography.ECCurve+NamedCurves]::nistP256)
    try {
        $rounds = [Security.Cryptography.PbeParameters]::new([Security.Cryptography.PbeEncryptionAlgorithm]::Aes256Cbc,
            [Security.Cryptography.HashAlgorithmName]::SHA256, $ReleaseKeyRounds)
        $pem = Use-Passphrase $Passphrase { param($letters) [string] $made.ExportEncryptedPkcs8PrivateKeyPem($letters, $rounds) }
        $public = $made.ExportParameters($false)
        $point = [byte[]] ($public.Q.X + $public.Q.Y)
    } finally { $made.Dispose() }
    Write-NewFile $file ([Text.Encoding]::ASCII.GetBytes(($pem -replace "`r`n", "`n") + "`n"))
    $publicText = Get-PublicKeyText $point
    $opened = Open-ReleaseKey -Key $file -Passphrase $Passphrase -PublicKey $publicText
    $opened.Dispose()
    return [pscustomobject]@{
        File = $file
        PublicKey = $publicText
        Fingerprint = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($point)).ToLowerInvariant()
    }
}

# The release key, opened with its passphrase: an encrypted PKCS#8 P-256 key whose public part is the one the program
# pins, or nothing is signed. The caller disposes of it.
function Open-ReleaseKey {
    param([Parameter(Mandatory)][string] $Key, [Parameter(Mandatory)][Security.SecureString] $Passphrase,
        [Parameter(Mandatory)][string] $PublicKey)
    if ($PublicKey -notmatch '\A[0-9a-fA-F]{128}\z') { throw 'The public key is 128 hexadecimal characters.' }
    $item = Get-Item -LiteralPath $Key
    if ($item.PSIsContainer -or $item.Length -gt 8KB) { throw 'That is not a release key file.' }
    $text = [IO.File]::ReadAllText($item.FullName, [Text.Encoding]::ASCII)
    if ($text -notmatch '\A-----BEGIN (?<label>[A-Z ]+)-----\r?\n' -or $Matches.label -cne 'ENCRYPTED PRIVATE KEY') {
        throw 'The release key must be an encrypted PKCS#8 key file, as release_key.ps1 writes it. Nothing was signed.'
    }
    $opened = [Security.Cryptography.ECDsa]::Create()
    try {
        try {
            Use-Passphrase $Passphrase { param($letters) $opened.ImportFromEncryptedPem($text, $letters) } | Out-Null
        } catch {
            throw 'The passphrase does not open this key. Nothing was signed.'
        }
        $public = $opened.ExportParameters($false)
        if ($public.Curve.Oid.Value -cne $ReleaseKeyCurve) { throw 'The release key is not a P-256 key. Nothing was signed.' }
        if ((Get-PublicKeyText ([byte[]] ($public.Q.X + $public.Q.Y))) -cne $PublicKey.ToLowerInvariant()) {
            throw 'This key is not the one the program pins (QKVM_UPDATE_PUBLIC_KEY). Nothing was signed.'
        }
        return $opened
    } catch {
        $opened.Dispose()
        throw
    }
}

# Whether a 64-byte P1363 signature of these bytes checks against the public key alone.
function Test-ReleaseSignature([byte[]] $Data, [byte[]] $Signature, [string] $PublicKey) {
    if ($null -eq $Signature -or $Signature.Length -ne 64 -or $PublicKey -notmatch '\A[0-9a-fA-F]{128}\z') { return $false }
    $point = [Convert]::FromHexString($PublicKey)
    $q = [Security.Cryptography.ECPoint]::new()
    $q.X = $point[0..31]
    $q.Y = $point[32..63]
    $parameters = [Security.Cryptography.ECParameters]::new()
    $parameters.Curve = [Security.Cryptography.ECCurve+NamedCurves]::nistP256
    $parameters.Q = $q
    $verifier = [Security.Cryptography.ECDsa]::Create()
    try {
        $verifier.ImportParameters($parameters)
        return $verifier.VerifyData($Data, $Signature, [Security.Cryptography.HashAlgorithmName]::SHA256,
            [Security.Cryptography.DSASignatureFormat]::IeeeP1363FixedFieldConcatenation)
    } finally { $verifier.Dispose() }
}

# A 64-byte P1363 signature (r || s) of these exact bytes, checked at once against the public key alone.
function Get-ReleaseSignature([Security.Cryptography.ECDsa] $Signer, [byte[]] $Data, [string] $PublicKey) {
    $signature = $Signer.SignData($Data, [Security.Cryptography.HashAlgorithmName]::SHA256,
        [Security.Cryptography.DSASignatureFormat]::IeeeP1363FixedFieldConcatenation)
    if (-not (Test-ReleaseSignature -Data $Data -Signature $signature -PublicKey $PublicKey)) {
        throw 'The new signature does not check against the public key. Nothing was written.'
    }
    return , $signature
}

$UpdatePayloadFields = @('schema', 'product', 'platform', 'channel', 'version', 'published', 'expires', 'package',
    'bytes', 'sha256', 'notes')
# Each kind of release (its platform, as the program compares it, letter for letter: windows-<x64 or arm64>-<kind>)
# and what its package's name ends in: a portable ZIP (stable.json, stable-arm64.json), an MSIX, and qKVM's own setup
# (stable-setup.json, stable-arm64-setup.json). The setup is one for both kinds of Windows: it installs the PC's own.
$UpdatePackageEndings = [ordered]@{
    'windows-x64-portable' = '.zip'
    'windows-x64-msix' = '.msix'
    'windows-x64-setup' = '-setup.exe'
    'windows-arm64-portable' = '.zip'
    'windows-arm64-msix' = '.msix'
    'windows-arm64-setup' = '-setup.exe'
}
# The kinds of Windows a release is for, by the names its platforms and files say them.
$ReleaseArchitectures = @('x64', 'arm64')

# The name of the feed a kind of release is served as, in the folder of the one the program has built in, as the
# program reads it (core/src/update_release.cpp, feedFor): on x64 a copy that qKVM's setup installed reads
# stable-setup.json, every other copy stable.json (0.1.0 to 0.1.2 among them); on Windows on Arm stable-arm64-setup.json
# and stable-arm64.json.
function Get-FeedName([string] $Platform) {
    if ($Platform -ceq 'windows-x64-setup') { return 'stable-setup.json' }
    if ($Platform -ceq 'windows-arm64-setup') { return 'stable-arm64-setup.json' }
    if ($Platform -ceq 'windows-arm64-portable' -or $Platform -ceq 'windows-arm64-msix') { return 'stable-arm64.json' }
    return 'stable.json'
}

# The version a setup says it is, in its file version information (its product version, as Inno Setup writes the
# setup's AppVersion there), read as data: the setup is never run.
function Get-SetupVersion {
    param([Parameter(Mandatory)][string] $Setup)
    $said = [Diagnostics.FileVersionInfo]::GetVersionInfo((Resolve-Path -LiteralPath $Setup).Path).ProductVersion
    if (-not $said) { throw 'The setup says no version in its file version information.' }
    return [string] $said.Trim()
}

function Read-UtcDate([string] $Text) {
    if ($Text -cnotmatch '\A\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z\z') { throw "Not a UTC date: $Text" }
    return [DateTime]::ParseExact($Text, "yyyy-MM-dd'T'HH:mm:ss'Z'", [Globalization.CultureInfo]::InvariantCulture,
        [Globalization.DateTimeStyles]::AdjustToUniversal -bor [Globalization.DateTimeStyles]::AssumeUniversal)
}

function Write-UtcDate([DateTime] $When) { return $When.ToUniversalTime().ToString("yyyy-MM-dd'T'HH:mm:ss'Z'") }

# A release payload as prepare_update_payload.ps1 writes it and the program takes it: UTF-8 without a byte-order mark,
# one object with exactly its eleven fields, each once and of its kind, a package of the platform's kind, and dates
# within 31 days of each other (and current, unless $AllowExpired: a feed that is being refreshed). Its fields, or why
# it will not do.
function Test-UpdatePayload {
    param([Parameter(Mandatory)][byte[]] $Bytes, [switch] $AllowExpired, [DateTime] $Now = [DateTime]::UtcNow)
    $why = 'Not a release payload of qKVM Desktop'
    if ($Bytes.Length -lt 2 -or $Bytes.Length -gt 70KB -or $Bytes[0] -ne 123) { throw "${why}: it is no JSON object." }
    try {
        $text = [Text.UTF8Encoding]::new($false, $true).GetString($Bytes)
        $document = [Text.Json.JsonDocument]::Parse($text)
    } catch { throw "${why}: it is no JSON." }
    try {
        $root = $document.RootElement
        if ($root.ValueKind -ne [Text.Json.JsonValueKind]::Object) { throw "${why}: it is no JSON object." }
        $names = @($root.EnumerateObject() | ForEach-Object { $_.Name })
        if ($names.Count -ne $UpdatePayloadFields.Count -or
            @($UpdatePayloadFields | Where-Object { @($names -ceq $_).Count -ne 1 }).Count -ne 0) {
            throw "${why}: its fields are not exactly $($UpdatePayloadFields -join ', '), each once."
        }
        $value = @{}
        foreach ($property in $root.EnumerateObject()) {
            $kind = $property.Value.ValueKind
            if ($property.Name -cin @('schema', 'bytes')) {
                $whole = [long] 0
                # (A JSON number with a point or an exponent is no Int64 to .NET: 40.0 and 4e1 are refused.)
                if ($kind -ne [Text.Json.JsonValueKind]::Number -or -not $property.Value.TryGetInt64([ref] $whole)) {
                    throw "${why}: $($property.Name) is not a whole number."
                }
                $value[$property.Name] = $whole
            } else {
                if ($kind -ne [Text.Json.JsonValueKind]::String) { throw "${why}: $($property.Name) is not text." }
                $value[$property.Name] = $property.Value.GetString()
            }
        }
    } finally { $document.Dispose() }
    # (Letter for letter: a dictionary of PowerShell's finds its names whatever their case.)
    $ending = if (@($UpdatePackageEndings.Keys) -ccontains $value.platform) { $UpdatePackageEndings[$value.platform] } else { '' }
    if ($value.schema -ne 1 -or $value.product -cne 'qkvm-desktop' -or $value.channel -cne 'stable' -or -not $ending) {
        throw "${why}: schema 1, product qkvm-desktop, channel stable and a platform of $($UpdatePackageEndings.Keys -join ', ')."
    }
    if ($value.version -cnotmatch '\A(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})\z' -or
        @($value.version.Split('.') | Where-Object { [int] $_ -gt 65535 }).Count -ne 0) {
        throw "${why}: the version is three numbers of at most 65535."
    }
    $address = $null
    if (-not [Uri]::TryCreate($value.package, [UriKind]::Absolute, [ref] $address) -or $address.Scheme -cne 'https' -or
        $address.UserInfo -or $address.Query -or $address.Fragment -or $value.package.Length -gt 2048 -or
        -not $address.AbsolutePath.EndsWith($ending, [StringComparison]::OrdinalIgnoreCase)) {
        throw "${why}: the package is a direct HTTPS address of a $ending file."
    }
    if ($value.bytes -lt 1 -or $value.bytes -gt 1GB -or $value.sha256 -cnotmatch '\A[0-9a-f]{64}\z' -or
        $value.notes.Length -gt 16000) {
        throw "${why}: its size, SHA-256 or notes will not do."
    }
    $published = Read-UtcDate $value.published
    $expires = Read-UtcDate $value.expires
    if ($expires -le $published -or $expires -gt $published.AddDays(31)) {
        throw "${why}: it expires within 31 days of its publication."
    }
    if (-not $AllowExpired -and ($expires -le $Now -or $published -gt $Now.AddMinutes(5))) {
        throw "${why}: it is not current (published $($value.published), expires $($value.expires))."
    }
    $fields = [ordered]@{}
    foreach ($name in $UpdatePayloadFields) { $fields[$name] = $value[$name] }
    $fields.schema = [int] $fields.schema
    return $fields
}

# A SHA256SUMS file as sha256sum writes it: ASCII lines of 64 lowercase hexadecimal characters, two spaces and a plain
# file name, each ending in one line feed, each name once. Its names, or why it will not do.
function Test-Sha256Sums([byte[]] $Bytes) {
    $why = 'Not a SHA256SUMS file'
    if ($Bytes.Length -lt 68 -or $Bytes.Length -gt 64KB -or @($Bytes | Where-Object { $_ -gt 127 }).Count -ne 0) {
        throw "${why}: it is not short ASCII text."
    }
    $text = [Text.Encoding]::ASCII.GetString($Bytes)
    if (-not $text.EndsWith("`n") -or $text.Contains("`r")) { throw "${why}: each line ends in one line feed." }
    $names = @()
    foreach ($line in $text.Substring(0, $text.Length - 1).Split("`n")) {
        if ($line -cnotmatch '\A[0-9a-f]{64}  (?<name>[A-Za-z0-9][A-Za-z0-9._+-]*)\z') {
            throw "${why}: a line is not 64 lowercase hexadecimal characters, two spaces and a plain file name."
        }
        if ($names -ccontains $Matches.name) { throw "${why}: $($Matches.name) is named twice." }
        $names += $Matches.name
    }
    return , $names
}

# What a file to be signed is: a release payload or a SHA256SUMS file, and nothing else. Its bytes, read once, what
# they say, and their SHA-256: what is shown before the passphrase is asked for is what is then signed.
function Get-SignableFile([string] $File) {
    $item = Get-Item -LiteralPath $File
    if ($item.PSIsContainer -or $item.Length -gt 70KB) { throw 'Sign a release payload or a SHA256SUMS file, nothing else.' }
    $bytes = [IO.File]::ReadAllBytes($item.FullName)
    if ($bytes.Length -gt 0 -and $bytes[0] -eq 123) {
        $fields = Test-UpdatePayload -Bytes $bytes
        $said = "a release payload: qKVM Desktop $($fields.version), $($fields.platform), $($fields.package), " +
            "$($fields.bytes) bytes, SHA-256 $($fields.sha256), valid until $($fields.expires)"
    } else {
        $names = Test-Sha256Sums $bytes
        $said = "a SHA256SUMS file of $($names.Count) files: $($names -join ', ')"
    }
    $sha256 = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant()
    return [pscustomobject]@{ Bytes = $bytes; Said = $said; Sha256 = $sha256 }
}

# Signs a release payload or a SHA256SUMS file with the release key: a 64-byte P1363 signature written to a new file,
# checked against the pinned public key before it is written. -What: the bytes Get-SignableFile read (and showed), not
# the file again; -File reads it here. The output file's name.
function Invoke-SignRelease {
    [CmdletBinding(DefaultParameterSetName = 'File')]
    param([Parameter(Mandatory)][string] $Key, [Parameter(Mandatory)][string] $PublicKey,
        [Parameter(Mandatory, ParameterSetName = 'File')][string] $File,
        [Parameter(Mandatory, ParameterSetName = 'What')][pscustomobject] $What,
        [Parameter(Mandatory)][string] $Output,
        [Parameter(Mandatory)][Security.SecureString] $Passphrase)
    if ($PSCmdlet.ParameterSetName -ceq 'File') { $What = Get-SignableFile $File }
    $target = Assert-NewPlace $Output
    $signer = Open-ReleaseKey -Key $Key -Passphrase $Passphrase -PublicKey $PublicKey
    try { $signature = Get-ReleaseSignature -Signer $signer -Data $what.Bytes -PublicKey $PublicKey } finally { $signer.Dispose() }
    Write-NewFile $target $signature
    return $target
}

# Several files to be signed at once (the owner, 2026-11-19: one passphrase for a release's files): each read once by
# Get-SignableFile, in the order given, and each a release payload or a SHA256SUMS file; none named twice. Where any one
# is not, nothing is: the file and why are said. What is shown is then what is signed, file for file.
function Get-SignableFiles([string[]] $Files) {
    if (-not $Files -or -not @($Files).Count) { throw 'Name the files to sign. Nothing was signed.' }
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $read = @()
    foreach ($file in @($Files)) {
        $full = [IO.Path]::GetFullPath($file)
        if (-not $seen.Add($full)) { throw "$full is named twice. Nothing was signed." }
        try { $what = Get-SignableFile $full } catch { throw "${full}: $($_.Exception.Message) Nothing was signed." }
        $read += [pscustomobject]@{ File = $full; Name = Split-Path -Leaf $full; Bytes = $what.Bytes; Said = $what.Said; Sha256 = $what.Sha256 }
    }
    return , $read
}

# Signs each of several files that Get-SignableFiles read (-What), with one opening of the release key: each signature
# made and checked against the pinned key, all of them, before the first is written, each to a new file of its own
# (-Outputs, in the same order, none twice and none one of the files signed). Where any one cannot be made or written,
# none is left: what this call wrote is taken away again. The names of what was written.
function Invoke-SignReleaseFiles {
    param([Parameter(Mandatory)][string] $Key, [Parameter(Mandatory)][string] $PublicKey,
        [Parameter(Mandatory)][object[]] $What, [Parameter(Mandatory)][string[]] $Outputs,
        [Parameter(Mandatory)][Security.SecureString] $Passphrase)
    if (@($What).Count -lt 1 -or @($What).Count -ne @($Outputs).Count) { throw 'Each file signed has one signature file of its own. Nothing was signed.' }
    $places = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($one in @($What)) { [void] $places.Add([IO.Path]::GetFullPath($one.File)) }
    $targets = @()
    foreach ($output in @($Outputs)) {
        $target = Assert-NewPlace $output
        if (-not $places.Add($target)) { throw "$target is named twice, or is one of the files signed. Nothing was signed." }
        $targets += $target
    }
    $signer = Open-ReleaseKey -Key $Key -Passphrase $Passphrase -PublicKey $PublicKey
    $signatures = [Collections.Generic.List[byte[]]]::new()
    try {
        foreach ($one in @($What)) { $signatures.Add([byte[]] (Get-ReleaseSignature -Signer $signer -Data $one.Bytes -PublicKey $PublicKey)) }
    } finally { $signer.Dispose() }
    $written = @()
    try {
        for ($at = 0; $at -lt $targets.Count; $at++) {
            Write-NewFile $targets[$at] $signatures[$at]
            $written += $targets[$at]
        }
    } catch {
        foreach ($made in $written) { Remove-Item -LiteralPath $made -Force -ErrorAction SilentlyContinue }
        throw "A signature could not be written ($($_.Exception.Message)). None was left."
    }
    return , $written
}

# A feed as assemble_update_feed.ps1 writes it, signed by the pinned key: its payload's bytes.
function Read-SignedFeed([string] $Feed, [string] $PublicKey) {
    $item = Get-Item -LiteralPath $Feed
    if ($item.PSIsContainer -or $item.Length -gt 96KB) { throw 'That is not an update feed.' }
    try {
        $document = [Text.Json.JsonDocument]::Parse([Text.UTF8Encoding]::new($false, $true).GetString([IO.File]::ReadAllBytes($item.FullName)))
    } catch { throw 'That is not an update feed.' }
    try {
        $root = $document.RootElement
        $names = if ($root.ValueKind -eq [Text.Json.JsonValueKind]::Object) { @($root.EnumerateObject() | ForEach-Object { $_.Name }) } else { @() }
        if ($names.Count -ne 2 -or $names -cnotcontains 'payload' -or $names -cnotcontains 'signature' -or
            $root.GetProperty('payload').ValueKind -ne [Text.Json.JsonValueKind]::String -or
            $root.GetProperty('signature').ValueKind -ne [Text.Json.JsonValueKind]::String) {
            throw 'That is not an update feed: it holds exactly a payload and a signature.'
        }
        $payloadText = $root.GetProperty('payload').GetString()
        $signatureText = $root.GetProperty('signature').GetString()
    } finally { $document.Dispose() }
    try {
        $payload = [Convert]::FromBase64String($payloadText)
        $signature = [Convert]::FromBase64String($signatureText)
    } catch { throw 'That is not an update feed: its payload and signature are base64.' }
    if ([Convert]::ToBase64String($payload) -cne $payloadText -or [Convert]::ToBase64String($signature) -cne $signatureText) {
        throw 'That is not an update feed: its base64 is not as the program reads it.'
    }
    if (-not (Test-ReleaseSignature -Data $payload -Signature $signature -PublicKey $PublicKey)) {
        throw 'This feed is not signed by the key the program pins. Nothing was signed.'
    }
    return , $payload
}

# How long before it is made (and so before it is signed) a payload says it was published: a PC whose clock is a
# little behind takes it all the same, where the program refuses one published more than its own 300 s ahead of its
# clock (core/src/update_release.cpp), which stays as it is (i020). Its expiry is counted from the making, as before.
$PublishedBeforeMaking = [TimeSpan]::FromMinutes(10)

# A feed to be signed again with new dates (published $PublishedBeforeMaking before $Now, expiring $ValidDays after it),
# every other field as it was: read and checked (signed by the pinned key, a release payload, expired or not), and the
# exact payload bytes that are then signed made now, so that what is shown before the passphrase is asked for (the
# feed's name, what it is, and the SHA-256 of those bytes) is what is signed. Nothing is signed or written here.
function Get-RefreshedPayload {
    param([Parameter(Mandatory)][string] $Feed, [Parameter(Mandatory)][string] $PublicKey,
        [Parameter(Mandatory)][ValidateRange(1, 30)][int] $ValidDays, [DateTime] $Now = [DateTime]::UtcNow)
    $full = [IO.Path]::GetFullPath($Feed)
    $fields = Read-RefreshableFeed -Feed $full -PublicKey $PublicKey
    $expired = $fields.expires
    $fields.published = Write-UtcDate ($Now - $PublishedBeforeMaking)
    $fields.expires = Write-UtcDate $Now.AddDays($ValidDays)
    $payload = [Text.UTF8Encoding]::new($false).GetBytes(($fields | ConvertTo-Json -Compress))
    $null = Test-UpdatePayload -Bytes $payload -Now $Now
    return [pscustomobject]@{
        File = $full; Name = Split-Path -Leaf $full; Fields = $fields; Expired = $expired; Bytes = $payload
        Sha256 = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($payload)).ToLowerInvariant()
    }
}

# Several feeds signed again at once (the owner, 2026-11-19: one passphrase), from what Get-RefreshedPayload made of
# each (-What): one opening of the release key, each new payload signed and the signature checked against the pinned
# key, every new feed made in memory before the first is written, each to a new file (-Outputs, in the same order, none
# twice and none one of the feeds read). Where any one cannot be made or written, none is left. The names written.
function Invoke-RefreshFeeds {
    param([Parameter(Mandatory)][string] $Key, [Parameter(Mandatory)][string] $PublicKey,
        [Parameter(Mandatory)][object[]] $What, [Parameter(Mandatory)][string[]] $Outputs,
        [Parameter(Mandatory)][Security.SecureString] $Passphrase)
    if (@($What).Count -lt 1 -or @($What).Count -ne @($Outputs).Count) { throw 'Each feed signed again has one new file of its own. Nothing was signed.' }
    $places = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($one in @($What)) {
        if (-not $places.Add([IO.Path]::GetFullPath($one.File))) { throw "$($one.File) is named twice. Nothing was signed." }
    }
    $targets = @()
    foreach ($output in @($Outputs)) {
        $target = Assert-NewPlace $output
        if (-not $places.Add($target)) { throw "$target is named twice, or is one of the feeds read. Nothing was signed." }
        $targets += $target
    }
    $feeds = [Collections.Generic.List[byte[]]]::new()
    $signer = Open-ReleaseKey -Key $Key -Passphrase $Passphrase -PublicKey $PublicKey
    try {
        foreach ($one in @($What)) {
            $signature = Get-ReleaseSignature -Signer $signer -Data $one.Bytes -PublicKey $PublicKey
            $envelope = [ordered]@{
                payload = [Convert]::ToBase64String($one.Bytes)
                signature = [Convert]::ToBase64String($signature)
            } | ConvertTo-Json -Compress
            $encoded = [Text.UTF8Encoding]::new($false).GetBytes($envelope)
            if ($encoded.Length -gt 96KB) { throw "The signed update feed $($one.Name) exceeds the desktop size limit. Nothing was written." }
            $feeds.Add($encoded)
        }
    } finally { $signer.Dispose() }
    $written = @()
    try {
        for ($at = 0; $at -lt $targets.Count; $at++) {
            Write-NewFile $targets[$at] $feeds[$at]
            $written += $targets[$at]
        }
    } catch {
        foreach ($made in $written) { Remove-Item -LiteralPath $made -Force -ErrorAction SilentlyContinue }
        throw "A refreshed feed could not be written ($($_.Exception.Message)). None was left."
    }
    return , $written
}

# The platforms of the feeds a release serves (0.2.0 on): x64's portable and installed copies', and Windows on Arm's.
$ReleaseFeedPlatforms = @('windows-x64-portable', 'windows-x64-setup', 'windows-arm64-portable', 'windows-arm64-setup')

# Why the feeds to be signed again (-What, from Get-RefreshedPayload) are not the set a release serves: none when they
# are (review armp-1 L6). Each names its own kind: one read from a file of a served name holds that name's own platform
# (a feed uploaded under the other kind's name is refused by every copy that reads it); none of a platform no release
# serves, and none twice; one version for all; and the four are all there, unless -Partial says that fewer are signed
# again on purpose (0.1.x's two, say), when the ones left out come back named (Missing): each runs out on its own date.
function Get-FeedSetRefusals {
    param([Parameter(Mandatory)][object[]] $What, [switch] $Partial)
    $refusals = @()
    $served = @($ReleaseFeedPlatforms | ForEach-Object { Get-FeedName $_ })
    $seen = @()
    foreach ($one in @($What)) {
        $platform = [string] $one.Fields.platform
        if ($ReleaseFeedPlatforms -cnotcontains $platform) { $refusals += "$($one.Name): $platform is no feed a release serves"; continue }
        if ($served -ccontains [string] $one.Name -and [string] $one.Name -cne (Get-FeedName $platform)) {
            $refusals += "$($one.Name) holds $platform, which is served as $(Get-FeedName $platform)"
        }
        if ($seen -ccontains $platform) { $refusals += "$platform is there twice" }
        $seen += $platform
    }
    $versions = @(@($What) | ForEach-Object { [string] $_.Fields.version } | Sort-Object -Unique)
    if ($versions.Count -gt 1) { $refusals += "the feeds offer more than one version ($($versions -join ', '))" }
    $missing = @($ReleaseFeedPlatforms | Where-Object { $seen -cnotcontains $_ } | ForEach-Object { Get-FeedName $_ })
    if ($missing.Count -and -not $Partial) {
        $refusals += "not the whole set: $($missing -join ', ') $(if ($missing.Count -eq 1) { 'is' } else { 'are' }) not among them (a release serves four feeds, each of which runs out on its own date; -Partial signs fewer on purpose)"
    }
    return [pscustomobject]@{ Refusals = @($refusals); Missing = @($missing) }
}

# The commands that upload each feed signed again (-Written, in the order of -What) to the bucket behind
# download.qkvm.com, under the name it is served at, never cached: as release_commands.ps1 says them.
function Get-FeedUploadLines {
    param([Parameter(Mandatory)][object[]] $What, [Parameter(Mandatory)][string[]] $Written, [Parameter(Mandatory)][string] $Bucket)
    for ($at = 0; $at -lt @($What).Count; $at++) {
        "npx --yes wrangler@4.152.0 r2 object put `"$Bucket/desktop/$(Get-FeedName $What[$at].Fields.platform)`" --file `"$($Written[$at])`" --content-type `"application/json`" --cache-control `"no-cache`" --remote"
    }
}

# One feed signed again (Get-RefreshedPayload, then Invoke-RefreshFeeds). What the new feed says.
function Invoke-RefreshFeed {
    param([Parameter(Mandatory)][string] $Feed, [Parameter(Mandatory)][string] $Key,
        [Parameter(Mandatory)][string] $PublicKey, [Parameter(Mandatory)][string] $Output,
        [Parameter(Mandatory)][ValidateRange(1, 30)][int] $ValidDays,
        [Parameter(Mandatory)][Security.SecureString] $Passphrase, [DateTime] $Now = [DateTime]::UtcNow)
    $what = Get-RefreshedPayload -Feed $Feed -PublicKey $PublicKey -ValidDays $ValidDays -Now $Now
    $null = Invoke-RefreshFeeds -Key $Key -PublicKey $PublicKey -What @($what) -Outputs @($Output) -Passphrase $Passphrase
    return $what.Fields
}

# The fields of a feed that is to be signed again: signed by the pinned key, and a release payload (expired or not).
function Read-RefreshableFeed([string] $Feed, [string] $PublicKey) {
    $payload = Read-SignedFeed -Feed $Feed -PublicKey $PublicKey
    return Test-UpdatePayload -Bytes $payload -AllowExpired
}
