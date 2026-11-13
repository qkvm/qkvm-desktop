#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

#include <QElapsedTimer>
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QSize>
#include <QString>
#include <QTimer>

#include <qkvm/core/live.h>
#include <qkvm/input/keys.h>

#include "control_view.h"
#include "key_source.h"
#include "keys_kept.h"

namespace qkvm::ui {

// Keyboard and mouse of the machine a window shows large (SPEC.md section 4, M3b; DESIGN.md
// section 3.8): what stands between where the keys come from (KeySource), the machine as it is
// had live (core::Live) and what the screen shows and asks (ControlView). One for each window.
// Nothing of Windows is in it.
//
// Taking. They are asked of the live, and held from when the live says so. A yes is taken only
// while whoever asked still waits for it: not after the time the machine is given to answer, not
// after the window was no longer in front, and not after anything was done in the window that
// says they went on to something else (`wentOn`). Why they are no longer waited for is said at
// once, as what it was; and a yes that comes after that is given back at once: what is typed by
// then is meant for something else.
//
// What the machine still owes, and that nothing is asked of it meanwhile. The line says no
// number with its words: a yes, or a no with or without a sentence, and nothing of what it
// answers. So what the machine was asked and has not answered is kept, in the order it was asked
// (`m_owed`): its answer to an asking that is over (a yes or a no), and its word for each giving
// back (a no without a sentence). While any of that is owed, keyboard and mouse are not asked
// for: the answer to an asking made then could not be told from the word that is owed. The
// screen is told so (`busy`), and the button that asks does nothing meanwhile. So, by how it is
// made:
// * While an asking waits, nothing is owed from before it: the first word the machine says is
//   that asking's answer. A yes takes them; a no is shown, in the machine's own sentence where
//   it has one.
// * While no asking waits, whatever the machine says is nobody's. A yes is given back at once
//   (and its word for that giving back is owed): keyboard and mouse are never held on a machine
//   with the bar saying that they are not. A no with a sentence takes the place of "did not
//   answer", where the asking's time was over; otherwise it is nothing.
// What is owed is owed no more when it has come, or when its time has run out. Which of it has
// come is told by what is known of how a machine answers:
// * In the order it was asked. So once it has said something, what was asked before that is
//   owed no more.
// * A yes answers an asking, never a giving back; and so does a no that comes with a sentence
//   (in use; sign-in refused). Assumed of every kind of live: its word for a giving back has no
//   sentence. That is so of a KVM of this PC's own (core/kvmd_live); of a server it is not known
//   here.
// * A no without a sentence is taken for the first of what is owed, whichever that is. Not told
//   apart: that it is the word for the giving back, with no answer to the asking before it ever
//   to come (a live that calls such an asking off and says the one word). The word that is
//   then still taken to be owed runs out its time.
// * A no with the code `given_up` is the machine's answer to an asking that was called off
//   from here, before it had answered: an answer like any other, and its sentence, which says
//   only what was done here, is shown nowhere. Why the asking was given up was said when it
//   was, and stands.
// A machine that says a word for every asking and every giving back, in the order it was asked
// (a KVM of this PC's own does: a giving back while keyboard and mouse were still being given
// is answered by a no with `given_up` and then the word for the giving back; a giving back by
// a window that holds nothing by the one word), owes nothing a turn of the loop after it was
// told, and Control is offered again then. One that does not (a server's line may not) has the
// button busy for the times below; and once that has lasted a moment (`owedIsSaidAfterMs`),
// the bar says why Control is not offered, in a sentence that holds whatever the giving back
// came of.
// For how long, which is how long the button can be busy. Nothing is owed for longer than
// `owedForMs` from when the machine can first have said it: its word for a giving back, from
// when they were given back; where the giving back ended an asking, from when that asking's
// time is over, since its answer comes first; and once the machine has said something, from
// then. So no longer than `answerWithinMs` and `owedForMs` from an asking (seventy seconds), and
// `owedForMs` from a giving back (five). Where the machine says yes to an asking that is over,
// that is given back, and its word for it is owed for `owedForMs` from there: a machine that
// answers each asking once says one such yes at the most, seventy-five seconds from the asking. One
// that says its words has the button back the moment they have come.
//
// Then the key source begins (anew, where it had ended by itself) and swallows; the keys that are
// down at that moment are noted; and from there on every key goes to the machine as the physical
// key it is, through one input::ControlKeys and one input::KeysDown for this one time of
// controlling:
// * The leave-key by itself gives them back; with Del it is Ctrl+Alt+Del on the machine; with
//   any other key it is nothing, and so is that key, until it was let go itself, whichever of
//   the two is let go first: that rule lives in input::ControlKeys, where the leave-key is
//   known. Here only this is added: while a modifier is kept back so, what is pressed under it
//   is kept back as under any modifier that is.
// * A key that was down when they were taken is not the machine's until it was let go here: not
//   its repeats either. Nor is a key whose press the key source could not keep from Windows
//   (Windows had it already): it was typed into this PC. And while one of the eight modifiers
//   is kept back so, the press of every other key and of every mouse button is kept back as
//   well, each until it was let go here: on the machine that modifier is up, and the key would
//   arrive there bare.
// * Every "up" of a key that is down on the machine is passed on, whatever else is so. The key
//   source tells of a key as the key it went down as, so the "up" lets go what the "down"
//   pressed, whatever Windows calls the key by then; and where it finds a key to be up whose
//   going up it did not hear, it hears no more, and everything is let go there. So what
//   is down on the machine is let go there key by key, where the keys are heard. Beneath that,
//   a net: where the key source says that none of the keys it heard go down is down any more
//   (`nothingIsDown`), every key that is still down on the machine is let go there. By then
//   each was told as gone up, so the net finds nothing to do while the key source keeps to what
//   it says of itself; it is there for where it does not. (Not the keys that are kept back
//   here: one that is held from before the keys were heard is not the key source's to know.)
// * A key that is down on the machine stays down there for as long as it is held here, however
//   long nothing is said of it: a held key is not let go for being silent. An "up" that Windows
//   left out is found where the keys are heard, by the key it was lost of, and ends the control:
//   only where Windows gives a second account of the keyboard (Raw Input), which the screen is
//   told of (`watched`, and a line in the bar where there is none).
// * A Windows key that goes down is said to the machine only when what comes next shows that it
//   is no Win+L: another key that is not L, or a button of the mouse, going down (the Windows
//   key first, then that), or the Windows key going up (down and up together). With L, Windows
//   locks this PC, whoever the two were meant for; that ends the control where the keys are
//   heard, and nothing of it has reached the machine.
//
// Giving back, in whichever way it comes about (it is asked for, the leave-key, the window no
// longer in front, no machine or another one shown large, the live gone or saying that it holds
// them no longer, the key source falling behind, hearing nothing any more or no longer looking,
// nobody at the keyboard for a time, this one ending): first everything that is down on the
// machine is let go and the live is told, where it still stands; then the key source stops
// swallowing. Where they were refused or taken away, why is kept to be shown, until they are
// next asked for or another machine is shown. Where this one finds that the key source swallows
// nothing any more and was not told why, it asks for the reason before it gives them back
// (`KeySource::tellNow`): the word for it may be on its way behind the very key that was told.
// Only where there is none (the leave-key gave the keyboard back where the keys are heard) are
// they given back with nothing said.
//
// What was kept from Windows and is still held. Windows does not know that such a key is down,
// and a hook that goes in later did not hear it: it is remembered, across takings, whatever the
// ending, and for all the windows of the program together (KeysKept). Which keys those are is
// asked of the key source as the swallowing stops (`whatIsKept`), beside what it told of: a
// press that was kept from Windows in the moment in which keys were heard no more is told to
// nobody. Taken again with a finger still on such a key, the key is kept back as any key that
// is down then, until one of two:
// * It is heard to go up. However long that takes: a key is not taken to be up for having been
//   silent.
// * It is seen to go down anew: it goes down when it has been silent for longer than a held key
//   that Windows says again ever is, with the keys heard all that time (`repeatsWithinMs`). The
//   finger had left it, where nobody heard: on one of Windows' own screens, or while nothing
//   listened. That press is the machine's. A "down" that comes sooner cannot be told from a held
//   key being said again, and leaves the key kept back, until it goes up.
// While such a key is a modifier, nothing else goes to the machine, and the bar says which key
// it is and what ends it (`note`): nothing is eaten without a word, and nothing goes bare. So of
// every modifier that is kept back, whyever: one that was down when they were taken, one that
// went to Windows, one that went down under another that is kept back or under the leave-key.
// The key source goes on listening, swallowing nothing, until every such key was let go, and
// not for ever: the memory forgets a minute after the last window gave keyboard and mouse back.
//
// Two keyboards. The key source does not tell keyboards apart: a key is the machine's key
// whichever keyboard it is pressed on, and it goes up on the machine when any keyboard lets go
// of it (ui/win/keyboard_hook, where the rule is). Nothing here knows of a second keyboard,
// and the bar says nothing of one. What whoever sits at two keyboards may meet is on the list
// for them: a modifier held on the one and tapped on the other is up on the machine from that
// tap on.
//
// A keyboard that goes. One that is unplugged, taken to another computer by a desk switch, or
// whose link is lost, with a key held, sends no "up" for it: the key would stay down on the
// machine, said again there by the machine itself, and the bar would go on saying that
// keyboard and mouse go to it. Where the key source says that a keyboard was taken away from
// this PC (`KeySource::Lost::AKeyboardWent`), keyboard and mouse are given back, which lets go
// of everything on the machine, and the bar says why. Whichever keyboard it was, and whether a
// key was held on it: nothing here knows, and nothing of a keyboard is kept. Where the key
// source is not told of keyboards that go (`isToldOfKeyboardsGoing`), the bar says while they
// are held that this is not noticed here, as it says where the keyboard is not watched. And
// one that drops out with no word of Windows' (asleep behind a receiver that stays plugged
// in) leaves the key down on the machine until it is tapped on another keyboard, or keyboard
// and mouse are given back: on the list for a person at a keyboard.
//
// What another program types into this PC (a remote-control program, a keyboard shared over the
// network, the on-screen keyboard) goes to no machine: the key source leaves it to Windows. The
// first time it says that such a key came, the bar says so for as long as they are held
// (`note`). Nothing of the key is known here.
//
// Nobody at the keyboard. Where neither a key nor the mouse has come from whoever sits here for
// a time, keyboard and mouse are given back and the bar says why. Nothing here keeps Windows
// from locking the PC, and this is not left to Windows alone.
//
// The mouse, while they are held: where the pointer is over the picture, no more often than
// sixty times a second, the last place winning; the buttons; and the wheel, a message for each
// notch. Nothing while the pointer is beside the picture. Not told apart, and not this one's to
// mend: what another program does with the mouse over the picture goes to the machine as a
// person's clicks do, and counts as somebody being there. (What another program types is told
// apart, where the keys are heard.)
//
// A click after a key. A key may be told a moment late (the key source waits for Windows' two
// accounts to be at one about a key that goes down, and a modifier that goes up stays behind a
// key that was pressed under it), and the mouse comes by another way: a button that is pressed,
// or the wheel turned, while keys are on their way waits here until every one of those was
// told, going down or going up, and goes to the machine after them, with where the pointer was.
// Shift and then a click is never a bare click, and Ctrl let go and then a click is no click
// with Ctrl. Which keys those are goes by their numbers (`KeySource::OnTheirWay`), not by how
// many keys are told: a key that was heard after the click may be told before one that was
// heard before it. What the mouse does behind that waits in its order (a button going up only
// behind its own going down: one whose press has gone out goes up at once), and the pointer's
// place is said after it. A few at the most, and none for long: where there are too many, or
// one has waited for as long as a key that is not typed late any more, they are given back as
// with keys that fell behind. Given back, whatever the reason, what waited is dropped: it never
// reached the machine. (Which came first, the key or the click, is taken from when each is seen
// here. A key that is pressed in the very moment after a click may count as before it. Set, and
// not shown by a test: that a key which was pressed before a click has been heard, where the
// keys are heard, by the time the click is seen here.)
//
// Nothing that is typed is kept, counted or written down. What is kept: which keys are down, and
// of a key that is kept back from before, when it was last said. And of the mouse, for the
// moment it waits behind a key: which button or how far the wheel, where, since when, and
// behind which of the keys that were on their way then.
//
// On the interface thread. The view, the key source and a memory that was handed in outlive it.
class MachineControl : public QObject {
    Q_OBJECT

public:
    // The times it keeps. A test makes them short.
    struct Pace {
        // How often the key source is told that the program is alive.
        int aliveEveryMs = 250;
        // How far apart two places of the pointer are said at the least (sixty a second).
        int moveEveryMs = 17;
        // The server allows 60 seconds for admission; five more cover transport. Cancellation
        // and loss of foreground still stop capture immediately and reject late grants.
        int answerWithinMs = 65'000;
        // For how long a key that was kept from Windows is remembered once no machine is
        // controlled, where this one has a memory of its own: no finger stays on a key for a
        // minute. (A memory that is handed in has its own time.)
        int lingerMs = 60'000;
        // The longest a held key that Windows says again stays silent between two sayings. From
        // memory, fairly sure: a keyboard waits a second at the most before it says a held key
        // again, and then says it at least twice a second; Windows' setting for slow keys
        // (Filter Keys) can stretch either to about two seconds. Three is beyond both. A key
        // that is taken for pressed anew though it was only said again is down on the machine
        // as it is under the finger, which is the harmless side.
        int repeatsWithinMs = 3000;
        // After how long without a key or the mouse they are given back. Fifteen minutes, as the
        // web's (IDLE_GIVE_BACK, rule 27 of its FOR-THE-DESKTOP-APP; the owner's decision of
        // 2026-10-18 for the web, which the desktop follows until the owner says otherwise): more
        // than anybody watches a machine start, or reads what scrolls by, without touching
        // anything. Control is not to be what leaves a keyboard on a server with nobody there.
        int idleAfterMs = 900'000;
        // For how long a click or a turn of the wheel waits at the most for the keys that were
        // pressed before it to be told: as long as a key waits before it is not typed late any
        // more (the key source's own second). After that it is not sent late.
        int lateAfterMs = 1000;
        // For how long a word is owed that the machine has not said: its answer to an asking,
        // from when that asking's time was over; its word for a giving back, from when they
        // were given back (and no sooner than the answer to the asking that the giving back
        // ended). As long again as it is given to answer. For that long they are not asked for
        // again, and the button that asks is busy: seventy seconds from an asking at the most, five
        // from a giving back (and five more where a yes still comes and is given back). After
        // that the word is not waited for, and what the machine says is the answer to whoever
        // asks then.
        int owedForMs = 5000;
        // After how long of owing a word the bar says why Control is not offered: longer than a
        // machine that says its words takes to say them (a turn of the loop for a KVM of this
        // PC's own, the way there and back for a server's), so that the bar does not flicker
        // with a sentence at every giving back; and a small part of the time the button is busy
        // where no word comes.
        int owedIsSaidAfterMs = 500;
        int textReleaseWithinMs = 3000;
    };

