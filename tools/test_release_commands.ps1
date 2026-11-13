#Requires -Version 7.2
<#
tools/release_commands.ps1 on made-up release files, signed with a throwaway key held in memory alone: the commands
it says (it runs none) once it has checked SHA256SUMS, its signature and the feed against the run, and what it
refuses, saying no upload at all. Every failed check is a "FAIL:" line, and the script then fails.
#>
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\build_common.ps1"

$failures = 0
function Check([bool] $Condition, [string] $Message) {
    if (-not $Condition) { Write-Host "FAIL: $Message"; $script:failures++ }
}
$work = Join-Path (Split-Path -Parent $PSScriptRoot) "build/release-tests/$([guid]::NewGuid().ToString('N'))"
$signer = [Security.Cryptography.ECDsa]::Create([Security.Cryptography.ECCurve+NamedCurves]::nistP256)
$other = [Security.Cryptography.ECDsa]::Create([Security.Cryptography.ECCurve+NamedCurves]::nistP256)
$q = $signer.ExportParameters($false).Q
$publicKey = ([Convert]::ToHexString($q.X) + [Convert]::ToHexString($q.Y)).ToLowerInvariant()
function Sign([byte[]] $Data, $Key = $signer) {
    return $Key.SignData($Data, [Security.Cryptography.HashAlgorithmName]::SHA256,
        [Security.Cryptography.DSASignatureFormat]::IeeeP1363FixedFieldConcatenation)
}
$zipName = 'qKVM-Desktop-0.1.0-windows-x64.zip'
$setupName = 'qKVM-Desktop-0.1.0-windows-x64-setup.exe'

