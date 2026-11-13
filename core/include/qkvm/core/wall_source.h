#pragma once

#include <functional>
#include <memory>

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>

#include <qkvm/base/result.h>
#include <qkvm/core/live.h>
#include <qkvm/core/server_types.h>

namespace qkvm::core {

// Where a wall's machines come from, and what is asked of that place for them (SPEC.md section 1):
// a QKVM server, or this PC's own list of KVMs, which the program speaks to itself. Whatever shows
// a wall and does things to its machines asks this, and cannot tell which of the two answers: the
// types and the words are the same.
//
// On the interface thread. A call comes back at once; what it asked for is handed to `done` later,
// on the same thread, at most once.
class WallSource {
public:
    virtual ~WallSource() = default;
    WallSource(const WallSource&) = delete;
    WallSource& operator=(const WallSource&) = delete;
    WallSource(WallSource&&) = delete;
    WallSource& operator=(WallSource&&) = delete;

    // The machines of the wall, in its order.
    virtual void askWall(std::function<void(base::Result<Wall>)> done) = 0;
    // How each of them is, as last known: nobody waits for a machine that is slow or away.
    virtual void askStatus(std::function<void(base::Result<QHash<QString, Status>>)> done) = 0;
    // A picture at least `width` pixels wide where the machine has that many, that may be up to
    // `mayBeOldSeconds` old.
    virtual void askSnapshot(const QString& machine, int width, double mayBeOldSeconds,
                             std::function<void(base::Result<Snapshot>)> done) = 0;
    // Has something done to a machine. The answer comes when the machine has done it.
    virtual void act(const QString& machine, const Action& what, std::function<void(base::Result<Done>)> done) = 0;
    // Whether an error of act() for `machine` leaves open that the machine did it all the same.
    // That is the place's own to say, and of this PC's own wall the machine's family's: what "no
    // answer" means differs between a server and a KVM, and between two families of KVM. What
    // may have been done is never sent a second time on the strength of the error.
    virtual bool mayHaveBeenDone(const QString& machine, const base::Error& error) const = 0;
    // The actions saved for any machine of the wall, and the computers a KVM can wake.
    virtual void askSavedActions(std::function<void(base::Result<QList<SavedAction>>)> done) = 0;
    virtual void askWakeTargets(const QString& machine, std::function<void(base::Result<QList<WakeTarget>>)> done) = 0;
    // Where a browser enters a machine's own console, signed in: a pass that is good for one use,
    // of a machine that says it hands such passes out (Status::opensSignedIn). A place that has
    // none to hand out refuses, as it refuses anything else.
    virtual void askConsole(const QString& machine, std::function<void(base::Result<QUrl>)> done) = 0;
    // Puts the wall's machines in an order: those named first, as named, and the rest after them
    // as they stood. What comes back is the order of all of them as it is now.
    virtual void arrange(const QStringList& machines, std::function<void(base::Result<QStringList>)> done) = 0;
    // A machine of the wall, live (live.h): its pictures as they come, no wider and no more a
    // second than `need` says, and keyboard and mouse to it. What is handed over is the caller's
    // from then on, and says itself what becomes of it; it says nothing before it was handed
    // over. Or why there is none to be had, in the place's own words. Whether the wall's machines
    // can be had live at all, the wall says (Wall::liveForm): a place that says no is not asked.
    virtual void openLive(const QString& machine, Live::Need need,
                          std::function<void(base::Result<std::unique_ptr<Live>>)> done) = 0;
    // Whoever sits here has seen what the place had to say of a machine in its details
    // (Machine::note): it need not be said again. A place that says nothing there ignores it.
    virtual void sawNote(const QString& machine) { static_cast<void>(machine); }
    // A PC with Intel AMT put in another of its three views (Machine::amtView: "health", "stills",
    // "live"): the view it is in after that, in the place's word. A place without such PCs (this
    // PC's own wall) refuses, a turn later, as it refuses anything else.
    virtual void setView(const QString& machine, const QString& view, std::function<void(base::Result<QString>)> done);

protected:
    WallSource() = default;
};

inline void WallSource::setView(const QString& /*machine*/, const QString& /*view*/,
                                std::function<void(base::Result<QString>)> done)
{
    QTimer::singleShot(
        0, [done = std::move(done)] { done(base::Error{"not_found", "No PC with Intel AMT is on this wall."}); });
}

} // namespace qkvm::core
