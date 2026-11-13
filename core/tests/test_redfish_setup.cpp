#include <memory>
#include <optional>
#include <string>
#include <utility>

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
        qkvm::core::SetupOverrides kept(file, [&at] { return at.toUTC(); });
        QVERIFY(!kept.mayBeSet(here, a, at.addSecs(60)).has_value());
        // By the number of its computer wherever it is; by its address where either has none.
        QVERIFY(kept.standing(there, a).has_value());
        QVERIFY(!kept.standing(here, b).has_value());
        QVERIFY(kept.standing(here, QString()).has_value());
        QVERIFY(!kept.standing(there, QString()).has_value());
        // A second press keeps the longer doubt, and says what stood.
        QCOMPARE(kept.mayBeSet(here, a, at.addSecs(30)), std::optional<QDateTime>(at.addSecs(60)));
        QCOMPARE(kept.standing(here, a).value_or(qkvm::core::SetupOverrides::Left{}).doubtUntil, at.addSecs(60));
        // Seen off before its doubt is over: it stands. After it: it goes.
        QVERIFY(!kept.seenOff(here, a, at.addSecs(59)));
        QVERIFY(kept.standing(here, a).has_value());
        kept.settle(here, a, at.addSecs(10));
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
        const RedfishDevice::Pace pace = setupPace();
        auto device = std::make_unique<RedfishDevice>(at(bmc), bmc.password, pace);
        // (Held apart from this case: the press may be told after it ends.)
        const auto answer = std::make_shared<std::optional<base::Result<qkvm::core::Done>>>();
        device->act(qkvm::core::Action::plain(u"bios_setup"_s),
                    [answer](const base::Result<qkvm::core::Done>& came) { *answer = came; });
        QTRY_COMPARE_WITH_TIMEOUT(bootChanges(bmc), QStringList{u"Once BiosSetup"_s}, 10000);
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