    // With a memory of its own of what is held: for a program of one window.
    MachineControl(ControlView& view, KeySource& keys, QObject* parent = nullptr);
    MachineControl(ControlView& view, KeySource& keys, Pace pace, QObject* parent = nullptr);
    // With the one memory that the windows of a program have together.
    MachineControl(ControlView& view, KeySource& keys, KeysKept& kept, QObject* parent = nullptr);
    MachineControl(ControlView& view, KeySource& keys, KeysKept& kept, Pace pace, QObject* parent = nullptr);
    ~MachineControl() override;
    MachineControl(const MachineControl&) = delete;
    MachineControl& operator=(const MachineControl&) = delete;
    MachineControl(MachineControl&&) = delete;
    MachineControl& operator=(MachineControl&&) = delete;

    // The machine its window shows large, by the wall's name for it, is had live through this
    // one from now on; nothing: it is not had live, or none is shown. Said while the one before
    // still stands: what is down there is let go there.
    void have(core::Live* live, const QString& machine);
    // The line the machine is had live by ended by itself, and this is why, in the words of
    // whoever gave it: said by whoever holds the live, before it hands in the next one or none.
    void lineEnded(const QString& message);
    // Whoever sits here may act on machines, and the wall is not only shown.
    void setAllowed(bool allowed);
    void setTakeoverAllowed(bool allowed);
    void askTakeover();
    void answerTakeover(bool yes);
    void setTextAllowed(bool allowed)
    {
        m_textAllowed = allowed;
        tell();
    }
    // Its window has the keyboard and can be seen. Until that is said, it does not.
    void setInFront(bool inFront);

