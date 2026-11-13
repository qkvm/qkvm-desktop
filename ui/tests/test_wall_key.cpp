#include <functional>
#include <memory>
#include <optional>

#include <QSet>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>

#include <qkvm/core/wall_feed.h>
#include <qkvm/core/wall_source.h>

#include "wall_key.h"
#include "wall_machines.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using ui::WallKeyParts;

namespace {

// Pairs of a source's id and a machine's id, among them those that would make one key if a key
// were the two side by side: a machine's id with the separator in it, at its start, at its end,
// alone and twice; an empty one; a very long one, of letters and of separators; and a sign that
// looks like the separator and is not it.
QList<WallKeyParts> pairs()
{
    const QString lookalike(QChar(0x2215));
    QList<WallKeyParts> all;
    for (const QString& source : {u"pc"_s, u"0a1b2c3d"_s, u"p"_s, u"pcc"_s}) {
        for (const QString& machine :
             {u"a"_s, QString(), u"/"_s, u"//"_s, u"a/b"_s, u"/a"_s, u"a/"_s, u"pc/a"_s, u"c/a"_s, u"0a1b2c3d/a"_s,
              lookalike, QString(u"a"_s + lookalike + u"b"_s), QString(100'000, u'x'), QString(100'000, u'/'),
              QString(QString(50'000, u'x') + u'/' + QString(50'000, u'y'))}) {
            all.append(WallKeyParts{source, machine});
        }
    }
    return all;
}

// A place a wall's machines come from that has the machines a test names, and does nothing to any.
// It answers each question a turn later, and counts how often it was asked how they are.
class NamedPlace final : public QObject, public core::WallSource {
public:
    explicit NamedPlace(const QStringList& ids)
    {
        for (const QString& id : ids) {
            core::Machine machine;
            machine.id = id;
            machine.name = id;
            wall.machines.append(machine);
            core::Status how;
            how.state = u"online"_s;
            status.insert(id, how);
        }
    }

    void askWall(std::function<void(base::Result<core::Wall>)> done) override
    {
        later([this, done = std::move(done)] { done(wall); });
    }
    void askStatus(std::function<void(base::Result<QHash<QString, core::Status>>)> done) override
    {
        ++statusAsked;
        later([this, done = std::move(done)] { done(status); });
    }
    void askSnapshot(const QString&, int, double, std::function<void(base::Result<core::Snapshot>)> done) override
    {
        later([done = std::move(done)] { done(base::Error{"no_stream", "No picture"}); });
    }
    void act(const QString&, const core::Action&, std::function<void(base::Result<core::Done>)> done) override
    {
        later([done = std::move(done)] { done(core::Done{}); });
    }
    bool mayHaveBeenDone(const QString& /*machine*/, const base::Error& /*error*/) const override { return false; }
    void askSavedActions(std::function<void(base::Result<QList<core::SavedAction>>)> done) override
    {
        later([done = std::move(done)] { done(QList<core::SavedAction>{}); });
    }
    void askWakeTargets(const QString&, std::function<void(base::Result<QList<core::WakeTarget>>)> done) override
    {
        later([done = std::move(done)] { done(QList<core::WakeTarget>{}); });
    }
    void askConsole(const QString&, std::function<void(base::Result<QUrl>)> done) override
    {
        later([done = std::move(done)] { done(base::Error{"no_console", "No console"}); });
    }
    void arrange(const QStringList& machines, std::function<void(base::Result<QStringList>)> done) override
    {
        later([done = std::move(done), machines] { done(machines); });
    }
    void openLive(const QString&, core::Live::Need,
                  std::function<void(base::Result<std::unique_ptr<core::Live>>)> done) override
    {
        later([done = std::move(done)] { done(base::Error{"no_live", "No live picture"}); });
    }

