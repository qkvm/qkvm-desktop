#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <qkvm/core/server_types.h>
#include <qkvm/core/wall_feed.h>
#include <qkvm/core/wall_source.h>
#include <qkvm/engine/scene.h>

#include "actions_view.h"
#include "live_picture.h"
#include "machine_actions.h"
#include "source.h"
#include "wall_cards.h"
#include "wall_key.h"
#include "wall_machines.h"
#include "wall_screen.h"
#include "wall_view.h"

namespace qkvm::ui {

class MachineControl;

// The wall as the program's windows have it. The machines of the sources it has become every
// window's cards, and their pictures go to every window's engine. Each window has its own way of
// doing things to machines (MachineActions), its own live picture of the machine it shows large
// (LivePicture), and whoever sends keyboard and mouse to that machine (MachineControl). And there
// is the program's own wall (`wall`), which no window need show: the machines for whoever goes by
// them without a window, and the pace the pictures are asked at.
//
// It has any number of sources at once (Source: this PC's own KVMs, QKVM servers), each with its
// place and its feed, in the order they were added. The cards are one list: the sources in their
// order, each in its own. A source's machines are on the wall while somebody is at its wall (from
// its `entered` to its `emptied`), as its feed last told them. A machine's key is its source's id
// and its own (wall_key.h), made when the cards are filled and kept in a table there; a key is
// looked up in that table, and one that is not in it reaches nothing. It is never taken apart to
// find a machine.
//
// What each source says of itself is heard here at once (its listeners are direct), and each is
// kept apart: who is at its wall and what whoever sits here may do there, its turn, its saved
// actions, its order. Whoever sits here may act on a source's machines where that source says so
// (an operator or an admin), and while the program does not only show. It only shows while this
// PC is a wall screen (WallScreen: from the moment a display link was used here until a person
// signs that screen out, whatever happens in between), and while a source is signed in by a
// display link: a screen nobody sits at, this PC's own KVMs included (they are still shown).
// Meanwhile nobody acts on any machine of any source, by any road of this class (menus, runs and a
// run sent again, questions, boxes, saved actions, keyboard and mouse, the wall's order), and the
// windows show the wall and nothing else where it begins. A source that leaves takes its own with
// it, and nothing of another's: its cards, picks, menus, boxes, questions, runs, restarts, streams
// and an order of it that is being made.
//
// The wall's own order is made for one source at a time (WallView::orderOf: the one whose order a
// window's arranging is of, from the moment it begins), of that source's machines alone, and sent
// to that source alone, for an admin of it.
//
// It is the only listener to the wallChanged, statusChanged, pictureCame and pictureFailed of each
// source's feed. Of a changed wall it fills every window's cards first, and then tells whoever does
// things to the machines (WallMachines, which it is). Of how the machines are, it fills the cards
// first; then it tells the source that its machines were looked at (Source::looked: a server's
// conversation goes by that); and last of all whoever does things to the machines.
//
// A picture is asked of the source a machine is of, once, as wide as the widest it is drawn at, and
// goes to every window that shows it. The machine a window shows large is had live, and keyboard and
// mouse sent to it, by the word of its own source.
//
// On the interface thread. The views and the program's own wall outlive it; a source is taken away
// (removeSource) before it ends.
class MergedWall final : public WallMachines {
    Q_OBJECT

public:
    // Where a window's pictures go: its engine as it is just now, or nothing (a window drawn
    // without one, or whose engine has ended). Asked each time.
    using Engine = std::function<engine::Scene*()>;

    // `wall`: the program's own (above). No source is on it until one is added.
    explicit MergedWall(WallView& wall, QObject* parent = nullptr);
    ~MergedWall() override;
    MergedWall(const MergedWall&) = delete;
    MergedWall& operator=(const MergedWall&) = delete;
    MergedWall(MergedWall&&) = delete;
    MergedWall& operator=(MergedWall&&) = delete;

    // A source's machines come onto the wall from now on, after those of the sources before it,
    // while somebody is at its wall. Not a source whose id is none (wall_key.h), or that of a source
    // the wall has, or one it has already: false then, and nothing is taken.
    bool addSource(Source& source);
    // And come no more: where somebody was at its wall, that wall is left here as it is when the
    // source leaves it by itself, and everything of it goes, its streams out of every engine.
    // Nothing of another source's goes.
    void removeSource(Source& source);
    // Whether the whole program only shows just now: this PC is a wall screen (onlyShowWhile), or
    // a source is signed in by a display link.
    bool onlyShows() const;
    void setAppRestricted(bool restricted);
    // Whether this PC is a wall screen is this one's to say, from now on. Once; it outlives this.
    void onlyShowWhile(const WallScreen& screen);
    // Of which source the wall's own order is made, in every window, where one is begun from now
    // on (WallView::setOrderOf).
    void setOrderOf(const QString& source);

