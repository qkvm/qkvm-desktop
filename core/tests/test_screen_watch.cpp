#include <atomic>
#include <memory>
#include <optional>
#include <utility>

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QPoint>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QThreadPool>
#include <QTimeZone>

#include <qkvm/core/kvm_editing.h>
#include <qkvm/core/local_wall.h>
#include <qkvm/core/screen_watch.h>
#include <qkvm/core/settings.h>

#include "made_up_kvm.h"
#include "memory_secrets.h"
#include "stand_in.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::ScreenWatch;
using core::testing::MadeUpKvm;
using core::testing::MemorySecrets;

// The screen watches of this PC's own wall (c7; the web's screens.py): no signal, or a screen unchanged, for N minutes,
// told once with the picture that set it off, kept beside the settings; back to normal when it is over, also after a
// restart; pictures compared off the interface thread; kept 30 days, and gone with their KVM.
class TestScreenWatch : public QObject {
    Q_OBJECT

private:
    // A screen of one shade, with a light box where one is asked for (a pointer, a window): the beginning of a JPEG
    // that says its size, and behind it what the test's decoder reads (shrunk). Core decodes no pictures: the program
    // hands its watches a decoder (ui's shrunkScreen, tried in test_ui_own_screen_alerts).
    static QByteArray screen(int shade, std::optional<QPoint> box = std::nullopt, int width = 640, int height = 360)
    {
        return core::testing::jpeg(width, height, 8) + static_cast<char>(shade) + (box ? 'b' : 'n');
    }
    static std::optional<QByteArray> shrunk(const QByteArray& picture)
    {
        if (picture.size() < 2) {
            return std::nullopt;
        }
        QByteArray squares(qsizetype{ScreenWatch::across} * ScreenWatch::down, picture.at(picture.size() - 2));
        if (picture.endsWith('b')) {
            for (int at = 0; at < 200; ++at) {
                squares[at] = static_cast<char>(255);
            }
        }
        return squares;
    }

    struct Clocks {
        qint64 ms = 1000;
        QDateTime wall = QDateTime(QDate(2026, 11, 6), QTime(12, 0), QTimeZone(QTimeZone::UTC));
    };
    static ScreenWatch::Times timesOf(Clocks& clocks)
    {
        ScreenWatch::Times times;
        times.roundMs = 3'600'000; // (the test has its rounds made: roundNow)
        times.minuteMs = 1000;
        times.clockMs = [&clocks] { return clocks.ms; };
        times.wallClock = [&clocks] { return clocks.wall.addMSecs(clocks.ms); };
        times.shrink = &TestScreenWatch::shrunk;
        return times;
    }
    // A round, once what stands is read back (none is made before), and whatever it tells once its comparing and its
    // files are over.
    static QList<ScreenWatch::Event> round(ScreenWatch& watch)
    {
        if (!QTest::qWaitFor([&] { return !watch.isReading(); }, 10000)) {
            return {};
        }
        QSignalSpy said(&watch, &ScreenWatch::alerted);
        watch.roundNow();
        if (!QTest::qWaitFor([&] { return !watch.isComparing(); }, 10000)) {
            return {};
        }
        // (The pictures of what went off are kept first, in its thread.)
        said.wait(300);
        QList<ScreenWatch::Event> told;
        for (const QList<QVariant>& one : std::as_const(said)) {
            told.append(one.at(0).value<ScreenWatch::Event>());
        }
        return told;
    }
    static QStringList pictures(const QString& folder) { return QDir(folder).entryList({u"*.jpg"_s}, QDir::Files); }

private slots:
    void initTestCase()
    {
        qRegisterMetaType<ScreenWatch::Event>();
        const core::KvmdDevice first(core::KvmdDevice::Where{}, QString());
    }

    void minutesAreHeldToADay()
    {
        QCOMPARE(ScreenWatch::minutesFrom(-5), 0);
        QCOMPARE(ScreenWatch::minutesFrom(0), 0);
        QCOMPARE(ScreenWatch::minutesFrom(30), 30);
        QCOMPARE(ScreenWatch::minutesFrom(5000), 1440);
    }

