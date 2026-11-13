#include <algorithm>
#include <functional>
#include <memory>
#include <utility>

#include <QPointer>
#include <QSignalSpy>
#include <QTest>

#include <qkvm/core/discovery.h>
#include <qkvm/core/tls.h>

#include "stand_in.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::Discovery;
using core::KvmProbe;

namespace {

// No socket, lookup or interface enumeration: the test owns every pending answer and can
// deliver it even after cancellation, as a misbehaving asynchronous source might do.
class Probes : public QObject {
public:
    struct Request {
        QUrl address;
        QPointer<QObject> lifetime;
        std::function<void(KvmProbe::Found)> done;
    };
    QList<Request> requests;
    int active = 0;
    int peak = 0;
    bool answerImmediately = false;

    Discovery::Probe probe()
    {
        return [this](const QUrl& address, QObject* lifetime, std::function<void(KvmProbe::Found)> done) {
            ++active;
            peak = std::max(peak, active);
            connect(lifetime, &QObject::destroyed, this, [this] { --active; });
            requests.append({address, lifetime, std::move(done)});
            if (answerImmediately) {
                answer(static_cast<int>(requests.size()) - 1, KvmProbe::Kind::Kvmd);
            }
        };
    }

    void answer(int index, KvmProbe::Kind kind, const QByteArray& certificate = {})
    {
        KvmProbe::Found found;
        found.kind = kind;
        found.shown = certificate;
        // Copy: answering can eventually append a request and move the list's storage.
        const auto done = requests[index].done;
        done(found);
    }
};

} // namespace

class TestDiscovery : public QObject {
    Q_OBJECT

private slots:
    void plansActualPrefix_data()
    {
        QTest::addColumn<QString>("scope");
        QTest::addColumn<int>("total");
        QTest::addColumn<bool>("confirmation");
        QTest::newRow("24") << u"192.168.1.89/24"_s << 254 << false;
        QTest::newRow("23") << u"192.168.1.89/23"_s << 510 << true;
        QTest::newRow("25") << u"192.168.1.89/25"_s << 126 << false;
        QTest::newRow("20") << u"10.1.12.1/20"_s << 4094 << true;
        QTest::newRow("31") << u"192.168.1.21/31"_s << 2 << false;
        QTest::newRow("32") << u"192.168.1.21/32"_s << 1 << false;
        QTest::newRow("link local") << u"169.254.2.0/24"_s << 254 << false;
        QTest::newRow("range") << u"172.16.0.250 - 172.16.1.4"_s << 11 << false;
        QTest::newRow("limit") << u"10.0.0.0-10.0.15.255"_s << 4096 << true;
        QTest::newRow("address") << u"192.168.1.20"_s << 1 << false;
        QTest::newRow("explicit port") << u"https://192.168.1.20:8443/"_s << 1 << false;
    }

    void plansActualPrefix()
    {
        QFETCH(QString, scope);
        QFETCH(int, total);
        QFETCH(bool, confirmation);
        const auto plan = Discovery::planFor({scope});
        QVERIFY2(plan.ok(), plan.ok() ? "" : plan.error().message.c_str());
        QCOMPARE(plan.value().total, total);
        QCOMPARE(plan.value().needsConfirmation, confirmation);
    }

    void refusesInvalidOrUnboundedScopes_data()
    {
        QTest::addColumn<QString>("scope");
        const QStringList scopes{
            u""_s,
            u"192.168.1.0/19"_s,
            u"0.0.0.0/0"_s,
            u"192.168.1.0/33"_s,
            u"192.168.1.0/-1"_s,
            u"192.168.1.0/+24"_s,
            u"192.168.1.0/999999999999"_s,
            u"192.168.1.0/24/2"_s,
            u"10.0.0.0-10.0.16.0"_s,
            u"255.255.255.255-0.0.0.0"_s,
            u"192.168.1.10-192.168.1.2"_s,
            u"192.168.1.1-8.8.8.8"_s,
            u"172.15.255.254-172.16.0.1"_s,
            u"8.8.8.8"_s,
            u"100.64.0.1"_s,
            u"127.0.0.1"_s,
            u"224.0.0.1"_s,
            u"192.168.1.256"_s,
            u"192.168.1.010"_s,
            u"192.168.1.010/24"_s,
            u"192.168.1"_s,
            u"kvm.example"_s,
            u"https://[::1]"_s,
            u"https://user:password@192.168.1.20"_s,
            u"ftp://192.168.1.20"_s,
            u"https://192.168.1.20:0"_s,
            u"192.168.1.20-30"_s,
        };
        int row = 0;
        for (const QString& scope : scopes) {
            QTest::newRow(qPrintable(QString::number(row++))) << scope;
        }
    }

