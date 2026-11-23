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
$armZipName = 'qKVM-Desktop-0.1.0-windows-arm64.zip'
$setupName = 'qKVM-Desktop-0.1.0-windows-setup.exe'
function Write-Feed($Payload, $Key, [string] $File) {
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes(($Payload | ConvertTo-Json -Compress))
    [ordered]@{ payload = [Convert]::ToBase64String($bytes); signature = [Convert]::ToBase64String((Sign $bytes $Key)) } |
        ConvertTo-Json -Compress | Set-Content -LiteralPath $File -Encoding utf8 -NoNewline
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$exportCommit = 'e' * 40
$exportTree = 'f' * 40
# The FFmpeg this PC built (build_release.ps1 -Part ffmpeg) and keeps the records of: made-up DLLs of each kind, by text.
function Copy-Dlls([hashtable] $Dlls) {
    $copy = @{}
    foreach ($kind in $Dlls.Keys) { $copy[$kind] = [ordered]@{}; foreach ($name in $Dlls[$kind].Keys) { $copy[$kind][$name] = $Dlls[$kind][$name] } }
    return $copy
}
$genuine = @{ x64 = [ordered]@{ 'avcodec-63.dll' = 'made-up x64 avcodec'; 'avutil-61.dll' = 'made-up x64 avutil' }
    arm64 = [ordered]@{ 'avcodec-63.dll' = 'made-up ARM64 avcodec'; 'avutil-61.dll' = 'made-up ARM64 avutil' } }
function Get-TextFile([string] $Path, [string] $Text) {
    $bytes = [Text.Encoding]::UTF8.GetBytes($Text)
    return [ordered]@{ path = $Path; bytes = $bytes.Length; sha256 = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant() }
}
# A record as build_release.ps1 -Part ffmpeg keeps it (New-FFmpegRecord's shape): one build of each kind, its pins and
# their digest, each DLL and every file by size and SHA-256.
function New-KeptRecord([string] $BuiltAt, [hashtable] $Dlls, [string[]] $Reproduces = @(), [string] $Commit = $exportCommit,
    [string] $Tree = $exportTree, [string] $Toolchain = 'made-up-compiler_1.0_amd64.deb', [switch] $SourcesOf012) {
    # (-SourcesOf012: x64's build of the sources and toolchain 0.1.2's release built its FFmpeg from, as build_common.ps1's
    # $FFmpegPublished names them.)
    $sources = @((Get-TextFile 'ffmpeg-9.0.2.tar.xz' 'made-up ffmpeg source'), (Get-TextFile 'zlib-1.3.2.tar.gz' 'made-up zlib source'))
    $builds = foreach ($kind in 'x64', 'arm64') {
        $pins = [ordered]@{ ffmpeg = "$($sources[0].path) $($sources[0].bytes) $($sources[0].sha256)"; zlib = "$($sources[1].path) $($sources[1].bytes) $($sources[1].sha256)"
            toolchain = @("$kind-$Toolchain 1234 $('a' * 64)"); scripts = @("build_ffmpeg.ps1 $('b' * 64)", "build_ffmpeg.sh $('c' * 64)") }
        if ($SourcesOf012 -and $kind -ceq 'x64') { $pins.ffmpeg = $FFmpegPublished.ffmpeg; $pins.zlib = $FFmpegPublished.zlib; $pins.toolchain = @($FFmpegPublished.toolchain) }
        $dllFiles = @($Dlls[$kind].Keys | ForEach-Object { Get-TextFile "bin/$_" $Dlls[$kind][$_] })
        [ordered]@{ architecture = $kind; pins = $pins; pinsDigest = Get-FFmpegPinsDigest $pins $kind; configuration = '--prefix=/ffmpeg'
            dlls = @($dllFiles | ForEach-Object { [ordered]@{ name = $_.path; bytes = $_.bytes; sha256 = $_.sha256 } })
            files = @(@($dllFiles) + (Get-TextFile 'build.json' "made-up build.json of $kind") | Sort-Object { $_.path } -CaseSensitive) }
    }
    return [ordered]@{ schema = 1; product = 'qKVM Desktop'; part = 'ffmpeg'; version = '0.1.0'; builtAt = $BuiltAt
        source = [ordered]@{ commit = $Commit; tree = $Tree }; wsl = 'made-up'
        sources = @($sources | ForEach-Object { [ordered]@{ archive = $_.path; bytes = $_.bytes; sha256 = $_.sha256 } }); builds = @($builds); reproduces = @($Reproduces) }
}
function Write-KeptRecord($Record, [string] $File) { $Record | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $File -Encoding utf8 }
# A release ZIP as package.ps1 makes it: one folder, the program, FFmpeg's DLLs ($Dlls) and the package manifest, which
# names $Listed's (the same unless said). Its manifest's SHA-256 comes back.
function New-ReleaseZip([string] $Zip, [string] $Kind, $Dlls, $Listed) {
    $top = "qKVM-Desktop-0.1.0-windows-$Kind"
    $folder = Join-Path $work "zip-$([guid]::NewGuid().ToString('N'))/$top"
    New-Item -ItemType Directory -Force -Path $folder | Out-Null
    $inside = [ordered]@{ 'qkvm-desktop.exe' = "made-up $Kind program" }
    foreach ($name in $Dlls.Keys) { $inside[$name] = $Dlls[$name] }
    foreach ($name in $inside.Keys) { [IO.File]::WriteAllText((Join-Path $folder $name), $inside[$name]) }
    $named = [ordered]@{ 'qkvm-desktop.exe' = "made-up $Kind program" }
    foreach ($name in $Listed.Keys) { $named[$name] = $Listed[$name] }
    $manifest = [ordered]@{ product = 'qKVM'; version = '0.1.0'; architecture = $Kind; files = @($named.Keys | ForEach-Object { Get-TextFile $_ $named[$_] }) }
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $folder 'package-manifest.json') -Encoding utf8
    [IO.Compression.ZipFile]::CreateFromDirectory($folder, $Zip, [IO.Compression.CompressionLevel]::Optimal, $true)
    return Get-Sha256 (Join-Path $folder 'package-manifest.json')
}

