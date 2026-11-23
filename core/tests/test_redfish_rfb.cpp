#include "redfish_device_tests.h"
#include "rfb_console_by_hand.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <vector>

#include <qkvm/base/screen_still.h>

// A server whose controller runs OpenBMC, on this PC's own wall: its console, RFB at /kvm/0 under the driver's own
// Redfish session, as the web's redfish.py and openbmc.py have it (tests/test_openbmc_console.py), against stand-ins on
// this PC's loopback address.
using namespace qkvm::core;
using namespace qkvm::core::testing;
namespace rfbt = qkvm::rfb::tests;
using namespace Qt::StringLiterals;
using Layout = RedfishByHand::Layout;
namespace base = qkvm::base;

namespace {

RedfishDevice::Pace rfbQuick()
{
    RedfishDevice::Pace pace = quick();
    pace.rfb.idleMs = 3000;
    pace.rfb.lookPaceMs = 100;
    pace.rfb.tickMs = 20;
    pace.rfb.retryMs = {500, 1000, 1500, 2000};
    pace.rfb.inputAfterMs = 150;
    pace.rfb.inputWaitMs = 3000;
    return pace;
}

// An OpenBMC controller that announces its console, and the console, which lets in the sessions it signed in.
struct OpenBmc {
    OpenBmc()
    {
        bmc.announcesKvm = true;
        console.servesAt(*bmc.stand, [this](const QByteArray& token) { return bmc.sessions.contains(token); });
    }
    RedfishByHand bmc{Layout::OpenBmc};
    RfbConsoleByHand console;
};

// A still of the card, asked again while the screen is on its way ("no_stream").
base::Result<Snapshot> snapshotOf(RedfishDevice& device, int width = 480)
{
    base::Result<Snapshot> found = base::Error{"never", "The still was never answered"};
    for (int tries = 0; tries < 60; ++tries) {
        std::optional<base::Result<Snapshot>> answer;
        device.askSnapshot(width, [&answer](base::Result<Snapshot> one) { answer = std::move(one); });
        if (!QTest::qWaitFor([&answer] { return answer.has_value(); }, 5000)) {
            return found;
        }
        found = answer.value_or(base::Error{"never", "The still was never answered"});
        if (found.ok() || found.error().code != "no_stream") {
            return found;
        }
        QTest::qWait(100);
    }
    return found;
}

} // namespace

class TestRedfishRfb : public QObject {
    Q_OBJECT

private slots:
    void anOpenBmcThatAnnouncesItsConsoleShowsItsScreen()
    {
        OpenBmc rig;
        RedfishDevice device(at(rig.bmc), u"secret"_s, rfbQuick());
        const Status looked = lookedAt(device);
        QCOMPARE(looked.state, u"online"_s);
        QVERIFY(looked.hasScreen);
        // (Its console takes keys and the pointer as RFB's events: keys, text and control, as the web's tile says.)
        for (const QString& word : {u"screen"_s, u"live"_s, u"keys"_s, u"text"_s, u"control"_s}) {
            QVERIFY2(supports(Machine{}, &looked, word), qPrintable(word));
        }
        const auto still = snapshotOf(device);
        QVERIFY2(still.ok(), still.ok() ? "" : still.error().message.c_str());
        QCOMPARE(still.value().picture, base::screen::still<QByteArray>(64, 48, rig.console.picture.rgb, 480));
        // Under the driver's own session: the one it signed in with, and none of AMI's web page.
        QCOMPARE(rig.bmc.logins, 1);
        QCOMPARE(rig.console.tokens.size(), 1);
        QVERIFY(rig.bmc.sessions.contains(rig.console.tokens.front()));
        QCOMPARE(rig.bmc.webLogins, 0);
        QCOMPARE(rig.bmc.tokenAsks, 0);
        QVERIFY(signsOnToNothing(rig.bmc));
    }

