#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <qkvm/core/video_diagnostics.h>

using namespace Qt::StringLiterals;
using qkvm::core::VideoDiagnostics;

namespace {
// Where a test's writer would end the program (std::terminate on its thread), it says so here and stops there
// instead, so that the test fails and does not crash. (Under Microsoft's runtime each thread has its own terminate
// handler: the writer's own sets it.)
std::atomic<bool> terminatedOnTheWriter = false;
[[noreturn]] void parkInstead()
{
    terminatedOnTheWriter = true;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::hours(1));
    }
}
} // namespace

class TestVideoDiagnostics : public QObject {
    Q_OBJECT
private slots:
    void boundedFilesContainOnlyStructuredMeasurements()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(u"video.jsonl"_s);
        {
            VideoDiagnostics log(path, 1024);
            QVERIFY(log.isOpen());
            VideoDiagnostics::record("unsafe/url?token=secret");
            VideoDiagnostics::record("decoder", {{"unsafe/header", 1}});
            log.flush();
            QFile empty(path);
            QVERIFY(empty.open(QIODevice::ReadOnly));
            QVERIFY(empty.readAll().isEmpty());
            empty.close();
            for (int index = 0; index < 100; ++index) {
                VideoDiagnostics::record("decoder", {{"decoded", index}, {"gpu", 1}});
            }
        }
        for (const QString& name : {path, path + u".1"_s}) {
            QFile file(name);
            QVERIFY(file.open(QIODevice::ReadOnly));
            QVERIFY(file.size() > 0 && file.size() <= 1024);
            while (!file.atEnd()) {
                const QJsonDocument entry = QJsonDocument::fromJson(file.readLine());
                QVERIFY(entry.isObject());
                QCOMPARE(entry.object()[u"event"_s].toString(), u"decoder"_s);
                const QJsonObject values = entry.object()[u"values"_s].toObject();
                QCOMPARE(values.size(), 2);
                QCOMPARE(values[u"gpu"_s].toInt(), 1);
                QVERIFY(values[u"decoded"_s].isDouble());
            }
        }
        const qint64 before = QFileInfo(path).size();
        VideoDiagnostics::record("closed", {{"value", 1}});
        QCOMPARE(QFileInfo(path).size(), before);
    }
    void unavailableLogDoesNotPreventVideoWork()
    {
        QTemporaryDir directory;
        VideoDiagnostics log(directory.path());
        QVERIFY(!log.isOpen());
        VideoDiagnostics::record("unavailable");
        log.flush();
    }

    // The reading of 077e539..2c3f523, L3: nothing recorded waits for the disk (the program records from its interface
    // thread, which nothing written may stall). With the disk held, a hundred lines are recorded at once and none is
    // written yet; let go, every one is.
    void aRecordNeverWaitsForTheDisk()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(u"video.jsonl"_s);
        const auto disk = std::make_shared<Held>();
        {
            VideoDiagnostics log(path, qint64{1024} * 1024, {.beforeWriting = [disk] { disk->waitWhileHeld(); }});
            QVERIFY(log.isOpen());
            QElapsedTimer recording;
            recording.start();
            for (int index = 0; index < 100; ++index) {
                VideoDiagnostics::record("decoder", {{"decoded", index}});
            }
            QVERIFY2(recording.elapsed() < 500, qPrintable(QString::number(recording.elapsed())));
            QTest::qWait(100);
            QCOMPARE(QFileInfo(path).size(), qint64{0});
            disk->letGo();
            log.flush();
            QCOMPARE(linesOf(path, "decoder"), 100);
        }
    }

    // What cannot wait for the disk is dropped, counted, and said in a line of its own once there is room: at the most
    // mostWaiting lines wait.
    void whatCannotWaitIsCountedAndSaid()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(u"video.jsonl"_s);
        const auto disk = std::make_shared<Held>();
        {
            VideoDiagnostics log(path, qint64{1024} * 1024, {.beforeWriting = [disk] { disk->waitWhileHeld(); }});
            for (int index = 0; index < 3000; ++index) {
                VideoDiagnostics::record("decoder", {{"decoded", index}});
            }
            disk->letGo();
            log.flush();
        }
        const int written = linesOf(path, "decoder");
        QVERIFY2(written <= VideoDiagnostics::mostWaiting + 1, qPrintable(QString::number(written)));
        QCOMPARE(written + droppedSaidIn(path), 3000);
    }

    // From the second reading of c7ec58c, L4: the file's times are in order. What was dropped is said once every line
    // that waited before it is written, after them (it was said ahead of up to 1023 older lines, with a later time).
    void theFilesTimesAreInOrder()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(u"video.jsonl"_s);
        const auto disk = std::make_shared<Held>();
        {
            VideoDiagnostics log(path, qint64{1024} * 1024, {.beforeWriting = [disk] { disk->waitWhileHeld(); }});
            for (int index = 0; index < 1500; ++index) {
                VideoDiagnostics::record("decoder", {{"decoded", index}});
            }
            QTest::qWait(50); // (a count said as the disk lets go has a later time than every line that waits)
            disk->letGo();
            log.flush();
            for (int index = 0; index < 5; ++index) {
                VideoDiagnostics::record("after", {{"index", index}});
            }
            log.flush();
        }
        const QList<QDateTime> times = timesIn(path);
        QVERIFY2(times.size() >= VideoDiagnostics::mostWaiting + 6, qPrintable(QString::number(times.size())));
        for (qsizetype at = 1; at < times.size(); ++at) {
            QVERIFY2(times[at - 1] <= times[at],
                     qPrintable(u"line %1: %2 after %3"_s.arg(at + 1).arg(times[at].toString(Qt::ISODateWithMs),
                                                                          times[at - 1].toString(Qt::ISODateWithMs))));
        }
        QCOMPARE(linesOf(path, "decoder") + droppedSaidIn(path), 1500);
        QCOMPARE(linesOf(path, "after"), 5);
    }

    // From the second reading of c7ec58c, L1: a writer whose thread cannot be begun (std::thread throws) leaves
    // diagnostics off, and nothing leaves where it is made (the program ended at its start); another can be made then.
    void aWriterWhoseThreadCannotBeBegunLeavesDiagnosticsOff()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(u"video.jsonl"_s);
        {
            VideoDiagnostics log(path, qint64{1024} * 1024, {.beforeStarting = [] {
                                     throw std::system_error(
                                         std::make_error_code(std::errc::resource_unavailable_try_again));
                                 }});
            QVERIFY(!log.isOpen());
            VideoDiagnostics::record("decoder", {{"decoded", 1}});
            log.flush();
        }
        QVERIFY(!QFileInfo::exists(path));
        const VideoDiagnostics after(path);
        QVERIFY(after.isOpen());
        VideoDiagnostics::record("decoder", {{"decoded", 2}});
        after.flush();
        QCOMPARE(linesOf(path, "decoder"), 1);
    }

    // L2: whatever throws on the writer's thread (memory run out, a file, a test's disk) is caught there, the line it
    // was writing counted with the dropped ones, and the lines after it written as ever (it ended the program).
    void whatThrowsOnTheWritersThreadIsCaughtAndCounted()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(u"video.jsonl"_s);
        terminatedOnTheWriter = false;
        auto calls = std::make_shared<int>(0);
        auto before = std::make_shared<std::terminate_handler>(nullptr);
        {
            VideoDiagnostics log(path, qint64{1024} * 1024, {.beforeWriting = [calls, before] {
                                     const int call = ++*calls;
                                     if (call == 1) {
                                         *before = std::set_terminate(&parkInstead);
                                     }
                                     if (call == 9) {
                                         std::set_terminate(*before);
                                     }
                                     if (call % 3 == 0) {
                                         throw std::runtime_error("a disk that throws");
                                     }
                                 }});
            for (int index = 0; index < 9; ++index) {
                VideoDiagnostics::record("decoder", {{"decoded", index}});
            }
            // (Bounded: where its thread ended or went silent, a flush would wait for ever.)
            QTRY_VERIFY_WITH_TIMEOUT(linesOf(path, "decoder") + droppedSaidIn(path) == 9 || terminatedOnTheWriter,
                                     5000);
            QVERIFY(!terminatedOnTheWriter);
            log.flush();
        }
        QCOMPARE(linesOf(path, "decoder"), 6);
        QCOMPARE(droppedSaidIn(path), 3);
    }

    // L5: a write that failed closes the file, and it is said closed (it was said open for ever).
    void aFileThatCannotBeWrittenIsSaidClosed()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(u"video.jsonl"_s);
        // (Its rotation cannot be done: what it would be renamed to is a folder.)
        QVERIFY(QDir().mkpath(path + u".1"_s));
        VideoDiagnostics log(path, 1024);
        QVERIFY(log.isOpen());
        for (int index = 0; index < 40; ++index) {
            VideoDiagnostics::record("decoder", {{"decoded", index}});
        }
        log.flush();
        QVERIFY(!log.isOpen());
    }

    // L6: the end waits atTheEndMs at the most for a disk that does not let go, and then leaves the writer's thread to
    // finish by itself: what waited is written once the disk lets go, and another writer can be had at once.
    void theEndWaitsForTheDiskTwoSecondsAtTheMost()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(u"video.jsonl"_s);
        const QString next = directory.filePath(u"next.jsonl"_s);
        const auto disk = std::make_shared<Held>();
        auto log = std::make_unique<VideoDiagnostics>(
            path, qint64{1024} * 1024, VideoDiagnostics::ForTests{.beforeWriting = [disk] { disk->waitWhileHeld(); }});
        QVERIFY(log->isOpen());
        for (int index = 0; index < 10; ++index) {
            VideoDiagnostics::record("decoder", {{"decoded", index}});
        }
        QElapsedTimer ending;
        ending.start();
        log.reset();
        const qint64 took = ending.elapsed();
        QVERIFY2(took >= VideoDiagnostics::atTheEndMs - 200 && took < VideoDiagnostics::atTheEndMs + 1500,
                 qPrintable(QString::number(took)));
        {
            const VideoDiagnostics another(next);
            QVERIFY(another.isOpen());
            VideoDiagnostics::record("another", {{"index", 1}});
            another.flush();
        }
        QCOMPARE(linesOf(next, "another"), 1);
        disk->letGo();
        QTRY_COMPARE_WITH_TIMEOUT(linesOf(path, "decoder"), 10, 5000);
        QCOMPARE(linesOf(path, "another"), 0);
    }

    // L3: at a crash, the lines that wait are written before the program ends, the nearest to the crash, which a crash
    // report needs most (they were flushed one by one before the writer's thread, and lost with it after): the crash
    // handler gives the writer's thread a while.
    void atACrashWhatWaitsIsWritten()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(u"video.jsonl"_s);
        const VideoDiagnostics log(path, qint64{1024} * 1024, {.beforeWriting = [] {
                                       std::this_thread::sleep_for(std::chrono::milliseconds(20));
                                   }});
        QVERIFY(log.isOpen());
        for (int index = 0; index < 30; ++index) {
            VideoDiagnostics::record("decoder", {{"decoded", index}});
        }
        VideoDiagnostics::writeWhatWaitsAtACrash();
        QCOMPARE(linesOf(path, "decoder"), 30);
    }
    // ... and waits for a disk that does not let go atACrashMs at the most ...
    void atACrashTheDiskIsWaitedForTwoSecondsAtTheMost()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(u"video.jsonl"_s);
        const auto disk = std::make_shared<Held>();
        const VideoDiagnostics log(path, qint64{1024} * 1024, {.beforeWriting = [disk] { disk->waitWhileHeld(); }});
        QVERIFY(log.isOpen());
        for (int index = 0; index < 5; ++index) {
            VideoDiagnostics::record("decoder", {{"decoded", index}});
        }
        QElapsedTimer crashing;
        crashing.start();
        VideoDiagnostics::writeWhatWaitsAtACrash();
        const qint64 took = crashing.elapsed();
        QVERIFY2(took >= VideoDiagnostics::atACrashMs - 200 && took < VideoDiagnostics::atACrashMs + 1500,
                 qPrintable(QString::number(took)));
        disk->letGo();
        log.flush();
    }
    // ... and on the writer's own thread (it is the one that crashed) waits for nothing.
    void atACrashOnTheWritersOwnThreadNothingIsWaitedFor()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(u"video.jsonl"_s);
        auto took = std::make_shared<std::atomic<qint64>>(-1);
        const VideoDiagnostics log(path, qint64{1024} * 1024, {.beforeWriting = [took] {
                                       if (*took < 0) {
                                           QElapsedTimer crashing;
                                           crashing.start();
                                           VideoDiagnostics::writeWhatWaitsAtACrash();
                                           *took = crashing.elapsed();
                                       }
                                   }});
        VideoDiagnostics::record("decoder", {{"decoded", 1}});
        VideoDiagnostics::record("decoder", {{"decoded", 2}});
        log.flush();
        QVERIFY2(*took >= 0 && *took < 500, qPrintable(QString::number(*took)));
        QCOMPARE(linesOf(path, "decoder"), 2);
    }

