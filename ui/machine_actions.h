#pragma once

#include <functional>
#include <optional>

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QUrl>

#include <qkvm/core/server_types.h>
#include <qkvm/core/wall_source.h>

#include "actions_view.h"
#include "bulk_run.h"
#include "control.h"
#include "machine_menu.h"
#include "wall_machines.h"
#include "wall_view.h"

namespace qkvm::ui {

// What is being done to which machine at this moment, by what it is called, from whichever of the
// program's windows, by the machine's key on the wall (so that machines of two sources that have
// one id are two). One thing at a time for each machine: the answer to a power button can take
// half a minute, and a second press while the first is out does not do it again, it does the
// opposite.
using UnderWay = QHash<QString, QString>;

// Does what the screens ask to have done to a machine, the way the page does it: opens the machine's
// menu and keeps it as the machine is, asks first where that is called for, sends one thing at a
// time to a machine, and tells in the corner how it went.
//
// What is sent may not be answered: the line falls dead, or the server loses sight of it. That is
// said as what it is ("it may have been done all the same"), and nothing is sent again by itself.
// Which errors mean that is asked of the place it was sent to (core::WallSource), a QKVM server or
// this PC's own KVMs, and of nobody else: no answer from a server and no answer from a KVM are not
// the same error.
//
// Everything is asked through the wall's machines (WallMachines): a machine is named by its key on
// the wall, and what is asked for it goes to the place, and the machine of that place, that its key
// reaches. A key that reaches nothing is of no machine on the wall, and nothing is done to it.
//
// The wall may have the machines of several sources (Reached::source), and each source is a
// matter by itself here: whether whoever sits here may act on its machines (its word for them),
// its turn (an answer counts only in the turn of the source it came from), and its saved actions.
// What one source's wall leaving, or letting nobody act any more, drops is that source's alone: its
// questions, its menus and boxes, what is under way to its machines and their picks, and a run that
// touches one of them. Only where nobody may act on anything here any more does everything go.
//
// The same for several machines that were picked on the wall: their menu, the question about all of
// them (with a tick where it can lose somebody's work, and the choice of how fast), and the run
// itself (BulkRun), which knows through this what is under way on each machine. Of machines whose
// source lets nobody act, none is taken. Several of more than one source are offered no saved
// action, and their menu has no place for them ("Your actions" is left out until the screens'
// round words it): a saved action is its source's, and goes only to machines of that source, which
// is held where it is sent as well as in the menu.
//
// One for each window, all of them writing into one `underWay`: what one window sent to a machine
// is under way for the others too. A window that is closed takes what it sent out of it: nobody
// is left to hear the answer.
//
// On the interface thread. The views, the wall's machines (and the places they reach) and
// `underWay` outlive it.
class MachineActions : public QObject {
    Q_OBJECT

public:
    void useControl(MachineControl& control) { m_control = &control; }
    MachineActions(ActionsView& view, WallView& wall, WallMachines& machines, UnderWay& underWay,
                   QObject* parent = nullptr);
    ~MachineActions() override;
    MachineActions(const MachineActions&) = delete;
    MachineActions& operator=(const MachineActions&) = delete;
    MachineActions(MachineActions&&) = delete;
    MachineActions& operator=(MachineActions&&) = delete;

