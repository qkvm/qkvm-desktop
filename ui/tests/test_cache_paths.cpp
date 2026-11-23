#include <array>

#include <QtTest>

#include <QFile>
#include <QProcess>
#include <QTemporaryDir>

#include <qkvm/base/machine.h>

#include <windows.h>

#include <appmodel.h>

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
    // A package's family has no kind of Windows in it: what this build derives is the family of the package of either
    // kind (x64 or ARM64), and it is derived as this build's own kind.
    void theFamilyIsThatOfThePackageOfEitherKindOfWindows()
    {
        std::wstring name = L"qKVM.Desktop.Development";
        std::wstring publisher = L"CN=qKVM Development";
        const auto family = familyFor(name, publisher);
        for (const UINT32 architecture : {UINT32{PROCESSOR_ARCHITECTURE_AMD64}, UINT32{PROCESSOR_ARCHITECTURE_ARM64}}) {
            PACKAGE_ID id{};
            id.name = name.data();
            id.publisher = publisher.data();
            id.processorArchitecture = architecture;
            std::array<wchar_t, PACKAGE_FAMILY_NAME_MAX_LENGTH + 1> made{};
            UINT32 count = static_cast<UINT32>(made.size());
            QCOMPARE(PackageFamilyNameFromId(&id, &count, made.data()), LONG{ERROR_SUCCESS});
            QCOMPARE(std::wstring(made.data()), family);
        }
        QCOMPARE(familyArchitecture(), UINT32{qkvm::base::programArchitecture() == qkvm::base::Architecture::Arm64
                                                  ? PROCESSOR_ARCHITECTURE_ARM64
                                                  : PROCESSOR_ARCHITECTURE_AMD64});
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
