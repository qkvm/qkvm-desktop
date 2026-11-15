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
  ends (the one start of a program in [Code]: the program, with --after-update); nothing that runs before the Ready
  page reaches {app}, read as Pascal Script reads [Code] (a name in any case, an indented or <event>'s routine,
  {code:} in a string, a constant) and with what [Setup], [Tasks] and [Files] ask by {code:} and Check:;
- the marker is "setup" and a line feed, put at {app}\qkvm-desktop.installed;
- the fixed AppId, publisher qKVM, the package's version, x64, Windows 10 22H2, lzma2 and solid, no closing of
  programs, the user's own Programs folder;
- Windows' junction guard left on (RedirectionGuard is never turned off); before an install nothing deleted but
  {app}\plugins and {app}\qml, which a later version may hold less of and Qt loads whole; the folder never chosen
  in the wizard (DisableDirPage=yes); the calls outside Pascal Script each as declared, ExitProcess and those that
  close a running copy (for Setup alone), and no other;
- the program's own file the last one put in (files.iss's last line, after the marker); in every mode it is set
  aside before any file is written, deleted after a whole copy, put back when the copy did not complete;
- after a whole copy only what the package's own list (kept.iss) has not is removed from plugins and qml: neither
  folder when it is itself a redirection, no redirection in them, no name ending in a dot or a blank; after a whole
  copy and at an uninstall, what a setup ended halfway left in the package's own folders (by its list, folders.iss,
  made at build time and never read from the folder; Inno Setup's temporary is-XXXXXXXXXX.tmp files: that very
  form, files only, no folder that is or is reached through a redirection), and at an uninstall the program an
  update ended halfway set aside; nothing else;
