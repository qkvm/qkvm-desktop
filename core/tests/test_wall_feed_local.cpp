#include <algorithm>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>

#include <qkvm/core/local_wall.h>
#include <qkvm/core/server_client.h>
#include <qkvm/core/wall_feed.h>

#include "made_up_kvm.h"
#include "memory_secrets.h"
#include "stand_in.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::LocalWall;
using core::OwnKvm;
using core::testing::Answered;
using core::testing::Asked;
using core::testing::MadeUpKvm;
using core::testing::MemorySecrets;
using core::testing::savedFor;
using core::testing::StandIn;

namespace {

core::KvmdDevice::Pace quick()
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

// This PC's own wall for a test: what it remembers in a folder that goes with the test, its
// passwords in memory, and drivers that do not wait long.
struct Own {
    QTemporaryDir folder;
    core::Settings settings{folder.filePath(u"settings.json"_s)};
    MemorySecrets secrets;
    LocalWall wall{settings, secrets, quick()};

    // Puts a made-up KVM on the wall, and says what the wall calls it.
    QString add(const MadeUpKvm& kvm, const QString& name)
    {
        OwnKvm own;
        own.address = kvm.stand->address();
        own.name = name;
        return wall.add(own, u"secret"_s).value();
    }
};

// This PC's own wall, which counts how often it is asked how its KVMs are.
class CountedWall : public LocalWall {
public:
    using LocalWall::LocalWall;

    void askStatus(std::function<void(base::Result<QHash<QString, core::Status>>)> done) override
    {
        ++askedHowTheyAre;
        LocalWall::askStatus(std::move(done));
    }

    int askedHowTheyAre = 0;
};

// A wall as an earlier program left it: a settings file that says of no KVM that it was asked
// what it is, and each KVM's password in the store. So each of them is asked what it is at this
// start, before it is signed in to. With the times and the numbers the program has.
struct LeftBehind {
    QTemporaryDir folder;
    MemorySecrets secrets;
    QStringList ids;

    QString file() const { return folder.filePath(u"settings.json"_s); }
    // Writes the file: a KVM at each of these addresses, in their order.
    bool write(const QStringList& addresses)
    {
        QJsonArray entries;
        for (qsizetype index = 0; index < addresses.size(); ++index) {
            const QString id = u"0a10%1"_s.arg(index, 4, 10, QLatin1Char('0'));
            ids.append(id);
            entries.append(QJsonObject{{u"kind"_s, u"kvmd"_s}, {u"id"_s, id}, {u"address"_s, addresses.at(index)}});
            secrets.all.insert(LocalWall::secretOf(id), savedFor("secret", addresses.at(index)));
        }
        QFile written(file());
        return written.open(QIODevice::WriteOnly) &&
               written.write(QJsonDocument(QJsonObject{{u"kvms"_s, entries}}).toJson()) > 0;
    }
};

// A QKVM server with a wall of its own, at four pictures a second: a KVM and a server.
constexpr const char* serversWall = R"({"interval": 0.25, "devices": [
    {"id": "a", "name": "Rack KVM", "host": "192.168.1.21", "kind": "kvmd", "picture": true},
    {"id": "s", "name": "Database server", "host": "192.168.1.71", "kind": "redfish", "picture": false}]})";
constexpr const char* serversStatus = R"({"a": {"state": "no_signal"}, "s": {"state": "online", "picture": false}})";

Answered asAServer(const Asked& asked)
{
    if (asked.path == "/api/v1/wall") {
        return {200, serversWall};
    }
    if (asked.path == "/api/v1/status") {
        return {200, serversStatus};
    }
    return {200, core::testing::aPicture(), "image/jpeg"};
}

} // namespace