    void aScreenUnchangedForItsMinutesIsToldWithItsPictureAndBackWhenItChanges()
    {
        const QTemporaryDir folder;
        Clocks clocks;
        ScreenWatch watch(timesOf(clocks));
        watch.keepIn(folder.path());
        watch.watch([] { return QList<ScreenWatch::Watched>{{u"k1"_s, u"online"_s, 0, 2}}; });
        static_cast<void>(round(watch)); // (who is watched is learnt at a round)
        const QByteArray still = screen(40);
        for (int second = 0; second < 2; ++second) {
            watch.saw(u"k1"_s, still, true);
            QVERIFY(round(watch).isEmpty());
            clocks.ms += 1000;
        }
        watch.saw(u"k1"_s, still, true);
        const QList<ScreenWatch::Event> told = round(watch);
        QCOMPARE(told.size(), 1);
        QCOMPARE(told.constFirst().watch, u"unchanged"_s);
        QVERIFY(!told.constFirst().back);
        QCOMPARE(told.constFirst().minutes, 2);
        QVERIFY(!told.constFirst().stamp.isEmpty());
        QCOMPARE(pictures(folder.path()), QStringList{u"k1-"_s + told.constFirst().stamp + u".jpg"_s});
        QCOMPARE(watch.standing(u"k1"_s), QStringList{u"unchanged"_s});
        // Told once.
        clocks.ms += 1000;
        watch.saw(u"k1"_s, still, true);
        QVERIFY(round(watch).isEmpty());
        // Its picture, as it was kept.
        std::optional<std::optional<QByteArray>> kept;
        watch.askPicture(u"k1"_s, told.constFirst().stamp,
                         [&kept](const std::optional<QByteArray>& bytes) { kept = bytes; });
        QTRY_VERIFY(kept.has_value());
        QCOMPARE(kept.value_or(std::nullopt).value_or(QByteArray()), still);
        // A window that opened: back to normal.
        clocks.ms += 1000;
        watch.saw(u"k1"_s, screen(40, QPoint(100, 100)), true);
        const QList<ScreenWatch::Event> back = round(watch);
        QCOMPARE(back.size(), 1);
        QVERIFY(back.constFirst().back);
        QVERIFY(watch.standing(u"k1"_s).isEmpty());
        QCOMPARE(watch.told(u"k1"_s).size(), 2);
        // Unchanged again: only the last alert's picture is kept.
        const QByteArray window = screen(40, QPoint(100, 100));
        for (int second = 0; second < 3; ++second) {
            clocks.ms += 1000;
            watch.saw(u"k1"_s, window, true);
            static_cast<void>(round(watch));
        }
        QCOMPARE(watch.standing(u"k1"_s), QStringList{u"unchanged"_s});
        QTRY_COMPARE(pictures(folder.path()).size(), 1);
        QVERIFY(pictures(folder.path()).constFirst() != u"k1-"_s + told.constFirst().stamp + u".jpg"_s);
    }

    void picturesAreComparedOffTheInterfaceThread()
    {
        Clocks clocks;
        ScreenWatch::Times times = timesOf(clocks);
        std::atomic<bool> elsewhere = false;
        QThread* const here = QThread::currentThread();
        times.whileComparing = [&elsewhere, here] { elsewhere = QThread::currentThread() != here; };
        ScreenWatch watch(times);
        watch.watch([] { return QList<ScreenWatch::Watched>{{u"k1"_s, u"online"_s, 0, 2}}; });
        static_cast<void>(round(watch));
        watch.saw(u"k1"_s, screen(40, std::nullopt, 3840, 2160), true);
        watch.roundNow();
        // (The round is back before the picture is shrunk: that is done in the pool, and told back.)
        QVERIFY(watch.isComparing());
        QTRY_VERIFY(!watch.isComparing());
        QVERIFY(elsewhere);
    }

