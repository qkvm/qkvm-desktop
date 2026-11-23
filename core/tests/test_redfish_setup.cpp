#include <atomic>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>
#include <QTimer>

#include <qkvm/core/redfish_answers.h>
#include <qkvm/core/redfish_device.h>
#include <qkvm/core/setup_overrides.h>

#include "redfish_by_hand.h"
#include "redfish_device_tests.h"
#include "redfish_setup_tests.h"
#include "stand_in.h"

#include <qt_windows.h>

using namespace Qt::StringLiterals;
namespace base = qkvm::base;
using namespace qkvm::core::testing;
using qkvm::core::RedfishDevice;

namespace {

// A controller whose mark moves at every read of its computer (one made from a clock), and that takes no mark it is
// shown but "*": the If-Match of every change of its boot override (`changes`) and of every clearing (`clearings`) it
// is asked for, in their order (outside review of 0.2.0's Redfish fixes, round 2, L3).
void marksMoveAtEveryRead(RedfishByHand& bmc, QList<QByteArray>* changes, QList<QByteArray>* clearings)
{
    bmc.strictMarks = true;
    bmc.whenAsked = [&bmc, changes, clearings](const Asked& asked) {
        if (asked.path != bmc.system().toLatin1()) {
            return;
        }
        if (asked.method == "GET") {
            bmc.mark = u"\"%1\""_s.arg(bmc.mark.mid(1, bmc.mark.size() - 2).toLongLong() + 1);
            return;
        }
        QList<QByteArray>* into = asked.body.contains("BiosSetup")  ? changes
                                  : asked.body.contains("Disabled") ? clearings
                                                                    : nullptr;
        if (asked.method == "PATCH" && into != nullptr) {
            into->append(asked.headers.value("if-match", "(none)"));
        }
    };
}

// Whether the marks shown are two that the controller gave, one after the other, then "*": sent three times, the third
// as meant whatever the state.
bool twoMarksThenWhatever(const QList<QByteArray>& shown)
{
    return shown.size() == 3 && shown.at(0).startsWith("W/") && shown.at(1).startsWith("W/") &&
           shown.at(0) != shown.at(1) && shown.at(2) == "*";
}

} // namespace