    void refusesInvalidOrUnboundedScopes()
    {
        QFETCH(QString, scope);
        Probes probes;
        Discovery discovery(probes.probe());
        const auto plan = Discovery::planFor({scope});
        QVERIFY(!plan);
        const auto started = discovery.start({scope}, true);
        QVERIFY(!started);
        QVERIFY(!discovery.running());
        QVERIFY(probes.requests.isEmpty());
    }

    void deduplicatesBeforeCountingAndLimitsCombinedScopes()
    {
        const auto overlaps = Discovery::planFor(
            {u"192.168.1.0/24"_s, u"192.168.1.20-192.168.1.30"_s, u"https://192.168.1.20:443"_s, u"192.168.1.20"_s});
        QVERIFY(overlaps);
        QCOMPARE(overlaps.value().total, 254);
        const auto tooMany = Discovery::planFor({u"10.0.0.0-10.0.15.255"_s, u"10.0.16.0"_s});
        QVERIFY(!tooMany);
        const auto repeatedLimit = Discovery::planFor({u"10.0.0.0-10.0.15.255"_s, u"10.0.0.0-10.0.15.255"_s});
        QVERIFY(repeatedLimit);
        QCOMPARE(repeatedLimit.value().total, Discovery::maxTargets);
    }

    void noTrafficUntilExplicitStartAndLargeConfirmation()
    {
        Probes probes;
        Discovery discovery(probes.probe());
        QSignalSpy progress(&discovery, &Discovery::progress);
        const auto plan = Discovery::planFor({u"192.168.0.0/23"_s});
        QVERIFY(plan);
        QVERIFY(plan.value().needsConfirmation);
        QCoreApplication::processEvents();
        QVERIFY(probes.requests.isEmpty());
        auto started = discovery.start({u"192.168.0.0/23"_s});
        QVERIFY(!started);
        QCOMPARE(started.error().code, std::string("confirmation_required"));
        QVERIFY(probes.requests.isEmpty());
        QVERIFY(progress.isEmpty());
        started = discovery.start({u"192.168.0.0/23"_s}, true);
        QVERIFY(started);
        QVERIFY(discovery.running());
        QVERIFY(probes.requests.isEmpty());
        QVERIFY(progress.isEmpty());
        QTRY_COMPARE(probes.requests.size(), Discovery::maxConcurrent);
        QCOMPARE(progress.front().at(0).toInt(), 0);
        QCOMPARE(progress.front().at(1).toInt(), 510);
        discovery.cancel();
        QCOMPARE(probes.active, 0);
    }

    void boundsConcurrencyAndReportsIncrementally()
    {
        Probes probes;
        probes.answerImmediately = true;
        Discovery discovery(probes.probe());
        QSignalSpy found(&discovery, &Discovery::found);
        QSignalSpy progress(&discovery, &Discovery::progress);
        QSignalSpy finished(&discovery, &Discovery::finished);
        QVERIFY(discovery.start({u"192.168.1.1-192.168.1.100"_s}));
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(finished.front().at(0).toBool(), false);
        QCOMPARE(probes.requests.size(), 100);
        QCOMPARE(probes.peak, Discovery::maxConcurrent);
        QCOMPARE(probes.active, 0);
        QCOMPARE(found.size(), 100);
        QCOMPARE(progress.size(), 101);
        for (int i = 0; i <= 100; ++i) {
            QCOMPARE(progress[i].at(0).toInt(), i);
            QCOMPARE(progress[i].at(1).toInt(), 100);
        }
        QVERIFY(!discovery.running());
    }