    void noSignalForItsMinutesIsToldWithTheLastPictureAndBackWithAPicture()
    {
        const QTemporaryDir folder;
        Clocks clocks;
        ScreenWatch watch(timesOf(clocks));
        watch.keepIn(folder.path());
        QString state = u"online"_s;
        watch.watch([&state] { return QList<ScreenWatch::Watched>{{u"k1"_s, state, 3, 0}}; });
        static_cast<void>(round(watch));
        const QByteArray last = screen(90);
        watch.saw(u"k1"_s, last, true);
        QVERIFY(round(watch).isEmpty());
        state = u"no_signal"_s;
        for (int second = 0; second < 3; ++second) {
            QVERIFY(round(watch).isEmpty());
            clocks.ms += 1000;
        }
        const QList<ScreenWatch::Event> told = round(watch);
        QCOMPARE(told.size(), 1);
        QCOMPARE(told.constFirst().watch, u"no_signal"_s);
        QVERIFY(!told.constFirst().stamp.isEmpty());
        std::optional<std::optional<QByteArray>> kept;
        watch.askPicture(u"k1"_s, told.constFirst().stamp,
                         [&kept](const std::optional<QByteArray>& bytes) { kept = bytes; });
        QTRY_VERIFY(kept.has_value());
        QCOMPARE(kept.value_or(std::nullopt).value_or(QByteArray()), last);
        // Away for a while is neither: it stands.
        state = u"offline"_s;
        QVERIFY(round(watch).isEmpty());
        state = u"online"_s;
        const QList<ScreenWatch::Event> back = round(watch);
        QCOMPARE(back.size(), 1);
        QVERIFY(back.constFirst().back);
        QCOMPARE(back.constFirst().watch, u"no_signal"_s);
    }

    void aKvmNotWatchedIsNotLookedAt()
    {
        // Not watched at all, and watched for no signal alone: no picture of either is ever compared.
        const QTemporaryDir folder;
        Clocks clocks;
        ScreenWatch watch(timesOf(clocks));
        watch.keepIn(folder.path());
        watch.watch(
            [] { return QList<ScreenWatch::Watched>{{u"k1"_s, u"no_signal"_s, 0, 0}, {u"k2"_s, u"online"_s, 5, 0}}; });
        static_cast<void>(round(watch));
        for (int second = 0; second < 3; ++second) {
            watch.saw(u"k1"_s, screen(40), true);
            watch.saw(u"k2"_s, screen(40), true);
            watch.roundNow();
            QVERIFY(!watch.isComparing());
            QVERIFY(round(watch).isEmpty());
            clocks.ms += 100'000;
        }
        QVERIFY(pictures(folder.path()).isEmpty());
    }

    void whatStandsIsReadBackAfterARestartAndBackIsToldThen()
    {
        const QTemporaryDir folder;
        Clocks clocks;
        const QByteArray still = screen(40);
        {
            ScreenWatch watch(timesOf(clocks));
            watch.keepIn(folder.path());
            watch.watch([] { return QList<ScreenWatch::Watched>{{u"k1"_s, u"online"_s, 0, 1}}; });
            static_cast<void>(round(watch));
            watch.saw(u"k1"_s, still, true);
            static_cast<void>(round(watch));
            clocks.ms += 1000;
            watch.saw(u"k1"_s, still, true);
            QCOMPARE(round(watch).size(), 1);
        }
        ScreenWatch again(timesOf(clocks));
        again.keepIn(folder.path());
        QTRY_COMPARE(again.standing(u"k1"_s), QStringList{u"unchanged"_s});
        QCOMPARE(again.told(u"k1"_s).size(), 1);
        QVERIFY(!again.told(u"k1"_s).constFirst().stamp.isEmpty());
        again.watch([] { return QList<ScreenWatch::Watched>{{u"k1"_s, u"online"_s, 0, 1}}; });
        static_cast<void>(round(again));
        again.saw(u"k1"_s, still, true);
        QVERIFY(round(again).isEmpty()); // (the first picture of the run)
        clocks.ms += 500;
        again.saw(u"k1"_s, screen(200), true);
        const QList<ScreenWatch::Event> back = round(again);
        QCOMPARE(back.size(), 1);
        QVERIFY(back.constFirst().back);
    }

