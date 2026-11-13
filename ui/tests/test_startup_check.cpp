#include "../installer/health_exchange.h"
#include "../installer/health_protocol.h"
#include "../installer/package_environment.h"
#include "../installer/setup_launch.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>
#include <array>
#include <atomic>
#include <thread>
#include <windows.h>
using namespace qkvm::installer;
using namespace Qt::StringLiterals;
class TestStartupCheck : public QObject {
    Q_OBJECT
private slots:
    void packageCheckCleanupAlsoRemovesTheJavaScriptEngineOverrides()
    {
        const QByteArray path = qgetenv("PATH");
        const auto restore = qScopeGuard([&] {
            qputenv("PATH", path);
            qunsetenv("QV4_MAX_CALL_DEPTH");
            qunsetenv("QV4_CRASH_ON_STACKOVERFLOW");
        });
        QVERIFY(qputenv("QV4_MAX_CALL_DEPTH", "1"));
        QVERIFY(qputenv("QV4_CRASH_ON_STACKOVERFLOW", "1"));
        preparePackageCheckEnvironment();
        QVERIFY2(!qEnvironmentVariableIsSet("QV4_MAX_CALL_DEPTH"), "QV4_ is not in the cleanup's prefix list");
        QVERIFY(!qEnvironmentVariableIsSet("QV4_CRASH_ON_STACKOVERFLOW"));
    }
    void packageCheckCleanupIgnoresTheCaseOfANameAsQtDoes()
    {
        const QByteArray path = qgetenv("PATH");
        const QByteArray cache = qgetenv("QML_DISABLE_DISK_CACHE");
        const auto restore = qScopeGuard([&] {
            qputenv("PATH", path);
            if (cache.isNull())
                qunsetenv("QML_DISABLE_DISK_CACHE");
            else
                qputenv("QML_DISABLE_DISK_CACHE", cache);
            for (const char* name : {"qt_qpa_platform", "qml2_import_path", "qsg_render_loop"})
                qunsetenv(name);
        });
        QVERIFY(!qEnvironmentVariableIsSet("QT_QPA_PLATFORM"));
        QVERIFY(qputenv("qt_qpa_platform", "offscreen"));
        QVERIFY(qputenv("qml2_import_path", "C:\\synthetic-poison"));
        QVERIFY(qputenv("qsg_render_loop", "synthetic-poison"));
        QVERIFY(qEnvironmentVariableIsSet("QT_QPA_PLATFORM")); // Qt's own read ignores case
        preparePackageCheckEnvironment();
        for (const char* name : {"QT_QPA_PLATFORM", "QML2_IMPORT_PATH", "QSG_RENDER_LOOP"}) {
            QVERIFY2(!qEnvironmentVariableIsSet(name), name);
            QVERIFY2(GetEnvironmentVariableW(QString::fromLatin1(name).toStdWString().c_str(), nullptr, 0) == 0, name);
        }
        QCOMPARE(qgetenv("QML_DISABLE_DISK_CACHE"), QByteArray("1"));
    }
    void theAppsFirstHopWorksFromAQtStylePathWithSpaces()
    {
        // installer_launcher.cpp passes QTemporaryDir::filePath(): '/' separators, and a user folder may
        // contain spaces. Real CreateProcessW + DESKTOP_APP_POLICY on hop 1 and the real relay hop 2;
        // only the read-only --check-helper is run. Error mode set so a missing DLL cannot open a dialog.
        const UINT mode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        const auto restoreMode = qScopeGuard([&] { SetErrorMode(mode); });
        const QDir bin(QCoreApplication::applicationDirPath());
        QTemporaryDir folder(bin.filePath(u"q kvm setup XXXXXX"_s));
        QVERIFY(folder.isValid());
        const QString helper = folder.filePath(u"qkvm-setup.exe"_s);
        QVERIFY(QFile::copy(bin.filePath(u"qkvm-setup.exe"_s), helper));
        for (const QString& runtime : bin.entryList({u"clang_rt.asan*.dll"_s}, QDir::Files))
            QVERIFY(QFile::copy(bin.filePath(runtime), folder.filePath(runtime))); // ASAN preset only
        QVERIFY(helper.contains(u'/') && helper.contains(u' '));
        QCOMPARE(launchSetup(helper.toStdWString(), {L"--check-helper"}, SetupHop::Relay, true), 0U);
    }
    void identityRefusalNeverSignalsReady_data()
    {
        QTest::addColumn<QString>("family");
        QTest::addColumn<QString>("image");
        const QString self = QCoreApplication::applicationFilePath().replace(u'/', u'\\');
        QTest::newRow("other family") << u"different_family"_s << self;
        QTest::newRow("other image") << QString() << u"C:\\not-the-test.exe"_s;
    }
    void identityRefusalNeverSignalsReady()
    {
        QFETCH(QString, family);
        QFETCH(QString, image);
        HealthExchange exchange(family.toStdWString(), image.toStdWString());
        std::atomic<bool> reported = true;
        std::jthread child([&] { reported = reportHealth(exchange.id(), 0); });
        const auto result = exchange.result(GetCurrentProcessId(), 500);
        child.join();
        QCOMPARE(result, uint(ERROR_TIMEOUT));
        QVERIFY(!reported);
    }
    void sharedIdentityIsReadOnceBeforeComparisons()
    {
        HealthExchange exchange({}, QCoreApplication::applicationFilePath().replace(u'/', u'\\').toStdWString());
        const std::wstring name = L"Local\\qKVM.Health." + exchange.id();
        const HANDLE mapping = OpenFileMappingW(FILE_MAP_WRITE | FILE_MAP_READ, FALSE, name.c_str());
        QVERIFY(mapping != nullptr);
        const auto close = qScopeGuard([&] { CloseHandle(mapping); });
        auto* shared = static_cast<detail::HealthReply*>(
            MapViewOfFile(mapping, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, sizeof(detail::HealthReply)));
        QVERIFY(shared != nullptr);
        const auto unmap = qScopeGuard([&] { UnmapViewOfFile(shared); });
        HealthIdentityApi api;
        api.family = [&](UINT32* size, PWSTR family) {
            // Deterministically change the shared mapping while the OS identity is
            // queried. Comparing the local snapshot must not re-read these strings.
            shared->family[0] = L'x';
            return GetCurrentPackageFamilyName(size, family);
        };
        std::atomic<bool> reported = false;
        std::jthread child([&] { reported = reportHealth(exchange.id(), 0, api); });
        const auto result = exchange.result(GetCurrentProcessId(), 500);
        child.join();
        QCOMPARE(result, 0U);
        QVERIFY(reported);
    }
    void readOnlySetupCheckTraversesTheRelay()
    {
        QProcess helper;
        helper.start(QCoreApplication::applicationDirPath() + u"/qkvm-setup.exe"_s,
                     {u"--check-helper"_s, u"--relay"_s});
        QVERIFY(helper.waitForFinished(10000));
        QCOMPARE(helper.exitStatus(), QProcess::NormalExit);
        QCOMPARE(helper.exitCode(), 0);
        QVERIFY(helper.readAllStandardOutput().contains("QKVM_RELAY_OK"));
    }
    void packageCheckCleanupReachesBothEnvironmentCopies()
    {
        const QList<QByteArray> names{"QT_QPA_PLATFORM", "QML2_IMPORT_PATH", "QSG_RENDER_LOOP", "QKVM_TEST_ENV",
                                      "PATH"};
        QHash<QByteArray, QByteArray> before;
        for (const auto& name : names) {
            before.insert(name, qgetenv(name.constData()));
            QVERIFY(qputenv(name.constData(), "synthetic-poison"));
        }
        const auto restore = qScopeGuard([&] {
            for (const auto& name : names) {
                if (before[name].isNull())
                    qunsetenv(name.constData());
                else
                    qputenv(name.constData(), before[name]);
            }
        });
        preparePackageCheckEnvironment();
        for (const auto& name : names) {
            std::array<wchar_t, 32768> value{};
            const DWORD size = GetEnvironmentVariableW(QString::fromLatin1(name).toStdWString().c_str(), value.data(),
                                                       static_cast<DWORD>(value.size()));
            if (name != "PATH") {
                QCOMPARE(size, 0UL);
                QVERIFY2(!qEnvironmentVariableIsSet(name.constData()), name.constData());
                QVERIFY(qgetenv(name.constData()).isEmpty());
                QVERIFY(qEnvironmentVariable(name.constData()).isEmpty());
            } else {
                QVERIFY(size > 0);
                QCOMPARE(qEnvironmentVariable("PATH"), QString::fromWCharArray(value.data()));
                QVERIFY(!qgetenv("PATH").contains("synthetic-poison"));
            }
        }
    }
    void aHealthReplyIsBoundToTheProcessAndPreservesTheError_data()
    {
        QTest::addColumn<uint>("error");
        QTest::newRow("healthy") << 0U;
        QTest::newRow("diagnostic failure") << 17U;
    }
    void aHealthReplyIsBoundToTheProcessAndPreservesTheError()
    {
        QFETCH(uint, error);
        QVERIFY(hasNoPackageIdentity());
        HealthExchange exchange({}, QCoreApplication::applicationFilePath().replace(u'/', u'\\').toStdWString());
        std::atomic<bool> reported = false;
        std::jthread child([&] { reported = reportHealth(exchange.id(), error); });
        QCOMPARE(exchange.result(GetCurrentProcessId(), 3000), error);
        child.join();
        QVERIFY(reported);
        QVERIFY(!reportHealth(exchange.id(), 0)); // One result per launch.
    }
    void aMissingReplyWrongIdentityAndWrongProcessCannotPass()
    {
        const auto executable = QCoreApplication::applicationFilePath().replace(u'/', u'\\').toStdWString();
        HealthExchange absent({}, executable);
        QCOMPARE(absent.result(GetCurrentProcessId(), 5), static_cast<uint>(ERROR_TIMEOUT));

        HealthExchange wrongProcess({}, executable);
        std::jthread child([&] { static_cast<void>(reportHealth(wrongProcess.id(), 0)); });
        QCOMPARE(wrongProcess.result(GetCurrentProcessId() + 1, 3000), static_cast<uint>(ERROR_INVALID_DATA));
        child.join();
        QVERIFY(!reportHealth(L"..\\arbitrary", 0));
    }
};
QTEST_GUILESS_MAIN(TestStartupCheck)
#include "test_startup_check.moc"
