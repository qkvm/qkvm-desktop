#pragma once

#include <optional>

#include <QList>
#include <QString>
#include <QVariantMap>

#include <qkvm/core/server_types.h>

// What can be done to a machine, as the page's menu for one machine has it: the same rows under the
// same headings, the same words, the same questions before what cannot be taken back. Which rows a
// machine gets is decided here and nowhere else; the screen shows what it is given, and whoever
// sends something to the server sends what a row holds.
namespace qkvm::ui {

// What a machine's menu is made from.
struct MenuOf {
    core::Machine machine;
    // Nothing: the server has not said yet how the machine is.
    std::optional<core::Status> status;
    QList<core::SavedAction> saved;
    // No saved action is offered at all, nor a place for them ("Your actions"): in a menu for
    // several machines, those of more than one source. (Said of the first of several, for all.)
    bool savedLeftOut = false;
    QList<core::WakeTarget> wake;
    // The machine is shown large already: the way there is left out.
    bool enlarged = false;
    // Whoever sits here may have the machine's virtual media: an admin of its qKVM, signed in in person. (Never on
    // this PC's own wall.) Its row is there where the server says the machine has some (`media` in `can`).
    bool media = false;
};

// What is sent and what is said when a row is chosen.
struct Ask {
    QString title; // what it is called: over the question, on its yes, in what is told afterwards
    core::Action action;
    QString done;         // what is told when it was done: "power button pressed"
    bool confirm = false; // asked about first
    bool danger = false;  // it can lose somebody's work
    QString question;     // the question, with the machine named in it
    QString warning;      // what is to be known before saying yes
    QString detail;       // what would be typed, to be looked at once more before it is
    QString yes;          // what its yes says, where that is not its title ("Restart into setup")
    // Done to several machines at once, it takes a tick as well as a yes, as everything does that
    // can lose somebody's work: a power button, a text.
    bool tick = false;
};

struct MenuRow {
    QString id; // which row, within its menu
    QString icon;
    QString label;
    QString hint; // the keys it sends, or how it stands ("on screen", "empty")
    bool danger = false;
    bool enabled = true;
    // Nothing: a row that leads somewhere else ("console", "enlarge", "edit", "sendKeys", "typeText").
    std::optional<Ask> ask;
};

struct MenuSection {
    QString title; // empty: the rows at the top, which need none
    QList<MenuRow> rows;
    QString note; // a sentence under the rows
};

struct Menu {
    QString title;    // the machine's name
    QString about;    // its address, and what it says it is
    QString warning;  // how it is, where it is not simply there
    QString kindNote; // what is to be known of this kind of device
    QString battery;  // that the battery of its Fingerbot is low, where it is
    QList<MenuSection> sections;

    // The row of this id, or nothing.
    const MenuRow* row(const QString& id) const;
};

Menu menuFor(const MenuOf& of);
QString abilityFor(const core::Action& action);

// What can be done to several machines at once, as the page's menu for a selection has it: what a
// KVM can do where a KVM is among them, what a server can do where a server is, and every machine
// answers for itself. No way to one machine's console or its computers, and nobody is woken. Each
// row asks about all of them ("... on 3 KVMs?").
Menu menuForSeveral(const QList<MenuOf>& of);

// One of the machines something is about to be done to, as the question lists them.
struct Target {
    QString name;
    QString host;
    QString state; // the server's word, for the colour of its dot
    QString label; // how it is, in a word
};

// Showing another of the computers behind a KVM: what the menu's row for it holds, for the numbers
// on the machine's card, which do the same. Nothing where the machine has no such computer.
std::optional<Ask> askToShow(const core::Machine& machine, const core::Status& status, int port);

// Keys of one's own choosing, pressed together and let go; and a text, typed as on the machine's
// keyboard, with Enter after it where that is asked. Both are asked about first: the text is shown
// once more, since typed into the wrong window it does whatever that window makes of it. Nothing
// where there is nothing to send, or what the server would not take.
// `who` is whom it is asked about: a machine's name, or "3 KVMs".
std::optional<Ask> askToPress(const QString& who, const QString& keys);
std::optional<Ask> askToType(const QString& who, const QString& text, bool enter);
// How several machines are called in a question.
QString severalKvms(qsizetype count);

// What the box for keys of one's own choosing offers, as the page's does: the keys that are held
// (Ctrl, Alt, Shift, Win), and the one key more that is pressed with them, the first of which is
// none. Each is {value, label}, the value being the server's word for the key.
QVariantList heldKeys();
QVariantList otherKeys();

// The menu as the screen takes it: {title, about, warning, kindNote, battery,
//  sections: [{title, note, rows: [{id, icon, label, hint, danger, enabled}]}]}.
QVariantMap shown(const Menu& menu);
// And a question: {title, text, detail, warning, danger, yes, targets, tick, paces}. `yes` is what
// the button says that says yes. For one machine the last three are empty.
QVariantMap shown(const Ask& ask);
// The same about several machines: `targets` lists them ({name, host, state, label}), `tick` is
// the sentence to be ticked first where a tick is asked for, and `paces` are the ways to go about
// it ({value, label}; the value is the seconds between one machine and the next, or -1 for a few
// at a time).
QVariantMap shown(const Ask& ask, const QList<Target>& targets);

// Keys as a person reads them: "ControlLeft,AltLeft,Delete" is "Ctrl+Alt+Del".
QString keysLabel(const QString& keys);

} // namespace qkvm::ui
