#Requires -Version 7.2
<#
.SYNOPSIS
Checks what is published of a release against what was built: every file, SHA256SUMS, its signature and the feeds.

.DESCRIPTION
    pwsh -File tools\verify_release.ps1 -Run <Root>\runs\<time> -From <place>[,<place>...]
        [-Feed <https address or file>] [-SetupFeed <https address or file>] [-RequireSignature]

Run by the owner after publishing (release plan B11, B12), from the run folder build_release.ps1 and
release_files.ps1 wrote (build-record.json, release-files.json), as release_commands.ps1 prints the command.
-From names one place or more, a comma between them (pwsh -File hands "a,b" over as one word); with no place,
or an empty one, nothing is checked and this fails. Each place is where the release files are:
a folder they were downloaded into (it must hold the release files and nothing else), or the address of a
folder of them, as https://github.com/qkvm/qkvm-desktop/releases/download/v<version> (GitHub answers with a
redirect, which is followed) or https://download.qkvm.com/desktop/<version> (the address the program
downloads from: a redirect there is a failure, as it is to the program). For each:
- SHA256SUMS is byte for byte the one release_files.ps1 wrote, and names exactly the release files;
- every release file is there, and is the one written, by SHA-256 (the ZIP and the setup also by the build
  record);
- SHA256SUMS.sig, where it is there (and it must be, with -RequireSignature), is a signature of SHA256SUMS by
  the key the program pins (from the build record).
