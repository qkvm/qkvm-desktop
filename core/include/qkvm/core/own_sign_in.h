#pragma once

#include <functional>
#include <memory>

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QUrl>

#include <qkvm/base/result.h>
#include <qkvm/core/jetkvm_device.h>
#include <qkvm/core/kvmd_device.h>
#include <qkvm/core/redfish_device.h>

namespace qkvm::core {

// The times the drivers of this PC's own wall keep, each family's own (OwnKvm::kind): for the wall,
// and for the boxes that add a KVM and change one, which make a driver of the KVM's family.
struct DriverPaces {
    KvmdDevice::Pace kvmd;
    JetKvmDevice::Pace jetkvm;
    // A server's management controller's (M7).
    RedfishDevice::Pace redfish;

    // A JetKVM's alike a kvmd's wherever the two keep the same time (to be let in, to answer, to be left
    // alone after a failure, the waits after a sign-in, how long a look counts), and its sign-ins counted
    // where the kvmd's are: whoever makes a kvmd's short (a test) has a JetKVM's short too. What only a
    // JetKVM keeps (its signalling line, a 429 at the most) stays its own.
    // And a controller's by the web's ratios to a kvmd's: an answer is waited for two and a half times as long
    // (BMC_WAITS, 20 s against WAITS, 8 s), a sign-in's too, its root before a sign-in twice that
    // (ROOT_WAIT_TIMES), and what a look found counts four times as long (LOOK_EVERY, 10 s against 2.5 s), with
    // what is timed against a look in the web's ratio to it (RETRY_LOOK, HICCUP, FAULT_HOLD); to be let in, the
    // rest of an answer, the waits after a failure and after a sign-in, and where sign-ins are counted, as a
    // kvmd's. So the program's are the web's, and a test's short kvmd is short for it too. The rest (a command's
    // confirmation, the goodbye, how often where it keeps things is asked anew) stays its own.
    static DriverPaces alike(KvmdDevice::Pace kvmd);
};

// One sign-in, by a driver that is made for it and ended after it, of the KVM's family: what the box
// that adds a KVM (kvm_adding.h) and the one that changes one (kvm_editing.h) sign in with before
// anything is saved, and what the wall hands out for that with the password it keeps
// (LocalWall::signerIn). It asks what the KVM is, as its family's driver asks it
// (KvmdDevice::identify, JetKvmDevice::identify), and nothing else: whatever else a driver does, the
// wall's own driver does once the KVM is on the wall (own_device.h).
//
// On the interface thread, as its driver is. identify() comes back at once; what it found is handed
// to `done` later, exactly once, unless this is gone by then. Whoever is handed it does not end this
// there and then, but a turn of the loop later (deleteLater): its driver is in the middle of handing
// it over.
class OneSignIn : public QObject {
    Q_OBJECT

public:
    // Where the KVM is and what it is known by, as each family's driver has it. A JetKVM has no user
    // name: what stands here is not used for one.
    struct Where {
        QUrl address;
        QString user = QStringLiteral("admin");
        bool knownByCertificate = false;
        QByteArray certificate;
        // The mark what answers is held to; empty: none, and what answers is taken (an addition, an edit with the
        // password typed). A JetKVM's: of the number it is known by (OwnKvm::number, JetKvmDevice::Where). A server's
        // controller's: the mark its server is kept by (OwnKvm::machine, RedfishDevice::Where), which its driver asks
        // which server answers by before anything goes. Of a kvmd, nothing.
        QString machine;
    };

    // What it says it is, in the words of both families.
    struct Said {
        QString model;
        QString version;
        QString hostname; // what it calls itself; a JetKVM calls itself nothing
        // A JetKVM's own number as a digest (JetKvmDevice::Identity: empty where it gives none; one it could not read
        // is no identity, J5); a server's, its computer's (RedfishDevice::Identity, as optional); a kvmd's: none.
        std::optional<QString> number;
        // A server's controller: the mark its server is known by after this sign-in (RedfishDevice::machine): the
        // one it was asked by, or the one it took where it was asked by none. Of a KVM, empty.
        QString machine;
    };

    // The sign-in of a KVM of this family (OwnKvm::kind) at `where`, with this password. Of a family
    // the program does not speak to by itself there is none ("invalid"). `twins`: what a JetKVM's sign-in
    // holds its own HTTPS to before its password goes in the clear (JetKvmDevice::lookAtItsHttpsBeforeEverySignIn):
    // the wall's (LocalWall::twinCheckFor), handed in by whoever makes one, so that no box sends a password where the
    // wall's own driver would send none (from the third reading of the twins, MEDIUM 1). A kvmd's sign-in has none.
    static base::Result<std::unique_ptr<OneSignIn>> of(const QString& family, const Where& where,
                                                       const QString& password, const DriverPaces& paces,
                                                       const JetKvmDevice::TwinCheck& twins);
    // A person has typed this password for a KVM of this family at `where` just now: it is tried as for
    // the first time, whatever was refused before, as its family's driver has it (SignInPacing::forget).
    static void tryAfresh(const QString& family, const Where& where, const QString& password, const DriverPaces& paces);

    ~OneSignIn() override;
    OneSignIn(const OneSignIn&) = delete;
    OneSignIn& operator=(const OneSignIn&) = delete;
    OneSignIn(OneSignIn&&) = delete;
    OneSignIn& operator=(OneSignIn&&) = delete;

    const QString& family() const { return m_family; }
    const Where& where() const { return m_where; }

    // Signs in, and reads what the KVM says of itself.
    void identify(std::function<void(base::Result<Said>)> done);

signals:
    // Its sign-in waits for another's with the same password to that KVM to be answered (another window's, the
    // wall's own driver's): said as it begins to wait (OwnDevice::waitsForAnotherSignIn).
    void waitsForAnotherSignIn();

private:
    OneSignIn(QString family, Where where);

    QString m_family;
    Where m_where;
    // Its family's driver: the one of the three.
    std::unique_ptr<KvmdDevice> m_kvmd;
    std::unique_ptr<JetKvmDevice> m_jetKvm;
    std::unique_ptr<RedfishDevice> m_redfish;
};

} // namespace qkvm::core