    bool isOn() const { return m_on; }
    bool isAsking() const { return m_asking; }
    bool textHoldsControl() const { return m_textHeld; }
    // Text is composed locally while this window retains ownership. Only confirmation
    // releases it; the action waits for that release and is never replayed.
    void pauseForText(const QString& machine);
    void cancelText(bool resume = true);
    using TextStart = std::function<void(std::function<void(bool)>)>;
    // Of every action that types or presses keys on `machine` (a text, keys, a saved action): where
    // this window holds that machine's keyboard and mouse, they are given back, and `start` waits for
    // the machine's word for that (which a KVM of this PC says once what was let go of has landed),
    // and they are asked for again after it went well. Not confirmed in time, or refused: `refused`. Refused as well
    // where something may still be down on that machine (mayBeDownOn), said before the word or since: nothing is sent
    // until keyboard and mouse were taken anew there.
    void textAfterRelease(const QString& machine, TextStart start, std::function<void()> refused);
    // Whether something pressed from here may still be down on `machine` (Live::leftDown), until keyboard and mouse
    // are taken anew there.
    bool mayBeDownOn(const QString& machine) const { return machine == m_machine && m_mayBeDown; }

    // What the view asks, and what it is to the machine.
    void take();
    void giveBack();
    void ctrlAltDel();
    void setLocalInput(bool local);
    void tapKeys(const QList<int>& usages);
    // Whoever sits here went on to something else while keyboard and mouse were asked for: they
    // pressed a key in the window, or opened something in it.
    void wentOn(bool byAKey);
    // Where in the picture the pointer is: each 0.0 to 1.0 over it. Any other place is beside
    // it: nothing is said of the pointer there, and no button is pressed and no wheel turned.
    void pointerAt(double x, double y);
    void setRelative(bool relative);
    void followPointerMode();
    void pointerBy(int dx, int dy);
    // A button, 1 to 5.
    void button(int number, bool down);
    // The wheel, as Qt says a turn (QWheelEvent::angleDelta): 120 for a notch.
    void wheel(int angleX, int angleY);

private:
    // Why keyboard and mouse that were asked for are waited for no longer.
    enum class GaveUp { No, TimeOver, Otherwise };
    // What whoever sits here can do about why they were not taken: nothing that is said with
    // it; or ask for them again, which is said as it can be done at that moment.
    enum class Remedy { None, AskAgain };
    // Whether the machine still owes its answer to an asking that ends.
    enum class Answer { Owed, Came };
    // A word that the machine may still say and that is nobody's by then: its answer to an
    // asking that is over, or its word for a giving back.
    struct Owed {
        bool anAnswer = false;
        // Until when it is owed, by this one's clock.
        qint64 untilMs = 0;
    };
    // The words that are owed, in the order they were asked for: a row of fixed size, so that
    // whatever ends the holding (this one's own end among it) takes no memory for noting them.
    class OwedWords {
    public:
        // How many are kept as owed at the most.
        static constexpr std::size_t most = 8;

