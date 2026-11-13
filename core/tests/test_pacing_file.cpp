#include <memory>
#include <optional>

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <qkvm/core/pacing_file.h>
#include <qkvm/core/sign_in_pacing.h>

#include "memory_secrets.h"

using namespace Qt::StringLiterals;
using qkvm::core::PacingFile;
using qkvm::core::SignInPacing;
using qkvm::core::testing::MemorySecrets;

namespace {

const QUrl kvm(u"https://192.0.2.20"_s);
const QUrl other(u"http://192.0.2.21"_s);
const SignInPacing::Steps steps; // a minute after a no, ten seconds after one that came to nothing

// One run of the program: its pacing on a clock of the test's own, and the file kept beside the settings, on a wall
// clock of the test's own as well (`wallMs`, shared by the runs: the PC's clock, which goes on across a restart).
struct Run {
    Run(const QString& file, MemorySecrets& secrets, qint64& wallMs, qint64 startsAt = 5'000)
        : steadyMs(startsAt), pacing(SignInPacing::mostKept, [this] { return steadyMs; }),
          kept(pacing, file, secrets, pace(wallMs))
    {}
    static PacingFile::Pace pace(qint64& wallMs)
    {
        PacingFile::Pace pace;
        pace.writtenAfterMs = 20;
        pace.wallMs = [&wallMs] { return wallMs; };
        return pace;
    }
    qint64 steadyMs;
    SignInPacing pacing;
    PacingFile kept;
};

QByteArray bytesOf(const QString& file)
{
    QFile read(file);
    return read.open(QIODevice::ReadOnly) ? read.readAll() : QByteArray();
}

} // namespace

class TestPacingFile : public QObject {
    Q_OBJECT

private slots:
    void aHoldAndItsCountsSurviveARestart()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json.sign-ins"_s);
        MemorySecrets secrets;
        qint64 wallMs = 1'750'000'000'000;
        const SignInPacing::Mark refused = SignInPacing::what(u"login", kvm, u"admin"_s, u"wrong"_s);
        const SignInPacing::Mark silent = SignInPacing::what(u"login", other, QString(), u"secret"_s);
        {
            Run first(file, secrets, wallMs);
            first.kept.read();
            QVERIFY(first.kept.keeps());
            QCOMPARE(first.pacing.saidNo(refused, steps), 60'000);
            QCOMPARE(first.pacing.unanswered(silent, steps), 10'000);
            first.steadyMs += 4'000;
            wallMs += 4'000;
            QVERIFY(first.kept.waitForWritten(5000));
        }
        // The program ends, and starts again 6 s later: its steady clock begins anew elsewhere, the PC's goes on.
        wallMs += 6'000;
        Run second(file, secrets, wallMs, 70);
        second.kept.read();
        QVERIFY(second.kept.putAside().isEmpty());
        QCOMPARE(second.pacing.waitLeftMs(refused), 50'000);
        QCOMPARE(second.pacing.times(refused), 1);
        QCOMPARE(second.pacing.waitLeftMs(silent), 0);
        QCOMPARE(second.pacing.silent(silent), 1);
        // Its count goes on: the next no in a row waits five minutes, not one.
        second.steadyMs += 50'000;
        QCOMPARE(second.pacing.saidNo(refused, steps), 300'000);
        // (What was read back is never a sign-in out: none of the run before is there to end it.)
        QVERIFY(!second.pacing.onlyOut(refused));
    }

    void aHoldReadBackIsNeverLongerThanTheLongest()
    {
        // The PC's clock set back an hour and more since it was written: the hold is the longest there is, no more.
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json.sign-ins"_s);
        MemorySecrets secrets;
        qint64 wallMs = 1'750'000'000'000;
        const SignInPacing::Mark refused = SignInPacing::what(u"login", kvm, u"admin"_s, u"wrong"_s);
        {
            Run first(file, secrets, wallMs);
            first.kept.read();
            first.pacing.saidNo(refused, steps);
            QVERIFY(first.kept.waitForWritten(5000));
        }
        wallMs -= 10'000'000;
        Run second(file, secrets, wallMs);
        second.kept.read();
        QCOMPARE(second.pacing.waitLeftMs(refused), 3'600'000);
    }

    void theFileHoldsNoPasswordAndNothingToTryOneAgainst()
    {
        // No password, and not the digest it is known by either, which is a fast hash of what a password is tried
        // against: the file is sealed under a key kept in the store of the program's secrets.
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json.sign-ins"_s);
        MemorySecrets secrets;
        qint64 wallMs = 1'750'000'000'000;
        const QString password = u"Correct-Horse-7"_s;
        const SignInPacing::Mark refused = SignInPacing::what(u"login", kvm, u"operator"_s, password);
        Run run(file, secrets, wallMs);
        run.kept.read();
        run.pacing.saidNo(refused, steps);
        QVERIFY(run.kept.waitForWritten(5000));
        const QByteArray written = bytesOf(file);
        QVERIFY(!written.isEmpty());
        for (const QByteArray& never :
             {password.toUtf8(), QByteArray(reinterpret_cast<const char*>(password.utf16()), password.size() * 2),
              refused.digest, refused.digest.toHex(), refused.digest.toBase64(), "192.0.2.20"_ba, "operator"_ba}) {
            QVERIFY2(!written.contains(never), never.constData());
        }
        // Its version outside, and the key in the store, of its own size, under its own name.
        QCOMPARE(QJsonDocument::fromJson(written).object().value("version"_L1).toInt(), PacingFile::version);
        QCOMPARE(secrets.all.value(QString::fromLatin1(PacingFile::keyName)).size(), 32);
        QCOMPARE(secrets.all.size(), 1);
    }

    void aRemovedKvmsEntriesGo()
    {
        // A KVM taken off the wall: what was kept of its sign-ins goes, whoever signed in and with whatever password,
        // in memory and at the next writing; but a sign-in to it that is out still, which its line's end settles.
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json.sign-ins"_s);
        MemorySecrets secrets;
        qint64 wallMs = 1'750'000'000'000;
        const SignInPacing::Mark one = SignInPacing::what(u"login", kvm, u"admin"_s, u"wrong"_s);
        const SignInPacing::Mark another = SignInPacing::what(u"session", kvm, u"root"_s, u"older"_s);
        const SignInPacing::Mark out = SignInPacing::what(u"web", kvm, u"admin"_s, u"secret"_s);
        const SignInPacing::Mark elsewhere = SignInPacing::what(u"login", other, QString(), u"wrong"_s);
        {
            Run first(file, secrets, wallMs);
            first.kept.read();
            first.pacing.saidNo(one, steps);
            first.pacing.saidNo(another, steps);
            first.pacing.saidNo(elsewhere, steps);
            static_cast<void>(first.pacing.sent(out, steps));
            first.pacing.forgetPlace(kvm);
            QVERIFY(!first.pacing.standing(one));
            QVERIFY(!first.pacing.standing(another));
            QVERIFY(first.pacing.standing(out));
            QCOMPARE(first.pacing.times(elsewhere), 1);
            first.pacing.accepted(out);
            QVERIFY(first.kept.waitForWritten(5000));
        }
        Run second(file, secrets, wallMs);
        second.kept.read();
        QVERIFY(!second.pacing.standing(one));
        QVERIFY(!second.pacing.standing(another));
        QCOMPARE(second.pacing.times(elsewhere), 1);
        QCOMPARE(second.pacing.kept(), 1);
    }

    void aFileThatCannotBeReadIsPutAsideAndNothingIsHeld_data()
    {
        QTest::addColumn<int>("how"); // 0: not JSON; 1: another version; 2: sealed under another key; 3: cut short;
                                      // 4: its key gone from the store
        QTest::newRow("not JSON") << 0;
        QTest::newRow("another version") << 1;
        QTest::newRow("sealed under another key") << 2;
        QTest::newRow("cut short") << 3;
        QTest::newRow("its key gone") << 4;
    }

    void aFileThatCannotBeReadIsPutAsideAndNothingIsHeld()
    {
        QFETCH(int, how);
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json.sign-ins"_s);
        MemorySecrets secrets;
        qint64 wallMs = 1'750'000'000'000;
        const SignInPacing::Mark refused = SignInPacing::what(u"login", kvm, u"admin"_s, u"wrong"_s);
        {
            Run first(file, secrets, wallMs);
            first.kept.read();
            first.pacing.saidNo(refused, steps);
            QVERIFY(first.kept.waitForWritten(5000));
        }
        QByteArray written = bytesOf(file);
        QJsonObject top = QJsonDocument::fromJson(written).object();
        if (how == 0) {
            written = "{\"version\":1,\"nonce\":"_ba;
        } else if (how == 1) {
            top.insert(u"version"_s, 2);
            written = QJsonDocument(top).toJson();
        } else if (how == 2) {
            secrets.all.insert(QString::fromLatin1(PacingFile::keyName), QByteArray(32, 'k'));
        } else if (how == 3) {
            top.insert(u"sealed"_s, top.value("sealed"_L1).toString().left(20));
            written = QJsonDocument(top).toJson();
        } else {
            secrets.all.remove(QString::fromLatin1(PacingFile::keyName));
        }
        if (how != 2 && how != 4) {
            QFile damaged(file);
            QVERIFY(damaged.open(QIODevice::WriteOnly | QIODevice::Truncate));
            damaged.write(written);
        }
        const QByteArray asItWas = bytesOf(file);
        Run second(file, secrets, wallMs);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"could not be read .* put aside as .* nothing held"_s));
        second.kept.read();
        QCOMPARE(second.pacing.kept(), 0);
        QCOMPARE(second.pacing.waitLeftMs(refused), 0);
        QCOMPARE(second.kept.putAside(), file + u".unreadable"_s);
        QCOMPARE(bytesOf(second.kept.putAside()), asItWas);
        QVERIFY(!QFile::exists(file));
        // And from then on it is kept anew, and read back at the next start.
        QVERIFY(second.kept.keeps());
        second.pacing.saidNo(refused, steps);
        QVERIFY(second.kept.waitForWritten(5000));
        Run third(file, secrets, wallMs);
        third.kept.read();
        QCOMPARE(third.pacing.times(refused), 1);
    }

    void itIsWrittenOffTheInterfaceThreadOneWriteForWhatChangesMeanwhile()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json.sign-ins"_s);
        MemorySecrets secrets;
        qint64 wallMs = 1'750'000'000'000;
        Run run(file, secrets, wallMs);
        run.kept.read();
        for (int each = 0; each < 50; ++each) {
            run.pacing.saidNo(SignInPacing::what(u"login", kvm, u"admin"_s, QString::number(each)), steps);
        }
        QVERIFY(!QFile::exists(file)); // (not written from within the change: a while after it)
        QVERIFY(run.kept.waitForWritten(5000));
        QCOMPARE(run.kept.writings(), 1);
        QVERIFY(run.kept.lastWrittenOn() != nullptr);
        QVERIFY(run.kept.lastWrittenOn() != QThread::currentThread());
        QVERIFY(QFile::exists(file));
    }