    core::Wall wall;
    QHash<QString, core::Status> status;
    int statusAsked = 0;

private:
    void later(std::function<void()> what) { QTimer::singleShot(0, this, std::move(what)); }
};

// What a key reaches, as "place: machine" with the place by its name in the test; "nothing" for
// nothing.
QString reached(const ui::OnePlace& place, const QString& key,
                const std::function<QString(const core::WallSource*)>& nameOf)
{
    const ui::Reached at = place.reach(key);
    return at ? nameOf(at.place) + u": "_s + at.machine : u"nothing"_s;
}

} // namespace

// What a machine is known by on a wall of several places, and what a key reaches on the wall of the
// one place there is.
class TestWallKey : public QObject {
    Q_OBJECT

private slots:
    void noTwoPairsMakeOneKey()
    {
        const QList<WallKeyParts> all = pairs();
        QSet<QString> keys;
        for (const WallKeyParts& pair : all) {
            const std::optional<QString> key = ui::wallKey(pair.source, pair.machine);
            QVERIFY(key.has_value());
            keys.insert(*key);
        }
        QCOMPARE(keys.size(), all.size());
    }

    void aKeyTakenApartGivesBackItsPair()
    {
        for (const WallKeyParts& pair : pairs()) {
            const std::optional<WallKeyParts> parts =
                ui::wallKeyParts(ui::wallKey(pair.source, pair.machine).value_or(QString()));
            QVERIFY2(parts == pair, qPrintable(QString(pair.source + u'/' + pair.machine.left(20))));
        }
    }

    void aKeyIsTakenApartAtItsFirstSeparatorOnly()
    {
        QCOMPARE(ui::wallKey(u"pc"_s, u"a/b"_s).value_or(QString()), u"pc/a/b"_s);
        QVERIFY(ui::wallKeyParts(u"pc/a/b"_s) == (WallKeyParts{u"pc"_s, u"a/b"_s}));
        QVERIFY(ui::wallKeyParts(u"pc//"_s) == (WallKeyParts{u"pc"_s, u"/"_s}));
        QVERIFY(ui::wallKeyParts(u"pc/"_s) == (WallKeyParts{u"pc"_s, QString()}));
    }

    void whatIsNoSourceIdMakesNoKey()
    {
        // Empty, or with the separator in it, anywhere: "a/b" and "c" would make the key that "a"
        // and "b/c" make.
        for (const QString& source : {QString(), u"/"_s, u"a/b"_s, u"/a"_s, u"a/"_s}) {
            QVERIFY2(!ui::isSourceId(source), qPrintable(source));
            QVERIFY2(!ui::wallKey(source, u"c"_s).has_value(), qPrintable(source));
        }
        QVERIFY(ui::isSourceId(u"pc"_s));
        QVERIFY(ui::isSourceId(QString(QChar(0x2215))));
    }

