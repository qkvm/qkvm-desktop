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
  of the notification area too, and ends. And while nobody can see the wall (every
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

Done when: a program that was locked, or is a wall screen, does nothing to any machine
until the password is given, also after it was ended and started again, also where it
signs in by itself; a sealed export is not written without it; and a reset leaves no
secret of the program's behind.

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
action sent to a machine is unanswered, a server's start into BIOS setup is under way or its
override may still be set, or a box holds changes that are not saved; it says which. The program
verifies the setup again, through a handle that lets nothing change, rename or remove it
until the program ends, and starts it from that very file, which lies alone in a folder of
the program's own update cache that only the person (and Windows' own administrators) may
change. Setup is told only the folder the program runs from and where to write its log.
The program then closes its windows and ends: its goodbyes to devices are given their time,
and it ends within 9 seconds after them, and 50 seconds in any case; Setup waits for that,
installs into the same folder, and starts qKVM again. The next start says "Updated to"
the new version, or that the update was not installed, with the place of Setup's log, and
offers it again; where Setup found qKVM still running, it says so. Setup unpacks itself in a
folder of the program's cache that only the person may change, not in a shared Temp. While
Setup is at work, qKVM started by hand ends at once, without a word or a window; only
Setup's own start at its end waits for it to finish, and then starts qKVM anew as the person
would. Data that is there but cannot be read is never taken for none: qKVM says so, and
where, and ends without writing anything. A downgrade or the same
version is never offered. A portable copy's update is still only shown, verified, and
never run; an MSIX copy's still goes to qKVM Setup.

