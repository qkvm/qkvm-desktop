#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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
#include "stand_in.h"

using namespace Qt::StringLiterals;
namespace base = qkvm::base;
using namespace qkvm::core::testing;
using qkvm::core::RedfishDevice;

namespace {

base::Result<qkvm::core::Done> doneBy(RedfishDevice& device, const char* kind)
{
    QList<base::Result<qkvm::core::Done>> answers;
    device.act(qkvm::core::Action::plain(QString::fromLatin1(kind)),
               [&](const base::Result<qkvm::core::Done>& answer) { answers.append(answer); });
    if (!QTest::qWaitFor([&] { return !answers.isEmpty(); }, 30000)) {
        return base::Error{"never", "The call was never answered."};
    }
    return answers.takeFirst();
}

std::string codeOf(const base::Result<qkvm::core::Done>& what)
{
    return what ? std::string("ok") : what.error().code;
}

std::string saidBy(const base::Result<qkvm::core::Done>& what)
{
    return what ? what.value().note.toStdString() : what.error().message;
}

bool says(const base::Result<qkvm::core::Done>& what, const char* part)
{
    return saidBy(what).find(part) != std::string::npos;
}

// The press's times made short: its restart within 1.5 s, the whole of it 3 s (four minutes in life); its clearing on
// the way out 4 s (thirty); and what may be left kept by the test, in memory.
RedfishDevice::Pace setupPace()
{
    RedfishDevice::Pace pace = quick();
    pace.setupWithinMs = 1500;
    pace.setupPatienceMs = 3000;
    pace.bootQuietMs = 1500;
    pace.confirmWithinMs = 600;
    pace.confirmEveryMs = 100;
    pace.setupClearWithinMs = 4000;
    pace.overrides = std::make_shared<qkvm::core::SetupOverrides>();
    return pace;
}

// What a server's card says of it, after a look.
QString leftOnTheCard(RedfishDevice& device)
{
    return lookedAt(device).server.value_or(qkvm::core::ServerReadings{}).setupLeft;
}

// A controller that offers a start into the BIOS setup, as AMI's boards list it.
void offersSetup(RedfishByHand& bmc)
{
    bmc.bootTargets = QJsonArray{u"None"_s, u"Pxe"_s, u"Hdd"_s, u"Cd"_s, u"BiosSetup"_s};
}

// The changes of its boot override, in the order they came: "Once BiosSetup", "Disabled".
QStringList bootChanges(const RedfishByHand& bmc)
{
    QStringList all;
    for (const QJsonObject& change : bmc.changes) {
        const QJsonObject boot = change.value(u"Boot"_s).toObject();
        if (!boot.isEmpty()) {
            all.append((boot.value(u"BootSourceOverrideEnabled"_s).toString() + u' ' +
                        boot.value(u"BootSourceOverrideTarget"_s).toString())
                           .trimmed());
        }
    }
    return all;
}

} // namespace

