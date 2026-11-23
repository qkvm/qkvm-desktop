#include <functional>
#include <memory>
#include <vector>

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include "win/profile_lock.h"
#include "win/single_instance.h"

// Windows' own headers; the first has to come first.
#include <windows.h>

#include <aclapi.h>
#include <sddl.h>

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

    // The pipe a copy of that name listens at, as the copies name it: of this Windows account (its SID), with the
    // session's number. (A version before 0.2.0 listens at the name with the session's number alone: olderPipeOf.)
    static QString pipeOf(const QString& name)
    {
        return u"%1-%2"_s.arg(ui::SingleInstance::ownName(name)).arg(session());
    }
    static QString olderPipeOf(const QString& name) { return u"%1-%2"_s.arg(name).arg(session()); }
    static DWORD session()
    {
        DWORD session = 0;
        ProcessIdToSessionId(GetCurrentProcessId(), &session);
        return session;
    }

    // This Windows account's SID, as text.
    static QString ownSid()
    {
        HANDLE token = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
            return {};
        }
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        std::vector<BYTE> user(size);
        QString said;
        LPWSTR text = nullptr;
        if (size != 0 && GetTokenInformation(token, TokenUser, user.data(), size, &size) != FALSE &&
            ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &text) != FALSE) {
            said = QString::fromWCharArray(text);
            LocalFree(text);
        }
        CloseHandle(token);
        return said;
    }

    // A mutex of this name as another account's is to this one: a list that lets nobody at it. Its handle.
    static HANDLE madeByAnotherAccount(const QString& mutex)
    {
        PSECURITY_DESCRIPTOR nobody = nullptr;
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:", SDDL_REVISION_1, &nobody, nullptr) == FALSE) {
            return nullptr;
        }
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), nobody, FALSE};
        const std::wstring named = mutex.toStdWString();
        const HANDLE made = CreateMutexW(&attributes, FALSE, named.c_str());
        LocalFree(nobody);
        return made;
    }

    // The entries of the list a mutex of this name has: each one's SID, as text (none where it cannot be read).
    static QStringList whoMayUse(const QString& mutex)
    {
        const std::wstring named = mutex.toStdWString();
        const HANDLE opened = OpenMutexW(READ_CONTROL, FALSE, named.c_str());
        if (opened == nullptr) {
            return {u"(not opened)"_s};
        }
        PACL list = nullptr;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        QStringList who;
        if (GetSecurityInfo(opened, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &list, nullptr,
                            &descriptor) == ERROR_SUCCESS &&
            list != nullptr) {
            for (DWORD index = 0; index < list->AceCount; ++index) {
                void* entry = nullptr;
                if (GetAce(list, index, &entry) == FALSE) {
                    continue;
                }
                auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(entry);
                LPWSTR text = nullptr;
                if (allowed->Header.AceType == ACCESS_ALLOWED_ACE_TYPE &&
                    ConvertSidToStringSidW(&allowed->SidStart, &text) != FALSE) {
                    who.append(QString::fromWCharArray(text));
                    LocalFree(text);
                } else {
                    who.append(u"(another kind of entry)"_s);
                }
            }
        }
        LocalFree(descriptor);
        CloseHandle(opened);
        return who;
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

    // A copy that Windows does not let at this account's own mutex (one made under its name by somebody who lets nobody
    // else at it) can tell nothing of the others: it is never taken for the first, so that two copies never write one
    // profile; and it neither listens nor tells (outside review of 0.1.2, C3).
    void aCopyThatCannotOpenItsAccountsMutexIsNeverTheFirst()
    {
        const QString name = aName();
        const HANDLE theirs = madeByAnotherAccount(u"Local\\"_s + ui::SingleInstance::ownName(name));
        QVERIFY(theirs != nullptr);
        const ui::SingleInstance mine(name);
        const bool first = mine.isFirst();
        const bool listened = say(name, "show\n");
        CloseHandle(theirs);
        QVERIFY2(!first, "a copy that could not open the mutex took itself for the first");
        QVERIFY(!listened);
        QVERIFY(mine.cannotTell());
        QVERIFY2(mine.whyNot().contains(u"Windows did not let it see"_s), qPrintable(mine.whyNot()));
        QVERIFY(mine.whyNot().contains(QString::number(ERROR_ACCESS_DENIED)));
        // (And a copy that can tell is never one that cannot.)
        const ui::SingleInstance alone(aName());
        QVERIFY(alone.isFirst());
        QVERIFY(!alone.cannotTell());
        QCOMPARE(alone.whyNot(), QString());
    }

    // One copy for each Windows account (the outside review of 0.1.2, C3, closed in 0.2.0): another account's copy in
    // this session (Run as different user) holds the name that Setup and qKVM 0.1.0 to 0.1.2 look for, and lets nobody
    // else at it; this account's copy is the first of its own all the same, listens, and is the one its later copies
    // find. Its profile is its own.
    //   PASS: the first of its own.  FAIL (0.1.2): it cannot tell, and ends.
    void anotherAccountsCopyKeepsNobodyOutOfTheirOwn()
    {
        const QString name = aName();
        const HANDLE theirs = madeByAnotherAccount(u"Local\\"_s + name);
        QVERIFY(theirs != nullptr);
        const auto mine = std::make_unique<ui::SingleInstance>(name);
        const bool first = mine->isFirst();
        const bool cannotTell = mine->cannotTell();
        const QSignalSpy asked(mine.get(), &ui::SingleInstance::askedToShow);
        const bool laterIsFirst = startedAgainIsFirst(name);
        CloseHandle(theirs);
        QVERIFY2(first && !cannotTell, qPrintable(mine->whyNot()));
        QVERIFY(!laterIsFirst);
        QTRY_COMPARE(asked.count(), 1);
    }

    // The name Setup and qKVM 0.1.0 to 0.1.2 look for ("Local\" and the name: Setup's AppMutex, its uninstaller) is
    // there while a copy runs, as before; and this account's own beside it. Each lets this Windows account at it, and
    // nobody else.
    //   PASS: both there, of this account alone.  FAIL (0.1.2): no name of the account's; the list is Windows' own.
    void theNamesAreThereForSetupAndOnlyThisAccountMayUseThem()
    {
        const QString name = aName();
        const QString sid = ownSid();
        QVERIFY(!sid.isEmpty());
        QVERIFY(ui::SingleInstance::ownName(name).endsWith(u'-' + sid));
        auto first = std::make_unique<ui::SingleInstance>(name);
        QVERIFY(first->isFirst());
        for (const QString& mutex : {u"Local\\"_s + name, u"Local\\"_s + ui::SingleInstance::ownName(name)}) {
            const std::wstring named = mutex.toStdWString();
            // (As Setup looks: OpenMutex with SYNCHRONIZE.)
            const HANDLE seen = OpenMutexW(SYNCHRONIZE, FALSE, named.c_str());
            QVERIFY2(seen != nullptr, qPrintable(mutex));
            CloseHandle(seen);
            QCOMPARE(whoMayUse(mutex), QStringList{sid});
        }
        // And once it has ended, neither is there.
        first.reset();
        for (const QString& mutex : {u"Local\\"_s + name, u"Local\\"_s + ui::SingleInstance::ownName(name)}) {
            const std::wstring named = mutex.toStdWString();
            const HANDLE seen = OpenMutexW(SYNCHRONIZE, FALSE, named.c_str());
            QVERIFY2(seen == nullptr, qPrintable(mutex));
        }
    }

    // A copy of a version before 0.2.0 of this account runs in this session: it holds the name all versions make, and
    // none of the account's. A copy of this version is not the first then (two copies would write one profile), and
    // tells it to show itself at its own pipe, as that version listens.
    //   PASS: not the first; the older copy told.  FAIL: the first, beside the older copy.
    void anOlderCopyOfThisAccountKeepsThisOneFromBeingTheFirst()
    {
        const QString name = aName();
        // (The older copy, as 0.1.2 makes it: the name with Windows' own list, and its pipe.)
        const std::wstring shared = (u"Local\\"_s + name).toStdWString();
        const HANDLE older = CreateMutexW(nullptr, FALSE, shared.c_str());
        QVERIFY(older != nullptr);
        QLocalServer olderPipe;
        olderPipe.setSocketOptions(QLocalServer::UserAccessOption);
        QVERIFY(olderPipe.listen(olderPipeOf(name)));
        QByteArray heard;
        connect(&olderPipe, &QLocalServer::newConnection, this, [&olderPipe, &heard] {
            while (QLocalSocket* caller = olderPipe.nextPendingConnection()) {
                connect(caller, &QLocalSocket::readyRead, caller, [caller, &heard] { heard += caller->readAll(); });
                connect(caller, &QLocalSocket::disconnected, caller, &QObject::deleteLater);
            }
        });
        const bool first = startedAgainIsFirst(name);
        QTRY_COMPARE(heard, QByteArray("show\n"));
        CloseHandle(older);
        QVERIFY2(!first, "a copy took itself for the first while an older copy of its account ran");
    }

    // A later copy tells only a first copy that Windows says runs as this user, and lets only that one come to the
    // front: the server of a pipe this process made is this process; no pipe, no server.
    void theFirstCopyToldIsOneOfThisUser()
    {
        QLocalServer listening;
        listening.setSocketOptions(QLocalServer::UserAccessOption);
        QVERIFY(listening.listen(pipeOf(aName())));
        QLocalSocket caller;
        caller.connectToServer(listening.fullServerName());
        QVERIFY(caller.waitForConnected(2000));
        QCOMPARE(ui::SingleInstance::serverOfThisUser(reinterpret_cast<void*>(caller.socketDescriptor())),
                 static_cast<unsigned long>(GetCurrentProcessId()));
        caller.abort();
        QCOMPARE(ui::SingleInstance::serverOfThisUser(nullptr), 0UL);
        QCOMPARE(ui::SingleInstance::serverOfThisUser(INVALID_HANDLE_VALUE), 0UL);
        // And a pipe another user serves: one of Windows' own, which every Windows has and any user may open (nothing
        // is said over it). Its server is a process, but not this user's: none.
        //   PASS: none.  FAIL: a pipe of another user's taken for the first copy's.
        bool reached = false;
        for (const QString& windows : {u"epmapper"_s, u"InitShutdown"_s, u"ntsvcs"_s}) {
            QLocalSocket theirs;
            theirs.connectToServer(windows);
            if (!theirs.waitForConnected(2000)) {
                continue;
            }
            reached = true;
            ULONG server = 0;
            QVERIFY(GetNamedPipeServerProcessId(reinterpret_cast<HANDLE>(theirs.socketDescriptor()), &server) != FALSE);
            QVERIFY(server != 0 && server != GetCurrentProcessId());
            QCOMPARE(ui::SingleInstance::serverOfThisUser(reinterpret_cast<void*>(theirs.socketDescriptor())), 0UL);
            theirs.abort();
            break;
        }
        QVERIFY2(reached, "none of Windows' own pipes could be opened");
    }

    // The lock of the profile: one copy of this account writes it, from whichever Windows session it runs in (the same
    // account signed in at the PC and by Remote Desktop has one profile, and a mutex of a session is that session's
    // alone: outside review of 0.1.2, C3). Taken by the first; refused to a second, which says why; taken again once
    // the first has let it go, as Windows lets it go of a program that ends, however it ends.
    //   PASS: one at a time.  FAIL (0.1.2): no lock, two copies in two sessions.
    void oneCopyOfAnAccountWritesItsProfileWhateverSessionItRunsIn()
    {
        const QTemporaryDir home;
        const QString file = home.filePath(u"QKVM Desktop/settings.json.running"_s);
        auto first = std::make_unique<ui::ProfileLock>(file);
        QVERIFY2(first->taken(), qPrintable(first->whyNot()));
        QCOMPARE(first->whyNot(), QString());
        QVERIFY(QFile::exists(file));
        // (Another copy is another program, with a handle of its own: here, another thread.)
        bool secondTook = true;
        bool secondHeld = false;
        QString secondSaid;
        QVERIFY(elsewhere([&] {
            const ui::ProfileLock second(file);
            secondTook = second.taken();
            secondHeld = second.heldElsewhere();
            secondSaid = second.whyNot();
        }));
        QVERIFY2(!secondTook, "a second copy took the lock of the profile");
        QVERIFY(secondHeld);
        QVERIFY2(secondSaid.contains(u"another Windows session"_s), qPrintable(secondSaid));
        first.reset();
        const ui::ProfileLock next(file);
        QVERIFY2(next.taken(), qPrintable(next.whyNot()));
    }

    // A lock that cannot be taken for another reason (its folder cannot be made, its file cannot be opened) is no
    // lock: the copy cannot tell whether another writes the profile, and says so with Windows' number.
    void aLockThatCannotBeTakenSaysWhy()
    {
        const QTemporaryDir home;
        // (A folder where the file should be: Windows opens no file there.)
        const QString file = home.filePath(u"settings.json.running"_s);
        QVERIFY(QDir().mkpath(file));
        const ui::ProfileLock none(file);
        QVERIFY(!none.taken());
        QVERIFY(!none.heldElsewhere());
        QVERIFY2(none.whyNot().contains(u"could not see whether"_s), qPrintable(none.whyNot()));
        QVERIFY(none.whyNot().contains(QString::number(ERROR_ACCESS_DENIED)));
    }

    // The lock holds nothing of the person's, and changes nothing beside it: the settings stay byte for byte.
    void theLockChangesNothingOfTheProfile()
    {
        const QTemporaryDir home;
        QVERIFY(QDir().mkpath(home.filePath(u"QKVM Desktop"_s)));
        const QString settings = home.filePath(u"QKVM Desktop/settings.json"_s);
        QFile written(settings);
        QVERIFY(written.open(QIODevice::WriteOnly) && written.write(R"({"theme": "light"})") > 0);
        written.close();
        {
            const ui::ProfileLock lock(settings + u".running"_s);
            QVERIFY(lock.taken());
        }
        QFile read(settings);
        QVERIFY(read.open(QIODevice::ReadOnly));
        QCOMPARE(read.readAll(), QByteArray(R"({"theme": "light"})"));
        QFile lockFile(settings + u".running"_s);
        QVERIFY(lockFile.open(QIODevice::ReadOnly));
        QCOMPARE(lockFile.size(), 0);
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
