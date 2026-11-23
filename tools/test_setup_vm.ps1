#Requires -Version 5.1
<#
.SYNOPSIS
Tries the setup for real: ONLY inside a throwaway Windows 10 (22H2) or 11 virtual machine, or on a Windows on Arm PC
bought for such tests (-PhysicalArmPc), as a standard user.

.DESCRIPTION
    powershell -ExecutionPolicy Bypass -File test_setup_vm.ps1 -Setup <new setup.exe> -OlderSetup <older setup.exe>
        -Out <results folder> -Disposable [-ListedSetup <the new setup, made with the test list>]
        [-OtherKindZip <the ZIP of an earlier published version of the other kind>] [-PhysicalArmPc]

NEVER on a PC anyone works on: it installs, upgrades, kills, updates and uninstalls qKVM Desktop for the user who
runs it, writes their registry and Start menu, and starts and ends the program. It refuses to begin unless it is
told -Disposable, finds the file qkvm-disposable-vm.txt in the user's own folder holding this machine's name, both as
Windows says them and never as the environment does (made
by whoever set the machine up for this), runs as a standard user (no administrator, not elevated), and finds no
qKVM Desktop installed yet. Windows PowerShell 5.1 is enough; nothing is fetched, nothing is sent anywhere.
And only in a machine whose firmware names a hypervisor; or, told -PhysicalArmPc, on a physical PC whose Windows is on
Arm (a virtual machine runs Windows on Arm only on an Arm host), whose guard file holds a second line that names its
firmware's maker and model and this one PC's SMBIOS UUID as Windows keeps them (Win32_ComputerSystemProduct):
"a physical Windows on Arm PC made for this test: <maker>, <model>, <UUID>" (on a virtual machine the guard file
holds the name alone, and -PhysicalArmPc is refused there). The guard file is made once, by whoever makes the PC one
for this test, as on a virtual machine; nothing that runs the test writes it. An account that is an
administrator of the PC but runs this unelevated (UAC's filtered token, as Windows starts programs for an
administrator) counts as a standard user, as Windows treats it; the preflight's JSON says which it is.

