#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QTest>

#include <qkvm/base/screen_still.h>
#include <qkvm/core/kvmd_answers.h>
#include <qkvm/core/live.h>
#include <qkvm/core/local_wall.h>
#include <qkvm/core/media_place.h>
#include <qkvm/core/proxmox_adding.h>
#include <qkvm/core/proxmox_device.h>
#include <qkvm/core/settings.h>
#include <qkvm/rfb/rfb.h>

#include "memory_secrets.h"
#include "proxmox_by_hand.h"
#include "proxmox_console_by_hand.h"
#include "proxmox_device_tests.h"
#include "stand_in.h"

using namespace Qt::StringLiterals;
namespace base = qkvm::base;
using namespace qkvm::core::testing;
using qkvm::core::Action;
using qkvm::core::Done;
using qkvm::core::Live;
using qkvm::core::LocalWall;
using qkvm::core::MediaDone;
using qkvm::core::MediaMount;
using qkvm::core::OwnLive;
using qkvm::core::ProxmoxAdding;
using qkvm::core::ProxmoxDevice;
using qkvm::core::Snapshot;
using qkvm::core::Status;
using Guest = ProxmoxByHand::Guest;

namespace {

ProxmoxDevice::Pace consoleQuick()
{
    ProxmoxDevice::Pace pace = quickProxmox();
    pace.places = std::make_shared<ProxmoxDevice::Places>();
    pace.confirmEveryMs = 50;
    pace.followUpMs.clear();
    pace.console = ProxmoxDevice::consoleLimits();
    pace.console.lookPaceMs = 50;
    pace.console.openingMs = 3000;
    pace.console.handshakeMs = 5000;
    return pace;
}

// A picture of the guest's screen, asked for as a card asks: again and again until it comes (the web's picture_of).
base::Result<Snapshot> pictureOf(ProxmoxDevice& device, int withinMs = 15'000)
{
    QElapsedTimer waited;
    waited.start();
    while (true) {
        const base::Result<Snapshot> got = answerOf<Snapshot>(
            [&device](std::function<void(base::Result<Snapshot>)> done) { device.askSnapshot(640, std::move(done)); });
        if (got || got.error().code != "no_stream" || waited.elapsed() > withinMs) {
            return got;
        }
        QTest::qWait(50);
    }
}

base::Result<Done> done(ProxmoxDevice& device, const Action& what)
{
    return answerOf<Done>(
        [&device, &what](std::function<void(base::Result<Done>)> said) { device.act(what, std::move(said)); });
}

std::string codeOf(const base::Result<Snapshot>& got)
{
    return got ? std::string("ok") : got.error().code;
}

QString numberOf(const Guest& guest)
{
    return u"proxmox:"_s +
           QString::fromLatin1(
               QCryptographicHash::hash(guest.uuid.toLower().toUtf8(), QCryptographicHash::Sha256).toHex().first(32));
}

// The keysyms a console was sent going down, in their order.
std::vector<std::uint32_t> downs(const RfbConsoleByHand& console)
{
    std::vector<std::uint32_t> pressed;
    for (const auto& [keysym, down] : console.keys) {
        if (down) {
            pressed.push_back(keysym);
        }
    }
    return pressed;
}

// --- through this PC's own wall ---

const QString debian = u"local:iso/debian-13.iso"_s;

// The stand-in's guest `vmid` added to the wall, as the box that adds one does: its id.
QString addedTo(LocalWall& wall, const ProxmoxByHand& pve, const qkvm::core::DriverPaces& paces, qint64 vmid)
{
    ProxmoxAdding adding(wall, paces);
    const ProxmoxAdding::Typed typed{pve.address().toString(), pve.tokenId, pve.secret, u"lab"_s};
    const auto offered = answerOf<ProxmoxAdding::Offered>(
        [&](std::function<void(base::Result<ProxmoxAdding::Offered>)> done) { adding.list(typed, std::move(done)); });
    if (!offered) {
        return {};
    }
    const auto made = answerOf<ProxmoxAdding::Added>([&](std::function<void(base::Result<ProxmoxAdding::Added>)> done) {
        adding.add(typed, offered.value(), vmid, std::move(done));
    });
    return made ? made.value().id : QString();
}

QStringList capabilitiesOf(LocalWall& wall, const QString& id)
{
    const auto all = answerOf<QHash<QString, Status>>(
        [&wall](std::function<void(base::Result<QHash<QString, Status>>)> done) { wall.askStatus(std::move(done)); });
    return all ? all.value().value(id).capabilities.value_or(QStringList()) : QStringList();
}

// An image of the stand-in's put in through the wall, and started from as asked ("none": not).
base::Result<MediaDone> mountedThrough(qkvm::core::MediaPlace& place, const ProxmoxByHand& pve, const QString& id,
                                       const QString& how, bool restart)
{
    MediaMount mount;
    mount.image = debian;
    mount.machine = u"web"_s;
    mount.boot = how;
    mount.restart = restart;
    // (Picked from a list of its images on its node, as a box's mount is.)
    mount.listed = ProxmoxDevice::listedMark(guestOf(pve), u"pve1"_s);
    return answerOf<MediaDone>(
        [&](std::function<void(base::Result<MediaDone>)> done) { place.mountMedia(id, mount, std::move(done)); });
}

// The guest's power commands, as Proxmox wrote their tasks.
QStringList powerTasksOf(const ProxmoxByHand& pve)
{
    QStringList kinds;
    for (const ProxmoxByHand::Task& task : pve.tasks) {
        if (task.type == u"qmstop"_s || task.type == u"qmstart"_s) {
            kinds.append(task.type);
        }
    }
    return kinds;
}

} // namespace

// A guest's screen, live, keys, typed text and control through the console its Proxmox hands out (proxmox_screen.cpp):
// vncproxy's ticket, vncwebsocket on this program's own line, RFB over it with the ticket's one-time password; one
// console held while somebody looks, one task in the node's log for it; rule 30 before each. Against the stand-in's
// consoles (proxmox_console_by_hand.h), ported from the web's tests (../app/tests/test_proxmox.py of web 0.3.1).
class TestProxmoxConsole : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { const ProxmoxDevice first(ProxmoxDevice::Where{}, QString(), quickProxmox()); }

