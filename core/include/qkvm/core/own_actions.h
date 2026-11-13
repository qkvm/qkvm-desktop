#pragma once

#include <optional>

#include <QList>
#include <QObject>
#include <QString>

#include <qkvm/base/result.h>
#include <qkvm/core/exchange.h>
#include <qkvm/core/secrets.h>
#include <qkvm/core/server_types.h>
#include <qkvm/core/settings.h>
#include <qkvm/core/wiped.h>

namespace qkvm::core {

// What a saved action of this PC's own wall is held to: the QKVM server's rules for one, one for
// one (CustomActionIn, check_action and fill_action in ../app/qkvm/server.py), for what is typed
// into a box and for what the settings' file says alike.
namespace action {

// The most the server reads as keys at all, whatever kind the action is of. (Eight keys by their
// longest names are fewer signs than this: it only bounds what is gone through.)
inline constexpr qsizetype longestKeysAsTyped = 160;
// More edits than this no action of the settings' file has had: a count beyond it is none this
// program wrote. It writes none: an action whose count is within two of it is not changed again
// (OwnActions::change), since a change counts one up, or two where it cannot be written down.
inline constexpr int mostEdits = 1'000'000'000;

// What is kept of an action's name as it was typed: one line that reads as it is written, without
// the blanks around it, of sixty signs at the most, and not nothing. The one rule the program has
// for such a line (kvm::neededLineFrom, kvm_adding.h), and so counted as that counts: by the
// places a text takes here, of which a sign from far up takes two. Refused as "invalid", in the
// server's sentence ("name: " and the rule), never with what was typed.
base::Result<QString> nameFrom(const QString& typed);
// Whether these are keys an action presses: up to eight, by the names a KVM knows them by, with
// commas between ("ControlLeft,AltLeft,Delete"). The one rule the program has for what a KVM is
// told to press (kvmd::callFor), asked here.
bool areKeys(const QString& keys);
// Whether a KVM's keyboard types this as one text: there is something of it, and no more than
// two thousand signs (a sign from far up is one). The one rule the program has for what a KVM is
// told to type (kvmd::callFor), asked here of the text by itself. (Enter after it is one sign
// more to a KVM: a text of two thousand with Enter is saved, as the server saves it, and is
// refused by the KVM's driver when it is run, as the server's is.) Of what that rule makes of the
// text on its way, the bytes it hands back are written over here; the copies it makes inside, to
// count the signs, are not: they are the driver's own way, as they are at a run.
bool isTypedAsOne(const QString& text);
// Whether this names a writing of a text as the list names one (OwnAction::writing): a place,
// "a" or "b", a colon, and sixteen of 0-9 and a-f.
bool namesAWriting(const QString& writing);
// Whether this is an action as the program keeps one, each field by the rules above: an id of
// eight of 0-9 and a-f; a name that is what nameFrom keeps of it; for one that presses, keys that
// are keys, no Enter and no writing of a text; for one that types, no keys, and a writing that
// is written as one or none; a count of edits from nothing to `mostEdits`. What the settings'
// file is held to, entry by entry.
bool isOne(const OwnAction& action);
// An action as a wall hands it out (server_types.h): never anything of its text.
SavedAction asHandedOut(const OwnAction& action);

} // namespace action

class LocalWall;

// The saved actions of this PC's own wall (SPEC.md section 4, M3a): things that were saved once to
// be done to any KVM of the wall, each either keys pressed together or a text that is typed (with
// Enter after it or not), some marked as dangerous. What a QKVM server keeps for its wall
// (ActionRecord in ../app/qkvm/store.py), kept here by the server's rules, and refused in the
// server's codes and sentences where the server has them.
//
// A class of its own, which the wall holds (LocalWall::actions): the list and its texts need no
// KVM, no line and no turn of the loop, and are tried without any of those; and every way to a
// text is in this one place.
//
// The list is in the settings (Remembered::actions), beside the KVMs. The text an action types
// is a secret, very often a password: it is in the secret store and nowhere else. It is in no
// settings file, no log, no sentence and no answer, and nothing here hands it out again once it
// is saved, but for two doors: forOneRun(), which is the wall's alone and which it goes through
// to have a text typed; and textsHandedOutForASealedFile(), named as what it is, for the day
// the wall is written to a file that is sealed.
//
// How a text is kept. One entry of Windows' store holds 2560 bytes, and a text may be two
// thousand signs, which is up to eight thousand bytes: so a text is kept in as many entries as
// it takes, `partBytes` of it in each. One saving of a text is a WRITING, with a mark of its own
// that is made anew each time (sixteen of 0-9 and a-f), in one of two PLACES, "a" or "b". The
// entries of a place are named "action:", the action's id, a colon, the place and the part's
// number ("action:0a1b2c3d:a1", "...:a2"). Each entry is four lines' worth, each line but the
// last ended by a line feed:
//     QKVM-ACTION-1
//     the action's id
//     the writing's mark, a blank, the part's number, a stroke, how many parts the writing has
//     the bytes of that part
// The first line says what this is, and which form of it. The bytes are all that follows the
// third line feed, whatever is in them: a text is taken as it was typed, line feeds and all, and
// its bytes are its signs in UTF-8, sign for sign (one that begins with U+FEFF begins with it
// when it is typed, too). Every part but the last is full.
//
// Which writing is the action's text, the list says (OwnAction::writing: the place and the mark).
// What the store holds in another form, for another action, or of another writing than the list
// names, is as none; so is a writing of which a part is missing or is not what its number says,
// and one that is no text, or more than an action types. Then the action has no text: it is on
// the list, and says when it is run that its text is not kept and that nothing was typed. So it
// is, too, where the settings came to this PC without the store (a file that was copied), and
// where an older file of the settings was put back: a text is typed only by the action as it
// was saved with that text.
//
// A text is kept whole or not at all, and what cannot be written down did not happen:
// * A new text is written into the place the action's text is NOT in, entry by entry. Where one
//   entry cannot be written, none of that writing stays, and the text the action had is as it
//   was: it was never touched.
// * Then the list is written, which names the new writing. Where that cannot be done, the new
//   writing is taken out again, and the action is what it was.
// * Only then is the writing before forgotten.
// So a change that fails at any step leaves the action with the text it had, and nothing has to
// be put back for that. What may be left behind where the program ends between two of these
// steps is one writing in the place that the list does not name: it is nobody's text, it is
// written over by the next text that is saved for that action, and forgotten with the action.
// NOT so of an adding or a removal that the program's end cuts off, or of a removal whose
// forgetting Windows does not do (it says nothing of that): then a text stays in the store under
// an id that no action has, and nothing takes it out; and so does the text of an action that
// the settings' file has and that is left out where the file is read. None of them can be run
// (the id and the writing's mark are looked at), and none is shown; that they stay at all is
// open, as it is for a KVM's password (DESIGN.md section 11).
//
// On the interface thread. Every call has done what it does when it comes back: the list is
// written down there and then (the settings are one file, written whole).
//
// The settings and the secret store outlive it.
class OwnActions : public QObject {
    Q_OBJECT

public:
    // What was typed into the box for one action.
    struct Typed {
        QString name;
        // It types `text`; otherwise it presses `keys`.
        bool types = false;
        QString keys;
        // Taken as it is typed, the blanks around it too. Empty on a change of an action that has
        // a text: that text stays.
        QString text;
        bool enter = false;
        bool danger = false;
    };

