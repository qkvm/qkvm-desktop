# QKVM Desktop: what it does

What the program is for, what it does at each step, and what proves a step done. How it is
built is in [DESIGN.md](DESIGN.md).

## Console Details increment, 2026-10-24

Details in the console combines current machine information and native stream
measurements with the selected server's history summary and sensor graphs. A personal
administrator also sees recent activity for that machine. Viewers and operators do
not request or retain the administrator activity page. Local-device consoles keep
native diagnostics; this increment adds no local history database.

The panel scrolls inside the console without moving its header or status strip.
Refresh is automatic only while open, with a manual Refresh command. Unknown history,
a failed read, a truncated sensor/activity page and missing audit entries are explained
rather than represented as complete empty history. Source, sign-in, permission,
machine, app-lock and hidden-window changes clear the memory-only results and reject
late replies. The full server activity search/export and state/event explorer remain
separate surfaces, not part of this panel.

## 1. Who uses it, and how

Somebody who looks after a handful to a few dozen machines behind IP KVMs and server
controllers, at home or in a small office or server room. They need nothing beside the
program: it speaks to their KVMs itself. (The owner's word of 2026-10-11. Until then this
document asked for a QKVM server, and M0 to M2 were built to that.) Whoever runs a QKVM
server as well has its wall in the program too. They use the program in two ways, and
both count the same:

* **At the desk.** A glance over all the machines, a double-click on one, and then typing
  and clicking on it as if sitting in front of it.
* **On the wall.** A screen nobody touches, showing every machine, for weeks.

Where the machines come from:

* **This PC.** KVMs are added in the program, by address and password. It signs in to
  them itself, keeps what it needs for that on this PC, and needs no server and no
  account anywhere. This is what the program starts into.
* **A QKVM server.** The program signs in to one and shows its wall, with what that
  account may do there. For whoever has one: for machines this PC cannot reach itself,
  for a wall that several people share, and for the kinds of device the program does not
  speak to yet.

The program learns to speak to devices one family at a time. First the KVMs that run
kvmd (PiKVM, GL.iNet Comet): M3. Then JetKVM (M6), then servers through their management
controllers (M7). Until its step is done, a device of a later family is on a wall
through a QKVM server, and the program says so where a device is added.

Nothing of the server's is carried inside the program: no Python, no engine beside it.

## 2. What must be true of it

In this order, when two of them pull apart:

1. **It tells the truth.** A tile never shows an old picture as if it were now. A machine
   that cannot be reached says so, in the words the page uses. A key that went down on the
   remote machine goes up again, whatever happens to the window.
2. **It stays up.** Weeks without growing. It gets over a lost network, a restarted
   server, a graphics driver replaced under it, a monitor unplugged, the PC sleeping.
3. **It is safe.** It keeps the rules of trust of section 8, towards a KVM as towards a
   server, and where it speaks to a QKVM server it is never a way around what an account
   may do.
4. **It is quick.** To start, to show a tile's new picture, and above all between a frame
   arriving from a controlled machine and that frame being on the monitor.
5. **It is one thing to look at.** One set of controls, all of the same shape, quiet.
6. **It is easy first.** Install, add a KVM by its address and its password, see the
   wall. Stricter ways are options, never the first thing a new user meets.

## 3. What being native buys

The reason this program exists. Each line is something a web page cannot do or cannot do
well, what Desktop does with it, and at which step.

| A native program can | The web page | What Desktop does | Step |
|---|---|---|---|
| Speak to the KVMs itself | Needs a server that somebody installs and keeps running | A wall with no server: KVMs are added in the program, which signs in to them itself | M3 |
| Receive every key: the Windows key, Alt+Tab, Alt+F4, Ctrl+W, Esc, Print Screen | Only in full screen, and never all of them | Sends them to the controlled machine, in a window as well as in full screen, as physical keys independent of keyboard layout | M3 |
| Read the mouse raw, hold it inside a window, hide it | With a prompt, and with a pause | Absolute and relative mouse, without prompts | M3 |
| Choose the decoder and own the path to the screen | The browser decides, and hides a buffer | Decodes on the graphics card, keeps the frame there, shows it the moment it arrives | M1, M3 |
| Run for weeks | Tabs are throttled, discarded, restarted with the browser | Starts with Windows, keeps the display awake, recovers by itself, watches its own memory | M2 |
| Put windows where it wants | One tab in one browser window | A wall per monitor, remembered per monitor, borderless full screen, a small always-on-top window for one machine | M2 |
| Live in the tray and speak when closed | Only while the tab is open | Tray icon that shows the worst state on the wall, toasts when a machine goes away or comes back | M2, M4 |
| Hold several places at once | Bound to one address | One wall over this PC's own machines and those of QKVM servers (home, office) | later (M3 shows one of them at a time) |
| Look at the network itself: multicast, UDP, the PC's own interfaces and routes, its Tailscale peers | None of it | Finds KVMs and server controllers from where the PC sits, across the networks the PC can reach (section 7) | M5 |
| Send to the local network | No UDP | Wake-on-LAN from the PC's own network, and "does it answer from here?" when a tile is offline | M5 |
| Keep secrets in the operating system | Cookies in a browser profile, readable by extensions | Sessions, and the KVMs' passwords, kept in Windows Credential Manager; nothing a browser extension can read or type into | M2, M3 |
| Know what the PC is doing: locked, asleep, on another network, on battery | Barely | Lets go of the controlled machine when the PC locks, pauses pictures when nothing is visible, reconnects at once when the network returns | M2, M3 |
| Use the disk and the clipboard freely | With prompts and limits | Screenshot to a file or the clipboard; paste the clipboard as typing | M3 |
| Be started with arguments, from a link, by a shortcut | No | `qkvm://` links and command-line switches to open a given wall or machine on a given monitor; a global shortcut to bring the wall up | later |
| Encode as well as decode | Limited | Recording a machine's screen to a file | later |
| Be the PC's own kind of program | The browser's | On Windows on ARM an ARM64 program of its own, never x64 under emulation: its code, Qt, FFmpeg (with its NEON code, for the tiles it decodes in software) and libdatachannel are ARM64, and it decodes on the graphics card through Direct3D 11 (D3D11VA) and draws with Direct3D 11 as on x64. It does the same on both, and its tests run on Windows on ARM itself (owner, 2026-11-19); how each kind is guarded against the misuse of its memory, and where ARM64 is weaker, is said below this table. Its interface script is interpreted there: Qt's own ARM64 build has no QML just-in-time compiler (Qt 6.12's x64 build has one), so what of the QML is not compiled to C++ runs slower on ARM64; the pictures do not go through it | 0.2.0 |

How each kind of the program is guarded against the misuse of its memory, as its files are built
(`tools/image-protections.json` names what each program and library of a package, and each of the two
update checks the setup carries, must have, read from its own bytes; `tools/package.ps1` makes no package,
its checks pass none, `tools/build_setup.ps1` makes no setup and the release audit's protections gate stays
open, refusing a beta, while a file lacks it; the setup's own loader and the uninstaller it leaves are Inno
Setup's programs, not read so; read of 0.2.0's packages on 2026-11-21):

- On both kinds every program and library is put in a place of memory chosen anew at each start, from the
  whole 64-bit room (ASLR, high entropy), and runs nothing from its data (DEP). Our own code, Qt's and the
  Visual C++ runtime's check every call through a pointer against their own table of what may be called
  (CFG). FFmpeg (built with MinGW-w64's compilers) and libdatachannel with Mbed TLS (built with their own
  options) have no CFG, on either kind.
- Returns, on x64: our program, Qt and the runtime are marked fit for the processor's shadow stack, by which
  Windows checks every return where the processor has one. A return address overwritten on the stack then
  ends a program of ours (seen on the build box; `test_base_guarded_returns`).
- Returns, on ARM64, which has no shadow stack: our own code and the Visual C++ runtime's alone. Most
  functions of our own (every one that keeps its return address on the stack: some 85 in 100 of the
  program's; a function that calls none keeps it in a register), and the runtime's, sign their return
  address with the processor's pointer authentication as they begin and check it before they return.
  Windows ends the program where the check fails, where Windows turns that check on: the ARM test PC's
  Windows 11 (26H2) does so for every program by default, and the program was seen ended there
  (`test_base_guarded_returns` tries it at every ARM64 run of the tests, and fails on a Windows that leaves
  it off). The program does not ask Windows for it. On a processor without pointer authentication those
  instructions do nothing. Qt's returns are not guarded so (below).
- What ARM64 lacks against x64: Qt's ARM64 build (Qt's own, as fetched) does not sign its returns, so Qt's
  code, the QML engine, the image plugins and the network and JSON code that read what servers and KVMs send
  among it, has no guard of its returns on ARM64, where on x64 the shadow stack guards them. FFmpeg's and
  libdatachannel's returns are guarded on neither kind.
- The x64 program run under Windows on ARM's emulation has no guard of its returns at all (the emulation
  gives it no shadow stack: seen on the ARM test PC), one more reason why Windows on ARM gets the ARM64
  program.

Being native buys nothing for a QKVM server's accounts, its settings and its activity
log: the page does those well, and Desktop sends people there until after M4.

## 4. The steps

### M0: toolchain and skeleton

* The folders of DESIGN.md section 2, each from the day it has something in it, a CMake
  build with presets (debug, release, and one with the address sanitizer), Qt 6.12 and
  FFmpeg each fetched by a script that checks what it downloaded against a recorded
  SHA-256.
* An empty program: a window in the wall's colours with the name on it, an About box that
  lists what ships with it and under which licence.
* `tools/check.ps1`: formats, lints, builds every preset, runs every test, and keeps its
  whole output in a file. It is what "it passes" means from here on.

Done when: on a PC that had none of it, the README's instructions lead to a running
program, and `tools/check.ps1` ends clean.

### M1: the engine, proven

The engine of DESIGN.md section 3 in `bench/`, a bare Win32 window with no Qt. Its
pictures come from files a script makes (moving test patterns and a "mostly still desktop
with a moving pointer" clip, as H.264 at 720p15, 1080p30 and 4K60, and as JPEG previews).
Nothing recorded from a real machine is ever committed.

1. 16, 36 and 64 tiles of: JPEG previews at 2 a second; H.264 720p15; H.264 1080p30. Each
   decoded on the graphics card, in software, and mixed. Recorded for each: processor
   load, the graphics card's decode and 3D load, graphics memory, pictures dropped.
2. One 4K60 stream alone in a second, borderless window, shown the moment each frame
   arrives; the time from "frame handed to the engine" to "present returned" recorded.
3. The graphics device taken away while 64 tiles play (`dxcap -forcetdr`): everything is
   back within seconds, by itself, with nothing leaked.
4. The window dragged between two monitors, and between two graphics adapters where the
   PC has two.
5. Three days unattended with 36 mixed tiles: handle count, committed memory and graphics
   memory logged every minute, and flat after the first hour.
6. The same engine under a Qt Quick window, with QML text and a button drawn over the
   video (DESIGN.md section 3.6): the cost of that, measured against the bare window, and
   step 3 repeated there.

Done when: the measurements report all six, the targets of section 9 are replaced
by numbers, and what could not be shown is said plainly. If step 6 fails, DESIGN.md names
the fallback; say so before going on.

### M2: the wall, on the server as it is

Everything here works with the server of today (section 6.1).

* **Connect.** The server's address; over HTTPS a certificate that Windows does not trust
  by itself is shown by its SHA-256 fingerprint the first time and remembered (section 8).
  Sign in with name, password and,
  where the account has one, the code; or with a display link, for a screen nobody sits
  at: the link is pasted where the address is typed, its key is kept in Windows Credential
  Manager, and it is what the screen comes back in with by itself when its session is
  gone. (A QKVM that is started the usual way knows after its restart whoever asked to be
  kept signed in, and the wall of such a one carries on by itself. One that is started
  with `--locked-start` knows nobody then, nor does any QKVM know a session that was not
  asked to be kept. A display link holds in every case: a wall that is to come back after
  a power cut whatever else is so is signed in with one.) Where the box "Keep this PC signed in for 30 days" is left ticked, the session is
  kept in Windows Credential Manager and the next start goes straight to the wall. The
  server that was used last is remembered either way, and gone to at the start.
* **The wall.** A tile for each machine the account may see, in the server's order, with
  what the page's tile shows: name, state, how long ago it was last seen, and for a
  machine without a picture, how it is. A server, seen through its controller, shows its
  own screen on its tile where the QKVM server reads one from the controller's console
  and the server runs, as a KVM's tile shows a screen; a fault does not cover it, the
  tile's edge says the rest. Switched off, or with no picture to be had, its tile says how
  the server is. Pictures come from the server's snapshots, asked
  for as the page asks for a tile of that size (twice its pixels, never under 480, so
  that the server makes one picture for a browser and this program looking at the same
  machine) and at the pace the server names; tiles that are not visible ask for nothing,
  nor does any tile of a window that is minimized or out of sight behind the program's
  icon, while how the machines are is still looked at.
  Four pictures are on their way at once and no more, each machine in its turn, so that
  what somebody does to a machine never waits behind pictures; a machine whose picture
  cannot be had is asked every three seconds and not at the wall's pace, and one whose
  stream is only being woken, a second later. All as the page keeps it.
* **Layouts.** The page's choices, from the bar's two menus, each kept on this PC. The
  groups: one group's machines or all of them; the button is there only where the wall's
  machines are in groups, and a group that is remembered and is not on the wall narrows
  nothing. "View": the order (as arranged on the server, by name, by group with the
  machines of no group last, problems first), and the columns (as many as fit, or one to
  six, which is the most there are: a narrow window shows fewer rather than cards under
  240 wide), and how often a new picture of each machine is asked for (the page's half a
  second to ten seconds, the server's own pace among them, which holds until another is
  chosen; and "Paused", which is for now, is never remembered, and is said on the bar
  where "View" stood: a wall that stands still looks like one that is up to date). A
  machine that is shown large gets its pictures also while the wall is paused. These
  stand in the place of the fixed grids (1, 4, 9 ... 64) that this document named before
  the owner said to lay the program out like the page. A menu of the bar that is longer
  than the window scrolls inside it. And the wall's own order, which is the server's and
  everybody's, made by hand as on the page: "Arrange the tiles…" in "View", for admins.
  Every machine is then shown, in the wall's own order, each card with its place on it:
  a card is dragged to another place (a line says where it would come), stepped one
  place earlier or later, or sent to the place that is typed into it. The bar rests
  meanwhile, and nothing else is done on the wall. "Save this order" at the foot of the
  wall sends it; the wall is in that order from that moment, in every window, and the
  order looked at is "As arranged". "Cancel", or Esc, leaves the wall as it was.
  Double-click, or Enter with the keyboard on a tile, enlarges it inside the window, in
  the wall's place, under a bar that says which machine and how it is; the arrow keys go
  to the machines beside it, Esc goes back.
* **Sources and saved workspaces.** Each desktop window chooses This PC or one saved
  QKVM server through View > Source for this window. Changing or losing one window's
  source leaves other sources alone. Windows remember that choice across restart.
  View > Saved workspaces (Ctrl+Shift+W) creates, updates, renames, deletes and opens
  named snapshots of all open windows: their source address, group, order, columns,
  group rotation and normal/maximized/full-screen position. At most 16 workspaces,
  eight windows per workspace and 80 characters per name; names are unique without
  regard to letter case. Source addresses are canonical HTTP(S) addresses, never
  credentials or display-link keys. A missing sign-in or changed certificate uses
  the existing connection flow. A removed monitor puts the window back in reach.
  Opening replaces the current layout, refuses during active control/actions or
  file/device operations, and clears enlarged consoles, selections, searches and
  display mode. It never takes keyboard or mouse control. Lock and wall-screen
  restrictions apply across all windows; workspace names and source choices are
  hidden and their commands refused while restricted. Signing out revokes all
  windows of that server immediately. Pending machine actions are shared by source
  and machine identity so two windows cannot send the same pending action twice.
  Background incidents have one observer per server even when several windows show
  it. Tray navigation prefers an already open window of the requested source, then
  the active window, and still checks the current account and inventory.
* **Combined source tree and shared live feeds.** View > Browse all sources
  (Ctrl+Shift+S), also available from the connection screen, lists This PC and every
  saved server with its connection state, groups and machines. Search matches
  sources, groups and machines, including collapsed branches. Selecting a source
  changes only this window; opening a machine checks the current account and fresh
  inventory first. Signed-out sources retain their reconnect entry but no machine
  names. Lock and wall-screen restrictions hide the tree and refuse its commands.
  Windows showing the same server machine through the same sign-in share one live
  connection. Different addresses or sign-ins never share it. Local devices retain
  their existing shared video connection. Each window decodes independently and
  keeps its own selection, controls and screenshots. The connection serves the
  largest requested size and frame rate; a joining H.264 viewer waits for a key
  frame. Closing one viewer leaves the others running; closing the last ends it.
  Only one window can hold keyboard and mouse on that connection. Outstanding
  control replies are settled before another window can take control; a release
  that is not confirmed within five seconds reconnects the video without granting
  control. Preview snapshots and foreground permission/status checks remain per window.
* **The wall as a file.** What "Export to a file" and "Import from a file" do on the
  page, through the same calls and with the same file (section 6.1), for admins, from
  the box of the bar's account button. (There. "Export…": what the file is to hold, the
  KVMs and servers, the saved actions, the alert settings, the wall settings; a
  passphrase for it, or none; and the password of whoever asks, once more. Where the
  file is kept is asked in Windows' own box, before anything is asked of the server,
  which then makes it. "Import…": a file is chosen, its passphrase is asked for where
  the file says of itself that it is encrypted, the server reads it and changes nothing,
  and what it holds is said: each device and each saved action by itself, what is on
  the wall already, what was left out and why, and what an action would type, to
  whoever asks to see it. Then the ticked parts are taken. What adds is ticked to begin
  with; what replaces a setting of the wall is not. The sentences are the page's. What
  a file says is shown with nothing in it unseen: a sign that draws nothing, or turns a
  line around, is written out as its number, and a console link is said by the place a
  browser would go to, in the letters that cannot be taken for others, or as something
  that is no plain address. A box that is closed keeps nothing and gives up what it had
  asked for; both close when the wall is left; neither is closed by a slip of the hand
  while the server works; the one that takes a file in stays until that is over, since
  it cannot be given up, and so does its window; and in a window too low for them, what
  they hold scrolls and their buttons stay. Tried against the stand-in server, with the place
  of a file named to the rig: a file with a passphrase and one without were made, read
  again, refused with a passphrase that was not theirs, and files with new machines and
  a saved action were taken in, which the wall showed by itself. Windows' own two boxes
  were seen to come up over the program's window and to go again; nobody has chosen a
  place or a file in them yet.)
* **Windows.** More than one wall window, each with its own layout and filter, each
  remembered on its monitor. F11 for borderless full screen. The display is kept awake
  while a wall is in full screen. (There: F11 and the bar's button, back to the window as
  it was, and the display and the PC kept awake meanwhile; and a window is where it was
  when the program was last closed, and as it was: of its own size, maximized with that
  size under it, or over the whole screen. A place that no monitor shows any more is not
  used. And more than one window: "New window" in the "View" menu, or Ctrl+N, opens
  another window of the wall, a step beside the one it was asked in, up to eight of
  them. Each has its own group, order and columns and its own place, all remembered; one
  that shows a single group says which in its title, so that the taskbar tells them
  apart. How often pictures are asked for, and a pause, are one choice for all of them.
  A machine is shown large, picked, or has its menu open in one window and not in the
  others; what one window sent to a machine is under way for all of them. A window that
  is closed while another is open is gone, the next time too. The last one is the
  program's: closing it ends the program, or puts it behind its icon where that was
  chosen. The windows that are open when the program ends are there again when it
  starts, each where it was. A picture is asked of the server once, as wide as the
  widest window draws it, and more often than the wall's pace only for a machine that
  is shown large.)
* **Doing things.** The page's menu for a machine: keys, text, saved actions, ports, wake,
  power, with the same confirmations, the same refusals and the same words. What the
  account may not do is not offered: whoever may only look finds no way into a menu.
  Who is signed in is asked again about once a minute, as the page asks: an admin may
  have changed elsewhere what this account may do, and whoever may act no longer has no
  menu left open.
  One thing at a time goes to a machine. What is sent and not answered is said as what
  it is ("it may have been done all the same"), and is never sent again by the program:
  a power button pressed twice undoes the first press. (There: the menu, from a machine's
  card and from the enlarged view, with the page's quick keys, the saved actions, the
  computers behind a KVM, wake, the power buttons, the locator light and the restart of
  the KVM or the controller; keys of one's own choosing and a text to type, each in its
  box, the text shown once more before it is typed; the question before what cannot be
  taken back; the notices in the corner; "Open console", for whoever may only look too:
  the machine's own console in the browser, entered signed in where a server's controller
  hands out a pass for that; and the card of a machine that was told to restart says so
  while it is away, instead of calling it an outage. And one thing done to several
  machines that were picked with "Select": their menu holds what any of them can do, and
  nothing that is one machine's own; whatever is chosen is asked about first, with each
  machine named and how it is; what can lose somebody's work (a danger, a power button, a
  text) takes a tick as well as the yes; it goes to a few at a time, or one after the
  other with a pause of the page's choosing; a box shows how each machine took it, lets
  the run be stopped, and offers once more what was refused or never sent, and never what
  got no answer. One run at a time, where the page lets a second begin beside the first:
  a second is said and not begun.)
* **The tray.** An icon that shows whether all is well; a menu to open a wall, to open the
  page in the browser, to quit. Start with Windows, as a setting. (There: the setting. The
  bar's settings button, and Ctrl+comma from anywhere, open a box with the program's own
  settings on this PC: "Start QKVM Desktop when I sign in to Windows", and the theme. The
  first is Windows' own list of what starts with a user, which takes no administrator;
  the program is never on it unasked. The tick shows what is so and not what was asked
  for: an entry that somebody switched off in Windows' settings, or one for a copy of the
  program somewhere else, is "off", and where Windows does not take a change the box says
  so. And the icon, in Windows' notification area: the program's mark, with an amber dot
  while a machine has no signal and a red one while something wants attention or QKVM
  does not answer; a pointer that rests on it is told the bar's own counts. A click on it
  brings the program's windows to the front as they were. Its menu: "Open QKVM Desktop", "Open the QKVM
  page" while somebody is signed in, and "Quit". A second box in the settings, not ticked
  until somebody ticks it: "Keep running in the notification area when the window is
  closed". The program's last window is then put out of sight and not closed, the icon
  goes on saying how the wall is, and "Quit" ends the program. Only while Windows has the icon: without
  it nothing would bring the window back, and closing closes. Windows puts a new icon
  among its hidden ones until whoever sits at the PC drags it out. The same mark is the
  window's own icon. One copy of the program runs at a time for whoever is signed in to
  Windows: started again, it brings the windows of the copy that runs to the front, out
  of the notification area too, and ends. Where Windows does not let it see whether a copy
  runs (one of another account in the same session, Run as different user), it says so in
  plain words and ends, never taking itself for the only one. And while nobody can see the wall (every
  window is minimized, or out of sight behind the icon), Windows shows a message when a machine
  changes state or recovers, and when QKVM stops answering or answers again; a third box
  in the settings switches that off. Settings > Alerts and incidents records offline,
  no-signal/powered-off, refused-login, device-error and server-health problems, their
  start, latest change and recovery, and local acknowledgement. A further bad state
  updates the same open incident and clears acknowledgement. Removing a device closes
  its incident as no longer monitored, without pretending it recovered. All active
  incidents and the latest 1,000 closed incidents are retained across restarts.
  Two consecutive completed status observations settle a change; initial states are
  recorded quietly. Restored states receive a two-minute startup/reconnect grace,
  ending earlier when an observation agrees with the saved state. A source outage is
  one server incident and does not change cached machine states. One observation batch
  produces at most one notification. A one-hour maintenance pause for the wall or one
  device still records history; resume reports only changes that still apply.
  Notifications require every wall window hidden, notifications enabled and the app
  unlocked. History continues when locked or notifications are disabled, but cannot be
  read or changed through the locked/display-only interface. History is separate for
  this PC and each server/account; server records are shown only for the current
  authorized inventory. An unreadable history file is preserved, with explicit backup
  and restart; failed writes are reported and retried. Saved local KVMs and up to 32
  saved servers are monitored independently of the selected wall while the app runs.
  Background servers use saved sessions and existing certificate checks, never display
  links or interactive credentials. Sign-out stops that server; reset stops all sources.
  Notifications retain their source/account and machine identity, verify the account
  and fresh inventory before opening, wait for app unlock, and never take input control.
  Grouped and source alerts open that source's incidents. External delivery and physical
  resident-app acceptance remain open.)
* **Favorites and recent machines.** The star on a card and the enlarged console's
  Add/Remove favorite button keep up to 32 favorites on this PC. The tray has Favorites
  and Recent machines submenus, with the source beside each machine and a clear command
  for each list. Recents are the last ten distinct machines opened in the desktop
  console, newest first; background monitoring does not add visits. Only machine
  identities are saved. Server identities include the server address and account as
  a hash; current names and availability come from authenticated inventories. Lists
  are hidden and cannot be changed while locked or showing a display link. A missing
  or unauthorized machine is omitted, while its bounded saved reference is retained
  for when it becomes available again. Opening uses notification navigation's fresh
  account and inventory checks and never takes input control. Favorites and recents
  are local preferences, not part of portable wall export.
