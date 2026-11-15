; qKVM Desktop's setup, compiled by Inno Setup 6 (tools/fetch_inno.ps1) through tools/build_setup.ps1, never by hand.
; That script puts this file beside what it includes and names: files.iss (one line for each file of the package,
; made from its package-manifest.json), the marker below and the program's icon. It gives the compiler the version
; and the package folder:
;     ISCC /O<output folder> /DAppVersion=<x.y.z> /DPackageDir=<package folder> qkvm-desktop.iss
; tools/test_setup.ps1 holds this file to what is said of each line below.
;
; The setup installs the program for whoever runs it, alone and with no administrator, into
; %LOCALAPPDATA%\Programs\qKVM Desktop. It adds a Start menu entry (a desktop one only when it is ticked) and an
; entry in Settings > Apps, from which it is uninstalled, and it upgrades an earlier copy where it is. The program's
; data in the user's profile (%LOCALAPPDATA%\QKVM Desktop, %LOCALAPPDATA%\qKVM and Windows Credential Manager) is
; never touched by an install or an uninstall, and they say nothing of it. It is not signed until a certificate
; exists, and Windows says so before it runs (owner, 2026-11-08).
;
; The contract with the program's in-place updates (builders INST and UPD, 2026-11-08; neither changes it alone):
; - The setup writes {app}\qkvm-desktop.installed, whose whole content is the ASCII line "setup" and a line feed.
;   A portable copy has no such file. The program reads it to know it was installed by the setup.
; - The program runs the setup with exactly
;     /SILENT /SUPPRESSMSGBOXES /NOCANCEL /NORESTART /CURRENTUSER /UPDATE=1 /DIR="<its own folder>" /LOG="<log>"
;   and then closes all its windows and exits within 10 s.
; - With /UPDATE=1 the setup waits up to 60 s for the program's mutex to be released, then installs; on success it
;   starts {app}\qkvm-desktop.exe again, as the user and without elevation (never where the setup runs with
;   administrator rights: it says so in its log, and the person starts qKVM); if the mutex is still held after 60 s,
;   it ends without changing anything, with exit code 105 (5 keeps Inno Setup's meaning: Abort at an Abort/Retry
;   box); otherwise it returns Inno Setup's own exit codes.
; - Ending with 105, or with Inno Setup's 7 when its Preparing to Install refuses an update, the setup writes one result
;   line into its log right before it ends (the program cannot see the exit code: no copy of it runs while Setup does):
;     qKVM-RESULT: 105 the program was still running after 60 seconds; nothing was changed
;     qKVM-RESULT: 7 nothing was changed: <the reason in plain words>
; - The setup holds a mutex of its own while it runs, in every mode: qKVM-Desktop-Setup, created as named, so in the
;   session's own namespace, where the program opens it as Local\qKVM-Desktop-Setup. At its start the program waits
;   up to 120 s for it to be gone before it takes its own (the setup starts it while it still holds it).
; - Without /UPDATE=1 the setup is an ordinary interactive one.
;
; Closing a running copy for the person (0.1.2; the program's side is ui/win/quit_requests.h, and neither changes it
; alone):
; - Without /UPDATE=1 the setup does not refuse at its start because the program's mutex is held (a copy run from
;   another folder, such as a portable ZIP's, holds it too, and is left alone; the uninstaller looks at the mutex
;   itself, and still refuses while the program runs). It finds the copy that runs from {app}
;   by the window every version keeps for its icon (class "QKVM Desktop notification area", ui/win/tray.cpp), and
;   takes that window's process only where Windows says its file is {app}\qkvm-desktop.exe.
; - Interactive: at Install, "Close qKVM and continue" or Cancel. On yes it sets the program's event
;   Local\qKVM-Desktop-Quit, which only this Windows user may set and which carries nothing; the program ends the
;   orderly way Install and restart ends it, and only where nothing would be cut short. It answers in the title of a
;   window of its own (class "qKVM Desktop quit answer"): "<n>" it is ending, "<n> <words>" it stays, and why,
;   "<n>* <words>" it heard a request too late (the setup asks again, and never takes that for staying). The
;   setup waits about 10 s with progress (up to 60 s once the program has said it is ending), then offers Try again,
;   End it now (the forced end, on that choice alone) or Cancel (back to the Ready page, nothing changed). The request
;   stands while the setup's own ticket Local\qKVM-Desktop-Quit-Wanted (an event that takes itself back when it is
;   waited for, set once for each request) is there: the program takes the request by taking it, the setup withdraws
;   the request by taking it back, and whoever is first has it, in one step of Windows' (reviews C4 and R1).
; - Silent (/SILENT or /VERYSILENT, not /UPDATE=1): the same request and wait; where the program stays, it is ended by
;   force only with /FORCECLOSE=1, and otherwise nothing is changed: Preparing to Install refuses (exit code 7) with
;   the result line "qKVM-RESULT: 7 nothing was changed: <why>".
; - Afterwards: an interactive setup's last page offers "Start qKVM Desktop", ticked after a first install and where
;   this setup closed the program; a silent one starts it again where it closed it.

#ifndef AppVersion
  #error Name the version: /DAppVersion=x.y.z (tools/build_setup.ps1 does).
#endif
#ifndef PackageDir
  #error Name the package folder: /DPackageDir=<folder> (tools/build_setup.ps1 does).
#endif
#ifndef PackageBytes
  #error Name the bytes of every file the setup puts in: /DPackageBytes=<n> (tools/build_setup.ps1 does).
#endif