// The feed that keeps a wall up to date, over this PC's own wall: made-up KVMs, and no server. And
// from one place a wall's machines come from to the other.
class TestWallFeedLocal : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // The drivers choose Windows' own TLS, which is refused once TLS is in use.
        const core::KvmdDevice first(core::KvmdDevice::Where{}, QString());
    }

    void thisPcsOwnWallIsKeptUpToDateAsAServersIs()
    {
        Own own;
        const MadeUpKvm build;
        const MadeUpKvm nas;
        const QString a = own.add(build, u"Build server"_s);
        const QString b = own.add(nas, u"NAS"_s);
        core::WallFeed feed(own.wall, 100);
        // How the two were, each time it was told.
        QList<QStringList> said;
        connect(&feed, &core::WallFeed::statusChanged, &feed,
                [&] { said.append({feed.status().value(a).state, feed.status().value(b).state}); });
        const QSignalSpy came(&feed, &core::WallFeed::pictureCame);
        // The first KVM is on the screen, 480 pixels wide; the second is not.
        feed.setShown({{a, 480}});
        feed.start();
        // Both machines, in the wall's order, as the wall calls them.
        QTRY_COMPARE(feed.machines().size(), 2);
        QCOMPARE(feed.machines().at(0).id, a);
        QCOMPARE(feed.machines().at(0).name, u"Build server"_s);
        QCOMPARE(feed.machines().at(1).id, b);
        QCOMPARE(feed.machines().at(1).name, u"NAS"_s);
        QCOMPARE(feed.serverPaceMs(), 1000);
        // Its machines can be had live, in the form this program speaks.
        QCOMPARE(feed.liveForm(), core::spokenLiveForm);
        // The first that is told of the two is how they are: the wall's answer waits a moment for
        // the look that the asking began. (This looked for "connecting" of both here first, as an
        // asking said it before its own look had answered, and the wall showed it until the next.)
        QTRY_COMPARE(feed.status().value(a).state, u"online"_s);
        QTRY_COMPARE(feed.status().value(b).state, u"online"_s);
        QCOMPARE(said.constFirst(), (QStringList{u"online"_s, u"online"_s}));
        QVERIFY(feed.isInContact());
        // A picture of the one that is shown, as the KVM gave it; the other is asked for none.
        QTRY_VERIFY(came.count() >= 1);
        QCOMPARE(came[0][0].toString(), a);
        QCOMPARE(came[0][1].toByteArray(), core::testing::jpeg(build.width, build.height));
        QCOMPARE(came[0][2].toBool(), true);
        QVERIFY(build.asked("/api/streamer/snapshot") >= 1);
        QCOMPARE(nas.asked("/api/streamer/snapshot"), 0);
        // And again, at the wall's pace: once a second.
        QTRY_VERIFY(came.count() >= 2);
        QCOMPARE(nas.asked("/api/streamer/snapshot"), 0);
    }

    void aKvmThatIsNotThereIsOfflineAndTheOthersPicturesKeepComing()
    {
        Own own;
        const MadeUpKvm there;
        const core::testing::NobodyThere nobody;
        const QString up = own.add(there, u"There"_s);
        OwnKvm away;
        away.address = nobody.address();
        away.name = u"Away"_s;
        const QString down = own.wall.add(away, u"secret"_s).value();
        core::WallFeed feed(own.wall, 100);
        int ofTheOneThere = 0;
        int ofTheOneAway = 0;
        connect(&feed, &core::WallFeed::pictureCame, &feed, [&](const QString& machine) {
            if (machine == up) {
                ++ofTheOneThere;
            } else {
                ++ofTheOneAway;
            }
        });
        // Why a picture could not be had, each time, as "machine: code: sentence".
        QStringList refused;
        connect(&feed, &core::WallFeed::pictureFailed, &feed,
                [&](const QString& machine, const QString& code, const QString& message) {
                    refused.append(machine + u": "_s + code + u": "_s + message);
                });
        // Both are on the screen, and a picture of each is wanted five times a second.
        feed.setPictureEvery(200);
        feed.setShown({{up, 480}, {down, 480}});
        feed.start();
        // The one that is away says so on its card, by itself: there is no "the wall is away" here.
        QTRY_COMPARE_WITH_TIMEOUT(feed.status().value(down).state, u"offline"_s, 20000);
        QCOMPARE(feed.status().value(down).errorMessage, u"Device is unreachable"_s);
        QTRY_COMPARE(feed.status().value(up).state, u"online"_s);
        QVERIFY(feed.isInContact());
        // Its picture cannot be had, and why is told in the words a card shows.
        QTRY_VERIFY_WITH_TIMEOUT(refused.contains(down + u": unreachable: Device is unreachable"_s), 20000);
        // The other has its pictures as ever, and goes on having them.
        QTRY_VERIFY(ofTheOneThere >= 1);
        const int before = ofTheOneThere;
        QTRY_VERIFY(ofTheOneThere >= before + 3);
        QCOMPARE(feed.status().value(down).state, u"offline"_s);
        QCOMPARE(feed.status().value(up).state, u"online"_s);
        QCOMPARE(ofTheOneAway, 0);
        for (const QString& one : std::as_const(refused)) {
            QVERIFY2(one.startsWith(down + u": "_s), qPrintable(one));
        }
    }

    void aWallThatSaysItHasChangedIsAskedAtOnce()
    {
        Own own;
        // A feed that looks at its machines every five seconds, as one does that is in no hurry.
        core::WallFeed feed(own.wall, 5000);
        // Stopped, it asks nothing whatever it is told.
        feed.askAnew();
        const QSignalSpy wallChanged(&feed, &core::WallFeed::wallChanged);
        feed.start();
        QTRY_COMPARE(wallChanged.count(), 1);
        QVERIFY(feed.machines().isEmpty());

        const MadeUpKvm kvm;
        const QString id = own.add(kvm, u"Build server"_s);
        feed.askAnew();
        // There at once, with how it is: long before the feed's own next look.
        QTRY_COMPARE_WITH_TIMEOUT(feed.machines().size(), 1, 1000);
        QCOMPARE(feed.machines().constFirst().id, id);
        QTRY_VERIFY_WITH_TIMEOUT(feed.status().contains(id), 1000);
        // And gone at once.
        QVERIFY(own.wall.remove(id).ok());
        feed.askAnew();
        QTRY_VERIFY_WITH_TIMEOUT(feed.machines().isEmpty(), 1000);

        // Asked anew while it is asking: the answer on its way may have been made before the
        // change, so the list is asked for once more when that has come.
        feed.askAnew();
        const MadeUpKvm next;
        const QString second = own.add(next, u"NAS"_s);
        feed.askAnew();
        QTRY_COMPARE_WITH_TIMEOUT(feed.machines().size(), 1, 1000);
        QCOMPARE(feed.machines().constFirst().id, second);
    }

    void anotherWallIsTakenOnlyByAFeedThatIsStopped()
    {
        // This PC's own wall of one KVM, and a QKVM server with a wall of two machines.
        Own own;
        const MadeUpKvm kvm;
        const QString mine = own.add(kvm, u"Build server"_s);
        StandIn server(asAServer);
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());

        core::WallFeed feed(own.wall, 100);
        feed.setShown({{mine, 480}, {u"a"_s, 480}});
        feed.start();
        QTRY_COMPARE(feed.status().value(mine).state, u"online"_s);
        QCOMPARE(feed.machines().size(), 1);

        // While it runs, another wall is not taken: the feed keeps the one it has and goes on
        // asking it, and the other is asked nothing.
        const QSignalSpy looked(&feed, &core::WallFeed::statusChanged);
        feed.setSource(client);
        QVERIFY(feed.isRunning());
        QCOMPARE(feed.machines().size(), 1);
        QCOMPARE(feed.machines().constFirst().id, mine);
        QCOMPARE(feed.status().value(mine).state, u"online"_s);
        QTRY_VERIFY(looked.count() >= 3);
        QCOMPARE(feed.machines().constFirst().id, mine);
        QVERIFY(server.asked.isEmpty());

        // Stopped, it is taken. Nothing of the wall that was is kept, and that is told; nothing
        // is asked of the new one before the feed is started.
        feed.stop();
        const QSignalSpy wallChanged(&feed, &core::WallFeed::wallChanged);
        const QSignalSpy statusChanged(&feed, &core::WallFeed::statusChanged);
        feed.setSource(client);
        QVERIFY(!feed.isRunning());
        QVERIFY(feed.machines().isEmpty());
        QVERIFY(feed.status().isEmpty());
        QCOMPARE(wallChanged.count(), 1);
        QCOMPARE(statusChanged.count(), 1);
        QTest::qWait(300);
        QVERIFY(server.asked.isEmpty());
        QCOMPARE(wallChanged.count(), 1);

        // Started, it keeps the server's wall up to date: its machines, how they are, its pace,
        // and a picture of the one that is shown.
        const QSignalSpy came(&feed, &core::WallFeed::pictureCame);
        const int ofMine = kvm.asked("/api/streamer/snapshot");
        feed.start();
        QTRY_COMPARE(feed.machines().size(), 2);
        QCOMPARE(feed.machines().at(0).name, u"Rack KVM"_s);
        QCOMPARE(feed.machines().at(1).id, u"s"_s);
        QTRY_COMPARE(feed.status().value(u"a"_s).state, u"no_signal"_s);
        QVERIFY(!feed.status().contains(mine));
        QCOMPARE(feed.serverPaceMs(), 250);
        QTRY_VERIFY(came.count() >= 3);
        for (const QList<QVariant>& one : came) {
            QCOMPARE(one[0].toString(), u"a"_s);
            QCOMPARE(one[1].toByteArray(), core::testing::aPicture());
        }
        // This PC's KVM is asked for no picture meanwhile.
        QCOMPARE(kvm.asked("/api/streamer/snapshot"), ofMine);

        // And back the same way: the server's wall is forgotten, with the pace it named, and the
        // server is asked no more.
        feed.stop();
        feed.setSource(own.wall);
        QVERIFY(feed.machines().isEmpty());
        QVERIFY(feed.status().isEmpty());
        QCOMPARE(feed.serverPaceMs(), 1000);
        feed.start();
        QTRY_COMPARE(feed.machines().size(), 1);
        QCOMPARE(feed.machines().constFirst().id, mine);
        QTRY_COMPARE(feed.status().value(mine).state, u"online"_s);
        QTRY_VERIFY(kvm.asked("/api/streamer/snapshot") > ofMine);
        // (What went out to the server before the stop has arrived there by now.)
        QTest::qWait(200);
        const qsizetype ofTheServer = server.asked.size();
        QTest::qWait(400);
        QCOMPARE(server.asked.size(), ofTheServer);
    }

    void aTileSaysWhatItsKvmCameToWithinAMomentAndNotAtTheFeedsNextTurn()
    {
        // A wall of twelve quick KVMs as an earlier program left it: each is asked what it is
        // before it is signed in to, and twelve are more than are asked at once.
        std::vector<std::unique_ptr<MadeUpKvm>> kvms;
        QStringList addresses;
        for (int number = 0; number < 12; ++number) {
            kvms.push_back(std::make_unique<MadeUpKvm>());
            addresses.append(kvms.back()->stand->address());
        }
        LeftBehind left;
        QVERIFY(left.write(addresses));
        core::Settings settings(left.file());
        QCOMPARE(settings.remembered().kvms.size(), 12);
        CountedWall wall(settings, left.secrets);
        // The feed of the program, which asks how the machines are every four seconds; and
        // the wall's own word that a KVM has come to something goes to it, as the program has
        // it go there.
        core::WallFeed feed(wall, 4000);
        connect(&wall, &LocalWall::cameToSomething, &feed, &core::WallFeed::lookNow);
        // When each tile first said "online", by the clock of this test; and how often the
        // wall had been asked how its KVMs are when the last of them did.
        QElapsedTimer watch;
        QHash<QString, qint64> onlineAt;
        int askedByThen = 0;
        connect(&feed, &core::WallFeed::statusChanged, &feed, [&] {
            for (const QString& id : std::as_const(left.ids)) {
                if (feed.status().value(id).state == u"online"_s && !onlineAt.contains(id)) {
                    onlineAt.insert(id, watch.elapsed());
                    askedByThen = wall.askedHowTheyAre;
                }
            }
        });
        watch.start();
        feed.start();
        // Every one of them, long before the feed's next turn. (Those beyond the eighth said
        // "connecting" until that turn, four seconds after the start: nothing told the feed
        // that they had been asked and signed in to.)
        QTRY_VERIFY2_WITH_TIMEOUT(
            onlineAt.size() == 12,
            qPrintable(u"%1 of twelve are online after %2 ms"_s.arg(onlineAt.size()).arg(watch.elapsed())), 3500);
        const qint64 last = std::ranges::max(onlineAt.values());
        // (What the wall says of the last of them may still be on its way.)
        QTest::qWait(400);
        qInfo().noquote() << u"twelve quick KVMs: every tile online after %1 ms; the wall had been asked %2 times by "
                             "then, and %3 times %4 ms after the start"_s.arg(last)
                                 .arg(askedByThen)
                                 .arg(wall.askedHowTheyAre)
                                 .arg(watch.elapsed());
        QVERIFY2(last < 2000, qPrintable(QString::number(last)));
        // A few askings for the twelve, and not one for each: what came within a moment of
        // each other is one word of the wall's, and one asking of the feed's.
        QVERIFY2(askedByThen <= 6, qPrintable(QString::number(askedByThen)));
        QVERIFY2(wall.askedHowTheyAre <= 8, qPrintable(QString::number(wall.askedHowTheyAre)));
        // And each was asked what it is once, and signed in to once.
        for (const auto& kvm : kvms) {
            QCOMPARE(kvm->signIns, 1);
            QCOMPARE(kvm->stand->count("/device/status"), 1);
        }
    }

    void atAStartEveryTileSaysWhatItsKvmCameToAlsoWhereThoseThatAreOffStandFirst()
    {
        // A wall of thirty as an earlier program left it. The first ten are off: at five
        // nothing listens, and five are switched off (each takes the call and never says a
        // word: nothing is let in until the time for that is over). Behind them, twenty that
        // are quick. With the times and the numbers the program has, and its feed.
        std::vector<std::unique_ptr<core::testing::NobodyThere>> nothingListens;
        std::vector<std::unique_ptr<QTcpServer>> switchedOff;
        std::vector<std::unique_ptr<MadeUpKvm>> quickOnes;
        QStringList addresses;
        for (int number = 0; number < 5; ++number) {
            nothingListens.push_back(std::make_unique<core::testing::NobodyThere>());
            addresses.append(nothingListens.back()->address());
        }
        for (int number = 0; number < 5; ++number) {
            switchedOff.push_back(std::make_unique<QTcpServer>());
            QVERIFY(switchedOff.back()->listen(QHostAddress::LocalHost, 0));
            addresses.append(u"https://127.0.0.1:%1"_s.arg(switchedOff.back()->serverPort()));
        }
        for (int number = 0; number < 20; ++number) {
            quickOnes.push_back(std::make_unique<MadeUpKvm>());
            addresses.append(quickOnes.back()->stand->address());
        }
        LeftBehind left;
        QVERIFY(left.write(addresses));
        core::Settings settings(left.file());
        QCOMPARE(settings.remembered().kvms.size(), 30);
        CountedWall wall(settings, left.secrets);
        core::WallFeed feed(wall, 4000);
        connect(&wall, &LocalWall::cameToSomething, &feed, &core::WallFeed::lookNow);

        // What the thirty tiles say, each time that is another than before: how many of the
        // ten say "connecting" and how many "offline", how many of the twenty "online", and
        // whatever else was said of any.
        struct Said {
            qint64 at = 0;
            int offConnecting = 0;
            int offOffline = 0;
            int quickConnecting = 0;
            int quickOnline = 0;
            QStringList otherwise;
            bool sameAs(const Said& other) const
            {
                return offConnecting == other.offConnecting && offOffline == other.offOffline &&
                       quickConnecting == other.quickConnecting && quickOnline == other.quickOnline &&
                       otherwise == other.otherwise;
            }
        };
        const QString nothingAnswers =
            u"Nothing answers at this address. No password is sent there before a KVM has answered."_s;
        QElapsedTimer watch;
        QList<Said> said;
        connect(&feed, &core::WallFeed::statusChanged, &feed, [&] {
            Said now;
            now.at = watch.elapsed();
            for (qsizetype index = 0; index < left.ids.size(); ++index) {
                const core::Status status = feed.status().value(left.ids.at(index));
                const bool plain = status.errorCode.isEmpty() && status.errorMessage.isEmpty();
                if (status.state == u"connecting"_s && plain) {
                    ++(index < 10 ? now.offConnecting : now.quickConnecting);
                } else if (index < 10 && status.state == u"offline"_s && status.errorCode == u"unreachable"_s &&
                           status.errorMessage == nothingAnswers) {
                    ++now.offOffline;
                } else if (index >= 10 && status.state == u"online"_s && plain) {
                    ++now.quickOnline;
                } else {
                    now.otherwise.append(
                        u"%1: %2 [%3] %4"_s.arg(index).arg(status.state, status.errorCode, status.errorMessage));
                }
            }
            if (said.isEmpty() || !now.sameAs(said.constLast())) {
                said.append(now);
                // (Each piece with its own mark of what it is made of: the compiler reads two
                // pieces of which only the first has one as plain bytes, inside a lambda.)
                const QString line = u"at %1 s: of the ten that are off, %2 connecting and %3 offline; of the twenty "
                                     u"that are quick, %4 connecting and %5 online; the wall was asked %6 times"_s;
                qInfo().noquote() << line.arg(QString::number(static_cast<double>(now.at) / 1000, 'f', 2))
                                         .arg(now.offConnecting)
                                         .arg(now.offOffline)
                                         .arg(now.quickConnecting)
                                         .arg(now.quickOnline)
                                         .arg(wall.askedHowTheyAre);
            }
        });
        watch.start();
        feed.start();
        // Until each of them has come to what it comes to: the twenty online, the ten offline.
        QTRY_VERIFY_WITH_TIMEOUT(
            !said.isEmpty() && said.constLast().quickOnline == 20 && said.constLast().offOffline == 10, 20000);
        // Nothing else was ever said of any of them: "connecting" and nothing with it, and
        // then what it came to.
        for (const Said& one : std::as_const(said)) {
            QVERIFY2(one.otherwise.isEmpty(), qPrintable(one.otherwise.join(u" | "_s)));
        }
        // The twenty were online long before the feed's next turn, and before one of those
        // that are switched off was given up on: no tile of a KVM that is well waited for
        // either.
        const auto twenty = std::ranges::find_if(said, [](const Said& one) { return one.quickOnline == 20; });
        QVERIFY(twenty != said.end());
        QVERIFY2(twenty->at < 3000, qPrintable(QString::number(twenty->at)));
        QCOMPARE(twenty->offConnecting + twenty->offOffline, 10);
        // And each of those that are off within a moment of when it was given up on: the five
        // that are switched off after as long as a line may take to be let in, and no tile of
        // them waited for the feed's turn after that, which is at eight seconds.
        const core::KvmdDevice::Pace pace;
        const auto ten = std::ranges::find_if(said, [](const Said& one) { return one.offOffline == 10; });
        QVERIFY(ten != said.end());
        QVERIFY2(ten->at >= pace.connectWithinMs - 100 && ten->at < 7000, qPrintable(QString::number(ten->at)));
        for (const auto& kvm : quickOnes) {
            QCOMPARE(kvm->signIns, 1);
        }
        qInfo().noquote() << u"thirty KVMs, ten of them off and first: the wall was asked how they are %1 times in "
                             "%2 s"_s.arg(wall.askedHowTheyAre)
                                 .arg(QString::number(static_cast<double>(watch.elapsed()) / 1000, 'f', 1));
    }
};

QTEST_GUILESS_MAIN(TestWallFeedLocal)
#include "test_wall_feed_local.moc"
