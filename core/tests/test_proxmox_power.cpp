#include <algorithm>
#include <memory>
#include <string>
#include <utility>

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QTest>
#include <QTimer>

#include <qkvm/core/proxmox_device.h>

#include "proxmox_by_hand.h"
#include "proxmox_device_tests.h"
#include "stand_in.h"

using namespace Qt::StringLiterals;
namespace base = qkvm::base;
using namespace qkvm::core::testing;
using qkvm::core::Action;
using qkvm::core::Done;
using qkvm::core::ProxmoxDevice;
using Guest = ProxmoxByHand::Guest;

namespace {

base::Result<Done> pressed(ProxmoxDevice& device, const QString& kind)
{
    return answerOf<Done>([&device, &kind](std::function<void(base::Result<Done>)> done) {
        device.act(Action::plain(kind), std::move(done));
    });
}

QString noteOf(const base::Result<Done>& what)
{
    return what ? what.value().note : u"(%1)"_s.arg(QString::fromStdString(what.error().code));
}

std::string messageOf(const base::Result<Done>& what)
{
    return what ? std::string() : what.error().message;
}

QStringList taskKinds(const ProxmoxByHand& pve)
{
    QStringList kinds;
    for (const ProxmoxByHand::Task& task : pve.tasks) {
        kinds.append(task.type);
    }
    return kinds;
}

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

QString numberOf(const Guest& guest)
{
    return u"proxmox:"_s +
           QString::fromLatin1(
               QCryptographicHash::hash(guest.uuid.toLower().toUtf8(), QCryptographicHash::Sha256).toHex().first(32));
}

// A stand-in with the web's two guests: "web" (100) running, "db" (101) stopped.
void twoGuests(ProxmoxByHand& pve)
{
    Guest db;
    db.name = u"db"_s;
    db.status = u"stopped"_s;
    pve.guests.insert(101, db);
}

ProxmoxDevice::Pace fast()
{
    ProxmoxDevice::Pace pace = quickProxmox();
    pace.places = std::make_shared<ProxmoxDevice::Places>();
    pace.confirmEveryMs = 50;
    pace.followUpMs.clear();
    return pace;
}

} // namespace