# The four files of the ARM64 start checks in the run's export (tools/test_package_archive.ps1 and what it runs), by
# made-up SHA-256s: the run's build record names them, and so does the record of the checks made with them.
$startTools = [ordered]@{ 'tools/test_package_archive.ps1' = '5' * 64; 'tools/test_package.ps1' = '6' * 64; 'tools/build_common.ps1' = '7' * 64
    'tools/image-protections.json' = '8' * 64 }
function Copy-Tools($Tools) { $copy = [ordered]@{}; foreach ($name in $Tools.Keys) { $copy[$name] = $Tools[$name] }; return $copy }
function New-Release([string] $Name, [scriptblock] $Change = {}, [switch] $Trial, [switch] $Unsigned, [string[]] $Extra = @(), [scriptblock] $FFmpeg = {},
    [scriptblock] $Started = {}) {
    $run = (New-Item -ItemType Directory -Force -Path "$work/$Name/run").FullName
    $files = (New-Item -ItemType Directory -Force -Path "$work/$Name/files").FullName
    # The FFmpeg part, built twice of the same pins on this PC and both records kept; the run took the second, which
    # reproduced the first. $FFmpeg changes what is kept, what the run names and what the ZIPs hold.
    $ff = [pscustomobject]@{
        Kept = [ordered]@{ 'ffmpeg-0.1.0-first.json' = (New-KeptRecord '2026-11-20T10:00:00.0000000Z' $genuine)
            'ffmpeg-0.1.0-second.json' = (New-KeptRecord '2026-11-20T11:00:00.0000000Z' $genuine @('ffmpeg-0.1.0-first.json (x64)', 'ffmpeg-0.1.0-first.json (arm64)')) }
        Named = $null; ZipDlls = (Copy-Dlls $genuine); ListedDlls = $null; Records = "$work/$Name/records"; RunCopy = $null; Embedded = $null }
    $ff.Named = $ff.Kept['ffmpeg-0.1.0-second.json']
    & $FFmpeg $ff
    New-Item -ItemType Directory -Force -Path $ff.Records | Out-Null
    foreach ($kept in $ff.Kept.Keys) { Write-KeptRecord $ff.Kept[$kept] (Join-Path $ff.Records $kept) }
    Write-KeptRecord $ff.Named "$run/$FFmpegRecordName"
    $namedSha256 = Get-Sha256 "$run/$FFmpegRecordName"
    if ($ff.RunCopy) { Write-KeptRecord $ff.RunCopy "$run/$FFmpegRecordName" }
    $listed = if ($ff.ListedDlls) { $ff.ListedDlls } else { $ff.ZipDlls }
    $manifests = [ordered]@{}
    foreach ($kind in 'x64', 'arm64') {
        $manifests[$kind] = New-ReleaseZip (Join-Path $files "qKVM-Desktop-0.1.0-windows-$kind.zip") $kind $ff.ZipDlls[$kind] $listed[$kind]
    }
    foreach ($file in (Get-ReleaseFileNames '0.1.0' | Where-Object { $_ -cne 'SHA256SUMS' -and $_ -notlike '*-windows-*.zip' }) + $Extra) {
        [IO.File]::WriteAllText((Join-Path $files $file), "made-up $file")
    }
    $sums = ((Get-ReleaseFileNames '0.1.0' | Where-Object { $_ -cne 'SHA256SUMS' } | Sort-Object -CaseSensitive |
        ForEach-Object { "$(Get-Sha256 (Join-Path $files $_))  $_`n" }) -join '')
    [IO.File]::WriteAllBytes("$files/SHA256SUMS", [Text.Encoding]::ASCII.GetBytes($sums))
    if (-not $Unsigned) { [IO.File]::WriteAllBytes("$files/SHA256SUMS.sig", (Sign ([IO.File]::ReadAllBytes("$files/SHA256SUMS")))) }
    $zip = Join-Path $files $zipName
    $armZip = Join-Path $files $armZipName
    $now = [DateTime]::UtcNow
    $context = [pscustomobject]@{
        Run = $run; Files = $files; Feed = "$work/$Name/stable.json"; FeedKey = $signer
        SetupFeed = "$work/$Name/stable-setup.json"; SetupFeedKey = $signer
        ArmFeed = "$work/$Name/stable-arm64.json"; ArmFeedKey = $signer
        ArmSetupFeed = "$work/$Name/stable-arm64-setup.json"; ArmSetupFeedKey = $signer
        Records = $ff.Records
        Record = [ordered]@{ schema = 2; version = '0.1.0'; trial = [bool] $Trial; updates = [ordered]@{ publicKey = $publicKey }
            source = [ordered]@{ commit = $exportCommit; tree = $exportTree }
            dependencies = [ordered]@{ ffmpeg = [ordered]@{ record = $(if ($ff.Embedded) { $ff.Embedded } else { $ff.Named }); recordSha256 = $namedSha256 } }
            packages = @(
                [ordered]@{ architecture = 'x64'; archive = $zipName; bytes = (Get-Item $zip).Length; sha256 = Get-Sha256 $zip; manifestSha256 = $manifests['x64'] }
                [ordered]@{ architecture = 'arm64'; archive = $armZipName; bytes = (Get-Item $armZip).Length; sha256 = Get-Sha256 $armZip; manifestSha256 = $manifests['arm64'] })
            setup = [ordered]@{ name = $setupName; bytes = (Get-Item "$files/$setupName").Length; sha256 = Get-Sha256 "$files/$setupName"
                # (build_setup.ps1's record of the packages it was made of, by their manifests.)
                made = [ordered]@{ packages = @([ordered]@{ architecture = 'x64'; manifestSha256 = $manifests['x64'] }, [ordered]@{ architecture = 'arm64'; manifestSha256 = $manifests['arm64'] }) } }
            # (The export's tools of the ARM64 start checks, by SHA-256, as build_release.ps1 records them from its tree.)
            checks = [ordered]@{ arm64StartupTools = Copy-Tools $startTools } }
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
        # (Windows on Arm's two, beside them: its portable copies' of the ARM64 ZIP, its installed copies' of the setup.)
        ArmPayload = [ordered]@{ schema = 1; product = 'qkvm-desktop'; platform = 'windows-arm64-portable'; channel = 'stable'
            version = '0.1.0'; published = $now.AddHours(-1).ToString("yyyy-MM-dd'T'HH:mm:ss'Z'")
            expires = $now.AddDays(29).ToString("yyyy-MM-dd'T'HH:mm:ss'Z'")
            package = "https://download.qkvm.com/desktop/0.1.0/$armZipName"; bytes = (Get-Item $armZip).Length; sha256 = Get-Sha256 $armZip
            notes = 'made-up notes' }
        ArmSetupPayload = [ordered]@{ schema = 1; product = 'qkvm-desktop'; platform = 'windows-arm64-setup'; channel = 'stable'
            version = '0.1.0'; published = $now.AddHours(-1).ToString("yyyy-MM-dd'T'HH:mm:ss'Z'")
            expires = $now.AddDays(29).ToString("yyyy-MM-dd'T'HH:mm:ss'Z'")
            package = "https://download.qkvm.com/desktop/0.1.0/$setupName"; bytes = (Get-Item "$files/$setupName").Length; sha256 = Get-Sha256 "$files/$setupName"
            notes = 'made-up notes' }
    }
    & $Change $context
    $context.Record | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath "$run/build-record.json" -Encoding utf8
    # (The record of the ARM64 ZIP's start checks, made on Windows on Arm and kept with the run by release_files.ps1;
    # $Started changes it, and one given 'drop' is not there.)
    $evidence = [ordered]@{ schema = 1; product = 'qKVM Desktop'; check = 'tools/test_package_archive.ps1'; tools = Copy-Tools $startTools; passed = $true; archive = $armZipName
        bytes = (Get-Item $armZip).Length; sha256 = Get-Sha256 $armZip; architecture = 'arm64'; version = '0.1.0'
        windows = [ordered]@{ machine = 'arm64'; process = 'arm64'; caption = 'Microsoft Windows 11 Pro'; version = '10.0.26100' }; at = '2026-11-22T10:00:00.0000000Z' }
    $null = & $Started $evidence
    if (-not $evidence.Contains('drop')) { $evidence | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $run $Arm64StartupName) -Encoding utf8 }
    Write-Feed $context.Payload $context.FeedKey $context.Feed
    Write-Feed $context.SetupPayload $context.SetupFeedKey $context.SetupFeed
    Write-Feed $context.ArmPayload $context.ArmFeedKey $context.ArmFeed
    Write-Feed $context.ArmSetupPayload $context.ArmSetupFeedKey $context.ArmSetupFeed
    return $context
}
function Invoke-Commands($Release, [string] $Bucket = 'qkvm-downloads', [switch] $NoFeed, [switch] $NoSetupFeed, [switch] $NoArmFeed, [switch] $NoArmSetupFeed, [switch] $NoRecords) {
    $arguments = @('-Run', $Release.Run, '-Files', $Release.Files, '-Bucket', $Bucket)
    if (-not $NoRecords) { $arguments += @('-Records', $Release.Records) }
    if (-not $NoFeed) { $arguments += @('-Feed', $Release.Feed) }
    if (-not $NoSetupFeed) { $arguments += @('-SetupFeed', $Release.SetupFeed) }
    if (-not $NoArmFeed) { $arguments += @('-Arm64Feed', $Release.ArmFeed) }
    if (-not $NoArmSetupFeed) { $arguments += @('-Arm64SetupFeed', $Release.ArmSetupFeed) }
    $said = & (Get-Process -Id $PID).Path -NoProfile -NonInteractive -File "$PSScriptRoot/release_commands.ps1" @arguments 2>&1
    return [pscustomobject]@{ Ended = $LASTEXITCODE; Lines = @($said | ForEach-Object { "$_" }) }
}
function Expect-Refusal($Release, [string] $Expected, [string] $Message, [switch] $NoFeed, [switch] $NoSetupFeed, [switch] $NoArmFeed, [switch] $NoArmSetupFeed, [switch] $NoRecords) {
    $result = Invoke-Commands $Release -NoFeed:$NoFeed -NoSetupFeed:$NoSetupFeed -NoArmFeed:$NoArmFeed -NoArmSetupFeed:$NoArmSetupFeed -NoRecords:$NoRecords
    $text = $result.Lines -join ' | '
    # The refusal's words as they were said, whatever the window: in a console window PowerShell wraps an error's
    # text at the window's width between "|" margins and may colour it, which split the words matched here (a
    # gate run in one failed two of these for that alone, 2026-11-16). No pattern here holds a "|".
    $words = ($text -replace "$([char]27)\[[0-9;]*m", '') -replace '\s*(\|\s*)+', ' '
    Check ($result.Ended -ne 0 -and $words -match $Expected) "$Message ($text)"
    # (Nothing to run is said before what it is about is checked: not one upload, not the release.)
    Check (-not ($result.Lines -match 'r2 object put|gh release (create|download|edit)')) "$Message, and no upload or release is said ($text)"
}