    void itsScreenIsLiveThroughTheServersLive()
    {
        OpenBmc rig;
        RedfishDevice device(at(rig.bmc), u"secret"_s, rfbQuick());
        static_cast<void>(lookedAt(device));
        // First the look pace of this rig, measured in the same rig (round-3 finding 5: a fixed bound was below this
        // rig's own look pace). A card looking at the still, for a second.
        const std::size_t lookBefore = rig.console.requests.size();
        QElapsedTimer looking;
        looking.start();
        while (looking.elapsed() < 1000) {
            device.askSnapshot(480, [](const base::Result<Snapshot>&) {});
            QTest::qWait(20);
        }
        const std::size_t lookPace = rig.console.requests.size() - lookBefore;
        QVERIFY2(lookPace > 0, QByteArray::number(static_cast<qulonglong>(lookPace)));
        // Then the live pace, while it is live.
        const std::unique_ptr<OwnLive> own = device.makeLive();
        const std::unique_ptr<Live> live = own->open(Live::Need{});
        std::vector<Live::Picture> pictures;
        std::vector<Live::Video> sizes;
        QObject::connect(live.get(), &Live::pictureCame, live.get(),
                         [&pictures](const Live::Picture& one) { pictures.push_back(one); });
        QObject::connect(live.get(), &Live::videoChanged, live.get(),
                         [&sizes](const Live::Video& one) { sizes.push_back(one); });
        QTRY_VERIFY_WITH_TIMEOUT(!pictures.empty(), 8000);
        QTRY_VERIFY_WITH_TIMEOUT(!sizes.empty(), 1000);
        QCOMPARE(sizes.front().width, 64);
        QVERIFY(pictures.front().keyFrame);
        // ...at a viewer's pace: at least half again the look pace. A ratio, so load that slows both the same does not
        // fail it; the drawing share does not throttle a screen this small.
        const std::size_t before = rig.console.requests.size();
        QTest::qWait(1000);
        const std::size_t livePace = rig.console.requests.size() - before;
        QVERIFY2(livePace * 2 >= lookPace * 3 && livePace >= lookPace + 3,
                 qPrintable(u"look %1 live %2"_s.arg(lookPace).arg(livePace)));
    }

    void anOpenBmcWithoutAConsoleHasNoScreenAndItsPasswordGoesNowhereElse()
    {
        RedfishByHand bmc(Layout::OpenBmc); // (no console announced)
        RedfishDevice device(at(bmc), u"secret"_s, rfbQuick());
        const Status looked = lookedAt(device);
        QVERIFY(!looked.hasScreen);
        QVERIFY(!supports(Machine{}, &looked, u"keys"_s) && !supports(Machine{}, &looked, u"control"_s));
        const auto still = snapshotOf(device);
        QVERIFY(!still.ok());
        QCOMPARE(still.error().code, std::string("no_picture"));
        QCOMPARE(bmc.webLogins, 0);
        QCOMPARE(bmc.tokenAsks, 0);
    }

    void whenItselfLandsDuringTheConsoleProbeNoPasswordIsPosted()
    {
        // The review's race (finding 4): a newly added OpenBMC with its KVM off. The still's console path sends the
        // unsigned /api/kvm/token probe (m_hasConsole not yet known) before the look has read the manager; the probe's
        // 401 (which an OpenBMC answers as AMI's door does) must not overwrite what the look then learns, and the
        // password is never posted to /api/session. The probe's answer is held back until the look has run itself().
        RedfishByHand bmc(RedfishByHand::Layout::OpenBmc); // (KVM off: no console announced)
        // The probe's 401 is held back long enough for the look to learn the controller is OpenBMC first, and short
        // enough that the web page's own line does not time the request out before it answers.
        bmc.tokenTakesMs = 1000;
        RedfishDevice device(at(bmc), u"secret"_s, rfbQuick());
        // Kick the console path first: the unsigned /api/kvm/token probe goes out while m_hasConsole is still unknown.
        device.askSnapshot(480, [](const base::Result<Snapshot>&) {});
        // Then the look; wait until its itself() has read the manager (learned it runs OpenBMC) before the probe's 401
        // answers. This is the review's race: the look learns, then the probe answers.
        device.look();
        QTRY_VERIFY_WITH_TIMEOUT(bmc.asked(u"GET "_s + bmc.manager()) >= 1, 900);
        QVERIFY(bmc.tokenAsks >= 1); // the unsigned probe did go out (the race path was taken)
        // Now let the probe answer (about 1 s) and the console flow run to its end.
        QTest::qWait(1500);
        QCOMPARE(bmc.webLogins, 0); // the password was never posted to its web page
        QCOMPARE(bmc.authorizations, 0);
        // And nothing of it is shown: no AMI console, no screen, no keys offered.
        const Status now = device.status();
        QVERIFY(!now.hasScreen);
        QVERIFY(!supports(Machine{}, &now, u"keys"_s));
    }

