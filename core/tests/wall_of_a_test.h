#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <utility>

#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QString>
#include <QTest>
#include <QTimer>
#include <QUrl>

#include <qkvm/core/local_wall.h>

// What holds a test's wall to this PC's own address: for the tests of this PC's wall as a file,
// in the core and of the boxes on the screen.
//
// The KVMs of such tests are made up, and are at this PC's own address. A wall may also hold a
// KVM at an address that reaches nothing by the books, for what is only laid out, taken and
// written; and, where a test says so in so many words, the five addresses of the server's own
// example files that this PC's wall takes (its KVMs, its JetKVM and its server's controller), which are what those
// files hold. No wall holds
// anything else, and a wall that holds a KVM that is not at this PC's own address is asked nothing: a test that would,
// has failed, and nothing was contacted. Held by the wall such a test has (WallOfATest), and not by each test's own
// care.
namespace qkvm::core::testing {

// This PC itself, by an address's signs: where a made-up KVM is, and where nobody is.
inline bool isThisPcsOwn(const QString& address)
{
    const QString host = QUrl(address).host();
    return host == QStringLiteral("127.0.0.1") || host == QStringLiteral("localhost") || host == QStringLiteral("::1");
}

// An address that reaches nothing, by the books: numbers of the three ranges that are set aside
// for examples (192.0.2.0/24, 198.51.100.0/24, 203.0.113.0/24), or a name under ".invalid".
inline bool reachesNothingByTheBooks(const QString& address)
{
    const QString host = QUrl(address).host();
    if (host.endsWith(QStringLiteral(".invalid"))) {
        return true;
    }
    const QHostAddress numbers(host);
    return numbers.protocol() == QAbstractSocket::IPv4Protocol &&
           (numbers.isInSubnet(QHostAddress(QStringLiteral("192.0.2.0")), 24) ||
            numbers.isInSubnet(QHostAddress(QStringLiteral("198.51.100.0")), 24) ||
            numbers.isInSubnet(QHostAddress(QStringLiteral("203.0.113.0")), 24));
}

// The KVMs of the files a QKVM server's own writer made (core/tests/data/export): they are at
// addresses of a home's network and at a name that could exist, and are what those files hold.
inline bool isOfTheServersExampleFiles(const QString& address)
{
    return address == QStringLiteral("https://192.168.1.21") || address == QStringLiteral("http://192.168.1.22:8080") ||
           address == QStringLiteral("https://kvm.example.net") || address == QStringLiteral("http://192.168.1.30") ||
           address == QStringLiteral("https://192.168.1.40");
}

// A wall of a test: this PC's own wall, which fails its test rather than have anything asked at
// an address that is not this PC's own.
//
// What it holds off, and what it does not. The five calls of a wall that have a KVM's driver
// speak to its address (askStatus, askSnapshot, act, askWakeTargets, openLive) are refused, and
// the test has failed, while the wall holds a KVM at another address. A box that asks at an
// address by itself (the one that changes a KVM, the one that looks at its certificate) is handed
// the wall by forABox() and by no other way, and is not handed it while the wall holds such a
// KVM. What a box then asks at an address that somebody TYPES into it is that test's own care:
// the wall does not see it. And whatever a wall holds is looked at whenever it changes: an address
// that is neither this PC's own nor one that a wall may hold has failed the test that put it there.
class WallOfATest : public LocalWall {
public:
    // What a wall may hold besides this PC's own addresses and those that reach nothing by the books.
    enum class AlsoHolds {
        NothingElse,
        // The five addresses of the server's own example files: for the tests that take those files.
        TheServersExampleFiles,
    };

    WallOfATest(Settings& settings, SecretStore& secrets, KvmdDevice::Pace pace,
                AlsoHolds alsoHolds = AlsoHolds::NothingElse)
        : LocalWall(settings, secrets, std::move(pace)), m_alsoHolds(alsoHolds)
    {
        connect(this, &LocalWall::changed, this, [this] { lookAtItsAddresses(); });
        lookAtItsAddresses();
    }