    // A text is kept in parts of so many bytes, and in so many parts at the most: two thousand
    // signs of four bytes each. (What stands before a part in its entry is some fifty bytes.)
    static constexpr qsizetype partBytes = 2500;
    static constexpr int mostParts = 4;

    OwnActions(Settings& settings, SecretStore& secrets, QObject* parent = nullptr);

    // The actions, in the order they were added in.
    const QList<OwnAction>& all() const;
    // The same as a wall hands them out, which is what a box that manages them shows: never the
    // text one types. (`changed` of each is what a change, a removal and a run are to name.)
    QList<SavedAction> handedOut() const;

    // Saves a new action, last on the list, under an id that is made for it, and hands it back as
    // it is handed out. Refused, each in the server's code and sentence where the server has one:
    // * "too_many": five hundred are kept (`mostSavedActions`), and no more. (The program's own:
    //   the server keeps as many as are saved.)
    // * "invalid", by the rules of what is typed in, in the server's order: the name
    //   (action::nameFrom); keys of more than 160 signs, and a text of more than two thousand,
    //   whichever kind the action is of; a text with half a sign in it, which no keyboard types
    //   (the program's own); for one that presses, keys that are none ("Choose at least one
    //   key"); for one that types, no text ("Enter the text to type").
    // * "not_saved": its text could not be kept, or the list could not be written down. Then
    //   nothing of it is anywhere.
    // What is kept of what was typed: the name as nameFrom keeps it; the keys of one that
    // presses, and none of one that types; Enter only of one that types; the text of one that
    // types, as it was typed, in the store.
    //
    // Two actions may have one name, as on the server: they are told apart by their ids.
    base::Result<SavedAction> add(const Typed& typed);
    // Import adds by name, never replaces. Text actions must carry their text.
    base::Result<SavedAction> takeFromAFile(const exchange::Action& action);
    bool hasName(const QString& name) const;
    static QString nameForImport(const QString& name);
    // Changes an action to what was typed, in its place on the list, and counts its `changed` up
    // by one. `lookedAt` is the `changed` of the state that was looked at when the box was
    // filled. Refused as add() refuses what is typed, and:
    // * "not_found": it is not on the list (the server's "Unknown action").
    // * "changed": it is in another state than was looked at (another window changed it, or
    //   this box was left open over a change): look again before changing it. (The program's
    //   own rule; the server takes any change.)
    // * "invalid", "Enter the text to type": no text was typed, and the action has none that
    //   could stay: it pressed until now, or the list names no writing for it. (Where the list
    //   names one that the store does not hold whole, the change is taken, as the server takes
    //   it, and the action says when it is run that its text is not kept.)
    // * "invalid", that it was changed as often as an action can be: its count of edits is
    //   within two of `action::mostEdits` (only a settings' file made by hand comes so far).
    // An empty text keeps the text that is saved: nothing of the store is touched. One that is
    // given is saved as a new writing, and the one before is forgotten once the list names the
    // new one. An action that presses from now on has its text forgotten.
    //
    // A change whose new text could not be kept ("not_saved") leaves everything as it was. One
    // whose list could not be written down ("not_saved") is taken back whole, its new text with
    // it; and the action then stands under a number nobody was shown: two higher than it was,
    // as the server does it. (Neither the number it had, under which somebody may hold it, nor
    // the next, which is what the change that was taken back would have been shown under, and
    // what the next change would be.) `changed()` is said for that: whoever holds the list
    // holds a number that is no longer the action's. So a box that is told "not_saved" reads
    // the action's number anew before it tries again.
    //
    // (The id by value, here and below: whoever names an action by the list's own text for it
    // names something that is gone once the list is another.)
    base::Result<SavedAction> change(QString id, int lookedAt, const Typed& typed);
    // Takes an action off the list and forgets its text. Refused: "not_found" and "changed", as
    // for a change; "not_saved" where the list could not be written down, and then the action
    // and its text are as they were.
    base::Result<void> remove(QString id, int lookedAt);

