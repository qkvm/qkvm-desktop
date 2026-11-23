#pragma once

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

#include <QCryptographicHash>
#include <QList>
#include <QSignalSpy>
#include <QString>
#include <QTest>

#include <qkvm/core/proxmox_device.h>
#include <qkvm/core/sign_in_pacing.h>

#include "proxmox_by_hand.h"
#include "stand_in.h"

// What the tests of a Proxmox guest spoken to by the program itself share: the times made short, a pacing of the
// test's own, where a guest of a stand-in is, and the asking of a driver and the waiting for its answer.
namespace qkvm::core::testing {

inline ProxmoxDevice::Pace quickProxmox()
{
    ProxmoxDevice::Pace pace;
    pace.connectWithinMs = 1500;
    pace.quietMs = 2000;
    pace.transferMs = 2000;
    pace.signInQuietMs = 4000;
    pace.holdMs = 200;
    pace.longestHoldMs = 800;
    pace.refusedHoldMs = 3000;
    pace.signInRetryMs = 1500;
    pace.freshMs = 0;
    pace.pacing = std::make_shared<SignInPacing>();
    return pace;
}

inline Tls proxmoxTls(const char* name = "first")
{
    const QString folder = QStringLiteral(QKVM_TESTDATA "/tls/");
    return {folder + QLatin1StringView(name) + QStringLiteral(".crt"),
            folder + QLatin1StringView(name) + QStringLiteral(".key")};
}

inline QByteArray certificateOf(const char* name = "first")
{
    return StandIn::certificateIn(proxmoxTls(name).certificateFile).digest(QCryptographicHash::Sha256);
}

// Somebody else at a Proxmox's address, or at another (another HTTPS service, a printer's page, a KVM's): it shows the
// certificate it is given, answers everything 404, and keeps what it heard.
struct SomebodyElse {
    QList<Asked> heard;
    StandIn stand;

    explicit SomebodyElse(const char* certificate, quint16 port = 0)
        : stand(
              [this](const Asked& asked) {
                  heard.append(asked);
                  return Answered(404, "Not Found", "text/plain");
              },
              proxmoxTls(certificate), port)
    {}
    QString address() const { return stand.address(); }
    // Whether anything it heard carried the token, in its header or in its address.
    bool heardTheToken(const QString& secret) const
    {
        return std::ranges::any_of(heard, [&secret](const Asked& one) {
            return one.headers.contains("authorization") || one.path.contains(secret.toLatin1());
        });
    }
};

// Guest `vmid` of the stand-in, on its node, known by the certificate the stand-in shows.
inline ProxmoxDevice::Where guestOf(const ProxmoxByHand& pve, qint64 vmid = 100)
{
    ProxmoxDevice::Where where;
    where.address = pve.address();
    where.node = pve.guests.value(vmid).node;
    where.vmid = vmid;
    where.user = pve.tokenId;
    where.knownByCertificate = true;
    where.certificate = certificateOf();
    return where;
}

// Has the guest looked at, and says how it is once that look is over.
inline Status lookedAt(ProxmoxDevice& device)
{
    QSignalSpy over(&device, &ProxmoxDevice::looked);
    device.look();
    if (!over.wait(20000)) {
        Status never;
        never.state = QStringLiteral("the look never ended");
        return never;
    }
    return device.status();
}

template <typename Answer>
base::Result<Answer> answerOf(const std::function<void(std::function<void(base::Result<Answer>)>)>& ask)
{
    QList<base::Result<Answer>> answers;
    ask([&answers](base::Result<Answer> answer) { answers.append(std::move(answer)); });
    if (!answers.isEmpty()) {
        return base::Error{"at_once", "The answer came before the call came back."};
    }
    if (!QTest::qWaitFor([&answers] { return !answers.isEmpty(); }, 30000)) {
        return base::Error{"never", "The call was never answered."};
    }
    return answers.takeFirst();
}

inline base::Result<ProxmoxDevice::Identity> identified(ProxmoxDevice& device)
{
    return answerOf<ProxmoxDevice::Identity>(
        [&device](std::function<void(base::Result<ProxmoxDevice::Identity>)> done) {
            device.identify(std::move(done));
        });
}

template <typename Answer>
std::string codeOf(const base::Result<Answer>& what)
{
    return what ? std::string("ok") : what.error().code;
}

} // namespace qkvm::core::testing
