# qKVM Desktop

qKVM Desktop is the qKVM screen wall as a Windows program of its own. It shows the live
screens of many machines on one wall, through their IP KVMs (GL.iNet Comet and other
PiKVM-style devices, JetKVM) and the management controllers of servers, and lets you work
on one of them with the real keyboard and mouse, including the keys a browser never gets
(the Windows key, Alt+Tab).

It needs no server: KVMs are added in the program, and it speaks to them itself. Whoever
also runs a [qKVM server](https://github.com/qkvm/qkvm) can sign the program in to it and
have that server's wall beside the program's own.

**This is a beta.** It is not yet signed with a certificate. A copy the setup installed
installs an update when you choose Install and restart; a copy from the ZIP shows the update
rather than installing it. Try it first where a mistake costs little.

Website: https://qkvm.com/#desktop

## Download

For Windows 10 (22H2) and Windows 11, x64, the same program in two forms:

- **The setup**,
  [qKVM-Desktop-0.1.1-windows-x64-setup.exe](https://github.com/qkvm/qkvm-desktop/releases/download/v0.1.1/qKVM-Desktop-0.1.1-windows-x64-setup.exe).
  It installs the program for you alone, without asking for an administrator, into
  `%LOCALAPPDATA%\Programs\qKVM Desktop`, and adds it to the Start menu (to the desktop only if
  you tick that) and to Settings, Apps.
- **The ZIP**,
  [qKVM-Desktop-0.1.1-windows-x64.zip](https://github.com/qkvm/qkvm-desktop/releases/download/v0.1.1/qKVM-Desktop-0.1.1-windows-x64.zip).
  Nothing is installed: unpack the whole ZIP into a folder of its own and start
  `qkvm-desktop.exe` in it.

The same files are also at
[download.qkvm.com](https://download.qkvm.com/desktop/0.1.1/qKVM-Desktop-0.1.1-windows-x64.zip), where
copies of 0.1.0 take their update from. Both files, their `SHA256SUMS` and the release notes are on the
[release page](https://github.com/qkvm/qkvm-desktop/releases/tag/v0.1.1). At the first start
the program asks for a password of its own.

Windows may say it protected your PC: neither file is signed with a certificate yet. Choose
More info, then Run anyway.

### Check the download

Before you run the setup or unpack the ZIP, compare its SHA-256 with the line for it in
`SHA256SUMS` on the release page. In PowerShell, in the folder you downloaded it to:

    Get-FileHash .\qKVM-Desktop-0.1.1-windows-x64-setup.exe -Algorithm SHA256
    Get-FileHash .\qKVM-Desktop-0.1.1-windows-x64.zip -Algorithm SHA256

If the two differ, do not run it.

### Upgrade and uninstall

To upgrade an installed copy, run the setup of the newer version: it installs over the copy
you have, where it is. Close the program first: while it runs, the setup asks you to close
it, and so does the uninstall.

An installed copy can also do it itself: when Settings, Updates offers a newer release, choose
Install and restart. qKVM checks the setup it downloaded against its signed feed, closes, lets
that setup install it, and starts again.

To upgrade a copy from the ZIP, close it and unpack the newer ZIP into a folder of its own, or
run the setup instead. Either finds your wall as it was (see below).

If an update was cut off (the PC lost power or the setup was ended) and qKVM no longer
starts, run the setup again, the newer one or the one you have: it puts the program back
first.

To uninstall it: Settings, Apps, Installed apps, qKVM Desktop, Uninstall. A copy from the ZIP
is removed by closing the program and deleting its folder.

### Where your data stays

Uninstalling the program, or deleting the folder of the ZIP, removes the program and nothing
of what it keeps for you. That stays with your Windows account, for the next copy you install
or unpack:

- the settings (the devices and servers of your wall, its layouts): `%LOCALAPPDATA%\QKVM Desktop`;
- crash summaries, video diagnostics and update downloads: `%LOCALAPPDATA%\qKVM\Cache`;
- the passwords of your KVMs and servers, saved actions and server sessions: Windows
  Credential Manager, in the entries whose names begin with `QKVM Desktop:`.

To remove those too, delete the two folders and those entries.

## With a qKVM server

Works on its own, and with a qKVM server 0.3 or later; what a server does not have is not
shown.

## Building

Windows 10 or 11, x64, and PowerShell 7. Four things are needed, and none of them is in the
repository:

1. **Microsoft's compiler.** Build Tools for Visual Studio 2022, with its C++ tools and its
   "C++ Clang tools": CMake, Ninja, clang-format and clang-tidy come with them. Installing it
   asks for administrator rights once, and takes about 9 GB:

       winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--passive --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --add Microsoft.VisualStudio.Component.VC.Llvm.Clang"

   Qt 6.12 refuses a compiler older than Visual Studio 2022's, and a CMake before 3.25.
2. **Qt 6.12.0**, the open-source edition, kept outside the repository because of its size
   (2.3 GB). The script takes only the modules the program uses, and writes where it put them
   into `local.cmake`, which git does not keep:

       tools\fetch_qt.ps1 -Into D:\Qt

3. **FFmpeg**, an LGPL build, into `.deps` (180 MB):

       tools\fetch_ffmpeg.ps1

4. **Native WebRTC**, libdatachannel 0.24.6 with Mbed TLS 3.6.7, built here from pinned
   sources (`experiments/webrtc/dependencies.json`). It installs no runtime or toolchain
   for the whole PC:

       tools\fetch_webrtc.ps1

   CMake copies `datachannel.dll` beside each program. Its C interface is used across the
   DLL boundary, so the release library also serves the debug and sanitizer builds.

The Qt and FFmpeg scripts check each downloaded archive against its recorded size and SHA-256.
Then:

    tools\check.ps1            all of it: how the code is set, three kinds of build, every test, the linters
    tools\build.ps1            the debug build alone, while working
    tools\run.ps1              starts the program
    tools\package.ps1          the program folder and its ZIP, under build\packages
    tools\make_testdata.ps1    makes the pictures the engine is tested and measured with

`tools\check.ps1` keeps everything it printed in `build\check.txt`; it has passed when its
last line says so. A test that passes only when it is run a second time is named there.

How the program is meant to behave is in [SPEC.md](SPEC.md), and how it is built in
[DESIGN.md](DESIGN.md). What changed from one release to the next: [CHANGELOG.md](CHANGELOG.md).

## Security

What is kept where, what the program connects to, and how to report a problem:
[SECURITY.md](SECURITY.md).

**The app lock on a shared Windows account.** A passer-by has no control through qKVM's
interface alone; with the Windows account, a Credential Manager Restore can replace the app
password record and its retry count. Also, restoring an older backup resets the wrong-try
count. Windows Credential Manager's *Back up Credentials* can export qKVM's secrets without
the app password, and *Restore* may bring in a password record and secrets from another PC.
App lock alone cannot protect saved secrets from another person who uses that unlocked
Windows account.

## Licence

Copyright (c) 2026 qKVM, part of [PyPV Python Platform Ventures](https://pypv.com). Free
software under the [GNU Affero General Public License, version 3](LICENSE) only: you may use,
study, change and share it, and if you pass it on, or run a changed version for others, you
must offer them your source under the same licence. It comes with no warranty. The name qKVM
and its logo are not covered by that licence.

Every line here is written for this project: no code is copied from another project, whatever
its licence. The libraries the program ships with, and their licences:
[THIRD-PARTY.md](THIRD-PARTY.md) and [licenses/](licenses/). Reports are welcome; pull requests
are not taken.