$good = New-Release 'good'
$result = Invoke-Commands $good
Check ($result.Ended -eq 0) "a signed release's commands are said ($($result.Lines -join ' | '))"
$puts = @($result.Lines | Where-Object { $_ -match '^npx --yes wrangler@4\.152\.0 r2 object put ' })
Check ($puts.Count -eq 11) "one upload for each of the seven files and the four feeds ($($puts.Count))"
foreach ($name in Get-ReleaseFileNames '0.1.0' -Signed) {
    Check (@($puts | Where-Object { $_.Contains("`"qkvm-downloads/desktop/0.1.0/$name`"") -and $_.Contains('immutable') -and $_.EndsWith('--remote') }).Count -eq 1) "$name goes to desktop/0.1.0/, kept for good"
}
Check (@($puts | Where-Object { $_.Contains('"qkvm-downloads/desktop/stable.json"') -and $_.Contains('--cache-control "no-cache"') }).Count -eq 1) 'the feed goes to desktop/stable.json, never cached'
Check (@($puts | Where-Object { $_.Contains('"qkvm-downloads/desktop/stable-setup.json"') -and $_.Contains("--file ""$((Resolve-Path -LiteralPath $good.SetupFeed).Path)""") -and $_.Contains('--content-type "application/json"') -and $_.Contains('--cache-control "no-cache"') }).Count -eq 1) 'installed copies'' feed goes to desktop/stable-setup.json, never cached'
Check (@($puts | Where-Object { $_.Contains('"qkvm-downloads/desktop/stable-arm64.json"') -and $_.Contains("--file ""$((Resolve-Path -LiteralPath $good.ArmFeed).Path)""") -and $_.Contains('--cache-control "no-cache"') }).Count -eq 1) 'Windows on Arm''s feed goes to desktop/stable-arm64.json, never cached'
Check (@($puts | Where-Object { $_.Contains('"qkvm-downloads/desktop/stable-arm64-setup.json"') -and $_.Contains("--file ""$((Resolve-Path -LiteralPath $good.ArmSetupFeed).Path)""") -and $_.Contains('--cache-control "no-cache"') }).Count -eq 1) 'Windows on Arm''s installed copies'' feed goes to desktop/stable-arm64-setup.json, never cached'
Check (@($puts | Where-Object { $_.Contains("`"qkvm-downloads/desktop/0.1.0/$armZipName`"") -and $_.Contains('--content-type "application/zip"') }).Count -eq 1) 'the ARM64 ZIP goes up as a ZIP'
$checked = [Array]::FindIndex([string[]] $result.Lines, [Predicate[string]] { param($l) $l -like '# Checked here: *' })
Check ($checked -ge 0 -and $checked -lt [Array]::IndexOf([string[]] $result.Lines, $puts[0])) 'what was checked is said before the first upload'
$release = @($result.Lines | Where-Object { $_ -match '^gh release create v0\.1\.0 --repo qkvm/qkvm-desktop --verify-tag --draft --prerelease ' })
Check ($release.Count -eq 1) "the GitHub release is a draft pre-release of v0.1.0, of the tag that is there ($($result.Lines -join ' | '))"
Check ($result.Lines -ccontains '$env:GH_TOKEN = gh auth token --user pypv') 'it is made as pypv'
Check (@($result.Lines | Where-Object { $_ -match '^pwsh -File tools\\verify_release\.ps1 .*-Feed https://download\.qkvm\.com/desktop/stable\.json -SetupFeed https://download\.qkvm\.com/desktop/stable-setup\.json -Arm64Feed https://download\.qkvm\.com/desktop/stable-arm64\.json -Arm64SetupFeed https://download\.qkvm\.com/desktop/stable-arm64-setup\.json -RequireSignature$' }).Count -eq 1) 'what is published is checked afterwards, the four feeds with it'
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
$firstFeed = & $at '^npx .* r2 object put "qkvm-downloads/desktop/stable(-arm64)?(-setup)?\.json"'
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
Expect-Refusal (New-Release 'zip-not-built' { param($c) $c.Record.packages[0].sha256 = '0' * 64 }) 'windows-x64\.zip is not the ZIP the run built' 'a ZIP that is not the one built is refused'
Expect-Refusal (New-Release 'arm-zip-not-built' { param($c) $c.Record.packages[1].sha256 = '0' * 64 }) 'windows-arm64\.zip is not the ZIP the run built' 'an ARM64 ZIP that is not the one built is refused'
Expect-Refusal (New-Release 'x64-alone' { param($c) $c.Record.packages = @($c.Record.packages[0]) }) 'built no arm64 ZIP' 'a run of x64 alone is refused'
Expect-Refusal (New-Release 'schema-1' { param($c) $c.Record.schema = 1 }) 'not of both kinds of Windows' 'a record of a run before both kinds is refused'
Expect-Refusal (New-Release 'setup-not-built' { param($c) $c.Record.setup.sha256 = '0' * 64 }) 'not the setup the run built' 'a setup that is not the one built is refused'
Expect-Refusal (New-Release 'setup-unrecorded' { param($c) $c.Record.Remove('setup') }) 'made no setup' 'a run that recorded no setup is refused'
# The feed: signed by the key the program pins, current, and naming this ZIP where it is uploaded.
Expect-Refusal (New-Release 'feed-key' { param($c) $c.FeedKey = $other }) 'not signed by the key the program pins' 'a feed signed by another key is refused'
Expect-Refusal (New-Release 'feed-version' { param($c) $c.Payload.version = '0.1.1' }) 'feed offers 0\.1\.1' 'a feed of another version is refused'
Expect-Refusal (New-Release 'feed-sha' { param($c) $c.Payload.sha256 = '0' * 64 }) 'not the x64 ZIP' 'a feed of another ZIP is refused'
Expect-Refusal (New-Release 'feed-bytes' { param($c) $c.Payload.bytes = 1 }) 'not the x64 ZIP' 'a feed of another size is refused'
Expect-Refusal (New-Release 'feed-of-arm' { param($c) $c.Payload = $c.ArmPayload }) 'stable\.json is of windows-arm64-portable' 'an x64 feed of the ARM64 ZIP is refused'
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
Expect-Refusal (New-Release 'setup-feed-of-arm' { param($c) $c.SetupPayload = $c.ArmSetupPayload }) 'stable-setup\.json will not do: it is not of windows-x64-setup' 'an x64 setup feed of Windows on Arm''s platform is refused'
# Windows on Arm's two feeds: the same rules, of its own platforms, the ARM64 ZIP and the one setup.
Expect-Refusal $good 'stable-arm64\.json and stable-arm64-setup\.json' 'a release without Windows on Arm''s feed is refused' -NoArmFeed
Expect-Refusal $good 'stable-arm64\.json and stable-arm64-setup\.json' 'a release without Windows on Arm''s installed copies'' feed is refused' -NoArmSetupFeed
Expect-Refusal (New-Release 'arm-feed-twice' { param($c) $c.ArmFeed = $c.Feed }) 'one is named twice' 'one feed named for two is refused'
Expect-Refusal (New-Release 'arm-feed-key' { param($c) $c.ArmFeedKey = $other }) 'stable-arm64\.json will not do: .*not signed by the key the program pins' 'an Arm feed signed by another key is refused'
Expect-Refusal (New-Release 'arm-feed-version' { param($c) $c.ArmPayload.version = '0.1.1' }) 'stable-arm64\.json.s feed offers 0\.1\.1' 'an Arm feed of another version is refused'
Expect-Refusal (New-Release 'arm-feed-of-x64' { param($c) $c.ArmPayload = $c.Payload }) 'stable-arm64\.json is of windows-x64-portable, not of the arm64 portable ZIP' 'an Arm feed of the x64 ZIP is refused'
Expect-Refusal (New-Release 'arm-feed-sha' { param($c) $c.ArmPayload.sha256 = '0' * 64 }) 'not the arm64 ZIP' 'an Arm feed of another ZIP is refused'
Expect-Refusal (New-Release 'arm-feed-x64-bytes' { param($c) $c.ArmPayload.bytes = $c.Payload.bytes; $c.ArmPayload.sha256 = $c.Payload.sha256 }) 'not the arm64 ZIP' 'an Arm feed naming the x64 ZIP''s bytes is refused'
Expect-Refusal (New-Release 'arm-feed-address' { param($c) $c.ArmPayload.package = "https://download.qkvm.com/desktop/0.1.0/$zipName" }) 'stable-arm64\.json names .* not where it is uploaded' 'an Arm feed naming the x64 ZIP''s address is refused'
Expect-Refusal (New-Release 'arm-feed-old' { param($c) $c.ArmPayload.published = '2026-01-01T00:00:00Z'; $c.ArmPayload.expires = '2026-01-20T00:00:00Z' }) 'stable-arm64\.json will not do' 'an expired Arm feed is refused'
Expect-Refusal (New-Release 'arm-setup-feed-key' { param($c) $c.ArmSetupFeedKey = $other }) 'stable-arm64-setup\.json will not do: .*not signed by the key the program pins' 'an Arm setup feed signed by another key is refused'
Expect-Refusal (New-Release 'arm-setup-feed-of-x64' { param($c) $c.ArmSetupPayload = $c.SetupPayload }) 'stable-arm64-setup\.json will not do: it is not of windows-arm64-setup' 'an Arm setup feed of x64''s platform is refused'
Expect-Refusal (New-Release 'arm-setup-feed-sha' { param($c) $c.ArmSetupPayload.sha256 = '0' * 64 }) 'stable-arm64-setup\.json will not do: it names other bytes' 'an Arm setup feed of another setup is refused'
Expect-Refusal (New-Release 'arm-setup-feed-version' { param($c) $c.ArmSetupPayload.version = '0.1.1' }) 'stable-arm64-setup\.json will not do: it offers 0\.1\.1' 'an Arm setup feed of another version is refused'
Expect-Refusal (New-Release 'arm-setup-feed-address' { param($c) $c.ArmSetupPayload.package = "https://github.com/qkvm/qkvm-desktop/releases/download/v0.1.0/$setupName" }) 'stable-arm64-setup\.json names .* not where it is uploaded' 'an Arm setup feed naming another address is refused'

# --- The FFmpeg that ships: the FFmpeg part this PC built twice and kept the records of (build_release.ps1 -Part ffmpeg
# -Records), held to each ZIP's DLLs and package manifest, and through the manifests to the setup (review armp-1 M1).
Expect-Refusal $good 'with -Records' 'a release without this PC''s FFmpeg records is refused' -NoRecords
Expect-Refusal (New-Release 'ffmpeg-forged' -FFmpeg { param($f)
    $f.ZipDlls['x64']['avcodec-63.dll'] = 'replaced avcodec'
    $f.Named = New-KeptRecord '2026-11-20T12:00:00.0000000Z' $f.ZipDlls @('ffmpeg-0.1.0-first.json (x64)', 'ffmpeg-0.1.0-first.json (arm64)') }) 'not a record this PC kept' 'a record written anew to match a DLL replaced after the FFmpeg part, which this PC never kept, is refused'
Expect-Refusal (New-Release 'ffmpeg-dll-swapped' -FFmpeg { param($f) $f.ZipDlls['x64']['avcodec-63.dll'] = 'replaced avcodec' }) 'x64 ZIP: avcodec-63\.dll is not the DLL the FFmpeg part made for x64' 'a ZIP with a DLL of other bytes than the kept record names is refused'
Expect-Refusal (New-Release 'ffmpeg-arm-of-x64' -FFmpeg { param($f) $f.ZipDlls['arm64'] = (Copy-Dlls $genuine)['x64'] }) 'arm64 ZIP: avcodec-63\.dll is not the DLL the FFmpeg part made for arm64' 'an ARM64 ZIP that holds x64''s FFmpeg is refused'
Expect-Refusal (New-Release 'ffmpeg-dll-more' -FFmpeg { param($f) $f.ZipDlls['x64']['swresample-6.dll'] = 'made-up swresample' }) 'x64 ZIP: swresample-6\.dll is not a DLL the FFmpeg part made for x64' 'a ZIP with an FFmpeg DLL the part did not make is refused'
Expect-Refusal (New-Release 'ffmpeg-listed-other' -FFmpeg { param($f) $f.ListedDlls = Copy-Dlls $f.ZipDlls; $f.ListedDlls['x64']['avutil-61.dll'] = 'another avutil' }) 'x64 ZIP''s package manifest: avutil-61\.dll is not the DLL the FFmpeg part made for x64' 'a package manifest that names other FFmpeg DLLs, which the setup carries, is refused'
Expect-Refusal (New-Release 'ffmpeg-manifest-not-setup' { param($c) $c.Record.setup.made.packages[1].manifestSha256 = '0' * 64 }) 'arm64 ZIP''s package manifest is not the one the setup was made of' 'a ZIP whose package is not the one the setup was made of is refused'
Expect-Refusal (New-Release 'ffmpeg-manifest-not-built' { param($c) $c.Record.packages[0].manifestSha256 = '0' * 64 }) 'x64 ZIP''s package manifest is not the one the run recorded' 'a ZIP whose package manifest is not the one the run recorded is refused'
Expect-Refusal (New-Release 'ffmpeg-run-copy-swapped' -FFmpeg { param($f) $f.RunCopy = $f.Kept['ffmpeg-0.1.0-first.json'] }) 'the run''s copy of the FFmpeg part''s record is not the record the build record names' 'a run whose copy of the record is another kept record is refused'
Expect-Refusal (New-Release 'ffmpeg-embedded-swapped' -FFmpeg { param($f)
    $f.Embedded = New-KeptRecord '2026-11-20T12:00:00.0000000Z' @{ x64 = [ordered]@{ 'avcodec-63.dll' = 'replaced avcodec'; 'avutil-61.dll' = 'made-up x64 avutil' }; arm64 = $genuine['arm64'] } }) 'the record the build record holds is not the kept record it names \(x64' 'a build record that holds another record than the kept one it names is refused'
Expect-Refusal (New-Release 'ffmpeg-other-commit' -FFmpeg { param($f)
    $f.Kept['ffmpeg-0.1.0-first.json'] = New-KeptRecord '2026-11-20T10:00:00.0000000Z' $genuine -Commit ('9' * 40)
    $f.Kept['ffmpeg-0.1.0-second.json'] = New-KeptRecord '2026-11-20T11:00:00.0000000Z' $genuine @('ffmpeg-0.1.0-first.json (x64)', 'ffmpeg-0.1.0-first.json (arm64)') -Commit ('9' * 40)
    $f.Named = $f.Kept['ffmpeg-0.1.0-second.json'] }) 'the kept record is of another commit \(9{40}\)' 'an FFmpeg part of another commit, though kept and reproduced, is refused'
Expect-Refusal (New-Release 'ffmpeg-two-differ' -FFmpeg { param($f)
    $differ = Copy-Dlls $genuine; $differ['arm64']['avutil-61.dll'] = 'another ARM64 avutil'
    $f.Kept['ffmpeg-0.1.0-first.json'] = New-KeptRecord '2026-11-20T10:00:00.0000000Z' $differ }) 'arm64: not byte for byte the build of the same pins recorded in ffmpeg-0\.1\.0-first\.json \(bin/avutil-61\.dll differ\)' 'two builds of the same pins that made other bytes are refused'
Expect-Refusal (New-Release 'ffmpeg-once' -FFmpeg { param($f) $f.Kept.Remove('ffmpeg-0.1.0-first.json') }) 'no other build of its pins kept here made the same bytes, for x64, arm64' 'the first build of FFmpeg''s pins, never reproduced, is refused'
Expect-Refusal (New-Release 'ffmpeg-copy-is-no-build' -FFmpeg { param($f) $f.Kept.Remove('ffmpeg-0.1.0-first.json'); $f.Kept['ffmpeg-0.1.0-copy.json'] = $f.Named }) 'no other build of its pins kept here made the same bytes, for x64, arm64' 'a copy of the named record kept twice is no second build'
Expect-Refusal (New-Release 'ffmpeg-other-pins' -FFmpeg { param($f) $f.Kept['ffmpeg-0.1.0-first.json'] = New-KeptRecord '2026-11-20T10:00:00.0000000Z' $genuine -Toolchain 'other-compiler_2.0_amd64.deb' }) 'no other build of its pins kept here made the same bytes, for x64, arm64' 'a build of other pins is no second build of these'
# (x64's FFmpeg of 0.1.2's sources and toolchain, built twice the same here, but not the DLLs 0.1.2's release published:
# refused, and the refusal names the DLL; review r2 of arm and armp, L4.)
Expect-Refusal (New-Release 'ffmpeg-not-as-published' -FFmpeg { param($f)
    $f.Kept['ffmpeg-0.1.0-first.json'] = New-KeptRecord '2026-11-20T10:00:00.0000000Z' $genuine -SourcesOf012
    $f.Kept['ffmpeg-0.1.0-second.json'] = New-KeptRecord '2026-11-20T11:00:00.0000000Z' $genuine @('ffmpeg-0.1.0-first.json (x64)', 'ffmpeg-0.1.0-first.json (arm64)') -SourcesOf012
    $f.Named = $f.Kept['ffmpeg-0.1.0-second.json'] }) 'x64: avcodec-63\.dll is not the one 0\.1\.2''s release published from the same sources and toolchain' 'x64''s FFmpeg of 0.1.2''s sources and toolchain that is not 0.1.2''s published DLLs is refused, by name'
Expect-Refusal (New-Release 'ffmpeg-records-not-alone' -FFmpeg { param($f) [IO.Directory]::CreateDirectory($f.Records) | Out-Null; [IO.File]::WriteAllText((Join-Path $f.Records 'notes.txt'), 'notes') }) 'notes\.txt is not the record of an FFmpeg part' 'a folder of records that holds anything else is refused'
# --- The ARM64 ZIP's start checks, which an x64 PC cannot run: passed on Windows on Arm for this very ZIP, by the record
# kept with the run (review armp-1 M3).
Expect-Refusal (New-Release 'arm-unstarted' -Started { param($e) $e['drop'] = $true }) 'start checks have not passed on Windows on Arm .*there is no record of the ARM64 package''s start checks' 'a release whose ARM64 ZIP was never started on Windows on Arm is refused'
Expect-Refusal (New-Release 'arm-started-other-zip' -Started { param($e) $e.sha256 = '0' * 64 }) 'not of this ZIP \(its size or its SHA-256 differs\)' 'the start checks of another ARM64 ZIP are refused'
Expect-Refusal (New-Release 'arm-started-on-x64' -Started { param($e) $e.windows.machine = 'x64' }) 'not made on Windows on Arm \(on x64\)' 'start checks made on x64 Windows are refused'
Expect-Refusal (New-Release 'arm-started-failed' -Started { param($e) $e.passed = $false }) 'does not say the checks passed' 'start checks that did not pass are refused'
Expect-Refusal (New-Release 'arm-started-of-x64' -Started { param($e) $e.archive = $zipName; $e.architecture = 'x64' }) 'not of an ARM64 package' 'the start checks of the x64 ZIP are refused'
# (Made by the tools of the run's export, as its build record names them, each by its SHA-256, and by no other; review r2
# of arm and armp, L2.)
Expect-Refusal (New-Release 'arm-started-by-other-tools' -Started { param($e) $e.tools['tools/build_common.ps1'] = '0' * 64 }) 'made by other tools than this run''s export''s \(tools/build_common\.ps1 differ' 'start checks made by another build_common.ps1 than the run''s export''s are refused'
Expect-Refusal (New-Release 'arm-started-by-another-table' -Started { param($e) $e.tools['tools/image-protections.json'] = '1' * 64 }) '\(tools/image-protections\.json differ' 'start checks made with another table of protections are refused'
Expect-Refusal (New-Release 'arm-started-naming-no-tools' -Started { param($e) $e.Remove('tools') }) 'does not name the tools that made it' 'a record of an earlier test_package_archive.ps1, which names no tools, is refused'
Expect-Refusal (New-Release 'arm-started-by-other-tools-words' -Started { param($e) $e.tools['tools/test_package.ps1'] = '0' * 64 }) 'the export''s tools folder of the run''s commit' 'the refusal says which tools to run the checks with'
Expect-Refusal (New-Release 'run-naming-no-start-tools' { param($c) $c.Record.checks.Remove('arm64StartupTools') }) 'names no tools of the ARM64 start checks' 'a run whose build record names no tools of the start checks is refused'
Expect-Refusal (New-Release 'run-naming-other-start-tools' { param($c) $c.Record.checks.arm64StartupTools['tools/test_package_archive.ps1'] = '3' * 64 }) 'made by other tools than this run''s export''s \(tools/test_package_archive\.ps1 differ' 'start checks made by other tools than the ones the run''s build record names are refused'

$gone = New-Release 'ffmpeg-records-gone'
Remove-Item -LiteralPath $gone.Records -Recurse -Force
Expect-Refusal $gone 'there is no folder of FFmpeg records' 'a folder of records that is not there is refused'

$signer.Dispose(); $other.Dispose()
if ($failures) { Write-Host "$failures release commands check(s) failed."; exit 1 }
Remove-Item -LiteralPath $work -Recurse -Force
Write-Output 'Release commands checks passed: the uploads, the draft release of its tag and the check are said for a signed release once its FFmpeg is held to the records this PC kept, and SHA256SUMS, its signature and the four feeds are checked against the run, and nothing for anything else.'
