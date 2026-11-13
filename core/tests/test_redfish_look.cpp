#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSignalSpy>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QTest>
#include <QTimer>

#include <qkvm/core/redfish_answers.h>
#include <qkvm/core/redfish_device.h>
#include <qkvm/core/sign_in_pacing.h>

#include "redfish_by_hand.h"

Q_DECLARE_METATYPE(QList<qkvm::core::testing::RedfishByHand::Image>)
#include "redfish_device_tests.h"
#include "stand_in.h"

using namespace Qt::StringLiterals;
namespace base = qkvm::base;
using namespace qkvm::core::testing;
using qkvm::core::RedfishDevice;
using qkvm::core::SignInPacing;

namespace {

using Image = RedfishByHand::Image;

// What the card and what adding it say of the firmware of a controller laid out as OpenBMC (the web's firmware_said):
// the one, after checking the two agree.
QString firmwareSaid(RedfishByHand& bmc, RedfishDevice::Pace pace = quick())
{
    RedfishDevice device(at(bmc), u"secret"_s, std::move(pace));
    const auto what = identified(device);
    if (!what) {
        return u"(not identified: %1)"_s.arg(QString::fromStdString(what.error().message));
    }
    const qkvm::core::Status status = lookedAt(device);
    if (status.version != what.value().version) {
        return u"(adding said %1, the card %2)"_s.arg(what.value().version, status.version);
    }
    return status.version;
}

QStringList inventoryAsked(const RedfishByHand& bmc)
{
    QStringList asked;
    for (const QString& one : bmc.requests) {
        if (one.contains(u"/FirmwareInventory/"_s)) {
            asked.append(one);
        }
    }
    return asked;
}

} // namespace

