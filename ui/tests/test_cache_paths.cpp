#include <QtTest>

#include <QFile>
#include <QProcess>
#include <QTemporaryDir>

#include "../win/cache_paths.h"

using namespace qkvm::storage;
namespace fs = std::filesystem;

class TestCachePaths : public QObject {
    Q_OBJECT
private slots:
    void installedCachesBelongToThePackageAndSettingsStayOutside()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const fs::path base(directory.path().toStdWString());
        const auto family = familyFor(L"qKVM.Desktop.Development", L"CN=qKVM Development");
        const auto owned = base / L"Packages" / family / L"LocalCache" / L"qKVM";
        for (const auto area : {Area::Downloads, Area::Helpers, Area::Installer, Area::Diagnostics}) {
            const auto path = cachePath(base, family, area);
            QCOMPARE(path.parent_path(), owned);
            prepareDirectory(path);
            QVERIFY(fs::is_directory(path));
        }
        const auto settings = base / L"qKVM" / L"settings.json";
        QVERIFY(settings.parent_path() != owned);
        QVERIFY(cachePath(base, {}, Area::Diagnostics).parent_path() == base / L"qKVM" / L"Cache");
    }
    void invalidFamiliesAndRedirectedFoldersAreRefused()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const fs::path base(directory.path().toStdWString());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, cachePath(base, L"../outside", Area::Installer));
        const auto target = base / L"outside";
        fs::create_directory(target);
        const auto link = base / L"redirected";
        QProcess command;
        command.start(QStringLiteral("cmd.exe"), {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
                                                  QDir::toNativeSeparators(QString::fromStdWString(link.wstring())),
                                                  QDir::toNativeSeparators(QString::fromStdWString(target.wstring()))});
        QVERIFY(command.waitForFinished(10000));
        QVERIFY2(command.exitCode() == 0, command.readAllStandardError().constData());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, prepareDirectory(link / L"should-not-exist"));
        QVERIFY(!fs::exists(target / L"should-not-exist"));
        // Remove only the test junction itself; never recursively traverse its target.
        QVERIFY(fs::remove(link));
    }
};
QTEST_GUILESS_MAIN(TestCachePaths)
#include "test_cache_paths.moc"
