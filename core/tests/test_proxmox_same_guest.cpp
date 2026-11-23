#include <memory>
#include <string>
#include <utility>

#include <QCryptographicHash>
#include <QSignalSpy>
#include <QTest>

#include <qkvm/core/proxmox_device.h>

#include "proxmox_by_hand.h"
#include "proxmox_device_tests.h"
#include "stand_in.h"

using namespace Qt::StringLiterals;
namespace base = qkvm::base;
using namespace qkvm::core::testing;
using qkvm::core::ProxmoxDevice;
using qkvm::core::Status;
using Guest = ProxmoxByHand::Guest;

namespace {

// The number a guest of the stand-in gives for itself, as the driver keeps it (the web's number_of).
QString numberOf(const Guest& guest)
{
    return u"proxmox:"_s +
           QString::fromLatin1(
               QCryptographicHash::hash(guest.uuid.toLower().toUtf8(), QCryptographicHash::Sha256).toHex().first(32));
}

// What the stand-in was asked but questions that only read.
QStringList commands(const ProxmoxByHand& pve)
{
    QStringList sent;
    for (const QString& one : pve.requests) {
        if (!one.startsWith(u"GET "_s)) {
            sent.append(one);
        }
    }
    return sent;
}

Guest named(const QString& name, const QString& node = u"pve1"_s)
{
    Guest made;
    made.name = name;
    made.node = node;
    return made;
}

ProxmoxDevice::Pace quickWithPlaces()
{
    ProxmoxDevice::Pace pace = quickProxmox();
    pace.places = std::make_shared<ProxmoxDevice::Places>();
    return pace;
}

} // namespace

