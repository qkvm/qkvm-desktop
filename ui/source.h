#pragma once

#include <QObject>
#include <QString>

#include <qkvm/core/server_types.h>
#include <qkvm/core/wall_feed.h>
#include <qkvm/core/wall_source.h>

namespace qkvm::core {
class ScreenWatch;
class ServerClient;
} // namespace qkvm::core

namespace qkvm::ui {

// One place the wall's machines come from, as the windows' wall has it (MergedWall): this PC's own
// KVMs (PcSource), or a QKVM server (ServerSource). It has the place that is asked
// (core::WallSource) and the feed that keeps its wall (core::WallFeed), and it says who is at its
// wall and when that changes. Each has its own turn: an answer to what was asked of it counts only
// in the turn it was asked in, whatever another source does meanwhile.
//
// Its id is what its machines' keys on the wall begin with (wall_key.h): never empty, never with
// the separator, and no other source's that is on the same wall.
//
// What it says, it says at the moment it happens, and whoever has the windows does their part there
// and then: its listeners are direct. On the interface thread.
class Source : public QObject {
    Q_OBJECT

public:
    ~Source() override = default;
    Source(const Source&) = delete;
    Source& operator=(const Source&) = delete;
    Source(Source&&) = delete;
    Source& operator=(Source&&) = delete;

    virtual QString id() const = 0;
    // The place its machines come from, and the feed that keeps their wall.
    virtual core::WallSource& place() = 0;
    // History and the activity log belong to a server; local devices have neither API.
    virtual core::ServerClient* detailsClient() { return nullptr; }
    // This PC's own wall: its KVMs' screen watches, which tell their Details (c7). None for a server's.
    virtual core::ScreenWatch* screens() { return nullptr; }
    virtual core::WallFeed& feed() = 0;
    // Whether somebody is at its wall: signed in, with or without contact, or at this PC's own.
    virtual bool isAtTheWall() const = 0;
    // Its wall is up and its place does not answer: what its cards show is what was last known.
    virtual bool isAway() const = 0;
    // Whether a machine of it may be had live, as far as the source goes: while a server answers,
    // and at this PC's own wall, where each KVM fails by itself.
    virtual bool mayBeLive() const = 0;
    // Whether whoever is at its wall may make the wall's own order there: an admin.
    virtual bool manages() const = 0;
    // Raised whenever what it was asked before is nobody's any more (its wall is left, and more:
    // each source says when); never lower than it was.
    virtual quint64 turn() const = 0;
    // The machines were looked at, and the wall's cards show what was found. (A server goes by
    // it; this PC's own wall does not.)
    virtual void looked() {}

signals:
    // Somebody is at its wall from now on, as this account; and whoever it is now. (Before the
    // state that follows is shown, and before its feed starts.)
    void entered(const core::Account& account);
    void whoChanged(const core::Account& account);
    // Nobody is at its wall any more; and then, once it has forgotten who was, what was shown of
    // its wall goes.
    void left();
    void emptied();
    // Where the source is may be otherwise now: whether a machine of it may be had live, and
    // whether it is away.
    void stateChanged();

protected:
    explicit Source(QObject* parent = nullptr) : QObject(parent) {}
};

} // namespace qkvm::ui