    // The web's test_a_guest_is_added_and_shows_its_screen: its card says it has a screen, keys, text, live and
    // control; one console, one task; its ticket used once; its password the one Proxmox handed out with it, and the
    // token in a header of every question and of the upgrade, never in an address, never to RFB.
    void aGuestShowsItsScreenThroughOneConsole()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxConsoles consoles(pve);
        ProxmoxDevice device(guestOf(pve), pve.secret, consoleQuick());
        const Status looked = lookedAt(device);
        QCOMPARE(looked.state, u"online"_s);
        QVERIFY(looked.hasScreen);
        QCOMPARE(looked.capabilities,
                 std::optional<QStringList>({u"power"_s, u"reset"_s, u"power_state"_s, u"media"_s, u"screen"_s,
                                             u"keys"_s, u"text"_s, u"live"_s, u"control"_s}));
        QCOMPARE(pve.consolesOpened(), 0); // (looking at it opens no console, and writes no task)
        const base::Result<Snapshot> picture = pictureOf(device);
        QVERIFY2(picture, picture ? "" : picture.error().message.c_str());
        // (The whole screen as a still is made for a card: as a server's.)
        QCOMPARE(picture.value().picture,
                 qkvm::base::screen::still<QByteArray>(64, 48, consoles.of(100).picture.rgb, 640));
        QCOMPARE(pve.consolesOpened(), 1);
        QCOMPARE(consoles.upgrades, 1);
        const RfbConsoleByHand& console = consoles.of(100);
        QCOMPARE(console.sessions, 1);
        QCOMPARE(console.passwordTries, 1);
        QCOMPARE(console.tokens.value(0), pve.authorization().toLatin1());
        QCOMPARE(console.protocols.value(0), "binary"_ba);
        QVERIFY(pve.inTheClear.isEmpty());
        QCOMPARE(pve.refused, 0);
        QVERIFY(pve.requests.contains(u"POST /nodes/pve1/qemu/100/vncproxy"_s));
        QTRY_COMPARE_WITH_TIMEOUT(lookedAt(device).width, 64, 5000);
    }

    // The web's test_a_guests_console_is_held_five_minutes_after_its_last_viewer, and its other times: the upgrade's
    // eight seconds (the port waits ten for its one connection), the ladder after one that failed, Proxmox's frames.
    void aGuestsConsoleIsHeldFiveMinutesAfterItsLastViewer()
    {
        const qkvm::core::RfbScreen::Limits limits = ProxmoxDevice::consoleLimits();
        QCOMPARE(limits.idleMs, 300'000);
        QCOMPARE(limits.openingMs, 8000);
        QCOMPARE(limits.retryMs, (std::array<int, 4>{15'000, 60'000, 300'000, 900'000}));
        QCOMPARE(limits.largestMessageBytes, qint64{128} * 1024 + 64);
        QCOMPARE(ProxmoxDevice::Pace{}.console.idleMs, 300'000);
    }

    // The web's test_one_console_is_held_while_somebody_looks_and_let_go_of_after.
    void oneConsoleIsHeldWhileSomebodyLooksAndLetGoOfAfter()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxConsoles consoles(pve);
        ProxmoxDevice::Pace pace = consoleQuick();
        pace.console.idleMs = 1000;
        ProxmoxDevice device(guestOf(pve), pve.secret, pace);
        static_cast<void>(lookedAt(device));
        QVERIFY(pictureOf(device));
        for (int look = 0; look < 30; ++look) { // three seconds of a card that is looked at: one console all along
            static_cast<void>(pictureOf(device));
            QTest::qWait(100);
        }
        QCOMPARE(pve.consolesOpened(), 1);
        QCOMPARE(consoles.of(100).watching, 1);
        // Nobody looks: let go of a second after.
        QTRY_COMPARE_WITH_TIMEOUT(consoles.of(100).watching, 0, 5000);
        QCOMPARE(pve.consolesOpened(), 1);
        QVERIFY(pictureOf(device));
        QCOMPARE(pve.consolesOpened(), 2);
    }

    // The web's test_a_stopped_guest_is_said_off_and_no_console_is_asked_for.
    void aStoppedGuestIsAskedForNoConsole()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxConsoles consoles(pve);
        Guest db;
        db.name = u"db"_s;
        db.status = u"stopped"_s;
        pve.guests.insert(101, db);
        ProxmoxDevice device(guestOf(pve, 101), pve.secret, consoleQuick());
        const Status looked = lookedAt(device);
        QCOMPARE(looked.state, u"no_signal"_s);
        QVERIFY(!looked.hasScreen);
        QCOMPARE(looked.capabilities,
                 std::optional<QStringList>({u"power"_s, u"reset"_s, u"power_state"_s, u"media"_s}));
        for (int ask = 0; ask < 5; ++ask) {
            QCOMPARE(codeOf(pictureOf(device)), std::string("no_picture"));
        }
        QCOMPARE(codeOf(done(device, Action::type(u"root"_s, false))), std::string("no_picture"));
        QCOMPARE(pve.consolesOpened(), 0);
        QCOMPARE(consoles.upgrades, 0);
    }

    // The web's test_a_console_that_fails_is_asked_for_less_and_less_often: every try is a task in the node's log.
    void aConsoleThatFailsIsAskedForLessAndLessOften()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxConsoles consoles(pve);
        consoles.fails.insert(100);
        ProxmoxDevice device(guestOf(pve), pve.secret, consoleQuick());
        static_cast<void>(lookedAt(device));
        QElapsedTimer asking;
        asking.start();
        while (asking.elapsed() < 3000) {
            static_cast<void>(pictureOf(device, 0));
            QTest::qWait(50);
        }
        QCOMPARE(pve.consolesOpened(), 1); // (the next only fifteen seconds after)
    }

    // The web's test_the_consoles_password_is_the_one_handed_out_and_counted_nowhere and
    // test_the_token_is_never_answered_to_a_console_that_asks_for_a_password_the_door_did_not_hand_out: a ticket that
    // comes with no password opens nothing, and the token is answered to no console.
    void aTicketWithoutItsPasswordOpensNothing()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxConsoles consoles(pve);
        pve.hostile = u"no_one_time"_s;
        ProxmoxDevice device(guestOf(pve), pve.secret, consoleQuick());
        static_cast<void>(lookedAt(device));
        const base::Result<Snapshot> got = pictureOf(device);
        QCOMPARE(codeOf(got), std::string("bad_response"));
        QCOMPARE(got.error().message, std::string("Proxmox handed out a console without a ticket and its password"));
        QCOMPARE(consoles.upgrades, 0);
        for (const QString& bad : {u"bad_ticket"_s, u"bad_port"_s}) {
            ProxmoxDevice other(guestOf(pve), pve.secret, consoleQuick());
            pve.hostile = bad;
            static_cast<void>(lookedAt(other));
            QCOMPARE(codeOf(pictureOf(other)), std::string("bad_response"));
        }
        QCOMPARE(consoles.upgrades, 0);
    }

    // The web's test_a_console_that_refuses_its_own_password_is_left_alone_and_the_token_is_not_blamed.
    void aConsoleThatRefusesItsOwnPasswordIsNotTheTokens()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxConsoles consoles(pve);
        ProxmoxDevice device(guestOf(pve), pve.secret, consoleQuick());
        static_cast<void>(lookedAt(device));
        // (The console takes another password than the ticket's: its door's consoles' own words.)
        RfbConsoleByHand& console = consoles.of(100);
        const auto before = console.door;
        console.door = [&console, before](const QByteArray& request) {
            RfbConsoleByHand::Door said = before(request);
            console.password = std::string("another");
            return said;
        };
        const base::Result<Snapshot> got = pictureOf(device);
        QCOMPARE(codeOf(got), std::string("bad_response"));
        QVERIFY(got.error().message.starts_with("The guest's console does not let qKVM in"));
        // Its card says the token is taken: it is not the token's doing.
        const Status looked = lookedAt(device);
        QCOMPARE(looked.state, u"online"_s);
        QVERIFY(!looked.hasScreen);
        QVERIFY(looked.server.value_or(qkvm::core::ServerReadings{})
                    .noScreen.startsWith(u"The guest's console does not let qKVM in"_s));
    }

    // The web's test_a_token_without_the_right_to_switch_is_told_by_proxmoxs_words, its console: refused by Proxmox,
    // said in its words, and no task written.
    void aTokenWithoutTheRightToTheConsoleIsToldSo()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxConsoles consoles(pve);
        pve.guests[100].mayConsole = false;
        ProxmoxDevice device(guestOf(pve), pve.secret, consoleQuick());
        static_cast<void>(lookedAt(device));
        const base::Result<Snapshot> got = pictureOf(device);
        QCOMPARE(codeOf(got), std::string("bad_response"));
        QVERIFY(got.error().message.find("VM.Console") != std::string::npos);
        QCOMPARE(pve.consolesOpened(), 0);
        QVERIFY(!lookedAt(device).hasScreen);
    }

    // The web's test_an_upgrade_that_gets_no_answer_is_given_up_on_in_time.
    void anUpgradeThatGetsNoAnswerIsGivenUpOnInTime()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxConsoles consoles(pve);
        pve.hostile = u"silent_upgrade"_s;
        ProxmoxDevice device(guestOf(pve), pve.secret, consoleQuick());
        static_cast<void>(lookedAt(device));
        QElapsedTimer took;
        took.start();
        const base::Result<Snapshot> got = pictureOf(device);
        QVERIFY(!got);
        QVERIFY2(took.elapsed() < 10'000, QByteArray::number(took.elapsed()).constData());
        QCOMPARE(consoles.hung, 1);
    }

    // The web's test_a_vm_given_a_deleted_guests_number_..., its screen, keys and text: another VM under the number,
    // running as the guest ran, is asked which it is right before its console, and shows nothing and is typed nothing.
    void aVmGivenADeletedGuestsNumberShowsNoScreenAndTakesNoKey()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxConsoles consoles(pve);
        ProxmoxDevice::Where where = guestOf(pve);
        where.machine = numberOf(pve.guests.value(100));
        ProxmoxDevice device(where, pve.secret, consoleQuick());
        QCOMPARE(lookedAt(device).state, u"online"_s);
        pve.deleteGuest(100);
        consoles.forget(100);
        Guest other;
        other.name = u"prod-db"_s;
        pve.makeGuest(other);
        QCOMPARE(codeOf(pictureOf(device)), std::string("another_machine"));
        for (const Action& what : {Action::type(u"s3cret"_s, false), Action::press(u"ControlLeft,AltLeft,Delete"_s),
                                   Action::plain(u"ctrl_alt_del"_s)}) {
            const base::Result<Done> said = done(device, what);
            QVERIFY(!said);
            QCOMPARE(said.error().code, std::string("another_machine"));
        }
        QTest::qWait(1500); // (whatever would have gone has had the time to)
        QCOMPARE(pve.consolesOpened(), 0);
        QCOMPARE(consoles.upgrades, 0);
        QVERIFY(consoles.of(100).keys.empty());
        const std::unique_ptr<OwnLive> own = device.makeLive();
        const std::unique_ptr<Live> live = own->open(Live::Need{});
        QVERIFY(!live->isOpen());
    }

    // Outside review of 0.2.0, 3.3, for a console's ticket (review cx3-1, L6): vncproxy refused 401 (the token taken
    // back for a moment) is asked for again only once which virtual machine has the guest's number was asked again.
    // Another made under it meanwhile is given no console: no ticket, no task in its node's log, no screen.
    void aTicketRefusedIsAskedForAgainOnlyForTheGuestItself()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxConsoles consoles(pve);
        ProxmoxDevice::Where where = guestOf(pve);
        where.machine = numberOf(pve.guests.value(100));
        ProxmoxDevice device(where, pve.secret, consoleQuick());
        QCOMPARE(lookedAt(device).state, u"online"_s);
        const QString ticket = u"POST /nodes/pve1/qemu/100/vncproxy"_s;
        bool once = false;
        pve.refuses = [&pve, &consoles, &once, &ticket](const QString& said) {
            if (said != ticket || std::exchange(once, true)) {
                return false;
            }
            // (Meanwhile the guest is deleted, and another made under its number that runs as it ran.)
            pve.deleteGuest(100);
            consoles.forget(100);
            Guest other;
            other.name = u"prod-db"_s;
            pve.makeGuest(other);
            return true;
        };
        QCOMPARE(codeOf(pictureOf(device)), std::string("another_machine"));
        QCOMPARE(pve.refusedPaths, QStringList{ticket});
        QCOMPARE(pve.asked(ticket), 0);
        QCOMPARE(pve.consolesOpened(), 0);
        QCOMPARE(consoles.upgrades, 0);
    }

    // Outside review 2, 4.1, for a console's ticket: a reading a sign-in old decides nothing, also where the driver is
    // signed in again by the time it comes. The console's reading of the guest's settings is out; another question of
    // the driver's gets nothing back, and a third signs in again in full, in which time the guest is deleted and
    // another made under its number. The reading comes with the guest's old number: no ticket goes on it, and the one
    // made after the new sign-in finds the other one. No console, no task in its node's log, no screen.
    void aTicketIsNotAskedForOnAReadingASignInOld()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxConsoles consoles(pve);
        ProxmoxDevice::Where where = guestOf(pve);
        where.machine = numberOf(pve.guests.value(100));
        ProxmoxDevice::Pace pace = consoleQuick();
        pace.quietMs = 8000; // (the console's reading is held three seconds)
        ProxmoxDevice device(where, pve.secret, pace);
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QVERIFY(identified(device)); // (its number read: the console's reading is the next)
        bool heldOnce = false;
        bool lost = false;
        bool signedInAgain = false;
        QElapsedTimer took;
        took.start();
        qint64 heldAt = -1;
        qint64 listedAt = -1;
        pve.answering = [&](const QString& said, Answered& answer) {
            if (said == u"GET /nodes/pve1/qemu/101/config"_s) {
                answer.hangUp = true;
                lost = true;
                return;
            }
            if (said == u"GET /nodes/pve1/qemu/100/config"_s && !std::exchange(heldOnce, true)) {
                answer.afterMs = 3000;
                heldAt = took.elapsed();
                QTimer::singleShot(100, &device, [&device] {
                    device.numberOfGuest(u"pve1"_s, 101, [](const base::Result<QString>& /*number*/) {});
                });
                QTimer::singleShot(900, &device, [&device, &listedAt, &took] {
                    device.listing([&listedAt, &took](const base::Result<qkvm::core::proxmox::Listing>& listed) {
                        listedAt = listed ? took.elapsed() : -2;
                    });
                });
                return;
            }
            if (said == u"GET /version"_s && lost && !std::exchange(signedInAgain, true)) {
                pve.deleteGuest(100);
                consoles.forget(100);
                Guest other;
                other.name = u"prod-db"_s;
                pve.makeGuest(other);
            }
        };
        QCOMPARE(codeOf(pictureOf(device)), std::string("another_machine"));
        QVERIFY(lost);
        QVERIFY(signedInAgain);
        // (Signed in again, and listed, before the console's reading came: so the driver was signed in when it came,
        // and only the sign-in that reading went under tells it from one made after. Fresh review of 4.1, L2.)
        QVERIFY2(heldAt >= 0 && listedAt > 0 && listedAt < heldAt + 2800,
                 qPrintable(u"its reading held at %1 ms, listed at %2 ms"_s.arg(heldAt).arg(listedAt)));
        QTest::qWait(500); // (whatever would have gone has had the time to)
        QCOMPARE(pve.asked(u"POST /nodes/pve1/qemu/100/vncproxy"_s), 0);
        QCOMPARE(pve.consolesOpened(), 0);
        QCOMPARE(consoles.upgrades, 0);
    }

    // Review cx3-2, L1, at its console: the console's line that falls (its Proxmox gone from the address, nothing else
    // asked meanwhile; the line cut, with no goodbye of Proxmox's on it: review cx3-3, L3) is a failure of contact as a
    // question's is. What answers there next, with a certificate the card accepts, is asked what it is before the token
    // goes again, the console's next asking for a way in among it.
    void afterTheConsolesLineFellWhatAnswersIsAskedBeforeTheTokenGoesAgain()
    {
        auto pve = std::make_unique<ProxmoxByHand>(proxmoxTls());
        auto consoles = std::make_unique<ProxmoxConsoles>(*pve);
        const auto port = static_cast<quint16>(pve->address().port());
        const QString secret = pve->secret;
        ProxmoxDevice::Pace pace = consoleQuick();
        // (A console that served a moment and fell is asked for again soon, and not a quarter of a minute later.)
        pace.console.retryMs = {300, 300, 300, 300};
        ProxmoxDevice device(guestOf(*pve), secret, pace);
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QVERIFY(pictureOf(device));
        consoles->cutEveryLine(); // (and all of it gone in the same turn: no goodbye said on any line)
        consoles.reset();
        pve.reset();
        const SomebodyElse other("first", port);
        QTest::qWait(1000); // (the fall of the console's line is seen, and its hold is over)
        const base::Result<Snapshot> got = pictureOf(device);
        QVERIFY(!got);
        QVERIFY(!other.heard.isEmpty());
        QVERIFY(!other.heardTheToken(secret));
    }

    // Review cx3-3, L3: a console's line that Proxmox closes with its goodbye (as it ends the console of a guest that
    // stops, whose QEMU ends its VNC server) is no failure of contact: it was let in, on a line held to what the
    // Proxmox is known by, and the token went on it. The next question carries the token with no unsigned question and
    // no sign-in first.
    void aConsoleClosedWithProxmoxsGoodbyeIsNoFailureOfContact()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxConsoles consoles(pve);
        ProxmoxDevice device(guestOf(pve), pve.secret, consoleQuick());
        QCOMPARE(lookedAt(device).state, u"online"_s);
        consoles.of(100).hangsUpAfterMs = 1500;
        QVERIFY(pictureOf(device));
        QTRY_COMPARE_WITH_TIMEOUT(consoles.of(100).watching, 0, 10'000);
        QTest::qWait(300); // (its end heard by the console's line)
        const qsizetype unsignedBefore = pve.unsignedAsked.size();
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QCOMPARE(pve.unsignedAsked.size(), unsignedBefore);
        QCOMPARE(pve.asked(u"GET /version"_s), 1);
    }

    // Outside review of 0.2.0, 3.3: where a look finds another virtual machine under the guest's number (one made in
    // the gap between the question which one has it and the console's ticket, whose console is the one open), every
    // live of the guest ends in those words, and its console is let go of: nothing of the other is shown, and no key
    // of anybody's goes to it.
    void aLiveEndsWhereAnotherVmHasTheNumber()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxConsoles consoles(pve);
        ProxmoxDevice::Where where = guestOf(pve);
        where.machine = numberOf(pve.guests.value(100));
        ProxmoxDevice device(where, pve.secret, consoleQuick());
        QCOMPARE(lookedAt(device).state, u"online"_s);
        const std::unique_ptr<OwnLive> own = device.makeLive();
        const std::unique_ptr<Live> live = own->open(Live::Need{});
        std::vector<Live::Picture> pictures;
        QObject::connect(live.get(), &Live::pictureCame, live.get(),
                         [&pictures](const Live::Picture& one) { pictures.push_back(one); });
        QString endedAs;
        QObject::connect(live.get(), &Live::closed, live.get(),
                         [&endedAs](const QString& code, const QString& /*message*/) { endedAs = code; });
        QTRY_VERIFY_WITH_TIMEOUT(!pictures.empty(), 10'000);
        QCOMPARE(consoles.of(100).watching, 1);
        // Another virtual machine under its number, on the console that is open: another UUID, a process begun since.
        pve.guests[100].uuid = ProxmoxByHand::fresh();
        pve.guests[100].uptime = 1;
        QCOMPARE(lookedAt(device).errorCode, u"another_machine"_s);
        QTRY_COMPARE_WITH_TIMEOUT(endedAs, u"another_machine"_s, 5000);
        QVERIFY(!live->isOpen());
        QTRY_COMPARE_WITH_TIMEOUT(consoles.of(100).watching, 0, 5000);
        QCOMPARE(pve.consolesOpened(), 1);
    }

    // Keys, Ctrl+Alt+Del and a text go to the guest's console, by their keysyms; the keyboard's Power key never.
    void keysAndTextGoToTheGuestsConsole()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxConsoles consoles(pve);
        ProxmoxDevice device(guestOf(pve), pve.secret, consoleQuick());
        static_cast<void>(lookedAt(device));
        QVERIFY(done(device, Action::plain(u"ctrl_alt_del"_s)));
        QVERIFY(done(device, Action::type(u"root"_s, true)));
        const std::vector<std::uint32_t> expected{0xFFE3, 0xFFE9, 0xFFFF, u'r', u'o', u'o', u't', 0xFF0D};
        // (Done is said once the last of it has left this program, which is not that the console has read it: the
        // line's socket is on a thread of its own, and the console's stand-in reads on this one, a turn or two later.)
        QTRY_COMPARE(downs(consoles.of(100)), expected);
        // Outside review 2.3: Enter after a text is pressed in the text's own typing, in its turn of the keyboard (as
        // the web types it: the text with a line's end after it), never in a typing of its own after it, which waits
        // behind whatever was asked for meanwhile and asks for the keyboard anew. A key asked for just after the text
        // comes after its Enter.
        consoles.of(100).keys.clear();
        const auto typed = std::make_shared<std::optional<base::Result<Done>>>();
        const auto pressed = std::make_shared<std::optional<base::Result<Done>>>();
        device.act(Action::type(u"ls"_s, true), [typed](base::Result<Done> one) { *typed = std::move(one); });
        device.act(Action::press(u"KeyA"_s), [pressed](base::Result<Done> one) { *pressed = std::move(one); });
        QTRY_VERIFY_WITH_TIMEOUT(typed->has_value() && pressed->has_value(), 15000);
        QVERIFY(typed->value_or(base::Error{"never", ""}).ok());
        QVERIFY(pressed->value_or(base::Error{"never", ""}).ok());
        QTRY_COMPARE(downs(consoles.of(100)).size(), std::size_t{4});
        QCOMPARE(downs(consoles.of(100)), (std::vector<std::uint32_t>{u'l', u's', 0xFF0D, u'a'}));
        const base::Result<Done> power = done(device, Action::press(u"Power"_s));
        QVERIFY(!power);
        QCOMPARE(power.error().code, std::string("invalid"));
        QCOMPARE(power.error().message, std::string(qkvm::core::kvmd::powerKeyRefused));
        QCOMPARE(pve.consolesOpened(), 1); // (one console for all of it)
    }

    // Review cx6-1, L5 (6.18): a text typed to a guest's console stops where it stands when its driver is told to (the
    // program locked, or made a wall screen: LocalWall::stopEveryTyping), and the Enter after it is not pressed.
    void aTypingToItsConsoleStopsWhereItStandsWhenItIsToGoNoFurther()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxConsoles consoles(pve);
        ProxmoxDevice::Pace pace = consoleQuick();
        pace.console.keysAtOnce = 2;
        pace.console.piecePauseMs = 50;
        ProxmoxDevice device(guestOf(pve), pve.secret, pace);
        static_cast<void>(lookedAt(device));
        const QString text(40, u'x');
        const auto typed = std::make_shared<std::optional<base::Result<Done>>>();
        device.act(Action::type(text, true), [typed](base::Result<Done> one) { *typed = std::move(one); });
        QTRY_VERIFY_WITH_TIMEOUT(downs(consoles.of(100)).size() >= 4, 15000);
        device.stopTyping();
        QTRY_VERIFY_WITH_TIMEOUT(typed->has_value(), 5000);
        const base::Result<Done> stopped = typed->value_or(Done{});
        QVERIFY(!stopped);
        QCOMPARE(stopped.error().code, std::string("stopped"));
        QTest::qWait(300);
        const std::vector<std::uint32_t> pressed = downs(consoles.of(100));
        QVERIFY2(pressed.size() < static_cast<std::size_t>(text.size()),
                 QByteArray::number(pressed.size()).constData());
        QVERIFY(std::ranges::none_of(pressed, [](std::uint32_t key) { return key == 0xFF0D; }));
    }

    // The web's test_a_guests_screen_live_with_its_keys_and_pointer: live, its pictures, and its keyboard over the one
    // console.
    void aGuestsScreenLiveWithItsKeys()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxConsoles consoles(pve);
        ProxmoxDevice device(guestOf(pve), pve.secret, consoleQuick());
        static_cast<void>(lookedAt(device));
        const std::unique_ptr<OwnLive> own = device.makeLive();
        const std::unique_ptr<Live> live = own->open(Live::Need{});
        std::vector<Live::Picture> pictures;
        QObject::connect(live.get(), &Live::pictureCame, live.get(),
                         [&pictures](const Live::Picture& one) { pictures.push_back(one); });
        bool control = false;
        QObject::connect(live.get(), &Live::controlChanged, live.get(),
                         [&control](bool on, const QString& /*code*/, const QString& /*message*/) { control = on; });
        QTRY_VERIFY_WITH_TIMEOUT(!pictures.empty(), 10'000);
        live->askControl(true);
        QTRY_VERIFY_WITH_TIMEOUT(control, 10'000);
        live->key(0x04, true); // a
        live->key(0x04, false);
        QTRY_VERIFY_WITH_TIMEOUT(downs(consoles.of(100)) == std::vector<std::uint32_t>{u'a'}, 5000);
        QCOMPARE(pve.consolesOpened(), 1);
        live->close();
    }

    // As the web's server (media_held; its test_a_restart_from_the_media_waits_for_whoever_has_the_keyboard_and_a_
    // mount_does_not), through this PC's own wall: a restart from the media is a power button's, refused while somebody
    // has the guest's keyboard and mouse live, in the web's words, and nothing is changed; putting an image in alone
    // touches no keyboard, and is done; and once they are given back, the start from the image is done.
    void aRestartFromTheMediaWaitsForWhoeverHasTheKeyboard()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxConsoles consoles(pve);
        const QTemporaryDir folder;
        qkvm::core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        qkvm::core::DriverPaces paces;
        paces.proxmox = consoleQuick();
        LocalWall wall(settings, secrets, paces);
        const QString web = addedTo(wall, pve, paces, 100);
        QVERIFY(!web.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(capabilitiesOf(wall, web).contains(u"media"_s), 10'000);
        std::unique_ptr<Live> live;
        wall.openLive(web, Live::Need{}, [&live](base::Result<std::unique_ptr<Live>> opened) {
            if (opened) {
                live = std::move(opened.value());
            }
        });
        QTRY_VERIFY_WITH_TIMEOUT(live != nullptr, 10'000);
        bool control = false;
        QObject::connect(live.get(), &Live::controlChanged, live.get(),
                         [&control](bool on, const QString& /*code*/, const QString& /*message*/) { control = on; });
        live->askControl(true);
        QTRY_VERIFY_WITH_TIMEOUT(control, 15'000);
        qkvm::core::MediaPlace& place = wall;
        for (const auto& [how, restart] : {std::pair{u"once"_s, false}, std::pair{u"until_unmounted"_s, true}}) {
            const base::Result<MediaDone> refused = mountedThrough(place, pve, web, how, restart);
            QCOMPARE(codeOf(refused), std::string("busy"));
            QCOMPARE(QString::fromStdString(refused.error().message),
                     u"A window of this PC has web's keyboard and mouse over a live line at this moment. Wait until "
                     u"they are given back."_s);
        }
        QVERIFY(pve.changes.isEmpty());
        QVERIFY(powerTasksOf(pve).isEmpty());
        // Putting an image in the drive touches no keyboard: it is done.
        const base::Result<MediaDone> put = mountedThrough(place, pve, web, u"none"_s, false);
        QVERIFY2(put, put ? "" : put.error().message.c_str());
        QCOMPARE(pve.guests.value(100).config.value(u"ide2"_s), debian + u",media=cdrom"_s);
        // Given back: started from it.
        live->askControl(false);
        QTRY_VERIFY_WITH_TIMEOUT(!control && !live->hasControl(), 10'000);
        const base::Result<MediaDone> started = mountedThrough(place, pve, web, u"once"_s, false);
        QVERIFY2(started, started ? "" : started.error().message.c_str());
        QCOMPARE(powerTasksOf(pve), (QStringList{u"qmstop"_s, u"qmstart"_s}));
        live->close();
    }

    // Review c9b-4, L1, as the web's live lines have it (live.py, control: `lines.busy`, asked again before they are
    // given): while the wall does something to the guest, a window is not given its keyboard and mouse, in the web's
    // words. So the refusal of a start from the image while a window has them holds to the guest's stop: asked while
    // the images are being listed for the start, they are not given; asked right before the start was asked, and given
    // by the console only after it, they are not given either. The guest is restarted from the image both times, and
    // once that is over they are given again.
    void keyboardAndMouseAreNotGivenWhileTheGuestIsActedOn()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxConsoles consoles(pve);
        const QTemporaryDir folder;
        qkvm::core::Settings settings(folder.filePath(u"settings.json"_s));
        MemorySecrets secrets;
        qkvm::core::DriverPaces paces;
        paces.proxmox = consoleQuick();
        paces.proxmox.console.retryMs = {300, 600}; // (its console asked again soon after the guest's restart)
        LocalWall wall(settings, secrets, paces);
        const QString web = addedTo(wall, pve, paces, 100);
        QVERIFY(!web.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(capabilitiesOf(wall, web).contains(u"media"_s), 10'000);
        std::unique_ptr<Live> live;
        wall.openLive(web, Live::Need{}, [&live](base::Result<std::unique_ptr<Live>> opened) {
            if (opened) {
                live = std::move(opened.value());
            }
        });
        QTRY_VERIFY_WITH_TIMEOUT(live != nullptr, 10'000);
        int pictures = 0;
        QObject::connect(live.get(), &Live::pictureCame, live.get(), [&pictures](const Live::Picture&) { ++pictures; });
        QStringList told;
        QObject::connect(live.get(), &Live::controlChanged, live.get(),
                         [&told](bool on, const QString& code, const QString& message) {
                             told.append(on ? u"given"_s : code + u": "_s + message);
                         });
        QTRY_VERIFY_WITH_TIMEOUT(pictures > 0, 15'000);
        // (The wall's words for a second action while one goes, the guest named as its sentences name a machine.)
        const QString busy = u"busy: Something else is being done to \"web\" at this moment. Wait for it to finish."_s;
        qkvm::core::MediaPlace& place = wall;
        // Asked while the images are being listed for a start from them.
        pve.afterImagesListed = [&live] { live->askControl(true); };
        const base::Result<MediaDone> first = mountedThrough(place, pve, web, u"once"_s, false);
        pve.afterImagesListed = nullptr;
        QVERIFY2(first, first ? "" : first.error().message.c_str());
        QCOMPARE(powerTasksOf(pve), (QStringList{u"qmstop"_s, u"qmstart"_s}));
        QTRY_VERIFY_WITH_TIMEOUT(!told.isEmpty(), 5000);
        QCOMPARE(told, QStringList{busy});
        QVERIFY(!live->hasControl());
        // Asked right before the start, and answered by the console after it was asked.
        const int before = pictures;
        QTRY_VERIFY_WITH_TIMEOUT(pictures > before, 15'000);
        live->askControl(true);
        const base::Result<MediaDone> second = mountedThrough(place, pve, web, u"once"_s, false);
        QVERIFY2(second, second ? "" : second.error().message.c_str());
        QCOMPARE(powerTasksOf(pve), (QStringList{u"qmstop"_s, u"qmstart"_s, u"qmstop"_s, u"qmstart"_s}));
        QTRY_VERIFY_WITH_TIMEOUT(told.size() == 2, 5000);
        QCOMPARE(told.last(), busy);
        QVERIFY(!live->hasControl());
        // Over: given.
        live->askControl(true);
        QTRY_VERIFY_WITH_TIMEOUT(live->hasControl(), 15'000);
        QCOMPARE(told.last(), u"given"_s);
        live->close();
    }
};

QTEST_GUILESS_MAIN(TestProxmoxConsole)
#include "test_proxmox_console.moc"
