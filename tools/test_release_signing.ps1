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
    # And sign_release.ps1 itself reads each file once (Get-SignableFiles): to show it, to sign it, and to say the
    # SHA-256 of what it signed; it shows every file's name and SHA-256 before it asks for the passphrase, once.
    $tool = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'sign_release.ps1'))
    Assert-SigningTest ([regex]::Matches($tool, 'Get-SignableFiles?\b').Count -eq 1 -and $tool -match 'Invoke-SignReleaseFiles [^\r\n]*-What \$what' -and
        $tool -notmatch 'Invoke-SignRelease\s' -and $tool -notmatch 'Get-FileHash|ReadAllBytes|Get-Content|Get-Item') 'sign_release.ps1 reads what it signs more than once.'
    $asks = $tool.IndexOf('Read-ReleasePassphrase')
    Assert-SigningTest ([regex]::Matches($tool, 'Read-ReleasePassphrase').Count -eq 1 -and $tool.IndexOf('$one.Name') -gt 0 -and
        $tool.IndexOf('$one.Sha256') -gt 0 -and $asks -gt $tool.IndexOf('$one.Name') -and $asks -gt $tool.IndexOf('$one.Sha256')) 'sign_release.ps1 does not show every file''s name and SHA-256 before it asks for the passphrase, once.'
    Assert-SigningTest ($tool -match '\$what\[\$at\]\.Sha256') 'sign_release.ps1 does not say the SHA-256 of what it signed.'

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
    # (Published ten minutes before it is signed, as a new payload is: a PC whose clock is a little behind takes it; i020.)
