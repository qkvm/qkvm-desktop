#Requires -Version 7.2
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/release_common.ps1"

# Test-only packages and ephemeral signing keys. No private key is read from or written to disk.
$folder = Join-Path $PSScriptRoot ('../build/update-release-tools/' + [Guid]::NewGuid().ToString('N'))
$folder = (New-Item -ItemType Directory -Path $folder).FullName
$checks = 0
function Assert-UpdateTest([bool] $Condition, [string] $Message) {
    if (-not $Condition) { throw "FAIL: $Message" }
    $script:checks++
}
# Refused, and for the reason given where one is (a refusal for another reason, a typing slip, is no proof).
function Expect-UpdateRefusal([scriptblock] $Operation, [string] $Because = '') {
    $refused = $false
    $said = ''
    try { & $Operation | Out-Null } catch { $refused = $true; $said = "$_" }
    Assert-UpdateTest $refused 'An invalid release operation was accepted.'
    if ($Because) { Assert-UpdateTest ($said -match $Because) "Refused, but not because '$Because': $said" }
}

# Each kind: the payload names its platform and its package, is signed by .NET and assembled; a public fixture of it
# is kept for the C++ verifier (the private key has already been disposed of: only the public coordinates, the signed
# feed and a fixed verification time).
function Test-Kind([string] $Kind, [string] $Ending, [string] $Platform, [string] $FeedName, [switch] $Portable,
    [switch] $Setup) {
    $package = Join-Path $folder "test$Ending"
    $payload = Join-Path $folder "payload-$Kind.json"
    $signature = Join-Path $folder "signature-$Kind.bin"
    $feed = Join-Path $folder "feed-$Kind.json"
    [IO.File]::WriteAllBytes($package, [Text.Encoding]::UTF8.GetBytes('A made-up test package; never installed.'))
    & "$PSScriptRoot/prepare_update_payload.ps1" -TestPackage -Portable:$Portable -Setup:$Setup -Package $package -Version '0.2.0' `
        -PackageUrl "https://updates.example.test/qkvm-0.2.0$Ending" -Notes 'Test release' -Output $payload | Out-Null
    $payloadBytes = [IO.File]::ReadAllBytes($payload)
    $parsed = [Text.Encoding]::UTF8.GetString($payloadBytes) | ConvertFrom-Json
    Assert-UpdateTest ($parsed.product -ceq 'qkvm-desktop' -and $parsed.platform -ceq $Platform) "Wrong product or platform for $Kind."
    Assert-UpdateTest ($parsed.package -ceq "https://updates.example.test/qkvm-0.2.0$Ending") "Wrong package address for $Kind."
    Assert-UpdateTest ($parsed.bytes -eq (Get-Item -LiteralPath $package).Length) 'Wrong package size.'
    Assert-UpdateTest ($parsed.sha256 -ceq (Get-FileHash -LiteralPath $package).Hash.ToLowerInvariant()) 'Wrong package hash.'
    Assert-UpdateTest ($payloadBytes[0] -eq 123) 'The payload must have no byte-order mark.'

    $testKey = [Security.Cryptography.ECDsa]::Create([Security.Cryptography.ECCurve+NamedCurves]::nistP256)
    try {
        $public = $testKey.ExportParameters($false)
        $publicHex = [Convert]::ToHexString([byte[]]($public.Q.X + $public.Q.Y))
        $signed = $testKey.SignData($payloadBytes, [Security.Cryptography.HashAlgorithmName]::SHA256,
            [Security.Cryptography.DSASignatureFormat]::IeeeP1363FixedFieldConcatenation)
    } finally { $testKey.Dispose() }
    [IO.File]::WriteAllBytes($signature, $signed)
    try {
        $said = @(& "$PSScriptRoot/assemble_update_feed.ps1" -Payload $payload -Signature $signature -PublicKey $publicHex -Output $feed)
    } catch { throw "FAIL: the $Kind payload was not assembled: $_" }
    # Where each kind's feed is served: a Setup copy's beside the one beta 1's portable copies read.
    Assert-UpdateTest (@($said | Where-Object { $_ -ceq "Serve it as $FeedName ($Platform, qKVM Desktop 0.2.0)." }).Count -eq 1) "The $Kind feed is not said to be served as ${FeedName}: $said"
    $envelope = Get-Content -LiteralPath $feed -Raw | ConvertFrom-Json
    Assert-UpdateTest ($envelope.payload -ceq [Convert]::ToBase64String($payloadBytes)) 'Payload bytes changed during assembly.'
    Assert-UpdateTest ($envelope.signature -ceq [Convert]::ToBase64String($signed)) 'Signature bytes changed during assembly.'
    $publicFixture = [ordered]@{
        source = "prepare_update_payload.ps1$(if ($Portable) { ' -Portable' })$(if ($Setup) { ' -Setup' }) and assemble_update_feed.ps1; ephemeral .NET P-256 key"
        checked_at = [Text.Encoding]::UTF8.GetString($payloadBytes) | Select-String -Pattern '"published":"([^"]+)"' |
            ForEach-Object { $_.Matches[0].Groups[1].Value }
        public_key = $publicHex
        feed = [Convert]::ToBase64String([IO.File]::ReadAllBytes($feed))
    }
    [IO.File]::WriteAllText((Join-Path $folder "desktop-fixture-$Kind.json"), ($publicFixture | ConvertTo-Json),
        [Text.UTF8Encoding]::new($false))
    return @{ Payload = $payload; Signature = $signature; Feed = $feed; Public = $publicHex; Signed = $signed; Package = $package }
}

$msix = Test-Kind -Kind 'msix' -Ending '.msix' -Platform 'windows-x64-msix' -FeedName 'stable.json'
$portable = Test-Kind -Kind 'portable' -Ending '.zip' -Platform 'windows-x64-portable' -FeedName 'stable.json' -Portable
$setup = Test-Kind -Kind 'setup' -Ending '-setup.exe' -Platform 'windows-x64-setup' -FeedName 'stable-setup.json' -Setup

$original = [IO.File]::ReadAllText($msix.Feed)
Expect-UpdateRefusal { & "$PSScriptRoot/assemble_update_feed.ps1" -Payload $msix.Payload -Signature $msix.Signature -PublicKey $msix.Public -Output $msix.Feed }
Assert-UpdateTest ([IO.File]::ReadAllText($msix.Feed) -ceq $original) 'An existing feed was overwritten.'
$badSignature = Join-Path $folder 'bad-signature.bin'
$signed = $msix.Signed
$signed[0] = $signed[0] -bxor 1
[IO.File]::WriteAllBytes($badSignature, $signed)
$refusedFeed = Join-Path $folder 'refused-feed.json'
Expect-UpdateRefusal { & "$PSScriptRoot/assemble_update_feed.ps1" -Payload $msix.Payload -Signature $badSignature -PublicKey $msix.Public -Output $refusedFeed }
Assert-UpdateTest (-not (Test-Path -LiteralPath $refusedFeed)) 'A failed signature left an output feed.'
$package = $msix.Package
Expect-UpdateRefusal { & "$PSScriptRoot/prepare_update_payload.ps1" -TestPackage -Package $package -Version "0.2.0`n" -PackageUrl 'https://updates.example.test/a.msix' -Notes 'Test' -Output $refusedFeed }
Expect-UpdateRefusal { & "$PSScriptRoot/prepare_update_payload.ps1" -TestPackage -Package $package -Version '65536.0.0' -PackageUrl 'https://updates.example.test/a.msix' -Notes 'Test' -Output $refusedFeed }
Expect-UpdateRefusal { & "$PSScriptRoot/prepare_update_payload.ps1" -TestPackage -Package $package -Version '0.2.0' -PackageUrl 'http://updates.example.test/a.msix' -Notes 'Test' -Output $refusedFeed }
Expect-UpdateRefusal { & "$PSScriptRoot/prepare_update_payload.ps1" -Package $package -Version '0.2.0' -PackageUrl 'https://updates.example.test/a.msix' -Notes 'Test' -Output $refusedFeed }
Expect-UpdateRefusal { & "$PSScriptRoot/prepare_update_payload.ps1" -TestPackage -Package $package -Version '0.2.0' -PackageUrl 'https://updates.example.com/a.msix' -Notes 'Test' -Output $refusedFeed }

# Each kind of release takes only its own kind of package, by its address and by its file.
$zip = $portable.Package
$becauseAddress = 'Use a direct HTTPS'
$becauseFile = 'The package must be a nonempty'
Expect-UpdateRefusal { & "$PSScriptRoot/prepare_update_payload.ps1" -TestPackage -Portable -Package $zip -Version '0.2.0' -PackageUrl 'https://updates.example.test/a.msix' -Notes 'Test' -Output $refusedFeed } $becauseAddress
Expect-UpdateRefusal { & "$PSScriptRoot/prepare_update_payload.ps1" -TestPackage -Portable -Package $zip -Version '0.2.0' -PackageUrl 'https://updates.example.test/a.zip.exe' -Notes 'Test' -Output $refusedFeed } $becauseAddress
Expect-UpdateRefusal { & "$PSScriptRoot/prepare_update_payload.ps1" -TestPackage -Portable -Package $package -Version '0.2.0' -PackageUrl 'https://updates.example.test/a.zip' -Notes 'Test' -Output $refusedFeed } $becauseFile
Expect-UpdateRefusal { & "$PSScriptRoot/prepare_update_payload.ps1" -TestPackage -Package $package -Version '0.2.0' -PackageUrl 'https://updates.example.test/a.zip' -Notes 'Test' -Output $refusedFeed } $becauseAddress
Expect-UpdateRefusal { & "$PSScriptRoot/prepare_update_payload.ps1" -TestPackage -Package $zip -Version '0.2.0' -PackageUrl 'https://updates.example.test/a.msix' -Notes 'Test' -Output $refusedFeed } $becauseFile
Expect-UpdateRefusal { & "$PSScriptRoot/prepare_update_payload.ps1" -TestPackage -Portable -Package $zip -Version '0.2.0' -PackageUrl 'https://updates.example.com/a.zip' -Notes 'Test' -Output $refusedFeed } 'example.test'
# A Setup release: qKVM's setup, by its address and by its file; one kind to a payload.
$setupFile = $setup.Package
Expect-UpdateRefusal { & "$PSScriptRoot/prepare_update_payload.ps1" -TestPackage -Setup -Package $setupFile -Version '0.2.0' -PackageUrl 'https://updates.example.test/a.exe' -Notes 'Test' -Output $refusedFeed } $becauseAddress
Expect-UpdateRefusal { & "$PSScriptRoot/prepare_update_payload.ps1" -TestPackage -Setup -Package $setupFile -Version '0.2.0' -PackageUrl 'https://updates.example.test/a-setup.exe.zip' -Notes 'Test' -Output $refusedFeed } $becauseAddress
Expect-UpdateRefusal { & "$PSScriptRoot/prepare_update_payload.ps1" -TestPackage -Setup -Package $setupFile -Version '0.2.0' -PackageUrl 'https://updates.example.test/a.zip' -Notes 'Test' -Output $refusedFeed } $becauseAddress
Expect-UpdateRefusal { & "$PSScriptRoot/prepare_update_payload.ps1" -TestPackage -Setup -Package $zip -Version '0.2.0' -PackageUrl 'https://updates.example.test/a-setup.exe' -Notes 'Test' -Output $refusedFeed } $becauseFile
Expect-UpdateRefusal { & "$PSScriptRoot/prepare_update_payload.ps1" -TestPackage -Portable -Package $setupFile -Version '0.2.0' -PackageUrl 'https://updates.example.test/a.zip' -Notes 'Test' -Output $refusedFeed } $becauseFile
Expect-UpdateRefusal { & "$PSScriptRoot/prepare_update_payload.ps1" -TestPackage -Package $setupFile -Version '0.2.0' -PackageUrl 'https://updates.example.test/a-setup.exe' -Notes 'Test' -Output $refusedFeed } $becauseAddress
Expect-UpdateRefusal { & "$PSScriptRoot/prepare_update_payload.ps1" -TestPackage -Setup -Portable -Package $setupFile -Version '0.2.0' -PackageUrl 'https://updates.example.test/a-setup.exe' -Notes 'Test' -Output $refusedFeed } 'not both'
$otherExe = Join-Path $folder 'other.exe'
[IO.File]::WriteAllBytes($otherExe, [Text.Encoding]::UTF8.GetBytes('A made-up program; never run.'))
Expect-UpdateRefusal { & "$PSScriptRoot/prepare_update_payload.ps1" -TestPackage -Setup -Package $otherExe -Version '0.2.0' -PackageUrl 'https://updates.example.test/a-setup.exe' -Notes 'Test' -Output $refusedFeed } $becauseFile
Assert-UpdateTest (-not (Test-Path -LiteralPath $refusedFeed)) 'A refused payload left an output file.'

# A real setup says the advertised version in its file version information, read as data and never run. (Windows'
# own where.exe stands in: its version is never a release's.)
$standSetup = Join-Path $folder 'qKVM-Desktop-0.2.0-windows-x64-setup.exe'
Copy-Item -LiteralPath (Join-Path $env:SystemRoot 'System32\where.exe') -Destination $standSetup
Assert-UpdateTest ((Get-SetupVersion -Setup $standSetup) -ceq (Get-Item -LiteralPath $standSetup).VersionInfo.ProductVersion.Trim()) 'The version of a setup was not read from its file version information.'
Expect-UpdateRefusal { & "$PSScriptRoot/prepare_update_payload.ps1" -Setup -Package $standSetup -Version '0.2.0' -PackageUrl 'https://updates.example.test/a-setup.exe' -Notes 'Test' -Output $refusedFeed } 'says version'
$noVersion = Join-Path $folder 'none-setup.exe'
[IO.File]::WriteAllBytes($noVersion, [Text.Encoding]::UTF8.GetBytes('No program at all; never run.'))
Expect-UpdateRefusal { & "$PSScriptRoot/prepare_update_payload.ps1" -Setup -Package $noVersion -Version '0.2.0' -PackageUrl 'https://updates.example.test/a-setup.exe' -Notes 'Test' -Output $refusedFeed } 'says no version'
Assert-UpdateTest (-not (Test-Path -LiteralPath $refusedFeed)) 'A refused setup payload left an output file.'

# Only a current release payload is assembled into a feed: a payload of another platform, expired, or no payload at all
# is refused, whatever signs it.
function Expect-AssemblyRefused([string] $Name, [string] $Text, [string] $Because) {
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($Text)
    $file = Join-Path $folder "assemble-$Name.json"
    [IO.File]::WriteAllBytes($file, $bytes)
    $key = [Security.Cryptography.ECDsa]::Create([Security.Cryptography.ECCurve+NamedCurves]::nistP256)
    try {
        $public = $key.ExportParameters($false)
        $hex = [Convert]::ToHexString([byte[]]($public.Q.X + $public.Q.Y))
        $signatureFile = Join-Path $folder "assemble-$Name.sig"
        [IO.File]::WriteAllBytes($signatureFile, $key.SignData($bytes, [Security.Cryptography.HashAlgorithmName]::SHA256,
            [Security.Cryptography.DSASignatureFormat]::IeeeP1363FixedFieldConcatenation))
    } finally { $key.Dispose() }
    $out = Join-Path $folder "assembled-$Name.json"
    Expect-UpdateRefusal { & "$PSScriptRoot/assemble_update_feed.ps1" -Payload $file -Signature $signatureFile -PublicKey $hex -Output $out } $Because
    Assert-UpdateTest (-not (Test-Path -LiteralPath $out)) "A refused $Name payload was assembled."
}
$goodSetup = [IO.File]::ReadAllText($setup.Payload)
Expect-AssemblyRefused 'setup-in-capitals' ($goodSetup -replace 'windows-x64-setup', 'WINDOWS-X64-SETUP') 'platform of'
Expect-AssemblyRefused 'setup-as-zip' ($goodSetup -replace 'qkvm-0.2.0-setup.exe', 'qkvm-0.2.0.zip') 'direct HTTPS address of a -setup.exe'
Expect-AssemblyRefused 'setup-another-exe' ($goodSetup -replace 'qkvm-0.2.0-setup.exe', 'qkvm-0.2.0.exe') 'direct HTTPS address of a -setup.exe'
Expect-AssemblyRefused 'setup-expired' ($goodSetup -replace '"expires":"[^"]+"', '"expires":"2000-01-02T00:00:00Z"' -replace '"published":"[^"]+"', '"published":"2000-01-01T00:00:00Z"') 'not current'
Expect-AssemblyRefused 'no-payload' "$('0' * 64)  qKVM.zip`n" 'no JSON object'

# A real portable ZIP: its one qkvm-desktop.exe, at its top or in its one folder, says the version that is advertised.
# Its version is read from a copy of it beside the output, which is never run and is removed. (Windows' own where.exe
# stands in for the program here: its version is never a release's.)
$stand = Join-Path $env:SystemRoot 'System32\where.exe'
$standVersion = (Get-Item -LiteralPath $stand).VersionInfo.ProductVersion
function New-TestZip([string] $Name, [string[]] $Inside) {
    $tree = Join-Path $folder "tree-$Name"
    foreach ($place in $Inside) {
        $at = Join-Path $tree $place
        New-Item -ItemType Directory -Path (Split-Path -Parent $at) -Force | Out-Null
        Copy-Item -LiteralPath $stand -Destination $at
    }
    $archive = Join-Path $folder "$Name.zip"
    [IO.Compression.ZipFile]::CreateFromDirectory($tree, $archive)
    return $archive
}
$inFolder = New-TestZip 'in-folder' @('qKVM-Desktop-0.2.0-windows-x64/qkvm-desktop.exe')
$atTop = New-TestZip 'at-top' @('qkvm-desktop.exe')
$twice = New-TestZip 'twice' @('qkvm-desktop.exe', 'qKVM/qkvm-desktop.exe')
$deeper = New-TestZip 'deeper' @('qKVM/bin/qkvm-desktop.exe')
$none = New-TestZip 'none' @('qKVM/other.exe')
Assert-UpdateTest ((Get-PortableProgramVersion -Zip $inFolder -Beside $folder) -ceq $standVersion) 'The version in a folder of the ZIP was not read.'
Assert-UpdateTest ((Get-PortableProgramVersion -Zip $atTop -Beside $folder) -ceq $standVersion) 'The version at the top of the ZIP was not read.'
Assert-UpdateTest (@(Get-ChildItem -LiteralPath $folder -Filter '.version-*' -Force).Count -eq 0) 'The copy read for its version was left behind.'
foreach ($wrong in @($twice, $deeper, $none)) {
    Expect-UpdateRefusal { Get-PortableProgramVersion -Zip $wrong -Beside $folder } 'exactly one qkvm-desktop.exe'
}
$notZip = Join-Path $folder 'not-a.zip'
[IO.File]::WriteAllBytes($notZip, [Text.Encoding]::UTF8.GetBytes('not a zip at all'))
Expect-UpdateRefusal { Get-PortableProgramVersion -Zip $notZip -Beside $folder } 'not a ZIP'
Expect-UpdateRefusal { & "$PSScriptRoot/prepare_update_payload.ps1" -Portable -Package $inFolder -Version '0.2.0' -PackageUrl 'https://updates.example.test/a.zip' -Notes 'Test' -Output $refusedFeed } 'is version'
Expect-UpdateRefusal { & "$PSScriptRoot/prepare_update_payload.ps1" -Portable -Package $none -Version '0.2.0' -PackageUrl 'https://updates.example.test/a.zip' -Notes 'Test' -Output $refusedFeed } 'exactly one qkvm-desktop.exe'
Assert-UpdateTest (-not (Test-Path -LiteralPath $refusedFeed)) 'A refused portable payload left an output file.'
Write-Output "$checks release-tool checks passed. Public test artifacts: $folder"
