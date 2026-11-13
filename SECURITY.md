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
  shows, by any way the program offers.
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

## Checking what you install

- Each release lists the SHA-256 of its files in `SHA256SUMS`, on its GitHub release page.
  Check the setup before you run it, or the ZIP before you unpack it:

      Get-FileHash .\qKVM-Desktop-0.1.1-windows-x64-setup.exe -Algorithm SHA256
      Get-FileHash .\qKVM-Desktop-0.1.1-windows-x64.zip -Algorithm SHA256

  A different value means: do not run it.
- The program checks for updates against a feed signed with a key that is built into it, and
  checks a downloaded update against that signature, its SHA-256 and its size, wherever it
  came from. It follows no redirect but the one a GitHub release's download answers with,
  to GitHub's own file host. A copy the
  setup installed takes only a setup from its own feed, and runs that verified setup with its
  own rights when you choose Install and restart; the setup never asks for an administrator's.
  A copy from the ZIP shows you the verified file; it runs nothing it downloaded.
- Neither the setup nor the program is signed with a code-signing certificate yet, so Windows
  SmartScreen may warn before the first start of either.

## Supported versions

The newest release is the one that gets fixes. There is no promise of a response time; a
report is answered as soon as it has been read and understood.
