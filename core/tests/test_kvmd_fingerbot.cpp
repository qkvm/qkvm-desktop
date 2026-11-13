#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>

#include <qkvm/core/board_holds.h>
#include <qkvm/core/kvmd_answers.h>
#include <qkvm/core/kvmd_device.h>
#include <qkvm/core/sign_in_pacing.h>

#include "made_up_kvm.h"
#include "stand_in.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::BoardHolds;
using core::KvmdDevice;
using core::testing::Answered;
using core::testing::good;
using core::testing::MadeUpKvm;
using core::testing::refusal;

// A GL.iNet Fingerbot paired with a Comet (rule 29 of the web's FOR-THE-DESKTOP-APP, its tests test_a_fingerbot.py):
// pressed only where the KVM said one is paired, on the line held anyway or on one opened before the press, never for
// nothing; its presses one at a time, held by the same hold as the KVM's power board, a press of either holding the
// other, also for a driver made anew; its battery asked once an hour, and low said. Against a made-up KVM.
class TestKvmdFingerbot : public QObject {
    Q_OBJECT

private:
    static KvmdDevice::Pace quick(std::shared_ptr<BoardHolds> boards = std::make_shared<BoardHolds>())
    {
        KvmdDevice::Pace pace;
        pace.connectWithinMs = 1500;
        pace.quietMs = 1500;
        pace.transferMs = 1500;
        pace.holdMs = 400;
        pace.longestHoldMs = 1600;
        pace.refusedHoldMs = 1200;
        pace.signInRetryMs = 800;
        pace.freshMs = 200;
        pace.firstContactMs = 400;
        pace.patienceMs = 400;
        pace.restAfterMs = 900;
        pace.pressHeldMs = 250;
        pace.longPressHeldMs = 700;
        pace.boardAnswerMs = 1200;
        pace.seenWithinMs = 1500;
        pace.seenEveryMs = 100;
        // (A Fingerbot's own, as short as a test can tell them apart.)
        pace.fingerbotPressMs = 150;
        pace.fingerbotLongPressMs = 400;
        pace.fingerbotAnswerMs = 900;
        pace.fingerbotBackMs = 200;
        pace.fingerbotAskWithinMs = 700;
        pace.fingerbotAskAgainMs = 1500;
        pace.batteryEveryMs = 60'000;
        pace.batteryRetryMs = 1500;
        pace.batteryWaitMs = 600;
        pace.pacing = std::make_shared<core::SignInPacing>();
        pace.boards = std::move(boards);
        return pace;
    }

    static base::Result<core::Done> doneBy(KvmdDevice& device, const QString& kind)
    {
        std::optional<base::Result<core::Done>> answer;
        device.act(core::Action::plain(kind), [&](base::Result<core::Done> came) { answer = std::move(came); });
        if (!QTest::qWaitFor([&] { return answer.has_value(); }, 20000)) {
            return base::Error{"never", "The call was never answered."};
        }
        return *answer;
    }
    static core::Status lookedAt(KvmdDevice& device)
    {
        QSignalSpy over(&device, &KvmdDevice::looked);
        device.look();
        if (!over.wait(20000)) {
            core::Status never;
            never.state = u"the look never ended"_s;
            return never;
        }
        return device.status();
    }
    // The driver holds the KVM's line for events, as it does while somebody watches its picture.
    static bool heldAwake(KvmdDevice& device, MadeUpKvm& kvm)
    {
        kvm.restsUntilWatched = true;
        std::optional<base::Result<core::Snapshot>> first;
        device.askSnapshot(400, [&first](base::Result<core::Snapshot> came) { first = std::move(came); });
        return QTest::qWaitFor([&first] { return first.has_value(); }, 10000) &&
               QTest::qWaitFor([&] { return kvm.stand->live.isConnected() && device.isKeptAwake(); }, 10000);
    }
    // What a Comet says on a line to its events of a Fingerbot paired with it, or of none.
    static QString fingerbotEvent(bool exists)
    {
        return QString::fromUtf8(QJsonDocument(QJsonObject{{u"event_type"_s, u"fingerbot"_s},
                                                           {u"event"_s, QJsonObject{{u"exist"_s, exists}}}})
                                     .toJson(QJsonDocument::Compact));
    }
    static std::string code(const base::Result<core::Done>& done)
    {
        return done ? std::string("(ok)") : done.error().code;
    }
    static std::string said(const base::Result<core::Done>& done)
    {
        return done ? std::string("(ok)") : done.error().message;
    }
    static bool says(const base::Result<core::Done>& done, const char* part)
    {
        return !done && done.error().message.find(part) != std::string::npos;
    }
    static int clicks(const MadeUpKvm& kvm) { return kvm.asked("/api/fingerbot/click"); }
    // A KVM with a Fingerbot, which says so on every line to its events.
    static void paired(MadeUpKvm& kvm)
    {
        kvm.fingerbot = true;
        kvm.stand->live.greeting = {fingerbotEvent(true)};
    }
    static bool heardPaired(KvmdDevice& device, MadeUpKvm& kvm)
    {
        return heldAwake(device, kvm) && QTest::qWaitFor([&] { return device.status().fingerbot.has_value(); }, 5000);
    }

private slots:
    void initTestCase()
    {
        // (Windows' own TLS is chosen before anything else touches TLS.)
        const KvmdDevice first(KvmdDevice::Where{}, QString());
    }

