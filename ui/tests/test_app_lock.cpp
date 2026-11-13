#include <QCryptographicHash>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "app_lock.h"
#include "memory_secrets.h"
#include "own_actions_view.h"
#include "own_wall.h"
#include "server_session.h"
#include <qkvm/core/pacing_file.h>
#include <qkvm/core/settings.h>
#include <qkvm/core/sign_in_pacing.h>

using namespace Qt::StringLiterals;
using qkvm::core::testing::MemorySecrets;
using qkvm::ui::AppLock;

namespace {
class WindowsSpellingSecrets final : public qkvm::core::SecretStore {
public:
    MemorySecrets store;
    qkvm::base::Result<void> keep(const QString& name, const QByteArray& secret) override
    {
        return store.keep(name.toCaseFolded(), secret);
    }
    QByteArray kept(const QString& name) override { return store.kept(name.toCaseFolded()); }
    void forget(const QString& name) override { store.forget(name.toCaseFolded()); }
    qkvm::base::Result<void> erase(const QString& name) override { return store.erase(name.toCaseFolded()); }
    qkvm::base::Result<QStringList> names() override
    {
        auto result = store.names();
        if (result && upper)
            for (auto& name : result.value())
                name = name.toUpper();
        return result;
    }
    bool upper = false;
};
const QString password = u"example-password"_s;
qkvm::base::Result<QByteArray> fast(QByteArrayView text, QByteArrayView salt, const std::stop_token&)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(salt);
    hash.addData(text);
    return hash.result();
}
void setUp(AppLock& lock, bool automatic = true)
{
    lock.submit({}, password, password, automatic);
    QTRY_VERIFY(!lock.busy());
    QVERIFY2(!lock.restricted(), qPrintable(lock.problem()));
}
} // namespace