* **When things go wrong.** Server gone, signed out, certificate changed, account
  suspended: each has its own plain sentence and its own way back. Tiles keep their last
  picture, marked as old. (There: while QKVM does not answer, a line over the wall says
  so in the page's words and since when, with the day, for a wall that is read days
  later; every card's screen is dimmed, and so is a machine that is shown large, so that
  no picture of a while ago is taken for the machine as it is; and the wall carries on
  by itself when QKVM answers again, with nobody asked for anything while the session
  still holds. The wall says that contact is lost where a look at its machines is not
  answered by the QKVM, and that it is back when such a look is answered again. One
  machine's picture that cannot be had, one that is too large or too slow among them,
  is that machine's and no more: the wall does not say that contact is lost for it,
  and a machine that is shown large stays live. Signed out is only the QKVM's own
  word for it. Where something else stands at the server's address for a while and
  wants a sign-in of its own for whatever it is asked (a 401 that is not in the
  QKVM's shape), the wall says that contact is lost, nobody is asked for a password,
  and the wall carries on with the session it had when the QKVM is back. A QKVM that
  comes back speaking another version of its API is said where a server is named,
  with the versions it speaks, and the wall is left. And a wall that came by a
  display link says that contact is back only when
  the link was taken again and a look at the machines was answered after that: where
  a QKVM takes the link and then does not know the session it gave for it, the wall
  goes on saying that contact is lost, and since when it first was.)

Done when: against `python -m tests.demo` every tile state the stand-ins produce looks
and reads as on the page; the wall survives the stand-in server being stopped and started;
a wall of 64 stand-in tiles meets section 9; and the owner has looked at it. (The wall of
64 was measured: section 9. It meets the processor and
the still wall, and misses memory, the start and the late frames, each narrowly or by a
choice that is the owner's.)

### M3: this PC's own KVMs, and one of them live

The program speaks to KVMs itself, and first to those that run kvmd (PiKVM, GL.iNet
Comet): section 6.3. It is built and tried against made-up KVMs, which speak as real
ones do (the server's own tests have them), and then on a real KVM the owner names. No
QKVM server runs anywhere while it is tried. Three parts, in this order.

Where it stands on 2026-10-18. Built, and tried against made-up KVMs: of M3a the start
into this PC's own wall, the box that adds a KVM and the box that changes one, what is
kept, the wall, its menu, a tile's ways back, the list of its KVMs in the settings,
and, in the core, the saved actions kept
on this PC, which are run from the wall; of M3b a KVM's live picture in the enlarged
view, the line that says keys and the mouse to a KVM, and keyboard and mouse taken in
the window that shows a machine large; of M3c the ways
between this PC's wall and a QKVM server's. The taking of keyboard and mouse is tested
against stand-ins only: in the tests its keys are pressed by hand; the part that hears
this PC's keys from Windows (the hook) is built and linked, and no test installs or
runs it. It has run once for real: the owner started the program, took keyboard and
mouse of a real KVM, and reported that keys arrive. Nothing else is known from that
run. Seen on one real KVM, which its owner named, apart from that run: it was asked
what it is, signed in to under the certificate it showed, its resting video woken,
looked at, pictures taken, and the line for keyboard and mouse let in and held; it was
had live for ten seconds without an error, by its pictures asked one after the other
(asked as fast as they come, it gave about fifty a second over the local network, of a
screen that stood still: section 6.3); one key and one small move of the pointer were
said to its computer by the owner's leave, and were not seen to arrive. Not seen: a
live picture of a screen that moves, the mouse arriving, and of the keys anything
beyond the owner's word that they arrive. Of M3c's wall as a file, the file itself is
read and written by the program (the server's own format, sealed files and plain ones,
held against files the server made and read back by the server's own reader); and this
PC's wall is taken in from such a file and written to one by the core
(`core/wall_exchange`), against made-up KVMs, and by the two boxes for it, "Export..." and
"Import..." in the settings' "Export and import", which are tried by tests only.
The 2026-10-21 increment adds the saved-action editor and app lock, then local action
and shared-setting import/export and clipboard typing. Settings replacement is opt-in;
text actions and passwords require a passphrase on export. Clipboard text is previewed
and confirmed before it is sent. The next increment adds screenshot save/copy and relative
mouse capture, tested with synthetic pictures and simulated input. The 2026-10-22 increment
adds a release-key setting (Right Ctrl, Scroll Lock or F12) and an Always on top console
switch. Real Windows mouse and device acceptance
remain open. The decision for several
sources at once is taken (M3c, "Its wall"), and the program's insides are being made
for it, and nothing a person sees has changed yet.
What is open in what is built is in DESIGN.md section 11.

**M3a: KVMs added on this PC.**

* **The start.** A program that knows no KVM and no QKVM server opens with two ways on:
  add a KVM, or connect to a QKVM server. From then on it starts into what it showed
  last: this PC's wall, or that server's.
* **Adding a KVM.** Its address and the password it is signed in to with; a name and a
  group where wanted. Before any password goes out, the program asks what answers at
  that address, and says plainly when it is not a KVM it speaks to: nothing at all,
  something else, or a device of a family that is not built yet, with the way that
  works today (a QKVM server). The certificate the KVM shows at that first contact is
  what it is known by from then on (section 8). A KVM that is on the wall already is
  said, and not added twice. A second box, of the same make, changes a KVM's address,
  name, user name, group and password, takes it off the wall, and shows the certificate
  it is known by beside the one it shows now, to be said yes to.
* **What is kept.** The list of KVMs, with what each is known by, in the program's
  settings on this PC. Each password in Windows Credential Manager and nowhere else
  (section 8), together with the address and
  the certificate it was saved for: one that was saved for something else is not used.
* **The wall.** As M2 has it for a server's machines, tile for tile: state, picture,
  when last seen, what is known of the machine, the page's words for each. Pictures are
  asked of each KVM itself, at the wall's pace, and never more of a KVM than it gives
  without falling behind. The order is this PC's own, made by hand as in M2. No KVM is
  signed in to before it was asked what it is (section 8). At the first start with a
  wall that was made before that rule, each KVM is asked once, and its tile says
  "Connecting..." meanwhile: about a second for a KVM that is well, and a KVM that
  takes three seconds to let a line in was online after about 9.5 s in the test.
* **Doing things.** The machine's menu of M2, with what a KVM can do and nothing that it
  cannot (section 5, "Only what a machine can do"): keys, a text to type, saved actions
  (kept on this PC), the computers behind a KVM with several, wake, the power buttons
  where they are wired, a restart of the KVM. One thing at a time goes to a machine, and
  what got no answer is never sent again, as in M2. Saved actions are kept on this PC,
  beside its KVMs, by the server's rules for them, and are run on any KVM of its wall;
  the box that adds, changes and removes them comes with the screens.
* **Who may.** Whoever sits at this PC, signed in to Windows, may do everything on this
  PC's wall, but while this PC is a wall screen, which only shows (section 8). It has
  no accounts and no roles: those are a QKVM server's.
* **When things go wrong.** A KVM that does not answer, one that refuses the password,
  one that shows another certificate than it is known by: each has the page's sentence
  and its own way back, on its tile, and none of them holds up the rest of the wall.
  A password that was refused is not tried again and again: a KVM locks an account out.

Done when: against the made-up KVMs every tile state they produce looks and reads as on
the page, with no server running; a KVM that goes away and comes back, and one whose
password is changed under the program, are said and got over; a wall of 64 made-up KVMs
meets section 9; nothing of a password is in a file, a log or a command line; and the
owner has added a real KVM and looked at it.

**M3b: one machine, live.**

Console alignment (2026-10-23): machine identity and the principal controls stay in
the main header during viewing, admission and control. Each control row has its own
measured overflow menu; the bottom strip keeps video and ownership status together.
The floating keyboard is movable/resizable, bounded to the console, and offers a
reset position. Its modifiers are local, one-shot selections. Every screen chord
requires existing ownership and sends balanced presses/releases; it never requests
or forces ownership. A local panel releases held input and temporarily stops physical
capture without releasing the server lease. Focus/permission/connection loss still
ends control. Relative control can begin with the keyboard already open; closing
the panel resumes capture. Keyboard positions are US USB usages and the remote
layout determines text. Panel geometry is session-local. Full server Details,
history and activity parity remains separate work.

The picture comes from the KVM itself, the best way it has (section 6.3): its stream of
JPEGs where it serves one; where it serves none, its pictures asked for one after the
other as it captured them, which a KVM answers in a few hundredths of a second on a
local network. Since 2026-10-20, the native C++ receiver negotiates H.264 over WebRTC
through kvmd's Janus service, shares it between viewers, and falls back to JPEG while
reconnecting. Local tests pass encrypted H.264 into the existing decoder; physical-device
acceptance remains open. For a machine of a
QKVM server's wall it needs the live connection of section 6.2. Both the desktop
and the web app implement it; joint acceptance remains open.
The desktop's stand-in is not that acceptance.

* **Live picture** in the enlarged view and in a window of its own: H.264 decoded on the
  graphics card, each frame shown as it arrives, nothing queued behind it. (There, in
  the enlarged view: a KVM's stream of JPEGs where it serves one, and where it serves
  none its pictures one after the other, at most twenty a second. Two windows that show
  one KVM large share what is asked of it. Native H.264 from kvmd Janus is integrated
  and tested against local peers. Not there: a window of its own; real-device H.264
  and graphics-card performance acceptance are still pending.)
* **Keyboard.** While the machine is being controlled, every key goes to it as a physical
  key. One key is kept back to leave control (Right Ctrl by default, configurable in
  Settings), and it is always named on screen. Ctrl+Alt+Del cannot be caught by any program: it is a button,
  and the leave-key with Del. (There: "Control" in the bar of the enlarged view takes
  them, of a KVM of this PC's wall as of a server's machine that is had live. While the
  machine is asked, the bar says so and offers to stop asking. The machine has 65
  seconds to answer. A yes that comes later than that, or after a key was pressed in
  the window, something else was opened in it, or it was no longer in front, is given
  back at once, and the bar says which of these it was. While the machine still owes
  an answer, to an asking that is over or to a giving back, "Control" does nothing:
  until the answer has come, and for seventy seconds from an asking and five from a giving
  back at the most (five more where a yes still comes late and is given back). A KVM
  of this PC's own answers every asking and every giving back, so there nothing is
  waited out, and "Control" is offered again at once, however they ended; a server
  may say less, and the time is for that. Behind why an asking was given up, the bar
  says when Control can be clicked again; and where nothing else says so, it says
  after half a second why Control is not offered. From the machine's yes
  on, every key goes to it as the physical key it is. The leave-key defaults to right Ctrl;
  Settings > General can choose Scroll Lock or F12 instead. Changing it ends control
  and cancels pending requests in every window. The
  bar names it while they are taken, with the button "Give back" beside it, always:
  whether the keyboard has that key cannot be known. Pressed and let go by itself it
  gives them back; with Del it is Ctrl+Alt+Del, which the bar has as a button too; with
  any other key it does nothing, and neither does that key until it was let go.
  Some keys are kept back from the machine, each until it is let go: one that was down
  when they were taken, one whose press went to Windows, and one that was held when
  they were last given back and was not heard to be let go since. While such a key is a
  Ctrl, a Shift, an Alt or a Windows key, no other key and no click goes to the
  machine, and a line in the bar names that key and says what ends it. The Windows key
  goes to the machine once what comes next shows that it is no Win+L, which locks this
  PC whoever it was meant for. What another program types into this PC (a
  remote-control program, a keyboard shared over the network, the on-screen keyboard)
  is not sent to the machine, and the bar says so from the first such key.
  Windows gives two accounts of the keyboard, and the program holds one against the
  other, so that it is noticed, at the first key, when Windows stops handing it keys.
  Where the two differ, keyboard and mouse are given back, and the bar says which of
  the two told of a key that the other did not, and what may be why (Windows no longer
  hands the program its keys, or no longer reports the keyboard to it; a key remapper;
  one of Windows' key helpers), in one of three sentences. Where the second account
  begins to report the keyboard only after they were taken (a keyboard may have been
  plugged in), and a key of that first moment cannot be checked, they are given back
  once, with a sentence of its own. Where this PC gives no second account, the bar
  says, while they are taken, that it would not be noticed if Windows stopped passing
  keys on.
  Keyboards are not told apart: a key goes up on the machine when any keyboard lets
  go of it. With two keyboards in use at once (a second one, a macro pad, a laptop's
  own), a Ctrl, a Shift, an Alt or a Windows key that is held on the one is let go by
  a tap of the same key on the other, and the next key from the first arrives bare:
  it is pressed again. That cost is known and accepted. The other side to fail on is
  a modifier that stays down on a server with nothing said.
  A keyboard that is unplugged, or switched to another computer, while they are taken
  ends them, where Windows says that it went (of a keyboard whose wireless link drops
  behind a receiver that stays plugged in it says nothing, and a key held on it stays
  down on the machine until they are given back): keyboard and mouse
  are given back, every key on the machine is let go, and the bar says why, whichever
  keyboard it was and whether a key was held on it or not (Windows counts some mice,
  headsets and docks as keyboards).
  Where this PC does not say when a keyboard goes, the bar says, while they are taken,
  that it would not be noticed. A keyboard that drops out without a word from Windows
  (as a wireless one may that sleeps or goes out of reach behind its receiver, and
  some desk switches) leaves a key that was held on it down on the machine, until
  that key is pressed and let go on another keyboard or "Give back" is clicked. None
  of this was tried with a real keyboard.
  They end by themselves, each time with every key on the machine let go first and the
  reason in the bar: when no key was pressed and the mouse not used for fifteen minutes;
  when Windows shows a screen of its own (the lock screen, Ctrl+Alt+Del, a question
  about administrator rights), or the keys are pressed that Windows itself acts on
  (Win+L, Ctrl+Alt+Del); when keys could not be passed on in time; when the program
  stopped looking at the keys; when a keyboard is taken away from this PC; and when
  the two accounts differ.
  The release-key choices are tested with keys that a test presses by hand.
  What hears this PC's keys from Windows, the hook, is built and linked, and no test
  installs or runs it. It has run once for real, when the owner took keyboard and
  mouse of a real KVM and reported that keys arrive; nothing else is known from that
  run. What is open: DESIGN.md section 11.)
* **Mouse.** Absolute by default, relative as a switch, the pointer held in the picture
  while relative. Wheel and five buttons. (There: absolute, by where the pointer is over
  the picture, at most sixty places a second; five buttons; the wheel. A click or a turn
  of the wheel goes to the machine behind everything of the keyboard that was heard
  before it, a key let go as well as a key pressed: Shift and then a click is no bare
  click, and Ctrl let go and then a click is no click with Ctrl. Not there: relative,
  and the pointer held in the picture. Tested against stand-ins only; nothing is known
  of the mouse from the one real run.)
* **Letting go.** When the window loses focus, the PC locks, the connection drops or the
  program ends: every key and button that is down on the remote machine is released first.
  (There, beneath the window: the line to a KVM, which says keys, buttons and the
  pointer as kvmd's own page says them, for one holder at a time, and lets go of
  everything that is down before it is given back or ended from here. Where the line
  ends by itself, that is the KVM's to do; and where something could not be let go
  from here, or the line then went without the KVM's answer to the goodbye it was
  let go behind, a KVM's line says that it may still be down on the machine, at
  once or some seconds later. The bar then says that something which was pressed may
  still be down on this machine: behind why keyboard and mouse went, or, where the word
  comes while they are taken again, as a line of its own. It stays until they are next
  taken, or another machine is shown. A line to a QKVM server never says it (section
  6.2). And in the window: whichever way keyboard
  and mouse end there, the machine lets go of everything first: given back, the
  leave-key, the window no longer in front, another machine or none shown, the line
  gone, nobody at the keyboard or the mouse for fifteen minutes, the keys of this PC no
  longer heard as they should be, a keyboard taken away from this PC, the window
  closed. That the PC locks is heard of
  through the hook: that is in no test, and nothing is known of it from the hook's one
  real run. Nothing of its own is there for sleep.)
* **Small things.** Paste the clipboard as typing (through the text action, never
  logged); a screenshot to a file or the clipboard; a line that can be switched on showing
  frames a second, decode time, how the picture is decoded. Clipboard typing is built
  through Type text: an explicit clipboard read, at most 2,000 Unicode characters,
  Enter unchecked, then the normal confirmation; no automatic clipboard synchronization.
  Video details is built. Screenshots remain to be implemented. Physical acceptance
  remains separate from the isolated automated checks.

Done when: on a made-up KVM, and then on a real KVM the owner names, typing is right on
keyboard layouts other than US, the Windows key and Alt+Tab act on the remote machine,
nothing stays pressed after Alt+Tab away or a pulled network cable, and the time from
frame to monitor meets section 9.

**M3c: a QKVM server beside it.**

* **Its wall.** "Connect to a QKVM server" at the start and from the account button:
  everything of M2, as built. And back to this PC's wall from the same place. One wall
  at a time, as built. What comes is several sources at once, in one device tree: this
  PC and any number of QKVM servers, each server with its own sign-in (the owner's word
  of 2026-10-17; the program takes no connection from another PC, and a wall is shared
  through a QKVM server). It is under way: the wall holds several sources at once (DESIGN.md section 5, "The sources
  behind the wall"); nothing of it is on the screen yet.
* **Its machines, taken over.** The wall as a file (M2) for this PC's wall too, with the
  same file, which the program then writes and reads itself. So a server's KVMs come to
  this PC with their passwords, through a file that server made with a passphrase, and
  this PC's go to a server the same way. A device of a family the program does not speak
  to yet is said, and left in the file. The file's KVMs are laid out one by one before
  anything is taken, each with its address, how it is spoken to and what it will be
  known by, and only those that were laid out and chosen are taken. Each is asked what
  it is before its password goes anywhere (section 8). A KVM whose file names no
  certificate is not known by what it shows first: it is taken on Windows' word, and
  where Windows does not trust what it shows, a person looks at its certificate first.
  Both boxes, "Export..." and "Import...", are reached from the settings, in the first
  line of their section "Export and import", which is there where the window shows this
  PC's wall; they open over the settings and close back to them.

Done when: a file made by the stand-in server, with a passphrase, is read by the program
with no server running, and its KVMs are on this PC's wall and answer; a file the program
made is taken in by the stand-in server; and a wrong passphrase, a damaged file and a
file from a newer QKVM are each refused in the page's words.

**M3d: the program's own password** (the owner's decision of 2026-10-18; section 8 has the
rules). Made at the first start, asked where section 8 says, "Lock" in the menu, a wall
screen that only a person with the password signs out, and the reset of a forgotten one.

Implemented in the 2026-10-21 development source together with Settings > Saved
actions. Settings > App lock manages automatic sign-in and password changes;
the View menu and Ctrl+L lock the program.

Done when: a program that was locked, or is a wall screen, asks nothing new of any machine
until the password is given, also after it was ended and started again, also where it
signs in by itself (what was asked before the lock and still waits for its device's sign-in or
connection may go after it, as the end of this paragraph says); a sealed export is not written without it;
and a reset leaves no secret of the program's behind. A text a driver of this PC's own wall
types key by key (a JetKVM's, a VNC server's, a guest's console, a server's console) stops where it stands
when the program is locked or made a wall screen, with nothing down, and the Enter asked
after it is not pressed; a typing that waited behind it is answered "stopped" with none of
its keys typed (the outside review of the wave 2 desktop, 6.18; a JetKVM's: review cx6-1,
M3). A kvmd is handed the text whole (its `/api/hid/print`), types it by itself, and is not
stopped once it has it; a press of keys together is as short as it is.
Before kvmd has it, while its driver waits for a sign-in (its own, or another driver's of
that KVM, which may take seconds), a text, keys pressed together or Ctrl+Alt+Del asked before
the lock is answered "stopped" at the lock and never sent, nor the text's Enter (review
cx6-2, L3). What else was asked before the lock and waits so (a press of a power button, a
switch of computers, a waking) goes once the sign-in comes, after the lock: the person's own
press, asked once and not doubled, which SECURITY.md names as known (the merged review of
0.2.0, B L4; stopping those at the lock too is left for after 0.2.0). So does a press that
waits for another device's sign-in or connection: a server controller's power command or start
into BIOS setup waiting for its session (`RedfishDevice::withToken`), a Proxmox guest's press
waiting for the question asked before its token goes, a JetKVM's press waiting for its connection
(round 2 of the merged review, L3).

### M4: ready to hand out

A per-user installer that needs no administrator; a signed program and installer; updates
that are checked by a signature of the project's own before anything is run; an About box
with every licence and where the sources of Qt and FFmpeg are offered; local crash
diagnostics that contain no credentials or typed text and are sent nowhere.
The initial implementation writes bounded technical summaries and an unfinished-session
marker, not raw memory dumps. Capture has platform limits; severe corruption and
failure before startup may leave no detailed report.

Settled by the owner on 2026-10-20: check automatically and notify; download and
install only when the user chooses. Settings > Updates also offers a manual check,
release notes, progress and cancellation. No automatic check interrupts control,
opens an installer or restarts the program. The first signed-feed client is built.
The Windows installer now implements explicit setup,
journalled recovery and compatible-version rollback. The current build is unsigned;
signed clean-machine acceptance remains required. Cache data now uses package-owned
storage for Windows uninstall cleanup, and local crash summaries are implemented.
Source and notice work, signing, update hosting and installed-Windows acceptance remain open.
This implementation does not yet complete M4.

Settled by the owner on 2026-11-08: a copy that qKVM's own Setup installed (per user, with
no administrator) updates itself in place. Such a copy reads its own feed, beside the one
portable copies read, and takes only Setup's release. Settings > Updates offers "Install
and restart" once that release is downloaded and verified: only when the person asks, and
not while control, actions, file operations or virtual media are under way in any window, an
action sent to a machine is unanswered, a server's start into BIOS setup is under way (a change
of its override may still come, for as long as its doubt runs on a clock that nothing puts right,
whatever the wall clock does meanwhile; a record left from before whose doubt is over by the wall
clock, or reaches further than any press notes, holds nothing), or a
box holds changes that are not saved; it says which. The program
verifies the setup again, through a handle that lets nothing change, rename or remove it
until the program ends, and starts it from that very file, which lies alone in a folder of
the program's own update cache that only the person (and Windows' own administrators) may
change. Setup is told only the folder the program runs from and where to write its log.
The program then closes its windows and ends: its goodbyes to devices are given their time,
and it ends within 9 seconds after them, and 50 seconds in any case; Setup waits for that,
installs into the same folder, and starts qKVM again. The next start says "Updated to"
the new version, or that the update was not installed, with the place of Setup's log, and
offers it again; where Setup found qKVM still running, it says so. A hand-over to a version
older than the one that starts is one a newer install has gone past since (that version's own
setup, or a copy of the zip): it is let go of, with nothing said and nothing looked for.
Setup unpacks itself in a
folder of the program's cache that only the person may change, not in a shared Temp. While
Setup is at work, qKVM started by hand ends at once, without a word or a window; only
Setup's own start at its end waits for it to finish, and then starts qKVM anew as the person
would. Setup waits for such a start to end before it judges its copy, a minute at the most:
the first start of a copy just written can take seconds while Windows' scanner looks at each
new library it maps. In an interactive install it says so on a page of its own ("Waiting for qKVM Desktop, started
during the install, to end"); that page takes no message while Setup waits, so Windows may show Setup as not
responding until the wait ends, within the minute, by itself (an update runs silently and shows no page). It never
ends one: one that still runs after the minute makes the copy not count, and Setup ends with 7 and says so; where that start still holds the program's file
then, the earlier version cannot all be put back yet, and the marker of an unfinished update
stays: the program's next start says that an update did not finish, and the next setup puts
it right, as where any file is held through a put-back (the outside review of 0.2.0, 1.5).
Data that is there but cannot be read is never taken for none: qKVM says so, and
where, and ends without writing anything. A downgrade is never offered, and the same
version only to an x64 copy on Windows on Arm (below). A portable copy's update is still only shown, verified, and
never run; an MSIX copy's still goes to qKVM Setup.

Windows on Arm (the owner, 2026-11-19): a person there ends up with the ARM64 program by every road, and x64 users see
no change. Every copy asks Windows what the PC is (IsWow64Process2's native machine; where Windows has no such call or
it fails, the copy takes its own kind) and takes only the release for the PC's own kind of Windows, by the signed
payload's platform, `windows-x64-<kind>` or `windows-arm64-<kind>`, letter for letter. On x64 a copy reads the feeds
0.1.0 to 0.1.2 read (`stable.json`, `stable-setup.json` for a Setup copy); on Windows on Arm `stable-arm64.json` and
`stable-arm64-setup.json`, beside them. An x64 copy of 0.2.0 or later on Windows on Arm (a ZIP, which Windows runs
under its emulation) is offered the ARM64 release of its own version as well as of a later one, and says that it
is for Windows on Arm; a copy of the PC's own kind is never offered its own version, on x64 and on Arm alike. A copy of
0.1.x on Windows on Arm is a ZIP's (their setups refused Arm): it takes the x64 0.2.0 ZIP, as it takes every release,
and that copy then offers the ARM64 one. The note of a hand-over to Setup says the kind of Windows its release is for,
and the next start says "Updated to" only where the program that starts is of that kind. qKVM's MSIX installer takes
only the package for the PC's own kind (no MSIX is published).

One setup for both kinds (0.2.0, the owner, 2026-11-19): `qKVM-Desktop-<version>-windows-setup.exe` carries x64's
program and Windows on Arm's, and puts in the PC's own: the ARM64 program on Windows on Arm, the x64 one everywhere
else (where Windows cannot run x64 programs, it installs nothing and says why). Over a copy of the other kind it puts in this
kind's files, and once the whole copy is in, what only the other kind had put there goes, by that version's own list,
where this user's Settings, Apps entry names that version installed in that folder (the setup's own record of an
install; the program's kind it reads from the program itself).
No setup of qKVM makes such a copy: 0.1.0 to 0.1.2's refuse Windows on Arm, and this one installs the PC's own kind. One
is there only where an install was carried over from a PC of the other kind: an x64 install on Windows on Arm (0.1.x's,
or a later version's), which Windows runs under its emulation. Carried over with its Settings, Apps entry, by a tool
that moves installed programs to a new PC, it is upgraded as above. Its folder copied by hand, the setup's marker in
it but no entry, it is taken for no install of a known version: the setup installs over it, and what only the other
kind had stays, unused (the ARM64 program loads no x64 library). The setup's suite (`tools/test_setup_vm.ps1`) runs
on Windows 11 on Arm too, and plants an x64 install there with its entry, to upgrade it, and to cut and put back such
an upgrade (outside review of 0.2.0, 1.4).
It notes in Settings, Apps which kind it installed. A release publishes x64's ZIP, Windows on Arm's ZIP
(`-windows-arm64.zip`) and the one setup, with the four feeds above.

The setup installs only into a folder of its own (0.2.0, review i020-2 M1): one that holds the
marker of an install by qKVM's setup (`qkvm-desktop.installed`, `setup` and a line feed), or one
that is not there yet or holds nothing, or one that holds nothing but what a setup of qKVM leaves
there (the markers of an update whose records read, the files those records name with their
endings, folders of the package's own names: what an uninstall could not remove of what an update
had set aside, which the setup then removes, putting nothing back). A record of what an update set
aside is followed only in a folder of its own, decided first; in any other the setup touches
nothing and does not begin (outside review of 0.2.0, 1.1 and 1.8). And only in a folder that no other
account may change (below), decided before anything there is put right by a record: in any other the
setup's start puts nothing right and looks for nothing to put right, and Preparing to Install, which
reads the records there to tell a folder of its own (`ForeignFolderReason`) before it judges the
rights and follows none of them, refuses the folder, saying that what an update left there stays as
it is until no other account may change the folder, or until qKVM is uninstalled (outside review 2 of
0.2.0, 1.3). The uninstall removes what the records of its own folder name whatever that folder's
rights: only files of qKVM's own names, and those names with a set-aside ending, in the folder it
empties. A folder that holds anything else, as an install told
`/DIR` may name, is refused in Preparing to Install before anything is closed or changed (exit
code 7 and the result line), and nothing in it is set aside, written over or removed. Every such
folder is refused, not only one whose files share a name with the package's, because:
* the package's names change with each version, so a rule by names would hold only until the next
  library came, and the set-aside and the copy would then rename and write over another program's
  file of that name, which the uninstall would afterwards remove as qKVM's;
* a program's own folder is the first place Windows looks for the libraries it loads, so qKVM's
  libraries beside another program's (or another's beside qKVM's) may be loaded by the wrong
  program whatever their names;
* a folder the setup did not make keeps the rights it was made with, which may let others change
  the program (the reason the folder is never asked in the wizard), and an uninstall removes only
  what the setup wrote, so the folder and the files of the other program stay;
* and for the same rights, a folder is refused, empty or of its own, where another account than the
  person's may change what is in it (by its list or its owner, the list of the folder a new one is
  made in, by what the new folder or the files made in it take of it (outside review 2 of 0.2.0,
  1.2), a folder above it that lets another account remove, rename or take it, a redirection, or
  a network share); the setup never changes a folder's rights to make it do (outside review of
  0.2.0, 1.2): they are the person's or an administrator's. Known limit, accepted: a folder not made
  yet is judged as it is to be made, and another account that may make folders (only) in the folder
  above could make it first, in the instant between Preparing to Install and the copy;
* a copy from the ZIP is such a folder too, and it stays the portable copy it is.

Done when: on a clean Windows 10 and a clean Windows 11, install, update and uninstall
work without administrator rights, and uninstall leaves nothing but the settings.

The setup and the uninstall take the person's Windows account whatever its kind: one of the PC or
of a domain (`S-1-5-21-...`), or of Microsoft Entra ID, work or school (`S-1-12-1-...`), read
where Windows' own record of the account points (outside review 2 of 0.2.0, 1.1: 0.2.0's
candidate took only the first kind, and refused every setup and uninstall of the other). The
test machines' accounts are local ones: what an Entra ID account's SID is read as is shown by
the setup's own routine run on made-up records of Windows' kind, not on a PC joined to Entra ID.

### M5: finding devices

Section 7.

### M6: JetKVM, by itself

A JetKVM speaks WebRTC and nothing else: its pictures, still or live, and its keys all
come over one such connection, and it lets one viewer in at a time. The program needs a
library for that which it does not have. That
library is in the installer, is loaded when the first JetKVM is added, and never before.
Until this step a JetKVM is on a wall through a QKVM server.