    // A window shows the wall from now on: its wall (the program's own, where the program has the
    // one window and no other wall), what is done to machines from it, and where its pictures go.
    // Until it is taken away again, which is before any of the three ends; a wall that was taken
    // away says nothing here that is acted on, and is not given here a second time.
    void add(WallView& wall, ActionsView& actions, Engine drawsIn);
    void remove(WallView& wall);
    // Keyboard and mouse of the machine a window shows large go through this from now on
    // (control.h): it is handed the Live that window's picture holds, whenever that is another one
    // or none; and is told whether whoever sits here may act on that machine, which nobody may at
    // a wall that is only shown. For a wall that was added, until it is taken away: before that,
    // what the machine holds is let go.
    void drive(WallView& wall, MachineControl& control);
    // How a machine's own console is opened in a browser (see MachineActions), in every window.
    void openPagesWith(std::function<bool(const QUrl&)> opener);
    // How long a window waits before it asks again for a machine live, in the windows that are
    // added from now on.
    void retryLiveAt(LivePicture::Pace pace) { m_livePace = pace; }
    // The QKVM's own page, in the browser of whoever sits here: where none opens, that is said in
    // the first window (the program's icon asks for the page too, and has no window of its own to
    // say it in). With no window, nothing.
    void openThePage(const QUrl& address);
    // What every window shows may be other now: each asks for the pictures it draws, the machine it
    // shows large is had live where it may be, and each source's feed asks for what is in sight.
    void askForPictures();

    const QList<core::Machine>& machines() const override { return m_machines; }
    const QHash<QString, core::Status>& status() const override { return m_status; }
    Reached reach(const QString& key) const override { return m_reached.value(key); }
    QString actionKey(const QString& key) const override { return m_actionKey ? m_actionKey(key) : key; }
    // Whether an action sent from any of its windows (or virtual media being changed) is not answered yet: the list of
    // what is under way that its windows write into holds something.
    bool actionsUnderWay() const { return !m_actionRegistry->isEmpty(); }
    // Set before adding windows. The registry outlives this wall.
    void shareActions(UnderWay& registry, std::function<QString(const QString&)> identity)
    {
        m_actionRegistry = &registry;
        m_actionKey = std::move(identity);
    }
    void shareLive(LivePicture::OpenLive open) { m_openLive = std::move(open); }
    void lookAgainSoon(const QString& key) override;

signals:
    // Whether the program only shows is otherwise now (onlyShows).
    void onlyShowsChanged();

private:
    // One source, as the wall has it.
    struct Side {
        Source* source = nullptr;
        QString id;
        // Which taking of the source this is: one that was taken away and added again is another.
        quint64 serial = 0;
        // Who is at its wall; nobody: its machines are not on the wall.
        std::optional<core::Account> who;
        // Its feed has told its machines since somebody came to its wall: only then are they on
        // the wall. (What a feed holds from before is the wall of a session before, maybe of
        // another server: its keys would reach the new one by the old ids.)
        bool answered = false;
        // Its machines as the wall has them (each with its key as its id), how they are by key,
        // the machine's own id at the source by key, and its cards: as its feed last told them.
        QList<core::Machine> machines;
        QHash<QString, core::Status> status;
        QHash<QString, QString> ids;
        QList<WallTile> tiles;
        // Whether its cards say that whoever sits here may act, and that it is away, as they were
        // last filled.
        bool actable = false;
        bool away = false;
    };
    // One window, as far as the wall goes.
    struct Screen {
        WallView* wall = nullptr;
        Engine scene;
        std::unique_ptr<MachineActions> actions;
        // The engine's stream for each machine's picture in this window, by its key.
        QHash<QString, engine::Stream*> streams;
        // The machine this window shows large, had live.
        std::unique_ptr<LivePicture> live;
        // Whether whoever sits at this window may act on machines, as its screens are told; and
        // who sends keyboard and mouse to the machine it shows large, where somebody does.
        QPointer<ActionsView> may;
        QPointer<MachineControl> control;
        // The machine whose details were last opened here, told to its place as seen (sawNote); empty
        // while they are closed.
        QString noteSeenFor;
    };