    void aKvmThatLeavesTheWallTakesItsPictureAlongAndOldOnesGo()
    {
        const QTemporaryDir folder;
        Clocks clocks;
        ScreenWatch::Times times = timesOf(clocks);
        times.sweepEveryMs = 1;
        ScreenWatch watch(times);
        watch.keepIn(folder.path());
        // One kept 31 days ago, of a KVM that is still on the wall.
        QFile old(QDir(folder.path()).filePath(u"k2-1000.jpg"_s));
        QVERIFY(old.open(QIODevice::WriteOnly));
        old.write(screen(10));
        old.setFileTime(clocks.wall.addDays(-31), QFileDevice::FileModificationTime);
        old.close();
        watch.watch([] { return QList<ScreenWatch::Watched>{{u"k1"_s, u"online"_s, 0, 1}}; });
        static_cast<void>(round(watch));
        watch.saw(u"k1"_s, screen(40), true);
        static_cast<void>(round(watch));
        clocks.ms += 1000;
        watch.saw(u"k1"_s, screen(40), true);
        QCOMPARE(round(watch).size(), 1);
        QTRY_COMPARE(pictures(folder.path()).size(), 1);
        QVERIFY(pictures(folder.path()).constFirst().startsWith(u"k1-"_s));
        watch.forget(u"k1"_s);
        clocks.ms += 1000;
        static_cast<void>(round(watch));
        QTRY_VERIFY(pictures(folder.path()).isEmpty());
        QVERIFY(watch.told(u"k1"_s).isEmpty());
    }

    void oldPicturesGoAtTheFirstRoundOfARun()
    {
        // Review 4's LOW 4: the pictures kept past their thirty days go at the first round of a run too, and not only
        // once the program has run for an hour (one that never runs that long kept them for ever).
        const QTemporaryDir folder;
        Clocks clocks;
        ScreenWatch watch(timesOf(clocks));
        QFile old(QDir(folder.path()).filePath(u"k2-1000.jpg"_s));
        QVERIFY(old.open(QIODevice::WriteOnly));
        old.write(screen(10));
        old.setFileTime(clocks.wall.addDays(-31), QFileDevice::FileModificationTime);
        old.close();
        QFile young(QDir(folder.path()).filePath(u"k3-2000.jpg"_s));
        QVERIFY(young.open(QIODevice::WriteOnly));
        young.write(screen(10));
        young.setFileTime(clocks.wall.addDays(-29), QFileDevice::FileModificationTime);
        young.close();
        watch.keepIn(folder.path());
        watch.watch([] { return QList<ScreenWatch::Watched>{{u"k1"_s, u"online"_s, 0, 1}}; });
        static_cast<void>(round(watch));
        QTRY_COMPARE(pictures(folder.path()), QStringList{u"k3-2000.jpg"_s});
    }