// Start into BIOS setup on a server of this PC's own wall (the web's `bios_setup`): the override set, the server
// restarted or switched on, given four minutes, and the override cleared again after, whatever happened, wherever it
// still shows: a server is never left booting into its setup. Against a controller by hand.
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

    void anotherToolsOverrideIsLeftAsItIs()
    {
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.bootEnabled = u"Once"_s;
        bmc.bootTarget = u"Pxe"_s;
        RedfishDevice device(at(bmc), bmc.password, setupPace());
        const base::Result<qkvm::core::Done> done = doneBy(device, "bios_setup");
        QCOMPARE(codeOf(done), std::string("action_failed"));
        QVERIFY2(says(done, "has a boot override set already (Pxe, Once)"), saidBy(done).c_str());
        QVERIFY(bootChanges(bmc).isEmpty());
        QVERIFY(bmc.actions.isEmpty());
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
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.bootTakesMs = 1300; // past the 1.5 s the restart has, with the reads before it
        RedfishDevice device(at(bmc), bmc.password, setupPace());
        const base::Result<qkvm::core::Done> done = doneBy(device, "bios_setup");
        QVERIFY2(says(done, "took more than 1 seconds before the restart"), saidBy(done).c_str());
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
        // (Read back for long enough to look at the file meanwhile.)
        pace.setupWithinMs = 8000;
        pace.setupPatienceMs = 10'000;
        pace.confirmWithinMs = 6000;
        pace.overrides = std::make_shared<qkvm::core::SetupOverrides>(file);
        auto device = std::make_unique<RedfishDevice>(at(bmc), bmc.password, pace);
        // What a run after a crash just now would read, a day on (a copy of the file as it stands), and whether a
        // look then that shows the override off leaves it standing.
        const auto standsAfterACrash = [&file, &bmc, &device] {
            const QTemporaryDir crash;
            const QString copy = crash.filePath(u"left"_s);
            if (!QFile::copy(file, copy)) {
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
        QTRY_VERIFY_WITH_TIMEOUT(standsAfterACrash(), 10000);
        QVERIFY2(!answer->has_value(), "the press ended before the queued mark was written down");
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
    // stands, marked queued (D1).
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
        device->act(qkvm::core::Action::plain(u"bios_setup"_s), [](const base::Result<qkvm::core::Done>&) {});
        QTRY_COMPARE_WITH_TIMEOUT(bmc.actions, QStringList{u"system ForceRestart"_s}, 10000);
        device.reset();
        pace.goodbyes->finish(pace.signOutWithinMs);
        QCOMPARE(bmc.bootEnabled, u"Disabled"_s);
        QCOMPARE(bootChanges(bmc), (QStringList{u"Once BiosSetup"_s, u"Disabled"_s}));
        QVERIFY2(pace.overrides->standing(bmc.address(), QString()).value_or(qkvm::core::SetupOverrides::Left{}).queued,
                 "the clearing on the way out let a queued record go");
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

    void theProgramsEndWhileTheChangeIsOutClearsItAndReadsItBackBeforeTheSessionGoes()
    {
        // The controller takes a while over the change (made as it comes, answered two seconds later), and the program
        // ends meanwhile: its driver let go of, and its goodbyes waited for as main.cpp waits for them. The override is
        // cleared again and read back, and only then is the session given back; the press is told that the override may
        // be set. The clearing was seen to work after the change was seen made: nothing is left noted.
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
        QVERIFY(!pace.overrides->standing(bmc.address(), QString()).has_value());
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
        // The card changed while the change was out (its driver let go of, a new one made, the program going on): the
        // clearing on the way out is refused, and the session is given back all the same. The driver made anew says on
        // the card that the override may be set, until a look shows it off (here: once somebody cleared it at the
        // controller's own page, and nothing of the press can still come).
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.bootTakesMs = 1000;
        bmc.clearStatus = 500;
        const RedfishDevice::Pace pace = setupPace();
        auto device = std::make_unique<RedfishDevice>(at(bmc), bmc.password, pace);
        device->act(qkvm::core::Action::plain(u"bios_setup"_s), [](const base::Result<qkvm::core::Done>&) {});
        QTRY_COMPARE_WITH_TIMEOUT(bootChanges(bmc), QStringList{u"Once BiosSetup"_s}, 10000);
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
        // override off; then nothing of it is kept.
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json.setup-overrides"_s);
        RedfishByHand bmc;
        offersSetup(bmc);
        bmc.bootTakesMs = 1000;
        bmc.clearStatus = 500;
        RedfishDevice::Pace pace = setupPace();
        pace.overrides = std::make_shared<qkvm::core::SetupOverrides>(file);
        QString machine;
        {
            auto device = std::make_unique<RedfishDevice>(at(bmc), bmc.password, pace);
            device->act(qkvm::core::Action::plain(u"bios_setup"_s), [](const base::Result<qkvm::core::Done>&) {});
            QTRY_COMPARE_WITH_TIMEOUT(bootChanges(bmc), QStringList{u"Once BiosSetup"_s}, 10000);
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