// A server's management controller spoken to by the program itself over Redfish (redfish_device.h),
// against a stand-in that answers as the web's does (redfish_by_hand.h): how it is, as its controller says it (1a-v).
// Ported from the web's own tests where they are named (../app/tests).
class TestRedfishLook : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // The driver chooses Windows' own TLS, which is refused once TLS is in use: so one is made before
        // anything else touches TLS.
        const RedfishDevice first(RedfishDevice::Where{}, QString(), quick());
    }

    void l1HowAServerIsSeen()
    {
        // (test_redfish.py:63) As its card shows it: no picture, its power known, the hotter of its processors. (A
        // controller with no console that qKVM reads, so that its screen is said to be none once that is known; its
        // screen where it has one: test_wall_server_screens.)
        RedfishByHand bmc;
        bmc.ownConsole = false;
        RedfishDevice device(at(bmc), u"secret"_s, quick());
        const qkvm::core::Status status = lookedAt(device);
        QCOMPARE(status.state, u"online"_s);
        QVERIFY(!status.hasPicture);
        QVERIFY(status.powerIsKnown);
        QCOMPARE(status.power, std::optional<bool>(true));
        QCOMPARE(status.temperature, std::optional<double>(61));
        QCOMPARE(status.model, u"SRV-2200"_s);
        QCOMPARE(status.version, u"1.42.0"_s);
        const qkvm::core::ServerReadings server = readingsOf(status);
        QCOMPARE(server.health, u"OK"_s);
        QCOMPARE(server.processors, 2);
        QCOMPARE(server.memoryGib, std::optional<double>(256));
        QCOMPARE(server.processor, u"Example Processor 4000"_s);
        // The fan that is not fitted is not counted; the one the controller is not happy with is.
        QCOMPARE(server.fans, 2);
        QCOMPARE(server.fansRunning, 2);
        QCOMPARE(server.fansNotWell, 1);
        // Its screen, live and still, is had where it is not known that it has no console (the web's `screen`: its
        // console is asked for with the first picture).
        QCOMPARE(status.capabilities, std::optional<QStringList>({u"power"_s, u"reset"_s, u"power_state"_s, u"reboot"_s,
                                                                  u"locator"_s, u"screen"_s, u"live"_s}));
        QVERIFY(status.hasScreen);
        QVERIFY(status.lastSeenSecondsAgo.has_value());
        // It has none: no picture of its screen, live or still, from then on, and no computer to wake.
        base::Result<qkvm::core::Snapshot> picture = base::Error{"never", "Not asked."};
        QVERIFY(QTest::qWaitFor(
            [&] {
                QList<base::Result<qkvm::core::Snapshot>> pictures;
                device.askSnapshot(640,
                                   [&](base::Result<qkvm::core::Snapshot> got) { pictures.append(std::move(got)); });
                if (!QTest::qWaitFor([&] { return !pictures.isEmpty(); }, 10000)) {
                    return false;
                }
                picture = pictures.first();
                return !picture && picture.error().code != "no_stream";
            },
            20000));
        QCOMPARE(picture.error().code, std::string("no_picture"));
        QCOMPARE(picture.error().message, std::string("A management controller has no picture of the screen"));
        QVERIFY(!device.status().hasScreen);
        QCOMPARE(device.status().capabilities,
                 std::optional<QStringList>({u"power"_s, u"reset"_s, u"power_state"_s, u"reboot"_s, u"locator"_s}));
        QList<base::Result<QList<qkvm::core::WakeTarget>>> wakes;
        device.askWakeTargets([&](base::Result<QList<qkvm::core::WakeTarget>> got) { wakes.append(std::move(got)); });
        QTRY_COMPARE(wakes.size(), 1);
        QVERIFY(wakes.first() && wakes.first().value().isEmpty());
        const std::unique_ptr<qkvm::core::OwnLive> live = device.makeLive();
        const std::unique_ptr<qkvm::core::Live> one = live->open(qkvm::core::Live::Need{});
        QSignalSpy closed(one.get(), &qkvm::core::Live::closed);
        QTRY_COMPARE(closed.size(), 1);
        QCOMPARE(closed.first().first().toString(), u"no_picture"_s);
        QVERIFY(signsOnToNothing(bmc));
    }

    void l2ALookIsFreshForItsTime()
    {
        // (test_redfish.py:133) Its controller is a small computer, and a server's state changes slowly: a look
        // every so often; where it keeps things, and its version, more rarely still.
        RedfishByHand bmc;
        RedfishDevice::Pace pace = quick();
        pace.freshMs = 600;
        RedfishDevice device(at(bmc), u"secret"_s, pace);
        QVERIFY(device.isStale());
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QVERIFY(!device.isStale());
        const qsizetype asked = bmc.requests.size();
        QTRY_VERIFY_WITH_TIMEOUT(device.isStale(), 2000);
        QCOMPARE(lookedAt(device).state, u"online"_s);
        // The computer, judged, and its sensors: nothing else.
        QCOMPARE(bmc.requests.mid(asked),
                 (QStringList{u"GET "_s + bmc.system(), u"GET /redfish/v1/Chassis/Self/Thermal"_s}));
    }

    void l3AServerThatIsOffIsThereAndDark()
    {
        // (test_redfish.py:147)
        RedfishByHand bmc;
        bmc.power = u"Off"_s;
        RedfishDevice device(at(bmc), u"secret"_s, quick());
        const qkvm::core::Status status = lookedAt(device);
        QCOMPARE(status.state, u"no_signal"_s);
        QCOMPARE(status.power, std::optional<bool>(false));
        QCOMPARE(status.signal, std::optional<bool>(false));
    }

    void l4AFaultThatComesAndGoesIsOneProblem()
    {
        // (test_redfish.py:799, 1169) A fan at its limit, on a real board: Critical for fifteen seconds, Warning for
        // five, Critical again, all evening. One problem, said once, and over when it has stayed away.
        RedfishByHand bmc;
        bmc.health = u"Critical"_s;
        bmc.temperatureHealth = u"Critical"_s;
        RedfishDevice::Pace pace = quick();
        pace.faultHoldMs = 1500;
        RedfishDevice device(at(bmc), u"secret"_s, pace);
        qkvm::core::Status status = lookedAt(device);
        QCOMPARE(status.state, u"error"_s);
        QCOMPARE(status.errorCode, u"health"_s);
        QCOMPARE(status.errorMessage, u"The server reports a critical fault (CPU0_TEMP, CPU1_FAN)"_s);
        QCOMPARE(status.power, std::optional<bool>(true)); // (still told how it is)
        bmc.health = u"Warning"_s;
        status = lookedAt(device);
        QCOMPARE(status.state, u"error"_s);
        QVERIFY(status.errorMessage.contains(u"comes and goes"_s));
        QCOMPARE(readingsOf(status).health, u"Warning"_s);
        bmc.health = u"Critical"_s;
        QCOMPARE(lookedAt(device).errorMessage, u"The server reports a critical fault (CPU0_TEMP, CPU1_FAN)"_s);
        bmc.health = u"Warning"_s;
        QCOMPARE(lookedAt(device).state, u"error"_s);
        QTest::qWait(pace.faultHoldMs + 200);
        QCOMPARE(lookedAt(device).state, u"online"_s);
    }

    void l5AMomentOfTheControllersDoesNotChangeWhatItsCardSays()
    {
        // (test_redfish.py:1039) A real controller answered errors for twenty-odd seconds at a stretch while its
        // server shut down: what was known a moment ago stands, and it is asked again sooner.
        RedfishByHand bmc;
        RedfishDevice::Pace pace = quick();
        pace.hiccupMs = 5000;
        pace.freshMs = 5000;
        RedfishDevice device(at(bmc), u"secret"_s, pace);
        const qkvm::core::Status before = lookedAt(device);
        QCOMPARE(before.state, u"online"_s);
        bmc.unwellFor(800);
        const qkvm::core::Status during = lookedAt(device);
        QCOMPARE(during.state, u"online"_s);
        QCOMPARE(readingsOf(during).fans, readingsOf(before).fans);
        QTRY_VERIFY_WITH_TIMEOUT(device.isStale(), 1000);
        QTest::qWait(900);
        QCOMPARE(lookedAt(device).state, u"online"_s);
        bmc.power = u"Off"_s;
        QTRY_VERIFY_WITH_TIMEOUT(device.isStale(), 6000);
        QCOMPARE(lookedAt(device).state, u"no_signal"_s);
    }

    void l6AControllerThatStaysAwayIsSaidToBeAway()
    {
        // (test_redfish.py:1064)
        RedfishByHand bmc;
        RedfishDevice::Pace pace = quick();
        pace.hiccupMs = 600;
        RedfishDevice device(at(bmc), u"secret"_s, pace);
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QElapsedTimer watch;
        watch.start();
        bmc.unwellFor(60'000);
        qkvm::core::Status status = lookedAt(device);
        QCOMPARE(status.state, u"online"_s);
        while (status.state == u"online"_s && watch.elapsed() < 5000) {
            QTest::qWait(100);
            status = lookedAt(device);
        }
        QCOMPARE(status.state, u"error"_s);
        QVERIFY(watch.elapsed() > 500 && watch.elapsed() < 4000);
        QVERIFY(status.errorMessage.contains(u"HTTP 503"_s));
        QVERIFY(!status.server);
        bmc.unwellFor(0);
        QCOMPARE(lookedAt(device).state, u"online"_s);
    }

    void l7NotBeingLetInIsNoPassingMoment()
    {
        // (test_redfish.py:1078) Said at once: nobody should wait a minute to learn the password is wrong.
        RedfishByHand bmc;
        RedfishDevice::Pace pace = quick();
        pace.hiccupMs = 60'000;
        RedfishDevice device(at(bmc), u"secret"_s, pace);
        QCOMPARE(lookedAt(device).state, u"online"_s);
        bmc.password = u"changed on the controller"_s;
        bmc.sessions.clear();
        const qkvm::core::Status status = lookedAt(device);
        QCOMPARE(status.state, u"auth"_s);
        QCOMPARE(status.errorCode, u"auth_failed"_s);
    }

    void l8AServerThatStopsAnsweringIsOfflineAndComesBack()
    {
        // (test_redfish.py:1152) The controller restarted: it knows none of the old sessions, and is signed in to
        // again by itself, once.
        auto first = std::make_unique<RedfishByHand>();
        first->beServer(aSystem, aRoot);
        const quint16 port = first->stand->serverPort();
        RedfishDevice device(at(*first), u"secret"_s, quick());
        QCOMPARE(lookedAt(device).state, u"online"_s);
        first.reset();
        const qkvm::core::Status gone = lookedAt(device);
        QCOMPARE(gone.state, u"offline"_s);
        QVERIFY(gone.errorCode == u"unreachable"_s || gone.errorCode == u"dropped"_s);
        RedfishByHand second(RedfishByHand::Layout::Ami, {}, port);
        QVERIFY(second.stand->isListening());
        second.beServer(aSystem, aRoot);
        QTRY_VERIFY_WITH_TIMEOUT(lookedAt(device).state == u"online"_s, 10000);
        QCOMPARE(second.logins, 1);
    }

    void l9ARootThatIsSlowWithoutASessionIsWaitedFor()
    {
        // (test_redfish.py:2050, redfish.py:131-134) One board answered its root, with no session, in 12 to 20.5
        // seconds: asked before the password goes, it is given twice a read's wait (made short here).
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        bmc.rootUnsignedTakesMs = 2000;
        RedfishDevice::Pace pace = quick();
        pace.rootUnsignedMs = 3000;
        RedfishDevice device(keptAs(bmc, markOf(aSystem) + digestOf(aRoot)), u"secret"_s, pace);
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QCOMPARE(bmc.rootsUnsigned, 1);
    }

    void l10TheNewerShapeOfTheSensors()
    {
        // (test_controllers.py:3-7; fake_redfish.py, the openbmc layout) Temperatures as readings, fans one each.
        RedfishByHand bmc(RedfishByHand::Layout::OpenBmc);
        RedfishDevice device(at(bmc), u"secret"_s, quick());
        const qkvm::core::Status status = lookedAt(device);
        QCOMPARE(status.state, u"online"_s);
        QCOMPARE(status.temperature, std::optional<double>(61));
        QCOMPARE(readingsOf(status).fans, 2);
        QCOMPARE(readingsOf(status).fansNotWell, 1);
        QVERIFY(bmc.requests.contains(u"GET /redfish/v1/Chassis/chassis/ThermalSubsystem/ThermalMetrics"_s));
        QVERIFY(bmc.requests.contains(u"GET /redfish/v1/Chassis/chassis/ThermalSubsystem/Fans/fan_CPU1_FAN"_s));
        QVERIFY(!bmc.requests.contains(u"GET /redfish/v1/Chassis/chassis/Thermal"_s));
        QCOMPARE(status.version, u"1.42.0"_s);
    }

    void l11TheOlderShapeIsTakenWhereAControllerHasBoth()
    {
        // (redfish.py:860-862) An iDRAC gives both: the one resource is read, and not one a fan.
        RedfishByHand bmc;
        bmc.alsoNewerShape = true;
        RedfishDevice device(at(bmc), u"secret"_s, quick());
        const qkvm::core::Status status = lookedAt(device);
        QCOMPARE(readingsOf(status).fans, 2);
        QVERIFY(bmc.requests.contains(u"GET /redfish/v1/Chassis/Self/Thermal"_s));
        for (const QString& one : std::as_const(bmc.requests)) {
            QVERIFY2(!one.contains(u"ThermalSubsystem"_s), qPrintable(one));
        }
    }

    void l12TheChassisTheComputerNamesIsTheOneWhoseSensorsAreRead()
    {
        // (test_controllers.py:44) An iDRAC lists its disk enclosures as chassis of their own, and may list one first.
        RedfishByHand bmc;
        bmc.enclosuresFirst = true;
        RedfishDevice device(at(bmc), u"secret"_s, quick());
        const qkvm::core::Status status = lookedAt(device);
        QCOMPARE(status.temperature, std::optional<double>(61));
        QCOMPARE(readingsOf(status).fans, 2);
        for (const QString& one : std::as_const(bmc.requests)) {
            QVERIFY2(!one.contains(u"Enclosure"_s), qPrintable(one));
        }
    }

    void l13SlowFansAreReadInTurnAndKept()
    {
        // (redfish.py:872-926) Forty fans answered slowly would hold one look for minutes: the sensors get so long
        // of a look, the fans read longest ago go first, and the others keep what was last read of them.
        RedfishByHand bmc(RedfishByHand::Layout::OpenBmc);
        bmc.moreFans = 37; // (forty places: three fans of its own, one not fitted, and 37 more)
        bmc.lastFanUnwell = true;
        bmc.fanTakesMs = 120;
        RedfishDevice::Pace pace = quick();
        pace.sensorsWithinMs = 700;
        RedfishDevice device(at(bmc), u"secret"_s, pace);
        qkvm::core::Status status = lookedAt(device);
        QCOMPARE(status.state, u"online"_s);
        int fans = readingsOf(status).fans;
        QVERIFY2(fans > 0 && fans < 39, qPrintable(QString::number(fans)));
        for (int look = 0; look < 15 && readingsOf(status).fans < 39; ++look) {
            status = lookedAt(device);
            QVERIFY(readingsOf(status).fans >= fans); // (what was read before is kept)
            fans = readingsOf(status).fans;
        }
        QCOMPARE(readingsOf(status).fans, 39);
        QCOMPARE(readingsOf(status).fansNotWell, 2);
    }

    void l14NoMoreThanFortyFansAreAskedFor()
    {
        RedfishByHand bmc(RedfishByHand::Layout::OpenBmc);
        bmc.moreFans = 45;
        RedfishDevice device(at(bmc), u"secret"_s, quick());
        const qkvm::core::Status status = lookedAt(device);
        QCOMPARE(readingsOf(status).fans, 39);
        QVERIFY(!bmc.requests.contains(u"GET /redfish/v1/Chassis/chassis/ThermalSubsystem/Fans/fan_EXTRA_40"_s));
    }

    void l15WhichServerAnswersIsAskedAtEveryLook()
    {
        // (redfish.py:2052-2054) What its card shows of a server is this one's: another one's power, sensors and
        // name are not shown as this one's, and its last good look is not said again for a moment of the other's.
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        RedfishDevice::Pace pace = quick();
        pace.hiccupMs = 60'000;
        RedfishDevice device(keptAs(bmc, markOf(aSystem) + digestOf(aRoot)), u"secret"_s, pace);
        QCOMPARE(lookedAt(device).state, u"online"_s);
        bmc.beServer(bSystem,
                     aRoot); // (its controller says this card's number: the password goes, the computer is asked)
        bmc.power = u"Off"_s;
        qkvm::core::Status status = lookedAt(device);
        QCOMPARE(status.state, u"error"_s);
        QCOMPARE(status.errorCode, u"another_machine"_s);
        QVERIFY(!status.server);
        QCOMPARE(status.power, std::nullopt);
        bmc.unwellFor(1000);
        status = lookedAt(device);
        QCOMPARE(status.state, u"error"_s);
        QVERIFY(!status.server);
        bmc.unwellFor(0);
        bmc.beServer(aSystem, aRoot);
        bmc.power = u"On"_s;
        QCOMPARE(lookedAt(device).state, u"online"_s);
    }

    void l16WhileAPasswordWaitsItIsAskedWhetherItIsThere()
    {
        // (kvm.py:1317-1340) While a refused password waits to be offered again, nothing is asked that needs a
        // sign-in: its root is asked, with no session, now and then, and a controller that went is said to be gone.
        RedfishByHand bmc;
        RedfishDevice::Pace pace = quick();
        pace.refusedHoldMs = 5000;
        pace.stillThereEveryMs = 300;
        RedfishDevice device(at(bmc), u"secret"_s, pace);
        QCOMPARE(lookedAt(device).state, u"online"_s);
        bmc.password = u"changed on the controller"_s;
        bmc.sessions.clear();
        QCOMPARE(lookedAt(device).state, u"auth"_s);
        const int roots = bmc.rootsUnsigned;
        QCOMPARE(lookedAt(device).state, u"auth"_s);
        QCOMPARE(bmc.rootsUnsigned, roots); // (it answered a moment ago, with the no: not asked at every look)
        QTest::qWait(400);
        bmc.stand->takeNoMore();
        const qkvm::core::Status status = lookedAt(device);
        QCOMPARE(status.state, u"offline"_s);
        QCOMPARE(status.errorCode, u"unreachable"_s);
        QCOMPARE(bmc.logins, 2);
    }

    void l17TheProgramsTimesAreTheWebs()
    {
        // (redfish.py:96-134, kvm.py:199-219) What a controller is given, as the web gives it.
        const RedfishDevice::Pace pace;
        QCOMPARE(pace.connectWithinMs, 4500);
        QCOMPARE(pace.quietMs, 20'000);
        QCOMPARE(pace.transferMs, 24'000);
        QCOMPARE(pace.signInQuietMs, 20'000);
        QCOMPARE(pace.rootUnsignedMs, 40'000);
        QCOMPARE(pace.holdMs, 1000);
        QCOMPARE(pace.longestHoldMs, 15'000);
        QCOMPARE(pace.refusedHoldMs, 60'000);
        QCOMPARE(pace.signInRetryMs, 10'000);
        QCOMPARE(pace.freshMs, 10'000);
        QCOMPARE(pace.retryLookMs, 4000);
        QCOMPARE(pace.hiccupMs, 45'000);
        QCOMPARE(pace.faultHoldMs, 300'000);
        QCOMPARE(pace.followUpMs, (QList<int>{2000, 4000, 6000, 8000, 11'000, 15'000}));
        QCOMPARE(pace.confirmWithinMs, 25'000);
        QCOMPARE(pace.confirmEveryMs, 2000);
        QCOMPARE(pace.signOutWithinMs, 5000);
        QCOMPARE(pace.placesFreshMs, 600'000);
        QCOMPARE(pace.aboutRetryMs, 60'000);
        QCOMPARE(pace.sensorsWithinMs, 15'000);
        QCOMPARE(pace.stillThereEveryMs, 30'000);
    }

    void r10AServerWithNothingToKnowItByIsSeenWhole()
    {
        // (test_twins.py:139) A server whose computer and controller give no number is judged by nothing, at every
        // look: its sensors and its version are its own all the same.
        RedfishByHand bmc;
        bmc.beServer(zero, QJsonValue());
        RedfishDevice device(keptAs(bmc, QString()), u"secret"_s, quick());
        for (int look = 0; look < 2; ++look) {
            const qkvm::core::Status status = lookedAt(device);
            QCOMPARE(status.state, u"online"_s);
            QCOMPARE(status.temperature, std::optional<double>(61));
            QCOMPARE(readingsOf(status).fans, 2);
            QCOMPARE(status.version, u"1.42.0"_s);
        }
        QCOMPARE(device.machine(), QString());
    }

    void r11WhatWasRefusedUnderASessionThatEndedIsAskedAgainUnderTheNext()
    {
        // The controller ends the session between its computer and its sensors (its time; somebody else's sign-in on a
        // board that keeps one session an account): the sensors are refused under it, as an operator's are. Under the
        // next session they are asked again, and are there.
        RedfishByHand bmc;
        RedfishDevice device(at(bmc), u"secret"_s, quick());
        QCOMPARE(lookedAt(device).temperature, std::optional<double>(61));
        bool once = false;
        bmc.whenAsked = [&](const Asked& asked) {
            if (once || asked.path != bmc.system().toLatin1()) {
                return;
            }
            once = true;
            QTimer::singleShot(0, &bmc, [&bmc] { bmc.sessions.clear(); }); // (after its computer's answer)
        };
        QVERIFY(!lookedAt(device).temperature);
        const qkvm::core::Status next = lookedAt(device);
        QCOMPARE(next.state, u"online"_s);
        QCOMPARE(next.temperature, std::optional<double>(61));
        QCOMPARE(bmc.logins, 2);
        bmc.whenAsked = nullptr;
    }

    void r12A401UnderASessionThatNeverAnsweredIsTheSessionAndNotTheResource()
    {
        // The first question a session made anew is asked is a detail (a fan, while the question it was made for waits
        // for a slow computer), and the controller has forgotten that session already: that is the session, signed in
        // to again, and not the fan refused to the account.
        RedfishByHand bmc(RedfishByHand::Layout::OpenBmc);
        bmc.beServer(aSystem, aRoot);
        bmc.moreFans = 1;
        RedfishDevice device(keptAs(bmc, markOf(aSystem) + digestOf(aRoot)), u"secret"_s, quick());
        QCOMPARE(readingsOf(lookedAt(device)).fans, 3);
        QList<base::Result<RedfishDevice::Identity>> identity;
        bool once = false;
        bool forget = false;
        QByteArray first;
        bmc.fanTakesMs = 200;
        bmc.whenAsked = [&](const Asked& asked) {
            if (!asked.path.contains("/Fans/fan_")) {
                return;
            }
            const QByteArray token = asked.headers.value("x-auth-token");
            if (!once) {
                once = true;
                first = token;
                QTimer::singleShot(0, &bmc, [&] {
                    bmc.sessions.clear(); // (the session the look began under: what it is is signed in anew)
                    bmc.systemTakesMs = 1200;
                    forget = true;
                    device.identify(
                        [&identity](base::Result<RedfishDevice::Identity> said) { identity.append(std::move(said)); });
                });
                return;
            }
            if (forget && token != first) {
                forget = false;
                bmc.sessions.clear(); // (and the one made anew, at the first question asked under it)
            }
        };
        const int before = bmc.logins;
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QVERIFY(identity.isEmpty()); // (the question the session was made for still out: else this proves nothing)
        QCOMPARE(bmc.logins, before + 2);
        QTRY_COMPARE_WITH_TIMEOUT(identity.size(), 1, 5000);
        bmc.whenAsked = nullptr;
    }

    void r13ADetailRefusedUnderTwoSessionsIsTheDetailRefusedAndNotTheSessions()
    {
        // A fan the account may not read, first asked under a session made anew for another question (as in r12), and
        // again under the next: the fan is refused, and not the sessions. Only what is not a detail may say they are.
        RedfishByHand bmc(RedfishByHand::Layout::OpenBmc);
        bmc.beServer(aSystem, aRoot);
        bmc.moreFans = 1;
        const RedfishDevice::Pace pace = quick();
        RedfishDevice device(keptAs(bmc, markOf(aSystem) + digestOf(aRoot)), u"secret"_s, pace);
        QCOMPARE(readingsOf(lookedAt(device)).fans, 3);
        QList<base::Result<RedfishDevice::Identity>> identity;
        bool once = false;
        bmc.fanTakesMs = 200;
        bmc.whenAsked = [&](const Asked& asked) {
            if (once || !asked.path.contains("/Fans/fan_")) {
                return;
            }
            once = true;
            QTimer::singleShot(0, &bmc, [&] {
                bmc.sessions.clear(); // (what it is is signed in anew; the fans after this go under that session)
                bmc.systemTakesMs = 1200;
                bmc.unauthorizedAt = u"/redfish/v1/Chassis/chassis/ThermalSubsystem/Fans/fan_CPU1_FAN"_s;
                device.identify(
                    [&identity](base::Result<RedfishDevice::Identity> said) { identity.append(std::move(said)); });
            });
        };
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QVERIFY(!pace.pacing->standing(SignInPacing::what(u"session", bmc.address(), u"admin"_s, u"secret"_s)));
        QTRY_COMPARE_WITH_TIMEOUT(identity.size(), 1, 5000);
        bmc.whenAsked = nullptr;
    }

    // --- its version out of its firmware inventory (2b) ---------------------------------------------------------

    void v1AnOpenBmcThatSaysNoVersionInItsManagerSaysItInItsFirmwareInventory()
    {
        // (test_controllers.py:179) OpenBMC's romulus image in QEMU says neither its version nor the image it runs in
        // its manager: bmcweb says those only where it knows which image runs. Its inventory has the image, naming the
        // controller as what it is for. (The first in the list is the host's.)
        RedfishByHand neither(RedfishByHand::Layout::OpenBmc);
        neither.managerSays = u"neither"_s;
        QCOMPARE(firmwareSaid(neither), u"1.42.0"_s);
        QVERIFY(neither.requests.contains(u"GET /redfish/v1/UpdateService/FirmwareInventory/7ed25e88"_s));
        // Said by the controller, the inventory is not asked; where it names the image it runs, that image is read.
        RedfishByHand both(RedfishByHand::Layout::OpenBmc);
        both.firmware = u"1.43.0"_s;
        QCOMPARE(firmwareSaid(both), u"1.43.0"_s);
        for (const QString& one : std::as_const(both.requests)) {
            QVERIFY2(!one.contains(u"UpdateService"_s), qPrintable(one));
        }
        RedfishByHand image(RedfishByHand::Layout::OpenBmc);
        image.managerSays = u"image"_s;
        image.images = QList<Image>{{u"aa11"_s, u"BMC"_s, u"StandbySpare"_s, u"1.41.0"_s},
                                    {u"bb22"_s, u"BMC"_s, u"Enabled"_s, u"1.42.7"_s}};
        QCOMPARE(firmwareSaid(image), u"1.42.7"_s);
        QCOMPARE(inventoryAsked(image), (QStringList{u"GET /redfish/v1/UpdateService/FirmwareInventory/bb22"_s,
                                                     u"GET /redfish/v1/UpdateService/FirmwareInventory/bb22"_s}));
    }

    void v2TheInventoryIsReadByKindAndOnlyTheControllersOwnActiveImageCounts_data()
    {
        QTest::addColumn<QList<Image>>("images");
        QTest::addColumn<QJsonValue>("imageLink");
        QTest::addColumn<QString>("said");
        const QJsonValue none;
        // The active one of the controller's images, and not the one standing by, nor the host's.
        QTest::newRow("the active one") << QList<Image>{{u"aa11"_s, u"BMC"_s, u"StandbySpare"_s, u"1.41.0"_s},
                                                        {u"cc33"_s, u"Host"_s, u"Enabled"_s, u"9.9"_s},
                                                        {u"bb22"_s, u"BMC"_s, u"Enabled"_s, u"1.42.0"_s}}
                                        << none << u"1.42.0"_s;
        // Two active ones that say two versions: which runs is not known, and is not guessed. Two that say one: that.
        QTest::newRow("two of two versions") << QList<Image>{{u"aa11"_s, u"BMC"_s, u"Enabled"_s, u"1.41.0"_s},
                                                             {u"bb22"_s, u"BMC"_s, u"Enabled"_s, u"1.42.0"_s}}
                                             << none << QString();
        QTest::newRow("two of one version") << QList<Image>{{u"aa11"_s, u"BMC"_s, u"Enabled"_s, u"1.42.0"_s},
                                                            {u"bb22"_s, u"BMC"_s, u"Enabled"_s, u"1.42.0"_s}}
                                            << none << u"1.42.0"_s;
        // None active: none.
        QTest::newRow("none active") << QList<Image>{{u"aa11"_s, u"BMC"_s, u"Disabled"_s, u"1.42.0"_s}} << none
                                     << QString();
        // An image for another controller, or for a place that is no place on this one, is not this controller's.
        const QList<Image> one{{u"aa11"_s, u"BMC"_s, u"Enabled"_s, u"1.42.0"_s}};
        QTest::newRow("for another controller") << one << QJsonValue(u"/redfish/v1/Managers/other"_s) << QString();
        QTest::newRow("for a place elsewhere")
            << one << QJsonValue(u"https://elsewhere.example/redfish/v1/Managers/bmc"_s) << QString();
        QTest::newRow("for a place that climbs")
            << one << QJsonValue(u"/redfish/v1/Managers/../Managers/bmc"_s) << QString();
        QTest::newRow("for a list") << one << QJsonValue(QJsonArray{u"/redfish/v1/Managers/bmc"_s}) << QString();
        // A version that is no text is none; a long one is cut, and said on one line.
        QTest::newRow("a version that is no text")
            << QList<Image>{{u"aa11"_s, u"BMC"_s, u"Enabled"_s, QJsonObject{{u"major"_s, 1}}}} << none << QString();
        QTest::newRow("a long version") << QList<Image>{{u"aa11"_s, u"BMC"_s, u"Enabled"_s,
                                                         QString(u"1.42.0\n"_s + QString(300, u'x'))}}
                                        << none << QString(u"1.42.0 "_s + QString(33, u'x'));
    }

    void v2TheInventoryIsReadByKindAndOnlyTheControllersOwnActiveImageCounts()
    {
        QFETCH(QList<Image>, images);
        QFETCH(QJsonValue, imageLink);
        QFETCH(QString, said);
        RedfishByHand bmc(RedfishByHand::Layout::OpenBmc);
        bmc.managerSays = u"neither"_s;
        bmc.images = images;
        if (!imageLink.isNull()) {
            bmc.imageLink = imageLink;
        }
        QCOMPARE(firmwareSaid(bmc), said);
    }

    void v3AnInventoryNotAllOfWhichCanBeReadSaysNoVersion_data()
    {
        QTest::addColumn<QJsonArray>("more");
        QTest::addColumn<QJsonObject>("says");
        // One of its images not read may be the other one that is active.
        QTest::newRow("a member that is no place here")
            << QJsonArray{QJsonObject{
                   {u"@odata.id"_s, u"https://elsewhere.example/redfish/v1/UpdateService/FirmwareInventory/x"_s}}}
            << QJsonObject();
        QTest::newRow("a member that is no record")
            << QJsonArray{u"/redfish/v1/UpdateService/FirmwareInventory/x"_s} << QJsonObject();
        QTest::newRow("more on another page")
            << QJsonArray()
            << QJsonObject{{u"Members@odata.nextLink"_s, u"/redfish/v1/UpdateService/FirmwareInventory?$skip=2"_s}};
        QTest::newRow("more than the list holds") << QJsonArray() << QJsonObject{{u"Members@odata.count"_s, 40}};
    }

    void v3AnInventoryNotAllOfWhichCanBeReadSaysNoVersion()
    {
        QFETCH(QJsonArray, more);
        QFETCH(QJsonObject, says);
        RedfishByHand bmc(RedfishByHand::Layout::OpenBmc);
        bmc.managerSays = u"neither"_s;
        bmc.inventoryMore = more;
        bmc.inventorySays = says;
        QCOMPARE(firmwareSaid(bmc), QString());
    }

    void v4ALongInventoryIsNotRead()
    {
        // (test_controllers.py:234) Not asked about each of dozens; nor is what the first few say taken (one not read
        // may be the other active image of the controller).
        QList<Image> listed;
        for (int at = 0; at < 200; ++at) {
            listed.append({u"i%1"_s.arg(at, 3, 10, u'0'), u"Host"_s, u"Enabled"_s, u"9.%1"_s.arg(at)});
        }
        listed.append({u"zz"_s, u"BMC"_s, u"Enabled"_s, u"1.42.0"_s});
        RedfishByHand many(RedfishByHand::Layout::OpenBmc);
        many.managerSays = u"neither"_s;
        many.images = listed;
        QCOMPARE(firmwareSaid(many), QString());
        QVERIFY(inventoryAsked(many).isEmpty());
        QList<Image> two{{u"aa"_s, u"BMC"_s, u"Enabled"_s, u"1.41.0"_s}};
        for (int at = 0; at < qkvm::core::redfish::mostImages - 1; ++at) {
            two.append({u"h%1"_s.arg(at), u"Host"_s, u"Enabled"_s, u"9.%1"_s.arg(at)});
        }
        two.append({u"bb"_s, u"BMC"_s, u"Enabled"_s, u"1.42.0"_s});
        RedfishByHand both(RedfishByHand::Layout::OpenBmc);
        both.managerSays = u"neither"_s;
        both.images = two;
        QCOMPARE(firmwareSaid(both), QString());
        RedfishByHand one(RedfishByHand::Layout::OpenBmc);
        one.managerSays = u"neither"_s;
        one.images = two.mid(0, two.size() - 1);
        QCOMPARE(firmwareSaid(one), u"1.41.0"_s); // (all of them read: that one)
    }

    void v5NoReadOfItsInventoryBeginsAfterItsTime()
    {
        RedfishByHand bmc(RedfishByHand::Layout::OpenBmc);
        bmc.managerSays = u"neither"_s;
        RedfishDevice::Pace pace = quick();
        pace.imagesWithinMs = 0;
        QCOMPARE(firmwareSaid(bmc, pace), QString());
        QVERIFY(!bmc.requests.contains(u"GET /redfish/v1/UpdateService/FirmwareInventory"_s));
    }

    void v6WhatItsInventorySaysUnderASessionSignedInAnewIsNotTaken()
    {
        // As r3: a sign-in made anew in the middle of a look, for another question, goes to another server of one make,
        // its controller's number the same. What its inventory says under that session is not taken for this one's.
        RedfishByHand bmc(RedfishByHand::Layout::OpenBmc);
        bmc.beServer(aSystem, aRoot);
        bmc.managerSays = u"neither"_s;
        bmc.managerFails = 1; // (its version is asked again in the next look)
        RedfishDevice::Pace pace = quick();
        pace.aboutRetryMs = 200;
        RedfishDevice device(keptAs(bmc, markOf(aSystem) + digestOf(aRoot)), u"secret"_s, pace);
        QCOMPARE(lookedAt(device).version, QString());
        QTest::qWait(300);
        QList<base::Result<RedfishDevice::Identity>> identity;
        bool once = false;
        bmc.inventoryTakesMs = 400; // (the sign-in anew is made while its UpdateService is answered)
        bmc.whenAsked = [&](const Asked& asked) {
            if (once || asked.path != "/redfish/v1/UpdateService") {
                return;
            }
            once = true;
            QTimer::singleShot(0, &bmc, [&] {
                bmc.beServer(bSystem, aRoot);
                bmc.firmware = u"9.99"_s;
                bmc.systemTakesMs = 1200;
                device.identify(
                    [&identity](base::Result<RedfishDevice::Identity> said) { identity.append(std::move(said)); });
            });
        };
        QCOMPARE(lookedAt(device).version, QString());
        QVERIFY(identity.isEmpty()); // (the other session not judged yet: else this proves nothing)
        QVERIFY(bmc.requests.contains(u"GET /redfish/v1/UpdateService/FirmwareInventory"_s));
        QTRY_COMPARE_WITH_TIMEOUT(identity.size(), 1, 5000);
        bmc.whenAsked = nullptr;
    }

    void r1AResourceTheAccountMayNotReadIsRefusedAndNotTheSession()
    {
        // An older board keeps its sensors from an operator: 401 under the session it honoured for the read before.
        // One sign-in for many looks, the card online, the sensors absent; that place is not asked again until
        // where it keeps things is found anew.
        RedfishByHand bmc;
        bmc.unauthorizedAt = u"/redfish/v1/Chassis/Self/Thermal"_s;
        int thermal = 0;
        bmc.whenAsked = [&thermal](const Asked& asked) {
            thermal += asked.path == "/redfish/v1/Chassis/Self/Thermal" ? 1 : 0;
        };
        RedfishDevice::Pace pace = quick();
        pace.placesFreshMs = 1500;
        RedfishDevice device(at(bmc), u"secret"_s, pace);
        for (int look = 0; look < 4; ++look) {
            const qkvm::core::Status status = lookedAt(device);
            QCOMPARE(status.state, u"online"_s);
            QVERIFY(!status.temperature);
            QCOMPARE(readingsOf(status).fans, 0);
            QCOMPARE(status.version, u"1.42.0"_s);
        }
        QCOMPARE(bmc.logins, 1);
        QCOMPARE(bmc.signedOut, 0);
        QCOMPARE(thermal, 1);
        // Where it keeps things found anew: asked once more, and refused once more. Still the one session.
        QTest::qWait(pace.placesFreshMs + 100);
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QCOMPARE(thermal, 2);
        QCOMPARE(bmc.logins, 1);
        bmc.whenAsked = nullptr;
    }

    void r3WhatWasReadUnderASessionSignedInAnewInALookIsNotTaken()
    {
        // A sign-in made anew in the middle of a look (for another question: here, what it is) goes to whatever
        // answers at its address now: another server of one make, its controller's number the same. Its sensors and
        // its version, read under that session before it was judged, are not shown as this one's.
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        bmc.managerFails = 1; // (its version is asked again in the next look)
        RedfishDevice::Pace pace = quick();
        pace.aboutRetryMs = 200;
        RedfishDevice device(keptAs(bmc, markOf(aSystem) + digestOf(aRoot)), u"secret"_s, pace);
        const qkvm::core::Status before = lookedAt(device);
        QCOMPARE(before.state, u"online"_s);
        QCOMPARE(before.temperature, std::optional<double>(61));
        QCOMPARE(before.version, QString());
        QTest::qWait(300);
        QList<base::Result<RedfishDevice::Identity>> identity;
        bool once = false;
        bmc.systemTakesMs = 400;
        bmc.whenAsked = [&](const Asked& asked) {
            if (once || asked.path != bmc.system().toLatin1()) {
                return;
            }
            once = true;
            QTimer::singleShot(0, &bmc, [&] {
                bmc.beServer(bSystem, aRoot);
                bmc.cpuTemperature = 99;
                bmc.firmware = u"9.99"_s;
                bmc.systemTakesMs = 1200;
                device.identify(
                    [&identity](base::Result<RedfishDevice::Identity> said) { identity.append(std::move(said)); });
            });
        };
        const qkvm::core::Status during = lookedAt(device);
        QCOMPARE(during.state, u"online"_s); // (its computer, read and judged under its own session)
        QVERIFY(!during.temperature);
        QCOMPARE(readingsOf(during).fans, 0);
        QCOMPARE(during.version, QString());
        QVERIFY(identity.isEmpty()); // (the other session not judged yet: else this proves nothing)
        QCOMPARE(bmc.logins, 2);
        QTRY_COMPARE_WITH_TIMEOUT(identity.size(), 1, 5000);
        QCOMPARE(codeOf(identity.first()), std::string("another_machine"));
        QCOMPARE(lookedAt(device).errorCode, u"another_machine"_s);
        bmc.whenAsked = nullptr;
    }

    void r4WhatWasReadUnderASessionSignedInAnewInALookIsNotTakenTheNewerShape()
    {
        // The same, in the newer shape: its temperatures, and the list of its fans (none of which is asked).
        RedfishByHand bmc(RedfishByHand::Layout::OpenBmc);
        bmc.beServer(aSystem, aRoot);
        RedfishDevice device(keptAs(bmc, markOf(aSystem) + digestOf(aRoot)), u"secret"_s, quick());
        const qkvm::core::Status before = lookedAt(device);
        QCOMPARE(before.temperature, std::optional<double>(61));
        QCOMPARE(readingsOf(before).fans, 2);
        QList<base::Result<RedfishDevice::Identity>> identity;
        bool once = false;
        bmc.systemTakesMs = 400;
        bmc.whenAsked = [&](const Asked& asked) {
            if (once || asked.path != bmc.system().toLatin1()) {
                return;
            }
            once = true;
            QTimer::singleShot(0, &bmc, [&] {
                bmc.beServer(bSystem, aRoot);
                bmc.cpuTemperature = 99;
                bmc.moreFans = 2;
                bmc.systemTakesMs = 1200;
                device.identify(
                    [&identity](base::Result<RedfishDevice::Identity> said) { identity.append(std::move(said)); });
            });
        };
        const qkvm::core::Status during = lookedAt(device);
        QCOMPARE(during.state, u"online"_s);
        QVERIFY(!during.temperature);
        QCOMPARE(readingsOf(during).fans, readingsOf(before).fans); // (as the last look found them)
        QCOMPARE(bmc.asked(u"GET /redfish/v1/Chassis/chassis/ThermalSubsystem/Fans/fan_EXTRA_0"_s), 0);
        QVERIFY(identity.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(identity.size(), 1, 5000);
        bmc.whenAsked = nullptr;
    }

    void r5AFanReadUnderASessionSignedInAnewInALookIsNotTaken()
    {
        // The same, with the fans read in turn: those read under the other session keep what was last read.
        RedfishByHand bmc(RedfishByHand::Layout::OpenBmc);
        bmc.beServer(aSystem, aRoot);
        bmc.moreFans = 1;
        RedfishDevice device(keptAs(bmc, markOf(aSystem) + digestOf(aRoot)), u"secret"_s, quick());
        const qkvm::core::Status before = lookedAt(device);
        QCOMPARE(readingsOf(before).fans, 3);
        QCOMPARE(readingsOf(before).fansNotWell, 1);
        QList<base::Result<RedfishDevice::Identity>> identity;
        bool once = false;
        bmc.fanTakesMs = 200;
        bmc.whenAsked = [&](const Asked& asked) {
            if (once || !asked.path.contains("/Fans/fan_")) {
                return;
            }
            once = true;
            QTimer::singleShot(0, &bmc, [&] {
                bmc.beServer(bSystem, aRoot);
                bmc.lastFanUnwell = true;
                bmc.systemTakesMs = 1200;
                device.identify(
                    [&identity](base::Result<RedfishDevice::Identity> said) { identity.append(std::move(said)); });
            });
        };
        const qkvm::core::Status during = lookedAt(device);
        QCOMPARE(during.state, u"online"_s);
        QCOMPARE(readingsOf(during).fans, 3);
        QCOMPARE(readingsOf(during).fansNotWell, 1);
        QCOMPARE(bmc.asked(u"GET /redfish/v1/Chassis/chassis/ThermalSubsystem/Fans/fan_EXTRA_0"_s), 2);
        QVERIFY(identity.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(identity.size(), 1, 5000);
        bmc.whenAsked = nullptr;
    }

    void r6AnotherServerFoundInTheMiddleOfALookEndsIt()
    {
        // Another controller found at its address while a look is out (a sign-in made anew for another question):
        // the look says so, and not what was read of this one a moment before.
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        RedfishDevice device(keptAs(bmc, markOf(aSystem) + digestOf(aRoot)), u"secret"_s, quick());
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QList<base::Result<RedfishDevice::Identity>> identity;
        bool once = false;
        bmc.systemTakesMs = 400;
        bmc.whenAsked = [&](const Asked& asked) {
            if (once || asked.path != bmc.system().toLatin1()) {
                return;
            }
            once = true;
            QTimer::singleShot(0, &bmc, [&] {
                bmc.beServer(bSystem, bRoot);
                device.identify(
                    [&identity](base::Result<RedfishDevice::Identity> said) { identity.append(std::move(said)); });
            });
        };
        const qkvm::core::Status during = lookedAt(device);
        QCOMPARE(identity.size(), 1);
        QCOMPARE(codeOf(identity.first()), std::string("another_machine"));
        QCOMPARE(during.state, u"error"_s);
        QCOMPARE(during.errorCode, u"another_machine"_s);
        QVERIFY(!during.server);
        QCOMPARE(bmc.logins, 1);
        bmc.whenAsked = nullptr;
    }

    void r7ALookThatFailsWithNothingGoodStandingIsFollowedInItsTime()
    {
        // (redfish.py:2047, 2058-2073) Sooner (RETRY_LOOK) only where a failed look left the last good one standing
        // (l5); after another server, a refusal, or an error with nothing good standing, in its time (LOOK_EVERY).
        RedfishDevice::Pace pace = quick();
        pace.freshMs = 1500;
        pace.retryLookMs = 100;
        {
            RedfishByHand bmc;
            bmc.password = u"changed on the controller"_s;
            RedfishDevice device(at(bmc), u"secret"_s, pace);
            QCOMPARE(lookedAt(device).state, u"auth"_s);
            QVERIFY(followedInItsTime(device));
        }
        {
            RedfishByHand bmc;
            bmc.systemUnwell = true;
            RedfishDevice device(at(bmc), u"secret"_s, pace);
            const qkvm::core::Status status = lookedAt(device);
            QCOMPARE(status.state, u"error"_s);
            QCOMPARE(status.errorCode, u"bad_response"_s);
            QVERIFY(followedInItsTime(device));
        }
        {
            RedfishByHand bmc;
            bmc.beServer(bSystem, bRoot);
            RedfishDevice device(keptAs(bmc, markOf(aSystem) + digestOf(aRoot)), u"secret"_s, pace);
            QCOMPARE(lookedAt(device).errorCode, u"another_machine"_s);
            QVERIFY(followedInItsTime(device));
        }
    }

    void r8NotBeingWhoItWasIsNoPassingMoment()
    {
        // (redfish.py:2066) Another certificate at its address, or one Windows does not trust, is said at once, and
        // not as one of its moments, which would show the last good look for a while. (The third, a TLS that is
        // not Windows' own, is the program's for good, from before anything is asked: no good look ever comes
        // before it. test_tls_other_first.)
        RedfishDevice::Pace pace = quick();
        pace.connectWithinMs = 5000; // (a TLS handshake under the address sanitizer on a busy PC)
        pace.hiccupMs = 60'000;
        {
            auto bmc = std::make_unique<RedfishByHand>(RedfishByHand::Layout::Ami, tls("first"));
            RedfishDevice::Where where = at(*bmc);
            where.knownByCertificate = true;
            where.certificate = sha256Of("first");
            RedfishDevice device(where, u"secret"_s, pace);
            QCOMPARE(lookedAt(device).state, u"online"_s);
            const quint16 port = bmc->stand->serverPort();
            bmc.reset();
            RedfishByHand other(RedfishByHand::Layout::Ami, tls("second"), port);
            QVERIFY(other.stand->isListening());
            const qkvm::core::Status status = lookedAt(device);
            QCOMPARE(status.state, u"error"_s);
            QCOMPARE(status.errorCode, u"cert_mismatch"_s);
            QVERIFY(other.stand->asked.isEmpty());
        }
        {
            const VouchedFor vouched(QStringLiteral(QKVM_TESTDATA "/tls/makers.crt"));
            auto bmc = std::make_unique<RedfishByHand>(RedfishByHand::Layout::Ami, tls("signed"));
            RedfishDevice device(at(*bmc), u"secret"_s, pace);
            QCOMPARE(lookedAt(device).state, u"online"_s);
            const quint16 port = bmc->stand->serverPort();
            bmc.reset();
            RedfishByHand other(RedfishByHand::Layout::Ami, tls("first"), port);
            QVERIFY(other.stand->isListening());
            const qkvm::core::Status status = lookedAt(device);
            QCOMPARE(status.state, u"error"_s);
            QCOMPARE(status.errorCode, u"cert_invalid"_s);
            QVERIFY(other.stand->asked.isEmpty());
        }
    }

    void r9ItsVersionIsKeptAndAskedAgainSoonerWhereItCouldNotBeRead()
    {
        // (redfish.py ABOUT_TTL, ABOUT_RETRY) Asked with where it keeps things; where it cannot be read, what was known
        // stands meanwhile, and it is asked again sooner than in its time.
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        RedfishDevice::Pace pace = quick();
        pace.placesFreshMs = 2000;
        pace.aboutRetryMs = 600;
        // (Both its numbers kept: its controller's manager is read for its version alone.)
        RedfishDevice device(keptAs(bmc, markOf(aSystem) + digestOf(aRoot)), u"secret"_s, pace);
        const QString manager = u"GET "_s + bmc.manager();
        QCOMPARE(lookedAt(device).version, u"1.42.0"_s);
        QCOMPARE(bmc.asked(manager), 1);
        QCOMPARE(lookedAt(device).version, u"1.42.0"_s);
        QCOMPARE(bmc.asked(manager), 1);
        QTest::qWait(pace.placesFreshMs + 100);
        bmc.managerFails = 1;
        bmc.firmware = u"1.43.0"_s;
        QCOMPARE(lookedAt(device).version, u"1.42.0"_s);
        QCOMPARE(bmc.asked(manager), 2);
        QCOMPARE(lookedAt(device).version, u"1.42.0"_s);
        QCOMPARE(bmc.asked(manager), 2);
        QTest::qWait(pace.aboutRetryMs + 100);
        QCOMPARE(lookedAt(device).version, u"1.43.0"_s);
        QCOMPARE(bmc.asked(manager), 3);
    }
};

QTEST_GUILESS_MAIN(TestRedfishLook)
#include "test_redfish_look.moc"