// Start into BIOS setup on a server of this PC's own wall (the web's `bios_setup`): the override set, the server
// restarted or switched on, given four minutes, and the override cleared again after, whatever happened, wherever it
// still shows: a server is never left booting into its setup; what is kept beside the settings of a press that may have
// left it set, and the way out of a driver let go of mid-way. Against a controller by hand. (A change the controller
// only queued, its task followed, and the prune of records: test_redfish_setup_queued.cpp, a program of its own since
// one program of both ran 236 to 475 s on the loaded build box, past CTest's 400.)
class TestRedfishSetup : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { const RedfishDevice first(RedfishDevice::Where{}, QString(), quick()); }

    void whereItsControllerOffersItTheServerCanStartIntoSetup()
    {
        RedfishByHand bmc;
        RedfishDevice device(at(bmc), bmc.password, setupPace());
        QVERIFY(!lookedAt(device).capabilities.value_or(QStringList()).contains(u"bios_setup"_s));
        bmc.bootTargets = QJsonArray{u"None"_s, u"Pxe"_s};
        QTest::qWait(400);
        QVERIFY(!lookedAt(device).capabilities.value_or(QStringList()).contains(u"bios_setup"_s));
        offersSetup(bmc);
        QTest::qWait(400);
        QVERIFY(lookedAt(device).capabilities.value_or(QStringList()).contains(u"bios_setup"_s));
        // Not offered: refused, nothing sent.
        bmc.bootTargets = QJsonArray{u"None"_s};
        const base::Result<qkvm::core::Done> refused = doneBy(device, "bios_setup");
        QCOMPARE(codeOf(refused), std::string("invalid"));
        QVERIFY(bootChanges(bmc).isEmpty());
        QVERIFY(bmc.actions.isEmpty());
    }

    // A controller that says which kinds of boot override it allows, and no "Once" among them, would keep the BIOS
    // setup override for every boot: the press is refused, in plain words, with nothing sent and nothing noted (the
    // owner's decision of 2026-11-18, a). One that says "Once" among them (in any case), or says nothing of them as a
    // list, is asked as ever.
    void aControllerThatAllowsNoOverrideForOneBootIsRefusedAndSentNothing()
    {
        const QJsonArray onlyAlways{u"Disabled"_s, u"Continuous"_s};
        for (const QJsonValue& kinds : {QJsonValue(onlyAlways), QJsonValue(QJsonArray{})}) {
            RedfishByHand bmc;
            offersSetup(bmc);
            bmc.bootEnabledKinds = kinds;
            const RedfishDevice::Pace pace = setupPace();
            RedfishDevice device(at(bmc), bmc.password, pace);
            const base::Result<qkvm::core::Done> refused = doneBy(device, "bios_setup");
            QCOMPARE(codeOf(refused), std::string("invalid"));
            QVERIFY2(says(refused, "Nothing was done: this controller allows no boot override for the next boot only "
                                   "('Once'), so a start into BIOS setup would leave every later boot in the setup "
                                   "too."),
                     saidBy(refused).c_str());
            QVERIFY(bootChanges(bmc).isEmpty());
            QVERIFY(bmc.actions.isEmpty());
            QVERIFY(pace.overrides->all().isEmpty());
        }
        for (const QJsonValue& kinds : {QJsonValue(QJsonArray{u"Disabled"_s, u"Once"_s, u"Continuous"_s}),
                                        QJsonValue(QJsonArray{u"ONCE"_s}), QJsonValue(u"Once"_s)}) {
            RedfishByHand bmc;
            offersSetup(bmc);
            bmc.bootEnabledKinds = kinds;
            RedfishDevice device(at(bmc), bmc.password, setupPace());
            const base::Result<qkvm::core::Done> done = doneBy(device, "bios_setup");
            QVERIFY2(done.ok(), saidBy(done).c_str());
            QCOMPARE(bmc.actions, QStringList{u"system ForceRestart"_s});
        }
    }

    void aRunningServerIsRestartedIntoItsSetupAndTakesTheOverrideAsItStarts()
    {
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.setupTakesMs = 300;
        const RedfishDevice::Pace pace = setupPace();
        RedfishDevice device(at(bmc), bmc.password, pace);
        const base::Result<qkvm::core::Done> done = doneBy(device, "bios_setup");
        QVERIFY2(done.ok(), saidBy(done).c_str());
        QCOMPARE(bootChanges(bmc), QStringList{u"Once BiosSetup"_s});
        QCOMPARE(bmc.actions, QStringList{u"system ForceRestart"_s});
        QVERIFY(bmc.inSetup);
        QCOMPARE(bmc.bootEnabled, u"Disabled"_s);
        // (Taken: nothing is left noted as maybe set, review 4's MEDIUM 1.)
        QVERIFY(pace.overrides->all().isEmpty());
        // Shown the state it was decided on.
        QVERIFY(!bmc.ifMatches.isEmpty() && bmc.ifMatches.constFirst() != "(none)");
        QVERIFY(signsOnToNothing(bmc));
    }

    void aServerThatIsOffIsSwitchedOnIntoItsSetup()
    {
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.power = u"Off"_s;
        RedfishDevice device(at(bmc), bmc.password, setupPace());
        const base::Result<qkvm::core::Done> done = doneBy(device, "bios_setup");
        QVERIFY2(done.ok(), saidBy(done).c_str());
        QCOMPARE(bmc.actions, QStringList{u"system On"_s});
        QVERIFY(bmc.inSetup);
    }

    void anOverrideTheServerHasNotTakenByTheEndIsClearedAfter()
    {
        // The restart accepted, and nothing came of it: at the press's end the override is cleared, so that no
        // ordinary boot later lands in the setup.
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.drops = {u"ForceRestart"_s};
        RedfishDevice device(at(bmc), bmc.password, setupPace());
        const base::Result<qkvm::core::Done> done = doneBy(device, "bios_setup");
        QCOMPARE(codeOf(done), std::string("timeout"));
        QVERIFY2(says(done, "the BIOS setup boot override was cleared again, so its next boot is an ordinary one"),
                 saidBy(done).c_str());
        QCOMPARE(bootChanges(bmc), (QStringList{u"Once BiosSetup"_s, u"Disabled"_s}));
        QCOMPARE(bmc.bootEnabled, u"Disabled"_s);
    }

    void aServerSwitchedOnThatStaysOffIsSaidSoAndClearedAfter()
    {
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.power = u"Off"_s;
        bmc.drops = {u"On"_s};
        RedfishDevice device(at(bmc), bmc.password, setupPace());
        const base::Result<qkvm::core::Done> done = doneBy(device, "bios_setup");
        QCOMPARE(codeOf(done), std::string("action_failed"));
        QVERIFY2(says(done, "the server is still off"), saidBy(done).c_str());
        QCOMPARE(bmc.bootEnabled, u"Disabled"_s);
    }

    void whereNothingAnswersMayBeSetIsSaid()
    {
        // Nothing of the controller's answers after the restart, to the end and beyond: that the override may be set
        // is said, and how to see to it.
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.startsIntoSetup = false;
        bool once = false;
        bmc.whenAsked = [&bmc, &once](const Asked& asked) {
            if (!once && asked.method == "POST" && asked.path.endsWith("ComputerSystem.Reset")) {
                once = true;
                bmc.unwellFor(60'000);
            }
        };
        RedfishDevice device(at(bmc), bmc.password, setupPace());
        const base::Result<qkvm::core::Done> done = doneBy(device, "bios_setup");
        QCOMPARE(codeOf(done), std::string("timeout"));
        QVERIFY2(says(done, "may be set and could not be cleared"), saidBy(done).c_str());
        QVERIFY2(says(done, "Look at the server's boot override"), saidBy(done).c_str());
    }

    void aStartWatchedByReadsThatShowNoOverrideIsNotTakenForOne()
    {
        // (Review 4's MEDIUM 2.) After the restart its computer shows no Boot at all, and then a null one: neither says
        // that the server took the override. Watched to the end of the press, and cleared then.
        for (const QJsonValue& shown : {QJsonValue(QJsonValue::Undefined), QJsonValue(QJsonValue::Null)}) {
            RedfishByHand bmc;
            offersSetup(bmc);
            bmc.startsIntoSetup = false;
            bmc.whenAsked = [&bmc, shown](const Asked& asked) {
                if (asked.method == "POST" && asked.path.endsWith("ComputerSystem.Reset")) {
                    bmc.bootSaidAs = shown;
                }
            };
            RedfishDevice device(at(bmc), bmc.password, setupPace());
            const base::Result<qkvm::core::Done> done = doneBy(device, "bios_setup");
            QCOMPARE(codeOf(done), std::string("timeout"));
            QVERIFY2(says(done, "was not seen to start into its BIOS setup"), saidBy(done).c_str());
            QCOMPARE(bmc.actions, QStringList{u"system ForceRestart"_s});
            QCOMPARE(bootChanges(bmc), (QStringList{u"Once BiosSetup"_s, u"Disabled"_s}));
            QCOMPARE(bmc.bootEnabled, u"Disabled"_s);
        }
    }

    void aClearingAnsweredTwoHundredThenShownAsNothingIsNoClearing()
    {
        // (Review 4's MEDIUM 2.) The restart refused, the override is cleared again: the clearing is answered 200, and
        // its computer shows an empty Boot after it. That is not seen cleared: it may be set, and that is said.
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.resetStatus = 400;
        bmc.clearAnswer = 200;
        bmc.whenAsked = [&bmc](const Asked& asked) {
            if (asked.method == "PATCH" && asked.body.contains("Disabled")) {
                bmc.bootSaidAs = QJsonObject{};
            }
        };
        RedfishDevice device(at(bmc), bmc.password, setupPace());
        const base::Result<qkvm::core::Done> done = doneBy(device, "bios_setup");
        QCOMPARE(codeOf(done), std::string("timeout"));
        QVERIFY2(says(done, "power change did not go through"), saidBy(done).c_str());
        QVERIFY2(says(done, "may be set and could not be cleared"), saidBy(done).c_str());
        QCOMPARE(bootChanges(bmc), (QStringList{u"Once BiosSetup"_s, u"Disabled"_s}));
    }

    void aRestartRefusedClearsTheOverrideAgain()
    {
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.resetStatus = 400;
        RedfishDevice device(at(bmc), bmc.password, setupPace());
        const base::Result<qkvm::core::Done> done = doneBy(device, "bios_setup");
        QCOMPARE(codeOf(done), std::string("action_failed"));
        QVERIFY2(says(done, "the power change did not go through"), saidBy(done).c_str());
        QVERIFY2(says(done, "the BIOS setup boot override was cleared again"), saidBy(done).c_str());
        QCOMPARE(bootChanges(bmc), (QStringList{u"Once BiosSetup"_s, u"Disabled"_s}));
        QCOMPARE(bmc.bootEnabled, u"Disabled"_s);
    }

    void aRestartWithNoAnswerIsWatched()
    {
        // The controller took the restart and never answered: whether it went is seen by the server taking the
        // override as it starts.
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.resetNeverAnswered = true;
        RedfishDevice device(at(bmc), bmc.password, setupPace());
        const base::Result<qkvm::core::Done> done = doneBy(device, "bios_setup");
        QVERIFY2(done.ok(), saidBy(done).c_str());
        QVERIFY(bmc.inSetup);
    }

    // Another tool's override set before the press is never touched, a BiosSetup one for every boot among them (the
    // owner's decision of 2026-11-18, b: only one shown after the press's change is qKVM's own).
    void anotherToolsOverrideIsLeftAsItIs()
    {
        for (const auto& [enabled, target] :
             {std::pair(u"Once"_s, u"Pxe"_s), std::pair(u"Continuous"_s, u"BiosSetup"_s)}) {
            RedfishByHand bmc;
            offersSetup(bmc);
            bmc.bootEnabled = enabled;
            bmc.bootTarget = target;
            RedfishDevice device(at(bmc), bmc.password, setupPace());
            const base::Result<qkvm::core::Done> done = doneBy(device, "bios_setup");
            QCOMPARE(codeOf(done), std::string("action_failed"));
            QVERIFY2(says(done, qPrintable(u"has a boot override set already (%1, %2)"_s.arg(target, enabled))),
                     saidBy(done).c_str());
            QVERIFY(bootChanges(bmc).isEmpty());
            QVERIFY(bmc.actions.isEmpty());
            QCOMPARE(bmc.bootEnabled, enabled);
        }
    }

    // A board that keeps "Once" as "Continuous" (the owner's decision of 2026-11-18, b): after the press's change, a
    // BiosSetup override kept for every boot is qKVM's own (none was there before the press: it is refused where one
    // is). Its change answered 204, the server is restarted, watched to the press's end (so that it has its time to
    // start into its setup with it), and the override cleared then, in one more change, and said; the record goes, as
    // for any press that cleared what it set. Where that clearing is refused, or answered 202 and read back as kept
    // for every boot still, "may be set" is said, and the record stands.
    void aBoardThatKeepsOnceAsContinuousHasQkvmsOwnOverrideClearedAfterTheRestart()
    {
        enum class Clearing { made, refused, answeredButStillShown };
        for (const Clearing clearing : {Clearing::made, Clearing::refused, Clearing::answeredButStillShown}) {
            const bool refused = clearing != Clearing::made;
            RedfishByHand bmc;
            offersSetup(bmc);
            bmc.keepsOnceAs = u"Continuous"_s;
            if (clearing == Clearing::refused) {
                bmc.clearStatus = 400;
            } else if (clearing == Clearing::answeredButStillShown) {
                stillShownForEveryBootAfterTheClearing(bmc);
                bmc.clearAnswer = 202;
            }
            const RedfishDevice::Pace pace = setupPace();
            RedfishDevice device(at(bmc), bmc.password, pace);
            QElapsedTimer took;
            took.start();
            const base::Result<qkvm::core::Done> pressed = doneBy(device, "bios_setup");
            QCOMPARE(bmc.actions, QStringList{u"system ForceRestart"_s});
            QVERIFY(bmc.inSetup);
            QVERIFY2(says(pressed, "The server was restarted with its BIOS setup boot override set, but its "
                                   "controller kept the override for every boot ('Continuous') in place of the next "
                                   "one only"),
                     saidBy(pressed).c_str());
            QVERIFY2(took.elapsed() >= pace.setupPatienceMs - 100, "qKVM's own override was cleared before the end");
            if (!refused) {
                QVERIFY2(pressed.ok(), saidBy(pressed).c_str());
                QVERIFY2(says(pressed, "qKVM cleared it again, so the server's later boots are ordinary ones"),
                         saidBy(pressed).c_str());
                QCOMPARE(bootChanges(bmc), (QStringList{u"Once BiosSetup"_s, u"Disabled"_s}));
                QCOMPARE(bmc.bootEnabled, u"Disabled"_s);
                QVERIFY(pace.overrides->all().isEmpty());
            } else {
                QCOMPARE(codeOf(pressed), std::string("timeout"));
                QVERIFY2(says(pressed, "may be set and could not be cleared"), saidBy(pressed).c_str());
                QVERIFY(pace.overrides->standing(bmc.address(), device.machine()).has_value());
            }
        }
    }

    // ...and one that shows it so before the restart (its change only queued, and read back as "Continuous", or read so
    // before its task's first look): nothing is restarted, qKVM's own override is cleared at once, and that is said;
    // the queue too, its mark kept.
    void aBoardThatShowsOnceKeptAsContinuousBeforeTheRestartHasItClearedAndRestartsNothing()
    {
        for (const bool followed : {false, true}) {
            RedfishByHand bmc;
            offersSetup(bmc);
            const QString monitor = u"/redfish/v1/TaskService/Tasks/7/Monitor"_s;
            if (followed) {
                bmc.bootQueuedAs = Answered(202, {}, "application/json", {"Location: " + monitor.toLatin1()});
                bmc.tasks.insert(monitor, {Answered(202, {})});
            } else {
                bmc.bootNotKept = true;
            }
            bmc.whenAsked = [&bmc](const Asked& asked) {
                if (asked.method == "PATCH" && asked.body.contains("BiosSetup")) {
                    bmc.bootEnabled = u"Continuous"_s;
                    bmc.bootTarget = u"BiosSetup"_s;
                }
            };
            const RedfishDevice::Pace pace = followed ? taskPace() : setupPace();
            auto device = std::make_unique<RedfishDevice>(at(bmc), bmc.password, pace);
            const base::Result<qkvm::core::Done> pressed = doneBy(*device, "bios_setup");
            QCOMPARE(codeOf(pressed), std::string("action_failed"));
            QVERIFY2(says(pressed, "Nothing was restarted: the controller set the BIOS setup boot override for every "
                                   "boot ('Continuous') in place of the next one only; the BIOS setup boot override "
                                   "was cleared again."),
                     saidBy(pressed).c_str());
            QVERIFY2(says(pressed, "queued the change"), saidBy(pressed).c_str());
            QVERIFY(bmc.actions.isEmpty());
            QCOMPARE(bootChanges(bmc), (QStringList{u"Once BiosSetup"_s, u"Disabled"_s}));
            QCOMPARE(bmc.bootEnabled, u"Disabled"_s);
            QVERIFY(pace.overrides->standing(bmc.address(), device->machine())
                        .value_or(qkvm::core::SetupOverrides::Left{})
                        .queued);
            // (Read before its task's first look, and cleared then.)
            QCOMPARE(bmc.asked(u"GET "_s + monitor), 0);
            device.reset();
            pace.goodbyes->finish(pace.signOutWithinMs);
        }
    }

    // Nor is qKVM's own override kept for every boot an override off to a look of the wall (its record stands, and its
    // card says it, once its doubt is over), nor to the clearing on the way out, which clears it (the owner's decision
    // of 2026-11-18, b).
    void qkvmsOwnOverrideForEveryBootIsNoOverrideOffToALookOrOnTheWayOut()
    {
        {
            RedfishByHand bmc;
            offersSetup(bmc);
            bmc.keepsOnceAs = u"Continuous"_s;
            bmc.clearStatus = 400;
            RedfishDevice::Pace pace = setupPace();
            qint64 steady = 0;
            pace.overrides = std::make_shared<qkvm::core::SetupOverrides>(
                QString(), [] { return QDateTime::currentDateTimeUtc(); }, [&steady] { return steady; });
            auto device = std::make_unique<RedfishDevice>(at(bmc), bmc.password, pace);
            QCOMPARE(codeOf(doneBy(*device, "bios_setup")), std::string("timeout"));
            steady += qkvm::core::SetupOverrides::longestDoubtMs + 1;
            QCOMPARE(bmc.bootEnabled, u"Continuous"_s);
            QVERIFY2(leftOnTheCard(*device) == qkvm::core::SetupOverrides::onTheCard(),
                     "a look took qKVM's own override for every boot for another tool's, and let its record go");
            QVERIFY(pace.overrides->standing(bmc.address(), device->machine()).has_value());
            device.reset();
            pace.goodbyes->finish(pace.signOutWithinMs);
        }
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.keepsOnceAs = u"Continuous"_s;
        RedfishDevice::Pace pace = setupPace();
        pace.setupPatienceMs = 20'000;
        auto device = std::make_unique<RedfishDevice>(at(bmc), bmc.password, pace);
        const auto answer = std::make_shared<std::optional<base::Result<qkvm::core::Done>>>();
        device->act(qkvm::core::Action::plain(u"bios_setup"_s),
                    [answer](const base::Result<qkvm::core::Done>& came) { *answer = came; });
        QTRY_COMPARE_WITH_TIMEOUT(bmc.actions, QStringList{u"system ForceRestart"_s}, 10000);
        QTest::qWait(300);
        device.reset();
        pace.goodbyes->finish(pace.signOutWithinMs);
        QCOMPARE(bootChanges(bmc), (QStringList{u"Once BiosSetup"_s, u"Disabled"_s}));
        QVERIFY2(bmc.bootEnabled == u"Disabled"_s,
                 "the clearing on the way out left qKVM's own override for every boot");
        QVERIFY(!pace.overrides->standing(bmc.address(), QString()).has_value());
        // And where the clearing on the way out is read back as kept for every boot still, for the whole of its time,
        // it is not taken for an override off: the record stands.
        RedfishByHand still;
        offersSetup(still);
        still.keepsOnceAs = u"Continuous"_s;
        stillShownForEveryBootAfterTheClearing(still);
        RedfishDevice::Pace stillPace = setupPace();
        stillPace.setupPatienceMs = 20'000;
        auto stillDevice = std::make_unique<RedfishDevice>(at(still), still.password, stillPace);
        stillDevice->act(qkvm::core::Action::plain(u"bios_setup"_s), [](const base::Result<qkvm::core::Done>&) {});
        QTRY_COMPARE_WITH_TIMEOUT(still.actions, QStringList{u"system ForceRestart"_s}, 10000);
        QTest::qWait(300);
        stillDevice.reset();
        stillPace.goodbyes->finish(stillPace.signOutWithinMs);
        QCOMPARE(bootChanges(still), (QStringList{u"Once BiosSetup"_s, u"Disabled"_s}));
        QVERIFY2(stillPace.overrides->standing(still.address(), QString()).has_value(),
                 "the clearing on the way out took qKVM's own override for every boot, read back, for one off");
    }

    void aChangeAnsweredWithAnErrorIsClearedAndNothingRestarted()
    {
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.bootStatus = 503;
        RedfishDevice device(at(bmc), bmc.password, setupPace());
        const base::Result<qkvm::core::Done> done = doneBy(device, "bios_setup");
        QVERIFY2(says(done, "Nothing was restarted: the controller answered the BIOS setup boot override with an error "
                            "of its own"),
                 saidBy(done).c_str());
        QVERIFY2(says(done, "the BIOS setup boot override was cleared again"), saidBy(done).c_str());
        QVERIFY(bmc.actions.isEmpty());
        QCOMPARE(bootChanges(bmc).constLast(), u"Disabled"_s);
    }

    void aChangeRefusedSetsNothingAndRestartsNothing()
    {
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.bootStatus = 400;
        const RedfishDevice::Pace pace = setupPace();
        RedfishDevice device(at(bmc), bmc.password, pace);
        const base::Result<qkvm::core::Done> done = doneBy(device, "bios_setup");
        QCOMPARE(codeOf(done), std::string("action_failed"));
        QVERIFY(bmc.actions.isEmpty());
        QCOMPARE(bootChanges(bmc), QStringList());
        // (Noted as it went, and let go of at the no: nothing was set, review 4's MEDIUM 1.)
        QVERIFY(pace.overrides->all().isEmpty());
    }

    void aChangeNotShownIsClearedAndNothingRestarted()
    {
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.bootNotKept = true;
        RedfishDevice device(at(bmc), bmc.password, setupPace());
        const base::Result<qkvm::core::Done> done = doneBy(device, "bios_setup");
        QVERIFY2(says(done, "does not show it set"), saidBy(done).c_str());
        QVERIFY(bmc.actions.isEmpty());
        QCOMPARE(bootChanges(bmc).constLast(), u"Disabled"_s);
    }

    void aChangeTooSlowForItsRestartIsClearedAndNothingRestarted()
    {
        // The change is answered only after the restart's whole time, whenever it went (its answer waited for: it is
        // that it comes too late that stops the restart, on a loaded machine too); its clearing at once.
        RedfishByHand bmc;
        offersSetup(bmc);
        RedfishDevice::Pace pace = setupPace();
        bmc.bootTakesMs = pace.setupWithinMs + 500;
        pace.bootQuietMs = pace.setupWithinMs + 2000;
        bmc.whenAsked = [&bmc](const Asked& asked) {
            if (asked.method == "PATCH" && asked.body.contains("Disabled")) {
                bmc.bootTakesMs = 0;
            }
        };
        RedfishDevice device(at(bmc), bmc.password, pace);
        const base::Result<qkvm::core::Done> done = doneBy(device, "bios_setup");
        QVERIFY2(says(done, "took more than 4 seconds before the restart"), saidBy(done).c_str());
        QVERIFY(bmc.actions.isEmpty());
        QCOMPARE(bmc.bootEnabled, u"Disabled"_s);
    }

    void aControllerThatWillNotTakeItsOwnMarkIsToldTheChangeIsMeant()
    {
        // It hands out weak marks and refuses them back (412): the computer read and looked at again, and the change
        // sent as meant whatever the state.
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.strictMarks = true;
        RedfishDevice device(at(bmc), bmc.password, setupPace());
        const base::Result<qkvm::core::Done> done = doneBy(device, "bios_setup");
        QVERIFY2(done.ok(), saidBy(done).c_str());
        QVERIFY(bmc.ifMatches.contains("*"_ba));
        QCOMPARE(bmc.actions, QStringList{u"system ForceRestart"_s});
    }

    void aServerBetweenStatesIsLeftAlone()
    {
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.power = u"PoweringOn"_s;
        RedfishDevice device(at(bmc), bmc.password, setupPace());
        const base::Result<qkvm::core::Done> done = doneBy(device, "bios_setup");
        QVERIFY2(says(done, "Nothing was done: the server is changing its state just now (PoweringOn)"),
                 saidBy(done).c_str());
        QVERIFY(bootChanges(bmc).isEmpty());
        QVERIFY(bmc.actions.isEmpty());
    }

    void anotherServerAtTheAddressIsSentNothing()
    {
        RedfishByHand bmc;
        offersSetup(bmc);
        RedfishDevice device(at(bmc), bmc.password, setupPace());
        QCOMPARE(lookedAt(device).state, u"online"_s);
        bmc.anotherServer();
        const base::Result<qkvm::core::Done> done = doneBy(device, "bios_setup");
        QCOMPARE(codeOf(done), std::string("another_machine"));
        QVERIFY(bootChanges(bmc).isEmpty());
        QVERIFY(bmc.actions.isEmpty());
    }

    // --- the outside review of 0.2.0, area 5 (MEDIUM 5.7 to 5.9) ----------------------------------------------------

    void aServerSwitchedOnThatStaysOffIsNotDoneThoughItsOverrideIsGone()
    {
        // (5.7) Switched on, it stays off, and its controller drops the one-time override all the same: "Disabled" is
        // read while the server is off. That is no start into its setup, and the press says the server is still off
        // ("power is checked, not believed"; before: "Done").
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.power = u"Off"_s;
        bmc.drops = {u"On"_s};
        bmc.whenAsked = [&bmc](const Asked& asked) {
            if (asked.method == "POST" && asked.path.endsWith("ComputerSystem.Reset")) {
                bmc.bootEnabled = u"Disabled"_s;
            }
        };
        RedfishDevice device(at(bmc), bmc.password, setupPace());
        const base::Result<qkvm::core::Done> done = doneBy(device, "bios_setup");
        QCOMPARE(codeOf(done), std::string("action_failed"));
        QVERIFY2(says(done, "the server is still off"), saidBy(done).c_str());
        QCOMPARE(bmc.power, u"Off"_s);
        bmc.whenAsked = nullptr;
    }

    void aClearingAnsweredTwoHundredFourIsReadBack()
    {
        // (5.7) The restart refused, the override is cleared again: the clearing is answered 204 and not made. A 204 is
        // not a read: the override is read back, still shows set, and the press says it may be set (before: cleared,
        // unread, and the record let go of).
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.resetStatus = 400;
        bmc.clearNotKept = true;
        const RedfishDevice::Pace pace = setupPace();
        RedfishDevice device(at(bmc), bmc.password, pace);
        const base::Result<qkvm::core::Done> done = doneBy(device, "bios_setup");
        QCOMPARE(codeOf(done), std::string("timeout"));
        QVERIFY2(says(done, "may be set and could not be cleared"), saidBy(done).c_str());
        QCOMPARE(bmc.bootEnabled, u"Once"_s);
        QVERIFY(pace.overrides->standing(bmc.address(), device.machine()).has_value());
    }

    void anOverrideThatCannotBeReadIsNotTakenForNone()
    {
        // (5.8) A provisioning tool set a one-time network boot, and the controller's read leaves out whether an
        // override is set (no BootSourceOverrideEnabled): that is not "none set". Nothing is changed, nothing
        // restarted.
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.bootSaidAs = QJsonObject{
            {u"BootSourceOverrideTarget"_s, u"Pxe"_s},
            {u"BootSourceOverrideTarget@Redfish.AllowableValues"_s, QJsonArray{u"None"_s, u"Pxe"_s, u"BiosSetup"_s}}};
        const RedfishDevice::Pace pace = setupPace();
        RedfishDevice device(at(bmc), bmc.password, pace);
        const base::Result<qkvm::core::Done> done = doneBy(device, "bios_setup");
        QCOMPARE(codeOf(done), std::string("action_failed"));
        QVERIFY2(says(done, "Nothing was done: the controller does not say whether a boot override is set"),
                 saidBy(done).c_str());
        QVERIFY(bootChanges(bmc).isEmpty());
        QVERIFY(bmc.actions.isEmpty());
        QVERIFY(pace.overrides->all().isEmpty());
    }

    void aChangeRefusedForAStateThatChangedIsSentAgainWithTheNewMark()
    {
        // (5.8) Between the read and the change something else changed the computer (its mark moved on): 412. It is
        // read and looked at again, and the change goes shown the new mark, not "whatever the state": a change that
        // comes between that read and this change is refused again, not written over. "*" only where the controller
        // refuses the very mark it gives (aControllerThatWillNotTakeItsOwnMarkIsToldTheChangeIsMeant).
        RedfishByHand bmc;
        offersSetup(bmc);
        bool once = false;
        bmc.whenAsked = [&bmc, &once](const Asked& asked) {
            if (!once && asked.method == "PATCH" && asked.body.contains("BiosSetup")) {
                once = true;
                bmc.mark = u"\"1791401399\""_s; // (another change came first)
            }
        };
        RedfishDevice device(at(bmc), bmc.password, setupPace());
        const base::Result<qkvm::core::Done> done = doneBy(device, "bios_setup");
        QVERIFY2(done.ok(), saidBy(done).c_str());
        QVERIFY2(!bmc.ifMatches.contains("*"_ba), bmc.ifMatches.join(' ').constData());
        QCOMPARE(bmc.ifMatches.value(1), "\"1791401399\""_ba);
        QCOMPARE(bmc.actions, QStringList{u"system ForceRestart"_s});
        bmc.whenAsked = nullptr;
    }

    void aQueuedMarkThatCannotBeWrittenIsSaid()
    {
        // (5.9) The note is written before the change goes; then the folder of qKVM's settings goes (a disk that
        // filled, a folder taken away) and the controller only queues the change: the mark cannot be written down. That
        // is said by the press (before: by nothing at all), and its card says it is queued for as long as this run
        // lasts.
        const auto folder = std::make_unique<QTemporaryDir>();
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.bootNotKept = true;
        bmc.whenAsked = [&folder](const Asked& asked) {
            if (asked.method == "PATCH" && asked.body.contains("BiosSetup")) {
                QDir(folder->path()).removeRecursively();
            }
        };
        RedfishDevice::Pace pace = setupPace();
        pace.overrides = std::make_shared<qkvm::core::SetupOverrides>(folder->filePath(u"left"_s));
        RedfishDevice device(at(bmc), bmc.password, pace);
        const base::Result<qkvm::core::Done> done = doneBy(device, "bios_setup");
        QVERIFY2(says(done, "queued the change"), saidBy(done).c_str());
        QVERIFY2(says(done, "qKVM could not write down that the controller queued this change"), saidBy(done).c_str());
        QVERIFY(pace.overrides->standing(bmc.address(), device.machine())
                    .value_or(qkvm::core::SetupOverrides::Left{})
                    .queued);
        QCOMPARE(leftOnTheCard(device), qkvm::core::SetupOverrides::onTheCard(true));
        bmc.whenAsked = nullptr;
    }

    void aQueuedMarkThatCouldNotBeWrittenIsNotSaidOnceThePressSawItsChangeThrough()
    {
        // (Outside review of 0.2.0's Redfish fixes, L1.) As above, the mark cannot be written down; then the controller
        // makes the change it queued, the server shows it at the read after the answer, starts into its setup and takes
        // it: the press ends as it should, its record is let go of, and nothing of a mark is left to say (before: "qKVM
        // could not write down that the controller queued this change", of a record no longer there).
        const auto folder = std::make_unique<QTemporaryDir>();
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.bootNotKept = true;
        bool queued = false;
        bmc.whenAsked = [&folder, &bmc, &queued](const Asked& asked) {
            if (asked.method == "PATCH" && asked.body.contains("BiosSetup")) {
                QDir(folder->path()).removeRecursively();
                queued = true;
            } else if (queued && asked.method == "GET" && asked.path == bmc.system().toLatin1()) {
                // (Made by the controller before the computer's next read.)
                bmc.bootEnabled = u"Once"_s;
                bmc.bootTarget = u"BiosSetup"_s;
                queued = false;
            }
        };
        RedfishDevice::Pace pace = setupPace();
        pace.overrides = std::make_shared<qkvm::core::SetupOverrides>(folder->filePath(u"left"_s));
        RedfishDevice device(at(bmc), bmc.password, pace);
        const base::Result<qkvm::core::Done> done = doneBy(device, "bios_setup");
        QVERIFY2(done.ok(), saidBy(done).c_str());
        QCOMPARE(bmc.actions, QStringList{u"system ForceRestart"_s});
        QVERIFY(bmc.inSetup);
        QVERIFY2(!says(done, "queued"), saidBy(done).c_str());
        QVERIFY2(!says(done, "could not write down"), saidBy(done).c_str());
        QVERIFY(!pace.overrides->standing(bmc.address(), device.machine()).has_value());
        bmc.whenAsked = nullptr;
    }

    void aQueuedMarkOfAnEarlierPressIsNotSaidUnwrittenByALaterOne()
    {
        // (Outside review of 0.2.0's Redfish fixes, round 2, L4.) An earlier press left the server's record marked
        // queued, and that mark was written down. A later press is queued too, and its writing of the mark fails (the
        // file held by another program from its change on, so that the file keeps what it had): the mark that stands is
        // the earlier press's, and in the file. The later press says nothing of a mark not written down, whether it
        // does not see its change through or does (its task done, then the server shows it; its restart refused).
        // Before: "qKVM could not write down that the controller queued this change ... after a restart of qKVM nothing
        // would", of a mark the file holds.
        using Left = qkvm::core::SetupOverrides::Left;
        for (const bool seenThrough : {false, true}) {
            const QTemporaryDir folder;
            const QString file = folder.filePath(u"left"_s);
            RedfishByHand bmc;
            offersSetup(bmc);
            bmc.bootNotKept = true;
            RedfishDevice::Pace pace = taskPace();
            pace.overrides = std::make_shared<qkvm::core::SetupOverrides>(file, nullptr, nullptr,
                                                                          qkvm::core::Settings::Patience{3, 20});
            RedfishDevice device(at(bmc), bmc.password, pace);
            const base::Result<qkvm::core::Done> earlier = doneBy(device, "bios_setup");
            QVERIFY2(says(earlier, "queued the change"), saidBy(earlier).c_str());
            QVERIFY2(!says(earlier, "could not write down"), saidBy(earlier).c_str());
            QVERIFY(qkvm::core::SetupOverrides(file).standing(bmc.address(), device.machine()).value_or(Left{}).queued);
            const QString place = u"/redfish/v1/TaskService/Tasks/7"_s;
            if (seenThrough) {
                const auto task = [&bmc](const char* state) {
                    return Answered(
                        200, QByteArray("{\"@odata.id\": \"/redfish/v1/TaskService/Tasks/7\", \"TaskState\": \"") +
                                 state + "\", \"Payload\": {\"TargetUri\": \"" + bmc.system().toLatin1() +
                                 "\", \"HttpOperation\": \"PATCH\"}}");
                };
                bmc.bootNotKept = false;
                bmc.bootQueuedAs = task("New");
                bmc.tasks.insert(place, {task("Running"), task("Completed")});
                bmc.resetStatus = 400;
            }
            HANDLE held = INVALID_HANDLE_VALUE;
            const auto letGo = qScopeGuard([&held] {
                if (held != INVALID_HANDLE_VALUE) {
                    CloseHandle(held);
                }
            });
            int looked = 0;
            bmc.whenAsked = [&bmc, &held, &looked, &file, &place, seenThrough](const Asked& asked) {
                if (asked.method == "PATCH" && asked.body.contains("BiosSetup") && held == INVALID_HANDLE_VALUE) {
                    held = holdAlone(file);
                } else if (seenThrough && asked.method == "GET" && asked.path == place.toLatin1() && ++looked == 2) {
                    // (The controller makes the change as its task ends.)
                    bmc.bootEnabled = u"Once"_s;
                    bmc.bootTarget = u"BiosSetup"_s;
                }
            };
            const base::Result<qkvm::core::Done> later = doneBy(device, "bios_setup");
            QVERIFY(held != INVALID_HANDLE_VALUE);
            QVERIFY2(says(later, "the BIOS setup boot override was cleared again"), saidBy(later).c_str());
            QCOMPARE(says(later, "queued the change"), !seenThrough);
            QVERIFY2(!says(later, "could not write down"), saidBy(later).c_str());
            QCOMPARE(pace.overrides->written(), std::optional<bool>(false));
            QVERIFY(pace.overrides->standing(bmc.address(), device.machine()).value_or(Left{}).queued);
            bmc.whenAsked = nullptr;
            CloseHandle(std::exchange(held, INVALID_HANDLE_VALUE));
            QVERIFY(qkvm::core::SetupOverrides(file).standing(bmc.address(), device.machine()).value_or(Left{}).queued);
        }
    }

    void aClearingRefusedForAStateThatChangedIsSentAgainWithTheNewMark()
    {
        // (Outside review of 0.2.0's Redfish fixes, L9.) The restart refused, the override is cleared again; between
        // the read before the clearing and the clearing something else changed the computer (its mark moved on): 412.
        // It is read and looked at again, and the clearing goes shown the new mark, as the change does (5.8): a
        // provisioning tool's one-time override set after that read is refused again, not written over with "Disabled"
        // (before: "*" after one refusal).
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.resetStatus = 400;
        bool once = false;
        bmc.whenAsked = [&bmc, &once](const Asked& asked) {
            if (!once && asked.method == "PATCH" && asked.body.contains("Disabled")) {
                once = true;
                bmc.mark = u"\"1791401399\""_s; // (another change came first)
            }
        };
        RedfishDevice device(at(bmc), bmc.password, setupPace());
        const base::Result<qkvm::core::Done> done = doneBy(device, "bios_setup");
        QVERIFY2(says(done, "the BIOS setup boot override was cleared again"), saidBy(done).c_str());
        QVERIFY2(!bmc.ifMatches.contains("*"_ba), bmc.ifMatches.join(' ').constData());
        QCOMPARE(bmc.ifMatches.size(), 3);
        QCOMPARE(bmc.ifMatches.value(2), "\"1791401399\""_ba);
        QCOMPARE(bmc.bootEnabled, u"Disabled"_s);
        bmc.whenAsked = nullptr;
    }

    void aChangeRefusedAtEveryMarkIsSentThreeTimesAtTheMost()
    {
        // (Outside review of 0.2.0's Redfish fixes, round 2, L3.) A controller whose mark moves at every read and that
        // refuses every mark it is shown: refused, the change is read and looked at again and sent shown the fresh
        // mark; refused twice, as meant whatever the state ("*"). Three changes, and no fourth (without the bound of
        // two refusals, it is sent again for as long as the marks move).
        RedfishByHand bmc;
        offersSetup(bmc);
        QList<QByteArray> changes;
        marksMoveAtEveryRead(bmc, &changes, nullptr);
        RedfishDevice device(at(bmc), bmc.password, setupPace());
        const base::Result<qkvm::core::Done> done = doneBy(device, "bios_setup");
        QVERIFY2(done.ok(), saidBy(done).c_str());
        QVERIFY2(twoMarksThenWhatever(changes), changes.join(' ').constData());
        QCOMPARE(bmc.actions, QStringList{u"system ForceRestart"_s});
        bmc.whenAsked = nullptr;
    }

    void aClearingRefusedAtEveryMarkIsSentThreeTimesAtTheMost()
    {
        // (Round 2, L3.) The same for the press's clearing: the restart refused, the override cleared again at a
        // controller whose mark moves at every read and that refuses every mark it is shown. Three clearings, the third
        // "*", and no fourth (without the bound, sent again for as long as the marks move: the clearing holds to no
        // time of its own).
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.resetStatus = 400;
        QList<QByteArray> changes;
        QList<QByteArray> clearings;
        marksMoveAtEveryRead(bmc, &changes, &clearings);
        RedfishDevice device(at(bmc), bmc.password, setupPace());
        const base::Result<qkvm::core::Done> done = doneBy(device, "bios_setup");
        QVERIFY2(says(done, "the BIOS setup boot override was cleared again"), saidBy(done).c_str());
        QVERIFY2(twoMarksThenWhatever(clearings), clearings.join(' ').constData());
        QCOMPARE(bmc.bootEnabled, u"Disabled"_s);
        QVERIFY(bmc.actions.isEmpty());
        bmc.whenAsked = nullptr;
    }

    void aChangeWhoseAnswerWasLostIsMarkedAsOneTheControllerMayHaveQueued()
    {
        // (5.9) The change goes and its answer never comes in its time: it may have been a 202, and a queued change can
        // be made after any look. Its record is marked queued at once, as at a 202, so that a look after its doubt (in
        // this run or the next) does not let it go, and the press says so (before: an ordinary note, let go of by the
        // next look showing it off).
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.bootTakesMs = 4000; // (past the 1.5 s the change's answer is waited for)
        const RedfishDevice::Pace pace = setupPace();
        RedfishDevice device(at(bmc), bmc.password, pace);
        const base::Result<qkvm::core::Done> done = doneBy(device, "bios_setup");
        QVERIFY(!done.ok());
        QVERIFY2(says(done, "did not come or could not be read"), saidBy(done).c_str());
        QVERIFY2(says(done, "The controller's answer to the change did not come, so it may have queued it"),
                 saidBy(done).c_str());
        QVERIFY(bmc.actions.isEmpty());
        QVERIFY(pace.overrides->standing(bmc.address(), device.machine())
                    .value_or(qkvm::core::SetupOverrides::Left{})
                    .queued);
    }

    // --- the outside review's second pass of 0.2.0 (outside review 2, 3.3, 3.5 and 3.6) ------------------------------

    void aChangeIsMarkedAsOneTheControllerMayQueueBeforeItGoes()
    {
        // (Outside review 2, 3.5.) The controller takes a while over the change, and the program ends while its answer
        // is out (quit; a crash or Windows' restart, which hand nothing over, leave the file as it stands when the
        // change goes). The server's record is marked as one the controller may have queued, written down before the
        // change went; it stays so through the clearing on the way out and through a later look that shows the override
        // off (before: an ordinary note, which that look let go of, so that a change the controller queued would land
        // at a later boot with nothing left to say so). The press says so.
        using Left = qkvm::core::SetupOverrides::Left;
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"left"_s);
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.bootTakesMs = 2000;
        std::optional<bool> markedAsItWent;
        bmc.whenAsked = [&markedAsItWent, &bmc, file](const Asked& asked) {
            if (!markedAsItWent && asked.method == "PATCH" && asked.body.contains("BiosSetup")) {
                markedAsItWent =
                    qkvm::core::SetupOverrides(file).standing(bmc.address(), QString()).value_or(Left{}).queued;
            }
        };
        RedfishDevice::Pace pace = setupPace();
        pace.connectWithinMs = 15'000;
        pace.quietMs = 15'000;
        pace.setupWithinMs = 30'000;
        qint64 steady = 0;
        pace.overrides = std::make_shared<qkvm::core::SetupOverrides>(
            file, [] { return QDateTime::currentDateTimeUtc(); }, [&steady] { return steady; });
        auto device = std::make_unique<RedfishDevice>(at(bmc), bmc.password, pace);
        const auto answer = std::make_shared<std::optional<base::Result<qkvm::core::Done>>>();
        device->act(qkvm::core::Action::plain(u"bios_setup"_s),
                    [answer](const base::Result<qkvm::core::Done>& came) { *answer = came; });
        QVERIFY(QTest::qWaitFor([&markedAsItWent, &answer] { return markedAsItWent || answer->has_value(); }, 60'000));
        QVERIFY2(markedAsItWent.has_value(), answer->has_value() ? saidBy(**answer).c_str() : "");
        QVERIFY2(*markedAsItWent, "the change went before its record was marked as one the controller may queue");
        const QString machine = device->machine();
        device.reset();
        pace.goodbyes->finish(pace.signOutWithinMs);
        QCOMPARE(bmc.bootEnabled, u"Disabled"_s);
        QTRY_VERIFY(answer->has_value());
        QVERIFY2(says(**answer, "the BIOS setup boot override may be set"), saidBy(**answer).c_str());
        QVERIFY2(says(**answer, "The controller's answer to the change did not come, so it may have queued it"),
                 saidBy(**answer).c_str());
        QVERIFY(pace.overrides->standing(bmc.address(), machine).value_or(Left{}).queued);
        QVERIFY(qkvm::core::SetupOverrides(file).standing(bmc.address(), machine).value_or(Left{}).queued);
        // A look of a driver made anew, its doubt over and the override off: it stands, and the card says the queue.
        steady += qkvm::core::SetupOverrides::longestDoubtMs + 1;
        bmc.bootTakesMs = 0;
        RedfishDevice after(at(bmc), bmc.password, pace);
        QCOMPARE(leftOnTheCard(after), qkvm::core::SetupOverrides::onTheCard(true));
        QVERIFY(pace.overrides->standing(bmc.address(), machine).value_or(Left{}).queued);
        bmc.whenAsked = nullptr;
    }

    void aChangeAnsweredAsMadeTakesTheMarkItWentWithOff()
    {
        // (Outside review 2, 3.5.) The mark the change went with comes off again where its answer says the change was
        // not queued: made (204, its restart then refused), refused, refused for a state that changed (another tool's
        // override set first, which the read after it shows), or never sent (another server answers after a new
        // sign-in); a mark an earlier press put on stays.
        using Left = qkvm::core::SetupOverrides::Left;
        enum class Answer { made, refused, changedFirst, neverSent };
        for (const Answer answer : {Answer::made, Answer::refused, Answer::changedFirst, Answer::neverSent}) {
            RedfishByHand bmc;
            offersSetup(bmc);
            bmc.bootStatus = answer == Answer::refused ? 400 : 0;
            bmc.resetStatus = 400; // (nothing restarted, and the override cleared again: the record goes)
            bool once = false;
            bmc.whenAsked = [&bmc, &once, answer](const Asked& asked) {
                if (once || asked.method != "PATCH" || !asked.body.contains("BiosSetup")) {
                    return;
                }
                once = true;
                if (answer == Answer::changedFirst) {
                    bmc.mark = u"\"1791401399\""_s;
                    bmc.bootEnabled = u"Once"_s;
                    bmc.bootTarget = u"Pxe"_s;
                } else if (answer == Answer::neverSent) {
                    bmc.beServer(bSystem, bmc.rootUuid);
                }
            };
            const RedfishDevice::Pace pace = setupPace();
            RedfishDevice device(at(bmc), bmc.password, pace);
            const base::Result<qkvm::core::Done> done = doneBy(device, "bios_setup");
            QVERIFY(!done.ok());
            QVERIFY2(!says(done, "queued"), saidBy(done).c_str());
            QVERIFY2(!pace.overrides->standing(bmc.address(), device.machine()).value_or(Left{}).queued,
                     saidBy(done).c_str());
            QVERIFY(bmc.actions.isEmpty());
            bmc.whenAsked = nullptr;
        }
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.bootStatus = 400;
        const RedfishDevice::Pace pace = setupPace();
        static_cast<void>(pace.overrides->mayBeSet(bmc.address(), QString(), QDateTime::currentDateTimeUtc()));
        QVERIFY(pace.overrides->markQueued(bmc.address(), QString()));
        RedfishDevice device(at(bmc), bmc.password, pace);
        QVERIFY(!doneBy(device, "bios_setup").ok());
        QVERIFY(pace.overrides->standing(bmc.address(), device.machine()).value_or(Left{}).queued);
    }

    void aServerSwitchedOnThatStaysOffIsSaidStillOffWhereItsOverrideIsKeptForEveryBoot()
    {
        // (Outside review 2, 3.6.) A board that keeps "Once" as "Continuous": switched on, the server stays off. Its
        // override is cleared at the press's end, as qKVM's own, and the press says the server is still off (before:
        // "The server was switched on with its BIOS setup boot override set", and done).
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.power = u"Off"_s;
        bmc.drops = {u"On"_s};
        bmc.keepsOnceAs = u"Continuous"_s;
        const RedfishDevice::Pace pace = setupPace();
        RedfishDevice device(at(bmc), bmc.password, pace);
        const base::Result<qkvm::core::Done> done = doneBy(device, "bios_setup");
        QCOMPARE(codeOf(done), std::string("action_failed"));
        QVERIFY2(says(done, "the server is still off"), saidBy(done).c_str());
        QVERIFY2(says(done, "kept the override for every boot ('Continuous')"), saidBy(done).c_str());
        QVERIFY2(!says(done, "was switched on"), saidBy(done).c_str());
        QCOMPARE(bmc.power, u"Off"_s);
        QCOMPARE(bootChanges(bmc), (QStringList{u"Once BiosSetup"_s, u"Disabled"_s}));
        QCOMPARE(bmc.bootEnabled, u"Disabled"_s);
    }

    void aServerSwitchedOnThatStaysOffIsSaidStillOffBesideAnotherToolsOverride()
    {
        // (Outside review 2, 3.6.) Switched on, the server stays off, and another tool's override shows meanwhile: it
        // is left as it is, the server watched to the press's end, and the press says it is still off (before: done, at
        // the first read that showed that override, whatever the server's power).
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.power = u"Off"_s;
        bmc.drops = {u"On"_s};
        bmc.whenAsked = [&bmc](const Asked& asked) {
            if (asked.method == "POST" && asked.path.endsWith("ComputerSystem.Reset")) {
                bmc.bootEnabled = u"Once"_s;
                bmc.bootTarget = u"Pxe"_s;
            }
        };
        const RedfishDevice::Pace pace = setupPace();
        RedfishDevice device(at(bmc), bmc.password, pace);
        QElapsedTimer took;
        took.start();
        const base::Result<qkvm::core::Done> done = doneBy(device, "bios_setup");
        QCOMPARE(codeOf(done), std::string("action_failed"));
        QVERIFY2(says(done, "the server is still off"), saidBy(done).c_str());
        QVERIFY2(says(done, "another tool's now (Pxe, Once), and was left as it is"), saidBy(done).c_str());
        QVERIFY2(took.elapsed() >= pace.setupPatienceMs - 100, "the press ended before the server had its time");
        QCOMPARE(bmc.power, u"Off"_s);
        QCOMPARE(bootChanges(bmc), QStringList{u"Once BiosSetup"_s});
        QCOMPARE(bmc.bootEnabled, u"Once"_s);
        QCOMPARE(bmc.bootTarget, u"Pxe"_s);
        bmc.whenAsked = nullptr;
    }

    void aRestartThatRestsOnItsComputerListedAloneReadsTheListAnew_data()
    {
        QTest::addColumn<bool>("anotherComesIn");
        QTest::newRow("listed alone still") << false;
        QTest::newRow("another node comes in during the press") << true;
    }

    void aRestartThatRestsOnItsComputerListedAloneReadsTheListAnew()
    {
        // (Outside review 2, 3.3.) Its computer found by its number, alone, at its place; it gives none now (as some
        // boards do at times). The restart, a power command of its own some time after the press began, reads its
        // controller's list anew before it goes: listed alone still, it is restarted into its setup as ever; where
        // another node came in meanwhile, nothing is restarted, nothing is cleared on a computer that cannot be told,
        // and the press says the override may be set (before: the restart went on the list of the press's start).
        QFETCH(bool, anotherComesIn);
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.beServer(aSystem, aRoot);
        bmc.setupTakesMs = 300;
        const QString kept = markOf(aSystem) + digestOf(aRoot);
        const qkvm::core::ServerPlace foundAt{bmc.system(), false};
        bmc.uuid = zero;
        bmc.whenAsked = [&bmc, anotherComesIn](const Asked& asked) {
            if (anotherComesIn && asked.method == "PATCH" && asked.body.contains("BiosSetup")) {
                bmc.others = {{u"Node2"_s, QJsonValue(), u"Off"_s}};
            }
        };
        const RedfishDevice::Pace pace = setupPace();
        RedfishDevice device(keptAs(bmc, kept, foundAt), bmc.password, pace);
        const base::Result<qkvm::core::Done> done = doneBy(device, "bios_setup");
        if (!anotherComesIn) {
            QVERIFY2(done.ok(), saidBy(done).c_str());
            QCOMPARE(bmc.actions, QStringList{u"system ForceRestart"_s});
            QVERIFY(bmc.inSetup);
        } else {
            QCOMPARE(codeOf(done), std::string("timeout"));
            QVERIFY2(says(done, "may be set and could not be cleared"), saidBy(done).c_str());
            QVERIFY2(bmc.actions.isEmpty(), qPrintable(bmc.actions.join(u", "_s)));
            QCOMPARE(bootChanges(bmc), QStringList{u"Once BiosSetup"_s});
            QCOMPARE(bmc.others.at(0).power, u"Off"_s);
            QVERIFY(pace.overrides->standing(bmc.address(), device.machine()).has_value());
        }
        bmc.whenAsked = nullptr;
    }

    void aStartFromOffIsSaidAsItWasSeen_data()
    {
        QTest::addColumn<QString>("way");
        // Known by its controller alone, the server switched on gives its number: watched as ever, its card asks after.
        QTest::newRow("its server gives its number once on, and takes the override") << u"takes"_s;
        QTest::newRow("its server gives its number once on, and does not take the override") << u"doesNotTake"_s;
        // Switched on, and no read of it comes back after (a controller busy through the server's start).
        QTest::newRow("no read after the switch-on") << u"unread"_s;
    }

    void aStartFromOffIsSaidAsItWasSeen()
    {
        // (Review cx2 of outside review 2, M2, and the owner's decision of 2026-11-22.) A card added while its server
        // was off, which gave no number then, is known by its controller alone. Start into BIOS setup switches the
        // server on, and the server gives its number once its BIOS has run. Its card asks whether that is its server,
        // and until that is answered nothing but looks goes to it: the start is watched by reads all the same, and said
        // as it was seen. Taken, it is done; not taken, the clearing does not go, and the press says why; and where no
        // read came back after the switch-on, the server is not said to be still off (before: every read after it was
        // refused, and the press said "still off" of a server on in its setup).
        QFETCH(QString, way);
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.beServer(zero, aRoot);
        bmc.power = u"Off"_s;
        bmc.setupTakesMs = 300;
        bmc.startsIntoSetup = way == u"takes"_s;
        const QString kept = QString(qkvm::core::redfish::saidNone) + digestOf(aRoot);
        const qkvm::core::ServerPlace takenAt{bmc.system(), false};
        bmc.whenAsked = [&bmc, way](const Asked& asked) {
            if (asked.method == "POST" && asked.path.endsWith("ComputerSystem.Reset")) {
                if (way == u"unread"_s) {
                    bmc.systemUnwell = true;
                } else {
                    bmc.uuid = aSystem;
                }
            }
        };
        if (way == u"unread"_s) {
            bmc.drops = {u"On"_s};
        }
        const RedfishDevice::Pace pace = setupPace();
        RedfishDevice device(keptAs(bmc, kept, takenAt), bmc.password, pace);
        const base::Result<qkvm::core::Done> done = doneBy(device, "bios_setup");
        QCOMPARE(bmc.actions, QStringList{u"system On"_s});
        QVERIFY2(!says(done, "still off"), saidBy(done).c_str());
        if (way == u"takes"_s) {
            QVERIFY2(done.ok(), saidBy(done).c_str());
            QVERIFY(bmc.inSetup);
        } else {
            QCOMPARE(codeOf(done), std::string("timeout"));
            QVERIFY2(says(done, "was not seen to start into its BIOS setup"), saidBy(done).c_str());
        }
        if (way == u"doesNotTake"_s) {
            QVERIFY2(says(done, "its card asks whether it is the server"), saidBy(done).c_str());
            QCOMPARE(bootChanges(bmc), QStringList{u"Once BiosSetup"_s});
        }
        bmc.whenAsked = nullptr;
        if (way != u"unread"_s) {
            QVERIFY(lookedAt(device).asksItsServer);
        }
    }

    void theClearingOnTheWayOutChangesNothingOnAComputerThatGivesANumberWhereTheCardKnowsNone()
    {
        // (Review cx2 of outside review 2, L6.) A card known by its controller alone: its change is out when its driver
        // is let go of, and by then the computer at its place gives a number of its own, which its card has not taken.
        // The clearing on the way out reads it and changes nothing there (before: it sent "Disabled" to it).
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.beServer(zero, aRoot);
        bmc.bootTakesMs = 1000;
        bmc.whenAsked = [&bmc](const Asked& asked) {
            if (asked.method == "PATCH" && asked.body.contains("BiosSetup")) {
                bmc.uuid = aSystem;
            }
        };
        RedfishDevice::Pace pace = setupPace();
        auto device =
            std::make_unique<RedfishDevice>(keptAs(bmc, QString(qkvm::core::redfish::saidNone) + digestOf(aRoot),
                                                   qkvm::core::ServerPlace{bmc.system(), false}),
                                            bmc.password, pace);
        device->act(qkvm::core::Action::plain(u"bios_setup"_s), [](const base::Result<qkvm::core::Done>&) {});
        QTRY_COMPARE_WITH_TIMEOUT(bootChanges(bmc), QStringList{u"Once BiosSetup"_s}, 10000);
        device.reset();
        pace.goodbyes->finish(pace.signOutWithinMs);
        QCOMPARE(bootChanges(bmc), QStringList{u"Once BiosSetup"_s});
        QCOMPARE(bmc.bootEnabled, u"Once"_s);
        QVERIFY(pace.overrides->standing(bmc.address(), QString()).has_value());
        bmc.whenAsked = nullptr;
    }

    void aChangeWhoseMarkCannotBeWrittenAsItGoesIsNotSent()
    {
        // (Review cx2 of outside review 2, L3.) The change's note is written; its first try is answered 401 (the
        // session ran out), and meanwhile the folder of qKVM's settings goes. Asked again after a new sign-in, the
        // change is to be marked as one the controller may queue as it goes, and that cannot be written down: it does
        // not go, as a change whose note cannot be written (before: it went with its mark in memory alone).
        const auto folder = std::make_unique<QTemporaryDir>();
        RedfishByHand bmc;
        offersSetup(bmc);
        bool once = false;
        bmc.whenAsked = [&bmc, &folder, &once](const Asked& asked) {
            if (!once && asked.method == "PATCH" && asked.body.contains("BiosSetup")) {
                once = true;
                bmc.sessions.clear();
                QDir(folder->path()).removeRecursively();
            }
        };
        RedfishDevice::Pace pace = setupPace();
        pace.overrides = std::make_shared<qkvm::core::SetupOverrides>(folder->filePath(u"left"_s));
        RedfishDevice device(at(bmc), bmc.password, pace);
        const base::Result<qkvm::core::Done> done = doneBy(device, "bios_setup");
        QVERIFY(once);
        QVERIFY(!done.ok());
        QVERIFY2(says(done, "qKVM could not save the note that this server may boot into its BIOS setup"),
                 saidBy(done).c_str());
        QVERIFY(bootChanges(bmc).isEmpty());
        QVERIFY(bmc.actions.isEmpty());
        bmc.whenAsked = nullptr;
    }

    void aRestartGoesOnlyToTheComputerItWasDecidedFor()
    {
        // (Outside review 2, 3.1.) While the controller takes its time over the change, it numbers its computers anew:
        // the card's is listed at another place now, and another node stands where it stood. A look under the same
        // session finds the card's computer at its new place. The restart, decided for the place before, goes to
        // neither, and nothing is cleared on the other node (before: it went to the place before, the other node).
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.beServer(aSystem, aRoot);
        bmc.bootTakesMs = 800;
        RedfishDevice::Pace pace = setupPace();
        pace.placesFreshMs = 0; // (where it keeps things, found anew at every look)
        RedfishDevice device(keptAs(bmc, markOf(aSystem) + digestOf(aRoot)), bmc.password, pace);
        bool renumbered = false;
        bmc.whenAsked = [&bmc, &renumbered, &device](const Asked& asked) {
            if (!renumbered && asked.method == "PATCH" && asked.body.contains("BiosSetup")) {
                renumbered = true;
                QTimer::singleShot(0, &bmc, [&bmc, &device] {
                    bmc.uuid = bSystem;
                    bmc.others = {{u"2"_s, QJsonValue(aSystem), u"On"_s}};
                    device.look();
                });
            }
        };
        const base::Result<qkvm::core::Done> done = doneBy(device, "bios_setup");
        QVERIFY(renumbered);
        QVERIFY(!done.ok());
        QVERIFY2(says(done, "another place"), saidBy(done).c_str());
        QVERIFY2(bmc.actions.isEmpty(), qPrintable(bmc.actions.join(u", "_s)));
        QCOMPARE(bootChanges(bmc), QStringList{u"Once BiosSetup"_s});
        QCOMPARE(bmc.others.at(0).power, u"On"_s);
        bmc.whenAsked = nullptr;
    }

    void aStartIntoSetupReadsTheListAnewAsItBegins()
    {
        // (Outside review 2, 3.2 and 3.3.) Its computer found by its number, alone; within the ten minutes where it
        // keeps things counts as known, another node comes in that gives the same number. The press reads its
        // controller's list anew as it begins: nothing is set, nothing restarted (before: it went on the place a look
        // found).
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.beServer(aSystem, aRoot);
        const RedfishDevice::Pace pace = setupPace();
        RedfishDevice device(keptAs(bmc, markOf(aSystem) + digestOf(aRoot)), bmc.password, pace);
        QVERIFY(identified(device));
        bmc.others = {{u"Node2"_s, QJsonValue(aSystem), u"Off"_s}};
        const base::Result<qkvm::core::Done> done = doneBy(device, "bios_setup");
        QCOMPARE(codeOf(done), std::string("another_machine"));
        QCOMPARE(saidBy(done), RedfishDevice::sameNumberTwice().message);
        QVERIFY(bootChanges(bmc).isEmpty());
        QVERIFY(bmc.actions.isEmpty());
        QVERIFY(pace.overrides->all().isEmpty());
    }

    // --- review 4's MEDIUM 1: a press under way when its driver goes, at the program's end or mid-way ---------------

    void whatMayBeLeftIsKeptByServerAndLetGoOnlyOnceItsDoubtIsOver()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"left"_s);
        const QDateTime at(QDate(2026, 11, 6), QTime(12, 0), QTimeZone(QTimeZone::UTC));
        const QUrl here(u"https://127.0.0.1:8443"_s);
        const QUrl there(u"https://127.0.0.1:9443"_s);
        const QString a = u"server:"_s + QString(32, u'a');
        const QString b = u"server:"_s + QString(32, u'b');
        qint64 steady = 0;
        qkvm::core::SetupOverrides kept(file, [&at] { return at.toUTC(); }, [&steady] { return steady; });
        const qkvm::core::SetupOverrides::Noted first = kept.mayBeSet(here, a, at.addSecs(60));
        QVERIFY(first.written);
        QVERIFY(!first.before.has_value());
        // By the number of its computer wherever it is; by its address where either has none.
        QVERIFY(kept.standing(there, a).has_value());
        QVERIFY(!kept.standing(here, b).has_value());
        QVERIFY(kept.standing(here, QString()).has_value());
        QVERIFY(!kept.standing(there, QString()).has_value());
        // A second press keeps the longer doubt, and says what stood.
        QCOMPARE(
            kept.mayBeSet(here, a, at.addSecs(30)).before.value_or(qkvm::core::SetupOverrides::Earlier{}).doubtUntil,
            at.addSecs(60));
        QCOMPARE(kept.standing(here, a).value_or(qkvm::core::SetupOverrides::Left{}).doubtUntil, at.addSecs(60));
        // Seen off before its doubt is over: it stands. After it (ten seconds on, on the clock its doubt runs on): it
        // goes.
        QVERIFY(!kept.seenOff(here, a, at.addSecs(59)));
        QVERIFY(kept.standing(here, a).has_value());
        kept.settle(here, a, at.addSecs(10));
        QVERIFY(!kept.seenOff(here, a, at.addSecs(10)));
        steady += 10'000;
        QVERIFY(kept.seenOff(here, a, at.addSecs(10)));
        QVERIFY(kept.all().isEmpty());
        // Kept in its file, and read back so; what is no record of it is not read.
        static_cast<void>(kept.mayBeSet(here, QString(), at));
        QCOMPARE(kept.written(), std::optional<bool>(true));
        QCOMPARE(qkvm::core::SetupOverrides(file).all(), kept.all());
        kept.drop(here, QString());
        QVERIFY(qkvm::core::SetupOverrides(file).all().isEmpty());
        QFile damaged(file);
        QVERIFY(damaged.open(QIODevice::WriteOnly));
        damaged.write(R"({"version": 1, "servers": [{"address": "not an address"}, 7]})");
        damaged.close();
        QVERIFY(qkvm::core::SetupOverrides(file).all().isEmpty());
        // Whether a note is written down, it says: kept in memory alone, yes; in a file that cannot be written, no
        // (C6: the change does not go out then).
        qkvm::core::SetupOverrides inMemory;
        QVERIFY(inMemory.mayBeSet(here, a, at.addSecs(60)).written);
        qkvm::core::SetupOverrides unwritable(folder.filePath(u"no such folder/left"_s));
        QVERIFY(!unwritable.mayBeSet(here, a, at.addSecs(60)).written);
        QVERIFY(unwritable.standing(here, a).has_value());
    }

    // A note that is there and cannot be opened is tried as the settings are (Settings::Patience: forty times, 50 ms
    // apart), so that one another program holds a moment is read whole (outside review of the fuzz work, round 3, M1).
    // One held for longer is never written over: it cannot be put aside either, so a press on another server is not
    // noted, and is not sent (C6); nothing is put in its place or beside it, and the start says so. Once it is let go,
    // it is read at the next writing, and what it holds, its queued mark too, is kept beside what was noted meanwhile.
    void aNoteThatCannotBeOpenedIsTriedAsTheSettingsAreAndNeverWrittenOver()
    {
        using Left = qkvm::core::SetupOverrides::Left;
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json.setup-overrides"_s);
        const QDateTime over = QDateTime::currentDateTimeUtc().addSecs(-60);
        const QUrl queued(u"https://127.0.0.1:8443"_s);
        const QUrl other(u"https://127.0.0.1:9443"_s);
        {
            qkvm::core::SetupOverrides first(file);
            static_cast<void>(first.mayBeSet(queued, QString(), over));
            QVERIFY(first.markQueued(queued, QString()));
        }
        const QByteArray written = bytesOf(file);
        {
            const HANDLE held = holdAlone(file);
            QVERIFY(held != INVALID_HANDLE_VALUE);
            const std::jthread lettingGo([held] {
                Sleep(150);
                CloseHandle(held);
            });
            const qkvm::core::SetupOverrides kept(file);
            QVERIFY2(!kept.wasUnreadable(), "a note held a moment was not tried as long as the settings are");
            QVERIFY(kept.standing(queued, QString()).value_or(Left{}).queued);
        }
        const HANDLE held = holdAlone(file);
        QVERIFY(held != INVALID_HANDLE_VALUE);
        qkvm::core::SetupOverrides kept(file, {}, {}, qkvm::core::Settings::Patience{3, 20});
        QVERIFY(kept.wasUnreadable());
        QVERIFY(kept.all().isEmpty());
        QVERIFY(!kept.putAsideWhatCouldNotBeRead().ok());
        QCOMPARE(kept.putAside(), QString());
        QVERIFY2(kept.toldOfItsFile().contains(u"Nothing is written over it"_s), qPrintable(kept.toldOfItsFile()));
        QVERIFY2(!kept.mayBeSet(other, QString(), over).written, "a note that could not be read was written over");
        QCOMPARE(QDir(folder.path()).entryList(QDir::Files), QStringList{u"settings.json.setup-overrides"_s});
        CloseHandle(held);
        QCOMPARE(bytesOf(file), written);
        QVERIFY(kept.mayBeSet(other, QString(), over).written);
        QVERIFY2(kept.standing(queued, QString()).value_or(Left{}).queued,
                 "what a note let go of later held was not taken in at the next writing");
        const qkvm::core::SetupOverrides again(file);
        QVERIFY(!again.wasUnreadable());
        QVERIFY(again.toldOfItsFile().isEmpty());
        QVERIFY(again.standing(queued, QString()).value_or(Left{}).queued);
        QVERIFY(again.standing(other, QString()).has_value());
    }

    // A note that is no note this build reads whole (no JSON, larger than any it writes, of another version, as after a
    // way back to it; a record that is none, beside one marked queued) is kept aside, byte for byte, before anything is
    // written in its place, and the start says where (outside review of the fuzz work, round 3, M1): a queued mark in
    // it is never written over, and what could be read of it is kept and told as ever. Where every name it is put
    // aside under is taken, nothing is written.
    void aNoteThatCannotBeReadWholeIsKeptAsideByteForByteAndTold()
    {
        using Left = qkvm::core::SetupOverrides::Left;
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json.setup-overrides"_s);
        const QString aside = file + u".unreadable"_s;
        const QUrl queued(u"https://127.0.0.1:8443"_s);
        const QUrl other(u"https://127.0.0.1:9443"_s);
        const QDateTime over = QDateTime::currentDateTimeUtc().addSecs(-60);
        const QByteArray record = "{\"address\": \"https://127.0.0.1:8443\", \"machine\": \"\", \"since\": "
                                  "\"2026-11-17T12:00:00.000Z\", \"doubtUntil\": \"2026-11-17T12:01:00.000Z\", "
                                  "\"queued\": true}";
        const QByteArray broken = "{\"address\": \"https://127.0.0.1:7443\", \"since\": \"not a time\"}";
        QByteArray more = "{\"version\": 1, \"servers\": [" + record;
        for (qsizetype number = 0; number < qkvm::core::SetupOverrides::mostKept; ++number) {
            more += ", " + record;
        }
        const QList<QByteArray> unreadable{
            "{\"version\": 1, \"servers\": [" + record,
            "{\"version\": 2, \"servers\": [" + record + "]}",
            "{\"version\": 1, \"servers\": {\"one\": " + record + "}}",
            "{\"version\": 1, \"servers\": [" + broken + ", " + record + "]}",
            more + "]}",
            QByteArray(qkvm::core::SetupOverrides::mostFileBytes + 1, ' '),
        };
        for (const QByteArray& content : unreadable) {
            QFile::remove(aside);
            QVERIFY(writeTo(file, content));
            qkvm::core::SetupOverrides kept(file);
            QVERIFY2(kept.wasUnreadable(), content.left(60).constData());
            QVERIFY(kept.toldOfItsFile().contains(u"could not read all of"_s));
            QVERIFY(kept.mayBeSet(other, QString(), over).written);
            QCOMPARE(kept.putAside(), aside);
            QVERIFY2(bytesOf(aside) == content, "a note that could not be read whole was not kept byte for byte");
            QVERIFY2(kept.toldOfItsFile().contains(u"settings.json.setup-overrides.unreadable"_s),
                     qPrintable(kept.toldOfItsFile()));
            const qkvm::core::SetupOverrides again(file);
            QVERIFY(!again.wasUnreadable());
            QVERIFY(again.standing(other, QString()).has_value());
            // (What could be read of it stays, its mark too.)
            if (content.contains(broken)) {
                QVERIFY(again.standing(queued, QString()).value_or(Left{}).queued);
            }
        }
        // Put aside at the start, before anything is written: said where it is.
        QFile::remove(aside);
        QVERIFY(writeTo(file, "{"));
        {
            qkvm::core::SetupOverrides started(file);
            QVERIFY(started.putAsideWhatCouldNotBeRead().ok());
            QCOMPARE(started.putAside(), aside);
            QVERIFY(!QFile::exists(file));
            QVERIFY(started.toldOfItsFile().contains(u"settings.json.setup-overrides.unreadable"_s));
        }
        // Every name taken: nothing written, and the note as it was.
        QVERIFY(writeTo(file, "{"));
        for (int number = 2; number <= 10; ++number) {
            QVERIFY(writeTo(file + u".unreadable-%1"_s.arg(number), "an older one"));
        }
        qkvm::core::SetupOverrides full(file);
        QVERIFY(!full.mayBeSet(other, QString(), over).written);
        QCOMPARE(bytesOf(file), QByteArray("{"));
        QCOMPARE(full.putAside(), QString());
        QVERIFY(full.toldOfItsFile().contains(u"Nothing is written over it"_s));
    }

    // At most mostKept servers are kept, and none is let go of to make room (the owner's rule that a queued record is
    // never pruned: the merged review of 0.2.0, A-L1, where the oldest went, a queued one too; and round 2's L4, where
    // an unmarked one went whose card still said its override may be set). A note of one more server is not noted then,
    // nothing is written, and every record stands as it was: its press does not go (`full`). A server already noted is
    // noted again as ever, and once a record goes by its own road (a look that shows the override off) there is room.
    // (The list is made in memory and kept in its file in one writing: written anew at each of 500 notes, one of those
    // writings failed on the build box in release, the single try review D's L2 is about.)
    void theCapLetsNoRecordGoToMakeRoom()
    {
        using Left = qkvm::core::SetupOverrides::Left;
        using Noted = qkvm::core::SetupOverrides::Noted;
        const QTemporaryDir folder;
        QDateTime at(QDate(2026, 11, 14), QTime(12, 0), QTimeZone(QTimeZone::UTC));
        const auto wall = [&at] { return at.toUTC(); };
        const auto steady = [&at] { return at.toMSecsSinceEpoch(); };
        const auto server = [](qsizetype number) { return QUrl(u"https://127.0.0.1:%1"_s.arg(20000 + number)); };
        constexpr qsizetype most = qkvm::core::SetupOverrides::mostKept;
        // The first marked queued, the second still in doubt, every other one over and unmarked: its card says that
        // its override may be set.
        qkvm::core::SetupOverrides kept(QString(), wall, steady);
        for (qsizetype number = 0; number < most; ++number) {
            QVERIFY(kept.mayBeSet(server(number), QString(), at.addSecs(number == 1 ? 300 : -60)).written);
        }
        QVERIFY(kept.markQueued(server(0), QString()));
        const QString file = folder.filePath(u"left"_s);
        kept.keepIn(file);
        const QByteArray before = bytesOf(file);
        const QList<Left> all = kept.all();
        QCOMPARE(all.size(), most);
        // One more: not noted, and nothing goes, not even the oldest unmarked record whose doubt is over.
        const Noted refused = kept.mayBeSet(server(most), QString(), at.addSecs(-60));
        QVERIFY(!refused.written);
        QVERIFY(refused.full);
        QVERIFY(!refused.before.has_value());
        QVERIFY(kept.all() == all);
        QVERIFY(!kept.standing(server(most), QString()).has_value());
        QVERIFY(kept.standing(server(2), QString()).has_value());
        QCOMPARE(bytesOf(file), before);
        // A server already noted is noted again as ever, the list full or not.
        QVERIFY(kept.mayBeSet(server(5), QString(), at.addSecs(-30)).written);
        // A look shows the third's override off: it goes by its own road, and there is room for one more.
        QVERIFY(kept.seenOff(server(2), QString(), at));
        const Noted later = kept.mayBeSet(server(most), QString(), at.addSecs(-60));
        QVERIFY(later.written && !later.full);
        QVERIFY(kept.standing(server(most), QString()).has_value());
        // And its file holds every record, the queued mark among them.
        const qkvm::core::SetupOverrides again(file, wall, steady);
        QVERIFY(!again.wasUnreadable());
        QCOMPARE(again.all().size(), most);
        QVERIFY(again.standing(server(0), QString()).value_or(Left{}).queued);
        QVERIFY(again.standing(server(1), QString()).has_value());
        QVERIFY(!again.standing(server(2), QString()).has_value());
    }

    // A change may still come only while a record's doubt is not over, on the wall clock the records keep: what holds
    // an update (WallWindows::whatHoldsAnUpdate). A record whose doubt is over holds nothing, and stays as it was, in
    // memory and in its file (B12: one left on 0.1.0's own profile, of a server that never answered, held every
    // update).
    void aChangeMayStillComeOnlyWhileARecordIsInDoubt()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"left"_s);
        QDateTime at(QDate(2026, 11, 14), QTime(12, 0), QTimeZone(QTimeZone::UTC));
        const QUrl here(u"https://127.0.0.1:8443"_s);
        const QUrl there(u"https://127.0.0.1:9443"_s);
        // (Time passes on both clocks alike here.)
        qkvm::core::SetupOverrides kept(file, [&at] { return at.toUTC(); }, [&at] { return at.toMSecsSinceEpoch(); });
        QVERIFY(!kept.changeMayStillCome());
        // One whose doubt is already over: nothing of it can come.
        static_cast<void>(kept.mayBeSet(there, QString(), at.addSecs(-60)));
        QVERIFY(!kept.changeMayStillCome());
        // One whose doubt goes on for a minute more: until then, and not after.
        static_cast<void>(kept.mayBeSet(here, QString(), at.addSecs(60)));
        QVERIFY(kept.changeMayStillCome());
        at = at.addSecs(59);
        QVERIFY(kept.changeMayStillCome());
        at = at.addSecs(2);
        QVERIFY(!kept.changeMayStillCome());
        // Both stay as they are, in memory and in their file.
        QCOMPARE(kept.all().size(), 2);
        QCOMPARE(qkvm::core::SetupOverrides(file).all(), kept.all());
        // The clock is read anew each time: one given later is the one asked.
        kept.useSteadyClock([&at] { return at.addSecs(-30).toMSecsSinceEpoch(); });
        QVERIFY(kept.changeMayStillCome());
    }

    // A doubt noted by this program runs on its own clock, which nothing puts right: the wall clock put back a quarter
    // of an hour (so that its end looks further off than any press notes) or forward past its end neither ends it nor
    // makes it look like one a clock that ran ahead noted; a look that shows the override off lets it go only once it
    // is over (outside review of 0.1.2, C5).
    void aDoubtThisProgramNotedHoldsWhateverTheWallClockDoes()
    {
        QDateTime at(QDate(2026, 11, 14), QTime(12, 0), QTimeZone(QTimeZone::UTC));
        qint64 steady = 5000;
        const QUrl here(u"https://127.0.0.1:8443"_s);
        qkvm::core::SetupOverrides kept(QString(), [&at] { return at.toUTC(); }, [&steady] { return steady; });
        static_cast<void>(kept.mayBeSet(here, QString(), at.addSecs(40)));
        QVERIFY(kept.changeMayStillCome());
        at = at.addSecs(qint64{-15} * 60);
        QVERIFY2(kept.changeMayStillCome(), "the wall clock put back ended the doubt");
        at = at.addSecs(qint64{75} * 60);
        QVERIFY2(kept.changeMayStillCome(), "the wall clock put forward ended the doubt");
        QVERIFY2(!kept.seenOff(here, QString(), at), "a look let the doubt go while it runs");
        QVERIFY(kept.standing(here, QString()).has_value());
        // Its forty seconds, on its own clock: a second before, still; a second after, no more, and a look lets it go.
        steady += 39'000;
        QVERIFY(kept.changeMayStillCome());
        steady += 2'000;
        QVERIFY(!kept.changeMayStillCome());
        QVERIFY(kept.seenOff(here, QString(), at));
        // Settled anew in this run (for longer than it was noted for): on its own clock too, whatever the wall clock
        // does.
        static_cast<void>(kept.mayBeSet(here, QString(), at.addSecs(10)));
        kept.settle(here, QString(), at.addSecs(30));
        at = at.addDays(-1);
        steady += 20'000;
        QVERIFY(kept.changeMayStillCome());
        steady += 11'000;
        QVERIFY(!kept.changeMayStillCome());
        // And no longer than any press notes, whatever its end says.
        static_cast<void>(kept.mayBeSet(here, QString(), at.addDays(1)));
        steady += qkvm::core::SetupOverrides::longestDoubtMs - 1;
        QVERIFY(kept.changeMayStillCome());
        steady += 2;
        QVERIFY(!kept.changeMayStillCome());
    }

    // A record of a run before (read from its file) whose doubt still runs as it is read (by the wall clock then, and
    // within the longest doubt) runs on from there on the steady clock, for what is left of it: a wall clock put on or
    // back later in this run neither ends it nor makes it one a clock that ran ahead noted (outside review's confirm
    // round of 0.1.2, R3). One whose doubt is over as it is read is judged by the wall clock.
    void aLiveRecordOfARunBeforeRunsOnTheSteadyClockFromItsReading()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"left"_s);
        QDateTime at(QDate(2026, 11, 14), QTime(12, 0), QTimeZone(QTimeZone::UTC));
        const QUrl here(u"https://127.0.0.1:8443"_s);
        const QUrl there(u"https://127.0.0.1:9443"_s);
        {
            qkvm::core::SetupOverrides before(file, [&at] { return at.toUTC(); });
            static_cast<void>(before.mayBeSet(here, QString(), at.addSecs(60)));
        }
        qint64 steady = 0;
        qkvm::core::SetupOverrides kept(file, [&at] { return at.toUTC(); }, [&steady] { return steady; });
        QVERIFY(kept.changeMayStillCome());
        at = at.addSecs(3600);
        QVERIFY2(kept.changeMayStillCome(), "the wall clock put on an hour ended it");
        at = at.addSecs(qint64{-2} * 3600);
        QVERIFY2(kept.changeMayStillCome(), "the wall clock put back an hour made it look too far off");
        QVERIFY(!kept.seenOff(here, QString(), at));
        steady += 59'000;
        QVERIFY(kept.changeMayStillCome());
        steady += 2'000;
        QVERIFY(!kept.changeMayStillCome());
        QVERIFY(kept.seenOff(here, QString(), at));
        // One whose doubt is over as it is read: nothing of it can come, by the wall clock.
        at = at.addSecs(3600);
        {
            qkvm::core::SetupOverrides before(file, [&at] { return at.toUTC(); });
            static_cast<void>(before.mayBeSet(there, QString(), at.addSecs(-60)));
        }
        qkvm::core::SetupOverrides over(file, [&at] { return at.toUTC(); }, [&steady] { return steady; });
        QVERIFY(!over.changeMayStillCome());
        QVERIFY(over.seenOff(there, QString(), at));
        // Noted again in this run for less than a doubt of a run before still runs: held for the longer.
        {
            qkvm::core::SetupOverrides before(file, [&at] { return at.toUTC(); });
            static_cast<void>(before.mayBeSet(here, QString(), at.addSecs(60)));
        }
        qkvm::core::SetupOverrides again(file, [&at] { return at.toUTC(); }, [&steady] { return steady; });
        static_cast<void>(again.mayBeSet(here, QString(), at.addSecs(10)));
        steady += 30'000;
        QVERIFY(again.changeMayStillCome());
        steady += 31'000;
        QVERIFY(!again.changeMayStillCome());
    }

    // Review 012-hold's LOW 1: a press noted and not settled yet holds by no clock (one its window let go of runs on in
    // its driver, and can outlast the doubt it noted): for as long as what pressUnderWay gave is kept, each of them,
    // and not after; given back after its keeper is gone, it touches nothing.
    void aPressUnderWayHoldsByNoClock()
    {
        QDateTime at(QDate(2026, 11, 14), QTime(12, 0), QTimeZone(QTimeZone::UTC));
        const QUrl here(u"https://127.0.0.1:8443"_s);
        qkvm::core::SetupOverrides kept(
            QString(), [&at] { return at.toUTC(); }, [&at] { return at.toMSecsSinceEpoch(); });
        std::shared_ptr<void> first = kept.pressUnderWay();
        std::shared_ptr<void> second = kept.pressUnderWay();
        QVERIFY(kept.changeMayStillCome());
        // Its record's doubt over: held all the same.
        static_cast<void>(kept.mayBeSet(here, QString(), at.addSecs(60)));
        at = at.addDays(1);
        QVERIFY(kept.changeMayStillCome());
        first.reset();
        QVERIFY(kept.changeMayStillCome());
        second.reset();
        QVERIFY(!kept.changeMayStillCome());
        QCOMPARE(kept.all().size(), 1);
        auto gone = std::make_unique<qkvm::core::SetupOverrides>();
        std::shared_ptr<void> late = gone->pressUnderWay();
        QVERIFY(gone->changeMayStillCome());
        gone.reset();
        late.reset();
        QVERIFY(!kept.changeMayStillCome());
    }

    // Review 012-final's INFO 3: what pressUnderWay gives says something while it is kept: a press's record that asks
    // whether it has one is told so.
    void whatAPressIsGivenIsSomething()
    {
        qkvm::core::SetupOverrides kept;
        std::shared_ptr<void> press = kept.pressUnderWay();
        QVERIFY(press != nullptr);
        QCOMPARE(kept.pressesUnderWay(), 1);
        press.reset();
        QCOMPARE(kept.pressesUnderWay(), 0);
    }

    // And the count is right on any thread (a press's record may go where its driver goes): many taken and given back
    // at once, on several threads, leave none under way.
    void pressesTakenAndGivenBackOnManyThreadsLeaveNoneUnderWay()
    {
        qkvm::core::SetupOverrides kept;
        std::atomic<bool> go{false};
        constexpr int many = 8;
        std::vector<std::thread> threads;
        threads.reserve(many);
        for (int one = 0; one < many; ++one) {
            threads.emplace_back([&kept, &go] {
                while (!go.load()) {
                    std::this_thread::yield();
                }
                for (int n = 0; n < 50'000; ++n) {
                    const std::shared_ptr<void> taken = kept.pressUnderWay();
                }
            });
        }
        go = true;
        for (std::thread& one : threads) {
            one.join();
        }
        QCOMPARE(kept.pressesUnderWay(), 0);
        QVERIFY(!kept.changeMayStillCome());
    }

    // Review 012-hold's LOW 2: a record of a run before whose doubt reaches further from now than the longest doubt a
    // press notes was noted by a clock that ran ahead and was put right since (a day fast at a press, then NTP): no
    // change of it can come that late, and it holds nothing; one within it holds as before. Both stay as they are.
    void aDoubtFurtherThanAnyPressNotesHoldsNothing()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"left"_s);
        QDateTime at(QDate(2026, 11, 14), QTime(12, 0), QTimeZone(QTimeZone::UTC));
        const QUrl here(u"https://127.0.0.1:8443"_s);
        const QUrl there(u"https://127.0.0.1:9443"_s);
        {
            qkvm::core::SetupOverrides before(file, [&at] { return at.toUTC(); });
            static_cast<void>(before.mayBeSet(there, QString(), at.addDays(1)));
            static_cast<void>(
                before.mayBeSet(here, QString(), at.addMSecs(qkvm::core::SetupOverrides::longestDoubtMs + 1000)));
        }
        qkvm::core::SetupOverrides kept(file, [&at] { return at.toUTC(); });
        QVERIFY(!kept.changeMayStillCome());
        // A second past the longest doubt: nothing; within it: held.
        at = at.addSecs(1);
        QVERIFY(kept.changeMayStillCome());
        QCOMPARE(kept.all().size(), 2);
    }

    // (A change the controller only queued, its task followed, and the prune of records:
    // test_redfish_setup_queued.cpp.)

    // An earlier doubt of a server that a refused press puts back stands as it stood, on the clock it runs on: the wall
    // clock moved an hour on in between does not end it (outside review's confirm round of 0.1.2, R3).
    void anEarlierDoubtStandsThroughARefusedPressWhateverTheWallClockDid()
    {
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.bootStatus = 403;
        RedfishDevice::Pace pace = setupPace();
        qint64 ahead = 0;
        qint64 steady = 0;
        pace.overrides = std::make_shared<qkvm::core::SetupOverrides>(
            QString(), [&ahead] { return QDateTime::currentDateTimeUtc().addMSecs(ahead); },
            [&steady] { return steady; });
        auto device = std::make_unique<RedfishDevice>(at(bmc), bmc.password, pace);
        // An earlier press left forty seconds of doubt for this server, as one that ended in doubt does.
        static_cast<void>(
            pace.overrides->mayBeSet(bmc.address(), QString(), QDateTime::currentDateTimeUtc().addSecs(40)));
        QVERIFY(pace.overrides->changeMayStillCome());
        // The wall clock is put an hour on, and a press now is refused by the controller: nothing of it was set.
        ahead = qint64{3600} * 1000;
        const auto answer = std::make_shared<std::optional<base::Result<qkvm::core::Done>>>();
        device->act(qkvm::core::Action::plain(u"bios_setup"_s),
                    [answer](const base::Result<qkvm::core::Done>& came) { *answer = came; });
        QTRY_VERIFY_WITH_TIMEOUT(answer->has_value(), 10000);
        QVERIFY(!(**answer).ok());
        QVERIFY2(pace.overrides->changeMayStillCome(), "the earlier doubt, put back, was ended by the wall clock");
        steady += 41'000;
        QVERIFY(!pace.overrides->changeMayStillCome());
        device.reset();
        pace.goodbyes->finish(pace.signOutWithinMs);
    }

    // A press whose note cannot be written beside the settings (a full disk, a file that cannot be written) asks the
    // server nothing: the change is never sent, nothing is changed on the server, and the press says why in plain
    // words. A program that then ended would have no note of a change it sent (outside review of 0.1.2, C6).
    void aPressWhoseNoteCannotBeWrittenAsksTheServerNothing()
    {
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.bootTakesMs = 1000;
        RedfishDevice::Pace pace = setupPace();
        const QTemporaryDir folder;
        pace.overrides = std::make_shared<qkvm::core::SetupOverrides>(folder.filePath(u"no such folder/left"_s));
        auto device = std::make_unique<RedfishDevice>(at(bmc), bmc.password, pace);
        const auto answer = std::make_shared<std::optional<base::Result<qkvm::core::Done>>>();
        device->act(qkvm::core::Action::plain(u"bios_setup"_s),
                    [answer](const base::Result<qkvm::core::Done>& came) { *answer = came; });
        QTRY_VERIFY_WITH_TIMEOUT(answer->has_value(), 10000);
        QVERIFY(!(**answer).ok());
        QVERIFY2(QString::fromStdString(saidBy(**answer)).contains(u"could not save the note"_s),
                 saidBy(**answer).c_str());
        QCOMPARE(bootChanges(bmc), QStringList());
        QVERIFY(bmc.actions.isEmpty());
        QCOMPARE(bmc.bootEnabled, u"Disabled"_s);
        QVERIFY(!pace.overrides->changeMayStillCome());
        device.reset();
        pace.goodbyes->finish(pace.signOutWithinMs);
        QCOMPARE(bootChanges(bmc), QStringList());
    }

    // ...and one that finds no room for its note, every one of the most kept marked queued, asks the server nothing
    // either, and says why: no record is let go of to make room for it (the owner's rule; the merged review of 0.2.0,
    // A-L1).
    void aPressWhoseNoteFindsNoRoomAsksTheServerNothing()
    {
        RedfishByHand bmc;
        offersSetup(bmc);
        RedfishDevice::Pace pace = setupPace();
        for (qsizetype number = 0; number < qkvm::core::SetupOverrides::mostKept; ++number) {
            const QUrl other(u"https://127.0.0.1:%1"_s.arg(20000 + number));
            QVERIFY(pace.overrides->mayBeSet(other, QString(), QDateTime::currentDateTimeUtc().addSecs(-60)).written);
            QVERIFY(pace.overrides->markQueued(other, QString()));
        }
        auto device = std::make_unique<RedfishDevice>(at(bmc), bmc.password, pace);
        const base::Result<qkvm::core::Done> answer = doneBy(*device, "bios_setup", 10000);
        QVERIFY(!answer.ok());
        QCOMPARE(codeOf(answer), std::string("too_many"));
        QVERIFY2(says(answer, "nothing was changed on it") && says(answer, "256"), saidBy(answer).c_str());
        QCOMPARE(bootChanges(bmc), QStringList());
        QVERIFY(bmc.actions.isEmpty());
        QCOMPARE(bmc.bootEnabled, u"Disabled"_s);
        QCOMPARE(pace.overrides->all().size(), qkvm::core::SetupOverrides::mostKept);
        QVERIFY(std::ranges::all_of(pace.overrides->all(),
                                    [](const qkvm::core::SetupOverrides::Left& one) { return one.queued; }));
        QVERIFY(!pace.overrides->standing(bmc.address(), QString()).has_value());
        QVERIFY(!pace.overrides->changeMayStillCome());
        device.reset();
        pace.goodbyes->finish(pace.signOutWithinMs);
        QCOMPARE(bootChanges(bmc), QStringList());
    }

    // The doubt a press notes at the driver's own pace (its four minutes, the clearing then and a change on its way) is
    // within the longest doubt: a press of the program's own is never taken for one noted by a clock that ran ahead.
    void theDoubtAPressNotesAtItsOwnPaceIsWithinTheLongest()
    {
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.bootTakesMs = 1000;
        RedfishDevice::Pace pace = setupPace();
        const RedfishDevice::Pace own;
        pace.setupWithinMs = own.setupWithinMs;
        pace.setupPatienceMs = own.setupPatienceMs;
        pace.bootQuietMs = own.bootQuietMs;
        pace.transferMs = own.transferMs;
        auto device = std::make_unique<RedfishDevice>(at(bmc), bmc.password, pace);
        device->act(qkvm::core::Action::plain(u"bios_setup"_s), [](const base::Result<qkvm::core::Done>&) {});
        QTRY_COMPARE_WITH_TIMEOUT(bootChanges(bmc), QStringList{u"Once BiosSetup"_s}, 10000);
        const qkvm::core::SetupOverrides::Left noted =
            pace.overrides->standing(bmc.address(), device->machine()).value_or(qkvm::core::SetupOverrides::Left{});
        QVERIFY(noted.doubtUntil.isValid());
        const qint64 doubt = QDateTime::currentDateTimeUtc().msecsTo(noted.doubtUntil);
        QVERIFY2(doubt > own.setupPatienceMs && doubt <= qkvm::core::SetupOverrides::longestDoubtMs,
                 QByteArray::number(doubt).constData());
        device.reset();
        pace.goodbyes->finish(pace.signOutWithinMs);
    }

    // The same through the driver: the wall clock the records keep is a day ahead, so the doubt each press notes is
    // over at once. A press holds all the same from its note to its end (taken as the server starts), and one whose
    // driver goes until it is handed over: while the change's answer is out, or while the start is watched, between two
    // reads.
    void aPressHoldsFromItsNoteToItsEndWhateverTheClockSays()
    {
        enum class Way { endsByItself, goesWhileTheChangeIsOut, goesWhileTheStartIsWatched };
        for (const Way way : {Way::endsByItself, Way::goesWhileTheChangeIsOut, Way::goesWhileTheStartIsWatched}) {
            RedfishByHand bmc;
            offersSetup(bmc);
            RedfishDevice::Pace pace = setupPace();
            if (way == Way::goesWhileTheStartIsWatched) {
                bmc.drops = {u"ForceRestart"_s};
                pace.setupPatienceMs = 20'000;
                pace.confirmEveryMs = 5000;
            } else {
                bmc.bootTakesMs = 1000;
            }
            pace.overrides = std::make_shared<qkvm::core::SetupOverrides>(
                QString(), [] { return QDateTime::currentDateTimeUtc().addDays(1); });
            auto device = std::make_unique<RedfishDevice>(at(bmc), bmc.password, pace);
            const auto answer = std::make_shared<std::optional<base::Result<qkvm::core::Done>>>();
            device->act(qkvm::core::Action::plain(u"bios_setup"_s),
                        [answer](const base::Result<qkvm::core::Done>& came) { *answer = came; });
            QVERIFY(!pace.overrides->changeMayStillCome());
            QTRY_COMPARE_WITH_TIMEOUT(bootChanges(bmc), QStringList{u"Once BiosSetup"_s}, 10000);
            QVERIFY(pace.overrides->standing(bmc.address(), device->machine()).has_value());
            QVERIFY(pace.overrides->changeMayStillCome());
            if (way == Way::endsByItself) {
                QTRY_VERIFY_WITH_TIMEOUT(answer->has_value(), 10000);
                QVERIFY2((**answer).ok(), saidBy(**answer).c_str());
                QVERIFY(!pace.overrides->changeMayStillCome());
                continue;
            }
            if (way == Way::goesWhileTheStartIsWatched) {
                QTRY_COMPARE_WITH_TIMEOUT(bmc.actions, QStringList{u"system ForceRestart"_s}, 10000);
                QTest::qWait(500);
                QVERIFY(pace.overrides->changeMayStillCome());
            }
            device.reset();
            QVERIFY(!pace.overrides->changeMayStillCome());
            pace.goodbyes->finish(pace.signOutWithinMs);
            QCOMPARE(bmc.bootEnabled, u"Disabled"_s);
        }
    }

    // A press whose waiter gave up on it (doneBy said "never") is still told what became of it when its driver goes, on
    // the program's loop (~RedfishDevice), after the waiter has returned: into what that waiter kept, never into a list
    // on the stack it left, where the next waiter's list then stands. Seen on a builder VM (2026-11-19): a case that
    // failed so made the next case of test_core_redfish_setup crash (0xC0000005) as the late answer was written there.
    void aPressItsWaiterGaveUpOnIsToldWhereThatWaiterKeepsItsAnswers()
    {
        RedfishByHand silent;
        offersSetup(silent);
        silent.loginNever = true;
        RedfishDevice::Pace patient = setupPace();
        patient.signInQuietMs = 30'000;
        patient.setupWithinMs = 30'000;
        patient.setupPatienceMs = 30'000;
        // (Held apart from this case, as the timer below that lets it go.)
        const auto first = std::make_shared<std::unique_ptr<RedfishDevice>>(
            std::make_unique<RedfishDevice>(at(silent), silent.password, patient));
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.setupTakesMs = 300;
        const RedfishDevice::Pace pace = setupPace();
        auto second = std::make_unique<RedfishDevice>(at(bmc), bmc.password, pace);
        QCOMPARE(codeOf(doneBy(**first, "bios_setup", 500)), std::string("never"));
        // The first driver goes while the second press is waited for: what it owed its press is told a turn later.
        QTimer::singleShot(100, this, [first] { first->reset(); });
        const base::Result<qkvm::core::Done> done = doneBy(*second, "bios_setup", 30000);
        QVERIFY2(done.ok(), saidBy(done).c_str());
        QCOMPARE(bootChanges(bmc), QStringList{u"Once BiosSetup"_s});
        QVERIFY(bmc.inSetup);
        QVERIFY(*first == nullptr);
        second.reset();
        patient.goodbyes->finish(patient.signOutWithinMs);
        pace.goodbyes->finish(pace.signOutWithinMs);
    }

    void theProgramsEndWhileTheChangeIsOutClearsItAndReadsItBackBeforeTheSessionGoes()
    {
        // The controller takes a while over the change (made as it comes, answered two seconds later), and the program
        // ends meanwhile: its driver let go of, and its goodbyes waited for as main.cpp waits for them. The override is
        // cleared again and read back, and only then is the session given back; the press is told that the override may
        // be set. The clearing was seen to work after the change was seen made; but the change's answer never came, and
        // the controller may have queued it: its record stays, marked so, and the press says so (outside review 2, 3.5;
        // before: let go of, so that a change the controller queued landed with no warning left).
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.bootTakesMs = 2000;
        // (What comes before the change is given all the time it may take: this is about what follows it. With
        // setupPace()'s 1.5 s, to be let in, for a read to answer and for the change to go out, the press was given up
        // before the change on a loaded PC, gate 47: "Nothing was done", as it is to be for a controller that slow.)
        RedfishDevice::Pace pace = setupPace();
        pace.connectWithinMs = 15'000;
        pace.quietMs = 15'000;
        pace.setupWithinMs = 30'000;
        auto device = std::make_unique<RedfishDevice>(at(bmc), bmc.password, pace);
        // (Held apart from this case: the press may be told after it ends.)
        const auto answer = std::make_shared<std::optional<base::Result<qkvm::core::Done>>>();
        device->act(qkvm::core::Action::plain(u"bios_setup"_s),
                    [answer](const base::Result<qkvm::core::Done>& came) { *answer = came; });
        // Until the change has come, or the press was answered without it (and then in its own words).
        QVERIFY(
            QTest::qWaitFor([&bmc, &answer] { return !bootChanges(bmc).isEmpty() || answer->has_value(); }, 60'000));
        QVERIFY2(!bootChanges(bmc).isEmpty(), answer->has_value() ? saidBy(**answer).c_str() : "");
        QCOMPARE(bootChanges(bmc), QStringList{u"Once BiosSetup"_s});
        // (Noted before it went.)
        QVERIFY(pace.overrides->standing(bmc.address(), device->machine()).has_value());
        const qsizetype before = bmc.requests.size();
        device.reset();
        QElapsedTimer took;
        took.start();
        pace.goodbyes->finish(pace.signOutWithinMs); // (as main.cpp: the sessions' own time)
        QVERIFY2(took.elapsed() < 8000, QByteArray::number(took.elapsed()).constData());
        QCOMPARE(bmc.bootEnabled, u"Disabled"_s);
        QCOMPARE(bootChanges(bmc), (QStringList{u"Once BiosSetup"_s, u"Disabled"_s}));
        const QStringList after = bmc.requests.mid(before);
        const QString computer = u"GET "_s + bmc.system();
        const QString clearing = u"PATCH "_s + bmc.system();
        QVERIFY2(after.indexOf(clearing) >= 0 && after.indexOf(clearing) < after.lastIndexOf(computer),
                 qPrintable(after.join(u", "_s)));
        QVERIFY2(after.constLast().startsWith(u"DELETE /redfish/v1/SessionService/Sessions/"_s),
                 qPrintable(after.join(u", "_s)));
        QCOMPARE(bmc.signedOut, 1);
        QTRY_VERIFY(answer->has_value());
        QVERIFY2(says(**answer, "the BIOS setup boot override may be set"), saidBy(**answer).c_str());
        QVERIFY2(says(**answer, "The controller's answer to the change did not come, so it may have queued it"),
                 saidBy(**answer).c_str());
        QVERIFY(pace.overrides->standing(bmc.address(), QString()).value_or(qkvm::core::SetupOverrides::Left{}).queued);
    }

    void theProgramsEndWhileTheStartIsWatchedClearsItToo()
    {
        // The change answered and the restart taken, and nothing came of it yet: the start is being watched as the
        // program ends. Cleared again on the way out, as at the end of the press's four minutes.
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.drops = {u"ForceRestart"_s};
        const RedfishDevice::Pace pace = setupPace();
        auto device = std::make_unique<RedfishDevice>(at(bmc), bmc.password, pace);
        // (Held apart from this case: the press may be told after it ends.)
        const auto answer = std::make_shared<std::optional<base::Result<qkvm::core::Done>>>();
        device->act(qkvm::core::Action::plain(u"bios_setup"_s),
                    [answer](const base::Result<qkvm::core::Done>& came) { *answer = came; });
        QTRY_COMPARE_WITH_TIMEOUT(bmc.actions, QStringList{u"system ForceRestart"_s}, 10000);
        QTest::qWait(300);
        QCOMPARE(bmc.bootEnabled, u"Once"_s);
        device.reset();
        pace.goodbyes->finish(pace.signOutWithinMs); // (as main.cpp: the sessions' own time)
        QCOMPARE(bmc.bootEnabled, u"Disabled"_s);
        QCOMPARE(bmc.signedOut, 1);
        QTRY_VERIFY(answer->has_value());
        QVERIFY2(says(**answer, "the BIOS setup boot override may be set"), saidBy(**answer).c_str());
        QVERIFY(!pace.overrides->standing(bmc.address(), QString()).has_value());
    }

    void aDriverLetGoOfMidWayWhoseClearingIsRefusedLeavesTheCardSayingSo()
    {
        // The card changed while the start was watched (its driver let go of, a new one made, the program going on):
        // the clearing on the way out is refused, and the session is given back all the same. The driver made anew says
        // on the card that the override may be set, until a look shows it off (here: once somebody cleared it at the
        // controller's own page, and nothing of the press can still come). (Let go of once the change was answered:
        // while its answer is out, its record is marked as one the controller may queue, which no look lets go,
        // aChangeIsMarkedAsOneTheControllerMayQueueBeforeItGoes; outside review 2, 3.5.)
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.bootTakesMs = 1000;
        bmc.clearStatus = 500;
        bmc.startsIntoSetup = false; // (not taken: the override stays set)
        const RedfishDevice::Pace pace = setupPace();
        auto device = std::make_unique<RedfishDevice>(at(bmc), bmc.password, pace);
        device->act(qkvm::core::Action::plain(u"bios_setup"_s), [](const base::Result<qkvm::core::Done>&) {});
        QTRY_COMPARE_WITH_TIMEOUT(bmc.actions, QStringList{u"system ForceRestart"_s}, 10000);
        QCOMPARE(bootChanges(bmc), QStringList{u"Once BiosSetup"_s});
        device.reset();
        QTRY_COMPARE_WITH_TIMEOUT(bmc.signedOut, 1, 10000);
        QCOMPARE(bmc.bootEnabled, u"Once"_s);
        RedfishDevice anew(at(bmc), bmc.password, pace);
        QCOMPARE(leftOnTheCard(anew), qkvm::core::SetupOverrides::onTheCard());
        bmc.bootEnabled = u"Disabled"_s;
        QTRY_VERIFY_WITH_TIMEOUT(leftOnTheCard(anew).isEmpty(), 20000);
        QVERIFY(!pace.overrides->standing(bmc.address(), anew.machine()).has_value());
    }

    void aClearingOnTheWayOutChangesNoOtherServerAndNoOtherToolsOverride()
    {
        // What the controller shows by the time the driver goes is not this press's to change: another computer under
        // the same session (by its number), or another tool's override in place of the BIOS setup one. Left as it is.
        for (const bool otherServer : {true, false}) {
            RedfishByHand bmc;
            offersSetup(bmc);
            bmc.bootTakesMs = 1000;
            const RedfishDevice::Pace pace = setupPace();
            auto device = std::make_unique<RedfishDevice>(at(bmc), bmc.password, pace);
            device->act(qkvm::core::Action::plain(u"bios_setup"_s), [](const base::Result<qkvm::core::Done>&) {});
            QTRY_COMPARE_WITH_TIMEOUT(bootChanges(bmc), QStringList{u"Once BiosSetup"_s}, 10000);
            if (otherServer) {
                bmc.uuid = RedfishByHand::fresh();
            } else {
                bmc.bootEnabled = u"Continuous"_s;
                bmc.bootTarget = u"Pxe"_s;
            }
            device.reset();
            pace.goodbyes->finish(pace.signOutWithinMs);
            QCOMPARE(bootChanges(bmc), QStringList{u"Once BiosSetup"_s});
            QCOMPARE(bmc.signedOut, 1);
            // (Another server's override says nothing of this one's: noted still. Another tool's in its place: the
            // BIOS setup one is not set, but the change of this press's may yet come, and it stands until a look.)
            QVERIFY(pace.overrides->standing(bmc.address(), QString()).has_value());
        }
    }

    void aClearingOnTheWayOutRefusedForAStateThatChangedIsSentAgainWithTheNewMark()
    {
        // (Outside review of 0.2.0's Redfish fixes, L9.) The same on the way out: the goodbyes' clearing, refused for a
        // mark that moved on since their read, is read again and sent shown the new mark, not "*".
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.bootTakesMs = 1000;
        bool once = false;
        bmc.whenAsked = [&bmc, &once](const Asked& asked) {
            if (!once && asked.method == "PATCH" && asked.body.contains("Disabled")) {
                once = true;
                bmc.mark = u"\"1791401399\""_s;
            }
        };
        const RedfishDevice::Pace pace = setupPace();
        auto device = std::make_unique<RedfishDevice>(at(bmc), bmc.password, pace);
        device->act(qkvm::core::Action::plain(u"bios_setup"_s), [](const base::Result<qkvm::core::Done>&) {});
        QTRY_COMPARE_WITH_TIMEOUT(bootChanges(bmc), QStringList{u"Once BiosSetup"_s}, 10000);
        device.reset();
        pace.goodbyes->finish(pace.signOutWithinMs);
        QCOMPARE(bmc.bootEnabled, u"Disabled"_s);
        QVERIFY2(!bmc.ifMatches.contains("*"_ba), bmc.ifMatches.join(' ').constData());
        QCOMPARE(bmc.ifMatches.constLast(), "\"1791401399\""_ba);
        QCOMPARE(bmc.signedOut, 1);
        bmc.whenAsked = nullptr;
    }

    void aClearingOnTheWayOutRefusedAtEveryMarkIsSentThreeTimesAtTheMost()
    {
        // (Outside review of 0.2.0's Redfish fixes, round 2, L3.) The goodbyes' clearing at a controller whose mark
        // moves at every read and that refuses every mark it is shown: three clearings, the third "*", and no fourth
        // (without the bound, sent again until their thirty seconds, here four, run out).
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.bootTakesMs = 1000;
        const RedfishDevice::Pace pace = setupPace();
        auto device = std::make_unique<RedfishDevice>(at(bmc), bmc.password, pace);
        device->act(qkvm::core::Action::plain(u"bios_setup"_s), [](const base::Result<qkvm::core::Done>&) {});
        QTRY_COMPARE_WITH_TIMEOUT(bootChanges(bmc), QStringList{u"Once BiosSetup"_s}, 10000);
        QList<QByteArray> clearings;
        marksMoveAtEveryRead(bmc, nullptr, &clearings);
        device.reset();
        pace.goodbyes->finish(pace.signOutWithinMs);
        QVERIFY2(twoMarksThenWhatever(clearings), clearings.join(' ').constData());
        QCOMPARE(bmc.bootEnabled, u"Disabled"_s);
        QCOMPARE(bmc.signedOut, 1);
        bmc.whenAsked = nullptr;
    }

    void aClearingOnTheWayOutReadBackAsNothingLeavesItNoted()
    {
        // The clearing taken (204), and its computer read back with an empty Boot: not seen off (review 4's MEDIUM 2),
        // so it stays noted for the next start.
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.bootTakesMs = 1000;
        bmc.whenAsked = [&bmc](const Asked& asked) {
            if (asked.method == "PATCH" && asked.body.contains("Disabled")) {
                bmc.bootSaidAs = QJsonObject{};
            }
        };
        const RedfishDevice::Pace pace = setupPace();
        auto device = std::make_unique<RedfishDevice>(at(bmc), bmc.password, pace);
        device->act(qkvm::core::Action::plain(u"bios_setup"_s), [](const base::Result<qkvm::core::Done>&) {});
        QTRY_COMPARE_WITH_TIMEOUT(bootChanges(bmc), QStringList{u"Once BiosSetup"_s}, 10000);
        device.reset();
        pace.goodbyes->finish(pace.signOutWithinMs);
        QCOMPARE(bootChanges(bmc), (QStringList{u"Once BiosSetup"_s, u"Disabled"_s}));
        QCOMPARE(bmc.signedOut, 1);
        QVERIFY(pace.overrides->standing(bmc.address(), QString()).has_value());
    }

    void theEndIsBoundedWhereTheClearingIsNotAnswered()
    {
        // A controller that answers no change in time: the program's end waits for the clearing its time and the
        // session's goodbye after it, and no longer; what was not seen is left noted.
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.bootTakesMs = 20'000;
        const RedfishDevice::Pace pace = setupPace();
        auto device = std::make_unique<RedfishDevice>(at(bmc), bmc.password, pace);
        device->act(qkvm::core::Action::plain(u"bios_setup"_s), [](const base::Result<qkvm::core::Done>&) {});
        QTRY_COMPARE_WITH_TIMEOUT(bootChanges(bmc), QStringList{u"Once BiosSetup"_s}, 10000);
        device.reset();
        QElapsedTimer took;
        took.start();
        pace.goodbyes->finish(pace.signOutWithinMs); // (as main.cpp: the sessions' own time)
        const qint64 waited = took.elapsed();
        QVERIFY2(waited < pace.setupClearWithinMs + pace.signOutWithinMs + 2000,
                 QByteArray::number(waited).constData());
        QCOMPARE(pace.goodbyes->underWay(), 0);
        QVERIFY(pace.overrides->standing(bmc.address(), QString()).has_value());
    }

    void theNextStartTellsWhatWasLeftOnTheCardAndAsANotice()
    {
        // The program ended with the override not seen cleared (the clearing refused): noted beside the settings. The
        // next start reads it, tells it as a notice for the server's card, and the card says it, until a look shows the
        // override off; then nothing of it is kept. (Ended once the change was answered, while the start was watched:
        // ended while its answer is out, the record is marked as one the controller may queue, which no look lets go,
        // aChangeIsMarkedAsOneTheControllerMayQueueBeforeItGoes; outside review 2, 3.5.)
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json.setup-overrides"_s);
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.bootTakesMs = 1000;
        bmc.clearStatus = 500;
        bmc.startsIntoSetup = false; // (not taken: the override stays set)
        RedfishDevice::Pace pace = setupPace();
        pace.overrides = std::make_shared<qkvm::core::SetupOverrides>(file);
        QString machine;
        {
            auto device = std::make_unique<RedfishDevice>(at(bmc), bmc.password, pace);
            device->act(qkvm::core::Action::plain(u"bios_setup"_s), [](const base::Result<qkvm::core::Done>&) {});
            QTRY_COMPARE_WITH_TIMEOUT(bmc.actions, QStringList{u"system ForceRestart"_s}, 10000);
            QCOMPARE(bootChanges(bmc), QStringList{u"Once BiosSetup"_s});
            machine = device->machine();
            device.reset();
            pace.goodbyes->finish(pace.signOutWithinMs); // (as main.cpp: the sessions' own time)
        }
        QCOMPARE(bmc.bootEnabled, u"Once"_s);
        pace.overrides.reset();
        // The next start.
        const auto next = std::make_shared<qkvm::core::SetupOverrides>(file);
        QCOMPARE(next->all().size(), 1);
        qkvm::core::OwnKvm card;
        card.id = u"0a1b2c3d"_s;
        card.name = u"rack 7"_s;
        card.kind = u"redfish"_s;
        card.address = bmc.address().toString();
        card.machine = machine;
        qkvm::core::OwnKvm other = card;
        other.id = u"4e5f6a7b"_s;
        other.name = u"another"_s;
        other.address = u"https://127.0.0.1:1"_s;
        other.machine.clear();
        const QStringList told = next->toldAtStart({card, other});
        QCOMPARE(told.size(), 1);
        QVERIFY2(told.constFirst().startsWith(u"rack 7: its BIOS setup boot override may still be set"_s),
                 qPrintable(told.constFirst()));
        RedfishDevice::Pace again = setupPace();
        again.overrides = next;
        RedfishDevice started(keptAs(bmc, machine), bmc.password, again);
        QCOMPARE(leftOnTheCard(started), qkvm::core::SetupOverrides::onTheCard());
        bmc.bootEnabled = u"Disabled"_s;
        QTRY_VERIFY_WITH_TIMEOUT(leftOnTheCard(started).isEmpty(), 20000);
        QVERIFY(qkvm::core::SetupOverrides(file).all().isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestRedfishSetup)
#include "test_redfish_setup.moc"