    // The action as it is now, where it is still in the state that was looked at: what whoever
    // runs one asks first. Refused in the server's codes and sentences: "not_found" ("That saved
    // action no longer exists"), and "changed" where it was edited since (its name, set off, and
    // "was changed since you opened this list. Look at it again before running it."). Nothing of
    // the store is read for this.
    base::Result<OwnAction> asLookedAt(const QString& id, int lookedAt) const;

    // What an action is to a KVM's driver for one run: its keys to press, or its text to type,
    // with Enter where it has that. The text was read from the store for this run alone, and is
    // written over when this goes; so it is no thing to copy, and whoever holds it lets it go as
    // soon as the driver has been handed the action.
    //
    // As far as that reaches: the bytes that were read from the store are written over (those
    // this program held: what Windows' own reading made of them on its way is Windows'), and
    // so is the text that was made of them here. A KVM's driver takes an action as a text of
    // Qt's (Action::text), and what it makes of that is not written over by anybody: the copy
    // it makes where Enter is put behind the text and the one it counts the signs of, the bytes
    // it sends, and what Qt's network code holds of them until they are sent.
    class ForOneRun {
    public:
        explicit ForOneRun(Action action);
        ForOneRun(const ForOneRun&) = delete;
        ForOneRun& operator=(const ForOneRun&) = delete;
        ForOneRun(ForOneRun&& other) noexcept;
        ForOneRun& operator=(ForOneRun&&) = delete;
        ~ForOneRun();

