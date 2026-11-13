#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <qkvm/input/keys.h>

#include "key_source.h"

class QWindow;

namespace qkvm::ui {

// Windows' own way of hearing every key of this PC's keyboard: a low-level keyboard hook
// (WH_KEYBOARD_LL; DESIGN.md section 3.8) on a thread of its own with a message loop. It is put
// in by begin() and taken out by end() and when this goes, and is never there otherwise.
//
// TO BE TRIED BY HAND, BY ITS OWNER. No test installs it and no rig is run for it, since a
// program that tried it would hold the keyboard of the PC it runs on: it has run where its owner
// tried it, on their own PC, and nowhere else. What it does with a key is `hook::Procedure`
// below, on plain values, and that is what the tests try.
//
// It sends this PC nothing: no key, no click, no input of any kind, ever. So nothing it does
// keeps Windows from locking the PC, from its screen saver, or from sleep. (It did once send
// itself a key four times a second to see whether it still heard: that is gone, and with it what
// came of two windows eating each other's such key, and of Windows refusing to take it.)
//
// What keeps it from ever taking the keyboard away from whoever sits at the PC:
// * Its procedure takes no memory, waits for no lock and logs nothing. It writes the key into a
//   ring of fixed size, wakes the interface thread, and is done.
// * A key is kept from Windows only while all of these hold: swallowing was asked for; the
//   interface thread has said within the last second that it is alive; the key came while
//   Windows still waited for the procedure's word, with a margin; and there is room to tell of
//   it. A program that hangs gives the keyboard back within a second, by itself.
// * And only while the window the hook was put in for is the one Windows has in front, as
//   Windows says on the hook's own thread with every key that goes down (`Procedure::isFor`).
//   Where another is, that key is the other window's, and from it on nothing is swallowed,
//   whatever the interface thread asked for: it hears of the other window when Qt tells it,
//   and that word waits while it is at other work. What was kept from Windows still goes up on
//   the machine. (The swallowing was ended by Qt's word alone: until it came, or until the
//   interface thread was silent for a second, the keys typed into another program's window
//   were kept from it.)
// * The leave-key (right Ctrl), pressed and let go with no other key going down between,
//   switches the swallowing off in the procedure itself, before the interface thread hears of
//   the key. (A key that is held and only said again is no key going down: a second keyboard
//   goes on saying its key while the leave-key is pressed on the first.)
// * What another program types is passed on and told to nobody: it is not sent to a machine.
//   That such a key came is marked, once for each time keys are swallowed, and nothing of the
//   key is: whoever sits at the far end of a remote-control program is told that what they type
//   reaches no machine.
// * A key that Windows saw go down, Windows sees go up: nothing stays held on this PC because
//   it was let go while a machine was controlled. And such a key's repeats are not told as
//   presses of the machine's.
// * A key is the key it went down as for as long as it is down (`KeysHeld`): said again, and
//   going up, it is told, and passed to Windows or kept from it, by the number, the mark and the
//   name it went down with. Windows names some keys by what else is held, and a key that went
//   down as the one and up as the other would stay down on the machine, or on this PC. Known to
//   me, each from memory: Num Lock is "Pause" under Ctrl (fairly sure; with the same number and
//   mark, as I believe, and then it is sent as the Num Lock it is); Pause is "Break" under Ctrl, with
//   another number and a mark (sure); Print Screen has another number under Alt (sure); the
//   number block's keys are named as the arrow block's under Shift with Num Lock on, with the
//   same number (sure) and, as that number never has the mark, without it (fairly sure). The
//   rule does not go by that list: whatever is told for a place of the table that is down is
//   that key, and a key that is told under another number is known by its USB number where it
//   has the same one.
// * A key whose "up" does not come as that key's does not stay down in the table, or on the
//   machine, wherever that can be known for sure. Two things are known, and no more:
//   - Num Lock may be told at two places of the table with two USB numbers (with its mark as
//     Num Lock, without it as "Pause" under Ctrl: not known whether Windows does, see
//     `input::usageOf`; and it names keys by the Ctrl it has itself, which a Ctrl that is kept
//     from it is not, so it may never do so while keys are swallowed: not sure of that either).
//     While one of the two places is down, what is told at the other is that key: said again,
//     and let go, as what it went down as. No other pair of places is made one so. The number
//     of every other key belongs, with the mark and without, to two keys (Enter and the number
//     block's, the two Ctrl, the arrow block and the number block, a letter and a key for
//     sound) or to a key and to what a keyboard makes up (Shift): an "up" there may be the
//     other key's, whose "down" this hook did not hear (Windows hands a hook that was slow no
//     key at all), and to let go a key that a finger holds is the worse error.
//   - Where the two accounts differ about an "up" that was told of and not heard (or was heard
//     under a USB number that was not told of), the control ends within the moment, as ever;
//     and a key that had been kept from Windows is up by the word of the account that told of
//     it, and is told as gone up (`takeWhatWasToldToGoUp`). At whichever time: also in the
//     first moment after that account began to tell on a PC where it had told of nothing.
//     An "up" that was heard and is never told of ends nothing: the key was let go by that
//     hearing, and what was noted of the hearing is dropped (`Procedure::look`).
//   (For a while there was a third: what the other account had as up, by an account of its
//   own of what is down, was let go by that word. Nothing came to it but a telling of that
//   first moment which had been forgiven, and none is forgiven any more: it is gone.)
//   With the "up" after which the table holds no key at all, that is said
//   (`Heard::nothingIsDown`): every key was told as gone up by then.
//   Not covered, and not to be had from here: on a PC that gives no second account, an "up"
//   that the hook is not called for, or that comes at another place than its "down" and is not
//   Num Lock's. That key stays down on the machine until it is pressed and let go again, or
//   control ends. The bar says on such a PC that keys which Windows stops passing on are not
//   noticed.
// * Two keyboards (a second one, a macro pad, a scanner, a laptop's own beside one that is
//   plugged in) are not told apart. A hook is not told which keyboard a key came from, and
//   the one rule is: a key is let go, in the table and on the machine, when any keyboard lets
//   go of it. (Raw Input does name the keyboard with every key, and its account was kept for
//   each keyboard for a while, so that a key held on the one was not let go by a tap of it on
//   the other. Each attempt at that made a worse rare case than the one it mended: a modifier
//   that stayed down on the machine with nobody told, where a keyboard dropped out and
//   Windows did not say so; or a hold that gave way under a finger. It is gone, by the
//   owner's decision: nothing that names a keyboard is asked for or kept.)
//   What the rule costs, and it is the side to fail on. With two keyboards in use at once, a
//   modifier that is held on the one is let go by the same modifier's "up" on the other, and
//   the next key from the first arrives bare: whoever sees it presses the key again. (Where
//   the first keyboard goes on saying the key, it goes down again with its next saying, as a
//   press.) Only a person at two keyboards can see it. One thing Raw Input says of where a
//   key came from is used: what came from no device was typed by a program, and is not
//   brought here (below).
// * A keyboard that goes, with a key held: unplugged, taken to another computer by a desk
//   switch, its link lost. No "up" comes for that key, from either account (so Windows is
//   taken to do: fairly sure; where one does come, the key goes up by it). It stayed down on
//   the machine, Enter or Del said again there by the machine itself, with nobody told, until
//   the key was pressed and let go on another keyboard, which whoever has one keyboard has
//   not. So Windows is asked to say when a keyboard is taken away (with the raw keyboard:
//   RIDEV_DEVNOTIFY, WM_INPUT_DEVICE_CHANGE), and on that word the swallowing ends, where
//   keys are swallowed (`Procedure::aKeyboardWent`, `Lost::AKeyboardWent`): whoever listens
//   lets go of everything on the machine, and says why. No keyboard is told apart for it and
//   nothing of one is kept: any keyboard that goes is enough, held key or none. What the
//   table has as down stays as it is, since nobody knows which of it was held there: whoever
//   listens keeps such a key back at the next taking and names it, until it is let go or
//   pressed anew (MachineControl). Thought through, with how sure each is, from memory:
//   - A keyboard that comes ends nothing; nor does a mouse or another device that is no
//     keyboard, of which Windows does not tell whoever asked for the keyboard only (fairly
//     sure), and which is told from a keyboard where Windows still says what it was
//     (`meansAKeyboardWent`; of a device that is gone it may not say: then it is taken for
//     a keyboard).
//   - A device that is both: a receiver for keyboard and mouse, a mouse with keys of its
//     own, a headset or a dock that Windows lists as a keyboard too. It goes as a keyboard,
//     and the control ends: said so in the bar's sentence, which is of what Windows said.
//   - A hub or a dock that takes every device away and brings it back within the second: the
//     first keyboard that goes ends the control, the others find nothing swallowed, and
//     those that come back end nothing. Keyboard and mouse are taken again by hand.
//   - Over a remote desktop: not known whether such a word comes at all. The session going
//     or coming ends the control by itself (below).
//   - A keyboard without a wire that sleeps or goes out of reach: behind a receiver that
//     stays plugged in, Windows says nothing (fairly sure), and it is as it was. On
//     Bluetooth, not known. The same of a desk switch that keeps a made-up keyboard at each
//     computer and cuts nothing off. Where such a switch or receiver does not let the keys
//     go itself, the key stays down on the machine until keyboard and mouse are given back,
//     or the key is tapped on a keyboard that is there: on the list for a person at one.
//   - Where Windows will not give that word (the asking is refused, or there is no second
//     account to ask it with), whoever asks is told so (`isToldOfKeyboardsGoing`), and the
//     bar says while keyboard and mouse are held that a keyboard that goes is not noticed.
//   Set, and not shown by a test: the asking and the reading of that word, which are
//   `WindowsKeyboard`'s. The plain part is tried with made-up notices.
// * What Windows cannot name (it tells of it as 0xFF) is no key, going down or going up: it
//   changes nothing in the table, lets nothing go, and is nothing to a machine. Assumed of
//   Windows, from memory: the Shift a keyboard makes up around Home, End, the arrows, Insert
//   and Del under Shift (and around some keys with Num Lock on) is either not handed to a hook
//   at all, or with a number no key has (0x22A, 0x236: so another program's source has it, as I
//   remember it), or with this name. Fairly sure that it is one of the three, not sure which.
//   Each is no key here. Were it handed to the hook as Shift's own going up, by Shift's name
//   and at its place, the machine would be told Shift up before Home: only a person at a
//   keyboard can show that it is not.
// * When Windows shows another desktop (Ctrl+Alt+Del, the lock screen, a question about
//   administrator rights), or the session is locked or taken over from afar, the keys that are
//   let go there are heard by nobody here: the swallowing ends, and whoever listens is told to
//   let go of everything (`lostHearing`). The same, with a word of its own, the moment the keys
//   are seen that Windows itself acts on: L under a Windows key, Del under Ctrl and Alt.
// * Once the swallowing has ended, the hook stays in only for the keys it kept from Windows to
//   be let go, tells of nothing else, and swallows nothing: that is so in the procedure itself.
//   After a minute its own thread takes it out, whatever has become of the interface thread.
//   And where the swallowing ended because keys were not heard, or because the interface
//   thread said nothing for a second, this hook swallows nothing ever again, whoever asks: the
//   next taking puts a new one in.
// * An interface thread that hangs. From the first key that finds it silent for a second, the
//   swallowing is over for good and marked as fallen behind, and no key is put on its way to
//   the interface thread any more but the going up of one that had been kept from Windows.
//   (Which keys are down, and which of them Windows has, is still followed in the fixed tables
//   for as long as the hook is in: `KeysHeld`, `WindowsKeys`. The next taking asks them.)
//   Silent for ten seconds, and the hook's own thread takes the hook out: no key of the PC
//   passes through the program while it hangs for longer than that.
// * A hook thread that hangs. Windows frees the keyboard by itself then, after the time it
//   waits for a hook. The thread says when it last looked (a stamp, at least ten times a
//   second), and the interface thread, while keys are swallowed, takes a stamp that has stood
//   still for three seconds for a thread that no longer looks: whoever listens is told, and
//   lets go of everything (`LookWatch`).
//
// Whether the hook still hears: Windows' other account of the keyboard. Windows takes a hook out
// without a word when its procedure was slow, and has no way of saying whether one is still in.
// So the hook's thread also asks for the raw keyboard (Raw Input, with RIDEV_INPUTSINK: told of
// every key whichever program is in front), which Windows tells of every key that a real
// keyboard sends, whether a hook is called for it or not, and which sends nothing. It is the
// keyboard's own account: it does not see what a remapping of keys, Windows' own key helpers
// (Sticky Keys, Filter Keys, Mouse Keys) or another program's hook make of a key before this
// hook is called for it, or instead. The two are held against each other, key by key
// (`TwoAccounts`):
// * A key that was told of and is not heard by the procedure within the moment: the swallowing
//   ends, and whoever listens is told (`lostHearing`, NotHandedKeys). That finds a hook that
//   Windows dropped at the first key after it, and a single key that Windows left out, an "up"
//   among them; and it is also what a key looks like that another program's hook, put in after
//   this one, kept to itself, or that one of Windows' key helpers held back. Which of these it
//   was is not known here, and is not claimed. And while no hook hears, the other account
//   tells of the going up of the keys that had been kept from Windows.
// * The other way round: a key that goes down is heard, and the other account, which has told
//   of keys of this keyboard, does not tell of it within the moment. Something makes or changes
//   keys on this PC: the swallowing ends with a word of its own (NotToldOfKeys). And where a
//   key that was heard about then is not told of while one that was told of is not heard, the
//   two told differently of what may be one key (ToldDifferently). Ending is right in each:
//   what the two say cannot be reconciled from here.
//   Set, and not shown by a test: that on a PC where nothing makes or changes keys, Raw Input
//   tells of every key which the hook is called for and which has a USB number, each saying of
//   a held key among them. As I remember it, fairly sure. Where it does not, the control ends
//   at that key, with that word: seen at once by whoever tries every key of a keyboard. (Where
//   it is a saying of a held key that it does not tell of, nothing ends by that; but the same
//   key let go and pressed anew within the moment of the last such saying waits for a word
//   that is taken for the saying's, and the control ends there. Within that moment, and no
//   longer: what was noted of such sayings a moment ago and more is dropped as the press is
//   heard (`TwoAccounts::pressedAnew`). Seen by whoever holds a key until it says itself
//   again, lets go, and taps it again at once.)
// * While the two are not at one, no key that goes down is handed on. One that the procedure
//   heard waits for the other account's word for it, which says that no key before it was left
//   out; and none is handed on while a key that was told of waits to be heard. What waits goes
//   out in its order when the two are at one again, and never where they are not within the
//   moment: the control ends then, and a "c" whose Ctrl Windows left out went to no machine.
//   A word is for the hearing it belongs to: that account tells in the order the keys came in,
//   and its word for a saying of the key that was heard before a press (the key was held, said
//   again, let go and pressed anew, with this thread a little behind) is that saying's, and
//   lets the press wait on for its own (`KeysWaiting::tell`).
// * A key that goes up waits for nobody's word where its going down has been handed on: it is
//   handed on at once, past whatever waits. Letting go on the machine what is down there is
//   always right, and a key held there for the moment longer is said again by the machine
//   itself (Enter, Del, Backspace). Two things it never passes: its own going down, where that
//   still waits, or the key would stay down on the machine; and, where it is one of the eight
//   modifiers, any key going down that waits, which was pressed under it and would reach the
//   machine bare. (A modifier that is held a moment longer on the machine types nothing there:
//   the machine does not say a modifier again. Windows does say a held modifier again, as any
//   key that is held.) It waits for nothing else, and for nothing that has to do with whose
//   it was. And the same key going down once more while its "up" waits so is a press like any
//   other: it takes the place behind that "up", and goes when its own word has come.
// * A key that is down and is only said again waits for nobody's word and takes no room to wait
//   in: it is handed on at once, and not at all while its own going down still waits (it adds
//   nothing to that). The two accounts are still held against each other saying by saying, so
//   that a hook which Windows took out under a held key is found by the next saying; but
//   however many sayings of a held key one account is ahead of the other, nothing waits for
//   them and nothing ends by their number (`TwoAccounts` counts those it has no room to note
//   the time of).
// * The moment is the time Windows waits for a hook, and a tenth of a second. A hearing that
//   comes later than Windows waits is none: the key is Windows' by then.
// * Which of the two comes first. Assumed: the hook hears first. As far as I know (from memory,
//   fairly sure, not certain): the procedure is called by a message that is sent to its thread,
//   and Raw Input's word is posted to it, and a thread is handed what was sent to it before
//   what was posted; so the procedure hears first wherever both wait for the thread, as they
//   do after it was slow. By how many keys it can be ahead is not known: Windows waits for the
//   procedure's word on one key before it goes on to the next, so I take it to be one or two
//   and not many (fairly sure; an earlier comment here said several, a reviewer said never
//   four). Nothing depends on it. Both orders are handled all the same, key by key and many at
//   a time: whichever comes first waits for the other, and nothing but for how long a key waits
//   depends on the order. The tests try both, with a fast pair of keys, with a key that is
//   said again thirty times a second, and with one that is said ten times before the other
//   account's first word.
// * What the other account will not tell of waits for no telling and takes none: a key that
//   has no USB number, the left Ctrl that Windows makes up under AltGr, and what a program
//   typed. Were the hearing of such a key noted, it could take the telling that belongs to a
//   real press which the hook was not called for.
// * Where this PC gives no second account (it was refused; a remote desktop where none comes),
//   nothing waits for one for longer than the first key's moment, the watch is silent, and
//   whoever asks is told so (`isWatched`): a hook that Windows took out is not found there.
//   That is taken to be so only while the other account has told of no key since the hook
//   went in. And there no hearing is noted at all: a hearing is noted to be held against the
//   other account's word for it, and where there is none to hold it against, nothing of a key
//   is kept but that it is down. (Where there is one, a hearing is noted until that word has
//   come, and for two of the gate's moments at the most where it does not.) Where that account
//   begins to tell later (a keyboard that it tells of is plugged in), hearings are noted from
//   its first telling on. A telling that comes within the gate's moment of the first and
//   finds no hearing may be the word for a key that was heard before any was noted, or for
//   one that went down since with the hook not called for it: nothing was noted to tell the
//   two apart by. So it is held against the hook as any other, the control ends, and the
//   word for it says that much and no more (`Lost::BeganToTell`). What that costs: on a PC
//   that begins to tell while a machine is controlled, with a key in flight at that very
//   moment, keyboard and mouse are given back once, and are taken again. (For a round such a
//   telling was forgiven. So was a Ctrl that went down unheard in that moment: the "c" after
//   it went to the machine bare, with nothing said. To forgive only what was heard before
//   is not to be had: of that, on such a PC, nothing is noted.)
// * A key that has no USB number of the keyboard's page (a key for sound) is not compared at
//   all, and nothing of its hearing is noted: Volume up has the keyboard's number of B with
//   the mark, and is not B. No key is read with its mark turned.
//
// What was thought through, and what of it only a person at a keyboard can show:
// * The same key under another name. They are compared by the keyboard's own number for the key
//   and its marks, read as `input::usageOf` reads them, never by Windows' name: Raw Input says
//   "Shift" where the hook says "left Shift". Three keys are told of with other numbers or marks
//   by the two, and are made one here (`fromRawInput`, `pressedOf`): Pause (two numbers with a
//   mark of its own there, one here), Num Lock and the right Shift (marked here, not there).
//   Num Lock is that key whatever the two call it: both say "Pause" of it under Ctrl, as I
//   believe, and there the mark of its own tells the Pause key from it, here the mark.
//   Set, and not shown by a test: that Windows tells of them so. It is as I remember it, and I
//   am sure of Pause, fairly sure of Num Lock, less sure of the right Shift. A key that the two
//   tell of in ways not foreseen here ends the control when it is pressed, which is the safe
//   side, and is seen at once by whoever tries every key of a keyboard. Not known, and to be
//   tried so: the keys that switch the writing on Japanese and Korean keyboards. And Print
//   Screen is to be tried with them: a window is told of it only as it goes up, by its key
//   message (sure), which an earlier comment here took for what a hook is told. A hook is
//   told of its going down and its going up as of any key's, and so is Raw Input (so I
//   remember it, fairly sure).
// * Keys another program types come to the hook marked, and are told to nobody. Raw Input, as I
//   remember it, tells of them with no device (RAWINPUTHEADER::hDevice is null) or not at all:
//   what has no device is not compared, and neither is the hearing of a key that came marked
//   as typed. Fairly sure of the null device, not certain. Were Raw Input to tell of a typed
//   key with a device after all, that telling would find no hearing and end the control, with
//   the word that Windows told of a key which was not heard: the safe side, and seen at once by
//   whoever tries it. (The same of the left Ctrl under AltGr, were Raw Input to tell of it.)
// * The Shift that Windows makes up around a key of the number block that is pressed under
//   Shift with Num Lock on. Not known how the hook is told of it: with a number no key has or
//   with no name, as I believe (then it is no key here, and nothing of it goes anywhere), or as
//   the Shift key's own going up and down (then the machine is told the same, and the other
//   account does not tell of it: the control ends, as two accounts that differ). To be tried.
//   What Raw Input tells of a made-up Shift comes without a name (so I remember it, sure), and
//   is no key there: its own account of what is down never has Shift as up for it.
// * Over a remote desktop there may be no Raw Input at all, or none with a device. That is
//   from memory, and I am not sure of it either way: where none comes, the first key that is
//   swallowed waits its moment for a word that does not come, and from then on this says
//   nothing, ends nothing, and is known as not watching.
// * On one of Windows' own screens (the lock screen, a question about administrator rights) I
//   expect neither account to hear: what was held then is kept back at the next taking until
//   it is heard to go up or seen to go down anew (MachineControl).
// * A PC that is merely busy. The hook's thread once looked at the clock ten times a second and
//   took a gap of 300 ms for keys that had passed it by; a waiting thread has such gaps (a
//   build, a scanner that suspends the program, memory paged out), and a gap in which no key
//   came harmed nobody. That beat ends nothing any more. What matters is a key: one the hook
//   never heard is found by the other account, and one it heard only after Windows had stopped
//   waiting for it is known by its own time stamp, is Windows' and not the machine's, and ends
//   the control as keys that fell behind. A real hang (the program stopped, the PC asleep)
//   shows as exactly such keys, when keys come; and as a stamp that stands still, when none do.
// * How long Windows waits for a hook: 300 ms, unless this PC's registry says otherwise
//   (LowLevelHooksTimeout, under the person's own desktop settings; read once, and never
//   written). A smaller number is believed. A larger one is not, and neither is none, nothing
//   that is a number, or 0: each is the 300 ms. Windows may read that number only as somebody
//   signs in (so I believe, not sure), and to believe a longer wait than Windows keeps is what
//   hurts: a key that Windows took for itself meanwhile would count as in time here, be typed
//   into both, and its "up" be kept from Windows. To believe a shorter one only ends the
//   control sooner. So does the gate's moment stay under what the interface thread takes for
//   too late, whatever the registry says (`lateAfterMs`). Not covered: a smaller number that
//   Windows read as somebody signed in and that was taken out or raised since.
// * One window per kind of device is told of the raw keyboard in a program: the one whose hook
//   asked last, which is the window that took keyboard and mouse last. An older hook, which is
//   in only until what it kept from Windows was let go, loses that account, is told so on its
//   own thread within a look, and compares nothing from then on; taken again, its window
//   begins anew. What such a hook cannot hear (the newer one keeps from it what it swallows) is
//   heard by the newer window's keys, and what is held is one memory for all the windows
//   (KeysKept).
//
// Two windows that are never shown, both the hook thread's own. One is a message-only window,
// which gets no broadcast: Raw Input is told to it. The other is a real window: Windows tells it
// of the session (WTSRegisterSessionNotification). Whether that works with a message-only window
// I do not know for sure, and a lock that is missed is worse than a broadcast that is heard.
//
// begin() and end() wait for the hook's thread for a bounded time. Where it is over, control is
// not taken, or the thread is left to end by itself: what it works on is its own (`Run` in the
// .cpp), outlives this, and swallows nothing from that moment on. Which keys were down, and
// when each was heard, is forgotten by that thread itself when it has ended (`LastToGo`).
//
// On the interface thread, the procedure excepted. `window`: the one whose machine the keys are
// for, which outlives what this makes. A hook is put in for that window by the handle Windows
// knows it by at begin(), and is gone on with only while that is so: a window whose own native
// window was made anew is another to Windows, and gets a hook of its own (the next of the
// program's hooks, by number). No window that Windows knows, no listening.
std::unique_ptr<KeySource> windowsKeyboard(QWindow& window);

// What the hook does with a key, apart from Windows: plain values in, plain values out.
namespace hook {

// How Windows marks a key it tells a listener of (KBDLLHOOKSTRUCT::flags): one of the keys with
// the mark (input::Pressed::extended), and one that no finger pressed but a program typed, of
// whichever standing that program is.
inline constexpr std::uint32_t markedFlag = 0x01;
inline constexpr std::uint32_t typedByAProgramFlags = 0x10 | 0x02;
// And how Raw Input marks one (RAWKEYBOARD::Flags): it goes up; it has the mark (E0); it is the
// first half of Pause (E1).
inline constexpr std::uint32_t rawUpFlag = 0x01;
inline constexpr std::uint32_t rawMarkedFlag = 0x02;
inline constexpr std::uint32_t rawPauseFlag = 0x04;

// For how long the interface thread's word that it is alive holds.
inline constexpr std::uint64_t aliveForMs = 1000;
// After how long without that word, while keys were to be swallowed, the hook's own thread
// takes the hook out: longer than a program that only stutters is silent (Windows itself calls a
// window that does not answer for five seconds hung), and no longer than whoever sits at the PC
// is to have their keys pass through a program that hangs.
inline constexpr std::uint64_t hungAfterMs = 10'000;
// A key that waited this long to be handed on is not handed on any more: what is typed goes to a
// machine now or not at all.
inline constexpr std::uint64_t lateAfterMs = 1000;
// For how long Windows waits for a hook's procedure before it hands the key on without it: 300
// ms, unless this PC was told a shorter time. No longer one is ever believed.
inline constexpr std::uint64_t windowsWaitsMs = 300;
// The margin kept under that: a key that comes this close to it may be Windows' already. More
// than the three ticks of Windows' clock that a key's stamp and the time read here can be apart.
inline constexpr std::uint64_t inTimeMarginMs = 50;
// What the moment within which the two accounts have to be at one is longer than Windows' wait:
// for the thread to get from one of the two to the other.
inline constexpr std::uint64_t atOneMarginMs = 100;
// That moment where Windows waits as it does unless told otherwise: the longest it ever is.
inline constexpr std::uint64_t heardWithinMs = windowsWaitsMs + atOneMarginMs;
// For how long the hook stays in after the swallowing ended, for what it kept from Windows to be
// let go: no finger stays on a key for a minute. After it, its thread takes it out by itself.
inline constexpr std::uint64_t staysForMs = 60'000;
// How often the hook's thread looks, at the least: whether a key waits to be heard or to be told
// of, and whether its time is up. Its wait for the next message has this limit, so it says that
// it looks ten times a second while nothing comes. (A look that comes late ends nothing by being
// late.)
inline constexpr std::uint32_t lookEveryMs = 100;
// A key that waited the moment in vain is handed on at the look after it, and is not too late
// by then, with as long again to spare for the interface thread: whatever the registry says.
static_assert(heardWithinMs + lookEveryMs <= lateAfterMs / 2);
// For how long the stamp of the hook's thread may stand still, as the interface thread sees it,
// before the thread is taken for one that no longer looks. Thirty of its looks: a thread that
// goes before the program's other work and only waits does not miss thirty in a row on a PC that
// is merely busy (the tenth of a second that once ended control falsely was one look). And of a
// time in which the interface thread did not look either (the program was stopped, the PC
// slept) no more than a second counts: the two were stopped together.
inline constexpr std::uint64_t looksWithinMs = 3000;
inline constexpr std::uint64_t looksAtMostMs = 1000;
// For how long begin() waits for the hook to be in, and end() for its thread to be gone.
inline constexpr int startsWithinMs = 2000;
inline constexpr int endsWithinMs = 2000;

// A key as Windows tells the hook of it. (The right Shift comes with the mark, which that key
// never had: it is taken off, or the key would be none.)
input::Pressed pressedOf(std::uint32_t virtualKey, std::uint32_t scanCode, std::uint32_t flags) noexcept;
bool isTypedByAProgram(std::uint32_t flags) noexcept;
// A key as Raw Input tells of it (RAWKEYBOARD: MakeCode, Flags, VKey), made the key the hook
// tells of. Nothing: it is no key of its own (the second half of Pause, the Shift a keyboard
// makes up around some keys, a keyboard that says it lost count).
std::optional<input::Pressed> fromRawInput(std::uint32_t makeCode, std::uint32_t flags,
                                           std::uint32_t virtualKey) noexcept;
// For how long a key has waited since Windows stamped it (KBDLLHOOKSTRUCT::time, the clock of
// GetTickCount, which goes round every seven weeks). 0 where that is not known: no stamp, or
// one from after `now`.
std::uint64_t waitedMs(std::uint32_t nowTicks, std::uint32_t stampedTicks) noexcept;
// Both times by one clock that only goes forward, in milliseconds.
bool isAlive(std::uint64_t nowMs, std::uint64_t saidAtMs) noexcept;
bool isLate(std::uint64_t nowMs, std::uint64_t heardAtMs) noexcept;
// What Windows says of the session (WM_WTSSESSION_CHANGE): locked or unlocked, taken to another
// screen or back from one, here or from afar. After each, nothing is known of the keys.
bool meansNothingIsHeard(std::uint32_t sessionChange) noexcept;
// What Windows says of a device that comes or goes, to whoever asked for the raw keyboard with
// that word (WM_INPUT_DEVICE_CHANGE): which of the two it is; and what kind of device Windows
// says it is, where that can still be asked of one that went (RID_DEVICE_INFO::dwType).
inline constexpr std::uint32_t deviceCame = 1;
inline constexpr std::uint32_t deviceWent = 2;
inline constexpr std::uint32_t kindMouse = 0;
inline constexpr std::uint32_t kindKeyboard = 1;
// True: a keyboard was taken away from this PC. A device that went and of which Windows no
// longer says what it was (nothing) is taken for one: the word is asked for of keyboards only,
// and to take a keyboard for none is what leaves a key down on a machine. One that Windows
// still names as something else is none, and a device that came is nothing.
bool meansAKeyboardWent(std::uint32_t deviceChange, std::optional<std::uint32_t> kindOfDevice) noexcept;
// For how long Windows is taken to wait for a hook on this PC, from what its registry says of it
// (nothing: it says nothing, or nothing that is a number): what it says where that is shorter
// than Windows' own 300 ms, and else those. And from that: a key that has waited less than this
// came while Windows still waited; and the two accounts have to be at one within this.
std::uint64_t windowsWaitsFor(std::optional<std::uint32_t> saidMs) noexcept;
std::uint64_t inTimeUnder(std::uint64_t windowsWaitsForMs) noexcept;
std::uint64_t atOneWithin(std::uint64_t windowsWaitsForMs) noexcept;

// The leave-key's way out, as the procedure runs it for every key a finger pressed: pressed and
// let go with no other key going down between, it gives the keyboard back. The rule is that of
// input::ControlKeys, which whoever listens runs as well; this one is where the keys are heard,
// so that it holds whatever becomes of the rest of the program.
class LeaveWatch {
public:
    // `isDownNow`: the leave-key is held as the watching begins. Letting it go then gives nothing
    // back: it was not pressed for that.
    explicit constexpr LeaveWatch(std::uint16_t leaveKey = input::usageRightCtrl, bool isDownNow = false) noexcept
        : m_leaveKey(leaveKey), m_isDown(isDownNow), m_wasUsed(isDownNow)
    {}

