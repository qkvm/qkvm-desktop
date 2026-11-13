#include <QCryptographicHash>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "alerts_view.h"
#include "memory_secrets.h"
#include "pretend_tray.h"
#include "quick_access.h"
#include "tray_keeper.h"

using namespace Qt::StringLiterals;
using namespace qkvm;

namespace {
base::Result<QByteArray> fast(QByteArrayView text, QByteArrayView salt, const std::stop_token&)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(text);
    hash.addData(salt);
    return hash.result();
}
core::Machine machine(const QString& id, const QString& name)
{
    core::Machine result;
    result.id = id;
    result.name = name;
    return result;
}
struct Desk {
    QTemporaryDir folder;
    core::Settings settings{folder.filePath(u"settings.json"_s)};
    core::testing::MemorySecrets secrets;
    ui::AppLock lock{secrets, fast, [] { return 1000; }};
    core::Incidents incidents{folder.filePath(u"incidents"_s)};
    ui::AlertsView sources{incidents, lock};
    ui::QuickAccess shortcuts{settings, sources, lock};
    void unlock()
    {
        lock.submit({}, u"example-password"_s, u"example-password"_s);
        QTRY_VERIFY(!lock.busy());
        QVERIFY(!lock.restricted());
    }
    void inventory(const QString& scope, const QString& name, const QList<core::Machine>& machines)
    {
        sources.watch(scope, scope == u"pc", name);
        sources.inventoryFor(scope, machines);
    }
};
ui::TrayIcon::Row submenu(const ui::PretendedTray& tray, const QString& name)
{
    for (const auto& row : tray.menu)
        if (row.text == name)
            return row;
    return {};
}
} // namespace

class TestQuickAccess : public QObject {
    Q_OBJECT
private slots:
    void choicesSurviveRestartWithoutSavingNamesAndStayWithTheirAccount()
    {
        Desk desk;
        desk.unlock();
        const QString first = u"server-"_s + QString(64, u'a');
        const QString second = u"server-"_s + QString(64, u'b');
        desk.inventory(first, u"First source"_s, {machine(u"a"_s, u"Private machine name"_s)});
        desk.inventory(second, u"Second source"_s, {machine(u"a"_s, u"Other account machine"_s)});
        desk.shortcuts.toggleFavorite({first, u"a"_s});
        desk.shortcuts.opened({first, u"a"_s});
        desk.shortcuts.opened({second, u"a"_s});
        QCOMPARE(desk.shortcuts.favorites().size(), 1);
        QCOMPARE(desk.shortcuts.recent().size(), 2);
        QFile file(desk.folder.filePath(u"settings.json"_s));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray bytes = file.readAll();
        QVERIFY(!bytes.contains("Private machine name"));
        QVERIFY(!bytes.contains("First source"));
        core::Settings reloaded(desk.folder.filePath(u"settings.json"_s));
        ui::QuickAccess restarted(reloaded, desk.sources, desk.lock);
        QCOMPARE(restarted.favorites(), desk.shortcuts.favorites());
        QCOMPARE(restarted.recent(), desk.shortcuts.recent());
        desk.sources.suspend(first);
        QVERIFY(restarted.favorites().isEmpty());
        QCOMPARE(restarted.recent().size(), 1);
        QCOMPARE(restarted.recent().front().reference.scope, second);
        desk.inventory(first, u"First source"_s, {machine(u"a"_s, u"Renamed machine"_s)});
        QCOMPARE(restarted.favorites().front().name, u"Renamed machine"_s);
        desk.sources.inventoryFor(first, {});
        QVERIFY(restarted.favorites().isEmpty());
        QCOMPARE(reloaded.remembered().favorites.size(), 1);
    }

    void recentMachinesAreBoundedDeduplicatedAndOnlyExplicitlyOpened()
    {
        Desk desk;
        desk.unlock();
        QList<core::Machine> machines;
        for (int i = 0; i < 12; ++i)
            machines.append(machine(QString::number(i), u"Machine %1"_s.arg(i)));
        desk.inventory(u"pc"_s, u"This PC"_s, machines);
        QVERIFY(desk.shortcuts.recent().isEmpty());
        desk.shortcuts.toggleFavorite({u"pc"_s, u"0"_s});
        for (int i = 0; i < 12; ++i)
            desk.shortcuts.opened({u"pc"_s, QString::number(i)});
        QCOMPARE(desk.shortcuts.recent().size(), 10);
        QCOMPARE(desk.shortcuts.recent().front().reference.machine, u"11"_s);
        QCOMPARE(desk.shortcuts.recent().back().reference.machine, u"2"_s);
        desk.shortcuts.opened({u"pc"_s, u"5"_s});
        QCOMPARE(desk.shortcuts.recent().size(), 10);
        QCOMPARE(desk.shortcuts.recent().front().reference.machine, u"5"_s);
        QSignalSpy changed(&desk.shortcuts, &ui::QuickAccess::changed);
        desk.shortcuts.opened({u"pc"_s, u"5"_s});
        desk.shortcuts.opened({u"pc"_s, u"unknown"_s});
        QCOMPARE(changed.size(), 0);
        desk.shortcuts.clearRecent();
        QVERIFY(desk.shortcuts.recent().isEmpty());
        QCOMPARE(desk.shortcuts.favorites().size(), 1);
    }