        const Action& action() const noexcept { return m_action; }

    private:
        Action m_action;
    };
    // THE OTHER DOOR to the texts (the first is the wall's alone: forOneRun, below), for a file
    // of the wall that is sealed with a passphrase, and for nothing else: the actions as a file
    // holds them (exchange.h), each that types WITH ITS TEXT. Named as what it is so that nobody
    // calls it for anything else: a plain file holds no text, and nothing that is shown or
    // logged is made of what this hands back. (Nothing calls it yet.) `withoutText`: the
    // actions that type and whose text is not kept, by their place on the list counted from 0;
    // they are handed back without, and whoever writes the file says so beside it. The texts
    // are the caller's from then on, and are written over by nobody.
    struct WithTheirTexts {
        QList<exchange::Action> actions;
        QList<qsizetype> withoutText;
    };
    WithTheirTexts textsHandedOutForASealedFile();

    // The name one part of an action's text is kept under: `place` is 'a' or 'b', `part` counted
    // from 1.
    static QString secretOf(const QString& id, QChar place, int part);

signals:
    // The list is not what it was: an action was added, changed or removed, or stands under
    // another number after a change that could not be written down. Said when that is so, from
    // inside the call that made it so. (The wall says it on as its own `changed`.)
    void changed();

private:
    // THE DOOR for a run: what `action` is to a KVM's driver. Of one that types, its text is read
    // from the store here. Refused ("no_text", the server's code) where there is none to read:
    // none is kept for the action as the list has it, or Windows does not hand it out (which
    // the store does not tell apart). The sentence names the action and says that nothing was
    // typed. The wall's alone, and so not among what anybody can call: it goes through it when
    // a run is about to go out to a KVM (LocalWall::act), and at no other time.
    friend class LocalWall;
    base::Result<ForOneRun> forOneRun(const OwnAction& action);

    // Keeps a text, by its bytes, as a new writing in a place: whole or not at all. What the
    // list is to name that writing by (OwnAction::writing), or why none of it is kept.
    base::Result<QString> keepText(const QString& id, QChar place, const SecretBytes& text);
    // The bytes of an action's text as the store holds it: the writing the list names for it,
    // whole. Nothing: it has none (own_actions.h, above, says what is none). The one place
    // where a text is read, which the two doors go through.
    std::optional<SecretBytes> textOf(const OwnAction& action);
    // Forgets whatever is kept in a place of an action's.
    void forgetPlace(const QString& id, QChar place);
    // Remembers these as the actions; where that cannot be written down, the ones before are
    // the actions again, and why is told.
    base::Result<void> remember(const QList<OwnAction>& actions, const char* what);

    Settings& m_settings;
    SecretStore& m_secrets;
};

} // namespace qkvm::core