    // A key went down or up, by its USB number (0: a key that has none, which counts for
    // nothing). `saidAgain`, of a key that goes down: it was down already, and is no key going
    // down. True: the keyboard is given back.
    constexpr bool take(std::uint16_t usage, bool down, bool saidAgain = false) noexcept
    {
        if (usage == 0) {
            return false;
        }
        if (usage != m_leaveKey) {
            m_wasUsed = m_wasUsed || (down && !saidAgain && m_isDown);
            return false;
        }
        if (down) {
            // (Windows says a held key again and again: the first time is the one that counts.)
            if (!m_isDown) {
                m_isDown = true;
                m_wasUsed = false;
            }
            return false;
        }
        const bool leaves = m_isDown && !m_wasUsed;
        m_isDown = false;
        return leaves;
    }
    // The keyboard is taken anew: a leave-key that is down just now was not pressed for leaving.
    constexpr void takenAnew() noexcept { m_wasUsed = m_wasUsed || m_isDown; }

private:
    std::uint16_t m_leaveKey;
    bool m_isDown;
    // Another key went down while the leave-key was down: letting it go leaves nothing.
    bool m_wasUsed;
};

// Whether the hook's thread still looks, as the interface thread finds it: asked every so often
// (four times a second while a machine is controlled) with the time and with the thread's stamp
// of when it last looked. While the stamp moves, it looks. Once it has stood still for
// `looksWithinMs` of the asker's own time, it does not; and of a stretch in which the asker did
// not ask either, no more than `looksAtMostMs` counts.
class LookWatch {
public:
    // True: the thread no longer looks.
    constexpr bool take(std::uint64_t nowMs, std::uint64_t stampedAtMs) noexcept
    {
        if (!m_asked || stampedAtMs != m_stamp) {
            m_asked = true;
            m_stamp = stampedAtMs;
            m_askedAtMs = nowMs;
            m_stoodForMs = 0;
            return false;
        }
        const std::uint64_t since = nowMs > m_askedAtMs ? nowMs - m_askedAtMs : 0;
        m_askedAtMs = nowMs;
        m_stoodForMs += since < looksAtMostMs ? since : looksAtMostMs;
        return m_stoodForMs >= looksWithinMs;
    }

private:
    bool m_asked = false;
    std::uint64_t m_stamp = 0;
    std::uint64_t m_askedAtMs = 0;
    std::uint64_t m_stoodForMs = 0;
};

// Who forgets what a hook's thread worked on, where that thread did not end in the time it was
// given and is left to end by itself: whichever of the two comes last. The thread says that it
// has ended, as the last thing it does with what it worked on; whoever waited for it says that
// it waits no longer. Each is told whether the forgetting is its own to do, and in whichever
// order the two come it is for one of them, and for one only. (Where the thread ended in its
// time, whoever waited forgets as ever, and does not ask.)
class LastToGo {
public:
    // The hook's thread. True: whoever waited has gone on already, and the thread forgets.
    bool theThreadHasEnded() noexcept { return m_state.exchange(ended) == leftBehind; }
    // Whoever waited. True: the thread has ended after all, in this very moment: forgotten here.
    bool isWaitedForNoLonger() noexcept { return m_state.exchange(leftBehind) == ended; }

private:
    static constexpr int neither = 0;
    static constexpr int ended = 1;
    static constexpr int leftBehind = 2;
    std::atomic<int> m_state{neither};
};

// What becomes of one key as it passes the procedure.
struct Verdict {
    bool swallow = false; // it is kept from every other program
    bool tell = false;    // the interface thread is to be woken: there is something for it