    void httpsIsPreferredAndDoesNotProbeHttpAfterIdentification()
    {
        Probes probes;
        Discovery discovery(probes.probe());
        QSignalSpy found(&discovery, &Discovery::found);
        QSignalSpy finished(&discovery, &Discovery::finished);
        QVERIFY(discovery.start({u"192.168.1.20"_s}));
        QTRY_COMPARE(probes.requests.size(), 1);
        QCOMPARE(probes.requests[0].address, QUrl(u"https://192.168.1.20"_s));
        probes.answer(0, KvmProbe::Kind::Kvmd, "certificate"_ba);
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(probes.requests.size(), 1);
        QCOMPARE(found.size(), 1);
        const auto device = qvariant_cast<Discovery::Device>(found[0][0]);
        QCOMPARE(device.address.scheme(), u"https"_s);
        QCOMPARE(device.certificate, "certificate"_ba);
    }

    void anonymousHttpFallbackOnlyForUnspecifiedScheme()
    {
        Probes probes;
        Discovery discovery(probes.probe());
        QSignalSpy found(&discovery, &Discovery::found);
        QSignalSpy progress(&discovery, &Discovery::progress);
        QSignalSpy finished(&discovery, &Discovery::finished);
        QVERIFY(discovery.start({u"192.168.1.20:8443"_s}));
        QTRY_COMPARE(probes.requests.size(), 1);
        probes.answer(0, KvmProbe::Kind::SomethingElse);
        QTRY_COMPARE(probes.requests.size(), 2);
        QCOMPARE(probes.requests[1].address, QUrl(u"http://192.168.1.20:8443"_s));
        QCOMPARE(progress.size(), 1); // Only completed addresses count, not transport attempts.
        QCOMPARE(probes.active, 1);
        probes.answer(1, KvmProbe::Kind::JetKvm);
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(qvariant_cast<Discovery::Device>(found[0][0]).kind, KvmProbe::Kind::JetKvm);
        QCOMPARE(progress.back().at(0).toInt(), 1);

        QVERIFY(discovery.start({u"https://192.168.1.21"_s}));
        QTRY_COMPARE(probes.requests.size(), 3);
        probes.answer(2, KvmProbe::Kind::Nothing);
        QTRY_COMPARE(finished.size(), 2);
        QCOMPARE(probes.requests.size(), 3);

        QVERIFY(discovery.start({u"http://192.168.1.22"_s}));
        QTRY_COMPARE(probes.requests.size(), 4);
        QCOMPARE(probes.requests[3].address.scheme(), u"http"_s);
        probes.answer(3, KvmProbe::Kind::Redfish);
        QTRY_COMPARE(finished.size(), 3);
        QCOMPARE(qvariant_cast<Discovery::Device>(found[1][0]).kind, KvmProbe::Kind::Redfish);
    }

    void explicitHttpsOverridesOverlappingRangeFallback()
    {
        for (const QStringList& scopes : {QStringList{u"192.168.1.20"_s, u"https://192.168.1.20"_s},
                                          QStringList{u"https://192.168.1.20"_s, u"192.168.1.20"_s}}) {
            Probes probes;
            Discovery discovery(probes.probe());
            QSignalSpy finished(&discovery, &Discovery::finished);
            QVERIFY(discovery.start(scopes));
            QTRY_COMPARE(probes.requests.size(), 1);
            probes.answer(0, KvmProbe::Kind::Nothing);
            QTRY_COMPARE(finished.size(), 1);
            QCOMPARE(probes.requests.size(), 1);
        }
    }

    void sharedCertificatesDoNotMergeDistinctHosts()
    {
        Probes probes;
        Discovery discovery(probes.probe());
        QSignalSpy found(&discovery, &Discovery::found);
        QSignalSpy finished(&discovery, &Discovery::finished);
        QVERIFY(discovery.start({u"192.168.1.20-192.168.1.21"_s}));
        QTRY_COMPARE(probes.requests.size(), 2);
        probes.answer(0, KvmProbe::Kind::Kvmd, "same factory certificate"_ba);
        probes.answer(1, KvmProbe::Kind::Kvmd, "same factory certificate"_ba);
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(found.size(), 2);
        QVERIFY(qvariant_cast<Discovery::Device>(found[0][0]).address !=
                qvariant_cast<Discovery::Device>(found[1][0]).address);
    }