    void favoriteLimitAndStorageFailureAreExplained()
    {
        Desk desk;
        desk.unlock();
        QList<core::Machine> machines;
        for (int i = 0; i < 33; ++i)
            machines.append(machine(QString::number(i), u"Machine %1"_s.arg(i)));
        desk.inventory(u"pc"_s, u"This PC"_s, machines);
        QSignalSpy problems(&desk.shortcuts, &ui::QuickAccess::problem);
        for (int i = 0; i < 33; ++i)
            desk.shortcuts.toggleFavorite({u"pc"_s, QString::number(i)});
        QCOMPARE(desk.shortcuts.favorites().size(), 32);
        QCOMPARE(problems.size(), 1);
        desk.shortcuts.toggleFavorite({u"pc"_s, u"0"_s});
        desk.shortcuts.toggleFavorite({u"pc"_s, u"32"_s});
        QVERIFY(desk.shortcuts.isFavorite({u"pc"_s, u"32"_s}));
        core::Settings unwritable(desk.folder.path()); // A directory cannot be replaced with settings.
        ui::QuickAccess temporary(unwritable, desk.sources, desk.lock);
        QSignalSpy failure(&temporary, &ui::QuickAccess::problem);
        temporary.toggleFavorite({u"pc"_s, u"0"_s});
        QCOMPARE(failure.size(), 1);
        QCOMPARE(temporary.favorites().size(), 1);
    }

    void trayChoicesKeepTheirTargetAcrossRefreshAndHideWhileLocked()
    {
        Desk desk;
        desk.unlock();
        desk.inventory(u"pc"_s, u"This PC"_s, {machine(u"a"_s, u"Machine A"_s), machine(u"b"_s, u"Machine B"_s)});
        ui::SessionView session;
        ui::WallView wall;
        ui::PretendedTray tray;
        ui::TrayKeeper keeper(tray, session, wall, desk.settings);
        keeper.useQuickAccess(desk.shortcuts);
        QVERIFY(!submenu(tray, u"Favorites"_s).children.front().enabled);
        desk.shortcuts.toggleFavorite({u"pc"_s, u"a"_s});
        desk.shortcuts.opened({u"pc"_s, u"a"_s});
        const auto snapshot = tray.menu;
        const auto favorite = submenu(tray, u"Favorites"_s).children.front();
        QVERIFY(favorite.text.contains(u"Machine A"));
        QVERIFY(favorite.text.contains(u"This PC"));
        desk.shortcuts.toggleFavorite({u"pc"_s, u"a"_s});
        desk.shortcuts.toggleFavorite({u"pc"_s, u"b"_s});
        QSignalSpy chosen(&tray, &ui::TrayIcon::shortcutChosen);
        tray.chooseSnapshot(snapshot, favorite.id);
        QCOMPARE(chosen.size(), 1);
        QCOMPARE(chosen.front().at(0).toString(), u"pc"_s);
        QCOMPARE(chosen.front().at(1).toString(), u"a"_s);
        desk.lock.lock();
        QVERIFY(!desk.shortcuts.allowed());
        QVERIFY(!tray.rows().contains(u"Favorites"_s));
        QVERIFY(!tray.rows().contains(u"Recent machines"_s));
        QVERIFY(desk.shortcuts.favorites().isEmpty());
        desk.shortcuts.toggleFavorite({u"pc"_s, u"b"_s});
        desk.shortcuts.opened({u"pc"_s, u"b"_s});
        tray.chooseSnapshot(snapshot, ui::TrayKeeper::ClearFavorites);
        tray.chooseSnapshot(snapshot, ui::TrayKeeper::ClearRecent);
        QCOMPARE(desk.settings.remembered().favorites.size(), 1);
        QCOMPARE(desk.settings.remembered().recentMachines.front().machine, u"a"_s);
        desk.lock.submit(u"example-password"_s, {}, {});
        QTRY_VERIFY(!desk.lock.restricted());
        QVERIFY(tray.rows().contains(u"Favorites"_s));
        tray.chooseSnapshot(tray.menu, ui::TrayKeeper::ClearRecent);
        QVERIFY(desk.shortcuts.recent().isEmpty());
        QCOMPARE(desk.shortcuts.favorites().size(), 1);
        desk.lock.becameWallScreen();
        QVERIFY(!tray.rows().contains(u"Favorites"_s));
        desk.shortcuts.clearFavorites();
        QCOMPARE(desk.settings.remembered().favorites.size(), 1);
    }
};

QTEST_MAIN(TestQuickAccess)
#include "test_quick_access.moc"
