#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>

#include <qkvm/core/jetkvm_device.h>
#include <qkvm/core/sign_in_pacing.h>
#include <qkvm/core/tls.h>

#include "jetkvm_by_hand.h"
#include "jetkvm_viewer_by_hand.h"

using namespace Qt::StringLiterals;
namespace base = qkvm::base;
using qkvm::core::Action;
using qkvm::core::Done;
using qkvm::core::JetKvmDevice;
using qkvm::core::SignInPacing;
using qkvm::core::Status;
using qkvm::core::WakeTarget;
using qkvm::core::testing::JetKvmByHand;
using qkvm::core::testing::JetKvmViewerByHand;

namespace {

JetKvmDevice::Pace quick()
{
    JetKvmDevice::Pace pace;
    pace.connectWithinMs = 3000;
    pace.quietMs = 3000;
    pace.transferMs = 3000;
    pace.holdMs = 400;
    pace.longestHoldMs = 1600;
    pace.refusedHoldMs = 1200;
    pace.signInRetryMs = 800;
    pace.lineWithinMs = 4000;
    pace.freshMs = 0;
    pace.pacing = std::make_shared<SignInPacing>();
    pace.viewer.connection.watchEveryMs = 100;
    pace.viewer.unwatchedEveryMs = 100;
    pace.viewer.retryPauseMs = 300;
    return pace;
}

JetKvmDevice::Where at(const JetKvmByHand& jet)
{
    JetKvmDevice::Where where;
    where.address = jet.address();
    return where;
}

struct Rig {
    explicit Rig(const JetKvmDevice::Pace& pace = quick()) : device(at(jet), u"secret"_s, pace) {}
    JetKvmByHand jet;
    JetKvmViewerByHand viewer{jet};
    JetKvmDevice device;
};

base::Result<Done> done(Rig& rig, const Action& action)
{
    std::optional<base::Result<Done>> answer;
    rig.device.act(action, [&answer](const base::Result<Done>& what) { answer = what; });
    if (!QTest::qWaitFor([&answer] { return answer.has_value(); }, 20'000)) {
        return base::Error{"never", "Never answered."};
    }
    return *answer;
}

// The calls of a method that came over the JetKVM's rpc.
QList<QJsonObject> paramsOf(JetKvmViewerByHand& viewer, const QString& method)
{
    QList<QJsonObject> said;
    for (const auto& [name, params] : viewer.rpcCalls()) {
        if (name == method) {
            said.append(params);
        }
    }
    return said;
}

Status lookedAt(JetKvmDevice& device)
{
    QSignalSpy over(&device, &JetKvmDevice::looked);
    device.look();
    if (!over.wait(20'000)) {
        return {};
    }
    return device.status();
}

} // namespace

// What else is done to this PC's own JetKVMs (P4, c2): power and reset by its ATX extension, a restart, Wake-on-LAN
// and its jiggler, each one call over its connection, as the web's act has them; and what it says it can.
class TestJetKvmActions : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY(qkvm::core::useWindowsOwnTls());
        QVERIFY2(qkvm::core::testing::JetKvmPeerByHand::filmIsThere(), qkvm::core::testing::JetKvmPeerByHand::noFilm);
    }

