#include <qkvm/core/own_sign_in.h>

#include <string>
#include <utility>

using namespace Qt::StringLiterals;

namespace qkvm::core {

namespace {

KvmdDevice::Where asAKvmdHasIt(const OneSignIn::Where& where)
{
    return KvmdDevice::Where{where.address, where.user, where.knownByCertificate, where.certificate};
}

// (A JetKVM has a password and no user name.)
JetKvmDevice::Where asAJetKvmHasIt(const OneSignIn::Where& where)
{
    return JetKvmDevice::Where{where.address, where.knownByCertificate, where.certificate, where.machine};
}

RedfishDevice::Where asAControllerHasIt(const OneSignIn::Where& where)
{
    return RedfishDevice::Where{where.address,     where.user,    where.knownByCertificate,
                                where.certificate, where.machine, where.serverPlace};
}

// (A VNC server's address as vnc::addressFrom writes it, which the QUrl holds as it is; no user, no mark.)
VncDevice::Where asAVncServerHasIt(const OneSignIn::Where& where)
{
    return VncDevice::Where{where.address.toString(QUrl::FullyEncoded), where.knownByCertificate, where.certificate};
}

ProxmoxDevice::Where asAGuestHasIt(const OneSignIn::Where& where)
{
    // (A sign-in changes no boot order: none is handed to it.)
    return ProxmoxDevice::Where{where.address,     where.node,    where.vmid,  where.user, where.knownByCertificate,
                                where.certificate, where.machine, std::nullopt};
}

} // namespace

DriverPaces DriverPaces::alike(KvmdDevice::Pace kvmd)
{
    DriverPaces paces;
    paces.jetkvm.connectWithinMs = kvmd.connectWithinMs;
    paces.jetkvm.quietMs = kvmd.quietMs;
    paces.jetkvm.transferMs = kvmd.transferMs;
    paces.jetkvm.holdMs = kvmd.holdMs;
    paces.jetkvm.longestHoldMs = kvmd.longestHoldMs;
    paces.jetkvm.refusedHoldMs = kvmd.refusedHoldMs;
    paces.jetkvm.signInRetryMs = kvmd.signInRetryMs;
    paces.jetkvm.freshMs = kvmd.freshMs;
    // (One pacing for the sign-ins of both: what a test keeps of its own, it keeps for both.)
    paces.jetkvm.pacing = kvmd.pacing;
    paces.vnc.pacing = kvmd.pacing;
    // A controller's, by the web's ratios (the header says which).
    RedfishDevice::Pace& server = paces.redfish;
    server.connectWithinMs = kvmd.connectWithinMs;
    server.quietMs = kvmd.quietMs * 5 / 2;
    server.transferMs = kvmd.transferMs;
    server.signInQuietMs = kvmd.quietMs * 5 / 2;
    server.rootUnsignedMs = server.quietMs * 2;
    server.holdMs = kvmd.holdMs;
    server.longestHoldMs = kvmd.longestHoldMs;
    server.refusedHoldMs = kvmd.refusedHoldMs;
    server.signInRetryMs = kvmd.signInRetryMs;
    server.stillThereEveryMs = kvmd.stillThereEveryMs;
    server.pacing = kvmd.pacing;
    server.freshMs = kvmd.freshMs * 4;
    server.retryLookMs = server.freshMs * 2 / 5;
    server.hiccupMs = server.freshMs * 9 / 2;
    server.faultHoldMs = server.freshMs * 30;
    // A guest's sign-ins counted where the kvmd's are, and its times its own (the web's for a guest).
    paces.proxmox.pacing = kvmd.pacing;
    paces.kvmd = std::move(kvmd);
    return paces;
}

OneSignIn::OneSignIn(QString family, Where where) : m_family(std::move(family)), m_where(std::move(where)) {}

OneSignIn::~OneSignIn() = default;

base::Result<std::unique_ptr<OneSignIn>> OneSignIn::of(const QString& family, const Where& where,
                                                       const QString& password, const DriverPaces& paces,
                                                       const JetKvmDevice::TwinCheck& twins)
{
    // (Made here, where its constructor may be called: there is no sign-in of a family it does not read.)
    std::unique_ptr<OneSignIn> made(new OneSignIn(family, where));
    OwnDevice* driver = nullptr;
    if (family == "kvmd"_L1) {
        made->m_kvmd = std::make_unique<KvmdDevice>(asAKvmdHasIt(where), password, paces.kvmd);
        driver = made->m_kvmd.get();
    } else if (family == "jetkvm"_L1) {
        made->m_jetKvm = std::make_unique<JetKvmDevice>(asAJetKvmHasIt(where), password, paces.jetkvm);
        driver = made->m_jetKvm.get();
        // (Its HTTPS looked at before its password goes in the clear, as the wall's own driver's is.)
        made->m_jetKvm->lookAtItsHttpsBeforeEverySignIn(twins);
    } else if (family == "redfish"_L1) {
        // (No twin check: a server's controller is never one device by its certificate. Which server answers is its
        // driver's own question, by the mark in `where`.)
        made->m_redfish = std::make_unique<RedfishDevice>(asAControllerHasIt(where), password, paces.redfish);
        driver = made->m_redfish.get();
    } else if (family == "vnc"_L1) {
        // (A VNC server's: its own connection, held to the certificate in `where` over vncs://.)
        made->m_vnc = std::make_unique<VncDevice>(asAVncServerHasIt(where), password, paces.vnc);
        driver = made->m_vnc.get();
    } else if (family == "proxmox"_L1) {
        // (No twin check: a Proxmox's certificate is every guest's of it. Which virtual machine has the guest's number
        // is its driver's own question, by the number in `where`: rule 30.)
        made->m_proxmox = std::make_unique<ProxmoxDevice>(asAGuestHasIt(where), password, paces.proxmox);
        driver = made->m_proxmox.get();
    } else {
        return base::Error{"invalid", "This program does not speak to that family of device itself."};
    }
    connect(driver, &OwnDevice::waitsForAnotherSignIn, made.get(), &OneSignIn::waitsForAnotherSignIn);
    return made;
}

void OneSignIn::tryAfresh(const QString& family, const Where& where, const QString& password, const DriverPaces& paces)
{
    if (family == "kvmd"_L1) {
        KvmdDevice::tryAfresh(asAKvmdHasIt(where), password, paces.kvmd);
    } else if (family == "jetkvm"_L1) {
        JetKvmDevice::tryAfresh(asAJetKvmHasIt(where), password, paces.jetkvm);
    } else if (family == "redfish"_L1) {
        RedfishDevice::tryAfresh(asAControllerHasIt(where), password, paces.redfish);
    } else if (family == "vnc"_L1) {
        VncDevice::tryAfresh(asAVncServerHasIt(where), password, paces.vnc);
    } else if (family == "proxmox"_L1) {
        ProxmoxDevice::tryAfresh(asAGuestHasIt(where), password, paces.proxmox);
    }
}

namespace {

// A box asks once: what a driver says of a sign-in that is asked again shortly (as the wall's card asks) is not said
// there, where nothing asks again.
base::Error askedOnce(base::Error why)
{
    static const std::string again = " It is asked again shortly.";
    if (why.message.ends_with(again)) {
        why.message.erase(why.message.size() - again.size());
    }
    return why;
}

} // namespace

void OneSignIn::identify(std::function<void(base::Result<Said>)> done)
{
    if (m_kvmd != nullptr) {
        m_kvmd->identify([done = std::move(done)](const base::Result<KvmdDevice::Identity>& who) {
            if (!who) {
                done(askedOnce(who.error()));
                return;
            }
            done(Said{who.value().model, who.value().version, who.value().hostname, std::nullopt, QString(),
                      ServerPlace{}});
        });
        return;
    }
    if (m_redfish != nullptr) {
        // (The mark is the driver's once it has answered: what it was asked by, or what it took.)
        m_redfish->identify(
            [done = std::move(done), driver = m_redfish.get()](const base::Result<RedfishDevice::Identity>& who) {
                if (!who) {
                    done(askedOnce(who.error()));
                    return;
                }
                done(Said{who.value().model, who.value().version, who.value().hostname, who.value().number,
                          driver->machine(), driver->serverPlace()});
            });
        return;
    }
    if (m_vnc != nullptr) {
        m_vnc->identify([done = std::move(done)](const base::Result<VncDevice::Identity>& who) {
            if (!who) {
                done(askedOnce(who.error()));
                return;
            }
            done(Said{who.value().model, who.value().version, who.value().hostname, std::nullopt, QString(),
                      ServerPlace{}});
        });
        return;
    }
    if (m_proxmox != nullptr) {
        // (The number is the driver's once it has answered: what it was asked by, or what it took.)
        m_proxmox->identify(
            [done = std::move(done), driver = m_proxmox.get()](const base::Result<ProxmoxDevice::Identity>& who) {
                if (!who) {
                    done(askedOnce(who.error()));
                    return;
                }
                done(Said{who.value().model, who.value().version, who.value().hostname, who.value().number,
                          driver->machine(), ServerPlace{}});
            });
        return;
    }
    m_jetKvm->identify([done = std::move(done)](const base::Result<JetKvmDevice::Identity>& who) {
        if (!who) {
            done(askedOnce(who.error()));
            return;
        }
        done(Said{who.value().model, who.value().version, QString(), who.value().number, QString(), ServerPlace{}});
    });
}

} // namespace qkvm::core
