# Changes

## 0.2.0 (beta)

New kinds of machines on this PC's own wall, virtual media, and upgrades that put the earlier qKVM back
where they stop.

- Virtual machines on Proxmox VE, as a qKVM server 0.3 has them. In Add a KVM, give a Proxmox host's
  address and an API token, then tick the guests to add. Each guest has a card of its own: start, shut
  down, stop and reset, its screen live with keys, typed text and control, and Media... to put an ISO
  image from its Proxmox storage in its CD-ROM drive and start from it. Its boot order is put back after,
  even across a restart of qKVM. A guest is known by its SMBIOS UUID: where Proxmox gave its number to
  another virtual machine, nothing is sent to that one. The token's secret stays in Credential Manager.
- VNC servers, as a qKVM server 0.3 has them: `vnc://` and `vncs://` (TLS, held to the certificate it
  showed first), with a password or without, the screen live with keyboard and mouse, keys and typed
  text. A `vnc://` server is said to be unencrypted wherever a KVM over plain HTTP is.
- The console of a server whose controller runs OpenBMC: its screen on the card and live, with keys,
  typed text and control.
- Virtual media of this PC's own GL.iNet Comet, PiKVM and JetKVM, as a qKVM server 0.3 has them. The
  machine menu's Media... lists the images on the KVM's own storage, mounts one as a CD-ROM drive, a USB
  flash drive or a disk, takes it out, deletes one, and uploads an image from this PC. Where the KVM has
  a power board and you tick Restart it now, the machine is restarted after the mount; a Comet or a
  PiKVM does not restart one whose card says Power unknown (where its board does not say whether the
  machine is on, it is reset, and the Media box says so). What is in the drive is on the card. Nothing is
  fetched from an address, and what another client of the KVM chose meanwhile is not taken for yours.
  One short moment is left on a Comet or a PiKVM: where a mount finds the wrong image in the drive and
  takes it out again, an image another client put in just then is taken out too.
- A KVM whose plain address sends everybody on to HTTPS says so: "This KVM answers on HTTPS". Add a KVM
  and the KVM's edit box offer that address (Use https://...) and take it only when you choose it. qKVM
  never follows the KVM there by itself, and adding or changing one sends no password the plain way to be
  passed on.
- A KVM whose power board says the computer is off while its screen shows a picture says Power unknown
  on its card, and its Details say why: its power-LED wires are probably not connected. On a KVM with
  several inputs the card says Power unknown alone: its board may be another computer's.
- On a PiKVM a lit disk light counts as a machine that is on, and on a GL.iNet Comet or a PiKVM whose power is
  unknown, a short press of its power button is refused and says why: it could shut a running machine down.
  A hold of the power button still goes. On a KVM with several inputs, a computer whose board says off
  while another input is shown is switched on by a short press once its own input is shown.
- Virtual media through a qKVM server 0.3.2 are held to the machine they were picked for. Where its tile
  was edited to another KVM meanwhile, nothing is mounted, removed or uploaded, and the Media box says why.
- On a qKVM server's wall, a KVM keeps the server's rules: where its tile says Power unknown, a short
  press of its power button goes to the server, which presses it. The refusal above is this PC's own
  wall's.
- A server's own screen through its controller, as web 0.3.2 has it: its console is left alone where the
  controller does not say whether it switches the server's own monitor off, and its card says so.