    bool operator==(const Verdict&) const = default;
};

// Which keys Windows itself has as down, by its own names for them (virtual keys): those it had
// when the listening began, and those that were passed on to it since. From that, what becomes
// of each key (`tell` here: it is the machine's press, or it goes up):
// * A press is swallowed where it can be, and is the machine's then. Not the machine's where
//   Windows has the key as down already: it is held since before, and what comes is its repeat.
// * A press that cannot be swallowed is Windows': it was typed into this PC, and is not typed
//   into a machine as well.
// * A key that goes up is never kept from Windows where Windows has the key as down: what it saw
//   go down, it sees go up.
class WindowsKeys {
public:
    static constexpr std::size_t names = 256;

    // Windows has this key as down: said of each such key as the listening begins.
    void has(std::uint32_t virtualKey) noexcept { m_down[place(virtualKey)] = true; }
    bool hasAsDown(std::uint32_t virtualKey) const noexcept { return m_down[place(virtualKey)]; }
    Verdict take(std::uint32_t virtualKey, bool down, bool canSwallow) noexcept;
    void forget() noexcept;

private:
    // (Windows has no name beyond 255; one that came all the same is the name no key has.)
    static constexpr std::size_t place(std::uint32_t virtualKey) noexcept
    {
        return virtualKey < names ? virtualKey : 0;
    }

