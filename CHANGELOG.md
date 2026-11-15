# Changes

## 0.1.2 (beta)

Upgrades that close qKVM for you.

- The setup says what is installed: "qKVM Desktop 0.1.1 is installed. This upgrades it to
  0.1.2 and keeps your wall, settings and passwords." An upgrade asks nothing it asked before:
  no folder, no desktop shortcut.
- Where qKVM is running, even only behind its icon in the notification area, the setup offers
  Close qKVM and continue. qKVM ends the way Install and restart ends it, and only where
  nothing would be cut short; otherwise it stays open, and the setup shows what qKVM says holds
  it. Where qKVM does not close within about 10 seconds, the setup offers End it now or Cancel;
  it ends qKVM by force only when you choose so. 0.1.0 and 0.1.1 cannot be asked: quit them
  from their icon, or choose End it now.
- The last page offers Start qKVM Desktop, ticked when the setup closed qKVM.
- A silent setup closes qKVM the same way and starts it again. Where qKVM stays open, it
  changes nothing, ends with exit code 7 and says why in its log (the `qKVM-RESULT:` line);
  `/FORCECLOSE=1` ends qKVM by force where it does not close.
- A copy from the ZIP that runs from its own folder is left alone by the setup.
- The uninstall does not begin while qKVM runs or while a setup is open, and says why.
- Everything survives an upgrade: your wall, settings, preferences, the passwords of your KVMs
  and servers, your server sign-ins, the app password and automatic sign-in. Every build is now
  tested against real profiles made by 0.1.0 and 0.1.1, which must open unlocked and unchanged.
- At the first start of another version, qKVM keeps a copy of its settings as they were
  (`settings.json.<version>.bak`, never with a password in it). Reset saved secrets keeps a copy
  first, erases nothing if that copy cannot be made, and says why the reset is needed.
- SPEC.md lists everything the program stores, and the rule that no release changes it without
  carrying your data across.
- A BIOS setup override left from before whose time of doubt is over (its server down, or no
  longer on the wall) no longer holds Install and restart or Close qKVM and continue: only a
  start into BIOS setup still under way does. Its card and the notice still say it.
- Where a server's controller only queues the BIOS setup boot override (it answers 202, or with
  a task), the press says so, and the server's card says "The controller queued a change qKVM
  could not follow; check the boot override in its BMC" until a later start into BIOS setup on
  it ends as it should, or the server is taken off the wall; no look or time ends it. Following
  the task itself comes in 0.2.0.

## 0.1.1 (beta)

Fixes for the first beta, and updates whose files may come from the GitHub release.

- The window's title says what the window shows, then the program's name, as Windows titles
  go: "This PC - qKVM", "192.168.1.10:8787 - qKVM", "Rack B - This PC - qKVM", and "qKVM"
  alone before anything is chosen. In 0.1.0 it began with "qKVM" and a long dash, and Windows
  added a second "- qKVM" after it.
- No long dash, short dash or curly quote in any text of the program, its notifications or its
  documents; the project's checks refuse them.
- Updates: a file of a release may come from its GitHub release page. The program follows the
  one redirect GitHub's download address answers with, to GitHub's own file host, and checks
  the file by the signed feed's SHA-256 and size as before. It follows no other redirect, and
  the feed follows none.

## 0.1.0 (beta)

The first public release of qKVM Desktop, a beta, for Windows 10 (22H2) and Windows 11, x64,
as a setup or a ZIP to unpack. It works with a qKVM server 0.3 or later.

- A wall of this PC's own machines, with no server: GL.iNet Comet and other kvmd KVMs such as
  PiKVM, JetKVM, and servers through their management controllers over Redfish (state, power,
  the light, sensors), with a server's own screen where its controller runs AMI MegaRAC.
- Each device known by the certificate it showed first; a password goes only to the device it
  was saved for, and only after what answers there has said what it is. Passwords and saved
  texts are kept in Windows Credential Manager.
- One machine shown large and live: H.264 over WebRTC where the KVM sends it, decoded on the
  graphics card, JPEG otherwise; keyboard and mouse taken in its window, with the Windows key,
  Alt+Tab and a leave key, absolute or relative mouse, typed text, and keys on screen.
- A qKVM server's wall beside the program's own, signed in with an account or a display link,
  with its live connection, its actions, its history and, for admins, its activity; virtual
  media of a server's machines through the server.
- Find devices on a private network; saved actions (key combinations and texts); favorites
  and recent machines; alerts for machines that go away or come back.
- The wall as a file, as the qKVM server writes it: exported and imported, sealed with a
  passphrase where it holds passwords.
- The program's own password, made at the first start: Lock (Ctrl+L), a wall screen that only
  shows until a person with the password signs it out, and a wrong password that waits longer
  each time.
- A setup, beside the ZIP. It installs the program for you alone, without asking for an
  administrator, into `%LOCALAPPDATA%\Programs\qKVM Desktop`, adds it to the Start menu (to
  the desktop only if you tick that) and to Settings, Apps, where it is uninstalled. It
  upgrades an installed copy where it is, and never puts an older version over a newer one.
  If an update was cut off and qKVM no longer starts, the setup run again puts it back.
- Updates: a daily check of a signed feed (it can be switched off). An installed copy installs
  its own: Settings, Updates, Install and restart. It takes only a setup from its own signed
  feed, checks it as every update is checked, closes once nothing it is doing would be cut
  short (or says what holds it), and starts again after; the next start says how the update
  went. A copy from the ZIP downloads and verifies a new release when you ask, and shows it to
  you to unpack. Your wall, its passwords and its settings stay with your Windows account, for
  either kind of copy.
- Several windows and monitors, light and dark, and the web page's look and words.