Assert-SigningTest ($newFields.published -ceq (Write-UtcDate $later.AddMinutes(-10)) -and $newFields.expires -ceq (Write-UtcDate $later.AddDays(30))) 'The refreshed feed has not the new dates.'
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
    $refreshTool = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'refresh_feed.ps1'))
    Assert-SigningTest ($refreshTool -match 'Get-FeedName \$what\[\$at\]\.Fields\.platform') 'refresh_feed.ps1 does not say which feed it wrote.'
    $refreshAsks = $refreshTool.IndexOf('Read-ReleasePassphrase')
    Assert-SigningTest ([regex]::Matches($refreshTool, 'Read-ReleasePassphrase').Count -eq 1 -and $refreshTool.IndexOf('$one.Name') -gt 0 -and
        $refreshTool.IndexOf('$one.Sha256') -gt 0 -and $refreshAsks -gt $refreshTool.IndexOf('$one.Name') -and
        $refreshAsks -gt $refreshTool.IndexOf('$one.Sha256') -and $refreshTool -match 'Invoke-RefreshFeeds [^\r\n]*-What \$what' -and
        $refreshTool -notmatch 'Invoke-RefreshFeed\s') 'refresh_feed.ps1 does not show every feed''s name and the SHA-256 of what it signs before it asks for the passphrase, once.'

    # 7. Several files signed with one passphrase (the owner, 2026-11-19): each read once and shown by its name and the
    # SHA-256 of its bytes; the key opened once; each signature of its own file's bytes; and where any one file is not
    # signable, named twice, or its signature's place is taken, nothing is signed or left.
    $script:openings = 0
    $opener = ${function:Open-ReleaseKey}
    function Open-ReleaseKey([string] $Key, [Security.SecureString] $Passphrase, [string] $PublicKey) {
        $script:openings++
        & $opener -Key $Key -Passphrase $Passphrase -PublicKey $PublicKey
    }
    $sumsMany = Write-TestFile 'SHA256SUMS-many' "$('3' * 64)  qKVM-Desktop-0.2.0-windows-arm64.zip`n$('4' * 64)  qKVM-Desktop-0.2.0-windows-setup.exe`n"
    $armZip = Join-Path $public 'arm64.zip'
    [IO.File]::WriteAllBytes($armZip, [Text.Encoding]::UTF8.GetBytes('A made-up test package for Arm; never installed.'))
    $armPayload = Join-Path $public 'payload-arm64.json'
    & "$PSScriptRoot/prepare_update_payload.ps1" -TestPackage -Portable -Arch arm64 -Package $armZip -Version '0.2.0' -ValidDays 2 `
        -PackageUrl 'https://updates.example.test/qKVM-Desktop-0.2.0-windows-arm64.zip' -Notes 'Test release' -Output $armPayload | Out-Null
    $many = @($payload, $armPayload, $sumsMany)
    $read = Get-SignableFiles $many
    Assert-SigningTest (@($read).Count -eq 3) "Three files to sign were read as $(@($read).Count)."
    for ($at = 0; $at -lt 3; $at++) {
        Assert-SigningTest ($read[$at].Name -ceq (Split-Path -Leaf $many[$at]) -and $read[$at].Sha256 -ceq (Get-FileHash -LiteralPath $many[$at]).Hash.ToLowerInvariant()) "File $at is not shown by its name and its SHA-256."
    }
    Assert-SigningTest ($read[1].Said -match 'windows-arm64-portable') "The ARM64 payload is not said as one: $($read[1].Said)"
    $manyOut = @("$payload.many.sig", "$armPayload.many.sig", "$sumsMany.many.sig")
    # (What is shown is what is signed: a file changes after it was read, and the bytes read are signed.)
    $readBytes = [Collections.Generic.List[byte[]]]::new()
    foreach ($one in $read) { $readBytes.Add([byte[]] $one.Bytes) }
    [IO.File]::WriteAllBytes($sumsMany, [Text.Encoding]::ASCII.GetBytes("$('5' * 64)  qKVM-Desktop-0.2.0-windows-arm64.zip`n"))
    $script:openings = 0
    $wrote = Invoke-SignReleaseFiles -Key $keyFile -PublicKey $made.PublicKey -What $read -Outputs $manyOut -Passphrase $passphrase
    Assert-SigningTest ($script:openings -eq 1) "The key was opened $($script:openings) times for three files."
    Assert-SigningTest (@($wrote).Count -eq 3) 'Not every file''s signature was said.'
    for ($at = 0; $at -lt 3; $at++) {
        $sig = [IO.File]::ReadAllBytes($manyOut[$at])
        Assert-SigningTest (Test-ReleaseSignature -Data $readBytes[$at] -Signature $sig -PublicKey $made.PublicKey) "Signature $at is not of the bytes read of its own file."
        Assert-SigningTest (-not (Test-ReleaseSignature -Data $readBytes[($at + 1) % 3] -Signature $sig -PublicKey $made.PublicKey)) "Signature $at checks against another file."
    }
    Assert-SigningTest (-not (Test-ReleaseSignature -Data ([IO.File]::ReadAllBytes($sumsMany)) -Signature ([IO.File]::ReadAllBytes($manyOut[2])) -PublicKey $made.PublicKey)) 'The file as it was changed after it was read was signed.'
    # Any one file that is no release file, or not current, or named twice: nothing signed, and it is named.
    $notRelease = Write-TestFile 'not-a-release.txt' "just some words that are no release file at all, long enough to be looked at`n"
    Expect-SigningRefusal { Get-SignableFiles @($payload, $notRelease, $armPayload) } 'not-a-release\.txt.*Nothing was signed'
    $expiredMany = Write-TestFile 'expired-many.json' (& $withDates $now.AddDays(-10) $now.AddDays(-1))
    Expect-SigningRefusal { Get-SignableFiles @($payload, $armPayload, $expiredMany) } 'expired-many\.json.*not current.*Nothing was signed'
    Expect-SigningRefusal { Get-SignableFiles @($payload, $armPayload, $payload) } 'named twice'
    Expect-SigningRefusal { Get-SignableFiles @() } 'Name the files'
    # A signature's place taken, named twice, or one of the files signed, or one missing: nothing signed, nothing left.
    $read = Get-SignableFiles @($payload, $armPayload)
    $script:openings = 0
    Expect-SigningRefusal { Invoke-SignReleaseFiles -Key $keyFile -PublicKey $made.PublicKey -What $read -Outputs @("$payload.r2.sig", $manyOut[1]) -Passphrase $passphrase } 'there already'
    Expect-SigningRefusal { Invoke-SignReleaseFiles -Key $keyFile -PublicKey $made.PublicKey -What $read -Outputs @("$payload.r3.sig", "$payload.r3.sig") -Passphrase $passphrase } 'named twice'
    Expect-SigningRefusal { Invoke-SignReleaseFiles -Key $keyFile -PublicKey $made.PublicKey -What $read -Outputs @("$payload.r4.sig") -Passphrase $passphrase } 'one signature file of its own'
    Assert-SigningTest ($script:openings -eq 0) 'The key was opened for files whose signatures had nowhere to go.'
    Expect-SigningRefusal { Invoke-SignReleaseFiles -Key $keyFile -PublicKey $made.PublicKey -What $read -Outputs @("$payload.r5.sig", "$armPayload.r5.sig") -Passphrase $wrong } 'passphrase does not open'
    Expect-SigningRefusal { Invoke-SignReleaseFiles -Key $keyFile -PublicKey $otherKey.PublicKey -What $read -Outputs @("$payload.r5.sig", "$armPayload.r5.sig") -Passphrase $passphrase } 'not the one the program pins'
    # One that cannot be written (its folder is not there): none left, the first taken away again.
    Expect-SigningRefusal { Invoke-SignReleaseFiles -Key $keyFile -PublicKey $made.PublicKey -What $read -Outputs @("$payload.r6.sig", (Join-Path $public 'no-such-folder\x.sig')) -Passphrase $passphrase } 'None was left'
    foreach ($place in "$notRelease.sig", "$expiredMany.sig", "$payload.r2.sig", "$payload.r3.sig", "$payload.r4.sig", "$payload.r5.sig", "$armPayload.r5.sig", "$payload.r6.sig") {
        Assert-SigningTest (-not (Test-Path -LiteralPath $place)) "A refusal left $place."
    }

    # 8. Several feeds signed again with one passphrase: each new payload made and shown (its feed's name and the
    # SHA-256 of the bytes then signed) before the key is opened, once; each new feed of its own payload; where any one
    # does not read or its new file's place is taken, nothing.
    $armSignature = Invoke-SignRelease -Key $keyFile -PublicKey $made.PublicKey -File $armPayload -Output "$armPayload.sig" -Passphrase $passphrase
    $armFeed = Join-Path $public 'stable-arm64.json'
    & "$PSScriptRoot/assemble_update_feed.ps1" -Payload $armPayload -Signature $armSignature -PublicKey $made.PublicKey -Output $armFeed | Out-Null
    $feeds = @($feed, $setupFeed, $armFeed)
    $fresh = @($feeds | ForEach-Object { Get-RefreshedPayload -Feed $_ -PublicKey $made.PublicKey -ValidDays 30 -Now $later })
    $freshOut = @((Join-Path $public 'many-stable.json'), (Join-Path $public 'many-stable-setup.json'), (Join-Path $public 'many-stable-arm64.json'))
    $script:openings = 0
    $wroteFeeds = Invoke-RefreshFeeds -Key $keyFile -PublicKey $made.PublicKey -What $fresh -Outputs $freshOut -Passphrase $passphrase
    Assert-SigningTest ($script:openings -eq 1 -and @($wroteFeeds).Count -eq 3) "Three feeds were refreshed with $($script:openings) openings of the key, $(@($wroteFeeds).Count) written."
    for ($at = 0; $at -lt 3; $at++) {
        $newBytes = Read-SignedFeed -Feed $freshOut[$at] -PublicKey $made.PublicKey
        Assert-SigningTest ($fresh[$at].Name -ceq (Split-Path -Leaf $feeds[$at])) "Feed $at is not shown by its name."
        Assert-SigningTest ($fresh[$at].Sha256 -ceq [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($newBytes)).ToLowerInvariant()) "Feed $at's SHA-256 shown is not that of the payload it signed."
        $before = Test-UpdatePayload -Bytes (Read-SignedFeed -Feed $feeds[$at] -PublicKey $made.PublicKey) -AllowExpired
        $after = Test-UpdatePayload -Bytes $newBytes
        foreach ($name in @($UpdatePayloadFields | Where-Object { $_ -cnotin 'published', 'expires' })) {
            Assert-SigningTest ($after[$name] -ceq $before[$name]) "Feed $at refreshed changed $name."
        }
    }
    Assert-SigningTest ((Get-FeedName $fresh[2].Fields.platform) -ceq 'stable-arm64.json') 'The ARM64 feed is not served as stable-arm64.json.'
    # The set refreshed is the one a release serves (review armp-1 L6): the four platforms once each, at one version,
    # each feed read from a file of a served name of that very name's kind; fewer only where -Partial says so, and
    # those left out named; and the upload of each under the name it is served at, never cached.
    $feedSet = Get-FeedSetRefusals -What $fresh
    Assert-SigningTest (@($feedSet.Refusals | Where-Object { $_ -match 'not the whole set: stable-arm64-setup\.json is not among them' }).Count -eq 1) "Three feeds of four were taken for the whole set ($(@($feedSet.Refusals) -join '; '))."
    $feedSet = Get-FeedSetRefusals -What $fresh -Partial
    Assert-SigningTest (@($feedSet.Refusals).Count -eq 0 -and (@($feedSet.Missing) -join ',') -ceq 'stable-arm64-setup.json') "Three feeds of four, said to be fewer, were not taken with the one left out named ($(@($feedSet.Refusals) -join '; '))."
    $madeUp = { param([string] $name, [string] $platform, [string] $version = '0.2.0') [pscustomobject]@{ Name = $name; Fields = [ordered]@{ platform = $platform; version = $version } } }
    $four = @((& $madeUp 'stable.json' 'windows-x64-portable'), (& $madeUp 'stable-setup.json' 'windows-x64-setup'), (& $madeUp 'stable-arm64.json' 'windows-arm64-portable'),
        (& $madeUp 'stable-arm64-setup.json' 'windows-arm64-setup'))
    $feedSet = Get-FeedSetRefusals -What $four
    Assert-SigningTest (@($feedSet.Refusals).Count -eq 0 -and @($feedSet.Missing).Count -eq 0) "The four feeds were not taken as the whole set ($(@($feedSet.Refusals) -join '; '))."
    $renamed = @((& $madeUp 'stable-new.json' 'windows-x64-portable'), (& $madeUp 'refresh-2.json' 'windows-x64-setup'), $four[2], $four[3])
    Assert-SigningTest (@((Get-FeedSetRefusals -What $renamed).Refusals).Count -eq 0) 'Feeds read from files of other names than the served ones were refused.'
    foreach ($case in @(
            @(@($four[0], $four[1], $four[2], (& $madeUp 'stable-arm64-setup.json' 'windows-x64-setup')), 'stable-arm64-setup\.json holds windows-x64-setup', 'A feed of one kind read from the served name of another'),
            @(@($four[0], $four[0], $four[2], $four[3]), 'windows-x64-portable is there twice', 'A platform twice'),
            @(@($four[0], $four[1], $four[2], (& $madeUp 'stable-arm64-setup.json' 'windows-arm64-setup' '0.2.1')), 'more than one version \(0\.2\.0, 0\.2\.1\)', 'Feeds of two versions'),
            @(@($four[0], $four[1], $four[2], (& $madeUp 'stable-arm64-msix.json' 'windows-arm64-msix')), 'windows-arm64-msix is no feed a release serves', 'A feed of a platform no release serves'))) {
        $refused = @((Get-FeedSetRefusals -What $case[0] -Partial).Refusals)
        Assert-SigningTest (@($refused | Where-Object { $_ -match $case[1] }).Count -eq 1) "$($case[2]) was not refused ($($refused -join '; '))."
    }
    $lines = @(Get-FeedUploadLines -What $four -Written @('C:\f\a.json', 'C:\f\b.json', 'C:\f\c.json', 'C:\f\d.json') -Bucket 'qkvm-downloads')
    $served = @('stable.json', 'stable-setup.json', 'stable-arm64.json', 'stable-arm64-setup.json')
    for ($at = 0; $at -lt 4; $at++) {
        Assert-SigningTest ($lines[$at] -ceq "npx --yes wrangler@4.152.0 r2 object put `"qkvm-downloads/desktop/$($served[$at])`" --file `"C:\f\$([char] (97 + $at)).json`" --content-type `"application/json`" --cache-control `"no-cache`" --remote") "Feed $at's upload is not said as release_commands.ps1 says it ($($lines[$at]))."
    }
    Assert-SigningTest ($lines.Count -eq 4) "Not one upload for each refreshed feed ($($lines.Count))."
    $refreshText = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'refresh_feed.ps1'))
    $setAt = $refreshText.IndexOf('$set = Get-FeedSetRefusals -What $what -Partial:$Partial')
    Assert-SigningTest ($setAt -gt 0 -and $setAt -lt $refreshText.IndexOf('$passphrase = Read-ReleasePassphrase') -and $refreshText.Contains('if (@($set.Refusals).Count) { throw') -and
        $refreshText.Contains('Get-FeedUploadLines -What $what -Written $written -Bucket $Bucket')) 'refresh_feed.ps1 does not hold the feeds to the set a release serves before it asks for the passphrase, or does not say their uploads.'
    Expect-SigningRefusal { Get-RefreshedPayload -Feed $tamperedFeed -PublicKey $made.PublicKey -ValidDays 30 } 'not signed by the key the program pins'
    $mixed = @($fresh[0], $fresh[1])
    $script:openings = 0
    Expect-SigningRefusal { Invoke-RefreshFeeds -Key $keyFile -PublicKey $made.PublicKey -What $mixed -Outputs @((Join-Path $public 'r7a.json'), $freshOut[2]) -Passphrase $passphrase } 'there already'
    Expect-SigningRefusal { Invoke-RefreshFeeds -Key $keyFile -PublicKey $made.PublicKey -What $mixed -Outputs @((Join-Path $public 'r7b.json'), $feeds[0]) -Passphrase $passphrase } 'there already'
    Expect-SigningRefusal { Invoke-RefreshFeeds -Key $keyFile -PublicKey $made.PublicKey -What @($fresh[0], $fresh[0]) -Outputs @((Join-Path $public 'r7c.json'), (Join-Path $public 'r7d.json')) -Passphrase $passphrase } 'named twice'
    Expect-SigningRefusal { Invoke-RefreshFeeds -Key $keyFile -PublicKey $made.PublicKey -What $mixed -Outputs @((Join-Path $public 'r7g.json'), (Join-Path $public 'r7g.json')) -Passphrase $passphrase } 'named twice'
    Expect-SigningRefusal { Invoke-RefreshFeeds -Key $keyFile -PublicKey $made.PublicKey -What $mixed -Outputs @((Join-Path $public 'r7e.json')) -Passphrase $passphrase } 'one new file of its own'
    Assert-SigningTest ($script:openings -eq 0) 'The key was opened for feeds whose new files had nowhere to go.'
    Expect-SigningRefusal { Invoke-RefreshFeeds -Key $keyFile -PublicKey $made.PublicKey -What $mixed -Outputs @((Join-Path $public 'r7f.json'), (Join-Path $public 'no-such-folder\y.json')) -Passphrase $passphrase } 'None was left'
    foreach ($place in 'r7a.json', 'r7b.json', 'r7c.json', 'r7d.json', 'r7e.json', 'r7f.json', 'r7g.json') {
        Assert-SigningTest (-not (Test-Path -LiteralPath (Join-Path $public $place))) "A refusal left $place."
    }
    Remove-Item Function:\Open-ReleaseKey

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