    std::array<bool, names> m_down{};
};

// Which keys are physically down, as the procedure saw each go down and up: every key, kept from
// Windows or not, by the keyboard's own number for it and its mark; and which of them it kept
// from Windows, as presses of their own. A table of fixed size, which the hook's thread writes
// and the interface thread reads without a lock. It holds which keys are down and nothing else:
// not when, not how often, not in which order.
//
// A key that is down is the key it went down as, with the name Windows gave it then, until it
// goes up: Windows may call it otherwise when it is said again, or as it goes up. And it is
// known under another number where that is the same key by its USB number (Windows tells of
// Pause as Break under Ctrl, and of Print Screen with another number under Alt): of no two
// physical keys does `input::usageOf` make the same. And Num Lock is known at either of its two
// places while one of them is down, though `input::usageOf` makes two keys of them (with the
// mark Num Lock, without it and named "Pause" the Pause key). Not told apart by that: the Pause
// key pressed while Num Lock is held, or the other way round, which counts as the one that is
// down said again.
//
// What Windows cannot name (0xFF) is no key: it goes neither down nor up here.
class KeysHeld {
public:
    // (A number beyond these is no key's: the left Ctrl that Windows makes up under AltGr has one.)
    static constexpr std::uint32_t scanCodes = 0x80;

