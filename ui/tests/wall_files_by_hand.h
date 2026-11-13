#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <stop_token>
#include <thread>

#include <QByteArray>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QTemporaryDir>

#include <qkvm/core/exchange.h>
#include <qkvm/core/kvmd_device.h>
#include <qkvm/core/settings.h>
#include <qkvm/core/wall_exchange.h>
#include <qkvm/core/wiped.h>

#include "exchange_by_hand.h"
#include "memory_secrets.h"
#include "wall_of_a_test.h"

// What the tests of the two boxes for this PC's wall as a file share (of the view, and of the
// screens): a wall that is held to this PC's own address, files of the wall made by hand, and
// the passphrase's work held up for as long as a test wants.
//
// A wall of these tests holds only this PC's own address, or one that reaches nothing by the
// books (192.0.2.x, 198.51.100.x, 203.0.113.x, a name under ".invalid"): anything else has
// failed the test that put it there, and a wall that holds an address that is not this PC's own
// is asked nothing (core::testing::WallOfATest, of the strict kind). A FILE may name an address
// of any kind, to be laid out: it is on no wall until it is taken.
namespace qkvm::ui::testing {

// The times a real KVM is given, made short. (No test of these asks a KVM anything.)
inline core::KvmdDevice::Pace quickDriver()
{
    core::KvmdDevice::Pace pace;
    pace.connectWithinMs = 1500;
    pace.quietMs = 1500;
    pace.transferMs = 1500;
    pace.holdMs = 400;
    pace.longestHoldMs = 1600;
    pace.freshMs = 200;
    pace.firstContactMs = 400;
    pace.patienceMs = 400;
    return pace;
}

// Sealing at a cost that a test can pay a hundred times.
inline core::exchange::Sealing quickly()
{
    return core::exchange::Sealing{core::testing::quickCost, core::exchange::windowsDerive()};
}

// This PC's own wall for one test: its settings in a folder that goes with the test, its secrets
// in memory, and held to this PC's own address.
struct WallOfOneTest {
    WallOfOneTest()
        : file(folder.filePath(QStringLiteral("settings.json"))), settings(file), wall(settings, secrets, quickDriver())
    {}

    // Puts a KVM on the wall as one that was added by hand, with the password the made-up KVMs
    // have; and gives the wall's name for it (empty: it was refused).
    QString put(const QString& address, const QString& name = {})
    {
        core::OwnKvm kvm;
        kvm.address = address;
        kvm.name = name;
        const base::Result<QString> id = wall.add(kvm, QStringLiteral("secret"));
        return id ? id.value() : QString();
    }
    QStringList names() const
    {
        QStringList all;
        for (const core::OwnKvm& kvm : wall.kvms()) {
            all.append(kvm.name);
        }
        return all;
    }