        bool isEmpty() const noexcept { return m_count == 0; }
        // Which of them is the first answer to an asking; `most`: none is.
        std::size_t firstAnswer() const noexcept;
        // Until when the one of them that runs out first is owed. (Asked while one is.)
        qint64 firstOverMs() const noexcept;
        // One more is owed. Where there is no room, the one that was asked for first is owed no
        // more: it is the one least likely to come still.
        void add(const Owed& word) noexcept;
        // Owed no more: the first so many; and those that are owed for no longer than until now.
        void dropFirst(std::size_t count) noexcept;
        void dropOverdue(qint64 nowMs) noexcept;
        // None of them is owed for longer than until then.
        void noLongerThan(qint64 untilMs) noexcept;
        void clear() noexcept { m_count = 0; }

    private:
        std::array<Owed, most> m_words{};
        std::size_t m_count = 0;
    };
    // What the mouse did while keys that were heard before it were still to be told.
    struct Waited {
        enum class What { Press, Release, Wheel, Move };
        What what = What::Press;
        // The button; or how many notches the wheel turned, away from the hand and to the right.
        int number = 0;
        int away = 0;
        int right = 0;
        // Where the pointer was; behind which keys it waits (those that were on their way then:
        // the number of the last of them, and how many of them are still to be told); and since
        // when it waits.
        QPoint place;
        quint64 behind = 0;
        int keysBefore = 0;
        qint64 sinceMs = 0;
    };

