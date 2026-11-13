#pragma once

#include <functional>
#include <memory>

#include <QByteArray>
#include <QList>
#include <QObject>

#include <qkvm/base/result.h>
#include <qkvm/core/live.h>
#include <qkvm/core/server_types.h>

namespace qkvm::core {

// What all who show a KVM of this PC's own wall live share of it (a kvmd's: KvmdLive): whoever asks
// is handed a Live of their own, and the KVM is taken away under all of them at once. On the
// interface thread.
class OwnLive : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    // The KVM live, for one who asks: the caller's from then on. It says nothing before the caller
    // holds it.
    virtual std::unique_ptr<Live> open(Live::Need need) = 0;
    // The KVM is taken away under whoever has it live: each is told `closed` with this code and
    // sentence a turn of the loop later, and nothing is asked of its driver after this.
    virtual void end(const base::Error& why) = 0;
};

// What a KVM of this PC's own wall is to the wall, of whichever family (OwnKvm::kind): its driver.
// How it is, a look at it, a picture of its screen, what is done to it, the computers it can wake,
// and what has it live. A kvmd's is KvmdDevice. The wall (local_wall.h) holds one for each of its
// KVMs and asks nothing of a family's driver beyond this. Whether an error of `act` leaves open that
// it was done all the same is each family's own word, which the wall asks of the family
// (LocalWall::mayHaveBeenDone), not of a driver: of a KVM that is gone there is none to ask.
//
// On the interface thread, as each driver is: a call comes back at once; what it asked for is handed
// to `done` later, on the same thread, exactly once, unless the driver is gone by then.
class OwnDevice : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    // How it is, as last known; whether a look is due; a look, which does nothing while one is under
    // way; and `then` when the look under way is over, or after `withinMs`, whichever is first, told
    // whether a look is still under way.
    virtual Status status() const = 0;
    virtual bool isStale() const = 0;
    virtual void look() = 0;
    virtual void afterLook(int withinMs, std::function<void(bool stillLooking)> then) = 0;
    // A picture of its screen for a card `width` pixels wide. Nobody waits long for it.
    virtual void askSnapshot(int width, std::function<void(base::Result<Snapshot>)> done) = 0;
    // Has the KVM do something. The answer comes when it has done it.
    virtual void act(const Action& what, std::function<void(base::Result<Done>)> done) = 0;
    // The computers the KVM has saved to wake over the network; none where it keeps no such list.
    virtual void askWakeTargets(std::function<void(base::Result<QList<WakeTarget>>)> done) = 0;
    // Lets go of what is only kept for whoever looks, once nobody has for a while.
    virtual void tidy() = 0;
    // What has the KVM live for all who show it: made when it is first asked for, and kept for as
    // long as this driver is. It holds this driver, which outlives it, or ends it first (OwnLive::end).
    virtual std::unique_ptr<OwnLive> makeLive() = 0;

signals:
    // A look that was under way is over.
    void looked();
    // The KVM has shown, on a line of this driver's, the certificate it is known by: the certificate
    // itself, as its bytes (DER), which have the SHA-256 the driver holds its lines to. Said once for
    // each driver, the first time, and never of another certificate: of a KVM that is known by none,
    // and of one that shows another, nothing is said. For whoever keeps the certificate itself beside
    // its fingerprint (LocalWall, for the wall as a file).
    void showedItsCertificate(const QByteArray& der);
    // A sign-in of this driver's waits for another's with the same password to that KVM to be answered
    // (SignInPacing): said as it begins to wait. For a box that signs in, which says so.
    void waitsForAnotherSignIn();
};

} // namespace qkvm::core
