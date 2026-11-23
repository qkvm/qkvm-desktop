#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <QVariantMap>

#include <qkvm/core/proxmox_device.h>
#include <qkvm/core/settings.h>

#include "add_kvm_view.h"
#include "edit_kvm_view.h"
#include "machine_menu.h"
#include "memory_secrets.h"
#include "own_wall.h"
#include "proxmox_by_hand.h"

using namespace Qt::StringLiterals;
namespace core = qkvm::core;
using core::testing::MemorySecrets;
using core::testing::ProxmoxByHand;
using qkvm::ui::AddKvmView;
using qkvm::ui::OwnWall;

namespace {

core::testing::Tls proxmoxTls(const QString& name = u"first"_s)
{
    const QString folder = QStringLiteral(QKVM_TESTDATA "/tls/");
    return {folder + name + u".crt"_s, folder + name + u".key"_s};
}

core::KvmAdding::Pace quick()
{
    core::KvmAdding::Pace pace;
    pace.probe.connectWithinMs = 5000;
    pace.probe.quietMs = 5000;
    pace.probe.withinMs = 10'000;
    pace.driver.connectWithinMs = 5000;
    pace.driver.quietMs = 5000;
    pace.driver.transferMs = 1500;
    pace.driver.holdMs = 400;
    pace.driver.freshMs = 200;
    pace.driver.pacing = std::make_shared<core::SignInPacing>();
    return pace;
}

template <typename Answer>
std::optional<Answer> answerOf(const std::function<void(std::function<void(qkvm::base::Result<Answer>)>)>& ask)
{
    std::optional<qkvm::base::Result<Answer>> came;
    ask([&came](qkvm::base::Result<Answer> answer) { came = std::move(answer); });
    static_cast<void>(QTest::qWaitFor([&came] { return came.has_value(); }, 20'000));
    const qkvm::base::Result<Answer> answer =
        came.value_or(qkvm::base::Result<Answer>(qkvm::base::Error{"never", "It was never answered."}));
    if (!answer) {
        return std::nullopt;
    }
    return answer.value();
}

} // namespace

// The box that adds to this PC's own wall, for virtual machines on Proxmox VE (wave 2): a token's guests listed to
// tick, the one the address names added at once, the ticked ones added one after the other, each said; as the page's
// "Virtual machines on Proxmox VE" (app.js addDevicesForm, fromProxmox). Against a Proxmox by hand.
class TestOwnWallProxmox : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // The drivers choose Windows' own TLS, which is refused once TLS is in use.
        const core::ProxmoxDevice first(core::ProxmoxDevice::Where{}, QString(), core::ProxmoxDevice::Pace{});
    }