- nothing is run but the program itself, and nothing asks the network;
- where build_setup.ps1 made the uncompressed twin for the byte scan: it holds the same files, it is readable (the
  mutex's name is seen in it), and neither it nor the setup names the folders the setup was made from.
Every failed check is a "FAIL:" line, and the script then fails.
#>
param(
    [string] $SetupDirectory = '',
    [string] $PackageDirectory = '',
    [string] $Inno = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\build_common.ps1"

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

function Get-SetupFailures([string] $Setup, [string] $Package) {
    <# What is wrong with the setup build_setup.ps1 made in $Setup from $Package: nothing, or one line for each. #>
    $said = [Collections.Generic.List[string]]::new()
    $manifest = Get-Content -LiteralPath (Join-Path $Package 'package-manifest.json') -Raw | ConvertFrom-Json
    $version = [string] $manifest.version
    $name = "qKVM-Desktop-$version-windows-x64-setup.exe"
    $exe = Join-Path $Setup $name
    $inputs = Join-Path $Setup 'inputs'
    $log = Join-Path $Setup 'compile.txt'

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

    # --- What the compiler put in: the package, file for file, the manifest and the marker; nothing else.
    $expected = @{}
    foreach ($entry in $manifest.files) {
        $expected[(Join-Path $Package ($entry.path -replace '/', '\')).ToLowerInvariant()] = [ordered]@{ path = $entry.path; sha256 = $entry.sha256; bytes = [long] $entry.bytes }
    }
    $manifestFile = Join-Path $Package 'package-manifest.json'
    $expected[$manifestFile.ToLowerInvariant()] = [ordered]@{ path = 'package-manifest.json'; sha256 = Get-Sha256 $manifestFile; bytes = (Get-Item -LiteralPath $manifestFile).Length }
    $markerSource = Join-Path $inputs 'qkvm-desktop.installed'
    $expected[$markerSource.ToLowerInvariant()] = [ordered]@{ path = 'qkvm-desktop.installed'
        sha256 = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($MarkerBytes)).ToLowerInvariant(); bytes = $MarkerBytes.Length }
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
    # files.iss: each of the package's files at its own place, and nothing more.
    $lines = @(Get-Content -LiteralPath (Join-Path $inputs 'files.iss') | Where-Object { $_.Trim() })
    $placed = @{}
    foreach ($line in $lines) {
        if ($line -cnotmatch '\ASource: "([^"{}]+)"; DestDir: "\{app\}((?:\\[^"\\{}]+)*)"; Flags: ignoreversion\z') { $said.Add("files.iss has a line that is not one file at its own place: $line"); continue }
        $relative = $Matches[1]
        $folder = $Matches[2]
        $wanted = Split-Path -Parent $relative
        if ($folder -cne $(if ($wanted) { "\$wanted" } else { '' })) { $said.Add("files.iss puts $relative elsewhere than its own place ({app}$folder)") }
        $placed[(Join-Path $Package $relative).ToLowerInvariant()] = $true
    }
    foreach ($source in $expected.Keys | Where-Object { $_ -cne $markerSource.ToLowerInvariant() }) {
        if (-not $placed.ContainsKey($source)) { $said.Add("files.iss does not put $($expected[$source].path) in the program's folder") }
    }
    if ($lines.Count -ne $expected.Count - 1) { $said.Add("files.iss has $($lines.Count) lines for $($expected.Count - 1) files") }
    # (The program's own file last: whatever starts it during the copy finds it beside the libraries it was built with.)
    if ($lines.Count -and $lines[-1] -cne 'Source: "qkvm-desktop.exe"; DestDir: "{app}"; Flags: ignoreversion') { $said.Add("the program's file is not the last one files.iss puts in") }

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
        'OpenReadWriteExclusive = $12;', 'TFileStream.Create(', '  else if Free < Needed then', "      if Busy.Count > 0 then begin",
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
        "  Reason := GateReason;`n  if Reason = '' then`n    Reason := RoomReason;`n  if (Reason = '') and WizardSilent and not UpdateMode then`n    Reason := CloseProgramSilently;`n  if Reason = '' then begin",
        # (Nothing closed and nothing changed where this setup holds no gate: its mutex not there to be opened, or its
        # event not made; review F5.)
        "  if GateReason <> '' then`n    Exit;`n  // (Nothing closed where there is no room",
        "  Gate := OpenMutexW(MutexSynchronize, 0, SetupGateName);`n  if Gate <> 0 then`n    CloseHandle(Gate);`n  if (Gate = 0) or (SetupRuns = 0) then`n    Result := 'this setup could not make what keeps qKVM Desktop from starting while it works';",
        '  MutexSynchronize = $00100000;',
        "    if ClosedIt then begin`n      StartAtEnd := True;`n      Reason := Reason + '; qKVM Desktop, which this setup had closed, is started again';",
        "  if StartAtEnd then`n    StartAgain;`nend;",
        "  if RoomReason <> '' then`n    Exit;`n  if TaskDialogMsgBox('qKVM Desktop is running',",
        "  if not GetSpaceOnDisk64(RoomFolder, Free, Total) then`n    Result := 'the free room on the drive of qKVM Desktop could not be told'",
        # (What is installed and what this does with it, on the first page; on an upgrade no page asks again.)
        "WizardForm.WelcomeLabel2.Caption := Format('qKVM Desktop %s is installed. This upgrades it to %s and keeps your wall, settings and passwords.', [InstalledVersion, '{#AppVersion}']);",
        "  InstalledVersion := Installed;", "  Result := (PageID = wpSelectTasks) and (InstalledVersion <> '');",
        # (The last page's tick, and the start at the end of an update or of a silent install that closed the program.)
        "  Result := MayStartProgram and not UpdateMode and ((InstalledVersion = '') or ClosedIt);", "  Result := MayStartProgram and not UpdateMode and not StartTicked;",
        "  Result := MayStartProgram and (UpdateMode or (WizardSilent and ClosedIt));")) {
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
    $runsAt = [regex]::Match($code, '(?s)\nfunction InitializeSetup: Boolean;\nvar\n.*?\nbegin\n  Result := True;\n(.*?)\n  if not RegQueryStringValue\(')
    if (-not $setupMutexNamed.Success -or $setupMutexNamed.Groups[1].Value.Replace('\\', '\') -cne 'Local\qKVM-Desktop-Setup' -or
        $code -cnotmatch "\n  SetupRunsName = '$([regex]::Escape($setupMutexNamed.Groups[1].Value.Replace('\\', '\')))-';\n" -or -not $runsAt.Success -or
        -not $runsAt.Groups[1].Value.EndsWith("  SetupRuns := CreateEventW(0, 1, 0, SetupRunsName + IntToStr(GetCurrentProcessId));")) {
        $said.Add('the setup does not say it is at work for as long as it runs, as the program tells a Setup that runs')
    }
    # The uninstaller refuses while the program runs, by the program's own mutex, looked at itself: Inno Setup keeps
    # nothing of AppMutex for it after an install made outside update mode (the first VM suite's step 25; review V2).
    $uninstall = [regex]::Match($code, '(?s)\nfunction InitializeUninstall: Boolean;\nbegin\n(.*?)\nend;')
    # (And from its first moment it holds the gate a start honours, as a setup's: Setup's mutex and its own event that
    # says it is at work, for the whole uninstall; and it looks again right before anything is removed, a best effort;
    # review R4. It refuses as it begins, where the uninstall is decided, while a setup is open (Setup's mutex there
    # already) and where it cannot make its gate or its event; review F4.)
    if ($code -cnotmatch "\n  ProgramMutexName = '$([regex]::Escape($ProgramMutex))';\n" -or -not $uninstall.Success -or
        ((@($uninstall.Groups[1].Value -split "`n" | Where-Object { -not $_.Trim().StartsWith('//') })) -join "`n") -cne "  Result := True;`n  UninstallGate := CreateMutexW(0, 0, SetupGateName);`n  if (UninstallGate <> 0) and (DLLGetLastError = ErrorAlreadyExists) then begin`n    Log('A setup of qKVM Desktop, or another uninstall of it, is at work: it is not uninstalled. Let that finish, then uninstall it again.');`n    if not UninstallSilent then`n      MsgBox('qKVM Desktop Setup, or another uninstall of qKVM Desktop, is at work. Let it finish, then uninstall qKVM Desktop again.', mbError, MB_OK);`n    Result := False;`n    Exit;`n  end;`n  UninstallRuns := CreateEventW(0, 1, 0, SetupRunsName + IntToStr(GetCurrentProcessId));`n  if (UninstallGate = 0) or (UninstallRuns = 0) then begin`n    Log('qKVM Desktop could not be kept from starting while it is uninstalled: it is not uninstalled.');`n    if not UninstallSilent then`n      MsgBox('qKVM Desktop could not be kept from starting while it is uninstalled, so it is not uninstalled. Uninstall it again later.', mbError, MB_OK);`n    Result := False;`n    Exit;`n  end;`n  if CheckForMutexes(ProgramMutexName) then begin`n    Log('qKVM Desktop is running: it is not uninstalled. Quit it first, then uninstall it again.');`n    if not UninstallSilent then`n      MsgBox('qKVM Desktop is running. Quit it first: right-click the qKVM icon in the notification area, then choose Quit. Then uninstall it again.', mbError, MB_OK);`n    Result := False;`n  end;" -or
        $code -cnotmatch "\n  SetupGateName = '$([regex]::Escape($setupMutexNamed.Groups[1].Value.Replace('\\', '\')))';\n" -or
        -not $code.Contains("  if CurUninstallStep = usUninstall then begin`n    // (Looked at again right before anything is removed, as a best effort: a copy that started all the same, one before`n    // 0.1.2 that does not honour the gate among them, is said in the log, and the exception ends this step; Inno`n    // Setup's help says that it ends the uninstall there, which is for the VM to show, and nothing here rests on it.`n    // A start in the instant between this look and the removal of the files is not stopped: a known limit`n    // (SECURITY.md). The refusal that decides is InitializeUninstall's; reviews R4, F4.)`n    if CheckForMutexes(ProgramMutexName) then begin`n      Log('qKVM Desktop started while it was being uninstalled: it is not uninstalled. Quit it first, then uninstall it again.');`n      RaiseException('qKVM Desktop is running. Quit it first, then uninstall it again.');`n    end;`n    RemoveSetupLeftovers;")) {
        $said.Add('the uninstaller does not refuse while the program runs, by the program''s own mutex')
    }
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
            'DeinitializeSetup' = "begin`n  if SetAside and not PostInstalled then`n    if not FileExists(ProgramPath) then`n      PutProgramBack;`n  if StartAtEnd then`n    StartAgain;`nend;"
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
        -not $prepare.Value.Contains("      FilesInUse(ExpandConstant('{app}'), Busy, Unlisted);`n      Began := GetTickCount;`n      while (Busy.Count > 0) and (TicksSince(Began) < LookMs - UpdateStepMs) do begin`n        Sleep(UpdateStepMs);`n        for I := Busy.Count - 1 downto 0 do begin`n          if TicksSince(Began) >= LookMs then`n            Break;`n          Path := Busy[I];`n          if Unlisted.IndexOf(Path) >= 0 then begin`n            Busy.Delete(I);`n            Unlisted.Delete(Unlisted.IndexOf(Path));`n            FilesInUse(Path, Busy, Unlisted);`n          end else if FreeToReplace(Path) then`n            Busy.Delete(I);`n        end;`n      end;`n      if Busy.Count > 0 then begin")) {
        $said.Add('Preparing to Install does not look at room and files in use in every mode')
    }
    # (The first look goes through the whole folder, however long it takes, and only the looks again count 2 s,
    # review F1: a slow disk is no reason to refuse.)
    $walk = [regex]::Match($codeOnly, '(?s)\nprocedure FilesInUse\(.*?\nend;')
    if (-not $walk.Success -or $walk.Value -match '\b(TicksSince|GetTickCount|LookMs)\b') {
        $said.Add('the first look at the files in use stops before it has looked at every file')
    }
    # The program's own file set aside there, last, and nowhere else: a failure is a refusal, with nothing copied
    # (review C7).
    if (-not $prepare.Success -or -not $prepare.Value.Contains("  if Reason = '' then`n    Reason := SetProgramAside;`n  if Reason <> '' then begin") -or
        ([regex]::Matches($codeOnly, '\bSetProgramAside\b')).Count -ne 2) {
        $said.Add('the program''s own file is not set aside last in Preparing to Install, where a failure stops the install')
    }
    $files = @($sections['Files'] | ForEach-Object { $_.Trim() } | Where-Object { $_ -and -not $_.StartsWith(';') })
    if ($files -cnotcontains 'Source: "{#SourcePath}qkvm-desktop.installed"; DestDir: "{app}"; Flags: ignoreversion' -or
        $files -cnotcontains '#include "files.iss"' -or $files.Count -ne 2 -or $files[-1] -cne '#include "files.iss"') {
        $said.Add('the [Files] are not the package and the marker at {app}\qkvm-desktop.installed')
    }
    if (-not (Test-Path -LiteralPath $markerSource) -or (Get-Sha256 $markerSource) -cne $expected[$markerSource.ToLowerInvariant()].sha256) {
        $said.Add('the marker is not "setup" and a line feed')
    }
    # --- The rest of what it is.
    foreach ($line in "AppId={{A964EFA6-87C6-4A23-A9CC-B71F87441433}", 'AppPublisher=qKVM', 'AppVersion={#AppVersion}',
        'ArchitecturesAllowed=x64os', 'ArchitecturesInstallIn64BitMode=x64os', 'MinVersion=10.0.19045', 'CloseApplications=no', 'SetupMutex=qKVM-Desktop-Setup', 'DisableDirPage=yes',
        'DisableWelcomePage=no',
        'DefaultDirName={userpf}\qKVM Desktop', 'UninstallDisplayIcon={app}\qkvm-desktop.exe',
        'OutputBaseFilename=qKVM-Desktop-{#AppVersion}-windows-x64-setup', 'SetupIconFile={#SourcePath}qkvm.ico') {
        if ($setupLines -cnotcontains $line) { $said.Add("the [Setup] section does not say $line") }
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
    # not; and started again at the end of an update and of a silent install that closed it.)
    $wantedRun = @('Filename: "{app}\qkvm-desktop.exe"; Parameters: "--after-update"; Description: "Start qKVM Desktop"; Flags: nowait postinstall skipifsilent; Check: StartTicked',
        'Filename: "{app}\qkvm-desktop.exe"; Parameters: "--after-update"; Description: "Start qKVM Desktop"; Flags: nowait postinstall skipifsilent unchecked; Check: StartUnticked',
        'Filename: "{app}\qkvm-desktop.exe"; Parameters: "--after-update"; Flags: nowait runasoriginaluser; Check: StartsAgain')
    if (($run -join "`n") -cne ($wantedRun -join "`n")) { $said.Add('something else than the program is run, or the program otherwise') }
    # Windows' junction guard stays on for the setup and the uninstall, as Inno Setup has it unless told otherwise.
    if (@($setupLines | Where-Object { $_ -imatch '^RedirectionGuard\s*=\s*no\b' }).Count) { $said.Add('the setup turns RedirectionGuard off') }
    # Nothing deleted before the copy; after a whole copy only what an earlier version left in the plugin and QML
    # folders (Qt loads them whole) and this package has not: by the package's own list, made into the setup at build
    # time (kept.iss, from package-manifest.json), never read from the folder; nothing outside the two folders.
    $kept = @(Get-Content -LiteralPath (Join-Path $inputs 'kept.iss') | ForEach-Object {
        if ($_ -cmatch "\A  Result := Result \+ '([^']+)\|';\z") { $Matches[1] } })
    $packaged = @($manifest.files | ForEach-Object { [string] $_.path } | Where-Object { $_ -cmatch '\A(plugins|qml)/' } |
        ForEach-Object { ($_ -replace '/', '\').ToLowerInvariant() })
    if ((@($kept | Sort-Object -CaseSensitive) -join "`n") -cne (@($packaged | Sort-Object -CaseSensitive) -join "`n")) {
        $said.Add("the setup's list of what it keeps in plugins and qml is not the package's ($($kept.Count) for $($packaged.Count))")
    }
    $keptText = [IO.File]::ReadAllText((Join-Path $inputs 'kept.iss')) -replace "`r`n", "`n"
    if (-not $keptText.Contains("function KeptFiles: String;`nbegin`n  Result := '|';`n") -or -not $keptText.TrimEnd().EndsWith("`nend;") -or
        @(($keptText -split "`n") | Where-Object { $_ -and -not $_.StartsWith('//') -and $_ -cnotmatch "\A(  Result := Result \+ '[^']+\|';|function KeptFiles: String;|begin|  Result := '\|';|end;)\z" }).Count) {
        $said.Add('kept.iss is not the one function that says the kept files and nothing else')
    }
    # What a setup ended halfway left is looked for only in the package's own folders: by the package's own list, made
    # into the setup at build time (folders.iss, from package-manifest.json), never read from the folder.
    $foldersFile = Join-Path $inputs 'folders.iss'
    $listedFolders = @(if (Test-Path -LiteralPath $foldersFile) { Get-Content -LiteralPath $foldersFile | ForEach-Object {
        if ($_ -cmatch "\A  Result := Result \+ '([^']*)\|';\z") { $Matches[1] } } })
    $ownFolders = [Collections.Generic.SortedSet[string]]::new([StringComparer]::Ordinal)
    [void] $ownFolders.Add('')
    foreach ($path in @($manifest.files | ForEach-Object { [string] $_.path })) { [void] $ownFolders.Add((Split-Path -Parent ($path -replace '/', '\'))) }
    if (($listedFolders -join "`n") -cne (@($ownFolders) -join "`n")) {
        $said.Add("the setup's list of the package's folders is not the package's ($($listedFolders.Count) for $($ownFolders.Count))")
    }
    $foldersText = if (Test-Path -LiteralPath $foldersFile) { [IO.File]::ReadAllText($foldersFile) -replace "`r`n", "`n" } else { '' }
    if (-not $foldersText.Contains("function PackageFolders: String;`nbegin`n  Result := '|';`n") -or -not $foldersText.TrimEnd().EndsWith("`nend;") -or
        @(($foldersText -split "`n") | Where-Object { $_ -and -not $_.StartsWith('//') -and $_ -cnotmatch "\A(  Result := Result \+ '[^']*\|';|function PackageFolders: String;|begin|  Result := '\|';|end;)\z" }).Count) {
        $said.Add('folders.iss is not the one function that says the package''s folders and nothing else')
    }
    foreach ($part in @('#include "kept.iss"', 'procedure RemoveStale(Folder, Relative, Kept: String);',
        "        end else if Pos('|' + Lowercase(Name) + '|', Kept) = 0 then begin", '          if DeleteFile(Path) then',
        '          if RemoveDir(Path) then', '  if CurStep = ssPostInstall then begin',
        "        else if (Found.Attributes and ReparsePointAttribute) <> 0 then`n          Log('Left as it is, a redirection: ' + Name)",
        # (A name ending in a dot or a blank is left alone: Windows reads it as the name without them, a kept file.)
        "        if (Copy(Found.Name, Length(Found.Name), 1) = '.') or (Copy(Found.Name, Length(Found.Name), 1) = ' ') then`n          Log('Left as it is, a name Windows reads as another: ' + Name)",
        # (Either folder, when it is itself a redirection, is not looked at at all.)
        'procedure RemoveStaleIn(Relative: String);', '  Redirected := (Found.Attributes and ReparsePointAttribute) <> 0;',
        "  if Redirected then`n    Log('Left as it is, a redirection: ' + Relative)`n  else`n    RemoveStale(Folder, Relative, KeptFiles);",
        "    RemoveStaleIn('plugins');", "    RemoveStaleIn('qml');",
        # In every mode (review L3) the program's file is set aside before any file is written, so that a start during the
        # copy finds no program beside half-copied libraries; deleted after a whole copy, put back when the copy did not
        # complete.
        # (Set aside last in Preparing to Install, where a failure stops the install with nothing copied, the program
        # started again where this setup closed it; review C7.)
        "  Result := ExpandConstant('{app}\qkvm-desktop.exe.old-update');", "  if CurStep = ssInstall then begin`n    Installing := True;",
        # (What a refusal left to start at the end goes as the copy begins: never a start the person did not tick, after a
        # try again that went on; review C8.)
        "    StartAtEnd := False;`n  end;",
        "    PostInstalled := True;",
        "  if not FileExists(ProgramPath) then`n    Exit;`n  SetAside := RenameFile(ProgramPath, ProgramAside);`n  if SetAside then`n    Log('Set the earlier program aside for the copy.')`n  else`n    Result := 'the program''s own file could not be set aside for the copy (qkvm-desktop.exe)';",
        "    if SetAside then`n      DropAside;", 'procedure DeinitializeSetup;',
        # What a setup ended halfway left in the folder and every folder in it (Inno Setup's temporary files, is- and ten
        # letters or digits and .tmp, files only, no redirection followed), removed after a whole copy and at an uninstall,
        # with the program an update ended halfway set aside; nothing else.
        'function SetupLeftover(Name: String): Boolean;',
        "  Result := (Length(Name) = 17) and (Lowercase(Copy(Name, 1, 3)) = 'is-') and (Lowercase(Copy(Name, 14, 4)) = '.tmp');",
        "  for I := 4 to 13 do`n    if Result and (Pos(Uppercase(Copy(Name, I, 1)), 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789') = 0) then`n      Result := False;",
        '#include "folders.iss"', 'procedure RemoveSetupLeftoversIn(Relative: String);',
        "  if ThroughRedirection(Relative) then begin`n    Log('Left as it is, a redirection: ' + Relative);`n    Exit;`n  end;",
        "  Folder := ExpandConstant('{app}');`n  if Relative <> '' then`n    Folder := Folder + '\' + Relative;`n  if not FindFirst(Folder + '\is-*.tmp', Found) then",
        "      if ((Found.Attributes and (DirectoryAttribute or ReparsePointAttribute)) = 0) and SetupLeftover(Found.Name) then begin`n        Entry := Folder + '\' + Found.Name;`n        if DeleteFile(Entry) then",
        "          Log('Could not remove what a setup ended halfway left: ' + Entry);`n      end;`n    until not FindNext(Found);",
        'function ThroughRedirection(Relative: String): Boolean;',
        "  Path := ExpandConstant('{app}');`n  Rest := Relative;`n  while (Rest <> '') and not Result do begin",
        "    Path := Path + '\' + Copy(Rest, 1, At - 1);`n    Rest := Copy(Rest, At + 1, Length(Rest));",
        "    if FindFirst(Path, Found) then begin`n      Result := (Found.Attributes and ReparsePointAttribute) <> 0;",
        "  Rest := PackageFolders;`n  Rest := Copy(Rest, 2, Length(Rest));`n  At := Pos('|', Rest);`n  while At > 0 do begin`n    RemoveSetupLeftoversIn(Copy(Rest, 1, At - 1));`n    Rest := Copy(Rest, At + 1, Length(Rest));`n    At := Pos('|', Rest);`n  end;",
        # (The install step's and the stale walk's own lines: misnamed, or cut after the first entry, they would
        # silently do nothing.)
        'procedure CurStepChanged(CurStep: TSetupStep);',
        "            Log('Could not remove what this version does not have: ' + Name);`n        end;`n      end;`n    until not FindNext(Found);",
        "    RemoveStaleIn('qml');`n    RemoveSetupLeftovers;",
        'procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);',
        "    RemoveSetupLeftovers;`n    DropAside;`n  end;",
        "  if SetAside and not PostInstalled then`n    if not FileExists(ProgramPath) then`n      PutProgramBack;")) {
        if (-not $code.Contains($part)) { $said.Add("the removal after the copy lacks: $part") }
    }
    # (Deleted: a stale file, what a setup ended halfway left, and the program's own copy set aside; renamed: that copy,
    # aside and back; nothing else.)
    $deletes = (@([regex]::Matches($code, 'DeleteFile\([^)]*\)') | ForEach-Object Value) | Sort-Object -CaseSensitive) -join '|'
    $renames = (@([regex]::Matches($code, 'RenameFile\([^)]*\)') | ForEach-Object Value) | Sort-Object -CaseSensitive) -join '|'
    if ($deletes -cne 'DeleteFile(Entry)|DeleteFile(Path)|DeleteFile(ProgramAside)' -or
        $renames -cne 'RenameFile(ProgramAside, ProgramPath)|RenameFile(ProgramPath, ProgramAside)' -or
        ([regex]::Matches($code, 'RemoveDir\(')).Count -ne 1) {
        $said.Add("the [Code] section deletes or renames otherwise than the removal after the copy, what a setup ended halfway left and the program set aside ($deletes; $renames)")
    }
    # The one result line the program reads in the log when the setup ends with 105 or 7 (it cannot see the exit code):
    # the 105 line right before ExitProcess, so it is in the log before the process ends.
    $codeLines = @($sections['Code'] | ForEach-Object { $_.TrimEnd() } | Where-Object { $_.Trim() -and -not $_.Trim().StartsWith('//') })
    $at = [Array]::IndexOf($codeLines, "    Log('qKVM-RESULT: 105 the program was still running after 60 seconds; nothing was changed');")
    if ($at -lt 0 -or $codeLines[$at + 1] -cne '    ExitProcess(StillRunningExitCode);') {
        $said.Add('the result line of exit code 105 is not logged right before ExitProcess')
    }
    if ($codeLines -cnotcontains "    Log('qKVM-RESULT: 7 nothing was changed: ' + Reason);") { $said.Add('the result line of exit code 7 is not logged') }
    if (([regex]::Matches($code, 'qKVM-RESULT: ')).Count -ne 2) { $said.Add('the [Code] section logs a result line otherwise than the two of the contract') }
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
        'function CloseHandle(Handle: THandle): Cardinal;' = "  external 'CloseHandle@kernel32.dll stdcall setuponly';"
        'function OpenProcess(Access, Inherit, ProcessId: Cardinal): THandle;' = "  external 'OpenProcess@kernel32.dll stdcall setuponly';"
        'function QueryFullProcessImageNameW(Process: THandle; Flags: Cardinal; Name: String; var Size: Cardinal): Cardinal;' = "  external 'QueryFullProcessImageNameW@kernel32.dll stdcall setuponly';"
        'function WaitForSingleObject(Handle: THandle; Milliseconds: Cardinal): Cardinal;' = "  external 'WaitForSingleObject@kernel32.dll stdcall setuponly';"
        'function TerminateProcess(Process: THandle; ExitCode: Cardinal): Cardinal;' = "  external 'TerminateProcess@kernel32.dll stdcall setuponly';"
        'function FindWindowExW(Parent, After: HWND; ClassName: String; WindowName: Cardinal): HWND;' = "  external 'FindWindowExW@user32.dll stdcall setuponly';"
        'function GetWindowThreadProcessId(Window: HWND; var ProcessId: Cardinal): Cardinal;' = "  external 'GetWindowThreadProcessId@user32.dll stdcall setuponly';"
        'function GetWindowTextW(Window: HWND; Text: String; Most: Integer): Integer;' = "  external 'GetWindowTextW@user32.dll stdcall setuponly';"
        # (Preparing to Install's 2 s, review C10.)
        'function GetTickCount: Cardinal;' = "  external 'GetTickCount@kernel32.dll stdcall setuponly';"
        # (Whether a file found in use is there still, review R5.)
        'function GetFileAttributesW(Name: String): Integer;' = "  external 'GetFileAttributesW@kernel32.dll stdcall setuponly';"
        # (Why a folder looked through has no next entry, review F3; and whether Setup's own mutex is there, review F5.)
        'function OpenMutexW(Access, Inherit: Cardinal; Name: String): THandle;' = "  external 'OpenMutexW@kernel32.dll stdcall setuponly';"
        'function FindNextFileW(Find: THandle; Data: String): Cardinal;' = "  external 'FindNextFileW@kernel32.dll stdcall setuponly';"
    }
    $calledRight = $outside.Count -eq $declared.Count
    foreach ($prototype in $declared.Keys) {
        if (-not $code.Contains("$prototype`n$($declared[$prototype])")) { $calledRight = $false }
    }
    if (-not $calledRight) { $said.Add("the [Code] section calls outside Pascal Script otherwise than it says: $($outside -join ' | ')") }
    foreach ($section in 'InstallDelete', 'UninstallRun', 'Registry', 'INI', 'UninstallDelete', 'Dirs', 'Components') {
        if ($sections.ContainsKey($section)) { $said.Add("the script has a [$section] section") }
    }
    # (One start of a program, and only of the program: what this setup closed and then changed nothing for, started
    # again as the setup ends, as the end of a setup starts it, with --after-update; review L1.)
    $startAgain = "  if ExecAsOriginalUser(ProgramPath, '--after-update', '', SW_SHOWNORMAL, ewNoWait, Started) then"
    if (([regex]::Matches($code, [regex]::Escape($startAgain))).Count -ne 1) { $said.Add('the [Code] section does not start the program again as it should, once') }
    $withoutTheStart = $code.Replace($startAgain, '')
    foreach ($word in 'Exec', 'Download', 'CreateOleObject', 'CreateComObject', 'ExtractArchive', 'http', 'RegWrite', 'SaveStringToFile', 'DelTree') {
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
        foreach ($folder in $inputs, $Package) {
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
function New-FixturePackage([string] $Name, [hashtable] $Files) {
    $package = Join-Path $work "$Name/qKVM-Desktop-0.1.0-windows-x64"
    foreach ($file in $Files.Keys) {
        $path = Join-Path $package $file
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
        [IO.File]::WriteAllText($path, $Files[$file])
    }
    $entries = @(Get-ChildItem -LiteralPath $package -Recurse -File | Sort-Object FullName | ForEach-Object {
        [ordered]@{ path = [IO.Path]::GetRelativePath($package, $_.FullName).Replace('\', '/'); bytes = $_.Length; sha256 = Get-Sha256 $_.FullName } })
    [ordered]@{ product = 'qKVM'; version = '0.1.0'; architecture = 'x64'; files = $entries } | ConvertTo-Json -Depth 4 |
        Set-Content -LiteralPath (Join-Path $package 'package-manifest.json') -Encoding utf8
    return $package
}
$fixture = @{ 'qkvm-desktop.exe' = 'a made-up program, never run'; 'Qt6Core.dll' = 'made-up'; 'plugins/platforms/qwindows.dll' = 'made-up'
    'qml/QtQuick/Shapes/qmldir' = 'module QtQuick.Shapes'; 'licenses/AGPL-3.0.txt' = 'licence'; 'START-HERE.txt' = 'start here' }
function Invoke-Build([string] $Package, [string] $Out, [string] $With = $Inno, [switch] $ScanTwin) {
    $arguments = @('-NoProfile', '-NonInteractive', '-File', "$PSScriptRoot/build_setup.ps1", '-PackageDirectory', $Package, '-Inno', $With, '-OutputDirectory', $Out)
    if ($ScanTwin) { $arguments += '-ScanTwin' }
    $said = & (Get-Process -Id $PID).Path @arguments 2>&1
    return [pscustomobject]@{ Ended = $LASTEXITCODE; Said = ($said | ForEach-Object { "$_" }) -join "`n" }
}
function Expect-Refusal([string] $Package, [string] $Expected, [string] $Message, [string] $Out = '', [string] $With = $Inno) {
    if (-not $Out) { $Out = Join-Path (Split-Path -Parent $Package) 'out' }
    $result = Invoke-Build $Package $Out $With
    Check ($result.Ended -ne 0 -and $result.Said -match $Expected) "$Message (exit $($result.Ended): $($result.Said))"
}

$good = New-FixturePackage 'good' $fixture
$goodOut = Join-Path $work 'good/out'
$result = Invoke-Build $good $goodOut -ScanTwin
Check ($result.Ended -eq 0) "a package is made into a setup ($($result.Said))"
if ($result.Ended -eq 0) {
    $said = @(Get-SetupFailures $goodOut $good)
    Check ($said.Count -eq 0) "the setup of a package passes ($($said -join '; '))"
    $record = Get-Content -LiteralPath (Join-Path $goodOut 'setup-record.json') -Raw | ConvertFrom-Json
    Check ($record.setup.name -ceq 'qKVM-Desktop-0.1.0-windows-x64-setup.exe' -and $record.setup.sha256 -ceq (Get-Sha256 (Join-Path $goodOut $record.setup.name))) 'the record names the setup by its SHA-256'
    Check ($record.inno.version -ceq $InnoVersion -and $record.package.manifestSha256 -ceq (Get-Sha256 (Join-Path $good 'package-manifest.json'))) 'the record names the Inno Setup and the package it was made with'

    # Each way of being wrong, alone, on a copy of what was made.
    function Expect-Failure([string] $Name, [scriptblock] $Change, [string] $Expected) {
        $copy = Join-Path $work "wrong-$Name"
        Copy-Item -LiteralPath $goodOut -Destination $copy -Recurse
        & $Change $copy
        $said = @(Get-SetupFailures $copy $good)
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
    Expect-Failure 'for another architecture' { param($c) Edit-Script $c 'ArchitecturesAllowed=x64os' 'ArchitecturesAllowed=x64compatible' } 'ArchitecturesAllowed=x64os'
    Expect-Failure 'that closes programs' { param($c) Edit-Script $c 'CloseApplications=no' 'CloseApplications=yes' } 'CloseApplications=no'
    Expect-Failure 'that turns the junction guard off' { param($c) Edit-Script $c 'CloseApplications=no' "CloseApplications=no`r`nRedirectionGuard=no" } 'turns RedirectionGuard off'
    Expect-Failure 'that deletes before it installs' { param($c) Edit-Script $c '[Files]' "[InstallDelete]`r`nType: filesandordirs; Name: `"{app}\plugins`"`r`n`r`n[Files]" } '\[InstallDelete\] section'
    Expect-Failure 'that keeps a file the package does not have' { param($c) $k = Join-Path $c 'inputs/kept.iss'; [IO.File]::WriteAllText($k, [IO.File]::ReadAllText($k).Replace("  Result := '|';", "  Result := '|';`n  Result := Result + 'plugins\stale.dll|';")) } 'list of what it keeps'
    Expect-Failure 'that keeps less than the package' { param($c) $k = Join-Path $c 'inputs/kept.iss'; [IO.File]::WriteAllText($k, (@([IO.File]::ReadAllLines($k)) | Where-Object { $_ -notmatch 'qwindows' }) -join "`n") } 'list of what it keeps'
    Expect-Failure 'whose list does more than say the files' { param($c) $k = Join-Path $c 'inputs/kept.iss'; [IO.File]::AppendAllText($k, "procedure Other;`nbegin`nend;`n") } 'one function that says the kept files'
    Expect-Failure 'that removes nothing after the copy' { param($c) Edit-Script $c "    RemoveStaleIn('qml');" '' } 'removal after the copy lacks'
    Expect-Failure 'that walks a redirected folder' { param($c) Edit-Script $c "  if Redirected then`r`n    Log('Left as it is, a redirection: ' + Relative)`r`n  else`r`n    RemoveStale" '    RemoveStale' } 'removal after the copy lacks'
    Expect-Failure 'that deletes a name ending in a dot' { param($c) Edit-Script $c "(Copy(Found.Name, Length(Found.Name), 1) = '.') or " '' } 'removal after the copy lacks'
    Expect-Failure 'that sets the program aside in update mode alone' { param($c) Edit-Script $c "  if not FileExists(ProgramPath) then`r`n    Exit;`r`n  SetAside" "  if not UpdateMode or not FileExists(ProgramPath) then`r`n    Exit;`r`n  SetAside" } 'removal after the copy lacks'
    # (A failed set-aside stops the install before any file is copied, review C7.)
    Expect-Failure 'that copies though the program could not be set aside' { param($c) Edit-Script $c "  else`r`n    Result := 'the program''s own file could not be set aside for the copy (qkvm-desktop.exe)';" "  else`r`n    Result := '';" } 'removal after the copy lacks'
    Expect-Failure 'that sets the program aside only as it copies' { param($c) Edit-Script $c "  if Reason = '' then`r`n    Reason := SetProgramAside;`r`n" ''; Edit-Script $c "  if CurStep = ssInstall then begin`r`n    Installing := True;" "  if CurStep = ssInstall then begin`r`n    Installing := True;`r`n    SetProgramAside;" } 'not set aside last in Preparing to Install'
    Expect-Failure 'that starts the program it closed though the copy went on' { param($c) Edit-Script $c "    StartAtEnd := False;`r`n  end;" "  end;" } 'removal after the copy lacks'
    Expect-Failure 'that starts it at the end of a whole copy' { param($c) Edit-Script $c "  if StartAtEnd then`r`n    StartAgain;`r`nend;" "  if StartAtEnd or PostInstalled then`r`n    StartAgain;`r`nend;" } 'StartAgain'
    Expect-Failure 'that sets the program aside before it looks at files in use' { param($c) Edit-Script $c "  if Reason = '' then`r`n    Reason := SetProgramAside;`r`n" ''; Edit-Script $c "  Reason := RoomReason;`r`n" "  Reason := RoomReason;`r`n  if Reason = '' then`r`n    Reason := SetProgramAside;`r`n" } 'not set aside last in Preparing to Install'
    Expect-Failure 'that never puts the program back' { param($c) Edit-Script $c "      PutProgramBack;`r`n  if StartAtEnd then" "      Log('');`r`n  if StartAtEnd then" } 'removal after the copy lacks'
    Expect-Failure 'that leaves the aside copy' { param($c) Edit-Script $c "    if SetAside then`r`n      DropAside;" '' } 'removal after the copy lacks'
    Expect-Failure 'that leaves what a setup ended halfway left' { param($c) Edit-Script $c "    RemoveStaleIn('qml');`r`n    RemoveSetupLeftovers;" "    RemoveStaleIn('qml');" } 'removal after the copy lacks'
    Expect-Failure 'whose uninstaller leaves what a setup ended halfway left' { param($c) Edit-Script $c "    RemoveSetupLeftovers;`r`n    DropAside;" '    DropAside;' } 'removal after the copy lacks'
    Expect-Failure 'whose uninstaller leaves the program set aside' { param($c) Edit-Script $c "    RemoveSetupLeftovers;`r`n    DropAside;" '    RemoveSetupLeftovers;' } 'removal after the copy lacks'
    Expect-Failure 'whose uninstall step is never called' { param($c) Edit-Script $c 'procedure CurUninstallStepChanged(' 'procedure CurUninstallStepChange(' } 'removal after the copy lacks'
    Expect-Failure 'that takes any .tmp for what a setup left' { param($c) Edit-Script $c '(Length(Name) = 17) and ' '' } 'removal after the copy lacks'
    Expect-Failure 'that takes any letters for what a setup left' { param($c) Edit-Script $c '  for I := 4 to 13 do' '  for I := 4 to 4 do' } 'removal after the copy lacks'
    Expect-Failure 'that follows a redirection for what a setup left' { param($c) Edit-Script $c '  if ThroughRedirection(Relative) then begin' '  if False then begin' } 'removal after the copy lacks'
    Expect-Failure 'that looks through a redirected folder on the way' { param($c) Edit-Script $c "  while (Rest <> '') and not Result do begin" '  while False do begin' } 'removal after the copy lacks'
    Expect-Failure 'that looks at no folder on the way' { param($c) Edit-Script $c '  Rest := Relative;' "  Rest := '';" } 'removal after the copy lacks'
    Expect-Failure 'that looks at another path on the way' { param($c) Edit-Script $c "    Path := Path + '\' + Copy(Rest, 1, At - 1);" "    Path := ExpandConstant('{userdocs}');" } 'removal after the copy lacks'
    Expect-Failure 'that removes more than what a setup left' { param($c) Edit-Script $c '= 0) and SetupLeftover(Found.Name) then begin' '= 0) then begin' } 'removal after the copy lacks'
    Expect-Failure 'that looks only in the first of its folders' { param($c) Edit-Script $c '  while At > 0 do begin' '  if At > 0 then begin' } 'removal after the copy lacks'
    Expect-Failure 'that stops at the first leftover' { param($c) Edit-Script $c "+ Entry);`r`n      end;`r`n    until not FindNext(Found);" "+ Entry);`r`n      end;`r`n    until True;" } 'removal after the copy lacks'
    Expect-Failure 'that stops at the first stale file' { param($c) Edit-Script $c "+ Name);`r`n        end;`r`n      end;`r`n    until not FindNext(Found);" "+ Name);`r`n        end;`r`n      end;`r`n    until True;" } 'removal after the copy lacks'
    Expect-Failure 'whose install step is never called' { param($c) Edit-Script $c 'procedure CurStepChanged(' 'procedure CurStepChange(' } 'removal after the copy lacks'
    Expect-Failure 'whose list of folders lacks one of the package''s' { param($c) $k = Join-Path $c 'inputs/folders.iss'; [IO.File]::WriteAllText($k, (@([IO.File]::ReadAllLines($k)) | Where-Object { $_ -notmatch 'platforms' }) -join "`n") } 'list of the package''s folders'
    Expect-Failure 'whose list of folders has one the package has not' { param($c) $k = Join-Path $c 'inputs/folders.iss'; [IO.File]::WriteAllText($k, [IO.File]::ReadAllText($k).Replace("  Result := '|';", "  Result := '|';`n  Result := Result + '..|';")) } 'list of the package''s folders'
    Expect-Failure 'whose list of folders does more than say them' { param($c) $k = Join-Path $c 'inputs/folders.iss'; [IO.File]::AppendAllText($k, "procedure Other;`nbegin`nend;`n") } 'one function that says the package''s folders'
    Expect-Failure 'that removes what a setup left elsewhere' { param($c) Edit-Script $c "        Entry := Folder + '\' + Found.Name;" "        Entry := ExpandConstant('{userdocs}') + '\' + Found.Name;" } 'removal after the copy lacks'
    Expect-Failure 'whose program is not written last' { param($c) $l = Join-Path $c 'inputs/files.iss'; $lines = @([IO.File]::ReadAllLines($l)); [IO.File]::WriteAllLines($l, [string[]] (@($lines[-1]) + $lines[0..($lines.Count - 2)])) } 'program''s file is not the last'
    Expect-Failure 'whose marker comes after the program' { param($c) Edit-Script $c "Source: `"{#SourcePath}qkvm-desktop.installed`"; DestDir: `"{app}`"; Flags: ignoreversion`r`n" ''; Edit-Script $c '#include "files.iss"' "#include `"files.iss`"`r`nSource: `"{#SourcePath}qkvm-desktop.installed`"; DestDir: `"{app}`"; Flags: ignoreversion" } 'the package and the marker'
    Expect-Failure 'that deletes something else' { param($c) Edit-Script $c '    Installing := True;' "    Installing := True;`r`n  DeleteFile(ExpandConstant('{userdocs}\x'));" } 'deletes or renames otherwise'
    Expect-Failure 'that says another 105' { param($c) Edit-Script $c 'qKVM-RESULT: 105 the program was still running after 60 seconds' 'qKVM-RESULT: 105 still running' } 'result line of exit code 105'
    Expect-Failure 'that says 105 after it has ended' { param($c) Edit-Script $c "    Log('qKVM-RESULT: 105 the program was still running after 60 seconds; nothing was changed');`r`n" '' } 'result line of exit code 105'
    Expect-Failure 'that says nothing of 7' { param($c) Edit-Script $c "    Log('qKVM-RESULT: 7 nothing was changed: ' + Reason);" "    Log('7');" } 'result line of exit code 7'
    Expect-Failure 'that lets its folder be chosen' { param($c) Edit-Script $c 'DisableDirPage=yes' 'DisableDirPage=auto' } 'DisableDirPage=yes'
    Expect-Failure 'that calls something else outside' { param($c) Edit-Script $c 'procedure ExitProcess(ExitCode: Cardinal);' "function MessageBeep(Kind: Cardinal): Boolean;`r`n  external 'MessageBeep@user32.dll stdcall';`r`nprocedure ExitProcess(ExitCode: Cardinal);" } 'calls outside Pascal Script otherwise'
    Expect-Failure 'with another AppId' { param($c) Edit-Script $c 'AppId={{A964EFA6' 'AppId={{B964EFA6' } 'AppId='
    Expect-Failure 'that runs something else' { param($c) Edit-Script $c "[Run]`r`n" "[Run]`r`nFilename: `"{sys}\cmd.exe`"; Flags: nowait`r`n" } 'something else than the program is run'
    Expect-Failure 'with registry entries' { param($c) Edit-Script $c '[Icons]' "[Registry]`r`nRoot: HKCU; Subkey: `"Software\qKVM`"`r`n`r`n[Icons]" } '\[Registry\] section'
    Expect-Failure 'that asks the network' { param($c) Edit-Script $c "[Code]`r`n" "[Code]`r`n// DownloadTemporaryFile`r`n" } 'uses Download'
    # (Its version texts made 0.1.1 in place: they are UTF-16 in the setup's version resource, which is not compressed.)
    Expect-Failure 'of another version' { param($c)
        $file = Join-Path $c 'qKVM-Desktop-0.1.0-windows-x64-setup.exe'
        $text = [Text.Encoding]::Latin1.GetString([IO.File]::ReadAllBytes($file))
        $from = [Text.Encoding]::Latin1.GetString([Text.Encoding]::Unicode.GetBytes('0.1.0 '))
        if (-not $text.Contains($from)) { throw 'The setup has no version text to change.' }
        [IO.File]::WriteAllBytes($file, [Text.Encoding]::Latin1.GetBytes($text.Replace($from, [Text.Encoding]::Latin1.GetString([Text.Encoding]::Unicode.GetBytes('0.1.1 '))))) } 'does not say the package''s version'
    Expect-Failure 'that launches the program without --after-update' { param($c) Edit-Script $c 'Parameters: "--after-update"; Description:' 'Description:' } 'something else than the program is run'
    Expect-Failure 'that restarts the program without --after-update' { param($c) Edit-Script $c 'Parameters: "--after-update"; Flags: nowait runasoriginaluser' 'Flags: nowait runasoriginaluser' } 'something else than the program is run'
    Expect-Failure 'that does not look at the room for an update' { param($c) Edit-Script $c 'GetSpaceOnDisk64(' 'GetSpaceOnDisk(' } 'lacks: GetSpaceOnDisk64'
    Expect-Failure 'that does not look for files in use' { param($c) Edit-Script $c 'TFileStream.Create(' 'TStringStream.Create(' } 'lacks: TFileStream'
    Expect-Failure 'that takes an older version' { param($c) Edit-Script $c 'ComparePackedVersion(' 'Max(' } 'lacks: ComparePackedVersion'
    Expect-Failure 'not compiled' { param($c) Remove-Item -LiteralPath (Join-Path $c 'qKVM-Desktop-0.1.0-windows-x64-setup.exe') } 'did not compile'
    # (Inno Setup leaves room in the setup's manifest for the level it writes: another of no more letters takes its place.)
    Expect-Failure 'whose manifest asks for an administrator' { param($c)
        $file = Join-Path $c 'qKVM-Desktop-0.1.0-windows-x64-setup.exe'
        $text = [Text.Encoding]::Latin1.GetString([IO.File]::ReadAllBytes($file))
        $from = [regex]::Match($text, 'level="asInvoker" +uiAccess').Value
        if (-not $from) { throw 'The setup''s manifest has no level to change.' }
        $to = 'level="requireAdministrator"'.PadRight($from.Length - 'uiAccess'.Length) + 'uiAccess'
        [IO.File]::WriteAllBytes($file, [Text.Encoding]::Latin1.GetBytes($text.Replace($from, $to))) } 'asks for more than the user'
    Expect-Failure 'whose twin is compressed' { param($c) Copy-Item -LiteralPath (Join-Path $c 'qKVM-Desktop-0.1.0-windows-x64-setup.exe') -Destination (Join-Path $c 'scan/qKVM-Desktop-0.1.0-windows-x64-setup.exe') -Force } 'twin for the byte scan is not readable'
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
    Expect-Failure 'that asks {app} for the first page it shows' { param($c) Edit-Script $c "  Result := (PageID = wpSelectTasks) and (InstalledVersion <> '');" "  Result := (PageID = wpSelectTasks) and FileExists(ProgramAside);" } 'expands \{app\} before Inno Setup knows it: ShouldSkipPage -> ProgramAside'
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
    Expect-Failure 'whose first page calls after a // in a string' { param($c) Edit-Script $c "  if (InstalledFolder <> '') and FindProgramAt(" "  Log('https://qkvm.com/'); if FindProgram then Log('');`r`n  if (InstalledFolder <> '') and FindProgramAt(" } 'expands \{app\} before Inno Setup knows it: InitializeWizard -> FindProgram -> ProgramPath'
    # (And one that names another constant that says it, review T3.)
    Expect-Failure 'whose first page names a constant named after one that says {app}' { param($c) Edit-Script $c "`r`nprocedure InitializeWizard;" "`r`nconst`r`n  InstallRoot = '{app}';`r`n  EarlyRoot = InstallRoot;`r`n`r`nprocedure InitializeWizard;"; Edit-Script $c "FindProgramAt(AddBackslash(InstalledFolder) + 'qkvm-desktop.exe')" 'FileExists(ExpandConstant(EarlyRoot))' } 'expands \{app\} before Inno Setup knows it: InitializeWizard -> EarlyRoot'
    Expect-Failure 'whose first page names a constant that says {app}' { param($c) Edit-Script $c "`r`nprocedure InitializeWizard;" "`r`nconst`r`n  EarlyPath = '{app}\qkvm-desktop.exe';`r`n`r`nprocedure InitializeWizard;"; Edit-Script $c "FindProgramAt(AddBackslash(InstalledFolder) + 'qkvm-desktop.exe')" 'FileExists(ExpandConstant(EarlyPath))' } 'expands \{app\} before Inno Setup knows it: InitializeWizard -> EarlyPath'
    Expect-Failure 'whose first page calls after a quote in a comment' { param($c) Edit-Script $c "FindProgramAt(AddBackslash(InstalledFolder) + 'qkvm-desktop.exe')" "{ it's early } FindProgram and (InstalledFolder <> 'x')" } 'expands \{app\} before Inno Setup knows it: InitializeWizard -> FindProgram -> ProgramPath'
    Expect-Failure 'that looks for the program set aside however early it ends' { param($c) Edit-Script $c "  if SetAside and not PostInstalled then`r`n    if not FileExists(ProgramPath) then" "  if not FileExists(ProgramPath) then`r`n    if SetAside and not PostInstalled then" } 'DeinitializeSetup does not reach what expands'
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
    Expect-Failure 'whose 2 s count the first look too' { param($c) Edit-Script $c "      FilesInUse(ExpandConstant('{app}'), Busy, Unlisted);`r`n      Began := GetTickCount;" "      Began := GetTickCount;`r`n      FilesInUse(ExpandConstant('{app}'), Busy, Unlisted);" } 'in every mode'
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
    Expect-Failure 'that closes the program before it looks at the room' { param($c) Edit-Script $c "  Reason := RoomReason;`r`n  if (Reason = '') and WizardSilent" "  Reason := '';`r`n  if (Reason = '') and WizardSilent" } 'closing of a running copy lacks'
    Expect-Failure 'that closes it by hand where there is no room' { param($c) Edit-Script $c "  if RoomReason <> '' then`r`n    Exit;`r`n" '' } 'closing of a running copy lacks'
    Expect-Failure 'that leaves closed what it closed' { param($c) Edit-Script $c "      StartAtEnd := True;`r`n" '' } 'closing of a running copy lacks'
    Expect-Failure 'that never starts it again' { param($c) Edit-Script $c "  if StartAtEnd then`r`n    StartAgain;`r`n" '' } 'closing of a running copy lacks'
    Expect-Failure 'that starts something else again' { param($c) Edit-Script $c "ExecAsOriginalUser(ProgramPath, '--after-update'" "ExecAsOriginalUser(ExpandConstant('{sys}\cmd.exe'), '--after-update'" } 'does not start the program again as it should'
    Expect-Failure 'that starts the program twice' { param($c) Edit-Script $c "procedure StartAgain;" "procedure StartTwice;`r`nvar`r`n  Started: Integer;`r`nbegin`r`n  if ExecAsOriginalUser(ProgramPath, '--after-update', '', SW_SHOWNORMAL, ewNoWait, Started) then`r`n    Log('');`r`nend;`r`n`r`nprocedure StartAgain;" } 'does not start the program again as it should'
    Expect-Failure 'that starts it otherwise' { param($c) Edit-Script $c "  if ExecAsOriginalUser(ProgramPath, '--after-update', '', SW_SHOWNORMAL, ewNoWait, Started) then" "  if ExecAsOriginalUser(ProgramPath, '', '', SW_SHOWNORMAL, ewNoWait, Started) then" } 'does not start the program again as it should'
    Expect-Failure 'that does not say what is installed' { param($c) Edit-Script $c 'This upgrades it to %s and keeps' 'This upgrades it and keeps' } 'closing of a running copy lacks'
    Expect-Failure 'that asks for the tasks again on an upgrade' { param($c) Edit-Script $c "  Result := (PageID = wpSelectTasks) and (InstalledVersion <> '');" '  Result := False;' } 'closing of a running copy lacks'
    Expect-Failure 'that starts it again untold' { param($c) Edit-Script $c '  Result := MayStartProgram and (UpdateMode or (WizardSilent and ClosedIt));' '  Result := MayStartProgram and (UpdateMode or WizardSilent);' } 'closing of a running copy lacks'
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
    Expect-Failure 'that ticks Start with administrator rights' { param($c) Edit-Script $c "  Result := MayStartProgram and not UpdateMode and ((InstalledVersion = '') or ClosedIt);" "  Result := not UpdateMode and ((InstalledVersion = '') or ClosedIt);" } 'closing of a running copy lacks'
    Expect-Failure 'that offers Start with administrator rights' { param($c) Edit-Script $c "  Result := MayStartProgram and not UpdateMode and not StartTicked;" "  Result := not UpdateMode and not StartTicked;" } 'closing of a running copy lacks'
    Expect-Failure 'that starts an update again with administrator rights' { param($c) Edit-Script $c "  Result := MayStartProgram and (UpdateMode or (WizardSilent and ClosedIt));" "  Result := UpdateMode or (WizardSilent and ClosedIt);" } 'closing of a running copy lacks'
}

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

# --- Then the setup named.
if ($SetupDirectory) {
    $setup = (Resolve-Path -LiteralPath $SetupDirectory).Path
    $package = (Resolve-Path -LiteralPath $PackageDirectory).Path
    $said = @(Get-SetupFailures $setup $package)
    foreach ($line in $said) { Check $false $line }
    if (-not $said.Count) { Write-Output "The setup in $setup is the package in $package, for the user alone, with the program's mutex and the marker." }
}

if ($failures) { Write-Host "$failures setup check(s) failed."; exit 1 }
Remove-Item -LiteralPath $work -Recurse -Force
Write-Output 'Setup checks passed: a package made into a setup passes, and every way of being wrong that is tried fails.'