class TestAppLock : public QObject {
    Q_OBJECT
private slots:
    void automaticSignInAcceptsWindowsStoredSpelling()
    {
        WindowsSpellingSecrets store;
        AppLock owner(store, fast, [] { return 1000; });
        setUp(owner);
        store.upper = true;
        AppLock next(store, fast, [] { return 1000; });
        QCOMPARE(next.mode(), QString());
        QVERIFY(!next.restricted());
        store.store.refuses = true;
        next.lock();
        QVERIFY2(next.problem().contains(u"This session is locked"), qPrintable(next.problem()));
    }
    void partialSetupAcceptsWindowsStoredSpelling()
    {
        WindowsSpellingSecrets store;
        AppLock lock(store, fast, [] { return 1000; });
        store.store.refusesTheKeep = 2; // Password saved; setup marker write refused.
        lock.submit({}, password, password, true);
        QTRY_VERIFY(!lock.busy());
        QCOMPARE(lock.mode(), u"setup"_s);
        QVERIFY(!store.kept(AppLock::recordName()).isEmpty());
        QVERIFY(!lock.problem().isEmpty());
        // Both names appear with Windows' spelling before the same setup is retried.
        QVERIFY(store.keep(u"app password established"_s, "1"));
        store.upper = true;
        lock.submit({}, password, password, true);
        QTRY_VERIFY(!lock.busy());
        QVERIFY2(!lock.restricted(), qPrintable(lock.problem()));
        QVERIFY(lock.mode().isEmpty());
    }
    void storedNameSpellingCannotTurnAValidPasswordIntoRecovery()
    {
        WindowsSpellingSecrets store;
        AppLock owner(store, fast, [] { return 1000; });
        setUp(owner);
        owner.lock();
        store.upper = true;
        AppLock next(store, fast, [] { return 1000; });
        QCOMPARE(next.mode(), u"unlock"_s);
        next.submit(password, {}, {});
        QTRY_VERIFY(!next.busy());
        QVERIFY(!next.restricted());
    }
    void resetRecognizesTheRemainingRecordWithWindowsStoredSpelling()
    {
        WindowsSpellingSecrets store;
        AppLock owner(store, fast, [] { return 1000; });
        setUp(owner);
        owner.forgot();
        store.upper = true;
        const QSignalSpy done(&owner, &AppLock::resetFinished);
        owner.resetSecrets(true);
        QCOMPARE(done.count(), 1);
        QVERIFY(store.store.all.isEmpty());
    }
    // PA: each framing guard alone, size kept wherever possible, so that no other guard
    // refuses the record first. PASS: mode "recovery" in every row.
    void passwordRecordFramingGuardsEachHold_data()
    {
        QTest::addColumn<int>("damage");
        QTest::newRow("magic altered") << 0;       // proves the magic compare (M5)
        QTest::newRow("first byte not zero") << 1; // proves the leading zero byte (M6)
        QTest::newRow("trailing zero pair") << 2;  // odd total, valid padding: proves the size equation (M4)
        QTest::newRow("length beyond data") << 3;  // ASAN: over-read if the size equation is removed (M4)
        QTest::newRow("short frame") << 4;         // ASAN: over-read if the minimum size is removed (M8)
    }
    void passwordRecordFramingGuardsEachHold()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString marker = directory.filePath(u"settings.json.locked"_s);
        MemorySecrets store;
        AppLock owner(store, fast, [] { return 1000; }, nullptr, marker);
        setUp(owner);
        QByteArray bytes = store.kept(AppLock::recordName());
        QCOMPARE(bytes.size() % 2, 1);
        QFETCH(int, damage);
        switch (damage) {
        case 0:
            bytes[1] = 'X';
            break;
        case 1:
            bytes[0] = 'A';
            break;
        case 2:
            bytes.append(2, '\0');
            break;
        case 3: // length field 2000 (even, so the expected padding stays one zero byte)
            bytes[6] = 0;
            bytes[7] = 0;
            bytes[8] = char(0x07);
            bytes[9] = '\xd0';
            break;
        case 4:
            bytes = QByteArray("\0QKVM\1\0", 7);
            break;
        default:
            QFAIL("Unknown framing row");
        }
        store.all.insert(AppLock::recordName(), bytes);
        AppLock next(store, fast, [] { return 1000; }, nullptr, marker);
        QCOMPARE(next.mode(), u"recovery"_s);
        QVERIFY(next.restricted());
    }

    // PB: a correctly framed record whose ONE field is wrong. The "control" row proves the
    // test's framing is accepted (mode "unlock", and the password unlocks), so every other
    // row proves its field check and not the framing. PASS: control unlocks, others "recovery".
    void framedRecordFieldsAreChecked_data()
    {
        QTest::addColumn<QString>("field");
        QTest::addColumn<QJsonValue>("value"); // Undefined: the field is removed
        QTest::addColumn<bool>("valid");
        QTest::newRow("control") << QString() << QJsonValue() << true;
        QTest::newRow("v 2") << u"v"_s << QJsonValue(2) << false;
        QTest::newRow("v text") << u"v"_s << QJsonValue(u"1"_s) << false;
        QTest::newRow("salt 15 bytes") << u"salt"_s << QJsonValue(QString::fromLatin1(QByteArray(15, 's').toBase64()))
                                       << false;
        QTest::newRow("verifier 31 bytes")
            << u"verifier"_s << QJsonValue(QString::fromLatin1(QByteArray(31, 'v').toBase64())) << false;
        QTest::newRow("automatic text") << u"automatic"_s << QJsonValue(u"true"_s) << false;
        QTest::newRow("locked missing") << u"locked"_s << QJsonValue(QJsonValue::Undefined) << false;
        QTest::newRow("screen number") << u"screen"_s << QJsonValue(0) << false;
        QTest::newRow("failures 31") << u"failures"_s << QJsonValue(31) << false;
        QTest::newRow("failures negative") << u"failures"_s << QJsonValue(-1) << false;
        QTest::newRow("failures fraction") << u"failures"_s << QJsonValue(1.5) << false;
        QTest::newRow("failures huge") << u"failures"_s << QJsonValue(1e308) << false;
        QTest::newRow("next negative") << u"next"_s << QJsonValue(-1) << false;
        QTest::newRow("next fraction") << u"next"_s << QJsonValue(0.5) << false;
        QTest::newRow("next huge") << u"next"_s << QJsonValue(1e308) << false;
        QTest::newRow("reset") << u"reset"_s << QJsonValue(true) << false;
        // M-D2: when the last wrong try was, optional (a record written before it has none).
        QTest::newRow("last given") << u"last"_s << QJsonValue(900) << true;
        QTest::newRow("last negative") << u"last"_s << QJsonValue(-1) << false;
        QTest::newRow("last fraction") << u"last"_s << QJsonValue(0.5) << false;
        QTest::newRow("last text") << u"last"_s << QJsonValue(u"900"_s) << false;
        QTest::newRow("last huge") << u"last"_s << QJsonValue(1e308) << false;
    }
    void framedRecordFieldsAreChecked()
    {
        QFETCH(QString, field);
        QFETCH(QJsonValue, value);
        QFETCH(bool, valid);
        MemorySecrets store;
        AppLock owner(store, fast, [] { return 1000; });
        setUp(owner); // leaves "app password established" in the store
        const QByteArray salt("0123456789abcdef");
        const QByteArray verifier = fast(QByteArrayView(password.toUtf8()), salt, std::stop_token{}).value();
        QJsonObject record{{u"v"_s, 1},
                           {u"salt"_s, QString::fromLatin1(salt.toBase64())},
                           {u"verifier"_s, QString::fromLatin1(verifier.toBase64())},
                           {u"automatic"_s, true},
                           {u"locked"_s, true},
                           {u"screen"_s, false},
                           {u"failures"_s, 0},
                           {u"next"_s, 0}};
        if (!field.isEmpty()) {
            if (value.isUndefined())
                record.remove(field);
            else
                record.insert(field, value);
        }
        const QByteArray json = QJsonDocument(record).toJson(QJsonDocument::Compact);
        QVERIFY(json.size() < 65536);
        QByteArray bytes("\0QKVM\1", 6);
        bytes.append(char(0)).append(char(0)).append(char((json.size() >> 8) & 0xff)).append(char(json.size() & 0xff));
        bytes.append(json);
        bytes.append(json.size() % 2 == 0 ? 1 : 2, '\0');
        store.all.insert(AppLock::recordName(), bytes);
        AppLock next(store, fast, [] { return 1000; });
        QCOMPARE(next.mode(), valid ? u"unlock"_s : u"recovery"_s);
        if (valid) {
            next.submit(password, {}, {});
            QTRY_VERIFY(!next.busy());
            QVERIFY(!next.restricted());
        }
    }

    // PC (defect probe, LOW, older than Phase 1b): `next` has a lower bound only. A wrong
    // password tried while the clock was ahead stores next = then + delay; after the clock is
    // corrected, retryAfter() is clamped to 300 for display but submit() refuses until the
    // stored time, so the correct password is refused for years and only "erase everything"
    // remains. PASS (sound): the correct password unlocks once the longest delay (300 s) passed.
    // FAIL (expected today): at the QVERIFY2 below, because submit() returned on retryAfter() > 0.
    void aRetryTimeFromAClockThatWasAheadDoesNotLockForever()
    {
        MemorySecrets store;
        qint64 now = 1000;
        AppLock lock(store, fast, [&] { return now; });
        setUp(lock);
        lock.lock();
        now = 2'000'000'000; // the PC's clock was years ahead
        lock.submit(u"wrong password"_s, {}, {});
        QTRY_VERIFY(!lock.busy());
        now = 1000; // the clock is corrected
        AppLock next(store, fast, [&] { return now; });
        QCOMPARE(next.mode(), u"unlock"_s);
        QVERIFY(next.retryAfter() <= 300);
        now += 301;
        next.submit(password, {}, {});
        QTRY_VERIFY(!next.busy());
        QVERIFY2(!next.restricted(), "a stored retry time beyond the longest delay refused the correct password");
    }
    void resetRequiresTheFinalStoreInspection_data()
    {
        QTest::addColumn<bool>("inspectionFails");
        QTest::newRow("listing fails after record erase") << true;
        QTest::newRow("entry appears after record erase") << false;
    }
    void resetRequiresTheFinalStoreInspection()
    {
        QFETCH(bool, inspectionFails);
        QTemporaryDir directory;
        const auto latch = directory.filePath(u"settings.json.locked"_s);
        MemorySecrets store;
        AppLock lock(store, fast, [] { return 1000; }, nullptr, latch);
        setUp(lock);
        store.all.insert(u"orphan"_s, "synthetic");
        lock.forgot();
        store.inspections = 0;
        store.onInspection = [&](int count) {
            if (count == 3) {
                if (inspectionFails)
                    store.refusesInspection = true;
                else
                    store.all.insert(u"late entry"_s, "synthetic");
            }
        };
        const QSignalSpy done(&lock, &AppLock::resetFinished);
        lock.resetSecrets(true);
        QCOMPARE(store.inspections, 3);
        QCOMPARE(done.size(), 0);
        QCOMPARE(lock.mode(), u"recovery"_s);
        QVERIFY(lock.restricted());
        store.onInspection = {};
        store.refusesInspection = false;
        AppLock restarted(store, fast, [] { return 1000; }, nullptr, latch);
        QCOMPARE(restarted.mode(), u"recovery"_s);
    }
    void ordinaryLockAndRestartUseTheProductionProfilePath()
    {
        QTemporaryDir directory;
        const auto profile = directory.filePath(u"settings.json"_s);
        const auto latch = profile + u".locked"_s;
        qkvm::core::Settings settings(profile);
        auto inventory = settings.remembered();
        inventory.server = u"https://example.invalid"_s;
        QVERIFY(settings.remember(inventory));
        MemorySecrets store;
        // Inventory without a password correctly needs the explicit reset first.
        AppLock first(store, fast, [] { return 1000; }, nullptr, latch);
        QCOMPARE(first.mode(), u"recovery"_s);
        first.forgot();
        first.resetSecrets(true);
        AppLock setup(store, fast, [] { return 1000; }, nullptr, latch);
        setUp(setup);
        setup.lock();
        QVERIFY(QFile::exists(latch));
        AppLock locked(store, fast, [] { return 1000; }, nullptr, latch);
        QCOMPARE(locked.mode(), u"unlock"_s);
        locked.submit(password, {}, {});
        QTRY_VERIFY(!locked.busy());
        QVERIFY(!locked.restricted());
        QVERIFY(!QFile::exists(latch));
        AppLock automatic(store, fast, [] { return 1000; }, nullptr, latch);
        QVERIFY(!automatic.restricted());
    }
    void failedFinalResetMarkerSaysSecretsWereErased()
    {
        QTemporaryDir directory;
        const QString latch = directory.filePath(u"settings.json.locked"_s);
        MemorySecrets store;
        AppLock lock(store, fast, [] { return 1000; }, nullptr, latch);
        setUp(lock);
        store.all.insert(u"orphan"_s, "synthetic");
        lock.forgot();
        store.onErased = [&](const QString& name) {
            if (name == AppLock::recordName()) {
                QVERIFY(QFile::remove(latch));
                QVERIFY(QDir().mkdir(latch));
            }
        };
        const QSignalSpy done(&lock, &AppLock::resetFinished);
        lock.resetSecrets(true);
        QVERIFY(store.all.isEmpty());
        QCOMPARE(done.size(), 0);
        QVERIFY(lock.restricted());
        QVERIFY2(lock.problem().contains(u"secrets were erased"), qPrintable(lock.problem()));
        QVERIFY(lock.problem().contains(u"could not be saved"));
    }
    void forgedRecordTypedIntoCredentialManagerNeverUnlocks()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString latch = directory.filePath(u"settings.json.locked"_s);
        MemorySecrets store;
        AppLock owner(store, fast, [] { return 1000; }, nullptr, latch);
        setUp(owner);
        store.all.insert(u"kvm:k1"_s, "saved device password");
        owner.lock();
        const QByteArray salt("0123456789abcdef");
        const QByteArray verifier = fast(QByteArrayView("attacker-password"), salt, std::stop_token{}).value();
        QByteArray json = "{\"v\":1,\"salt\":\"" + salt.toBase64() + "\",\"verifier\":\"" + verifier.toBase64() +
                          "\",\"automatic\":true,\"locked\":false,\"screen\":false,\"failures\":0,\"next\":0}";
        if (json.size() % 2 != 0)
            json.append(' ');
        QString typed;
        for (qsizetype at = 0; at < json.size(); at += 2)
            typed.append(QChar(char16_t(quint8(json[at]) | (quint8(json[at + 1]) << 8))));
        const QByteArray stored(reinterpret_cast<const char*>(typed.utf16()), typed.size() * 2);
        QCOMPARE(stored, json); // what Windows keeps for the typed text is exactly the record
        store.all.insert(AppLock::recordName(), stored);
        AppLock next(store, fast, [] { return 1000; }, nullptr, latch);
        QVERIFY(next.restricted());
        if (next.mode() == u"unlock"_s) {
            next.submit(u"attacker-password"_s, {}, {});
            QTRY_VERIFY(!next.busy());
        }
        QVERIFY2(next.restricted(), "a record typed into Credential Manager unlocked every saved secret");
    }

    // P3 quittingDuringFirstSetupStillOffersSetup
    //   Models production: main.cpp passes <settings>.locked, and WindowKeeper writes the first
    //   window's place into settings.json 400 ms after it is shown (window_keeper.cpp:59-68, 128-147;
    //   wall_windows.h:81), i.e. while the setup box is still open. The person closes qKVM (or the PC
    //   restarts) before choosing a password; nothing secret was ever saved.
    //   PASS (sound): the next start offers setup again.
    //   FAIL (defect, expected today): mode is "recovery" ("The app password record is missing or
    //   cannot be read. Reset saved secrets to continue.").
    void quittingDuringFirstSetupStillOffersSetup()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString profile = directory.filePath(u"settings.json"_s);
        MemorySecrets store;
        {
            AppLock first(store, fast, [] { return 1000; }, nullptr, profile + u".locked"_s);
            QCOMPARE(first.mode(), u"setup"_s);
            qkvm::core::Settings settings(profile);
            auto remembered = settings.remembered();
            qkvm::core::WallWindow window;
            window.place = qkvm::core::WindowPlace{24, 24, 1280, 800, false, false};
            remembered.windows.append(window);
            remembered.lastUpdateCheck = QDateTime::fromSecsSinceEpoch(1000);
            QVERIFY(settings.remember(remembered).ok());
        }
        QVERIFY(store.all.isEmpty());
        AppLock again(store, fast, [] { return 1000; }, nullptr, profile + u".locked"_s);
        QCOMPARE(again.mode(), u"setup"_s);
    }

    // The merged review's HIGH 1 (2026-11-06): main.cpp reads the wrong-password counts kept on disk before the lock
    // is made, and that keeps a new key for them in the same store (PacingFile::read). The real PacingFile and AppLock,
    // in one store, in the program's order, start after start:
    //   PASS: a first start offers first setup; so does the next one after quitting before a password was chosen, and
    //   that setup is finished with the key beside it; a reset of the saved secrets erases the key with the rest, the
    //   file sealed under it is put aside at the next start, and that start offers setup again.
    //   FAIL (before the fix): every one of those starts is in recovery ("The app password record is missing or cannot
    //   be read").
    void theKeyOfTheSignInCountsIsNoSignOfAProgramUsedBefore()
    {
        using qkvm::core::PacingFile;
        using qkvm::core::SignInPacing;
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString profile = directory.filePath(u"settings.json"_s);
        const QString counts = profile + u".sign-ins"_s;
        const QString latch = profile + u".locked"_s;
        const QString keyName = QString::fromLatin1(PacingFile::keyName);
        const SignInPacing::Mark wrong =
            SignInPacing::what(u"login", QUrl(u"https://192.0.2.20"_s), u"admin"_s, u"wrong-password"_s);
        PacingFile::Pace pace;
        pace.writtenAfterMs = 20;
        MemorySecrets store;
        {
            // A new PC.
            SignInPacing pacing;
            PacingFile kept(pacing, counts, store, pace);
            kept.read();
            QVERIFY(kept.keeps());
            QCOMPARE(store.all.keys(), QStringList{keyName});
            const AppLock first(store, fast, [] { return 1000; }, nullptr, latch);
            QCOMPARE(first.mode(), u"setup"_s);
            QVERIFY(!first.canStartSources());
        }
        QCOMPARE(store.all.keys(), QStringList{keyName});
        {
            // Quit before a password was chosen, and started again.
            SignInPacing pacing;
            PacingFile kept(pacing, counts, store, pace);
            kept.read();
            QVERIFY(kept.putAside().isEmpty());
            AppLock again(store, fast, [] { return 1000; }, nullptr, latch);
            QCOMPARE(again.mode(), u"setup"_s);
            // (The password saved and its marker not, the first time: finished at the second, with the key beside it.)
            store.refusesTheKeep = store.keeps + 2;
            again.submit({}, password, password, true);
            QTRY_VERIFY(!again.busy());
            QCOMPARE(again.mode(), u"setup"_s);
            QVERIFY(store.all.contains(AppLock::recordName()));
            setUp(again);
            QVERIFY(again.canStartSources());
            pacing.saidNo(wrong, {});
            QVERIFY(kept.waitForWritten(5000));
        }
        QVERIFY(QFile::exists(counts));
        {
            SignInPacing pacing;
            PacingFile kept(pacing, counts, store, pace);
            kept.read();
            QCOMPARE(pacing.times(wrong), 1);
            AppLock next(store, fast, [] { return 1000; }, nullptr, latch);
            QCOMPARE(next.mode(), QString());
            QVERIFY(!next.restricted());
            next.forgot();
            const QSignalSpy done(&next, &AppLock::resetFinished);
            next.resetSecrets(true);
            QCOMPARE(done.count(), 1);
            QVERIFY(store.all.isEmpty());
            // (Written as the program ends, under the key it read at its start.)
            pacing.saidNo(wrong, {});
        }
        {
            SignInPacing pacing;
            PacingFile kept(pacing, counts, store, pace);
            QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"no key opens it.*put aside as"_s));
            kept.read();
            QCOMPARE(kept.putAside(), counts + u".unreadable"_s);
            QCOMPARE(pacing.kept(), 0);
            QCOMPARE(pacing.times(wrong), 0);
            QCOMPARE(store.all.keys(), QStringList{keyName});
            AppLock after(store, fast, [] { return 1000; }, nullptr, latch);
            QCOMPARE(after.mode(), u"setup"_s);
            setUp(after);
        }
    }

    // P4 setupInterruptedAfterResetIsNotAWallScreen
    //   Power is lost after persist() wrote the new record but before the marker and before
    //   unlatch() removed the completed-reset marker "setup\n" (app_lock.cpp:423-428).
    //   load() then treats "setup\n" as an unreadable marker (app_lock.cpp:130).
    //   PASS (sound): unlock with the new password gives an unrestricted program, no wall screen.
    //   FAIL (defect, expected today): wallScreen() is true after the restart and after unlock.
    void setupInterruptedAfterResetIsNotAWallScreen()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString latch = directory.filePath(u"settings.json.locked"_s);
        MemorySecrets store;
        AppLock lock(store, fast, [] { return 1000; }, nullptr, latch);
        setUp(lock);
        lock.forgot();
        lock.resetSecrets(true);
        QVERIFY(store.all.isEmpty());
        AppLock next(store, fast, [] { return 1000; }, nullptr, latch);
        QCOMPARE(next.mode(), u"setup"_s);
        setUp(next);
        store.all.remove(u"app password established"_s); // the marker write never happened
        QFile marker(latch);                             // nor did the removal of "setup\n"
        QVERIFY(marker.open(QIODevice::WriteOnly));
        QCOMPARE(marker.write("setup\n"), 6);
        marker.close();
        AppLock after(store, fast, [] { return 1000; }, nullptr, latch);
        QCOMPARE(after.mode(), u"unlock"_s);
        QVERIFY(!after.wallScreen());
        after.submit(password, {}, {});
        QTRY_VERIFY(!after.busy());
        QVERIFY(!after.restricted());
    }

    void passwordRecordRejectsTextPaddingAndDamagedFraming_data()
    {
        QTest::addColumn<int>("damage");
        QTest::newRow("UTF-16 padded") << 0;
        QTest::newRow("truncated") << 1;
        QTest::newRow("missing magic") << 2;
        QTest::newRow("wrong length") << 3;
        QTest::newRow("wrong terminator") << 4;
    }
    void passwordRecordRejectsTextPaddingAndDamagedFraming()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString marker = directory.filePath(u"settings.json.locked"_s);
        MemorySecrets store;
        AppLock owner(store, fast, [] { return 1000; }, nullptr, marker);
        setUp(owner);
        QByteArray bytes = store.kept(AppLock::recordName());
        QCOMPARE(bytes.size() % 2, 1);
        QCOMPARE(bytes.front(), '\0');
        QFETCH(int, damage);
        switch (damage) {
        case 0:
            bytes.append('\0');
            break;
        case 1:
            bytes.chop(1);
            break;
        case 2:
            bytes.remove(0, 6);
            break;
        case 3:
            bytes[9] ^= 1;
            break;
        case 4:
            bytes.back() = 'x';
            break;
        default:
            QFAIL("Unknown framing damage row");
        }
        store.all.insert(AppLock::recordName(), bytes);
        AppLock next(store, fast, [] { return 1000; }, nullptr, marker);
        QCOMPARE(next.mode(), u"recovery"_s);
        QVERIFY(next.restricted());
    }
    void realScryptMatchesWebCost()
    {
        const auto result = AppLock::derivePassword("example-password", "0123456789abcdef", {});
        QVERIFY(result);
        QCOMPARE(result.value().toHex(),
                 QByteArray("347adc05ecd19927f229c108e8ede9939ae2e66bbe12c7ac299d4fe62834ad4e"));
    }
    void lockClosesLocalWorkAndAppliesToEveryWall()
    {
        QTemporaryDir directory;
        qkvm::core::Settings settings(directory.filePath(u"settings.json"_s));
        MemorySecrets store;
        AppLock lock(store, fast, [] { return 1000; });
        setUp(lock);
        qkvm::ui::OwnWall own(settings, store);
        own.useAppLock(lock);
        qkvm::ui::SessionView session;
        qkvm::ui::WallView all;
        qkvm::ui::ServerSession server(session, all, settings, store);
        server.useOwnWall(own.wall());
        server.useAppLock(lock);
        qkvm::ui::WallView first;
        qkvm::ui::WallView second;
        qkvm::ui::ActionsView firstActions;
        qkvm::ui::ActionsView secondActions;
        server.add(first, firstActions, nullptr);
        server.add(second, secondActions, nullptr);
        const auto remove = qScopeGuard([&] {
            server.remove(first);
            server.remove(second);
        });
        server.begin();
        QTRY_VERIFY(firstActions.allowed());
        QTRY_VERIFY(secondActions.allowed());
        qkvm::ui::AddKvmView adding;
        qkvm::ui::OwnFilesView files;
        own.serve(adding);
        own.serve(files);
        files.makeSelectedFile(u"example export password"_s, false, true, true);
        QCOMPARE(lock.mode(), u"export"_s);
        QVERIFY(files.isBusy());
        lock.lock();
        QVERIFY(!firstActions.allowed());
        QVERIFY(!secondActions.allowed());
        QVERIFY(!files.isBusy());
        QVERIFY(!files.isMade());
        files.makeFile({});
        QVERIFY(!files.isBusy());
        QVERIFY(!files.isMade());
        adding.addAsked(u"invalid address"_s, {}, {}, {}, false);
        QVERIFY(!adding.busy());
        lock.submit(password, {}, {});
        QTRY_VERIFY(!lock.busy());
        QVERIFY(firstActions.allowed());
        QVERIFY(secondActions.allowed());
        QVERIFY(!files.isMade());
    }
    void setupAndAutomaticStart()
    {
        MemorySecrets store;
        AppLock lock(store, fast, [] { return 1000; });
        QVERIFY(lock.restricted());
        QCOMPARE(lock.mode(), u"setup"_s);
        lock.submit({}, u"short"_s, u"short"_s);
        QVERIFY(lock.restricted());
        QVERIFY(store.all.isEmpty());
        setUp(lock);
        QVERIFY(!store.all.value(AppLock::recordName()).contains(password.toUtf8()));
        AppLock next(store, fast, [] { return 1000; });
        QVERIFY(!next.restricted());
    }
    void deletingPasswordEvidenceNeverExposesRemainingSecrets_data()
    {
        QTest::addColumn<int>("deleted");
        for (int mask = 0; mask < 8; ++mask) {
            QTest::newRow(qPrintable(QString::number(mask))) << mask;
        }
    }
    void deletingPasswordEvidenceNeverExposesRemainingSecrets()
    {
        QFETCH(int, deleted);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString latch = directory.filePath(u"settings.json.locked"_s);
        MemorySecrets store;
        AppLock owner(store, fast, [] { return 1000; }, nullptr, latch);
        setUp(owner);
        store.all.insert(u"kvm:k1"_s, "saved device password");
        store.all.insert(u"action:orphan"_s, "saved text");
        store.all.insert(u"session at example.invalid"_s, "saved session");
        owner.lock();
        QVERIFY(QFile::exists(latch));
        if ((deleted & 1) != 0)
            store.all.remove(AppLock::recordName());
        if ((deleted & 2) != 0)
            store.all.remove(u"app password established"_s);
        if ((deleted & 4) != 0)
            QVERIFY(QFile::remove(latch));
        const auto before = store.all;
        AppLock next(store, fast, [] { return 1000; }, nullptr, latch);
        QCOMPARE(next.mode(), (deleted & 1) != 0 ? u"recovery"_s : u"unlock"_s);
        QVERIFY(next.restricted());
        next.submit({}, u"replacement-password"_s, u"replacement-password"_s, true);
        QTRY_VERIFY(!next.busy());
        QVERIFY(next.restricted());
        QCOMPARE(store.all.value(u"kvm:k1"_s), before.value(u"kvm:k1"_s));
        QCOMPARE(store.all.value(u"action:orphan"_s), before.value(u"action:orphan"_s));
        QCOMPARE(store.all.value(u"session at example.invalid"_s), before.value(u"session at example.invalid"_s));
    }
    void anyPrefixedEntryPreventsFreshSetup_data()
    {
        QTest::addColumn<QString>("name");
        for (const auto& name :
             {u"KVM password"_s, u"action text"_s, u"session"_s, u"display link"_s, u"unknown orphan"_s})
            QTest::newRow(qPrintable(name)) << name;
    }
    void anyPrefixedEntryPreventsFreshSetup()
    {
        QFETCH(QString, name);
        MemorySecrets store;
        store.all.insert(name, {}); // Even an empty or unrecognised record is prior use.
        AppLock lock(store, fast, [] { return 1000; });
        QCOMPARE(lock.mode(), u"recovery"_s);
        QVERIFY(!lock.canStartSources());
        lock.submit({}, password, password, true);
        QVERIFY(lock.restricted());
        QCOMPARE(store.all.size(), 1);
    }
    void profileEvidenceDistinguishesInventoryFromFirstStartSettings_data()
    {
        QTest::addColumn<QString>("file");
        QTest::addColumn<QByteArray>("contents");
        QTest::addColumn<bool>("prior");
        QTest::newRow("empty settings") << u"settings.json"_s << QByteArray("{}") << false;
        QTest::newRow("metadata backup") << u"settings.json.backup"_s << QByteArray("{}") << false;
        QTest::newRow("damaged metadata") << u"settings.unreadable.json"_s << QByteArray("broken") << false;
        QTest::newRow("lock only") << u"settings.json.locked"_s << QByteArray("locked\n") << true;
        QTest::newRow("incidents") << u"settings.json.incidents"_s << QByteArray("{}") << true;
        QTest::newRow("damaged incidents")
            << u"settings.json.incidents.damaged-fixture"_s << QByteArray("broken") << true;
        QTest::newRow("server") << u"settings.json"_s << QByteArray("{\"server\":\"https://example.invalid\"}") << true;
        QTest::newRow("servers backup") << u"settings.json.backup"_s
                                        << QByteArray("{\"servers\":[\"https://example.invalid\"]}") << true;
        QTest::newRow("KVM inventory") << u"settings.json"_s
                                       << QByteArray(
                                              "{\"kvms\":[{\"id\":\"1234abcd\",\"kind\":\"kvmd\",\"name\":\"fixture\","
                                              "\"address\":\"http://127.0.0.1\"}]}")
                                       << true;
        // (Of a family this build does not read, or of none, as an older build wrote them: a wall was
        // kept all the same, and the file is no first start's.)
        QTest::newRow("KVM inventory this build does not read")
            << u"settings.json"_s
            << QByteArray("{\"kvms\":[{\"id\":\"1234abcd\",\"name\":\"fixture\",\"address\":\"http://127.0.0.1\"}]}")
            << true;
        QTest::newRow("no KVM of the program's shape, put aside")
            << u"settings.unreadable.json"_s
            << QByteArray("{\"kvms\":[{\"id\":\"1234ABCD\",\"kind\":\"jetkvm\",\"address\":\"http://127.0.0.1\"}]}")
            << false;
        QTest::newRow("KVM inventory put aside")
            << u"settings.unreadable.json"_s
            << QByteArray("{\"kvms\":[{\"id\":\"1234abcd\",\"kind\":\"jetkvm\",\"address\":\"http://127.0.0.1\"}]}")
            << true;
        QTest::newRow("actions") << u"settings.json"_s
                                 << QByteArray(
                                        "{\"actions\":[{\"id\":\"1234abcd\",\"name\":\"fixture\",\"kind\":\"keys\","
                                        "\"enter\":false,\"danger\":false,\"changed\":0,\"keys\":\"KeyA\"}]}")
                                 << true;
    }
    void profileEvidenceDistinguishesInventoryFromFirstStartSettings()
    {
        QFETCH(QString, file);
        QFETCH(QByteArray, contents);
        QFETCH(bool, prior);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QFile saved(directory.filePath(file));
        QVERIFY(saved.open(QIODevice::WriteOnly));
        QCOMPARE(saved.write(contents), contents.size());
        saved.close();
        MemorySecrets store;
        AppLock lock(store, fast, [] { return 1000; }, nullptr, directory.filePath(u"settings.json.locked"_s));
        QCOMPARE(lock.mode(), prior ? u"recovery"_s : u"setup"_s);
        QVERIFY(!lock.canStartSources());
    }
    void secretsArrivingDuringSetupAreNeverAdopted()
    {
        MemorySecrets store;
        AppLock lock(store, fast, [] { return 1000; });
        lock.submit({}, password, password);
        store.all.insert(u"orphan"_s, "must not be adopted");
        QTRY_VERIFY(!lock.busy());
        QCOMPARE(lock.mode(), u"recovery"_s);
        QVERIFY(lock.restricted());
        QCOMPARE(store.all.size(), 1);
    }
    void firstSetupWriteFailureCanBeRetriedWithoutReset()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString latch = directory.filePath(u"app.locked"_s);
        MemorySecrets store;
        const auto before = store.all;
        AppLock lock(store, fast, [] { return 1000; }, nullptr, latch);
        store.refuses = true;
        lock.submit({}, password, password);
        QTRY_VERIFY(!lock.busy());
        QVERIFY(lock.restricted());
        QCOMPARE(store.all, before);
        store.refuses = false;
        AppLock next(store, fast, [] { return 1000; }, nullptr, latch);
        QCOMPARE(next.mode(), u"setup"_s);
        setUp(next);
    }
    void interruptedSetupCanFinishWithTheSavedPassword()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString latch = directory.filePath(u"app.locked"_s);
        MemorySecrets store;
        store.refusesTheKeep = 2; // The password saves, but the setup marker does not.
        AppLock lock(store, fast, [] { return 1000; }, nullptr, latch);
        lock.submit({}, password, password);
        QTRY_VERIFY(!lock.busy());
        QVERIFY(lock.restricted());
        QVERIFY(store.all.contains(AppLock::recordName()));
        QVERIFY(!store.all.contains(u"app password established"_s));
        AppLock next(store, fast, [] { return 1000; }, nullptr, latch);
        QCOMPARE(next.mode(), u"unlock"_s);
        next.submit(password, {}, {});
        QTRY_VERIFY(!next.busy());
        QVERIFY(!next.restricted());
        QVERIFY(store.all.contains(u"app password established"_s));
    }
    void establishedPasswordWithoutItsMarkerRequiresUnlock()
    {
        MemorySecrets store;
        AppLock lock(store, fast, [] { return 1000; });
        setUp(lock);
        store.all.remove(u"app password established"_s);
        AppLock next(store, fast, [] { return 1000; });
        QCOMPARE(next.mode(), u"unlock"_s);
        QVERIFY(next.restricted());
        next.submit(password, {}, {});
        QTRY_VERIFY(!next.busy());
        QVERIFY(!next.restricted());
        QVERIFY(store.all.contains(u"app password established"_s));
    }
    void manualStartAndExplicitLock()
    {
        MemorySecrets store;
        AppLock lock(store, fast, [] { return 1000; });
        setUp(lock, false);
        AppLock next(store, fast, [] { return 1000; });
        QVERIFY(next.locked());
        next.submit(password, {}, {});
        QTRY_VERIFY(!next.busy());
        QVERIFY(!next.locked());
        next.ask(u"automatic"_s);
        next.submit(password, {}, {});
        QTRY_VERIFY(!next.busy());
        QVERIFY(next.automatic());
        next.lock();
        AppLock lockedAgain(store, fast, [] { return 1000; });
        QVERIFY(lockedAgain.locked());
    }
    void failedAttemptsPersist()
    {
        MemorySecrets store;
        qint64 now = 1000;
        AppLock lock(store, fast, [&] { return now; });
        setUp(lock);
        lock.lock();
        lock.submit(u"wrong"_s, {}, {});
        QTRY_VERIFY(!lock.busy());
        QVERIFY(lock.locked());
        QCOMPARE(lock.retryAfter(), 1);
        AppLock next(store, fast, [&] { return now; });
        next.submit(password, {}, {});
        QVERIFY(!next.busy());
        QVERIFY(next.locked());
        now += 1;
        next.submit(u"wrong again"_s, {}, {});
        QTRY_VERIFY(!next.busy());
        QCOMPARE(next.retryAfter(), 2);
        now += 2;
        next.submit(password, {}, {});
        QTRY_VERIFY(!next.busy());
        QVERIFY(!next.locked());
        QCOMPARE(next.retryAfter(), 0);
    }
    // M-D2, "a day of tries" (the handoff has the table). Within a run a wrong try's wait is also
    // counted on a clock nobody sets, and the later of the two waits holds: setting the PC's clock
    // ahead does not shorten it. (Before: the wait was gone with the clock set ahead.)
    void aClockSetAheadDoesNotShortenAWait()
    {
        MemorySecrets store;
        qint64 now = 1000;
        qint64 steady = 50;
        AppLock lock(store, fast, [&] { return now; }, nullptr, {}, [&] { return steady; });
        setUp(lock);
        lock.lock();
        for (int tries = 0; tries < 5; ++tries) { // waits of 1, 2, 4, 8, then 16 seconds
            now += 16;
            steady += 16;
            lock.submit(u"wrong"_s, {}, {});
            QTRY_VERIFY(!lock.busy());
        }
        QCOMPARE(lock.retryAfter(), 16);
        now += 3600; // the PC's clock is set an hour ahead
        QCOMPARE(lock.retryAfter(), 16);
        lock.submit(password, {}, {});
        QVERIFY(!lock.busy());
        QVERIFY(lock.locked());
        steady += 16;
        QCOMPARE(lock.retryAfter(), 0);
        lock.submit(password, {}, {});
        QTRY_VERIFY(!lock.busy());
        QVERIFY2(!lock.locked(), qPrintable(lock.problem()));
    }
    // A clock put back (a fast clock corrected) costs a correct password at most the longest wait,
    // counted from when it went back. (Before: the wait lasted until the clock caught up again.)
    void aClockSetBackHoldsNoLongerThanTheLongestWait()
    {
        MemorySecrets store;
        qint64 now = 100000;
        qint64 steady = 50;
        AppLock lock(store, fast, [&] { return now; }, nullptr, {}, [&] { return steady; });
        setUp(lock);
        lock.lock();
        lock.submit(u"wrong"_s, {}, {});
        QTRY_VERIFY(!lock.busy());
        QCOMPARE(lock.retryAfter(), 1);
        now -= 3600;        // the clock is put back an hour
        QTest::qWait(1100); // the program's own one-second look at the wait goes by
        now += 300;
        steady += 300;
        QCOMPARE(lock.retryAfter(), 0);
        lock.submit(password, {}, {});
        QTRY_VERIFY(!lock.busy());
        QVERIFY2(!lock.locked(), qPrintable(lock.problem()));
    }
    // Round 2 (review L3): a clock put back holds a correct password no longer than the wait the count
    // owes, not the longest wait: one typo, then a fast clock corrected by ten minutes, costs a second,
    // in the same run and after a restart. (Before: 300 s.)
    void aClockPutBackHoldsOnlyTheWaitThatIsOwed_data()
    {
        QTest::addColumn<bool>("restarted");
        QTest::newRow("in the same run") << false;
        QTest::newRow("after a restart") << true;
    }
    void aClockPutBackHoldsOnlyTheWaitThatIsOwed()
    {
        QFETCH(bool, restarted);
        MemorySecrets store;
        qint64 now = 100000;
        qint64 steady = 50;
        AppLock lock(store, fast, [&] { return now; }, nullptr, {}, [&] { return steady; });
        setUp(lock);
        lock.lock();
        lock.submit(u"wrong"_s, {}, {});
        QTRY_VERIFY(!lock.busy());
        QCOMPARE(lock.retryAfter(), 1);
        now -= 600; // the clock was ten minutes fast, and is corrected
        AppLock again(store, fast, [&] { return now; }, nullptr, {}, [&] { return steady; });
        AppLock& trying = restarted ? again : lock;
        QVERIFY2(trying.retryAfter() <= 1, qPrintable(u"%1 s for a wait of 1 s"_s.arg(trying.retryAfter())));
        now += 1;
        steady += 1;
        QCOMPARE(trying.retryAfter(), 0);
        trying.submit(password, {}, {});
        QTRY_VERIFY(!trying.busy());
        QVERIFY2(!trying.locked(), qPrintable(trying.problem()));
    }
    // A wrong try recorded while the clock was far ahead does not keep the count from being forgotten:
    // its time is taken as no later than now. (Before: forgotten only once the clock passed it and a day.)
    void aWrongTryRecordedWhileTheClockWasAheadIsForgottenADayLater()
    {
        MemorySecrets store;
        qint64 now = 1000;
        AppLock lock(store, fast, [&] { return now; });
        setUp(lock);
        lock.lock();
        now = 2'000'000'000; // the clock was years ahead
        for (int tries = 0; tries < 10; ++tries) {
            now += 300;
            lock.submit(u"wrong"_s, {}, {});
            QTRY_VERIFY(!lock.busy());
        }
        now = 1000; // corrected
        AppLock next(store, fast, [&] { return now; });
        now += 86400 + 301;
        next.submit(u"wrong"_s, {}, {});
        QTRY_VERIFY(!next.busy());
        QCOMPARE(next.retryAfter(), 1);
    }
    // A day with no wrong try forgets the count: the next wrong try waits a second, not five minutes.
    // (Before: the count was never forgotten but by a right password.)
    void aDayWithoutAWrongTryForgetsTheCount_data()
    {
        QTest::addColumn<bool>("restarted");
        QTest::newRow("in the same run") << false;
        QTest::newRow("after a restart") << true;
    }
    void aDayWithoutAWrongTryForgetsTheCount()
    {
        QFETCH(bool, restarted);
        MemorySecrets store;
        qint64 now = 1000;
        qint64 steady = 50;
        AppLock lock(store, fast, [&] { return now; }, nullptr, {}, [&] { return steady; });
        setUp(lock);
        lock.lock();
        for (int tries = 0; tries < 10; ++tries) {
            now += 300;
            steady += 300;
            lock.submit(u"wrong"_s, {}, {});
            QTRY_VERIFY(!lock.busy());
        }
        QCOMPARE(lock.retryAfter(), 300);
        now += 86400;
        steady += 86400;
        AppLock again(store, fast, [&] { return now; }, nullptr, {}, [] { return 7; }); // its own run and clock
        AppLock& trying = restarted ? again : lock;
        trying.submit(u"wrong"_s, {}, {});
        QTRY_VERIFY(!trying.busy());
        QCOMPARE(trying.retryAfter(), 1);
    }
    // Wrong tries less than a day apart go on counting.
    void wrongTriesSpreadOverTheDayKeepCounting()
    {
        MemorySecrets store;
        qint64 now = 1000;
        qint64 steady = 50;
        AppLock lock(store, fast, [&] { return now; }, nullptr, {}, [&] { return steady; });
        setUp(lock);
        lock.lock();
        for (int tries = 0; tries < 10; ++tries) {
            now += qint64{23} * 3600;
            steady += qint64{23} * 3600;
            lock.submit(u"wrong"_s, {}, {});
            QTRY_VERIFY(!lock.busy());
        }
        QCOMPARE(lock.retryAfter(), 300);
    }
    // Within a run, a day is a day on both clocks: setting the clock a day ahead forgets nothing.
    // (Holds the rule of the fix; it passed before the fix too, when nothing was ever forgotten.)
    void aClockSetADayAheadForgetsNothing()
    {
        MemorySecrets store;
        qint64 now = 1000;
        qint64 steady = 50;
        AppLock lock(store, fast, [&] { return now; }, nullptr, {}, [&] { return steady; });
        setUp(lock);
        lock.lock();
        for (int tries = 0; tries < 10; ++tries) {
            now += 300;
            steady += 300;
            lock.submit(u"wrong"_s, {}, {});
            QTRY_VERIFY(!lock.busy());
        }
        now += 86400;
        steady += 300;
        lock.submit(u"wrong"_s, {}, {});
        QTRY_VERIFY(!lock.busy());
        QCOMPARE(lock.retryAfter(), 300);
    }
    void lockSurvivesFailedCredentialWrite()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString latch = directory.filePath(u"app.locked"_s);
        MemorySecrets store;
        AppLock lock(store, fast, [] { return 1000; }, nullptr, latch);
        setUp(lock);
        store.refuses = true;
        lock.lock();
        QVERIFY(lock.locked());
        QVERIFY(!lock.problem().isEmpty());
        AppLock next(store, fast, [] { return 1000; }, nullptr, latch);
        QVERIFY(next.locked());
        store.refuses = false;
        next.submit(password, {}, {});
        QTRY_VERIFY(!next.busy());
        QVERIFY(!next.locked());
        QVERIFY(!QFileInfo::exists(latch));
    }
    void missingOrBrokenRecordIsRecovery()
    {
        MemorySecrets store;
        AppLock initial(store, fast, [] { return 1000; });
        setUp(initial);
        store.all.remove(AppLock::recordName());
        store.all.insert(u"orphan action text"_s, "never show this");
        AppLock missing(store, fast, [] { return 1000; });
        QCOMPARE(missing.mode(), u"recovery"_s);
        missing.submit({}, password, password);
        QVERIFY(missing.restricted());
        store.all.insert(AppLock::recordName(), "broken");
        AppLock broken(store, fast, [] { return 1000; });
        QCOMPARE(broken.mode(), u"recovery"_s);
    }
    // M-D1. Windows not handing over the list of what qKVM keeps, once or every time, is no evidence
    // that anything was lost: the program says so and offers Try again and Quit, never setup, never
    // a reset, never a write. (Before: mode "recovery", whose only way on is erasing every secret.)
    void aStoreThatCannotBeListedOffersTryAgainNeverAReset()
    {
        QTemporaryDir directory;
        const QString latch = directory.filePath(u"settings.json.locked"_s);
        MemorySecrets store;
        AppLock initial(store, fast, [] { return 1000; }, nullptr, latch);
        setUp(initial);
        const auto before = store.all;
        const int keepsBefore = store.keeps;
        store.refusesInspection = true;
        AppLock lock(store, fast, [] { return 1000; }, nullptr, latch);
        QCOMPARE(lock.mode(), u"unavailable"_s);
        QVERIFY(lock.restricted());
        QVERIFY(lock.locked());
        QVERIFY(!lock.canStartSources());
        QVERIFY2(lock.problem().contains(u"Windows"), qPrintable(lock.problem()));
        // None of the ways to a reset or a write is open.
        lock.forgot();
        QCOMPARE(lock.mode(), u"unavailable"_s);
        lock.resetSecrets(true);
        lock.submit(password, {}, {});
        QVERIFY(!lock.busy());
        lock.lock();
        lock.becameWallScreen();
        QVERIFY(!lock.wallScreen());
        lock.ask(u"unlock"_s);
        QCOMPARE(lock.mode(), u"unavailable"_s);
        QCOMPARE(store.all, before);
        QCOMPARE(store.keeps, keepsBefore);
        QVERIFY(!QFileInfo::exists(latch));
        // Every time: Try again asks again and stays where it was.
        lock.tryAgain();
        QCOMPARE(lock.mode(), u"unavailable"_s);
        QCOMPARE(store.all, before);
        QCOMPARE(store.keeps, keepsBefore);
        // Closing the box leaves the program locked, and asking opens it again.
        lock.cancel();
        QVERIFY(lock.restricted());
        lock.ask(u"unlock"_s);
        QCOMPARE(lock.mode(), u"unavailable"_s);
        const QSignalSpy quit(&lock, &AppLock::quitAsked);
        lock.quit();
        QCOMPARE(quit.size(), 1);
    }
    void aStoreThatAnswersAgainCarriesOnAsUsual_data()
    {
        QTest::addColumn<bool>("automatic");
        QTest::newRow("signed in automatically") << true;
        QTest::newRow("asked for the password") << false;
    }
    void aStoreThatAnswersAgainCarriesOnAsUsual()
    {
        QFETCH(bool, automatic);
        MemorySecrets store;
        AppLock initial(store, fast, [] { return 1000; });
        setUp(initial, automatic);
        const int keepsBefore = store.keeps;
        store.refusesTheInspection = store.inspections + 1; // the next one only
        AppLock lock(store, fast, [] { return 1000; });
        QCOMPARE(lock.mode(), u"unavailable"_s);
        QVERIFY(!lock.canStartSources());
        const QSignalSpy changed(&lock, &AppLock::changed);
        lock.tryAgain();
        QVERIFY(changed.size() > 0);
        QCOMPARE(lock.mode(), automatic ? QString() : u"unlock"_s);
        QCOMPARE(lock.restricted(), !automatic);
        QVERIFY(lock.canStartSources());
        QVERIFY2(lock.problem().isEmpty(), qPrintable(lock.problem()));
        QCOMPARE(store.keeps, keepsBefore);
    }
    // The list names the password record, and Windows does not hand the record over: no damage is
    // known, so this is the same "not available", not "missing or broken".
    void aRecordThatCannotBeReadIsNoDamage()
    {
        MemorySecrets store;
        AppLock initial(store, fast, [] { return 1000; });
        setUp(initial);
        store.refusesReads = true;
        AppLock lock(store, fast, [] { return 1000; });
        QCOMPARE(lock.mode(), u"unavailable"_s);
        store.refusesReads = false;
        lock.tryAgain();
        QVERIFY2(!lock.restricted(), qPrintable(lock.problem()));
    }
    // Listed, then gone when it is read: asked again, the store's own answer decides (here the record
    // is really missing, which is recovery as before).
    void aRecordThatGoesBetweenListAndReadIsAskedAgain()
    {
        MemorySecrets store;
        AppLock initial(store, fast, [] { return 1000; });
        setUp(initial);
        store.onRead = [&](const QString& name) { store.all.remove(name); };
        AppLock lock(store, fast, [] { return 1000; });
        QCOMPARE(lock.mode(), u"unavailable"_s);
        store.onRead = {};
        lock.tryAgain();
        QCOMPARE(lock.mode(), u"recovery"_s);
    }
    // At the end of setup the list is asked again (secrets that appeared meanwhile are never
    // adopted). A list Windows does not hand over then is no such secret: nothing is saved, setup
    // stays, and the next try sets the password. (Before: recovery.)
    void aListThatFailsAtTheEndOfSetupLeavesSetupToTryAgain()
    {
        MemorySecrets store;
        AppLock lock(store, fast, [] { return 1000; });
        QCOMPARE(lock.mode(), u"setup"_s);
        store.refusesTheInspection = store.inspections + 1; // the one in finish()
        lock.submit({}, password, password, true);
        QTRY_VERIFY(!lock.busy());
        QCOMPARE(lock.mode(), u"setup"_s);
        QVERIFY(lock.restricted());
        QVERIFY(store.all.isEmpty());
        QVERIFY2(lock.problem().contains(u"Try again"), qPrintable(lock.problem()));
        lock.submit({}, password, password, true);
        QTRY_VERIFY(!lock.busy());
        QVERIFY2(!lock.restricted(), qPrintable(lock.problem()));
    }
    // A list that fails as a reset begins: the reset says that some secrets could not be erased and
    // stays in recovery (as it did before M-D1; held here).
    void aListFailureDuringAResetSaysSomeSecretsRemain()
    {
        MemorySecrets store;
        AppLock lock(store, fast, [] { return 1000; });
        setUp(lock);
        store.all.insert(u"orphan"_s, "synthetic");
        lock.forgot();
        store.refusesTheInspection = store.inspections + 1;
        const QSignalSpy done(&lock, &AppLock::resetFinished);
        lock.resetSecrets(true);
        QCOMPARE(done.size(), 0);
        QCOMPARE(lock.mode(), u"recovery"_s);
        QVERIFY(store.all.contains(u"orphan"_s));
        QVERIFY2(lock.problem().contains(u"could not be erased"), qPrintable(lock.problem()));
    }
    void changedPasswordReplacesOldOne()
    {
        MemorySecrets store;
        qint64 now = 1000;
        AppLock lock(store, fast, [&] { return now; });
        setUp(lock);
        lock.ask(u"password"_s);
        const QString replacement = u"different-password"_s;
        lock.submit(password, replacement, replacement);
        QTRY_VERIFY(!lock.busy());
        QVERIFY(!lock.restricted());
        lock.lock();
        lock.submit(password, {}, {});
        QTRY_VERIFY(!lock.busy());
        QVERIFY(lock.locked());
        now += 1;
        lock.submit(replacement, {}, {});
        QTRY_VERIFY(!lock.busy());
        QVERIFY(!lock.locked());
    }
    void lockInvalidatesQueuedApproval()
    {
        MemorySecrets store;
        AppLock lock(store, fast, [] { return 1000; });
        setUp(lock);
        QObject owner;
        int approved = 0;
        QVERIFY(lock.authorizeExport(&owner, [&] { ++approved; }));
        lock.submit(password, {}, {});
        lock.lock();
        QTest::qWait(30);
        QCOMPARE(approved, 0);
        QVERIFY(lock.restricted());
    }
    void exportApprovalIsOneUseAndOwned()
    {
        MemorySecrets store;
        AppLock lock(store, fast, [] { return 1000; });
        setUp(lock);
        QObject owner;
        int approved = 0;
        QVERIFY(lock.authorizeExport(&owner, [&] { ++approved; }));
        lock.submit(password, {}, {});
        QTRY_COMPARE(approved, 1);
        lock.submit(password, {}, {});
        QCOMPARE(approved, 1);
        QVERIFY(lock.authorizeExport(&owner, [&] { ++approved; }));
        lock.cancelApproval(&owner);
        lock.submit(password, {}, {});
        QCOMPARE(approved, 1);
        auto gone = std::make_unique<QObject>();
        QVERIFY(lock.authorizeExport(gone.get(), [&] { ++approved; }));
        gone.reset();
        QVERIFY(lock.mode().isEmpty());
        lock.submit(password, {}, {});
        QTRY_VERIFY(!lock.busy());
        QCOMPARE(approved, 1);
    }
    void wallScreenNeedsLocalPassword()
    {
        MemorySecrets store;
        AppLock lock(store, fast, [] { return 1000; });
        setUp(lock);
        lock.becameWallScreen();
        QVERIFY(lock.restricted());
        AppLock next(store, fast, [] { return 1000; });
        QVERIFY(next.wallScreen());
        next.ask(u"endScreen"_s);
        next.submit(password, {}, {});
        QTRY_VERIFY(!next.busy());
        QVERIFY(next.wallScreen()); // No local cleanup handler: never silently unlock.
        next.endScreenWith([] { return true; });
        next.submit(password, {}, {});
        QTRY_VERIFY(!next.busy());
        QVERIFY(!next.restricted());
    }
    void wallScreenCanEndFromOwnWallWithoutItsServer()
    {
        QTemporaryDir directory;
        qkvm::core::Settings settings(directory.filePath(u"settings.json"_s));
        MemorySecrets store;
        AppLock lock(store, fast, [] { return 1000; });
        setUp(lock);
        store.all.insert(u"wall screen"_s, "https://example.invalid:8787");
        store.all.insert(u"display link at https://example.invalid:8787"_s, "test link");
        store.all.insert(u"session at https://example.invalid:8787"_s, "test session");
        qkvm::ui::OwnWall own(settings, store);
        qkvm::ui::SessionView session;
        qkvm::ui::WallView all;
        qkvm::ui::ServerSession server(session, all, settings, store);
        server.useOwnWall(own.wall());
        server.useAppLock(lock);
        server.begin();
        QVERIFY(session.ownWall());
        QVERIFY(lock.wallScreen());
        session.signOut();
        QCOMPARE(lock.mode(), u"endScreen"_s);
        lock.submit(password, {}, {});
        QTRY_VERIFY(!lock.busy());
        QVERIFY(!lock.restricted());
        QCOMPARE(store.all.size(), 2);
        QVERIFY(store.all.contains(AppLock::recordName()));
        QVERIFY(store.all.contains(u"app password established"_s));
    }
    void aWallScreenWithoutAnAppPasswordRequiresRecovery()
    {
        QTemporaryDir directory;
        qkvm::core::Settings settings(directory.filePath(u"settings.json"_s));
        MemorySecrets store;
        store.all.insert(u"wall screen"_s, "https://example.invalid:8787");
        store.all.insert(u"display link at https://example.invalid:8787"_s, "saved display link");
        AppLock lock(store, fast, [] { return 1000; });
        qkvm::ui::OwnWall own(settings, store);
        qkvm::ui::SessionView session;
        qkvm::ui::WallView all;
        qkvm::ui::ServerSession server(session, all, settings, store);
        server.useOwnWall(own.wall());
        server.useAppLock(lock);
        QCOMPARE(lock.mode(), u"recovery"_s);
        QVERIFY(lock.wallScreen());
        QVERIFY(!lock.canStartSources());
        lock.submit({}, password, password);
        QTRY_VERIFY(!lock.busy());
        QVERIFY(!lock.canStartSources());
        QVERIFY(lock.restricted());
        QVERIFY(lock.wallScreen());
        QCOMPARE(store.all.value(u"display link at https://example.invalid:8787"_s), QByteArray("saved display link"));
        AppLock next(store, fast, [] { return 1000; });
        QCOMPARE(next.mode(), u"recovery"_s);
        QVERIFY(next.restricted());
    }
    void resetErasesOrphansAndRequiresConfirmation()
    {
        MemorySecrets store;
        AppLock lock(store, fast, [] { return 1000; });
        setUp(lock);
        store.all.insert(u"action:orphan:a1"_s, "private text");
        store.all.insert(u"session at example.invalid"_s, "session");
        store.all.insert(u"display link at example.invalid"_s, "link");
        QSignalSpy done(&lock, &AppLock::resetFinished);
        lock.resetSecrets(true);
        QCOMPARE(store.all.size(), 5);
        lock.forgot();
        lock.resetSecrets(false);
        QCOMPARE(store.all.size(), 5);
        lock.resetSecrets(true);
        QVERIFY(store.all.isEmpty());
        QCOMPARE(done.size(), 1);
        QVERIFY(lock.restricted());
        AppLock next(store, fast, [] { return 1000; });
        QCOMPARE(next.mode(), u"setup"_s);
    }
    void explicitResetRetainsInventoryButErasesAllSecretsBeforeSetup()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString profile = directory.filePath(u"settings.json"_s);
        const QString latch = profile + u".locked"_s;
        MemorySecrets store;
        AppLock lock(store, fast, [] { return 1000; }, nullptr, latch);
        setUp(lock);
        QFile inventory(profile);
        QVERIFY(inventory.open(QIODevice::WriteOnly));
        QCOMPARE(inventory.write("{\"inventory\":[]}"), 16);
        inventory.close();
        store.all.insert(u"orphan"_s, "secret");
        lock.forgot();
        lock.resetSecrets(true);
        QVERIFY(store.all.isEmpty());
        QVERIFY(inventory.exists());
        AppLock next(store, fast, [] { return 1000; }, nullptr, latch);
        QCOMPARE(next.mode(), u"setup"_s);
        setUp(next);
        QVERIFY(!QFile::exists(latch));
        QVERIFY(inventory.exists());
    }
    void aCompletedResetMarkerCannotAdoptSecrets()
    {
        QTemporaryDir directory;
        const QString latch = directory.filePath(u"settings.json.locked"_s);
        QFile marker(latch);
        QVERIFY(marker.open(QIODevice::WriteOnly));
        QCOMPARE(marker.write("setup\n"), 6);
        marker.close();
        MemorySecrets store;
        store.all.insert(u"orphan"_s, "secret");
        AppLock lock(store, fast, [] { return 1000; }, nullptr, latch);
        QCOMPARE(lock.mode(), u"recovery"_s);
        QVERIFY(lock.restricted());
    }
    void incompleteResetNeverAcceptsOldPassword()
    {
        MemorySecrets store;
        AppLock lock(store, fast, [] { return 1000; });
        setUp(lock);
        store.all.insert(u"orphan"_s, "text");
        store.forgetsNothing = true;
        lock.forgot();
        lock.resetSecrets(true);
        QVERIFY(lock.restricted());
        AppLock next(store, fast, [] { return 1000; });
        QCOMPARE(next.mode(), u"recovery"_s);
        next.submit(password, {}, {});
        QVERIFY(next.restricted());
        store.forgetsNothing = false;
        next.forgot();
        next.resetSecrets(true);
        QVERIFY(store.all.isEmpty());
    }
    void resetSurvivesFailedCredentialWrite()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString latch = directory.filePath(u"app.locked"_s);
        MemorySecrets store;
        AppLock lock(store, fast, [] { return 1000; }, nullptr, latch);
        setUp(lock);
        store.all.insert(u"orphan"_s, "preserve until reset can start");
        const auto before = store.all;
        store.refuses = true;
        lock.forgot();
        lock.resetSecrets(true);
        QVERIFY(lock.restricted());
        QCOMPARE(store.all, before);
        AppLock next(store, fast, [] { return 1000; }, nullptr, latch);
        QCOMPARE(next.mode(), u"recovery"_s);
        next.submit(password, {}, {});
        QVERIFY(next.restricted());
        store.refuses = false;
        next.forgot();
        next.resetSecrets(true);
        QVERIFY(store.all.isEmpty());
        QVERIFY(QFileInfo::exists(latch)); // Explicit completed reset, not an implicit fresh profile.
        AppLock fresh(store, fast, [] { return 1000; }, nullptr, latch);
        QCOMPARE(fresh.mode(), u"setup"_s);
    }
    void failedResetReportsWhenNeitherMarkerCanBeSaved()
    {
        MemorySecrets store;
        AppLock lock(store, fast, [] { return 1000; }); // No fallback persistence path.
        setUp(lock);
        store.refuses = true;
        lock.forgot();
        lock.resetSecrets(true);
        QVERIFY(lock.restricted());
        QVERIFY(lock.problem().contains(u"reset could not be saved"));
    }
    void savedActionsProtectTextAndStaleEdits()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        qkvm::core::Settings settings(directory.filePath(u"settings.json"_s));
        MemorySecrets store;
        AppLock lock(store, fast, [] { return 1000; });
        qkvm::core::OwnActions actions(settings, store);
        qkvm::ui::OwnActionsView view(actions, lock);
        QVERIFY(!view.save({}, 0, u"Private"_s, true, {}, u"kept secret"_s, true, true).isEmpty());
        setUp(lock);
        QVERIFY(view.save({}, 0, u"Private"_s, true, {}, u"kept secret"_s, true, true).isEmpty());
        const QVariantMap entry = view.entries().first().toMap();
        QVERIFY(!entry.contains(u"text"_s));
        QVERIFY(!QJsonDocument::fromVariant(view.entries()).toJson().contains("kept secret"));
        const QString id = entry.value(u"id"_s).toString();
        const int revision = entry.value(u"revision"_s).toInt();
        QVERIFY(view.save(id, revision, u"Renamed"_s, true, {}, {}, true, true).isEmpty());
        QVERIFY(!view.save(id, revision, u"Stale edit"_s, true, {}, u"other text"_s, true, true).isEmpty());
        QVERIFY(!view.remove(id, revision).isEmpty());
        lock.lock();
        const auto current = view.entries().first().toMap();
        QVERIFY(!view.remove(id, current.value(u"revision"_s).toInt()).isEmpty());
        QCOMPARE(actions.all().size(), 1);
    }
};
QTEST_GUILESS_MAIN(TestAppLock)
#include "test_app_lock.moc"
