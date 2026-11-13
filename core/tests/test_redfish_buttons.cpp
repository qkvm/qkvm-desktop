#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QTest>
#include <QTimer>

#include <qkvm/core/redfish_answers.h>
#include <qkvm/core/redfish_device.h>
#include <qkvm/core/sign_in_pacing.h>

#include "redfish_by_hand.h"
#include "redfish_device_tests.h"
#include "stand_in.h"

using namespace Qt::StringLiterals;
namespace base = qkvm::base;
using namespace qkvm::core::testing;
using qkvm::core::RedfishDevice;

namespace {

// Has the controller do something, and says what came of it.
base::Result<qkvm::core::Done> doneBy(RedfishDevice& device, const qkvm::core::Action& what)
{
    QList<base::Result<qkvm::core::Done>> answers;
    device.act(what, [&](base::Result<qkvm::core::Done> answer) { answers.append(std::move(answer)); });
    if (!QTest::qWaitFor([&] { return !answers.isEmpty(); }, 30000)) {
        return base::Error{"never", "The call was never answered."};
    }
    return answers.takeFirst();
}

qkvm::core::Action button(const char* kind)
{
    return qkvm::core::Action::plain(QString::fromLatin1(kind));
}

std::string codeOf(const base::Result<qkvm::core::Done>& what)
{
    return what ? std::string("ok") : what.error().code;
}

// Lets go of a driver the moment it would next be handed something on its own loop: as a wall that lets go of a driver
// in the very turn something of it is handed. (What it would have been handed then is gone with it.)
class LetGoAtItsNextCall : public QObject {
public:
    bool armed = false;
    std::function<void()> letGo;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (armed && event->type() == QEvent::MetaCall) {
            armed = false;
            letGo();
            return true;
        }
        return QObject::eventFilter(watched, event);
    }
};

} // namespace