    void whatTheCometSaysOfItsFingerbotIsRead()
    {
        // `{"exist": true}` and `{"exist": false}` in a `fingerbot` event; anything else says neither (kvm.py
        // fingerbot_heard). A battery above a hundred is a full one; what is no number of 0 to 255 is none.
        QCOMPARE(core::kvmd::fingerbotHeard(fingerbotEvent(true).toUtf8()), std::optional<bool>(true));
        QCOMPARE(core::kvmd::fingerbotHeard(fingerbotEvent(false).toUtf8()), std::optional<bool>(false));
        const auto event = [](const QString& kind, const QJsonValue& exists, qsizetype more = 0) {
            QJsonObject word{{u"event_type"_s, kind}, {u"event"_s, QJsonObject{{u"exist"_s, exists}}}};
            if (more > 0) {
                word.insert(u"more"_s, QString(more, u'x'));
            }
            return QJsonDocument(word).toJson(QJsonDocument::Compact);
        };
        QCOMPARE(core::kvmd::fingerbotHeard(event(u"fingerbot"_s, u"yes"_s)), std::optional<bool>());
        QCOMPARE(core::kvmd::fingerbotHeard(event(u"atx"_s, true)), std::optional<bool>());
        QCOMPARE(core::kvmd::fingerbotHeard(event(u"fingerbot"_s, true, 5000)), std::optional<bool>());
        QCOMPARE(core::kvmd::fingerbotHeard(event(u"fingerbot"_s, true, 100)), std::optional<bool>(true));
        QCOMPARE(core::kvmd::batterySaid({{u"battery"_s, 101}}), std::optional<int>(100));
        QCOMPARE(core::kvmd::batterySaid({{u"battery"_s, 12.4}}), std::optional<int>(12));
        QCOMPARE(core::kvmd::batterySaid({{u"battery"_s, 256}}), std::optional<int>());
        QCOMPARE(core::kvmd::batterySaid({{u"battery"_s, -1}}), std::optional<int>());
        QCOMPARE(core::kvmd::batterySaid({{u"battery"_s, u"12"_s}}), std::optional<int>());
    }