    void aKvmOfThisPcsWallIsWatchedByThePicturesItsCardsAreHanded()
    {
        // The wall shows its watches the pictures it hands out, and asks nothing of a KVM for them; and what a KVM is
        // watched for is kept with it.
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        core::LocalWall::Own own;
        own.screens.roundMs = 3'600'000;
        own.screens.minuteMs = 300;
        own.screens.shrink = &TestScreenWatch::shrunk;
        core::KvmdDevice::Pace pace;
        pace.connectWithinMs = 1500;
        pace.quietMs = 1500;
        pace.transferMs = 1500;
        pace.freshMs = 200;
        pace.firstContactMs = 400;
        pace.patienceMs = 400;
        pace.boards = std::make_shared<core::BoardHolds>();
        core::LocalWall wall(settings, secrets, pace, own);
        wall.screens().keepIn(folder.filePath(u"alert-pictures"_s));
        MadeUpKvm kvm;
        kvm.picture = screen(40);
        core::OwnKvm added;
        added.address = kvm.stand->address();
        added.unchangedMinutes = 1;
        added.noSignalMinutes = 5000; // (held to a day)
        const base::Result<QString> id = wall.add(added, u"secret"_s);
        QVERIFY(id.ok());
        QSignalSpy said(&wall.screens(), &ScreenWatch::alerted);
        QElapsedTimer took;
        took.start();
        while (said.isEmpty() && took.elapsed() < 10'000) {
            std::optional<base::Result<core::Snapshot>> picture;
            wall.askSnapshot(id.value(), 400, 0,
                             [&picture](const base::Result<core::Snapshot>& came) { picture = came; });
            QTRY_VERIFY(picture.has_value());
            wall.screens().roundNow();
            QTRY_VERIFY(!wall.screens().isComparing());
            QTest::qWait(150);
        }
        QCOMPARE(said.size(), 1);
        QCOMPARE(said.constFirst().at(0).value<ScreenWatch::Event>().watch, u"unchanged"_s);
        QCOMPARE(said.constFirst().at(0).value<ScreenWatch::Event>().machine, id.value());
        // Changed in its Change box: saved with the rest, nothing a sign-in guards.
        {
            core::KvmEditing editing(wall);
            core::KvmEditing::Typed typed;
            typed.address = wall.kvms().constFirst().address;
            typed.user = wall.kvms().constFirst().user;
            typed.noSignalMinutes = 3;
            typed.unchangedMinutes = 1;
            std::optional<core::KvmEditing::Outcome> came;
            editing.change(id.value(), typed, [&came](const core::KvmEditing::Outcome& outcome) { came = outcome; });
            QTRY_VERIFY(came.has_value());
            QVERIFY(came->result.ok());
            QCOMPARE(wall.kvms().constFirst().noSignalMinutes, 3);
            QCOMPARE(wall.kvms().constFirst().unchangedMinutes, 1);
        }
        // Kept with it, and read back so.
        QVERIFY(wall.screens().standing(id.value()) == QStringList{u"unchanged"_s});
        {
            static_cast<void>(settings.writeDown());
            const core::Settings reread(folder.filePath(u"settings.json"_s));
            QCOMPARE(reread.remembered().kvms.constFirst().unchangedMinutes, 1);
            QCOMPARE(reread.remembered().kvms.constFirst().noSignalMinutes, 3);
        }
        // Off the wall: nothing of it stands.
        QVERIFY(wall.remove(id.value()).ok());
        QVERIFY(wall.screens().standing(id.value()).isEmpty());
    }

    void whatStandsIsReadOffTheInterfaceThreadAndTakenInOnceItIs()
    {
        // Review 4's LOW 5: screens.json (up to a megabyte) is read in its file thread; until it is in, no round is
        // made, and what is written waits for it, so nothing read is written over; then it is said (readBack).
        const QTemporaryDir folder;
        Clocks clocks;
        {
            ScreenWatch first(timesOf(clocks));
            first.keepIn(folder.path());
            first.watch(
                [] { return QList<ScreenWatch::Watched>{{u"k1"_s, u"online"_s, 0, 1}, {u"k2"_s, u"online"_s, 0, 1}}; });
            QTRY_VERIFY(!first.isReading());
            static_cast<void>(round(first));
            for (const QString& machine : {u"k1"_s, u"k2"_s}) {
                first.saw(machine, screen(40), true);
            }
            static_cast<void>(round(first));
            clocks.ms += 1000;
            for (const QString& machine : {u"k1"_s, u"k2"_s}) {
                first.saw(machine, screen(40), true);
            }
            QCOMPARE(round(first).size(), 2);
        }
        ScreenWatch::Times times = timesOf(clocks);
        auto held = std::make_shared<std::atomic<bool>>(true);
        auto elsewhere = std::make_shared<std::atomic<bool>>(false);
        // (And between each two things its thread does, what stands for k1 is never gone from the file: nothing is
        // written over what is still being read.)
        auto lost = std::make_shared<std::atomic<bool>>(false);
        QThread* const here = QThread::currentThread();
        times.whileFiling = [held, elsewhere, lost, here, state = QDir(folder.path()).filePath(u"screens.json"_s)] {
            *elsewhere = QThread::currentThread() != here;
            QFile file(state);
            if (!file.open(QIODevice::ReadOnly) || !file.readAll().contains("\"k1\"")) {
                *lost = true;
            }
            QElapsedTimer waited;
            waited.start();
            while (*held && waited.elapsed() < 10'000) {
                QThread::msleep(5);
            }
        };
        ScreenWatch again(times);
        QSignalSpy read(&again, &ScreenWatch::readBack);
        again.keepIn(folder.path());
        // (Its file thread holds still: nothing of it is in yet, and nothing waits for it here.)
        QVERIFY(again.isReading());
        QVERIFY(again.standing(u"k1"_s).isEmpty());
        again.watch([] { return QList<ScreenWatch::Watched>{{u"k1"_s, u"online"_s, 0, 1}}; });
        again.roundNow();
        QVERIFY(!again.isComparing());
        // A KVM that leaves the wall meanwhile: what is read of it goes too, and the file is written once all is in.
        again.forget(u"k2"_s);
        *held = false;
        QTRY_VERIFY(!again.isReading());
        QCOMPARE(read.size(), 1);
        QVERIFY(*elsewhere);
        QCOMPARE(again.standing(u"k1"_s), QStringList{u"unchanged"_s});
        QVERIFY(again.standing(u"k2"_s).isEmpty());
        QVERIFY(again.told(u"k2"_s).isEmpty());
        // Written so (k1 kept, k2 gone), and read back so by a third.
        QTest::qWait(300);
        ScreenWatch third(timesOf(clocks));
        third.keepIn(folder.path());
        QTRY_VERIFY(!third.isReading());
        QCOMPARE(third.standing(u"k1"_s), QStringList{u"unchanged"_s});
        QVERIFY(third.told(u"k2"_s).isEmpty());
        QVERIFY(!*lost);
    }