    MachineControl(ControlView& view, KeySource& keys, KeysKept* shared, Pace pace, QObject* parent);

    bool isPossible() const;
    bool mayTakeOver() const;
    void requestControl(const QString& takeover);
    void cancelTakeover();
    // What the live says of keyboard and mouse: whether it holds them, and where not, a code
    // and a sentence for why.
    void answered(bool on, const QString& code, const QString& message);
    // The machine was told that keyboard and mouse are given back, or asked for no longer: its
    // word for that is owed, and with `ofAnAsking` its answer to the asking that this ended,
    // which had so long left of its time.
    void oweForAGivingBack(bool ofAnAsking, int restOfTheAskingMs);
    // What is owed for longer than it is waited for is owed no more.
    void forgetWhatIsOverdue();
    // Whether a word is owed is looked at: from when one is, the time runs after which the bar
    // says so; and where none is any more, that is over.
    void lookWhetherAWordIsOwed();
    // The machine said something that answers an asking: the first answer that is owed, and
    // whatever is owed before it, is owed no more. False: no answer was owed.
    bool takeTheAnswerThatIsOwed();
    // The machine said something while nobody waited, and what it owes was held against it:
    // what is still owed is owed from here, and the screen is told.
    void theMachineSpoke();
    // What is owed is looked at again when the first of it runs out: the button that asks is
    // busy until then, and no longer. And from when a word has been owed for a moment, the bar
    // says why Control is not offered.
    void waitForWhatIsOwed();
    // Why Control is not offered while the machine owes a word, as a sentence; empty: nothing
    // is owed, or not for long enough to be said.
    QString whyControlIsNotOffered() const;
    // The time the machine was given for the asking is over.
    void answerTimeIsOver();
    // The key source says that keys are heard no more, and why, as a sentence; with what can
    // be done about it, where something can.
    void heardNoMore(const QString& why, Remedy remedy = Remedy::None);
    void begin();
    void releasePressed();
    void keyCame(const input::Pressed& key, bool down, bool swallowed, quint64 number);
    // What a key that came is to the machine, and to what is remembered of the keys.
    void takeKey(const input::Pressed& key, bool down, bool swallowed);
    // The key source says that none of the keys it heard go down is down any more.
    void nothingIsDown();
    // A button goes down, goes up, or the wheel turned, now: `at`, where the pointer was then,
    // for what waited; nothing: where it is.
    void press(int number, const std::optional<QPoint>& at);
    void release(int number, const std::optional<QPoint>& at);
    void turn(int away, int right, const std::optional<QPoint>& at);
    // Whether what the mouse does now has to wait: keys that went down are still to be told,
    // or something of the mouse waits already.
    bool theMouseWaits() const;
    void holdBack(Waited what);
    // What waited and need not wait any longer goes to the machine, in its order.
    void passTheMouse();
    void sayWhereItWas(const std::optional<QPoint>& at);
    // A key goes down that is kept back as held from before: where it has been silent for longer
    // than a held key that is said again ever is, it is seen to go down anew, and is kept back
    // no longer.
    void lookWhetherItGoesDownAnew(std::uint16_t usage);
    // The key source has this key as one that it kept from Windows and that is still down.
    bool isKeptWhereTheKeysAreHeard(const input::Pressed& key) const;
    void pass(std::uint16_t usage, bool down, bool isL);
    // To the machine, where that changes what is down there.
    void say(std::uint16_t usage, bool down);
    // The Windows key that waited to be said is said now: what comes is no Win+L.
    void sayTheWindowsKey();
    bool holdsBackAModifier() const;
    // Somebody is at the keyboard or the mouse, now.
    void touched();
    void sayThePlace();
    void sayRelative();
    void moveRelative(QPoint delta);
    // The one way out of holding or asking, for every reason there is. `remedy`: what can be
    // done about the reason. `answer`, of an asking that ends: whether the machine has answered
    // it by now.
    void stop(const QString& problem, Remedy remedy = Remedy::None, Answer answer = Answer::Owed,
              bool keepOwnership = false);
    void textReleaseAnswered(bool on, const QString& code, const QString& message);
    void startText();
    // Why nothing is heard any more, as a sentence.
    static QString sentenceFor(KeySource::Lost why);
    // Why they were not given, or were taken away, with what can be done about it at this
    // moment.
    QString problemAsItIsSaid() const;
    // The key source is ended once nothing is held or asked for and every key that was kept
    // from Windows was let go, or forgotten.
    void listenNoLongerThanNeeded();
    // What is to be known while they are taken.
    QString note() const;
    void tell();