Done when: on a clean Windows 10 and a clean Windows 11, install, update and uninstall
work without administrator rights, and uninstall leaves nothing but the settings.

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
* A server is known by the numbers its computer and its controller give for themselves,
  kept with its card as digests (the web's `machine`), and never by its certificate: its
  maker may have put one certificate, and one key, on every board. So its kept password goes
  to its own address and to no other, whatever certificate shows there; a second card of the
  same server, by its computer's number, is refused, and the web's "keep both" is not offered
  (the owner's word); two whose computers give no number are two. Another server at its
  address is said on its card and sent nothing; the change box takes the server that answers
  now only where its password is typed. A card that keeps no numbers yet takes the server
  that answers first, and keeps it.
* At the program's end every driver of the wall and of its boxes is let go of before the
  program waits for the controllers' goodbyes, so that each session is given back.
* From a wall's file (part 1b-ii): with the numbers its server is kept by (`machine`, as
  the web writes it; a file of 0.1.x, which kept them as what the server is, is read so),
  asked what it is before its password goes, and held to them; two of one certificate are
  two, and none is kept beside its twin. The wall's own file carries them.
* In the boxes and the lists, called what the page calls it: "Server (Redfish)".

The order of M4 to M7 is the owner's to change.

### Later, not designed yet

Tours (a tile that walks through machines). Recording. Links and command-line switches. A QKVM server's accounts
and settings inside the program. Windows on ARM. Linux.

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
| Add a KVM | Address, password, and where wanted a name and a group. What answered there, and what the KVM is known by. For a KVM that is on the wall: the same, to change it or to take it off. A password or a passphrase that is typed, here and in every other box, is hidden, and has an eye beside it that shows it until it is pressed again, the box is sent, or the box is left, as on the page. |
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
| Who it is | `GET /api/auth/check`, and where the KVM has them its model, version and name | |
| A picture | `GET /api/streamer/snapshot` | A JPEG, as captured or made smaller by the KVM, which costs the KVM time: small pictures are asked small, anything wider is asked as captured. A header says whether the computer sends a picture at all. A KVM whose video rests while nobody watches says so, and is woken |
| How it is | Its state calls: the video, the hardware, power where it is wired, the computers behind it | Whether the computer is on is GLKVM's own word (`atx.power`: on, sleep, off), whose power light stays off on a real Comet whatever the computer does; stock kvmd's light only where there is no word. A board that said it is there is not taken away by an answer that says it is not (a Comet's board says so to whoever asks while it answers somebody else) or by none, for a minute, and what it said of the computer is kept ten seconds (as the web app does). Where the line that keeps its video awake is held (kvmd's `/api/ws`, its line for events), the board's state is heard there (its `atx` event, at every change), and asked whether the line is there every five seconds in kvmd's own words (`ping`); a look reads the board there while the line was heard from within twelve seconds and says the board is there, and asks `/api/atx` otherwise: one asker less on a board that answers one at a time. No line is opened for the board alone. One maker's real KVM says nothing of its video while that rests: no size of the picture, and not whether the computer sends one. There what the last picture said stands in: its size as the KVM captured it, and its header for the signal; each only while the picture that said it is new, and never against what the state does say |
| Doing things | Keys, a text to type, the power buttons, the computer shown, a restart of the KVM | Each a call of its own; one at a time to a KVM. What got no answer may have been done, and is not sent again: only what certainly never left this PC is told as "not there". A power button: the board asked first, whatever its line said, and the press refused where either says it is busy (and whether the computer is on is what it says then); answered once the board has let go of it (half a second, six and a half for the long press), and a press that must change the computer's power once the board shows the change (twenty seconds at the most; else done, with what was seen); only a 4xx is a no; after an answer that says neither (none in eight seconds, one that broke off, a 5xx, the driver let go of meanwhile) the board is left alone for eight seconds and the button's hold, also by a driver made anew (section 8) |
| The live picture | Its stream of JPEGs (`GET /streamer/stream`) where it serves one; else its pictures as captured, one after the other; H.264 over Janus/WebRTC with reconnect and JPEG fallback, implemented against local peers in 2026-10-20; device acceptance pending | One stream to a KVM at a time, shared by every window that shows it live. A stream that ended is asked for again after a wait that grows, with the pictures asked for one after the other meanwhile; that a KVM serves none is remembered for a minute, and then asked anew. 404 is read as "serves none", and so is a yes (200) with something of another kind than a stream of pictures; kvmd's own 503 is read as a video that rests; a 502, 503 or 504 in anything but kvmd's own shape is read as "serves none" only where a picture of the KVM has come within the last moments, so that its video is awake, and otherwise as a video that rests (DESIGN.md section 4, and section 11 for what is open in it). Seen on a real KVM of one maker (2026-10-12): no stream of JPEGs there. At its stream's place, what stands before kvmd answers 502 with a page of its own while its video rests, which is read as a video that rests; and while its video is awake, 200 with something whose kind is not a stream's, which is read as "serves none": the stream is asked for once, and then the pictures one after the other. (That last reading is newer than the look at that KVM, and is tried against made-up KVMs: until it, such a yes was taken for a stream that broke, and the stream was asked for again and again for as long as the KVM was shown.) Nothing at `/api/media`, and a page of its own that shows the picture over WebRTC only. A picture as captured took it 25 to 45 ms, a small one 160 to 240 ms: the KVM shrinks it. Had live in the rig for ten seconds, by its pictures, without an error, 2560 by 1440 (pictures only: nothing was sent to its computer). Asked one after the other as fast as they come, it gave sixty pictures as captured in 1.2 seconds: about fifty a second over the local network, about 160 kB each, the slowest in 38 ms. Fifty-eight of the sixty were byte for byte the one before, its screen being still, and the live view hands on only pictures that changed. Not seen: a live picture of a screen that moves. A stock PiKVM, where the stream should be, was not tried |
| Keys and mouse | The KVM's WebSocket, `/api/ws`: one short message for each key, button and move, in bytes as kvmd's own page says them, or as a text of JSON for a KVM known to take only those. The pointer goes the way the KVM's mouse is set (`/api/hid`, `mouse.absolute`, read at every look and followed when it changes: places to an absolute one, steps to a relative one); the program never switches the KVM's `mouse_output`, a setting its other users share | The same real KVM let the line in and kept it (its own page falls back to this line where its WebRTC channel fails), and its own page says keys and the mouse in bytes. One key and one small move of the pointer were said to it so, by the owner's leave (2026-10-12). That they arrived at its computer was not seen: the picture showed no pointer. Apart from that, the owner has taken keyboard and mouse of a real KVM in the program's own window and reported that keys arrive; nothing else is known from that run. A KVM that signs the program in and refuses this line all the same is told as that (`not_allowed`), and is not signed in to anew with every asking (tried against a made-up KVM) |

Rules:

* Every answer of a KVM is bounded and checked as a server's is (section 8). A KVM is
  somebody else's software on the network.
* The program is one viewer of a KVM among others, a QKVM server perhaps among them. It
  asks no more of a KVM than one browser would, and never signs in twice where once
  does.

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
  certificate for it. This keeps a password from a machine that is no KVM at all. Not
  from somebody who answers as one: against that there is HTTPS and a certificate.
  The settings keep with a KVM over HTTPS that it was asked ("asked"), and beside its
  fingerprint the certificate itself that it is known by ("certificate_pem"), for the
  day the wall is written to a file. Neither is a secret.
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
    it stays held, for as long as that other one is on the wall: taken off, no KVM of the wall
    is known by that certificate any more, and a look anew would find no twin either. That is so
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
    A number that could not be read this once is no "none": the one kept stays. (Servers are
    otherwise: section M7.)
  * Never back from HTTPS to plain HTTP by itself. One over HTTPS that has not answered for 2
    minutes is asked the plain way, once a minute at most; where a JetKVM answers there, its
    card says that it answers only over plain HTTP now, as the web says it (its password would
    travel unencrypted there, or, with none, nothing would be checked), and to take it off the
    wall and add it again as `http://...` to use it so.
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
  (`SecretText`), the digest the pacing knows a sign-in by, and the body a sign-in sends (made once, as it goes, and held
  by nothing but what goes: the asker keeps it until Qt has let go of what it was handed, a turn after the reply went,
  and then writes it over). Out of reach: what Qt's network makes of a body on its own thread to send it (the copy it
  hands that thread, the request as it is written, the socket's and TLS's buffers), which Qt frees as it is; and what
  Windows' TLS keeps. A test looks at what the program's own thread gives back after real sign-ins of every road.
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
* **A server's controller is asked which it is on the line its password goes on.** Its
  service root, which anybody may read, is read there first, and the number it gives is
  compared with the one kept since it was added; another number, or none where one was
  kept, and nothing goes. Where none is kept (one that gave none), nothing is compared: the
  password is then held to that controller by its certificate alone, on its own road and on
  its web page's (the way to its console). The server has the same gap.