    void whatIsNoKeyGivesNothing()
    {
        for (const QString& said : {QString(), u"pc"_s, u"/"_s, u"//"_s, u"/a"_s, u"/a/b"_s, QString(100'000, u'x'),
                                    QString(u"pc"_s + QChar(0x2215) + u"a"_s)}) {
            QVERIFY2(!ui::wallKeyParts(said).has_value(), qPrintable(said.left(20)));
        }
    }

    // --- the one place there is: a key is a machine's own id ---

    void aKeyReachesItsMachineAndOneThatIsNotOnTheWallNothing()
    {
        NamedPlace server({u"a"_s, u"b/c"_s, u"pc/a"_s});
        core::WallFeed feed(server, 60'000);
        ui::OnePlace place(server, feed);
        const auto nameOf = [&](const core::WallSource* asked) {
            return asked == &server ? u"server"_s : u"another"_s;
        };
        // Before the wall is known, no key is on it.
        QCOMPARE(reached(place, u"a"_s, nameOf), u"nothing"_s);
        feed.start();
        QTRY_COMPARE(place.machines().size(), 3);
        QCOMPARE(place.status().size(), 3);
        QCOMPARE(reached(place, u"a"_s, nameOf), u"server: a"_s);
        QCOMPARE(reached(place, u"b/c"_s, nameOf), u"server: b/c"_s);
        QCOMPARE(reached(place, u"pc/a"_s, nameOf), u"server: pc/a"_s);
        // Nothing for what only looks like a key of the wall: a part of one, one in other letters,
        // the empty one, one made of a source's id and a machine's that is there.
        for (const QString& key : {u"b"_s, u"c"_s, u"A"_s, QString(), u"/"_s, u"pc/b/c"_s, u"a "_s}) {
            QCOMPARE(reached(place, key, nameOf), u"nothing"_s);
        }
        // A machine that leaves the wall is reached no more.
        server.wall.machines.removeFirst();
        server.status.remove(u"a"_s);
        feed.askAnew();
        QTRY_COMPARE(place.machines().size(), 2);
        QCOMPARE(reached(place, u"a"_s, nameOf), u"nothing"_s);
        QCOMPARE(reached(place, u"b/c"_s, nameOf), u"server: b/c"_s);
    }

    void anotherPlaceIsTakenOnlyWhileTheFeedIsStoppedAndThenEveryKeyReachesIt()
    {
        NamedPlace server({u"a"_s, u"b"_s});
        NamedPlace own({u"b"_s, u"k"_s});
        core::WallFeed feed(server, 60'000);
        ui::OnePlace place(server, feed);
        const auto nameOf = [&](const core::WallSource* asked) {
            return asked == &server ? u"server"_s : asked == &own ? u"own"_s : u"another"_s;
        };
        const QSignalSpy wallChanged(&place, &ui::WallMachines::wallChanged);
        const QSignalSpy statusChanged(&place, &ui::WallMachines::statusChanged);
        feed.start();
        QTRY_COMPARE(reached(place, u"b"_s, nameOf), u"server: b"_s);
        // What the feed tells, this tells.
        QVERIFY(wallChanged.count() > 0);
        QTRY_VERIFY(statusChanged.count() > 0);
        const qsizetype walls = wallChanged.count();
        const qsizetype looks = statusChanged.count();

        // Under a feed that runs, the place stays the one it was.
        place.setPlace(own);
        QCOMPARE(&place.place(), &server);
        QCOMPARE(reached(place, u"b"_s, nameOf), u"server: b"_s);

        // Stopped: the feed asks the other one, and the wall that was is gone with what it was.
        feed.stop();
        place.setPlace(own);
        QCOMPARE(&place.place(), &own);
        QCOMPARE(wallChanged.count(), walls + 1);
        QCOMPARE(statusChanged.count(), looks + 1);
        QCOMPARE(reached(place, u"a"_s, nameOf), u"nothing"_s);
        QCOMPARE(reached(place, u"b"_s, nameOf), u"nothing"_s);
        feed.start();
        QTRY_COMPARE(place.machines().size(), 2);
        QCOMPARE(reached(place, u"b"_s, nameOf), u"own: b"_s);
        QCOMPARE(reached(place, u"k"_s, nameOf), u"own: k"_s);
        QCOMPARE(reached(place, u"a"_s, nameOf), u"nothing"_s);
    }

    void somethingDoneToAMachineHasTheWallLookedAtSoon()
    {
        NamedPlace server({u"a"_s, u"b"_s});
        // (How they are is asked every minute by itself: a look within two seconds is this one's.)
        core::WallFeed feed(server, 60'000);
        ui::OnePlace place(server, feed);
        feed.start();
        QTRY_COMPARE(place.status().size(), 2);
        QTest::qWait(50);
        const int before = server.statusAsked;
        place.lookAgainSoon(u"a"_s);
        QTRY_VERIFY_WITH_TIMEOUT(server.statusAsked > before, 2500);
    }
};

QTEST_GUILESS_MAIN(TestWallKey)
#include "test_wall_key.moc"