Done so far (2026-10-28, increment 1 of the step): a JetKVM is a family of this PC's own
wall ("jetkvm" in the settings and in a wall's file). It is added in the Add KVM box, asked
what it is first, both ways where it may answer both (plain HTTP, and HTTPS beside it: as
section 8 says), and signed in to by a driver of its own (its one password; one out of its
box, which runs without one, is asked as one that needs none); it is taken from a wall's
file, changed, and looked at: whether it is online (its version is not shown yet: it comes
with its picture, in increment 2). A KVM of the one family where one of the other answers is
said to be that ("other_family") and hears no password. Its picture, its keyboard and mouse
and its actions say "not yet" on its card: through a QKVM server it has them all. Following
one onto HTTPS by itself comes with the next step.

Since increment 2 (2g, 2026-10-31): its picture. Its card shows the newest key frame of its
video (a card that asks connects; a look never does), and shown large every picture of the
same one connection, which its cards and windows share; the connection is given back after
five minutes with nobody watching. Where its own page (or another program) takes its one
viewer, the program steps aside and stays aside, across a change of the KVM, until "Show
here". How it is comes over that connection while one is open. Its keyboard and mouse and
its actions but "Show here" still say "not yet" (increments 3 and 4). "Show here" (2h) is the
button on the card of any machine in use elsewhere, of this PC's wall or a QKVM server's, as
on the page: asked first, and once done every window that shows it has it live again.

Since 0.2.0's second wave (A3, 2026-11-17): its virtual media, as the web's 0.3.0 has them (jetkvm.py, "virtual
media"; `core/jetkvm_media`, `jetkvm_upload.cpp`), over the `rpc` channel of the connection its card holds, which is
only ever arranged with the JetKVM it was added for (its number is asked before every connection). What is in its
drive (`getVirtualMediaState`) is asked beside every look that goes over that connection, and is on its card after
such a look; one that went the plain way (its connection given back for nobody watching) says nothing of the drive,
while its card still offers its media. One that has no such method (an older JetKVM) offers none. Its Media box lists
its own storage (`listStorageFiles`,
`getStorageSpace`), mounts an image read-only as a CD-ROM drive or a disk (`mountWithStorage`, waited for 30 s; never
a USB flash drive, never writable, never started from; 300 sectors at least as a CD-ROM drive), restarts the machine
by its ATX extension where that is on and asked (not where a window had its keyboard and mouse as the change began:
that is looked at once, then, and a window that takes them during the mount, up to 30 s, does not stop the restart;
the outside review of the wave 2 desktop, 4.17, a look again right before the press left for after 0.2.0), takes it out
(`unmountImage`) and deletes one (`deleteStorageFile`, not while it is in the drive); a change between CD-ROM and disk
drops its keyboard and mouse for a moment, which the box says first. An image is uploaded from this PC: opened over
the connection (`startStorageFileUpload`; only one it opens anew goes on), its bytes by `POST
/storage/upload?uploadId=` on a line of their own after the JetKVM that answers was asked again which it is (`GET
/device`) under the session they go with, the asking and the bytes both at the one address the name of its address
was found at; streamed from the file as it is read, 16 GB at the most, hashed on its way, one at a time, done only
where every byte went and its hash is of every byte read, and held to its listing it whole afterwards; what an upload
cut off, stopped, not taken whole or whose start got no answer in time leaves (`<name>.incomplete`) is taken off
again, and said where it cannot be. A message on `rpc` larger than 64 KiB (a list of a few hundred long names) is let
go, and the call that waited for it says that limit was met. Never `mountWithHTTP`, `checkMountUrl`, its built-in
images or `setMassStorageMode`: nothing is fetched from an address. Tried against a JetKVM by hand (the web's
stand-in's storage); no real JetKVM was asked.

Where a JetKVM's media here do otherwise than the web's 0.3.0 (review c8b-1):
* The asking of which JetKVM it is before an upload and the upload's bytes go to the one address the name was found
  at, its Host header and the name its certificate is held to (and said to it) still the name: two JetKVMs that answer
  to one name never have the asking go to one and the bytes, with the session, to the other. Stricter than the web,
  whose bytes' connection finds the name anew.
* An upload's waits: its bytes have 300 s of quiet either way, from the connecting on, and a day all told. The web
  has 4 s to connect, 60 s for a JetKVM that takes no bytes, and 300 s for its answer after the last byte. So a JetKVM
  that stops taking bytes holds an upload here five minutes, not one; and the bytes' line, which is made right after
  the asking of which JetKVM it is found the JetKVM there (that asking has the usual 4.5 s to be let in), waits as long
  as Windows takes to give up (about 21 s) where it never comes up all the same, not 4 s.
* An upload the JetKVM answered "Upload completed" to before every byte went, and was read and hashed, is not done:
  discarded, and said. The web asserts there. (Qt answers no reply before the whole body is written, so it is a rule
  for a JetKVM that is none.)
* A start that got no answer in time has what it may have opened taken off; the web leaves it, and the next upload of
  that name is refused for a part of an earlier one.
* A message on `rpc` larger than 64 KiB is let go and said; the web ends the connection.
* What is in its drive is on its card only after a look over its connection (above); the web's every look connects.
* An image mounted from an address at its own page is never taken for an image of its storage named "(an address)":
  no SHA-256 of it, no "It was mounted so already", no refusal to delete that one as mounted. The web takes the one
  for the other.
* An image mounted from an address goes by an id of that mount's own, "(an address)/" and 16 hex digits of an
  HMAC-SHA256 of what the JetKVM says of it under a key made at random in each run of the program (which says nothing
  of the address): an unmount asked of one is refused where one from another address is in the drive by then, and
  nothing is taken out (outside review 4.4). The web's id is "(an address)" for every one.
* The restart after a mount asks its ATX extension anew whether the machine is off (`getATXState`), and presses its
  power button only where it plainly says so, else its reset button (outside review 4.3): a machine switched on at
  its front panel during the mount is not shut down. The web goes by what its last look saw. "Off" is its power
  light's word, and a power light whose lead is not connected always reads off: one whose disk light is lit at that
  moment is on, and is reset (review cx4-1 L4). One whose extension does not say in time is reset, and the box says
  that whether it was on was not known (a reset does nothing to a machine that is off).
  A running machine whose power light reads off and whose disk is idle at that moment (or whose disk light is driven
  by no disk it has) reads off all the same, and has its power button pressed, which shuts it down (review cx4-2 L5).
  So it is also where its screen shows a picture: a picture holds a kvmd's press back (section 6.3), never a
  JetKVM's. The JetKVM keeps this rule (the owner's decision of 2026-11-19; kvmd's was made for one test board whose
  power light was not wired). Its card said nothing of its power before the mount where it showed a picture: a
  JetKVM's power light off beside a picture is said neither "Power off" nor "Power unknown", and a JetKVM makes no
  note of why. The CHANGELOG says this limit.
* What it says in a shape the program does not read is never taken for the reassuring (outside review 4.6, 4.7;
  review cx4-1 M2): a state of its drive that is neither null nor an object, or that says no `source` of the
  firmware's three ("Storage", "HTTP", "WebRTC"), is no empty drive and no image from an address (a deletion, a mount
  and an unmount are refused while it is so), and one that names neither `CDROM` nor `Disk` is neither drive; an image
  of its storage in the drive whose name it does not say as one the program hands back goes by an id of its own, never
  the name it reads as ("an image" where it says none), so that it is no "It was mounted so already" and has no
  SHA-256 of that one, and no image is deleted while it is in (it may be that one), while it is taken out by the id
  the box listed; one sent from a browser at its own page is none of its storage, said so; a list of its storage that
  is no list, or has an entry whose name cannot be read, or more than 2,000, never says an image is not there, is
  gone, or that nothing of an upload is left (a deletion, an upload and a discard say that it is not known), and its
  box says that not every image on it may be shown (review cx4-1 L5). The web
  takes each for the reassuring one. (A call answered with no `result` at all is read as null, an empty drive, as
  the web reads it, and on purpose: the connection hands both on alike, and an empty drive may come so, the
  firmware's answer leaving out a `result` that is nothing, so reading it as a drive that cannot be read could refuse
  every mount of an empty drive. The firmware refuses a second mount while one is in; what is left is a deletion of
  the image in the drive, under a firmware that broke its own protocol so.)
* A session it refused for an upload (401) is let go of, so that the upload tried again goes under one given anew
  (outside review 4.8); the web keeps it until its connection is made anew. The JetKVM has one session for
  everybody, so that sign-in anew, the person's own try again, ends the session of whoever signed in at its own page
  meanwhile, and the words say so (review cx4-1 L6); nothing signs in anew by itself for an upload.
* An image that it lists after its upload at another size than went is said so, and is not called nothing.
* A mount or unmount that drops its keyboard and mouse for a moment is not held while a window of this PC has them:
  the web holds it only for somebody else, and here every window is the asker's own (the box says it first). A
  restart from it is refused where a window has them as the change begins, as the web's; it is not looked at again
  before the press (above).
* Its drive and whether it is writable are checked once, against what it offers, in the driver; the web checks them
  in its server and again in its driver.
* A person's asking (keys, a press, its media) asks again at once which JetKVM answers only where another one did; the
  web's urgent asking passes over every pause.
* The uploader is "this PC", and nothing says who mounted an image or when: there are no accounts here.
* What its notes of carried images know it by is its address and its number; the web's is its tile and address.
* An upload stopped is said "The upload was stopped before all of it had gone." (`desktop:stopped`, or
  `action_failed` where something is left), as a kvmd's here; the web says "...before the image came in full."
  (`disconnected`).
* Its card is neither changed nor taken off while an upload goes to it; the web lets an edit be made.

### M7: servers, by themselves

Through their management controllers: how the server is, its power buttons and its
sensors, and its own screen where the controller's console can be read, with a decoder
of the program's own. Whatever touches a controller is tried by the owner. Until this
step a server is on a wall through a QKVM server.

Done so far: the controller's driver (Redfish sessions only, never Basic; which server
answers; how it is; its buttons and its light; the way into its console), and a server on
this PC's own wall (part 1b-i):
* Added in the Add box as a KVM is: what answers is asked first (its service root waited
  for as its driver waits for it, an older board taking nine to twenty seconds to show it),
  then signed in to once, and that session given back; named by the name its computer goes
  by, or by where it is.
