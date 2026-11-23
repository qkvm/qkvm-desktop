# Security

qKVM Desktop holds the passwords of your KVMs and can press the power button of the machines
behind them, so problems in it matter. Reports are welcome.

## Reporting a problem

Please do not open a public issue for something that could be used against other people's
computers. Report it privately instead: on https://github.com/qkvm/qkvm-desktop open the
*Security* tab and choose *Report a vulnerability*. A problem that is in the qKVM server as
well can be reported the same way on https://github.com/qkvm/qkvm.

Say which version you run (*About* in the program), what you did, and what happened. A way to
reproduce it helps most.

## What counts

- Acting on a machine while the program is locked, or while it is a wall screen that only
  shows, by any way the program offers. Known, and not counted: a press of a power button, a
  switch of computers, a waking or a start into BIOS setup that you asked for just before the
  lock, and that still waits for its device's sign-in or connection (a GL.iNet Comet's or a
  PiKVM's sign-in, a server controller's, a Proxmox host's question before its token goes, a
  JetKVM's connection), goes once that comes, after the lock. It is your own press, sent once.
  A typing or keys asked so are stopped at the lock.
- Making the program send a KVM's or a server's password to something other than the device
  it was saved for, or to an address that was not first asked what it is.
- Reading a saved password, a saved text or a server session from the program's settings,
  its logs, its crash summaries or its diagnostics.
- Making the program accept a device certificate other than the one it knows that device by,
  without being asked.
- Running code on the PC through the program, including through its update check or a file
  of a wall that it reads.

## What qKVM Desktop assumes

- Whoever is signed in to Windows as you, and every program that runs as you, is trusted.
  Windows keeps the program's secrets for that account, and another program running as the
  same person can ask Windows for them.
- **The app lock on a shared Windows account.** A passer-by has no control through qKVM's
  interface alone; with the Windows account, a Credential Manager Restore can replace the app
  password record and its retry count. Also, restoring an older backup resets the wrong-try
  count. Windows Credential Manager's *Back up Credentials* can export the program's secrets
  without the app password. Sealing the secrets under the app password is planned, not built.
- The first contact with a KVM is taken on trust, as the qKVM server takes it: the certificate
  it shows then is the one it is known by from then on. On a network you do not trust, compare
  the fingerprint the program shows with the one on the KVM's own page.
- A qKVM server the program signs in to is trusted with what it shows and what it is asked to
  do. Its accounts, roles and device passwords stay on that server; the program keeps none of
  that server's device passwords.
