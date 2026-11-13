# Changes

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