// A guest's power buttons, as the web offers them (proxmox.py `act`, `_carried_out`, `_task`): start, ask to shut
// down, stop at once, reset; by what is wanted ("already on"); which virtual machine has the number asked right before
// (rule 30); a start or a stop waited for until it is seen, with Proxmox's task asked how it stands; and what may have
// been done said so. Ported from the web's tests (../app/tests/test_proxmox.py of web 0.3.1).
class TestProxmoxPower : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { const ProxmoxDevice first(ProxmoxDevice::Where{}, QString(), quickProxmox()); }

    // The web's test_the_power_buttons.
    void thePowerButtons()
    {
        ProxmoxByHand pve(proxmoxTls());
        twoGuests(pve);
        ProxmoxDevice device(guestOf(pve, 101), pve.secret, fast());
        QCOMPARE(noteOf(pressed(device, u"atx_reset"_s)), u"(action_failed)"_s); // nothing to reset
        QCOMPARE(noteOf(pressed(device, u"power_off"_s)), u"already off"_s);
        QCOMPARE(noteOf(pressed(device, u"power_on"_s)), QString());
        QCOMPARE(pve.guests.value(101).status, u"running"_s);
        QCOMPARE(noteOf(pressed(device, u"power_on"_s)), u"already on"_s);
        QCOMPARE(noteOf(pressed(device, u"atx_reset"_s)), QString());
        QCOMPARE(noteOf(pressed(device, u"power_off"_s)), QString()); // asked to shut down
        QCOMPARE(pve.guests.value(101).status, u"stopped"_s);
        QCOMPARE(noteOf(pressed(device, u"atx_power"_s)), QString());      // the power button: it is off, so on
        QCOMPARE(noteOf(pressed(device, u"atx_power_long"_s)), QString()); // held: stopped at once
        QCOMPARE(pve.guests.value(101).status, u"stopped"_s);
        const base::Result<Done> reboot = pressed(device, u"reboot"_s);
        QCOMPARE(noteOf(reboot), u"(invalid)"_s);
        QCOMPARE(noteOf(pressed(device, u"jiggler_on"_s)), u"(invalid)"_s);
        QCOMPARE(taskKinds(pve), (QStringList{u"qmstart"_s, u"qmreset"_s, u"qmshutdown"_s, u"qmstart"_s, u"qmstop"_s}));
        // Asked to shut down, a guest with nothing in it to hear that goes on running: said, not waited for.
        pve.guests[101].ignoresShutdown = true;
        QCOMPARE(noteOf(pressed(device, u"power_on"_s)), QString());
        QCOMPARE(noteOf(pressed(device, u"power_off"_s)), QString());
        QCOMPARE(pve.guests.value(101).status, u"running"_s);
    }

    // Review c9b-1, L6: a sign-in that comes inside a start's wait (the token taken back right after the command went,
    // and Proxmox holding its 401 to the sign-in a long while) is held to the press's one bound: the press is answered
    // by then, and not once the sign-in's own time is up.
    void aSignInInsideTheWaitIsHeldToThePressesBound()
    {
        ProxmoxByHand pve(proxmoxTls());
        twoGuests(pve);
        pve.powerTakesMs = 60'000;
        pve.afterCommand = [&pve] {
            pve.secret = ProxmoxByHand::fresh();
            pve.refuseSignInAfterMs = 15'000;
        };
        ProxmoxDevice::Pace pace = fast();
        pace.signInQuietMs = 20'000;
        pace.confirmWithinMs = 1000;
        pace.taskWithinMs = 2000;
        ProxmoxDevice device(guestOf(pve, 101), pve.secret, pace);
        QElapsedTimer clock;
        clock.start();
        const base::Result<Done> said = pressed(device, u"power_on"_s);
        QVERIFY(!said);
        QVERIFY2(clock.elapsed() < 9000, qPrintable(u"answered after %1 ms"_s.arg(clock.elapsed())));
    }

    // Outside review of 0.2.0, 3.3: a command Proxmox refuses 401 (the token taken back for a moment) is sent again
    // after a sign-in, as the web does, and only once which virtual machine has the guest's number was asked again: a
    // sign-in's time went by, in which another could be made under it. Another is sent nothing, and nothing is said to
    // have perhaps been done; the guest itself is reset, the question asked between the sign-in and the command.
    void aCommandSentAgainAfterASignInIsJudgedAgainFirst()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice::Where where = guestOf(pve);
        where.machine = numberOf(pve.guests.value(100));
        ProxmoxDevice device(where, pve.secret, fast());
        QCOMPARE(lookedAt(device).state, u"online"_s);
        const QString reset = u"POST /nodes/pve1/qemu/100/status/reset"_s;
        bool refusedOnce = false;
        pve.refuses = [&pve, &refusedOnce, &reset](const QString& said) {
            if (said != reset || std::exchange(refusedOnce, true)) {
                return false;
            }
            // (Meanwhile the guest is deleted, and another made under its number that runs as it ran.)
            pve.deleteGuest(100);
            Guest other;
            other.name = u"prod-db"_s;
            pve.makeGuest(other);
            return true;
        };
        const base::Result<Done> said = pressed(device, u"atx_reset"_s);
        QCOMPARE(noteOf(said), u"(another_machine)"_s);
        QVERIFY(!ProxmoxDevice::mayHaveBeenDone(said.error()));
        QVERIFY(pve.tasks.isEmpty());
        QCOMPARE(pve.refusedPaths, QStringList{reset});
        QCOMPARE(pve.asked(reset), 0);
        // The guest itself, the command refused once: sent again after the sign-in and the question, and done once.
        ProxmoxByHand same(proxmoxTls());
        ProxmoxDevice::Where itself = guestOf(same);
        itself.machine = numberOf(same.guests.value(100));
        ProxmoxDevice again(itself, same.secret, fast());
        QCOMPARE(lookedAt(again).state, u"online"_s);
        bool once = false;
        same.refuses = [&once, &reset](const QString& asked) { return asked == reset && !std::exchange(once, true); };
        QCOMPARE(noteOf(pressed(again, u"atx_reset"_s)), QString());
        QCOMPARE(taskKinds(same), QStringList{u"qmreset"_s});
        QCOMPARE(same.requests.mid(same.requests.size() - 3),
                 (QStringList{u"GET /version"_s, u"GET /nodes/pve1/qemu/100/config"_s, reset}));
    }

    // Outside review of 0.2.0, 3.2, on every road (review cx3-1, H1): the sign-in after a command refused 401 asks
    // first what answers, on the very line the token is to go on. Somebody else answers by then: nothing goes a second
    // time, and nothing was done.
    void aCommandRefusedIsSignedInAgainOnlyWhereAProxmoxAnswers()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice device(guestOf(pve), pve.secret, fast());
        QCOMPARE(lookedAt(device).state, u"online"_s);
        const QString reset = u"POST /nodes/pve1/qemu/100/status/reset"_s;
        bool once = false;
        pve.refuses = [&pve, &once, &reset](const QString& said) {
            if (said != reset || std::exchange(once, true)) {
                return false;
            }
            pve.unsignedStatus = 404;
            return true;
        };
        const base::Result<Done> said = pressed(device, u"atx_reset"_s);
        QCOMPARE(noteOf(said), u"(action_failed)"_s);
        QVERIFY2(messageOf(said).starts_with("Nothing was done"), messageOf(said).c_str());
        QVERIFY(!ProxmoxDevice::mayHaveBeenDone(said.error()));
        QVERIFY(pve.tasks.isEmpty());
        QCOMPARE(pve.asked(u"GET /version"_s), 1);
        QCOMPARE(pve.unsignedAsked.size(), 2);
    }

    // The web's test_a_power_command_that_is_not_carried_out_is_said_so.
    void aCommandThatIsNotCarriedOutIsSaidSo()
    {
        ProxmoxByHand pve(proxmoxTls());
        twoGuests(pve);
        pve.powerTakesMs = 60'000;
        ProxmoxDevice::Pace pace = fast();
        pace.confirmWithinMs = 1000;
        ProxmoxDevice device(guestOf(pve, 101), pve.secret, pace);
        const base::Result<Done> said = pressed(device, u"power_on"_s);
        QCOMPARE(noteOf(said), u"(action_failed)"_s);
        QVERIFY(messageOf(said).find("still stopped") != std::string::npos);
        QVERIFY(!ProxmoxDevice::mayHaveBeenDone(said.error()));
    }

    // The web's test_a_start_whose_task_takes_its_time_is_waited_for_while_the_task_runs: given its confirmation time
    // from the task's end, not from the press; the task asked of the node its name gives.
    void aStartWhoseTaskTakesItsTimeIsWaitedFor()
    {
        ProxmoxByHand pve(proxmoxTls());
        twoGuests(pve);
        pve.taskTakesMs = 3000;
        pve.powerTakesMs = 800;
        ProxmoxDevice::Pace pace = fast();
        pace.confirmWithinMs = 2500;
        pace.taskWithinMs = 15'000;
        ProxmoxDevice device(guestOf(pve, 101), pve.secret, pace);
        QElapsedTimer took;
        took.start();
        QCOMPARE(noteOf(pressed(device, u"power_on"_s)), QString());
        QVERIFY(took.elapsed() > 3000 && took.elapsed() < 15'000);
        QCOMPARE(pve.guests.value(101).status, u"running"_s);
        QVERIFY(std::ranges::any_of(
            pve.requests, [](const QString& one) { return one.startsWith(u"GET /nodes/pve1/tasks/UPID:"_s); }));
    }

    // The web's test_a_task_that_still_runs_is_said_to_be_running_and_not_to_have_failed.
    void aTaskThatStillRunsIsSaidToBeUnderWay()
    {
        ProxmoxByHand pve(proxmoxTls());
        twoGuests(pve);
        pve.taskTakesMs = 60'000;
        ProxmoxDevice::Pace pace = fast();
        pace.confirmWithinMs = 500;
        pace.taskWithinMs = 1500;
        ProxmoxDevice device(guestOf(pve, 101), pve.secret, pace);
        const base::Result<Done> said = pressed(device, u"power_on"_s);
        QCOMPARE(noteOf(said), u"(timeout)"_s);
        QVERIFY(messageOf(said).find("still running") != std::string::npos);
        QVERIFY(messageOf(said).find("not running yet") != std::string::npos);
        QVERIFY(ProxmoxDevice::mayHaveBeenDone(said.error()));
    }

    // The web's test_a_task_that_ends_in_an_error_is_said_at_once_in_proxmoxs_words.
    void aTaskThatEndsInAnErrorIsSaidAtOnce()
    {
        ProxmoxByHand pve(proxmoxTls());
        twoGuests(pve);
        pve.taskTakesMs = 300;
        pve.taskFails = u"start failed: QEMU exited with code 1"_s;
        ProxmoxDevice::Pace pace = fast();
        pace.confirmWithinMs = 8000;
        ProxmoxDevice device(guestOf(pve, 101), pve.secret, pace);
        QElapsedTimer took;
        took.start();
        const base::Result<Done> said = pressed(device, u"power_on"_s);
        QCOMPARE(noteOf(said), u"(action_failed)"_s);
        QCOMPARE(messageOf(said),
                 std::string("Proxmox could not start the guest: start failed: QEMU exited with code 1"));
        QVERIFY(took.elapsed() < 5000);
        QCOMPARE(pve.guests.value(101).status, u"stopped"_s);
    }

    // The web's test_a_task_that_ended_with_warnings_is_done.
    void aTaskThatEndedWithWarningsIsDone()
    {
        ProxmoxByHand pve(proxmoxTls());
        twoGuests(pve);
        pve.taskTakesMs = 300;
        pve.taskFails = u"WARNINGS: 1"_s;
        pve.powerTakesMs = 600;
        ProxmoxDevice::Pace pace = fast();
        pace.confirmWithinMs = 8000;
        ProxmoxDevice device(guestOf(pve, 101), pve.secret, pace);
        QCOMPARE(noteOf(pressed(device, u"power_on"_s)), QString());
        QCOMPARE(pve.guests.value(101).status, u"running"_s);
    }

    // The web's test_a_guest_under_proxmoxs_high_availability_is_waited_for_as_long_as_a_task.
    void aGuestUnderHighAvailabilityIsWaitedForAsLongAsATask()
    {
        ProxmoxByHand pve(proxmoxTls());
        twoGuests(pve);
        pve.guests[101].ha = true;
        pve.powerTakesMs = 2000;
        ProxmoxDevice::Pace pace = fast();
        pace.confirmWithinMs = 500;
        pace.taskWithinMs = 15'000;
        ProxmoxDevice device(guestOf(pve, 101), pve.secret, pace);
        QCOMPARE(noteOf(pressed(device, u"power_on"_s)), QString());
        QCOMPARE(pve.guests.value(101).status, u"running"_s);
        QCOMPARE(taskKinds(pve), QStringList{u"hastart"_s});
        // And one that HA does not carry out in the time: said to be under way, not to have failed.
        pace.taskWithinMs = 1000;
        pve.powerTakesMs = 60'000;
        ProxmoxDevice soon(guestOf(pve, 101), pve.secret, pace);
        const base::Result<Done> said = pressed(soon, u"atx_power_long"_s);
        QCOMPARE(noteOf(said), u"(timeout)"_s);
        QVERIFY(messageOf(said).find("high availability") != std::string::npos);
    }

    // The web's test_a_task_the_token_may_not_read_leaves_it_unknown_and_not_failed.
    void aTaskTheTokenMayNotReadLeavesItUnknown()
    {
        ProxmoxByHand pve(proxmoxTls());
        twoGuests(pve);
        pve.tasksHidden = true;
        pve.taskTakesMs = 60'000;
        ProxmoxDevice::Pace pace = fast();
        pace.confirmWithinMs = 500;
        ProxmoxDevice device(guestOf(pve, 101), pve.secret, pace);
        const base::Result<Done> said = pressed(device, u"power_on"_s);
        QCOMPARE(noteOf(said), u"(timeout)"_s);
        QVERIFY(messageOf(said).find("did not say how its task went") != std::string::npos);
    }

    // The web's test_a_token_without_the_right_to_switch_is_told_by_proxmoxs_words.
    void aTokenWithoutTheRightToSwitchIsToldSo()
    {
        ProxmoxByHand pve(proxmoxTls());
        pve.guests[100].mayPower = false;
        ProxmoxDevice device(guestOf(pve), pve.secret, fast());
        const base::Result<Done> said = pressed(device, u"atx_power_long"_s);
        QVERIFY(!said);
        QVERIFY(messageOf(said).find("VM.PowerMgmt") != std::string::npos);
        QCOMPARE(pve.guests.value(100).status, u"running"_s);
        QVERIFY(pve.tasks.isEmpty());
    }

    // The web's test_a_power_command_whose_answer_was_lost_may_have_been_done: handed on to another node, or through a
    // proxy, and its answer lost on the way back: it may have been done, and is said so.
    void aCommandWhoseAnswerWasLostMayHaveBeenDone_data()
    {
        QTest::addColumn<int>("lost");
        for (const int lost : {596, 597, 502, 503, 504}) {
            QTest::newRow(QByteArray::number(lost).constData()) << lost;
        }
    }
    void aCommandWhoseAnswerWasLostMayHaveBeenDone()
    {
        QFETCH(int, lost);
        ProxmoxByHand pve(proxmoxTls());
        pve.powerAnswer = lost;
        ProxmoxDevice device(guestOf(pve), pve.secret, fast());
        const base::Result<Done> said = pressed(device, u"atx_reset"_s);
        QCOMPARE(noteOf(said), u"(timeout)"_s);
        QVERIFY(messageOf(said).find("may have been done") != std::string::npos);
        QVERIFY(ProxmoxDevice::mayHaveBeenDone(said.error()));
        QCOMPARE(taskKinds(pve), QStringList{u"qmreset"_s});
    }

    // The web's test_a_power_command_proxmox_could_not_hand_on_is_not_done: 595, the node could not be connected to.
    void aCommandProxmoxCouldNotHandOnIsNotDone()
    {
        ProxmoxByHand pve(proxmoxTls());
        pve.powerAnswer = 595;
        ProxmoxDevice device(guestOf(pve), pve.secret, fast());
        const base::Result<Done> said = pressed(device, u"atx_reset"_s);
        QCOMPARE(noteOf(said), u"(unreachable)"_s);
        QVERIFY(!ProxmoxDevice::mayHaveBeenDone(said.error()));
        QVERIFY(pve.tasks.isEmpty());
    }

    // The web's test_a_power_button_is_not_pressed_on_a_guess: a state of a kind of its own decides nothing.
    void aPowerButtonIsNotPressedOnAGuess()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice device(guestOf(pve), pve.secret, fast());
        pve.hostile = u"odd_status"_s;
        for (const QString& kind : {u"atx_power"_s, u"power_on"_s, u"power_off"_s, u"atx_reset"_s}) {
            const base::Result<Done> said = pressed(device, kind);
            QCOMPARE(noteOf(said), u"(bad_response)"_s);
            QCOMPARE(messageOf(said), std::string("Proxmox did not say whether the guest runs"));
        }
        QVERIFY(commands(pve).isEmpty());
    }

    // The web's test_a_vm_given_a_deleted_guests_number_is_sent_no_power_command_..., its buttons: a VM made under the
    // number that runs as the guest ran (nothing in its state tells it apart), on its node or on another, is asked
    // which it is right before each button, and sent nothing.
    void aVmGivenADeletedGuestsNumberIsSentNoPowerCommand_data()
    {
        QTest::addColumn<QString>("node");
        QTest::newRow("on-its-node") << u"pve1"_s;
        QTest::newRow("on-another-node") << u"pve2"_s;
    }
    void aVmGivenADeletedGuestsNumberIsSentNoPowerCommand()
    {
        QFETCH(QString, node);
        ProxmoxByHand pve(proxmoxTls());
        pve.nodes = {u"pve1"_s, u"pve2"_s};
        ProxmoxDevice::Where where = guestOf(pve);
        where.machine = numberOf(pve.guests.value(100));
        ProxmoxDevice device(where, pve.secret, fast());
        QCOMPARE(lookedAt(device).state, u"online"_s);
        pve.deleteGuest(100);
        Guest other;
        other.name = u"prod-db"_s;
        other.node = node;
        pve.makeGuest(other);
        for (const QString& kind :
             {u"power_on"_s, u"power_off"_s, u"atx_power"_s, u"atx_power_long"_s, u"atx_reset"_s}) {
            const base::Result<Done> said = pressed(device, kind);
            QCOMPARE(noteOf(said), u"(another_machine)"_s);
            QVERIFY(messageOf(said).starts_with("Another virtual machine has this guest's number"));
        }
        QCOMPARE(pve.guests.value(100).status, u"running"_s);
        QVERIFY(pve.tasks.isEmpty());
        QVERIFY(commands(pve).isEmpty());
        QCOMPARE(lookedAt(device).errorCode, u"another_machine"_s);
    }

    // The web's test_a_vm_that_gives_no_number_where_the_guest_gave_one_is_sent_nothing.
    void aVmThatGivesNoNumberIsSentNothing()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice::Where where = guestOf(pve);
        where.machine = numberOf(pve.guests.value(100));
        ProxmoxDevice device(where, pve.secret, fast());
        pve.deleteGuest(100);
        Guest old;
        old.name = u"old-one"_s;
        old.uuid.clear();
        pve.makeGuest(old);
        const base::Result<Done> said = pressed(device, u"atx_power_long"_s);
        QCOMPARE(noteOf(said), u"(another_machine)"_s);
        QVERIFY(messageOf(said).find("gives no SMBIOS UUID") != std::string::npos);
        QVERIFY(pve.tasks.isEmpty());
    }

    // The web's test_a_guest_that_gives_no_number_and_was_remembered_by_none_works_as_before.
    void aGuestThatGivesNoNumberAndWasKeptByNoneIsSwitchedAsBefore()
    {
        ProxmoxByHand pve(proxmoxTls());
        twoGuests(pve);
        pve.guests[101].uuid.clear();
        ProxmoxDevice device(guestOf(pve, 101), pve.secret, fast());
        QCOMPARE(noteOf(pressed(device, u"power_on"_s)), QString());
        QCOMPARE(pve.guests.value(101).status, u"running"_s);
    }

    // The web's test_the_guest_this_tile_was_added_for_is_followed_to_another_node_..., its reset: moved within its
    // cluster, followed and reset there.
    void theGuestIsResetWhereItWasMovedTo()
    {
        ProxmoxByHand pve(proxmoxTls());
        pve.nodes = {u"pve1"_s, u"pve2"_s};
        ProxmoxDevice device(guestOf(pve), pve.secret, fast());
        QVERIFY(identified(device));
        pve.guests[100].node = u"pve2"_s;
        QCOMPARE(noteOf(pressed(device, u"atx_reset"_s)), QString());
        QCOMPARE(device.where().node, u"pve2"_s);
        QCOMPARE(taskKinds(pve), QStringList{u"qmreset"_s});
        QVERIFY(pve.requests.contains(u"POST /nodes/pve2/qemu/100/status/reset"_s));
    }

    // The web's test_a_guest_changed_while_its_start_is_waited_for_is_said_to_have_perhaps_been_started: its driver let
    // go of while the start is waited for, after its task ended: whether it was carried out was not seen.
    void aDriverLetGoOfWhileAStartIsWaitedForSaysItMayHaveBeenDone()
    {
        ProxmoxByHand pve(proxmoxTls());
        twoGuests(pve);
        pve.taskTakesMs = 200;
        pve.powerTakesMs = 60'000;
        ProxmoxDevice::Pace pace = fast();
        pace.confirmWithinMs = 30'000;
        auto device = std::make_unique<ProxmoxDevice>(guestOf(pve, 101), pve.secret, pace);
        QList<base::Result<Done>> answers;
        device->act(Action::plain(u"power_on"_s), [&answers](const base::Result<Done>& said) { answers.append(said); });
        QVERIFY(QTest::qWaitFor(
            [&pve] {
                return std::ranges::any_of(pve.tasks, [](const ProxmoxByHand::Task& task) {
                    return task.type == u"qmstart"_s && task.status == u"OK"_s;
                });
            },
            10'000));
        QTest::qWait(300);
        device.reset();
        QVERIFY(QTest::qWaitFor([&answers] { return !answers.isEmpty(); }, 5000));
        QCOMPARE(noteOf(answers.first()), u"(timeout)"_s);
        QVERIFY(messageOf(answers.first()).find("may have been done") != std::string::npos);
    }

    // The web's test_the_wait_for_a_guest_has_one_bound_from_the_press_however_slowly_proxmox_answers: reads of the
    // guest that take three seconds each after the command; the wait ends at its bound, and says the guest is not
    // running yet.
    void theWaitHasOneBoundFromThePress()
    {
        ProxmoxByHand pve(proxmoxTls());
        twoGuests(pve);
        pve.taskTakesMs = 500;
        pve.powerTakesMs = 60'000;
        pve.statusTakesMsAfterACommand = 3000;
        ProxmoxDevice::Pace pace = fast();
        pace.taskWithinMs = 1000;
        pace.confirmWithinMs = 5000;
        pace.quietMs = 6000;
        // (A read in the wait is given what is left of it, and never less than getting a connection may take, and a
        // little more: here 4.5 s, as the web's are with its own waits.)
        pace.connectWithinMs = 4000;
        ProxmoxDevice device(guestOf(pve, 101), pve.secret, pace);
        QElapsedTimer took;
        took.start();
        const base::Result<Done> said = pressed(device, u"power_on"_s);
        QCOMPARE(noteOf(said), u"(timeout)"_s);
        QVERIFY2(messageOf(said).find("is not running yet") != std::string::npos, messageOf(said).c_str());
        QVERIFY2(took.elapsed() < 15'000, QByteArray::number(took.elapsed()).constData());
    }

    // The web's test_a_read_in_the_wait_for_a_start_is_not_waited_for_past_its_bound: after the command every look at
    // the guest answers a byte at a time; each read in the wait is given what is left of the wait (and never less than
    // getting a connection may take), not a question's whole time. And as a real Proxmox is slow, its status line late
    // (pveproxy writes an answer when it is ready: review cx3-3, L2): a read given up at its moment is no failure of
    // contact, and nothing signs in again. The look given up at the wait's bound, the task is not asked how it stands
    // after it, and that it was not said is said (the web asks it once more past the bound, and says it still runs:
    // outside review 2, 4.4).
    void aReadInTheWaitIsNotWaitedForPastItsBound_data()
    {
        QTest::addColumn<bool>("trickles");
        QTest::newRow("a byte at a time") << true;
        QTest::newRow("its status line late") << false;
    }
    void aReadInTheWaitIsNotWaitedForPastItsBound()
    {
        QFETCH(bool, trickles);
        ProxmoxByHand pve(proxmoxTls());
        twoGuests(pve);
        pve.taskTakesMs = 60'000;
        pve.statusTricklesAfterACommand = trickles;
        pve.statusTakesMsAfterACommand = trickles ? 0 : 20'000;
        ProxmoxDevice::Pace pace = fast();
        pace.taskWithinMs = 1000;
        pace.confirmWithinMs = 1000;
        pace.connectWithinMs = 1000;
        pace.quietMs = 8000;
        pace.transferMs = 8000; // (a question's own whole: 17 s)
        ProxmoxDevice device(guestOf(pve, 101), pve.secret, pace);
        QElapsedTimer took;
        took.start();
        const base::Result<Done> said = pressed(device, u"power_on"_s);
        QCOMPARE(noteOf(said), u"(timeout)"_s);
        QVERIFY2(messageOf(said).find("did not say how its task went") != std::string::npos, messageOf(said).c_str());
        QVERIFY2(took.elapsed() < 9000, QByteArray::number(took.elapsed()).constData());
        QTest::qWait(500); // (whatever would have gone has had the time to)
        QCOMPARE(pve.asked(u"GET /version"_s), 1);
        QVERIFY(std::ranges::none_of(pve.requests, [](const QString& one) { return one.contains(u"/tasks/"_s); }));
    }

    // Outside review of 0.2.0, 3.8: a press is bounded as a whole, from the press. The questions it is decided by (its
    // state, which virtual machine has the number) are held to Pace::decideWithinMs from the press, and its command
    // goes out within it or not at all: here the guest's settings answer too slowly for what is left of it, or the two
    // answer within their own times and the bound is over by then. Nothing is sent, and that is said.
    void whatAPressIsDecidedByIsHeldToItsBound_data()
    {
        QTest::addColumn<int>("statusMs");
        QTest::addColumn<int>("configMs");
        QTest::addColumn<int>("withinMs");
        QTest::newRow("its-state-too-slow") << 5000 << 0 << 4500;
        QTest::newRow("its-settings-too-slow") << 1500 << 5000 << 4500;
        QTest::newRow("over-by-the-time-both-are-in") << 2000 << 1000 << 6000;
    }
    void whatAPressIsDecidedByIsHeldToItsBound()
    {
        QFETCH(int, statusMs);
        QFETCH(int, configMs);
        QFETCH(int, withinMs);
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice::Pace pace = fast();
        pace.connectWithinMs = 1000;
        pace.quietMs = 6000;
        pace.transferMs = 2000;
        pace.decideWithinMs = 2500;
        ProxmoxDevice device(guestOf(pve), pve.secret, pace);
        QVERIFY(identified(device)); // (signed in, and its number known)
        pve.statusTakesMs = statusMs;
        pve.configTakesMs = configMs;
        QElapsedTimer took;
        took.start();
        const base::Result<Done> said = pressed(device, u"atx_reset"_s);
        QCOMPARE(noteOf(said), u"(action_failed)"_s);
        QVERIFY2(messageOf(said).starts_with("Nothing was sent"), messageOf(said).c_str());
        QVERIFY(pve.tasks.isEmpty());
        QVERIFY(commands(pve).isEmpty());
        QVERIFY2(took.elapsed() < withinMs, QByteArray::number(took.elapsed()).constData());
    }

    // ...and a follow to another node in a start's wait is held to the wait's bound: the guest moved right after the
    // command, and the list of its Proxmox's guests, or its settings there, slow to come, the press is answered at its
    // bound, and not once they are in.
    void aFollowInTheWaitIsHeldToItsBound_data()
    {
        QTest::addColumn<int>("listMs");
        QTest::addColumn<int>("configMs");
        QTest::newRow("its-list-slow") << 8000 << 0;
        QTest::newRow("its-settings-there-slow") << 0 << 8000;
    }
    void aFollowInTheWaitIsHeldToItsBound()
    {
        QFETCH(int, listMs);
        QFETCH(int, configMs);
        ProxmoxByHand pve(proxmoxTls());
        twoGuests(pve);
        pve.nodes = {u"pve1"_s, u"pve2"_s};
        pve.powerTakesMs = 60'000;
        pve.afterCommand = [&pve, listMs, configMs] {
            pve.guests[101].node = u"pve2"_s;
            pve.resourcesTakesMs = listMs;
            pve.configTakesMs = configMs;
        };
        ProxmoxDevice::Pace pace = fast();
        pace.taskWithinMs = 1000;
        pace.confirmWithinMs = 1000;
        pace.connectWithinMs = 1000;
        pace.quietMs = 10'000;
        pace.transferMs = 2000;
        ProxmoxDevice device(guestOf(pve, 101), pve.secret, pace);
        QElapsedTimer took;
        took.start();
        const base::Result<Done> said = pressed(device, u"power_on"_s);
        QCOMPARE(noteOf(said), u"(timeout)"_s);
        QVERIFY2(took.elapsed() < 6000, QByteArray::number(took.elapsed()).constData());
    }

    // ...and its command, refused 401 (the token taken back for a moment), is sent again only within that bound: the
    // sign-in after it slow to come back, or the question which virtual machine has the number answered after it,
    // nothing goes a second time, and nothing is said to have perhaps been done.
    void aCommandRefusedIsNotSentAgainPastItsBound_data()
    {
        QTest::addColumn<int>("signInMs");
        QTest::addColumn<int>("configMs");
        QTest::newRow("its-sign-in-slow") << 4000 << 0;
        QTest::newRow("its-question-answered-late") << 1000 << 1200;
    }
    void aCommandRefusedIsNotSentAgainPastItsBound()
    {
        QFETCH(int, signInMs);
        QFETCH(int, configMs);
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice::Pace pace = fast();
        pace.connectWithinMs = 1000;
        pace.quietMs = 6000;
        pace.transferMs = 2000;
        pace.signInQuietMs = 6000;
        pace.decideWithinMs = 2000;
        ProxmoxDevice device(guestOf(pve), pve.secret, pace);
        QVERIFY(identified(device));
        const QString reset = u"POST /nodes/pve1/qemu/100/status/reset"_s;
        bool once = false;
        pve.refuses = [&pve, &once, &reset, signInMs, configMs](const QString& said) {
            if (said != reset || std::exchange(once, true)) {
                return false;
            }
            // (The sign-in after it, and the question after that, slow to come back.)
            pve.versionTakesMs = signInMs;
            pve.configTakesMs = configMs;
            return true;
        };
        const base::Result<Done> said = pressed(device, u"atx_reset"_s);
        QCOMPARE(noteOf(said), u"(action_failed)"_s);
        QVERIFY2(messageOf(said).starts_with("Nothing was done"), messageOf(said).c_str());
        QVERIFY(!ProxmoxDevice::mayHaveBeenDone(said.error()));
        QVERIFY(pve.tasks.isEmpty());
        QCOMPARE(pve.asked(reset), 0);
    }

    // Review cx3-1, L1 (outside review of 0.2.0, 3.8): a command Proxmox answers "not on this node" (the guest moved
    // between the press's reads and its command) is followed to where it is now within the press's bound, and sent
    // there within it or not at all: the list of its Proxmox's guests slow to come, nothing is done, and that is said
    // within a few seconds of the bound; quick, the guest is reset where it went.
    void aCommandAnsweredNotOnThisNodeIsFollowedWithinThePressesBound_data()
    {
        QTest::addColumn<int>("listMs");
        QTest::addColumn<int>("configMs");
        QTest::addColumn<bool>("done");
        // (A question held to a moment is given a connection's time and half a second at the least, 3.5 s here: the
        // list too slow for that; or in within it, and the settings on the new node in after the bound.)
        QTest::newRow("its-list-slow") << 8000 << 0 << false;
        QTest::newRow("its-settings-there-past-the-bound") << 2000 << 1000 << false;
        QTest::newRow("both-quick") << 0 << 0 << true;
    }
    void aCommandAnsweredNotOnThisNodeIsFollowedWithinThePressesBound()
    {
        QFETCH(int, listMs);
        QFETCH(int, configMs);
        QFETCH(bool, done);
        ProxmoxByHand pve(proxmoxTls());
        pve.nodes = {u"pve1"_s, u"pve2"_s};
        ProxmoxDevice::Pace pace = fast();
        pace.connectWithinMs = 3000;
        pace.quietMs = 10'000;
        pace.transferMs = 2000;
        pace.decideWithinMs = 2500;
        ProxmoxDevice device(guestOf(pve), pve.secret, pace);
        QVERIFY(identified(device)); // (signed in, and its number known)
        bool moved = false;
        pve.afterConfigRead = [&pve, &moved, listMs, configMs] {
            if (std::exchange(moved, true)) {
                return;
            }
            // (Right after the press's read of its settings, before its command.)
            pve.guests[100].node = u"pve2"_s;
            pve.resourcesTakesMs = listMs;
            pve.configTakesMs = configMs;
        };
        QElapsedTimer took;
        took.start();
        const base::Result<Done> said = pressed(device, u"atx_reset"_s);
        if (done) {
            QCOMPARE(noteOf(said), QString());
            QCOMPARE(taskKinds(pve), QStringList{u"qmreset"_s});
            QCOMPARE(pve.asked(u"POST /nodes/pve2/qemu/100/status/reset"_s), 1);
            return;
        }
        QCOMPARE(noteOf(said), u"(action_failed)"_s);
        QVERIFY2(messageOf(said).starts_with("Nothing was done"), messageOf(said).c_str());
        QVERIFY(!ProxmoxDevice::mayHaveBeenDone(said.error()));
        QVERIFY(pve.tasks.isEmpty());
        QVERIFY2(took.elapsed() < 6000, QByteArray::number(took.elapsed()).constData());
    }

    // ...and a command sent again after a 401 goes to the node the guest is on by then: the question of rule 30 before
    // it followed the guest there (review cx3-1, L1).
    void aCommandSentAgainAfterA401GoesWhereTheGuestIsNow()
    {
        ProxmoxByHand pve(proxmoxTls());
        pve.nodes = {u"pve1"_s, u"pve2"_s};
        ProxmoxDevice device(guestOf(pve), pve.secret, fast());
        QVERIFY(identified(device));
        const QString reset = u"POST /nodes/pve1/qemu/100/status/reset"_s;
        bool once = false;
        pve.refuses = [&pve, &once, &reset](const QString& said) {
            if (said != reset || std::exchange(once, true)) {
                return false;
            }
            pve.guests[100].node = u"pve2"_s; // (moved meanwhile)
            return true;
        };
        const base::Result<Done> said = pressed(device, u"atx_reset"_s);
        QVERIFY2(said, messageOf(said).c_str());
        QCOMPARE(taskKinds(pve), QStringList{u"qmreset"_s});
        QCOMPARE(pve.asked(u"POST /nodes/pve2/qemu/100/status/reset"_s), 1);
        QCOMPARE(pve.asked(reset), 0);
    }

    // Review cx3-3, L1: a command that finds its driver signed out on its way (another question of the driver's got
    // nothing back while the press's own reading of its settings was out: a busy Proxmox, a line that fell) is sent
    // only once which virtual machine has the guest's number was asked again on a reading after that sign-in, as after
    // a 401, and within the press's bound: another made under the number while the sign-in was out is sent nothing,
    // and where the sign-in comes back after the bound, nothing is sent either, and that is said.
    void aCommandThatFindsItsDriverSignedOutIsJudgedAgainWithinItsBound_data()
    {
        QTest::addColumn<int>("signInMs");
        QTest::addColumn<QString>("code");
        QTest::newRow("another vm made meanwhile") << 0 << u"another_machine"_s;
        QTest::newRow("its sign-in past the bound") << 4000 << u"action_failed"_s;
    }
    void aCommandThatFindsItsDriverSignedOutIsJudgedAgainWithinItsBound()
    {
        QFETCH(int, signInMs);
        QFETCH(QString, code);
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice::Pace pace = fast();
        pace.decideWithinMs = 2500;
        ProxmoxDevice::Where where = guestOf(pve);
        where.machine = numberOf(pve.guests.value(100));
        ProxmoxDevice device(where, pve.secret, pace);
        QVERIFY(identified(device)); // (signed in, and its number known)
        bool heldOnce = false;
        bool signedOut = false;
        pve.answering = [&](const QString& said, Answered& answer) {
            if (said == u"GET /nodes/pve1/qemu/101/config"_s) {
                // Another question of the driver's, asked while the press's reading is out: nothing comes back.
                answer.hangUp = true;
                signedOut = true;
                pve.versionTakesMs = signInMs;
                return;
            }
            if (said == u"GET /nodes/pve1/qemu/100/config"_s && !std::exchange(heldOnce, true)) {
                answer.afterMs = 1000; // (the press's own reading, a moment in coming)
                QTimer::singleShot(100, &device, [&device] {
                    device.numberOfGuest(u"pve1"_s, 101, [](const base::Result<QString>& /*number*/) {});
                });
                return;
            }
            if (said == u"GET /version"_s && signedOut && signInMs == 0) {
                // While the sign-in again is out, the guest is deleted and another made under its number.
                pve.deleteGuest(100);
                Guest other;
                other.name = u"prod-db"_s;
                pve.makeGuest(other);
            }
        };
        QElapsedTimer took;
        took.start();
        const base::Result<Done> said = pressed(device, u"atx_reset"_s);
        QVERIFY(signedOut);
        QCOMPARE(noteOf(said), u"(%1)"_s.arg(code));
        if (code == u"action_failed"_s) {
            QVERIFY2(messageOf(said).starts_with("Nothing was done"), messageOf(said).c_str());
            QVERIFY2(took.elapsed() < 4000, QByteArray::number(took.elapsed()).constData());
        }
        QVERIFY(!ProxmoxDevice::mayHaveBeenDone(said.error()));
        QTest::qWait(signInMs + 500); // (whatever would have gone has had the time to)
        QVERIFY(pve.tasks.isEmpty());
        QVERIFY(commands(pve).isEmpty());
    }

    // Outside review 2, 4.1: a reading a sign-in old decides nothing, also where its driver is signed in again by the
    // time it comes. The press's own reading of the guest's settings is out, a while in coming; another question of
    // the driver's gets nothing back (contact lost), and a third signs in again in full while that reading is still
    // out, in which time the guest is deleted and another made under its number. The reading, made before all that,
    // comes with the guest's old number: the command does not go on it, but only once which virtual machine has the
    // number was asked on a reading after the new sign-in. That finds the other one, and nothing is sent to it.
    void aReadingASignInOldDecidesNothingThoughItsDriverIsSignedInAgain()
    {
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice::Pace pace = fast();
        pace.quietMs = 8000; // (the press's reading is held three seconds)
        ProxmoxDevice::Where where = guestOf(pve);
        where.machine = numberOf(pve.guests.value(100));
        ProxmoxDevice device(where, pve.secret, pace);
        QVERIFY(identified(device)); // (signed in, and its number known)
        QElapsedTimer took;
        took.start();
        bool heldOnce = false;
        bool lost = false;
        qint64 signedInAgainAt = -1;
        qint64 listedAt = -1;
        pve.answering = [&](const QString& said, Answered& answer) {
            if (said == u"GET /nodes/pve1/qemu/101/config"_s) {
                answer.hangUp = true; // (another question of the driver's: nothing comes back)
                lost = true;
                return;
            }
            if (said == u"GET /nodes/pve1/qemu/100/config"_s && !std::exchange(heldOnce, true)) {
                // The press's own reading, made now, with the guest's number: three seconds in coming. Meanwhile
                // contact is lost, and a list of the guests signs in again in full.
                answer.afterMs = 3000;
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
            if (said == u"GET /version"_s && lost && signedInAgainAt < 0) {
                // While the sign-in again is out, the guest is deleted and another made under its number.
                signedInAgainAt = took.elapsed();
                pve.deleteGuest(100);
                Guest other;
                other.name = u"prod-db"_s;
                pve.makeGuest(other);
            }
        };
        const base::Result<Done> said = pressed(device, u"atx_reset"_s);
        QVERIFY(lost);
        // (Signed in again, and listed, before the press's reading came.)
        QVERIFY2(signedInAgainAt > 0 && listedAt > 0 && listedAt < 2800,
                 qPrintable(u"signed in again at %1 ms, listed at %2 ms"_s.arg(signedInAgainAt).arg(listedAt)));
        QCOMPARE(noteOf(said), u"(another_machine)"_s);
        QVERIFY(!ProxmoxDevice::mayHaveBeenDone(said.error()));
        QTest::qWait(500); // (whatever would have gone has had the time to)
        QVERIFY(pve.tasks.isEmpty());
        QVERIFY(commands(pve).isEmpty());
    }

    // Outside review 2, 4.4: a command's own answer is held to the press's bound too. It went within that bound (its
    // reads quick, or taking most of it), Proxmox carried it out and its answer is a long while in coming: what came of
    // it is said by the bound, or a connection's time and half a second after the command went where that is later,
    // and not after a question's own whole time. It may have been done, and that is said.
    void aCommandsOwnAnswerIsHeldToThePressesBound_data()
    {
        QTest::addColumn<int>("configMs");
        QTest::newRow("its reads quick") << 0;
        QTest::newRow("its reads taking most of the bound") << 3000;
    }
    void aCommandsOwnAnswerIsHeldToThePressesBound()
    {
        QFETCH(int, configMs);
        ProxmoxByHand pve(proxmoxTls());
        ProxmoxDevice::Pace pace = fast();
        pace.connectWithinMs = 1000;
        pace.quietMs = 10'000;
        pace.transferMs = 2000; // (a question's own whole: 13 s)
        pace.decideWithinMs = 5000;
        ProxmoxDevice device(guestOf(pve), pve.secret, pace);
        QVERIFY(identified(device)); // (signed in, and its number known)
        pve.configTakesMs = configMs;
        const QString reset = u"POST /nodes/pve1/qemu/100/status/reset"_s;
        pve.answering = [&reset](const QString& said, Answered& answer) {
            if (said == reset) {
                answer.afterMs = 12'000; // (carried out; its answer a long while in coming)
            }
        };
        QElapsedTimer took;
        took.start();
        const base::Result<Done> said = pressed(device, u"atx_reset"_s);
        // (The bound is 5 s, and a command that went within it is given what is left of it, 1.5 s at the least: 6.5 s
        // at the most. A question's own whole would have been 13 s.)
        QVERIFY2(took.elapsed() < 8000, QByteArray::number(took.elapsed()).constData());
        QCOMPARE(noteOf(said), u"(timeout)"_s);
        QVERIFY(ProxmoxDevice::mayHaveBeenDone(said.error()));
        QVERIFY2(messageOf(said).find("may have been done") != std::string::npos, messageOf(said).c_str());
        QCOMPARE(taskKinds(pve), QStringList{u"qmreset"_s});
    }

    // ...and the wait for a start sends no read once its bound is over: neither the task's after a look that came back
    // past it, nor the next look where that is due after it (the wait is judged at its bound), nor the next step of a
    // follow to another node (the guest moved right after the command) where its state on the old node, the list of
    // its Proxmox's guests or its settings on the new node came back past it, nor a look where the command's own
    // answer came back past it. What came of it is said then.
    void aStartsWaitSendsNoReadPastItsBound_data()
    {
        QTest::addColumn<QString>("how");
        QTest::newRow("the task after a look past the bound") << u"task"_s;
        QTest::newRow("the next look due past the bound") << u"look"_s;
        QTest::newRow("a follow after a look past the bound") << u"follow"_s;
        QTest::newRow("a follow after its list past the bound") << u"list"_s;
        QTest::newRow("a follow after its settings there past the bound") << u"there"_s;
        QTest::newRow("a look after the command's answer past the bound") << u"command"_s;
        // (And one handed to Proxmox's high availability, whose task ends at once, every look of its wait getting
        // nothing back: second review, N1.)
        QTest::newRow("a start handed to high availability, its looks unanswered") << u"ha"_s;
    }
    void aStartsWaitSendsNoReadPastItsBound()
    {
        QFETCH(QString, how);
        ProxmoxByHand pve(proxmoxTls());
        twoGuests(pve);
        pve.nodes = {u"pve1"_s, u"pve2"_s};
        pve.powerTakesMs = 60'000; // (never seen running)
        ProxmoxDevice::Pace pace = fast();
        pace.quietMs = 10'000;
        pace.transferMs = 2000;
        // (The wait's bound is 3 s, its reads begun by 2.5 s: room for the press's own reads and its command on a
        // loaded PC. Fresh review of 4.4, L3.)
        pace.taskWithinMs = 1500;
        pace.confirmWithinMs = 1500;
        if (how == u"look"_s) {
            // The task ends well at once, and the next look is due a while after the wait's bound.
            pace.confirmEveryMs = 5000;
            pace.connectWithinMs = 1000;
        } else {
            // A read held to the bound is given 5.5 s at the least: the look after the command, a step of its follow,
            // or the command's own answer, comes back 1.5 s past the bound.
            pace.connectWithinMs = 5000;
            pve.taskTakesMs = 60'000;
        }
        if (how == u"ha"_s) {
            pve.guests[101].ha = true;
            pve.taskTakesMs = 0; // (its own task, "hastart", ends at once; high availability starts it, here never)
        }
        const qint64 boundMs = pace.taskWithinMs + pace.confirmWithinMs;
        const bool follows = how == u"follow"_s || how == u"list"_s || how == u"there"_s;
        bool commanded = false;
        pve.afterCommand = [&pve, &commanded, follows] {
            commanded = true;
            if (follows) {
                pve.guests[101].node = u"pve2"_s;
            }
        };
        // The one answer that comes back past the bound (within what its read is given).
        const QString late = how == u"command"_s                  ? u"POST /nodes/pve1/qemu/101/status/start"_s
                             : how == u"list"_s                   ? u"GET /cluster/resources"_s
                             : how == u"there"_s                  ? u"GET /nodes/pve2/qemu/101/config"_s
                             : how == u"look"_s || how == u"ha"_s ? QString()
                                                                  : u"GET /nodes/pve1/qemu/101/status/current"_s;
        QElapsedTimer took;
        QList<std::pair<QString, qint64>> readsAfter;
        pve.answering = [&](const QString& said, Answered& answer) {
            if ((commanded || said.startsWith(u"POST "_s)) && said == late) {
                answer.afterMs = 4500;
            }
            if (how == u"ha"_s && commanded && said == u"GET /nodes/pve1/qemu/101/status/current"_s) {
                answer.hangUp = true; // (nothing back to any look after the command)
            }
            if (commanded && said.startsWith(u"GET "_s)) {
                readsAfter.append({said, took.elapsed()});
            }
        };
        ProxmoxDevice device(guestOf(pve, 101), pve.secret, pace);
        QVERIFY(identified(device));
        took.start();
        const base::Result<Done> said = pressed(device, u"power_on"_s);
        QVERIFY(how == u"command"_s || !readsAfter.isEmpty());
        for (const auto& [read, at] : std::as_const(readsAfter)) {
            // (Sent by the bound: it reaches the stand-in a moment later at the most. One sent after what came back
            // late would reach it 1.5 s past the bound.)
            QVERIFY2(at < boundMs + 800, qPrintable(u"%1 at %2 ms"_s.arg(read).arg(at)));
        }
        if (how == u"follow"_s) {
            QVERIFY(std::ranges::none_of(readsAfter, [](const auto& one) { return one.first.contains(u"/pve2/"_s); }));
        }
        // (Judged at its bound where nothing is out then; else as soon as what is out comes back.)
        const qint64 within = how == u"look"_s ? 1500 : 6000;
        QVERIFY2(took.elapsed() < boundMs + within, QByteArray::number(took.elapsed()).constData());
        // (Under way and perhaps done: the guest was not seen running within the bound.)
        QCOMPARE(noteOf(said), u"(timeout)"_s);
        // And said as it is: where no read of the guest came back after the command, it is not said to be still
        // stopped, nor where none was tried after its task's end that it could not be read (fresh review of 4.4, L1).
        if (how != u"task"_s) {
            QVERIFY2(messageOf(said).find("still stopped") == std::string::npos, messageOf(said).c_str());
        }
        QVERIFY2(messageOf(said).find("could not be read") == std::string::npos, messageOf(said).c_str());
        if (how == u"ha"_s) {
            QVERIFY2(messageOf(said).find("high availability") != std::string::npos, messageOf(said).c_str());
            QVERIFY2(messageOf(said).find("is not running") == std::string::npos, messageOf(said).c_str());
        }
    }

    // A command answered with what is not the name of a task of Proxmox's is no yes (the web's UPID).
    void aCommandAnsweredWithNoTaskIsNoYes()
    {
        ProxmoxByHand pve(proxmoxTls());
        pve.answersNoTask = true;
        ProxmoxDevice device(guestOf(pve), pve.secret, fast());
        const base::Result<Done> said = pressed(device, u"atx_reset"_s);
        QCOMPARE(noteOf(said), u"(bad_response)"_s);
        QCOMPARE(messageOf(said), std::string("Proxmox did not say that it took the power command"));
    }
};

QTEST_GUILESS_MAIN(TestProxmoxPower)
#include "test_proxmox_power.moc"
