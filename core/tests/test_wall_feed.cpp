#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSignalSpy>
#include <QTest>

#include <qkvm/core/server_client.h>
#include <qkvm/core/wall_feed.h>

#include "stand_in.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::testing::Answered;
using core::testing::Asked;
using core::testing::StandIn;

// The feed that keeps a wall up to date, against a stand-in server.
class TestWallFeed : public QObject {
    Q_OBJECT

private:
    // A wall of so many KVMs, "m1" and so on, at four pictures a second; and how they are.
    static QByteArray wallOf(int machines)
    {
        QJsonArray devices;
        for (int number = 1; number <= machines; ++number) {
            devices.append(QJsonObject{{u"id"_s, u"m%1"_s.arg(number)},
                                       {u"name"_s, u"Machine %1"_s.arg(number)},
                                       {u"host"_s, u"192.168.1.%1"_s.arg(20 + number)},
                                       {u"kind"_s, u"kvmd"_s},
                                       {u"picture"_s, true}});
        }
        return QJsonDocument(QJsonObject{{u"interval"_s, 0.25}, {u"devices"_s, devices}}).toJson();
    }

    static QByteArray statusOf(int machines)
    {
        QJsonObject how;
        for (int number = 1; number <= machines; ++number) {
            how.insert(u"m%1"_s.arg(number), QJsonObject{{u"state"_s, u"online"_s}});
        }
        return QJsonDocument(how).toJson();
    }

    static QHash<QString, int> allOf(int machines)
    {
        QHash<QString, int> shown;
        for (int number = 1; number <= machines; ++number) {
            shown.insert(u"m%1"_s.arg(number), 320);
        }
        return shown;
    }

    // Which machine a picture is asked of.
    static QString machineOf(const Asked& asked) { return QString::fromLatin1(asked.path.split('/').value(4)); }

    // A wall of a KVM, a second KVM and a server, at four pictures a second.
    static constexpr const char* wall = R"({"interval": 0.25, "devices": [
        {"id": "a", "name": "Build server", "host": "192.168.1.21", "kind": "kvmd", "picture": true},
        {"id": "b", "name": "NAS", "host": "192.168.1.22", "kind": "kvmd", "picture": true},
        {"id": "s", "name": "Database server", "host": "192.168.1.71", "kind": "redfish", "picture": false}]})";
    static constexpr const char* status =
        R"({"a": {"state": "online"}, "b": {"state": "no_signal"}, "s": {"state": "online", "picture": false}})";

    static Answered ordinary(const Asked& asked)
    {
        if (asked.path == "/api/v1/wall") {
            return {200, wall};
        }
        if (asked.path == "/api/v1/status") {
            return {200, status};
        }
        return {200, core::testing::aPicture(), "image/jpeg"};
    }