function New-Release([string] $Name, [scriptblock] $Change = {}, [switch] $Trial, [switch] $Unsigned, [string[]] $Extra = @()) {
    $run = (New-Item -ItemType Directory -Force -Path "$work/$Name/run").FullName
    $files = (New-Item -ItemType Directory -Force -Path "$work/$Name/files").FullName
    foreach ($file in (Get-ReleaseFileNames '0.1.0' | Where-Object { $_ -cne 'SHA256SUMS' }) + $Extra) {
        [IO.File]::WriteAllText((Join-Path $files $file), "made-up $file")
    }
    $sums = ((Get-ReleaseFileNames '0.1.0' | Where-Object { $_ -cne 'SHA256SUMS' } | Sort-Object -CaseSensitive |
        ForEach-Object { "$(Get-Sha256 (Join-Path $files $_))  $_`n" }) -join '')
    [IO.File]::WriteAllBytes("$files/SHA256SUMS", [Text.Encoding]::ASCII.GetBytes($sums))
    if (-not $Unsigned) { [IO.File]::WriteAllBytes("$files/SHA256SUMS.sig", (Sign ([IO.File]::ReadAllBytes("$files/SHA256SUMS")))) }
    $zip = Join-Path $files $zipName
    $now = [DateTime]::UtcNow
    $context = [pscustomobject]@{
        Run = $run; Files = $files; Feed = "$work/$Name/stable.json"; FeedKey = $signer
        SetupFeed = "$work/$Name/stable-setup.json"; SetupFeedKey = $signer
        Record = [ordered]@{ schema = 1; version = '0.1.0'; trial = [bool] $Trial; updates = [ordered]@{ publicKey = $publicKey }
            package = [ordered]@{ archive = $zipName; bytes = (Get-Item $zip).Length; sha256 = Get-Sha256 $zip }
            setup = [ordered]@{ name = $setupName; bytes = (Get-Item "$files/$setupName").Length; sha256 = Get-Sha256 "$files/$setupName" } }
        Payload = [ordered]@{ schema = 1; product = 'qkvm-desktop'; platform = 'windows-x64-portable'; channel = 'stable'
            version = '0.1.0'; published = $now.AddHours(-1).ToString("yyyy-MM-dd'T'HH:mm:ss'Z'")
            expires = $now.AddDays(29).ToString("yyyy-MM-dd'T'HH:mm:ss'Z'")
            package = "https://download.qkvm.com/desktop/0.1.0/$zipName"; bytes = (Get-Item $zip).Length; sha256 = Get-Sha256 $zip
            notes = 'made-up notes' }
        # (Installed copies' own feed, as prepare_update_payload.ps1 -Setup and assemble_update_feed.ps1 make it.)
        SetupPayload = [ordered]@{ schema = 1; product = 'qkvm-desktop'; platform = 'windows-x64-setup'; channel = 'stable'
            version = '0.1.0'; published = $now.AddHours(-1).ToString("yyyy-MM-dd'T'HH:mm:ss'Z'")
            expires = $now.AddDays(29).ToString("yyyy-MM-dd'T'HH:mm:ss'Z'")
            package = "https://download.qkvm.com/desktop/0.1.0/$setupName"; bytes = (Get-Item "$files/$setupName").Length; sha256 = Get-Sha256 "$files/$setupName"
            notes = 'made-up notes' }
    }
    & $Change $context
    $context.Record | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$run/build-record.json" -Encoding utf8
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes(($context.Payload | ConvertTo-Json -Compress))
    [ordered]@{ payload = [Convert]::ToBase64String($bytes); signature = [Convert]::ToBase64String((Sign $bytes $context.FeedKey)) } |
        ConvertTo-Json -Compress | Set-Content -LiteralPath $context.Feed -Encoding utf8 -NoNewline
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes(($context.SetupPayload | ConvertTo-Json -Compress))
    [ordered]@{ payload = [Convert]::ToBase64String($bytes); signature = [Convert]::ToBase64String((Sign $bytes $context.SetupFeedKey)) } |
        ConvertTo-Json -Compress | Set-Content -LiteralPath $context.SetupFeed -Encoding utf8 -NoNewline
    return $context
}
function Invoke-Commands($Release, [string] $Bucket = 'qkvm-downloads', [switch] $NoFeed, [switch] $NoSetupFeed) {
    $arguments = @('-Run', $Release.Run, '-Files', $Release.Files, '-Bucket', $Bucket)
    if (-not $NoFeed) { $arguments += @('-Feed', $Release.Feed) }
    if (-not $NoSetupFeed) { $arguments += @('-SetupFeed', $Release.SetupFeed) }
    $said = & (Get-Process -Id $PID).Path -NoProfile -NonInteractive -File "$PSScriptRoot/release_commands.ps1" @arguments 2>&1
    return [pscustomobject]@{ Ended = $LASTEXITCODE; Lines = @($said | ForEach-Object { "$_" }) }
}
function Expect-Refusal($Release, [string] $Expected, [string] $Message, [switch] $NoFeed, [switch] $NoSetupFeed) {
    $result = Invoke-Commands $Release -NoFeed:$NoFeed -NoSetupFeed:$NoSetupFeed
    $text = $result.Lines -join ' | '
    Check ($result.Ended -ne 0 -and $text -match $Expected) "$Message ($text)"
    # (Nothing to run is said before what it is about is checked: not one upload, not the release.)
    Check (-not ($result.Lines -match 'r2 object put|gh release (create|download|edit)')) "$Message, and no upload or release is said ($text)"
}

