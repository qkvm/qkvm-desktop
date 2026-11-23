; qKVM Desktop's setup, compiled by Inno Setup 6 (tools/fetch_inno.ps1) through tools/build_setup.ps1, never by hand.
; That script puts this file beside what it includes and names: files.iss (one line for each file of each package,
; made from its package-manifest.json), the marker below and the program's icon. It gives the compiler the version
; and the package folders, x64's and, where the setup carries it, Windows on Arm's:
;     ISCC /O<output folder> /DAppVersion=<x.y.z> /DPackageDir=<x64 package> [/DArm64PackageDir=<arm64 package>] qkvm-desktop.iss
;
; One setup for both kinds of Windows (the owner, 2026-11-19: a person on Windows on Arm ends up with the ARM64 program
; by every road, and x64 users see no change). Where it carries Windows on Arm's package, it installs wherever Windows
; runs x64 programs, and puts in the ARM64 program where Windows is on Arm (Inno Setup's IsArm64) and the x64 program
; everywhere else, into the same folder either way: a folder that held one kind takes the other's files, and what only
; the other kind's install had there goes after a whole copy, by that version's own list (RemoveEarlierFiles). Every
; list it goes by (its files, their folders, what earlier versions installed) is the one of the kind it installs:
; IsArm64 chooses, below. It notes the kind it installed in its entry in Settings > Apps ("qKVM Desktop
; Architecture", x64 or arm64), for a person to see; no setup reads it back: an earlier install's kind is its program's
; own, by its PE header (ImageKind). A setup of x64's package alone (a
; development build's) installs on x64 Windows alone, as 0.1.0 to 0.1.2's did, and refuses Windows on Arm.
; tools/test_setup.ps1 holds this file to what is said of each line below.
;
; The setup installs the program for whoever runs it, alone and with no administrator, into
; %LOCALAPPDATA%\Programs\qKVM Desktop. It adds a Start menu entry (a desktop one only when it is ticked) and an
; entry in Settings > Apps, from which it is uninstalled, and it upgrades an earlier copy where it is. The program's
; data in the user's profile (%LOCALAPPDATA%\QKVM Desktop, %LOCALAPPDATA%\qKVM and Windows Credential Manager) is
; never touched by an install or an uninstall, and they say nothing of it. It is not signed until a certificate
; exists, and Windows says so before it runs (owner, 2026-11-08).
;
; The contract with the program's in-place updates (2026-11-08; the setup and the updates never change it apart):
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
; - Ending with 105, or with Inno Setup's 7 when its Preparing to Install refuses an update, or with 7 where its copy came
;   to its end and does not count (not whole, or not marked finished) and the earlier version is back, or with Inno
;   Setup's 1 where it does not begin because an update that did not finish left what it cannot put right, or because
;   another setup or uninstall of the account is at work (review 1.3), the setup
;   writes one result line into its log right before it ends (the program cannot see the exit code: no copy of it runs
;   while Setup does):
;     qKVM-RESULT: 105 the program was still running after 60 seconds; nothing was changed
;     qKVM-RESULT: 7 nothing was changed: <the reason in plain words>
;   or, where this setup first put right what an update that did not finish had left (in full or in part), and then
;   did not go on (0.2.0, review i020-2 L9):
;     qKVM-RESULT: 7 first, Setup <what it put right>, and changed nothing else: <the reason in plain words>
; - The setup holds a mutex of its own while it runs, in every mode: qKVM-Desktop-Setup, created as named, so in the
;   session's own namespace, where the program opens it as Local\qKVM-Desktop-Setup. At its start the program waits
;   up to 120 s for it to be gone before it takes its own (the setup starts it while it still holds it).
; - And the gate of this Windows account, in whatever session of it (0.2.0, review 1.3): the mutex
;   Global\qKVM-Desktop-Setup-Account-<the account's SID>, made owned as the setup begins (before anything is put right)
;   and held until it ends, as the uninstall holds it too. A second setup or uninstall of the account, in this session or
;   another, waits up to 3 s for it, then does not begin, and a setup writes the result line of 7. A start of the program
;   holds it for an instant, and takes a stale marker of an update away only then (ui/win/unfinished_update.h, review
;   1.4). An uninstall before the program has ever run for the account makes the lock of its profile and holds it.
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
;   this setup closed the program; a silent one starts it again where it closed it. None of this where the copy does not
;   count and left nothing whole: a first install that went on without a file starts nothing and offers nothing to
;   start (review 1.7).
;
; The earlier version set aside for the copy (0.2.0, i020; the program's side is ui/win/unfinished_update.h, and neither
; changes it alone):
; - Before it copies, where the program's folder holds any file of its package, the setup writes the marker
;   {app}\qkvm-desktop.update-unfinished, whose record says which files it renames and with which ending (.old-update,
;   or .2.old-update to .9.old-update: the first that no file of its package's has beside it), whole and on the disk
;   before anything is renamed; then renames each such file to its own name and that ending, in its own folder, the
;   program's own first. After a whole copy it renames the marker to qkvm-desktop.update-finishing, removes what its
;   record names, then that marker. Where the copy does not complete, it puts every file back, then removes the marker;
;   only where it saw every file its record names, and removed every file only the copy made: else the marker and its
;   record stay for the next setup (review 1.5), and a start of the program counts such a file of the copy's, still
;   there, as an update that did not finish.
; - A start of the program that finds qkvm-desktop.update-unfinished and a file its record set aside beside it, or a
;   record it cannot read, says "An update of qKVM did not finish. Run the qKVM setup again: it puts the earlier version
;   back, or finishes the update." in a box of Windows' own, and ends; one whose record names nothing still set aside
;   takes the marker away, and starts.
; - Every setup, in any mode, first puts right what such an update left (before its refusal of an older version), by
;   the record alone: the earlier version put back where the first marker is there, what was set aside removed where
;   the second is. A file of a set-aside ending that no record names is not the setup's: it never touches it, and it is
;   never in the way. A record it cannot read (a later version's, or not ours), it refuses, naming it. A record decides
;   only in a folder of its own, which is decided first: one that holds the contract's marker below, or one that holds
;   nothing but what a setup of ours leaves (the markers, the files their records name with their endings, and folders of
;   the package's own names); there, where no install of ours is (an uninstall that could not remove all an update had
;   set aside), what the records name is removed and nothing is put back (review 1.8). In any other folder nothing is
;   touched, and the setup does not begin, naming what is there (review 1.1). And only in a folder that no other
;   account may change (below), which is decided before anything there is put right by a record: in any other nothing
;   is put right, and Preparing to Install refuses the folder, saying that what an update left there stays as it is
;   (outside review 2, 1.3). (Preparing to Install reads the records there to tell a folder of its own before it judges
;   the rights, ForeignFolderReason, and follows none of them then.)
; - It installs only into a folder of its own (one that holds the contract's marker above, or nothing but what a setup of
;   ours leaves there) or one that is not there or holds nothing: a folder /DIR names that holds anything else (another
;   program's, a person's, a copy from the ZIP) is refused in Preparing to Install, exit code 7 and the result line,
;   before anything is closed or changed, and nothing in it is set aside, written over or removed (review i020-2, M1).
; - And only into a folder that this Windows account and Windows itself alone may change (review 1.2): one that another
;   account may write in (by an entry of its list, or of the folder it is made in that it or the files made in it take,
;   outside review 2, 1.2, that gives it more than reading and running), or whose owner is another, or below a folder
;   that lets another account remove, rename or take it, or on a
;   network share, or that is a redirection, is refused the same way. Its rights are never changed: they are the
;   person's or an administrator's to set. The default folder, in the person's own profile, is such a one wherever the
;   profile keeps the rights Windows gives it.

#ifndef AppVersion
  #error Name the version: /DAppVersion=x.y.z (tools/build_setup.ps1 does).
#endif
#ifndef PackageDir
  #error Name the x64 package's folder: /DPackageDir=<folder> (tools/build_setup.ps1 does).
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
; Where it carries Windows on Arm's package: on x64 Windows and on Windows 11 on Arm (Inno Setup's x64compatible), each
; the program of its own kind (IsArm64, [Files]). Where it carries x64's alone: on x64 Windows alone (x64os), as 0.1.0
; to 0.1.2's did. The owner's decision of 2026-11-19.
#ifdef Arm64PackageDir
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
#else
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
#endif
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
OutputBaseFilename=qKVM-Desktop-{#AppVersion}-windows-setup
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
; qKVM Setup's update check (ui/win/update_check.cpp, tools/build_setup.ps1 -UpdateCheck), of each kind of Windows the
; setup carries, carried and never copied as such: the [Code] section unpacks the one of the PC's kind (UpdateCheck) and
; puts it at the program's own name while the earlier version is set aside (PlaceUpdateCheck). First of all, so that it
; is unpacked without the rest (the setup is compressed solid).
Source: "{#SourcePath}qkvm-update-check.exe"; Flags: dontcopy noencryption
#ifdef Arm64PackageDir
Source: "{#SourcePath}qkvm-update-check-arm64.exe"; Flags: dontcopy noencryption
#endif
; The marker of the contract, first of what is copied.
Source: "{#SourcePath}qkvm-desktop.installed"; DestDir: "{app}"; Flags: ignoreversion
; Exactly the package's files of the PC's kind (Check: IsArm64, not IsArm64), each at its own place under the program's
; folder, and the program's own file last: started during the copy, it is never a new program beside an earlier
; version's libraries.
#include "files.iss"

[Icons]
Name: "{userprograms}\qKVM Desktop"; Filename: "{app}\qkvm-desktop.exe"; WorkingDir: "{app}"
Name: "{userdesktop}\qKVM Desktop"; Filename: "{app}\qkvm-desktop.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
; Nothing is run but the program itself, and nothing before the copy is judged: offered at the end of an interactive
; setup ("Start qKVM Desktop", ticked after a first install and where this setup closed the program, StartTicked), as
; postinstall entries, which run from the last page, after the post-install step; always with the one argument
; --after-update (the contract: the program waits for this setup's mutex to be gone, then starts itself again through
; the user's shell). Never where the copy does not count and left nothing whole (CopyStarts, review 1.7).
; An update's own start, and a silent install's that closed the program (StartsAgain), is made in [Code] at the very end
; of the post-install step (StartTheCopy), never here: Inno Setup runs an entry without postinstall before that step,
; while the copy is not yet flushed and marked finished, and the program it starts holds its own image, which the flush
; and a put-back must open; the copy then never counted, and every update was left unfinished (rehearsal on Windows on
; Arm, 2026-11-19).
Filename: "{app}\qkvm-desktop.exe"; Parameters: "--after-update"; Description: "Start qKVM Desktop"; Flags: nowait postinstall skipifsilent; Check: StartTicked
Filename: "{app}\qkvm-desktop.exe"; Parameters: "--after-update"; Description: "Start qKVM Desktop"; Flags: nowait postinstall skipifsilent unchecked; Check: StartUnticked

[Code]
// The update mode of the contract, the checks before an update changes anything, the refusal of an older version
// over a newer one, the closing of a copy that runs from the program's folder (asked, the orderly way; by force only
// on the person's choice or /FORCECLOSE=1), the earlier version set aside for the copy and put back where the copy does
// not complete, the removal after a whole copy of what the earlier version installed in that very folder and this one
// has not, by that version's own list, and, after a whole copy and at an uninstall, of what a setup or an update ended
// halfway left; nothing else.

const
  UpdateWaitMs = 60000;
  UpdateStepMs = 250;
  // How long Preparing to Install looks again at the files in use, from the end of its first look (review C10, R6, F1).
  LookMs = 2000;
  // (Delphi's fmOpenReadWrite or fmShareExclusive, and two file attributes: Pascal Script names none of them.)
  OpenReadWriteExclusive = $12;
  DirectoryAttribute = $10;
  ReparsePointAttribute = $400;
  ReadOnlyAttribute = $1;
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
  // The gate of this Windows account, in whatever session (review 1.3): its name, with the account's SID after it, made
  // owned by a setup or an uninstall as it begins and held until it ends; a second one waits for it up to
  // AccountGateWaitMs (a start of the program holds it for an instant to take a stale marker away, ui/win/unfinished_update.h),
  // then does not begin. (Windows' answers of a wait that took a mutex left by one that ended, and of none.)
  AccountGatePrefix = 'Global\qKVM-Desktop-Setup-Account-';
  AccountGateWaitMs = 3000;
  WaitAbandoned = $80;
  // (Opened, or made where it is not there; and what Windows says of a file that is one on a disk.)
  OpenAlways = 4;
  FileTypeDisk = 1;
  ErrorAlreadyExists = 183;
  // (What Windows says of a lock another holds, and of a handle that is none.)
  ErrorLockViolation = 33;
  ErrorInvalidHandle = 6;
  // The file whose lock a copy of qKVM Desktop 0.2.0 or later holds while it runs, in whatever Windows session of
  // this account (core::profile::runningFile, ui/win/profile_lock.h): beside its settings, in the person's data, which
  // this only opens; and the rights it is opened with.
  ProfileLockFile = '{localappdata}\QKVM Desktop\settings.json.running';
  GenericRead = $80000000;
  FileShareReadWrite = 3;
  OpenExisting = 3;
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
  // The earlier version set aside for the copy (i020; ui/win/unfinished_update.h names the first and the ending alike):
  // the marker of an update whose copy has not finished, written before the first file is set aside; the one it
  // becomes, in one rename, once the copy is whole; and the ending of each file set aside.
  UnfinishedMarker = 'qkvm-desktop.update-unfinished';
  FinishingMarker = 'qkvm-desktop.update-finishing';
  AsideEnding = '.old-update';
  // The record in the marker, of what the set-aside renames (ui/win/unfinished_update.h reads it the same way): its kind,
  // its last line, which says it was written whole, and the kinds of its lines that name a file; the name it is written
  // under before it is the marker; and the endings a set-aside gives, AsideEnding first (each set-aside the first that
  // no file of the package's has beside it).
  RecordKind = 'qKVM Desktop set-aside record 1';
  RecordEnd = 'end of the record';
  AsideKind = 'aside';
  LeftKind = 'left';
  RecordDraft = 'qkvm-desktop.update-record';
  // (And the name the update check is copied under, beside the program, before it is renamed into the program's place.)
  CheckDraft = 'qkvm-desktop.update-check';
  // (The SHA-256 of the contract's marker, "setup" and a line feed: a whole copy holds it so, review i020-2 L7.)
  MarkerSha256 = '9752eb62a6845a78a9e2eaeb4a6eb2d93a1b654ee8665f71d516cfaca3e7cf57';
  SetAsideEndings = '|.old-update|.2.old-update|.3.old-update|.4.old-update|.5.old-update|.6.old-update|.7.old-update|.8.old-update|.9.old-update|';
  // (The most of a record that is read, as the program reads it: one of the setup's own is some 10 KiB; review i020-2 L6.)
  MostRecord = 1048576;
  // (A file opened to be flushed, shared with whoever has it open.)
  GenericWrite = $40000000;
  FileShareAll = 7;
  // (How many times, a second apart, what was set aside is put back where the copy did not complete, while a file of
  // the copy's is still in its way after Inno Setup's own rollback: one that a scanner of the PC has just opened is let
  // go of within that.)
  PutBackTries = 5;
  // (A file of the copy that cannot be flushed is tried again so often, for up to 5 s in all, review 1.6.)
  FlushTries = 20;
  FlushStepMs = 250;
  // (How long the copy waits, at the most, for the starts of the program made while it copied to end, before it is
  // judged; and how many processes Windows' list of them holds here, at the most: WaitForStartsToEnd.)
  StartsEndMs = 60000;
  ProcessListMost = 16384;
  // Who may change the program's folder (review 1.2): Windows' kinds of what it is asked of (a file or folder, its owner
  // and its access list), of a token (to read it; its user), and of a drive on the network.
  FileObject = 1;
  OwnerAndList = 5;
  TokenQuery = $0008;
  TokenUserClass = 1;
  DriveRemote = 4;
  // TrustedInstaller, Windows' own servicing account, which owns the drive's root and Windows' own folders.
  TrustedInstallerSid = 'S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464';
  // The rights an entry gives, by the halves of Windows' access mask (the high half, bits 16 to 31, and the low): those
  // that only read and run (a file's reading and running rights, and Windows' generic ones of reading and running), any
  // other being one that changes; and those that let a folder above the program's be removed, renamed or given to
  // another (DELETE, FILE_DELETE_CHILD, WRITE_DAC, WRITE_OWNER, ACCESS_SYSTEM_SECURITY, MAXIMUM_ALLOWED, GENERIC_ALL).
  ReadsHigh = $A012;
  ReadsLow = $00A9;
  AboveHigh = $130D;
  AboveLow = $0040;
  // SDDL's words for rights, two letters each, and the mask each gives in eight hex digits.
  RightWords = '|GA10000000|GR80000000|GW40000000|GX20000000|RC00020000|SD00010000|WD00040000|WO00080000|RP00000010|WP00000020|CC00000001|DC00000002|LC00000004|SW00000008|LO00000080|DT00000040|CR00000100|FA001F01FF|FR00120089|FW00120116|FX001200A0|';
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
  // Whether the program's own file was set aside for the copy, and whether the copy came to its end.
  SetAside: Boolean;
  PostInstalled: Boolean;
  // Why a copy that came to its end does not count ('' where it does): not whole, or not marked finished (DoNotCount).
  NotCounted: String;
  // Whether such a copy left the earlier version whole again (put back): only then is anything started at the end
  // (review 1.7; never after a first install whose copy does not count).
  EarlierBack: Boolean;
  // Whether a put-back by this setup found the earlier program, which its record names as set aside, gone since: the
  // earlier version is then back all but its program, and is never said to be whole (review i020-2, L3).
  EarlierProgramGone: Boolean;
  // The record this setup wrote into its marker, kept for as long as it runs; and its handle of that marker, held without
  // the right for anything else to remove it, from the record's writing until the marker is renamed or this setup puts
  // right (0 at any other time; review i020-2, L1).
  RecordText: String;
  MarkerHold: THandle;
  // What this setup put right of an update that did not finish, said where it refuses after it ('' where nothing):
  // a refusal after a put-back never says that nothing was changed (review i020-1, L4).
  PutRightDone: String;
  // The version of ours installed in the program's folder before this copy, as its own record proves it (InstalledHere);
  // '' where nothing proves one. Only its own files are removed after a whole copy (RemoveEarlierFiles).
  EarlierHere: String;
  // And of which kind of Windows that install was: x64 or arm64, by its program's own PE header (ImageKind); '' where
  // its program is of neither.
  EarlierKindHere: String;
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
  // The page of an interactive install's wait for the starts of the program made during its copy (WaitForStartsToEnd).
  StartsPage: TOutputProgressWizardPage;
  // This setup's ticket of a request that stands, while it waits for the answer (0 at any other time).
  Wanted: THandle;
  // This setup's event that says it is at work, kept for as long as it runs (InitializeSetup, review R2); and the
  // uninstall's gate and its own such event, held for as long as it runs (InitializeUninstall, review R4).
  SetupRuns: THandle;
  UninstallGate: THandle;
  UninstallRuns: THandle;
  // The uninstall's handle of the profile's lock file, with the lock, held for as long as it runs (InitializeUninstall);
  // and that file, and its folder, where the uninstall made them, no copy having run for this account yet ('' where it
  // did not): removed as it ends (review 1.3).
  UninstallProfile: THandle;
  ProfileFileMade: String;
  ProfileFolderMade: String;
  // This setup's or this uninstall's gate of the Windows account, owned from its start to its end (review 1.3).
  AccountGate: THandle;

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
  external 'CloseHandle@kernel32.dll stdcall';
function OpenProcess(Access, Inherit, ProcessId: Cardinal): THandle;
  external 'OpenProcess@kernel32.dll stdcall setuponly';
// (Every process's number: the starts of the program that run from its folder while the copy is judged, found by
// StartsRunning. Ids is a String of 2 * ProcessListMost characters it writes the numbers into, two to each.)
function K32EnumProcesses(Ids: String; Bytes: Cardinal; var Needed: Cardinal): Cardinal;
  external 'K32EnumProcesses@kernel32.dll stdcall setuponly';
function QueryFullProcessImageNameW(Process: THandle; Flags: Cardinal; Name: String; var Size: Cardinal): Cardinal;
  external 'QueryFullProcessImageNameW@kernel32.dll stdcall setuponly';
function WaitForSingleObject(Handle: THandle; Milliseconds: Cardinal): Cardinal;
  external 'WaitForSingleObject@kernel32.dll stdcall';
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
// (Whether a file found in use is there still, and why not where it is not, review R5; and the attributes of a file of
// the setup's own it removes, by Setup and by the uninstaller, whose read-only one it clears first, review i020-1 M2.)
function GetFileAttributesW(Name: String): Integer;
  external 'GetFileAttributesW@kernel32.dll stdcall';
function SetFileAttributesW(Name: String; Attributes: Cardinal): Cardinal;
  external 'SetFileAttributesW@kernel32.dll stdcall';
// (The next entry of a folder looked through, on the handle Pascal Script's FindFirst opened, so that why there is
// none is known by DLLGetLastError, review F3; Data is a String of FindDataChars characters it writes into.)
function FindNextFileW(Find: THandle; Data: String): Cardinal;
  external 'FindNextFileW@kernel32.dll stdcall setuponly';
// (Whether this setup's own mutex is there, as Inno Setup made it: its gate, review F5.)
function OpenMutexW(Access, Inherit: Cardinal; Name: String): THandle;
  external 'OpenMutexW@kernel32.dll stdcall setuponly';
// (A file opened: by the uninstaller, the lock of the profile a copy of 0.2.0 or later holds while it runs,
// InitializeUninstall; by Setup, the record of what it sets aside, to be flushed, FlushedFile. LockFile, the
// uninstaller's alone, takes one byte at once or not at all, and needs no OVERLAPPED.)
function CreateFileW(Name: String; Access, Share, Security, Disposition, Flags, Template: Cardinal): THandle;
  external 'CreateFileW@kernel32.dll stdcall';
// (For Setup alone: the record of what it sets aside on the disk before anything is renamed.)
function FlushFileBuffers(Handle: THandle): Cardinal;
  external 'FlushFileBuffers@kernel32.dll stdcall setuponly';
function LockFile(Handle: THandle; OffsetLow, OffsetHigh, LengthLow, LengthHigh: Cardinal): Cardinal;
  external 'LockFile@kernel32.dll stdcall uninstallonly';
// (Who owns a folder and who may do what with it, in Windows' own words (SDDL), and who this setup runs as (its token's
// user, as a SID): whether the program's folder lets anyone but this account and Windows itself change it, review 1.2.
// Each text Windows gives is copied out of its memory, which is then given back; the token, closed.)
function GetNamedSecurityInfoW(Name: String; ObjectType, Info: Cardinal; var Owner, Group, Dacl, Sacl, Descriptor: THandle): Cardinal;
  external 'GetNamedSecurityInfoW@advapi32.dll stdcall setuponly';
function ConvertSecurityDescriptorToStringSecurityDescriptorW(Descriptor: THandle; Revision, Info: Cardinal; var Text: THandle; var Size: Cardinal): Cardinal;
  external 'ConvertSecurityDescriptorToStringSecurityDescriptorW@advapi32.dll stdcall setuponly';
function GetCurrentProcess: THandle;
  external 'GetCurrentProcess@kernel32.dll stdcall';
function OpenProcessToken(Process: THandle; Access: Cardinal; var Token: THandle): Cardinal;
  external 'OpenProcessToken@advapi32.dll stdcall';
function GetTokenInformation(Token: THandle; Kind: Cardinal; Info: String; Size: Cardinal; var Needed: Cardinal): Cardinal;
  external 'GetTokenInformation@advapi32.dll stdcall';
function ConvertSidToStringSidW(Sid: String; var Text: THandle): Cardinal;
  external 'ConvertSidToStringSidW@advapi32.dll stdcall';
function lstrlenW(Text: THandle): Integer;
  external 'lstrlenW@kernel32.dll stdcall';
function lstrcpynW(Into: String; Text: THandle; Most: Integer): THandle;
  external 'lstrcpynW@kernel32.dll stdcall';
function LocalFree(Memory: THandle): THandle;
  external 'LocalFree@kernel32.dll stdcall';
// (Where a String's own characters are: asked for the character before a string's very start, Windows gives back that
// start. The SID of the token's user is read where Windows' own pointer in what it wrote says, outside review 2, 1.1.)
function CharPrevW(Start, Current: String): THandle;
  external 'CharPrevW@user32.dll stdcall';
// (Whether a drive is one of this PC's or a network share's, whose own rights no look at its folders shows.)
function GetDriveTypeW(Root: String): Cardinal;
  external 'GetDriveTypeW@kernel32.dll stdcall setuponly';
// (Whether a handle is one of a file on a disk: that this setup holds the marker it wrote, review 1.4.)
function GetFileType(Handle: THandle): Cardinal;
  external 'GetFileType@kernel32.dll stdcall setuponly';

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

// What a setup ended halfway (killed, or the power gone) leaves in the program's folder: Inno Setup writes each file
// first under a temporary name (is-, ten letters or digits, .tmp) in the folder it goes to, and one cut off in the
// middle of that is left, which no later setup and no uninstaller would remove. After a whole copy, and at an
// uninstall, those files are removed from the package's own folders, by the package's own list of them, made into
// the setup when it is built (folders.iss, by tools/build_setup.ps1 from package-manifest.json), never read from the
// folder: a program's folder chosen where other programs live (/DIR) is not walked. Names of that very form only,
// files only; a folder that is a redirection, or is reached through one, is left as it is. Nothing else is touched.
#include "folders.iss"

// The kind of Windows this setup installs for: the PC's own (Inno Setup's IsArm64), and the lists of that kind's package
// (folders.iss, package.iss, earlier.iss: one of each for x64, and for Windows on Arm, which say nothing where the setup
// carries x64's package alone, and which it never installs then, x64os).
function ThisKind: String;
begin
  if IsArm64 then
    Result := 'arm64'
  else
    Result := 'x64';
end;

function PackageFolders: String;
begin
  if IsArm64 then
    Result := PackageFoldersArm64
  else
    Result := PackageFoldersX64;
end;

function SetupLeftover(Name: String): Boolean;
var
  I: Integer;
begin
  Result := (Length(Name) = 17) and (Lowercase(Copy(Name, 1, 3)) = 'is-') and (Lowercase(Copy(Name, 14, 4)) = '.tmp');
  for I := 4 to 13 do
    if Result and (Pos(Uppercase(Copy(Name, I, 1)), 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789') = 0) then
      Result := False;
end;

// Whether the folder named under Base, the program's folder ('' for the folder itself), is a redirection, or is reached
// through one; Base itself among them, where it is no drive's root (review 1.2: a redirection there leads all of it
// elsewhere). (Base is given: what an update left is looked at before {app} is known, PutRight.)
function ThroughRedirection(Base, Relative: String): Boolean;
var
  Found: TFindRec;
  Path, Rest: String;
  At: Integer;
begin
  Result := False;
  Path := RemoveBackslash(Base);
  if (Length(Path) > 2) and FindFirst(Path, Found) then begin
    Result := (Found.Attributes and ReparsePointAttribute) <> 0;
    FindClose(Found);
  end;
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

procedure RemoveSetupLeftoversIn(Base, Relative: String);
var
  Found: TFindRec;
  Folder, Entry: String;
begin
  if ThroughRedirection(Base, Relative) then begin
    Log('Left as it is, a redirection: ' + Relative);
    Exit;
  end;
  Folder := RemoveBackslashUnlessRoot(Base);
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

// Each of the package's folders in its list, under Base, the program's folder itself first: under {app} after a whole
// copy and at an uninstall (RemoveSetupLeftovers), and where an update that did not finish is put back (PutRight, which
// knows the folder before {app} is known).
procedure RemoveSetupLeftoversOf(Base: String);
var
  Rest: String;
  At: Integer;
begin
  Rest := PackageFolders;
  Rest := Copy(Rest, 2, Length(Rest));
  At := Pos('|', Rest);
  while At > 0 do begin
    RemoveSetupLeftoversIn(Base, Copy(Rest, 1, At - 1));
    Rest := Copy(Rest, At + 1, Length(Rest));
    At := Pos('|', Rest);
  end;
end;

procedure RemoveSetupLeftovers;
begin
  RemoveSetupLeftoversOf(ExpandConstant('{app}'));
end;

// The earlier version set aside for the copy (0.2.0, i020; the program's side is ui/win/unfinished_update.h). In every
// mode, before any file is written (last in Preparing to Install, where a failure stops the install with nothing copied,
// review C7), every file of this package's that is in the program's folder is set aside: renamed to its own name and an
// ending of the setup's, in its own folder, the program's own first, once the marker of an unfinished update is there
// beside them with its record of what is renamed and with which ending. The copy then makes every file anew: a start of
// the program during it finds no program, and a copy that does not complete is never a mix of two versions (Inno Setup's
// own rollback removes what it made and keeps what it replaced; here it replaces nothing). After a whole copy the marker
// becomes that of a finishing update, in one rename, and what its record names goes, then that marker (FinishCopy). When
// the copy does not complete, every file its record names is put back, and the marker goes (PutPackageBack). A setup
// ended from outside (killed, or the power gone) runs none of this: the program then says that an update did not finish,
// and the next setup, in any mode, puts right first what it left (PutRight), by the record. What is the setup's own is
// decided by its package's list and the record together, never by an ending alone: a file of a set-aside ending that no
// record names is not the setup's, and is never touched, nor ever in the way (each set-aside takes the first ending that
// no file of the package's has beside it). The list is the package's own, made into the setup when it is built
// (package.iss, by tools/build_setup.ps1 from package-manifest.json), never read from the folder: nothing outside the
// package's names is set aside, put back or removed, and nothing that is a folder or a redirection. (A record that a
// setup of another version wrote names its own package's files: known here where a published version of ours installed
// them, earlier.iss below; a record that names another is not read, and the setup refuses.)
//
// What a cut (the power gone, the setup ended from outside) leaves at each moment, by these steps, and what a start of
// qKVM then finds at the program's own name (review i020-1, M3):
// - before the record is the marker (WriteRecord: a draft, flushed, renamed): the earlier version as it was, and a draft
//   at most; it starts.
// - the marker there, before the program's own file is renamed: the earlier version whole; it starts (0.2.0 and later
//   find a record naming nothing set aside, take the marker away; 0.1.x does not look). While this setup runs it holds
//   the marker, which a start in another Windows session cannot take away then (review i020-2, L1).
// - the program's own file renamed, before the update check is in its place (copied under its draft's name, flushed,
//   renamed: review i020-2, L4): no program at that name, and a draft at most; the next setup puts the earlier version
//   back.
// - from the update check in the program's place, through the other renames and the whole copy, to the copy's own
//   program (written last, files.iss): the update check; it says that an update did not finish (while a setup is at
//   work it ends at once, saying nothing). Within it, the instant in which Inno Setup replaces the contract's marker,
//   the first file it copies (it removes the earlier one, then renames its own into place): no marker of an install of
//   ours, so the next setup takes the folder for none of its own and refuses, naming what is there (a record decides
//   only in a folder of ours, review 1.1); "setup" and a line feed written to qkvm-desktop.installed lets it go on.
// - the copy's program written, before the copy is marked finished (NotCopied, FlushCopy, MarkFinishing): the new
//   version whole beside the marker and what was set aside; it says, by the program's own look at the marker's record
//   (ui/win/unfinished_update.h), that an update did not finish.
// - marked finished, while what was set aside is removed (FinishRecorded): the new version whole; it starts, and the
//   next setup removes what is left.
// - putting back (PutBackRecorded: after a copy that failed or does not count, and as the next setup begins): what is at
//   the program's name removed, then the update check put there (a cut in between, or while it is placed, leaves no
//   program at that name, and the marker: the next setup puts back again); a cut while the libraries come back, or what
//   the copy made goes, leaves the update check; one between its removal and the earlier program's rename back, no
//   program at that name. Where the earlier program is gone since it was set aside (review i020-2, L3), the update
//   check is taken away last, before the marker: a cut leaves it with the marker, and after it no program at that name
//   and no marker, until a setup completes.
#include "package.iss"

function PackageFiles: String;
begin
  if IsArm64 then
    Result := PackageFilesArm64
  else
    Result := PackageFilesX64;
end;

function PackageFileBytes: String;
begin
  if IsArm64 then
    Result := PackageFileBytesArm64
  else
    Result := PackageFileBytesX64;
end;

function PackageFileSha256: String;
begin
  if IsArm64 then
    Result := PackageFileSha256Arm64
  else
    Result := PackageFileSha256X64;
end;
// What versions of ours before this one installed, of either kind of Windows, and this version's package of the other
// kind, and this one has not (earlier.iss, made by tools/build_setup.ps1 from packaging/published-files.json and the
// packages): all of them, names a record of what an update set aside may give, as the package's own (OwnName); and each
// version's own of each kind, with their folders, removed after a whole copy where that version of that kind was
// installed in that very folder (RemoveEarlierFiles, InstalledHere, EarlierKindHere).
#include "earlier.iss"

function EarlierFiles: String;
begin
  if IsArm64 then
    Result := EarlierFilesArm64
  else
    Result := EarlierFilesX64;
end;

function EarlierFilesOf(Version, Kind: String): String;
begin
  if IsArm64 then
    Result := EarlierFilesOfArm64(Version, Kind)
  else
    Result := EarlierFilesOfX64(Version, Kind);
end;

function EarlierFoldersOf(Version, Kind: String): String;
begin
  if IsArm64 then
    Result := EarlierFoldersOfArm64(Version, Kind)
  else
    Result := EarlierFoldersOfX64(Version, Kind);
end;

// The name of qKVM Setup's update check of the PC's kind, which the set-aside puts in the program's place
// (ui/win/update_check.cpp; [Files]).
function UpdateCheck: String;
begin
  if IsArm64 then
    Result := 'qkvm-update-check-arm64.exe'
  else
    Result := 'qkvm-update-check.exe';
end;

function ProgramPath: String;
begin
  Result := ExpandConstant('{app}\qkvm-desktop.exe');
end;

// Of which kind of Windows a program is, by its own PE header: the machine its image is for, 'arm64' (ARM64) or 'x64'
// (AMD64); '' where the file cannot be read as a program of either. What a folder holds is known by its program, never
// by the entry of Settings > Apps, which Inno Setup makes anew at every install and an update that did not finish
// leaves without the kind (rehearsal on Windows on Arm, 2026-11-19). A program that cannot be opened (a scanner of the
// PC that holds it alone for a moment) is opened again, half a second later, once, as SameBytes reads a file (review
// armp-1 L8): where its kind is not known, what only the other kind's install had in the folder stays there.
function ImageKind(Path: String): String;
var
  Stream: TFileStream;
  Header: AnsiString;
  At, Machine, Tries: Integer;
  Opened: Boolean;
begin
  Result := '';
  Opened := False;
  Tries := 0;
  while not Opened and (Tries < 2) do begin
    if Tries > 0 then
      Sleep(500);
    Tries := Tries + 1;
    try
      Stream := TFileStream.Create(Path, fmOpenRead or fmShareDenyNone);
      Opened := True;
      try
        SetLength(Header, 64);
        Stream.ReadBuffer(Header, 64);
        if (Header[1] <> 'M') or (Header[2] <> 'Z') or (Ord(Header[63]) <> 0) or (Ord(Header[64]) <> 0) then
          Exit;
        At := Ord(Header[61]) + Ord(Header[62]) * 256;
        if At < 64 then
          Exit;
        Stream.Seek(At, soFromBeginning);
        SetLength(Header, 6);
        Stream.ReadBuffer(Header, 6);
        if (Header[1] <> 'P') or (Header[2] <> 'E') or (Ord(Header[3]) <> 0) or (Ord(Header[4]) <> 0) then
          Exit;
        Machine := Ord(Header[5]) + Ord(Header[6]) * 256;
        if Machine = $AA64 then
          Result := 'arm64'
        else if Machine = $8664 then
          Result := 'x64';
      finally
        Stream.Free;
      end;
    except
      if not Opened then
        Log('The program in this folder could not be opened, to tell its kind of Windows: ' + Path + ' (' + GetExceptionMessage + ')');
      Result := '';
    end;
  end;
end;

// A name in the program's folder.
function InApp(Name: String): String;
begin
  Result := ExpandConstant('{app}\') + Name;
end;

// The names of a list made at build time ('|a|b|'), added to Names in their order.
procedure AddNames(List: String; Names: TStringList);
var
  Rest: String;
  At: Integer;
begin
  Rest := Copy(List, 2, Length(List));
  At := Pos('|', Rest);
  while At > 0 do begin
    Names.Add(Copy(Rest, 1, At - 1));
    Rest := Copy(Rest, At + 1, Length(Rest));
    At := Pos('|', Rest);
  end;
end;

// Whether anything is at Path (a file, a folder, a redirection), as a look in its folder shows it.
function ThingAt(Path: String): Boolean;
var
  Found: TFindRec;
begin
  Result := FindFirst(Path, Found);
  if Result then
    FindClose(Found);
end;

// Whether Windows could tell whether anything is at Path, and There, whether it is: a look Windows refuses for another
// reason than its not being there (a folder whose list is shut, a file whose deletion waits) tells nothing, and a
// put-back or a removal never takes it for one that is not there (review 1.5).
function LookAt(Path: String; var There: Boolean): Boolean;
var
  Error: Integer;
begin
  There := GetFileAttributesW(Path) <> NoFileAttributes;
  Result := There;
  if not There then begin
    Error := DLLGetLastError;
    Result := (Error = ErrorFileNotFound) or (Error = ErrorPathNotFound);
  end;
end;

// Whether what is at Path is a plain file: no folder, and no redirection.
function PlainFileAt(Path: String): Boolean;
var
  Found: TFindRec;
begin
  Result := False;
  if FindFirst(Path, Found) then begin
    Result := (Found.Attributes and (DirectoryAttribute or ReparsePointAttribute)) = 0;
    FindClose(Found);
  end;
end;

// Whether Folder holds an install of ours, as the contract's marker of an install by a setup of ours proves it: a plain
// file qkvm-desktop.installed there, of its six bytes ("setup" and a line feed), as the program reads it. Only where one
// is does a setup set anything of the folder aside, put the program's own file of 0.1.2 right, or remove what an earlier
// version installed: a folder that holds no install of ours is not the setup's to touch (review i020-2, M1).
function OurInstallIn(Folder: String): Boolean;
var
  Found: TFindRec;
  Marker: String;
  Said: AnsiString;
begin
  Result := False;
  Marker := AddBackslash(Folder) + 'qkvm-desktop.installed';
  if not FindFirst(Marker, Found) then
    Exit;
  try
    if ((Found.Attributes and (DirectoryAttribute or ReparsePointAttribute)) <> 0) or (Found.SizeHigh <> 0) or (Found.SizeLow <> 6) then
      Exit;
  finally
    FindClose(Found);
  end;
  if LoadStringFromFile(Marker, Said) then
    Result := Said = 'setup' + #10;
end;

// Whether a name is the setup's own: one of its package's, or one a published version of ours installed. (One that holds a
// |, which no name of Windows' holds, is none: two names of the list side by side are not one, review i020-2 L6.)
function OwnName(Name: String): Boolean;
begin
  Result := (Name <> '') and (Pos('|', Name) = 0) and (Pos('|' + Lowercase(Name) + '|', Lowercase(PackageFiles + EarlierFiles)) > 0);
end;

// Whether an ending is one a set-aside gives (and one, never two of them side by side).
function OwnEnding(Suffix: String): Boolean;
begin
  Result := (Suffix <> '') and (Pos('|', Suffix) = 0) and (Pos('|' + Suffix + '|', SetAsideEndings) > 0);
end;

// Whether what is at Path is of at most Most bytes, as a look in its folder shows it.
function AtMost(Path: String; Most: Cardinal): Boolean;
var
  Found: TFindRec;
begin
  Result := False;
  if FindFirst(Path, Found) then begin
    Result := (Found.SizeHigh = 0) and (Found.SizeLow <= Most);
    FindClose(Found);
  end;
end;

// A marker's record read, as SetPackageAside writes it: True where it is whole and of RecordKind, says its ending once,
// first and as one of the setup's, and names only the setup's own files. Then its ending, and for each file it names,
// its name, its ending and its kind: AsideKind, renamed by that set-aside (put back where the copy did not finish,
// removed where it did); LeftKind, what an earlier update could not remove, carried with its own ending (removed).
// And apart, in Copies, each file that copy writes (its "copy" lines, review i020-1 L3): what of them is there and was
// not set aside, the copy made, and a put-back removes it. False: nothing is decided by it, and nothing it names is
// touched.
function ReadRecord(Path: String; var Suffix: String; Names, Endings, Kinds, Copies: TStringList): Boolean;
var
  Lines: TArrayOfString;
  I, Count, At: Integer;
  Line, Value: String;
begin
  Result := False;
  Suffix := '';
  Names.Clear;
  Endings.Clear;
  Kinds.Clear;
  Copies.Clear;
  if not PlainFileAt(Path) then
    Exit;
  // (Of at most MostRecord, before it is read: a larger file of anything else's is not loaded whole, review i020-2 L6.)
  if not AtMost(Path, MostRecord) then
    Exit;
  if not LoadStringsFromFile(Path, Lines) then
    Exit;
  Count := GetArrayLength(Lines);
  while (Count > 0) and (Lines[Count - 1] = '') do
    Count := Count - 1;
  if Count < 4 then
    Exit;
  if (Lines[1] <> RecordKind) or (Lines[Count - 1] <> RecordEnd) or (Copy(Lines[2], 1, 7) <> 'ending ') then
    Exit;
  Suffix := Copy(Lines[2], 8, Length(Lines[2]));
  if not OwnEnding(Suffix) then
    Exit;
  for I := 3 to Count - 2 do begin
    Line := Lines[I];
    if Copy(Line, 1, 6) = 'aside ' then begin
      if not OwnName(Copy(Line, 7, Length(Line))) then
        Exit;
      Names.Add(Copy(Line, 7, Length(Line)));
      Endings.Add(Suffix);
      Kinds.Add(AsideKind);
    end else if Copy(Line, 1, 5) = 'left ' then begin
      Value := Copy(Line, 6, Length(Line));
      At := Pos(' ', Value);
      if At = 0 then
        Exit;
      if not OwnEnding(Copy(Value, 1, At - 1)) or not OwnName(Copy(Value, At + 1, Length(Value))) then
        Exit;
      Names.Add(Copy(Value, At + 1, Length(Value)));
      Endings.Add(Copy(Value, 1, At - 1));
      Kinds.Add(LeftKind);
    end else if Copy(Line, 1, 5) = 'copy ' then begin
      if not OwnName(Copy(Line, 6, Length(Line))) then
        Exit;
      Copies.Add(Copy(Line, 6, Length(Line)));
    end else
      Exit;
  end;
  Result := True;
end;

// What the records of the markers in Folder name, with their endings, and those markers: the files a setup of ours leaves
// there where an update did not finish or an uninstall could not remove all it set aside, by their names under Folder,
// small letters, each followed by |. A marker whose record does not read is not among them.
function LeftoversOf(Folder: String): String;
var
  Names, Endings, Kinds, Copies: TStringList;
  Markers: TArrayOfString;
  Suffix: String;
  M, I: Integer;
begin
  Result := '|';
  SetArrayLength(Markers, 2);
  Markers[0] := FinishingMarker;
  Markers[1] := UnfinishedMarker;
  Names := TStringList.Create;
  Endings := TStringList.Create;
  Kinds := TStringList.Create;
  Copies := TStringList.Create;
  try
    for M := 0 to 1 do
      if ReadRecord(AddBackslash(Folder) + Markers[M], Suffix, Names, Endings, Kinds, Copies) then begin
        Result := Result + Lowercase(Markers[M]) + '|';
        for I := 0 to Names.Count - 1 do
          Result := Result + Lowercase(Names[I] + Endings[I]) + '|';
      end;
  finally
    Names.Free;
    Endings.Free;
    Kinds.Free;
    Copies.Free;
  end;
end;

// Whether a folder's name under the program's folder is one of the setup's own: one that holds, at any depth, a file of
// its package's or one a published version of ours installed (package.iss, earlier.iss).
function OwnFolderName(Relative: String): Boolean;
begin
  Result := (Relative <> '') and (Pos('|', Relative) = 0) and (Pos('|' + Lowercase(Relative) + '\', Lowercase(PackageFiles + EarlierFiles)) > 0);
end;

// The first entry within Relative ('' for Folder itself) that is not what a setup of ours leaves there, by its name under
// Folder ('' where there is none): a file Leftovers does not name, a folder of a name not the setup's own (OwnFolderName),
// or a redirection; within a folder of the setup's own, the same, looked through. A folder that cannot be looked through,
// or not to its end, says why in CutShort (as FilesInUse's, review F3). A look alone, which changes nothing.
function OtherWithin(Folder, Relative, Leftovers: String; var CutShort: String): String;
var
  Found: TFindRec;
  Base, Name, Data, Entry: String;
  Attributes: Cardinal;
  Error: Integer;
begin
  Result := '';
  Base := RemoveBackslashUnlessRoot(Folder);
  if Relative <> '' then
    Base := AddBackslash(Base) + Relative;
  if not FindFirst(AddBackslash(Base) + '*', Found) then begin
    CutShort := 'the folder ' + Base + ' cannot be looked through, to see that it holds nothing but qKVM Desktop';
    Exit;
  end;
  try
    Name := Found.Name;
    Attributes := Found.Attributes;
    while (Result = '') and (CutShort = '') do begin
      if (Name <> '.') and (Name <> '..') then begin
        Entry := Name;
        if Relative <> '' then
          Entry := Relative + '\' + Name;
        if (Attributes and ReparsePointAttribute) <> 0 then
          Result := Entry
        else if (Attributes and DirectoryAttribute) <> 0 then begin
          if OwnFolderName(Entry) then
            Result := OtherWithin(Folder, Entry, Leftovers, CutShort)
          else
            Result := Entry;
        end else if Pos('|' + Lowercase(Entry) + '|', Leftovers) = 0 then
          Result := Entry;
      end;
      if (Result = '') and (CutShort = '') then begin
        Data := StringOfChar(' ', FindDataChars);
        if FindNextFileW(Found.FindHandle, Data) = 0 then begin
          Error := DLLGetLastError;
          if Error <> ErrorNoMoreFiles then
            CutShort := Format('the folder %s could not be looked through to its end, to see that it holds nothing but qKVM Desktop (error %d: %s)', [Base, Error, SysErrorMessage(Error)]);
          Break;
        end;
        Name := Copy(Data, FindNameAt, FindNameChars);
        if Pos(#0, Name) = 0 then begin
          CutShort := 'the folder ' + Base + ' could not be looked through to its end, to see that it holds nothing but qKVM Desktop (a name in it could not be read)';
          Break;
        end;
        Name := Copy(Name, 1, Pos(#0, Name) - 1);
        Attributes := Ord(Data[1]);
      end;
    end;
  finally
    FindClose(Found);
  end;
end;

// Why Folder, where no install of ours is, holds more than a setup of ours leaves there ('' where it holds nothing else,
// or nothing at all): the markers of an update whose records read, the files those records name with their endings, and
// folders of the setup's own names that hold nothing else (what an uninstall leaves that could not remove all an update
// had set aside, review 1.8). Anything else (another program's, a person's, a copy from the ZIP, a file of the package's
// own name, a record that does not read) is another's, and the setup neither installs into it nor acts there on a
// record it holds (review 1.1). A folder that cannot be looked through, or not to its end, is not known to hold nothing.
function LeftoversOnlyReason(Folder: String): String;
var
  Other, CutShort: String;
begin
  CutShort := '';
  Other := OtherWithin(Folder, '', LeftoversOf(Folder), CutShort);
  if CutShort <> '' then
    Result := CutShort
  else if Other <> '' then
    Result := Format('the folder %s holds what is not an install of qKVM Desktop (%s): qKVM Desktop is installed only into a folder of its own, or an empty one', [RemoveBackslashUnlessRoot(Folder), Other])
  else
    Result := '';
end;

// Whether a file is on the disk as it was written: Windows' own flush of it, on a handle of its own. (A handle that is
// none is no flush; closing it is nothing.)
function FlushedFile(Path: String): Boolean;
var
  Handle: THandle;
begin
  Handle := CreateFileW(Path, GenericWrite, FileShareAll, 0, OpenExisting, 0, 0);
  Result := FlushFileBuffers(Handle) <> 0;
  CloseHandle(Handle);
end;

// A file of the setup's own removed (what it set aside, what an earlier update could not remove, a file of a copy that
// did not complete, the markers of an update and the drafts of the record and of the update check, review i020-2 L5):
// its read-only attribute cleared first (a rename keeps it, and a delete refuses it), then Windows'
// delete, tried again twice a quarter of a second apart (a scanner of the PC that has just opened it lets go of it).
// Whether it is gone. One that is not stays where it is, and the caller says so; it never stops a setup (review
// i020-1, M2).
function RemoveOwn(Path: String): Boolean;
var
  Attributes, Tries: Integer;
begin
  Attributes := GetFileAttributesW(Path);
  if Attributes <> NoFileAttributes then
    if (Attributes and ReadOnlyAttribute) <> 0 then
      SetFileAttributesW(Path, Attributes and not ReadOnlyAttribute);
  Result := DeleteFile(Path);
  Tries := 1;
  while not Result and (Tries < 3) do begin
    Sleep(250);
    Result := DeleteFile(Path);
    Tries := Tries + 1;
  end;
end;

// qKVM Setup's update check (ui/win/update_check.cpp: Windows' own calls and its own C runtime alone), unpacked from this
// setup into its temporary folder, once. Whether it is there.
function UpdateCheckReady: Boolean;
begin
  Result := FileExists(ExpandConstant('{tmp}\') + UpdateCheck);
  if not Result then
    try
      ExtractTemporaryFile(UpdateCheck);
      Result := FileExists(ExpandConstant('{tmp}\') + UpdateCheck);
    except
      Log('The setup''s update check could not be unpacked: ' + GetExceptionMessage);
    end;
end;

// The update check put at Path, the program's own name, where nothing is (review i020-1, M3): whatever starts qKVM there
// while the earlier version is set aside (the Start menu, the start with Windows) starts it, and it says that an update
// did not finish, where it would otherwise find no program, or a program beside only some of its libraries. Copied
// first under a name of the setup's own beside it (CheckDraft), flushed by Windows, then renamed into the program's place
// in one step (review i020-2, L4), as the record is: a cut while it is copied leaves the draft alone, which the next
// placing and the uninstall remove, and never a file cut short at the program's name, which Windows would not start.
// Whether it is there (no draft is left where it is not).
function PlaceUpdateCheck(Path: String): Boolean;
var
  Draft: String;
begin
  Result := False;
  if not UpdateCheckReady then
    Exit;
  Draft := ExtractFilePath(Path) + CheckDraft;
  if ThingAt(Draft) then
    if not RemoveOwn(Draft) then
      Exit;
  if CopyFile(ExpandConstant('{tmp}\') + UpdateCheck, Draft, True) then
    if FlushedFile(Draft) then
      Result := RenameFile(Draft, Path);
  if not Result then
    if ThingAt(Draft) then
      RemoveOwn(Draft);
end;

// Whether the file at Path is this setup's own update check, by its bytes (the SHA-256 of the one it carries): what a
// put-back takes away from the program's place where the earlier program is gone (review i020-2, L3). False where either
// cannot be read.
function IsOwnUpdateCheck(Path: String): Boolean;
begin
  Result := False;
  if PlainFileAt(Path) and UpdateCheckReady then
    try
      Result := GetSHA256OfFile(Path) = GetSHA256OfFile(ExpandConstant('{tmp}\') + UpdateCheck);
    except
      Log('What is at the program''s name could not be read, to tell whether it is the setup''s update check: ' + GetExceptionMessage);
    end;
end;

// The record written whole, and on the disk, before it is the marker Marker (the unfinished one; or the finishing one,
// FinishCopy): under a name of the setup's own first (RecordDraft), flushed, then renamed to the marker in one step. So a
// marker there always holds a whole record, and the record is on the disk before anything is renamed; a cut while it is
// written leaves the draft alone, which the next set-aside and the uninstall remove. '' where the marker holds it, else
// why not (nothing written is left).
function WriteRecord(Text, Marker: String): String;
var
  Draft: String;
begin
  Result := 'the record of what the copy sets aside could not be written in the program''s folder (' + Marker + ')';
  Draft := InApp(RecordDraft);
  if ThingAt(Draft) then
    if not RemoveOwn(Draft) then
      Exit;
  if SaveStringToFile(Draft, Text, False) then
    if FlushedFile(Draft) then
      if RenameFile(Draft, InApp(Marker)) then
        Result := '';
  if Result <> '' then
    if ThingAt(Draft) then
      RemoveOwn(Draft);
end;

// (This setup's hold of its marker let go of, where it has one.)
procedure LetGoOfMarker;
begin
  if MarkerHold <> 0 then
    CloseHandle(MarkerHold);
  MarkerHold := 0;
end;

// The unfinished marker this setup wrote, held open from now on without the right for anything else to remove it (only
// to read it, as the program does): nothing removes it while this setup works (review i020-2, L1), and a start of the
// program, which takes away a marker whose record names nothing set aside yet, does so only while it holds this
// account's gate, which this setup holds from its start (review 1.4: the instant between the marker's rename and this
// hold too). Let go of before the marker is renamed, and before this setup puts anything right (LetGoOfMarker). Whether
// it is held: a handle of a file on a disk, and the marker there holding the record this setup wrote (Text); where it is
// not, nothing is held, and nothing is set aside (review 1.4).
function HoldMarker(Text: String): Boolean;
var
  Said: AnsiString;
  Back: String;
begin
  MarkerHold := CreateFileW(InApp(UnfinishedMarker), GenericRead, FileShareReadWrite, 0, OpenExisting, 0, 0);
  Result := GetFileType(MarkerHold) = FileTypeDisk;
  if Result then begin
    Result := LoadStringFromFile(InApp(UnfinishedMarker), Said);
    Back := Said;
    Result := Result and (Back = Text);
  end;
  if not Result then begin
    Log('The marker of this update could not be held, or does not hold this setup''s record: nothing is set aside.');
    LetGoOfMarker;
  end;
end;

// The ending this set-aside gives what it renames: the first of SetAsideEndings that no file of the package's has
// beside it, so that a file of a set-aside ending which is not the setup's own (a person's, or what an earlier update
// could not remove) is never in its way, and never touched. '' where every one is taken.
function FreeEnding: String;
var
  Names: TStringList;
  Rest, Suffix: String;
  I, At: Integer;
  Taken: Boolean;
begin
  Result := '';
  Names := TStringList.Create;
  try
    AddNames(PackageFiles, Names);
    Rest := Copy(SetAsideEndings, 2, Length(SetAsideEndings));
    At := Pos('|', Rest);
    while (At > 0) and (Result = '') do begin
      Suffix := Copy(Rest, 1, At - 1);
      Rest := Copy(Rest, At + 1, Length(Rest));
      At := Pos('|', Rest);
      Taken := False;
      for I := 0 to Names.Count - 1 do
        if ThingAt(InApp(Names[I]) + Suffix) then
          Taken := True;
      if not Taken then
        Result := Suffix;
    end;
  finally
    Names.Free;
  end;
end;

// What this setup put right of an update that did not finish, added to what it says of it (PutRightDone), in the order
// it was done (review i020-2, L9).
procedure SayPutRight(What: String);
begin
  if PutRightDone = '' then
    PutRightDone := What
  else if Pos(What, PutRightDone) = 0 then
    PutRightDone := PutRightDone + ', and ' + What;
end;

// The result line of exit code 7 (the contract): "nothing was changed: <why>" where nothing was; where this setup put
// right first what an update that did not finish left, what it put right and that it changed nothing else, never that
// nothing was changed (review i020-2, L9).
function UnchangedLine(Reason: String): String;
begin
  if PutRightDone = '' then
    Result := 'qKVM-RESULT: 7 nothing was changed: ' + Reason
  else
    Result := 'qKVM-RESULT: 7 first, Setup ' + PutRightDone + ', and changed nothing else: ' + Reason;
end;

// The program's own file that a setup of 0.1.2 or before set aside, alone and with no marker (0.1.2's own rule, by its
// exact name): removed where the program is there, else put back, as those setups' next one does; only in a folder that
// holds an install of ours (those setups wrote the contract's marker first, review i020-2 M1).
procedure PutProgramRight(Folder: String);
var
  Base, Aside: String;
begin
  if not OurInstallIn(Folder) then
    Exit;
  Base := AddBackslash(Folder) + 'qkvm-desktop.exe';
  Aside := Base + AsideEnding;
  if not PlainFileAt(Aside) then
    Exit;
  if ThingAt(Base) then begin
    if DeleteFile(Aside) then begin
      Log('Removed the earlier program, set aside by a setup before.');
      SayPutRight('removed the earlier program that a setup of 0.1.2 had set aside, as a program stood in its place');
    end;
  end else if RenameFile(Aside, Base) then begin
    Log('Put the earlier program back, set aside by a setup ended halfway.');
    SayPutRight('put back the earlier program that a setup of 0.1.2 had set aside');
  end else
    Log('Could not put the earlier program back.');
end;

// What a finishing update left (its marker there: its copy was whole), put right: every file its record names removed
// (what it set aside, and what an earlier update could not remove), each a plain file reached through no redirection;
// then the marker. One that cannot be removed stays, named in the record, for the next setup, and stops nothing. ''
// where the record could be read, else why not (a later version's, or not ours: nothing is touched).
function FinishRecorded(Folder: String): String;
var
  Names, Endings, Kinds, Copies: TStringList;
  Marker, Suffix, Aside: String;
  I, Removed, Left: Integer;
  There: Boolean;
begin
  Result := '';
  Marker := AddBackslash(Folder) + FinishingMarker;
  if not ThingAt(Marker) then
    Exit;
  Removed := 0;
  Left := 0;
  Names := TStringList.Create;
  Endings := TStringList.Create;
  Kinds := TStringList.Create;
  Copies := TStringList.Create;
  try
    if not ReadRecord(Marker, Suffix, Names, Endings, Kinds, Copies) then begin
      Result := Format('the record of an update is not one this setup can read (%s)', [FinishingMarker]);
      Exit;
    end;
    for I := 0 to Names.Count - 1 do begin
      Aside := AddBackslash(Folder) + Names[I] + Endings[I];
      // (One that cannot be looked at is not taken for gone: it stays named, review 1.5.)
      if not LookAt(Aside, There) then begin
        Left := Left + 1;
        Log('Could not look at what an update set aside: ' + Names[I] + Endings[I]);
      end else if There then begin
        Left := Left + 1;
        // (What the set-aside renamed goes only where the file of its own name is there beside it, as the whole copy
        // left it: never the one copy of a file that has gone since, review i020-1 M1.)
        if PlainFileAt(Aside) then
          if (Kinds[I] = LeftKind) or PlainFileAt(AddBackslash(Folder) + Names[I]) then
            if not ThroughRedirection(Folder, RemoveBackslash(ExtractFilePath(Names[I]))) then
              if RemoveOwn(Aside) then begin
                Left := Left - 1;
                Removed := Removed + 1;
              end;
        if ThingAt(Aside) then
          Log('Could not remove yet what an update set aside: ' + Names[I] + Endings[I]);
      end;
    end;
  finally
    Names.Free;
    Endings.Free;
    Kinds.Free;
    Copies.Free;
  end;
  Log(Format('Finished an update whose copy was whole: removed the earlier version it had set aside (%d files).', [Removed]));
  if Removed > 0 then
    SayPutRight('removed the earlier version that an update whose copy was whole had set aside');
  // (The marker last, once all is removed: a cut before this leaves it, and the next setup does this again. One that
  // cannot be removed is said, and is stale: nothing its record names is there.)
  if Left > 0 then
    Log(Format('What an update set aside could not all be removed (%d files): they stay named in its record, and the next setup removes them.', [Left]))
  else if not RemoveOwn(Marker) then
    Log('Could not remove the marker of a finishing update: ' + Marker);
end;

// What an unfinished update left (its marker there: its copy did not finish), put right, in this order (review i020-1,
// M3 and L3): first the program's own name, where the earlier program is set aside or none was there: what is there now
// (the update check, or the copy's own program) removed and this setup's update check put there, so that a cut while
// the earlier version comes back leaves the update check, and never a program beside some of its libraries; then every
// other file its record names as set aside by it (AsideKind) put back where it was, the copy's file of that name
// removed first, from the end; then each file the copy writes ("copy") that is there and was not set aside, which only
// the copy made, removed (one that cannot be, or cannot be looked at, stops the put-back there with the marker kept, so
// that the earlier version is never said whole beside it, review 1.5); then the earlier program
// back in its place, the update check removed first (or, where none was there, the update check removed); then what an
// earlier update could not remove, carried in the record (LeftKind), removed; then the marker (or, where something
// carried is still there, the marker made that of a finishing update, whose record names it). Nothing else is touched:
// a file of a set-aside ending that the record does not name is not the setup's own. Nothing that cannot be looked at
// is taken for not there: a put-back counts as whole only where every file its record names was seen, and as it should
// be (review 1.5). '' where all is right, else why not, naming the file (what could be put back, is).
function PutBackRecorded(Folder: String): String;
var
  Names, Endings, Kinds, Copies: TStringList;
  Marker, Suffix, Base, Aside, ProgramFile, ProgramAside, SetAsideNames: String;
  I, Count, Left: Integer;
  ProgramSetAside, ProgramCopied, ProgramGone, There, ProgramThere: Boolean;
begin
  Result := '';
  Marker := AddBackslash(Folder) + UnfinishedMarker;
  if not ThingAt(Marker) then
    Exit;
  Count := 0;
  Left := 0;
  Names := TStringList.Create;
  Endings := TStringList.Create;
  Kinds := TStringList.Create;
  Copies := TStringList.Create;
  try
    if not ReadRecord(Marker, Suffix, Names, Endings, Kinds, Copies) then begin
      Result := Format('the record of an update is not one this setup can read (%s)', [UnfinishedMarker]);
      Exit;
    end;
    ProgramFile := AddBackslash(Folder) + 'qkvm-desktop.exe';
    ProgramAside := '';
    SetAsideNames := '|';
    for I := 0 to Names.Count - 1 do
      if Kinds[I] = AsideKind then begin
        SetAsideNames := SetAsideNames + Lowercase(Names[I]) + '|';
        if Names[I] = 'qkvm-desktop.exe' then
          ProgramAside := ProgramFile + Endings[I];
      end;
    // (Whatever cannot be looked at is never taken for not there: a put-back is whole only where every file its record
    // names was seen, and as it should be; review 1.5. What could not be is said, and the marker stays with its record.)
    ProgramSetAside := False;
    ProgramThere := False;
    if (ProgramAside <> '') and not LookAt(ProgramAside, ProgramSetAside) then
      Result := 'what an update set aside could not be looked at (qkvm-desktop.exe' + Suffix + ')';
    if (Result = '') and not LookAt(ProgramFile, ProgramThere) then
      Result := 'what is at the program''s name could not be looked at (qkvm-desktop.exe)';
    ProgramCopied := (ProgramAside = '') and (Copies.IndexOf('qkvm-desktop.exe') >= 0);
    // (Named set aside, and gone since, with nothing at its name but this setup's update check, or nothing: review i020-2,
    // L3. A program there is the earlier one put back already by a put-back whose marker stayed, and is whole.)
    ProgramGone := (Result = '') and (ProgramAside <> '') and not ProgramSetAside and (not ProgramThere or IsOwnUpdateCheck(ProgramFile));
    // (The program's own name first: the update check there while the rest comes back.)
    if (Result = '') and (ProgramSetAside or ProgramCopied) then begin
      if ThingAt(ProgramFile) then
        if not RemoveOwn(ProgramFile) then
          Result := 'a file of a copy that did not complete could not be removed, to put back the earlier one (qkvm-desktop.exe)';
      if Result = '' then
        if not PlaceUpdateCheck(ProgramFile) then
          Log('The setup''s update check could not be put in the program''s place while the earlier version is put back.');
    end;
    // (From the end: every library, before the program.)
    for I := Names.Count - 1 downto 0 do begin
      Base := AddBackslash(Folder) + Names[I];
      Aside := Base + Endings[I];
      if (Result = '') and (Kinds[I] = AsideKind) and (Names[I] <> 'qkvm-desktop.exe') and not LookAt(Aside, There) then
        Result := Format('what an update set aside could not be looked at (%s)', [Names[I] + Endings[I]]);
      if (Result = '') and (Kinds[I] = AsideKind) and (Names[I] <> 'qkvm-desktop.exe') and There then begin
        if not PlainFileAt(Aside) or (ThingAt(Base) and not PlainFileAt(Base)) or ThroughRedirection(Folder, RemoveBackslash(ExtractFilePath(Names[I]))) then
          Result := Format('what an update set aside is not a plain file of qKVM Desktop''s (%s)', [Names[I] + Endings[I]])
        else begin
          if ThingAt(Base) then
            if not RemoveOwn(Base) then
              Result := Format('a file of a copy that did not complete could not be removed, to put back the earlier one (%s)', [Names[I]]);
          if Result = '' then begin
            if RenameFile(Aside, Base) then
              Count := Count + 1
            else
              Result := Format('a file of the earlier version could not be put back (%s)', [Names[I]]);
          end;
        end;
      end;
    end;
    // (What the copy made that the earlier version had not: there, and not set aside. One that cannot be looked at, or
    // is not a plain file of ours, or cannot be removed, stops the put-back there, the marker kept with its record, so
    // that the earlier version is never said to be whole beside it: the next setup, or this one's next try, removes it,
    // and a start of the program meanwhile says that an update did not finish; review 1.5.)
    for I := 0 to Copies.Count - 1 do begin
      Base := AddBackslash(Folder) + Copies[I];
      if (Result = '') and (Copies[I] <> 'qkvm-desktop.exe') and (Pos('|' + Lowercase(Copies[I]) + '|', SetAsideNames) = 0) then begin
        if not LookAt(Base, There) then
          Result := Format('what a copy that did not complete made could not be looked at (%s)', [Copies[I]])
        else if There then begin
          if not PlainFileAt(Base) or ThroughRedirection(Folder, RemoveBackslash(ExtractFilePath(Copies[I]))) then
            Result := Format('what a copy that did not complete made is not a plain file of qKVM Desktop''s (%s)', [Copies[I]])
          else if RemoveOwn(Base) then
            Log('Removed what the copy that did not complete had made: ' + Copies[I])
          else
            Result := Format('what a copy that did not complete made could not be removed (%s)', [Copies[I]]);
        end;
      end;
    end;
    // (The program last: the update check removed, the earlier program renamed back; or, where none was there, the
    // update check removed.)
    if (Result = '') and ProgramSetAside then begin
      if not PlainFileAt(ProgramAside) then
        Result := 'what an update set aside is not a plain file of qKVM Desktop''s (qkvm-desktop.exe' + Suffix + ')'
      else if ThingAt(ProgramFile) and not RemoveOwn(ProgramFile) then
        Result := 'a file of a copy that did not complete could not be removed, to put back the earlier one (qkvm-desktop.exe)'
      else if RenameFile(ProgramAside, ProgramFile) then
        Count := Count + 1
      else
        Result := 'a file of the earlier version could not be put back (qkvm-desktop.exe)';
    end;
    if (Result = '') and ProgramCopied and PlainFileAt(ProgramFile) then
      if not RemoveOwn(ProgramFile) then
        Log('Could not remove what the copy that did not complete had made: qkvm-desktop.exe');
    // (Where the earlier program is gone since it was set aside (a setup of 0.1.2 removed it, as its own rule says where
    // something stands at the program's name), this setup's update check there is taken away too, by its bytes: none is
    // left at the program's name without a marker, and the earlier version is said to be back all but its program, never
    // whole (review i020-2, L3). Anything else there stays.)
    if (Result = '') and ProgramGone and IsOwnUpdateCheck(ProgramFile) then
      if not RemoveOwn(ProgramFile) then
        Result := 'the setup''s update check in the program''s place could not be taken away, and the earlier program is gone (qkvm-desktop.exe): close its box, then run the setup again';
    if Result <> '' then begin
      // (What could be put back, is: said so, never that nothing was changed, L9.)
      if Count > 0 then
        SayPutRight('put back part of the earlier version, which an update that did not finish had set aside');
      Exit;
    end;
    if ProgramGone then begin
      EarlierProgramGone := True;
      Log(Format('Put back what an update that did not finish had set aside (%d files), all but the earlier program, which was gone: no program of qKVM Desktop is in its folder until a setup completes.', [Count]));
      SayPutRight('put back what an update that did not finish had set aside, all but the earlier program, which was gone');
    end else begin
      Log(Format('Put the earlier version back, which an update that did not finish had set aside (%d files).', [Count]));
      if Count > 0 then
        SayPutRight('put back the earlier version, which an update that did not finish had set aside');
    end;
    for I := 0 to Names.Count - 1 do begin
      Aside := AddBackslash(Folder) + Names[I] + Endings[I];
      if (Kinds[I] = LeftKind) and not LookAt(Aside, There) then
        Left := Left + 1
      else if (Kinds[I] = LeftKind) and There then begin
        Left := Left + 1;
        if PlainFileAt(Aside) then
          if not ThroughRedirection(Folder, RemoveBackslash(ExtractFilePath(Names[I]))) then
            if RemoveOwn(Aside) then
              Left := Left - 1;
      end;
    end;
  finally
    Names.Free;
    Endings.Free;
    Kinds.Free;
    Copies.Free;
  end;
  // (The marker last, once all is right: a cut before this leaves it, and the next setup does this again. One that
  // cannot be removed is said, and is stale: nothing its record set aside is there.)
  if Left = 0 then begin
    if not RemoveOwn(Marker) then
      Log('Could not remove the marker of an update that did not finish: ' + Marker);
  end else if not ThingAt(AddBackslash(Folder) + FinishingMarker) and RenameFile(Marker, AddBackslash(Folder) + FinishingMarker) then
    Log(Format('What an earlier update set aside could not all be removed (%d files): they stay named in the record, and the next setup removes them.', [Left]))
  else
    Log('Could not keep the record of what an earlier update set aside: ' + Marker);
end;

// Every file the records of Folder's markers name (what an update set aside, and what an earlier one could not remove),
// each a plain file reached through no redirection, removed; then each marker whose record names nothing that is still
// there. A marker whose record names what could not be removed stays with it, as that of a finishing update (an
// unfinished one renamed so, where it can be): a later setup removes what it names, and never puts it back into a folder
// emptied of its install (review i020-1, L2). Nothing else: a file of a set-aside ending that no record names is not the
// setup's. Where an install of ours is there (Installed: the uninstall's own folder), what a copy that did not finish
// wrote too, by the record's "copy" lines, which no uninstall log of Inno Setup's names; where none is (what an uninstall
// left, review 1.8), never a file of the package's own name, whatever a record says.
procedure DropRecorded(Folder: String; Installed: Boolean);
var
  Names, Endings, Kinds, Copies: TStringList;
  Markers: TArrayOfString;
  M, I, Left: Integer;
  Suffix, Aside: String;
  There: Boolean;
begin
  SetArrayLength(Markers, 2);
  Markers[0] := AddBackslash(Folder) + FinishingMarker;
  Markers[1] := AddBackslash(Folder) + UnfinishedMarker;
  Names := TStringList.Create;
  Endings := TStringList.Create;
  Kinds := TStringList.Create;
  Copies := TStringList.Create;
  try
    for M := 0 to 1 do
      if ReadRecord(Markers[M], Suffix, Names, Endings, Kinds, Copies) then begin
        Left := 0;
        if Installed and (M = 1) then
          for I := 0 to Copies.Count - 1 do
            if PlainFileAt(AddBackslash(Folder) + Copies[I]) then
              if not ThroughRedirection(Folder, RemoveBackslash(ExtractFilePath(Copies[I]))) then
                RemoveOwn(AddBackslash(Folder) + Copies[I]);
        for I := 0 to Names.Count - 1 do begin
          Aside := AddBackslash(Folder) + Names[I] + Endings[I];
          // (One that cannot be looked at stays named, as one that cannot be removed, review 1.5.)
          if not LookAt(Aside, There) then
            Left := Left + 1
          else if PlainFileAt(Aside) then
            if not ThroughRedirection(Folder, RemoveBackslash(ExtractFilePath(Names[I]))) then begin
              if RemoveOwn(Aside) then
                Log('Removed what an update had set aside: ' + Names[I] + Endings[I])
              else
                Left := Left + 1;
            end;
        end;
        if Left = 0 then
          RemoveOwn(Markers[M])
        else begin
          Log(Format('What an update set aside could not all be removed (%d files): they stay, named in its record.', [Left]));
          if M = 1 then
            if not ThingAt(Markers[0]) then
              RenameFile(Markers[1], Markers[0]);
        end;
      end;
  finally
    Names.Free;
    Endings.Free;
    Kinds.Free;
    Copies.Free;
  end;
end;

// What an update that did not finish left in Folder, put right: by every setup, in any mode, first (InitializeSetup,
// before its refusal of an older version, in the folder this setup installs into as far as it can tell it there), again
// as Preparing to Install sets the package aside, and as the copy ends. '' where all is right (or nothing was to be put
// right), else why not, naming the file (what could be put right, is).
// - The marker of a finishing update there (the copy was whole): every file its record names is removed (FinishRecorded).
// - The marker of an unfinished one: every file its record set aside is put back (PutBackRecorded). The earlier version
//   is whole again. (Both may be there: a finishing update's record names other files, of another ending, than an
//   unfinished one set aside after it.)
// - Neither: the program's own file that a setup of 0.1.2 or before set aside (PutProgramRight).
// A record that does not read is not decided by: the setup refuses, naming the marker, and touches nothing it names. And
// a record decides only in a folder of ours, which is decided first (review 1.1): one that holds an install of ours
// (OurInstallIn); or one that holds nothing but what a setup of ours leaves there (LeftoversOnlyReason: an uninstall that
// could not remove all an update had set aside, review 1.8), where every file its records name is removed and nothing
// is put back, as no install of ours is there to put it back into (DropRecorded). In any other folder nothing is touched,
// and the setup does not go on, naming what is there. And only in a folder that no other account may change, which its
// callers decide before it: InitializeSetup by FolderRightsReason, Preparing to Install by RightsReason, and the copy
// and its put-back come after that (outside review 2, 1.3).
function PutRight(Folder: String): String;
var
  Unfinished: Boolean;
begin
  Result := '';
  // (This setup's hold of its own marker let go of first: what it puts right may remove or rename it, L1.)
  LetGoOfMarker;
  if not ThingAt(AddBackslash(Folder) + UnfinishedMarker) and not ThingAt(AddBackslash(Folder) + FinishingMarker) then begin
    PutProgramRight(Folder);
    Exit;
  end;
  if not OurInstallIn(Folder) then begin
    Result := LeftoversOnlyReason(Folder);
    if Result = '' then begin
      Log('The folder holds no install of qKVM Desktop, only what a setup of it left: what the records name is removed, and nothing is put back.');
      DropRecorded(Folder, False);
      SayPutRight('removed what an uninstall had left of what an update set aside');
    end;
    Exit;
  end;
  Result := FinishRecorded(Folder);
  if Result = '' then begin
    Unfinished := ThingAt(AddBackslash(Folder) + UnfinishedMarker);
    Result := PutBackRecorded(Folder);
    // (And what the copy that was cut left under Inno Setup's temporary names, a file it was writing: the folder is then
    // the earlier version's and nothing more, as after a whole copy (step 41 of test_setup_vm). Only where the
    // put-back is whole: one that stopped keeps its marker, and the next setup does this again.)
    if Unfinished and (Result = '') then
      RemoveSetupLeftoversOf(Folder);
  end;
end;

// The version of ours installed in this very folder before this copy, as its own record proves it: this user's Settings >
// Apps entry of this setup's AppId names {app} as its folder (Inno Setup's own record of where it installed) and that
// version, and the contract's marker of an install by a setup of ours is there (OurInstallIn; the version is not in it,
// which the contract keeps to those six bytes). '' where nothing proves one: a first install, or a folder that /DIR
// names where no qKVM was installed; nothing of an earlier version is removed then (review i020-1, M4).
function InstalledHere: String;
begin
  Result := '';
  if (InstalledVersion = '') or (InstalledFolder = '') then
    Exit;
  if not PathSame(InstalledFolder, RemoveBackslashUnlessRoot(ExpandConstant('{app}'))) then
    Exit;
  if OurInstallIn(ExpandConstant('{app}')) then
    Result := InstalledVersion;
end;

// Why this setup does not install into the program's folder ('' where it does): it installs only into a folder that is
// not there yet, or holds an install of ours (OurInstallIn), or holds nothing but what a setup of ours leaves there (none
// at all, or what an uninstall could not remove of what an update set aside, review 1.8: LeftoversOnlyReason). A folder
// that holds anything else, of whatever names, is another's (a program's, a person's, a copy from the ZIP): nothing in it
// is set aside, written over or removed, and Preparing to Install refuses it before anything is closed or changed (review
// i020-2, M1; SPEC, M4). A look at its entries alone, and within folders of the setup's own names, never a change; a
// folder that cannot be looked through to its end is not known to hold nothing (as FilesInUse's, review F3).
function ForeignFolderReason: String;
var
  Folder: String;
  Error: Integer;
begin
  Result := '';
  Folder := RemoveBackslashUnlessRoot(ExpandConstant('{app}'));
  if OurInstallIn(Folder) then
    Exit;
  // (A redirection is no folder of its own, whatever it leads to: said as one, review 1.2; RightsReason says so too.)
  if ThroughRedirection(Folder, '') then begin
    Result := 'the folder ' + Folder + ' is a redirection (a junction or a link): qKVM Desktop is installed only into a folder that is itself there';
    Exit;
  end;
  // (Nothing there: a first install. A folder there that cannot be looked at is not known to hold nothing.)
  if GetFileAttributesW(Folder) = NoFileAttributes then begin
    Error := DLLGetLastError;
    if (Error = ErrorFileNotFound) or (Error = ErrorPathNotFound) then
      Exit;
  end;
  Result := LeftoversOnlyReason(Folder);
end;

// The SID that a TOKEN_USER Windows wrote into Info names (Needed: the bytes it wrote), in Windows' own words, of whatever
// authority: "S-1-5-21-..." for an account of this PC or of a domain, "S-1-12-1-..." for one of Microsoft Entra ID (work
// or school), and any other (outside review 2, 1.1: only NT's authority, 5, was taken, and every setup, update and
// uninstall of a person signed in with Microsoft Entra ID ended at once, the account taken for none). Reached where
// Windows' own pointer at Info's start says it is (TOKEN_USER.User.Sid: in this 32-bit Setup the first four bytes, the
// four of its attributes after them), never at a place of this setup's choosing, and read only where the whole of it
// lies within the bytes Windows wrote; turned into words by Windows' own converter, which takes only a SID. '' where it
// does not. (Info by var: its own characters, which that pointer points into.)
function SidInTokenUser(var Info: String; Needed: Cardinal): String;
var
  Base, Text: THandle;
  Bytes, BaseLow, BaseHigh, At, Count, Size: Integer;
  Sid: String;
begin
  Result := '';
  Bytes := Needed;
  if Bytes > 2 * Length(Info) then
    Exit;
  // (The pointer's bytes from where Info's characters begin, by its two halves: within the same 64 KiB, or the next.)
  Base := CharPrevW(Info, Info);
  BaseLow := Base and $FFFF;
  BaseHigh := Base shr 16;
  if Ord(Info[2]) = BaseHigh then
    At := Ord(Info[1]) - BaseLow
  else if Ord(Info[2]) = BaseHigh + 1 then
    At := Ord(Info[1]) + 65536 - BaseLow
  else
    Exit;
  // (After the pointer and the attributes, at a character of Info; its revision, how many parts follow its authority, and
  // that authority, within what Windows wrote, and then those parts too.)
  if (At < 8) or ((At mod 2) <> 0) or (At + 8 > Bytes) then
    Exit;
  Count := Ord(Info[At div 2 + 1]) div 256;
  if (Count < 1) or (Count > 15) or (At + 8 + 4 * Count > Bytes) then
    Exit;
  Sid := Copy(Info, At div 2 + 1, 4 + 2 * Count);
  Text := 0;
  if ConvertSidToStringSidW(Sid, Text) = 0 then
    Exit;
  Size := lstrlenW(Text);
  Result := StringOfChar(' ', Size);
  lstrcpynW(Result, Text, Size + 1);
  LocalFree(Text);
  if Copy(Result, 1, 4) <> 'S-1-' then
    Result := '';
end;

// The Windows account this setup runs as, as Windows names it: its token's user, read into a String (SidInTokenUser); ''
// where Windows does not say.
function OwnAccount: String;
var
  Token: THandle;
  Info: String;
  Needed: Cardinal;
  Read: Boolean;
begin
  Result := '';
  Token := 0;
  if OpenProcessToken(GetCurrentProcess, TokenQuery, Token) = 0 then
    Exit;
  Info := StringOfChar(#0, 64);
  Needed := 0;
  Read := GetTokenInformation(Token, TokenUserClass, Info, 128, Needed) <> 0;
  CloseHandle(Token);
  if Read then
    Result := SidInTokenUser(Info, Needed);
end;

// Who owns Path and who may do what with it, in Windows' own words ("O:<owner>D:<flags>(entry)(entry)..."); '' where
// Windows does not say.
function RightsOf(Path: String): String;
var
  Owner, Group, Dacl, Sacl, Descriptor, Text: THandle;
  Size: Cardinal;
  Chars: Integer;
begin
  Result := '';
  Owner := 0;
  Group := 0;
  Dacl := 0;
  Sacl := 0;
  Descriptor := 0;
  if GetNamedSecurityInfoW(Path, FileObject, OwnerAndList, Owner, Group, Dacl, Sacl, Descriptor) <> 0 then
    Exit;
  Text := 0;
  Size := 0;
  if ConvertSecurityDescriptorToStringSecurityDescriptorW(Descriptor, 1, OwnerAndList, Text, Size) <> 0 then begin
    Chars := lstrlenW(Text);
    Result := StringOfChar(' ', Chars);
    lstrcpynW(Result, Text, Chars + 1);
    LocalFree(Text);
  end;
  LocalFree(Descriptor);
end;

// Whether an account of Windows' words (a SID, or SDDL's two letters for one) is trusted with the program's folder: this
// setup's own account, Windows itself (SYSTEM, TrustedInstaller), the administrators (who may do anything on this PC in
// any case), and the creator and the owner of what is in it (CREATOR OWNER, OWNER RIGHTS), which stand for one of those
// where no other may make anything there. (Windows' words name this setup's own account by SDDL's two letters only where
// it is this PC's own Administrator, LA, whose SID ends in -500; any other, of this PC, of a domain or of Microsoft Entra
// ID, by its SID in full, as OwnAccount gives it: outside review 2, 1.1. An account of Entra ID whose SID ended in -500
// by chance would take LA, an administrator of this PC, for its own: one that is trusted in any case.)
function TrustedAccount(Account, Own: String): Boolean;
begin
  Result := (Account = Own) or (Account = 'SY') or (Account = 'BA') or (Account = 'CO') or (Account = 'OW') or
    (Account = 'S-1-5-18') or (Account = 'S-1-5-32-544') or (Account = 'S-1-3-0') or (Account = 'S-1-3-4') or
    (Account = TrustedInstallerSid) or ((Account = 'LA') and (Copy(Own, Length(Own) - 3, 4) = '-500'));
end;

// An account of SDDL's words as a person reads it: the groups an entry most often names, in words, and its own words.
function AccountWords(Account: String): String;
begin
  if Account = 'BU' then
    Result := 'the users of this PC (BU)'
  else if Account = 'AU' then
    Result := 'every signed-in account (AU)'
  else if Account = 'WD' then
    Result := 'everyone (WD)'
  else if Account = 'IU' then
    Result := 'whoever is signed in at the PC (IU)'
  else
    Result := 'the account ' + Account;
end;

// Whether SDDL's letters, read two by two, hold Pair (never one across two of them).
function HasPair(Words, Pair: String): Boolean;
var
  At: Integer;
begin
  Result := False;
  At := 1;
  while (At < Length(Words)) and not Result do begin
    Result := Copy(Words, At, 2) = Pair;
    At := At + 2;
  end;
end;

// An entry's rights in Windows' words ("0x1301bf", or SDDL's letters two by two, "FA", "SDGXGWGR"), as the two halves of
// its mask (Top: bits 16 to 31, Bottom: 0 to 15). False where they are not words this setup reads.
function RightsMask(Words: String; var Top, Bottom: Integer): Boolean;
var
  Hex: String;
  At, Found, Half: Integer;
begin
  Result := False;
  Top := 0;
  Bottom := 0;
  if Copy(Words, 1, 2) = '0x' then begin
    Hex := Copy(Words, 3, Length(Words));
    if (Hex = '') or (Length(Hex) > 8) then
      Exit;
    Hex := StringOfChar('0', 8 - Length(Hex)) + Hex;
    Top := StrToIntDef('$' + Copy(Hex, 1, 4), -1);
    Bottom := StrToIntDef('$' + Copy(Hex, 5, 4), -1);
    Result := (Top >= 0) and (Bottom >= 0);
    Exit;
  end;
  if (Words = '') or ((Length(Words) mod 2) <> 0) then
    Exit;
  At := 1;
  while At < Length(Words) do begin
    Found := Pos('|' + Copy(Words, At, 2), RightWords);
    if Found = 0 then
      Exit;
    Half := StrToIntDef('$' + Copy(RightWords, Found + 3, 4), -1);
    if Half < 0 then
      Exit;
    Top := Top or Half;
    Half := StrToIntDef('$' + Copy(RightWords, Found + 7, 4), -1);
    if Half < 0 then
      Exit;
    Bottom := Bottom or Half;
    At := At + 2;
  end;
  Result := True;
end;

// Why the folder Path, whose rights Windows words as Rights, lets another account than those trusted (TrustedAccount)
// change what it guards ('' where it does not): its owner (who may give itself any right), and every entry that allows
// such an account a right of the halves Not (any but those) or Danger (one of those) say; MadeWithin alone: only entries
// that a folder made in Path takes (CI), or that the files made in that folder take (OI), and every folder made within it
// for its own files (outside review 2, 1.2: an entry that only files take, Modify on "files only" or Write on "this
// folder and files", was passed, and each file installed in a new folder below it could be changed by that account;
// Modify on "this folder and files" was refused all the same, as it lets that account remove Path); Itself alone: only
// those of Path itself (not IO). (NP, an entry that goes no further than what is made right in Path, is not read: such
// an entry is judged as one without it, which may refuse a rare folder, and never passes one.) No list at all lets
// anyone do anything; an entry of a kind, or of words, this setup does not read is not taken for harmless.
function OthersMayChange(Path, Rights, Own: String; AnyBut: Boolean; MaskHigh, MaskLow: Integer; MadeWithin, Itself: Boolean): String;
var
  At, Ends, Field, Top, Bottom: Integer;
  Owner, List, Entry, Kind, Flags, Words, Account, Rest: String;
  Fields: TArrayOfString;
  Dangerous: Boolean;
begin
  Result := '';
  At := Pos('D:', Rights);
  if (Copy(Rights, 1, 2) <> 'O:') or (At < 4) then begin
    Result := 'who may change ' + Path + ' could not be read';
    Exit;
  end;
  Owner := Copy(Rights, 3, At - 3);
  if not TrustedAccount(Owner, Own) then begin
    Result := AccountWords(Owner) + ' owns ' + Path;
    Exit;
  end;
  List := Copy(Rights, At + 2, Length(Rights));
  if Pos('(', List) = 0 then begin
    if Pos('NO_ACCESS_CONTROL', List) > 0 then
      Result := 'anyone may change ' + Path + ', which has no list of who may';
    Exit;
  end;
  if Pos('NO_ACCESS_CONTROL', Copy(List, 1, Pos('(', List) - 1)) > 0 then begin
    Result := 'anyone may change ' + Path + ', which has no list of who may';
    Exit;
  end;
  SetArrayLength(Fields, 6);
  At := Pos('(', List);
  while (At > 0) and (Result = '') do begin
    Rest := Copy(List, At + 1, Length(List));
    Ends := Pos(')', Rest);
    if Ends = 0 then begin
      Result := 'who may change ' + Path + ' could not be read';
      Exit;
    end;
    Entry := Copy(Rest, 1, Ends - 1);
    // (Six fields, ";" between them, and no condition of its own, "(...)" within it.)
    Field := 0;
    Fields[0] := '';
    while (Entry <> '') and (Field < 6) do begin
      if Pos(';', Entry) > 0 then begin
        Fields[Field] := Copy(Entry, 1, Pos(';', Entry) - 1);
        Entry := Copy(Entry, Pos(';', Entry) + 1, Length(Entry));
      end else begin
        Fields[Field] := Entry;
        Entry := '';
      end;
      Field := Field + 1;
    end;
    if (Field <> 6) or (Entry <> '') or (Pos('(', Copy(Rest, 1, Ends - 1)) > 0) then begin
      Result := 'an entry of who may change ' + Path + ' is of a kind this setup does not read (' + Copy(Rest, 1, Ends - 1) + ')';
      Exit;
    end;
    Kind := Fields[0];
    Flags := Fields[1];
    Words := Fields[2];
    Account := Fields[5];
    if (Kind <> 'A') and (Kind <> 'D') then begin
      Result := 'an entry of who may change ' + Path + ' is of a kind this setup does not read (' + Kind + ')';
      Exit;
    end;
    if Kind = 'A' then begin
      if not RightsMask(Words, Top, Bottom) then begin
        Result := 'an entry of who may change ' + Path + ' gives rights this setup does not read (' + Words + ')';
        Exit;
      end;
      if AnyBut then
        Dangerous := ((Top and not MaskHigh) <> 0) or ((Bottom and not MaskLow) <> 0)
      else
        Dangerous := ((Top and MaskHigh) <> 0) or ((Bottom and MaskLow) <> 0);
      // (The flags two letters each: CI, what a folder made within takes; OI, what the files made within it take; IO,
      // what Path itself does not.)
      if MadeWithin and not HasPair(Flags, 'CI') and not HasPair(Flags, 'OI') then
        Dangerous := False;
      if Itself and HasPair(Flags, 'IO') then
        Dangerous := False;
      if Dangerous and not TrustedAccount(Account, Own) then
        Result := AccountWords(Account) + ' may change ' + Path + ' (' + Words + ')';
    end;
    At := Pos('(', Copy(List, At + 1, Length(List)));
    if At > 0 then
      At := At + Length(List) - Length(Rest);
  end;
end;

// Why the folder Folder is not one that this Windows account and Windows itself alone may change ('' where it is): a
// folder another account may write in is one where it may put a library the program then loads, or change one of the
// program's, which then runs as the person (review 1.2). Refused, never made safe: the rights of a folder the person or
// an administrator chose, and those of the folders above it, are theirs to set, and a per-user setup may not take away
// what it did not give; nothing is closed or changed, and the reason names the folder and the account. Looked at: the
// folder itself where it is there (no redirection, its owner, every entry that gives another account more than reading
// and running), else the nearest folder above it that is there, by the entries a folder made in it, or a file made in
// that, takes (outside review 2, 1.2); and each folder above, up to the drive's root (its owner, and every entry of its
// own that lets another account remove, rename or take it: whoever may move a folder away may put another in its
// place). A folder on a network share is refused (its share's own rights are not seen, and nothing on it is looked at),
// and so is one whose rights Windows does not say. Asked of the program's folder (RightsReason), and at the setup's start
// of the folder whose update that did not finish it would put right (InitializeSetup, outside review 2, 1.3).
function FolderRightsReason(Folder: String): String;
var
  Above, Own, Rights, Why, Up: String;
  Made: Boolean;
begin
  Result := '';
  if (Copy(Folder, 1, 2) = '\\') or (GetDriveTypeW(AddBackslash(ExtractFileDrive(Folder))) = DriveRemote) then begin
    Result := 'the folder ' + Folder + ' is on a network share, whose own rights this setup cannot see: qKVM Desktop is installed only on a drive of this PC';
    Exit;
  end;
  Own := OwnAccount;
  if Own = '' then begin
    Result := 'this setup could not tell which Windows account it runs as, to see who may change the folder ' + Folder;
    Exit;
  end;
  Made := GetFileAttributesW(Folder) <> NoFileAttributes;
  if Made and ThroughRedirection(Folder, '') then begin
    Result := 'the folder ' + Folder + ' is a redirection (a junction or a link): qKVM Desktop is installed only into a folder that is itself there';
    Exit;
  end;
  // (Where it is not there yet: the nearest folder above it that is, whose entries it takes when it is made.)
  Above := Folder;
  while not Made and (ExtractFileDir(Above) <> Above) do begin
    Above := ExtractFileDir(Above);
    Made := GetFileAttributesW(Above) <> NoFileAttributes;
  end;
  Rights := RightsOf(Above);
  if Rights = '' then
    Why := 'who may change ' + Above + ' could not be told'
  else
    Why := OthersMayChange(Above, Rights, Own, True, ReadsHigh, ReadsLow, Above <> Folder, False);
  // (And each folder above it, up to the drive's root.)
  Up := Above;
  if Above = Folder then
    Up := ExtractFileDir(Folder);
  while (Why = '') and (Up <> '') do begin
    Rights := RightsOf(Up);
    if Rights = '' then
      Why := 'who may change ' + Up + ' could not be told'
    else
      Why := OthersMayChange(Up, Rights, Own, False, AboveHigh, AboveLow, False, True);
    if ExtractFileDir(Up) = Up then
      Up := ''
    else
      Up := ExtractFileDir(Up);
  end;
  if Why <> '' then
    Result := 'the folder ' + Folder + ' is not one that this Windows account and Windows itself alone may change (' + Why + '): qKVM Desktop is installed only into such a folder';
end;

// Why the program's folder ({app}) is not one that this Windows account and Windows itself alone may change ('' where it
// is): FolderRightsReason, asked in Preparing to Install and at Install before anything is closed. Where an update that
// did not finish left something there (a marker of an update, or the program's own file set aside by a setup of 0.1.2,
// looked at by name alone, on a drive of this PC), the reason says that it stays as it is, as nothing is put right in
// such a folder, and what to do (outside review 2, 1.3: a start of qKVM there says to run the setup, which then refused
// naming the account alone).
function RightsReason: String;
var
  Folder: String;
begin
  Folder := RemoveBackslashUnlessRoot(ExpandConstant('{app}'));
  Result := FolderRightsReason(Folder);
  if (Result = '') or (Copy(Folder, 1, 2) = '\\') or (GetDriveTypeW(AddBackslash(ExtractFileDrive(Folder))) = DriveRemote) then
    Exit;
  if ThingAt(AddBackslash(Folder) + UnfinishedMarker) or ThingAt(AddBackslash(Folder) + FinishingMarker) or ThingAt(AddBackslash(Folder) + 'qkvm-desktop.exe' + AsideEnding) then
    Result := Result + '. What an update that did not finish left there stays as it is until then, and qKVM Desktop may not start meanwhile: run this setup again once no other account may change the folder, or uninstall qKVM Desktop in Settings, Apps';
end;

// One setup or uninstall of qKVM Desktop at a time for this Windows account, in whatever Windows session of it (review
// 1.3: the setup's own mutex is that session's alone, and two setups of one account, at the PC and by Remote Desktop,
// worked on one folder at once, the second taking the first's live record for one to put back). The account's gate: a
// mutex of the whole PC's names, named after the account's SID, made owned by this setup or uninstall as it begins and
// held until it ends (Windows lets go of it then, however it ends); one there already is waited for, up to
// AccountGateWaitMs (a start of qKVM Desktop holds it for an instant to take a stale marker away, review 1.4), and one
// left by a setup that ended without letting go of it is taken. Made with Windows' own list of this account's, so another
// account can neither take it nor wait for it; one of its name made first by another account, which this setup may not
// open, is not known to be free. '' where this setup holds it, else why not.
function TakeAccountGate: String;
var
  Own: String;
  Error, Answer: Cardinal;
begin
  Result := '';
  Own := OwnAccount;
  if Own = '' then begin
    Result := 'this setup could not tell which Windows account it runs as, to keep two setups of qKVM Desktop from working at once';
    Exit;
  end;
  AccountGate := CreateMutexW(0, 1, AccountGatePrefix + Own);
  Error := DLLGetLastError;
  if AccountGate = 0 then begin
    Result := Format('it could not be seen whether another setup of qKVM Desktop is at work for this Windows account (error %d: %s)', [Error, SysErrorMessage(Error)]);
    Exit;
  end;
  if Error <> ErrorAlreadyExists then
    Exit;
  Answer := WaitForSingleObject(AccountGate, AccountGateWaitMs);
  if (Answer = WaitObject0) or (Answer = WaitAbandoned) then
    Exit;
  CloseHandle(AccountGate);
  AccountGate := 0;
  Result := 'another setup or uninstall of qKVM Desktop is at work for this Windows account, here or in another Windows session of it';
end;

// Last in Preparing to Install, once nothing else stands in the way, so that no file is copied where it could not be
// done (review C7: a file that may be read and written but not renamed, as a folder that lets nobody delete in it has
// them, passes FilesInUse): what an update that did not finish left put right first (PutRight); then, where the folder
// holds any file of the package's (an upgrade or a reinstall; a first install sets nothing aside and writes no marker),
// the marker with its record, and each such file set aside with the record's ending, the program's own first. Before
// the marker, and so before anything is renamed, a refusal where one is not a plain file. What an earlier finishing
// update could not remove yet is carried into the new record, and its marker goes. Where one cannot be renamed, those
// that were are put back and the marker goes, and nothing is copied. '' where all is set aside (or nothing was there),
// else why not.
function SetPackageAside: String;
var
  Names, Carried, CarriedEndings, Kinds, Copies: TStringList;
  I, Count: Integer;
  Path, Suffix, Old, Text, Done: String;
begin
  Result := PutRight(ExpandConstant('{app}'));
  if Result <> '' then begin
    Result := 'what an update that did not finish left could not be put right: ' + Result;
    Exit;
  end;
  // (A marker PutRight could not take away: nothing new is recorded beside it.)
  if ThingAt(InApp(UnfinishedMarker)) then begin
    Result := 'the marker of an earlier update could not be removed (' + UnfinishedMarker + ')';
    Exit;
  end;
  // (Nothing set aside where no install of ours is proven in the folder: one that holds anything else is not this
  // setup's, and Preparing to Install has refused it before, review i020-2 M1. A folder that holds nothing goes on.)
  if not OurInstallIn(ExpandConstant('{app}')) then begin
    Result := ForeignFolderReason;
    Exit;
  end;
  // (Which version of ours was installed here, as the folder now is: the earlier version whole, put right above.)
  EarlierHere := InstalledHere;
  EarlierKindHere := '';
  if EarlierHere <> '' then begin
    EarlierKindHere := ImageKind(ProgramPath);
    if EarlierKindHere = '' then
      Log('qKVM Desktop ' + EarlierHere + ' was installed in this folder, and its program is of neither kind of Windows: nothing of what that version alone installed is removed after the copy.')
    else
      Log('qKVM Desktop ' + EarlierHere + ' (' + EarlierKindHere + ') was installed in this folder: what it installed and this version (' + ThisKind + ') has not goes after a whole copy.');
  end;
  Names := TStringList.Create;
  Carried := TStringList.Create;
  CarriedEndings := TStringList.Create;
  Kinds := TStringList.Create;
  Copies := TStringList.Create;
  try
    AddNames(PackageFiles, Names);
    Count := 0;
    for I := 0 to Names.Count - 1 do begin
      Path := InApp(Names[I]);
      if (Result = '') and ThingAt(Path) then begin
        if PlainFileAt(Path) then
          Count := Count + 1
        else
          Result := Format('a file of qKVM Desktop is not a plain file (%s)', [Names[I]]);
      end;
    end;
    if (Result = '') and (Count > 0) then begin
      Suffix := FreeEnding;
      if Suffix = '' then
        Result := 'every ending for setting aside the files of qKVM Desktop is taken by files of that name left beside them'
      else if not UpdateCheckReady then
        Result := 'the setup could not unpack its update check';
    end;
    if (Result = '') and (Count > 0) then begin
      // (What a finishing update could not remove yet, still there and named in its record: carried, with its ending.)
      if ThingAt(InApp(FinishingMarker)) then
        if ReadRecord(InApp(FinishingMarker), Old, Carried, CarriedEndings, Kinds, Copies) then
          for I := Carried.Count - 1 downto 0 do
            if not ThingAt(InApp(Carried[I]) + CarriedEndings[I]) then begin
              Carried.Delete(I);
              CarriedEndings.Delete(I);
            end;
      Text := 'qKVM Desktop {#AppVersion} is being installed here; the version before it is set aside beside this file.' + #13#10 + RecordKind + #13#10 + 'ending ' + Suffix + #13#10;
      for I := 0 to Names.Count - 1 do
        if ThingAt(InApp(Names[I])) then
          Text := Text + 'aside ' + Names[I] + #13#10;
      for I := 0 to Carried.Count - 1 do
        Text := Text + 'left ' + CarriedEndings[I] + ' ' + Carried[I] + #13#10;
      for I := 0 to Names.Count - 1 do
        Text := Text + 'copy ' + Names[I] + #13#10;
      Text := Text + RecordEnd + #13#10;
      Result := WriteRecord(Text, UnfinishedMarker);
      // (Held from now on, and kept, for the copy's end, review i020-2 L1; where it cannot be held as this setup's own,
      // nothing is renamed, and the marker, which names nothing set aside, is the next setup's to take away, review 1.4.)
      if Result = '' then begin
        RecordText := Text;
        if not HoldMarker(Text) then begin
          RecordText := '';
          Result := 'the marker of this update could not be held for as long as this setup works (' + UnfinishedMarker + ')';
        end;
      end;
      if (Result = '') and ThingAt(InApp(FinishingMarker)) then
        if not RemoveOwn(InApp(FinishingMarker)) then begin
          Result := 'the marker of an earlier update could not be removed (' + FinishingMarker + ')';
          RemoveOwn(InApp(UnfinishedMarker));
        end;
      if Result = '' then begin
        SetAside := True;
        Count := 0;
        for I := 0 to Names.Count - 1 do begin
          Path := InApp(Names[I]);
          if (Result = '') and ThingAt(Path) then begin
            if RenameFile(Path, Path + Suffix) then
              Count := Count + 1
            else
              Result := Format('a file of qKVM Desktop could not be set aside for the copy (%s)', [Names[I]]);
          end;
          // (The program's own file is first: the update check in its place at once, before any library goes.)
          if (Result = '') and (I = 0) then
            if not PlaceUpdateCheck(ProgramPath) then
              Result := 'the setup''s update check could not be put in the program''s place (qkvm-desktop.exe)';
        end;
      end;
    end;
  finally
    Names.Free;
    Carried.Free;
    CarriedEndings.Free;
    Kinds.Free;
    Copies.Free;
  end;
  if not SetAside then
    Exit;
  if Result = '' then begin
    Log(Format('Set the earlier version aside for the copy (%d files).', [Count]));
    Log('What was set aside ends in ' + Suffix + ', as its record says.');
    Exit;
  end;
  // (Nothing is copied: what was set aside is put back, and the marker goes, review C7. What cannot be yet is tried
  // again as the setup ends, DeinitializeSetup, and by the next setup. This setup's own set-aside undone is not what it
  // put right of an earlier update, L9.)
  Done := PutRightDone;
  if PutRight(ExpandConstant('{app}')) = '' then
    SetAside := False
  else
    Log('What was set aside for the copy could not all be put back yet.');
  PutRightDone := Done;
end;

// The marker of an unfinished update made that of a finishing one, in one rename (this setup's hold of it let go of
// first).
function MarkFinishing: Boolean;
begin
  LetGoOfMarker;
  Result := RenameFile(InApp(UnfinishedMarker), InApp(FinishingMarker));
end;

// After a whole copy on the disk, first: the marker made that of a finishing update (tried three times, half a second
// apart, each try said; where it is gone, this setup's record written again as that marker, L1), then what its record
// names removed, and that marker (PutRight). A cut from the rename on leaves
// a whole copy, which the next setup finishes; one before it leaves the earlier version set aside whole, which the next
// setup puts back. Where the marker cannot be renamed, nothing set aside is removed and the copy does not count: the
// earlier version is put back (DoNotCount). Whether the copy is marked finished: only then is anything of the earlier
// version that is not set aside removed (CurStepChanged), which putting it back would then lack.
function FinishCopy: Boolean;
var
  Tries: Integer;
  Reason: String;
begin
  Result := MarkFinishing;
  Tries := 1;
  while not Result and (Tries < 3) and ThingAt(InApp(UnfinishedMarker)) do begin
    Log('The copy is whole; its marker could not be made that of a finishing update yet, and is tried again.');
    Sleep(500);
    Result := MarkFinishing;
    Tries := Tries + 1;
  end;
  // (The marker gone all the same (taken away by something else while the copy ran): the copy is whole, and this setup
  // knows what it set aside, so its own record is written again, as that of a finishing update, and the copy is
  // finished; it is never taken for one to put back, review i020-2 L1.)
  if not Result and not ThingAt(InApp(UnfinishedMarker)) and (RecordText <> '') then begin
    Result := WriteRecord(RecordText, FinishingMarker) = '';
    if Result then
      Log('The copy is whole, and its marker was gone: its record is written again, as that of a finishing update.');
  end;
  if not Result then begin
    Log('The copy is whole, but its marker could not be made that of a finishing update: nothing set aside is removed.');
    Exit;
  end;
  Reason := PutRight(ExpandConstant('{app}'));
  if Reason <> '' then
    Log('The copy is whole; what was set aside for it could not all be removed (' + Reason + '): the next setup removes the rest first.');
end;

// When the copy did not complete (DeinitializeSetup, after Inno Setup's own rollback has removed what it made), or came
// to its end and does not count (DoNotCount): every file the record set aside put back, a file of its name that is there
// removed first; then the marker (PutRight). A file in the way that cannot be removed yet is tried again, a second
// apart, PutBackTries times in all; what is still set aside then stays so, with the marker: the program says that an
// update did not finish, and the next setup puts it back first. Whether the earlier version is whole again.
function PutPackageBack: Boolean;
var
  Tries: Integer;
  Reason: String;
begin
  Reason := PutRight(ExpandConstant('{app}'));
  Tries := 1;
  while (Reason <> '') and (Tries < PutBackTries) do begin
    Sleep(1000);
    Reason := PutRight(ExpandConstant('{app}'));
    Tries := Tries + 1;
  end;
  Result := (Reason = '') and not EarlierProgramGone;
  if Result then
    Log('The copy did not complete: the earlier version is whole again.')
  else if Reason = '' then
    Log('The copy did not complete: the earlier version is back, all but its program, which was gone since it was set aside.')
  else
    Log('The copy did not complete, and the earlier version could not all be put back (' + Reason + '): the next setup puts back the rest first.');
end;

// Whether the file at Path holds the bytes whose SHA-256 is Sum, as Windows reads it now. One that cannot be read (a
// scanner of the PC that holds it alone for a moment) is read again, half a second later, once; one that still cannot
// be is not shown to be whole (review i020-2, L7).
function SameBytes(Path, Sum: String): Boolean;
var
  Tries: Integer;
  Read: Boolean;
begin
  Result := False;
  Read := False;
  Tries := 0;
  while not Read and (Tries < 2) do begin
    if Tries > 0 then
      Sleep(500);
    Tries := Tries + 1;
    try
      Result := Lowercase(GetSHA256OfFile(Path)) = Lowercase(Sum);
      Read := True;
    except
      Log('A file of the copy could not be read, to tell that it is whole: ' + Path + ' (' + GetExceptionMessage + ')');
    end;
  end;
end;

// After a copy that came to its end, before anything else: every file of the package's there as the package has it, a
// plain file of the bytes its manifest gives, by their size and their SHA-256 (package.iss, PackageFileBytes and
// PackageFileSha256), and the contract's marker of its six bytes. A file the copy did not write (its box's Skip this
// file, or one a scanner took away since) is not, and nor is one of the right size with other bytes (a scanner that
// cleaned it in place, a fault between the write and now): the earlier file of that name is set aside, so a copy that
// went on without it is not whole (reviews i020-1 M1, i020-2 L7). '' where every one is, else the first that is not.
function NotCopied: String;
var
  Names, Bytes, Sums: TStringList;
  I: Integer;
  Found: TFindRec;
  Sized: Boolean;
begin
  Result := '';
  Names := TStringList.Create;
  Bytes := TStringList.Create;
  Sums := TStringList.Create;
  try
    AddNames(PackageFiles, Names);
    AddNames(PackageFileBytes, Bytes);
    AddNames(PackageFileSha256, Sums);
    Names.Add('qkvm-desktop.installed');
    Bytes.Add('6');
    Sums.Add(MarkerSha256);
    if (Names.Count <> Bytes.Count) or (Names.Count <> Sums.Count) then
      Result := 'the setup''s own list of its files';
    for I := 0 to Names.Count - 1 do
      if Result = '' then begin
        Result := Names[I];
        if FindFirst(InApp(Names[I]), Found) then begin
          Sized := ((Found.Attributes and (DirectoryAttribute or ReparsePointAttribute)) = 0) and (Found.SizeHigh = 0) and (IntToStr(Found.SizeLow) = Bytes[I]);
          FindClose(Found);
          if Sized and SameBytes(InApp(Names[I]), Sums[I]) then
            Result := '';
        end;
      end;
  finally
    Names.Free;
    Bytes.Free;
    Sums.Free;
  end;
end;

// What the copy wrote, on the disk before the copy is marked finished and the earlier version goes (review i020-1, L13):
// each file of the package's, and the contract's marker, flushed by Windows; one that cannot be is tried again every
// FlushStepMs, up to FlushTries times in all (a scanner of the PC that holds it alone for a moment lets go of it, and a
// start of the program, which maps its file for a moment and ends at once while this setup works, is over). '' where
// every one is on the disk, else the first that is not: the copy then does not count, and nothing set aside goes, for a
// power cut in the moments after could leave that file unwritten with its earlier one gone (review 1.6).
function FlushCopy: String;
var
  Names: TStringList;
  I, Tries: Integer;
  Flushed: Boolean;
begin
  Result := '';
  Names := TStringList.Create;
  try
    AddNames(PackageFiles, Names);
    Names.Add('qkvm-desktop.installed');
    for I := 0 to Names.Count - 1 do
      if Result = '' then begin
        Flushed := FlushedFile(InApp(Names[I]));
        Tries := 1;
        while not Flushed and (Tries < FlushTries) do begin
          Sleep(FlushStepMs);
          Flushed := FlushedFile(InApp(Names[I]));
          Tries := Tries + 1;
        end;
        if not Flushed then
          Result := Names[I];
      end;
  finally
    Names.Free;
  end;
  if Result <> '' then
    Log('A file of the copy could not be flushed to the disk before the copy is marked finished: ' + Result + '.')
  else
    Log('The copy is on the disk.');
end;

// A copy that came to its end and does not count (NotCounted says why: not whole, or not marked finished): nothing of
// the earlier version removed, and the earlier version, where it was set aside, put back now under the version
// Settings > Apps named before (Inno Setup has written this one's there by now); the result line, and a box where the
// setup is not silent, say so; the setup ends with 7 (GetCustomSetupExitCode). A failed copy always leaves the earlier
// version that was there (review i020-1, M1).
procedure DoNotCount;
var
  Why, Done: String;
begin
  Log('The copy does not count: ' + NotCounted + '.');
  Why := NotCounted;
  // (This copy's own undone is not what this setup put right of an earlier update, L9.)
  Done := PutRightDone;
  if SetAside then begin
    if PutPackageBack then begin
      PutRightDone := Done;
      EarlierBack := True;
      Why := Why + '; the earlier version is back';
      if InstalledVersion <> '' then
        RegWriteStringValue(HKEY_CURRENT_USER, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{A964EFA6-87C6-4A23-A9CC-B71F87441433}_is1', 'DisplayVersion', InstalledVersion);
      Log(UnchangedLine(Why));
    end else if EarlierProgramGone then
      Why := Why + '; the earlier version is back, all but its program, which was gone since it was set aside'
    else
      Why := Why + '; the earlier version could not all be put back yet';
  end;
  if not WizardSilent then
    MsgBox('qKVM Desktop {#AppVersion} was not installed: ' + Why + '. Run the setup again.', mbError, MB_OK);
end;

// The exit code of a setup that came to its end: 7 where its copy does not count (DoNotCount), as Preparing to Install's
// refusal ends; Inno Setup's own otherwise.
function GetCustomSetupExitCode: Integer;
begin
  Result := 0;
  if NotCounted <> '' then
    Result := 7;
end;

// At an uninstall, before the program's files are removed, so that the folder is left empty and goes with them: every
// file a marker's record names, what a copy that did not finish wrote, and each marker whose record names nothing that is
// still there (DropRecorded, an install of ours being there); the program's own file as a setup of 0.1.2 or before set it
// aside (its own rule, by its exact name), and a record's draft. Nothing else (a file of a set-aside ending that no record
// names is not the setup's), and nothing reached through a redirection. What could not be removed stays, named in a
// record a later setup reads, in a folder that setup takes for one of its own (review 1.8).
procedure DropSetAside;
begin
  DropRecorded(ExpandConstant('{app}'), True);
  if PlainFileAt(InApp('qkvm-desktop.exe' + AsideEnding)) then
    if RemoveOwn(InApp('qkvm-desktop.exe' + AsideEnding)) then
      Log('Removed the earlier program, set aside by a setup before.');
  if PlainFileAt(InApp(RecordDraft)) then
    RemoveOwn(InApp(RecordDraft));
  if PlainFileAt(InApp(CheckDraft)) then
    RemoveOwn(InApp(CheckDraft));
end;

// After a whole copy, once it is marked finished (CurStepChanged): what the version of ours installed in this very folder
// before (InstalledHere) installed and this one has not (a licence, a library that went), each by its exact name from
// that version's own list (earlier.iss, EarlierFilesOf), a plain file only and never one reached through a redirection;
// then each of their folders that this leaves empty, the deepest first (RemoveDir takes only an empty folder, and the
// list never names the program's folder itself). Nothing where no version of ours is proven there. Never a pattern and
// never a walk of the folder: a person's file beside them stays, and so does a folder that holds one.
procedure RemoveEarlierFiles(Version, Kind: String);
var
  Names: TStringList;
  I: Integer;
  Path: String;
begin
  if Version = '' then
    Exit;
  Names := TStringList.Create;
  try
    AddNames(EarlierFilesOf(Version, Kind), Names);
    for I := 0 to Names.Count - 1 do begin
      Path := InApp(Names[I]);
      if PlainFileAt(Path) and not ThroughRedirection(ExpandConstant('{app}'), RemoveBackslash(ExtractFilePath(Names[I]))) then begin
        if DeleteFile(Path) then
          Log('Removed what qKVM Desktop ' + Version + ' (' + Kind + ') installed and this one has not: ' + Names[I])
        else
          Log('Could not remove what qKVM Desktop ' + Version + ' (' + Kind + ') installed and this one has not: ' + Names[I]);
      end;
    end;
    Names.Clear;
    AddNames(EarlierFoldersOf(Version, Kind), Names);
    for I := 0 to Names.Count - 1 do begin
      Path := InApp(Names[I]);
      if ThingAt(Path) and not ThroughRedirection(ExpandConstant('{app}'), Names[I]) then
        if RemoveDir(Path) then
          Log('Removed the emptied folder ' + Names[I]);
    end;
  finally
    Names.Free;
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
// However early Setup ends, a cancel on its first page or a version refused included: the program's folder is looked at
// only where this setup has come to its copy (SetAside, in Preparing to Install) or refused there (StartAtEnd), where
// {app} is known. What was set aside for a copy that did not complete is put back first; in update mode, where that
// leaves the earlier version whole, the program, which ended itself for the update, is started again (review i020-1,
// L5); then what this setup closed is started again.
procedure DeinitializeSetup;
begin
  if SetAside and not PostInstalled then
    if PutPackageBack and UpdateMode then
      StartAgain;
  if StartAtEnd then
    StartAgain;
end;

// Whether a copy of qKVM Desktop 0.2.0 or later runs for this Windows account, in any of its sessions: it holds the
// lock of the file beside its settings for as long as it runs. Where the lock is free, the uninstall takes it and keeps
// it until it ends, so that no copy starts meanwhile (a start that finds it held, and the uninstall's gate there, ends
// at once). The mutex alone does not see a copy of this account that another account's copy in this Windows session
// keeps from the shared name (Run as different user), nor one in another session: this does. Where there is no such
// file, no copy of 0.2.0 or later has run for this account yet: the uninstall makes it, empty, as a first start does
// (and its folder where that is not there either), and holds its lock all the same, so that a first start in another
// Windows session finds it held while the files are removed (review 1.3); both are removed as the uninstall ends
// (DeinitializeUninstall), by this very name, and only what it made. Where even that cannot be made, the mutex says the
// rest, as before. '' where none runs; else why the uninstall does not begin. Nothing of the person's data is read or
// written.
function ProfileInUse: String;
var
  Profile: THandle;
  Opened, Locked: Cardinal;
  Folder: String;
begin
  Result := '';
  Profile := CreateFileW(ExpandConstant(ProfileLockFile), GenericRead, FileShareReadWrite, 0, OpenExisting, 0, 0);
  Opened := DLLGetLastError;
  if (Opened = ErrorFileNotFound) or (Opened = ErrorPathNotFound) then begin
    Folder := ExtractFileDir(ExpandConstant(ProfileLockFile));
    if not DirExists(Folder) then
      if CreateDir(Folder) then
        ProfileFolderMade := Folder;
    Profile := CreateFileW(ExpandConstant(ProfileLockFile), GenericRead, FileShareReadWrite, 0, OpenAlways, 0, 0);
    Opened := DLLGetLastError;
    // (Made here, not opened: no ERROR_ALREADY_EXISTS. One a first start made meanwhile is that start's.)
    if Opened = 0 then
      ProfileFileMade := ExpandConstant(ProfileLockFile)
    else if Opened = ErrorAlreadyExists then
      Opened := 0;
    if (Opened <> 0) and (ProfileFolderMade <> '') then
      if RemoveDir(ProfileFolderMade) then
        ProfileFolderMade := '';
    if Opened <> 0 then
      Log('The lock of the profile could not be made, no copy of qKVM Desktop having run for this Windows account (error ' + IntToStr(Opened) + ').');
  end;
  if LockFile(Profile, 0, 0, 1, 0) <> 0 then begin
    UninstallProfile := Profile;
    Exit;
  end;
  Locked := DLLGetLastError;
  if Locked = ErrorLockViolation then
    Result := 'qKVM Desktop is running for this Windows account (here, or in another Windows session of it)'
  // (No handle: the file could not be opened. Not there: no copy of 0.2.0 or later has run here.)
  else if (Locked <> ErrorInvalidHandle) or ((Opened <> ErrorFileNotFound) and (Opened <> ErrorPathNotFound)) then begin
    // (Windows' word of the step that failed: the lock's, else the opening's.)
    if Locked = ErrorInvalidHandle then
      Locked := Opened;
    Result := 'it could not be seen whether qKVM Desktop runs for this Windows account (error ' + IntToStr(Locked) + ' at ' + ExpandConstant(ProfileLockFile) + ')';
  end;
end;

// The uninstaller refuses while qKVM Desktop runs: the program's own mutex, looked at here as it starts, whatever Inno
// Setup kept of AppMutex. (It keeps nothing of it for an install made outside update mode: the first VM suite's step
// 25 uninstalled a fresh install while the program ran, with no look at a mutex in its log; review M2, V2.) Silent, it
// ends as an uninstall that did not begin; by hand, it says why first. This is where the uninstall is decided: every
// refusal is here (review F4).
function InitializeUninstall: Boolean;
var
  Running: String;
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
  // (And this account's gate, as a setup's: no setup or other uninstall of this account at work, in any Windows session,
  // while this one is; review 1.3.)
  Running := TakeAccountGate;
  if Running <> '' then begin
    Log(Running + ': it is not uninstalled. Let that finish, then uninstall it again.');
    if not UninstallSilent then
      MsgBox('qKVM Desktop is not uninstalled: ' + Running + '. Let that finish, then uninstall qKVM Desktop again.', mbError, MB_OK);
    Result := False;
    Exit;
  end;
  if CheckForMutexes(ProgramMutexName) then begin
    Log('qKVM Desktop is running: it is not uninstalled. Quit it first, then uninstall it again.');
    if not UninstallSilent then
      MsgBox('qKVM Desktop is running. Quit it first: right-click the qKVM icon in the notification area, then choose Quit. Then uninstall it again.', mbError, MB_OK);
    Result := False;
    Exit;
  end;
  // (And by the lock of its profile, after the gate: a start that took the lock before is seen here, one after it
  // finds the gate. 0.2.0, the account-qualified look of SECURITY.md.)
  Running := ProfileInUse;
  if Running <> '' then begin
    Log(Running + ': it is not uninstalled. Quit it first, then uninstall it again.');
    if not UninstallSilent then
      MsgBox('qKVM Desktop is running for this Windows account, or it could not be seen whether it is (' + Running + '). Quit it first, in every Windows session of this account: right-click the qKVM icon in the notification area, then choose Quit. Then uninstall it again.', mbError, MB_OK);
    Result := False;
  end;
end;

// At an uninstall, before the program's files are removed, so that the folder is left empty and goes with them: what
// a setup ended halfway left, and what an update ended halfway set aside, with its marker (DropSetAside).
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then begin
    // (Looked at again right before anything is removed, as a best effort: a copy that started all the same, one before
    // 0.1.2 that does not honour the gate among them, is said in the log, and the exception ends this step; Inno
    // Setup's help says that it ends the uninstall there, which is for the VM to show, and nothing here rests on it.
    // A start of 0.2.0 or later in the instant between this look and the removal of the files ends by itself: it
    // looks at the gate again once its mutex is there, and finds the profile's lock held by this uninstall
    // (SECURITY.md). The refusal that decides is InitializeUninstall's; reviews R4, F4.)
    if CheckForMutexes(ProgramMutexName) then begin
      Log('qKVM Desktop started while it was being uninstalled: it is not uninstalled. Quit it first, then uninstall it again.');
      RaiseException('qKVM Desktop is running. Quit it first, then uninstall it again.');
    end;
    RemoveSetupLeftovers;
    DropSetAside;
  end;
end;

// However the uninstall ends: the lock of the profile it made, no copy having run for this account (ProfileInUse), let go
// of, then that file removed by its very name, and its folder where the uninstall made that too and it holds nothing
// (RemoveDir takes only an empty one); one a start opened meanwhile stays, as that start's. Nothing of the person's is
// touched (review 1.3).
procedure DeinitializeUninstall;
begin
  if ProfileFileMade = '' then
    Exit;
  if UninstallProfile <> 0 then
    CloseHandle(UninstallProfile);
  UninstallProfile := 0;
  if DeleteFile(ProfileFileMade) then
    Log('Removed the lock of the profile, which this uninstall had made: ' + ProfileFileMade);
  if ProfileFolderMade <> '' then
    RemoveDir(ProfileFolderMade);
end;

// The folder this setup installs into, as far as InitializeSetup can tell it ({app} is not known there yet): an update's
// /DIR where it names a whole path, else the installed folder as Settings > Apps keeps it, else the folder of a first
// install (DefaultDirName).
function FolderToPutRight: String;
begin
  Result := ExpandConstant('{param:DIR|}');
  if (Copy(Result, 2, 2) <> ':\') and (Copy(Result, 1, 2) <> '\\') then
    Result := InstalledFolder;
  if Result = '' then
    Result := ExpandConstant('{userpf}\qKVM Desktop');
  Result := RemoveBackslashUnlessRoot(Result);
end;

// First, in every mode: what an update that did not finish left, put right (PutRight), before anything else is looked
// at, the refusal of an older version below among it; only in a folder that this Windows account and Windows itself
// alone may change, which is decided before it (FolderRightsReason; in any other nothing is put right, nor anything of it
// looked at, and Preparing to Install refuses the folder, outside review 2, 1.3). Where it cannot be, nothing else is
// changed and the setup does not begin, saying why. Then: never an older version over a newer one: the version this
// user's Settings > Apps entry says is installed, against this setup's. An interactive setup says why in plain words; a
// silent one ends without a word, with Inno Setup's exit code of a setup that did not begin, having changed nothing. The
// version installed is kept: the first page names it (InitializeWizard).
function InitializeSetup: Boolean;
var
  Installed, Reason, Rest: String;
  Have, Offered: Int64;
begin
  Result := True;
  // This setup at work, as the program can tell for as long as it runs: its own event, named after its mutex and its
  // process's number, kept until it ends. A handle to the mutex kept elsewhere after it has ended keeps the mutex there,
  // but no process of that number runs any more, and the program starts (review R2). Where it cannot be made, nothing
  // is closed and nothing is changed (GateReason, review F5).
  SetupRuns := CreateEventW(0, 1, 0, SetupRunsName + IntToStr(GetCurrentProcessId));
  if RegQueryStringValue(HKEY_CURRENT_USER, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{A964EFA6-87C6-4A23-A9CC-B71F87441433}_is1', 'InstallLocation', InstalledFolder) then
    InstalledFolder := RemoveBackslashUnlessRoot(InstalledFolder);
  // (This account's gate first, before anything is put right or changed: one setup or uninstall at a time for the
  // account, in whatever Windows session, review 1.3. Where it cannot be had, the setup does not begin, and says why where
  // the program shows it.)
  Reason := TakeAccountGate;
  if Reason <> '' then begin
    Log('qKVM Desktop Setup does not begin: ' + Reason + '. Nothing was changed.');
    Log(UnchangedLine(Reason));
    if not WizardSilent then
      MsgBox('qKVM Desktop Setup does not begin: ' + Reason + '. Let that finish, then run this setup again. Nothing was changed.', mbError, MB_OK);
    Result := False;
    Exit;
  end;
  // (The folder's rights first: a record of what an update set aside is followed, and the program's own file of 0.1.2 put
  // right, only in a folder that no other account may change (outside review 2, 1.3: in a folder of ours that others may
  // change, a record was followed, removing and renaming files with the person's rights, and only then did Preparing to
  // Install refuse the folder). Where another account may change it, nothing in it is put right, nor looked at for it, and
  // Preparing to Install refuses it, naming the account, before anything is closed or changed.)
  Reason := FolderRightsReason(FolderToPutRight);
  if Reason <> '' then begin
    Log('Nothing that an update left is put right, nor looked for: ' + Reason + '. Preparing to Install refuses such a folder.');
    Reason := '';
  end else
    Reason := PutRight(FolderToPutRight);
  if Reason <> '' then begin
    // (What could be put right, is, and is said: never that nothing was changed after it, L9.)
    if PutRightDone <> '' then
      Rest := '. First, this setup ' + PutRightDone + '; nothing else was changed.'
    else
      Rest := '. Nothing else was changed.';
    Log('An update of qKVM Desktop that did not finish left what this setup cannot put right: ' + Reason + Rest);
    // (And the result line the program reads in the log it named, as Preparing to Install's refusal writes it: an update
    // that does not begin says why where the program shows it, never in the log alone.)
    Log(UnchangedLine('an update that did not finish left what this setup cannot put right: ' + Reason));
    if not WizardSilent then
      MsgBox('An update of qKVM Desktop that did not finish left what this setup cannot put right: ' + Reason + Rest, mbError, MB_OK);
    Result := False;
    Exit;
  end;
  if not RegQueryStringValue(HKEY_CURRENT_USER, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{A964EFA6-87C6-4A23-A9CC-B71F87441433}_is1', 'DisplayVersion', Installed) then
    Exit;
  InstalledVersion := Installed;
  if not StrToVersion(Installed + '.0', Have) or not StrToVersion('{#AppVersion}.0', Offered) then
    Exit;
  if ComparePackedVersion(Have, Offered) > 0 then begin
    // (Where this setup put the earlier version back first, it says so, and that it changed nothing else, L4.)
    if PutRightDone = '' then
      Reason := 'Nothing was changed.'
    else
      Reason := 'First, this setup ' + PutRightDone + '; nothing else was changed.';
    Log(Format('qKVM Desktop %s is installed, newer than this setup''s %s. %s', [Installed, '{#AppVersion}', Reason]));
    if not WizardSilent then
      MsgBox(Format('qKVM Desktop %s is installed, which is newer than this setup (%s). %s', [Installed, '{#AppVersion}', Reason]), mbInformation, MB_OK);
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
  StartsPage := CreateOutputProgressPage('Waiting for qKVM Desktop', 'It was started while Setup installed it, and ends by itself; Setup waits a minute at the most.');
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
    // (A folder this setup may not look through, as Inno Setup's RedirectionGuard keeps it from following a junction
    // the person made: its drive's root is looked at instead, and the redirection is refused below in its own words,
    // ForeignFolderReason and RightsReason; step 44 of test_setup_vm.)
    if not GetSpaceOnDisk64(AddBackslash(ExtractFileDrive(ExpandConstant('{app}'))), Free, Total) then
      Result := 'the free room on the drive of qKVM Desktop could not be told';
  if (Result = '') and (Free < Needed) then
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
  // (Nor for an install into a folder that holds what is not an install of ours: Preparing to Install refuses it,
  // review i020-2 M1.)
  if ForeignFolderReason <> '' then
    Exit;
  // (Nor for one that another account may change: Preparing to Install refuses it, review 1.2.)
  if RightsReason <> '' then
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

// Whether what is in the program's folder may be started as the setup ends: a copy that counts, or one that does not
// and left the earlier version whole again (DoNotCount put it back). Never a copy that does not count and left nothing
// whole: a first install that went on without a file, or one not known to be on the disk (review 1.7).
function CopyStarts: Boolean;
begin
  Result := (NotCounted = '') or EarlierBack;
end;

// The last page's "Start qKVM Desktop": ticked after a first install, and where this setup closed the program; not
// ticked after an upgrade of a copy that was not running. Started again with nothing to ask: at the end of an update
// (the contract), and of a silent install that closed it.
// None of them where this setup may start nothing (MayStartProgram), nor where what it copied may not be (CopyStarts).
function StartTicked: Boolean;
begin
  Result := MayStartProgram and CopyStarts and not UpdateMode and ((InstalledVersion = '') or ClosedIt);
end;

function StartUnticked: Boolean;
begin
  Result := MayStartProgram and CopyStarts and not UpdateMode and not StartTicked;
end;

function StartsAgain: Boolean;
begin
  Result := MayStartProgram and CopyStarts and (UpdateMode or (WizardSilent and ClosedIt));
end;

// How many processes run now from the program's folder (their file in it, or in a folder within it), by Windows' list of
// every process and the file each runs: the starts of the program made while this setup copied (WaitForStartsToEnd).
// A process this account may not look at (another account's, or one above it) is none of them: no start of the
// person's is; nor is one that has ended, whose number a handle still keeps.
function StartsRunning: Integer;
var
  Ids, Name, App: String;
  Needed, Size, Id: Cardinal;
  I: Integer;
  Process: THandle;
begin
  Result := 0;
  Ids := StringOfChar(#0, 2 * ProcessListMost);
  Needed := 0;
  if K32EnumProcesses(Ids, 4 * ProcessListMost, Needed) = 0 then begin
    Log('The processes that run could not be listed: ' + SysErrorMessage(DLLGetLastError));
    Exit;
  end;
  App := RemoveBackslashUnlessRoot(ExpandConstant('{app}'));
  for I := 0 to Integer(Needed div 4) - 1 do begin
    Id := Cardinal(Ord(Ids[2 * I + 1])) + Cardinal(Ord(Ids[2 * I + 2])) * 65536;
    Process := 0;
    if Id <> 0 then
      Process := OpenProcess(ProcessQueryLimitedInformation or ProcessSynchronize, 0, Id);
    if Process <> 0 then begin
      Size := 1024;
      Name := StringOfChar(' ', Size);
      if (QueryFullProcessImageNameW(Process, 0, Name, Size) <> 0) and (WaitForSingleObject(Process, 0) <> WaitObject0) then begin
        Name := Copy(Name, 1, Size);
        if (Length(Name) > Length(App) + 1) and (Copy(Name, Length(App) + 1, 1) = '\') and PathSame(Copy(Name, 1, Length(App)), App) then
          Result := Result + 1;
      end;
      CloseHandle(Process);
    end;
  end;
end;

// Before the copy is judged (whole, on the disk and marked finished, or put back where it does not count): every start
// of the program made while this setup copied, ended. Such a start ends by itself, in its main(), before any window and
// without its mutex, as it finds this setup at work (ui/main.cpp); but until then it holds the program's file and the
// libraries it maps, against the flush (FlushCopy, whose tries last about 5 s) and the put-back, and the first start of
// a copy just written can take many seconds: Windows' scanner (Microsoft Defender) scans each new library as the start
// maps it (on a test VM, 2026-11-21: 0.4 to 0.7 s for each of Qt's and FFmpeg's, 2 to 8 s for a whole copy, more while
// it scans something else; about 60 ms once scanned). A start of the program made during an update that ends within
// the minute no longer costs the update so. In an interactive install the person sees the wait, on a page whose
// progress is stepped at each look (review armp-1 L9); a silent install, an update among them, shows nothing more. The
// page takes no message while this sleeps, so Windows may show the setup as not responding until the wait ends, within
// the minute (a known limit, CHANGELOG; a VM run, 2026-11-22).
// Looked at a quarter of a second apart, until StartsEndMs after Since (the post-install step's start: a minute in all,
// however often it is asked): how many still run then. None is ever ended by this setup: where one still runs and the
// earlier version was set aside, the copy does not count, as any copy that does not, and the earlier version is put
// back (CurStepChanged).
function WaitForStartsToEnd(Since: Cardinal): Integer;
var
  Began: Cardinal;
  Most: Integer;
  Shown: Boolean;
begin
  Result := StartsRunning;
  if Result = 0 then
    Exit;
  Began := GetTickCount;
  Most := Result;
  Shown := not WizardSilent;
  if Shown then begin
    StartsPage.SetText('Waiting for qKVM Desktop, started during the install, to end...', '');
    StartsPage.SetProgress(Integer(TicksSince(Since)), StartsEndMs);
    StartsPage.Show;
  end;
  try
    while (Result > 0) and (TicksSince(Since) < StartsEndMs) do begin
      Sleep(UpdateStepMs);
      Result := StartsRunning;
      if Result > Most then
        Most := Result;
      if Shown then
        StartsPage.SetProgress(Integer(TicksSince(Since)), StartsEndMs);
    end;
  finally
    if Shown then
      StartsPage.Hide;
  end;
  if Result = 0 then
    Log(Format('Waited %d ms for the starts of qKVM Desktop made during the copy to end (%d at once at the most): each ends by itself while this setup works.', [Integer(TicksSince(Began)), Most]))
  else
    Log(Format('%d starts of qKVM Desktop made during the copy still run from its folder after %d ms: this setup ends none of them.', [Result, Integer(TicksSince(Since))]));
end;

// An update's own start, and a silent install's that closed the program (StartsAgain), at the very end of the
// post-install step: after the copy is flushed and marked finished and what it set aside is gone, or after the earlier
// version is back where the copy does not count; never from [Run], whose entries without postinstall Inno Setup runs
// before this step, while the program it starts would hold its own image against the flush and a put-back. As the
// entry did, it starts the program with --after-update while this setup still holds its mutex: the program waits for
// the mutex to be gone, as the setup ends, then starts itself anew through the person's shell (the contract).
procedure StartTheCopy;
var
  Started: Integer;
begin
  if ExecAsOriginalUser(ProgramPath, '--after-update', '', SW_SHOWNORMAL, ewNoWait, Started) then
    Log('Started qKVM Desktop from its folder with --after-update, its copy judged: it starts itself anew once this setup has ended.')
  else
    Log('Could not start qKVM Desktop from its folder: ' + SysErrorMessage(Started));
end;

// (After StartsAgain, which it asks once the copy is judged: Pascal Script knows a routine only below its own text.)
procedure CurStepChanged(CurStep: TSetupStep);
var
  Finished: Boolean;
  StillRunning: Integer;
  StartsSince: Cardinal;
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
    // (The starts made during the copy ended first: they hold what is judged, WaitForStartsToEnd.)
    StartsSince := GetTickCount;
    StillRunning := WaitForStartsToEnd(StartsSince);
    // (The copy whole first, every file there as the package has it, else it does not count and the earlier version is
    // put back now (M1); then on the disk (L13); then what was set aside goes, before anything else is removed:
    // FinishCopy. Where the copy could not be marked finished, it does not count either. Nothing of the earlier
    // version is removed where the copy does not count.)
    Finished := True;
    NotCounted := NotCopied;
    if NotCounted <> '' then
      NotCounted := 'the copy was not whole (' + NotCounted + ')'
    else if SetAside and (StillRunning > 0) then
      // (One that still runs after a minute holds what would be flushed and put back: the copy does not count, and says
      // why, and the earlier version is put back; this setup never ends it.)
      NotCounted := 'qKVM Desktop, started during the copy, still ran from its folder after a minute'
    else if SetAside then begin
      // (A file not known to be on the disk is not whole: nothing set aside goes, review 1.6.)
      NotCounted := FlushCopy;
      // (A start made as the copy was flushed, after the look above, that holds a file of it: waited for too, within the
      // same minute, and the flush tried again, as the VM suite found it, 2026-11-21.)
      while (NotCounted <> '') and (StillRunning = 0) and (TicksSince(StartsSince) < StartsEndMs) and (StartsRunning > 0) do begin
        StillRunning := WaitForStartsToEnd(StartsSince);
        if StillRunning = 0 then
          NotCounted := FlushCopy;
      end;
      if StillRunning > 0 then
        NotCounted := 'qKVM Desktop, started during the copy, still ran from its folder after a minute'
      else if NotCounted <> '' then
        NotCounted := 'the copy could not all be written to the disk (' + NotCounted + ')'
      else begin
        Finished := FinishCopy;
        if not Finished then
          NotCounted := 'the copy could not be marked finished (' + UnfinishedMarker + ')';
      end;
    end;
    if NotCounted <> '' then begin
      Finished := False;
      DoNotCount;
    end;
    if Finished then begin
      // (The kind it installed, beside the version Inno Setup notes, in its entry of Settings > Apps.)
      if not RegWriteStringValue(HKEY_CURRENT_USER, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{A964EFA6-87C6-4A23-A9CC-B71F87441433}_is1', 'qKVM Desktop Architecture', ThisKind) then
        Log('The kind of Windows this install is for could not be noted in its entry of Settings > Apps.');
      RemoveEarlierFiles(EarlierHere, EarlierKindHere);
    end;
    RemoveSetupLeftovers;
    if StartsAgain then
      StartTheCopy;
  end;
end;


// What the record of a finishing update names that is still there, a plain file: what an update set aside and could
// not remove yet (FinishRecorded said so, as InitializeSetup put right first), each by its path in the program's
// folder, into Paths. This setup replaces none of them: it carries them into its own record (SetPackageAside), and a
// later setup removes them; held by another program, they never stop a setup (34ede5fa: one that still cannot be
// removed stays named in its record for a later setup and stops none). A record that does not read names none here:
// PutRight has refused it before.
procedure AddLeftAside(Paths: TStringList);
var
  Names, Endings, Kinds, Copies: TStringList;
  Suffix: String;
  I: Integer;
begin
  if not ThingAt(InApp(FinishingMarker)) then
    Exit;
  Names := TStringList.Create;
  Endings := TStringList.Create;
  Kinds := TStringList.Create;
  Copies := TStringList.Create;
  try
    if ReadRecord(InApp(FinishingMarker), Suffix, Names, Endings, Kinds, Copies) then
      for I := 0 to Names.Count - 1 do
        if PlainFileAt(InApp(Names[I]) + Endings[I]) then
          Paths.Add(InApp(Names[I]) + Endings[I]);
  finally
    Names.Free;
    Endings.Free;
    Kinds.Free;
    Copies.Free;
  end;
end;

// Before anything is changed, in every mode (when this says why not, Setup ends with its exit code 7): this setup's
// gate and its event that says it is at work (GateReason, first, review F5), room on the program's drive for every file
// the setup puts in (RoomReason: nothing is closed for an install that could not be made), the program's folder one of
// its own or an empty one (ForeignFolderReason, review i020-2 M1: nothing of another's is touched) and one that this
// Windows account and Windows itself alone may change (RightsReason, review 1.2), a silent install's
// running copy closed (CloseProgramSilently), and every file of the program's folder free
// to be replaced, looked at again for up to 2 s (a start of the program that ends at once, as one does while Setup is at
// work, or one just closed, holds its files a moment), but what a finishing update could not remove yet, which is left
// to its record, in use or not (AddLeftAside); and, last, the earlier version set aside for the copy
// (SetPackageAside): where it cannot be, nothing is copied (review C7), and a copy that then fails halfway is never a
// mix of two versions. Where this setup closed qKVM Desktop and then changes nothing, it is started again as the setup
// ends, and the result line says so (review L1).
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Reason, Path: String;
  Busy, Unlisted, LeftAside: TStringList;
  Began: Cardinal;
  I: Integer;
begin
  Result := '';
  Reason := GateReason;
  if Reason = '' then
    Reason := RoomReason;
  if Reason = '' then
    Reason := ForeignFolderReason;
  if Reason = '' then
    Reason := RightsReason;
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
    LeftAside := TStringList.Create;
    try
      FilesInUse(ExpandConstant('{app}'), Busy, Unlisted);
      AddLeftAside(LeftAside);
      for I := Busy.Count - 1 downto 0 do
        if (Unlisted.IndexOf(Busy[I]) < 0) and (LeftAside.IndexOf(Busy[I]) >= 0) then begin
          Log('In use, and left to the record of the update that set it aside: ' + Busy[I]);
          Busy.Delete(I);
        end;
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
      LeftAside.Free;
      Unlisted.Free;
      Busy.Free;
    end;
  end;
  if Reason = '' then
    Reason := SetPackageAside;
  if Reason <> '' then begin
    if ClosedIt then begin
      StartAtEnd := True;
      Reason := Reason + '; qKVM Desktop, which this setup had closed, is started again';
    end;
    // The one result line the program reads in the log it named (it cannot see Setup's exit code 7), and the words of
    // the page: never that nothing was changed where this setup put right first what an earlier update left (L9).
    Log(UnchangedLine(Reason));
    if PutRightDone = '' then
      Result := 'Nothing was changed: ' + Reason + '.'
    else
      Result := 'First, this setup ' + PutRightDone + ', and changed nothing else: ' + Reason + '.';
  end;
end;