    // What became of a key that went down or up.
    struct Taken {
        // The key as it went down, where it was down already (said again, or let go now). Else
        // as it is told now.
        input::Pressed key;
        bool wasDown = false;
        // It had been kept from Windows (which it goes on being while it is only said again).
        bool wasKept = false;
        // It went up, and with that no key at all is down.
        bool nothingIsDown = false;
    };

    // The hook's thread. A key went down or up.
    Taken take(const input::Pressed& key, bool down) noexcept;
    // This key, which is down, was kept from Windows as a press of its own.
    void keep(const input::Pressed& key) noexcept;
    // The key as it went down, where it is down, under whichever number; nothing: it is not.
    std::optional<input::Pressed> asItWentDown(const input::Pressed& key) const noexcept;
    void forget() noexcept;
    // This key going down, with what is down now, is what Windows itself takes the keyboard with:
    // L under a Windows key locks the PC, Del under Ctrl and Alt brings its own screen. Windows
    // acts on either whatever a listener does with the key.
    bool isTakenByWindows(const input::Pressed& key) const noexcept;
    // Either thread.
    bool isDown(std::uint32_t scanCode, bool extended) const noexcept;
    // This key is down, under whichever number, and had been kept from Windows.
    bool isKept(const input::Pressed& key) const noexcept;
    // The key that is down at a place of the table and had been kept from Windows, as it went
    // down; nothing: none is. (For going through them without taking memory.)
    static constexpr std::size_t places = std::size_t{scanCodes} * 2;
    std::optional<input::Pressed> keptAt(std::size_t at) const noexcept;
    // The same of whichever key is down there.
    std::optional<input::Pressed> downAt(std::size_t at) const noexcept;
    // The interface thread: all of them, each with the name Windows gave it as it went down;
    // and those of them that were kept from Windows.
    std::vector<input::Pressed> all() const;
    std::vector<input::Pressed> kept() const;

private:
    static constexpr std::size_t place(std::uint32_t scanCode, bool extended) noexcept
    {
        return (scanCode * 2) + (extended ? 1 : 0);
    }
    // The key that is down at a place, with the name it went down under.
    static constexpr input::Pressed keyAt(std::size_t at, std::uint32_t slot) noexcept
    {
        return {static_cast<std::uint32_t>(at / 2), at % 2 != 0, slot & nameBits};
    }
    // What this table holds: a key with a number of the keyboard's that Windows could name.
    static bool isAKey(const input::Pressed& key) noexcept;
    // Where a key is in the table: its own place, where it is down there or nowhere; and the
    // place of the same key under another number, where it is down there.
    std::size_t placeOf(const input::Pressed& key) const noexcept;

    // For each key: 0 while it is up, and else the name Windows gave it as it went down, with
    // `isDownBit`, and with `isKeptBit` where its press was kept from Windows.
    static constexpr std::uint32_t isDownBit = 0x8000'0000;
    static constexpr std::uint32_t isKeptBit = 0x4000'0000;
    static constexpr std::uint32_t nameBits = 0xFFFF;
    std::array<std::atomic<std::uint32_t>, places> m_keys{};
    // How many of them are down.
    std::atomic<std::uint32_t> m_down{0};
    // The left Ctrl that Windows makes up under AltGr is no key, and counts for Windows as Ctrl.
    std::atomic<bool> m_madeUpCtrl{false};
};

// A key on its way from the procedure to the interface thread, and when it was heard.
struct Heard {
    input::Pressed key;
    bool down = false;
    std::uint64_t atMs = 0;
    // Of a key that went down: it was kept from Windows, as a press of its own.
    bool swallowed = false;
    // Of a key that went up: no key at all was down after it, and nothing waited to be handed on
    // as it was.
    bool nothingIsDown = false;
    // Which of the keys that are on their way it is, in the order they were heard in
    // (`Procedure::onTheirWay`).
    std::uint64_t number = 0;
};

// The way there: a ring of fixed size, which one thread puts into and one other takes out of. No
// lock, no memory taken. What was taken out is wiped where it stood: a key is handed on and
// forgotten.
class KeyRing {
public:
    // (One place is always left empty: that is how full is told from empty.)
    static constexpr std::size_t places = 256;

    // The thread that puts in.
    bool hasRoom() const noexcept;
    bool put(const Heard& heard) noexcept;
    // The thread that takes out.
    std::optional<Heard> take() noexcept;
    // While nobody puts in or takes out.
    void clear() noexcept;

private:
    static constexpr std::size_t after(std::size_t place) noexcept { return (place + 1) % places; }

    std::array<Heard, places> m_slots{};
    std::atomic<std::size_t> m_in{0};
    std::atomic<std::size_t> m_out{0};
};

// The keys that were heard and are not handed on yet, because the two accounts of the keyboard
// are not at one: in the order they were heard in, a few, each for no longer than the moment.
// The keys that went down, and behind each the keys that may not pass it: its own going up, and
// the going up of a modifier. A row of fixed size, the hook thread's own. What goes out, or is
// dropped, is wiped where it stood.
class KeysWaiting {
public:
    static constexpr std::size_t places = 32;

    struct One {
        Heard heard;
        // Of a key that went down: the other account has told of it, or is not waited for.
        bool told = false;
        // Of a key that went down and waits for that account's word: how many sayings of the
        // key were heard since, each of which waits for a word of its own. That account tells
        // in the order the keys came in, so its word is this one's when just so many wait
        // behind the hearing it was for. (What waits at this moment, for as long as this
        // does: nothing of how often a key was pressed.)
        std::uint32_t saidSince = 0;
    };

    bool isEmpty() const noexcept { return m_count == 0; }
    bool hasRoom() const noexcept { return m_count < places; }
    std::size_t count() const noexcept { return m_count; }
    bool put(const Heard& heard, bool told) noexcept;
    // The one at a place of the row, 0 being the one that has waited longest.
    const One& oneAt(std::size_t place) const noexcept { return m_slots[place]; }
    // It goes out, or is dropped: what is behind it moves up.
    void drop(std::size_t place) noexcept;
    // Whether a key that goes up has to stay behind something among the first `before` of the
    // row: its own going down, or, where it is a modifier, any key going down.
    bool holdsBackTheUpOf(const input::Pressed& key, std::size_t before) const noexcept;
    // The going down of this key waits in the row.
    bool holdsTheDownOf(const input::Pressed& key) const noexcept;
    // This key was heard to go down once more, and that hearing waits for the other account's
    // word. (Said before the hearing itself is put in the row, where it is.)
    void heardOnceMore(const input::Pressed& key) noexcept;
    // The other account told of this key going down, and that was its word for a hearing
    // behind which `stillWait` hearings of the key going down still wait for theirs. The
    // press in the row whose hearing that was waits for it no more; none does, where it was
    // the word for a saying of the key that is not in the row (the key said again).
    void tell(const input::Pressed& key, std::size_t stillWait) noexcept;
    // It tells of no key any more, or of none of this keyboard: none waits for its word.
    void tellAll() noexcept;
    void clear() noexcept;

private:
    // (The one that has waited longest is the first, always.)
    std::array<One, places> m_slots{};
    std::size_t m_count = 0;
};

// Windows' two accounts of the keyboard, held against each other: what the hook's procedure
// heard, and what Raw Input told of. Every key that was told of has to have been heard, the same
// key going the same way, within a moment before or after: whichever of the two comes first
// waits for the other, and a key that was told of and waits in vain says that the hook did not
// hear it. A key that was heard and never told of says nothing here (what to make of it where
// Raw Input does tell of the keyboard is the gate's to say, `Procedure`): its hearing stays
// noted until a telling takes it or finds it older than the moment, until whoever looks drops
// what is older than it has a use for (`dropWhatWasHeardBefore`), or all is forgotten
// (`forget`); a telling takes no hearing that is older than the moment. Only
// the hearings of keys that the other account can tell of are brought here: not what a program
// typed, and not what Windows cannot name or gives a number no key has. And none at all where
// there is no other account to hold a hearing against: on such a PC nothing is noted of a key,
// however often it is pressed (`Procedure::key` brings none).
//
// Only the keys of the keyboard's page, each by its USB number, as `input::usageOf` reads its
// number and mark. A key that has none (a key for sound, a key that starts a program) is not
// compared, and nothing of it is noted, heard or told of: such a key shares its number with a
// letter and differs by the mark (Volume up and B, Mute and D), and is not that letter. No key
// is read with its mark turned: its hearing would be noted as the letter's, and take the telling
// of a press of that letter which the hook was not called for. What that costs: a hook that
// Windows took out is not found by a key for sound, and is found by the next key of the
// keyboard's page. And a key of which the two accounts tell with marks that differ, in a way
// not foreseen here (`pressedOf`, `fromRawInput`), is no one key to the comparing: the control
// ends when it is pressed, and whoever tries every key of a keyboard sees it at once.
//
// A key that was down when the comparing began is not compared until it has gone up: of what it
// does until then, one account may tell and the other never (it was on its way as the two
// began).
//
// (What Raw Input says is down was kept here as an account of its own, beside the comparing,
// and a key that it had as up was let go by that word. Nothing came to that but a telling that
// was forgiven, and none is forgiven any more: it is gone. Which keys are down is the hook's
// own table's to say, by the keys as they are heard, and nobody else's.)
//
// Tables of fixed size, the hook thread's own: for each key and each way, when it was heard and
// has not been told of yet, and when it was told of and has not been heard yet. The time of a
// few of each at the most, each wiped as it is matched; where more of one key wait than that
// (a held key that is said again while one account is ahead of the other), how many more, and
// when the last of them came. That is a count of what waits at this moment, for as long as it
// waits, and of nothing else: nothing here says how often a key was pressed.
class TwoAccounts {
public:
    // What a hearing found: the key is not compared; the other account's word for it is still
    // to come; it has come already.
    enum class Compared { No, Waits, Told };