- **A BIOS setup boot override that a server's controller only queued.** Where a controller
  answers Start into BIOS setup by queueing the change (a 202, or a task), the change may be
  made at any time later, and the server's next boot may then land in its BIOS setup. qKVM marks
  the server's record queued and writes it down at once, at that answer; from 0.2.0 on already as
  the change goes, the mark taken off where the answer says the change was made or refused, so
  that a qKVM that ends before the answer (quit, a crash, Windows' restart) leaves it marked too.
  From 0.2.0 on, where
  the controller names the task, qKVM follows it, and only reads of the server's own computer
  that see this change through take the mark off (the change shown set after the task's end, or
  shown set and then taken as the server restarted). The task's own word never does, whatever it
  says: a board can say its task is done before the change shows. Otherwise no look and no time
  lets the mark go: the server's card says "The controller queued a change qKVM could not
  follow; check the boot override in its BMC" until a later start into BIOS setup on that
  server ends as it should (its change seen made, then the override seen off), or the server is
  taken off the wall.
  Check the override in the BMC yourself.

## What is kept, and where

- **Secrets** (the password of each KVM and server added on this PC, the text of each saved
  action, sessions with qKVM servers, display links) are in Windows Credential Manager, and
  nowhere else: not in the settings file, not in a log, not on a command line.
- **The program's own password** is never kept: only what scrypt makes of it, with its salt.
- **Settings** (the devices on this PC's wall, the servers, the layouts) are in
  `settings.json` in `%LOCALAPPDATA%\QKVM Desktop`. **Crash summaries, video diagnostics and
  update downloads** are under `%LOCALAPPDATA%\qKVM\Cache`. Nothing in them is a secret, and
  nothing is uploaded.
- **What is typed is never written down**: not keys, not pasted text, not passwords. The logs
  say that something was done, not what.
- **A file of the wall** exported with a passphrase holds the KVMs' passwords, sealed with a
  key that the passphrase locks (scrypt, AES-256-GCM). It is as safe as its passphrase.

## What qKVM Desktop connects to

- The KVMs and servers added in it, directly, at the addresses saved for them.
- The qKVM servers it is signed in to.
- While somebody searches for devices: the addresses of the private or link-local network
  chosen for the search (at most a /20), on the ports KVMs answer on. Only when asked.
- `download.qkvm.com`, to ask whether a newer release exists, and to fetch one when you ask
  for it (*Settings, Updates, Check automatically* switches the daily check off). Where the
  signed feed names a file of a GitHub release, the file comes from `github.com` and the one
  address GitHub sends the program on to, on GitHub's own file host.

Nothing else: no telemetry, no licence check, nothing that reports use. It takes no incoming
network connection.

## What it listens for on this PC

- A second start of the program, which can only ask the running copy to show its window, over
  a pipe that only your Windows account may open, named with your account's SID. It says so
  only to a copy that Windows says runs as you, and lets only that copy come to the front.
- One copy for each Windows account (0.2.0): which copy is the first in a Windows session is a
  mutex named with your account's SID, which only your account may use; which copy writes your
  settings, in whatever session of your account it runs (signed in at the PC and by Remote
  Desktop at once), is a lock on an empty file beside them, `settings.json.running`, which
  Windows lets go of when the copy ends. A second copy of your account in another session says
  so and ends; another account's copy in the same session (Run as different user) is that
  account's, with its own settings, and keeps nobody out. The mutex Setup and earlier versions
  look for, `Local\QKVM Desktop`, is still made while a copy runs, with a list that admits only
  your account.
- In a copy the setup installed (0.1.2 and later), the setup's request to close it for an
  upgrade: an event of your Windows session that only your Windows account may set, and that
  carries nothing. The program then ends the way Install and restart ends it, and only where
  nothing would be cut short and that setup still waits for it (one it gave up on is refused);
  otherwise it stays, and says what holds it in the title of a
  hidden window of its own, which nothing else may set. A copy from the ZIP listens for no such
  request. Any program running as you could ask qKVM to close this way, as it could end qKVM in
  any case.
- The setup ends qKVM by force only when you choose End it now, or when a silent setup is
  given `/FORCECLOSE=1`.
- While a setup or the uninstall works, a start of qKVM ends at once: the setup's mutex is
  there, and an event named with the number of a process that runs says that it is at work.
  The uninstall does not begin while a setup is open, or while qKVM runs: by its mutex, and
  (0.2.0) by the lock of its settings, which the uninstall then holds until it ends, so that it
  sees a copy of your account in any of its Windows sessions, and one that another account's
  copy keeps from the mutex's name. Where no copy of 0.2.0 or later has run for your account yet,
  the uninstall makes that empty file itself (and its folder where that is not there either),
  holds its lock all the same, and removes what it made as it ends: a first start in another
  Windows session during the uninstall then ends too.
- One setup or uninstall at a time for each Windows account (0.2.0), in whatever Windows session,
  and whatever kind of account it is (one of the PC, of a domain, or of Microsoft Entra ID):
  a mutex of the whole PC's names, named with your account's SID and made with Windows' own list
  of your account's, which each setup and uninstall takes as it begins and holds until it ends. A
  second one, at the PC or by Remote Desktop, waits a few seconds for it, then does not begin and
  says why. A start of qKVM takes it for an instant, and only then takes away a marker of an update
  whose record names nothing still set aside, so never one a setup has just written.
- Closed in 0.2.0: a start of qKVM in the instant between the uninstall's last look for it and
  the removal of its files. A start looks at the setup's gate again once its own mutex is there,
  and the uninstall makes its gate before it looks: one of the two always sees the other; and
  the uninstall holds the lock of the settings, which a start then cannot take. (A copy of 0.1.2
  or earlier, started from its ZIP, takes no such lock and looks once: it is not stopped so.)
- Known limit, which remains: the small update check that stands in the program's place while an
  update has the earlier version set aside looks for a setup at work in its own Windows session
  only, as the setup's mutex is that session's. Started from another session of your account
  during an update (Remote Desktop beside the PC), it shows its box there, and while that box is
  open its file is in use: the copy then cannot put the program in its place, the setup puts the
  earlier version back, and says so. Choose OK in that box, then run the setup again.
- Known limit, which remains: a program running as you that deliberately keeps the setup's
  handles open, or keeps its event until Windows gives that process number to another program,
  can delay a start of qKVM or the uninstall, or make a start take a setup to be at work. A
  program running as you is trusted (above), and could end qKVM in any case. The setup's own
  mutex and event are named by Inno Setup's rules, not by your account, so another account's
  program in the same Windows session could do the same. And another account's program on this
  PC could make the account's gate (above) under your account's name first, which it may then not
  open: your setups and uninstalls then do not begin, and say so, until it lets go; nothing of
  yours is changed.

## Checking what you install

- Each release lists the SHA-256 of its files in `SHA256SUMS`, on its GitHub release page.
  Check the setup before you run it, or the ZIP before you unpack it:

      Get-FileHash .\qKVM-Desktop-0.2.0-windows-x64-setup.exe -Algorithm SHA256
      Get-FileHash .\qKVM-Desktop-0.2.0-windows-x64.zip -Algorithm SHA256

  A different value means: do not run it.
- The program checks for updates against a feed signed with a key that is built into it, and
  checks a downloaded update against that signature, its SHA-256 and its size, wherever it
  came from. It follows no redirect but the one a GitHub release's download answers with,
  to GitHub's own file host. A copy the
  setup installed takes only a setup from its own feed, and runs that verified setup with its
  own rights when you choose Install and restart; the setup never asks for an administrator's.
  A copy from the ZIP shows you the verified file; it runs nothing it downloaded.
- An update never leaves the program a mix of two versions (0.2.0). Before it copies, the setup
  writes a marker in the program's folder (`qkvm-desktop.update-unfinished`) holding its record of
  which files it renames and with which ending, whole and on the disk before anything is renamed;
  then it renames each file of its own package that is there to its name and that ending
  (`.old-update`, or `.2.old-update` to `.9.old-update`: the first that no file of its package has
  beside it), beside it; after a whole copy, on the disk, it removes them, and where the copy does
  not complete it puts each back. A copy counts as whole only where every file of its package is
  there with the SHA-256 its manifest gives: one that went on without a file (its box's Skip this file),
  or with one of other bytes,
  puts the earlier version back too, and says so; so does a copy that Windows could not write to the
  disk, file by file, before the earlier version would go. Where a first install does not count,
  nothing is started. A put-back counts as whole only where every file
  its record names was seen and is as it should be: one that Windows will not let it look at, or a
  file only the newer copy made that it cannot remove, stays named in the record, which the next
  setup reads, and qKVM meanwhile says that an update did not finish. A file it set aside that cannot be removed after
  a whole copy (read-only, or held by another program) stays named in its record, and a later setup
  removes it; it never stops one. A setup ended halfway (the power gone) leaves them so: qKVM then says at its start
  that an update did not finish and starts nothing (while the earlier version is set aside, a small
  program of Windows' own calls alone, which the setup carries, stands in the program's place and
  says so; once the copy is whole, the program says so itself), and every setup, in any mode and before anything
  else (its refusal of an older version among it), first puts the earlier version back, or finishes
  the update where its copy was whole; where it cannot, it says why, and an update that cannot says
  so where qKVM shows it. What is the setup's own is decided by its package's list and the record
  together, never by an ending alone: it touches only what a record names, of the names of its own
  package and of what earlier versions of ours installed, made into it when it is built
  (`packaging/published-files.json`, from each published version's own package), never a pattern or
  a walk of the folder. A file of such an ending that no record names (a person's, or one left by
  anything else) it never touches, and it is never in the way, with one exception: where no marker
  of an update is there, `qkvm-desktop.exe.old-update` beside the program is taken for the earlier
  program as a setup of 0.1.2 set it aside, by that setup's own rule (removed where the program is
  there, renamed back where it is not), and only in a folder that holds an install of ours. A record
  it cannot read (a later version's, or not ours) it refuses, and names. After a whole copy it removes, by those exact names,
  what the earlier version installed and this one does not ship, and a folder only where that leaves
  it empty: only where that version's own record proves it was installed in that very folder (never
  on a first install, never in a folder another program has), by that version's own list. No folder
  is walked, plugins and qml among them. A marker whose record names nothing still set aside keeps nothing from starting: qKVM
  takes it away, only while no setup or uninstall of your account is at work. The setup holds its
  marker from its writing on, and sets nothing aside where it cannot. Where the earlier program itself is gone since it was set aside (0.1.2's own setup,
  run over a cut upgrade, removes it by its rule), the next setup puts back the rest, takes its own
  update check away from the program's place, and says so: the folder then holds no program until a
  setup completes. An update check of another version's setup is not told from a program, and stays.
- The setup installs only into a folder of its own (0.2.0): one that holds the marker of an
  install by qKVM's setup (`qkvm-desktop.installed`), or one that is not there yet or is empty, or
  one that holds nothing but what a setup of qKVM leaves there (the markers of an update, the files
  their records name with their endings, and folders of the package's own names: what an uninstall
  could not remove of what an update had set aside, which the next setup then removes). A folder
  that `/DIR` names and that holds anything else (another program's, your own files, a copy from
  the ZIP) is refused before anything is closed or changed, whatever its files are named, and
  nothing in it is set aside, written over or removed (SPEC.md, M4, says why every such folder).
  A record of what an update set aside is followed only in a folder of its own, which is decided
  first: in any other, nothing is put back or removed by it, and the setup does not begin, saying
  what is there. And only in a folder that no other account may change (below), which is decided
  before anything there is put right by a record: in any other, nothing in it is put right, and the
  setup refuses the folder before anything is closed or changed, saying that what an update left
  there stays as it is until then (or until qKVM is uninstalled). (To tell a folder of its own,
  the setup reads the records there before it judges the rights, and follows none of them then.)
  The uninstall removes what the records of its own folder name, whatever that folder's rights:
  only files of qKVM's own names, and those names with a set-aside ending, in the folder it empties.
- And only into a folder that your Windows account and Windows itself alone may change (0.2.0): a
  folder another account may write in could be given a library that qKVM then loads, and that
  would run as you. Refused before anything is closed or changed, naming the account: a folder
  whose list lets another account more than read and run what is in it, one whose owner is
  another, one not made yet below a folder whose such rights a new folder, or a file made in it,
  takes (Modify on "files only" among them), one below a
  folder that lets another account remove, rename or take it, a junction or a link, and a folder
  on a network share (whose share's own rights the setup cannot see). The setup never changes a
  folder's rights: they are yours or an administrator's to set. The default folder, in your own
  profile, is one wherever the profile keeps the rights Windows gives it.
- Neither the setup nor the program is signed with a code-signing certificate yet, so Windows
  SmartScreen may warn before the first start of either.

## Supported versions

The newest release is the one that gets fixes. There is no promise of a response time; a
report is answered as soon as it has been read and understood.
