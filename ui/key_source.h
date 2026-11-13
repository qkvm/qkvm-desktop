#pragma once

#include <vector>

#include <QObject>

#include <qkvm/input/keys.h>

namespace qkvm::ui {

// Where keys come from while a machine is controlled: every key of this PC's keyboard, as the
// physical key it is, for as long as it is listened for. Behind it Windows' own way of hearing
// every key (ui/win/keyboard_hook), or in a test one that is fed by hand.
//
// Nothing is heard before begin() and after end(). While it swallows, the keys that come are kept
// from every other program, and from this one's own screens. It stops swallowing by itself,
// whatever it was told, when the leave-key supplied to begin() was pressed and let go with no
// other key between: that is decided where the keys are heard and not by whoever listens here,
// who hears of that key as of any other and then finds swallows() false.
//
// Whether the keyboard that is plugged in has the leave-key cannot be asked: Windows knows which
// keys the keyboard's layout names, and every usual layout names a right Ctrl, also for a small
// keyboard that has none. So nothing here answers that question, and whoever shows the way back
// names the button for it beside the key, always.
//
// What it tells. While it was asked to swallow, every key that goes down and every key that goes
// up, each press with whether it was kept from Windows: one that was not (Windows had the key
// already, from before the listening began) is Windows' and not the machine's. While it was
// not asked to swallow, only that a key which it had kept from Windows itself went up: whoever
// listens waits for those to be let go, and ends the listening then. Of any other key nothing is
// told then. (With the word that keys are heard no more, what still waited to be told is told
// where it is a key going up, and dropped where it is one going down: which keys it has kept from
// Windows and are still down is asked of it, `whatIsKept`.) So a key of the PC passes through
// here only while a machine is controlled, and after that until the fingers are off what was
// pressed for it. And a key that goes down is told in the order the keys went down in, and may be
// told a moment late: where Windows gives a second account of the keyboard, not before the two
// are at one about it. A key that goes up is told at once where its going down was told (letting
// go needs nobody's word), and so may come before a key that went down before it and still
// waits; never before its own going down, and one of the eight modifiers never before any key
// that waits: that key was pressed under it. A key that is down and is only said again is told
// at once as well, or not at all while its own going down still waits.
//
// So what is told is not told in the order it was heard in. For whoever has something to send
// after the keys (a click): each key that is on its way has a number, in the order the keys were
// heard in, and is told with it; which number was given last, and how many are on their way, is
// asked of it (`onTheirWay`). A click that came then goes to the machine once as many keys with a
// number no higher than that one were told.
//
// A key is told as the key it went down as, for as long as it is down: said again, and going up,
// whatever Windows calls it by then (it names some keys by what else is held). So the going up of
// a key lets go exactly what its going down pressed. And where a key that was heard to go down
// is found to be up with its going up not heard here, nothing is heard any more: whoever
// listens hears of every key that it was told went down that it went up, or that nothing is
// heard any more (and then lets go of everything).
//
// Keyboards are not told apart: a key is the same key whichever keyboard it is pressed on, and
// it is told as gone up when any keyboard lets go of it. So a key that is held on two keyboards
// at once goes up when the first of them lets go, and is told as going down again where the
// other says it again.
//
// A keyboard that goes. One that is unplugged, taken to another computer by a desk switch, or
// whose link is lost, with a key held, sends no "up" for that key: told as down, it would stay
// down for whoever listens. So where Windows says that a keyboard was taken away from the PC,
// while keys are swallowed, nothing is heard any more (`Lost::AKeyboardWent`): whichever
// keyboard it was and whatever was held on it, since none is told apart and nothing of one is
// kept. A keyboard that comes ends nothing. Whether Windows says so here is asked of it
// (`isToldOfKeyboardsGoing`); where it does not, or where a keyboard drops out and Windows has
// no word for it (one that sleeps behind a receiver which stays plugged in), such a key stays
// down until it is pressed and let go elsewhere, or nothing is heard any more.
//
// What another program types (a remote-control program, a keyboard shared over the network, the
// on-screen keyboard) is left to Windows and told to nobody, as a key. That such a key came
// while keys were swallowed is said, once for each time they are (`typedByAProgram`): nothing of
// the key itself.
//
// It never sends this PC a key, a click or anything else: it listens.
//
// On the interface thread. A key is handed on and forgotten: none is kept, counted or written
// down. What is kept is which keys are down; and, only where Windows gives a second account to
// hold it against and only until that account has told of the key, when it was heard. Nothing
// else, and on a PC without a second account nothing of when or how often.
class KeySource : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    // The keys that were heard and are not told yet, at one moment: the number of the one that
    // was heard last, and how many they are. Numbered from 1 in the order they were heard in, and
    // from 1 again whenever none is on its way: the numbers say the order of what is on its way
    // at this moment, and nothing of how much was typed.
    struct OnTheirWay {
        quint64 last = 0;
        int count = 0;
    };

