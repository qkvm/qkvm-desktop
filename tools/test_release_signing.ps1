#Requires -Version 7.6
# The release key tools, with throwaway keys only: each is made in a new folder of the system's temporary folder
# (inside no repository), used, and removed with that folder at the end. No real release key is made or touched here.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/release_common.ps1"

$checks = 0
function Assert-SigningTest([bool] $Condition, [string] $Message) {
    if (-not $Condition) { throw "FAIL: $Message" }
    $script:checks++
}
# (Its own variables have names that no operation uses: an operation's variables are looked up through it.)
function Expect-SigningRefusal([scriptblock] $Operation, [string] $Because) {
    $wasRefusedHere = $false
    $saidHere = ''
    try { & $Operation *>&1 | Out-Null } catch { $wasRefusedHere = $true; $saidHere = "$_" }
    Assert-SigningTest $wasRefusedHere "Accepted, where it should have been refused because '$Because'."
    Assert-SigningTest ($saidHere -match $Because) "Refused, but not because '$Because': $saidHere"
}
# A passphrase as a test makes it: from its letters, for throwaway keys only.
function New-TestPassphrase([string] $Letters) {
    $made = [Security.SecureString]::new()
    foreach ($letter in $Letters.ToCharArray()) { $made.AppendChar($letter) }
    $made.MakeReadOnly()
    return $made
}