    void itsGoingWaitsForItsFileThreadOnlySoLong()
    {
        // Review 4's LOW 5: a file thread that does not come back (a disk that hangs) holds the interface thread up no
        // longer than Times::filesWithinMs as this goes; what is left is the program's to see to the end of.
        const QTemporaryDir folder;
        Clocks clocks;
        ScreenWatch::Times times = timesOf(clocks);
        times.filesWithinMs = 300;
        auto held = std::make_shared<std::atomic<bool>>(true);
        times.whileFiling = [held] {
            QElapsedTimer waited;
            waited.start();
            while (*held && waited.elapsed() < 5000) {
                QThread::msleep(5);
            }
        };
        auto watch = std::make_unique<ScreenWatch>(times);
        watch->keepIn(folder.path());
        watch->forget(u"k1"_s); // (something for its thread to do)
        QTest::qWait(100);
        QElapsedTimer took;
        took.start();
        watch.reset();
        const qint64 waited = took.elapsed();
        *held = false;
        QVERIFY2(waited < 2000, QByteArray::number(waited).constData());
    }

    void aRoundsComparingStopsWhenItGoes()
    {
        // Review 4's LOW 5: its comparing in the program's pool stops after the picture in hand as this goes, so the
        // program's end (which waits for that pool) does not wait out the rest of a round, up to Times::roundMostMs.
        Clocks clocks;
        ScreenWatch::Times times = timesOf(clocks);
        times.roundMostMs = 60'000;
        times.shrink = [](const QByteArray& picture) {
            QThread::msleep(200);
            return shrunk(picture);
        };
        QList<ScreenWatch::Watched> many;
        for (int machine = 0; machine < 30; ++machine) {
            many.append({u"k%1"_s.arg(machine), u"online"_s, 0, 1});
        }
        auto watch = std::make_unique<ScreenWatch>(times);
        watch->watch([many] { return many; });
        static_cast<void>(round(*watch));
        for (const ScreenWatch::Watched& each : std::as_const(many)) {
            watch->saw(each.machine, screen(40), true);
        }
        watch->roundNow();
        QVERIFY(watch->isComparing());
        QTest::qWait(300);
        watch.reset();
        QElapsedTimer took;
        took.start();
        QVERIFY(QThreadPool::globalInstance()->waitForDone(10'000));
        QVERIFY2(took.elapsed() < 1500, QByteArray::number(took.elapsed()).constData());
    }
};

QTEST_MAIN(TestScreenWatch)
#include "test_screen_watch.moc"
