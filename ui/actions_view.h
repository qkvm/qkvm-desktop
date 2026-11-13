#pragma once

#include <functional>

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include "media_view.h"
#include "notices.h"

namespace qkvm::ui {

// What the screens are given for doing things to machines: whether whoever is signed in may, the
// menu of the machine it was opened for, the question before what is asked about first, and the
// notices in the corner of the window. It decides nothing: what a screen asks goes out as a signal,
// and what is shown is what it was told to show. Made in C++ and handed to the screens; lives on
// the interface thread.
class ActionsView : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("The screens are handed this by the program.")
    // Whoever is signed in may act on machines: only then is a way into a menu drawn at all. With
    // the machines of several sources on the wall this means "at any source" (MachineActions).
    // (For the screens' round, R7: Wall.qml and Enlarged.qml are to go by each card's own
    // `actable` then, and not by this for every card.)
    Q_PROPERTY(bool allowed READ allowed NOTIFY allowedChanged)
    // The open menu, as machine_menu.h's shown() gives it; empty: none is open.
    Q_PROPERTY(QVariantMap menu READ menu NOTIFY menuChanged)
    // Something the menu sent is being answered, or asked about: the menu holds still until then.
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    // What is asked before something is done, as machine_menu.h's shown() gives it for one machine
    // or for several; empty: nothing is.
    Q_PROPERTY(QVariantMap question READ question NOTIFY questionChanged)
    // One thing being done to several machines, as BulkRun::shown() gives it; empty: none is shown.
    Q_PROPERTY(QVariantMap run READ run NOTIFY runChanged)
    Q_PROPERTY(qkvm::ui::Notices* notices READ notices CONSTANT)
    // The box that is open for keys of one's own choosing ("keys") or for a text to type ("text"),
    // for which machine, and what is amiss with what was put into it: {kind, machine, problem};
    // empty: none is open.
    Q_PROPERTY(QVariantMap form READ form NOTIFY formChanged)
    // What the box for keys offers (see machine_menu.h): each {value, label}.
    Q_PROPERTY(QVariantList heldKeys READ heldKeys CONSTANT)
    Q_PROPERTY(QVariantList otherKeys READ otherKeys CONSTANT)
    // The box for a machine's virtual media through its qKVM: what it tells is told in this window's corner.
    Q_PROPERTY(qkvm::ui::MediaView* media READ media CONSTANT)

public:
    // A test lets the notices go sooner (see Notices).
    explicit ActionsView(QObject* parent = nullptr, int noticesStayPercent = 100);

    bool allowed() const { return m_allowed; }
    QVariantMap menu() const { return m_menu; }
    bool busy() const { return m_busy; }
    QVariantMap question() const { return m_question; }
    QVariantMap run() const { return m_run; }
    Notices* notices() { return &m_notices; }
    QVariantMap form() const { return m_form; }
    MediaView* media() { return &m_media; }
    static QVariantList heldKeys();
    static QVariantList otherKeys();
    // Keys as a person reads them, for the box to say what is chosen so far.
    Q_INVOKABLE static QString read(const QString& keys);

    // What the screens ask for. The menu of a machine, by the wall's name for it; that it be closed
    // again; a row of it, by the row's id; yes or no to the question; and another of the computers
    // behind a KVM, by its number on the machine's card, counted from 0.
    Q_INVOKABLE void open(const QString& machine);
    Q_INVOKABLE void close();
    Q_INVOKABLE void choose(const QString& row);
    Q_INVOKABLE void answer(bool yes);
    // The same where the question was about several machines: how to go about it (the seconds
    // between one machine and the next, or -1 for a few at a time), and whether what had to be
    // ticked was ticked.
    Q_INVOKABLE void answerWith(bool yes, int paceSeconds, bool ticked);
    // The menu of the machines that are picked on the wall: of several, or of the one.
    Q_INVOKABLE void openPicked();
    // Of what is being done to several: that nothing more be sent; that what can be sent once more
    // be sent once more; and that its box be closed, which while it runs is a stop as well.
    Q_INVOKABLE void stopRun();
    Q_INVOKABLE void retryRun();
    Q_INVOKABLE void closeRun();
    Q_INVOKABLE void showPort(const QString& machine, int port);
    // The machine's own console, in the browser of whoever sits here. Also for whoever may only
    // look: that one is sent to the console's door, and signs in there.
    Q_INVOKABLE void openConsole(const QString& machine);
    // The button on a machine's card, where it has one: where it leads is the program's to say.
    Q_INVOKABLE void cardAction(const QString& machine);
    // And of the open box: that these keys be sent, that this text be typed, or that it be closed.
    Q_INVOKABLE void sendKeys(const QString& keys);
    Q_INVOKABLE void typeText(const QString& text, bool enter);
    Q_INVOKABLE void openText(const QString& machine) { emit textFormAsked(machine); }
    Q_INVOKABLE void loadClipboard();
    void readClipboardWith(std::function<QString()> read) { m_clipboard = std::move(read); }
    Q_INVOKABLE void closeForm();
    // Something a box of this window did is over: told in the window's corner, as done or as
    // something to be looked at.
    Q_INVOKABLE void tell(const QString& said, bool good);
    // The same, of what was done while nobody may have been looking at this window: where it is
    // out of sight, the notice stays until somebody sees the window (Notices::tellWhenSeen).
    Q_INVOKABLE void tellWhenSeen(const QString& said, bool good);

    // What they are told to show.
    void setAllowed(bool allowed);
    void showMenu(const QVariantMap& menu);
    void setBusy(bool busy);
    void showQuestion(const QVariantMap& question);
    void showForm(const QVariantMap& form);
    void showRun(const QVariantMap& run);

signals:
    void textFormAsked(const QString& machine);
    void allowedChanged();
    void menuChanged();
    void busyChanged();
    void questionChanged();
    void formChanged();
    void runChanged();

    void openAsked(const QString& machine);
    void closeAsked();
    void chosen(const QString& row);
    void answered(bool yes, int paceSeconds, bool ticked);
    void pickedAsked();
    void runStopAsked();
    void runRetryAsked();
    void runCloseAsked();
    void portAsked(const QString& machine, int port);
    void consoleAsked(const QString& machine);
    void cardActionAsked(const QString& machine);
    void keysAsked(const QString& keys);
    void textAsked(const QString& text, bool enter);
    void clipboardLoaded(const QString& text);
    void formCloseAsked();

    // For the screens, from whoever does what they ask: a KVM of this PC's own wall is to be
    // changed in its box; `review`: its certificate is to be looked at there, and nothing else.
    void editAsked(const QString& machine, bool review);

private:
    bool m_allowed = false;
    QVariantMap m_menu;
    bool m_busy = false;
    QVariantMap m_question;
    QVariantMap m_form;
    QVariantMap m_run;
    Notices m_notices;
    std::function<QString()> m_clipboard;
    MediaView m_media;
};

} // namespace qkvm::ui
