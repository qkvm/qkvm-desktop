#include <QCryptographicHash>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "alerts_view.h"
#include "memory_secrets.h"

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
core::Status status(const QString& state)
{
    core::Status result;
    result.state = state;
    result.errorMessage = u"Private server reply that must not be persisted"_s;
    return result;
}
void unlock(ui::AppLock& lock)
{
    lock.submit({}, u"example-password"_s, u"example-password"_s);
    QTRY_VERIFY(!lock.busy());
    QVERIFY(!lock.restricted());
}
} // namespace

class TestAlertsView : public QObject {
    Q_OBJECT
private slots:
    void sourceTreeGroupsMachinesAndClearsPrivateInventory()
    {
        QTemporaryDir dir;
        core::testing::MemorySecrets secrets;
        ui::AppLock lock(secrets, fast, [] { return 1000; });
        unlock(lock);
        core::Incidents incidents(dir.filePath(u"history"_s));
        ui::AlertsView view(incidents, lock);
        view.watch(u"server-one"_s, false, u"First server"_s);
        auto first = machine(u"same-id"_s, u"Build <b>machine</b>"_s);
        first.group = u"Office"_s;
        view.inventoryFor(u"server-one"_s, {first, machine(u"ungrouped"_s, u"Z machine"_s)});
        view.watch(u"server-two"_s, false, u"Second server"_s);
        view.inventoryFor(u"server-two"_s, {first});
        const auto rows = view.treeSource(u"server-one"_s).value(u"rows"_s).toList();
        QCOMPARE(rows.size(), 4);
        QCOMPARE(rows.first().toMap().value(u"label"_s).toString(), u"Ungrouped"_s);
        QCOMPARE(rows.last().toMap().value(u"scope"_s).toString(), u"server-one"_s);
        QVERIFY(rows.last().toMap().value(u"key"_s) !=
                view.treeSource(u"server-two"_s).value(u"rows"_s).toList().last().toMap().value(u"key"_s));
        QSignalSpy changed(&view, &ui::AlertsView::changed);
        view.observeFor(u"server-one"_s, {{u"same-id"_s, status(u"online"_s)}}, true);
        QVERIFY(!changed.isEmpty());
        QCOMPARE(
            view.treeSource(u"server-one"_s).value(u"rows"_s).toList().last().toMap().value(u"status"_s).toString(),
            u"Online"_s);
        view.observeFor(u"server-one"_s, {}, false);
        QCOMPARE(
            view.treeSource(u"server-one"_s).value(u"rows"_s).toList().last().toMap().value(u"status"_s).toString(),
            u"Status unavailable"_s);
        view.unavailable(u"server-one"_s, u"First server"_s, u"Sign in to reconnect"_s);
        QVERIFY(view.treeSource(u"server-one"_s).value(u"rows"_s).toList().isEmpty());
        QVERIFY(!view.treeSource(u"server-two"_s).value(u"rows"_s).toList().isEmpty());
        lock.lock();
        QVERIFY(view.treeSource(u"server-two"_s).isEmpty());
    }

    void lockHidesHistoryAndStopsMutationsButMonitoringContinues()
    {
        QTemporaryDir dir;
        core::testing::MemorySecrets secrets;
        ui::AppLock lock(secrets, fast, [] { return 1000; });
        unlock(lock);
        core::Incidents incidents(dir.filePath(u"history"_s));
        ui::AlertsView view(incidents, lock);
        QSignalSpy notifications(&view, &ui::AlertsView::notification);
        view.context(u"pc"_s, true);
        view.inventory({machine(u"a"_s, u"<b>Machine</b>"_s)});
        for (int i = 0; i < 2; ++i)
            view.observe({{u"a"_s, status(u"online"_s)}}, true);
        for (int i = 0; i < 2; ++i)
            view.observe({{u"a"_s, status(u"auth"_s)}}, true);
        QCOMPARE(notifications.size(), 1);
        QCOMPARE(view.attention(), 1);
        const QString id = view.entries().front().toMap().value(u"id"_s).toString();
        lock.lock();
        QVERIFY(view.entries().isEmpty());
        QCOMPARE(view.attention(), 0);
        view.acknowledge(id);
        view.pause(true);
        QVERIFY(!incidents.entries(u"pc"_s).front().acknowledged);
        QVERIFY(!incidents.pausedUntil(u"pc"_s).isValid());
        for (int i = 0; i < 2; ++i)
            view.observe({{u"a"_s, status(u"online"_s)}}, true);
        QCOMPARE(notifications.size(), 1);
        QVERIFY(incidents.entries(u"pc"_s).front().closed.isValid());
        QVERIFY(incidents.flush());
        QFile file(dir.filePath(u"history"_s));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(!file.readAll().contains("Private server reply"));
    }