-Feed (stable.json, at https://download.qkvm.com/desktop/stable.json, never by a redirect, or a file): its
signature by that key, and its payload: qkvm-desktop, stable, windows-x64-portable, this version, the ZIP by
name, size and SHA-256 at an https address on the host of one of the places (the direct host's), published
before now and valid for no more than 31 days from it; and the ZIP at the address it names is fetched, as the
program fetches it, and must be the one built, by size and SHA-256. (Nothing is asked of a host that is not
one of the places or the feed's.)
-SetupFeed (stable-setup.json, installed copies' feed, at https://download.qkvm.com/desktop/stable-setup.json, or a
file): the same, of windows-x64-setup and the setup built, and the setup at the address it names is fetched and
must be the one built.
Every check that fails is a "FAIL:" line; the script fails if any did. Nothing is changed or uploaded.
#>
param(
    [Parameter(Mandatory)] [string] $Run,
    [string[]] $From = @(),
    [string] $Feed = '',
    [string] $SetupFeed = '',
    [switch] $RequireSignature
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
. "$PSScriptRoot\build_common.ps1"

$failures = [Collections.Generic.List[string]]::new()
function Check([bool] $Condition, [string] $Message) {
    if ($Condition) { Write-Output "ok: $Message" } else { Write-Output "FAIL: $Message"; $failures.Add($Message) }
}
function Test-Signature([byte[]] $Data, [byte[]] $Signature, [string] $PublicKey) {
    # ECDSA P-256 over SHA-256 of the exact bytes, 64 bytes r || s, by the 128-digit key the program pins.
    if ($Signature.Length -ne 64 -or -not (Test-ReleasePublicKey $PublicKey)) { return $false }
    $parameters = [Security.Cryptography.ECParameters]::new()
    $parameters.Curve = [Security.Cryptography.ECCurve+NamedCurves]::nistP256
    $point = [Security.Cryptography.ECPoint]::new()
    $point.X = [Convert]::FromHexString($PublicKey.Substring(0, 64))
    $point.Y = [Convert]::FromHexString($PublicKey.Substring(64))
    $parameters.Q = $point
    $key = [Security.Cryptography.ECDsa]::Create($parameters)
    try {
        return $key.VerifyData($Data, $Signature, [Security.Cryptography.HashAlgorithmName]::SHA256,
            [Security.Cryptography.DSASignatureFormat]::IeeeP1363FixedFieldConcatenation)
    } finally { $key.Dispose() }
}
function Get-Published([string] $Address, [string] $Into) {
    <# Brings one published file to $Into; $null where it is not there. A redirect is followed only from GitHub. #>
    $uri = [uri] $Address
    $loopback = $uri.Scheme -ceq 'http' -and $uri.Host -ceq '127.0.0.1'
    if (-not $uri.IsAbsoluteUri -or ($uri.Scheme -cne 'https' -and -not $loopback) -or $uri.UserInfo -or $uri.Query) {
        throw "Not a plain https address: $Address"
    }
    $handler = [Net.Http.HttpClientHandler]::new()
    $handler.AllowAutoRedirect = $uri.Host -ceq 'github.com'
    $client = [Net.Http.HttpClient]::new($handler)
    $client.Timeout = [TimeSpan]::FromMinutes(10)
    try {
        $answer = $client.GetAsync($uri, [Net.Http.HttpCompletionOption]::ResponseHeadersRead).GetAwaiter().GetResult()
        $status = [int] $answer.StatusCode
        if ($status -eq 404) { return $null }
        if ($status -ge 300 -and $status -lt 400) { throw "$Address answered $status, a redirect, which the program does not follow." }
        if ($status -ne 200) { throw "$Address answered $status." }
        $from = $answer.Content.ReadAsStreamAsync().GetAwaiter().GetResult()
        $to = [IO.File]::Create($Into)
        try { $from.CopyTo($to) } finally { $to.Dispose(); $from.Dispose() }
        return $Into
    } finally {
        $client.Dispose()
    }
}

# (Every place, from one -From or several: pwsh -File gives "a,b" as one word, a call in PowerShell an array.)
$places = @($From | ForEach-Object { "$_".Split(',') } | ForEach-Object { $_.Trim() })
if (@($places | Where-Object { -not $_ }).Count) {
    throw 'A -From names an empty place (two commas, or one at an end): nothing was checked.'
}
if (-not $places.Count) {
    throw 'There is no place to check: name one or more with -From, a comma between them. Nothing was checked.'
}
# The hosts the places are on: the one the feed names its ZIP on must be among them (nothing is asked of another).
$addresses = @($places | Where-Object { $_ -match '^[a-z]+://' } | ForEach-Object { ([uri] $_).GetLeftPart([UriPartial]::Authority) })

$Run = (Resolve-Path -LiteralPath $Run).Path
$record = Get-Content -LiteralPath (Join-Path $Run 'build-record.json') -Raw | ConvertFrom-Json
$written = Get-Content -LiteralPath (Join-Path $Run 'release-files.json') -Raw | ConvertFrom-Json
$version = [string] $record.version
$publicKey = [string] $record.updates.publicKey
Check (-not $record.trial) 'the run is no trial'
Check ($written.version -ceq $version -and $written.commit -ceq $record.source.commit) 'the release files are of this run'
$names = Get-ReleaseFileNames $version
$expected = @{}
foreach ($file in $written.files) { $expected[$file.name] = $file.sha256 }
Check ((@($expected.Keys) | Sort-Object -CaseSensitive) -join '|' -ceq (($names | Sort-Object -CaseSensitive) -join '|')) 'release-files.json names the release files'
$zipName = "qKVM-Desktop-$version-windows-x64.zip"
Check ($expected[$zipName] -ceq $record.package.sha256) 'the ZIP written is the ZIP built'
$setupName = Get-SetupName $version
$builtSetup = if ($record.PSObject.Properties['setup'] -and $record.setup) { [string] $record.setup.sha256 } else { '' }
Check ($builtSetup -and $expected[$setupName] -ceq $builtSetup) 'the setup written is the setup built'

$work = Join-Path ([IO.Path]::GetTempPath()) "qkvm-verify-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $work | Out-Null
try {
    $number = 0
    foreach ($place in $places) {
        ++$number
        Write-Output "--- $place"
        $isAddress = $place -match '^[a-z]+://'
        if ($isAddress) {
            $folder = (New-Item -ItemType Directory -Path (Join-Path $work "from-$number")).FullName
            $fetch = { param($name) Get-Published "$($place.TrimEnd('/'))/$name" (Join-Path $folder $name) }
        } else {
            $folder = (Resolve-Path -LiteralPath $place).Path
            $fetch = { param($name) $file = Join-Path $folder $name; if (Test-Path -LiteralPath $file -PathType Leaf) { $file } else { $null } }
            try { Assert-ReleaseFolder $folder $version -Signed; Check $true 'the folder holds the release files and nothing else' }
            catch { Check $false "the folder holds the release files and nothing else ($($_.Exception.Message))" }
        }
        try {
            $sums = & $fetch 'SHA256SUMS'
            Check ($null -ne $sums -and (Get-Sha256 $sums) -ceq $expected['SHA256SUMS']) 'SHA256SUMS is the one written'
            if ($null -eq $sums) { continue }
            $listed = @{}
            foreach ($line in [IO.File]::ReadAllText($sums).Split("`n") | Where-Object { $_ }) {
                if ($line -cmatch '^([0-9a-f]{64})  (\S+)$') { $listed[$Matches[2]] = $Matches[1] } else { $listed['?'] = $line }
            }
            Check ((@($listed.Keys) | Sort-Object -CaseSensitive) -join '|' -ceq (($names | Where-Object { $_ -cne 'SHA256SUMS' } | Sort-Object -CaseSensitive) -join '|')) 'SHA256SUMS names exactly the release files'
            foreach ($name in $names | Where-Object { $_ -cne 'SHA256SUMS' }) {
                $file = & $fetch $name
                Check ($null -ne $file -and (Get-Sha256 $file) -ceq $expected[$name] -and $listed[$name] -ceq $expected[$name]) "$name is the one written and listed"
            }
            $signature = & $fetch 'SHA256SUMS.sig'
            if ($null -eq $signature) {
                if ($RequireSignature) { Check $false 'SHA256SUMS.sig is there' }
                else { Write-Output 'note: SHA256SUMS.sig is not there yet (-RequireSignature asks for it)' }
            } else {
                Check (Test-Signature ([IO.File]::ReadAllBytes($sums)) ([IO.File]::ReadAllBytes($signature)) $publicKey) 'SHA256SUMS.sig is signed by the key the program pins'
            }
        } catch {
            Check $false "$place could be read ($($_.Exception.Message))"
        }
    }

    function Test-PublishedFeed([string] $Address, [string] $Label, [string] $Kind, [string] $Platform, [string] $Name,
        [string] $Thing, [long] $Bytes, [string] $Sha256, [string] $As) {
        <#
        One feed, as the program reads it: at $Address (never by a redirect) or in a file; $Label says it in every check.
        Signed by the key the program pins, of $Platform, this version, naming $Name ($Thing) by $Bytes and $Sha256 at an
        address on a host that is checked, current; and what it names is fetched there, as the program fetches it.
        #>
        Write-Output "--- $Address"
        try {
            $feedFile = if ($Address -match '^[a-z]+://') { Get-Published $Address (Join-Path $work $As) } else { (Resolve-Path -LiteralPath $Address).Path }
            if ($null -eq $feedFile) { throw 'it is not there' }
            $envelope = Get-Content -LiteralPath $feedFile -Raw | ConvertFrom-Json
            $payloadBytes = [Convert]::FromBase64String([string] $envelope.payload)
            $signatureBytes = [Convert]::FromBase64String([string] $envelope.signature)
            Check (Test-Signature $payloadBytes $signatureBytes $publicKey) "$Label is signed by the key the program pins"
            # (Read as JSON is, every value as written: PowerShell's own reader turns dates into its own.)
            $document = [Text.Json.JsonDocument]::Parse([Text.UTF8Encoding]::new($false, $true).GetString($payloadBytes))
            $payload = @{}
            foreach ($property in $document.RootElement.EnumerateObject()) {
                $payload[$property.Name] = if ($property.Value.ValueKind -eq 'Number') { $property.Value.GetInt64() } else { "$($property.Value)" }
            }
            $document.Dispose()
            Check ($payload['schema'] -eq 1 -and $payload['product'] -ceq 'qkvm-desktop' -and $payload['channel'] -ceq 'stable' -and
                $payload['platform'] -ceq $Platform) "$Label is of qKVM Desktop's stable $Kind"
            Check ($payload['version'] -ceq $version) "$Label offers $version"
            $package = [uri] [string] $payload['package']
            $named = $package.IsAbsoluteUri -and -not $package.Query -and -not $package.UserInfo -and -not $package.Fragment -and
                ($package.Scheme -ceq 'https' -or ($package.Scheme -ceq 'http' -and $package.Host -ceq '127.0.0.1')) -and
                $package.Segments[-1] -ceq $Name -and $addresses -ccontains $package.GetLeftPart([UriPartial]::Authority)
            Check $named "$Label names $Name at an https address, on a host that is checked"
            Check ($payload['bytes'] -eq $Bytes -and $payload['sha256'] -ceq $Sha256) "$Label names the $Thing built, by size and SHA-256"
            if ($named) {
                # What the feed names, at the address it names, as the program fetches it: no redirect, the bytes it says.
                try {
                    $fetched = Get-Published $package.AbsoluteUri (Join-Path $work "named-$As")
                    Check ($null -ne $fetched -and (Get-Item -LiteralPath $fetched).Length -eq $Bytes -and
                        (Get-Sha256 $fetched) -ceq $Sha256) "the $Thing $Label names is the one built, by size and SHA-256"
                } catch {
                    Check $false "the $Thing $Label names is the one built (it could not be fetched: $($_.Exception.Message))"
                }
            }
            $culture = [Globalization.CultureInfo]::InvariantCulture
            $styles = [Globalization.DateTimeStyles]::AdjustToUniversal -bor [Globalization.DateTimeStyles]::AssumeUniversal
            $published = [DateTime]::ParseExact($payload['published'], "yyyy-MM-dd'T'HH:mm:ss'Z'", $culture, $styles)
            $expires = [DateTime]::ParseExact($payload['expires'], "yyyy-MM-dd'T'HH:mm:ss'Z'", $culture, $styles)
            $now = [DateTime]::UtcNow
            Check ($published -le $now -and $now -lt $expires) "$Label is valid now (until $($payload['expires']))"
            Check (($expires - $published).TotalDays -le 31) "$Label is valid for no more than 31 days"
        } catch {
            Check $false "$Label could be read ($($_.Exception.Message))"
        }
    }
    if ($Feed) {
        Test-PublishedFeed $Feed 'the feed' 'portable release' 'windows-x64-portable' $zipName 'ZIP' $record.package.bytes $record.package.sha256 'stable.json'
    }
    if ($SetupFeed) {
        $setupBytes = if ($builtSetup) { [long] $record.setup.bytes } else { 0 }
        Test-PublishedFeed $SetupFeed "installed copies' feed" 'setup' 'windows-x64-setup' $setupName 'setup' $setupBytes $builtSetup 'stable-setup.json'
    }
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
if ($failures.Count) { throw "$($failures.Count) check(s) of what is published failed." }
$feeds = if ($Feed -and $SetupFeed) { ' and both feeds' } elseif ($Feed) { ' and the feed' } elseif ($SetupFeed) { ' and installed copies'' feed' } else { '' }
Write-Output "What is published is what was built: qKVM Desktop $version, $($places.Count) place(s)$feeds."