private slots:
    void theWallAndHowItIsAreAskedAtOnce()
    {
        StandIn server(ordinary);
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        core::WallFeed feed(client, 100);
        QSignalSpy wallChanged(&feed, &core::WallFeed::wallChanged);
        QSignalSpy statusChanged(&feed, &core::WallFeed::statusChanged);
        feed.start();
        QVERIFY(feed.isRunning());
        QTRY_VERIFY(wallChanged.count() >= 1 && statusChanged.count() >= 1);
        QCOMPARE(feed.machines().size(), 3);
        QCOMPARE(feed.machines()[1].name, u"NAS"_s);
        QCOMPARE(feed.status().value(u"b"_s).state, u"no_signal"_s);
        QVERIFY(feed.isInContact());
        // And how it is, again and again.
        QTRY_VERIFY(statusChanged.count() >= 3);
    }

    void afterSomethingWasDoneTheMachinesAreLookedAtSoonerThanAtTheUsualPace()
    {
        StandIn server(ordinary);
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        // (At its usual pace this feed looks once a minute.)
        core::WallFeed feed(client, 60'000);
        const QSignalSpy looked(&feed, &core::WallFeed::statusChanged);
        feed.start();
        QTRY_COMPARE(looked.count(), 1);
        QElapsedTimer since;
        since.start();
        feed.lookAgainSoon();
        // Something else was done meanwhile: that is still one round of looks, not two.
        feed.lookAgainSoon();
        // About a second after, and not at once: what was done takes the machine a moment.
        QTRY_COMPARE_WITH_TIMEOUT(looked.count(), 2, 3000);
        QVERIFY2(since.elapsed() >= 800, qPrintable(QString::number(since.elapsed())));
        QTest::qWait(400);
        QCOMPARE(looked.count(), 2);
        // And about three seconds after.
        QTRY_COMPARE_WITH_TIMEOUT(looked.count(), 3, 5000);
        QVERIFY2(since.elapsed() >= 2900, qPrintable(QString::number(since.elapsed())));
    }

    void aFeedThatIsStoppedLooksNoMore()
    {
        StandIn server(ordinary);
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        core::WallFeed feed(client, 60'000);
        const QSignalSpy looked(&feed, &core::WallFeed::statusChanged);
        feed.start();
        QTRY_COMPARE(looked.count(), 1);
        feed.lookAgainSoon();
        feed.stop();
        // Nor is a round begun on one that is stopped.
        feed.lookAgainSoon();
        QTest::qWait(1400);
        QCOMPARE(looked.count(), 1);
        QCOMPARE(server.count("/api/v1/status"), 1);
    }

    void aPlaceThatSaysAMachineHasComeToSomethingIsAskedAtOnceAndOnceForAllItSaidMeanwhile()
    {
        // A place that takes a while over how its machines are.
        StandIn server([](const Asked& asked) {
            Answered answer = ordinary(asked);
            if (asked.path == "/api/v1/status") {
                answer.afterMs = 400;
            }
            return answer;
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        // (At its usual pace this feed looks once a minute.)
        core::WallFeed feed(client, 60'000);
        const QSignalSpy looked(&feed, &core::WallFeed::statusChanged);
        const QSignalSpy wallChanged(&feed, &core::WallFeed::wallChanged);
        // Stopped, it asks nothing whatever it is told, and notes nothing for later.
        feed.lookNow();
        QTest::qWait(200);
        QVERIFY(server.asked.isEmpty());
        feed.start();
        QTRY_COMPARE(looked.count(), 1);
        QCOMPARE(server.count("/api/v1/status"), 1);
        QCOMPARE(server.count("/api/v1/wall"), 1);

        // The place says that a machine has come to something: how they are is asked there and
        // then, a minute before the feed's own turn. The list is not: it is the one it was.
        feed.lookNow();
        QTRY_COMPARE_WITH_TIMEOUT(server.count("/api/v1/status"), 2, 1000);
        // It says so three times more while that is being asked. The answer on its way may
        // have been made before any of them: so it is asked once more when that has come, and
        // that is one asking for the three, not three.
        feed.lookNow();
        feed.lookNow();
        feed.lookNow();
        QCOMPARE(server.count("/api/v1/status"), 2);
        QTRY_COMPARE(looked.count(), 2);
        QTRY_COMPARE_WITH_TIMEOUT(server.count("/api/v1/status"), 3, 1000);
        QTRY_COMPARE(looked.count(), 3);
        // And then no more, until something is said again.
        QTest::qWait(700);
        QCOMPARE(server.count("/api/v1/status"), 3);
        QCOMPARE(looked.count(), 3);
        QCOMPARE(server.count("/api/v1/wall"), 1);
        QCOMPARE(wallChanged.count(), 1);
        // What was said to a feed that was then stopped is asked for by nobody afterwards: a
        // new start asks everything, once.
        feed.lookNow();
        feed.lookNow();
        feed.stop();
        feed.start();
        QTRY_COMPARE(looked.count(), 4);
        QTest::qWait(700);
        QCOMPARE(looked.count(), 4);
    }

    void aPictureIsAskedOnlyForAMachineThatIsLookedAt()
    {
        StandIn server(ordinary);
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        core::WallFeed feed(client, 100);
        QSignalSpy came(&feed, &core::WallFeed::pictureCame);
        // The first KVM is on the screen, 480 pixels wide; and so is the server, which has no picture.
        feed.setShown({{u"a"_s, 480}, {u"s"_s, 480}});
        feed.start();
        QTRY_VERIFY(came.count() >= 3);
        QCOMPARE(came[0][0].toString(), u"a"_s);
        QCOMPARE(came[0][1].toByteArray(), core::testing::aPicture());
        QVERIFY(server.count("/api/v1/devices/a/snapshot?w=480") >= 3);
        QCOMPARE(server.count("/api/v1/devices/b/"), 0);
        QCOMPARE(server.count("/api/v1/devices/s/"), 0);
    }

    void aServerWhoseScreenCanBeReadIsAskedForPicturesAsAKvmIs()
    {
        StandIn server([](const Asked& asked) {
            if (asked.path == "/api/v1/status") {
                return Answered{200, R"({"a": {"state": "online"}, "b": {"state": "no_signal"},
                                        "s": {"state": "online", "picture": false, "screen": true}})"};
            }
            return ordinary(asked);
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        core::WallFeed feed(client, 100);
        QSignalSpy came(&feed, &core::WallFeed::pictureCame);
        feed.setShown({{u"s"_s, 480}});
        feed.start();
        QTRY_VERIFY(came.count() >= 2);
        QCOMPARE(came[0][0].toString(), u"s"_s);
        QVERIFY(server.count("/api/v1/devices/s/snapshot?w=480") >= 2);
    }

    // b5: a PC with Intel AMT in health only is asked for no picture, whatever its status says of
    // its screen (by the status's view, the fresher, as by the wall's); one in stills is, and its
    // still says how old it is.
    void aPcInHealthOnlyIsAskedForNoPicture()
    {
        StandIn server([](const Asked& asked) {
            if (asked.path == "/api/v1/wall") {
                return Answered{200, R"({"interval": 0.25, "devices": [
                    {"id": "h", "name": "Front desk", "kind": "amt", "picture": false, "amt_view": "health"},
                    {"id": "v", "name": "Lab PC", "kind": "amt", "picture": false, "amt_view": "stills"},
                    {"id": "t", "name": "Till", "kind": "amt", "picture": false, "amt_view": "stills"}]})"};
            }
            if (asked.path == "/api/v1/status") {
                return Answered{200, R"({"h": {"state": "online", "picture": false, "screen": true, "server": {}},
                    "v": {"state": "online", "picture": false, "screen": true, "server": {"amt": {"view": "health"}}},
                    "t": {"state": "online", "picture": false, "screen": true, "server": {"amt": {"view": "stills"}}}})"};
            }
            return Answered{200, core::testing::aPicture(), "image/jpeg", {"X-KVM-Still: 1", "X-KVM-Age: 90"}};
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        core::WallFeed feed(client, 100);
        QSignalSpy came(&feed, &core::WallFeed::pictureCame);
        feed.setShown({{u"h"_s, 480}, {u"v"_s, 480}, {u"t"_s, 480}});
        feed.start();
        QTRY_VERIFY(came.count() >= 3);
        QTest::qWait(300);
        QCOMPARE(server.count("/api/v1/devices/h/"), 0);
        QCOMPARE(server.count("/api/v1/devices/v/"), 0);
        QVERIFY(server.count("/api/v1/devices/t/snapshot") >= 3);
        QVERIFY(feed.isStill(u"t"_s));
        QVERIFY(!feed.isStill(u"h"_s));
        const std::optional<int> age = feed.stillAgeOf(u"t"_s);
        QVERIFY(age.has_value());
        QVERIFY2(*age >= 90 && *age <= 92, qPrintable(QString::number(age.value_or(-1))));
    }

    void picturesAreAskedForAsOftenAsWasChosenHere()
    {
        // The server names four a second.
        StandIn server(ordinary);
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        core::WallFeed feed(client, 100);
        const auto pictures = [&] { return server.count("/api/v1/devices/a/snapshot"); };
        QCOMPARE(feed.serverPaceMs(), 1000);
        feed.setShown({{u"a"_s, 480}});
        // One a second was chosen here, before the wall was come to.
        feed.setPictureEvery(1000);
        feed.start();
        QTRY_COMPARE(pictures(), 1);
        QCOMPARE(feed.serverPaceMs(), 250);
        QTest::qWait(1600);
        // (At the server's pace it would have been six or seven.)
        QVERIFY2(pictures() >= 2 && pictures() <= 3, qPrintable(QString::number(pictures())));

        // As the server names, again.
        feed.setPictureEvery(std::nullopt);
        int before = pictures();
        QTest::qWait(1100);
        QVERIFY2(pictures() - before >= 3, qPrintable(QString::number(pictures() - before)));

        // Paused: none are asked for, and how the machines are is asked as ever.
        feed.setPictureEvery(0);
        QTest::qWait(300);
        before = pictures();
        const int looks = server.count("/api/v1/status");
        QTest::qWait(900);
        QCOMPARE(pictures(), before);
        QVERIFY(server.count("/api/v1/status") >= looks + 3);
        // A machine that comes into sight meanwhile has no picture to keep: it is given one, so
        // that its card does not show nothing, and no more than that one.
        feed.setShown({{u"a"_s, 480}, {u"b"_s, 320}});
        QTRY_COMPARE(server.count("/api/v1/devices/b/snapshot"), 1);
        QTest::qWait(700);
        QCOMPARE(server.count("/api/v1/devices/b/snapshot"), 1);
        QCOMPARE(pictures(), before);
        feed.setShown({{u"a"_s, 480}});
        // A machine that is looked at closely gets its pictures all the same, and only while it is.
        feed.setLookedAtClosely({u"a"_s});
        QTRY_VERIFY(pictures() >= before + 2);
        feed.setLookedAtClosely({});
        QTest::qWait(300);
        before = pictures();
        QTest::qWait(700);
        QCOMPARE(pictures(), before);

        // The pause ends: a picture is asked for at once, and then at the pace that was chosen.
        feed.setPictureEvery(5000);
        QTRY_COMPARE_WITH_TIMEOUT(pictures(), before + 1, 1000);
        QTest::qWait(800);
        QCOMPARE(pictures(), before + 1);
    }

    void aWallThatIsComeToPausedShowsEachMachineOnce()
    {
        // The first KVM's picture cannot be had at first.
        bool refuses = true;
        StandIn server([&](const Asked& asked) {
            if (refuses && asked.path.startsWith("/api/v1/devices/a/snapshot")) {
                return Answered{502, R"({"code": "no_stream", "message": "No picture just now"})"};
            }
            return ordinary(asked);
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        core::WallFeed feed(client, 100);
        feed.setShown({{u"a"_s, 480}, {u"b"_s, 480}});
        feed.setPictureEvery(0);
        feed.start();
        // The second KVM is shown as it is now, once.
        QTRY_COMPARE(server.count("/api/v1/devices/b/snapshot"), 1);
        QTest::qWait(800);
        QCOMPARE(server.count("/api/v1/devices/b/snapshot"), 1);
        // The first has no picture yet, and is asked for one until one comes.
        QVERIFY(server.count("/api/v1/devices/a/snapshot") >= 3);
        refuses = false;
        QTest::qWait(600);
        const int asked = server.count("/api/v1/devices/a/snapshot");
        QTest::qWait(700);
        QCOMPARE(server.count("/api/v1/devices/a/snapshot"), asked);

        // Started anew (signed in again, say), still paused: each is shown once again.
        feed.stop();
        feed.start();
        QTRY_COMPARE(server.count("/api/v1/devices/b/snapshot"), 2);
        QTest::qWait(700);
        QCOMPARE(server.count("/api/v1/devices/b/snapshot"), 2);
        QCOMPARE(server.count("/api/v1/devices/a/snapshot"), asked + 1);
    }

    void noMoreThanAFewPicturesAreOutAtOnce()
    {
        // Twelve machines on the screen, and a server that takes its time over each picture.
        QJsonArray devices;
        QJsonObject how;
        QHash<QString, int> shown;
        for (int number = 1; number <= 12; ++number) {
            const QString id = u"m%1"_s.arg(number);
            devices.append(QJsonObject{{u"id"_s, id},
                                       {u"name"_s, u"Machine %1"_s.arg(number)},
                                       {u"host"_s, u"192.168.1.%1"_s.arg(20 + number)},
                                       {u"kind"_s, u"kvmd"_s},
                                       {u"picture"_s, true}});
            how.insert(id, QJsonObject{{u"state"_s, u"online"_s}});
            shown.insert(id, 320);
        }
        const QByteArray wallSaid = QJsonDocument(QJsonObject{{u"interval"_s, 0.25}, {u"devices"_s, devices}}).toJson();
        const QByteArray statusSaid = QJsonDocument(how).toJson();
        QElapsedTimer clock;
        clock.start();
        QList<qint64> arrived;
        QStringList whose;
        StandIn server([&](const Asked& asked) {
            if (asked.path == "/api/v1/wall") {
                return Answered{200, wallSaid};
            }
            if (asked.path == "/api/v1/status") {
                return Answered{200, statusSaid};
            }
            // (What is done to a machine is done at once.)
            if (asked.method == "POST") {
                return Answered{200, R"({"ok": true, "note": null})"};
            }
            arrived.append(clock.elapsed());
            whose.append(QString::fromLatin1(asked.path.split('/').value(4)));
            Answered picture{200, core::testing::aPicture(), "image/jpeg"};
            picture.afterMs = 300;
            return picture;
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        core::WallFeed feed(client, 100);
        feed.setShown(shown);
        feed.start();
        // Two rounds of them.
        QTRY_VERIFY_WITH_TIMEOUT(arrived.size() >= 24, 10000);
        // Each takes 300 ms. Had five been on their way at once, the fifth would have gone out
        // before the first was answered.
        for (qsizetype at = 4; at < arrived.size(); ++at) {
            QVERIFY2(arrived[at] - arrived[at - 4] >= 250,
                     qPrintable(u"the question after %1 went out %2 ms after the fourth before it"_s.arg(at).arg(
                         arrived[at] - arrived[at - 4])));
        }
        // Every machine has its turn before any has a second one.
        const QStringList firstRound = whose.mid(0, 12);
        QCOMPARE(QSet<QString>(firstRound.begin(), firstRound.end()).size(), 12);
        // And how the machines are was asked as ever meanwhile: two of the six questions a program
        // has out to one server stay free for it, and for what somebody does.
        QVERIFY2(server.count("/api/v1/status") >= 8, qPrintable(QString::number(server.count("/api/v1/status"))));

        // What somebody does to a machine while the pictures flow does not wait behind them: it is
        // answered in less time than one picture takes. (The quickest of three: the PC may be busy.)
        qint64 quickest = 10'000;
        for (int time = 0; time < 3; ++time) {
            QElapsedTimer waited;
            waited.start();
            bool done = false;
            client.act(u"m1"_s, core::Action::plain(u"jiggler_on"_s),
                       [&done](const base::Result<core::Done>& took) { done = took.ok(); });
            QTRY_VERIFY(done);
            quickest = std::min(quickest, waited.elapsed());
            QTest::qWait(130);
        }
        QVERIFY2(quickest < 200, qPrintable(u"%1 ms"_s.arg(quickest)));
    }

    void whoeverWaitsForAFreeQuestionGoesAsSoonAsOneIs()
    {
        // Twelve machines, a picture of each every five seconds, and each picture takes 300 ms.
        QElapsedTimer clock;
        QList<qint64> arrived;
        StandIn server([&](const Asked& asked) {
            if (asked.path == "/api/v1/wall") {
                return Answered{200, wallOf(12)};
            }
            if (asked.path == "/api/v1/status") {
                return Answered{200, statusOf(12)};
            }
            if (arrived.isEmpty()) {
                clock.start();
            }
            arrived.append(clock.elapsed());
            Answered picture{200, core::testing::aPicture(), "image/jpeg"};
            picture.afterMs = 300;
            return picture;
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        core::WallFeed feed(client, 100);
        feed.setPictureEvery(5000);
        feed.setShown(allOf(12));
        feed.start();
        QTRY_COMPARE_WITH_TIMEOUT(arrived.size(), 12, 6000);
        // Four, and four more as soon as those are answered, and the last four after that: under a
        // second. Not at the feed's next look and the one after, a second apart.
        QVERIFY2(arrived.constLast() < 1500,
                 qPrintable(u"the twelfth went out after %1 ms"_s.arg(arrived.constLast())));
    }

    void aMachineThatWaitsForItsTurnWaitsOnce()
    {
        // Twelve machines whose pictures take 400 ms each at first: more than the wall's pace
        // lets through four at a time, so machines wait. Then the server is quick.
        bool slow = true;
        QElapsedTimer clock;
        clock.start();
        QList<qint64> arrived;
        StandIn server([&](const Asked& asked) {
            if (asked.path == "/api/v1/wall") {
                return Answered{200, wallOf(12)};
            }
            if (asked.path == "/api/v1/status") {
                return Answered{200, statusOf(12)};
            }
            arrived.append(clock.elapsed());
            Answered picture{200, core::testing::aPicture(), "image/jpeg"};
            picture.afterMs = slow ? 400 : 0;
            return picture;
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        core::WallFeed feed(client, 100);
        feed.setShown(allOf(12));
        feed.start();
        QTest::qWait(2500);
        slow = false;
        // What was on its way comes in; and from then on each machine is asked at the wall's pace,
        // four times a second, and not once for every look it had waited through.
        QTest::qWait(600);
        const qint64 from = clock.elapsed();
        QTest::qWait(1000);
        const qsizetype within = std::ranges::count_if(arrived, [&](qint64 at) { return at >= from; });
        QVERIFY2(within >= 30 && within <= 60, qPrintable(u"%1 pictures were asked for in a second"_s.arg(within)));
    }

    void aMachineThatWentOutOfSightWhileItWaitedIsNotAskedAfterAll()
    {
        QStringList whose;
        StandIn server([&](const Asked& asked) {
            if (asked.path == "/api/v1/wall") {
                return Answered{200, wallOf(12)};
            }
            if (asked.path == "/api/v1/status") {
                return Answered{200, statusOf(12)};
            }
            whose.append(machineOf(asked));
            Answered picture{200, core::testing::aPicture(), "image/jpeg"};
            picture.afterMs = 300;
            return picture;
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        core::WallFeed feed(client, 100);
        feed.setShown(allOf(12));
        feed.start();
        // The first four are on their way, the others wait for their turn. The wall is scrolled:
        // only the first machine's card is left in sight.
        QTRY_COMPARE(whose.size(), 4);
        const QStringList first = whose;
        feed.setShown({{u"m1"_s, 320}});
        QTest::qWait(1500);
        // Nothing was asked of a machine nobody looks at: after those four, the first one alone.
        QVERIFY(whose.size() > 4);
        for (const QString& machine : whose.mid(4)) {
            QCOMPARE(machine, u"m1"_s);
        }
        QCOMPARE(QSet<QString>(first.begin(), first.end()).size(), 4);
    }

    void aCardThatComesIntoSightIsAskedForAtOnce()
    {
        // A picture of each every five seconds: the feed looks whose turn it is once a second.
        StandIn server(ordinary);
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        core::WallFeed feed(client, 100);
        feed.setPictureEvery(5000);
        feed.setShown({{u"a"_s, 480}});
        feed.start();
        QTRY_COMPARE(server.count("/api/v1/devices/a/snapshot"), 1);
        QCOMPARE(server.count("/api/v1/devices/b/snapshot"), 0);
        // The second KVM's card comes into sight a moment later: not at the feed's next look.
        QTest::qWait(150);
        feed.setShown({{u"a"_s, 480}, {u"b"_s, 480}});
        QTRY_COMPARE_WITH_TIMEOUT(server.count("/api/v1/devices/b/snapshot"), 1, 450);
        // And the first is not asked again for that: its turn has not come.
        QTest::qWait(300);
        QCOMPARE(server.count("/api/v1/devices/a/snapshot"), 1);
    }

    void anOrderThatWasSetFromHereIsTheWallsAtOnce()
    {
        StandIn server(ordinary);
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        core::WallFeed feed(client, 100);
        feed.start();
        QTRY_COMPARE(feed.machines().size(), 3);
        const auto ids = [&] {
            QStringList all;
            for (const core::Machine& machine : feed.machines()) {
                all.append(machine.id);
            }
            return all;
        };
        QCOMPARE(ids(), (QStringList{u"a"_s, u"b"_s, u"s"_s}));
        const QSignalSpy changed(&feed, &core::WallFeed::wallChanged);
        // The server's word after it was told an order: the machines are in it, and whoever shows
        // the wall is told; the wall is not asked for a second time.
        feed.takeOrder({u"s"_s, u"a"_s, u"b"_s});
        QCOMPARE(ids(), (QStringList{u"s"_s, u"a"_s, u"b"_s}));
        QCOMPARE(changed.count(), 1);
        QTest::qWait(250);
        QCOMPARE(server.count("/api/v1/wall"), 1);
        // The same order again is no news.
        feed.takeOrder({u"s"_s, u"a"_s, u"b"_s});
        QCOMPARE(changed.count(), 1);
        // A machine the list does not name stays after those it names, and one it names that the
        // wall does not have is nothing.
        feed.takeOrder({u"b"_s, u"gone"_s, u"s"_s});
        QCOMPARE(ids(), (QStringList{u"b"_s, u"s"_s, u"a"_s}));
        QCOMPARE(changed.count(), 2);
        // How the machines are is still each machine's own.
        QCOMPARE(feed.machines().at(0).name, u"NAS"_s);
    }

    void onlyAMachineThatIsLookedAtCloselyIsAskedMoreOften()
    {
        StandIn server(ordinary);
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        core::WallFeed feed(client, 100);
        const auto ofA = [&] { return server.count("/api/v1/devices/a/snapshot"); };
        const auto ofB = [&] { return server.count("/api/v1/devices/b/snapshot"); };
        // A picture every ten seconds. Two machines are shown: one on the wall in one window, the
        // other large in a window beside it.
        feed.setPictureEvery(10000);
        feed.setShown({{u"a"_s, 1920}, {u"b"_s, 480}});
        const QSignalSpy came(&feed, &core::WallFeed::pictureCame);
        feed.start();
        QTRY_COMPARE(ofA(), 1);
        QTRY_COMPARE(ofB(), 1);
        // (Both have come, and not only been asked for: a machine whose picture is still on its
        // way is not asked for another, whatever begins anew.)
        QTRY_COMPARE(came.count(), 2);
        feed.setLookedAtClosely({u"a"_s});
        // Four a second of the one; of the other the one that a new beginning brings, and no more.
        // (Five more in two and a half seconds: one a second, which is how often the feed looks
        // at whose turn it is while no machine is looked at closely, would be three.)
        QTRY_VERIFY_WITH_TIMEOUT(ofA() >= 6, 2500);
        QTRY_COMPARE(ofB(), 2);
        QTest::qWait(700);
        QCOMPARE(ofB(), 2);

        // Paused: the one that is looked at closely goes on, the wall beside it stands still.
        feed.setPictureEvery(0);
        int before = ofA();
        QTRY_VERIFY(ofA() >= before + 2);
        QCOMPARE(ofB(), 2);
        feed.setLookedAtClosely({});
        QTest::qWait(400);
        before = ofA();
        QTest::qWait(600);
        QCOMPARE(ofA(), before);

        // A window more shows the wall: each shown machine is given a picture for it, paused or
        // not, and one only.
        feed.showAnew();
        QTRY_COMPARE(ofA(), before + 1);
        QTRY_COMPARE(ofB(), 3);
        QTest::qWait(700);
        QCOMPARE(ofA(), before + 1);
        QCOMPARE(ofB(), 3);

        // The same where the wall is not paused, and slow: now, and not when its ten seconds
        // are over.
        feed.setPictureEvery(10000);
        QTRY_COMPARE(ofA(), before + 2);
        QTRY_COMPARE(ofB(), 4);
        QTest::qWait(300);
        feed.showAnew();
        QTRY_COMPARE_WITH_TIMEOUT(ofA(), before + 3, 1500);
        QTRY_COMPARE_WITH_TIMEOUT(ofB(), 5, 1500);
    }

    void aNewPaceAndALookFromCloseHoldFromThatMoment()
    {
        StandIn server(ordinary);
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        core::WallFeed feed(client, 100);
        const auto pictures = [&] { return server.count("/api/v1/devices/a/snapshot"); };
        // (Each step waits until the picture that was asked for has come: what is still on its way
        // when something is chosen is the next test's matter.)
        const QSignalSpy came(&feed, &core::WallFeed::pictureCame);
        // A picture every ten seconds.
        feed.setPictureEvery(10000);
        feed.setShown({{u"a"_s, 480}});
        feed.start();
        QTRY_COMPARE(came.count(), 1);
        QTest::qWait(300);
        QCOMPARE(pictures(), 1);
        // One every second is chosen: from now, and not when the ten seconds are over.
        feed.setPictureEvery(1000);
        QTRY_COMPARE_WITH_TIMEOUT(came.count(), 2, 2500);
        QTRY_COMPARE_WITH_TIMEOUT(came.count(), 3, 2500);
        // Ten seconds again: a picture at once, and then none for a while.
        feed.setPictureEvery(10000);
        QTRY_COMPARE_WITH_TIMEOUT(came.count(), 4, 2500);
        QTest::qWait(1300);
        QCOMPARE(pictures(), 4);
        // The machine is looked at from close: pictures at once, four a second.
        feed.setLookedAtClosely({u"a"_s});
        QTRY_VERIFY_WITH_TIMEOUT(came.count() >= 6, 2500);
        feed.setLookedAtClosely({});
        // (That, too, is a new beginning: one more, and then the ten seconds again.)
        QTest::qWait(1600);
        const int settled = pictures();
        QTest::qWait(700);
        QCOMPARE(pictures(), settled);
        QTRY_COMPARE(came.count(), settled);

        // The feed is stopped and started anew (somebody signed out and in): the first look of the
        // new run asks, whatever the turns of the old one were.
        feed.stop();
        feed.start();
        QTRY_COMPARE_WITH_TIMEOUT(pictures(), settled + 1, 2500);
    }

    void aPaceChosenWhileAPictureIsOnItsWayHoldsForThatMachinesNextTurn()
    {
        // A picture every ten seconds, and each takes 400 ms to come.
        StandIn server([](const Asked& asked) {
            Answered answer = ordinary(asked);
            if (asked.path.startsWith("/api/v1/devices/a/snapshot")) {
                answer.afterMs = 400;
            }
            return answer;
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        core::WallFeed feed(client, 100);
        const QSignalSpy came(&feed, &core::WallFeed::pictureCame);
        feed.setPictureEvery(10000);
        feed.setShown({{u"a"_s, 480}});
        feed.start();
        QTRY_COMPARE(server.count("/api/v1/devices/a/snapshot"), 1);
        QCOMPARE(came.count(), 0);
        // While the first is on its way, one every half second is chosen. The machine's next turn
        // is half a second after its question went out, and not ten.
        feed.setPictureEvery(500);
        QTRY_COMPARE_WITH_TIMEOUT(came.count(), 1, 2500);
        QTRY_VERIFY_WITH_TIMEOUT(server.count("/api/v1/devices/a/snapshot") >= 3, 3000);
    }

    void aPictureThatTakesLongDoesNotMakeTheNextOneLater()
    {
        // A picture of the first KVM every second, and each takes 600 ms to come.
        StandIn server([](const Asked& asked) {
            Answered answer = ordinary(asked);
            if (asked.path.startsWith("/api/v1/devices/a/snapshot")) {
                answer.afterMs = 600;
            }
            return answer;
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        core::WallFeed feed(client, 100);
        feed.setPictureEvery(1000);
        feed.setShown({{u"a"_s, 480}});
        feed.start();
        QTRY_COMPARE(server.count("/api/v1/devices/a/snapshot"), 1);
        // Its turns are counted from when each question went out: one a second all the same, and
        // not one every 1.6 seconds, which the feed's looks would make every other second.
        QTest::qWait(4300);
        const int asked = server.count("/api/v1/devices/a/snapshot");
        QVERIFY2(asked >= 5 && asked <= 6, qPrintable(QString::number(asked)));
    }

    void aMachineWhosePictureCannotBeHadIsAskedLessOften()
    {
        bool down = true;
        StandIn server([&](const Asked& asked) {
            if (down && asked.path.startsWith("/api/v1/devices/a/snapshot")) {
                return Answered{502, R"({"code": "unreachable", "message": "Device is unreachable"})"};
            }
            return ordinary(asked);
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        core::WallFeed feed(client, 100);
        const auto asked = [&](const char* machine) {
            return server.count(QByteArray("/api/v1/devices/") + machine + "/snapshot");
        };
        feed.setShown({{u"a"_s, 480}, {u"b"_s, 480}});
        feed.start();
        // The one that delivers is asked four times a second, as the server names; the one that is
        // down, every three seconds.
        QTRY_VERIFY(asked("b") >= 8);
        QVERIFY2(asked("a") <= 2, qPrintable(QString::number(asked("a"))));
        QTest::qWait(1500);
        QVERIFY2(asked("a") <= 2, qPrintable(QString::number(asked("a"))));
        // It is back: found at its next turn, and asked as the others from then on.
        down = false;
        const int before = asked("a");
        QTRY_VERIFY_WITH_TIMEOUT(asked("a") >= before + 4, 6000);
    }

    void aStreamThatIsBeingWokenIsAskedAgainSoon()
    {
        // A wall whose pictures are asked for every five seconds. The first KVM's stream was idle:
        // the server wakes it, and has no picture for the first three questions.
        int withoutOne = 3;
        StandIn server([&](const Asked& asked) {
            if (asked.path.startsWith("/api/v1/devices/a/snapshot") && withoutOne > 0) {
                --withoutOne;
                return Answered{503, R"({"code": "no_stream", "message": "The stream is starting"})"};
            }
            return ordinary(asked);
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        core::WallFeed feed(client, 100);
        QSignalSpy came(&feed, &core::WallFeed::pictureCame);
        feed.setPictureEvery(5000);
        feed.setShown({{u"a"_s, 480}, {u"b"_s, 480}});
        feed.start();
        // Asked again a second later each time, and not five: its picture is there after three.
        QTRY_VERIFY_WITH_TIMEOUT(server.count("/api/v1/devices/a/snapshot") >= 4, 4500);
        QCOMPARE(server.count("/api/v1/devices/b/snapshot"), 1);
        // From then on at the wall's pace.
        QTest::qWait(1500);
        QCOMPARE(server.count("/api/v1/devices/a/snapshot"), 4);
        QCOMPARE(server.count("/api/v1/devices/b/snapshot"), 1);
    }

    void whoeverIsToldOfAPictureMaySayAtOnceWhatIsShown()
    {
        // A picture has come, and whoever was told lays the wall out anew there and then, as a
        // screen does when a card changes. That is not the machine's next turn.
        StandIn server(ordinary);
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        core::WallFeed feed(client, 100);
        const QHash<QString, int> shown{{u"a"_s, 480}};
        connect(&feed, &core::WallFeed::pictureCame, &feed, [&] { feed.setShown(shown); });
        feed.setPictureEvery(5000);
        feed.setShown(shown);
        feed.start();
        QTRY_COMPARE(server.count("/api/v1/devices/a/snapshot"), 1);
        QTest::qWait(800);
        QCOMPARE(server.count("/api/v1/devices/a/snapshot"), 1);
    }

    void aMachineWhosePictureIsStillOutIsSkippedNotQueued()
    {
        StandIn server([](const Asked& asked) {
            Answered answer = ordinary(asked);
            // The first KVM's pictures never come; the second's do.
            answer.never = asked.path.startsWith("/api/v1/devices/a/");
            return answer;
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        core::WallFeed feed(client, 100);
        QSignalSpy came(&feed, &core::WallFeed::pictureCame);
        feed.setShown({{u"a"_s, 480}, {u"b"_s, 320}});
        feed.start();
        QTRY_VERIFY(came.count() >= 4); // four of the second KVM's, a second's worth
        QCOMPARE(server.count("/api/v1/devices/a/"), 1);
    }

    void aMachineAddedElsewhereIsFollowed()
    {
        int walls = 0;
        StandIn server([&](const Asked& asked) {
            if (asked.path == "/api/v1/wall") {
                // The first time the wall is asked it has one machine less than the status names.
                return ++walls == 1
                           ? Answered{200, R"({"interval": 1, "devices": [{"id": "a", "name": "Build server"}]})"}
                           : Answered{200, wall};
            }
            return ordinary(asked);
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        core::WallFeed feed(client, 100);
        feed.start();
        QTRY_COMPARE(feed.machines().size(), 3);
        QVERIFY(walls >= 2);
    }

    void aSessionThatIsGoneEndsTheFeedAndSaysSo()
    {
        StandIn server(
            [](const Asked&) { return Answered{401, R"({"code": "unauthorized", "message": "Sign in first"})"}; });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        core::WallFeed feed(client, 100);
        QSignalSpy signedOut(&feed, &core::WallFeed::signedOut);
        feed.start();
        QTRY_COMPARE(signedOut.count(), 1);
        QVERIFY(!feed.isRunning());
        // And nothing more is asked.
        const qsizetype asked = server.asked.size();
        QTest::qWait(400);
        QCOMPARE(server.asked.size(), asked);
        QCOMPARE(signedOut.count(), 1);
    }

    void aRefusalThatOnlySaysSoDoesNotEndTheFeed_data()
    {
        QTest::addColumn<QString>("word");
        QTest::newRow("signed out") << u"signed_out"_s;
        QTest::newRow("another certificate") << u"certificate_changed"_s;
        QTest::newRow("an unknown certificate") << u"certificate_unknown"_s;
        QTest::newRow("as the program writes: signed out") << QString::fromLatin1(core::ServerClient::signedOut);
        QTest::newRow("as the program writes: another certificate")
            << QString::fromLatin1(core::ServerClient::certificateChanged);
    }

    void aRefusalThatOnlySaysSoDoesNotEndTheFeed()
    {
        // The feed ends when the session is gone (the server answers 401) or the certificate is not
        // the server's (found on the line, before anything is sent). A picture that is refused with
        // one of those words in the answer is a picture that is refused.
        QFETCH(QString, word);
        StandIn server([&word](const Asked& asked) {
            if (asked.path.contains("/snapshot")) {
                return Answered{400, (u"{\"code\": \"%1\", \"message\": \"No\"}"_s).arg(word).toUtf8()};
            }
            return ordinary(asked);
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        core::WallFeed feed(client, 100);
        const QSignalSpy signedOut(&feed, &core::WallFeed::signedOut);
        const QSignalSpy certificateRefused(&feed, &core::WallFeed::certificateRefused);
        const QSignalSpy pictureFailed(&feed, &core::WallFeed::pictureFailed);
        feed.setShown({{u"a"_s, 480}});
        feed.start();
        QTRY_VERIFY(pictureFailed.count() >= 1);
        QCOMPARE(pictureFailed[0][0].toString(), u"a"_s);
        QCOMPARE(pictureFailed[0][2].toString(), u"No"_s);
        // The feed goes on: the machines are asked how they are, again and again.
        const int looks = server.count("/api/v1/status");
        QTRY_VERIFY(server.count("/api/v1/status") >= looks + 3);
        QCOMPARE(signedOut.count(), 0);
        QCOMPARE(certificateRefused.count(), 0);
        QVERIFY(feed.isRunning());
        QVERIFY(feed.isInContact());
    }

    void lostContactIsSaidAndSoIsItsReturn()
    {
        bool down = true;
        StandIn server([&](const Asked& asked) {
            return down ? Answered{503, R"({"code": "starting", "message": "QKVM is starting"})"} : ordinary(asked);
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        core::WallFeed feed(client, 100);
        QSignalSpy contact(&feed, &core::WallFeed::contactChanged);
        feed.start();
        QTRY_VERIFY(!feed.isInContact());
        QCOMPARE(contact.count(), 1);
        QVERIFY(feed.isRunning()); // it keeps asking

        down = false;
        QTRY_VERIFY(feed.isInContact());
        QCOMPARE(contact.count(), 2);
        QTRY_COMPARE(feed.machines().size(), 3);
    }

    void aNewStartBeginsInContactAndSaysSoWhenThatIsLostAgain()
    {
        StandIn server(
            [](const Asked&) { return Answered{503, R"({"code": "starting", "message": "QKVM is starting"})"}; });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        core::WallFeed feed(client, 100);
        QSignalSpy contact(&feed, &core::WallFeed::contactChanged);
        feed.start();
        QTRY_VERIFY(!feed.isInContact());
        QCOMPARE(contact.count(), 1);
        feed.stop();

        // Whoever starts it again has just been answered by the server: it begins in contact, and
        // says so when that is lost once more.
        feed.start();
        QVERIFY(feed.isInContact());
        QTRY_COMPARE(contact.count(), 2);
        QVERIFY(!feed.isInContact());
    }

    void afterAStopNothingIsToldAnyMore()
    {
        StandIn server(ordinary);
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        core::WallFeed feed(client, 100);
        QSignalSpy wallChanged(&feed, &core::WallFeed::wallChanged);
        QSignalSpy came(&feed, &core::WallFeed::pictureCame);
        feed.setShown({{u"a"_s, 480}});
        feed.start();
        feed.stop(); // before a single answer has come
        QTest::qWait(500);
        QCOMPARE(wallChanged.count(), 0);
        QCOMPARE(came.count(), 0);
        QVERIFY(!feed.isRunning());
    }

    void aFeedThatIsGoneBeforeItsAnswersIsNotCalled()
    {
        StandIn server(ordinary);
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        {
            core::WallFeed feed(client, 100);
            feed.setShown({{u"a"_s, 480}});
            feed.start();
        }
        // The client lives on and its answers come: they find nobody, and nothing happens.
        QTest::qWait(400);
        QVERIFY(server.asked.size() >= 2);
    }
};

QTEST_GUILESS_MAIN(TestWallFeed)
#include "test_wall_feed.moc"