    void forget() noexcept;
    // Nothing that was heard waits to be told of any more: there is no account to hold it
    // against. (What is left out of the comparing stays left out.)
    void forgetWhatWasHeard() noexcept;
    // This key is down as the comparing begins.
    void leaveOutUntilUp(const input::Pressed& key) noexcept;
    // The procedure heard this key go down or up.
    Compared heard(const input::Pressed& key, bool down, std::uint64_t nowMs) noexcept;
    // Said before that, of a key that goes down and was up: a press. What was heard of the key
    // going down longer ago than the moment is no hearing of this press's, and waits to be
    // told of no more. (Sayings of the key from before it was let go, which the other account
    // never told of: they stood for two moments and a look, and the press's own hearing
    // renewed the time of those that had no room, so that its word was taken for one of
    // theirs.)
    void pressedAnew(const input::Pressed& key, std::uint64_t nowMs, std::uint64_t withinMs = heardWithinMs) noexcept;
    // Raw Input told of it. True: it had been heard, within the moment, and that waited.
    bool told(const input::Pressed& key, bool down, std::uint64_t nowMs,
              std::uint64_t withinMs = heardWithinMs) noexcept;
    // How many hearings of this key going this way wait to be told of, however long.
    std::size_t waitToBeTold(const input::Pressed& key, bool down) const noexcept;
    // What was heard longer ago than this waits to be told of no more.
    void dropWhatWasHeardBefore(std::uint64_t nowMs, std::uint64_t withinMs) noexcept;
    // Nothing that was heard is noted at this moment: none waits to be told of.
    bool notesNoHearing() const noexcept;
    // A key that was told of waits to be heard: the two are not at one.
    bool waitsToBeHeard() const noexcept { return m_toldWaiting > 0; }
    // This key going this way was told of and not heard.
    bool isToldAndNotHeard(const input::Pressed& key, bool down) const noexcept;
    // A key that was told of has waited longer than the moment to be heard. Asked after
    // everything that had come was looked at: what was only late has been matched by then.
    bool isBehind(std::uint64_t nowMs, std::uint64_t withinMs = heardWithinMs) const noexcept;
    // The same, of a key that was told of later than `afterMs`.
    bool isBehindOfWhatWasToldAfter(std::uint64_t afterMs, std::uint64_t nowMs, std::uint64_t withinMs) const noexcept;
    // The other way round: a key was heard at least this long ago, and no longer ago than that,
    // and has not been told of.
    bool wasHeardAndNotTold(std::uint64_t nowMs, std::uint64_t atLeastMs, std::uint64_t atMostMs) const noexcept;
    // Whether the two are the same key to both accounts: one key of the keyboard's page.
    static bool isSameKey(const input::Pressed& one, const input::Pressed& other) noexcept;

private:
    // The keys that are compared: those `input::usageOf` knows, by the number it gives them.
    static constexpr std::size_t keys = 256;
    // Of how many of one key, going one way, that wait at a time the time is noted: a held key
    // that is said again while the thread was kept from its work for a moment.
    static constexpr std::size_t atATime = 3;
    // What waits of one key going one way: when each of the first few came (a time plus one, so
    // that 0 is "none"); and of those that came while there was no room to note that, how many
    // they are and when the last of them came.
    struct Waiting {
        std::array<std::uint64_t, atATime> at{};
        std::uint32_t more = 0;
        std::uint64_t lastOfMore = 0;
    };
    // Which account still leaves a key out, until that account has said that it went up.
    static constexpr std::uint8_t heardSide = 0x01;
    static constexpr std::uint8_t toldSide = 0x02;

    static std::optional<std::size_t> numberOf(const input::Pressed& key) noexcept;
    // True: this side leaves the key out (and, where it goes up, no longer from here on).
    bool isLeftOut(std::size_t key, bool down, std::uint8_t side) noexcept;
    // One more waits. And the one that has waited longest waits no more: false where none did.
    static void note(Waiting& waiting, std::uint64_t nowMs) noexcept;
    static bool takeTheOldest(Waiting& waiting) noexcept;
    // What has waited longer than the moment waits no more.
    static void dropTheOverdue(Waiting& waiting, std::uint64_t nowMs, std::uint64_t withinMs) noexcept;

    std::array<Waiting, keys * 2> m_heard{};
    std::array<Waiting, keys * 2> m_told{};
    std::array<std::uint8_t, keys> m_leftOut{};
    // How many tellings wait to be heard.
    std::size_t m_toldWaiting = 0;
};

// Which window Windows has in front at this moment, by its handle (GetForegroundWindow, asked on
// the hook's own thread): a plain function, which waits for nothing and takes no memory. In a test,
// one that says what the test says.
using WindowInFront = std::uintptr_t (*)() noexcept;

// Everything the hook's procedure does with a key, and what the interface thread says to it and
// takes from it. The two threads share nothing else.
//
// One of these is for one hook. It begins swallowing nothing; swallows while it is asked to; and
// once that has ended it tells only of what it kept from Windows going up, for as long as the
// hook stays (`staysForMs`). After that it does nothing with a key any more. And once it is
// closed (keys were not heard, the interface thread was silent, or its time was up) it never
// swallows again, whoever asks.
class Procedure {
public:
    // What the hook's thread is to do after it has looked: go on, or take the hook out and end.
    enum class Next { GoOn, End };
    struct Looked {
        Next next = Next::GoOn;
        // The interface thread is to be woken: there is something for it.
        bool wake = false;
    };
    // The keys that were heard and are still to be taken with `next`, at one moment: the number
    // of the one that was put on its way last, and how many they are.
    struct OnTheirWay {
        std::uint64_t last = 0;
        std::uint32_t count = 0;
    };

    // While no key comes (before the hook is in): nothing is heard, held or swallowed so far, and
    // the interface thread was alive a moment ago. `windowsWaitsForMs`: for how long Windows
    // waits for a hook on this PC (`windowsWaitsFor`).
    void begin(std::uint64_t nowMs, std::uint64_t windowsWaitsForMs = windowsWaitsMs,
               std::uint16_t leaveKey = input::usageRightCtrl) noexcept;
    // And as it is put in: Windows has this key as down; the leave-key is held; this key is
    // down, by what Windows or an earlier hook knows, and is not compared until it has gone up.
    void windowsHas(std::uint32_t virtualKey) noexcept { m_windows.has(virtualKey); }
    void leaveKeyIsHeld() noexcept { m_leave = LeaveWatch(m_leaveKey, true); }
    std::uint16_t leaveKey() const noexcept { return m_leaveKey; }
    void isDownAlready(const input::Pressed& key) noexcept { m_accounts.leaveOutUntilUp(key); }
    // And as it is put in, before any key comes: the window the keys are for, by its handle, and
    // how Windows is asked which window is in front. From then on, a key that goes down (pressed,
    // or a held key said again) while another window is in front ends the swallowing as it is
    // heard, as the leave-key does: it is that window's key, and so is every key after it, until
    // swallowing is asked for anew; and asked for anew while another window is in front, it ends
    // again at the next such key. What was kept from Windows is told as it goes up, as ever.
    // (Not said, as in a test that is about something else: no window is asked after.)
    void isFor(std::uintptr_t window, WindowInFront inFront) noexcept
    {
        m_window = window;
        m_inFront = inFront;
    }
    // While no key comes any more (the hook is out, and its thread gone): nothing of what was
    // typed stays.
    void forget() noexcept;