    void powerAndResetArePressedByItsAtxExtension_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<QString>("press");
        QTest::newRow("power") << u"atx_power"_s << u"power-short"_s;
        QTest::newRow("power long") << u"atx_power_long"_s << u"power-long"_s;
        QTest::newRow("reset") << u"atx_reset"_s << u"reset"_s;
    }
    void powerAndResetArePressedByItsAtxExtension()
    {
        QFETCH(QString, kind);
        QFETCH(QString, press);
        Rig rig;
        rig.viewer.extension = "atx-power";
        const base::Result<Done> pressed = done(rig, Action::plain(kind));
        QVERIFY2(pressed, pressed ? "" : pressed.error().message.c_str());
        QCOMPARE(paramsOf(rig.viewer, u"getActiveExtension"_s).size(), 1);
        const QList<QJsonObject> presses = paramsOf(rig.viewer, u"setATXPowerAction"_s);
        QCOMPARE(presses.size(), 1);
        QCOMPARE(presses.first().value(u"action"_s).toString(), press);
    }

    void noAtxExtensionPressesNothing()
    {
        Rig rig;
        const base::Result<Done> pressed = done(rig, Action::plain(u"atx_power"_s));
        QVERIFY(!pressed);
        QCOMPARE(pressed.error().code, std::string("action_failed"));
        QCOMPARE(pressed.error().message, std::string("This JetKVM has no ATX power extension switched on"));
        QVERIFY(paramsOf(rig.viewer, u"setATXPowerAction"_s).isEmpty());
    }

    // 24d: the extension's question cut off by its own page taking it is "in_use"; not answered, "Nothing was
    // pressed". Either way nothing is pressed.
    void anExtensionQuestionCutByAHandOverIsInUse()
    {
        Rig rig;
        rig.viewer.extension = "atx-power";
        rig.viewer.handOverOn = "getActiveExtension";
        const base::Result<Done> pressed = done(rig, Action::plain(u"atx_reset"_s));
        QVERIFY(!pressed);
        QCOMPARE(pressed.error().code, std::string("in_use"));
        QVERIFY(paramsOf(rig.viewer, u"setATXPowerAction"_s).isEmpty());
    }
    void anExtensionQuestionNotAnsweredPressesNothing()
    {
        JetKvmDevice::Pace pace = quick();
        pace.viewer.connection.callWithinMs = 1500;
        Rig rig(pace);
        rig.viewer.extension = "atx-power";
        rig.viewer.silentOn = "getActiveExtension";
        const base::Result<Done> pressed = done(rig, Action::plain(u"atx_power_long"_s));
        QVERIFY(!pressed);
        QCOMPARE(pressed.error().code, std::string("action_failed"));
        QCOMPARE(pressed.error().message,
                 std::string("Nothing was pressed: qKVM was not told whether the JetKVM's power extension is on"));
        QVERIFY(paramsOf(rig.viewer, u"setATXPowerAction"_s).isEmpty());
    }

    // A restart of the JetKVM itself: forced, as the web asks it; one that went out and had no answer is done (it
    // may be gone before it can answer).
    void aRestartIsAskedForced()
    {
        Rig rig;
        QVERIFY(done(rig, Action::plain(u"reboot"_s)));
        QCOMPARE(paramsOf(rig.viewer, u"reboot"_s).size(), 1);
        QCOMPARE(paramsOf(rig.viewer, u"reboot"_s).first().value(u"force"_s).toBool(), true);
    }
    void aRestartWithNoAnswerIsDone()
    {
        JetKvmDevice::Pace pace = quick();
        pace.viewer.connection.callWithinMs = 1500;
        Rig rig(pace);
        rig.viewer.silentOn = "reboot";
        const base::Result<Done> restarted = done(rig, Action::plain(u"reboot"_s));
        QVERIFY2(restarted, restarted ? "" : restarted.error().message.c_str());
    }

    void wakeOnLanAndTheJiggler()
    {
        Rig rig;
        QVERIFY(done(rig, Action::wake(u"aa:bb:cc:dd:ee:ff"_s)));
        QCOMPARE(paramsOf(rig.viewer, u"sendWOLMagicPacket"_s).first().value(u"macAddress"_s).toString(),
                 u"aa:bb:cc:dd:ee:ff"_s);
        QVERIFY(done(rig, Action::plain(u"jiggler_on"_s)));
        QVERIFY(done(rig, Action::plain(u"jiggler_off"_s)));
        const QList<QJsonObject> jiggler = paramsOf(rig.viewer, u"setJigglerState"_s);
        QCOMPARE(jiggler.size(), 2);
        QCOMPARE(jiggler.at(0).value(u"enabled"_s).toBool(), true);
        QCOMPARE(jiggler.at(1).value(u"enabled"_s).toBool(), false);
    }

    // An address that is none is refused before anything is asked of the JetKVM.
    void aMacThatIsNoneIsRefusedBeforeAnything()
    {
        Rig rig;
        const base::Result<Done> woken = done(rig, Action::wake(u"aa:bb:cc:dd:ee"_s));
        QVERIFY(!woken);
        QCOMPARE(woken.error().code, std::string("invalid"));
        QCOMPARE(rig.jet.offersHeard.size(), 0);
    }

    // The computers it has saved to wake, as it says them: only those with an address that is one.
    void itsWakeTargetsAreAskedOfIt()
    {
        Rig rig;
        rig.viewer.wakeTargets = R"([{"name":"Office PC","macAddress":"11:22:33:44:55:66"},)"
                                 R"({"name":"broken","macAddress":"nope"},{"macAddress":"aa-bb-cc-dd-ee-ff"}])";
        std::optional<base::Result<QList<WakeTarget>>> said;
        rig.device.askWakeTargets([&said](const base::Result<QList<WakeTarget>>& targets) { said = targets; });
        QTRY_VERIFY_WITH_TIMEOUT(said.has_value(), 20000);
        const base::Result<QList<WakeTarget>> targets = said.value_or(QList<WakeTarget>{});
        QVERIFY(targets);
        QCOMPARE(targets.value().size(), 2);
        QCOMPARE(targets.value().at(0).name, u"Office PC"_s);
        QCOMPARE(targets.value().at(0).mac, u"11:22:33:44:55:66"_s);
        QCOMPARE(targets.value().at(1).mac, u"aa-bb-cc-dd-ee-ff"_s);
    }

    // What it says it can (the web's `can` of a JetKVM): power and reset only where its ATX extension is on.
    void itSaysWhatAJetKvmCan()
    {
        const QStringList plain{u"screen"_s, u"keys"_s,   u"text"_s, u"jiggler"_s, u"wol"_s,
                                u"reboot"_s, u"resume"_s, u"live"_s, u"control"_s};
        QCOMPARE(JetKvmDevice::abilities(false), plain);
        QCOMPARE(JetKvmDevice::abilities(true), (plain + QStringList{u"power"_s, u"reset"_s}));
        Rig rig;
        QCOMPARE(lookedAt(rig.device).capabilities, std::optional<QStringList>(plain));
    }
};

QTEST_GUILESS_MAIN(TestJetKvmActions)
#include "test_jetkvm_actions.moc"