    void beforeItIsKnownWhatTheControllerIsAmisWebPageIsNotOfferedThePassword()
    {
        // A picture asked for before the first look has read what the controller is: not of AMI's console, whose door
        // an OpenBMC controller answers as AMI's does.
        OpenBmc rig;
        RedfishDevice device(at(rig.bmc), u"secret"_s, rfbQuick());
        std::optional<base::Result<Snapshot>> first;
        device.askSnapshot(480, [&first](base::Result<Snapshot> one) { first = std::move(one); });
        QTRY_VERIFY_WITH_TIMEOUT(first.has_value(), 5000);
        QVERIFY(first.has_value() && !first->ok());
        QCOMPARE(rig.bmc.webLogins, 0);
        // Once a look has read it: its own console.
        static_cast<void>(lookedAt(device));
        const auto still = snapshotOf(device);
        QVERIFY(still.ok());
        QCOMPARE(rig.bmc.webLogins, 0);
    }

    void anotherServerAtItsAddressIsShownNothing()
    {
        OpenBmc rig;
        rig.bmc.uuid = aSystem;
        rig.bmc.rootUuid = aRoot;
        RedfishDevice::Pace pace = rfbQuick();
        pace.rfb.idleMs = 400;
        RedfishDevice device(keptAs(rig.bmc, markOf(aSystem)), u"secret"_s, pace);
        static_cast<void>(lookedAt(device));
        QVERIFY(snapshotOf(device).ok());
        QCOMPARE(rig.console.askings, 1);
        // Its console given back (nobody looked), another server comes at its address: before the next session is
        // opened, which server answers is asked, and nothing is opened of the other one, also where no look said so.
        QTRY_COMPARE_WITH_TIMEOUT(rig.console.watching, 0, 4000);
        rig.bmc.beServer(bSystem, bRoot);
        const auto still = snapshotOf(device);
        QVERIFY(!still.ok());
        QCOMPARE(still.error().code, std::string("another_machine"));
        QCOMPARE(rig.console.askings, 1);
    }

    void aTypingToItsConsoleStopsWhereItStandsWhenItIsToGoNoFurther()
    {
        // Review cx6-1, L5 (6.18): a text typed to a server's console stops where it stands when its driver is told to
        // (the program locked, or made a wall screen: LocalWall::stopEveryTyping), with nothing down, and the Enter
        // after it is not pressed.
        OpenBmc rig;
        RedfishDevice::Pace pace = rfbQuick();
        pace.rfb.keysAtOnce = 2;
        pace.rfb.piecePauseMs = 50;
        RedfishDevice device(at(rig.bmc), u"secret"_s, pace);
        static_cast<void>(lookedAt(device));
        const QString text(40, u'x');
        const auto typed = std::make_shared<std::optional<base::Result<Done>>>();
        device.act(Action::type(text, true), [typed](base::Result<Done> one) { *typed = std::move(one); });
        QTRY_VERIFY_WITH_TIMEOUT(rig.console.keys.size() >= 8, 15000);
        device.stopTyping();
        QTRY_VERIFY_WITH_TIMEOUT(typed->has_value(), 5000);
        const base::Result<Done> stopped = typed->value_or(Done{});
        QVERIFY(!stopped.ok());
        QCOMPARE(stopped.error().code, std::string("stopped"));
        QTest::qWait(300);
        const auto& keys = rig.console.keys;
        QVERIFY2(keys.size() < 2 * static_cast<std::size_t>(text.size()), QByteArray::number(keys.size()).constData());
        QVERIFY(std::ranges::none_of(keys, [](const auto& key) { return key.first == 0xFF0D; }));
        QCOMPARE(std::ranges::count_if(keys, [](const auto& key) { return key.second; }),
                 std::ranges::count_if(keys, [](const auto& key) { return !key.second; }));
    }