    void cancellationImmediatelyDestroysActiveWorkAndRejectsLateResults()
    {
        Probes probes;
        Discovery discovery(probes.probe());
        QSignalSpy found(&discovery, &Discovery::found);
        QSignalSpy finished(&discovery, &Discovery::finished);
        QVERIFY(discovery.start({u"192.168.1.0/24"_s}));
        QTRY_COMPARE(probes.requests.size(), Discovery::maxConcurrent);
        // One result is already queued, and another arrives after cancellation.
        probes.answer(0, KvmProbe::Kind::Kvmd);
        discovery.cancel();
        QCOMPARE(probes.active, 0);
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished[0][0].toBool(), true);
        discovery.cancel();
        QCOMPARE(finished.size(), 1);
        QVERIFY(discovery.start({u"https://192.168.2.20"_s}));
        probes.answer(1, KvmProbe::Kind::Kvmd);
        QTRY_COMPARE(probes.requests.size(), Discovery::maxConcurrent + 1);
        QCOMPARE(found.size(), 0);
        QCOMPARE(finished.size(), 1);
        probes.answer(Discovery::maxConcurrent, KvmProbe::Kind::Redfish);
        QTRY_COMPARE(finished.size(), 2);
        QCOMPARE(found.size(), 1);
        QCOMPARE(qvariant_cast<Discovery::Device>(found[0][0]).address.host(), u"192.168.2.20"_s);
        QCOMPARE(probes.active, 0);
    }

    void refusesConcurrentStartAndCanCancelBeforeTraffic()
    {
        Probes probes;
        Discovery discovery(probes.probe());
        QSignalSpy finished(&discovery, &Discovery::finished);
        QVERIFY(discovery.start({u"192.168.1.20"_s}));
        const auto second = discovery.start({u"192.168.2.20"_s});
        QVERIFY(!second);
        QCOMPARE(second.error().code, std::string("busy"));
        discovery.cancel();
        QCoreApplication::processEvents();
        QCOMPARE(probes.requests.size(), 0);
        QCOMPARE(finished.size(), 1);
    }

    void ownerDestructionStopsWorkWithoutCompletionSignals()
    {
        Probes probes;
        auto discovery = std::make_unique<Discovery>(probes.probe());
        QSignalSpy finished(discovery.get(), &Discovery::finished);
        QVERIFY(discovery->start({u"192.168.1.0/24"_s}));
        QTRY_COMPARE(probes.requests.size(), Discovery::maxConcurrent);
        discovery.reset();
        QCOMPARE(probes.active, 0);
        QCOMPARE(finished.size(), 0);
        probes.answer(0, KvmProbe::Kind::Kvmd);
        QCoreApplication::processEvents();
        QCOMPARE(probes.requests.size(), Discovery::maxConcurrent);
    }

    void cancellationFromAResultStopsMoreRequests()
    {
        Probes probes;
        Discovery discovery(probes.probe());
        QSignalSpy progress(&discovery, &Discovery::progress);
        QSignalSpy finished(&discovery, &Discovery::finished);
        connect(&discovery, &Discovery::found, &discovery, [&discovery] { discovery.cancel(); });
        QVERIFY(discovery.start({u"192.168.1.0/24"_s}));
        QTRY_COMPARE(probes.requests.size(), Discovery::maxConcurrent);
        probes.answer(0, KvmProbe::Kind::Kvmd);
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(finished[0][0].toBool(), true);
        QCOMPARE(probes.active, 0);
        QCOMPARE(probes.requests.size(), Discovery::maxConcurrent);
        QCOMPARE(progress.size(), 1);
    }

    void damagedTlsIsReportedWithoutDowngrade()
    {
        Probes probes;
        Discovery discovery(probes.probe());
        QSignalSpy problem(&discovery, &Discovery::problem);
        QSignalSpy finished(&discovery, &Discovery::finished);
        QVERIFY(discovery.start({u"192.168.1.0/24"_s}));
        QTRY_COMPARE(probes.requests.size(), Discovery::maxConcurrent);
        probes.answer(0, KvmProbe::Kind::NotAsked);
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(problem.size(), 1);
        QCOMPARE(problem[0][0].toString(), QString::fromLatin1(core::tlsDamagedSentence));
        QCOMPARE(probes.active, 0);
        for (const auto& request : probes.requests) {
            QCOMPARE(request.address.scheme(), u"https"_s);
        }
    }

    void oneSearchAcrossWindowsReleasesOnEveryExit()
    {
        Probes firstProbes;
        Probes secondProbes;
        auto first = std::make_unique<Discovery>(firstProbes.probe());
        Discovery second(secondProbes.probe());
        const QStringList scopes{u"https://192.168.1.20"_s};
        QSignalSpy firstFinished(first.get(), &Discovery::finished);
        QSignalSpy secondFinished(&second, &Discovery::finished);
        QVERIFY(first->start(scopes));
        auto blocked = second.start(scopes);
        QVERIFY(!blocked);
        QCOMPARE(blocked.error().code, std::string("busy"));
        QCOMPARE(secondProbes.requests.size(), 0);

        // Cancel releases the application-wide slot, even before any request began.
        first->cancel();
        QVERIFY(second.start(scopes));
        QTRY_COMPARE(secondProbes.requests.size(), 1);
        secondProbes.answer(0, KvmProbe::Kind::Kvmd);
        QTRY_COMPARE(secondFinished.size(), 1);

        // Natural completion also releases it.
        QVERIFY(first->start(scopes));
        QTRY_COMPARE(firstProbes.requests.size(), 1);
        firstProbes.answer(0, KvmProbe::Kind::NotAsked);
        QTRY_COMPARE(firstFinished.size(), 2);

        // A TLS failure releases it, without waiting for dialog destruction.
        QVERIFY(second.start(scopes));
        second.cancel();
        QVERIFY(first->start(scopes));
        QTRY_COMPARE(firstProbes.requests.size(), 2);
        first.reset();
        QCOMPARE(firstProbes.active, 0);

        // Destroying an owning window releases it too.
        QVERIFY(second.start(scopes));
        second.cancel();
    }

    void realAnonymousProbeTalksOnlyToLoopbackStandIn()
    {
        using core::testing::Answered;
        using core::testing::Asked;
        using core::testing::StandIn;
        QList<Asked> requests;
        StandIn stand([&requests](const Asked& asked) {
            requests.append(asked);
            if (asked.path == "/api/auth/check"_ba) {
                return Answered{401,
                                "{\"ok\":false,\"result\":{\"error\":\"UnauthorizedError\",\"error_msg\":\"\"}}"_ba};
            }
            return Answered{404, "{}"_ba};
        });
        QVERIFY(stand.isListening());
        // The only real requests in this suite are explicitly redirected by the test adapter
        // to a loopback server. No private-network address is ever passed to KvmProbe.
        Discovery discovery([&stand](const QUrl&, QObject* lifetime, std::function<void(KvmProbe::Found)> done) {
            auto* probe = new KvmProbe(lifetime); // NOLINT(cppcoreguidelines-owning-memory): owned by the test attempt
            probe->ask(QUrl(stand.address()), std::move(done));
        });
        QSignalSpy found(&discovery, &Discovery::found);
        QSignalSpy finished(&discovery, &Discovery::finished);
        QVERIFY(discovery.start({u"http://192.168.1.20"_s}));
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(found.size(), 1);
        QCOMPARE(qvariant_cast<Discovery::Device>(found[0][0]).kind, KvmProbe::Kind::Kvmd);
        QCOMPARE(requests.size(), 3);
        for (const Asked& asked : requests) {
            QCOMPARE(asked.method, "GET"_ba);
            QVERIFY(!asked.headers.contains("authorization"_ba));
            QVERIFY(!asked.headers.contains("cookie"_ba));
            QVERIFY(!asked.headers.contains("x-kvmd-user"_ba));
            QVERIFY(!asked.headers.contains("x-kvmd-passwd"_ba));
            QVERIFY(asked.body.isEmpty());
        }
    }
};

QTEST_GUILESS_MAIN(TestDiscovery)
#include "test_discovery.moc"