* Its card shows how the server is, and its own screen where its controller's console can
  be read (AMI's firmware; part 1c): a still of it on the card, made smaller as the web
  makes its pictures of it, and the screen live in its large view, with keyboard and mouse
  as the console allows; keys and a typed text from its menu go over the same console (one
  console for all of it, the owner's decision of 2026-10-29), never the keyboard's Power key.
  The console's manners are the web's: somebody else at it is not asked for it, the session
  the controller ends after a while is opened again, and a console that would switch the
  server's own monitor off is left alone; the card says why its screen is not shown for as
  long as that stands. One seat of a controller's console is held at a time by every driver
  of it in the program (review 3's MEDIUM 2): one made anew as its card changes waits for the
  old one's goodbye before it signs in to the controller's page again; the controller is
  found by its address and by its own number, never by its certificate. Its buttons are the
  web's server tiles' (power on, shut down, hold power, reset, restart the controller, the
  light); "Open console" opens its controller's own page, plain (no pass in an address: the
  owner's word). Its frames are read, and
  the card's still made smaller, on a thread of their own (review 3's HIGH 1, 2026-11-06:
  a busy 1080p frame held the interface thread about 46 ms on a slow core): the interface
  thread only takes them off the console's line, two wait to be read at the most while the
  controller is held back, and a screen whose thread was busy is shown the newest picture
  with all that changed meanwhile.
* OpenBMC's console (from 0.2.0, as the web's `openbmc.py`; `core/rfb_screen`, the `rfb/`
  reader): a controller whose manager says it runs OpenBMC (`Model` "OpenBmc", or an
  `Oem.OpenBmc` part) and announces an enabled graphical console of the KVMIP kind has its
  console at bmcweb's `/kvm/0`, RFB over a WebSocket. Its line is opened under the driver's
  own Redfish session (its token as `X-Auth-Token`), and where a number is kept, only under
  the session its computer was found to be the card's under. No password goes over RFB
  (security None alone: a console that asks for VNC authentication is told nothing), and
  AMI's web page is never signed in to on a controller that says it runs OpenBMC, whether it
  announces a console or not. A session is held only while somebody looks, and let go of a
  minute after the last look; one at a time; one that came to nothing is asked for again on
  the controller's console ladder (30 s, 1, 2 and 5 minutes). Its pictures are read off the
  interface thread and held to the web's bounds and drawing share (half a drawing thread,
  counted on the reading thread's own processor time), a Tight JPEG decoded only where it is
  baseline: a costly screen is shown more slowly, and a session whose single steps take two
  seconds of that thread more than three times in a minute is ended, the server at fault.
  Keys and the pointer go as RFB's events, from 1.5 s after the handshake; a text by each
  character's keysym on the US layout, in pieces that leave nothing down; the Power key
  never; and a typing on it stops at the lock as every console's does (M3d). It comes with no
  words of the controller's about the server's own monitor, and is opened without them, as
  the web opens it (section 8). Tried against a stand-in of bmcweb's `/kvm/0` and
  obmc-ikvm's RFB on this PC's loopback address.
* A server is known by the numbers its computer and its controller give for themselves,
  kept with its card as digests (the web's `machine`), and never by its certificate: its
  maker may have put one certificate, and one key, on every board. So its kept password goes
  to its own address and to no other, whatever certificate shows there; a second card of the
  same server, by its computer's number, is refused, and the web's "keep both" is not offered
  (the owner's word); two whose computers give no number are two. Another server at its
  address is said on its card and sent nothing; the change box takes the server that answers
  now only where its password is typed. A card that keeps no numbers yet takes the server
  that answers first, and keeps it. Beside them, where its controller keeps that server's
  computer, and whether that controller lists several (section 8, from 0.2.0): kept and
  taken as the numbers are, so that another node of a chassis never passes for its computer,
  but in the one place section 8 names, where no number the program keeps can tell.
* At the program's end every driver of the wall and of its boxes is let go of before the
  program waits for the controllers' goodbyes, so that each session is given back.
* From a wall's file (part 1b-ii): with the numbers its server is kept by (`machine`, as
  the web writes it; a file of 0.1.x, which kept them as what the server is, is read so),
  asked what it is before its password goes, and held to them; two of one certificate are
  two, and none is kept beside its twin. The wall's own file carries them, and not where its
  controller keeps the server's computer: a card from a file learns that anew (section 8).
* In the boxes and the lists, called what the page calls it: "Server (Redfish)".

Keys and the pointer over a controller's console (its part 4) never leave anything down
on purpose: what is let go of is said even behind what waits to leave, and is watched by
where it ends among all that was said; where it has not left in time while the line goes
on, the console's session is ended. That last step rests on an assumption of the web's,
that an AMI board lets go of what a session had down as the session ends: it is not yet
seen on a real board, and the tests that end a session for it ("too late") prove the
program's half only.

The order of M4 to M7 is the owner's to change.

### Later, not designed yet

Tours (a tile that walks through machines). Recording. Links and command-line switches. A QKVM server's accounts
and settings inside the program. Linux. (Windows on ARM came in 0.2.0: section 3.)

## 5. The screens

The look comes from the page: its colours, its sizes and its words are in
[the web app's `qkvm/static/app.css`](https://github.com/qkvm/qkvm/blob/main/qkvm/static/app.css) (the variables at the top, for
dark and for light) and [`app.js`](https://github.com/qkvm/qkvm/blob/main/qkvm/static/app.js). So does the layout: the
owner said on 2026-10-09 that the program is to be laid out like the page as the live demo
shows it (demo.qkvm.com). That is:

* **The top bar**, one row: the mark and the name (which opens About); the wall's states as
  one control with their counts (All, Online, No signal, Attention); "Find a KVM"; the
  groups; then at the right "View" (order, columns, how often, and display mode),
  "Select", the
  blue "Add KVMs", and small buttons for full screen, theme, settings and the account; the
  time of day at the very end. ("Add KVMs" opens, on this PC's wall, the box where a KVM
  is added (M3a). On a QKVM server's wall it leads to that QKVM's own page in the
  browser, where its machines are added, and is there only for whoever manages it. The theme
  button goes from dark to light to "as Windows is set" and round again, as the page's
  does, and the choice is kept on this PC. "Select", for whoever may act, begins the
  picking of machines for something done to several of them: every card shows whether it
  is picked, a click picks it or lets go of it and nothing on a card does anything else,
  and a bar at the foot of the wall says how many are picked, picks all that are shown or
  none, opens what can be done to them ("Run action…"), and leaves. The page's keys: S,
  Esc, and "/" for "Find a KVM". Settings opens the program's own settings.)
* **Display mode**, the page's: the screens and nothing else, for a monitor on the wall.
  From the View menu's last row and with D; no bar, and the cards as large as lets all of
  them be seen, whatever number of columns was chosen. A small bar of its own at the top
  right says which group is shown, has "Rotate groups" where the wall has more than one
  (each group for twenty seconds; that they take turns is kept on this PC, the group whose
  turn it is is not the chosen one and is not), and "Exit (Esc)"; it shows as display
  mode begins and for three seconds whenever the pointer moves, and while the keyboard is
  in it. Out of the way it is not gone: a click where it lies brings it back and does
  nothing else, so that its buttons are not pressed by somebody who cannot see them. (Here
  the program differs from the page, whose faded bar lets such a click through to the
  card under it.) The groups wait while a machine is shown large or a box lies over the wall. A
  screen that is opened with a display link begins in it, and so does every window for
  as long as this PC is a wall screen (section 8): "Exit (Esc)" leaves it there too,
  and nothing can be done all the same. Whoever signs in with an account, at a PC that
  is no wall screen, does not.
* **The wall**, a grid of cards that fills the width. A card is the machine's picture with
  rounded corners, and under it one line: a dot for the state, the name, the address in grey,
  and at the right what is known (size, power, temperature). A machine with several
  computers behind it has their numbers on the picture. A card without a picture says why in
  its middle ("Connecting...", "Offline" with when it was last seen, "Login failed" with the
  way to mend it); a server's card shows On or Off large, with health, temperature and fans.

* **Only what a machine can do.** The program asks what a machine can do and shows that,
  and nothing else. In three ways:
  1. *By machine.* A card, a menu and the enlarged view have a button only where that
     machine has what the button needs: power where it is wired, the computers behind a
     KVM only where there are several, "take control" where keys and mouse can be sent.
     A machine of a QKVM server's wall can do what that server offers for it.
  2. *By family.* Settings and menus that belong to one family of device are there from
     the first device of that family on the wall, and gone with the last. Whoever has no
     JetKVM reads nothing about JetKVM.
  3. *By part of the program.* What only one family needs is loaded when its first device
     is connected to. On a PC without such a device it takes no memory and reads nothing
     from the network. Every such part is in the installer; none is fetched later.

The owner's taste, settled over many rounds on the page, holds here too:

* Crisp controls, all of the same shape (one corner radius, one height), dark by default,
  with light and "as Windows" as choices. A soft glow in the background, nothing louder.
* The top bar is one row. What does not fit goes into menus, not onto a second row.
* A section's main button sits at the top right of its title. No text hangs under buttons.
* Picking several things at once happens in a select mode that is entered on purpose.
* Quiet: no badges that shout, no animation for its own sake. Movement respects the
  Windows setting that turns animations off.

| Screen | What is on it |
|---|---|
| Start | Only where nothing is known yet: "Add a KVM", and "Connect to a QKVM server". |
| Add a KVM | Address, password, and where wanted a name and a group. What is added, as the page chooses it (from 0.2.0): KVMs and servers, told apart by what answers; VNC servers, which are asked nothing of the kind, have no user name, and take an address without `vnc://` as a `vnc://` one; or virtual machines on Proxmox VE, signed in to with an API token (its ID where the user name goes, its secret where the password goes), whose virtual machines the token may see are listed to tick in a list of one height that alone scrolls, and added one card each from what was listed (an address with a guest's number after it, `192.168.1.5/100`, adds that one at once). The address's hint stands in a place as high as the tallest of the three choices' hints. The box keeps one size and its controls their places whichever is chosen and whatever is said of an adding (the boxes-hold-still rule): the lines and what is said under them take as much as a Proxmox's guests need, as far as the window has room, and of KVMs and VNC servers what came of an adding has the rest; its main button is as wide with either of its words. A guest named by its address is a run of its own, said from its own line. An address that begins with `vnc://` or `vncs://` is a VNC server's where KVMs and servers are chosen too; where Proxmox's guests are, it is a Proxmox's address whose guests are asked for, as the page asks them, and refused. One that begins with `vnc` or `vncs` (in any case) and a colon or a slash but not `://` is refused whichever is chosen, as a scheme written amiss, and not read as a host called `vnc` or `vncs` (stricter than the web, which reads it so); a host that is called so is written with its scheme (`vnc://vnc:5901`, or `https://vnc:8443` for a KVM), as the refusal says. A VNC server's host is written as a KVM's, as the web's `vnc_url` writes it by its `device_url`: letters of the twenty-six, digits, dots, hyphens and the underscore (a name of other letters is refused as typed so, and is written in its `xn--` form), or numbers written the one way; so no sign that Qt drops from a name or makes another letter of (a soft hyphen, a sign of no width, letters of the full width) makes another host of what was typed. Stricter than the web's `vnc_url`, which reads each of these, none of them to another host than was typed: a colon with no port behind it (`box:` is `box` to the web); a question mark or a cross anywhere (the web ends the host and its port at either, so that `vnc://vncs:?/10.0.0.6` is a server at a host called `vncs` to it, and `10.0.0.6?:5901` one at port 5900); a tab, a line's end or another sign of control anywhere (the web drops them, or strips them at the ends, and reads on); a name Qt will not take (a part that begins or ends with a hyphen, or is longer than 63 signs, or two dots together); and what Qt reads as numbers where the web reads a name (`0b0`, `123`). Taken both ways and written otherwise: an IPv4 address within IPv6 is `[::ffff:127.0.0.1]` here, which the web may write as `[::ffff:7f00:1]`; a wall's file is read either way. What answered there, and what the KVM is known by. For a KVM that is on the wall: the same, to change it or to take it off (a KVM's own box, which makes no KVM a VNC server, says of a VNC server's scheme written amiss only that a KVM at a host of that name is written with `https://` before it). A VNC server's address stands whole in its box (`vncs://` and all); moved, it takes its password typed again, as the web's `known_again` (for `https://` alone) has it, whatever certificate shows there (one with none kept is moved with none); a `vncs://` one is not changed to `vnc://` at its own host and port, and is moved to `vnc://` anywhere else (an address typed without a scheme among them, and its own machine written another way) only once its box has shown what that gives up and somebody has said yes to that very address, as an HTTPS KVM to plain HTTP elsewhere (stricter than the web, which asks nothing there); and its certificate is looked at, compared and said yes to as an HTTPS KVM's, a yes being one for that very `vncs://` address. A password or a passphrase that is typed, here and in every other box, is hidden, and has an eye beside it that shows it until it is pressed again, the box is sent, or the box is left, as on the page. |
| Connect | A QKVM server's address, then what the server asks for (first-run setup is done on the page: say so and offer to open it), name, password, code, "keep me signed in"; or a display link. The certificate's fingerprint the first time. |
| Wall | Top bar: whose wall (this PC's, or which server's and which account), layout, group filter, select mode, a menu for the rest. The tiles. A status line only when something is wrong. |
| One machine | The picture as large as it goes, keeping its shape. A thin bar: name, state, take or leave control, Ctrl+Alt+Del, the machine's menu, full screen. While controlling: the leave-key, always visible. Ctrl+Alt+Del and Alt+F4 are asked first, as on the page ("Ctrl+Alt+Del on "NAS"?", "Alt+F4 on "NAS"? It closes the window that is in front there."), from the bar's buttons, from the menu of the keys that do not fit, and from the keyboard on the screen (Ctrl and Alt with Delete, Alt with F4); nothing goes before the yes, only to the machine it was asked for while its keyboard and mouse are still taken, and the keys are this PC's while it is asked. Not asked: the leave-key with Del, which is the program's own Ctrl+Alt+Del on purpose (M3, Keyboard); an Alt+F4, or any chord, pressed on this PC's keyboard while the keys are taken, which goes to the machine as every key does then, as a KVM viewer's captured keyboard does; and a saved action, asked only where it is marked dangerous or types text, as on the page. |
| Your account | On a server's wall: who is signed in, and to which QKVM; "Sign out". For admins, the wall as a file: "Export…" and "Import…", each a box of its own, as on the page. On this PC's wall ("This PC"): that it is this PC's own wall, and "Connect to a QKVM server". (This PC's wall as a file is in the settings.) |
| Machine menu | As on the page: keys, text, saved actions, ports, wake, power. Dangerous ones ask, naming the machine. Where it is longer than the window, it scrolls and follows the keyboard. |
| Notices | Lower right corner, over everything: what was done, what was refused and why, what is under way. They go by themselves; what is under way stays until it is over. |
| Settings | One box in the page's shape, from the bar's button and Ctrl+, anywhere: the sections at the left under small group headings, the one shown marked (in a row above the section in a narrow window), the arrow keys from one to the next. It opens where it was left, also after a restart, and keeps one size and place while it is open. Every section has one shape: a first line with its name, a short sentence and what one does in it as a whole, which stays in sight while the rest scrolls; under it, framed lines (what it is at the left, what one does about it at the right) and lists. Where the window shows this PC's wall, "This PC's wall": KVMs, each KVM a row with its name, where it is and how it is spoken to, "Same device as" another and "No password saved" where so, "Edit" and "Remove" (which asks first), and "Add KVMs"; and Export and import: what a file of this wall holds, of either kind, what taking one in does, "Export..." and "Import...". Always, "This program": General (the theme; release key; going on behind the icon in the notification area; Windows' messages of the wall; start with Windows), Video (hardware acceleration: Automatic, On or Off, taken up by every window at each stream's next key frame, which is asked for where the stream's way of decoding changes; the graphics card to draw and decode on: the most powerful, the one that saves power, or as set in Windows, each shown with the name of the card it comes to on this PC and, where the screen is on another adapter than that one, that Windows carries every picture to it; taken up at the next start) and About, which shows what the About box shows. At a QKVM server's wall, General says first that its machines, accounts and own settings are managed on its page, with the button that opens it. Not there yet: a saved default mouse mode, the diagnostics line, where the logs are; and sections for the servers and the saved actions, which come into the same list. |
| Tray | State of the wall, open a wall, open the page, quit. |
| About | Version, licences, sources, the server's version. |

Every control can be reached and worked with the keyboard, and has a name a screen reader
can say.

## 6. What it talks to

The KVMs themselves (6.3), and a QKVM server where its owner has one (6.1, and 6.2 for a
machine of its wall live). The numbers are in the order they were written, not in the
order of weight.

### 6.1 A QKVM server, as it is today

The source is the truth ([the web app's `qkvm/server.py`](https://github.com/qkvm/qkvm/blob/main/qkvm/server.py)); this is the map.
Everything is JSON over HTTP or HTTPS. A refusal is `{"code": ..., "message": ...}` with a
fitting status, and `message` is written to be shown to a person as it is. The program
takes only a refusal of that shape for the server's word. A 401 in it, to anything but
a sign-in, is "you are signed out"; a refusal in any other shape was said by something
that stands in the server's place, and signs nobody out.

| For | Call | Note |
|---|---|---|
| Whether it is a QKVM, and which versions of the API it speaks | `GET /healthz`, open to anybody, sent no session | Asked first, before anything else, each time the program comes to a server, and again before anything that says who asks goes there: before every password that is posted, and before a display link is handed in again while the wall is up. And once contact was lost, before anything that carries the session goes there again: until a QKVM of the program's version has answered a question that was asked after that, such a call is not sent. Contact is lost by what says something of who answers: a call got no answer, one that broke off, or one that is not in a QKVM's shape (a refusal in another shape, a 401 among them; a wall, a list of the machines' states, a session or a picture that is none). It is not lost by a picture that is only larger than is taken or too slow: that picture fails, and nothing else. What has left this PC when contact is lost, and has its answer begun or is a POST, is waited for, and not given up; a POST that has left is never asked again. What has not left yet is taken back, and so is what has left with no answer begun where Qt would send it again by itself on a new line, to whatever listens there by then (a GET, a PUT): what was to change something is given up, and what only asks is asked again once the QKVM has said what it is. Over HTTPS nothing of it goes anywhere before the certificate was looked at. What answers there may be another program than it was. `api` lists the versions the server speaks, each under `/api/<version>/`, and the program speaks `v1`. What does not answer as a QKVM is said as that where the address was typed; of a QKVM that does not list `v1` nothing more is asked, and the program says which versions it does speak and that one of the two has to be updated, also where such a one answers after contact with a wall's server was lost: that wall is left. `locked` (restarted, and nobody has signed in since) is told where somebody signs in |
| What server is this | `GET /api/v1/session`, open to anybody | Whether it is set up, whether it asks for a name beside the password, who is signed in |
| Its version | `GET /api/v1/about`, once signed in | |
| Signing in | `POST /api/v1/login` with `name`, `password`, `code`, `remember` | Answers with a session cookie. Kept signed in for 30 days when asked, and the server remembers that over a restart |
| A screen nobody sits at | `POST /api/v1/display` with `key` | A display link made by an admin: looks, never acts. The program keeps the key and asks again when its session is gone, no sooner after the last time than the wall's own pace. Where the key was handed in and neither taken nor called no good, the next time waits twice as long, and so on up to five minutes, and never less than a `Retry-After` asks for that came with a refusal in the server's own shape, of which five minutes at the most are taken; one on the page of something that stands before the server counts for nothing. A key that is taken, and whose session the server does not know at the first look at the machines, counts as not taken: for that wait, and for whether the wall says that contact is back. It forgets the key only on the server's own word that the link is not good (`bad_key`), and on signing out |
| A script | `Authorization: Bearer qkvm_...` on every call | An API token made by an admin: viewer or operator, for chosen machines. Not what a person signs in with |
| The tiles | `GET /api/v1/wall` | The machines this account may see, in the wall's order, and `interval`, the pace for pictures |
| How each machine is | `GET /api/v1/status` | By machine: `state` (`connecting`, `offline`, `no_signal`, `busy`, `error`, and the good ones: read `Device.status` and the drivers), the error, `last_seen_ago`, and what the driver knows |
| A picture | `GET /api/v1/devices/{id}/snapshot?w=&age=` | A JPEG or a PNG, whichever the machine gives (a KVM without a signal may send a PNG where it otherwise sends JPEGs), at least `w` wide where the machine has that many; `age` says how old it may be; the header `X-KVM-Signal` says whether the machine sends a picture at all. The program takes as a picture only what says that it is one of the two (`image/jpeg`, `image/png`) and begins as one: anything else that comes with a yes is no QKVM's answer |
| Doing something | `POST /api/v1/devices/{id}/actions` | `action` is one of a fixed list (keys, text, port, wake, power and so on). `409 busy` while another action is out on that machine. Only the server's own refusal is taken as one: an error of something that stands before it (a gateway's 502 or 504), an answer that breaks off, and one that is larger than is read, whatever its number, leave open whether it was done. That is said as "may have been done", is never sent again by the program, and is not among what a run over several machines offers once more, as what was refused is |
| Saved actions, wake targets | `GET /api/v1/actions`, `GET /api/v1/devices/{id}/wol` | |
| A machine's virtual media (0.3.0) | `GET /api/v1/devices/{id}/media`, `POST .../media`, `.../media/unmount`, `.../media/delete`, `POST .../media/upload?name=&machine=&listed=` | An admin's, signed in in person (the Media box). Each change is asked first, and names the machine as its question named it: `409 changed` where it is called otherwise by then. Since 0.3.2 each also names the list it was picked from: the list's `listed`, a mark of the machine it was made of, taken only as 32 hex digits (a list with one in any other shape is not understood, and nothing can be picked from it), goes back with the change, in its body or, for an upload, in its address; where the tile's machine is no longer the one listed (the tile edited to another KVM meanwhile), the server does nothing with it (`409 moved`), and the box closes and says so in the server's words. The mark sent is that of the list shown when the change was asked, as the page takes it; and the box is held to the mark of the first list it was opened with, as this PC's own wall's box is to its first listing: a later look (the one after a change, or one asked again) that lists the machine by another mark, or by none, is not shown, nothing is sent with its mark, and the box closes and says why in the server's words, "Its media box was closed: list its images again from its menu." (stricter than the page, which shows each list it is answered and takes its mark: a tile edited to a twin between a change and the look after it had the next change go to the twin under the box's title; where the server first learns the machine's number between two looks, the box closes for the same machine too, and is opened again). An upload asks for the list again first, as the page does since 0.3.2: the server refuses one with another mark before it reads a byte, and closes the line under what is still being sent, so its answer is often lost; where the mark is another by then, or none (stricter than the page, which lets the upload go where that list names none), the box says so in the same words and closes, and nothing of the image is sent; where that look gets no answer, the upload goes, and the server still holds to the mark; closing the box while that look is out stops the upload at once, and nothing goes when its answer comes. That look also reads in its fresh list what the box reads in the list shown when an upload is asked, which the server refuses at once too (the name there, a part of it, no room, another upload under way), and says so with nothing sent (beyond the page, which reads it of the list shown alone). Signed in to the server again while the box is open (as another account, or to another server through the same client), the box closes when it next hears from the server or is answered yes, and says why in the corner; a question asked before is not sent under the new sign-in (beyond the page, whose sign-in is the browser's). A server before 0.3.2 lists none, is sent none and is asked nothing first, as before |
| The wall as a file | `POST /api/v1/export` with `password`, `passphrase`, `parts` | For admins, with their own password once more: `401 bad_password` where it is not right, which signs nobody out. Answers with the file itself, which says `"format": "qkvm-export"` of itself, and in the header `X-QKVM-Not-Opened` with how many saved passwords or texts are not in it because they do not open any more. With a passphrase of ten characters or more the file is encrypted and holds the devices' passwords; with none it is plain and holds no password. `parts` is of `devices`, `actions`, `alerts`, `wall` |
| A file taken in | `POST /api/v1/import/read`, then `POST /api/v1/import`, both with `file` (its text) and `passphrase`, the second with `take` | The first changes nothing and says what the file holds: every device and every saved action, which of them are new, what was left out and why. The second takes the parts `take` names. `422` with `passphrase_needed`, `wrong_passphrase`, `not_a_file`, `newer`, `damaged` or `too_large`; `429 busy` while another file is worked on |
| Signing out | `POST /api/v1/logout` | |

Two rules a program has to know. A call that changes something, made with a cookie, must
carry the header `X-QKVM: 1` (the server's guard against other sites' pages). And three
roles exist, `viewer`, `operator` and `admin`: the server enforces them, the program only
hides what would be refused.

The web app implements live v2 (section 6.2).

### 6.2 A QKVM server's live connection (draft 2, joint acceptance pending)

Both sides implement live v2. The desktop now allows 65 seconds for the server's
60-second admission deadline, follows advertised pointer modes, and releases its own
control before confirmed text typing.
This original draft is not a certification of full compatibility.

A server that has live pictures says so in `/api/v1/wall`: `"live": 2`, the form of the live
connection it speaks. The program asks for a machine live only where that is the form it
speaks itself; a server that says nothing of the kind is asked for none.

Two steps. First `POST /api/v1/devices/{id}/live`, a call like every other (cookie or token,
`X-QKVM: 1`; a viewer may watch, an operator may also control). It answers
`{"v": 2, "ticket": "..."}`: a pass for one live line to that machine, good for half a
minute and for one use, 16 to 200 letters, digits, `-` or `_`. A server that has no live
pictures, or none of that machine, refuses in its own words (`no_live`), and the program
stays with snapshots. Then `GET /api/v1/devices/{id}/live`, upgraded to a WebSocket, which
carries no cookie: the pass is in its first message, and a line whose first message is
not a pass the server gave is closed.

Why a pass, where draft 1 let the line in by its cookie: the program looks at a server's
certificate itself, for every call, before a byte of the call goes out (section 8). The
library that speaks WebSocket gives no moment for that look in every case, so the session
is never put on that line; what could be lost there is a pass for one line, already used.
And a pass in the first message is in no address, and so in no log of addresses.

Text messages are JSON with a field `t`. From the program:

| `t` | Fields | Meaning |
|---|---|---|
| `hello` | `v: 2`, `ticket`, `video: ["h264", "jpeg"]`, `max_w`, `max_fps` | First message. The pass, what the program can show, and how much of it this tile needs |
| `shape` | `max_w`, `max_fps` | The tile changed size |
| `key_frame` | | A frame was lost or dropped: send a key frame |
| `control` | `on` | Take or give back keyboard and mouse |
| `k` | `u`, `d` | A key by its USB HID usage (page 7), down or up |
| `m` | `x`, `y` | Pointer, absolute, each 0 to 32767 over the picture |
| `mr` | `dx`, `dy` | Pointer, relative |
| `mb` | `b`, `d` | Button 1 to 5, down or up |
| `mw` | `dx`, `dy` | Wheel |
| `up` | | Release everything that is down |

From the server:

| `t` | Fields | Meaning |
|---|---|---|
| `video` | `codec`, `w`, `h` | What follows, sent at the start and whenever it changes |
| `state` | `state`, and when there is one `error` (`code`, `message`) | As in `/api/v1/status`, the moment it changes |
| `control` | `on`, and when refused `code`, `message` | Whether this connection holds keyboard and mouse |
| `bye` | `code`, `message` | Why the connection is about to close |

Binary messages from the server are pictures, one per message: a 16-byte header, then the
data. Byte 0: kind (1 = one H.264 access unit in Annex B form, 2 = one JPEG). Byte 1:
flags (bit 0 = key frame). Bytes 2 and 3: zero. Bytes 4 to 7: a counter, one higher for
each picture sent, little-endian. Bytes 8 to 15: when the server got the picture, in
microseconds on a clock that only goes forward, little-endian.

Rules of the draft:

* Every H.264 key frame carries its parameter sets, so a decoder can start at any key frame.
  A new `video` message is followed by a key frame.
* The program asks whether the server is there (a WebSocket ping) every few seconds, and
  takes a server of which nothing at all has come for three such questions to be gone: a
  still screen sends no pictures, so silence by itself says nothing.
* Where H.264 begins (the line's first picture, or the first after a JPEG) and after a
  gap in its counter, the program shows nothing until a key frame, and asks for one once.
* Keys and the mouse are sent only after the server has said `control` with `on: true`.
* The server answers each `control` of the program's with one `control` of its own, in
  the order they came: `on: true` or a refusal to an asking, and `on: false` without a
  `message` to a giving back. No message carries a number, so the program tells which
  answer is whose by that order, and by whether a refusal has a `message`. It asks for
  nothing while an answer is still owed. Admission allows 65 seconds, covering the server's
  60-second deadline; release acknowledgements have five seconds
  (DESIGN.md section 3.8, "Asking"). A server that says fewer is borne: "Control" then
  does nothing for those seconds. A KVM of this PC's own answers each one.
* The server never keeps more than a few pictures waiting for one client. A client that
  falls behind is brought forward to the next key frame; it sees that as a jump in the
  counter.
* The program never keeps more than one decoded picture waiting to be shown.
* The server writes in its activity log that control was taken and given back, and by
  whom. Never which keys.
* When the connection ends for any reason, and when it gives keyboard and mouse back or
  has them taken away, the server releases everything that client had down. The program
  says `up` itself before it gives them back or closes the line. (The draft has no word
  by which a server says whether it could. So a line to a server never tells whoever
  held keyboard and mouse that something may still be down, as a line to a KVM does:
  DESIGN.md section 4, `Live::leftDown`.)
* A `code` with a `:` in it is the program's own and is never sent by a server.

What stands behind it on the server, for information: a kvmd device's own H.264 stream is
passed on as it comes; a JetKVM's WebRTC video is passed on without decoding it; a
server's console is sent as JPEG when its screen changes.

### 6.3 The KVMs themselves: kvmd (PiKVM, GL.iNet Comet)

What the program asks of a KVM of this family and what the KVM answers. Two things are
the truth here, and this is the map: kvmd's own description of its interface (PiKVM
publishes it), and the server's driver, [the web app's `qkvm/kvm.py`](https://github.com/qkvm/qkvm/blob/main/qkvm/kvm.py),
which holds what real KVMs were seen to do, where the two makes differ, and every limit
that was learned the hard way. The program's driver is written anew in C++; it is to do
what that one does, and where it does otherwise the reason is written beside it. Each
row is filled in as it is built (M3a, M3b); until then it says what the server's driver
does. Built so far, in `core/kvmd_device` and beside it, and tried against made-up KVMs:
every row, the last two as far as they say (the live picture now includes Janus/WebRTC tested with local peers; keys and
the mouse from the window that shows a machine large to the KVM's line, with keys that
a test presses by hand: no test runs what hears this PC's keys from Windows). This
PC's own wall uses it: a KVM is added in the program's box and is on the wall from then
on (M3a). On 2026-10-12 a real KVM of one maker, which its owner named, was asked what
it is, signed in to under the certificate it showed, looked at, and put on this PC's
wall in the rig: all of it as against the made-up ones. It was had live there for ten
seconds, by its pictures, with nothing sent to its computer. Its line for keyboard and
mouse was let in and held, and one key and one small move of the pointer were said to
the computer behind it, by the owner's leave; nothing else was sent there in that
look. Apart from it, the owner has started the program, taken keyboard and mouse of a
real KVM in its window, and reported that keys arrive; nothing else is known from that
run. What the KVM of that look does otherwise than was assumed is in the table.

| For | Call | Note |
|---|---|---|
| What answers at an address | Questions without a password | A JetKVM and a server's controller say what they are to anybody; so does a KVM of this family. No password goes to something that has not said it is one |
| What it is known by | The certificate it shows, read without trusting it | These KVMs make their own, wrongly dated and named `localhost`: only which certificate it is can be checked, and is (section 8) |
| Signing in | `POST /api/auth/login` with `user`, `passwd` | Answers with a pass for the session: one maker hands it over in the answer, the other as a cookie. A password that was refused is not sent again for a minute |
| A plain address that sends everybody on to HTTPS | Every answer read for a redirect (301, 302, 303, 307 or 308) to `https://` at the same host (`core/kvm_probe`: `kvm::httpsInstead`) | As web 0.3.2 (`https_only`: kvm.py's https_instead and on_https, server.py's detect_kind and sent_on_to_https), a web server before kvmd once HTTPS is its only way in: said with that address, "This KVM answers on HTTPS: its plain address sends everybody to https://... To use it there, type https://...", never followed, and offered by the box that adds a KVM and the box that changes one ("Use https://..."), which take it only when the person says so (the change box puts it in its address line, the password to be typed there). Read of the three questions an add asks, where nothing answers as a family (kvmd's question's first: a web server may send the API on alone); of the question a sign-in asks first with no password, so that none goes the plain way to be sent on; of the sign-in itself, held back then as one that came to neither yes nor no, so that the password does not go out at every look; and of any question after it, whose pass the driver then lets go of, and holds what needs a pass back as that sign-in's word holds it: the pass went the plain way to it at every look (the merged review of 0.2.0, B-M1). From then on, the next sign-in asks first with no password however lately the KVM was heard, until kvmd itself signs the program in there; so also where only the question a held sign-in asks with nothing, whether the KVM is there at all, heard the redirect (round 2 of the merged review, L1). Not let go of at their own redirect, which is read after 0.2.0: what opened with the pass before the first question heard the redirect (its stream of pictures, its video line, its line for keyboard and mouse or for its events, an upload, a Fingerbot's press) carried the pass the plain way once for each opening, until the next question lets it go, within the PC feed's four seconds or with the live view's next picture (round 2, L2). Its card says it as `error`: it is there, and no machine gone. A redirect to another host, to plain HTTP, or to nowhere that can be read is no such word. Otherwise than the web, for the desktop's own rules: a change of the name or the group alone asks nothing (section 4, M3a), which is the web's save without checking of such a tile; and the sign-in asks with no password first wherever the KVM was not heard a moment before, where the web sends the password once at a tile's first sign-in and says it then. Tried against made-up KVMs only |
| Who it is | `GET /api/auth/check`, and where the KVM has them its model, version and name | |
| A picture | `GET /api/streamer/snapshot` | A JPEG, as captured or made smaller by the KVM, which costs the KVM time: small pictures are asked small, anything wider is asked as captured. A header says whether the computer sends a picture at all. A KVM whose video rests while nobody watches says so, and is woken |
| How it is | Its state calls: the video, the hardware, power where it is wired, the computers behind it | Whether the computer is on is GLKVM's own word (`atx.power`: on, sleep, off), whose power light stays off on a real Comet whatever the computer does; stock kvmd's lights where there is no word: a lit disk light (`leds.hdd`) is a machine that is on, as a JetKVM's two lights are read, else its power light (the owner's decision of 2026-11-19, stricter than the web, which reads the power light alone: review cx4-2 L5); both are kept as its line for events says them. A board that said it is there is not taken away by an answer that says it is not (a Comet's board says so to whoever asks while it answers somebody else) or by none, for a minute, and what it said of the computer is kept ten seconds (as the web app does). Where the line that keeps its video awake is held (kvmd's `/api/ws`, its line for events), the board's state is heard there (its `atx` event, at every change), and asked whether the line is there every five seconds in kvmd's own words (`ping`); a look reads the board there while the line was heard from within twelve seconds and says the board is there, and asks `/api/atx` otherwise: one asker less on a board that answers one at a time. No line is opened for the board alone. One maker's real KVM says nothing of its video while that rests: no size of the picture, and not whether the computer sends one. There what the last picture said stands in: its size as the KVM captured it, and its header for the signal; each only while the picture that said it is new, and never against what the state does say |
| Why its power is unknown | The same calls | As web 0.3.2 (`power_led_unwired`: kvm.py's POWER_LED_UNWIRED, app.js's machineText and powerNote): where the board says the computer is off (its light, or GLKVM's word) while the computer sends a picture, by what the look and the last picture say, its card says "Power unknown", and its Details say why: "The KVM's board says the power light is off while the screen shows a picture: its power-LED wires are probably not connected." (`Status::powerNote`). Never of a KVM that has shown a driver of it several inputs, whose board may be another computer's than the one on screen: also after an answer of its switch that was no yes (it is then asked again only after ten minutes), and for a driver made anew for the same KVM, at its address and known by what it was known by (another password or user name: the wall hands that on, `KvmdKept`), as the web carries it to the client that takes over; its card says "Power unknown" alone there while the look says its inputs, and nothing of its power after such an answer of its switch (no inputs said, as the web's page). Wherever the board says off beside a picture, the note made or not, a short press of its power button is refused (Doing things, the owner's decision of 2026-11-19). Read the same of a qKVM server 0.3.2's status (`atx.note`). Otherwise than the web: the page puts the why in the title of a tile's footer too, which a pointer shows; the desktop's cards have no such titles, and the why is in its Details alone. Tried against made-up KVMs only |
| Doing things | Keys, a text to type, the power buttons, the computer shown, a restart of the KVM | Each a call of its own; one at a time to a KVM. What got no answer may have been done, and is not sent again: only what certainly never left this PC is told as "not there". A power button: the board asked first, whatever its line said, and the press refused where either says it is busy (and whether the computer is on is what it says then); answered once the board has let go of it (half a second, six and a half for the long press), and a press that must change the computer's power once the board shows the change (twenty seconds at the most; else done, with what was seen); only a 4xx is a no; after an answer that says neither (none in eight seconds, one that broke off, a 5xx, the driver let go of meanwhile) the board is left alone for eight seconds and the button's hold, also by a driver made anew (section 8). A short press of the power button, its board's or its Fingerbot's, is refused with nothing pressed where the board is no witness of the power: where it says off beside a picture by the last look (its card's "Power unknown", with its why, `power_led_unwired`, or alone on a KVM that has shown several inputs, whose board may be another computer's than the one on screen; or nothing of its power, after an answer of its switch that was no yes), or, for the board's press, where the board asked for the press says off then beside the last look's picture (the Fingerbot's press asks no board, and goes by the last look alone). Refused in words that say why, and which of the two (`power_unknown`): it was to switch the machine on, and may shut a running one down (the owner's decision of 2026-11-19, stricter than the web; the merged review of 0.2.0, A-M1: a KVM of several inputs was pressed). What it costs: the computer of such a KVM's board, off while another input is shown, is not switched on by a short press until its own input is shown. So is the restart after a mount that would press it. A hold of the power button and the reset button go as before: the one cuts the power and the other resets, whatever the machine is doing. kvmd's own `/api/atx/power` (on, off, off_hard, reset_hard) is never asked: only its buttons are clicked (`/api/atx/click`) |
| The live picture | Its stream of JPEGs (`GET /streamer/stream`) where it serves one; else its pictures as captured, one after the other; H.264 over Janus/WebRTC with reconnect and JPEG fallback, implemented against local peers in 2026-10-20; device acceptance pending | One stream to a KVM at a time, shared by every window that shows it live. A stream that ended is asked for again after a wait that grows, with the pictures asked for one after the other meanwhile; that a KVM serves none is remembered for a minute, and then asked anew. 404 is read as "serves none", and so is a yes (200) with something of another kind than a stream of pictures; kvmd's own 503 is read as a video that rests; a 502, 503 or 504 in anything but kvmd's own shape is read as "serves none" only where a picture of the KVM has come within the last moments, so that its video is awake, and otherwise as a video that rests (DESIGN.md section 4, and section 11 for what is open in it). Seen on a real KVM of one maker (2026-10-12): no stream of JPEGs there. At its stream's place, what stands before kvmd answers 502 with a page of its own while its video rests, which is read as a video that rests; and while its video is awake, 200 with something whose kind is not a stream's, which is read as "serves none": the stream is asked for once, and then the pictures one after the other. (That last reading is newer than the look at that KVM, and is tried against made-up KVMs: until it, such a yes was taken for a stream that broke, and the stream was asked for again and again for as long as the KVM was shown.) Nothing at `/api/media`, and a page of its own that shows the picture over WebRTC only. A picture as captured took it 25 to 45 ms, a small one 160 to 240 ms: the KVM shrinks it. Had live in the rig for ten seconds, by its pictures, without an error, 2560 by 1440 (pictures only: nothing was sent to its computer). Asked one after the other as fast as they come, it gave sixty pictures as captured in 1.2 seconds: about fifty a second over the local network, about 160 kB each, the slowest in 38 ms. Fifty-eight of the sixty were byte for byte the one before, its screen being still, and the live view hands on only pictures that changed. Not seen: a live picture of a screen that moves. A stock PiKVM, where the stream should be, was not tried |
| Keys and mouse | The KVM's WebSocket, `/api/ws`: one short message for each key, button and move, in bytes as kvmd's own page says them, or as a text of JSON for a KVM known to take only those. The pointer goes the way the KVM's mouse is set (`/api/hid`, `mouse.absolute`, read at every look and followed when it changes: places to an absolute one, steps to a relative one); the program never switches the KVM's `mouse_output`, a setting its other users share | The same real KVM let the line in and kept it (its own page falls back to this line where its WebRTC channel fails), and its own page says keys and the mouse in bytes. One key and one small move of the pointer were said to it so, by the owner's leave (2026-10-12). That they arrived at its computer was not seen: the picture showed no pointer. Apart from that, the owner has taken keyboard and mouse of a real KVM in the program's own window and reported that keys arrive; nothing else is known from that run. A KVM that signs the program in and refuses this line all the same is told as that (`not_allowed`), and is not signed in to anew with every asking (tried against a made-up KVM) |
| Virtual media | `GET /api/msd` (the state of its drive and storage), `POST /api/msd/set_params` (`image`, `cdrom`, `rw`) and `set_connected`, `POST /api/msd/remove`, and `POST /api/msd/write` (`image`, `remove_incomplete=1`, the image as the body with its length said first) | As the web has them (0.3.0: kvm.py's msd parts and server.py's media routes, in their words; GAPS A2). The state of its drive is kept as the web keeps it, of the line for events the driver holds anyway (kvmd's `msd` event: the whole state as the line opens, then what changed, read up to 256 kB) and of a question a look asks beside itself every 30 seconds at the most; what is in the drive is on the card. A mount as a CD-ROM drive (300 sectors at least) or a USB flash drive (writable only there, where it is chosen), and a restart after it by the power board where the KVM has one (its power button where the board, asked then, says the machine is off, else its reset button, and where it does not say, its reset button and the box says that whether it was on was not known; "off" is GLKVM's word, and on stock kvmd its lights': a lit disk light is a machine that is on, and is reset; where the board says off beside a picture (its power light probably not wired, or, on a KVM that has shown several inputs, its board perhaps another computer's), its power button is not pressed, and the box says that the image is mounted and the machine was not restarted, and why (the owner's decision of 2026-11-19, review cx4-2 L5; the merged review of 0.2.0, A-M1); a running machine that shows no picture, whose power light's lead is not connected and whose disk is idle, reads off all the same and has its power button pressed, which shuts it down); an unmount; a deletion, done once the storage lists it no more; each refused, nothing sent, where kvmd says the drive is off, offline, busy, or open to the machine as GL.iNet's File Sharing. An image uploaded from this PC: streamed from the file as it is read (never whole in the program, read on a thread of its own), 16 GB at the most, hashed on its way, one at a time to a KVM, held to the KVM's listing it whole afterwards; a file that changes while it is read is refused, and what an upload cut off, stopped or refused leaves on the KVM is removed (nothing, as the web). Never `write_remote` (an image by address: nothing is fetched from an address), `read`, `set_enabled` or GL.iNet's partition routes. Stricter than the web (outside review 4.1, 4.2, 4.6, 4.7; review cx4-1): kvmd connects whatever image is chosen when its connect comes, and has no connect that names one, so what a mount chose is read back before its drive is connected, and nothing is connected where that is not the image as asked, or where the drive is connected by then (said as connected by another client). That narrows the moment between that look and the connect, and does not close it. kvmd takes a connect only while its drive is not connected (stock kvmd and GL.iNet's refuse one otherwise, MsdConnectedError, as the web's stand-in has it), so what a connect it answered yes to finds in the drive was put there by that connect: where that is not as asked (this image otherwise, or another client's choice made in that moment), it is taken out again at once, read back as an unmount is (still in, it is said so), and said, the other client's choice left chosen; another client that chose and connected in that moment has the connect refused, and nothing is taken out. Not held: another client that takes out what the connect put in, chooses and connects its own, all between the connect and its taking out (its read-back's answer, and the taking out's way to the KVM, a sign-in among them where the session went meanwhile), has that taken out (review cx4-2 L3). That kvmd refuses a connect while its drive is connected is the web reference's reading of stock kvmd and GL.iNet's firmware (its stand-in has it so), not yet seen on a device: one that took such a connect would have this mount's connect put another client's deliberate mount in, and its taking out take it out. What kvmd says in no shape the program reads is not taken for the reassuring: a drive that is no object (none at all is an offline one), or that does not plainly say whether it is connected, is no empty drive, and every change of it is refused; an image in the drive whose name it does not say as one the program hands back goes by an id of its own, never the name it reads as ("an image" where it says none), so that it is no "It was mounted so already" and has no SHA-256 of that one, and no image is deleted while it is in (it may be that one), while it is taken out by the id the box listed (review cx4-1 M2); an image that does not plainly say it is complete is not whole (no image to mount, no SHA-256), and is not removed after an upload either, as it may be whole: what an upload left is removed only where kvmd plainly says it is not complete (review cx4-1 L1); a drive that does not plainly say it is read-only is taken to be writable; a list of its images that is none, or not read whole (an image of a name the program hands back in no image's shape, or more than 2,000), never says an image is not there or gone, or that nothing of an upload is left, and its box says that not every image on it may be shown (review cx4-1 L5). Tried against made-up KVMs only; no real one was asked |

Rules:

* Every answer of a KVM is bounded and checked as a server's is (section 8). A KVM is
  somebody else's software on the network.
* The program is one viewer of a KVM among others, a QKVM server perhaps among them. It
  asks no more of a KVM than one browser would, and never signs in twice where once
  does.
* The rows above are this PC's own wall's. A KVM of this family on a qKVM server's wall is
  the server's: its tile says what the server's status says (a 0.3.2's `atx.note` read as
  the note above, so "Power unknown" with its why), and a press of its power button goes to
  the server, which presses it by its own rule (web 0.3.2 reads the power light alone and
  refuses no short press where the power is unknown). The refusals of this section hold on
  this PC's own wall only (a refusal of the program's own for a server's tiles is the
  owner's to decide, after 0.2.0).
* Of the virtual media row's refusals, "each" is a mount's, an upload's and a deletion's.
  An unmount is refused, nothing sent, only where kvmd says the drive is off or busy: where
  it says the drive is offline or open as File Sharing, the unmount the person asked for is
  sent, and kvmd answers for it (`KvmdDevice::unmountMedia`; the outside review of the wave
  2 desktop, 4.12).

### 6.4 Virtual machines on Proxmox VE (wave 2)

What the program asks of a Proxmox VE for one of its guests, as the web app's `proxmox.py` of 0.3.1 does
(`core/proxmox_device`, `proxmox_screen.cpp`, `proxmox_answers`, `proxmox_adding`). Tried against a Proxmox
by hand on this PC's loopback address, typed from what a real Proxmox VE 9.2 answered the web app; no real
Proxmox was asked by this program.

| For | Call | Note |
|---|---|---|
| Who asks | `Authorization: PVEAPIToken=<id>=<secret>` on every question and the console's upgrade | An API token alone, never a person's password; what is no token's shape is sent nowhere. The first question is the sign-in, counted for every guest of that Proxmox together |
| What a no says | The words of its status line (Proxmox's API server writes a refusal so, its body `{"data":null}`, with `errors` for a parameter it did not take), or a `message` in its body (its other format) | As the web's `_said`: the body's message first, else the status line's words (ASCII alone, up to Qt's 1024 bytes), else `HTTP <status>`; one line, the token cut out of it. A guest on another node, a digest refused (`changed`) and a right the token lacks are told by these words |
| What it is known by | The certificate it shows before the token goes, where Windows does not trust it; and an unsigned `GET /api2/json/version` on the very line the token is to go on, right before every sign-in with it | As a KVM's (section 8); a Proxmox's certificate is every guest's of it, so no two guests are one by it. And what answers that question is a Proxmox only where it refuses it 401 (as a real one refuses every question without a token) or says its version: a 404, a login page, a redirect, a 401 that asks for a sign-in of its own (`WWW-Authenticate`: a camera's Basic, a switch's Digest, a Windows sign-in; Proxmox's API server asks for none: a real Proxmox VE asked so on 2026-11-18 answered `401 No ticket` 3.07 s later, `Server: pve-api-daemon/3.0` and no `WWW-Authenticate`; and a token in the Authorization header answers no such challenge) or any other answer is somebody else at that address, and is sent nothing (`bad_response`, and asked again only after a hold, as a failure; the web sends the token with its first question, to whatever answers). Asked by the guest's own driver, before every sign-in of its own and before another guest's sign-in with the token is taken for its own: so on every road a card comes by, whatever the wall says of it (an add, an edit to a new address or at its own, a yes to another certificate, a file, a start of the program, a sign-in again after a 401, in a follow to another node or not). And signed in again, so asked first, after any failure of contact with what answers there, by every guest's driver of that Proxmox (by its address as written) whichever of them saw it, and for a certificate refused by every guest's driver known the same way as the one that saw it (by that pin, or by Windows' trust: the others' lines are held to what they accept, so one card still on its Proxmox's old certificate does not sign out at each of its askings the cards said yes to the new one): a question of any kind that got nothing back at all (nothing there, the line fallen before any answer, another certificate shown, or one Windows does not trust), that unsigned question and the sign-in among them, or the guest's console's line that ended so before Proxmox let it in, or fell after that without Proxmox's goodbye (not one Proxmox closed with its goodbye, as it ends the console of a guest that stops, nor one this program ended itself, nor a way in that failed, whose questions count as they fail); the sign-in stood for what answered then (the token is no session, and goes with every question). And a sign-in stands for what answered the unsigned question right before it: where contact was lost while the two were out, by any guest's driver of that Proxmox, it stands for nothing, and the question that waited for it has that question asked again, and a sign-in again, before the token goes with it. An answer of any kind, one that began and was too slow or too large among them, is no such failure, nor is a question that got no answer in time, the program's own bound for it among them (its line was let in by what the Proxmox is known by, and the token went on it: asking first would keep the token from nobody, and cost an unsigned question and a sign-in), and between those the token goes on what the Proxmox is known by, as a KVM's session does (section 8). The question is waited for as the sign-in is (20 s of quiet), for a real Proxmox answers it three seconds late, as it holds back every 401 of its. A Proxmox some of whose guests are on the wall is known by what their cards are known by, each way in turn, and by no certificate it shows on first use: one that shows another is sent nothing, not even that question (`cert_mismatch`), until one of those cards was said yes to it, and so is an add from a listing made before those cards came onto the wall (the web pins what it shows on first contact at each listing). An address typed without a port is a Proxmox at 8006, its own, in the Add box and in the box that changes a guest's card, as in the web's (that box shows a guest's Proxmox with its port written, 443 too, so that a guest of a Proxmox at 443 saved as it stands stays there; the web shows the url as it keeps it, and a tile at 443 saved so moves to 8006); a guest's url in a wall's file (`https://<host>[:<port>]/nodes/<node>/qemu/<vmid>`, the web's and this program's) is read as a wall keeps it, without a port at 443, as both writers mean it (each keeps 443 unwritten: a Proxmox behind a proxy), so that a wall with such a guest is written to a file and taken from it whole (the web's own reader takes such a url at 8006). Which Proxmox it is goes by its address as typed, its host and its port as written: another name of the same host, or another node of its cluster (each node shows a certificate of its own), is a Proxmox with no guests on the wall, and what it shows is taken on first use, as for a Proxmox added the first time (tying it to the wall's cards by the token would take reading every kept secret, and would hold another node to a certificate it never shows; the web ties nothing) |
| The guests a token may see | `GET /cluster/resources?type=vm` | Read within 4 MB, by kind: templates, nodes and containers are no guests |
| How it is | `GET /nodes/<node>/qemu/<vmid>/status/current` | Running or stopped in so many words; a word of a kind of its own is no state |
| Which machine has its number (rule 30) | `GET .../config`, its `smbios1` `uuid=` | Kept as `proxmox:` and 32 hex; asked before every power command and console, and at a look once a minute and at once where the guest looks made anew; and asked again before a question that does something (a command, a console's ticket, a change of its settings) is sent a second time after a 401, a sign-in's time later, or is sent after a sign-in its driver had to make on its way (contact with what answers there failed, or another question was answered 401, while the reading it was decided on was out: a reading a sign-in old decides nothing), or after one another question of the driver's made meanwhile (each reading is held to the sign-in it went out under, and a command or a console's ticket goes only under the sign-in of the reading it was decided on: so a reading a sign-in old decides nothing also where its driver is signed in again by the time it comes), within the same moment, and not sent where it is signed out, or in again, by then (the web sends it again unasked; Proxmox's commands and its console take nothing that holds them to the guest, so the gap is narrowed, not closed). Another number, or none where one is kept: `another_machine`, and nothing is sent; and every live of the guest ends in those words, and its console is let go of (the web's stays open) |
| Where it went | The list above, once a minute at the most | Where Proxmox says the guest is not on its node (a 500 saying its settings file is not there, `Configuration file '.../qemu-server/<vmid>.conf' does not exist`, or that there is no such node; no other "does not exist": a storage's, a volume's or a boot device's is said in its own words): followed there only once the machine there is judged this guest |
| Power | `POST .../status/start`, `shutdown`, `stop`, `reset`; `GET /nodes/<node>/tasks/<upid>/status` | By what is wanted; a start or a stop waited for with its task, all bounded from the press: the questions it is decided by (its state, which machine has its number, and a sign-in or a follow to another node on their way) within one question's whole (36.5 s) from the press (a sign-in begun on the way of such a question, or of a read of the wait below, goes on for the driver past that bound, its unsigned question and the token with it, as every sign-in does; the question is answered at its bound, nothing done to the guest), its command sent within that or not at all (a second time after a 401 too, to the node the guest is on by then, and after a sign-in its driver had to make on its way, each on a reading after that sign-in; and where Proxmox answers it that the guest is not on that node, followed there within the same bound, else nothing was done; the web follows a command with no bound), its own answer waited for until that bound, and once it went for a connection's time and half a second at the least (5 s; a question held to a moment is given that), so that what came of it is said within 5 s of the bound, 41.5 s from the press at the most, and where no answer came by then, that it may have been done (and the guest looked at again); and each read of the wait, a follow to another node among them, held to the wait's bound (85 s from the press: 60 s while its task runs, and 25 s from the task's end to see the guest so), none begun in its last half second or after it, which could not even be let in by it (one begun before that is given 5 s at the least), and the wait judged at its bound on what was read by then, within 5 s of it at the most (the web bounds the wait alone); an answer lost on its way back (596, 597, 502 to 504) may have been done |
| Its screen | `POST .../vncproxy` with `websocket=1`, then `GET .../vncwebsocket?port=&vncticket=` (subprotocol `binary`) | RFB with VNC authentication by the ticket's one-time password, never the token; a ticket for one connection, used at once (Proxmox takes it for a short while only), which stands in the address the console's line asks for and in Proxmox's answer, held while the console lasts and not written over (its one-time password is, and the token's copies of the console are: section 8); the console held five minutes after the last look, each one a task in the node's log |
| Its virtual media: the images | `GET /nodes/<node>/storage?content=iso&enabled=1`, then `GET /nodes/<node>/storage/<storage>/content?content=iso` for each | Its settings read and judged first (rule 30), on the node it is on now; 64 storages and 2000 images at the most, taken by kind (an ISO image of that storage, named as the web hands a name on: no comma, slash or blank); one Proxmox says is not active is said and not asked; one the token may not see is not even named. The storages asked one after another within one bound for them all: those not asked within 36.5 s (one question's whole time) of the storages' list are said, and not asked; one asked before that is held to it, but given 5 s at the least (a connection's time and half a second), so the storages are listed within 41.5 s. The guest's settings and the list of storages are asked before them, each in its own question's time (36.5 s at the most): a Media box, and a mount before its change, wait 1 min 55 s at the most for these, and beside them for the unsigned question of its API and a sign-in where the driver signs in first (made anew, after a 401, or after a failure of contact: each its own question, which waits longer for Proxmox's answer, 20 s of quiet), for the question itself asked once more after a 401, and for three more questions where the guest's settings are followed to another node of its cluster (the list of its Proxmox's guests, where none read within the last minute says where it went, and its settings there twice: read to judge the virtual machine there, and then as asked); and a mount whose change finds the driver signed out right before it waits for those, and for its settings read again after that sign-in, before the change goes, as after a 401. (The web, whose bounds these are, holds the listing to no bound of its own: each question its own whole. The desktop's bound for the storages is its own, and it says the true one.) |
| An image in its CD-ROM drive, and out of it | `PUT .../config` with `ide2=<storage>:iso/<name>,media=cdrom` (or `none,media=cdrom`) and the `digest` of the settings read for it | Which virtual machine has the number asked again right before; only an image listed just then, and on the node it was listed on (a guest followed to another node since: `changed`, nothing written); and picked from a list of that node's images: a list carries a mark of the guest and its node (`listed`, 32 hex digits), a mount names it, and one that names another node's, or none, is `changed` and writes nothing (the web takes any volume of that name its own listing finds); read-only; a drive added at ide2, or the next free place of IDE and SATA, where there is none (a running guest's from its next start). Refused for its digest (an older Proxmox's "checksum mismatch", Proxmox VE 9's "detected modified configuration"): read again, done where the drive holds what was asked (Qt asks a PUT once more by itself where the line falls before its answer), else `changed`; where it cannot be read again, `timeout` (it may have been done). Never Proxmox's download-url, no upload, nothing deleted |
| Starting it from the image | `PUT .../config` with `boot=order=<the drive>;<the rest as it was>` and the `digest` of the settings read for it; `POST .../status/stop` where it runs, then `POST .../status/start` | As the web's `media_boot`: "once" restarts it at once and puts its order back after (a start takes a new order, a reset does not: a restart from inside the guest starts from the image again while it is in the drive); "until unmounted" restarts it where that is asked, and its order is put back at the unmount where it is still the one written. The order it replaces is kept with the guest (its stored data, section 11.2: `boot_order_was`), written down before the order is changed (where that cannot be done, the order is not changed), and kept until it is put back or found changed on Proxmox since: so a start that failed halfway, an answer lost, and a restart of the program leave it to the next unmount, or the next start "once", to put back (the web keeps it for "until unmounted" alone, in memory). It goes with the guest's card: the question before the card is removed says so, with both orders, and that the guest keeps the order it has unless the image is taken out first or the order set on Proxmox (the outside review of the wave 2 desktop, 6.4); where the order changed since the question was shown (a start from the image, or an unmount, meanwhile), the question is shown again as it stands now, and nothing is removed until it is answered (review cx6-1, L1). A file of the wall does not carry that order: a wall moved to another PC by its file leaves it behind, and its guest keeps the order it has. Where the guest's order is the one written, that next start takes the kept one for the guest's own; and where its drive was moved since (another order to write), it puts that one back first and only then keeps and writes the new one, so that the record is never replaced while its order is not back. A restart asked and said yes to is done every time, as the web's `_restart`: what a running guest started from is not Proxmox's to say (its process's age and the order it began with are not what its firmware started from, and a guest moved to another node runs a young process there), so a guest the program started from the image is stopped and started again where that is asked. An order written the old way (`cdn`), or none, is changed by nobody: said. Each change on settings read and judged right before it (rule 30), each stop and start a power button's press with its waits. A restart without a start from it is a power button's: refused. And a restart from the media, as the web's `media_held` has it, is refused while a live of the guest holds its keyboard and mouse ("busy", in the web's words with whoever has them said as "A window of this PC"; nothing changed); a mount alone goes on (the web's admin may take them over, and its restart waits for the last holder's keys first: no accounts here, and a guest's restart stops it). And while the wall does something to the guest (a press of a power button, a change of its media) its lives are not given its keyboard and mouse, as the web's live lines are refused them while their machine is acted on ("busy", the wall's words for a second action), asked as the console would give them: so from a restart's start on, nobody's keys are under its stop. What fails after the image went in is said after "The image is mounted, and <guest>" and how far the start came: "was not started from it", "may have been stopped", "was stopped, and not started again", "may have been started from it", "started again by another hand", or "started from it" where the order is what failed; and, for "once", whether its order was put back, and why not (and the guest named in the log). Only from the image confirmed: another in its drive when the start begins, or right before the guest is started (put there by another hand since: looked at before the start, and again on the start's own reading of its settings for rule 30, right before its command, and on that reading again before the command is sent a second time after a 401, or after a sign-in its driver had to make on its way (nothing sent where that reading did not come), or on the reading on the node it is followed to where Proxmox answers it that the guest is not on the node it went to; a start takes no digest, so the gap is narrowed to that), is not started from (`changed`; the web starts from whatever is in it) |

## 7. Finding devices (M5)

A QKVM server can look for devices only on the network it sits on, which is often a
container's or a VM's. The PC the owner sits at is often somewhere else: on the office
network, on a management network, on a VPN. Desktop looks from there, and what it finds
it can add to this PC's own wall, where no server is needed to reach it.

What it uses, all without any extra software on the PC:

| Way | Finds | Reaches |
|---|---|---|
| The PC's own interfaces and routes | Which networks there are to look at | |
| A sweep: one small unauthenticated question to each address | KVMs and server controllers by how they answer (the same signs the server uses: see [the web app's `qkvm/discover.py`](https://github.com/qkvm/qkvm/blob/main/qkvm/discover.py)) | Every private network the PC is on, and others the owner names (another VLAN, reached through the router) |
| Name service on the local link (mDNS / DNS-SD, through Windows' own interface) | Devices that announce themselves | The PC's own links only: multicast does not cross a router |
| The Redfish announcement (SSDP) and the IPMI presence ping (UDP 623) | Server controllers, where their owner left these on | Own links by multicast, named networks one address at a time |
| The neighbour table (which hardware addresses the PC has seen) | Candidates by maker, to ask first | Own links |
| The Tailscale client on the PC | Its peers, by name | The tailnet |

What it does with what it finds:

* Each found device is shown with what it is, where, and whether it is on the wall
  already (a device is known by more than its address).
* On this PC's wall, a found KVM is added in the program's own box (M3a), with its
  address filled in. A device of a family the program does not speak to yet is shown
  as what it is, with the way that works today.
* On a QKVM server's wall, a found device is of use only if **the server** can reach it.
  Adding goes through the server, which tries; when it cannot reach the address, the
  program says exactly that, and says that the device does answer from this PC. The
  password is typed on the page: the program opens the page's Add dialog with the
  address filled in, which is a small addition to the page to ask for.
* For a tile that is offline, "does it answer from here?" uses the same questions, and
  on a server's wall tells apart a machine that is down from a server that has lost its
  way to it.

Its manners, which are not negotiable:

* It asks only private addresses (10/8, 172.16/12, 192.168/16, link-local, and on
  100.64/10 only the Tailscale peers themselves). An address range that is not private is
  refused, typed or not. This must not become a tool for scanning other people.
* It looks only when a person asks it to. A network reached through a VPN is listed but
  not ticked: it is often somebody else's.
* A named network larger than a /24 is confirmed first; larger than a /20 is refused.
* At most 64 addresses are asked at a time, each once, each with a few seconds to answer.
* It never sends a password to anything it has found. A password goes to a device only
  when somebody adds that device and types the password for it.

## 8. Safety

The server's rules, kept here. Towards a QKVM server:

* **Where a secret is about to go is checked first.** Over HTTPS, a certificate that
  Windows itself trusts for the server's name is taken as a browser takes it. Any other,
  and that is every QKVM that made its own, is shown by its SHA-256 fingerprint the first
  time it is seen (the server prints the same fingerprint when it starts), and nothing is
  sent to the server before somebody has said yes to it. From then on the server is known
  by that certificate and by no other, whoever vouches for another. A different
  certificate later stops everything, with a screen that shows both and says what it can
  mean; it is never accepted quietly, and never by a key that was pressed for something
  else.
* **Never off HTTPS by itself.** An address typed with `https://` is never tried over
  plain HTTP. A server that was reached by HTTPS once is never spoken to in the clear.
  Plain HTTP works where the owner types it, with a visible "not encrypted" on the bar.
  (Towards a JetKVM these hold with the exceptions said at the end of the next list.)

Towards a KVM of this PC's own wall, the same, as the server keeps it towards its
devices:

* **A KVM is known by its certificate.** The one it shows when it is added is kept, and
  from then on it is that KVM and no other, whatever its dates and its name say (these
  KVMs make their own, wrongly dated). A different certificate later is not taken: the
  KVM's tile says so, shows both fingerprints and what it can mean, and the password
  goes nowhere until somebody has said yes to the new one. That first contact is taken
  on trust, as the server takes it: whoever adds a KVM on a network they do not trust
  compares the fingerprint, which the box shows, with the one the KVM's own page shows.
* **A password goes to the KVM it was typed for and to nothing else**: not to another
  address, not the plain way where the KVM was added over HTTPS, not to whatever answers
  where the KVM was. (Whoever moves such a KVM to a plain-HTTP address somewhere else,
  and types its password for that, is first shown what is given up there, and nothing
  goes to that address before the yes to it.)
* **No password goes to an address that was not asked what it is.** Before a KVM's
  password goes anywhere, what answers at its address is asked what it is, with nothing
  in the question that says who asks, and has to say that it is a KVM of the family the
  program speaks to. Over HTTPS it is asked on lines that are held to what the KVM is
  known by: what shows another certificate, or one that Windows does not trust where
  the KVM is known by none, hears nothing at all. Asked so before its first sign-in:
  every KVM that came from a file; every KVM of a wall that was made before this rule,
  once, at its next start; a KVM over plain HTTP, once in every run of the program,
  since nothing shows who answers there; and a KVM after somebody said yes to another
  certificate for it. A Proxmox VE guest's token is held to the same rule by the guest's
  own driver, more strictly: right before every sign-in with it (and before another
  guest's sign-in with it is taken for this one's), on the very line it is to go on,
  what answers there is asked the unsigned question of its API, and has to say that it
  is a Proxmox; on every road a card comes by, whatever its "asked" says (section 6.4).
  A token is no session, and goes with every question: after any failure of contact
  with what answers there (nothing back at all: nothing there, the line fallen before any
  answer, another certificate; of any question, the unsigned one and the sign-in among
  them, or of the guest's console's line where it fell without Proxmox's goodbye; not a
  question that got no answer in time on a line let in by what the Proxmox is known by),
  the guest signs in again, and so asks first, and so does every other guest of that
  Proxmox (for a certificate refused, every other known the same way: by that pin, or by
  Windows' trust). Between those, the token goes on what the Proxmox is known by, as a
  KVM's session does.
  This keeps a password from a machine that is no KVM at all. Not
  from somebody who answers as one: against that there is HTTPS and a certificate.
  The settings keep with a KVM over HTTPS that it was asked ("asked"), and beside its
  fingerprint the certificate itself that it is known by ("certificate_pem"), for the
  day the wall is written to a file. Neither is a secret.
* **What is spoken to in the clear is said.** A KVM over plain HTTP, and from 0.2.0 a VNC
  server over `vnc://`, is said to be spoken to unencrypted by one rule
  (`OwnKvm::isInTheClear`: an address that is neither `https://` nor `vncs://`, as the
  web's row in its settings has it): its card writes `http://` or `vnc://` before its
  address, its row in the settings says it is unencrypted, and the bar has its "Not
  encrypted" for as long as one such is on the wall, its description naming both. A
  Proxmox VE guest is spoken to over HTTPS alone (its Proxmox's API, and its console by
  `wss://` at that address, section 6.4): encrypted by the same rule, beside any other.
* **A JetKVM, as the server has it** (the owner's decision of 2026-10-27). A JetKVM serves
  plain HTTP, and HTTPS beside it once its owner turns that on; the program asks it both
  ways, as the web does. For a JetKVM alone, then, and in these things alone:
  * The way beside an address is asked with nothing in the question but the question
    (`GET /device/status`), and its certificate is not judged for that: so a JetKVM is asked
    the plain way beside an address typed with `https://`, and over HTTPS beside one typed
    with `http://`. Nothing else goes there, and a password only to the way that is chosen.
    An address typed with a scheme and a port of its own is asked only that way.
  * Added: where it answers over HTTPS, it is added over HTTPS, also where `http://` was
    typed, and known by the certificate it showed there, which the box shows. Typed
    `https://`, and answering only the plain way: refused ("plain_only"), with what to type.
    Typed without a scheme, and answering only the plain way: added that way, with "not
    encrypted" said; unless something else answers over HTTPS at that address (that is what
    was meant), or nothing answered there in time and yet something listens on its HTTPS
    port where no port was typed (refused, with what to type).
  * Signed in to anew, one over plain HTTP is asked over HTTPS at its own host first: with its
    password typed at the address it has, or with the one kept under another user name or at
    that very HTTPS address. Where it answers there, the password goes there, and it is saved
    there, known by the certificate shown on that first contact. A password kept for one over
    plain HTTP goes with it onto HTTPS at its own host where a JetKVM answers there, without
    being typed, and where it is kept for the KVM (one not read back this once is not taken
    along as none): the one way a kept password goes where it would not go for any KVM.
  * Moved, with a password typed: both ways, as when it is added, and the address chosen is
    held to every rule above. Chosen at its own HTTPS at the same host, where a JetKVM answers,
    it is known there by the certificate shown on that first contact, as above; where
    something else answers there, that is another KVM ("other_family"), and hears no
    password; any other address chosen is a new one. Where only the plain way answers, the
    change box says what to type to keep it over plain HTTP or to move it there so. One that
    runs without a password, and has none kept, is moved without one, asked at its new address
    first (both ways); chosen at its own HTTPS, it is known there by the certificate it shows
    on that first contact.
  * Followed onto HTTPS by itself: one over plain HTTP is asked whether it offers HTTPS at
    its own host when the wall first asks what it is, then at most every 10 minutes, and
    once a minute at most while it has not answered for 2 minutes. Where a JetKVM answers
    there, the wall moves it there itself, by its own rules (the same host, HTTPS only, an
    answer that says it is a JetKVM, the place free): its password goes along, saved anew for
    that address and the certificate shown there on that first contact, by which it is known
    from then on; its details say "Over HTTPS now" with that certificate's fingerprint, until
    whoever sits here has opened them, also after a start. Not where the address has a port
    of its own, and not onto another KVM of the wall (its certificate, or its number). Where
    the move cannot be saved, nothing is changed, and it is asked again later. A box open over
    it says at its save that it was changed meanwhile. Asked beside the wall's asking of what
    it is at its first asking, and then once a minute at most. Where its HTTPS shows the
    certificate another KVM of the wall is known by, that other one may be what answers its
    plain way too (an address handed out anew): it is held back, its card says so, and no
    password goes to its plain way until its HTTPS shows a certificate no other KVM of the wall
    is known by, or nothing listens there; in doubt (something listens there and shows nothing),
    it stays held, for as long as a KVM of the wall is known by that certificate: where that
    other one is taken off and another is still known by it (two kept both under one
    certificate, a PiKVM and its own VNC server, or two displays behind one TLS proxy), the hold
    goes over to that one, as a look anew would find it behind that HTTPS; where none is, no KVM
    of the wall is known by that certificate any more, and a look anew would find no twin
    either. (A limit that stays: the hold keeps which KVM it is held for, and not the
    certificate its HTTPS showed. Where that certificate was renewed first on the other card of
    the two, its yes taking the new one, and the card held for is then taken off, as the
    CHANGELOG's known limit for two VNC servers behind one certificate says to do, the hold
    goes, also in doubt, though that device is on the wall under its new certificate; a look
    that shows that one holds it again. Keeping the certificate shown with the hold would be a
    new field of the settings, section 11.) That is so
    whatever its HTTPS answers (an error, an answer past its time), whether the wall asks or its
    driver does, on every sign-in of its driver and of the Add and Change boxes (each looks at
    the HTTPS on that very sign-in), at every look a box takes before that (what one look saw is
    judged there and then, and not undone by a later look that sees nothing: one judgement for
    all of them), and from the start of the next run: the hold is kept with
    the KVM, and which other one it is held for, and goes with the KVM when it is taken off or
    moved onto HTTPS. Its card says why, and the acts there are for it, in plain words: where it
    is that device, one of the two can go; where it is another device that is well (only port
    443 was forwarded to the other one, an HTTPS that hangs), remove it and add it again (the
    Add box asks anew), or turn on HTTPS on the JetKVM and change its address to https://. No
    act sends its password in the clear beside that doubt. What shows the certificate another
    KVM of the wall is known by is that one also where Windows trusts that certificate there
    (the KVM would be known there by Windows' word): not followed there, and not added, moved
    or signed in to there by a box.
  * One device under two addresses (the owner's answer of 2026-10-28, as the web): over
    plain HTTP a JetKVM shows no certificate, and it gives a number for itself, which is kept
    with it as a digest. A second card of a JetKVM that gives the number one of the wall gives
    is not made when it is added, changed or taken from a file ("This says it is the same device
    as ..."): as on the page, the box asks, and the person may keep both; one kept so is not
    asked about again, and a file says so of it ("keep_both"). Two that give none are two. A
    certificate both are known by makes two one, whatever numbers they give, and that is not
    asked: it is proven, a number is said, and a certificate is where a kept password may go.
    Not where one of the two is a VNC server: one TLS proxy before several displays of a host
    (stunnel before `:5901` and `:5902`) shows one certificate for all of them, and a PiKVM's
    VNC server shows its web page's. There the certificate says it, as a number does, and the
    box asks, and the person may keep both, as the web's page asks of any twin by its
    certificate ("Keep both"; the outside review of the wave 2 desktop, 6.5). A VNC server's
    kept password never goes to another address all the same. "Keep both" answers only "are
    these two the same device?", never "may this card's password go to that card's
    certificate?": the review box's yes to a certificate on one card is never taken where
    another card is known by that certificate, whatever either was kept beside, and the review
    box names that card and offers no yes (two hosts behind TLS proxies whose addresses DHCP
    swapped). A card moved to another address is asked anew there whether it is another's twin:
    what it was kept beside before says nothing of what shows there (review cx6-1, M1). A card
    kept beside its twin is still the twin, by what the certificate says, of one that comes
    there (added, moved to another address, or taken from a file), the twin it was kept beside
    gone or not: that one is named, and nothing goes without "Keep both" said for it in that
    very box or file row (review cx6-2, L1). So where the one certificate of a TLS proxy
    before two displays kept both is renewed, the first display's yes takes it, and the
    other display is removed and added again, its password typed; the other's review box
    says that road where one of the two is a VNC server (review cx6-2, L2).
    A number that could not be read this once is no "none": the one kept stays. (Servers are
    otherwise: section M7.)
  * Never back from HTTPS to plain HTTP by itself. One over HTTPS that has not answered for 2
    minutes is asked the plain way, once a minute at most; where a JetKVM answers there, its
    card says that it answers only over plain HTTP now, as the web says it (its password would
    travel unencrypted there, or, with none, nothing would be checked), and to take it off the
    wall and add it again as `http://...` to use it so.
  * A hold for a twin is written at once, and kept from the start of the next run once it is
    on disk (the outside review of the wave 2 desktop, 6.3). Where the settings cannot be
    written then (held, a full disk, a copy owed that cannot be made), it stays noted and goes
    with the next writing, 2 to 30 seconds later, or at the end: a program killed before that
    starts knowing of no hold, as before 6.3 (review cx6-1, L6).
* **The text a saved action types is a secret too**, very often a password. It is kept
  in Windows Credential Manager and nowhere else, and the program hands it out at two
  places only: to a KVM's driver for one run of that action, read for that run once
  nothing else stands in its way; and to the writer of a sealed file of this PC's wall,
  which nothing asks for yet. The list of saved actions is handed out as a server hands
  its own out, without the text. A text is typed only by the action it was saved for:
  one that the store holds in another form, for another action, of another saving or
  not whole, is as none, and that action types nothing and says so.
* **The program is not a way around a KVM's own guard.** A refused password is not tried
  again and again, and nothing is tried that was not typed. One sign-in with a password is
  out to a KVM at a time, whichever of the program's connections to it sends it (the wall's,
  a box's): one that is out holds every other until what it comes to is heard. It goes on a
  line of its own, which outlives the connection that sent it (a box closed, the wall's
  driver made anew), and that line's end, the KVM's answer or the line giving up at the end
  of its time, ends the hold; the clock ends it only for a line that never ends, at twice
  that time. Whoever else would sign in with it meanwhile waits for that answer, as the
  server's second asker does: a no is its no too, and after a yes it signs in itself. A box
  that waits so says so; and a box, which asks once, never says that it is asked again. A
  password typed anew is tried as for the first time, and still after a sign-in with it
  that is out.
* **What holds a password in the program's own hands is written over before it is given back**: a driver's password and the one typed into the Add and Change boxes
  (`SecretText`; not yet a Proxmox token's secret typed into the Add box, below), the digest the pacing knows a sign-in by, and the body a sign-in sends (made once, as it goes, and held
  by nothing but what goes: the asker keeps it until Qt has let go of what it was handed, a turn after the reply went,
  and then writes it over). Out of reach: what Qt's network makes of a body on its own thread to send it (the copy it
  hands that thread, the request as it is written, the socket's and TLS's buffers), which Qt frees as it is; and what
  Windows' TLS keeps. A test looks at what the program's own thread gives back after real sign-ins of every road: it
  follows every block the program made of the secret until it is given back, by whichever thread, and looks through the
  heap for one held still, so a copy kept past any wait is found; Qt's network's own copies are passed over wherever
  they are given back (the copy it hands its thread is made on the program's: the outside review of the wave 2
  desktop, 6.6). A test of the look itself shows it finding each way a secret is left behind. Its limits (review cx6-1,
  L3): on the sign-in and console roads it follows what the program's own thread makes of the secret, and not what
  another of its threads makes there (a pool's, a line's own), which it cannot tell from what a stand-in on a thread of
  its own makes of what it is sent; those roads make none so today, and the drivers' roads are looked at on every
  thread. And where Qt's network is passed over, so is a block a Qt Network call makes and hands back to the program (a
  reply's header, a cookie's raw form): out of reach as Qt's own copies are.
  A Proxmox guest's API token goes in the Authorization header of every question (wave 2), made anew for each: Qt's
  request takes those very bytes and keeps them until it lets go of the request, out of reach as above, so nothing of
  it is written over once a question went. The console's upgrade carries it too: its way in (RfbScreen::WayIn) holds
  it as a secret's bytes, written over as the way goes on every road, and the console's line (DeviceLine::Where) a
  copy of its own, written over once the upgrade went, or as the line goes where it never went; the upgrade's asking
  is made once with it and written over once Qt has its copy to send (review c9b-1, L1, after the VNC servers'
  rebase). Since the line's socket is on a thread of its own (outside review 2.2), the asking is handed to that thread
  as a copy of its own, which that thread writes over once Qt has its copy, and which the line writes over itself
  where the line ends before that thread took it. `test_core_secrets_wiped` looks at what every thread but the tests'
  own consoles gives back on those three roads, the line's socket's thread among them. The one exception: the token's
  secret as it is typed into the Add box for a Proxmox goes from the box through its adding as plain text
  (`ProxmoxAdding::Typed::secret`, a QString; `ui/own_wall.cpp`), and so do the copies its refusals' words are cleaned
  with (`core/src/proxmox_answers.cpp`), each given back without being written over. Only a reader of the program's
  memory gains from it; carrying it as a `SecretText` from the box on is left for after 0.2.0 (the outside review of
  the wave 2 desktop, 3.18).
* **No device's TLS on the interface thread** (outside review 2.2, 2026-11-18): every line to a device (a kvmd KVM's,
  a JetKVM's, a live link, a server's console, a Proxmox guest's, a VNC server's RFB) speaks TLS, and has the
  certificate its device shows judged, on a thread of its own: at the handshake, and at each judgement Qt makes after
  it (each session ticket of TLS 1.3, each key update, each renegotiation). Windows judges a certificate on the thread
  that speaks TLS, and where the certificate names a signer to be fetched from an address that does not answer (an
  AIA fetch, on a PC without internet), it waits there for as long as it gives that (17 s on the build box). The
  certificate a VNC server shows to the boxes is asked for the same way, and every question of HTTP already was (on
  Qt's own thread, each of its managers ended off the interface thread). Meanwhile the interface thread goes on, every
  line ends in its own time, one cut, or thrown away neither closed nor cut, goes at once, and nothing more of what was
  said on it goes to Windows, its asking with who asks in it least of all (outside review 2, 2.2: a line thrown away so
  had all of it handed over the moment Windows let its thread go, and closed behind it), but for the one message that
  thread may be handing over at that very instant: one it had looked at for a reset just before (outside review 2,
  2.8), or one in whose flush by Qt Windows holds that thread over TLS (a key update or a renegotiation read behind
  it). That one goes once, to the device that passed the certificate's look, with the reset right behind it. One
  closed goes at once behind what was said before its close and its goodbye. The program's end waits 300 ms at the
  most for such threads, and leaves one Windows still holds to the end of the process. The certificate's rule is the
  driver's, as before, on that thread; nothing that says who asks goes before it was judged.
  `test_core_device_line_tls`, every kind of line, judgements after the handshake of an
  OpenSSL server (a renegotiation of TLS 1.2, a key update of TLS 1.3), and the program's end to its process's end.
  Review 2.6's rule
  holds on that thread as well (what has waited longer than the line's limit is never handed on): each message that
  thread's loop takes from Windows is looked at before Qt has it (Qt hands what a socket holds to Windows by itself, at
  the first turn of the loop that has room for it), and what has waited too long ends the line there; the socket is
  given one message at a time, and only while it holds nothing, so that a thread put aside between that look and Qt's
  own work lets no more than that one message leave late; and what leaves is looked at as it leaves and ends the line
  (reset) where it had waited too long (`test_core_device_line`'s
  `whatWaitedTooLongIsNotHandedOnWhereTheLinesOwnThreadWasHeldPastIt`). Those threads run above normal priority, so
  that work at normal priority does not put them aside. What remains, said plainly: where Windows puts such a thread
  aside for longer than the limit between its last look and Qt's own write (seen with the thread starved on purpose,
  and shown by cases that put it aside so), at most one message may still leave late, the last one its socket was
  given; and what Windows had already accepted into the socket's send buffer (four kilobytes, `roomWithWindowsBytes`) before the reset may still go out
  while the reset waits for that thread. On a line that is not reset (nothing has waited in qKVM past the limit), what
  Windows took is delivered whenever the way comes back: the bullet below, What is typed waits in qKVM. The line on the interface thread before this shared the second part. What is
  said counts as waiting from the moment it is said until that thread hands it to Windows, so a burst said faster than
  that thread hands it on (thousands of messages in a millisecond: a test's loop, never a person's input) is held to
  the line's room (`Limits::mostUnsentBytes`) meanwhile, as what Windows did not take is. (Outside
  review of tls, round 1.) A call Qt posts to the socket (its flush of what the socket holds) is looked at first too,
  and a line reset from its own thread is reset there at once, wherever that thread learns of it, but in one place:
  where it learns of it in Windows' judgement of the certificate after the handshake (a key update, a ticket, a
  renegotiation), Qt closes the line itself there, and what Windows had accepted for it still goes, as on the line
  before this (outside review of tls, round 2, L1: left for after 0.2.0, one call and a case of its own). The asking
  is not held to the limit: it is no input, it may wait behind a judgement of the certificate after the handshake,
  and the time to be let in bounds it. Mid-line, a judgement that holds the thread past the limit while something
  waits ends the line Late, where before it froze the window and the line went on: the right trade for input, said
  here. The line is not opened again by itself: whoever uses it opens it again as after any end (a console of RFB
  whose session served less than half a minute waits its ladder's first rung, 30 s, first). Every line's socket holds
  no more of what came than a message's worth, whatever the line reads or throws away, and the device is held back by TCP as before (kvmd's console and awake lines read nothing or short
  texts: unbounded, a device that floods them grew the program's memory at the link's rate while the window was busy).
  A thread Windows holds is let go of with its line and ends when Windows lets go, and its line's connection stays open
  until then (a console that takes one session at a time, an AMI controller's, says it is in use meanwhile, where
  before the window froze as long); so where Windows still holds four such threads of one place's lines, let go of more
  than two seconds ago, a line to that place is not reached (the device is shown as not answering) until Windows lets
  go, and other places' lines go on. A thread that cannot be started is counted out at once, and its line is not
  reached.
* **What is typed waits in qKVM a quarter of a second at the most; what Windows took goes when the way comes back**
  (review 2.6's rule, its limits named: the merged review of 0.2.0, B-M2 and round 2's M1, and the owner's decision of
  2026-11-19). The limit is of what waits in this program to go (`DeviceLine::Limits::lateAfterMs`). It is 250 ms on
  every line that carries keys, the pointer or a text: a kvmd KVM's line for keyboard and mouse, a VNC server's, a
  Proxmox guest's and an OpenBMC controller's console (each an `RfbScreen`), and from 0.2.0 on a qKVM server's live
  link (`LiveLink::Watch::lateAfterMs`), whose limit until then was its watch for silence (nine seconds,
  `LiveLink::Watch::silentAfterMs`: what waited in qKVM while its way stalled was handed on when the way came back, up
  to nine seconds late). What has waited longer ends the line there, goes with it, and is never handed on; the line is
  opened again as after any end. The one longer limit, for 0.2.0: AMI's console through its controller
  (`ServerScreen`) keeps ten seconds (`ServerScreen::Limits::sayMs`, the web's `CONSOLE_SAY`), and nothing that
  presses, moves or turns the wheel is said behind what has waited two seconds there (`inputSayMs`, the web's
  `INPUT_SAY`), so a key said within the first two seconds of a stall may wait in qKVM up to ten seconds and then go.
  Why: that line also carries the console's own answers and the letting go of what is down, a controller's small
  processor may read nothing for a few seconds and then read on, and a console ended there is opened again only after
  half a minute at the least, its one session perhaps taken meanwhile. To be made tighter after 0.2.0, once measured on
  the owner's real boards.
  What no limit of this program holds, on every line: what Windows has already taken for the connection (its send
  buffer, four kilobytes a line, `roomWithWindowsBytes`: some thirty keys), what is on its way through the network,
  and what the far end's TCP took while its program was busy. A short command typed into a stall is taken by Windows
  at once, nothing of it waits in qKVM, and it is pressed when the way comes back, unless the line was given up
  meanwhile for its silence: nine seconds on the live link (`LiveLink::Watch::silentAfterMs`), three on a kvmd KVM's
  line for keyboard and mouse (`KvmdConsoleLine::Pace::silentAfterMs`), forty on AMI's console
  (`ServerScreen::Limits::quietMs`), and on an `RfbScreen` whose screen does not change up to some 85 seconds (an
  update asked outright after `RfbScreen::Limits::heartbeatMs`, 25 s, then `messageWithinMs`, 60 s, for its answer).
  These are as read, not measured. Only a line on which more than Windows' room was said during the stall (a moving
  pointer, a long text) ends at the limit. A JetKVM's input goes over WebRTC, held to what may wait there
  (`mostBufferedBytes`), with no limit of time. Bounding what Windows holds is for after 0.2.0.
* **A KVM's session goes in a cookie, and in an address only to GL.iNet's** (the owner's decision of 2026-11-05:
  as the web, kvm.py `_line`). The pass a kvmd KVM gives goes with every question in the cookie
  `auth_token`. The one exception: the lines to its `/api/ws` (the one that keeps its video awake, the one for keyboard
  and mouse) carry it in the address as well where the KVM is known to be GL.iNet's (it answered
  `/api/upgrade/version` in GL.iNet's shape), as GL.iNet's own page sends it. To stock kvmd, which writes the address of
  every request into its log where the session would stand for whoever reads it, and to a KVM not known to be either
  (not asked yet, or its answer said neither), the cookie alone; a KVM that answers there with a 404 later is stock kvmd
  from then on. Never to the video's line (Janus).
* **A power button is not pressed a second time on the strength of a doubt** (as the web app does). A press of
  a KVM's power board holds the machine as one thing until the board has let go of the button. One whose answer said
  neither yes nor no (none in eight seconds, one that broke off, the KVM's 5xx), or whose driver was let go of while it
  was out (the KVM edited, held back for its twin, the program ending), may be taken by the board as late as that: the
  board is left alone for eight seconds and the button's hold from then, whoever asks, also a driver made anew for the
  same KVM, found by its address and by the certificate it is known by (`BoardHolds`, outside the drivers, as the
  sign-in pacing is). The board is held from a press's look to its click's going out, for as long as that road runs (a
  sign-in, a question of the Fingerbot's battery; five minutes at the most for one whose driver is lost), and a click
  still out holds it past its time while its answer is on its way (twice its time at the most). A press meanwhile is refused, and the KVM
  is not asked. Not across a restart of the program. A
  Fingerbot's press on the same machine is held by the same hold, from its look to its click as well, and holds it
  the same way (below).
* **A Fingerbot paired with a Comet is pressed one press at a time, and never for nothing** (the web's rule 29). Whether
  one is paired the Comet says only on a line to its events: it is heard on the one held anyway while the picture is
  watched, and kept beside the board's hold for a driver made anew. Where it has not said either way of late, a line
  is opened before a press (the press goes to the machine anyway) and its word waited for ten seconds; where it said
  "none", or nothing when it was asked, less than two minutes ago, or it is stock kvmd, a press is refused with no
  line opened (a line opened to a Comet woke its sleeping PC). A press (`/api/fingerbot/click`, firmly) holds the
  machine until the Comet has said yes, the press's own time has passed and a second more for the arm; only a 4xx is
  a no; anything else, none within eight seconds beside the press, or a driver let go of while it is out, holds both
  buttons of the machine for as long as a press taken then can last. Its battery is asked once an hour while one is
  paired (ten minutes after one unanswered, never while a press runs or may), and low below 25 %. Its strength is
  the Comet page's default ("firmly"): this PC's wall has no setting for it yet.
* **A server's controller is asked which it is on the connection its password goes on.** Its
  service root, which anybody may read, is read there first, and is to be a Redfish service
  root (answered 200, with its RedfishVersion) whether a number is kept or not: an error of
  its own, a page that is not there, another device's sign-in (401), and nothing goes. The
  number it gives is compared with the one kept since it was added; another number, or none
  where one was kept, and nothing goes. The password then goes on that very connection, on
  its own road and on its web page's (the way to its console), and so does the console's
  pass: over HTTPS, a connection made anew for it is stopped as the two agree how to speak,
  before a byte of it goes; where the root's answer says that its connection ends, it is not
  sent at all. The root is then read once more, on a new connection, and where that one ends
  too, nothing goes, and the card says that the controller closes the connection after every
  answer (stricter than the web, which lets a certificate no other card shows carry it on).
  Over plain HTTP a connection the controller ends without a word, or that ends because it
  answers in HTTP/1.0 (which Qt does not show), in the moment between its root's answer and the
  password, is not seen, and the password goes on a new one: nothing on a plain line holds it to
  that controller anyway (the outside review of 0.2.0's Redfish fixes, L3). Where no number is kept (one that gave none),
  nothing more is compared: the password is then held to that controller by its certificate
  (over plain HTTP, by nothing but its address). The server has the same gap. What carries a
  session after that (its token, the cookies of its web page, the console's line) is held to
  its controller by that session, which another controller does not know and refuses: nothing
  is done there. It goes on whichever connection is open to that address, so in a clash of
  leases under one certificate a session's token may reach the other board, which cannot use
  it on itself.
* **A server is never known by its certificate.** Its maker may have put one certificate,
  and one key, on every board. So a server's controller is one device by the numbers its
  computer and its controller give for themselves (kept with its card as digests), never by
  the certificate it shows; its kept password goes to its own address and to no other,
  whatever certificate shows there; and a second card of the same server is refused. Of
  several computers its controller lists (the nodes of a chassis), the card's is the one that
  gives its number, wherever it is listed (the first sixteen are read; one of them that answers
  and cannot be read is passed over, and what it answered is said only where none gives the
  card's number: the outside review of 0.2.0's Redfish fixes, L5; one that gives it beyond the
  sixteenth is not seen); where more than one of them gives it (boards whose maker gave them one
  number for all), none is the card's, nothing is done to any of them, and its card says so
  (outside review 2, 3.2: the first that gave it was taken). A number a board's maker puts on
  every board of its make is no number: AMI's `03000200-0400-0500-0006-000700080009`, and the
  same in the byte order of the SMBIOS table it is read from; a computer that gives it is held as
  one that gives none, to where the card's computer was found (review cx2 of outside review 2,
  M1: where the card's node was pulled, or could not be read, the other that gave it was taken).
  And where one of them answered and could not be read, and the one that gives the card's number
  is at another place than the card's computer was found at, what the one that could not be read
  answered is said, and nothing is done. A card's kept mark of that placeholder (0.1.x took it
  for a number) is read as none: the card is one known by its controller alone (review cx2-2,
  M3); one that keeps no place (as every card of 0.1.x) then says to change it and type its
  password, its one way past. What remains: two computers that give one number of
  their own that is no maker's known placeholder (a firmware fault), the card's node pulled, and
  the other taken for the card's; and, for a card that keeps no place yet, one such computer
  beside the card's node while that node cannot be read (taken, as a node that answers an error
  is passed over: L5 of the Redfish fixes; review cx2-2, L5). A press reads its controller's list anew as it begins,
  and does not go on what a look found up to ten minutes before. A card that keeps no number
  (one from a file without them, or added while it could not be asked) takes the first listed
  that gives one, as the web takes the first listed: nothing tells which the card was meant
  for (L4); and none where the first gives none. One that gives no number is taken for the
  card's only where the controller lists it alone, its controller's number compared before that
  session, at the place the card's computer was last found at by its number, and where that
  controller was never seen to list more than one computer; and a command resting on that goes
  only on a list read for it: the press's own, read as it began, for what it sends at once, and
  for a later command of a start into BIOS setup (its restart, its clearing) the list read anew
  right before it goes (outside review 2, 3.3: "listed alone" was taken from a list up to ten
  minutes old). Both are kept with the card (its
  stored data, section 11.2: `server_place`), the place from every read that gives the card's
  number, and the several from every read under a session its controller's number was compared
  for, or that gives the card's number (the outside review of its Redfish fixes, M1). A card
  that keeps no place (one from before 0.2.0, from a file of the wall, or one that went back to
  0.1.2, which drops it, and came up again) passes none that gives no number until its computer
  has given its number at a read, which keeps where it is: its node may have been pulled before
  that read came, and another, off and giving none, left alone in its stead (round 2, L1: one
  listed alone passed, run after run). Its card says so, and how past it: the server switched
  on another way gives its number, or the card is changed with its password typed. A card known
  by its controller alone (its computer gave none when it was added) is held the same way, to
  the place its computer was taken at when it was added; one of it that keeps no place passes
  nothing until it is changed with its password typed. Nor does a computer there that gives a
  number of its own now, on or off, pass for it by itself: the card's own, started since, and
  another node put in its slot are not told apart by anything kept (outside review 2, 3.4:
  whatever its computer said, it passed). Its card asks once whether that is the server it was
  added for (the owner's decision of 2026-11-22): its button, "Is it this server?", asks Yes or
  No, with what that computer says of itself (its model, serial number and host name, where it
  says them) to go by, apart from the card's words, and the keyboard at neither answer, so that
  no key says one by a slip (review cx2-3, L-d), every time it opens, whatever an earlier
  question in that window did: the box would give the keyboard back to the answer that had it
  last, a Yes clicked say (review cx2-4, M1). Yes takes that number for the card, with its controller's, where it was
  taken, with no password typed, where it is the number the box was shown and the computer there
  still gives it at a read made then (else nothing is taken, and the card asks of the one there
  now; review cx2-2, L1); the card is then as one whose computer gave its number when it was
  added. No, by its button, keeps it refusing ("No keeps refusing"): that place is kept as one
  whose controller was seen to list several (`server_place` among several, section 11.2), so that
  no computer passes for the card's there, giving a number or none, on or off, in this run and
  the next, until the card is changed with its password typed while its server gives its UUID (a
  change while it gives none takes the same mark, and the several stays: the card's words say,
  once the server it was added for stands there again, to switch it on from its BMC's own page,
  then change the card and type its password, or to remove the card and add it again, the one
  way past where its own server gives no UUID even when it is on; review cx2-3, L-a; cx2-4, L2)
  (review cx2-2, M2). The No goes whatever the card says by the time it is clicked. While the card
  asks it is taken whatever number it carries (it refuses the place, and no number); once the
  card asks no more, of a number the card asked about while its driver lived: the box may stand
  open while that computer goes off, gives none, and passes for the card's at a look (review
  cx2-3, M-a). A console opened since, with its live views and a typing on it, is let go of then,
  as where the card begins to ask (review cx2-4, L1); and a typing stops at its next asking
  whether it may go on wherever anything keeps the screen from the card (another server found
  at its address too: L3). One that cannot be sent, or is not taken, is said in the corner, with
  what to do: not to press the card's buttons, and to say No again when it asks, or remove the
  card. So is one its wall does not hand on: the wall does nothing else to a card while
  something is under way on it (a typing, say), and says so; what is under way then goes on to
  that computer to its end, and until the card asks again (once that computer gives its number)
  the one way to refuse it is to remove the card (review cx2-5, L3). It is answered "refused from now
  on" as the driver keeps it, and the wall writes it to the settings 2 s after the last of what
  it noted then, and at most 30 s after the first (the longer while many cards are first asked),
  with whatever else its drivers found (`noteByItself`), and at the wall's end; a write that is
  lost (a qKVM that crashes or is ended by force within those seconds, a settings file that
  cannot be written for the whole run) loses the No: at the next
  start the card asks again where that computer gives its number, and takes it where it gives
  none (review cx2-3, L-b). 0.1.2 reads no `server_place`, and holds such a card to no place and
  to no No: there it takes whatever computer answers at its controller, the one it was told No of
  too. Back on 0.2.0 it refuses again, as among several, or as one that keeps no place where 0.1.2
  wrote the settings and dropped it, until it is changed with its password typed (review cx2-3,
  M-b; a known limit, CHANGELOG). A mark that 0.1.2 would refuse is not written: it would drop
  the controller's number, which holds the password to its controller before it goes.
  The box put away (Esc, the cross, a click beside it) sends nothing, and the card goes on asking.
  Until it is answered, what is read of it is read (the card's looks, and the watch of a start
  into BIOS setup under way, which says what it saw), and nothing else goes to it: no power
  command, no boot override and no clearing of one, no key, no text, no saved action, and no
  console, so its screen is not shown either: a console open when the card begins to ask is let
  go of, with every live view of it and what is being typed, no key after it (review cx2-2, M1;
  `test_core_wall_server_screens`); and a look that fails while it asks says what failed, and not
  how the server was at the last look before the asking (review cx2-3, L-c). Each says that
  its card asks first, the card offers its two answers alone, and the box that changes it says
  to answer on the card or type its password there (L4). A change with the password typed takes
  the server that answers then, as an add does, and where its sign-in found its computer; where
  that is the server the card was known by, that its controller was seen to list several stays
  (round 2, L1). What is kept is let go of only so (round 2, L2): a place by the computer's next
  read that gives its number at another place (a firmware that renames it, a trailing slash
  too), or by a change with the password typed; the several by a change with the password typed
  that takes another server (one known by its controller alone where it was known by its
  computer, say), or by the card taken off the wall and added again; a stored value of no shape
  the program writes is read as one of several (section 11.2). Until then one that gives no
  number is refused, and its card says to change it and type its password. So another node of a
  chassis never passes for the card's, after a restart of the program too, but in one place no
  number the program keeps can tell: on a controller never seen to list two, another node in
  the card's own slot (a controller that names its computers by their slot, whose one node was
  changed between two looks or while qKVM was closed), off and giving no number, is the card's
  own as far as anything read tells, as the owner's boards, one computer each, are when they are
  off and give no number; those are pressed as before (`test_core_redfish_which_server` w4, w7;
  a second number of the computer's, its serial number say, kept beside its place, would tell
  them apart, and is not in 0.2.0). For a card known by its controller alone that place is the
  same, but on or off: its own server may give no number at all, whatever its state, so another
  node in its slot that gives none, running or not, is its own as far as anything read tells
  (`test_core_redfish_which_server` t14, w14, w16). Where its computer gives no number, the server is
  not asked whether it is off, as 0.1.2 did not ask: a board that gives none as it restarts would
  have its start into BIOS setup refused, and its override left set. So a node of several that gives no number (while it is off,
  or as it restarts) is not pressed, watched or cleared then: its card says which server answers cannot be told, and a
  start into BIOS setup says it may be set and could not be cleared (L5). A command goes only to
  the computer it was decided for: where its controller signed qKVM in anew before it went, or
  its computer was judged at another place since, or the judgement rests on an older list than
  its own (above), the list is read anew and its computer judged where it is now, and where that
  is another place, nothing is sent, and the press says so (outside review 2, 3.1: after a second
  sign-in the card's computer was found at its new place, and the command went to the old one,
  where another node stood). A controller is
  signed in to with a session (never Basic), one kept by the card's driver and given back when
  the driver goes, also at the program's end within its time.
* **A server is never left booting into its setup** (Start into BIOS setup on this PC's own servers, the web's
  `bios_setup`, stricter). Offered where the controller allows a "BiosSetup" boot override, for one machine, asked
  first. Where the controller says which kinds of boot override it allows (`BootSourceOverrideEnabled@Redfish.
  AllowableValues`, as a list) and "Once" is not among them, the press is refused with nothing sent and nothing noted:
  "Nothing was done: this controller allows no boot override for the next boot only ('Once'), so a start into BIOS
  setup would leave every later boot in the setup too." (the owner's decision of 2026-11-18, a; one that says nothing
  of them is asked as ever). Which server answers is asked first; another tool's override is not replaced (a "BiosSetup"
  one for every boot among them: "Clear it first"), nor one whose state the controller does not say (a Boot without
  `BootSourceOverrideEnabled`: "Nothing was done: the controller does not say whether a boot override is set", the
  outside review of 0.2.0, area 5, MEDIUM 5.8); a server between states is left alone. The one-time override is
  set (shown the state it was decided on) and seen set (or, where no read of the server comes back after the
  controller said yes to the change, after its task's end where that end was seen (reads while it ran show it unset,
  as they should), and no failure was said at the task's first look, not seen, and gone on with as 0.1.2 and the web
  go on: the outside review of D1, L7, kept for the outside review of 0.2.0, 5.6, and said so here since its Redfish
  fixes, L2 and round 2, L5), and the server restarted where it is on or switched on where it
  is off, within two minutes of the press or not at all: held to that when it goes too, so that a restart that waits
  for a sign-in, for which server answers to be read again, or is asked again after a 401, is never sent once the two
  minutes are over; the press then says "Nothing was restarted: the controller took more than 120 seconds before the
  restart", and clears the override (the outside review of the fuzz work, round 3, L4). A change refused for the
  state it was decided on (412, 428) is read and looked at again and shown the mark that read gives: told it is meant
  whatever the state ("*") only where the controller refuses the very mark it gives, gives none, or refused it twice,
  and only then is a change another tool makes in the moment between that read and this change written over (5.8).
  Every clearing, the press's and the one on the way out, is sent again after a refusal by the same rule (the outside
  review of 0.2.0's Redfish fixes, L9: "*" after one refusal, which wrote "Disabled" over a provisioning tool's
  one-time override set in that moment).
  From the moment the override may be set, every way out clears it again where it still shows: a refused or unsent power
  change, a change answered with an error or not shown, a press that ran out of time; and after the restart the start is
  watched for the server taking the override, and one it has not taken by the end of the press's four minutes is cleared
  all the same (the web leaves it set where the restart may have gone). Only a "Disabled" in so many words, read back,
  counts as taken or cleared: a clearing's own answer (a 204 among them) is read back (5.7), and a read that shows no
  override (no Boot, a null, an empty one) says nothing, and is watched or read on within the time left. A board that
  keeps "Once" as "Continuous" (a firmware fault) shows "Continuous", "BiosSetup" after the change: that override is
  qKVM's own (the press is refused where any other stood before it), never another tool's
  (the owner's decision of 2026-11-18, b). Shown after the restart, the start is watched on to the press's end, so that
  the server has its time to start into its setup with it, and it is cleared then, in one more change sent only for
  qKVM's own override: "The server was restarted with its BIOS setup boot override set, but its controller kept the
  override for every boot ('Continuous') in place of the next one only; qKVM cleared it again, so the server's later
  boots are ordinary ones." Shown before the restart (the read-back, or the read before a task's look), nothing is
  restarted on it and it is cleared at once: "Nothing was restarted: the controller set the BIOS setup boot override
  for every boot ('Continuous') in place of the next one only; the BIOS setup boot override was cleared again." Every
  clearing of the press, and the clearing on the way out, clear it and do not take it for an override off, nor does a
  look of the wall: its record stands, and its card says it, until a look shows the override off. Where it could not
  be cleared, "may be set" is said, and how to see to it. Power is checked, not believed: a server switched on that
  stays off is said so, and an override read "Disabled" while it is off is no start into its setup: the start is watched
  on (5.7). So is it where its controller kept the override for every boot (cleared at the end, and the server said
  still off), and where another tool's override shows meanwhile, which is left as it is while the start is watched on
  to the press's end (outside review 2, 3.6: "switched on" was said of a server that stayed off, and the press was
  done). "Still off" is said only from a read after the switch-on, the watch's or a clearing's, that showed it off;
  where none came back (a controller busy through the server's start), the press says the server was not seen to start
  into its setup (review cx2 of outside review 2, M2: said from the read before the press). At the program's end, or where the server's driver is let
  go of mid-way (its card changed), an override that may be set is cleared on the way out by the program's goodbyes,
  under the driver's session, and read back, thirty seconds at the most, before that session is given back; the press
  is told that it may be set. From the moment the change can go out the server is noted beside the settings as "may
  be set" (no secret in it), and let go of only where the override is seen off with nothing of the press's change
  still on its way; otherwise the next start tells it, as a notice and on the server's card, until a look shows the
  override off. Where that note cannot be written (a full disk, a file that cannot be written), the change is not
  sent: the press ends saying so, with nothing changed on the server. Where the controller only queues the change (a
  202, the monitor of a task named, or a Task as its answer), the server's record is marked queued and written down at
  once, at that answer, before anything else of the press (a program ended by force then leaves it marked); so is it
  where the change's answer was lost (none in its time, or a line that fell: it may have been a 202), and the press says
  that the controller may have queued it (5.9). From 0.2.0 on it is marked so, and written down, as the change is handed
  to its line (after any sign-in it waited for; each time it goes, after a 401 too), and the change's answer takes that
  mark off again where it says the change was not queued (made, refused, refused for a state that changed, never sent):
  a program that ends while that answer is out (quit, a crash, Windows' restart) leaves it marked, and a driver let go
  of then says that the controller may have queued it (outside review 2, 3.5: an ordinary note stood, which the first
  look after its doubt let go of, so that a change the controller queued landed at a later boot with no warning left).
  Where that first mark cannot be written down, the change is not sent, as where its note cannot be (review cx2, L3).
  The card's words for such a mark are those of a queued change, though the controller may not have queued it: the
  record keeps no word of which it was. Where the mark of a queued answer cannot be written down (a disk that filled since the
  note, its folder taken away), the press says so: it stands in memory, on the card until the program ends, and goes
  into the file with the next writing that works (5.9). The mark comes off only by reads of the server's own computer
  that see this very change through (the outside review of D1,
  round 2, H1): the override shown "Once"/"BiosSetup" at a read after the task's end was seen, or shown so at a read
  after the change and then shown "Disabled" at a read after the restart (the server took it as it started). The
  task's own word never takes it off, whatever it says (a Task "Completed", a monitor's 204, a failure): a board that
  stages the change for the next boot can say its task is done before the server shows it. The reads that count are
  the read-back's and those before the task's looks; the change's own answer (a 204 that names a monitor all the
  same), the clearing's read and another tool's override after the restart count as none, and an override shown
  "Once"/"BiosSetup" already before the press (set by hand in the BMC, or left by an earlier press) proves nothing of
  this change (round 2, L3, L6). Where no read can tell, the mark stays, with 0.1.2's notice: nothing is lost against
  0.1.2. Wherever a press leaves the mark of its own change on, it says the queue, as its card does; a driver let go of
  in the middle of such a press keeps the mark, and says so. From 0.2.0 on the task is followed where the answer names
  one (D1, as the web follows AMI's mounts, redfish.py `_task_done`): the Task its body names (`@odata.id`), else the
  place its Location header names by its path (a task monitor), each only below the controller's
  `/redfish/v1/TaskService/` and a place on it that climbs nowhere, a Task before a monitor; where the answer names
  both, only where they name one task (the same place, the monitor below the Task's own, or the monitor the Task names
  as its `TaskMonitor`), else neither (round 2, L4). Looked at every two seconds, for one minute at the most (half the
  restart's two), never into the read-back's 25 seconds before the restart's own time, which the press keeps for after
  the follow, and sixty times at the most (`Pace::taskWithinMs`, `mostTaskLooks`; the outside review of D1, M1: a task
  that does not end used up the whole window before). Before each look, the first too, the computer is read, and each
  read and each look is held to that time, so that a slow or a silent answer takes none of the read-back's (round 2,
  L1): a sign-in before it goes (the session ran out, or was given back), the same question asked again after a 401,
  and the judge's read of the controller's number where the server gives none, too; what is not answered by then is
  answered as too late, and nothing of it is sent after (the outside review of the fuzz work, round 3, L3). A change
  answered so late that the read-back's time has begun is not followed at all. A
  read that shows the change made goes on as after a 202, the server restarted into its setup as 0.1.2 restarted it,
  however long the task runs on (a board whose task ends only at the next boot, as pending settings do); its mark goes
  where the start is then seen to take the override. A task not ended in its time leaves the read-back its time: the
  server is restarted once the override shows set, within the press's two minutes and never after. A monitor's 202 or
  a Task in a state that is no end ("New", "Running", "Pending", "Suspended", ...) is under way; a passing error at
  either (a 5xx, 408, 429) says nothing of the task, and it is looked at again. An end counts only where it is surely
  this change's (`redfish::taskOf`): seen at a look after the task was seen under way at one before; of the task the
  change named; a Task that names itself nothing but the place looked at (at a monitor, that monitor's own Task), and
  whose Payload, where it has one, names a PATCH of the server's own computer. What the person sees:
  - Done (a monitor's 204 or 200 with nothing, or a Task "Completed"), then read set: restarted into its setup, the
    press ends as any does, nothing of a queue is said, and the mark goes.
  - Done, and a read after the task's end shows it unset for the whole read-back: nothing is restarted, the override
    is cleared all the same, and the press says "Nothing was restarted: the controller's task says it is done, but the
    server does not show the BIOS setup boot override set; the BIOS setup boot override was cleared again. The
    controller queued the change: it may still be made later, whatever its task says. Look at the server's boot
    override in its BMC before you rely on it." The mark stays.
  - Done, and nothing read back after the task's end (reads while it ran showed it unset, as they should, and say
    nothing of its end: the outside review of the fuzz work, round 3, L1): restarted, as after a 202 in 0.1.2 (the
    task's word and the press are then all there is to go by, and a restart is what was asked for: no power action
    nobody asked for; the outside review of D1, L7), the queue said in the same words, the mark kept.
  - A Task ended in "Exception", "Killed" or "Cancelled" (or "Completed" as "Critical"), or a monitor's error answer (a
    4xx, the change's own answer as DMTF has a monitor's last, as AMI's real board answered when its task failed),
    after the task was seen under way: nothing is restarted, the override is cleared, and the press says why in the
    controller's words ("the controller's task did not set the BIOS setup boot override (...)", "the controller
    answered for its task with an error (...)") and says the queue. The mark stays. Where the override was "Once",
    "BiosSetup" already before the press, the computer is read once more: still shown, it stands as it stood, and the
    server is restarted into its setup within the two minutes, as 0.1.2 restarted it, the words said beside it ("...,
    but the server showed it set from before the press, and was restarted into its setup."), the queue said, the mark
    kept (round 3, L6 a).
  - An end at the first look (perhaps an old task's, ended before the change): read back as a task that cannot be
    followed. A failure said then, by a Task ("its task said at once that it did not set it (...), and qKVM cannot be
    sure that task was this change's") or by its monitor's error answer (a 405 from one that takes no GET, say: "its
    task's monitor answered at once with an error (...), and qKVM cannot be sure that answer was of this change"; round
    3, L6 b), is said, and with it the server is restarted only where a read shows the override set, never blind
    (round 2, L2). Where a read shows it set, the server is restarted and the words are said beside it ("...; the
    server then showed the BIOS setup boot override set, and was restarted into its setup."); the restart then seen to
    take it, the reads saw the change through, and the mark goes (round 3, L2).
  - Everything else is a task qKVM cannot follow, a queued change as 0.1.2 has it: one not to be read (a 404, a 401, a
    Task's own place answered with an error or with nothing, what is no record, a state no Task has or none at all), a
    look that names another task, a Task of a change of another place or by another method, or a task not ended
    within its time or its looks. Read back and restarted as 0.1.2 does; where its change is not seen through, the
    press says that the controller queued it and that qKVM cannot follow it to its end, and to look at the boot
    override in the server's BMC.

  A 401 at the task's place is that place refused, not the session (a monitor bound to the session that made it, or
  of other rights): the session is kept, with no second sign-in and no hold on the controller's sign-ins, and the
  press reads back and restarts under it (the outside review of D1, M3). No look and no time lets a record marked
  queued go: its card (and the notice at each start) says "The controller queued a change qKVM could not follow; check
  the boot override in its BMC" until one of three things: reads of the press that marked it see its change through,
  a later start into BIOS setup on that server ends as it should (its change seen made, and the override read
  "Disabled" after the restart), or the server is taken off the wall, and the record goes with its card
  (`SetupOverrides::markQueued`, `seenMade`, `forget`). Nor does the cap: at most 256 records are kept, and none is
  let go of to make room, a queued one or one whose card says its override may be set; where 256 are kept, a press on
  another server is not noted and does not go, nothing sent, in words that say why (`too_many`; the owner's rule, the
  merged review of 0.2.0, A-L1, where the oldest went, a queued one too, and round 2's L4, where an unmarked one on a
  card went). A mark an earlier press put on the record stays when a later
  press's change is seen through. A change answered 204 counts as seen made, as the standard has it and as in 0.1.2:
  a later press answered so and restarted, whose watch then reads "Disabled", lets an earlier press's mark go with
  the record (the outside review of the fuzz work, round 3, L7: left so; it matters only for a board that answers 202
  once and 204 another time while it stages both). Install and restart is held as for any press, by the press's own doubt alone; the
  record and its mark are carried across an update in their file. A record of a server on no card of the wall any
  more (taken off by a version before, or by a file that replaced the list), its doubt over and not marked queued,
  goes at the next start and at the next change of the wall's list (`SetupOverrides::prune`): nothing of it can come
  and nothing tells of it. One whose doubt lasts is never let go of that way; before 0.2.0 such a record stayed in its
  file for good. Nor is one marked queued, whatever the list says: a list read whole at one start may be one that a
  run wrote after its settings could not be read, with none of the old cards (the outside review of the prune, round
  2, M1); it goes only with its card, or stays in its file, holding nothing. Only a list that was really read says
  that a server is on no card: nothing goes in a run whose settings could not be read at its start (held by another
  program for longer than they are tried, damaged: the program starts as new), nor where their list is no list, holds
  a KVM this version does not read (a later version's), or lost an entry in its reading (`Settings::listReadWhole`,
  the outside review of the prune, M2); its records come back with their cards once the settings are read again. Nor
  does anything go while settings that could not be read at a start stand put aside beside the settings
  (`settings.unreadable.json`, `-N`; `Settings::keepsAsideWhatCouldNotBeRead`): the run after such a start writes
  settings of no cards, and its list, read whole at the next start, is no proof that a server is on no card; put back
  as the start's notice invites, the kept file brings the cards back, each with its record's note ("may be set", or
  the queue). Once whoever looks after the PC takes the kept file away, the prune goes by the list as it stands (the
  outside review of the fuzz work, round 3, M2). The records' file itself is read as the settings are: tried forty
  times where it cannot be opened, and one that still cannot be read whole is never written over nor replaced by one
  that drops its records. It is put aside before anything is written (`settings.json.setup-overrides.unreadable`,
  byte for byte), and the start says so: "qKVM could not read all of its note of which of this PC's servers may still
  boot into their BIOS setup (...). It is kept as it was, as ... beside the settings, and a server it names may say
  nothing of it on its card. Look at the boot override of this PC's servers in their BMCs." Where it cannot be put
  aside (something holds it), nothing is written and no start into BIOS setup is sent until it can be read or put
  aside, which the start says; once it opens, what it holds is taken in, its queued marks too (round 3, M1).
* **A screen is watched by the pictures the wall gets anyway** (screen watches on this PC's own wall, the web's
  screens.py). A KVM may be watched for no signal, or for an unchanged screen, for 1 to 1440 minutes (its Change box).
  Nothing is asked of a KVM for it: the pictures its cards and views are handed are compared, one every 15 seconds at
  the most, shrunk to 64 by 36 squares of brightness in the program's pool, never on the interface thread; its files
  are read and written in a thread of their own (what stands read back after a start before any round), and the
  program's end waits for neither that thread (two seconds at the most) nor a round's comparing. The picture
  that set an alert off is kept beside the settings for 30 days (looked for at each start, and every hour; the last of
  each KVM, gone with it), and shown in its
  Details; the alert is one of the program's own notices, and "back to normal" is told when it is over, also after a
  restart.
* **A server's console is opened only where the server's own monitor stays on.** Its
  controller hands out two words with the console's pass, `automatic_off` and
  `local_monitor_off`. The console is opened only where both are said, and both are a plain
  no (0, false, "0", "off", "no", "disabled", "false", in any case, among blanks). Anywhere
  else it is left alone, and the card says why, in the web's three sentences (rule 15 of its
  notes for the desktop, as web 0.3.2 has it): where either is not said at all, or said as
  nothing (an empty text, or blanks as the web's strip() takes them, the separators 0x1C to
  0x1F among them), that it does not say whether it does; where both are a yes, or a word
  that cannot be read, that the controller is set to switch the server's own monitor off
  while its console is open; where one is a plain no and the other is not, only that one of
  its two settings is off (whether the monitor goes dark then is not known). Whoever stands
  at the server would see the monitor go dark: that is not "only looking" any more. (The
  owner's decision of 2026-11-05: a word that is not said is no no. Web 0.3.2 holds the same
  since its commits 06f6242d, b30be80b and 1cac5c63.) This is the rule of AMI's console, whose
  pass carries the two words. OpenBMC's console (bmcweb's `/kvm/0`, M7) has no pass and no such
  words: its controller says nothing of the server's own monitor, and it is opened without
  them, as the web opens it (`openbmc.py`). It goes under the driver's own session, held to
  the card's computer as M7 says, and carries no password.

A wall screen (built), for a server's machines and this PC's alike:

* **A display link makes this PC a screen that only shows.** From the moment a display
  link signs a QKVM server's wall in here, the whole program shows and does nothing:
  every window shows the wall, this PC's own KVMs with it, and nothing can be done to
  any machine of any wall, by any way the program has (a menu, several at once,
  keyboard and mouse, saved actions, the wall's order, the server's wall as a file,
  changing or taking off a KVM of this PC). A passer-by has no control through qKVM's interface alone; with the Windows
  account, a Credential Manager Restore can replace the app password record and its retry count. Adding a KVM, editing saved
  actions, and this PC's own import and export boxes follow the same restriction in C++.
* **It stays a screen whatever happens to it**: the server taking the link back, the
  server away or restarted, a QKVM of another version, a certificate to be looked at,
  the program started again. It ends only when a person at this PC chooses "Sign out
  wall screen" and gives the local app password. This also works while this PC's own
  wall is selected or the server is unavailable. Saved display links and their
  sessions are removed locally before actions are enabled again.

The program's own password (decided by the owner on 2026-10-18; implemented in the
2026-10-21 development source). The model is
the client program of a camera system (Hikvision iVMS-4200, Dahua SmartPSS) and the QKVM
server itself: the program has an admin account of its own, as the server has, because a
wall often runs on a PC whose Windows account is shared by everyone in the room, and
Windows' own sign-in then tells nobody apart.

* **Made at the first start, and required.** Ten signs at the least, as the server's.
  "Sign in automatically on this PC" comes ticked, so that a wall that is used every day
  asks nothing at its start; cleared, the password is asked at every start.
  The owner's 2026-10-25 decision removes development-profile adoption. Setup is
  permitted only with no credential entry under the application prefix and no
  saved inventory, incident history or lock marker. Window geometry and update-check
  metadata written before password setup completes do not count as prior use. Unknown
  and empty credential entries count too. Any evidence of prior use without a valid password
  requires recovery; another password cannot adopt existing secrets. Setup rechecks
  the credential prefix before saving the derived password.
* **What it guards.** Unlocking a program that was locked ("Lock" in the menu: every
  window only shows, and nobody acts anew on any machine, until the password is given; M3d
  says what was asked just before and still waits for a sign-in) and
  signing out a wall screen (a display link's screen only shows until a person who knows
  the password ends it); writing a file of the wall that holds passwords (a sealed
  export); changing the password, and ticking the automatic sign-in. Not the wall's daily
  use: looking, acting, adding and changing KVMs ask nothing more.
* **A lock outlives the program.** A program that was locked, or is a wall screen, starts
  locked again, also where it signs in by itself; a wrong password waits longer each time
  (1, 2, 4 ... seconds, at most 300), and that is counted over a restart too. However, restoring an older backup resets the wrong-try count.
  Within a run the wait is also counted on a clock nobody sets, and the later of the two holds:
  setting the PC's clock ahead does not shorten it. A clock put back holds a correct password no
  longer than the wait the count owes, counted from when the program sees the clock go back. A day
  without a wrong try forgets the count (within a run, a
  day on both clocks). Changing the system time takes an administrator on Windows; across a restart
  only the PC's clock is known.
* **The password is never kept**: only what scrypt makes of it, with its salt, beside the
  other secrets in Windows Credential Manager, and never in the settings file.
  Password work uses scrypt N=131072, r=8, p=1 on a worker thread. A nonsecret
  `.locked` marker beside the settings provides a second persistence path if
  Credential Manager refuses a lock write. If both stores refuse writes, the
  session remains locked and reports that the lock could not be saved.
* **A forgotten password** is reset from the sign-in box, at a price that keeps the lock
  worth something: every secret the program keeps is erased (the KVMs' passwords, the
  texts of saved actions, servers' sessions and display links). The KVMs stay on the wall
  and ask for their passwords again. Where the program's record of its password is gone
  or damaged after setup was established, it is treated as forgotten, never as a first
  start. The password record has a strictly checked binary form with an odd byte count,
  which Credential Manager text editing and cmdkey UTF-16 input cannot create. It does not
  protect against arbitrary code running as the same Windows user; there is no legacy JSON
  record migration in this unreleased product. First setup requires the entire prefix
  to be empty and no prior profile evidence, as above, but for one entry the program keeps for itself
  with nobody's password in it: the key its wrong-password counts are sealed under (`sign-in-pacing`),
  made at every start before the lock looks, and erased by a reset with the rest (its file is then put
  aside at the next start). If the password was saved but its marker was
  not, the next start asks for that password and can finish setup without erasing
  other secrets. A credential store that Windows does not answer (its list of qKVM's entries, or a
  listed password record it does not hand over) is not taken for one that lost anything: the
  program says so ("Saved secrets not available") and offers Try again and Quit (and the box's close
  button, which leaves the program locked), never setup and never a reset; the app lock writes
  nothing until Windows answers, and then it carries on as usual. The
  same holds where the list fails at the end of setup: setup stays, and nothing is saved.
  Reset requires a separate confirmation, enumerates this application's credential
  prefix so orphaned entries are included, and keeps a recovery marker until
  erasure is complete. The nonsecret settings-side marker also records an
  unfinished reset, so a refused credential write cannot restore automatic
  sign-in on restart. If neither marker can be saved, no secrets are erased and
  the app explicitly reports that reset persistence failed. The app then closes
  after successful erasure to discard cached credentials. The
  next start creates a new app password; KVM inventory and action metadata remain.
  The reset writes a nonsecret completed-reset marker only after re-enumerating an
  empty prefix. This explicit authorization allows setup with retained inventory;
  the marker never allows setup while any credential remains. Successful password
  persistence removes it. An interrupted reset stays in recovery.
* **No proxy.** The lines the program itself opens go straight to where they are meant to: a qKVM
  server, a KVM, the update feed. No proxy the program or Windows names (WPAD, PAC, the system's own)
  is used for them, so none can see or hold those lines, and Windows never signs on to one with the
  user's account for them. Windows' own certificate checks (revocation and chain fetches, for the
  installer's signature and the feed's certificate) follow Windows' own settings, which the program
  does not choose. Where a network reaches the internet only through a proxy, update checks fail;
  an explicit proxy setting is the owner's to decide.
* **What it is not**: with the automatic sign-in it guards the program's doors, not
  Windows'. Somebody who runs programs of their own in the signed-in Windows account can
  still ask Windows for what the program keeps there, as with any program of this kind.
  The stricter way, later and as a setting: the kept passwords sealed under the program's
  password, which is then asked at every start.
* **A QKVM server's machines** keep that server's own accounts, as now; a hosted service's
  sign-in comes after 1.x.0. Accounts of the program's own for several people (admin,
  operator, viewer, as the camera programs have) are later.

**App-lock boundary on a shared Windows account:** Windows Credential Manager's
**Back up Credentials** can export qKVM secrets without the app password. **Restore**
may import a valid binary password record and secrets from another PC. The binary
record prevents typed-text forgery; it does not prevent credential backup/restore or
access by another program running as the same Windows user. App lock alone cannot
protect retained secrets from another person using that unlocked Windows account.
Sealing secrets under the app password is planned, not implemented. The backup/restore
acceptance probe is VM-only and remains unverified; no owner credentials were exported
or restored for this work.

And for both:

* **Secrets live in Windows Credential Manager** and nowhere else: not in the settings
  file, not in a log, not in a crash dump's file name, not on a command line. That is a
  session with a QKVM server, a display link's key, the password of each KVM that was
  added on this PC, and the text of each saved action of this PC's wall. Windows keeps
  them for whoever is signed in to it: another program that runs as the same person can
  ask Windows for them. (The program's own password guards its doors, above; sealing what
  is kept under it is the stricter way, later and as a setting.)
* **What is typed is never written down.** Not keys, not pasted text, not passwords: the
  logs say that something was done, not what.
* **The program never runs as administrator**, and its installer does not ask for it.
* **Nothing it downloads is run unchecked.** An update is verified against a key that
  ships inside the program before a byte of it is executed. The one thing it ever runs
  is a Setup copy's setup, when the person asks, from the very file that was verified,
  held so that nothing can change it between the check and the start.
* **It sends nothing anywhere** but to the KVMs its owner added, to the servers its
  owner connected it to, and to the update address. No telemetry.
* **A reply is never trusted**, a KVM's no more than a server's. Sizes are bounded, JSON
  is checked, a picture's stated size is checked before memory is taken for it, and a
  stream that misbehaves is closed. What reads a device's bytes is tried with bytes that
  were made to break it (fuzzing) before it is handed out: from 0.2.0 on, the readers of
  what a KVM or a server sends that DESIGN.md section 8 names (all but the few it names as
  not fuzzed yet) have libFuzzer targets in the tree (the `fuzz` preset), run
  by every check for a bounded time from seeds the tests' own inputs make at build time,
  and the check fails on anything one finds, on a target it could not start and on one that
  fuzzed nothing (`tools/fuzz.ps1 -Minutes 6 -FastCores`, DESIGN.md section 8; on the build
  box, held to its fast cores, the step took 6.59 minutes from nothing and 6.46 built,
  2026-11-17). Each
  target is shown to reach deep into its reader by a canary of its own: built with
  `QKVM_FUZZ_CANARY` (the `fuzz-canary` preset), each harness ends its run where its
  reader read an input to a deep state it names, which none of its seeds reaches, and
  `tools/fuzz.ps1 -Canary` (from the seeds) or `-Canary -Smallest` (from one small seed
  each) passes only where every target finds its own within its time.
* **No incoming network connections.** No bundled or launched server and nothing on
  this PC's network for anybody to speak to. Native Setup and the proposed isolated
  decoder are local helpers, never servers. Decoder isolation is reviewed before
  implementation in Phase 2. (A second start of the program tells the
  first to show itself, over a pipe that only the same person signed in to Windows can
  open.)

## 9. Targets

They began as first guesses, to make M1 measure the right things. Where M1 has measured,
the number is beside the target, and a target that turned out wrong is corrected here.
The measurements are from one fast PC, and from the
engine in a bare window unless it says otherwise. The machine shown large in the program's
own window is measured again by `tools\measure_live.ps1` (part 9), and the walls by
`tools\measure.ps1` and the session rig (parts 1 and 7). JetKVMs (M6 2i) by
`tools\measure_live.ps1 -JetKvm` (part 10), so far against the fake only: the program's own
JetKVMs by hand on this PC, and the web's aiortc fake; the owner's JetKVM comes later.

| What | Target | Measured in M1, and in M2 where it says so |
|---|---|---|
| Start to first tiles, with the KVMs or the server reachable | Under 2 seconds | M2: 2.0 to 2.2 seconds to the wall's cards with a session that was kept, 1.5 to 1.7 to the window's first frame. Not quite met |
| A wall of 64 preview tiles at one picture a second each | Under 5% of the processor of a current desktop PC; no interface frame over 16.7 ms | 0.9 % at two pictures a second, without the interface. M2, in the program's own window: 0.5 to 0.7 %; a frame's work 1 ms and never 5; but a few frames in some hundreds are handed over 38 to 62 ms late, and the interface thread answers later than 16.7 ms 3 to 4 times in 30 seconds. Not quite met. 2026-10-27 (part 9, one run of 30 seconds, the PC otherwise idle): 0.33 %, no frame handed over later than 16.7 ms and no answer later than 16.7 ms |
| A wall where nothing changes | No drawing at all | M2: none on a paused wall that is only shown; one frame a second where the bar's clock is in sight |
| Controlled machine, decoded on the graphics card: frame arrived to present returned | A mean under 2 ms and 98 in 100 within 4 ms, at 1080p and at 4K (it was: under 8 ms and under 12 ms) | 0.7 to 0.9 ms mean; 98 to 99 in 100 within 4 ms. In the program's own window (part 9, since the frame of a new picture of a machine shown large is asked on the window's drawing thread; one run of 30 seconds each, on a monitor of 120 a second): 0.5 to 0.7 ms mean, 99.4 to 99.7 in 100 within 4 ms, at 1080p30, 1080p60 and 4K60. Met there; on the same PC busy with other work, the moving 1080p60 film 1.0 to 1.1 ms the same day |
| The same, decoded in software | Under 4 ms at 1080p; 4K in software is outside the target | 1.8 to 3 ms at 1080p30; 7 to 10 ms at 4K60. In the program's own window: 2.8 ms at 1080p60 (97.5 in 100 within 4 ms), 3.5 ms for a 1080p JPEG; 8.9 ms at 4K60. Met at 1080p |
| A server's screen, read from its controller's console by the program's own ASPEED decoder (M7 3c) | Off the interface thread: while a busy screen is read, what that thread does itself in any one wait under 16 ms, and it is still no longer than 50 ms (under the address sanitizer, which makes all of it slower, 100 and 134 ms) (since review 3's HIGH 1, 2026-11-06); for the reader itself none set, measured for what the standard library's checks in it cost | 2026-11-06, in the debug build on the PC's slower cores with other work beside it (core/tests/test_server_screen.cpp and test_wall_server_screens.cpp, a busy 1080p screen of six frames): the interface thread's longest stillness 4.6 to 6.7 ms while the frames are read and shown, and 2.7 to 3.7 ms with a card asking for its still ten times a second, measured then by a clock of 15.6 ms steps; before, on the interface thread, 612.6 and 568.1 ms. Measured again by a clock of 1 ms steps, the thread's own work in one wait (a card's still, the busy screen): at most 5.6 ms in the release build, 6.3 ms in the debug build, 18 to 52 ms under the address sanitizer; with the reading put back on the interface thread, 39, 265 and 271 to 295 ms. 2026-10-29, on one slower core of the PC, as the release build compiles it, best of five runs over a busy 1080p screen made by the tests' own writer (every block JPEG, 1.6 MB a frame): 29.5 frames a second without the checks, 21.5 with them (-27 %). It is built with them: an index out of bounds, also inside one block, ends the program there instead of reading or writing where it should not (bmc/CMakeLists.txt). The cost is accepted (2026-10-29) |
| Controlled machine: every picture that arrives is shown | None made way for a newer one or was thrown away, at 30 and at 60 a second, 1080p and 4K, on the card | In the program's own window, since the frame is asked on the drawing thread: 0 of 901 to 1,859 in 30 seconds, every film (part 9, one run each). Met there (before: 1 of 1,800 in one film of part 9's first table) |
| Controlled machine: even steps | Under 1 in 100 pictures taking more than 8 ms longer or shorter to the screen than the one before, at 30 and at 60 a second, on the card | 0 to 0.2 in 100 (part 9; 7 to 8 in 100 with the moving films of 60 a second before the frame was asked on the drawing thread). Met on a monitor of 120 a second; not measured on one of 60 |
| Pictures waiting to be shown | Never more than one | By construction |
| Memory with 64 preview tiles | Under 300 MB committed | 119 MB, without the interface. M2, in the program's own window: 376 to 380 MB with pictures of twice the width they are drawn at, as the page asks for them; 291 MB with pictures of the width drawn. Not met as it is |
| A wall of 64 JetKVM cards (M6 2i) | As 64 preview tiles: under 5 % of the processor, under 300 MB committed, no interface frame over 16.7 ms | Against the fake (2026-10-31, part 10, on the PC's slower cores, every card receiving its JetKVM's whole 1080p60 stream): 1.4 to 1.9 %, 477 to 511 MB, no frame over 16.7 ms, but the interface thread later than 16.7 ms 13 to 16 times in 30 seconds, every four seconds at the wall's status rounds, the latest 51 to 62 ms; 16 cards 1.2 to 1.4 %, 364 to 370 MB, none late. Against the fake: the processor met, memory not, the interface thread not quite. 2026-11-15 (PERF, the release build on the slower cores, three runs of 30 s each, the program's own JetKVMs by hand; before at d2562193, beside 0 to 2 other builds: 1.8 to 2.2 %, 456 to 502 MB, the card's own memory 128 to 171 MB, a frame's work 5.0 to 5.6 ms on the mean, the interface thread late 11 to 15 times, the latest 61.8 to 65.8 ms, every 4 s): with the status round's looks four a frame (PERF-1) and each card's still kept at the size drawn (PERF-2), with no other build beside them, 1.6 to 1.7 %, 362 to 366 MB (a paused wall 343, before 427), the card's own memory 69 to 70 MB, a frame's work 1.8 ms on the mean and never over 8.1, the interface thread late 0, 0 and 2 times (the latest 9.6 and 15.4 ms, and the two 55 and 72 ms while every text field asked the clipboard, which had changed); PERF-1 alone, four runs: late 0, 0, 7 (beside other builds, its frames late too) and 1, the latest 6.9 to 29.3 ms. The processor and the frames met; the interface thread met on a quiet PC; memory not yet (the rest is under the table). 2026-11-16, with two card stills decoded at once (three runs at a picture of each card a second, three at one every 500 ms, the wall's fastest): 350 to 368 MB, a frame's work 0.9 to 1.0 ms on the mean, the interface thread late 0 times in the three at a second (the latest 7.2 to 9.5 ms) and 0, 0 and 2 times in the three at 500 ms (the latest 10.6, 11.1 and 44.4 ms); 343 to 370 MB in the 23 runs of the day that drew, whichever of the limits tried, the floor and where it goes under the table. All of these on the PC's slower cores beside other builds. Measured quietly at 7ff2047e (2026-11-16, three runs of 30 s each, nothing else running): on this PC (its fast cores, the monitor at 120 a second) 365 to 386 MB, 0.69 to 0.85 %, a frame's work 0.53 to 0.60 ms on the mean, the interface thread never later than 16.7 ms (the latest 4.0 to 11.8 ms); on a second, faster PC whose display is 1920 x 1080 at 60 a second, where the rig's window is 1280 x 800 pixels, before the perf work (9acc19cf) 483 to 493 MB and a frame's work 3.7 to 3.9 ms, after it 315 to 330 MB and 0.39 to 0.42 ms, a paused wall 371 to 396 MB before and 299 to 322 after. The processor, the frames and the interface thread met; memory not: 300 MB is missed by 15 to 30 MB there and by 65 to 86 MB here, the stills being kept at the size drawn in the monitor's pixels. A JetKVM not yet |
| A JetKVM shown large: frame arrived to present returned | As the controlled machine's above | Against the fake (part 10, 1080p60, the monitor at 120 a second): on the card 1.2 to 1.3 ms mean, 99.1 to 99.9 in 100 within 4 ms; in software 2.2 to 2.5 ms; 0 to 15 of about 1,800 made way for a newer one where the fake sent two within one refresh; beside 63 other JetKVM cards 2.3 to 2.7 ms and 31 to 36 made way. Against the fake: met alone and beside 16 but for every picture shown; not beside 63. Quietly at 7ff2047e (2026-11-16, this PC's fast cores, the monitor at 120 a second, the program's own JetKVMs by hand, runs of 30 s): alone 0.7 ms on the mean, 99.5 to 99.8 in 100 within 4 ms, 0, 0 and 2 of about 1,883 not shown; beside 63 cards 0.9 and 1.0 ms, 99.5 and 99.7 in 100 within 4 ms, 0 and 0 of 1,937 not shown (a third run, whose window lost the front 19 times, 1.6 ms and 5 not shown, left out). Met alone and beside 63, but for 2 pictures in one run alone. A JetKVM not yet |
| A JetKVM's first picture (M6 2i) | None set; plan 4 (M1) expects a window on a card's connection to have one in about 0.5 s and a decode | Against the fake (part 10): a card's first key frame 0.23 to 0.27 s after it is asked for, and 4 to 43 ms after the rest of a JetKVM that rested 5.5 or 15 s; a window on the card's connection 0.23 to 0.54 s (the film's next key frame). Against the web's fake (aiortc): 2.1 to 2.2 s, aiortc's own connecting; "Show here" 2.1 s. A JetKVM not yet |
| Live tiles decoded in software | Up to 64 at 720p15, up to 16 at 1080p30; beyond that tiles stay previews | 64 at 720p15 for 9 % of the processor; 1080p30 falls short from 36 tiles |
| Live tiles decoded on the card | As many as its memory carries, at about 37 MB a 720p stream and 75 MB a 1080p one, and never more than its driver is known to bear | 64 at 1080p30 on the fastest card, with its decoder full; one maker's driver hangs from 32 |
| Three days (later thirty) | Handles, memory and graphics memory flat after the first hour | Not yet |
| Graphics device lost | Wall back within 5 seconds, by itself | Told of the loss: previews within a second, 16 and 36 live tiles within 2, 64 at 1080p30 within 6; a real loss not yet |
| Server lost and back | Tiles live again within one picture interval of the server answering | M2: seen to carry on by itself, not timed |
| Installer | Under 80 MB | Not yet |

Where the memory of a wall of 64 JetKVM cards goes (PERF-2, 2026-11-15; `tools\measure_live.ps1 -JetKvm -Cards 64
-Wall`, whose report says what the committed memory is made of as the window is shown, when the wall has its cards
and after the measuring; the release build at d2562193 on the PC's slower cores, beside 0 to 2 other builds, three
runs of 30 seconds and a paused wall's one, against the program's own JetKVMs by hand). 456 to 502 MB committed, 476
to 596 as the measuring began; of it the program's own 391 to 434 MB (the rest is the writable part of its
libraries), against 114 to 117 MB as the window is shown, of which 34 MB in use in its heaps, 69 to 71 MB outside a
heap and 2.9 MB of 39 threads' stacks.

* (a) Each card's H.264 decoder and its frame pool between key frames: none is kept (a key frame's decoder is made
  for it and freed, M6 2c). The heaps' memory committed beyond what is in use: 9 MB as the window is shown, 61 MB on
  a paused wall (each card's first picture decoded, none after it), 97 to 115 MB with each card's picture decoded
  once a second, on up to eleven of the twelve decoding threads at once. (Taken then for what the decoders' making
  and freeing leaves. With fewer decoded at once, below, the heaps kept 69 to 95 MB beyond what is in use, the least
  with two: how much of it is the decoders' was not shown.)
* (b) Each card's still at its stream's size, 1920 x 1080: 3.1 MB a card on the graphics card (128 to 171 MB of its
  own memory), and in this program's memory, outside every heap, four or five allocations of 20 to 32 MB that come
  with the cards' pictures (127 to 135 MB; tried once with every still a quarter of its size: 58 MB, and 70 MB on
  the card). What waits to be drawn is one picture, 3.1 MB, until the next frame.
* (c) Each card's WebRTC connection and driver: 95 threads more than the window had (134), 5.6 MB of their stacks;
  libdatachannel keeps one pool of threads for all connections, as many as the PC has processors, and one thread for
  all their ICE, by its own options already. The heaps' memory in use 51 to 68 MB more than the window's (0.8 to 1.1
  MB a card, each card's own screen with it: some 200,000 small pieces more); about 2,950 handles.

With each still kept at the size its card draws it (PERF-2's first cut, f513c8e6), 362 to 366 MB committed, of which
the program's own 295 to 299: outside a heap 102 to 118 (the driver's allocations three or four of 18 to 23 MB, 58 to
61 MB, where four or five of 20 to 32 MB were; and sixteen of 4 MB, ten of them there as the window is shown), the
heaps 168 to 188 of which 86 to 87 in use, stacks 8.4. What is left over the 300 is mostly the heaps' 82 to 101 MB
committed beyond what is in use (56 on a paused wall; 441 to 463 MB as the measuring began, 362 to 366 at its end).
Tried and of no use: Windows' segment heap for the whole program (its manifest), 368 and 376 MB.

2026-11-16 (PERF, after the review's fixes; the release build on the slower cores, runs of 30 s with a picture of
each card a second unless said, beside 0 to 9 other builds, the load of each run in the builder's notes), what was
left to try without changing the decoding pool or anything on the path of a machine shown large:

* Fewer card stills decoded at once (the stills' line, Gpu::Options::stillsAtOnce: a card's key frame waits in line
  where it is handed in, and no other stream does). Committed at the end, of the 23 runs of 64 cards that drew: with
  no limit 358.7 and 358.8 MB at a picture a second (436.6 and 430.1 as the measuring began), up to eleven at once,
  and 359.7 to 362.2 at one every 500 ms, the wall's fastest (three runs); with two 350.5 to 359.9 (430.8 to 436.0;
  three runs) and 351.0 to 367.5 at 500 ms (three); with four 354.9 to 370.3 (six), and 343.3 to 364.1 with the heaps
  compacted as the measuring began (three); with six 356.0 to 357.2 (three). The totals of every limit lie within one
  spread. What the heaps kept beyond what is in use, at the end: with two 69.3 to 75.6 MB (six runs, at a second and
  at 500 ms), with no limit 79.1 to 86.3 (five), with four 69.7 to 94.7 (nine, three of them compacted), with six
  79.8 to 87.9 (three). Every run with two kept less than every run with no limit, by 3.5 to 17.0 MB, which the
  totals do not show (they spread by more: outside a heap 105.3 to 130.8 MB, below); why, and how much of it is the
  decoders', was not shown. What the limit changes most is the drawing: a frame's work 0.9 to 1.0 ms on the mean and
  99 in 100 within 1.5 to 1.7 ms with two (six runs), against 1.8 to 2.1 and 5.1 to 8.1 ms with no limit (five); no
  still waited longer than 99 ms for its turn (69 to 99 at a second, 70 to 77 at 500 ms) and none was taken the place
  of while it waited. The program decodes two at once, in each window: each window has its own device, and its own
  line (wall_windows.cpp). A JetKVM shown large beside 63 cards, in the rig's one window, which then asks no card's
  picture, so that its line is idle (where another window shows the wall, the cards' pictures go to every window's
  streams, and the window that shows the machine large decodes its own hidden cards' stills through its own line):
  three pairs of runs, two at once and no limit in turn, beside 0 to 4 other builds, on the card 1.9 to 2.0 ms on the
  mean against 1.8 to 1.9, 97.3 to 98.3 in 100 within 4 ms against 98.2 to 98.6, 5, 0 and 0 of about 1,900 pictures
  not shown against 0, 0 and 0. Before the pairs: with no limit 1, 1 and 0 not shown, with four at once 1, 4 and 5;
  and three runs of two at once beside 2, 15 and 9 other builds, 4, 68 and 56 of 1,916, 1,780 and 1,180 not shown,
  1.8, 3.4 and 24.1 ms on the mean, left out for their load, which is why the pairs were run.
* The heaps compacted once the cards came (_heapmin and HeapCompact on every heap of the program's; three runs, four
  at once): 0 to 1.5 MB given back (433 before and after, 444 and 442, 412 and 412), 343 to 364 MB at the end.
* libdatachannel's threads, by the names it gives them, in every report of 64 cards: 24 "RTC worker" (its pool, as
  many as the PC has processors, rtcSetThreadPoolSize), one "juice poll" (its ICE), and two more begun in it without
  a name: 27, some 1.7 MB of stacks at what a thread had on the mean (8.3 to 8.8 MB of stacks for 134 to 149
  threads). Its option is left as it is. (Of those threads, 64 are Qt's "QNetworkAccessManager thread", one for each
  KVM's network manager; 20 to 35 the graphics driver's; 12 decoding.)

The floor of a wall of 64 JetKVM cards, then, so far, as the 23 runs of 2026-11-16 that drew measured it: 343.3 to
370.3 MB committed at the end. Of it the program's own 275.8 to 303.1 MB: the heaps 155.4 to 181.2, of which 85.8 to
97.5 in use (86 in all but one run: about 52 MB more than as the window is shown, 0.8 MB a card) and 69.3 to 94.7
kept beyond that (the pieces of up to 4 KB in use 496,595 to 511,474 when the wall had its cards, 376,267 to 390,155
at the end);
outside a heap and a stack 105.3 to 130.8 (what the rig calls the graphics driver's or a library's own: sixteen
allocations of about 4 MB in all but one run, which had fifteen, ten of them there as the window is shown; three of
15 to 25 MB and one of 8); the threads' stacks 8.3 to 8.8. The rest, 66.5 to 67.6 MB, the writable part of its
libraries. As the measuring began, 56.1 to 90.2 MB more were committed than at its end, given back within the 30 s
(compacting the heaps gave back 0 to 1.5 of it). Under 300 would take less of what the heaps keep beyond what is in
use, or less of the memory outside them: neither is on the stills' path, what keeps either was not found, and nothing
of it was tried.

## 10. Not in it

* Nothing of the server's inside it: no Python, no engine beside the program, no local
  server. The program is one process that speaks to the devices itself. (Until
  2026-10-11 this line said the opposite: no signing in to a device, no device password
  kept. The owner turned it.)
* No installer that brings a QKVM server along. Whoever wants the server installs the
  server.
* No web view. Where the page does something better, the page is opened in the browser.
* No part of the program fetched from the net after it is installed, but a whole update
  that is checked (section 8).
* No account of its own, no cloud, no telemetry.
* No settings of the server, no accounts, no activity log, until after M4. (The wall as
  a file is the one thing of an admin's that is in it: section 4, M2.)
* No sound, no file transfer: later, if ever. (Virtual media are in it, as the web has them in 0.3.0: a server's
  machines' through the server since 0.1.0, and from 0.2.0 this PC's own kvmd KVMs' and JetKVMs', spoken to by the
  program itself, an image uploaded from this PC to the KVM's own storage among them: section 6.3, and M6 for a
  JetKVM. Nothing is fetched from an address. Until 2026-11-13 this line said "no virtual media: later, if ever"; the
  owner put this PC's own in 0.2.0's first wave, and a JetKVM's in its second. A server's own through its controller
  is not in it yet.)

## 11. What the program stores, and the upgrade rule

The owner, 2026-11-14: "the upgrade should be seamless: everything stays the same, the
settings, preferences, username, password". So every place the program keeps something of
a person's is listed here, with its format and the code that reads and writes it. Nothing
else is kept. A name or format that is not here is not one the program may lose.

### 11.1 The rule, from 0.1.0 on

* **No clean cuts in user data.** The rule for unreleased work ("no legacy code in
  development") ended with the first published version, 0.1.0. A change to any file, key,
  credential name or record format below comes with a migration that is versioned (the
  old version is read, and written in the new form only after it was read whole), and with
  a fixture test from every published version (`ui/tests/fixtures/profiles/`, a person's
  and a wall screen's profile of each version, with the Credential Manager records that
  version's own Windows code made of its secrets; `test_ui_upgrade_continuity` and
  `test_ui_credential_shapes`, which fail where a name, a format or a record's shape
  changed without one; and the VM step `tools/test_upgrade_keeps_vm.ps1`, whose writer is
  built at the version's own commit). A change to how the program keeps a credential (its
  type, persistence, target name or the filter it lists by) is a format change too.
* **A rename is a migration, never a drop.** A key, a credential name or a sibling file
  that is to be called otherwise is read under its old name, written under the new, and
  the old one is taken away only once the new one is written.
* **Additions only, in settings.json.** A key that a version adds is new; one it no longer
  needs is still read and written as it was until a migration takes it out. (An older
  version that writes the file drops the keys it does not know, but for the KVMs of a
  family it does not speak to, which it keeps as they stand where it can. 0.1.0 to 0.1.2
  keep such an entry only where its id has the 8-hex shape and its address is an http(s)
  address exactly as `kvm::addressFrom` writes it: any other, a VNC server's (`vnc://`,
  `vncs://`, from 0.2.0) among them, they drop when they write the file. Only 0.1.2 keeps a
  copy of the settings byte for byte before it writes anything (`settings.json.<writer>.bak`,
  below), which holds it until three later changes of version have made three newer copies of
  that kind; 0.1.0 and 0.1.1 keep none, and what they drop is gone. Its Credential Manager
  record stays, used for nothing then. From
  0.2.0 on, an entry of a family the version does not speak to is kept as it stands
  whatever its address, or without one, where its id has the 8-hex shape and its compact
  JSON is 64 KiB at the most (`core/src/settings.cpp`, `kvmFrom`); and so is an entry of a
  family it speaks to that it cannot read as one of that family (an address that is none of
  the family's, as 0.1.0's and 0.1.1's own example of a later family, a `vnc` at an
  `https://` address; a mark of its machine of no driver's shape), never dropped. So going
  back to an older version keeps what that version knew, and nothing more; the setup
  refuses an older version over a newer one, and the program's own update only goes
  forward.)
* **Every release adds its fixture.** Before a version is published, its own profile is
  made by `qkvm-test-make-profile` built at the release's commit, and committed beside the
  others (the fixtures' README says how).
* **The app lock's recovery rule stays.** Secrets without a readable app-password record
  go to "Reset saved secrets" (`ui/app_lock.cpp`): deleting the record must not open the
  lock. So the record is the one thing an upgrade must never fail to read, and the fixtures
  prove that it does not.
* **A way back before anything is reset or changed** (from 0.1.2). At a start the version
  that left the settings as they are is found: the version recorded, where they were not
  written since it recorded them (their time: its own start found them so; from 0.2.0, so
  that a version started again before it wrote them keeps no second copy); else the version
  they say wrote them (`written_by`, from 0.1.2 on, which a version before 0.1.2 drops when
  it writes them); else one from before 0.1.2. Where that is not this version (newer, older, or one run in between that
  keeps no record), the settings are kept as they were found (`settings.json.<version>.bak`,
  below) before anything of them is written, and only then is this version recorded. Where
  that copy cannot be made, the person is told once, at that start, in plain words, and the
  settings owe it (`Settings::owesACopy`): the Settings that writes them keeps it before it
  writes anything of them, records this version once it is kept, and writes nothing while it
  cannot be kept. So no writing marks the change of version done without its copy, and the
  settings stay as the version before left them, which makes the next start owe it still
  (settings that another program held while the start looked are copied once they are
  free). Before "Reset saved secrets" erases
  anything, the settings are kept as a copy of their own kind, and the box says what will be
  erased, why (a profile made by a test build from before the app password existed is said
  to be one; a record of a newer version is said to be one), and the very file the copy goes
  to. Where that copy cannot be made, nothing is erased and it is said; chosen again, the
  reset goes on (the settings themselves stay), so nobody is kept out of a reset for good.
  Each kind keeps its newest three; a full count never keeps a copy from being made. A format
  change that comes later keeps its copy first too. No secret goes into a copy: the settings
  hold none.

### 11.2 `%LOCALAPPDATA%\QKVM Desktop`

Qt's `AppLocalDataLocation` for the application name "QKVM Desktop" (`ui/application.cpp`,
`prepareApplication`: the internal name stays, only the displayed one is "qKVM"). The file
names beside the settings are made from the settings' own (`core::Settings::usualFile()`,
then `ui/main.cpp`). None of these holds a secret.

| File | What it is, and its format | Read and written by |
|---|---|---|
| `settings.json` | One JSON object, no version field; from 0.1.2 `written_by`, the version that wrote it. `server` (the last server's address as `ServerClient` writes it, with its path), `servers` (those whose sign-in was kept), `favorites`, `recent_machines`, `picture_scales` (each `{scope, machine[, scale]}`, scope `pc` or `server-` and a SHA-256), `reached_encrypted` (host names), `certificates` (`"host:port": sha256 hex`), `theme`, `stay_in_tray`, `tell_news`, `release_key`, `hardware_decoding`, `graphics_card`, `check_updates`, `last_update_check` (ISO, UTC), `own_wall`, `picture_every_ms`, `settings_section`, `windows` (each `{source, place {x, y, width, height, maximized, full_screen}, group, order, columns, rotate}`), `workspaces` (each `{id, name, windows}`), `kvms` (each `{id, kind, name, address, user, group, known_by_certificate, certificate, asked, number, machine}`, and where set `no_signal_minutes`, `unchanged_minutes`, `certificate_pem`, `secret_kept`, `kept_beside_twin`, `followed_onto_https`, `held_for_twin`, and of a `redfish` one from 0.2.0 `server_place` (`{place, among_several}`: where its controller keeps the card's computer, the place it was last found at by its number as a controller names one, `/redfish/v1/Systems/Self`, or none, and whether that controller was ever seen to list more than one computer, or its card was told No (section 8); noted by the wall as its driver finds them, and kept with the mark, section 8; one in any other shape is read as one of several, with no place, so that no computer that gives no number passes for the card's, and the card is on the wall all the same; a file of the wall does not carry it; 0.1.2, which writes of a card the keys it reads, drops it at its next writing, and the card is then as one from before: `test_core_settings`, `test_ui_upgrade_continuity`, the way back too, and from 0.2.0's fixture on); `kind` `kvmd`, `jetkvm`, `redfish`, or from 0.2.0 `vnc`, a VNC server, whose `address` is `vnc://host[:port]` or `vncs://host[:port]` as `vnc::addressFrom` writes it (its host as a KVM's: small letters of the twenty-six, digits, dots, hyphens and the underscore, or numbers the one way, IPv6 in brackets; no port where it is 5900), whose `known_by_certificate`, `certificate` and `certificate_pem` are a `vncs://` one's as an HTTPS KVM's, and whose `user` is kept as written and not used; or from 0.2.0 `proxmox`, a Proxmox VE guest, with `node` and `vmid` too, whose `address` is its Proxmox's API, HTTPS as `kvm::addressFrom` writes it (8006 kept), so several guests share one address, whose `user` is the API token's ID, and whose `machine` is the number it gives for itself (`proxmox:` and 32 hex of its SMBIOS UUID's SHA-256, rule 30), and where its driver replaced its boot order to start it from an image and has not put it back yet `boot_order_was` (`{order, written, vmid, machine}`: the order before and the one written, each as a guest's settings write one after `order=`, the guest's number and the number it gave; noted by the wall and written down at once; one in any other shape keeps the entry as it stands, unread: `test_core_local_wall_proxmox`, and `test_ui_upgrade_continuity` from 0.2.0's fixture on): an entry the published 0.1.2 keeps as it stands, whose rule is an id of eight hex and an address as `kvm::addressFrom` writes it, its family one it does not speak to (`test_core_local_wall_proxmox`); a KVM of a family this build does not speak to is kept as it stands, from 0.2.0 whatever its address, 64 KiB at the most, and from 0.2.0 so is one of a family it speaks to that it cannot read as one of it (an address that is none of that family's, a `machine` of no driver's shape, a `proxmox` entry's `node`, `vmid`, address in the clear or `boot_order_was` of no shape the program writes)), `actions` (each `{id, name, kind, keys, enter, danger, changed, writing}`). Written whole, through a new file that takes the old one's place | `core/src/settings.cpp` (`readFrom`, `Settings::remember`), for `core/src/local_wall.cpp`, `own_actions.cpp`, `updates.cpp`, `ui/server_source.cpp`, `server_session.cpp`, `settings_view.cpp`, `look.cpp`, `wall_windows.cpp`, `workspaces_view.cpp`, `quick_access.cpp`, `wall_pace.cpp`; read also by `ui/app_lock.cpp` (a wall in it is a profile that was used) |
| `settings.unreadable.json`, `settings.unreadable-N.json` (N up to 20) | A settings file that could not be read when the program started, moved aside before anything is written | `Settings::putAsideWhatCouldNotBeRead`; read by `ui/app_lock.cpp` as above |
| `settings.json.sign-ins` | The wrong-password counts of every sign-in (a9). `{"version": 1, "nonce", "sealed"}`, the sealed part AES-256-GCM under the key `sign-in-pacing` (below) for "qkvm sign-in pacing 1", inside `{"kept": [{digest, road, address, user, noes, silent, until}]}` (`until` in milliseconds on the PC's clock). One that does not open is moved to `.unreadable` (`.unreadable-N`, N up to 10) | `core/src/pacing_file.cpp` |
| `settings.json.locked` | The app lock's second record where Windows would not write the first: one line, `locked`, `screen`, `reset` or `setup` | `ui/app_lock.cpp` (`latch`, `unlatch`, `load`) |
| `settings.json.incidents` | The alerts' history. `{"version": 1, "watches", "entries", "pauses"}`; one that cannot be read is kept as `.damaged-<uuid>` when it is repaired | `core/src/incidents.cpp`; read also by `ui/app_lock.cpp` (history is a profile that was used) |
| `settings.json.setup-overrides` | The servers whose BIOS setup boot override may still be set. `{"version": 1, "servers": [{address, machine, since, doubtUntil, queued (true where the controller only queued a press's change, from 0.1.2)}]}`. From 0.2.0, one that is there and cannot be opened is tried as the settings are (40 times, 50 ms apart), and one that still cannot be read whole (held, larger than 256 KiB, no JSON, another `version`, `servers` no list, a record that is none, more than 256 records) is never written over: it is moved to `.unreadable` (`.unreadable-N`, N up to 10), byte for byte, before anything is written, and where it cannot be moved nothing is written (no start into BIOS setup is sent then) | `core/src/setup_overrides.cpp` |
| `alert-pictures\` | The screen watches' pictures, `<kvm id>-<stamp>.jpg`, and their state, `screens.json` | `core/src/screen_watch.cpp` |
| `updates\setup-<version>.log`, `updates\setup-handover.json` | A Setup's log, and the note of a hand-over to Setup that is told at the next start: `{schema 1, from, to, process, created, temp, at}`, and from 0.2.0 `machine` (`x64` or `arm64`, letter for letter: the kind of Windows the release handed over is for). A note without `machine` (every one of 0.1.x) is read by its version alone, as 0.1.x read it; one with any other `machine` does not read, and is let go (`test_core_updates`; `test_ui_upgrade_continuity` reads each published version's). 0.1.2 reads a note of 0.2.0 by its version, and passes `machine` by | `core/src/updates.cpp` |
| `settings.json.running` (from 0.2.0) | Empty. Its lock (Windows' `LockFileEx`, the first byte, exclusive) is held by the one copy of the program that writes this profile, in whatever Windows session of the account it runs, until it ends; the uninstall holds it too while it works. Made at a start where it is not there, never written; a version before 0.2.0 passes it by. An uninstall that finds it not there (no copy of 0.2.0 or later has run for the account) makes it, and its folder where that is not there, holds its lock, and removes both as it ends (outside review of 0.2.0, 1.3) | `ui/win/profile_lock.cpp` (`ProfileLock`, in `ui/main.cpp` after the data folder is looked at), `core/src/profile.cpp` (`runningFile`); `packaging/qkvm-desktop.iss` (`ProfileInUse`, opens it, or makes it where it is not there; `DeinitializeUninstall` removes what it made) |
| `settings.json.version` (from 0.1.2) | The version that last started with this profile, the one before it, and the settings' time as that start left them: `{"version", "previous", "settings_time"}` (`previous` empty where none was recorded; `settings_time` in milliseconds since 1970, UTC) | `core/src/profile.cpp` (`recordStart`, `recorded`), at every start in `ui/main.cpp` before the settings are read |
| `settings.json.<version>.bak`, `settings.json.before-<version>.bak`, `settings.json.reset-<version>.bak`, each also `-2` to `-999`, and past those by the time of the copy (from 0.1.2) | The way back, in two kinds kept apart, each to its newest three: the settings byte for byte as they were found at a start of another version (named by the version that wrote them; `before-` and the version that found them where that was one before 0.1.2), and as they were before a "Reset saved secrets" (`reset-` and the version that reset). Never written over; no secret is in them | `core/src/profile.cpp` (`keepCopy`), for `recordStart` and `ui/app_lock.cpp` (`resetSecrets`); read by `ui/app_lock.cpp` (a wall in one is a profile that was used) |

What a server's card meets where it keeps no `server_place` (the row of `settings.json` above). Every card of 0.1.0 to
0.1.2 comes so, every card after a way back to 0.1.2 (which drops it) and up again, and every card taken from a file of
the wall (which does not carry it); a card known by its computer's number takes its place at the first read that gives
that number. Until then, by the rule of section 8 (the owner's decision of 2026-11-19: such cards stay strict), a card known
by its computer's number is refused while that computer gives none (a board that gives none while it is off: Power on
is refused too), until the server, switched on another way, gives its number, or the card is changed with its password
typed; and a card known by its controller alone (its computer gave none when it was added, as 0.1.x added such a card)
is refused whatever its computer says, until it is changed with its password typed. Nothing is done to the server, and
its card says why and how past it (`RedfishDevice::whereItsComputerIsNotKnownYet`, `whereItWasTakenIsNotKnown`;
`test_core_redfish_which_server` w7 and w8, `test_core_wall_servers`). Nothing of the card is lost: its numbers, its
password and its name stay. The CHANGELOG says it to the person among 0.2.0's known limits.

### 11.3 `%LOCALAPPDATA%\qKVM\Cache`

A cache: anything in it may go, and nothing of the person's is in it. A packaged copy has it
under `%LOCALAPPDATA%\Packages\<family>\LocalCache\qKVM` instead (`ui/win/cache_paths.cpp`).
`Downloads\` (`update-XXXXXX\` with a downloaded package, `setup-temp-XXXXXX\`, pruned after
a week: `core/src/update_cache.cpp`, `updates.cpp`), `Helpers\` (`qkvm-setup-XXXXXX\`:
`ui/installer_launcher.cpp`), `Installer\` (`setup.lock`, `history.json`:
`ui/installer/main.cpp`), `Diagnostics\` (`video.jsonl` and `video.jsonl.1`:
`core/src/video_diagnostics.cpp`; `{uuid}.crash.txt`: `ui/win/crash_report.cpp`). And the
program's own Temp, `%LOCALAPPDATA%\Temp`, after a Setup started it (`ui/win/own_temp.cpp`).

Qt's own caches, under the program's data folder (`QStandardPaths::CacheLocation`:
`%LOCALAPPDATA%\QKVM Desktop\cache`), are caches as well, and hold nothing of a person's:
`qmlcache\*.qmlc` is what Qt's QML engine compiles of QML that it reads from a file on disk,
and this build reads none so (its own QML is compiled into the program by qmlcachegen, Qt's
into Qt's plugins); `qtpipelinecache-<architecture>\qqpc_<backend>` is Qt Quick's automatic
pipeline cache, on by Qt's default, which Qt 6.12 with Direct3D 11 left unwritten when it was
looked for (2026-11-14, by Qt's own QML runtime and a scratch window: it saves only what it
compiles while it runs, and Qt Quick's shaders come compiled). Either may appear with another
Qt or backend; neither is read as anything of the person's, and either may go.

### 11.4 Windows Credential Manager

Generic credentials of this PC only (`CRED_PERSIST_LOCAL_MACHINE`), each named
`QKVM Desktop: ` and the name below (`core/src/windows_secrets.cpp`); 2560 bytes at the
most each.

| Name | Record | Read and written by |
|---|---|---|
| `app password` | `00 'Q' 'K' 'V' 'M' 01`, the length of the JSON in 4 bytes (big-endian), the JSON (compact), then one or two zero bytes so that the whole is odd. JSON `{"v": 1, "salt" (16 bytes, base64), "verifier" (32 bytes of scrypt N=131072 r=8 p=1, base64), "automatic", "locked", "screen", "failures", "next", "last"}`; during a reset `{"v": 1, "reset": true}` | `ui/app_lock.cpp` (`recordBytes`, `readRecord`, `load`, `persist`) |
| `app password established` | `1`: the app password was set once on this PC | `ui/app_lock.cpp` |
| `sign-in-pacing` | 32 random bytes: the key `settings.json.sign-ins` is sealed under. Made at a first start before the lock looks; not a sign of a profile that was used | `core/src/pacing_file.cpp`; `ui/app_lock.cpp` lets it pass |
| `kvm:<id>` | A KVM's password (a VNC server's too, from 0.2.0, its address `vnc://` or `vncs://`; of a Proxmox guest, from 0.2.0, its API token's secret, saved for its Proxmox's address and certificate, one record for each guest, whatever they share): `QKVM-KVM-2`, a line feed, its address (as written, `%`-encoded beyond letters, digits and `.-_:/[]`), a line feed, `none` or `sha256:` and the hex of its certificate's SHA-256, a line feed, then the password in UTF-8. Read only where the lines are the KVM's as it is now. From 0.2.0, while a change takes the password to another address or certificate, two such records in one: `QKVM-KVM-2+`, a line feed, the length of the first record in decimal digits (no nought first), a line feed, the record for the KVM as it was, then the one for it as it is to be; the one whose lines are the KVM's is read, and each of the two must begin as a record does (else none is). Written before the settings and replaced by the second alone after them, so a change cut off anywhere leaves the card its password (the outside review of the wave 2 desktop, 6.1). Where that replacing is cut off (the program ended before it, or Windows did not take it), the record of two stands until the next start, which replaces it, before anything reads it, by the record whose lines are the KVM's as the settings have it, alone (one that holds none for the KVM as it is stays as it is, used for nothing; one Windows does not take then stays for the start after; a JetKVM held back for its twin is read nothing of while that lasts, section 8, and is made one at the first start after, or by its next change). 0.1.x reads it as no password (the card asks for it) and erases nothing of it: with nothing done to that card there, the password is back up again on 0.2.0. Lost only where 0.1.x is started after such a cut before 0.2.0 made it one (at a start where it is not held back for its twin: review cx6-2, L5), and a yes to another certificate of that card is said there: 0.1.x takes the card on without its password and drops its mark, and 0.2.0 then has none for it (the card asks for it to be typed). `test_ui_upgrade_continuity` holds it, by a fixture with such a record (review cx6-1, M2) | `core/src/local_wall.cpp` (`savedFor`, `KeptAs`, `KeptTogether`, `recordsIn`, `recordFor`, `passwordIn`, `makeOne`) |
| `action:<id>:<a or b><part>` | A saved action's typed text, in parts of 2500 bytes: `QKVM-ACTION-1`, the action's id, `<writing mark> <part>/<parts>`, each line ended by a line feed, then the part's bytes. The settings name which writing is the action's (`writing`) | `core/src/own_actions.cpp` |
| `session at <server address>` | The server's session (its `qkvm_session` cookie), where "Stay signed in" was ticked | `ui/server_source.cpp`; read also by `ui/server_session.cpp`; erased by `ui/wall_screen.cpp` |
| `display link at <server address>` | A display link's key | `ui/server_source.cpp`; erased by `ui/wall_screen.cpp` |
| `wall screen` | The servers this PC is a wall screen of, one address to a line | `ui/wall_screen.cpp` |

### 11.5 Elsewhere

* Starting with Windows: the value `QKVM Desktop` under
  `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` (the program's file in quotes), and
  what Windows notes of it under `...\Explorer\StartupApproved\Run`; for a packaged copy the
  startup task `qKVMStartup` (`core/src/windows_auto_start.cpp`). The setup installs into
  the same folder on an upgrade, so the value still names the program.
