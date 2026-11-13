#include <functional>
#include <memory>
#include <utility>

#include <QAbstractItemModelTester>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>

#include <qkvm/core/discovery.h>

#include "actions_view.h"
#include "add_kvm_view.h"
#include "discovery_view.h"

using namespace Qt::StringLiterals;
using namespace qkvm;

namespace {

// Nothing in these tests can open a socket: even a complete search is answered by this function.
struct Probes {
    struct Pending {
        QUrl address;
        QPointer<QObject> lifetime;
        std::function<void(core::KvmProbe::Found)> done;
    };
    QList<Pending> pending;

    core::Discovery::Probe asking()
    {
        return [this](const QUrl& address, QObject* lifetime, std::function<void(core::KvmProbe::Found)> done) {
            pending.append({address, lifetime, std::move(done)});
        };
    }

    void answer(int index, core::KvmProbe::Kind kind)
    {
        Pending one = pending.at(index);
        if (one.lifetime != nullptr) {
            core::KvmProbe::Found found;
            found.kind = kind;
            one.done(found);
        }
    }
};

QList<core::Discovery::Network> interfaces()
{
    core::Discovery::Network ethernet;
    ethernet.interfaceName = u"ethernet0"_s;
    ethernet.label = u"Ethernet"_s;
    ethernet.address = u"192.168.40.1"_s;
    ethernet.cidr = u"192.168.40.0/30"_s;
    ethernet.prefixLength = 30;
    ethernet.hostCount = 2;
    core::Discovery::Network vpn = ethernet;
    vpn.interfaceName = u"vpn0"_s;
    vpn.label = u"Work VPN"_s;
    vpn.address = u"10.0.0.1"_s;
    vpn.cidr = u"10.0.0.0/8"_s;
    vpn.prefixLength = 8;
    vpn.hostCount = 16'777'214;
    return {ethernet, vpn};
}

struct Desk {
    Probes probes;
    core::Discovery scanner{probes.asking()};
    ui::DiscoveryView view{scanner, interfaces()};
    QAbstractItemModelTester model{&view, QAbstractItemModelTester::FailureReportingMode::QtTest};

    void open()
    {
        view.prepare();
        view.setActive(true);
    }
};

QQuickItem* itemNamed(QQuickItem* under, const QString& name)
{
    if (under->objectName() == name) {
        return under;
    }
    for (QQuickItem* child : under->childItems()) {
        if (QQuickItem* found = itemNamed(child, name)) {
            return found;
        }
    }
    return nullptr;
}

} // namespace

class TestDiscoveryView : public QObject {
    Q_OBJECT

private slots:
    void openingNeverSelectsOrSearchesAnyNetwork()
    {
        Desk desk;
        QVERIFY(desk.probes.pending.isEmpty());
        desk.open();
        QCOMPARE(desk.view.networks().size(), 2);
        for (const QVariant& row : desk.view.networks()) {
            QVERIFY(!row.toMap().value(u"selected"_s).toBool());
        }
        desk.view.start();
        QVERIFY(!desk.view.busy());
        QVERIFY(!desk.view.problem().isEmpty());
        QVERIFY(desk.probes.pending.isEmpty());
        // Even after a previous selection, reopening starts with no networks selected.
        desk.view.selectNetwork(0, true);
        desk.view.setManualScope(u"192.168.40.10"_s);
        desk.view.prepare();
        QVERIFY(desk.view.manualScope().isEmpty());
        QVERIFY(!desk.view.networks().at(0).toMap().value(u"selected"_s).toBool());
        QVERIFY(desk.probes.pending.isEmpty());
    }