    void serverScopesAndRevokedMachineAccessHideOldHistory()
    {
        QTemporaryDir dir;
        core::testing::MemorySecrets secrets;
        ui::AppLock lock(secrets, fast, [] { return 1000; });
        unlock(lock);
        core::Incidents incidents(dir.filePath(u"history"_s));
        ui::AlertsView view(incidents, lock);
        view.context(u"server-one-account-one"_s, false);
        view.inventory({machine(u"a"_s, u"Restricted machine"_s)});
        for (int i = 0; i < 2; ++i)
            view.observe({{u"a"_s, status(u"offline"_s)}}, true);
        QCOMPARE(view.entries().size(), 1);
        const QString id = view.entries().front().toMap().value(u"id"_s).toString();
        view.inventory({});
        view.setHistory(true);
        QVERIFY(view.entries().isEmpty());
        view.acknowledge(id);
        QVERIFY(!incidents.entries(u"server-one-account-one"_s).front().acknowledged);
        view.context(u"server-one-account-two"_s, false);
        view.inventory({machine(u"a"_s, u"Same id, different access"_s)});
        QVERIFY(view.entries().isEmpty());
        view.context({}, false);
        QVERIFY(!view.allowed());
    }

    void serverOutageDoesNotMakeEveryCachedMachineOffline()
    {
        QTemporaryDir dir;
        QDateTime now = QDateTime::currentDateTimeUtc();
        core::testing::MemorySecrets secrets;
        ui::AppLock lock(secrets, fast, [] { return 1000; });
        unlock(lock);
        core::Incidents incidents(dir.filePath(u"history"_s), [&] { return now; });
        ui::AlertsView view(incidents, lock);
        view.context(u"server"_s, false);
        view.inventory({machine(u"a"_s, u"A"_s)});
        for (int i = 0; i < 2; ++i)
            view.observe({{u"a"_s, status(u"online"_s)}}, true);
        for (int i = 0; i < 2; ++i)
            view.observe({{u"a"_s, status(u"offline"_s)}}, false);
        QCOMPARE(view.entries().size(), 1);
        QCOMPARE(view.entries().front().toMap().value(u"name"_s).toString(), u"qKVM server"_s);
        view.observe({{u"a"_s, status(u"offline"_s)}}, true);
        QCOMPARE(view.entries().size(), 1);
        now = now.addSecs(121);
        for (int i = 0; i < 2; ++i)
            view.observe({{u"a"_s, status(u"online"_s)}}, true);
        QVERIFY(view.entries().isEmpty());
        view.setHistory(true);
        QCOMPARE(view.entries().size(), 1);
    }

    void acknowledgementMaintenanceAndServerHealthAreLocalChoices()
    {
        QTemporaryDir dir;
        core::testing::MemorySecrets secrets;
        ui::AppLock lock(secrets, fast, [] { return 1000; });
        unlock(lock);
        core::Incidents incidents(dir.filePath(u"history"_s));
        ui::AlertsView view(incidents, lock);
        view.context(u"pc"_s, true);
        view.inventory({machine(u"a"_s, u"A"_s)});
        core::Status health = status(u"online"_s);
        health.server = core::ServerReadings();
        health.server->health = u"Critical"_s;
        for (int i = 0; i < 2; ++i)
            view.observe({{u"a"_s, health}}, true);
        QCOMPARE(view.attention(), 1);
        const QString id = view.entries().front().toMap().value(u"id"_s).toString();
        view.pauseDevice(id, true);
        QCOMPARE(view.attention(), 0);
        view.pauseDevice(id, false);
        QCOMPARE(view.attention(), 1);
        view.acknowledge(id);
        QCOMPARE(view.attention(), 0);
        QVERIFY(view.entries().front().toMap().value(u"acknowledged"_s).toBool());
        lock.becameWallScreen();
        QVERIFY(view.entries().isEmpty());
    }
};
QTEST_GUILESS_MAIN(TestAlertsView)
#include "test_alerts_view.moc"
