#include <functional>
#include <memory>

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QTest>
#include <QThread>

#include "win/single_instance.h"

// Windows' own header.
#include <windows.h>

using namespace Qt::StringLiterals;
using namespace qkvm;

// One running copy at a time: with Windows' own mutexes and pipes, under names of the test's own
// that no copy of the program has.
//
// The first copy is the test's own thread, which goes on listening as a running program does. A
// later copy is another program, and here a thread of its own: it waits for the first copy to take
// what it says, and a pipe hands nothing over until the other end reads.
class TestSingleInstance : public QObject {
    Q_OBJECT

private:
    int m_made = 0;

    // A name no other test and no other run of this one has.
    QString aName() { return u"QKVM Desktop test %1 %2"_s.arg(QCoreApplication::applicationPid()).arg(++m_made); }

    // The pipe a copy of that name listens at, as the copies name it: with the session's number.
    static QString pipeOf(const QString& name)
    {
        DWORD session = 0;
        ProcessIdToSessionId(GetCurrentProcessId(), &session);
        return u"%1-%2"_s.arg(name).arg(session);
    }

    // Does this as another program would, while this thread goes on with its own work. False
    // where it did not come to an end.
    static bool elsewhere(const std::function<void()>& work)
    {
        const std::unique_ptr<QThread> other(QThread::create(work));
        other->start();
        for (QDeadlineTimer limit(20'000); !other->isFinished() && !limit.hasExpired();) {
            QTest::qWait(5);
        }
        return other->wait(1000);
    }

    // The program is started again under this name. Whether that copy is the first.
    static bool startedAgainIsFirst(const QString& name)
    {
        bool first = true;
        const bool ended = elsewhere([&] {
            const ui::SingleInstance later(name);
            first = later.isFirst();
        });
        return ended && first;
    }

    // Somebody says this to whoever listens under the name, and hangs up. Whether anybody listened.
    static bool say(const QString& name, const QByteArray& what)
    {
        bool reached = false;
        const bool ended = elsewhere([&] {
            QLocalSocket caller;
            caller.connectToServer(pipeOf(name));
            reached = caller.waitForConnected(2000);
            if (reached && !what.isEmpty()) {
                caller.write(what);
                caller.waitForBytesWritten(2000);
            }
            caller.abort();
        });
        return ended && reached;
    }

private slots:
    void theFirstCopyIsTheFirstAndALaterOneIsNot()
    {
        const QString name = aName();
        const ui::SingleInstance first(name);
        QVERIFY(first.isFirst());
        const QSignalSpy asked(&first, &ui::SingleInstance::askedToShow);

        // Started again: not the first, and the first is told to show itself.
        QVERIFY(!startedAgainIsFirst(name));
        QTRY_COMPARE(asked.count(), 1);
        // And again, each time.
        QVERIFY(!startedAgainIsFirst(name));
        QTRY_COMPARE(asked.count(), 2);
        QVERIFY(first.isFirst());
    }

    void copiesOfAnotherNameHaveNothingToDoWithEachOther()
    {
        const ui::SingleInstance one(aName());
        const ui::SingleInstance other(aName());
        QVERIFY(one.isFirst());
        QVERIFY(other.isFirst());
        const QSignalSpy asked(&one, &ui::SingleInstance::askedToShow);
        QVERIFY(startedAgainIsFirst(aName()));
        QTest::qWait(200);
        QCOMPARE(asked.count(), 0);
    }

    void whenTheFirstCopyHasEndedTheNextOneIsTheFirst()
    {
        const QString name = aName();
        auto first = std::make_unique<ui::SingleInstance>(name);
        QVERIFY(first->isFirst());
        first.reset();
        const ui::SingleInstance next(name);
        QVERIFY(next.isFirst());
        // And it listens, as the first did.
        const QSignalSpy asked(&next, &ui::SingleInstance::askedToShow);
        QVERIFY(!startedAgainIsFirst(name));
        QTRY_COMPARE(asked.count(), 1);
    }

    void onlyTheOneLineThereIsMakesTheFirstCopyShowItself_data()
    {
        QTest::addColumn<QByteArray>("said");
        QTest::addColumn<int>("shown");
        QTest::newRow("the line") << QByteArray("show\n") << 1;
        QTest::newRow("nothing") << QByteArray() << 0;
        QTest::newRow("the line without its end") << QByteArray("show") << 0;
        QTest::newRow("another word") << QByteArray("quit\n") << 0;
        QTest::newRow("the word with more behind it") << QByteArray("show me\n") << 0;
        QTest::newRow("in capitals") << QByteArray("SHOW\n") << 0;
        QTest::newRow("the line twice") << QByteArray("show\nshow\n") << 2;
        QTest::newRow("the line behind another") << QByteArray("quit\nshow\n") << 1;
        QTest::newRow("a great deal") << QByteArray(100'000, 'x') << 0;
        QTest::newRow("a great deal that ends in the line") << QByteArray(100'000, 'x') + "\nshow\n" << 0;
    }

    void onlyTheOneLineThereIsMakesTheFirstCopyShowItself()
    {
        QFETCH(QByteArray, said);
        QFETCH(int, shown);
        const QString name = aName();
        const ui::SingleInstance first(name);
        const QSignalSpy asked(&first, &ui::SingleInstance::askedToShow);
        QVERIFY(say(name, said));
        // Whatever was said: the first copy goes on listening, and the next one that asks is heard.
        QTest::qWait(150);
        QCOMPARE(asked.count(), shown);
        QVERIFY(say(name, "show\n"));
        QTRY_COMPARE(asked.count(), shown + 1);
    }

    void whoeverSaysTooMuchIsHungUpOn()
    {
        const QString name = aName();
        const ui::SingleInstance first(name);
        bool hungUpOn = false;
        QVERIFY(elsewhere([&] {
            QLocalSocket caller;
            caller.connectToServer(pipeOf(name));
            if (!caller.waitForConnected(2000)) {
                return;
            }
            // Never a whole line, and more than a copy of the program ever says.
            caller.write(QByteArray(100'000, 'x'));
            hungUpOn = caller.waitForDisconnected(3000);
        }));
        QVERIFY(hungUpOn);
    }

    void aCallerThatSaysNothingAndStaysDoesNotKeepOthersOut()
    {
        const QString name = aName();
        const ui::SingleInstance first(name);
        const QSignalSpy asked(&first, &ui::SingleInstance::askedToShow);
        QLocalSocket silent;
        silent.connectToServer(pipeOf(name));
        QVERIFY(silent.waitForConnected(1000));
        QVERIFY(!startedAgainIsFirst(name));
        QTRY_COMPARE(asked.count(), 1);
    }
};

QTEST_GUILESS_MAIN(TestSingleInstance)
#include "test_single_instance.moc"