[Setup]
; Made once, on 2026-11-08, and never changed: Windows and every later setup know the program by it.
AppId={{A964EFA6-87C6-4A23-A9CC-B71F87441433}
AppName=qKVM Desktop
AppVersion={#AppVersion}
AppPublisher=qKVM
AppPublisherURL=https://qkvm.com/
VersionInfoVersion={#AppVersion}
VersionInfoProductName=qKVM Desktop
VersionInfoProductVersion={#AppVersion}
VersionInfoCompany=qKVM
VersionInfoDescription=qKVM Desktop Setup
VersionInfoCopyright=Copyright (c) 2026 qKVM
; For whoever runs it and no one else: no administrator is asked for, and nothing on the command line or in a
; dialog can ask for one instead (there is no PrivilegesRequiredOverridesAllowed).
PrivilegesRequired=lowest
DefaultDirName={userpf}\qKVM Desktop
; Always there, never a folder of the person's choosing (one such as C:\qKVM would give every user of the PC the right
; to change the program). An update names it with /DIR, as the contract says.
DisableDirPage=yes
DisableProgramGroupPage=yes
; The first page says what is installed and what this installs (InitializeWizard).
DisableWelcomePage=no
; The program is built for x64 Windows alone.
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
; Windows 10 22H2 and later.
MinVersion=10.0.19045
; The program's single-instance mutex: "Local\" and the name ui/main.cpp gives ui/win/single_instance.cpp. While
; it is held, the uninstall removes no file of the program: the uninstaller looks at it itself (InitializeUninstall,
; review V2), as Inno Setup keeps nothing of this for an install made outside update mode. The name goes through
; ProgramMutex below, which in update
; mode first gives the program its time to end, and outside it leaves the setup's own start alone (the copy that runs
; from {app} is closed for the person instead, and a copy that runs from elsewhere is left alone).
AppMutex={code:ProgramMutex|Local\QKVM Desktop}
; The setup's own, held while it runs: a second setup waits for it, and so does the program (see the contract above).
SetupMutex=qKVM-Desktop-Setup
; Nothing is closed for the setup by Windows either: a file in use is an error, never a program ended by Windows.
CloseApplications=no
; (RedirectionGuard is left as Inno Setup has it, on: a setup or an uninstall that someone starts elevated cannot be
; steered by a junction in the program's folder. tools/test_setup.ps1 refuses RedirectionGuard=no. With
; PrivilegesRequired=lowest the program this setup starts inherits the mitigation (Inno Setup's help, RedirectionGuard,
; Notes), and could not then follow a junction the user made, to where they keep its data: so every start of it here
; passes --after-update, and the program then waits for this setup's mutex to be gone and starts itself again
; through the user's shell, a copy that inherits nothing of the setup. The contract above.)
SetupIconFile={#SourcePath}qkvm.ico
UninstallDisplayIcon={app}\qkvm-desktop.exe
UninstallDisplayName=qKVM Desktop
ShowLanguageDialog=no
WizardStyle=modern
ChangesAssociations=no
ChangesEnvironment=no
SourceDir={#PackageDir}
OutputBaseFilename=qKVM-Desktop-{#AppVersion}-windows-x64-setup
; The compiler's own list of every file it put into the setup, with each one's SHA-256: tools/build_setup.ps1
; checks it against the package's manifest.
OutputManifestFile=setup-files.txt
#ifdef ScanTwin
; (The same setup with nothing compressed, made for the byte scan alone and never handed out: tools/build_setup.ps1.)
Compression=none
InternalCompressLevel=none
#else
Compression=lzma2
SolidCompression=yes
#endif

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; The marker of the contract, first.
Source: "{#SourcePath}qkvm-desktop.installed"; DestDir: "{app}"; Flags: ignoreversion
; Exactly the package's files, each at its own place under the program's folder, and the program's own file last:
; started during the copy, it is never a new program beside an earlier version's libraries.
#include "files.iss"

[Icons]
Name: "{userprograms}\qKVM Desktop"; Filename: "{app}\qkvm-desktop.exe"; WorkingDir: "{app}"
Name: "{userdesktop}\qKVM Desktop"; Filename: "{app}\qkvm-desktop.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
; Nothing is run but the program itself: offered at the end of an interactive setup ("Start qKVM Desktop", ticked
; after a first install and where this setup closed the program, StartTicked), and started again at the end of an
; update and of a silent install that closed it (StartsAgain); always with the one argument --after-update (the
; contract: the program waits for this setup's mutex to be gone, then starts itself again through the user's shell).
Filename: "{app}\qkvm-desktop.exe"; Parameters: "--after-update"; Description: "Start qKVM Desktop"; Flags: nowait postinstall skipifsilent; Check: StartTicked
Filename: "{app}\qkvm-desktop.exe"; Parameters: "--after-update"; Description: "Start qKVM Desktop"; Flags: nowait postinstall skipifsilent unchecked; Check: StartUnticked
Filename: "{app}\qkvm-desktop.exe"; Parameters: "--after-update"; Flags: nowait runasoriginaluser; Check: StartsAgain

[Code]
// The update mode of the contract, the checks before an update changes anything, the refusal of an older version
// over a newer one, the closing of a copy that runs from the program's folder (asked, the orderly way; by force only
// on the person's choice or /FORCECLOSE=1), the removal after a whole copy of what an earlier version left in plugins
// and qml, and, after a whole copy and at an uninstall, of what a setup or an update ended halfway left; nothing else.

const
  UpdateWaitMs = 60000;
  UpdateStepMs = 250;
  // How long Preparing to Install looks again at the files in use, from the end of its first look (review C10, R6, F1).
  LookMs = 2000;
  // (Delphi's fmOpenReadWrite or fmShareExclusive, and two file attributes: Pascal Script names none of them.)
  OpenReadWriteExclusive = $12;
  DirectoryAttribute = $10;
  ReparsePointAttribute = $400;
  // Room kept on top of the bytes of every file the setup puts in: its uninstaller's files and its log.
  SpareBytes = 33554432;
  // (Setup's own GetCustomSetupExitCode is asked only after a whole install: this one ends it before anything is.)
  StillRunningExitCode = 105;
  // What a copy of the program answers by (ui/win/quit_requests.h, ui/win/tray.cpp): the event that asks it to end,
  // the window whose title answers, and the window every version keeps for its icon.
  QuitRequestEvent = 'Local\qKVM-Desktop-Quit';
  QuitAnswerWindow = 'qKVM Desktop quit answer';
  // This setup's ticket of a request that stands: an event of its own making that takes itself back when one waits for
  // it, set once for each request. The program takes the request by taking the ticket; this setup withdraws it by
  // taking the ticket back; whoever comes first has it, in one step of Windows', and the other finds it gone: never
  // both, and nothing between that a stall or a timeout could cut (reviews C4 and R1). Never the event's being there,
  // which a handle kept elsewhere keeps.
  QuitWantedEvent = 'Local\qKVM-Desktop-Quit-Wanted';
  // The program's single-instance mutex, as AppMutex names it: what the uninstaller looks at itself (review V2).
  ProgramMutexName = 'Local\QKVM Desktop';
  // The start of the name of this setup's event that says it is at work: its mutex's (SetupMutex, in this session's
  // names, as the program opens it) and its process's number after it (ui/win/setup_wait.h's setupRunsName, review R2).
  SetupRunsName = 'Local\qKVM-Desktop-Setup-';
  // The mutex a start of the program honours (SetupMutex, in this session's names), which the uninstall holds too.
  SetupGateName = 'Local\qKVM-Desktop-Setup';
  ErrorAlreadyExists = 183;
  // (What Windows says of a path that is not there, and of a file's attributes it cannot tell.)
  ErrorFileNotFound = 2;
  ErrorPathNotFound = 3;
  NoFileAttributes = -1;
  // (And of a folder looked through to its end; and a WIN32_FIND_DATAW in characters: its size, where its name begins
  // and how long that may be; review F3.)
  ErrorNoMoreFiles = 18;
  FindDataChars = 296;
  FindNameAt = 23;
  FindNameChars = 260;
  IconWindow = 'QKVM Desktop notification area';
  // A copy asked to end is waited for about 10 s, and up to UpdateWaitMs in all once it has said it is ending.
  CloseWaitMs = 10000;
  // Windows' rights asked for (the event: only to set it; the copy found: to be told of and waited for, and to be
  // ended only by EndProgram), and a wait's answer that the thing waited for is over.
  EventModifyState = $0002;
  ProcessTerminate = $0001;
  ProcessQueryLimitedInformation = $1000;
  ProcessSynchronize = $00100000;
  // (The same right, asked of Setup's mutex to see that it is there, review F5.)
  MutexSynchronize = $00100000;
  WaitObject0 = 0;
  // What became of a copy asked to end.
  Ended = 0;
  Held = 1;
  Ending = 2;
  NoAnswer = 3;
  CannotBeAsked = 4;

var
  // (The wait is made once, before anything is installed: Setup may ask the mutex's name again later.)
  Waited: Boolean;
  Installing: Boolean;
  // Whether the program's own file was set aside for the copy, and whether the copy was whole.
  SetAside: Boolean;
  PostInstalled: Boolean;
  // Whether qKVM Desktop, which this setup closed, is started again as it ends: Preparing to Install refused, and
  // nothing was changed (review L1); cleared as the copy begins (review C8). Never the last page's "Start qKVM
  // Desktop", which is the person's own choice ([Run]).
  StartAtEnd: Boolean;
  // Whether the log says already that this setup starts nothing (MayStartProgram).
  ToldNoStart: Boolean;
  // The version this user's Settings > Apps entry said was installed when the setup began ('' where none was), and
  // its folder as the entry keeps it ('' where it says none): what the first page looks at, as {app} is known only
  // once the wizard has passed the folder's page, which it never shows (InitializeWizard runs before that).
  InstalledVersion: String;
  InstalledFolder: String;
  // The copy that runs from {app}, once found: its process, held open (so that its number stays its own), and that
  // number. Whether this setup closed it (it is offered, or started, again at the end). What a copy that stays said.
  ProgramProcess: THandle;
  ProgramId: Cardinal;
  ClosedIt: Boolean;
  HeldBy: String;
  ClosingPage: TOutputProgressWizardPage;
  // This setup's ticket of a request that stands, while it waits for the answer (0 at any other time).
  Wanted: THandle;
  // This setup's event that says it is at work, kept for as long as it runs (InitializeSetup, review R2); and the
  // uninstall's gate and its own such event, held for as long as it runs (InitializeUninstall, review R4).
  SetupRuns: THandle;
  UninstallGate: THandle;
  UninstallRuns: THandle;

procedure ExitProcess(ExitCode: Cardinal);
  external 'ExitProcess@kernel32.dll stdcall';
// (The number of this setup's own process, for its event that says it is at work, review R2.)
function GetCurrentProcessId: Cardinal;
  external 'GetCurrentProcessId@kernel32.dll stdcall';
// (The events of a setup and an uninstall at work, the ticket of a request; and the uninstall's gate, the mutex a start
// of the program honours as a setup's, review R4.)
function CreateEventW(Attributes, ManualReset, InitialState: Cardinal; Name: String): THandle;
  external 'CreateEventW@kernel32.dll stdcall';
function CreateMutexW(Attributes, InitialOwner: Cardinal; Name: String): THandle;
  external 'CreateMutexW@kernel32.dll stdcall';
// (For closing a running copy, and for Setup alone. Windows' yes or no comes as a number: anything but 0 is yes.)
function OpenEventW(Access, Inherit: Cardinal; Name: String): THandle;
  external 'OpenEventW@kernel32.dll stdcall setuponly';
function SetEvent(Event: THandle): Cardinal;
  external 'SetEvent@kernel32.dll stdcall setuponly';
function CloseHandle(Handle: THandle): Cardinal;
  external 'CloseHandle@kernel32.dll stdcall setuponly';
function OpenProcess(Access, Inherit, ProcessId: Cardinal): THandle;
  external 'OpenProcess@kernel32.dll stdcall setuponly';
function QueryFullProcessImageNameW(Process: THandle; Flags: Cardinal; Name: String; var Size: Cardinal): Cardinal;
  external 'QueryFullProcessImageNameW@kernel32.dll stdcall setuponly';
function WaitForSingleObject(Handle: THandle; Milliseconds: Cardinal): Cardinal;
  external 'WaitForSingleObject@kernel32.dll stdcall setuponly';
function TerminateProcess(Process: THandle; ExitCode: Cardinal): Cardinal;
  external 'TerminateProcess@kernel32.dll stdcall setuponly';
function FindWindowExW(Parent, After: HWND; ClassName: String; WindowName: Cardinal): HWND;
  external 'FindWindowExW@user32.dll stdcall setuponly';
function GetWindowThreadProcessId(Window: HWND; var ProcessId: Cardinal): Cardinal;
  external 'GetWindowThreadProcessId@user32.dll stdcall setuponly';
function GetWindowTextW(Window: HWND; Text: String; Most: Integer): Integer;
  external 'GetWindowTextW@user32.dll stdcall setuponly';
// (Windows' tick count, for Preparing to Install's 2 s: it only goes forward, review C10.)
function GetTickCount: Cardinal;
  external 'GetTickCount@kernel32.dll stdcall setuponly';
// (Whether a file found in use is there still, and why not where it is not, review R5.)
function GetFileAttributesW(Name: String): Integer;
  external 'GetFileAttributesW@kernel32.dll stdcall setuponly';
// (The next entry of a folder looked through, on the handle Pascal Script's FindFirst opened, so that why there is
// none is known by DLLGetLastError, review F3; Data is a String of FindDataChars characters it writes into.)
function FindNextFileW(Find: THandle; Data: String): Cardinal;
  external 'FindNextFileW@kernel32.dll stdcall setuponly';
// (Whether this setup's own mutex is there, as Inno Setup made it: its gate, review F5.)
function OpenMutexW(Access, Inherit: Cardinal; Name: String): THandle;
  external 'OpenMutexW@kernel32.dll stdcall setuponly';

function UpdateMode: Boolean;
begin
  Result := ExpandConstant('{param:UPDATE|0}') = '1';
end;

// A silent install's word that a copy which does not close when asked is ended by force (/FORCECLOSE=1).
function ForceClose: Boolean;
begin
  Result := ExpandConstant('{param:FORCECLOSE|0}') = '1';
end;

// AppMutex's value. In update mode, the first time it is asked for (when Setup starts), the program is first given
// up to 60 s to release it; if it has not, Setup ends here, before it has changed anything, with exit code 105.
// Outside update mode none for Setup's own check at its start (it finds the copy that runs from {app} and closes it for
// the person, and leaves a copy run from elsewhere alone), and the program's once installing. (That Inno Setup keeps
// what it is then for the uninstaller was not so, the first VM suite's step 25: the uninstaller looks at the program's
// mutex itself, InitializeUninstall, review V2.)
function ProgramMutex(Param: String): String;
var
  Elapsed: Integer;
begin
  Result := Param;
  if not UpdateMode and not Installing then begin
    Result := '';
    Exit;
  end;
  if Waited or Installing or not UpdateMode then
    Exit;
  Waited := True;
  Elapsed := 0;
  while CheckForMutexes(Param) and (Elapsed < UpdateWaitMs) do begin
    Sleep(UpdateStepMs);
    Elapsed := Elapsed + UpdateStepMs;
  end;
  if CheckForMutexes(Param) then begin
    // The one result line the program reads in the log it named (no copy of it runs while Setup does, to see the
    // exit code). Setup writes each line of its log as it is logged (Inno Setup 6.7.4's log is a text writer straight
    // on the file, with no buffer of its own), so it is in the file before ExitProcess ends the process.
    Log('qKVM-RESULT: 105 the program was still running after 60 seconds; nothing was changed');
    // (Ended here, Setup leaves its own temporary folder in the user's Temp: accepted, as nothing of the program
    // or of the user's is changed, and Windows' cleaning of Temp takes it.)
    ExitProcess(StillRunningExitCode);
  end;
  Log(Format('qKVM Desktop had ended after %d ms: the update goes on.', [Elapsed]));
end;

// After a whole copy, and only then: what an earlier version left in the two folders Qt loads whole, the plugins and
// the QML modules, and this version's package has not. The package's own list of them is made into the setup when it
// is built (kept.iss, by tools/build_setup.ps1 from package-manifest.json), never read from the folder. Outside the
// two folders nothing is touched here (what a setup ended halfway left: below); a redirection in them is left as it is
// and not followed; a folder this empties
// is removed. (Before the copy nothing is deleted: a setup ended halfway leaves the earlier version its own files.)
#include "kept.iss"

procedure RemoveStale(Folder, Relative, Kept: String);
var
  Found: TFindRec;
  Path, Name: String;
begin
  if not FindFirst(Folder + '\*', Found) then
    Exit;
  try
    repeat
      if (Found.Name <> '.') and (Found.Name <> '..') then begin
        Path := Folder + '\' + Found.Name;
        Name := Relative + '\' + Found.Name;
        // (Windows reads a name ending in a dot or a blank as the name without them: deleting it would delete a kept
        // file. It is left alone.)
        if (Copy(Found.Name, Length(Found.Name), 1) = '.') or (Copy(Found.Name, Length(Found.Name), 1) = ' ') then
          Log('Left as it is, a name Windows reads as another: ' + Name)
        else if (Found.Attributes and ReparsePointAttribute) <> 0 then
          Log('Left as it is, a redirection: ' + Name)
        else if (Found.Attributes and DirectoryAttribute) <> 0 then begin
          RemoveStale(Path, Name, Kept);
          if RemoveDir(Path) then
            Log('Removed the emptied folder ' + Name);
        end else if Pos('|' + Lowercase(Name) + '|', Kept) = 0 then begin
          if DeleteFile(Path) then
            Log('Removed what this version does not have: ' + Name)
          else
            Log('Could not remove what this version does not have: ' + Name);
        end;
      end;
    until not FindNext(Found);
  finally
    FindClose(Found);
  end;
end;

// One of the two folders, unless it is itself a redirection: then none of it is looked at.
procedure RemoveStaleIn(Relative: String);
var
  Folder: String;
  Found: TFindRec;
  Redirected: Boolean;
begin
  Folder := ExpandConstant('{app}\') + Relative;
  if not FindFirst(Folder, Found) then
    Exit;
  Redirected := (Found.Attributes and ReparsePointAttribute) <> 0;
  FindClose(Found);
  if Redirected then
    Log('Left as it is, a redirection: ' + Relative)
  else
    RemoveStale(Folder, Relative, KeptFiles);
end;

// What a setup ended halfway (killed, or the power gone) leaves in the program's folder: Inno Setup writes each file
// first under a temporary name (is-, ten letters or digits, .tmp) in the folder it goes to, and one cut off in the
// middle of that is left, which no later setup and no uninstaller would remove. After a whole copy, and at an
// uninstall, those files are removed from the package's own folders, by the package's own list of them, made into
// the setup when it is built (folders.iss, by tools/build_setup.ps1 from package-manifest.json), never read from the
// folder: a program's folder chosen where other programs live (/DIR) is not walked. Names of that very form only,
// files only; a folder that is a redirection, or is reached through one, is left as it is. Nothing else is touched.
#include "folders.iss"

function SetupLeftover(Name: String): Boolean;
var
  I: Integer;
begin
  Result := (Length(Name) = 17) and (Lowercase(Copy(Name, 1, 3)) = 'is-') and (Lowercase(Copy(Name, 14, 4)) = '.tmp');
  for I := 4 to 13 do
    if Result and (Pos(Uppercase(Copy(Name, I, 1)), 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789') = 0) then
      Result := False;
end;

// Whether the folder named under the program's folder ('' for the folder itself) is a redirection, or is reached
// through one.
function ThroughRedirection(Relative: String): Boolean;
var
  Found: TFindRec;
  Path, Rest: String;
  At: Integer;
begin
  Result := False;
  Path := ExpandConstant('{app}');
  Rest := Relative;
  while (Rest <> '') and not Result do begin
    At := Pos('\', Rest);
    if At = 0 then
      At := Length(Rest) + 1;
    Path := Path + '\' + Copy(Rest, 1, At - 1);
    Rest := Copy(Rest, At + 1, Length(Rest));
    if FindFirst(Path, Found) then begin
      Result := (Found.Attributes and ReparsePointAttribute) <> 0;
      FindClose(Found);
    end;
  end;
end;

procedure RemoveSetupLeftoversIn(Relative: String);
var
  Found: TFindRec;
  Folder, Entry: String;
begin
  if ThroughRedirection(Relative) then begin
    Log('Left as it is, a redirection: ' + Relative);
    Exit;
  end;
  Folder := ExpandConstant('{app}');
  if Relative <> '' then
    Folder := Folder + '\' + Relative;
  if not FindFirst(Folder + '\is-*.tmp', Found) then
    Exit;
  try
    repeat
      if ((Found.Attributes and (DirectoryAttribute or ReparsePointAttribute)) = 0) and SetupLeftover(Found.Name) then begin
        Entry := Folder + '\' + Found.Name;
        if DeleteFile(Entry) then
          Log('Removed what a setup ended halfway left: ' + Entry)
        else
          Log('Could not remove what a setup ended halfway left: ' + Entry);
      end;
    until not FindNext(Found);
  finally
    FindClose(Found);
  end;
end;

// Each of the package's folders in its list, the program's folder itself first.
procedure RemoveSetupLeftovers;
var
  Rest: String;
  At: Integer;
begin
  Rest := PackageFolders;
  Rest := Copy(Rest, 2, Length(Rest));
  At := Pos('|', Rest);
  while At > 0 do begin
    RemoveSetupLeftoversIn(Copy(Rest, 1, At - 1));
    Rest := Copy(Rest, At + 1, Length(Rest));
    At := Pos('|', Rest);
  end;
end;

// In every mode the program's own file is set aside before any file is written (last in Preparing to Install, where a
// failure stops the install with nothing copied, review C7), so that a start of it
// during the copy (by an update's own end, or from the Start menu just after Setup closed it, review L3) finds no program
// at all, never a new one beside half-copied libraries or an old one in their way.
// After a whole copy the set-aside copy is deleted; when the copy does not complete (DeinitializeSetup with no
// ssPostInstall), it is put back where the copy has left no program. (A setup ended from outside runs none of this:
// the next setup, in any mode, puts a set-aside copy back first when it finds no program beside it.)
function ProgramPath: String;
begin
  Result := ExpandConstant('{app}\qkvm-desktop.exe');
end;

function ProgramAside: String;
begin
  Result := ExpandConstant('{app}\qkvm-desktop.exe.old-update');
end;

procedure DropAside;
begin
  if DeleteFile(ProgramAside) then
    Log('Removed the earlier program, set aside for the copy.');
end;

procedure PutProgramBack;
begin
  if RenameFile(ProgramAside, ProgramPath) then
    Log('Put the earlier program back.')
  else
    Log('Could not put the earlier program back.');
end;

// Last in Preparing to Install, once nothing else stands in the way, so that no file is copied where it could not be
// done (review C7: a file that may be read and written but not renamed, as a folder that lets nobody delete in it has
// them, passes FilesInUse): '' where it is set aside (or there is none), else why not.
function SetProgramAside: String;
begin
  Result := '';
  if FileExists(ProgramAside) then begin
    if FileExists(ProgramPath) then
      DropAside
    else
      PutProgramBack;
  end;
  if not FileExists(ProgramPath) then
    Exit;
  SetAside := RenameFile(ProgramPath, ProgramAside);
  if SetAside then
    Log('Set the earlier program aside for the copy.')
  else
    Result := 'the program''s own file could not be set aside for the copy (qkvm-desktop.exe)';
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then begin
    Installing := True;
    // (The copy begins: a refusal of Preparing to Install before it, and a try again that went on, leave nothing to
    // start for. What starts at the end now is the person's tick on the last page, or an update's or a silent
    // install's own start, StartsAgain; never this; review C8.)
    StartAtEnd := False;
  end;
  if CurStep = ssPostInstall then begin
    PostInstalled := True;
    RemoveStaleIn('plugins');
    RemoveStaleIn('qml');
    RemoveSetupLeftovers;
    if SetAside then
      DropAside;
  end;
end;

// What this setup closed and then changed nothing for is started again as the setup ends, as the end of a setup starts
// it (--after-update: it waits for this setup to be gone, then starts itself anew through the person's shell).
// Whether this setup may start qKVM Desktop at all: never where it runs with administrator rights (elevated, or an
// administrator's with UAC off), for what it starts would run so too, with no person's own token to start it with, and
// qKVM Desktop runs only as the person, unelevated (review C1: the program refuses such a start as well, and starts
// nothing). Said in the log, once.
function MayStartProgram: Boolean;
begin
  Result := not IsAdmin;
  if not Result and not ToldNoStart then begin
    ToldNoStart := True;
    Log('qKVM Desktop is not started by this setup: the setup runs with administrator rights, and qKVM Desktop runs only as the person, unelevated. Start it from the Start menu.');
  end;
end;

procedure StartAgain;
var
  Started: Integer;
begin
  if not MayStartProgram then
    Exit;
  if ExecAsOriginalUser(ProgramPath, '--after-update', '', SW_SHOWNORMAL, ewNoWait, Started) then
    Log('Started qKVM Desktop again, which this setup had closed and then changed nothing for.')
  else
    Log('Could not start qKVM Desktop again, which this setup had closed: ' + SysErrorMessage(Started));
end;
// However early Setup ends, a cancel on its first page or a version refused included: the program's path is looked at
// only where this setup has come to its copy (SetAside, in Preparing to Install) or refused there (StartAtEnd), where
// {app} is known; nested, never on the strength of how an "and" is evaluated.
procedure DeinitializeSetup;
begin
  if SetAside and not PostInstalled then
    if not FileExists(ProgramPath) then
      PutProgramBack;
  if StartAtEnd then
    StartAgain;
end;

// The uninstaller refuses while qKVM Desktop runs: the program's own mutex, looked at here as it starts, whatever Inno
// Setup kept of AppMutex. (It keeps nothing of it for an install made outside update mode: the first VM suite's step
// 25 uninstalled a fresh install while the program ran, with no look at a mutex in its log; review M2, V2.) Silent, it
// ends as an uninstall that did not begin; by hand, it says why first. This is where the uninstall is decided: every
// refusal is here (review F4).
function InitializeUninstall: Boolean;
begin
  Result := True;
  // First the gate a start of the program honours, as a setup's: Setup's mutex and this uninstall's own event that
  // says it is at work (review R2), held for the whole uninstall, so that no start comes in between this look and the
  // removal of the files (review R4: one could start while the uninstall's box was asked). Made here, never opened:
  // where Setup's mutex is there already, a setup is open (or another uninstall, or something that keeps a handle to
  // it), and the uninstall does not begin; nor where the gate or the event cannot be made (review F4).
  UninstallGate := CreateMutexW(0, 0, SetupGateName);
  if (UninstallGate <> 0) and (DLLGetLastError = ErrorAlreadyExists) then begin
    Log('A setup of qKVM Desktop, or another uninstall of it, is at work: it is not uninstalled. Let that finish, then uninstall it again.');
    if not UninstallSilent then
      MsgBox('qKVM Desktop Setup, or another uninstall of qKVM Desktop, is at work. Let it finish, then uninstall qKVM Desktop again.', mbError, MB_OK);
    Result := False;
    Exit;
  end;
  UninstallRuns := CreateEventW(0, 1, 0, SetupRunsName + IntToStr(GetCurrentProcessId));
  if (UninstallGate = 0) or (UninstallRuns = 0) then begin
    Log('qKVM Desktop could not be kept from starting while it is uninstalled: it is not uninstalled.');
    if not UninstallSilent then
      MsgBox('qKVM Desktop could not be kept from starting while it is uninstalled, so it is not uninstalled. Uninstall it again later.', mbError, MB_OK);
    Result := False;
    Exit;
  end;
  if CheckForMutexes(ProgramMutexName) then begin
    Log('qKVM Desktop is running: it is not uninstalled. Quit it first, then uninstall it again.');
    if not UninstallSilent then
      MsgBox('qKVM Desktop is running. Quit it first: right-click the qKVM icon in the notification area, then choose Quit. Then uninstall it again.', mbError, MB_OK);
    Result := False;
  end;
end;

// At an uninstall, before the program's files are removed, so that the folder is left empty and goes with them: what
// a setup ended halfway left, and the earlier program an update ended halfway set aside.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then begin
    // (Looked at again right before anything is removed, as a best effort: a copy that started all the same, one before
    // 0.1.2 that does not honour the gate among them, is said in the log, and the exception ends this step; Inno
    // Setup's help says that it ends the uninstall there, which is for the VM to show, and nothing here rests on it.
    // A start in the instant between this look and the removal of the files is not stopped: a known limit
    // (SECURITY.md). The refusal that decides is InitializeUninstall's; reviews R4, F4.)
    if CheckForMutexes(ProgramMutexName) then begin
      Log('qKVM Desktop started while it was being uninstalled: it is not uninstalled. Quit it first, then uninstall it again.');
      RaiseException('qKVM Desktop is running. Quit it first, then uninstall it again.');
    end;
    RemoveSetupLeftovers;
    DropAside;
  end;
end;

// Never an older version over a newer one: the version this user's Settings > Apps entry says is installed, against
// this setup's. An interactive setup says why in plain words; a silent one ends without a word, with Inno Setup's
// exit code of a setup that did not begin, having changed nothing. The version installed is kept: the first page
// names it (InitializeWizard).
function InitializeSetup: Boolean;
var
  Installed: String;
  Have, Offered: Int64;
begin
  Result := True;
  // This setup at work, as the program can tell for as long as it runs: its own event, named after its mutex and its
  // process's number, kept until it ends. A handle to the mutex kept elsewhere after it has ended keeps the mutex there,
  // but no process of that number runs any more, and the program starts (review R2). Where it cannot be made, nothing
  // is closed and nothing is changed (GateReason, review F5).
  SetupRuns := CreateEventW(0, 1, 0, SetupRunsName + IntToStr(GetCurrentProcessId));
  if not RegQueryStringValue(HKEY_CURRENT_USER, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{A964EFA6-87C6-4A23-A9CC-B71F87441433}_is1', 'DisplayVersion', Installed) then
    Exit;
  InstalledVersion := Installed;
  if RegQueryStringValue(HKEY_CURRENT_USER, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{A964EFA6-87C6-4A23-A9CC-B71F87441433}_is1', 'InstallLocation', InstalledFolder) then
    InstalledFolder := RemoveBackslashUnlessRoot(InstalledFolder);
  if not StrToVersion(Installed + '.0', Have) or not StrToVersion('{#AppVersion}.0', Offered) then
    Exit;
  if ComparePackedVersion(Have, Offered) > 0 then begin
    Log(Format('qKVM Desktop %s is installed, newer than this setup''s %s: nothing was changed.', [Installed, '{#AppVersion}']));
    if not WizardSilent then
      MsgBox(Format('qKVM Desktop %s is installed, which is newer than this setup (%s). Nothing was changed.', [Installed, '{#AppVersion}']), mbInformation, MB_OK);
    Result := False;
  end;
end;

// Whether a file can be replaced now: opened alone for writing (not where a program has it open, or it is read-only),
// and no redirection (a junction or a link, looked at as what it is and never followed). One that Windows says is not
// there any more (deleted, or moved away, since it was found in use) is in nobody's way: free; one that cannot be
// looked at (access denied, as for a file whose deletion waits for its last handle to close, and the like) is not
// (review R5).
function FreeToReplace(Path: String): Boolean;
var
  Found: TFindRec;
  Stream: TFileStream;
  Error: Integer;
begin
  Result := False;
  if GetFileAttributesW(Path) = NoFileAttributes then begin
    Error := DLLGetLastError;
    Result := (Error = ErrorFileNotFound) or (Error = ErrorPathNotFound);
    Exit;
  end;
  if FindFirst(Path, Found) then begin
    try
      if (Found.Attributes and (ReparsePointAttribute or DirectoryAttribute)) <> 0 then
        Exit;
    finally
      FindClose(Found);
    end;
  end;
  try
    Stream := TFileStream.Create(Path, OpenReadWriteExclusive);
    Stream.Free;
    Result := True;
  except
  end;
end;

// The milliseconds since Began, by Windows' tick count, which only goes forward (over its turn at 49 days too).
function TicksSince(Began: Cardinal): Cardinal;
var
  At: Cardinal;
begin
  At := GetTickCount;
  if At >= Began then
    Result := At - Began
  else
    Result := At + ($FFFFFFFF - Began) + 1;
end;

// Every entry under Folder that cannot be replaced now, into Busy, in the order they come: a file FreeToReplace does
// not free (judged by it, as each look again is), or a redirection. Each is looked at once, and every one, however
// long that takes: the program's folder is a set of about ninety files, and a slow disk is no reason to refuse
// (review F1). A folder that cannot be looked through (Folder itself, or one within) is not known to hold nothing in
// use: it is in use itself, into Busy and Unlisted, unless Windows says it is not there (FreeToReplace's judgement,
// which frees a folder only so, as a fresh install's program folder), as a file is (review R5). So is one that Windows
// could not go through to its end: the entries it did not show are not known to be free (review F3).
procedure FilesInUse(Folder: String; Busy, Unlisted: TStringList);
var
  Found: TFindRec;
  Path, Name, Data, CutShort: String;
  Attributes: Cardinal;
  Error: Integer;
begin
  if not FindFirst(Folder + '\*', Found) then begin
    if not FreeToReplace(Folder) then begin
      Log('A folder of qKVM Desktop cannot be looked through: ' + Folder);
      Busy.Add(Folder);
      Unlisted.Add(Folder);
    end;
    Exit;
  end;
  CutShort := '';
  try
    Name := Found.Name;
    Attributes := Found.Attributes;
    while True do begin
      if (Name <> '.') and (Name <> '..') then begin
        Path := Folder + '\' + Name;
        if (Attributes and ReparsePointAttribute) <> 0 then
          Busy.Add(Path)
        else if (Attributes and DirectoryAttribute) <> 0 then
          FilesInUse(Path, Busy, Unlisted)
        else if not FreeToReplace(Path) then
          Busy.Add(Path);
      end;
      // (The next entry by Windows' own call, so that why there is none is known: "no more files" is the folder's end;
      // any other answer, and an entry whose name cannot be read, is a look cut short; review F3. Its WIN32_FIND_DATAW
      // read as characters: the attributes' low half, which holds those looked at, first, and the name from the 23rd
      // character to the first 0.)
      Data := StringOfChar(' ', FindDataChars);
      if FindNextFileW(Found.FindHandle, Data) = 0 then begin
        Error := DLLGetLastError;
        if Error <> ErrorNoMoreFiles then
          CutShort := Format('error %d: %s', [Error, SysErrorMessage(Error)]);
        Break;
      end;
      Name := Copy(Data, FindNameAt, FindNameChars);
      if Pos(#0, Name) = 0 then begin
        CutShort := 'a name in it could not be read';
        Break;
      end;
      Name := Copy(Name, 1, Pos(#0, Name) - 1);
      Attributes := Ord(Data[1]);
    end;
  finally
    FindClose(Found);
  end;
  if CutShort <> '' then begin
    Log('A folder of qKVM Desktop could not be looked through to its end (' + CutShort + '): ' + Folder);
    Busy.Add(Folder);
    Unlisted.Add(Folder);
  end;
end;

// Whether a copy of the program runs from {app}: found by the window every version of it keeps for its icon
// (ui/win/tray.cpp), and taken only where Windows says the file of that window's process is {app}\qkvm-desktop.exe. A
// copy run from another folder (a portable ZIP's) is never taken, and is left alone. The process found is held open
// (ProgramProcess) until another is looked for: so its number (ProgramId) stays its own, and what is asked of that
// number, and waited for, is that process and no other.
function FindProgramAt(Path: String): Boolean;
var
  Window: HWND;
  Id, Size: Cardinal;
  Process: THandle;
  Name: String;
begin
  if ProgramProcess <> 0 then
    CloseHandle(ProgramProcess);
  ProgramProcess := 0;
  ProgramId := 0;
  Window := FindWindowExW(0, 0, IconWindow, 0);
  while (Window <> 0) and (ProgramProcess = 0) do begin
    Id := 0;
    GetWindowThreadProcessId(Window, Id);
    Process := OpenProcess(ProcessQueryLimitedInformation or ProcessSynchronize, 0, Id);
    if Process <> 0 then begin
      Size := 1024;
      Name := StringOfChar(' ', Size);
      if (QueryFullProcessImageNameW(Process, 0, Name, Size) <> 0) and PathSame(Copy(Name, 1, Size), Path) and
         (WaitForSingleObject(Process, 0) <> WaitObject0) then begin
        ProgramProcess := Process;
        ProgramId := Id;
      end else
        CloseHandle(Process);
    end;
    Window := FindWindowExW(0, Window, IconWindow, 0);
  end;
  Result := ProgramProcess <> 0;
  if Result then
    Log(Format('qKVM Desktop runs from its folder (process %d).', [ProgramId]));
end;

// The copy that runs from {app}: only from the Ready page on, where {app} is known (never in InitializeSetup,
// InitializeWizard or a page before; tools/test_setup.ps1 holds that).
function FindProgram: Boolean;
begin
  Result := FindProgramAt(ProgramPath);
end;

// The answer of the copy found, as the title of its answer window (ui/win/quit_requests.h), taken only from a window of
// that very process: '' where it has none (a version before 0.1.2, or one that has gone).
function ProgramAnswer: String;
var
  Window: HWND;
  Id: Cardinal;
  Count: Integer;
begin
  Result := '';
  Window := FindWindowExW(0, 0, QuitAnswerWindow, 0);
  while Window <> 0 do begin
    Id := 0;
    GetWindowThreadProcessId(Window, Id);
    if Id = ProgramId then begin
      Result := StringOfChar(' ', 512);
      Count := GetWindowTextW(Window, Result, 512);
      Result := Copy(Result, 1, Count);
      Exit;
    end;
    Window := FindWindowExW(0, Window, QuitAnswerWindow, 0);
  end;
end;

// An answer's number (the request it answers), without the mark of one heard too late, and its words: '' while it is
// ending.
function AnswerNumber(Answer: String): String;
begin
  Result := Answer;
  if Pos(' ', Answer) > 0 then
    Result := Copy(Answer, 1, Pos(' ', Answer) - 1);
  if Copy(Result, Length(Result), 1) = '*' then
    Result := Copy(Result, 1, Length(Result) - 1);
end;

// Whether an answer is to a request the program heard too late ("<n>* <words>": one an earlier wait gave up on, or none
// of a Setup's): never the answer to this wait's own request, and never that the program stays open (review N1).
function AnswerLate(Answer: String): Boolean;
begin
  Result := (Pos(' ', Answer) > 1) and (Copy(Answer, Pos(' ', Answer) - 1, 1) = '*');
end;

function AnswerWords(Answer: String): String;
begin
  Result := '';
  if Pos(' ', Answer) > 0 then
    Result := Copy(Answer, Pos(' ', Answer) + 1, Length(Answer));
end;

// The request: this setup's event of a request that stands made (and kept until it stops waiting), then the program's
// event set, which only this Windows user may set (the program makes it so). It carries nothing. False where the copy
// has no such event: a version before 0.1.2, which cannot be asked.
function AskProgram: Boolean;
var
  Event: THandle;
begin
  // (Where it cannot be made, another program may hold its name: the copy cannot be asked, as one that does not hear
  // Setup cannot, and is never taken for one that did not answer.)
  if Wanted = 0 then begin
    // (Made here and now, and set once: one of the name that is there already is another program's, whose ticket this
    // setup does not own, and is never used; a request asked again while this one stands is the same request, and
    // its ticket is not set again, as the program may have taken it; reviews C4 and R1.)
    Wanted := CreateEventW(0, 0, 0, QuitWantedEvent);
    if (Wanted <> 0) and (DLLGetLastError = ErrorAlreadyExists) then begin
      CloseHandle(Wanted);
      Wanted := 0;
    end;
    if Wanted = 0 then begin
      Log('qKVM Desktop cannot be asked to close: this setup could not make its event of a request that stands (another program may hold its name).');
      Result := False;
      Exit;
    end;
    SetEvent(Wanted);
  end;
  Event := OpenEventW(EventModifyState, 0, QuitRequestEvent);
  Result := (Event <> 0) and (SetEvent(Event) <> 0);
  if Event <> 0 then
    CloseHandle(Event);
  if Result then
    Log('Asked qKVM Desktop to close, the orderly way.')
  else
    Log('qKVM Desktop cannot be asked to close (a version before 0.1.2).');
end;

// The request withdrawn: its ticket taken back, at once and in one step (reviews C4 and R1). Where this setup has it
// back, the program never takes the request, and one that hears it from now on stays. Where it is gone, the program
// took it first, and is ending: True, and it is waited for as one that ends, never told that it did not answer, nor
// that it stays (review F2: that outcome wins over every answer read before it). No wait, so no timeout that could be
// taken for a withdrawal.
function WithdrawRequest: Boolean;
begin
  Result := False;
  if Wanted <> 0 then begin
    Result := WaitForSingleObject(Wanted, 0) <> WaitObject0;
    CloseHandle(Wanted);
    Wanted := 0;
    if Result then
      Log('qKVM Desktop had taken the request to close: it is ending.')
    else
      Log('Withdrew the request to close.');
  end;
end;

// The copy found, asked to end and waited for: up to CloseWaitMs, and up to UpdateWaitMs in all once it has said it is
// ending (its goodbyes are given their time); shown on Progress where one is given. What became of it: Ended, Held
// (HeldBy says why), Ending, NoAnswer or CannotBeAsked. However it ends, the request is withdrawn.
function CloseProgram(Progress: TOutputProgressWizardPage): Integer;
var
  Before, Said: String;
  Elapsed, Most: Integer;
  Withdrawn, AskedAgain: Boolean;
begin
  HeldBy := '';
  if WaitForSingleObject(ProgramProcess, 0) = WaitObject0 then begin
    Result := Ended;
    Exit;
  end;
  Before := ProgramAnswer;
  // A copy with no answer window cannot be asked: a version before 0.1.2, or one that does not hear Setup (whose name
  // was taken by another, as its own log says). Never taken for one that does not answer (review L2).
  if Before = '' then begin
    Log('qKVM Desktop has no answer window: it cannot be asked to close.');
    Result := CannotBeAsked;
    Exit;
  end;
  Before := AnswerNumber(Before);
  Withdrawn := False;
  AskedAgain := False;
  try
    if not AskProgram then begin
      Result := CannotBeAsked;
      Exit;
    end;
    Result := NoAnswer;
    Elapsed := 0;
    Most := CloseWaitMs;
    while Elapsed < Most do begin
      if WaitForSingleObject(ProgramProcess, UpdateStepMs) = WaitObject0 then begin
        Log(Format('qKVM Desktop closed after %d ms.', [Elapsed]));
        Result := Ended;
        Exit;
      end;
      Elapsed := Elapsed + UpdateStepMs;
      Said := ProgramAnswer;
      if (Said <> '') and (AnswerNumber(Said) <> Before) and AnswerLate(Said) then begin
        // The answer to a request given up on before, not to this one: never taken for one that stays open, nor ended
        // by force for it (review N1). While this setup still waits, asked again, and waited for as before. Once it has
        // given up and withdrawn, asked again once only, with a second of its own for an answer, then withdrawn with the
        // grace again (review R1: a request asked again then, and left with none, could be taken just as the setup goes
        // on without it); never more, so that answers all too late make no loop.
        Before := AnswerNumber(Said);
        if not Withdrawn then begin
          Log('qKVM Desktop answered a request it heard too late: it is asked again.');
          if not AskProgram then
            Log('qKVM Desktop could not be asked again.');
        end else if not AskedAgain then begin
          Log('qKVM Desktop answered a request it heard too late, after this setup had given up: it is asked once more.');
          AskedAgain := True;
          Withdrawn := False;
          Most := Elapsed + 1000;
          if not AskProgram then
            Log('qKVM Desktop could not be asked again.');
        end else
          Log('qKVM Desktop answered another request it heard too late: it is not asked again.');
      end else if (Said <> '') and (AnswerNumber(Said) <> Before) then begin
        if AnswerWords(Said) <> '' then begin
          // (A word that it stays is believed only once the request is withdrawn, and was not taken first: where the
          // program had taken it all the same, that word was its answer to a request before, read late, and it is
          // ending, whatever it said; that word is let be, and the program waited for as one that ends; review F2.)
          if not WithdrawRequest then begin
            Result := Held;
            HeldBy := AnswerWords(Said);
            Log('qKVM Desktop stays open: ' + HeldBy);
            Exit;
          end;
          Before := AnswerNumber(Said);
        end;
        if Result <> Ending then begin
          Result := Ending;
          Most := UpdateWaitMs;
          if Progress <> nil then
            Progress.SetText('qKVM Desktop is closing: it lets its consoles and sessions go first.', '');
        end;
      end;
      // Given up: the request withdrawn, so that the program, should its interface answer later, stays. Where the
      // program had taken it first, it is ending, and is waited for as such (review R1); else a last second given for an
      // answer heard too late (review N1's asking again).
      if (Elapsed >= Most) and (Result = NoAnswer) and not Withdrawn then begin
        if WithdrawRequest then begin
          Result := Ending;
          Most := UpdateWaitMs;
          if Progress <> nil then
            Progress.SetText('qKVM Desktop is closing: it lets its consoles and sessions go first.', '');
        end else begin
          Withdrawn := True;
          Most := Most + 1000;
        end;
      end;
      if Progress <> nil then
        Progress.SetProgress(Elapsed, Most);
    end;
  finally
    // (However it ends, the request withdrawn; one the program had taken is one that ends, whatever was said before:
    // never one that stays, cannot be asked or did not answer; review F2.)
    if WithdrawRequest then
      if Result <> Ended then
        Result := Ending;
  end;
end;

// The forced end: only ever on the person's explicit choice (End it now), or on a silent install told /FORCECLOSE=1.
// Of the copy found and no other: opened by its number while this setup still holds that process open.
function EndProgram: Boolean;
var
  Process: THandle;
begin
  Log('qKVM Desktop is ended by force, as chosen.');
  Process := OpenProcess(ProcessTerminate, 0, ProgramId);
  Result := (Process <> 0) and (TerminateProcess(Process, 1) <> 0);
  if Process <> 0 then
    CloseHandle(Process);
  Result := Result and (WaitForSingleObject(ProgramProcess, 5000) = WaitObject0);
end;

// Why a copy is still running, for the person (Try again, End it now, Cancel).
function StillRunningText(Outcome: Integer): String;
begin
  case Outcome of
    Held: Result := 'qKVM Desktop says: ' + HeldBy + #13#10#13#10 + 'Finish that, then choose Try again. End it now ends it at once and cuts that short.';
    Ending: Result := 'qKVM Desktop is still closing: it lets its consoles and sessions go first. Choose Try again to wait for it. End it now ends it at once.';
    CannotBeAsked: Result := 'This copy of qKVM Desktop cannot be asked to close by Setup: it is older than 0.1.2, or it does not hear Setup. Quit it from its icon in the notification area: right-click the qKVM icon, then choose Quit. Then choose Try again. End it now ends it at once, without knowing what it is doing.';
  else
    Result := 'qKVM Desktop did not answer within 10 seconds. Choose Try again to wait longer. End it now ends it at once.';
  end;
end;

// And for a silent install's result line, in one line.
function StillRunningReason(Outcome: Integer): String;
begin
  case Outcome of
    Held: begin
      // (Its sentence, without its full stop: more follows.)
      Result := 'qKVM Desktop is running and stays open: ' + HeldBy;
      if Copy(Result, Length(Result), 1) = '.' then
        Result := Copy(Result, 1, Length(Result) - 1);
    end;
    Ending: Result := 'qKVM Desktop was still closing after 60 seconds';
    CannotBeAsked: Result := 'qKVM Desktop is running, and this copy of it cannot be asked to close';
  else
    Result := 'qKVM Desktop is running and did not answer within 10 seconds';
  end;
  if ForceClose then
    Result := Result + '; it could not be ended by force'
  else
    Result := Result + '; /FORCECLOSE=1 ends it by force';
end;

// The first page: what is installed, and what this does with it; and whether a copy runs now.
procedure InitializeWizard;
begin
  ClosingPage := CreateOutputProgressPage('Closing qKVM Desktop', 'Setup asked qKVM Desktop to close, the orderly way.');
  if InstalledVersion = '' then
    Exit;
  if InstalledVersion = '{#AppVersion}' then
    WizardForm.WelcomeLabel2.Caption := Format('qKVM Desktop %s is installed. This installs it again and keeps your wall, settings and passwords.', [InstalledVersion])
  else
    WizardForm.WelcomeLabel2.Caption := Format('qKVM Desktop %s is installed. This upgrades it to %s and keeps your wall, settings and passwords.', [InstalledVersion, '{#AppVersion}']);
  // (In the installed folder as Settings > Apps keeps it: {app} is not known yet here.)
  if (InstalledFolder <> '') and FindProgramAt(AddBackslash(InstalledFolder) + 'qkvm-desktop.exe') then
    WizardForm.WelcomeLabel2.Caption := WizardForm.WelcomeLabel2.Caption + #13#10#13#10 + 'qKVM Desktop is running. When you choose Install, Setup offers to close it for you.';
end;

// On an upgrade no page asks again what was chosen before: the folder is never asked (DisableDirPage), and the
// desktop entry stays as the earlier install had it (Inno Setup's UsePreviousTasks).
function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := (PageID = wpSelectTasks) and (InstalledVersion <> '');
end;

// The folder whose drive is looked at for room: the program's where it is there already (an upgrade, an update), else
// its drive's root (a first install: GetSpaceOnDisk64 takes only a folder that is there).
function RoomFolder: String;
begin
  Result := ExpandConstant('{app}');
  if not DirExists(Result) then
    Result := AddBackslash(ExtractFileDrive(Result));
end;

// Room on the program's drive for every file the setup puts in: '' where there is, else why not. Looked at before a
// running copy is closed (review L1): nothing is closed for an install that could not be made.
function RoomReason: String;
var
  Free, Total, Needed: Int64;
begin
  Result := '';
  Needed := {#PackageBytes};
  Needed := Needed + SpareBytes;
  if not GetSpaceOnDisk64(RoomFolder, Free, Total) then
    Result := 'the free room on the drive of qKVM Desktop could not be told'
  else if Free < Needed then
    Result := 'there is not room enough on the drive of qKVM Desktop';
end;

// Whether this setup holds what keeps a start of qKVM Desktop off while it works: its mutex (Inno Setup makes it from
// SetupMutex at Setup's start; looked for here, never made again, so as not to stand in the way of that start's own
// look for a setup that runs) and its event that says it is at work (made in InitializeSetup, review R2). '' where it
// holds both, else why not: without them a start of qKVM could come in while the files are replaced, so nothing is
// closed and nothing is changed (review F5). Looked at first, before the room.
function GateReason: String;
var
  Gate: THandle;
begin
  Result := '';
  Gate := OpenMutexW(MutexSynchronize, 0, SetupGateName);
  if Gate <> 0 then
    CloseHandle(Gate);
  if (Gate = 0) or (SetupRuns = 0) then
    Result := 'this setup could not make what keeps qKVM Desktop from starting while it works';
end;

// Install, chosen on the Ready page: where a copy of the program runs from {app}, the person is asked whether Setup
// may close it ("Close qKVM and continue"). It is asked to end the orderly way and waited for, with progress; where it
// stays, the person is told why, in its own words where it said them, and chooses: Try again, End it now (the forced
// end, on this choice alone) or Cancel (back to the Ready page, nothing changed). A copy run from elsewhere is left
// alone, and so is everything in a silent install or an update (PrepareToInstall, ProgramMutex).
function CloseAtInstall: Boolean;
var
  Outcome, Choice: Integer;
begin
  Result := True;
  if WizardSilent or UpdateMode or not FindProgram then
    Exit;
  // (Nothing closed where this setup holds no gate (review F5), nor where there is no room: Preparing to Install says
  // so, and qKVM goes on.)
  if GateReason <> '' then
    Exit;
  // (Nothing closed where there is no room: Preparing to Install says so, and qKVM goes on.)
  if RoomReason <> '' then
    Exit;
  if TaskDialogMsgBox('qKVM Desktop is running', 'Setup closes it before it installs, the orderly way: its consoles are let go, its keys released, and a start into BIOS setup is cleared. It closes only where nothing would be cut short.',
       mbConfirmation, MB_OKCANCEL, ['Close qKVM and continue', 'Cancel'], 0) <> IDOK then begin
    Result := False;
    Exit;
  end;
  repeat
    ClosingPage.SetText('Waiting for qKVM Desktop to close...', '');
    ClosingPage.SetProgress(0, CloseWaitMs);
    ClosingPage.Show;
    try
      Outcome := CloseProgram(ClosingPage);
    finally
      ClosingPage.Hide;
    end;
    if Outcome = Ended then begin
      ClosedIt := True;
      Exit;
    end;
    Choice := TaskDialogMsgBox('qKVM Desktop is still running', StillRunningText(Outcome), mbError, MB_YESNOCANCEL, ['Try again', 'End it now', 'Cancel'], 0);
    if Choice = IDNO then begin
      if EndProgram then begin
        ClosedIt := True;
        Exit;
      end;
      MsgBox('qKVM Desktop could not be ended. Nothing was changed.', mbError, MB_OK);
    end;
  until Choice = IDCANCEL;
  Result := False;
end;

// Every page's Next, the Ready page's Install among them: Install alone looks for a running copy ({app} is known only
// from there on, and NextButtonClick is asked of the pages before it too, a silent install's included).
function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID = wpReady then
    Result := CloseAtInstall;
end;

// A silent install (not an update) where a copy runs from {app}: asked and waited for as at Install; where it stays, it
// is ended by force only with /FORCECLOSE=1. Why it is still running, where it is; '' where it is not.
function CloseProgramSilently: String;
var
  Outcome: Integer;
begin
  Result := '';
  if not FindProgram then
    Exit;
  Outcome := CloseProgram(nil);
  if (Outcome <> Ended) and ForceClose then begin
    // A copy that could not be asked is ended without knowing what it is doing (a start into BIOS setup under way among
    // what that may cut): said plainly first (review L2).
    if Outcome = CannotBeAsked then
      Log('qKVM Desktop could not be asked to close, so what it is doing is not known; it is ended by force all the same, as /FORCECLOSE=1 says.');
    if EndProgram then
      Outcome := Ended;
  end;
  if Outcome = Ended then
    ClosedIt := True
  else
    Result := StillRunningReason(Outcome);
end;

// The last page's "Start qKVM Desktop": ticked after a first install, and where this setup closed the program; not
// ticked after an upgrade of a copy that was not running. Started again with nothing to ask: at the end of an update
// (the contract), and of a silent install that closed it.
// None of them where this setup may start nothing (MayStartProgram).
function StartTicked: Boolean;
begin
  Result := MayStartProgram and not UpdateMode and ((InstalledVersion = '') or ClosedIt);
end;

function StartUnticked: Boolean;
begin
  Result := MayStartProgram and not UpdateMode and not StartTicked;
end;

function StartsAgain: Boolean;
begin
  Result := MayStartProgram and (UpdateMode or (WizardSilent and ClosedIt));
end;


// Before anything is changed, in every mode (when this says why not, Setup ends with its exit code 7): this setup's
// gate and its event that says it is at work (GateReason, first, review F5), room on the program's drive for every file
// the setup puts in (RoomReason: nothing is closed for an install that could not be made), a silent install's running copy closed (CloseProgramSilently), and every file of the program's folder free
// to be replaced, looked at again for up to 2 s (a start of the program that ends at once, as one does while Setup is at
// work, or one just closed, holds its files a moment); and, last, the program's own file set aside for the copy
// (SetProgramAside): where it cannot be, nothing is copied (review C7). A failed copy halfway would leave the program a
// mix of two versions. Where this setup closed qKVM Desktop and then changes nothing, it is started again as the setup
// ends, and the result line says so (review L1).
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Reason, Path: String;
  Busy, Unlisted: TStringList;
  Began: Cardinal;
  I: Integer;
begin
  Result := '';
  Reason := GateReason;
  if Reason = '' then
    Reason := RoomReason;
  if (Reason = '') and WizardSilent and not UpdateMode then
    Reason := CloseProgramSilently;
  if Reason = '' then begin
    // (Every file looked at once; the whole folder, however long that takes (review F1); then only those in use again,
    // for 2 s from the end of that look, and no look goes through the whole folder again; review C10. The 2 s are
    // looked at before every look again: none is looked at after them, and a wait for one begins only where it ends
    // before them; review R6. A folder that could not be looked through, or not to its end, is looked through again,
    // whole, at each look again: once it can be, it is not in use itself, and what in it is in use joins the rest,
    // looked at again from the next round; where it still cannot be, it is in use again, and where it is gone, free.
    // Never by FreeToReplace, which frees no folder that is there: one whose files nobody looked at is not free
    // (review F7). The reason names such a folder as such where it is the first in use.)
    Busy := TStringList.Create;
    Unlisted := TStringList.Create;
    try
      FilesInUse(ExpandConstant('{app}'), Busy, Unlisted);
      Began := GetTickCount;
      while (Busy.Count > 0) and (TicksSince(Began) < LookMs - UpdateStepMs) do begin
        Sleep(UpdateStepMs);
        for I := Busy.Count - 1 downto 0 do begin
          if TicksSince(Began) >= LookMs then
            Break;
          Path := Busy[I];
          if Unlisted.IndexOf(Path) >= 0 then begin
            Busy.Delete(I);
            Unlisted.Delete(Unlisted.IndexOf(Path));
            FilesInUse(Path, Busy, Unlisted);
          end else if FreeToReplace(Path) then
            Busy.Delete(I);
        end;
      end;
      if Busy.Count > 0 then begin
        if Unlisted.IndexOf(Busy[0]) >= 0 then
          Reason := Format('a folder of qKVM Desktop cannot be looked through (%s)', [Busy[0]])
        else
          Reason := Format('a file of qKVM Desktop is in use or cannot be replaced (%s)', [Busy[0]]);
      end;
    finally
      Unlisted.Free;
      Busy.Free;
    end;
  end;
  if Reason = '' then
    Reason := SetProgramAside;
  if Reason <> '' then begin
    if ClosedIt then begin
      StartAtEnd := True;
      Reason := Reason + '; qKVM Desktop, which this setup had closed, is started again';
    end;
    // The one result line the program reads in the log it named (it cannot see Setup's exit code 7).
    Log('qKVM-RESULT: 7 nothing was changed: ' + Reason);
    Result := 'Nothing was changed: ' + Reason + '.';
  end;
end;
