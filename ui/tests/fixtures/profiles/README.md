# Profiles of the published versions

Two folders for each published version of qKVM Desktop, each holding a whole profile as that version's own code made
it: `<version>` of a person at the PC, and `<version>-wall-screen` of a PC nobody sits at. `test_ui_upgrade_continuity`
(`../../test_upgrade_continuity.cpp`) reads each with the build it is part of, and fails where anything of it does
not come up as it was left (SPEC.md section 11); `test_ui_credential_shapes` (`../../test_credential_shapes.cpp`) has
this build's own Windows code find the very records that version's Windows code wrote. None may go, and every release
adds its own before it is published.

Everything in them is made up: the KVMs are at addresses set aside for examples (192.0.2.0/24, 198.51.100.0/24), the
servers are stand-ins on this PC's own address (`http://127.0.0.1:47391/`, `http://127.0.0.1:47392/`, ports below those
Windows hands out by itself), the certificate is the server's own example files' (`core/tests/data/export`), and every
name, password, session, link and text was typed for these files and is used nowhere else.

## What each holds

* `profile/`: the files of `%LOCALAPPDATA%\QKVM Desktop` as that version wrote them, byte for byte.
  * A person's: `settings.json`, `settings.json.sign-ins`, `settings.json.incidents`, `settings.json.setup-overrides`,
    `alert-pictures\` (a screen watch's picture and its state, `screens.json`) and `updates\` (a hand-over to Setup).
    No `settings.json.locked`: the app lock was left unlocked under automatic sign-in. Setup's log of that hand-over
    (`updates\setup-<version>.log`) is Setup's file, and the repository keeps no `.log` (`tools/check_staged.py`):
    `made.json` holds it (`update.setup_log`, its name and its text), and `test_ui_upgrade_continuity` puts it there.
  * A wall screen's: `settings.json` and `settings.json.locked` (it was locked).
* `secrets.json`: every secret the program kept, by the name the program gives it, with its bytes (`bytes`, base64).
* `credentials.json`: what that version's own Windows code (`core/src/windows_secrets.cpp` at its commit) made of each
  of them, by `qkvm-test-credential-shape` (`../../credential_shape.cpp`): the record's target name, type, persistence,
  flags, user name, comment, alias and blob, as Windows Credential Manager would have kept it, and every filter, type
  and flag that version writes, lists, reads and erases them with.
* `made.json`: what was done and typed, in plain words, which the tests hold the profile to.

A person's profile: the app password set with automatic sign-in at the first start; a qKVM server signed in to with
"Stay signed in"; a kvmd over HTTPS known by its certificate, a kvmd over plain HTTP with two screen watches, a JetKVM
and a server's management controller, each with its password and a group, and a kvmd taken from a file of the wall
with its certificate itself (`certificate_pem`); a KVM of a family that version does not speak to, as a newer build
would have left it; an action that presses keys, one that types a short text and one whose text is kept in two parts;
every choice of the settings box away from what a new profile has; favorites, recent machines, a picture's size, two
windows and a layout; two refused sign-ins and one that came to nothing; a screen watch gone off with its picture, and
paused; a server's BIOS setup boot override in doubt; and an update handed over to Setup that Setup did not install. It
ended on This PC's own wall.

A wall screen's profile: the app password set with automatic sign-in; a display link that signed a server's wall in
here, so that this PC is a wall screen of that server (`wall screen`, `display link at <server>`); then locked.

Not in them: the program's cache (`%LOCALAPPDATA%\qKVM\Cache` and Qt's own `cache`), which may go at any time.

## How they were made

By `qkvm-test-make-profile` (`../../profile_maker.cpp`, `person` and `wall-screen`) and then
`qkvm-test-credential-shape`, each compiled at the version's own commit, with that version's Settings, AppLock,
LocalWall, OwnActions, ServerSource, WallScreen, PacingFile, Incidents, ScreenWatch, SetupOverrides and
windows_secrets.cpp, and a store in memory (`core/tests/memory_secrets.h`; for the shapes,
`../../windows_secrets_in_memory.h`): never Windows' own Credential Manager. Two are written by the maker in the form
of that version's code, as no other way makes them without starting a Setup: the hand-over note
(`Updates::noteHandOver`'s fields) and Setup's log in `made.json` (Inno Setup's lines and the contract's
`qKVM-RESULT`).

| Folders | Version | Commit |
|---|---|---|
| `0.1.0`, `0.1.0-wall-screen` | 0.1.0 | `bbb09328` |
| `0.1.1`, `0.1.1-wall-screen` | 0.1.1 | `15fcb72a` |

For each, on 2026-11-14 (builder k012):

    git worktree add --detach <scratch>\v010 bbb09328
    (copy profile_maker.cpp, credential_shape.cpp, profile_writer.cpp, profile_writing.h, disposable_machine.h,
     credential_capture.h and windows_secrets_in_memory.h into <scratch>\v010\desktop\ui\tests, and add their three
     targets to ui\CMakeLists.txt as they stand here; none of it is committed there)
    cmake --preset debug ; cmake --build --preset debug --target qkvm-test-make-profile qkvm-test-credential-shape
    build\debug\bin\qkvm-test-make-profile.exe <this folder>\0.1.0 <the full commit> person
    build\debug\bin\qkvm-test-make-profile.exe <this folder>\0.1.0-wall-screen <the full commit> wall-screen
    build\debug\bin\qkvm-test-credential-shape.exe <this folder>\0.1.0 <the full commit>   (and the wall screen's)
    cmake --preset release ; cmake --build --preset release --target qkvm-test-write-profile
    git worktree remove --force <scratch>\v010

The files were committed as the tools wrote them (`.gitattributes` here keeps their bytes). A new release does the same
at its release commit, into folders named by its version, and adds the version to `published` in both tests.

## In a virtual machine

`tools/test_upgrade_keeps_vm.ps1` puts a person's fixture into a throwaway VM's real Credential Manager and profile
folder with `qkvm-test-write-profile` built at the fixture's own commit (so that the records are written by that
version's own Windows code, as the person's were), over the older setup of the same version; upgrades with the new
setup; sees the program start at the profile's own views (a window for each that `settings.json` remembers, titled
as the program titles it, and no box for the app password or a reset); and compares the profile with
`qkvm-test-check-profile` of the new build, file by file and credential by credential. Its header says how to run it;
`tools/test_vm_scripts.ps1` drives its helpers in Windows PowerShell, as a VM runs them, in every build's tests. The
script, the writer and the checker each refuse unless the machine's firmware names a hypervisor (QEMU, VMware,
Hyper-V, VirtualBox) and the user's profile holds `qkvm-disposable-vm.txt` with the machine's name.
