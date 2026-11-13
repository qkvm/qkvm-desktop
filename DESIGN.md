# QKVM Desktop: how it is built

## A copy that Setup installed, added 2026-11-08

The owner decided on 2026-11-08 that a copy installed by qKVM's own Setup (Inno Setup, per user) may update itself
in place. The program and Setup keep a contract between them, and neither changes it alone. Setup writes
`{app}\qkvm-desktop.installed`, whose whole content is `setup` and a line feed (a portable copy has no such file).

* **The kind** (`core::updateKindOf`, `UpdateKind::Setup`). A copy is Setup's when that marker lies beside the running
  program with exactly that content (six bytes: no other line end, no byte-order mark, no other word); otherwise an
  MSIX copy where Windows gives the program a package identity, as before; otherwise portable. The signed payload's
  platform for it is `windows-x64-setup`, and its package's name ends in `-setup.exe`. A release of another kind is
  refused, as any wrong release is, and a Setup copy refuses the portable and MSIX releases.
* **Its feed** (`core::setupFeedBeside`). A Setup copy reads `stable-setup.json`, in the folder of the feed built into
  the program: `https://download.qkvm.com/desktop/stable-setup.json`. Every other copy reads the built-in
  `stable.json` as before, so beta 1's portable copies, which take only a payload of `windows-x64-portable`, keep
  working unchanged. Chosen over one payload that names a package per kind because that is the smaller change: each
  feed stays the one payload of eleven fields and one platform that the program, `sign_release.ps1` and
  `refresh_feed.ps1` already check; beta 1 could not read a payload of another shape; there is no new build setting
  (the second address follows from the first, so the release audit and the build tools still check one); and since
  the platform is signed, a copy of one kind is never handed the other's package even where the two files were
  swapped on the server. The tools: `prepare_update_payload.ps1 -Setup` (the setup's file version information must
  say the advertised version, read as data, never run), and `assemble_update_feed.ps1` and `refresh_feed.ps1` say which
  of the two names the feed they wrote is served as. Nothing of the feed becomes an argument, a path or a command:
  it names a package, its size and its hash, and its version (three numbers) names Setup's log.
* **Install and restart** (`Updates::runSetup`, `UpdatesView::install`). After the download, which is verified as
  every kind's is, and only when the person presses it, with a way to hand over (`UpdatesView::handOverWith`, given
  by `main`), and only where the program's end cuts nothing short (`WallWindows::whatHoldsAnUpdate`, which says what
  holds it in the person's words, `UpdatesView::holdWith`): every window idle as MSIX copies have it (no control,
  action or file operation under way, `readyForUpdate`); no box with changes that are not saved (Add a KVM, Edit,
  Type text, a saved action's editor: the window's `unsavedEdit`, the box's title); no virtual media being uploaded or
  changed in any window; no action sent from a card or menu that is not answered yet (the list of what is under way
  that every window writes into: a start into BIOS setup through a server among them); and no start into BIOS setup of
  this PC's own servers under way, nor an override of one that may still be set (`core::SetupOverrides`: the program's
  end would wait up to 30 s for its clearing). In this order:
  1. The download's own folder of the update cache (`%LOCALAPPDATA%\qKVM\Cache\Downloads\update-XXXXXX`, never
     Downloads or a shared Temp) was made with its own list of who may do what, in the one call that made it
     (`CreateDirectoryW` with that security descriptor): this user, SYSTEM and the administrators, all of it, and
     nothing taken from the folders above (a profile's folders may let a sandbox's users or an app's capability write
     into everything below them, and on a PC this was built on they do). The cache's Downloads that holds it is made
     so, or kept so from the first download on.
  2. The file is opened with a handle that shares reading only (no writing, no deleting or renaming), kept until the
     program ends, and its size and SHA-256 are read again through that handle against the signed release. A file
     that is not the verified one is removed and downloaded anew.
  3. Setup is started from the file's final name, as Windows says it of that handle (`GetFinalPathNameByHandle`):
     every folder in that name holds the open file, and none of them can be renamed or taken away while the handle is
     open; a junction on the way could be swapped to point elsewhere at the next opening, so the name the file was
     saved under is not used. That folder must hold the file alone (a library beside Setup would be loaded by it), be
     owned by one of the three, and let nobody else add to it, take from it, or change its list or owner.
  4. The folder the program runs from must still hold Setup's marker; it is Setup's `/DIR`. `/LOG` is
     `updates\setup-<version>.log` in the program's data folder. The command line is exactly the contract's
     (`setupCommandLine`): each path whole on a drive, between quotes, and refused where it holds a quote, a sign below
     a space, or ends in a backslash (which would take the closing quote for one of its own). Every path Setup is given
     (its own, `/DIR`, `/LOG`, TEMP and TMP) is the final one, as Windows says it of the folder through no junction:
     Setup runs with Inno's RedirectionGuard and crosses no junction the person made (a data folder moved behind one),
     and would make no log there and give up; the checks above apply to the resolved paths. A log of an earlier
     attempt at the same version is removed before Setup starts (Setup is not run where it cannot be), so that what
     the next start reads is this attempt's.
  5. Setup's process is made suspended, with no handle of the program's, the hand-over noted beside the settings
     (`updates\setup-handover.json`: from and to which version, Setup's process number and start time), and then let
     go. Setup's TEMP and TMP name a folder of its own (`Downloads\setup-temp-XXXXXX`, made as its folder is), so that
     Inno Setup unpacks its `setup.tmp` where nobody else may write, not in the person's Temp; the note names it, and
     the next start takes it away once Setup is over and it is empty; one that was left (Setup ended
     leaving what it unpacked, or its outer process still held it) is taken away whole at a start a week on, where
     all of it is that old and nothing in it is a link or junction (`pruneUpdateCache`). The program that Setup starts at its end would
     have them too: at its start, before anything else, a TEMP or TMP naming such a folder is put back to the person's
     own (`ui::takeBackOwnTemp`; both it and the cache compared as Windows says them of the folders, through no
     junction and in long names, as Setup's TEMP was given). The program closes every window and quits. Its quit's goodbyes (`ui::endTheProgram`: the controllers'
     sessions given back, a clearing of a BIOS setup override, a JetKVM letting go) are given their time, and only
     then is the forced end counted: 9 s after them, and 50 s after the hand-over in any case (`ui::HandOverEnd`), so
     that Setup, which waits up to 60 s for the single-instance mutex (it goes only with the process) and then gives
     up changing nothing, is never left waiting in vain.
* **A start while Setup works** (`ui::whileSetupWorks`, first in `main`, before any window and before the
  single-instance mutex). Setup holds `Local\qKVM-Desktop-Setup` while it runs. A start while it is there ends at once
  and without a word (no box: one that waits for a click keeps the program loaded, and Setup, which tries a file in
  use for four seconds, would give up and roll back): a program that runs while Setup replaces its files keeps
  them from being replaced (its Qt libraries are loaded the moment it starts), and Setup would give up halfway. Only
  Setup's own starts (the update's restart, and the Launch at the end of a setup), with the one argument
  `--after-update`, wait for it to be gone (Setup still holds it then), two minutes at most; then the program is
  started anew without the argument through the person's own shell (`ui::startAnewThroughTheShell`: the desktop's
  `Shell.Application`, `IShellDispatch2::ShellExecute`), so that it is a child of the person's Explorer and takes
  nothing of Setup's (Inno Setup's RedirectionGuard, which a lowest-privilege setup hands down, keeps a program from
  crossing a junction the person made), and Setup's start ends without a window. Where that cannot be done, it goes
  on itself. The argument means nothing else and skips nothing.
* **Data that cannot be read** (`core::whyDataCannotBeRead`, in `main` after the single-instance mutex, before the
  settings are read). The data folder, the list of what is in it, and the settings: where Windows says any of them is
  plainly not there, that is a first start; where it cannot be read for any other reason (a list that shuts the
  person out, a junction this start may not cross), the program says that it cannot read its data, where, and in
  Windows' words, and ends, writing nothing: it never takes such a folder for an empty one.
* **The next start** (`Updates::lookAtHandOver`). A hand-over note and this program's version: the new one, "Updated
  to qKVM X." Not the new one, and Setup's process (that number with that start time) still running (its last
  moments): the note stays, the program says that Setup is still at work, and waits for its end. Once Setup is over
  and the version is not the new one, what Setup said is read from its log (no copy of the program runs while Setup
  does, so its exit code reaches nobody): the last line of the log's last 64 KiB that holds `qKVM-RESULT: `, read as
  the person's own text (one line, printable signs only, at most 200; `setupResultIn`). 105: qKVM was still running
  when Setup needed it closed, so Setup changed nothing. 7: not installed, and nothing was changed, with Setup's
  reason. None: the update was not installed, with the place of Setup's log. Where it was not installed, the update is
  checked for again at once, so that it is offered again (its download still waits for the person). What is said is
  in Settings > Updates and in the first window's corner. A note that does not read is let go of.
* Downgrades and the same version stay refused where the feed is read; a portable copy's update is still only shown,
  and an MSIX copy's still opened in qKVM Setup.
* Tests run a stand-in of Setup's that the build makes (`core/tests/stand_in_setup.cpp`): it writes its command line,
  the SHA-256 of the file it was started from, that file's name, and its TEMP and TMP, and ends with the code it is
  told. `whyDataCannotBeRead` is tried on a folder and settings whose lists deny the test's own user. No test runs a
  setup. `ui/tests/stuck_program.cpp` holds a mutex and never quits, so that only `HandOverEnd` ends it.

## Combined Details added 2026-10-24

Each `WallView` owns a memory-only `DetailsView`. `MergedWall` binds it to the
selected source's `ServerClient`, actual machine ID, source generation, account,
role and window visibility. `Source::detailsClient()` is absent for local devices;
`ServerSource` supplies its existing client. No second credential/session store is
introduced. Closing, hiding or changing access invalidates replies and clears data.

The model bounds work to one in-flight batch and coalesces retargeting. History and
administrator activity are requested together; samples follow a successful summary
with sensors. Typed parsers enforce payload, field and sample limits and retain only
audit display fields. The audit `missing` contract is null or a count/since/why object.
Authorization and MFA refusals clear the entire model; temporary failures are visible
and back off. Refresh runs only while the panel is open.

`ConsoleDetails` keeps headings above a bounded, keyboard-scrollable `Flickable`.
`HistoryGraph` draws bounded average series with an accessible caption and latest
readings. Native diagnostics retain their original measurement definitions. The
panel uses the existing local-input pause and requires no new Qt module or backend
endpoint. Unit, rendered-interface and joint fixture tests cover it; physical and
installed acceptance are still open.

## Console alignment added 2026-10-23

`ConsoleOverflow` measures each offered `ConsoleAction` and moves that same item
between its row and a bounded popup. Priorities choose overflow; principal controls
are pinned. Reparenting preserves command connections and state. `Enlarged` retains
its identity/control row and adds a status strip; `FloatingKeyboard` overlays the
picture and keeps geometry inside the header/status bounds.

Main's dialog state is bound into `Enlarged.externalPanel`, so Actions,
confirmation and text-entry dialogs pause capture too. Capture resumes only
when all local panels are closed. Keyboard focus scrolls each selected key into
view when the panel is small or resized.

`ControlView::setLocalInput` pauses physical capture within an owned session.
`MachineControl` releases remote presses, holds physical keys back until released,
and restores capture only while the original target and foreground permission still
hold. `RelativePointer` releases confinement while local input is active.
`tapKeys` validates bounded USB chords, releases prior input, and checks an input
generation and live identity before every send and after callbacks. Losing ownership
or changing targets interrupts the chord; the normal stop path releases remaining
input. QML owns only one-shot modifier selections, cleared after use or input reset.
No screen key acquires control. These paths use the existing Live protocol and do
not require a new server endpoint. Synthetic native tests and joint server/browser
fixtures cover them; physical-device acceptance is still open.

The shape of the program and the patterns to keep to. What it does is in
[SPEC.md](SPEC.md).

Two kinds of statement are in here. What is called a **rule** is decided. What is called
the **plan** is the way expected to work, not yet shown to work: section 11 lists those,
with what to do when one fails. Calls into Qt, Direct3D and FFmpeg are named from
knowledge of them, not from a build: read the documentation of the version in use for
each before leaning on it.

**Read this first.** Everything below was written for a program that is a client of a
QKVM server and nothing else, and describes what is built: M0 to M2, and the beginning of
a server's live connection. On 2026-10-11 the owner turned the program (SPEC.md,
section 1): it speaks to KVMs itself, and a server is a second place machines can come
from. Of that there is: the driver for KVMs that run kvmd (section 4, `KvmdDevice`),
with a KVM's stream of pictures (`KvmdStream`) and its line for keyboard and mouse
(`KvmdConsoleLine`); this PC's own wall behind a seam (`WallSource`, `LocalWall`); the
adding of a KVM (`KvmProbe`, `KvmAdding`) and the changing of one (`KvmEditing`,
`KvmCertificateReview`), each with its box; a machine had live from either place
(`Live`, `KvmdLive`, `LiveLink`), in the window that shows it large; its keyboard and
mouse taken in that window (`MachineControl`, section 4; the hook, section 3.8); a
session that shows either wall (section 5, "Two walls, one window"); and this PC's wall
taken in from a file and written to one, in the core (`WallExchange`, section 4) and in
two boxes on the screen, tried by tests only (`OwnFilesView`, section 5), behind a rule
of the wall's own: no password goes to an address before what answers there was asked
what it is (section 4, `LocalWall`). In the core as well, and
with no box on the screen yet: the saved actions of this PC's wall (`OwnActions`,
section 4), which the wall runs on its KVMs. The taking of
keyboard and mouse is tested against stand-ins only: the part that hears this PC's keys
from Windows is built and linked, and no test installs or runs it. It has run once for
real: the owner started the program, took keyboard and mouse of a real KVM, and
reported that keys arrive; nothing else is known from that run. Not there: the box for
saved actions kept on this PC. What the rest will change in this document, so that
nobody builds on the old shape:

* `core/` gets a driver for each family of device, the first for kvmd (SPEC.md section
  6.3), beside `ServerClient`: the same rules (a time limit and a size limit on every
  call, strict reading into types, the certificate looked at before a secret goes out),
  and the list of this PC's own KVMs with what each is known by.
* `ui/` has a seam where there was `ServerSession` alone: the wall and the menus ask
  "where these machines come from", which is this PC or a QKVM server. The enlarged
  view's live picture is behind it too (`WallSource::openLive`). Still to come behind
  it: what a machine can do (SPEC.md section 5, "Only what a machine can do"). On
  2026-10-17 the owner decided that the program shows several sources at once, in one
  device tree: this PC's KVMs and any number of QKVM servers; it takes no connection
  from another PC (a wall is shared through a QKVM server), and a hosted cloud comes
  only after a first 1.x.0. The classes for that are in, four rounds of eight and the
  mending of the fourth (section 5, "The sources behind the wall"): inside, the wall
  holds several sources at once; the screens are not in, and show one wall at a time.
* A driver's part that reads a device's bytes is tried with bytes made to break it
  (section 8 gets fuzzing), and what only one family needs is loaded at its first
  device (section 9).
* Nothing of the server's is bundled, and there is no second process: the rules of
  section 1 stay as they are.

Each of these is written into its section as it is built.

## 1. The shape

```
   ui/qml          what is seen. No logic.
      |            properties, models, signals
   ui/             one view model per screen; the glue between core and engine; the Windows bits
    |      \
  core/    engine/   input/
  server   decode    keys and
  client,  and draw  mouse from
  models,            Windows
  settings,
  finding
```

**Rules of dependency.** They are what keeps the hard parts testable alone.

* `engine/` and `input/` know nothing of Qt, of servers or of each other. They are plain
  C++ on the Windows SDK (and, for the engine, FFmpeg). They can be hosted by anything:
  `bench/` hosts the engine in a bare window.
* `bmc/` is plain C++ on the standard library alone: what a server's management controller
  sends of its screen, read (ASPEED's video format, as the web app's `qkvm/aspeed.py`), and
  its console's messages read and written (AMI's protocol, its keyboard and mouse reports
  among them, as its `qkvm/ami.py`, without
  the network: the WebSocket that carries them is `core/`'s). It knows nothing of Qt, of
  Windows, of the network or of the other parts, so that it is tested and fuzzed by itself.
  It runs in the program's own process, as the KVMs' H.264 does (the owner's answer,
  2026-10-29): `core/`'s ServerScreen hosts it, on a thread that may wait for a frame.
  It is built with the standard library's own checks in every build, the release one too,
  and so is `core/src/server_screen.cpp`, which reads what the console says beside its
  frames: an index out of bounds ends the program there (a 27 % cost to the decoder, SPEC
  section 9), and a build without the checks is refused by the files themselves.
* `core/` uses Qt Core and Qt Network only, and `bmc/`. It knows nothing of windows, of
  QML, of the engine. It runs and is tested without a screen.
* `ui/` is the only place that knows all of them. It is thin: it passes bytes from core to
  the engine, events from input to core, and state to QML.
* Nothing depends on `ui/`.

## 2. Folders

```
desktop/
  README.md  SPEC.md  DESIGN.md  THIRD-PARTY.md
  CMakeLists.txt  CMakePresets.json
  base/        what all share: Result, the log, bounded queues, a clock that tests can move
  engine/      include/qkvm/engine/   the narrow interface (section 3.1)
               src/                   Direct3D 11, DXGI, FFmpeg
               tests/
  input/       keys: scan code to HID usage, what is down, the leave-key
  bmc/         a server's own screen from its management controller: ASPEED's frames read,
               AMI's console messages read and written; tests/ (with the web's writer of frames, ported), fuzz/ (libFuzzer's, by hand)
  core/        server client, state machines, models, settings, secrets, finding/
  ui/          main, view models, VideoTile, win/ (tray, title bar, start-up, single instance,
               the keyboard hook)
               qml/  screens/  controls/  Theme.qml  Words.qml
  bench/       the engine in a bare window: the measuring rig and the long run of M1
  tools/       check.ps1, build.ps1, run.ps1, fetch_qt.ps1, fetch_ffmpeg.ps1, make_testdata.ps1,
               the stand-ins: a QKVM's live connection, a large wall, one KVM by itself
```

## 3. The engine

### 3.1 Its interface

A sketch. The names may change; the seams may not: packets go in, a draw call comes out,
and nothing of Direct3D or FFmpeg shows in the header.

```cpp
namespace qkvm::engine {

enum class Codec { H264, Still, KeyFrameStill };  // a still: a JPEG or a PNG, told apart by how it begins;
                                         // a key-frame still may also be an H.264 key frame (a JetKVM's card)
enum class Importance { Preview, ShownLarge, Controlled };

struct Packet {                          // one picture as it came: an H.264 access unit, or a still
    std::span<const std::byte> data;
    std::int64_t arrivedUs;              // on this PC's steady clock
    bool keyFrame;
};

class Stream {                           // one machine's pictures
public:
    void push(const Packet&);            // from one thread. Copies, never waits. When behind, drops up to the next key frame
    void shownAt(SizePx onScreen, Importance);   // how large it is drawn: decides how it is decoded
    bool takeKeyFrameWish();             // true once after pictures were dropped: the host asks the server for a key frame
    Stats stats() const;                 // decoded, dropped, how decoded, milliseconds per stage
};

struct Placed { StreamId stream; RectPx where; };   // physical pixels; the picture keeps its shape inside

class Scene {                            // what one window shows
public:
    Stream& add(StreamId, Codec);
    void remove(StreamId);
    void place(std::span<const Placed>); // taken over at the next draw
    bool draw(const Target&);            // on the drawing thread: the newest picture of each placed stream.
                                         // False when nothing had changed and nothing was drawn
};

struct Host {                            // what the engine asks of whoever hosts it
    std::function<void()> somethingNew;  // a newer picture is ready: draw soon. From any thread
    std::function<void(Fault)> fault;    // a stream failed, the device was lost and is back, ...
};

class Gpu;                               // one per graphics adapter: owns the device, loses it, gets it back (3.4)

}
```

### 3.2 The way of one picture

```
packet in --> decoder --> slot of one --> (upload) --> drawn as a quad
 any thread   its thread   newest wins    drawing thread
```

* **Decoder.** One interface, three kinds: H.264 on the graphics card (FFmpeg with
  D3D11VA: the picture is a slice of a texture array and never leaves the card), H.264 in
  software, stills in software. A still is a JPEG or a PNG, told apart by how it begins:
  a server passes on whichever a machine gives, and a machine may give now one and now
  the other. A JPEG's output stays as its planes; a PNG's pixels are made into three
  planes of green, blue and red, with what is seen through laid over black, so that one
  way of drawing serves both. Each still is handed to its decoder marked as a whole
  picture: FFmpeg's decoder of PNGs takes one that is not marked so for what has
  changed since the last, and adds the two together where they are of one size and
  kind (seen on this PC's own wall, whose made-up KVMs send such PNGs one after the
  other; a server's stills had the same coming). A stream changes kind while running,
  at a key frame.
  Since M6 increment 2c a still of a stream made for them (`Codec::KeyFrameStill`, a JetKVM's
  card, whose source says what it sends: how a packet begins never decides it) may also be an
  H.264 key frame that begins with its SPS, as a JetKVM's connection holds one. The
  size its parameters say is read first, by FFmpeg's parser of H.264, which takes nothing by it:
  one larger than the program shows, or of no size the parser could read, reaches no decoder
  (a decoder takes its tables by that size before refusing it, some 100 MB for an SPS that says
  16000 by 16000; 2c, after its reading). It carries one SPS, one PPS and one picture (the same
  SPS and PPS twice, as core's assembler hands them where a device sends them in the picture
  too, are taken; 2c, the second reading: one was judged by its first SPS alone). Parameter sets
  are known by their ids: SPS and PPS of two ids in one packet are lawful, one id that comes twice
  and differs is refused, as is a set whose id or size cannot be read, and every SPS is read for
  the size it says (by the program's own reading of H.264's 7.3.2.1.1, the emulation bytes taken
  out, which takes nothing by that size), not only the one the parser follows (2d and the SPS
  gate, after their reading). That reading takes only the profiles it and FFmpeg read alike:
  baseline, main and extended without chroma fields, the four high ones (100, 110, 122, 244) with
  them; any other SPS is one that cannot be read, and refused. (2e and the SPS reader, after their
  reading: the book and FFmpeg differ on which profiles carry chroma fields; an SPS of profile 144
  made to say 16 by 64 to this reading said 16000 by 16000 to this build of FFmpeg, which took 94
  MB by it.) Where FFmpeg's first reading of an SPS fails, it reads it a second way, as it came,
  its emulation bytes left in: so what it refuses on that first reading is refused here (more than
  16 reference frames, two bit depths, colour planes coded apart), and an SPS that has emulation
  bytes is read as it came too, and refused where that reading says a size the program does not
  show (2e, the second reading: an SPS made to say 48 by 176 taken out, read beyond its end there,
  and 1760 by 73200 as it came, had this build of FFmpeg take 44 MB by it live and 48 MB as a still,
  where the parser had read it once, as 48 by 176). A live H.264
  decoder, in software or on the card, is held to the
  same: an SPS new to it for its id is read for its size before the decoder has it, and a packet
  with one too large, or with parameters that differ within it, is refused (the stream waits for
  its next key frame). Such a refusal is the packet's fault, not the card's: a stream on the card
  stays there (it went to software, and stayed, at one such key frame). Otherwise it is decoded by
  an H.264 decoder
  made for it alone, given the packet and then the stream's end, its one picture taken in
  software planes and the decoder freed. So nothing stays between pictures: no picture of
  one card can come out for another, 64 cards hold no decoder each (some 3 to 10 MB a
  decoder; `aKeyFrameStillHoldsNoDecoderAfterIt`), and a broken key frame breaks that one
  picture. A still that is byte for byte the one last decoded is let go and counted
  (`Stats::stillsSame`), where nothing else waits in its inbox (no newer one, no redraw after
  a lost device) and none is being decoded: what is drawn is that picture already, and a
  card whose machine's screen does not change is asked again and again. A still whose
  decoding failed never becomes the last decoded, so it is tried again when it comes again;
  nor is one whose picture could not be copied to the card because a texture or a view could
  not be made with the device as it was (its memory full): it is not on the screen, and the same
  again is decoded again (one of a kind the engine does not draw at all is not tried again and
  again; nor is one whose grey texture could not be made, since 2d's reading). The one decoded
  is kept as the last before its picture is told of. What a window shows as pictures on the
  screen a second counts the stills let go as the same: the screen shows them.
* **A copy that fails.** Making a stream's planes anew lets go of the ones before, and what is
  drawn goes with them first: a copy that then fails leaves an empty tile, never views that
  are gone (2c, after its reading; they were drawn from before). The same where a decoder on the
  card gives a new texture and the views of it cannot be made (2c, the second reading). And what
  is drawn holds its views (`Drawable`, by `ComPtr`), so that nothing drawn can outlive them.
  A stream that goes from the card to software lets go of the card decoder's views with its first
  picture drawn from memory, so that the decoder's texture can go (2d's reading; the engine's
  count of the card's memory does not show that, `aStreamGoneToSoftwareLetsGoOfTheCardsTexture`
  measures it only: held by reading).
  Every decoder is told the largest picture the program shows, and refuses a larger one
  before it takes memory for it.
* **The slot.** Between decoder and drawing there is room for exactly one picture. A newer
  one replaces an older one that was never drawn, and that is counted. Nothing queues.
* **Upload.** Software pictures are copied into textures on the drawing thread, just
  before drawing, with a budget of time per draw: what does not fit waits for the next.
* **Drawing.** Every tile of a window is a quad in one draw pass into one swap chain. One
  pixel shader per layout of planes turns YUV into what the screen wants, with the
  stream's own matrix and range. No colour management beyond that: values go out as Qt's
  own drawing sends them.
* **Nothing per frame from the heap.** Packets, pictures and textures come from pools.

### 3.3 Threads

| Thread | Does | Never |
|---|---|---|
| Interface (Qt's main) | QML, view models, the ordinary calls to the server | Waits for a file, a device, a lock held during decoding |
| Drawing (Qt's render thread, or the rig's) | `Scene::draw`, uploads | Decodes |
| Network (one) | Live connections: reads pictures, hands them to `Stream::push`, sends keys and mouse | Touches a QML object |
| Decoding | A pool shared by every stream, software or on the card (each packet's work runs on it; the "one light thread per stream decoded on the card" first planned here was never so): 2 to 12 threads, the PC's processors less two (`Gpu::Options::decodeThreads` for tests and the rig). Since M6 2d it has two lines: a stream shown large or controlled goes before previews that wait, and previews run on all its threads but one, so that one is free for such a stream also while previews' decoding is under way (on a pool of two, previews have one thread: 64 JetKVM cards then fall behind, each showing its key frames less often, and a redraw after a lost device takes longer). Nor do previews starve: where the pool has three threads or more, urgent work runs on all but one while a preview waits, so that one is theirs (2d's reading; on a pool of two urgent work may have both); where none waits, urgent work takes every thread (2e's reading). A stream taken away while its decoding is under way is ended by the pool once that is over: whoever takes it away does not wait (it waited, up to 45 ms of the interface thread for an 8 MB key frame); a scene waits for those as it ends. What waited for it goes as it is taken away, nothing more of it is decoded, and it is urgent no more (2d's reading: what waited was decoded, urgent, ahead of the next machine shown large). 64 cards redrawn after a lost device: the first draw 14 ms on this PC | Blocks on the interface |
| Reading a server's screen (M7; review 3's HIGH 1, 2026-11-06) | A server's console frames, read into its picture (bmc/'s ASPEED reader: about 46 ms a busy 1080p frame on a slow core in release), and the card's still made smaller from it, on a thread of Qt's pool (`QThreadPool::globalInstance`, which the application waits for as it ends), one screen's on one thread at a time in their order (core/src/screen_reader.h). Two frames wait for it at the most: the console's line is held back meanwhile (`DeviceLine::holdBack`), and nothing is thrown away unread (each frame brings only what changed). What it read goes back through a picture of its own; a screen whose thread was busy is handed the newest picture with all that changed since it last took one | Touches the screen or anything of Qt's but the one word it posts to the screen; holds a lock while a frame is read |
| Keyboard hook | Exists while a machine is controlled, and after that only until the keys it kept from Windows were let go, a minute at the most (3.8): says of each key whether Windows gets it, hands it on, and holds Windows' second account of the keyboard against what it heard | In its procedure: takes memory, waits for a lock, logs |
| Writer | Log and measurements to disk | Makes anybody wait: a full queue drops and counts |

Every thread is made in one place and has a name. Every queue has a size and says what it
does when full. Nothing polls with a sleep.

### 3.4 The graphics device, and losing it

**Rule: losing the device is an ordinary event.** Windows takes it away when the driver is
replaced (Windows Update does that by itself), when a laptop changes adapter, after a
reset. A wall that runs for weeks will meet it.

* `Gpu` owns the device and its context, made with video support and with multithread
  protection on, and in debug builds with the debug layer on and its messages turned into
  test failures.
* One lock guards the immediate context. Drawing takes it for its pass; FFmpeg takes it
  through the lock callbacks of its device context.
* Everything made from the device (decoders, texture pools, shaders, swap chains) is
  registered with `Gpu` and can be dropped and made again. When a call reports the device
  removed, `Gpu` drops everything, makes a new device, rebuilds, and tells the host;
  streams stay connected and ask for a key frame: every H.264 stream asks as its drawn picture
  goes with the device (a still screen sends nothing by itself), one on the card also as its
  decoder goes; and at its next packet each looks again at whether it belongs on the card of
  the device made anew, asking where it is not there and the card has room for it, and the
  packet is not the key frame that puts it there (a key frame that came while the device was
  gone was decoded in software). A stream the card gave up on is tried on it again after a
  loss, for one key frame and one try. A JPEG stream decodes its last picture again unless a
  newer one waits (it keeps that picture as it came, tens of kilobytes for a tile; what is
  decoded again is the newest decoded when the decoding comes to it), so a paused wall is
  there again after a loss without a new picture.
* A window's device is on the adapter the settings' choice comes to, or on Windows' own where
  that one cannot make a device, and after a loss on the one the choice comes to then, which
  need not be the one before. It does not follow the window to a monitor on another adapter:
  Windows carries every frame there. (Planned at first: rebuilt on that adapter. Measured on
  a PC whose monitor is on one card, a window of 1600 by 1000 drawn on the other showed every
  picture, its presentation returning as soon, at a quarter of the cards' drawing for one
  machine shown large; a window as large as a 4K monitor did
  not keep up there in the bare rig (part 2), and two monitors, each on a card of its own, were
  not tried. The settings say, where the screen is on another adapter than the one chosen,
  that Windows carries every picture to it.)
* Tests lose the device on purpose, through the same path.

### 3.5 Who gets the graphics card

**Rule: a policy object decides how each stream is decoded, and nothing else does.** It is
given, for every stream, how large it is on screen and how important, and, for the PC,
how loaded the decoder and the memory of the card are.

* Small and idle tiles are JPEG previews, scaled by the device or the server: this is
  what makes a wall of 64 cheap, and it is what QKVM's page does already. A JetKVM's card is
  its newest key frame as a still (3.2), decoded in software: a still never takes the card,
  so no card takes a hardware decoder from a machine shown large.
* The controlled machine always gets the card.
* Tiles shown large get the card while there is room. A decoder on the card holds about a
  dozen surfaces, and a 1080p surface is 3 MB: 64 of them would take gigabytes. So there is
  a budget in megabytes and in decoders, and what does not fit is decoded in software or
  stays a preview.
* When pictures are being dropped, the policy first lowers what it asks of the server
  (pace, then size), then moves streams off the card.
* Each tile can say how it is being decoded, for the diagnostics line.
* Whoever sits at the PC chooses in the settings (Video). Automatic is what of this policy
  is built: the card for streams shown large or controlled, as long as it has decoders free
  (16 a device), and software for the rest; no budget in megabytes, no lowering of the
  pace, no moving off the card yet. On asks the card for every H.264 stream, as far as it
  has room; Off decodes everything in software. A stream whose way of decoding the new
  choice changes asks for a key frame and changes at it (`Gpu::setDecoding`); one whose way
  stays the same, or cannot change (the device decodes nothing itself, or every decoder it may
  have is taken), asks for nothing. Chosen On, a stream the card once gave up on is tried on
  it again (and asks for a key frame for that). And which adapter: the most powerful there is (the
  default, `DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE`), the one that saves power, or the one
  Windows gives a program that asks for none; by the choice and never by an adapter's name,
  which is only shown, and taken up when the program starts. Where the chosen adapter
  cannot make a device, Windows' own is taken. Not built: the settings show the adapter the
  choice comes to, not the one a window fell back to; and a window's details show the name
  it started with, also where a lost device came back on another adapter.

### 3.6 Under Qt Quick

**The plan.** The engine draws underneath the QML scene, in Qt's own render pass, into
Qt's own swap chain: one device for everything, no copy, and QML draws names, states and
menus over the video like over any background.

1. Qt Quick is told to use Direct3D 11, and each `QQuickWindow` is given a device of the
   engine's before it is first shown (`QQuickGraphicsDevice::fromDeviceAndContext`). A
   device of its own for each window (`engine::Gpu`): Qt draws every window on a thread
   of that window's, a device has one context to draw through, and two threads that
   draw through it at once set each other's state. (The other way is one device, and the
   windows' drawing taking turns under the device's lock, which the decoders take
   already. It would save what a device costs, and let
   one decoded picture be drawn in two windows; it is not built.)
2. A `VideoTile` item paints nothing. At the synchronize step, when Qt holds the interface
   thread still, the rectangles of all tiles of the window are copied to `Scene::place`.
3. On `beforeRenderPassRecording`, connected directly so that it runs on the render
   thread, between `beginExternalCommands` and `endExternalCommands`, the engine draws.
4. A new picture asks the window for a frame. One of a stream shown large or controlled
   (`somethingUrgent`) asks the window's drawing thread itself, as a job of the window's
   there (`QQuickWindow::scheduleRenderJob`, handed over from the interface thread) that
   calls `update()` on that thread: Qt then draws a frame at once, without the interface
   thread's polish and sync, which a new picture does not need (0.1 ms from the picture
   decoded to the frame that takes it, against 5.8 through the interface thread, as
   measured). A wall tile's (`somethingNew`) asks through the interface
   thread, at Qt's own pace, which gathers the pictures of many tiles into one frame. The
   window draws only when something changed.

This is to be shown in M1, step 6, with QML text over 64 tiles, two windows on two
monitors, and the device taken away. Section 11 has the other ways.

### 3.7 The controlled machine's picture

A KVM's frames come at the device's pace, not the monitor's. So: present when a frame
arrives, never keep a second one waiting, and prefer a late frame dropped to a queue
built. What decides between presenting with the monitor's refresh and presenting at once
(tearing allowed) is a measurement in M1, on the rig and under Qt, and it becomes a
setting only if the two differ in a way a person can see.

### 3.8 Keyboard and mouse (`input/`, `ui/win/keyboard_hook`)

Four parts, each behind a seam, so that each is tested without a keyboard:

* `input/keys`, plain C++ that knows nothing of Windows: `usageOf` (what Windows tells
  of a key becomes its USB number), `KeysDown` (what is down at the far end: a held key
  is said once, and a key goes up there only if it went down there) and `ControlKeys`
  (the leave-key's rule, below). One `ControlKeys` and one `KeysDown` are for one time
  of controlling a machine.
* `KeySource` (`ui/key_source.h`): where keys come from while a machine is controlled,
  and what is told of them. Behind it the hook; or in a test one that is fed by hand
  (`ui/tests/keys_by_hand.h`), or the hook's own procedure, handed each key as Windows
  hands a hook its keys and as Raw Input tells of them (`KeysAsWindowsTellsOfThem` in
  `ui/tests/test_control.cpp`).
* The hook (`ui/win/keyboard_hook`), among the Windows bits of `ui/` and not in
  `input/` as section 1 has it. `WindowsKeyboard` is what touches Windows. Everything
  it decides is in plain parts beside it (`hook::`), plain values in and plain values
  out: `Procedure`, which is all that is done with a key, with `KeysHeld`,
  `WindowsKeys`, `LeaveWatch`, `TwoAccounts`, `KeysWaiting` and `KeyRing` in it;
  `LookWatch`; `LastToGo`; `meansAKeyboardWent`; `whatToTell` and `Telling`;
  `heldKeys`.
* `MachineControl` (`ui/control`): who takes keyboard and mouse in a window and passes
  them on (section 4, "Keyboard and mouse of the machine that is shown large"), with
  the one memory of what is held (`KeysKept`, `ui/keys_kept`).

All of it is tested against stand-ins only. `WindowsKeyboard` is built and linked into
the program, and no test installs or runs it: a test that did would hold the keyboard
of the PC it runs on. What is tested are the plain parts, on plain values
(`ui/tests/test_keyboard_hook.cpp`, `input/tests/test_keys.cpp`), and `MachineControl`
with keys that a test presses by hand, in some tests through the hook's own procedure
(`ui/tests/test_control.cpp`).
It has run once for real: the owner started the program, took keyboard and mouse of a
real KVM, and reported that keys arrive. Nothing else is known from that run. So what
is said of Windows, here and at the code, is still from memory: `keyboard_hook.h` says
of each thing how sure it is, and none of it was looked at one by one. Section 11 has
what is open in it, what is untested because it lives only in `WindowsKeyboard`, and
what only a person at a keyboard can try.

**Physical keys.** A KVM sends USB HID reports, so what is read is the scan code, and it
is turned into a HID usage with Microsoft's own table, including the odd ones (Pause,
Print Screen, Num Lock, the right-hand modifiers): `usageOf`. It goes by the keyboard's
number for the key and its mark (the keys that came later than the first PC's have one)
wherever those say which key it is, and by Windows' name only where they do not: Windows
names some keys by what else is held (Num Lock is "Pause" to it under Ctrl), and the key
is the same key all the while. What follows from that fact in the hook:

* A key is the key it went down as, for as long as it is down (`KeysHeld`: a table of
  fixed size of which keys are down, by number and mark, each with the name it went
  down under). Said again, and going up, it is told, and passed to Windows or kept from
  it, as what it went down as; also where it comes under another number that is the
  same key by its USB number (Pause is "Break" under Ctrl, Print Screen has another
  number under Alt). So the "up" of a key lets go on the machine what its "down"
  pressed there, and what Windows saw go down under one name it sees go up under that
  name.
* Num Lock is one key at either of its two places in that table. With its mark it is
  Num Lock; without it and named "Pause" it is the Pause key by its USB number, and
  whether Windows ever tells a hook of Num Lock so, under Ctrl, is not known. While one
  of the two places is down, what is told at the other is that key, said again or let
  go as what it went down as, whether Ctrl is let go before it or after it. What it
  went down as may then be the wrong one of the two for the machine: Ctrl+Pause where
  Ctrl+Num Lock was pressed. No other pair of places is made one so: with the mark and
  without it, the number of every other key belongs to two keys, or to a key and to
  what a keyboard makes up, and to let go a key that a finger holds is the worse error.
* What Windows cannot name (it tells of it as 0xFF) is no key, going down or going up:
  it changes nothing in that table, lets nothing go, and is nothing to a machine. A
  keyboard makes up a Shift going up and down around Home, End, the arrows, Insert and
  Del under Shift. Were a hook handed that Shift without a name at Shift's own place,
  the Shift that a finger holds stays down on the machine, and what arrives is
  Shift+Home. Whether Windows hands a hook that Shift at all, and how, is not known:
  section 11.
* Which keys are down is that table's to say, by the keys as the hook hears them, and
  nobody else's. While control goes on, a key that is down there goes up by its own
  "up" as it is heard, and by nothing that Raw Input tells meanwhile: Raw Input keeps
  no account of its own of what is down, and its word lets no key go under a finger
  (`aKeyThatAFingerHoldsIsNotLetGoOnTheOtherAccountsWord`,
  `aKeyGoesUpByItsUpAsItIsHeard` in `ui/tests/test_keyboard_hook.cpp`). An "up" that
  Windows names in a way not foreseen here, so that it is no key of the keyboard's (a
  case nobody has seen), is not that key's and lets nothing go: Raw Input's telling of
  the "up" waits in vain to be heard, and control ends within the moment, as two
  accounts that differ (below). A key is up by Raw Input's word only where control
  ends with it or is over: as it ends over an "up" that was told of and not heard, a
  key that had been kept from Windows and that Raw Input told of as gone up is let go
  in the table and told as gone up, so that nobody waits for it
  (`Procedure::takeWhatWasToldToGoUp`;
  `anUpNamedAsNoKeyOfTheKeyboardsIsNotThatKeysAndEndsTheControl`); and once nothing is
  swallowed any more, Raw Input tells of such a key going up where the hook did not
  hear it (`whileNoHookHearsRawInputTellsOfWhatWasKeptFromWindowsGoingUp`). An "up"
  that was heard and is never told of ends nothing: the key was let go by that
  hearing.
* Keyboards are not told apart (a second keyboard, a macro pad, a scanner, a laptop's
  own beside one that is plugged in). A hook is not told which keyboard a key came
  from, and nothing here asks: a key is let go, in that table and on the machine, when
  any keyboard lets go of it. A key that is held on two keyboards at once goes up when
  the first of them lets go, and goes down again, as a press, where the other says it
  again. (Windows is shown that "up", or is not, as of any key: what it saw go down,
  it sees go up.) Why it is so, and what it costs, is below ("Two keyboards").
* With the "up" after which that table holds no key at all, and nothing waits to be
  handed on, that is said (`KeySource::nothingIsDown`), and `MachineControl` lets go on
  the machine whatever is still down there. Every key was told as gone up by then: it
  is a net beneath the rest, and finds nothing to do while the key source keeps to what
  it says of itself.

Not covered, and not to be had from a hook alone: on a PC that gives no second account,
an "up" that the hook is never called for, or that comes at another place than its
"down" and is not Num Lock's. That key stays down on the machine until it is pressed
and let go again, or control ends.

AltGr arrives as a left Ctrl of Windows' making followed by right Alt. The made-up Ctrl
comes with a number no key has, so it has no USB number and is no key: it is not sent,
not kept back and not compared, and its "up" lets go of nothing. Tried with the numbers
Windows gives for it, not at a keyboard with such a layout. A key that is already down
is not sent down again (the remote machine repeats by itself).

**When the hook is in.** A low-level keyboard hook (`WH_KEYBOARD_LL`) on a thread of its
own, with a message loop. The thread goes before the program's other work (a higher
priority) and looks at least ten times a second. The hook is put in when keyboard and
mouse are taken (`KeySource::begin`, said at every taking). A hook that is in goes on
where it was not closed, is the last one the program put in, is still the one that is
told of the raw keyboard, and its thread still looks; otherwise a new one is put in,
which is told which keys the one before it had as down. Everything that takes time is
done before the hook goes in; then the hook; then Raw Input is asked for, and with it
Windows' word for a keyboard that is taken away; then what Windows has as down is
asked. What a key does in the instant between the hook going in and Raw Input being
asked for is not closed, in either order of the two askings: section 11.

It does not go the moment they are given back where a finger is still on a key that it
kept from Windows: Windows does not know that such a key is down, and nobody else would
hear it go up. Such a hook stays, swallows nothing, and tells of nothing but those keys
going up, until each was let go (`MachineControl::listenNoLongerThanNeeded`); and for a
minute at the most, after which its own thread takes it out, whatever has become of the
interface thread (`staysForMs`). Otherwise it is never there: no key of the PC passes
through the program while no machine is controlled, or was a moment ago. The window is
not looked at in the hook: one that is no longer in front gives keyboard and mouse
back (`MachineControl`), and they are taken anew by hand when it is in front again.

`begin()` and `end()` wait for the hook's thread for two seconds at the most. A thread
that does not answer in that time is left to end by itself: what it works on (`Run`) is
held by it and by the interface thread, ends with whichever lets go last, and swallows
nothing from that moment on. Which keys were down, and when each was heard, is
forgotten by whichever of the two comes last, and by that one only: the thread, as the
last thing it does with what it worked on, or whoever waited for it, where the thread
ended in that very moment (`LastToGo`).

**What the procedure may do.** Windows calls it for every key before any program is
told of the key, and waits for it. So it takes no memory, waits for no lock, logs
nothing and calls nothing that can: it reads and writes tables of fixed size, puts what
is to be told into the gate's row (`KeysWaiting`, 32 places) or into a ring (`KeyRing`,
256), sets the event that wakes the interface thread, and is done. The two threads
share nothing but one `Procedure`. A key is kept from Windows (swallowed) only while
all of these hold: swallowing was asked for, and this hook was not closed; the
interface thread has said within the last second that it is alive (`stillThere`, said
four times a second while a machine is controlled); the key came while Windows still
waited for the procedure, with a margin (below); and there is room to tell of it.
Nothing is swallowed that cannot be told. Beyond that:

* What Windows saw go down, it sees go up (`WindowsKeys`): a key that was down when the
  hook went in, or whose press could not be swallowed, is never kept from Windows when
  it is let go, and its repeats are not told as presses of the machine's. A press that
  cannot be swallowed is Windows': it was typed into this PC, is told as that, and is
  not typed into a machine as well.
* While keys are swallowed, every key that goes down and up is told, a press with
  whether it was kept from Windows. While they are not, only the going up of a key that
  this hook had kept from Windows, and nothing of any other key of the PC.
* What another program types, which Windows marks, is passed on before anything else is
  looked at, and is told to nobody as a key: it is not sent to a machine, not noted and
  not compared. Only that such a key came while keys were swallowed is marked, once for
  each time they are (`KeySource::typedByAProgram`).
* What is kept is which keys are down, by the hook's own table; of the keys that are
  on their way to the interface thread, how many they are and the number of the last
  (below, "Clicks behind keys"), which begins at 1 again whenever none is on its way;
  and, only where this PC gives a second account to hold it against, when a key was
  heard that is not told of yet, and when one was told of that is not heard yet (of a
  held key that is said again more often than there is room to note that, how many
  more sayings wait, and when the last came; and of a press that waits in the gate's
  row, how many sayings of its key were heard since). A hearing is noted until the
  second account has told of the key, and for two of the gate's moments at the most
  where it does not: a count of what waits at this moment, which says nothing of how
  often a key was pressed. Where there is no second account, nothing is noted of a
  hearing at all: not when, and not how often
  (`whereThereIsNoSecondAccountNothingIsNotedOfAHearing` in
  `ui/tests/test_keyboard_hook.cpp`). Nothing of a keyboard is kept: no handle of a
  device, and nothing else that names one. And nothing else: nothing that is typed is
  counted or written down.
* A key is handed on and forgotten, and what held it is wiped where it stood: the
  ring's place as the key is taken out, and the gate's place as a key goes out or is
  dropped. What the interface thread has to tell stands in a row of its own for the
  time of one telling (`Telling`): each place of it is wiped as what stood there is
  taken out to be said, all of them where the row is forgotten, and the whole of a
  row that was outgrown before it is given up (`whatWasToldIsWipedWhereItStood`).
  That row, the one copy of a key that a pass holds while it is said, the ring, the
  gate's row and the tables of the two accounts where a hook begins anew or is
  forgotten, and in `WindowsKeyboard` what was read of a key from Raw Input and the
  note of which keys the hook before had as down, are wiped in Windows' own way of
  writing nothing over memory (`SecureZeroMemory`), which a build that is made fast
  does not leave out where the memory is given back right after. Not wiped, and said
  so at the code: what stands of a key in `Procedure::key`'s own places as it
  returns, and what the compiler keeps of one by itself (a register, a place that was
  given back).
* Nothing in the program types into Windows, ever: no `SendInput`, no key's message
  posted to a window. (A watch that sent the program a key of its own four times a
  second was built once and taken out: that is input, it keeps a PC from locking, and
  where the hook is gone it reaches other programs.) A test reads every folder of the
  sources for the ways of doing it (`nothingInTheProgramTypesIntoWindows`). So control
  does nothing that keeps Windows from locking the PC, from its screen saver or from
  sleep. (A window that fills a screen keeps the display awake, on purpose and by no
  input: `ui/win/display_awake`. Control adds nothing to that.)

**The two accounts, and the gate.** Windows takes a hook out without a word when its
procedure was slow, has no way of saying whether one is still in, and may leave it out
for a single key. So the hook's thread asks for a second account of the keyboard beside
the hook's: Raw Input (`RIDEV_INPUTSINK`, told to a message-only window of the thread's
own), which Windows tells of every key that a real keyboard sends, whether a hook is
called for it or not, and which sends nothing. It is the keyboard's own account: it
does not see what a remapping of keys, Windows' key helpers (Sticky, Filter and Mouse
Keys) or another program's hook make of a key. The two are held against each other, key
by key (`TwoAccounts`): by the keyboard's number for the key and its marks, as `usageOf`
reads them, never by Windows' name. Three keys that the two tell of with other numbers
or marks are made one (`fromRawInput`, `pressedOf`: Pause, Num Lock, the right Shift).
What the second account does not tell of is not compared, waits for no telling and
takes none: the Ctrl of AltGr, what a program typed, and a key without a USB number of
the keyboard's page. A key for sound is such a key: Volume up has the keyboard's
number of B with the mark, and is not B. No key is read with its mark turned, nothing
of such a key's hearing is noted, and what Raw Input may tell of it is nothing to the
comparing (`aKeyForSoundIsNotTheLetterThatSharesItsNumber`,
`theHearingOfAKeyForSoundDoesNotTakeTheTellingOfTheLetter`). What that costs: a hook
that Windows took out is not found by a key for sound, and is found by the next key
of the keyboard's page. A key that was down as the comparing began is not compared
until it has gone up.

What is compared is what the procedure heard against what Raw Input told of: every
key that was told of has to have been heard, the same key going the same way, within
a moment before or after, and every press that was heard has to be told of. That is
all that is made of what Raw Input tells. It is one account for all the keyboards of
the PC: Raw Input names the keyboard with every key, and that is not looked at and
not kept. One thing it says of where a key came from is used: a key that comes with
no device was typed by a program, and is not brought to the comparing. It is no
account of its own of what is down, and while control goes on no key is let go on its
word ("Physical keys", above).

* **The moment** within which the two have to be at one about a key is the time Windows
  waits for a hook and a tenth of a second: 400 ms at the most. Whichever of the two
  comes first waits for the other. The hook is taken to hear first; both orders are
  handled the same, and the tests try both: with a fast pair of keys, with a key that
  is said again thirty times a second, and with one that is said ten times before the
  other account's first word.
* **The gate** (`KeysWaiting`; `Procedure::hand`, `passOn`). A key that goes down is
  handed on to the interface thread only once the second account has told of it: that
  word says that no key before this one was left out. It goes behind whatever waits
  before it, and not while a key that was told of waits to be heard. What waits goes
  out in its order when the two are at one again, and never where they are not: a "c"
  whose Ctrl Windows left out goes to no machine. And a press goes by its own word,
  and by no other. Raw Input tells in the order the keys came in, so its word for a
  key going down is a press's own when just as many sayings of the key still wait for
  theirs as were heard since that press (`KeysWaiting::One::saidSince`,
  `KeysWaiting::tell`). Its word for a saying of the key from before the press (the
  key was held, said again, let go and pressed anew, with the hook's thread a little
  behind) is that saying's, and the press waits on for its own: taken for the
  press's, it would let the press out before a key that Raw Input has still to tell
  of between the two (`aWordOfRawInputsIsForTheHearingItBelongsTo`). That Raw Input tells
  of one key's sayings in the order the hook was called for them is from memory.
* A key that goes up waits for nobody's word where its "down" has been handed on:
  letting go on the machine what is down there is always right, and a key that is held
  there a moment longer is said again by the machine itself. It never passes its own
  "down" where that still waits; and one of the eight modifiers never passes any "down"
  that waits, which was pressed under it and would arrive bare. It waits for nothing
  else, and for nothing that has to do with whose it was. Where it waits so and the
  same key goes down once more, that is a press like any other: it takes its place
  behind that "up", and goes when its own word has come
  (`aModifierGoingDownOnceMoreWhileItsUpWaitsTakesItsPlaceBehindIt`,
  `whatIsPressedAfterAModifierWasLetGoGoesOutBehindItsUp`).
* A key that is down and is only said again waits for nobody's word either, and takes
  no room to wait in: it is handed on at once, and not at all while its own "down"
  still waits, to which it adds nothing. The two accounts are still held against each
  other saying by saying, so that a hook which Windows took out under a held key is
  found by the next saying. But however many sayings of a held key one account is
  ahead of the other, nothing waits for them, and nothing ends by their number. Where
  Raw Input does not tell of a saying of a held key (it is taken to tell of each),
  nothing ends by that either. But the same key, let go and pressed anew within the
  moment of the last such saying, waits for a word that is taken for the saying's, and
  control ends there. Within that moment and no longer: what was noted of such sayings
  longer ago is dropped as the press is heard (`TwoAccounts::pressedAnew`;
  `aPressGoesByItsOwnWordOnceSayingsThatWereNeverToldOfAreAMomentOld`).
* The hook's thread may be behind Raw Input's words, or ahead of them, by more than
  one hearing: whichever comes first waits for the other, for the moment. A thread
  that is a quarter of a second behind is borne. Shift tapped twice in that time is a
  press, an "up", a press and an "up" that are heard before the first word: they wait
  in four places of the row's thirty-two, each for no longer than its word is on its
  way, and each press goes by its own word with its "up" right behind it. A key that
  is held until it says itself again, let go, and tapped again at once goes on as
  well: the saying and the "up" go at once, and the new press waits for its own word
  (control: `aModifierThatSaidItselfAgainCanBeLetGoAndTappedAgainWithTheHooksThreadBehind`).
  Every short sequence of one key, and of a modifier with a key under it, is held to
  these rules at every step, in every order of the two accounts
  (`aKeyIsRightAtEveryStepOfEveryShortSequence`,
  `aModifierAndAKeyAreRightAtEveryStepOfEveryShortSequence`); each says what it leaves
  out.
* **Two keyboards** are not told apart, and there is one rule (`Procedure::key`): a key
  is let go, in the hook's table and on the machine, when any keyboard lets go of it.
  An "up" lets its key go as it is heard, whichever keyboard it came from and
  whichever else may hold the key. What that costs is known, and is the side to fail
  on: with two keyboards in use at once, a modifier that is held on the one is let go
  by the same modifier's "up" on the other, and the next key from the first arrives
  bare; whoever sees it presses the key again. Where the first keyboard goes on saying
  the key, it goes down again with its next saying, as a press. A key that is kept
  back as held from before the keys were heard is kept back no longer after such a
  tap, and the bar's line for it goes. The leave-key gives the keyboard back by its
  "up" as the hook hears it, whatever Raw Input has still to say; held on one keyboard
  and tapped on another, the tap's "up" gives the keyboard back. Why it is so: for two
  rounds of mending Raw Input's account was kept for each keyboard, so that a key held
  on the one was not let go by a tap of it on the other. For three readings running,
  every finding of middle weight was in that telling apart, and each attempt at it
  made a worse rare case than the one it was built to mend: a modifier that stayed
  down on the machine with nothing said, where a keyboard dropped out and Windows did
  not say so, or a hold that gave way under a finger. A bare key is seen, and pressed
  again; a modifier that stays down on a server is not. So it was taken out, by the
  owner's decision, and nothing that names a keyboard is asked for or kept
  (`aKeyIsLetGoWhenAnyKeyboardLetsGoOfIt`, `aKeyHeldFromBeforeIsLetGoByAnyKeyboardsUp`,
  `theLeaveKeyGivesTheKeyboardBackByItsUpAsItIsHeardWhateverRawInputHasStillToSay`;
  control: `aKeyHeldOnOneKeyboardIsLetGoOnTheMachineByTheSameKeyOnAnother`,
  `aKeyHeldFromBeforeTheKeysWereHeardIsLetGoByAnotherKeyboardsTap`,
  `aKeyThatIsKeptBackIsKeptBackNoLongerWhenAnyKeyboardLetsGoOfIt`). The tests give the
  hook one stream of keys: only a person at two keyboards can see it.
* **A keyboard that goes** ends control. One that is unplugged, taken to another
  computer by a desk switch, or whose link is lost, with a key held, sends no "up" for
  that key, by either account (so Windows is taken to do): the key would stay down on
  the machine, Enter or Del said again there by the machine itself, with nobody told.
  So Windows' word for a device that is taken away is asked for with Raw Input
  (`RIDEV_DEVNOTIFY`; `WM_INPUT_DEVICE_CHANGE`). When a keyboard goes while keys are
  swallowed, the hook is closed (`Procedure::aKeyboardWent`,
  `KeySource::Lost::AKeyboardWent`): keyboard and mouse are given back, which lets go
  of everything on the machine, and the bar says why. Any keyboard that goes is
  enough, with a key held on it or none: none is told apart, and nothing of one is
  kept (the handle that comes with the word is asked what kind of device it is, and
  forgotten). A device that went and of which Windows no longer says what kind it was
  counts as a keyboard, since to take a keyboard for none is what leaves a key down on
  a machine; one that it still names as something else is none; and a keyboard that
  comes ends nothing (`meansAKeyboardWent`). What the hook's table has as down stays
  as it is, since nobody knows which of it was held there: such a key is kept back at
  the next taking, and named in the bar where it is a modifier, until it is let go or
  pressed anew. While nothing is swallowed, the word is nothing. Where the word
  cannot be had (Windows refuses the asking with it, and Raw Input is then asked for
  without it; there is no second account to ask it with; or that account is another
  window's hook's by now), whoever asks is told
  (`KeySource::isToldOfKeyboardsGoing`), and the bar says, while keyboard and mouse
  are held, that a keyboard which is unplugged is not noticed
  on this PC. Not covered: a keyboard that drops out with no word from Windows (asleep
  or out of reach behind a receiver that stays plugged in; a desk switch that keeps a
  made-up keyboard at each computer). Its key stays down on the machine until it is
  pressed and let go on another keyboard, or keyboard and mouse are given back. The
  plain part is tested with made-up notices
  (`whatWindowsSaysOfADeviceThatComesOrGoesIsReadForAKeyboardThatWent`,
  `aKeyboardThatGoesEndsTheSwallowingWhateverWasHeldOnIt`; control:
  `aKeyboardThatGoesGivesKeyboardAndMouseBackAndTheBarSaysWhy`,
  `whereThisPcDoesNotSayThatAKeyboardWentTheBarSaysSo`,
  `aModifierOfAKeyboardThatDropsOutUnsaidStaysDownOnTheMachineUntilItIsTappedElsewhereOrTheyAreGivenBack`).
  The asking for that word and the reading of it are `WindowsKeyboard`'s, in no test,
  and everything that is said of it is from memory: what Windows counts as a keyboard
  (a receiver for keyboard and mouse, a mouse with keys of its own, a headset or a
  dock may be one, and then goes as one), a hub or a dock that takes every device with
  it, a keyboard on Bluetooth, a remote desktop. Section 11.
* Where the two are not at one within the moment, this hook is closed: it swallows
  nothing ever again, whoever asks, and the next taking puts a new one in. Control ends
  with which of the two told of a key that the other did not (`KeySource::Lost`). Told
  of and not heard (`NotHandedKeys`): what a hook that Windows took out looks like at
  the first key after it, and a single key that Windows left out, an "up" among them
  (the "up" of a key that had been kept from Windows is then told as that); and also a
  key that another program's hook, or a key helper, held back. Heard and not told of,
  by an account that has told of keys of this keyboard since the hook went in
  (`NotToldOfKeys`): that account is told to this program no more, by a taking that is
  not followed here, or something makes or changes keys on this PC. Or both about then
  (`ToldDifferently`). Which of the causes it was is not known here and not claimed:
  each has a sentence that says what was seen, what may be why, and what can be done
  (`MachineControl::sentenceFor`).
* Where this PC gives no second account (Windows refused it, or none comes, as may be
  so over a remote desktop): the first key that is swallowed waits its moment for a
  word that does not come, and goes on then; from there nothing waits, no hearing is
  noted, the watch is silent, and a hook that Windows took out is not found. Whoever
  asks is told (`isWatched`), and the bar says so for as long as they are held. It is
  taken to be so only while the second account has told of no key since the hook went
  in: with the first key it tells of, the comparing begins (`beginComparingAt`), and
  what is down at that moment is left out of it until it has gone up.
* Where that account begins to tell late, on a PC that was taken not to tell (a
  keyboard that it tells of was plugged in), hearings are noted from its first telling
  on. A telling that comes within the gate's moment of that first one and finds no
  hearing may be the word for a key that was heard before any hearing was noted, or
  for one that went down since with the hook not called for it: nothing was noted to
  tell the two apart by. It is not forgiven. It is held against the hook as any
  other, and control ends, once, with a word of its own that says that much and no
  more (`KeySource::Lost::BeganToTell`); behind its sentence the bar says when
  Control can be clicked again. What that costs: on a PC that begins to tell while a
  machine is controlled, with a key in flight at that very moment, keyboard and mouse
  are given back once, and are taken again. Forgiven, such a telling would let a "c"
  go to the machine bare where its Ctrl went down unheard in that moment; and to
  forgive only what was heard before is not to be had, since of that nothing is noted
  (`whereTheOtherAccountBeginsToTellLaterAKeyHeardBeforeThatEndsTheControlOnceWithAWordOfItsOwn`,
  `aKeyThatIsNotHeardInTheFirstMomentAfterRawInputBeginsToTellEndsTheControl`,
  `anUpThatIsNotHeardInTheFirstMomentAfterRawInputBeginsToTellEndsTheControl`).
* Windows tells one window of a program of the raw keyboard: the one that asked last,
  which is the hook of the window that took keyboard and mouse last. An older hook,
  which is in only until what it kept from Windows was let go, finds on its own thread
  that the account is another's, and compares nothing from then on.

**How long Windows waits for a hook.** 300 ms, unless this PC's registry names a
shorter time (`LowLevelHooksTimeout`, read once and never written; `windowsWaitsFor`).
No longer time is ever believed, and neither is none, 0, or what is no number: each is
the 300 ms. To believe a longer wait than Windows keeps is what hurts: a key that
Windows took for itself meanwhile would count as in time here, be typed into both, and
its "up" be kept from Windows. To believe a shorter one only ends control sooner. A key
that has waited to within 50 ms of that time since Windows stamped it may be Windows'
already (`waitedMs`, `inTimeUnder`; half of the time, where the registry names a tenth
of a second or less): it is not swallowed, is not the machine's, and control ends as
keys that fell behind. And a key that has waited a second since it was
heard, on its way to the interface thread, is not typed late, nor is anything behind it
(`lateAfterMs`, `whatToTell`): what is typed goes to a machine now or not at all. The
gate's moment and one look of the hook's thread are half of that second at the most,
whatever the registry says (a `static_assert` beside the constants).

**What stops the swallowing, in each failure.** No failure of the program may leave the
keyboard of whoever sits at the PC with a machine.

| What happens | What stops the swallowing | When |
|---|---|---|
| The interface thread hangs | The procedure. A key that finds the interface thread's word older than a second is Windows', and so is every key after it: the hook is closed and marked as fallen behind, and nothing more is put on its way to that thread but the going up of a key that had been kept from Windows. Silent for ten seconds, and the hook's own thread takes the hook out (`hungAfterMs`) | At the next key, within a second. Control ends, as keys that could not be passed on in time, when the interface thread is back |
| The hook's thread hangs | Windows, which hands a key on without the hook once it has waited for it. And the interface thread, which asks four times a second when the hook's thread last looked (`LookWatch`) | Key by key, after Windows' wait. Control ends when that stamp has stood still for three seconds (`NotLooking`); of a stretch in which the interface thread did not ask either, no more than one second counts |
| Windows takes the hook out, or leaves it out for a key | A hook that is out swallows nothing. That keys are not heard is found by the second account | At the first key after it, within the moment and a look. Not found where this PC gives no second account |
| Windows shows a screen of its own: the lock screen, Ctrl+Alt+Del, a question about administrator rights | The keys that Windows itself acts on, as the last of them goes down: L under a Windows key, Del under Ctrl and Alt (`KeysHeld::isTakenByWindows`), heard by the procedure or told of by Raw Input (`KeysOfWindows`). And Windows' own words, on the hook's thread: another desktop is shown, and the session is locked, unlocked, connected or disconnected, here or from afar (`AnotherDesktop`) | At that key, before it could go to a machine; or at Windows' word. The hook is closed: what is let go on such a screen is heard by nobody |
| Another program types into this PC | Nothing has to: such a key is never swallowed | Always |
| Whoever sits there wants out | The leave-key, decided in the procedure itself: before the gate, and before the interface thread hears of the key | At its "up" |
| A key comes after Windows has stopped waiting for the procedure, or finds no room to be told of | It is not swallowed, and the hook is closed, as fallen behind | At that key |
| A keyboard is taken away from this PC, with a key held on it or none | Windows' word for it, on the hook's thread: the hook is closed (`AKeyboardWent`), and everything on the machine is let go. Not where Windows has no word for it (a keyboard that drops out behind a receiver which stays plugged in), and not where this PC will not give that word, which the bar says while keyboard and mouse are held | At that word, while keys are swallowed |
| The two accounts differ, at whichever time: also in the first moment after the second one began to tell late (`BeganToTell`) | The hook is closed | Within the moment and a look |

A hook that was closed stays closed. What the interface thread tells whoever listens,
each time it is woken, is made in one place (`whatToTell`): why nothing is heard any
more first, and one reason only, the one that is known best (the keys that Windows acts
on, before a screen of Windows' own, before a keyboard that went, before the two
accounts that differ, before keys that fell behind); then the keys, in the order they
were handed on. Each key is asked,
as it is taken, whether the hook has been closed by then. The hook's thread goes on
meanwhile, and a press that it handed on in the very call that closed the hook comes
behind reasons that were asked for a moment too soon: the gate found the two accounts
to differ over a key before it and dropped that key, and a "c" whose Ctrl was dropped
would reach the machine bare. Such a press is not typed, nor is what is behind it: of
those only a going up is told, and why is asked for once more there and said before
what follows. Whatever closes the hook marks why before it closes it.

The telling itself is `Telling`: what one pass made is said to whoever listens through
the key source's own signals, one thing at a time and in its order. Whoever is told of
a key may ask, from inside that telling, for everything that waits to be told
(`KeySource::tellNow`: it found that nothing is swallowed any more, and was not told
why). What the pass has not said yet is kept for that, and is said first, there and
then, the reason among it, and not a second time afterwards. So why control ended is
shown also where the reason comes behind a key in the same pass: said only once the
key's telling had returned, it would find keyboard and mouse given back with no reason
already, and be dropped (`whyControlEndedIsSaidWhereItComesBehindAKeyInTheSamePass`
in `ui/tests/test_control.cpp`).

**The leave-key** defaults to right Ctrl (`input::usageRightCtrl`). Settings > General
also offers Scroll Lock and F12; the local settings file validates the stable key name
and falls back to Right Ctrl for missing or invalid values. Pressed and let go with no
other key going down between, it gives keyboard and mouse back. The rule is in two
places on purpose. `LeaveWatch` runs in the procedure, for every key a finger pressed,
so that it holds whatever has become of the rest of the program. `ControlKeys` runs on
the interface thread, and knows what a key under the leave-key is: Del is Ctrl+Alt+Del
on the machine; any other key does nothing, and is the leave-key's until it was let go
itself. Nothing of such a key is ever sent: not when Windows says it again, and not
after the leave-key was let go, which whoever uses the right Ctrl as a Ctrl does first
as often as not; sent then, the machine would hold the key bare. A key that is only
said again is no key going down, in either place: a second keyboard that goes on saying
its key does not keep the leave-key from leaving. A leave-key that is held as the hook
goes in, or as they are taken anew, is not the way out until it is pressed anew. Where
only the hook heard the leave-key, `MachineControl` finds that nothing is swallowed, at
its next look or over the very key that is told. Before it makes anything of that it
asks the key source whether it knows a reason (`tellNow`: keys that fell behind while
the interface thread was held up look the same, and the word for it may be on its way
behind that key). A reason that is known is the one that is said; only where there is
none are the machine's given back with nothing said.
Whether the keyboard that is plugged in has a right Ctrl cannot be asked of Windows, so
the bar names "Give back" beside the selected key, always. `ControlView` receives the
shared setting in every window; a change gives back control and cancels any pending
request before it advertises the new key. `MachineControl` passes that key to both
`ControlKeys` and `KeySource::begin`. A Windows hook with a different release key is
replaced before it can swallow again. Its held-key check and the relative pointer's
independent watchdog use the same Windows key mapping (`ui/win/leave_key.h`). The key
is fixed for each capture; the pointer's old watchdog is joined before it changes.

**Always on top.** The console's idle toolbar offers a per-window switch. `Main.qml`
applies Qt's `WindowStaysOnTopHint` only while that window shows a console, is outside
full screen and display mode, and the app is unlocked. Leaving those conditions clears
the choice, including return to the wall and sign-out. It is not persisted or copied
to new windows. Changing it never takes keyboard or mouse control.

Ctrl+Alt+Del, by the bar's button or by the leave-key with Del
(`MachineControl::ctrlAltDel`): the three are said down in the order a hand presses
them, whatever is held, and after they are let go what fingers still hold is as it was
before (a modifier that was down on the machine is down there again).

**Keys kept back.** `MachineControl` sends a key only where the machine has the
modifiers that the hand has: one that is down here and up there would make the next key
arrive bare. Kept back, each until it is let go here:

* a key that is down when keyboard and mouse are taken (`KeySource::whatIsDown`: what
  the procedure heard go down, and what Windows has as down from before, where the name
  Windows has is one physical key's alone);
* a key whose press could not be kept from Windows (Windows had it already): it was
  typed into this PC;
* a key that was kept from Windows in an earlier time of holding them, in whichever
  window, and not heard to go up since (`KeysKept`, below). That one until it is heard
  to go up, however long that takes, or seen to go down anew: a "down" after it has
  been silent for longer than a held key that Windows says again ever is (three
  seconds, `repeatsWithinMs`), with the keys heard all that time. The finger had left
  it where nobody heard, as on one of Windows' own screens;
* and while one of the eight modifiers is kept back, for any of these reasons or as a
  key that went down under the leave-key, every other key that is pressed, in its
  turn, and every click.

Every "up" of a key that is down on the machine is passed on, whatever else is so. A key
that is down there stays down for as long as it is held here, however long nothing is
said of it. And nothing is eaten without a word: while a modifier is kept back, a line
in the bar names it and says what ends it (`MachineControl::note`), in one sentence for
those held from before and one for the others.

**What is held** (`KeysKept`). Windows does not know of a key that was kept from it, and
a hook that goes in later did not hear it go down. What is physically down is one thing
for the whole program, so there is one memory of it: `WallWindows` owns it, and hands it
to every window's `MachineControl`. A key goes in when its press was kept from Windows,
and out when its "up" is heard, by whichever window's keys, or when it is seen to go
down anew. As the swallowing stops, whatever the ending, the key source is asked which
keys it has kept and not heard go up (`whatIsKept`), beside what it told: a press that
was kept in the very moment in which keys were heard no more is told to nobody. While a
machine is controlled nothing is forgotten. A minute after the last window gave
keyboard and mouse back, everything is: no finger stays on a key for a minute, and
nothing of the keyboard is kept by a program that controls nothing.

**The Windows key** is said to the machine only when what comes next shows that it is
no Win+L: another key that is not L, or a button of the mouse, going down (the Windows
key first, then that), or the Windows key going up (down and up together). With L,
Windows locks this PC, whoever the two were meant for: the procedure ends the
swallowing at that L, and nothing of the two has reached the machine.

**Clicks behind keys.** A key may be told a moment late (one that goes down waits in the
gate, and a modifier that goes up stays behind a key that was pressed under it), and
the mouse comes by another way, through the window. And what is told is not told in the
order it was heard in: a key that goes up, or is only said again, may pass one that
waits. So every key that is heard and is to be told, going down or up, has a number
while it is on its way to the interface thread, in the order the keys were heard in,
and is told with it (`Heard::number`, `KeySource::keyChanged`); the numbers begin at 1
again whenever none is on its way, and say nothing of how much was typed. Which number
was given last, and how many keys are on their way, is asked of the key source
(`KeySource::onTheirWay`, from `Procedure::onTheirWay`). A button that is pressed, or
the wheel turned, while keys are on their way waits in `MachineControl` until every one
of those was told, an "up" as well as a "down", and goes to the machine after them,
with where the pointer was: Shift and then a click is no bare click, and Ctrl let go
and then a click is no click with Ctrl. Which keys those are goes by their numbers, not
by how many keys are told: a key that was heard after the click and is told sooner is
none of them.
What the mouse does behind that waits in its order; a button goes up at once where its
press has gone out. A few at the most (32), and none for long: where there are too
many, or one has waited a second, keyboard and mouse are given back as with keys that
fell behind, and what waited is dropped. Which came first, the key or the click, is
taken from when each is seen on the interface thread.

**Asking.** "Control" asks the live (`Live::askControl`), and they are held from its
yes. The machine has 65 seconds to answer (`answerWithinMs`), covering the server's 60-second admission deadline and transport time. A yes is taken only
while whoever asked still waits for it: not after that time, not after the window was
no longer in front, and not after a key was pressed in the window or something else
was opened in it (`wentOn`). Why they are no longer waited for is said at once, as
what it was, and a yes that still comes is given back at once: what is typed by then
is meant for something else.

A line says no number with its words: a yes, or a no with or without a sentence, and
nothing of what it answers. So what the machine was asked and has not answered is kept
as a row, in the order it was asked (`MachineControl::OwedWords`, eight at the most):
its answer to an asking that is over, and its word for each giving back. And no asking
is made while any of that is owed: the answer to an asking made then could not be told
from the word that is owed. The screen is told so (`ControlView::busy`), and the
button that asks does nothing until what is owed has come, or has run out. From that,
by how it is made:

* While an asking waits, nothing is owed from before it, and the first word the
  machine says is that asking's answer. A yes takes them. A no is shown, in the
  machine's own sentence where it has one, and of that asking only the word for the
  giving back that follows is owed from there.
* While no asking waits, whatever the machine says is nobody's, and is held against
  the row. A yes is given back at once, and the machine's word for that giving back is
  owed in its turn: keyboard and mouse are never held on a machine with the bar saying
  that they are not. A no with a sentence takes the place of "did not answer" where
  the asking's time was over (but for a no with the code `given_up`, below);
  otherwise nothing is shown of it.
* Nothing is owed while they are held: a no then is theirs, and takes them away.

Which of what is owed has come is told by what is known of how a machine answers. It
answers in the order it was asked, so once it has said something, whatever was asked
before that is owed no more. A yes answers an asking, never a giving back, and so does
a no that comes with a sentence: that its word for a giving back has no sentence is so
of a KVM of this PC's own, and is assumed of a server. A no without a sentence is
taken for the first of what is owed, whichever that is. One thing is not told apart by
this: that such a no is the word for the giving back, with no answer to the asking
before it ever to come. The word that is then still taken to be owed runs out its
time. And a no with the code `given_up` is the machine's answer to an asking that was
called off from here before it had answered: the answer that is owed, like any other,
and nothing to show. Its sentence says only what was done here, and is shown nowhere.
Why the asking was given up was said when it was, and stands: "did not answer", where
it was the time.

Each word is owed for a time and no longer (`owedForMs`, five seconds), from when the
machine can first have said it: its word for a giving back from when they were given
back; where the giving back ended an asking, from when that asking's time is over,
since its answer comes first; and once the machine has said something, from then. So
the button is busy for no longer than ten seconds from an asking and five from a
giving back, and for five more where a yes still comes and is given back; what is owed
is looked at again when the first of it runs out, and the screen is told. A machine
that says its words has the button back the moment they have come. A KVM of this PC's
own says a word for every asking and every giving back (section 4, `KvmdLive`): a
giving back while keyboard and mouse were still being given is answered by a no with
`given_up` and then by the word for the giving back, and a giving back by a window
that holds nothing by the one word. So what is owed from it comes a turn of the loop
later, nothing has to run out, and after every ending Control is offered again within
a turn of the loop: an asking that was given up, stopped, refused or not answered in
its time, keyboard and mouse taken away, a plain giving back
(`afterEveryEndingControlIsOfferedAgainWithinATurnOfTheLoop` in
`ui/tests/test_control.cpp`, with a stand-in machine that says every word, as that
driver does). A server's line passes on what the server says, which may be less, and
the time is for that.

What the bar says of it (`MachineControl::problemAsItIsSaid`): behind why an asking
was given up from here (a key was pressed, something else was opened, the window was
no longer in front), and behind the sentence for a second account that began to tell
late, that Control can be clicked again once the machine has answered, or after the
time it is given for that; and "Click Control again" once nothing is owed. Nobody is
told to click what does nothing. Behind every other ending, and where nothing else was
said: once a word has been owed for half a second (`owedIsSaidAfterMs`: longer than a
machine that says its words takes to say them, so that the bar does not change twice
at every giving back), the bar says why Control is not offered, in one sentence that
holds whatever the giving back came of: the machine has still to confirm that its
keyboard and mouse are no longer held or asked for here, and Control can be clicked
again once it has, or after a few seconds (`whyControlIsNotOffered`). Only where
Control is there to be clicked: of a machine that is not had live any more, or to
whoever may not act, nothing is said of it. The sentence goes when nothing is owed
any more (`whereTheMachineSaysNoWordTheBarSaysWhyControlIsNotOffered`,
`whyControlIsNotOfferedIsSaidOnlyWhereItCanBeOffered`, with a stand-in machine that
says no word). With the driver for a KVM itself beneath `MachineControl`, only a plain
giving back is in a test: section 11.

**Everything up.** Every way they end goes through one function,
`MachineControl::stop`: they are given back; the leave-key; the window no longer in
front or out of sight; another machine or none shown large; the live gone, or saying
that it holds them no longer; whoever sits here no longer allowed to act; the key
source fallen behind, hearing nothing any more (a keyboard taken away from this PC is
one of the reasons for that), or no longer looking; what the mouse did waiting too
long behind a key; nobody at the keyboard or the mouse for fifteen minutes, as the
web's (`idleAfterMs`: control is not to be what leaves a keyboard on a server with nobody
there, and that is not left to Windows' lock alone); and the window ending. First the
machine: everything that is down there is let go (`Live::releaseAll`, by the live's
own account of what it said), and the live is told. Then the key source stops
swallowing, and what it still keeps from Windows is noted (`KeysKept`). Why they were
refused or taken away stands in the bar until they are next asked for, or another
machine is shown. Where `MachineControl` finds that the key source swallows nothing
any more and was not told why, it asks for the reason before it gives them back (the
leave-key, above). The PC locking is heard of on the hook's thread: that is in no
test, and nothing is known of it from the hook's one real run. Nothing of its own is
there for sleep.

**Something may still be down** (`Live::leftDown`, section 4). `MachineControl` hears
it from the live its window has, and the bar says it: behind why keyboard and mouse
went; or, where the word comes while they are held, as a line of that holding. It may
come seconds after they went, and they may be held anew in the same window by then: it
is said all the same, in a holding it is not of, and takes nothing from it. It goes
when they are taken anew (whoever sits here has the machine's keyboard again, and can
press and let go what stayed down), or when another machine is shown.

**The mouse.** Absolute, from where the pointer is over the picture and not over the
bars beside it (`inPicture`): sixty places a second at the most, the last one winning;
five buttons; the wheel, a notch at a time. This PC's pointer stays in sight. What
another program does with the mouse over the picture goes to the machine as a person's
clicks do: that is not told apart, as what a program types is.

Relative mode is an explicit choice before taking control. `RelativePointer` waits for
the toolbar layout, fits the remote picture inside its tile and converts that area to
physical screen pixels with the window's device ratio and Windows' client origin.
`PointerCapture` registers foreground raw mouse input and confines the pointer; QML
hides it only after capture succeeds. Absolute tablet/remote-desktop packets are ignored.
The existing Qt mouse path still carries buttons and wheel. `MachineControl` aggregates
relative movement, preserves its order with gated keys and clicks, and divides large
movements into bounded transport messages without losing distance. No waiting input
survives the end of control. Changing machines resets the relative-mode choice.

A KVM of this PC's own that runs kvmd is not switched to another mouse mode: its
`mouse_output` is a setting of the device that its other users share (as the web app's
`pointer_kind`). `KvmdDevice` reads `mouse.absolute` from `/api/hid` at
every look, and `KvmdLive` says it as the live's pointer mode (absolute, relative, or unknown
where the KVM does not say) and says it again when a look finds it changed; `MachineControl`
follows it, and sends places to an absolute mouse and steps, by pointer capture, to a relative
one (kvmd drops a move of the other kind). A person who wants the other mode sets it on the KVM's
own page. This follows PiKVM's [mouse modes](https://docs.pikvm.org/mouse/) and
[HID API](https://github.com/pikvm/kvmd/blob/master/kvmd/apps/kvmd/api/hid.py);
GL.iNet's [published HID implementation](https://github.com/gl-inet/glkvm/blob/master/kvmd/plugins/hid/otg/__init__.py)
also reports the mouse mode. The desktop implementation is written for this project; no device
code is copied.

The selected release key, focus loss, hiding, changing the picture's physical bounds,
control loss or destruction releases capture. A separate watchdog releases confinement
if the UI heartbeat stops for 1.5 seconds; it also checks foreground ownership and the
selected release key (Right Ctrl by default, or Scroll Lock or F12).
The Windows adapter refuses to replace an existing raw mouse registration and removes
only its own. It frees confinement only if the current rectangle is still its own.
The API contracts are Microsoft's [raw input registration](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-registerrawinputdevices),
[mouse packet](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-rawmouse)
and [cursor confinement](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-clipcursor)
documentation. Tests inject all Windows mouse operations; real hardware acceptance is open.

**Screenshots.** `ScreenCapture` accepts an explicit save/copy request only in an unlocked,
visible, actionable enlarged console without active or pending control. `VideoHost` asks
the scene on the render thread, inside Qt's external-command boundary. The scene draws
the current decoded drawable into a private native-resolution canvas and reads its BGRA
pixels. It restores the render target and viewport and does not consume a queued frame,
change presentation statistics or include QML overlays. Missing pictures and lost devices
return an error. The existing decoder and canvas suffice; no new capture dependency is used.

The GUI receives an owned image. A generation, machine and stream check rejects stale
callbacks after a switch, lock, hide or control request. A five-second timeout ends a
request whose render callback never arrives. Save uses the native PNG file chooser and
an atomic write, rechecking authorization after its nested event loop; copy writes an
image only on request. A save dialog keeps its window and update restart alive until it
returns even if the request has been canceled. No clipboard watcher or image history exists.

**Clipboard typing.** `ActionsView::loadClipboard` reads plain text only when explicitly
asked from an allowed Type text form, checks the 2,000-code-point limit and complete
UTF-16, then emits it to that form. A changed or closed form discards the result. QML
shows the text, clears Enter, and uses the existing confirmation and action path.
Closing, hiding, minimizing or locking clears the form. There is no clipboard watcher
or automatic send; tests inject a clipboard reader and use temporary profiles.

## 4. The core

* **`ServerClient`.** Every call to the server in one class: the address, the
  certificate, the session, the `X-QKVM` header, a time limit and a size limit on every
  call, strict reading of JSON into types. Nothing else in the program builds a URL.
  Every call but the first is under `/api/v1/`. A call has two limits on its time: how
  long nothing may come, and the whole of it, from the question to the last byte of its
  answer, which may take twice as long (`finish`, `wholeWithinTimes`). What keeps the
  second goes with its answer, and does not live out its time behind every picture
  of a wall (`limitsAlive`). An answer that comes a byte at a time never reaches the
  first; given up on at the second, it is
  told as a server that did not answer (`desktop:unreachable`), and what had come of
  it is no answer. Nor is a good answer (2xx) that broke off before all of it had
  come, the line fallen under it or silent: what came is not read, since half a
  picture would be handed on as a picture, and it is told as a server that did not
  answer too, which leaves open whether something that was asked of a machine was
  done (`anAnswerThatBreaksOffIsNoAnswer`). And of something that was to be done to a
  machine (`act`) only the server's own no is a no. An error of what stands before
  the server (500 and above, in anything but the server's own shape: a gateway's 502
  or 504 in the middle of a long action), an answer that was cut off for its size,
  whatever its number, since nobody read whether it was a no, and a yes that was
  said with what is not understood, are told as a server that did not answer: "may
  have been done" (`mayHaveBeenDone`), as the driver for a KVM has it, and never a
  refusal after which the button would be offered again. So are the server's own
  `timeout` (504) and `dropped` (502), in its shape: what it sent to the machine went
  out, and no answer came, or a broken one. And so is its own 500, whatever its word
  (told as `internal`, its sentence kept): its middleware answers that for any error
  in a handler, one raised after a press went out among them (the page's
  `unanswered`). Its other words of 502 to 504 for the machine
  (`unreachable`, `no_stream`) say that nothing was sent, and are refusals.
  A 401 is the news that the session is gone (`desktop:signed_out`) only where it
  comes in the server's own shape, and not for a sign-in, where it is the server's
  word about what was typed. In any other shape it was said by something that
  stands in the server's place and wants a sign-in of its own: a refusal that is
  not the server's (`refused`), contact lost (below), and nobody is signed out by
  it. And a picture is taken only where its answer says that it is a JPEG or a PNG
  and begins as one (`snapshotFrom` in `server_types`; which of the two it says and
  which it is need not agree, since the engine goes by how it begins). The server
  sends no other kind, which was checked against its source: a yes with anything
  else was said by something else (`bad_answer`).
  HTTP/2 is not used with a server (`Http2AllowedAttribute` set to false), as with a
  KVM: one way of speaking, the one for which Qt tells when a question has gone
  out, and on that hangs what is given up where contact is lost and whether
  something may have been done. Set, and not shown by a test: the tests' server
  speaks no other way.
  The first call to a server is `askHealth` (`GET /healthz`, without the session): what
  answers and is no QKVM ends as `bad_answer`, a QKVM that does not list the version of
  the API this program speaks (`spokenApi`) as `desktop:other_version`, and nothing more
  is asked of either. `ServerSource` asks it when a server is named, and again before
  anything that says who asks goes there: before a kept session goes with the first
  question and before every posting of a password (`ifAQkvmAnswersThere`), before a
  display link is handed in again (`comeBackByTheLink`), and, while the wall is up,
  before the wall as a file posts what was typed for it (`ifAQkvmStillAnswersThere`:
  section 5, "The wall as a file"). The QKVM that was asked what it is may have made
  way for something else since, and a password goes to nothing but a QKVM this
  program speaks to.
  After contact was lost, the session goes to nothing that has not said what it is
  either. That rule is the client's own, because the session is: every call that
  carries it passes one function (`ask`), and none has to remember. Contact is lost
  (`lostContact`) by what says something of who answers, and by nothing else. That
  is no answer at all: the line was refused, it fell (also under an answer that had
  begun), or nothing came in a call's time. And it is an answer that is no QKVM's
  where a QKVM's always has a shape: a refusal that is not in the server's own shape
  (a gateway's page, another program's, a 401 among them: `isRefusalOfAServers`); an
  answer other than a picture that is cut off for its size, or that dribbles for
  longer than a whole call may take; a wall, a list of how the machines are, or a
  session that is none; a picture that is none (above); a yes to something that was
  to be done, said with what is not understood; and what `/healthz` is answered with
  that is not the word of a QKVM this program speaks to. It is not lost by a
  picture that is larger than is taken, or that the server is too slow to give: a
  machine's screen may be that large, and the server fetches the picture from the
  machine while it is asked. That picture fails, and nothing else (`askSnapshot`
  marks its question, and `finish` looks at the mark).
  From then on a call that would carry the session first has `/healthz` there
  asked, with nothing that says who asks (`askWhatAnswersNow`), and calls that come
  meanwhile wait for the one answer. Behind the yes of a QKVM of this program's
  version they go out: a yes to a question that was asked after contact was last
  lost (`m_lost`), whoever asked it, the wall's keeper before a password as well as
  the client for a call that waits. One that was asked before that may be the last
  word of what was there then, and brings nothing back. Otherwise the calls are not
  sent, and are told so in a code of the program's own (`desktop:not_sent`: no
  refusal, and nothing that may have been done; a certificate that is not the
  server's, and a QKVM that speaks another version of the API, are told as that),
  and the next call asks again: a server that is away is asked what it is as often
  as it was asked anything before, and no oftener.
  What has left when contact is lost, and has its answer begun or is a POST, is not
  given up. Its session went with it: giving it up would take nothing back, and would
  end what a QKVM is about to answer, an action that takes its time among it. (Given
  up, as every question that only asks was, one machine's picture that was too slow
  or too large took the look at the machines with it: the wall counted as away, and
  the machine that was shown large lost its live line, keyboard and mouse with it.)
  The rest is taken back, a turn later (`takeBack`): what has not left this PC yet,
  and what has left with no answer begun where it is of what Qt sends again by itself
  (a GET, a PUT). Why: Windows goes on trying to reach a place for seconds on behalf
  of a question (some four on this PC, where nothing listens any more), and Qt keeps
  what it has no line for, and either would hand it, session and all, to whatever
  answers at the address by then. And where a line falls before an answer, Qt sends a
  GET, and a PUT (the wall's order), again inside itself, on a new line, and the
  program hears nothing of it, nor that the new line is being made; a POST it sends
  once. That new line is one Windows tries to make for those seconds. So a server
  that went with a picture out (it was still fetching it from the machine) had Qt
  send the picture's question again, session and all, to a program that began to
  listen at its address meanwhile: seen in a full run of the session's tests, as the
  first question that newcomer was asked. An
  answer that has begun ends that: Qt sends nothing again from then on, and the
  client notes it where the answer's head comes (`answerBegun`). Of what is taken
  back, a PUT, and a POST that had not left, are given up, which keeps a password, an
  action or an order from following a server that was replaced; each is told as a
  server that did not answer (`desktop:unreachable`). An order taken back after it
  left may have been saved by the server all the same, and is told as failed: to be
  said as "may have been saved", in a later round. A question that only asks is asked
  anew behind the question for what answers there now, and whoever asked hears
  nothing of it, so that the wall's own look is not lost for it; three times at the
  most for one question, after which it is told as given up. A POST that has left is
  never taken back, nor asked again: Qt does not send it twice, and what it does may
  be under way. (For one round of mending only what does something was taken back. A
  look or a picture that waited for a line then went, with the session, to a program
  that had taken the server's place: a test of the session that says it may not failed
  one run in ten for it.) So the rule does not keep back: whatever had its answer
  begun or was a POST when contact was lost, the call that found it lost among them;
  what Qt gets a line for before the taking back has reached it (a turn of the loop,
  and then the way to Qt's own thread), or sends in the moment before it says that a
  question left; and what Qt sends again by itself before contact is seen to be lost.
  Nothing of `QNetworkRequest` or of `QNetworkAccessManager` turns Qt's sending again
  off in Qt 6.12, and a sender of the program's own that never sends twice, as the
  line to a KVM has one, is not built (section 11). Over HTTPS none of it reaches what
  is not the server: every line, one Qt makes anew by itself among them, has the
  certificate looked at before a byte of a question goes out (below). Over plain HTTP
  what the rule does not keep back reaches whatever listens at the address by then,
  with the session.
  (Tried, in `core/tests/test_server_client.cpp`:
  `afterContactWasLostNothingCarriesTheSessionBeforeAQkvmHasSaidWhatItIs`,
  `whatIsOutWhenContactIsLostIsNotGivenUpAndWhatIsAskedNextWaitsForWhatAnswers`,
  `aPictureThatIsTooLargeOrTooSlowFailsAloneAndContactIsNotLost`,
  `a401InAnotherShapeThanTheServersIsNotTheNewsThatASessionIsGone`,
  `whatIsNoPictureOfAKindThisProgramDrawsIsNoQkvmsAnswer`,
  `whatAQkvmSaidOfItselfBeforeContactWasLostDoesNotBringContactBack`,
  `aQkvmThatComesBackSpeakingAnotherVersionIsToldAsThat`, and for what Qt asks
  again by itself, which is held there so that another Qt is noticed,
  `whatQtAsksAgainByItselfWhereItsLineFellBeforeAnAnswer`. That what has not left
  is taken back is tried with a question that only asks,
  `whatHadNotLeftWhenContactWasLostIsTakenBackAndNotHandedToWhateverAnswersThereNext`:
  asked of an address where nothing answers, a question is on its way for as long
  as Windows tries, and something begins to answer there meanwhile: another
  program, or the QKVM again; and the question only asks, or was to do something.
  Each of the four is noticed where the taking back is left out for it, and where
  a question that was taken back is not asked anew. That what Qt was sending again is
  taken back is tried with a picture the server is still fetching when it goes,
  `whatQtSendsAgainByItselfToWhatIsNotThereIsTakenBackWhereContactIsLost`, in two
  rows: contact lost before the picture's line falls, and while Qt tries to make the
  new one; then something else begins to answer at the address, and is asked nothing
  that carries the session. It failed every time before the mending, the newcomer
  asked for that picture with the session. Not tried: one that is taken back after
  somebody was signed out, or after another server was named, and the three times;
  a PUT taken back after it left. Section 11 has what reviewers found in this, and
  what is open.)
  Over HTTPS a server is one of two. Either Windows trusts its certificate for its name,
  and it is known by no certificate here. Or it is known by one (`setKnownCertificate`,
  its SHA-256 from the settings), and then it has to show exactly that one, which is
  looked at before a byte of a request goes out, in the shape that is described at
  `KvmdDevice` below: Windows is given nobody to vouch for anybody, its objections are
  passed over only for the certificate the server is known by and only where every
  objection names that certificate itself (`sslErrors`), and the second look, when the
  two have agreed on how to speak, stays (`QNetworkReply::encrypted`). A server that is
  neither hears nothing; the call ends with `certificate_unknown` or
  `certificate_changed` (or `certificate_signer_unknown`, for the certificate it is
  known by where somebody signed it whom Windows does not know), and what the server
  showed can be asked for. TLS is Windows' own (Schannel) on every PC: Qt would take
  OpenSSL where it finds one, and the program would then trust differently from one PC
  to the next. It is chosen in one place (`core/tls`, `useWindowsOwnTls()`), which
  everything that speaks TLS calls before it does, and before it makes anything of
  Qt's that could speak TLS: this client, the driver, the probe and the lines
  (`DeviceLine`, `LiveLink`). A socket takes Qt's own pick when it is made, so in the
  two lines, whose socket is a member, the choice is a member that stands before the
  socket. It answers whether Windows' own is what is in use. Where it is not (TLS was
  in use before anybody called it, with something else; or the part of Qt that
  speaks Windows' own is missing from this copy of the program), nobody judges a
  certificate as Windows does, and the program speaks to nothing over HTTPS. Each of
  the five refuses an address that is spoken to over TLS before anything goes out
  (`refusedForItsTls`, a plain function of that answer and the address), in one code
  of the program's own and one sentence, which says that this copy of the program
  is damaged and is to be installed anew (`damagedTls`, `desktop:tls_damaged`). A
  server at such an address is not named (`setAddress`); a KVM there is told as
  something to look at (`error`), not as one that is away; the probe asks nothing
  there (`KvmProbe::Kind::NotAsked`), and an adding or a look at a certificate ends
  with that code. Plain HTTP is spoken as ever. Tried in two test programs:
  `core/tests/test_tls.cpp`, where a line to a KVM is the first thing made that could
  speak TLS, and `core/tests/test_tls_other_first.cpp`, a process of its own in which
  another TLS was chosen first. What is not there yet is in section 11.
* **`KvmdDevice`** (with `kvmd_answers`). One KVM of this PC's own that runs kvmd, spoken
  to by the program itself (SPEC.md section 6.3): signing in, how it is, a picture, its
  stream of pictures, what is done to it, and the line for keyboard and mouse. It
  answers in `ServerClient`'s types and in the server's driver's words (`Status`,
  `Snapshot`, `Done`; "Device is unreachable"), so that nothing above it can tell which
  of the two it shows. This PC's own wall has one for each of its KVMs (`LocalWall`,
  below). The readers of what a KVM answers and the writers of what it is asked are
  plain functions in `kvmd_answers`, apart from the line they travel on. What it keeps to:
  * The certificate is `ServerClient`'s rule, per KVM: trusted by Windows for its name, or
    exactly the one it is known by, looked at before a byte of a question goes out. Each
    KVM has a `QNetworkAccessManager` of its own, so that no line made for one KVM is
    ever used for another. For a KVM that is known by a certificate the look is made
    where Windows objects (`sslErrors`), and Windows is given nobody to vouch for anybody
    (`knownByItsCertificateAlone`), so that it objects to every certificate, the KVM's
    own included. That matters because Qt makes several lines to a place at once and
    pairs a question with a line only when the line is up: what Qt is told to pass over
    holds for all of them, and a line that comes up with no question waiting is looked
    at by nobody later (`encrypted` goes to a question that waits, and to no other). So
    Qt is told to pass over Windows' objections only for the certificate the KVM is
    known by, and every other line ends there. The second look, when the two have
    agreed, stays. (`ServerClient` has the same shape; what of it is tried for which of
    the two: section 11.)
  * Qt signs on to nothing by itself (`AuthenticationReuseAttribute` set to manual): asked
    in some ways (NTLM, Negotiate), it would answer with the account of whoever is
    signed in to Windows and send the question again.
  * Straight to the KVM (no proxy), followed nowhere (a KVM that says "go there" has
    given an answer that is none), Qt's store of cookies neither written nor read, the
    answer asked for unpacked, and a packed one refused: the program unpacks nothing a
    device sends.
  * Three limits on a question: to be let in at all, the longest nothing may come, and
    the whole of it. "Not there" (`unreachable`) is only what certainly never left this
    PC: a place that refused the line or has no address, or a question of which Qt would
    have said that it went out (`requestSent`) and has not. Everything else that got no
    answer "may have been done" (`KvmdDevice::mayHaveBeenDone`), and is never sent again:
    nothing came for too long, the line fell, the answer broke off, or the KVM (or what
    stands before it) answered with an error of its own, 500 and above, in the middle of
    it. Qt says when a question has gone out for a GET and for a POST with something in
    it, also on a kept line; for a POST with nothing in it, which is every button of a
    KVM, it does not: such a question is never "not there" on a guess. And the limit on
    being let in is put only on questions that do nothing to the computer (looks,
    pictures, the sign-in): Qt's word comes a moment after the question went, and in
    that moment the limit would call "not there" what is on its way; and it would cut
    off a button the KVM holds for seconds. And Qt's other way of posting, by the name of the kind, moves what stands
    behind the question mark into the body, where a KVM does not look. Qt asks a GET
    again by itself when the line fell, and never a POST. All of this is pinned by
    tests (`qtSaysWhenAQuestionHasGoneOut` and its neighbours in
    `core/tests/test_kvmd_device.cpp`): when one of them fails after Qt was changed, read
    this paragraph again.
  * A refused password is not sent again for a minute; a KVM that is away is asked less
    and less often; a picture never waits long (a card is told to ask again, and what a
    slow KVM sends after that goes to whoever asks next); what a KVM is (model, one
    computer or several) is asked rarely. An asking for a picture that follows closely
    on another is answered with the picture that one brought: two windows that show
    one card are one question to the KVM. Whoever has the KVM live asks otherwise
    (`askNewSnapshot`): that one is never answered with a picture somebody was handed
    before. It is a question to the KVM each time, unless one for that size is under
    way, which it joins, or a picture has come that nobody was handed yet. That one
    only while it is new: asked of the KVM after this asking began, or come no longer
    ago than `Pace::freshMs`. An older one is thrown away, and the KVM is asked. (Such
    a picture lands after whoever asked had given up, and is kept; without an age it
    was a live view's first picture minutes later, and was taken for the sign that
    the KVM's video is awake.)
  * Whoever asks how a KVM is, is told what the look found that the asking began. What
    is known of a KVM is fresh for a little less long than a wall waits between two
    askings (`Pace::freshMs`), so every asking begins a look (`isStale`, `look`), and
    the answer waits for that look, for a moment and no longer (`afterLook`,
    `Pace::lookPatienceMs`; `LocalWall::askStatus` waits once for all its KVMs, which
    are looked at side by side). A KVM that is slower, or away, is told as it was last
    known. Answered at once, a new KVM was "connecting" until the wall asked again,
    and every change came that much late.
  * Where a KVM's state says nothing of its video (one maker's says none of it while
    the video rests), what the last picture said stands in, and is said at once
    (`pictureSays`, `standIn`): whether the computer sends a picture, by a picture
    that came with kvmd's header for that; and how large its picture is, by one as the
    KVM captured it, since one the KVM made smaller for a card has another size. Never
    the other way round: what the state says is how it is. Each of the two stands in
    only while the picture that said it is new (`Pace::restAfterMs`): what a picture
    said an hour ago says nothing of a computer that may have been switched off, or
    set to another size, since. The size has a time of its own for that
    (`Sight::sizeAt`): only a picture as captured says one, and the small pictures
    that come after it do not keep it new. And only of the computer the picture
    showed: what was seen is dropped wherever the driver notices another computer on
    the KVM's screen than before (`forgetWhatWasSeen`), when it puts another there
    itself and when a look finds another there than it knew of.
  * The line that keeps a resting KVM's video awake (`core/awake_line`) is a WebSocket of
    the program's own, and not Qt's. Asked to sign on in some ways (NTLM, Negotiate),
    Qt's WebSocket answers by itself with the account of whoever is signed in to Windows,
    names this PC in doing so, and asks for the line again for as long as it is asked: a
    made-up KVM had it ask twenty times and more within a second. It tells nobody
    before it does, and cannot be told not to. The program's own speaks the little that
    holding a line takes: it looks at the certificate when the two have agreed on how to
    speak and before a byte is written (the pass goes out with the asking), asks once,
    takes "you are in" with WebSocket's own mark for an answer and anything else for the
    end, throws away what comes without keeping it, and answers "are you there". A line
    under which the KVM rests all the same has ended without a word (the KVM lost its
    power, a router forgot the line), and is made anew.
  Tried against a made-up KVM in C++ (`MadeUpKvm` in the tests) and against the app's own
  in Python, a real web server with kept lines, TLS and a WebSocket
  (`tools/stand_in_kvm.py`, with `qkvm-kvmd-rig` from `bench/`; with `--probe 1` and
  `--known`, a certificate's SHA-256 or the word `windows`, the rig asks an address as
  this PC's wall asks a KVM of its own, held to what the KVM is known by, and signs
  in to nothing). And, on 2026-10-12,
  against one real KVM that its owner named: asked what it is, signed in to under the
  certificate it showed, its resting video woken, looked at, pictures taken, its stream
  asked for (it serves none), the line for keyboard and mouse let in and held, and
  the KVM had live for ten seconds by its pictures (`--live` and `--new` of the rig,
  which ask for pictures and send nothing). By the owner's leave one key and one
  small move of the pointer were said on that line (`--touch` of the rig); nothing
  else was done to it, and section 11 says what of this was seen.
* **A KVM's stream of pictures** (`core/multipart_pictures`, `core/kvmd_stream`). kvmd
  passes its live picture on as an answer that never ends, one JPEG after the other
  (`GET /streamer/stream`). `MultipartPictures` is the reader, apart from the line: fed
  the bytes in whatever cut, it hands out each part's bytes as they came (whether they
  are a picture, the decoder says). It keeps one part at most; the lines a part begins
  with are bounded; a part has to say how long it is, no more than is taken as one
  picture, and a stated length takes no memory by itself. What breaks a rule ends the
  reading for good (`End`), and so does the KVM's own closing word.
  `KvmdDevice::watch()` hands out the stream (`KvmdStream`), which is the driver's own
  question, by its rules. One at a time to a KVM: a stream holds one of the six lines
  Qt keeps to a place, and six streams left none for a look or a button; a second is
  told `busy` and asks the KVM nothing. What a KVM answers in the place of a stream is
  told apart. 401 or 403 under a session it has only just given is `not_allowed`: the
  stream is what it refuses, the session stays for every other question, and no stream
  is asked for under it for a while. 404 is `no_stream_here`: this KVM serves none.
  So is a yes (200) with something of another kind than a stream of pictures: what
  it names is not `multipart/x-mixed-replace`, or it names no kind at all
  (`callsItselfAStream`). The KVM answers at this place with another thing, and has
  no stream of JPEGs. A yes that calls itself a stream of pictures and is none this
  program reads (it names no word between its parts, or breaks a stream's rules
  later) is `bad_response`, and so is any other number than those named here: that
  says nothing of whether the KVM has a stream.
  kvmd's own 503 is a video that rests: it is woken, and whoever asked asks again
  (`no_stream`). A 502, 503 or 504 in anything but kvmd's own shape is a gateway's
  page, and the page alone does not tell the two apart (`saidByAGateway`). What
  stands before kvmd on a stock KVM is taken to answer so while the program that
  makes the stream is not running, which is whenever nobody watches. One maker's
  real KVM, whose live picture comes another way, was seen to answer so while its
  video rests, and with the yes of another kind while its video is awake. Whether
  the video is awake tells the two readings of a page apart: where the driver has
  had a picture of the KVM within the last moments (`sawItsVideoAwake`,
  `Pace::freshMs`), the video is awake and still there is no stream
  (`no_stream_here`); otherwise it is a video that rests for all that is known, and
  is woken. So of a KVM nothing was seen of, the first such answer is `no_stream`,
  and the one that follows a picture is `no_stream_here`. What comes with such a
  refusal is waited for as long as a stream may be silent, counted once from the
  beginning of the answer and not anew from each byte of it, and then read as far as
  it came: a refusal whose words come slowly, packed or broken off is a refusal all
  the same, and the KVM is not counted as away for it. What waits to be read is bounded,
  so a KVM that sends faster than it is read is held back by the line. A stream has
  no end to wait for, so it has two limits, and is over by either (`timeout`). One
  is on silence: nothing at all comes for `Pace::streamQuietMs`. The other is on each
  picture: it is whole within `Pace::streamPictureMs` of the first byte that came
  behind the picture before it. That one is the longer, so that a large picture over
  a slow way arrives; and it is there because a KVM that says a picture of megabytes
  follows, and sends a byte of it now and then, is never silent. (The limit on
  silence takes a KVM to send something now and then also of a screen that does not
  change: assumed of kvmd's streamer, not tried on a real KVM.) The video is kept
  awake while a stream is open, and for a moment after it ended
  (`Pace::restAfterMs`).
* **A line to a KVM** (`core/device_line`): the program's own small WebSocket, on a
  `QSslSocket`, for the two things a KVM is held a line for. Nothing of what a KVM
  sends on it is kept or read: it is thrown away, bounded in the size of a message
  and in messages and bytes a second, and "are you there" is answered. Short texts,
  and short messages of bytes (`sayBytes`), can be said on it, each masked anew. It is
  asked for once and never a second time; what would split the asking (a line's end in
  the path or the pass) is not sent; and the certificate is looked at by the driver's
  own rule, which the two share (`core/kvm_certificate`: Windows' objections are
  passed over only for the very certificate the KVM is known by, and only where every
  objection names that one).
  Windows is given little room to hold what was said, and every line the program ends
  without a goodbye is reset (`DeviceLine::reset`), so that what Windows still held
  for it is thrown away with it: one that ended by itself, and one that is cut. A line
  that is closed (`close()`) is said goodbye on, behind what was said before that,
  and all of it is offered to Windows there and then; where something had waited
  too long already, neither is done, and the line is reset at once. After the
  goodbye the line is not ended from here, and nothing more is said on it: the KVM
  answers a goodbye with its own and ends the line, and that is waited for, for two
  seconds. `gone(bool arrived)` says when it is over: the KVM has ended it, or its
  time was up and it was reset as a cut one is, with what had not left, or it had
  ended before it was closed. `arrived` says whether the KVM's own goodbye came
  behind the one from here, which is its answer to that one, and the one sign there
  is that it got what was said. A line that went without it (it fell, the KVM ended
  it without a word, or its time was up and it was reset) may have lost what was
  said on it last. (A goodbye that the KVM said of its own accord, and that crossed
  the one from here on the way, is taken for the answer: a guess.) What `close()`
  says is less: that
  the goodbye was said and all of it offered to Windows, on a line that stood after
  that. That is not "it has left", and not "it arrived": what Windows did not take
  at once waits in the program, and goes with a line that is reset. So the end of
  the line does not travel with what was said on it last, which for keyboard and
  mouse is the last
  "up". Why that matters is Windows' doing: it resets a socket that is closed while
  something it had received on it was not read, and the far end may then throw away
  what it had received and not yet read. Measured against the tests' made-up KVM, on
  this PC: the last "up" of a line was lost whenever the KVM had said something the
  line had not read (25 times of 25), and never where nothing was unread (0 of 125).
  Whether a real KVM, which is kvmd on Linux, loses it the same way is not tried.
  (This paragraph blamed the reader before, Qt's own WebSocket, which plays the KVM
  in some tests. That was a wrong reading: the made-up KVM that reads its line by
  hand lost the "up" as well, and of a line that ends in order Qt's WebSocket loses
  nothing: `theServersLastWordIsHeardAlsoWhereItComesTogetherWithTheLinesEnd` in
  `core/tests/test_live_link.cpp`.) All of that is for a closed line that is kept
  until then. One that is thrown away in the same breath is neither waited for nor
  reset. For that one `~DeviceLine` first takes off the line what had come from the
  KVM and was not read, as much as a KVM has reason to send in a second and no more,
  so that Windows closes the line and does not reset it, and goes on delivering what
  it took for as long as the KVM takes it. (Tests on this PC, in
  `core/tests/test_kvmd_console_line.cpp`, show: that a line which is cut or ends by
  itself is reset, and that nothing more arrives after it; that a closed line is
  waited for, is said to have gone when the KVM has ended it, and is reset when its
  time is up (`aLineThatWasClosedSaysOnceThatItHasGoneWhenTheKvmHasEndedIt`,
  `aLineThatWasClosedAndThatTheKvmDoesNotEndIsResetWhenItsTimeIsUp`); that a line
  which has gone says whether the KVM answered its goodbye, in the clear only
  (`aLineThatHasGoneSaysWhetherTheKvmAnsweredItsGoodbye`); and that what a line said
  last arrives where the line is thrown away over something of the KVM's that was
  not read, in the clear and over TLS, where what had come lies on the line hidden
  and is taken off it as that
  (`whatALineSaidLastArrivesAlsoWhereItIsThrownAwayOverSomethingOfTheKvmsThatWasNotRead`).
  Set, and not shown: what a KVM says after that last reading finds no line, and is
  answered with a reset all the same; and a way that itself stalls, which no test on
  one PC can make.) `AwakeLine` is one such line on which nothing is ever said: it
  keeps a KVM's video awake.
* **Keyboard and mouse to a KVM** (`core/kvmd_console_line`, `core/kvmd_input`).
  `kvmd_input` makes each message kvmd's `/api/ws` takes, in the two wordings kvmd
  has: short messages of bytes, which are what kvmd's own pages send, and texts of
  JSON. In both: a key by the name browsers have for it (a table from the USB number
  of a physical key; one that has no name there is not sent, never guessed), a button,
  the pointer's place over the picture, a move, the wheel. The asking whether the KVM
  is there is a text in either. `KvmdConsoleLine` is the line:
  `KvmdDevice::openConsole` hands it out, signed in. It says keys and the mouse in
  bytes, and in texts only for a KVM that whoever makes the line knows to take only
  those (`Where::inTexts`, which nothing in the program sets today). What it guards,
  since what it says lands on somebody's server:
  * Nothing is said before the KVM has let the line in, and what was asked before is
    dropped, not kept for later.
  * A key or button that was down here while the line could not say so (before it
    was in, or after everything was let go) is not pressed by its repeat: nothing is
    said for it until it was let go here. And while one of the eight modifiers is
    held here in that way, no other press is said either, of a key or of a button of
    the mouse: on the machine that modifier is up, and Ctrl+C would arrive as a bare
    "c", Ctrl and a click as a click. Where the pointer is, how far it moved and the
    wheel are said all the same.
  * It remembers what it said is down. Letting go of everything says the buttons up
    first and then the keys, the last pressed first (Ctrl up before Z would leave a
    bare "z"); closing does that first. Whoever is done with a line hands it over to
    be kept until it has gone (`KvmdConsoleLine::letGo`, as `KvmdLive::giveBack` does):
    it is closed, the KVM ends it after the goodbye, and one that the KVM has not
    ended within the line's time is reset then, with what had not left, so that a
    key that waited is not played later. Closing says what it could do (`close()`,
    and `letGo` with it): true where everything that was down was said to be up and
    that was offered to Windows with the goodbye, on a line that stood, which is as
    far as this program sees at that moment; false where something may still be
    down on the machine, because a key or a button could not be said up any more,
    or what was said up since the KVM was last heard from went with a line that was
    reset and not pushed out. What shows only later is told later, to whoever
    `letGo` is handed for that (`letGo(line, stillDown)`). Where the closing said
    that all was let go, on the strength of an "up" that the KVM was not heard from
    after, and the line then went without the KVM's own goodbye (`gone(false)`: it
    fell, the KVM ended it without one, or the KVM did not end it in its time and
    it was reset, the "up" with it), `stillDown` is called, once, when the line has
    gone, and never from inside
    `letGo`: something may still be down on the machine after all. It is not called
    where the KVM's goodbye came; nor where the closing itself said no; nor for a
    line that had ended before, which said what it left down when it ended
    (`closed`). A line that is
    thrown away in the same breath lets go and says goodbye too, and is neither
    waited for nor reset ("A line to a KVM", above): so it is when the program ends.
  * A watch: it asks every second, and a KVM from which nothing at all has come for
    three is taken to be gone. And nothing is said while what was said before has
    waited in this program for longer than a quarter of a second: the line ends
    instead, and is reset. A key that cannot go now does not go later.
  * Where the line ends by itself, what is down can be let go no more. It says so
    (`closed`, `leftDown`): where it had said keys or buttons down, and also where it
    had said some up since the KVM was last heard from, since that word may have
    gone with the line. The machine may still hold them, and it is the KVM's to let
    go.
  * A line that ended because the KVM does not know its pass (`auth_failed`) says
    which pass that was (`pass()`), and whoever holds it tells the driver
    (`KvmdDevice::passWasRefused`). The driver forgets the pass only if it is still
    the one it holds, and signs in anew for the next line. Not where the KVM had only
    just given that pass (the first after a line's refusal had the one before
    forgotten, with no line let in under it since), or had refused the line under it
    in this way before: then it is the line that the KVM refuses, and not the pass,
    under which every other question is served. The pass is kept, whoever asked for
    keyboard and mouse is told `not_allowed`, and for a while no line is handed out
    under that pass and the KVM is not asked (`openConsole`, `Pace::signInRetryMs`).
    Given up for lost each time, such a pass had every asking for keyboard and mouse
    throw away a session that works and post the password again.
  * What that does not cover is said at the class: what the far end's own Windows
    or Linux has taken off the line and its program has not read yet is played when
    it reads again; only the watch bounds how much that is.
  Seen on the real KVM: the line is let in and held. One key and one small move of
  the pointer were said on it in bytes, by the owner's leave; that they arrived at
  its computer was not seen (section 11).
* **`WallSource`, and `LocalWall`.** `WallSource` is what a wall asks of the place its
  machines come from: the machines and their order, how each is, a picture, something
  done, the saved actions, the computers to wake, a pass into a console, a machine
  live (`openLive`, below), and whether an error leaves open that something was done.
  That one is the place's own to say: behind a QKVM server's rule a KVM's fallen line
  would read as a refusal, and a run over several machines would offer the button
  again. Two places answer it, and what asks cannot tell which: `ServerClient` for a
  QKVM server, and `LocalWall` for this PC. What asks is `WallFeed` (given another
  place with `setSource`, only while it is stopped, and then it knows nothing of the
  wall that was), and in `ui/` `MachineActions` and `LivePicture`, through
  `WallMachines` (by a machine's key on the wall, with no place of their own: section
  5, "The sources behind the wall"). None of them names `ServerClient` for what it
  asks; the feed still knows the codes of the errors that end a server's wall (signed
  out, a certificate that is not the server's, a QKVM that came back speaking another
  version of the API), which this PC's wall never gives. `LocalWall` is this PC's own
  wall behind it: the KVMs that were added in the program, what each is known by in the
  settings (`OwnKvm`: an id of the program's making, name, address, user, group,
  certificate), each password in the secret store under the KVM's id and nowhere else,
  and a `KvmdDevice` for each. It answers in a QKVM server's types and words. Each KVM
  fails by itself, in its own `Status`: there is no "the wall is away" here. One thing
  at a time goes to a KVM (`busy`, in the server's sentence), and a KVM is not taken
  off the wall under what is being done to it. What cannot be written down is not
  added: neither the KVM nor its password is kept then. There are no accounts and no
  roles on it, and no KVM of it hands out a pass into its console (`no_console`).
  Every KVM of it can be had live, and the wall says so (`Wall::liveForm`). Its saved
  actions are kept beside its KVMs (`OwnActions`, the next bullet).
  A KVM's password is kept in one secret together with what it was saved for (SPEC.md
  section 8): a first line `QKVM-KVM-2`, then the address as the settings have it, then
  `none` or `sha256:` and the certificate in hex, then the password, which is all that
  follows, whatever is in it (`savedFor`, `passwordIn`). A secret of any other form,
  and one that was saved for another address or another certificate, is as none: that
  KVM has no password (`no_password` on its card), and nothing is made of what is
  there. So where the list and the store do not agree (the list could not be written
  and the password before could not be put back, or the program ended between the two
  writings), what is in the store goes to nobody. Where the kept password may go
  without being typed anew is one rule (`keptPasswordMayGoTo`), which the wall keeps
  whoever asks, for signing in with it (`signerIn`) and for saving (`change`): to the
  KVM as it is known, at the address it has; or to another address only where both are
  HTTPS and the KVM is known by a certificate, and then to what shows that certificate
  and to nothing else. The wall refuses by itself as well: an address that is not
  written as the program writes one (`notAnAddress`: the settings would not read it
  back at the next start); plain HTTP at the place where a KVM is on the wall over
  HTTPS (`intoTheClear`, `not_in_the_clear`); and, for a certificate that somebody
  said yes to (`knowBy`), one that another KVM of the wall is known by (`twinOf`,
  `sameDevice`: what shows that one is that KVM, at this one's address, and this one's
  kept password does not go to it). It counts the writings of each KVM's secret
  (`secretWrites`), for whoever reads a KVM, speaks to it for a while and saves then.
  A writing is counted once it has happened: a change that was refused with what was
  kept still kept, or kept again, counts nothing, and whoever read the KVM before it
  still has it as it is.
  Beside each KVM the wall keeps a mark of whether a password is kept for it
  (`Remembered::kvmPasswordsKept`, "secret_kept" on the KVM in the settings file;
  `LocalWall::marksAPasswordFor`). It is no secret: it says that a password was given,
  and nothing of it. The wall writes it in the same writing as the list, at every keeping
  and every forgetting of that KVM's password, and never otherwise: an adding (no mark
  where the password was given empty), a change that writes a password or takes the kept
  one out of use (a yes to another certificate where none was kept for the KVM as it was,
  `knowBy`), a taking from a file (no mark for a device that came without one), and a
  removal, which takes the mark with the KVM before the password is forgotten. Where the
  list cannot be written, mark and store are what they were. In the file only a flag that
  says true counts, and a mark for an id that is no KVM of the list is not written. Whoever
  lists the KVMs (the settings' list, section 5) says "No password saved" by the mark, and
  by what the KVM's driver found at its last look (`foundWithoutAPassword`: its
  `no_password`, which wins over the mark, since Windows may have lost what it kept), and
  reads no password for either. So a wall that was written before the mark was kept says
  "No password saved" of every KVM until its password is given anew; and a mark over an
  entry that Windows lost shows as none only once the driver has looked, never for a KVM
  that was not asked yet. Tried in `core/tests/test_local_wall.cpp`
  (`theWallMarksWithTheListAtEveryKeepingAndForgettingWhetherAPasswordIsKept`,
  `aYesToAnotherCertificateMarksWhatComesOfThePasswordThatIsKept`,
  `whereTheListCannotBeWrittenTheMarksAndTheStoreAreWhatTheyWere`), `test_settings.cpp`
  (`theMarkThatAPasswordIsKeptStandsWithItsKvmAndIsSoOnlyWhereTheFileSaysSo`) and
  `ui/tests/test_own_kvms_view.cpp` (`theListReadsNoPassword`, with a store that counts
  its readings).
  A password is read from the store only to be used: for a KVM's driver (at a start, of
  one that was asked what it is; once an asking found one, `wasAsked`; where a change
  makes the driver anew), for a sign-in of somebody's change (`signerIn`), for a change
  that takes it along or may have to put it back (`alter`, which reads once more after a
  writing that failed), and for a sealed file of the wall (`asAFile`). Never to say
  whether one is kept, and a new name or group reads nothing. Every reading writes over
  what was read and what was decoded of it, on every way out but the one that hands it
  on: where its signs are found wrong, and where something throws (`keptPasswordOf` and
  `passwordIn`, with `WipedOnLeaving` of `core/wiped.h`; `alter` writes over what it held
  to put back). Windows' own blob is written over in its memory before it is given back
  to Windows, on every road, one that is too long as well (`takenOut` before `CredFree`,
  `core/src/windows_secrets.cpp`), and so is the copy the store makes for Windows to keep.
  What is handed on (to a driver, to a file) is the receiver's, and is not written over
  here; nor is what another holds as well (Qt shares a copy until one of the two is written
  to, as a store in memory does: `wipeInPlace`). A test sees the guards write over memory
  of its own (`whatHoldsASecretIsWrittenOverOnEveryWayOutButTheOneThatHandsItOn`,
  `whatWindowsHandsOverIsCopiedOutAndWrittenOverWhereItIs` in `core/tests/test_sealing.cpp`);
  that the store and `passwordIn` use them rests on reading. Not written over: the bytes
  the wall makes to keep a password, and a sealed file's passwords (section 11).
  No password goes to an address before what answers there was asked what it is, and
  said that it is a KVM of the family the program speaks to. Whether a KVM was asked
  is kept with it (`OwnKvm::askedWhatItIs`, "asked" in the settings file), and is the
  wall's own to say. Not asked yet, and so asked before its first sign-in: every KVM
  that came from a file (`takeFromAFile`); every KVM of a settings file that does not
  say "asked" of it in so many words, which is each KVM of a wall that exists today,
  once, at its next start; a KVM over plain HTTP, once in every run of the program
  (over plain HTTP nothing shows who answers, so having been asked is not kept from
  one run to the next, and a settings file that says otherwise is not believed:
  `keepsThatItWasAsked`); and a KVM after somebody said yes to another certificate for
  it (`knowBy`). Until it was asked, a KVM's driver has no password and is asked
  nothing, and the password stays in the store. The asking is the three questions the
  box that adds a KVM asks, with nothing in them that says who asks, on lines that are
  held to what the KVM is known by (`KvmProbe::askAsKnown`, below). What this keeps a
  password from is a machine that is no KVM at all and has the address now. Not from
  somebody who answers as one: against that there is HTTPS and a certificate, and
  nothing else.
  Asking and driver have their times from one place (`core/kvm_times.h`): each step of
  an asking has what the driver that signs in afterwards has for it, 4.5 s to be let
  in, 8 s of quiet, 36.5 s in all (`KvmProbe::Pace::beforeASignInWith`;
  `anAskingIsGivenWhatTheDriverThatSignsInIsGivenForEachStep` in
  `core/tests/test_kvm_probe.cpp`). The asking had less at first, and a KVM that is
  slow to let a line in (behind a VPN or a relay) would never have been signed in to
  (`atItsFirstStartAWallThatWorkedIsAskedAndSignedInToAlsoWhereAKvmIsSlowToLetALineIn`
  in `core/tests/test_local_wall.cpp`). The wall asks in eight places
  (`LocalWall::Own::askedAtOnce`). An asking holds its place for 0.4 s
  (`placeHeldMs`) and then goes on without it, so that what is off does not keep
  those that are well waiting; 48 askings are under way at the most (`askedInAll`),
  each a thread of Qt's and up to three lines. Those that were never asked in vain are
  asked first, in the wall's order; those that were, by turns, the one first that has
  been due for the longest, so that none of many that stay off is left unasked
  (`kvmsThatAreOffAndStandFirstDoNotKeepTheOnesThatAreWellWaiting`,
  `asManyAreAskedAtOnceAsThereMayBeAndNoMoreWhateverTheWallHolds`,
  `everyKvmThatStaysOffIsAskedAgainInItsTurnAlsoTheLastOfMany`). Behind K that are
  off, a KVM that is well is first asked after K/8 times 0.4 s while K is under 48;
  from there on, every 48 cost one giving-up (4.5 s where nothing is let in). Whoever
  asks how the KVMs are waits for an asking only while it holds a place. What a KVM
  comes to after that, the wall says by itself (`cameToSomething`, once for all that
  came to something within 0.1 s), and whoever shows the wall looks at that word
  (`WallFeed::lookNow`, joined in `PcSource`): a tile says what its
  KVM came to within a moment, and not at the feed's next turn, four seconds later
  (`aTileOfThisPcsWallSaysWhatItsKvmCameToWithinAMomentAndNotAtTheNextLook` in
  `ui/tests/test_server_session.cpp`). While a KVM is asked for
  the first time its tile says "connecting" and nothing else, and a picture or a live
  view that is asked for meanwhile is told `no_stream`, which is "ask again". A KVM
  where nothing answers is "offline" (`unreachable`: "Nothing answers at this address.
  No password is sent there before a KVM has answered."). What answers and is no KVM
  of the family is a problem (`error`), in the code the box that adds a KVM has for
  it, with a sentence that ends "No password was sent to it."; and a line that showed
  what the KVM is not known by is said as the KVM's driver says it (`cert_mismatch`,
  `cert_invalid`). Where all that answers is a gateway's 502, 503 or 504
  (`KvmProbe::Found::saidByAGatewayAlone`), that is `unreachable` and offline, in a
  sentence that says that only a gateway answered: a KVM behind a proxy, which
  restarts, is not called something that is no KVM
  (`aKvmBehindAGatewayThatSaysItIsAwayIsAwayAndNotSomethingElse`). Each of them is
  asked again later and later, as a driver leaves a
  KVM alone that is away: after a second at first, twice as long each time, 15 s at
  the most. Tried against made-up KVMs, and the asking, by the rig, on one real KVM.
  The wall itself was not started against a real KVM. Section 11 has that, and what
  is open.
  What the wall records by itself, with nobody having done anything (that a KVM was
  asked; a certificate it kept), is written to the settings together (`Settings::note`,
  `writeDown`): 2 s after the last of it, 30 s after the first at the latest, and when
  the wall goes (`Own::writtenAfterMs`, `writtenAtTheLatestMs`;
  `whatTheWallRecordsByItselfIsWrittenTogetherAndWhatSomebodyDidAtOnce`). What a person
  did (an adding, a change, a removal) is written at once, as before. The certificate
  itself of a KVM that is known by one is kept beside the list ("certificate_pem" in
  the settings file, `Remembered::kvmCertificates`), for the day the wall is written
  to a file: from a file that names it, or from when the KVM next shows the very
  certificate it is known by on a line of its driver's
  (`KvmdDevice::showedItsCertificate`). Never another; it goes when the KVM comes to
  be known by another or by none, or is taken off the wall. With the certificates, the
  settings file of the largest wall is some forty-three megabytes: a file may be 64
  MiB now (8 before), and of its list of KVMs only the first 2000 entries are looked
  at (`Remembered::mostKvms`; `theLargestWallThereIsIsReadBackWhole` in
  `core/tests/test_settings.cpp`). And every sentence of the core that names a KVM
  sets the name off (`LocalWall::calledInASentence`, by `exchange::nameSetOff`;
  `everySentenceOfTheWallThatNamesAKvmSetsItsNameOff`): a name is free text, typed or
  from a file. A device without a name has none, and is called by where it is now.
* **`OwnActions`** (`core/own_actions`). The saved actions of this PC's wall, held by
  the wall (`LocalWall::actions()`): keys pressed together, or a text that is typed,
  with Enter after it or not. The list is in the settings beside the KVMs
  (`Remembered::actions`; `OwnAction`: id, name, presses or types, keys, Enter,
  dangerous, `changed`, and which writing of its text is the action's), in the order of
  adding; a change keeps its place, and two may share a name, as on the server. Each is
  held to the QKVM server's rules one for one (`action::nameFrom`, `areKeys`,
  `isTypedAsOne`, `isOne`) and refused in the server's codes and sentences; 500 at the
  most (`mostSavedActions`). The settings file's list is read back with a bound, each
  entry held to those rules, and no id twice. The text an action types is a secret: in
  Windows' store only (its form is said at "Settings and secrets", below), never in the
  settings file, a log, a sentence or an answer, but through two doors: `forOneRun`,
  private and the wall's alone, for one run; and `textsHandedOutForASealedFile`, named
  so that it cannot be called by oversight, for a sealed file of the wall, which nothing
  calls yet. It is kept whole or not at all: a new text is written into the place the
  action's text is not in, then the list, and only then is the writing before
  forgotten. What the store holds in another form, for another action, of another
  writing, or with a part missing, is as none: the action has no text, and says so
  when it is run (`no_text`, nothing typed). A change and a removal name the state that
  was looked at (`changed`; refused as `changed` where it is another). A change counts
  `changed` up by one; one that could not be written down is taken back and then
  stands under a number nobody was shown (two higher), as the server does it; an empty
  text in a change keeps the saved one. An action whose count is within two of a billion
  (`action::mostEdits`) is not changed again (`invalid`): it would be gone at the next
  start (`anActionChangedAsOftenAsAnyMayBeIsNotChangedOnceMoreAndStaysOnTheList`). A run
  is `LocalWall::act` with `Action::run`: refused `not_found` or `changed`, then `busy`,
  and only then is the text read (into `SecretBytes`) for that run and typed by the
  driver as any typed text is; the driver's own copies are not wiped.
  `LocalWall::askSavedActions` hands the list out as a server does, never the text. A
  change of the list is said as `LocalWall::changed`: whoever shows the wall asks anew,
  which costs one round of looks at the KVMs, as an edit of a KVM does. Tried in
  `core/tests/test_own_actions.cpp` (93, as Qt counts them) and `test_settings.cpp`;
  one test meets Windows' real store under a name of its own and takes back what it
  wrote. No box on the screen manages them yet; section 11 has what is open.
* **`KvmProbe`, and `kvm::addressFrom`.** What is asked before any password goes
  anywhere. `kvm::addressFrom` makes the address that is used out of what was typed, as
  the server's `device_url` does, and refuses in the server's sentence; where the two
  differ it is the stricter (a line end inside what was typed is refused, where the
  server drops it and reads another address). `KvmProbe` asks an address three
  questions with nothing in them that says who asks, and tells what answers: a KVM
  that runs kvmd (only one that answers in kvmd's own shape: the server takes
  everything else for one, and this does not), a JetKVM, a server's controller,
  something else, or nothing. Over HTTPS it tells the certificate that was shown and
  whether Windows trusts it for that name, which is what a KVM is then known by. It
  passes Windows' objections over, since nothing secret is sent; so each asking has
  lines of its own that end with it, and no password ever goes out on a line of a
  probe. Asked of a KVM that is known already (`askAsKnown`, with a `Known`), the same
  three questions go out on lines that are held to what the KVM is known by, as its
  driver's lines are: over HTTPS a line that shows another certificate than the one
  the KVM is known by, or, where it is known by none, one that Windows does not trust
  for its name, hears nothing at all, not the questions either (`Found::refused`, and
  then nothing that answered counts;
  `aKvmThatIsKnownByACertificateIsAskedOnlyWhereItShowsThatOne`,
  `aKvmThatIsKnownByWindowsWordIsAskedOnlyWhereWindowsTrustsWhatItShows`). Over plain
  HTTP there is nothing to hold a line to, and it is asked as any address is. Each step
  of an asking, of either kind, has the driver's time (`core/kvm_times.h`; `LocalWall`,
  above). Not yet: a JetKVM's other way (plain HTTP beside HTTPS), which comes with
  JetKVM itself.
* **`KvmAdding`.** A KVM is added in one sequence, and one at a time (`busy` for a
  second while the first is under way). What was typed is read by the server's rules
  for such lines (`kvm::nameFrom`, `groupFrom`, `userFrom`: no line end, nothing that
  turns the writing round, the server's limits and sentences); the address is asked
  what answers there, with no password (`KvmProbe`); what is not a KVM of the kvmd
  family is said as that (`not_a_kvm`, and `not_yet` for a JetKVM or a server's
  controller, with the way that works today); then the KVM is signed in to once,
  under the certificate the probe saw, by a driver made for the check; and only then
  is it on the wall (`LocalWall::add`), under a name of its own where none was typed
  (what it calls itself, unless that is what every KVM of its make is called; else
  where it is). The wall is looked at again after the KVM was spoken to: it may have
  changed meanwhile. The password leaves the sequence when it is handed to the wall,
  and is in no error and no log. What it tells back (`Added`: the name, the model,
  the version, whether the KVM is spoken to over plain HTTP, and the certificate it is
  known by from now on) is what the box says. An adding can be given up (`giveUp`): a
  KVM that answers a byte at a time kept the box for minutes. Nothing is on the wall
  then, whoever asked is told so once (`given_up`), and what the probe or the driver
  still answers reaches nothing (each answer is taken only in the turn it was asked
  in). What the KVM was asked up to then, it was asked. A KVM that is on the wall is
  not put there a second time the other way of speaking to it (`LocalWall::atOnePlace`:
  one host, and neither with a port of its own, or the one port that is spoken to,
  written or not): least of all in the clear where it was added over HTTPS, which
  would send its password a second way, unencrypted. (Not seen as one place: a name
  and the number it stands for, and the two ways of writing one IPv6 address that
  holds an IPv4 one. A KVM that is known by its certificate is found all the same.)
* **`KvmEditing`, and `KvmCertificateReview`** (`core/kvm_editing`). A KVM of the wall
  is changed as the server changes a device, by its rules and in its sentences, with
  the differences that are written at the class. Among them: a new name or group
  alone asks nothing of the KVM (one that is away can still be renamed); there is no
  "save without checking" yet; a KVM is not moved onto another one of the wall
  (`duplicate` for its address, with nothing asked there; `same_device` for what shows
  its certificate, before a password goes there); and a KVM that is on the wall over
  HTTPS is not changed to plain HTTP at the place it is at (`not_in_the_clear`, with
  its password typed or without, and nothing is asked there either). A new user name
  or password is signed in with first, and saved only where that worked. A new
  address with a password typed is asked what answers there, as when adding, and what
  the KVM would be known by there is found anew. One rule then holds for the move:
  over HTTPS the KVM is signed in to at once only where it will be known as it is
  known now. That is where what answers there shows the certificate it is known by;
  or where it is known on Windows' word where it is, and Windows trusts what is shown
  there for the name that was typed. (The server forgets the certificate with the
  address, and knows the device by what it shows there.) Known otherwise there, the
  password does not go there: another certificate than the one it is known by; a
  certificate, where it is known by none (it is spoken to in the clear, or on
  Windows' word); and Windows' word, where it is known by a certificate or is spoken
  to in the clear. The change ends with `certificate_differs`, nothing but the
  probe's questions has gone there, and whoever asked is handed what the KVM is known
  by now, what is shown there, the address, and whether Windows trusts what is shown
  there, so that the KVM would be known on Windows' word and by no certificate
  (`Outcome::remembered`, `presented`, `presentedAt`, `onWindowsWord`), to be shown
  side by side. With the yes to that very thing (`Typed::saidYesTo`, and
  `saidYesOnWindowsWord` for which of the two the yes was to: a yes to the same
  certificate, known the other way, is none) it is read once more, signed in to on
  lines that are held to it, and saved as known that way: on Windows' word by no
  certificate from then on, not by the one it had either. What shows another by then
  is refused in the same way, with what it shows now; what shows another only when
  the sign-in goes out hears nothing (`cert_mismatch`; section 5 has what the box
  makes of it at the next saving). Adding a KVM anew is not held to this: there is
  nothing it was known by to put beside what it shows. A move to a plain-HTTP
  address at another host or port, of a KVM that is spoken to over HTTPS, is held to
  the same rule (at the place the KVM is at there is no yes to say:
  `not_in_the_clear`, above). Nothing can be
  shown of a place that speaks in the clear; what can be said is what is given up
  there. Without a yes the change ends with `plain_http_there`, a code of the
  program's own, before the probe's questions too: nothing at all has gone to that
  address, and whoever asked is handed what the KVM is known by now and the address
  (`Outcome::remembered`, `presentedAt`), to show the first beside what is given up.
  With the yes to that very address, and to no other
  (`Typed::saidYesToPlainHttpAt`), it is asked what answers there, signed in to
  once, and saved as spoken to in the clear and known by nothing
  (`Changed::overPlainHttp` says so to whoever shows the save). A KVM that is spoken
  to over plain HTTP already gives nothing up at another plain-HTTP address: it is
  asked what answers there and signed in to, as before.
  A new address without one: the kept password goes there only where both addresses
  are HTTPS and the KVM is known by a certificate, and then to what shows that
  certificate and to nothing else, whatever was said yes to (`LocalWall::signerIn`,
  which makes that driver itself: the kept password is handed to nobody); anywhere
  else it has to be typed (`password_needed`). A KVM that was not asked yet what it is
  (`LocalWall::wasAskedWhatItIs`; `LocalWall`, above) is signed in to by nobody, this
  change included. Anything that would sign in to it at the address it has (another
  user name, a password typed for that address) ends with `not_asked_yet`, and nothing
  goes to it. Moved with its kept password, it is moved as the wall moves it: the
  change asks at the new address itself, on lines that are held to the certificate
  the KVM is known by (`KvmProbe::askAsKnown`), before anything is saved, and signs
  in to nothing (`Changed::signedIn` is false). What shows another certificate there
  hears nothing (`password_needed`, in the sentence for a device that could not be
  checked at its new address), and what is no KVM, or nothing, is said in the wall's
  words and not saved (`LocalWall::notTheKvm`). Where the KVM is known by no
  certificate, the password has to be typed (`password_needed`,
  `LocalWall::keptPasswordStaysBehind`). Its name and its group are changed all the
  same, and it stays one that was not asked: the wall asks it where it is from then
  on (`aKvmThatWasNotAskedYetIsMovedWithItsKeptPasswordAsTheWallMovesIt`,
  `aKvmThatWasNotAskedYetIsNotMovedToWhatIsNotItNorWhereItsKeptPasswordMayNotGo`).
  What a KVM is known by and its
  password are saved together or not at all, and its driver is replaced where what
  that is made of changed; whoever waited for a picture from the old one is told to
  ask again. A KVM that was changed from another window while this one spoke to it
  is not saved over (`conflict`); nor is one whose password alone was saved anew
  meanwhile, of which the list shows nothing (`LocalWall::secretWrites`). And whoever
  had the KVM before them for a while says what they saw (`Typed::seen`, a `Seen`:
  the KVM as a box was filled with it, and how often its secret had been written by
  then): a KVM that is not that any more is not changed (`conflict`, before anything
  else is looked at). Not changed, for either of the two, is a KVM of which the wall
  recorded something by itself meanwhile, which is that it was asked what it is
  (`LocalWall::isStillTheOne`): a box that was open while the wall asked is not
  refused for it (`whatTheWallRecordedByItselfMeanwhileIsNoChangeThatSomebodyMade`).
  The wall keeps its own rules whoever asks (above), and what it refuses passes
  through as it is.
  What a change comes to is an `Outcome`: why the KVM is as it was, with what is to
  be shown side by side where that is `certificate_differs`, and what the KVM is
  known by and the address where it is `plain_http_there`; or the KVM as it is now
  (`Changed`), which says whether it was signed in to, whether it is spoken to over
  plain HTTP, and the certificate it is known by now (`Changed::certificate`), for
  whoever shows that it was saved. A change that is under way can be given up
  (`giveUp()`): a KVM that answers a byte at a time is not waited for to the end.
  Nothing is changed then, and whoever asked is told so once (`given_up`). Given up
  when its result is on its way already, or in the turn in which the wall is told of
  it, they are told what came of it, and that may be that it was saved. Whoever ends
  it there and then instead, which tells nobody, can ask what it had come to
  (`cameTo()`, from when that is settled until whoever asked was told): what was
  saved is theirs to say from then on. The review of a certificate has the same
  (`giveUp()`, `cameTo()`), for the reading and for the yes, and there it may be
  that the certificate is trusted. It reads what the KVM shows as the probe reads
  it, on lines that carry nothing secret, beside what it is known by. Whoever had
  the KVM before them for a while says so here too, for the reading and for the yes
  (`seen`, a `KvmEditing::Seen`): a KVM that is not that any more is neither read nor
  trusted, and nothing is asked of it (`conflict`), also where it became another
  while it was read. Where what it shows is the certificate another KVM of the wall
  is known by, it says which
  (`Shown::presentedIsOf`, `presentedIsCalled`): the two changed places, as addresses
  that are handed out do. Saying yes reads the certificate once more, and remembers
  it only if it is the very one that was shown (`cert_mismatch` otherwise, with what
  it shows now handed over to be shown in its place: `Outcome::shownNow`) and no
  other KVM's of the wall (`same_device`, which the wall refuses once more). It
  stands beside the wall and not in it, so that no line of the wall's is one that was
  let up unlooked-at. Taking a KVM off the wall forgets its password, and whoever
  still waited for an answer from it is told that it is not there. The box that uses
  the two is in section 5 ("Changing a KVM of this PC's wall"). Where a saving leaves
  the KVM known by another certificate than the box showed (`Changed::certificate`),
  the box shows it; and where a look finds another KVM's certificate
  (`presentedIsOf`), the box says whose it is.
* **A machine had live, whichever place it comes from** (`core/live.h`). `Live` is one
  machine live: its pictures as they come, none kept, and keyboard and mouse to it.
  Keys and the mouse go out only while this one holds them (`askControl`,
  `controlChanged`). What whoever asks may count on: each asking and each giving back
  is to be answered by one word, in the order they were said. A `KvmdLive` does so; a
  `LiveLink` passes on what the server says, which may be less. No word comes of a
  live that has ended (`closed` was its last), and whoever asks sets its own time for
  the answer (section 3.8, "Asking"). Before they are given back or it is ended from
  here, everything that is down is let go. Where that could not be done, or the word
  for it may have gone with the way it went by, whoever held keyboard and mouse is told
  that
  something may still be down on the machine (`leftDown`, said just before the
  `controlChanged(false)` for the same end; or seconds after it, where it shows only
  then that what was let go may not have arrived, as with a KVM's line that went
  without the KVM's goodbye): the machine's own end is to let go of it
  then, and nothing from here can make it. Whoever gives the live says where it can
  know that. A `KvmdLive` says it; a `LiveLink` never does, since a server has no
  word for whether it could let go (SPEC.md section 6.2). In `ui/` it is heard by
  `MachineControl`, for as long as that live is the one its window has, and the bar
  says it (section 3.8, "Something may still be down"). It is live from when it is
  handed over, says once
  why it ended (`closed`), and is never live a second time. Whoever shows a machine
  live asks the place its wall's machines come from (`WallSource::openLive`), and
  cannot tell which of the two it holds: a `LiveLink` from `ServerClient`, or a `Live`
  of a `KvmdLive` from `LocalWall` (both below). Whether a wall's machines can be had
  live at all, the wall says (`Wall::liveForm`): a place that says no is not asked.
* **A KVM of this PC's own, live** (`core/kvmd_live`). One `KvmdLive` beside each KVM's
  driver, made when the KVM is first asked for live. Each who asks is handed a `Live`
  of its own, and all share what is asked of the KVM, at the widest and the fastest
  any of them needs: two windows are not two loads on a KVM. While nobody has it
  live, nothing is asked of it here. Pictures come the best way the KVM has. First its
  stream (`KvmdDevice::watch()`). A stream that ended is asked for again after a wait:
  a second at first, twice as long each time in a row, thirty at the most
  (`Pace::streamAgainMs`, `streamAgainLongestMs`), and from the beginning again after
  a stream that gave pictures for as long as that longest wait. So for one that ended
  after its pictures had come, for one that answered with something that is no stream
  and says nothing of whether the KVM has one (`bad_response`: an error of the KVM's
  own, or what calls itself a stream of pictures and breaks the rules of one), and
  for any that
  ended while the pictures come one after the other. Meanwhile, and until the stream
  that was asked for again has given a picture, the pictures are asked for one after
  the other, so that the picture does not stand still. A stream has given when a
  picture of it was handed on: one whose parts are no pictures, for as long as one
  picture may take (`KvmdDevice::Pace::streamPictureMs`), is ended as one that
  answered with something that is none. Only the first stream since somebody came,
  where it could not be asked at all or broke off before a picture (the KVM is away,
  or refuses), ends the live. A KVM that says it serves none (`no_stream_here`:
  nothing is at the stream's place, or a yes comes from there with something of
  another kind) is
  asked for its pictures one after the other instead, as it captured them: the next
  when the last has come, no more often than whoever looks needs, at most twenty a
  second. Each is asked for as a new one (`KvmdDevice::askNewSnapshot`): a card's
  question that follows closely on another the driver answers with the picture it
  has, which would be five different pictures a second at the most. One that is byte
  for byte the one before it is handed only to whoever has had none yet: a screen on
  which nothing changes gives the same picture again. That it serves none is
  remembered for a minute (`Pace::noStreamForMs`), for whoever has it live and
  whoever comes; then its stream is asked for again, and what it answers then holds.
  It is forgotten sooner when the driver signs in anew (`KvmdDevice::passGiven`: kvmd
  was started anew, and may be another by now). Remembered for good, as it was, it
  left a KVM whose gateway had answered for its stream for some seconds without that
  stream until the program was ended. A stream of which only "the
  video rests" is said is asked for once more when a picture has shown the video
  awake; resting then again, it serves none. So a KVM whose gateway answers for a
  stream it does not have costs two askings, or one where the driver has just had a
  picture of it, as a wall's cards take them; one that answers there with a yes of
  another kind costs one; and a KVM whose video only rested has
  its stream at the second (so against made-up KVMs; no stock PiKVM was tried:
  section 11). Three failures in a row to get a picture end the live. Every picture
  is a whole JPEG, handed on only where it states a size that is shown; no key frame
  is ever asked for. Keyboard and mouse are one holder's at a time: `askControl(true)`
  has the driver open its line (`KvmdDevice::openConsole`), a second who asks is told
  `in_use` without the KVM being asked anything, and a line that ends by itself is
  said (`controlChanged(false)`) while the pictures go on. Every asking and every
  giving back is answered by one word of its own, in the order they were said, a turn
  of the loop later at the soonest. A giving back before they were given is answered
  by two: a no for the asking, which is over with it, with the code `given_up` and a
  sentence; and then the word for the giving back, which has no code and no sentence.
  A giving back by one who holds nothing is answered by that one word, where it was
  answered by nothing. The exceptions: an asking by one who holds them already, or is
  being given them, gets no second word (one asking at a time is the caller's to
  keep); and nothing is said to one whose live is over. So whoever asks can tell
  which word is whose, and need not wait out a time for one that never comes: with a
  word that never came, "Control" stayed inert for the seconds a word is owed (section
  3.8, "Asking"; `keyboardAndMouseGivenBackBeforeTheirLineWasThereAskTheKvmForNone`
  and `keyboardAndMouseGoToTheKvmFromTheOneWhoHoldsThem` in
  `core/tests/test_kvmd_live.cpp`). Where the driver hands out no line, its word is
  passed on: it could not sign in, or the KVM signs it in and refuses the line all the
  same (`not_allowed`, above). Given back, closed, thrown away, or the live over: the
  line lets go of what is down first, and is handed over to be kept until it has gone
  (`giveBack`, `KvmdConsoleLine::letGo`). Where the line
  could not let go of everything, whoever held keyboard and mouse is told
  (`Live::leftDown`), whichever way they went: the line ended by itself and said so
  (`KvmdConsoleLine::closed`), or it took no more while it was let go of. After
  `close()` that is said a turn of the loop later; whoever threw its `Live` away is
  not there to be told. And where it shows only later: the line said that all was
  let go, and then went without the KVM's own goodbye (it fell, or the KVM did not
  end it in its time and it was reset, the "up" that was said last with it).
  `giveBack` hands `letGo` whom to tell, the `Live` that held keyboard and mouse,
  and that one is told `leftDown` then, late as that is: as long after that end as
  the line was waited for, behind the `controlChanged(false)` where one was said
  for it (none is after `close()`), and only if it is still there. Where it is not
  (thrown away meanwhile, or the program
  ending), there is nobody to tell, and what is down is the KVM's to let go
  (`whereALineThatWasLetGoIsResetWhenItsTimeIsUpWhoeverHeldItIsToldLate` in
  `core/tests/test_kvmd_live.cpp`; section 11 has what is open in it). A KVM that
  is taken off
  the wall, or whose driver is replaced, ends its live (`KvmdLive::end`): keyboard and
  mouse are let go there and then, and whoever had it live is told a turn of the loop
  later.
* **One machine of a server's wall live** is a line of its own to the server
  (`core/live_link`, a `Live`; SPEC.md section 6.2), beside the calls. `ServerClient`
  makes it, in two steps (`openLive` does both, as a wall asks): it asks for a pass, as
  it asks everything (with the session, the certificate looked at first), and then
  opens the line, whose first word is the pass. A pass goes to the server that gave it:
  where another server was named before the pass came, no line is opened. The session
  never goes over the line, because the library that speaks WebSocket gives a later
  look at the certificate than a call gets. The rule is the calls' rule all the same: a
  server known by a certificate has to have shown exactly that one, and one known by
  none has to be trusted by Windows. Where Windows objects, that is decided before
  anything has gone out. Where Windows is content with a certificate that is not the
  one the server is known by, it is decided when the server has taken the line and
  before the pass is said: such a server has by then seen the question for the line,
  which names the machine and holds no secret, and nothing else.
  `LiveLink` hands pictures over as they come and keeps none; where H.264 begins, and
  after a gap in it, it hands nothing over until a key frame and asks for one, once.
  Keys and the mouse go out only while the server has said that the line holds them, and
  before they are given back or the line is ended from here, everything that is down is
  let go. A still screen sends no pictures, so silence says nothing: the server is asked
  every few seconds whether it is there, and one of which nothing at all has come for
  three such questions is gone. So is one that takes nothing off the line while what is
  said to it piles up. The line says once why it ended, in the server's word or in one of
  the program's own (which is never taken from a server), and is never opened a second
  time: whoever wants the machine live again asks for another. It says what it says from
  the middle of the socket's work, so whoever holds one ends it later, not from inside
  what it said. The tests have the other end (`core/tests/live_stand_in.h`), at the
  address of the stand-in for the calls, as a QKVM has both at one address; and they run
  on Windows' own TLS, as the program does, with makers of certificates of their own
  where a test needs a server that Windows vouches for.
* **The live picture on the screen.** The machine a window shows large is had live in
  that window (`ui/live_picture`, one per window, held by `MergedWall`), from the
  place the wall's machines come from (`WallSource::openLive`), whichever it is: where
  that wall says that it has live pictures in the form this program speaks
  (`Wall::liveForm`), while a server answers, or at this PC's own wall, where each KVM
  fails by itself, and while somebody can see the window. The pictures that come are
  pushed into a stream of their own in the window's engine, H.264 or stills as
  they come, as `ShownLarge`. Once the engine has made a picture of them, that stream is
  what the enlarged view draws (`WallView::liveStream`), and no snapshot of the machine is
  asked for that window meanwhile. Until then its snapshots are drawn and asked for as
  before, so a server without live pictures is the program as it was, and so is a line
  whose pictures the engine cannot read.
  What is drawn never goes back in time. When the line is gone (ended, the server away,
  the window out of sight), its last picture stays up until a snapshot has come that is
  newer, or the next line has a picture; the snapshot from before the machine was had
  live is not shown again. The same holds where the server goes over from stills to
  H.264 on one line. Another machine, or none, and nothing of the one before stays.
  A machine is asked for once it has stayed shown for a moment: whoever steps through the
  machines with a key held down asks for none of those passed. The place's own word
  that there is no live picture to be had (`no_live`, `not_found`, `not_allowed`,
  `forbidden`, or a form this program does not speak) is taken until the machine is
  shown anew: a KVM that was taken off this PC's wall while it was shown is not asked
  for again. So is the program's own word that this copy of it speaks to nothing
  over HTTPS (`desktop:tls_damaged`), which holds for as long as it runs. And `in_use`
  (M6 2h): a JetKVM whose one viewer somebody else has, refused so or ended so, is asked
  for again only where "Show here" took it back: its card's button for any machine in use
  elsewhere, on this PC's wall or a server's, asks first, as the page does ("Show <name>
  here? Whoever has its console open is disconnected from it."), and has it `resume`d;
  done, every window that shows it and stopped on that no asks at once
  (`MachineActions::resumed`, `WallMachines::liveMayOpenAgain`, `LivePicture::openAgain`). Anything
  else (not reached, refused without such a word, a line that
  ended) is tried again after two seconds, then four, up to thirty; a line that held for
  thirty seconds begins that anew, one that gave a picture and ended does not. The
  width asked for is the width the picture is drawn at, to the next 160 pixels, so that
  a window being dragged larger says so now and then. When the engine has lost its place
  (pictures thrown away, the device lost), a key frame is asked for with the next
  picture that comes (`Live::askForAKeyFrame`: a server is asked; a KVM's pictures are
  each whole, and nothing is). Two windows that show the same machine large have a
  `Live` each. Of a server's machine that is a line each: the server is the one who
  knows that both carry the same pictures. Of a KVM of this PC's own, the two share
  what is asked of the KVM (`KvmdLive`). A server whose screen comes live is shown as a
  KVM is: its readout would lie over the picture. Whoever sends keyboard and mouse to
  the machine (the next bullet) is told through which `Live` it is had
  (`LivePicture::live()`, `liveChanged`): where one goes, that is said while it still
  stands and before it is ended, so that what is down is let go there first.
* **Keyboard and mouse of the machine that is shown large** (`ui/control`). One
  `MachineControl` for each window stands between where the keys come from (`KeySource`
  in `ui/key_source.h`: the hook of section 3.8, or in a test one that is fed by hand),
  the machine as its window has it live (`MergedWall::drive` hands it that `Live`
  whenever it is another one or none) and what the screen shows and asks
  (`ControlView`). They can be taken where the machine is had live, whoever sits here
  may act on that machine (by the word of its own source, and never while the program
  only shows: section 5, "A wall screen"), the window's wall is not in display mode,
  and the window is in front.
  "Control" in the enlarged view's bar asks the live (`Live::askControl`) and waits five
  seconds for its answer at most; meanwhile the bar says that it asks, and offers to
  stop asking. What the machine was asked and has not answered is kept in the order it
  was asked, each word for a time (`OwedWords`, `owedForMs`), and while any of it is
  owed nothing is asked of the machine: the button that asks does nothing
  (`ControlView::busy`) until it has come or has run out. So the first word to an
  asking is its answer; a yes that nobody waits for is given back; a no that comes
  with a sentence is shown as what the machine said; and a no with the code
  `given_up` is the machine's answer to an asking that was called off from here, and
  shows nothing. A machine that says a word for every asking and every giving back, as
  a KVM of this PC's own does, has "Control" offered again a turn of the loop after
  every ending; with one that says fewer, the bar says after half a second why it is
  not offered (section 3.8, "Asking"). From its
  yes the key source listens and swallows, and the bar says "Keyboard and mouse go to
  ...", which key gives them back, "Ctrl+Alt+Del" and "Give back" (`Enlarged.qml`);
  where the window is narrow the sentence gives way, and the machine's name stands
  whole. Under that row the bar has one line more, where there is something to say.
  While they are held, what is to be known meanwhile (`ControlView::note`): which
  modifier is kept back and keeps everything else from the machine, that keys which
  another program types reach no machine, that this PC gives no second account of its
  keyboard, that a keyboard which is unplugged would not be noticed on this PC, that
  something may still be down on the machine. While they are not, why they were
  refused or taken away (`problem`), until they are next asked for or another machine
  is shown; behind why an asking was given up from here, when Control can be clicked
  again; where the machine still owes a word and nothing else says so, why Control is
  not offered; and behind it that something may still be down. Every key goes to the
  machine as the physical key it is (`ControlKeys`, `KeysDown`). Keyboards are not
  told apart: a key goes up on the machine when any keyboard lets go of it, nothing
  here knows of a second keyboard, and the bar says nothing of one. Where the key
  source says that a keyboard was taken away from this PC
  (`KeySource::Lost::AKeyboardWent`), keyboard and mouse are given back, which lets go
  of everything on the machine, and the bar says why: whichever keyboard it was, and
  whether a key was held on it, since nothing of a keyboard is kept. A key that was
  held then is kept back at the next taking, until it is let go or pressed anew. What
  is kept back and why, the leave-key, the Windows key, clicks behind keys, the
  asking, two keyboards, a keyboard that goes, and the ways they end are in section
  3.8. Every one of those ways goes through one function
  (`MachineControl::stop`; section 3.8, "Everything up"): the machine lets go of
  everything first, and the swallowing ends after. So it is too where the leave-key was heard only by the hook,
  which gave the keyboard back by itself. The mouse goes by where the pointer is over
  the picture, and not over the bars beside it (`inPicture`): sixty places a second at
  most, the buttons, the wheel. `WallWindows` makes one control, one view and one key
  source for each window (`Makers::keys`), and owns the one memory of what is held,
  which it hands to every window's control (`KeysKept`). Tested against stand-ins only
  (section 11): no test installs or runs the real hook. It has run once for real, when
  the owner took keyboard and mouse of a real KVM and reported that keys arrive, and
  nothing else is known from that run.
* **Pictures for the wall.** One scheduler (`WallFeed`) asks for a snapshot per visible
  tile at the server's pace: of every KVM, and of every server whose status says that its
  screen can be read. The width asked for is the page's for a tile of that size on
  screen (twice its pixels, never under 480): the spare detail keeps the scaled-down
  picture crisp, and the server then makes one picture for a browser and this program
  looking at the same machine. A tile whose last request is still out is skipped, never
  queued. Four requests for pictures are out at once and no more, and the machines whose
  turn has come wait for one of the four in the order it came: Qt keeps six connections to
  one server, and with every card's request sent at once an action, or the look at how
  the machines are, waited behind all of them. Each machine has the time of its next turn
  (`m_pictureDue`), counted from when its last request went out: the pace after a
  picture, three seconds after a failure, a second where the server says the stream is
  only being woken (`no_stream`). That time is written down before anybody is told of the
  answer, since whoever is told may say there and then what is shown, which asks again.
  A picture that could not be had is that machine's own (`pictureFailed`), and says
  nothing of the wall. The feed says that contact is lost (`contactChanged`) only
  where a look at the machines fails, the list or how they are, and that it is back
  when how they are is answered again. Three answers end the feed instead,
  whichever of its questions they come to, for whoever owns it to act on: the
  server's own word that the session is gone, a
  certificate that is not the server's, and a QKVM that came back speaking another
  version of the API (`signedOut`, `certificateRefused`, `otherVersion`).
  The bytes go to the engine as they are. Which tiles are visible the screen
  says: the first and the last of the cards the bar's choices leave that are in sight,
  with a row more above and below so that a turn of the wheel finds its pictures there.
  A card that is filtered off the wall or scrolled far away asks for nothing. A machine
  that is shown large covers the wall: pictures of it alone, as wide as it is drawn, and
  at least four a second however slow the wall's own pace is (the page's enlarged view
  does the same); the wall's cards ask again when it is closed.
* **`LiveLink`.** One live connection to a server, a `Live`. Since 2026-10-25
  it uses `DeviceLine`, never Qt's automatic-auth WebSocket.
  A pinned line has an empty CA set and checks the leaf before the upgrade.
  HTTP authentication challenges are final refusals, with no retry or Windows SSO;
  the socket uses NoProxy. The upgrade carries X-QKVM only, then the one-use ticket
  is the first message. DeviceLine's live mode bounds UTF-8 text and binary messages,
  fragmented messages, socket buffering and throughput; native kvmd/Janus modes
  retain their existing limits. It reads the 16-byte header
  of SPEC.md section 6.2 and hands the picture on; it sends keys and mouse. As built it
  is on the interface thread, as a `KvmdLive` is; the network thread of section 3.3 is
  the plan.
* **State machines, written out.** `Connection` (none, connecting, wants trust, wants
  sign-in, signing in, signed in, away and trying again), `Tile` (what the server says,
  plus "old" when the picture is older than three intervals), `Control` (off, asking, on,
  letting go). Each is an enum and one function from (state, event) to state: no flags
  spread over a class. Trying again backs off from one second to thirty, and starts at
  once when Windows says the network changed. (`Connection` is there, in
  `core/connection.h`. A certificate that is not the one a server is known by leads to
  "wants trust" from wherever the server was being spoken to, the wall included. While
  away, the server is asked at the wall's own pace, with no backing off yet. `Control`
  is not written out so: `MachineControl` keeps whether keyboard and mouse are asked
  for and whether they are held, and has one way out of both.)
* **Models.** `QAbstractListModel`s with roles; view models hold them. Nothing in QML
  holds data of its own.
* **The wall as a file, read and written here** (`core/exchange`, `exchange_work`, `scrypt`,
  `sealing`, `strict_json`, `wiped`; the rig `qkvm-exchange-rig`). The file is the server's
  own (the web app's `qkvm/exchange.py`): the same one, so that a server's KVMs come to this PC
  with their passwords and this PC's go to a server. Built: the file itself; and, in the
  core, putting what was read onto this PC's wall and writing the wall out (`WallExchange`,
  the next bullet); and the boxes on the screen (section 5, "This PC's wall as a file").
  The seal is the server's: scrypt of the passphrase, a file key sealed under that, the
  contents sealed under the file key, both with AES-256-GCM. Windows has AES-GCM and PBKDF2
  (CNG) and no scrypt, and Qt has none either; no library was added for it. scrypt is
  written here from RFC 7914 and held against that book's own vectors and, which counts
  for more, against files that the server's own code made (`core/tests/data/export`:
  made-up contents; a wrong scrypt does not open them). Its working words are zeroed on the
  way out, and are declared so that the optimised build cannot keep copies elsewhere. What
  held a passphrase's bytes or a key is wiped before it is freed.
  A file may be made by anybody. Its size, the numbers it asks of the key's derivation,
  and the lengths of what it hands over are looked at before any work or memory is spent:
  the most a file can cost is one derivation of 256 MiB and about a second. JSON is read
  by a strict reader of this piece's own (Qt's takes what the server's refuses, and cannot
  tell 1 from 1.0). Every field is taken by the server's rule, and more strictly where a
  library of Python's cannot be said in C++; a device's address is taken where the
  program's own rule takes it (`kvm::addressFrom`). What was left out is handed back
  with a sentence for each, beside what was read. No secret is in any sentence, and no
  address that was refused: a password can be written into an address in ways no look at
  its signs finds. A name from a file stands in a sentence only set off so that it cannot
  end its own setting-off (`nameSetOff`).
  A console link is kept as the text a file had (`LinkAsWritten`), since a server may get
  the file back; the one way from it to an address that may be opened or shown is
  `consoleAddress()`, which hands out words that are all ASCII (`ConsoleAddress`).
  The writer writes what the reader reads back, and a link as it was, unless a server
  refuses that link for certain by its own rule (a name or a password in it; nothing that
  begins as an address of the web; nothing where a host stands): then nothing is written,
  since neither program would read the device back, and a password would stand in a file
  that may be a plain one. What a server may
  not take of the file, or a file cannot say (a name in its `xn--` form; a KVM that is
  known here by its certificate's fingerprint only, which a server reads as "pinned, by
  whatever it shows first"), comes back as lines beside the file (`NotInIt`), and the
  file's text is handed out only to whoever says that those were looked at.
  `FileWork` runs a reading or a writing off the interface thread, one file at a time for
  the whole program, and tells whoever asked on the interface thread, unless that one is
  gone.
  For this PC's wall to write its KVMs' certificates, it keeps them, beside their
  fingerprints (`LocalWall`, above). And the box that takes a file in shows, before a
  device from a file is on the wall: its address as kept, in ASCII,
  with `xn--` flagged; that http means the password goes in the clear; what the file says
  of TLS and of the certificate; the name and the address together; that nothing was asked
  at that address yet (the box that adds a KVM asks what answers there before any password
  goes; a file brings everything unasked); and every sentence of what was left out. Each
  of these is laid out for that box by `WallExchange` (`Offered`, the next bullet).
* **This PC's wall and the wall as a file, joined** (`core/wall_exchange`, `WallExchange`).
  What the boxes on the screen call (`OwnFilesView`, one `WallExchange` for each window:
  `OwnWall::serve`; section 5, "This PC's wall as a file"). Nothing here is a box. A file
  may be made by anybody, and it holds the passwords of KVMs, so taking is
  two steps, and the first changes nothing. `layOut()` reads the file and lays out each
  device as an `Offered`: whether it can be taken and as what, or why not, with everything
  a person has to be shown before saying yes (the list above, in plain values and whole
  sentences), and no password. `take(number, places)` takes, of what was laid out last,
  the devices that are named by their places: each is taken or is not, what came of each
  is handed back, and the wall's list is written once for all of them
  (`LocalWall::takeFromAFile`). Neither step asks anything of any address. The list of
  devices is handed out only for the word that what stands beside it was looked at
  (`BesideTheList::wasLookedAt`).
  Only a KVM of the kvmd family is taken (`not_yet` for a JetKVM and for a server's
  controller), and never twice: a device at the place of a KVM of the wall, and one whose
  file names the certificate a KVM of the wall is known by, is laid out as on the wall
  already (`duplicate`, `same_device`), and of two such in one file the first is the one.
  Only what was laid out as to be taken is taken, however the wall has changed since, and
  each of those is looked at once more against the wall as it is at the taking. A KVM from
  a file is known by the file's certificate where the file says "pinned" and names it, and
  by Windows' word where the file says "system". Never by whatever it shows first, which
  is what a server makes of a file that says "insecure" or names no certificate: such a
  KVM is taken as known by Windows' word, and what Windows does not trust is put before a
  person before anything is sent to it (`notByWhatItShowsFirst`;
  `aKvmWhoseFileNamesNoCertificateIsNotKnownByWhatItShowsFirst`). Every KVM that is taken
  is on the wall as not asked yet (`LocalWall`, above).
  Kept between the two steps: only what can be taken, each password in `SecretBytes`; of a
  device that cannot be taken, the reason and nothing else. It is forgotten with a taking,
  with another laying out, with `forget()`, at the object's end, and after ten minutes
  (`keptForMsAsItIs`); `take()` says `not_laid_out` then. `forget()` also gives up a file
  that is still being read: when its reading ends nothing is laid out, and whoever asked
  is told `given_up` (`aFileThatIsGivenUpWhileItIsStillReadIsNotLaidOut`). What cannot be
  wiped for its type is said at the class.
  `write()` hands back the file as the writer has it (`exchange::Written`), with lines
  beside it on top of the writer's own: unsupported server alerts and local-only
  accounts, app password, layout and preferences; and each KVM whose password Windows
  did not hand out. `writeParts()` selects devices, saved actions and shared settings
  (picture interval and update checks). A plain file has no password or text action,
  and reads no secrets from the store. A wall that is too large for a file with its
  certificates is written once more without them, all of them or none, and the first
  line beside the file says so. One file at a time in the whole program (`busy`).
  Actions merge by the webapp's normalized, full-case-folded name, checked both at
  preview and apply. Settings are one replacement row, initially unchecked; only
  supplied shared fields change. Server alerts remain unsupported and are explained.
  Action text is held in wiping storage during preview and revealed only on request;
  it is absent from list metadata. Expiry, replacement, closing and locking discard it.
  `LocalWall::takeSettings` restores the remembered values after a failed write and
  notifies `WallPace` and `Updates` after success without unpausing the walls.
  Tried in `core/tests/test_wall_exchange.cpp` (sixty, as Qt counts them), against made-up
  KVMs on this PC: every wall there is a `WallOfATest`, which fails its test rather than
  have anything asked at an address that is not this PC's own. Section 11 has what is open.
* **Settings and secrets.** Settings are one JSON file under the user's profile
  (`%LOCALAPPDATA%\QKVM Desktop\settings.json`), written whole to a new file and renamed,
  and read as what anybody may have written: a file that is not the program's counts as
  none. But it is not written over. The list of this PC's own KVMs is in that file, with
  what each is known by; a file that another program held while this one started, or
  that is damaged in one place, would otherwise be replaced by an empty one at the first
  thing the program remembers, and every KVM would have to be added and looked at
  anew. So a file that is there and cannot be opened is tried for two seconds; one
  that could not be read at all (not opened, larger than the program writes, not JSON
  of the program's shape) is put aside, beside where it was (`settings.unreadable.json`,
  and `-2` and so on after it): by whoever starts the program, before anything is
  remembered, and at the latest before the first writing. Put aside is moved, never
  copied: what something still holds cannot be moved, stays what and where it is, and
  nothing is written until it can be. The first window says which of these it was,
  with the name the file has now (`OwnWall::saidOfSettingsThatCouldNotBeRead`). A
  file of the program's shape with something in it that the program cannot use is
  read, and that is left out, as before; a KVM's address counts as one only written
  the way the program writes it, since whether a KVM is on the wall already is asked
  by its address. The largest file the program writes (a wall of as many KVMs as one
  holds, each with the longest of everything) is one it reads back: tried.
  Secrets go through one small interface (`SecretStore`) to Windows Credential
  Manager, kept for this PC only, each under "QKVM Desktop: " and a name. There are
  five kinds: a KVM's password, under "kvm:" and the KVM's id (its form is said at
  `LocalWall` above); the text a saved action of this PC's wall types (`OwnActions`
  above), in parts of 2500 bytes (an entry of Windows' store holds 2560), each part in
  an entry of its own under "action:", the action's id, a colon, the place ("a" or
  "b") and the part's number ("action:0a1b2c3d:a1"), and of four lines: a first line
  `QKVM-ACTION-1`, then the id, then the writing's mark, the part's number and how many
  parts the writing has ("0123456789abcdef 1/2"), then the bytes of that part, which
  are all that follows; the key of a display link (below); the mark that this PC is a
  wall screen, under "wall screen", as the addresses of those servers, one to a line
  (section 5, "A wall screen"); and the session of a server, under "session at " and
  the server's address. A session is kept only where whoever
  signed in asked to stay signed in; it is forgotten on signing out, when the server
  says that it no longer knows it (its own 401: what answers 401 in the server's
  place has nothing forgotten), and when the next sign-in does not ask for it. It
  goes to the
  server it came from and to no other. The client holds the
  session itself, puts it on each question by hand, and takes a new one from the answer
  to a sign-in alone; Qt's store of cookies is never written, and so stays empty. Any answer that
  is still on its way writes into that store: a sign-out's, which takes the cookie back,
  when it comes after the next sign-in (the wall then said "signed out" a few seconds
  later), and a sign-in's that comes after another server was named, since a cookie by
  itself does not look at the port. And a sign-out lets go of the session here before the
  server answers: one that never reaches the server leaves nothing to come back in with.
  The settings hold the server that was used last and the names of the servers
  that were reached by HTTPS; one of those is not spoken to in the clear again (typed
  without "http://" it is taken to mean HTTPS, typed with it, it is refused). They hold
  the program's windows as well, in the order they were opened in: of each, where it was
  and how its wall was shown (group, order, columns, whether the groups take turns). A
  window's keepers read and write that window's own part (`core::WindowSlot`):
  `WallChoices` puts what was remembered on the wall at the start and writes down each
  change, and what is for the moment (the state that is looked at, what is searched for)
  is not written; `WindowKeeper` does the same for the place. Which part is a window's
  is its number among the windows, and changes when one before it is closed. No more
  windows are read, or written, than the program opens. They hold the section of the
  settings box that was shown when the box was last closed, or when the program ended
  with it open (`Remembered::settingsSection`, "settings_section"; section 5, "The
  settings"): only a key that names a section (`Remembered::namesASettingsSection`) is
  read or written, and anything else is none
  (`theSectionOfTheSettingsShownLastIsOneOfTheirsOrNone`). And beside each of this PC's
  KVMs the mark of whether a password is kept for it (`LocalWall`, above). No
  migrations while nothing is released.
* **Display links.** The key of a display link is a secret too, under "display link
  at " and the server's address, kept from the first time it opened a session. With
  one, nobody is shown a line for a password: the link is handed in while the screen still
  says that it is on its way to the server. When the wall's session is gone, the wall
  stays as it is and says that contact is lost. What answers there now is asked what it
  is first (`askHealth`: a server that was restarted may be another program than it
  was), and the link is handed in again only to a QKVM that speaks this program's
  version of the API (`comeBackByTheLink`). While nothing answers there, or something
  that is no QKVM, that is asked again at the wall's own pace, with nothing in the
  question that says who asks. The way back is paced as well. A link that is taken
  counts as taken only once a look at how the machines are was answered under its
  session (`m_linkTakenUnproven`, which the feed's `statusChanged` clears): until
  then the waiting below does not begin anew, and contact that was lost does not
  count as back: the wall goes on saying that it is lost, and since when it first
  was. Where the
  first look under it is refused as signed out, that taking counts as none, and the
  link is handed in again after the wait that is longer each time. (Each taking
  began the waiting anew before, and counted as contact back: a QKVM that took the
  link each time and never knew the session it gave for it had the link posted
  every few seconds for ever, and the wall said "in contact" for the length of one
  question each time and began "away since" anew.
  `aLinkThatIsTakenAndWhoseSessionNeverHoldsIsHandedInLessAndLessOften` in
  `ui/tests/test_server_session.cpp` holds both, the waits and what the wall says
  meanwhile.) A session that held, and is
  gone a moment after its link was handed in, is not come back to at the pace of
  the answers, but no sooner after the last time than a server that is away is
  asked again (`handedTheLinkInJustNow`). Only the QKVM's own word that the link is
  not good (`bad_key`) forgets
  it, and somebody is asked to sign in. Whatever else comes of handing it in (no
  answer, a gateway's error, "too many", what is not understood) leaves it kept: tried
  again later while the wall is up, and said where the server is named the first time.
  Later is longer each time (`handTheLinkInLater`): the pace a server that is away is
  asked at, twice as long for each such handing-in in a row, five minutes at the
  most, and never less than the server asked to be left alone for, where it said so
  with a refusal in its own shape (`Retry-After` in whole seconds, of which no more
  than the same five minutes are taken, `ServerClient::longestLinkWaitMs`; what a
  gateway's page names is the gateway's guess and counts for nothing:
  `ServerClient::linkWaitSaidMs`). A link that is taken, and under whose session a
  look at the machines was answered, begins that anew, and a
  handing-in that was not sent at all (`desktop:not_sent`) counts for nothing. At the
  wall's pace for ever, a link that a QKVM would not take was posted to it some
  twenty thousand times a day.
  There it is tried again too, where the server was named by the link that is kept
  for it (`cameByItsKeptLink`): a wall screen that starts a moment before its server
  is up has nobody at it to name the server again. Such a screen goes on asking by
  itself, from the first question, at the pace a wall's server that is away is asked
  at, or after the longer wait where its link was handed in and not taken
  (`notComeTo`), and says that it does (`SessionView::tryingAgain`). It is not busy
  meanwhile: another
  address can be typed, and whoever types there or names a server ends the asking
  (`stopTrying`), so that no wall comes up under what is being typed. A link that
  was pasted just now has somebody at it, and is not tried by itself.
  A QKVM that speaks another version of the API is said where a server is named; the
  link was not handed to it, and stays kept, and it is not asked again by itself:
  somebody has to update one of the two first. So it is where contact was lost
  while a wall was up, with a link or without one, and what answers there
  afterwards is a QKVM of another version (`WallFeed::otherVersion`): the wall is
  left for where a server is named, with that sentence, and not left standing to
  say "contact lost" for ever; nothing was sent to that QKVM, and what is kept for
  the server stays kept
  (`aQkvmThatComesBackSpeakingAnotherVersionOfItsApiIsSaidWhereAServerIsNamed` in
  `ui/tests/test_server_session.cpp`). Signing out forgets the link too. It is
  that server's and no other's. And a display link's use makes this PC a wall screen,
  which only shows until a person signs it out here (section 5, "A wall screen"): the
  link taken back, contact lost and everything else above leave it one.
* **Finding (`core/finding/`).** One `Way` per method of SPEC.md section 7, all feeding one
  list keyed by what a device is. Every address passes one function, `mayAsk`, before
  anything is sent to it: that function is the manners of section 7, and it has the most
  thorough tests in the program.

## 5. The interface

* **View models hold everything; QML shows it.** A QML file binds to properties and calls
  the view model's functions. It has no network calls, no timers that carry logic, no
  rules. One view model per screen, made in C++, handed to QML by name.
* **Controls of our own**, built on Qt Quick Templates, in `ui/qml/controls`: button, icon
  button, field, toggle, menu, dialog, badge, tooltip. One radius, one height, colours
  from `Theme.qml`, which holds the page's values for dark and light. None of Qt's
  ready-made styles is used: they do not look like QKVM, and Qt itself calls its Windows
  style unfinished. A field that hides what is typed (`Field.qml`, `secret`) has an eye at its
  right edge, as the page's has: pressed, what was typed is on show; pressed again, it is
  hidden. It is hidden again by itself when it is sent (`conceal()`, called by each screen
  where it sends) and when the field goes out of sight. The eye is no stop for the Tab
  key, and what is typed into such a field is marked as nobody's to learn or to suggest,
  on show or not. (Tried: `whatIsTypedIntoALineThatHidesItIsShownWhileItsEyeIsOpenAndNoLongerOnceItIsSent`.)
* **Doing things to a machine.** Three parts, each with one job. `machine_menu` is the
  rules and nothing else: from a machine, how it is and what the server has saved, which
  rows its menu has, what each row sends, what it is called and what is asked first, in
  the page's words. It has no network and no screen, and its tests are a table of
  machines. `MachineActions` does what the screens ask: opens a menu and keeps it as the
  machine is, asks first where a row says so, sends through `WallSource::act` (of the
  place the machine's key reaches: section 4, and "The sources behind the wall"
  below), and tells how it went. It notes which machines something is out to, by
  their keys on the wall, in a table every window's actions share (`UnderWay`), so
  that a second thing is said and not sent, from whichever window. `ActionsView` is
  what the screens are handed, and decides nothing. An answer is taken only in the
  turn of the source it was asked of: leaving that source's wall makes what is still
  out to it nobody's. An error of `act` says whether the machine may have done
  it all the same (`mayHaveBeenDone`, asked of the place that was sent to, for one
  machine and for a run over several); that is told as such and never tried again. After
  something was done the feed looks at the machines sooner than at its usual pace
  (`WallFeed::lookAgainSoon`, the page's 1, 3, 6 and 10 seconds).
* **Two walls, one window.** The screens show a QKVM server's wall or this PC's own
  (`ServerSession`, with `useOwnWall` given once by whoever makes the program), and
  never a mix. Inside, the windows' wall has both sources on it, this PC's first and
  the server's after it (the next bullet), and only the one whose wall is shown has
  anybody at it. On the way from one to the other the one that is left is left (the
  server stands aside, `ServerSource::standAside`; this PC's wall says itself that it
  is left, `PcSource::leave`), its feed is stopped and its cards go, the other is gone
  to, and whatever was under way for the wall that is left reaches nobody (the turn
  of the source each answer is asked of). That holds also where no wall
  of a server's was up and a server was only being asked what it is, or who is signed
  in: going to this PC's own wall ends that asking. (Its answer was taken once, the
  kept session sent after it, and the server's wall came up under this one.) At this
  PC's own wall nobody is signed in to anything: the view says `ownWall`, `manages`
  (whoever sits here may do everything, but while the program only shows: "A wall
  screen", below) and `encrypted`, no account and no address.
  The server that was used last is not signed out of when its wall is left for this
  PC's, and is asked nothing while the other wall is shown. Which wall was shown last
  is remembered (`Remembered::showsOwnWall`), and the program starts into that; one
  that never used a server starts into this PC's own wall. What is a server's only is
  not offered here: the server's boxes for the wall as a file (this PC's wall has its
  own, "This PC's wall as a file", below), the server's page (the bar's button opens the
  box that adds a KVM instead, and the icon's menu has no row for a page), signing out
  (the account's box has "Connect to a QKVM server" in its place). What is spoken to
  over plain HTTP is said: the bar has its "Not encrypted" where one KVM of the wall
  is, and that KVM's card writes `http://` before its address (`Machine::plain`, which
  this PC's wall sets and a server does not say). A KVM that is added or taken off is
  on the wall, or off it, at once (`WallFeed::askAnew`; where the list is being asked
  for that moment, it is asked for once more when the answer has come, which may have
  been made before the change), and looked at again soon after, as after something
  done to a machine.
  `OwnWall` (`ui/own_wall`) holds this PC's wall for the program: the `LocalWall`,
  the `KvmAdding`, the `KvmEditing` and the `KvmCertificateReview`, and the tie
  between the boxes and these. `AddKvmView` is what a box (`AddKvm.qml`) is given,
  one for each window (`WallWindows` makes it, `OwnWall::serve` ties it): it passes
  the wish on and shows what came of it, and keeps nothing; what was typed, the
  password with it, is emptied when the box closes. One KVM is added at a time,
  whichever window asks: a second box is told to wait, and is told nothing of the
  first one's KVM. While a KVM is being asked the box is not left by a slip of the
  hand; its "Cancel" gives the asking up, and so does a box that goes (its window
  closed, the wall left for a server's, which closes the box). A KVM that was added
  all the same, in the very turn in which its box was closed from outside, is said by
  the window, with the certificate the box would have shown (`addedAsItClosed`;
  "Notices", below). After a KVM was added the box says which certificate it is known
  by from now on ("Remembered SHA-256"), to be compared with what the KVM says of
  itself, and stays for the next, which is signed in to as the last was. After one
  that was not added, what was typed stands and the keyboard is back in the line it
  was in. Its lines scroll where the window is too low, and what came of the adding
  and its buttons stay in sight. An empty wall of this PC's says what it is for and
  has the button for the first KVM, where a wall that the bar's choices leave empty
  says "Nothing to show" as ever. A card whose machine is called by its address
  writes the address once.
* **The sources behind the wall.** The program is being made to show several sources
  at once, in one device tree (the owner's decision of 2026-10-17: the opening of this
  document). The change is cut into eight rounds, R1 to R8, each of which leaves the
  program whole. R1 to R3 changed nothing of what it does: reviewers read each, and
  the large test files of the session are byte for byte as they were. R4, with its
  mending R4b, put several sources inside the windows' wall at once; a reader of each
  found nothing that sends a press, a key or a session to the wrong place. The
  screens still show one source at a time, as above: R7 changes that. What the rounds
  made of `ui/`: the wall's machines come from places (`WallSource`), each kept by a
  `WallFeed` of its own. A source (`Source`, `ui/source.h`) is one such place with its
  feed, as the windows' wall has it: its id, whether somebody is at its wall, whether
  it is away, whether a machine of it may be had live, whether whoever is there makes
  its order, and a turn of its own. A QKVM server is a `ServerSource`
  (`ui/server_source`): its client and feed, where it is with the server
  (`core::Connection`), the sign-in and its second step, display links,
  certificates, contact, the wall as a file, and what is remembered. This PC is a
  `PcSource` (`ui/pc_source`, whose id is `pc`): its `LocalWall` and a feed of its
  own, and whoever sits here may do everything on it (`PcSource::whoever`), but while
  the program only shows (the next bullet). A source says what happened (`entered`,
  `whoChanged`, `left`, `emptied`, `stateChanged`) over direct connections, and the
  windows' part follows there and then.
  What the windows do is `MergedWall`'s (`ui/merged_wall`): each window's wall,
  actions, live picture, and keyboard and mouse, the pictures' streams, and the wall's
  own order. It holds any number of sources at once, in the order they were added
  (`addSource`, which takes no source whose id is none or is that of one on the wall
  already; `removeSource`, which takes everything of it away, its streams out of
  every engine, and nothing of another's). The cards are one list: the sources in
  their order, each in its own. A source's machines are on the wall from its
  `entered` to its `emptied`, and only once its feed has told them since somebody came
  to its wall: what a feed holds from before is the wall of a session before, maybe
  another server's, whose keys would reach the new one by the old ids. (A window
  opened between a sign-in and the feed's first answer showed the session before's
  machines.) A feed's word makes its own source's cards anew and no other's, and then
  all are joined. A machine's key is `<source>/<machine>` (`wallKey` in
  `ui/wall_key.h`), made where the cards are filled and kept there in a table of what
  each key reaches (`WallMachines::reach`: the place, the machine's own id there, and
  the source's id, `Reached::source`). A key that is not in the table reaches nothing,
  and a key is never taken apart to find a machine. So two servers that each have a
  machine called "a" have two cards and two streams, and what is done goes to the one
  it was done to. Until servers are kept each with an id of its own (R5), the one
  server's id is `server` (`ServerSession::serverId`). `MergedWall` is the only
  listener to each feed's word of the wall, of how the machines are and of their
  pictures. It fills the cards first; of how the machines are, it then tells the
  source (`Source::looked`); and last of all whoever does things to the machines.
  Those (`MachineActions`, and so `BulkRun`; each window's `LivePicture`) ask through
  `WallMachines` (`ui/wall_machines`), by a machine's key on the wall, and hold no
  place of their own. (`OnePlace` is one place and its feed by themselves, for the
  tests, where a key is the machine's own id.)
  Each source is a matter by itself. A card says which source it is of, whether that
  source is away, and whether whoever sits here may act on it (`WallTile::source`,
  `away` and `actable`, roles of the same names): a server that does not answer greys
  its own cards and no other's. A card may be acted on only where the windows' wall
  says so (`actable` is false where nobody stamps it), and only such a card is picked:
  one that may no longer be acted on is let go of (`WallModel::setPicked`, `pick`).
  The screens do not go by these yet: "old" is still one banner over every card, and
  whether a card has a menu is `ActionsView::allowed`, which means "at any source"
  (R7). In `MachineActions` each source has its own turn (all drawn from one count, so
  that no turn of one source is ever another's, nor one it had before), whether
  whoever sits here may act there (`enter(source, mayAct)`: by that source's word, an
  operator or an admin there), and its saved actions. Every answer counts only in the
  turn of the source that asked. A source whose wall is left, or that lets nobody act
  any more, takes its own with it and nothing of another's: its questions, menus and
  boxes, what is under way to its machines and its notices of that, its picks, a pass
  into a console that is being made out, and a run that touches one of its machines;
  only where nobody may act on anything here does everything go. What is under way is
  kept by the key on the wall (`UnderWay`), so that one id at two servers holds
  neither back. A saved action is its source's: a machine's menu lists its own
  source's; a menu for several offers them only where every one of the several is of
  one source, and otherwise leaves "Your actions" out, until R7 words it
  (`savedLeftOut`); and the rule is held again where the action is sent, for one
  machine and for a run (`MachineActions::goesTo`), so that a saved action's id never
  runs whatever has that id at another server. Keyboard and mouse go by the word of
  the source of the machine that is shown large (`MachineActions::mayActOn`), and so
  does whether it is had live (`Source::mayBeLive`). Pictures are asked of each source
  for its own machines, by their own ids there, each at the widest it is drawn in any
  window, and those looked at closely likewise. The wall's own order is made for one
  source at a time (below, "The wall's own order"). What a source that nobody is at
  says of whoever is at it is nobody's (`MergedWall::know`). Whoever comes first to a
  wall nobody is at sees it as a person does, out of display mode; whoever comes to
  another source's wall beside one that is up leaves every window as it is, as
  somebody may have chosen it. The pace that holds until another is chosen is the one
  the first source that somebody is at names (a second where there is none), and
  every other such source's is among the choices as well (`WallView::setOtherPaces`);
  how the choices are named where there are several is R7's.
  Tried in `ui/tests/test_merged_wall.cpp`, against two stand-in servers and a
  made-up KVM, each rule seen to fail with it taken out (but the rule for a port
  switch: the single machine's own check stands behind it, and only with both taken
  out does a test fail): `machinesOfEverySourceAreOnTheWallEachInItsOwnOrder`,
  `twoServersEachWithAMachineCalledAAreTwoCards`,
  `aServerThatIsAwayGreysOnlyItsOwnTiles`, `aServerThatSignsOutTakesOnlyItsMachinesOff`,
  `leavingOneSourceDropsNoAnswerOfAnother`,
  `picturesAreAskedOfEachSourceForItsOwnMachinesAtTheWidestWidth`,
  `whoMayActIsEachSourcesWord`, `keyboardAndMouseAreEachSourcesWord`,
  `aSavedActionGoesOnlyToMachinesOfItsServer`, `theOrderIsMadeForOneSourceAtATime`,
  `aSourcesWordAndItsLeavingStopOnlyAnOrderOfItsOwn`,
  `whatALeavingSourceHadUnderWayIsOverWhereAnotherStays`,
  `aSourceThatLeavesTakesItsStreamsOutOfEveryEngine`,
  `aWindowOpenedBeforeAFeedHasAnsweredShowsNothingOfBefore`,
  `whatASourceNobodyIsAtSaysOfWhoeverItIsIsNothing`,
  `anotherSourceThatComesLeavesTheWayEveryWindowShowsTheWall`,
  `everySourcesPaceIsOffered`, and those of the next bullet. Each of those in which a
  source leaves has a second one stay beside it, so that none can pass because this
  PC's wall is always there.
  Until R6 `ServerSession` is a facade, with the interface it always had: it routes
  what the screens ask, holds the mark of a wall screen (the next bullet), puts this
  PC on the windows' wall first and the server after it, says of which source an
  order is made (`MergedWall::setOrderOf`: the one whose wall is shown), serves the
  boxes for the wall as a file and the box that changes a KVM, and goes between the
  two walls, one at a time. To come: the servers kept in the settings as a list, each
  with an id of its own, and the start into the first one that is shown, or into this
  PC's wall (R5); `Sources` owning them, the facade gone, and a `ServerClient` that
  ends with questions out and tells nobody (R6); the screens for several sources: the
  wall always there, "old" and a menu per card, the banner naming each server that is
  away, an Add menu for a KVM or a server, the paces' names, "Your actions" for
  several, and a sign-out where the program only shows (R7); the device tree, which
  narrows each window by source and group (R8).
* **App lock and saved actions (2026-10-21).** `AppLock` now supplies the local
  password gate described in SPEC section 8. Its restriction is included in
  `MergedWall::onlyShows`; `OwnWall`, local file operations and the saved-action
  editor enforce it in C++. A successful local password check can clear all
  display links and screen marks without contacting a server. This supersedes
  the earlier sign-out limitation and unguarded add/import/export paths in the
  historical R4 notes and section 11. Password work runs off the UI thread.
  Export approval belongs to one request and is cancelled on lock, reset or
  destruction of the requesting view. A confirmed reset enumerates this app's
  credential prefix, erases orphaned entries too, and closes the app after
  complete erasure. Failed erasure leaves recovery active; the settings-side
  nonsecret marker also records an unfinished reset when Credential Manager
  refuses its recovery marker. If both writes fail, no secrets are erased and
  the persistence failure is reported. The saved-action
  editor uses the existing revision checks and never reads saved text back.
  By the owner's decision of 2026-10-25 there is no adoption of older profiles.
  First setup requires an empty credential prefix and no settings/profile siblings/remnants/lock
  (the key of the wrong-password counts, `sign-in-pacing`, made before the lock looks, is not counted).
  A valid password missing its marker still requires that password to finish setup.
  Recovery erases the whole prefix, rechecks emptiness, then writes a nonsecret
  completed-reset marker that permits retaining inventory on the next setup.
  Credentials appearing while password derivation runs prevent adoption too.
* **A wall screen** (`WallScreen`, `ui/wall_screen`; historical R4 description,
  extended by the app lock above). From the moment a display link
  signs a QKVM server's wall in here (whoever is at it is a display link's:
  `core::Account::isDisplay`), this PC is a wall screen, and the whole program only
  shows (`MergedWall::onlyShows`: this PC is a wall screen, or a source is signed in by
  a display link just now), until a person signs that server's screen out at this PC.
  That goes through one function, `ServerSource::signOut()` (the screens' "Sign out"
  while that server's wall is shown), which alone ends a screen here
  (`WallScreen::signedOutAt` has no other caller): whatever is to be asked before a
  screen is signed out is asked in front of it, and the program's own password will
  be (M3d; SPEC.md section 8). Nothing else ends it: not the server taking the link
  back (`bad_key`: the screen says so, and still only shows), not contact lost, not a
  QKVM of another version, not a certificate that has to be looked at, not a start
  while the server is away, not a restart of the program with the link kept, and not
  a person signing in at another server or out of it. The mark is kept where the
  links are, in Windows' store under "wall screen": the addresses of those servers as
  their clients write them, one to a line, read once at the start. What forgets it is
  said in one place (`wall_screen.h`): a person signing out each of those screens; a
  Windows that does not keep the entry (the program holds it all the same while it
  runs) or cannot read it at a start; and whoever takes the entry out of Windows'
  Credential Manager by hand. While it only shows, nobody acts on any machine of any
  source, this PC's included, by any road: a menu and the box for keys or a text, a
  run and a run sent again, a port switched, a pass into a console signed in (its
  door, where one signs in oneself, still opens, as it does for whoever may only
  look), the card's button, saved actions, picking, keyboard and mouse, the wall's
  order; and of what the facade serves, the server's boxes for the wall as a file
  (each refused at once in the program's own code, `desktop:not_sent`, and nothing is
  asked of the server) and the box that changes or takes off a KVM of this PC (it
  opens for none, `EditKvmView::setMayChange`). This PC's KVMs are shown. Where it
  begins, whoever was making an order stops, and every window shows the wall and
  nothing else (display mode), as does a window that comes meanwhile; "Exit (Esc)"
  still leaves display mode, and nothing acts all the same. Where it ends, whoever
  sits here may act again where each source says so, and the windows are left as
  they are. A display link's session whose role at its server is operator or admin
  could act before R4 while its windows only showed; now nobody can, and that is
  meant. Open, each with its row in section 11: this PC's own boxes for its wall as a
  file and the box that adds a KVM, which are not held yet; a screen whose server took
  the link back, was given another address or is gone, which the program can hardly
  or not at all end; and a mark that anybody at the unlocked Windows session can take
  out. A person who walks by can sign the screen out and then act on this PC's KVMs:
  that is what the program's own password will stand in front of. Tried in
  `ui/tests/test_merged_wall.cpp`, each with this PC's KVMs beside the screen and each
  seen to fail with its rule taken out: `aDisplayLinkLetsNobodyActOnAnyMachine`,
  `aScreenWhoseLinkIsTakenBackStillOnlyShows`,
  `aScreenWhoseServerComesBackSpeakingAnotherVersionStillOnlyShows`,
  `aScreenWhoseServerShowsAnotherCertificateStillOnlyShows`,
  `aScreenThatStartsWhileItsServerIsAwayOnlyShows`,
  `aScreenThatIsStartedAgainWithItsLinkKeptOnlyShows`,
  `onlyAPersonSigningTheScreenOutHereEndsIt`, `whileTheProgramOnlyShowsNoRoadActs`,
  `anOrderMadeWhileTheProgramOnlyShowsIsRefused` and
  `aWallScreenChangesNoKvmOfThisPcAndAsksNothingOfTheWallAsAFile`. Windows' real
  store was not used: the tests keep secrets in memory.
* **Changing a KVM of this PC's wall** is a second box, of the same make as the one
  that adds (`EditKvm.qml`, `EditKvmView` one for each window, `OwnWall::serve` and
  `OwnWall::openFor`). The ways into it are there only for a machine that says it is
  this PC's own (`Machine::own`, which `LocalWall` sets and nothing else): the row
  "Edit…" in its menu, and its card's own button, in the page's words, "Update
  login" where the KVM refused the sign-in and "Review certificate" where it shows
  another certificate than it is known by (`cardWayOf` says where the button leads;
  `cardFor` clears that word for every other machine as before); and the settings' list of
  this PC's KVMs, whose "Edit" opens it as it is and whose "Remove" opens it at its
  question alone ("The settings", below). The box changes the
  name, the address, the user name, the password (left empty: the kept one) and the
  group through `core::KvmEditing`, and shows each refusal in the sentence it comes
  with. "Remove" asks first, in the box, in the page's words. "Review certificate"
  shows in the same box what the KVM is known by beside what it shows now, as the page
  writes them, and where they differ the page's warning and "Trust this certificate".
  Two stand there too where a saving is refused over an address that was typed, that
  address written out with the second: with its address changed, a KVM is signed in
  to only where it will be known as it is known now, or after somebody was shown
  what that would become, beside what it is known by now, and said yes (section 4
  has the rule, section 11 what it leaves open). The second is one of three. A
  certificate that is shown there (`certificate_differs`). Windows' word, where
  Windows trusts what is shown there: said in the place of a certificate, and the
  button reads "Trust Windows' word" (`onWindowsWord`). Or, where the address is
  plain HTTP and the KVM is spoken to over HTTPS (`plain_http_there`,
  `plainHttpThere`), what is given up: nothing shows who answers there, and its
  password, and what is seen and typed, can be read on the way; the button reads
  "Use plain HTTP". A KVM that is known by Windows' word is shown as that in the
  place of the first, with what a yes gives up. The lines keep what was typed
  meanwhile, and the yes saves it. The view keeps what the last yes was to at each
  address that was typed, while the box is open for this KVM and not filled anew.
  What is shown there at a later saving and is another (another certificate, or
  Windows' word for a certificate, or the other way round) is said, marked, and
  takes its yes only after a longer rest (`yesWasToAnother`, `yesWasOnWindowsWord`,
  `trustRestsAnewMs`), whatever was asked in between: what answers there may show
  one to every question and another only to the sign-in. So it is in a look, where
  the KVM shows another than the one that was just said yes to. Where a look finds
  the certificate of another KVM of the wall, the box says whose it is and has no
  yes (`presentedIsCalled`). What is offered is what is shown: saying yes hands on
  the very thing the box showed, never one read again in between; `OwnWall` keeps
  as offered to a box only what the box shows once it was told to (`keepAsOffered`,
  `EditKvmView::offers`), and takes a yes only where the box says, with the wish
  that carries it, that this is what its button was pressed on (`saysYesTo()`), and
  once. Anything else is a saving without a yes, and nothing is trusted. The button
  stands in the line of the box's title (`inTitle` of the dialog), where no button
  of the box stands in any other state: the two arrive when whatever answers at the
  address has answered, and a click that was on its way to what stood there before
  is no yes. It takes no click for a moment after the two have come
  (`EditKvmView::trustRestsMs`, which the view keeps whatever the screens ask), the
  keyboard does not reach it, and that moment runs only while the two are looked at
  (`lookedAt`: the box open on them, in the active window). A saving after which the
  KVM is known by another certificate than the box showed, or is spoken to in the
  clear and was not, leaves the box open on the KVM as it is now, with what it is
  known by written whole under the lines; any other closes it with a notice. While
  the program only shows, a box that is open is closed, and none opens for any KVM or
  passes anything on (`EditKvmView::setMayChange`, which the session sets: "A wall
  screen", above). One change and one look at a certificate at a time, whichever
  window asks. "Cancel"
  while the KVM is asked has the one that does it give up (`KvmEditing::giveUp`,
  `KvmCertificateReview::giveUp`), and the box is told what came of it: that it was
  given up, or, where the wall had taken it in that very turn, that it was saved. A
  box that is closed, a box that goes and a window that goes end what was asked
  there and then (the change or the look is thrown away and made anew, so that the
  next box does not wait a turn for it), and nothing is saved or trusted after
  that. What had been saved or trusted in that very turn is said by the window, as
  a notice that waits until the window is seen again (`cameTo()`, `doneAsItClosed`;
  "Notices", below). A window that goes out of sight, closed or behind the
  program's icon, has its boxes closed (`WallWindows::closeBoxes`, one rule for this
  box and the one that adds). Nothing typed is kept when the box closes, and it
  closes with the wall. What a box asks, it asks over the KVM as the box was filled
  with it (`KvmEditing::Seen`, which `OwnWall` keeps for each box and the screens
  never hold; `filledWith`): a saving, a look at a certificate and the yes to it
  alike, and of that KVM only, so that a wish which names another is nobody's. A
  KVM that another window changed since is not saved over, read or trusted
  ("conflict"), and the box is filled anew with it as it is (`fillAnewOver`). A box
  that is open follows the wall: its title says what the KVM is called now (of any
  other change it learns only when it asks, as above); and where the KVM is taken off
  the wall, by another window or a file, the box says so at once and stays where it
  is, as large (no larger than a window made smaller has room for), whatever it shows
  (`EditKvmView::gone`): at its question in the place of the question's words,
  otherwise where it says what came of something, in room the lines above give up.
  What would do something takes no click, and the view passes nothing on for that KVM
  any more; the way out closes it and has the keyboard, and a saving under way ends
  with that word.
* **Who is signed in** is asked when the wall is come to, and again with every fifteenth
  look at the machines, which is the page's "about once a minute": an admin may have
  changed elsewhere what the account may do, or what it is called. `MachineActions` is
  told what the account may do now, for that server alone (`enter(source, mayAct)`).
  Where it may act no longer, that is as if that server's wall had been left by one and
  come to by another: what was open of it is closed, what is still out to it is
  nobody's, and none of its machines is picked; another source's stay as they are. The
  question is beside whatever else is asked and has no turn of its own; its answer is
  dropped once the wall was left.
* **What the program finds itself, and what a server says.** An error has a code, and
  the server's refusals bring the server's own. Seven the client finds itself, and much
  hangs on each: the server was not reached (something may have been done; the wall
  waits), the session is gone by the server's own 401 (the wall is left), the
  certificate is unknown or another
  (somebody has to look), the certificate the server is known by was signed by somebody
  Windows does not know (its sentence says what to do), a QKVM answers that speaks
  another version of the API (nothing more is asked of it, and a wall that was up is
  left), and a call was not sent,
  since contact had been lost and what answers there has not said that it is a QKVM
  (`desktop:not_sent`: nothing went anywhere, and nothing may have been done; section
  4). They are written
  `desktop:…`, and a server's answer that is
  written with a colon is not taken as a word (`refusalFrom`). One more is written so
  and is not the client's: `desktop:tls_damaged`, for an HTTPS address where TLS is not
  Windows' own (section 4). And `desktop:not_sent` is said by the session too, for
  what the wall as a file never sent ("The wall as a file", below). The server's own
  word for
  a machine it cannot reach is `unreachable` too, and before the two were told apart such
  a machine was said to have "maybe done it" and kept its last picture.
* **One thing done to several machines.** The same three parts, and a fourth. The rules
  (`menuForSeveral`) make one menu of what any of the picked machines can do, and say what
  takes a tick as well as a yes: whatever is marked as a danger, a power button of any
  kind, and a text. `MachineActions` asks about all of them, naming each, and holds the
  tick to be a condition of its own: a yes that comes without it is no yes, whatever a
  screen lets through. `BulkRun` is the run: a few at a time, or one after the other with
  a pause; what is on its way cannot be called back and is waited for, what was not sent
  is not sent once it is stopped. It sends nothing itself and keeps no list of what is
  out to which machine: it is handed the ways to do both (`BulkRun::Ways`), so that a run
  and a machine's own menu know of each other through one list, and its tests need no
  server. What got no answer is counted as that and is not among what is offered again;
  what was refused and what was never sent is. One run at a time: a second is said and
  not begun, before anything of it is asked or anything is closed, so that the run that
  goes on keeps where each of its machines was sent and whose word its answers are. A
  run's entries in the server's log are tied together by one word of the program's
  making (`Action::batch`), as the page's are. Only a card that may be acted on is
  picked, and of picked machines whose source lets nobody act none is taken; a saved
  action is offered for several only where all are of one source, and goes only to
  machines of the source it is of, where it is sent as well ("The sources behind the
  wall", above).
* **A machine's own console** opens in the browser of whoever sits at the program, never in
  it. What is handed to Windows to be opened is an address that came from a server, and
  behind it from a device: only http and https with a host and without a name in it get
  that far (`core::webAddress`, where the wall is read), since Windows opens anything else
  with whatever it has for its kind, a program among them. A pass into a server's console
  is followed only to the place the wall names for that machine (`core::sameSite`): a
  server that names another place has a browser sent nowhere. The program is given the
  way to open a browser from outside (`openPagesWith`); a test gives its own, and none
  opens one.
* **Notices.** `Notices` is a list model: told, they go by themselves after the time the
  page gives them; what is under way stays until whoever began it says it is over. The
  screen draws them in the layer the dialogs are in, above them. What was done while
  nobody may have been looking, and is said nowhere else, is told otherwise
  (`tellWhenSeen`, and `ActionsView::tellWhenSeen` for the screens): where nobody sees
  the window (`setSeen`, which `WallWindows` says: seen is a window that is in sight,
  not minimized, and the active one), the notice stays until somebody does, and has
  its time from then; told to a window behind the program's icon, or one that is
  only in sight again behind another program's, it would have come and gone unread.
  It does not make room for a newer one meanwhile. The two boxes of this PC's wall
  tell so what was added, saved or trusted in the very turn in which the box was
  closed from outside. A notice that still waits when its window goes for good is
  taken out (`takeWhatWaits`) and told in the first window that is open, where it
  waits for that one to be seen (`WallWindows::forget`). Not where the program ends,
  as a whole or by its last open window being closed: there is no window to say it
  in then, and it is not kept for the next start. A certificate's SHA-256 in a
  notice stands on lines of its own, eight pairs to each, as its box shows one
  (`NoticeList.qml`): it has nothing a line could be broken at, and was broken
  inside a pair.
* **How often a picture is asked for** is three things kept apart: the pace the server
  names (`WallFeed::serverPaceMs`), the pace that was chosen on this PC and is remembered
  (`WallView::chosenPace`), and a pause, which is for now and is kept nowhere. The view
  makes of them what the screens show (`pace`, and `paces` to choose from) and what the
  feed is told (`ownPace`: nothing while the server's pace holds). The feed keeps its
  timer and asks for nothing while paused, except for a machine that is looked at
  closely: which machines those are is said to it by name, and the others go on at the
  wall's pace, since one window may show a machine large while another shows the wall.
  Where the wall has the machines of several sources, the pace that holds is the one
  the first source that somebody is at names, and each other's is among the choices
  too (`WallView::setOtherPaces`, `offeredPaces`): a pace that was chosen because one
  server named it stays a choice while another server's names the one that holds.
* **The wall's own order, made by hand** ("Arrange the tiles…") is a state of the wall's
  view as well: `WallView::arranging`. It is made for one source at a time: the one
  the windows' wall says when it begins (`WallView::setOrderOf`, which the session sets
  to the source whose wall is shown), kept for as long as it lasts whatever is said
  afterwards (`arrangingOf`); a wall that has no machine of that source begins none.
  While it holds, the filter shows every machine of that source as it came, and no
  other (`WallFilter::setArranging`), over whatever the bar had chosen, which is
  untouched and holds again afterwards; and the view holds the order that is being made
  (`WallView::arrangement`), of that source's machines alone. The screen draws each card where its machine stands in that
  order (`Wall.qml`: a card's `at`), so a card that is moved is still the card it was: it
  keeps the keyboard, where its place was typed into it, and is seen to glide. (The order
  was the filter's at first, sorted anew for each move. A filter that sorts anew has
  Qt Quick make every card anew, also where the filter says that it is a sort and no
  more: the card lost the keyboard, and none was seen to move.) A card's move is
  `placeAt`, whatever the screen made it from: a drag, a step, a number. The layer on a
  card is in `Tile.qml`; which card a dragged one is over is the wall's to say
  (`Wall.qml`); the drag is a `MouseArea` that does not let itself be taken over, since
  the wall under it would take a drag for a push to scroll. "Save this order" is a
  signal of the view's (`orderAsked`); the windows' wall sends it to that source alone,
  by its machines' own ids there, for an admin of it (`MergedWall::arrange`, and for a
  server `ServerClient::arrange`). It refuses, and sends nothing for, a list that mixes
  sources, names a key that is not on the wall or a machine of another source than
  the order's, comes where whoever sits here is no admin of that source, or comes
  while the program only shows. What the source hands back, the order of all its
  machines, is given to its feed (`WallFeed::takeOrder`): the wall is in it at once, in
  every window, without being asked for again, and without a moment in which it shows
  the old order. The answer counts only in that source's turn, and only while it is
  the same taking of that source on the wall (one taken away and added again is
  another). A refusal is told in the window that asked, and the order stays as it was
  made. That source's wall being left, or its word that whoever sits here is no admin
  there any more, ends it, and nothing of another source's does; so does the program
  coming to only show.
* **The wall as a file** ("Export…" and "Import…" in the account's box, for admins) is the
  server's work from end to end: it makes the file and it reads one, and this program
  neither opens a file's contents nor writes one of its own making. `ServerClient` has
  the three calls (`exportWall`, `readFile`, `takeFile`), each with a longer time to
  answer than a call has otherwise, since a passphrase costs the server seconds, and
  with an answer as large as a file may be. What comes back is read as everything from
  outside is (`server_types`): what the server answers to an export is written out only
  where it says of itself that it is a file of QKVM's (the sign-in page of something
  that stands before the server is not kept as the wall); a file's contents are
  counted and held to what a wall holds; a saved action that presses keys has no text,
  whatever is said of it; and what an action types is whole or the reading is refused,
  since a text that was cut would hide its end.
  `FilesView` is what the two boxes of a window are handed: one for each window, made
  by `WallWindows` beside the window's wall and its actions, so that what one window
  has chosen, read or asked for is not another's. It holds the file that was chosen and
  what the server said is in it, and makes the page's sentences of that. Three rules
  are its own. *Every question has a number*, and an answer is taken only for what is
  waited for just now: what was asked before a box was closed, before the wall was
  left, or behind one of Windows' own boxes while something else happened, is nobody's
  when it comes. *Nothing is kept past its box*: the passphrase and the password are
  never held by it (the boxes hand them in with each press and empty their lines when
  they close), and the file's text and what was read of it go when the box closes
  (`reset`), as does whatever was under way. *Nothing a file says goes unseen*: a sign
  that draws nothing, turns a line around or begins another line, a blank that is not
  the blank, and the letters and marks the server itself lists as not drawn, are
  written out as their numbers, in a name as in what a button will type, where each
  line's end is marked as well, since it is Enter. The dot that stands between the
  things said of a device is the program's own: one that a name brings along, or a sign
  that looks like it, is written out, so that a name cannot pose as the facts that
  follow it. A console link is said
  by the place a browser would go to with it, in the letters that cannot be taken for
  others; and one that is no plain address (a backslash or a tab in it, which a browser
  reads otherwise than this program; a name and password; another way than the web's;
  a place written as numbers in any other form than four of them with dots, on which
  programs do not agree) is said as that, whole, and never as a place. (What a browser
  reads as numbers is a last part of digits only, or of sixteens: a name that begins
  with a digit is a name.) A device's own place, which is where a password goes that is
  typed for it afterwards, and the place alerts go to are said in the same letters;
  and one that is more than a place (a name before it, or something that begins another
  part of an address) is said as that.
  A file that came is kept only if it is of the kind that was asked for: one without a
  passphrase is not written under the name of one with. The parts of a file that was
  read tell the screen of themselves only when they are others (`partsChanged`), so
  that what somebody unticked stays unticked when the server refuses and is asked
  again. A taking is said to have taken nothing only where the server says no in its
  own words, which it does before it changes anything, or where the file was never
  sent to it at all (`nothingWasSent`, below); whatever else goes wrong (no answer,
  one that was cut short, one from something that stands before the server) leaves
  open whether it did, and the box says so.
  Where a file is kept and which is opened is asked through a `FileDoor` (`ui/file_door`, which this PC's boxes share), two functions:
  Windows' own boxes over the window that asked (`ui/win/file_dialogs`), a folder of
  their own in the tests, names given at the start in the rig. The program goes on
  while such a box is open, so the view counts as busy for that time and looks, when
  the box comes back, whether it still waits for it. Where to keep a file is asked
  before the server is asked for it: a file with every password of the wall in it is
  not made for nobody. It is written whole or not at all (`QSaveFile`). A box of
  Windows' that does not come up is said, and is not taken for a no.
  The session stands between the views and the client (`ServerSession::serve` hands a
  box's questions to `ServerSource`): it sends nothing for whoever is not at the wall,
  nor anything while the program only shows (each is refused at once, in the
  program's own code, `desktop:not_sent`, with nothing asked of the server, and a box
  is emptied where that begins: "A wall screen", above), passes each answer back under
  the number it was asked under, and tells every view when the wall is left. Each of the three calls posts
  something that is not for whatever answers at the address to have: an export the
  password of whoever is signed in, once more; a reading and a taking the file, which
  may hold the passwords of machines, and its passphrase. So
  each is held by the session for one question, and goes only after `/healthz` there
  was answered by a QKVM this program speaks to (`ifAQkvmStillAnswersThere`), with
  whoever asked still at that wall. Otherwise nothing is sent, and the view is told
  so in a code of the program's own (`FilesView::nothingWasSent`,
  `desktop:not_sent`), with the sentence for why. Where the session stands is not
  changed by that answer: that the server is away, or another, the looks at the
  machines say. The boxes (`ExportBox.qml`, `ImportBox.qml`) close when the wall is
  left, with the account's; while the server
  works they are not closed by Esc or a click beside them, and what was typed into
  them is not written over; and the import box cannot be closed at all while a file is
  taken in (`Dialog.qml`: `closesBeside`, `canClose`), which cannot be given up and
  whose outcome is to be told. Nor is the window itself closed while a file of its is
  under way, or while one of Windows' own boxes stands over it, whatever became of the
  question meanwhile (`WallWindows::eventFilter`, `FilesView::holdsItsWindow`): Qt ends
  a program in which a window goes while one of its screens is still answering a press.
  What a box holds scrolls where the window is too low for it, with a mark at its edge
  that says so (`ScrollMark.qml`); its buttons stay, and the line that has the keyboard
  is scrolled to.
  Known, and left: giving up what was asked does not stop the server, which finishes
  its file; whoever is signed out while a file is taken in is not told what came of it,
  and sees it on the wall; the file that is chosen is read where the screens are drawn,
  so that a file on a slow share holds them up for as long; a file of two thousand
  machines has the box make a line for each at once; a server that sends its answer a
  letter at a time can keep a question open, and its window with it, for as long as it
  likes, and only "Quit" ends that; and once Windows has asked the program to end and
  was then told not to, a window is not held by a file for the rest of that run.
* **This PC's wall as a file** ("Export..." and "Import..." in the first line of the
  settings' section "Export and import", at this PC's wall: "The settings", below; the
  account's box there has them no longer) is the program's own work: it makes the file and reads one
  (`WallExchange`, section 4), and no server is asked anything. `OwnFilesView`
  (`ui/own_files_view`) is what the two boxes of a window are handed (`OwnExportBox.qml`,
  `OwnImportBox.qml`): one for each window, with a `WallExchange` of its own
  (`OwnWall::serve`), so that what one window laid out of a file is not another's to
  take. What it shares with the server's boxes (`FilesView`, above) is `ui/file_door`:
  Windows' own boxes for where a file is kept and which is opened, a chosen file read up
  to a limit, and a file written whole or not at all.
  Export: chosen devices, actions and shared settings, then a passphrase where wanted,
  in a line that hides what is typed, with its eye;
  the file is made off the interface thread; then every line of what is not in it stands
  in the box (unsupported server alerts, local-only state, plain files' omitted text
  actions, and each KVM whose password Windows did not hand out), and only then, after
  a moment, does "Save…" open Windows' Save box. The name
  says what the file is: a sealed file ends in `.qkvm`, a plain one in `.json`; a wrong
  ending that stands last is replaced, and a dot or blank at the end is dropped as
  Windows drops them (`asWindowsTakesIt`, `namedAsWhatItIs`). Another file that is there
  is never written over without Windows' box having asked about it. The window's corner
  names the file that was written.
  Import: a file says of itself whether it is sealed, and only then is a passphrase asked
  for. "Read the file" lays it out (`WallExchange::layOut`), and nothing on the wall
  changes. Above the list stands, once: how many of the file's devices are in the list,
  how many are ticked, how many want a look and how many cannot be taken, in words
  (`LaidOutList::saidOfItsCounts`); that nothing has changed yet; that each KVM that is
  taken is asked what it is before a password goes to it; and, where the file has no
  password for any device that can be taken, that, once. In the list: the notes of what
  the reader left out, folded into one row ("N notes about this file") that a click
  opens; then every device as one short row: its tick, its name and address whole
  (never cut short), and one line of what matters (`LaidOutList::inShort`: known by its
  certificate, with its SHA-256 shortened in the middle; by Windows' word; plain HTTP;
  why it cannot be taken). The rest is behind "Details": the whole sentences, and the
  whole SHA-256 in lines of eight pairs. Ticked to begin with is only what can be taken
  and is known by its certificate or by Windows' word, at an address elsewhere
  (`tickedToBeginWith`): a device over plain HTTP, one whose name is in its `xn--` form,
  one at this PC's own address and a link-local one are not, and say why on their row.
  A device whose address cannot be shown is called "Device N of the file". "Take N KVMs"
  takes the ticked ones and no other. Where all are taken the box closes and the corner
  says how many; where one is not, the box stays with what came of each, and only
  "Close".
  The boxes hold still: three readings running had found a live "Take" coming to stand
  where another control had stood a moment before. Each box keeps one size and place
  for as long as it is open (the window's room, at most 700 by 760 for Import and 520 by
  480 for Export), its buttons stand at its foot in fixed places, and only the list
  scrolls; the button that takes is there for as long as a list is shown ("Take 0 KVMs"
  takes nothing); opening or closing a row's "Details" keeps that row's first line where
  it was. Nothing takes a press until it has stood for a moment (one second,
  `takeRestsMs`, which runs while the box is open in the active window): the list and
  what takes after the list comes, after a row is opened or closed, after the notes are
  opened, and after the box moves or changes size with its window (`boxPlace`); what
  takes after a first tick; the list after a key moved it (`rowsMoved`); "Save…" after
  its lines come on show. That is held in C++ for every way of pressing (`settled`,
  `mayTake`, `maySave`), not only in QML. After a taking that took all, the window
  under the box takes no press for that moment either (`heldAfterTaking`). Tests compare
  where every pressable control stands around every change the box knows, at 640 by 400
  and 1280 by 800 (`noButtonOfEitherBoxComesToStandWhereAnotherStood`,
  `noTickNoteTakingOrEndOfItsTimeComesToStandWhereAnotherStood`,
  `aWindowThatChangesSizeHoldsWhatTakesAndWhatSavesForTheirMoment`).
  What is kept, and for how long: what was laid out, ten minutes (`keptForMsAsItIs`; the
  box then says to choose the file again, and keeps its size); a file that was chosen
  and not read, the same ten minutes, with what was typed for it. A passphrase leaves its
  line once its file is made or read, or is not the chosen one any more. The core is
  told to `forget()` when the box closes, when the window leaves this PC's wall, and
  when the window goes.
  Tried in `ui/tests/test_own_files_view.cpp` (seventy-eight, as Qt counts them) and
  `test_own_files_screens.cpp` (forty-two), in both looks and at both sizes, with
  pictures of the tests' own windows. Section 11 has what is open.
* **A wall that is only shown** (display mode) is a state of the wall's view, not of the
  window: `WallView::displaying`, with `rotating` and the timer that gives each group its
  turn. A group whose turn came is shown by the same filter a chosen group is; the view
  says while it sets it (`isTurningNow`), and whoever remembers the chosen group
  (`WallChoices`) keeps its own word for it, so that nothing written down later takes
  the group of the moment for the chosen one. The screen tells the view when a box lies
  over the wall (`covered`). Where the keyboard goes when the bar disappears is said in
  `Main.qml`: to the wall, since Qt would hand it to the next thing in line, a button of
  the small bar that nobody sees. Besides a person, the windows' wall sets it: on in
  every window where the program comes to only show, and in a window that comes
  meanwhile ("A wall screen", above); off where somebody comes to a wall that nobody
  was at; and as it is where another source's wall is come to beside one that is up.
  A wall whose order is being made is not put into it: it keeps its bar, where the way
  out of that is.
* **Menus.** `controls/Menu.qml` is what drops from a button of the bar: rows given as
  data, headings between them, a tick at what is chosen. It does nothing itself but say
  which row was chosen; the bar hands that to the view model. One row is marked at a time,
  by the pointer or by the arrow keys, which start at the chosen row, pass over headings
  and go round the ends. The order and the group are the wall's filter's
  (`WallFilter`, a sort and filter model over the wall's machines), so the cards, their
  count and what is asked of the server follow from one place.
* **What comes from outside is letters.** Every `Text` says `textFormat: Text.PlainText`.
  Left to itself, Qt takes a text that is written like a page for one: it draws the
  markup, and fetches a picture the text names. A machine's name, a device's word for
  itself or a refusal could then make the program ask an address of somebody else's
  choosing. A gate in `tools/check.ps1` holds every line of text to it, and a test draws
  every screen with such a text in every place and sees that nothing is fetched.
* **The screens ask nothing of the network.** Behind that, a second wall: the engine that
  draws the screens is given a network access manager that asks nobody
  (`keepOffTheNetwork`, `ui/application.cpp`). A picture, a font or a page named by an
  address is not fetched, whoever names it and through whichever element. What the
  program asks of a QKVM server it asks through `core::ServerClient`, which is not the
  engine's.
* **Words in one place.** `Words.qml` holds every sentence, through `qsTr`, and for
  everything the page also says, the page's words.
* **How it looks** is `Look` (dark, light, as Windows is set), which tells Qt, and Qt tells
  Windows' title bar; `Theme.qml` reads what the application was told and holds the
  page's colours for both. Qt's platform without a screen, which the tests draw on, takes
  no telling: there the light colours are never drawn, and a picture of them comes from
  the real program.
* **Where the window was** is `WindowKeeper`'s: the place the window has while it is of
  its own size, and whether it is maximized or over the whole screen instead. Which of the
  two it is, is taken down the moment it changes (a window that is maximized and at once
  made to fill the screen was maximized); where it is, a moment after it has come to rest,
  and when it is closed. At the start the place is used only where enough of the window's
  top edge would be on a monitor to take hold of it by (`canBePutBack`), and what the
  window was under full screen is told to the window itself (`wasMaximized`, which
  `Main.qml` goes back to). Qt's platform without a screen does not give a window its old
  place back after it was maximized; Windows does, which was looked at with the rig.
* **The program's windows** are `WallWindows`'. Every production window owns a
  `SessionView`, a foreground `ServerSession` and its own `MergedWall`, views, graphics
  device, engine and keepers. `ServerSession::makeWindow` shares the local device
  service, credentials, settings, app lock and quick-access model. Source selection
  and foreground requests belong to that window; a source switch cannot empty the
  other windows. Preview snapshots and foreground permission/status requests remain
  independent per window, bounded by the eight-window limit. `WallPace` keeps the
  chosen picture interval shared.
  `SharedLive`, owned by the coordinator, pools server live connections by a
  memory-only SHA-256 of canonical server address and session token, plus the machine
  ID. Tokens and pool identities never enter settings, logs or QML. `LivePicture`
  receives a per-window `Live` handle through `MergedWall::shareLive`; local sources
  continue using `KvmdLive`'s existing sharing. A group aggregates size/FPS demand,
  fans out compressed frames without caching them, replays video metadata and asks
  for a key frame for a joining H.264 viewer. Each window keeps its own decoder and
  renderer. Last-viewer departure retires the connection; a queued join still hears
  an opening failure after handover. The initiating source may end after opening
  without ending the other viewers; its destruction while opening fails the group
  so surviving windows can retry through their own source.
  A group routes input only from its controller handle. It accounts for outstanding
  on/off requests before reassigning control. Missing release replies expire after
  five seconds by closing the ambiguous connection; unexpected grants also close it.
  Neither reconnect nor another viewer joining grants control. Deferred destruction
  lets Live signal callbacks close viewers safely.
  `AlertsView::treeSource` exposes the current authenticated inventories and observed
  state already kept by background monitoring. `ServerSession::showSources` builds
  each window's combined source/group/machine rows. Source/account-scoped keys avoid
  collisions between identical machine IDs. `SourcesBox` searches and expands this
  tree in a bounded, scrollable dialog; machine activation reuses fresh-account and
  inventory checks from notification navigation. Restricted sessions expose no rows.
  The coordinator keeps local monitoring and status-only saved-server monitors. An
  active foreground session becomes the server's incident observer; other windows of
  that server keep their own wall feeds but do not duplicate incident observations.
  When its last window leaves, saved-session monitoring resumes. Account changes
  invalidate older-account windows and sign-out clears all windows of that address.
  An app-wide `UnderWay` registry prevents simultaneous commands to the same source
  and machine. `WallMachines::actionKey` maps the window's authoritative `Reached`
  identity to an address-scoped key; `MachineActions` captures that key when a command
  starts so cleanup cannot affect another source after a switch.
  `WindowSlot::source` is stored with each window. `WorkspacesView` stores bounded
  named collections of `WallWindow` records in Settings, with stable random IDs and
  case-insensitive unique names. `WallWindows` captures current geometry before save,
  queues restore past the QML handler, checks busy/lock state again, and makes extra
  windows before replacing an existing layout. Creation failure retains the prior
  layout. Restore clears transient console/control and filter state, reuses
  `WallChoices` and `WindowKeeper`, and falls back to the first available monitor when
  the saved title bar would be unreachable. Workspace records contain no credentials,
  display-link keys or enlarged-machine identifiers.
  Windows closed while another is open are forgotten and later slots move up.
  Closing the last window or quitting the application preserves the layout for restart.
  The tray status and page command follow the last active window. Notification and
  shortcut navigation prefer a window already showing the requested account scope,
  then the active window. The coordinator's separate unseen wall still tracks whether
  any window is visible for notification delivery.
* **The Windows bits** live in `ui/win/`, each a small class over the Windows API: the
  tray icon, the dark title bar, one running copy at a time, keeping the display awake,
  hearing that the PC locked or the network changed, Windows' own boxes for where a
  file is kept and which is opened, and the keyboard hook (section 3.8). Qt Widgets is
  not linked.
* **The icon in the notification area** is three things. `TrayIcon` (`ui/tray.h`) is what
  the program deals with: the mark with or without a dot, a line for the pointer, a menu
  as rows, and what somebody did with it. Behind it `ui/win/tray.cpp`, Windows' one call
  for such icons, from a window of its own that nobody sees: Windows tells that window
  what is done with the icon, and that its taskbar has started (then every icon has to
  be put there again, and one that could not be put there at sign-in is). Its menu is
  Windows' own, as every icon's there is. `TrayKeeper` is the rest, and all of it that
  has tests: what the icon says of the wall (`trayStateOf`), and the program's last
  window going out of sight instead of closing where that was chosen. It refuses that
  window's closing by taking the event before the window sees it; and since Qt closes
  every window when the program is told to end, and does not end while one refuses, it
  stops refusing the moment "Quit" is chosen. It is told of a window (`keep`) before
  that window's `WindowKeeper` is made, which is then asked first and still writes down
  where the window was. The mark is drawn by `markImage`, in
  whatever size Windows wants on this monitor; the window's own icon is the same
  picture. What Windows does with the icon was tried by hand, through the rig: that it
  holds it, what its menu says, a click, closing the window, and "Quit".
* **One running copy** (`ui/win/single_instance`). Which copy is the first is settled by
  something of Windows' that only one can make: a mutex, named in the session of whoever
  is signed in, so that two people at one PC each have their copy. The telling goes over
  a named pipe that only the same user may open, and all that can be said over it is one
  line, "show": nothing else does anything, and what is too long to be that line is hung
  up on. A pipe hands nothing over until the
  other end reads, so a later copy waits a moment for the first to take the line, and
  ends whether it did or not. (For the same reason the tests run a later copy in a thread
  of its own, as another program would be: in one thread with the first it would wait for
  itself.) The later copy tells Windows that the first may come to the front, which
  Windows otherwise lets only the program in front decide. `bringBack` is what both the
  icon and a second start do to the window.
* **Persistent alerts** use `core::Incidents`, `AlertsView` and `SettingsAlerts.qml`.
  `ServerSession::useAlerts` attaches once, after `useOwnWall` and before sources start.
  It scopes records to this PC or a SHA-256 of server address and stable account id.
  Only completed `WallFeed::statusObserved` samples count; model changes, picture
  failures and repainting do not. Two agreeing observations settle a transition;
  missing/unknown observations break the sequence. Startup/reconnect gets 120 seconds
  of grace for restored states, ending early on agreement. A server outage records
  `@source` only; cached machine states never become a cascade of false outages.
  One episode lasts until recovery or removal, with its latest bad state and an
  acknowledgement flag. Wall/device maintenance deadlines and the pre-pause state
  survive restart, so resume reports the net change after a fresh settled sample.
  The application connects aggregate changes to `TrayIcon::tellAbout` only when the wall
  is unseen and messages are wanted; `AlertsView` additionally gates them on app lock.
  The old `wall_news`/`WallTeller` helpers remain tested but are no longer wired in main.
  History lives beside the settings in `.incidents`, as bounded versioned JSON written
  atomically with `QSaveFile`: at most 10,000 watchers, all active incidents and 1,000
  closed incidents, at most 16 MiB. Changes batch for 250 ms and flush on shutdown;
  write failures stay visible and retry after 60 seconds. An invalid file is left
  untouched until an explicit repair renames it to a unique backup and writes the
  current run's observations. IDs, display names, state, timestamps and local choices
  are stored, with no credentials, typed text, address fields or raw error replies.
  Display names can contain hostnames when no name was given. This metadata is not
  encrypted; app lock restricts the interface, not Windows access to the file.
  Server history is filtered against its current authorized inventory, and all reads
  and edits are gated while locked or in display-only mode. The settings list uses the
  existing virtualized `Listed` component. Per-scope channels keep observations separate
  from the selected history source. `ServerSession` keeps the local feed alive and owns
  status-only `ServerSource` instances for other saved servers (32 maximum), without
  adding them to `MergedWall`. Selecting a server hands its monitoring to the foreground;
  leaving it starts its background source. Saved session names restore addresses from
  older builds without copying secret values into settings. Missing sessions and unknown
  certificates wait for user attention. Transport failures retry; revoked sessions stop.
  Display links never open as background sources, including an account changed into one.
  `TrayIcon` serializes at most 20 waiting notifications, keeping source/machine targets
  with each message. General notices, including updates, use the same queue with no
  machine target. Locking, showing a wall, changing the selected wall or disabling
  notifications discards waiting messages. A visible message can still request unlock.
  Native notifications use a new 16-bit icon id for each message, rejecting callbacks
  from previous messages (including retained Windows 10 banners). After 65,534 messages
  in one process, additional banners are suppressed; incident recording continues. A
  notification opens in a restored active window. The account is revalidated even for
  the current server, and older inventory requests are invalidated before opening a
  machine. Account changes, removal, sign-out and display mode cannot route an old
  notification into another machine or bypass app lock. No input control is taken.
  Physical notification, Explorer restart and extended lifecycle acceptance remain open.
* **Quick access** (`ui/quick_access`) resolves bounded favorites and recent machine
  references from `Settings` against `AlertsView`'s authenticated source inventories.
  Names and source labels are not persisted with references. A reference is `pc` or
  the server/account scope hash, plus the machine ID; a wall key is resolved through
  `MergedWall::reach`, never parsed to guess a destination. `ServerSession` connects
  each wall's favorite request and enlarged-machine change to the shared model, so
  all windows show the same stars. `TrayKeeper` builds Favorites and Recent machines
  submenus only while the app is unrestricted. Native menus retain the original rows
  until their selection is delivered; refreshing numeric row IDs cannot retarget a
  queued click. Changes close an open native menu, including app lock. Shortcut
  selections use `WallWindows::openIncident` and its fresh account/inventory gates.
  Limits are 32 favorites and ten recent machines; each submenu can clear its list.
  Failed saves keep the change for this run and report that it could not be saved.
* **The keyboard is the wall's from the moment somebody has signed in** (`Main.qml`). Qt
  leaves it where it was, and that is the line the password was typed into, which nobody
  sees any more: a letter typed at the wall would go there, and its mark would go on
  blinking. And what changes by itself on the wall is read only while it is seen: the
  bar's clock. A wall where nothing changes is not drawn at all, which a test counts.
* **A window nobody can see** says so to the wall (`WallView::unseen`, which `Main.qml`
  sets from the window's own state), and no picture is asked for while it lasts: a
  minimized wall of sixty-four machines costs the QKVM nothing but the look at how they
  are, which the icon needs.
* **The settings** (`SettingsBox.qml`, `SettingsView`) are one box in the shape of the
  page's (its `settingsDialog`), opened by the bar's button, where the bar has room for
  it, and by Ctrl+, wherever the window is, also before anybody is signed in. At its left
  the sections, under small group headings, each with its icon, the one shown marked
  (`SectionList.qml`); in a window under 784 px wide (`Theme.narrowWindow`) they lie in
  one row above the section, which scrolls sideways and has no headings. Up and Down, and
  Left and Right, go from one section to the next, round the ends, in both; Tab stops at
  the one shown. Which sections there are is the program's (`SettingsView::sectionsFor`):
  where the window shows this PC's wall, "This PC's wall" with KVMs and Export and import,
  then "This program" with General and About; at a QKVM server's wall, and where nobody is
  signed in yet, the program's two alone, under no heading (a heading stands only where
  there are two groups). What is a server's own stays on its page: at a server's wall,
  General says first that its machines, its accounts and its own settings are managed
  there, with "Open its page". The sections listed are those of what the window showed
  when the box was opened, and where the window comes to show another wall meanwhile, the
  box closes. It opens where it was left (`opensAt`): at the section shown last, where it
  is listed for what the window shows, and otherwise at the first that is, which does not
  count as where it was left. That is written into the settings when the box closes and
  when the program ends with it open (`~SettingsView`), and not at each section on the
  way: the settings are one file, megabytes of it for a large wall (section 4).
  The box holds still. It has one size and one place while it is open, whatever it shows
  and whatever changes in it: 1080 by 780, or the window's room less 24 where that is
  smaller, in the middle of the window, and never as large as what is in it. Only a
  section's content scrolls, under its first line. The section shown is made when it is
  chosen, and goes when another is (a `Loader`). Every section has one shape, made of
  controls of their own in `ui/qml/controls`, and none is laid out by hand. `Section`:
  the first line, with the
  section's name and a short sentence at the left and what one does in it as a whole at
  the right ("Add KVMs"; "Export..." and "Import..."), which stays in sight while the rest
  scrolls; what the section has to say of what came to pass meanwhile stands in it, in the
  place of the short sentence and in no more lines than that has, so that the line is as
  high with it as without; and what a section is asking, or what went wrong in that, after
  the last of it (no section of today's asks for what it shows: the shape has it for those
  that will, as the servers' will). `Intro`, what explains a section, before its first
  block. `Block`, a small heading and a frame of lines, or a heading and a text without a
  frame, as a licence is. `Line`, what it is at the left, with a sentence under it where one
  is needed, and at the right a tick, a choice, a button or just what is so; in a narrow
  window the right-hand side stands under the left; a line with a tick takes a click on
  its whole left side, as the page's label does. `Listed` and `ListRow`, a list in a
  frame: the columns' titles, a row for each, what one does to a row at its right end, or
  one line that says there is nothing in it yet. `Footnote`, what is said after the last
  block. `Tip`, what a row says whole, a moment after the pointer rests on it, where its
  place cuts some of it. What one presses in them is the program's own `Button`, `Check`
  and `Choice`. The About box and the section About show one `AboutBlocks`, not two
  copies. A later section is a QML file made of these shapes, its key in
  `SettingsView::sectionsFor` and in `Remembered::namesASettingsSection` (so that it can
  be remembered), its name, icon and component in `SettingsBox.qml`, and its words in
  `Words.qml`: so the servers' section and the saved actions' are to come, and no section
  is there before it has something in it.
  Rows hold still too. Every row of a list is of one height from the start: each column's
  cell has a number of lines that is fixed for the list (`Listed`'s `lines`), whatever is in
  them, and what is longer is cut at their end and is whole in the row's tooltip and in its
  name for a screen reader. So nothing that comes late, and no name however long, moves a
  row. And so the list knows how high it is without making its rows: it makes those that
  are in sight in what scrolls it, and a thousand pixels' worth above and below, each at
  its own place by its number, and makes the others as they come into sight; the row the
  keyboard is in stays made however far it is scrolled away. With as many KVMs as a wall
  holds (2000), the list opened in some 80 ms in release, with 21 rows made and 7 MB more
  (it was 3 s and 686 MB while every row was made; measured with the secrets in memory,
  `aWallOfAsManyKvmsAsItHoldsOpensSoonAndScrolls`).
  The KVMs (`SettingsKvms.qml`, over `OwnKvmsView`, one for the program, which every
  window's box is handed): every KVM of this PC's wall as a row. What it is called (its
  name, or where it is where it has none, as its card calls it), and under it, in the
  colour of a problem, "Same device as" the others of the wall that are known by its
  certificate (`LocalWall::twinOf`, each as a sentence of the wall calls it) and "No
  password saved"; where it is, and under it what kind of KVM; how it is spoken to (known by
  its certificate, by Windows' word, or plain HTTP, with its password in the clear); and at
  its end "Edit" and "Remove", the second in red, as the page's is. What a row says of its
  KVM is the wall's as it is, and the rest of its words are in `Words.qml`. Nothing of it is
  secret, and the list reads no password: "No password saved" is the wall's mark, or what
  the KVM's driver found at its last look, whichever says none (section 4, `LocalWall`),
  read of the wall without asking a KVM anything whenever a row is made or follows its
  KVM, and when the wall says that its KVMs came to something. The list holds still while
  any box shows it in a window that is in sight (`begin` and `end`: a window behind the
  icon or made small shows it to nobody). A KVM that is changed elsewhere is changed in its
  row, one that is added comes last, and one that is taken off keeps its row, which says
  "Taken off this wall", with its buttons there and taking no click. The wall's order, and
  what was taken off, are taken anew only when the list is shown anew, and where the KVMs
  are then what the rows are, KVM for KVM in their order, no row is made anew and the
  keyboard stays where it was. (Rows that moved up into the place of one that was taken
  off would bring another KVM's "Edit" under a pointer on its way to the one that stood
  there.) "Edit" opens the box that changes a KVM, as it is; "Remove" opens the same box at
  its question alone (`EditKvmView::openToRemove`, `removesOnly`), with the keyboard at its
  no and its yes in red. The question follows the wall as the box does ("Changing a KVM of
  this PC's wall", above): a new name stands in its title, and where the KVM is taken off
  elsewhere the question stays where it is, says so in the place of its words, and only
  its way out takes a click. After a removal the keyboard goes to the next row's
  button that takes a press, and where none does, to the section's entry in the list; so
  it does where its row itself went. While the program only shows, "Edit" and "Remove"
  open nothing (`EditKvmView::setMayChange`); "Add KVMs", "Export..." and "Import..." are
  not held yet (section 11).
  The boxes the sections open (adding a KVM, changing one, taking one off, this PC's wall
  written to a file, a file's KVMs taken onto it) stand over the settings, as the page
  stacks them, and close back to the section, with the keyboard where it was. Export and
  import says what a file of this PC's wall holds, of either kind, and what taking one in
  does, and its first line opens the two boxes ("This PC's wall as a file", above). General
  has the look, the two things of a window that is out of sight (going on behind the icon;
  Windows' messages of the wall), and starting with Windows, with what went wrong where
  Windows did not take a change in a line of its own, last, where nothing stands under it.
  The theme's choice is as wide as its widest word, whichever is chosen.
  Tried in `ui/tests/test_settings_screens.cpp`, `test_settings_view.cpp` and
  `test_own_kvms_view.cpp`, each rule seen to fail with it taken out (but one: that the box
  opens where it was left also after a restart), with pictures of every section in both
  looks, wide and narrow, of the tests' own windows
  (`picturesOfEverySectionInBothLooksWideAndNarrow`).
  `theBoxHoldsStillAroundEveryChangeItKnows` compares where every control a person can
  press stands around another section, a list that grows, shrinks or has a row changed,
  the wall's order, a line that comes and goes, a tick and another look, at 1280 by 800
  and at 640 by 400; `theRowsAreOfOneHeightWhateverComesLateAndHoweverLongANameIs` holds
  the rows. That every section is made of the shared shapes and of nothing else is held by
  a walk of the tree (`everySectionIsMadeOfTheSharedShapesAndOfNothingElse`). Each shape
  says which it is (`shape`), and each of its parts that a rule names is named. A section's
  content is shapes; a block's frame holds lines alone and a list's rows alone, each the
  whole width of it; what a line or a row does holds the program's own controls alone
  (known by their QML type, and walked inside for anything that takes a press and is not
  theirs), and they stand nowhere else; nothing else takes a press but the part of a line
  that clicks its tick; every text is one of its shape's own; and what is hidden is walked
  too. `theWalkFindsWhatIsLaidOutByHand` plants parts made by hand (a text in a frame, a
  line with a place of its own, a mouse area working as a button, a hidden button, a part
  that calls itself a shared control, and more) and sees each found. Known: after a saving
  that leaves the box that changes a KVM open, the line that says it was saved goes when the
  next thing is asked, and the box gets shorter by it. Section 11 has what is not shown at
  all.
* **Starting with Windows** is `core::AutoStart`: one interface, and behind it Windows'
  own list for one user (the "Run" key under the user's part of the registry), written by
  the Windows API and by nothing of Qt's. Whether the program is on it is never kept in
  the program's settings: it is asked of Windows when the settings box opens, since
  Windows' own settings switch an entry off as well (noted beside the list, a first byte
  that is odd), and a copy of the program that was moved leaves an entry that is not this
  program's. `SettingsView` hands that to the box, and says what came of a change. The
  tests of the Windows side write under a key of their own and take it out again; the
  tests of the screens and the rigs are given a pretended list (`ui/pretend`), so that
  nothing but the program itself, asked by its user, ever touches the real one.
* **A list's rows take their click for themselves** (`controls/Choice.qml`: a `MouseArea`
  in each row). A row that only listens for a tap (`TapHandler`) leaves the same click to
  whatever lies under the list, through the box the list belongs to: a card of the wall
  took the keyboard, Esc no longer closed the box, and Enter would have enlarged a
  machine behind it. What is put into a popup takes its clicks the way a button does.
* **The window frame is Windows' own**, dark. No custom frame: it would need private Qt
  interfaces or one more library.
* **The settings box.** A row of a list kept made for the keyboard far out
  of sight stays while the keyboard is out of the window or in a box over the section, and is let
  go only where the keyboard comes to rest on another item of its section (`Listed.qml`). The
  box that changes a KVM and the list's question before a removal stay where they are when their
  KVM is taken off elsewhere (above), so the section's own "taken off meanwhile" note went; what
  a KVM's password is kept as (its lines and its UTF-8) is written over before it is freed, on
  every road (`KeptAs`, `local_wall.cpp`); a line's eye is drawn as a layer, held to the edge of
  what scrolls also in software (`Field.qml`, as `Check.qml`'s mark). Tests in
  `ui/tests/test_settings_screens.cpp` and `ui/tests/test_own_wall.cpp`.

## 6. The patterns, by name

| Pattern | Where | The rule |
|---|---|---|
| Ports and adapters | engine, input, core against ui | The hard parts have narrow interfaces and no knowledge of their host |
| Pipeline with a slot of one | A stream's pictures | Newest wins; nothing queues; what is dropped is counted |
| Strategy | Decoders; the ways of finding | One interface, chosen at run time by a policy |
| Policy object | Who gets the graphics card; what is asked of the server | One place decides, from measurements, and can be tested with numbers alone |
| State machine, written out | Connection, tile, control, device | An enum and one transition function; no state in scattered flags |
| Model, view, view model | The whole interface | QML has no logic and no data |
| Ownership by scope | Everywhere | `unique_ptr`, `ComPtr`, wrappers that free FFmpeg's objects; no `new`, no `delete`, no owning plain pointer |
| Object pool | Packets, pictures, textures | No heap in the steady state |
| Passing by constructor | Everywhere | No singletons, no globals with state; a clock and a server are handed in, so tests hand in others |
| One choke point | URLs (`ServerClient`), addresses to ask (`mayAsk`), secrets, releasing keys | A rule that matters is enforced in exactly one function |
| Rebuild as an ordinary path | The graphics device, a connection | What can be lost can be made again, and tests lose it |
| Bounded everything | Queues, sizes from outside, retries, logs | Every limit is a named constant with the reason beside it |

## 7. Errors, logs, and what is measured

* **Errors are values.** A small `Result<T>` in `base/`. Our own code throws nothing;
  every thread and every callback from Windows, Qt or FFmpeg catches at its top, so that
  nothing ever crosses a boundary flying.
* **Three kinds of failure.** A stream's (that stream starts again), the device's
  (rebuild), the program's (a small dump and a last log line in the logs folder, and
  Windows is asked beforehand to start the program again after a crash).
* **The log** is our own small one: levels, a bounded queue to the writer thread, files of
  2 MB, five kept. It never holds a secret or anything that was typed. Qt's own messages
  go into it.
* **Once a minute, one line of numbers**: handles, committed memory, graphics memory in
  use, streams by kind of decoding, pictures dropped. The long run of M1 is judged from
  these, and every installed copy keeps them, so that a wall that slowly grows can be seen
  to.
* **The diagnostics line** on a tile shows what `Stream::stats` knows.

## 8. Tests

One framework: Qt Test, which comes with Qt.

| Part | How it is tested |
|---|---|
| engine | Decoding in software against known pictures. Drawing on Windows' software adapter (WARP) into a texture, compared with expected pictures within a tolerance, so it runs anywhere. The device lost on purpose. At the end of every test the debug layer must report nothing alive. Everything again with the address sanitizer |
| input | The scan-code table against Microsoft's. AltGr. For random sequences of keys cut off at a random point: nothing stays down |
| bmc | Against frames made by the tests' own writer (the web's `tests/aspeed_frames.py`, ported); pictures the same as the web's reader makes, by their SHA-256; the console's messages byte for byte as the web's writes them. Seeded tests of damaged frames, headers at their edges, and console streams cut anywhere and damaged, in the ordinary suite, which counts every allocation and times every frame against its bound; libFuzzer's targets in a build of their own (`QKVM_FUZZ`), run by hand |
| core | Against a stand-in of a hundred lines over a TCP socket, in the test itself: every refusal the server can give, slow answers, answers too large, broken JSON, a changed certificate. Marked tests run against `python -m tests.demo` |
| finding | `mayAsk`, exhaustively. Sweeps only against stand-ins on the loopback address |
| ui | View models without a screen. `qmllint` with warnings as errors. Every screen loaded once with pretend view models |
| the whole | `bench` for load and for the long run; a script judges the minute lines for growth |

Hardware decoding cannot be tested on a machine without a graphics card: those tests are
marked, and run where there is one.

The tests are run on the fast cores of a PC that has two kinds (`Use-FastCores` in
`tools/common.ps1`, which `tools/check.ps1` uses around the tests of each build; the
builds have all cores). Windows runs what is not in front of whoever sits at the PC on
the slow cores when it sees fit. The made-up KVMs of the tests speak TLS on the test's
own thread, and under the address sanitizer, on a slow core, three lines are not let
in within the second and a half the tests give one: programs that are right fail with
"unreachable". Seen on 2026-10-13, when three builds of different hours all failed so
and all passed held to the fast cores; two whole checks before that had the same three
programs failing under the sanitizer, which was put down to other work on the PC then.
What stays open is the cause behind it: those tests have less time to spare than a
slow machine needs (section 11).

Every program CTest runs fails quietly and at once (`qkvm_test_quiet`, linked into each
by `qkvm_add_test`; `qkvm_test_quiet_static` into `test_installer_storage`, which carries
the C library in it; and the web runner `qkvm-web-compat-tests`): a failed check of the
standard library, an assert or an `_ASSERTE`, the debug heap finding a block written past
its end, a bad argument to the C library, an abort or a crash ends it by itself, with its
words on stderr and an exit code that is not 0, never going on past the failure and
never with a window that waits for a click. One did, on the owner's screen (2026-10-29);
a check run left to itself would have waited there until CTest's limit.
`test_base_quiet_failures` tries each way on a child of each kind, and the configuration
stops at a test of which it is not said what program it runs (`qkvm_test_runs`, which
`qkvm_add_test` says itself), or whose program links neither library. A shipped program
cannot link them: it defines the mark they define (`qkvmTestProgram`). Not covered, and why: the review
guards (PowerShell's); Windows' own programs a test starts (`cmd`, `cmdkey`,
`powershell`); `qkvm-crash-test-child`, whose crashes are what its test reads and which
sets its own error mode; and `qkvm-setup.exe` as `test_ui_startup_check` starts it, the
shipped program as it ships, which inherits the test's error mode (no box for a crash)
but in a debug build would show its own assert's window until the test gives up on it
after 10 s.

## 9. Build and tools

* CMake with presets (`debug`, `release` with symbols kept, `asan`), Ninja, Microsoft's
  compiler. Warnings are errors (`/W4 /WX /permissive- /utf-8`); the hardening switches
  of the compiler and linker are on. One warning is off in one place: "unreachable code"
  in the C++ that Qt writes from QML files, where it fires inside Qt's own headers.
* Every program is linked whole, in every build (`/INCREMENTAL:NO`). Left to patch a
  program in place, Microsoft's linker put a test together from a library that had just
  changed and left the old code in it; the test passed on the code of the build before.
* Qt 6.12, outside the repository: `tools/fetch_qt.ps1` fetches it, pinned as FFmpeg is,
  and writes its place into `local.cmake`, which git ignores and the build reads. (Not
  into `CMakeUserPresets.json`: presets there could not have the names of the ones here.)
  Modules: Core, Gui, Qml, Quick, Quick Templates, Network, Svg; Test and WebSockets
  are used only by tests and their stand-ins. Before using any other, look up its licence in Qt's list: only LGPL ones.
* FFmpeg: `tools/fetch_ffmpeg.ps1` fetches a pinned LGPL build as DLLs and checks its
  SHA-256. Only `avcodec` and `avutil` are linked: packets arrive already cut, colours
  are converted in the shader. Before M4 the project builds its own, with nothing in it
  but the H.264, JPEG and PNG decoders and D3D11VA, so that what ships is known exactly.
  (PNG brings zlib into that build. The other way would be to let Qt read PNGs, which
  has zlib and libpng in it already, and to give the engine a way in for pictures that
  are decoded elsewhere: more seams for one library less. Not taken; the owner's to
  overrule.)
* `clang-format` and `clang-tidy`, each with its file in the folder; `qmlformat` and
  `qmllint` as Qt sets them, a warning of `qmllint` counting as a failure.
* `tools/check_signs.py` refuses, in any file of the project, a sign that nobody can see
  (one that draws nothing, turns a line around, or is a blank of another kind), and in
  the sources a letter that is not one of the twenty-six. A test about such a sign
  makes it from its number where it runs. (One was written into a test as itself once,
  by an editor that resolved its escape, and nobody reading the file could have
  known.) `tools/test_check_signs.py` tries the check itself first, on files made for
  it: a check that refuses nothing would always pass.
* `tools/check.ps1` runs all of it and keeps the whole output. There is no build server
  yet: a release is built on a PC from the public tree, by the same scripts.
* A test program is run by `ctest` with `QT_FORCE_STDERR_LOGGING=1`: without a console
  attached, Qt Test on Windows writes to the debugger alone, and a failure would say
  nothing.
* The off-screen tests of the screens need Windows' fonts shown to them
  (`QT_QPA_FONTDIR`), or every letter is an empty box.

## 10. In the code

* One naming style everywhere, Qt's, because QML needs it: types `LikeThis`, functions
  and variables `likeThis`, members `m_likeThis`, files `like_this.cpp`. Namespaces
  `qkvm::engine`, `qkvm::input`, `qkvm::core`, `qkvm::ui`.
* Comments say why, in whole sentences, as the server's code does (read a file of
  [the web app's `qkvm/`](https://github.com/qkvm/qkvm/tree/main/qkvm) for the voice). Public headers say what every function
  promises and from which thread it may be called.
* Anything that comes from outside (a size, a count, a length, a string) is checked
  against a named limit before it is used. A picture is at most 4096 by 2304; a message
  at most 8 MB.
* Intervals are measured on the steady clock; the wall clock is for log lines only.
* Nothing waits on the interface thread: no file, no network, no lock held while decoding.
* No code is copied from another project. Where a program was read
  to learn how it does something, the comment may say "as X does".

## 11. Not proven yet

| What | Shown in | If it fails |
|---|---|---|
| The engine drawing under QML on one shared device (3.6). Shown with one window: nine live pictures under the wall's cards, with QML's words and buttons over them. Not shown yet: two windows, and the loss of the device, which Qt cannot get over by itself when the device is not its own | M1 step 6 | Let Qt make its own device per window, bind each stream to its window, and rebuild on Qt's signals that the scene graph went and came. If that fails too: the engine draws the whole wall into one texture, shown by one QML item, at the cost of one copy per frame |
| The controlled picture is not later under Qt than in the bare window (3.7) | M1 step 6 | The controlled view becomes a window of the engine's own, with its own swap chain and its bar drawn by the engine |
| A graphics driver never hangs. It does: one maker's driver stops for good inside Present when it is given a thirty-second stream to decode at once (thirty-one work), and gives no error first | found in M1 | The engine decodes at most sixteen streams on the card unless told otherwise. That is a margin, not a cure: a watch on the drawing thread, which ends the program and has Windows start it again when a draw has not come back for some seconds, is still to be built (M2, "it stays up") |
| Nothing of a stream is left behind when the device is lost and made anew. After a loss the program holds about 3 MB of graphics memory and 4 MB of its own more for every stream that had been on the card: the size of one software picture, so probably what a stream needed while it was decoded in software in between, and kept | found in M1 | Find it and let go of it when the stream is back on the card; lose the device a second time and see that nothing is added |
| Decoder output can be sampled directly on every maker's driver | M1 step 1 | Per adapter: copy the slice to a texture that can be sampled; failing that, software |
| A wall of 64 small H.264 tiles in software is affordable | M1 step 1 | Small tiles stay previews; only large ones are live |
| The keyboard hook keeps its time limit while the program is busy. Built for it (3.8): the hook has a thread of its own, which goes before the program's other work; its procedure takes no memory, waits for no lock and logs nothing; a key that comes after Windows has stopped waiting for the procedure, or within 50 ms of that, is known by its own stamp, is Windows' and ends control; and no longer wait than Windows' own 300 ms is believed, whatever the registry says. A hook that Windows has dropped is found by Windows' second account of the keyboard, at the first key after it; where this PC gives no second account it is not found, and the bar says so while keyboard and mouse are held. All of that on plain values (`aKeyThatComesAfterWindowsStoppedWaitingForItIsWindowsAndEndsTheSwallowing`, `noLongerWaitThanWindowsOwnIsBelievedWhateverTheRegistrySays`, `aKeyThatRawInputToldOfAndTheHookDidNotHearEndsTheSwallowing` in `ui/tests/test_keyboard_hook.cpp`; `whereThisPcGivesNoSecondAccountOfItsKeyboardTheBarSaysSo` in `ui/tests/test_control.cpp`). Not measured and not seen: no test runs the hook, and of its one real run nothing is known but that keys arrive (the row on the real hook), so what its procedure takes on a busy PC, and whether Windows drops it there, is not known | M3b, by a person at a keyboard, on a busy PC | What its procedure takes is measured, and what is found is mended in `hook::Procedure`, with a test on plain values that is seen to fail first |
| The driver for kvmd does on a real KVM what it does on the made-up ones. Seen on one real KVM of one maker (2026-10-12): the question without a password, its own certificate, the sign-in, its resting video woken, its state, pictures, the line for keyboard and mouse let in and held; and, with the rig's `--live` and `--new`, which ask for pictures and send nothing to its computer, the KVM had live for ten seconds without an error, 2560 by 1440, by its pictures asked one after the other (the next row has the numbers). Not seen: a live picture of a screen that moves, or one in a window; anything done to it (one key and one small move of the pointer were said to it and not seen to arrive: a row below; no button of the menu, no text, no restart), a KVM of the other maker (a stock PiKVM), one with several computers behind it, one with a power board | M3a and M3b, on KVMs the owner names, and each thing done only where the owner allows it | What a real KVM does otherwise is written into `kvmd_answers`, `kvmd_input` and the made-up KVMs, as the server's driver had it written into it. So far: no size of the picture while its video rests (what a picture says stands in); no stream of JPEGs, and at its stream's place a gateway's 502 while its video rests and a 200 of another kind while it is awake (the next two rows); keys and the mouse said in bytes |
| A KVM that serves no stream of JPEGs is had live as well as it can be. Today by its pictures asked one after the other (`KvmdLive`: the next when the last has come, at most twenty a second, each asked for as a new one: `KvmdDevice::askNewSnapshot`), tried against made-up KVMs. One of those that gives a picture in three hundredths of a second gave more than the five different pictures a second that a card's askings come to (`whoeverHasItLiveGetsMoreThanFivePicturesASecondAndNeverAsksForTwoAtOnce` in `core/tests/test_kvmd_live.cpp`). And measured on one real KVM of one maker, which its owner named, on 2026-10-12, with the rig (`qkvm-kvmd-rig --new` and `--live`: pictures only, nothing was sent to its computer). Asked one after the other as fast as they come, it gave sixty pictures as captured in 1.2 seconds: about fifty a second over the local network, about 160 kB each, the slowest in 38 ms. Fifty-eight of the sixty were byte for byte the one before, its screen being still, and the live view hands on only pictures that changed (and asks for no more than twenty a second). Had live, it gave pictures for ten seconds without an error, 2560 by 1440. Not seen: a live picture of a screen that moves, and a live picture in a window. Mended since that look: at its stream's place that KVM answers, while its video is awake, 200 with something whose kind is not a stream's. That was told as `bad_response`, which says nothing of whether a KVM has a stream, so its stream was asked for again at a wait that grew to half a minute, for as long as it was shown, beside its pictures. A yes with something of another kind is `no_stream_here` now, remembered as that is: the stream is asked for once, and then the pictures one after the other (`whatIsNoStreamOfPicturesIsNotRead` in `core/tests/test_kvmd_stream.cpp`, whose rows looked for `bad_response` before; the row "it answers with a yes and something of another kind" of `aKvmWithoutAStreamIsAskedForItsPicturesOneAfterTheOther` in `core/tests/test_kvmd_live.cpp`). That is tried against made-up KVMs. What a reviewer found here on 2026-10-12 is mended: a picture that came and that nobody was handed has an age now, and whoever asks for a new one is handed it only while it is new (`whoeverAsksForANewPictureIsHandedOneNobodyHadOnlyWhileItHasJustCome` in `core/tests/test_kvmd_device.cpp`, `aPictureThatCameLongAgoIsNotShownAsNewNorTakenForAVideoThatIsAwake` in `core/tests/test_kvmd_live.cpp`). H.264 over WebRTC, which is what such a KVM offers, is not built: it needs a library, and that is the owner's decision | M3b, in a window on a real KVM, with a screen that moves, and there the yes of another kind read as it is against the made-up ones; WebRTC as a step of its own | It stays one picture after the other |
| A gateway's 502, 503 or 504 at a KVM's stream's place (anything but kvmd's own shape) is read rightly. The page alone does not say which of two it is: a KVM that serves no stream; or a video that only rests, as a stock PiKVM is taken to answer while nobody watches. (One maker's real KVM was taken at first to answer 502 there also while its video is awake. Looked at again on 2026-10-12, it answered 502 with a gateway's page while its video rested, which is read right, as a video that rests; and while its video was awake, 200 with something whose kind is not a stream's, which is the row above.) So it is read by whether the video is awake (`saidByAGateway`): "serves no stream" (`no_stream_here`) only where the driver has had a picture of the KVM within the last moments (`sawItsVideoAwake`, `Pace::freshMs`), and otherwise a video that rests, which is woken and asked again. A refusal whose words come slowly, packed or broken off is a refusal all the same, and the KVM is not counted as away for it. Tried against made-up KVMs, and on the real KVM as far as the one reading above. Not tried: any stock PiKVM, so whether one answers so while its video only rests, and then has its stream at the second asking, is not seen. What a reviewer found here on 2026-10-12 is mended, both parts of it. A picture that nobody was handed has an age (the row above), so one that came long ago is no longer taken for the picture that shows the video awake. And "serves no stream" is remembered for a minute, not for as long as the driver lives, and is forgotten sooner when the driver signs in anew (`KvmdLive::Pace::noStreamForMs`, `KvmdDevice::passGiven`): a KVM whose gateway answered for its stream for a moment has its stream again after that | `core/tests/test_kvmd_stream.cpp` (`aGatewaysPageSaysThatTheVideoRestsUnlessAPictureHasJustCome`, `aPictureThatCameAWhileAgoSaysNothingOfAVideoThatMayRestSince`, `aRefusalWhoseWordsNeverComeWholeIsARefusalAllTheSame`, `aRefusalThatComesPackedIsARefusalAllTheSame`, `aRefusalThatBreaksOffIsReadAsFarAsItCame`, `whatComesWithARefusalHasItsTimeAlsoWhereTheRefusalItselfCameLate`) and `core/tests/test_kvmd_live.cpp` (`aStreamThatOnlyRestsIsAskedForOnceMoreWhenAPictureHasCome`, `aKvmWhoseVideoIsKnownToBeAwakeAndWhoseGatewayAnswersWithAPageIsAskedForItsStreamOnce`, `thatAKvmServesNoStreamIsRememberedForATimeAndNotForGood`, `whatIsRememberedOfAKvmsStreamIsForgottenWhenItsDriverSignsInAnew`, `aKvmWhoseGatewayAnswersForItsStreamForAMomentHasItsStreamAgainAfterThat`, `anAnswerThatSaysTheKvmItselfIsBrokenIsNotRememberedAsNoStream`). On a stock PiKVM the owner names: M3b | What a stock PiKVM answers is written into `kvmd_stream` and the made-up KVMs |
| A key that was typed into a line that stalls is not played later. Guarded on this end (the watch, the quarter of a second, the reset of a cut line); not on the far end: what the KVM's own system has taken and its program has not read is played when it reads again, up to the three seconds the watch allows. And that a KVM lets go of every key when its line falls is taken from kvmd's source, not seen | M3b, on a real KVM, with the owner's leave | Keys typed while the KVM's program is held (a debugger's stop on a test KVM), and a look at what its computer got |
| A key that is down when its line is closed while the way stalls goes up on the machine. Closing says the "up", then goodbye, and offers both to Windows; the KVM is then to answer with its own goodbye and end the line, and that is waited for (`DeviceLine::close`, `gone`). Where something had waited too long already, neither is said and the line is reset at once. Where something was down then, or was said up since the KVM was last heard from, whoever held keyboard and mouse is told that something may still be down (`KvmdConsoleLine::close` says so, `Live::leftDown` passes it on), as where the line ended by itself in that state. A closed line that is kept (`KvmdConsoleLine::letGo`, to which `KvmdLive` hands every line it is done with) and that the KVM has not ended within two seconds is reset then, with what had not left, and the word may never arrive. Whether it arrived, the KVM's own goodbye says and nothing else (`gone(bool arrived)`). Where the closing said that everything was let go, and the line then went without that goodbye (it fell, the KVM ended it without a word, or its time was up and it was reset), whoever held keyboard and mouse is told then that something may still be down, late as that is and if that one is still there (`letGo` with whom to tell, `KvmdLive::giveBack`, `Live::leftDown`). The key then stays down until the KVM lets go of it by itself: that kvmd lets go of everything when a line falls is taken from kvmd's source, not seen (the row above). A line that is thrown away in the same breath, as when the program ends, is neither waited for nor reset: what had come from the KVM and was not read is taken off it first (`~DeviceLine`), so that Windows does not reset it, and Windows goes on delivering what it took for as long as the KVM takes it, the "up" and whatever was said before it. Tried against made-up KVMs, on this PC: the tests named at "A line to a KVM" in section 4, with `closingSaysWhetherEverythingThatWasDownCouldBeLetGo`, `aLineThatIsLetGoIsKeptUntilItHasGoneAndSaysNothingToWhoeverHeldIt` and `whoeverHeldALineThatWasLetGoIsToldLateWhereWhatWasSaidUpMayNotHaveArrived` beside them, and in `core/tests/test_kvmd_live.cpp` `whereTheLineEndsByItselfWithAKeyDownTheHolderIsToldThatItMayStillBeDown`, `whereTheLineTakesNoMoreWhileItIsLetGoOfTheHolderIsToldThatSomethingMayStillBeDown`, `aLiveThatEndsByItselfUnderWhoeverHoldsKeyboardAndMouseLetsGoFirstAndTellsThem` and `whereALineThatWasLetGoIsResetWhenItsTimeIsUpWhoeverHeldItIsToldLate`. What a reviewer found here on 2026-10-12 is mended, and the last of each list is its test, with `aLineThatHasGoneSaysWhetherTheKvmAnsweredItsGoodbye` of section 4: a line that the KVM never ends was reset two seconds after its holder was told that everything was let go, and nobody was told then. Whoever sits at the program is told: `MachineControl` hears `leftDown`, and the bar says that something may still be down on the machine (section 3.8; `thatSomethingMayStillBeDownOnTheMachineIsSaidInTheBar` in `ui/tests/test_control.cpp`, with a live that a test makes say it). Not there: a `LiveLink` never says it. Not tried: whether a real KVM, kvmd on Linux, loses what it had not read when its line is reset, as the made-up KVM on this PC does. What the reviewer of that mending found is not mended, but for that hearing of `leftDown`, and has a row of its own below ("What a reviewer found on 2026-10-12 in the mending ...") | M3b, on a real KVM, with the owner's leave | Where a KVM does not let go: the "up" is said once more on a line of its own before the KVM is left, and its card says that a key may be down |
| Keys and the mouse arrive at a real KVM's computer. On 2026-10-12 one key and one small move of the pointer were said to one real KVM, by its owner's leave, in bytes, as its own page says them (`qkvm-kvmd-rig --touch`). That they arrived was not seen: the picture showed no pointer. Since then the owner has started the program, taken keyboard and mouse of a real KVM in its window, and reported that keys arrive. Nothing else is known from that run: not which keys, nothing of the mouse, and not whether anything stayed down. So that keys arrive is the owner's word of one run, and that control works on a real KVM beyond that is not shown | M3b, on a real KVM, with the owner's leave: something typed where its computer's screen shows it | What its own page sends is read again, and written into `kvmd_input` and the made-up KVMs |
| Keyboard and mouse are taken in a window. Built (`MachineControl`, section 4; the keys, the hook and the rest, section 3.8), and tested against stand-ins only; what is known of the real hook is the next row. Tested: `MachineControl` with keys that a test presses by hand, in some tests through the hook's own procedure on plain values (`KeysAsWindowsTellsOfThem`), and a live that notes what it is told (`ui/tests/test_control.cpp`); what a key is, and the leave-key's rule (`input/tests/test_keys.cpp`); through the session to a made-up KVM and over the stand-in for a server's line (`keyboardAndMouseOfAKvmOfThisPcsOwnWallGoToTheKvmItself`, `keyboardAndMouseOfAServersMachineGoOverTheLineItsWindowHasItLiveBy` in `ui/tests/test_server_session.cpp`); each window's own control and keys, and the one memory of what is held (`keyboardAndMouseOfAMachineAreEachWindowsOwn`, `whatIsHeldIsKnownToEveryWindowOfTheProgram` in `ui/tests/test_wall_windows.cpp`); the bar and the pointer over the picture (`ui/tests/test_console_screens.cpp`); and of the hook its plain parts, on plain values, ten of its tests three ways: on a PC that gives no second account of its keyboard, and on one that does, in either order of the two (`ui/tests/test_keyboard_hook.cpp`). Four reviewers read it on 2026-10-12, one after the other: the first the piece as it was first built, each of the others the mending that the reading before it had led to. What they found is mended in this code, each with tests. Three more read it after them, the same day, and two more after those, each the round before it: what the fifth to the ninth found is the five rows after the next, each with what of it is mended, what went out again, and what is open. Of the tests named here, "hook:" stands before those in `ui/tests/test_keyboard_hook.cpp` and "control:" before those in `ui/tests/test_control.cpp`; any other file is named. From the first reading. A modifier that went down where it could not be swallowed, or in the moment of the taking, is known, and a key that went to Windows is kept back: the procedure keeps what is down and tells of every press with whose it is (hook: `whileKeysAreSwallowedEveryPressIsToldWithWhoseItIs`, `theKeysThatAreDownAreKnownWhateverBecameOfThem`; control: `aKeyThatWentDownWhileNothingCouldBeSwallowedIsNotTheMachines`, `noKeyGoesDownUnknownInTheMomentTheyAreTaken`, `aKeyThatWasKeptFromWindowsIsStillKnownWhenTheyAreTakenAgain`). Windows' own screens end control by the keys that bring them and by Windows' word for the session, beside the desktop's event (hook: `whatWindowsTakesTheKeyboardWithEndsTheSwallowingFirst`, `whenWindowsShowsAnotherDesktopThatIsSaidAsWhatItIs`). A yes is taken only while it is waited for, and the bar says that it asks (control: `aYesIsTakenOnlyWhileItIsWaitedFor`; `whileKeyboardAndMouseAreAskedForTheBarSaysSoAndOffersToStop` in `test_console_screens.cpp`). In a narrow window the machine's name stands whole, and the bar names the leave-key and "Give back" beside it, always (`whileKeyboardAndMouseAreTakenTheMachinesNameStandsWhole`, `theBarNamesTheLeaveKeyAndTheButtonBesideItAlways`, both in `test_console_screens.cpp`). Ctrl+Alt+Del leaves what fingers hold as it was (control: `ctrlAltDelLeavesWhatFingersHoldAsItWas`). Why a line ended is said where they were only asked for (control: `whyTheLineEndedIsSaidWhereTheyWereOnlyAskedFor`; `whyTheLineEndedIsSaidWhereKeyboardAndMouseWereOnlyAskedFor` in `test_server_session.cpp`). And control: `aButtonIsNotNotedAsDownAfterTheLiveEndedUnderIt`; hook: `underAltGrTheLeftCtrlWindowsMakesUpIsNotSaidToBeDown`. From the second. The watch of the first mending typed a key into Windows four times a second, which keeps a PC from locking: nothing in the program types into Windows any more (hook: `theProgramAsksItselfNothingByTyping`, `nothingInTheProgramTypesIntoWindows`, `theLookForTypingIntoWindowsFindsWhatItLooksFor`), Windows' second account of the keyboard is what finds a hook that hears no more (hook: `theTwoAccountsOfTheKeyboardAreHeldAgainstEachOther`, `aKeyThatRawInputToldOfAndTheHookDidNotHearEndsTheSwallowing`), and control has an idle limit of its own (control: `keyboardAndMouseThatAreLeftAreGivenBack`). A key that is held and silent is not let go on the machine (control: `aKeyThatIsHeldAndSaidNoMoreStaysDownOnTheMachine`). What was kept from Windows is known at the next taking, whatever the ending (control: `whatWasKeptFromWindowsIsKnownAtTheNextTakingWhateverTheEnding`, `aKeyThatWasKeptFromWindowsAndToldToNobodyIsKnownAtTheNextTaking`; hook: `whileNoHookHearsRawInputTellsOfWhatWasKeptFromWindowsGoingUp`). A hook that stays for its kept keys goes after a minute by its own thread's doing, and tells of nothing else meanwhile (hook: `onceTheSwallowingEndedTheHookStaysForAWhileAndNoLonger`, `whileNothingIsSwallowedOnlyTheGoingUpOfAKeyThisHookKeptIsTold`). From the third. A key that went down under the leave-key is never typed bare, by its repeat either (`aKeyThatWentDownUnderTheLeaveKeyIsNeverSentAlsoNotByItsRepeat` in `input/tests/test_keys.cpp`; control: `aKeyPressedUnderTheLeaveKeyIsNeverTypedBareAlsoNotByItsRepeat`, `aModifierThatWentDownUnderTheLeaveKeyHoldsBackWhatIsPressedUnderIt`). A key that is held from before is kept back until it is heard to go up or seen to go down anew, and not for five seconds (control: `aKeyThatMayHaveBeenLetGoUnheardIsKeptBackUntilItIsSeenToGoDownAnew`). The gate (hook: `whileTheTwoAccountsAreNotAtOneNothingNewIsHandedOn`, `whatWaitedGoesOutInItsOrderWhenTheTwoAccountsAreAtOneAgain`, `aKeyThatWindowsOtherAccountNeverTellsOfIsHandedOnAfterTheMoment`, `aKeyThatWasDownWhenTheComparingBeganIsNotComparedUntilItHasGoneUp`). A hook thread that hangs, and an interface thread that hangs (hook: `aHookThreadThatNoLongerLooksIsFound`, `whileTheInterfaceThreadIsSilentNothingIsNotedAndTheHookGoesAfterSomeSeconds`). What is held is one memory for the program (control: `whatIsHeldIsOneMemoryForAllTheWindows`, `whatIsHeldIsForgottenOnlyOnceNoWindowControlsAMachine`). And control: `whereThisPcGivesNoSecondAccountOfItsKeyboardTheBarSaysSo`, `theWindowsKeyIsSaidToTheMachineOnlyWhenItIsNoWinL`, `oneAskingAtATime`; hook: `howLongWindowsWaitsForAHookIsReadAndAMarginKeptUnderIt`; and `theButtonThatAsksDoesNothingWhileAnEarlierAskingIsStillUnderWay` in `test_console_screens.cpp`. From the fourth. An "up" whose "down" went out does not wait in the gate, and a click does not pass a key that was pressed before it (hook: `aKeyThatGoesUpDoesNotWaitWhereItsGoingDownWasHandedOn`, `whatRawInputWillNotTellOfTakesNoTelling`, and `whichKeysAreOnTheirWayIsKnownAndInWhichOrderTheyWereHeard`, as the fifth mending turned and renamed it; control: `aClickDoesNotPassAKeyThatWasPressedBeforeItAndIsStillToBeTold`, `whatTheMouseDidBehindAKeyIsNotSentLateAndNotWithoutEnd`). A key is the key it went down as (hook: `aKeyIsTheKeyItWentDownAsWhateverWindowsCallsItByThen`, `whatWindowsSawGoDownItSeesGoUpUnderWhicheverName`). No longer wait than Windows' own is believed (hook: `noLongerWaitThanWindowsOwnIsBelievedWhateverTheRegistrySays`, `whatTheInterfaceThreadTellsAndInWhichOrder`). Where the two accounts differ, the sentence says which told of a key that the other did not (hook: `whereTheTwoAccountsDifferItIsKnownWhichToldOfAKeyTheOtherDidNot`; control: `whenNothingIsHeardAnyMoreItIsSaidWhy`). That another program types keys which reach no machine is said (hook: `thatAProgramTypedIsSaidOnceAndNothingOfTheKey`; control: `thatAnotherProgramTypesKeysWhichReachNoMachineIsSaid`). An asking has a time (control: `anAskingThatNothingAnswersIsOverAndTheyCanBeAskedForAgain`, `whatTheMachineSaysOfAnAskingThatIsOverIsNobodys`). And control: `theBarNamesEveryModifierThatIsKeptBackWhyever`, `whyNothingIsSwallowedAnyMoreIsAskedForBeforeItIsTakenForTheLeaveKey`; hook: `aKeyThatFindsNoRoomToWaitInIsWindowsAndEndsTheSwallowing`, `theTwoAccountsInEitherOrderWithAFastPairOfKeys`, `theTwoAccountsInEitherOrderWithAKeyThatIsSaidAgainThirtyTimesASecond`; and in both, `aKeyThatIsOnlySaidAgainIsNoKeyGoingDownForTheLeaveKey`. Built since, by the owner: the bar says where something may still be down on the machine (control: `thatSomethingMayStillBeDownOnTheMachineIsSaidInTheBar`). Built since: clipboard typing, screenshot save/copy and relative mouse capture (their checks are tracked). Not built: the leave-key as a setting. Physical input acceptance remains open | The next six rows | One of these tests failing says what is open again |
| The real hook does on Windows what its plain parts do in the tests. Not shown by a test, any of it: `WindowsKeyboard` (`ui/win/keyboard_hook.cpp`) is built and linked, the program and `qkvm-session-rig` are given it, and no test installs or runs it. It has run once for real: the owner started the program, took keyboard and mouse of a real KVM, and reported that keys arrive. Nothing else is known from that run, so nothing of the list below counts as tried. Untested, because it lives only there: the hook's thread and its loop; the hook going in and out; Raw Input asked for and read; with it Windows' word for a device that is taken away, asked for and read, and the device asked what kind it is; the wiping of what was read of a key there; the session's and the desktop's events (the lock screen, a question about administrator rights); the reading of the registry; the keys' stamps; what `handOn` emits; `tellNow`; and the asking of the look-watch. Everything that is said of Windows in the comments is from memory, and each says there how sure it is. Only a person at a keyboard can try it, in this order. The ways back, every time: right Ctrl alone; else the mouse, "Give back" or a click on another window; else Ctrl+Alt+Del, which Windows always takes; and a program that hangs frees the keyboard within a second. (1) Take them, type, right Ctrl: it gives them back. (2) The program stopped in a debugger: the keyboard is back within a second. (3) Ctrl+Alt+Del, Win+L, a question about administrator rights: each ends control with a sentence that says what it was (the first two with the one for the keys that Windows itself acts on, the question with the one for a screen of Windows' own), and nothing is stuck afterwards. (4) Enter held, and the keyboard's plug pulled, with a second keyboard at hand: control ends with the sentence for a keyboard that went, and Enter is up on the machine. The same with Shift: taken again within a minute, the bar names Shift as held from before, and nothing goes to the machine until it is pressed anew on the keyboard that is left. (5) Only a mouse unplugged: nothing ends. A headset, a dock, a receiver that is mouse and keyboard in one: note which of them end control. (6) A hub switched off and on: by the code the first keyboard that goes ends control, and those that come back end nothing. (7) A wireless keyboard switched off with a key held; a desk switch that takes the keyboard to another computer: expect nothing said, and the key down on the machine until "Give back". (8) Two keyboards: a modifier held on the one and tapped on the other, then a key on the first. It arrives bare: that is the rule. (9) A key held until it repeats, let go, and tapped again at once: control goes on. Where it ends there, with the sentence for a key that Windows did not report, Raw Input does not tell of every saying of a held key on that PC. (10) Every key once: none is to end control. Print Screen, Pause, Num Lock under Ctrl, AltGr, the number block under Shift, the keys for sound. (11) Ctrl+C fast; Shift and then a click; Shift+Home: what arrives. (12) The on-screen keyboard, or a remote-control program: the bar says that such keys reach no machine. (13) Over a remote desktop: which lines the bar shows. Not on that list, and not tried either: "Control", "Stop asking" and "Control" again quickly, on a KVM of this PC's own and through a server; Sticky, Filter and Mouse Keys, and a key remapper, with which sentence comes and whether it is true; Ctrl+Num Lock with Ctrl let go first and with Ctrl let go last, Alt+Print Screen, the right Shift; the keys that switch the writing on Japanese and Korean keyboards; a second keyboard that goes on saying a held key while right Ctrl is pressed on the first; `LowLevelHooksTimeout` at 1000 and at 100; the wheel's direction, sleep and resume, a keyboard without a right Ctrl | M3b: a person at a keyboard, with a made-up KVM first, then on a real KVM the owner names | What is found is mended in `hook::Procedure` or in `MachineControl`, each with a test on plain values that is seen to fail first; the row on the hook's time limit names its first risk |
| What a fifth reviewer found on 2026-10-12 in keyboard and mouse as they were after those four mendings. Nothing high. It is mended in this code, each with tests ("hook:" and "control:" as two rows above), but for the second half of (1), which went out again, and for what the end of this row names. (1) Medium: the net `nothingIsDown` was never said in the cases it was added for. An "up" that came at another place of the hook's table than its "down", or that the hook was never called for, left the table's count at one or more, and the key down on the machine. Mended key by key, where it can be known for sure, and not by the net: Num Lock's two places in the table are one key. The second half of that mending is not in this code any more: where Raw Input tells of the keyboard, it kept an account of its own of what is down, by which a key was let go that it had as up. In the end nothing came to that but a telling which had been forgiven, and it went out with the forgiving in the ninth round (four rows below). An "up" that comes as no key of the keyboard's lets nothing go and ends control within the moment, and what was told to go up is let go as it ends (section 3.8, "Physical keys"). The net stays beneath, and the three comments on it were corrected (hook: `numLockIsOneKeyAtEitherOfItsTwoPlaces`, `aKeyThatAFingerHoldsIsNotLetGoOnTheOtherAccountsWord`, and `anUpNamedAsNoKeyOfTheKeyboardsIsNotThatKeysAndEndsTheControl`, which is the test of that letting go turned round; control, through the hook's own procedure and by no signal emitted by hand: `aKeyThatNoFingerHoldsAnyMoreDoesNotStayDownOnTheMachine`, which took the place of a test that said the key source's words by hand, `anUpNamedAsNoKeyOfTheKeyboardsEndsTheControlAndLeavesNoKeyDownOnTheMachine`, turned round likewise, `thatNoKeyIsDownAnyMoreSaysNothingOfAKeyThatIsHeldFromBefore`). (2) Low to medium: a click passed a modifier's "up" that was heard before it. Every key that is heard has a number while it is on its way, and a click and the wheel go behind everything of the keyboard that was heard before them, an "up" as well as a "down" (hook: `whichKeysAreOnTheirWayIsKnownAndInWhichOrderTheyWereHeard`; control: `aClickGoesBehindEverythingOfTheKeyboardThatWasHeardBeforeIt`, `theWheelGoesBehindEverythingOfTheKeyboardThatWasHeardBeforeIt`, `aClickGoesBehindNothingThatWasHeardAfterIt`, `aClickGoesBehindTheKeysAsTheHooksOwnProcedureNumbersThem`). (3) Low: words of the machine that were nobody's could take the next asking's answer, stayed owed without end where none came, and turned every later "no" into "did not answer"; and the machine's "no" to a giving back was read as a quick new asking's. What is owed is a row now, in the order it was asked and by kind, each word for a time (`owedForMs`), and a "no" with its sentence is shown as what the machine said (control: `theMachinesWordForAGivingBackIsNotTheNextAskingsAnswer`, `aYesThatCameLateOwesAWordForItsOwnGivingBack`, `aNoWithItsSentenceIsShownAsWhatTheMachineSaid`, `aMachineThatWasLateWithItsNoIsSaidAsWhatItSaid`, `whatTheMachineOwesRunsOutByItself`; turned round in part: `whatTheMachineSaysOfAnAskingThatIsOverIsNobodys`, `oneAskingAtATime`). What this mending left wrong in it was (1) of the next row. The sixth mending turned these tests round once more, each where it had asked for keyboard and mouse, or had "Control" offered, while a word was owed, and put another test in the place of the one that asked anew the moment the machine's word for a giving back had come: `afterTheOneWordOfAMachineThatCallsAnAskingOffWithoutAWordTheOtherRunsOutItsTime`, as it is named since a KVM of this PC's own says two words there. (4) Low, a suspicion: an "up" that Windows names 0xFF let a held key go. What Windows cannot name is no key again, going down or up (hook: `whatWindowsCannotNameIsNoKeyGoingDownOrUp`, and the part of `aKeyIsTheKeyItWentDownAsWhateverWindowsCallsItByThen` that held the old way taken out; control: `theShiftThatAKeyboardMakesUpDoesNotLetGoTheShiftThatIsHeld`). Whether Windows hands a hook the Shift that a keyboard makes up, and how, only a person at a keyboard can show: (11) of the list in the row above. (5) Low: a held key that was said again four times before Raw Input's first telling of it ended control. A key that is only said again waits for no telling and takes no room (hook: `aKeyThatIsSaidTenTimesBeforeTheOtherAccountsFirstWordWaitsForNoneOfThem`, in either order; turned round in part: `aKeyThatGoesUpDoesNotWaitWhereItsGoingDownWasHandedOn`, `theTwoAccountsInEitherOrderWithAKeyThatIsSaidAgainThirtyTimesASecond`). And where Raw Input's tellings stop for good, the sentence says what is known and blames nobody: that Windows did not report the key from the keyboard, and may no longer be reporting the keyboard to the program, or that something made or changed the key (control: the row of `whenNothingIsHeardAnyMoreItIsSaidWhy` for it, turned round). (6) Comments and tests that said more than is so: the comment at `input/src/keys.cpp` on the key that Windows renames, and the one at `keyboard_hook.h` on a modifier that is said again, were corrected; the keys of which the two accounts tell differently pass the gate in the form `fromRawInput` makes of them (hook: `theKeysOfWhichTheTwoAccountsTellDifferentlyPassTheGateAsRawInputTellsOfThem`); and the exhaustive test of the leave-key feeds each side its own "said again", and says what it does not show (hook: `theLeaveKeysMachineIsRightForEveryShortSequence`). Left as it was, and said at the code as what it is: on a PC that gives no second account, an "up" that the hook is never called for, or that comes at another place than its "down" and is not Num Lock's, leaves the key down on the machine until it is pressed and let go again, or control ends; a smaller number in the registry that Windows read as somebody signed in and that was taken out since; and which came first, a key or a click, goes by when each is seen | `ui/tests/test_keyboard_hook.cpp` and `ui/tests/test_control.cpp`. What is left, and what only a keyboard shows: M3b | One of these tests failing says which of them is open again |
| What a sixth reviewer found on 2026-10-12 in that fifth mending. Nothing high. It is mended in this code, each with tests ("hook:" and "control:" as three rows above), but for (2), whose mending went out again, and for what the end of this row names; what a seventh reviewer then found in this mending is the next row. (1) Medium: after an asking that nothing answered, "Control" was offered while words were still owed. A yes that came then was given back as one that "may be its answer to an earlier asking", and that giving back owed two words again: whoever followed the sentence and clicked at once was refused the same way, every time. No asking is made while the machine owes a word: the button that asks does nothing until what is owed has come or has run out, which is ten seconds from an asking at the most and five from a giving back (five more where a yes still comes and is given back). So the first word to an asking is its answer, and the rule and the sentence for a yes that may be an earlier asking's are gone, with the comment at `control.h` on what such a yes costs. Behind why an asking was given up from here, the bar says when Control can be clicked again, and tells nobody to click what does nothing (control: `noAskingIsMadeWhileTheMachineOwesAWord`, `afterASilentAskingAYesAndAWaitTheNextYesTakesThem`, which is the reviewer's own sequence, `theBarAsksForNoClickThatCannotWork`, `theButtonIsBusyForNoLongerThanIsSaid`; turned round: `anAskingThatNothingAnswersIsOverAndTheyCanBeAskedForAgain`, `oneAskingAtATime`, which says what it shows now, and the tests of askings that the row above names; and `theButtonThatAsksDoesNothingWhileAnEarlierAskingIsStillUnderWay` in `test_console_screens.cpp`, with the sentence as the bar says it now). (2) Low to medium, and older than the fifth mending: two keyboards. A key that was held on the one was let go on the machine by the same key's "up" on the other, and the next key from the first arrived bare: a hook is not told which keyboard a key came from. The sixth mending kept Raw Input's account of what is down for each keyboard, so that a key was up only when no keyboard held it. That is not in this code: for three readings running every finding of middle weight was in it, and the eighth round took it out again, by the owner's decision (two rows below; section 3.8, "Two keyboards"). What this reviewer found is the rule again, and its known cost: a key is let go on the machine when any keyboard lets go of it. The tests of the telling apart were turned round to hold that rule, or taken out where they held nothing any more (hook: `aKeyIsLetGoWhenAnyKeyboardLetsGoOfIt`, `aKeyGoesUpByItsUpAsItIsHeard`, `whatIsPressedAfterAModifierWasLetGoGoesOutBehindItsUp`, `aKeyHeldFromBeforeIsLetGoByAnyKeyboardsUp`, `theLeaveKeyGivesTheKeyboardBackByItsUpAsItIsHeardWhateverRawInputHasStillToSay`; control: `aKeyHeldOnOneKeyboardIsLetGoOnTheMachineByTheSameKeyOnAnother`, `aKeyThatIsKeptBackIsKeptBackNoLongerWhenAnyKeyboardLetsGoOfIt`). Only a person at two keyboards can see it: (8) of the list two rows above. (3) Low, older: a press that was handed on in the very call that closed the gate could be told to the interface thread, and reach the machine bare for a moment; the reason that followed found control over, and no sentence was shown. What is told to the interface thread is asked, key by key, whether the hook has been closed by then, and why is asked for once more there (hook: `aPressHandedOnInTheCallThatClosesTheGateIsNotTold`). And where `MachineControl` finds over a key that nothing is swallowed any more, it asks for the reason before it gives keyboard and mouse back (control: `whyKeysAreHeardNoMoreIsAskedForBeforeTheyAreGivenBackOverAKey`). What that left was (3) of the next row, which is mended. (4) Low, new in the fifth mending: on a PC that gives no second account, nothing emptied what the hook noted as heard. From the fourth press of a key on, a count went up with every press of that key, and the time of its last press was kept, for as long as the hook lived: against the rule that nothing typed is counted or kept. Where there is no second account to hold a hearing against, nothing is noted of one; where there is, a hearing is noted until that account has told of the key, and for two of the gate's moments at the most where it does not (hook: `whereThereIsNoSecondAccountNothingIsNotedOfAHearing`). (5) Low, tests. The two tests of the letting go by Raw Input's word reached it only by an event that their own comments called not known to happen. That letting go is not in this code any more (the row above, (1)), and the two were turned round to what such an "up" does now, which is to end control (hook: `anUpNamedAsNoKeyOfTheKeyboardsIsNotThatKeysAndEndsTheControl`; control: `anUpNamedAsNoKeyOfTheKeyboardsEndsTheControlAndLeavesNoKeyDownOnTheMachine`). The sequences that had no test have one: a telling of Raw Input's that is lost, of a key going down and of one going up (hook: `aTellingOfRawInputsThatIsLostIsFoundOrDoesNoHarm`); a typed key that Raw Input tells of with a device (hook: `aTypedKeyThatRawInputTellsOfWithADeviceEndsTheControl`); Pause as Break, and Print Screen under Alt, let go as the keys they went down as (hook: `pauseAndPrintScreenAreLetGoAsTheKeysTheyWentDownAs`); a key that is kept back, let go (control: `aKeyThatIsKeptBackIsKeptBackNoLongerWhenAnyKeyboardLetsGoOfIt`); and the numbers of keys on their way beginning anew while another thread takes them (hook: `theNumbersOfKeysOnTheirWayBeginAnewWhileAnotherThreadTakesThem`). And `aKeyThatNoFingerHoldsAnyMoreDoesNotStayDownOnTheMachine` says in itself what it asserts and is no good thing: Ctrl+Pause sent to the machine where Ctrl+Num Lock was pressed, if Windows drops the key's mark, which is (10) of the list two rows above. (6) Comments: what becomes of an "up" that comes as no key of the keyboard's is said at `keyboard_hook.h` (it is not compared, Raw Input's telling of the key going up waits to be heard, and control ends within the moment, where what was told to go up is let go). Left as it was, as decided then: `MachineControl::nothingIsDown` finds nothing to do with the real key source, and its loop has no test; and an "up" whose "down" Windows never handed the hook is kept from Windows while keys are swallowed. Unchanged above all of it: no test installs or runs the hook (two rows above) | `ui/tests/test_keyboard_hook.cpp` and `ui/tests/test_control.cpp`. What a seventh reviewer found in it: the next row | One of these tests failing says which of them is open again |
| What a seventh reviewer found on 2026-10-12 in that sixth mending. Nothing high. It is mended in this code, each with tests ("hook:" and "control:" as four rows above), or went out with the telling apart of keyboards, which the eighth round took out again (the next row); what is open in it is at the end of this row. (1) Medium, new in the sixth mending: a keyboard that held a key by Raw Input's account and never let go of it made that key stick on the machine. It was unplugged and Windows' word for that did not come, or its "up" reached neither account: its place in that account kept the key. The same key, pressed and let go on another keyboard, had two holders then: nothing was let go and nothing was told, the key stayed down on the machine until control ended, and the bar said nothing. And the second way of asking for Raw Input, which is tried where the first is refused, asked without the word for keyboards that come and go, and told nobody that it did. The seventh round mended that within the telling apart, with a line in the bar for a key that was held elsewhere and a hold that gave way after three seconds. None of that is in this code. No keyboard holds a key for itself any more: a tap on any keyboard lets the key go, a keyboard that goes ends control (two rows below), and where Windows' word for that cannot be had, the bar says so (control: `whereThisPcDoesNotSayThatAKeyboardWentTheBarSaysSo`). (2) Medium to low: control ended on one keyboard with nothing unusual done. With the hook's thread some 50 ms behind, a key that was held until it said itself again, let go, and tapped again ended control as "Keys could not be passed on in time", which was not what happened, and that press went to Windows. The same where one keyboard said a key again that another tapped. The same key going down once more while its "up" waits is a press like any other now, behind that "up" in the row, and a thread that is a quarter of a second behind is borne (hook: `aModifierGoingDownOnceMoreWhileItsUpWaitsTakesItsPlaceBehindIt`; control: `aModifierThatSaidItselfAgainCanBeLetGoAndTappedAgainWithTheHooksThreadBehind`). And a ninth keyboard ended control with that sentence too, which was as untrue there: it went out with the telling apart, and nothing ends by how many keyboards there are. (3) Low: why control ended was lost where it came behind a key in the same pass. The interface thread took a key while the hook was open; the hook's thread marked why and closed; the next look asked why, so the reason stood behind that key in what was told. The key was handed on, `MachineControl::takeKey` found nothing swallowed and asked for a reason that was taken already: control ended with nothing said, and the reason then found control over. What a pass has not said yet is kept now, and is said there and then to whoever asks for it from inside the telling of a key (`hook::Telling`; control: `whyControlEndedIsSaidWhereItComesBehindAKeyInTheSamePass`, through the real key source's own procedure and telling). (4) Low. Where Raw Input's telling of an "up" never came (it was lost, or the keyboard tells with no device), the key stayed down on the machine for the moment and one look, about half a second, every press waited behind it, and control ended. An "up" waits for no word of Raw Input's any more: the key is let go by the hook's own hearing, what is pressed meanwhile goes on, and nothing ends (hook: `aKeyGoesUpByItsUpAsItIsHeard`, `aTellingOfRawInputsThatIsLostIsFoundOrDoesNoHarm`). A PC that was taken not to tell, on which a device begins to tell later, ended control with "Windows saw a key on the keyboard that QKVM Desktop was not handed", which was untrue: the tellings waited for hearings that were never noted. Hearings are noted from the first telling on, and what cannot be known of that first moment ends control once, with a sentence of its own (two rows below, (2)). And a sound key's hearing was noted under the letter that shares its number (Volume up under B, Mute under D). A key without a USB number of the keyboard's page is not compared at all, and no key is read with its mark turned (hook: `aKeyForSoundIsNotTheLetterThatSharesItsNumber`, `theHearingOfAKeyForSoundDoesNotTakeTheTellingOfTheLetter`). (5) Low, tests. `aKeyHeldFromBeforeOnOneKeyboardIsNotLetGoByTheSameKeyOnAnother` held only because the first keyboard said the key again in the test. It is `aKeyHeldFromBeforeIsLetGoByAnyKeyboardsUp` now and says what is so, with what whoever sits there sees: the bar's line for the key that is kept back goes away at the tap on the other keyboard, and the next key goes bare (control: `aKeyHeldFromBeforeTheKeysWereHeardIsLetGoByAnotherKeyboardsTap`). The tests' stand-in machine said eight words before every taking, so that no older test was in the state the program is really in after a giving back. It says what a machine says: a word for every asking and every giving back, as a KVM of this PC's own does, or none (`Says::EveryWord`, `Says::NoWord` in `ui/tests/test_control.cpp`), and each test of askings says which of the two it is. And the bad half of (3) has its test. (6) Low, and ordinary use. With a KVM of this PC's own, "Control" was inert for the whole time a word is owed after everything but a plain giving back (an asking that was given up or stopped, a refusal, keyboard and mouse taken away), since each left a word owed that never came. Mended at its root by the driver: a KVM of this PC's own answers every asking and every giving back with a word of its own (section 4, `KvmdLive`; `keyboardAndMouseGivenBackBeforeTheirLineWasThereAskTheKvmForNone` and `keyboardAndMouseGoToTheKvmFromTheOneWhoHoldsThem` in `core/tests/test_kvmd_live.cpp`). And in this piece: a no with the code `given_up` is taken for the machine's answer to an asking that was called off from here, and its sentence is shown nowhere, so that "did not answer" stands where the asking's time had run out; with a machine that says every word, Control is offered again within a turn of the loop after every ending; and with one that says none, the bar says after half a second why Control is not offered, and only where it can be offered (control: `afterEveryEndingControlIsOfferedAgainWithinATurnOfTheLoop` and `whereTheMachineSaysNoWordTheBarSaysWhyControlIsNotOffered`, each for seven ways a holding or an asking ends, a refusal among them; `whyControlIsNotOfferedIsSaidOnlyWhereItCanBeOffered`; `afterTheOneWordOfAMachineThatCallsAnAskingOffWithoutAWordTheOtherRunsOutItsTime`). The comment at `control.h` says the two words of such a KVM. Open in it still: those tests have a stand-in machine that says its words as the driver does. With the driver itself beneath `MachineControl`, only a plain giving back is shown (`keyboardAndMouseOfAKvmOfThisPcsOwnWallGoToTheKvmItself` in `ui/tests/test_server_session.cpp`), and the state after a refusal by such a KVM is in no test with it. With a machine that says fewer words, as a server's line may, the button is busy until the time has run out, and how many words a server's line says is not known here. Unchanged above all of it: no test installs or runs the hook, and all that is known of its one real run is that keys arrive (three rows above) | `ui/tests/test_keyboard_hook.cpp` and `ui/tests/test_control.cpp`. What an eighth reviewer found in the seventh round: the next row. What is open: M3b | One of these tests failing says which of them is open again |
| What an eighth reviewer found in that seventh round. Nothing high. For the third reading running, every finding of middle weight was in one place: the telling apart of keyboards (Raw Input's account kept for each keyboard; an "up" that let nothing go while another keyboard held the key; an "up" of unknown keyboard that waited; holds that gave way after three seconds; the line in the bar for a key that was held elsewhere; the notice of keyboards coming and going, as it was used then; the ninth keyboard). It was built to save one bare key in a rare case, the same modifier held on one keyboard and tapped on another, and each attempt at it had made a worse rare case: a modifier that stayed down on the machine with nothing said, where a keyboard dropped out without notice, or a hold that gave way under a finger and cost every key until the finger was lifted, again with nothing said. By the owner's decision all of it went out in the eighth round, and one rule stands in its place, the one this piece had before the sixth mending: a key is let go on the machine when any keyboard lets go of it. Raw Input's account is one for all keyboards, and no device handle and nothing else that names a keyboard is kept (section 3.8, "Two keyboards"). What the rule costs is known, and is the side to fail on: (8) of the list four rows above. The tests that held the telling apart were turned round to hold the one rule, or taken out where they held nothing any more ("hook:" and "control:" as five rows above; hook: `aKeyIsLetGoWhenAnyKeyboardLetsGoOfIt`, `aKeyGoesUpByItsUpAsItIsHeard`, `whatIsPressedAfterAModifierWasLetGoGoesOutBehindItsUp`, `aKeyHeldFromBeforeIsLetGoByAnyKeyboardsUp`, `theLeaveKeyGivesTheKeyboardBackByItsUpAsItIsHeardWhateverRawInputHasStillToSay`, `pauseAndPrintScreenAreLetGoAsTheKeysTheyWentDownAs`; control: `aKeyHeldOnOneKeyboardIsLetGoOnTheMachineByTheSameKeyOnAnother`, `aKeyThatIsKeptBackIsKeptBackNoLongerWhenAnyKeyboardLetsGoOfIt`, `aKeyHeldFromBeforeTheKeysWereHeardIsLetGoByAnotherKeyboardsTap`; and the row for the ninth keyboard's sentence is out of `whenNothingIsHeardAnyMoreItIsSaidWhy`). What that reviewer found beside it is mended in this code, each with tests. (1) The exhaustive test of a modifier checked only the end state of each sequence: a Shift that was told as up and as down again under a finger that never left it passed, and so did a press that went out before its word. Every short sequence of one key, and of a modifier with a key that is typed under it, is held to the rule at every step, against what the fingers did and what each account has told by then and not against the procedure's own table, in every order of the two accounts (hook: `aKeyIsRightAtEveryStepOfEveryShortSequence`, `aModifierAndAKeyAreRightAtEveryStepOfEveryShortSequence`). Each says what it leaves out: a moment that runs out, a telling or a hearing that is lost, a key that was down before the hook went in, more than six events, an interface thread that takes late what it is handed; and that the rows "on two keyboards" are one keyboard's events with one more "up", which is all a second keyboard is to a hook. (2) A repeat's word was taken for a later press of the same key: Del held until it said itself again, let go, and pressed again went out before its own word, and so before a key that Raw Input had still to tell of between the two. A word is for the hearing it belongs to, counted by the sayings of the key that were heard since the press (hook: `aWordOfRawInputsIsForTheHearingItBelongsTo`). (3) Typed keys were left in memory: the row of what the interface thread has to tell kept what had been taken out of it in the memory behind it, and forgot by setting its length to nothing. Each place is wiped as what stood there is taken out, and all of them where the row is forgotten or outgrown (hook: `whatWasToldIsWipedWhereItStood`, which looks at the memory behind the row). (4) Comments: `keyboard_hook.h` said "run by nobody" of a hook that has run once, on its owner's PC. It says what is so: no test installs it. Unchanged above all of it: no test installs or runs the hook (four rows above) | `ui/tests/test_keyboard_hook.cpp` and `ui/tests/test_control.cpp`. What a ninth reviewer found in this round: the next row. The cost of the one rule: M3b, by a person at two keyboards | One of these tests failing says which of them is open again |
| What a ninth reviewer found in that eighth round, read together with the seventh. Nothing high: the take-out is clean, and an ordinary keyboard was traced through rollover, held modifiers, repeats and fast taps, each of which came out right. One thing of middle weight and a few small ones. They are mended in this code, each with tests ("hook:" and "control:" as six rows above), but for what the end of this row names. (1) Medium: a keyboard that goes with a key held left that key down on the machine, and nobody was told. The keyboard is unplugged, a desk switch takes it to another computer, or a wireless link is lost, with Enter, Del, Backspace or a modifier down, and no "up" comes from either account. The key stayed down on the server, a key that is no modifier said again there by the server itself, until keyboard and mouse were given back, the window left the front, or ten idle minutes were over; and whoever has one keyboard has no other to tap the key on. A keyboard that goes ends control now. Windows' notice that a device was taken away is asked for with Raw Input; when any keyboard goes while keys are swallowed, keyboard and mouse are given back, which lets go of everything on the machine, and the bar says why; a device that went and cannot be asked its kind counts as a keyboard; a keyboard that comes ends nothing; and where the notice cannot be had, the bar says so while they are held (section 3.8, "A keyboard that goes"; hook: `whatWindowsSaysOfADeviceThatComesOrGoesIsReadForAKeyboardThatWent`, `aKeyboardThatGoesEndsTheSwallowingWhateverWasHeldOnIt`; control: `aKeyboardThatGoesGivesKeyboardAndMouseBackAndTheBarSaysWhy`, `whereThisPcDoesNotSayThatAKeyboardWentTheBarSaysSo`, and its row in `whenNothingIsHeardAnyMoreItIsSaidWhy`). (2) Low: in the first moment after Raw Input began to tell late, on a PC that was taken not to tell, a key that the hook missed was forgiven, and the next went out bare: Ctrl down unheard in that moment, and "c" to the machine with nothing said. Nothing is forgiven any more. A telling of that first moment which finds no hearing ends control once, with a sentence of its own that is true whichever of the two it was, and with what to do behind it. With the forgiving went Raw Input's own account of what is down, and the letting go of a key by its word, to which nothing else had come (hook: `aKeyThatIsNotHeardInTheFirstMomentAfterRawInputBeginsToTellEndsTheControl`, `anUpThatIsNotHeardInTheFirstMomentAfterRawInputBeginsToTellEndsTheControl`, `whereTheOtherAccountBeginsToTellLaterAKeyHeardBeforeThatEndsTheControlOnceWithAWordOfItsOwn`; control: its row in `whenNothingIsHeardAnyMoreItIsSaidWhy`). What that costs: on a PC that begins to tell while a machine is controlled, with a key in flight at that very moment, keyboard and mouse are given back once, and are taken again. (3) Smaller. "Within the moment", for the same key pressed again after sayings that Raw Input did not tell of, was up to two moments and a look, since the new press's own hearing renewed the time: what was noted of such sayings a moment ago and more is dropped as the press is heard (hook: `aPressGoesByItsOwnWordOnceSayingsThatWereNeverToldOfAreAMomentOld`, which also holds what is left as it was: pressed anew within the moment of such a saying, control ends there). What is typed and was not wiped: a row that was outgrown, the one copy of a key that a pass holds while it is said, and what is read of a key from Raw Input are wiped in Windows' own way of writing nothing over memory, which an optimised build does not drop where the memory is given back right after; and the tables of a hook's thread that did not end in its two seconds are forgotten by whichever comes last, the thread or whoever waited for it (hook: `whatWasToldIsWipedWhereItStood`, which looks at each outgrown row byte for byte, and `whatAThreadThatWasLeftBehindWorkedOnIsForgottenByWhicheverComesLast`; the wipe of what is read from Raw Input is `WindowsKeyboard`'s, and in no test). (4) Tests and comments. The step-by-step tests take what was handed on by itself after every step, and hold the waking and the verdict at every step; one row has a look in the middle; and each says what its rows "on two keyboards" hold. The test of a key of a keyboard that is gone was turned into what is so now ((1) above). The comment at `keyboard_hook.h` on Print Screen says what a hook is told, and the one on "within the moment" what is so. Open: a keyboard that drops out with no word from Windows (a wireless link lost behind a receiver that stays plugged in, some desk switches) still leaves its key down on the machine, with nothing said, until the key is pressed and let go on another keyboard or keyboard and mouse are given back (control: `aModifierOfAKeyboardThatDropsOutUnsaidStaysDownOnTheMachineUntilItIsTappedElsewhereOrTheyAreGivenBack`, which holds that it is so): (7) of the list five rows above. An "up" between the hook going in and Raw Input being asked for (microseconds) leaves its key left out of the comparing on one side only, and control ends at that key's next press with a sentence that is not true of it; no order of the two askings closes it, and it is said at the code. `Procedure::key`'s own locals are not wiped as it returns, nor what the compiler keeps of a key by itself: said at the code. And every thing about the notice of a keyboard going is from memory, and none of it was tried: that Windows posts it to a message-only window, and for keyboards only where only the keyboard was asked for; whether a device that went can still be asked its kind; and what Windows says for a headset, a dock, a mouse with keys of its own, a receiver that is both mouse and keyboard, a hub that takes every device with it, Bluetooth, a remote desktop. Where such a device goes as a keyboard, control ends with no key held, which costs a click: (4) to (6) and (13) of that list. Unchanged above all of it: no test installs or runs the hook. It has run once for real, on its owner's PC, where keys arrived on a real KVM, and nothing else is known from that | M3b: a person at a keyboard, with a made-up KVM first; and the tenth reviewer's reading of this round | Each is mended with a test that is seen to fail first and that drives the plain parts of the real source, not a signal emitted by hand; or left, and said here as what it is |
| A KVM is spoken to straight, whatever proxy the PC uses: set, and not tried (no test has a proxy) | M3a | A test with a made-up proxy that counts what comes to it |
| No password of a KVM stays in Windows' store without its KVM, and no text of a saved action without its action. It can: the program ending between the keeping of a password and the writing of the list (`LocalWall::add`), and an entry of the list that is no longer taken when the list is read, leave one behind. So for a saved action's text (`OwnActions`, section 4): where the program's end cuts off an adding or a removal, where Windows does not do a forgetting (it says nothing of that), and for an action that is left out where the settings file is read. None of those texts can be run (the id and the writing's mark are looked at), none is shown, and nothing takes them out; the store is not asked what it holds | M3a | The store learns to list its names, and what has no KVM or no action is forgotten at the start |
| The saved actions of this PC's wall are kept as a QKVM server keeps its own, and the text one types as a secret of this PC (`OwnActions`, section 4). Tried against a store in memory that takes in one entry what Windows' own takes, and once against Windows' real store, by one test under a name of its own that takes back what it wrote (`core/tests/test_own_actions.cpp`, `test_settings.cpp`). A reviewer who had not seen it read it on 2026-10-17: nothing high or medium, six low. Mended after that reading: an action whose count is within two of `action::mostEdits` is not changed again, where it would have been gone at the next start (`anActionChangedAsOftenAsAnyMayBeIsNotChangedOnceMoreAndStaysOnTheList`). Open from it: (1) a text that stays in the store with no action naming it, the row above; (2) an action's name is counted in places of a text (a sign from far up takes two), where the server and the program's own reader of a file count signs: names of 31 to 60 such signs, which the server keeps, are refused here, which matters once a server's file is taken in with its actions; (3) saving builds a whole call of the driver to check a text (`action::isTypedAsOne`), which leaves two copies of the text in memory that are not written over; (4) the copies of every secret that `WindowsSecrets::keep` and `kept` leave in memory are not written over, KVM passwords included (older than this). Updated on 2026-10-21: the saved-action editor and action import/export are built; the earlier review findings above remain unless explicitly marked fixed. And taken as it is from that reading: a change of the list is said as the wall's own change (`LocalWall::changed`), so whoever shows the wall asks for it anew and looks at its KVMs once more, as after an edit of a KVM; that costs one round of looks at every KVM of the wall for each change of the list. | M3a, with the box that manages them | Each is mended with a test seen to fail first, or stays said here as what it is |
| Windows' store keeps every secret a wall may have. It does not: on this PC it refused its 1010th entry (error 8), and its writes had slowed from 17 ms to 166 ms each by the 900th (found by a reviewer, on this PC alone). A wall of 2000 KVMs with passwords (`Settings::mostKvms`) and 500 saved actions of up to four entries each (`exchange::mostActions`, `OwnActions::mostParts`), with the servers' sessions and links beside them, cannot all be kept: past the store's limit, what has to keep a secret is refused (a KVM whose password cannot be kept is not added: section 4), and near it every writing takes some ten times as long | open, for the owner | Fewer entries (several secrets to an entry), a lower limit on the wall, or the stricter way's sealed store (SPEC.md section 8): the owner's to choose |
| A KVM whose certificate was signed by somebody Windows does not know, and that sends that signer's certificate along, can be added. It cannot: the probe takes it, and the driver then refuses it as signed by a maker (the strict rule of section 4), so the adding ends there. Found by a reviewer, not tried: the made-up KVMs send no signer | M3a, with the stand-in that sends a signer | Either the box says so before anything is typed, or such a KVM is known by its own certificate as the others are, once it is settled that Qt passes nothing else over for it |
| The driver that signs in once for an adding or a change signs out again. It does not: each adding leaves a session on the KVM until that ends by itself | M3a | A sign-out when that driver is let go, which may not hold up the box |
| `core/kvm_editing` is fit to be used by a window. A reviewer's five findings in it are mended, and a window uses it now (section 5). Where the kept password may go is the wall's own rule, whoever asks (`keptPasswordMayGoTo`, in `LocalWall::change` as in `signerIn`). A KVM that is on the wall over HTTPS is not changed to plain HTTP at the same place, with its password typed or without (`intoTheClear`, `not_in_the_clear`). A KVM is not moved onto the address of another one of the wall (`duplicate`), nor onto what shows another one's certificate (`same_device`). A change does not save over a password that was saved anew meanwhile (`LocalWall::secretWrites`, `conflict`). And what a change tells back names the certificate it remembers (`Changed::certificate`). What the box does with all this is the next row | `core/tests/test_kvm_editing.cpp` (`aKvmOnTheWallOverHttpsIsNotChangedToPlainHttpAtItsPlace`, `theAddressOfAnotherKvmOfTheWallIsNotTaken`, `aKvmIsNotMovedOntoAnotherOneOfTheWall`, `aChangeIsNotSavedOverAPasswordThatWasChangedMeanwhile`, `whatAChangeRemembersIsTold`) and `core/tests/test_local_wall.cpp` (`withoutItsPasswordAKvmIsChangedOnlyToWhereTheKeptOneMayGo`, `aKvmOnTheWallOverHttpsIsNotChangedToThePlainWayAtItsPlace`, `aKvmIsNotChangedIntoAnotherOfTheWall`, `everyWritingOfAKvmsSecretIsCounted`) | One of these tests failing says which of the five is open again |
| The box that changes a KVM is fit to be handed out. Not yet. A reviewer read it on 2026-10-12, and the three findings this row named are mended, each with tests. Of a KVM that is known by a certificate, a password typed for a new address does not go to what shows another certificate there, one that Windows does not trust, before the box has shown the two and somebody said yes to that very one (`certificate_differs`, `Typed::saidYesTo`); and a saving that changes what the KVM is known by leaves the box open on it. "Trust this certificate" stands where no other button of the box ever stands, takes no click for a moment after the two have come, and is not reached by the keyboard. A box that was left open does not save over what another window changed (`KvmEditing::Seen`, `conflict`). And a window that goes out of sight has its boxes closed, with what they asked ended. A second reviewer read the mending on 2026-10-12, and the four things this row listed from that reading are mended now, each with tests. (a) Where what answers shows another certificate than the one that was just said yes to, the box says so, marks it, and the yes rests anew, three times as long (`yesWasToAnother`, `trustRestsAnewMs`; a look hands the new one over with its refusal, `KvmCertificateReview::Outcome::shownNow`). (b) A move to an HTTPS address has one rule now: with its address changed, a KVM is signed in to only where it will be known as it is known now (the same certificate; or Windows' word here and there), or after somebody was shown what it will be known by there, beside what it is known by now, and said yes. That holds also for a KVM that was known on Windows' word or spoken to in the clear, and where Windows trusts what is shown there, which takes a yes of its own (`Typed::saidYesOnWindowsWord`, `Outcome::onWindowsWord`, the button "Trust Windows' word"). (c) The moment in which the yes takes no click runs only while the two are looked at: the box open on them, in the active window (`lookedAt`). (d) What was saved, trusted or added in the turn in which its box was closed from outside is said as a notice when the window is seen again (`cameTo()`, `Notices::tellWhenSeen`, `setSeen`). Mended with them: `OwnWall` hands on a yes only for what it last told that box to show, and once (`Offered`); a writing of a KVM's secret is counted once it has happened (`LocalWall::secretWrites`), so that a box whose saving could not be written down saves at its next try; and where a look finds the certificate of another KVM of the wall, the box says whose it is and has no yes (`presentedIsOf`). A third reviewer read that mending on 2026-10-12, and the four things this row listed from that reading are mended now, each with tests. (1) A KVM that is spoken to over HTTPS and is moved, with its password typed, to a plain-HTTP address at another host, or at another port of the same host, is refused before anything is sent there (`plain_http_there`), and is signed in to there only after a yes to plain HTTP at that very address (`Typed::saidYesToPlainHttpAt`, the button "Use plain HTTP"); the test that held the old way was turned round. (2) The box keeps the last yes for each address while it is open for the KVM. A third certificate that first shows at the sign-in itself, after a yes, still hears nothing (`cert_mismatch`); shown at the next saving, it is said to be another than was said yes to, marked, and has the longer rest (`yesWasToAnother`). (3) A notice that waits for a window that goes for good is told in the first window that is open (`Notices::takeWhatWaits`, `WallWindows::forget`); where the program ends there is no such window, and it is told to nobody. (4) A test takes "Windows' word" through `OwnWall`. Mended with them: what is offered is what is shown, and a yes is one only where the box says that this is what its button was pressed on (`keepAsOffered`, `EditKvmView::offers`, `saysYesTo()`); a look at a certificate and the yes to it are held to the KVM the box was filled with, as a saving is (`filledWith`, `fillAnewOver`, and `seen` at `KvmCertificateReview`); a window's notices count as seen while the window is in sight, not minimized, and the active one; a certificate in a notice stands in lines of eight pairs; and a KVM that is known by Windows' word, moved to where a certificate is shown that Windows does not trust, is shown as known by Windows' word beside that certificate, with what a yes gives up. A fourth reviewer read that mending on 2026-10-12, and what that one found is NOT mended in this code. (i) Where a yes was to a certificate, the sign-in was refused, and Windows then trusts that same certificate, the box says "another certificate" of what is the same one. (ii) A box that was opened for the certificate alone, and whose yes is refused as "conflict", says that the fields were filled in again, where it shows none. (iii) Nothing of this box was tried by a person, or against Windows' real store of certificates: where a test needs Windows to vouch, it has makers of certificates of its own | The mended, from the first reading: `core/tests/test_kvm_editing.cpp` (`whatShowsAnotherCertificateAtANewAddressIsNotSignedInToBeforeSomebodySaidYes`, `withTheYesToItACertificateShownAtANewAddressIsWhatTheKvmIsSignedInToAndKnownBy`, `aYesToOneCertificateIsNoYesToWhatIsShownInItsPlaceByThen`, `whatTakesThePlaceOfWhatWasSaidYesToBeforeTheSignInGoesOutHearsNothing`, `whatWasTypedOverAKvmThatIsAnotherByNowIsNotSaved`), `ui/tests/test_own_wall.cpp` (`aPasswordTypedForANewAddressGoesToNoOtherCertificateBeforeTheBoxShowedTheTwo`, `aSaveThatChangesWhatAKvmIsKnownByLeavesTheBoxOpenOnWhatItIsKnownByNow`, `aYesIsNotPassedOnInTheFirstMomentAfterTheTwoCertificatesHaveCome`, `aBoxThatWasOpenedBeforeAnotherBoxChangedItsKvmDoesNotSaveOverThat`), `ui/tests/test_box_screens.cpp` (`whatIsShownAtAnAddressThatWasTypedIsLookedAtInTheBoxBeforeThePasswordGoesThere`, `theYesToACertificateStandsWhereNoOtherButtonOfTheBoxEverStands`, `theYesToACertificateTakesNoClickForAMomentAfterItHasAppeared`, `theYesToACertificateIsNotReachedByTheKeyboard`, `whatASavingChangedOfHowAKvmIsKnownOrSpokenToIsInSightInTheBoxThatStays`) and `ui/tests/test_wall_windows.cpp` (`aWindowThatGoesEndsWhatItsBoxesHadAsked`, `theLastWindowOutOfSightBehindTheIconHasItsBoxesClosed`, `whatAWindowsBoxAsksInTheTurnInWhichTheWindowGoesIsNotDone`). From the second: `core/tests/test_kvm_editing.cpp` (`whatReachesANewAddressBeforeAYesAndAfterItByHowTheKvmIsKnownAndWhatIsShownThere`, `whereWindowsVouchesAtTheNewAddressTheCertificateAKvmIsKnownByIsGivenUpOnlyOnAYes`, `whatAKvmShowsInThePlaceOfWhatWasSaidYesToIsHandedOverToBeShown`, `whatAChangeCameToIsKnownFromWhenItIsSettledUntilWhoeverAskedIsTold`, `whatAYesCameToIsKnownFromWhenItIsSettledUntilWhoeverAskedIsTold`, `whatCouldNotBeWrittenDownIsSavedAtTheNextTryFromTheSameBox`), `core/tests/test_local_wall.cpp` (`aWritingOfAKvmsSecretIsCountedWhenWhatIsKeptIsAnotherAndNotBefore`), `ui/tests/test_own_wall.cpp` (`afterAYesToWhatIsShownNoLongerTheBoxSaysSoAndItsYesRestsForLonger`, `theMomentBeforeAYesRunsOnlyWhileTheTwoAreLookedAt`, `aYesIsOneOnlyForWhatItsBoxWasLastToldToShowAndOnce`, `whatWasDoneInTheTurnInWhichItsBoxWasClosedIsSaidForItsWindowToTell`, `aKvmThatWasAddedInTheTurnInWhichItsBoxWasClosedIsSaidForItsWindowToTell`, `whatShowsTheCertificateOfAnotherKvmOfTheWallIsSaidInTheBoxAndTakesNoYes`, `aBoxWhoseSavingCouldNotBeWrittenDownSavesAtItsNextTryWhatItStillHolds`), `ui/tests/test_box_screens.cpp` (`whatTookThePlaceOfWhatWasSaidYesToIsSaidInTheBoxAndMarkedAndTheYesRestsForLonger`, `whatAKvmWouldBeKnownByAtAnAddressThatWasTypedIsShownBesideWhatItIsKnownByNow`, `theYesToACertificateRestsWhileItsWindowIsNotTheActiveOne`, `whatShowsTheCertificateOfAnotherKvmOfTheWallIsSaidInTheBoxWhichHasNoYesForIt`, `whatABoxHadDoneAsItsWindowWentOutOfSightIsToldWhenTheWindowIsBack`), `ui/tests/test_wall_windows.cpp` (`whatWasDoneAsTheLastWindowWentBehindTheIconIsSaidForWhenItComesBack`) and `ui/tests/test_notices.cpp` (`whatIsToldForWhenTheWindowIsSeenStaysUntilSomebodySeesTheWindow`). From the third: `core/tests/test_kvm_editing.cpp` (`whereTheNewAddressIsPlainHttpAPasswordThatIsTypedGoesThereOnlyOnAYesToThat`, `aYesToPlainHttpIsOneForThatAddressAloneAndForAPasswordThatWasTyped`, `aYesOnWindowsWordThatMeetsWhatWindowsDoesNotTrustAtTheSignInSendsNoPassword`, `aKvmThatIsNotWhatWhoeverAsksHadBeforeThemIsNeitherReadNorTrusted`), `ui/tests/test_own_wall.cpp` (`aPasswordTypedForPlainHttpElsewhereGoesThereOnlyOnceTheBoxSaidYesToThat`, `whatFirstShowsAtTheSignInAfterAYesIsSaidToBeAnotherWhenTheNextSavingShowsIt`, `whatTheBoxHoldsAgainstWhatIsShownAtAnAddressIsTheLastYesForThatAddressWhileItIsOpen`, `whatABoxDoesNotShowIsNotOfferedToItAndAYesToItIsNone`, `aLookAtACertificateAndTheYesToItAreHeldToTheKvmTheBoxWasFilledWith`, `aWishThatNamesAnotherKvmThanItsBoxWasFilledWithIsNobodys`, and for "Windows' word" through `OwnWall` `whereWindowsVouchesAtANewAddressTheBoxShowsItsWordAndAYesIsToThat`), `ui/tests/test_box_screens.cpp` (`whatIsGivenUpAtAPlainHttpAddressIsShownBesideWhatTheKvmIsKnownByAndTakesAYesOfItsOwn`, `whatIsShownAtALaterSavingIsHeldAgainstTheYesThatWasGivenForThatAddress`, `aKvmThatIsKnownByWindowsWordIsShownAsThatBesideTheCertificateThatWouldTakeItsPlace`, `aCertificateInANoticeStandsInLinesOfEightPairsAndIsBrokenInsideNone`), `ui/tests/test_wall_windows.cpp` (`whatAWindowThatGoesForGoodStillHadToSayIsSaidInAnotherThatIsOpen`, `aWindowsNoticesAreSeenWhileTheWindowIsLookedAtAndNotWhileItIsOnlyInSight`, `whatABoxSaysAsItsWindowGoesOutOfSightIsSaidToAWindowThatNobodySees`) and `ui/tests/test_notices.cpp` (`whatStillWaitsIsTakenOutToBeToldSomewhereElse`). What is not mended: M3a, each mending with a test seen to fail first; and the box by a person at it, with Windows' own store of certificates | The box is not handed out before each is mended, and the mending read by a reviewer who has not seen it |
| A KVM's password is never sent to another place than it was saved for, whatever fails. Built as this row asked: the password is kept in one secret together with the address and the certificate it was saved for (`QKVM-KVM-2`, section 4), and one that was saved for something else is not used. Where the list cannot be written and putting the old password back fails too, or the program ends between the two writings, the store still holds a password for another address than the list names: that KVM then has no password (`no_password`), and nothing of it goes anywhere. The second of the two is tried with a store filled by hand, since nothing in a wall ends the program half way | `core/tests/test_local_wall.cpp`: `aSecretThatWasNotSavedForTheKvmAsItIsIsNotUsed`, `whereThePasswordBeforeCannotBePutBackTheOneThatStaysGoesNowhereElse`, `whereTheProgramEndsBetweenTheKeepingOfAPasswordAndTheWritingOfTheListThePasswordIsLost` | The password is typed again for the KVM as the list has it. What stays in the store meanwhile is the row on Windows' store above |
| What stands in the settings as a KVM's name, group and user name is read by the rules it was typed under. It is cut to length and no more: a file written by hand can hold a name with a line's end or a turned direction in it, and a cut can halve a sign of two places | M3a | The rules of `kvm::nameFrom` and its like move where the settings can use them |
| No line to something that is not the server, or not the KVM, stays up unlooked-at. Built so in `ServerClient` and in `KvmdDevice` (Windows objects every time; passed over only for the certificate it is known by, and only where every objection names that certificate itself). Tried for `ServerClient` with a relay that stalls a line past its limit and lets it come up afterwards, with no question waiting: the old shape sent the session down it, this one does not (`aLineThatComesUpWithNoQuestionWaitingCarriesNoneLater`). The driver has the same shape and no such test of its own. Not tried at all: a place that sends its signer's certificate along, where Windows' objection names the signer and the certificate is refused as one to be known by (the tests' own server sends none along) | M3a | The relay for the driver's tests too; and a stand-in that sends a chain, which Qt's own server on Windows does not: the app's made-up KVM in Python does, with the rig |
| A KVM that stays away is asked less and less often: built as the server's driver has it (the pause doubles), tried only as far as "left alone for a while" | M3a | A test that counts the questions over the pauses |
| An answer to the driver without a stated length that is larger than is taken: left unread until it is whole, as the driver leaves it, it never becomes more than Qt's store holds and ends as one that took too long, not as one that is too large (found by the probe's tests, where it is taken out as it arrives). It is bounded either way; the driver tells it by the wrong word | M3a | The driver takes an answer out as it arrives, as the probe does |
| Everything the program says over TLS is said with Windows' own. It is chosen in one place, at the first call (`useWindowsOwnTls()`), which answers whether that is what is in use; in `DeviceLine` and `LiveLink` the choice is made before the socket is. Where it is not what is in use (TLS was in use before anybody called it, with something else, or Windows' own is missing from this copy of the program), each of the five that speak TLS refuses an HTTPS address before anything goes out, in one code and one sentence (`refusedForItsTls`, `damagedTls`, `desktop:tls_damaged`: section 4). Built, and tried in a process in which another TLS was chosen first; two other test programs ask for a yes where they begin. Not tried: a copy of the program from which Qt's part for Windows' own TLS is really missing. Not there: the program does not say so at its start; it is said where an address is typed, and on a KVM's card. What a reviewer found here on 2026-10-12 is mended: `desktop:tls_damaged` is one of the words that end the asking for a live picture (`LivePicture`), where it was asked for again and again before, as after a line that ended | `core/tests/test_tls.cpp` (`aLineThatIsTheFirstThingMadeSpeaksWindowsOwnTls`, `whetherAnAddressIsSpokenToIsDecidedByItsKindAndByWhoseTlsItIs`) and `core/tests/test_tls_other_first.cpp` (`theDriverOfAKvmRefusesAnHttpsAddressBeforeAnythingGoesOut`, `theQuestionOfWhatAnswersThereIsNotAskedOfAnHttpsAddress`, `aQkvmServerAtAnHttpsAddressIsNotNamed`, `aLineToAKvmIsNotAskedForAtAnHttpsAddress`, `aMachineIsNotHadLiveOverALineThatWouldBeEncrypted`), and `ui/tests/test_live_picture.cpp` (`whatThisCopyOfTheProgramDoesNotSpeakToIsNotAskedAgainWhileTheMachineStaysShown`). What is not there: M3a | The program says so at its start |
| What a gateway answers for a QKVM server, and a server that is away, are taken as carefully as the driver takes them of a KVM. The two things a reviewer found here on 2026-10-12 are mended, each with tests. A gateway's 502 or 504 on something that was to be done to a machine, in anything but the server's own shape, leaves open whether it was done (`ServerClient::act` tells it as `desktop:unreachable`, which `mayHaveBeenDone` counts), as the driver for a KVM has it (section 4); so does what breaks off or is not understood after the server said yes, and an answer that is cut off for its size, whatever its number. It was read as a clean refusal, and a run over several machines offered it again. And a kept link that was handed in, and neither taken nor called no good, is handed in less and less often (`ServerSource::handTheLinkInLater`: twice as long for each time in a row, five minutes at the most, and never sooner than a `Retry-After` that came with a refusal in the server's own shape, of which the same five minutes at most are taken), while its wall is up and where the screen starts while its server is away (section 4, "Display links"). So is a link that is taken and whose session does not hold: such a taking counts as none, for the waiting and for whether contact is back, until a look at the machines was answered under it. (That was open here: taken each time, the link was handed in at the pace a server that is away is asked at, for as long as that went on.) A session that held, and is gone a moment after its link was handed in, is come back to no sooner than a server that is away is asked again (`handedTheLinkInJustNow`). Mended before, in the same work, with tests: an answer that broke off is no answer (`anAnswerThatBreaksOffIsNoAnswer`), what is typed for the wall as a file goes only to what was just asked what it is (`whatIsTypedForAFileGoesOnlyToWhatWasJustAskedWhatItIsAndIsAQkvm`), and a wall screen that starts while its server is away comes to its wall by itself (`aScreenThatStartsWhileItsServerIsAwayComesToItsWallByItself`) | `core/tests/test_server_client.cpp` (`whatAGatewaySaysOfSomethingThatWasToBeDoneIsNoRefusal`, `whateverBreaksOffAfterTheServerSaidYesMayHaveBeenDone`, `howLongAServerAsksToBeLeftAloneCountsOnlyInItsOwnAnswerAndNotForLongerThanFiveMinutes`) and `ui/tests/test_server_session.cpp` (`aLinkThatAQkvmDoesNotTakeIsHandedInLessAndLessOften`, `aScreenThatWaitsWhereItsServerIsNamedHandsItsLinkInLessAndLessOften`, `aLinkThatIsTakenAndWhoseSessionNeverHoldsIsHandedInLessAndLessOften`) | One of these tests failing says which of them is open again |
| After contact with a QKVM server was lost, its session goes to nothing that has not said what it is, and the rule costs nothing else (`ServerClient`: `lostContact`, `askWhatAnswersNow`, `desktop:not_sent`; section 4). Built, and tried against the tests' stand-in (`afterContactWasLostNothingCarriesTheSessionBeforeAQkvmHasSaidWhatItIs` in `core/tests/test_server_client.cpp`, `whatAnswersWhereTheServerWasWhileItIsAwayIsHandedNoSession` in `ui/tests/test_server_session.cpp`). A reviewer read it on 2026-10-12 and found four things, of which three are mended, each with tests, and one stands. (1) When contact was lost, every question that only asks and was still out was given up. So one picture that was too slow or too large, or one answer outside the server's shape, could take the look at the machines with it: the wall then counted as away, and the live line of the machine that is shown large was ended, keyboard and mouse with it. Mended: nothing that is out is given up but a POST or a PUT that has not left; and contact is lost only by what says something of who answers, which a picture that is too large or too slow does not (section 4). (2) A 401 in any shape, a gateway's too, was taken for "signed out", and the kept session was forgotten. Mended: only a 401 in the server's own shape is that; any other is contact lost, and the session is kept. (4) `Retry-After` was taken from whatever answered where a link's key was posted, and for up to an hour. Mended: only from a refusal in the server's own shape, and for five minutes at most (`linkWaitSaidMs`, `longestLinkWaitMs`). Mended with them: what a QKVM said of itself brings contact back only where it was asked after contact was last lost; a picture is one only where it says that it is a JPEG or a PNG and begins as one; a QKVM that comes back speaking another version of the API is told as that, and the wall is left for where a server is named; and HTTP/2 is not used with a server, which is set, and not shown by a test. What stands is (3): over plain HTTP the session still goes out, with whatever was out or is sent until an answer says that it is no QKVM's, and with a GET or a PUT that Qt asks again by itself where the line fell before an answer. Qt 6.12 offers no switch for that, and a sender of the program's own is not built: only a certificate keeps the session from what is not the server. Since round R4b of the several sources, what Qt was sending again is taken back where contact is lost; what is still open of (3) is the row after the next. What the reviewer of this mending found is the next row | `core/tests/test_server_client.cpp` (`whatIsOutWhenContactIsLostIsNotGivenUpAndWhatIsAskedNextWaitsForWhatAnswers`, `aPictureThatIsTooLargeOrTooSlowFailsAloneAndContactIsNotLost`, `a401InAnotherShapeThanTheServersIsNotTheNewsThatASessionIsGone`, `howLongAServerAsksToBeLeftAloneCountsOnlyInItsOwnAnswerAndNotForLongerThanFiveMinutes`, `whatAQkvmSaidOfItselfBeforeContactWasLostDoesNotBringContactBack`, `whatIsNoPictureOfAKindThisProgramDrawsIsNoQkvmsAnswer`, `aQkvmThatComesBackSpeakingAnotherVersionIsToldAsThat`, and `whatQtAsksAgainByItselfWhereItsLineFellBeforeAnAnswer`, which holds what stands), `core/tests/test_server_types.cpp` (`aPictureIsOneOnlyWhereItSaysSoAndBeginsAsOne`) and `ui/tests/test_server_session.cpp` (`oneMachinesPictureThatFailsDoesNotTakeTheLiveMachineDown`, `whatAnswers401ToEverythingInTheServersPlaceSignsNobodyOut`, `aQkvmThatComesBackSpeakingAnotherVersionOfItsApiIsSaidWhereAServerIsNamed`). For (3): M3c | One of these tests failing says which of the three is open again. For (3): a sender of the program's own for the calls that carry the session, as the line to a KVM has one; or what cannot be kept back over plain HTTP is said where a server is named that way |
| What a reviewer found on 2026-10-12 in the mending of the two rows above and of the row on a key that is down when its line is closed (the session with a QKVM server after contact was lost, and a KVM's line that was let go), and is NOT mended in this code, but for (a). One medium, the rest low. The medium one, which is mended: (a) `Live::leftDown` is heard in `ui/`, by `MachineControl`, and the bar says that something may still be down on the machine. One that is said late, seconds after keyboard and mouse went, to a `Live` that holds them anew in the same window by then, is said in that holding as a line of the bar's, and takes nothing from it; it goes when they are next taken, or another machine is shown (section 3.8, "Something may still be down"; `thatSomethingMayStillBeDownOnTheMachineIsSaidInTheBar` in `ui/tests/test_control.cpp`). Low, and not mended: (b) A loss of contact that is noticed while the client is already unsure is not counted (`lostContact` returns before `m_lost`), so what a QKVM said of itself to a question that was asked before that loss brings contact back all the same. (c) A goodbye of the KVM's own accord that crosses the one from here counts as its answer to it (`arrived`), which is a guess; and `arrived` is tried in the clear only, not over TLS. (d) Signing in and handing in a display link take the session's cookie from any good answer (2xx), before the answer was read as an account (`signIn`, `openDisplay`). (e) What proves that the session a link opened holds, after contact came back, is one answered look at how the machines are: the waiting begins anew behind that one. (f) The five-minute limit on `Retry-After` cuts the server's own word where that asks for longer. (g) What answers `/healthz` in a QKVM's shape and lists other versions than this program's is believed as far as being told so, whatever it is: the wall is left, and it is asked no more. (h) The test `aLineThatIsLetGoIsKeptUntilItHasGoneAndSaysNothingToWhoeverHeldIt` says more in its name than it shows. (i) What Qt asks again by itself, (3) of the row above. Found after that reading, by a test that failed one run in six: (j) one look at the machines that gets no answer makes the wall count as away, and the live line of the machine that is shown large is ended for that, keyboard and mouse with it, although that line stood; it is had anew once the QKVM has said what it is. One way to it is Qt's own: where the server hangs up under an answer that had begun, Qt fails with it every question to that server that still waits for a line, a look at the machines among them (`server_client.h`). The test's made-up server hung up under a picture it had called too large, which no QKVM does; it keeps its line there now (`moreToCome`), as a server that is still sending does. Whether a live line that stands should be kept while the wall is unsure is open. (k) What had not left when contact is lost is taken back since the same day (section 4): a reviewer read that mending and found nothing high; open from it are the turn of the loop and the way to Qt's own thread in which Qt still sends what it gets a line for, and the cases without a test that section 4 names. Since round R4b of the several sources, what has left with no answer begun is taken back too, where Qt would send it again by itself: the next row | M3c | Each of those that are open is mended with a test seen to fail first, or left and said here as what it is |
| What Qt sends again by itself goes, with the session, to the QKVM server it was asked of and to nothing else. Over HTTPS it does: every line, one that Qt makes anew by itself among them, has the server's certificate looked at before a byte of a question goes out (section 4), and so HTTPS was never open to this. Over plain HTTP, what has left with no answer begun is taken back where contact is lost, a GET to be asked again behind the question for what answers there, a PUT given up (`takeBack`, section 4; `whatQtSendsAgainByItselfToWhatIsNotThereIsTakenBackWhereContactIsLost` in `core/tests/test_server_client.cpp`, which failed every time before). A reviewer read that mending in round R4b of the several sources: right for what it covers. Open over plain HTTP: for about four seconds after a server goes, which is how long Windows goes on trying to make a line on this PC, a GET that was out (a picture the server was still fetching, a look at the machines) may be sent again by Qt on a new line, with the session, to whatever begins to listen at the address meanwhile, where nothing has said by then that contact is lost. With only GETs out when a server goes, which is the usual case, nothing says it in time: the look that would find nobody there is tried for those same seconds. Qt says nothing when it begins that line, and Qt 6.12 has no switch for it. And an order taken back after it left is told as failed, where the server may have saved it | M3c | A sender of the program's own for the calls that carry the session, which never sends twice, as the line to a KVM has one (`device_line.h`); and an order taken back said as "may have been saved" |
| A wall screen can be ended at this PC by a person, always, and by nothing else. By nothing else: held (section 5, "A wall screen"). By a person: only through `ServerSource::signOut()`, which needs that server's wall up here, and the screens offer "Sign out" only on a wall. So a screen whose server took its link back is ended only where somebody signs in at that server anew, with an account of it, and signs out; and a screen whose server was given another address, or is gone, cannot be ended in the program at all: only by taking its entry ("QKVM Desktop: wall screen") out of Windows' Credential Manager by hand, which ends every wall screen of this PC, and that at the next start | M3d | The program's own password brings an end that needs no server: a person who knows it signs the screen out here, whatever became of the server |
| What makes this PC a wall screen cannot be taken away at the PC without the program. It can: the mark is an entry in Windows' store (`WallScreen`, under "wall screen"), which anybody at the unlocked Windows session can take out, with Windows' Credential Manager or a program of their own, and the next start acts on every machine again. And an entry that Windows cannot read at a start reads as no wall screen. Not tried against Windows' real store: the tests keep secrets in memory | M3d | The mark is kept in the record of the program's own password, which that password guards, and a record that cannot be read is treated as forgotten, never as a start without one (SPEC.md section 8) |
| While the program only shows, nothing on this PC is changed by a press. Not yet: this PC's own two boxes for its wall as a file (in the settings' "Export and import": `OwnFilesView`) and the box that adds a KVM (`AddKvmView`, also from the settings' "Add KVMs") are not held. At a screen nobody sits at, a KVM can be added, this PC's wall written to a file (a sealed one, under a passphrase of whoever types it, with its KVMs' passwords), and a file's KVMs taken onto it. None of the three acts on a machine of the wall, and a file's KVMs are asked what they are before a password goes to them (section 4). Held: the box that changes or takes off a KVM (`EditKvmView::setMayChange`), and the server's boxes for its wall as a file | M3d | Held as the box that changes a KVM is, where the session serves the boxes: M3d holds them |
| The wall as a file is read and written as the server reads and writes it (`core/exchange`). Four reviewers read it, one after each round of mending (2026-10-12 and 13); none found anything high, and the scrypt, the sealing, the bounds on a hostile file and the secrets in memory were read line by line. Held against the server: six files made by the server's own `pack`, read to what is in them; what this writer made was read by the server's own `unpack` (by hand: it is in no test program, since the tests run no Python). The fourth reading found one thing of middle weight in the last round, which is mended (a console link with a name and a password in it was written as it was, into a plain file too: `aConsoleLinkThatAServerRefusesForCertainStopsTheFile`). Open from that reading: `nameSetOff` guards the sign and not the look (signs that join two quotes, more look-alikes of a quote, a name that overprints the sentence); a file at its largest that is read whole cannot always be written again (a sign beyond ASCII is written in six or twelve bytes); `ident`, `program` and the certificate of a device that is not pinned are written unjudged, and a fingerprint is not held against the certificate beside it; two sentences name no entry; `isThisPcsOwn` does not flag `[::ffff:0.0.0.0]`, this PC's own name or its addresses on the network. Open before it: the reader leaves out a device whose console link has a zone, a `[v1.x]` or a control sign in front, which a server keeps; a device's address being ASCII rests on Qt's encoding of it; what the server's libraries make of some names is from memory (two kinds of name get a line beside the file for that); a failing call of Windows' cryptography is shown by no test (it rests on reading); this piece is used by `WallExchange` in the core (the next row), and by this PC's two boxes (the row after it) | with the wall's part of M3c | Each is mended with a test seen to fail first, or said here as what it is. The server's own reader has four weaknesses that this work showed (a password in a quoted address; the seal's associated data by the reader's version; the file key's length unchecked; numbers taken as strings): those are the server's to mend |
| No password of a KVM of this PC's wall goes to an address that was not asked what it is; and a file's KVMs come onto the wall only as they were laid out (`LocalWall`, `KvmProbe::askAsKnown`, `WallExchange`; section 4). Two reviewers who had not seen the code read it, the first the building and the second the mending: neither found a way for a password to reach an address before it was asked, and nothing high. Tried against made-up KVMs on this PC: all of it (`core/tests/test_local_wall.cpp`, `test_kvm_probe.cpp`, `test_kvm_editing.cpp`, `test_settings.cpp`, `test_wall_exchange.cpp`). On one real KVM, which its owner named, on 2026-10-13, looking only (`qkvm-kvmd-rig --probe 1 --known`): asked under the certificate it is known by, it answered as a KVM in about 135 ms, three times of three; held to another certificate, and held to Windows' word (its certificate is its own), it heard nothing; and the merged driver signed in to it under its certificate and took pictures as before. Not watched by anybody: the program itself, started with a settings file that exists (a wall that worked the day before), against real KVMs. From the second reading: (1) Mended on 2026-10-13, and the mending read by a third reviewer, who found nothing against the rule. At a start, KVMs that are off and stood first held all eight places for as long as each was given (4.5 s and more), and those that are well waited behind them; an asking holds its place for 0.4 s now and goes on without it, 48 in all (section 4). From that third reading, mended with it: those that were asked in vain are asked again by turns (in the wall's order, beyond some two hundred that stay off, the last were never asked a second time, and said "nothing answers" also once they were well). Left from it: the tests of this hold times with less than a second to spare, and will fail on a busy PC before the code is wrong (`whoeverAsksHowTheKvmsAreDoesNotWaitForAnAskingThatHasGoneOnWithoutItsPlace`, `asManyAreAskedAtOnceAsThereMayBeAndNoMoreWhateverTheWallHolds`, `kvmsThatAreOffAndStandFirstDoNotKeepTheOnesThatAreWellWaiting`); no test counts the lines or holds the 48 at the program's numbers; beside the 48 there is one asking for each open box and a network manager for each KVM that is signed in to; and a suspicion that was not settled: 48 askings of names that do not resolve share Qt's one pool for looking names up with every driver, so a KVM that is well and is addressed by name may not be let in within its time beside them. (2) Mended: the wall says by itself what a KVM came to, and whoever shows the wall looks then (section 4). One assertion of the first-start test stays eased: a quick KVM whose asking outlasts 0.4 s, or whose first look outlasts 1.5 s, says "connecting" in the first answer, until a tenth of a second and one answer after that look. And an ordinary start has the wall asked how its KVMs are once or twice more than it needs, which costs no call to any KVM. (3) In the two boxes a mistyped address waits longer for its sentence: 4.5 s where nothing is there (it was 2), 4.5 s over HTTPS and 8 s over plain HTTP where something takes the call and never speaks (2 and 3), 36.5 s where something drips bytes (5). Cancel is still at once; the asking it leaves lives on unheard for up to 36.5 s. (4) Mended on the wall: where all that answers is a gateway's 502, 503 or 504, the KVM reads as offline, in a sentence that says only a gateway answered (section 4). Left: the box that adds a KVM and the box that changes one still say "not a KVM" of the same gateway; what answers 502 at kvmd's place and a 404 or a page elsewhere stays "not a KVM" (a KVM's own front may do that; not seen); and something that is no KVM at all behind a gateway that says 503 reads as away for ever, where it read as "not a KVM". (5) Three sentences that are finished in QML put a name in as it is ("%1 saved", "%1 added", "shows the certificate of %1"): `nameSetOff` holds for the sentences the core makes whole. (6) The box that adds a KVM by hand still keeps the address as the name where none is typed, and that name is stale after a move. (7) A wall of two thousand KVMs whose names and passwords are long and of signs beyond ASCII throughout does not fit a file even without its certificates: refused and said, nothing written. (8) In the tests: `WallOfATest` guards five calls (`askStatus`, `askSnapshot`, `act`, `askWakeTargets`, `openLive`); `forABox()` fails the test and hands the wall over all the same; other test files still hold addresses of private ranges, held off the network by each test's own care. (9) Not tried: a wall of two thousand; that memory is wiped (it rests on the types); Windows' real store of secrets; a KVM that is slow to let a line in over plain HTTP (made over TLS only). The boxes for a file call `WallExchange` (the next row). Updated on 2026-10-21: saved actions and shared wall settings are taken; server alerts remain unsupported locally | M3a, by a person who starts the program with their own settings file and watches each tile say "Connecting..." and then come online; the boxes with M3c | What a real wall does otherwise at such a start is written into `LocalWall` and the made-up KVMs. Each of those that are left is mended with a test seen to fail first, or stays said here as what it is |
| The two boxes for this PC's wall as a file take only what a person ticked after it was shown, and nothing that takes a press comes to stand where something else stood a moment before (`OwnFilesView`, `ui/file_door`, `OwnExportBox.qml`, `OwnImportBox.qml`; section 5, "This PC's wall as a file"). Built in one round and mended in four, each read by a reviewer who had not seen the code: five readings, and none found anything high. Three readings running found a medium of one kind, a live "Take" coming to stand where another control had stood a moment before; the boxes holding still ended it (one size and place, the buttons in fixed places, a moment before anything takes a press, held in C++ by `settled` and `mayTake`), which `noButtonOfEitherBoxComesToStandWhereAnotherStood`, `noTickNoteTakingOrEndOfItsTimeComesToStandWhereAnotherStood` and `aWindowThatChangesSizeHoldsWhatTakesAndWhatSavesForTheirMoment` hold. Tried by tests only (`ui/tests/test_own_files_view.cpp`, `test_own_files_screens.cpp`). Open: the chosen file is read on the interface thread (up to 21 MB, as the server's box does); "Cancel" does not end work that is under way (it runs out unheard, and another file meanwhile is told "busy"); the wheel and dragging move the list without a moment (the hand on the pointer moves it while the person watches); a change of the screen's scale, a remote session's resize, a move to another monitor, touch and a screen reader were not tried; what Windows' Save box hands back for a name typed with a dot or a blank at its end is shown by no test (`asWindowsTakesIt`); nothing of either box was tried by a person or with Windows' real boxes; the Import box is large and mostly empty before a file is chosen; the count does not say how many devices the file has in all (the reader does not hand that number over) | M3c, by a person who writes this PC's wall to a file and takes it in again, through Windows' own boxes | Each is mended with a test seen to fail first, or stays said here as what it is |
| A sealed file of this PC's wall leaves none of its passwords in memory. It does: they were never wiped. Each is read from the store and what was read is written over (`keptPasswordOf`), but it is handed on as a text of the file's (`exchange::Device::password`, by `LocalWall::asAFile`), which is copied into what the writer is given and kept for a second writing where the first was too large (`WallExchange::write`); none of those copies is written over. `exchange.h` and `wall_exchange.h` say that what is handed in is the caller's, and a text of Qt's cannot be wiped where it is shared | M3c | The passwords go to the writer in memory that is written over when it goes, as the passwords of a file that is taken in do (`SecretBytes` in `WallExchange`), and what was handed over is let go once the file is written |
| The settings box looks and works in a real window as it does in the tests. It was seen only in the tests' pictures, which Qt draws in software on its platform without a screen: never in a real window, at another scale of the screen, or with a screen reader; its tooltip and where the keyboard goes after a removal and after a box closes were seen only there. Nobody has used it (section 5, "The settings") | M3a, by a person who opens it in the program, at two scales, and with Narrator | What is found is mended with a test seen to fail first, or stays said here as what it is |
| The tests pass on a slow machine as on a fast one. They do not. Under the address sanitizer, on a slow core, three programs that speak TLS to made-up KVMs on the test's own thread (`test_core_kvm_editing`, `test_core_wall_exchange`, `test_ui_own_wall`) fail with "unreachable", with a question missing, or with a sign-in before its time, because three lines are not let in within the second and a half their tests give one. Found on 2026-10-13 on a PC with fast and slow cores, where Windows had put the tests on the slow ones while somebody worked at the PC: three builds of different hours failed so, among them one of the code as it was before that day's work; held to the fast cores the same program passed four times of four, held to the slow ones it failed. Since then the tests are run on the fast cores (`Use-FastCores`; section 8). That hides it on such a PC and mends nothing: a machine that is slow throughout, a hosted runner say, will show the same | before the tests are run anywhere but on this PC (M4) | The made-up KVMs get a thread of their own, or the tests' times come from one place that a slow build makes longer; until then a failure under the sanitizer alone, in a test that speaks TLS, is run again on a fast core before it is believed |
| Qt 6.12 gets fixes for long enough | over time | Stay on the last 6.12 that is free to use; write down what was hit |


## Native kvmd video integration, 2026-10-20

The owner selected the C++ receiver after the dependency trials. `KvmdLive` now owns one
`KvmdWebRtc` attempt per device and shares its pictures among viewers. `KvmdDevice` opens
`/janus/ws` with the same authentication and certificate rules as its other device connections.
`DeviceLine` opts into bounded text reception for this line, requiring `janus-protocol`.
`KvmdWebRtc` owns Janus session/handle negotiation and keepalive/media deadlines.
`WebRtcReceiver` wraps the pinned libdatachannel C API, marshals callbacks to Qt through a
bounded mailbox, and feeds complete Annex B access units assembled by `H264Rtp` into the
existing `LivePicture` and FFmpeg/D3D11 engine. Native media does not acquire keyboard control.
Since M6 increment 2 the mailbox's pictures are a `PictureBox` (`picture_box.h`), shared with the
offering side a JetKVM needs: each picture is stamped on this PC's steady clock as it becomes whole,
on the library's thread (`Live::Picture::arrivedUs`, what the engine counts as its arrival), and the
box holds as many as the engine lets wait, eight, so that a stall of the interface thread of up to
about 130 ms with the engine's queue empty loses nothing (it held two, and a stall of 33 ms at 60 a
second froze the view until the next key frame). A ninth keeps the run from the newest key frame
held, where that run is eight or fewer; otherwise all go, and a key frame is asked for, twice a
second at most for a Comet. Every picture the box lets go is counted (`letGoBehind`), recorded in
`video.jsonl` from the drain (`rtc_let_go`, no lock held) and said to whoever shows the machine
(`Live::picturesLetGo`), where Video details counts it as dropped behind with the engine's own. A
window that asks for a key frame (one that joins a shared connection does) throws nothing away any
more: it froze every window of the machine until the next key frame. A wish the pace refuses is not
forgotten: it is asked once the pace has passed, at the next packet or by a timer at the pace's end
where none comes (a screen that stands still), unless a key frame came first; what was let go is
said to each window from its own start, never less than before, also across the lines of one
machine (2c, the second reading);
and so is one while the box waits for the next key frame (2b, after its reading: a window opened
within half a second of another asking stayed blank until the device's own next key frame). The lock discipline (`rtc_seam.h`: `Held`, `intoLibrary`, the registry by the
library's id) and the reading of a session description (`rtc_sdp`) are the receiver's and the
offering side's alike; `tools/test_review_guards.ps1` holds the files that speak to the library to
it, and every other file of the program off the library. The offering side is `WebRtcOffer` (M6 2e,
`webrtc_offer.h`), beside the receiver and not a second way into it: it offers a video it only
receives in the H.264 formats handed in (no RTX), opens the data channels handed in itself (reliable,
in order, each text or binary with its largest message), takes an answer only of a format it
offered with one application, each under the mid it was offered under, takes the far end's addresses
as `candidate:` texts alone (2048 bytes, 40 at most; their mid is the library's), those inside the
answer taken out of it and judged and counted in the same way, deletes what channels the far end
opens, drops and counts a message of the wrong kind or too large, ends where more waits than it takes,
and reads the stop word handed in on the library's thread before any lock, raising a flag whoever
sends reads first. An address is taken only where it is UDP to a host by its number (no name is ever
looked up: the library would look one up on a thread of its own, at a name the device chose) and to one
that may be reached (not unspecified, multicast, the broadcast, or this PC's loopback unless the
device's signalling address is loopback), each refusal counted by its reason (2e's reading: a device
that answered with some 1500 addresses by name could have had the PC start as many threads and
lookups, by the reviewer's reading of the library, and ones by number could have had it send to its own
services). The library is handed an address made anew of what was judged (its foundation, component,
UDP, priority, the host as the number judged, port and type), never the device's text, which is taken
only of visible ASCII with one space between its words, a foundation of ICE's signs, numbers within
their bounds and a type ICE names (2e, the second reading: the library splits a candidate at whitespace
of any kind, so that a tab could have had it read another host than the one judged); 0.0.0.0/8, and IPv4
written the old way inside ::/96, are refused too; and a description this program and the library would
read two ways (a line that begins with whitespace, a section that says its mid twice) is no answer.
The most a message may be is one over the largest a
channel takes (the library cuts a larger one down to it, which at 64 KiB made a megabyte look like
64 KiB that were whole). It is begun once; once it ended, nothing more is taken, and that is said
once (2e's reading). It knows
nothing of a JetKVM: its tests run against a libdatachannel peer that plays one on 127.0.0.1
(`core/tests/jetkvm_peer_by_hand.h`), sending `jetkvm-1080p60.h264` (made by
`tools/make_testdata.ps1`: the desktop, 1080p60, a key frame every 30 pictures).

One session to a JetKVM is `JetKvmConnection` (M6 2f, `jetkvm_connection.h`), on that offering side and on
the driver's one road to its signalling line (`JetKvmDevice::openSignalling`, which what it is asks over
too: its session, signed in to once more where the line refuses one somebody has since replaced, and the
line handed over from inside its "you are in", before the JetKVM's next word is read). It says the offer as
the JetKVM's own page does (`{"type":"offer","data":{"sd": base64 of JSON}}`, the two H.264 formats,
`rpc` text of 64 KiB and `hidrpc` binary of 64 bytes); takes the answer within 15 s (or
`timeout`), an `{"error": ...}` in its place or one that cannot be read as `bad_response`; hands the
addresses that follow to the offering side, their mid let go, those that came before the answer after it;
and is not ended by its line falling once answered. It is up at `rpc`'s opening, within 15 s of its start
(or `timeout`, "The JetKVM is taking too long to connect"). JSON-RPC on `rpc` has numbers of its own from 1,
answered in any order and only under a whole number that waits, 8 s each (`timeout`), an `error` is
`action_failed` in its words made plain. Of the events it hears `otherSessionConnected` (once: taken by
another viewer, nothing more of ours goes to it, and its end is `in_use`), `videoInputState` (clamped; the
moment its input came back is the word's stamp), `usbState`, `atxState` and `localVersion`. Watched every
second, it ends `dropped` where `rpc` closes under it, where no whole picture came for 5 s while it does not
say "no signal" (after its first: a rested JetKVM takes seconds), where none came within 20 s after its
input came back (a picture counts as after that only 100 ms or more after the word), or where the session
fails. Every number is the web's (`jetkvm.py:90-111`) and a test's `Pace`. Its tests run against a JetKVM
by hand (`core/tests/jetkvm_by_hand.h`, its signalling grown) and its one viewer
(`core/tests/jetkvm_viewer_by_hand.h`), which answers each offer with a peer of its own and hands the one
before over as the real one does.

A Comet's Janus is held to the same rule for its addresses (since 2e's second reading): its offer goes to the
library with its addresses taken out of it, and those, and the ones it trickles, go through `judgeAddress`
(`WebRtcReceiver::candidate`), each handed on made anew and with the bundle's mid (Janus's is let go), 64 at
the most (only those taken count toward it), this PC's loopback only where the KVM itself is spoken to there
(`spokenToOnThisPc`, the JetKVM's test too). One refused is counted by its reason (`WebRtcReceiver::Refused`)
and let go, and the session goes on with those taken. Before, each went to the library as it came, names
among them (the library looks a name up on a thread of its own), and the 65th trickled, or one the library
could not read, ended the session.

A JetKVM's one viewer is `JetKvmViewer` (M6 2g, `jetkvm_viewer.h`), in its driver: the one connection for its
cards and every window that shows it (`JetKvmLive`, the wall's `OwnLive` for it, as `KvmdLive` without its
JPEG half). A card that asks is somebody watching and connects; it is answered `in_use` while the program
stands aside, the newest key frame where it is at most 5 s old and arrived 100 ms or more after the JetKVM
said its input came back, with no signal the newest key frame said as such, and otherwise "Connecting to the
JetKVM" or "Waiting for the JetKVM's video". The card's stream of an own JetKVM decodes that key frame as a
still (`engine::Codec::KeyFrameStill`, `cardCodecOf`). While no window shows it, its connection's box keeps
key frames only; the first window has every picture from the next key frame after it came. With no window
and no card asking for five minutes (UNWATCHED, looked at every ten seconds) the connection is given back; a
look never counts. A look while a connection is open asks over it (the video's state, USB, the extension,
the version, and the power board where its extension is on); with none, or where the connection went
meanwhile, its status the plain way, `busy` while aside, and what the last connection showed (no signal)
until another shows otherwise. One connection at a time for the KVM, across its drivers: what outlives a
driver is `JetKvmKept` (`jetkvm_kept.h`), in the wall's record of the KVM (`LocalWall::Held::jetkvm`), handed
to every driver it has; a new connection begins only once the newest is gone, waited for 8 s at most, and
otherwise the asking fails as `not_connected` ("The JetKVM is taking too long to connect"), which was not
sent. Another viewer's word over the newest connection (or over an older one as `JetKvmKept::judge` says) sets the KVM aside, ends the connection and tells each window `closed("in_use")` once,
which ends a line for good; while aside nothing connects, and only `act("resume")` ("Show here") takes it
back. Keyboard and mouse are refused `not_yet` (increment 3). Tested in `core/tests/test_jetkvm_live.cpp`
against the JetKVM and its viewer by hand; and against another implementation of a JetKVM, the web app's own
fake (aiortc, the web app's `tests/fake_jetkvm.py`, on 127.0.0.1 only), by `tools/test_jetkvm_joint.py` with
`qkvm-jetkvm-rig` (`bench/jetkvm_rig.cpp`): what it is, how it is, its cards' key frames, its picture live, how
it is over its connection, stepping aside at a hand-over and "Show here", in a Python with the web's test
requirements, outside `tools/check.ps1`.

JPEG remains available during negotiation. A first key frame selects H.264; failure returns
to JPEG and schedules a fresh receiver with a capped backoff. Missing dependencies in a frame
cause a key-frame request and dependent frames are discarded. No decoded output in a viewer
also selects fallback. Physical-device acceptance and performance work remain open. Earlier descriptions of H.264
as a future transport are historical.
