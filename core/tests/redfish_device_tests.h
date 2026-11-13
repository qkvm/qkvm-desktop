#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <QCryptographicHash>
#include <QJsonValue>
#include <QList>
#include <QSignalSpy>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QString>
#include <QTest>

#include <qkvm/core/redfish_device.h>
#include <qkvm/core/sign_in_pacing.h>

#include "redfish_by_hand.h"
#include "stand_in.h"

// What the tests of a server's controller spoken to by the program itself share (test_redfish_sign_in,
// test_redfish_which_server, test_redfish_look): the times made short, the numbers of two servers of one make, and
// the asking of a driver and the waiting for its answer.
namespace qkvm::core::testing {

// Times made short, and a pacing and goodbyes of the test's own: nothing of one test's sign-ins reaches
// another's.
inline RedfishDevice::Pace quick()
{
    RedfishDevice::Pace pace;
    pace.connectWithinMs = 1500;
    pace.quietMs = 1500;
    pace.transferMs = 1500;
    pace.signInQuietMs = 3750;
    pace.rootUnsignedMs = 3000;
    pace.holdMs = 400;
    pace.longestHoldMs = 1600;
    pace.refusedHoldMs = 600;
    pace.signInRetryMs = 800;
    pace.signOutWithinMs = 1000;
    pace.freshMs = 300;
    pace.retryLookMs = 100;
    pace.hiccupMs = 0; // (a look that fails says so at once, unless a test is about that: as the web's tests)
    pace.sensorsWithinMs = 3000;
    pace.pacing = std::make_shared<SignInPacing>();
    pace.goodbyes = std::make_shared<ControllerGoodbyes>();
    return pace;
}

inline Tls tls(const char* name)
{
    const QString folder = QStringLiteral(QKVM_TESTDATA "/tls/");
    return {folder + QLatin1StringView(name) + QStringLiteral(".crt"),
            folder + QLatin1StringView(name) + QStringLiteral(".key")};
}

inline QByteArray sha256Of(const char* name)
{
    return StandIn::certificateIn(tls(name).certificateFile).digest(QCryptographicHash::Sha256);
}

inline RedfishDevice::Where at(const RedfishByHand& bmc)
{
    RedfishDevice::Where where;
    where.address = bmc.address();
    return where;
}

inline QString digestOf(const QJsonValue& number)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(number.toString().toLower().toUtf8(), QCryptographicHash::Sha256).toHex().first(32));
}

// Two servers of one make in one rack: the same certificate and password, numbers of their own.
inline const QString aSystem = QStringLiteral("4c4c4544-0042-3510-8054-b9c04f4a3532");
inline const QString aRoot = QStringLiteral("1a2b3c4d-5e6f-4a1b-8c2d-3e4f5a6b7c8d");
inline const QString bSystem = QStringLiteral("4c4c4544-0042-3510-8054-c7c04f4a3533");
inline const QString bRoot = QStringLiteral("9f8e7d6c-5b4a-4f9e-8d7c-6b5a4f3e2d1c");
inline const QString zero = QStringLiteral("00000000-0000-0000-0000-000000000000"); // a board whose maker set no number

// A stand-in of server A, and where it is, known by `machine`.
inline RedfishDevice::Where keptAs(const RedfishByHand& bmc, const QString& machine)
{
    RedfishDevice::Where where;
    where.address = bmc.address();
    where.machine = machine;
    return where;
}

inline QString markOf(const QJsonValue& number)
{
    return QStringLiteral("server:") +
           QString::fromLatin1(
               QCryptographicHash::hash(number.toString().toLower().toUtf8(), QCryptographicHash::Sha256)
                   .toHex()
                   .first(32));
}

inline base::Result<RedfishDevice::Identity> identified(RedfishDevice& device)
{
    QList<base::Result<RedfishDevice::Identity>> answers;
    device.identify([&](base::Result<RedfishDevice::Identity> answer) { answers.append(std::move(answer)); });
    if (!answers.isEmpty()) {
        return base::Error{"at_once", "The answer came before the call came back."};
    }
    if (!QTest::qWaitFor([&] { return !answers.isEmpty(); }, 30000)) {
        return base::Error{"never", "The call was never answered."};
    }
    return answers.takeFirst();
}

// Has the controller looked at, and says how the server is once that look is over.
inline qkvm::core::Status lookedAt(RedfishDevice& device)
{
    QSignalSpy over(&device, &RedfishDevice::looked);
    device.look();
    if (!over.wait(20000)) {
        qkvm::core::Status never;
        never.state = QStringLiteral("the look never ended");
        return never;
    }
    return device.status();
}

inline qkvm::core::ServerReadings readingsOf(const qkvm::core::Status& status)
{
    return status.server.value_or(qkvm::core::ServerReadings{});
}

inline std::string codeOf(const base::Result<RedfishDevice::Identity>& what)
{
    return what ? std::string("ok") : what.error().code;
}

// While it stands, Windows trusts the makers of certificates in `makersFile` too (as test_kvmd_device's).
class VouchedFor {
public:
    explicit VouchedFor(const QString& makersFile) : m_before(QSslConfiguration::defaultConfiguration())
    {
        QSslConfiguration with = m_before;
        with.addCaCertificates(QSslCertificate::fromPath(makersFile, QSsl::Pem));
        QSslConfiguration::setDefaultConfiguration(with);
    }
    ~VouchedFor() { QSslConfiguration::setDefaultConfiguration(m_before); }
    VouchedFor(const VouchedFor&) = delete;
    VouchedFor& operator=(const VouchedFor&) = delete;
    VouchedFor(VouchedFor&&) = delete;
    VouchedFor& operator=(VouchedFor&&) = delete;

private:
    QSslConfiguration m_before;
};

// Whether a look that ended so is followed in its time, and not sooner: not stale a while after (more than the
// sooner wait), and stale once its time is over.
inline bool followedInItsTime(const RedfishDevice& device)
{
    QTest::qWait(400);
    if (device.isStale()) {
        return false;
    }
    return QTest::qWaitFor([&device] { return device.isStale(); }, 3000);
}

// Never Basic, never a cookie: whatever the test, no question that reached the controller carried either.
inline bool signsOnToNothing(const RedfishByHand& bmc)
{
    return bmc.authorizations == 0 && bmc.cookies == 0;
}

} // namespace qkvm::core::testing