- Start into BIOS setup: where the controller only queues the change, qKVM follows the controller's task
  for up to a minute, and restarts the server once the server shows the change, or, where nothing can
  be read back at all, as 0.1.2 did; where it cannot tell, it says so, and the card keeps its note. A
  controller that allows no override for the next boot only is not asked, and an override of qKVM's
  that a board kept for every boot is cleared again. Where qKVM ends before the controller has answered
  the change (quit, a crash, Windows' restart), the server's card says at the next start that the
  controller may have queued it. A server switched on that stays off is said still off, also where its
  controller kept the override for every boot or another tool's override shows meanwhile.
- A server's card is not taken for another computer of its controller, such as another node of a
  chassis. Where the server gives no UUID (some boards give none while they are off), qKVM goes by the
  place on the controller where it last saw the card's computer, and does nothing to a server whose
  place it has not seen yet. On a controller that lists several computers, a server that gives no UUID
  is taken for no card at all: such a node, off and giving none, is not switched on from qKVM, where
  0.1.2 pressed it. Nor is any of several computers that give the same UUID (boards whose maker gave
  them one for all), and AMI's placeholder UUID (03000200-0400-0500-0006-000700080009) counts as no
  UUID at all. Each press reads the controller's list of computers anew, and a command goes only to
  the computer it was decided for: where the controller numbers its computers anew meanwhile, nothing
  is sent, and the card says so. The cases this cannot tell: another computer put in the card's own
  place, off and giving no UUID, on a controller never seen to list more than one; for a card added
  while its server gave no UUID, such a computer on or off; and two computers that give one UUID of
  their own that is no known placeholder, the card's own pulled. Where the computer at the place of a
  card added while its server gave no UUID gives a UUID of its own now, the card asks once whether it
  is its server, Yes or No: Yes knows it by that UUID from then on, with no password typed; No refuses
  that computer from then on, on or off, until you change the card and type its password while the server
  gives its UUID; until it is answered, nothing but looks goes to it (no power, no boot override, no keys,
  no console: one open then is closed). The known limits below say what this means for server cards of
  0.1.x.
- A server's controller is held to more than in 0.1.2. Its password goes only where the address
  answers as a Redfish controller (its service root): an error page or another device's sign-in there
  gets nothing. A controller that closes the connection after every answer is not signed in to, since
  its password would have to go on another connection than the one where qKVM asked what it is, and its
  card says so. Start into BIOS setup is refused, with nothing sent, where the controller does not say
  whether a boot override is set.
- An update or upgrade whose copy stops partway (a file in use, a full disk) puts the earlier qKVM back
  at once: the setup sets every file aside first. Where the PC loses power or the setup is ended during
  the copy, qKVM does not start until a setup runs again: its start says that an update did not finish,
  and the next setup, of any version, puts it right. (In 0.1.2 there was then no program at all to
  start.)
- The setup installs only into a folder of its own that no other account may change, one setup or
  uninstall runs at a time for your account, and an upgrade removes exactly the files an earlier version
  installed and this one no longer ships. Your own files beside the program stay.
- Keys and clicks to a machine through a qKVM server no longer wait in qKVM: what has waited there longer
  than a quarter of a second is never sent, as on every other line, and the live view is opened again.
  Before, it could wait in qKVM and be pressed up to nine seconds later. What qKVM had already handed to
  Windows when the way stalled (under 4 KB, some thirty keys) may still arrive when the way comes back,
  on every line, until the line is given up for its silence: 9 seconds on a live view through a qKVM
  server, 3 on a GL.iNet Comet's or a PiKVM's keyboard, 40 on a server's console of AMI's, and up to
  some 85 seconds on a VNC server's, a virtual machine's or an OpenBMC console whose screen does not
  change. On a server's console of AMI's, what waits in qKVM may wait ten seconds in 0.2.0, as its
  controller may pause for a few seconds.
- One copy of qKVM for each Windows account: another account's copy in the same session no longer keeps
  yours from starting, and a second copy of yours in another session says so and ends.
- No more freezes of up to 17 seconds where a device's certificate names a signer that Windows cannot
  fetch: every line to a device speaks TLS on a thread of its own. Quitting does not wait for Windows
  there. It still gives each server's controller its session back first (5 seconds at the most), and
  clears a start into BIOS setup still under way (30 seconds at the most).
- A large wall takes less memory: a card's picture is kept at the size the card draws it, and the wall
  looks at its machines a few at a time.
- Your wall, settings and passwords survive an upgrade and a way back: an entry of the settings that
  this version cannot read, or that a later version wrote, is kept as it stands, and a change of a saved
  password that was cut off is made whole at the next start. Every build is tested against real profiles
  of 0.1.0, 0.1.1 and 0.1.2. What a way back drops, and the server cards of 0.1.x that refuse everything
  until you act, are in the known limits below.
- The readers of what KVMs, VNC servers and server controllers send are fuzzed by every check before a
  release, but for these, which are not fuzzed yet: kvmd's WebRTC signalling, AMI's web sign-in, the
  probe of an address being added, kvmd's Fingerbot, a Proxmox host's answers, what a JetKVM says of its
  media, and a qKVM server's answers. No reader fault was found.
- An update's feed is taken by a PC whose clock is a few minutes behind.
- For Windows on Arm, the ARM64 program, which runs natively: a ZIP of its own
  (`qKVM-Desktop-0.2.0-windows-arm64.zip`), and one setup for both kinds of PC
  (`qKVM-Desktop-0.2.0-windows-setup.exe`), which installs the x64 program or, on Windows on Arm, the ARM64
  one. A copy of the x64 program of 0.2.0 or later on Windows on Arm, which Windows runs under its emulation,
  is offered the ARM64 program of its own version too, and says that it is for Windows on Arm. x64 PCs see no
  change.
- Known limit: a server card from 0.1.x does not know yet where on its controller its server is, and two
  kinds of them refuse everything after the upgrade, with nothing done to the server and the card saying
  why. A card whose server gives no UUID while it is off is refused while the server stays off, Power on
  too: switch the server on once another way (from its BMC's own page, say), and the card finds it; or
  change the card and type its password. A card added under 0.1.x while its server gave no UUID, or gave
  AMI's placeholder UUID (03000200-0400-0500-0006-000700080009), which 0.2.0 takes for no UUID, is refused
  until you change it and type its password. Have those BMC passwords at hand before you
  upgrade. The same comes back after going back to 0.1.2 and up again, and for a server card taken from
  a file of the wall.
- Known limit: a server card added while its server gave no UUID (some boards give none while they are
  off) asks once, when the server gives a UUID of its own (switched on, say), whether that is its server,
  since qKVM cannot tell it from another computer put in its place. Until you answer Yes on the card (or
  change the card and type its password), nothing but looks goes to it, its screen included. A start into
  BIOS setup under way then is watched to its end and says what it saw. After a No the card refuses that
  computer, also once it is off and gives no UUID. To take your own server there after that, once it stands
  there again, switch it on from its BMC's own page, then change the card and type its password; or remove
  the card and add it again, the only way where your own server gives no UUID even when it is on. A No is
  written to the settings a few seconds after you say it (half a minute at the most): if qKVM crashes
  within them, or cannot write its settings file at all, the No is lost, and the card takes that computer
  as its own again while it gives no UUID, and asks again once it gives one.
- Known limit: going back to 0.1.2. 0.1.2 does not hold a server card to its place or to a No. A card added
  while its server gave no UUID takes whatever computer answers at its controller there, the one you said
  No to as well, until you come back up; then it refuses until you change it and type its password. Before
  you go back, remove such a card if another computer may stand in its server's place, and add it again
  under 0.1.2; or, under 0.1.2, look at its screen before you press its buttons.
- Known limit: 0.1.2 drops a VNC server from the settings when it writes them. The copy of the settings
  it keeps first (`settings.json.<version>.bak`) still holds it until three later changes of version, and
  its password stays saved. 0.1.0 and 0.1.1 keep no such copy: started over the settings of 0.2.0, they
  drop a VNC server for good.
- Known limit: where a change of a saved password was cut off, and 0.1.0, 0.1.1 or 0.1.2 is started
  before 0.2.0 and trusts a new certificate of that device, its saved password is lost and asked for
  again.
- Known limit: two VNC servers behind one certificate (one TLS proxy before several displays, or a PiKVM
  and its own VNC server) are asked Keep both when they are added. When that certificate is renewed, the
  first card's yes takes the new one, and the other card is removed and added again with its password.
  A JetKVM over plain HTTP held back for that other card (its HTTPS showed the old certificate) is let go
  then where its HTTPS shows nothing at all, and its password may go the plain way: remove such a
  JetKVM's card before, and add it again after.
- Known limit: Proxmox's power commands and consoles carry nothing that ties them to one virtual machine.
  qKVM asks which virtual machine has the guest's number right before each one, and sends it on that
  very answer; a virtual machine deleted and made anew under that number in the moment between the
  answer and the command is not seen.
- Known limit: on a JetKVM, Restart it now after a mount presses the machine's power button where the ATX
  extension says its power light is off and its disk light is dark at that moment, also while the
  screen shows a picture. A running machine whose power light is not wired is then shut down, not
  restarted. Leave Restart it now unticked for such a machine.
- Known limit: on Windows on Arm, a ZIP copy of 0.1.x takes two steps to the ARM64 program: it is offered
  the x64 0.2.0 ZIP first, as every 0.1.x ZIP copy is, and that copy then offers the ARM64 one.
- Known limit: the ARM64 program is guarded less than the x64 one. Qt's own ARM64 build does not sign
  its return addresses, so Qt's code in it (the interface's engine, and the network, JSON and picture
  readers that take in what KVMs and servers send) has no guard of its returns, where on x64 the
  processor's shadow stack guards them on a processor that has one. qKVM's own code has its guard where
  Windows turns it on, as Windows 11 on Arm did on the PC it was tried on. And the interface's script is
  interpreted there: what of it is not compiled runs slower than on x64 (the pictures do not go through
  it). SPEC.md section 3 says what each kind has.
- Known limit: Setup waits up to a minute for a start of qKVM made while it installs to end. An
  installation you run yourself says so on its own page; during that wait, and for some seconds after it,
  Windows may show Setup as not responding, and Setup ends by itself soon after: leave it be. One that
  still runs after that (held by a scanner of the PC, say) is never ended, and the update is not
  installed: Setup says why. Where that start still holds the program then, the earlier version cannot
  all be put back yet, and qKVM's next start says that an update did not finish: run the setup again,
  and it puts everything right.

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
  (`settings.json.before-<version>.bak`, never with a password in it). Reset saved secrets keeps a copy
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