    void aBoxListsAProxmoxsGuestsAndAddsTheTickedOnes()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxByHand::Guest db;
        db.name = u"db"_s;
        db.status = u"stopped"_s;
        pve.guests.insert(101, db);
        pve.containers = {300};
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        OwnWall own(settings, secrets, quick());
        AddKvmView box;
        own.serve(box);
        const QString address = pve.address().toString();
        box.listGuests(address, pve.tokenId, pve.secret, u"lab"_s);
        QVERIFY(box.busy());
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20'000);
        QVERIFY2(box.problem().isEmpty(), qPrintable(box.problem()));
        QCOMPARE(box.guests().size(), 2);
        QCOMPARE(box.containers(), 1);
        const QVariantMap first = box.guests().at(0).toMap();
        QCOMPARE(first.value(u"vmid"_s).toLongLong(), qint64{100});
        QCOMPARE(first.value(u"name"_s).toString(), u"web"_s);
        QCOMPARE(first.value(u"node"_s).toString(), u"pve1"_s);
        QVERIFY(first.value(u"taken"_s).toString().isEmpty());
        box.addGuests(address, pve.tokenId, pve.secret, u"lab"_s, {100, 101});
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20'000);
        QCOMPARE(box.count(), 2);
        QCOMPARE(own.wall().kvms().size(), 2);
        QVERIFY2(box.added().startsWith(u"\"web\" added (VM 100 on pve1 Proxmox VE 9.2.21)"_s),
                 qPrintable(box.added()));
        QVERIFY(!box.certificate().isEmpty());
        // Listed again, both are on the wall: neither may be ticked.
        box.listGuests(address, pve.tokenId, pve.secret, u"lab"_s);
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20'000);
        QVERIFY(box.guests().at(0).toMap().value(u"taken"_s).toString().startsWith(u"on the wall as"_s));
        QVERIFY(box.guests().at(1).toMap().value(u"taken"_s).toString().startsWith(u"on the wall as"_s));
        // What was listed is what is added from: another Proxmox typed meanwhile adds nothing.
        box.addGuests(u"https://127.0.0.2:8006"_s, pve.tokenId, pve.secret, u"lab"_s, {100});
        QVERIFY(box.problem().startsWith(u"List the Proxmox's virtual machines again"_s));
        QCOMPARE(own.wall().kvms().size(), 2);
    }

    // ...and as a run of its own (review c9b-4, L7, as 74c6b0c5's rule has it for a run of the ticked ones): after an
    // earlier run of the box, only its own line is said.
    void anAddressThatNamesAGuestAddsItAtOnce()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxByHand::Guest db;
        db.name = u"db"_s;
        db.status = u"stopped"_s;
        pve.guests.insert(101, db);
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        OwnWall own(settings, secrets, quick());
        AddKvmView box;
        own.serve(box);
        box.listGuests(pve.address().toString(), pve.tokenId, pve.secret, QString());
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20'000);
        box.addGuests(pve.address().toString(), pve.tokenId, pve.secret, QString(), {101});
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20'000);
        QVERIFY2(box.added().startsWith(u"\"db\" added"_s), qPrintable(box.added()));
        box.listGuests(pve.address().toString() + u"/100"_s, pve.tokenId, pve.secret, QString());
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20'000);
        QCOMPARE(box.count(), 2);
        QCOMPARE(own.wall().kvms().value(1).vmid, qint64{100});
        QVERIFY2(box.added().startsWith(u"\"web\" added"_s) && !box.added().contains(u'\n'), qPrintable(box.added()));
    }

    // After the rebase onto the VNC servers: where Proxmox's guests are chosen, what is typed is a Proxmox's address,
    // whatever it begins with (the box passes it as it was typed: test_ui_box_screens), as the page asks a Proxmox's
    // guests of it. A VNC server's is none: refused in the listing's words before anything is sent, and nothing added.
    void aVncServersAddressIsNoProxmoxsWhereItsGuestsAreChosen()
    {
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        OwnWall own(settings, secrets, quick());
        AddKvmView box;
        own.serve(box);
        for (const QString& typed : {u"vnc://127.0.0.1:5901"_s, u"vncs://127.0.0.1:5901"_s}) {
            box.listGuests(typed, u"qkvm@pve!wall"_s, u"6c1d0e2f-8a3b-4c5d-9e7f-0a1b2c3d4e5f"_s, QString());
            QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 5000);
            QVERIFY2(box.problem().contains(u"spoken over HTTPS only"_s), qPrintable(box.problem()));
            QVERIFY(box.guests().isEmpty());
            QCOMPARE(box.count(), 0);
        }
        QVERIFY(own.wall().kvms().isEmpty());
    }

    // Its card and its menu offer its virtual media by what it can (`media` in `can`), as the page's Media... (the
    // web's `canDo(single, 'media')`); the box itself is MediaView's, as for any machine with media.
    void aGuestsMenuOffersItsMediaByWhatItCan()
    {
        ProxmoxByHand pve(proxmoxTls());
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        OwnWall own(settings, secrets, quick());
        AddKvmView box;
        own.serve(box);
        box.listGuests(pve.address().toString() + u"/100"_s, pve.tokenId, pve.secret, QString());
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20'000);
        QCOMPARE(box.count(), 1);
        core::LocalWall& wall = own.wall();
        const QString id = wall.kvms().value(0).id;
        std::optional<core::Status> status;
        const auto canHaveMedia = [&wall, &id, &status] {
            const auto all = answerOf<QHash<QString, core::Status>>(
                [&wall](std::function<void(qkvm::base::Result<QHash<QString, core::Status>>)> done) {
                    wall.askStatus(std::move(done));
                });
            status = all ? std::optional<core::Status>(all->value(id)) : std::nullopt;
            return status && status->capabilities && status->capabilities->contains(u"media"_s);
        };
        QTRY_VERIFY_WITH_TIMEOUT(canHaveMedia(), 10'000);
        const auto shown = answerOf<core::Wall>(
            [&wall](std::function<void(qkvm::base::Result<core::Wall>)> done) { wall.askWall(std::move(done)); });
        QVERIFY(shown);
        const core::Wall seen = shown.value_or(core::Wall{});
        const auto machine =
            std::ranges::find_if(seen.machines, [&id](const core::Machine& one) { return one.id == id; });
        QVERIFY(machine != seen.machines.end());
        qkvm::ui::MenuOf of;
        of.machine = *machine;
        of.status = status;
        of.media = true;
        QVERIFY(qkvm::ui::menuFor(of).row(u"media"_s) != nullptr);
        of.media = false; // (whoever may not have them is offered none)
        QVERIFY(qkvm::ui::menuFor(of).row(u"media"_s) == nullptr);
    }

    // Review c9b-1, L2: Cancel gives up a listing at once, and a run of adds; a second box that asks meanwhile is told
    // to wait.
    void cancelGivesUpAListingAndARunOfAdds()
    {
        ProxmoxByHand pve(proxmoxTls());
        for (const qint64 vmid : {101, 102}) {
            ProxmoxByHand::Guest more;
            more.name = u"vm%1"_s.arg(vmid);
            pve.guests.insert(vmid, more);
        }
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        OwnWall own(settings, secrets, quick());
        AddKvmView box;
        AddKvmView other;
        own.serve(box);
        own.serve(other);
        const QString address = pve.address().toString();
        pve.versionTakesMs = 15'000;
        box.listGuests(address, pve.tokenId, pve.secret, u"lab"_s);
        QVERIFY(box.busy());
        other.listGuests(address, pve.tokenId, pve.secret, u"lab"_s);
        QTRY_VERIFY_WITH_TIMEOUT(other.problem().contains(u"being asked already"_s), 2000);
        QVERIFY(!other.busy());
        box.giveUp();
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 2000);
        QVERIFY(box.problem().isEmpty());
        // A run of adds, given up after it began: none after it is added.
        pve.versionTakesMs = 0;
        box.listGuests(address, pve.tokenId, pve.secret, u"lab"_s);
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20'000);
        QCOMPARE(box.guests().size(), 3);
        pve.versionTakesMs = 15'000;
        box.addGuests(address, pve.tokenId, pve.secret, u"lab"_s, {100, 101, 102});
        QVERIFY(box.busy());
        box.giveUp();
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 2000);
        QTest::qWait(500);
        QVERIFY(own.wall().kvms().isEmpty());
    }

    // Outside review of 0.2.0, 3.7: each box adds from what it listed itself, and never from another box's listing.
    // One box lists a Proxmox while it shows its certificate; its address shows another one by the time a second box
    // lists it (a machine in the middle, taken on first use there: no guest of it is on the wall). The first box's Add
    // goes on lines held to the certificate the first box listed it by: what shows the other is sent nothing, and
    // nothing is added.
    void eachBoxAddsFromWhatItListedItself()
    {
        auto pve = std::make_unique<ProxmoxByHand>(proxmoxTls());
        const auto port = static_cast<quint16>(pve->address().port());
        const QString address = pve->address().toString();
        const QString uuid = pve->guests.value(100).uuid;
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        OwnWall own(settings, secrets, quick());
        AddKvmView box;
        AddKvmView other;
        own.serve(box);
        own.serve(other);
        box.listGuests(address, pve->tokenId, pve->secret, u"lab"_s);
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20'000);
        QVERIFY2(box.problem().isEmpty(), qPrintable(box.problem()));
        pve.reset();
        ProxmoxByHand middle(proxmoxTls(u"second"_s), port);
        middle.guests[100].uuid = uuid;
        other.listGuests(address, middle.tokenId, middle.secret, u"lab"_s);
        QTRY_VERIFY_WITH_TIMEOUT(!other.busy(), 20'000);
        QVERIFY2(other.problem().isEmpty(), qPrintable(other.problem()));
        const qsizetype heard = middle.requests.size();
        box.addGuests(address, middle.tokenId, middle.secret, u"lab"_s, {100});
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20'000);
        QCOMPARE(box.count(), 0);
        QVERIFY(own.wall().kvms().isEmpty());
        QCOMPARE(middle.requests.size(), heard);
        QCOMPARE(middle.refused, 0);
        // The second box adds from its own listing, as it was listed there.
        other.addGuests(address, middle.tokenId, middle.secret, u"lab"_s, {100});
        QTRY_VERIFY_WITH_TIMEOUT(!other.busy(), 20'000);
        QCOMPARE(other.count(), 1);
        QCOMPARE(own.wall().kvms().size(), 1);
    }

    // Review c9b-2, L4: ProxmoxAdding answers a refusal a turn of the loop later, and Cancel does not take it back.
    // One that comes after Cancel is told to nobody, and the guests after it in the run are not added.
    void aRefusalThatComesAfterCancelIsToldToNobody()
    {
        ProxmoxByHand pve(proxmoxTls());
        for (const qint64 vmid : {101, 102}) {
            ProxmoxByHand::Guest more;
            more.name = u"vm%1"_s.arg(vmid);
            pve.guests.insert(vmid, more);
        }
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        OwnWall own(settings, secrets, quick());
        AddKvmView box;
        own.serve(box);
        const QString address = pve.address().toString();
        box.listGuests(address, pve.tokenId, pve.secret, u"lab"_s);
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20'000);
        box.addGuests(address, pve.tokenId, pve.secret, u"lab"_s, {100});
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20'000);
        QCOMPARE(own.wall().kvms().size(), 1);
        QVERIFY2(box.added().contains(u"\"web\""_s), qPrintable(box.added()));
        box.listGuests(address, pve.tokenId, pve.secret, u"lab"_s);
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20'000);
        QCOMPARE(box.guests().size(), 3);
        // A run whose first guest is on the wall already (refused a turn later), given up in the same turn. (A few
        // turns of the loop, and no more: the refusal comes in the first, and an add of the next guest would be under
        // way from it, whatever its line and the PC's load make of its time. Review c9b-3, L5.)
        box.addGuests(address, pve.tokenId, pve.secret, u"lab"_s, {100, 101, 102});
        QVERIFY(box.busy());
        box.giveUp();
        QVERIFY(!box.busy());
        for (int turn = 0; turn < 5; ++turn) {
            QCoreApplication::processEvents();
        }
        QVERIFY(!own.asksAProxmox());
        QVERIFY2(box.problem().isEmpty(), qPrintable(box.problem()));
        QCOMPARE(own.wall().kvms().size(), 1);
        // A listing refused a turn later (no token's shape), given up in the same turn: nothing is said of it either.
        box.listGuests(address, u"root@pam"_s, u"a password"_s, u"lab"_s);
        QVERIFY(box.busy());
        box.giveUp();
        QTest::qWait(500);
        QVERIFY2(box.problem().isEmpty(), qPrintable(box.problem()));
        // And the box asks again as before; and a run is said with its own lines, not under the run's before it (review
        // c9b-3, L3).
        box.listGuests(address, pve.tokenId, pve.secret, u"lab"_s);
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20'000);
        QCOMPARE(box.guests().size(), 3);
        QVERIFY(box.problem().isEmpty());
        box.addGuests(address, pve.tokenId, pve.secret, u"lab"_s, {101});
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20'000);
        QCOMPARE(own.wall().kvms().size(), 2);
        QVERIFY2(box.added().contains(u"\"vm101\""_s) && !box.added().contains(u"\"web\""_s), qPrintable(box.added()));
    }

    void aTokenThatIsNoneOrWrongIsSaid()
    {
        ProxmoxByHand pve(proxmoxTls());
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        OwnWall own(settings, secrets, quick());
        AddKvmView box;
        own.serve(box);
        box.listGuests(pve.address().toString(), u"root@pam"_s, u"a password"_s, QString());
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20'000);
        QVERIFY(box.problem().startsWith(u"The user name is to be an API token's ID"_s));
        QCOMPARE(pve.stand->lines, 0);
        box.listGuests(pve.address().toString(), pve.tokenId, u"00000000-0000-0000-0000-000000000000"_s, QString());
        QTRY_VERIFY_WITH_TIMEOUT(!box.busy(), 20'000);
        QVERIFY(box.problem().contains(u"does not take the saved API token"_s));
        QCOMPARE(box.count(), 0);
        QVERIFY(own.wall().kvms().isEmpty());
    }

    // Review cx3-3, M1, in the box that changes a card: a guest's Proxmox stands there with its port written, 443 as
    // well, as the Add box reads an address (one typed without a port is at 8006 there, and in this box too): a guest
    // of a Proxmox at 443 shown as its host alone would read as one at 8006. Saved as it stands, it stays where it is.
    void aGuestsBoxShowsItsProxmoxWithItsPort()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        const auto guest = [](const QString& id, const QString& address, qint64 vmid) {
            return QJsonObject{{u"id"_s, id},
                               {u"kind"_s, u"proxmox"_s},
                               {u"name"_s, u"web"_s},
                               {u"address"_s, address},
                               {u"node"_s, u"pve1"_s},
                               {u"vmid"_s, vmid},
                               {u"user"_s, u"qkvm@pve!wall"_s}};
        };
        {
            QFile written(file);
            QVERIFY(written.open(QIODevice::WriteOnly));
            written.write(
                QJsonDocument(
                    QJsonObject{{u"kvms"_s, QJsonArray{guest(u"0a0b0c0d"_s, u"https://127.0.0.1"_s, 100),
                                                       guest(u"0e0f0a0b"_s, u"https://127.0.0.1:8006"_s, 101)}}})
                    .toJson());
        }
        core::Settings settings(file);
        MemorySecrets secrets;
        OwnWall own(settings, secrets, quick());
        QCOMPARE(own.wall().kvms().size(), 2);
        qkvm::ui::EditKvmView box;
        own.serve(box);
        box.openFor(u"0e0f0a0b"_s, false);
        QCOMPARE(box.address(), u"127.0.0.1:8006"_s);
        box.closed();
        box.openFor(u"0a0b0c0d"_s, false);
        QCOMPARE(box.address(), u"127.0.0.1:443"_s);
        box.save(u"behind a proxy"_s, box.address(), box.user(), QString(), box.group());
        QTRY_COMPARE_WITH_TIMEOUT(own.wall().kvms().value(0).name, u"behind a proxy"_s, 10'000);
        QCOMPARE(own.wall().kvms().value(0).address, u"https://127.0.0.1"_s);
        QCOMPARE(own.wall().kvms().value(1).address, u"https://127.0.0.1:8006"_s);
    }
};

QTEST_MAIN(TestOwnWallProxmox)
#include "test_own_wall_proxmox.moc"