    // Why nothing is heard any more.
    enum class Lost {
        // Windows shows a desktop of its own (the lock screen, Ctrl+Alt+Del, a question about
        // administrator rights), or the session was locked or taken over from afar: the keys go
        // to nobody here.
        AnotherDesktop,
        // Windows' other account of the keyboard told of a key that was not heard here. Windows
        // took the listening away, which it does without a word to a listener that was slow; or
        // it left out a key; or something on this PC kept the key from this listener (another
        // program's listening, one of Windows' own key helpers). Which, is not known here.
        NotHandedKeys,
        // The other way round: a key was heard here that Windows' other account, which had told
        // of this keyboard, did not tell of. That account is told to this program no more
        // (another window of it, or something loaded into it, asked for it since), or something
        // on this PC makes or changes keys. Which, is not known here.
        NotToldOfKeys,
        // Both at once: the two accounts told differently of what may be one key.
        ToldDifferently,
        // The keys were pressed that Windows itself acts on, whoever they were meant for: L
        // under a Windows key locks this PC, Del under Ctrl and Alt brings Windows' own screen.
        KeysOfWindows,
        // Whatever listens no longer looks at what comes: its thread hangs, or has ended.
        NotLooking,
        // Windows said that a keyboard was taken away from this PC: unplugged, switched to
        // another computer, its link lost. A key that was held on it will never be heard to
        // go up, and which keys those are is not known here.
        AKeyboardWent,
        // Windows' other account of the keyboard, which had told of nothing, began to tell
        // while keys were swallowed (a keyboard that it tells of was plugged in), and a key it
        // told of in its first moment found no hearing: one that was heard before any hearing
        // was noted, or one that was not heard at all. Which, is not known here.
        BeganToTell,
    };
    Q_ENUM(Lost)

    // Keys are listened for from now on. Said at every taking: where it listens already it goes
    // on, and where its listening had ended by itself, or was lost, it begins anew. False where
    // they cannot be listened for: then nothing listens.
    virtual bool begin(std::uint16_t leaveKey = input::usageRightCtrl) = 0;
    // And no longer. Nothing where it was never begun.
    virtual void end() = 0;
    // The keys that are physically down this moment, as far as that can be known: those that were
    // heard go down and not up since it began (anew), kept from Windows or not, and those Windows
    // has as down from before. A key that went down before that and was kept from Windows then is
    // known to nobody but whoever listened.
    virtual std::vector<input::Pressed> whatIsDown() const = 0;
    // The keys it has kept from Windows, as presses of their own, and has not heard go up since:
    // Windows does not know that they are down. Whether it has told of each or not: a press that
    // was kept from Windows in the moment in which keys were heard no more is told to nobody as
    // a press, and is down all the same. Asked by whoever stops the swallowing, right after.
    virtual std::vector<input::Pressed> whatIsKept() const = 0;
    // Whether Windows' second account of the keyboard is told to it, so that a listening which
    // Windows took away, or a key it left out, would be noticed. False where this PC gives none
    // (it was refused; a remote desktop where none comes): then that is not noticed here.
    virtual bool isWatched() const = 0;
    // Whether Windows tells it when a keyboard is taken away from this PC, so that a key which
    // was held on that keyboard is not left down for whoever listens (`Lost::AKeyboardWent`).
    // False where this PC gives no such word (it was refused, or there is no second account to
    // ask it with): then a keyboard that goes is not noticed here.
    virtual bool isToldOfKeyboardsGoing() const = 0;
    // Whether the keys that come are kept from every other program.
    virtual bool swallows() const = 0;
    virtual void setSwallowing(bool swallowing) = 0;
    // Whoever listens says that it is alive, every so often while keys are swallowed: a program
    // that hangs is not left holding the keyboard of whoever sits at the PC.
    virtual void stillThere() = 0;
    // The keys that were heard, going down or up, and are not told yet, at this moment: each will
    // be told with its number, or the listening ends.
    virtual OnTheirWay onTheirWay() const = 0;
    // What waits to be told is told now, before this returns: keys, and why nothing is heard any
    // more. For whoever finds that nothing is swallowed and was not told why. Asked from inside
    // the telling of a key as well: then what was to be told behind that key is told there and
    // then, the reason among it, and not a second time afterwards.
    virtual void tellNow() = 0;

signals:
    // `swallowed`, of a key that went down: it was kept from Windows, as a press of its own.
    // `number`: which of the keys that were on their way it is (`OnTheirWay`); 0: not known,
    // which counts as heard before everything.
    void keyChanged(const qkvm::input::Pressed& key, bool down, bool swallowed, quint64 number = 0);
    // Said right after a key that went up: of the keys that were heard to go down, none is down
    // any more, and nothing waits to be told. (Not said of every such moment: only where it is
    // known for sure.) Each of those keys was told as gone up before this: it adds nothing where
    // whoever listens has kept to what it was told, and is there for where it has not.
    void nothingIsDown();
    // A key came that another program typed, while keys were swallowed: once for each time they
    // are. It went to Windows, and is told to nobody as a key.
    void typedByAProgram();
    // Keys came that were not told, or would have been told too late to be typed: whoever
    // listens lets go of everything. Nothing more is swallowed until it begins anew.
    void fellBehind();
    // Keys are not heard any more, and what was let go since may not be known: the same.
    void lostHearing(qkvm::ui::KeySource::Lost why);
};

} // namespace qkvm::ui