private:
    // A disk that is slow: the writer waits while it is held (five seconds at the most: a record that waited for it is
    // then seen to have taken that long, and not as a test that never ends). Shared with the writer's thread, which
    // may outlive the test's own scope.
    struct Held {
        void waitWhileHeld()
        {
            std::unique_lock held(lock);
            free.wait_for(held, std::chrono::seconds(5), [this] { return !holding; });
        }
        void letGo()
        {
            {
                const std::lock_guard held(lock);
                holding = false;
            }
            free.notify_all();
        }
        ~Held() { letGo(); }
        Held() = default;
        Held(const Held&) = delete;
        Held& operator=(const Held&) = delete;
        Held(Held&&) = delete;
        Held& operator=(Held&&) = delete;
        std::mutex lock;
        std::condition_variable free;
        bool holding = true;
    };
    static QList<QJsonObject> entriesOf(const QString& path)
    {
        QList<QJsonObject> entries;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            return entries;
        }
        for (const QByteArray& line : file.readAll().split('\n')) {
            if (!line.isEmpty()) {
                entries.append(QJsonDocument::fromJson(line).object());
            }
        }
        return entries;
    }
    static int linesOf(const QString& path, const char* event)
    {
        int count = 0;
        for (const QJsonObject& entry : entriesOf(path)) {
            count += entry[u"event"_s].toString() == QLatin1StringView(event) ? 1 : 0;
        }
        return count;
    }
    static int droppedSaidIn(const QString& path)
    {
        qint64 dropped = 0;
        for (const QJsonObject& entry : entriesOf(path)) {
            if (entry[u"event"_s].toString() == u"diagnostics_dropped"_s) {
                dropped += entry[u"values"_s].toObject()[u"count"_s].toInteger();
            }
        }
        return static_cast<int>(dropped);
    }
    static QList<QDateTime> timesIn(const QString& path)
    {
        QList<QDateTime> times;
        for (const QJsonObject& entry : entriesOf(path)) {
            times.append(QDateTime::fromString(entry[u"time"_s].toString(), Qt::ISODateWithMs));
        }
        return times;
    }
};
QTEST_GUILESS_MAIN(TestVideoDiagnostics)
#include "test_video_diagnostics.moc"
