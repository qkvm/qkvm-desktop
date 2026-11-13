#include <QFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <qt_windows.h>

#include "fixture_phase.h"

class TestFixturePhase : public QObject {
    Q_OBJECT
private slots:
    void onlyABoundedJsonObjectIsAReply_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        QTest::addColumn<bool>("empty");
        QTest::addColumn<bool>("ok");
        QTest::newRow("success") << QByteArray("{\"ok\":true}") << false << true;
        QTest::newRow("explicit refusal") << QByteArray("{\"ok\":false}") << false << false;
        QTest::newRow("empty") << QByteArray() << true << false;
        QTest::newRow("unfinished JSON") << QByteArray("{\"ok\":") << true << false;
        QTest::newRow("wrong shape") << QByteArray("[true]") << true << false;
        QTest::newRow("oversized") << QByteArray(4097, ' ') << true << false;
    }

    void onlyABoundedJsonObjectIsAReply()
    {
        QFETCH(QByteArray, bytes);
        QFETCH(bool, empty);
        QFETCH(bool, ok);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QDir directory(temporary.path());
        QFile file(directory.filePath(u"reply.json"_s));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(bytes), bytes.size());
        file.close();
        const auto result = fixturePhase(directory, u"reply"_s);
        QCOMPARE(result.isEmpty(), empty);
        QCOMPARE(result.value(u"ok"_s).toBool(), ok);
    }

    void anAbsentReplyStillEndsAtTheDeadline()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QElapsedTimer clock;
        clock.start();
        const auto result = fixturePhase(QDir(temporary.path()), u"missing"_s, 50);
        QVERIFY(result.isEmpty());
        QVERIFY(clock.hasExpired(49));
    }

    void aPublishedReplyMayBeTemporarilyLockedByAnotherReader()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QDir directory(temporary.path());
        const QString path = directory.filePath(u"reply.json"_s);
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("{\"ok\":true}"), 11);
        file.close();
        HANDLE locked = CreateFileW(QDir::toNativeSeparators(path).toStdWString().c_str(), GENERIC_READ, 0, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(locked != INVALID_HANDLE_VALUE);
        const auto unlock = qScopeGuard([&] {
            if (locked != INVALID_HANDLE_VALUE)
                CloseHandle(locked);
        });
        QObject timerOwner;
        bool released = false;
        QTimer::singleShot(200, &timerOwner, [&] {
            CloseHandle(locked);
            locked = INVALID_HANDLE_VALUE;
            released = true;
        });
        const auto result = fixturePhase(directory, u"reply"_s);
        QVERIFY(result.value(u"ok"_s).toBool());
        QVERIFY(released);
        QVERIFY(QFile::exists(directory.filePath(u"reply.request"_s)));
    }
};

QTEST_GUILESS_MAIN(TestFixturePhase)
#include "test_fixture_phase.moc"