    void whatIsStillToWriteIsWrittenAsItGoesWithNoLoopRun()
    {
        // The merged review's LOW 8 (2026-11-06): at the program's end the file goes after the loop has ended and the
        // wall has gone (main.cpp). What was still to write is written then, and waited for, without running the loop:
        // what was handed to it meanwhile is not run from inside the going (rule 6).
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json.sign-ins"_s);
        MemorySecrets secrets;
        qint64 wallMs = 1'750'000'000'000;
        const SignInPacing::Mark refused = SignInPacing::what(u"login", kvm, u"admin"_s, u"wrong"_s);
        bool ranInside = false;
        {
            Run first(file, secrets, wallMs);
            first.kept.read();
            first.pacing.saidNo(refused, steps); // (still to write: a while after the change)
            QMetaObject::invokeMethod(
                QCoreApplication::instance(), [&ranInside] { ranInside = true; }, Qt::QueuedConnection);
        }
        QVERIFY(!ranInside);
        QCoreApplication::processEvents();
        QVERIFY(ranInside);
        Run second(file, secrets, wallMs);
        second.kept.read();
        QCOMPARE(second.pacing.times(refused), 1);
    }

    void aWritingUnderWayIsWaitedForAsItGoesAndWhatCameAfterItIsWrittenToo()
    {
        // (As above, with a writing on its way off the interface thread when it goes, and a change after that.)
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json.sign-ins"_s);
        MemorySecrets secrets;
        qint64 wallMs = 1'750'000'000'000;
        const SignInPacing::Mark first = SignInPacing::what(u"login", kvm, u"admin"_s, u"wrong"_s);
        const SignInPacing::Mark second = SignInPacing::what(u"login", other, QString(), u"wrong"_s);
        bool ranInside = false;
        {
            Run run(file, secrets, wallMs);
            run.kept.read();
            run.pacing.saidNo(first, steps);
            // Its while over: the writing goes to the pool, and nothing of the loop is run after it.
            QTRY_VERIFY(run.kept.isWriting() || run.kept.writings() > 0);
            run.pacing.saidNo(second, steps);
            QMetaObject::invokeMethod(
                QCoreApplication::instance(), [&ranInside] { ranInside = true; }, Qt::QueuedConnection);
        }
        QVERIFY(!ranInside);
        QCoreApplication::processEvents();
        Run again(file, secrets, wallMs);
        again.kept.read();
        QCOMPARE(again.pacing.times(first), 1);
        QCOMPARE(again.pacing.times(second), 1);
    }

    void whereTheStoreHandsOverNoKeyNothingIsReadOrWritten()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json.sign-ins"_s);
        MemorySecrets secrets;
        secrets.refusesReads = true;
        qint64 wallMs = 1'750'000'000'000;
        Run run(file, secrets, wallMs);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"was not handed over"_s));
        run.kept.read();
        QVERIFY(!run.kept.keeps());
        run.pacing.saidNo(SignInPacing::what(u"login", kvm, u"admin"_s, u"wrong"_s), steps);
        QVERIFY(run.kept.waitForWritten(1000));
        QVERIFY(!QFile::exists(file));
        QCOMPARE(run.kept.writings(), 0);
    }
};

QTEST_GUILESS_MAIN(TestPacingFile)
#include "test_pacing_file.moc"