    // Somebody is at this source's wall from now on, or whoever is there is somebody else from now
    // on: whether they may act on its machines is said (by the source's word for them, and by
    // whether anybody may act on anything: MergedWall). Whoever may act there no longer has nothing
    // of that source's open, under way or picked, as if its wall had been left by one and come to
    // by another. `source`: as a key reaches it (Reached::source).
    void enter(const QString& source, bool mayAct);
    // The same for the place of a wall that has one place by itself (OnePlace, whose keys reach no
    // source), by the server's word for this account.
    void enter(const core::Account& account);
    // Whether this account may act on machines: an operator or an admin.
    static bool mayActAs(const core::Account& account);
    // This source's wall was left. What was open of it is closed, and an answer of it that is
    // still out is nobody's.
    void leave(const QString& source);
    // Every source's.
    void leave();
    // This source is not on the wall at all any more: what was its goes as where its wall is left,
    // and nothing of it is kept here. (An answer of it that still comes is nobody's: no turn of
    // another source is ever one it had.)
    void forgetSource(const QString& source);
    // Whether whoever sits here may act on the machine this key reaches, by the word of its source.
    bool mayActOn(const QString& key) const;
    bool supportsOn(const QString& key, const QString& ability) const;
    // How an address is opened in the browser of whoever sits here, and whether that went. Without
    // one nothing is opened, and that is said: a test opens no browser.
    void openPagesWith(std::function<bool(const QUrl&)> opener);
    // Of which source whoever sits here may have the machines' virtual media, and through which client: an admin of
    // that qKVM, signed in in person, while the program does not only show (the windows' wall says: MergedWall).
    // Nothing: of none, as without this. Asked whenever a menu is filled, a box opened, or who is there is told.
    void useMedia(std::function<core::ServerClient*(const QString& source)> of) { m_mediaOf = std::move(of); }
    // The QKVM's own page, in the same browser: only a web address is opened, and where none
    // opens, that is said.
    void openThePage(const QUrl& address);

signals:
    // A KVM, or a server's controller, was told to restart itself, and took it: by its key on the
    // wall.
    void restarted(const QString& key);
    // A JetKVM's picture was taken back ("Show here", act "resume"), and that was done: by its key on the wall.
    void resumed(const QString& key);

private:
    // What is each source's here.
    struct Side {
        // Raised whenever its wall is left, or whoever sits here may act there no longer: an answer
        // to what was asked of it before that is nobody's. Every side's turns are drawn from one
        // count, so no turn of one source is ever another's, nor one it had before.
        quint64 turn = 0;
        bool mayAct = false;
        QList<core::SavedAction> saved;
    };
    // This source's side, made where it has none yet, with a turn of its own.
    Side& sideOf(const QString& source);
    quint64 turnOf(const QString& source) const;
    bool mayActAt(const QString& source) const;
    bool anyMayAct() const;
    // The source a key reaches; nothing where it reaches none.
    QString sourceOf(const QString& key) const { return m_machines.reach(key).source; }
    // Nobody may act at this source any more: its turn is over, and what is its goes; where nobody
    // may act on anything here any more, everything goes, and what was told in the corner with it.
    void letGoOf(const QString& source);
    // What is this source's goes; nothing: everything does.
    void forget(const std::optional<QString>& only);
    // Its machines are picked no longer.
    void unpick(const QString& source);

    void open(const QString& machine);
    void close();
    void choose(const QString& row);
    void answer(bool yes, int paceSeconds, bool ticked);
    // Several machines: those that are picked on the wall get their menu (one alone gets its own).
    void openPicked();
    void chooseForSeveral(const QString& row);
    void fillForSeveral();
    // Of the machines the open menu is of, those that are still on the wall, in the wall's order.
    QList<core::Machine> severalThere() const;
    // The sources these machines are of, as their keys reach them now.
    QSet<QString> sourcesOf(const QList<core::Machine>& machines) const;
    // `savedOf`: the source whose saved actions the menu that asked listed; a saved action goes only
    // to machines of that source.
    void askOfSeveral(const QList<core::Machine>& targets, const Ask& ask, const std::optional<QString>& savedOf);
    void answerOfSeveral(bool yes, int paceSeconds, bool ticked);
    // Whether a run of several is going on, which nothing for several is begun beside; where one
    // is, that is said.
    bool refusedWhileARunGoes();
    void showRun();
    void runFinished();
    void closeRun();
    // The run that is there forgets what it did, and is shown no more.
    void forgetTheRun();
    BulkRun::Ways waysOfARun();
    void showPort(const QString& machine, int port);
    void openConsole(const QString& machine);
    // The button on a machine's card: for a KVM of this PC's own wall, the way to the box that
    // changes it, or to that box's look at its certificate (cardWayOf).
    void cardAction(const QString& machine);
    void browseTo(const core::Machine& machine, const QUrl& address);
    // The box for keys of one's own choosing, or for a text: opened for the machine whose menu led
    // there, which is closed for it, as on the page.
    void openForm(const core::Machine& machine, const QString& kind);
    void sendKeys(const QString& keys);
    void typeText(const QString& text, bool enter);
    void closeForm(bool continuing = false);
    // What was put into the box is sent on its way, or the box says what is amiss with it.
    void fromTheForm(const std::optional<Ask>& ask, const QString& amiss);
    // Whom the open box is for, as a question names them: a machine, or "3 KVMs".
    QString formIsFor() const;
    void followTheWall();
    // The media box of this machine (by its key), for whoever may have its media; and whose they are: the client of
    // that source where whoever sits here may have them, and nothing otherwise. The box goes where that is so no
    // longer (keepTheMediaBox).
    void openMedia(const QString& machine);
    core::ServerClient* mediaOf(const QString& source) const;
    void keepTheMediaBox();
    // Asks first where that is called for, and sends otherwise. `fromMenu`: the menu holds still
    // until it is over. `savedOf`: as for several.
    void perform(const core::Machine& machine, const Ask& ask, bool fromMenu, const std::optional<QString>& savedOf);
    void send(const core::Machine& machine, const Ask& ask, bool fromMenu, const std::optional<QString>& savedOf);
    // Whether this goes to a machine of this source: a saved action only to the source it is of.
    static bool goesTo(const Ask& ask, const std::optional<QString>& savedOf, const QString& source);
    // The answer of the place it was sent to.
    void told(const core::Machine& machine, const Ask& ask, const base::Result<core::Done>& answer,
              const Reached& sentTo);
    // Whether nothing is out to this machine; where something is, that is said.
    bool isFree(const core::Machine& machine);
    void holdStill(bool still);
    // Something was sent to this machine from here, and its answer is out; it has come; and
    // nothing that was sent from here (of this source) is anybody's any more.
    void began(const QString& machine, const QString& title, const QString& source);
    void ended(const QString& machine);
    void endAll();
    void endAllOf(const QString& source);
    // A notice that says something is under way, of this source's; and it is over.
    int underWay(const QString& text, const QString& source);
    void over(int notice);
    // The saved actions of the place the open menu's machines are of, which is of this source.
    void askForSaved(core::WallSource& place, const QString& source);
    // The saved actions the open menu lists: of the source m_menuSavedOf names, or none.
    QList<core::SavedAction> savedOfTheMenu() const;
    void fill();
    const core::Machine* machineOf(const QString& id) const;

