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
;   starts {app}\qkvm-desktop.exe again, as the user and without elevation; if the mutex is still held after 60 s,
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
; The program is built for x64 Windows alone.
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
; Windows 10 22H2 and later.
MinVersion=10.0.19045
; The program's single-instance mutex: "Local\" and the name ui/main.cpp gives ui/win/single_instance.cpp. While
; it is held, neither the setup nor the uninstall replaces or removes a file of the program. The name goes through
; ProgramMutex below, which in update mode first gives the program its time to end.
AppMutex={code:ProgramMutex|Local\QKVM Desktop}
; The setup's own, held while it runs: a second setup waits for it, and so does the program (see the contract above).
SetupMutex=qKVM-Desktop-Setup
; Nothing is closed for the setup either: a file in use is an error, never a program ended by Windows.
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
; Nothing is run but the program itself: offered at the end of an interactive setup, and started again at the end
; of an update; both with the one argument --after-update (the contract: the program waits for this setup's mutex to
; be gone, then starts itself again through the user's shell).
Filename: "{app}\qkvm-desktop.exe"; Parameters: "--after-update"; Description: "{cm:LaunchProgram,qKVM Desktop}"; Flags: nowait postinstall skipifsilent; Check: not UpdateMode
Filename: "{app}\qkvm-desktop.exe"; Parameters: "--after-update"; Flags: nowait runasoriginaluser; Check: UpdateMode

[Code]
// The update mode of the contract, the checks before an update changes anything, the refusal of an older version
// over a newer one, the removal after a whole copy of what an earlier version left in plugins and qml, and, after a
// whole copy and at an uninstall, of what a setup or an update ended halfway left; nothing else.

const
  UpdateWaitMs = 60000;
  UpdateStepMs = 250;
  // (Delphi's fmOpenReadWrite or fmShareExclusive, and two file attributes: Pascal Script names none of them.)
  OpenReadWriteExclusive = $12;
  DirectoryAttribute = $10;
  ReparsePointAttribute = $400;
  // Room kept on top of the bytes of every file the setup puts in: its uninstaller's files and its log.
  SpareBytes = 33554432;
  // (Setup's own GetCustomSetupExitCode is asked only after a whole install: this one ends it before anything is.)
  StillRunningExitCode = 105;

var
  // (The wait is made once, before anything is installed: Setup may ask the mutex's name again later.)
  Waited: Boolean;
  Installing: Boolean;
  // Whether the program's own file was set aside for the copy, and whether the copy was whole.
  SetAside: Boolean;
  PostInstalled: Boolean;

procedure ExitProcess(ExitCode: Cardinal);
  external 'ExitProcess@kernel32.dll stdcall';

function UpdateMode: Boolean;
begin
  Result := ExpandConstant('{param:UPDATE|0}') = '1';
end;

// AppMutex's value. In update mode, the first time it is asked for (when Setup starts), the program is first given
// up to 60 s to release it; if it has not, Setup ends here, before it has changed anything, with exit code 105.
function ProgramMutex(Param: String): String;
var
  Elapsed: Integer;
begin
  Result := Param;
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

// In update mode the program's own file is set aside before any file is written (at ssInstall), so that a start of it
// during the copy finds no program at all, never a new one beside half-copied libraries or an old one in their way.
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
    Log('Removed the earlier program, set aside for the update.');
end;

procedure PutProgramBack;
begin
  if RenameFile(ProgramAside, ProgramPath) then
    Log('Put the earlier program back.')
  else
    Log('Could not put the earlier program back.');
end;

procedure SetProgramAside;
begin
  if FileExists(ProgramAside) then begin
    if FileExists(ProgramPath) then
      DropAside
    else
      PutProgramBack;
  end;
  if UpdateMode and FileExists(ProgramPath) then
    SetAside := RenameFile(ProgramPath, ProgramAside);
  if SetAside then
    Log('Set the earlier program aside for the update.');
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then begin
    Installing := True;
    SetProgramAside;
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

procedure DeinitializeSetup;
begin
  if SetAside and not PostInstalled and not FileExists(ProgramPath) then
    PutProgramBack;
end;

// At an uninstall, before the program's files are removed, so that the folder is left empty and goes with them: what
// a setup ended halfway left, and the earlier program an update ended halfway set aside.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then begin
    RemoveSetupLeftovers;
    DropAside;
  end;
end;

// Never an older version over a newer one: the version this user's Settings > Apps entry says is installed, against
// this setup's. An interactive setup says why in plain words; a silent one ends without a word, with Inno Setup's
// exit code of a setup that did not begin, having changed nothing.
function InitializeSetup: Boolean;
var
  Installed: String;
  Have, Offered: Int64;
begin
  Result := True;
  if not RegQueryStringValue(HKEY_CURRENT_USER, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{A964EFA6-87C6-4A23-A9CC-B71F87441433}_is1', 'DisplayVersion', Installed) then
    Exit;
  if not StrToVersion(Installed + '.0', Have) or not StrToVersion('{#AppVersion}.0', Offered) then
    Exit;
  if ComparePackedVersion(Have, Offered) > 0 then begin
    Log(Format('qKVM Desktop %s is installed, newer than this setup''s %s: nothing was changed.', [Installed, '{#AppVersion}']));
    if not WizardSilent then
      MsgBox(Format('qKVM Desktop %s is installed, which is newer than this setup (%s). Nothing was changed.', [Installed, '{#AppVersion}']), mbInformation, MB_OK);
    Result := False;
  end;
end;

// The first file under Folder that cannot be replaced now: one that cannot be opened alone for writing (a program
// has it open, or it is read-only), or a redirection (a junction or a link). Nothing where every file can be.
function FileInUse(Folder: String): String;
var
  Found: TFindRec;
  Path: String;
  Stream: TFileStream;
begin
  Result := '';
  if not FindFirst(Folder + '\*', Found) then
    Exit;
  try
    repeat
      if (Found.Name <> '.') and (Found.Name <> '..') then begin
        Path := Folder + '\' + Found.Name;
        if (Found.Attributes and ReparsePointAttribute) <> 0 then
          Result := Path
        else if (Found.Attributes and DirectoryAttribute) <> 0 then
          Result := FileInUse(Path)
        else begin
          try
            Stream := TFileStream.Create(Path, OpenReadWriteExclusive);
            Stream.Free;
          except
            Result := Path;
          end;
        end;
      end;
    until (Result <> '') or not FindNext(Found);
  finally
    FindClose(Found);
  end;
end;

// In update mode, before anything is changed (when this says why not, Setup ends with its exit code 7): room on the
// program's drive for every file the setup puts in, and every file of the program's folder free to be replaced. A
// failed copy halfway would leave the program a mix of two versions.
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Free, Total, Needed: Int64;
  Busy, Reason: String;
begin
  Result := '';
  if not UpdateMode then
    Exit;
  Reason := '';
  Needed := {#PackageBytes};
  Needed := Needed + SpareBytes;
  if not GetSpaceOnDisk64(ExpandConstant('{app}'), Free, Total) then
    Reason := 'the free room on the drive of qKVM Desktop could not be told'
  else if Free < Needed then
    Reason := 'there is not room enough on the drive of qKVM Desktop for the update'
  else begin
    Busy := FileInUse(ExpandConstant('{app}'));
    if Busy <> '' then
      Reason := Format('a file of qKVM Desktop is in use or cannot be replaced (%s)', [Busy]);
  end;
  if Reason <> '' then begin
    // The one result line the program reads in the log it named (it cannot see Setup's exit code 7).
    Log('qKVM-RESULT: 7 nothing was changed: ' + Reason);
    Result := 'Nothing was changed: ' + Reason + '.';
  end;
end;