    void resultsArriveIncrementallyAndOnlyAFamilyThisProgramAddsCanSupplyAnAddress()
    {
        Desk desk;
        desk.open();
        desk.view.setManualScope(u"192.168.40.10-192.168.40.12"_s);
        desk.view.start();
        QVERIFY(desk.view.busy());
        QCOMPARE(desk.view.total(), 3);
        QTRY_COMPARE(desk.probes.pending.size(), 3);
        const QSignalSpy chosen(&desk.view, &ui::DiscoveryView::addressChosen);
        desk.probes.answer(0, core::KvmProbe::Kind::JetKvm);
        QTRY_COMPARE(desk.view.count(), 1);
        QTRY_COMPARE(desk.view.completed(), 1);
        QVERIFY(desk.view.busy());
        // A JetKVM is one this program adds by itself, as a kvmd is.
        QCOMPARE(desk.view.index(0).data(ui::DiscoveryView::FamilyRole).toString(), u"jetkvm"_s);
        QVERIFY(desk.view.index(0).data(ui::DiscoveryView::SupportedRole).toBool());
        desk.probes.answer(1, core::KvmProbe::Kind::Redfish);
        desk.probes.answer(2, core::KvmProbe::Kind::Kvmd);
        QTRY_VERIFY(!desk.view.busy());
        QCOMPARE(desk.view.state(), u"complete"_s);
        QCOMPARE(desk.view.count(), 3);
        // A server's controller too, from M7 1b-i on: this program adds one by itself.
        QCOMPARE(desk.view.index(1).data(ui::DiscoveryView::FamilyRole).toString(), u"redfish"_s);
        QVERIFY(desk.view.index(1).data(ui::DiscoveryView::SupportedRole).toBool());
        QVERIFY(desk.view.index(2).data(ui::DiscoveryView::SupportedRole).toBool());
        desk.view.choose(-1);
        desk.view.choose(99);
        QVERIFY(chosen.isEmpty());
        // Each with the family of what answered there.
        desk.view.choose(2);
        QCOMPARE(chosen.count(), 1);
        QCOMPARE(chosen.at(0).at(0).toString(), u"https://192.168.40.12"_s);
        QCOMPARE(chosen.at(0).at(1).toString(), u"kvmd"_s);
        desk.view.choose(0);
        QCOMPARE(chosen.count(), 2);
        QCOMPARE(chosen.at(1).at(0).toString(), u"https://192.168.40.10"_s);
        QCOMPARE(chosen.at(1).at(1).toString(), u"jetkvm"_s);
        desk.view.choose(1);
        QCOMPARE(chosen.count(), 3);
        QCOMPARE(chosen.at(2).at(0).toString(), u"https://192.168.40.11"_s);
        QCOMPARE(chosen.at(2).at(1).toString(), u"redfish"_s);
        // Selection supplies no credential or trusted certificate and invokes no add operation.
        desk.view.setActive(false);
        desk.view.choose(2);
        QCOMPARE(chosen.count(), 3);
    }

    void selectedInterfacesUseTheirActualPrefixAndNeverImplicitlyIncludeVpn()
    {
        Desk desk;
        desk.open();
        desk.view.selectNetwork(0, true);
        desk.view.start();
        QTRY_COMPARE(desk.probes.pending.size(), 2);
        QCOMPARE(desk.view.total(), 2);
        for (const auto& pending : desk.probes.pending) {
            QVERIFY(pending.address.host().startsWith(u"192.168.40."));
        }
        desk.view.stop();
        desk.view.selectNetwork(1, true);
        desk.view.start();
        QVERIFY(!desk.view.busy());
        QVERIFY(!desk.view.problem().isEmpty());
        QCOMPARE(desk.probes.pending.size(), 2);
    }

    void aLargeSearchNeedsReviewAndAnyScopeChangeInvalidatesIt()
    {
        Desk desk;
        desk.open();
        desk.view.setManualScope(u"192.168.40.0/23"_s);
        desk.view.start(true);
        QVERIFY(desk.view.confirmationNeeded());
        QCOMPARE(desk.view.planned(), 510);
        QVERIFY(desk.probes.pending.isEmpty());
        desk.view.setManualScope(u"192.168.42.0/23"_s);
        QVERIFY(!desk.view.confirmationNeeded());
        desk.view.start(true);
        QVERIFY(desk.view.confirmationNeeded());
        QVERIFY(!desk.view.busy());
        desk.view.start(true);
        QVERIFY(desk.view.busy());
        QVERIFY(!desk.view.confirmationNeeded());
        QTRY_VERIFY(!desk.probes.pending.isEmpty());
        desk.view.stop();
        QVERIFY(!desk.view.busy());
    }

    void aBrokenTlsInstallationShowsItsProblemWithoutPretendingTheSearchCompleted()
    {
        Desk desk;
        desk.open();
        desk.view.setManualScope(u"192.168.40.10"_s);
        desk.view.start();
        QTRY_COMPARE(desk.probes.pending.size(), 1);
        desk.probes.answer(0, core::KvmProbe::Kind::NotAsked);
        QTRY_VERIFY(!desk.view.busy());
        QCOMPARE(desk.view.state(), u"failed"_s);
        QVERIFY(!desk.view.problem().isEmpty());
        QCOMPARE(desk.view.completed(), 0);
        QCOMPARE(desk.probes.pending.size(), 1);
    }

    void hidingClosingAndDestroyingTheViewCancelOutstandingWork()
    {
        Desk desk;
        desk.open();
        desk.view.setManualScope(u"192.168.40.10"_s);
        desk.view.start();
        QTRY_COMPARE(desk.probes.pending.size(), 1);
        desk.view.setActive(false);
        QVERIFY(!desk.view.busy());
        QCOMPARE(desk.view.state(), u"stopped"_s);
        QVERIFY(desk.probes.pending.at(0).lifetime.isNull());
        desk.probes.answer(0, core::KvmProbe::Kind::Kvmd);
        QCOMPARE(desk.view.count(), 0);
        desk.view.start();
        QVERIFY(!desk.view.busy());
        desk.view.closed();
        QCOMPARE(desk.view.state(), u"idle"_s);
        QCOMPARE(desk.view.total(), 0);
        QVERIFY(desk.view.manualScope().isEmpty());
        {
            ui::DiscoveryView another(desk.scanner, interfaces());
            another.prepare();
            another.setActive(true);
            another.setManualScope(u"192.168.40.20"_s);
            another.start();
            QTRY_COMPARE(desk.probes.pending.size(), 2);
        }
        QVERIFY(!desk.scanner.running());
        QVERIFY(desk.probes.pending.at(1).lifetime.isNull());
    }