$secret = 'throwaway test passphrase, never a real one'
$keys = Join-Path ([IO.Path]::GetTempPath()) ('qkvm-release-test-' + [Guid]::NewGuid().ToString('N'))
$public = Join-Path $PSScriptRoot ('../build/release-signing-tools/' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $keys | Out-Null
$public = (New-Item -ItemType Directory -Path $public).FullName
try {
    Assert-SigningTest (-not (Test-InsideRepository $keys)) 'The temporary folder for throwaway keys is inside a repository.'

    # 1. The three tools ask a person at the console for the passphrase, and refuse to run where their input is
    # not one (here: a pipe). They take no passphrase by a parameter, and read no environment.
    foreach ($tool in @('release_key.ps1', 'sign_release.ps1', 'refresh_feed.ps1')) {
        $path = Join-Path $PSScriptRoot $tool
        $named = @((Get-Command $path).Parameters.Keys | Where-Object { $_ -match 'pass|phrase|secret|word|pin' })
        Assert-SigningTest ($named.Count -eq 0) "$tool takes a parameter that could carry the passphrase: $named"
        $text = [IO.File]::ReadAllText($path)
        Assert-SigningTest ($text -notmatch 'env:|GetEnvironmentVariable|ConvertTo-SecureString|AsPlainText') "$tool reads the environment or a plain passphrase."
        Assert-SigningTest ($text -match 'Assert-PersonAtConsole' -and $text -match 'Read-ReleasePassphrase') "$tool does not ask a person for the passphrase."
    }
    $common = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'release_common.ps1'))
    Assert-SigningTest ($common -notmatch 'env:|GetEnvironmentVariable|ConvertTo-SecureString|AsPlainText|PtrToString|Write-Host \$letters') 'release_common.ps1 reads the environment or makes the passphrase a string.'
    $start = [Diagnostics.ProcessStartInfo]::new((Get-Process -Id $PID).Path)
    foreach ($argument in @('-NoProfile', '-File', (Join-Path $PSScriptRoot 'release_key.ps1'), '-Directory', $keys)) {
        $start.ArgumentList.Add($argument)
    }
    $start.RedirectStandardInput = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.UseShellExecute = $false
    $piped = [Diagnostics.Process]::Start($start)
    $output = $piped.StandardOutput.ReadToEndAsync()
    $errors = $piped.StandardError.ReadToEndAsync()
    $piped.StandardInput.WriteLine($secret)
    $piped.StandardInput.WriteLine($secret)
    $piped.StandardInput.Close()
    $ended = $piped.WaitForExit(60000)
    if (-not $ended) { $piped.Kill($true); $piped.WaitForExit() }
    $said = $output.GetAwaiter().GetResult() + $errors.GetAwaiter().GetResult()
    Assert-SigningTest $ended 'release_key.ps1 waited for a passphrase where its input was a pipe, and did not refuse.'
    Assert-SigningTest ($piped.ExitCode -ne 0 -and $said -match 'console window') "release_key.ps1 took a passphrase from a pipe: $said"
    Assert-SigningTest (-not (Test-Path -LiteralPath (Join-Path $keys $ReleaseKeyName))) 'release_key.ps1 wrote a key from a piped passphrase.'
    Assert-SigningTest (-not $said.Contains($secret)) 'The piped passphrase was said back.'

    # 2. The passphrase as asked: twice for a new key, long, the same both times; never shown.
    $answers = [Collections.Generic.Queue[string]]::new()
    function Read-Host([switch] $AsSecureString, [string] $Prompt) {
        if (-not $AsSecureString) { throw 'FAIL: the passphrase was asked for where it is shown as it is typed.' }
        return New-TestPassphrase $answers.Dequeue()
    }
    $answers.Enqueue('short')
    Expect-SigningRefusal { Read-ReleasePassphrase -New } 'at least 16'
    $answers.Enqueue($secret); $answers.Enqueue($secret + '!')
    Expect-SigningRefusal { Read-ReleasePassphrase -New } 'differ'
    $answers.Enqueue($secret); $answers.Enqueue($secret.Substring(0, $secret.Length - 1) + '?')
    Expect-SigningRefusal { Read-ReleasePassphrase -New } 'differ'
    $answers.Enqueue($secret); $answers.Enqueue($secret)
    $asked = Read-ReleasePassphrase -New
    Assert-SigningTest ($asked -is [Security.SecureString] -and $asked.Length -eq $secret.Length) 'The passphrase asked twice was not handed on.'
    $answers.Enqueue($secret)
    $once = Read-ReleasePassphrase
    Assert-SigningTest ($once.Length -eq $secret.Length) 'The passphrase asked once was not handed on.'
    Remove-Item Function:\Read-Host

    # 3. A throwaway key: encrypted PKCS#8 (PBES2, PBKDF2-HMAC-SHA256 with 600000 rounds, AES-256-CBC), with the
    # passphrase nowhere in it or in what was said; its public key, as the program pins it.
    $passphrase = New-TestPassphrase $secret
    $wrong = New-TestPassphrase ($secret + '?')
    $saidAtMaking = @(New-ReleaseKey -Directory $keys -Passphrase $passphrase *>&1)
    $made = $saidAtMaking | Where-Object { $_.PSObject.Properties.Name -contains 'PublicKey' } | Select-Object -First 1
    $keyFile = Join-Path $keys $ReleaseKeyName
    Assert-SigningTest ($made.File -ceq $keyFile -and (Test-Path -LiteralPath $keyFile)) 'The key file is not where it was asked for, by its name.'
    Assert-SigningTest ($made.PublicKey -cmatch '\A[0-9a-f]{128}\z') "The public key is not 128 lowercase hexadecimal characters: $($made.PublicKey)"
    $point = [Convert]::FromHexString($made.PublicKey)
    Assert-SigningTest ($made.Fingerprint -ceq [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($point)).ToLowerInvariant()) 'The fingerprint is not the SHA-256 of the public key.'
    Assert-SigningTest (-not (($saidAtMaking | Out-String).Contains($secret))) 'The passphrase was said while the key was made.'
    $keyText = [IO.File]::ReadAllText($keyFile)
    Assert-SigningTest ($keyText.StartsWith('-----BEGIN ENCRYPTED ' + 'PRIVATE KEY-----')) 'The key file is not an encrypted PKCS#8 key.'
    Assert-SigningTest (-not $keyText.Contains($secret)) 'The passphrase is in the key file.'
    $der = [Convert]::FromBase64String(($keyText -replace '-----[^-]+-----', '' -replace '\s', ''))
    $reader = [Formats.Asn1.AsnReader]::new($der, [Formats.Asn1.AsnEncodingRules]::DER)
    $algorithm = $reader.ReadSequence().ReadSequence()
    Assert-SigningTest ($algorithm.ReadObjectIdentifier() -ceq '1.2.840.113549.1.5.13') 'The key file is not encrypted by PBES2.'
    $pbes2 = $algorithm.ReadSequence()
    $derivation = $pbes2.ReadSequence()
    Assert-SigningTest ($derivation.ReadObjectIdentifier() -ceq '1.2.840.113549.1.5.12') 'The key is not derived by PBKDF2.'
    $derivationParameters = $derivation.ReadSequence()
    $null = $derivationParameters.ReadOctetString()
    Assert-SigningTest ($derivationParameters.ReadInteger() -ge 600000) 'The key file is derived with fewer than 600000 rounds.'
    Assert-SigningTest ($derivationParameters.ReadSequence().ReadObjectIdentifier() -ceq '1.2.840.113549.2.9') 'The key is not derived with HMAC-SHA256.'
    Assert-SigningTest ($pbes2.ReadSequence().ReadObjectIdentifier() -ceq '2.16.840.1.101.3.4.1.42') 'The key file is not encrypted with AES-256-CBC.'
    $opened = [Security.Cryptography.ECDsa]::Create()
    $opened.ImportFromEncryptedPem($keyText, $secret)
    $openedPublic = $opened.ExportParameters($false)
    Assert-SigningTest ($openedPublic.Curve.Oid.Value -ceq '1.2.840.10045.3.1.7') 'The key is not P-256.'
    Assert-SigningTest ([Convert]::ToHexString([byte[]] ($openedPublic.Q.X + $openedPublic.Q.Y)).ToLowerInvariant() -ceq $made.PublicKey) 'The public key said is not the key file''s.'
    $opened.Dispose()

    # Never twice into one place, never into a repository, never with a short passphrase.
    $before = [IO.File]::ReadAllBytes($keyFile)
    Expect-SigningRefusal { New-ReleaseKey -Directory $keys -Passphrase $passphrase } 'there already'
    Assert-SigningTest ([Linq.Enumerable]::SequenceEqual([byte[]] $before, [byte[]] [IO.File]::ReadAllBytes($keyFile))) 'The key file was changed by a second key.'
    $repository = Join-Path $keys 'a-repository'
    New-Item -ItemType Directory -Path (Join-Path $repository '.git') | Out-Null
    $inside = (New-Item -ItemType Directory -Path (Join-Path $repository 'tools')).FullName
    Expect-SigningRefusal { New-ReleaseKey -Directory $inside -Passphrase $passphrase } 'inside a git repository'
    Assert-SigningTest (-not (Test-Path -LiteralPath (Join-Path $inside $ReleaseKeyName))) 'A key was written inside a repository.'
    $worktree = Join-Path $keys 'a-worktree'
    New-Item -ItemType Directory -Path $worktree | Out-Null
    Set-Content -LiteralPath (Join-Path $worktree '.git') -Value 'gitdir: elsewhere'
    Expect-SigningRefusal { New-ReleaseKey -Directory $worktree -Passphrase $passphrase } 'inside a git repository'
    Expect-SigningRefusal { New-ReleaseKey -Directory (Join-Path $keys 'nowhere') -Passphrase $passphrase } 'There is no folder'
    $other = (New-Item -ItemType Directory -Path (Join-Path $keys 'other')).FullName
    Expect-SigningRefusal { New-ReleaseKey -Directory $other -Passphrase (New-TestPassphrase 'too short') } 'at least 16'
    Assert-SigningTest (-not (Test-Path -LiteralPath (Join-Path $other $ReleaseKeyName))) 'A key was written with a short passphrase.'
    $otherKey = New-ReleaseKey -Directory $other -Passphrase $passphrase

    # 4. A payload signed: 64 bytes, P1363, checked by assemble_update_feed.ps1 (its own .NET check) into a feed.
    $zip = Join-Path $public 'test.zip'
    [IO.File]::WriteAllBytes($zip, [Text.Encoding]::UTF8.GetBytes('A made-up test package; never installed.'))
    $payload = Join-Path $public 'payload.json'
    & "$PSScriptRoot/prepare_update_payload.ps1" -TestPackage -Portable -Package $zip -Version '0.2.0' -ValidDays 2 `
        -PackageUrl 'https://updates.example.test/qkvm-0.2.0.zip' -Notes 'Test release' -Output $payload | Out-Null
    $signature = Join-Path $public 'payload.json.sig'
    $saidAtSigning = Invoke-SignRelease -Key $keyFile -PublicKey $made.PublicKey -File $payload -Output $signature -Passphrase $passphrase *>&1
    Assert-SigningTest (-not (($saidAtSigning | Out-String).Contains($secret))) 'The passphrase was said while signing.'
    $signed = [IO.File]::ReadAllBytes($signature)
    Assert-SigningTest ($signed.Length -eq 64) "The signature is $($signed.Length) bytes, not 64."
    Assert-SigningTest (Test-ReleaseSignature -Data ([IO.File]::ReadAllBytes($payload)) -Signature $signed -PublicKey $made.PublicKey) 'The signature does not check against the public key.'
    Assert-SigningTest (-not (Test-ReleaseSignature -Data ([IO.File]::ReadAllBytes($payload)) -Signature $signed -PublicKey $otherKey.PublicKey)) 'The signature checks against another key.'
    $feed = Join-Path $public 'stable.json'
    & "$PSScriptRoot/assemble_update_feed.ps1" -Payload $payload -Signature $signature -PublicKey $made.PublicKey.ToUpperInvariant() -Output $feed | Out-Null
    Assert-SigningTest (Test-Path -LiteralPath $feed) 'The signed payload was not assembled into a feed.'

    # Nothing is signed by another key than the pinned one, with a wrong passphrase, over a file that is there, or of
    # what is no release payload and no SHA256SUMS file; and nothing is written where it is refused.
    $refused = Join-Path $public 'refused.sig'
    Expect-SigningRefusal { Invoke-SignRelease -Key $keyFile -PublicKey $otherKey.PublicKey -File $payload -Output $refused -Passphrase $passphrase } 'not the one the program pins'
    Expect-SigningRefusal { Invoke-SignRelease -Key $keyFile -PublicKey $made.PublicKey -File $payload -Output $refused -Passphrase $wrong } 'passphrase does not open'
    Expect-SigningRefusal { Invoke-SignRelease -Key $keyFile -PublicKey $made.PublicKey -File $payload -Output $signature -Passphrase $passphrase } 'there already'
    Assert-SigningTest ([Linq.Enumerable]::SequenceEqual([byte[]] $signed, [byte[]] [IO.File]::ReadAllBytes($signature))) 'A signature was written over.'
    $plainKey = Join-Path $keys 'plain.pem'
    $loose = [Security.Cryptography.ECDsa]::Create([Security.Cryptography.ECCurve+NamedCurves]::nistP256)
    [IO.File]::WriteAllText($plainKey, $loose.ExportPkcs8PrivateKeyPem())
    $loosePublic = $loose.ExportParameters($false)
    $loose.Dispose()
    Expect-SigningRefusal { Invoke-SignRelease -Key $plainKey -PublicKey ([Convert]::ToHexString([byte[]] ($loosePublic.Q.X + $loosePublic.Q.Y))) -File $payload -Output $refused -Passphrase $passphrase } 'encrypted PKCS#8'
    $otherCurve = Join-Path $keys 'p384.p8'
    $wide = [Security.Cryptography.ECDsa]::Create([Security.Cryptography.ECCurve+NamedCurves]::nistP384)
    $rounds = [Security.Cryptography.PbeParameters]::new([Security.Cryptography.PbeEncryptionAlgorithm]::Aes256Cbc, [Security.Cryptography.HashAlgorithmName]::SHA256, 1000)
    [IO.File]::WriteAllText($otherCurve, $wide.ExportEncryptedPkcs8PrivateKeyPem($secret, $rounds))
    $wide.Dispose()
    Expect-SigningRefusal { Invoke-SignRelease -Key $otherCurve -PublicKey $made.PublicKey -File $payload -Output $refused -Passphrase $passphrase } 'not a P-256 key'
    function Write-TestFile([string] $Name, [string] $Text) {
        $at = Join-Path $public $Name
        [IO.File]::WriteAllBytes($at, [Text.UTF8Encoding]::new($false).GetBytes($Text))
        return $at
    }
    $good = [IO.File]::ReadAllText($payload)
    $now = [DateTime]::UtcNow
    $withDates = { param($published, $expires) $good -replace '"published":"[^"]+"', "`"published`":`"$(Write-UtcDate $published)`"" -replace '"expires":"[^"]+"', "`"expires`":`"$(Write-UtcDate $expires)`"" }
    $wrongOnes = [ordered]@{
        'an expired payload' = @((& $withDates $now.AddDays(-10) $now.AddDays(-1)), 'not current')
        'a payload of the future' = @((& $withDates $now.AddDays(1) $now.AddDays(2)), 'not current')
        'a payload too long valid' = @((& $withDates $now $now.AddDays(32)), 'within 31 days')
        'another product' = @(($good -replace 'qkvm-desktop', 'qkvm-server'), 'product qkvm-desktop')
        'another platform' = @(($good -replace 'windows-x64-portable', 'windows-x64'), 'platform of')
        'a platform in capitals' = @(($good -replace 'windows-x64-portable', 'WINDOWS-X64-PORTABLE'), 'platform of')
        'an msix address for a zip' = @(($good -replace 'qkvm-0.2.0.zip', 'qkvm-0.2.0.msix'), 'direct HTTPS address of a .zip')
        'a field twice' = @(($good -replace '^\{', '{"version":"9.0.0",'), 'each once')
        'a field more' = @(($good -replace '^\{', '{"run":"setup.exe",'), 'each once')
        'a byte-order mark' = @(([char] 0xFEFF + $good), 'ASCII')
        'a size in text' = @(($good -replace '"bytes":(\d+)', '"bytes":"$1"'), 'whole number')
        'a size with a point' = @(($good -replace '"bytes":(\d+)', '"bytes":$1.0'), 'whole number')
        'a size with an exponent' = @(($good -replace '"bytes":(\d+)', '"bytes":4e1'), 'whole number')
        'a hash in capitals' = @(($good -replace '"sha256":"([0-9a-f]+)"', { '"sha256":"' + $_.Groups[1].Value.ToUpperInvariant() + '"' }), 'SHA-256')
        'a version with a suffix' = @(($good -replace '"version":"0.2.0"', '"version":"0.2.0-beta.1"'), 'three numbers')
        'a sums line with one space' = @("$('0' * 64) qKVM.zip`n", 'two spaces')
        'a sums line in capitals' = @("$('A' * 64)  qKVM.zip`n", 'two spaces')
        'a sums line with a folder' = @("$('0' * 64)  dist/qKVM.zip`n", 'two spaces')
        'sums with a carriage return' = @("$('0' * 64)  qKVM.zip`r`n", 'one line feed')
        'sums named twice' = @("$('0' * 64)  qKVM.zip`n$('1' * 64)  qKVM.zip`n", 'named twice')
        'plain text' = @("just some words that are no release file at all, long enough to be looked at`n", 'two spaces')
    }
    $number = 0
    foreach ($case in $wrongOnes.GetEnumerator()) {
        $number++
        $file = Write-TestFile "wrong-$number.json" $case.Value[0]
        Expect-SigningRefusal { Invoke-SignRelease -Key $keyFile -PublicKey $made.PublicKey -File $file -Output $refused -Passphrase $passphrase } $case.Value[1]
    }
    Assert-SigningTest (-not (Test-Path -LiteralPath $refused)) 'A refused signing left a signature.'

    # 5. A SHA256SUMS file signed, as it is.
    $sums = Write-TestFile 'SHA256SUMS' "$('0' * 64)  qKVM-Desktop-0.2.0-windows-x64.zip`n$('a' * 64)  qkvm-desktop-0.2.0-source.tar.gz`n"
    $sumsSignature = Invoke-SignRelease -Key $keyFile -PublicKey $made.PublicKey -File $sums -Output "$sums.sig" -Passphrase $passphrase
    Assert-SigningTest (Test-ReleaseSignature -Data ([IO.File]::ReadAllBytes($sums)) -Signature ([IO.File]::ReadAllBytes($sumsSignature)) -PublicKey $made.PublicKey) 'The SHA256SUMS signature does not check.'

    # 5b. What is shown is what is signed: the file is read once, what it says and its SHA-256 are shown, and those
    # very bytes are signed, whatever lies in the file by the time the passphrase has been typed.
    $shownFile = Write-TestFile 'SHA256SUMS-shown' "$('1' * 64)  qKVM-Desktop-0.2.0-windows-x64.zip`n"
    $asShown = [IO.File]::ReadAllBytes($shownFile)
    $shown = Get-SignableFile $shownFile
    Assert-SigningTest ($shown.Sha256 -ceq [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($asShown)).ToLowerInvariant()) "What is shown does not say the SHA-256 of what was read ($($shown.Sha256))."
    [IO.File]::WriteAllBytes($shownFile, [Text.Encoding]::ASCII.GetBytes("$('2' * 64)  qKVM-Desktop-0.2.0-windows-x64.zip`n"))
    $shownSignature = Invoke-SignRelease -Key $keyFile -PublicKey $made.PublicKey -What $shown -Output "$shownFile.sig" -Passphrase $passphrase
    $signedShown = [IO.File]::ReadAllBytes($shownSignature)
    Assert-SigningTest (Test-ReleaseSignature -Data $asShown -Signature $signedShown -PublicKey $made.PublicKey) 'What was shown is not what was signed.'
    Assert-SigningTest (-not (Test-ReleaseSignature -Data ([IO.File]::ReadAllBytes($shownFile)) -Signature $signedShown -PublicKey $made.PublicKey)) 'The file as it was changed after it was shown was signed.'
    # And sign_release.ps1 itself reads the file once: to show it, to sign it, and to say the SHA-256 of what it signed.
    $tool = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'sign_release.ps1'))
    Assert-SigningTest ([regex]::Matches($tool, 'Get-SignableFile').Count -eq 1 -and $tool -match 'Invoke-SignRelease [^\r\n]*-What \$what' -and
        $tool -notmatch 'Invoke-SignRelease [^\r\n]*-File' -and $tool -notmatch 'Get-FileHash|ReadAllBytes|Get-Content|Get-Item') 'sign_release.ps1 reads what it signs more than once.'
    Assert-SigningTest ([regex]::Matches($tool, '\$what\.Sha256').Count -eq 2) 'sign_release.ps1 does not show the SHA-256 of what it signs, and say it of what it signed.'

    # 6. The feed signed again: new dates, every other field as it was, and the program's own check of it (a public
    # fixture, made below, for the C++ verifier).
    $refreshed = Join-Path $public 'stable-refreshed.json'
    $later = [DateTime]::UtcNow.AddSeconds(-5)
    $told = Invoke-RefreshFeed -Feed $feed -Key $keyFile -PublicKey $made.PublicKey -Output $refreshed -ValidDays 30 -Passphrase $passphrase -Now $later
    $newPayload = Read-SignedFeed -Feed $refreshed -PublicKey $made.PublicKey
    $newFields = Test-UpdatePayload -Bytes $newPayload
    $oldFields = Test-UpdatePayload -Bytes ([IO.File]::ReadAllBytes($payload))
    foreach ($name in $UpdatePayloadFields) {
        if ($name -cin @('published', 'expires')) { continue }
        Assert-SigningTest ($newFields[$name] -ceq $oldFields[$name]) "The refreshed feed changed $name."
    }
    Assert-SigningTest ($newFields.published -ceq (Write-UtcDate $later) -and $newFields.expires -ceq (Write-UtcDate $later.AddDays(30))) 'The refreshed feed has not the new dates.'
    $oldText = [Text.Encoding]::UTF8.GetString([IO.File]::ReadAllBytes($payload)) -replace '"(published|expires)":"[^"]+"', ''
    $newText = [Text.Encoding]::UTF8.GetString($newPayload) -replace '"(published|expires)":"[^"]+"', ''
    Assert-SigningTest ($newText -ceq $oldText) 'The refreshed payload is not written as prepare_update_payload.ps1 writes it.'
    Assert-SigningTest ($told.expires -ceq $newFields.expires) 'The refresh said another expiry than it signed.'
    # An expired feed is refreshed too, its notes as they were written (quotes, a new line); a feed of another key
    # is not; nothing is written over.
    $noted = (& $withDates $now.AddDays(-20) $now.AddDays(-1)) -replace '"notes":"Test release"', '"notes":"Test \"quoted\" release\nand more"'
    Assert-SigningTest ($noted.Contains('quoted')) 'The notes of the expired payload were not written.'
    $expiredPayload = Write-TestFile 'expired-payload.json' $noted
    $expiredSigner = Open-ReleaseKey -Key $keyFile -Passphrase $passphrase -PublicKey $made.PublicKey
    try { $expiredSignature = Get-ReleaseSignature -Signer $expiredSigner -Data ([IO.File]::ReadAllBytes($expiredPayload)) -PublicKey $made.PublicKey } finally { $expiredSigner.Dispose() }
    $expiredFeed = Write-TestFile 'expired-feed.json' (([ordered]@{ payload = [Convert]::ToBase64String([IO.File]::ReadAllBytes($expiredPayload)); signature = [Convert]::ToBase64String($expiredSignature) }) | ConvertTo-Json -Compress)
    $fromExpired = Join-Path $public 'from-expired.json'
    $null = Invoke-RefreshFeed -Feed $expiredFeed -Key $keyFile -PublicKey $made.PublicKey -Output $fromExpired -ValidDays 7 -Passphrase $passphrase
    $fromExpiredPayload = Read-SignedFeed -Feed $fromExpired -PublicKey $made.PublicKey
    $null = Test-UpdatePayload -Bytes $fromExpiredPayload
    $notedBefore = $noted -replace '"(published|expires)":"[^"]+"', ''
    $notedAfter = [Text.Encoding]::UTF8.GetString($fromExpiredPayload) -replace '"(published|expires)":"[^"]+"', ''
    Assert-SigningTest ($notedAfter -ceq $notedBefore) "The refreshed notes are not as they were: $notedAfter"
    Assert-SigningTest ((Test-UpdatePayload -Bytes $fromExpiredPayload).notes -ceq "Test ""quoted"" release`nand more") 'The notes were read otherwise.'
    $refusedFeed = Join-Path $public 'refused-feed.json'
    Expect-SigningRefusal { Invoke-RefreshFeed -Feed $feed -Key $keyFile -PublicKey $otherKey.PublicKey -Output $refusedFeed -ValidDays 30 -Passphrase $passphrase } 'not signed by the key the program pins'
    Expect-SigningRefusal { Invoke-RefreshFeed -Feed $feed -Key (Join-Path $other $ReleaseKeyName) -PublicKey $otherKey.PublicKey -Output $refusedFeed -ValidDays 30 -Passphrase $passphrase } 'not signed by the key the program pins'
    Expect-SigningRefusal { Invoke-RefreshFeed -Feed $feed -Key $keyFile -PublicKey $made.PublicKey -Output $refreshed -ValidDays 30 -Passphrase $passphrase } 'there already'
    Expect-SigningRefusal { Invoke-RefreshFeed -Feed $feed -Key $keyFile -PublicKey $made.PublicKey -Output $refusedFeed -ValidDays 30 -Passphrase $wrong } 'passphrase does not open'
    $kept = [IO.File]::ReadAllText($feed) | ConvertFrom-Json
    $flipped = [Convert]::FromBase64String($kept.signature)
    $flipped[10] = $flipped[10] -bxor 1
    $tamperedFeed = Write-TestFile 'tampered-feed.json' (([ordered]@{ payload = $kept.payload; signature = [Convert]::ToBase64String($flipped) }) | ConvertTo-Json -Compress)
    Expect-SigningRefusal { Invoke-RefreshFeed -Feed $tamperedFeed -Key $keyFile -PublicKey $made.PublicKey -Output $refusedFeed -ValidDays 30 -Passphrase $passphrase } 'not signed by the key the program pins'
    Assert-SigningTest (-not (Test-Path -LiteralPath $refusedFeed)) 'A refused refresh left a feed.'

    # 6b. A Setup release's feed (stable-setup.json) is signed, assembled and refreshed the same way, by itself.
    $setupFile = Join-Path $public 'qKVM-Desktop-0.2.0-windows-x64-setup.exe'
    [IO.File]::WriteAllBytes($setupFile, [Text.Encoding]::UTF8.GetBytes('A made-up test setup; never run.'))
    $setupPayload = Join-Path $public 'setup-payload.json'
    & "$PSScriptRoot/prepare_update_payload.ps1" -TestPackage -Setup -Package $setupFile -Version '0.2.0' -ValidDays 2 `
        -PackageUrl 'https://updates.example.test/qKVM-Desktop-0.2.0-windows-x64-setup.exe' -Notes 'Test release' -Output $setupPayload | Out-Null
    $setupFeed = Join-Path $public 'stable-setup.json'
    $setupRefreshed = Join-Path $public 'stable-setup-refreshed.json'
    try {
        $setupSignature = Invoke-SignRelease -Key $keyFile -PublicKey $made.PublicKey -File $setupPayload -Output "$setupPayload.sig" -Passphrase $passphrase
        $assembledSaid = @(& "$PSScriptRoot/assemble_update_feed.ps1" -Payload $setupPayload -Signature $setupSignature -PublicKey $made.PublicKey -Output $setupFeed)
        $setupTold = Invoke-RefreshFeed -Feed $setupFeed -Key $keyFile -PublicKey $made.PublicKey -Output $setupRefreshed -ValidDays 30 -Passphrase $passphrase -Now $later
    } catch { throw "FAIL: the setup feed was not signed, assembled and refreshed: $_" }
    Assert-SigningTest (($assembledSaid -join "`n") -match 'Serve it as stable-setup\.json') "The setup feed is not said to be served as stable-setup.json: $assembledSaid"
    $setupNew = Test-UpdatePayload -Bytes (Read-SignedFeed -Feed $setupRefreshed -PublicKey $made.PublicKey)
    $setupOld = Test-UpdatePayload -Bytes ([IO.File]::ReadAllBytes($setupPayload))
    foreach ($name in $UpdatePayloadFields) {
        if ($name -cin @('published', 'expires')) { continue }
        Assert-SigningTest ($setupNew[$name] -ceq $setupOld[$name]) "The refreshed setup feed changed $name."
    }
    Assert-SigningTest ($setupNew.platform -ceq 'windows-x64-setup' -and $setupTold.expires -ceq $setupNew.expires) 'The setup feed was not refreshed as a setup feed.'
    Assert-SigningTest ((Get-FeedName $setupNew.platform) -ceq 'stable-setup.json' -and (Get-FeedName $newFields.platform) -ceq 'stable.json') 'The feeds are not named for their kinds.'
    Assert-SigningTest ([IO.File]::ReadAllText((Join-Path $PSScriptRoot 'refresh_feed.ps1')) -match 'Get-FeedName \$was\.platform') 'refresh_feed.ps1 does not say which feed it wrote.'

    $publicFixture = [ordered]@{
        source = 'prepare_update_payload.ps1 -Portable, Invoke-SignRelease (sign_release.ps1), assemble_update_feed.ps1, then Invoke-RefreshFeed (refresh_feed.ps1); a throwaway encrypted .NET P-256 key, removed'
        checked_at = $newFields.published
        public_key = $made.PublicKey
        feed = [Convert]::ToBase64String([IO.File]::ReadAllBytes($refreshed))
    }
    [IO.File]::WriteAllText((Join-Path $public 'desktop-fixture-refreshed.json'), ($publicFixture | ConvertTo-Json) + "`n",
        [Text.UTF8Encoding]::new($false))
    $passphrase.Dispose()
    $wrong.Dispose()
} finally {
    Remove-Item -LiteralPath $keys -Recurse -Force
}
Assert-SigningTest (-not (Test-Path -LiteralPath $keys)) 'The throwaway keys were not removed.'
Write-Output "$checks release-signing checks passed. Throwaway keys removed; public test files: $public"