    ActionsView& m_view;
    // Its view's media box, as it stands: let go of where this goes, and never reached through a view that went
    // first (a window is taken from the wall before its views end; one that was not is no use-after-free here).
    QPointer<MediaView> m_media;
    QPointer<MachineControl> m_control;
    WallView& m_wall;
    WallMachines& m_machines;
    // By source (Reached::source).
    QHash<QString, Side> m_sides;
    quint64 m_turns = 0;
    // The machine whose menu is open, and its source; empty: none is.
    QString m_open;
    QString m_openOf;
    Menu m_menu;
    // The source whose saved actions the open menu lists: that of its machine, or the one source
    // all of its several are of; nothing where they are of more than one.
    std::optional<QString> m_menuSavedOf;
    QList<core::WakeTarget> m_wake;
    // What is under way on which machine, from any window; and of that, what was sent from here,
    // with the source it was sent to.
    UnderWay& m_underWay;
    QHash<QString, QString> m_mine;
    QHash<QString, QString> m_pendingKeys; // Captured before a source can change or leave.
    // The notices that say that something sent from here is under way, with its source.
    QHash<int, QString> m_noticesOf;
    // What was asked about and is not answered yet.
    struct Asked {
        core::Machine machine;
        Ask ask;
        bool fromMenu = false;
        QString source;
        std::optional<QString> savedOf;
    };
    std::optional<Asked> m_asked;
    // The machine the open box is for, its source, and which box it is.
    std::optional<core::Machine> m_formFor;
    QString m_formOf;
    QString m_formKind;
    // The machines that were picked and whose menu is open, where it is the menu of several; and
    // those the open box is for, where it is for several; each with the sources they are of.
    QStringList m_several;
    QSet<QString> m_severalOf;
    QList<core::Machine> m_formForSeveral;
    QSet<QString> m_formSeveralOf;
    // What was asked about several machines and is not answered yet.
    struct AskedOfSeveral {
        QList<core::Machine> targets;
        Ask ask;
        QSet<QString> sources;
        std::optional<QString> savedOf;
    };
    std::optional<AskedOfSeveral> m_askedOfSeveral;
    // One thing being done to several machines, and whether its box is on the screen.
    BulkRun m_bulk;
    bool m_runShown = false;
    // Where each machine of that run is sent, by its key: what the key reached when the run
    // began. What is answered is of that place, and what is sent once more goes there: also for a
    // machine that has left the wall since, which reaches nothing now. And whose saved action it
    // sends, where it sends one.
    QHash<QString, Reached> m_runAt;
    std::optional<QString> m_runSavedOf;
    // Which of the things the open menu sent it is holding still for; 0: none.
    quint64 m_still = 0;
    quint64 m_lastSent = 0;
    std::function<bool(const QUrl&)> m_opener;
    // A pass into a console is being made out, of a machine of this source: a second click
    // meanwhile would have a second one made.
    bool m_entering = false;
    QString m_enteringOf;
    // A PC with Intel AMT is being put in another view (b5): not asked a second time meanwhile.
    void putInView(const core::Machine& machine, const QString& view);
    bool m_viewing = false;
    // Whose media whoever sits here may have (useMedia); and the source of the machine the media box is open for.
    std::function<core::ServerClient*(const QString& source)> m_mediaOf;
    QString m_mediaFor;
};

} // namespace qkvm::ui
