#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

#include <qkvm/core/server_types.h>
#include <qkvm/core/wall_feed.h>
#include <qkvm/core/wall_source.h>

namespace qkvm::ui {

// Where a machine's key on the wall leads: the place its machine comes from, and the machine's own
// id there, which is what that place is asked by. No place: the key is of no machine on the wall.
// And the id of the source the place is of (Source::id), by which whoever does things to machines
// keeps apart what is each source's; nothing where the wall has one place by itself (OnePlace).
struct Reached {
    core::WallSource* place = nullptr;
    QString machine;
    QString source;

    explicit operator bool() const { return place != nullptr; }
};

// What whoever does things to the wall's machines needs of the wall (MachineActions, and BulkRun
// and a window's LivePicture through it): the machines, and how each is, each by its key on the
// wall; which place, and which machine of it, a key reaches; and what "something was just done to
// this one" sets going. They ask through this and hold no place of their own: what is asked for a
// machine goes to the place its key reaches, and a key that is not on the wall reaches nothing.
//
// A key is what the wall's cards, menus, picks and windows name a machine by. The windows' wall
// (MergedWall) is one, of the machines of several sources at once, where a key is the source's id
// and the machine's (wall_key.h); OnePlace, below, is one place and its feed by themselves, as tests
// have them, where a key is the machine's own id.
//
// On the interface thread.
class WallMachines : public QObject {
    Q_OBJECT

public:
    ~WallMachines() override = default;
    WallMachines(const WallMachines&) = delete;
    WallMachines& operator=(const WallMachines&) = delete;
    WallMachines(WallMachines&&) = delete;
    WallMachines& operator=(WallMachines&&) = delete;

    // The machines of the wall, in its order, each with its key on the wall as its id.
    virtual const QList<core::Machine>& machines() const = 0;
    // How each of them is, as last known, by its key.
    virtual const QHash<QString, core::Status>& status() const = 0;
    // Which place, and which machine of it, this key reaches; nothing for a key that is not on the
    // wall.
    virtual Reached reach(const QString& key) const = 0;
    // Stable identity for a pending action, shared across independent window sessions.
    virtual QString actionKey(const QString& key) const { return key; }
    // Something was just done to this machine. What it did shows once the machine has got round to
    // it and been asked again: it is asked again soon, and not at the wall's usual pace alone
    // (core::WallFeed::lookAgainSoon).
    virtual void lookAgainSoon(const QString& key) = 0;

signals:
    // The machines of the wall, or how they are, are not what they were.
    void wallChanged();
    void statusChanged();
    // The machine of this key may be had live again: "Show here" took a JetKVM's one viewer back (M6 2h).
    // Every window that shows it, and stopped asking on a no, asks again (LivePicture::openAgain).
    void liveMayOpenAgain(const QString& key);

protected:
    explicit WallMachines(QObject* parent = nullptr) : QObject(parent) {}
};

// One place a wall's machines come from, by itself: a QKVM server, or this PC's own KVMs
// (core::WallSource), and the feed that keeps its wall (core::WallFeed), for whoever does things to
// machines without the windows' wall around them (tests). A key is the machine's own id, and
// reaches that place for as long as the feed has that machine, as of no source (Reached::source is
// nothing).
//
// What the feed tells of its wall is told on from here, at once, to whoever listens here: after
// whoever listened to the feed itself before this was made, and so in the order they all heard it
// when they listened to the feed. The place and the feed outlive it.
class OnePlace final : public WallMachines {
    Q_OBJECT

public:
    OnePlace(core::WallSource& place, core::WallFeed& feed, QObject* parent = nullptr);

    // The machines come from another place from now on: it is what the feed asks, and what every
    // key reaches. Only while the feed is stopped, as the feed itself takes another only then: under
    // one that runs this does nothing, and the place stays the one it was. Nobody else is told:
    // whoever asked the place before for anything is still waiting for its answer, which is of a
    // turn that is over, until whoever set the place has them leave their wall
    // (MachineActions::leave).
    void setPlace(core::WallSource& place);
    // The place that is asked just now, for what is asked of a wall and of no machine of it (the
    // wall's own order).
    core::WallSource& place() const { return *m_place; }

    const QList<core::Machine>& machines() const override { return m_feed.machines(); }
    const QHash<QString, core::Status>& status() const override { return m_feed.status(); }
    Reached reach(const QString& key) const override;
    void lookAgainSoon(const QString& key) override;

private:
    // Never null: the place that is asked, until another is set.
    core::WallSource* m_place;
    core::WallFeed& m_feed;
};

} // namespace qkvm::ui