// Rule 30: a guest of Proxmox is the one it was added for, by the number it gives for itself (its SMBIOS UUID), asked
// afresh before anything is decided or sent (proxmox_device.h). Ported from the web's tests of it
// (../app/tests/test_proxmox.py of web 0.3.1, from test_a_vm_given_a_deleted_guests_number_... on), and its row
// FX-B2 of FOR-THE-DESKTOP-APP.md. (Its power buttons and its console ask the same: their own steps' tests.)
class TestProxmoxSameGuest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { const ProxmoxDevice first(ProxmoxDevice::Where{}, QString(), quickProxmox()); }

    // A guest's number, read by kind (the web's own_number).
    void aGuestsNumberIsItsSmbiosUuid()
    {
        namespace proxmox = qkvm::core::proxmox;
        const QJsonValue lower =
            QJsonObject{{u"smbios1"_s, u"uuid=23e4e388-cf47-451b-b791-20054c233cc6,manufacturer=QEMU"_s}};
        const QJsonValue upper = QJsonObject{{u"smbios1"_s, u"uuid=23E4E388-CF47-451B-B791-20054C233CC6"_s}};
        QCOMPARE(proxmox::ownNumber(lower), proxmox::ownNumber(upper));
        QVERIFY(proxmox::ownNumber(lower).startsWith(u"proxmox:"_s));
        QCOMPARE(proxmox::ownNumber(lower).size(), 8 + 32);
        QCOMPARE(proxmox::ownNumber(
                     QJsonObject{{u"smbios1"_s, u"manufacturer=QEMU,uuid=23e4e388-cf47-451b-b791-20054c233cc6"_s}}),
                 proxmox::ownNumber(lower));
        for (const QJsonValue& none :
             {QJsonValue(QJsonObject{}), QJsonValue(QJsonObject{{u"smbios1"_s, u"manufacturer=x"_s}}),
              QJsonValue(QJsonObject{{u"smbios1"_s, u"uuid=00000000-0000-0000-0000-000000000000"_s}}),
              QJsonValue(QJsonObject{{u"smbios1"_s, 5}}), QJsonValue(),
              QJsonValue(QJsonObject{{u"smbios1"_s, u"uuid="_s + QString(5000, u'a')}}),
              QJsonValue(QJsonObject{{u"smbios1"_s, u"xuuid=23e4e388-cf47-451b-b791-20054c233cc6"_s}})}) {
            QVERIFY(proxmox::ownNumber(none).isEmpty());
        }
    }

    // Added, a guest is known by the number it gives for itself from then on (the server keeps it with the tile).
    void aGuestIsKnownByItsNumberWhenItIsAdded()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice device(guestOf(pve), pve.secret, quickWithPlaces());
        QSignalSpy known(&device, &ProxmoxDevice::knownAs);
        const base::Result<ProxmoxDevice::Identity> found = identified(device);
        QVERIFY2(found, found ? "" : found.error().message.c_str());
        QCOMPARE(found.value().number, std::optional<QString>(numberOf(pve.guests.value(100))));
        QCOMPARE(device.machine(), numberOf(pve.guests.value(100)));
        QCOMPARE(known.size(), 1);
        QCOMPARE(known.at(0).at(1).toString(), numberOf(pve.guests.value(100)));
    }

    // The web's test_a_vm_given_a_deleted_guests_number_is_sent_no_power_command_no_key_and_shows_no_screen, its card,
    // on its node and on another (where Proxmox says the guest is not on its node, and lists the number on another):
    // "web" (100) deleted and "prod-db" made has 100. Nothing of the other one is said as this guest's, and it is not
    // followed there.
    void aVmGivenADeletedGuestsNumberIsNotThisGuest_data()
    {
        QTest::addColumn<QString>("node");
        QTest::newRow("on-its-node") << u"pve1"_s;
        QTest::newRow("on-another-node") << u"pve2"_s;
    }
    void aVmGivenADeletedGuestsNumberIsNotThisGuest()
    {
        QFETCH(QString, node);
        ProxmoxByHand pve(proxmoxTls());
        pve.nodes = {u"pve1"_s, u"pve2"_s};
        ProxmoxDevice::Where where = guestOf(pve);
        where.machine = numberOf(pve.guests.value(100));
        ProxmoxDevice device(where, pve.secret, quickWithPlaces());
        QCOMPARE(lookedAt(device).state, u"online"_s);
        pve.deleteGuest(100);
        Guest other = named(u"prod-db"_s, node);
        other.uptime = 5; // (made a moment ago: a process of its own; one running as long as the guest did waits for
                          // the minute at a look, and is found by the button that asks first: the power step's tests)
        QCOMPARE(pve.makeGuest(other), qint64{100});
        const Status status = lookedAt(device);
        QCOMPARE(status.state, u"error"_s);
        QCOMPARE(status.errorCode, u"another_machine"_s);
        QVERIFY(status.errorMessage.startsWith(u"Another virtual machine has this guest's number"_s));
        QVERIFY(!status.server.has_value());
        QVERIFY(!status.power.has_value());
        QVERIFY(status.model.isEmpty());
        QVERIFY(commands(pve).isEmpty());
        QCOMPARE(device.where().node, u"pve1"_s);
        QCOMPARE(device.machine(), where.machine);
        // And so it says for the minute, asking nothing of it meanwhile.
        const int asked = static_cast<int>(pve.requests.size());
        QCOMPARE(lookedAt(device).errorCode, u"another_machine"_s);
        QCOMPARE(pve.requests.size(), asked);
    }

    // The web's test_a_look_asks_which_vm_has_the_number_once_a_minute_and_no_more_while_another_has_it.
    void aLookAsksWhichVmHasTheNumberOnceAMinute()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice::Pace pace = quickWithPlaces();
        pace.sameGuestEveryMs = 4000;
        ProxmoxDevice device(guestOf(pve), pve.secret, pace);
        for (int look = 0; look < 10; ++look) {
            QCOMPARE(lookedAt(device).state, u"online"_s);
        }
        // The first look took its number; none since.
        QCOMPARE(pve.asked(u"GET /nodes/pve1/qemu/100/config"_s), 1);
        QCOMPARE(pve.asked(u"GET /nodes/pve1/qemu/100/status/current"_s), 10);
        pve.deleteGuest(100);
        pve.makeGuest(named(u"prod-db"_s)); // (running as the guest ran: nothing in its state tells it apart)
        QTest::qWait(4200);                 // (a minute later)
        for (int look = 0; look < 10; ++look) {
            QCOMPARE(lookedAt(device).errorCode, u"another_machine"_s);
        }
        // Asked once (after its state, as every look reads it), and said from then on until the minute is over.
        QCOMPARE(pve.asked(u"GET /nodes/pve1/qemu/100/config"_s), 2);
        QCOMPARE(pve.asked(u"GET /nodes/pve1/qemu/100/status/current"_s), 11);
    }

    // The web's test_a_look_that_finds_the_guest_stopped_started_or_begun_anew_asks_at_once_which_vm_has_the_number.
    void aLookThatFindsTheGuestOtherwiseAsksAtOnce_data()
    {
        QTest::addColumn<QString>("made");
        QTest::newRow("stopped") << u"stopped"_s;
        QTest::newRow("running") << u"running"_s;
    }
    void aLookThatFindsTheGuestOtherwiseAsksAtOnce()
    {
        QFETCH(QString, made);
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice device(guestOf(pve), pve.secret, quickWithPlaces()); // (a minute between questions)
        QCOMPARE(lookedAt(device).state, u"online"_s);
        pve.deleteGuest(100);
        Guest other = named(u"prod-db"_s);
        other.status = made;
        other.uptime = 1; // (a new virtual machine runs a process of its own)
        pve.makeGuest(other);
        const Status status = lookedAt(device);
        QCOMPARE(status.errorCode, u"another_machine"_s);
        QVERIFY(!status.server.has_value());
    }

    // The web's test_the_first_look_after_a_tile_is_added_or_edited_asks_which_vm_has_the_number.
    void theFirstLookAfterAnAddingAsksWhichVmHasTheNumber()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice device(guestOf(pve), pve.secret, quickWithPlaces());
        QVERIFY(identified(device));
        pve.deleteGuest(100);
        pve.makeGuest(named(u"prod-db"_s)); // (running, its uptime as the guest's was)
        QCOMPARE(lookedAt(device).errorCode, u"another_machine"_s);
    }

    // The web's test_the_guest_itself_stopped_and_started_is_asked_once_at_each_change_and_never_refused.
    void theGuestItselfStoppedAndStartedIsNeverRefused()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice device(guestOf(pve), pve.secret, quickWithPlaces());
        QCOMPARE(lookedAt(device).state, u"online"_s);
        const int asked = pve.asked(u"GET /nodes/pve1/qemu/100/config"_s);
        pve.guests[100].status = u"stopped"_s;
        QCOMPARE(lookedAt(device).state, u"no_signal"_s);
        pve.guests[100].status = u"running"_s;
        pve.guests[100].uptime = 2;
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QCOMPARE(pve.asked(u"GET /nodes/pve1/qemu/100/config"_s), asked + 2);
    }

    // The web's test_the_guest_this_tile_was_added_for_is_followed_to_another_node_and_another_vm_there_is_not, its
    // card: moved within its cluster (the same guest, the same number), it is followed; deleted there and another made
    // on its first node, it is not followed back.
    void theGuestIsFollowedToAnotherNodeAndAnotherVmThereIsNot()
    {
        ProxmoxByHand pve(proxmoxTls());
        pve.nodes = {u"pve1"_s, u"pve2"_s};
        ProxmoxDevice::Pace pace = quickWithPlaces();
        ProxmoxDevice device(guestOf(pve), pve.secret, pace);
        QVERIFY(identified(device));
        const QString known = device.machine();
        QCOMPARE(known, numberOf(pve.guests.value(100)));
        pve.guests[100].node = u"pve2"_s;
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QCOMPARE(device.where().node, u"pve2"_s);
        QCOMPARE(device.machine(), known);
        QVERIFY(pve.requests.contains(u"GET /nodes/pve2/qemu/100/config"_s));
        pve.deleteGuest(100);
        pve.makeGuest(named(u"prod-db"_s, u"pve1"_s));
        // (A minute later: the list is read again for it.)
        ProxmoxDevice::Pace later = pace;
        later.places = std::make_shared<ProxmoxDevice::Places>();
        ProxmoxDevice::Where where = device.where();
        where.machine = known;
        ProxmoxDevice again(where, pve.secret, later);
        const Status status = lookedAt(again);
        QCOMPARE(status.errorCode, u"another_machine"_s);
        QCOMPARE(again.where().node, u"pve2"_s);
        QVERIFY(commands(pve).isEmpty());
    }

    // A Proxmox that says "not on this node" at every look has its list read once a minute at the most (the web's
    // test_a_proxmox_that_says_not_here_at_every_look_has_its_list_read_once_a_minute).
    void aProxmoxThatSaysNotHereHasItsListReadOnceAMinute()
    {
        ProxmoxByHand pve(proxmoxTls());
        pve.hostile = u"lost_guest"_s; // "not on this node", and listed on it all the same
        ProxmoxDevice device(guestOf(pve), pve.secret, quickWithPlaces());
        for (int look = 0; look < 10; ++look) {
            const Status status = lookedAt(device);
            QCOMPARE(status.state, u"error"_s);
        }
        QCOMPARE(pve.asked(u"GET /cluster/resources"_s), 1);
        QVERIFY(lookedAt(device).errorMessage.startsWith(u"Proxmox has no guest 100 on pve1, nor on another node"_s));
    }

    // The web's test_a_vm_that_gives_no_number_where_the_guest_gave_one_is_sent_nothing, its card.
    void aVmThatGivesNoNumberWhereTheGuestGaveOneIsNotThisGuest()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice::Where where = guestOf(pve);
        where.machine = numberOf(pve.guests.value(100));
        ProxmoxDevice device(where, pve.secret, quickWithPlaces());
        pve.deleteGuest(100);
        Guest old = named(u"old-one"_s);
        old.uuid.clear();
        pve.makeGuest(old);
        const Status status = lookedAt(device);
        QCOMPARE(status.errorCode, u"another_machine"_s);
        QVERIFY(status.errorMessage.contains(u"gives no SMBIOS UUID"_s));
    }

    // The web's test_a_guest_that_gives_no_number_and_was_remembered_by_none_works_as_before.
    void aGuestThatGivesNoNumberAndWasKeptByNoneWorksAsBefore()
    {
        ProxmoxByHand pve(proxmoxTls());
        pve.guests[100].uuid.clear();
        ProxmoxDevice device(guestOf(pve), pve.secret, quickWithPlaces());
        const base::Result<ProxmoxDevice::Identity> found = identified(device);
        QVERIFY(found);
        QCOMPARE(found.value().number, std::optional<QString>(QString()));
        QVERIFY(device.machine().isEmpty());
        QCOMPARE(lookedAt(device).state, u"online"_s);
    }

    // The web's test_a_tile_that_remembers_no_number_takes_the_first_it_sees_and_keeps_it.
    void aGuestKeptByNoNumberTakesTheFirstItSees()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice::Pace pace = quickWithPlaces();
        pace.sameGuestEveryMs = 300;
        ProxmoxDevice device(guestOf(pve), pve.secret, pace);
        QSignalSpy known(&device, &ProxmoxDevice::knownAs);
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QCOMPARE(device.machine(), numberOf(pve.guests.value(100)));
        QCOMPARE(known.size(), 1);
        QVERIFY(known.at(0).at(0).toString().isEmpty());
        pve.deleteGuest(100);
        pve.makeGuest(named(u"prod-db"_s));
        QTest::qWait(400);
        QCOMPARE(lookedAt(device).errorCode, u"another_machine"_s);
        QCOMPARE(known.size(), 1);
    }

    // The web's test_a_guest_whose_settings_cannot_be_read_as_it_is_added_is_not_added_unknown, and
    // test_an_edit_without_the_secret_while_the_settings_cannot_be_read_is_saved_and_keeps_the_number, the driver's
    // part: kept, what could not be read is said as nothing, and the number stays; none kept, nothing is taken.
    void settingsThatCannotBeReadTakeNoNumber()
    {
        ProxmoxByHand pve(proxmoxTls());
        pve.configFails = 1;
        ProxmoxDevice unknown(guestOf(pve), pve.secret, quickWithPlaces());
        const base::Result<ProxmoxDevice::Identity> refused = identified(unknown);
        QVERIFY(!refused);
        QCOMPARE(refused.error().code, std::string("bad_response"));
        QVERIFY(refused.error().message.starts_with("Which virtual machine has this guest's number could not be told"));
        QVERIFY(unknown.machine().isEmpty());
        QVERIFY(identified(unknown));
        QCOMPARE(unknown.machine(), numberOf(pve.guests.value(100)));

        ProxmoxDevice::Where where = guestOf(pve);
        where.machine = numberOf(pve.guests.value(100));
        ProxmoxDevice kept(where, pve.secret, quickWithPlaces());
        pve.configFails = 1;
        const base::Result<ProxmoxDevice::Identity> read = identified(kept);
        QVERIFY(read);
        QVERIFY(!read.value().number.has_value());
        QCOMPARE(kept.machine(), where.machine);
        // A look whose settings could not be read where a number is kept decides nothing on that.
        pve.configFails = 1;
        const Status status = lookedAt(kept);
        QCOMPARE(status.state, u"error"_s);
        QCOMPARE(kept.machine(), where.machine);
    }

    // A driver made anew for a guest kept by its number (a start, an edit) asks at its first look.
    void aGuestKeptByAnotherNumberIsNotTakenAtItsFirstLook()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice::Where where = guestOf(pve);
        Guest gone;
        where.machine = numberOf(gone);
        ProxmoxDevice device(where, pve.secret, quickWithPlaces());
        QCOMPARE(lookedAt(device).errorCode, u"another_machine"_s);
        const base::Result<ProxmoxDevice::Identity> found = identified(device);
        QVERIFY(!found);
        QCOMPARE(found.error().code, std::string("another_machine"));
        QCOMPARE(device.machine(), where.machine);
    }
};

QTEST_GUILESS_MAIN(TestProxmoxSameGuest)
#include "test_proxmox_same_guest.moc"