// A server's management controller spoken to by the program itself over Redfish (redfish_device.h),
// against a stand-in that answers as the web's does (redfish_by_hand.h): its power buttons and its
// controller's restart (1a-vi), and its light (1a-vii). Ported from the web's own tests where they are
// named (../app/tests).
class TestRedfishButtons : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // The driver chooses Windows' own TLS, which is refused once TLS is in use: so one is made before
        // anything else touches TLS.
        const RedfishDevice first(RedfishDevice::Where{}, QString(), quick());
    }

    void p1ThePowerButtonsMeanWhatTheyMeanOnAServer()
    {
        // (test_redfish.py:155)
        RedfishByHand bmc;
        RedfishDevice device(at(bmc), u"secret"_s, quick());
        QVERIFY(doneBy(device, button("atx_power"))); // running: asked to shut down
        QCOMPARE(bmc.actions, QStringList{u"system GracefulShutdown"_s});
        QCOMPARE(bmc.power, u"Off"_s);
        QVERIFY(doneBy(device, button("atx_power"))); // off: switched on
        QVERIFY(doneBy(device, button("atx_reset")));
        QVERIFY(doneBy(device, button("atx_power_long")));
        QVERIFY(doneBy(device, button("reboot"))); // the controller itself
        QCOMPARE(bmc.actions, (QStringList{u"system GracefulShutdown"_s, u"system On"_s, u"system ForceRestart"_s,
                                           u"system ForceOff"_s, u"controller ForceRestart"_s}));
        // What a KVM does, a controller does not: said before anything is sent. (Keys, Ctrl+Alt+Del and a text go over
        // its console, as the web's: test_wall_server_screens.)
        const qsizetype asked = bmc.stand->asked.size();
        for (const qkvm::core::Action& other :
             {qkvm::core::Action::wake(u"02:00:5E:00:53:01"_s), qkvm::core::Action::plain(u"jiggler_on"_s),
              qkvm::core::Action::show(1)}) {
            const auto refused = doneBy(device, other);
            QCOMPARE(codeOf(refused), std::string("invalid"));
            QCOMPARE(refused.error().message, std::string("A management controller cannot do that"));
        }
        QCOMPARE(bmc.stand->asked.size(), asked);
    }

    void p2AButtonTheControllerDoesNotOfferIsNotPressedSomeOtherWay()
    {
        // (test_redfish.py:667) "Shut down nicely" is not on offer: pulling the plug is no stand-in. A power cycle
        // does stand in for the reset button.
        RedfishByHand bmc;
        bmc.offered = {u"On"_s, u"ForceOff"_s, u"PowerCycle"_s};
        RedfishDevice device(at(bmc), u"secret"_s, quick());
        const QByteArray reset = "/redfish/v1/Systems/Self/Actions/ComputerSystem.Reset";
        const auto refused = doneBy(device, button("atx_power"));
        QCOMPARE(codeOf(refused), std::string("action_failed"));
        QVERIFY(QString::fromStdString(refused.error().message).contains(u"GracefulShutdown"_s));
        QCOMPARE(bmc.stand->count(reset), 0); // (nothing sent: the controller was not asked to say no)
        QVERIFY(doneBy(device, button("atx_reset")));
        QCOMPARE(bmc.actions, QStringList{u"system PowerCycle"_s});
        // Said on the action itself, the same.
        bmc.offeredOnTheAction = true;
        RedfishDevice again(at(bmc), u"secret"_s, quick());
        QCOMPARE(codeOf(doneBy(again, button("atx_power"))), std::string("action_failed"));
        QCOMPARE(bmc.stand->count(reset), 1);
        QVERIFY(doneBy(again, button("atx_reset")));
        QCOMPARE(bmc.actions, (QStringList{u"system PowerCycle"_s, u"system PowerCycle"_s}));
    }

    void p3SwitchingOnLeavesAloneWhatIsOn()
    {
        // (test_redfish.py:693) Asked for by what is wanted: a server that is there already is left as it is, and
        // says so.
        RedfishByHand bmc;
        RedfishDevice device(at(bmc), u"secret"_s, quick());
        auto left = doneBy(device, button("power_on"));
        QVERIFY(left);
        QCOMPARE(left.value().note, u"already on"_s);
        QVERIFY(bmc.actions.isEmpty());
        QVERIFY(doneBy(device, button("power_off")));
        QCOMPARE(bmc.actions, QStringList{u"system GracefulShutdown"_s});
        left = doneBy(device, button("power_off"));
        QCOMPARE(left.value().note, u"already off"_s);
        const auto on = doneBy(device, button("power_on"));
        QVERIFY(on);
        QCOMPARE(on.value().note, QString());
        QCOMPARE(bmc.actions, (QStringList{u"system GracefulShutdown"_s, u"system On"_s}));
    }

    void p4ACommandAcceptedAndNotCarriedOutIsNotCalledDone()
    {
        // (test_redfish.py:1088) A real controller took the command to cut the power, answered "accepted", and left
        // the server running.
        RedfishByHand bmc;
        bmc.drops = {u"ForceOff"_s, u"On"_s};
        RedfishDevice::Pace pace = quick();
        pace.confirmWithinMs = 600;
        pace.confirmEveryMs = 100;
        RedfishDevice device(at(bmc), u"secret"_s, pace);
        const auto refused = doneBy(device, button("atx_power_long"));
        QCOMPARE(codeOf(refused), std::string("action_failed"));
        QCOMPARE(
            refused.error().message,
            std::string("The controller accepted this, but the server is still on. The command was not carried out."));
        QCOMPARE(bmc.actions, QStringList{u"system ForceOff"_s}); // (sent once, and not again behind anybody's back)
        QVERIFY(!RedfishDevice::mayHaveBeenDone(refused.error()));
        bmc.power = u"Off"_s;
        const auto still = doneBy(device, button("atx_power"));
        QVERIFY(QString::fromStdString(still.error().message).contains(u"the server is still off"_s));
    }

    void p5APowerCommandIsDoneWhenTheServerIsSeenToHaveChanged()
    {
        // (test_redfish.py:1133) What cannot be seen at once is not waited for: a shut down takes its time, a reset
        // shows nothing.
        RedfishByHand bmc;
        bmc.powerTakesMs = 500;
        RedfishDevice::Pace pace = quick();
        pace.confirmEveryMs = 100;
        RedfishDevice device(at(bmc), u"secret"_s, pace);
        QElapsedTimer watch;
        watch.start();
        QVERIFY(doneBy(device, button("atx_power_long")));
        QVERIFY(watch.elapsed() >= 500);
        QCOMPARE(bmc.power, u"Off"_s);
        watch.restart();
        QVERIFY(doneBy(device, button("atx_power")));
        QVERIFY(watch.elapsed() >= 500);
        // The shut down and the reset are done as the controller took them: the command is the last thing the
        // controller was asked before the answer, and no look at the server came between to see whether it went
        // off (it would, 500 ms after the shut down; it never does after the reset). (Counted, not timed: on a
        // loaded PC the two took longer than 500 ms with no look between.)
        for (const char* kind : {"atx_power", "atx_reset"}) {
            QList<base::Result<qkvm::core::Done>> answers;
            qsizetype askedByTheAnswer = 0;
            device.act(button(kind), [&](base::Result<qkvm::core::Done> answer) {
                askedByTheAnswer = bmc.stand->asked.size();
                answers.append(std::move(answer));
            });
            QVERIFY(QTest::qWaitFor([&] { return !answers.isEmpty(); }, 30000));
            QVERIFY2(answers.first(), kind);
            QVERIFY(askedByTheAnswer > 0);
            const Asked& last = bmc.stand->asked.at(askedByTheAnswer - 1);
            QCOMPARE(last.method, "POST"_ba);
            QVERIFY2(last.path.endsWith("/Actions/ComputerSystem.Reset"), last.path.constData());
        }
        QCOMPARE(bmc.actions, (QStringList{u"system ForceOff"_s, u"system On"_s, u"system GracefulShutdown"_s,
                                           u"system ForceRestart"_s}));
    }

    void p6ADriverLetGoOfWhileItConfirmsSaysItMayHaveBeenDone()
    {
        // (test_redfish.py:1107) Taken for one of its moments, the wait ran out and said "not carried out" of a
        // server that may have been switching on.
        RedfishByHand bmc;
        bmc.drops = {u"ForceOff"_s};
        RedfishDevice::Pace pace = quick();
        pace.confirmWithinMs = 5000;
        pace.confirmEveryMs = 100;
        auto device = std::make_unique<RedfishDevice>(at(bmc), u"secret"_s, pace);
        QList<base::Result<qkvm::core::Done>> answers;
        device->act(button("atx_power_long"),
                    [&](base::Result<qkvm::core::Done> said) { answers.append(std::move(said)); });
        QTRY_VERIFY(!bmc.actions.isEmpty());
        QTest::qWait(200);
        device.reset();
        QTRY_COMPARE(answers.size(), 1);
        QCOMPARE(answers.first().error().code, std::string("timeout"));
        QVERIFY(RedfishDevice::mayHaveBeenDone(answers.first().error()));
        QCOMPARE(bmc.actions, QStringList{u"system ForceOff"_s});
    }

    void p7ACommandGoesOnlyUnderTheSessionTheServerWasAskedUnder()
    {
        // (test_which_server_answers.py:208) The new lease comes between the question which server answers and the
        // command: the command goes under the session it was asked under, which the other server does not know; it is
        // not sent under the one the other hands out until that one was asked which it is.
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        bmc.whenAsked = [&](const Asked& asked) {
            if (asked.method == "POST" && asked.path.endsWith("ComputerSystem.Reset") &&
                bmc.uuid == QJsonValue(aSystem)) {
                bmc.beServer(bSystem,
                             aRoot); // (its controller says this card's number: the password goes, the command not)
            }
        };
        RedfishDevice device(keptAs(bmc, markOf(aSystem) + digestOf(aRoot)), u"secret"_s, quick());
        const auto refused = doneBy(device, button("atx_power_long"));
        QCOMPARE(codeOf(refused), std::string("another_machine"));
        QVERIFY(bmc.actions.isEmpty());
        QVERIFY(!RedfishDevice::mayHaveBeenDone(refused.error()));
    }

    void p8WhatCameOfACommandIsSaidAsItIs_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<bool>("never");
        QTest::addColumn<QByteArray>("code");
        QTest::addColumn<bool>("maybe");
        // Never answered: it went, and may have been done. An error of the controller's own: neither yes nor no. A
        // no in its own words: not done.
        QTest::newRow("never answered") << 0 << true << "timeout"_ba << true;
        QTest::newRow("500") << 500 << false << "timeout"_ba << true;
        QTest::newRow("400") << 400 << false << "action_failed"_ba << false;
        // An answer a command does not have: neither yes nor no.
        QTest::newRow("201") << 201 << false << "timeout"_ba << true;
        QTest::newRow("302") << 302 << false << "timeout"_ba << true;
    }

    void p8WhatCameOfACommandIsSaidAsItIs()
    {
        QFETCH(int, status);
        QFETCH(bool, never);
        QFETCH(QByteArray, code);
        QFETCH(bool, maybe);
        RedfishByHand bmc;
        bmc.resetNeverAnswered = never;
        bmc.resetStatus = status;
        RedfishDevice device(at(bmc), u"secret"_s, quick());
        const auto said = doneBy(device, button("atx_reset"));
        QCOMPARE(QByteArray::fromStdString(codeOf(said)), code);
        QCOMPARE(RedfishDevice::mayHaveBeenDone(said.error()), maybe);
        if (status >= 400 && status < 500) {
            QCOMPARE(said.error().message, std::string("Internal error")); // (its no, in its own words)
        }
    }

    void p9AButtonToNobodyWasNotPressed()
    {
        const NobodyThere nobody;
        RedfishDevice::Where where;
        where.address = QUrl(nobody.address());
        RedfishDevice device(where, u"secret"_s, quick());
        const auto refused = doneBy(device, button("atx_power"));
        QCOMPARE(codeOf(refused), std::string("unreachable"));
        QVERIFY(!RedfishDevice::mayHaveBeenDone(refused.error()));
    }

    void p10AControllersRestartWhoseLineDropsIsDone()
    {
        // (redfish.py:2236-2238) A controller that is already going down may never answer.
        RedfishByHand bmc;
        bmc.resetHangsUp = true;
        RedfishDevice device(at(bmc), u"secret"_s, quick());
        QVERIFY(doneBy(device, button("reboot")));
        QCOMPARE(bmc.actions, QStringList{u"controller ForceRestart"_s});
        // A power button whose line drops after it went: it may have been done.
        const auto dropped = doneBy(device, button("atx_reset"));
        QCOMPARE(codeOf(dropped), std::string("dropped"));
        QVERIFY(RedfishDevice::mayHaveBeenDone(dropped.error()));
    }

    void p11WhatWasDoneShowsWithoutWaitingForItsNextTurn()
    {
        // (test_redfish.py:984; redfish.py:96-100) A controller says "accepted" and shows the change a little later:
        // looked at again at the moments after.
        RedfishByHand bmc;
        RedfishDevice::Pace pace = quick();
        pace.freshMs = 60'000;
        pace.followUpMs = {300, 700};
        RedfishDevice device(at(bmc), u"secret"_s, pace);
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QVERIFY(!device.isStale());
        QVERIFY(doneBy(device, button("atx_reset")));
        QVERIFY(device.isStale());
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QVERIFY(!device.isStale());
        QTRY_VERIFY_WITH_TIMEOUT(device.isStale(), 1000);
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QTRY_VERIFY_WITH_TIMEOUT(device.isStale(), 1000);
        QCOMPARE(lookedAt(device).state, u"online"_s);
        QTest::qWait(500);
        QVERIFY(!device.isStale());
    }

    void p12AConfirmingReadThatIsRefusedIsNotTheComputerRefused()
    {
        // The read that confirms a power command is not vital (as the web's), and its computer is no detail the
        // account may be refused: the session ran out just then, it is signed in to again, and the server is seen.
        RedfishByHand bmc;
        bmc.power = u"Off"_s;
        RedfishDevice::Pace pace = quick();
        pace.confirmEveryMs = 100;
        pace.confirmWithinMs = 1500;
        RedfishDevice device(at(bmc), u"secret"_s, pace);
        QCOMPARE(lookedAt(device).state, u"no_signal"_s);
        bool sent = false;
        bmc.whenAsked = [&](const Asked& asked) {
            if (asked.method == "POST" && asked.path.endsWith("ComputerSystem.Reset")) {
                sent = true;
                return;
            }
            if (sent && asked.method == "GET" && asked.path == bmc.system().toLatin1()) {
                sent = false;
                bmc.sessions.clear(); // (that question, and the session it came under, refused)
            }
        };
        const auto done = doneBy(device, button("power_on"));
        QVERIFY2(done, done ? "" : done.error().message.c_str());
        QCOMPARE(bmc.logins, 2);
        QCOMPARE(lookedAt(device).state, u"online"_s);
        bmc.whenAsked = nullptr;
    }

    void q1TheLightIsSwitchedShownTheStateItWasDecidedOn()
    {
        // (test_redfish.py:958) Lit, and put out again: each change showed the controller the state it was decided on
        // (the mark it put on it), and went through at the first try. Looked at again after, as anything done.
        RedfishByHand bmc;
        RedfishDevice::Pace pace = quick();
        pace.freshMs = 60'000;
        RedfishDevice device(at(bmc), u"secret"_s, pace);
        QCOMPARE(readingsOf(lookedAt(device)).locator, std::optional<bool>(false));
        const QByteArray first = bmc.mark.toLatin1();
        QVERIFY(doneBy(device, button("locate_on")));
        QCOMPARE(bmc.led, QJsonValue(u"Lit"_s));
        QVERIFY(device.isStale());
        QCOMPARE(readingsOf(lookedAt(device)).locator, std::optional<bool>(true));
        const QByteArray second = bmc.mark.toLatin1();
        QVERIFY(doneBy(device, button("locate_off")));
        QCOMPARE(readingsOf(lookedAt(device)).locator, std::optional<bool>(false));
        QCOMPARE(bmc.changes, (QList<QJsonObject>{QJsonObject{{u"IndicatorLED"_s, u"Lit"_s}},
                                                  QJsonObject{{u"IndicatorLED"_s, u"Off"_s}}}));
        QCOMPARE(bmc.ifMatches, (QList<QByteArray>{first, second}));
    }

    void q2AControllerThatWillNotTakeBackTheMarkItGaveIsToldTheChangeIsMeant()
    {
        // (test_redfish.py:975) What one of the two boards answered: a weak mark, which a strict comparison never
        // accepts. Asked once more, whatever the state.
        RedfishByHand bmc;
        bmc.strictMarks = true;
        RedfishDevice device(at(bmc), u"secret"_s, quick());
        const QByteArray weak = "W/" + bmc.mark.toLatin1();
        QVERIFY(doneBy(device, button("locate_on")));
        QCOMPARE(bmc.changes, (QList<QJsonObject>{QJsonObject{{u"IndicatorLED"_s, u"Lit"_s}}}));
        QCOMPARE(bmc.ifMatches, (QList<QByteArray>{weak, "*"}));
    }

    void q3AControllerThatPutsNoMarkAndWantsOneIsToldTheChangeIsMeant()
    {
        RedfishByHand bmc;
        bmc.putsNoMark = true;
        RedfishDevice device(at(bmc), u"secret"_s, quick());
        QVERIFY(doneBy(device, button("locate_on")));
        QCOMPARE(bmc.changes, (QList<QJsonObject>{QJsonObject{{u"IndicatorLED"_s, u"Lit"_s}}}));
        QCOMPARE(bmc.ifMatches, (QList<QByteArray>{"(none)", "*"}));
        // One that says its mark in the header alone is shown it, and takes the change at the first try.
        bmc.putsNoMark = false;
        bmc.marksInItsHeaderOnly = true;
        const QByteArray mark = bmc.mark.toLatin1();
        QVERIFY(doneBy(device, button("locate_off")));
        QCOMPARE(bmc.ifMatches, (QList<QByteArray>{"(none)", "*", mark}));
    }

    void q4TheNewerKindOfLightAndNoneAtAll()
    {
        // (test_redfish.py:1017) A plain on and off; and a board without one: nothing is sent.
        RedfishByHand bmc;
        bmc.locating = false;
        RedfishDevice device(at(bmc), u"secret"_s, quick());
        QVERIFY(doneBy(device, button("locate_on")));
        QCOMPARE(bmc.changes, (QList<QJsonObject>{QJsonObject{{u"LocationIndicatorActive"_s, true}}}));
        QCOMPARE(readingsOf(lookedAt(device)).locator, std::optional<bool>(true));
        bmc.locating = QJsonValue();
        bmc.led = u"Unknown"_s;
        QCOMPARE(readingsOf(lookedAt(device)).locator, std::optional<bool>());
        const auto none = doneBy(device, button("locate_on"));
        QCOMPARE(codeOf(none), std::string("action_failed"));
        QCOMPARE(none.error().message, std::string("This controller has no locator light to switch"));
        QCOMPARE(bmc.changes.size(), 1);
        QCOMPARE(bmc.ifMatches.size(), 1);
    }

    void q5WhatCameOfAChangeOfTheLightIsSaidAsItIs()
    {
        // A no in its own words: not done, and not asked again whatever the state (that is for 412 and 428 alone).
        // An error of its own: it may have been done.
        RedfishByHand bmc;
        bmc.lightStatus = 400;
        RedfishDevice device(at(bmc), u"secret"_s, quick());
        const auto refused = doneBy(device, button("locate_on"));
        QCOMPARE(codeOf(refused), std::string("action_failed"));
        QVERIFY(!RedfishDevice::mayHaveBeenDone(refused.error()));
        QCOMPARE(bmc.ifMatches.size(), 1);
        bmc.lightStatus = 500;
        const auto unsure = doneBy(device, button("locate_on"));
        QCOMPARE(codeOf(unsure), std::string("timeout"));
        QVERIFY(RedfishDevice::mayHaveBeenDone(unsure.error()));
        QCOMPARE(bmc.ifMatches.size(), 2);
    }

    void q6TheLightIsChangedOnlyUnderTheSessionTheServerWasAskedUnder()
    {
        // As p7: the new lease comes between the question which server answers and the change. Nothing changes on the
        // other server.
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        bmc.whenAsked = [&](const Asked& asked) {
            if (asked.method == "PATCH" && bmc.uuid == QJsonValue(aSystem)) {
                bmc.beServer(bSystem, aRoot);
            }
        };
        RedfishDevice device(keptAs(bmc, markOf(aSystem) + digestOf(aRoot)), u"secret"_s, quick());
        const auto refused = doneBy(device, button("locate_on"));
        QCOMPARE(codeOf(refused), std::string("another_machine"));
        QVERIFY(bmc.changes.isEmpty());
        QCOMPARE(bmc.led, QJsonValue(u"Off"_s));
        bmc.whenAsked = nullptr;
    }

    // --- after the reading of 1a-vi -----------------------------------------------------------------------------

    void p13AnAnswerIsHandedThoughItsDriverIsLetGoOfInTheTurnItCame()
    {
        RedfishByHand bmc;
        auto device = std::make_unique<RedfishDevice>(at(bmc), u"secret"_s, quick());
        QCOMPARE(lookedAt(*device).state, u"online"_s);
        LetGoAtItsNextCall letGo;
        letGo.letGo = [&device] { QTimer::singleShot(0, [&device] { device.reset(); }); };
        device->installEventFilter(&letGo);
        bmc.whenAsked = [&letGo](const Asked& asked) {
            if (asked.method == "POST" && asked.path.endsWith("ComputerSystem.Reset")) {
                letGo.armed = true;
            }
        };
        QList<base::Result<qkvm::core::Done>> answers;
        device->act(button("atx_reset"),
                    [&answers](base::Result<qkvm::core::Done> said) { answers.append(std::move(said)); });
        QTRY_COMPARE(answers.size(), 1);
        QVERIFY(answers.first());
        bmc.whenAsked = nullptr;
    }

    void p14AConfirmingLookThatNeverCameBackSaysItMayHaveBeenDone()
    {
        // No look at the server came back while a cut of the power was confirmed: whether it was done was not seen.
        // ("Not carried out" would have somebody press it again.)
        RedfishByHand bmc;
        bmc.drops = {u"ForceOff"_s};
        RedfishDevice::Pace pace = quick();
        pace.confirmWithinMs = 600;
        pace.confirmEveryMs = 100;
        RedfishDevice device(at(bmc), u"secret"_s, pace);
        bmc.whenAsked = [&bmc](const Asked& asked) {
            if (asked.method == "POST" && asked.path.endsWith("ComputerSystem.Reset")) {
                bmc.systemUnwell = true;
            }
        };
        const auto unsure = doneBy(device, button("atx_power_long"));
        QCOMPARE(codeOf(unsure), std::string("timeout"));
        QVERIFY(RedfishDevice::mayHaveBeenDone(unsure.error()));
        QCOMPARE(bmc.actions, QStringList{u"system ForceOff"_s});
        bmc.whenAsked = nullptr;
    }

    void p15WhatDependsOnAPowerStateItDoesNotSayIsNotPressed_data()
    {
        QTest::addColumn<QString>("power");
        QTest::newRow("a state that is neither") << u"Unknown"_s;
        QTest::newRow("none said") << QString();
    }

    void p15WhatDependsOnAPowerStateItDoesNotSayIsNotPressed()
    {
        // Counted as on, the power button would shut it down and "switch on" would say "already on". Refused, and
        // said why (stricter than the web); what does not depend on it goes as ever.
        QFETCH(QString, power);
        RedfishByHand bmc;
        bmc.power = power;
        RedfishDevice device(at(bmc), u"secret"_s, quick());
        for (const char* kind : {"power_on", "power_off", "atx_power"}) {
            const auto refused = doneBy(device, button(kind));
            QCOMPARE(codeOf(refused), std::string("action_failed"));
            QVERIFY(QString::fromStdString(refused.error().message).contains(u"nothing was sent"_s));
            QVERIFY(!RedfishDevice::mayHaveBeenDone(refused.error()));
        }
        QVERIFY(bmc.actions.isEmpty());
        QVERIFY(doneBy(device, button("atx_reset")));
        QCOMPARE(bmc.actions, QStringList{u"system ForceRestart"_s});
    }

    void p16APressLetGoOfBeforeItsCommandWentIsStopped()
    {
        RedfishByHand bmc;
        bmc.systemTakesMs = 600;
        auto device = std::make_unique<RedfishDevice>(at(bmc), u"secret"_s, quick());
        QList<base::Result<qkvm::core::Done>> answers;
        device->act(button("atx_reset"),
                    [&answers](base::Result<qkvm::core::Done> said) { answers.append(std::move(said)); });
        QTRY_VERIFY(bmc.asked(u"GET "_s + bmc.system()) > 0);
        device.reset();
        QTRY_COMPARE(answers.size(), 1);
        QCOMPARE(codeOf(answers.first()), std::string("stopped"));
        QVERIFY(!RedfishDevice::mayHaveBeenDone(answers.first().error()));
        QTest::qWait(800);
        QVERIFY(bmc.actions.isEmpty());
    }

    void p17ANewLeaseBeforeItsControllersRestartRestartsNothing()
    {
        // As p7, for the controller's own restart.
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        bmc.whenAsked = [&](const Asked& asked) {
            if (asked.method == "POST" && asked.path.endsWith("Manager.Reset") && bmc.uuid == QJsonValue(aSystem)) {
                bmc.beServer(bSystem, aRoot);
            }
        };
        RedfishDevice device(keptAs(bmc, markOf(aSystem) + digestOf(aRoot)), u"secret"_s, quick());
        const auto refused = doneBy(device, button("reboot"));
        QCOMPARE(codeOf(refused), std::string("another_machine"));
        QVERIFY(bmc.actions.isEmpty());
        bmc.whenAsked = nullptr;
    }

    void p18ASessionEndedBetweenTheQuestionAndTheCommandSendsItOnce()
    {
        // Its session ends after the server was asked which it is: signed in anew, asked once more, and the command
        // sent once, under the new session.
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        RedfishDevice device(keptAs(bmc, markOf(aSystem) + digestOf(aRoot)), u"secret"_s, quick());
        QCOMPARE(lookedAt(device).state, u"online"_s);
        bool once = false;
        bmc.whenAsked = [&](const Asked& asked) {
            if (!once && asked.method == "GET" && asked.path == bmc.system().toLatin1()) {
                once = true;
                QTimer::singleShot(0, &bmc, [&bmc] { bmc.sessions.clear(); }); // (after its answer)
            }
        };
        QVERIFY(doneBy(device, button("atx_reset")));
        QCOMPARE(bmc.actions, QStringList{u"system ForceRestart"_s});
        QCOMPARE(bmc.logins, 2);
        bmc.whenAsked = nullptr;
    }

    void p19ACommandWhoseSignInCameToNothingWasNotSent_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<QByteArray>("at");
        QTest::newRow("its controller's restart") << u"reboot"_s << "Manager.Reset"_ba;
        QTest::newRow("a power button") << u"atx_reset"_s << "ComputerSystem.Reset"_ba;
    }

    void p19ACommandWhoseSignInCameToNothingWasNotSent()
    {
        // A board that keeps one session an account, somebody at its own page: the command answered 401, signed in
        // anew, and its root, read first, hangs up. Nothing went: not "done", and not "may have been done".
        QFETCH(QString, kind);
        QFETCH(QByteArray, at);
        RedfishByHand bmc;
        bmc.beServer(aSystem, aRoot);
        RedfishDevice device(keptAs(bmc, markOf(aSystem) + digestOf(aRoot)), u"secret"_s, quick());
        QCOMPARE(lookedAt(device).state, u"online"_s);
        bmc.whenAsked = [&bmc, at](const Asked& asked) {
            if (asked.method == "POST" && asked.path.endsWith(at)) {
                bmc.sessions.clear();
                bmc.rootHangsUp = 5;
            }
        };
        const auto said = doneBy(device, qkvm::core::Action::plain(kind));
        // (It went, and was refused under its session: not done, and said so.)
        QCOMPARE(codeOf(said), std::string("action_failed"));
        QVERIFY(QString::fromStdString(said.error().message).startsWith(u"The controller refused this"_s));
        QVERIFY(QString::fromStdString(said.error().message).endsWith(u"it was not done."_s));
        QVERIFY(!RedfishDevice::mayHaveBeenDone(said.error()));
        QVERIFY(bmc.actions.isEmpty());
        bmc.whenAsked = nullptr;
    }

    void p20ACommandWhoseServerCouldNotBeAskedWhichItIsWasNotSent()
    {
        // Its computer, read before anything is sent, does not answer in time: nothing went, and that is said (not "it
        // may have been done", as the read's own time-out would read).
        RedfishByHand bmc;
        bmc.systemTakesMs = 2500;
        RedfishDevice device(at(bmc), u"secret"_s, quick());
        for (const char* kind : {"atx_reset", "reboot", "locate_on"}) {
            const auto said = doneBy(device, button(kind));
            QCOMPARE(codeOf(said), std::string("action_failed"));
            QVERIFY(QString::fromStdString(said.error().message).startsWith(u"Nothing was sent"_s));
            QVERIFY(!RedfishDevice::mayHaveBeenDone(said.error()));
        }
        QVERIFY(bmc.actions.isEmpty());
        QVERIFY(bmc.changes.isEmpty());
    }

    void p21WhatSaysNothingWasDoneKeepsItsKind()
    {
        // (The first question's reading.) A refused password, an answer that is none: nothing was done, and what came
        // of it keeps its own words and kind (only what would read as "it may have been done" is said as nothing sent).
        RedfishByHand bmc;
        bmc.systemUnwell = true;
        RedfishDevice device(at(bmc), u"secret"_s, quick());
        QCOMPARE(codeOf(doneBy(device, button("atx_reset"))), std::string("bad_response"));
        RedfishByHand other;
        other.password = u"changed on the controller"_s;
        RedfishDevice refused(at(other), u"secret"_s, quick());
        const auto said = doneBy(refused, button("atx_reset"));
        QCOMPARE(codeOf(said), std::string("auth_failed"));
        QVERIFY(bmc.actions.isEmpty() && other.actions.isEmpty());
        // And where the command's own sign-in anew is refused (its session ended, the password changed meanwhile).
        RedfishByHand third;
        third.beServer(aSystem, aRoot);
        RedfishDevice changed(keptAs(third, markOf(aSystem) + digestOf(aRoot)), u"secret"_s, quick());
        QCOMPARE(lookedAt(changed).state, u"online"_s);
        third.whenAsked = [&third](const Asked& asked) {
            if (asked.method == "POST" && asked.path.endsWith("ComputerSystem.Reset")) {
                third.sessions.clear();
                third.password = u"changed on the controller"_s;
            }
        };
        QCOMPARE(codeOf(doneBy(changed, button("atx_reset"))), std::string("auth_failed"));
        QVERIFY(third.actions.isEmpty());
        third.whenAsked = nullptr;
    }

    void p22WhatAControllerCannotDoIsSaidThoughItsDriverGoesAtOnce()
    {
        RedfishByHand bmc;
        auto device = std::make_unique<RedfishDevice>(at(bmc), u"secret"_s, quick());
        QList<base::Result<qkvm::core::Done>> answers;
        // (Ctrl+Alt+Del goes over its console now, as the web's: something no controller does instead.)
        device->act(button("jiggler_on"),
                    [&answers](base::Result<qkvm::core::Done> said) { answers.append(std::move(said)); });
        device.reset();
        QTRY_COMPARE(answers.size(), 1);
        QCOMPARE(codeOf(answers.first()), std::string("invalid"));
        QVERIFY(bmc.stand->asked.isEmpty());
    }

    void q7AControllerThatRefusesTheChangeMeantWhateverTheStateIsNotAskedAgain()
    {
        RedfishByHand bmc;
        bmc.lightStatus = 412;
        RedfishDevice device(at(bmc), u"secret"_s, quick());
        const auto refused = doneBy(device, button("locate_on"));
        QCOMPARE(codeOf(refused), std::string("action_failed"));
        QCOMPARE(bmc.ifMatches.size(), 2);
        QVERIFY(bmc.changes.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestRedfishButtons)
#include "test_redfish_buttons.moc"