* **A server is never known by its certificate.** Its maker may have put one certificate,
  and one key, on every board. So a server's controller is one device by the numbers its
  computer and its controller give for themselves (kept with its card as digests), never by
  the certificate it shows; its kept password goes to its own address and to no other,
  whatever certificate shows there; and a second card of the same server is refused. A
  controller is signed in to with a session (never Basic), one kept by the card's driver and
  given back when the driver goes, also at the program's end within its time.
* **A server is never left booting into its setup** (Start into BIOS setup on this PC's own servers, the web's
  `bios_setup`, stricter). Offered where the controller allows a "BiosSetup" boot override, for one machine, asked
  first. Which server answers is asked first; another tool's override is not replaced; a server between states is
  left alone. The one-time override is set (shown the state it was decided on), seen set, and the server restarted
  where it is on or switched on where it is off, within two minutes of the press or not at all. From the moment the
  override may be set, every way out clears it again where it still shows: a refused or unsent power change, a change
  answered with an error or not shown, a press that ran out of time; and after the restart the start is watched for
  the server taking the override, and one it has not taken by the end of the press's four minutes is cleared all the
  same (the web leaves it set where the restart may have gone). Only a "Disabled" in so many words counts as taken or
  cleared: a read that shows no override (no Boot, a null, an empty one) says nothing, and is watched or read on within
  the time left. Where it could not be cleared, "may be set" is said, and how to see to it. Power is checked, not
  believed: a server switched on that stays off is said so. At the program's end, or where the server's driver is let
  go of mid-way (its card changed), an override that may be set is cleared on the way out by the program's goodbyes,
  under the driver's session, and read back, thirty seconds at the most, before that session is given back; the press
  is told that it may be set. From the moment the change can go out the server is noted beside the settings as "may
  be set" (no secret in it), and let go of only where the override is seen off with nothing of the press's change
  still on its way; otherwise the next start tells it, as a notice and on the server's card, until a look shows the
  override off.
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
  no (0, false, "0", "off", "no", "disabled", "false"). Anywhere else it is left alone, and
  the card says why: where either is a yes, or a word that cannot be read, that the
  controller is set to switch the server's own monitor off while its console is open; where
  either is not said at all, that it does not say whether it does. Whoever stands at the
  server would see the monitor go dark: that is not "only looking" any more. (The owner's
  decision of 2026-11-05: a word that is not said is no no. The web's code counts it as one.)

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
  window only shows, and nobody acts on any machine, until the password is given) and
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
  were made to break it (fuzzing) before it is handed out.
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
| A wall of 64 JetKVM cards (M6 2i) | As 64 preview tiles: under 5 % of the processor, under 300 MB committed, no interface frame over 16.7 ms | Against the fake (2026-10-31, part 10, on the PC's slower cores, every card receiving its JetKVM's whole 1080p60 stream): 1.4 to 1.9 %, 477 to 511 MB, no frame over 16.7 ms, but the interface thread later than 16.7 ms 13 to 16 times in 30 seconds, every four seconds at the wall's status rounds, the latest 51 to 62 ms; 16 cards 1.2 to 1.4 %, 364 to 370 MB, none late. Against the fake: the processor met, memory not, the interface thread not quite. A JetKVM not yet |
| A JetKVM shown large: frame arrived to present returned | As the controlled machine's above | Against the fake (part 10, 1080p60, the monitor at 120 a second): on the card 1.2 to 1.3 ms mean, 99.1 to 99.9 in 100 within 4 ms; in software 2.2 to 2.5 ms; 0 to 15 of about 1,800 made way for a newer one where the fake sent two within one refresh; beside 63 other JetKVM cards 2.3 to 2.7 ms and 31 to 36 made way. Against the fake: met alone and beside 16 but for every picture shown; not beside 63. A JetKVM not yet |
| A JetKVM's first picture (M6 2i) | None set; plan 4 (M1) expects a window on a card's connection to have one in about 0.5 s and a decode | Against the fake (part 10): a card's first key frame 0.23 to 0.27 s after it is asked for, and 4 to 43 ms after the rest of a JetKVM that rested 5.5 or 15 s; a window on the card's connection 0.23 to 0.54 s (the film's next key frame). Against the web's fake (aiortc): 2.1 to 2.2 s, aiortc's own connecting; "Show here" 2.1 s. A JetKVM not yet |
| Live tiles decoded in software | Up to 64 at 720p15, up to 16 at 1080p30; beyond that tiles stay previews | 64 at 720p15 for 9 % of the processor; 1080p30 falls short from 36 tiles |
| Live tiles decoded on the card | As many as its memory carries, at about 37 MB a 720p stream and 75 MB a 1080p one, and never more than its driver is known to bear | 64 at 1080p30 on the fastest card, with its decoder full; one maker's driver hangs from 32 |
| Three days (later thirty) | Handles, memory and graphics memory flat after the first hour | Not yet |
| Graphics device lost | Wall back within 5 seconds, by itself | Told of the loss: previews within a second, 16 and 36 live tiles within 2, 64 at 1080p30 within 6; a real loss not yet |
| Server lost and back | Tiles live again within one picture interval of the server answering | M2: seen to carry on by itself, not timed |
| Installer | Under 80 MB | Not yet |

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
* No sound, no virtual media, no file transfer: later, if ever.