    void screenSearchStopAndCloseUseTheSameLifecycle()
    {
        Desk desk;
        QQmlEngine engine;
        const QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            import Qkvm
            Window {
                required property DiscoveryView view
                width: 800; height: 700
                DiscoverKvms {
                    objectName: "discoverBox"
                    discovery: view
                }
            }
        )",
                          QUrl());
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> root(
            component.createWithInitialProperties({{u"view"_s, QVariant::fromValue(&desk.view)}}));
        auto* window = qobject_cast<QQuickWindow*>(root.get());
        QVERIFY(window != nullptr);
        window->show();
        QObject* box = window->findChild<QObject*>(u"discoverBox"_s);
        QVERIFY(box != nullptr);
        QVERIFY(QMetaObject::invokeMethod(box, "open"));
        QTRY_VERIFY(box->property("opened").toBool());
        desk.view.setActive(true);
        QVERIFY(desk.probes.pending.isEmpty());
        desk.view.setManualScope(u"192.168.40.10"_s);
        QQuickItem* start = itemNamed(window->contentItem(), u"discoveryStart"_s);
        QVERIFY(start != nullptr);
        QVERIFY(QMetaObject::invokeMethod(start, "clicked"));
        QTRY_COMPARE(desk.probes.pending.size(), 1);
        QQuickItem* stop = itemNamed(window->contentItem(), u"discoveryStop"_s);
        QVERIFY(stop != nullptr);
        QVERIFY(stop->isVisible());
        QVERIFY(QMetaObject::invokeMethod(stop, "clicked"));
        QVERIFY(!desk.view.busy());
        QVERIFY(QMetaObject::invokeMethod(start, "clicked"));
        QTRY_COMPARE(desk.probes.pending.size(), 2);
        QVERIFY(QMetaObject::invokeMethod(box, "close"));
        QTRY_VERIFY(!box->property("opened").toBool());
        QVERIFY(!desk.view.busy());
        QCOMPARE(desk.view.state(), u"idle"_s);
        QVERIFY(warnings.isEmpty());
    }

    void choosingPrefillsTheExistingAddFormWithoutAddingOrRememberingTrust()
    {
        ui::AddKvmView adding;
        ui::ActionsView actions;
        const QSignalSpy addAsked(&adding, &ui::AddKvmView::addAsked);
        QQmlEngine engine;
        const QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            import Qkvm
            Window {
                id: window
                required property AddKvmView adding
                required property ActionsView actions
                width: 800; height: 700
                AddKvm {
                    objectName: "addBox"
                    adding: window.adding
                    actions: window.actions
                }
            }
        )",
                          QUrl());
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> root(component.createWithInitialProperties(
            {{u"adding"_s, QVariant::fromValue(&adding)}, {u"actions"_s, QVariant::fromValue(&actions)}}));
        auto* window = qobject_cast<QQuickWindow*>(root.get());
        QVERIFY(window != nullptr);
        window->show();
        QObject* box = window->findChild<QObject*>(u"addBox"_s);
        QVERIFY(box != nullptr);
        QVERIFY(QMetaObject::invokeMethod(box, "open"));
        QTRY_VERIFY(box->property("opened").toBool());
        auto* discovery = box->findChild<ui::DiscoveryView*>();
        QVERIFY(discovery != nullptr);
        // The result-selection gate is exercised above; this checks the QML handoff only.
        emit discovery->addressChosen(u"https://192.168.40.12"_s, u"kvmd"_s);
        QObject* address = box->findChild<QObject*>(u"kvmAddress"_s);
        auto* user = box->findChild<QQuickItem*>(u"kvmUser"_s);
        QVERIFY(address != nullptr && user != nullptr);
        QCOMPARE(address->property("text").toString(), u"https://192.168.40.12"_s);
        QVERIFY(user->isVisible());
        QVERIFY(addAsked.isEmpty());
        QVERIFY(adding.certificate().isEmpty());
        // A JetKVM has no user name: while its address stands in the box, there is no line for one,
        // and none is sent.
        user->setProperty("text", u"root"_s);
        emit discovery->addressChosen(u"https://192.168.40.10"_s, u"jetkvm"_s);
        QCOMPARE(address->property("text").toString(), u"https://192.168.40.10"_s);
        QVERIFY(!user->isVisible());
        QVERIFY(QMetaObject::invokeMethod(box, "submit"));
        QCOMPARE(addAsked.count(), 1);
        QCOMPARE(addAsked.constFirst().at(0).toString(), u"https://192.168.40.10"_s);
        QCOMPARE(addAsked.constFirst().at(1).toString(), QString());
        // Another address typed: what is there is not known, and the line is there again.
        address->setProperty("text", u"192.168.40.11"_s);
        QVERIFY(user->isVisible());
        QVERIFY(warnings.isEmpty());
    }
};

QTEST_MAIN(TestDiscoveryView)
#include "test_discovery_view.moc"