    ControlView& m_view;
    KeySource& m_keys;
    Pace m_pace;
    // The keys that were kept from Windows and not heard to go up: its own, or the one that the
    // windows of the program have together.
    std::unique_ptr<KeysKept> m_own;
    KeysKept& m_kept;
    QPointer<core::Live> m_live;
    quint64 m_targetTurn = 0;
    QString m_machine;
    QSize m_picture;
    bool m_allowed = false;
    bool m_takeoverAllowed = false;
    std::optional<core::Live::ControlOwner> m_takeover;
    bool m_textAllowed = true;
    bool m_inFront = false;
    bool m_asking = false;
    bool m_takingOver = false;
    bool m_on = false;
    // They were asked for and are waited for no longer: a yes that still comes is given back.
    GaveUp m_gaveUp = GaveUp::No;
    // The words the machine still owes and that are nobody's, in the order it was asked: what it
    // says while nobody waits is held against those, and while any is owed nothing is asked of
    // it. A few at the most.
    OwedWords m_owed;
    // The key source said that a key came which another program typed, since they are held.
    bool m_typed = false;
    bool m_localInput = false;
    quint64 m_inputTurn = 0;
    // The key source was begun and not ended.
    bool m_listens = false;
    // The memory was told that this one controls a machine.
    bool m_counted = false;
    // This one is ending: its key source ends with it, whatever is still held.
    bool m_ending = false;
    // Windows' second account of the keyboard is there, as the key source last said. And so is
    // Windows' word for a keyboard that is taken away.
    bool m_watched = true;
    bool m_goingIsTold = true;
    // Why they were not given, or were taken away; and what can be done about it.
    QString m_problem;
    // Whoever gave the live said that something may still be down on this machine
    // (Live::leftDown): shown until keyboard and mouse are taken anew, or another machine is
    // had. The word may come seconds after they went, in a time of holding them that it is
    // not of: it is said then all the same, and changes nothing else.
    bool m_mayBeDown = false;
    Remedy m_remedy = Remedy::None;
    // For this one time of controlling.
    std::optional<input::ControlKeys> m_control;
    std::optional<input::KeysDown> m_down;
    // The keys that are down on the machine, by their USB numbers (what `m_down` holds, to be
    // asked for one key).
    std::set<std::uint16_t> m_there;
    // The keys that are kept back until each was let go here.
    std::set<std::uint16_t> m_keptBack;
    // The keys that are kept back as held from before they were taken, each until it was heard to
    // go up or seen to go down anew: with when it was last said since, or when they were taken.
    std::map<std::uint16_t, qint64> m_fromBefore;
    // A Windows key that went down and is not said to the machine yet.
    std::optional<std::uint16_t> m_windowsKey;
    // The buttons that are down on the machine.
    std::set<int> m_buttons;
    // Where the pointer is, where that is still to be said; whether it is over the picture; and
    // what the wheel has turned short of a notch.
    std::optional<QPoint> m_place;
    bool m_over = false;
    bool m_relative = false;
    bool m_textHeld = false;
    bool m_textRunning = false;
    bool m_textStarted = false;
    bool m_textResume = false;
    quint64 m_textTurn = 0;
    TextStart m_textStart;
    std::function<void()> m_textRefused;
    QTimer m_textDeadline;
    bool m_captured = false;
    QPoint m_delta;
    QPoint m_turned;
    // Where the pointer is over the picture, said or not; nothing: beside it. And where the
    // machine was last told that it is.
    std::optional<QPoint> m_at;
    std::optional<QPoint> m_said;
    // What the mouse did and is still to be sent, behind keys that are still to be told.
    std::vector<Waited> m_mouse;
    // When a key or the mouse last came from whoever sits here.
    qint64 m_touchedAtMs = 0;
    QElapsedTimer m_clock;
    QTimer m_alive;
    QTimer m_moves;
    QTimer m_answer;
    // Until the first of what the machine owes runs out.
    QTimer m_owing;
    // Until the machine has owed a word, without a break, for long enough to be said; and that
    // it has.
    QTimer m_owedIsSaid;
    bool m_owedForLong = false;
};

} // namespace qkvm::ui
