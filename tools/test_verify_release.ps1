#Requires -Version 7.2
<#
tools/verify_release.ps1 on made-up published files: a folder, and the same served on this PC's own address
(127.0.0.1, by Python's http.server), signed with a throwaway key held in memory alone, and run as it is run: by
pwsh -File, every place in one -From as release_commands.ps1 prints it. What was built passes; every changed file,
list, signature and feed fails, so does a redirect from anywhere but GitHub, a package the feed names that is not
the ZIP built, and a run with no place to check. Nothing is asked of any host but this PC's own. Every failed check
is a "FAIL:" line, and the script then fails.
#>
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\build_common.ps1"

$failures = 0
function Check([bool] $Condition, [string] $Message) {
    if (-not $Condition) { Write-Host "FAIL: $Message"; $script:failures++ }
}
$work = Join-Path (Split-Path -Parent $PSScriptRoot) "build/release-tests/$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $work | Out-Null
$signer = [Security.Cryptography.ECDsa]::Create([Security.Cryptography.ECCurve+NamedCurves]::nistP256)
$other = [Security.Cryptography.ECDsa]::Create([Security.Cryptography.ECCurve+NamedCurves]::nistP256)
$q = $signer.ExportParameters($false).Q
$publicKey = ([Convert]::ToHexString($q.X) + [Convert]::ToHexString($q.Y)).ToLowerInvariant()
function Sign([byte[]] $Data, $Key = $signer) {
    return $Key.SignData($Data, [Security.Cryptography.HashAlgorithmName]::SHA256,
        [Security.Cryptography.DSASignatureFormat]::IeeeP1363FixedFieldConcatenation)
}
$names = Get-ReleaseFileNames '0.1.0'
$zipName = 'qKVM-Desktop-0.1.0-windows-x64.zip'
$setupName = 'qKVM-Desktop-0.1.0-windows-x64-setup.exe'

# The stand-in host, first: what each case publishes is served from the folder it is written to.
$listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
$listener.Start(); $port = $listener.LocalEndpoint.Port; $listener.Stop()
$host_ = "http://127.0.0.1:$port"
$python = (Get-Command python -CommandType Application | Select-Object -First 1).Source
$served = Start-Process -FilePath $python -ArgumentList @('-I', '-m', 'http.server', '--bind', '127.0.0.1', "$port", '--directory', $work) -PassThru -WindowStyle Hidden

function Write-Feed($Context, [string] $File, [hashtable] $Change = @{}, [switch] $Setup) {
    # (-Setup: installed copies' own feed, stable-setup.json.)
    $from = if ($Setup) { $Context.SetupPayload } else { $Context.Payload }
    $payload = [ordered]@{}
    foreach ($key in $from.Keys) { $payload[$key] = $from[$key] }
    foreach ($key in $Change.Keys) { $payload[$key] = $Change[$key] }
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes(($payload | ConvertTo-Json -Compress))
    $key = if ($Setup) { $Context.SetupFeedKey } else { $Context.FeedKey }
    [ordered]@{ payload = [Convert]::ToBase64String($bytes); signature = [Convert]::ToBase64String((Sign $bytes $key)) } |
        ConvertTo-Json -Compress | Set-Content -LiteralPath $File -Encoding utf8 -NoNewline
}
function New-Published([string] $Name, [scriptblock] $Change = {}) {
    $case = Join-Path $work $Name
    $run = (New-Item -ItemType Directory -Path "$case/run" -Force).FullName
    $files = (New-Item -ItemType Directory -Path "$case/published" -Force).FullName
    foreach ($file in $names | Where-Object { $_ -cne 'SHA256SUMS' }) { [IO.File]::WriteAllText((Join-Path $files $file), "made-up $file") }
    $sums = (($names | Where-Object { $_ -cne 'SHA256SUMS' } | Sort-Object -CaseSensitive |
        ForEach-Object { "$(Get-Sha256 (Join-Path $files $_))  $_`n" }) -join '')
    [IO.File]::WriteAllBytes("$files/SHA256SUMS", [Text.Encoding]::ASCII.GetBytes($sums))
    [IO.File]::WriteAllBytes("$files/SHA256SUMS.sig", (Sign ([IO.File]::ReadAllBytes("$files/SHA256SUMS"))))
    $zip = Join-Path $files $zipName
    $record = [ordered]@{ schema = 1; version = '0.1.0'; trial = $false; source = [ordered]@{ commit = 'c' * 40 }
        updates = [ordered]@{ publicKey = $publicKey }
        package = [ordered]@{ archive = $zipName; bytes = (Get-Item $zip).Length; sha256 = Get-Sha256 $zip }
        setup = [ordered]@{ name = $setupName; bytes = (Get-Item (Join-Path $files $setupName)).Length; sha256 = Get-Sha256 (Join-Path $files $setupName) } }
    $written = [ordered]@{ version = '0.1.0'; commit = 'c' * 40; files = @($names | ForEach-Object {
        [ordered]@{ name = $_; bytes = (Get-Item (Join-Path $files $_)).Length; sha256 = Get-Sha256 (Join-Path $files $_) } }) }
    $now = [DateTime]::UtcNow
    $payload = [ordered]@{ schema = 1; product = 'qkvm-desktop'; platform = 'windows-x64-portable'; channel = 'stable'
        version = '0.1.0'; published = $now.AddHours(-1).ToString("yyyy-MM-dd'T'HH:mm:ss'Z'")
        expires = $now.AddDays(29).ToString("yyyy-MM-dd'T'HH:mm:ss'Z'")
        package = "$host_/$Name/published/$zipName"; bytes = $record.package.bytes; sha256 = $record.package.sha256
        notes = 'made-up notes' }
    $setupPayload = [ordered]@{ schema = 1; product = 'qkvm-desktop'; platform = 'windows-x64-setup'; channel = 'stable'
        version = '0.1.0'; published = $payload.published; expires = $payload.expires
        package = "$host_/$Name/published/$setupName"; bytes = $record.setup.bytes; sha256 = $record.setup.sha256
        notes = 'made-up notes' }
    $context = [pscustomobject]@{ Name = $Name; Case = $case; Run = $run; Files = $files; Record = $record; Written = $written
        Payload = $payload; FeedKey = $signer; Feed = "$case/stable.json"; Served = "$host_/$Name/published"
        SetupPayload = $setupPayload; SetupFeedKey = $signer; SetupFeed = "$case/stable-setup.json" }
    & $Change $context
    $context.Record | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$run/build-record.json" -Encoding utf8
    $context.Written | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$run/release-files.json" -Encoding utf8
    Write-Feed $context $context.Feed
    Write-Feed $context $context.SetupFeed -Setup
    return $context
}
function Invoke-Line([string[]] $Arguments) {
    $said = & (Get-Process -Id $PID).Path -NoProfile -NonInteractive @Arguments 2>&1
    return [pscustomobject]@{ Ended = $LASTEXITCODE; Said = ($said | ForEach-Object { "$_" }) -join "`n" }
}
function Invoke-Verify($Context, [string[]] $From, [switch] $RequireSignature, [switch] $NoFeed) {
    # (As the owner runs it: pwsh -File, and every place in one -From, a comma between them.)
    $arguments = @('-File', "$PSScriptRoot/verify_release.ps1", '-Run', $Context.Run)
    if ($From.Count) { $arguments += @('-From', ($From -join ',')) }
    if (-not $NoFeed) { $arguments += @('-Feed', $Context.Feed, '-SetupFeed', $Context.SetupFeed) }
    if ($RequireSignature) { $arguments += '-RequireSignature' }
    return Invoke-Line $arguments
}
function Expect-Failure($Context, [string] $Expected, [string] $Message, [string[]] $From = @($Context.Files, $Context.Served), [switch] $RequireSignature) {
    $result = Invoke-Verify $Context $From -RequireSignature:$RequireSignature
    Check ($result.Ended -ne 0 -and $result.Said -match "FAIL: $Expected") "$Message (exit $($result.Ended): $($result.Said))"
}

try {
    $ready = $false
    for ($try = 0; $try -lt 50 -and -not $ready; ++$try) {
        try { $client = [Net.Sockets.TcpClient]::new('127.0.0.1', $port); $client.Dispose(); $ready = $true } catch { Start-Sleep -Milliseconds 100 }
    }
    Check $ready 'the stand-in host answers'

    # --- What was built, as published: in a folder and on a host, both in one -From, and the feed.
    $good = New-Published 'good'
    $result = Invoke-Verify $good @($good.Files, $good.Served) -RequireSignature
    Check ($result.Ended -eq 0 -and $result.Said -match 'What is published is what was built: qKVM Desktop 0\.1\.0, 2 place\(s\) and both feeds') "what was built passes ($($result.Said))"
    Check ($result.Said.Contains("--- $($good.Files)") -and $result.Said.Contains("--- $($good.Served)")) "each place is checked as itself ($($result.Said))"
    Check ($result.Said -match 'ok: the ZIP the feed names is the one built') 'the ZIP the feed names is fetched and checked'
    Check ($result.Said -match 'ok: the setup installed copies'' feed names is the one built') 'the setup installed copies'' feed names is fetched and checked'

    # --- Nothing to check is no pass.
    $result = Invoke-Verify $good @() -NoFeed
    Check ($result.Ended -ne 0 -and $result.Said -match 'no place') "a run with no place to check fails ($($result.Said))"
    $result = Invoke-Verify $good @()
    Check ($result.Ended -ne 0 -and $result.Said -match 'no place') "a run with a feed and no place fails ($($result.Said))"
    $result = Invoke-Verify $good @(',')
    Check ($result.Ended -ne 0 -and $result.Said -match 'empty place') "a -From with nothing in it fails ($($result.Said))"
    $result = Invoke-Verify $good @($good.Files, '')
    Check ($result.Ended -ne 0 -and $result.Said -match 'empty place') "a -From with an empty place in it fails ($($result.Said))"

    # --- Files, lists and signatures that are not what was written (in the folder and on the host alike).
    Expect-Failure (New-Published 'zip' { param($c) [IO.File]::AppendAllText((Join-Path $c.Files $zipName), 'x') }) "$([regex]::Escape($zipName)) is the one written" 'a changed ZIP fails'
    Expect-Failure (New-Published 'sums' { param($c) [IO.File]::AppendAllText((Join-Path $c.Files 'SHA256SUMS'), "0  extra`n") }) 'SHA256SUMS is the one written' 'a changed SHA256SUMS fails'
    Expect-Failure (New-Published 'missing' { param($c) Remove-Item (Join-Path $c.Files 'qKVM-Desktop-0.1.0-source.zip') }) 'qKVM-Desktop-0.1.0-source.zip is the one written' 'a missing file fails'
    Expect-Failure (New-Published 'extra' { param($c) [IO.File]::WriteAllText((Join-Path $c.Files 'qkvm-setup.exe'), 'x') }) 'the folder holds the release files and nothing else' 'a folder with another file fails'
    Expect-Failure (New-Published 'built' { param($c) $c.Record.package.sha256 = '0' * 64 }) 'the ZIP written is the ZIP built' 'a ZIP that is not the one built fails'
    # The setup, as published: the one written, and the one built.
    Expect-Failure (New-Published 'setup' { param($c) [IO.File]::AppendAllText((Join-Path $c.Files $setupName), 'x') }) "$([regex]::Escape($setupName)) is the one written" 'a changed setup fails'
    Expect-Failure (New-Published 'setup-missing' { param($c) Remove-Item (Join-Path $c.Files $setupName) }) "$([regex]::Escape($setupName)) is the one written" 'a missing setup fails'
    Expect-Failure (New-Published 'setup-built' { param($c) $c.Record.setup.sha256 = '0' * 64 }) 'the setup written is the setup built' 'a setup that is not the one built fails'
    Expect-Failure (New-Published 'setup-unrecorded' { param($c) $c.Record.Remove('setup') }) 'the setup written is the setup built' 'a run that recorded no setup fails'
    Expect-Failure (New-Published 'trial' { param($c) $c.Record.trial = $true }) 'the run is no trial' 'a trial fails'
    Expect-Failure (New-Published 'signature' { param($c) [IO.File]::WriteAllBytes((Join-Path $c.Files 'SHA256SUMS.sig'), (Sign ([IO.File]::ReadAllBytes((Join-Path $c.Files 'SHA256SUMS'))) $other)) }) 'SHA256SUMS.sig is signed by the key the program pins' 'a signature by another key fails'
    $unsigned = New-Published 'unsigned' { param($c) Remove-Item (Join-Path $c.Files 'SHA256SUMS.sig') }
    Expect-Failure $unsigned 'SHA256SUMS.sig is there' 'no signature fails where one is asked for' -RequireSignature
    Check ((Invoke-Verify $unsigned @($unsigned.Files, $unsigned.Served)).Ended -eq 0) 'no signature passes before the owner has signed'
    # (The second place of one -From is checked as well as the first.)
    $second = New-Published 'second' { param($c) New-Item -ItemType Directory -Path "$($c.Case)/mirror" | Out-Null
        foreach ($file in Get-ChildItem -LiteralPath $c.Files) { Copy-Item -LiteralPath $file.FullName -Destination "$($c.Case)/mirror" }
        [IO.File]::AppendAllText("$($c.Case)/mirror/$zipName", 'x') }
    Expect-Failure $second "$([regex]::Escape($zipName)) is the one written" 'a changed ZIP at the second place fails' -From @($second.Served, "$host_/second/mirror")

    # --- Feeds that are not this release's.
    Expect-Failure (New-Published 'feed-key' { param($c) $c.FeedKey = $other }) 'the feed is signed by the key the program pins' 'a feed signed by another key fails'
    Expect-Failure (New-Published 'feed-version' { param($c) $c.Payload.version = '0.1.1' }) 'the feed offers 0.1.0' 'a feed of another version fails'
    Expect-Failure (New-Published 'feed-sha' { param($c) $c.Payload.sha256 = '0' * 64 }) 'the feed names the ZIP built' 'a feed of another ZIP fails'
    Expect-Failure (New-Published 'feed-bytes' { param($c) $c.Payload.bytes = 1 }) 'the feed names the ZIP built' 'a feed of another size fails'
    Expect-Failure (New-Published 'feed-name' { param($c) $c.Payload.package = "$host_/feed-name/published/other.zip" }) 'the feed names' 'a feed naming another file fails'
    Expect-Failure (New-Published 'feed-http' { param($c) $c.Payload.package = "http://download.qkvm.com/desktop/0.1.0/$zipName" }) 'the feed names' 'a feed with an http address fails'
    Expect-Failure (New-Published 'feed-msix' { param($c) $c.Payload.platform = 'windows-x64-msix' }) 'the feed is of qKVM Desktop.s stable portable release' 'a feed of an MSIX fails'
    Expect-Failure (New-Published 'feed-old' { param($c) $c.Payload.published = '2026-01-01T00:00:00Z'; $c.Payload.expires = '2026-01-20T00:00:00Z' }) 'the feed is valid now' 'an expired feed fails'
    Expect-Failure (New-Published 'feed-future' { param($c) $c.Payload.published = [DateTime]::UtcNow.AddDays(2).ToString("yyyy-MM-dd'T'HH:mm:ss'Z'") }) 'the feed is valid now' 'a feed published later than now fails'
    Expect-Failure (New-Published 'feed-long' { param($c) $c.Payload.expires = [DateTime]::UtcNow.AddDays(40).ToString("yyyy-MM-dd'T'HH:mm:ss'Z'") }) 'the feed is valid for no more than 31 days' 'a feed valid for longer than a month fails'
    # The package the feed names is fetched and checked itself: where it is not the ZIP built, or not there, or on a
    # host that is not among the places checked (it is then not fetched at all).
    $elsewhere = New-Published 'feed-elsewhere' { param($c) New-Item -ItemType Directory -Path "$($c.Case)/other" | Out-Null
        # (Of the same size: what tells it apart is its SHA-256.)
        [IO.File]::WriteAllText("$($c.Case)/other/$zipName", "made-up $($zipName.ToUpperInvariant())")
        $c.Payload.package = "$host_/feed-elsewhere/other/$zipName" }
    Expect-Failure $elsewhere 'the ZIP the feed names is the one built' 'a feed naming another ZIP of the same name fails'
    Expect-Failure (New-Published 'feed-gone' { param($c) $c.Payload.package = "$host_/feed-gone/nothing-here/$zipName" }) 'the ZIP the feed names is the one built' 'a feed naming a ZIP that is not there fails'
    # (Another host, here this PC on a port nothing answers on: it is not asked.)
    $unused = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $unused.Start(); $unusedPort = $unused.LocalEndpoint.Port; $unused.Stop()
    $offPlace = New-Published 'feed-off-place' { param($c) $c.Payload.package = "http://127.0.0.1:$unusedPort/desktop/0.1.0/$zipName" }
    $result = Invoke-Verify $offPlace @($offPlace.Served)
    Check ($result.Ended -ne 0 -and $result.Said -match 'FAIL: the feed names .* on a host that is checked' -and $result.Said -notmatch 'the ZIP the feed names is the one built') "a feed naming a host that is not checked fails, and is not asked ($($result.Said))"

    # --- Installed copies' feed (stable-setup.json): signed by the key the program pins, of the setup platform, this
    # version, the setup built, current; and the setup at the address it names is fetched and is the one built.
    Expect-Failure (New-Published 'setup-feed-key' { param($c) $c.SetupFeedKey = $other }) 'installed copies'' feed is signed by the key the program pins' 'a setup feed signed by another key fails'
    Expect-Failure (New-Published 'setup-feed-version' { param($c) $c.SetupPayload.version = '0.1.1' }) 'installed copies'' feed offers 0.1.0' 'a setup feed of another version fails'
    Expect-Failure (New-Published 'setup-feed-sha' { param($c) $c.SetupPayload.sha256 = '0' * 64 }) 'installed copies'' feed names the setup built' 'a setup feed of another setup fails'
    Expect-Failure (New-Published 'setup-feed-bytes' { param($c) $c.SetupPayload.bytes = 1 }) 'installed copies'' feed names the setup built' 'a setup feed of another size fails'
    Expect-Failure (New-Published 'setup-feed-portable' { param($c) $c.SetupPayload.platform = 'windows-x64-portable' }) 'installed copies'' feed is of qKVM Desktop.s stable setup' 'a setup feed of the portable ZIP fails'
    Expect-Failure (New-Published 'setup-feed-name' { param($c) $c.SetupPayload.package = "$host_/setup-feed-name/published/$zipName" }) 'installed copies'' feed names .* at an https address' 'a setup feed naming another file fails'
    Expect-Failure (New-Published 'setup-feed-gone' { param($c) $c.SetupPayload.package = "$host_/setup-feed-gone/nothing-here/$setupName" }) 'the setup installed copies'' feed names is the one built' 'a setup feed naming a setup that is not there fails'
    Expect-Failure (New-Published 'setup-feed-old' { param($c) $c.SetupPayload.published = '2026-01-01T00:00:00Z'; $c.SetupPayload.expires = '2026-01-20T00:00:00Z' }) 'installed copies'' feed is valid now' 'an expired setup feed fails'
    Expect-Failure (New-Published 'setup-feed-long' { param($c) $c.SetupPayload.expires = [DateTime]::UtcNow.AddDays(40).ToString("yyyy-MM-dd'T'HH:mm:ss'Z'") }) 'installed copies'' feed is valid for no more than 31 days' 'a setup feed valid for longer than a month fails'
    $noSetupFeed = New-Published 'setup-feed-missing'
    Remove-Item -LiteralPath $noSetupFeed.SetupFeed
    Expect-Failure $noSetupFeed 'installed copies'' feed could be read' 'a setup feed that is not there fails'

    # --- A redirect, and a place with nothing.
    $moved = New-Published 'moved' { param($c) Remove-Item (Join-Path $c.Files 'SHA256SUMS'); New-Item -ItemType Directory -Path (Join-Path $c.Files 'SHA256SUMS') | Out-Null }
    $result = Invoke-Verify $moved @($moved.Served) -NoFeed
    Check ($result.Ended -ne 0 -and $result.Said -match 'a redirect') "a redirect is refused ($($result.Said))"
    $result = Invoke-Verify $good @("$host_/good/nothing-here") -NoFeed
    Check ($result.Ended -ne 0 -and $result.Said -match 'FAIL: SHA256SUMS is the one written') "a place with nothing fails ($($result.Said))"

    # --- The command release_commands.ps1 prints, run as printed: its places (GitHub's and the direct host's) and its
    # feed swapped for this PC's own, every other word as it is.
    $printed = New-Published 'printed' { param($c) New-Item -ItemType Directory -Path "$($c.Case)/mirror" | Out-Null
        foreach ($file in Get-ChildItem -LiteralPath $c.Files) { Copy-Item -LiteralPath $file.FullName -Destination "$($c.Case)/mirror" } }
    $realFeed = "$($printed.Case)/stable-as-published.json"
    Write-Feed $printed $realFeed @{ package = "https://download.qkvm.com/desktop/0.1.0/$zipName" }
    $realSetupFeed = "$($printed.Case)/stable-setup-as-published.json"
    Write-Feed $printed $realSetupFeed @{ package = "https://download.qkvm.com/desktop/0.1.0/$setupName" } -Setup
    $commands = Invoke-Line @('-File', "$PSScriptRoot/release_commands.ps1", '-Run', $printed.Run, '-Files', $printed.Files, '-Bucket', 'qkvm-downloads', '-Feed', $realFeed, '-SetupFeed', $realSetupFeed)
    $checks = @($commands.Said -split "`n" | Where-Object { $_ -like 'pwsh -File tools\verify_release.ps1 *' })
    $line = @($checks | Where-Object { $_ -like '* -Feed *' })
    $draftLine = @($checks | Where-Object { $_ -notlike '* -Feed *' })
    Check ($commands.Ended -eq 0 -and $checks.Count -eq 2 -and $line.Count -eq 1 -and $draftLine.Count -eq 1) "release_commands.ps1 prints the draft's check and the last one ($($commands.Said))"
    if ($draftLine.Count -eq 1) {
        # The draft's check, run as printed: its folder (made here as gh release download makes it, the release files
        # alone) and the direct host's place, swapped for this PC's own, in the one quoted word it is printed as.
        $draft = Join-Path (Split-Path -Parent (Resolve-Path -LiteralPath $printed.Files).Path) 'draft-v0.1.0'
        Check ($draftLine[0].Contains(" -From `"$draft,https://download.qkvm.com/desktop/0.1.0`" ") -and $draftLine[0].EndsWith(' -RequireSignature')) "the draft's check names its folder and the direct host in one word, and no feed ($($draftLine[0]))"
        New-Item -ItemType Directory -Path $draft | Out-Null
        foreach ($file in Get-ChildItem -LiteralPath $printed.Files) { Copy-Item -LiteralPath $file.FullName -Destination $draft }
        $asRun = $draftLine[0].Replace('https://download.qkvm.com/desktop/0.1.0', "$host_/printed/mirror")
        $words = @([regex]::Matches($asRun, '"[^"]*"|\S+') | ForEach-Object { $_.Value.Trim('"') })
        Check ($words[0] -ceq 'pwsh' -and $words[1] -ceq '-File') "the draft's check is run by pwsh -File ($asRun)"
        Push-Location (Split-Path -Parent $PSScriptRoot)
        try { $result = Invoke-Line @($words[1..($words.Count - 1)]) } finally { Pop-Location }
        Check ($result.Ended -eq 0 -and $result.Said -match '2 place\(s\)' -and $result.Said.Contains("--- $draft") -and
            $result.Said.Contains("--- $host_/printed/mirror")) "the draft's check as printed checks the draft's folder and the direct host ($($result.Said))"
        [IO.File]::AppendAllText("$draft/$zipName", 'x')
        Push-Location (Split-Path -Parent $PSScriptRoot)
        try { $result = Invoke-Line @($words[1..($words.Count - 1)]) } finally { Pop-Location }
        Check ($result.Ended -ne 0 -and $result.Said -match "FAIL: $([regex]::Escape($zipName)) is the one written") "the draft's check as printed fails on a file of the draft ($($result.Said))"
    }
    if ($line.Count -eq 1) {
        $swaps = [ordered]@{
            'https://github.com/qkvm/qkvm-desktop/releases/download/v0.1.0' = $printed.Served
            'https://download.qkvm.com/desktop/0.1.0' = "$host_/printed/mirror"
            'https://download.qkvm.com/desktop/stable.json' = "$host_/printed/stable.json"
            'https://download.qkvm.com/desktop/stable-setup.json' = "$host_/printed/stable-setup.json"
        }
        $asRun = $line[0]
        foreach ($swap in $swaps.GetEnumerator()) {
            Check (([regex]::Matches($asRun, [regex]::Escape("$($swap.Key)") + '(?=[ ,]|$)')).Count -eq 1) "the printed check names $($swap.Key) once ($asRun)"
            $asRun = [regex]::Replace($asRun, [regex]::Escape("$($swap.Key)") + '(?=[ ,]|$)', $swap.Value.Replace('$', '$$'))
        }
        $words = @([regex]::Matches($asRun, '"[^"]*"|\S+') | ForEach-Object { $_.Value.Trim('"') })
        Check ($words[0] -ceq 'pwsh' -and $words[1] -ceq '-File') "the check is run by pwsh -File ($asRun)"
        Push-Location (Split-Path -Parent $PSScriptRoot)
        try { $result = Invoke-Line @($words[1..($words.Count - 1)]) } finally { Pop-Location }
        Check ($result.Ended -eq 0 -and $result.Said -match '2 place\(s\) and both feeds' -and $result.Said.Contains("--- $($printed.Served)") -and
            $result.Said.Contains("--- $host_/printed/mirror")) "the check as printed checks both places and both feeds ($($result.Said))"
        [IO.File]::AppendAllText("$($printed.Case)/mirror/$zipName", 'x')
        Push-Location (Split-Path -Parent $PSScriptRoot)
        try { $result = Invoke-Line @($words[1..($words.Count - 1)]) } finally { Pop-Location }
        Check ($result.Ended -ne 0 -and $result.Said -match "FAIL: $([regex]::Escape($zipName)) is the one written") "the check as printed fails on its second place ($($result.Said))"
    }
} finally {
    Stop-Process -Id $served.Id -Force -ErrorAction SilentlyContinue
    $served.WaitForExit(5000) | Out-Null
}
$signer.Dispose(); $other.Dispose()

if ($failures) { Write-Host "$failures verify-release check(s) failed."; exit 1 }
Remove-Item -LiteralPath $work -Recurse -Force
Write-Output 'Published-release checks passed: what was built passes, from a folder and from a host, as release_commands.ps1 prints the check; every other file, list, signature, feed, package and redirect fails, and so does nothing to check.'