    // The hook's thread. A key as Windows tells of it (KBDLLHOOKSTRUCT), whether it went down,
    // and for how long it has waited since Windows stamped it (`waitedMs`). No memory, no lock,
    // no log.
    //
    // What is told (see KeySource): while swallowing is asked for, every key that goes down and
    // up; otherwise only the going up of a key that this hook kept from Windows, and nothing of
    // any other key of the PC. A key that should have been told and was not, for want of room,
    // counts as having fallen behind; and so does one that came after Windows had stopped
    // waiting for it, which is Windows' key by then, and one that finds the interface thread
    // silent, of which nothing is told at all. Of a key that a program typed nothing is told,
    // noted or compared: only that one came while keys were swallowed, once (`tell` then).
    //
    // What is noted of a key that a finger pressed: that it is down, until it is up; and, only
    // where Windows' other account is told to this hook (`isWatched`), when it was heard, until
    // that account has told of it or two of the gate's moments are over. Where there is no such
    // account, nothing of a hearing is noted: not when, and not how often.
    //
    // The one rule for a key that goes up, whichever keyboard it was pressed on and however
    // many hold it: it is let go, in the table and for whoever listens, by the "up" as it is
    // heard. A hook is not told which keyboard a key came from, and nothing here asks. What
    // that costs with two keyboards in use at once: a modifier held on the one is let go by
    // the same modifier's "up" on the other, and the next key from the first arrives bare.
    // That is the side to fail on: the other is a modifier that stays down on a machine.
    Verdict key(std::uint32_t virtualKey, std::uint32_t scanCode, std::uint32_t flags, bool down, std::uint64_t nowMs,
                std::uint64_t waitedForMs = 0) noexcept;
    // The hook's thread. Raw Input told of a key of a real keyboard (`fromRawInput`): of
    // whichever keyboard, which is not said here. True: the interface thread is to be woken.
    bool rawKey(const input::Pressed& key, bool down, std::uint64_t nowMs) noexcept;
    // The hook's thread, as the comparing begins and where it ends: Raw Input was asked for and
    // tells this thread of the keyboard; it tells it no more (another window's hook asked).
    // (Asked for is not yet known to tell: that is so from the first key it tells of.)
    void rawInputTells() noexcept { m_rawTells.store(true); }
    void rawInputTellsNoMore() noexcept;
    // The hook's thread, every so often, after everything that had come was looked at.
    Looked look(std::uint64_t nowMs) noexcept;
    // The hook's thread. Keys will not be told any more (Windows would not wait for this thread):
    // nothing more is swallowed, and whoever listens is to let go of everything.
    void fallBehind(std::uint64_t nowMs) noexcept;
    // The hook's thread. Windows shows another desktop, or the session is another's or locked:
    // the same, with its own word for why.
    void anotherDesktop(std::uint64_t nowMs) noexcept;
    // The hook's thread. Windows says that a keyboard was taken away from this PC
    // (`meansAKeyboardWent`): whichever keyboard, which is not said here and not asked. A key
    // that was held on it will never be heard to go up, by either account. While keys are
    // swallowed: nothing more is swallowed, and whoever listens is told to let go of
    // everything, with a word of its own. True then: the interface thread is to be woken.
    // While none are swallowed it is nothing: no machine has a key down by this hook. (What
    // the table has as down stays as it is: which of it was held on that keyboard is not
    // known. Whoever listens keeps such a key back at the next taking, until it is let go or
    // seen to go down anew.)
    bool aKeyboardWent(std::uint64_t nowMs) noexcept;
    // The hook's thread, as it begins: Windows was asked to say when a keyboard goes, and took
    // the asking.
    void keyboardsGoingIsTold() noexcept { m_goingIsTold.store(true); }

    // The interface thread. True: keys are swallowed from now on.
    bool setSwallowing(bool swallowing, std::uint64_t nowMs) noexcept;
    bool swallows() const noexcept { return m_state.load() == isSwallowing; }
    // Nothing is swallowed by this hook any more, whoever asks. (Either thread.)
    void close(std::uint64_t nowMs) noexcept;
    bool isClosed() const noexcept { return m_state.load() == isClosedForGood; }
    void stillThere(std::uint64_t nowMs) noexcept
    {
        m_saidAtMs.store(nowMs);
        m_wasFoundSilent.store(false);
    }
    // When the hook's thread last looked.
    std::uint64_t lookedAtMs() const noexcept { return m_lookedAtMs.load(); }
    // Whether Windows' other account of the keyboard is told to this hook's thread, as far as
    // that is known: it was asked for, is not another hook's by now, and no key has waited in
    // vain for its word since it last told of one.
    bool isWatched() const noexcept { return m_rawTells.load(); }
    // Whether Windows tells this hook's thread when a keyboard is taken away from the PC: it
    // was asked to, and that word is not another hook's by now.
    bool isToldOfKeyboardsGoing() const noexcept { return m_goingIsTold.load(); }
    // The next key that was heard; nothing: none waits.
    std::optional<Heard> next() noexcept;
    // The keys, going down or up, that were heard and are still to be taken with `next`: those
    // that wait for the two accounts to be at one, and those that wait to be taken. Each has a
    // number (`Heard::number`), given in the order they were heard in; what is taken with `next`
    // does not come in that order (a key that goes up may pass one that waits). For whoever
    // sends something after the keys that were heard before it. (What is on its way at this
    // moment, and nothing of what was typed: the count is 0 again when they came, and the
    // numbers begin at 1 again from there.)
    OnTheirWay onTheirWay() const noexcept
    {
        const std::uint64_t way = m_way.load();
        return {way >> wayCountBits, static_cast<std::uint32_t>(way & wayCountMask)};
    }
    // Keys came that could not be told since this was last asked.
    bool takeFellBehind() noexcept { return m_fellBehind.exchange(false); }
    // Windows showed another desktop since this was last asked.
    bool takeAnotherDesktop() noexcept { return m_anotherDesktop.exchange(false); }
    // The keys were pressed that Windows itself acts on, since this was last asked.
    bool takeKeysOfWindows() noexcept { return m_keysOfWindows.exchange(false); }
    // Raw Input told of a key that was not heard here, while keys were swallowed, since this was
    // last asked.
    bool takeNotHanded() noexcept { return m_notHanded.exchange(false); }
    // A key was heard here that Raw Input, which tells of this keyboard, did not tell of, while
    // keys were swallowed, since this was last asked.
    bool takeNotTold() noexcept { return m_notTold.exchange(false); }
    // A keyboard was taken away from this PC, while keys were swallowed, since this was last
    // asked.
    bool takeAKeyboardWent() noexcept { return m_keyboardWent.exchange(false); }
    // Raw Input began to tell where it had told of nothing, and a key it told of in its first
    // moment found no hearing, while keys were swallowed, since this was last asked.
    bool takeBeganToTell() noexcept { return m_beganToTell.exchange(false); }
    // A key came that a program typed, for the first time since keys are swallowed, since this
    // was last asked.
    bool takeTypedByAProgram() noexcept { return m_typedIsToBeSaid.exchange(false); }
    // The keys that were heard go down and not up, kept from Windows or not; and those of them
    // that were kept from Windows, told of yet or not.
    std::vector<input::Pressed> whatIsDown() const { return m_held.all(); }
    std::vector<input::Pressed> whatIsKept() const { return m_held.kept(); }
    // Of what was heard, nothing is noted at this moment but which keys are down. (While no key
    // comes: for a test to hold the rule to.)
    bool notesNoHearing() const noexcept { return m_accounts.notesNoHearing(); }

private:
    static constexpr int isIdle = 0;
    static constexpr int isSwallowing = 1;
    static constexpr int isClosedForGood = 2;

    void stopSwallowing(std::uint64_t nowMs) noexcept;
    // The keys that Windows itself acts on were pressed: nothing more is swallowed.
    void keysOfWindows(std::uint64_t nowMs) noexcept;
    // The hook has stayed its time since the swallowing ended.
    bool isOver(std::uint64_t nowMs) const noexcept;
    // There is room to tell of a key, now or when the two accounts are at one.
    bool hasRoomToTell() const noexcept { return m_ring.hasRoom() && m_waiting.hasRoom(); }
    // A key is to be told: handed on now, or kept in the row until it may go. `saidAgain`, of a
    // key that goes down: it was down already. True: something was handed on, or could not be,
    // and the interface thread is to be woken.
    bool hand(Heard heard, TwoAccounts::Compared compared, bool saidAgain, std::uint64_t nowMs) noexcept;
    // A key is on its way from here on: its number. And one is on its way no longer (it was
    // taken, or dropped).
    std::uint64_t onItsWay() noexcept;
    void offItsWay() noexcept;
    // Into the ring, for the interface thread to take. Where there is no room, that is marked:
    // a key may be down on a machine, or down here unknown.
    void intoTheRing(const Heard& heard) noexcept;
    // What waited and need not wait any longer is handed on. True: something was, or the
    // interface thread is to be woken for another reason.
    bool passOn(std::uint64_t nowMs) noexcept;
    // Whether the key going down at this place of the row may go now. It may not while the two
    // accounts are not at one; and where it has waited the moment in vain for the other
    // account's word, that account either does not tell of this keyboard (it goes, and nothing
    // waits for such a word from here on) or told differently (the hook is closed: false).
    bool mayGo(const KeysWaiting::One& one, std::uint64_t nowMs) noexcept;
    // The control ends over a key that was told of and not heard: where that was the going up
    // of a key that had been kept from Windows, it is up, and told as such.
    bool takeWhatWasToldToGoUp(std::uint64_t nowMs) noexcept;
    // The other account tells of a key where it was taken not to tell (a first key had waited
    // for its word in vain): the comparing begins here, with no hearing noted so far. False:
    // this telling waits for no hearing. What it tells of within the gate's moment from here
    // may be its word for a key that was heard before any hearing was noted, or for one that
    // went down since and was not heard: such a telling waits its moment like any other, and
    // where it waited in vain the control ends as over any other, with a word of its own for
    // not knowing which it was (`look`).
    bool beginComparingAt(const input::Pressed& key, bool down, std::uint64_t nowMs) noexcept;