$good = New-Release 'good'
$result = Invoke-Commands $good
Check ($result.Ended -eq 0) "a signed release's commands are said ($($result.Lines -join ' | '))"
$puts = @($result.Lines | Where-Object { $_ -match '^npx --yes wrangler@4\.152\.0 r2 object put ' })
Check ($puts.Count -eq 8) "one upload for each of the six files and the two feeds ($($puts.Count))"
foreach ($name in Get-ReleaseFileNames '0.1.0' -Signed) {
    Check (@($puts | Where-Object { $_.Contains("`"qkvm-downloads/desktop/0.1.0/$name`"") -and $_.Contains('immutable') -and $_.EndsWith('--remote') }).Count -eq 1) "$name goes to desktop/0.1.0/, kept for good"
}
Check (@($puts | Where-Object { $_.Contains('"qkvm-downloads/desktop/stable.json"') -and $_.Contains('--cache-control "no-cache"') }).Count -eq 1) 'the feed goes to desktop/stable.json, never cached'
Check (@($puts | Where-Object { $_.Contains('"qkvm-downloads/desktop/stable-setup.json"') -and $_.Contains("--file ""$((Resolve-Path -LiteralPath $good.SetupFeed).Path)""") -and $_.Contains('--content-type "application/json"') -and $_.Contains('--cache-control "no-cache"') }).Count -eq 1) 'installed copies'' feed goes to desktop/stable-setup.json, never cached'
$checked = [Array]::FindIndex([string[]] $result.Lines, [Predicate[string]] { param($l) $l -like '# Checked here: *' })
Check ($checked -ge 0 -and $checked -lt [Array]::IndexOf([string[]] $result.Lines, $puts[0])) 'what was checked is said before the first upload'
$release = @($result.Lines | Where-Object { $_ -match '^gh release create v0\.1\.0 --repo qkvm/qkvm-desktop --verify-tag --draft --prerelease ' })
Check ($release.Count -eq 1) "the GitHub release is a draft pre-release of v0.1.0, of the tag that is there ($($result.Lines -join ' | '))"
Check ($result.Lines -ccontains '$env:GH_TOKEN = gh auth token --user pypv') 'it is made as pypv'
Check (@($result.Lines | Where-Object { $_ -match '^pwsh -File tools\\verify_release\.ps1 .*-Feed https://download\.qkvm\.com/desktop/stable\.json -SetupFeed https://download\.qkvm\.com/desktop/stable-setup\.json -RequireSignature$' }).Count -eq 1) 'what is published is checked afterwards, both feeds with it'
# The order (the dress rehearsal of 2026-11-09): the files, then the draft; the draft as GitHub holds it, downloaded
# as pypv (a draft's files are not at its public address), checked with the direct host's before anything is
# published; then the release published; and only then the feeds, so that no copy of the program is told of a
# release whose page is still a draft; the last check from the public places, with both feeds.
$lines = [string[]] $result.Lines
$at = { param([string] $pattern) [Array]::FindIndex($lines, [Predicate[string]] { param($l) $l -match $pattern }) }
$draft = Join-Path (Split-Path -Parent (Resolve-Path -LiteralPath $good.Files).Path) 'draft-v0.1.0'
$lastFile = [Array]::FindLastIndex($lines, [Predicate[string]] { param($l) $l -match '^npx .* r2 object put "qkvm-downloads/desktop/0\.1\.0/' })
$created = & $at '^gh release create v0\.1\.0 '
$downloaded = & $at ('^gh release download v0\.1\.0 --repo qkvm/qkvm-desktop --dir "' + [regex]::Escape($draft) + '"$')
$draftChecked = & $at ('^pwsh -File tools\\verify_release\.ps1 -Run ".*" -From "' + [regex]::Escape($draft) + ',https://download\.qkvm\.com/desktop/0\.1\.0" -RequireSignature$')
$published = & $at '^gh release edit v0\.1\.0 --repo qkvm/qkvm-desktop --verify-tag --tag v0\.1\.0 --draft=false$'
$firstFeed = & $at '^npx .* r2 object put "qkvm-downloads/desktop/stable(-setup)?\.json"'
$lastCheck = & $at '^pwsh -File tools\\verify_release\.ps1 .*-Feed '
Check ($lastFile -ge 0 -and $created -gt $lastFile) "the draft is made once every file is on the direct host ($lastFile, $created)"
Check ($downloaded -gt $created -and $draftChecked -gt $downloaded) "the draft is downloaded into a folder of its own and checked with the direct host's files ($created, $downloaded, $draftChecked)"
Check (@($lines | Where-Object { $_ -match '^gh release download ' }).Count -eq 1 -and $lines[$downloaded - 1] -ceq '$env:GH_TOKEN = gh auth token --user pypv') 'the draft is downloaded as pypv: a draft is not public'
Check ($published -gt $draftChecked) "the release is published only after its draft was checked ($draftChecked, $published)"
Check ($firstFeed -gt $published -and $lastCheck -gt $firstFeed) "the feeds go up only after the release is published, and the last check after them ($published, $firstFeed, $lastCheck)"
Check (@($lines | Where-Object { $_ -match '^gh release edit ' }).Count -eq 1) 'the release is published once'
New-Item -ItemType Directory -Path $draft | Out-Null
Expect-Refusal $good 'draft-v0\.1\.0' 'a folder for the draft that is there already is refused: it must hold the draft''s files alone'
Remove-Item -LiteralPath $draft -Recurse -Force
# (The draft's check names its folder and the direct host in one word, split at its commas: a comma in the folder's
# path would make another place of it.)
Expect-Refusal (New-Release 'good,comma') 'comma or a quote' 'release files in a folder whose path has a comma are refused: the draft''s check could not name it'
Check (-not ($result.Lines -match '\.pdb"|latest\.json')) 'no symbols or latest.json are in any command'
# (Every program a command names is the setup: as many ".exe" as names of it.)
$programs = @($result.Lines | Where-Object { [regex]::Matches($_, '\.exe"').Count -ne [regex]::Matches($_, [regex]::Escape("$setupName`"")).Count })
Check ($programs.Count -eq 0) "no program but the setup is in any command ($($programs -join ' | '))"
Check (@($puts | Where-Object { $_.Contains("/$setupName`"") -and $_.Contains('--content-type "application/vnd.microsoft.portable-executable"') }).Count -eq 1) 'the setup goes up as a Windows program'
Check (@($release | Where-Object { $_.Contains("$setupName`"") }).Count -eq 1) 'the setup is a file of the GitHub release'

# --- What is refused, before anything is said.
Expect-Refusal $good 'stable.json first' 'a release without its feed is refused' -NoFeed
Expect-Refusal (New-Release 'trial' -Trial) 'is a trial' 'a trial is refused'
Expect-Refusal (New-Release 'unsigned' -Unsigned) 'not signed yet' 'an unsigned release is refused'
Expect-Refusal (New-Release 'extra' -Extra @('qkvm-setup.exe')) 'Never a release file: qkvm-setup.exe' 'a folder with another file is refused'
$result = Invoke-Commands $good -Bucket 'Not A Bucket'
Check ($result.Ended -ne 0 -and -not ($result.Lines -match 'r2 object put')) 'a bucket name that is none is refused'
# SHA256SUMS, its signature and the files it names.
Expect-Refusal (New-Release 'sums-key' { param($c) [IO.File]::WriteAllBytes("$($c.Files)/SHA256SUMS.sig", (Sign ([IO.File]::ReadAllBytes("$($c.Files)/SHA256SUMS")) $other)) }) 'SHA256SUMS\.sig is not a signature of SHA256SUMS by the key the program pins' 'a SHA256SUMS signed by another key is refused'
Expect-Refusal (New-Release 'changed-file' { param($c) [IO.File]::AppendAllText("$($c.Files)/qKVM-Desktop-0.1.0-source.zip", 'x') }) 'qKVM-Desktop-0.1.0-source\.zip is not the file SHA256SUMS names' 'a file changed after SHA256SUMS is refused'
Expect-Refusal (New-Release 'zip-not-built' { param($c) $c.Record.package.sha256 = '0' * 64 }) 'not the ZIP the run built' 'a ZIP that is not the one built is refused'
Expect-Refusal (New-Release 'setup-not-built' { param($c) $c.Record.setup.sha256 = '0' * 64 }) 'not the setup the run built' 'a setup that is not the one built is refused'
Expect-Refusal (New-Release 'setup-unrecorded' { param($c) $c.Record.Remove('setup') }) 'made no setup' 'a run that recorded no setup is refused'
# The feed: signed by the key the program pins, current, and naming this ZIP where it is uploaded.
Expect-Refusal (New-Release 'feed-key' { param($c) $c.FeedKey = $other }) 'not signed by the key the program pins' 'a feed signed by another key is refused'
Expect-Refusal (New-Release 'feed-version' { param($c) $c.Payload.version = '0.1.1' }) 'feed offers 0\.1\.1' 'a feed of another version is refused'
Expect-Refusal (New-Release 'feed-sha' { param($c) $c.Payload.sha256 = '0' * 64 }) 'not this ZIP' 'a feed of another ZIP is refused'
Expect-Refusal (New-Release 'feed-bytes' { param($c) $c.Payload.bytes = 1 }) 'not this ZIP' 'a feed of another size is refused'
Expect-Refusal (New-Release 'feed-address' { param($c) $c.Payload.package = "https://github.com/qkvm/qkvm-desktop/releases/download/v0.1.0/$zipName" }) 'where it is uploaded' 'a feed naming another address is refused'
Expect-Refusal (New-Release 'feed-old' { param($c) $c.Payload.published = '2026-01-01T00:00:00Z'; $c.Payload.expires = '2026-01-20T00:00:00Z' }) 'not current' 'an expired feed is refused'
Expect-Refusal (New-Release 'feed-msix' { param($c) $c.Payload.platform = 'windows-x64-msix'; $c.Payload.package = "https://download.qkvm.com/desktop/0.1.0/qKVM.msix" }) 'portable' 'a feed of an MSIX is refused'
# Installed copies' feed: signed by the key the program pins, current, and naming this setup where it is uploaded.
Expect-Refusal $good 'stable-setup\.json too' 'a release without installed copies'' feed is refused' -NoSetupFeed
Expect-Refusal (New-Release 'setup-feed-key' { param($c) $c.SetupFeedKey = $other }) 'stable-setup\.json will not do: .*not signed by the key the program pins' 'a setup feed signed by another key is refused'
Expect-Refusal (New-Release 'setup-feed-version' { param($c) $c.SetupPayload.version = '0.1.1' }) 'stable-setup\.json will not do: it offers 0\.1\.1' 'a setup feed of another version is refused'
Expect-Refusal (New-Release 'setup-feed-sha' { param($c) $c.SetupPayload.sha256 = '0' * 64 }) 'stable-setup\.json will not do: it names other bytes' 'a setup feed of another setup is refused'
Expect-Refusal (New-Release 'setup-feed-bytes' { param($c) $c.SetupPayload.bytes = 1 }) 'stable-setup\.json will not do: it names other bytes' 'a setup feed of another size is refused'
Expect-Refusal (New-Release 'setup-feed-address' { param($c) $c.SetupPayload.package = "https://github.com/qkvm/qkvm-desktop/releases/download/v0.1.0/$setupName" }) 'stable-setup\.json names .* not where it is uploaded' 'a setup feed naming another address is refused'
Expect-Refusal (New-Release 'setup-feed-old' { param($c) $c.SetupPayload.published = '2026-01-01T00:00:00Z'; $c.SetupPayload.expires = '2026-01-20T00:00:00Z' }) 'stable-setup\.json will not do: it is not current' 'an expired setup feed is refused'
Expect-Refusal (New-Release 'setup-feed-long' { param($c) $c.SetupPayload.expires = [DateTime]::UtcNow.AddDays(40).ToString("yyyy-MM-dd'T'HH:mm:ss'Z'") }) 'stable-setup\.json will not do: it expires' 'a setup feed valid for longer than 31 days is refused'
Expect-Refusal (New-Release 'setup-feed-portable' { param($c) $c.SetupPayload.platform = 'windows-x64-portable' }) 'stable-setup\.json will not do: it is not of windows-x64-setup' 'a setup feed of the portable ZIP is refused'
Expect-Refusal (New-Release 'setup-feed-field' { param($c) $c.SetupPayload['extra'] = 'x' }) 'stable-setup\.json will not do: its fields' 'a setup feed with a field more is refused'

$signer.Dispose(); $other.Dispose()
if ($failures) { Write-Host "$failures release commands check(s) failed."; exit 1 }
Remove-Item -LiteralPath $work -Recurse -Force
Write-Output 'Release commands checks passed: the uploads, the draft release of its tag and the check are said for a signed release once SHA256SUMS, its signature and both feeds are checked against the run, and nothing for anything else.'