    QTemporaryDir folder;
    QString file;
    core::Settings settings;
    core::testing::MemorySecrets secrets;
    core::testing::WallOfATest wall;
};

// A KVM as a file names one: of the family this program speaks to, with a password.
inline core::exchange::Device kvmAt(const QString& address, const QString& name = {},
                                    const QString& password = QStringLiteral("secret"))
{
    core::exchange::Device device;
    device.address = address;
    device.name = name.isEmpty() ? address.mid(address.indexOf(QStringLiteral("://")) + 3) : name;
    device.user = QStringLiteral("admin");
    device.password = password;
    device.kind = QStringLiteral("kvmd");
    device.tls = QStringLiteral("pinned");
    return device;
}

// The same, with one of the tests' certificates named in the file (.testdata/tls).
inline core::exchange::Device knownBy(core::exchange::Device device, const char* certificate)
{
    QFile file(QStringLiteral(QKVM_TESTDATA "/tls/") + QLatin1StringView(certificate) + QStringLiteral(".crt"));
    const std::optional<core::exchange::KeptCertificate> kept =
        file.open(QIODevice::ReadOnly) ? core::exchange::certificateFrom(QString::fromLatin1(file.readAll()))
                                       : std::nullopt;
    if (kept) {
        device.certificatePem = kept->pem;
        device.certificate = kept->sha256;
    }
    return device;
}

// And with another word for how its certificate is checked ("system", "insecure").
inline core::exchange::Device saying(core::exchange::Device device, const QString& tlsWord)
{
    device.tls = tlsWord;
    return device;
}

// One that cannot be taken onto this PC's wall for what the file has for it: a user name of forty signs from
// far up, which a file holds and which takes eighty places in a text here, more than this wall keeps.
inline core::exchange::Device notKeptHere(core::exchange::Device device)
{
    const char32_t wide = 0x1F600;
    device.user = QString::fromUcs4(&wide, 1).repeated(40);
    return device;
}

// A file that holds these devices: sealed with the tests' passphrase, with their passwords; or
// plain, and then without.
inline QByteArray fileOf(const QList<core::exchange::Device>& devices, bool sealed = true)
{
    core::exchange::Content content;
    content.devices = devices;
    const base::Result<core::exchange::Written> written = core::exchange::writeWith(
        content, core::SecretBytes::utf8Of(sealed ? core::testing::thePassphrase() : QString()), quickly());
    return written ? core::testing::textOf(written.value()) : QByteArray();
}

// A plain file with so many devices that the reader leaves out, each with a sentence (they have
// no address), and after them these, which it takes: a file made by anybody, with as many lines
// beside its devices as it wants to put before them. By hand: no writer of QKVM's writes a
// device without an address.
inline QByteArray fileWithSoManyLeftOut(int leftOut, const QList<core::exchange::Device>& devices)
{
    QJsonArray all;
    for (int number = 1; number <= leftOut; ++number) {
        all.append(QJsonObject{{QStringLiteral("name"), QStringLiteral("Nowhere %1").arg(number)},
                               {QStringLiteral("url"), QString()},
                               {QStringLiteral("username"), QStringLiteral("admin")},
                               {QStringLiteral("kind"), QStringLiteral("kvmd")},
                               {QStringLiteral("group"), QString()},
                               {QStringLiteral("tls"), QStringLiteral("system")},
                               {QStringLiteral("cert_pem"), QJsonValue()},
                               {QStringLiteral("open_url"), QString()},
                               {QStringLiteral("alerts"), true},
                               {QStringLiteral("ident"), QString()}});
    }
    const QJsonArray taken =
        QJsonDocument::fromJson(fileOf(devices, false)).object()[QStringLiteral("devices")].toArray();
    for (qsizetype at = 0; at < taken.size(); ++at) {
        all.append(taken.at(at));
    }
    return core::testing::plainFile({{QStringLiteral("devices"), all}});
}

// The passphrase's work, held up for as long as a test says: a file is "being worked on" from
// when its work begins until the test lets it go. (At the quick cost, once it is let go.)
class HeldWork {
public:
    core::exchange::Sealing sealing() const
    {
        return core::exchange::Sealing{core::testing::quickCost,
                                       [state = m_state](QByteArrayView passphrase, QByteArrayView salt,
                                                         const core::scrypt::Cost& cost,
                                                         const std::stop_token& stop) -> base::Result<core::SecretKey> {
                                           ++state->begun;
                                           while (state->held.load() && !stop.stop_requested()) {
                                               std::this_thread::sleep_for(std::chrono::milliseconds(2));
                                           }
                                           return core::exchange::windowsDerive()(passphrase, salt, cost, stop);
                                       }};
    }
    void hold() { m_state->held = true; }
    void letGo() { m_state->held = false; }
    // How often the passphrase's work was begun.
    int begun() const { return m_state->begun.load(); }

private:
    struct State {
        std::atomic<bool> held{false};
        std::atomic<int> begun{0};
    };
    std::shared_ptr<State> m_state = std::make_shared<State>();
};

} // namespace qkvm::ui::testing