    LeaveWatch m_leave;
    std::uint16_t m_leaveKey = input::usageRightCtrl;
    // The window the keys are for, and how to ask which is in front (`isFor`). Said before the hook
    // is in, and read by the hook's thread alone from then on.
    std::uintptr_t m_window = 0;
    WindowInFront m_inFront = nullptr;
    WindowsKeys m_windows;
    KeysHeld m_held;
    KeyRing m_ring;
    KeysWaiting m_waiting;
    TwoAccounts m_accounts;
    // Raw Input has told of a key of this keyboard since this hook went in: the hook thread's
    // own. From then on a key that it does not tell of is one the two accounts differ about.
    bool m_rawHasTold = false;
    // The comparing began at a telling, with hearings noted only from then on: until when a
    // telling is one of that first moment, which may be the word for a key that was heard
    // before any hearing was noted (a time plus one; 0: none is). The hook thread's own, and
    // kept for no longer than two of the gate's moments.
    std::uint64_t m_beganToTellUntil = 0;
    // The keys that are in the row or the ring: how many, in the lower bits, and above them the
    // number of the one that was put there last. One word, so that whoever asks has the two as
    // they were at one moment. (No more are ever on their way than the row and the ring hold.)
    static constexpr unsigned wayCountBits = 16;
    static constexpr std::uint64_t wayCountMask = (std::uint64_t{1} << wayCountBits) - 1;
    static_assert(KeyRing::places + KeysWaiting::places <= wayCountMask);
    std::atomic<std::uint64_t> m_way{0};
    // What a key may have waited at the most to have come in time; and the moment within which
    // the two accounts have to be at one. Set as it begins, before any key comes.
    std::uint64_t m_inTimeMs = inTimeUnder(windowsWaitsMs);
    std::uint64_t m_atOneMs = heardWithinMs;
    // Whether keys are swallowed, and since when they are not; and when the interface thread
    // last said that it is alive.
    std::atomic<int> m_state{isIdle};
    std::atomic<std::uint64_t> m_sinceMs{0};
    std::atomic<std::uint64_t> m_saidAtMs{0};
    // A key found the interface thread silent while keys were to be swallowed, and it has not
    // said anything since.
    std::atomic<bool> m_wasFoundSilent{false};
    std::atomic<std::uint64_t> m_lookedAtMs{0};
    // Raw Input tells this hook's thread of the keyboard, as far as is known.
    std::atomic<bool> m_rawTells{false};
    // Swallowing was asked for anew, and the leave-key's watch does not know yet.
    std::atomic<bool> m_takenAnew{false};
    std::atomic<bool> m_fellBehind{false};
    std::atomic<bool> m_anotherDesktop{false};
    std::atomic<bool> m_keysOfWindows{false};
    std::atomic<bool> m_notHanded{false};
    std::atomic<bool> m_notTold{false};
    std::atomic<bool> m_keyboardWent{false};
    std::atomic<bool> m_beganToTell{false};
    // Windows tells this hook's thread when a keyboard goes, as far as is known.
    std::atomic<bool> m_goingIsTold{false};
    // A key that a program typed came since keys are swallowed; and that is still to be said.
    std::atomic<bool> m_typedWasSeen{false};
    std::atomic<bool> m_typedIsToBeSaid{false};
};

// What the interface thread tells whoever listens, each time it is woken or asked: made from
// what the procedure has for it, apart from Windows, in the order it is to be said in.
// * Why nothing is heard any more, first, and one reason only, the one that is known best: the
//   keys that Windows acts on, before a desktop of Windows' own, before a keyboard that went,
//   before the two accounts that differ (in the first moment of the one that began to tell
//   late; told and not heard, heard and not told, or both), before keys that fell behind.
//   Whoever listens lets go of everything before anything else.
// * Then the keys, in the order they were handed on. One that has waited `lateAfterMs` since it
//   was heard is not typed late: with it the keys have fallen behind, the hook is closed, and
//   of that key and every key behind it only a going up is told (it is waited for).
// * Each key is asked, as it is taken, whether the hook has been closed by then. The hook's
//   thread goes on meanwhile: a press that it handed on in the very call that closed the hook
//   (the gate found the two accounts to differ over a key before it, and dropped that key)
//   comes behind reasons that were asked for a moment too soon. It is not typed: of it and of
//   what is behind it only a going up is told, and why is asked for once more there, and said
//   before what follows. (Whatever closes the hook marks why before it closes it.)
// * With a key that went up and left no key down at all: that.
struct Told {
    enum class What { Key, FellBehind, LostHearing, NothingIsDown, TypedByAProgram };

    What what = What::Key;
    // Of a key.
    Heard heard;
    // Of hearing that was lost.
    KeySource::Lost why = KeySource::Lost::NotHandedKeys;
};
// Each thing is handed to `say` as it is made; and, for a test, all of it in a row.
void whatToTell(Procedure& procedure, std::uint64_t nowMs, const std::function<void(const Told&)>& say);
std::vector<Told> whatToTell(Procedure& procedure, std::uint64_t nowMs);

// The telling itself, by the interface thread: what `whatToTell` makes is said to whoever
// listens through a key source's own signals, one thing at a time and in its order. Whoever is
// told of a key may ask, from inside that telling, for everything that waits to be told
// (`KeySource::tellNow`: it found that nothing is swallowed any more, and was not told why). What
// one pass made and has not said yet is therefore kept here, and said first by the pass that is
// asked for inside it: the reason that stands behind a key in one pass reaches whoever asks
// while the key is told, and is not said a second time when that returns. (It was said after
// the key's telling had returned, by which time whoever listened had given keyboard and mouse
// back with no reason, and dropped the one that came.)
//
// What is kept so is kept for the time of one telling, in a row of this one's own, and wiped
// where it stood: each place as what stood there is taken out to be said, all of them where
// the row is forgotten, and the whole of a row that was outgrown before it is given up. A key
// is handed on and forgotten, here as in the ring. (The row was a list that kept what had been
// taken out of it in the memory behind it, and forgot by setting its length to nothing.) And
// wiped so that it stays wiped: in Windows' own way of writing nothing over memory, which is
// not left out where the memory is given back right after, as a row that was outgrown is. The
// one copy of a key that a pass holds while it is said is wiped so as well, once it was said.
// Set, and not shown by a test: that of the copies; and that nothing is left where the
// compiler keeps a key by itself (a place that was given back, a register).
class Telling {
public:
    // Everything that waits to be told is said through `source`, before this returns.
    // `goesOn`: asked before each thing; false where whoever was told has ended the listening
    // in the middle, and what was not said went with it.
    void all(Procedure& procedure, std::uint64_t nowMs, KeySource& source, const std::function<bool()>& goesOn);
    // Nothing waits to be said any more, and nothing of what waited is kept.
    void forget() noexcept;
    // The memory the row has, every place of it, whether something waits there or not: for a
    // test to look at what is left in it.
    std::span<const Told> memory() const noexcept { return m_places; }
    // And the memory of a row that was outgrown, at the moment it is given up: for a test to
    // look at what is left in that.
    void whenARowIsOutgrown(std::function<void(std::span<const Told>)> look) { m_lookAtTheOutgrown = std::move(look); }

private:
    // One more waits, behind what does; and the next of what waits, which waits no more, into
    // a place of whoever says it.
    void put(const Told& told);
    void takeTheNext(Told& into) noexcept;

    // What was made and is still to be said: the places from `m_next` up to `m_end`, the next
    // of it first. Every other place holds nothing.
    std::vector<Told> m_places;
    std::size_t m_next = 0;
    std::size_t m_end = 0;
    std::function<void(std::span<const Told>)> m_lookAtTheOutgrown;
};

// The physical keys that are down, from what Windows says of its own names for keys: `nameOf`
// is Windows' name for a physical key (scan code, and whether it has the mark; 0: it has none),
// `isDown` whether the key of a name is down. Only keys that have a USB number, and only where
// the name is one such key's alone: Enter and the Enter of the number block share theirs, and so
// do the arrows and the number block's. Of two that share a name neither is said: a key that was
// said to be down and is not would be waited for, to be let go, in vain. For the same reason the
// left Ctrl is not said while the right Alt is down: under AltGr Windows has a left Ctrl of its
// own making as down, which is no key and is never let go as one. (A real left Ctrl that is held
// together with the right Alt before the listening begins is therefore not known.)
std::vector<input::Pressed> heldKeys(const std::function<std::uint32_t(std::uint32_t scanCode, bool extended)>& nameOf,
                                     const std::function<bool(std::uint32_t virtualKey)>& isDown);
// The same, of every key whose name is down, also where two keys share the name: the keys that
// may be down, which are not compared until each has gone up.
std::vector<input::Pressed>
keysThatMayBeDown(const std::function<std::uint32_t(std::uint32_t scanCode, bool extended)>& nameOf,
                  const std::function<bool(std::uint32_t virtualKey)>& isDown);
// What is down, from the two that know: what the procedure heard, and what Windows has from
// before. A key both know of is said once.
std::vector<input::Pressed> together(std::vector<input::Pressed> heard, const std::vector<input::Pressed>& ofWindows);

} // namespace hook

} // namespace qkvm::ui
