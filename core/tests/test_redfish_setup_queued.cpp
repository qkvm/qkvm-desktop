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
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

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

// Start into BIOS setup on a server of this PC's own wall, where the controller only queues the change (a 202, the
// monitor of a task named, or a Task as its answer: D1): the server's record marked queued at the answer and written
// down, and taken off only by reads of the server's own computer that see this very change through, never by the
// task's word; its task followed, held to its time; and the prune of records of servers on no card, which never takes
// a marked one. Against a controller by hand. (The press itself, what is kept beside the settings and the way out:
// test_redfish_setup.cpp. One program was 236 to 475 s on the loaded build box, past CTest's 400.)
class TestRedfishSetupQueued : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { const RedfishDevice first(RedfishDevice::Where{}, QString(), quick()); }

    // A controller that only queues the change, with a 202 or as a task (outside review's confirm round of 0.1.2, D1:
    // it may be made later than any time the press waits, at any time, and the task's monitor is followed only from
    // 0.2.0 on): the press says so in plain words; the server's record is marked queued and stays on its card, and no
    // look lets it go, however late; what holds an update is the press's own doubt, as for any press.
    void aChangeTheControllerOnlyQueuedIsToldAndNoLookLetsItGo()
    {
        enum class Way { accepted, aTask, aTasksMonitor };
        for (const Way way : {Way::accepted, Way::aTask, Way::aTasksMonitor}) {
            RedfishByHand bmc;
            offersSetup(bmc);
            if (way == Way::aTask) {
                // (A plain literal: a raw one here hides the test's class from AutoMoc.)
                bmc.bootQueuedAs =
                    Answered(200, "{\"@odata.type\": \"#Task.v1_4_3.Task\", \"Id\": \"7\", \"TaskState\": \"New\"}");
            } else if (way == Way::aTasksMonitor) {
                bmc.bootQueuedAs =
                    Answered(200, {}, "application/json", {"Location: /redfish/v1/TaskService/Tasks/7/Monitor"});
            } else {
                bmc.bootNotKept = true;
            }
            RedfishDevice::Pace pace = setupPace();
            qint64 steady = 0;
            pace.overrides = std::make_shared<qkvm::core::SetupOverrides>(
                QString(), [] { return QDateTime::currentDateTimeUtc(); }, [&steady] { return steady; });
            auto device = std::make_unique<RedfishDevice>(at(bmc), bmc.password, pace);
            const auto answer = std::make_shared<std::optional<base::Result<qkvm::core::Done>>>();
            device->act(qkvm::core::Action::plain(u"bios_setup"_s),
                        [answer](const base::Result<qkvm::core::Done>& came) { *answer = came; });
            QTRY_VERIFY_WITH_TIMEOUT(answer->has_value(), 15000);
            QVERIFY2(QString::fromStdString(saidBy(**answer)).contains(u"queued the change"_s),
                     saidBy(**answer).c_str());
            QVERIFY(QString::fromStdString(saidBy(**answer)).contains(u"in its BMC"_s));
            QVERIFY(pace.overrides->standing(bmc.address(), device->machine())
                        .value_or(qkvm::core::SetupOverrides::Left{})
                        .queued);
            QCOMPARE(leftOnTheCard(*device), qkvm::core::SetupOverrides::onTheCard(true));
            // Its doubt over on its own clock: nothing holds an update any more, as for any press.
            steady += qkvm::core::SetupOverrides::longestDoubtMs + 1;
            QVERIFY(!pace.overrides->changeMayStillCome());
            // No look lets it go, however late: a day on, by both clocks, the driver's own look reads the override
            // "Disabled" and its card still says it; nor does a look told it is a day on, or a press of its own that
            // knows it cleared it (SetupOverrides::drop).
            steady += qint64{24} * 3600 * 1000;
            QCOMPARE(bmc.bootEnabled, u"Disabled"_s);
            QVERIFY2(!leftOnTheCard(*device).isEmpty(), "a look a day on let a queued change's record go");
            QCOMPARE(leftOnTheCard(*device), qkvm::core::SetupOverrides::onTheCard(true));
            QVERIFY(pace.overrides->standing(bmc.address(), device->machine()).has_value());
            QVERIFY2(
                !pace.overrides->seenOff(bmc.address(), device->machine(), QDateTime::currentDateTimeUtc().addDays(1)),
                "a look told it is a day on let a queued change's record go");
            QVERIFY(!pace.overrides->drop(bmc.address(), device->machine()));
            QVERIFY(pace.overrides->standing(bmc.address(), device->machine()).has_value());
            QVERIFY(!pace.overrides->changeMayStillCome());
            device.reset();
            pace.goodbyes->finish(pace.signOutWithinMs);
        }
    }

    // The mark that the controller only queued the change is written down at its answer, not at the press's end: a
    // program ended by force between the two (a crash, Windows restarting for its updates) leaves it marked, and the
    // next run's looks let it go no more than this run's would, however late (D1).
    void aQueuedMarkIsWrittenDownAtTheAnswerNotAtThePressesEnd()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json.setup-overrides"_s);
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.bootNotKept = true;
        RedfishDevice::Pace pace = setupPace();
        // (Read back for long enough to look at the file meanwhile; its restart's time well past what the reads before
        // the change take on a loaded machine.)
        pace.setupWithinMs = 20'000;
        pace.setupPatienceMs = 25'000;
        pace.confirmWithinMs = 6000;
        pace.overrides = std::make_shared<qkvm::core::SetupOverrides>(file);
        auto device = std::make_unique<RedfishDevice>(at(bmc), bmc.password, pace);
        // What a run after a crash just now would read, a day on (a copy of the file as it stands), and whether a
        // look then that shows the override off leaves it standing. (The file read with every sharing a reader can
        // give, so that this copy never stands in the way of the press's writing, which puts a new file in its place.)
        const auto standsAfterACrash = [&file, &bmc, &device] {
            const QTemporaryDir crash;
            const QString copy = crash.filePath(u"left"_s);
            const HANDLE read = CreateFileW(QDir::toNativeSeparators(file).toStdWString().c_str(), GENERIC_READ,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (read == INVALID_HANDLE_VALUE) {
                return false;
            }
            QByteArray bytes;
            QByteArray chunk(4096, Qt::Uninitialized);
            DWORD got = 0;
            while (ReadFile(read, chunk.data(), static_cast<DWORD>(chunk.size()), &got, nullptr) != FALSE && got > 0) {
                bytes.append(chunk.constData(), static_cast<qsizetype>(got));
            }
            CloseHandle(read);
            if (!writeTo(copy, bytes)) {
                return false;
            }
            const QDateTime dayOn = QDateTime::currentDateTimeUtc().addDays(1);
            qkvm::core::SetupOverrides next(copy, [dayOn] { return dayOn.toUTC(); });
            const std::optional<qkvm::core::SetupOverrides::Left> left =
                next.standing(bmc.address(), device->machine());
            return left.has_value() && left->queued && !next.seenOff(bmc.address(), device->machine(), dayOn) &&
                   !next.changeMayStillCome();
        };
        const auto answer = std::make_shared<std::optional<base::Result<qkvm::core::Done>>>();
        device->act(qkvm::core::Action::plain(u"bios_setup"_s),
                    [answer](const base::Result<qkvm::core::Done>& came) { *answer = came; });
        // The change answered (202), or the press over before it: whichever comes first, however long the reads before
        // the change take.
        QTRY_VERIFY_WITH_TIMEOUT(bmc.asked(u"PATCH "_s + bmc.system()) > 0 || answer->has_value(), 30000);
        QVERIFY2(!answer->has_value(),
                 ("the press ended before its change went: " + saidBy(answer->value_or(base::Error{"", ""}))).c_str());
        // ...and from its answer on, the mark is in the file at once, while the press still reads the change back.
        QTRY_VERIFY2_WITH_TIMEOUT(standsAfterACrash(), "the queued mark was not in the file within 5 s of the change",
                                  5000);
        QVERIFY2(!answer->has_value(), ("the press ended before the queued mark was written down: " +
                                        saidBy(answer->value_or(base::Error{"", ""})))
                                           .c_str());
        // (Its clearing not sent yet: it was still reading the change back.)
        QCOMPARE(bootChanges(bmc), QStringList{u"Once BiosSetup"_s});
        QTRY_VERIFY_WITH_TIMEOUT(answer->has_value(), 20000);
        QVERIFY(standsAfterACrash());
        device.reset();
        pace.goodbyes->finish(pace.signOutWithinMs);
    }

    // A queued record goes with the next start into BIOS setup on that server that ends as it should: its change seen
    // made, and the override seen off after it (taken as the server started). Not with one that ends in doubt (D1).
    void aLaterStartIntoSetupThatEndsAsItShouldLetsAQueuedRecordGo()
    {
        using Left = qkvm::core::SetupOverrides::Left;
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.bootNotKept = true;
        const RedfishDevice::Pace pace = setupPace();
        auto device = std::make_unique<RedfishDevice>(at(bmc), bmc.password, pace);
        const base::Result<qkvm::core::Done> queued = doneBy(*device, "bios_setup");
        QVERIFY2(says(queued, "queued the change"), saidBy(queued).c_str());
        QVERIFY(pace.overrides->standing(bmc.address(), device->machine()).has_value());
        // One that changes nothing (refused for certain) leaves it as it stood; so does one whose change is seen made
        // and cleared again, but whose restart was refused: it did not end as it should.
        bmc.bootNotKept = false;
        bmc.bootStatus = 403;
        QVERIFY(!doneBy(*device, "bios_setup").ok());
        QVERIFY(pace.overrides->standing(bmc.address(), device->machine()).value_or(Left{}).queued);
        bmc.bootStatus = 0;
        bmc.resetStatus = 400;
        const base::Result<qkvm::core::Done> refused = doneBy(*device, "bios_setup");
        QVERIFY2(says(refused, "the BIOS setup boot override was cleared again"), saidBy(refused).c_str());
        QVERIFY2(pace.overrides->standing(bmc.address(), device->machine()).value_or(Left{}).queued,
                 "a start into BIOS setup that did not end as it should let a queued record go");
        // One answered 204 and taken as the server starts: let go of.
        bmc.resetStatus = 0;
        const base::Result<qkvm::core::Done> normal = doneBy(*device, "bios_setup");
        QVERIFY2(normal.ok(), saidBy(normal).c_str());
        QVERIFY2(!pace.overrides->standing(bmc.address(), device->machine()).has_value(),
                 "a start into BIOS setup that ended as it should left a queued record standing");
        QVERIFY(leftOnTheCard(*device).isEmpty());
        device.reset();
        pace.goodbyes->finish(pace.signOutWithinMs);
    }

    // Nor does a press cut off: a change the controller queued and then made (seen set), its server restarted, and its
    // driver let go of while the start is watched. The goodbyes clear the override and read it back off; the record
    // stands, marked queued (D1), and the press says the queue, as the card does (outside review of D1, round 2, H1).
    void aQueuedPressCutOffLeavesItsRecordWhateverItsClearingShows()
    {
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.bootNotKept = true;
        // (The controller makes the queued change at once, and its restart is taken without the server starting.)
        bmc.whenAsked = [&bmc](const Asked& asked) {
            if (asked.method == "PATCH" && asked.body.contains("BiosSetup")) {
                bmc.bootEnabled = u"Once"_s;
                bmc.bootTarget = u"BiosSetup"_s;
            }
        };
        bmc.drops = {u"ForceRestart"_s};
        RedfishDevice::Pace pace = setupPace();
        pace.setupPatienceMs = 20'000;
        auto device = std::make_unique<RedfishDevice>(at(bmc), bmc.password, pace);
        const auto answer = std::make_shared<std::optional<base::Result<qkvm::core::Done>>>();
        device->act(qkvm::core::Action::plain(u"bios_setup"_s),
                    [answer](const base::Result<qkvm::core::Done>& came) { *answer = came; });
        QTRY_COMPARE_WITH_TIMEOUT(bmc.actions, QStringList{u"system ForceRestart"_s}, 10000);
        device.reset();
        pace.goodbyes->finish(pace.signOutWithinMs);
        QCOMPARE(bmc.bootEnabled, u"Disabled"_s);
        QCOMPARE(bootChanges(bmc), (QStringList{u"Once BiosSetup"_s, u"Disabled"_s}));
        QVERIFY2(pace.overrides->standing(bmc.address(), QString()).value_or(qkvm::core::SetupOverrides::Left{}).queued,
                 "the clearing on the way out let a queued record go");
        QTRY_VERIFY_WITH_TIMEOUT(answer->has_value(), 5000);
        QVERIFY2(says(**answer, "queued the change"), saidBy(**answer).c_str());
    }

    // A change answered as made (204) that names a task's monitor all the same is a queued one, and that answer is the
    // controller's word, no read: restarted as after any 204, its record stays marked and the queue is said, though
    // the watched start reads the override "Disabled" (it never showed set: nothing says the restart took it).
    void aChangeAnsweredMadeThatNamesATaskStaysMarkedUntilAReadShowsIt()
    {
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.bootQueuedAs = Answered(204, {}, "application/json", {"Location: /redfish/v1/TaskService/Tasks/7/Monitor"});
        const RedfishDevice::Pace pace = setupPace();
        auto device = std::make_unique<RedfishDevice>(at(bmc), bmc.password, pace);
        const base::Result<qkvm::core::Done> pressed = doneBy(*device, "bios_setup");
        QVERIFY2(pressed.ok(), saidBy(pressed).c_str());
        QCOMPARE(bmc.actions, QStringList{u"system ForceRestart"_s});
        QVERIFY2(says(pressed, "queued the change"), saidBy(pressed).c_str());
        QVERIFY2(pace.overrides->standing(bmc.address(), device->machine())
                     .value_or(qkvm::core::SetupOverrides::Left{})
                     .queued,
                 "a queued change never read made, and a start that read the override off, let the record go");
        device.reset();
        pace.goodbyes->finish(pace.signOutWithinMs);
    }

    // A BIOS setup boot override already shown before the press ("Once", "BiosSetup": set by hand in the BMC, or left
    // by an earlier press) proves nothing of a queued change after it (outside review of D1, round 2, L3): a read that
    // shows it restarts nothing while the task runs, the task is followed to its end first, and then the server is
    // restarted into its setup, as asked. No read can show this change made, so its mark stays and the queue is said.
    void anOverrideShownBeforeThePressProvesNothingOfAQueuedChange()
    {
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.bootEnabled = u"Once"_s;
        bmc.bootTarget = u"BiosSetup"_s;
        const QString monitor = u"/redfish/v1/TaskService/Tasks/7/Monitor"_s;
        bmc.bootQueuedAs = Answered(202, {}, "application/json", {"Location: " + monitor.toLatin1()});
        bmc.tasks.insert(monitor, {Answered(202, {}), Answered(202, {}), Answered(204, {})});
        int looksBeforeTheRestart = -1;
        bmc.whenAsked = [&bmc, &monitor, &looksBeforeTheRestart](const Asked& asked) {
            if (asked.method == "POST" && asked.path.endsWith("ComputerSystem.Reset") && looksBeforeTheRestart < 0) {
                looksBeforeTheRestart = bmc.asked(u"GET "_s + monitor);
            }
        };
        const RedfishDevice::Pace pace = taskPace();
        auto device = std::make_unique<RedfishDevice>(at(bmc), bmc.password, pace);
        const base::Result<qkvm::core::Done> pressed = doneBy(*device, "bios_setup");
        QVERIFY2(pressed.ok(), saidBy(pressed).c_str());
        QCOMPARE(bmc.actions, QStringList{u"system ForceRestart"_s});
        QCOMPARE(looksBeforeTheRestart, 3);
        QVERIFY2(says(pressed, "queued the change"), saidBy(pressed).c_str());
        QVERIFY2(pace.overrides->standing(bmc.address(), device->machine())
                     .value_or(qkvm::core::SetupOverrides::Left{})
                     .queued,
                 "an override shown before the press was taken for the queued change's own");
        device.reset();
        pace.goodbyes->finish(pace.signOutWithinMs);
    }

    // A start into BIOS setup whose server, restarted, shows another tool's override in place of the one it was to take
    // (a provisioning tool's network boot for every boot, say): the press ends as one that left it as it is, but that
    // is no override taken, and a record an earlier press marked queued stays marked (outside review of D1, round 2,
    // L6: only "Disabled" read after the restart is the restart having taken it). (A BiosSetup one kept for every boot
    // is qKVM's own, and cleared: aBoardThatKeepsOnceAsContinuousHasQkvmsOwnOverrideClearedAfterTheRestart.)
    void anotherToolsOverrideAfterTheRestartLetsNoQueuedMarkGo()
    {
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.bootNotKept = true;
        const RedfishDevice::Pace pace = setupPace();
        auto device = std::make_unique<RedfishDevice>(at(bmc), bmc.password, pace);
        QVERIFY(says(doneBy(*device, "bios_setup"), "queued the change"));
        QVERIFY(pace.overrides->standing(bmc.address(), device->machine())
                    .value_or(qkvm::core::SetupOverrides::Left{})
                    .queued);
        bmc.bootNotKept = false;
        bmc.startsIntoSetup = false;
        bmc.whenAsked = [&bmc](const Asked& asked) {
            if (asked.method == "POST" && asked.path.endsWith("ComputerSystem.Reset")) {
                bmc.bootEnabled = u"Continuous"_s;
                bmc.bootTarget = u"Pxe"_s;
            }
        };
        const base::Result<qkvm::core::Done> pressed = doneBy(*device, "bios_setup");
        QVERIFY2(pressed.ok(), saidBy(pressed).c_str());
        QVERIFY2(says(pressed, "another tool's now (Pxe, Continuous)"), saidBy(pressed).c_str());
        QVERIFY2(pace.overrides->standing(bmc.address(), device->machine())
                     .value_or(qkvm::core::SetupOverrides::Left{})
                     .queued,
                 "another tool's override after the restart let a queued mark go");
        device.reset();
        pace.goodbyes->finish(pace.signOutWithinMs);
    }

    // And a change answered as one that is made (a 200 with nothing of a task), but not seen made, is no queued one:
    // the press says nothing of a queue, and a look after its doubt lets its record go, as ever.
    void aChangeAnsweredAsMadeIsNoQueuedOne()
    {
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.bootQueuedAs = Answered(200, {});
        RedfishDevice::Pace pace = setupPace();
        qint64 steady = 0;
        pace.overrides = std::make_shared<qkvm::core::SetupOverrides>(
            QString(), [] { return QDateTime::currentDateTimeUtc(); }, [&steady] { return steady; });
        auto device = std::make_unique<RedfishDevice>(at(bmc), bmc.password, pace);
        const auto answer = std::make_shared<std::optional<base::Result<qkvm::core::Done>>>();
        device->act(qkvm::core::Action::plain(u"bios_setup"_s),
                    [answer](const base::Result<qkvm::core::Done>& came) { *answer = came; });
        QTRY_VERIFY_WITH_TIMEOUT(answer->has_value(), 15000);
        QVERIFY2(!QString::fromStdString(saidBy(**answer)).contains(u"queued"_s), saidBy(**answer).c_str());
        QVERIFY(pace.overrides->standing(bmc.address(), device->machine()).has_value());
        QVERIFY(!pace.overrides->standing(bmc.address(), device->machine())
                     .value_or(qkvm::core::SetupOverrides::Left{})
                     .queued);
        steady += qkvm::core::SetupOverrides::longestDoubtMs + 1;
        QVERIFY(pace.overrides->seenOff(bmc.address(), device->machine(), QDateTime::currentDateTimeUtc()));
        device.reset();
        pace.goodbyes->finish(pace.signOutWithinMs);
    }

    // A change the controller only queued, whose task it names (a Task in its answer, or its monitor in the Location
    // header), is followed to its end (D1, from 0.2.0 on, as the web follows AMI's mounts): looked at every
    // confirmEveryMs while it runs. Its "done" is the controller's word, and takes no mark off: only a read of the
    // server after it that shows the change made does (outside review of D1, round 2, H1). Done and then read made: the
    // server restarted into its setup as after a 204, the press ending as it should, saying nothing of a queue, and its
    // record going.
    void aTaskDoneTakesTheMarkOffOnlyWhereTheServerThenShowsTheChange()
    {
        using Left = qkvm::core::SetupOverrides::Left;
        enum class Way { aTask, aTasksMonitor };
        for (const Way way : {Way::aTask, Way::aTasksMonitor}) {
            RedfishByHand bmc;
            offersSetup(bmc);
            const QString place =
                way == Way::aTask ? u"/redfish/v1/TaskService/Tasks/7"_s : u"/redfish/v1/TaskService/Tasks/7/Monitor"_s;
            // (A Task that says what it changes: the server's computer, by a PATCH.)
            const auto task = [&bmc](const char* state) {
                return Answered(200,
                                QByteArray("{\"@odata.id\": \"/redfish/v1/TaskService/Tasks/7\", \"TaskState\": \"") +
                                    state + "\", \"Payload\": {\"TargetUri\": \"" + bmc.system().toLatin1() +
                                    "\", \"HttpOperation\": \"PATCH\"}}");
            };
            if (way == Way::aTask) {
                bmc.bootQueuedAs = task("New");
                bmc.tasks.insert(place, {task("Running"), task("Running"), task("Completed")});
            } else {
                bmc.bootQueuedAs = Answered(202, {}, "application/json", {"Location: " + place.toLatin1()});
                bmc.tasks.insert(place, {Answered(202, {}), Answered(202, {}), Answered(204, {})});
            }
            // (The controller makes the change as its task ends.)
            int looked = 0;
            bmc.whenAsked = [&bmc, &looked, place](const Asked& asked) {
                if (asked.method == "GET" && asked.path == place.toLatin1() && ++looked == 3) {
                    bmc.bootEnabled = u"Once"_s;
                    bmc.bootTarget = u"BiosSetup"_s;
                }
            };
            const RedfishDevice::Pace pace = taskPace();
            auto device = std::make_unique<RedfishDevice>(at(bmc), bmc.password, pace);
            const base::Result<qkvm::core::Done> done = doneBy(*device, "bios_setup");
            QVERIFY2(done.ok(), saidBy(done).c_str());
            QVERIFY2(!says(done, "queued"), saidBy(done).c_str());
            QCOMPARE(bmc.asked(u"GET "_s + place), 3);
            QCOMPARE(bmc.actions, QStringList{u"system ForceRestart"_s});
            QVERIFY(bmc.inSetup);
            QVERIFY2(pace.overrides->all().isEmpty(),
                     "a press whose task was done and that ended well left its record");
            QVERIFY(leftOnTheCard(*device).isEmpty());
            // Done and read made, and its restart refused: the press does not end as it should, but nothing of that
            // change can come any more, and its record is not marked queued: cleared and seen off, it goes as any
            // press's does (a record marked queued would stand).
            bmc.tasks.insert(place, way == Way::aTask ? QList<Answered>{task("Running"), task("Completed")}
                                                      : QList<Answered>{Answered(202, {}), Answered(204, {})});
            looked = 1;
            bmc.resetStatus = 400;
            const base::Result<qkvm::core::Done> refused = doneBy(*device, "bios_setup");
            QVERIFY2(says(refused, "the BIOS setup boot override was cleared again"), saidBy(refused).c_str());
            QVERIFY2(!says(refused, "queued"), saidBy(refused).c_str());
            QVERIFY2(!pace.overrides->standing(bmc.address(), device->machine()).has_value(),
                     "a press whose task was done left its record marked queued");
            // Done by its word, and its override never shown set (a board that stages the change for the next boot,
            // and says its task is done as it stages it): nothing is restarted, it is cleared all the same, and the
            // mark stays and the queue is said, whatever the task says (its task ended, seen under way before).
            bmc.tasks.insert(place, way == Way::aTask ? QList<Answered>{task("Running"), task("Completed")}
                                                      : QList<Answered>{Answered(202, {}), Answered(204, {})});
            bmc.whenAsked = {};
            bmc.resetStatus = 0;
            const qsizetype restarts = bmc.actions.size();
            const base::Result<qkvm::core::Done> unshown = doneBy(*device, "bios_setup");
            QVERIFY2(says(unshown, "the controller's task says it is done, but the server does not show the BIOS "
                                   "setup boot override set"),
                     saidBy(unshown).c_str());
            QVERIFY2(says(unshown, "cleared again"), saidBy(unshown).c_str());
            QVERIFY2(says(unshown, "queued the change: it may still be made later, whatever its task says"),
                     saidBy(unshown).c_str());
            QCOMPARE(bmc.actions.size(), restarts);
            QCOMPARE(bmc.bootEnabled, u"Disabled"_s);
            QVERIFY2(pace.overrides->standing(bmc.address(), device->machine()).value_or(Left{}).queued,
                     "a task's word that it is done took the queued mark off");
            QCOMPARE(leftOnTheCard(*device), qkvm::core::SetupOverrides::onTheCard(true));
            // Shown made while its task runs, and its restart refused: nothing took the change, and the task was not
            // seen to end; the mark stays, and the press says the queue.
            pace.overrides->forget(bmc.address(), device->machine());
            bmc.tasks.insert(place,
                             way == Way::aTask ? QList<Answered>{task("Running")} : QList<Answered>{Answered(202, {})});
            bmc.whenAsked = [&bmc](const Asked& asked) {
                if (asked.method == "PATCH" && asked.body.contains("BiosSetup")) {
                    bmc.bootEnabled = u"Once"_s;
                    bmc.bootTarget = u"BiosSetup"_s;
                }
            };
            bmc.resetStatus = 400;
            const base::Result<qkvm::core::Done> notTaken = doneBy(*device, "bios_setup");
            QVERIFY2(says(notTaken, "cleared again"), saidBy(notTaken).c_str());
            QVERIFY2(says(notTaken, "queued the change"), saidBy(notTaken).c_str());
            QVERIFY2(pace.overrides->standing(bmc.address(), device->machine()).value_or(Left{}).queued,
                     "a change shown made, and taken by no restart, took the queued mark off");
            device.reset();
            pace.goodbyes->finish(pace.signOutWithinMs);
        }
    }

    // One whose task ends without the change (in an exception), seen under way before, and one whose monitor answers
    // an error (the change's own answer, as AMI's real board said 403 as its task failed): no restart, the override
    // cleared all the same, in the controller's own words. Neither is a read of the server, so neither takes the mark
    // off, and the queue is said (outside review of D1, H1; round 2, H1). A mark an earlier press put on the record
    // stays where a later press's own change is read made after its task's end, and goes, as in 0.1.2, with a later
    // press that ends as it should (D1).
    void aQueuedChangeWhoseTaskFailsIsClearedAndToldInItsWords()
    {
        using Left = qkvm::core::SetupOverrides::Left;
        enum class Way { anException, aMonitorsError };
        for (const Way way : {Way::anException, Way::aMonitorsError}) {
            RedfishByHand bmc;
            offersSetup(bmc);
            if (way == Way::anException) {
                bmc.bootQueuedAs =
                    Answered(200, "{\"@odata.id\": \"/redfish/v1/TaskService/Tasks/8\", \"TaskState\": \"New\"}");
                bmc.tasks.insert(
                    u"/redfish/v1/TaskService/Tasks/8"_s,
                    {Answered(200, "{\"TaskState\": \"Running\"}"),
                     Answered(
                         200,
                         "{\"TaskState\": \"Exception\", \"TaskStatus\": \"Critical\", \"Messages\": [{\"Message\": "
                         "\"BiosSetup is not allowed now.\"}]}")});
            } else {
                bmc.bootQueuedAs =
                    Answered(202, {}, "application/json", {"Location: /redfish/v1/TaskService/Tasks/8/Monitor"});
                bmc.tasks.insert(u"/redfish/v1/TaskService/Tasks/8/Monitor"_s,
                                 {Answered(202, {}),
                                  Answered(400, "{\"error\": {\"message\": \"BiosSetup is not allowed now.\"}}")});
            }
            const RedfishDevice::Pace pace = taskPace();
            auto device = std::make_unique<RedfishDevice>(at(bmc), bmc.password, pace);
            const base::Result<qkvm::core::Done> failed = doneBy(*device, "bios_setup");
            QCOMPARE(codeOf(failed), std::string("action_failed"));
            QVERIFY2(says(failed, way == Way::anException
                                      ? "the controller's task did not set the BIOS setup boot override (BiosSetup is "
                                        "not allowed now.)"
                                      : "the controller answered for its task with an error (BiosSetup is not allowed "
                                        "now.)"),
                     saidBy(failed).c_str());
            QVERIFY2(says(failed, "cleared again"), saidBy(failed).c_str());
            QVERIFY(bmc.actions.isEmpty());
            QCOMPARE(bootChanges(bmc), (QStringList{u"Once BiosSetup"_s, u"Disabled"_s}));
            QVERIFY2(says(failed, "queued the change"), saidBy(failed).c_str());
            QVERIFY2(pace.overrides->standing(bmc.address(), device->machine()).value_or(Left{}).queued,
                     way == Way::anException ? "a task's word that it failed took the queued mark off"
                                             : "a monitor's error answer took off a queued change's mark");
            if (way == Way::aMonitorsError) {
                device.reset();
                pace.goodbyes->finish(pace.signOutWithinMs);
                continue;
            }
            QVERIFY2(says(failed, "whatever its task says"), saidBy(failed).c_str());
            // A later press whose own change is read made after its task's end, and whose restart is refused: the mark
            // the earlier press put on the record stays (that read says nothing of the earlier change).
            const QString later = u"/redfish/v1/TaskService/Tasks/9"_s;
            bmc.bootQueuedAs = Answered(200, "{\"@odata.id\": \"/redfish/v1/TaskService/Tasks/9\", \"TaskState\": "
                                             "\"New\"}");
            const auto madeAsItEnds = [&bmc, &later] {
                bmc.tasks.insert(later, {Answered(200, "{\"TaskState\": \"Running\"}"),
                                         Answered(200, "{\"TaskState\": \"Completed\"}")});
                bmc.whenAsked = [&bmc, &later](const Asked& asked) {
                    if (asked.method == "GET" && asked.path == later.toLatin1() && bmc.asked(u"GET "_s + later) == 1) {
                        bmc.bootEnabled = u"Once"_s;
                        bmc.bootTarget = u"BiosSetup"_s;
                    }
                };
            };
            madeAsItEnds();
            bmc.resetStatus = 400;
            const base::Result<qkvm::core::Done> refused = doneBy(*device, "bios_setup");
            QVERIFY2(says(refused, "cleared again"), saidBy(refused).c_str());
            QVERIFY2(pace.overrides->standing(bmc.address(), device->machine()).value_or(Left{}).queued,
                     "a later press's change read made took off a mark an earlier press put on the record");
            // One that ends as it should (its change read made, and taken by the server as it restarts): the record
            // goes, its earlier mark with it, as in 0.1.2.
            bmc.requests.clear();
            madeAsItEnds();
            bmc.resetStatus = 0;
            const base::Result<qkvm::core::Done> normal = doneBy(*device, "bios_setup");
            QVERIFY2(normal.ok(), saidBy(normal).c_str());
            QVERIFY2(!says(normal, "queued"), saidBy(normal).c_str());
            QVERIFY2(!pace.overrides->standing(bmc.address(), device->machine()).has_value(),
                     "a start into BIOS setup that ended as it should left a queued record standing");
            device.reset();
            pace.goodbyes->finish(pace.signOutWithinMs);
        }
    }

    // Only a task that surely ended, and surely this change's, takes the queued mark off (outside review of D1, H1):
    // its end seen at a look after the task was seen under way at one before, of the task the change named, of no
    // other change. None of these takes it off, and each is told and marked as 0.1.2 has a queued change: a monitor's
    // passing error (a 503 then a 204, never seen under way; a 500 for good), an empty answer at a Task's own place, an
    // old task the answer names (ended at the first look), a look that names another task, a Task of a change of
    // another place, a state no Task has, and a monitor's last answer that says nothing of how it went. One followed
    // on through a passing error to its end does, by the read after it that shows the change made (round 2, H1). An
    // end at the first look keeps the controller's words of a failure, said as not surely this change's; and after
    // such a failure, a computer that cannot be read back is not restarted blind (outside review of D1, round 2, L2).
    void aTasksEndTakesTheMarkOffOnlyWhereItIsSurelyThisChangesEnd()
    {
        using Left = qkvm::core::SetupOverrides::Left;
        const QString monitor = u"/redfish/v1/TaskService/Tasks/7/Monitor"_s;
        const QString itself = u"/redfish/v1/TaskService/Tasks/7"_s;
        const auto running = [] { return Answered(200, "{\"TaskState\": \"Running\"}"); };
        struct Row {
            const char* name;
            Answered patched; // the change's answer
            QHash<QString, QList<Answered>> tasks;
            bool off = false;            // the mark taken off, the server restarted
            const char* words = nullptr; // what the press says beside the queue
            bool silent = false;         // its computer answers no read from the change on
            bool shownRefused = false;   // ...or shows the change from its task's first look on, its restart refused
        };
        const auto queuedAt = [](const QString& place) {
            return Answered(202, {}, "application/json", {"Location: " + place.toLatin1()});
        };
        const QByteArray busy = "{\"error\": {\"message\": \"Busy.\"}}";
        const QList<Row> rows{
            {"a monitor's 503, then 204, never seen under way",
             queuedAt(monitor),
             {{monitor, {Answered(503, busy), Answered(204, {})}}}},
            {"a monitor's 500 for good", queuedAt(monitor), {{monitor, {Answered(500, busy)}}}},
            {"a Task's place answering 204", queuedAt(itself), {{itself, {running(), Answered(204, {})}}}},
            {"a Task's place answering 200 with nothing", queuedAt(itself), {{itself, {running(), Answered(200, {})}}}},
            // (An old task's end at the first look is no end of this change's, H1: where its computer then shows the
            // change made and its restart is refused, nothing took the change and nothing saw it through, so the mark
            // stays. Taken for this change's end, the read that shows it would have seen it through, and let the mark
            // go: the behaviour that tells H1's two apart, ahead of the words below. Outside review of 0.2.0's Redfish
            // fixes, L7.)
            {"an old task named, ended at the first look, then shown set, its restart refused",
             Answered(202, "{\"@odata.id\": \"/redfish/v1/TaskService/Tasks/1\", \"TaskState\": \"Completed\"}"),
             {{u"/redfish/v1/TaskService/Tasks/1"_s, {Answered(200, "{\"TaskState\": \"Completed\"}")}}},
             false,
             nullptr,
             false,
             true},
            // (Told as a task that cannot be followed, not as one whose end was seen: an end at the first look is no
            // end of this change's, H1. Where the server never shows the change, the words are all that tells the two
            // apart: the read-back shows the override unset either way, the mark stays either way.)
            {"an old task named, ended at the first look",
             Answered(202, "{\"@odata.id\": \"/redfish/v1/TaskService/Tasks/1\", \"TaskState\": \"Completed\"}"),
             {{u"/redfish/v1/TaskService/Tasks/1"_s, {Answered(200, "{\"TaskState\": \"Completed\"}")}}},
             false,
             "the controller took the BIOS setup boot override and does not show it set"},
            {"an old task named, ended in an exception at the first look",
             Answered(202, "{\"@odata.id\": \"/redfish/v1/TaskService/Tasks/1\"}"),
             {{u"/redfish/v1/TaskService/Tasks/1"_s,
               {Answered(200, "{\"TaskState\": \"Exception\", \"Messages\": [{\"Message\": \"Another time.\"}]}")}}},
             false,
             "does not show it set; its task said at once that it did not set it (Another time.), and qKVM cannot be "
             "sure that task was this change's"},
            {"an old task named, ended in an exception at the first look, its computer not read back",
             Answered(202, "{\"@odata.id\": \"/redfish/v1/TaskService/Tasks/1\"}"),
             {{u"/redfish/v1/TaskService/Tasks/1"_s,
               {Answered(200, "{\"TaskState\": \"Exception\", \"Messages\": [{\"Message\": \"Another time.\"}]}")}}},
             false,
             "Nothing was restarted: the controller's task said at once that it did not set the BIOS setup boot "
             "override (Another time.), qKVM cannot be sure that task was this change's, and the server could not be "
             "read back",
             true},
            {"a monitor's error at its first look (one that takes no GET)",
             queuedAt(monitor),
             {{monitor, {Answered(405, "{\"error\": {\"message\": \"Not allowed.\"}}")}}},
             false,
             "does not show it set; its task's monitor answered at once with an error (Not allowed.), and qKVM cannot "
             "be sure that answer was of this change"},
            {"a monitor's error at its first look, its computer not read back",
             queuedAt(monitor),
             {{monitor, {Answered(405, "{\"error\": {\"message\": \"Not allowed.\"}}")}}},
             false,
             "Nothing was restarted: its task's monitor answered at once with an error (Not allowed.), qKVM cannot be "
             "sure that answer was of this change, and the server could not be read back",
             true},
            {"a look that names another task",
             queuedAt(itself),
             {{itself,
               {running(),
                Answered(200, "{\"@odata.id\": \"/redfish/v1/TaskService/Tasks/3\", \"TaskState\": \"Completed\"}")}}}},
            {"a Task of a change of another place",
             queuedAt(itself),
             {{itself,
               {running(), Answered(200, "{\"TaskState\": \"Completed\", \"Payload\": {\"TargetUri\": "
                                         "\"/redfish/v1/Managers/Self\", \"HttpOperation\": \"PATCH\"}}")}}}},
            {"a state no Task has",
             queuedAt(itself),
             {{itself, {running(), Answered(200, "{\"TaskState\": \"Done\"}")}}}},
            {"a monitor's last answer that says nothing of how it went",
             queuedAt(monitor),
             {{monitor,
               {Answered(202, {}), Answered(200, "{\"Boot\": {\"BootSourceOverrideEnabled\": \"Once\", "
                                                 "\"BootSourceOverrideTarget\": \"BiosSetup\"}}")}}}},
            {"a monitor followed on through a 503 to its 204",
             queuedAt(monitor),
             {{monitor, {Answered(202, {}), Answered(503, busy), Answered(204, {})}}},
             true},
        };
        for (const Row& row : rows) {
            RedfishByHand bmc;
            offersSetup(bmc);
            bmc.bootQueuedAs = row.patched;
            bmc.tasks = row.tasks;
            if (row.off) {
                // (The controller makes the change as its task ends.)
                bmc.whenAsked = [&bmc, &monitor](const Asked& asked) {
                    if (asked.method == "GET" && asked.path == monitor.toLatin1() &&
                        bmc.asked(u"GET "_s + monitor) == 2) {
                        bmc.bootEnabled = u"Once"_s;
                        bmc.bootTarget = u"BiosSetup"_s;
                    }
                };
            } else if (row.silent) {
                bmc.whenAsked = [&bmc](const Asked& asked) {
                    if (asked.method == "PATCH" && asked.body.contains("BiosSetup")) {
                        bmc.systemUnwell = true;
                    }
                };
            } else if (row.shownRefused) {
                // (Made as its task is first looked at: the read before that look shows nothing, so the look is made,
                // and the reads after it show the change.)
                bmc.whenAsked = [&bmc](const Asked& asked) {
                    if (asked.method == "GET" && asked.path == "/redfish/v1/TaskService/Tasks/1") {
                        bmc.bootEnabled = u"Once"_s;
                        bmc.bootTarget = u"BiosSetup"_s;
                    }
                };
                bmc.resetStatus = 400;
            }
            RedfishDevice::Pace pace = taskPace();
            pace.mostTaskLooks = 4; // (few of them)
            auto device = std::make_unique<RedfishDevice>(at(bmc), bmc.password, pace);
            const base::Result<qkvm::core::Done> pressed = doneBy(*device, "bios_setup");
            const std::optional<Left> left = pace.overrides->standing(bmc.address(), device->machine());
            if (row.off) {
                QVERIFY2(pressed.ok(), (std::string(row.name) + ": " + saidBy(pressed)).c_str());
                QVERIFY2(!says(pressed, "queued"), saidBy(pressed).c_str());
                QCOMPARE(bmc.actions, QStringList{u"system ForceRestart"_s});
                QVERIFY2(!left.has_value(), row.name);
            } else {
                // (The mark first: what the press did, before what it says of it.)
                QVERIFY2(left.value_or(Left{}).queued, (std::string(row.name) + ": the queued mark went").c_str());
                QVERIFY2(says(pressed, "queued the change"), (std::string(row.name) + ": " + saidBy(pressed)).c_str());
                QVERIFY2(bmc.actions.isEmpty(), row.name);
                QCOMPARE(bmc.bootEnabled, u"Disabled"_s);
                if (row.words != nullptr) {
                    QVERIFY2(says(pressed, row.words), (std::string(row.name) + ": " + saidBy(pressed)).c_str());
                }
            }
            device.reset();
            pace.goodbyes->finish(pace.signOutWithinMs);
        }
    }

    // A task that does not end within the press uses up none of its restart's time (outside review of D1, M1; round 2,
    // L1 and L5), at the program's own pace made shorter by one measure (the defaults over 20: the restart within 6 s,
    // the read-back's 1.25 s, a look every 100 ms; a look's own limits are quick()'s, 4.5 s in all, longer than the
    // read-back as a look's 48.5 s is in life). (a) A board whose task ends only at the next boot, as pending settings
    // do, and that shows the override set at once: its computer is read before the task's first look, and restarted
    // then, as 0.1.2 restarted it at once, the press ending as it should and its record going with it. (b) One whose
    // computer answers nothing through the task's time: the follow ends within the task's own time and leaves the
    // read-back its time before the restart's; the override read then shows set, and the server is restarted. (c) One
    // that answers the change so late that the read-back's time has begun (from the press, whatever the reads before
    // the change took): its task is not looked at at all, and the override read back; (c') so too where its computer
    // shows the change only from its second read after the change (the first read before a look, which a follow would
    // make, then shows nothing, and a look would follow it: the fuzz work's removal M1-1 seen again). (d) One whose
    // monitor, late in the task's time, answers a byte at a time for longer than the read-back takes, and whose
    // computer shows the change only from the end of the task's time: that look is cut at the follow's end, and the
    // override read back in time. (e) One whose computer, late in the task's time, answers a byte at a time so (and
    // shows the change only from the end of the task's time): that read is cut at the follow's end as well. In (d) and
    // (e) the reads before the change take 700 ms each, as under the address sanitizer on a loaded PC, so that the
    // restart's own time ends the follow before the task's, and the slow answers come late in the follow's time as the
    // device has it, not as the task's alone would have it (outside review of the fuzz work, round 3, L5). Never a
    // restart after the press's time: no power action nobody asked for.
    void aTaskThatDoesNotEndUsesUpNoneOfTheRestartsTime()
    {
        enum class Way {
            shownAtOnce,
            computerSilentThroughTheTasksTime,
            changeAnsweredLate,
            changeAnsweredLateShownAtTheSecondRead,
            aLookSlowerThanTheReadBack,
            aReadSlowerThanTheReadBack
        };
        for (const Way way : {Way::shownAtOnce, Way::computerSilentThroughTheTasksTime, Way::changeAnsweredLate,
                              Way::changeAnsweredLateShownAtTheSecondRead, Way::aLookSlowerThanTheReadBack,
                              Way::aReadSlowerThanTheReadBack}) {
            const bool answeredLate =
                way == Way::changeAnsweredLate || way == Way::changeAnsweredLateShownAtTheSecondRead;
            RedfishByHand bmc;
            offersSetup(bmc);
            const QString monitor = u"/redfish/v1/TaskService/Tasks/7/Monitor"_s;
            bmc.bootQueuedAs = Answered(202, {}, "application/json", {"Location: " + monitor.toLatin1()});
            bmc.tasks.insert(monitor, {Answered(202, {})});
            const RedfishDevice::Pace defaults;
            // (The program's own pace: a task's time is half the restart's window at the most, the rest the read-back's
            // and the restart's.)
            QVERIFY(defaults.taskWithinMs <= defaults.setupWithinMs / 2);
            QVERIFY(defaults.taskWithinMs + defaults.confirmWithinMs < defaults.setupWithinMs);
            constexpr int over = 20;
            RedfishDevice::Pace pace = setupPace();
            pace.setupWithinMs = defaults.setupWithinMs / over;
            pace.setupPatienceMs = defaults.setupPatienceMs / over;
            pace.bootQuietMs = defaults.bootQuietMs / over;
            pace.confirmWithinMs = defaults.confirmWithinMs / over;
            pace.confirmEveryMs = defaults.confirmEveryMs / over;
            pace.taskWithinMs = defaults.taskWithinMs / over;
            pace.mostTaskLooks = defaults.mostTaskLooks;
            if (answeredLate) {
                pace.bootQuietMs = pace.setupWithinMs; // (the change's answer waited for as long as it takes)
            }
            const auto showsTheChange = [&bmc] {
                bmc.bootEnabled = u"Once"_s;
                bmc.bootTarget = u"BiosSetup"_s;
            };
            QElapsedTimer clock;
            clock.start();
            qint64 pressedAt = -1;
            qint64 changedAt = -1;
            qint64 lastLook = -1;
            int looksBeforeTheRestart = -1;
            int failedReads = 0;
            int readsAfterTheChange = 0;
            const bool slowAnswers = way == Way::aLookSlowerThanTheReadBack || way == Way::aReadSlowerThanTheReadBack;
            const auto followEndsAt = [&changedAt, &pressedAt, &pace] {
                return std::min(changedAt + pace.taskWithinMs, pressedAt + pace.setupWithinMs - pace.confirmWithinMs);
            };
            bmc.whenAsked = [&](const Asked& asked) {
                const bool aRead = asked.method == "GET" && asked.path == bmc.system().toLatin1();
                if (aRead && slowAnswers) {
                    // (Its reads before the change slow, as under the address sanitizer on a busy PC: the restart's
                    // own time binds the follow's then, not the task's, outside review of the fuzz work, round 3, L5.)
                    bmc.systemTakesMs = changedAt < 0 ? 700 : 0;
                }
                if (aRead && way == Way::changeAnsweredLateShownAtTheSecondRead && changedAt >= 0 &&
                    ++readsAfterTheChange == 2) {
                    showsTheChange(); // (as this second read is answered: the first showed nothing)
                }
                if (asked.method == "PATCH" && asked.body.contains("BiosSetup")) {
                    changedAt = clock.elapsed();
                    if (answeredLate) {
                        // (Answered a fifth of the read-back's time into it, counted from the press.)
                        bmc.bootTakesMs =
                            static_cast<int>(std::max<qint64>(0, pressedAt + pace.setupWithinMs - pace.confirmWithinMs +
                                                                     (pace.confirmWithinMs / 5) - changedAt));
                    }
                    if (way != Way::aLookSlowerThanTheReadBack && way != Way::aReadSlowerThanTheReadBack &&
                        way != Way::changeAnsweredLateShownAtTheSecondRead) {
                        showsTheChange(); // (made at once, its task ending only at the next boot)
                    }
                }
                const qint64 sinceTheChange = changedAt < 0 ? -1 : clock.elapsed() - changedAt;
                // (Until the end of the follow's time as the device has it: its task's from the change, or the
                // restart's less the read-back's from the press, whichever comes first; the device's own is no
                // earlier, its press and the change's answer coming after these. Round 3, L5.)
                const qint64 toTheEnd =
                    changedAt < 0 ? std::numeric_limits<qint64>::max() : followEndsAt() - clock.elapsed();
                if (aRead && way == Way::aReadSlowerThanTheReadBack) {
                    // (Late in its task's time its computer answers a byte at a time, never to an end; from its end on
                    // at once, showing the change.)
                    const bool late = toTheEnd <= 400;
                    const bool over = toTheEnd <= 0;
                    bmc.systemByteEveryMs = late && !over ? 50 : 0;
                    if (over) {
                        showsTheChange();
                    }
                }
                if (asked.method == "GET" && asked.path == monitor.toLatin1()) {
                    lastLook = clock.elapsed();
                    if (way == Way::aLookSlowerThanTheReadBack && toTheEnd <= 400) {
                        // (Late in its task's time, its monitor answers a byte at a time, never to an end.)
                        Answered slowly(202, QByteArray(4000, ' '));
                        slowly.byteEveryMs = 50;
                        bmc.tasks.insert(monitor, {slowly});
                    }
                }
                if (aRead && way == Way::aLookSlowerThanTheReadBack && toTheEnd <= 0) {
                    showsTheChange();
                }
                if (aRead && way == Way::computerSilentThroughTheTasksTime) {
                    bmc.systemUnwell = changedAt >= 0 && sinceTheChange < pace.taskWithinMs + 100;
                    failedReads += bmc.systemUnwell ? 1 : 0;
                }
                if (asked.method == "POST" && asked.path.endsWith("ComputerSystem.Reset") &&
                    looksBeforeTheRestart < 0) {
                    looksBeforeTheRestart = bmc.asked(u"GET "_s + monitor);
                }
            };
            auto device = std::make_unique<RedfishDevice>(at(bmc), bmc.password, pace);
            pressedAt = clock.elapsed();
            const base::Result<qkvm::core::Done> pressed = doneBy(*device, "bios_setup");
            QVERIFY2(pressed.ok(), saidBy(pressed).c_str());
            QVERIFY2(!says(pressed, "queued"), saidBy(pressed).c_str());
            QCOMPARE(bmc.actions, QStringList{u"system ForceRestart"_s});
            QVERIFY(bmc.inSetup);
            QVERIFY2(pace.overrides->all().isEmpty(), "a press that ended as it should left its record");
            switch (way) {
            case Way::shownAtOnce:
                // (Read before its task's first look, and restarted then.)
                QCOMPARE(looksBeforeTheRestart, 0);
                break;
            case Way::changeAnsweredLate:
                QCOMPARE(bmc.asked(u"GET "_s + monitor), 0);
                break;
            case Way::changeAnsweredLateShownAtTheSecondRead:
                QCOMPARE(bmc.asked(u"GET "_s + monitor), 0);
                QVERIFY2(readsAfterTheChange >= 2, qPrintable(QString::number(readsAfterTheChange)));
                break;
            case Way::computerSilentThroughTheTasksTime:
                // (Its task's time, 3 s at this pace, a look's pause and a moment more: half the window, not all of
                // it; and its computer was read while it answered nothing.)
                QVERIFY2(failedReads >= 1, "no read of the computer failed while its task was looked at");
                QVERIFY2(lastLook - changedAt <= pace.taskWithinMs + pace.confirmEveryMs + 500,
                         qPrintable(u"the last look at its task %1 ms after the change"_s.arg(lastLook - changedAt)));
                break;
            case Way::aLookSlowerThanTheReadBack:
            case Way::aReadSlowerThanTheReadBack:
                // (Its last look begun within the follow's time: the slow answer cut at its end. The room is a look's
                // pause and the way of an answer and a look, loaded, under the sanitizer: round 3, L5.)
                QVERIFY2(lastLook <= followEndsAt() + pace.confirmEveryMs + 400,
                         qPrintable(u"the last look at its task %1 ms after the follow's end"_s.arg(lastLook -
                                                                                                    followEndsAt())));
                break;
            }
            device.reset();
            pace.goodbyes->finish(pace.signOutWithinMs);
        }
    }

    // The follow's time holds whatever comes in it (outside review of the fuzz work, round 3, L3): in it, the
    // controller forgets its session, and its computer's read is refused (401). (a) Late in it, and the sign-in that
    // follows takes longer than the follow has left; (b) halfway through it, and the server gives no numbers, so that
    // which server answers is judged by its controller's, whose read (under the new session) takes longer than that.
    // Either way the follow ends at its time, its read answered as too late and not sent after it, and the read-back
    // keeps its own time: the press, whose change never shows, ends within the task's time and the read-back's of the
    // change, and not a sign-in's or a judge's later.
    void aFollowsTimeHoldsThroughASignInAndTheJudgesRead()
    {
        enum class Way { aSignIn, theJudgesRead };
        for (const Way way : {Way::aSignIn, Way::theJudgesRead}) {
            RedfishByHand bmc;
            offersSetup(bmc);
            if (way == Way::theJudgesRead) {
                bmc.uuid = QJsonValue();
                bmc.rootUuid = QJsonValue();
            }
            const QString monitor = u"/redfish/v1/TaskService/Tasks/7/Monitor"_s;
            bmc.bootQueuedAs = Answered(202, {}, "application/json", {"Location: " + monitor.toLatin1()});
            bmc.tasks.insert(monitor, {Answered(202, {})});
            RedfishDevice::Pace pace = setupPace();
            pace.setupWithinMs = 12'000;
            pace.setupPatienceMs = 16'000;
            pace.taskWithinMs = 3000;
            pace.confirmWithinMs = 3000;
            pace.confirmEveryMs = 200;
            pace.mostTaskLooks = 1000;
            QElapsedTimer clock;
            clock.start();
            qint64 changedAt = -1;
            bool forgotten = false;
            bool slowJudge = false;
            QStringList asks; // (what was asked late in the follow and after it, for a failure's words)
            // When the controller forgets the session: late in the follow for the sign-in, which then outlasts it; and
            // halfway through it for the judge's read, so that the sign-in before it, the computer read again and the
            // judge's read itself come inside the follow on a loaded machine too (the forgetting 400 ms before the
            // follow's end had them come after it now and then on the build box: the judge's read was then the
            // read-back's, which holds to no time of the follow's, and the press ended late).
            const qint64 forgetsAt = way == Way::aSignIn ? pace.taskWithinMs - 400 : pace.taskWithinMs / 2;
            bmc.whenAsked = [&](const Asked& asked) {
                if (asked.method == "PATCH" && asked.body.contains("BiosSetup")) {
                    changedAt = clock.elapsed();
                }
                if (changedAt >= 0 && clock.elapsed() - changedAt >= forgetsAt - 200) {
                    asks.append(u"%1 %2 %3"_s.arg(clock.elapsed() - changedAt)
                                    .arg(QString::fromLatin1(asked.method), QString::fromLatin1(asked.path)));
                }
                if (asked.method == "GET" && asked.path == bmc.manager().toLatin1()) {
                    // (The judge's read after the forgetting answers a byte at a time, for longer than the follow has
                    // left, and is cut at its whole time unless held to the follow's; those after it, at once.)
                    bmc.managerByteEveryMs = slowJudge ? 10 : 0;
                    slowJudge = false;
                }
                if (!forgotten && changedAt >= 0 && asked.method == "GET" && asked.path == bmc.system().toLatin1() &&
                    clock.elapsed() - changedAt >= forgetsAt) {
                    forgotten = true;
                    bmc.sessions.clear();
                    if (way == Way::aSignIn) {
                        bmc.loginTakesMs = 2500;
                    } else {
                        slowJudge = true;
                    }
                }
            };
            auto device = std::make_unique<RedfishDevice>(at(bmc), bmc.password, pace);
            const base::Result<qkvm::core::Done> pressed = doneBy(*device, "bios_setup");
            const qint64 ended = clock.elapsed();
            QVERIFY(forgotten);
            QVERIFY2(says(pressed, "does not show it set"), saidBy(pressed).c_str());
            QVERIFY(bmc.actions.isEmpty());
            QCOMPARE(bmc.bootEnabled, u"Disabled"_s);
            QVERIFY2(ended - changedAt <= pace.taskWithinMs + pace.confirmWithinMs + 1500,
                     qPrintable(u"%1: the press ended %2 ms after the change, signed in %3 times; asked %4"_s
                                    .arg(way == Way::aSignIn ? u"a sign-in"_s : u"the judge's read"_s)
                                    .arg(ended - changedAt)
                                    .arg(bmc.logins)
                                    .arg(asks.first(std::min<qsizetype>(asks.size(), 40)).join(u"; "_s))));
            if (way == Way::aSignIn) {
                QCOMPARE(bmc.logins, 2);
            }
            device.reset();
            pace.goodbyes->finish(pace.signOutWithinMs);
        }
    }

    // The restart is held to its two minutes when it goes, not only when it is asked for (outside review of the fuzz
    // work, round 3, L4): the controller forgets its session as the restart comes, refuses it (401: not done), and the
    // sign-in again takes past the restart's time. The restart is not sent again: the press says it took too long,
    // and the override is cleared. No power action after the press's time, and none twice.
    void aRestartThatWouldGoPastItsTimeIsNotSent()
    {
        RedfishByHand bmc;
        offersSetup(bmc);
        RedfishDevice::Pace pace = setupPace();
        // (The sign-in again takes longer than the restart's whole time, so that it ends past that time whenever the
        // first restart went, on a loaded machine too; its answer waited for.)
        const int signInTakesMs = pace.setupWithinMs + 500;
        pace.signInQuietMs = signInTakesMs + 2000;
        int restarts = 0;
        bmc.whenAsked = [&bmc, &restarts, signInTakesMs](const Asked& asked) {
            if (asked.method == "POST" && asked.path.endsWith("ComputerSystem.Reset") && ++restarts == 1) {
                bmc.sessions.clear();
                bmc.loginTakesMs = signInTakesMs;
            }
        };
        auto device = std::make_unique<RedfishDevice>(at(bmc), bmc.password, pace);
        const base::Result<qkvm::core::Done> pressed = doneBy(*device, "bios_setup");
        QCOMPARE(codeOf(pressed), std::string("action_failed"));
        QVERIFY2(says(pressed, "Nothing was restarted: the controller took more than 4 seconds before the restart; the "
                               "BIOS setup boot override was cleared again."),
                 saidBy(pressed).c_str());
        QCOMPARE(restarts, 1);
        QVERIFY2(bmc.actions.isEmpty(), "a restart went out after the press's time");
        QCOMPARE(bmc.bootEnabled, u"Disabled"_s);
        QTest::qWait(500);
        QCOMPARE(restarts, 1);
        device.reset();
        pace.goodbyes->finish(pace.signOutWithinMs);
    }

    // A task seen under way and then done, whose computer answers no read from the change on: restarted as after a 202
    // in 0.1.2, the task's word and the press being all there is to go by (outside review of D1, L7, said as it is in
    // SPEC). But no read showed the change, so its mark stays and the queue is said, as after a 202 in 0.1.2 (outside
    // review of D1, round 2, H1). A read that shows the override unset restarts nothing
    // (aTaskDoneTakesTheMarkOffOnlyWhereTheServerThenShowsTheChange).
    void aTaskDoneWhoseComputerIsNotReadBackIsRestartedAndStaysMarked()
    {
        RedfishByHand bmc;
        offersSetup(bmc);
        const QString monitor = u"/redfish/v1/TaskService/Tasks/7/Monitor"_s;
        bmc.bootQueuedAs = Answered(202, {}, "application/json", {"Location: " + monitor.toLatin1()});
        bmc.tasks.insert(monitor, {Answered(202, {}), Answered(204, {})});
        // (Its computer answers 503 from the change to the restart.)
        bmc.whenAsked = [&bmc](const Asked& asked) {
            if (asked.method == "PATCH" && asked.body.contains("BiosSetup")) {
                bmc.systemUnwell = true;
            }
            if (asked.method == "POST" && asked.path.endsWith("ComputerSystem.Reset")) {
                bmc.systemUnwell = false;
            }
        };
        const RedfishDevice::Pace pace = taskPace();
        auto device = std::make_unique<RedfishDevice>(at(bmc), bmc.password, pace);
        const base::Result<qkvm::core::Done> pressed = doneBy(*device, "bios_setup");
        QVERIFY2(pressed.ok(), saidBy(pressed).c_str());
        QCOMPARE(bmc.asked(u"GET "_s + monitor), 2);
        QCOMPARE(bmc.actions, QStringList{u"system ForceRestart"_s});
        QVERIFY2(says(pressed, "queued the change: it may still be made later, whatever its task says"),
                 saidBy(pressed).c_str());
        QVERIFY2(pace.overrides->standing(bmc.address(), device->machine())
                     .value_or(qkvm::core::SetupOverrides::Left{})
                     .queued,
                 "a task's word that it is done, read back by nothing, took the queued mark off");
        device.reset();
        pace.goodbyes->finish(pace.signOutWithinMs);
    }

    // ...and one whose computer was read while its task ran, showing the override unset as it should then, and answers
    // no read from the task's end on: no read after the end shows it unset, so it is restarted as after a 202 in 0.1.2
    // and as SPEC's table has it, its mark kept and the queue said (outside review of the fuzz work, round 3, L1: the
    // reads before the end said nothing of the end, and were taken for "does not show it set").
    void aTaskDoneWhoseComputerIsReadOnlyWhileItRunsIsRestartedAndStaysMarked()
    {
        RedfishByHand bmc;
        offersSetup(bmc);
        const QString monitor = u"/redfish/v1/TaskService/Tasks/7/Monitor"_s;
        bmc.bootQueuedAs = Answered(202, {}, "application/json", {"Location: " + monitor.toLatin1()});
        bmc.tasks.insert(monitor, {Answered(202, {}), Answered(202, {}), Answered(204, {})});
        int looks = 0;
        int readsWhileItRan = 0;
        bmc.whenAsked = [&bmc, &monitor, &looks, &readsWhileItRan](const Asked& asked) {
            if (asked.method == "GET" && asked.path == monitor.toLatin1() && ++looks == 3) {
                bmc.systemUnwell = true; // (from its end on)
            }
            if (asked.method == "GET" && asked.path == bmc.system().toLatin1() && looks > 0 && !bmc.systemUnwell) {
                ++readsWhileItRan;
            }
            if (asked.method == "POST" && asked.path.endsWith("ComputerSystem.Reset")) {
                bmc.systemUnwell = false;
            }
        };
        const RedfishDevice::Pace pace = taskPace();
        auto device = std::make_unique<RedfishDevice>(at(bmc), bmc.password, pace);
        const base::Result<qkvm::core::Done> pressed = doneBy(*device, "bios_setup");
        QVERIFY2(readsWhileItRan >= 1, "its computer was not read while its task ran");
        QCOMPARE(looks, 3);
        QVERIFY2(pressed.ok(), saidBy(pressed).c_str());
        QCOMPARE(bmc.actions, QStringList{u"system ForceRestart"_s});
        QVERIFY2(says(pressed, "queued the change: it may still be made later, whatever its task says"),
                 saidBy(pressed).c_str());
        QVERIFY2(pace.overrides->standing(bmc.address(), device->machine())
                     .value_or(qkvm::core::SetupOverrides::Left{})
                     .queued,
                 "a task's word that it is done, read back by nothing after it, took the queued mark off");
        device.reset();
        pace.goodbyes->finish(pace.signOutWithinMs);
    }

    // A failure its task said at its first look (perhaps an old task's), or an error its monitor answered then (round
    // 3, L6 b), and then a read that shows the change made: the server is restarted into its setup, and the
    // controller's words are said beside it (outside review of the fuzz work, round 3, L2: they were lost on this way).
    // The restart then seen to take it, the reads saw the change through: its mark goes, and the record with it, as
    // the rule allows.
    void aFailureSaidAtTheFirstLookIsSaidBesideTheRestartWhereTheServerThenShowsTheChange()
    {
        for (const bool byItsMonitor : {false, true}) {
            RedfishByHand bmc;
            offersSetup(bmc);
            const QString old =
                byItsMonitor ? u"/redfish/v1/TaskService/Tasks/1/Monitor"_s : u"/redfish/v1/TaskService/Tasks/1"_s;
            if (byItsMonitor) {
                bmc.bootQueuedAs = Answered(202, {}, "application/json", {"Location: " + old.toLatin1()});
                bmc.tasks.insert(old, {Answered(405, "{\"error\": {\"message\": \"Another time.\"}}")});
            } else {
                bmc.bootQueuedAs = Answered(202, "{\"@odata.id\": \"/redfish/v1/TaskService/Tasks/1\"}");
                bmc.tasks.insert(old, {Answered(200, "{\"TaskState\": \"Exception\", \"Messages\": [{\"Message\": "
                                                     "\"Another time.\"}]}")});
            }
            bmc.whenAsked = [&bmc, &old](const Asked& asked) {
                if (asked.method == "GET" && asked.path == old.toLatin1()) {
                    bmc.bootEnabled = u"Once"_s;
                    bmc.bootTarget = u"BiosSetup"_s;
                }
            };
            const RedfishDevice::Pace pace = taskPace();
            auto device = std::make_unique<RedfishDevice>(at(bmc), bmc.password, pace);
            const base::Result<qkvm::core::Done> pressed = doneBy(*device, "bios_setup");
            QVERIFY2(pressed.ok(), saidBy(pressed).c_str());
            QCOMPARE(bmc.actions, QStringList{u"system ForceRestart"_s});
            QVERIFY(bmc.inSetup);
            QVERIFY2(says(pressed, byItsMonitor ? "Its task's monitor answered at once with an error (Another time.), "
                                                  "and qKVM cannot be sure that answer was of this change; the server "
                                                  "then showed the BIOS setup boot override set, and was restarted "
                                                  "into its setup."
                                                : "Its task said at once that it did not set the BIOS setup boot "
                                                  "override (Another time.), and qKVM cannot be sure that task was "
                                                  "this change's; the server then showed the BIOS setup boot override "
                                                  "set, and was restarted into its setup."),
                     saidBy(pressed).c_str());
            QVERIFY2(!says(pressed, "queued"), saidBy(pressed).c_str());
            QVERIFY(pace.overrides->all().isEmpty());
            device.reset();
            pace.goodbyes->finish(pace.signOutWithinMs);
        }
    }

    // An override set before the press ("Once", "BiosSetup": by hand in the BMC, or left by an earlier press), and a
    // followed task that then fails, or whose monitor then answers an error: that says nothing of an override that
    // stood before the change. Read once more: still shown, it stands as it stood, and the server is restarted into
    // its setup, as 0.1.2 restarted it at its first read and as was asked; the controller's words are said beside the
    // restart, and the mark stays, the queue said, as no read can show this change made (outside review of the fuzz
    // work, round 3, L6 a). Where that read shows it gone (cleared meanwhile), nothing is restarted, as before.
    void anOverrideSetBeforeThePressStandsThroughItsTasksFailure()
    {
        using Left = qkvm::core::SetupOverrides::Left;
        enum class Way { anException, aMonitorsError, clearedMeanwhile };
        for (const Way way : {Way::anException, Way::aMonitorsError, Way::clearedMeanwhile}) {
            RedfishByHand bmc;
            offersSetup(bmc);
            bmc.bootEnabled = u"Once"_s;
            bmc.bootTarget = u"BiosSetup"_s;
            QString place;
            if (way == Way::anException) {
                place = u"/redfish/v1/TaskService/Tasks/8"_s;
                bmc.bootQueuedAs = Answered(200, "{\"@odata.id\": \"/redfish/v1/TaskService/Tasks/8\", \"TaskState\": "
                                                 "\"New\"}");
                bmc.tasks.insert(place, {Answered(200, "{\"TaskState\": \"Running\"}"),
                                         Answered(200, "{\"TaskState\": \"Exception\", \"Messages\": [{\"Message\": "
                                                       "\"BiosSetup is not allowed now.\"}]}")});
            } else {
                place = u"/redfish/v1/TaskService/Tasks/8/Monitor"_s;
                bmc.bootQueuedAs = Answered(202, {}, "application/json", {"Location: " + place.toLatin1()});
                bmc.tasks.insert(place, {Answered(202, {}), Answered(400, "{\"error\": {\"message\": \"BiosSetup is "
                                                                          "not allowed now.\"}}")});
            }
            int looks = 0;
            bmc.whenAsked = [&bmc, &place, &looks, way](const Asked& asked) {
                if (asked.method == "GET" && asked.path == place.toLatin1() && ++looks == 2 &&
                    way == Way::clearedMeanwhile) {
                    bmc.bootEnabled = u"Disabled"_s;
                }
            };
            const RedfishDevice::Pace pace = taskPace();
            auto device = std::make_unique<RedfishDevice>(at(bmc), bmc.password, pace);
            const base::Result<qkvm::core::Done> pressed = doneBy(*device, "bios_setup");
            QCOMPARE(looks, 2);
            QVERIFY2(says(pressed, "queued the change"), saidBy(pressed).c_str());
            QVERIFY(pace.overrides->standing(bmc.address(), device->machine()).value_or(Left{}).queued);
            if (way == Way::clearedMeanwhile) {
                QCOMPARE(codeOf(pressed), std::string("action_failed"));
                QVERIFY2(says(pressed, "Nothing was restarted: the controller answered for its task with an error "
                                       "(BiosSetup is not allowed now.)"),
                         saidBy(pressed).c_str());
                QVERIFY(bmc.actions.isEmpty());
            } else {
                QVERIFY2(pressed.ok(), saidBy(pressed).c_str());
                QCOMPARE(bmc.actions, QStringList{u"system ForceRestart"_s});
                QVERIFY(bmc.inSetup);
                QVERIFY2(says(pressed, way == Way::anException
                                           ? "The controller's task did not set the BIOS setup boot override "
                                             "(BiosSetup is not allowed now.), but the server showed it set from "
                                             "before the press, and was restarted into its setup."
                                           : "The controller answered for its task with an error (BiosSetup is not "
                                             "allowed now.), but the server showed the BIOS setup boot override set "
                                             "from before the press, and was restarted into its setup."),
                         saidBy(pressed).c_str());
            }
            QCOMPARE(bmc.bootEnabled, u"Disabled"_s);
            device.reset();
            pace.goodbyes->finish(pace.signOutWithinMs);
        }
    }

    // A task's place that refuses this session (401) while the computer takes it (a monitor bound to the session that
    // made it, or one of other rights) is the place refused, not the session (outside review of D1, M3): nothing is
    // signed in to again, the controller's sign-ins are not held, and the press goes on as one whose task cannot be
    // followed, its read-back and its restart under the same session. At the first look, and late: after the task was
    // seen under way, as the controller makes the change.
    void aTaskPlaceThatRefusesTheSessionLeavesItAndThePressGoesOn()
    {
        enum class Way { atTheFirstLook, late };
        for (const Way way : {Way::atTheFirstLook, Way::late}) {
            RedfishByHand bmc;
            offersSetup(bmc);
            const QString monitor = u"/redfish/v1/TaskService/Tasks/7/Monitor"_s;
            bmc.bootQueuedAs = Answered(202, {}, "application/json", {"Location: " + monitor.toLatin1()});
            bmc.tasks.insert(monitor, {Answered(202, {})});
            // (It refuses the session at its monitor from its first look, or its third, and makes the change then.)
            bmc.whenAsked = [&bmc, &monitor, way](const Asked& asked) {
                if (asked.method == "GET" && asked.path == monitor.toLatin1() &&
                    (way == Way::atTheFirstLook || bmc.asked(u"GET "_s + monitor) == 2)) {
                    bmc.unauthorizedAt = monitor;
                    bmc.bootEnabled = u"Once"_s;
                    bmc.bootTarget = u"BiosSetup"_s;
                }
            };
            const RedfishDevice::Pace pace = taskPace();
            auto device = std::make_unique<RedfishDevice>(at(bmc), bmc.password, pace);
            const base::Result<qkvm::core::Done> pressed = doneBy(*device, "bios_setup");
            QVERIFY2(pressed.ok(), saidBy(pressed).c_str());
            QCOMPARE(bmc.actions, QStringList{u"system ForceRestart"_s});
            QVERIFY(bmc.inSetup);
            QCOMPARE(bmc.asked(u"GET "_s + monitor), way == Way::atTheFirstLook ? 0 : 2);
            QVERIFY2(bmc.logins == 1, qPrintable(u"signed in %1 times"_s.arg(bmc.logins)));
            QCOMPARE(bmc.signedOut, 0);
            QVERIFY2(pace.overrides->all().isEmpty(), "a press that ended as it should left its record");
            device.reset();
            pace.goodbyes->finish(pace.signOutWithinMs);
        }
    }

    // A task that cannot be followed to its end is a queued change as 0.1.2 has it (told, its record marked, no look
    // lets it go): one not ended after so many looks, or by its time, or never read (a 404); and one named anywhere but
    // below the controller's TaskService, or climbing out of it, is not looked at at all (D1); nor one whose answer
    // names two tasks that nothing ties, a Task in its body and another's monitor in its Location (outside review of
    // D1, round 2, L4).
    void aTaskThatCannotBeFollowedToItsEndIsQueuedAsBefore()
    {
        using Left = qkvm::core::SetupOverrides::Left;
        enum class Way { tooManyLooks, tooLong, neverRead, outsideItsService, climbingOut, twoTasksNamed };
        for (const Way way : {Way::tooManyLooks, Way::tooLong, Way::neverRead, Way::outsideItsService, Way::climbingOut,
                              Way::twoTasksNamed}) {
            RedfishByHand bmc;
            offersSetup(bmc);
            QString place = u"/redfish/v1/TaskService/Tasks/7"_s;
            if (way == Way::outsideItsService) {
                place = u"/redfish/v1/Managers/Self/Tasks/7"_s;
            } else if (way == Way::climbingOut) {
                place = u"/redfish/v1/TaskService/Tasks/%2E%2E/%2E%2E/Systems/Self"_s;
            } else if (way == Way::twoTasksNamed) {
                place = u"/redfish/v1/TaskService/Tasks/7/Monitor"_s;
            }
            bmc.bootQueuedAs = Answered(202, {}, "application/json", {"Location: " + place.toLatin1()});
            if (way == Way::twoTasksNamed) {
                bmc.bootQueuedAs->body = "{\"@odata.id\": \"/redfish/v1/TaskService/Tasks/1\", \"TaskState\": \"New\"}";
                bmc.tasks.insert(
                    u"/redfish/v1/TaskService/Tasks/1"_s,
                    {Answered(200, "{\"TaskState\": \"Running\"}"), Answered(200, "{\"TaskState\": \"Completed\"}")});
            }
            if (way != Way::neverRead) {
                bmc.tasks.insert(place, {Answered(200, "{\"TaskState\": \"Running\"}")});
            }
            RedfishDevice::Pace pace = taskPace();
            pace.mostTaskLooks = way == Way::tooLong ? 1000 : 3;
            pace.taskWithinMs = way == Way::tooLong ? 900 : 120'000;
            auto device = std::make_unique<RedfishDevice>(at(bmc), bmc.password, pace);
            QElapsedTimer took;
            took.start();
            const base::Result<qkvm::core::Done> queued = doneBy(*device, "bios_setup");
            QVERIFY2(says(queued, "queued the change"), saidBy(queued).c_str());
            QVERIFY(pace.overrides->standing(bmc.address(), device->machine()).value_or(Left{}).queued);
            const int looks = bmc.asked(u"GET "_s + place);
            switch (way) {
            case Way::tooManyLooks:
                QCOMPARE(looks, 3);
                break;
            case Way::tooLong:
                // (Every 100 ms for 900: some eight looks, two on a loaded PC whose reads and looks take long, and
                // never the thousand it might have: outside review of D1, L5. 450 ms gave one look in the build box's
                // Debug run beside other programs: round 3 of the fuzz work's review.)
                QVERIFY2(looks >= 2 && looks <= 14, qPrintable(QString::number(looks)));
                break;
            case Way::neverRead:
                QCOMPARE(looks, 1);
                break;
            case Way::outsideItsService:
            case Way::climbingOut:
            case Way::twoTasksNamed:
                QCOMPARE(looks, 0);
                QVERIFY2(!bmc.requests.join(u' ').contains(u"Tasks/"_s), qPrintable(bmc.requests.join(u", "_s)));
                break;
            }
            device.reset();
            pace.goodbyes->finish(pace.signOutWithinMs);
        }
    }

    // The readers of a task, by the web's rules (redfish.py `_task_done`): where it is followed, and what a look says.
    void theTaskOfAQueuedChangeIsReadAsTheWebReadsIt()
    {
        namespace redfish = qkvm::core::redfish;
        using State = redfish::TaskSaid::State;
        const QByteArray task = "{\"@odata.id\": \"/redfish/v1/TaskService/Tasks/3\"}";
        // A Task before a monitor; a Location by its path alone; nothing outside the TaskService, or that is no place.
        QCOMPARE(redfish::taskPlace("/redfish/v1/TaskService/Tasks/3/Monitor", task),
                 u"/redfish/v1/TaskService/Tasks/3"_s);
        QCOMPARE(redfish::taskPlace("/redfish/v1/TaskService/Tasks/3",
                                    "{\"@odata.id\": \"/redfish/v1/TaskService/Tasks/3/Monitor\"}"),
                 u"/redfish/v1/TaskService/Tasks/3"_s);
        QCOMPARE(redfish::taskPlace("https://bmc.example/redfish/v1/TaskService/Tasks/4/Monitor", {}),
                 u"/redfish/v1/TaskService/Tasks/4/Monitor"_s);
        QCOMPARE(redfish::taskPlace({}, "{\"@odata.id\": \"/redfish/v1/TaskService/TaskMonitors/5\"}"),
                 u"/redfish/v1/TaskService/TaskMonitors/5"_s);
        QCOMPARE(redfish::taskPlace("/redfish/v1/Systems/Self", "{\"@odata.id\": \"/redfish/v1/Systems/Self\"}"),
                 QString());
        QCOMPARE(redfish::taskPlace("/redfish/v1/TaskService/Tasks/%2E%2E/%2E%2E/Systems/Self", {}), QString());
        QCOMPARE(redfish::taskPlace("/redfish/v1/TaskService/Tasks/5\r\nX: y", {}), QString());
        QCOMPARE(redfish::taskPlace({}, "not a record"), QString());
        // A Task in the body and a Location are followed only where they name one task: the Task's own place, its
        // monitor below it, or the monitor it names (TaskMonitor); else nothing ties them, and neither is followed
        // (outside review of D1, round 2, L4).
        QCOMPARE(redfish::taskPlace("/redfish/v1/TaskService/Tasks/3/", task), u"/redfish/v1/TaskService/Tasks/3"_s);
        QCOMPARE(redfish::taskPlace("/redfish/v1/TaskService/Tasks/7/Monitor", task), QString());
        QCOMPARE(redfish::taskPlace("/redfish/v1/TaskService/Tasks/7", task), QString());
        QCOMPARE(redfish::taskPlace("/redfish/v1/TaskService/TaskMonitors/22",
                                    "{\"@odata.id\": \"/redfish/v1/TaskService/Tasks/22\", \"TaskMonitor\": "
                                    "\"/redfish/v1/TaskService/TaskMonitors/22\"}"),
                 u"/redfish/v1/TaskService/Tasks/22"_s);
        QCOMPARE(redfish::taskPlace("/redfish/v1/TaskService/TaskMonitors/22",
                                    "{\"@odata.id\": \"/redfish/v1/TaskService/Tasks/22\"}"),
                 QString());
        QCOMPARE(redfish::taskPlace("/redfish/v1/TaskService/TaskMonitors/22",
                                    "{\"@odata.id\": \"/redfish/v1/TaskService/Tasks/22\", \"TaskMonitor\": "
                                    "\"/redfish/v1/TaskService/TaskMonitors/23\"}"),
                 QString());
        // (A body Task outside the TaskService ties nothing, and the Location alone is followed, as before.)
        QCOMPARE(redfish::taskPlace("/redfish/v1/TaskService/Tasks/4/Monitor", "{\"@odata.id\": \"/redfish/v1/Systems/"
                                                                               "Self\"}"),
                 u"/redfish/v1/TaskService/Tasks/4/Monitor"_s);
        // What a look says: at a monitor and at a Task, of a change made to `of`.
        const QString monitor = u"/redfish/v1/TaskService/Tasks/3/Monitor"_s;
        const QString itself = u"/redfish/v1/TaskService/Tasks/3"_s;
        const QString of = u"/redfish/v1/Systems/Self"_s;
        const auto look = [&of](const QString& place, int status, const QByteArray& body) {
            return redfish::taskOf(place, status, body, of);
        };
        QCOMPARE(look(monitor, 202, {}).state, State::UnderWay);
        QCOMPARE(look(monitor, 204, {}).state, State::Done);
        QCOMPARE(look(monitor, 200, {}).state, State::Done);
        QCOMPARE(look(monitor, 404, {}).state, State::Unreadable);
        QCOMPARE(look(itself, 403, {}).state, State::Unreadable);
        // A monitor's error answer: the change's own, as the web reads it (AMI's real board said 403 as its task
        // failed), in the controller's words; refused, which is no task's end for certain (outside review of D1, H1).
        const redfish::TaskSaid refused = look(monitor, 400, "{\"error\": {\"message\": \"No.\"}}");
        QCOMPARE(refused.state, State::Refused);
        QCOMPARE(refused.words, u"No."_s);
        QCOMPARE(look(monitor, 403, "{\"error\": {\"message\": \"Not allowed now.\"}}").state, State::Refused);
        // A monitor that is not to be read at all, or is gone: nothing said of the task.
        QCOMPARE(look(monitor, 401, {}).state, State::Unreadable);
        QCOMPARE(look(monitor, 410, {}).state, State::Unreadable);
        // A passing error, at a monitor or a Task: the place could not answer just then, which says nothing of the
        // task; looked at again (outside review of D1, H1).
        for (const int passing : {500, 502, 503, 504, 408, 429}) {
            QCOMPARE(look(monitor, passing, "{\"error\": {\"message\": \"Busy.\"}}").state, State::Passing);
            QCOMPARE(look(itself, passing, {}).state, State::Passing);
            QVERIFY(look(monitor, passing, "{\"error\": {\"message\": \"Busy.\"}}").words.isEmpty());
        }
        // A Task's own place answers with the Task: a 204 or an empty 200 there says nothing of it.
        QCOMPARE(look(itself, 204, {}).state, State::Unreadable);
        QCOMPARE(look(itself, 200, {}).state, State::Unreadable);
        // Neither does a status no look at a task answers with.
        QCOMPARE(look(monitor, 201, "{\"TaskState\": \"Completed\"}").state, State::Unreadable);
        QCOMPARE(look(monitor, 302, {}).state, State::Unreadable);
        QCOMPARE(look(itself, 200, "{\"TaskState\": \"Running\"}").state, State::UnderWay);
        QCOMPARE(look(itself, 200, "{\"TaskState\": \"New\"}").state, State::UnderWay);
        for (const char* running :
             {"Starting", "Suspended", "Interrupted", "Pending", "Stopping", "Service", "Cancelling"}) {
            QCOMPARE(look(itself, 200, QByteArray("{\"TaskState\": \"") + running + "\"}").state, State::UnderWay);
        }
        QCOMPARE(look(itself, 200, "{\"TaskState\": \"Completed\", \"TaskStatus\": \"OK\"}").state, State::Done);
        QCOMPARE(look(itself, 200, "{\"TaskState\": \"COMPLETED\"}").state, State::Done);
        QCOMPARE(look(itself, 200, "{\"TaskState\": \"Completed\", \"TaskStatus\": \"Critical\"}").state,
                 State::Failed);
        for (const char* ended : {"Exception", "Killed", "Cancelled"}) {
            const redfish::TaskSaid said =
                look(itself, 200,
                     QByteArray("{\"TaskState\": \"") + ended + "\", \"Messages\": [{\"Message\": \"Not\\nnow.\"}]}");
            QCOMPARE(said.state, State::Failed);
            QCOMPARE(said.words, u"Not now."_s);
        }
        QCOMPARE(look(itself, 200, "[1]").state, State::Unreadable);
        QCOMPARE(look(itself, 200, "no record").state, State::Unreadable);
        // What is unclear says nothing: a state no Task has, none at all (at a monitor, the change's own answer at its
        // end: no word of how it went), a Task that names itself another, a Task of a change to another place or of
        // another kind (outside review of D1, H1).
        QCOMPARE(look(itself, 200, "{\"TaskState\": \"Done\"}").state, State::Unreadable);
        QCOMPARE(look(itself, 200, "{\"TaskState\": 3}").state, State::Unreadable);
        QCOMPARE(look(itself, 200, "{}").state, State::Unreadable);
        QCOMPARE(look(monitor, 200, "{\"Boot\": {\"BootSourceOverrideEnabled\": \"Once\"}}").state, State::Unreadable);
        QCOMPARE(look(itself, 200, "{\"@odata.id\": \"/redfish/v1/TaskService/Tasks/1\", \"TaskState\": \"Completed\"}")
                     .state,
                 State::Unreadable);
        QCOMPARE(look(itself, 200, "{\"@odata.id\": 3, \"TaskState\": \"Completed\"}").state, State::Unreadable);
        QCOMPARE(
            look(itself, 200, "{\"@odata.id\": \"/redfish/v1/TaskService/Tasks/3/\", \"TaskState\": \"Completed\"}")
                .state,
            State::Done);
        QCOMPARE(look(itself, 200,
                      "{\"TaskState\": \"Completed\", \"Payload\": {\"TargetUri\": \"/redfish/v1/Managers/Self\"}}")
                     .state,
                 State::Unreadable);
        QCOMPARE(look(itself, 200,
                      "{\"TaskState\": \"Exception\", \"Payload\": {\"TargetUri\": \"/redfish/v1/Systems/Other\"}}")
                     .state,
                 State::Unreadable);
        QCOMPARE(look(itself, 200, "{\"TaskState\": \"Completed\", \"Payload\": {\"TargetUri\": 7}}").state,
                 State::Unreadable);
        QCOMPARE(look(itself, 200,
                      "{\"TaskState\": \"Completed\", \"Payload\": {\"TargetUri\": \"/redfish/v1/Systems/Self\", "
                      "\"HttpOperation\": \"POST\"}}")
                     .state,
                 State::Unreadable);
        QCOMPARE(look(itself, 200,
                      "{\"TaskState\": \"Completed\", \"Payload\": {\"TargetUri\": "
                      "\"https://bmc.example/redfish/v1/Systems/Self/\", \"HttpOperation\": \"patch\"}}")
                     .state,
                 State::Done);
        QCOMPARE(look(monitor, 200,
                      "{\"TaskState\": \"Completed\", \"Payload\": {\"TargetUri\": \"/redfish/v1/Systems/Self\"}}")
                     .state,
                 State::Done);
        // A Task at a monitor that names itself: the monitor's own Task (its place above the monitor, or one that
        // names the monitor as its TaskMonitor), else another's (outside review of D1, round 2, L4).
        QCOMPARE(
            look(monitor, 200, "{\"@odata.id\": \"/redfish/v1/TaskService/Tasks/3\", \"TaskState\": \"Completed\"}")
                .state,
            State::Done);
        QCOMPARE(
            look(monitor, 200, "{\"@odata.id\": \"/redfish/v1/TaskService/Tasks/1\", \"TaskState\": \"Completed\"}")
                .state,
            State::Unreadable);
        QCOMPARE(redfish::taskOf(u"/redfish/v1/TaskService/TaskMonitors/22"_s, 200,
                                 "{\"@odata.id\": \"/redfish/v1/TaskService/Tasks/22\", \"TaskMonitor\": "
                                 "\"/redfish/v1/TaskService/TaskMonitors/22\", \"TaskState\": \"Completed\"}",
                                 of)
                     .state,
                 State::Done);
        QCOMPARE(redfish::taskOf(u"/redfish/v1/TaskService/TaskMonitors/22"_s, 200,
                                 "{\"@odata.id\": \"/redfish/v1/TaskService/Tasks/22\", \"TaskState\": \"Completed\"}",
                                 of)
                     .state,
                 State::Unreadable);
        // (Words only where the controller said why it did not.)
        QVERIFY(look(itself, 200, "{\"TaskState\": \"Completed\", \"Messages\": [{\"Message\": \"Done.\"}]}")
                    .words.isEmpty());
    }

    // A record of a server on no card any more goes once its doubt is over, and one a clock that ran ahead noted (it
    // holds nothing); never while its doubt lasts, nor one of a server on a card (found by its mark at another address
    // too), nor because a card of another kind stands at its address, nor one marked queued, whatever the list says
    // (outside review of the prune, round 2, M1). The file is written anew. Before 0.2.0 such a record stayed in its
    // file for good.
    void aRecordOfAServerOnNoCardGoesOnceItsDoubtIsOver()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"left"_s);
        const QDateTime at(QDate(2026, 11, 17), QTime(12, 0), QTimeZone(QTimeZone::UTC));
        const auto address = [](int port) { return QUrl(u"https://127.0.0.1:%1"_s.arg(port)); };
        const QString mark = u"server:0123456789abcdef0123456789abcdef"_s;
        {
            qkvm::core::SetupOverrides before(file, [&at] { return at.toUTC(); });
            const QDateTime over = at.addSecs(-60);
            static_cast<void>(before.mayBeSet(address(1), QString(), over)); // on a card
            static_cast<void>(before.mayBeSet(address(2), mark, over)); // on a card by its mark, at another address
            static_cast<void>(before.mayBeSet(address(3), QString(), over));           // on no card
            static_cast<void>(before.mayBeSet(address(4), QString(), at.addSecs(60))); // ...its doubt lasting
            static_cast<void>(before.mayBeSet(address(5), QString(), over));           // ...queued
            static_cast<void>(before.markQueued(address(5), QString()));
            static_cast<void>(before.mayBeSet(address(6), QString(), at.addDays(1))); // ...noted by a clock ahead
        }
        qint64 steady = 0;
        qkvm::core::SetupOverrides kept(file, [&at] { return at.toUTC(); }, [&steady] { return steady; });
        QCOMPARE(kept.all().size(), 6);
        const auto card = [](const char* kind, int port, const QString& machine) {
            qkvm::core::OwnKvm kvm;
            kvm.id = u"0000000%1"_s.arg(port);
            kvm.kind = QString::fromLatin1(kind);
            kvm.address = u"https://127.0.0.1:%1"_s.arg(port);
            kvm.machine = machine;
            return kvm;
        };
        // (The wall's list as its settings hold it, read whole.)
        const QString settingsFile = folder.filePath(u"settings.json"_s);
        {
            qkvm::core::Remembered wall;
            wall.kvms = {card("redfish", 1, QString()), card("redfish", 7, mark), card("kvmd", 3, QString())};
            qkvm::core::Settings writer(settingsFile);
            QVERIFY(writer.remember(wall).ok());
        }
        const qkvm::core::Settings kvms(settingsFile);
        QCOMPARE(kvms.remembered().kvms.size(), 3);
        QVERIFY(kvms.listReadWhole());
        QVERIFY(kept.prune(kvms));
        const auto ports = [](const QList<qkvm::core::SetupOverrides::Left>& all) {
            QList<int> found;
            for (const qkvm::core::SetupOverrides::Left& left : all) {
                found.append(QUrl(left.address).port());
            }
            std::ranges::sort(found);
            return found;
        };
        QCOMPARE(ports(kept.all()), (QList<int>{1, 2, 4, 5}));
        QCOMPARE(ports(qkvm::core::SetupOverrides(file, [&at] { return at.toUTC(); }).all()), (QList<int>{1, 2, 4, 5}));
        // Nothing more goes while its doubt lasts; once it is over, it goes. The queued one never does.
        QVERIFY(!kept.prune(kvms));
        steady += 59'000;
        QVERIFY(!kept.prune(kvms));
        steady += 2000;
        QVERIFY(kept.prune(kvms));
        QCOMPARE(ports(kept.all()), (QList<int>{1, 2, 5}));
        QCOMPARE(ports(qkvm::core::SetupOverrides(file, [&at] { return at.toUTC(); }).all()), (QList<int>{1, 2, 5}));
        QVERIFY(kept.standing(address(5), QString()).value_or(qkvm::core::SetupOverrides::Left{}).queued);
    }

    // A start prunes by the wall's list only where its settings read it whole (outside review of the prune, M2): not
    // where they could not be read (held by another program for longer than they are tried, damaged), nor where their
    // list is no list, or holds a KVM this build does not read. The record stays, its queued mark too, and once the
    // settings are read again, it is told for its card as before. A list really read without its card lets an unmarked
    // record go; one marked queued stays all the same (outside review of the prune, round 2, M1).
    void aStartPrunesByTheWallsListOnlyWhereItsSettingsReadItWhole()
    {
        using Left = qkvm::core::SetupOverrides::Left;
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        const auto write = [&file](const QByteArray& content) {
            QFile written(file);
            return written.open(QIODevice::WriteOnly) && written.write(content) == content.size();
        };
        const QUrl address(u"https://127.0.0.1:8443"_s);
        qkvm::core::Remembered wall;
        qkvm::core::OwnKvm card;
        card.id = u"0123abcd"_s;
        card.kind = u"redfish"_s;
        card.address = address.toString();
        card.name = u"rack 7"_s;
        wall.kvms.append(card);
        {
            qkvm::core::Settings writer(file);
            QVERIFY(writer.remember(wall).ok());
        }
        QFile kept(file);
        QVERIFY(kept.open(QIODevice::ReadOnly));
        const QByteArray whole = kept.readAll();
        kept.close();
        qkvm::core::SetupOverrides records;
        static_cast<void>(records.mayBeSet(address, QString(), QDateTime::currentDateTimeUtc().addSecs(-60)));
        static_cast<void>(records.markQueued(address, QString()));
        // (And one with no mark, its doubt over, that a list read whole without its card would let go: the queued one
        // stays by the prune's own rule whatever list it is given, so only this one shows that such a list is not used.
        // Nothing is put aside here either, which a start would also have kept it for.)
        const QUrl unmarkedToo(u"https://127.0.0.1:7443"_s);
        static_cast<void>(records.mayBeSet(unmarkedToo, QString(), QDateTime::currentDateTimeUtc().addSecs(-60)));
        for (const QByteArray& notWhole :
             {QByteArray("{\"kvms\": [{\"id\": "), QByteArray("{\"kvms\": 5}"),
              QByteArray("{\"kvms\": [{\"id\": \"89abcdef\", \"kind\": \"of-a-later-version\", \"address\": "
                         "\"https://127.0.0.1:1\"}]}")}) {
            QVERIFY(write(notWhole));
            const qkvm::core::Settings started(file);
            QVERIFY(started.remembered().kvms.isEmpty());
            QVERIFY(!started.keepsAsideWhatCouldNotBeRead());
            QVERIFY2(!records.prune(started), notWhole.constData());
            QVERIFY2(records.standing(address, QString()).value_or(Left{}).queued, notWhole.constData());
            QVERIFY2(records.standing(unmarkedToo, QString()).has_value(), notWhole.constData());
        }
        records.forget(unmarkedToo, QString());
        // Read again: told for its card, as a notice at the start.
        QVERIFY(write(whole));
        const qkvm::core::Settings again(file);
        QVERIFY(!records.prune(again));
        const QStringList told = records.toldAtStart(again.remembered().kvms);
        QCOMPARE(told.size(), 1);
        QVERIFY2(told.constFirst().startsWith(u"rack 7: the controller queued a change"_s),
                 qPrintable(told.constFirst()));
        // A list really read, without its card: an unmarked record goes, the queued one stays.
        const QUrl unmarked(u"https://127.0.0.1:9443"_s);
        static_cast<void>(records.mayBeSet(unmarked, QString(), QDateTime::currentDateTimeUtc().addSecs(-60)));
        QVERIFY(write("{\"kvms\": []}"));
        QVERIFY(records.prune(qkvm::core::Settings(file)));
        QVERIFY(!records.standing(unmarked, QString()).has_value());
        QVERIFY2(records.standing(address, QString()).value_or(Left{}).queued,
                 "a list read whole without its card pruned a record marked queued");
        QCOMPARE(records.all().size(), 1);
    }

    // The start after one whose settings could not be read (outside review of the prune, round 2, M1): that start put
    // the file aside and began as new, and its run wrote settings of no cards (the update check's date, a window
    // moved); the next start reads that list whole, with none of the old cards. A record marked queued stays all the
    // same, and so, while the kept file stands beside the settings, does one with no mark, its doubt over (outside
    // review of the fuzz work, round 3, M2): once the settings with their cards are back (the kept file put back, as
    // the notice says), each card and the start tell them again. Only once the kept file is gone does the prune go by
    // the list as it stands.
    void aRecordMarkedQueuedOutlivesTheStartAfterSettingsThatCouldNotBeRead()
    {
        using Left = qkvm::core::SetupOverrides::Left;
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        const QString kept = folder.filePath(u"settings.json.setup-overrides"_s);
        const auto write = [&file](const QByteArray& content) {
            QFile written(file);
            return written.open(QIODevice::WriteOnly) && written.write(content) == content.size();
        };
        const QUrl queued(u"https://127.0.0.1:8443"_s);
        const QUrl unmarked(u"https://127.0.0.1:9443"_s);
        qkvm::core::Remembered wall;
        for (const QUrl& address : {queued, unmarked}) {
            qkvm::core::OwnKvm card;
            card.id = address.port() == 8443 ? u"0123abcd"_s : u"4567cdef"_s;
            card.kind = u"redfish"_s;
            card.address = address.toString();
            card.name = address.port() == 8443 ? u"rack 7"_s : u"rack 8"_s;
            wall.kvms.append(card);
        }
        {
            qkvm::core::Settings writer(file);
            QVERIFY(writer.remember(wall).ok());
            qkvm::core::SetupOverrides records(kept);
            const QDateTime over = QDateTime::currentDateTimeUtc().addSecs(-60);
            static_cast<void>(records.mayBeSet(queued, QString(), over));
            QVERIFY(records.markQueued(queued, QString()));
            static_cast<void>(records.mayBeSet(unmarked, QString(), over));
        }
        QFile whole(file);
        QVERIFY(whole.open(QIODevice::ReadOnly));
        const QByteArray withTheCards = whole.readAll();
        whole.close();
        // A start whose settings cannot be read: put aside, begun as new, no prune; and its run writes its settings.
        QVERIFY(write("{\"kvms\": [{\"id\": "));
        {
            qkvm::core::Settings started(file);
            QVERIFY(started.wasUnreadable());
            QVERIFY(started.putAsideWhatCouldNotBeRead().ok());
            QVERIFY(!started.putAside().isEmpty());
            qkvm::core::SetupOverrides records(kept);
            QVERIFY(!records.prune(started));
            QVERIFY(started.remember(qkvm::core::Remembered{}).ok());
        }
        // The next start: a list read whole, of no cards, and the settings put aside still beside it. Nothing goes,
        // the record with no mark neither: its card's "may be set" comes back with the kept file (outside review of
        // the fuzz work, round 3, M2).
        {
            const qkvm::core::Settings started(file);
            QVERIFY(!started.wasUnreadable());
            QVERIFY(started.listReadWhole());
            QVERIFY(started.remembered().kvms.isEmpty());
            qkvm::core::SetupOverrides records(kept);
            QVERIFY(!records.prune(started));
            QVERIFY2(records.standing(unmarked, QString()).has_value(),
                     "the start after settings that could not be read pruned a record its card says may be set");
            QVERIFY2(records.standing(queued, QString()).value_or(Left{}).queued,
                     "the start after settings that could not be read pruned a record marked queued");
        }
        // The settings with the cards back: told again, on each card and at the start.
        QVERIFY(write(withTheCards));
        const qkvm::core::Settings back(file);
        QCOMPARE(back.remembered().kvms.size(), 2);
        qkvm::core::SetupOverrides records(kept);
        QVERIFY(!records.prune(back));
        const QStringList told = records.toldAtStart(back.remembered().kvms);
        QCOMPARE(told.size(), 2);
        QVERIFY2(told.constFirst().startsWith(u"rack 7: the controller queued a change"_s),
                 qPrintable(told.constFirst()));
        QVERIFY2(told.constLast().startsWith(u"rack 8: its BIOS setup boot override may still be set"_s),
                 qPrintable(told.constLast()));
        // Once the kept file is gone (whoever looks after the PC took it away), a list read whole is that wall's own:
        // without the cards, the record with no mark goes, the queued one stays.
        QVERIFY(QFile::remove(folder.filePath(u"settings.unreadable.json"_s)));
        QVERIFY(write("{\"kvms\": []}"));
        QVERIFY(records.prune(qkvm::core::Settings(file)));
        QVERIFY(!records.standing(unmarked, QString()).has_value());
        QVERIFY(records.standing(queued, QString()).value_or(Left{}).queued);
    }

    // The queued mark is kept in its file the moment it is made: read in a later run, the record is marked still, no
    // look lets it go however late, it holds no update once its doubt is over, and it is told so at the start and on
    // its card. It goes only where it is forgotten (a start into BIOS setup that ended as it should, or the server
    // taken off the wall). A record a version before wrote, with no mark, is no queued one; and one kept in memory
    // before its file is given keeps its mark there (D1).
    void aQueuedMarkIsKeptInItsFileAndOnlyForgettingLetsItGo()
    {
        using Left = qkvm::core::SetupOverrides::Left;
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"left"_s);
        QDateTime at(QDate(2026, 11, 15), QTime(12, 0), QTimeZone(QTimeZone::UTC));
        const QUrl here(u"https://127.0.0.1:8443"_s);
        const QUrl there(u"https://127.0.0.1:9443"_s);
        {
            qkvm::core::SetupOverrides before(file, [&at] { return at.toUTC(); });
            static_cast<void>(before.mayBeSet(here, QString(), at.addSecs(60)));
            static_cast<void>(before.mayBeSet(there, QString(), at.addSecs(60)));
            before.markQueued(here, QString());
            QCOMPARE(before.written(), std::optional<bool>(true));
            // (Written at the mark: the file says it before anything else changes.)
            QVERIFY(qkvm::core::SetupOverrides(file).standing(here, QString()).value_or(Left{}).queued);
        }
        qint64 steady = 0;
        qkvm::core::SetupOverrides later(file, [&at] { return at.toUTC(); }, [&steady] { return steady; });
        QVERIFY(later.standing(here, QString()).value_or(Left{}).queued);
        QVERIFY(!later.standing(there, QString()).value_or(Left{}).queued);
        steady += 61'000;
        QVERIFY(!later.changeMayStillCome());
        at = at.addDays(30);
        steady += qint64{30} * 24 * 3600 * 1000;
        QVERIFY2(!later.seenOff(here, QString(), at), "a look a month on let a queued change's record go");
        QVERIFY(!later.drop(here, QString()));
        QVERIFY(later.seenOff(there, QString(), at));
        QVERIFY(!later.changeMayStillCome());
        qkvm::core::OwnKvm card;
        card.name = u"rack 7"_s;
        card.kind = u"redfish"_s;
        card.address = here.toString();
        const QStringList told = later.toldAtStart({card});
        QCOMPARE(told.size(), 1);
        QVERIFY2(told.constFirst().startsWith(u"rack 7: the controller queued a change"_s),
                 qPrintable(told.constFirst()));
        QVERIFY(told.constFirst().contains(u"in its BMC"_s));
        QVERIFY(qkvm::core::SetupOverrides::onTheCard(true).contains(u"check the boot override in its BMC"_s));
        QVERIFY(qkvm::core::SetupOverrides::onTheCard(true) != qkvm::core::SetupOverrides::onTheCard());
        later.forget(here, QString());
        QVERIFY(later.all().isEmpty());
        QVERIFY(qkvm::core::SetupOverrides(file).all().isEmpty());
        // A record of a version before (no mark): no queued one.
        QFile older(file);
        QVERIFY(older.open(QIODevice::WriteOnly));
        older.write("{\"version\": 1, \"servers\": [{\"address\": \"https://127.0.0.1:8443\", \"machine\": \"\", "
                    "\"since\": \"2026-11-15T12:00:00.000Z\", \"doubtUntil\": \"2026-11-15T12:01:00.000Z\"}]}");
        older.close();
        QVERIFY(!qkvm::core::SetupOverrides(file).standing(here, QString()).value_or(Left{}).queued);
        // Marked in memory before its file is given: kept so there, beside what the file had.
        qkvm::core::SetupOverrides inMemory;
        static_cast<void>(inMemory.mayBeSet(here, QString(), at.addSecs(60)));
        inMemory.markQueued(here, QString());
        inMemory.keepIn(file);
        QVERIFY(qkvm::core::SetupOverrides(file).standing(here, QString()).value_or(Left{}).queued);
    }
};

QTEST_GUILESS_MAIN(TestRedfishSetupQueued)
#include "test_redfish_setup_queued.moc"
