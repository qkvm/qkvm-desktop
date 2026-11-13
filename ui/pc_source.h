#pragma once

#include <QObject>
#include <QString>

#include <qkvm/core/local_wall.h>
#include <qkvm/core/server_types.h>
#include <qkvm/core/wall_feed.h>

#include "source.h"

namespace qkvm::ui {

// This PC's own wall as a place the wall's machines come from: the KVMs that were added in the
// program (core::LocalWall), which it speaks to itself, and the feed that keeps their wall, which
// is its own. Nobody signs in to it: whoever sits at this PC may do everything there, as with a
// browser that was kept signed in to a QKVM; Windows' own lock is the lock.
//
// While its wall is shown, what the wall says of itself goes to the feed at once, and not at the
// feed's next turn: a KVM that was added, changed or taken off, or an order that was made, has the
// list asked for anew and the machines looked at again soon (a KVM that was just added is being
// signed in to), and one that came to something with nobody having asked has the machines looked
// at now. While another wall is shown, those updates are forwarded only when background
// monitoring was explicitly attached and the feed is still running.
//
// Whoever shows its wall says when that is (enter, leave), and starts and stops the feed with it
// (ServerSession); it says so itself to whoever has the windows (Source::entered, left, emptied).
// On the interface thread. The wall outlives it.
class PcSource final : public Source {
    Q_OBJECT

public:
    // How the machines are is asked as often as a server's wall asks it: as often as the page does,
    // and more often in a test.
    explicit PcSource(core::LocalWall& wall, int statusEveryMs = 4000, QObject* parent = nullptr);

    // Its id, as its machines' keys on the wall begin with it: no server's is ever this.
    static QString idOfThisPc();
    QString id() const override { return idOfThisPc(); }
    // The place its machines come from, and the feed that keeps their wall.
    core::WallSource& place() override { return m_wall; }
    core::WallFeed& feed() override { return m_feed; }
    core::ScreenWatch* screens() override { return &m_wall.screens(); }
    // Whether its wall is the one that is shown: from enter() to leave(); not at first.
    bool isAtTheWall() const override { return m_atTheWall; }
    // Each KVM fails by itself: the wall as a whole is never away.
    bool isAway() const override { return false; }
    bool mayBeLive() const override { return m_atTheWall; }
    // Whoever sits here makes its order, as everything else on it.
    bool manages() const override { return true; }
    // Raised whenever its wall is left: an answer of the wall to what was asked while it was shown
    // is nobody's any more.
    quint64 turn() const override { return m_turn; }
    // Its wall is shown from now on, with whoever sits here at it (whoever()), which is said
    // (entered); and is not any more: that is said too (left, emptied). Each only where it changes
    // something.
    void enter();
    void leave();
    void monitorInBackground() { m_monitors = true; }
    // Whoever sits at this PC, as its wall knows them: one who may do everything there. There are
    // no accounts on it.
    static core::Account whoever();
    // Whether everything on its wall is spoken to encrypted: no KVM of it is spoken to over plain
    // HTTP (its card says which one is). Nothing travels to a server from here.
    bool isEncrypted() const;

signals:
    // The wall's KVMs, or their order, are not what they were (core::LocalWall::changed: a KVM was
    // added, changed or taken off, or the order was made), while the wall is shown, and the feed
    // was told: whether everything on it is spoken to encrypted may be otherwise now.
    void kvmsChanged();

private:
    core::LocalWall& m_wall;
    core::WallFeed m_feed;
    bool m_atTheWall = false;
    bool m_monitors = false;
    quint64 m_turn = 0;
};

} // namespace qkvm::ui