It runs on x64 Windows and on Windows 11 on Arm, and holds what is installed to the PC's own kind: x64 or arm64, by
Windows' own word of its machine (IsWow64Process2), whatever kind this PowerShell runs as. Every program and library of
the installed package is an image of that kind (its PE header, as the setup reads an earlier install's, ImageKind),
and so are its package-manifest.json's architecture and the kind the Settings > Apps entry notes ("qKVM Desktop
Architecture": every setup of 0.2.0 and later notes it, none before); qKVM Setup's update check at the program's name
while the earlier version is set aside is one of that kind; and the program, started (steps 02, 06, 07, 13, 54), runs
as that kind (step 55 starts the x64 version it put back, which runs as x64). On Windows on Arm both setups must be
ones that install there: the published 0.1.0 to 0.1.2's refuse Windows on Arm (x64os), and so does a setup of x64's
package alone. A release round there takes the release's own setup as -Setup and, as -OlderSetup, a throwaway older
build of the same commit: its version lowered in desktop/CMakeLists.txt (0.1.99, say), both packages built and one
setup made of them by tools/build_setup.ps1, never handed out. Steps 54 and 55 take 0.1.2's published x64 ZIP as
-OtherKindZip.

-Setup is the setup to try (qKVM-Desktop-<version>-windows-setup.exe, as tools/build_setup.ps1 makes it);
-OlderSetup one of an earlier version, which is installed first so that the upgrade is a real one. Each step writes
one JSON file into -Out (NN-<step>.json: passed, what was seen, each failure), and the setups' logs beside them;
summary.json at the end. Steps:
  00 preflight             Windows, x64 or Arm64 (the PC's kind), a standard user, nothing installed; both setups'
                           versions (older first); on Windows on Arm, Windows 11 and an older setup that installs there
  01 first-install         the older setup, silent: no administrator asked for (no consent prompt seen), exit 0,
                           the user's own entry in Settings > Apps (HKCU) and none for the PC (HKLM), exactly the
                           package's files in %LOCALAPPDATA%\Programs\qKVM Desktop with the marker and the
                           uninstaller, a Start menu entry and no desktop one
  02 program-starts        the program starts (its single-instance mutex appears), stays up, and is closed
  03 killed-upgrade        the new setup is killed once it has written Qt6Core.dll: the folder is compared, file by
                           file, with the earlier version's (what is still the earlier one's, what is the new one's
                           or gone or new), and the earlier program is found set aside for the copy (in every mode
                           since 0.1.2, review L3), with qKVM Setup's update check in its place (0.2.0, M3) until the
                           next setup puts it back; and (0.2.0)
                           the marker of an unfinished update is there, and every file of the earlier version is there
                           byte for byte, as itself or set aside beside it (name.old-update)
  04 upgrade               the new setup over the earlier version: exit 0, its version, exactly its files, the earlier
                           version put back first (03 left it aside), before this copy set it aside again, its log
                           naming it and its kind (this PC's, read from its program's PE header); a file
                           and a folder this puts in plugins and qml beforehand, and a file beside them, which no
                           version of ours installed, are all left (0.2.0 never walks a folder, M4); and the copy is
                           said to be on the disk before the earlier version goes (L13)
  05 downgrade-refused     the older setup over the new version, silent: it ends with a code that is not 0, and
                           nothing is changed
  06 program-starts        the upgraded program starts
  07 update-mode           the contract's update: the program runs, the setup is started with exactly
                           /SILENT /SUPPRESSMSGBOXES /NOCANCEL /NORESTART /CURRENTUSER /UPDATE=1 /DIR=".." /LOG="..",
                           the program ends within 10 s; the setup waits, installs, exits 0 and starts the
                           program again from its folder with the one argument --after-update, only once its copy
                           was whole and marked finished (nothing of an unfinished update left behind), and it runs
                           as the PC's kind; nothing
                           takes the program's mutex while the setup still holds its own (Local\qKVM-Desktop-Setup)
  08 update-still-running  the same with the program left running: exit 105 within about 60 s, nothing changed,
                           the program still running, and the setup's log ends with the contract's result line
                           (qKVM-RESULT: 105 ...)
  09 update-file-in-use    the same with the program ended but one of its files held open: the setup refuses
                           before it changes anything (Inno Setup's exit code 7), nothing changed, and its log has
                           the contract's result line (qKVM-RESULT: 7 nothing was changed: ...)
  10 uninstall-refused     the uninstaller while the program runs: refused, nothing removed
  11 uninstall             the uninstaller: the program's files, its Start menu entry and its Settings > Apps entry
                           gone; the program's data (%LOCALAPPDATA%\QKVM Desktop, %LOCALAPPDATA%\qKVM, Windows
                           Credential Manager) left as it was
  12 reinstall             the new setup again: exit 0, its files, the data still there; then the program, started
                           without --after-update while a setup's mutex is held, ends at once without taking its
                           own (the mutex held by this test, as a setup holds it: a reinstall ends a few
                           milliseconds after its files are in, too soon to start the program within it; starts
                           during a real copy are steps 07 and 14), within a minute, as a setup waits for such a
                           start: this first start of the new files waits for Windows' scanner to scan each of them
                           first (2026-11-21: 2 to 8 s for a whole copy on a test VM; some 60 ms once scanned); and
                           a second start under the same mutex, the files scanned by then, ends within 2 s, so that a
                           program that came to spend long before its look at the setup's mutex is seen here and not
                           hidden by the setup's minute (review armp-1 L5); both times are kept
  13 program-starts        the reinstalled program starts
  14 starts-during-copy    the new setup again, with the program started without an argument every half second while
                           the setup copies its files: every start finds the update check in the program's place
                           (the program set aside for the copy), which ends at once with 0, the install completes, no start takes the program's mutex while
                           the setup holds its own, none is left running without it (one that comes as the setup
                           ends may find it gone and run as the program does), and then the new version starts
  15 update-across-qt      the older version installed again, then the contract's update to the new one, whose Qt
                           libraries differ (where the two setups ship the same Qt, the step says so, notSeen, and
                           judges the rest), with the program started every half second during the copy: its file is
                           set aside for the copy, the update check in its place, the update completes, the aside
                           copy is gone, and the new version starts; and one start of the new program held suspended
                           from the moment its file is in until 15 s after the copy's end, as Windows' scanner holds
                           the first start of a copy just written (2026-11-21: the copy's flush, about 5 s of tries, failed
                           on such a start, and the update was lost): the setup waits for it to end (its log says so),
                           then judges the copy, and the update completes
  16 update-fails-during-copy  the older version again, then the contract's update with a file of the copy's taken and
                           held open (with no sharing) once the copy has begun, at the name of the last file it writes
                           before the program, and let go of as it rolls back: the update fails, and the earlier
                           version is back whole (every file byte for byte, nothing set aside left, no marker), the
                           setup starts it again itself (L5: the update had ended it), and it starts; the new setup then
                           installs as usual
  17 killed-then-uninstall  the new setup killed once its copy has reached licenses (what it leaves, in any folder, is
                           seen), then the uninstaller: exit 0, and the program's folder gone with nothing left in it;
                           the data left as it was
Steps 18 to 23 (0.1.2): a running copy closed for the person, the orderly way (the program's event, its answer in the
title of its window, ui/win/quit_requests.h); each starts from the new version installed (18 installs it after 17).
  18 tray-only-closed      the program set to stay in the tray (its settings' stay_in_tray), started, and its window
                           closed: it runs with no window to be seen, only its icon's. The new setup, silent: it asks
                           the program to close (its log says so), the program ends by itself (no forced end), the
                           setup installs (exit 0, its files) and starts it again; the setting is put back after
  19 busy-held             first a note beside the settings of a server's BIOS setup boot override whose doubt is
                           long over (a server at a loopback port where nothing answers, on no card, as B12 found
                           on 0.1.0's own profile): the program lets go of that record at its start, as SPEC says
                           of SetupOverrides::prune (its settings there, their list read whole), it holds nothing,
                           the silent setup closes the program and upgrades it, and changes nothing of the note.
                           Then the program holding
                           something that its end would cut (WallWindows::whatHoldsAnUpdate: a note for the same
                           server whose doubt goes on, a change that may still come): the new setup, silent, changes nothing
                           (exit 7, its files and entry as they were), the program still runs, and the result line
                           carries the program's own sentence; once the program has ended (and the note is gone), the
                           setup installs and does not start the program (it was not running then)
  20 hung                  the program not answering (every thread of it suspended): silent, the setup gives up
                           after about 10 s and changes nothing (exit 7, "did not answer within 10 seconds"); with
                           /FORCECLOSE=1 it ends the program by force (its log says so), installs, and starts it again
  21 zip-copy-beside       a copy of the program from another folder (the installed one copied without the setup's
                           marker and uninstaller: a portable ZIP's) running: the new setup, silent, finds no copy of
                           its own folder, installs (exit 0) and leaves the other copy running, never asked, and
                           starts nothing
  22 hung-end-it-now       interactive, driven as a person would through UI Automation: the program suspended; the
                           first page says what is installed and that it runs; Install asks "Close qKVM and
                           continue"; after about 10 s the setup says it did not answer and offers End it now, which
                           is chosen; the upgrade completes, the last page offers "Start qKVM Desktop" ticked, and
                           Finish starts it
  23 older-running         the older version installed again and running: a version before 0.1.2 cannot be asked,
                           so a silent install changes nothing (exit 7, "cannot be asked to close"), and interactive,
                           the first page says it upgrades the older version to the new one, the setup says how to
                           quit it from its icon and offers End it now, which is chosen, and the upgrade completes; an
                           older version of 0.1.2 or later is asked, and the silent install closes it and upgrades
  24 stalled-given-up      a request the setup gave up on (review M1): the program's interface stalled (its first
                           thread alone suspended) past the setup's wait; silent, the setup changes nothing (exit 7)
                           and withdraws its request; the interface answers again and the program stays, saying the
                           request came too late; then by hand, Close qKVM and continue, the box's Cancel once it says
                           the program did not answer (the setup still runs, on its Ready page), the interface answers
                           again, and the program stays; nothing changed
  25 fresh-install-uninstall-refused  a fresh install made outside update mode (uninstalled first, so that its uninstall
                           log is its own), the program started, the uninstaller: refused (not 0) as it begins, in
                           InitializeUninstall, nothing removed, the Settings > Apps and Start menu entries there
                           (review M2: what the uninstaller checks is the mutex this install kept in that log); then,
                           the program ended, Setup's mutex held as an open setup holds it: refused (not 0) as it
                           begins, nothing removed (review F4)
  26 closed-then-refused   the program running and a file of its package held (one it does not load): the silent setup
                           closes the program, then finds the file in use and changes nothing (exit 7), and starts the
                           program again as it ends, its result line saying so (review L1; room is looked at before
                           anything is closed)
  27 cannot-be-asked       the request's name taken first by another program (this test holds it), then the program
                           started: it has no answer window; the silent setup takes it for one that cannot be asked,
                           at once, never for one that did not answer, and changes nothing (exit 7); with
                           /FORCECLOSE=1 its log says plainly that it ends a copy it could not ask, and it upgrades
                           (review L2)
  28 busy-while-waiting    the program's interface busy for 6 s (its first thread suspended, let go 6 s into the setup)
                           while a silent install told /FORCECLOSE=1 waits its 10 s: the request still stands, the
                           program ends the orderly way, never by force, and the setup installs and starts it again
                           (review N1)
Steps 29 to 31 and 34 (0.2.0): the earlier version set aside for the copy (each file renamed to its name and
.old-update, beside the marker qkvm-desktop.update-unfinished), put back where the copy does not complete, and what an
update that did not finish left put right by the next setup first. Each starts from the older version installed again
(34 from the new one) and ends with the new one installed.
  29 copy-fails-keeps-earlier  a silent upgrade whose copy fails at its very end: the program's own file, the last the
                           copy writes, taken and held open by this test (with no sharing) once the copy has begun, past
                           the setup's look at the files in use, and let go of as it rolls back. The setup ends not 0;
                           its log says the earlier version is whole again; every file of the earlier version is there
                           byte for byte (its manifest's SHA-256), nothing set aside is left and no marker; the earlier
                           program starts. Then the new setup installs as usual
  30 power-cut-recovered   (M3) the new setup ended by force in the middle of its copy (once it has written
                           Qt6Core.dll), as a power cut ends it: the marker and the earlier version set aside are left,
                           and the update check in the program's place; a start of it (what the Start menu's entry
                           starts) says in a box titled qKVM "An update of qKVM did not finish. Choose OK, then run the
                           qKVM setup again: it puts the earlier version back, or finishes the update.", takes no mutex,
                           shows no wall, changes nothing, and OK ends it with 1. The next setup puts the earlier version
                           back first, installs, and the new version starts. Then a real cut after a whole copy: the
                           marker held from the start of the copy so that it cannot be made that of a finishing update,
                           the setup ended as it tries again; the new program in its place beside the marker says the
                           same in its own box, ends with 1 and changes nothing; the setup run again puts back first,
                           installs, and the program starts
  31 rename-refused        a file of the earlier version that Windows will not rename (this test, as the same user, denies
                           itself deleting it and deleting in its folder): Preparing to Install refuses (exit 7) with the
                           result line "qKVM-RESULT: 7 nothing was changed: a file of qKVM Desktop could not be set aside
                           for the copy (qt.conf)", every file of the earlier version is there byte for byte, nothing set
                           aside is left and no marker, the earlier program starts; the entries taken away, the new setup
                           installs
  32 uninstall-sees-profile-lock  (0.2.0) the lock of settings.json.running held by this test, as a copy of this
                           account in another Windows session holds it (or one another account's copy in this session
                           keeps from the mutex's name), the program not running here: the uninstaller refuses as it
                           begins, its log says why, and nothing is removed
  33 start-while-profile-held  (0.2.0) the same lock held: a start of the program shows a box titled qKVM that says it
                           runs for this account in another session, no wall beside it, and writes nothing of the
                           settings; OK ends it with exit code 0. The lock let go of, the next start takes it
  34 stale-marker          (0.2.0) the marker of an unfinished update left beside the new version, whose record names
                           nothing still set aside, and files of the ending that no record names (a person's, and one of
                           the package's own name): the program starts as usual, shows no box, takes the marker away and
                           leaves them as they are; then a silent setup ends 0, sets aside under the next ending
                           (.2.old-update, its log says) and leaves them byte for byte (H1)
  35 earlier-files-by-test-list  (0.2.0, M4) ONLY WITH -ListedSetup, a hand-built setup that is never handed out (below);
                           without it the step is not run, and the summary says so. The older version installed again,
                           with four files the test list gives it put in as it would have, and two of a person's: after
                           the silent upgrade by -ListedSetup the log says the older version was installed in this
                           folder, the four are gone, the folder they alone filled is gone, the folder that holds a
                           person's file stays with it, and the person's files stay; then -Setup installs over it again,
                           so that every other step runs on the setup that ships alone
  36 stranger-beside-upgrade (0.2.0, H1) the older version installed again, and files of the ending beside it that no
                           record names (a person's notes.txt.old-update, Qt6Core.dll.old-update and
                           licenses\AGPL-3.0.txt.old-update): a silent upgrade whose copy fails at the program's own file
                           (held as in 29) puts the earlier version back byte for byte with those files as they were, and
                           the earlier program starts; the silent upgrade then ends 0, sets aside under .2.old-update, and
                           leaves them byte for byte
  37 skip-is-not-whole     (0.2.0, M1) interactive, by UI Automation: the older version again; the upgrade, and once its
                           copy has begun, the last file before the program held by this test so that the copy cannot
                           write it; Inno Setup's box offers Skip this file, which is chosen once the file is let go of.
                           The copy goes on, then the setup finds that file not as the package has it: its box says
                           "qKVM Desktop <version> was not installed: the copy was not whole (...)", it ends with 7, every
                           file of the earlier version is back byte for byte (its uninstaller aside), nothing set aside
                           or marked, Settings > Apps names the earlier version again, and the earlier program starts.
                           (And step 04's log, L13: the copy said to be on the disk before the earlier version goes.)
  38 set-aside-not-removable  (0.2.0, M2, L1) the older version again; every file set aside made read-only by this test
                           during the copy: the upgrade ends 0 and nothing set aside or marked is left. Then again, with
                           Qt6Core.dll.old-update held by this test (no sharing of its deletion) through the upgrade: it
                           ends 0, that file alone stays, the finishing marker's record names it, and the program starts
                           with no box. The next setup with it still held ends 0 and carries it ("left .old-update
                           Qt6Core.dll" in its finishing marker); let go of, the setup after removes it, and nothing set
                           aside and no marker is left
  39 dir-into-foreign-folder (0.2.0, M4; review i020-2, M1) nothing installed; a silent first install by /DIR into
                           another program's folder is refused (exit 7, the result line "qKVM-RESULT: 7 nothing was
                           changed: the folder ... holds what is not an install of qKVM Desktop (...): ..."), with nothing
                           in Settings > Apps and that folder byte for byte with nothing added: once with names of its own
                           (in plugins, qml and beside them), once with names the package shares (vcruntime140.dll,
                           msvcp140.dll, Qt6Core.dll, plugins\platforms\qwindows.dll); then a first install by /DIR into
                           an empty folder ends 0, its log finds no earlier version of ours there, and its uninstall
                           leaves nothing once the uninstaller has removed itself (it does so after its first process
                           has ended); then the new version is installed in its own folder again
  40 cut-during-finishing  (0.2.0, M3) the older version again; three files set aside held by this test during the copy
                           so that their removal stalls, and the setup ended once its log says the first of them could
                           not be removed yet: the finishing marker is left with what was set aside, beside the new version,
                           which starts with no box; the next setup removes the rest first, installs, and nothing set
                           aside or marked is left (up to three tries, for the cut to come within those moments)
  41 put-back-then-refused (0.2.0, L4) the older version again, the upgrade cut in the middle of its copy, and Settings >
                           Apps' entry made to name 9.9.9: the setup then puts the earlier version back first and
                           refuses as older, its log saying it put the earlier version back and changed nothing else
                           (never "Nothing was changed."); the folder is the earlier version byte for byte; the entry put
                           back as it was, the setup installs
Steps 42 and after (0.2.0, the outside review of 0.2.0's setup, area 1):
  42 record-in-foreign-folder (1.1) nothing installed; another program's folder with Qt6Core.dll, its own
                           Qt6Core.dll.old-update and a file of its own, beside a marker of an unfinished update whose
                           record (one the setup reads) names Qt6Core.dll: a silent setup by /DIR into it does not begin,
                           its result line says the folder holds what is not an install of qKVM Desktop, the folder is
                           byte for byte as it was, nothing in Settings > Apps; then the new version in its own folder
  43 uninstall-left-aside  (1.8) the older version, the upgrade with Qt6Core.dll.old-update held as in 38; the uninstall with
                           it still held ends 0 and leaves that file and the finishing marker alone (once the
                           uninstaller has removed itself, after its first process has ended); let go of, the new
                           setup into the program's own folder ends 0, says it removed what the uninstall had left, and
                           nothing set aside or marked is left
  44 folder-others-may-change (1.2) nothing installed; a silent setup by /DIR into an empty folder the users of this PC may
                           change (given by this test, its owner), into a folder not made yet below one they may change,
                           into a folder not made yet below one whose files alone they may change ("files only", and
                           Write on "this folder and files": outside review 2, 1.2, every file installed below it would
                           be theirs to change), and
                           into a junction: each refused with 7 and its result line, nothing in Settings > Apps,
                           nothing added, the folder not made, its rights as they were; then the new version again
  45 account-gate          (1.3) this test holds the account's gate (Global\qKVM-Desktop-Setup-Account-<SID>) as a setup in
                           another session would: a silent setup does not begin within seconds, with its result line, the
                           uninstaller does not either, nothing changed; let go of, the setup ends 0
  46 uninstall-before-first-start (1.3) the lock file taken away as before a first start: the uninstall is seen holding the
                           lock, ends 0, removes the lock it made, the data as it was; then the new version again
  47 stale-marker-while-gate-held (1.4) a stale marker beside the new version and the account's gate held: the program
                           starts with no box and leaves the marker; the gate let go of, the next start takes it away
  48 unseen-aside-stays-named (1.5) the older version; an upgrade whose copy fails at the program's file (as 29), with a
                           file set aside made one this account may not look at: never said whole, the marker and its
                           record stay, the update check at the program's name; looked at again, the next setup puts back
                           first and installs
  49 flush-refused-keeps-earlier (1.6) the older version; a silent upgrade with its new Qt6Core.dll held so that its flush
                           is refused: exit 7, "could not all be written to the disk", nothing set aside removed, the
                           earlier version byte for byte and named, and it starts
  50 first-install-skip-starts-nothing (1.7) nothing installed; a first install by hand whose copy skips a file: its box
                           says it was not installed, the last page offers no Start qKVM Desktop, Finish starts nothing,
                           exit 7; then the new version silent
  51 start-held-past-the-minute (0.2.0) the older version; the contract's update with one start of the new program held
                           suspended from the moment its file is in until the setup has ended, past the minute the
                           setup waits for such starts: the setup ends with 7, its log says that qKVM Desktop, started
                           during the copy, still ran from its folder after a minute, nothing set aside is removed, and
                           the held start is never ended by the setup (this test ends it then); the next setup puts
                           right what was left and installs the new version whole
  52 interactive-start-held (review armp-1 L9) the new version installed; the new setup by hand (Next, Install), with
                           one start of the new program held suspended from the moment its file is in until 10 s
                           later: while it waits for that start the setup shows "Waiting for qKVM Desktop, started
                           during the install, to end" (whether its window answers meanwhile is recorded, not
                           judged: Inno Setup's page does not take messages while the script sleeps, a known limit);
                           let go of, the start ends by itself, the install completes, its log says it waited, Finish
                           starts nothing
  53 put-right-in-shared-folder (outside review 2, 1.3) the older version, its folder then given by this test (its owner)
                           to the users of this PC to change, and in it a marker of an unfinished update whose record
                           (one the setup reads) names Qt6Core.dll as what the copy made: a silent setup does not follow
                           it (its log says nothing is put right there), refuses the folder with 7 and its result line
                           naming the users of this PC and saying that what the update left stays as it is, and what to
                           do, and the folder is byte for byte as it was, the marker with it;
                           the same with the earlier program as a setup of 0.1.2 set it aside
                           (qkvm-desktop.exe.old-update, no marker), which is left; the folder's rights as this test left
                           them; then, those rights and files taken away, the new version installs
Steps 54 and 55 (0.2.0; the outside review of 0.2.0, 1.4): an earlier install of the other kind in the program's folder,
which only Windows on Arm holds: an x64 install (0.1.x's, or a later version's), there only where it was carried over
from an x64 PC. No setup of ours makes one there: 0.1.0 to 0.1.2's refuse Windows on Arm (x64os), and 0.2.0's installs
the ARM64 program there. Carried over with its Settings > Apps entry (by a tool that moves installed programs to a new
PC), the setup knows the version and kind installed in the folder, and removes what only that install had after a
whole copy, by that version's own list; copied by hand, without the entry, it installs over it and removes nothing of
it, which leaves the other kind's extra files (SPEC). The suite tries the first, the one the setup can put right: it
is planted, from -OtherKindZip (the ZIP of a published x64 version, 0.1.2's, whose list of files the setup carries):
the older version installed again, every file of its package taken away and the ZIP's package put in their place, file
by file as its manifest has them (plain names below the program's folder alone), beside the setup's own marker and
uninstaller, and the Settings > Apps entry made that version's (no kind noted where it is a version before 0.2.0, as
their setups noted none). Not run on x64 Windows (an install of the other kind could not start there, and no setup
makes one), nor without -OtherKindZip.
  54 other-kind-upgraded   the contract's update with the planted x64 program running (under Windows' emulation, its
                           process seen as x64): it ends, the setup installs exactly the new version's ARM64 package, its
                           log says that version (x64) was installed in this folder and names as removed each file only
                           the x64 package had (by that version's own list), none of them is left, the entry notes arm64,
                           and the setup starts the program again, which runs as ARM64
  55 other-kind-cut-put-back  the planted x64 install, and the setup ended by force once it has written Qt6Core.dll: every
                           file of the x64 install there or set aside as it was, the marker, qKVM Setup's ARM64 update
                           check at the program's name and the x64 program set aside beside it; the next setup puts the x64
                           version back first, then installs the ARM64 one and removes what only the x64 install had. Then
                           planted again, and a copy that fails at the program's own file (held as in 29): the x64 version
                           back byte for byte, nothing set aside or marked, its program starts and runs as x64; the new
                           setup then installs the ARM64 one as usual
Every step but 35 runs on -Setup, the setup as it ships, and the suite passes whole on the release build without
-ListedSetup (review i020-2, M2). Step 35 runs only on -ListedSetup: the same package as -Setup, made into a second setup
by hand by tools/build_setup.ps1 with -PublishedFiles naming a copy of packaging/published-files.json in which the older
setup's version's build of the PC's kind (x64's, or on Windows on Arm arm64's) also lists licenses/qkvm-vm-gone.txt,
qkvm-vm-gone/a.txt, qkvm-vm-gone/deep/b.txt and
qkvm-vm-kept/c.txt (names no package of ours has; nothing else of it differs from the release's). The versions of ours
published so far all installed the same names, so the setup that ships has nothing of theirs to remove, and step 35
could not bite on it. -ListedSetup is a test's own setup, never handed out: its SHA-256 is not the release's, and the
step's JSON names it.
Steps 22 to 24 press the setup's own buttons through UI Automation, as a person would: run the test in the signed-in
session with its desktop there (not in a remote desktop that was closed), and touch nothing while they run. Each step's
JSON keeps what the setup's windows showed before each press, and what they showed last where a press never came.
Step 15 sees an update across a change of Qt only where the two setups' Qt libraries differ. Two releases of the same Qt
(0.1.2 and 0.2.0 ship the same Qt6Core.dll) cannot show it: there the step judges the rest, passes on it, and names what
it did not see in its JSON and the summary (notSeen), which the release's record repeats. To see it, make the newer setup
from a copy of its package of the PC's kind in which one byte is appended to Qt6Core.dll (an overlay that Windows' loader leaves alone),
with that file's new size and SHA-256 written into package-manifest.json, by tools/build_setup.ps1. A setup ended from outside (killed) runs no code of its own: the
earlier version set aside for its copy stays aside (each file as name.old-update, beside the marker
qkvm-desktop.update-unfinished) until the next setup, which puts it back first (steps 16 and 29 make the copy fail
inside the setup instead, as a full disk or a file in use does).
The data is told apart by a file this puts in each data folder at the start (qkvm-setup-vm-test.txt) and by the
names of the program's own entries in Windows Credential Manager (cmdkey /list, those named QKVM Desktop: ...),
which no setup and no uninstaller may change. The program adds its own when it starts (its sign-in pacing key at
its first start), so they are listed again after each start of the program, and none it had may be gone then;
the entries Windows keeps there of its own (a Microsoft account's) are not looked at.
A failed install by a full disk is not tried here: it needs a volume made small for it, which needs an
administrator; the setup's own check of free space (in update mode) is read in its script by test_setup.ps1. The
killed setup (03) and the file in use (09) are the failed installs tried.
#>
param(
    [Parameter(Mandatory = $true)] [string] $Setup,
    [Parameter(Mandatory = $true)] [string] $OlderSetup,
    [Parameter(Mandatory = $true)] [string] $Out,
    [switch] $Disposable,
    # (The new setup made by hand with the test list, for step 35 alone; '' leaves step 35 not run.)
    [string] $ListedSetup = '',
    # (For steps 54 and 55, on Windows on Arm: the ZIP of an earlier published version's x64 package, planted as an
    # install carried over from an x64 PC; '' leaves them not run.)
    [string] $OtherKindZip = '',
    # (A physical PC on Windows on Arm bought for such tests: the head says what its guard file holds besides its name.)
    [switch] $PhysicalArmPc
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

# Where this runs, as Windows says it (GetComputerNameExW, SHGetKnownFolderPath), never as the environment does:
# COMPUTERNAME, USERPROFILE and LOCALAPPDATA are whoever starts this to set (review 012, N2; as k012's
# disposable_machine.h asks, and tools/test_upgrade_keeps_vm.ps1).
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class QkvmVmWhere {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern bool GetComputerNameExW(int kind, StringBuilder name, ref uint size);
    [DllImport("shell32.dll")] static extern int SHGetKnownFolderPath([MarshalAs(UnmanagedType.LPStruct)] Guid id, uint flags, IntPtr token, out IntPtr path);
    // The machine's own NetBIOS name, which the guard file holds ("" where Windows does not say): the physical one
    // (ComputerNamePhysicalNetBIOS, 4), never the cluster's that _CLUSTER_NETWORK_NAME_ gives the other kinds (review R2).
    public static string Machine() {
        StringBuilder name = new StringBuilder(64);
        uint size = (uint) name.Capacity;
        return GetComputerNameExW(4, name, ref size) ? name.ToString() : "";
    }
    // A known folder of whoever runs this ("" where Windows does not say).
    public static string Folder(string id) {
        IntPtr path;
        if (SHGetKnownFolderPath(new Guid(id), 0, IntPtr.Zero, out path) != 0) return "";
        try { return Marshal.PtrToStringUni(path); } finally { Marshal.FreeCoTaskMem(path); }
    }
    [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool IsWow64Process2(IntPtr process, out ushort processMachine, out ushort nativeMachine);
    // The machine Windows itself runs on (IMAGE_FILE_MACHINE_AMD64 0x8664, IMAGE_FILE_MACHINE_ARM64 0xAA64, ...), whatever
    // kind this process is of (a PowerShell under Windows on Arm's emulation is told the PC's own); 0 where Windows does
    // not say.
    public static int NativeMachine() {
        ushort process, native;
        return IsWow64Process2(GetCurrentProcess(), out process, out native) ? native : 0;
    }
}
'@
$Machine = [QkvmVmWhere]::Machine()
# (FOLDERID_Profile: Windows has it from the user's own record, whatever USERPROFILE says.)
$UserFolder = [QkvmVmWhere]::Folder('5E6C858F-0E22-4760-9AFE-EA3317B67173')

# --- Never anywhere but on a machine made for it.
if (-not $Disposable) { throw 'This installs and uninstalls qKVM Desktop for real. Run it only in a throwaway virtual machine, with -Disposable.' }
if (-not $Machine -or -not $UserFolder) { throw 'Windows did not say which machine this is, or where this user''s folder is.' }
$guard = Join-Path $UserFolder 'qkvm-disposable-vm.txt'
# (Its lines: this machine's name; and on a physical Windows on Arm PC, the words of its firmware below.)
$guardLines = @()
if (Test-Path -LiteralPath $guard) { $guardLines = @(([string] (Get-Content -LiteralPath $guard -Raw)).Trim() -split '\r?\n' | ForEach-Object { $_.Trim() }) }
if (-not $guardLines.Count -or $guardLines[0] -ne $Machine) {
    throw "There is no $guard holding this machine's name ($Machine): this is not a machine made for this test."
}
function Get-Hypervisor([string] $Maker, [string] $Product, [string] $Vendor) {
    <# The hypervisor the firmware's own words name (QEMU, VMware, Hyper-V, VirtualBox); '' where they name none. #>
    if ($Maker -match '\bQEMU\b' -or $Product -match '\bQEMU\b') { return 'QEMU' }
    if ($Maker -match '\bVMware\b' -or $Product -match '\bVMware\b') { return 'VMware' }
    if ($Maker -eq 'Microsoft Corporation' -and $Product -eq 'Virtual Machine') { return 'Hyper-V' }
    if ($Product -match '\bVirtualBox\b' -or $Maker -match '^innotek\b') { return 'VirtualBox' }
    return ''
}
function Get-FirmwareWord([string] $Name) {
    <# One of the firmware's words as Windows keeps them for everyone to read (HKLM\HARDWARE\DESCRIPTION\System\BIOS). #>
    $bios = Get-ItemProperty -LiteralPath 'HKLM:\HARDWARE\DESCRIPTION\System\BIOS' -ErrorAction SilentlyContinue
    if (-not $bios -or -not $bios.PSObject.Properties[$Name]) { return '' }
    return ([string] $bios.$Name).Trim()
}
# (And a machine whose firmware says it is a virtual one, as a standard user can read it: a guard file alone can be put
# on any machine, review T5.)
$Hypervisor = Get-Hypervisor (Get-FirmwareWord 'SystemManufacturer') (Get-FirmwareWord 'SystemProductName') (Get-FirmwareWord 'BIOSVendor')
# The kind of Windows this PC is, by Windows' own word of its machine: what the setup installs here, and what every check
# below holds the installed program to (the head of this test).
$NativeMachine = [QkvmVmWhere]::NativeMachine()
$PcKind = switch ($NativeMachine) { 0x8664 { 'x64' } 0xAA64 { 'arm64' } default { '' } }
$OtherPcKind = switch ($PcKind) { 'x64' { 'arm64' } 'arm64' { 'x64' } default { '' } }
if ($Hypervisor) {
    if ($PhysicalArmPc) { throw "This is a virtual machine ($Hypervisor): run this without -PhysicalArmPc." }
    if ($guardLines.Count -ne 1) { throw "$guard holds more than this machine's name: on a virtual machine it holds the name alone." }
} else {
    # (A physical PC is taken only told so at each run, only where Windows is on Arm, which a virtual machine runs only on
    # an Arm host, and only where its guard file names the very firmware Windows says it has, and this one PC's own number,
    # its SMBIOS UUID, which every user may read, beside its name: a guard file made for a virtual machine, for another PC,
    # or for another PC of the same model, is never one for this PC. One whose firmware leaves the UUID blank names no PC.)
    if (-not $PhysicalArmPc) {
        throw "This machine's firmware names no hypervisor (QEMU, VMware, Hyper-V or VirtualBox: it says $(Get-FirmwareWord 'SystemManufacturer'), $(Get-FirmwareWord 'SystemProductName')): this test runs in a virtual machine alone, or on a Windows on Arm PC made for it (-PhysicalArmPc, as the head of this test says)."
    }
    if ($PcKind -ne 'arm64') { throw "-PhysicalArmPc is for a PC whose Windows is on Arm, and this one's machine is $('0x{0:X4}' -f $NativeMachine): this test runs in a virtual machine alone here." }
    $uuid = ([string] (Get-CimInstance -ClassName Win32_ComputerSystemProduct -ErrorAction SilentlyContinue).UUID).Trim()
    if ($uuid -notmatch '\A[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\z' -or $uuid -match '\A[0-]+\z' -or $uuid -match '\A[Ff-]+\z') {
        throw "This PC's firmware gives it no UUID of its own ('$uuid'): a physical PC is taken only by its own."
    }
    $physicalWords = "a physical Windows on Arm PC made for this test: $(Get-FirmwareWord 'SystemManufacturer'), $(Get-FirmwareWord 'SystemProductName'), $uuid"
    if ($guardLines.Count -ne 2 -or $guardLines[1] -cne $physicalWords) {
        throw "$guard does not say, on the line after this machine's name, '$physicalWords': this is not a PC made for this test."
    }
    $Hypervisor = 'none: a physical Windows on Arm PC made for this test'
}
# (FOLDERID_LocalAppData, as the program itself finds it; only once the guard has passed.)
$Local = [QkvmVmWhere]::Folder('F1B32785-6FBA-4FCF-9D55-7B8E7F157091')
if (-not $Local) { throw 'Windows did not say where this user''s local application data is.' }

$AppId = '{A964EFA6-87C6-4A23-A9CC-B71F87441433}_is1'
$ProgramMutex = 'Local\QKVM Desktop'
# The setup's own, held while it runs (SetupMutex=qKVM-Desktop-Setup, in the session's namespace).
$SetupMutex = 'Local\qKVM-Desktop-Setup'
# How long a setup past its time is waited for with no new line in its log (Invoke-Setup, V1b).
$StallSeconds = 300
$App = Join-Path $Local 'Programs\qKVM Desktop'
$Program = Join-Path $App 'qkvm-desktop.exe'
# The setup's marker of an update whose copy has not finished, and the ending of each file it sets aside for its copy
# (0.2.0; packaging/qkvm-desktop.iss, ui/win/unfinished_update.h).
$UpdateMarker = Join-Path $App 'qkvm-desktop.update-unfinished'
$AsideEnding = '.old-update'
$UserKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\$AppId"
$MachineKeys = @("HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\$AppId",
    "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\$AppId")
$StartMenuEntry = Join-Path ([Environment]::GetFolderPath('Programs')) 'qKVM Desktop.lnk'
$DesktopEntry = Join-Path ([Environment]::GetFolderPath('Desktop')) 'qKVM Desktop.lnk'
$DataFolders = @((Join-Path $Local 'QKVM Desktop'), (Join-Path $Local 'qKVM'))
$Sentinel = 'qkvm-setup-vm-test.txt'
$SilentArguments = '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART'

$Setup = (Resolve-Path -LiteralPath $Setup).Path
$OlderSetup = (Resolve-Path -LiteralPath $OlderSetup).Path
if ($ListedSetup) { $ListedSetup = (Resolve-Path -LiteralPath $ListedSetup).Path }
if (-not (Test-Path -LiteralPath $Out)) { New-Item -ItemType Directory -Path $Out | Out-Null }
$Out = (Resolve-Path -LiteralPath $Out).Path
if (@(Get-ChildItem -LiteralPath $Out -Force).Count) { throw "$Out is not empty: each run writes its results into a new folder." }

$results = New-Object System.Collections.ArrayList

function Get-Sha256([string] $File) {
    <# A file's SHA-256. One that cannot be read at that moment (a setup this test has just ended still closing it, or
    Windows' scanner looking at a file just written) is read again a quarter of a second apart, for up to 10 s, and then
    the error stands (step 03 on a test VM found a temporary file of the setup it had just killed held so, 2026-11-21). #>
    for ($try = 1; ; $try++) {
        try { return (Get-FileHash -LiteralPath $File -Algorithm SHA256 -ErrorAction Stop).Hash.ToLowerInvariant() }
        catch { if ($try -ge 40) { throw }; Start-Sleep -Milliseconds 250 }
    }
}

function Get-KindOfMachine([int] $Number) {
    <# A machine's number (IMAGE_FILE_MACHINE_*) as this test names a kind of Windows image: x64, arm64, x86, or the
    number itself; '' for none. #>
    switch ($Number) { 0 { return '' } 0x8664 { return 'x64' } 0xAA64 { return 'arm64' } 0x14C { return 'x86' } default { return ('0x{0:X4}' -f $Number) } }
}

function Get-HeadKind([byte[]] $Head, [int] $Count) {
    <# Of which kind a Windows image is, by its PE header in the first $Count bytes of $Head: the machine its image is for,
    as the setup reads an earlier install's (ImageKind); '' where they are not the start of a Windows image. #>
    if ($Count -lt 64 -or $Head[0] -ne 0x4D -or $Head[1] -ne 0x5A) { return '' }
    $at = [BitConverter]::ToInt32($Head, 0x3C)
    if ($at -lt 64 -or $at + 6 -gt $Count) { return '' }
    if ($Head[$at] -ne 0x50 -or $Head[$at + 1] -ne 0x45 -or $Head[$at + 2] -ne 0 -or $Head[$at + 3] -ne 0) { return '' }
    return (Get-KindOfMachine ([BitConverter]::ToUInt16($Head, $at + 4)))
}

function Read-Head([IO.Stream] $Stream, [byte[]] $Head) {
    <# As much of the stream's start as fills $Head (or all of it, where it is shorter): how many bytes were read. #>
    $count = 0
    while ($count -lt $Head.Length) {
        $got = $Stream.Read($Head, $count, $Head.Length - $count)
        if ($got -le 0) { break }
        $count += $got
    }
    return $count
}

function Get-ImageKind([string] $Path) {
    <# Of which kind of Windows a program or library on the disk is (Get-HeadKind); '' where it is not there, cannot be
    read, or is no Windows image. #>
    $head = New-Object byte[] 4096
    try { $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)) } catch { return '' }
    try { $count = Read-Head $stream $head } finally { $stream.Dispose() }
    return (Get-HeadKind $head $count)
}

# (Of which kind a running process's code is: Windows 11's ProcessMachineTypeInfo, which tells an x64 program under
# Windows on Arm's emulation from an ARM64 one; where Windows has no such class (Windows 10), IsWow64Process2's, by which
# a process that is not WOW64's is of the PC's own kind.)
if (-not ('QkvmVmProcessKind' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class QkvmVmProcessKind {
    [StructLayout(LayoutKind.Sequential)] struct MachineInformation { public ushort ProcessMachine; public ushort Reserved; public uint Attributes; }
    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr OpenProcess(uint access, bool inherit, int id);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool GetProcessInformation(IntPtr process, int kind, out MachineInformation information, int size);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool IsWow64Process2(IntPtr process, out ushort processMachine, out ushort nativeMachine);
    // The machine (IMAGE_FILE_MACHINE_*) the code of process id runs as; 0 where it cannot be told (gone, or not opened).
    public static int Of(int id) {
        IntPtr process = OpenProcess(0x1000, false, id);
        if (process == IntPtr.Zero) return 0;
        try {
            MachineInformation information;
            if (GetProcessInformation(process, 9, out information, Marshal.SizeOf(typeof(MachineInformation)))) return information.ProcessMachine;
            ushort machine, native;
            if (IsWow64Process2(process, out machine, out native)) return machine != 0 ? machine : native;
            return 0;
        } finally { CloseHandle(process); }
    }
}
'@
}
function Get-ProcessKind([int] $Id) { return (Get-KindOfMachine ([QkvmVmProcessKind]::Of($Id))) }

function Test-RunsAsKind([string] $Wanted, [System.Collections.ArrayList] $Failures, [string] $What = 'the program') {
    <# Every running copy of the installed program runs as $Wanted (x64 or arm64); what each runs as. (A copy that has
    ended between Windows' list and the look, as the program started with --after-update ends once it has started
    itself again, is left out; one still there whose kind cannot be told counts against it.) #>
    $kinds = @(Get-ProgramProcesses | ForEach-Object {
            $id = [int] $_.ProcessId
            $runsAs = Get-ProcessKind $id
            if ($runsAs -or (Get-Process -Id $id -ErrorAction SilentlyContinue)) { $(if ($runsAs) { $runsAs } else { 'not to be told' }) }
        })
    if (-not $kinds.Count) { [void] $Failures.Add("$What is not running, to tell what it runs as") }
    elseif (@($kinds | Where-Object { $_ -ne $Wanted }).Count) { [void] $Failures.Add("$What runs as $(@($kinds | Sort-Object -Unique) -join ', '), not as $Wanted") }
    return $kinds
}

function Get-SetupProcessesLeft {
    <# Every process of this account that runs from Inno Setup's own temporary folders (setup.tmp, in is-XXXXXXXXXX.tmp
    under the Temp folder Inno Setup takes, Windows' GetTempPath) or from the program's folder: its number, file and
    start. (Step 03 names so what is left as it reads the folder after its kill.) #>
    $temp = [IO.Path]::GetTempPath()
    return @(Get-CimInstance Win32_Process | Where-Object {
            $_.ExecutablePath -and (($_.ExecutablePath -like "$($temp)is-*.tmp\*") -or ($_.ExecutablePath -like "$App\*")) } |
        ForEach-Object { [ordered]@{ id = [int] $_.ProcessId; file = [string] $_.ExecutablePath; began = [string] $_.CreationDate } })
}

# How long things take, as this suite measures them itself (a setup's run, a wait, a hold), is read from a monotonic
# clock (Get-Clock: seconds since the suite began), never from the wall clock, which Windows' time service may step: the
# Windows 10 test VMs start from their snapshot with the clock 8 h fast and are stepped back some minutes later, and step
# 20 on one of them read a setup's 14 s as -28786 s (2026-11-23). What has the wall clock's times only (a setup's log,
# a process's start, the notes the program reads) cannot be held so: a step during which the wall clock moved more than
# 2 s against the monotonic one fails, saying so (Write-Step), never judged as if nothing had happened.
$Clock = [Diagnostics.Stopwatch]::StartNew()
$ClockWallBegan = [DateTime]::UtcNow
$ClockShift = 0.0
function Get-Clock { return $script:Clock.Elapsed.TotalSeconds }
function Get-ClockShift { return ([DateTime]::UtcNow - $script:ClockWallBegan).TotalSeconds - $script:Clock.Elapsed.TotalSeconds }

function Write-Step([string] $Number, [string] $Name, [DateTime] $Began, [System.Collections.ArrayList] $Failures, $Seen, [string] $NotSeen = '') {
    <# -NotSeen: what of the step these two setups could not show, and why; the rest was judged. It is said in the step's
    JSON, in the summary (notSeen) and in its last line, and fails nothing. #>
    $shift = Get-ClockShift
    $moved = $shift - $script:ClockShift
    $script:ClockShift = $shift
    if ([Math]::Abs($moved) -gt 2) {
        [void] $Failures.Add("the wall clock was moved by $([Math]::Round($moved)) s during this step (Windows' time service sets it): the times this step read from setup logs and from Windows cannot be judged; run the suite once the machine's clock is set")
    }
    $step = [ordered]@{
        step = "$Number-$Name"; passed = ($Failures.Count -eq 0)
        began = $Began.ToUniversalTime().ToString('o'); ended = [DateTime]::UtcNow.ToString('o')
        failures = @($Failures); seen = $Seen
    }
    if ($NotSeen) { $step.notSeen = $NotSeen }
    $step | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $Out "$Number-$Name.json") -Encoding UTF8
    $entry = [ordered]@{ step = $step.step; passed = $step.passed; failures = $Failures.Count }
    if ($NotSeen) { $entry.notSeen = $NotSeen }
    [void] $results.Add($entry)
    Write-Output ("{0} {1}-{2}{3}{4}" -f $(if ($step.passed) { 'passed:' } else { 'FAILED:' }), $Number, $Name, $(if ($Failures.Count) { ': ' + ($Failures -join '; ') } else { '' }), $(if ($NotSeen) { " (not seen: $NotSeen)" } else { '' }))
}

function Write-NotRun([string] $Number, [string] $Name, [string] $Why) {
    <# A step this run does not make, and why: passed is not claimed of it, and it fails nothing. #>
    $now = [DateTime]::UtcNow.ToString('o')
    $step = [ordered]@{ step = "$Number-$Name"; passed = $true; notRun = $Why; began = $now; ended = $now; failures = @(); seen = $null }
    $step | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $Out "$Number-$Name.json") -Encoding UTF8
    [void] $results.Add([ordered]@{ step = $step.step; passed = $true; failures = 0; notRun = $Why })
    Write-Output "not run: $Number-$Name`: $Why"
}

function Test-Mutex([string] $Name) {
    $mutex = $null
    if ([Threading.Mutex]::TryOpenExisting($Name, [ref] $mutex)) { $mutex.Dispose(); return $true }
    return $false
}
function Test-ProgramRunning { return (Test-Mutex $ProgramMutex) }
function Test-SetupRunning { return (Test-Mutex $SetupMutex) }

# A look that Windows refuses is "not yet", not the end of the suite: a file Windows is still deleting (an uninstaller
# that removes itself, its delete pending) answers Test-Path with access denied, which this script's Stop made a
# terminating error (seen once on a Windows 11 test VM in step 43, the uninstall itself having succeeded).
function Test-Condition([scriptblock] $Condition) {
    try { return [bool] (& $Condition) } catch { return $false }
}
function Wait-Until([scriptblock] $Condition, [int] $Seconds) {
    $until = (Get-Clock) + $Seconds
    while ((Get-Clock) -lt $until) {
        if (Test-Condition $Condition) { return $true }
        Start-Sleep -Milliseconds 250
    }
    return (Test-Condition $Condition)
}

function Get-ProgramProcesses {
    return @(Get-CimInstance Win32_Process -Filter "Name = 'qkvm-desktop.exe'" | Where-Object { $_.ExecutablePath -and $_.ExecutablePath -ieq $Program })
}

function Start-Program {
    <# Starts the installed program as this user; whether its mutex appeared within 30 s, and its process. #>
    $process = Start-Process -FilePath $Program -WorkingDirectory $App -PassThru
    $up = Wait-Until { Test-ProgramRunning } 30
    return [pscustomobject]@{ Process = $process; Up = $up }
}

function Stop-Program([int] $Within = 10) {
    <# Closes every running copy of the installed program, by its window first; how each ended. #>
    $how = New-Object System.Collections.ArrayList
    foreach ($found in Get-ProgramProcesses) {
        $process = Get-Process -Id $found.ProcessId -ErrorAction SilentlyContinue
        if (-not $process) { continue }
        [void] $process.CloseMainWindow()
        if ($process.WaitForExit($Within * 1000)) { [void] $how.Add("closed ($($found.ProcessId))") }
        else { Stop-Process -Id $found.ProcessId -Force; [void] $how.Add("ended ($($found.ProcessId))") }
    }
    [void] (Wait-Until { -not (Test-ProgramRunning) } 10)
    return @($how)
}

function Stop-Tree([int] $Id, [DateTime] $Since = [DateTime]::MinValue, [Nullable[DateTime]] $Listed = $null, [switch] $FromList) {
    <#
    Ends the process and what it started, children first. A child only where it began at or after its parent: Windows
    keeps a process's ParentProcessId after its parent has ended, and the parent's number may since be another's, the
    setup's among them, whose "children" are then older programs of the person's (review T1). And each only while its
    number is still the process it was: one that began before $Since (its parent's start) is another's by now.
    (Windows keeps those starts by the wall clock: a step during which that clock moved fails in Write-Step.)
    #>
    # (Held open by its handle from here on: while it is held, its number is its own, and it is told and ended by that
    # handle, never by the number again. A child is ended only where the process its number opens began when Windows'
    # list said that child began, $Listed: one that ended between the list and the opening, its number taken by another,
    # is never ended for it; review R7. The same start exactly, at the list's own precision: Windows keeps a start to
    # 100 ns, CIM says it to the microsecond, cut, not rounded; so the opened process's, cut to the microsecond in UTC,
    # must be the listed one, tick for tick. Where either start is not there, the child is left alone; review F6.)
    $process = $null
    $began = $null
    try {
        $process = [Diagnostics.Process]::GetProcessById($Id)
        [void] $process.Handle
        $began = $process.StartTime
    } catch {
        if ($process) { $process.Dispose() }
        return
    }
    try {
        if ($null -eq $began) { return }
        if ($began -lt $Since) { return }
        if ($FromList) {
            if ($null -eq $Listed) { return }
            $opened = $began.ToUniversalTime().Ticks
            if (($opened - ($opened % 10)) -ne ([DateTime] $Listed).ToUniversalTime().Ticks) { return }
        }
        foreach ($child in @(Get-CimInstance Win32_Process -Filter "ParentProcessId = $Id")) { Stop-Tree ([int] $child.ProcessId) $began $child.CreationDate -FromList }
        if (-not $process.HasExited) { $process.Kill() }
    } catch {
        # (Ended meanwhile, or not ours to end: left as it is.)
    } finally {
        $process.Dispose()
    }
}

function Invoke-Setup([string] $File, [string] $Arguments, [int] $Seconds = 300, [scriptblock] $During = $null, [scriptblock] $Patient = $null) {
    <#
    Runs a setup (or the uninstaller) as this user and waits for it: its exit code (or 'killed' / 'timeout'), how long
    it took, and whether Windows' consent prompt for an administrator was seen while it ran. $During is called every
    quarter of a second with the process and the seconds gone; it returns $true to have the setup killed.
    Past $Seconds it is ended only where it is not copying (its log, where /LOG names one, says so: a setup ended as it
    copies leaves the folder half one version, and the steps after it fail for that; review V1), nor $Patient (called
    as $During is) says to wait, and never later than an hour past $Seconds. Nor is it waited for there once its log
    has not grown for 5 minutes: it is ended then, 'stalled', and the step fails and the run goes on (a run of the
    suite waited on one setup for hours; V1b).
    #>
    $start = New-Object System.Diagnostics.ProcessStartInfo
    $start.FileName = $File
    $start.Arguments = $Arguments
    $start.UseShellExecute = $false
    $began = [DateTime]::UtcNow
    $t0 = Get-Clock
    $process = [Diagnostics.Process]::Start($start)
    $consent = $false
    $ended = $null
    $logged = if ($Arguments -match '/LOG="([^"]+)"') { $Matches[1] } else { '' }
    $length = -1
    $grew = $t0
    while (-not $process.HasExited) {
        if (Get-Process -Name consent -ErrorAction SilentlyContinue) { $consent = $true }
        $gone = (Get-Clock) - $t0
        if ($logged) {
            $now = Get-Item -LiteralPath $logged -ErrorAction SilentlyContinue
            $size = if ($now) { $now.Length } else { -1 }
            if ($size -ne $length) { $length = $size; $grew = Get-Clock }
        }
        if ($During -and (& $During $process $gone)) { Stop-Tree $process.Id; $ended = 'killed'; break }
        if ($gone -gt $Seconds) {
            $stalled = $logged -and ((Get-Clock) - $grew) -ge $StallSeconds
            $waits = -not $stalled -and $gone -lt $Seconds + 3600 -and (($logged -and (Test-Copying $logged)) -or ($Patient -and (& $Patient $process $gone)))
            if (-not $waits) { Stop-Tree $process.Id; $ended = if ($stalled) { 'stalled' } else { 'timeout' }; break }
        }
        Start-Sleep -Milliseconds 250
    }
    $process.WaitForExit(10000) | Out-Null
    if (-not $ended) { $ended = $process.ExitCode }
    return [pscustomobject]@{ Exit = $ended; Seconds = [Math]::Round((Get-Clock) - $t0, 1); Consent = $consent; Began = $began }
}

function Get-Installed {
    <# The program's folder, file by file with its SHA-256, and its Settings > Apps entries. #>
    $files = [ordered]@{}
    if (Test-Path -LiteralPath $App) {
        foreach ($file in Get-ChildItem -LiteralPath $App -Recurse -File -Force | Sort-Object FullName) {
            $files[$file.FullName.Substring($App.Length + 1).Replace('\', '/')] = [ordered]@{ sha256 = Get-Sha256 $file.FullName; written = $file.LastWriteTimeUtc.ToString('o') }
        }
    }
    $entry = $null
    if (Test-Path -LiteralPath $UserKey) {
        $values = Get-ItemProperty -LiteralPath $UserKey
        $entry = [ordered]@{}
        # (And the kind of Windows the setup notes it installed for, 0.2.0 and later's: the head of this test.)
        foreach ($name in 'DisplayName', 'DisplayVersion', 'Publisher', 'InstallLocation', 'UninstallString', 'DisplayIcon', 'qKVM Desktop Architecture') {
            $entry[$name] = $(if ($values.PSObject.Properties[$name]) { [string] $values.$name } else { $null })
        }
    }
    return [pscustomobject]@{ Files = $files; Entry = $entry; Machine = @($MachineKeys | Where-Object { Test-Path -LiteralPath $_ })
        StartMenu = (Test-Path -LiteralPath $StartMenuEntry); Desktop = (Test-Path -LiteralPath $DesktopEntry) }
}

function Test-Installed([string] $Version, [System.Collections.ArrayList] $Failures) {
    <# The installed program as a setup of $Version leaves it: exactly the package's files, the marker, the uninstaller. #>
    $state = Get-Installed
    if (-not $state.Entry) { [void] $Failures.Add('there is no entry in Settings > Apps for the user') }
    else {
        if ($state.Entry.DisplayName -ne 'qKVM Desktop' -or $state.Entry.Publisher -ne 'qKVM') { [void] $Failures.Add("the Settings > Apps entry is $($state.Entry.DisplayName) of $($state.Entry.Publisher)") }
        if ($state.Entry.DisplayVersion -ne $Version) { [void] $Failures.Add("the Settings > Apps entry says version $($state.Entry.DisplayVersion), not $Version") }
        # (The kind it notes, where its setup notes one: every setup of 0.2.0 and later does, -Setup among them.)
        $noted = $state.Entry['qKVM Desktop Architecture']
        if ($noted -and $noted -cne $PcKind) { [void] $Failures.Add("the Settings > Apps entry notes $noted, not this PC's kind ($PcKind)") }
        if (-not $noted -and $Version -eq $newVersion) { [void] $Failures.Add("the Settings > Apps entry does not note the kind installed ($PcKind)") }
    }
    if ($state.Machine.Count) { [void] $Failures.Add("there is an entry for the whole PC: $($state.Machine -join ', ')") }
    if (-not $state.StartMenu) { [void] $Failures.Add('there is no Start menu entry') }
    if ($state.Desktop) { [void] $Failures.Add('there is a desktop entry, which was not asked for') }
    $manifestFile = Join-Path $App 'package-manifest.json'
    if (-not (Test-Path -LiteralPath $manifestFile)) { [void] $Failures.Add('the program''s folder has no package-manifest.json'); return $state }
    $manifest = Get-Content -LiteralPath $manifestFile -Raw | ConvertFrom-Json
    if ($manifest.version -ne $Version) { [void] $Failures.Add("the installed package is version $($manifest.version), not $Version") }
    # (The package of this PC's kind, by its manifest and by each of its programs and libraries: the setup chooses the
    # PC's own, IsArm64.)
    $packageKind = if ($manifest.PSObject.Properties['architecture']) { [string] $manifest.architecture } else { '' }
    if ($packageKind -cne $PcKind) { [void] $Failures.Add("the installed package is for $(if ($packageKind) { $packageKind } else { 'no kind' }), not this PC's kind ($PcKind)") }
    $wanted = @{}
    foreach ($entry in $manifest.files) { $wanted[$entry.path] = $entry.sha256 }
    $otherImages = New-Object System.Collections.ArrayList
    foreach ($path in $wanted.Keys) {
        if (-not $state.Files.Contains($path)) { [void] $Failures.Add("$path is missing") }
        elseif ($state.Files[$path].sha256 -ne $wanted[$path]) { [void] $Failures.Add("$path is not the package's") }
        elseif ($path -match '\.(exe|dll)$') {
            $imageKind = Get-ImageKind (Join-Path $App ($path -replace '/', '\'))
            if ($imageKind -cne $PcKind) { [void] $otherImages.Add("$path ($(if ($imageKind) { $imageKind } else { 'no Windows image' }))") }
        }
    }
    if ($otherImages.Count) { [void] $Failures.Add("programs or libraries of the installed package are not of this PC's kind ($PcKind): $(@($otherImages | Sort-Object) -join ', ')") }
    $own = @('package-manifest.json', 'qkvm-desktop.installed', 'unins000.exe', 'unins000.dat')
    foreach ($path in $state.Files.Keys) {
        if (-not $wanted.ContainsKey($path) -and $own -notcontains $path) { [void] $Failures.Add("$path is in the program's folder, and not in the package") }
    }
    foreach ($path in $own) { if (-not $state.Files.Contains($path)) { [void] $Failures.Add("$path is missing") } }
    $marker = Join-Path $App 'qkvm-desktop.installed'
    if ((Test-Path -LiteralPath $marker) -and -not ([Convert]::ToBase64String([IO.File]::ReadAllBytes($marker)) -ceq 'c2V0dXAK')) {
        [void] $Failures.Add('the marker is not "setup" and a line feed')
    }
    return $state
}

# The names of the program's own entries in Windows Credential Manager (QKVM Desktop: ...), sorted, one per line.
function Get-ProgramCredentials {
    $names = New-Object System.Collections.Generic.List[string]
    foreach ($line in @(& cmdkey.exe /list)) {
        if ([string] $line -match '(QKVM Desktop: .+?)\s*$') { $names.Add($Matches[1]) }
    }
    return ((@($names) | Sort-Object -Unique) -join "`n")
}

# A setup's log as it is now, while the setup may still be writing it: opened so that its writer may go on.
function Read-Shared([string] $Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return '' }
    try {
        $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    } catch [IO.IOException] { return '' }
    try { return [IO.StreamReader]::new($stream, $true).ReadToEnd() } finally { $stream.Dispose() }
}

function Test-Copying([string] $Log) {
    <# Whether the setup of this log is copying now: it began installing and has neither ended its copy nor rolled back. #>
    $said = Read-Shared $Log
    return ($said.Contains('Starting the installation process.') -and -not $said.Contains('Installation process succeeded.') -and
        -not $said.Contains('Rolling back changes.') -and -not $said.Contains('Deinitializing Setup.'))
}

function Get-LogSeconds([string] $Name, [string] $From, [string] $To) {
    <# The seconds between the first line of a setup's log that says $From and the first after it that says $To, by the
    times the setup put on them itself; $null where either is not there. What the setup did when, never when a look at
    its windows saw it (review V1). (Wall clock times: a step during which that clock moved fails in Write-Step.) #>
    $at = $null
    foreach ($line in @((Read-Log $Name) -split "`r?`n")) {
        if ($line -notmatch '^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3}) ') { continue }
        $when = [DateTime]::ParseExact($Matches[1], 'yyyy-MM-dd HH:mm:ss.fff', [Globalization.CultureInfo]::InvariantCulture)
        if ($null -eq $at -and $line.Contains($From)) { $at = $when; continue }
        if ($null -ne $at -and $line.Contains($To)) { return [Math]::Round(($when - $at).TotalSeconds, 1) }
    }
    return $null
}

function Test-Data([System.Collections.ArrayList] $Failures, [switch] $ProgramStarted) {
    foreach ($folder in $DataFolders) {
        $file = Join-Path $folder $Sentinel
        # (Read as it was written, UTF-8: a folder of a name with signs outside ASCII is its own, review T2.)
        if (-not (Test-Path -LiteralPath $file) -or [IO.File]::ReadAllText($file, [Text.Encoding]::UTF8).Trim() -cne $folder) { [void] $Failures.Add("the program's data in $folder was touched") }
    }
    $now = Get-ProgramCredentials
    if ($ProgramStarted) {
        # The program may add its own; none it had may be gone.
        $has = @($now -split "`n" | Where-Object { $_ })
        foreach ($name in @($script:credentials -split "`n" | Where-Object { $_ })) {
            if ($has -cnotcontains $name) { [void] $Failures.Add("the program's entry '$name' in Windows Credential Manager is gone") }
        }
        $script:credentials = $now
    } elseif ($now -cne $script:credentials) {
        [void] $Failures.Add("the program's entries in Windows Credential Manager changed (were: $($script:credentials -replace "`n", ', '); are: $($now -replace "`n", ', '))")
    }
}

function Get-Version([string] $File) {
    # (Nothing where the file is not there, as after a killed setup it may not be.)
    if (-not (Test-Path -LiteralPath $File -PathType Leaf)) { return $null }
    return ([string] [Diagnostics.FileVersionInfo]::GetVersionInfo($File).ProductVersion).Trim()
}
function Get-Log([string] $Name) { return Join-Path $Out "$Name.log" }
function Get-ResultLines([string] $Name) {
    <# The contract's result lines in a setup's log (qKVM-RESULT: ...), each without the time Inno Setup puts before it. #>
    return @((Read-Log $Name) -split "`r?`n" | Where-Object { $_.Contains('qKVM-RESULT: ') } | ForEach-Object { $_.Substring($_.IndexOf('qKVM-RESULT: ')).TrimEnd() })
}
function Read-Log([string] $Name) { $file = Get-Log $Name; if (Test-Path -LiteralPath $file) { return Get-Content -LiteralPath $file -Raw } else { return '' } }
# What a setup's log says where it started the program with the one argument --after-update: an update's own start, and
# a silent install's that closed it, once its copy is judged (StartTheCopy, 0.2.0: never an entry of [Run], which Inno
# Setup would run before the copy is flushed and marked finished); what it had closed and then changed nothing for
# (StartAgain); and the last page's "Start qKVM Desktop", Inno Setup's own postinstall entry.
$StartedLine = '(?m)^[^\r\n]*(Started qKVM Desktop from its folder with --after-update, its copy judged|Started qKVM Desktop again, which this setup had closed)|Filename: [^\r\n]*\\qkvm-desktop\.exe[ \t]*\r?\n[^\r\n]*Parameters: --after-update[ \t]*\r?\n'

function Get-NotKept($Earlier, $Now) {
    <# The files of the earlier version (Get-Installed's, before a copy) that are neither there as they were nor set aside
    beside it as they were (name.old-update, 0.2.0): what a setup ended halfway would have lost of it. #>
    $lost = New-Object System.Collections.ArrayList
    foreach ($path in $Earlier.Files.Keys) {
        $was = $Earlier.Files[$path].sha256
        $asIs = $Now.Files.Contains($path) -and $Now.Files[$path].sha256 -eq $was
        $aside = $Now.Files.Contains("$path$AsideEnding") -and $Now.Files["$path$AsideEnding"].sha256 -eq $was
        if (-not $asIs -and -not $aside) { [void] $lost.Add($path) }
    }
    return @($lost)
}

function Test-Earlier($Earlier, [System.Collections.ArrayList] $Failures) {
    <# The program's folder exactly as the earlier version had it ($Earlier, Get-Installed's): every file byte for byte,
    and nothing more (nothing set aside, no marker of an update, nothing a copy made), its Settings > Apps entry as it
    was (0.2.0's staged install, put back where the copy did not complete). #>
    $now = Get-Installed
    foreach ($path in $Earlier.Files.Keys) {
        if (-not $now.Files.Contains($path)) { [void] $Failures.Add("$path of the earlier version is gone") }
        elseif ($now.Files[$path].sha256 -ne $Earlier.Files[$path].sha256) { [void] $Failures.Add("$path is not the earlier version's") }
    }
    foreach ($path in $now.Files.Keys) {
        if (-not $Earlier.Files.Contains($path)) { [void] $Failures.Add("$path is in the program's folder, and was not before") }
    }
    if (($Earlier.Entry | ConvertTo-Json) -ne ($now.Entry | ConvertTo-Json)) { [void] $Failures.Add('the Settings > Apps entry is not as it was') }
}

function Test-AnyUpdateCheckAt([string] $Path) {
    <# Whether the file at $Path is an update check of qKVM Setup's, of either kind: a Windows program of at most 4 MiB,
    where the program is some 13 MB, and not a file of either version's package. Where one must not be (the new program
    in its place), any kind of it fails. #>
    $file = Get-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
    if (-not $file -or $file.PSIsContainer -or $file.Length -gt 4MB -or $file.Length -lt 2) { return $false }
    $head = New-Object byte[] 2
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    try { [void] $stream.Read($head, 0, 2) } finally { $stream.Dispose() }
    return ($head[0] -eq 0x4D -and $head[1] -eq 0x5A)
}

function Test-UpdateCheckAt([string] $Path) {
    <# Whether the file at $Path is qKVM Setup's update check, which the setup puts at the program's name while the earlier
    version is set aside (0.2.0, M3), and the one of this PC's kind, which the setup unpacks of the two it carries. #>
    return ((Test-AnyUpdateCheckAt $Path) -and (Get-ImageKind $Path) -ceq $PcKind)
}

function Get-AtProgramName {
    <# What is at the program's name, as a failure says it: its kind of Windows image and its size; 'nothing' where none. #>
    $file = Get-Item -LiteralPath $Program -Force -ErrorAction SilentlyContinue
    if (-not $file) { return 'nothing' }
    $imageKind = Get-ImageKind $Program
    return "$(if ($imageKind) { "a program of $imageKind" } else { 'no Windows image' }), $($file.Length) bytes"
}

function Test-PutBackFirst([string] $Said) {
    <# Whether a setup's log says it put back the earlier version that an update which did not finish had set aside,
    before it set it aside again for its own copy (0.2.0). #>
    $back = [regex]::Match($Said, 'Put the earlier version back, which an update that did not finish had set aside \(\d+ files\)\.')
    $aside = [regex]::Match($Said, 'Set the earlier version aside for the copy \(\d+ files\)\.')
    return ($back.Success -and $aside.Success -and $back.Index -lt $aside.Index)
}

function Read-OtherKindPackage([string] $Zip) {
    <# The package in the ZIP of a published version (tools/package.ps1's: one folder, its package-manifest.json and its
    files): its folder's name, version, kind by its manifest and by its program's PE header, and its files as the
    manifest has them; what is wrong with it ('' where nothing). Read in place, nothing unpacked. #>
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $read = [ordered]@{ zip = Split-Path -Leaf $Zip; sha256 = Get-Sha256 $Zip; top = ''; version = ''; kind = ''; programKind = ''; files = @(); wrong = '' }
    $archive = [IO.Compression.ZipFile]::OpenRead($Zip)
    try {
        $tops = @($archive.Entries | ForEach-Object { ([string] $_.FullName -split '/')[0] } | Sort-Object -Unique)
        if ($tops.Count -ne 1) { $read.wrong = 'it does not hold one folder'; return [pscustomobject] $read }
        $read.top = [string] $tops[0]
        $entry = $archive.GetEntry("$($read.top)/package-manifest.json")
        if (-not $entry) { $read.wrong = 'it has no package-manifest.json'; return [pscustomobject] $read }
        $reader = New-Object IO.StreamReader($entry.Open())
        try { $manifest = $reader.ReadToEnd() | ConvertFrom-Json } finally { $reader.Dispose() }
        $read.version = [string] $manifest.version
        $read.kind = if ($manifest.PSObject.Properties['architecture']) { [string] $manifest.architecture } else { '' }
        $read.files = @($manifest.files | ForEach-Object { [pscustomobject] @{ path = [string] $_.path; sha256 = ([string] $_.sha256).ToLowerInvariant() } })
        # (Each a plain name below the program's folder, as a package of ours names its files: never rooted, never a
        # drive, never . or .., so that the plant writes nothing outside that folder.)
        $odd = @($read.files | Where-Object { $_.path -cnotmatch '\A[A-Za-z0-9_+ -][A-Za-z0-9_.+ -]*(/[A-Za-z0-9_+ -][A-Za-z0-9_.+ -]*)*\z' -or ('/' + $_.path + '/').Contains('/../') })
        if ($odd.Count) { $read.wrong = "its manifest names what is not a plain name below the program's folder: $($odd[0].path)"; return [pscustomobject] $read }
        $entry = $archive.GetEntry("$($read.top)/qkvm-desktop.exe")
        if (-not $entry) { $read.wrong = 'it has no qkvm-desktop.exe'; return [pscustomobject] $read }
        $head = New-Object byte[] 4096
        $stream = $entry.Open()
        try { $count = Read-Head $stream $head } finally { $stream.Dispose() }
        $read.programKind = Get-HeadKind $head $count
    } finally { $archive.Dispose() }
    return [pscustomobject] $read
}

function Get-EntryValue([string] $Name) {
    <# One value of this user's Settings > Apps entry of qKVM Desktop; $null where it has none. #>
    $values = Get-ItemProperty -LiteralPath $UserKey -ErrorAction SilentlyContinue
    if (-not $values -or -not $values.PSObject.Properties[$Name]) { return $null }
    return $values.$Name
}

# --- 00 preflight
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$os = [Environment]::OSVersion.Version
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$admin = (New-Object Security.Principal.WindowsPrincipal $identity).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$adminGroup = @($identity.Groups | Where-Object { $_.Value -eq 'S-1-5-32-544' }).Count -gt 0
# (An administrator of the PC unelevated has the group for denying alone, which Groups leaves out: said, not refused,
# as Windows treats such a token as a standard user's. The head of this test.)
$deniedAdministrators = @($identity.Claims | Where-Object { $_.Type -eq [Security.Claims.ClaimTypes]::DenyOnlySid -and $_.Value -eq 'S-1-5-32-544' }).Count -gt 0
$newVersion = Get-Version $Setup
$oldVersion = Get-Version $OlderSetup
if ($os.Major -ne 10 -or $os.Build -lt 19045) { [void] $failures.Add("Windows $os is older than Windows 10 22H2") }
if (-not [Environment]::Is64BitOperatingSystem) { [void] $failures.Add('Windows is not 64-bit') }
if (-not $PcKind) { [void] $failures.Add("Windows runs on a machine of neither kind the setup installs for, x64 or Arm64 ($('0x{0:X4}' -f $NativeMachine))") }
if ($admin -or $adminGroup) { [void] $failures.Add('this runs as an administrator: run it as a standard user') }
# (The setup installs on Windows on Arm from Windows 11 alone, x64compatible: Windows 10 on Arm runs no x64 program.)
if ($PcKind -eq 'arm64' -and $os.Build -lt 22000) { [void] $failures.Add("Windows $os on Arm is older than Windows 11: the setup installs on Windows 11 on Arm alone") }
# (On Windows on Arm the older setup must install there: the published 0.1.0 to 0.1.2's refuse Windows on Arm, x64os. A
# throwaway older build of the setup's own commit, 0.1.99 say, carries both kinds: the head of this test.)
if ($PcKind -eq 'arm64' -and $oldVersion -in '0.1.0', '0.1.1', '0.1.2') {
    [void] $failures.Add("the older setup ($oldVersion) is a published one that refuses Windows on Arm: give one that carries both kinds, a throwaway older build of the setup's own commit (the head of this test; an x64 install of 0.1.x is planted from -OtherKindZip, steps 54 and 55)")
}
# (The other kind's earlier version for steps 54 and 55: a published version's ZIP of the other kind, older than -Setup.)
$otherPackage = $null
if ($OtherKindZip) {
    $OtherKindZip = (Resolve-Path -LiteralPath $OtherKindZip).Path
    $otherPackage = Read-OtherKindPackage $OtherKindZip
    if ($otherPackage.wrong) { [void] $failures.Add("-OtherKindZip is not a package's ZIP: $($otherPackage.wrong)") }
    elseif ($otherPackage.kind -cne $OtherPcKind -or $otherPackage.programKind -cne $OtherPcKind) { [void] $failures.Add("-OtherKindZip is a package for $($otherPackage.kind) (its program $($otherPackage.programKind)), not for the other kind ($OtherPcKind)") }
    elseif (-not ($otherPackage.version -match '\A\d+\.\d+\.\d+\z' -and [version] $otherPackage.version -lt [version] $newVersion)) { [void] $failures.Add("-OtherKindZip is version $($otherPackage.version), not one before the setup ($newVersion)") }
}
if ((Test-Path -LiteralPath $UserKey) -or (Test-Path -LiteralPath $App)) { [void] $failures.Add('qKVM Desktop is installed already: start from a clean machine') }
if (-not ([version] $oldVersion -lt [version] $newVersion)) { [void] $failures.Add("the older setup ($oldVersion) is not older than the setup ($newVersion)") }
# (Nothing installed, and so nothing of the program's own may be there either: a profile left, or passwords it saved,
# would be opened by the steps below, which start the program, and it would reach what they hold; review T5. Before
# anything is written here.)
if (-not (Test-Path -LiteralPath $UserKey) -and -not (Test-Path -LiteralPath $App)) {
    $profileLeft = @($DataFolders | Where-Object { (Test-Path -LiteralPath $_) -and @(Get-ChildItem -LiteralPath $_ -Force).Count })
    if ($profileLeft.Count -or (Get-ProgramCredentials)) {
        throw "qKVM Desktop is not installed, but this user has its profile ($($profileLeft -join ', ')) or passwords it saved (QKVM Desktop: in Windows Credential Manager): start from a clean machine."
    }
}
$seen00 = [ordered]@{
    windows = "$os"; kind = $PcKind; nativeMachine = ('0x{0:X4}' -f $NativeMachine); powershellRunsAs = (Get-ProcessKind $PID); hypervisor = $Hypervisor
    user = $identity.Name; administrator = ($admin -or $adminGroup)
    account = $(if ($deniedAdministrators) { 'an administrator of this PC, unelevated (UAC''s filtered token)' } else { 'a standard user' })
    otherKindPackage = $(if ($otherPackage) { [ordered]@{ zip = $otherPackage.zip; sha256 = $otherPackage.sha256; version = $otherPackage.version; kind = $otherPackage.kind
            programKind = $otherPackage.programKind; files = @($otherPackage.files).Count; wrong = $otherPackage.wrong } } else { $null })
    setup = [ordered]@{ file = Split-Path -Leaf $Setup; version = $newVersion; sha256 = Get-Sha256 $Setup; signature = [string] (Get-AuthenticodeSignature -LiteralPath $Setup).Status }
    olderSetup = [ordered]@{ file = Split-Path -Leaf $OlderSetup; version = $oldVersion; sha256 = Get-Sha256 $OlderSetup; signature = [string] (Get-AuthenticodeSignature -LiteralPath $OlderSetup).Status } }
# (Refused before anything is written into the account's data folders: a PC that is refused keeps them as they are.)
if ($failures.Count) {
    Write-Step '00' 'preflight' $began $failures $seen00
    throw 'The machine is not one this test can run on; see 00-preflight.json.'
}
foreach ($folder in $DataFolders) {
    if (-not (Test-Path -LiteralPath $folder)) { New-Item -ItemType Directory -Path $folder | Out-Null }
    # (Its folder's own name, written as UTF-8 without a mark, so that a name with signs outside ASCII stays itself:
    # ASCII wrote "?" for a letter with a mark on it, and every step then said the data was touched; review T2.)
    [IO.File]::WriteAllText((Join-Path $folder $Sentinel), $folder, (New-Object Text.UTF8Encoding $false))
}
$script:credentials = Get-ProgramCredentials
Write-Step '00' 'preflight' $began $failures $seen00

# --- 01 first install, of the older setup
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$run = Invoke-Setup $OlderSetup "$SilentArguments /LOG=`"$(Get-Log '01-first-install')`""
if ($run.Consent) { [void] $failures.Add('Windows asked for an administrator') }
if ($run.Exit -ne 0) { [void] $failures.Add("the setup ended with $($run.Exit)") }
$state = Test-Installed $oldVersion $failures
$log = Read-Log '01-first-install'
if ($log -match 'Administrative install mode: Yes') { [void] $failures.Add('the setup ran in administrative install mode') }
Write-Step '01' 'first-install' $began $failures ([ordered]@{ run = $run; entry = $state.Entry; files = $state.Files.Count
    nonAdministrativeMode = ($log -match 'Administrative install mode: No') })

# --- 02 the program starts
function Test-Starts([string] $Number) {
    $began = [DateTime]::UtcNow
    $failures = New-Object System.Collections.ArrayList
    $started = Start-Program
    if (-not $started.Up) { [void] $failures.Add('the program did not start (its mutex did not appear within 30 s)') }
    Start-Sleep -Seconds 5
    if (-not (Test-ProgramRunning)) { [void] $failures.Add('the program did not stay up for 5 s') }
    $version = Get-Version $Program
    # (Of this PC's kind: on Windows on Arm the ARM64 program, never x64 under emulation.)
    $runsAs = @(Test-RunsAsKind $PcKind $failures)
    $how = Stop-Program
    Test-Data $failures -ProgramStarted
    Write-Step $Number 'program-starts' $began $failures ([ordered]@{ version = $version; runsAs = $runsAs; ended = $how
        programCredentials = @($script:credentials -split "`n" | Where-Object { $_ }) })
}
Test-Starts '02'

# --- 03 a killed upgrade: what is left of the earlier version, and whether it still starts
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$log = Get-Log '03-killed-upgrade'
$earlier = Get-Installed
# (Killed once Qt6Core.dll is written: the log names the file after it, or after 60 s.)
$run = Invoke-Setup $Setup "$SilentArguments /LOG=`"$log`"" 120 {
    param($process, $gone)
    $said = if (Test-Path -LiteralPath $log) { (Read-Shared $log) } else { '' }
    $at = $said.IndexOf('\Qt6Core.dll')
    return ($at -ge 0 -and $said.IndexOf('Dest filename:', $at) -ge 0) -or $gone -gt 60
}
if ($run.Exit -ne 'killed') { [void] $failures.Add("the setup was not killed while it installed (it ended with $($run.Exit))") }
# (What is still running as the kill returns (Kill does not wait for a process to finish ending), from Inno Setup's
# temporary folders or the program's folder, named in the step's JSON, and waited for to end, up to 10 s: so that a file
# found held as the folder is read has its holder named. A test-only record.)
$left03 = @(Get-SetupProcessesLeft)
if ($left03.Count) { [void] (Wait-Until { @(Get-SetupProcessesLeft).Count -eq 0 } 10) }
$stillLeft03 = @(Get-SetupProcessesLeft)
Start-Sleep -Seconds 2
$after = Get-Installed
# What a killed setup leaves: file by file against the earlier version's.
$kept = New-Object System.Collections.ArrayList
$replaced = New-Object System.Collections.ArrayList
$missing = New-Object System.Collections.ArrayList
$added = New-Object System.Collections.ArrayList
foreach ($path in $earlier.Files.Keys) {
    if (-not $after.Files.Contains($path)) { [void] $missing.Add($path) }
    elseif ($after.Files[$path].sha256 -eq $earlier.Files[$path].sha256) { [void] $kept.Add($path) }
    else { [void] $replaced.Add($path) }
}
foreach ($path in $after.Files.Keys) { if (-not $earlier.Files.Contains($path)) { [void] $added.Add($path) } }
# (Set aside for the copy in every mode since 0.1.2, review L3: a setup ended from outside runs no code of its own, so
# the earlier program stays aside until the next setup puts it back first, step 04. Since 0.2.0 qKVM Setup's update
# check is in its place, M3: a start there says that an update did not finish.)
$programLeft = Test-UpdateCheckAt $Program
$asideLeft = Test-Path -LiteralPath "$Program.old-update"
$atName03 = Get-AtProgramName
if (-not $programLeft -or -not $asideLeft) { [void] $failures.Add("the earlier program was not set aside for the copy, with the update check of this PC's kind ($PcKind) in its place (at its name: $atName03)") }
# (And, 0.2.0, the whole earlier version: each of its files there byte for byte, as itself or set aside beside it, with
# the marker that says an update did not finish.)
$notKept = @(Get-NotKept $earlier $after)
if ($notKept.Count) { [void] $failures.Add("files of the earlier version are neither there nor set aside as they were: $($notKept -join ', ')") }
if (-not (Test-Path -LiteralPath $UpdateMarker)) { [void] $failures.Add('there is no marker of an unfinished update beside what the copy set aside') }
$how = @()
$entryVersion = if ($after.Entry) { $after.Entry.DisplayVersion } else { $null }
Write-Step '03' 'killed-upgrade' $began $failures ([ordered]@{ run = $run; entryVersion = $entryVersion; programVersion = Get-Version $Program
    programSetAside = $asideLeft; updateCheckInItsPlace = $programLeft; atProgramName = $atName03; markerLeft = (Test-Path -LiteralPath $UpdateMarker); notKept = $notKept
    stillTheEarlierVersions = $kept.Count; replaced = @($replaced); gone = @($missing); added = @($added); ended = $how
    runningAsTheKillReturned = @($left03); stillRunningAfter10s = @($stillLeft03)
    leftInTemp = @(Get-ChildItem -LiteralPath $env:TEMP -Directory -Filter 'is-*.tmp' | ForEach-Object Name) })

# --- 04 the upgrade
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
# (Files of no version of ours, a person's or another program's: in the two folders Qt loads whole and beside them.
# 0.2.0 never walks a folder, plugins and qml among them: each stays, M4.)
$staleFile = Join-Path $App 'plugins\imageformats\qkvm-vm-stale.dll'
$staleFolder = Join-Path $App 'qml\QkvmVmStale'
$outside = Join-Path $App 'qkvm-vm-outside.txt'
foreach ($file in $staleFile, (Join-Path $staleFolder 'qmldir'), $outside) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $file) | Out-Null
    Set-Content -LiteralPath $file -Value 'put here by test_setup_vm.ps1' -Encoding ASCII
}
$run = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '04-upgrade')`""
if ($run.Consent) { [void] $failures.Add('Windows asked for an administrator') }
if ($run.Exit -ne 0) { [void] $failures.Add("the setup ended with $($run.Exit)") }
if (-not (Test-Path -LiteralPath $staleFile)) { [void] $failures.Add('a file in plugins that no version of ours installed was removed') }
if (-not (Test-Path -LiteralPath (Join-Path $staleFolder 'qmldir'))) { [void] $failures.Add('a folder in qml that no version of ours installed was removed') }
if (-not (Test-Path -LiteralPath $outside)) { [void] $failures.Add('a file beside the program that no version of ours installed was removed') }
foreach ($file in $staleFile, $staleFolder, $outside) { Remove-Item -LiteralPath $file -Recurse -Force -ErrorAction SilentlyContinue }
$state = Test-Installed $newVersion $failures
if ((Get-Version $Program) -ne $newVersion) { [void] $failures.Add("the program is version $(Get-Version $Program), not $newVersion") }
# (The earlier version that 03's killed setup left aside, put back first, then set aside for this copy, outside update
# mode as in it: review L3, and 0.2.0's staged install.)
$said = Read-Log '04-upgrade'
if (-not (Test-PutBackFirst $said)) {
    [void] $failures.Add('the setup did not put the earlier version back first, or did not set it aside for its copy')
}
# (The earlier version found in its own folder, of its kind by its program's PE header, put back first: this PC's.)
if ($said -notmatch [regex]::Escape("qKVM Desktop $oldVersion ($PcKind) was installed in this folder")) {
    [void] $failures.Add("the setup's log does not say qKVM Desktop $oldVersion ($PcKind) was installed in this folder")
}
# (0.2.0, L13: the copy flushed to the disk before its marker is made that of a finishing update and the earlier
# version goes.)
$onDisk = $said.IndexOf('The copy is on the disk.')
$finished = $said.IndexOf('Finished an update whose copy was whole')
if ($onDisk -lt 0 -or $finished -lt 0 -or $onDisk -gt $finished) { [void] $failures.Add('the setup did not say the copy was on the disk before it removed the earlier version') }
Test-Data $failures
Write-Step '04' 'upgrade' $began $failures ([ordered]@{ run = $run; entry = $state.Entry; files = $state.Files.Count })

# --- 05 the older setup over the newer version: refused, nothing changed
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$before = Get-Installed
$run = Invoke-Setup $OlderSetup "$SilentArguments /LOG=`"$(Get-Log '05-downgrade-refused')`"" 120
if ($run.Exit -eq 0) { [void] $failures.Add('the older setup ended with 0 over the newer version') }
$after = Get-Installed
if (($before.Files | ConvertTo-Json -Depth 4) -ne ($after.Files | ConvertTo-Json -Depth 4) -or ($before.Entry | ConvertTo-Json) -ne ($after.Entry | ConvertTo-Json)) {
    [void] $failures.Add('the older setup changed something')
}
Write-Step '05' 'downgrade-refused' $began $failures ([ordered]@{ run = $run })

Test-Starts '06'

# --- 07 the update mode of the contract: the program ends within 10 s, the setup waits, installs and starts it again
$contract = { param([string] $log) "/SILENT /SUPPRESSMSGBOXES /NOCANCEL /NORESTART /CURRENTUSER /UPDATE=1 /DIR=`"$App`" /LOG=`"$log`"" }
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$started = Start-Program
if (-not $started.Up) { [void] $failures.Add('the program did not start before the update') }
$firstId = $started.Process.Id
$endedAt = $null
$tookEarly = $false
$setupSeen = $false
$run = Invoke-Setup $Setup (& $contract (Get-Log '07-update-mode')) 180 {
    param($process, $gone)
    # (As the program does: it starts the setup, then closes and exits within 10 s; here after 3 s.)
    if (-not $script:endedAt -and $gone -gt 3) { Stop-Process -Id $firstId -Force -ErrorAction SilentlyContinue; $script:endedAt = $gone; return $false }
    # (Once it has ended, nothing may take the program's mutex while the setup still holds its own.)
    if ($script:endedAt -and (Test-SetupRunning)) { if (Test-ProgramRunning) { $script:tookEarly = $true } else { $script:setupSeen = $true } }
    return $false
}
if ($run.Consent) { [void] $failures.Add('Windows asked for an administrator') }
if ($run.Exit -ne 0) { [void] $failures.Add("the setup ended with $($run.Exit)") }
$again = Wait-Until { Test-ProgramRunning } 30
$copies = @(Get-ProgramProcesses | Where-Object { $_.ProcessId -ne $firstId })
if (-not $again -or -not $copies.Count) { [void] $failures.Add('the setup did not start the program again from its folder') }
# (Started by the setup with the one argument --after-update, as the setup's log says of its [Run] entry: the copy it
# started may already have started itself again through the shell and ended. The command lines seen are kept.)
$commandLines = @(Get-CimInstance Win32_Process -Filter "Name = 'qkvm-desktop.exe'" | Where-Object { $_.ProcessId -ne $firstId } | ForEach-Object { [string] $_.CommandLine })
# (Started only once the copy was marked finished and what it set aside was gone: before that, the program would hold its
# own image against the flush, the copy would never count and the update would be left unfinished, as the rehearsal on
# Windows on Arm found it on 2026-11-19. Nothing of an unfinished update is left: Test-Installed below holds the folder
# to exactly the package's files, the marker and the uninstaller.)
if ((Read-Log '07-update-mode') -notmatch '(?s)Finished an update whose copy was whole[^\r\n]*\r?\n.*Started qKVM Desktop from its folder with --after-update, its copy judged') {
    [void] $failures.Add('the setup''s log does not say it started the program with the one argument --after-update, once its copy was whole and finished')
}
if ((Read-Log '07-update-mode') -match 'does not count|could not be flushed') { [void] $failures.Add('the update''s copy did not count') }
foreach ($left in @(Get-ChildItem -LiteralPath $App -Recurse -Force -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -like '*.old-update' -or $_.Name -eq 'qkvm-desktop.update-unfinished' })) {
    [void] $failures.Add("the update left $($left.Name) behind")
}
$state = Test-Installed $newVersion $failures
$log = Read-Log '07-update-mode'
if ($log -notmatch 'had ended after \d+ ms') { [void] $failures.Add('the setup''s log does not say it waited for the program') }
if (@(Get-ResultLines '07-update-mode').Count) { [void] $failures.Add('the log of a setup that installed has a result line of the contract') }
if (-not $script:setupSeen) { [void] $failures.Add('the setup''s own mutex was not seen while it ran') }
if ($script:tookEarly) { [void] $failures.Add('the program took its mutex while the setup still held its own') }
$runsAs07 = @(Test-RunsAsKind $PcKind $failures 'the program the update started again')
$how = Stop-Program
Test-Data $failures
Write-Step '07' 'update-mode' $began $failures ([ordered]@{ run = $run; programEndedAfterSeconds = $script:endedAt
    startedAgain = @($copies | ForEach-Object { $_.ExecutablePath }); commandLines = $commandLines; runsAs = $runsAs07; ended = $how })

# --- 08 the update mode with the program still running: exit code 105, nothing changed
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$started = Start-Program
$before = Get-Installed
$run = Invoke-Setup $Setup (& $contract (Get-Log '08-update-still-running')) 180
if ($run.Exit -ne 105) { [void] $failures.Add("the setup ended with $($run.Exit), not 105") }
if ($run.Seconds -lt 55 -or $run.Seconds -gt 120) { [void] $failures.Add("the setup ended after $($run.Seconds) s, not after its 60 s") }
$after = Get-Installed
if (($before.Files | ConvertTo-Json -Depth 4) -ne ($after.Files | ConvertTo-Json -Depth 4) -or ($before.Entry | ConvertTo-Json) -ne ($after.Entry | ConvertTo-Json)) {
    [void] $failures.Add('the setup changed something')
}
if (-not (Get-Process -Id $started.Process.Id -ErrorAction SilentlyContinue)) { [void] $failures.Add('the program is no longer running') }
# (The program reads this one line: it cannot see the exit code. It must be in the log though the setup ended itself.)
$resultLines = @(Get-ResultLines '08-update-still-running')
if ($resultLines.Count -ne 1 -or $resultLines[0] -cne 'qKVM-RESULT: 105 the program was still running after 60 seconds; nothing was changed') {
    [void] $failures.Add("the setup's log does not have the one result line of 105 ($($resultLines -join ' | '))")
}
$how = Stop-Program
Write-Step '08' 'update-still-running' $began $failures ([ordered]@{ run = $run; ended = $how; resultLines = $resultLines })

# --- 09 the update mode with a file of the program held open: refused before anything is changed
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$before = Get-Installed
$held = [IO.File]::Open((Join-Path $App 'Qt6Core.dll'), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
try {
    $run = Invoke-Setup $Setup (& $contract (Get-Log '09-update-file-in-use')) 180
} finally {
    $held.Dispose()
}
if ($run.Exit -ne 7) { [void] $failures.Add("the setup ended with $($run.Exit), not 7 (its Preparing to Install refusal)") }
$after = Get-Installed
if (($before.Files | ConvertTo-Json -Depth 4) -ne ($after.Files | ConvertTo-Json -Depth 4) -or ($before.Entry | ConvertTo-Json) -ne ($after.Entry | ConvertTo-Json)) {
    [void] $failures.Add('the setup changed something')
}
$resultLines = @(Get-ResultLines '09-update-file-in-use')
if ($resultLines.Count -ne 1 -or -not $resultLines[0].StartsWith('qKVM-RESULT: 7 nothing was changed: a file of qKVM Desktop is in use or cannot be replaced (', [StringComparison]::Ordinal)) {
    [void] $failures.Add("the setup's log does not have the one result line of 7 for a file in use ($($resultLines -join ' | '))")
}
Write-Step '09' 'update-file-in-use' $began $failures ([ordered]@{ run = $run; resultLines = $resultLines })

# --- 10 the uninstaller while the program runs
$uninstaller = Join-Path $App 'unins000.exe'
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$started = Start-Program
$before = Get-Installed
$run = Invoke-Setup $uninstaller "$SilentArguments /LOG=`"$(Get-Log '10-uninstall-refused')`"" 120
if ($run.Exit -eq 0) { [void] $failures.Add('the uninstaller ended with 0 while the program ran') }
$after = Get-Installed
if (($before.Files | ConvertTo-Json -Depth 4) -ne ($after.Files | ConvertTo-Json -Depth 4) -or -not $after.Entry) { [void] $failures.Add('the uninstaller removed something while the program ran') }
$how = Stop-Program
Write-Step '10' 'uninstall-refused' $began $failures ([ordered]@{ run = $run; ended = $how })

# --- 11 the uninstall keeps the data
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$run = Invoke-Setup $uninstaller "$SilentArguments /LOG=`"$(Get-Log '11-uninstall')`"" 180
if ($run.Consent) { [void] $failures.Add('Windows asked for an administrator') }
if ($run.Exit -ne 0) { [void] $failures.Add("the uninstaller ended with $($run.Exit)") }
# (Its second part, from a copy of it in Temp, may still be removing its own files for a moment.)
[void] (Wait-Until { -not (Test-Path -LiteralPath $App) } 30)
$after = Get-Installed
if ($after.Files.Count) { [void] $failures.Add("$($after.Files.Count) file(s) are left in the program's folder: $(@($after.Files.Keys) -join ', ')") }
if ($after.Entry -or $after.Machine.Count) { [void] $failures.Add('the Settings > Apps entry is still there') }
if ($after.StartMenu -or $after.Desktop) { [void] $failures.Add('a Start menu or desktop entry is still there') }
Test-Data $failures
Write-Step '11' 'uninstall' $began $failures ([ordered]@{ run = $run; folderLeft = (Test-Path -LiteralPath $App) })

# --- 12 the reinstall; then a start of the program while a setup's mutex is held
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$log = Get-Log '12-reinstall'
$run = Invoke-Setup $Setup "$SilentArguments /LOG=`"$log`""
if ($run.Consent) { [void] $failures.Add('Windows asked for an administrator') }
if ($run.Exit -ne 0) { [void] $failures.Add("the setup ended with $($run.Exit)") }
$state = Test-Installed $newVersion $failures
# The program, started without --after-update while a setup holds its mutex, must end at once without taking its
# own. The mutex is held here, under the setup's own name in this session, as a setup holds it while it copies: a
# reinstall's log says it succeeded a few milliseconds before it ends, too soon to start the program within it
# (seen on Windows 10 and 11, 2026-11-12). Starts during a real copy: steps 07 and 14. And with the event a setup makes
# to say it is at work, named after that mutex and the number of a process that runs, this one's: a start takes a
# setup to be at work only so, never by the mutex alone (review R2, ui/win/setup_wait.h's setupRunsName).
$tookEarly = $false
$endedAtOnce = $false
$created = $false
$held = [Threading.Mutex]::new($true, $SetupMutex, [ref] $created)
$atWork = New-Object System.Threading.EventWaitHandle($false, [System.Threading.EventResetMode]::ManualReset, "$SetupMutex-$PID")
try {
    if (-not $created) { [void] $failures.Add("a setup's mutex was there already, held by another") }
    # (Within a minute, as a setup waits for such a start before it judges its copy (WaitForStartsToEnd): this first
    # start of the files the reinstall just wrote maps each of them, and Windows' scanner scans each first (2026-11-21: 2 to 8 s
    # for a whole copy on a test VM, more while it scans something else, some 60 ms once scanned); once in its main(),
    # the program ends at once. How long it took is kept.)
    $startedAt = Get-Clock
    $startedDuring = Start-Process -FilePath $Program -WorkingDirectory $App -PassThru
    [void] $startedDuring.Handle
    $until = $startedAt + 60
    while ((Get-Clock) -lt $until) {
        if (Test-ProgramRunning) { $tookEarly = $true }
        if ($startedDuring.HasExited) { $endedAtOnce = $true; break }
        Start-Sleep -Milliseconds 100
    }
    $endedAfterMs = [int] (((Get-Clock) - $startedAt) * 1000)
    # (Then the same files started again under the same mutex: scanned by now, the start ends at once in its main(),
    # within 2 s; a program that came to spend long before its look at the setup's mutex fails here, where the first
    # start's minute would hide it, and so would every setup's minute; review armp-1 L5.)
    $againEndedAtOnce = $false
    $againEndedAfterMs = $null
    $againDuring = $null
    if ($endedAtOnce) {
        $againAt = Get-Clock
        $againDuring = Start-Process -FilePath $Program -WorkingDirectory $App -PassThru
        [void] $againDuring.Handle
        $againUntil = $againAt + 30
        while ((Get-Clock) -lt $againUntil) {
            if (Test-ProgramRunning) { $tookEarly = $true }
            if ($againDuring.HasExited) { $againEndedAtOnce = $true; break }
            Start-Sleep -Milliseconds 50
        }
        $againEndedAfterMs = [int] (((Get-Clock) - $againAt) * 1000)
    }
} finally {
    if ($created) { $held.ReleaseMutex() }
    $held.Dispose()
    $atWork.Dispose()
}
if (-not $endedAtOnce) { [void] $failures.Add('the program started while a setup held its mutex did not end within 60 s') }
elseif ($startedDuring.ExitCode -ne 0) { [void] $failures.Add("the program started while a setup held its mutex ended with $($startedDuring.ExitCode), not 0") }
elseif (-not $againEndedAtOnce) { [void] $failures.Add('the program started again while a setup held its mutex, its files scanned by then, did not end within 30 s') }
elseif ($againEndedAfterMs -gt 2000) { [void] $failures.Add("the program started again while a setup held its mutex, its files scanned by then, ended after $againEndedAfterMs ms, not within 2 s") }
elseif ($againDuring.ExitCode -ne 0) { [void] $failures.Add("the program started again while a setup held its mutex ended with $($againDuring.ExitCode), not 0") }
if ($tookEarly) { [void] $failures.Add('the program started while a setup held its mutex took its own') }
$how = Stop-Program
Test-Data $failures
Write-Step '12' 'reinstall' $began $failures ([ordered]@{ run = $run; files = $state.Files.Count
    startedWhileSetupMutexHeld = [ordered]@{ endedAtOnce = $endedAtOnce; endedAfterMs = $endedAfterMs; againEndedAtOnce = $againEndedAtOnce
        againEndedAfterMs = $againEndedAfterMs; tookItsMutex = $tookEarly }; ended = $how })
Test-Starts '13'

# --- 14 starts of the program, again and again, while the setup copies its files
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$log = Get-Log '14-starts-during-copy'
$script:starts = 0
$script:startsRefused = 0
$script:lastStart = -1000.0
$script:tookEarly = $false
# (From the first file the setup writes until it says the copy is done: a start without an argument every half
# second, as a person might double-click it then. Each finds the update check in the program's place (the earlier
# program is set aside for the copy in every mode, review L3, and the update check put there since 0.2.0, M3), which
# ends at once and says nothing while the setup is at work, as step 15 sees in update mode.)
$script:startedDuring = New-Object System.Collections.ArrayList
$script:checkSeen = $false
$run = Invoke-Setup $Setup "$SilentArguments /LOG=`"$log`"" 300 {
    param($process, $gone)
    $said = if (Test-Path -LiteralPath $log) { (Read-Shared $log) } else { '' }
    # (None once the setup has named the program's own file, the last it writes: a start then may reach the program's
    # check of the setup's mutex only after the setup has ended, and run as the program does.)
    if ($said.Contains('Dest filename:') -and $said -notmatch '(?m)Dest filename: .*\\qkvm-desktop\.exe\s*$' -and
        ((Get-Clock) - $script:lastStart) -ge 0.5) {
        $script:lastStart = Get-Clock
        if (Test-UpdateCheckAt $Program) { $script:checkSeen = $true }
        try { $one = Start-Process -FilePath $Program -WorkingDirectory $App -PassThru; [void] $one.Handle; [void] $script:startedDuring.Add($one); $script:starts++ } catch { $script:startsRefused++ }
    }
    if ((Test-SetupRunning) -and (Test-ProgramRunning)) { $script:tookEarly = $true }
    return $false
}
if ($run.Consent) { [void] $failures.Add('Windows asked for an administrator') }
if ($run.Exit -ne 0) { [void] $failures.Add("the setup ended with $($run.Exit) with the program started during its copy") }
if ($script:starts + $script:startsRefused -eq 0) { [void] $failures.Add('the program was not once started while the setup copied its files') }
if (-not $script:checkSeen) { [void] $failures.Add('the update check was not in the program''s place during the copy: the earlier program was not set aside') }
# (Each start during the copy that found the update check ended at once, with 0, saying nothing.)
$notEnded = @($script:startedDuring | Where-Object { -not $_.WaitForExit(10000) })
$notZero = @($script:startedDuring | Where-Object { $_.HasExited -and $_.ExitCode -ne 0 })
if ($notEnded.Count -or $notZero.Count) { [void] $failures.Add("starts during the copy did not end at once with 0 ($($notEnded.Count) still running, $($notZero.Count) ended otherwise)") }
if ($script:tookEarly) { [void] $failures.Add('a start during the copy took the program''s mutex while the setup held its own') }
# (Every start while the setup holds its mutex ends at once. One that comes as the setup ends may find its mutex gone
# and run as the program does, holding its own: seen on Windows 10, 2026-11-12. That one is left, a single process
# holding the program's mutex; any other still running is a start that neither ended nor ran.)
$settled = Wait-Until { $left = @(Get-ProgramProcesses); $left.Count -eq 0 -or ($left.Count -eq 1 -and (Test-ProgramRunning)) } 15
$leftRunning = @(Get-ProgramProcesses).Count
$lateStart = $leftRunning -eq 1 -and (Test-ProgramRunning)
if (-not $settled) { [void] $failures.Add("a start during the copy is still running after the setup ended, without the program's mutex ($leftRunning process(es))") }
[void] (Stop-Program)
$state = Test-Installed $newVersion $failures
$started = Start-Program
if (-not $started.Up) { [void] $failures.Add('after the install, the new version does not start') }
$version = Get-Version $Program
if ($version -ne $newVersion) { [void] $failures.Add("the program is version $version, not $newVersion") }
$how = Stop-Program
Test-Data $failures
Write-Step '14' 'starts-during-copy' $began $failures ([ordered]@{ run = $run; starts = $script:starts; startsRefused = $script:startsRefused
    lateStartRanAsTheProgram = $lateStart; files = $state.Files.Count; version = $version; ended = $how })

# (Steps 15 and 16 start from the older version: uninstalled, then the older setup again, as a machine that has it.)
function Reset-ToOlder([string] $Number, [System.Collections.ArrayList] $Failures) {
    $uninstalled = Invoke-Setup (Join-Path $App 'unins000.exe') "$SilentArguments /LOG=`"$(Get-Log "$Number-uninstall-first")`"" 180
    [void] (Wait-Until { -not (Test-Path -LiteralPath $App) } 30)
    $installed = Invoke-Setup $OlderSetup "$SilentArguments /LOG=`"$(Get-Log "$Number-older-first")`""
    if ($uninstalled.Exit -ne 0 -or $installed.Exit -ne 0) {
        [void] $Failures.Add("the older version could not be installed again first (uninstall $($uninstalled.Exit), install $($installed.Exit))")
    }
    return Get-Installed
}

# (A start of the program held as Windows' scanner holds the first start of a copy just written: made suspended, so that
# its process holds the program's file mapped, as one does while the scanner looks at each new library it maps, and let
# go of when told; step 15. Its handles are closed as it is let go of: a handle kept would keep its number.)
if (-not ('QkvmVmHeldStart' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class QkvmVmHeldStart {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct StartupInfo {
        public int cb; public string lpReserved; public string lpDesktop; public string lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
        public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct ProcessInformation { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool CreateProcessW(string application, StringBuilder line, IntPtr processAttributes, IntPtr threadAttributes,
        bool inherit, uint flags, IntPtr environment, string directory, ref StartupInfo startup, out ProcessInformation information);
    [DllImport("kernel32.dll")] static extern uint ResumeThread(IntPtr thread);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll")] static extern bool TerminateProcess(IntPtr process, uint code);
    [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);
    static ProcessInformation held;
    // The program at path started suspended (CREATE_SUSPENDED), with no argument; its process's number, or 0.
    public static int Start(string path, string directory) {
        if (held.hProcess != IntPtr.Zero) return 0;
        StartupInfo startup = new StartupInfo();
        startup.cb = Marshal.SizeOf(typeof(StartupInfo));
        ProcessInformation information;
        if (!CreateProcessW(path, new StringBuilder("\"" + path + "\""), IntPtr.Zero, IntPtr.Zero, false, 0x4, IntPtr.Zero, directory, ref startup, out information)) return 0;
        held = information;
        return information.dwProcessId;
    }
    // Whether the start held is there still, not ended (by anyone).
    public static bool Running() {
        return held.hProcess != IntPtr.Zero && WaitForSingleObject(held.hProcess, 0) == 0x102;
    }
    // This test's own start ended, as such a start ends by itself in the end, and its handles closed.
    public static void End() {
        if (held.hProcess != IntPtr.Zero) TerminateProcess(held.hProcess, 1);
        if (held.hThread != IntPtr.Zero) CloseHandle(held.hThread);
        if (held.hProcess != IntPtr.Zero) CloseHandle(held.hProcess);
        held = new ProcessInformation();
    }
    // Let go of: it runs on (and ends by itself where a setup is at work), and its handles are closed.
    public static void Release() {
        if (held.hThread != IntPtr.Zero) { ResumeThread(held.hThread); CloseHandle(held.hThread); }
        if (held.hProcess != IntPtr.Zero) CloseHandle(held.hProcess);
        held = new ProcessInformation();
    }
}
'@
}

# --- 15 the contract's update across a change of the Qt libraries, with starts of the program during its copy
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$older = Reset-ToOlder '15' $failures
$olderQt = if ($older.Files.Contains('Qt6Core.dll')) { $older.Files['Qt6Core.dll'].sha256 } else { '' }
$started = Start-Program
$firstId = $started.Process.Id
$log = Get-Log '15-update-across-qt'
$aside = "$Program.old-update"
$script:ended15 = $null
$script:starts15 = 0
$script:refused15 = 0
$script:last15 = -1000.0
$script:absent15 = $false
$script:aside15 = $false
$script:took15 = $false
$script:held15 = 0
$script:copied15 = $null
$script:let15 = $false
try {
$run = Invoke-Setup $Setup (& $contract $log) 300 {
    param($process, $gone)
    if (-not $script:ended15 -and $gone -gt 3) { Stop-Process -Id $firstId -Force -ErrorAction SilentlyContinue; $script:ended15 = $gone; return $false }
    $said = if (Test-Path -LiteralPath $log) { (Read-Shared $log) } else { '' }
    # (The held start: made as soon as the new program's file is in, let go of 15 s after the copy's end, longer than
    # the flush's tries; the setup must wait for it to end before it judges the copy.)
    if (-not $script:held15) {
        $at = $said.IndexOf("Dest filename: $Program")
        if ($at -ge 0 -and $said.IndexOf('Successfully installed the file.', $at) -ge 0) { $script:held15 = [QkvmVmHeldStart]::Start($Program, $App) }
    } elseif (-not $script:let15) {
        if ($null -eq $script:copied15 -and $said.Contains('Installation process succeeded')) { $script:copied15 = Get-Clock }
        if ($null -ne $script:copied15 -and ((Get-Clock) - $script:copied15) -ge 15) { [QkvmVmHeldStart]::Release(); $script:let15 = $true }
    }
    if ($said.Contains('Dest filename:') -and -not $said.Contains('Installation process succeeded')) {
        if (Test-UpdateCheckAt $Program) { $script:absent15 = $true }
        if (Test-Path -LiteralPath $aside) { $script:aside15 = $true }
        if (((Get-Clock) - $script:last15) -ge 0.5) {
            $script:last15 = Get-Clock
            try { Start-Process -FilePath $Program -WorkingDirectory $App | Out-Null; $script:starts15++ } catch { $script:refused15++ }
        }
    }
    if ($script:ended15 -and (Test-SetupRunning) -and (Test-ProgramRunning)) { $script:took15 = $true }
    return $false
}
} finally {
    [QkvmVmHeldStart]::Release()
}
if ($run.Exit -ne 0) { [void] $failures.Add("the update ended with $($run.Exit)") }
if (-not $script:held15) { [void] $failures.Add('no start of the new program could be held as its file came in') }
elseif (-not $script:let15) { [void] $failures.Add('the setup did not wait for the held start of the new program: it ended within 15 s of its copy') }
if ((Read-Log '15-update-across-qt') -notmatch 'Waited \d+ ms for the starts of qKVM Desktop made during the copy to end') {
    [void] $failures.Add('the setup''s log does not say it waited for the starts made during the copy before it judged the copy')
}
$after = Get-Installed
$newQt = if ($after.Files.Contains('Qt6Core.dll')) { $after.Files['Qt6Core.dll'].sha256 } else { '' }
# (Where the two setups ship the same Qt, as 0.1.2 and 0.2.0 do, the update is not one across a change of Qt: the rest
# of the step is judged, and what was not seen is said. The head of this test says how to make the newer one's differ.)
$notSeen15 = ''
if (-not $olderQt -or -not $newQt) { [void] $failures.Add('Qt6Core.dll is not in the older install or the new one') }
elseif ($newQt -eq $olderQt) { $notSeen15 = 'an update across a change of Qt: the two setups ship the same Qt6Core.dll (the head of this test says how to make the newer one''s differ)' }
if (-not $script:aside15 -or -not $script:absent15) { [void] $failures.Add('the program''s file was not set aside for the copy, with the update check in its place') }
if (Test-Path -LiteralPath $aside) { [void] $failures.Add('the set-aside copy of the program is still there after the update') }
if ($script:took15) { [void] $failures.Add('a start during the copy took the program''s mutex while the setup held its own') }
if (-not (Wait-Until { Test-ProgramRunning } 30)) { [void] $failures.Add('the setup did not start the program again after the update') }
if ((Read-Log '15-update-across-qt') -notmatch $StartedLine) {
    [void] $failures.Add('the setup''s log does not say it started the program with the one argument --after-update')
}
$how = Stop-Program
$state = Test-Installed $newVersion $failures
Test-Data $failures
Write-Step '15' 'update-across-qt' $began $failures ([ordered]@{ run = $run; qtChanged = [bool] ($olderQt -and $newQt -and $newQt -ne $olderQt)
    startsDuringCopy = $script:starts15; startsFindingNoProgram = $script:refused15; heldStart = $script:held15; heldLetGoAfterTheCopy = $script:let15
    ended = $how; files = $state.Files.Count }) $notSeen15

# --- 16 the contract's update failing during its copy: the earlier program back in its place
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$older = Reset-ToOlder '16' $failures
$olderProgram = if ($older.Files.Contains('qkvm-desktop.exe')) { $older.Files['qkvm-desktop.exe'].sha256 } else { '' }
# (The name of the file the setup puts in last before the program, taken once the copy has begun: the setup's own
# check before the copy found the folder free, and its copy then fails on it, as on a file in use or a full disk. Since
# 0.2.0 the earlier file of that name is set aside by then, so the name is taken by a file of this test's own, held with
# no sharing, as a scanner of the PC may hold a file the copy has just made; and let go of as the setup rolls back, so
# that the earlier version can be put back whole.)
$manifest = Get-Content -LiteralPath (Join-Path $App 'package-manifest.json') -Raw | ConvertFrom-Json
$names = [string[]] @($manifest.files | ForEach-Object { [string] $_.path } | Where-Object { $_ -ne 'qkvm-desktop.exe' })
[Array]::Sort($names, [StringComparer]::Ordinal)
$heldName = $names[-1]
$started = Start-Program
$firstId = $started.Process.Id
$log = Get-Log '16-update-fails-during-copy'
$script:ended16 = $null
$script:held16 = $null
$script:heldAt16 = $false
try {
    $run = Invoke-Setup $Setup (& $contract $log) 300 {
        param($process, $gone)
        if (-not $script:ended16 -and $gone -gt 3) { Stop-Process -Id $firstId -Force -ErrorAction SilentlyContinue; $script:ended16 = $gone; return $false }
        $said = Read-Shared $log
        if (-not $script:heldAt16 -and $said.Contains('Dest filename:')) {
            try { $script:held16 = [IO.File]::Open((Join-Path $App ($heldName -replace '/', '\')), [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None); $script:heldAt16 = $true } catch { }
        }
        if ($script:held16 -and $said.Contains('Rolling back changes.')) { $script:held16.Dispose(); $script:held16 = $null }
        return $false
    }
} finally {
    if ($script:held16) { $script:held16.Dispose() }
}
if (-not $script:heldAt16) { [void] $failures.Add("$heldName could not be held during the copy") }
if ($run.Exit -eq 0) { [void] $failures.Add('the update did not fail with a file of the folder held during its copy') }
$programNow = if (Test-Path -LiteralPath $Program) { (Get-FileHash -LiteralPath $Program -Algorithm SHA256).Hash.ToLowerInvariant() } else { '' }
if (-not $programNow) { [void] $failures.Add('after the failed update there is no program') }
elseif ($programNow -ne $olderProgram) { [void] $failures.Add('after the failed update the program is not the earlier one') }
if (Test-Path -LiteralPath "$Program.old-update") { [void] $failures.Add('the set-aside copy of the program is still there after the failed update') }
# (0.2.0: the whole earlier version, byte for byte, and nothing set aside or marked left; and, the update having
# ended the program, the setup starts the earlier one again itself, L5.)
$startedAgain16 = (Read-Log '16-update-fails-during-copy') -match 'Started qKVM Desktop again' -and (Wait-Until { Test-ProgramRunning } 30)
if (-not $startedAgain16) { [void] $failures.Add('the update whose copy did not complete did not start the earlier program again') }
$how = Stop-Program
Test-Earlier $older $failures
$restarted = Start-Program
if (-not $restarted.Up) { [void] $failures.Add('after the failed update, the earlier program does not start') }
$how += @(Stop-Program)
$finish = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '16-finish')`""
if ($finish.Exit -ne 0) { [void] $failures.Add("the new setup did not install afterwards ($($finish.Exit))") }
Test-Data $failures
Write-Step '16' 'update-fails-during-copy' $began $failures ([ordered]@{ run = $run; held = $heldName; programIsTheEarlier = ($programNow -eq $olderProgram); startedAgain = $startedAgain16
    ended = $how; finish = $finish })

# --- 17 a setup ended halfway, then the uninstaller: the program's folder gone, nothing left in it
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$log = Get-Log '17-killed-then-uninstall'
# (Killed once its copy has reached licenses: what is being written then, in whatever folder, is left as Inno Setup's
# temporary file, which the uninstaller must take with the rest. Seen in the review of 2026-11-12: a cut below the
# top of the folder, and an uninstall with no whole setup between, had never been tried.)
$run = Invoke-Setup $Setup "$SilentArguments /LOG=`"$log`"" 120 {
    param($process, $gone)
    $said = Read-Shared $log
    $at = $said.IndexOf('\licenses\')
    return ($at -ge 0 -and $said.IndexOf('Dest filename:', $at) -ge 0) -or $gone -gt 60
}
if ($run.Exit -ne 'killed') { [void] $failures.Add("the setup was not killed while it installed (it ended with $($run.Exit))") }
Start-Sleep -Seconds 2
$leftovers = @(Get-ChildItem -LiteralPath $App -Recurse -File -Force -Filter 'is-*.tmp' -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName.Substring($App.Length + 1) })
$uninstall = Invoke-Setup (Join-Path $App 'unins000.exe') "$SilentArguments /LOG=`"$(Get-Log '17-uninstall')`"" 180
if ($uninstall.Exit -ne 0) { [void] $failures.Add("the uninstaller ended with $($uninstall.Exit)") }
[void] (Wait-Until { -not (Test-Path -LiteralPath $App) } 30)
if (Test-Path -LiteralPath $App) {
    $left = @(Get-ChildItem -LiteralPath $App -Recurse -Force -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName.Substring($App.Length + 1) })
    [void] $failures.Add("the program's folder is left after the uninstall, holding: $($left -join ', ')")
}
Test-Data $failures
Write-Step '17' 'killed-then-uninstall' $began $failures ([ordered]@{ run = $run; leftBeforeTheUninstall = $leftovers
    uninstall = $uninstall; folderLeft = (Test-Path -LiteralPath $App) })

# --- Steps 18 to 23 (0.1.2): a running copy closed for the person. What they need: a program held so that it does not
# answer (every thread of it suspended), its windows looked at, and the setup's own windows driven as a person would
# (UI Automation; a press from a thread of its own, as what it starts in the setup, a wait or a box, would otherwise
# hold this script).
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
Add-Type -ReferencedAssemblies UIAutomationClient, UIAutomationTypes, WindowsBase -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Automation;
public static class QkvmVm {
    [DllImport("ntdll.dll")] public static extern int NtSuspendProcess(IntPtr process);
    [DllImport("ntdll.dll")] public static extern int NtResumeProcess(IntPtr process);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string kind, string title);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr window);
    delegate bool EachWindow(IntPtr window, IntPtr data);
    [DllImport("user32.dll")] static extern bool EnumWindows(EachWindow each, IntPtr data);
    public static int VisibleWindowsOf(uint process) {
        int count = 0;
        EnumWindows(delegate (IntPtr window, IntPtr data) {
            uint owner;
            GetWindowThreadProcessId(window, out owner);
            if (owner == process && IsWindowVisible(window)) count++;
            return true;
        }, IntPtr.Zero);
        return count;
    }
    // A process's own top-level windows that are shown, from Windows' own list (EnumWindows): no window of any other
    // process is asked anything, a hung program's among them (review V1).
    public static IntPtr[] WindowsOf(uint process) {
        System.Collections.Generic.List<IntPtr> found = new System.Collections.Generic.List<IntPtr>();
        EnumWindows(delegate (IntPtr window, IntPtr data) {
            uint owner;
            GetWindowThreadProcessId(window, out owner);
            if (owner == process && IsWindowVisible(window)) found.Add(window);
            return true;
        }, IntPtr.Zero);
        return found.ToArray();
    }
    public static bool HasWindowOfKind(uint process, string kind) {
        for (IntPtr at = FindWindowEx(IntPtr.Zero, IntPtr.Zero, kind, null); at != IntPtr.Zero; at = FindWindowEx(IntPtr.Zero, at, kind, null)) {
            uint owner;
            GetWindowThreadProcessId(at, out owner);
            if (owner == process) return true;
        }
        return false;
    }
    [DllImport("kernel32.dll")] static extern IntPtr OpenThread(uint access, bool inherit, uint id);
    [DllImport("kernel32.dll")] static extern uint SuspendThread(IntPtr thread);
    [DllImport("kernel32.dll")] static extern uint ResumeThread(IntPtr thread);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr window, System.Text.StringBuilder text, int most);
    // The thread a program began with (its interface's, in qKVM Desktop) suspended alone, the rest of it left to run;
    // its handle, for ResumeAndLetGo (zero where it could not be).
    public static IntPtr SuspendFirstThread(int process) {
        System.Diagnostics.ProcessThread first = null;
        foreach (System.Diagnostics.ProcessThread one in System.Diagnostics.Process.GetProcessById(process).Threads) {
            try { if (first == null || one.StartTime < first.StartTime) first = one; } catch (Exception) { }
        }
        if (first == null) return IntPtr.Zero;
        IntPtr thread = OpenThread(0x0002, false, (uint) first.Id);
        if (thread != IntPtr.Zero && SuspendThread(thread) == 0xFFFFFFFF) { CloseHandle(thread); return IntPtr.Zero; }
        return thread;
    }
    public static void ResumeAndLetGo(IntPtr thread) {
        if (thread == IntPtr.Zero) return;
        ResumeThread(thread);
        CloseHandle(thread);
    }
    // The title of a process's window of this class ("" where it has none).
    public static string TitleOf(uint process, string kind) {
        for (IntPtr at = FindWindowEx(IntPtr.Zero, IntPtr.Zero, kind, null); at != IntPtr.Zero; at = FindWindowEx(IntPtr.Zero, at, kind, null)) {
            uint owner;
            GetWindowThreadProcessId(at, out owner);
            if (owner != process) continue;
            System.Text.StringBuilder text = new System.Text.StringBuilder(1024);
            GetWindowText(at, text, text.Capacity);
            return text.ToString();
        }
        return "";
    }
    public static void Press(AutomationElement button) {
        Thread pressing = new Thread(delegate () {
            try { ((InvokePattern) button.GetCurrentPattern(InvokePattern.Pattern)).Invoke(); } catch (Exception) { }
        });
        pressing.IsBackground = true;
        pressing.Start();
    }
    // Every look of UI Automation's, bounded as the Win32 looks are (each within a second, SMTO_ABORTIFHUNG): made on a
    // thread of its own, given at most `ms`, and what it found then is all there is (nothing where it found nothing in
    // time); a thread that does not come back is left to end by itself, in the background, and the script goes on. A
    // look at a window whose thread waits without pumping a message, or one of qKVM held still by the test, can wait
    // for ever: the first VM run hung on Windows 10 there (outside review of 0.1.2's VM test, i020).
    public const int AutomationMs = 5000;
    static T Bounded<T>(Func<T> look, T none, int ms) {
        T found = none;
        Thread looking = new Thread(delegate () {
            try { T seen = look(); lock (typeof(QkvmVm)) { found = seen; } } catch (Exception) { }
        });
        looking.IsBackground = true;
        looking.Start();
        if (!looking.Join(ms)) return none;
        lock (typeof(QkvmVm)) { return found; }
    }
    // The first enabled button UI Automation names in a top-level window whose name, without its & sign, begins with
    // `name` (a task dialog's, which are no windows of their own); null where there is none, or the look took longer
    // than `ms`. Its name and whether it is enabled are read on the look's thread too.
    public static AutomationElement ButtonByAutomation(IntPtr top, string name, int ms) {
        return Bounded<AutomationElement>(delegate () {
            Condition isButton = new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Button);
            foreach (AutomationElement element in AutomationElement.FromHandle(top).FindAll(TreeScope.Descendants, isButton)) {
                string said = element.Current.Name ?? "";
                if (said.Replace("&", "").StartsWith(name, StringComparison.OrdinalIgnoreCase) && element.Current.IsEnabled) return element;
            }
            return null;
        }, null, ms);
    }
    // Every name UI Automation gives inside a top-level window, in its order (a task dialog's words); none where the
    // look took longer than `ms`.
    public static string[] NamesByAutomation(IntPtr top, int ms) {
        return Bounded<string[]>(delegate () {
            System.Collections.Generic.List<string> names = new System.Collections.Generic.List<string>();
            foreach (AutomationElement element in AutomationElement.FromHandle(top).FindAll(TreeScope.Descendants, Condition.TrueCondition)) {
                names.Add(element.Current.Name ?? "");
            }
            return names.ToArray();
        }, new string[0], ms);
    }
    // (Windows' own look at a setup's windows, which never waits on one: V1b. UI Automation named the buttons of Inno
    // Setup's wizard no buttons where it reached them by their window's handle, and a look at the loader's window,
    // whose thread waits for the wizard without pumping a message, can wait for ever.)
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr parent, EachWindow each, IntPtr data);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern uint RealGetWindowClassW(IntPtr window, System.Text.StringBuilder kind, uint most);
    [DllImport("user32.dll")] static extern bool IsWindowEnabled(IntPtr window);
    [DllImport("user32.dll")] static extern int GetWindowLongW(IntPtr window, int index);
    [DllImport("user32.dll")] static extern bool PostMessageW(IntPtr window, uint message, IntPtr w, IntPtr l);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW")] static extern IntPtr SendNullTimeout(IntPtr window, uint message, IntPtr w, IntPtr l, uint flags, uint ms, out IntPtr result);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", CharSet = CharSet.Unicode)] static extern IntPtr SendTextTimeout(IntPtr window, uint message, IntPtr w, System.Text.StringBuilder l, uint flags, uint ms, out IntPtr result);
    // Whether a window's thread answers within a second (WM_NULL, SMTO_ABORTIFHUNG): one that waits without pumping a
    // message, or a hung one, is never asked anything else.
    public static bool Answers(IntPtr window) {
        IntPtr result;
        return SendNullTimeout(window, 0x0000, IntPtr.Zero, IntPtr.Zero, 0x0002, 1000, out result) != IntPtr.Zero;
    }
    // A window's text as it says it itself (WM_GETTEXT, within a second; "" where it does not answer). GetWindowText
    // gives no control's text of another process.
    public static string TextOf(IntPtr window) {
        System.Text.StringBuilder text = new System.Text.StringBuilder(2048);
        IntPtr result;
        if (SendTextTimeout(window, 0x000D, (IntPtr) text.Capacity, text, 0x0002, 1000, out result) == IntPtr.Zero) return "";
        return text.ToString();
    }
    // Every shown window inside a top-level window, at any depth, in Windows' own order (a hidden page's are not shown).
    public static IntPtr[] ShownInside(IntPtr top) {
        System.Collections.Generic.List<IntPtr> found = new System.Collections.Generic.List<IntPtr>();
        EnumChildWindows(top, delegate (IntPtr window, IntPtr data) {
            if (IsWindowVisible(window)) found.Add(window);
            return true;
        }, IntPtr.Zero);
        return found.ToArray();
    }
    // The push buttons inside a top-level window that are shown and enabled: Windows' own (base class "Button", by
    // RealGetWindowClass: a message box's) and Inno Setup's TNewButton, whose base class Windows says is TNewButton
    // itself and which UI Automation names a pane (the VM suite of 26c4c6e7 pressed none: V1c); and by their kind of
    // button (a push button or a command link, not a check box, a choice or a frame).
    public static IntPtr[] ButtonsOf(IntPtr top) {
        System.Collections.Generic.List<IntPtr> found = new System.Collections.Generic.List<IntPtr>();
        foreach (IntPtr window in ShownInside(top)) {
            System.Text.StringBuilder kind = new System.Text.StringBuilder(64);
            RealGetWindowClassW(window, kind, (uint) kind.Capacity);
            int type = GetWindowLongW(window, -16) & 0x0F;
            if ((kind.ToString() == "Button" || kind.ToString() == "TNewButton") && IsWindowEnabled(window) && (type == 0 || type == 1 || type == 0x0E || type == 0x0F)) found.Add(window);
        }
        return found.ToArray();
    }
    // The items of a list inside the setup's windows, as the list says them itself (LB_GETCOUNT, LB_GETTEXTLEN and
    // LB_GETTEXT, each within a second; at most 32): Inno Setup's TNewCheckListBox, the last page's "Start qKVM
    // Desktop" among them, whose items are no windows of their own and have no name in UI Automation (V1c), and
    // Windows' own list box. None for any other window, or one that does not answer.
    public static string[] ItemsOf(IntPtr window) {
        System.Collections.Generic.List<string> items = new System.Collections.Generic.List<string>();
        System.Text.StringBuilder kind = new System.Text.StringBuilder(64);
        RealGetWindowClassW(window, kind, (uint) kind.Capacity);
        if (kind.ToString() != "TNewCheckListBox" && kind.ToString() != "ListBox") return items.ToArray();
        IntPtr count;
        if (SendNullTimeout(window, 0x018B, IntPtr.Zero, IntPtr.Zero, 0x0002, 1000, out count) == IntPtr.Zero) return items.ToArray();
        for (int at = 0; at < Math.Min(count.ToInt64(), 32); at++) {
            IntPtr length;
            if (SendNullTimeout(window, 0x018A, (IntPtr) at, IntPtr.Zero, 0x0002, 1000, out length) == IntPtr.Zero) break;
            if (length.ToInt64() < 0 || length.ToInt64() > 1024) continue;
            System.Text.StringBuilder item = new System.Text.StringBuilder((int) length.ToInt64() + 1);
            IntPtr said;
            if (SendTextTimeout(window, 0x0189, (IntPtr) at, item, 0x0002, 1000, out said) == IntPtr.Zero) break;
            if (said.ToInt64() >= 0) items.Add(item.ToString());
        }
        return items.ToArray();
    }
    // A press of a window's own button, as a click of it (BM_CLICK, as UI Automation presses one), posted: never
    // waited for, as what it starts (a box, a wait) would hold this script.
    public static void Click(IntPtr button) { PostMessageW(button, 0x00F5, IntPtr.Zero, IntPtr.Zero); }
}
'@

# The window every version of the program keeps for its icon (ui/win/tray.cpp), and the program's settings.
$IconWindow = 'QKVM Desktop notification area'
$SettingsFile = Join-Path $Local 'QKVM Desktop\settings.json'
$OverridesFile = "$SettingsFile.setup-overrides"

function Set-StaysInTray([bool] $On) {
    <# The program's own setting "stay in the tray" (settings.json's stay_in_tray), as it writes it (UTF-8 without a
    byte order mark); whether it was there to be set. Only while the program does not run. #>
    if (-not (Test-Path -LiteralPath $SettingsFile)) { return $false }
    $text = [IO.File]::ReadAllText($SettingsFile)
    $pattern = '"stay_in_tray"(\s*):(\s*)(true|false)'
    if ($text -notmatch $pattern) { return $false }
    $word = if ($On) { 'true' } else { 'false' }
    $text = [regex]::Replace($text, $pattern, { param($found) '"stay_in_tray"' + $found.Groups[1].Value + ':' + $found.Groups[2].Value + $word })
    [IO.File]::WriteAllText($SettingsFile, $text, (New-Object Text.UTF8Encoding $false))
    return $true
}

function Stop-ById([int] $Id) {
    <# A process ended, whatever it is doing (one suspended is let go of first). #>
    $process = Get-Process -Id $Id -ErrorAction SilentlyContinue
    if (-not $process) { return }
    try { [void] [QkvmVm]::NtResumeProcess($process.Handle) } catch { }
    Stop-Process -Id $Id -Force -ErrorAction SilentlyContinue
    [void] $process.WaitForExit(10000)
}

function Test-Unchanged($Before, [System.Collections.ArrayList] $Failures) {
    $after = Get-Installed
    if (($Before.Files | ConvertTo-Json -Depth 4) -ne ($after.Files | ConvertTo-Json -Depth 4) -or ($Before.Entry | ConvertTo-Json) -ne ($after.Entry | ConvertTo-Json)) {
        [void] $Failures.Add('the setup changed something')
    }
}

function Test-SameStart([DateTime] $Began, $Listed) {
    <# Whether a process opened by its number began when Windows' list said the process of that number began: the
    same start exactly, at the list's own precision (Windows keeps a start to 100 ns, CIM says it to the microsecond,
    cut, not rounded), as Stop-Tree has it (reviews R7, F6). Where the list says no start, it is not the same. #>
    if ($null -eq $Listed) { return $false }
    $opened = $Began.ToUniversalTime().Ticks
    return ($opened - ($opened % 10)) -eq ([DateTime] $Listed).ToUniversalTime().Ticks
}

function Get-SetupProcesses([int] $SetupId) {
    <# The setup's process and those it started, and they in turn, by Windows' list (at most 16): a process is taken
    for a child only where it began at or after its parent, as Stop-Tree takes one (Windows keeps a parent's number
    after it has ended, and it may since be another's); and, opened again by its number, only where it is still the
    process the list named: one that ended between the list and the opening, its number taken by another program, is
    never taken for the setup's, so that no press is aimed at another program's windows (outside review of 0.1.2's
    VM test, i020). Inno Setup runs its wizard in a second process, setup.tmp in a folder is-*.tmp, that the setup's
    own file starts (V1b). #>
    $ids = New-Object System.Collections.ArrayList
    $next = New-Object System.Collections.Queue
    # (The setup's own process is the one this script started and holds; its children are each held to the list.)
    $next.Enqueue(@($SetupId, $null))
    while ($next.Count -and $ids.Count -lt 16) {
        $queued = $next.Dequeue()
        $id = [int] $queued[0]
        $began = $null
        try { $began = [Diagnostics.Process]::GetProcessById($id).StartTime } catch { }
        if ($null -eq $began) { continue }
        if ($id -ne $SetupId -and -not (Test-SameStart $began $queued[1])) { continue }
        [void] $ids.Add($id)
        foreach ($child in @(Get-CimInstance Win32_Process -Filter "ParentProcessId = $id")) {
            if ($null -ne $child.CreationDate -and $child.CreationDate -ge $began -and -not $ids.Contains([int] $child.ProcessId)) { $next.Enqueue(@([int] $child.ProcessId, $child.CreationDate)) }
        }
    }
    return @($ids)
}

function Get-SetupWindows([int] $SetupId) {
    <# The top-level windows of a running setup's processes (Get-SetupProcesses), boxes included, that are shown: from
    Windows' own list of each process's windows, and only those whose thread answers within a second. The loader's
    window, whose thread waits for the wizard without pumping a message, is never asked anything (a look at it can
    wait for ever: V1b), nor is any window of another process, a hung program's among them (V1). Their handles. #>
    $found = New-Object System.Collections.ArrayList
    foreach ($id in Get-SetupProcesses $SetupId) {
        foreach ($handle in [QkvmVm]::WindowsOf([uint32] $id)) {
            if ([QkvmVm]::Answers($handle)) { [void] $found.Add($handle) }
        }
    }
    return @($found)
}

function Find-SetupButton([int] $SetupId, [string] $Name, $Within = $null) {
    <# An enabled, shown button of the setup's windows (of the one top-level window $Within, where it is given) whose
    name, without its & sign, begins with $Name; $null where none. Windows' own buttons first, by their base class and
    the text they say themselves: the wizard's (Inno Setup's TNewButton) and a message box's (UI Automation names
    Inno Setup's buttons panes: the VM runs of 3f88ccb2 and 26c4c6e7 pressed none, V1b and V1c). Then what UI Automation names a button
    there: a task dialog's, which are no windows of their own. What is found: its top-level window, and the button's
    handle or UI Automation's element (Push-SetupButton presses either). #>
    $tops = if ($Within) { @($Within) } else { @(Get-SetupWindows $SetupId) }
    foreach ($top in $tops) {
        foreach ($button in [QkvmVm]::ButtonsOf($top)) {
            if ([QkvmVm]::TextOf($button).Replace('&', '').StartsWith($Name, [StringComparison]::OrdinalIgnoreCase)) {
                return [pscustomobject]@{ Window = $top; Button = $button; Element = $null }
            }
        }
    }
    # (Each look of UI Automation's on a thread of its own, at most [QkvmVm]::AutomationMs: i020.)
    foreach ($top in $tops) {
        $element = [QkvmVm]::ButtonByAutomation($top, $Name, [QkvmVm]::AutomationMs)
        if ($null -ne $element) { return [pscustomobject]@{ Window = $top; Button = [IntPtr]::Zero; Element = $element } }
    }
    return $null
}

function Find-ButtonBeside([int] $SetupId, [string] $Beside, [string] $Name) {
    <# A button whose name begins with $Name in the very top-level window (a box) where a button beginning with $Beside
    is: the Cancel of a box, and not the wizard's. $null where none. #>
    $known = Find-SetupButton $SetupId $Beside
    if (-not $known) { return $null }
    return Find-SetupButton $SetupId $Name $known.Window
}

function Push-SetupButton($Found) {
    <# A press of what Find-SetupButton found: a window's own button clicked by a message posted to it, and a task
    dialog's by UI Automation from a thread of its own; neither waited for, as what it starts (a box, a wait) would
    hold this script. #>
    if ($Found.Element) { [QkvmVm]::Press($Found.Element) } else { [QkvmVm]::Click($Found.Button) }
}

function Get-SetupText([int] $SetupId) {
    <# Every text the setup's windows show, one a line, what a person reads there: each top-level window's title and
    the text of each shown window in it, as each says it itself (a hidden page's are not shown), and the items of
    each list in it (the last page's "Start qKVM Desktop", V1c), then what UI Automation names in it (a task dialog's
    words, which are in no window of their own). #>
    $said = New-Object System.Collections.ArrayList
    foreach ($top in Get-SetupWindows $SetupId) {
        [void] $said.Add([QkvmVm]::TextOf($top))
        foreach ($inside in [QkvmVm]::ShownInside($top)) {
            [void] $said.Add([QkvmVm]::TextOf($inside))
            foreach ($item in [QkvmVm]::ItemsOf($inside)) { [void] $said.Add($item) }
        }
        foreach ($one in [QkvmVm]::NamesByAutomation($top, [QkvmVm]::AutomationMs)) { [void] $said.Add([string] $one) }
    }
    return ($said -join "`n")
}
function Invoke-SetupByHand([string] $Log, [object[]] $Presses, [int] $Seconds = 300) {
    <#
    The new setup run as a person runs it (no switch but its log): each of $Presses (the start of a button's name) is
    pressed once it is there, in that order, and what the setup's windows showed just before each press is kept
    (Shown, one for each press). Whether every press was made, and how long after the one before each came (Gaps);
    what the windows showed last while a press was waited for (LastSeen, looked at every 10 s), for a run that stuck.
    A press may also be @{ Press = <name>; Beside = <name> } (the button of the box where the other is: a box's own
    Cancel), or a script block, run once when its turn comes (what a person does elsewhere meanwhile).
    #>
    $script:byHand = [ordered]@{ At = 0; Shown = (New-Object System.Collections.ArrayList); Gaps = (New-Object System.Collections.ArrayList); Last = 0.0
        LastSeen = ''; Looked = 0.0 }
    $run = Invoke-Setup $Setup "/LOG=`"$Log`"" $Seconds {
        param($process, $gone)
        # (Nothing pressed in the first 3 s: the wizard is given the time a person's eyes take to find it.)
        if ($script:byHand.At -ge $Presses.Count -or $gone -lt 3) { return $false }
        if ($gone - $script:byHand.Looked -ge 10) { $script:byHand.LastSeen = Get-SetupText $process.Id; $script:byHand.Looked = $gone }
        $step = $Presses[$script:byHand.At]
        if ($step -is [scriptblock]) {
            [void] $script:byHand.Shown.Add((Get-SetupText $process.Id))
            [void] $script:byHand.Gaps.Add([Math]::Round($gone - $script:byHand.Last, 1))
            [void] (& $step)
            $script:byHand.Last = $gone
            $script:byHand.At++
            return $false
        }
        $button = if ($step -is [hashtable]) { Find-ButtonBeside $process.Id ([string] $step.Beside) ([string] $step.Press) } else { Find-SetupButton $process.Id ([string] $step) }
        if ($button) {
            [void] $script:byHand.Shown.Add((Get-SetupText $process.Id))
            [void] $script:byHand.Gaps.Add([Math]::Round($gone - $script:byHand.Last, 1))
            $script:byHand.Last = $gone
            Push-SetupButton $button
            $script:byHand.At++
        }
        return $false
    } {
        # (Past $Seconds: once every press is made, the setup is given 15 minutes more after the last one, to end of
        # itself; a copy under way is waited for in any case, review V1.)
        param($process, $gone)
        return $script:byHand.At -ge $Presses.Count -and ($gone - $script:byHand.Last) -lt 900
    }
    return [pscustomobject]@{ Run = $run; Pressed = $script:byHand.At; Shown = @($script:byHand.Shown); Gaps = @($script:byHand.Gaps)
        LastSeen = $(if ($script:byHand.At -lt $Presses.Count) { $script:byHand.LastSeen } else { '' }) }
}

function Test-StartedAgain([string] $LogName, [System.Collections.ArrayList] $Failures, [bool] $Expected) {
    <# Whether the setup started the program at its end (its log, and the program's mutex), as $Expected says. #>
    $said = (Read-Log $LogName) -match $StartedLine
    $running = Wait-Until { Test-ProgramRunning } $(if ($Expected) { 30 } else { 5 })
    if ($Expected -and (-not $said -or -not $running)) { [void] $Failures.Add('the setup did not start the program again') }
    if (-not $Expected -and ($said -or $running)) { [void] $Failures.Add('the setup started the program, which was not running before it') }
}

# (The new version first: 17 left nothing installed.)
$installed = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '18-install-first')`""
$firstFailures = New-Object System.Collections.ArrayList
if ($installed.Exit -ne 0) { [void] $firstFailures.Add("the new version could not be installed first ($($installed.Exit))") }

# --- 18 the program only in the tray: closed by the setup, the orderly way, and started again
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
foreach ($failure in $firstFailures) { [void] $failures.Add($failure) }
$log = Get-Log '18-tray-only-closed'
if (-not (Set-StaysInTray $true)) { [void] $failures.Add('the program''s settings have no stay_in_tray to set') }
$started = Start-Program
$trayId = $started.Process.Id
if (-not $started.Up) { [void] $failures.Add('the program did not start') }
Start-Sleep -Seconds 3
[void] $started.Process.CloseMainWindow()
Start-Sleep -Seconds 3
$onlyTray = (Get-Process -Id $trayId -ErrorAction SilentlyContinue) -and [QkvmVm]::VisibleWindowsOf($trayId) -eq 0 -and [QkvmVm]::HasWindowOfKind($trayId, $IconWindow)
if (-not $onlyTray) { [void] $failures.Add('the program did not go on in the tray alone, with no window to be seen') }
$run = Invoke-Setup $Setup "$SilentArguments /LOG=`"$log`"" 180
if ($run.Exit -ne 0) { [void] $failures.Add("the setup ended with $($run.Exit)") }
$said = Read-Log '18-tray-only-closed'
if ($said -notmatch 'Asked qKVM Desktop to close, the orderly way\.') { [void] $failures.Add('the setup''s log does not say it asked the program to close') }
if ($said -notmatch 'qKVM Desktop closed after \d+ ms\.') { [void] $failures.Add('the setup''s log does not say the program closed') }
if ($said -match 'ended by force') { [void] $failures.Add('the program was ended by force') }
if (Get-Process -Id $trayId -ErrorAction SilentlyContinue) { [void] $failures.Add('the program that ran in the tray is still running') }
if (@(Get-ResultLines '18-tray-only-closed').Count) { [void] $failures.Add('the log of a setup that installed has a result line of the contract') }
$state = Test-Installed $newVersion $failures
Test-StartedAgain '18-tray-only-closed' $failures $true
$how = Stop-Program
if (-not (Set-StaysInTray $false)) { [void] $failures.Add('the program''s setting stay_in_tray could not be put back') }
Test-Data $failures -ProgramStarted
Write-Step '18' 'tray-only-closed' $began $failures ([ordered]@{ run = $run; firstInstall = $installed; onlyInTheTray = [bool] $onlyTray; files = $state.Files.Count; ended = $how })

# --- 19 the program holding what its end would cut: nothing changed, its sentence in the result line; and a record left
# from before whose doubt is over holds nothing (B12)
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$log = Get-Log '19-busy-held'
# (The notes' times as the program writes them: UTC, to the millisecond, whatever this machine's culture.)
function Get-NoteTime([DateTime] $At) { $At.ToString('yyyy-MM-ddTHH:mm:ss.fffZ', [Globalization.CultureInfo]::InvariantCulture) }
# (First, as B12 found it on 0.1.0's own profile: a server's BIOS setup boot override noted beside the settings, for a
# server at a loopback port where nothing answers and on no card, its doubt over long ago: a day ago, by this machine's
# own clock, so that a machine whose clock is behind takes it as over too. It holds nothing: the silent setup closes the
# program and upgrades it. And, as SPEC says of SetupOverrides::prune, the program lets go of that record at its start:
# a record of a server on no card of the wall any more, its doubt over and not marked queued, goes at the next start,
# where the settings' list was read whole (the settings are there, from the program's starts before in this run); the
# setup changes nothing of the note. Until 2026-11-21 this step wanted the note byte for byte after it all, which no start of a
# version with the prune leaves so. A note the prune keeps, of a server on a card of the wall whose doubt is over, holding
# no update, is asked by ui/tests/test_wall_windows.cpp's anOverrideWhoseDoubtIsOverHoldsNoUpdate through the program's
# own wall and Setup's request: here it would need a server card and its password made by hand in this machine's
# profile, which this suite does not make (review armp-1 L4, left for after 0.2.0).)
$dayAgo = [DateTime]::UtcNow.AddDays(-1)
$left = '{"version":1,"servers":[{"address":"https://127.0.0.1:9/","machine":"","since":"' + (Get-NoteTime $dayAgo) + '","doubtUntil":"' + (Get-NoteTime $dayAgo.AddMinutes(2)) + '"}]}'
[IO.File]::WriteAllText($OverridesFile, $left, (New-Object Text.UTF8Encoding $false))
if (-not (Test-Path -LiteralPath $SettingsFile)) { [void] $failures.Add('the program''s settings are not there: their list says nothing then of a server on no card') }
$named19 = { (Test-Path -LiteralPath $OverridesFile) -and ([IO.File]::ReadAllText($OverridesFile)).Contains('"https://127.0.0.1:9/"') }
$started = Start-Program
$overId = $started.Process.Id
if (-not $started.Up) { [void] $failures.Add('the program did not start') }
if (-not (Wait-Until { -not (& $named19) } 10)) {
    [void] $failures.Add('the program did not let go at its start of the record of a server on no card whose doubt was over (SetupOverrides::prune)')
}
$prunedBytes = if (Test-Path -LiteralPath $OverridesFile) { [IO.File]::ReadAllBytes($OverridesFile) } else { [byte[]] @() }
Start-Sleep -Seconds 5
$over = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '19-doubt-over')`"" 180
if ($over.Exit -ne 0) { [void] $failures.Add("with a note whose doubt is over, the setup ended with $($over.Exit), not 0") }
if (Get-Process -Id $overId -ErrorAction SilentlyContinue) { [void] $failures.Add('with a note whose doubt is over, the program was not closed') }
if ((Read-Log '19-doubt-over') -match 'stays open|ended by force') { [void] $failures.Add('a note whose doubt is over held the program, or it was ended by force') }
Test-StartedAgain '19-doubt-over' $failures $true
[void] (Stop-Program)
$nowBytes = if (Test-Path -LiteralPath $OverridesFile) { [IO.File]::ReadAllBytes($OverridesFile) } else { [byte[]] @() }
if ([Convert]::ToBase64String($nowBytes) -cne [Convert]::ToBase64String($prunedBytes)) { [void] $failures.Add('the setup, or the program it started again, changed the note the program had let go of that record in') }
# (Then a change that may still come: a note whose doubt goes on for eight minutes more, as a start into BIOS setup
# leaves it, for the same server, within the longest doubt a press notes (SetupOverrides::longestDoubtMs, ten minutes:
# one that reaches further was noted by a clock that ran ahead, and holds nothing); held until its doubt is over.)
$notedAt = [DateTime]::UtcNow
$noted = '{"version":1,"servers":[{"address":"https://127.0.0.1:9/","machine":"","since":"' + (Get-NoteTime $notedAt) + '","doubtUntil":"' + (Get-NoteTime $notedAt.AddMinutes(8)) + '"}]}'
[IO.File]::WriteAllText($OverridesFile, $noted, (New-Object Text.UTF8Encoding $false))
$started = Start-Program
$busyId = $started.Process.Id
if (-not $started.Up) { [void] $failures.Add('the program did not start') }
Start-Sleep -Seconds 5
$before = Get-Installed
$run = Invoke-Setup $Setup "$SilentArguments /LOG=`"$log`"" 180
if ($run.Exit -ne 7) { [void] $failures.Add("the setup ended with $($run.Exit), not 7") }
Test-Unchanged $before $failures
if (-not (Get-Process -Id $busyId -ErrorAction SilentlyContinue)) { [void] $failures.Add('the program is no longer running') }
$resultLines = @(Get-ResultLines '19-busy-held')
$heldLine = 'qKVM-RESULT: 7 nothing was changed: qKVM Desktop is running and stays open: A start into BIOS setup of a server is under way. Install once it has ended; /FORCECLOSE=1 ends it by force'
if ($resultLines.Count -ne 1 -or $resultLines[0] -cne $heldLine) {
    [void] $failures.Add("the setup's log does not have the one result line with the program's sentence ($($resultLines -join ' | '))")
}
if ((Read-Log '19-busy-held') -match 'ended by force') { [void] $failures.Add('the program was ended by force') }
# (Once it has ended, nothing holds the setup: it installs, and starts nothing, as nothing ran then.)
$how = Stop-Program
Remove-Item -LiteralPath $OverridesFile -Force -ErrorAction SilentlyContinue
$finish = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '19-after-it-ended')`"" 180
if ($finish.Exit -ne 0) { [void] $failures.Add("once the program had ended, the setup ended with $($finish.Exit)") }
$state = Test-Installed $newVersion $failures
Test-StartedAgain '19-after-it-ended' $failures $false
[void] (Stop-Program)
Test-Data $failures -ProgramStarted
Write-Step '19' 'busy-held' $began $failures ([ordered]@{ doubtOver = $over; run = $run; resultLines = $resultLines; ended = $how; finish = $finish })

# --- 20 the program not answering: nothing changed, and with /FORCECLOSE=1 ended by force and upgraded
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$started = Start-Program
$hungId = $started.Process.Id
if (-not $started.Up) { [void] $failures.Add('the program did not start') }
Start-Sleep -Seconds 5
$suspended = [QkvmVm]::NtSuspendProcess($started.Process.Handle) -eq 0
if (-not $suspended) { [void] $failures.Add('the program could not be suspended') }
try {
    $before = Get-Installed
    $run = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '20-hung')`"" 180
    if ($run.Exit -ne 7) { [void] $failures.Add("the setup ended with $($run.Exit), not 7") }
    if ($run.Seconds -lt 9 -or $run.Seconds -gt 60) { [void] $failures.Add("the setup gave up after $($run.Seconds) s, not after about 10 s") }
    Test-Unchanged $before $failures
    if (-not (Get-Process -Id $hungId -ErrorAction SilentlyContinue)) { [void] $failures.Add('the program was ended without /FORCECLOSE=1') }
    $resultLines = @(Get-ResultLines '20-hung')
    if ($resultLines.Count -ne 1 -or $resultLines[0] -cne 'qKVM-RESULT: 7 nothing was changed: qKVM Desktop is running and did not answer within 10 seconds; /FORCECLOSE=1 ends it by force') {
        [void] $failures.Add("the setup's log does not have the one result line of a program that does not answer ($($resultLines -join ' | '))")
    }
    $forced = Invoke-Setup $Setup "$SilentArguments /FORCECLOSE=1 /LOG=`"$(Get-Log '20-hung-forced')`"" 180
    if ($forced.Exit -ne 0) { [void] $failures.Add("with /FORCECLOSE=1 the setup ended with $($forced.Exit)") }
    if ((Read-Log '20-hung-forced') -notmatch 'qKVM Desktop is ended by force, as chosen\.') { [void] $failures.Add('the setup''s log does not say it ended the program by force') }
    if (Get-Process -Id $hungId -ErrorAction SilentlyContinue) { [void] $failures.Add('with /FORCECLOSE=1 the program is still there') }
    $state = Test-Installed $newVersion $failures
    Test-StartedAgain '20-hung-forced' $failures $true
} finally {
    Stop-ById $hungId
}
$how = Stop-Program
Test-Data $failures -ProgramStarted
Write-Step '20' 'hung' $began $failures ([ordered]@{ run = $run; resultLines = $resultLines; forced = $forced; ended = $how })

# --- 21 a copy from another folder running beside the installed one: left alone
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$zip = Join-Path $UserFolder 'qkvm-vm-zip-copy'
Remove-Item -LiteralPath $zip -Recurse -Force -ErrorAction SilentlyContinue
Copy-Item -LiteralPath $App -Destination $zip -Recurse
foreach ($own in 'qkvm-desktop.installed', 'unins000.exe', 'unins000.dat') { Remove-Item -LiteralPath (Join-Path $zip $own) -Force -ErrorAction SilentlyContinue }
$other = Start-Process -FilePath (Join-Path $zip 'qkvm-desktop.exe') -WorkingDirectory $zip -PassThru
if (-not (Wait-Until { Test-ProgramRunning } 30)) { [void] $failures.Add('the copy from another folder did not start') }
Start-Sleep -Seconds 3
if (@(Get-ProgramProcesses).Count) { [void] $failures.Add('the installed program runs beside the other copy') }
$run = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '21-zip-copy-beside')`"" 180
if ($run.Exit -ne 0) { [void] $failures.Add("the setup ended with $($run.Exit)") }
$said = Read-Log '21-zip-copy-beside'
if ($said -match 'runs from its folder|Asked qKVM Desktop to close|ended by force') { [void] $failures.Add('the setup took the copy from another folder for its own') }
if (-not (Get-Process -Id $other.Id -ErrorAction SilentlyContinue)) { [void] $failures.Add('the copy from another folder is no longer running') }
$state = Test-Installed $newVersion $failures
Start-Sleep -Seconds 3
if (@(Get-ProgramProcesses).Count) { [void] $failures.Add('the setup started the installed program') }
[void] $other.CloseMainWindow()
if (-not $other.WaitForExit(10000)) { Stop-ById $other.Id }
[void] (Wait-Until { -not (Test-ProgramRunning) } 10)
Remove-Item -LiteralPath $zip -Recurse -Force -ErrorAction SilentlyContinue
Test-Data $failures -ProgramStarted
Write-Step '21' 'zip-copy-beside' $began $failures ([ordered]@{ run = $run; otherStillRan = $true; files = $state.Files.Count })

# --- 22 interactive: the program not answering, End it now, the upgrade, and Start qKVM Desktop ticked
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$started = Start-Program
$hungId = $started.Process.Id
if (-not $started.Up) { [void] $failures.Add('the program did not start') }
Start-Sleep -Seconds 5
if ([QkvmVm]::NtSuspendProcess($started.Process.Handle) -ne 0) { [void] $failures.Add('the program could not be suspended') }
$waitedForIt = $null
try {
    $hand = Invoke-SetupByHand (Get-Log '22-hung-end-it-now') @('Next', 'Install', 'Close qKVM and continue', 'End it now', 'Finish')
    if ($hand.Pressed -ne 5) { [void] $failures.Add("the setup was not seen through by hand: $($hand.Pressed) of 5 presses made (it ended with $($hand.Run.Exit))") }
    elseif ($hand.Run.Exit -ne 0) { [void] $failures.Add("the setup ended with $($hand.Run.Exit)") }
    if ($hand.Shown.Count -ge 1 -and ($hand.Shown[0] -notmatch "qKVM Desktop $([regex]::Escape($newVersion)) is installed\. This installs it again and keeps your wall, settings and passwords\." -or $hand.Shown[0] -notmatch 'qKVM Desktop is running\. When you choose Install, Setup offers to close it for you\.')) {
        [void] $failures.Add('the first page does not say what is installed and that it runs')
    }
    if ($hand.Shown.Count -ge 4 -and $hand.Shown[3] -notmatch 'did not answer within 10 seconds') { [void] $failures.Add('the setup did not say that the program did not answer') }
    # (How long the setup waited, by the times on its own log: the request, then its withdrawal as it offers End it now;
    # never by when a look at its windows saw the box, review V1.)
    $waitedForIt = Get-LogSeconds '22-hung-end-it-now' 'Asked qKVM Desktop to close, the orderly way.' 'Withdrew the request to close.'
    if ($null -eq $waitedForIt) { [void] $failures.Add('the setup''s log has no request to close followed by its withdrawal (it never came to ask)') }
    elseif ($waitedForIt -lt 9 -or $waitedForIt -gt 13) { [void] $failures.Add("the setup gave up on its request $waitedForIt s after it asked, by its own log, not after about 10 s") }
    if ($hand.Shown.Count -ge 5 -and $hand.Shown[4] -notmatch 'Start qKVM Desktop') { [void] $failures.Add('the last page does not offer Start qKVM Desktop') }
    if ((Read-Log '22-hung-end-it-now') -notmatch 'qKVM Desktop is ended by force, as chosen\.') { [void] $failures.Add('the setup''s log does not say it ended the program by force') }
    if (Get-Process -Id $hungId -ErrorAction SilentlyContinue) { [void] $failures.Add('after End it now the program is still there') }
    $state = Test-Installed $newVersion $failures
    # (Finish with the tick as the page had it: ticked, as the program ran before, so it is started.)
    Test-StartedAgain '22-hung-end-it-now' $failures $true
} finally {
    Stop-ById $hungId
}
$how = Stop-Program
Test-Data $failures -ProgramStarted
Write-Step '22' 'hung-end-it-now' $began $failures ([ordered]@{ run = $hand.Run; pressed = $hand.Pressed; secondsBetweenPresses = $hand.Gaps; secondsAskedToWithdrawn = $waitedForIt
    shown = $hand.Shown; lastSeen = $hand.LastSeen; ended = $how })

# --- 23 an older version running: one before 0.1.2 cannot be asked
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$older = Reset-ToOlder '23' $failures
$asksIt = [version] $oldVersion -ge [version] '0.1.2'
$started = Start-Program
$olderId = $started.Process.Id
if (-not $started.Up) { [void] $failures.Add('the older program did not start') }
Start-Sleep -Seconds 5
$before = Get-Installed
$hand = $null
try {
    $run = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '23-older-running')`"" 180
    $resultLines = @(Get-ResultLines '23-older-running')
    if ($asksIt) {
        # (An older version that listens is asked, and closes: the silent install upgrades it.)
        if ($run.Exit -ne 0) { [void] $failures.Add("the setup ended with $($run.Exit)") }
        if ((Read-Log '23-older-running') -match 'ended by force') { [void] $failures.Add('the older program was ended by force') }
        Test-StartedAgain '23-older-running' $failures $true
    } else {
        if ($run.Exit -ne 7) { [void] $failures.Add("the setup ended with $($run.Exit), not 7") }
        Test-Unchanged $before $failures
        if ($resultLines.Count -ne 1 -or $resultLines[0] -cne 'qKVM-RESULT: 7 nothing was changed: qKVM Desktop is running, and this copy of it cannot be asked to close; /FORCECLOSE=1 ends it by force') {
            [void] $failures.Add("the setup's log does not have the one result line of a version that cannot be asked ($($resultLines -join ' | '))")
        }
        if (-not (Get-Process -Id $olderId -ErrorAction SilentlyContinue)) { [void] $failures.Add('the older program is no longer running') }
        $hand = Invoke-SetupByHand (Get-Log '23-older-by-hand') @('Next', 'Install', 'Close qKVM and continue', 'End it now', 'Finish')
        if ($hand.Pressed -ne 5) { [void] $failures.Add("the setup was not seen through by hand: $($hand.Pressed) of 5 presses made (it ended with $($hand.Run.Exit))") }
        elseif ($hand.Run.Exit -ne 0) { [void] $failures.Add("the setup ended with $($hand.Run.Exit)") }
        if ($hand.Shown.Count -ge 1 -and $hand.Shown[0] -notmatch "qKVM Desktop $([regex]::Escape($oldVersion)) is installed\. This upgrades it to $([regex]::Escape($newVersion)) and keeps your wall, settings and passwords\.") {
            [void] $failures.Add('the first page does not name the installed version and the new one')
        }
        if ($hand.Shown.Count -ge 4 -and $hand.Shown[3] -notmatch 'cannot be asked to close by Setup: .*Quit it from its icon in the notification area') {
            [void] $failures.Add('the setup did not say how to quit a version that cannot be asked')
        }
        if (Get-Process -Id $olderId -ErrorAction SilentlyContinue) { [void] $failures.Add('after End it now the older program is still there') }
        Test-StartedAgain '23-older-by-hand' $failures $true
    }
    $state = Test-Installed $newVersion $failures
} finally {
    Stop-ById $olderId
}
$how = Stop-Program
Test-Data $failures -ProgramStarted
Write-Step '23' 'older-running' $began $failures ([ordered]@{ olderAsked = $asksIt; run = $run; resultLines = $resultLines
    byHand = $(if ($hand) { [ordered]@{ run = $hand.Run; pressed = $hand.Pressed; shown = $hand.Shown; lastSeen = $hand.LastSeen } } else { $null }); ended = $how })

# --- 24 a request Setup gave up on: the program's interface stalled past Setup's wait, then answering again (review M1)
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$AnswerWindow = 'qKVM Desktop quit answer'
$started = Start-Program
$stallId = $started.Process.Id
if (-not $started.Up) { [void] $failures.Add('the program did not start') }
Start-Sleep -Seconds 5
$before = Get-Installed
# Silent: the interface stalls (its thread alone suspended, the rest of the program running), the setup gives up and
# changes nothing; the interface answers again, and the program, hearing the request now, stays.
$script:stalled24 = [QkvmVm]::SuspendFirstThread($stallId)
if ($script:stalled24 -eq [IntPtr]::Zero) { [void] $failures.Add('the program''s interface could not be stalled') }
try {
    $run = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '24-stalled-given-up')`"" 180
} finally {
    [QkvmVm]::ResumeAndLetGo($script:stalled24)
    $script:stalled24 = [IntPtr]::Zero
}
if ($run.Exit -ne 7) { [void] $failures.Add("the setup ended with $($run.Exit), not 7") }
$resultLines = @(Get-ResultLines '24-stalled-given-up')
if ($resultLines.Count -ne 1 -or $resultLines[0] -cne 'qKVM-RESULT: 7 nothing was changed: qKVM Desktop is running and did not answer within 10 seconds; /FORCECLOSE=1 ends it by force') {
    [void] $failures.Add("the setup's log does not have the one result line of a program that does not answer ($($resultLines -join ' | '))")
}
if ((Read-Log '24-stalled-given-up') -notmatch 'Withdrew the request to close\.') { [void] $failures.Add('the setup''s log does not say it withdrew its request') }
Test-Unchanged $before $failures
Start-Sleep -Seconds 15
$silentAlive = [bool] (Get-Process -Id $stallId -ErrorAction SilentlyContinue)
if (-not $silentAlive) { [void] $failures.Add('once its interface answered again, the program closed for a setup that had given up') }
$silentAnswer = [QkvmVm]::TitleOf([uint32] $stallId, $AnswerWindow)
if ($silentAnswer -notmatch '^1\* .*too late') { [void] $failures.Add("the program did not say that the request came too late ($silentAnswer)") }
# By hand: Close qKVM and continue, the interface stalled; when the setup says it did not answer, Cancel (back to the
# Ready page: the setup still runs, and holds its mutex); the interface answers again; the program stays. Then the
# setup is left (Cancel, Yes).
$script:stalled24 = [QkvmVm]::SuspendFirstThread($stallId)
$script:alive24 = $false
$script:answer24 = ''
try {
    $hand = Invoke-SetupByHand (Get-Log '24-by-hand-cancel') @('Next', 'Install', 'Close qKVM and continue', @{ Press = 'Cancel'; Beside = 'End it now' }, {
            Start-Sleep -Seconds 2
            [QkvmVm]::ResumeAndLetGo($script:stalled24)
            $script:stalled24 = [IntPtr]::Zero
            Start-Sleep -Seconds 15
            $script:alive24 = [bool] (Get-Process -Id $stallId -ErrorAction SilentlyContinue)
            $script:answer24 = [QkvmVm]::TitleOf([uint32] $stallId, $AnswerWindow)
        }, 'Cancel', 'Yes')
} finally {
    [QkvmVm]::ResumeAndLetGo($script:stalled24)
    $script:stalled24 = [IntPtr]::Zero
}
if ($hand.Pressed -ne 7) { [void] $failures.Add("the setup was not seen through by hand: $($hand.Pressed) of 7 steps made (it ended with $($hand.Run.Exit))") }
if ($hand.Shown.Count -ge 4 -and $hand.Shown[3] -notmatch 'did not answer within 10 seconds') { [void] $failures.Add('the setup did not say that the program did not answer') }
if (-not $script:alive24) { [void] $failures.Add('after Cancel, once its interface answered again, the program closed while the setup waited on its Ready page') }
if ($script:answer24 -notmatch '^2\* .*too late') { [void] $failures.Add("after Cancel the program did not say that the request came too late ($($script:answer24))") }
if ((Read-Log '24-by-hand-cancel') -match 'ended by force') { [void] $failures.Add('the program was ended by force') }
Test-Unchanged $before $failures
$how = Stop-Program
Test-Data $failures -ProgramStarted
Write-Step '24' 'stalled-given-up' $began $failures ([ordered]@{ run = $run; resultLines = $resultLines; silentAnswer = $silentAnswer
    byHand = [ordered]@{ run = $hand.Run; pressed = $hand.Pressed; shown = $hand.Shown; lastSeen = $hand.LastSeen; answer = $script:answer24 }; ended = $how })

# --- 25 a fresh install made outside update mode, the program running, the uninstaller: refused (review M2)
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
# (Uninstalled first, so that the install below writes an uninstall log of its own, with no record of an earlier one:
# what the uninstaller checks is what this install kept there.)
$uninstalled = Invoke-Setup (Join-Path $App 'unins000.exe') "$SilentArguments /LOG=`"$(Get-Log '25-uninstall-first')`"" 180
[void] (Wait-Until { -not (Test-Path -LiteralPath $App) } 30)
if ($uninstalled.Exit -ne 0 -or (Test-Path -LiteralPath $App)) { [void] $failures.Add("the program could not be uninstalled first ($($uninstalled.Exit))") }
$fresh = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '25-fresh-install')`""
if ($fresh.Exit -ne 0) { [void] $failures.Add("the fresh install ended with $($fresh.Exit)") }
$state = Test-Installed $newVersion $failures
$started = Start-Program
if (-not $started.Up) { [void] $failures.Add('the program did not start') }
Start-Sleep -Seconds 5
$before = Get-Installed
$run = Invoke-Setup (Join-Path $App 'unins000.exe') "$SilentArguments /LOG=`"$(Get-Log '25-uninstall-refused')`"" 120
if ($run.Exit -eq 0) { [void] $failures.Add('the uninstaller ended with 0 while the program ran') }
# (Refused by its own look at the program's mutex, review V2: Inno Setup keeps nothing of AppMutex for it here. And
# refused as it begins, in InitializeUninstall, where the uninstall is decided, never only at its look again before
# the removal, review F4.)
if ((Read-Log '25-uninstall-refused') -notmatch 'qKVM Desktop is running: it is not uninstalled\.') { [void] $failures.Add('the uninstaller''s log does not say it refused while the program ran') }
if ((Read-Log '25-uninstall-refused') -match 'started while it was being uninstalled') { [void] $failures.Add('the uninstaller refused only at its look again before the removal, not as it began') }
$after = Get-Installed
if (($before.Files | ConvertTo-Json -Depth 4) -ne ($after.Files | ConvertTo-Json -Depth 4) -or -not $after.Entry -or -not $after.StartMenu) {
    [void] $failures.Add('the uninstaller removed something while the program ran')
}
if (-not (Get-Process -Id $started.Process.Id -ErrorAction SilentlyContinue)) { [void] $failures.Add('the program is no longer running') }
$how = Stop-Program
# (And while a setup is open, the program not running: Setup's mutex held here, under the setup's own name in this
# session, as an open setup holds it. The uninstaller finds it there already as it begins, and refuses, review F4.)
$created = $false
$open = [Threading.Mutex]::new($false, $SetupMutex, [ref] $created)
try {
    if (-not $created) { [void] $failures.Add("a setup's mutex was there already, held by another") }
    $beforeOpen = Get-Installed
    $whileOpen = Invoke-Setup (Join-Path $App 'unins000.exe') "$SilentArguments /LOG=`"$(Get-Log '25-uninstall-while-setup')`"" 120
    if ($whileOpen.Exit -eq 0) { [void] $failures.Add('the uninstaller ended with 0 while a setup was open') }
    if ((Read-Log '25-uninstall-while-setup') -notmatch 'A setup of qKVM Desktop, or another uninstall of it, is at work: it is not uninstalled\.') {
        [void] $failures.Add('the uninstaller''s log does not say it refused while a setup was open')
    }
    $afterOpen = Get-Installed
    if (($beforeOpen.Files | ConvertTo-Json -Depth 4) -ne ($afterOpen.Files | ConvertTo-Json -Depth 4) -or -not $afterOpen.Entry -or -not $afterOpen.StartMenu) {
        [void] $failures.Add('the uninstaller removed something while a setup was open')
    }
} finally {
    $open.Dispose()
}
Test-Data $failures -ProgramStarted
Write-Step '25' 'fresh-install-uninstall-refused' $began $failures ([ordered]@{ uninstalledFirst = $uninstalled; fresh = $fresh; run = $run; ended = $how; whileSetupOpen = $whileOpen })

# --- 26 a silent install that closed the program and then changes nothing starts it again (review L1)
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$started = Start-Program
$closedId = $started.Process.Id
if (-not $started.Up) { [void] $failures.Add('the program did not start') }
Start-Sleep -Seconds 5
$before = Get-Installed
# (A file of the package that the program does not load, held so that it cannot be replaced: the setup finds it only
# once it has closed the program, which holds the program's own files.)
$manifest = Get-Content -LiteralPath (Join-Path $App 'package-manifest.json') -Raw | ConvertFrom-Json
$heldName = @($manifest.files | ForEach-Object { [string] $_.path } | Where-Object { $_ -like 'licenses/*' } | Sort-Object)[0]
$held = [IO.File]::Open((Join-Path $App ($heldName -replace '/', '\')), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
try {
    $run = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '26-closed-then-refused')`"" 180
} finally {
    $held.Dispose()
}
if ($run.Exit -ne 7) { [void] $failures.Add("the setup ended with $($run.Exit), not 7") }
Test-Unchanged $before $failures
$said = Read-Log '26-closed-then-refused'
if ($said -notmatch 'qKVM Desktop closed after \d+ ms\.') { [void] $failures.Add('the setup did not close the program first') }
if (Get-Process -Id $closedId -ErrorAction SilentlyContinue) { [void] $failures.Add('the program that ran was not closed') }
$resultLines = @(Get-ResultLines '26-closed-then-refused')
if ($resultLines.Count -ne 1 -or -not $resultLines[0].StartsWith('qKVM-RESULT: 7 nothing was changed: a file of qKVM Desktop is in use or cannot be replaced (', [StringComparison]::Ordinal) -or
    -not $resultLines[0].EndsWith('; qKVM Desktop, which this setup had closed, is started again', [StringComparison]::Ordinal)) {
    [void] $failures.Add("the setup's log does not have the one result line of a refusal after it closed the program ($($resultLines -join ' | '))")
}
if (-not (Wait-Until { Test-ProgramRunning } 30)) { [void] $failures.Add('the program that the setup closed was not started again') }
$how = Stop-Program
Test-Data $failures -ProgramStarted
Write-Step '26' 'closed-then-refused' $began $failures ([ordered]@{ run = $run; held = $heldName; resultLines = $resultLines; ended = $how })

# --- 27 a copy that does not hear the setup (the request's name taken first by another program): cannot be asked, never
# "did not answer"; a silent install changes nothing, and /FORCECLOSE=1 ends it only saying so plainly (review L2)
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$squatter = New-Object System.Threading.EventWaitHandle($false, [System.Threading.EventResetMode]::AutoReset, 'Local\qKVM-Desktop-Quit')
try {
    $started = Start-Program
    $deafId = $started.Process.Id
    if (-not $started.Up) { [void] $failures.Add('the program did not start') }
    Start-Sleep -Seconds 5
    if ([QkvmVm]::TitleOf([uint32] $deafId, 'qKVM Desktop quit answer')) { [void] $failures.Add('a copy whose request was taken first has an answer window') }
    $before = Get-Installed
    $run = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '27-cannot-be-asked')`"" 180
    if ($run.Exit -ne 7) { [void] $failures.Add("the setup ended with $($run.Exit), not 7") }
    if ($run.Seconds -gt 8) { [void] $failures.Add("the setup waited $($run.Seconds) s for a copy that cannot be asked") }
    Test-Unchanged $before $failures
    $resultLines = @(Get-ResultLines '27-cannot-be-asked')
    if ($resultLines.Count -ne 1 -or $resultLines[0] -cne 'qKVM-RESULT: 7 nothing was changed: qKVM Desktop is running, and this copy of it cannot be asked to close; /FORCECLOSE=1 ends it by force') {
        [void] $failures.Add("the setup's log does not have the one result line of a copy that cannot be asked ($($resultLines -join ' | '))")
    }
    if ((Read-Log '27-cannot-be-asked') -match 'Asked qKVM Desktop to close') { [void] $failures.Add('the setup asked a copy that has no answer window') }
    $forced = Invoke-Setup $Setup "$SilentArguments /FORCECLOSE=1 /LOG=`"$(Get-Log '27-cannot-be-asked-forced')`"" 180
    if ($forced.Exit -ne 0) { [void] $failures.Add("with /FORCECLOSE=1 the setup ended with $($forced.Exit)") }
    $said = Read-Log '27-cannot-be-asked-forced'
    if ($said -notmatch 'could not be asked to close, so what it is doing is not known; it is ended by force all the same, as /FORCECLOSE=1 says\.') {
        [void] $failures.Add('the setup ended a copy it could not ask without saying so plainly')
    }
    if (Get-Process -Id $deafId -ErrorAction SilentlyContinue) { [void] $failures.Add('with /FORCECLOSE=1 the program is still there') }
    $state = Test-Installed $newVersion $failures
    Test-StartedAgain '27-cannot-be-asked-forced' $failures $true
    $how = Stop-Program
} finally {
    $squatter.Dispose()
}
Test-Data $failures -ProgramStarted
Write-Step '27' 'cannot-be-asked' $began $failures ([ordered]@{ run = $run; resultLines = $resultLines; forced = $forced; ended = $how })
# --- 28 the interface busy for 6 s while the setup still waits, on a silent install told /FORCECLOSE=1: the request
# still stands, and the program ends the orderly way, never by force (review N1)
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$started = Start-Program
$busyId = $started.Process.Id
if (-not $started.Up) { [void] $failures.Add('the program did not start') }
Start-Sleep -Seconds 5
$script:stalled28 = [QkvmVm]::SuspendFirstThread($busyId)
if ($script:stalled28 -eq [IntPtr]::Zero) { [void] $failures.Add('the program''s interface could not be stalled') }
$script:resumedAt28 = $null
try {
    # (Its interface answers again 6 s into the setup: within the setup's 10 s.)
    $run = Invoke-Setup $Setup "$SilentArguments /FORCECLOSE=1 /LOG=`"$(Get-Log '28-busy-while-waiting')`"" 180 {
        param($process, $gone)
        if (-not $script:resumedAt28 -and $gone -ge 6) {
            [QkvmVm]::ResumeAndLetGo($script:stalled28)
            $script:stalled28 = [IntPtr]::Zero
            $script:resumedAt28 = $gone
        }
        return $false
    }
} finally {
    [QkvmVm]::ResumeAndLetGo($script:stalled28)
    $script:stalled28 = [IntPtr]::Zero
}
if ($run.Exit -ne 0) { [void] $failures.Add("the setup ended with $($run.Exit)") }
$said = Read-Log '28-busy-while-waiting'
if ($said -match 'ended by force') { [void] $failures.Add('the program was ended by force, though it would have ended the orderly way') }
if ($said -match 'stays open|too late') { [void] $failures.Add('the setup took the request for refused') }
if ($said -notmatch 'qKVM Desktop closed after \d+ ms\.') { [void] $failures.Add('the setup''s log does not say the program closed') }
$state = Test-Installed $newVersion $failures
Test-StartedAgain '28-busy-while-waiting' $failures $true
$how = Stop-Program
Test-Data $failures -ProgramStarted
Write-Step '28' 'busy-while-waiting' $began $failures ([ordered]@{ run = $run; resumedAfterSeconds = $script:resumedAt28; ended = $how })

# --- 29 to 31 (0.2.0): the earlier version set aside for the copy, put back where the copy does not complete, and what
# an update that did not finish left put right by the next setup first. Each from the older version installed again.
function Get-ProgramBoxText([int] $Id) {
    <# What a process's boxes say (each shown top-level window titled "qKVM" alone, and the texts in it); '' where it
    shows none. #>
    $said = New-Object System.Collections.ArrayList
    foreach ($top in [QkvmVm]::WindowsOf([uint32] $Id)) {
        if (-not [QkvmVm]::Answers($top) -or [QkvmVm]::TextOf($top) -cne 'qKVM') { continue }
        foreach ($inside in [QkvmVm]::ShownInside($top)) { [void] $said.Add([QkvmVm]::TextOf($inside)) }
    }
    return ($said -join "`n")
}
function Test-ShowsAWall([int] $Id) {
    <# Whether a process shows a window of a wall (titled "<name> - qKVM"). #>
    foreach ($top in [QkvmVm]::WindowsOf([uint32] $Id)) { if ([QkvmVm]::TextOf($top) -like '* - qKVM') { return $true } }
    return $false
}
function Push-BoxOk([int] $Id) {
    <# The OK of a process's box pressed; whether it had one. #>
    foreach ($top in [QkvmVm]::WindowsOf([uint32] $Id)) {
        foreach ($button in [QkvmVm]::ButtonsOf($top)) { if ([QkvmVm]::TextOf($button).Replace('&', '') -eq 'OK') { [QkvmVm]::Click($button); return $true } }
    }
    return $false
}
# (What the program says where an update did not finish, ui/win/unfinished_update.cpp; and what the setup writes into
# its marker, packaging/qkvm-desktop.iss.)
$UnfinishedWords = 'An update of qKVM did not finish. Choose OK, then run the qKVM setup again: it puts the earlier version back, or finishes the update.'
function Get-RecordText([string] $Ending, [string[]] $Asides) {
    <# The record of what a set-aside renamed, as the setup writes it into its marker (packaging/qkvm-desktop.iss,
    SetPackageAside; ui/win/unfinished_update.h reads it): its words, its kind, its ending, a line for each file, its end. #>
    $lines = @("qKVM Desktop $newVersion is being installed here; the version before it is set aside beside this file.", 'qKVM Desktop set-aside record 1', "ending $Ending")
    $lines += @($Asides | ForEach-Object { "aside $_" })
    $lines += 'end of the record'
    return (($lines -join "`r`n") + "`r`n")
}
$MarkerText = Get-RecordText $AsideEnding @('qkvm-desktop.exe', 'Qt6Core.dll')

# 29: a silent upgrade whose copy fails at its very end: the program's own file, the last the copy writes, taken by this
# test with no sharing once the copy has begun (the earlier one set aside by then), and let go of as the setup rolls
# back. The earlier version is back whole, and starts.
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$older = Reset-ToOlder '29' $failures
[void] (Test-Installed $oldVersion $failures)
$log = Get-Log '29-copy-fails-keeps-earlier'
$script:held29 = $null
$script:heldAt29 = $false
try {
    $run = Invoke-Setup $Setup "$SilentArguments /LOG=`"$log`"" 300 {
        param($process, $gone)
        $said = Read-Shared $log
        if (-not $script:heldAt29 -and $said.Contains('Dest filename:')) {
            try { $script:held29 = [IO.File]::Open($Program, [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None); $script:heldAt29 = $true } catch { }
        }
        if ($script:held29 -and $said.Contains('Rolling back changes.')) { $script:held29.Dispose(); $script:held29 = $null }
        return $false
    }
} finally {
    if ($script:held29) { $script:held29.Dispose() }
}
if (-not $script:heldAt29) { [void] $failures.Add('the program''s own file could not be taken during the copy') }
if ($run.Exit -eq 0) { [void] $failures.Add('the upgrade did not fail with the program''s own file held during its copy') }
if ((Read-Log '29-copy-fails-keeps-earlier') -notmatch 'The copy did not complete: the earlier version is whole again\.') {
    [void] $failures.Add('the setup''s log does not say the earlier version is whole again')
}
Test-Earlier $older $failures
$restarted = Start-Program
if (-not $restarted.Up) { [void] $failures.Add('after the failed upgrade, the earlier program does not start') }
$version = Get-Version $Program
if ($version -ne $oldVersion) { [void] $failures.Add("after the failed upgrade the program is version $version, not $oldVersion") }
$how = Stop-Program
$finish = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '29-finish')`""
if ($finish.Exit -ne 0) { [void] $failures.Add("the new setup did not install afterwards ($($finish.Exit))") }
[void] (Test-Installed $newVersion $failures)
Test-Data $failures -ProgramStarted
Write-Step '29' 'copy-fails-keeps-earlier' $began $failures ([ordered]@{ run = $run; held = 'qkvm-desktop.exe'; version = $version; ended = $how; finish = $finish })

# 30 (M3): the setup ended by force in the middle of its copy, as a power cut ends it: the update check is in the
# program's place, and a start of qKVM there says that an update did not finish; then the next setup puts the earlier
# version back first and installs. Then a cut later, the copy whole: the unfinished marker held by this test (no
# sharing of its deletion) so that the setup cannot make it that of a finishing update, and the setup ended by force
# while it tries again (its log says each try): the new version whole beside the marker and what was set aside, and a
# start of the program itself says the same, in its own box. The setup run again puts back first, and installs.
function Get-BoxOf([string] $File, [int] $Seconds = 30) {
    <# A start of $File: what its box said, whether it took the program's mutex, whether it showed a wall, and how it
    ended once OK was pressed. #>
    $process = Start-Process -FilePath $File -WorkingDirectory $App -PassThru
    $took = $false
    $said = ''
    $until = (Get-Clock) + $Seconds
    while ((Get-Clock) -lt $until -and -not $process.HasExited) {
        if (Test-ProgramRunning) { $took = $true }
        $said = Get-ProgramBoxText $process.Id
        if ($said) { break }
        Start-Sleep -Milliseconds 250
    }
    $wall = Test-ShowsAWall $process.Id
    $ok = Push-BoxOk $process.Id
    $exit = $null
    if ($process.WaitForExit(15000)) { $exit = $process.ExitCode } else { Stop-ById $process.Id }
    return [pscustomobject]@{ Said = $said; Took = ($took -or (Test-ProgramRunning)); Wall = $wall; Ok = $ok; Exit = $exit }
}
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$older = Reset-ToOlder '30' $failures
[void] (Test-Installed $oldVersion $failures)
$log = Get-Log '30-power-cut-recovered'
$run = Invoke-Setup $Setup "$SilentArguments /LOG=`"$log`"" 120 {
    param($process, $gone)
    $said = Read-Shared $log
    $at = $said.IndexOf('\Qt6Core.dll')
    return ($at -ge 0 -and $said.IndexOf('Dest filename:', $at) -ge 0) -or $gone -gt 60
}
if ($run.Exit -ne 'killed') { [void] $failures.Add("the setup was not ended in the middle of its copy (it ended with $($run.Exit))") }
Start-Sleep -Seconds 2
$cut = Get-Installed
$notKept = @(Get-NotKept $older $cut)
if ($notKept.Count) { [void] $failures.Add("files of the earlier version are neither there nor set aside as they were: $($notKept -join ', ')") }
$markerLeft = Test-Path -LiteralPath $UpdateMarker
if (-not $markerLeft) { [void] $failures.Add('there is no marker of an unfinished update beside what the copy set aside') }
$checkThere = Test-UpdateCheckAt $Program
if (-not $checkThere) { [void] $failures.Add("the update check of this PC's kind ($PcKind) is not in the program's place after a cut during the copy (at its name: $(Get-AtProgramName))") }
# (What a person meets: the Start menu's entry starts the file at the program's name.)
$checkBox = Get-BoxOf $Program
if (-not $checkBox.Said) { [void] $failures.Add('a start after the cut showed no box') }
elseif (-not $checkBox.Said.Contains($UnfinishedWords)) { [void] $failures.Add("the box after the cut does not say what to do: $($checkBox.Said)") }
if ($checkBox.Wall -or $checkBox.Took) { [void] $failures.Add('a start after the cut showed a wall or took the program''s mutex') }
if (-not $checkBox.Ok -or $checkBox.Exit -ne 1) { [void] $failures.Add("the box after the cut had no OK, or OK did not end it with 1 ($($checkBox.Exit))") }
$afterBox = Get-Installed
if (($cut.Files | ConvertTo-Json -Depth 4) -ne ($afterBox.Files | ConvertTo-Json -Depth 4)) { [void] $failures.Add('a start after the cut changed the program''s folder') }
$next = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '30-next-setup')`""
if ($next.Exit -ne 0) { [void] $failures.Add("the next setup ended with $($next.Exit)") }
if (-not (Test-PutBackFirst (Read-Log '30-next-setup'))) { [void] $failures.Add('the next setup did not put the earlier version back first, before it set it aside again') }
[void] (Test-Installed $newVersion $failures)
$started = Start-Program
if (-not $started.Up) { [void] $failures.Add('after the next setup, the new version does not start') }
Start-Sleep -Seconds 5
$how = @(Stop-Program)
# (A cut after the whole copy, before it is marked finished: the marker held from the start of the copy, so that the
# setup cannot rename it, and the setup ended while it tries again.)
[void] (Reset-ToOlder '30b' $failures)
$log = Get-Log '30-cut-before-finished'
$script:hold30 = $null
try {
    $late = Invoke-Setup $Setup "$SilentArguments /LOG=`"$log`"" 300 {
        param($process, $gone)
        $said = Read-Shared $log
        if (-not $script:hold30 -and $said.Contains('Dest filename:')) {
            try { $script:hold30 = [IO.File]::Open($UpdateMarker, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read) } catch { }
        }
        return $said.Contains('its marker could not be made that of a finishing update yet, and is tried again')
    }
} finally {
    if ($script:hold30) { $script:hold30.Dispose() }
}
if ($late.Exit -ne 'killed') { [void] $failures.Add("the setup was not ended as it tried to mark its whole copy finished (it ended with $($late.Exit))") }
Start-Sleep -Seconds 2
$whole = Get-Installed
if (-not (Test-Path -LiteralPath $UpdateMarker)) { [void] $failures.Add('the marker of an unfinished update is not there after the cut before the copy was marked finished') }
if ((Get-Version $Program) -ne $newVersion -or (Test-AnyUpdateCheckAt $Program)) { [void] $failures.Add('the new program is not in its place after a cut once the copy was whole') }
$ownBox = Get-BoxOf $Program
if (-not $ownBox.Said) { [void] $failures.Add('the program showed no box beside a whole copy not marked finished') }
elseif (-not $ownBox.Said.Contains($UnfinishedWords)) { [void] $failures.Add("the program's box does not say what to do: $($ownBox.Said)") }
if ($ownBox.Wall -or $ownBox.Took) { [void] $failures.Add('the program showed a wall or took its mutex beside a copy not marked finished') }
if (-not $ownBox.Ok -or $ownBox.Exit -ne 1) { [void] $failures.Add("the program's box had no OK, or OK did not end it with 1 ($($ownBox.Exit))") }
$afterOwnBox = Get-Installed
if (($whole.Files | ConvertTo-Json -Depth 4) -ne ($afterOwnBox.Files | ConvertTo-Json -Depth 4)) { [void] $failures.Add('the program changed its folder beside a copy not marked finished') }
$again = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '30-setup-again')`""
if ($again.Exit -ne 0) { [void] $failures.Add("the setup run again ended with $($again.Exit)") }
if (-not (Test-PutBackFirst (Read-Log '30-setup-again'))) { [void] $failures.Add('the setup run again did not put back what was set aside first') }
[void] (Test-Installed $newVersion $failures)
$started = Start-Program
if (-not $started.Up) { [void] $failures.Add('after the setup run again, the program does not start') }
$how += @(Stop-Program)
Test-Data $failures -ProgramStarted
Write-Step '30' 'power-cut-recovered' $began $failures ([ordered]@{ run = $run; notKept = $notKept; markerLeft = $markerLeft; updateCheckInPlace = $checkThere
    checkBox = $checkBox; next = $next; late = $late; ownBox = $ownBox; again = $again; ended = $how })

# 31: a file of the earlier version that Windows will not rename (this test, as the same user, denies itself deleting
# qt.conf and deleting in the program's folder: a rename needs either): Preparing to Install refuses, and puts back what
# it had set aside before it.
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$older = Reset-ToOlder '31' $failures
[void] (Test-Installed $oldVersion $failures)
$denied = Join-Path $App 'qt.conf'
$me = [Security.Principal.WindowsIdentity]::GetCurrent().User
$fileRule = New-Object System.Security.AccessControl.FileSystemAccessRule($me, [System.Security.AccessControl.FileSystemRights]::Delete, [System.Security.AccessControl.AccessControlType]::Deny)
$folderRule = New-Object System.Security.AccessControl.FileSystemAccessRule($me, [System.Security.AccessControl.FileSystemRights]::DeleteSubdirectoriesAndFiles,
    [System.Security.AccessControl.InheritanceFlags]::None, [System.Security.AccessControl.PropagationFlags]::None, [System.Security.AccessControl.AccessControlType]::Deny)
$fileInfo = New-Object IO.FileInfo($denied)
$folderInfo = New-Object IO.DirectoryInfo($App)
$fileDenied = $false
$folderDenied = $false
$renamed = $false
$run = $null
try {
    if (-not $fileInfo.Exists) { [void] $failures.Add('the earlier version has no qt.conf') }
    else {
        $acl = $fileInfo.GetAccessControl([System.Security.AccessControl.AccessControlSections]::Access)
        $acl.AddAccessRule($fileRule)
        $fileInfo.SetAccessControl($acl)
        $fileDenied = $true
        $acl = $folderInfo.GetAccessControl([System.Security.AccessControl.AccessControlSections]::Access)
        $acl.AddAccessRule($folderRule)
        $folderInfo.SetAccessControl($acl)
        $folderDenied = $true
        # (Proven to hold first: this user's own rename of it is refused.)
        try { [IO.File]::Move($denied, "$denied.probe"); $renamed = $true; [IO.File]::Move("$denied.probe", $denied) } catch { }
        if ($renamed) { [void] $failures.Add('qt.conf could still be renamed: the step proves nothing') }
        $run = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '31-rename-refused')`"" 180
    }
} finally {
    if ($folderDenied) { $acl = $folderInfo.GetAccessControl([System.Security.AccessControl.AccessControlSections]::Access); [void] $acl.RemoveAccessRuleSpecific($folderRule); $folderInfo.SetAccessControl($acl) }
    if ($fileDenied) { $acl = $fileInfo.GetAccessControl([System.Security.AccessControl.AccessControlSections]::Access); [void] $acl.RemoveAccessRuleSpecific($fileRule); $fileInfo.SetAccessControl($acl) }
}
if ($null -eq $run -or $run.Exit -ne 7) { [void] $failures.Add("the setup did not refuse with 7 (it ended with $(if ($run) { $run.Exit } else { 'nothing' }))") }
$resultLines = @(Get-ResultLines '31-rename-refused')
if ($resultLines.Count -ne 1 -or $resultLines[0] -cne 'qKVM-RESULT: 7 nothing was changed: a file of qKVM Desktop could not be set aside for the copy (qt.conf)') {
    [void] $failures.Add("the setup's log does not have the one result line of 7 for a file that could not be set aside ($($resultLines -join ' | '))")
}
Test-Earlier $older $failures
$restarted = Start-Program
if (-not $restarted.Up) { [void] $failures.Add('after the refused upgrade, the earlier program does not start') }
$how = Stop-Program
$finish = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '31-finish')`""
if ($finish.Exit -ne 0) { [void] $failures.Add("the new setup did not install afterwards ($($finish.Exit))") }
[void] (Test-Installed $newVersion $failures)
Test-Data $failures -ProgramStarted
Write-Step '31' 'rename-refused' $began $failures ([ordered]@{ run = $run; resultLines = $resultLines; ended = $how; finish = $finish })

# --- 32 and 33 (0.2.0): one copy of an account writes its profile, in whatever Windows session it runs. The lock of the
# file beside the settings (settings.json.running, which every start of 0.2.0 makes and locks) held here by this test,
# as a copy of this account in another session holds it (a Windows client lets an account have one session at a time,
# so the other session is played by the lock alone).
$LockFile = "$SettingsFile.running"
# 32: the uninstaller refuses as it begins, and removes nothing (it sees a copy the mutex does not show).
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$state = Test-Installed $newVersion $failures
if (-not (Test-Path -LiteralPath $LockFile)) { [void] $failures.Add('the program made no lock file beside its settings at its starts') }
$settingsBefore = if (Test-Path -LiteralPath $SettingsFile) { Get-Sha256 $SettingsFile } else { '' }
$before = Get-Installed
$holder = $null
$run = $null
$boxText = ''
$boxExit = $null
$boxShowedAWall = $false
try {
    $holder = [IO.File]::Open($LockFile, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    $holder.Lock(0, 1)
    if (Test-ProgramRunning) { [void] $failures.Add('the program runs in this session: the step needs it ended') }
    $run = Invoke-Setup (Join-Path $App 'unins000.exe') "$SilentArguments /LOG=`"$(Get-Log '32-uninstall-profile-held')`"" 120
    if ($run.Exit -eq 0) { [void] $failures.Add('the uninstaller ended with 0 while the profile''s lock was held') }
    if ((Read-Log '32-uninstall-profile-held') -notmatch 'qKVM Desktop is running for this Windows account \(here, or in another Windows session of it\): it is not uninstalled\.') {
        [void] $failures.Add('the uninstaller''s log does not say it refused for the profile''s lock')
    }
    $after = Get-Installed
    if (($before.Files | ConvertTo-Json -Depth 4) -ne ($after.Files | ConvertTo-Json -Depth 4) -or -not $after.Entry -or -not $after.StartMenu) {
        [void] $failures.Add('the uninstaller removed something while the profile''s lock was held')
    }
    Write-Step '32' 'uninstall-sees-profile-lock' $began $failures ([ordered]@{ run = $run; lockFile = $LockFile })

    # 33: a start of the program while the lock is held elsewhere and no setup is at work says so in a box, shows no
    # wall and writes nothing; OK ends it. Then, the lock let go of, the next start takes it.
    $began = [DateTime]::UtcNow
    $failures = New-Object System.Collections.ArrayList
    $process = Start-Process -FilePath $Program -WorkingDirectory $App -PassThru
    if (-not (Wait-Until { (Get-ProgramBoxText $process.Id) -ne '' } 30)) { [void] $failures.Add('the program showed no box while its profile was held elsewhere') }
    $boxText = Get-ProgramBoxText $process.Id
    if ($boxText -notmatch 'qKVM is already running for this Windows account, in another Windows session') { [void] $failures.Add("the box does not say a copy runs in another session: $boxText") }
    foreach ($top in [QkvmVm]::WindowsOf([uint32] $process.Id)) { if ([QkvmVm]::TextOf($top) -like '* - qKVM') { $boxShowedAWall = $true } }
    if ($boxShowedAWall) { [void] $failures.Add('a wall was shown beside the box') }
    $ok = $null
    foreach ($top in [QkvmVm]::WindowsOf([uint32] $process.Id)) {
        foreach ($button in [QkvmVm]::ButtonsOf($top)) { if ([QkvmVm]::TextOf($button).Replace('&', '') -eq 'OK') { $ok = $button } }
    }
    if ($ok) { [QkvmVm]::Click($ok) } else { [void] $failures.Add('the box has no OK') }
    if ($process.WaitForExit(15000)) { $boxExit = $process.ExitCode } else { Stop-ById $process.Id; [void] $failures.Add('the program did not end after its box') }
    if ($null -ne $boxExit -and $boxExit -ne 0) { [void] $failures.Add("the program ended with $boxExit") }
    $settingsAfter = if (Test-Path -LiteralPath $SettingsFile) { Get-Sha256 $SettingsFile } else { '' }
    if ($settingsAfter -ne $settingsBefore) { [void] $failures.Add('the settings were written by a start that did not take the profile''s lock') }
} finally {
    if ($holder) { $holder.Dispose() }
}
$started = Start-Program
if (-not $started.Up) { [void] $failures.Add('once the lock was let go of, the program did not start') }
Start-Sleep -Seconds 5
$taken = $false
try { $probe = [IO.File]::Open($LockFile, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite); try { $probe.Lock(0, 1); $probe.Unlock(0, 1) } catch { $taken = $true } finally { $probe.Dispose() } } catch { $taken = $true }
if (-not $taken) { [void] $failures.Add('the program that started holds no lock of its profile') }
$how = Stop-Program
Test-Data $failures -ProgramStarted
Write-Step '33' 'start-while-profile-held' $began $failures ([ordered]@{ box = $boxText; exit = $boxExit; ended = $how })

# --- 34 (0.2.0): the marker of an unfinished update whose record names nothing still set aside (what a cut leaves once
# the earlier version is whole again), and files of a set-aside ending that no record names (a person's, and one of the
# package's own name): the program starts as usual, takes the marker away, and leaves the others as they are; then a
# setup, which never takes them for its own (H1): it upgrades over them, sets aside under the next ending, and leaves
# them byte for byte. A marker left behind never keeps the program from starting, and a file of that ending never
# keeps a setup from finishing.
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
[void] (Test-Installed $newVersion $failures)
$strangers = @("notes.txt$AsideEnding", "Qt6Core.dll$AsideEnding")
foreach ($stranger in $strangers) { [IO.File]::WriteAllText((Join-Path $App $stranger), "put here by test_setup_vm.ps1: $stranger") }
$strangerSums = @{}
foreach ($stranger in $strangers) { $strangerSums[$stranger] = Get-Sha256 (Join-Path $App $stranger) }
# (As a put-back that went through leaves it where its marker could not be taken away: the set-aside had taken the
# third ending, and nothing of it is there now.)
[IO.File]::WriteAllText($UpdateMarker, (Get-RecordText ".3$AsideEnding" @('qkvm-desktop.exe', 'Qt6Core.dll')))
$started = Start-Program
if (-not $started.Up) { [void] $failures.Add('the program did not start beside a marker whose record names nothing still set aside') }
Start-Sleep -Seconds 5
$boxText = Get-ProgramBoxText $started.Process.Id
if ($boxText) { [void] $failures.Add("the program showed a box: $boxText") }
$markerLeft = Test-Path -LiteralPath $UpdateMarker
if ($markerLeft) { [void] $failures.Add('the marker whose record names nothing still set aside is still there') }
$how = Stop-Program
foreach ($stranger in $strangers) {
    $file = Join-Path $App $stranger
    if (-not (Test-Path -LiteralPath $file) -or (Get-Sha256 $file) -ne $strangerSums[$stranger]) { [void] $failures.Add("$stranger, which no record names, was touched by the program") }
}
$run = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '34-stale-marker-then-setup')`""
if ($run.Exit -ne 0) { [void] $failures.Add("the setup ended with $($run.Exit) beside files of a set-aside ending that no record names") }
$said = Read-Log '34-stale-marker-then-setup'
if ($said -notmatch [regex]::Escape("What was set aside ends in .2$AsideEnding, as its record says.")) { [void] $failures.Add('the setup did not set aside under the next ending, past a file of the first that is not its own') }
foreach ($stranger in $strangers) {
    $file = Join-Path $App $stranger
    if (-not (Test-Path -LiteralPath $file) -or (Get-Sha256 $file) -ne $strangerSums[$stranger]) { [void] $failures.Add("$stranger, which no record names, was touched by the setup") }
    Remove-Item -LiteralPath $file -Force -ErrorAction SilentlyContinue
}
[void] (Test-Installed $newVersion $failures)
Test-Data $failures -ProgramStarted
Write-Step '34' 'stale-marker' $began $failures ([ordered]@{ box = $boxText; markerLeft = $markerLeft; strangers = $strangers; run = $run; ended = $how })

# --- 35 (0.2.0, M4), ONLY WITH -ListedSetup (review i020-2, M2): what the earlier version installed in this very folder
# and this one has not, removed after the upgrade by its exact name, from that version's own list in the setup (made at
# build time from the list of what each published version installed). The published versions share their names (0.1.0
# to 0.2.0 do), so the setup that ships removes nothing here, and this step runs on a second setup of the same package,
# made by hand with -PublishedFiles naming a copy of packaging/published-files.json in which the older setup's version
# lists these as well (as the head of this test says): licenses/qkvm-vm-gone.txt, qkvm-vm-gone/a.txt,
# qkvm-vm-gone/deep/b.txt and qkvm-vm-kept/c.txt. This test puts them in as that version would have, and a person's
# qkvm-vm-kept/person.txt and licenses/qkvm-vm-person.txt beside them. After the upgrade: the four are gone, the folder
# qkvm-vm-gone with them (it is emptied), qkvm-vm-kept stays with the person's file in it, and the person's licence
# stays. Then the setup that ships installs over it again, so that the steps after it run on that setup alone.
if (-not $ListedSetup) {
    Write-NotRun '35' 'earlier-files-by-test-list' 'it needs -ListedSetup, the new setup made by hand with the test list (the head of this test says how); the setup that ships has no earlier version''s names to remove'
} else {
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
if ((Get-Version $ListedSetup) -ne $newVersion) { [void] $failures.Add("-ListedSetup is version $(Get-Version $ListedSetup), not $newVersion") }
[void] (Reset-ToOlder '35' $failures)
[void] (Test-Installed $oldVersion $failures)
$listed35 = @('licenses\qkvm-vm-gone.txt', 'qkvm-vm-gone\a.txt', 'qkvm-vm-gone\deep\b.txt', 'qkvm-vm-kept\c.txt')
$persons = @('qkvm-vm-kept\person.txt', 'licenses\qkvm-vm-person.txt')
foreach ($file in @($listed35) + @($persons)) {
    $at = Join-Path $App $file
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $at) | Out-Null
    Set-Content -LiteralPath $at -Value "put here by test_setup_vm.ps1: $file" -Encoding ASCII
}
$run = Invoke-Setup $ListedSetup "$SilentArguments /LOG=`"$(Get-Log '35-earlier-files-by-test-list')`""
if ($run.Exit -ne 0) { [void] $failures.Add("the setup ended with $($run.Exit)") }
$said = Read-Log '35-earlier-files-by-test-list'
# (With the kind of Windows that install was for, as the one setup for both kinds says it (1a93912a): this PC's.)
if ($said -notmatch [regex]::Escape("qKVM Desktop $oldVersion ($PcKind) was installed in this folder")) { [void] $failures.Add("the setup did not find the earlier version ($PcKind) installed in its own folder") }
$gone = @($listed35 | Where-Object { -not (Test-Path -LiteralPath (Join-Path $App $_)) })
if ($gone.Count -ne $listed35.Count) {
    [void] $failures.Add("what the older version's list names and this one has not is still there ($(@($listed35 | Where-Object { $gone -notcontains $_ }) -join ', ')): -ListedSetup is not made with the list this step names (the head of this test)")
}
if (Test-Path -LiteralPath (Join-Path $App 'qkvm-vm-gone')) { [void] $failures.Add('the folder qkvm-vm-gone, which removing the older version''s files emptied, is still there') }
$personsLeft = @($persons | Where-Object { Test-Path -LiteralPath (Join-Path $App $_) })
if ($personsLeft.Count -ne $persons.Count) { [void] $failures.Add("a person's file beside them was removed ($(@($persons | Where-Object { $personsLeft -notcontains $_ }) -join ', '))") }
foreach ($file in $persons) { Remove-Item -LiteralPath (Join-Path $App $file) -Force -ErrorAction SilentlyContinue }
Remove-Item -LiteralPath (Join-Path $App 'qkvm-vm-kept') -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath (Join-Path $App 'qkvm-vm-gone') -Recurse -Force -ErrorAction SilentlyContinue
[void] (Test-Installed $newVersion $failures)
# (The setup that ships over it again, for the steps after this one.)
$shipped = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '35-shipped-again')`""
if ($shipped.Exit -ne 0) { [void] $failures.Add("the setup that ships did not install over it again ($($shipped.Exit))") }
[void] (Test-Installed $newVersion $failures)
Test-Data $failures
Write-Step '35' 'earlier-files-by-test-list' $began $failures ([ordered]@{ listedSetup = [ordered]@{ file = Split-Path -Leaf $ListedSetup; sha256 = Get-Sha256 $ListedSetup }; run = $run; listed = $listed35; gone = $gone; personsKept = $personsLeft; shippedAgain = $shipped })
}

# --- 36 (0.2.0, H1): files of a set-aside ending beside the older version before an upgrade, which no record names (a
# person's, and one of a name of the package's with the first ending): a silent upgrade whose copy fails at the
# program's own file (held by this test as in 29) puts the earlier version back whole and leaves them byte for byte;
# the silent upgrade then goes through, setting aside under the next ending, and leaves them. Nothing of that ending
# ever wedges a setup.
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
[void] (Reset-ToOlder '36' $failures)
[void] (Test-Installed $oldVersion $failures)
$strangers = @("notes.txt$AsideEnding", "Qt6Core.dll$AsideEnding", "licenses\AGPL-3.0.txt$AsideEnding")
foreach ($stranger in $strangers) { [IO.File]::WriteAllText((Join-Path $App $stranger), "put here by test_setup_vm.ps1: $stranger") }
$older = Get-Installed
$log = Get-Log '36-stranger-copy-fails'
$script:held36 = $null
$script:heldAt36 = $false
try {
    $run = Invoke-Setup $Setup "$SilentArguments /LOG=`"$log`"" 300 {
        param($process, $gone)
        $said = Read-Shared $log
        if (-not $script:heldAt36 -and $said.Contains('Dest filename:')) {
            try { $script:held36 = [IO.File]::Open($Program, [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None); $script:heldAt36 = $true } catch { }
        }
        if ($script:held36 -and $said.Contains('Rolling back changes.')) { $script:held36.Dispose(); $script:held36 = $null }
        return $false
    }
} finally {
    if ($script:held36) { $script:held36.Dispose() }
}
if (-not $script:heldAt36) { [void] $failures.Add('the program''s own file could not be taken during the copy') }
if ($run.Exit -eq 0) { [void] $failures.Add('the upgrade did not fail with the program''s own file held during its copy') }
if ((Read-Log '36-stranger-copy-fails') -notmatch 'The copy did not complete: the earlier version is whole again\.') {
    [void] $failures.Add('the setup''s log does not say the earlier version is whole again beside files of that ending it does not own')
}
# (The earlier version byte for byte, and the files of that ending among it as they were.)
Test-Earlier $older $failures
$restarted = Start-Program
if (-not $restarted.Up) { [void] $failures.Add('after the failed upgrade, the earlier program does not start') }
$how = Stop-Program
$finish = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '36-stranger-upgrade')`""
if ($finish.Exit -ne 0) { [void] $failures.Add("the upgrade over files of that ending it does not own ended with $($finish.Exit)") }
if ((Read-Log '36-stranger-upgrade') -notmatch [regex]::Escape("What was set aside ends in .2$AsideEnding, as its record says.")) { [void] $failures.Add('the upgrade did not set aside under the next ending') }
foreach ($stranger in $strangers) {
    $key = $stranger.Replace('\', '/')
    $file = Join-Path $App $stranger
    if (-not (Test-Path -LiteralPath $file) -or (Get-Sha256 $file) -ne $older.Files[$key].sha256) { [void] $failures.Add("$stranger, which no record names, was touched by the upgrade") }
    Remove-Item -LiteralPath $file -Force -ErrorAction SilentlyContinue
}
[void] (Test-Installed $newVersion $failures)
Test-Data $failures -ProgramStarted
Write-Step '36' 'stranger-beside-upgrade' $began $failures ([ordered]@{ run = $run; strangers = $strangers; ended = $how; finish = $finish })

# --- 37 (0.2.0, M1): a copy that went on without one of its files does not count. Interactive, driven as a person
# would through UI Automation: the older version installed again; the upgrade, and once its copy has begun (the earlier
# version set aside), the name of the last file before the program taken by this test with no sharing, so that the copy
# cannot write it; Inno Setup's box offers Try again, Skip this file and Cancel installation; the name let go of, Skip
# this file chosen, as a person may. The copy goes on to its end, and the setup finds that file not as the package has
# it: it puts the earlier version back (every file of the package's byte for byte, nothing set aside, no marker), names
# its version in Settings > Apps again, says why in a box, and ends with 7. The earlier program starts.
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$older = Reset-ToOlder '37' $failures
[void] (Test-Installed $oldVersion $failures)
$manifest = Get-Content -LiteralPath (Join-Path $App 'package-manifest.json') -Raw | ConvertFrom-Json
$names = [string[]] @($manifest.files | ForEach-Object { [string] $_.path } | Where-Object { $_ -ne 'qkvm-desktop.exe' })
[Array]::Sort($names, [StringComparer]::Ordinal)
$heldName37 = $names[-1]
$log = Get-Log '37-skip-is-not-whole'
$script:stage37 = 0
$script:held37 = $null
$script:shown37 = New-Object System.Collections.ArrayList
try {
    $run = Invoke-Setup $Setup "/LOG=`"$log`"" 600 {
        param($process, $gone)
        if ($gone -lt 3) { return $false }
        switch ($script:stage37) {
            0 { $found = Find-SetupButton $process.Id 'Next'; if ($found) { Push-SetupButton $found; $script:stage37 = 1 } }
            1 { $found = Find-SetupButton $process.Id 'Install'; if ($found) { Push-SetupButton $found; $script:stage37 = 2 } }
            2 {
                if ((Read-Shared $log).Contains('Dest filename:')) {
                    try { $script:held37 = [IO.File]::Open((Join-Path $App ($heldName37 -replace '/', '\')), [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None); $script:stage37 = 3 } catch { }
                }
            }
            3 {
                $found = Find-SetupButton $process.Id 'Skip this file'
                if ($found) {
                    [void] $script:shown37.Add((Get-SetupText $process.Id))
                    $script:held37.Dispose()
                    $script:held37 = $null
                    Push-SetupButton $found
                    $script:stage37 = 4
                }
            }
            4 {
                $found = Find-SetupButton $process.Id 'OK'
                if ($found) { [void] $script:shown37.Add((Get-SetupText $process.Id)); Push-SetupButton $found; $script:stage37 = 5 }
            }
            5 { $found = Find-SetupButton $process.Id 'Finish'; if ($found) { Push-SetupButton $found; $script:stage37 = 6 } }
        }
        return $false
    }
} finally {
    if ($script:held37) { $script:held37.Dispose() }
}
if ($script:stage37 -lt 6) { [void] $failures.Add("the setup was not seen through by hand (stage $($script:stage37) of 6; it ended with $($run.Exit))") }
if ($run.Exit -ne 7) { [void] $failures.Add("the setup ended with $($run.Exit), not 7, after a copy that skipped a file") }
if ($script:shown37.Count -ge 2 -and $script:shown37[1] -notmatch [regex]::Escape("qKVM Desktop $newVersion was not installed: the copy was not whole (") ) {
    [void] $failures.Add('the setup did not say that its copy was not whole')
}
$said = Read-Log '37-skip-is-not-whole'
if ($said -notmatch 'The copy does not count: the copy was not whole \(' -or $said -notmatch 'The copy did not complete: the earlier version is whole again\.') {
    [void] $failures.Add('the setup''s log does not say the copy did not count and the earlier version is whole again')
}
if ($said -match 'Finished an update whose copy was whole') { [void] $failures.Add('the setup removed what it had set aside after a copy that skipped a file') }
# (The earlier version byte for byte, its version named again; the uninstaller Inno Setup wrote at the end of its copy is
# the newer one's, which uninstalls the earlier version as well, and is not looked at.)
$now = Get-Installed
foreach ($path in $older.Files.Keys) {
    if ($path -like 'unins000.*') { continue }
    if (-not $now.Files.Contains($path)) { [void] $failures.Add("$path of the earlier version is gone") }
    elseif ($now.Files[$path].sha256 -ne $older.Files[$path].sha256) { [void] $failures.Add("$path is not the earlier version's") }
}
foreach ($path in $now.Files.Keys) { if (-not $older.Files.Contains($path)) { [void] $failures.Add("$path is in the program's folder, and was not before") } }
if (-not $now.Entry -or $now.Entry.DisplayVersion -ne $oldVersion) { [void] $failures.Add("Settings > Apps names version $(if ($now.Entry) { $now.Entry.DisplayVersion }), not the earlier $oldVersion") }
$restarted = Start-Program
if (-not $restarted.Up) { [void] $failures.Add('after the copy that did not count, the earlier program does not start') }
$how = Stop-Program
$finish = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '37-finish')`""
if ($finish.Exit -ne 0) { [void] $failures.Add("the new setup did not install afterwards ($($finish.Exit))") }
[void] (Test-Installed $newVersion $failures)
Test-Data $failures -ProgramStarted
Write-Step '37' 'skip-is-not-whole' $began $failures ([ordered]@{ run = $run; held = $heldName37; stage = $script:stage37; shown = @($script:shown37); ended = $how; finish = $finish })

# --- 38 (0.2.0, M2, L1): what was set aside that cannot be removed after a whole copy never stops a setup. First every
# file set aside made read-only during the copy (a rename keeps the attribute, a delete refuses it): the upgrade ends 0
# and nothing set aside is left. Then one held open by this test with no sharing of its deletion, through the whole
# upgrade: it ends 0, the file stays, named in the finishing marker's record; the program starts as usual beside it.
# The next setup, with the file held still, finishes nothing of it, carries it into its own record, ends 0, and leaves
# a finishing marker naming it; let go of, the setup after removes it first, and nothing is left.
function Get-SetAside {
    <# The files of the program's folder of a set-aside ending, by their names under it. #>
    return @(Get-ChildItem -LiteralPath $App -Recurse -File -Force | Where-Object { $_.Name -like "*$AsideEnding" } | ForEach-Object { $_.FullName.Substring($App.Length + 1) })
}
$FinishingMarker = Join-Path $App 'qkvm-desktop.update-finishing'
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
[void] (Reset-ToOlder '38' $failures)
[void] (Test-Installed $oldVersion $failures)
$log = Get-Log '38-read-only-set-aside'
$script:madeReadOnly38 = 0
$run = Invoke-Setup $Setup "$SilentArguments /LOG=`"$log`"" 300 {
    param($process, $gone)
    if ($script:madeReadOnly38 -eq 0 -and (Read-Shared $log).Contains('Dest filename:')) {
        foreach ($name in Get-SetAside) {
            $file = Get-Item -LiteralPath (Join-Path $App $name) -Force
            $file.Attributes = $file.Attributes -bor [IO.FileAttributes]::ReadOnly
            $script:madeReadOnly38++
        }
    }
    return $false
}
if ($script:madeReadOnly38 -lt 10) { [void] $failures.Add("only $($script:madeReadOnly38) files set aside could be made read-only during the copy") }
if ($run.Exit -ne 0) { [void] $failures.Add("the upgrade ended with $($run.Exit) over files set aside made read-only") }
$leftReadOnly = @(Get-SetAside)
if ($leftReadOnly.Count) { [void] $failures.Add("files set aside are left after the upgrade, though only read-only: $($leftReadOnly -join ', ')") }
if ((Test-Path -LiteralPath $UpdateMarker) -or (Test-Path -LiteralPath $FinishingMarker)) { [void] $failures.Add('a marker is left after the upgrade over read-only files set aside') }
[void] (Test-Installed $newVersion $failures)
# (Then one that cannot be removed at all while it is held: the setup's delete refused for as long as this test holds it.)
[void] (Reset-ToOlder '38b' $failures)
$heldName38 = "Qt6Core.dll$AsideEnding"
$held38 = Join-Path $App $heldName38
$log = Get-Log '38-set-aside-held'
$script:hold38 = $null
try {
    $run = Invoke-Setup $Setup "$SilentArguments /LOG=`"$log`"" 300 {
        param($process, $gone)
        if (-not $script:hold38 -and (Test-Path -LiteralPath $held38)) {
            try { $script:hold38 = [IO.File]::Open($held38, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read) } catch { }
        }
        return $false
    }
    if (-not $script:hold38) { [void] $failures.Add("$heldName38 could not be held during the upgrade") }
    if ($run.Exit -ne 0) { [void] $failures.Add("the upgrade ended with $($run.Exit) where a file it set aside could not be removed") }
    $leftHeld = @(Get-SetAside)
    if (($leftHeld -join '|') -ne $heldName38) { [void] $failures.Add("after the upgrade what is set aside is not the held file alone: $($leftHeld -join ', ')") }
    if (-not (Test-Path -LiteralPath $FinishingMarker) -or -not ([IO.File]::ReadAllText($FinishingMarker)).Contains("aside Qt6Core.dll`r`n")) {
        [void] $failures.Add('the finishing marker does not name the file set aside that could not be removed')
    }
    if (Test-Path -LiteralPath $UpdateMarker) { [void] $failures.Add('the marker of an unfinished update is left after a whole upgrade') }
    $started = Start-Program
    if (-not $started.Up) { [void] $failures.Add('the program did not start beside what a finished update could not remove') }
    Start-Sleep -Seconds 3
    $boxText = Get-ProgramBoxText $started.Process.Id
    if ($boxText) { [void] $failures.Add("the program showed a box beside what a finished update could not remove: $boxText") }
    $how = Stop-Program
    # (The next setup, the file still held: nothing of it finished, carried into its own record, never in its way.)
    $again = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '38-next-setup-held')`""
    if ($again.Exit -ne 0) { [void] $failures.Add("the next setup ended with $($again.Exit) while the file left by the update before was still held") }
    $carried = if (Test-Path -LiteralPath $FinishingMarker) { [IO.File]::ReadAllText($FinishingMarker) } else { '' }
    if (-not $carried.Contains("left $AsideEnding Qt6Core.dll`r`n")) { [void] $failures.Add('the next setup did not carry the file it could not remove into its own record') }
    if (@(Get-SetAside).Count -ne 1) { [void] $failures.Add("after the next setup, what is set aside is not the held file alone: $(@(Get-SetAside) -join ', ')") }
} finally {
    if ($script:hold38) { $script:hold38.Dispose() }
}
# (Let go of: the setup after removes it first.)
$last = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '38-setup-after')`""
if ($last.Exit -ne 0) { [void] $failures.Add("the setup after ended with $($last.Exit)") }
if (@(Get-SetAside).Count -or (Test-Path -LiteralPath $FinishingMarker) -or (Test-Path -LiteralPath $UpdateMarker)) {
    [void] $failures.Add("something set aside, or a marker, is left after the setup after: $(@(Get-SetAside) -join ', ')")
}
[void] (Test-Installed $newVersion $failures)
Test-Data $failures -ProgramStarted
Write-Step '38' 'set-aside-not-removable' $began $failures ([ordered]@{ run = $run; madeReadOnly = $script:madeReadOnly38; held = $heldName38; again = $again; last = $last; ended = $how })

# --- 39 (0.2.0; review i020-2, M1): a first install that /DIR sends into a folder that holds what is not an install of
# ours is refused in Preparing to Install (exit 7, the result line) before anything is closed or changed, whatever the
# names there, and every file of that folder stays byte for byte, with nothing added: first another program's folder of
# names no package of ours has (in plugins and qml too), then one of names the package shares (vcruntime140.dll,
# msvcp140.dll, Qt6Core.dll, plugins\platforms\qwindows.dll), which the set-aside would have renamed and the copy written
# over before. Nothing in Settings > Apps for either. Then a first install by /DIR into an empty folder goes on as before
# (exit 0, the program there, no earlier version of ours found there), and its uninstall leaves nothing there. Nothing
# installed first (the uninstaller of the program's folder run), and the new version installed in its own folder again
# after.
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$uninstalled = Invoke-Setup (Join-Path $App 'unins000.exe') "$SilentArguments /LOG=`"$(Get-Log '39-uninstall-first')`"" 180
[void] (Wait-Until { -not (Test-Path -LiteralPath $App) } 30)
if ($uninstalled.Exit -ne 0 -or (Test-Path -LiteralPath $UserKey)) { [void] $failures.Add("qKVM Desktop could not be uninstalled first ($($uninstalled.Exit))") }
function Get-FolderSums([string] $Folder) {
    <# Every file under $Folder, by its path within it, with its SHA-256; and every folder within it, by its path. #>
    $sums = [ordered]@{}
    if (Test-Path -LiteralPath $Folder) {
        foreach ($item in @(Get-ChildItem -LiteralPath $Folder -Recurse -Force | Sort-Object FullName)) {
            $sums[$item.FullName.Substring($Folder.Length + 1)] = $(if ($item.PSIsContainer) { 'folder' } else { Get-Sha256 $item.FullName })
        }
    }
    return $sums
}
$refused39 = New-Object System.Collections.ArrayList
$kinds39 = @(
    [ordered]@{ Name = 'foreign-names'; Files = @('plugins\platforms\foreign.dll', 'qml\Foreign\qmldir', 'foreign.exe') },
    [ordered]@{ Name = 'shared-names'; Files = @('vcruntime140.dll', 'msvcp140.dll', 'Qt6Core.dll', 'plugins\platforms\qwindows.dll') })
foreach ($kind in $kinds39) {
    $foreign = Join-Path $Local "qkvm-vm-$($kind.Name)"
    foreach ($file in $kind.Files) {
        $at = Join-Path $foreign $file
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $at) | Out-Null
        Set-Content -LiteralPath $at -Value "another program's, put here by test_setup_vm.ps1: $file" -Encoding ASCII
    }
    $before = Get-FolderSums $foreign
    $logName = "39-dir-into-$($kind.Name)"
    $run = Invoke-Setup $Setup "$SilentArguments /DIR=`"$foreign`" /LOG=`"$(Get-Log $logName)`"" 180
    $lines = @(Get-ResultLines $logName)
    if ($run.Exit -ne 7) { [void] $failures.Add("the install into another program's folder ($($kind.Name)) did not refuse with 7 (it ended with $($run.Exit))") }
    if ($lines.Count -ne 1 -or $lines[0] -cnotmatch '\AqKVM-RESULT: 7 nothing was changed: the folder .+ holds what is not an install of qKVM Desktop \(.+\): qKVM Desktop is installed only into a folder of its own, or an empty one\z') {
        [void] $failures.Add("the setup's log does not have the one result line of 7 for another program's folder ($($kind.Name): $($lines -join ' | '))")
    }
    $after = Get-FolderSums $foreign
    if (($before | ConvertTo-Json) -cne ($after | ConvertTo-Json)) {
        [void] $failures.Add("another program's folder ($($kind.Name)) is not as it was: $(@($after.Keys) -join ', ')")
    }
    if (Test-Path -LiteralPath $UserKey) { [void] $failures.Add("the refused install ($($kind.Name)) left an entry in Settings > Apps") }
    if (Test-Path -LiteralPath $StartMenuEntry) { [void] $failures.Add("the refused install ($($kind.Name)) left a Start menu entry") }
    [void] $refused39.Add([ordered]@{ kind = $kind.Name; run = $run; resultLines = $lines; files = @($kind.Files) })
    Remove-Item -LiteralPath $foreign -Recurse -Force -ErrorAction SilentlyContinue
}
# (An empty folder by /DIR: a first install there, as before.)
$empty = Join-Path $Local 'qkvm-vm-empty'
New-Item -ItemType Directory -Force -Path $empty | Out-Null
$run = Invoke-Setup $Setup "$SilentArguments /DIR=`"$empty`" /LOG=`"$(Get-Log '39-dir-into-empty')`""
if ($run.Exit -ne 0) { [void] $failures.Add("the install into an empty folder ended with $($run.Exit)") }
if (-not (Test-Path -LiteralPath (Join-Path $empty 'qkvm-desktop.exe'))) { [void] $failures.Add('the install did not go into the empty folder /DIR named') }
if ((Read-Log '39-dir-into-empty') -match 'was installed in this folder') { [void] $failures.Add('the setup took an empty folder for one where qKVM was installed') }
$removed = Invoke-Setup (Join-Path $empty 'unins000.exe') "$SilentArguments /LOG=`"$(Get-Log '39-uninstall-empty')`"" 180
if ($removed.Exit -ne 0) { [void] $failures.Add("the uninstall from that folder ended with $($removed.Exit)") }
# (Until the uninstaller has removed itself: Inno Setup's second phase waits for its first process to end, then half a
# second, then deletes unins000.exe (Setup.Uninstall.pas, DeleteUninstallDataFiles), so that it is still there as that
# process ends. b5235009 looked at the folder then. And until the folder is gone or empty: a file whose delete is still
# pending is listed, and its sum cannot be read; what is left after the wait is judged below.)
[void] (Wait-Until { -not (Test-Path -LiteralPath $empty) -or -not @(Get-ChildItem -LiteralPath $empty -Recurse -Force).Count } 30)
$leftInEmpty = @((Get-FolderSums $empty).Keys)
if ($leftInEmpty.Count) { [void] $failures.Add("the uninstall left in that folder: $($leftInEmpty -join ', ')") }
Remove-Item -LiteralPath $empty -Recurse -Force -ErrorAction SilentlyContinue
$again = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '39-install-again')`""
if ($again.Exit -ne 0) { [void] $failures.Add("the new version did not install in its own folder again ($($again.Exit))") }
[void] (Test-Installed $newVersion $failures)
Test-Data $failures
Write-Step '39' 'dir-into-foreign-folder' $began $failures ([ordered]@{ uninstalled = $uninstalled; refused = @($refused39); intoEmpty = $run; removed = $removed; again = $again })

# --- 40 (0.2.0, M3): a cut while what was set aside is removed after a whole copy marked finished. Three files set
# aside held by this test (no sharing of their deletion) from the start of the copy, so that the removal stalls on them
# (each tried again for half a second), and the setup ended by force once its log says the first could not be removed:
# the finishing marker is left with what was set aside not yet removed, beside the new version whole; the program starts
# as usual (no box); let go of, the next setup removes the rest first (its log says so), installs, and nothing set
# aside or marked is left. Tried up to three times, as the cut must come within those moments.
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$FinishingMarker40 = Join-Path $App 'qkvm-desktop.update-finishing'
$held40 = @("Qt6Core.dll$AsideEnding", "Qt6Gui.dll$AsideEnding", "Qt6Network.dll$AsideEnding")
$cutDuring = $false
$tries40 = 0
while (-not $cutDuring -and $tries40 -lt 3) {
    $tries40++
    [void] (Reset-ToOlder "40-$tries40" $failures)
    $log = Get-Log "40-cut-during-finishing-$tries40"
    $script:holds40 = New-Object System.Collections.ArrayList
    try {
        $run = Invoke-Setup $Setup "$SilentArguments /LOG=`"$log`"" 300 {
            param($process, $gone)
            if ($script:holds40.Count -eq 0 -and (Read-Shared $log).Contains('Dest filename:')) {
                foreach ($name in $held40) {
                    try { [void] $script:holds40.Add([IO.File]::Open((Join-Path $App $name), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)) } catch { }
                }
            }
            # (The first of them refused for its half second: the copy is marked finished, and the removal under way.)
            return (Read-Shared $log).Contains("Could not remove yet what an update set aside: Qt6Core.dll$AsideEnding")
        }
    } finally {
        foreach ($hold in $script:holds40) { $hold.Dispose() }
    }
    $cutDuring = $run.Exit -eq 'killed' -and (Test-Path -LiteralPath $FinishingMarker40) -and @(Get-ChildItem -LiteralPath $App -Recurse -File -Force | Where-Object { $_.Name -like "*$AsideEnding" }).Count -gt 0
    if (-not $cutDuring) { Start-Sleep -Seconds 2; [void] (Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log "40-tidy-$tries40")`"") }
}
if (-not $cutDuring) { [void] $failures.Add("the setup could not be cut while it removed what was set aside, in $tries40 tries") }
else {
    if (Test-Path -LiteralPath $UpdateMarker) { [void] $failures.Add('the marker of an unfinished update is there after a cut during the finishing removal') }
    if ((Get-Version $Program) -ne $newVersion) { [void] $failures.Add('the new version is not in its place after a cut during the finishing removal') }
    $started = Start-Program
    if (-not $started.Up) { [void] $failures.Add('the program did not start after a cut during the finishing removal') }
    Start-Sleep -Seconds 3
    $boxText = Get-ProgramBoxText $started.Process.Id
    if ($boxText) { [void] $failures.Add("the program showed a box after a cut during the finishing removal: $boxText") }
    $how = Stop-Program
    $next = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '40-next-setup')`""
    if ($next.Exit -ne 0) { [void] $failures.Add("the next setup ended with $($next.Exit)") }
    $said = Read-Log '40-next-setup'
    $finishedFirst = $said.IndexOf('Finished an update whose copy was whole')
    $asideAgain = $said.IndexOf('Set the earlier version aside for the copy')
    if ($finishedFirst -lt 0 -or $asideAgain -lt 0 -or $finishedFirst -gt $asideAgain) { [void] $failures.Add('the next setup did not finish the earlier update first, before it set aside again') }
    if (@(Get-ChildItem -LiteralPath $App -Recurse -File -Force | Where-Object { $_.Name -like "*$AsideEnding" }).Count -or (Test-Path -LiteralPath $FinishingMarker40) -or (Test-Path -LiteralPath $UpdateMarker)) {
        [void] $failures.Add('something set aside, or a marker, is left after the next setup')
    }
}
[void] (Test-Installed $newVersion $failures)
Test-Data $failures -ProgramStarted
Write-Step '40' 'cut-during-finishing' $began $failures ([ordered]@{ tries = $tries40; cutDuring = $cutDuring; run = $run })

# --- 41 (0.2.0, L4): what an update that did not finish left, put back by a setup that then refuses as older than the
# version Settings > Apps names. The older version again, the upgrade cut in the middle of its copy (as 30), and
# Settings > Apps' entry made to name a version above this setup's (9.9.9, as a later version's would): the setup run
# then puts the earlier version back first, as every setup does before its refusal of an older version, and then
# refuses; its words say that it put the earlier version back and changed nothing else, never that nothing was changed;
# the folder is the earlier version byte for byte. The entry put back as it was, the setup then installs.
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$older = Reset-ToOlder '41' $failures
$log = Get-Log '41-cut'
$run = Invoke-Setup $Setup "$SilentArguments /LOG=`"$log`"" 120 {
    param($process, $gone)
    $said = Read-Shared $log
    $at = $said.IndexOf('\Qt6Core.dll')
    return ($at -ge 0 -and $said.IndexOf('Dest filename:', $at) -ge 0) -or $gone -gt 60
}
if ($run.Exit -ne 'killed') { [void] $failures.Add("the setup was not ended in the middle of its copy (it ended with $($run.Exit))") }
Start-Sleep -Seconds 2
$named41 = (Get-ItemProperty -LiteralPath $UserKey).DisplayVersion
Set-ItemProperty -LiteralPath $UserKey -Name DisplayVersion -Value '9.9.9'
try {
    $refused = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '41-put-back-then-refused')`"" 120
} finally {
    Set-ItemProperty -LiteralPath $UserKey -Name DisplayVersion -Value $named41
}
if ($refused.Exit -eq 0) { [void] $failures.Add('the setup did not refuse beside a later version named in Settings > Apps') }
$said = Read-Log '41-put-back-then-refused'
$back = $said.IndexOf('Put the earlier version back, which an update that did not finish had set aside')
$refusal = $said.IndexOf("qKVM Desktop 9.9.9 is installed, newer than this setup's $newVersion. First, this setup put back the earlier version, which an update that did not finish had set aside; nothing else was changed.")
if ($back -lt 0 -or $refusal -lt 0 -or $back -gt $refusal) { [void] $failures.Add('the setup did not put the earlier version back first and then say so in its refusal') }
if ($said.Contains("$newVersion. Nothing was changed.")) { [void] $failures.Add('the refusal after a put-back says that nothing was changed') }
# (The earlier version byte for byte; the entry is the one this test put back.)
Test-Earlier $older $failures
$finish = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '41-finish')`""
if ($finish.Exit -ne 0) { [void] $failures.Add("the new setup did not install afterwards ($($finish.Exit))") }
[void] (Test-Installed $newVersion $failures)
Test-Data $failures
Write-Step '41' 'put-back-then-refused' $began $failures ([ordered]@{ run = $run; refused = $refused; namedBefore = $named41; finish = $finish })

# --- 42 (0.2.0; outside review 1.1): a record of what an update set aside, put in another program's folder, is never
# acted on: whether the folder is one of the setup's own is decided before any record is. Nothing installed first (the
# uninstaller of the program's folder run). A folder of another program's holds a file of a name the package shares
# (Qt6Core.dll), a file of its own (foreign.exe), its own Qt6Core.dll.old-update, and beside them the marker of an
# unfinished update whose record, one the setup can read, names Qt6Core.dll as set aside and as what the copy writes. A
# silent setup told /DIR that folder does not begin (not 0), its log has the one result line of 7 saying that the folder
# holds what is not an install of qKVM Desktop, every file of that folder is byte for byte what it was, nothing added,
# and nothing is in Settings > Apps. The new version is then installed in its own folder again.
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$uninstalled42 = Invoke-Setup (Join-Path $App 'unins000.exe') "$SilentArguments /LOG=`"$(Get-Log '42-uninstall-first')`"" 180
[void] (Wait-Until { -not (Test-Path -LiteralPath $App) } 30)
if ($uninstalled42.Exit -ne 0 -or (Test-Path -LiteralPath $UserKey)) { [void] $failures.Add("qKVM Desktop could not be uninstalled first ($($uninstalled42.Exit))") }
$foreign42 = Join-Path $Local 'qkvm-vm-planted-record'
$files42 = [ordered]@{
    'Qt6Core.dll' = "another program's Qt6Core.dll, put here by test_setup_vm.ps1"
    "Qt6Core.dll$AsideEnding" = "another program's Qt6Core.dll$AsideEnding, put here by test_setup_vm.ps1"
    'foreign.exe' = "another program's foreign.exe, put here by test_setup_vm.ps1"
    'qkvm-desktop.update-unfinished' = "words`r`nqKVM Desktop set-aside record 1`r`nending $AsideEnding`r`naside Qt6Core.dll`r`ncopy Qt6Core.dll`r`ncopy qkvm-desktop.exe`r`nend of the record`r`n" }
New-Item -ItemType Directory -Force -Path $foreign42 | Out-Null
foreach ($name in $files42.Keys) { [IO.File]::WriteAllText((Join-Path $foreign42 $name), $files42[$name]) }
$before42 = Get-FolderSums $foreign42
$run = Invoke-Setup $Setup "$SilentArguments /DIR=`"$foreign42`" /LOG=`"$(Get-Log '42-dir-into-planted-record')`"" 180
$lines42 = @(Get-ResultLines '42-dir-into-planted-record')
if ($run.Exit -eq 0 -or $run.Exit -eq 'killed') { [void] $failures.Add("the setup into another program's folder with a record in it did not refuse (it ended with $($run.Exit))") }
if ($lines42.Count -ne 1 -or $lines42[0] -cnotmatch '\AqKVM-RESULT: 7 nothing was changed: an update that did not finish left what this setup cannot put right: the folder .+ holds what is not an install of qKVM Desktop \(.+\): qKVM Desktop is installed only into a folder of its own, or an empty one\z') {
    [void] $failures.Add("the setup's log does not have the one result line of 7 for a record in another program's folder: $($lines42 -join ' | ')")
}
$after42 = Get-FolderSums $foreign42
if (($before42 | ConvertTo-Json) -cne ($after42 | ConvertTo-Json)) { [void] $failures.Add("another program's folder with a record in it is not as it was: $(@($after42.Keys) -join ', ')") }
if (Test-Path -LiteralPath $UserKey) { [void] $failures.Add('the refused setup left an entry in Settings > Apps') }
Remove-Item -LiteralPath $foreign42 -Recurse -Force -ErrorAction SilentlyContinue
$again42 = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '42-install-again')`""
if ($again42.Exit -ne 0) { [void] $failures.Add("the new version did not install in its own folder again ($($again42.Exit))") }
[void] (Test-Installed $newVersion $failures)
Test-Data $failures
Write-Step '42' 'record-in-foreign-folder' $began $failures ([ordered]@{ uninstalled = $uninstalled42; run = $run; resultLines = $lines42; again = $again42 })

# --- 43 (0.2.0; outside review 1.8): what an uninstall could not remove of what an update had set aside never keeps a
# later setup out of qKVM's own folder. The older version again, then the upgrade with Qt6Core.dll.old-update held by
# this test (no sharing of its deletion), as 38: it ends 0, and that file stays, named in the finishing marker. Then the
# uninstall with the file still held: it ends 0, and the program's folder keeps that file and the finishing marker alone
# (no file of the package's). Let go of, the new setup, silent, into the program's own folder, which holds no install of
# ours now: it ends 0, its log says it removed what an uninstall had left of what an update set aside, nothing set aside
# and no marker is left, and the new version is installed whole.
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
[void] (Reset-ToOlder '43' $failures)
$held43 = Join-Path $App "Qt6Core.dll$AsideEnding"
$script:hold43 = $null
$left43 = @()
try {
    $run = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '43-upgrade-held')`"" 300 {
        param($process, $gone)
        if (-not $script:hold43 -and (Test-Path -LiteralPath $held43)) {
            try { $script:hold43 = [IO.File]::Open($held43, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read) } catch { }
        }
        return $false
    }
    if (-not $script:hold43) { [void] $failures.Add("Qt6Core.dll$AsideEnding could not be held during the upgrade") }
    if ($run.Exit -ne 0) { [void] $failures.Add("the upgrade ended with $($run.Exit) where a file it set aside could not be removed") }
    $removed43 = Invoke-Setup (Join-Path $App 'unins000.exe') "$SilentArguments /LOG=`"$(Get-Log '43-uninstall-held')`"" 180
    if ($removed43.Exit -ne 0) { [void] $failures.Add("the uninstall ended with $($removed43.Exit) where a file an update set aside was held") }
    # (Until the uninstaller has removed itself, after its first process has ended, as in 39: 02dd349d looked before.
    # And until the folder holds what it should, the two below: a file whose delete is still pending is listed. What it
    # holds after the wait is judged.)
    # (The two it should leave, sorted as the list read of the folder is: 02dd349d wrote them in an order of their own,
    # which unins000.exe, still there as the step looked, hid.)
    $list43 = { @(if (Test-Path -LiteralPath $App) { Get-ChildItem -LiteralPath $App -Recurse -File -Force | ForEach-Object { $_.FullName.Substring($App.Length + 1) } | Sort-Object }) }
    $expected43 = @(@("Qt6Core.dll$AsideEnding", 'qkvm-desktop.update-finishing') | Sort-Object) -join '|'
    [void] (Wait-Until { (@(& $list43) -join '|') -ceq $expected43 } 30)
    $left43 = @(& $list43)
    if (($left43 -join '|') -cne $expected43) {
        [void] $failures.Add("the uninstall did not leave the held file and the finishing marker alone: $($left43 -join ', ')")
    }
} finally {
    if ($script:hold43) { $script:hold43.Dispose() }
}
$after43 = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '43-setup-after-uninstall')`""
if ($after43.Exit -ne 0) { [void] $failures.Add("the setup after the uninstall ended with $($after43.Exit) in qKVM's own folder") }
if (-not (Read-Log '43-setup-after-uninstall').Contains('The folder holds no install of qKVM Desktop, only what a setup of it left: what the records name is removed, and nothing is put back.')) {
    [void] $failures.Add('the setup after the uninstall did not say it removed what the uninstall had left')
}
if (@(Get-SetAside).Count -or (Test-Path -LiteralPath $FinishingMarker) -or (Test-Path -LiteralPath $UpdateMarker)) {
    [void] $failures.Add("something set aside, or a marker, is left after the setup after the uninstall: $(@(Get-SetAside) -join ', ')")
}
[void] (Test-Installed $newVersion $failures)
Test-Data $failures
Write-Step '43' 'uninstall-left-aside' $began $failures ([ordered]@{ run = $run; leftByTheUninstall = $left43; after = $after43 })

# --- 44 (0.2.0; outside review 1.2): only into a folder that this Windows account and Windows itself alone may change.
# Nothing installed first. A silent setup told /DIR each of: an empty folder to which this test (its owner) gave the
# users of this PC the right to change it; a folder not made yet below one to which it gave them that right, for what is
# made in it; two folders not made yet, each below one to which it gave them a right for its files alone, (OI) without
# (CI): Modify on "files only" ((OI)(IO)), and Write on "this folder and files", which lets them remove neither that
# folder nor what is in it (Windows gives a new folder there that entry for its own files, and each folder below it the
# same, so that every file installed would be theirs to change; outside review 2, 1.2. Modify on "this folder and
# files" was refused before it, as it lets them remove the folder above, for which every folder above is judged); and a
# junction that leads to an empty folder. Each is refused (exit 7) with the one result line of 7 that says why (another account may change it; or
# a redirection), before anything is changed: nothing is in Settings > Apps, nothing is added to the folder, the folder
# not made yet is still not there, and the folder's own rights are as this test left them. (A case that installs all the
# same is uninstalled again, so that the cases and steps after it begin as they should.) Then the new version is
# installed in its own folder again, which passes as every other step shows.
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$uninstalled44 = Invoke-Setup (Join-Path $App 'unins000.exe') "$SilentArguments /LOG=`"$(Get-Log '44-uninstall-first')`"" 180
[void] (Wait-Until { -not (Test-Path -LiteralPath $App) } 30)
if ($uninstalled44.Exit -ne 0 -or (Test-Path -LiteralPath $UserKey)) { [void] $failures.Add("qKVM Desktop could not be uninstalled first ($($uninstalled44.Exit))") }
$shared44 = Join-Path $Local 'qkvm-vm-shared'
$sharedParent44 = Join-Path $Local 'qkvm-vm-shared-parent'
$filesParent44 = Join-Path $Local 'qkvm-vm-shared-files-parent'
$writeParent44 = Join-Path $Local 'qkvm-vm-shared-write-parent'
$target44 = Join-Path $Local 'qkvm-vm-junction-target'
$junction44 = Join-Path $Local 'qkvm-vm-junction'
foreach ($folder in $shared44, $sharedParent44, $filesParent44, $writeParent44, $target44) { New-Item -ItemType Directory -Force -Path $folder | Out-Null }
# (The users of this PC, by their SID, may change the folder and what is made in it: as an administrator's /DIR may name.
# And, for the next two, the files made in them alone.)
& icacls.exe $shared44 /grant '*S-1-5-32-545:(OI)(CI)M' | Out-Null
& icacls.exe $sharedParent44 /grant '*S-1-5-32-545:(OI)(CI)M' | Out-Null
& icacls.exe $filesParent44 /grant '*S-1-5-32-545:(OI)(IO)M' | Out-Null
& icacls.exe $writeParent44 /grant '*S-1-5-32-545:(OI)W' | Out-Null
& cmd.exe /c mklink /J "$junction44" "$target44" | Out-Null
$cases44 = @(
    [ordered]@{ Name = 'shared'; Dir = $shared44; Want = 'is not one that this Windows account and Windows itself alone may change \(the users of this PC \(BU\) may change ' },
    [ordered]@{ Name = 'below-shared'; Dir = (Join-Path $sharedParent44 'qKVM Desktop'); Want = 'is not one that this Windows account and Windows itself alone may change \(the users of this PC \(BU\) may change ' },
    [ordered]@{ Name = 'below-shared-files'; Dir = (Join-Path $filesParent44 'qKVM Desktop'); Want = 'is not one that this Windows account and Windows itself alone may change \(the users of this PC \(BU\) may change ' },
    [ordered]@{ Name = 'below-shared-write'; Dir = (Join-Path $writeParent44 'qKVM Desktop'); Want = 'is not one that this Windows account and Windows itself alone may change \(the users of this PC \(BU\) may change ' },
    [ordered]@{ Name = 'junction'; Dir = $junction44; Want = 'is a redirection \(a junction or a link\)' })
$refused44 = New-Object System.Collections.ArrayList
foreach ($case in $cases44) {
    # (Its rights only where it is there: icacls on a folder not made yet writes to its error stream, which Windows
    # PowerShell 5.1 turns into an exception under Stop, and the suite ended here, 2026-11-21.)
    $rightsBefore = if (Test-Path -LiteralPath $case.Dir) { (& icacls.exe $case.Dir 2>$null) -join "`n" } else { '' }
    $logName = "44-dir-$($case.Name)"
    $run = Invoke-Setup $Setup "$SilentArguments /DIR=`"$($case.Dir)`" /LOG=`"$(Get-Log $logName)`"" 180
    $lines = @(Get-ResultLines $logName)
    if ($run.Exit -ne 7) { [void] $failures.Add("the install into $($case.Name) did not refuse with 7 (it ended with $($run.Exit))") }
    if ($lines.Count -ne 1 -or $lines[0] -cnotmatch "\AqKVM-RESULT: 7 nothing was changed: the folder .+ $($case.Want).*\z") {
        [void] $failures.Add("the setup's log does not have the one result line of 7 for $($case.Name): $($lines -join ' | ')")
    }
    if (Test-Path -LiteralPath $UserKey) {
        [void] $failures.Add("the refused install into $($case.Name) left an entry in Settings > Apps")
        # (Installed all the same: uninstalled from where it went, so that what follows begins with nothing installed.)
        $went = [string] (Get-ItemProperty -LiteralPath $UserKey).InstallLocation
        if ($went) {
            [void] (Invoke-Setup (Join-Path $went 'unins000.exe') "$SilentArguments /LOG=`"$(Get-Log "$logName-uninstall")`"" 180)
            # (Until the folder is gone or empty, not only its uninstaller: the looks below are at that folder.)
            [void] (Wait-Until { -not (Test-Path -LiteralPath $went) -or -not @(Get-ChildItem -LiteralPath $went -Recurse -Force).Count } 30)
        }
    }
    if ($case.Name -like 'below-shared*') {
        if (Test-Path -LiteralPath $case.Dir) { [void] $failures.Add("the refused install made its folder below the shared one ($($case.Name))") }
    } else {
        $inside = @(Get-ChildItem -LiteralPath $case.Dir -Force -ErrorAction SilentlyContinue)
        if ($inside.Count) { [void] $failures.Add("the refused install put something in $($case.Name): $(@($inside | ForEach-Object Name) -join ', ')") }
        if (((& icacls.exe $case.Dir 2>$null) -join "`n") -cne $rightsBefore) { [void] $failures.Add("the refused install changed the rights of $($case.Name)") }
    }
    [void] $refused44.Add([ordered]@{ case = $case.Name; run = $run; resultLines = $lines })
}
& cmd.exe /c rmdir "$junction44" | Out-Null
foreach ($folder in $shared44, $sharedParent44, $filesParent44, $writeParent44, $target44) { Remove-Item -LiteralPath $folder -Recurse -Force -ErrorAction SilentlyContinue }
$again44 = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '44-install-again')`""
if ($again44.Exit -ne 0) { [void] $failures.Add("the new version did not install in its own folder again ($($again44.Exit))") }
[void] (Test-Installed $newVersion $failures)
Test-Data $failures
Write-Step '44' 'folder-others-may-change' $began $failures ([ordered]@{ uninstalled = $uninstalled44; refused = @($refused44); again = $again44 })

# --- 45 (0.2.0; outside review 1.3): one setup or uninstall at a time for the Windows account, in whatever session. This
# test holds the account's gate (Global\qKVM-Desktop-Setup-Account-<its SID>, owned) as a setup at work in another Windows
# session of the account holds it. A silent setup then does not begin (not 0) within a few seconds, with the one result
# line of 7 saying another setup or uninstall is at work for this account, and the uninstaller does not begin either
# (not 0); the new version is still installed byte for byte. Let go of, the setup ends 0, and the uninstaller would too
# (step 46 runs it).
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$Sid = [System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$GateName = "Global\qKVM-Desktop-Setup-Account-$Sid"
$before45 = Get-Installed
$made45 = $false
$gate45 = New-Object System.Threading.Mutex($true, $GateName, [ref] $made45)
try {
    if (-not $made45) { [void] $failures.Add('the account''s gate was there already before this test took it') }
    $run = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '45-setup-while-gate-held')`"" 120
    $lines45 = @(Get-ResultLines '45-setup-while-gate-held')
    if ($run.Exit -eq 0 -or $run.Exit -eq 'killed' -or $run.Exit -eq 'timeout') { [void] $failures.Add("the setup did not refuse while another held the account's gate (it ended with $($run.Exit))") }
    if ($run.Seconds -gt 30) { [void] $failures.Add("the setup took $($run.Seconds) s to refuse, not a few") }
    if ($lines45.Count -ne 1 -or $lines45[0] -cne 'qKVM-RESULT: 7 nothing was changed: another setup or uninstall of qKVM Desktop is at work for this Windows account, here or in another Windows session of it') {
        [void] $failures.Add("the setup's log does not have the one result line of 7 for the account's gate held: $($lines45 -join ' | ')")
    }
    $removed45 = Invoke-Setup (Join-Path $App 'unins000.exe') "$SilentArguments /LOG=`"$(Get-Log '45-uninstall-while-gate-held')`"" 120
    if ($removed45.Exit -eq 0) { [void] $failures.Add('the uninstaller did not refuse while another held the account''s gate') }
    if (-not (Read-Log '45-uninstall-while-gate-held').Contains('another setup or uninstall of qKVM Desktop is at work for this Windows account')) { [void] $failures.Add('the uninstaller''s log does not say why it did not begin') }
    Test-Earlier $before45 $failures
} finally {
    $gate45.ReleaseMutex()
    $gate45.Dispose()
}
$after45 = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '45-setup-after')`""
if ($after45.Exit -ne 0) { [void] $failures.Add("the setup ended with $($after45.Exit) once the account's gate was let go of") }
[void] (Test-Installed $newVersion $failures)
Test-Data $failures
Write-Step '45' 'account-gate' $began $failures ([ordered]@{ gate = $GateName; run = $run; resultLines = $lines45; uninstall = $removed45; after = $after45 })

# --- 46 (0.2.0; outside review 1.3): an uninstall before qKVM has ever run for the account holds the lock of its
# profile all the same. The program not running and its lock file settings.json.running taken away, as before a first
# start of 0.2.0: during the uninstall this test finds the lock held (it tries to take it every quarter of a second, and
# lets go at once), the uninstall ends 0 and says it removed the lock it had made, the file is not there afterwards, and
# the program's data is as it was. Then the new version is installed again.
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$LockFile46 = Join-Path $Local 'QKVM Desktop\settings.json.running'
$how = Stop-Program
Remove-Item -LiteralPath $LockFile46 -Force -ErrorAction SilentlyContinue
if (Test-Path -LiteralPath $LockFile46) { [void] $failures.Add('the lock file could not be taken away first') }
$script:seenHeld46 = 0
# (Looked at every 10 ms from beside, while the uninstall runs: it holds the lock for as long as it works, about a
# quarter of a second on a test VM, which a look every quarter of a second missed (9b470a8c). A read of the file's
# first byte, which the lock refuses, never a lock of this test's own, which could stand in the uninstall's way.)
$stop46 = New-Object System.Threading.ManualResetEvent($false)
$probe46 = [PowerShell]::Create()
[void] $probe46.AddScript({
        param([string] $Path, [System.Threading.ManualResetEvent] $Stop)
        $seen = 0
        while (-not $Stop.WaitOne(10)) {
            if ([IO.File]::Exists($Path)) {
                try {
                    $file = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare] ([int] [IO.FileShare]::ReadWrite -bor [int] [IO.FileShare]::Delete))
                    try { $one = New-Object byte[] 1; [void] $file.Read($one, 0, 1) } catch [System.IO.IOException] { $seen++ } finally { $file.Dispose() }
                } catch { }
            }
        }
        $seen
    }).AddArgument($LockFile46).AddArgument($stop46)
$looking46 = $probe46.BeginInvoke()
try {
    $removed46 = Invoke-Setup (Join-Path $App 'unins000.exe') "$SilentArguments /LOG=`"$(Get-Log '46-uninstall-before-first-start')`"" 180
} finally {
    [void] $stop46.Set()
    $script:seenHeld46 = [int] (@($probe46.EndInvoke($looking46)) | Select-Object -Last 1)
    $probe46.Dispose()
    $stop46.Dispose()
}
[void] (Wait-Until { -not (Test-Path -LiteralPath $App) } 30)
if ($removed46.Exit -ne 0) { [void] $failures.Add("the uninstall ended with $($removed46.Exit)") }
if ($script:seenHeld46 -lt 1) { [void] $failures.Add('the lock of the profile was never seen held during the uninstall') }
if (-not (Read-Log '46-uninstall-before-first-start').Contains('Removed the lock of the profile, which this uninstall had made')) { [void] $failures.Add('the uninstall did not say it removed the lock it had made') }
if (Test-Path -LiteralPath $LockFile46) { [void] $failures.Add('the uninstall left the lock it had made') }
Test-Data $failures
$again46 = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '46-install-again')`""
if ($again46.Exit -ne 0) { [void] $failures.Add("the new version did not install again ($($again46.Exit))") }
[void] (Test-Installed $newVersion $failures)
Write-Step '46' 'uninstall-before-first-start' $began $failures ([ordered]@{ seenHeld = $script:seenHeld46; uninstall = $removed46; again = $again46; ended = $how })

# --- 47 (0.2.0; outside review 1.4): a start of the program takes away a stale marker of an update only while it holds the
# account's gate. A marker beside the new version whose record names Qt6Core.dll as set aside, with no such file beside
# it (what a setup that has just written its record, and renamed nothing yet, has there); this test holds the account's
# gate as that setup does. The program starts as usual (no box), and the marker is still there. The gate let go of, the
# next start takes the marker away.
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$record47 = "qKVM Desktop $newVersion is being installed here; the version before it is set aside beside this file.`r`nqKVM Desktop set-aside record 1`r`nending $AsideEnding`r`naside Qt6Core.dll`r`ncopy Qt6Core.dll`r`nend of the record`r`n"
[IO.File]::WriteAllText($UpdateMarker, $record47)
$made47 = $false
$gate47 = New-Object System.Threading.Mutex($true, $GateName, [ref] $made47)
try {
    $started = Start-Program
    if (-not $started.Up) { [void] $failures.Add('the program did not start beside a stale marker while the account''s gate was held') }
    Start-Sleep -Seconds 3
    $boxText = Get-ProgramBoxText $started.Process.Id
    if ($boxText) { [void] $failures.Add("the program showed a box: $boxText") }
    if (-not (Test-Path -LiteralPath $UpdateMarker)) { [void] $failures.Add('the program took the marker away while the account''s gate was held') }
    $how = Stop-Program
} finally {
    $gate47.ReleaseMutex()
    $gate47.Dispose()
}
$again47 = Start-Program
if (-not $again47.Up) { [void] $failures.Add('the program did not start once the account''s gate was let go of') }
[void] (Wait-Until { -not (Test-Path -LiteralPath $UpdateMarker) } 15)
if (Test-Path -LiteralPath $UpdateMarker) { [void] $failures.Add('the program did not take the stale marker away once the account''s gate was free') }
$how = Stop-Program
[void] (Test-Installed $newVersion $failures)
Test-Data $failures -ProgramStarted
Write-Step '47' 'stale-marker-while-gate-held' $began $failures ([ordered]@{ gate = $GateName; heldRun = $started.Up; box = $boxText; ended = $how })

# --- 48 (0.2.0; outside review 1.5): a put-back counts as whole only where it saw every file its record names. The older
# version again; a silent upgrade whose copy fails at its very end (the program's own file held, as 29), and, once the
# copy has begun, plugins\platforms\qwindows.dll.old-update made one that this account may not look at (this test, its
# owner, denies itself reading its attributes and listing its folder). The setup ends not 0, its log says that file could
# not be looked at and that the earlier version could not all be put back, the marker of an unfinished update is still
# there and its record still names it, and the update check stands at the program's name. Let it be looked at again (the
# denials taken away), the next setup puts the earlier version back first, then installs, and the new version is whole.
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
[void] (Reset-ToOlder '48' $failures)
$unseen48 = Join-Path $App "plugins\platforms\qwindows.dll$AsideEnding"
$unseenFolder48 = Join-Path $App 'plugins\platforms'
$log = Get-Log '48-unseen-aside'
$script:held48 = $null
$script:heldAt48 = $false
$script:denied48 = $false
try {
    $run = Invoke-Setup $Setup "$SilentArguments /LOG=`"$log`"" 300 {
        param($process, $gone)
        $said = Read-Shared $log
        # (Taken once only, as 29 does: let go of at the rollback, it is never taken again, so that the put-back can put the
        # update check in the program's place.)
        if (-not $script:heldAt48 -and $said.Contains('Dest filename:')) {
            try { $script:held48 = [IO.File]::Open($Program, [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None); $script:heldAt48 = $true } catch { }
            # (The denial asked first: once made, the file may not be looked at, and Test-Path would throw.)
            if (-not $script:denied48 -and (Test-Path -LiteralPath $unseen48)) {
                & icacls.exe $unseen48 /deny "*${Sid}:(RA)" | Out-Null
                & icacls.exe $unseenFolder48 /deny "*${Sid}:(RD)" | Out-Null
                $script:denied48 = $true
            }
        }
        if ($script:held48 -and $said.Contains('Rolling back changes.')) { $script:held48.Dispose(); $script:held48 = $null }
        return $false
    }
} finally {
    if ($script:held48) { $script:held48.Dispose() }
}
$said48 = Read-Log '48-unseen-aside'
$markerLeft48 = Test-Path -LiteralPath $UpdateMarker
$checkInPlace48 = Test-UpdateCheckAt $Program
if ($script:denied48) {
    & icacls.exe $unseenFolder48 /remove:d "*$Sid" | Out-Null
    & icacls.exe $unseen48 /remove:d "*$Sid" | Out-Null
}
if (-not $script:denied48) { [void] $failures.Add('the file set aside could not be made one this account may not look at during the copy') }
if ($run.Exit -eq 0) { [void] $failures.Add('the upgrade did not fail with the program''s own file held') }
if (-not $said48.Contains("what an update set aside could not be looked at (plugins\platforms\qwindows.dll$AsideEnding)")) { [void] $failures.Add('the setup''s log does not say what it could not look at') }
if ($said48 -match 'the earlier version is whole again') { [void] $failures.Add('the setup said the earlier version was whole though it could not look at a file its record names') }
if (-not $markerLeft48) { [void] $failures.Add('the marker of the unfinished update did not stay') }
elseif (-not ([IO.File]::ReadAllText($UpdateMarker)).Contains("aside plugins\platforms\qwindows.dll`r`n")) { [void] $failures.Add('the marker''s record no longer names the file it could not look at') }
if (-not (Test-Path -LiteralPath $unseen48)) { [void] $failures.Add('the file set aside that could not be looked at is gone') }
if (-not $checkInPlace48) { [void] $failures.Add('the update check does not stand at the program''s name') }
$next48 = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '48-next-setup')`""
if ($next48.Exit -ne 0) { [void] $failures.Add("the next setup ended with $($next48.Exit)") }
if (-not (Test-PutBackFirst (Read-Log '48-next-setup'))) { [void] $failures.Add('the next setup did not put the earlier version back first') }
[void] (Test-Installed $newVersion $failures)
Test-Data $failures
Write-Step '48' 'unseen-aside-stays-named' $began $failures ([ordered]@{ run = $run; denied = $script:denied48; markerLeft = $markerLeft48; updateCheckInPlace = $checkInPlace48; next = $next48 })

# --- 49 (0.2.0; outside review 1.6): a copy that cannot all be written to the disk does not count, and nothing set aside
# goes. The older version again; a silent upgrade, and once its copy has written Qt6Core.dll (the next file begun), that
# new file held by this test for reading, sharing reading alone, so that Windows' flush of it, which writes, is refused;
# let go of once the log says it could not be flushed. The setup ends with 7, its log says the copy could not all be
# written to the disk (Qt6Core.dll) and that the earlier version is whole again, and never that it finished the update;
# every file of the earlier version is back byte for byte, Settings > Apps names its version, and it starts.
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$older = Reset-ToOlder '49' $failures
$log = Get-Log '49-flush-refused'
$script:held49 = $null
$script:heldAt49 = $false
try {
    $run = Invoke-Setup $Setup "$SilentArguments /LOG=`"$log`"" 300 {
        param($process, $gone)
        $said = Read-Shared $log
        $at = $said.IndexOf('\Qt6Core.dll')
        if (-not $script:heldAt49 -and $at -ge 0 -and $said.IndexOf('Dest filename:', $at) -ge 0) {
            try { $script:held49 = [IO.File]::Open((Join-Path $App 'Qt6Core.dll'), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read); $script:heldAt49 = $true } catch { }
        }
        if ($script:held49 -and $said.Contains('could not be flushed to the disk before the copy is marked finished')) { $script:held49.Dispose(); $script:held49 = $null }
        return $false
    }
} finally {
    if ($script:held49) { $script:held49.Dispose() }
}
if (-not $script:heldAt49) { [void] $failures.Add('the new Qt6Core.dll could not be held during the copy') }
if ($run.Exit -ne 7) { [void] $failures.Add("the setup ended with $($run.Exit), not 7, where a file of its copy could not be written to the disk") }
$said = Read-Log '49-flush-refused'
if (-not $said.Contains('The copy does not count: the copy could not all be written to the disk (Qt6Core.dll).')) { [void] $failures.Add('the setup''s log does not say its copy could not all be written to the disk') }
if ($said -notmatch 'The copy did not complete: the earlier version is whole again\.') { [void] $failures.Add('the setup''s log does not say the earlier version is whole again') }
if ($said -match 'Finished an update whose copy was whole') { [void] $failures.Add('the setup removed what it had set aside though its copy was not on the disk') }
$now = Get-Installed
foreach ($path in $older.Files.Keys) {
    if ($path -like 'unins000.*') { continue }
    if (-not $now.Files.Contains($path)) { [void] $failures.Add("$path of the earlier version is gone") }
    elseif ($now.Files[$path].sha256 -ne $older.Files[$path].sha256) { [void] $failures.Add("$path is not the earlier version's") }
}
foreach ($path in $now.Files.Keys) { if (-not $older.Files.Contains($path)) { [void] $failures.Add("$path is in the program's folder, and was not before") } }
if (-not $now.Entry -or $now.Entry.DisplayVersion -ne $oldVersion) { [void] $failures.Add("Settings > Apps names version $(if ($now.Entry) { $now.Entry.DisplayVersion }), not the earlier $oldVersion") }
$restarted = Start-Program
if (-not $restarted.Up) { [void] $failures.Add('after the copy that could not be written to the disk, the earlier program does not start') }
$how = Stop-Program
$finish = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '49-finish')`""
if ($finish.Exit -ne 0) { [void] $failures.Add("the new setup did not install afterwards ($($finish.Exit))") }
[void] (Test-Installed $newVersion $failures)
Test-Data $failures -ProgramStarted
Write-Step '49' 'flush-refused-keeps-earlier' $began $failures ([ordered]@{ run = $run; held = 'Qt6Core.dll'; ended = $how; finish = $finish })

# --- 50 (0.2.0; outside review 1.7): a first install whose copy does not count starts nothing. Interactive, by UI
# Automation, as 37: nothing installed; the new setup by hand (Next, Next, Install), and once its copy has begun, the name
# of the last file before the program taken by this test with no sharing; Skip this file chosen once it is let go of. The
# setup's box says it was not installed; the last page offers no "Start qKVM Desktop", and Finish starts nothing. The
# setup ends with 7. Then the new setup, silent, installs whole.
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$manifest50 = Get-Content -LiteralPath (Join-Path $App 'package-manifest.json') -Raw | ConvertFrom-Json
$names50 = [string[]] @($manifest50.files | ForEach-Object { [string] $_.path } | Where-Object { $_ -ne 'qkvm-desktop.exe' })
[Array]::Sort($names50, [StringComparer]::Ordinal)
$heldName50 = $names50[-1]
$uninstalled50 = Invoke-Setup (Join-Path $App 'unins000.exe') "$SilentArguments /LOG=`"$(Get-Log '50-uninstall-first')`"" 180
[void] (Wait-Until { -not (Test-Path -LiteralPath $App) } 30)
if ($uninstalled50.Exit -ne 0 -or (Test-Path -LiteralPath $UserKey)) { [void] $failures.Add("qKVM Desktop could not be uninstalled first ($($uninstalled50.Exit))") }
$log = Get-Log '50-first-install-skip'
$script:stage50 = 0
$script:held50 = $null
$script:shown50 = New-Object System.Collections.ArrayList
try {
    $run = Invoke-Setup $Setup "/LOG=`"$log`"" 600 {
        param($process, $gone)
        if ($gone -lt 3) { return $false }
        switch ($script:stage50) {
            0 { $found = Find-SetupButton $process.Id 'Next'; if ($found) { Push-SetupButton $found; $script:stage50 = 1 } }
            1 {
                $found = Find-SetupButton $process.Id 'Install'
                if ($found) { Push-SetupButton $found; $script:stage50 = 2 }
                else { $found = Find-SetupButton $process.Id 'Next'; if ($found) { Push-SetupButton $found } }
            }
            2 {
                if ((Read-Shared $log).Contains('Dest filename:')) {
                    try { $script:held50 = [IO.File]::Open((Join-Path $App ($heldName50 -replace '/', '\')), [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None); $script:stage50 = 3 } catch { }
                }
            }
            3 {
                $found = Find-SetupButton $process.Id 'Skip this file'
                if ($found) { $script:held50.Dispose(); $script:held50 = $null; Push-SetupButton $found; $script:stage50 = 4 }
            }
            4 {
                $found = Find-SetupButton $process.Id 'OK'
                if ($found) { [void] $script:shown50.Add((Get-SetupText $process.Id)); Push-SetupButton $found; $script:stage50 = 5 }
            }
            5 {
                $found = Find-SetupButton $process.Id 'Finish'
                if ($found) { [void] $script:shown50.Add((Get-SetupText $process.Id)); Push-SetupButton $found; $script:stage50 = 6 }
            }
        }
        return $false
    }
} finally {
    if ($script:held50) { $script:held50.Dispose() }
}
if ($script:stage50 -lt 6) { [void] $failures.Add("the setup was not seen through by hand (stage $($script:stage50) of 6; it ended with $($run.Exit))") }
if ($run.Exit -ne 7) { [void] $failures.Add("the first install ended with $($run.Exit), not 7, after a copy that skipped a file") }
if ($script:shown50.Count -ge 1 -and $script:shown50[0] -notmatch [regex]::Escape("qKVM Desktop $newVersion was not installed: the copy was not whole (")) { [void] $failures.Add('the setup did not say that its copy was not whole') }
if ($script:shown50.Count -ge 2 -and $script:shown50[1] -match 'Start qKVM Desktop') { [void] $failures.Add('the last page offers Start qKVM Desktop after a first install that does not count') }
Start-Sleep -Seconds 10
if (@(Get-ProgramProcesses).Count) { [void] $failures.Add('the program was started after a first install that does not count') }
$how = Stop-Program
$finish = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '50-finish')`""
if ($finish.Exit -ne 0) { [void] $failures.Add("the new setup did not install afterwards ($($finish.Exit))") }
[void] (Test-Installed $newVersion $failures)
Test-Data $failures
Write-Step '50' 'first-install-skip-starts-nothing' $began $failures ([ordered]@{ uninstalled = $uninstalled50; run = $run; held = $heldName50; stage = $script:stage50; shown = @($script:shown50); ended = $how; finish = $finish })

# --- 51 (0.2.0; the coordinator's rule for 15): a start of the program made during an update that still runs from the
# program's folder past the minute the setup waits for such starts is never ended by the setup: the copy does not count,
# as any that does not, and the setup says why. The older version again; the contract's update with one start of the new
# program held suspended (as Windows' scanner holds a first start, for longer than any did) from the moment its file is
# in until the setup has ended: it ends with 7, its log says that qKVM Desktop, started during the copy, still ran from
# its folder after a minute, nothing set aside is removed, nothing is ended by force, and the held start is there still
# as the setup ends. This test then ends it (as it would end by itself), and the next setup puts right what was left and
# installs the new version whole. (The put-back in between is whole only where the start lets go of what it holds:
# held through it, the program's file cannot be removed, and the marker of an unfinished update stays for the next
# setup, as for any file held through a put-back, review 1.5.)
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
[void] (Reset-ToOlder '51' $failures)
$log = Get-Log '51-start-held'
$script:held51 = 0
$stillThere51 = $false
try {
    $run = Invoke-Setup $Setup (& $contract $log) 300 {
        param($process, $gone)
        if (-not $script:held51) {
            $said = if (Test-Path -LiteralPath $log) { (Read-Shared $log) } else { '' }
            $at = $said.IndexOf("Dest filename: $Program")
            if ($at -ge 0 -and $said.IndexOf('Successfully installed the file.', $at) -ge 0) { $script:held51 = [QkvmVmHeldStart]::Start($Program, $App) }
        }
        return $false
    }
    $stillThere51 = [QkvmVmHeldStart]::Running()
} finally {
    [QkvmVmHeldStart]::End()
}
if (-not $script:held51) { [void] $failures.Add('no start of the new program could be held as its file came in') }
if ($run.Exit -ne 7) { [void] $failures.Add("the setup ended with $($run.Exit), not 7, while a start made during its copy still ran from its folder") }
$said51 = Read-Log '51-start-held'
if (-not $said51.Contains('The copy does not count: qKVM Desktop, started during the copy, still ran from its folder after a minute.')) {
    [void] $failures.Add('the setup''s log does not say that its copy does not count because a start made during it still ran')
}
if ($said51 -notmatch 'starts of qKVM Desktop made during the copy still run from its folder after \d+ ms: this setup ends none of them') {
    [void] $failures.Add('the setup''s log does not say that it waited the minute and ended none of the starts')
}
if ($said51 -match 'Finished an update whose copy was whole') { [void] $failures.Add('the setup removed what it had set aside though a start made during its copy still ran') }
if ($said51 -match 'ended by force') { [void] $failures.Add('the setup ended something by force') }
if ($script:held51 -and -not $stillThere51) { [void] $failures.Add('the held start was not there any more as the setup ended: something ended it') }
$finish = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '51-finish')`""
if ($finish.Exit -ne 0) { [void] $failures.Add("the next setup did not install ($($finish.Exit))") }
[void] (Test-Installed $newVersion $failures)
Test-Data $failures
Write-Step '51' 'start-held-past-the-minute' $began $failures ([ordered]@{ run = $run; held = $script:held51; heldThereAtTheEnd = $stillThere51; finish = $finish })

# --- 52 (review armp-1 L9): an interactive install's wait for a start of the program made during its copy is shown, on
# a page of its own whose progress is stepped at each look. The new version is installed (51 left it so); the new setup
# run by hand, Next and Install; one start of the new program held suspended from the moment its file is in, as Windows'
# scanner holds a first start, and let go of once the page has been seen: until then the setup waits for it and says so;
# then the start ends by itself, the install completes, its log says it waited, and Finish starts nothing (the program
# did not run before). Whether the wizard's windows answer a message within a second during the wait is recorded
# (answered, unanswered) and not judged: Inno Setup's progress page takes no message while the script sleeps (a test VM,
# 2026-11-22: 10 of 12 looks unanswered), so Windows may show the setup as not responding until the wait ends, within
# the minute, a known limit (CHANGELOG); the setup is not changed for it before 0.2.0 (the coordinator, 2026-11-22).
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$log52 = Get-Log '52-interactive-start-held'
$script:held52 = 0
$script:heldAt52 = $null
$script:page52 = ''
$script:answered52 = 0
$script:unanswered52 = 0
$script:let52 = $false
$hold52 = {
    # (Once the new program's file is in, by the setup's log: a start of it held, a minute at the most to come.)
    $until = (Get-Clock) + 120
    while (-not $script:held52 -and (Get-Clock) -lt $until) {
        $said = if (Test-Path -LiteralPath $log52) { (Read-Shared $log52) } else { '' }
        $at = $said.IndexOf("Dest filename: $Program")
        if ($at -ge 0 -and $said.IndexOf('Successfully installed the file.', $at) -ge 0) {
            $script:held52 = [QkvmVmHeldStart]::Start($Program, $App)
            $script:heldAt52 = Get-Clock
        } else { Start-Sleep -Milliseconds 100 }
    }
}
$watch52 = {
    # (The wizard's windows looked at while it waits, every quarter of a second, for the page's words and whether they
    # answer within a second; the loader's are not, whose thread waits for the wizard without pumping a message. Let
    # go of once the page has been seen and looked at three times, and 10 s after it was held, or after 30 s of looks
    # at the most: a look at the setup's words can take seconds, and the first VM runs of this step, 2026-11-22, let go
    # by the clock before the first look; the setup's minute is never reached so.)
    if (-not $script:held52) { return }
    # (What the setup's windows said just before this turn, as Invoke-SetupByHand kept it, is a look too.)
    $shownBefore52 = @($script:byHand.Shown)
    if ($shownBefore52.Count -and [string] $shownBefore52[-1] -match 'Waiting for qKVM Desktop, started during the install, to end') { $script:page52 = [string] $shownBefore52[-1] }
    $lookUntil = (Get-Clock) + 30
    $looks = 0
    while ((Get-Clock) -lt $lookUntil -and -not ($script:page52 -and $looks -ge 3 -and ((Get-Clock) - $script:heldAt52) -ge 10)) {
        $looks++
        $windows = @(Get-SetupProcesses $process.Id | Where-Object { $_ -ne $process.Id } | ForEach-Object { [QkvmVm]::WindowsOf([uint32] $_) })
        foreach ($window in $windows) { if ([QkvmVm]::Answers($window)) { $script:answered52++ } else { $script:unanswered52++ } }
        $shown = Get-SetupText $process.Id
        if ($shown -match 'Waiting for qKVM Desktop, started during the install, to end') { $script:page52 = $shown }
        Start-Sleep -Milliseconds 250
    }
    [QkvmVmHeldStart]::Release()
    $script:let52 = $true
}
try {
    $hand = Invoke-SetupByHand $log52 @('Next', 'Install', $hold52, $watch52, 'Finish')
} finally {
    [QkvmVmHeldStart]::Release()
}
if ($hand.Pressed -ne 5) { [void] $failures.Add("the setup was not seen through by hand: $($hand.Pressed) of 5 steps made (it ended with $($hand.Run.Exit))") }
elseif ($hand.Run.Exit -ne 0) { [void] $failures.Add("the setup ended with $($hand.Run.Exit)") }
if (-not $script:held52) { [void] $failures.Add('no start of the new program could be held as its file came in') }
elseif (-not $script:page52) { [void] $failures.Add('while it waited for the start made during its copy, the setup did not show "Waiting for qKVM Desktop, started during the install, to end"') }
if ((Read-Log '52-interactive-start-held') -notmatch 'Waited \d+ ms for the starts of qKVM Desktop made during the copy to end') {
    [void] $failures.Add('the setup''s log does not say it waited for the start made during its copy before it judged the copy')
}
[void] (Test-Installed $newVersion $failures)
Test-StartedAgain '52-interactive-start-held' $failures $false
$how = Stop-Program
Test-Data $failures
Write-Step '52' 'interactive-start-held' $began $failures ([ordered]@{ run = $hand.Run; pressed = $hand.Pressed; held = $script:held52; letGo = $script:let52
    windowAnswers = [ordered]@{ answered = $script:answered52; unanswered = $script:unanswered52; judged = $false
        why = 'recorded, not judged: Inno Setup''s progress page takes no message while the script sleeps; Windows may show the setup as not responding until the wait ends (a known limit, CHANGELOG)' }
    page = $script:page52; shown = $hand.Shown; lastSeen = $hand.LastSeen; ended = $how })

# --- 53 (outside review 2, 1.3): what an update that did not finish left is put right only in a folder that no other
# account may change, which is decided before anything there is read. The older version installed again; this test (its
# owner) then gives the users of this PC the right to change the program's folder and what is in it, as a folder an
# administrator chose for 0.1.x could have, and puts in it what another account could then put there: first the marker of
# an unfinished update whose record, one the setup reads, names Qt6Core.dll as what the copy made (a put-back would
# remove the earlier version's Qt6Core.dll by it); then, that marker taken away, the earlier program as a setup of 0.1.2
# set it aside (qkvm-desktop.exe.old-update beside the program, which 0.1.2's rule would remove). Each time, a silent
# setup follows nothing of it: its log says that nothing there is put right, it ends with 7 and the one result line that
# says the users of this PC may change the folder, that what an update left there stays as it is, and what to do; and the
# folder is byte for byte as it was, with what this test put in;
# its rights are as this test left them. Then the grant and those files taken away, the folder gives what it gave before
# (Get-RightsGiven), and the new version installs.
# (What a folder gives, and to whom: each entry of its list as it is kept, inherited or its own alike, once each, in no
# order. On the Windows on Arm test PC, whose profile's folders held their inherited entries without the mark that they
# are kept up from above (an SDDL of "D:" without "AI"), icacls's /grant wrote the list anew with those entries as the
# folder's own beside the same ones taken again from above, and the /remove:g after it left the same entries in that
# other form; an x64 Windows 11 given the same form kept no copies. So the end is held to the entries, not to the form;
# the forms are kept with the step.)
function Get-RightsGiven([string] $Path) {
    # (A list Get-Acl cannot read is said, and the step fails by it below, rather than ending the suite here.)
    try { $rules = @((Get-Acl -LiteralPath $Path).GetAccessRules($true, $true, [Security.Principal.SecurityIdentifier])) }
    catch { return "not read: $($_.Exception.Message)" }
    $given = @($rules | ForEach-Object { '{0} {1} 0x{2:x8} {3} {4}' -f $_.AccessControlType, $_.IdentityReference.Value, [int] $_.FileSystemRights, $_.InheritanceFlags, $_.PropagationFlags })
    return (@($given | Sort-Object -Unique) -join "`n")
}
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
[void] (Reset-ToOlder '53' $failures)
$planted53 = @(
    [ordered]@{ Name = 'record'; File = $UpdateMarker; Text = "words`r`nqKVM Desktop set-aside record 1`r`nending $AsideEnding`r`ncopy Qt6Core.dll`r`nend of the record`r`n" },
    [ordered]@{ Name = 'program-of-0.1.2'; File = "$Program$AsideEnding"; Text = "qkvm-desktop.exe$AsideEnding, put here by test_setup_vm.ps1" })
$refused53 = New-Object System.Collections.ArrayList
$rightsBefore53 = (& icacls.exe $App 2>$null) -join "`n"
$rightsGiven53Before = Get-RightsGiven $App
& icacls.exe $App /grant '*S-1-5-32-545:(OI)(CI)M' | Out-Null
$shared53 = (& icacls.exe $App 2>$null) -join "`n"
try {
    if ($shared53 -ceq $rightsBefore53) { [void] $failures.Add('the users of this PC could not be given the right to change the program''s folder') }
    foreach ($plant in $planted53) {
        [IO.File]::WriteAllText($plant.File, $plant.Text)
        $sums53Before = Get-FolderSums $App
        $logName = "53-$($plant.Name)"
        $run = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log $logName)`"" 180
        $lines = @(Get-ResultLines $logName)
        $said = Read-Log $logName
        if ($run.Exit -ne 7) { [void] $failures.Add("the setup into the shared folder with the $($plant.Name) in it did not refuse with 7 (it ended with $($run.Exit))") }
        if ($lines.Count -ne 1 -or $lines[0] -cnotmatch '\AqKVM-RESULT: 7 nothing was changed: the folder .+ is not one that this Windows account and Windows itself alone may change \(the users of this PC \(BU\) may change .+\): qKVM Desktop is installed only into such a folder\. What an update that did not finish left there stays as it is until then, and qKVM Desktop may not start meanwhile: run this setup again once no other account may change the folder, or uninstall qKVM Desktop in Settings, Apps\z') {
            [void] $failures.Add("the setup's log does not have the one result line of 7 for the shared folder with the $($plant.Name) in it: $($lines -join ' | ')")
        }
        if (-not $said.Contains('Nothing that an update left is put right, nor looked for: the folder ')) { [void] $failures.Add("the setup's log does not say that it put nothing right in the shared folder ($($plant.Name))") }
        foreach ($followed in 'Removed what the copy that did not complete had made', 'Put the earlier version back', 'Removed the earlier program, set aside by a setup before', 'Put the earlier program back') {
            if ($said.Contains($followed)) { [void] $failures.Add("the setup followed what was put in the shared folder ($($plant.Name)): $followed") }
        }
        $sums53After = Get-FolderSums $App
        if (($sums53Before | ConvertTo-Json) -cne ($sums53After | ConvertTo-Json)) {
            $changed = @(@($sums53Before.Keys) + @($sums53After.Keys) | Sort-Object -Unique | Where-Object { -not $sums53Before.Contains($_) -or -not $sums53After.Contains($_) -or $sums53Before.Item($_) -ne $sums53After.Item($_) })
            [void] $failures.Add("the shared folder with the $($plant.Name) in it is not as it was: $($changed -join ', ')")
        }
        if (((& icacls.exe $App 2>$null) -join "`n") -cne $shared53) { [void] $failures.Add("the setup changed the rights of the shared folder ($($plant.Name))") }
        Remove-Item -LiteralPath $plant.File -Force -ErrorAction SilentlyContinue
        [void] $refused53.Add([ordered]@{ case = $plant.Name; run = $run; resultLines = $lines })
    }
} finally {
    foreach ($plant in $planted53) { Remove-Item -LiteralPath $plant.File -Force -ErrorAction SilentlyContinue }
    & icacls.exe $App /remove:g '*S-1-5-32-545' | Out-Null
}
$rightsAfter53 = (& icacls.exe $App 2>$null) -join "`n"
$rightsGiven53After = Get-RightsGiven $App
if ($rightsGiven53Before -like 'not read:*' -or $rightsGiven53After -like 'not read:*') {
    [void] $failures.Add("the program's folder's rights could not be read: before [$rightsGiven53Before] after [$rightsGiven53After]")
} elseif ($rightsGiven53After -cne $rightsGiven53Before) {
    [void] $failures.Add("the program's folder does not give what it gave once this test took its grant away: before [$($rightsGiven53Before -replace "`n", ' | ')] after [$($rightsGiven53After -replace "`n", ' | ')]")
}
$finish = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '53-finish')`""
if ($finish.Exit -ne 0) { [void] $failures.Add("the new setup did not install afterwards ($($finish.Exit))") }
[void] (Test-Installed $newVersion $failures)
Test-Data $failures
Write-Step '53' 'put-right-in-shared-folder' $began $failures ([ordered]@{ refused = @($refused53); finish = $finish
    rights = [ordered]@{ before = $rightsBefore53; shared = $shared53; after = $rightsAfter53; givenBefore = $rightsGiven53Before; givenAfter = $rightsGiven53After } })

# --- 54 and 55 (0.2.0; the outside review of 0.2.0, 1.4): an earlier install of the other kind in the program's folder,
# which only Windows on Arm holds: an x64 install, carried over from an x64 PC (no setup of ours makes one there: the head
# of this test). Planted from -OtherKindZip, then upgraded to the PC's own kind by the contract's update (54), and cut and
# put back (55). Not run on x64 Windows, nor without -OtherKindZip.
function New-OtherKindInstall([string] $Number, [System.Collections.ArrayList] $Failures) {
    <# The older version installed again (Reset-ToOlder), then made the other kind's earlier version as an install carried
    over from a PC of that kind leaves it: every file of the older version's package taken away (and each of its folders
    that leaves empty), the package of -OtherKindZip put in, file by file as its manifest has them, beside the setup's own
    marker and uninstaller, and the Settings > Apps entry made that version's: its version, and the kind noted only where
    that version's setup noted one (0.2.0 and later). What Get-Installed then says of the folder. #>
    # (Where the older version could not be installed again (an earlier part of the step that failed may have left no
    # uninstaller, or a folder the setup takes for another's), or the plant fails, the step fails and the run goes on.)
    try { [void] (Reset-ToOlder $Number $Failures) } catch { [void] $Failures.Add("the older version could not be installed again first: $($_.Exception.Message)") }
    try { [void] (Write-OtherKindInPlace $Failures) } catch { [void] $Failures.Add("the $OtherPcKind install could not be planted: $($_.Exception.Message)") }
    return (Get-Installed)
}
function Write-OtherKindInPlace([System.Collections.ArrayList] $Failures) {
    <# New-OtherKindInstall's plant, over the older version just installed. #>
    $olderManifest = Get-Content -LiteralPath (Join-Path $App 'package-manifest.json') -Raw | ConvertFrom-Json
    $folders = New-Object System.Collections.Generic.HashSet[string]
    foreach ($entry in @($olderManifest.files)) {
        $relative = ([string] $entry.path) -replace '/', '\'
        Remove-Item -LiteralPath (Join-Path $App $relative) -Force
        $parent = Split-Path -Parent $relative
        while ($parent) { [void] $folders.Add($parent); $parent = Split-Path -Parent $parent }
    }
    Remove-Item -LiteralPath (Join-Path $App 'package-manifest.json') -Force
    foreach ($folder in @($folders | Sort-Object { $_.Length } -Descending)) {
        $at = Join-Path $App $folder
        if ((Test-Path -LiteralPath $at) -and -not @(Get-ChildItem -LiteralPath $at -Force).Count) { Remove-Item -LiteralPath $at -Force }
    }
    $archive = [IO.Compression.ZipFile]::OpenRead($OtherKindZip)
    try {
        foreach ($file in @($otherPackage.files) + @([pscustomobject] @{ path = 'package-manifest.json'; sha256 = '' })) {
            $to = Join-Path $App ($file.path -replace '/', '\')
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $to) | Out-Null
            $from = $archive.GetEntry("$($otherPackage.top)/$($file.path)")
            if (-not $from) { [void] $Failures.Add("-OtherKindZip has no $($file.path), which its manifest names"); continue }
            $in = $from.Open()
            try {
                $written = [IO.File]::Create($to)
                try { $in.CopyTo($written) } finally { $written.Dispose() }
            } finally { $in.Dispose() }
            if ($file.sha256 -and (Get-Sha256 $to) -ne $file.sha256) { [void] $Failures.Add("$($file.path) of -OtherKindZip is not as its manifest has it") }
        }
    } finally { $archive.Dispose() }
    $version = [version] $otherPackage.version
    Set-ItemProperty -LiteralPath $UserKey -Name 'DisplayVersion' -Value $otherPackage.version
    foreach ($part in @(@('MajorVersion', $version.Major), @('MinorVersion', $version.Minor))) {
        if ($null -ne (Get-EntryValue $part[0])) { Set-ItemProperty -LiteralPath $UserKey -Name $part[0] -Value $part[1] -Type DWord }
    }
    if ($version -lt [version] '0.2.0') { Remove-ItemProperty -LiteralPath $UserKey -Name 'qKVM Desktop Architecture' -ErrorAction SilentlyContinue }
    else { Set-ItemProperty -LiteralPath $UserKey -Name 'qKVM Desktop Architecture' -Value $OtherPcKind }
    $plantedKind = Get-ImageKind $Program
    if ($plantedKind -cne $OtherPcKind) { [void] $Failures.Add("the planted install's program is of $plantedKind, not of $OtherPcKind") }
}
function Start-ProgramIfThere {
    <# Start-Program where the program's file is there; where a part of the step before failed and left none, a start
    that did not come up (the step fails by that, and the run goes on). #>
    if (-not (Test-Path -LiteralPath $Program -PathType Leaf)) { return [pscustomobject]@{ Process = $null; Up = $false } }
    return (Start-Program)
}
function Test-OtherKindGone([string] $Said, [System.Collections.ArrayList] $Failures) {
    <# After a whole copy over the planted install: each file only the other kind's package had (by the two manifests) is
    gone, and the setup's log names it as removed, by that version's own list (RemoveEarlierFiles). Those names. (No
    manifest there, where the setup did not install: the step fails, and the run goes on.) #>
    $manifestFile = Join-Path $App 'package-manifest.json'
    if (-not (Test-Path -LiteralPath $manifestFile)) { [void] $Failures.Add('the program''s folder has no package-manifest.json, to tell what only the other kind''s install had'); return @() }
    $newNames = @(@((Get-Content -LiteralPath $manifestFile -Raw | ConvertFrom-Json).files | ForEach-Object { [string] $_.path }) + 'package-manifest.json')
    $onlyOther = @($otherPackage.files | ForEach-Object { [string] $_.path } | Where-Object { $newNames -notcontains $_ })
    foreach ($name in $onlyOther) {
        $relative = $name -replace '/', '\'
        if (Test-Path -LiteralPath (Join-Path $App $relative)) { [void] $Failures.Add("$name, which only the $OtherPcKind install had, is still there") }
        if (-not $Said.Contains("Removed what qKVM Desktop $($otherPackage.version) ($OtherPcKind) installed and this one has not: $relative")) {
            [void] $Failures.Add("the setup's log does not say it removed $name, which only the $OtherPcKind install had")
        }
    }
    return $onlyOther
}
$otherNotRun = if ($PcKind -ne 'arm64') {
    'an earlier install of the other kind (ARM64) on x64 Windows: no setup of ours makes one, and one carried over from a PC on Arm could not start here; the case is Windows on Arm''s, where an x64 install carried over from an x64 PC runs under emulation (the head of this test)'
} elseif (-not $otherPackage) {
    'it needs -OtherKindZip, the ZIP of an earlier published version''s x64 package (0.1.2''s), planted as an x64 install carried over from an x64 PC (the head of this test says why no setup can make one here)'
} else { '' }
if ($otherNotRun) {
    Write-NotRun '54' 'other-kind-upgraded' $otherNotRun
    Write-NotRun '55' 'other-kind-cut-put-back' $otherNotRun
} else {
# 54: the planted x64 install, its program running (under Windows' emulation), then the contract's update: the new
# version of the PC's kind installed whole, what only the x64 package had removed, and the program started again as ARM64.
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$planted54 = New-OtherKindInstall '54' $failures
$foundLine = "qKVM Desktop $($otherPackage.version) ($OtherPcKind) was installed in this folder: what it installed and this version ($PcKind) has not goes after a whole copy."
$started = Start-ProgramIfThere
if (-not $started.Up) { [void] $failures.Add("the planted $OtherPcKind program did not start (its mutex did not appear within 30 s)") }
$plantedRunsAs = @(Test-RunsAsKind $OtherPcKind $failures "the planted $OtherPcKind program")
$firstId = if ($started.Process) { $started.Process.Id } else { 0 }
$log = Get-Log '54-other-kind-upgraded'
$script:ended54 = $null
$run = Invoke-Setup $Setup (& $contract $log) 300 {
    param($process, $gone)
    # (As the program does: it starts the setup, then closes and exits within 10 s; here after 3 s.)
    if (-not $script:ended54 -and $gone -gt 3) { if ($firstId) { Stop-Process -Id $firstId -Force -ErrorAction SilentlyContinue }; $script:ended54 = $gone }
    return $false
}
if ($run.Exit -ne 0) { [void] $failures.Add("the update over the $OtherPcKind install ended with $($run.Exit)") }
$said = Read-Log '54-other-kind-upgraded'
if (-not $said.Contains($foundLine)) { [void] $failures.Add("the setup's log does not say: $foundLine") }
$state = Test-Installed $newVersion $failures
$onlyOther54 = @(Test-OtherKindGone $said $failures)
$notSeen54 = if (-not $onlyOther54.Count) { "files only the $OtherPcKind install had, removed: the two packages share every name" } else { '' }
if (-not (Wait-Until { Test-ProgramRunning } 30)) { [void] $failures.Add('the setup did not start the program again after the update') }
if ((Read-Log '54-other-kind-upgraded') -notmatch $StartedLine) { [void] $failures.Add('the setup''s log does not say it started the program with the one argument --after-update') }
$runsAs54 = @(Test-RunsAsKind $PcKind $failures 'the program the update started again')
$how = Stop-Program
Test-Data $failures -ProgramStarted
Write-Step '54' 'other-kind-upgraded' $began $failures ([ordered]@{ planted = [ordered]@{ version = $otherPackage.version; kind = $OtherPcKind; files = $planted54.Files.Count; runsAs = $plantedRunsAs }
    run = $run; programEndedAfterSeconds = $script:ended54; onlyTheOtherKindHad = $onlyOther54; runsAs = $runsAs54; files = $state.Files.Count; ended = $how }) $notSeen54

# 55: the planted x64 install, and the setup ended by force in the middle of its copy, as a power cut ends it: the x64
# install kept (as itself or set aside), the update check of the PC's kind at the program's name, the x64 program aside
# beside it; the next setup puts the x64 version back first, then installs the PC's kind. Then a copy that fails at the
# program's own file (held by this test, as in 29): the x64 version back whole, and its program starts as x64.
$began = [DateTime]::UtcNow
$failures = New-Object System.Collections.ArrayList
$planted55 = New-OtherKindInstall '55' $failures
$log = Get-Log '55-other-kind-cut'
$run = Invoke-Setup $Setup "$SilentArguments /LOG=`"$log`"" 120 {
    param($process, $gone)
    $said = Read-Shared $log
    $at = $said.IndexOf('\Qt6Core.dll')
    return ($at -ge 0 -and $said.IndexOf('Dest filename:', $at) -ge 0) -or $gone -gt 60
}
if ($run.Exit -ne 'killed') { [void] $failures.Add("the setup over the $OtherPcKind install was not ended in the middle of its copy (it ended with $($run.Exit))") }
[void] (Wait-Until { @(Get-SetupProcessesLeft).Count -eq 0 } 10)
Start-Sleep -Seconds 2
$cut55 = Get-Installed
$notKept55 = @(Get-NotKept $planted55 $cut55)
if ($notKept55.Count) { [void] $failures.Add("files of the $OtherPcKind install are neither there nor set aside as they were: $($notKept55 -join ', ')") }
$markerLeft55 = Test-Path -LiteralPath $UpdateMarker
if (-not $markerLeft55) { [void] $failures.Add('there is no marker of an unfinished update beside what the copy set aside') }
$atName55 = Get-AtProgramName
$checkThere55 = Test-UpdateCheckAt $Program
if (-not $checkThere55) { [void] $failures.Add("the update check of this PC's kind ($PcKind) is not at the program's name after the cut (at its name: $atName55)") }
$asideKind55 = Get-ImageKind "$Program$AsideEnding"
if ($asideKind55 -cne $OtherPcKind) { [void] $failures.Add("the program set aside for the copy is $(if ($asideKind55) { "of $asideKind55" } else { 'not there' }), not the $OtherPcKind one") }
$next = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '55-next-setup')`""
if ($next.Exit -ne 0) { [void] $failures.Add("the next setup ended with $($next.Exit)") }
$saidNext = Read-Log '55-next-setup'
if (-not (Test-PutBackFirst $saidNext)) { [void] $failures.Add("the next setup did not put the $OtherPcKind version back first, before it set it aside again") }
if (-not $saidNext.Contains($foundLine)) { [void] $failures.Add("the next setup's log does not say: $foundLine") }
[void] (Test-Installed $newVersion $failures)
$onlyOther55 = @(Test-OtherKindGone $saidNext $failures)
# (Planted again: a copy that fails at its very end, the program's own file taken by this test once the copy has begun.)
$earlier55 = New-OtherKindInstall '55b' $failures
$log = Get-Log '55-other-kind-copy-fails'
$script:held55 = $null
$script:heldAt55 = $false
try {
    $failed = Invoke-Setup $Setup "$SilentArguments /LOG=`"$log`"" 300 {
        param($process, $gone)
        $said = Read-Shared $log
        if (-not $script:heldAt55 -and $said.Contains('Dest filename:')) {
            try { $script:held55 = [IO.File]::Open($Program, [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None); $script:heldAt55 = $true } catch { }
        }
        if ($script:held55 -and $said.Contains('Rolling back changes.')) { $script:held55.Dispose(); $script:held55 = $null }
        return $false
    }
} finally {
    if ($script:held55) { $script:held55.Dispose() }
}
if (-not $script:heldAt55) { [void] $failures.Add('the program''s own file could not be taken during the copy over the planted install') }
if ($failed.Exit -eq 0) { [void] $failures.Add("the upgrade over the $OtherPcKind install did not fail with the program's own file held during its copy") }
if ((Read-Log '55-other-kind-copy-fails') -notmatch 'The copy did not complete: the earlier version is whole again\.') {
    [void] $failures.Add('the setup''s log does not say the earlier version is whole again')
}
Test-Earlier $earlier55 $failures
$restarted = Start-ProgramIfThere
if (-not $restarted.Up) { [void] $failures.Add("the $OtherPcKind program put back does not start") }
$backRunsAs = @(Test-RunsAsKind $OtherPcKind $failures "the $OtherPcKind program put back")
$backVersion = Get-Version $Program
if ($backVersion -ne $otherPackage.version) { [void] $failures.Add("the program put back is version $backVersion, not $($otherPackage.version)") }
$how = Stop-Program
$finish = Invoke-Setup $Setup "$SilentArguments /LOG=`"$(Get-Log '55-finish')`""
if ($finish.Exit -ne 0) { [void] $failures.Add("the new setup did not install afterwards ($($finish.Exit))") }
[void] (Test-Installed $newVersion $failures)
[void] (Test-OtherKindGone (Read-Log '55-finish') $failures)
Test-Data $failures -ProgramStarted
Write-Step '55' 'other-kind-cut-put-back' $began $failures ([ordered]@{ run = $run; notKept = $notKept55; markerLeft = $markerLeft55; atProgramName = $atName55
    updateCheckInPlace = $checkThere55; programSetAsideKind = $asideKind55; next = $next; onlyTheOtherKindHad = $onlyOther55
    copyFails = [ordered]@{ run = $failed; held = 'qkvm-desktop.exe'; putBackRunsAs = $backRunsAs; version = $backVersion }; ended = $how; finish = $finish })
}

$notRun = @($results | Where-Object { $_.Contains('notRun') } | ForEach-Object { $_.step })
$notSeen = @($results | Where-Object { $_.Contains('notSeen') } | ForEach-Object { $_.step })
$summary = [ordered]@{ machine = $Machine; windows = "$os"; kind = $PcKind; hypervisor = $Hypervisor; setup = $newVersion; olderSetup = $oldVersion; notRun = $notRun; notSeen = $notSeen
    passed = (@($results | Where-Object { -not $_.passed }).Count -eq 0); steps = @($results) }
$summary | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $Out 'summary.json') -Encoding UTF8
if ($summary.passed) { Write-Output "All $($results.Count - $notRun.Count) steps run passed$(if ($notRun.Count) { ", $($notRun.Count) not run ($($notRun -join ', '))" })$(if ($notSeen.Count) { ", part of $($notSeen.Count) not seen with these setups ($($notSeen -join ', '))" }): $Out" } else { Write-Output "Not all steps passed: $Out"; exit 1 }
