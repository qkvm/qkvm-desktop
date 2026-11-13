#Requires -Version 7.2
<#
tools/release_files.ps1 on a made-up run (a record, its ZIP and setup, the sources it was built from, an export): the five
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
function New-Zip([string] $Zip, [hashtable] $Files) {
    $folder = Join-Path $work "zip-$([guid]::NewGuid().ToString('N'))"
    $top = Join-Path $folder 'qKVM-Desktop-0.1.0-windows-x64'
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
$templateCommit = New-Datachannel $template
$scanner = Join-Path $work 'scan.py'
[IO.File]::WriteAllText($scanner, "import os, sys`nsys.exit(int(os.environ.get('MADE_UP_SCAN', '0')))`n")

# One made-up build folder, run and export, made anew for each case.
function New-Case([string] $Name, [scriptblock] $Change = {}) {
    $case = Join-Path $work $Name
    $root = Join-Path $case 'root'
    New-Item -ItemType Directory -Force -Path "$root/downloads/ffmpeg", "$root/src/.deps/webrtc-trial", "$root/runs/20260101-000000" | Out-Null
    [IO.File]::WriteAllText("$root/.qkvm-release-root", 'made by build_release.ps1')
    [IO.File]::WriteAllText("$root/downloads/ffmpeg/ffmpeg-9.0.2.tar.xz", 'made-up ffmpeg source')
    [IO.File]::WriteAllText("$root/downloads/ffmpeg/zlib-1.3.2.tar.gz", 'made-up zlib source')
    [IO.File]::WriteAllText("$root/downloads/ffmpeg/make_4.3_amd64.deb", 'a tool, never a release file')
    [IO.File]::WriteAllText("$root/src/.deps/webrtc-trial/mbedtls-3.6.7.tar.bz2", 'made-up mbedtls source')
    $datachannelFolder = "$root/src/.deps/webrtc-trial/libdatachannel"
    Copy-Item -LiteralPath $template -Destination $datachannelFolder -Recurse -Force
    $datachannel = $templateCommit
    $submodules = @(& $git -C $datachannelFolder submodule status | ForEach-Object { "$_".Trim() })
    $export = Join-Path $case 'export'
    $commit = New-Repository $export @{ 'CMakeLists.txt' = 'project(QkvmDesktop VERSION 0.1.0 LANGUAGES CXX)'; 'tools/a.ps1' = "# a`n# b`n"
        'experiments/webrtc/dependencies.json' = $manifest }
    # (As this PC's git is set: line ends turned to Windows' on the way out.)
    & $git -C $export config core.autocrlf true
    $run = "$root/runs/20260101-000000"
    $zip = "$run/qKVM-Desktop-0.1.0-windows-x64.zip"
    New-Zip $zip @{ 'qkvm-desktop.exe' = 'made-up program'; 'THIRD-PARTY.md' = 'notices'; 'plugins/platforms/qwindows.dll' = 'plugin' }
    # The setup the run made from its package (tools/build_setup.ps1), where build_release.ps1 leaves it.
    New-Item -ItemType Directory -Path "$run/setup" | Out-Null
    $setup = "$run/setup/qKVM-Desktop-0.1.0-windows-x64-setup.exe"
    [IO.File]::WriteAllText($setup, 'made-up setup, never run')
    $record = [ordered]@{
        schema = 1; version = '0.1.0'; trial = $false
        source = [ordered]@{ commit = $commit }
        updates = [ordered]@{ publicKey = New-ThrowawayPublicKey }
        package = [ordered]@{ archive = 'qKVM-Desktop-0.1.0-windows-x64.zip'; bytes = (Get-Item $zip).Length; sha256 = Get-Sha256 $zip }
        dependencies = [ordered]@{
            ffmpeg = [ordered]@{
                ffmpeg = [ordered]@{ archive = 'ffmpeg-9.0.2.tar.xz'; sha256 = Get-Sha256 "$root/downloads/ffmpeg/ffmpeg-9.0.2.tar.xz" }
                zlib = [ordered]@{ archive = 'zlib-1.3.2.tar.gz'; sha256 = Get-Sha256 "$root/downloads/ffmpeg/zlib-1.3.2.tar.gz" } }
            webrtc = [ordered]@{ libdatachannel = $datachannel; submodules = $submodules
                mbedtls = Get-Sha256 "$root/src/.deps/webrtc-trial/mbedtls-3.6.7.tar.bz2" }
        }
        setup = [ordered]@{ name = 'qKVM-Desktop-0.1.0-windows-x64-setup.exe'; file = 'setup/qKVM-Desktop-0.1.0-windows-x64-setup.exe'
            bytes = (Get-Item $setup).Length; sha256 = Get-Sha256 $setup }
        checks = [ordered]@{ betaAudit = 'accepted'; betaRefused = @(); privateScan = 'clean'; setupTest = 'passed' }
    }
    $context = [pscustomobject]@{ Root = $root; Run = $run; Zip = $zip; Export = $export; Record = $record; Out = "$case/out"
        Datachannel = $datachannelFolder; Setup = $setup }
    & $Change $context
    $context.Record | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$run/build-record.json" -Encoding utf8
    return $context
}
function Invoke-Files($Context, [hashtable] $Environment = @{}) {
    $saved = @{}
    foreach ($name in $Environment.Keys) { $saved[$name] = [Environment]::GetEnvironmentVariable($name); [Environment]::SetEnvironmentVariable($name, $Environment[$name]) }
    try {
        $said = & (Get-Process -Id $PID).Path -NoProfile -NonInteractive -File "$PSScriptRoot/release_files.ps1" -Run $Context.Run `
            -Export $Context.Export -Out $Context.Out -Scanner $scanner 2>&1
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
function Expect-Refusal($Context, [string] $Expected, [string] $Message, [hashtable] $Environment = @{}) {
    $result = Invoke-Files $Context $Environment
    Check ($result.Ended -ne 0 -and $result.Said -match $Expected) "$Message (exit $($result.Ended): $($result.Said))"
}
function Set-Zip($Context, [hashtable] $Files) {
    New-Zip $Context.Zip $Files
    $Context.Record.package.bytes = (Get-Item $Context.Zip).Length
    $Context.Record.package.sha256 = Get-Sha256 $Context.Zip
}

# --- A run that is a release.
$good = New-Case 'good'
$result = Invoke-Files $good
Check ($result.Ended -eq 0) "a run that passed is released ($($result.Said))"
$names = @(Get-ChildItem -LiteralPath $good.Out -Force | ForEach-Object Name | Sort-Object -CaseSensitive)
Check (($names -join '|') -ceq ((Get-ReleaseFileNames '0.1.0' | Sort-Object -CaseSensitive) -join '|') -and $names.Count -eq 5) "the five release files and nothing else ($($names -join ', '))"
Check ((Get-Sha256 "$($good.Out)/qKVM-Desktop-0.1.0-windows-x64.zip") -ceq $good.Record.package.sha256) 'the ZIP is the one built'
Check ((Get-Sha256 "$($good.Out)/qKVM-Desktop-0.1.0-windows-x64-setup.exe") -ceq $good.Record.setup.sha256) 'the setup is the one built'
$sums = [IO.File]::ReadAllText("$($good.Out)/SHA256SUMS")
$wanted = ((@('qKVM-Desktop-0.1.0-source.zip', 'qKVM-Desktop-0.1.0-third-party-sources.zip', 'qKVM-Desktop-0.1.0-windows-x64.zip',
    'qKVM-Desktop-0.1.0-windows-x64-setup.exe') | Sort-Object -CaseSensitive | ForEach-Object { "$(Get-Sha256 "$($good.Out)/$_")  $_`n" }) -join '')
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
Check (@($listed.files).Count -eq 5 -and $listed.commit -ceq $good.Record.source.commit) 'release-files.json names the five and the commit'

# --- Runs that are not released.
Expect-Refusal (New-Case 'trial' { param($c) $c.Record.trial = $true }) 'is a trial' 'a trial is not released'
Expect-Refusal (New-Case 'beta' { param($c) $c.Record.checks.betaAudit = 'refused'; $c.Record.checks.betaRefused = @('ffmpeg') }) 'beta audit did not pass: ffmpeg' 'a run its beta audit refused is not released'
Expect-Refusal (New-Case 'scan' { param($c) $c.Record.checks.privateScan = 'findings' }) 'byte scan did not pass' 'a run with findings is not released'
Expect-Refusal (New-Case 'unscanned' { param($c) $c.Record.checks.privateScan = 'not run' }) 'byte scan did not pass' 'a run not scanned is not released'
Expect-Refusal (New-Case 'other-zip' { param($c) [IO.File]::AppendAllText($c.Zip, 'x') }) 'not the one the build record names' 'a ZIP that is not the one built is refused'
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
Expect-Refusal (New-Case 'setup-elsewhere' { param($c) $c.Record.setup.file = '../../elsewhere/qKVM-Desktop-0.1.0-windows-x64-setup.exe' }) 'not where the run keeps it' 'a setup the record names elsewhere is refused'
Expect-Refusal (New-Case 'setup-name' { param($c) $c.Record.setup.name = 'qkvm-setup.exe' }) 'not named qKVM-Desktop-0\.1\.0-windows-x64-setup\.exe' 'a setup of another name is refused'
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
Check ($five -ccontains 'qKVM-Desktop-0.1.0-windows-x64-setup.exe' -and $five.Count -eq 5) "the release files are five, the setup among them ($($five -join ', '))"
Reset-Folder $five
try { Assert-ReleaseFolder $folder '0.1.0' } catch { Check $false "the five are a release ($($_.Exception.Message))" }
foreach ($stray in 'qkvm-setup.exe', 'qkvm-desktop.exe', 'qKVM-Desktop-0.1.1-windows-x64-setup.exe', 'qKVM-Desktop-0.1.0-windows-x64-setup-2.exe',
    'qkvm-desktop.pdb', 'latest.json', 'LATEST.JSON', 'notes.txt', 'SHA256SUMS.sig') {
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
Write-Output 'Release files checks passed: the five files of a run that passed, and every run, ZIP, export and folder that is refused.'