    // Whether every KVM of it is at this PC's own address: only then is it asked anything.
    bool holdsOnlyThisPcsOwn() const
    {
        return std::ranges::all_of(kvms(), [](const OwnKvm& kvm) { return isThisPcsOwn(kvm.address); });
    }
    // The wall, for a box that asks at a KVM's address by itself (the one that changes a KVM,
    // the one that looks at its certificate): looked at as before every asking. Where it holds a
    // KVM at another address than this PC's own, the test has failed and the wall is NOT handed
    // over: nothing, and whoever asked has no wall to open a box on.
    LocalWall* forABox()
    {
        if (holdsOnlyThisPcsOwn()) {
            return this;
        }
        fail(QStringLiteral("A wall that holds a KVM at another address than this PC's own was to have a box "
                            "opened on it. It was not handed over."));
        return nullptr;
    }
    // What is done where a test has failed by this wall's rules: the test fails. The test of
    // these rules themselves hands in something that notes what was said, and no other test does.
    void insteadOfFailing(std::function<void(const QString& why)> noted) { m_noted = std::move(noted); }

    void askStatus(std::function<void(base::Result<QHash<QString, core::Status>>)> done) override
    {
        if (mayBeAsked("how its KVMs are")) {
            LocalWall::askStatus(std::move(done));
        } else {
            refuse(std::move(done));
        }
    }
    void askSnapshot(const QString& machine, int width, double mayBeOldSeconds,
                     std::function<void(base::Result<core::Snapshot>)> done) override
    {
        if (mayBeAsked("for a picture")) {
            LocalWall::askSnapshot(machine, width, mayBeOldSeconds, std::move(done));
        } else {
            refuse(std::move(done));
        }
    }
    void act(const QString& machine, const core::Action& what,
             std::function<void(base::Result<core::Done>)> done) override
    {
        if (mayBeAsked("to do something")) {
            LocalWall::act(machine, what, std::move(done));
        } else {
            refuse(std::move(done));
        }
    }
    void askWakeTargets(const QString& machine,
                        std::function<void(base::Result<QList<core::WakeTarget>>)> done) override
    {
        if (mayBeAsked("which computers a KVM can wake")) {
            LocalWall::askWakeTargets(machine, std::move(done));
        } else {
            refuse(std::move(done));
        }
    }
    void openLive(const QString& machine, core::Live::Need need,
                  std::function<void(base::Result<std::unique_ptr<core::Live>>)> done) override
    {
        if (mayBeAsked("for a KVM live")) {
            LocalWall::openLive(machine, need, std::move(done));
        } else {
            refuse(std::move(done));
        }
    }

private:
    void fail(const QString& why) const
    {
        if (m_noted) {
            m_noted(why);
        } else {
            QTest::qFail(qPrintable(why), __FILE__, __LINE__);
        }
    }
    bool mayBeAsked(const char* what) const
    {
        if (holdsOnlyThisPcsOwn()) {
            return true;
        }
        fail(QStringLiteral("A wall that holds a KVM at another address than this PC's own was asked %1. "
                            "The call was refused, and nothing was asked at any address.")
                 .arg(QLatin1StringView(what)));
        return false;
    }
    template <typename Answer>
    void refuse(std::function<void(base::Result<Answer>)> done)
    {
        QTimer::singleShot(0, this, [done = std::move(done)] {
            done(base::Error{"test", "A wall of these tests is asked nothing while it holds another address."});
        });
    }
    // What it holds, whenever that is another: this PC's own, what reaches nothing by the books,
    // or, where the test said so, what the server's example files hold. Anything else has failed
    // the test that put it there.
    void lookAtItsAddresses() const
    {
        for (const OwnKvm& kvm : kvms()) {
            const bool ofTheExamples =
                m_alsoHolds == AlsoHolds::TheServersExampleFiles && isOfTheServersExampleFiles(kvm.address);
            if (!isThisPcsOwn(kvm.address) && !reachesNothingByTheBooks(kvm.address) && !ofTheExamples) {
                fail(QStringLiteral("A wall of these tests holds %1: neither this PC's own address, nor one that "
                                    "reaches nothing by the books.")
                         .arg(kvm.address));
            }
        }
    }

    AlsoHolds m_alsoHolds;
    std::function<void(const QString& why)> m_noted;
};

} // namespace qkvm::core::testing