    void aHoldSaysWhoseItIs()
    {
        // One hold for the machine's power buttons, which says whose press holds it: the words of a refusal name it.
        qint64 clock = 1000;
        BoardHolds holds(BoardHolds::mostKept, [&clock] { return clock; });
        const BoardHolds::Of kvm{QUrl(u"http://127.0.0.1:1"_s), {}};
        QCOMPARE(holds.heldBy(kvm), BoardHolds::By::board);
        const quint64 press = holds.out(kvm, 5000, BoardHolds::By::fingerbot);
        QCOMPARE(holds.heldBy(kvm), BoardHolds::By::fingerbot);
        holds.pressed(kvm, press, 1000, BoardHolds::By::fingerbot);
        QCOMPARE(holds.heldBy(kvm), BoardHolds::By::fingerbot);
        // A board's doubt that holds it longer is the board's.
        holds.mayBePressing(kvm, 9000);
        QCOMPARE(holds.heldBy(kvm), BoardHolds::By::board);
        QVERIFY(holds.unclear(kvm));
        // A Fingerbot's click that is refused gives back what held it before: the board's.
        const quint64 refused = holds.out(kvm, 20'000, BoardHolds::By::fingerbot);
        QCOMPARE(holds.heldBy(kvm), BoardHolds::By::fingerbot);
        holds.notPressed(kvm, refused);
        QCOMPARE(holds.heldBy(kvm), BoardHolds::By::board);
        clock += 10'000;
        QCOMPARE(holds.heldMs(kvm), 0);
        QCOMPARE(holds.heldBy(kvm), BoardHolds::By::board);
        // What the KVM said of its Fingerbot, kept by the same keys: found by its certificate at another address.
        const BoardHolds::Of known{QUrl(u"https://127.0.0.1:2"_s), "certificate"_ba};
        BoardHolds::Fingerbot said;
        said.paired = true;
        holds.keepFingerbot(known, said);
        QCOMPARE(holds.fingerbot({QUrl(u"https://127.0.0.1:3"_s), "certificate"_ba}).paired, std::optional<bool>(true));
        QCOMPARE(holds.fingerbot({QUrl(u"https://127.0.0.1:3"_s), {}}).paired, std::optional<bool>());
    }

    void aFingerbotIsHeardOnTheLineHeldAnywayAndKeptForADriverMadeAnew()
    {
        // The Comet says it only on a line to its events: the one held while its picture is watched. No line is
        // opened for it. What it said stays when that line ends, and for a driver made anew (an edit).
        MadeUpKvm kvm;
        paired(kvm);
        const auto boards = std::make_shared<BoardHolds>();
        KvmdDevice device(kvm.where(), u"secret"_s, quick(boards));
        QVERIFY(lookedAt(device).fingerbot == std::nullopt);
        QVERIFY(heardPaired(device, kvm));
        QCOMPARE(kvm.stand->live.lines, 1);
        QVERIFY(kvm.stand->live.paths.constFirst().contains("stream=1"));
        // Its battery, asked by a look, once: a full one.
        static_cast<void>(lookedAt(device));
        QTRY_COMPARE_WITH_TIMEOUT(device.status().fingerbot.value_or(core::Fingerbot{}).battery,
                                  std::optional<int>(100), 5000);
        QVERIFY(!device.status().fingerbot.value_or(core::Fingerbot{0, true}).low);
        static_cast<void>(lookedAt(device));
        QTest::qWait(1600);
        static_cast<void>(lookedAt(device));
        QCOMPARE(kvm.asked("/api/fingerbot/battery"), 1);
        KvmdDevice anew(kvm.where(), u"secret"_s, quick(boards));
        QVERIFY(anew.status().fingerbot.has_value());
        QCOMPARE(anew.status().fingerbot.value_or(core::Fingerbot{}).battery, std::optional<int>(100));
        static_cast<void>(lookedAt(anew));
        QCOMPARE(kvm.asked("/api/fingerbot/battery"), 1);
        QCOMPARE(kvm.stand->live.lines, 1);
    }

    void aLowBatteryIsSaidAndOneUnansweredIsAskedAgainLater()
    {
        MadeUpKvm kvm;
        paired(kvm);
        kvm.battery = 12;
        kvm.first["/api/fingerbot/battery"] = {Answered{503, "{}"}};
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        QVERIFY(heardPaired(device, kvm));
        static_cast<void>(lookedAt(device));
        QTRY_COMPARE_WITH_TIMEOUT(kvm.asked("/api/fingerbot/battery"), 1, 5000);
        // Not at every look after one that went unanswered: after a retry's time.
        QTest::qWait(300);
        static_cast<void>(lookedAt(device));
        QTest::qWait(200);
        QCOMPARE(kvm.asked("/api/fingerbot/battery"), 1);
        QVERIFY(!device.status().fingerbot.value_or(core::Fingerbot{0, false}).battery.has_value());
        QTest::qWait(1500);
        static_cast<void>(lookedAt(device));
        QTRY_COMPARE_WITH_TIMEOUT(device.status().fingerbot.value_or(core::Fingerbot{}).battery, std::optional<int>(12),
                                  5000);
        QVERIFY(device.status().fingerbot.value_or(core::Fingerbot{}).low);
    }

    void aPressGoesOnceAndIsAnsweredAfterItsTimeAndTheArms()
    {
        // The Comet answers once the Fingerbot took the press: the machine is held for the press's own time after
        // that, and the arm's way back. Firmly, as the Comet's own page presses by default.
        MadeUpKvm kvm;
        paired(kvm);
        kvm.atx = true;
        const auto boards = std::make_shared<BoardHolds>();
        KvmdDevice device(kvm.where(), u"secret"_s, quick(boards));
        QVERIFY(heardPaired(device, kvm));
        QElapsedTimer took;
        took.start();
        std::optional<base::Result<core::Done>> first;
        device.act(core::Action::plain(u"fingerbot_power"_s),
                   [&](const base::Result<core::Done>& came) { first = came; });
        QTRY_COMPARE_WITH_TIMEOUT(clicks(kvm), 1, 5000);
        // (Once the Comet's yes is in: the press holds the buttons for its time, and that is no doubt.)
        const BoardHolds::Of of{kvm.address(), {}};
        QTRY_VERIFY_WITH_TIMEOUT(boards->heldMs(of) > 0 && !boards->unclear(of), 5000);
        // Meanwhile: the same driver's second press, and a board's press of a driver made anew.
        const base::Result<core::Done> second = doneBy(device, u"fingerbot_power"_s);
        QCOMPARE(code(second), std::string("busy"));
        KvmdDevice anew(kvm.where(), u"secret"_s, quick(boards));
        const base::Result<core::Done> board = doneBy(anew, u"atx_power"_s);
        QVERIFY2(says(board, "Its Fingerbot is still holding the power button"), said(board).c_str());
        QTRY_VERIFY_WITH_TIMEOUT(first.has_value(), 5000);
        QVERIFY2(first.value_or(base::Error{"never", ""}).ok(), said(first.value_or(base::Error{"never", ""})).c_str());
        QVERIFY2(took.elapsed() >= 350, std::to_string(took.elapsed()).c_str());
        QCOMPARE(kvm.did.count("fingerbot 150 2"_ba), 1);
        QCOMPARE(kvm.asked("/api/atx/click"), 0);
        // Held: eight seconds in life, here 400 ms, and the arm.
        took.restart();
        const base::Result<core::Done> held = doneBy(device, u"fingerbot_power_long"_s);
        QVERIFY2(held.ok(), said(held).c_str());
        QVERIFY2(took.elapsed() >= 600, std::to_string(took.elapsed()).c_str());
        QCOMPARE(kvm.did.count("fingerbot 400 2"_ba), 1);
        QCOMPARE(kvm.stand->live.lines, 1);
    }

    void aPressWithNoClearAnswerHoldsBothButtonsForAnyDriver()
    {
        // The Comet's 5xx after the Fingerbot may have been told: it may have pressed. Nothing more goes to either
        // button of the machine for as long as a press taken then can last, whoever sends it.
        MadeUpKvm kvm;
        paired(kvm);
        kvm.atx = true;
        Answered gateway{502, "<html>Bad Gateway</html>"};
        gateway.afterMs = 500;
        kvm.first["/api/fingerbot/click"] = {gateway};
        const auto boards = std::make_shared<BoardHolds>();
        KvmdDevice device(kvm.where(), u"secret"_s, quick(boards));
        QVERIFY(heardPaired(device, kvm));
        const base::Result<core::Done> first = doneBy(device, u"fingerbot_power"_s);
        QCOMPARE(code(first), std::string("action_unknown"));
        QVERIFY(KvmdDevice::mayHaveBeenDone(first.error()));
        // (Past the hold that began as the click went out, 1250 ms before the answer's own would end.)
        QTest::qWait(900);
        const base::Result<core::Done> again = doneBy(device, u"fingerbot_power"_s);
        QVERIFY2(says(again, "The Fingerbot may still be pressing"), said(again).c_str());
        KvmdDevice anew(kvm.where(), u"secret"_s, quick(boards));
        const base::Result<core::Done> board = doneBy(anew, u"atx_power"_s);
        QVERIFY2(says(board, "Its Fingerbot may still be pressing the power button"), said(board).c_str());
        const base::Result<core::Done> bot = doneBy(anew, u"fingerbot_power"_s);
        QCOMPARE(code(bot), std::string("busy"));
        QCOMPARE(clicks(kvm), 1);
        QCOMPARE(kvm.asked("/api/atx/click"), 0);
        // A battery question is not asked meanwhile either (a look would ask it now, the first since one was heard).
        QCOMPARE(kvm.asked("/api/fingerbot/battery"), 0);
        static_cast<void>(lookedAt(device));
        QTest::qWait(100);
        QCOMPARE(kvm.asked("/api/fingerbot/battery"), 0);
        // Once that time is over (900 + 150 + 200 ms from the answer), a press goes.
        QTest::qWait(700);
        QVERIFY2(doneBy(anew, u"fingerbot_power"_s).ok(), "after the hold");
        QCOMPARE(clicks(kvm), 2);
    }

    void aPressNotAnsweredInTimeMayHavePressed()
    {
        MadeUpKvm kvm;
        paired(kvm);
        Answered late = good({{u"result"_s, u"success"_s}});
        late.afterMs = 1400; // beyond the 900 + 150 ms it is given
        kvm.first["/api/fingerbot/click"] = {late};
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        QVERIFY(heardPaired(device, kvm));
        const base::Result<core::Done> first = doneBy(device, u"fingerbot_power"_s);
        QCOMPARE(code(first), std::string("timeout"));
        QVERIFY2(says(first, "did not say whether its Fingerbot pressed"), said(first).c_str());
        // (Past the hold that began as the click went out.)
        QTest::qWait(400);
        QCOMPARE(code(doneBy(device, u"fingerbot_power"_s)), std::string("busy"));
        QCOMPARE(clicks(kvm), 1);
    }

    void aBoardPressWithNoClearAnswerHoldsTheFingerbot()
    {
        MadeUpKvm kvm;
        paired(kvm);
        kvm.atx = true;
        kvm.first["/api/atx/click"] = {Answered{502, "<html>Bad Gateway</html>"}};
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        QVERIFY(heardPaired(device, kvm));
        QCOMPARE(code(doneBy(device, u"atx_power"_s)), std::string("action_unknown"));
        const base::Result<core::Done> bot = doneBy(device, u"fingerbot_power"_s);
        QVERIFY2(says(bot, "Its power board may still be pressing a button"), said(bot).c_str());
        QCOMPARE(clicks(kvm), 0);
    }

    void aNoFromTheCometIsANoAndHoldsNothing()
    {
        // Only a 4xx is a no ("Press Failed" on the Comet's own page): nothing was pressed, and nothing is held.
        MadeUpKvm kvm;
        paired(kvm);
        kvm.first["/api/fingerbot/click"] = {refusal(400, u"BadRequestError"_s, u"Failed to click"_s)};
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        QVERIFY(heardPaired(device, kvm));
        const base::Result<core::Done> first = doneBy(device, u"fingerbot_power"_s);
        QCOMPARE(code(first), std::string("action_failed"));
        QVERIFY2(says(first, R"(The Fingerbot did not press: the KVM said "Failed to click")"), said(first).c_str());
        QVERIFY(!KvmdDevice::mayHaveBeenDone(first.error()));
        QVERIFY(doneBy(device, u"fingerbot_power"_s).ok());
        QCOMPARE(clicks(kvm), 2);
    }

    void aYesOfAnotherShapeIsNoYes()
    {
        MadeUpKvm kvm;
        paired(kvm);
        kvm.first["/api/fingerbot/click"] = {good({{u"result"_s, u"pending"_s}})};
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        QVERIFY(heardPaired(device, kvm));
        QCOMPARE(code(doneBy(device, u"fingerbot_power"_s)), std::string("action_unknown"));
        QCOMPARE(code(doneBy(device, u"fingerbot_power"_s)), std::string("busy"));
        QCOMPARE(clicks(kvm), 1);
    }

    void whereItHasNotSaidALineIsOpenedBeforeThePress()
    {
        // Nobody watched its picture: a line to its events is opened before the press (stream=0, no video), and its
        // word waited for. The press goes to the machine anyway.
        MadeUpKvm kvm;
        paired(kvm);
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        static_cast<void>(lookedAt(device));
        QCOMPARE(kvm.stand->live.lines, 0);
        const base::Result<core::Done> pressed = doneBy(device, u"fingerbot_power"_s);
        QVERIFY2(pressed.ok(), said(pressed).c_str());
        QCOMPARE(kvm.stand->live.lines, 1);
        QVERIFY(kvm.stand->live.paths.constFirst().contains("stream=0"));
        QCOMPARE(clicks(kvm), 1);
        // (Asked, it is known: the next press opens none.)
        QTRY_VERIFY_WITH_TIMEOUT(!kvm.stand->live.isConnected(), 5000);
        QVERIFY(doneBy(device, u"fingerbot_power"_s).ok());
        QCOMPARE(kvm.stand->live.lines, 1);
    }

    void whereItSaidNothingAPressIsRefusedWithNoLineForAWhile()
    {
        MadeUpKvm kvm;
        kvm.fingerbot = true; // (and says nothing of it)
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        static_cast<void>(lookedAt(device));
        const base::Result<core::Done> first = doneBy(device, u"fingerbot_power"_s);
        QCOMPARE(code(first), std::string("action_failed"));
        QVERIFY2(says(first, "did not say whether a Fingerbot is paired with it"), said(first).c_str());
        QCOMPARE(kvm.stand->live.lines, 1);
        const base::Result<core::Done> second = doneBy(device, u"fingerbot_power"_s);
        QVERIFY2(says(second, "It is asked again in"), said(second).c_str());
        QCOMPARE(kvm.stand->live.lines, 1);
        QCOMPARE(clicks(kvm), 0);
        // After a while, asked again.
        QTest::qWait(1600);
        kvm.stand->live.greeting = {fingerbotEvent(true)};
        QVERIFY(doneBy(device, u"fingerbot_power"_s).ok());
        QCOMPARE(kvm.stand->live.lines, 2);
    }

    void whereItSaidNoneAPressIsRefusedWithNoLine()
    {
        MadeUpKvm kvm;
        kvm.stand->live.greeting = {fingerbotEvent(false)};
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        static_cast<void>(lookedAt(device));
        const base::Result<core::Done> first = doneBy(device, u"fingerbot_power"_s);
        QCOMPARE(code(first), std::string("invalid"));
        QVERIFY2(says(first, "No Fingerbot is paired with this KVM"), said(first).c_str());
        QCOMPARE(code(doneBy(device, u"fingerbot_power"_s)), std::string("invalid"));
        QCOMPARE(kvm.stand->live.lines, 1);
        QVERIFY(device.status().fingerbot == std::nullopt);
        QCOMPARE(clicks(kvm), 0);
    }

    void aStockKvmdIsAskedNothingOfAFingerbot()
    {
        MadeUpKvm kvm;
        kvm.glinet = false;
        kvm.fingerbot = true;
        kvm.stand->live.greeting = {fingerbotEvent(true)};
        const auto boards = std::make_shared<BoardHolds>();
        BoardHolds::Fingerbot before;
        before.paired = true; // (what was said at its address before it was stock kvmd)
        before.saidAt = boards->nowMs();
        boards->keepFingerbot({kvm.address(), {}}, before);
        KvmdDevice device(kvm.where(), u"secret"_s, quick(boards));
        static_cast<void>(lookedAt(device));
        const base::Result<core::Done> first = doneBy(device, u"fingerbot_power"_s);
        QCOMPARE(code(first), std::string("invalid"));
        QCOMPARE(kvm.stand->live.lines, 0);
        QCOMPARE(clicks(kvm), 0);
        QVERIFY(device.status().fingerbot == std::nullopt);
    }

    void aBatteryQuestionUnderWayIsLetFinishBeforeAPress()
    {
        // The Comet talks to its Fingerbot one request at a time: a press sent while another was being taken failed.
        MadeUpKvm kvm;
        paired(kvm);
        Answered slow = good({{u"battery"_s, 80}});
        slow.afterMs = 400;
        kvm.first["/api/fingerbot/battery"] = {slow};
        KvmdDevice device(kvm.where(), u"secret"_s, quick());
        QVERIFY(heardPaired(device, kvm));
        static_cast<void>(lookedAt(device));
        QTRY_COMPARE_WITH_TIMEOUT(kvm.asked("/api/fingerbot/battery"), 1, 5000);
        std::optional<base::Result<core::Done>> pressed;
        device.act(core::Action::plain(u"fingerbot_power"_s),
                   [&](const base::Result<core::Done>& came) { pressed = came; });
        QTest::qWait(200);
        QCOMPARE(clicks(kvm), 0);
        const base::Result<core::Done> meanwhile = doneBy(device, u"fingerbot_power"_s);
        QVERIFY2(says(meanwhile, "The Fingerbot is pressing the power button at this moment"), said(meanwhile).c_str());
        QTRY_VERIFY_WITH_TIMEOUT(pressed.has_value(), 5000);
        QVERIFY2(pressed.value_or(base::Error{"never", ""}).ok(),
                 said(pressed.value_or(base::Error{"never", ""})).c_str());
        QCOMPARE(clicks(kvm), 1);
        QCOMPARE(device.status().fingerbot.value_or(core::Fingerbot{}).battery, std::optional<int>(80));
    }

    void aFingerbotsPressHoldsTheButtonsFromItsLookToItsClick()
    {
        // As a board's press holds its board from its look to its click (the merged review's LOW 9): while the first
        // driver's Fingerbot press waits for a battery question under way, another driver of the same KVM finds the
        // machine's buttons held, for its board's press and its Fingerbot's alike, said as a press of the Fingerbot's.
        MadeUpKvm kvm;
        paired(kvm);
        kvm.atx = true;
        Answered slow = good({{u"battery"_s, 80}});
        slow.afterMs = 500;
        kvm.first["/api/fingerbot/battery"] = {slow};
        const auto boards = std::make_shared<BoardHolds>();
        KvmdDevice device(kvm.where(), u"secret"_s, quick(boards));
        QVERIFY(heardPaired(device, kvm));
        static_cast<void>(lookedAt(device));
        QTRY_COMPARE_WITH_TIMEOUT(kvm.asked("/api/fingerbot/battery"), 1, 5000);
        std::optional<base::Result<core::Done>> pressed;
        device.act(core::Action::plain(u"fingerbot_power"_s),
                   [&](const base::Result<core::Done>& came) { pressed = came; });
        KvmdDevice other(kvm.where(), u"secret"_s, quick(boards));
        const base::Result<core::Done> board = doneBy(other, u"atx_power"_s);
        QVERIFY2(says(board, "Its Fingerbot is pressing the power button at this moment"), said(board).c_str());
        const base::Result<core::Done> bot = doneBy(other, u"fingerbot_power"_s);
        QVERIFY2(says(bot, "The Fingerbot is pressing the power button at this moment"), said(bot).c_str());
        QTRY_VERIFY_WITH_TIMEOUT(pressed.has_value(), 5000);
        QVERIFY2(pressed.value_or(base::Error{"never", ""}).ok(),
                 said(pressed.value_or(base::Error{"never", ""})).c_str());
        QCOMPARE(clicks(kvm), 1);
        QCOMPARE(kvm.asked("/api/atx/click"), 0);
    }

    void aBoardsPressHoldsTheFingerbotFromItsLook()
    {
        // The other way round: while the first driver's board press asks its board (slowly), another driver's Fingerbot
        // press is refused as a press under way, and nothing goes to the Fingerbot.
        MadeUpKvm kvm;
        paired(kvm);
        kvm.atx = true;
        const auto boards = std::make_shared<BoardHolds>();
        KvmdDevice device(kvm.where(), u"secret"_s, quick(boards));
        QVERIFY(heardPaired(device, kvm));
        Answered slow = good({{u"enabled"_s, true}, {u"busy"_s, false}, {u"power"_s, u"on"_s}});
        slow.afterMs = 500;
        kvm.first["/api/atx"] = {slow};
        std::optional<base::Result<core::Done>> pressed;
        device.act(core::Action::plain(u"atx_reset"_s), [&](const base::Result<core::Done>& came) { pressed = came; });
        KvmdDevice other(kvm.where(), u"secret"_s, quick(boards));
        const base::Result<core::Done> bot = doneBy(other, u"fingerbot_power"_s);
        QVERIFY2(says(bot, "A power button of this machine is being pressed at this moment"), said(bot).c_str());
        QTRY_VERIFY_WITH_TIMEOUT(pressed.has_value(), 5000);
        QCOMPARE(clicks(kvm), 0);
        QCOMPARE(kvm.asked("/api/atx/click"), 1);
    }

    void aDriverLetGoOfWhileItsPressIsOutHoldsTheButtons()
    {
        // The Comet goes on with a press whose asker gave up: an edit of the card while it is out holds the machine's
        // buttons for the driver made anew.
        MadeUpKvm kvm;
        paired(kvm);
        Answered slow = good({{u"result"_s, u"success"_s}});
        slow.afterMs = 1000; // (within the 900 + 150 ms it is given)
        kvm.first["/api/fingerbot/click"] = {slow};
        const auto boards = std::make_shared<BoardHolds>();
        auto device = std::make_unique<KvmdDevice>(kvm.where(), u"secret"_s, quick(boards));
        QVERIFY(heardPaired(*device, kvm));
        device->act(core::Action::plain(u"fingerbot_power"_s), [](const base::Result<core::Done>&) {});
        QTRY_COMPARE_WITH_TIMEOUT(clicks(kvm), 1, 5000);
        QTest::qWait(800);
        device.reset();
        // (Past the hold that began as the click went out: held from when its driver was let go of.)
        QTest::qWait(500);
        KvmdDevice anew(kvm.where(), u"secret"_s, quick(boards));
        const base::Result<core::Done> bot = doneBy(anew, u"fingerbot_power"_s);
        QVERIFY2(says(bot, "The Fingerbot may still be pressing"), said(bot).c_str());
        QCOMPARE(clicks(kvm), 1);
    }

    void aPressReservesTheButtonsForAsLongAsItsRoadRuns()
    {
        // Review 4's LOW 3: the buttons are held from a press's look to its click, however long the road between takes
        // (here a sign-in the KVM answers after three seconds, far past what a click's own time and its answer's
        // would hold): another driver of the same KVM is refused as a press under way, and one press goes.
        MadeUpKvm kvm;
        paired(kvm);
        kvm.atx = true;
        const auto boards = std::make_shared<BoardHolds>();
        KvmdDevice::Pace pace = quick(boards);
        pace.fingerbotAnswerMs = 500; // (its click and its answer: 850 ms, twice that 1.7 s)
        KvmdDevice first(kvm.where(), u"secret"_s, pace);
        QVERIFY(heardPaired(first, kvm));
        // A driver made anew (a card changed) signs in before its click: slowly.
        kvm.signInAfterMs = 3000;
        KvmdDevice anew(kvm.where(), u"secret"_s, pace);
        std::optional<base::Result<core::Done>> pressed;
        anew.act(core::Action::plain(u"fingerbot_power"_s),
                 [&](const base::Result<core::Done>& came) { pressed = came; });
        QTest::qWait(2200);
        QCOMPARE(clicks(kvm), 0);
        const base::Result<core::Done> bot = doneBy(first, u"fingerbot_power"_s);
        QVERIFY2(says(bot, "The Fingerbot is pressing the power button at this moment"), said(bot).c_str());
        const base::Result<core::Done> board = doneBy(first, u"atx_power"_s);
        QVERIFY2(says(board, "Its Fingerbot is pressing the power button at this moment"), said(board).c_str());
        QTRY_VERIFY_WITH_TIMEOUT(pressed.has_value(), 10000);
        QVERIFY2(pressed.value_or(base::Error{"never", ""}).ok(),
                 said(pressed.value_or(base::Error{"never", ""})).c_str());
        QCOMPARE(clicks(kvm), 1);
        QCOMPARE(kvm.asked("/api/atx/click"), 0);
    }

    void aReservationLostByItsHolderRunsOutOnlyFarPastAnyRoad()
    {
        // (Review 4's LOW 3.) Given back by its holder, it holds nothing; held, it stands minutes past the longest road
        // (a battery question, a sign-in, a line to its events), and runs out only then.
        qint64 clock = 1000;
        BoardHolds holds(BoardHolds::mostKept, [&clock] { return clock; });
        const BoardHolds::Of kvm{QUrl(u"http://127.0.0.1:1"_s), {}};
        const quint64 mine = holds.reserve(kvm, BoardHolds::By::fingerbot);
        clock += 60'000;
        QVERIFY(holds.pressing(kvm));
        QVERIFY(holds.stillOut(kvm, mine));
        QCOMPARE(holds.heldBy(kvm), BoardHolds::By::fingerbot);
        holds.notPressed(kvm, mine);
        QCOMPARE(holds.heldMs(kvm), 0);
        const quint64 lost = holds.reserve(kvm);
        clock += BoardHolds::reservedMostMs - 1;
        QVERIFY(holds.heldMs(kvm) > 0);
        clock += 1;
        QCOMPARE(holds.heldMs(kvm), 0);
        QVERIFY(holds.stillOut(kvm, lost)); // (its holder's still, should it come back)
    }
};

QTEST_GUILESS_MAIN(TestKvmdFingerbot)
#include "test_kvmd_fingerbot.moc"
