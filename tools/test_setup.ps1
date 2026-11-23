#Requires -Version 7.2
<#
The setup's static test: what tools/build_setup.ps1 made, read and never run.

    pwsh -File tools\test_setup.ps1 -SetupDirectory <what build_setup.ps1 made> -PackageDirectory <its package>
    pwsh -File tools\test_setup.ps1                  (its own cases alone)

First its own cases, on made-up packages in a folder of its own, compiled with the Inno Setup of .deps\inno
(tools/fetch_inno.ps1; -Inno names another folder of it): a package of a few files is made into a setup that
passes, and then each way of being wrong is seen to fail, one at a time, so that a check that refuses nothing is
caught. build_setup.ps1 refuses a package with a file its manifest does not name, with a file of other bytes or
without one it names, with a name Inno Setup would read as something else, an output folder that is not new, and
an Inno Setup that is not the pinned one. The checks below fail a setup with a file more or less than the package,
one of other bytes or put elsewhere, one that asks for more than the user, a script other than the one committed,
another mutex, no marker or another one, other compression, something else run, the network asked, and a setup
whose own manifest asks for an administrator.

Then, with -SetupDirectory, the setup made from the package named:
- the script compiled, and the script is packaging/qkvm-desktop.iss as committed;
- the compiler's own list of what it put in (setup-files.txt) is the package's manifest, file for file and by
  SHA-256, with the manifest itself and the marker and nothing else, and files.iss puts each at its own place;
- PrivilegesRequired=lowest, with nothing that overrides it, and the setup's own manifest asks for nothing more
  (asInvoker);
- AppMutex is the program's single-instance mutex, as ui/main.cpp and ui/win/single_instance.cpp make it, and the
  update mode of the contract is there: /UPDATE=1, 60 s, exit code 105, the program started again; and the setup holds
  its own mutex, qKVM-Desktop-Setup (the program waits for it as Local\qKVM-Desktop-Setup); in update mode, before
  anything is changed, the room for every file and every file of the folder free to be replaced (Preparing to
  Install, exit code 7), in every mode; and an older version over a newer one refused (the installed DisplayVersion,
  compared);
- a running copy closed for the person (0.1.2): outside update mode the setup's start does not refuse for the
  mutex, and gives it once installing (the uninstall log keeps it: the uninstaller refuses); the copy of {app} found by the window of its icon (ui/win/tray.cpp's class)
  and taken only where its process's file is {app}\qkvm-desktop.exe; asked by the program's event
  (ui/win/quit_requests.h's name, in this session's names, with the right to set it alone); its answer read only
  from a window of that process; about 10 s waited, 60 s once it says it is ending; the forced end in one place,
  reached only from End it now and a silent install's /FORCECLOSE=1; Cancel back to the Ready page; a silent
  install's refusal through Preparing to Install; the first page naming the installed version and this one, no
  tasks page on an upgrade, and "Start qKVM Desktop" ticked after a first install and where it closed the program;
  room looked at before a running copy is closed, and what it closed and then changed nothing for started again as it
  ends, and an update's own start only at the very end of the post-install step, once its copy is flushed and marked
  finished, never from [Run], every entry of which is postinstall (the two starts of a program in [Code]: the
  program, with --after-update); an earlier install's kind of Windows from its program's PE header; nothing that runs before the Ready
  page reaches {app}, read as Pascal Script reads [Code] (a name in any case, an indented or <event>'s routine,
  {code:} in a string, a constant) and with what [Setup], [Tasks] and [Files] ask by {code:} and Check:;
- the marker is "setup" and a line feed, put at {app}\qkvm-desktop.installed;
- the fixed AppId, publisher qKVM, the package's version, wherever Windows runs x64 programs (x64compatible),
  Windows 10 22H2, lzma2 and solid, no closing of programs, the user's own Programs folder;
- Windows' junction guard left on (RedirectionGuard is never turned off); before an install nothing deleted but
  {app}\plugins and {app}\qml, which a later version may hold less of and Qt loads whole; the folder never chosen
  in the wizard (DisableDirPage=yes); the calls outside Pascal Script each as declared, ExitProcess and those that
  close a running copy (for Setup alone), and no other;
- the program's own file the last one put in (files.iss's last line, after the marker);
- qKVM Setup's update check (0.2.0, review i020-1 M3: -UpdateCheck, a Windows program, the bytes setup-record.json
  names) carried first in [Files] and never copied as such: unpacked once, put in the program's place right after the
  program is set aside and before any library, only where nothing is there, under its draft's name first, flushed and
  renamed into place (review i020-2, L4); a put-back puts it there first and the earlier program back last, and removes
  what the copy made that the earlier version had not (the record's copy lines); where the earlier program is gone since
  it was set aside, the update check at its name, by its bytes, is taken away, and the earlier version is never said to
  be whole then (L3);
  build_setup.ps1 refuses an update check that is no Windows program, or none;
- the earlier version set aside for the copy (0.2.0): the package's own list of its files (package.iss, made at build
  time, the program's own first), the marker of an unfinished update as the program names it
  (ui/win/unfinished_update.h) written before the first file is set aside, in every mode, last in Preparing to Install,
  a file that cannot be set aside a refusal with the rest put back; after a whole copy the marker renamed first, then
  what was set aside removed; put back when the copy did not complete; and every setup first puts right what an update
  that did not finish left, before its refusal of an older version (a file set aside of another name refused, never
  touched); the one file the [Code] section writes is that marker;
- only a folder of its own (the contract's marker there, of its six bytes) or one not there or empty, or one that holds
  nothing but what a setup of ours leaves (the markers whose records read, the files they name with their endings,
  folders of the setup's own names), installed into (review i020-2, M1; reviews 1.1 and 1.8): any other refused in
  Preparing to Install before anything is closed or changed, whatever its names, by a look at its entries that removes
  nothing; nothing set aside, nor the program's own file of 0.1.2 put right, where no install of ours is; a record
  decides only in a folder of its own, decided first, and where no install of ours is there what the records name is
  removed and nothing put back, never a file of the package's own name; and at the setup's start only in a folder that
  no other account may change, its rights judged before anything there is put right or looked for, and a refusal for
  them saying that what an update left there stays as it is, and what to do (outside review 2, 1.3);
- a copy one of whose files Windows could not flush to the disk (tried three times) not whole, and nothing set aside
  removed then (review 1.6); nothing started or offered at the end where the copy does not count and left nothing whole,
  a first install's (review 1.7); before the copy is judged, the starts of the program made during it waited for, a
  minute at the most, every process whose file is in the program's folder by Windows' list of processes (the VM suite, 2026-11-21: such a
  start holds the program's file against the flush, and Windows' scanner can hold the first start of a copy just
  written for seconds); what a finishing update could not remove yet left to its record by the in-use look, never in a
  setup's way;
- a put-back whole only where every file its record names was seen (a look Windows refuses is never "not there") and
  every file only the copy made removed; else it stops with the marker and its record kept (review 1.5); finishing and
  the uninstall keep what they cannot look at named alike;
- and only into a folder that this Windows account and Windows itself alone may change (review 1.2): the folder, or
  the one it is made in, by its owner and every entry of its list (of the one it is made in, those the new folder or
  the files made in it take, outside review 2, 1.2), and each folder above it by its owner and the entries that let
  another account remove, rename or take it; a redirection and a network share refused; asked in Preparing to Install
  and at Install before anything is closed; no rights ever written; this account read as Windows' own pointer in its
  token's user says, of whatever authority (an account of Microsoft Entra ID's S-1-12-1, outside review 2, 1.1);
- no folder walked, plugins and qml among them (review i020-1, M4); after a whole copy marked finished, what the
  version of ours installed in that very folder before (Settings > Apps' entry names the folder and the version, and
  the contract's marker is there) installed and this one has not, each by its exact name from that version's own list
  (earlier.iss, made at build time from packaging/published-files.json or -PublishedFiles, the versions before the
  package's), a plain file through no redirection, then their folders where emptied, the deepest first
  (build_setup.ps1 refuses a list that names what no package of ours can have); after a whole copy, after a whole put-back of an update that did not finish, and at an uninstall, what a setup ended halfway left in the package's own folders (by
  its list, folders.iss, made at build time and never read from the folder; Inno Setup's temporary
  is-XXXXXXXXXX.tmp files: that very form, files only, no folder that is or is reached through a redirection), and at
  an uninstall what an update set aside, of the package's names, and the markers; nothing else;
- nothing is run but the program itself, and nothing asks the network;
- where build_setup.ps1 made the uncompressed twin for the byte scan: it holds the same files, it is readable (the
  mutex's name is seen in it), and neither it nor the setup names the folders the setup was made from.
Every failed check is a "FAIL:" line, and the script then fails.
#>
param(
    [string] $SetupDirectory = '',
    # (x64's package, and Windows on Arm's where the setup carries it.)
    [string[]] $PackageDirectory = @(),
    [string] $Inno = '',
    # (The list of what each published version installed that the setup named was made with: build_setup.ps1's own.)
    [string] $PublishedFiles = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\build_common.ps1"
. "$PSScriptRoot\release_common.ps1"

$desktop = Split-Path -Parent $PSScriptRoot
if (-not $Inno) { $Inno = Join-Path $desktop ".deps\inno\$InnoVersion" }
$failures = 0
function Check([bool] $Condition, [string] $Message) {
    if (-not $Condition) { Write-Host "FAIL: $Message"; $script:failures++ }
}

# The program's mutex, as the program makes it: "Local\" and the name main.cpp gives SingleInstance.
$main = [IO.File]::ReadAllText((Join-Path $desktop 'ui/main.cpp'))
$single = [IO.File]::ReadAllText((Join-Path $desktop 'ui/win/single_instance.cpp'))
$named = [regex]::Match($main, 'SingleInstance\s+\w+\(u"([^"]+)"_s\)')
Check ($named.Success -and $single.Contains('(u"Local\\"_s + name)')) 'the program''s mutex is read from ui/main.cpp and ui/win/single_instance.cpp'
$ProgramMutex = "Local\$($named.Groups[1].Value)"
# The names a copy of the program answers the setup by (0.1.2): its event and its answer window, as
# ui/win/quit_requests.h names them, and the window every version keeps for its icon, as ui/win/tray.cpp names it.
$quitFile = Join-Path $desktop 'ui/win/quit_requests.h'
$quit = if (Test-Path -LiteralPath $quitFile) { [IO.File]::ReadAllText($quitFile) } else { '' }
$tray = [IO.File]::ReadAllText((Join-Path $desktop 'ui/win/tray.cpp'))
$eventNamed = [regex]::Match($quit, 'quitRequestEvent\[\] = "([^"\\]*(?:\\\\[^"\\]*)*)";')
$answerNamed = [regex]::Match($quit, 'quitAnswerWindow\[\] = "([^"\\]+)";')
$wantedNamed = [regex]::Match($quit, 'quitWantedEvent\[\] = "([^"\\]*(?:\\\\[^"\\]*)*)";')
$iconNamed = [regex]::Match($tray, 'kindOfWindow\[\] = L"([^"\\]+)";')
$QuitEvent = $eventNamed.Groups[1].Value.Replace('\\', '\')
# (And how long the program honours a request: longer than this setup waits for its answer, review N1.)
$freshNamed = [regex]::Match($quit, 'std::chrono::milliseconds fresh\{([0-9'']+)\};')
$FreshMs = if ($freshNamed.Success) { [int] ($freshNamed.Groups[1].Value -replace "'", '') } else { 0 }
$AnswerWindow = $answerNamed.Groups[1].Value
$WantedEvent = $wantedNamed.Groups[1].Value.Replace('\\', '\')
$IconWindow = $iconNamed.Groups[1].Value
Check ($freshNamed.Success -and $eventNamed.Success -and $answerNamed.Success -and $wantedNamed.Success -and $iconNamed.Success -and $QuitEvent.StartsWith('Local\') -and $WantedEvent.StartsWith('Local\')) 'the program''s quit request and its windows are read from ui/win/quit_requests.h and ui/win/tray.cpp, the request in this session''s names'
# The marker of an update that did not finish and the ending of what it set aside, as the program looks for them at its
# start (0.2.0, ui/win/unfinished_update.h).
$unfinishedFile = Join-Path $desktop 'ui/win/unfinished_update.h'
$unfinished = if (Test-Path -LiteralPath $unfinishedFile) { [IO.File]::ReadAllText($unfinishedFile) } else { '' }
$markerNamed = [regex]::Match($unfinished, 'unfinishedUpdateMarker\[\] = "([^"\\]+)";')
$endingNamed = [regex]::Match($unfinished, 'setAsideEnding\[\] = "([^"\\]+)";')
$kindNamed = [regex]::Match($unfinished, 'setAsideRecordKind\[\] = "([^"\\]+)";')
$recordEndNamed = [regex]::Match($unfinished, 'setAsideRecordEnd\[\] = "([^"\\]+)";')
$UnfinishedMarker = $markerNamed.Groups[1].Value
$AsideEnding = $endingNamed.Groups[1].Value
$RecordKind = $kindNamed.Groups[1].Value
$RecordEnd = $recordEndNamed.Groups[1].Value
Check ($markerNamed.Success -and $endingNamed.Success -and $AsideEnding.StartsWith('.') -and $kindNamed.Success -and $recordEndNamed.Success) 'the marker of an update that did not finish, the ending of what it set aside and its record are read from ui/win/unfinished_update.h'
$MarkerBytes = [byte[]] @(0x73, 0x65, 0x74, 0x75, 0x70, 0x0A)
$Committed = Join-Path $desktop 'packaging/qkvm-desktop.iss'

function Get-Sections([string] $Text) {
    <# The script's lines, by the section they are in (its head before the first section is ''). #>
    $sections = @{ '' = [Collections.Generic.List[string]]::new() }
    $at = ''
    foreach ($line in ($Text -replace "`r`n", "`n").Split("`n")) {
        if ($line -match '^\[(\w+)\]\s*$') { $at = $Matches[1]; if (-not $sections.ContainsKey($at)) { $sections[$at] = [Collections.Generic.List[string]]::new() }; continue }
        $sections[$at].Add($line)
    }
    return $sections
}

function Read-Pascal([string[]] $Lines) {
    <# The lines of a [Code] section as Pascal Script reads them, one for each: its code with every comment out ({...},
       (*...*) and //..., the first two over any number of lines) and every string said as '' (a string ends at its
       line), and the strings it says. A line of the preprocessor (#include, #ifdef ...) is no code. #>
    $text = (@($Lines | ForEach-Object { if ($_ -match '^\s*#(include|if|ifdef|ifndef|ifexist|ifnexist|else|elif|endif|define|undef|error|pragma|emit|expr|insert|sub|endsub|for)\b') { '' } else { $_ } }) -join "`n")
    $code = [Text.StringBuilder]::new()
    $strings = [Collections.Generic.List[object]]::new()
    $strings.Add([Collections.Generic.List[string]]::new())
    foreach ($token in [regex]::Matches($text, "(?<s>'(?:[^'\n]|'')*'?)|(?<c>//[^\n]*|\{[^}]*\}?|\(\*[\s\S]*?(?:\*\)|\z))|(?<o>[^'/{(\n]+|[/(\n])")) {
        if ($token.Groups['s'].Success) {
            $said = $token.Value.Substring(1)
            if ($said.EndsWith("'")) { $said = $said.Substring(0, $said.Length - 1) }
            $strings[-1].Add($said.Replace("''", "'"))
            [void] $code.Append("''")
            continue
        }
        $part = if ($token.Groups['c'].Success) { $token.Value -replace '[^\n]', ' ' } else { $token.Value }
        [void] $code.Append($part)
        for ($n = 0; $n -lt ([regex]::Matches($part, "`n")).Count; $n++) { $strings.Add([Collections.Generic.List[string]]::new()) }
    }
    $codeLines = $code.ToString().Split("`n")
    for ($at = 0; $at -lt $codeLines.Count; $at++) { [pscustomobject]@{ Code = $codeLines[$at]; Strings = @($strings[$at]) } }
}

function Get-SetupFailures([string] $Setup, [string[]] $Packages, [string] $Published) {
    <# What is wrong with the setup build_setup.ps1 made in $Setup from $Packages (x64's, and Windows on Arm's where it
       carries it): nothing, or one line for each. #>
    $said = [Collections.Generic.List[string]]::new()
    # (Each package by its kind of Windows, x64's first.)
    $carried = [ordered]@{}
    foreach ($folder in @($Packages)) {
        $one = Get-Content -LiteralPath (Join-Path $folder 'package-manifest.json') -Raw | ConvertFrom-Json
        $carried[[string] $one.architecture] = [pscustomobject]@{ Kind = [string] $one.architecture; Folder = $folder; Manifest = $one }
    }
    $kinds = @(@('x64', 'arm64') | Where-Object { $carried.Contains($_) })
    $manifest = $carried['x64'].Manifest
    $Package = $carried['x64'].Folder
    $version = [string] $manifest.version
    $name = "qKVM-Desktop-$version-windows-setup.exe"
    $exe = Join-Path $Setup $name
    $inputs = Join-Path $Setup 'inputs'
    $log = Join-Path $Setup 'compile.txt'
    $suffixOf = @{ 'x64' = 'X64'; 'arm64' = 'Arm64' }

    # --- It compiled, from the script as committed.
    if (-not (Test-Path -LiteralPath $exe -PathType Leaf) -or -not (Test-Path -LiteralPath $log) -or
        -not (Select-String -LiteralPath $log -Pattern '^Successful compile' -Quiet)) {
        $said.Add("the script did not compile into $name")
        return $said
    }
    $script = [IO.File]::ReadAllText((Join-Path $inputs 'qkvm-desktop.iss'))
    if ((Get-Sha256 (Join-Path $inputs 'qkvm-desktop.iss')) -cne (Get-Sha256 $Committed)) { $said.Add('the script compiled is not packaging/qkvm-desktop.iss as committed') }
    $sections = Get-Sections $script
    $setupLines = @($sections['Setup'] | ForEach-Object { $_.Trim() } | Where-Object { $_ -and -not $_.StartsWith(';') })
    $record = Get-Content -LiteralPath (Join-Path $Setup 'setup-record.json') -Raw | ConvertFrom-Json
    if ($record.schema -ne 2 -or (@($record.architectures) -join ',') -cne ($kinds -join ',')) {
        $said.Add("the setup's record does not say the kinds of Windows it carries ($(@($record.architectures) -join ', ') for $($kinds -join ', '))")
    }

    # --- What the compiler put in: each package, file for file, its manifest, the marker and the update checks; nothing
    # else.
    $expected = @{}
    foreach ($one in $carried.Values) {
        foreach ($entry in $one.Manifest.files) {
            $expected[(Join-Path $one.Folder ($entry.path -replace '/', '\')).ToLowerInvariant()] = [ordered]@{ path = $entry.path; sha256 = $entry.sha256; bytes = [long] $entry.bytes; kind = $one.Kind }
        }
        $manifestFile = Join-Path $one.Folder 'package-manifest.json'
        $expected[$manifestFile.ToLowerInvariant()] = [ordered]@{ path = 'package-manifest.json'; sha256 = Get-Sha256 $manifestFile; bytes = (Get-Item -LiteralPath $manifestFile).Length; kind = $one.Kind }
    }
    $markerSource = Join-Path $inputs 'qkvm-desktop.installed'
    $expected[$markerSource.ToLowerInvariant()] = [ordered]@{ path = 'qkvm-desktop.installed'
        sha256 = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($MarkerBytes)).ToLowerInvariant(); bytes = $MarkerBytes.Length; kind = '' }
    # (And qKVM Setup's update check of each kind it carries, carried and never copied as such, review i020-1 M3: the
    # bytes setup-record.json names, a Windows program of that kind.)
    $checkSources = @()
    foreach ($kind in $kinds) {
        $checkName = if ($kind -ceq 'arm64') { 'qkvm-update-check-arm64.exe' } else { 'qkvm-update-check.exe' }
        $checkSource = Join-Path $inputs $checkName
        $checkRecord = @($record.updateChecks | Where-Object { $_.architecture -ceq $kind -and $_.name -ceq $checkName })
        if (-not (Test-Path -LiteralPath $checkSource -PathType Leaf) -or $checkRecord.Count -ne 1 -or (Get-Sha256 $checkSource) -cne [string] $checkRecord[0].sha256 -or
            (Get-PeArchitecture ([IO.File]::ReadAllBytes($checkSource))) -cne $kind) {
            $said.Add("the setup's update check for $kind is not the Windows program of that kind its record names")
        } else {
            $expected[$checkSource.ToLowerInvariant()] = [ordered]@{ path = $checkName; sha256 = Get-Sha256 $checkSource; bytes = (Get-Item -LiteralPath $checkSource).Length; kind = $kind }
        }
        $checkSources += $checkSource.ToLowerInvariant()
    }
    if (@($record.updateChecks).Count -ne $kinds.Count) { $said.Add('the setup''s record names another update check than one of each kind it carries') }
    # (The room an update asks for before it changes anything: every file it puts in on a PC of the kind that takes the
    # most, its manifest, the marker and the larger update check.)
    $most = [long] 0
    foreach ($one in $carried.Values) {
        $sum = [long] (Get-Item -LiteralPath (Join-Path $one.Folder 'package-manifest.json')).Length
        foreach ($entry in $one.Manifest.files) { $sum += [long] $entry.bytes }
        if ($sum -gt $most) { $most = $sum }
    }
    $mostCheck = [long] 0
    foreach ($one in @($record.updateChecks)) { if ([long] $one.bytes -gt $mostCheck) { $mostCheck = [long] $one.bytes } }
    if ([long] $record.roomAsked -ne $most + $MarkerBytes.Length + $mostCheck) {
        $said.Add("the setup asks for other room than the kind that takes the most ($($record.roomAsked) for $($most + $MarkerBytes.Length + $mostCheck))")
    }
    $listed = @(Get-Content -LiteralPath (Join-Path $Setup 'setup-files.txt') | ConvertFrom-Csv -Delimiter "`t")
    $seen = @{}
    foreach ($row in $listed) {
        $source = ([string] $row.SourceFilename).ToLowerInvariant()
        if (-not $expected.ContainsKey($source)) { $said.Add("the setup holds a file the package does not have: $($row.SourceFilename)"); continue }
        if ($seen.ContainsKey($source)) { $said.Add("the setup holds $($expected[$source].path) twice"); continue }
        $seen[$source] = $true
        if ([string] $row.SHA256Sum -cne $expected[$source].sha256 -or [long] $row.OriginalSize -ne $expected[$source].bytes) {
            $said.Add("the setup holds other bytes than the package's manifest names for $($expected[$source].path)")
        }
        if ([string] $row.Encrypted -cne 'no') { $said.Add("the setup holds $($expected[$source].path) encrypted") }
    }
    foreach ($source in $expected.Keys | Where-Object { -not $seen.ContainsKey($_) }) {
        $said.Add("the setup lacks $($expected[$source].path), which the package has")
    }
    # files.iss: each of each package's files at its own place, for the PC of that kind alone (x64's: Check: not IsArm64,
    # from /DPackageDir; Windows on Arm's: Check: IsArm64, from /DArm64PackageDir), and nothing more.
    $lines = @(Get-Content -LiteralPath (Join-Path $inputs 'files.iss') | Where-Object { $_.Trim() })
    $placed = @{}
    $kindOfLine = @()
    foreach ($line in $lines) {
        if ($line -cnotmatch '\ASource: "(\{#Arm64PackageDir\}\\)?([^"{}]+)"; DestDir: "\{app\}((?:\\[^"\\{}]+)*)"; Flags: ignoreversion; Check: (not IsArm64|IsArm64)\z') { $said.Add("files.iss has a line that is not one file at its own place: $line"); $kindOfLine += ''; continue }
        $arm = $Matches[1] -ceq '{#Arm64PackageDir}\'
        $relative = $Matches[2]
        $folder = $Matches[3]
        $kind = if ($arm) { 'arm64' } else { 'x64' }
        $kindOfLine += $kind
        if ($arm -ne ($Matches[4] -ceq 'IsArm64') -or -not $carried.Contains($kind)) { $said.Add("files.iss puts a file of one kind of Windows in for another: $line"); continue }
        $wanted = Split-Path -Parent $relative
        if ($folder -cne $(if ($wanted) { "\$wanted" } else { '' })) { $said.Add("files.iss puts $relative elsewhere than its own place ({app}$folder)") }
        $placed[(Join-Path $carried[$kind].Folder $relative).ToLowerInvariant()] = $true
    }
    foreach ($source in $expected.Keys | Where-Object { $_ -cne $markerSource.ToLowerInvariant() -and $checkSources -cnotcontains $_ }) {
        if (-not $placed.ContainsKey($source)) { $said.Add("files.iss does not put $($expected[$source].path) in the program's folder") }
    }
    if ($lines.Count -ne $expected.Count - 1 - $kinds.Count) { $said.Add("files.iss has $($lines.Count) lines for $($expected.Count - 1 - $kinds.Count) files") }
    # (Each program's own file last: whatever starts it during the copy finds it beside the libraries it was built with;
    # x64's then Windows on Arm's, after every other file of either kind.)
    $lastLines = @(foreach ($kind in $kinds) { if ($kind -ceq 'arm64') { 'Source: "{#Arm64PackageDir}\qkvm-desktop.exe"; DestDir: "{app}"; Flags: ignoreversion; Check: IsArm64' } else { 'Source: "qkvm-desktop.exe"; DestDir: "{app}"; Flags: ignoreversion; Check: not IsArm64' } })
    if ($lines.Count -lt $kinds.Count -or (@($lines | Select-Object -Last $kinds.Count) -join "`n") -cne ($lastLines -join "`n")) { $said.Add("the program's file is not the last one files.iss puts in") }

    # --- For the user alone.
    if ($setupLines -cnotcontains 'PrivilegesRequired=lowest') { $said.Add('PrivilegesRequired is not lowest') }
    if (@($setupLines | Where-Object { $_ -match '^PrivilegesRequiredOverridesAllowed' }).Count) { $said.Add('PrivilegesRequired may be overridden') }
    $level = [regex]::Match((Get-PeManifest $exe), 'requestedExecutionLevel\s+level="([^"]+)"\s+uiAccess="([^"]+)"')
    if (-not $level.Success -or $level.Groups[1].Value -cne 'asInvoker' -or $level.Groups[2].Value -cne 'false') {
        $said.Add("the setup's own manifest asks for more than the user ($($level.Value))")
    }
    # --- The mutex, the update mode and the marker of the contract.
    if ($setupLines -cnotcontains "AppMutex={code:ProgramMutex|$ProgramMutex}") { $said.Add("AppMutex is not the program's mutex ($ProgramMutex)") }
    $code = ($sections['Code'] -join "`n")
    foreach ($part in @("ExpandConstant('{param:UPDATE|0}') = '1'", 'UpdateWaitMs = 60000;', 'StillRunningExitCode = 105;',
        'ExitProcess(StillRunningExitCode);', "external 'ExitProcess@kernel32.dll stdcall';", 'Result := Param;',
        # (In update mode, before anything is changed: room for every file, and every file of the folder free to be
        # replaced; else Preparing to Install refuses, Inno Setup's exit code 7.)
        'function PrepareToInstall(var NeedsRestart: Boolean): String;', 'GetSpaceOnDisk64(', '{#PackageBytes}',
        'OpenReadWriteExclusive = $12;', 'TFileStream.Create(', '  if (Result = '''') and (Free < Needed) then', "      if Busy.Count > 0 then begin",
        "        if (Attributes and ReparsePointAttribute) <> 0 then`n          Busy.Add(Path)",
        # (A folder that Windows could not go through to its end is in use, as one that cannot be looked through: why
        # there is no next entry is asked of Windows itself, and only "no more files" is the end; review F3.)
        '  ErrorNoMoreFiles = 18;', '  FindDataChars = 296;', '  FindNameAt = 23;', '  FindNameChars = 260;',
        "      Data := StringOfChar(' ', FindDataChars);`n      if FindNextFileW(Found.FindHandle, Data) = 0 then begin`n        Error := DLLGetLastError;`n        if Error <> ErrorNoMoreFiles then`n          CutShort := Format('error %d: %s', [Error, SysErrorMessage(Error)]);`n        Break;`n      end;`n      Name := Copy(Data, FindNameAt, FindNameChars);`n      if Pos(#0, Name) = 0 then begin`n        CutShort := 'a name in it could not be read';`n        Break;`n      end;`n      Name := Copy(Name, 1, Pos(#0, Name) - 1);`n      Attributes := Ord(Data[1]);`n    end;`n  finally`n    FindClose(Found);`n  end;`n  if CutShort <> '' then begin`n    Log('A folder of qKVM Desktop could not be looked through to its end (' + CutShort + '): ' + Folder);`n    Busy.Add(Folder);`n    Unlisted.Add(Folder);`n  end;`nend;",
        # (The first look whole, however long it takes: no count of time in it, review F1; FilesInUse below.)
        # (And a folder that cannot be looked through, the program's folder or one within, is in use itself unless
        # Windows says it is not there, as a file is, and the reason says so where it is the first in use.)
        '  LookMs = 2000;', 'procedure FilesInUse(Folder: String; Busy, Unlisted: TStringList);',
        "  if not FindFirst(Folder + '\*', Found) then begin`n    if not FreeToReplace(Folder) then begin`n      Log('A folder of qKVM Desktop cannot be looked through: ' + Folder);`n      Busy.Add(Folder);`n      Unlisted.Add(Folder);`n    end;`n    Exit;`n  end;`n  CutShort := '';`n  try`n    Name := Found.Name;`n    Attributes := Found.Attributes;`n    while True do begin`n      if (Name <> '.') and (Name <> '..') then begin",
        "          FilesInUse(Path, Busy, Unlisted)",
        "      if Busy.Count > 0 then begin`n        if Unlisted.IndexOf(Busy[0]) >= 0 then`n          Reason := Format('a folder of qKVM Desktop cannot be looked through (%s)', [Busy[0]])`n        else`n          Reason := Format('a file of qKVM Desktop is in use or cannot be replaced (%s)', [Busy[0]]);`n      end;",
        "      if (Found.Attributes and (ReparsePointAttribute or DirectoryAttribute)) <> 0 then`n        Exit;",
        # (A file found in use that Windows says is not there any more is free; one it cannot look at is not; and the
        # first look judges each file as every look again does, review R5.)
        "  Result := False;`n  if GetFileAttributesW(Path) = NoFileAttributes then begin`n    Error := DLLGetLastError;`n    Result := (Error = ErrorFileNotFound) or (Error = ErrorPathNotFound);`n    Exit;`n  end;",
        '  ErrorFileNotFound = 2;', '  ErrorPathNotFound = 3;', '  NoFileAttributes = -1;',
        "        else if not FreeToReplace(Path) then`n          Busy.Add(Path);",
        # (Never an older version over a newer one.)
        'function InitializeSetup: Boolean;', 'ComparePackedVersion(', '  if ComparePackedVersion(Have, Offered) > 0 then begin',
        "RegQueryStringValue(HKEY_CURRENT_USER, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{A964EFA6-87C6-4A23-A9CC-B71F87441433}_is1', 'DisplayVersion', Installed)")) {
        if (-not $code.Contains($part)) { $said.Add("the update mode of the contract lacks: $part") }
    }
    # --- A running copy closed for the person (0.1.2). Outside update mode the setup's own start does not refuse for
    # the mutex (a copy run from elsewhere holds it too): it finds the copy of {app} by its icon's window, takes that
    # window's process only where its file is {app}\qkvm-desktop.exe, asks it by the program's event (the right to set
    # it and no other), reads its answer only from a window of that process, waits about 10 s (60 s once it says it is
    # ending), and ends it by force only on the person's End it now or a silent install's /FORCECLOSE=1.
    foreach ($part in @("  QuitRequestEvent = '$QuitEvent';", "  QuitAnswerWindow = '$AnswerWindow';", "  IconWindow = '$IconWindow';",
        "  QuitWantedEvent = '$WantedEvent';",
        # (A copy asked only where it has its answer window; one that has none cannot be asked, and is ended by force by a
        # silent install only where its log says so plainly first; review L2.)
        "  Before := ProgramAnswer;",
        "  if Before = '' then begin`n    Log('qKVM Desktop has no answer window: it cannot be asked to close.');`n    Result := CannotBeAsked;`n    Exit;`n  end;`n  Before := AnswerNumber(Before);",
        "    if Outcome = CannotBeAsked then`n      Log('qKVM Desktop could not be asked to close, so what it is doing is not known; it is ended by force all the same, as /FORCECLOSE=1 says.');`n    if EndProgram then",
        # (A request that stands while this setup waits, withdrawn whenever it stops waiting, and at once where it gives up:
        # a program whose interface answers later stays; review M1.)
        # (A ticket of its own making, which takes itself back as it is waited for, set once for each request; withdrawn
        # by taking it back, which tells whether the program took it first; never a reset, a lock or a wait; review R1.)
        "    Wanted := CreateEventW(0, 0, 0, QuitWantedEvent);`n    if (Wanted <> 0) and (DLLGetLastError = ErrorAlreadyExists) then begin`n      CloseHandle(Wanted);`n      Wanted := 0;`n    end;`n    if Wanted = 0 then begin`n      Log('qKVM Desktop cannot be asked to close: this setup could not make its event of a request that stands (another program may hold its name).');`n      Result := False;`n      Exit;`n    end;`n    SetEvent(Wanted);`n  end;`n  Event := OpenEventW(EventModifyState, 0, QuitRequestEvent);",
        "  if Wanted <> 0 then begin`n    Result := WaitForSingleObject(Wanted, 0) <> WaitObject0;`n    CloseHandle(Wanted);`n    Wanted := 0;",
        '  ErrorAlreadyExists = 183;',
        "      if (Elapsed >= Most) and (Result = NoAnswer) and not Withdrawn then begin`n        if WithdrawRequest then begin`n          Result := Ending;`n          Most := UpdateWaitMs;",
        "        end else begin`n          Withdrawn := True;`n          Most := Most + 1000;`n        end;",
        # (A request found taken as it is withdrawn is one that ends, whatever was said before, a word that it stays
        # among them: withdrawn before that word is believed, and waited for as one that ends; review F2.)
        "  finally`n    // (However it ends, the request withdrawn; one the program had taken is one that ends, whatever was said before:`n    // never one that stays, cannot be asked or did not answer; review F2.)`n    if WithdrawRequest then`n      if Result <> Ended then`n        Result := Ending;`n  end;`nend;",
        "          if not WithdrawRequest then begin`n            Result := Held;`n            HeldBy := AnswerWords(Said);`n            Log('qKVM Desktop stays open: ' + HeldBy);`n            Exit;`n          end;`n          Before := AnswerNumber(Said);`n        end;`n        if Result <> Ending then begin",
        '  CloseWaitMs = 10000;', '  EventModifyState = $0002;', '  ProcessTerminate = $0001;',
        # (None for Setup's own check at its start, and the program's once installing: Inno Setup keeps that in the
        # uninstall log, which is all the uninstaller checks; review M2.)
        "  if not UpdateMode and not Installing then begin`n    Result := '';`n    Exit;`n  end;`n  if Waited or Installing or not UpdateMode then",
        "  Window := FindWindowExW(0, 0, IconWindow, 0);",
        "    Process := OpenProcess(ProcessQueryLimitedInformation or ProcessSynchronize, 0, Id);",
        "      if (QueryFullProcessImageNameW(Process, 0, Name, Size) <> 0) and PathSame(Copy(Name, 1, Size), Path) and",
        # (Its path: {app}'s from the Ready page on; the installed folder's as Settings > Apps keeps it on the first page.)
        "function FindProgram: Boolean;`nbegin`n  Result := FindProgramAt(ProgramPath);`nend;",
        "  if RegQueryStringValue(HKEY_CURRENT_USER, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{A964EFA6-87C6-4A23-A9CC-B71F87441433}_is1', 'InstallLocation', InstalledFolder) then",
        "  if (InstalledFolder <> '') and FindProgramAt(AddBackslash(InstalledFolder) + 'qkvm-desktop.exe') then",
        "    Window := FindWindowExW(0, Window, IconWindow, 0);",
        "  Window := FindWindowExW(0, 0, QuitAnswerWindow, 0);`n  while Window <> 0 do begin`n    Id := 0;`n    GetWindowThreadProcessId(Window, Id);`n    if Id = ProgramId then begin",
        "  Event := OpenEventW(EventModifyState, 0, QuitRequestEvent);",
        "  try`n    if not AskProgram then begin`n      Result := CannotBeAsked;",
        "      end else if (Said <> '') and (AnswerNumber(Said) <> Before) then begin`n        if AnswerWords(Said) <> '' then begin",
        # (An answer to a request heard too late is never this wait's, nor staying open: asked again; review N1.)
        "      if (Said <> '') and (AnswerNumber(Said) <> Before) and AnswerLate(Said) then begin",
        "        Before := AnswerNumber(Said);`n        if not Withdrawn then begin",
        # (Once it has given up and withdrawn, asked again once only, with a second of its own and then the grace again;
        # never a loop: review R1.)
        "        end else if not AskedAgain then begin",
        "          AskedAgain := True;`n          Withdrawn := False;`n          Most := Elapsed + 1000;`n          if not AskProgram then",
        "  if Copy(Result, Length(Result), 1) = '*' then`n    Result := Copy(Result, 1, Length(Result) - 1);",
        "  Result := (Pos(' ', Answer) > 1) and (Copy(Answer, Pos(' ', Answer) - 1, 1) = '*');",
        "          Result := Ending;`n          Most := UpdateWaitMs;",
        "  Process := OpenProcess(ProcessTerminate, 0, ProgramId);`n  Result := (Process <> 0) and (TerminateProcess(Process, 1) <> 0);",
        "  Result := ExpandConstant('{param:FORCECLOSE|0}') = '1';",
        "  if WizardSilent or UpdateMode or not FindProgram then`n    Exit;",
        "MB_OKCANCEL, ['Close qKVM and continue', 'Cancel'], 0) <> IDOK then begin`n    Result := False;`n    Exit;`n  end;",
        "MB_YESNOCANCEL, ['Try again', 'End it now', 'Cancel'], 0);`n    if Choice = IDNO then begin`n      if EndProgram then begin",
        "  until Choice = IDCANCEL;`n  Result := False;",
        "  Outcome := CloseProgram(nil);`n  if (Outcome <> Ended) and ForceClose then begin",
        # (Room before a running copy is closed, and what this setup closed started again as it ends where it then changes
        # nothing, the result line saying so; review L1.)
        "  Reason := GateReason;`n  if Reason = '' then`n    Reason := RoomReason;`n  if Reason = '' then`n    Reason := ForeignFolderReason;`n  if Reason = '' then`n    Reason := RightsReason;`n  if (Reason = '') and WizardSilent and not UpdateMode then`n    Reason := CloseProgramSilently;`n  if Reason = '' then begin",
        # (Nothing closed and nothing changed where this setup holds no gate: its mutex not there to be opened, or its
        # event not made; review F5.)
        "  if GateReason <> '' then`n    Exit;`n  // (Nothing closed where there is no room",
        "  Gate := OpenMutexW(MutexSynchronize, 0, SetupGateName);`n  if Gate <> 0 then`n    CloseHandle(Gate);`n  if (Gate = 0) or (SetupRuns = 0) then`n    Result := 'this setup could not make what keeps qKVM Desktop from starting while it works';",
        '  MutexSynchronize = $00100000;',
        "    if ClosedIt then begin`n      StartAtEnd := True;`n      Reason := Reason + '; qKVM Desktop, which this setup had closed, is started again';",
        "  if StartAtEnd then`n    StartAgain;`nend;",
        "  if RoomReason <> '' then`n    Exit;`n  // (Nor for an install into a folder that holds what is not an install of ours: Preparing to Install refuses it,`n  // review i020-2 M1.)`n  if ForeignFolderReason <> '' then`n    Exit;`n  // (Nor for one that another account may change: Preparing to Install refuses it, review 1.2.)`n  if RightsReason <> '' then`n    Exit;`n  if TaskDialogMsgBox('qKVM Desktop is running',",
        # (Where the program's folder cannot be looked through, a junction Inno Setup's RedirectionGuard does not follow,
        # its drive's root is, and the redirection is refused in its own words.)
        "  if not GetSpaceOnDisk64(RoomFolder, Free, Total) then`n    // (A folder this setup may not look through, as Inno Setup's RedirectionGuard keeps it from following a junction`n    // the person made: its drive's root is looked at instead, and the redirection is refused below in its own words,`n    // ForeignFolderReason and RightsReason; step 44 of test_setup_vm.)`n    if not GetSpaceOnDisk64(AddBackslash(ExtractFileDrive(ExpandConstant('{app}'))), Free, Total) then`n      Result := 'the free room on the drive of qKVM Desktop could not be told';`n  if (Result = '') and (Free < Needed) then`n    Result := 'there is not room enough on the drive of qKVM Desktop';",
        # (What is installed and what this does with it, on the first page; on an upgrade no page asks again.)
        "WizardForm.WelcomeLabel2.Caption := Format('qKVM Desktop %s is installed. This upgrades it to %s and keeps your wall, settings and passwords.', [InstalledVersion, '{#AppVersion}']);",
        "  InstalledVersion := Installed;", "  Result := (PageID = wpSelectTasks) and (InstalledVersion <> '');",
        # (The last page's tick, and the start at the end of an update or of a silent install that closed the program.)
        "  Result := MayStartProgram and CopyStarts and not UpdateMode and ((InstalledVersion = '') or ClosedIt);", "  Result := MayStartProgram and CopyStarts and not UpdateMode and not StartTicked;",
        "  Result := MayStartProgram and CopyStarts and (UpdateMode or (WizardSilent and ClosedIt));",
        # (Review 1.7: nothing started or offered where the copy does not count and left nothing whole, a first install's.)
        "  Result := (NotCounted = '') or EarlierBack;")) {
        if (-not $code.Contains($part)) { $said.Add("the closing of a running copy lacks: $part") }
    }
    # Nothing started by a setup that runs with administrator rights (review C1): the one rule, said in the log, and
    # every way the program is started asks it first.
    $mayStart = [regex]::Match($code, '(?s)\nfunction MayStartProgram: Boolean;\nbegin\n(.*?)\nend;')
    if (-not $mayStart.Success -or $mayStart.Groups[1].Value -cne "  Result := not IsAdmin;`n  if not Result and not ToldNoStart then begin`n    ToldNoStart := True;`n    Log('qKVM Desktop is not started by this setup: the setup runs with administrator rights, and qKVM Desktop runs only as the person, unelevated. Start it from the Start menu.');`n  end;") {
        $said.Add('the setup may start the program where it runs with administrator rights')
    }
    # The setup says it is at work for as long as it runs, as the program tells a Setup that runs from a mutex kept there
    # by a handle elsewhere: its own event, named after the mutex the program opens and its process's number, made
    # first in InitializeSetup (review R2; ui/win/setup_wait.h's setupRunsName and setupMutexName).
    $waitHeader = [IO.File]::ReadAllText((Join-Path $desktop 'ui/win/setup_wait.h'))
    $setupMutexNamed = [regex]::Match($waitHeader, 'setupMutexName\[\] = "([^"\\]*(?:\\\\[^"\\]*)*)";')
    $runsAt = [regex]::Match($code, '(?s)\nfunction InitializeSetup: Boolean;\nvar\n.*?\nbegin\n  Result := True;\n(.*?)\n  if RegQueryStringValue\(')
    if (-not $setupMutexNamed.Success -or $setupMutexNamed.Groups[1].Value.Replace('\\', '\') -cne 'Local\qKVM-Desktop-Setup' -or
        $code -cnotmatch "\n  SetupRunsName = '$([regex]::Escape($setupMutexNamed.Groups[1].Value.Replace('\\', '\')))-';\n" -or -not $runsAt.Success -or
        -not $runsAt.Groups[1].Value.EndsWith("  SetupRuns := CreateEventW(0, 1, 0, SetupRunsName + IntToStr(GetCurrentProcessId));")) {
        $said.Add('the setup does not say it is at work for as long as it runs, as the program tells a Setup that runs')
    }
    # The uninstaller refuses while the program runs, by the program's own mutex, looked at itself: Inno Setup keeps
    # nothing of AppMutex for it after an install made outside update mode (the first VM suite's step 25; review V2).
    $uninstall = [regex]::Match($code, '(?s)\nfunction InitializeUninstall: Boolean;\nvar\n  Running: String;\nbegin\n(.*?)\nend;')
    # (And from its first moment it holds the gate a start honours, as a setup's: Setup's mutex and its own event that
    # says it is at work, for the whole uninstall; and it looks again right before anything is removed, a best effort;
    # review R4. It refuses as it begins, where the uninstall is decided, while a setup is open (Setup's mutex there
    # already) and where it cannot make its gate or its event; review F4.)
    if ($code -cnotmatch "\n  ProgramMutexName = '$([regex]::Escape($ProgramMutex))';\n" -or -not $uninstall.Success -or
        ((@($uninstall.Groups[1].Value -split "`n" | Where-Object { -not $_.Trim().StartsWith('//') })) -join "`n") -cne "  Result := True;`n  UninstallGate := CreateMutexW(0, 0, SetupGateName);`n  if (UninstallGate <> 0) and (DLLGetLastError = ErrorAlreadyExists) then begin`n    Log('A setup of qKVM Desktop, or another uninstall of it, is at work: it is not uninstalled. Let that finish, then uninstall it again.');`n    if not UninstallSilent then`n      MsgBox('qKVM Desktop Setup, or another uninstall of qKVM Desktop, is at work. Let it finish, then uninstall qKVM Desktop again.', mbError, MB_OK);`n    Result := False;`n    Exit;`n  end;`n  UninstallRuns := CreateEventW(0, 1, 0, SetupRunsName + IntToStr(GetCurrentProcessId));`n  if (UninstallGate = 0) or (UninstallRuns = 0) then begin`n    Log('qKVM Desktop could not be kept from starting while it is uninstalled: it is not uninstalled.');`n    if not UninstallSilent then`n      MsgBox('qKVM Desktop could not be kept from starting while it is uninstalled, so it is not uninstalled. Uninstall it again later.', mbError, MB_OK);`n    Result := False;`n    Exit;`n  end;`n  Running := TakeAccountGate;`n  if Running <> '' then begin`n    Log(Running + ': it is not uninstalled. Let that finish, then uninstall it again.');`n    if not UninstallSilent then`n      MsgBox('qKVM Desktop is not uninstalled: ' + Running + '. Let that finish, then uninstall qKVM Desktop again.', mbError, MB_OK);`n    Result := False;`n    Exit;`n  end;`n  if CheckForMutexes(ProgramMutexName) then begin`n    Log('qKVM Desktop is running: it is not uninstalled. Quit it first, then uninstall it again.');`n    if not UninstallSilent then`n      MsgBox('qKVM Desktop is running. Quit it first: right-click the qKVM icon in the notification area, then choose Quit. Then uninstall it again.', mbError, MB_OK);`n    Result := False;`n    Exit;`n  end;`n  Running := ProfileInUse;`n  if Running <> '' then begin`n    Log(Running + ': it is not uninstalled. Quit it first, then uninstall it again.');`n    if not UninstallSilent then`n      MsgBox('qKVM Desktop is running for this Windows account, or it could not be seen whether it is (' + Running + '). Quit it first, in every Windows session of this account: right-click the qKVM icon in the notification area, then choose Quit. Then uninstall it again.', mbError, MB_OK);`n    Result := False;`n  end;" -or
        $code -cnotmatch "\n  SetupGateName = '$([regex]::Escape($setupMutexNamed.Groups[1].Value.Replace('\\', '\')))';\n" -or
        -not $code.Contains("  if CurUninstallStep = usUninstall then begin`n    // (Looked at again right before anything is removed, as a best effort: a copy that started all the same, one before`n    // 0.1.2 that does not honour the gate among them, is said in the log, and the exception ends this step; Inno`n    // Setup's help says that it ends the uninstall there, which is for the VM to show, and nothing here rests on it.`n    // A start of 0.2.0 or later in the instant between this look and the removal of the files ends by itself: it`n    // looks at the gate again once its mutex is there, and finds the profile's lock held by this uninstall`n    // (SECURITY.md). The refusal that decides is InitializeUninstall's; reviews R4, F4.)`n    if CheckForMutexes(ProgramMutexName) then begin`n      Log('qKVM Desktop started while it was being uninstalled: it is not uninstalled. Quit it first, then uninstall it again.');`n      RaiseException('qKVM Desktop is running. Quit it first, then uninstall it again.');`n    end;`n    RemoveSetupLeftovers;")) {
        $said.Add('the uninstaller does not refuse while the program runs, by the program''s own mutex')
    }
    # (And by the lock of the profile, which a copy of 0.2.0 or later takes in any Windows session of its account, on
    # the file the program names (core/src/profile.cpp, runningFile); opened only, never made, and kept, with its lock,
    # for the whole uninstall: ui/win/profile_lock.h, SECURITY.md.)
    $profileCode = [IO.File]::ReadAllText((Join-Path $PSScriptRoot '..\core\src\profile.cpp'))
    $inUse = [regex]::Match($code, '(?s)\nfunction ProfileInUse: String;\n.*?\nend;')
    if (-not $profileCode.Contains("QString runningFile(const QString& settings)`n{`n    return settings + u`".running`"_s;`n}") -or
        $code -cnotmatch "\n  ProfileLockFile = '\{localappdata\}\\QKVM Desktop\\settings\.json\.running';\n" -or
        $code -cnotmatch "\n  OpenExisting = 3;\n" -or -not $inUse.Success -or
        -not $inUse.Value.Contains("  Profile := CreateFileW(ExpandConstant(ProfileLockFile), GenericRead, FileShareReadWrite, 0, OpenExisting, 0, 0);`n  Opened := DLLGetLastError;`n  if (Opened = ErrorFileNotFound) or (Opened = ErrorPathNotFound) then begin`n") -or
        # (Review 1.3: where no copy has made it yet, the uninstall makes it, as a first start does, and its folder where
        # that is not there, holds it all the same, and removes as it ends only what it made: ERROR_ALREADY_EXISTS is a
        # start's.)
        -not $inUse.Value.Contains("    Profile := CreateFileW(ExpandConstant(ProfileLockFile), GenericRead, FileShareReadWrite, 0, OpenAlways, 0, 0);`n    Opened := DLLGetLastError;`n    // (Made here, not opened: no ERROR_ALREADY_EXISTS. One a first start made meanwhile is that start's.)`n    if Opened = 0 then`n      ProfileFileMade := ExpandConstant(ProfileLockFile)`n    else if Opened = ErrorAlreadyExists then`n      Opened := 0;") -or
        -not $inUse.Value.Contains("  if LockFile(Profile, 0, 0, 1, 0) <> 0 then begin`n    UninstallProfile := Profile;`n    Exit;`n  end;`n  Locked := DLLGetLastError;`n  if Locked = ErrorLockViolation then`n") -or
        $code -cnotmatch "\n  OpenAlways = 4;\n" -or
        -not $code.Contains("procedure DeinitializeUninstall;`nbegin`n  if ProfileFileMade = '' then`n    Exit;`n  if UninstallProfile <> 0 then`n    CloseHandle(UninstallProfile);`n  UninstallProfile := 0;`n  if DeleteFile(ProfileFileMade) then`n    Log('Removed the lock of the profile, which this uninstall had made: ' + ProfileFileMade);`n  if ProfileFolderMade <> '' then`n    RemoveDir(ProfileFolderMade);`nend;") -or
        -not $inUse.Value.Contains("  else if (Locked <> ErrorInvalidHandle) or ((Opened <> ErrorFileNotFound) and (Opened <> ErrorPathNotFound)) then begin`n")) {
        $said.Add('the uninstaller does not refuse while the program runs, by the lock of its profile')
    }
    # (Review 1.3: one setup or uninstall at a time for the Windows account, in whatever session: the account's gate, a
    # mutex of the whole PC's names after the account's SID, the name the program takes for an instant to take a stale
    # marker away (ui/win/unfinished_update.h, setupAccountGatePrefix), made owned as the setup begins, before anything is
    # put right, and by the uninstall after its own gate; waited for 3 s where it is there, taken where its owner ended,
    # else a refusal; one another account made, which this setup may not open, is no free one. Review 1.4: the marker held
    # by a handle of a file on a disk that holds this setup's record, or nothing is set aside.)
    $gateNamed = [regex]::Match($unfinished, 'setupAccountGatePrefix\[\] = L"([^"\\]*(?:\\\\[^"\\]*)*)";')
    $accountGate = $gateNamed.Groups[1].Value.Replace('\\', '\')
    if (-not $gateNamed.Success -or $accountGate -cne 'Global\qKVM-Desktop-Setup-Account-' -or $code -cnotmatch "\n  AccountGatePrefix = '$([regex]::Escape($accountGate))';\n") {
        $said.Add("the setup's gate of the account is not the one the program takes (ui/win/unfinished_update.h: $accountGate)")
    }
    foreach ($part in @('  AccountGateWaitMs = 3000;', '  WaitAbandoned = $80;', '  FileTypeDisk = 1;',
        "  Own := OwnAccount;`n  if Own = '' then begin`n    Result := 'this setup could not tell which Windows account it runs as, to keep two setups of qKVM Desktop from working at once';`n    Exit;`n  end;`n  AccountGate := CreateMutexW(0, 1, AccountGatePrefix + Own);`n  Error := DLLGetLastError;`n  if AccountGate = 0 then begin",
        "  if Error <> ErrorAlreadyExists then`n    Exit;`n  Answer := WaitForSingleObject(AccountGate, AccountGateWaitMs);`n  if (Answer = WaitObject0) or (Answer = WaitAbandoned) then`n    Exit;`n  CloseHandle(AccountGate);`n  AccountGate := 0;`n  Result := 'another setup or uninstall of qKVM Desktop is at work for this Windows account, here or in another Windows session of it';",
        "    InstalledFolder := RemoveBackslashUnlessRoot(InstalledFolder);`n  // (This account's gate first, before anything is put right or changed: one setup or uninstall at a time for the`n  // account, in whatever Windows session, review 1.3. Where it cannot be had, the setup does not begin, and says why where`n  // the program shows it.)`n  Reason := TakeAccountGate;`n  if Reason <> '' then begin`n    Log('qKVM Desktop Setup does not begin: ' + Reason + '. Nothing was changed.');`n    Log(UnchangedLine(Reason));`n    if not WizardSilent then`n      MsgBox(",
        "      MsgBox('qKVM Desktop Setup does not begin: ' + Reason + '. Let that finish, then run this setup again. Nothing was changed.', mbError, MB_OK);`n    Result := False;`n    Exit;`n  end;`n  // (The folder's rights first: a record of what an update set aside is followed, and the program's own file of 0.1.2 put`n",
        "  MarkerHold := CreateFileW(InApp(UnfinishedMarker), GenericRead, FileShareReadWrite, 0, OpenExisting, 0, 0);`n  Result := GetFileType(MarkerHold) = FileTypeDisk;`n  if Result then begin`n    Result := LoadStringFromFile(InApp(UnfinishedMarker), Said);`n    Back := Said;`n    Result := Result and (Back = Text);`n  end;`n  if not Result then begin",
        "        if not HoldMarker(Text) then begin`n          RecordText := '';`n          Result := 'the marker of this update could not be held for as long as this setup works (' + UnfinishedMarker + ')';")) {
        if (-not $code.Contains($part)) { $said.Add("the setup's gate of the account, or its hold of its marker, is not as it should be: $part") }
    }
    if (([regex]::Matches((@($sections['Code'] | ForEach-Object { $_ -replace '//.*$', '' }) -join "`n"), '\bTakeAccountGate\b')).Count -ne 3) { $said.Add('the setup and the uninstall do not both take the gate of the account') }
    $startAgainBody = [regex]::Match($code, '(?s)\nprocedure StartAgain;\nvar\n  Started: Integer;\nbegin\n  if not MayStartProgram then\n    Exit;\n  if ExecAsOriginalUser\(')
    if (-not $startAgainBody.Success) { $said.Add('the setup may start the program where it runs with administrator rights') }
    $waited = [regex]::Match($code, 'CloseWaitMs = (\d+);')
    if (-not $waited.Success -or $FreshMs -lt [int] $waited.Groups[1].Value + 1000) {
        $said.Add("the program honours a request for less time than this setup waits for its answer and the second after ($FreshMs ms)")
    }
    # The forced end in one place (the declaration and one call), reached from the two explicit choices alone (the
    # function and two calls; the switch read, said in the reason, and asked before the one call); nothing else ends a
    # process. (Counted in the code without its comments.)
    $codeOnly = (@($sections['Code'] | ForEach-Object { $_ -replace '//.*$', '' }) -join "`n")
    # Nothing that runs before Inno Setup knows {app} expands it (0.1.2's first VM run of steps 18 to 24: InitializeWizard
    # reached ProgramPath, and every setup started while qKVM ran ended at once, exit 1, "An attempt was made to expand
    # the app constant before it was initialized"). The routines of [Code]; those that expand {app} ({group} and
    # {uninstallexe} alike), and every one that calls one of them, through any number of calls; none of those reached
    # from what runs before {app} is set: InitializeSetup, InitializeWizard, ShouldSkipPage, the page buttons of pages
    # before the Ready page, CheckPassword and CheckSerial, and what [Setup] asks by {code:} (ProgramMutex, AppMutex's
    # value, asked at Setup's start). NextButtonClick (asked of every page) and DeinitializeSetup (asked however early
    # Setup ends) reach them only under their own guards, held to the letter: the Ready page, and a setup that has come
    # to its copy.
    # Read as Pascal Script reads it (review 012-final's LOW 1): a name is one whatever its case; a routine's line may
    # be indented, and an <event('...')> before it makes it that event's under a name of its own; a comment ({...},
    # (*...*), //...) is no call, and a string ends no comment and is no call, but {code:Name} in a string expands Name;
    # a constant whose string says {app} expands it wherever it is named; what the files [Code] includes say is [Code]
    # too; and a {code:} of [Setup] (AppMutex's own among them), and a Check: or a {code:} of [Tasks], [Components],
    # [Types] or [Files], is asked before the Ready page (or may be, for [Files]: taken so whatever page asks it).
    # (Each routine's lines as written, comments out, held to the letter below where a guard is; its code, strings
    # blanked, where its calls are looked for; and its strings, where {app} and {code:} are. In a scope of its own: none
    # of its names is the setup's, $name above among them.)
    & {
        $ic = [StringComparer]::OrdinalIgnoreCase
        $known = @('{app}', '{group}', '{uninstallexe}')
        $raw = [Collections.Generic.List[string]]::new()
        foreach ($line in $sections['Code']) {
            if ($line -match '^\s*#include\s+"([^"]+)"') {
                $included = Join-Path $inputs $Matches[1]
                $raw.Add('')
                if (Test-Path -LiteralPath $included -PathType Leaf) { foreach ($more in [IO.File]::ReadAllLines($included)) { $raw.Add($more) } }
                $raw.Add('#include ended')
                continue
            }
            $raw.Add($line)
        }
        $read = @(Read-Pascal $raw.ToArray())
        $routines = [Collections.Generic.Dictionary[string, Collections.Generic.List[string]]]::new($ic)
        $strings = [Collections.Generic.Dictionary[string, Collections.Generic.List[string]]]::new($ic)
        $written = [Collections.Generic.Dictionary[string, Collections.Generic.List[string]]]::new($ic)
        $declared = [Collections.Generic.Dictionary[string, string]]::new($ic)
        $events = [Collections.Generic.Dictionary[string, string]]::new($ic)
        # (A constant said with {app} in its string, by its name: "Name = '...{app}...'" or "Name: String = ...".)
        $constants = [Collections.Generic.Dictionary[string, string]]::new($ic)
        # (And every declaration of one by another's name, "Alias = Name;", by what it names: review T3.)
        $aliases = [Collections.Generic.List[object]]::new()
        $at = $null
        $event = $null
        for ($n = 0; $n -lt $read.Count; $n++) {
            $line = $read[$n].Code
            $saysKnown = @($known | Where-Object { $one = $_; @($read[$n].Strings | Where-Object { $_.IndexOf($one, [StringComparison]::OrdinalIgnoreCase) -ge 0 }).Count })
            if ($saysKnown.Count -and $line -match '^\s*(\w+)\s*(?::\s*[\w.]+\s*)?=(?!=)') { $constants[$Matches[1]] = $saysKnown[0] }
            if ($line -match '^\s*(\w+)\s*(?::\s*[\w.]+\s*)?=(?!=)(.*)$') {
                $aliases.Add([pscustomobject]@{ Name = $Matches[1]; Names = @([regex]::Matches($Matches[2], '\b[A-Za-z_]\w*\b') | ForEach-Object Value) })
            }
            if ($line -match '^\s*<\s*event\s*\(\s*''''\s*\)\s*>\s*$') { $event = @($read[$n].Strings)[0]; continue }
            if ($line -match '^\s*(?:<\s*event\s*\(\s*''''\s*\)\s*>\s*)?(?:function|procedure)\s+(\w+)') {
                $at = $Matches[1]
                if ($line -match '^\s*<\s*event') { $event = @($read[$n].Strings)[0] }
                if (-not $declared.ContainsKey($at)) { $declared[$at] = $at }
                $at = $declared[$at]
                if ($event) { $events[$at] = $event }
                $event = $null
                $routines[$at] = [Collections.Generic.List[string]]::new(); $strings[$at] = [Collections.Generic.List[string]]::new(); $written[$at] = [Collections.Generic.List[string]]::new()
                continue
            }
            if ($raw[$n] -match '^\s*#include\b') { $at = $null; continue }
            if ($at) { $written[$at].Add(($raw[$n] -replace '//.*$', '')); $routines[$at].Add($line); foreach ($one in $read[$n].Strings) { $strings[$at].Add($one) } }
        }
        # (A constant declared by the name of one that says {app} says it too, through any number of them.)
        do {
            $grewAliases = $false
            foreach ($alias in $aliases) {
                if ($constants.ContainsKey($alias.Name)) { continue }
                $named = @($alias.Names | Where-Object { $constants.ContainsKey($_) })
                if ($named.Count) { $constants[$alias.Name] = $constants[$named[0]]; $grewAliases = $true }
            }
        } while ($grewAliases)
        # Who expands which of them itself, and whom each calls (another routine, by any case, or {code:} in a string, or a
        # constant that says one); then everyone who reaches one of those, through any number of calls.
        $direct = [Collections.Generic.Dictionary[string, string]]::new($ic)
        foreach ($one in $constants.Keys) { $direct[$one] = $constants[$one] }
        $calls = [Collections.Generic.Dictionary[string, string[]]]::new($ic)
        foreach ($one in $routines.Keys) {
            $all = $strings[$one] -join "`n"
            $saysKnown = @($known | Where-Object { $all.IndexOf($_, [StringComparison]::OrdinalIgnoreCase) -ge 0 })
            if ($saysKnown.Count) { $direct[$one] = $saysKnown[0] }
            $named = @([regex]::Matches(($routines[$one] -join "`n"), '\b[A-Za-z_]\w*\b') | ForEach-Object Value) + @([regex]::Matches($all, '(?i)\{code:(\w+)') | ForEach-Object { $_.Groups[1].Value })
            $calls[$one] = @($named | Where-Object { $_ -ne $one -and ($declared.ContainsKey($_) -or $constants.ContainsKey($_)) } |
                ForEach-Object { if ($declared.ContainsKey($_)) { $declared[$_] } else { $_ } } | Sort-Object -Unique)
        }
        $expanding = [Collections.Generic.HashSet[string]]::new([string[]] @($direct.Keys), $ic)
        do {
            $grew = $false
            foreach ($one in $routines.Keys) {
                if (-not $expanding.Contains($one) -and @($calls[$one] | Where-Object { $expanding.Contains($_) }).Count) { [void] $expanding.Add($one); $grew = $true }
            }
        } while ($grew)
        # What runs before {app} is known: those events by their names or by an <event('...')> (and NextButtonClick and
        # DeinitializeSetup under any other name than their own, held to the letter below), and what the sections ask.
        $early = @('InitializeSetup', 'InitializeWizard', 'ShouldSkipPage', 'CurPageChanged', 'BackButtonClick', 'CancelButtonClick', 'CheckPassword', 'CheckSerial')
        $roots = [Collections.Generic.List[object]]::new()
        foreach ($one in $routines.Keys) {
            $as = if ($events.ContainsKey($one)) { $events[$one] } else { $one }
            $isEarly = @($early | Where-Object { $_ -ieq $one -or $_ -ieq $as }).Count -gt 0
            $guardedElsewhere = ($as -ieq 'NextButtonClick' -or $as -ieq 'DeinitializeSetup') -and $as -ine $one
            if ($isEarly -or $guardedElsewhere) { $roots.Add([pscustomobject]@{ Label = $(if ($as -ine $one) { "$as as $one" } else { $one }); Routine = $one }) }
        }
        foreach ($section in 'Setup', 'Languages', 'Tasks', 'Components', 'Types', 'Files') {
            if (-not $sections.ContainsKey($section)) { continue }
            $entries = [Collections.Generic.List[string]]::new()
            foreach ($line in $sections[$section]) {
                if ($line -match '^\s*#include\s+"([^"]+)"') {
                    $included = Join-Path $inputs $Matches[1]
                    if (Test-Path -LiteralPath $included -PathType Leaf) { foreach ($more in [IO.File]::ReadAllLines($included)) { $entries.Add($more) } }
                    continue
                }
                $entries.Add($line)
            }
            foreach ($line in $entries) {
                $entry = $line.Trim()
                if (-not $entry -or $entry.StartsWith(';') -or $entry.StartsWith('#')) { continue }
                $label = "[$section] " + $(if ($entry -match '^(\w+)\s*=') { $Matches[1] } elseif ($entry -match '(?i)\b(?:Name|Source):\s*"([^"]*)"') { $Matches[1] } else { $entry })
                foreach ($asked in [regex]::Matches($entry, '(?i)\{code:(\w+)')) {
                    $name = $asked.Groups[1].Value
                    if ($declared.ContainsKey($name)) { $roots.Add([pscustomobject]@{ Label = $label; Routine = $declared[$name] }) }
                    else { $said.Add("$label asks {code:$name}, which the [Code] section does not have") }
                }
                if ($section -in 'Setup', 'Languages') { continue }
                if ($entry -match '(?i)(?:^|;)\s*Check:\s*([^;]+)') {
                    foreach ($name in @([regex]::Matches($Matches[1], '\b[A-Za-z_]\w*\b') | ForEach-Object Value | Where-Object { $declared.ContainsKey($_) })) {
                        $roots.Add([pscustomobject]@{ Label = $label; Routine = $declared[$name] })
                    }
                }
                if ($section -ne 'Files') {
                    foreach ($one in $known) { if ($entry.IndexOf($one, [StringComparison]::OrdinalIgnoreCase) -ge 0) { $said.Add("$label says $one, which Inno Setup does not know when it makes its pages") } }
                }
            }
        }
        foreach ($root in $roots) {
            if (-not $expanding.Contains($root.Routine)) { continue }
            # (Said from what asks it: an event by its name, "InitializeWizard as Other", or a section's entry.)
            $at = $root.Routine
            $chain = @($root.Label) + @(if ($root.Label -cne $at -and -not $root.Label.EndsWith(" as $at")) { $at })
            $guard = 0
            while (-not $direct.ContainsKey($at) -and $guard++ -lt 1000) { $at = @($calls[$at] | Where-Object { $expanding.Contains($_) })[0]; $chain += $at }
            $said.Add("the [Code] section expands $($direct[$at]) before Inno Setup knows it: $($chain -join ' -> ')")
        }
        $guarded = [ordered]@{
            'NextButtonClick' = "begin`n  Result := True;`n  if CurPageID = wpReady then`n    Result := CloseAtInstall;`nend;"
            'DeinitializeSetup' = "begin`n  if SetAside and not PostInstalled then`n    if PutPackageBack and UpdateMode then`n      StartAgain;`n  if StartAtEnd then`n    StartAgain;`nend;"
        }
        foreach ($one in $guarded.Keys) {
            $body = if ($written.ContainsKey($one)) { (($written[$one] | ForEach-Object { $_.TrimEnd() }) -join "`n").Trim() } else { '' }
            if ($body -cne $guarded[$one]) { $said.Add("$one does not reach what expands {app} under its own guard alone, as written: $($body -replace "`n", ' | ')") }
        }
    }
    # The wait is bounded: Most is set by these alone, and asking again after giving up is done once at most.
    $closing = [regex]::Match($codeOnly, '(?s)function CloseProgram\(Progress: TOutputProgressWizardPage\): Integer;.*?\nend;')
    $mosts = (@([regex]::Matches($closing.Value, '\bMost := [^;]+;') | ForEach-Object Value) | Sort-Object -CaseSensitive) -join '|'
    $again = (@([regex]::Matches($closing.Value, '\bAskedAgain := [^;]+;') | ForEach-Object Value) | Sort-Object -CaseSensitive) -join '|'
    if ($mosts -cne 'Most := CloseWaitMs;|Most := Elapsed + 1000;|Most := Most + 1000;|Most := UpdateWaitMs;|Most := UpdateWaitMs;' -or $again -cne 'AskedAgain := False;|AskedAgain := True;') {
        $said.Add("the wait for a running copy is not bounded as it should be ($mosts; $again)")
    }
    if (([regex]::Matches($codeOnly, '\bTerminateProcess\(')).Count -ne 2 -or ([regex]::Matches($codeOnly, '\bEndProgram\b')).Count -ne 3 -or
        ([regex]::Matches($codeOnly, '\bForceClose\b')).Count -ne 3) {
        $said.Add('the [Code] section ends the program by force otherwise than on End it now or /FORCECLOSE=1')
    }
    # Before anything is changed, in every mode: room, and every file of the folder free (no longer for updates alone).
    $prepare = [regex]::Match($codeOnly, '(?s)function PrepareToInstall\(var NeedsRestart: Boolean\): String;.*?\nend;')
    if (-not $prepare.Success -or ([regex]::Matches($prepare.Value, '\bUpdateMode\b')).Count -ne 1 -or
        -not $prepare.Value.Contains('  Reason := RoomReason;') -or
        -not $prepare.Value.Contains("      FilesInUse(ExpandConstant('{app}'), Busy, Unlisted);`n      AddLeftAside(LeftAside);`n      for I := Busy.Count - 1 downto 0 do`n        if (Unlisted.IndexOf(Busy[I]) < 0) and (LeftAside.IndexOf(Busy[I]) >= 0) then begin`n          Log('In use, and left to the record of the update that set it aside: ' + Busy[I]);`n          Busy.Delete(I);`n        end;`n      Began := GetTickCount;`n      while (Busy.Count > 0) and (TicksSince(Began) < LookMs - UpdateStepMs) do begin`n        Sleep(UpdateStepMs);`n        for I := Busy.Count - 1 downto 0 do begin`n          if TicksSince(Began) >= LookMs then`n            Break;`n          Path := Busy[I];`n          if Unlisted.IndexOf(Path) >= 0 then begin`n            Busy.Delete(I);`n            Unlisted.Delete(Unlisted.IndexOf(Path));`n            FilesInUse(Path, Busy, Unlisted);`n          end else if FreeToReplace(Path) then`n            Busy.Delete(I);`n        end;`n      end;`n      if Busy.Count > 0 then begin")) {
        $said.Add('Preparing to Install does not look at room and files in use in every mode')
    }
    # (What the record of a finishing update names and could not remove yet, a plain file, is left to that record by the
    # look at the files in use: the setup carries it into its own record, and a later one removes it; held by another
    # program, it never stops a setup (34ede5fa; step 38 of test_setup_vm). A redirection is never left so.)
    $addLeftAside = @('procedure AddLeftAside(Paths: TStringList);', 'var', '  Names, Endings, Kinds, Copies: TStringList;', '  Suffix: String;', '  I: Integer;',
        'begin', '  if not ThingAt(InApp(FinishingMarker)) then', '    Exit;', '  Names := TStringList.Create;', '  Endings := TStringList.Create;',
        '  Kinds := TStringList.Create;', '  Copies := TStringList.Create;', '  try', '    if ReadRecord(InApp(FinishingMarker), Suffix, Names, Endings, Kinds, Copies) then',
        '      for I := 0 to Names.Count - 1 do', '        if PlainFileAt(InApp(Names[I]) + Endings[I]) then', '          Paths.Add(InApp(Names[I]) + Endings[I]);',
        '  finally', '    Names.Free;', '    Endings.Free;', '    Kinds.Free;', '    Copies.Free;', '  end;', 'end;') -join "`n"
    if (-not $code.Contains($addLeftAside) -or -not $prepare.Success -or -not $prepare.Value.Contains("    LeftAside := TStringList.Create;`n") -or
        -not $prepare.Value.Contains("      LeftAside.Free;`n      Unlisted.Free;`n      Busy.Free;`n") -or
        -not $prepare.Value.Contains("      FilesInUse(ExpandConstant('{app}'), Busy, Unlisted);`n      AddLeftAside(LeftAside);`n      for I := Busy.Count - 1 downto 0 do`n        if (Unlisted.IndexOf(Busy[I]) < 0) and (LeftAside.IndexOf(Busy[I]) >= 0) then begin`n          Log('In use, and left to the record of the update that set it aside: ' + Busy[I]);`n          Busy.Delete(I);`n        end;`n      Began := GetTickCount;`n") -or
        ([regex]::Matches($code, '\n\s+AddLeftAside\(')).Count -ne 1) {
        $said.Add('Preparing to Install does not leave to its record what a finishing update could not remove')
    }
    # (The first look goes through the whole folder, however long it takes, and only the looks again count 2 s,
    # review F1: a slow disk is no reason to refuse.)
    $walk = [regex]::Match($codeOnly, '(?s)\nprocedure FilesInUse\(.*?\nend;')
    if (-not $walk.Success -or $walk.Value -match '\b(TicksSince|GetTickCount|LookMs)\b') {
        $said.Add('the first look at the files in use stops before it has looked at every file')
    }
    # The earlier version set aside there, last, and nowhere else: a failure is a refusal, with nothing copied
    # (review C7).
    if (-not $prepare.Success -or -not $prepare.Value.Contains("  if Reason = '' then`n    Reason := SetPackageAside;`n  if Reason <> '' then begin") -or
        ([regex]::Matches($codeOnly, '\bSetPackageAside\b')).Count -ne 2) {
        $said.Add('the earlier version is not set aside last in Preparing to Install, where a failure stops the install')
    }
    # (M1: the folder's reason asked in Preparing to Install, before a silent install closes anything; at Install by hand,
    # before anything is closed; and in the set-aside; the proof of an install of ours wherever a file of the folder is
    # set aside, put right by 0.1.2's rule or by a record (review 1.1), or taken for an earlier version's; review i020-2.)
    if (([regex]::Matches($codeOnly, '\bForeignFolderReason\b')).Count -ne 4 -or ([regex]::Matches($codeOnly, '\bOurInstallIn\(')).Count -ne 6) {
        $said.Add('the setup does not install only into a folder of its own or an empty one, as it should')
    }
    # (Review 1.2: and only into a folder that this Windows account and Windows itself alone may change: asked in Preparing
    # to Install after the folder's own reason, and at Install by hand before anything is closed. Refused, never made
    # safe: nothing of a folder's rights is ever written. The folder itself where it is there (no redirection, its owner,
    # every entry that gives another account more than reading and running), else the nearest folder above it, by the
    # entries a folder made in it, or a file made in that, takes (outside review 2, 1.2: an entry that only files take
    # was passed); and every folder above it up to the drive's root (its owner, and its own entries that let another
    # account remove, rename or take it); a network share, a folder without a list, an entry or rights this setup does
    # not read, and an account it cannot tell, refused.)
    foreach ($part in @('  FileObject = 1;', '  OwnerAndList = 5;', '  TokenQuery = $0008;', '  TokenUserClass = 1;', '  DriveRemote = 4;',
        "  TrustedInstallerSid = 'S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464';",
        '  ReadsHigh = $A012;', '  ReadsLow = $00A9;', '  AboveHigh = $130D;', '  AboveLow = $0040;',
        "  RightWords = '|GA10000000|GR80000000|GW40000000|GX20000000|RC00020000|SD00010000|WD00040000|WO00080000|RP00000010|WP00000020|CC00000001|DC00000002|LC00000004|SW00000008|LO00000080|DT00000040|CR00000100|FA001F01FF|FR00120089|FW00120116|FX001200A0|';",
        "  if (Copy(Folder, 1, 2) = '\\') or (GetDriveTypeW(AddBackslash(ExtractFileDrive(Folder))) = DriveRemote) then begin",
        "  Own := OwnAccount;`n  if Own = '' then begin",
        "  Made := GetFileAttributesW(Folder) <> NoFileAttributes;`n  if Made and ThroughRedirection(Folder, '') then begin",
        "  while not Made and (ExtractFileDir(Above) <> Above) do begin`n    Above := ExtractFileDir(Above);`n    Made := GetFileAttributesW(Above) <> NoFileAttributes;`n  end;`n  Rights := RightsOf(Above);`n  if Rights = '' then`n    Why := 'who may change ' + Above + ' could not be told'`n  else`n    Why := OthersMayChange(Above, Rights, Own, True, ReadsHigh, ReadsLow, Above <> Folder, False);",
        "  while (Why = '') and (Up <> '') do begin`n    Rights := RightsOf(Up);`n    if Rights = '' then`n      Why := 'who may change ' + Up + ' could not be told'`n    else`n      Why := OthersMayChange(Up, Rights, Own, False, AboveHigh, AboveLow, False, True);",
        "  if Why <> '' then`n    Result := 'the folder ' + Folder + ' is not one that this Windows account and Windows itself alone may change (' + Why + '): qKVM Desktop is installed only into such a folder';",
        "  Owner := Copy(Rights, 3, At - 3);`n  if not TrustedAccount(Owner, Own) then begin`n    Result := AccountWords(Owner) + ' owns ' + Path;`n    Exit;`n  end;",
        "  if Pos('(', List) = 0 then begin`n    if Pos('NO_ACCESS_CONTROL', List) > 0 then`n      Result := 'anyone may change ' + Path + ', which has no list of who may';`n    Exit;`n  end;`n  if Pos('NO_ACCESS_CONTROL', Copy(List, 1, Pos('(', List) - 1)) > 0 then begin",
        "    if (Field <> 6) or (Entry <> '') or (Pos('(', Copy(Rest, 1, Ends - 1)) > 0) then begin",
        "    if (Kind <> 'A') and (Kind <> 'D') then begin",
        "      if not RightsMask(Words, Top, Bottom) then begin`n        Result := 'an entry of who may change ' + Path + ' gives rights this setup does not read (' + Words + ')';`n        Exit;`n      end;`n      if AnyBut then`n        Dangerous := ((Top and not MaskHigh) <> 0) or ((Bottom and not MaskLow) <> 0)`n      else`n        Dangerous := ((Top and MaskHigh) <> 0) or ((Bottom and MaskLow) <> 0);",
        "      if MadeWithin and not HasPair(Flags, 'CI') and not HasPair(Flags, 'OI') then`n        Dangerous := False;`n      if Itself and HasPair(Flags, 'IO') then`n        Dangerous := False;`n      if Dangerous and not TrustedAccount(Account, Own) then",
        "  Result := (Account = Own) or (Account = 'SY') or (Account = 'BA') or (Account = 'CO') or (Account = 'OW') or`n    (Account = 'S-1-5-18') or (Account = 'S-1-5-32-544') or (Account = 'S-1-3-0') or (Account = 'S-1-3-4') or`n    (Account = TrustedInstallerSid) or ((Account = 'LA') and (Copy(Own, Length(Own) - 3, 4) = '-500'));",
        "    Found := Pos('|' + Copy(Words, At, 2), RightWords);`n    if Found = 0 then`n      Exit;",
        "  Result := False;`n  At := 1;`n  while (At < Length(Words)) and not Result do begin`n    Result := Copy(Words, At, 2) = Pair;`n    At := At + 2;`n  end;",
        "  if Copy(Result, 1, 4) <> 'S-1-' then`n    Result := '';")) {
        if (-not $code.Contains($part)) { $said.Add("the folder's rights are not looked at as they should be: $part") }
    }
    # (And of the program's folder by the same routine as of the folder an update that did not finish left, which the
    # setup's start judges before it puts anything right there: outside review 2, 1.3.)
    if (([regex]::Matches($codeOnly, '\bRightsReason\b')).Count -ne 3 -or ([regex]::Matches($codeOnly, '\bFolderRightsReason\(')).Count -ne 3 -or
        -not $code.Contains("function RightsReason: String;`nvar`n  Folder: String;`nbegin`n  Folder := RemoveBackslashUnlessRoot(ExpandConstant('{app}'));`n  Result := FolderRightsReason(Folder);`n") -or
        $code -match '(?i)SetNamedSecurityInfo|SetFileSecurity|SetSecurityInfo|SetEntriesInAcl|SetKernelObjectSecurity') {
        $said.Add('the setup does not refuse a folder another account may change in Preparing to Install and at Install, or writes rights')
    }
    # (Where it refuses the program's folder for its rights and an update that did not finish left something there, a
    # marker or 0.1.2's program set aside, looked at by name alone and never on a network share, it says that this stays
    # as it is and what to do: nothing is put right in such a folder, and a start of qKVM there says to run the setup.)
    if (-not $code.Contains("  if (Result = '') or (Copy(Folder, 1, 2) = '\\') or (GetDriveTypeW(AddBackslash(ExtractFileDrive(Folder))) = DriveRemote) then`n    Exit;`n  if ThingAt(AddBackslash(Folder) + UnfinishedMarker) or ThingAt(AddBackslash(Folder) + FinishingMarker) or ThingAt(AddBackslash(Folder) + 'qkvm-desktop.exe' + AsideEnding) then`n    Result := Result + '. What an update that did not finish left there stays as it is until then, and qKVM Desktop may not start meanwhile: run this setup again once no other account may change the folder, or uninstall qKVM Desktop in Settings, Apps';`nend;")) {
        $said.Add('the setup does not say, where it refuses a folder for its rights, that what an update left there stays as it is, and what to do')
    }
    # (Outside review 2, 1.1: the Windows account this setup and the uninstall run as, whatever its kind, a local or a
    # domain account's S-1-5-21 or Microsoft Entra ID's S-1-12-1 among them: its token's user read where Windows' own
    # pointer in it says (TOKEN_USER.User.Sid, against where the String's characters begin), only within the bytes
    # Windows wrote, and turned into words by Windows' converter; never at a place of the setup's choosing, and nothing of
    # its authority looked at.)
    $sidBody = [regex]::Match($code, "(?s)\nfunction SidInTokenUser\(var Info: String; Needed: Cardinal\): String;\n(.*?)\nend;\n")
    foreach ($part in @("  Bytes := Needed;`n  if Bytes > 2 * Length(Info) then`n    Exit;",
        "  Base := CharPrevW(Info, Info);`n  BaseLow := Base and `$FFFF;`n  BaseHigh := Base shr 16;`n  if Ord(Info[2]) = BaseHigh then`n    At := Ord(Info[1]) - BaseLow`n  else if Ord(Info[2]) = BaseHigh + 1 then`n    At := Ord(Info[1]) + 65536 - BaseLow`n  else`n    Exit;",
        "  if (At < 8) or ((At mod 2) <> 0) or (At + 8 > Bytes) then`n    Exit;`n  Count := Ord(Info[At div 2 + 1]) div 256;`n  if (Count < 1) or (Count > 15) or (At + 8 + 4 * Count > Bytes) then`n    Exit;`n  Sid := Copy(Info, At div 2 + 1, 4 + 2 * Count);`n  Text := 0;`n  if ConvertSidToStringSidW(Sid, Text) = 0 then`n    Exit;")) {
        if (-not $sidBody.Success -or -not $sidBody.Groups[1].Value.Contains($part)) { $said.Add("the setup does not read the Windows account it runs as where Windows says it is: $part") }
    }
    if (-not $sidBody.Success -or $sidBody.Groups[1].Value -match '\$0500|\$0C00|Info\[[3-9]\]' -or $code -match 'Copy\(Info, 5,' -or
        -not $code.Contains("  Info := StringOfChar(#0, 64);`n  Needed := 0;`n  Read := GetTokenInformation(Token, TokenUserClass, Info, 128, Needed) <> 0;`n  CloseHandle(Token);`n  if Read then`n    Result := SidInTokenUser(Info, Needed);`nend;") -or
        ([regex]::Matches($codeOnly, '\bSidInTokenUser\(')).Count -ne 2 -or ([regex]::Matches($codeOnly, '\bConvertSidToStringSidW\(')).Count -ne 2 -or
        ([regex]::Matches($codeOnly, '\bCharPrevW\(')).Count -ne 2) {
        $said.Add('the setup takes the Windows account it runs as only where it is of one kind, or reads it at a place of its own choosing')
    }
    $files = @($sections['Files'] | ForEach-Object { $_.Trim() } | Where-Object { $_ -and -not $_.StartsWith(';') })
    if ($files -cnotcontains 'Source: "{#SourcePath}qkvm-desktop.installed"; DestDir: "{app}"; Flags: ignoreversion' -or
        $files -cnotcontains '#include "files.iss"' -or $files.Count -ne 6 -or $files[-1] -cne '#include "files.iss"' -or
        # (The update checks first, never copied as such: unpacked by the [Code] section alone, M3; Windows on Arm's
        # where the setup carries its package.)
        (@($files | Select-Object -First 4) -join "`n") -cne (@('Source: "{#SourcePath}qkvm-update-check.exe"; Flags: dontcopy noencryption',
            '#ifdef Arm64PackageDir', 'Source: "{#SourcePath}qkvm-update-check-arm64.exe"; Flags: dontcopy noencryption', '#endif') -join "`n")) {
        $said.Add('the [Files] are not the update check carried, the package and the marker at {app}\qkvm-desktop.installed')
    }
    if (-not (Test-Path -LiteralPath $markerSource) -or (Get-Sha256 $markerSource) -cne $expected[$markerSource.ToLowerInvariant()].sha256) {
        $said.Add('the marker is not "setup" and a line feed')
    }
    # --- The rest of what it is.
    foreach ($line in "AppId={{A964EFA6-87C6-4A23-A9CC-B71F87441433}", 'AppPublisher=qKVM', 'AppVersion={#AppVersion}',
        'MinVersion=10.0.19045', 'CloseApplications=no', 'SetupMutex=qKVM-Desktop-Setup', 'DisableDirPage=yes',
        'DisableWelcomePage=no',
        'DefaultDirName={userpf}\qKVM Desktop', 'UninstallDisplayIcon={app}\qkvm-desktop.exe',
        'OutputBaseFilename=qKVM-Desktop-{#AppVersion}-windows-setup', 'SetupIconFile={#SourcePath}qkvm.ico') {
        if ($setupLines -cnotcontains $line) { $said.Add("the [Setup] section does not say $line") }
    }
    # (Where it carries Windows on Arm's package: wherever Windows runs x64 programs, x64 Windows and Windows 11 on Arm, in
    # 64-bit mode; else on x64 Windows alone, as 0.1.0 to 0.1.2's setups. Nothing else says where it installs.)
    $where = "#ifdef Arm64PackageDir`nArchitecturesAllowed=x64compatible`nArchitecturesInstallIn64BitMode=x64compatible`n#else`nArchitecturesAllowed=x64os`nArchitecturesInstallIn64BitMode=x64os`n#endif"
    if (-not ($script -replace "`r`n", "`n").Contains($where) -or @($setupLines | Where-Object { $_ -match '^Architectures' }).Count -ne 4) {
        $said.Add('the [Setup] section does not say ArchitecturesAllowed=x64compatible and ArchitecturesInstallIn64BitMode=x64compatible with Windows on Arm''s package, and x64os without it')
    }
    if (-not ($script -replace "`r`n", "`n").Contains("#else`nCompression=lzma2`nSolidCompression=yes`n#endif")) { $said.Add('the setup is not compressed with lzma2, solid') }
    # Its version, as the program's updates read it (prepare_update_payload.ps1 -Setup): the fixed file and product
    # versions exactly X.Y.Z.0, and the texts X.Y.Z (Inno Setup pads its version texts with blanks).
    $info = [Diagnostics.FileVersionInfo]::GetVersionInfo($exe)
    $fixed = '{0}.{1}.{2}.{3}' -f $info.FileMajorPart, $info.FileMinorPart, $info.FileBuildPart, $info.FilePrivatePart
    $product = '{0}.{1}.{2}.{3}' -f $info.ProductMajorPart, $info.ProductMinorPart, $info.ProductBuildPart, $info.ProductPrivatePart
    if ($fixed -cne "$version.0" -or $product -cne "$version.0" -or "$($info.FileVersion)".Trim() -cne $version -or "$($info.ProductVersion)".Trim() -cne $version) {
        $said.Add("the setup does not say the package's version ($version): file $fixed '$($info.FileVersion)', product $product '$($info.ProductVersion)'")
    }
    if ((Get-Sha256 (Join-Path $inputs 'qkvm.ico')) -cne (Get-Sha256 (Join-Path $desktop 'ui/brand/qkvm.ico'))) { $said.Add('the setup''s icon is not the program''s') }
    # --- Nothing run but the program itself, nothing asked of the network.
    $run = @($sections['Run'] | ForEach-Object { $_.Trim() } | Where-Object { $_ -and -not $_.StartsWith(';') })
    # (Each with the one argument --after-update: the program then starts itself again through the shell, a copy that
    # inherits nothing of the setup, Windows' junction guard included. "Start qKVM Desktop" on the last page, ticked or
    # not. Every entry is postinstall: Inno Setup runs one without it before the post-install step, while the copy is not
    # yet flushed and marked finished, and the program it starts holds its own image, which the flush and a put-back must
    # open; the copy then never counts, and the update is left unfinished (rehearsal on Windows on Arm, 2026-11-19). An
    # update's own start, and a silent install's that closed the program, is made in [Code] after that, StartTheCopy.)
    $wantedRun = @('Filename: "{app}\qkvm-desktop.exe"; Parameters: "--after-update"; Description: "Start qKVM Desktop"; Flags: nowait postinstall skipifsilent; Check: StartTicked',
        'Filename: "{app}\qkvm-desktop.exe"; Parameters: "--after-update"; Description: "Start qKVM Desktop"; Flags: nowait postinstall skipifsilent unchecked; Check: StartUnticked')
    if (($run -join "`n") -cne ($wantedRun -join "`n")) { $said.Add('something else than the program is run, or the program otherwise') }
    if (@($run | Where-Object { $_ -match '^Filename:' -and $_ -notmatch '\bFlags:[^;]*\bpostinstall\b' }).Count) {
        $said.Add('a [Run] entry starts something before the copy is flushed and marked finished')
    }
    # Windows' junction guard stays on for the setup and the uninstall, as Inno Setup has it unless told otherwise.
    if (@($setupLines | Where-Object { $_ -imatch '^RedirectionGuard\s*=\s*no\b' }).Count) { $said.Add('the setup turns RedirectionGuard off') }
    # Nothing deleted before the copy, and no folder walked to remove anything: plugins and qml held a person's file
    # and another program's as well (review i020-1, M4). The one look through a folder, every entry of it, is the look
    # at the files in use, which removes nothing.
    if ((Test-Path -LiteralPath (Join-Path $inputs 'kept.iss')) -or $code -match '(?i)kept\.iss|\bRemoveStale|\bKeptFiles\b' -or
        ([regex]::Matches($code, [regex]::Escape("FindFirst(Folder + '\*', Found)"))).Count -ne 1 -or
        # (And the look at the folder's entries, whether it holds anything but what a setup of ours leaves there, and within
        # folders of the setup's own names (OtherWithin; review i020-2 M1, reviews 1.1 and 1.8), once, which removes
        # nothing; and no look by a pattern but these two and that for what a setup ended halfway left.)
        ([regex]::Matches($code, [regex]::Escape("FindFirst(AddBackslash(Base) + '*', Found)"))).Count -ne 1 -or
        -not [regex]::Match($code, '(?s)\nfunction OtherWithin\(.*?\nend;').Success -or
        [regex]::Match($code, '(?s)\nfunction OtherWithin\(.*?\nend;').Value -match '\b(DeleteFile|RemoveOwn|RenameFile|RemoveDir|DelTree)\b' -or
        ([regex]::Matches($code, "FindFirst\([^;\n]*\*")).Count -ne 3 -or
        -not $code.Contains("procedure FilesInUse(Folder: String; Busy, Unlisted: TStringList);`nvar`n  Found: TFindRec;`n  Path, Name, Data, CutShort: String;`n  Attributes: Cardinal;`n  Error: Integer;`nbegin`n  if not FindFirst(Folder + '\*', Found) then begin")) {
        $said.Add('the [Code] section walks a folder of the program''s to remove what is in it')
    }
    # What a setup ended halfway left is looked for only in the package's own folders: by the package's own list, made
    # into the setup at build time (folders.iss, from package-manifest.json), never read from the folder.
    $foldersFile = Join-Path $inputs 'folders.iss'
    $foldersText = if (Test-Path -LiteralPath $foldersFile) { [IO.File]::ReadAllText($foldersFile) -replace "`r`n", "`n" } else { '' }
    foreach ($kind in 'x64', 'arm64') {
        $part = [regex]::Match($foldersText, "(?s)\nfunction PackageFolders$($suffixOf[$kind]): String;\nbegin\n  Result := '\|';\n(.*?)end;\n")
        $listedFolders = @(if ($part.Success) { [regex]::Matches($part.Groups[1].Value, "(?m)^  Result := Result \+ '([^']*)\|';$") | ForEach-Object { $_.Groups[1].Value } })
        $ownFolders = [Collections.Generic.SortedSet[string]]::new([StringComparer]::Ordinal)
        if ($carried.Contains($kind)) {
            [void] $ownFolders.Add('')
            foreach ($path in @($carried[$kind].Manifest.files | ForEach-Object { [string] $_.path })) { [void] $ownFolders.Add((Split-Path -Parent ($path -replace '/', '\'))) }
        }
        if (-not $part.Success -or ($listedFolders -join "`n") -cne (@($ownFolders) -join "`n")) {
            $said.Add("the setup's list of the package's folders is not the package's ($kind, $($listedFolders.Count) for $($ownFolders.Count))")
        }
    }
    if (@(($foldersText -split "`n") | Where-Object { $_ -and -not $_.StartsWith('//') -and $_ -cnotmatch "\A(  Result := Result \+ '[^']*\|';|function PackageFolders(X64|Arm64): String;|begin|  Result := '\|';|end;)\z" }).Count -or
        ([regex]::Matches($foldersText, '(?m)^function ')).Count -ne 2) {
        $said.Add('folders.iss is not the one function of each kind that says the package''s folders, and nothing else')
    }
    foreach ($part in @('  if CurStep = ssPostInstall then begin',
        "  if CurStep = ssInstall then begin`n    Installing := True;",
        # (What a refusal left to start at the end goes as the copy begins: never a start the person did not tick, after a
        # try again that went on; review C8.)
        "    StartAtEnd := False;`n  end;",
        "    PostInstalled := True;", 'procedure DeinitializeSetup;',
        # What a setup ended halfway left in the folder and every folder in it (Inno Setup's temporary files, is- and ten
        # letters or digits and .tmp, files only, no redirection followed), removed after a whole copy and at an uninstall,
        # with the program an update ended halfway set aside; nothing else.
        'function SetupLeftover(Name: String): Boolean;',
        "  Result := (Length(Name) = 17) and (Lowercase(Copy(Name, 1, 3)) = 'is-') and (Lowercase(Copy(Name, 14, 4)) = '.tmp');",
        "  for I := 4 to 13 do`n    if Result and (Pos(Uppercase(Copy(Name, I, 1)), 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789') = 0) then`n      Result := False;",
        '#include "folders.iss"', 'procedure RemoveSetupLeftoversIn(Base, Relative: String);',
        "  if ThroughRedirection(Base, Relative) then begin`n    Log('Left as it is, a redirection: ' + Relative);`n    Exit;`n  end;",
        "  Folder := RemoveBackslashUnlessRoot(Base);`n  if Relative <> '' then`n    Folder := Folder + '\' + Relative;`n  if not FindFirst(Folder + '\is-*.tmp', Found) then",
        "      if ((Found.Attributes and (DirectoryAttribute or ReparsePointAttribute)) = 0) and SetupLeftover(Found.Name) then begin`n        Entry := Folder + '\' + Found.Name;`n        if DeleteFile(Entry) then",
        "          Log('Could not remove what a setup ended halfway left: ' + Entry);`n      end;`n    until not FindNext(Found);",
        'function ThroughRedirection(Base, Relative: String): Boolean;',
        "  Path := RemoveBackslash(Base);`n  if (Length(Path) > 2) and FindFirst(Path, Found) then begin`n    Result := (Found.Attributes and ReparsePointAttribute) <> 0;`n    FindClose(Found);`n  end;`n  Rest := Relative;`n  while (Rest <> '') and not Result do begin",
        "    Path := Path + '\' + Copy(Rest, 1, At - 1);`n    Rest := Copy(Rest, At + 1, Length(Rest));",
        "    if FindFirst(Path, Found) then begin`n      Result := (Found.Attributes and ReparsePointAttribute) <> 0;",
        'procedure RemoveSetupLeftoversOf(Base: String);', "procedure RemoveSetupLeftovers;`nbegin`n  RemoveSetupLeftoversOf(ExpandConstant('{app}'));`nend;",
        "  Rest := PackageFolders;`n  Rest := Copy(Rest, 2, Length(Rest));`n  At := Pos('|', Rest);`n  while At > 0 do begin`n    RemoveSetupLeftoversIn(Base, Copy(Rest, 1, At - 1));`n    Rest := Copy(Rest, At + 1, Length(Rest));`n    At := Pos('|', Rest);`n  end;",
        # (The install step's own line: misnamed, it would silently do nothing.)
        'procedure CurStepChanged(CurStep: TSetupStep);',
        "    if Finished then begin`n      // (The kind it installed, beside the version Inno Setup notes, in its entry of Settings > Apps.)`n      if not RegWriteStringValue(HKEY_CURRENT_USER, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{A964EFA6-87C6-4A23-A9CC-B71F87441433}_is1', 'qKVM Desktop Architecture', ThisKind) then`n        Log('The kind of Windows this install is for could not be noted in its entry of Settings > Apps.');`n      RemoveEarlierFiles(EarlierHere, EarlierKindHere);`n    end;`n    RemoveSetupLeftovers;",
        'procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);',
        "    RemoveSetupLeftovers;`n    DropSetAside;`n  end;",
        "  if SetAside and not PostInstalled then`n    if PutPackageBack and UpdateMode then`n      StartAgain;",
        # (A refusal of an older version after a put-back says what was put back, never that nothing was changed; L4.)
        "    if PutRightDone = '' then`n      Reason := 'Nothing was changed.'`n    else`n      Reason := 'First, this setup ' + PutRightDone + '; nothing else was changed.';",
        "      if Count > 0 then`n        SayPutRight('put back the earlier version, which an update that did not finish had set aside');",
        # (L9 of review i020-2: what was put right said, in full or in part, by every way of putting right; this setup's own
        # undoing never said so; the result line of 7 never "nothing was changed" after it.)
        "  if PutRightDone = '' then`n    PutRightDone := What`n  else if Pos(What, PutRightDone) = 0 then`n    PutRightDone := PutRightDone + ', and ' + What;",
        "  if PutRightDone = '' then`n    Result := 'qKVM-RESULT: 7 nothing was changed: ' + Reason`n  else`n    Result := 'qKVM-RESULT: 7 first, Setup ' + PutRightDone + ', and changed nothing else: ' + Reason;",
        "    if DeleteFile(Aside) then begin`n      Log('Removed the earlier program, set aside by a setup before.');`n      SayPutRight('removed the earlier program that a setup of 0.1.2 had set aside, as a program stood in its place');",
        "  end else if RenameFile(Aside, Base) then begin`n    Log('Put the earlier program back, set aside by a setup ended halfway.');`n    SayPutRight('put back the earlier program that a setup of 0.1.2 had set aside');",
        "  if Removed > 0 then`n    SayPutRight('removed the earlier version that an update whose copy was whole had set aside');",
        "    if Result <> '' then begin`n      // (What could be put back, is: said so, never that nothing was changed, L9.)`n      if Count > 0 then`n        SayPutRight('put back part of the earlier version, which an update that did not finish had set aside');`n      Exit;`n    end;",
        "  Done := PutRightDone;`n  if PutRight(ExpandConstant('{app}')) = '' then`n    SetAside := False`n  else`n    Log('What was set aside for the copy could not all be put back yet.');`n  PutRightDone := Done;",
        "  Done := PutRightDone;`n  if SetAside then begin`n    if PutPackageBack then begin`n      PutRightDone := Done;",
        "    Log(UnchangedLine(Reason));`n    if PutRightDone = '' then`n      Result := 'Nothing was changed: ' + Reason + '.'`n    else`n      Result := 'First, this setup ' + PutRightDone + ', and changed nothing else: ' + Reason + '.';")) {
        if (-not $code.Contains($part)) { $said.Add("the removal after the copy lacks: $part") }
    }
    # The earlier version set aside for the copy (0.2.0, i020): in every mode, last in Preparing to Install, where the
    # folder holds any file of the package's, the marker of an unfinished update written first and then each such file
    # renamed to its name and the ending, the program's own first, by the package's own list (package.iss, made at build
    # time from package-manifest.json, never read from the folder); a file that cannot be set aside, or that is not a
    # plain file, or one in its way, a refusal with what was set aside put back and nothing copied (review C7). After a
    # whole copy, first, the marker made that of a finishing update in one rename and only then what was set aside
    # removed; nothing removed where the rename cannot be made. When the copy does not complete, everything put back,
    # tried again a second apart. At an uninstall, what was set aside and the markers removed. And every setup first puts
    # right what an update that did not finish left (PutRight, before its refusal of an older version): with the first
    # marker every file of the package's names put back (the copy's file of that name removed first), the program's own
    # last; with the second, each removed; the markers last; with neither, the program's own file as 0.1.2 set it aside;
    # with either, a file set aside of a name not the package's, in a folder of the package's, a refusal that names it
    # and touches nothing.
    $packageFile = Join-Path $inputs 'package.iss'
    $packageText = if (Test-Path -LiteralPath $packageFile) { [IO.File]::ReadAllText($packageFile) -replace "`r`n", "`n" } else { '' }
    foreach ($kind in 'x64', 'arm64') {
        $parts = @(foreach ($list in 'PackageFiles', 'PackageFileBytes', 'PackageFileSha256') {
            $part = [regex]::Match($packageText, "(?s)\nfunction $list$($suffixOf[$kind]): String;\nbegin\n  Result := '\|';\n(.*?)end;\n")
            if ($part.Success) { $part.Groups[1].Value } else { $null } })
        $listedFiles = @(if ($null -ne $parts[0]) { [regex]::Matches($parts[0], "(?m)^  Result := Result \+ '([^']+)\|';$") | ForEach-Object { $_.Groups[1].Value } })
        # (And the bytes of each, as the package's manifest gives them: what a copy is held to before it counts, M1; by
        # their size and their SHA-256, review i020-2 L7.)
        $listedBytes = @(if ($null -ne $parts[1]) { [regex]::Matches($parts[1], "(?m)^  Result := Result \+ '([0-9]+)\|';$") | ForEach-Object { $_.Groups[1].Value } })
        $listedSums = @(if ($null -ne $parts[2]) { [regex]::Matches($parts[2], "(?m)^  Result := Result \+ '([0-9a-f]{64})\|';$") | ForEach-Object { $_.Groups[1].Value } })
        $ownFiles = @(); $bytesOf = @{}; $sumOf = @{}
        if ($carried.Contains($kind)) {
            $one = $carried[$kind]
            foreach ($entry in $one.Manifest.files) { $bytesOf[([string] $entry.path) -replace '/', '\'] = [string] [long] $entry.bytes; $sumOf[([string] $entry.path) -replace '/', '\'] = ([string] $entry.sha256).ToLowerInvariant() }
            $bytesOf['package-manifest.json'] = [string] (Get-Item -LiteralPath (Join-Path $one.Folder 'package-manifest.json')).Length
            $sumOf['package-manifest.json'] = Get-Sha256 (Join-Path $one.Folder 'package-manifest.json')
            $ownOrder = [string[]] @(@($one.Manifest.files | ForEach-Object { [string] $_.path }) + 'package-manifest.json' | Where-Object { $_ -cne 'qkvm-desktop.exe' })
            [Array]::Sort($ownOrder, [StringComparer]::Ordinal)
            $ownFiles = @('qkvm-desktop.exe') + @($ownOrder | ForEach-Object { $_ -replace '/', '\' })
        }
        if (@($parts | Where-Object { $null -eq $_ }).Count -or ($listedFiles -join "`n") -cne ($ownFiles -join "`n")) {
            $said.Add("the setup's list of what it sets aside is not the package's, the program's own first ($kind, $($listedFiles.Count) for $($ownFiles.Count))")
        }
        if (($listedBytes -join "`n") -cne (@($ownFiles | ForEach-Object { $bytesOf[$_] }) -join "`n")) {
            $said.Add("the setup's list of the bytes of its files is not the package's ($kind, $($listedBytes.Count) for $($ownFiles.Count))")
        }
        if (($listedSums -join "`n") -cne (@($ownFiles | ForEach-Object { $sumOf[$_] }) -join "`n")) {
            $said.Add("the setup's list of the SHA-256 of its files is not the package's ($kind, $($listedSums.Count) for $($ownFiles.Count))")
        }
    }
    if (([regex]::Matches($packageText, '(?m)^function ')).Count -ne 6 -or
        @(($packageText -split "`n") | Where-Object { $_ -and -not $_.StartsWith('//') -and $_ -cnotmatch "\A(  Result := Result \+ '[^']+\|';|function PackageFiles(X64|Arm64): String;|function PackageFileBytes(X64|Arm64): String;|function PackageFileSha256(X64|Arm64): String;|begin|  Result := '\|';|end;)\z" }).Count) {
        $said.Add('package.iss is not the three functions of each kind that say the package''s files, their bytes and their SHA-256, and nothing else')
    }
    # What earlier versions of ours installed and this one has not (0.2.0, i020 step 4, review i020-1 M4): by the list of
    # what each published version installed, the versions before this package's. All of them once, whatever their case
    # (EarlierFiles: what a record may name as the setup's own); and each version's own, each name once whatever its
    # case (EarlierFilesOf), with its folders, a folder after every folder within it, never the program's folder itself
    # (EarlierFoldersOf), removed only where that version was installed in that very folder. Made at build time into
    # earlier.iss, never read from the folder.
    $earlierFile = Join-Path $inputs 'earlier.iss'
    $earlierText = if (Test-Path -LiteralPath $earlierFile) { [IO.File]::ReadAllText($earlierFile) -replace "`r`n", "`n" } else { '' }
    $byVersion = {
        param([string] $Part)
        $found = [ordered]@{}
        $at = $null
        foreach ($line in $Part -split "`n") {
            if ($line -cmatch "\A  if \(Version = '([^']+)'\) and \(Kind = '([^']+)'\) then begin\z") { $at = "$($Matches[1]) $($Matches[2])"; $found[$at] = [Collections.Generic.List[string]]::new() }
            elseif ($line -cmatch "\A    Result := Result \+ '([^']+)\|';\z" -and $null -ne $at) { $found[$at].Add($Matches[1]) }
            elseif ($line -ceq '  end;') { $at = $null }
        }
        return $found
    }
    # (What each version of each kind installed: the published versions before this package's, and this version's own
    # packages as the setup carries them.)
    $publishedList = Get-Content -LiteralPath $Published -Raw | ConvertFrom-Json
    $installedBy = [ordered]@{}
    foreach ($release in @($publishedList.versions)) {
        if ([version] [string] $release.version -ge [version] $version) { continue }
        foreach ($build in @($release.builds)) { $installedBy["$($release.version) $($build.architecture)"] = @($build.files | ForEach-Object { [string] $_ }) }
    }
    foreach ($one in $carried.Values) { $installedBy["$version $($one.Kind)"] = @($one.Manifest.files | ForEach-Object { [string] $_.path }) }
    foreach ($kind in 'x64', 'arm64') {
        $suffix = $suffixOf[$kind]
        $all = [regex]::Match($earlierText, "(?s)(?:\A|\n)function EarlierFiles$($suffix): String;\nbegin\n  Result := '\|';\n(.*?)end;\n")
        $of = [regex]::Match($earlierText, "(?s)\nfunction EarlierFilesOf$($suffix)\(Version, Kind: String\): String;\nbegin\n  Result := '\|';(.*?)\nend;\n")
        $foldersOf = [regex]::Match($earlierText, "(?s)\nfunction EarlierFoldersOf$($suffix)\(Version, Kind: String\): String;\nbegin\n  Result := '\|';(.*?)\nend;\n")
        $listedEarlier = @(if ($all.Success) { [regex]::Matches($all.Groups[1].Value, "(?m)^  Result := Result \+ '([^']+)\|';$") | ForEach-Object { $_.Groups[1].Value } })
        $listedOf = if ($of.Success) { & $byVersion $of.Groups[1].Value } else { [ordered]@{} }
        $listedFoldersOf = if ($foldersOf.Success) { & $byVersion $foldersOf.Groups[1].Value } else { [ordered]@{} }
        $wantEarlier = [Collections.Generic.List[string]]::new()
        $wantOf = [ordered]@{}
        $wantFoldersOf = [ordered]@{}
        if ($carried.Contains($kind)) {
            $ours = [Collections.Generic.HashSet[string]]::new([string[]] @(@($carried[$kind].Manifest.files | ForEach-Object { [string] $_.path }) + 'package-manifest.json' + 'qkvm-desktop.installed'), [StringComparer]::OrdinalIgnoreCase)
            $seenEarlier = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
            foreach ($at in $installedBy.Keys) {
                if ($at -ceq "$version $kind") { continue }
                $seenHere = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
                $own = [Collections.Generic.List[string]]::new()
                foreach ($path in @($installedBy[$at])) {
                    if ($ours.Contains($path)) { continue }
                    if ($seenEarlier.Add($path)) { $wantEarlier.Add(($path -replace '/', '\')) }
                    if ($seenHere.Add($path)) { $own.Add(($path -replace '/', '\')) }
                }
                $ownSorted = [string[]] $own.ToArray()
                [Array]::Sort($ownSorted, [StringComparer]::Ordinal)
                $folders = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
                foreach ($path in $ownSorted) {
                    $cut = $path.LastIndexOf('\')
                    while ($cut -gt 0) { [void] $folders.Add($path.Substring(0, $cut)); $cut = $path.LastIndexOf('\', $cut - 1) }
                }
                $backwards = [string[]] @($folders)
                [Array]::Sort($backwards, [StringComparer]::Ordinal)
                [Array]::Reverse($backwards)
                $wantOf[$at] = @($ownSorted)
                $wantFoldersOf[$at] = @($backwards)
            }
        }
        $wantEarlierSorted = [string[]] $wantEarlier.ToArray()
        [Array]::Sort($wantEarlierSorted, [StringComparer]::Ordinal)
        if (-not $all.Success -or ($listedEarlier -join "`n") -cne ($wantEarlierSorted -join "`n")) {
            $said.Add("the setup's list of what earlier versions installed and this one has not is not the published list's ($kind, $($listedEarlier.Count) for $($wantEarlierSorted.Count))")
        }
        $saidOf = (@($listedOf.Keys | ForEach-Object { "$_=" + (@($listedOf[$_]) -join ',') }) -join ';')
        $wantedOf = (@($wantOf.Keys | ForEach-Object { "$_=" + (@($wantOf[$_]) -join ',') }) -join ';')
        if (-not $of.Success -or $saidOf -cne $wantedOf) { $said.Add("the setup's list of what each earlier version installed and this one has not is not the published list's ($kind, $saidOf)") }
        $saidFoldersOf = (@($listedFoldersOf.Keys | ForEach-Object { "$_=" + (@($listedFoldersOf[$_]) -join ',') }) -join ';')
        $wantedFoldersOf = (@($wantFoldersOf.Keys | ForEach-Object { "$_=" + (@($wantFoldersOf[$_]) -join ',') }) -join ';')
        if (-not $foldersOf.Success -or $saidFoldersOf -cne $wantedFoldersOf) { $said.Add("the setup's list of each earlier version's folders is not theirs, a folder after every folder within it ($kind, $saidFoldersOf)") }
    }
    if (([regex]::Matches($earlierText, '(?m)^function ')).Count -ne 6 -or
        @(($earlierText -split "`n") | Where-Object { $_ -and -not $_.StartsWith('//') -and $_ -cnotmatch "\A(  Result := Result \+ '[^']+\|';|    Result := Result \+ '[^']+\|';|  if \(Version = '\d+\.\d+\.\d+'\) and \(Kind = '(x64|arm64)'\) then begin|  end;|function EarlierFiles(X64|Arm64): String;|function EarlierFilesOf(X64|Arm64)\(Version, Kind: String\): String;|function EarlierFoldersOf(X64|Arm64)\(Version, Kind: String\): String;|begin|  Result := '\|';|end;)\z" }).Count) {
        $said.Add('earlier.iss is not the three functions of each kind that say the earlier versions'' files, each one''s own and its folders, and nothing else')
    }
    if ($code -cnotmatch "\n  UnfinishedMarker = '$([regex]::Escape($UnfinishedMarker))';\n" -or $code -cnotmatch "\n  AsideEnding = '$([regex]::Escape($AsideEnding))';\n" -or
        $code -cnotmatch "\n  FinishingMarker = 'qkvm-desktop\.update-finishing';\n" -or $UnfinishedMarker -ceq 'qkvm-desktop.update-finishing') {
        $said.Add("the setup's marker of an unfinished update, or the ending of what it sets aside, is not the program's (ui/win/unfinished_update.h: $UnfinishedMarker, $AsideEnding)")
    }
    # (The record in the marker, as the program reads it, ui/win/unfinished_update.h: its kind and its last line; the
    # endings a set-aside gives, AsideEnding first and then .2 to .9 before it; the kinds of its lines; the draft it is
    # written under before it is the marker.)
    $wantEndings = '|' + ((@($AsideEnding) + @(2..9 | ForEach-Object { ".$_$AsideEnding" })) -join '|') + '|'
    if ($code -cnotmatch "\n  RecordKind = '$([regex]::Escape($RecordKind))';\n" -or $code -cnotmatch "\n  RecordEnd = '$([regex]::Escape($RecordEnd))';\n" -or
        $code -cnotmatch "\n  SetAsideEndings = '$([regex]::Escape($wantEndings))';\n" -or $code -cnotmatch "\n  AsideKind = 'aside';\n" -or
        $code -cnotmatch "\n  LeftKind = 'left';\n" -or $code -cnotmatch "\n  RecordDraft = 'qkvm-desktop\.update-record';\n") {
        $said.Add("the setup's record of what it sets aside is not the one the program reads (ui/win/unfinished_update.h: $RecordKind, $RecordEnd)")
    }
    # (Never a look for files by their ending: what is the setup's own is what a record names, and nothing else.)
    if ($code -match "'\*' \+ AsideEnding|\+ '\*' \+ AsideEnding|\*\.old-update" -or $code -match 'StrangerSetAside') {
        $said.Add('the [Code] section looks for files by the ending of what an update set aside')
    }
    foreach ($part in @('#include "package.iss"', 'function SetPackageAside: String;', 'function PutRight(Folder: String): String;',
        '  PutBackTries = 5;', '  FlushTries = 20;', '  FlushStepMs = 250;',
        "  Rest := Copy(List, 2, Length(List));`n  At := Pos('|', Rest);`n  while At > 0 do begin`n    Names.Add(Copy(Rest, 1, At - 1));`n    Rest := Copy(Rest, At + 1, Length(Rest));`n    At := Pos('|', Rest);`n  end;",
        "  Result := FindFirst(Path, Found);`n  if Result then`n    FindClose(Found);",
        "  Result := False;`n  if FindFirst(Path, Found) then begin`n    Result := (Found.Attributes and (DirectoryAttribute or ReparsePointAttribute)) = 0;",
        "  Result := ExpandConstant('{app}\') + Name;",
        # (The setup's own: a name of its package's or one a published version installed, and an ending it gives.)
        "  Result := (Name <> '') and (Pos('|', Name) = 0) and (Pos('|' + Lowercase(Name) + '|', Lowercase(PackageFiles + EarlierFiles)) > 0);",
        "  Result := (Suffix <> '') and (Pos('|', Suffix) = 0) and (Pos('|' + Suffix + '|', SetAsideEndings) > 0);",
        # (L6 of review i020-2: a record read only where it is of at most 1 MiB, as the program reads it.)
        "  MostRecord = 1048576;",
        "  Result := False;`n  if FindFirst(Path, Found) then begin`n    Result := (Found.SizeHigh = 0) and (Found.SizeLow <= Most);`n    FindClose(Found);`n  end;",
        # (A record read only where whole, of its kind, its ending first and its own, and every line one of its kinds that
        # names the setup's own; else nothing is decided by it.)
        "  if not PlainFileAt(Path) then`n    Exit;`n  // (Of at most MostRecord, before it is read: a larger file of anything else's is not loaded whole, review i020-2 L6.)`n  if not AtMost(Path, MostRecord) then`n    Exit;`n  if not LoadStringsFromFile(Path, Lines) then`n    Exit;`n  Count := GetArrayLength(Lines);`n  while (Count > 0) and (Lines[Count - 1] = '') do`n    Count := Count - 1;`n  if Count < 4 then`n    Exit;",
        "  if (Lines[1] <> RecordKind) or (Lines[Count - 1] <> RecordEnd) or (Copy(Lines[2], 1, 7) <> 'ending ') then`n    Exit;`n  Suffix := Copy(Lines[2], 8, Length(Lines[2]));`n  if not OwnEnding(Suffix) then`n    Exit;`n  for I := 3 to Count - 2 do begin",
        "    if Copy(Line, 1, 6) = 'aside ' then begin`n      if not OwnName(Copy(Line, 7, Length(Line))) then`n        Exit;`n      Names.Add(Copy(Line, 7, Length(Line)));`n      Endings.Add(Suffix);`n      Kinds.Add(AsideKind);",
        "      At := Pos(' ', Value);`n      if At = 0 then`n        Exit;`n      if not OwnEnding(Copy(Value, 1, At - 1)) or not OwnName(Copy(Value, At + 1, Length(Value))) then`n        Exit;`n      Names.Add(Copy(Value, At + 1, Length(Value)));`n      Endings.Add(Copy(Value, 1, At - 1));`n      Kinds.Add(LeftKind);`n    end else if Copy(Line, 1, 5) = 'copy ' then begin`n      if not OwnName(Copy(Line, 6, Length(Line))) then`n        Exit;`n      Copies.Add(Copy(Line, 6, Length(Line)));`n    end else`n      Exit;`n  end;`n  Result := True;",
        # (The record whole and on the disk before it is the marker, and before anything is renamed: a draft of the
        # setup's own name, flushed by Windows, renamed in one step; nothing of it left where it could not be.)
        "  Handle := CreateFileW(Path, GenericWrite, FileShareAll, 0, OpenExisting, 0, 0);`n  Result := FlushFileBuffers(Handle) <> 0;`n  CloseHandle(Handle);",
        "  Draft := InApp(RecordDraft);`n  if ThingAt(Draft) then`n    if not RemoveOwn(Draft) then`n      Exit;`n  if SaveStringToFile(Draft, Text, False) then`n    if FlushedFile(Draft) then`n      if RenameFile(Draft, InApp(Marker)) then`n        Result := '';`n  if Result <> '' then`n    if ThingAt(Draft) then`n      RemoveOwn(Draft);",
        # (L1 of review i020-2: the unfinished marker held open without the right for anything else to remove it, from the
        # record's writing until it is renamed or this setup puts right; where it is gone at the end of a whole copy, the
        # record written again as the finishing marker, never a put-back.)
        "  MarkerHold := CreateFileW(InApp(UnfinishedMarker), GenericRead, FileShareReadWrite, 0, OpenExisting, 0, 0);",
        "  if MarkerHold <> 0 then`n    CloseHandle(MarkerHold);`n  MarkerHold := 0;",
        "  Result := '';`n  // (This setup's hold of its own marker let go of first: what it puts right may remove or rename it, L1.)`n  LetGoOfMarker;",
        "      Result := WriteRecord(Text, UnfinishedMarker);`n      // (Held from now on, and kept, for the copy's end, review i020-2 L1; where it cannot be held as this setup's own,`n      // nothing is renamed, and the marker, which names nothing set aside, is the next setup's to take away, review 1.4.)`n      if Result = '' then begin`n        RecordText := Text;`n        if not HoldMarker(Text) then begin`n          RecordText := '';`n          Result := 'the marker of this update could not be held for as long as this setup works (' + UnfinishedMarker + ')';`n        end;`n      end;",
        "  LetGoOfMarker;`n  Result := RenameFile(InApp(UnfinishedMarker), InApp(FinishingMarker));",
        "  if not Result and not ThingAt(InApp(UnfinishedMarker)) and (RecordText <> '') then begin`n    Result := WriteRecord(RecordText, FinishingMarker) = '';",
        '  GenericWrite = $40000000;', '  FileShareAll = 7;',
        # (Each set-aside's ending the first that no file of the package's has beside it: a file of that name and ending
        # which is not the setup's is never in its way.)
        "      Taken := False;`n      for I := 0 to Names.Count - 1 do`n        if ThingAt(InApp(Names[I]) + Suffix) then`n          Taken := True;`n      if not Taken then`n        Result := Suffix;",
        "    AddNames(PackageFiles, Names);`n    Rest := Copy(SetAsideEndings, 2, Length(SetAsideEndings));",
        # (Set aside: what an update left put right first; a marker that could not be taken away, a refusal; a refusal,
        # before the marker and so before any rename, where a file of the list is not a plain file; nothing, and no
        # marker, where nothing of the package's is there; the record of each file there, with the ending, and of what
        # an earlier update could not remove, written first; that earlier update's marker gone; each file renamed in its
        # turn, the program's own first.)
        "  Result := PutRight(ExpandConstant('{app}'));`n  if Result <> '' then begin`n    Result := 'what an update that did not finish left could not be put right: ' + Result;`n    Exit;`n  end;`n  // (A marker PutRight could not take away: nothing new is recorded beside it.)`n  if ThingAt(InApp(UnfinishedMarker)) then begin`n    Result := 'the marker of an earlier update could not be removed (' + UnfinishedMarker + ')';`n    Exit;`n  end;",
        "    AddNames(PackageFiles, Names);`n    Count := 0;`n    for I := 0 to Names.Count - 1 do begin`n      Path := InApp(Names[I]);`n      if (Result = '') and ThingAt(Path) then begin`n        if PlainFileAt(Path) then`n          Count := Count + 1`n        else`n          Result := Format('a file of qKVM Desktop is not a plain file (%s)', [Names[I]]);`n      end;`n    end;`n    if (Result = '') and (Count > 0) then begin`n      Suffix := FreeEnding;`n      if Suffix = '' then",
        "      if ThingAt(InApp(FinishingMarker)) then`n        if ReadRecord(InApp(FinishingMarker), Old, Carried, CarriedEndings, Kinds, Copies) then`n          for I := Carried.Count - 1 downto 0 do`n            if not ThingAt(InApp(Carried[I]) + CarriedEndings[I]) then begin",
        "Text := 'qKVM Desktop {#AppVersion} is being installed here; the version before it is set aside beside this file.' + #13#10 + RecordKind + #13#10 + 'ending ' + Suffix + #13#10;`n      for I := 0 to Names.Count - 1 do`n        if ThingAt(InApp(Names[I])) then`n          Text := Text + 'aside ' + Names[I] + #13#10;`n      for I := 0 to Carried.Count - 1 do`n        Text := Text + 'left ' + CarriedEndings[I] + ' ' + Carried[I] + #13#10;`n      for I := 0 to Names.Count - 1 do`n        Text := Text + 'copy ' + Names[I] + #13#10;`n      Text := Text + RecordEnd + #13#10;`n      Result := WriteRecord(Text, UnfinishedMarker);`n      // (Held from now on, and kept, for the copy's end, review i020-2 L1; where it cannot be held as this setup's own,`n      // nothing is renamed, and the marker, which names nothing set aside, is the next setup's to take away, review 1.4.)`n      if Result = '' then begin`n        RecordText := Text;`n        if not HoldMarker(Text) then begin`n          RecordText := '';`n          Result := 'the marker of this update could not be held for as long as this setup works (' + UnfinishedMarker + ')';`n        end;`n      end;`n      if (Result = '') and ThingAt(InApp(FinishingMarker)) then`n        if not RemoveOwn(InApp(FinishingMarker)) then begin",
        "      if Result = '' then begin`n        SetAside := True;`n        Count := 0;`n        for I := 0 to Names.Count - 1 do begin`n          Path := InApp(Names[I]);`n          if (Result = '') and ThingAt(Path) then begin`n            if RenameFile(Path, Path + Suffix) then`n              Count := Count + 1`n            else`n              Result := Format('a file of qKVM Desktop could not be set aside for the copy (%s)', [Names[I]]);",
        # (A refusal puts back what was set aside, and the marker goes; review C7.)
        "  if not SetAside then`n    Exit;`n  if Result = '' then begin`n    Log(Format('Set the earlier version aside for the copy (%d files).', [Count]));",
        "  if PutRight(ExpandConstant('{app}')) = '' then`n    SetAside := False`n  else",
        # (After a whole copy, first: the marker renamed, then what was set aside removed; nothing where it was not renamed.)
        # (And nothing of the earlier version that is not set aside, what this version has not in plugins and qml, removed
        # where the copy could not be marked finished: putting the earlier version back would then lack it.)
        "    Finished := True;`n    NotCounted := NotCopied;`n    if NotCounted <> '' then`n      NotCounted := 'the copy was not whole (' + NotCounted + ')'`n    else if SetAside and (StillRunning > 0) then`n      // (One that still runs after a minute holds what would be flushed and put back: the copy does not count, and says`n      // why, and the earlier version is put back; this setup never ends it.)`n      NotCounted := 'qKVM Desktop, started during the copy, still ran from its folder after a minute'`n    else if SetAside then begin`n      // (A file not known to be on the disk is not whole: nothing set aside goes, review 1.6.)`n      NotCounted := FlushCopy;`n      // (A start made as the copy was flushed, after the look above, that holds a file of it: waited for too, within the`n      // same minute, and the flush tried again, as the VM suite found it, 2026-11-21.)`n      while (NotCounted <> '') and (StillRunning = 0) and (TicksSince(StartsSince) < StartsEndMs) and (StartsRunning > 0) do begin`n        StillRunning := WaitForStartsToEnd(StartsSince);`n        if StillRunning = 0 then`n          NotCounted := FlushCopy;`n      end;`n      if StillRunning > 0 then`n        NotCounted := 'qKVM Desktop, started during the copy, still ran from its folder after a minute'`n      else if NotCounted <> '' then`n        NotCounted := 'the copy could not all be written to the disk (' + NotCounted + ')'`n      else begin`n        Finished := FinishCopy;`n        if not Finished then`n          NotCounted := 'the copy could not be marked finished (' + UnfinishedMarker + ')';`n      end;`n    end;`n    if NotCounted <> '' then begin`n      Finished := False;`n      DoNotCount;`n    end;`n    if Finished then begin`n      // (The kind it installed, beside the version Inno Setup notes, in its entry of Settings > Apps.)`n      if not RegWriteStringValue(HKEY_CURRENT_USER, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{A964EFA6-87C6-4A23-A9CC-B71F87441433}_is1', 'qKVM Desktop Architecture', ThisKind) then`n        Log('The kind of Windows this install is for could not be noted in its entry of Settings > Apps.');`n      RemoveEarlierFiles(EarlierHere, EarlierKindHere);`n    end;`n    RemoveSetupLeftovers;`n    if StartsAgain then`n      StartTheCopy;`n  end;",
        # (Whole: every file of the package's, and the contract's marker, a plain file at its bytes; M1.)
        "    AddNames(PackageFiles, Names);`n    AddNames(PackageFileBytes, Bytes);`n    AddNames(PackageFileSha256, Sums);`n    Names.Add('qkvm-desktop.installed');`n    Bytes.Add('6');`n    Sums.Add(MarkerSha256);`n    if (Names.Count <> Bytes.Count) or (Names.Count <> Sums.Count) then`n      Result := 'the setup''s own list of its files';`n    for I := 0 to Names.Count - 1 do`n      if Result = '' then begin`n        Result := Names[I];`n        if FindFirst(InApp(Names[I]), Found) then begin`n          Sized := ((Found.Attributes and (DirectoryAttribute or ReparsePointAttribute)) = 0) and (Found.SizeHigh = 0) and (IntToStr(Found.SizeLow) = Bytes[I]);`n          FindClose(Found);`n          if Sized and SameBytes(InApp(Names[I]), Sums[I]) then`n            Result := '';",
        # (L7 of review i020-2: by the SHA-256 the manifest gives, read again once where it cannot be read; one that still
        # cannot be is not whole. The marker's own.)
        "  while not Read and (Tries < 2) do begin`n    if Tries > 0 then`n      Sleep(500);`n    Tries := Tries + 1;`n    try`n      Result := Lowercase(GetSHA256OfFile(Path)) = Lowercase(Sum);`n      Read := True;`n    except`n      Log('A file of the copy could not be read, to tell that it is whole: ' + Path + ' (' + GetExceptionMessage + ')');`n    end;`n  end;`nend;",
        "  MarkerSha256 = '$([Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($MarkerBytes)).ToLowerInvariant())';",
        # (On the disk before it is marked finished, each file flushed; L13.)
        "    AddNames(PackageFiles, Names);`n    Names.Add('qkvm-desktop.installed');`n    for I := 0 to Names.Count - 1 do`n      if Result = '' then begin`n        Flushed := FlushedFile(InApp(Names[I]));`n        Tries := 1;`n        while not Flushed and (Tries < FlushTries) do begin`n          Sleep(FlushStepMs);`n          Flushed := FlushedFile(InApp(Names[I]));`n          Tries := Tries + 1;`n        end;`n        if not Flushed then`n          Result := Names[I];`n      end;",
        # (A copy that does not count: the earlier version put back, under its version again, said, and 7.)
        "  if SetAside then begin`n    if PutPackageBack then begin`n      PutRightDone := Done;`n      EarlierBack := True;`n      Why := Why + '; the earlier version is back';`n      if InstalledVersion <> '' then`n        RegWriteStringValue(HKEY_CURRENT_USER, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{A964EFA6-87C6-4A23-A9CC-B71F87441433}_is1', 'DisplayVersion', InstalledVersion);`n      Log(UnchangedLine(Why));",
        "  if not WizardSilent then`n    MsgBox('qKVM Desktop {#AppVersion} was not installed: ' + Why + '. Run the setup again.', mbError, MB_OK);",
        "function GetCustomSetupExitCode: Integer;`nbegin`n  Result := 0;`n  if NotCounted <> '' then`n    Result := 7;`nend;",
        "  Result := (Reason = '') and not EarlierProgramGone;`n  if Result then`n    Log('The copy did not complete: the earlier version is whole again.')",
        # (Finished: what the set-aside renamed goes only where its own name is there, M1.)
        "        if PlainFileAt(Aside) then`n          if (Kinds[I] = LeftKind) or PlainFileAt(AddBackslash(Folder) + Names[I]) then`n            if not ThroughRedirection(Folder, RemoveBackslash(ExtractFilePath(Names[I]))) then`n              if RemoveOwn(Aside) then begin",
        # (What the setup removes of its own: its read-only attribute cleared, then deleted, tried again twice; M2.)
        "  Attributes := GetFileAttributesW(Path);`n  if Attributes <> NoFileAttributes then`n    if (Attributes and ReadOnlyAttribute) <> 0 then`n      SetFileAttributesW(Path, Attributes and not ReadOnlyAttribute);`n  Result := DeleteFile(Path);`n  Tries := 1;`n  while not Result and (Tries < 3) do begin`n    Sleep(250);`n    Result := DeleteFile(Path);`n    Tries := Tries + 1;`n  end;",
        '  ReadOnlyAttribute = $1;',
        # (What earlier versions installed and this one has not (step 4): each by its exact name, a plain file and through
        # no redirection; then their folders that this leaves empty, through no redirection either.)
        '#include "earlier.iss"', 'procedure RemoveEarlierFiles(Version, Kind: String);',
        "  if Version = '' then`n    Exit;`n  Names := TStringList.Create;`n  try`n    AddNames(EarlierFilesOf(Version, Kind), Names);`n    for I := 0 to Names.Count - 1 do begin`n      Path := InApp(Names[I]);`n      if PlainFileAt(Path) and not ThroughRedirection(ExpandConstant('{app}'), RemoveBackslash(ExtractFilePath(Names[I]))) then begin`n        if DeleteFile(Path) then",
        # (Only where the version of ours is proven in that very folder: its Settings > Apps entry names the folder and
        # it, and the contract's marker is there; review i020-1, M4.)
        "  Result := '';`n  if (InstalledVersion = '') or (InstalledFolder = '') then`n    Exit;`n  if not PathSame(InstalledFolder, RemoveBackslashUnlessRoot(ExpandConstant('{app}'))) then`n    Exit;`n  if OurInstallIn(ExpandConstant('{app}')) then`n    Result := InstalledVersion;",
        # (An install of ours: the contract's marker there, a plain file of its six bytes, as the program reads it; review
        # i020-2, M1.)
        "  Result := False;`n  Marker := AddBackslash(Folder) + 'qkvm-desktop.installed';`n  if not FindFirst(Marker, Found) then`n    Exit;`n  try`n    if ((Found.Attributes and (DirectoryAttribute or ReparsePointAttribute)) <> 0) or (Found.SizeHigh <> 0) or (Found.SizeLow <> 6) then`n      Exit;`n  finally`n    FindClose(Found);`n  end;`n  if LoadStringFromFile(Marker, Said) then`n    Result := Said = 'setup' + #10;",
        # (M1: a folder that holds anything but an install of ours is refused, whatever its names, before anything is
        # closed or changed; one not there, or that holds nothing, is a first install; one that cannot be looked through,
        # or not to its end, is not known to hold nothing.)
        "  Result := '';`n  Folder := RemoveBackslashUnlessRoot(ExpandConstant('{app}'));`n  if OurInstallIn(Folder) then`n    Exit;`n  // (A redirection is no folder of its own, whatever it leads to: said as one, review 1.2; RightsReason says so too.)`n  if ThroughRedirection(Folder, '') then begin`n    Result := 'the folder ' + Folder + ' is a redirection (a junction or a link): qKVM Desktop is installed only into a folder that is itself there';`n    Exit;`n  end;`n  // (Nothing there: a first install. A folder there that cannot be looked at is not known to hold nothing.)`n  if GetFileAttributesW(Folder) = NoFileAttributes then begin`n    Error := DLLGetLastError;`n    if (Error = ErrorFileNotFound) or (Error = ErrorPathNotFound) then`n      Exit;`n  end;`n  Result := LeftoversOnlyReason(Folder);",
        # (Reviews 1.1 and 1.8: anything in the folder that is not what a setup of ours leaves there (the markers whose
        # records read, the files those records name with their endings, folders of the setup's own names that hold no
        # more) makes it another's, a redirection among them; a folder looked through not to its end is not known to hold
        # nothing.)
        "  CutShort := '';`n  Other := OtherWithin(Folder, '', LeftoversOf(Folder), CutShort);`n  if CutShort <> '' then`n    Result := CutShort`n  else if Other <> '' then`n    Result := Format('the folder %s holds what is not an install of qKVM Desktop (%s): qKVM Desktop is installed only into a folder of its own, or an empty one', [RemoveBackslashUnlessRoot(Folder), Other])`n  else`n    Result := '';",
        "  if not FindFirst(AddBackslash(Base) + '*', Found) then begin`n    CutShort := 'the folder ' + Base + ' cannot be looked through, to see that it holds nothing but qKVM Desktop';`n    Exit;`n  end;",
        "        if (Attributes and ReparsePointAttribute) <> 0 then`n          Result := Entry`n        else if (Attributes and DirectoryAttribute) <> 0 then begin`n          if OwnFolderName(Entry) then`n            Result := OtherWithin(Folder, Entry, Leftovers, CutShort)`n          else`n            Result := Entry;`n        end else if Pos('|' + Lowercase(Entry) + '|', Leftovers) = 0 then`n          Result := Entry;",
        "        if FindNextFileW(Found.FindHandle, Data) = 0 then begin`n          Error := DLLGetLastError;`n          if Error <> ErrorNoMoreFiles then`n            CutShort := Format('the folder %s could not be looked through to its end, to see that it holds nothing but qKVM Desktop (error %d: %s)', [Base, Error, SysErrorMessage(Error)]);`n          Break;`n        end;`n        Name := Copy(Data, FindNameAt, FindNameChars);`n        if Pos(#0, Name) = 0 then begin`n          CutShort := 'the folder ' + Base + ' could not be looked through to its end, to see that it holds nothing but qKVM Desktop (a name in it could not be read)';`n          Break;`n        end;",
        "      if ReadRecord(AddBackslash(Folder) + Markers[M], Suffix, Names, Endings, Kinds, Copies) then begin`n        Result := Result + Lowercase(Markers[M]) + '|';`n        for I := 0 to Names.Count - 1 do`n          Result := Result + Lowercase(Names[I] + Endings[I]) + '|';`n      end;",
        "  Result := (Relative <> '') and (Pos('|', Relative) = 0) and (Pos('|' + Lowercase(Relative) + '\', Lowercase(PackageFiles + EarlierFiles)) > 0);",
        # (And the set-aside itself only where an install of ours is proven, else that reason.)
        "  if ThingAt(InApp(UnfinishedMarker)) then begin`n    Result := 'the marker of an earlier update could not be removed (' + UnfinishedMarker + ')';`n    Exit;`n  end;`n  // (Nothing set aside where no install of ours is proven in the folder: one that holds anything else is not this`n  // setup's, and Preparing to Install has refused it before, review i020-2 M1. A folder that holds nothing goes on.)`n  if not OurInstallIn(ExpandConstant('{app}')) then begin`n    Result := ForeignFolderReason;`n    Exit;`n  end;`n  // (Which version of ours was installed here",
        "  // (Which version of ours was installed here, as the folder now is: the earlier version whole, put right above.)`n  EarlierHere := InstalledHere;",
        "    Names.Clear;`n    AddNames(EarlierFoldersOf(Version, Kind), Names);`n    for I := 0 to Names.Count - 1 do begin`n      Path := InApp(Names[I]);`n      if ThingAt(Path) and not ThroughRedirection(ExpandConstant('{app}'), Names[I]) then`n        if RemoveDir(Path) then",
        "  Result := RenameFile(InApp(UnfinishedMarker), InApp(FinishingMarker));",
        "  Result := MarkFinishing;`n  Tries := 1;`n  while not Result and (Tries < 3) and ThingAt(InApp(UnfinishedMarker)) do begin`n    Log('The copy is whole; its marker could not be made that of a finishing update yet, and is tried again.');`n    Sleep(500);`n    Result := MarkFinishing;`n    Tries := Tries + 1;`n  end;`n  // (The marker gone all the same",
        "      Log('The copy is whole, and its marker was gone: its record is written again, as that of a finishing update.');`n  end;`n  if not Result then begin`n    Log('The copy is whole, but its marker could not be made that of a finishing update: nothing set aside is removed.');`n    Exit;`n  end;",
        "    Exit;`n  end;`n  Reason := PutRight(ExpandConstant('{app}'));`n  if Reason <> '' then`n    Log('The copy is whole;",
        # (Where the copy does not complete: everything put back, tried again a second apart.)
        "  Reason := PutRight(ExpandConstant('{app}'));`n  Tries := 1;`n  while (Reason <> '') and (Tries < PutBackTries) do begin`n    Sleep(1000);`n    Reason := PutRight(ExpandConstant('{app}'));`n    Tries := Tries + 1;`n  end;",
        # (At an uninstall: what each marker's record names, through no redirection; the program's own as 0.1.2 set it
        # aside, by its exact name; a draft; and the markers.)
        "  Markers[0] := AddBackslash(Folder) + FinishingMarker;`n  Markers[1] := AddBackslash(Folder) + UnfinishedMarker;",
        "    for M := 0 to 1 do`n      if ReadRecord(Markers[M], Suffix, Names, Endings, Kinds, Copies) then begin`n        Left := 0;",
        "        for I := 0 to Names.Count - 1 do begin`n          Aside := AddBackslash(Folder) + Names[I] + Endings[I];`n          // (One that cannot be looked at stays named, as one that cannot be removed, review 1.5.)`n          if not LookAt(Aside, There) then`n            Left := Left + 1`n          else if PlainFileAt(Aside) then`n            if not ThroughRedirection(Folder, RemoveBackslash(ExtractFilePath(Names[I]))) then begin`n              if RemoveOwn(Aside) then",
        # (The uninstall's drop is the same, in the program's folder, an install of ours being there; review 1.8.)
        "procedure DropSetAside;`nbegin`n  DropRecorded(ExpandConstant('{app}'), True);",
        "        if Left = 0 then`n          RemoveOwn(Markers[M])`n        else begin`n          Log(Format('What an update set aside could not all be removed (%d files): they stay, named in its record.', [Left]));`n          if M = 1 then`n            if not ThingAt(Markers[0]) then`n              RenameFile(Markers[1], Markers[0]);",
        "  if PlainFileAt(InApp('qkvm-desktop.exe' + AsideEnding)) then`n    if RemoveOwn(InApp('qkvm-desktop.exe' + AsideEnding)) then",
        "  if PlainFileAt(InApp(RecordDraft)) then`n    RemoveOwn(InApp(RecordDraft));`n  if PlainFileAt(InApp(CheckDraft)) then",
        # (Put right: by the markers' records; neither, the program's own file as 0.1.2 set it aside.)
        "  if not ThingAt(AddBackslash(Folder) + UnfinishedMarker) and not ThingAt(AddBackslash(Folder) + FinishingMarker) then begin`n    PutProgramRight(Folder);`n    Exit;`n  end;`n  if not OurInstallIn(Folder) then begin`n    Result := LeftoversOnlyReason(Folder);`n    if Result = '' then begin`n      Log('The folder holds no install of qKVM Desktop, only what a setup of it left: what the records name is removed, and nothing is put back.');`n      DropRecorded(Folder, False);`n      SayPutRight('removed what an uninstall had left of what an update set aside');`n    end;`n    Exit;`n  end;`n  Result := FinishRecorded(Folder);`n  if Result = '' then begin`n    Unfinished := ThingAt(AddBackslash(Folder) + UnfinishedMarker);`n    Result := PutBackRecorded(Folder);",
        # (A finishing update's record: each file it names removed, a plain file through no redirection; one that cannot
        # be stays named, and the marker with it; the marker last. A record that does not read, a refusal.)
        "    if not ReadRecord(Marker, Suffix, Names, Endings, Kinds, Copies) then begin`n      Result := Format('the record of an update is not one this setup can read (%s)', [FinishingMarker]);`n      Exit;`n    end;`n    for I := 0 to Names.Count - 1 do begin`n      Aside := AddBackslash(Folder) + Names[I] + Endings[I];`n      // (One that cannot be looked at is not taken for gone: it stays named, review 1.5.)`n      if not LookAt(Aside, There) then begin`n        Left := Left + 1;`n        Log('Could not look at what an update set aside: ' + Names[I] + Endings[I]);`n      end else if There then begin`n        Left := Left + 1;",
        # (Review 1.5: a look Windows refuses for another reason than the file's not being there tells nothing.)
        "  There := GetFileAttributesW(Path) <> NoFileAttributes;`n  Result := There;`n  if not There then begin`n    Error := DLLGetLastError;`n    Result := (Error = ErrorFileNotFound) or (Error = ErrorPathNotFound);`n  end;",
        "  if Left > 0 then`n    Log(Format('What an update set aside could not all be removed (%d files): they stay named in its record, and the next setup removes them.', [Left]))`n  else if not RemoveOwn(Marker) then",
        # (An unfinished update's record: each file it set aside put back, by its ending, the copy's file of that name
        # removed first, the program's own last; then what it carried removed; the marker last, or made the finishing one
        # where something carried is still there.)
        "    if not ReadRecord(Marker, Suffix, Names, Endings, Kinds, Copies) then begin`n      Result := Format('the record of an update is not one this setup can read (%s)', [UnfinishedMarker]);`n      Exit;`n    end;",
        # (M3: the program's own name first, where the earlier program is set aside or none was there: what is there
        # removed, and the update check put there; then the libraries from the end; then what the copy made and the
        # earlier version had not, L3; the earlier program last, the update check removed first.)
        "        if Names[I] = 'qkvm-desktop.exe' then`n          ProgramAside := ProgramFile + Endings[I];`n      end;",
        # (Review 1.5: the program's own set-aside file, and what is at its name, seen, or the put-back says so and stops.)
        "    ProgramSetAside := False;`n    ProgramThere := False;`n    if (ProgramAside <> '') and not LookAt(ProgramAside, ProgramSetAside) then`n      Result := 'what an update set aside could not be looked at (qkvm-desktop.exe' + Suffix + ')';`n    if (Result = '') and not LookAt(ProgramFile, ProgramThere) then`n      Result := 'what is at the program''s name could not be looked at (qkvm-desktop.exe)';`n    ProgramCopied := (ProgramAside = '') and (Copies.IndexOf('qkvm-desktop.exe') >= 0);",
        "    ProgramGone := (Result = '') and (ProgramAside <> '') and not ProgramSetAside and (not ProgramThere or IsOwnUpdateCheck(ProgramFile));`n    // (The program's own name first: the update check there while the rest comes back.)`n    if (Result = '') and (ProgramSetAside or ProgramCopied) then begin`n      if ThingAt(ProgramFile) then`n        if not RemoveOwn(ProgramFile) then`n          Result := 'a file of a copy that did not complete could not be removed, to put back the earlier one (qkvm-desktop.exe)';`n      if Result = '' then`n        if not PlaceUpdateCheck(ProgramFile) then",
        "    for I := Names.Count - 1 downto 0 do begin`n      Base := AddBackslash(Folder) + Names[I];`n      Aside := Base + Endings[I];`n      if (Result = '') and (Kinds[I] = AsideKind) and (Names[I] <> 'qkvm-desktop.exe') and not LookAt(Aside, There) then`n        Result := Format('what an update set aside could not be looked at (%s)', [Names[I] + Endings[I]]);`n      if (Result = '') and (Kinds[I] = AsideKind) and (Names[I] <> 'qkvm-desktop.exe') and There then begin`n        if not PlainFileAt(Aside) or (ThingAt(Base) and not PlainFileAt(Base)) or ThroughRedirection(Folder, RemoveBackslash(ExtractFilePath(Names[I]))) then",
        "        else begin`n          if ThingAt(Base) then`n            if not RemoveOwn(Base) then`n              Result := Format(",
        "          if Result = '' then begin`n            if RenameFile(Aside, Base) then`n              Count := Count + 1`n            else`n              Result := Format('a file of the earlier version could not be put back (%s)', [Names[I]]);",
        "    for I := 0 to Copies.Count - 1 do begin`n      Base := AddBackslash(Folder) + Copies[I];`n      if (Result = '') and (Copies[I] <> 'qkvm-desktop.exe') and (Pos('|' + Lowercase(Copies[I]) + '|', SetAsideNames) = 0) then begin`n        if not LookAt(Base, There) then`n          Result := Format('what a copy that did not complete made could not be looked at (%s)', [Copies[I]])`n        else if There then begin`n          if not PlainFileAt(Base) or ThroughRedirection(Folder, RemoveBackslash(ExtractFilePath(Copies[I]))) then`n            Result := Format('what a copy that did not complete made is not a plain file of qKVM Desktop''s (%s)', [Copies[I]])`n          else if RemoveOwn(Base) then`n            Log('Removed what the copy that did not complete had made: ' + Copies[I])`n          else`n            Result := Format('what a copy that did not complete made could not be removed (%s)', [Copies[I]]);",
        "    if (Result = '') and ProgramSetAside then begin`n      if not PlainFileAt(ProgramAside) then`n        Result := 'what an update set aside is not a plain file of qKVM Desktop''s (qkvm-desktop.exe' + Suffix + ')'`n      else if ThingAt(ProgramFile) and not RemoveOwn(ProgramFile) then`n        Result := 'a file of a copy that did not complete could not be removed, to put back the earlier one (qkvm-desktop.exe)'`n      else if RenameFile(ProgramAside, ProgramFile) then`n        Count := Count + 1",
        "    if (Result = '') and ProgramCopied and PlainFileAt(ProgramFile) then`n      if not RemoveOwn(ProgramFile) then",
        "      Exit;`n    end;`n    if ProgramGone then begin",
        "    end else begin`n      Log(Format('Put the earlier version back, which an update that did not finish had set aside (%d files).', [Count]));",
        # (The update check: unpacked once, from this setup; put only where nothing is; and in the set-aside, right after
        # the program's own file, before any library goes, the set-aside refused where it cannot be.)
        "  Result := FileExists(ExpandConstant('{tmp}\') + UpdateCheck);`n  if not Result then`n    try`n      ExtractTemporaryFile(UpdateCheck);",
        # (L4 of review i020-2: under its draft's name beside the program, flushed, then renamed into the program's place,
        # where nothing is; no draft left where it is not.)
        "  Result := False;`n  if not UpdateCheckReady then`n    Exit;`n  Draft := ExtractFilePath(Path) + CheckDraft;`n  if ThingAt(Draft) then`n    if not RemoveOwn(Draft) then`n      Exit;`n  if CopyFile(ExpandConstant('{tmp}\') + UpdateCheck, Draft, True) then`n    if FlushedFile(Draft) then`n      Result := RenameFile(Draft, Path);`n  if not Result then`n    if ThingAt(Draft) then`n      RemoveOwn(Draft);",
        "  CheckDraft = 'qkvm-desktop.update-check';",
        "  if PlainFileAt(InApp(CheckDraft)) then`n    RemoveOwn(InApp(CheckDraft));`nend;",
        # (L3: where the earlier program is gone since it was set aside, the update check at its name, by its bytes, taken
        # away; the put-back never said whole then.)
        "    ProgramGone := (Result = '') and (ProgramAside <> '') and not ProgramSetAside and (not ProgramThere or IsOwnUpdateCheck(ProgramFile));",
        "  Result := False;`n  if PlainFileAt(Path) and UpdateCheckReady then`n    try`n      Result := GetSHA256OfFile(Path) = GetSHA256OfFile(ExpandConstant('{tmp}\') + UpdateCheck);",
        "    if (Result = '') and ProgramGone and IsOwnUpdateCheck(ProgramFile) then`n      if not RemoveOwn(ProgramFile) then`n        Result := 'the setup''s update check in the program''s place could not be taken away, and the earlier program is gone (qkvm-desktop.exe): close its box, then run the setup again';`n    if Result <> '' then begin",
        "    if ProgramGone then begin`n      EarlierProgramGone := True;",
        "  Result := (Reason = '') and not EarlierProgramGone;",
        "    if ProgramGone then begin`n      EarlierProgramGone := True;`n      Log(Format('Put back what an update that did not finish had set aside (%d files), all but the earlier program, which was gone: no program of qKVM Desktop is in its folder until a setup completes.', [Count]));`n      SayPutRight('put back what an update that did not finish had set aside, all but the earlier program, which was gone');",
        "    end else if EarlierProgramGone then`n      Why := Why + '; the earlier version is back, all but its program, which was gone since it was set aside'",
        "          if (Result = '') and (I = 0) then`n            if not PlaceUpdateCheck(ProgramPath) then`n              Result := 'the setup''s update check could not be put in the program''s place (qkvm-desktop.exe)';",
        "        Result := 'every ending for setting aside the files of qKVM Desktop is taken by files of that name left beside them'`n      else if not UpdateCheckReady then`n        Result := 'the setup could not unpack its update check';",
        # (At an uninstall, what a copy that did not finish wrote, which no uninstall log names, by the record's names.)
        # (And only where an install of ours is: what an uninstall left is never a reason to remove a file of the
        # package's own name; review 1.8.)
        "        if Installed and (M = 1) then`n          for I := 0 to Copies.Count - 1 do`n            if PlainFileAt(AddBackslash(Folder) + Copies[I]) then`n              if not ThroughRedirection(Folder, RemoveBackslash(ExtractFilePath(Copies[I]))) then`n                RemoveOwn(AddBackslash(Folder) + Copies[I]);",
        # (Finishing never takes what the copy writes for what it set aside: it reads the record's set-aside alone.)
        "      Aside := AddBackslash(Folder) + Names[I] + Endings[I];`n      // (One that cannot be looked at is not taken for gone: it stays named, review 1.5.)`n      if not LookAt(Aside, There) then begin`n        Left := Left + 1;",
        "      if (Kinds[I] = LeftKind) and not LookAt(Aside, There) then`n        Left := Left + 1`n      else if (Kinds[I] = LeftKind) and There then begin`n        Left := Left + 1;`n        if PlainFileAt(Aside) then`n          if not ThroughRedirection(Folder, RemoveBackslash(ExtractFilePath(Names[I]))) then`n            if RemoveOwn(Aside) then`n              Left := Left - 1;",
        "  if Left = 0 then begin`n    if not RemoveOwn(Marker) then`n      Log('Could not remove the marker of an update that did not finish: ' + Marker);`n  end else if not ThingAt(AddBackslash(Folder) + FinishingMarker) and RenameFile(Marker, AddBackslash(Folder) + FinishingMarker) then",
        "  if not OurInstallIn(Folder) then`n    Exit;`n  Base := AddBackslash(Folder) + 'qkvm-desktop.exe';`n  Aside := Base + AsideEnding;`n  if not PlainFileAt(Aside) then`n    Exit;`n  if ThingAt(Base) then begin`n    if DeleteFile(Aside) then",
        "  end else if RenameFile(Aside, Base) then",
        # (First of all at a setup's start, in the folder it installs into as far as it can tell it then, before its
        # refusal of an older version; where it cannot be put right, the setup does not begin.)
        "  Result := ExpandConstant('{param:DIR|}');`n  if (Copy(Result, 2, 2) <> ':\') and (Copy(Result, 1, 2) <> '\\') then`n    Result := InstalledFolder;`n  if Result = '' then`n    Result := ExpandConstant('{userpf}\qKVM Desktop');")) {
        if (-not $code.Contains($part)) { $said.Add("the staged install lacks: $part") }
    }
    $atStart = [regex]::Match($code, "(?s)\nfunction InitializeSetup: Boolean;\nvar\n.*?\nbegin\n(.*?)\n  if not RegQueryStringValue\(HKEY_CURRENT_USER, '[^']+', 'DisplayVersion', Installed\) then\n    Exit;\n")
    # (Outside review 2, 1.3: and only where the folder's rights are such that no other account may change it, judged
    # first, before anything there is put right or looked for; else nothing there is put right, and Preparing to Install
    # refuses the folder. PutRight called once, there.)
    if (-not $atStart.Success -or -not $atStart.Groups[1].Value.Contains("`n  Reason := FolderRightsReason(FolderToPutRight);`n  if Reason <> '' then begin`n    Log('Nothing that an update left is put right, nor looked for: ' + Reason + '. Preparing to Install refuses such a folder.');`n    Reason := '';`n  end else`n    Reason := PutRight(FolderToPutRight);`n  if Reason <> '' then begin`n    // (What could be put right, is, and is said: never that nothing was changed after it, L9.)`n    if PutRightDone <> '' then`n      Rest := '. First, this setup ' + PutRightDone + '; nothing else was changed.'`n    else`n      Rest := '. Nothing else was changed.';`n    Log('An update of qKVM Desktop that did not finish left what this setup cannot put right: ' + Reason + Rest);`n") -or
        ([regex]::Matches($atStart.Groups[1].Value, '\bPutRight\(')).Count -ne 1 -or ([regex]::Matches($atStart.Groups[1].Value, '\bFolderRightsReason\(')).Count -ne 1 -or
        # (And the result line the program shows, in update mode as in any: never in the log alone, H1.)
        -not $atStart.Groups[1].Value.Contains("    Log(UnchangedLine('an update that did not finish left what this setup cannot put right: ' + Reason));`n    if not WizardSilent then`n      MsgBox(") -or
        -not $atStart.Groups[1].Value.EndsWith("`n    Result := False;`n    Exit;`n  end;") -or $code -notmatch "\n  if ComparePackedVersion\(Have, Offered\) > 0 then begin\n") {
        $said.Add('the setup does not put right what an update that did not finish left first, before its refusal of an older version')
    }
    # (Deleted: a stale file, what a setup ended halfway left, what an update's record names as set aside, a file of a
    # copy that did not complete where the earlier one is put back, the record's draft, and the markers; renamed: a file
    # aside and back, the draft to the marker, and the marker to the finishing one; nothing else.)
    $deletes = (@([regex]::Matches($code, 'DeleteFile\([^)]*\)') | ForEach-Object Value) | Sort-Object -CaseSensitive) -join '|'
    $renames = (@([regex]::Matches($code, 'RenameFile\([^)]*\)') | ForEach-Object Value) | Sort-Object -CaseSensitive) -join '|'
    if ($deletes -cne "DeleteFile(Aside)|DeleteFile(Entry)|DeleteFile(Path)|DeleteFile(Path)|DeleteFile(Path)|DeleteFile(ProfileFileMade)" -or
        ([regex]::Matches($code, '\bRemoveOwn\(')).Count -ne 23 -or
        $renames -cne 'RenameFile(Aside, Base)|RenameFile(Aside, Base)|RenameFile(Draft, InApp(Marker)|RenameFile(Draft, Path)|RenameFile(InApp(UnfinishedMarker)|RenameFile(Marker, AddBackslash(Folder)|RenameFile(Markers[1], Markers[0])|RenameFile(Path, Path + Suffix)|RenameFile(ProgramAside, ProgramFile)' -or
        ([regex]::Matches($code, 'RemoveDir\(')).Count -ne 3) {
        $said.Add("the [Code] section deletes or renames otherwise than the removal after the copy, what a setup ended halfway left and the earlier version set aside ($deletes; $renames)")
    }
    # The one result line the program reads in the log when the setup ends with 105 or 7 (it cannot see the exit code):
    # the 105 line right before ExitProcess, so it is in the log before the process ends.
    $codeLines = @($sections['Code'] | ForEach-Object { $_.TrimEnd() } | Where-Object { $_.Trim() -and -not $_.Trim().StartsWith('//') })
    $at = [Array]::IndexOf($codeLines, "    Log('qKVM-RESULT: 105 the program was still running after 60 seconds; nothing was changed');")
    if ($at -lt 0 -or $codeLines[$at + 1] -cne '    ExitProcess(StillRunningExitCode);') {
        $said.Add('the result line of exit code 105 is not logged right before ExitProcess')
    }
    if ($codeLines -cnotcontains "    Log(UnchangedLine(Reason));" -or $codeLines -cnotcontains "    Result := 'qKVM-RESULT: 7 nothing was changed: ' + Reason") { $said.Add('the result line of exit code 7 is not logged') }
    # (The 105 line, and the two of 7 that UnchangedLine gives, review i020-2 L9: the contract's.)
    if (([regex]::Matches($code, 'qKVM-RESULT: ')).Count -ne 3 -or ([regex]::Matches($code, '\bUnchangedLine\(')).Count -ne 5) { $said.Add('the [Code] section logs a result line otherwise than those of the contract') }
    # The calls outside Pascal Script, each as declared here and no other: ExitProcess (the contract's 105), and, for
    # Setup alone, what closing a running copy needs (its event set, its process found by its icon's window and its
    # file, its answer read, waited for, and ended by force where that is chosen).
    $outside = @($sections['Code'] | Where-Object { $_ -match '\bexternal\b' })
    $declared = [ordered]@{
        'procedure ExitProcess(ExitCode: Cardinal);' = "  external 'ExitProcess@kernel32.dll stdcall';"
        # (The number of its own process, for its event that says it is at work, review R2.)
        'function GetCurrentProcessId: Cardinal;' = "  external 'GetCurrentProcessId@kernel32.dll stdcall';"
        # (Not for Setup alone: the uninstall's own event that says it is at work, and its gate, review R4.)
        'function CreateEventW(Attributes, ManualReset, InitialState: Cardinal; Name: String): THandle;' = "  external 'CreateEventW@kernel32.dll stdcall';"
        'function CreateMutexW(Attributes, InitialOwner: Cardinal; Name: String): THandle;' = "  external 'CreateMutexW@kernel32.dll stdcall';"
        'function OpenEventW(Access, Inherit: Cardinal; Name: String): THandle;' = "  external 'OpenEventW@kernel32.dll stdcall setuponly';"
        'function SetEvent(Event: THandle): Cardinal;' = "  external 'SetEvent@kernel32.dll stdcall setuponly';"
        'function CloseHandle(Handle: THandle): Cardinal;' = "  external 'CloseHandle@kernel32.dll stdcall';"
        'function OpenProcess(Access, Inherit, ProcessId: Cardinal): THandle;' = "  external 'OpenProcess@kernel32.dll stdcall setuponly';"
        # (Every process's number, for the starts of the program that run from its folder while the copy is judged.)
        'function K32EnumProcesses(Ids: String; Bytes: Cardinal; var Needed: Cardinal): Cardinal;' = "  external 'K32EnumProcesses@kernel32.dll stdcall setuponly';"
        'function QueryFullProcessImageNameW(Process: THandle; Flags: Cardinal; Name: String; var Size: Cardinal): Cardinal;' = "  external 'QueryFullProcessImageNameW@kernel32.dll stdcall setuponly';"
        'function WaitForSingleObject(Handle: THandle; Milliseconds: Cardinal): Cardinal;' = "  external 'WaitForSingleObject@kernel32.dll stdcall';"
        'function TerminateProcess(Process: THandle; ExitCode: Cardinal): Cardinal;' = "  external 'TerminateProcess@kernel32.dll stdcall setuponly';"
        'function FindWindowExW(Parent, After: HWND; ClassName: String; WindowName: Cardinal): HWND;' = "  external 'FindWindowExW@user32.dll stdcall setuponly';"
        'function GetWindowThreadProcessId(Window: HWND; var ProcessId: Cardinal): Cardinal;' = "  external 'GetWindowThreadProcessId@user32.dll stdcall setuponly';"
        'function GetWindowTextW(Window: HWND; Text: String; Most: Integer): Integer;' = "  external 'GetWindowTextW@user32.dll stdcall setuponly';"
        # (Preparing to Install's 2 s, review C10.)
        'function GetTickCount: Cardinal;' = "  external 'GetTickCount@kernel32.dll stdcall setuponly';"
        # (Whether a file found in use is there still, review R5.)
        'function GetFileAttributesW(Name: String): Integer;' = "  external 'GetFileAttributesW@kernel32.dll stdcall';"
        # (And a file of the setup's own made writable before it is removed, by Setup and the uninstaller; M2.)
        'function SetFileAttributesW(Name: String; Attributes: Cardinal): Cardinal;' = "  external 'SetFileAttributesW@kernel32.dll stdcall';"
        # (Why a folder looked through has no next entry, review F3; and whether Setup's own mutex is there, review F5.)
        'function OpenMutexW(Access, Inherit: Cardinal; Name: String): THandle;' = "  external 'OpenMutexW@kernel32.dll stdcall setuponly';"
        'function FindNextFileW(Find: THandle; Data: String): Cardinal;' = "  external 'FindNextFileW@kernel32.dll stdcall setuponly';"
        # (A file opened: by the uninstaller, the lock of the profile a copy of 0.2.0 or later holds while it runs; by
        # Setup, the record of what it sets aside, to be flushed; i020. And Setup's flush of it.)
        'function CreateFileW(Name: String; Access, Share, Security, Disposition, Flags, Template: Cardinal): THandle;' = "  external 'CreateFileW@kernel32.dll stdcall';"
        'function FlushFileBuffers(Handle: THandle): Cardinal;' = "  external 'FlushFileBuffers@kernel32.dll stdcall setuponly';"
        'function LockFile(Handle: THandle; OffsetLow, OffsetHigh, LengthLow, LengthHigh: Cardinal): Cardinal;' = "  external 'LockFile@kernel32.dll stdcall uninstallonly';"
        # (Who owns the program's folder and the folders above it, and who may do what with them, in Windows' words, and
        # who this setup runs as; each text copied out of Windows' memory, which is given back; review 1.2.)
        'function GetNamedSecurityInfoW(Name: String; ObjectType, Info: Cardinal; var Owner, Group, Dacl, Sacl, Descriptor: THandle): Cardinal;' = "  external 'GetNamedSecurityInfoW@advapi32.dll stdcall setuponly';"
        'function ConvertSecurityDescriptorToStringSecurityDescriptorW(Descriptor: THandle; Revision, Info: Cardinal; var Text: THandle; var Size: Cardinal): Cardinal;' = "  external 'ConvertSecurityDescriptorToStringSecurityDescriptorW@advapi32.dll stdcall setuponly';"
        'function GetCurrentProcess: THandle;' = "  external 'GetCurrentProcess@kernel32.dll stdcall';"
        'function OpenProcessToken(Process: THandle; Access: Cardinal; var Token: THandle): Cardinal;' = "  external 'OpenProcessToken@advapi32.dll stdcall';"
        'function GetTokenInformation(Token: THandle; Kind: Cardinal; Info: String; Size: Cardinal; var Needed: Cardinal): Cardinal;' = "  external 'GetTokenInformation@advapi32.dll stdcall';"
        'function ConvertSidToStringSidW(Sid: String; var Text: THandle): Cardinal;' = "  external 'ConvertSidToStringSidW@advapi32.dll stdcall';"
        'function lstrlenW(Text: THandle): Integer;' = "  external 'lstrlenW@kernel32.dll stdcall';"
        'function lstrcpynW(Into: String; Text: THandle; Most: Integer): THandle;' = "  external 'lstrcpynW@kernel32.dll stdcall';"
        'function LocalFree(Memory: THandle): THandle;' = "  external 'LocalFree@kernel32.dll stdcall';"
        # (Where the String that holds the token's user begins, which Windows' own pointer in it is reckoned from: in the
        # uninstall too; outside review 2, 1.1.)
        'function CharPrevW(Start, Current: String): THandle;' = "  external 'CharPrevW@user32.dll stdcall';"
        'function GetDriveTypeW(Root: String): Cardinal;' = "  external 'GetDriveTypeW@kernel32.dll stdcall setuponly';"
        # (Not for Setup alone: the account's gate, by its SID, and the wait for it, in the uninstall too; and whether this
        # setup holds the marker it wrote, review 1.3 and 1.4.)
        'function GetFileType(Handle: THandle): Cardinal;' = "  external 'GetFileType@kernel32.dll stdcall setuponly';"
    }
    $calledRight = $outside.Count -eq $declared.Count
    foreach ($prototype in $declared.Keys) {
        if (-not $code.Contains("$prototype`n$($declared[$prototype])")) { $calledRight = $false }
    }
    if (-not $calledRight) { $said.Add("the [Code] section calls outside Pascal Script otherwise than it says: $($outside -join ' | ')") }
    foreach ($section in 'InstallDelete', 'UninstallRun', 'Registry', 'INI', 'UninstallDelete', 'Dirs', 'Components') {
        if ($sections.ContainsKey($section)) { $said.Add("the script has a [$section] section") }
    }
    # (Two starts of a program, and only of the program, each with --after-update: what this setup closed and then changed
    # nothing for, started again as the setup ends, review L1; and an update's own start, and a silent install's that
    # closed it, at the very end of the post-install step, once the copy is flushed and marked finished, or the earlier
    # version is back where it does not count (StartTheCopy; never from [Run], above).)
    $startAgain = "  if ExecAsOriginalUser(ProgramPath, '--after-update', '', SW_SHOWNORMAL, ewNoWait, Started) then"
    if (([regex]::Matches($code, [regex]::Escape($startAgain))).Count -ne 2) { $said.Add('the [Code] section does not start the program again as it should, twice') }
    $postInstall = [regex]::Match($code, "(?s)\nprocedure CurStepChanged\(CurStep: TSetupStep\);.*?\n  if CurStep = ssPostInstall then begin\n.*?\nend;\n").Value
    $judged = $postInstall.IndexOf("        Finished := FinishCopy;`n")
    $started = $postInstall.IndexOf("      RemoveEarlierFiles(EarlierHere, EarlierKindHere);`n    end;`n    RemoveSetupLeftovers;`n    if StartsAgain then`n      StartTheCopy;`n  end;`nend;`n")
    if ($judged -lt 0 -or $started -lt $judged -or ([regex]::Matches($code, '\n\s*StartTheCopy;')).Count -ne 1 -or
        -not $code.Contains("procedure StartTheCopy;`nvar`n  Started: Integer;`nbegin`n$startAgain`n") -or
        $code.IndexOf("`nfunction StartsAgain: Boolean;") -gt $code.IndexOf("`nprocedure CurStepChanged(CurStep: TSetupStep);")) {
        $said.Add('the [Code] section does not start the program after its copy only once the copy is flushed and marked finished')
    }
    # (Before the copy is judged (whole, on the disk and marked finished, or put back where it does not count), the starts
    # of the program made while it copied waited for, a quarter of a second at a time, a minute at the most: such a start
    # ends by itself as it finds the setup at work, but until then it holds the program's file and its libraries against
    # the flush and the put-back, and Windows' scanner can hold the first start of a copy just written for seconds, so
    # that the update was lost (step 15 of test_setup_vm). Every process whose file is in the program's folder, or in
    # a folder within it, counts while it runs, by Windows' list of processes and the file each runs.)
    $startsRunning = @('function StartsRunning: Integer;', 'var', '  Ids, Name, App: String;', '  Needed, Size, Id: Cardinal;', '  I: Integer;', '  Process: THandle;',
        'begin', '  Result := 0;', '  Ids := StringOfChar(#0, 2 * ProcessListMost);', '  Needed := 0;', '  if K32EnumProcesses(Ids, 4 * ProcessListMost, Needed) = 0 then begin',
        "    Log('The processes that run could not be listed: ' + SysErrorMessage(DLLGetLastError));", '    Exit;', '  end;',
        "  App := RemoveBackslashUnlessRoot(ExpandConstant('{app}'));", '  for I := 0 to Integer(Needed div 4) - 1 do begin',
        '    Id := Cardinal(Ord(Ids[2 * I + 1])) + Cardinal(Ord(Ids[2 * I + 2])) * 65536;', '    Process := 0;', '    if Id <> 0 then',
        '      Process := OpenProcess(ProcessQueryLimitedInformation or ProcessSynchronize, 0, Id);', '    if Process <> 0 then begin', '      Size := 1024;',
        "      Name := StringOfChar(' ', Size);", '      if (QueryFullProcessImageNameW(Process, 0, Name, Size) <> 0) and (WaitForSingleObject(Process, 0) <> WaitObject0) then begin',
        '        Name := Copy(Name, 1, Size);', "        if (Length(Name) > Length(App) + 1) and (Copy(Name, Length(App) + 1, 1) = '\') and PathSame(Copy(Name, 1, Length(App)), App) then",
        '          Result := Result + 1;', '      end;', '      CloseHandle(Process);', '    end;', '  end;', 'end;') -join "`n"
    $waitForStarts = @('function WaitForStartsToEnd(Since: Cardinal): Integer;', 'var', '  Began: Cardinal;', '  Most: Integer;', '  Shown: Boolean;', 'begin', '  Result := StartsRunning;', '  if Result = 0 then',
        '    Exit;', '  Began := GetTickCount;', '  Most := Result;', '  Shown := not WizardSilent;', '  if Shown then begin',
        "    StartsPage.SetText('Waiting for qKVM Desktop, started during the install, to end...', '');", '    StartsPage.SetProgress(Integer(TicksSince(Since)), StartsEndMs);',
        '    StartsPage.Show;', '  end;', '  try', '    while (Result > 0) and (TicksSince(Since) < StartsEndMs) do begin', '      Sleep(UpdateStepMs);',
        '      Result := StartsRunning;', '      if Result > Most then', '        Most := Result;', '      if Shown then', '        StartsPage.SetProgress(Integer(TicksSince(Since)), StartsEndMs);',
        '    end;', '  finally', '    if Shown then', '      StartsPage.Hide;', '  end;', '  if Result = 0 then',
        "    Log(Format('Waited %d ms for the starts of qKVM Desktop made during the copy to end (%d at once at the most): each ends by itself while this setup works.', [Integer(TicksSince(Began)), Most]))",
        '  else', "    Log(Format('%d starts of qKVM Desktop made during the copy still run from its folder after %d ms: this setup ends none of them.', [Result, Integer(TicksSince(Since))]));",
        'end;') -join "`n"
    $waitsFirst = $postInstall.IndexOf("    PostInstalled := True;`n    // (The starts made during the copy ended first: they hold what is judged, WaitForStartsToEnd.)`n    StartsSince := GetTickCount;`n    StillRunning := WaitForStartsToEnd(StartsSince);`n    // (The copy whole first,")
    if (-not $code.Contains($startsRunning) -or -not $code.Contains($waitForStarts) -or -not $code.Contains("  StartsEndMs = 60000;`n  ProcessListMost = 16384;`n") -or
        ([regex]::Matches($code, '\bWaitForStartsToEnd\(StartsSince\);')).Count -ne 2 -or $waitsFirst -lt 0 -or $waitsFirst -gt $postInstall.IndexOf("    NotCounted := NotCopied;`n") -or
        $code.IndexOf("`nprocedure WaitForStartsToEnd;") -gt $code.IndexOf("`nprocedure CurStepChanged(CurStep: TSetupStep);") -or
        ([regex]::Matches($code, 'Result := StartsRunning;')).Count -ne 2) {
        $said.Add('the [Code] section does not wait for the starts of the program made during the copy before it judges the copy')
    }
    # (In an interactive install the person sees that wait, on a page of its own whose progress is stepped at each look; a
    # silent install, an update among them, shows nothing more; review armp-1 L9. The page takes no message while the
    # script sleeps, so Windows may show the setup as not responding until the wait ends: a known limit, CHANGELOG.)
    if (-not $code.Contains("  StartsPage := CreateOutputProgressPage('Waiting for qKVM Desktop', 'It was started while Setup installed it, and ends by itself; Setup waits a minute at the most.');`n") -or
        -not $code.Contains("  ClosingPage: TOutputProgressWizardPage;`n  // The page of an interactive install's wait for the starts of the program made during its copy (WaitForStartsToEnd).`n  StartsPage: TOutputProgressWizardPage;`n") -or
        ([regex]::Matches($code, '\bStartsPage\.(Show|Hide|SetProgress|SetText)\(?')).Count -ne 5 -or -not $code.Contains("  Shown := not WizardSilent;`n")) {
        $said.Add('an interactive install does not show its wait for the starts of the program made during the copy')
    }
    # (And where one still runs after the minute and the earlier version was set aside, the copy does not count, as any
    # that does not (exit 7, the earlier version put back), and says why; the setup never ends such a start: its only
    # forced end is End it now's and /FORCECLOSE=1's, counted above: the coordinator's rule for step 15 of test_setup_vm.)
    $stillRuns = ("    else if SetAside and (StillRunning > 0) then`n" +
        "      // (One that still runs after a minute holds what would be flushed and put back: the copy does not count, and says`n" +
        "      // why, and the earlier version is put back; this setup never ends it.)`n" +
        "      NotCounted := 'qKVM Desktop, started during the copy, still ran from its folder after a minute'`n    else if SetAside then begin`n")
    if (-not $postInstall.Contains("      NotCounted := 'the copy was not whole (' + NotCounted + ')'`n$stillRuns") -or
        -not $code.Contains("procedure CurStepChanged(CurStep: TSetupStep);`nvar`n  Finished: Boolean;`n  StillRunning: Integer;`n  StartsSince: Cardinal;`nbegin`n")) {
        $said.Add('the copy counts where a start of the program made during it still runs after the minute')
    }
    # (A start made as the copy is flushed, after that look, which holds a file of it: waited for too, within the same
    # minute, and the flush tried again; past the minute, as above. The VM suite found such a start, 2026-11-21.)
    if (-not $postInstall.Contains("      NotCounted := FlushCopy;`n      // (A start made as the copy was flushed, after the look above, that holds a file of it: waited for too, within the`n      // same minute, and the flush tried again, as the VM suite found it, 2026-11-21.)`n      while (NotCounted <> '') and (StillRunning = 0) and (TicksSince(StartsSince) < StartsEndMs) and (StartsRunning > 0) do begin`n        StillRunning := WaitForStartsToEnd(StartsSince);`n        if StillRunning = 0 then`n          NotCounted := FlushCopy;`n      end;`n      if StillRunning > 0 then`n        NotCounted := 'qKVM Desktop, started during the copy, still ran from its folder after a minute'`n      else if NotCounted <> '' then`n") -or ([regex]::Matches($code, 'NotCounted := FlushCopy;')).Count -ne 2) {
        $said.Add('the copy is flushed but once beside a start of the program made as it is flushed')
    }
    # (Where an update that did not finish is put back whole, what its cut copy left under Inno Setup's temporary names
    # goes too, in the package's own folders under that folder: the folder is the earlier version's and nothing more, as
    # after a whole copy (step 41 of test_setup_vm). A put-back that stopped keeps its marker, and leaves them.)
    $putBackLeftovers = ("  Result := FinishRecorded(Folder);`n  if Result = '' then begin`n    Unfinished := ThingAt(AddBackslash(Folder) + UnfinishedMarker);`n" +
        "    Result := PutBackRecorded(Folder);`n" +
        "    // (And what the copy that was cut left under Inno Setup's temporary names, a file it was writing: the folder is then`n" +
        "    // the earlier version's and nothing more, as after a whole copy (step 41 of test_setup_vm). Only where the`n" +
        "    // put-back is whole: one that stopped keeps its marker, and the next setup does this again.)`n" +
        "    if Unfinished and (Result = '') then`n      RemoveSetupLeftoversOf(Folder);`n  end;`nend;")
    if (-not $code.Contains($putBackLeftovers) -or ([regex]::Matches($code, '\bRemoveSetupLeftoversOf\(')).Count -ne 3 -or
        -not $code.Contains("function PutRight(Folder: String): String;`nvar`n  Unfinished: Boolean;`nbegin`n")) {
        $said.Add('a put-back of an update that did not finish leaves what its cut copy left')
    }
    # (And one file written, of the setup's own making: the record of what an update sets aside, under its draft's name
    # in the program's folder and renamed to the marker, as the set-aside begins; nothing else, 0.2.0.)
    $markerWrite = "  if SaveStringToFile(Draft, Text, False) then"
    if (([regex]::Matches($code, [regex]::Escape($markerWrite))).Count -ne 1 -or ([regex]::Matches($code, 'WriteRecord\(')).Count -ne 3 -or
        -not $code.Contains("  Draft := InApp(RecordDraft);")) { $said.Add('the [Code] section does not write the marker of an unfinished update as it should, once') }
    # (And one value written, by the copy that does not count: Settings > Apps' DisplayVersion back to the version it
    # named before, as the earlier version is back; M1.)
    $versionBack = "        RegWriteStringValue(HKEY_CURRENT_USER, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{A964EFA6-87C6-4A23-A9CC-B71F87441433}_is1', 'DisplayVersion', InstalledVersion);"
    if (([regex]::Matches($code, [regex]::Escape($versionBack))).Count -ne 1) { $said.Add('the [Code] section does not put the version back as it should, once') }
    # (And one file copied, and one unpacked: qKVM Setup's update check, from its temporary folder into the program's
    # place, M3.)
    $checkCopy = "  if CopyFile(ExpandConstant('{tmp}\') + UpdateCheck, Draft, True) then"
    $checkUnpack = "      ExtractTemporaryFile(UpdateCheck);"
    if (([regex]::Matches($code, [regex]::Escape($checkCopy))).Count -ne 1 -or ([regex]::Matches($code, [regex]::Escape($checkUnpack))).Count -ne 1 -or
        $code -cmatch "\n  UpdateCheck = ") {
        $said.Add('the [Code] section does not unpack and put the update check in the program''s place as it should, once')
    }
    # (And one value more, after a whole copy: the kind of Windows it installed, in its entry of Settings > Apps, beside
    # the version Inno Setup notes. The next setup never reads it back: Inno Setup makes that entry anew at every install,
    # and an update that did not finish leaves it without the kind. An earlier install's kind is its program's own, by its
    # PE header (ImageKind); a program of neither kind removes nothing of what that version alone installed, never taken
    # for x64's (rehearsal on Windows on Arm, 2026-11-19).)
    $kindWrite = "      if not RegWriteStringValue(HKEY_CURRENT_USER, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{A964EFA6-87C6-4A23-A9CC-B71F87441433}_is1', 'qKVM Desktop Architecture', ThisKind) then"
    $imageKind = @(
        'function ImageKind(Path: String): String;', 'var', '  Stream: TFileStream;', '  Header: AnsiString;', '  At, Machine, Tries: Integer;', '  Opened: Boolean;', 'begin',
        "  Result := '';", '  Opened := False;', '  Tries := 0;', '  while not Opened and (Tries < 2) do begin', '    if Tries > 0 then', '      Sleep(500);', '    Tries := Tries + 1;',
        '    try', '      Stream := TFileStream.Create(Path, fmOpenRead or fmShareDenyNone);', '      Opened := True;', '      try', '        SetLength(Header, 64);',
        '        Stream.ReadBuffer(Header, 64);', "        if (Header[1] <> 'M') or (Header[2] <> 'Z') or (Ord(Header[63]) <> 0) or (Ord(Header[64]) <> 0) then",
        '          Exit;', '        At := Ord(Header[61]) + Ord(Header[62]) * 256;', '        if At < 64 then', '          Exit;', '        Stream.Seek(At, soFromBeginning);',
        '        SetLength(Header, 6);', '        Stream.ReadBuffer(Header, 6);', "        if (Header[1] <> 'P') or (Header[2] <> 'E') or (Ord(Header[3]) <> 0) or (Ord(Header[4]) <> 0) then",
        '          Exit;', '        Machine := Ord(Header[5]) + Ord(Header[6]) * 256;', '        if Machine = $AA64 then', "          Result := 'arm64'", '        else if Machine = $8664 then',
        "          Result := 'x64';", '      finally', '        Stream.Free;', '      end;', '    except', '      if not Opened then',
        "        Log('The program in this folder could not be opened, to tell its kind of Windows: ' + Path + ' (' + GetExceptionMessage + ')');", "      Result := '';", '    end;', '  end;', 'end;') -join "`n"
    if (([regex]::Matches($code, [regex]::Escape($kindWrite))).Count -ne 1 -or -not $code.Contains("    if Finished then begin`n") -or
        -not $code.Contains("$kindWrite`n        Log('The kind of Windows this install is for could not be noted in its entry of Settings > Apps.');`n      RemoveEarlierFiles(EarlierHere, EarlierKindHere);`n    end;") -or
        -not $code.Contains($imageKind) -or $code.Contains('InstalledKind') -or $code -match "RegQueryStringValue\([^;]*'qKVM Desktop Architecture'" -or
        -not $code.Contains("  EarlierKindHere := '';`n  if EarlierHere <> '' then begin`n    EarlierKindHere := ImageKind(ProgramPath);`n    if EarlierKindHere = '' then`n")) {
        $said.Add('the [Code] section does not note the kind of Windows it installed, and take an earlier install''s from its program, as it should')
    }
    # (Every list, and the update check, of the kind of Windows it installs: Inno Setup's IsArm64.)
    foreach ($chooser in @(@('function PackageFolders: String;', 'PackageFoldersArm64', 'PackageFoldersX64'), @('function PackageFiles: String;', 'PackageFilesArm64', 'PackageFilesX64'),
            @('function PackageFileBytes: String;', 'PackageFileBytesArm64', 'PackageFileBytesX64'), @('function PackageFileSha256: String;', 'PackageFileSha256Arm64', 'PackageFileSha256X64'),
            @('function EarlierFiles: String;', 'EarlierFilesArm64', 'EarlierFilesX64'), @('function EarlierFilesOf(Version, Kind: String): String;', 'EarlierFilesOfArm64(Version, Kind)', 'EarlierFilesOfX64(Version, Kind)'),
            @('function EarlierFoldersOf(Version, Kind: String): String;', 'EarlierFoldersOfArm64(Version, Kind)', 'EarlierFoldersOfX64(Version, Kind)'),
            @('function UpdateCheck: String;', "'qkvm-update-check-arm64.exe'", "'qkvm-update-check.exe'"), @('function ThisKind: String;', "'arm64'", "'x64'"))) {
        if (([regex]::Matches($code, [regex]::Escape("$($chooser[0])`nbegin`n  if IsArm64 then`n    Result := $($chooser[1])`n  else`n    Result := $($chooser[2]);`nend;"))).Count -ne 1) {
            $said.Add("the [Code] section does not choose $($chooser[0]) by the PC's kind of Windows")
        }
    }
    if (([regex]::Matches($codeOnly, '\bIsArm64\b')).Count -ne 9) { $said.Add('the [Code] section asks for the PC''s kind of Windows elsewhere') }
    $withoutTheStart = $code.Replace($startAgain, '').Replace($markerWrite, '').Replace($versionBack, '').Replace($checkCopy, '').Replace($checkUnpack, '').Replace($kindWrite, '')
    foreach ($word in 'Exec', 'Download', 'CreateOleObject', 'CreateComObject', 'ExtractArchive', 'http', 'RegWrite', 'SaveStringToFile', 'DelTree', 'CopyFile', 'ExtractTemporary', 'FileCopy') {
        if ($withoutTheStart -match "(?i)$word") { $said.Add("the [Code] section uses $word") }
    }
    # --- The twin of the byte scan, where it was made.
    $twin = Join-Path $Setup "scan\$name"
    if (Test-Path -LiteralPath $twin) {
        $twinRows = @(Get-Content -LiteralPath (Join-Path $Setup 'scan\setup-files.txt') | ConvertFrom-Csv -Delimiter "`t" | ForEach-Object { "$($_.SourceFilename)|$($_.SHA256Sum)" } | Sort-Object)
        $rows = @($listed | ForEach-Object { "$($_.SourceFilename)|$($_.SHA256Sum)" } | Sort-Object)
        if (($twinRows -join "`n") -cne ($rows -join "`n")) { $said.Add('the twin for the byte scan does not hold the same files') }
        $bytes = [IO.File]::ReadAllBytes($twin)
        $wide = [Text.Encoding]::Unicode
        if (-not ($wide.GetString($bytes).Contains($ProgramMutex) -or $wide.GetString($bytes, 1, $bytes.Length - 1).Contains($ProgramMutex))) {
            $said.Add('the twin for the byte scan is not readable (the mutex''s name is not seen in it)')
        }
        foreach ($folder in @($inputs) + @($carried.Values | ForEach-Object { $_.Folder })) {
            foreach ($file in $exe, $twin) {
                if (Test-NamesFolder $file $folder) { $said.Add("$(Split-Path -Leaf (Split-Path -Parent $file))\$(Split-Path -Leaf $file) names the folder $folder") }
            }
        }
    }
    return $said
}

# --- Its own cases first.
if (-not (Test-Path -LiteralPath (Join-Path $Inno 'ISCC.exe'))) { throw "There is no Inno Setup in ${Inno}: run tools\fetch_inno.ps1 first." }
$work = Join-Path $desktop "build/setup-tests/$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $work | Out-Null
function New-FixturePackage([string] $Name, [hashtable] $Files, [string] $Kind = 'x64') {
    $package = Join-Path $work "$Name/qKVM-Desktop-0.1.0-windows-$Kind"
    foreach ($file in $Files.Keys) {
        $path = Join-Path $package $file
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
        [IO.File]::WriteAllText($path, $Files[$file])
    }
    $entries = @(Get-ChildItem -LiteralPath $package -Recurse -File | Sort-Object FullName | ForEach-Object {
        [ordered]@{ path = [IO.Path]::GetRelativePath($package, $_.FullName).Replace('\', '/'); bytes = $_.Length; sha256 = Get-Sha256 $_.FullName } })
    [ordered]@{ product = 'qKVM'; version = '0.1.0'; architecture = $Kind; files = $entries } | ConvertTo-Json -Depth 4 |
        Set-Content -LiteralPath (Join-Path $package 'package-manifest.json') -Encoding utf8
    return $package
}
$fixture = @{ 'qkvm-desktop.exe' = 'a made-up program, never run'; 'Qt6Core.dll' = 'made-up'; 'plugins/platforms/qwindows.dll' = 'made-up'
    'qml/QtQuick/Shapes/qmldir' = 'module QtQuick.Shapes'; 'licenses/AGPL-3.0.txt' = 'licence'; 'START-HERE.txt' = 'start here' }
# (Windows on Arm's made-up package: its own program and libraries, one file x64's has not, and none of one file x64's has.)
$fixtureArm = @{ 'qkvm-desktop.exe' = 'a made-up ARM64 program, never run'; 'Qt6Core.dll' = 'made-up for Arm'; 'plugins/platforms/qwindows.dll' = 'made-up for Arm'
    'qml/QtQuick/Shapes/qmldir' = 'module QtQuick.Shapes'; 'licenses/AGPL-3.0.txt' = 'licence'; 'START-HERE.txt' = 'start here on Arm'
    'licenses/arm-only.txt' = 'a notice of the Arm build alone' }
$fixture['x64-only.dll'] = 'made-up, of x64''s build alone'
# (A list of what each published version installed, of the made-up packages' own: versions before the package's, whose
# files the package has not are removed after a whole copy, one name in two cases, folders within folders; and the
# package's own version and a later one, whose files are not.)
# (Schema 2: each version's builds, of x64 or arm64, each with what it installed.)
function New-PublishedList([string] $Name, $Versions) {
    $file = Join-Path $work "$Name.json"
    [ordered]@{ schema = 2; versions = @($Versions) } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $file -Encoding utf8
    return $file
}
function Of([string] $Version, [string[]] $Files, [string[]] $ArmFiles = @()) {
    <# A version of the list: its x64 build's files, and its Windows on Arm build's where it had one. #>
    $builds = @([ordered]@{ architecture = 'x64'; files = @($Files) })
    if ($ArmFiles.Count) { $builds += [ordered]@{ architecture = 'arm64'; files = @($ArmFiles) } }
    return [ordered]@{ version = $Version; builds = $builds }
}
$fixturePublished = New-PublishedList 'published' @(
    (Of '0.0.8' @('qkvm-desktop.exe', 'Qt6Core.dll', 'old.dll', 'licenses/old.txt', 'plugins/old/old.dll', 'qml/Old/Deep/qmldir', 'package-manifest.json'))
    (Of '0.0.9' @('qkvm-desktop.exe', 'Old.dll', 'licenses/AGPL-3.0.txt') @('qkvm-desktop.exe', 'arm-old.dll'))
    (Of '0.1.0' @('qkvm-desktop.exe', 'same-version.dll'))
    (Of '0.2.0' @('qkvm-desktop.exe', 'later.dll')))
# (A made-up update check of each kind, never run: a 64-bit Windows image of that kind built as our own programs must be,
# which tools/build_setup.ps1 holds it to (tools/image-protections.json: its relocations kept, ASLR of high entropy, DEP,
# CFG with its load configuration's word of it, the debug directory's CET_COMPAT, and four functions that sign their
# returns for ARM64; review r2 of arm and armp, L1). $Lacking takes away what it names: cet, cfg, pac.)
function New-FixtureCheck([string] $Name, [uint16] $Machine, [string[]] $Lacking = @()) {
    $image = [byte[]]::new(0x400)
    $put = { param([int] $At, [long] $Value, [int] $Width) [Array]::Copy([BitConverter]::GetBytes($Value), 0, $image, $At, $Width) }
    $image[0] = 0x4D; $image[1] = 0x5A
    & $put 0x3C 0x40 4
    & $put 0x40 0x4550 4
    & $put 0x44 $Machine 2
    & $put 0x46 1 2
    & $put 0x54 240 2
    & $put 0x58 0x20B 2
    & $put (0x58 + 70) $(if ($Lacking -ccontains 'cfg') { 0x0160 } else { 0x4160 }) 2
    & $put (0x58 + 108) 16 4
    # (In its one section, at 0x200 of the file and 0x1000 in memory: the load configuration at 0, its GuardFlags at 144;
    # the debug directory at 0xA0, one entry of type 20 whose word at 0xC0 says CET_COMPAT; the relocations at 0xC8; the
    # unwind table of four functions at 0x100, and their PACIBSP at 0x120.)
    $directory = { param([int] $Index, [int] $At, [int] $Size) & $put (0x58 + 112 + 8 * $Index) (0x1000 + $At) 4; & $put (0x58 + 116 + 8 * $Index) $Size 4 }
    & $put 0x200 148 4
    & $put (0x200 + 144) 0x100 4
    & $directory 10 0 148
    & $put (0x200 + 0xA0 + 12) 20 4
    & $put (0x200 + 0xA0 + 16) 4 4
    & $put (0x200 + 0xA0 + 20) (0x1000 + 0xC0) 4
    & $put (0x200 + 0xA0 + 24) (0x200 + 0xC0) 4
    & $put (0x200 + 0xC0) $(if ($Lacking -ccontains 'cet') { 0 } else { 1 }) 4
    & $directory 6 0xA0 28
    & $directory 5 0xC8 8
    & $directory 3 0x100 32
    if ($Lacking -cnotcontains 'pac') { for ($i = 0; $i -lt 4; ++$i) { & $put (0x200 + 0x120 + 4 * $i) 0xD503237F 4 } }
    $section = 0x58 + 240
    [Text.Encoding]::ASCII.GetBytes('.text').CopyTo($image, $section)
    & $put ($section + 8) 0x200 4
    & $put ($section + 12) 0x1000 4
    & $put ($section + 16) 0x200 4
    & $put ($section + 20) 0x200 4
    & $put ($section + 36) 0x60000020 4
    [Text.Encoding]::ASCII.GetBytes('a made-up update check, never run').CopyTo($image, 0x380)
    $file = Join-Path $work $Name
    [IO.File]::WriteAllBytes($file, $image)
    return $file
}
$fixtureCheck = New-FixtureCheck 'update-check.exe' 0x8664
$fixtureCheckArm = New-FixtureCheck 'update-check-arm64.exe' 0xAA64
function Invoke-Build([string[]] $Package, [string] $Out, [string] $With = $Inno, [switch] $ScanTwin, [string] $Published = $fixturePublished, [string[]] $Check = @($fixtureCheck)) {
    $arguments = @('-NoProfile', '-NonInteractive', '-File', "$PSScriptRoot/build_setup.ps1", '-PackageDirectory', ($Package -join ','), '-Inno', $With, '-OutputDirectory', $Out, '-PublishedFiles', $Published, '-UpdateCheck', ($Check -join ','))
    if ($ScanTwin) { $arguments += '-ScanTwin' }
    $said = & (Get-Process -Id $PID).Path @arguments 2>&1
    return [pscustomobject]@{ Ended = $LASTEXITCODE; Said = ($said | ForEach-Object { "$_" }) -join "`n" }
}
function Expect-Refusal([string[]] $Package, [string] $Expected, [string] $Message, [string] $Out = '', [string] $With = $Inno, [string] $Published = $fixturePublished, [string[]] $Check = @($fixtureCheck)) {
    if (-not $Out) { $Out = Join-Path (Split-Path -Parent $Package[0]) 'out' }
    $result = Invoke-Build $Package $Out $With -Published $Published -Check $Check
    # The refusal's words as they were said, whatever the window: in a console PowerShell wraps an error's text at
    # the window's width between "|" margins and may colour it, which split the words matched here (gate box09 failed
    # three of these for that alone, 2026-11-18, as 9a794ade found for the release commands). No pattern holds a "|".
    $words = ("$($result.Said)" -replace "$([char]27)\[[0-9;]*m", '') -replace '\s*(\|\s*)+', ' '
    Check ($result.Ended -ne 0 -and $words -match $Expected) "$Message (exit $($result.Ended): $($result.Said))"
}

$good = New-FixturePackage 'good' $fixture
$goodArm = New-FixturePackage 'good' $fixtureArm 'arm64'
$goodBoth = @($good, $goodArm)
$goodOut = Join-Path $work 'good/out'
$result = Invoke-Build $goodBoth $goodOut -ScanTwin -Check @($fixtureCheckArm, $fixtureCheck)
Check ($result.Ended -eq 0) "x64's package and Windows on Arm's are made into one setup ($($result.Said))"
# (And x64's alone, as a development build makes it: for x64 Windows alone.)
$x64Out = Join-Path $work 'good/out-x64'
$x64Result = Invoke-Build $good $x64Out
Check ($x64Result.Ended -eq 0) "x64's package alone is made into a setup ($($x64Result.Said))"
if ($x64Result.Ended -eq 0) {
    $said = @(Get-SetupFailures $x64Out @($good) $fixturePublished)
    Check ($said.Count -eq 0) "the setup of x64's package alone passes ($($said -join '; '))"
    $x64Record = Get-Content -LiteralPath (Join-Path $x64Out 'setup-record.json') -Raw | ConvertFrom-Json
    $x64Earlier = [IO.File]::ReadAllText((Join-Path $x64Out 'inputs/earlier.iss')) -replace "`r`n", "`n"
    Check ((@($x64Record.architectures) -join ',') -ceq 'x64' -and $x64Earlier.Contains("function EarlierFilesArm64: String;`nbegin`n  Result := '|';`nend;") -and
        -not (Test-Path -LiteralPath (Join-Path $x64Out 'inputs/qkvm-update-check-arm64.exe'))) 'the setup of x64''s package alone carries nothing of Windows on Arm''s'
}
if ($result.Ended -eq 0) {
    $said = @(Get-SetupFailures $goodOut $goodBoth $fixturePublished)
    # (The list made from it: what the versions before the package's installed and it has not, each once.)
    $earlierMade = [IO.File]::ReadAllText((Join-Path $goodOut 'inputs/earlier.iss'))
    # (All of them once, the first case met; then each version's own, of its own case and kind, with its folders; for
    # each kind the setup installs, with this version's package of the other kind among them.)
    $earlierMade = $earlierMade -replace "`r`n", "`n"
    Check ($earlierMade.Contains("function EarlierFilesX64: String;`nbegin`n  Result := '|';`n  Result := Result + 'arm-old.dll|';`n  Result := Result + 'licenses\arm-only.txt|';`n  Result := Result + 'licenses\old.txt|';`n  Result := Result + 'old.dll|';`n  Result := Result + 'plugins\old\old.dll|';`n  Result := Result + 'qml\Old\Deep\qmldir|';`nend;") -and
        $earlierMade.Contains("  if (Version = '0.0.8') and (Kind = 'x64') then begin`n    Result := Result + 'licenses\old.txt|';`n    Result := Result + 'old.dll|';`n    Result := Result + 'plugins\old\old.dll|';`n    Result := Result + 'qml\Old\Deep\qmldir|';`n  end;`n  if (Version = '0.0.9') and (Kind = 'x64') then begin`n    Result := Result + 'Old.dll|';`n  end;`n  if (Version = '0.0.9') and (Kind = 'arm64') then begin`n    Result := Result + 'arm-old.dll|';`n  end;`n  if (Version = '0.1.0') and (Kind = 'arm64') then begin`n    Result := Result + 'licenses\arm-only.txt|';`n  end;`nend;") -and
        $earlierMade.Contains("  if (Version = '0.0.8') and (Kind = 'x64') then begin`n    Result := Result + 'qml\Old\Deep|';`n    Result := Result + 'qml\Old|';`n    Result := Result + 'qml|';`n    Result := Result + 'plugins\old|';`n    Result := Result + 'plugins|';`n    Result := Result + 'licenses|';`n  end;") -and
        # (On Arm: what x64's package of this very version has and Windows on Arm's has not, removed where x64's was there.)
        $earlierMade.Contains("  if (Version = '0.1.0') and (Kind = 'x64') then begin`n    Result := Result + 'x64-only.dll|';`n  end;`nend;") -and
        $earlierMade.Contains("function EarlierFilesArm64: String;`nbegin`n  Result := '|';`n  Result := Result + 'arm-old.dll|';`n  Result := Result + 'licenses\old.txt|';`n  Result := Result + 'old.dll|';`n  Result := Result + 'plugins\old\old.dll|';`n  Result := Result + 'qml\Old\Deep\qmldir|';`n  Result := Result + 'x64-only.dll|';`nend;") -and
        -not $earlierMade.Contains('same-version') -and -not $earlierMade.Contains('later') -and
        -not $earlierMade.Contains("'Qt6Core.dll|'") -and -not $earlierMade.Contains('AGPL') -and -not $earlierMade.Contains('package-manifest')) 'the setup removes what the versions before the package''s, and this version''s other kind, installed and it has not, and nothing else'
    Check ($said.Count -eq 0) "the setup of x64's package and Windows on Arm's passes ($($said -join '; '))"
    $record = Get-Content -LiteralPath (Join-Path $goodOut 'setup-record.json') -Raw | ConvertFrom-Json
    Check ($record.setup.name -ceq 'qKVM-Desktop-0.1.0-windows-setup.exe' -and $record.setup.sha256 -ceq (Get-Sha256 (Join-Path $goodOut $record.setup.name))) 'the record names the setup by its SHA-256'
    Check ($record.inno.version -ceq $InnoVersion -and (@($record.architectures) -join ',') -ceq 'x64,arm64' -and
        @($record.packages | Where-Object { $_.architecture -ceq 'x64' -and $_.manifestSha256 -ceq (Get-Sha256 (Join-Path $good 'package-manifest.json')) }).Count -eq 1 -and
        @($record.packages | Where-Object { $_.architecture -ceq 'arm64' -and $_.manifestSha256 -ceq (Get-Sha256 (Join-Path $goodArm 'package-manifest.json')) }).Count -eq 1 -and
        @($record.updateChecks | Where-Object { $_.architecture -ceq 'arm64' -and $_.name -ceq 'qkvm-update-check-arm64.exe' -and $_.sha256 -ceq (Get-Sha256 $fixtureCheckArm) }).Count -eq 1) 'the record names the Inno Setup, the packages and the update checks it was made with, by their kinds'

    # Each way of being wrong, alone, on a copy of what was made.
    function Expect-Failure([string] $Name, [scriptblock] $Change, [string] $Expected) {
        $copy = Join-Path $work "wrong-$Name"
        Copy-Item -LiteralPath $goodOut -Destination $copy -Recurse
        & $Change $copy
        $said = @(Get-SetupFailures $copy $goodBoth $fixturePublished)
        Check (@($said | Where-Object { $_ -match $Expected }).Count -gt 0) "a setup $Name fails ($($said -join '; '))"
    }
    function Edit-Script([string] $Copy, [string] $From, [string] $To) {
        $file = Join-Path $Copy 'inputs/qkvm-desktop.iss'
        $text = [IO.File]::ReadAllText($file)
        # (The script is kept with Unix line ends: .gitattributes.)
        $From = $From.Replace("`r`n", "`n")
        $To = $To.Replace("`r`n", "`n")
        if (-not $text.Contains($From)) { throw "The script has no $From to change." }
        [IO.File]::WriteAllText($file, $text.Replace($From, $To))
    }
    function Edit-List([string] $Copy, [scriptblock] $Rows) {
        $file = Join-Path $Copy 'setup-files.txt'
        $lines = [Collections.Generic.List[string]]::new([string[]] @(Get-Content -LiteralPath $file))
        & $Rows $lines
        Set-Content -LiteralPath $file -Value $lines -Encoding utf8
    }
    $sha = { param([string] $text) [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($text))).ToLowerInvariant() }
    Expect-Failure 'with a file more' { param($c) Edit-List $c { param($l) $l.Add("99`t$good\extra.dll`t2026-01-01 00:00:00.000`t`t$(& $sha 'x')`t1`t1`t1`t0`t0`t1`tno`t") } } 'a file the package does not have'
    Expect-Failure 'with a file less' { param($c) Edit-List $c { param($l) $l.RemoveAt(@($l | Select-String -SimpleMatch 'Qt6Core.dll')[0].LineNumber - 1) } } 'lacks Qt6Core.dll'
    Expect-Failure 'with other bytes' { param($c) Edit-List $c { param($l) $at = @($l | Select-String -SimpleMatch 'Qt6Core.dll')[0].LineNumber - 1; $l[$at] = $l[$at] -replace '\t[0-9a-f]{64}\t', "`t$('0' * 64)`t" } } 'other bytes .* Qt6Core\.dll'
    Expect-Failure 'with a file put elsewhere' { param($c) $f = Join-Path $c 'inputs/files.iss'; [IO.File]::WriteAllText($f, [IO.File]::ReadAllText($f).Replace('DestDir: "{app}\plugins\platforms"', 'DestDir: "{app}"')) } 'elsewhere than its own place'
    Expect-Failure 'with a file at a place of its own choosing' { param($c) $f = Join-Path $c 'inputs/files.iss'; [IO.File]::AppendAllText($f, "Source: `"Qt6Core.dll`"; DestDir: `"{sys}`"; Flags: ignoreversion`r`n") } 'not one file at its own place'
    Expect-Failure 'from another script' { param($c) Edit-Script $c 'AppPublisherURL=https://qkvm.com/' 'AppPublisherURL=https://qkvm.com/x' } 'not packaging/qkvm-desktop\.iss as committed'
    Expect-Failure 'for an administrator' { param($c) Edit-Script $c 'PrivilegesRequired=lowest' 'PrivilegesRequired=admin' } 'PrivilegesRequired is not lowest'
    Expect-Failure 'that may be overridden' { param($c) Edit-Script $c 'PrivilegesRequired=lowest' "PrivilegesRequired=lowest`r`nPrivilegesRequiredOverridesAllowed=commandline" } 'may be overridden'
    Expect-Failure 'with another mutex' { param($c) Edit-Script $c "|$ProgramMutex}" '|Global\QKVM Desktop}' } 'AppMutex is not'
    Expect-Failure 'that never waits' { param($c) Edit-Script $c 'UpdateWaitMs = 60000;' 'UpdateWaitMs = 0;' } 'lacks: UpdateWaitMs = 60000'
    Expect-Failure 'that ends with Inno Setup''s own 5' { param($c) Edit-Script $c 'StillRunningExitCode = 105;' 'StillRunningExitCode = 5;' } 'lacks: StillRunningExitCode = 105'
    Expect-Failure 'without a mutex of its own' { param($c) Edit-Script $c 'SetupMutex=qKVM-Desktop-Setup' '' } 'SetupMutex=qKVM-Desktop-Setup'
    Expect-Failure 'with its mutex for the whole PC' { param($c) Edit-Script $c 'SetupMutex=qKVM-Desktop-Setup' 'SetupMutex=Global\qKVM-Desktop-Setup' } 'SetupMutex=qKVM-Desktop-Setup'
    Expect-Failure 'without the marker' { param($c) Edit-Script $c 'Source: "{#SourcePath}qkvm-desktop.installed"; DestDir: "{app}"; Flags: ignoreversion' '' } 'the marker at'
    Expect-Failure 'with another marker' { param($c) [IO.File]::WriteAllText((Join-Path $c 'inputs/qkvm-desktop.installed'), 'setup') } 'marker is not'
    Expect-Failure 'compressed otherwise' { param($c) Edit-Script $c "#else`r`nCompression=lzma2" "#else`r`nCompression=zip" } 'lzma2, solid'
    # (x64compatible: x64 Windows, and Windows 11 on Arm, which runs x64 programs; x64os refuses Arm, as 0.1.2's did.)
    Expect-Failure 'for x64 Windows alone' { param($c) Edit-Script $c 'ArchitecturesAllowed=x64compatible' 'ArchitecturesAllowed=x64os' } 'ArchitecturesAllowed=x64compatible'
    Expect-Failure 'installing in 32-bit mode on Arm' { param($c) Edit-Script $c 'ArchitecturesInstallIn64BitMode=x64compatible' 'ArchitecturesInstallIn64BitMode=x64os' } 'ArchitecturesInstallIn64BitMode=x64compatible'
    Expect-Failure 'that closes programs' { param($c) Edit-Script $c 'CloseApplications=no' 'CloseApplications=yes' } 'CloseApplications=no'
    Expect-Failure 'that turns the junction guard off' { param($c) Edit-Script $c 'CloseApplications=no' "CloseApplications=no`r`nRedirectionGuard=no" } 'turns RedirectionGuard off'
    Expect-Failure 'that deletes before it installs' { param($c) Edit-Script $c '[Files]' "[InstallDelete]`r`nType: filesandordirs; Name: `"{app}\plugins`"`r`n`r`n[Files]" } '\[InstallDelete\] section'
    Expect-Failure 'that sets the earlier version aside in update mode alone' { param($c) Edit-Script $c "    if (Result = '') and (Count > 0) then begin`r`n      Suffix := FreeEnding;" "    if (Result = '') and (Count > 0) and UpdateMode then begin`r`n      Suffix := FreeEnding;" } 'staged install lacks'
    # (A failed set-aside stops the install before any file is copied, review C7.)
    Expect-Failure 'that copies though a file could not be set aside' { param($c) Edit-Script $c "            else`r`n              Result := Format('a file of qKVM Desktop could not be set aside for the copy (%s)', [Names[I]]);" "            else`r`n              Result := '';" } 'staged install lacks'
    Expect-Failure 'that sets the earlier version aside only as it copies' { param($c) Edit-Script $c "  if Reason = '' then`r`n    Reason := SetPackageAside;`r`n" ''; Edit-Script $c "  if CurStep = ssInstall then begin`r`n    Installing := True;" "  if CurStep = ssInstall then begin`r`n    Installing := True;`r`n    SetPackageAside;" } 'not set aside last in Preparing to Install'
    Expect-Failure 'that starts the program it closed though the copy went on' { param($c) Edit-Script $c "    StartAtEnd := False;`r`n  end;" "  end;" } 'removal after the copy lacks'
    Expect-Failure 'that starts it at the end of a whole copy' { param($c) Edit-Script $c "  if StartAtEnd then`r`n    StartAgain;`r`nend;" "  if StartAtEnd or PostInstalled then`r`n    StartAgain;`r`nend;" } 'StartAgain'
    Expect-Failure 'that sets the earlier version aside before it looks at files in use' { param($c) Edit-Script $c "  if Reason = '' then`r`n    Reason := SetPackageAside;`r`n" ''; Edit-Script $c "  Reason := RoomReason;`r`n" "  Reason := RoomReason;`r`n  if Reason = '' then`r`n    Reason := SetPackageAside;`r`n" } 'not set aside last in Preparing to Install'
    Expect-Failure 'that never puts the earlier version back' { param($c) Edit-Script $c "    if PutPackageBack and UpdateMode then`r`n      StartAgain;`r`n  if StartAtEnd then" "    Log('');`r`n  if StartAtEnd then" } 'removal after the copy lacks'
    # (L4 and L5.)
    Expect-Failure 'that says nothing was changed after it put back' { param($c) Edit-Script $c "    if PutRightDone = '' then`r`n      Reason := 'Nothing was changed.'" "    if True then`r`n      Reason := 'Nothing was changed.'" } 'removal after the copy lacks'
    Expect-Failure 'that never knows it put back' { param($c) Edit-Script $c "      if Count > 0 then`r`n        SayPutRight('put back the earlier version," "      if False then`r`n        SayPutRight('put back the earlier version," } 'removal after the copy lacks'
    Expect-Failure 'whose failed update leaves qKVM closed' { param($c) Edit-Script $c "    if PutPackageBack and UpdateMode then`r`n      StartAgain;" "    PutPackageBack;" } 'DeinitializeSetup does not reach'
    Expect-Failure 'whose failed copy starts qKVM again outside update mode' { param($c) Edit-Script $c "    if PutPackageBack and UpdateMode then`r`n      StartAgain;" "    if PutPackageBack then`r`n      StartAgain;" } 'DeinitializeSetup does not reach'
    Expect-Failure 'that leaves what it set aside after a whole copy' { param($c) Edit-Script $c "      else begin`r`n        Finished := FinishCopy;`r`n" "      else begin`r`n" } 'staged install lacks'
    Expect-Failure 'that leaves what a setup ended halfway left' { param($c) Edit-Script $c "      RemoveEarlierFiles(EarlierHere, EarlierKindHere);`r`n    end;`r`n    RemoveSetupLeftovers;" "      RemoveEarlierFiles(EarlierHere, EarlierKindHere);`r`n    end;" } 'removal after the copy lacks'
    # (What earlier versions installed and this one has not, step 4: each protection taken away, alone.)
    Expect-Failure 'whose earlier list names a file the package has' { param($c) $k = Join-Path $c 'inputs/earlier.iss'; [IO.File]::WriteAllText($k, [IO.File]::ReadAllText($k).Replace("function EarlierFilesX64: String;`r`nbegin`r`n  Result := '|';", "function EarlierFilesX64: String;`r`nbegin`r`n  Result := '|';`r`n  Result := Result + 'Qt6Core.dll|';")) } 'list of what earlier versions installed'
    Expect-Failure 'whose earlier list lacks one' { param($c) $k = Join-Path $c 'inputs/earlier.iss'; [IO.File]::WriteAllText($k, (@([IO.File]::ReadAllLines($k)) | Where-Object { $_ -ne "  Result := Result + 'old.dll|';" }) -join "`n") } 'list of what earlier versions installed'
    Expect-Failure 'whose earlier list names what a later version installed' { param($c) $k = Join-Path $c 'inputs/earlier.iss'; [IO.File]::WriteAllText($k, [IO.File]::ReadAllText($k).Replace("  Result := Result + 'old.dll|';", "  Result := Result + 'old.dll|';`r`n  Result := Result + 'later.dll|';")) } 'list of what earlier versions installed'
    Expect-Failure 'whose earlier folders come before what is within them' { param($c) $k = Join-Path $c 'inputs/earlier.iss'; [IO.File]::WriteAllText($k, [IO.File]::ReadAllText($k).Replace("    Result := Result + 'qml\Old\Deep|';`r`n    Result := Result + 'qml\Old|';", "    Result := Result + 'qml\Old|';`r`n    Result := Result + 'qml\Old\Deep|';")) } 'each earlier version''s folders'
    Expect-Failure 'whose earlier list does more than say them' { param($c) $k = Join-Path $c 'inputs/earlier.iss'; [IO.File]::AppendAllText($k, "procedure Other;`nbegin`nend;`n") } 'earlier.iss is not the three functions'
    Expect-Failure 'whose list of one earlier version names a file the package has' { param($c) $k = Join-Path $c 'inputs/earlier.iss'; [IO.File]::WriteAllText($k, [IO.File]::ReadAllText($k).Replace("  if (Version = '0.0.9') and (Kind = 'x64') then begin`r`n", "  if (Version = '0.0.9') and (Kind = 'x64') then begin`r`n    Result := Result + 'Qt6Core.dll|';`r`n")) } 'what each earlier version installed'
    Expect-Failure 'whose list of one earlier version names another''s file' { param($c) $k = Join-Path $c 'inputs/earlier.iss'; [IO.File]::WriteAllText($k, [IO.File]::ReadAllText($k).Replace("  if (Version = '0.0.9') and (Kind = 'x64') then begin`r`n", "  if (Version = '0.0.9') and (Kind = 'x64') then begin`r`n    Result := Result + 'licenses\old.txt|';`r`n")) } 'what each earlier version installed'
    Expect-Failure 'that never removes what an earlier version installed' { param($c) Edit-Script $c "      RemoveEarlierFiles(EarlierHere, EarlierKindHere);`r`n    end;`r`n" "      Log('');`r`n    end;`r`n" } 'staged install lacks'
    Expect-Failure 'that removes what an earlier version installed though the copy was not marked finished' { param($c) Edit-Script $c "    if Finished then begin`r`n      // (The kind" "    begin`r`n      // (The kind" } 'staged install lacks'
    # (M4: only where that very version was installed in that very folder, and by its own list alone.)
    Expect-Failure 'that removes what an earlier version installed where none is proven' { param($c) Edit-Script $c "  EarlierHere := InstalledHere;" "  EarlierHere := InstalledVersion;" } 'staged install lacks'
    Expect-Failure 'that takes another folder for the one installed' { param($c) Edit-Script $c "  if not PathSame(InstalledFolder, RemoveBackslashUnlessRoot(ExpandConstant('{app}'))) then`r`n    Exit;`r`n" '' } 'staged install lacks'
    Expect-Failure 'that takes a folder without the contract''s marker for one installed' { param($c) Edit-Script $c "  if OurInstallIn(ExpandConstant('{app}')) then`r`n    Result := InstalledVersion;" "  Result := InstalledVersion;" } 'staged install lacks'
    Expect-Failure 'that takes any marker for the contract''s' { param($c) Edit-Script $c "    Result := Said = 'setup' + #10;" "    Result := True;" } 'staged install lacks'
    # (M1, review i020-2: a folder that holds what is not an install of ours is refused before anything is closed or
    # changed, whatever its names; nothing of it set aside or put right by 0.1.2's rule; each protection alone.)
    Expect-Failure 'that takes a marker of any size for the contract''s' { param($c) Edit-Script $c " or (Found.SizeHigh <> 0) or (Found.SizeLow <> 6) then" " then" } 'staged install lacks'
    Expect-Failure 'that takes a folder named qkvm-desktop.installed for the contract''s marker' { param($c) Edit-Script $c "    if ((Found.Attributes and (DirectoryAttribute or ReparsePointAttribute)) <> 0) or (Found.SizeHigh" "    if (Found.SizeHigh" } 'staged install lacks'
    Expect-Failure 'that installs into a folder of another''s' { param($c) Edit-Script $c "  if Reason = '' then`r`n    Reason := ForeignFolderReason;`r`n" '' } 'closing of a running copy lacks'
    Expect-Failure 'that closes the program for an install into a folder of another''s' { param($c) Edit-Script $c "  if ForeignFolderReason <> '' then`r`n    Exit;`r`n  // (Nor for one" "  // (Nor for one" } 'closing of a running copy lacks'
    Expect-Failure 'that closes a silent install''s program before it looks at the folder' { param($c) Edit-Script $c "  if Reason = '' then`r`n    Reason := ForeignFolderReason;`r`n  if Reason = '' then`r`n    Reason := RightsReason;`r`n  if (Reason = '') and WizardSilent and not UpdateMode then`r`n    Reason := CloseProgramSilently;`r`n" "  if (Reason = '') and WizardSilent and not UpdateMode then`r`n    Reason := CloseProgramSilently;`r`n  if Reason = '' then`r`n    Reason := ForeignFolderReason;`r`n  if Reason = '' then`r`n    Reason := RightsReason;`r`n" } 'closing of a running copy lacks'
    Expect-Failure 'that sets aside in a folder with no install of ours' { param($c) Edit-Script $c "  if not OurInstallIn(ExpandConstant('{app}')) then begin`r`n    Result := ForeignFolderReason;`r`n    Exit;`r`n  end;`r`n" '' } 'staged install lacks'
    Expect-Failure 'that takes a folder of another''s for one of its own' { param($c) Edit-Script $c "    Result := Format('the folder %s holds what is not an install of qKVM Desktop" "    Result := ''; Name := Format('the folder %s holds what is not an install of qKVM Desktop" } 'staged install lacks'
    Expect-Failure 'that takes a folder it cannot look through for an empty one' { param($c) Edit-Script $c "    CutShort := 'the folder ' + Base + ' cannot be looked through, to see that it holds nothing but qKVM Desktop';" "    CutShort := '';" } 'staged install lacks'
    Expect-Failure 'that takes a folder looked through in part for an empty one' { param($c) Edit-Script $c "          if Error <> ErrorNoMoreFiles then`r`n            CutShort := Format('the folder %s could not be looked through" "          if False then`r`n            CutShort := Format('the folder %s could not be looked through" } 'staged install lacks'
    # (Reviews 1.1 and 1.8: a record decides only in a folder of ours, decided first; where only what a setup of ours
    # left is there, what the records name is removed, never put back, and never a file of the package's own name.)
    Expect-Failure 'that puts right by a record in a folder of another''s' { param($c) Edit-Script $c "  if not OurInstallIn(Folder) then begin`r`n    Result := LeftoversOnlyReason(Folder);" "  if False then begin`r`n    Result := LeftoversOnlyReason(Folder);" } 'staged install lacks'
    Expect-Failure 'that puts back what an uninstall left' { param($c) Edit-Script $c "      DropRecorded(Folder, False);" "      PutBackRecorded(Folder);" } 'staged install lacks'
    Expect-Failure 'that removes files of the package''s names where no install of ours is' { param($c) Edit-Script $c "        if Installed and (M = 1) then" "        if M = 1 then" } 'staged install lacks'
    Expect-Failure 'that takes a folder of any files for what a setup left' { param($c) Edit-Script $c "        end else if Pos('|' + Lowercase(Entry) + '|', Leftovers) = 0 then`r`n          Result := Entry;" "        end;" } 'staged install lacks'
    Expect-Failure 'that takes a folder of any name for one of its own' { param($c) Edit-Script $c "          if OwnFolderName(Entry) then" "          if True then" } 'staged install lacks'
    Expect-Failure 'that looks through a redirection for what a setup left' { param($c) Edit-Script $c "        if (Attributes and ReparsePointAttribute) <> 0 then`r`n          Result := Entry`r`n        else if" "        if" } 'staged install lacks'
    Expect-Failure 'that takes a record that does not read for what a setup left' { param($c) Edit-Script $c "      if ReadRecord(AddBackslash(Folder) + Markers[M], Suffix, Names, Endings, Kinds, Copies) then begin`r`n        Result := Result + Lowercase(Markers[M]) + '|';" "      if ReadRecord(AddBackslash(Folder) + Markers[M], Suffix, Names, Endings, Kinds, Copies) or True then begin`r`n        Result := Result + Lowercase(Markers[M]) + '|';" } 'staged install lacks'
    # (Review 1.2: a folder another account may change, refused before anything is closed or changed.)
    Expect-Failure 'that installs into a folder another account may change' { param($c) Edit-Script $c "  if Reason = '' then`r`n    Reason := RightsReason;`r`n" '' } 'closing of a running copy lacks'
    Expect-Failure 'that closes the program for an install into a folder another account may change' { param($c) Edit-Script $c "  if RightsReason <> '' then`r`n    Exit;`r`n" '' } 'closing of a running copy lacks'
    Expect-Failure 'that trusts every account with the folder' { param($c) Edit-Script $c "  Result := (Account = Own) or (Account = 'SY')" "  Result := True or (Account = Own) or (Account = 'SY')" } 'folder''s rights are not looked at'
    Expect-Failure 'that takes any owner of the folder' { param($c) Edit-Script $c "  if not TrustedAccount(Owner, Own) then begin" "  if False then begin" } 'folder''s rights are not looked at'
    Expect-Failure 'that takes rights it does not read for none' { param($c) Edit-Script $c "      if not RightsMask(Words, Top, Bottom) then begin" "      if False then begin" } 'folder''s rights are not looked at'
    Expect-Failure 'that takes an entry of another kind for harmless' { param($c) Edit-Script $c "    if (Kind <> 'A') and (Kind <> 'D') then begin" "    if False then begin" } 'folder''s rights are not looked at'
    Expect-Failure 'that takes a folder with no list for one of its own' { param($c) Edit-Script $c "  if Pos('(', List) = 0 then begin`r`n    if Pos('NO_ACCESS_CONTROL', List) > 0 then" "  if Pos('(', List) = 0 then begin`r`n    if False then" } 'folder''s rights are not looked at'
    Expect-Failure 'that takes writing for reading' { param($c) Edit-Script $c '  ReadsLow = $00A9;' '  ReadsLow = $FFFF;' } 'folder''s rights are not looked at'
    Expect-Failure 'that lets another account move a folder above away' { param($c) Edit-Script $c '  AboveHigh = $130D;' '  AboveHigh = $0000;' } 'folder''s rights are not looked at'
    Expect-Failure 'that looks at the folder alone, not those above it' { param($c) Edit-Script $c "  while (Why = '') and (Up <> '') do begin" "  while False do begin" } 'folder''s rights are not looked at'
    Expect-Failure 'that installs on a network share' { param($c) Edit-Script $c "  if (Copy(Folder, 1, 2) = '\\') or (GetDriveTypeW(AddBackslash(ExtractFileDrive(Folder))) = DriveRemote) then begin" "  if False then begin" } 'folder''s rights are not looked at'
    Expect-Failure 'that takes a redirection for a folder of its own' { param($c) Edit-Script $c "  if ThroughRedirection(Folder, '') then begin`r`n    Result := 'the folder ' + Folder + ' is a redirection" "  if False then begin`r`n    Result := 'the folder ' + Folder + ' is a redirection" } 'staged install lacks'
    Expect-Failure 'that installs into a redirection' { param($c) Edit-Script $c "  if Made and ThroughRedirection(Folder, '') then begin" "  if False then begin" } 'folder''s rights are not looked at'
    Expect-Failure 'that makes the folder safe in its own way' { param($c) Edit-Script $c "  Result := '';`r`n  if (Copy(Folder, 1, 2) = '\\') or (GetDriveTypeW" "  Result := '';`r`n  SetNamedSecurityInfoW(Folder);`r`n  if (Copy(Folder, 1, 2) = '\\') or (GetDriveTypeW" } 'writes rights'
    # (Outside review 2, 1.3: a refusal for the folder's rights says that what an update left there stays, and what to
    # do; by a look at names alone, and none on a network share.)
    Expect-Failure 'that says nothing of what an update left where it refuses a folder for its rights' { param($c) Edit-Script $c "  if ThingAt(AddBackslash(Folder) + UnfinishedMarker) or ThingAt(AddBackslash(Folder) + FinishingMarker) or ThingAt(AddBackslash(Folder) + 'qkvm-desktop.exe' + AsideEnding) then" "  if False then" } 'does not say, where it refuses a folder for its rights'
    Expect-Failure 'that looks on a network share for what an update left' { param($c) Edit-Script $c "  if (Result = '') or (Copy(Folder, 1, 2) = '\\') or (GetDriveTypeW(AddBackslash(ExtractFileDrive(Folder))) = DriveRemote) then" "  if Result = '' then" } 'does not say, where it refuses a folder for its rights'
    # (Outside review 2, 1.2: what the files made in a new folder take of the folder it is made in, judged too.)
    Expect-Failure 'that judges a folder not made yet by what a folder takes alone' { param($c) Edit-Script $c "      if MadeWithin and not HasPair(Flags, 'CI') and not HasPair(Flags, 'OI') then" "      if MadeWithin and not HasPair(Flags, 'CI') then" } 'folder''s rights are not looked at'
    # (Outside review 2, 1.1: the Windows account this setup runs as, of whatever authority, read where Windows says.)
    Expect-Failure 'that takes only an account of NT''s authority for its own' { param($c) Edit-Script $c "  Sid := Copy(Info, At div 2 + 1, 4 + 2 * Count);" "  if Ord(Info[At div 2 + 4]) <> `$0500 then`r`n    Exit;`r`n  Sid := Copy(Info, At div 2 + 1, 4 + 2 * Count);" } 'only where it is of one kind'
    Expect-Failure 'that reads its account at a place of its own choosing' { param($c) Edit-Script $c "  if Ord(Info[2]) = BaseHigh then`r`n    At := Ord(Info[1]) - BaseLow`r`n  else if Ord(Info[2]) = BaseHigh + 1 then`r`n    At := Ord(Info[1]) + 65536 - BaseLow`r`n  else`r`n    Exit;" "  At := 8;" } 'does not read the Windows account it runs as where Windows says'
    Expect-Failure 'that reads its account past what Windows wrote' { param($c) Edit-Script $c "  if (Count < 1) or (Count > 15) or (At + 8 + 4 * Count > Bytes) then" "  if (Count < 1) or (Count > 15) then" } 'does not read the Windows account it runs as where Windows says'
    Expect-Failure 'that reads its account by a pointer it does not check' { param($c) Edit-Script $c "  Bytes := Needed;`r`n  if Bytes > 2 * Length(Info) then`r`n    Exit;" "  Bytes := Needed;" } 'does not read the Windows account it runs as where Windows says'
    Expect-Failure 'whose look for a redirection skips the folder itself' { param($c) Edit-Script $c "  if (Length(Path) > 2) and FindFirst(Path, Found) then begin`r`n    Result := (Found.Attributes and ReparsePointAttribute) <> 0;`r`n    FindClose(Found);`r`n  end;`r`n  Rest := Relative;" "  Rest := Relative;" } 'removal after the copy lacks'
    # (Reviews 1.3 and 1.4: the account's gate, by the setup and the uninstall; the profile's lock where no copy made it;
    # the marker's hold checked.)
    Expect-Failure 'that begins while another setup of the account is at work' { param($c) Edit-Script $c "  Reason := TakeAccountGate;`r`n  if Reason <> '' then begin`r`n    Log('qKVM Desktop Setup does not begin" "  Reason := '';`r`n  if Reason <> '' then begin`r`n    Log('qKVM Desktop Setup does not begin" } 'gate of the account'
    Expect-Failure 'whose uninstall begins while a setup of the account is at work' { param($c) Edit-Script $c "  Running := TakeAccountGate;" "  Running := '';" } 'uninstaller does not refuse'
    Expect-Failure 'whose gate is of the session alone' { param($c) Edit-Script $c "  AccountGatePrefix = 'Global\qKVM-Desktop-Setup-Account-';" "  AccountGatePrefix = 'Local\qKVM-Desktop-Setup-Account-';" } 'gate of the account is not the one the program takes'
    Expect-Failure 'that takes a gate it waited for in vain' { param($c) Edit-Script $c "  if (Answer = WaitObject0) or (Answer = WaitAbandoned) then`r`n    Exit;`r`n  CloseHandle(AccountGate);" "  Exit;`r`n  CloseHandle(AccountGate);" } 'gate of the account'
    Expect-Failure 'that takes a gate it does not own for its own' { param($c) Edit-Script $c "  AccountGate := CreateMutexW(0, 1, AccountGatePrefix + Own);" "  AccountGate := CreateMutexW(0, 0, AccountGatePrefix + Own);" } 'gate of the account'
    Expect-Failure 'that takes a gate it may not open for a free one' { param($c) Edit-Script $c "  if AccountGate = 0 then begin`r`n    Result := Format('it could not be seen whether" "  if False then begin`r`n    Result := Format('it could not be seen whether" } 'gate of the account'
    Expect-Failure 'whose uninstall holds no lock where no copy has run' { param($c) Edit-Script $c "  if (Opened = ErrorFileNotFound) or (Opened = ErrorPathNotFound) then begin`r`n    Folder := ExtractFileDir" "  if False then begin`r`n    Folder := ExtractFileDir" } 'lock of its profile'
    Expect-Failure 'whose uninstall leaves the lock it made' { param($c) Edit-Script $c "  if DeleteFile(ProfileFileMade) then" "  if False then" } 'lock of its profile'
    Expect-Failure 'whose uninstall removes a lock a start made' { param($c) Edit-Script $c "    if Opened = 0 then`r`n      ProfileFileMade := ExpandConstant(ProfileLockFile)" "    if True then`r`n      ProfileFileMade := ExpandConstant(ProfileLockFile)" } 'lock of its profile'
    Expect-Failure 'that sets aside under a marker it does not hold' { param($c) Edit-Script $c "        if not HoldMarker(Text) then begin" "        if False then begin" } 'staged install lacks'
    Expect-Failure 'that takes any handle for its hold of the marker' { param($c) Edit-Script $c "  Result := GetFileType(MarkerHold) = FileTypeDisk;" "  Result := True;" } 'hold of its marker'
    Expect-Failure 'that takes another record for the one it wrote' { param($c) Edit-Script $c "    Result := Result and (Back = Text);" "    Result := True;" } 'hold of its marker'
    Expect-Failure 'that removes what it looks at to tell what a setup left' { param($c) Edit-Script $c "        end else if Pos('|' + Lowercase(Entry) + '|', Leftovers) = 0 then`r`n          Result := Entry;" "        end else if Pos('|' + Lowercase(Entry) + '|', Leftovers) = 0 then`r`n          Result := Entry`r`n        else`r`n          RemoveOwn(AddBackslash(Folder) + Entry);" } 'walks a folder of the program''s'
    Expect-Failure 'that takes a folder of its own for any marker''s' { param($c) Edit-Script $c "  Folder := RemoveBackslashUnlessRoot(ExpandConstant('{app}'));`r`n  if OurInstallIn(Folder) then`r`n    Exit;" "  Folder := RemoveBackslashUnlessRoot(ExpandConstant('{app}'));`r`n  if FileExists(AddBackslash(Folder) + 'qkvm-desktop.installed') then`r`n    Exit;" } 'staged install lacks'
    Expect-Failure 'that puts the program of 0.1.2 right where no install of ours is' { param($c) Edit-Script $c "  if not OurInstallIn(Folder) then`r`n    Exit;`r`n  Base := AddBackslash(Folder) + 'qkvm-desktop.exe';" "  Base := AddBackslash(Folder) + 'qkvm-desktop.exe';" } 'staged install lacks'
    Expect-Failure 'that walks the folder to see whether it holds anything' { param($c) Edit-Script $c "procedure PutProgramRight(Folder: String);`r`nvar" "procedure LookAgain(Folder: String);`r`nvar`r`n  Found: TFindRec;`r`nbegin`r`n  if FindFirst(Folder + '\plugins\*', Found) then`r`n    FindClose(Found);`r`nend;`r`n`r`nprocedure PutProgramRight(Folder: String);`r`nvar" } 'walks a folder of the program''s'
    Expect-Failure 'that removes what every earlier version installed' { param($c) Edit-Script $c "    AddNames(EarlierFilesOf(Version, Kind), Names);" "    AddNames(EarlierFiles, Names);" } 'staged install lacks'
    Expect-Failure 'that walks a folder again to remove what is in it' { param($c) Edit-Script $c "procedure PutProgramRight(Folder: String);`r`nvar" "procedure WalkAgain(Folder: String);`r`nvar`r`n  Found: TFindRec;`r`nbegin`r`n  if FindFirst(Folder + '\*', Found) then`r`n    FindClose(Found);`r`nend;`r`n`r`nprocedure PutProgramRight(Folder: String);`r`nvar" } 'walks a folder of the program''s'
    Expect-Failure 'that removes an earlier file through a redirection' { param($c) Edit-Script $c "      if PlainFileAt(Path) and not ThroughRedirection(ExpandConstant('{app}'), RemoveBackslash(ExtractFilePath(Names[I]))) then begin" "      if PlainFileAt(Path) then begin" } 'staged install lacks'
    Expect-Failure 'that removes what is not a plain file of an earlier version''s' { param($c) Edit-Script $c "      if PlainFileAt(Path) and not ThroughRedirection(ExpandConstant('{app}'), RemoveBackslash(ExtractFilePath(Names[I]))) then begin" "      if ThingAt(Path) and not ThroughRedirection(ExpandConstant('{app}'), RemoveBackslash(ExtractFilePath(Names[I]))) then begin" } 'staged install lacks'
    Expect-Failure 'that removes an earlier folder through a redirection' { param($c) Edit-Script $c "      if ThingAt(Path) and not ThroughRedirection(ExpandConstant('{app}'), Names[I]) then`r`n        if RemoveDir(Path) then" "      if ThingAt(Path) then`r`n        if RemoveDir(Path) then" } 'staged install lacks'
    Expect-Failure 'whose record takes an earlier version''s file for none of its own' { param($c) Edit-Script $c "Lowercase(PackageFiles + EarlierFiles)) > 0);" "Lowercase(PackageFiles)) > 0);" } 'staged install lacks'
    # (H1: what is the setup's own is what its record names, by the record's own ending; nothing by an ending alone.)
    Expect-Failure 'whose record names what is not its own' { param($c) Edit-Script $c "  Result := (Name <> '') and (Pos('|', Name) = 0) and (Pos('|' + Lowercase(Name) + '|', Lowercase(PackageFiles + EarlierFiles)) > 0);" "  Result := Name <> '';" } 'staged install lacks'
    # (L6 of review i020-2: two names of the list side by side, or two endings, are not one; a record larger than 1 MiB is
    # not read.)
    Expect-Failure 'whose record takes two names side by side for one of its own' { param($c) Edit-Script $c "  Result := (Name <> '') and (Pos('|', Name) = 0) and " "  Result := (Name <> '') and " } 'staged install lacks'
    Expect-Failure 'whose record takes two endings side by side for one of its own' { param($c) Edit-Script $c "  Result := (Suffix <> '') and (Pos('|', Suffix) = 0) and " "  Result := (Suffix <> '') and " } 'staged install lacks'
    Expect-Failure 'whose record is read whatever its size' { param($c) Edit-Script $c "  if not AtMost(Path, MostRecord) then`r`n    Exit;`r`n" '' } 'staged install lacks'
    Expect-Failure 'whose record is read up to another size than the program''s' { param($c) Edit-Script $c "  MostRecord = 1048576;" "  MostRecord = 104857600;" } 'staged install lacks'
    Expect-Failure 'whose look at a size takes one of more than 4 GiB for a small one' { param($c) Edit-Script $c "    Result := (Found.SizeHigh = 0) and (Found.SizeLow <= Most);" "    Result := Found.SizeLow <= Most;" } 'staged install lacks'
    # (L5: the markers and the drafts removed as the setup's own files are, read-only cleared first and tried again.)
    Expect-Failure 'that removes a finishing marker without making it writable' { param($c) Edit-Script $c "  else if not RemoveOwn(Marker) then" "  else if not DeleteFile(Marker) then" } 'deletes or renames otherwise'
    Expect-Failure 'that removes an unfinished marker without making it writable' { param($c) Edit-Script $c "  if Left = 0 then begin`r`n    if not RemoveOwn(Marker) then" "  if Left = 0 then begin`r`n    if not DeleteFile(Marker) then" } 'deletes or renames otherwise'
    Expect-Failure 'whose set-aside removes the earlier finishing marker without making it writable' { param($c) Edit-Script $c "        if not RemoveOwn(InApp(FinishingMarker)) then begin" "        if not DeleteFile(InApp(FinishingMarker)) then begin" } 'deletes or renames otherwise'
    Expect-Failure 'whose uninstaller removes a marker without making it writable' { param($c) Edit-Script $c "          RemoveOwn(Markers[M])" "          DeleteFile(Markers[M])" } 'deletes or renames otherwise'
    Expect-Failure 'that removes the record''s draft without making it writable' { param($c) Edit-Script $c "    if not RemoveOwn(Draft) then`r`n      Exit;`r`n  if SaveStringToFile(" "    if not DeleteFile(Draft) then`r`n      Exit;`r`n  if SaveStringToFile(" } 'deletes or renames otherwise'
    Expect-Failure 'whose record is read though not whole' { param($c) Edit-Script $c "(Lines[Count - 1] <> RecordEnd) or " '' } 'staged install lacks'
    Expect-Failure 'whose record is read though of another kind' { param($c) Edit-Script $c "  if (Lines[1] <> RecordKind) or " '  if ' } 'staged install lacks'
    Expect-Failure 'whose record is read with an ending not its own' { param($c) Edit-Script $c "  if not OwnEnding(Suffix) then`r`n    Exit;`r`n  for I := 3" "  for I := 3" } 'staged install lacks'
    Expect-Failure 'whose record is read with a line of another kind' { param($c) Edit-Script $c "      Copies.Add(Copy(Line, 6, Length(Line)));`r`n    end else`r`n      Exit;" "      Copies.Add(Copy(Line, 6, Length(Line)));`r`n    end;" } 'staged install lacks'
    Expect-Failure 'whose record takes any ending for its own' { param($c) Edit-Script $c "  Result := (Suffix <> '') and (Pos('|', Suffix) = 0) and (Pos('|' + Suffix + '|', SetAsideEndings) > 0);" "  Result := Suffix <> '';" } 'staged install lacks'
    Expect-Failure 'whose endings are others than the program reads' { param($c) Edit-Script $c "|.9.old-update|';" "|.10.old-update|';" } 'record of what it sets aside is not the one the program reads'
    Expect-Failure 'whose record is of another kind than the program reads' { param($c) Edit-Script $c "  RecordKind = 'qKVM Desktop set-aside record 1';" "  RecordKind = 'qKVM Desktop set-aside record 2';" } 'record of what it sets aside is not the one the program reads'
    Expect-Failure 'that removes what was set aside by an ending of its own choosing' { param($c) Edit-Script $c "      Aside := AddBackslash(Folder) + Names[I] + Endings[I];`r`n      // (One that cannot be looked at is not taken for gone: it stays named, review 1.5.)`r`n      if not LookAt(Aside, There) then begin" "      Aside := AddBackslash(Folder) + Names[I] + AsideEnding;`r`n      // (One that cannot be looked at is not taken for gone: it stays named, review 1.5.)`r`n      if not LookAt(Aside, There) then begin" } 'staged install lacks'
    Expect-Failure 'that puts back by an ending of its own choosing' { param($c) Edit-Script $c "      Aside := Base + Endings[I];" "      Aside := Base + AsideEnding;" } 'staged install lacks'
    Expect-Failure 'that puts back what an earlier update could not remove' { param($c) Edit-Script $c "(Result = '') and (Kinds[I] = AsideKind) and (Names[I] <> 'qkvm-desktop.exe') and There" "(Result = '') and (Names[I] <> 'qkvm-desktop.exe') and There" } 'staged install lacks'
    Expect-Failure 'that sets aside with an ending a file of the package''s has beside it' { param($c) Edit-Script $c "          Taken := True;`r`n      if not Taken then" "          Taken := False;`r`n      if not Taken then" } 'staged install lacks'
    Expect-Failure 'that looks for files by the ending of what was set aside' { param($c) Edit-Script $c "procedure PutProgramRight(Folder: String);`r`nvar" "procedure LookByEnding(Folder: String);`r`nvar`r`n  Found: TFindRec;`r`nbegin`r`n  if FindFirst(AddBackslash(Folder) + '*' + AsideEnding, Found) then`r`n    FindClose(Found);`r`nend;`r`n`r`nprocedure PutProgramRight(Folder: String);`r`nvar" } 'looks for files by the ending'
    Expect-Failure 'whose record is not on the disk before anything is renamed' { param($c) Edit-Script $c "  Result := FlushFileBuffers(Handle) <> 0;" "  Result := True;" } 'staged install lacks'
    Expect-Failure 'whose record is the marker before it is whole' { param($c) Edit-Script $c "  Draft := InApp(RecordDraft);" "  Draft := InApp(UnfinishedMarker);" } 'staged install lacks'
    # (L1 of review i020-2: the marker held, let go of before it is renamed or put right; written again where it is gone.)
    Expect-Failure 'that never holds its marker' { param($c) Edit-Script $c "  MarkerHold := CreateFileW(InApp(UnfinishedMarker), GenericRead, FileShareReadWrite, 0, OpenExisting, 0, 0);`r`n  Result := GetFileType" "  Result := GetFileType" } 'staged install lacks'
    Expect-Failure 'that holds its marker so that it may be removed' { param($c) Edit-Script $c "  MarkerHold := CreateFileW(InApp(UnfinishedMarker), GenericRead, FileShareReadWrite, 0, OpenExisting, 0, 0);" "  MarkerHold := CreateFileW(InApp(UnfinishedMarker), GenericRead, FileShareAll, 0, OpenExisting, 0, 0);" } 'staged install lacks'
    Expect-Failure 'that renames its marker while it holds it' { param($c) Edit-Script $c "  LetGoOfMarker;`r`n  Result := RenameFile(InApp(UnfinishedMarker), InApp(FinishingMarker));" "  Result := RenameFile(InApp(UnfinishedMarker), InApp(FinishingMarker));" } 'staged install lacks'
    Expect-Failure 'that puts right while it holds its marker' { param($c) Edit-Script $c "  // (This setup's hold of its own marker let go of first: what it puts right may remove or rename it, L1.)`r`n  LetGoOfMarker;`r`n" '' } 'staged install lacks'
    Expect-Failure 'that takes a whole copy whose marker went for one to put back' { param($c) Edit-Script $c "  if not Result and not ThingAt(InApp(UnfinishedMarker)) and (RecordText <> '') then begin" "  if False then begin" } 'staged install lacks'
    Expect-Failure 'that waits for a marker that went' { param($c) Edit-Script $c "  while not Result and (Tries < 3) and ThingAt(InApp(UnfinishedMarker)) do begin" "  while not Result and (Tries < 3) do begin" } 'staged install lacks'
    Expect-Failure 'that carries nothing an earlier update could not remove' { param($c) Edit-Script $c "        Text := Text + 'left ' + CarriedEndings[I] + ' ' + Carried[I] + #13#10;`r`n" '' } 'staged install lacks'
    Expect-Failure 'that sets aside though a marker it could not take away is there' { param($c) Edit-Script $c "  if ThingAt(InApp(UnfinishedMarker)) then begin`r`n    Result := 'the marker of an earlier update could not be removed (' + UnfinishedMarker + ')';`r`n    Exit;`r`n  end;`r`n" '' } 'staged install lacks'
    Expect-Failure 'that does not say why it does not begin where the program shows it' { param($c) Edit-Script $c "    Log(UnchangedLine('an update that did not finish left what this setup cannot put right: ' + Reason));`r`n" '' } 'does not put right what an update that did not finish left first'
    # (M1 and L13: a copy counts only where whole, on the disk, and marked finished; else the earlier version is back.)
    Expect-Failure 'that counts a copy that skipped a file as whole' { param($c) Edit-Script $c "    NotCounted := NotCopied;" "    NotCounted := '';" } 'staged install lacks'
    Expect-Failure 'that takes a file of other bytes for the package''s' { param($c) Edit-Script $c "(Found.SizeHigh = 0) and (IntToStr(Found.SizeLow) = Bytes[I])" 'True' } 'staged install lacks'
    Expect-Failure 'that takes a folder for a file of the package''s' { param($c) Edit-Script $c "          Sized := ((Found.Attributes and (DirectoryAttribute or ReparsePointAttribute)) = 0) and (Found.SizeHigh = 0)" "          Sized := (Found.SizeHigh = 0)" } 'staged install lacks'
    Expect-Failure 'that does not look at the marker of the contract after the copy' { param($c) Edit-Script $c "    Names.Add('qkvm-desktop.installed');`r`n    Bytes.Add('6');`r`n" '' } 'staged install lacks'
    Expect-Failure 'that removes what was set aside though the copy does not count' { param($c) Edit-Script $c "    if NotCounted <> '' then begin`r`n      Finished := False;`r`n      DoNotCount;" "    if False then begin`r`n      Finished := False;`r`n      DoNotCount;" } 'staged install lacks'
    Expect-Failure 'that leaves the earlier version set aside where the copy does not count' { param($c) Edit-Script $c "  if SetAside then begin`r`n    if PutPackageBack then begin" "  if SetAside then begin`r`n    if True then begin" } 'staged install lacks'
    Expect-Failure 'that ends with 0 where the copy does not count' { param($c) Edit-Script $c "  if NotCounted <> '' then`r`n    Result := 7;" "  if NotCounted <> '' then`r`n    Result := 0;" } 'staged install lacks'
    Expect-Failure 'that counts a copy not marked finished' { param($c) Edit-Script $c "        if not Finished then`r`n          NotCounted :=" "        if False then`r`n          NotCounted :=" } 'staged install lacks'
    Expect-Failure 'that marks a copy finished before it is on the disk' { param($c) Edit-Script $c "      NotCounted := FlushCopy;`r`n      // (A start made as the copy was flushed" "      // (A start made as the copy was flushed" } 'staged install lacks'
    # (Review 1.6: a file it could not flush is not on the disk, and the copy does not count; tried again for 5 s.)
    Expect-Failure 'that tries a file it could not flush once' { param($c) Edit-Script $c '  FlushTries = 20;' '  FlushTries = 1;' } 'staged install lacks'
    Expect-Failure 'that takes a file it could not flush for one on the disk' { param($c) Edit-Script $c "        if not Flushed then`r`n          Result := Names[I];" "        if False then`r`n          Result := Names[I];" } 'staged install lacks'
    Expect-Failure 'that removes what it set aside though the copy is not on the disk' { param($c) Edit-Script $c "      else if NotCounted <> '' then`r`n        NotCounted := 'the copy could not all be written to the disk (' + NotCounted + ')'`r`n      else begin" "      else begin" } 'staged install lacks'
    # (Review 1.7: a first install whose copy does not count starts nothing, and offers nothing to start.)
    Expect-Failure 'that starts a copy that does not count' { param($c) Edit-Script $c "  Result := (NotCounted = '') or EarlierBack;" "  Result := True;" } 'closing of a running copy lacks'
    Expect-Failure 'that ticks Start for a first install that does not count' { param($c) Edit-Script $c "  Result := MayStartProgram and CopyStarts and not UpdateMode and ((InstalledVersion = '') or ClosedIt);" "  Result := MayStartProgram and not UpdateMode and ((InstalledVersion = '') or ClosedIt);" } 'closing of a running copy lacks'
    Expect-Failure 'that offers Start for a first install that does not count' { param($c) Edit-Script $c "  Result := MayStartProgram and CopyStarts and not UpdateMode and not StartTicked;" "  Result := MayStartProgram and not UpdateMode and not StartTicked;" } 'closing of a running copy lacks'
    Expect-Failure 'that starts again what does not count' { param($c) Edit-Script $c "  Result := MayStartProgram and CopyStarts and (UpdateMode or (WizardSilent and ClosedIt));" "  Result := MayStartProgram and (UpdateMode or (WizardSilent and ClosedIt));" } 'closing of a running copy lacks'
    Expect-Failure 'whose finishing removes the one copy of a file gone since' { param($c) Edit-Script $c "          if (Kinds[I] = LeftKind) or PlainFileAt(AddBackslash(Folder) + Names[I]) then" "          if True then" } 'staged install lacks'
    Expect-Failure 'that says nothing where the copy does not count' { param($c) Edit-Script $c "      Log(UnchangedLine(Why));`r`n" '' } 'staged install lacks'
    # (L9 of review i020-2: never "nothing was changed" after what was put right first; each way of putting right said;
    # this setup's own undoing not taken for it.)
    Expect-Failure 'that says nothing was changed after it put right' { param($c) Edit-Script $c "  if PutRightDone = '' then`r`n    Result := 'qKVM-RESULT: 7 nothing was changed: ' + Reason" "  if True then`r`n    Result := 'qKVM-RESULT: 7 nothing was changed: ' + Reason" } 'removal after the copy lacks'
    Expect-Failure 'that says nothing was changed on the page after it put right' { param($c) Edit-Script $c "    if PutRightDone = '' then`r`n      Result := 'Nothing was changed: ' + Reason + '.'" "    if True then`r`n      Result := 'Nothing was changed: ' + Reason + '.'" } 'removal after the copy lacks'
    Expect-Failure 'that says nothing of a put-back in part' { param($c) Edit-Script $c "        SayPutRight('put back part of the earlier version, which an update that did not finish had set aside');" "        Log('');" } 'removal after the copy lacks'
    Expect-Failure 'that says nothing of 0.1.2''s program put back' { param($c) Edit-Script $c "    SayPutRight('put back the earlier program that a setup of 0.1.2 had set aside');" "    Log('');" } 'removal after the copy lacks'
    Expect-Failure 'that says nothing of 0.1.2''s program removed' { param($c) Edit-Script $c "      SayPutRight('removed the earlier program that a setup of 0.1.2 had set aside, as a program stood in its place');" "      Log('');" } 'removal after the copy lacks'
    Expect-Failure 'that says nothing of what a finishing update had set aside, removed' { param($c) Edit-Script $c "  if Removed > 0 then`r`n    SayPutRight(" "  if False then`r`n    SayPutRight(" } 'removal after the copy lacks'
    Expect-Failure 'that takes its own set-aside undone for what it put right' { param($c) Edit-Script $c "    Log('What was set aside for the copy could not all be put back yet.');`r`n  PutRightDone := Done;" "    Log('What was set aside for the copy could not all be put back yet.');" } 'removal after the copy lacks'
    Expect-Failure 'that takes its own copy undone for what it put right' { param($c) Edit-Script $c "    if PutPackageBack then begin`r`n      PutRightDone := Done;" "    if PutPackageBack then begin" } 'removal after the copy lacks'
    Expect-Failure 'that says the last put right alone' { param($c) Edit-Script $c "    PutRightDone := PutRightDone + ', and ' + What;" "    PutRightDone := What;" } 'removal after the copy lacks'
    Expect-Failure 'whose list of bytes is not the package''s' { param($c) $k = Join-Path $c 'inputs/package.iss'; [IO.File]::WriteAllText($k, [IO.File]::ReadAllText($k).Replace("function PackageFileBytesX64: String;`r`nbegin`r`n  Result := '|';`r`n  Result := Result + '", "function PackageFileBytesX64: String;`r`nbegin`r`n  Result := '|';`r`n  Result := Result + '9")) } 'list of the bytes of its files'
    Expect-Failure 'that writes the registry otherwise' { param($c) Edit-Script $c "    Installing := True;" "    Installing := True;`r`n    RegWriteStringValue(HKEY_CURRENT_USER, 'Software\qKVM', 'x', 'y');" } 'uses RegWrite'
    # (M2 and L2: what the setup removes of its own made writable first and tried again; never a block; the uninstall
    # keeps a marker, as a finishing one, where what it names could not go.)
    Expect-Failure 'that removes what it set aside with its read-only attribute on' { param($c) Edit-Script $c "      SetFileAttributesW(Path, Attributes and not ReadOnlyAttribute);" "      Log('');" } 'staged install lacks'
    Expect-Failure 'that removes what it set aside once only' { param($c) Edit-Script $c "  while not Result and (Tries < 3) do begin`r`n    Sleep(250);" "  while False do begin`r`n    Sleep(250);" } 'staged install lacks'
    Expect-Failure 'whose finishing deletes without making it writable' { param($c) Edit-Script $c "              if RemoveOwn(Aside) then begin" "              if DeleteFile(Aside) then begin" } 'staged install lacks'
    Expect-Failure 'that puts back over a read-only file of the copy''s' { param($c) Edit-Script $c "            if not RemoveOwn(Base) then" "            if not DeleteFile(Base) then" } 'staged install lacks'
    Expect-Failure 'whose uninstaller removes the marker of what it could not remove' { param($c) Edit-Script $c "        if Left = 0 then`r`n          RemoveOwn(Markers[M])" "        if True then`r`n          RemoveOwn(Markers[M])" } 'staged install lacks'
    Expect-Failure 'whose uninstaller keeps an unfinished marker to be put back' { param($c) Edit-Script $c "              RenameFile(Markers[1], Markers[0]);" "              Log('');" } 'staged install lacks'
    # (M3 and L3: the update check in the program's place while the earlier version is set aside; the put-back's order;
    # what the cut copy made removed.)
    Expect-Failure 'whose update check is not carried' { param($c) Edit-Script $c "Source: `"{#SourcePath}qkvm-update-check.exe`"; Flags: dontcopy noencryption`r`n" '' } 'update check carried'
    Expect-Failure 'whose update check is copied as a file of the program''s' { param($c) Edit-Script $c 'Source: "{#SourcePath}qkvm-update-check.exe"; Flags: dontcopy noencryption' 'Source: "{#SourcePath}qkvm-update-check.exe"; DestDir: "{app}"; Flags: ignoreversion' } 'update check carried'
    Expect-Failure 'that leaves no program in the program''s place while the earlier version is set aside' { param($c) Edit-Script $c "          if (Result = '') and (I = 0) then`r`n            if not PlaceUpdateCheck(ProgramPath) then" "          if False then`r`n            if not PlaceUpdateCheck(ProgramPath) then" } 'staged install lacks'
    Expect-Failure 'that puts the update check over what is at the program''s name' { param($c) Edit-Script $c "      Result := RenameFile(Draft, Path);" "      Result := FileCopy(Draft, Path, False);" } 'staged install lacks'
    # (L4 and L3 of review i020-2: placed by a draft, flushed, renamed; taken away where the earlier program is gone.)
    Expect-Failure 'that copies the update check straight to the program''s name' { param($c) Edit-Script $c "  if CopyFile(ExpandConstant('{tmp}\') + UpdateCheck, Draft, True) then`r`n    if FlushedFile(Draft) then`r`n      Result := RenameFile(Draft, Path);" "  if CopyFile(ExpandConstant('{tmp}\') + UpdateCheck, Path, True) then`r`n    Result := True;" } 'staged install lacks'
    Expect-Failure 'that renames the update check into place before it is on the disk' { param($c) Edit-Script $c "    if FlushedFile(Draft) then`r`n      Result := RenameFile(Draft, Path);" "    Result := RenameFile(Draft, Path);" } 'staged install lacks'
    Expect-Failure 'that leaves the draft of an update check it could not place' { param($c) Edit-Script $c "  if not Result then`r`n    if ThingAt(Draft) then`r`n      RemoveOwn(Draft);`r`nend;" "end;" } 'staged install lacks'
    Expect-Failure 'that places the update check over a draft left before' { param($c) Edit-Script $c "  if ThingAt(Draft) then`r`n    if not RemoveOwn(Draft) then`r`n      Exit;`r`n  if CopyFile(" "  if CopyFile(" } 'staged install lacks'
    Expect-Failure 'whose uninstaller leaves the draft of an update check' { param($c) Edit-Script $c "  if PlainFileAt(InApp(CheckDraft)) then`r`n    RemoveOwn(InApp(CheckDraft));`r`n" '' } 'staged install lacks'
    Expect-Failure 'that leaves the update check where the earlier program is gone' { param($c) Edit-Script $c "    if (Result = '') and ProgramGone and IsOwnUpdateCheck(ProgramFile) then" "    if False then" } 'staged install lacks'
    Expect-Failure 'that takes anything at the program''s name for its update check' { param($c) Edit-Script $c "      Result := GetSHA256OfFile(Path) = GetSHA256OfFile(ExpandConstant('{tmp}\') + UpdateCheck);" "      Result := True;" } 'staged install lacks'
    Expect-Failure 'that takes a program named set aside and gone for one there' { param($c) Edit-Script $c "    ProgramGone := (Result = '') and (ProgramAside <> '') and not ProgramSetAside and (not ProgramThere or IsOwnUpdateCheck(ProgramFile));" "    ProgramGone := False;" } 'staged install lacks'
    Expect-Failure 'that takes the earlier program put back already for one gone' { param($c) Edit-Script $c " and not ProgramSetAside and (not ProgramThere or IsOwnUpdateCheck(ProgramFile));" " and not ProgramSetAside;" } 'staged install lacks'
    Expect-Failure 'that says the earlier version is whole where its program was gone' { param($c) Edit-Script $c "  Result := (Reason = '') and not EarlierProgramGone;" "  Result := Reason = '';" } 'staged install lacks'
    Expect-Failure 'that says nothing of the earlier program gone where the copy does not count' { param($c) Edit-Script $c "    end else if EarlierProgramGone then`r`n      Why := Why + '; the earlier version is back, all but its program, which was gone since it was set aside'`r`n    else" "    end else" } 'staged install lacks'
    Expect-Failure 'that says it put the earlier version back where its program was gone' { param($c) Edit-Script $c "      SayPutRight('put back what an update that did not finish had set aside, all but the earlier program, which was gone');" "      SayPutRight('put back the earlier version, which an update that did not finish had set aside');" } 'staged install lacks'
    Expect-Failure 'that sets aside without its update check' { param($c) Edit-Script $c "      else if not UpdateCheckReady then`r`n        Result := 'the setup could not unpack its update check';" "      else if False then`r`n        Result := 'the setup could not unpack its update check';" } 'staged install lacks'
    Expect-Failure 'that leaves the copy''s program while the libraries come back' { param($c) Edit-Script $c "    if (Result = '') and (ProgramSetAside or ProgramCopied) then begin" "    if False then begin" } 'staged install lacks'
    Expect-Failure 'that puts the earlier program back among its libraries' { param($c) Edit-Script $c "(Kinds[I] = AsideKind) and (Names[I] <> 'qkvm-desktop.exe') and There" "(Kinds[I] = AsideKind) and There" } 'staged install lacks'
    Expect-Failure 'that leaves what the copy made beside the earlier version' { param($c) Edit-Script $c "(Copies[I] <> 'qkvm-desktop.exe') and (Pos('|' + Lowercase(Copies[I]) + '|', SetAsideNames) = 0) then begin" "False then begin" } 'staged install lacks'
    Expect-Failure 'that removes what the copy writes though it was set aside' { param($c) Edit-Script $c " and (Pos('|' + Lowercase(Copies[I]) + '|', SetAsideNames) = 0) then begin" " then begin" } 'staged install lacks'
    # (Review 1.5: a put-back is whole only where every file its record names was seen and is as it should be; what
    # cannot be looked at, or removed, stays named in a record that is still read.)
    Expect-Failure 'that takes a file set aside it cannot look at for one gone' { param($c) Edit-Script $c "      if (Result = '') and (Kinds[I] = AsideKind) and (Names[I] <> 'qkvm-desktop.exe') and not LookAt(Aside, There) then`r`n        Result := Format('what an update set aside could not be looked at (%s)'" "      if False then`r`n        Result := Format('what an update set aside could not be looked at (%s)'" } 'staged install lacks'
    Expect-Failure 'that takes the program set aside it cannot look at for one gone' { param($c) Edit-Script $c "    if (ProgramAside <> '') and not LookAt(ProgramAside, ProgramSetAside) then`r`n      Result := 'what an update set aside could not be looked at" "    if (ProgramAside <> '') and LookAt(ProgramAside, ProgramSetAside) and False then`r`n      Result := 'what an update set aside could not be looked at" } 'staged install lacks'
    Expect-Failure 'that takes what the copy made and it cannot look at for gone' { param($c) Edit-Script $c "        if not LookAt(Base, There) then`r`n          Result := Format('what a copy that did not complete made could not be looked at" "        if False then`r`n          Result := Format('what a copy that did not complete made could not be looked at" } 'staged install lacks'
    Expect-Failure 'that says the earlier version is whole beside what the copy made' { param($c) Edit-Script $c "            Result := Format('what a copy that did not complete made could not be removed (%s)', [Copies[I]]);" "            Log('');" } 'staged install lacks'
    Expect-Failure 'whose finishing takes what it cannot look at for gone' { param($c) Edit-Script $c "      if not LookAt(Aside, There) then begin`r`n        Left := Left + 1;`r`n        Log('Could not look at what an update set aside: '" "      if False then begin`r`n        Left := Left + 1;`r`n        Log('Could not look at what an update set aside: '" } 'staged install lacks'
    Expect-Failure 'whose uninstall takes what it cannot look at for gone' { param($c) Edit-Script $c "          if not LookAt(Aside, There) then`r`n            Left := Left + 1`r`n          else if PlainFileAt(Aside) then" "          if False then`r`n            Left := Left + 1`r`n          else if PlainFileAt(Aside) then" } 'staged install lacks'
    Expect-Failure 'that takes a look it was refused for one of a file not there' { param($c) Edit-Script $c "    Result := (Error = ErrorFileNotFound) or (Error = ErrorPathNotFound);`r`n  end;`r`nend;`r`n`r`n// Whether what is at Path is a plain file" "    Result := True;`r`n  end;`r`nend;`r`n`r`n// Whether what is at Path is a plain file" } 'staged install lacks'
    Expect-Failure 'that records nothing of what the copy writes' { param($c) Edit-Script $c "        Text := Text + 'copy ' + Names[I] + #13#10;`r`n" '' } 'staged install lacks'
    Expect-Failure 'that takes what the copy writes for what it set aside' { param($c) Edit-Script $c "      Copies.Add(Copy(Line, 6, Length(Line)));" "      Names.Add(Copy(Line, 6, Length(Line)));" } 'staged install lacks'
    Expect-Failure 'whose uninstaller leaves what a copy that did not finish made' { param($c) Edit-Script $c "                RemoveOwn(AddBackslash(Folder) + Copies[I]);" "                Log('');" } 'staged install lacks'
    Expect-Failure 'whose uninstaller takes no install of ours to be in its folder' { param($c) Edit-Script $c "  DropRecorded(ExpandConstant('{app}'), True);" "  DropRecorded(ExpandConstant('{app}'), False);" } 'staged install lacks'
    Expect-Failure 'whose uninstaller removes no record''s files' { param($c) Edit-Script $c "      if ReadRecord(Markers[M], Suffix, Names, Endings, Kinds, Copies) then" "      if False then" } 'staged install lacks'
    Expect-Failure 'whose uninstaller leaves what a setup ended halfway left' { param($c) Edit-Script $c "    RemoveSetupLeftovers;`r`n    DropSetAside;" '    DropSetAside;' } 'removal after the copy lacks'
    Expect-Failure 'whose uninstaller leaves what an update set aside' { param($c) Edit-Script $c "    RemoveSetupLeftovers;`r`n    DropSetAside;" '    RemoveSetupLeftovers;' } 'removal after the copy lacks'
    # (The earlier version set aside for the copy, 0.2.0: each protection taken away, alone.)
    Expect-Failure 'whose list sets aside a file the package does not have' { param($c) $k = Join-Path $c 'inputs/package.iss'; [IO.File]::WriteAllText($k, [IO.File]::ReadAllText($k).Replace("  Result := '|';", "  Result := '|';`n  Result := Result + 'stale.dll|';")) } 'list of what it sets aside'
    Expect-Failure 'whose list sets aside less than the package' { param($c) $k = Join-Path $c 'inputs/package.iss'; [IO.File]::WriteAllText($k, (@([IO.File]::ReadAllLines($k)) | Where-Object { $_ -notmatch 'qwindows' }) -join "`n") } 'list of what it sets aside'
    Expect-Failure 'whose list sets the program aside last' { param($c) $k = Join-Path $c 'inputs/package.iss'; $lines = @([IO.File]::ReadAllLines($k)); $exe = @($lines | Where-Object { $_ -match "'qkvm-desktop\.exe\|'" }); [IO.File]::WriteAllText($k, ((@($lines | Where-Object { $_ -notmatch "'qkvm-desktop\.exe\|'" -and $_ -ne 'end;' }) + $exe + 'end;') -join "`n")) } 'list of what it sets aside'
    Expect-Failure 'whose list of files does more than say them' { param($c) $k = Join-Path $c 'inputs/package.iss'; [IO.File]::AppendAllText($k, "procedure Other;`nbegin`nend;`n") } 'package.iss is not the three functions'
    # (L7 of review i020-2: a whole copy by the SHA-256 of each file, the list of them the package's, the marker's own.)
    Expect-Failure 'whose list of SHA-256 is not the package''s' { param($c) $k = Join-Path $c 'inputs/package.iss'; $t = [IO.File]::ReadAllText($k); $at = $t.IndexOf('function PackageFileSha256'); $sum = [regex]::Match($t.Substring($at), "'([0-9a-f]{64})\|'").Groups[1].Value; [IO.File]::WriteAllText($k, $t.Substring(0, $at) + $t.Substring($at).Replace($sum, '0' * 64)) } 'list of the SHA-256 of its files'
    Expect-Failure 'that takes a file of the right size and other bytes for the package''s' { param($c) Edit-Script $c "          if Sized and SameBytes(InApp(Names[I]), Sums[I]) then" "          if Sized then" } 'staged install lacks'
    Expect-Failure 'that takes a file it cannot read for the package''s' { param($c) Edit-Script $c "      Log('A file of the copy could not be read, to tell that it is whole: ' + Path + ' (' + GetExceptionMessage + ')');" "      Result := True;" } 'staged install lacks'
    Expect-Failure 'that takes any marker of six bytes for the contract''s after the copy' { param($c) Edit-Script $c "    Sums.Add(MarkerSha256);" "    Sums.Add(GetSHA256OfFile(InApp('qkvm-desktop.installed')));" } 'staged install lacks'
    Expect-Failure 'whose SHA-256 of the marker is another' { param($c) Edit-Script $c "  MarkerSha256 = '9752eb62a6845a78a9e2eaeb4a6eb2d93a1b654ee8665f71d516cfaca3e7cf57';" "  MarkerSha256 = '0000000000000000000000000000000000000000000000000000000000000000';" } 'staged install lacks'
    Expect-Failure 'with another marker than the program looks for' { param($c) Edit-Script $c "  UnfinishedMarker = 'qkvm-desktop.update-unfinished';" "  UnfinishedMarker = 'qkvm-desktop.updating';" } 'marker of an unfinished update'
    Expect-Failure 'that sets aside under another ending than the program looks for' { param($c) Edit-Script $c "  AsideEnding = '.old-update';" "  AsideEnding = '.old';" } 'marker of an unfinished update'
    Expect-Failure 'whose finishing marker is another' { param($c) Edit-Script $c "  FinishingMarker = 'qkvm-desktop.update-finishing';" "  FinishingMarker = 'qkvm-desktop.update-unfinished';" } 'marker of an unfinished update'
    Expect-Failure 'that sets aside before it puts right what an update left' { param($c) Edit-Script $c "  Result := PutRight(ExpandConstant('{app}'));`r`n  if Result <> '' then begin`r`n    Result := 'what an update that did not finish left could not be put right: ' + Result;`r`n    Exit;`r`n  end;`r`n  // (A marker PutRight" "  Result := '';`r`n  // (A marker PutRight" } 'staged install lacks'
    Expect-Failure 'that writes its marker on a first install' { param($c) Edit-Script $c "    if (Result = '') and (Count > 0) then begin`r`n" "    if Result = '' then begin`r`n" } 'staged install lacks'
    Expect-Failure 'that sets aside without its record' { param($c) Edit-Script $c "      Result := WriteRecord(Text, UnfinishedMarker);" "      Result := '';" } 'does not write the marker of an unfinished update'
    Expect-Failure 'that sets aside what is not a plain file' { param($c) Edit-Script $c "        if PlainFileAt(Path) then`r`n          Count := Count + 1" "        if True then`r`n          Count := Count + 1" } 'staged install lacks'
    Expect-Failure 'that sets aside though a refusal came before its marker' { param($c) Edit-Script $c "    if (Result = '') and (Count > 0) then begin`r`n" "    if Count > 0 then begin`r`n" } 'staged install lacks'
    Expect-Failure 'that leaves set aside what it renamed before a refusal' { param($c) Edit-Script $c "  if PutRight(ExpandConstant('{app}')) = '' then`r`n    SetAside := False" "  if True then`r`n    SetAside := False" } 'staged install lacks'
    Expect-Failure 'that removes what it set aside though the copy was not marked whole' { param($c) Edit-Script $c "  if not Result then begin`r`n    Log('The copy is whole, but" "  if False then begin`r`n    Log('The copy is whole, but" } 'staged install lacks'
    Expect-Failure 'that removes what it set aside after the earlier version''s files' { param($c) Edit-Script $c "    Finished := True;`r`n    NotCounted := NotCopied;" "    Finished := True;`r`n    RemoveEarlierFiles(EarlierHere);`r`n    NotCounted := NotCopied;" } 'staged install lacks'
    Expect-Failure 'that marks the copy whole without renaming its marker' { param($c) Edit-Script $c "  Result := RenameFile(InApp(UnfinishedMarker), InApp(FinishingMarker));" "  Result := True;" } 'staged install lacks'
    Expect-Failure 'that puts the earlier version back once only' { param($c) Edit-Script $c "  while (Reason <> '') and (Tries < PutBackTries) do begin" "  while False do begin" } 'staged install lacks'
    Expect-Failure 'that puts back over a file of the copy''s' { param($c) Edit-Script $c "          if ThingAt(Base) then`r`n            if not RemoveOwn(Base) then" "          if False then`r`n            if not RemoveOwn(Base) then" } 'staged install lacks'
    Expect-Failure 'that removes what an unfinished update set aside' { param($c) Edit-Script $c "    Result := PutBackRecorded(Folder);" "    Result := FinishRecorded(Folder);" } 'staged install lacks'
    Expect-Failure 'that puts the program back before its libraries' { param($c) Edit-Script $c "    for I := Names.Count - 1 downto 0 do begin" "    for I := 0 to Names.Count - 1 do begin" } 'staged install lacks'
    Expect-Failure 'that takes its marker away before all is right' { param($c) Edit-Script $c "      Exit;`r`n    end;`r`n    if ProgramGone then begin" "    end;`r`n    if ProgramGone then begin" } 'staged install lacks'
    Expect-Failure 'that removes the record of what it could not remove' { param($c) Edit-Script $c "  if Left > 0 then`r`n    Log(Format('What an update set aside could not all be removed" "  if False then`r`n    Log(Format('What an update set aside could not all be removed" } 'staged install lacks'
    Expect-Failure 'that puts right through a redirection' { param($c) Edit-Script $c " or ThroughRedirection(Folder, RemoveBackslash(ExtractFilePath(Names[I]))) then" " then" } 'staged install lacks'
    Expect-Failure 'that leaves the program 0.1.2 set aside' { param($c) Edit-Script $c "FinishingMarker) then begin`r`n    PutProgramRight(Folder);" "FinishingMarker) then begin`r`n    Log('');" } 'staged install lacks'
    Expect-Failure 'whose uninstaller leaves the marker' { param($c) Edit-Script $c "        if Left = 0 then`r`n          RemoveOwn(Markers[M])" "        if Left = 0 then`r`n          Log('')" } 'staged install lacks'
    Expect-Failure 'whose uninstaller follows a redirection to what was set aside' { param($c) Edit-Script $c "            if not ThroughRedirection(Folder, RemoveBackslash(ExtractFilePath(Names[I]))) then begin`r`n              if RemoveOwn(Aside) then" "            if True then begin`r`n              if RemoveOwn(Aside) then" } 'staged install lacks'
    Expect-Failure 'that writes another file' { param($c) Edit-Script $c "    Installing := True;" "    Installing := True;`r`n    SaveStringToFile(ExpandConstant('{app}\x.txt'), 'x', False);" } 'uses SaveStringToFile'
    # (First of all at its start, before its refusal of an older version, and in a folder told without {app}.)
    Expect-Failure 'that puts right nothing at its start' { param($c) Edit-Script $c "    Reason := PutRight(FolderToPutRight);`r`n" "    Reason := '';`r`n" } 'does not put right what an update that did not finish left first'
    Expect-Failure 'that goes on though it could not put right at its start' { param($c) Edit-Script $c "+ Reason + Rest, mbError, MB_OK);`r`n    Result := False;" "+ Reason + Rest, mbError, MB_OK);`r`n    Result := True;" } 'does not put right what an update that did not finish left first'
    Expect-Failure 'that puts right in {app} at its start' { param($c) Edit-Script $c "    Reason := PutRight(FolderToPutRight);" "    Reason := PutRight(ExpandConstant('{app}'));" } 'expands \{app\} before Inno Setup knows it: InitializeSetup'
    Expect-Failure 'that puts right at its start through what expands {app}' { param($c) Edit-Script $c "  Base := AddBackslash(Folder) + 'qkvm-desktop.exe';" "  Base := ExpandConstant('{app}\qkvm-desktop.exe');" } 'expands \{app\} before Inno Setup knows it: InitializeSetup -> PutRight -> PutProgramRight'
    # (Outside review 2, 1.3: the folder's rights judged at the setup's start, before anything there is put right or looked
    # for, and of the folder it would put right, without {app}.)
    Expect-Failure 'that puts right before it judges the folder''s rights' { param($c) Edit-Script $c "  Reason := FolderRightsReason(FolderToPutRight);`r`n  if Reason <> '' then begin`r`n    Log('Nothing that an update left is put right, nor looked for: ' + Reason + '. Preparing to Install refuses such a folder.');`r`n    Reason := '';`r`n  end else`r`n    Reason := PutRight(FolderToPutRight);" "  Reason := PutRight(FolderToPutRight);" } 'does not put right what an update that did not finish left first'
    Expect-Failure 'that puts right in a folder another account may change' { param($c) Edit-Script $c "  end else`r`n    Reason := PutRight(FolderToPutRight);" "  end;`r`n  Reason := PutRight(FolderToPutRight);" } 'does not put right what an update that did not finish left first'
    Expect-Failure 'that judges another folder at its start than it would put right' { param($c) Edit-Script $c "  Reason := FolderRightsReason(FolderToPutRight);" "  Reason := FolderRightsReason(InstalledFolder);" } 'does not put right what an update that did not finish left first'
    Expect-Failure 'that judges the folder at its start through what expands {app}' { param($c) Edit-Script $c "  Path := RemoveBackslash(Base);" "  Path := ExpandConstant('{app}');" } 'expands \{app\} before Inno Setup knows it: InitializeSetup -> FolderRightsReason'
    Expect-Failure 'that takes a DIR of no whole path at its start' { param($c) Edit-Script $c "  if (Copy(Result, 2, 2) <> ':\') and (Copy(Result, 1, 2) <> '\\') then" "  if Result = '' then" } 'staged install lacks'
    Expect-Failure 'whose uninstall step is never called' { param($c) Edit-Script $c 'procedure CurUninstallStepChanged(' 'procedure CurUninstallStepChange(' } 'removal after the copy lacks'
    Expect-Failure 'that takes any .tmp for what a setup left' { param($c) Edit-Script $c '(Length(Name) = 17) and ' '' } 'removal after the copy lacks'
    Expect-Failure 'that takes any letters for what a setup left' { param($c) Edit-Script $c '  for I := 4 to 13 do' '  for I := 4 to 4 do' } 'removal after the copy lacks'
    Expect-Failure 'that follows a redirection for what a setup left' { param($c) Edit-Script $c "  if ThroughRedirection(Base, Relative) then begin" '  if False then begin' } 'removal after the copy lacks'
    Expect-Failure 'that looks through a redirected folder on the way' { param($c) Edit-Script $c "  while (Rest <> '') and not Result do begin" '  while False do begin' } 'removal after the copy lacks'
    Expect-Failure 'that looks at no folder on the way' { param($c) Edit-Script $c '  Rest := Relative;' "  Rest := '';" } 'removal after the copy lacks'
    Expect-Failure 'that looks at another path on the way' { param($c) Edit-Script $c "    Path := Path + '\' + Copy(Rest, 1, At - 1);" "    Path := ExpandConstant('{userdocs}');" } 'removal after the copy lacks'
    Expect-Failure 'that removes more than what a setup left' { param($c) Edit-Script $c '= 0) and SetupLeftover(Found.Name) then begin' '= 0) then begin' } 'removal after the copy lacks'
    Expect-Failure 'that looks only in the first of its folders' { param($c) Edit-Script $c '  while At > 0 do begin' '  if At > 0 then begin' } 'removal after the copy lacks'
    Expect-Failure 'that stops at the first leftover' { param($c) Edit-Script $c "+ Entry);`r`n      end;`r`n    until not FindNext(Found);" "+ Entry);`r`n      end;`r`n    until True;" } 'removal after the copy lacks'
    Expect-Failure 'whose install step is never called' { param($c) Edit-Script $c 'procedure CurStepChanged(' 'procedure CurStepChange(' } 'removal after the copy lacks'
    Expect-Failure 'whose list of folders lacks one of the package''s' { param($c) $k = Join-Path $c 'inputs/folders.iss'; [IO.File]::WriteAllText($k, (@([IO.File]::ReadAllLines($k)) | Where-Object { $_ -notmatch 'platforms' }) -join "`n") } 'list of the package''s folders'
    Expect-Failure 'whose list of folders has one the package has not' { param($c) $k = Join-Path $c 'inputs/folders.iss'; [IO.File]::WriteAllText($k, [IO.File]::ReadAllText($k).Replace("  Result := '|';", "  Result := '|';`n  Result := Result + '..|';")) } 'list of the package''s folders'
    # (Windows on Arm, 0.2.0: each kind's own files, lists and update check, chosen by the PC; the kind noted.)
    Expect-Failure 'that puts x64''s files in on Arm' { param($c) $f = Join-Path $c 'inputs/files.iss'; [IO.File]::WriteAllText($f, [IO.File]::ReadAllText($f).Replace('Source: "{#Arm64PackageDir}\Qt6Core.dll"; DestDir: "{app}"; Flags: ignoreversion; Check: IsArm64', 'Source: "{#Arm64PackageDir}\Qt6Core.dll"; DestDir: "{app}"; Flags: ignoreversion; Check: not IsArm64')) } 'one kind of Windows in for another'
    Expect-Failure 'that puts a file in on every PC' { param($c) $f = Join-Path $c 'inputs/files.iss'; [IO.File]::WriteAllText($f, [IO.File]::ReadAllText($f).Replace('Source: "Qt6Core.dll"; DestDir: "{app}"; Flags: ignoreversion; Check: not IsArm64', 'Source: "Qt6Core.dll"; DestDir: "{app}"; Flags: ignoreversion')) } 'not one file at its own place'
    Expect-Failure 'whose Arm program is not written last' { param($c) $l = Join-Path $c 'inputs/files.iss'; $lines = @([IO.File]::ReadAllLines($l)); [IO.File]::WriteAllLines($l, [string[]] (@($lines[0..($lines.Count - 3)]) + $lines[-1] + $lines[-2])) } 'program''s file is not the last'
    Expect-Failure 'whose lists on Arm are x64''s' { param($c) Edit-Script $c "    Result := PackageFilesArm64`r`n" "    Result := PackageFilesX64`r`n" } 'does not choose function PackageFiles'
    Expect-Failure 'whose earlier lists on Arm are x64''s' { param($c) Edit-Script $c "    Result := EarlierFilesOfArm64(Version, Kind)`r`n" "    Result := EarlierFilesOfX64(Version, Kind)`r`n" } 'does not choose function EarlierFilesOf'
    Expect-Failure 'whose update check on Arm is x64''s' { param($c) Edit-Script $c "    Result := 'qkvm-update-check-arm64.exe'`r`n" "    Result := 'qkvm-update-check.exe'`r`n" } 'does not choose function UpdateCheck'
    Expect-Failure 'that notes x64 on Arm' { param($c) Edit-Script $c "  if IsArm64 then`r`n    Result := 'arm64'`r`n" "  if IsArm64 then`r`n    Result := 'x64'`r`n" } 'does not choose function ThisKind'
    Expect-Failure 'that asks for the kind elsewhere' { param($c) Edit-Script $c "  if Version = '' then`r`n    Exit;`r`n  Names := TStringList.Create;" "  if (Version = '') or IsArm64 then`r`n    Exit;`r`n  Names := TStringList.Create;" } 'asks for the PC''s kind of Windows elsewhere'
    Expect-Failure 'that removes another kind''s files of an earlier version' { param($c) Edit-Script $c "      RemoveEarlierFiles(EarlierHere, EarlierKindHere);" "      RemoveEarlierFiles(EarlierHere, ThisKind);" } 'kind of Windows it installed'
    Expect-Failure 'that never notes the kind it installed' { param($c) Edit-Script $c "'qKVM Desktop Architecture', ThisKind) then" "'qKVM Desktop Kind', ThisKind) then" } 'kind of Windows it installed'
    Expect-Failure 'that takes every install before for x64''s' { param($c) Edit-Script $c "    EarlierKindHere := ImageKind(ProgramPath);" "    EarlierKindHere := 'x64';" } 'kind of Windows it installed'
    Expect-Failure 'that reads the kind back from its entry of Settings, Apps' { param($c) Edit-Script $c "    EarlierKindHere := ImageKind(ProgramPath);" "    RegQueryStringValue(HKEY_CURRENT_USER, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{A964EFA6-87C6-4A23-A9CC-B71F87441433}_is1', 'qKVM Desktop Architecture', EarlierKindHere);" } 'kind of Windows it installed'
    Expect-Failure 'that takes a program of any machine for Arm''s' { param($c) Edit-Script $c '        if Machine = $AA64 then' '        if Machine <> $8664 then' } 'kind of Windows it installed'
    Expect-Failure 'that takes a program of neither kind for x64''s' { param($c) Edit-Script $c "        else if Machine = `$8664 then`r`n          Result := 'x64';" "        else`r`n          Result := 'x64';" } 'kind of Windows it installed'
    Expect-Failure 'that reads the machine at another place' { param($c) Edit-Script $c '        Machine := Ord(Header[5]) + Ord(Header[6]) * 256;' '        Machine := Ord(Header[3]) + Ord(Header[4]) * 256;' } 'kind of Windows it installed'
    Expect-Failure 'that installs on Arm with x64''s package alone' { param($c) Edit-Script $c "#else`r`nArchitecturesAllowed=x64os" "#else`r`nArchitecturesAllowed=x64compatible" } 'x64os without it'
    Expect-Failure 'whose Arm update check is not carried' { param($c) Edit-Script $c "Source: `"{#SourcePath}qkvm-update-check-arm64.exe`"; Flags: dontcopy noencryption`r`n" '' } 'update check carried'
    Expect-Failure 'whose list of Arm''s folders lacks one' { param($c) $k = Join-Path $c 'inputs/folders.iss'; $t = [IO.File]::ReadAllText($k); $at = $t.IndexOf('function PackageFoldersArm64'); [IO.File]::WriteAllText($k, $t.Substring(0, $at) + $t.Substring($at).Replace("  Result := Result + 'plugins\platforms|';", '')) } 'package''s folders is not the package''s \(arm64'
    Expect-Failure 'whose list of what Arm sets aside is x64''s' { param($c) $k = Join-Path $c 'inputs/package.iss'; $t = [IO.File]::ReadAllText($k); $at = $t.IndexOf('function PackageFilesArm64'); [IO.File]::WriteAllText($k, $t.Substring(0, $at) + $t.Substring($at).Replace("  Result := Result + 'licenses\arm-only.txt|';", "  Result := Result + 'x64-only.dll|';")) } 'what it sets aside is not the package''s.*arm64'
    Expect-Failure 'whose Arm earlier list lacks x64''s own of this version' { param($c) $k = Join-Path $c 'inputs/earlier.iss'; $t = [IO.File]::ReadAllText($k); $at = $t.IndexOf('function EarlierFilesOfArm64'); [IO.File]::WriteAllText($k, $t.Substring(0, $at) + $t.Substring($at).Replace("  if (Version = '0.1.0') and (Kind = 'x64') then begin`r`n    Result := Result + 'x64-only.dll|';", "  if (Version = '0.1.0') and (Kind = 'x64') then begin")) } 'what each earlier version installed .*arm64'
    Expect-Failure 'whose record says it carries x64 alone' { param($c) $r = Join-Path $c 'setup-record.json'; $j = Get-Content -LiteralPath $r -Raw | ConvertFrom-Json; $j.architectures = @('x64'); $j | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $r -Encoding utf8 } 'kinds of Windows it carries'
    Expect-Failure 'whose Arm update check is x64''s' { param($c) Copy-Item -LiteralPath (Join-Path $c 'inputs/qkvm-update-check.exe') -Destination (Join-Path $c 'inputs/qkvm-update-check-arm64.exe') -Force } 'update check for arm64 is not'
    Expect-Failure 'whose list of folders does more than say them' { param($c) $k = Join-Path $c 'inputs/folders.iss'; [IO.File]::AppendAllText($k, "procedure Other;`nbegin`nend;`n") } 'one function of each kind that says the package''s folders'
    Expect-Failure 'that removes what a setup left elsewhere' { param($c) Edit-Script $c "        Entry := Folder + '\' + Found.Name;" "        Entry := ExpandConstant('{userdocs}') + '\' + Found.Name;" } 'removal after the copy lacks'
    Expect-Failure 'whose program is not written last' { param($c) $l = Join-Path $c 'inputs/files.iss'; $lines = @([IO.File]::ReadAllLines($l)); [IO.File]::WriteAllLines($l, [string[]] (@($lines[-1]) + $lines[0..($lines.Count - 2)])) } 'program''s file is not the last'
    Expect-Failure 'whose marker comes after the program' { param($c) Edit-Script $c "Source: `"{#SourcePath}qkvm-desktop.installed`"; DestDir: `"{app}`"; Flags: ignoreversion`r`n" ''; Edit-Script $c '#include "files.iss"' "#include `"files.iss`"`r`nSource: `"{#SourcePath}qkvm-desktop.installed`"; DestDir: `"{app}`"; Flags: ignoreversion" } 'the package and the marker'
    Expect-Failure 'that deletes something else' { param($c) Edit-Script $c '    Installing := True;' "    Installing := True;`r`n  DeleteFile(ExpandConstant('{userdocs}\x'));" } 'deletes or renames otherwise'
    Expect-Failure 'that says another 105' { param($c) Edit-Script $c 'qKVM-RESULT: 105 the program was still running after 60 seconds' 'qKVM-RESULT: 105 still running' } 'result line of exit code 105'
    Expect-Failure 'that says 105 after it has ended' { param($c) Edit-Script $c "    Log('qKVM-RESULT: 105 the program was still running after 60 seconds; nothing was changed');`r`n" '' } 'result line of exit code 105'
    Expect-Failure 'that says nothing of 7' { param($c) Edit-Script $c "    Log(UnchangedLine(Reason));" "    Log('7');" } 'result line of exit code 7'
    Expect-Failure 'that lets its folder be chosen' { param($c) Edit-Script $c 'DisableDirPage=yes' 'DisableDirPage=auto' } 'DisableDirPage=yes'
    Expect-Failure 'that calls something else outside' { param($c) Edit-Script $c 'procedure ExitProcess(ExitCode: Cardinal);' "function MessageBeep(Kind: Cardinal): Boolean;`r`n  external 'MessageBeep@user32.dll stdcall';`r`nprocedure ExitProcess(ExitCode: Cardinal);" } 'calls outside Pascal Script otherwise'
    Expect-Failure 'with another AppId' { param($c) Edit-Script $c 'AppId={{A964EFA6' 'AppId={{B964EFA6' } 'AppId='
    Expect-Failure 'that runs something else' { param($c) Edit-Script $c "[Run]`r`n" "[Run]`r`nFilename: `"{sys}\cmd.exe`"; Flags: nowait`r`n" } 'something else than the program is run'
    Expect-Failure 'with registry entries' { param($c) Edit-Script $c '[Icons]' "[Registry]`r`nRoot: HKCU; Subkey: `"Software\qKVM`"`r`n`r`n[Icons]" } '\[Registry\] section'
    Expect-Failure 'that asks the network' { param($c) Edit-Script $c "[Code]`r`n" "[Code]`r`n// DownloadTemporaryFile`r`n" } 'uses Download'
    # (Its version texts made 0.1.1 in place: they are UTF-16 in the setup's version resource, which is not compressed.)
    Expect-Failure 'of another version' { param($c)
        $file = Join-Path $c 'qKVM-Desktop-0.1.0-windows-setup.exe'
        $text = [Text.Encoding]::Latin1.GetString([IO.File]::ReadAllBytes($file))
        $from = [Text.Encoding]::Latin1.GetString([Text.Encoding]::Unicode.GetBytes('0.1.0 '))
        if (-not $text.Contains($from)) { throw 'The setup has no version text to change.' }
        [IO.File]::WriteAllBytes($file, [Text.Encoding]::Latin1.GetBytes($text.Replace($from, [Text.Encoding]::Latin1.GetString([Text.Encoding]::Unicode.GetBytes('0.1.1 '))))) } 'does not say the package''s version'
    Expect-Failure 'that launches the program without --after-update' { param($c) Edit-Script $c 'Parameters: "--after-update"; Description:' 'Description:' } 'something else than the program is run'
    # (An update's own start: never from [Run], which Inno Setup runs before the post-install step; only at its very end,
    # the copy judged.)
    Expect-Failure 'that starts the program from a Run entry before the copy is judged' { param($c) Edit-Script $c "Check: StartUnticked`r`n" "Check: StartUnticked`r`nFilename: `"{app}\qkvm-desktop.exe`"; Parameters: `"--after-update`"; Flags: nowait runasoriginaluser; Check: StartsAgain`r`n" } 'before the copy is flushed and marked finished'
    Expect-Failure 'that starts the program before the copy is flushed' { param($c)
        Edit-Script $c "    RemoveSetupLeftovers;`r`n    if StartsAgain then`r`n      StartTheCopy;`r`n" "    RemoveSetupLeftovers;`r`n"
        Edit-Script $c "    Finished := True;`r`n    NotCounted := NotCopied;`r`n" "    if StartsAgain then`r`n      StartTheCopy;`r`n    Finished := True;`r`n    NotCounted := NotCopied;`r`n" } 'only once the copy is flushed and marked finished'
    Expect-Failure 'that never starts the program after an update' { param($c) Edit-Script $c "    RemoveSetupLeftovers;`r`n    if StartsAgain then`r`n      StartTheCopy;`r`n" "    RemoveSetupLeftovers;`r`n" } 'only once the copy is flushed and marked finished'
    # (The starts made during the copy waited for before it is judged, for a minute at the most, every one that runs
    # from the program's folder and only while it runs; what a finishing record names left to it by the in-use look, a
    # plain file only; and what a cut copy left removed where its update is put back whole, and only there.)
    $waits = "    PostInstalled := True;`r`n    // (The starts made during the copy ended first: they hold what is judged, WaitForStartsToEnd.)`r`n    StartsSince := GetTickCount;`r`n    StillRunning := WaitForStartsToEnd(StartsSince);`r`n"
    Expect-Failure 'that judges the copy while starts made during it still run' { param($c) Edit-Script $c $waits "    PostInstalled := True;`r`n    StartsSince := GetTickCount;`r`n    StillRunning := 0;`r`n" } 'does not wait for the starts of the program'
    Expect-Failure 'that waits for the starts only once the copy is flushed' { param($c) Edit-Script $c $waits "    PostInstalled := True;`r`n    StartsSince := GetTickCount;`r`n    StillRunning := 0;`r`n"
        Edit-Script $c "      else begin`r`n        Finished := FinishCopy;" "      else begin`r`n        StillRunning := WaitForStartsToEnd(StartsSince);`r`n        Finished := FinishCopy;" } 'does not wait for the starts of the program'
    Expect-Failure 'whose interactive install shows nothing while it waits for the starts' { param($c) Edit-Script $c '  Shown := not WizardSilent;' '  Shown := False;' } 'interactive install does not show its wait'
    Expect-Failure 'whose page of the wait is never stepped' { param($c) Edit-Script $c "        Most := Result;`r`n      if Shown then`r`n        StartsPage.SetProgress(Integer(TicksSince(Since)), StartsEndMs);`r`n" "        Most := Result;`r`n" } 'interactive install does not show its wait'
    Expect-Failure 'that opens the earlier program but once to tell its kind' { param($c) Edit-Script $c '  while not Opened and (Tries < 2) do begin' '  while not Opened and (Tries < 1) do begin' } 'take an earlier install''s from its program'
    Expect-Failure 'that waits for the starts without end' { param($c) Edit-Script $c '  while (Result > 0) and (TicksSince(Since) < StartsEndMs) do begin' '  while Result > 0 do begin' } 'does not wait for the starts of the program'
    Expect-Failure 'that flushes but once beside a start made as it flushes' { param($c) Edit-Script $c "      while (NotCounted <> '') and (StillRunning = 0) and (TicksSince(StartsSince) < StartsEndMs) and (StartsRunning > 0) do begin`r`n        StillRunning := WaitForStartsToEnd(StartsSince);`r`n        if StillRunning = 0 then`r`n          NotCounted := FlushCopy;`r`n      end;`r`n" '' } 'flushed but once beside a start'
    Expect-Failure 'that waits beyond the minute beside a start made as it flushes' { param($c) Edit-Script $c "      while (NotCounted <> '') and (StillRunning = 0) and (TicksSince(StartsSince) < StartsEndMs) and (StartsRunning > 0) do begin" "      while (NotCounted <> '') and (StartsRunning > 0) do begin" } 'flushed but once beside a start'
    Expect-Failure 'whose copy counts while a start still runs after the minute' { param($c) Edit-Script $c "    else if SetAside and (StillRunning > 0) then`r`n      // (One that still runs after a minute holds what would be flushed and put back: the copy does not count, and says`r`n      // why, and the earlier version is put back; this setup never ends it.)`r`n      NotCounted := 'qKVM Desktop, started during the copy, still ran from its folder after a minute'`r`n" '' } 'the copy counts where a start of the program made during it still runs'
    Expect-Failure 'that waits for the starts of any folder' { param($c) Edit-Script $c " and (Copy(Name, Length(App) + 1, 1) = '\') and PathSame(Copy(Name, 1, Length(App)), App) then" ' then' } 'does not wait for the starts of the program'
    Expect-Failure 'that waits for a start that has ended' { param($c) Edit-Script $c ' and (WaitForSingleObject(Process, 0) <> WaitObject0) then begin' ' then begin' } 'does not wait for the starts of the program'
    Expect-Failure 'that refuses beside what a finishing update could not remove' { param($c) Edit-Script $c "      AddLeftAside(LeftAside);`r`n" '' } 'does not leave to its record what a finishing update could not remove'
    Expect-Failure 'that leaves to a record a redirection it names' { param($c) Edit-Script $c '        if PlainFileAt(InApp(Names[I]) + Endings[I]) then' '        if ThingAt(InApp(Names[I]) + Endings[I]) then' } 'does not leave to its record what a finishing update could not remove'
    Expect-Failure 'that leaves what a cut copy left after its put-back' { param($c) Edit-Script $c "    if Unfinished and (Result = '') then`r`n      RemoveSetupLeftoversOf(Folder);`r`n" '' } 'leaves what its cut copy left'
    Expect-Failure 'that removes what a cut copy left where its put-back stopped' { param($c) Edit-Script $c "    if Unfinished and (Result = '') then" '    if Unfinished then' } 'leaves what its cut copy left'
    Expect-Failure 'that restarts the program without --after-update' { param($c) Edit-Script $c "procedure StartTheCopy;`r`nvar`r`n  Started: Integer;`r`nbegin`r`n  if ExecAsOriginalUser(ProgramPath, '--after-update'" "procedure StartTheCopy;`r`nvar`r`n  Started: Integer;`r`nbegin`r`n  if ExecAsOriginalUser(ProgramPath, ''" } 'does not start the program again as it should'
    Expect-Failure 'that cannot tell the room of a folder behind a junction' { param($c) Edit-Script $c "    if not GetSpaceOnDisk64(AddBackslash(ExtractFileDrive(ExpandConstant('{app}'))), Free, Total) then`r`n      Result := 'the free room" "      Result := 'the free room" } 'closing of a running copy lacks'
    Expect-Failure 'that does not look at the room for an update' { param($c) Edit-Script $c 'GetSpaceOnDisk64(' 'GetSpaceOnDisk(' } 'lacks: GetSpaceOnDisk64'
    Expect-Failure 'that does not look for files in use' { param($c) Edit-Script $c 'TFileStream.Create(' 'TStringStream.Create(' } 'lacks: TFileStream'
    Expect-Failure 'that takes an older version' { param($c) Edit-Script $c 'ComparePackedVersion(' 'Max(' } 'lacks: ComparePackedVersion'
    Expect-Failure 'not compiled' { param($c) Remove-Item -LiteralPath (Join-Path $c 'qKVM-Desktop-0.1.0-windows-setup.exe') } 'did not compile'
    # (Inno Setup leaves room in the setup's manifest for the level it writes: another of no more letters takes its place.)
    Expect-Failure 'whose manifest asks for an administrator' { param($c)
        $file = Join-Path $c 'qKVM-Desktop-0.1.0-windows-setup.exe'
        $text = [Text.Encoding]::Latin1.GetString([IO.File]::ReadAllBytes($file))
        $from = [regex]::Match($text, 'level="asInvoker" +uiAccess').Value
        if (-not $from) { throw 'The setup''s manifest has no level to change.' }
        $to = 'level="requireAdministrator"'.PadRight($from.Length - 'uiAccess'.Length) + 'uiAccess'
        [IO.File]::WriteAllBytes($file, [Text.Encoding]::Latin1.GetBytes($text.Replace($from, $to))) } 'asks for more than the user'
    Expect-Failure 'whose twin is compressed' { param($c) Copy-Item -LiteralPath (Join-Path $c 'qKVM-Desktop-0.1.0-windows-setup.exe') -Destination (Join-Path $c 'scan/qKVM-Desktop-0.1.0-windows-setup.exe') -Force } 'twin for the byte scan is not readable'
    # (Closing a running copy for the person, 0.1.2: each protection taken away, alone.)
    Expect-Failure 'that ends the program by force without the choice' { param($c) Edit-Script $c '    if Choice = IDNO then begin' '    if True then begin' } 'closing of a running copy lacks'
    Expect-Failure 'that ends it by force on a silent install untold' { param($c) Edit-Script $c '  if (Outcome <> Ended) and ForceClose then begin' '  if Outcome <> Ended then begin' } 'ends the program by force otherwise'
    Expect-Failure 'that ends it by force elsewhere' { param($c) Edit-Script $c "function StillRunningText(Outcome: Integer): String;`r`nbegin" "function StillRunningText(Outcome: Integer): String;`r`nbegin`r`n  EndProgram;" } 'ends the program by force otherwise'
    Expect-Failure 'that ends another process' { param($c) Edit-Script $c "      end else`r`n        CloseHandle(Process);" "      end else begin`r`n        TerminateProcess(Process, 1);`r`n        CloseHandle(Process);`r`n      end;" } 'ends the program by force otherwise'
    Expect-Failure 'that takes a copy run from elsewhere' { param($c) Edit-Script $c 'PathSame(Copy(Name, 1, Size), Path)' 'True' } 'closing of a running copy lacks'
    Expect-Failure 'that reads the answer of another process' { param($c) Edit-Script $c '    if Id = ProgramId then begin' '    if Id <> 0 then begin' } 'closing of a running copy lacks'
    Expect-Failure 'that asks more of the event than to set it' { param($c) Edit-Script $c 'EventModifyState = $0002;' 'EventModifyState = $1F0003;' } 'closing of a running copy lacks'
    Expect-Failure 'that asks by an event for the whole PC' { param($c) Edit-Script $c "QuitRequestEvent = '$QuitEvent';" "QuitRequestEvent = 'Global\qKVM-Desktop-Quit';" } 'closing of a running copy lacks'
    Expect-Failure 'that looks for another window' { param($c) Edit-Script $c "IconWindow = '$IconWindow';" "IconWindow = 'QTrayIconMessageWindowClass';" } 'closing of a running copy lacks'
    Expect-Failure 'that refuses at its start for the mutex' { param($c) Edit-Script $c "  if not UpdateMode and not Installing then begin`r`n    Result := '';`r`n    Exit;`r`n  end;`r`n" '' } 'closing of a running copy lacks'
    Expect-Failure 'whose uninstall log keeps no mutex' { param($c) Edit-Script $c 'if not UpdateMode and not Installing then begin' 'if not UpdateMode then begin' } 'closing of a running copy lacks'
    Expect-Failure 'that keeps the mutex only for an uninstaller that never asks' { param($c) Edit-Script $c 'if not UpdateMode and not Installing then begin' 'if not UpdateMode and not IsUninstaller then begin' } 'closing of a running copy lacks'
    Expect-Failure 'that does not wait for a copy that is ending' { param($c) Edit-Script $c "          Result := Ending;`r`n          Most := UpdateWaitMs;" '          Result := Ending;' } 'closing of a running copy lacks'
    # (Its request that stands is a state of its own making, set and reset, and withdrawn under its lock: review C4.)
    Expect-Failure 'that never sets its request that stands' { param($c) Edit-Script $c "    SetEvent(Wanted);`r`n  end;" '  end;' } 'closing of a running copy lacks'
    Expect-Failure 'that withdraws by letting go of its ticket alone' { param($c) Edit-Script $c '    Result := WaitForSingleObject(Wanted, 0) <> WaitObject0;' '    Result := False;' } 'closing of a running copy lacks'
    Expect-Failure 'that keeps a ticket that stays set' { param($c) Edit-Script $c '    Wanted := CreateEventW(0, 0, 0, QuitWantedEvent);' '    Wanted := CreateEventW(0, 1, 0, QuitWantedEvent);' } 'closing of a running copy lacks'
    Expect-Failure 'that takes an event of the name made by another' { param($c) Edit-Script $c "    if (Wanted <> 0) and (DLLGetLastError = ErrorAlreadyExists) then begin" "    if False then begin" } 'closing of a running copy lacks'
    Expect-Failure 'that sets its ticket again for a request asked again' { param($c) Edit-Script $c "    SetEvent(Wanted);`r`n  end;`r`n  Event := OpenEventW(" "  end;`r`n  SetEvent(Wanted);`r`n  Event := OpenEventW(" } 'closing of a running copy lacks'
    Expect-Failure 'that tells a program that took the request that it did not answer' { param($c) Edit-Script $c "        if WithdrawRequest then begin`r`n          Result := Ending;" "        if WithdrawRequest and False then begin`r`n          Result := Ending;" } 'closing of a running copy lacks'
    Expect-Failure 'that asks without a request that stands' { param($c) Edit-Script $c "    Wanted := CreateEventW(0, 0, 0, QuitWantedEvent);`r`n" '' } 'closing of a running copy lacks'
    Expect-Failure 'that asks though its request that stands could not be made' { param($c) Edit-Script $c "    if Wanted = 0 then begin`r`n      Log('qKVM Desktop cannot be asked to close: this setup could not make its event" "    if False then begin`r`n      Log('qKVM Desktop cannot be asked to close: this setup could not make its event" } 'closing of a running copy lacks'
    Expect-Failure 'that asks again without end after giving up' { param($c) Edit-Script $c "          AskedAgain := True;`r`n" '' } 'closing of a running copy lacks'
    Expect-Failure 'that asks again after giving up with no second of its own' { param($c) Edit-Script $c "          Most := Elapsed + 1000;`r`n" '' } 'closing of a running copy lacks'
    Expect-Failure 'that asks again after giving up and never withdraws it' { param($c) Edit-Script $c "          Withdrawn := False;`r`n" '' } 'closing of a running copy lacks'
    Expect-Failure 'whose wait for a running copy is stretched otherwise' { param($c) Edit-Script $c "      Elapsed := Elapsed + UpdateStepMs;" "      Elapsed := Elapsed + UpdateStepMs;`r`n      Most := Most + 1;" } 'not bounded as it should be'
    Expect-Failure 'that keeps wanting a request it gave up on' { param($c) Edit-Script $c "        if WithdrawRequest then begin" '        if False then begin' } 'closing of a running copy lacks'
    Expect-Failure 'that wants it still once it has gone on' { param($c) Edit-Script $c "    if WithdrawRequest then`r`n      if Result <> Ended then`r`n        Result := Ending;" "    Log('');" } 'closing of a running copy lacks'
    # (A request taken wins over a word that it stays read before, review F2.)
    Expect-Failure 'that believes a word that it stays from a program that took the request' { param($c) Edit-Script $c "          if not WithdrawRequest then begin`r`n            Result := Held;" "          if True then begin`r`n            Result := Held;" } 'closing of a running copy lacks'
    Expect-Failure 'that keeps a word that it stays over a request taken' { param($c) Edit-Script $c "      if Result <> Ended then`r`n        Result := Ending;" "      if Result = NoAnswer then`r`n        Result := Ending;" } 'closing of a running copy lacks'
    Expect-Failure 'that reads that word again once the request was taken' { param($c) Edit-Script $c "          Before := AnswerNumber(Said);`r`n        end;`r`n" "        end;`r`n" } 'closing of a running copy lacks'
    Expect-Failure 'that tells a program that took the request that it stays, by not waiting for it' { param($c) Edit-Script $c "          Before := AnswerNumber(Said);`r`n        end;`r`n        if Result <> Ending then begin" "          Before := AnswerNumber(Said);`r`n          Exit;`r`n        end;`r`n        if Result <> Ending then begin" } 'closing of a running copy lacks'
    Expect-Failure 'that takes a copy with no answer window for one that does not answer' { param($c) Edit-Script $c "  if Before = '' then begin" '  if False then begin' } 'closing of a running copy lacks'
    Expect-Failure 'that ends a copy it could not ask without saying so' { param($c) Edit-Script $c "    if Outcome = CannotBeAsked then`r`n      Log('qKVM Desktop could not be asked to close, so what it is doing is not known; it is ended by force all the same, as /FORCECLOSE=1 says.');`r`n" '' } 'closing of a running copy lacks'
    Expect-Failure 'that takes a late answer for one that stays open' { param($c) Edit-Script $c "(AnswerNumber(Said) <> Before) and AnswerLate(Said) then begin" "(AnswerNumber(Said) <> Before) and False then begin" } 'closing of a running copy lacks'
    Expect-Failure 'that keeps the mark of a late answer in its number' { param($c) Edit-Script $c "  if Copy(Result, Length(Result), 1) = '*' then`r`n    Result := Copy(Result, 1, Length(Result) - 1);`r`n" '' } 'closing of a running copy lacks'
    Expect-Failure 'that waits longer than the program honours a request' { param($c) Edit-Script $c 'CloseWaitMs = 10000;' 'CloseWaitMs = 15000;' } 'less time than this setup waits'
    # ({app} before Inno Setup knows it, each way alone: 0.1.2's first VM run.)
    Expect-Failure 'that looks for the program in {app} on its first page' { param($c) Edit-Script $c "FindProgramAt(AddBackslash(InstalledFolder) + 'qkvm-desktop.exe')" 'FindProgram' } 'expands \{app\} before Inno Setup knows it: InitializeWizard -> FindProgram -> ProgramPath'
    Expect-Failure 'that asks {app} at its start' { param($c) Edit-Script $c "  InstalledVersion := Installed;" "  InstalledVersion := Installed;`r`n  Log(RoomFolder);" } 'expands \{app\} before Inno Setup knows it: InitializeSetup -> RoomFolder'
    Expect-Failure 'that asks {app} for the first page it shows' { param($c) Edit-Script $c "  Result := (PageID = wpSelectTasks) and (InstalledVersion <> '');" "  Result := (PageID = wpSelectTasks) and FileExists(ProgramPath);" } 'expands \{app\} before Inno Setup knows it: ShouldSkipPage -> ProgramPath'
    Expect-Failure 'whose Next asks {app} on every page' { param($c) Edit-Script $c "  if CurPageID = wpReady then`r`n    Result := CloseAtInstall;" "  Result := CloseAtInstall;" } 'NextButtonClick does not reach what expands'
    # (And each way of saying it that Pascal Script reads as the same, or that Inno Setup asks early: review 012-final.)
    Expect-Failure 'that calls the {app} routine on its first page in another case' { param($c) Edit-Script $c "FindProgramAt(AddBackslash(InstalledFolder) + 'qkvm-desktop.exe')" 'findProgram' } 'expands \{app\} before Inno Setup knows it: InitializeWizard -> FindProgram -> ProgramPath'
    Expect-Failure 'that asks {app} at its start through a code constant in a string' { param($c) Edit-Script $c "  InstalledVersion := Installed;`r`n" "  InstalledVersion := Installed;`r`n  Log(ExpandConstant('{code:RoomFolder}'));`r`n" } 'expands \{app\} before Inno Setup knows it: InitializeSetup -> RoomFolder'
    Expect-Failure 'whose first page has a routine of another name' { param($c) Edit-Script $c 'function NextButtonClick(CurPageID: Integer): Boolean;' "<event('InitializeWizard')>`r`nprocedure LookEarly;`r`nbegin`r`n  if FindProgram then`r`n    Log('');`r`nend;`r`n`r`nfunction NextButtonClick(CurPageID: Integer): Boolean;" } 'expands \{app\} before Inno Setup knows it: InitializeWizard as LookEarly -> FindProgram -> ProgramPath'
    Expect-Failure 'whose Next has a routine of another name' { param($c) Edit-Script $c 'function NextButtonClick(CurPageID: Integer): Boolean;' "<event('NextButtonClick')>`r`nfunction NextOnEveryPage(CurPageID: Integer): Boolean;`r`nbegin`r`n  Result := CloseAtInstall;`r`nend;`r`n`r`nfunction NextButtonClick(CurPageID: Integer): Boolean;" } 'expands \{app\} before Inno Setup knows it: NextButtonClick as NextOnEveryPage -> CloseAtInstall'
    Expect-Failure 'whose first page, indented, looks in {app}' { param($c) Edit-Script $c "`r`nprocedure InitializeWizard;" "`r`n  procedure InitializeWizard;"; Edit-Script $c "FindProgramAt(AddBackslash(InstalledFolder) + 'qkvm-desktop.exe')" 'FindProgram' } 'expands \{app\} before Inno Setup knows it: InitializeWizard -> FindProgram -> ProgramPath'
    Expect-Failure 'whose Setup section asks {app} by a code constant' { param($c) Edit-Script $c "SetupMutex=qKVM-Desktop-Setup`r`n" "SetupMutex=qKVM-Desktop-Setup`r`nAppVerName={code:RoomFolder}`r`n" } 'expands \{app\} before Inno Setup knows it: \[Setup\] AppVerName -> RoomFolder'
    Expect-Failure 'whose Setup section asks a code constant the Code section does not have' { param($c) Edit-Script $c "SetupMutex=qKVM-Desktop-Setup`r`n" "SetupMutex=qKVM-Desktop-Setup`r`nAppVerName={code:NoSuchRoutine}`r`n" } '\[Setup\] AppVerName asks \{code:NoSuchRoutine\}, which the \[Code\] section does not have'
    Expect-Failure 'whose task is checked by what expands {app}' { param($c) Edit-Script $c 'GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked' 'GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked; Check: findProgram' } 'expands \{app\} before Inno Setup knows it: \[Tasks\] desktopicon -> FindProgram -> ProgramPath'
    Expect-Failure 'whose task says {app}' { param($c) Edit-Script $c 'Description: "{cm:CreateDesktopIcon}"' 'Description: "{app}"' } '\[Tasks\] desktopicon says \{app\}'
    Expect-Failure 'whose file is checked by what expands {app}' { param($c) Edit-Script $c 'Source: "{#SourcePath}qkvm-desktop.installed"; DestDir: "{app}"; Flags: ignoreversion' 'Source: "{#SourcePath}qkvm-desktop.installed"; DestDir: "{app}"; Flags: ignoreversion; Check: FindProgram' } 'expands \{app\} before Inno Setup knows it: \[Files\] \{#SourcePath\}qkvm-desktop.installed -> FindProgram -> ProgramPath'
    Expect-Failure 'whose first page calls after two slashes in a string' { param($c) Edit-Script $c "  if (InstalledFolder <> '') and FindProgramAt(" "  Log('https://qkvm.com/'); if FindProgram then Log('');`r`n  if (InstalledFolder <> '') and FindProgramAt(" } 'expands \{app\} before Inno Setup knows it: InitializeWizard -> FindProgram -> ProgramPath'
    # (And one that names another constant that says it, review T3.)
    Expect-Failure 'whose first page names a constant named after one that says {app}' { param($c) Edit-Script $c "`r`nprocedure InitializeWizard;" "`r`nconst`r`n  InstallRoot = '{app}';`r`n  EarlyRoot = InstallRoot;`r`n`r`nprocedure InitializeWizard;"; Edit-Script $c "FindProgramAt(AddBackslash(InstalledFolder) + 'qkvm-desktop.exe')" 'FileExists(ExpandConstant(EarlyRoot))' } 'expands \{app\} before Inno Setup knows it: InitializeWizard -> EarlyRoot'
    Expect-Failure 'whose first page names a constant that says {app}' { param($c) Edit-Script $c "`r`nprocedure InitializeWizard;" "`r`nconst`r`n  EarlyPath = '{app}\qkvm-desktop.exe';`r`n`r`nprocedure InitializeWizard;"; Edit-Script $c "FindProgramAt(AddBackslash(InstalledFolder) + 'qkvm-desktop.exe')" 'FileExists(ExpandConstant(EarlyPath))' } 'expands \{app\} before Inno Setup knows it: InitializeWizard -> EarlyPath'
    Expect-Failure 'whose first page calls after a quote in a comment' { param($c) Edit-Script $c "FindProgramAt(AddBackslash(InstalledFolder) + 'qkvm-desktop.exe')" "{ it's early } FindProgram and (InstalledFolder <> 'x')" } 'expands \{app\} before Inno Setup knows it: InitializeWizard -> FindProgram -> ProgramPath'
    Expect-Failure 'that puts the earlier version back however early it ends' { param($c) Edit-Script $c "  if SetAside and not PostInstalled then`r`n    if PutPackageBack" "  if not PostInstalled then`r`n    if PutPackageBack" } 'DeinitializeSetup does not reach what expands'
    Expect-Failure 'whose request stands under another name' { param($c) Edit-Script $c "QuitWantedEvent = '$WantedEvent';" "QuitWantedEvent = 'Global\qKVM-Desktop-Quit-Wanted';" } 'closing of a running copy lacks'
    Expect-Failure 'that waits less' { param($c) Edit-Script $c 'CloseWaitMs = 10000;' 'CloseWaitMs = 1000;' } 'closing of a running copy lacks'
    Expect-Failure 'that never offers to close it' { param($c) Edit-Script $c '  if WizardSilent or UpdateMode or not FindProgram then' '  if True or WizardSilent or UpdateMode or not FindProgram then' } 'closing of a running copy lacks'
    Expect-Failure 'that closes it without asking' { param($c) Edit-Script $c "['Close qKVM and continue', 'Cancel'], 0) <> IDOK then begin" "['Close qKVM and continue', 'Cancel'], 0) = 0 then begin" } 'closing of a running copy lacks'
    Expect-Failure 'that goes on after Cancel' { param($c) Edit-Script $c "  until Choice = IDCANCEL;`r`n  Result := False;" "  until Choice = IDCANCEL;`r`n  Result := True;" } 'closing of a running copy lacks'
    Expect-Failure 'that closes nothing on a silent install' { param($c) Edit-Script $c '    Reason := CloseProgramSilently;' "    Reason := '';" } 'closing of a running copy lacks'
    Expect-Failure 'that looks at files in use only for an update' { param($c) Edit-Script $c "  if Reason = '' then begin`r`n    // (Every file looked at once;" "  if (Reason = '') and UpdateMode then begin`r`n    // (Every file looked at once;" } 'in every mode'
    # (The 2 s count the looks, and no look goes through the whole folder again, review C10.)
    Expect-Failure 'that looks through the whole folder again each time' { param($c) Edit-Script $c "          Path := Busy[I];`r`n          if Unlisted.IndexOf(Path) >= 0 then begin" "          Busy.Clear;`r`n          FilesInUse(ExpandConstant('{app}'), Busy, Unlisted);`r`n          Path := Busy[I];`r`n          if Unlisted.IndexOf(Path) >= 0 then begin" } 'in every mode'
    # (A folder that could not be looked through is looked through again, whole, at each look again, and free once it
    # can be, with what in it is in use joining the rest; never freed by FreeToReplace unlooked at; review F7.)
    Expect-Failure 'that looks again at a folder it could not look through as at a file' { param($c) Edit-Script $c "          if Unlisted.IndexOf(Path) >= 0 then begin" "          if False then begin" } 'in every mode'
    Expect-Failure 'that frees a folder it could not look through without looking through it' { param($c) Edit-Script $c "            FilesInUse(Path, Busy, Unlisted);`r`n          end else if" "          end else if" } 'in every mode'
    Expect-Failure 'that keeps a folder it can look through now in use' { param($c) Edit-Script $c "            Busy.Delete(I);`r`n            Unlisted.Delete(Unlisted.IndexOf(Path));`r`n" "            Unlisted.Delete(Unlisted.IndexOf(Path));`r`n" } 'in every mode'
    Expect-Failure 'whose 2 s count the waits alone' { param($c) Edit-Script $c "      while (Busy.Count > 0) and (TicksSince(Began) < LookMs - UpdateStepMs) do begin" "      while (Busy.Count > 0) and (I < 8) do begin" } 'in every mode'
    # (No file looked at again after the 2 s, review R6; and the first look whole, its 2 s counted from its end, review
    # F1.)
    Expect-Failure 'whose 2 s count the first look too' { param($c) Edit-Script $c "        end;`r`n      Began := GetTickCount;`r`n      while (Busy.Count > 0)" "        end;`r`n      while (Busy.Count > 0)"
        Edit-Script $c "    try`r`n      FilesInUse(ExpandConstant('{app}'), Busy, Unlisted);" "    try`r`n      Began := GetTickCount;`r`n      FilesInUse(ExpandConstant('{app}'), Busy, Unlisted);" } 'in every mode'
    Expect-Failure 'whose first look stops at the 2 s' { param($c) Edit-Script $c "    while True do begin`r`n      if (Name <> '.') and" "    while True do begin`r`n      if TicksSince(0) >= LookMs then`r`n        Break;`r`n      if (Name <> '.') and" } 'stops before it has looked at every file'
    # (A folder that cannot be looked through is in use, not empty, unless it is not there, review R5's way.)
    Expect-Failure 'that takes a folder it cannot look through for an empty one' { param($c) Edit-Script $c "    if not FreeToReplace(Folder) then begin`r`n      Log('A folder of qKVM Desktop cannot be looked through: ' + Folder);`r`n      Busy.Add(Folder);`r`n      Unlisted.Add(Folder);`r`n    end;`r`n" '' } 'update mode of the contract lacks'
    Expect-Failure 'that takes a folder it cannot look through for one that is not there' { param($c) Edit-Script $c '    if not FreeToReplace(Folder) then begin' '    if not DirExists(Folder) then begin' } 'update mode of the contract lacks'
    Expect-Failure 'that leaves a folder it cannot look through out of what is in use' { param($c) Edit-Script $c "      Busy.Add(Folder);`r`n      Unlisted.Add(Folder);" '      Unlisted.Add(Folder);' } 'update mode of the contract lacks'
    Expect-Failure 'that names a folder it cannot look through as a file in use' { param($c) Edit-Script $c "        if Unlisted.IndexOf(Busy[0]) >= 0 then" "        if False then" } 'update mode of the contract lacks'
    Expect-Failure 'whose look again goes on after the 2 s' { param($c) Edit-Script $c "          if TicksSince(Began) >= LookMs then`r`n            Break;`r`n" '' } 'in every mode'
    Expect-Failure 'that waits for a look again past the 2 s' { param($c) Edit-Script $c '(TicksSince(Began) < LookMs - UpdateStepMs) do begin' '(TicksSince(Began) < LookMs) do begin' } 'in every mode'
    Expect-Failure 'whose 2 s are longer' { param($c) Edit-Script $c '  LookMs = 2000;' '  LookMs = 20000;' } 'update mode of the contract lacks'
    Expect-Failure 'that takes a redirection for a file it may replace' { param($c) Edit-Script $c "      if (Found.Attributes and (ReparsePointAttribute or DirectoryAttribute)) <> 0 then`r`n        Exit;" "      if False then`r`n        Exit;" } 'update mode of the contract lacks'
    # (A file gone since it was found in use is free, one that cannot be looked at is not, review R5.)
    Expect-Failure 'that takes a file gone since for one in use' { param($c) Edit-Script $c "    Result := (Error = ErrorFileNotFound) or (Error = ErrorPathNotFound);" "    Result := False;" } 'update mode of the contract lacks'
    Expect-Failure 'that takes a file it cannot look at for a free one' { param($c) Edit-Script $c "    Result := (Error = ErrorFileNotFound) or (Error = ErrorPathNotFound);" "    Result := True;" } 'update mode of the contract lacks'
    Expect-Failure 'that takes another error for a file gone' { param($c) Edit-Script $c '  ErrorPathNotFound = 3;' '  ErrorPathNotFound = 5;' } 'update mode of the contract lacks'
    # (A folder Windows could not go through to its end is not taken for a whole one, review F3.)
    Expect-Failure 'that takes a look cut short for the end of the folder' { param($c) Edit-Script $c "        if Error <> ErrorNoMoreFiles then`r`n" "        if False then`r`n" } 'update mode of the contract lacks'
    Expect-Failure 'that takes another error for the end of the folder' { param($c) Edit-Script $c '  ErrorNoMoreFiles = 18;' '  ErrorNoMoreFiles = 5;' } 'update mode of the contract lacks'
    Expect-Failure 'that takes a name it cannot read for one it can' { param($c) Edit-Script $c "      if Pos(#0, Name) = 0 then begin" "      if False then begin" } 'update mode of the contract lacks'
    Expect-Failure 'that changes the files where a folder was cut short' { param($c) Edit-Script $c "  if CutShort <> '' then begin" "  if False then begin" } 'update mode of the contract lacks'
    Expect-Failure 'that reads its next entry into too little' { param($c) Edit-Script $c '  FindDataChars = 296;' '  FindDataChars = 200;' } 'update mode of the contract lacks'
    Expect-Failure 'whose first look judges a file otherwise than the looks again' { param($c) Edit-Script $c "        else if not FreeToReplace(Path) then" "        else if not FileExists(Path) then" } 'update mode of the contract lacks'
    # (Nothing closed and nothing changed without the gate, review F5.)
    Expect-Failure 'that changes the files without its gate' { param($c) Edit-Script $c "  Reason := GateReason;`r`n" "  Reason := '';`r`n" } 'closing of a running copy lacks'
    Expect-Failure 'that closes the program without its gate' { param($c) Edit-Script $c "  if GateReason <> '' then`r`n    Exit;`r`n" '' } 'closing of a running copy lacks'
    Expect-Failure 'that takes a gate it cannot open for one there' { param($c) Edit-Script $c '  if (Gate = 0) or (SetupRuns = 0) then' '  if SetupRuns = 0 then' } 'closing of a running copy lacks'
    Expect-Failure 'that takes an event it could not make for one' { param($c) Edit-Script $c '  if (Gate = 0) or (SetupRuns = 0) then' '  if Gate = 0 then' } 'closing of a running copy lacks'
    Expect-Failure 'that closes the program before it looks at the room' { param($c) Edit-Script $c "  Reason := RoomReason;`r`n  if Reason = '' then`r`n    Reason := ForeignFolderReason;" "  Reason := '';`r`n  if Reason = '' then`r`n    Reason := ForeignFolderReason;" } 'closing of a running copy lacks'
    Expect-Failure 'that closes it by hand where there is no room' { param($c) Edit-Script $c "  if RoomReason <> '' then`r`n    Exit;`r`n" '' } 'closing of a running copy lacks'
    Expect-Failure 'that leaves closed what it closed' { param($c) Edit-Script $c "      StartAtEnd := True;`r`n" '' } 'closing of a running copy lacks'
    Expect-Failure 'that never starts it again' { param($c) Edit-Script $c "  if StartAtEnd then`r`n    StartAgain;`r`n" '' } 'closing of a running copy lacks'
    Expect-Failure 'that starts something else again' { param($c) Edit-Script $c "ExecAsOriginalUser(ProgramPath, '--after-update'" "ExecAsOriginalUser(ExpandConstant('{sys}\cmd.exe'), '--after-update'" } 'does not start the program again as it should'
    Expect-Failure 'that starts the program twice' { param($c) Edit-Script $c "procedure StartAgain;" "procedure StartTwice;`r`nvar`r`n  Started: Integer;`r`nbegin`r`n  if ExecAsOriginalUser(ProgramPath, '--after-update', '', SW_SHOWNORMAL, ewNoWait, Started) then`r`n    Log('');`r`nend;`r`n`r`nprocedure StartAgain;" } 'does not start the program again as it should'
    Expect-Failure 'that starts it otherwise' { param($c) Edit-Script $c "  if ExecAsOriginalUser(ProgramPath, '--after-update', '', SW_SHOWNORMAL, ewNoWait, Started) then" "  if ExecAsOriginalUser(ProgramPath, '', '', SW_SHOWNORMAL, ewNoWait, Started) then" } 'does not start the program again as it should'
    Expect-Failure 'that does not say what is installed' { param($c) Edit-Script $c 'This upgrades it to %s and keeps' 'This upgrades it and keeps' } 'closing of a running copy lacks'
    Expect-Failure 'that asks for the tasks again on an upgrade' { param($c) Edit-Script $c "  Result := (PageID = wpSelectTasks) and (InstalledVersion <> '');" '  Result := False;' } 'closing of a running copy lacks'
    Expect-Failure 'that starts it again untold' { param($c) Edit-Script $c '  Result := MayStartProgram and CopyStarts and (UpdateMode or (WizardSilent and ClosedIt));' '  Result := MayStartProgram and CopyStarts and (UpdateMode or WizardSilent);' } 'closing of a running copy lacks'
    Expect-Failure 'with no first page' { param($c) Edit-Script $c 'DisableWelcomePage=no' 'DisableWelcomePage=yes' } 'DisableWelcomePage=no'
    Expect-Failure 'that never ticks Start' { param($c) Edit-Script $c 'Flags: nowait postinstall skipifsilent; Check: StartTicked' 'Flags: nowait postinstall skipifsilent unchecked; Check: StartTicked' } 'something else than the program is run'
    # (The setup says it is at work, as the program tells a Setup that runs, review R2.)
    Expect-Failure 'that never says it is at work' { param($c) Edit-Script $c "  SetupRuns := CreateEventW(0, 1, 0, SetupRunsName + IntToStr(GetCurrentProcessId));`r`n" '' } 'does not say it is at work'
    Expect-Failure 'that says it is at work under another name' { param($c) Edit-Script $c "  SetupRunsName = 'Local\qKVM-Desktop-Setup-';" "  SetupRunsName = 'Local\qKVM-Desktop-Setup ';" } 'does not say it is at work'
    Expect-Failure 'that says it is at work by another process''s number' { param($c) Edit-Script $c 'SetupRunsName + IntToStr(GetCurrentProcessId));' "SetupRunsName + '0');" } 'does not say it is at work'
    # (The uninstaller refuses while the program runs, by its own look at the program's mutex, review V2.)
    Expect-Failure 'whose uninstaller does not look at the program' { param($c) Edit-Script $c '  if CheckForMutexes(ProgramMutexName) then begin' '  if False then begin' } 'uninstaller does not refuse while the program runs'
    Expect-Failure 'whose uninstaller looks at another mutex' { param($c) Edit-Script $c "  ProgramMutexName = '$ProgramMutex';" "  ProgramMutexName = 'Global\QKVM Desktop';" } 'uninstaller does not refuse while the program runs'
    Expect-Failure 'whose uninstaller goes on while the program runs' { param($c) Edit-Script $c "      MsgBox('qKVM Desktop is running. Quit it first: right-click the qKVM icon in the notification area, then choose Quit. Then uninstall it again.', mbError, MB_OK);`r`n    Result := False;" "      MsgBox('qKVM Desktop is running. Quit it first: right-click the qKVM icon in the notification area, then choose Quit. Then uninstall it again.', mbError, MB_OK);`r`n    Result := True;" } 'uninstaller does not refuse while the program runs'
    # (And by the lock of the profile, from 0.2.0: in any Windows session of the account, and where another account's
    # copy in this session holds the mutex's name.)
    Expect-Failure 'whose uninstaller does not look at the profile''s lock' { param($c) Edit-Script $c '  Running := ProfileInUse;' "  Running := '';" } 'uninstaller does not refuse while the program runs'
    Expect-Failure 'whose uninstaller looks at another file' { param($c) Edit-Script $c "  ProfileLockFile = '{localappdata}\QKVM Desktop\settings.json.running';" "  ProfileLockFile = '{localappdata}\QKVM Desktop\settings.json.lock';" } 'by the lock of its profile'
    Expect-Failure 'whose uninstaller makes the file it looks at' { param($c) Edit-Script $c '  OpenExisting = 3;' '  OpenExisting = 4;' } 'by the lock of its profile'
    Expect-Failure 'whose uninstaller takes a lock held for none' { param($c) Edit-Script $c '  if Locked = ErrorLockViolation then' '  if False then' } 'by the lock of its profile'
    Expect-Failure 'whose uninstaller takes a file it could not open for none' { param($c) Edit-Script $c '  else if (Locked <> ErrorInvalidHandle) or ((Opened <> ErrorFileNotFound) and (Opened <> ErrorPathNotFound)) then' '  else if False then' } 'by the lock of its profile'
    Expect-Failure 'whose uninstaller goes on while the profile is in use' { param($c) Edit-Script $c "  if Running <> '' then begin" '  if False then begin' } 'uninstaller does not refuse while the program runs'
    # (The gate held for the whole uninstall, and the look again before anything is removed, review R4.)
    Expect-Failure 'whose uninstaller holds no gate' { param($c) Edit-Script $c "  UninstallGate := CreateMutexW(0, 0, SetupGateName);`r`n" '' } 'uninstaller does not refuse while the program runs'
    Expect-Failure 'whose uninstaller does not say it is at work' { param($c) Edit-Script $c "  UninstallRuns := CreateEventW(0, 1, 0, SetupRunsName + IntToStr(GetCurrentProcessId));`r`n" '' } 'uninstaller does not refuse while the program runs'
    Expect-Failure 'whose uninstaller holds its gate only after it looks' { param($c) Edit-Script $c "  Result := True;`r`n  // First the gate" "  Result := True;`r`n  if CheckForMutexes(ProgramMutexName) then`r`n    Exit;`r`n  // First the gate" } 'uninstaller does not refuse while the program runs'
    # (Refused as it begins while a setup is open, and where its gate or its event cannot be made, review F4.)
    Expect-Failure 'whose uninstaller takes an open setup''s mutex for its own gate' { param($c) Edit-Script $c '  if (UninstallGate <> 0) and (DLLGetLastError = ErrorAlreadyExists) then begin' '  if False then begin' } 'uninstaller does not refuse while the program runs'
    Expect-Failure 'whose uninstaller goes on while a setup is open' { param($c) Edit-Script $c "    Result := False;`r`n    Exit;`r`n  end;`r`n  UninstallRuns :=" "    Exit;`r`n  end;`r`n  UninstallRuns :=" } 'uninstaller does not refuse while the program runs'
    Expect-Failure 'whose uninstaller takes a gate it could not make for one' { param($c) Edit-Script $c '  if (UninstallGate = 0) or (UninstallRuns = 0) then begin' '  if UninstallRuns = 0 then begin' } 'uninstaller does not refuse while the program runs'
    Expect-Failure 'whose uninstaller takes an event it could not make for one' { param($c) Edit-Script $c '  if (UninstallGate = 0) or (UninstallRuns = 0) then begin' '  if UninstallGate = 0 then begin' } 'uninstaller does not refuse while the program runs'
    Expect-Failure 'whose uninstaller does not look again before it removes' { param($c) Edit-Script $c "      RaiseException('qKVM Desktop is running. Quit it first, then uninstall it again.');`r`n" '' } 'uninstaller does not refuse while the program runs'
    Expect-Failure 'whose uninstaller holds another gate' { param($c) Edit-Script $c "  SetupGateName = 'Local\qKVM-Desktop-Setup';" "  SetupGateName = 'Local\qKVM-Desktop-Uninstall';" } 'uninstaller does not refuse while the program runs'
    Expect-Failure 'whose uninstaller is never asked' { param($c) Edit-Script $c 'function InitializeUninstall: Boolean;' 'function InitializeUninstal: Boolean;' } 'uninstaller does not refuse while the program runs'
    # (Nothing started where the setup runs with administrator rights, review C1: the rule, and each way of starting.)
    Expect-Failure 'that starts the program with administrator rights' { param($c) Edit-Script $c '  Result := not IsAdmin;' '  Result := True;' } 'may start the program where it runs with administrator rights'
    Expect-Failure 'that starts it again with administrator rights' { param($c) Edit-Script $c "begin`r`n  if not MayStartProgram then`r`n    Exit;`r`n  if ExecAsOriginalUser(" "begin`r`n  if ExecAsOriginalUser(" } 'may start the program where it runs with administrator rights'
    Expect-Failure 'that ticks Start with administrator rights' { param($c) Edit-Script $c "  Result := MayStartProgram and CopyStarts and not UpdateMode and ((InstalledVersion = '') or ClosedIt);" "  Result := CopyStarts and not UpdateMode and ((InstalledVersion = '') or ClosedIt);" } 'closing of a running copy lacks'
    Expect-Failure 'that offers Start with administrator rights' { param($c) Edit-Script $c "  Result := MayStartProgram and CopyStarts and not UpdateMode and not StartTicked;" "  Result := CopyStarts and not UpdateMode and not StartTicked;" } 'closing of a running copy lacks'
    Expect-Failure 'that starts an update again with administrator rights' { param($c) Edit-Script $c "  Result := MayStartProgram and CopyStarts and (UpdateMode or (WizardSilent and ClosedIt));" "  Result := CopyStarts and (UpdateMode or (WizardSilent and ClosedIt));" } 'closing of a running copy lacks'
}

# The committed list of what each published version installed names every version CHANGELOG.md says was released
# (review i020-1, L9): a release that forgot to add its own would leave its files unknown to later setups, which would
# then refuse a record of an update of its that did not finish, and never remove what it shipped and they do not.
# (That each entry is that release's own ZIP, by its SHA-256 in that release's SHA256SUMS, is the release audit's: the
# published files are not in this tree.)
function Get-UnlistedReleases([string] $Changelog, [string] $Published, [string] $Building) {
    <# The versions $Changelog's headings say were released ("## x.y.z" or "## x.y.z (beta)", never "(not released
    yet)") that $Published does not list, but $Building, the version this tree builds: a release cannot list the files
    of its own ZIP before that ZIP is built, so its own heading says it is released while its entry is added after it
    is published, and the next version's tree is held to that entry. #>
    $released = @([regex]::Matches([IO.File]::ReadAllText($Changelog), '(?m)^## (\d+\.\d+\.\d+)(?: \(([^)]*)\))?[ \t]*\r?$') |
        Where-Object { $_.Groups[2].Value -notmatch 'not released' } | ForEach-Object { $_.Groups[1].Value })
    $listed = @((Get-Content -LiteralPath $Published -Raw | ConvertFrom-Json).versions | ForEach-Object { [string] $_.version })
    return @($released | Where-Object { $_ -cne $Building -and $listed -cnotcontains $_ })
}
$treeVersion = [regex]::Match([IO.File]::ReadAllText((Join-Path $desktop 'CMakeLists.txt')), 'project\(QkvmDesktop VERSION (\d+\.\d+\.\d+)').Groups[1].Value
Check ($treeVersion -cmatch '\A\d+\.\d+\.\d+\z') "the version this tree builds is read from CMakeLists.txt ($treeVersion)"
$unlisted = @(Get-UnlistedReleases (Join-Path $desktop 'CHANGELOG.md') (Join-Path $desktop 'packaging/published-files.json') $treeVersion)
Check ($unlisted.Count -eq 0) "packaging/published-files.json names every version CHANGELOG.md says was released before $treeVersion (it lacks: $($unlisted -join ', '))"
# (And the look is seen to refuse: a made-up CHANGELOG with one release more, one not released yet, which is not, and
# the version being built, which is not either, but is where another version is being built.)
$madeChangelog = Join-Path $work 'CHANGELOG.md'
[IO.File]::WriteAllText($madeChangelog, "# Changes`n`n## 0.4.0 (beta)`n`n- w`n`n## 0.3.0 (not released yet)`n`n- x`n`n## 0.2.0`n`n- y`n`n## 0.1.0 (beta)`n`n- z`n")
$madeList = New-PublishedList 'published-for-changelog' @((Of '0.1.0' @('a.dll')))
$madeUnlisted = @(Get-UnlistedReleases $madeChangelog $madeList '0.4.0')
Check (($madeUnlisted -join ',') -ceq '0.2.0') "a release CHANGELOG.md names that the list lacks is found, and neither one not released yet nor the one being built is ($($madeUnlisted -join ', '))"
$madeUnlisted = @(Get-UnlistedReleases $madeChangelog $madeList '0.5.0')
Check (($madeUnlisted -join ',') -ceq '0.4.0,0.2.0') "a release that is not the one being built is held to the list ($($madeUnlisted -join ', '))"

# build_setup.ps1's refusals.
$extra = New-FixturePackage 'unrecorded' $fixture
[IO.File]::WriteAllText((Join-Path $extra 'stray.dll'), 'not in the manifest')
Expect-Refusal $extra 'not in its manifest: stray\.dll' 'a package with a file its manifest does not name is refused'
$changed = New-FixturePackage 'changed' $fixture
[IO.File]::AppendAllText((Join-Path $changed 'Qt6Core.dll'), 'x')
Expect-Refusal $changed 'not the bytes its manifest names: Qt6Core\.dll' 'a package with a file of other bytes is refused'
$missing = New-FixturePackage 'missing' $fixture
Remove-Item -LiteralPath (Join-Path $missing 'Qt6Core.dll')
Expect-Refusal $missing 'lacks Qt6Core\.dll' 'a package without a file its manifest names is refused'
$braces = New-FixturePackage 'braces' ($fixture + @{ 'qml/{app}/x.qml' = 'a name Inno Setup reads as a constant' })
Expect-Refusal $braces 'cannot be named to Inno Setup' 'a name Inno Setup would read as something else is refused'
$quote = New-FixturePackage 'quote' ($fixture + @{ "qml/it's.qml" = 'a quote' })
Expect-Refusal $quote 'cannot be named to Inno Setup' 'a name with a quote is refused'
$used = New-FixturePackage 'used' $fixture
New-Item -ItemType Directory -Path (Join-Path $work 'used/out') | Out-Null
[IO.File]::WriteAllText((Join-Path $work 'used/out/old.txt'), 'old')
Expect-Refusal $used 'is not empty' 'an output folder with something in it is refused'
$otherInno = Join-Path $work 'other-inno'
Copy-Item -LiteralPath $Inno -Destination $otherInno -Recurse
[IO.File]::AppendAllText((Join-Path $otherInno 'Default.isl'), "`r`n; changed")
Expect-Refusal (New-FixturePackage 'other-inno' $fixture) 'not the pinned Inno Setup' 'an Inno Setup other than the pinned one is refused' -With $otherInno
# (And a list of what published versions installed that names what no package of ours can have, or is no such list.)
$outside = New-PublishedList 'published-outside' @((Of '0.0.9' @('qkvm-desktop.exe', '../outside.dll')))
Expect-Refusal (New-FixturePackage 'published-outside' $fixture) 'names a file that cannot be named to Inno Setup as it is: \.\./outside\.dll' 'a list of what published versions installed that names a file outside the program''s folder is refused' -Published $outside
$quoted = New-PublishedList 'published-quote' @((Of '0.0.9' @("it's.dll")))
Expect-Refusal (New-FixturePackage 'published-quote' $fixture) 'cannot be named to Inno Setup' 'a list of what published versions installed that names a file with a quote is refused' -Published $quoted
$notOne = Join-Path $work 'published-not-one.json'
Set-Content -LiteralPath $notOne -Value '{"schema": 2, "versions": []}' -Encoding utf8
Expect-Refusal (New-FixturePackage 'published-not-one' $fixture) 'is not a list of what each published version installed' 'a list of another shape is refused' -Published $notOne
$twice = New-PublishedList 'published-twice' @((Of '0.0.9' @('a.dll')), (Of '0.0.9' @('b.dll')))
Expect-Refusal (New-FixturePackage 'published-twice' $fixture) 'names version 0\.0\.9 twice' 'a list that names a version twice is refused' -Published $twice
$notAProgram = Join-Path $work 'not-a-program.exe'
[IO.File]::WriteAllText($notAProgram, 'a text, not a program')
Expect-Refusal (New-FixturePackage 'check-not-a-program' $fixture) 'is not a Windows program' 'an update check that is no Windows program is refused' -Check $notAProgram
Expect-Refusal (New-FixturePackage 'check-none' $fixture) 'is not a file of qKVM Setup''s update check' 'an update check that is not there is refused' -Check (Join-Path $work 'no-such-check.exe')
$badVersion = New-PublishedList 'published-version' @((Of 'next' @('a.dll')))
Expect-Refusal (New-FixturePackage 'published-version' $fixture) 'a version that is not one' 'a list with a version that is none is refused' -Published $badVersion
# (Schema 1, before Windows on Arm: refused, no build of either kind said.)
$schemaOne = Join-Path $work 'published-schema-one.json'
Set-Content -LiteralPath $schemaOne -Value '{"schema": 1, "versions": [{"version": "0.0.9", "files": ["a.dll"]}]}' -Encoding utf8
Expect-Refusal (New-FixturePackage 'published-schema-one' $fixture) 'is not a list of what each published version installed' 'a list of the shape before Windows on Arm is refused' -Published $schemaOne
$kindTwice = New-PublishedList 'published-kind-twice' @([ordered]@{ version = '0.0.9'; builds = @([ordered]@{ architecture = 'x64'; files = @('a.dll') }, [ordered]@{ architecture = 'x64'; files = @('b.dll') }) })
Expect-Refusal (New-FixturePackage 'published-kind-twice' $fixture) 'not one of x64 or arm64, or names one twice' 'a list that names a build of one kind twice is refused' -Published $kindTwice
$kindOther = New-PublishedList 'published-kind-other' @([ordered]@{ version = '0.0.9'; builds = @([ordered]@{ architecture = 'aarch64'; files = @('a.dll') }) })
Expect-Refusal (New-FixturePackage 'published-kind-other' $fixture) 'not one of x64 or arm64' 'a list with a build of another kind is refused' -Published $kindOther
# (One setup of x64's package always, and of Windows on Arm's at most once, of one version; each update check of the
# kind of its package, one for each.)
Expect-Refusal @((New-FixturePackage 'arm-alone' $fixtureArm 'arm64')) 'carries x64''s package always' 'Windows on Arm''s package alone is refused' -Check @($fixtureCheckArm)
Expect-Refusal @((New-FixturePackage 'x64-twice' $fixture), (New-FixturePackage 'x64-twice-2' $fixture)) 'Two packages of x64' 'two packages of x64 are refused' -Check @($fixtureCheck, $fixtureCheck)
$laterArm = New-FixturePackage 'versions' $fixtureArm 'arm64'
$manifestFile = Join-Path $laterArm 'package-manifest.json'
[IO.File]::WriteAllText($manifestFile, [IO.File]::ReadAllText($manifestFile).Replace('"0.1.0"', '"0.1.1"'))
Expect-Refusal @((New-FixturePackage 'versions' $fixture), $laterArm) 'other versions' 'packages of two versions are refused' -Check @($fixtureCheck, $fixtureCheckArm)
Expect-Refusal @((New-FixturePackage 'check-kind' $fixture)) 'update check for arm64 Windows, and the setup carries no package of that kind' 'an update check of another kind than the package''s is refused' -Check @($fixtureCheckArm)
Expect-Refusal @((New-FixturePackage 'check-count' $fixture), (New-FixturePackage 'check-count' $fixtureArm 'arm64')) 'one update check for each package' 'one update check for two packages is refused' -Check @($fixtureCheck)
Expect-Refusal @((New-FixturePackage 'check-twice' $fixture), (New-FixturePackage 'check-twice' $fixtureArm 'arm64')) 'Two update checks for x64' 'two update checks of one kind are refused' -Check @($fixtureCheck, $fixtureCheck)
# (Each update check built with what tools/image-protections.json asks of our own programs of its kind, as every image of
# a package is: x64's with its shadow stack's mark, and ARM64's with CFG whatever its share of signed returns, which the
# static runtime's pieces make enough without /guard:signret; review r2 of arm and armp, L1.)
$checkWithoutCet = New-FixtureCheck 'update-check-without-cet.exe' 0x8664 @('cet')
Expect-Refusal (New-FixturePackage 'check-without-cet' $fixture) "update-check-without-cet\.exe is not built with what its table asks: qkvm-update-check\.exe lacks cet, which every image of qKVM Desktop's own has on x64" 'an x64 update check without its shadow stack''s mark is refused' -Check @($checkWithoutCet)
$checkArmWithoutCfg = New-FixtureCheck 'update-check-arm64-without-cfg.exe' 0xAA64 @('cfg')
Expect-Refusal @((New-FixturePackage 'check-arm-without-cfg' $fixture), (New-FixturePackage 'check-arm-without-cfg' $fixtureArm 'arm64')) "update-check-arm64-without-cfg\.exe is not built with what its table asks: qkvm-update-check-arm64\.exe lacks cfg, which every image of qKVM Desktop's own has on arm64" 'Windows on Arm''s update check without CFG is refused' -Check @($fixtureCheck, $checkArmWithoutCfg)
$checkArmWithoutPac = New-FixtureCheck 'update-check-arm64-without-pac.exe' 0xAA64 @('pac')
Expect-Refusal @((New-FixturePackage 'check-arm-without-pac' $fixture), (New-FixturePackage 'check-arm-without-pac' $fixtureArm 'arm64')) 'qkvm-update-check-arm64\.exe lacks pac' 'Windows on Arm''s update check without its signed returns is refused' -Check @($fixtureCheck, $checkArmWithoutPac)
$mixedArm = New-FixturePackage 'mixed' ($fixtureArm + @{ 'Qt6Gui.dll' = 'x' }) 'arm64'
[IO.File]::Copy($fixtureCheck, (Join-Path $mixedArm 'Qt6Gui.dll'), $true)
$mixedManifest = Get-Content -LiteralPath (Join-Path $mixedArm 'package-manifest.json') -Raw | ConvertFrom-Json
foreach ($entry in $mixedManifest.files | Where-Object { $_.path -ceq 'Qt6Gui.dll' }) { $entry.bytes = (Get-Item -LiteralPath (Join-Path $mixedArm 'Qt6Gui.dll')).Length; $entry.sha256 = Get-Sha256 (Join-Path $mixedArm 'Qt6Gui.dll') }
$mixedManifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $mixedArm 'package-manifest.json') -Encoding utf8
Expect-Refusal @((New-FixturePackage 'mixed' $fixture), $mixedArm) 'for arm64 Windows holds a program for x64: Qt6Gui\.dll' 'a package for Arm holding a program for x64 is refused' -Check @($fixtureCheck, $fixtureCheckArm)

# --- Then the setup named.
if ($SetupDirectory) {
    $setup = (Resolve-Path -LiteralPath $SetupDirectory).Path
    $packages = @($PackageDirectory | ForEach-Object { $_ -split ',' } | Where-Object { $_ } | ForEach-Object { (Resolve-Path -LiteralPath $_).Path })
    if (-not $PublishedFiles) { $PublishedFiles = Join-Path $desktop 'packaging/published-files.json' }
    $said = @(Get-SetupFailures $setup $packages $PublishedFiles)
    foreach ($line in $said) { Check $false $line }
    if (-not $said.Count) { Write-Output "The setup in $setup is the packages in $($packages -join ', '), for the user alone, with the program's mutex and the marker." }
}

if ($failures) { Write-Host "$failures setup check(s) failed."; exit 1 }
Remove-Item -LiteralPath $work -Recurse -Force
Write-Output 'Setup checks passed: a package made into a setup passes, and every way of being wrong that is tried fails.'
