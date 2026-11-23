#Requires -Version 7.2
<#
tools/release_files.ps1 on a made-up run (a record, its two ZIPs and setup, the sources it was built from, an export): the six
release files and nothing else, and every run, ZIP, export and folder it refuses. Every failed check is a "FAIL:"
line, and the script then fails. Nothing is fetched; the byte scan is a stand-in that says what it is told to.
#>
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\build_common.ps1"
Add-Type -AssemblyName System.IO.Compression.FileSystem

$failures = 0
function Check([bool] $Condition, [string] $Message) {
    if (-not $Condition) { Write-Host "FAIL: $Message"; $script:failures++ }
}
$work = Join-Path (Split-Path -Parent $PSScriptRoot) "build/release-tests/$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $work | Out-Null
$git = (Get-Command git -CommandType Application | Select-Object -First 1).Source
function Invoke-Git([string] $Folder, [string[]] $Arguments) {
    & $git -C $Folder -c user.name=fixture -c user.email=fixture@example.invalid -c core.autocrlf=false @Arguments 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "git $($Arguments -join ' ') failed in $Folder." }
}
function New-Repository([string] $Folder, [hashtable] $Files) {
    New-Item -ItemType Directory -Force -Path $Folder | Out-Null
    foreach ($name in $Files.Keys) {
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent (Join-Path $Folder $name)) | Out-Null
        [IO.File]::WriteAllText((Join-Path $Folder $name), $Files[$name])
    }
    Invoke-Git $Folder @('init', '-q')
    Invoke-Git $Folder @('add', '-A')
    Invoke-Git $Folder @('commit', '-q', '-m', 'fixture')
    return (& $git -C $Folder rev-parse HEAD).Trim()
}
function New-Zip([string] $Zip, [hashtable] $Files, [string] $Top = 'qKVM-Desktop-0.1.0-windows-x64') {
    $folder = Join-Path $work "zip-$([guid]::NewGuid().ToString('N'))"
    $top = Join-Path $folder $Top
    foreach ($name in $Files.Keys) {
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent (Join-Path $top $name)) | Out-Null
        [IO.File]::WriteAllText((Join-Path $top $name), $Files[$name])
    }
    if (Test-Path -LiteralPath $Zip) { Remove-Item -LiteralPath $Zip }
    [IO.Compression.ZipFile]::CreateFromDirectory($top, $Zip, [IO.Compression.CompressionLevel]::Optimal, $true)
}
# libdatachannel as the build has it: its submodules added from made-up upstream repositories, the two the build
# uses checked out, and one it does not use left out (as deps/json is: git submodule status says "-" of it).
function New-Datachannel([string] $Folder) {
    $upstream = Join-Path (Split-Path -Parent $Folder) 'upstream'
    foreach ($name in 'libjuice', 'plog', 'json') { New-Repository "$upstream/$name" @{ 'LICENSE' = "made-up $name licence"; 'src.c' = "// $name" } | Out-Null }
    New-Repository $Folder @{ 'src/peer.cpp' = 'made-up'; 'LICENSE' = 'MPL' } | Out-Null
    foreach ($name in 'libjuice', 'plog', 'json') {
        Invoke-Git $Folder @('-c', 'protocol.file.allow=always', 'submodule', 'add', '-q', "$upstream/$name".Replace('\', '/'), "deps/$name")
    }
    Invoke-Git $Folder @('commit', '-q', '-m', 'submodules')
    Invoke-Git $Folder @('submodule', 'deinit', '-q', '-f', 'deps/json')
    return (& $git -C $Folder rev-parse HEAD).Trim()
}
$manifest = '{"cpp": {"gitlinks": {"libjuice": "x", "plog": "y"}}}'
# (Made once, and copied into each case with its submodules: making it anew for each case is many git runs.)
$template = Join-Path $work 'datachannel-template/libdatachannel'
$null = New-Datachannel $template
# An entry committed in $Folder as git keeps it, with no file for it in the checkout: one Windows cannot hold under its
# name, which a checkout on Windows leaves out (git itself takes any name). A submodule's is then recorded in
# libdatachannel as the commit it is at.
function Add-Entry([string] $Folder, [string] $Path) {
    $blob = ('made-up' | & $git -C $Folder hash-object -w --stdin).Trim()
    Invoke-Git $Folder @('-c', 'core.protectNTFS=false', 'update-index', '--add', '--cacheinfo', "100644,$blob,$Path")
    Invoke-Git $Folder @('-c', 'core.protectNTFS=false', 'commit', '-q', '-m', 'an entry Windows cannot hold')
}
function Add-ModuleEntry([string] $Datachannel, [string] $Module, [string] $Path) {
    Add-Entry "$Datachannel/deps/$Module" $Path
    Invoke-Git $Datachannel @('add', "deps/$Module")
    Invoke-Git $Datachannel @('commit', '-q', '-m', "deps/$Module moved")
}
$scanner = Join-Path $work 'scan.py'
[IO.File]::WriteAllText($scanner, "import os, sys`nsys.exit(int(os.environ.get('MADE_UP_SCAN', '0')))`n")

function Copy-Tools($Tools) { $copy = [ordered]@{}; foreach ($name in $Tools.Keys) { $copy[$name] = $Tools[$name] }; return $copy }
# One made-up build folder, run and export, made anew for each case. Its ARM64 ZIP's start checks passed on Windows on
# Arm, as tools/test_package_archive.ps1 -Evidence records them there (New-StartupEvidence's shape), of the ZIP as the
# case leaves it, by the export's tools; $Evidence changes that record, and a record set to $null is not given.
# $Sources changes libdatachannel's checkout as the build had it (given its folder), before what the build records of it
# is worked out from it: its commit, its submodules, and each kind's WebRTC build record of the tree it read.
function New-Case([string] $Name, [scriptblock] $Change = {}, [scriptblock] $Evidence = {}, [scriptblock] $Sources = {}) {
    $case = Join-Path $work $Name
    $root = Join-Path $case 'root'
    New-Item -ItemType Directory -Force -Path "$root/downloads/ffmpeg", "$root/src/.deps/webrtc-trial", "$root/src/.deps/arm64/webrtc", "$root/runs/20260101-000000" | Out-Null
    [IO.File]::WriteAllText("$root/.qkvm-release-root", 'made by build_release.ps1')
    [IO.File]::WriteAllText("$root/downloads/ffmpeg/ffmpeg-9.0.2.tar.xz", 'made-up ffmpeg source')
    [IO.File]::WriteAllText("$root/downloads/ffmpeg/zlib-1.3.2.tar.gz", 'made-up zlib source')
    [IO.File]::WriteAllText("$root/downloads/ffmpeg/make_4.3_amd64.deb", 'a tool, never a release file')
    [IO.File]::WriteAllText("$root/src/.deps/webrtc-trial/mbedtls-3.6.7.tar.bz2", 'made-up mbedtls source')
    $datachannelFolder = "$root/src/.deps/webrtc-trial/libdatachannel"
    Copy-Item -LiteralPath $template -Destination $datachannelFolder -Recurse -Force
    & $Sources $datachannelFolder
    $datachannel = (& $git -C $datachannelFolder rev-parse HEAD).Trim()
    $submodules = @(& $git -C $datachannelFolder submodule status | ForEach-Object { "$_".Trim() })
    # Each kind's WebRTC build record, as experiments/webrtc/build-cpp-dependencies.ps1 writes it beside its install
    # folder: the sources it was built from, the tree as it read it among them (New-WebRtcRecord's shape).
    $gitlinks = [ordered]@{}
    foreach ($module in 'libjuice', 'plog') { $gitlinks[$module] = (& $git -C "$datachannelFolder/deps/$module" rev-parse HEAD).Trim() }
    $read = Get-SourceTreeDigest $datachannelFolder
    $mbedtlsSha256 = Get-Sha256 "$root/src/.deps/webrtc-trial/mbedtls-3.6.7.tar.bz2"
    $webrtcRecords = [ordered]@{}
    foreach ($kind in 'x64', 'arm64') {
        $webrtcRecords[$kind] = [ordered]@{ schema = 1; product = 'qKVM Desktop'; part = 'webrtc'; architecture = $kind
            sources = [ordered]@{
                libdatachannel = [ordered]@{ version = '0.24.6'; commit = $datachannel; gitlinks = Copy-Tools $gitlinks; tree = $read }
                mbedtls = [ordered]@{ version = '3.6.7'; archiveSha256 = $mbedtlsSha256; tree = 'e' * 64 } }
            script = [ordered]@{ name = 'experiments/webrtc/build-cpp-dependencies.ps1'; sha256 = 'd' * 64 }; commit = $null
            built = @([ordered]@{ name = 'bin/datachannel.dll'; bytes = 1; sha256 = 'c' * 64 }) }
    }
    $export = Join-Path $case 'export'
    $commit = New-Repository $export @{ 'CMakeLists.txt' = 'project(QkvmDesktop VERSION 0.1.0 LANGUAGES CXX)'; 'tools/a.ps1' = "# a`n# b`n"
        'experiments/webrtc/dependencies.json' = $manifest
        # (The four files of the ARM64 start checks, made up, which the record of those checks names by their SHA-256.)
        'tools/test_package_archive.ps1' = "# made-up start checks`n"; 'tools/test_package.ps1' = "# made-up package checks`n"
        'tools/build_common.ps1' = "# made-up shared tools`n"; 'tools/image-protections.json' = "{}`n" }
    $tools = [ordered]@{}
    foreach ($name in 'test_package_archive.ps1', 'test_package.ps1', 'build_common.ps1', 'image-protections.json') { $tools["tools/$name"] = Get-Sha256 "$export/tools/$name" }
    # (As this PC's git is set: line ends turned to Windows' on the way out.)
    & $git -C $export config core.autocrlf true
    $run = "$root/runs/20260101-000000"
    $zip = "$run/qKVM-Desktop-0.1.0-windows-x64.zip"
    New-Zip $zip @{ 'qkvm-desktop.exe' = 'made-up program'; 'THIRD-PARTY.md' = 'notices'; 'plugins/platforms/qwindows.dll' = 'plugin' }
    $armZip = "$run/qKVM-Desktop-0.1.0-windows-arm64.zip"
    New-Zip $armZip @{ 'qkvm-desktop.exe' = 'made-up ARM64 program'; 'THIRD-PARTY.md' = 'notices'; 'plugins/platforms/qwindows.dll' = 'ARM64 plugin' } 'qKVM-Desktop-0.1.0-windows-arm64'
    # The setup the run made from its package (tools/build_setup.ps1), where build_release.ps1 leaves it.
    New-Item -ItemType Directory -Path "$run/setup" | Out-Null
    $setup = "$run/setup/qKVM-Desktop-0.1.0-windows-setup.exe"
    [IO.File]::WriteAllText($setup, 'made-up setup, never run')
    $record = [ordered]@{
        schema = 2; version = '0.1.0'; trial = $false
        source = [ordered]@{ commit = $commit }
        updates = [ordered]@{ publicKey = New-ThrowawayPublicKey }
        packages = @(
            [ordered]@{ architecture = 'x64'; archive = 'qKVM-Desktop-0.1.0-windows-x64.zip'; bytes = (Get-Item $zip).Length; sha256 = Get-Sha256 $zip }
            [ordered]@{ architecture = 'arm64'; archive = 'qKVM-Desktop-0.1.0-windows-arm64.zip'; bytes = (Get-Item $armZip).Length; sha256 = Get-Sha256 $armZip })
        dependencies = [ordered]@{
            # (The FFmpeg part's record, as the build part took it: its sources, and each kind's build of them by its pins,
            # among the rest.)
            ffmpeg = [ordered]@{ record = [ordered]@{ part = 'ffmpeg'; sources = @(
                [ordered]@{ archive = 'ffmpeg-9.0.2.tar.xz'; bytes = (Get-Item "$root/downloads/ffmpeg/ffmpeg-9.0.2.tar.xz").Length; sha256 = Get-Sha256 "$root/downloads/ffmpeg/ffmpeg-9.0.2.tar.xz" }
                [ordered]@{ archive = 'zlib-1.3.2.tar.gz'; bytes = (Get-Item "$root/downloads/ffmpeg/zlib-1.3.2.tar.gz").Length; sha256 = Get-Sha256 "$root/downloads/ffmpeg/zlib-1.3.2.tar.gz" })
                builds = @(foreach ($kind in 'x64', 'arm64') {
                    [ordered]@{ architecture = $kind; pins = [ordered]@{
                        ffmpeg = "ffmpeg-9.0.2.tar.xz $((Get-Item "$root/downloads/ffmpeg/ffmpeg-9.0.2.tar.xz").Length) $(Get-Sha256 "$root/downloads/ffmpeg/ffmpeg-9.0.2.tar.xz")"
                        zlib = "zlib-1.3.2.tar.gz $((Get-Item "$root/downloads/ffmpeg/zlib-1.3.2.tar.gz").Length) $(Get-Sha256 "$root/downloads/ffmpeg/zlib-1.3.2.tar.gz")" } }
                }) } }
            webrtc = [ordered]@{ libdatachannel = $datachannel; submodules = $submodules; mbedtls = $mbedtlsSha256 }
        }
        setup = [ordered]@{ name = 'qKVM-Desktop-0.1.0-windows-setup.exe'; file = 'setup/qKVM-Desktop-0.1.0-windows-setup.exe'
            bytes = (Get-Item $setup).Length; sha256 = Get-Sha256 $setup }
        checks = [ordered]@{ betaAudit = 'accepted'; betaRefused = @(); privateScan = 'clean'; setupTest = 'passed'
            # (The export's tools of the ARM64 start checks, by SHA-256, as build_release.ps1 records them from its tree.)
            arm64StartupTools = Copy-Tools $tools }
    }
    # (WebRtc: each kind's WebRTC build record, where its build wrote it, $null for none; Audited: what each kind's beta
    # audit names that record by, its SHA-256 as written unless the case says another.)
    $context = [pscustomobject]@{ Root = $root; Run = $run; Zip = $zip; ArmZip = $armZip; Export = $export; Record = $record; Out = "$case/out"
        Datachannel = $datachannelFolder; Setup = $setup; Evidence = "$case/arm64-startup-on-arm.json"; WebRtc = $webrtcRecords
        WebRtcFiles = [ordered]@{ x64 = "$root/src/.deps/webrtc-trial/webrtc-record.json"; arm64 = "$root/src/.deps/arm64/webrtc/webrtc-record.json" }
        Audited = [ordered]@{} }
    & $Change $context
    $context.Record | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath "$run/build-record.json" -Encoding utf8
    # Each kind's beta audit, which held its package's datachannel.dll to that kind's WebRTC build record (audit_release.ps1
    # names the record by its place and SHA-256).
    foreach ($kind in 'x64', 'arm64') {
        $file = $context.WebRtcFiles[$kind]
        if ($null -ne $context.WebRtc[$kind]) { $context.WebRtc[$kind] | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $file -Encoding utf8 }
        $sha256 = if ($context.Audited.Contains($kind)) { $context.Audited[$kind] } elseif (Test-Path -LiteralPath $file) { Get-Sha256 $file } else { '0' * 64 }
        [ordered]@{ gates = @(); webrtcRecord = [ordered]@{ file = $file; sha256 = $sha256 }; webrtcRefusals = @() } | ConvertTo-Json -Depth 4 |
            Set-Content -LiteralPath "$run/audit-beta-$kind.json" -Encoding utf8
    }
    $started = [ordered]@{ schema = 1; product = 'qKVM Desktop'; check = 'tools/test_package_archive.ps1'; tools = Copy-Tools $tools; passed = $true
        archive = 'qKVM-Desktop-0.1.0-windows-arm64.zip'; bytes = $(if (Test-Path -LiteralPath $armZip) { (Get-Item -LiteralPath $armZip).Length } else { 0 })
        sha256 = $(if (Test-Path -LiteralPath $armZip) { Get-Sha256 $armZip } else { '0' * 64 }); architecture = 'arm64'; version = '0.1.0'
        windows = [ordered]@{ machine = 'arm64'; process = 'arm64'; caption = 'Microsoft Windows 11 Pro'; version = '10.0.26100' }; at = '2026-11-22T10:00:00.0000000Z' }
    $null = & $Evidence $started
    if ($started.Contains('drop')) { $started = $null }
    if ($null -ne $started) { $started | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $context.Evidence -Encoding utf8 } else { $context.Evidence = '' }
    return $context
}
function Invoke-Files($Context, [hashtable] $Environment = @{}) {
    $saved = @{}
    foreach ($name in $Environment.Keys) { $saved[$name] = [Environment]::GetEnvironmentVariable($name); [Environment]::SetEnvironmentVariable($name, $Environment[$name]) }
    try {
        $started = @(if ($Context.Evidence) { '-Arm64Startup', $Context.Evidence })
        $said = & (Get-Process -Id $PID).Path -NoProfile -NonInteractive -File "$PSScriptRoot/release_files.ps1" -Run $Context.Run `
            -Export $Context.Export -Out $Context.Out -Scanner $scanner @started 2>&1
        return [pscustomobject]@{ Ended = $LASTEXITCODE; Said = ($said | ForEach-Object { "$_" }) -join "`n" }
    } finally {
        # (A variable that was not there is taken away again: PowerShell hands .NET an empty text for $null, and the
        # case after it would be run with the variable empty.)
        foreach ($name in $saved.Keys) {
            if ($null -eq $saved[$name]) { Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue }
            else { [Environment]::SetEnvironmentVariable($name, $saved[$name]) }
        }
    }
}
function Expect-Refusal($Context, [string] $Expected, [string] $Message, [hashtable] $Environment = @{}, [string] $Without = '') {
    $result = Invoke-Files $Context $Environment
    # (The refusal's words as they were said, whatever the window: PowerShell wraps an error's text at the window's
    # width between "|" margins and may colour it, as test_release_commands.ps1 says. No pattern here holds a "|".
    # A pattern that starts with (?-i) is held to its capitals. $Without: words the refusal must not say.)
    $words = ($result.Said -replace "$([char]27)\[[0-9;]*m", '') -replace '\s*(\|\s*)+', ' '
    Check ($result.Ended -ne 0 -and $words -match $Expected -and (-not $Without -or $words -notmatch $Without)) "$Message (exit $($result.Ended): $($result.Said))"
}
function Set-Zip($Context, [hashtable] $Files, [string] $Kind = 'x64', [string] $Top = '') {
    $zip = if ($Kind -ceq 'x64') { $Context.Zip } else { $Context.ArmZip }
    New-Zip $zip $Files $(if ($Top) { $Top } else { "qKVM-Desktop-0.1.0-windows-$Kind" })
    $built = @($Context.Record.packages | Where-Object { $_.architecture -ceq $Kind })[0]
    $built.bytes = (Get-Item $zip).Length
    $built.sha256 = Get-Sha256 $zip
}

# --- A run that is a release.
$good = New-Case 'good'
$result = Invoke-Files $good
Check ($result.Ended -eq 0) "a run that passed is released ($($result.Said))"
$names = @(Get-ChildItem -LiteralPath $good.Out -Force | ForEach-Object Name | Sort-Object -CaseSensitive)
Check (($names -join '|') -ceq ((Get-ReleaseFileNames '0.1.0' | Sort-Object -CaseSensitive) -join '|') -and $names.Count -eq 6) "the six release files and nothing else ($($names -join ', '))"
Check ((Get-Sha256 "$($good.Out)/qKVM-Desktop-0.1.0-windows-x64.zip") -ceq $good.Record.packages[0].sha256) 'the x64 ZIP is the one built'
Check ((Get-Sha256 "$($good.Out)/qKVM-Desktop-0.1.0-windows-arm64.zip") -ceq $good.Record.packages[1].sha256) 'the ARM64 ZIP is the one built'
Check ((Get-Sha256 "$($good.Out)/qKVM-Desktop-0.1.0-windows-setup.exe") -ceq $good.Record.setup.sha256) 'the setup is the one built'
$sums = [IO.File]::ReadAllText("$($good.Out)/SHA256SUMS")
$wanted = ((@('qKVM-Desktop-0.1.0-source.zip', 'qKVM-Desktop-0.1.0-third-party-sources.zip', 'qKVM-Desktop-0.1.0-windows-x64.zip',
    'qKVM-Desktop-0.1.0-windows-arm64.zip', 'qKVM-Desktop-0.1.0-windows-setup.exe') | Sort-Object -CaseSensitive | ForEach-Object { "$(Get-Sha256 "$($good.Out)/$_")  $_`n" }) -join '')
Check ($sums -ceq $wanted) 'SHA256SUMS is as sha256sum writes it'
$source = [IO.Compression.ZipFile]::OpenRead("$($good.Out)/qKVM-Desktop-0.1.0-source.zip")
$entries = @($source.Entries | ForEach-Object FullName)
# As committed, byte for byte, whatever this PC's git does with line ends on the way out (it is told to make them
# Windows' here).
$reader = [IO.StreamReader]::new($source.GetEntry('qkvm-desktop-0.1.0/tools/a.ps1').Open())
$asArchived = $reader.ReadToEnd(); $reader.Dispose(); $source.Dispose()
Check ($asArchived -ceq "# a`n# b`n") "the source is as committed, with its line ends ($([regex]::Escape($asArchived)))"
Check ($entries -ccontains 'qkvm-desktop-0.1.0/CMakeLists.txt' -and $entries -ccontains 'qkvm-desktop-0.1.0/tools/a.ps1') "the source is the export's commit ($($entries -join ', '))"
$bundle = [IO.Compression.ZipFile]::OpenRead("$($good.Out)/qKVM-Desktop-0.1.0-third-party-sources.zip")
$entries = @($bundle.Entries | ForEach-Object { $_.FullName.Replace('\', '/') }); $bundle.Dispose()
foreach ($part in 'README.txt', 'ffmpeg-9.0.2.tar.xz', 'zlib-1.3.2.tar.gz', 'mbedtls-3.6.7.tar.bz2', 'libdatachannel/src/peer.cpp', 'libdatachannel/LICENSE',
    'libdatachannel/deps/libjuice/LICENSE', 'libdatachannel/deps/plog/src.c') {
    Check ($entries -ccontains $part) "the third-party sources hold $part"
}
Check (-not ($entries -match '^libdatachannel/deps/json/.')) 'the third-party sources hold nothing of a submodule the build does not use'
Check (-not ($entries -match '\.deb$')) 'the third-party sources hold no tool'
$listed = Get-Content -LiteralPath "$($good.Run)/release-files.json" -Raw | ConvertFrom-Json
Check (@($listed.files).Count -eq 6 -and $listed.commit -ceq $good.Record.source.commit) 'release-files.json names the six and the commit'
# (The record of the ARM64 ZIP's start checks, kept with the run for release_commands.ps1, and named by its SHA-256.)
Check ((Test-Path -LiteralPath "$($good.Run)/arm64-startup.json") -and (Get-Sha256 "$($good.Run)/arm64-startup.json") -ceq (Get-Sha256 $good.Evidence) -and
    $listed.arm64Startup -ceq (Get-Sha256 $good.Evidence)) 'the record of the ARM64 start checks is kept with the run, and release-files.json names it'

# --- The ARM64 package's start checks, which an x64 PC cannot run: passed on Windows on Arm for this very ZIP, or no
# release files (review armp-1 M3).
Expect-Refusal (New-Case 'arm-unstarted' -Evidence { param($e) $e['drop'] = $true }) 'start checks have not passed on Windows on Arm .*there is no record of the ARM64 package''s start checks' 'a run whose ARM64 ZIP was never started on Windows on Arm is not released'
Check (-not (Test-Path -LiteralPath (Join-Path $work 'arm-unstarted/out'))) 'and nothing is made for it'
Expect-Refusal (New-Case 'arm-started-other-zip' -Evidence { param($e) $e.sha256 = '0' * 64 }) 'not of this ZIP \(its size or its SHA-256 differs\)' 'the start checks of another ARM64 ZIP are not this one''s'
Expect-Refusal (New-Case 'arm-started-other-size' -Evidence { param($e) $e.bytes = 1 }) 'not of this ZIP \(its size or its SHA-256 differs\)' 'the start checks of an ARM64 ZIP of another size are not this one''s'
Expect-Refusal (New-Case 'arm-started-of-x64-zip' -Evidence { param($e) $e.archive = 'qKVM-Desktop-0.1.0-windows-x64.zip'; $e.architecture = 'x64' }) 'not of qKVM-Desktop-0\.1\.0-windows-arm64\.zip' 'the start checks of the x64 ZIP are not the ARM64 one''s'
Expect-Refusal (New-Case 'arm-started-of-x64-kind' -Evidence { param($e) $e.architecture = 'x64' }) 'not of an ARM64 package' 'start checks that name another kind of package are refused'
Expect-Refusal (New-Case 'arm-started-on-x64' -Evidence { param($e) $e.windows.machine = 'x64' }) 'not made on Windows on Arm \(on x64\)' 'start checks made on x64 Windows are refused'
Expect-Refusal (New-Case 'arm-started-failed' -Evidence { param($e) $e.passed = $false }) 'does not say the checks passed' 'start checks that did not pass are refused'
Expect-Refusal (New-Case 'arm-started-passed-text' -Evidence { param($e) $e.passed = 'true' }) 'does not say the checks passed' 'start checks whose word for passed is not true itself are refused'
Expect-Refusal (New-Case 'arm-started-other-check' -Evidence { param($e) $e.check = 'tools/test_package.ps1' }) 'not a record of tools/test_package_archive\.ps1' 'a record of another check is refused'
Expect-Refusal (New-Case 'arm-started-schema' -Evidence { param($e) $e.schema = 2 }) 'not a record of tools/test_package_archive\.ps1' 'a record of a schema this does not know is refused'
# (Made by the tools of the run's export, the export's tools folder copied to Windows on Arm: each of its four files of
# the checks by its SHA-256, and no other; review r2 of arm and armp, L2.)
Expect-Refusal (New-Case 'arm-started-by-other-tools' -Evidence { param($e) $e.tools['tools/build_common.ps1'] = '0' * 64 }) 'made by other tools than this run''s export''s \(tools/build_common\.ps1 differ' 'start checks made by another build_common.ps1 than the export''s are refused'
Expect-Refusal (New-Case 'arm-started-by-another-table' -Evidence { param($e) $e.tools['tools/image-protections.json'] = '1' * 64 }) '\(tools/image-protections\.json differ' 'start checks made with another table of protections are refused'
Expect-Refusal (New-Case 'arm-started-by-another-check' -Evidence { param($e) $e.tools['tools/test_package_archive.ps1'] = '4' * 64 }) '\(tools/test_package_archive\.ps1 differ' 'start checks made by another test_package_archive.ps1 are refused'
Expect-Refusal (New-Case 'arm-started-without-a-tool' -Evidence { param($e) $e.tools.Remove('tools/test_package.ps1') }) '\(tools/test_package\.ps1 differ' 'a record that names one tool fewer is refused'
Expect-Refusal (New-Case 'arm-started-with-a-tool-more' -Evidence { param($e) $e.tools['tools/other.ps1'] = '2' * 64 }) '\(tools/other\.ps1 differ' 'a record that names one tool more is refused'
Expect-Refusal (New-Case 'arm-started-naming-no-tools' -Evidence { param($e) $e.Remove('tools') }) 'does not name the tools that made it' 'a record of an earlier test_package_archive.ps1, which names no tools, is refused'
Expect-Refusal (New-Case 'arm-started-by-other-tools-words' -Evidence { param($e) $e.tools['tools/build_common.ps1'] = '0' * 64 }) 'Copy the export''s tools folder \(.*export\\tools\) to Windows on Arm' 'the refusal says which tools folder to run the checks from'
# (And the run's build record names the export's: what release_commands.ps1 holds the record to on the owner's PC.)
Expect-Refusal (New-Case 'run-naming-no-start-tools' { param($c) $c.Record.checks.Remove('arm64StartupTools') }) 'names no tools of the ARM64 start checks' 'a run whose build record names no tools of the start checks is refused'
Expect-Refusal (New-Case 'run-naming-other-start-tools' { param($c) $c.Record.checks.arm64StartupTools['tools/test_package_archive.ps1'] = '3' * 64 }) 'names other tools of the ARM64 start checks than the export''s' 'a run whose build record names other tools of the start checks is refused'
Expect-Refusal (New-Case 'export-tool-changed' { param($c) [IO.File]::AppendAllText("$($c.Export)/tools/build_common.ps1", '# changed') }) 'changes that are not committed' 'an export whose tools were changed since its commit is refused'
$broken = New-Case 'arm-started-broken'
[IO.File]::WriteAllText($broken.Evidence, 'not JSON')
Expect-Refusal $broken 'is no JSON' 'a record that is no JSON is refused'
$kept = New-Case 'arm-started-kept-other'
[IO.File]::WriteAllText("$($kept.Run)/arm64-startup.json", 'another record')
Expect-Refusal $kept 'is there already, and is another record' 'a run that keeps another record of the start checks is not given a second'
# (What test_package_archive.ps1 -Evidence writes: New-StartupEvidence, of a ZIP as package.ps1 makes it, on this PC.)
$thisTools = Get-StartupCheckTools $PSScriptRoot
$made = New-StartupEvidence -Zip $good.ArmZip -Manifest ([pscustomobject]@{ architecture = 'arm64'; version = '0.1.0' }) -Tools $thisTools
Check ($made.check -ceq 'tools/test_package_archive.ps1' -and $made.passed -eq $true -and $made.archive -ceq 'qKVM-Desktop-0.1.0-windows-arm64.zip' -and
    $made.sha256 -ceq (Get-Sha256 $good.ArmZip) -and $made.bytes -eq (Get-Item $good.ArmZip).Length -and $made.architecture -ceq 'arm64') 'the record of the checks names the ZIP by name, size and SHA-256, and its kind'
Check ($made.windows.machine -ceq (Get-NativeMachine) -and (Get-NativeMachine) -cin 'x64', 'arm64') "and the kind of Windows they ran on, as Windows says it ($($made.windows.machine))"
# (And the tools that ran them, of the tools folder they ran from: the start checks, the package's checks they call, what
# both take in and the table of protections, each by its SHA-256; review r2 of arm and armp, L2.)
$toolNames = @('tools/test_package_archive.ps1', 'tools/test_package.ps1', 'tools/build_common.ps1', 'tools/image-protections.json')
Check ((@($made.tools.Keys) -join ',') -ceq ($toolNames -join ',') -and @($toolNames | Where-Object { $made.tools[$_] -cne (Get-Sha256 (Join-Path $PSScriptRoot ($_ -replace '^tools/', ''))) }).Count -eq 0) "the record names the four tools of the checks by their SHA-256 ($(@($made.tools.Keys) -join ', '))"
$threw = ''
try { Get-StartupCheckTools (Join-Path $work 'no-tools-here') | Out-Null } catch { $threw = "$($_.Exception.Message)" }
Check ($threw -match 'There is no test_package_archive\.ps1 in') "a folder without the checks' tools names none ($threw)"
$archiveCheck = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'test_package_archive.ps1'))
$readAt = $archiveCheck.IndexOf('$tools = if ($Evidence) { Get-StartupCheckTools $PSScriptRoot }')
Check ($readAt -ge 0 -and $readAt -lt $archiveCheck.IndexOf('& "$PSScriptRoot/test_package.ps1" -PackageDirectory $package') -and
    $archiveCheck.Contains('New-StartupEvidence -Zip $Archive -Manifest $manifest -Tools $tools')) 'test_package_archive.ps1 reads the tools it runs from its own folder before its checks, and names them in its record'
$madeHere = Join-Path $work 'made-here.json'
$made | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $madeHere -Encoding utf8
$here = @(Get-Arm64StartupRefusals -Evidence $madeHere -Name 'qKVM-Desktop-0.1.0-windows-arm64.zip' -Bytes $made.bytes -Sha256 $made.sha256 -Tools $thisTools)
Check ((Get-NativeMachine) -ceq 'arm64' -or @($here | Where-Object { $_ -match 'not made on Windows on Arm' }).Count -eq 1) "a record made on this x64 PC is refused ($($here -join '; '))"
Check (@($here | Where-Object { $_ -match 'tools' }).Count -eq 0) "a record of this folder's tools is taken for them ($($here -join '; '))"

# --- Runs that are not released.
Expect-Refusal (New-Case 'trial' { param($c) $c.Record.trial = $true }) 'is a trial' 'a trial is not released'
Expect-Refusal (New-Case 'beta' { param($c) $c.Record.checks.betaAudit = 'refused'; $c.Record.checks.betaRefused = @('ffmpeg') }) 'beta audit did not pass: ffmpeg' 'a run its beta audit refused is not released'
Expect-Refusal (New-Case 'scan' { param($c) $c.Record.checks.privateScan = 'findings' }) 'byte scan did not pass' 'a run with findings is not released'
Expect-Refusal (New-Case 'unscanned' { param($c) $c.Record.checks.privateScan = 'not run' }) 'byte scan did not pass' 'a run not scanned is not released'
Expect-Refusal (New-Case 'other-zip' { param($c) [IO.File]::AppendAllText($c.Zip, 'x') }) 'x64 ZIP is not the one the build record names' 'a ZIP that is not the one built is refused'
# (Both kinds: a run of 0.2.0 or later is of x64's package and Windows on Arm's, each held to its own record.)
Expect-Refusal (New-Case 'schema-1' { param($c) $c.Record.schema = 1 }) 'not one this script knows' 'a record of a run before both kinds is refused'
Expect-Refusal (New-Case 'x64-alone' { param($c) $c.Record.packages = @($c.Record.packages[0]) }) 'not of one package of each kind of Windows \(it is of x64\)' 'a run of x64 alone is refused'
Expect-Refusal (New-Case 'x64-twice' { param($c) $c.Record.packages = @($c.Record.packages[0], $c.Record.packages[0]) }) 'not of one package of each kind' 'a run of x64 twice is refused'
Expect-Refusal (New-Case 'arm-kind-in-capitals' { param($c) $c.Record.packages[1].architecture = 'ARM64' }) 'not of one package of each kind' 'a package of a kind not written as the tools write it is refused'
Expect-Refusal (New-Case 'other-arm-zip' { param($c) [IO.File]::AppendAllText($c.ArmZip, 'x') }) 'arm64 ZIP is not the one the build record names' 'an ARM64 ZIP that is not the one built is refused'
# (Of the same size: what tells them apart is the SHA-256.)
Expect-Refusal (New-Case 'arm-zip-other-sha' { param($c) $c.Record.packages[1].sha256 = '0' * 64 }) 'arm64 ZIP is not the one the build record names' 'an ARM64 ZIP of the size built and other bytes is refused'
Expect-Refusal (New-Case 'zip-other-sha' { param($c) $c.Record.packages[0].sha256 = '0' * 64 }) 'x64 ZIP is not the one the build record names' 'an x64 ZIP of the size built and other bytes is refused'
Expect-Refusal (New-Case 'arm-zip-name' { param($c) $c.Record.packages[1].archive = 'qKVM-Desktop-0.1.0-windows-x64.zip' }) 'arm64 ZIP is not named qKVM-Desktop-0\.1\.0-windows-arm64\.zip' 'an ARM64 ZIP named as x64''s is refused'
Expect-Refusal (New-Case 'arm-zip-gone' { param($c) Remove-Item -LiteralPath $c.ArmZip }) 'arm64 ZIP is not the one the build record names' 'a run without its ARM64 ZIP is refused'
Expect-Refusal (New-Case 'arm-symbols' { param($c) Set-Zip $c @{ 'qkvm-desktop.exe' = 'p'; 'qkvm-desktop.pdb' = 's' } 'arm64' }) 'never handed out' 'an ARM64 ZIP with symbols is refused'
Expect-Refusal (New-Case 'arm-top-of-x64' { param($c) Set-Zip $c @{ 'qkvm-desktop.exe' = 'p' } 'arm64' 'qKVM-Desktop-0.1.0-windows-x64' }) 'outside its one folder' 'an ARM64 ZIP whose folder is x64''s is refused'
Expect-Refusal (New-Case 'ffmpeg-record-without-zlib' { param($c) $c.Record.dependencies.ffmpeg.record.sources = @($c.Record.dependencies.ffmpeg.record.sources[0]) }) 'does not name FFmpeg''s and zlib''s archives' 'an FFmpeg part''s record without zlib''s source is refused'
Expect-Refusal (New-Case 'ffmpeg-source-gone' { param($c) Remove-Item -LiteralPath "$($c.Root)/downloads/ffmpeg/zlib-1.3.2.tar.gz" }) 'zlib-1\.3\.2\.tar\.gz is not the archive' 'a run without its zlib source is refused'
Expect-Refusal (New-Case 'symbols' { param($c) Set-Zip $c @{ 'qkvm-desktop.exe' = 'p'; 'qkvm-desktop.pdb' = 's' } }) 'never handed out' 'a ZIP with symbols is refused'
Expect-Refusal (New-Case 'setup' { param($c) Set-Zip $c @{ 'qkvm-desktop.exe' = 'p'; 'qkvm-setup.exe' = 's' } }) 'program that is not handed out' 'a ZIP with a second program is refused'
Expect-Refusal (New-Case 'latest' { param($c) Set-Zip $c @{ 'qkvm-desktop.exe' = 'p'; 'build/latest.json' = '{}' } }) 'never handed out' 'a ZIP with latest.json is refused'
Expect-Refusal (New-Case 'no-program' { param($c) Set-Zip $c @{ 'THIRD-PARTY.md' = 'n' } }) 'no qkvm-desktop.exe' 'a ZIP without the program is refused'
Expect-Refusal (New-Case 'commit' { param($c) [IO.File]::WriteAllText("$($c.Export)/later.txt", 'later'); Invoke-Git $c.Export @('add', '-A'); Invoke-Git $c.Export @('commit', '-q', '-m', 'later') }) 'the run was built from' 'an export at another commit is refused'
Expect-Refusal (New-Case 'dirty' { param($c) [IO.File]::WriteAllText("$($c.Export)/stray.txt", 'stray') }) 'not committed' 'an export with changes is refused'
Expect-Refusal (New-Case 'full' { param($c) New-Item -ItemType Directory -Path $c.Out | Out-Null; [IO.File]::WriteAllText("$($c.Out)/old.txt", 'old') }) 'is not empty' 'an out folder with something in it is refused'
Expect-Refusal (New-Case 'ffmpeg' { param($c) [IO.File]::AppendAllText("$($c.Root)/downloads/ffmpeg/ffmpeg-9.0.2.tar.xz", 'x') }) 'ffmpeg-9.0.2.tar.xz is not the archive' 'an FFmpeg source that is not the one built from is refused'
Expect-Refusal (New-Case 'mbedtls' { param($c) [IO.File]::AppendAllText("$($c.Root)/src/.deps/webrtc-trial/mbedtls-3.6.7.tar.bz2", 'x') }) 'Mbed TLS archive is not' 'an Mbed TLS source that is not the one built from is refused'
Expect-Refusal (New-Case 'datachannel' { param($c) $c.Record.dependencies.webrtc.libdatachannel = '0' * 40 }) 'libdatachannel is at' 'libdatachannel at another commit is refused'
Expect-Refusal (New-Case 'found' { param($c) }) 'byte scan refused the release files' 'what the byte scan refuses is not released' @{ MADE_UP_SCAN = '1' }
# The setup: the one the run made and its static test passed, from where the run keeps it.
Expect-Refusal (New-Case 'no-setup' { param($c) $c.Record.Remove('setup') }) 'made no setup' 'a run without a setup is not released'
Expect-Refusal (New-Case 'setup-untested' { param($c) $c.Record.checks.setupTest = 'failed' }) 'setup''s static test did not pass' 'a run whose setup failed its test is not released'
Expect-Refusal (New-Case 'setup-other' { param($c) [IO.File]::AppendAllText($c.Setup, 'x') }) 'not the setup the build record names' 'a setup that is not the one built is refused'
Expect-Refusal (New-Case 'setup-same-size' { param($c) [IO.File]::WriteAllText($c.Setup, 'y' * (Get-Item $c.Setup).Length) }) 'not the setup the build record names' 'a setup of the same size and other bytes is refused'
Expect-Refusal (New-Case 'setup-elsewhere' { param($c) $c.Record.setup.file = '../../elsewhere/qKVM-Desktop-0.1.0-windows-setup.exe' }) 'not where the run keeps it' 'a setup the record names elsewhere is refused'
Expect-Refusal (New-Case 'setup-name' { param($c) $c.Record.setup.name = 'qkvm-setup.exe' }) 'not named qKVM-Desktop-0\.1\.0-windows-setup.exe' 'a setup of another name is refused'
# The submodules: each checked out at the commit libdatachannel records, and at the one the build record names.
Expect-Refusal (New-Case 'module-moved' { param($c) [IO.File]::WriteAllText("$($c.Datachannel)/deps/libjuice/later.c", 'later')
    Invoke-Git "$($c.Datachannel)/deps/libjuice" @('add', '-A'); Invoke-Git "$($c.Datachannel)/deps/libjuice" @('commit', '-q', '-m', 'later') }) 'deps/libjuice is not at the commit libdatachannel records \(\+\)' 'a submodule at another commit than libdatachannel records ("+") is refused'
Expect-Refusal (New-Case 'module-gone' { param($c) Invoke-Git $c.Datachannel @('submodule', 'deinit', '-q', '-f', 'deps/plog') }) 'deps/plog is not checked out \(-\), and the build uses it' 'a submodule the build uses that is not checked out ("-") is refused'
Expect-Refusal (New-Case 'module-other-commit' { param($c) $c.Record.dependencies.webrtc.submodules = @($c.Record.dependencies.webrtc.submodules | ForEach-Object { $_ -replace '^[0-9a-f]{40}( deps/libjuice)', ('1' * 40 + '$1') }) }) 'deps/libjuice is not as the build record names it' 'a submodule at another commit than the build record names is refused'
Expect-Refusal (New-Case 'module-not-recorded' { param($c) $c.Record.dependencies.webrtc.submodules = @($c.Record.dependencies.webrtc.submodules | Where-Object { $_ -notmatch ' deps/plog' }) }) 'deps/plog is not as the build record names it' 'a submodule the build record does not name is refused'
Expect-Refusal (New-Case 'module-recorded-moved' { param($c) $c.Record.dependencies.webrtc.submodules = @($c.Record.dependencies.webrtc.submodules | ForEach-Object { $_ -replace '^([0-9a-f]{40} deps/libjuice)', '+$1' }) }) 'deps/libjuice is not as the build record names it' 'a submodule the build had at another commit ("+" in its record) is refused'
Expect-Refusal (New-Case 'module-recorded-only' { param($c) $c.Record.dependencies.webrtc.submodules = @($c.Record.dependencies.webrtc.submodules) + ('2' * 40 + ' deps/extra (v1)') }) 'names deps/extra, which libdatachannel has not' 'a submodule only the build record names is refused'
Expect-Refusal (New-Case 'module-used-unknown' { param($c)
    [IO.File]::WriteAllText("$($c.Export)/experiments/webrtc/dependencies.json", '{"cpp": {"gitlinks": {"libjuice": "x", "plog": "y", "usrsctp": "z"}}}')
    Invoke-Git $c.Export @('add', '-A'); Invoke-Git $c.Export @('commit', '-q', '-m', 'manifest')
    $c.Record.source.commit = (& $git -C $c.Export rev-parse HEAD).Trim() }) 'deps/usrsctp, which the build uses, is not checked out as it was' 'a submodule the build uses that libdatachannel has not is refused'

# --- What goes into the third-party sources is what the build used (outside review 2, 5.1): every tree unpacked whole,
# and the bundle as written holds each archive by its size and SHA-256 and libdatachannel with its submodules as the tree
# each kind's WebRTC build read, by its digest; or nothing is made of it, and the refusal says which and what to do.
function Test-NothingMade($Context, [string] $Message) {
    Check (-not (Test-Path -LiteralPath "$($Context.Out)/SHA256SUMS") -and -not (Test-Path -LiteralPath "$($Context.Out)/qKVM-Desktop-0.1.0-third-party-sources.zip")) "$Message, and neither the third-party sources nor SHA256SUMS are made"
    Check (@(Get-ChildItem -LiteralPath $Context.Root -Force | Where-Object Name -like 'release-staging-*').Count -eq 0) "$Message, and nothing of the unpacking is left in the build folder"
}
# (A case's name is part of every path of its git objects, a submodule's deepest, which git here writes only below
# Windows' 260 signs: a case that commits in a submodule has a name no longer than module-moved's.)
# (Windows' tar cannot create a file whose name is longer than Windows takes, which git keeps and gives: it says so,
# unpacks the rest, and ends with exit 1. A name with a sign Windows does not take, git archive refuses here itself.)
$long = New-Case 'tar-fails' -Sources { param($d) Add-Entry $d ('src/' + ('x' * 300) + '.c') }
Expect-Refusal $long "(?-i)Windows' tar could not unpack all of libdatachannel \(tar\.exe exit 1: .*Can't create.*nothing is made of sources unpacked in part\. If the disk of .* is full, free room there and run this again, naming a new or empty -Out\. If tar cannot create one of the tree's files on Windows .*: the WebRTC sources are to be pinned \(build_common\.ps1's \`$WebRtcPins\) at commits whose trees Windows can hold, and the release built again from them" 'a libdatachannel that tar unpacks in part is refused, in tar''s words, with a way on for each cause'
Test-NothingMade $long 'a libdatachannel that tar unpacks in part is refused'
$longer = New-Case 'module-tar' -Sources { param($d) Add-ModuleEntry $d 'libjuice' ('src/' + ('y' * 300) + '.c') }
Expect-Refusal $longer "Windows' tar could not unpack all of libdatachannel/deps/libjuice at [0-9a-f]{40} \(tar\.exe exit 1: .*Can't create" 'a submodule that tar unpacks in part is refused, in tar''s words'
Test-NothingMade $longer 'a submodule that tar unpacks in part is refused'
# (git archive that fails partway, as on a full disk: here a commit whose tree names a file the repository has lost.
# git writes what it can of the tar, then says so and ends with an error.)
$lost = New-Case 'archive-fails' -Sources { param($d)
    Invoke-Git $d @('update-index', '--add', '--cacheinfo', '100644,0123456789abcdef0123456789abcdef01234567,src/lost.c')
    $tree = "$(& $git -C $d write-tree --missing-ok)".Trim()
    $commit = "$(& $git -C $d -c user.name=fixture -c user.email=fixture@example.invalid commit-tree $tree -p HEAD -m 'a file lost')".Trim()
    Invoke-Git $d @('update-ref', 'HEAD', $commit) }
Expect-Refusal $lost "(?-i)git archive could not write all of libdatachannel \(git exit \d+: .*0123456789abcdef0123456789abcdef01234567.*\), and nothing is made of sources written in part\. If the disk of .* is full, free room there and run this again, naming a new or empty -Out\. Otherwise git's words say what of the checkout it could not read\. Build the run again" 'a libdatachannel that git archive writes in part is refused, in git''s words, with what to do'
Test-NothingMade $lost 'a libdatachannel that git archive writes in part is refused'
# (Sources unpacked whole that are not the checkout the build read: a file committed that the checkout does not hold,
# and a file the checkout holds that is not committed; as a sparse checkout, or a file made in it, would leave them.)
$committed = New-Case 'holds-unread' -Sources { param($d) Add-ModuleEntry $d 'plog' 'src/later.c' }
Expect-Refusal $committed "(?-i)not what the build used, and were removed: its libdatachannel, with its submodules, is not the tree the x64 build was built from .*is not the tree the arm64 build was built from .*it holds deps/plog/src/later\.c, which the checkout has not\. Build the run again, .*; if the same refusal comes back, the checkout's commits do not give the tree the build read, and the release waits until they do" 'sources holding a file the build did not read are refused, naming the file, with what to do' -Without 'free room'
Test-NothingMade $committed 'sources holding a file the build did not read are refused'
# (The bundle written, and its check, that end in an error of their own, as a full disk or a file held open: the bundle
# is removed all the same. Here a file of the checkout, which the check reads where the bundle differs, is held open.)
$held = New-Case 'held-open' -Sources { param($d) [IO.File]::WriteAllText("$d/deps/plog/generated.h", 'made in the checkout') }
$open = [IO.File]::Open("$($held.Datachannel)/deps/plog/generated.h", [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
try {
    Expect-Refusal $held '(?-i)The third-party sources could not be written and checked \(.*\), and nothing is left of them in .*; then run this again, naming a new or empty -Out' 'sources whose check ends in an error are refused, with what to do'
} finally {
    $open.Dispose()
}
Test-NothingMade $held 'sources whose check ends in an error are refused'
$made = New-Case 'lacks-read' -Sources { param($d) [IO.File]::WriteAllText("$d/deps/plog/generated.h", 'made in the checkout') }
Expect-Refusal $made 'not the tree the x64 build was built from .*: it lacks deps/plog/generated\.h' 'sources lacking a file the build read are refused, naming the file'
Test-NothingMade $made 'sources lacking a file the build read are refused'
Expect-Refusal (New-Case 'record-of-other-tree' { param($c) $c.WebRtc.arm64.sources.libdatachannel.tree = 'a' * 64 }) 'is not the tree the arm64 build was built from \(its digest is [0-9a-f]{64}, the one that build''s record names a{64}\).*the checkout the build read .* file for file: that checkout is no longer the tree the build read, and was changed after the build, so the run is to be built again' 'sources that are not the tree a build''s record names are refused, saying the checkout changed after the build'
# (Each kind's WebRTC build record, where the run's build wrote it, the one its beta audit held its datachannel.dll to.)
Expect-Refusal (New-Case 'webrtc-record-gone' { param($c) $c.WebRtc.arm64 = $null }) '(?-i)There is no record of the arm64 WebRTC build at .*so none of it is offered\. Build the run again, and make its release files before another run is built in' 'a run whose ARM64 WebRTC build record is gone is refused'
Expect-Refusal (New-Case 'webrtc-record-not-audited' { param($c) $c.Audited.x64 = 'b' * 64 }) 'is not the record of the x64 WebRTC build that the run''s beta audit held its datachannel\.dll to .*build the run again' 'a WebRTC build record that is not the one the beta audit held is refused'
Expect-Refusal (New-Case 'webrtc-record-other-sources' { param($c)
    $c.WebRtc.x64.architecture = 'arm64'; $c.WebRtc.x64.sources.libdatachannel.commit = '0' * 40
    $c.WebRtc.x64.sources.libdatachannel.gitlinks.libjuice = '1' * 40; $c.WebRtc.x64.sources.mbedtls.archiveSha256 = '2' * 64 }) '(?-i)The record of the x64 WebRTC build .* is not of the sources this run names: it is of a build for arm64; it names libdatachannel at 0{40}, the build record [0-9a-f]{40}; it names deps/libjuice at 1{40}, checked out at [0-9a-f]{40}; it names Mbed TLS''s archive 2{64}, the build record [0-9a-f]{64}\. Build the run again' 'a WebRTC build record of other sources than the run names is refused, saying each'
Expect-Refusal (New-Case 'webrtc-record-other-modules' { param($c)
    $c.WebRtc.arm64.sources.libdatachannel.gitlinks.Remove('plog'); $c.WebRtc.arm64.sources.libdatachannel.gitlinks['usrsctp'] = '3' * 40 }) 'The record of the arm64 WebRTC build .* is not of the sources this run names: it names deps/usrsctp, which is not checked out; it does not name deps/plog, which is' 'a WebRTC build record of other submodules than are checked out is refused, saying which'
Expect-Refusal (New-Case 'webrtc-record-no-tree' { param($c) $c.WebRtc.x64.sources.libdatachannel.tree = 'not a digest' }) 'The record of the x64 WebRTC build .* is not of the sources this run names: it names no tree of libdatachannel' 'a WebRTC build record that names no tree is refused'
Expect-Refusal (New-Case 'webrtc-record-of-another-part' { param($c) $c.WebRtc.arm64.part = 'ffmpeg' }) 'The record of the arm64 WebRTC build .* is not of the sources this run names: it is no record of a WebRTC build' 'a record of another part where the WebRTC build''s is is refused'
# (FFmpeg's and zlib's archives: as the FFmpeg part's record names them, by size and SHA-256, the ones both kinds were
# built from.)
Expect-Refusal (New-Case 'ffmpeg-built-from-other' { param($c) $c.Record.dependencies.ffmpeg.record.builds[1].pins.zlib = "zlib-1.3.1.tar.gz 5 $('0' * 64)" }) 'The FFmpeg part''s record does not say its arm64 FFmpeg was built from zlib-1\.3\.2\.tar\.gz as its sources name it .*Build the FFmpeg part again \(build_release\.ps1 -Part ffmpeg, twice, with the same -Records\)' 'an archive the FFmpeg part''s record does not say a kind was built from is refused, with how a release''s FFmpeg is built'
Expect-Refusal (New-Case 'ffmpeg-source-other-size' { param($c)
    $ffmpegRecord = $c.Record.dependencies.ffmpeg.record
    $ffmpegRecord.sources[0].bytes = 1
    foreach ($build in $ffmpegRecord.builds) { $build.pins.ffmpeg = "ffmpeg-9.0.2.tar.xz 1 $($ffmpegRecord.sources[0].sha256)" } }) '(?-i)ffmpeg-9\.0\.2\.tar\.xz is not the archive the build record names .*\. Build the run again from its FFmpeg part, .*\(each run''s build part copies its FFmpeg part''s archives there\)' 'an FFmpeg source of another size than the record names is refused, with why the run is built again' -Without 'src anew'
# The bundle as written (build_common.ps1's Get-SourcesBundleRefusals), on ZIPs made here: at its top README.txt and each
# archive by its size and SHA-256, libdatachannel/ the tree the build read by Get-SourceTreeDigest's rules, nothing else.
$bundles = Join-Path $work 'bundles'
$checkout = Join-Path $bundles 'checkout'
foreach ($file in @(@('src/a.c', "int a;`r`n"), @('src/b.c', 'int b;'), @('deps/plog/LICENSE', 'made-up'), @('.git/HEAD', 'ref: x'), @('deps/plog/.git', 'gitdir: x'))) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent (Join-Path $checkout $file[0])) | Out-Null
    [IO.File]::WriteAllText((Join-Path $checkout $file[0]), $file[1])
}
$tree = Get-SourceTreeDigest $checkout
$archiveBytes = [Text.Encoding]::ASCII.GetBytes('made-up archive')
$archives = @([pscustomobject]@{ name = 'zlib-1.3.2.tar.gz'; bytes = $archiveBytes.Length; sha256 = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($archiveBytes)).ToLowerInvariant() })
$builtFrom = [ordered]@{ 'the x64 build' = $tree; 'the arm64 build' = $tree }
$wholeBundle = [ordered]@{ 'README.txt' = 'what each is'; 'zlib-1.3.2.tar.gz' = 'made-up archive'; 'libdatachannel/src/a.c' = "int a;`n"; 'libdatachannel/src/b.c' = 'int b;'
    'libdatachannel/deps/plog/LICENSE' = 'made-up'; 'libdatachannel/deps/json/' = $null }
function New-Bundle([string] $Name, [Collections.IDictionary] $Entries) {
    $zip = Join-Path $bundles "$Name.zip"
    $archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($entry in $Entries.GetEnumerator()) {
            $made = $archive.CreateEntry(($entry.Key -replace '#\d+$', ''))
            if ($null -ne $entry.Value) { $stream = $made.Open(); $bytes = [Text.Encoding]::ASCII.GetBytes($entry.Value); $stream.Write($bytes, 0, $bytes.Length); $stream.Dispose() }
        }
    } finally { $archive.Dispose() }
    return $zip
}
function Get-BundleRefusals([string] $Name, [scriptblock] $Change = {}) {
    $entries = [ordered]@{}; foreach ($entry in $wholeBundle.GetEnumerator()) { $entries[$entry.Key] = $entry.Value }
    & $Change $entries
    return @(Get-SourcesBundleRefusals -Bundle (New-Bundle $Name $entries) -Archives $archives -Trees $builtFrom -Checkout $checkout)
}
if (-not (Get-Command Get-SourcesBundleRefusals -ErrorAction SilentlyContinue)) {
    Check $false 'build_common.ps1 holds the check of the third-party sources as written (Get-SourcesBundleRefusals)'
} else {
    $said = @(Get-BundleRefusals 'whole')
    Check ($said.Count -eq 0) "a bundle of the archives and of the tree the build read (its line ends as the checkout's, read as LF) is taken ($($said -join '; '))"
    $said = @(Get-BundleRefusals 'no-readme' { param($e) $e.Remove('README.txt') })
    Check ($said.Count -eq 1 -and $said[0] -match '\Ait holds no README\.txt\z') "a bundle without its README is refused ($($said -join '; '))"
    $said = @(Get-BundleRefusals 'archive-gone' { param($e) $e.Remove('zlib-1.3.2.tar.gz') })
    Check ($said.Count -eq 1 -and $said[0] -match '\Ait holds no zlib-1\.3\.2\.tar\.gz\z') "a bundle without one of its archives is refused ($($said -join '; '))"
    $said = @(Get-BundleRefusals 'archive-other-bytes' { param($e) $e['zlib-1.3.2.tar.gz'] = 'made-up archivf' })
    Check ($said.Count -eq 1 -and $said[0] -match '\Aits zlib-1\.3\.2\.tar\.gz is not the archive the build used \(its size or its SHA-256 differs\)\z') "an archive of other bytes of the same size is refused ($($said -join '; '))"
    $said = @(Get-BundleRefusals 'archive-shorter' { param($e) $e['zlib-1.3.2.tar.gz'] = 'made-up' })
    Check ($said.Count -eq 1 -and $said[0] -match 'is not the archive the build used') "an archive cut short is refused ($($said -join '; '))"
    $said = @(Get-BundleRefusals 'archive-twice' { param($e) $e['zlib-1.3.2.tar.gz#2'] = 'made-up archive' })
    Check ($said.Count -eq 1 -and $said[0] -match '\Ait holds zlib-1\.3\.2\.tar\.gz twice\z') "a bundle holding an archive twice is refused ($($said -join '; '))"
    $said = @(Get-BundleRefusals 'more-at-top' { param($e) $e['notes.txt'] = 'more' })
    Check ($said.Count -eq 1 -and $said[0] -match '\Ait holds notes\.txt, which is none of the sources\z') "a bundle holding a file more at its top is refused ($($said -join '; '))"
    $said = @(Get-BundleRefusals 'git-inside' { param($e) $e['libdatachannel/deps/plog/.git'] = 'gitdir: x' })
    Check ($said.Count -eq 1 -and $said[0] -match '\Ait holds libdatachannel/deps/plog/\.git, which is git''s own and no source\z') "a bundle holding git's own files is refused ($($said -join '; '))"
    $said = @(Get-BundleRefusals 'file-fewer' { param($e) $e.Remove('libdatachannel/src/b.c') })
    Check ($said.Count -eq 3 -and $said[0] -match '\Aits libdatachannel, with its submodules, is not the tree the x64 build was built from \(its digest is [0-9a-f]{64}, the one that build''s record names [0-9a-f]{64}\)\z' -and
        $said[1] -match 'not the tree the arm64 build was built from' -and $said[2] -match '\Awhere it differs from the checkout the build read \(.*checkout\): it lacks src/b\.c\z') "a bundle that lacks a file of the tree is refused, naming it ($($said -join '; '))"
    $said = @(Get-BundleRefusals 'file-more' { param($e) $e['libdatachannel/deps/plog/extra.h'] = 'more' })
    Check ($said.Count -eq 3 -and $said[2] -match ': it holds deps/plog/extra\.h, which the checkout has not\z') "a bundle with a file more in the tree is refused, naming it ($($said -join '; '))"
    $said = @(Get-BundleRefusals 'file-changed' { param($e) $e['libdatachannel/src/a.c'] = "int A;`n" })
    Check ($said.Count -eq 3 -and $said[2] -match ': it holds src/a\.c with other bytes than the checkout''s\z') "a bundle with a file of other bytes in the tree is refused, naming it ($($said -join '; '))"
    $said = @(Get-BundleRefusals 'many-fewer' { param($e) foreach ($name in 'src/a.c', 'src/b.c', 'deps/plog/LICENSE') { $e.Remove("libdatachannel/$name") }
        foreach ($n in 1..7) { $e["libdatachannel/more/$n.c"] = "$n" } })
    Check ($said.Count -eq 3 -and $said[2] -match ': it lacks deps/plog/LICENSE, src/a\.c, src/b\.c; it holds more/1\.c, more/2\.c, more/3\.c, more/4\.c, more/5\.c and 2 more, which the checkout has not\z') "a bundle that differs in many files names the first of each kind and counts the rest ($($said -join '; '))"
    $other = [ordered]@{ 'the x64 build' = $tree; 'the arm64 build' = 'f' * 64 }
    $entries = [ordered]@{}; foreach ($entry in $wholeBundle.GetEnumerator()) { $entries[$entry.Key] = $entry.Value }
    $said = @(Get-SourcesBundleRefusals -Bundle (New-Bundle 'one-other-tree' $entries) -Archives $archives -Trees $other -Checkout $checkout)
    Check ($said.Count -eq 2 -and $said[0] -match 'not the tree the arm64 build was built from \(its digest is [0-9a-f]{64}, the one that build''s record names f{64}\)' -and
        $said[1] -match '\Ait is the checkout the build read \(.*checkout\) file for file: that checkout is no longer the tree the build read, and was changed after the build, so the run is to be built again\z') "a bundle of the checkout as it is, which one build did not read, is refused, with what to do ($($said -join '; '))"
}
# (A git that fails says nothing on its output: a git status that fails is not an export without changes, and each
# git that fails is said as such, not as what its empty answer would mean.)
Expect-Refusal (New-Case 'export-status-fails' { param($c) [IO.File]::WriteAllText("$($c.Export)/.git/index", 'not an index') }) 'git status of the export failed \(git exit \d+\)' 'an export whose git status fails is refused'
function Set-NoRepository([string] $Folder) {
    Remove-Item -LiteralPath "$Folder/.git" -Recurse -Force
    [IO.File]::WriteAllText("$Folder/.git", "gitdir: $Folder/no-such-repository")
}
Expect-Refusal (New-Case 'export-not-a-repository' { param($c) Set-NoRepository $c.Export }) 'git could not say the export''s commit \(git exit \d+\): name with -Export the export''s folder' 'an export that git cannot read is refused as such'
Expect-Refusal (New-Case 'datachannel-not-a-repository' { param($c) Set-NoRepository $c.Datachannel }) '(?-i)git could not say libdatachannel''s commit in .* \(git exit \d+\)\. Build the run again' 'a libdatachannel that git cannot read is refused as such'
Expect-Refusal (New-Case 'export-without-manifest' { param($c)
    Invoke-Git $c.Export @('rm', '-q', 'experiments/webrtc/dependencies.json'); Invoke-Git $c.Export @('commit', '-q', '-m', 'no manifest')
    $c.Record.source.commit = (& $git -C $c.Export rev-parse HEAD).Trim() }) 'git could not show experiments/webrtc/dependencies\.json of the export''s commit \(git exit \d+\)' 'an export whose commit holds no WebRTC manifest is refused as such'

# --- The allowlist itself.
$folder = Join-Path $work 'allowlist'
function Reset-Folder([string[]] $Names) {
    if (Test-Path -LiteralPath $folder) { Remove-Item -LiteralPath $folder -Recurse -Force }
    New-Item -ItemType Directory -Path $folder | Out-Null
    foreach ($name in $Names) { [IO.File]::WriteAllText((Join-Path $folder $name), 'x') }
}
function Refuses([scriptblock] $Action, [string] $Expected, [string] $Message) {
    try { & $Action; Check $false "$Message (it was accepted)" }
    catch { Check ($_.Exception.Message -match $Expected) "$Message (refused for another reason: $($_.Exception.Message))" }
}
$five = Get-ReleaseFileNames '0.1.0'
Check ($five -ccontains 'qKVM-Desktop-0.1.0-windows-setup.exe' -and $five -ccontains 'qKVM-Desktop-0.1.0-windows-arm64.zip' -and $five.Count -eq 6) "the release files are six, the setup and both ZIPs among them ($($five -join ', '))"
Reset-Folder $five
try { Assert-ReleaseFolder $folder '0.1.0' } catch { Check $false "the six are a release ($($_.Exception.Message))" }
foreach ($stray in 'qkvm-setup.exe', 'qkvm-desktop.exe', 'qKVM-Desktop-0.1.1-windows-setup.exe', 'qKVM-Desktop-0.1.0-windows-x64-setup.exe', 'qKVM-Desktop-0.1.0-windows-setup-2.exe',
    'qKVM-Desktop-0.1.0-windows-arm64-setup.exe', 'qKVM-Desktop-0.1.1-windows-arm64.zip', 'qKVM-Desktop-0.1.0-windows-aarch64.zip',
    'qkvm-desktop.pdb', 'latest.json', 'latest-arm64.json', 'LATEST.JSON', 'notes.txt', 'SHA256SUMS.sig') {
    Reset-Folder ($five + $stray)
    Refuses { Assert-ReleaseFolder $folder '0.1.0' } 'release file' "$stray is refused"
}
Reset-Folder ($five + 'SHA256SUMS.sig')
try { Assert-ReleaseFolder $folder '0.1.0' -Signed } catch { Check $false "the owner's signature is taken where it is asked for ($($_.Exception.Message))" }
foreach ($left in $five) {
    Reset-Folder ($five | Where-Object { $_ -cne $left })
    Refuses { Assert-ReleaseFolder $folder '0.1.0' } 'lacks' "a release without $left is refused"
}
Reset-Folder $five
New-Item -ItemType Directory -Path (Join-Path $folder 'symbols') | Out-Null
Refuses { Assert-ReleaseFolder $folder '0.1.0' } 'no folder' 'a folder among the release files is refused'

if ($failures) { Write-Host "$failures release files check(s) failed."; exit 1 }
Remove-Item -LiteralPath $work -Recurse -Force
Write-Output 'Release files checks passed: the six files of a run of both kinds that passed, and every run, ZIP, export and folder that is refused.'