    void keysAndATextFromTheMenuGoAsRfbsKeyEvents()
    {
        // The menu's keys and its text over OpenBMC's console, as the web's act sends them there (key_events,
        // text_events with the US layout's Shift); the keyboard's Power key never.
        OpenBmc rig;
        RedfishDevice device(at(rig.bmc), u"secret"_s, rfbQuick());
        static_cast<void>(lookedAt(device));
        // (The answer held apart from the call: one it gave up on is told after it has returned, by the driver's end at
        // the latest; as test_redfish_setup's presses.)
        const auto acted = [&device](const Action& what) {
            const auto answer = std::make_shared<std::optional<base::Result<Done>>>();
            device.act(what, [answer](base::Result<Done> one) { *answer = std::move(one); });
            static_cast<void>(QTest::qWaitFor([&answer] { return answer->has_value(); }, 15000));
            return answer->value_or(base::Error{"never", "never answered"});
        };
        const base::Result<Done> salute = acted(Action::plain(u"ctrl_alt_del"_s));
        QVERIFY2(salute.ok(), salute.ok() ? "" : salute.error().message.c_str());
        using Key = std::pair<std::uint32_t, bool>;
        QTRY_COMPARE_WITH_TIMEOUT(rig.console.keys.size(), std::size_t{6}, 3000);
        QVERIFY(
            rig.console.keys ==
            (std::vector<Key>{
                {0xFFE3, true}, {0xFFE9, true}, {0xFFFF, true}, {0xFFFF, false}, {0xFFE9, false}, {0xFFE3, false}}));
        const base::Result<Done> power = acted(Action::press(u"Power"_s));
        QCOMPARE(power.ok() ? std::string("ok") : power.error().code, std::string("invalid"));
        rig.console.keys.clear();
        const base::Result<Done> typed = acted(Action::type(u"A"_s, true));
        QVERIFY2(typed.ok(), typed.ok() ? "" : typed.error().message.c_str());
        QTRY_COMPARE_WITH_TIMEOUT(rig.console.keys.size(), std::size_t{6}, 3000);
        QVERIFY(rig.console.keys ==
                (std::vector<Key>{
                    {0xFFE1, true}, {0x41, true}, {0x41, false}, {0xFFE1, false}, {0xFF0D, true}, {0xFF0D, false}}));
        // Outside review 2.3: Enter after a text is pressed in the text's own typing, in its turn of the keyboard (as
        // the web types it), never in a typing of its own after it, which waits behind whatever was asked for meanwhile
        // and asks for the keyboard anew. A key asked for just after the text comes after its Enter.
        rig.console.keys.clear();
        const auto inTurn = std::make_shared<std::optional<base::Result<Done>>>();
        const auto after = std::make_shared<std::optional<base::Result<Done>>>();
        device.act(Action::type(u"a"_s, true), [inTurn](base::Result<Done> one) { *inTurn = std::move(one); });
        device.act(Action::press(u"KeyB"_s), [after](base::Result<Done> one) { *after = std::move(one); });
        QTRY_VERIFY_WITH_TIMEOUT(inTurn->has_value() && after->has_value(), 15000);
        QVERIFY(inTurn->value_or(base::Error{"never", ""}).ok());
        QVERIFY(after->value_or(base::Error{"never", ""}).ok());
        QTRY_COMPARE_WITH_TIMEOUT(rig.console.keys.size(), std::size_t{6}, 3000);
        QVERIFY(rig.console.keys ==
                (std::vector<Key>{
                    {0x61, true}, {0x61, false}, {0xFF0D, true}, {0xFF0D, false}, {0x62, true}, {0x62, false}}));
    }

    void itsConsoleIsGivenBackWhenNobodyLooks()
    {
        OpenBmc rig;
        RedfishDevice::Pace pace = rfbQuick();
        pace.rfb.idleMs = 600;
        RedfishDevice device(at(rig.bmc), u"secret"_s, pace);
        static_cast<void>(lookedAt(device));
        QVERIFY(snapshotOf(device).ok());
        QCOMPARE(rig.console.watching, 1);
        QTRY_COMPARE_WITH_TIMEOUT(rig.console.watching, 0, 4000);
    }
};

QTEST_GUILESS_MAIN(TestRedfishRfb)
#include "test_redfish_rfb.moc"