    Side* sideOf(const Source* source);
    Side* sideNamed(const QString& id);
    // What the source and its feed say is heard here from now on.
    void listenTo(Source& added);
    // Somebody is at the source's wall from now on (know: whoever it is now, where somebody is
    // there); nobody is any more; and what was shown of it goes.
    void enter(Source& source, const core::Account& account);
    void know(Source& source, const core::Account& account);
    void leave(Source& source);
    void empty(Source& source);
    // Whoever is at its wall is who they are now: who may act where is told, and whoever is no
    // admin of it is not in the middle of making its order.
    void settle(const Side& side);
    // Whether whoever sits here may act on this source's machines just now.
    bool mayAt(const Side& side) const;
    // Where whether the program only shows is otherwise now: everybody is told who may act, and
    // where it only shows from now on, nobody is in the middle of making an order and every window
    // shows the wall and nothing else.
    void lookWhetherItOnlyShows();
    // Every window's actions are told who may act where, and whoever sends keyboard and mouse there
    // with them; and a card that says otherwise says it anew.
    void tellWhoMayAct();
    // Whoever is in the middle of making this source's order is not any more, in every window;
    // an order of another source's that is being made stays.
    void stopArranging(const Side& side);
    // The program's own wall, and each window's. (Where a window shows the program's own, as the
    // one window of a test does, that wall is told twice, which is telling it once.)
    template <typename Does>
    void onEveryWall(Does does);
    // A window's wall asks for this order to be the wall's own: sent to the source the machines are
    // of, which is the one the window's arranging is of, for an admin of it, and told in that
    // window how it went.
    void arrange(WallView& wall, ActionsView& actions, const QStringList& keys);
    // A machine's picture is drawn in this window from now on; and none is any more.
    static void addStream(Screen& screen, const QString& key, quint32 number, engine::Codec codec);
    void dropStreams(Screen& screen);
    // This source's cards are made anew from what its feed says (build), and all of them are put
    // together in every window (join), its streams with them; both (fill). Its cards say what is
    // so of the source now (stamp): whether it is away, and whether whoever sits here may act.
    void build(Side& side);
    void join();
    void fill(Side& side);
    void stamp(Side& side) const;
    // The pace the first source that somebody is at names, which holds in every window until
    // another is chosen there; a second where there is none. And those the others name: every
    // window offers each of them too. (How they are called is the screens' to say.)
    int serverPace() const;
    QList<int> otherPaces() const;
    void tellThePace();
    void tellThePace(WallView& wall) const;
    // A picture of a machine came, or could not be had: a server's card shows its screen or says
    // how the server is by that.
    void notePicture(const QString& key, bool came);
    // Whoever sends keyboard and mouse from this wall's window is told how things are there now:
    // through which Live its machine is had, and whether whoever sits here may.
    void tellControl(const WallView& wall);
    void tellDetails(WallView& wall);
    // The details of the machine shown large are open: its place is told that whoever sits here has
    // seen what it had to say of it there (core::WallSource::sawNote), once for each opening.
    void tellNoteSeen(WallView& wall);

    WallView& m_wall;
    // In the order they were added.
    std::vector<std::unique_ptr<Side>> m_sides;
    quint64 m_lastSide = 0;
    // Whether this PC is a wall screen (onlyShowWhile); none: it is none. And whether the program
    // only showed the last time that was looked at.
    const WallScreen* m_screen = nullptr;
    bool m_appRestricted = false;
    bool m_onlyShowed = false;
    // Of which source an order is made where one is begun (setOrderOf).
    QString m_orderOf;
    // The machines of every source on the wall, in its order, each by its key; how they are; and
    // what each key reaches. Made anew whenever a source's cards are.
    QList<core::Machine> m_machines;
    QHash<QString, core::Status> m_status;
    QHash<QString, Reached> m_reached;
    // What is under way on which machine, from whichever window it was sent.
    UnderWay m_underWay;
    UnderWay* m_actionRegistry = &m_underWay;
    std::function<QString(const QString&)> m_actionKey;
    // After what their actions write into: they end before it.
    std::vector<Screen> m_screens;
    std::function<bool(const QUrl&)> m_opener;
    LivePicture::Pace m_livePace;
    LivePicture::OpenLive m_openLive;
    // Each machine that has a picture, by its key: the number its stream has in every window's
    // engine, which the machine's card names.
    struct Pictured {
        quint32 number = 0;
        bool hasFrame = false;                      // a picture of it has come, and the last one asked for did not fail
        engine::Codec codec = engine::Codec::Still; // what its card is drawn from (cardCodecOf)
        bool still = false; // the last picture was a still kept between sessions (its card says its age)
    };
    QHash<QString, Pictured> m_streams;
    // The walls are being given their machines, one wall after the other: what is in sight is
    // asked for once all of them have theirs, and not by what the first shows alone.
    bool m_filling = false;
    quint32 m_lastStream = 0;
    // The machines that were told to restart and are waited for, by key, by this clock.
    QHash<QString, Restart> m_restarts;
    QElapsedTimer m_clock;
};

} // namespace qkvm::ui
