#include <qkvm/core/redfish_device.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonDocument>
#include <QTimer>

#include <qkvm/core/kvmd_answers.h>
#include <qkvm/core/setup_overrides.h>

using namespace Qt::StringLiterals;

// Start into BIOS setup on a server of this PC's own wall (the web's `bios_setup`, redfish.py `_boot_into_setup`,
// `_setup_steps`, `_clear_setup_override`; FOR-THE-DESKTOP-APP part 2, the `bios_setup` row): the server's one-time
// boot override set to its setup ("Once", "BiosSetup"), where its controller offers that, then the server restarted
// (where it is on) or switched on (where it is off). A server must never be left booting into its setup: an ordinary
// boot later (an update at night, a crash) would stop in it, and the machine stay down. So from the moment the
// override may be set, every way out clears it again wherever it still shows, and says so where that could not be
// done. Stricter than the web, which leaves it set where the restart may have gone through: here the start is
// watched, and an override the server has not taken by the end of the press's four minutes is cleared all the same.
// And stricter than the web's stop, which only logs (review 4's MEDIUM 1): a driver let go of mid-way (at the program's
// end too) has the goodbyes clear it on the way out, bounded, read back before its session goes; and from the moment
// the change can go out the server is noted beside the settings as "may be set" (setup_overrides.h), let go of only
// where it is seen off with nothing of the change still on its way, and told at the next start otherwise; one whose
// change the controller only queued, by no look at all (D1), unless reads of the server see that change through (its
// task followed from 0.2.0 on; the task's own word takes no mark off). It is marked so as the change goes, and its
// answer takes that mark off where it says the change was not queued (outside review 2, 3.5).
namespace qkvm::core {

namespace {

const QString ours0 = u"once"_s;
const QString ours1 = u"biossetup"_s;

QString notCleared()
{
    return u"the BIOS setup boot override may be set and could not be cleared, so the next boot may land in the BIOS "
           u"setup"_s;
}

QString lookAgain()
{
    return u"Look at the server's boot override, or start into BIOS setup again."_s;
}

// Whether an answer's body is a task (Redfish's Task, by its type or its state): what a controller that only queues a
// change answers with.
bool aTask(const QJsonObject& said)
{
    return said.value("@odata.type"_L1).toString().startsWith(u"#Task."_s) || said.contains("TaskState"_L1);
}

// What a press says beside what came of it where the controller only queued the change and no read of the server saw it
// through (D1); `taskEnded`: its task was followed to an end, which is only the controller's word (outside review of
// D1, round 2, H1).
QString queuedSaid(bool taskEnded)
{
    if (taskEnded) {
        return u"The controller queued the change: it may still be made later, whatever its task says. Look at the "
               u"server's boot override in its BMC before you rely on it."_s;
    }
    return u"The controller queued the change, and qKVM cannot follow it to its end: it may still be made later. Look "
           u"at the server's boot override in its BMC before you rely on it."_s;
}

// ...where its answer to the change was lost, so that it may have queued it (outside review of 0.2.0, area 5, 5.9).
QString mayHaveQueuedSaid()
{
    return u"The controller's answer to the change did not come, so it may have queued it, and qKVM cannot follow "
           u"it to its end: it may still be made later. Look at the server's boot override in its BMC before you "
           u"rely on it."_s;
}

// ...and where the mark that it queued it, or may have, could not be written down beside the settings (5.9).
QString markNotWrittenSaid()
{
    return u"qKVM could not write down that the controller queued this change (check the free space and the folder of "
           u"qKVM's settings): the server's card says it until qKVM ends, and after a restart of qKVM nothing would."_s;
}

// Whether a queued mark of the server's record stands unwritten: its record marked, and nothing written down since (a
// later writing that worked carried the mark into the file). At a press's end a record that is gone was let go of by
// the press itself, its change seen through (settleTheNote), and one no longer marked was seen through by reads: there
// is nothing to say of either (outside review of 0.2.0's Redfish fixes, L1: an unwritten mark was said of a record no
// longer there). Asked only of a mark this press put on (markTheQueue; round 2, L4).
bool stillUnwritten(const SetupOverrides& kept, const QUrl& address, const QString& machine)
{
    const std::optional<SetupOverrides::Left> left = kept.standing(address, machine);
    return left && left->queued && kept.written() == std::optional<bool>(false);
}

// What a press that restarted nothing says, `lead` first, then what became of the override (`left`: nothing where it
// was cleared again).
base::Error leftAs(const QString& lead, const std::optional<QString>& left)
{
    if (!left) {
        return {"action_failed", (lead + u"; the BIOS setup boot override was cleared again."_s).toStdString()};
    }
    if (*left == notCleared()) {
        return {"timeout", (lead + u"; "_s + *left + u". "_s + lookAgain()).toStdString()};
    }
    return {"action_failed", (lead + u"; "_s + *left + u"."_s).toStdString()};
}

// A press whose power change did not go through (refused, or never sent): what became of the override.
base::Error unset(const base::Error& why, const std::optional<QString>& left)
{
    const QString said = QString::fromStdString(why.message);
    if (!left) {
        return {why.code, u"Nothing was done: the power change did not go through (%1); the BIOS setup boot override "
                          u"was cleared again."_s.arg(said)
                              .toStdString()};
    }
    if (*left == notCleared()) {
        return {"timeout",
                u"The power change did not go through (%1); %2. %3"_s.arg(said, *left, lookAgain()).toStdString()};
    }
    return {why.code,
            u"Nothing was done: the power change did not go through (%1); %2."_s.arg(said, *left).toStdString()};
}

// Another tool's override as a read after this press's change shows it: not the BIOS setup one kept for every boot,
// which is this press's own (a board that keeps "Once" as "Continuous"; the press was refused where any other override
// stood before it, setupRead), and is cleared as the one-time one is (the owner's decision of 2026-11-18, b).
QString othersAfterTheChange(const QJsonObject& system)
{
    return redfish::setupForEveryBoot(system) ? QString() : redfish::anotherOverride(system);
}

// A failure said at a queued change's first look at its task, perhaps an old task's (outside review of D1, round 2,
// L2): a task that said it failed, or its monitor that answered with an error (a 405 from one that takes no GET, say:
// outside review of the fuzz work, round 3, L6 b). Its words, in the three places a press says them.
struct SaidAtOnce {
    bool byItsMonitor = false;
    QString words; // the controller's; empty: none given

    QString why() const { return words.isEmpty() ? u"it gave no reason"_s : words; }
    // Beside "does not show it set".
    QString clause() const
    {
        return byItsMonitor ? u"its task's monitor answered at once with an error (%1), and qKVM cannot be sure that "
                              u"answer was of this change"_s.arg(why())
                            : u"its task said at once that it did not set it (%1), and qKVM cannot be sure that task "
                              u"was this change's"_s.arg(why());
    }
    // Where the server could not be read back after it.
    QString unread() const
    {
        return byItsMonitor ? u"Nothing was restarted: its task's monitor answered at once with an error (%1), qKVM "
                              u"cannot be sure that answer was of this change, and the server could not be read "
                              u"back"_s.arg(why())
                            : u"Nothing was restarted: the controller's task said at once that it did not set the BIOS "
                              u"setup boot override (%1), qKVM cannot be sure that task was this change's, and the "
                              u"server could not be read back"_s.arg(why());
    }
    // Beside the restart, after a read that showed the override set.
    QString restarted() const
    {
        return byItsMonitor ? u"Its task's monitor answered at once with an error (%1), and qKVM cannot be sure that "
                              u"answer was of this change; the server then showed the BIOS setup boot override set, "
                              u"and was restarted into its setup."_s.arg(why())
                            : u"Its task said at once that it did not set the BIOS setup boot override (%1), and qKVM "
                              u"cannot be sure that task was this change's; the server then showed the BIOS setup boot "
                              u"override set, and was restarted into its setup."_s.arg(why());
    }
};

// What a press says where what is kept beside the settings could not be written before its change: it is not sent
// (outside review of 0.1.2, C6; and its queued mark as the change goes, review cx2 of outside review 2, L3).
base::Error noteNotSaved()
{
    return {"storage", "qKVM could not save the note that this server may boot into its BIOS setup, so it did not ask "
                       "the server: nothing was changed on it. Check the free space and the folder of qKVM's settings, "
                       "and try again."};
}

// ...and beside what came of a press while the server's card asks whether the computer at its place, which gives a
// number of its own now, is its server: nothing but reads went to it since (the owner's decision of 2026-11-22).
QString asksSaid()
{
    return u"Its server gives a number of its own now (its UUID): its card asks whether it is the server the card was "
           u"added for, and nothing but looks goes to it until that is answered."_s;
}

// What a press that only read so far says where the controller could not be read: nothing was sent that changes
// anything (a "may have been done" of a read is none of a press).
base::Error onlyRead(const base::Error& why)
{
    if (why.code == "timeout" || why.code == "dropped") {
        return {"action_failed", "Nothing was done: the controller could not be read (" + why.message + ")."};
    }
    return why;
}

} // namespace

struct RedfishDevice::Setup {
    std::shared_ptr<Pressing> pressing;
    qint64 restartBy = 0; // the restart goes out before this, or not at all (Pace::setupWithinMs)
    qint64 endsAt = 0;    // the press's end: an override still shown then is cleared (Pace::setupPatienceMs)
    QString place;        // the computer
    QString resetTarget;
    QString resetKind;
    // As it began, its controller's list read anew (Ask::listedFrom: for its change alone).
    qint64 listedFrom = 0;
    bool running = true;    // the server was on: it is restarted (else switched on)
    bool maySet = false;    // the change of the override may have been made (from when it can go out)
    bool seenUnset = false; // a read after the change came, and did not show it
    // ...and one did so after its task's end was seen (reads while it ran show it unset, as they should, and say
    // nothing of its end: outside review of the fuzz work, round 3, L1)
    bool seenUnsetAfterTheEnd = false;
    // The restart went out (or may have), and what is said beside what comes of it: a failure's words said before the
    // server showed the override set (round 3, L2).
    bool restarted = false;
    QString restartNote;
    QString lastPower; // the server's power as the last read said it
    // ...and as a read after the restart or the switch-on said it, the watch's or a clearing's; empty: none came back.
    // What is said of a server still off rests on this alone (review cx2 of outside review 2, M2: said from the read
    // before).
    QString powerAfter;
    bool over = false;
    // The mark the change was last refused for (412, 428), and how often it was (outside review of 0.2.0, 5.8).
    QByteArray refused;
    int refusals = 0;
    // What is kept beside the settings for it (setup_overrides.h, review 4's MEDIUM 1): whether it was noted there, and
    // until when what stood there before this press doubted; until when the change it sent may still come to be made
    // (this driver's clock); whether that change was seen made (a 204, a 200 or a read that shows it, a read before a
    // clearing that shows it), so that nothing of it can come later; and when the override was seen off at the end
    // (taken, cleared, another tool's in its place). And, from its note on, that it is under way, by no clock
    // (SetupOverrides::pressUnderWay, review 012-hold's LOW 1): given back as this goes, with the press's last step, at
    // its end (settleTheNote) or as its driver hands it over (handTheSetupsOver), whatever its window did; set (not
    // empty) from its note on.
    bool noted = false;
    std::shared_ptr<void> underWay;
    std::optional<SetupOverrides::Earlier> before;
    qint64 lateUntil = 0;
    bool landed = false;
    // The controller only queued the change (a 202, or a task: D1): it may be made later than any time waited here;
    // and whether this press marked its record so (markQueued), which only a read of the server that saw this change
    // through takes off again (seenThrough).
    bool queued = false;
    bool markedQueued = false;
    // ...the change's answer was lost, so that the controller may have queued it, and is marked so (a 202 that never
    // came); and the mark could not be written down beside the settings (outside review of 0.2.0, area 5, MEDIUM 5.9).
    bool answerLost = false;
    bool markNotWritten = false;
    // The change is out, its answer not come yet: its record is marked as one the controller may queue (markTheQueue,
    // before it went), and a hand-over says so (outside review 2, 3.5).
    bool changeOut = false;
    // Its task was seen under way at a look of this press's: only an end seen after that is surely this change's (an
    // old task the answer names, ended before the change, is seen ended at once: outside review of D1, H1); and such
    // an end seen, done or not done. That end is the controller's word alone, and takes no mark off: a board that
    // stages the change for the next boot may say its task is done before the server shows it (outside review of D1,
    // round 2, H1).
    bool taskSeenUnderWay = false;
    bool taskEnded = false;
    // ...and a failure its task, or its monitor, said at its first look: no end surely this change's, but said, and
    // with it the server is restarted only on a read that shows the change made (round 2, L2; round 3, L6 b).
    std::optional<SaidAtOnce> failedAtOnce;
    // What the reads of the server's own computer showed after the change went out: the change made (the override
    // "Once", "BiosSetup"), at any read; at a read after its task's end; and the override taken ("Disabled") at a read
    // after the restart. And whether a read before the change already showed it "Once", "BiosSetup" (set by hand, or
    // left by an earlier press): then no read after it shows this change made (outside review of D1, round 2, L3).
    bool shownMade = false;
    bool shownAfterTheEnd = false;
    bool taken = false;
    bool shownBefore = false;
    // A read after the change showed qKVM's own override kept for every boot ("Continuous", "BiosSetup": a board that
    // keeps "Once" so): no other tool's, never taken by a boot, and cleared by the press (the owner's decision of
    // 2026-11-18, b).
    bool forEveryBoot = false;
    std::optional<qint64> seenOffAt;
    // The press ended as a start into BIOS setup should (done, not refused, timed out or cut off).
    bool endedWell = false;

    // A read of the computer after the change went out shows it made (this change's only where it did not show so
    // before the press).
    void seenSet()
    {
        landed = true;
        if (!shownBefore) {
            shownMade = true;
            shownAfterTheEnd = shownAfterTheEnd || taskEnded;
        }
    }
    // Whether reads of the server saw this change through, so that nothing of it can come any more: shown made after
    // its task's end, or shown made and then taken by the restart. Nothing else lets a queued mark of this press's go,
    // the task's word least of all (outside review of D1, round 2, H1).
    bool seenThrough() const { return shownAfterTheEnd || (shownMade && taken); }
};

void RedfishDevice::startIntoSetup(const std::shared_ptr<Pressing>& pressing)
{
    auto setup = std::make_shared<Setup>();
    setup->pressing = pressing;
    setup->restartBy = now() + m_pace.setupWithinMs;
    setup->endsAt = now() + m_pace.setupPatienceMs;
    m_setups.append(setup);
    // (Decided on its controller's list of computers read for it, as a button's press: outside review 2, 3.2 and 3.3.
    // Its restart and its clearing, later, each read it anew where the server gives no number: Ask::listedFrom.)
    setup->listedFrom = now();
    find(true, [this, setup](const base::Result<Places>& places) {
        if (!places) {
            setupOver(setup, onlyRead(places.error()));
            return;
        }
        setup->place = places.value().system;
        setupRead(setup, false);
    });
}

void RedfishDevice::setupRead(const std::shared_ptr<Setup>& setup, bool meant)
{
    // Which server answers, asked before anything is sent to it: the change goes under the session it was read under.
    checkedRead(
        setup->place, true, [this, setup, meant](const base::Result<QJsonObject>& computer, const Checked& checked) {
            if (!computer) {
                setupOver(setup, onlyRead(computer.error()));
                return;
            }
            const QJsonObject& system = computer.value();
            if (!redfish::bootTargets(system).contains(u"BiosSetup"_s)) {
                setupOver(setup,
                          base::Error{"invalid", "This controller does not offer starting into the BIOS setup (it "
                                                 "allows no 'BiosSetup' boot override)"});
                return;
            }
            // One that says which kinds of override it allows, and no "Once" among them, would keep it for every boot:
            // each later boot would stop in the setup. Refused, with nothing sent (the owner's decision of
            // 2026-11-18, a). (One that says nothing of them is asked as ever; one that keeps "Once" as "Continuous"
            // all the same has qKVM's own override cleared: clearTheOverride.)
            if (const std::optional<QStringList> kinds = redfish::overrideKinds(system);
                kinds && !kinds->contains(ours0)) {
                setupOver(setup, base::Error{"invalid", "Nothing was done: this controller allows no boot override for "
                                                        "the next boot only ('Once'), so a start into BIOS setup would "
                                                        "leave every later boot in the setup too."});
                return;
            }
            // Another tool's override (a provisioning tool's network boot, an override for every boot) is not replaced.
            if (const QString other = redfish::anotherOverride(system); !other.isEmpty()) {
                setupOver(setup,
                          base::Error{"action_failed", u"Nothing was done: the controller has a boot override set "
                                                       u"already (%1). Clear it first."_s.arg(other)
                                                           .toStdString()});
                return;
            }
            // Nor is one whose state it does not say (no BootSourceOverrideEnabled, a null, no text) taken for none:
            // another tool's may stand there (outside review of 0.2.0, area 5, MEDIUM 5.8).
            if (!redfish::overrideOff(system).has_value()) {
                setupOver(setup, base::Error{"action_failed",
                                             "Nothing was done: the controller does not say whether a boot override is "
                                             "set (its BootSourceOverrideEnabled). Look at the server's boot override "
                                             "in its BMC."});
                return;
            }
            // Switching on, shutting down, or not said: neither "switch on" nor "restart" is sure to be right.
            const QString power = kvmd::plain(system.value("PowerState"_L1), 20);
            if (power != "On"_L1 && power != "Off"_L1 && power != "Paused"_L1) {
                setupOver(setup,
                          base::Error{"action_failed",
                                      (power.isEmpty() ? u"Nothing was done: the server is in a state its "
                                                         u"controller does not say. Try again in a moment."_s
                                                       : u"Nothing was done: the server is changing its state just "
                                                         u"now (%1). Try again in a moment."_s.arg(power))
                                          .toStdString()});
                return;
            }
            setup->running = power != "Off"_L1;
            setup->lastPower = power;
            // (Ours already, before anything is sent: whatever shows it after the change is no sign of that change.)
            setup->shownBefore =
                setup->shownBefore || redfish::bootOverride(system) == std::pair<QString, QString>(ours0, ours1);
            const QStringList wanted = setup->running
                                           ? QStringList{u"ForceRestart"_s, u"GracefulRestart"_s, u"PowerCycle"_s}
                                           : QStringList{u"On"_s};
            const auto choose = [this, setup, wanted, mark = checked.mark, meant](const QString& target,
                                                                                  const QStringList& kinds) {
                const QString kind = redfish::kindToSend(wanted, kinds);
                if (kind.isEmpty()) {
                    // Nothing is set where it could not be carried out: the server is left as it is.
                    setupOver(setup,
                              base::Error{"action_failed", setup->running ? "This controller does not offer a "
                                                                            "restart from the state it is in"
                                                                          : "This controller does not offer "
                                                                            "switching on from the state it is in"});
                    return;
                }
                setup->resetTarget = target;
                setup->resetKind = kind;
                if (now() >= setup->restartBy) {
                    setupOver(setup, base::Error{"action_failed", u"Nothing was done: the controller took more than %1 "
                                                                  u"seconds."_s.arg(m_pace.setupWithinMs / 1000)
                                                                      .toStdString()});
                    return;
                }
                setTheOverride(setup, mark, meant);
            };
            const redfish::Offered offered = redfish::offeredKinds(system, "#ComputerSystem.Reset"_L1);
            QString target = offered.target;
            if (target.isEmpty()) {
                QString base = setup->place;
                while (base.endsWith(u'/')) {
                    base.chop(1);
                }
                target = base + u"/Actions/ComputerSystem.Reset"_s;
            }
            if (offered.kinds || offered.described.isEmpty()) {
                choose(target, offered.kinds.value_or(QStringList()));
                return;
            }
            read(offered.described, false, [choose, target](const base::Result<QJsonObject>& info) {
                choose(target, info ? redfish::kindsIn(info.value()) : QStringList());
            });
        });
}

void RedfishDevice::setTheOverride(const std::shared_ptr<Setup>& setup, const QByteArray& mark, bool meant)
{
    Ask change;
    change.method = "PATCH"_ba;
    change.path = setup->place;
    change.body = QJsonDocument(QJsonObject{{u"Boot"_s, QJsonObject{{u"BootSourceOverrideEnabled"_s, u"Once"_s},
                                                                    {u"BootSourceOverrideTarget"_s, u"BiosSetup"_s}}}})
                      .toJson(QJsonDocument::Compact);
    change.bodyIs = "application/json"_ba;
    change.doesSomething = true;
    change.command = true;
    change.decidedFor = setup->place;
    change.listedFrom = setup->listedFrom;
    change.quietMs = m_pace.bootQuietMs;
    // (Where a controller that queues the change names the task's monitor: its place as sent, to be followed, D1.)
    change.headers = {"Location"_ba};
    change.headersAsSent = {"Location"_ba};
    // A controller wants to be shown the state the change was decided on; one that will not take the mark it gave
    // (or gave none) is told that the change is meant whatever the state, once the computer was read and looked at
    // again (setupRead). Where that read gives another mark than the one refused, the state changed meanwhile: shown
    // that one, so that a change made between this read and this change is refused again, not written over; "*" only
    // where it refuses the very mark it gives, gives none, or refused twice (outside review of 0.2.0, area 5,
    // MEDIUM 5.8).
    const bool whatever = meant && (mark.isEmpty() || mark == setup->refused || setup->refusals > 1);
    const QByteArray shown = whatever ? "*"_ba : mark;
    if (!shown.isEmpty()) {
        change.sent = {{"If-Match"_ba, shown}};
    }
    // From here on what comes may have set it: it may be set from the moment it can go out (a driver let go of has it
    // cleared on the way out), and it is noted beside the settings before it goes (review 4's MEDIUM 1). Where that
    // note cannot be written, it does not go: a program that then ended would know nothing of it (outside review of
    // 0.1.2, C6). The press ends there, with nothing changed on the server; what stood before stands (settleTheNote).
    setup->lateUntil = now() + m_pace.bootQuietMs + m_pace.transferMs;
    const SetupOverrides::Noted noted = noteMayBeSet(setup);
    if (noted.full) {
        // (No note is let go of to make room for it: the merged review of 0.2.0, A-L1.)
        setup->maySet = false;
        setupOver(setup,
                  base::Error{"too_many",
                              "qKVM did not ask the server, and nothing was changed on it: qKVM keeps notes of at most "
                              "256 servers whose BIOS setup boot override may be set, it keeps 256, and it lets none "
                              "of them go to make room, as each card says its server's override may still be set. A "
                              "note goes once a look shows its server's override off, once a start into BIOS setup "
                              "on its server ends as it should, or when its card is removed. Look at those servers' "
                              "cards, and try again."});
        return;
    }
    if (!noted.written) {
        setup->maySet = false;
        setupOver(setup, noteNotSaved());
        return;
    }
    // Its record marked as one the controller may queue, and written down, as the change is handed to its line (after
    // any sign-in, or reading of which server answers, it waits for; each time it goes, after a 401 too): a program
    // that ends while its answer is out (quit, a crash, Windows' restart) leaves it so, and no look lets it go, as at a
    // 202 (outside review 2, 3.5: an ordinary note stood, which a look after its doubt let go of, and a change the
    // controller queued landed at a later boot with no warning left; and its review's L2: marked before the change
    // left). Its answer takes this press's mark off again where it says that the change was not queued. Where the mark
    // cannot be written down, the change does not go, as where its note cannot be (C6; review L3).
    change.goingOut = [this, setup]() -> std::optional<base::Error> {
        markTheQueue(setup);
        if (setup->markedQueued && stillUnwritten(overrides(), m_where.address, m_machine)) {
            unmarkTheQueue(setup);
            return noteNotSaved();
        }
        setup->changeOut = true;
        return std::nullopt;
    };
    sentFor(setup->pressing);
    setup->maySet = true;
    request(change, [this, setup, whatever, shown](const base::Result<Reply>& reply) {
        if (setup->over) {
            return;
        }
        setup->changeOut = false;
        if (!reply) {
            if (!mayHaveBeenDone(reply.error())) {
                // (It never went out: nothing was set.)
                setup->maySet = false;
                unmarkTheQueue(setup);
                setupOver(setup, base::Error{reply.error().code, "Nothing was done: the change did not reach the "
                                                                 "controller (" +
                                                                     reply.error().message + ")."});
                return;
            }
            setup->maySet = true;
            // Its answer lost: the controller may have only queued it (a 202 that never came), and a queued change may
            // be made after any look. Marked so at once, as at a 202 (outside review of 0.2.0, area 5, MEDIUM 5.9: only
            // an ordinary note stood, and a look after its doubt let it go). No read follows that could see it through:
            // the mark stays, as D1 has it.
            setup->queued = true;
            setup->answerLost = true;
            markTheQueue(setup);
            clearTheOverride(setup, [this, setup, why = reply.error()](const std::optional<QString>& left) {
                setupOver(setup, leftAs(u"Nothing was restarted: the controller's answer to the BIOS setup boot "
                                        u"override did not come or could not be read (%1)"_s.arg(
                                            QString::fromStdString(why.message)),
                                        left));
            });
            return;
        }
        const int status = reply.value().status;
        if ((status == 412 || status == 428) && !whatever) {
            // The state changed since it was read, or the controller wants a mark it did not give: nothing was set.
            // Read again, and looked at again (another tool's override, or a power change meanwhile, is not gone
            // over), then sent again, as above.
            setup->maySet = false;
            unmarkTheQueue(setup);
            setup->refused = shown;
            ++setup->refusals;
            setupRead(setup, true);
            return;
        }
        if (status >= 400 && status < 500) {
            // Refused for certain: nothing was set.
            setup->maySet = false;
            unmarkTheQueue(setup);
            const base::Result<void> verdict = redfish::verdictOn(status, reply.value().body);
            setupOver(setup,
                      verdict ? base::Error{"action_failed", "The controller refused the change"} : verdict.error());
            return;
        }
        setup->maySet = true;
        // A change the controller only queued (a 202, the monitor of a task named, or a Task as the answer): it may be
        // made later than any time this press waits. Its server's record is marked so now, and written down at once,
        // before anything else of the press (a program ended by force then leaves it marked): no look lets it go any
        // more, only reads of this press that see the change through (seenThrough), a later start into BIOS setup that
        // ends as it should, or the server taken off the wall (outside review's confirm round of 0.1.2, D1). Read back
        // as ever; and where it is not seen through, told. (A header asked for is there, empty, where the answer has
        // none.)
        setup->queued = setup->queued || status == 202 || !reply.value().headers.value("location").isEmpty() ||
                        aTask(QJsonDocument::fromJson(reply.value().body).object());
        if (setup->queued) {
            markTheQueue(setup);
        } else {
            unmarkTheQueue(setup); // (answered, and not as queued: the mark it went with comes off)
        }
        if (status != 200 && status != 202 && status != 204) {
            QString said = redfish::errorSaid(reply.value().body);
            if (said.isEmpty()) {
                said = u"HTTP %1"_s.arg(status);
            }
            const QString how = status >= 500 ? u"an error of its own"_s : u"an answer a change does not have"_s;
            clearTheOverride(setup, [this, setup, how, said](const std::optional<QString>& left) {
                setupOver(setup, leftAs(u"Nothing was restarted: the controller answered the BIOS setup boot override "
                                        u"with %1 (%2)"_s.arg(how, said),
                                        left));
            });
            return;
        }
        followUp();
        // A 204 is the standard's word that the change was made in full; a 200 shows the computer as it now is. A 202
        // (accepted, to be done), or a 200 that does not show it, is read back until it shows.
        if (status == 204 ||
            (status == 200 && redfish::bootOverride(QJsonDocument::fromJson(reply.value().body).object()) ==
                                  std::pair<QString, QString>(ours0, ours1))) {
            setup->landed = true;
            restartIntoSetup(setup);
            return;
        }
        // Its task, where it names one, followed to its end first (D1, as the web follows AMI's mounts): for its own
        // time and so many looks, and never into the read-back's time before the restart's, which the press keeps
        // for after the follow (outside review of D1, M1: a task that does not end used up the whole window, and a
        // press 0.1.2 completed failed).
        // (Its Location as it was sent: the place's case is the controller's.)
        if (setup->queued) {
            const qint64 until = std::min(now() + m_pace.taskWithinMs, setup->restartBy - m_pace.confirmWithinMs);
            if (const QString task =
                    redfish::taskPlace(reply.value().headersAsSent.value("location"), reply.value().body);
                !task.isEmpty() && now() < until) {
                followTheTask(setup, task, until, m_pace.mostTaskLooks);
                return;
            }
        }
        lookForTheOverride(setup, std::min(now() + m_pace.confirmWithinMs, setup->restartBy));
    });
}

void RedfishDevice::followTheTask(const std::shared_ptr<Setup>& setup, const QString& place, qint64 until, int looks)
{
    // Before each look the computer is read, before the first too: a change it shows made goes on as after a 202, the
    // server restarted into its setup as 0.1.2 restarted it, however long the task runs on (a board's task that ends
    // only at the next boot, as pending settings do: outside review of D1, M1), and at once, whatever the task's place
    // does (a monitor that never answered held the first read back by a look's whole time: round 2, L1). Its mark
    // stays until the restart is seen to take the override (seenThrough): the task's end was not seen. The read, as
    // each look, is held to the follow's time, so that a slow or a silent one takes none of the read-back's (round 2,
    // L1): a sign-in, the same read asked again after a 401, and the judge's own reads too (outside review of the fuzz
    // work, round 3, L3).
    checkedRead(setup->place, false, until,
                [this, setup, place, until, looks](const base::Result<QJsonObject>& read, const Checked&) {
                    if (setup->over) {
                        return;
                    }
                    if (read && redfish::setupForEveryBoot(read.value())) {
                        keptForEveryBoot(setup);
                        return;
                    }
                    // (Not where it showed so before the press: that says nothing of this change, and its task is
                    // followed to its end first, round 2, L3.)
                    const bool shows =
                        read && redfish::bootOverride(read.value()) == std::pair<QString, QString>(ours0, ours1);
                    if (shows && !setup->shownBefore) {
                        setup->seenSet();
                        restartIntoSetup(setup);
                        return;
                    }
                    setup->seenUnset = setup->seenUnset || (read.ok() && !shows);
                    if (now() >= until) {
                        // (Its time over: not followed to its end, and read back as 0.1.2 has it.)
                        lookForTheOverride(setup, std::min(now() + m_pace.confirmWithinMs, setup->restartBy));
                        return;
                    }
                    lookAtTheTask(setup, place, until, looks);
                });
}

void RedfishDevice::lookAtTheTask(const std::shared_ptr<Setup>& setup, const QString& place, qint64 until, int looks)
{
    Ask look;
    look.path = place;
    look.vital = false; // (a task not answered for leaves the controller as it was)
    // A place the account may be refused under a session the controller honours (a monitor bound to the session that
    // made it, or of other rights): its 401 is the place refused, not the session, which is kept, with no second
    // sign-in and no hold on the controller's sign-ins in the middle of the press; the task is then one that cannot be
    // followed (outside review of D1, M3).
    look.detail = true;
    // (Its answer whole within the follow's time, a sign-in or the look asked again after a 401 too: one cut there, or
    // answered as too late, is one not to be read. Outside review of the fuzz work, round 3, L3.)
    look.by = until;
    request(look, [this, setup, place, until, looks](const base::Result<Reply>& reply) {
        if (setup->over) {
            return;
        }
        using State = redfish::TaskSaid::State;
        const redfish::TaskSaid said =
            reply ? redfish::taskOf(place, reply.value().status, reply.value().body, setup->place)
                  : redfish::TaskSaid{State::Unreadable, {}};
        // Ended, done or not done, and surely this change's task: seen under way at a look before, and of no other
        // change (taskOf). That is the controller's word alone: it takes no mark off (a board that stages the change
        // for the next boot may say done before the server shows it); only a read after it that shows the change made
        // says that nothing of it can come any more (seenThrough, outside review of D1, round 2, H1). An end seen at
        // once is no such end: an old task the answer names, or a monitor's passing error before it; it is as one that
        // cannot be followed (outside review of D1, H1).
        switch (said.state) {
        case State::Done:
            if (!setup->taskSeenUnderWay) {
                break;
            }
            // Done, by the controller's word: read back as a 202 is in 0.1.2, the server restarted once a read shows
            // the override set (and the mark taken off by that read), and also where no read of the computer came back
            // at all (the press's own, as after a 202, its mark kept); a read that shows it unset restarts nothing, the
            // override is cleared all the same, and the mark stays (outside review of D1, L7; round 2, H1).
            setup->taskEnded = true;
            lookForTheOverride(setup, std::min(now() + m_pace.confirmWithinMs, setup->restartBy));
            return;
        case State::Failed:
            if (!setup->taskSeenUnderWay) {
                // (At its first look: perhaps an old task's, so read back as one that cannot be followed; but its words
                // are said, and no blind restart follows them: outside review of D1, round 2, L2.)
                setup->failedAtOnce = SaidAtOnce{false, said.words};
                break;
            }
            // Not made, in the controller's words; cleared all the same, as every way out of a press is. Its mark
            // stays: only a read can tell that nothing of the change is still to come.
            setup->taskEnded = true;
            failedAfterAll(setup,
                           u"the controller's task did not set the BIOS setup boot override (%1)"_s.arg(
                               said.words.isEmpty() ? u"it gave no reason"_s : said.words),
                           u"The controller's task did not set the BIOS setup boot override (%1), but the server "
                           u"showed it set from before the press, and was restarted into its setup."_s.arg(
                               said.words.isEmpty() ? u"it gave no reason"_s : said.words));
            return;
        case State::Refused:
            if (!setup->taskSeenUnderWay) {
                // (At its first look, a monitor's error, a 405 from one that takes no GET, say: as a task's failure
                // said then, read back with its words said, and no blind restart: outside review of the fuzz work,
                // round 3, L6 b. 0.1.2 read back here.)
                setup->failedAtOnce = SaidAtOnce{true, said.words};
                break;
            }
            // Its monitor answered with an error: the change's own answer, as DMTF has a monitor's last, and as AMI's
            // real board answered as its task failed. Nothing is restarted and the override is cleared, in the
            // controller's words; but an error at a monitor is no task's end for certain, so the mark stays and the
            // queue is said (outside review of D1, H1).
            failedAfterAll(setup,
                           u"the controller answered for its task with an error (%1)"_s.arg(
                               said.words.isEmpty() ? u"it gave no reason"_s : said.words),
                           u"The controller answered for its task with an error (%1), but the server showed the BIOS "
                           u"setup boot override set from before the press, and was restarted into its setup."_s.arg(
                               said.words.isEmpty() ? u"it gave no reason"_s : said.words));
            return;
        case State::UnderWay:
            setup->taskSeenUnderWay = true;
            [[fallthrough]];
        case State::Passing:
            // (Under way, or a passing error that says nothing of it: looked at again within the time and the looks,
            // the computer read before it.)
            if (looks > 1 && now() < until) {
                QTimer::singleShot(m_pace.confirmEveryMs, this, [this, setup, place, until, looks] {
                    followTheTask(setup, place, until, looks - 1);
                });
                return;
            }
            break;
        case State::Unreadable:
            break;
        }
        // Not to be followed to its end (not to be read, an end not surely this change's, or not ended in time or in
        // its looks): queued as 0.1.2 has it, read back and told, its record marked.
        lookForTheOverride(setup, std::min(now() + m_pace.confirmWithinMs, setup->restartBy));
    });
}

void RedfishDevice::lookForTheOverride(const std::shared_ptr<Setup>& setup, qint64 until)
{
    checkedRead(setup->place, false, [this, setup, until](const base::Result<QJsonObject>& read, const Checked&) {
        if (setup->over) {
            return;
        }
        if (read && redfish::bootOverride(read.value()) == std::pair<QString, QString>(ours0, ours1)) {
            setup->seenSet();
            // (A failure its task said at its first look is said beside the restart: outside review of the fuzz work,
            // round 3, L2.)
            if (setup->failedAtOnce) {
                setup->restartNote = setup->failedAtOnce->restarted();
            }
            restartIntoSetup(setup);
            return;
        }
        if (read && redfish::setupForEveryBoot(read.value())) {
            keptForEveryBoot(setup);
            return;
        }
        setup->seenUnset = setup->seenUnset || read.ok();
        setup->seenUnsetAfterTheEnd = setup->seenUnsetAfterTheEnd || (setup->taskEnded && read.ok());
        if (now() < until) {
            QTimer::singleShot(m_pace.confirmEveryMs, this, [this, setup, until] { lookForTheOverride(setup, until); });
            return;
        }
        // (A failure its task or its monitor said at its first look, in its words: perhaps not this change's, and said
        // as such.)
        const QString atOnce = setup->failedAtOnce ? setup->failedAtOnce->clause() : QString();
        // (After its task said done, only a read after that end says the server does not show it: one while the task
        // ran showed it unset as it should, outside review of the fuzz work, round 3, L1.)
        if (setup->taskEnded ? setup->seenUnsetAfterTheEnd : setup->seenUnset) {
            // It took the change and does not show it: no restart, since it would not land in the setup, and the
            // override cleared, in case it is made later. (After its task said done: the two disagree, and the server's
            // own read is believed; the mark stays, outside review of D1, round 2, H1.)
            clearTheOverride(setup, [this, setup, atOnce](const std::optional<QString>& left) {
                const QString lead = setup->taskEnded ? u"Nothing was restarted: the controller's task says it is "
                                                        u"done, but the server does not show the BIOS setup boot "
                                                        u"override set"_s
                                                      : u"Nothing was restarted: the controller took the BIOS setup "
                                                        u"boot override and does not show it set"_s;
                setupOver(setup, leftAs(atOnce.isEmpty() ? lead : lead + u"; "_s + atOnce, left));
            });
            return;
        }
        if (setup->failedAtOnce) {
            // Never read back, and its task said at once that it failed: whatever task that was, the server is not
            // restarted on the strength of nothing (outside review of D1, round 2, L2). Cleared, as every way out is.
            clearTheOverride(setup, [this, setup](const std::optional<QString>& left) {
                setupOver(setup, leftAs(setup->failedAtOnce->unread(), left));
            });
            return;
        }
        // (Never read back: the controller said yes to the change, and the restart is what was asked for.)
        restartIntoSetup(setup);
    });
}

void RedfishDevice::failedAfterAll(const std::shared_ptr<Setup>& setup, const QString& failure, const QString& note)
{
    // An override shown before the press (set by hand in the BMC, or left by an earlier press) says nothing of this
    // change, and the failure of its task nothing of it: read once more. Where it still shows, it stands as it stood,
    // and the server is restarted into it, as 0.1.2 restarted it at its first read and as was asked, within the press's
    // two minutes (restartIntoSetup), the failure said beside it; its mark stays, as no read can show this change made
    // (outside review of the fuzz work, round 3, L6 a). Otherwise nothing is restarted, and it is cleared, as every way
    // out of a press is.
    const auto cleared = [this, setup, failure] {
        clearTheOverride(setup, [this, setup, failure](const std::optional<QString>& left) {
            setupOver(setup, leftAs(u"Nothing was restarted: "_s + failure, left));
        });
    };
    if (!setup->shownBefore) {
        cleared();
        return;
    }
    checkedRead(setup->place, false,
                [this, setup, note, cleared](const base::Result<QJsonObject>& read, const Checked& /*under*/) {
                    if (setup->over) {
                        return;
                    }
                    if (read && redfish::bootOverride(read.value()) == std::pair<QString, QString>(ours0, ours1)) {
                        setup->restartNote = note;
                        restartIntoSetup(setup);
                        return;
                    }
                    cleared();
                });
}

void RedfishDevice::keptForEveryBoot(const std::shared_ptr<Setup>& setup)
{
    // Read back before the restart as qKVM's own override kept for every boot: made, but not as asked, and every later
    // boot would stop in the setup. Nothing is restarted on it, and it is cleared at once, and said (the owner's
    // decision of 2026-11-18, b). Its mark, where the change was only queued, stays: no read saw a one-time override.
    setup->forEveryBoot = true;
    setup->landed = true;
    clearTheOverride(setup, [this, setup](const std::optional<QString>& left) {
        setupOver(setup, leftAs(u"Nothing was restarted: the controller set the BIOS setup boot override for every "
                                u"boot ('Continuous') in place of the next one only"_s,
                                left));
    });
}

void RedfishDevice::restartIntoSetup(const std::shared_ptr<Setup>& setup)
{
    // Too late for a restart: whoever pressed may have stopped waiting for it.
    const auto tooLateFor = [this, setup] {
        clearTheOverride(setup, [this, setup](const std::optional<QString>& left) {
            setupOver(setup, leftAs(u"Nothing was restarted: the controller took more than %1 seconds before the "
                                    u"restart"_s.arg(m_pace.setupWithinMs / 1000),
                                    left));
        });
    };
    if (now() >= setup->restartBy) {
        tooLateFor();
        return;
    }
    Ask command;
    command.method = "POST"_ba;
    command.path = setup->resetTarget;
    command.body = QJsonDocument(QJsonObject{{u"ResetType"_s, setup->resetKind}}).toJson(QJsonDocument::Compact);
    command.bodyIs = "application/json"_ba;
    command.doesSomething = true;
    command.command = true;
    // (Some time after the press began: where the server gives no number, its controller's list is read anew before it
    // goes. Outside review 2, 3.3.)
    command.decidedFor = setup->place;
    command.quietMs = m_pace.bootQuietMs;
    // (Held to the same time when it goes: one that waits for a sign-in, or for which server answers to be read again,
    // or is asked again after a 401, is not sent once that time is over. Outside review of the fuzz work, round 3, L4.)
    command.by = setup->restartBy;
    request(command, [this, setup, tooLateFor](const base::Result<Reply>& reply) {
        if (setup->over) {
            return;
        }
        if (!reply && reply.error().code == tooLateCode) {
            tooLateFor(); // (never sent)
            return;
        }
        if (!reply && !mayHaveBeenDone(reply.error())) {
            // It never went out: the override is cleared again, so that no ordinary boot lands in the setup.
            clearTheOverride(setup, [this, setup, why = reply.error()](const std::optional<QString>& left) {
                setupOver(setup, unset(why, left));
            });
            return;
        }
        if (reply && reply.value().status >= 400 && reply.value().status < 500) {
            const base::Result<void> verdict = redfish::verdictOn(reply.value().status, reply.value().body);
            clearTheOverride(
                setup,
                [this, setup, why = verdict ? base::Error{"action_failed", "The controller refused"} : verdict.error()](
                    const std::optional<QString>& left) { setupOver(setup, unset(why, left)); });
            return;
        }
        // Taken, or perhaps taken (no answer, an error of the controller's own): the server may be starting into its
        // setup now. Watched until it has taken the override, and the override cleared at the press's end where it
        // has not.
        setup->restarted = true;
        followUp();
        watchTheStart(setup);
    });
}

void RedfishDevice::watchTheStart(const std::shared_ptr<Setup>& setup)
{
    if (now() >= setup->endsAt) {
        setupEnds(setup);
        return;
    }
    QTimer::singleShot(
        std::min<qint64>(m_pace.confirmEveryMs, std::max<qint64>(0, setup->endsAt - now())), this, [this, setup] {
            checkedRead(setup->place, false, [this, setup](const base::Result<QJsonObject>& read, const Checked&) {
                if (setup->over) {
                    return;
                }
                if (read) {
                    setup->lastPower = kvmd::plain(read.value().value("PowerState"_L1), 20);
                    setup->powerAfter = setup->lastPower;
                    // Taken as it started: the controller clears a one-time override at the boot that uses it. (Only
                    // where it says "Disabled": a read that shows no override says nothing of it, and the start is
                    // watched on, review 4's MEDIUM 2. And only of a server seen on: one switched on that stays off
                    // has not started into anything, whatever its controller did with the override, and is watched
                    // on, to be said still off at the end: power is checked, not believed. Outside review of 0.2.0,
                    // area 5, MEDIUM 5.7.)
                    if (redfish::overrideOff(read.value()) == std::optional<bool>(true) &&
                        setup->lastPower == "On"_L1) {
                        setup->seenOffAt = now();
                        setup->taken = true;
                        setupOver(setup, Done{});
                        return;
                    }
                    if (redfish::setupForEveryBoot(read.value())) {
                        // qKVM's own override, kept for every boot (a board that keeps "Once" so): no other tool's,
                        // and no boot takes it. Watched on to the press's end, so that the server has its time to
                        // start into its setup with it, and cleared then (setupEnds; the owner's decision of
                        // 2026-11-18, b).
                        setup->forEveryBoot = true;
                    } else if (const QString other = redfish::anotherOverride(read.value());
                               !other.isEmpty() && (setup->running || setup->lastPower != "Off"_L1)) {
                        // (Not of a server switched on that is still off: it has not started at all, whatever the
                        // override, and is watched on, to be said still off at the end, that override left as it is
                        // then too. Outside review 2, 3.6: done, whatever its power.)
                        setup->seenOffAt = now();
                        setupOver(setup, Done{u"the boot override is another tool's now "
                                              u"(%1), and was left as it is"_s.arg(other)});
                        return;
                    }
                }
                watchTheStart(setup);
            });
        });
}

void RedfishDevice::setupEnds(const std::shared_ptr<Setup>& setup)
{
    clearTheOverride(setup, [this, setup](const std::optional<QString>& left) {
        const int minutes = std::max(1, m_pace.setupPatienceMs / 60'000);
        const bool stillOff = !setup->running && setup->powerAfter == "Off"_L1;
        if (stillOff) {
            // Switched on and still off: said so, whatever became of the override, kept for every boot by its
            // controller or another tool's (outside review 2, 3.6: "switched on" was said of a server that stayed off).
            QString lead =
                u"The controller accepted switching on, but the server is still off after %1 minutes"_s.arg(minutes);
            if (setup->forEveryBoot) {
                lead += u", and its controller kept the override for every boot ('Continuous') in place of the next "
                        u"one only"_s;
            }
            if (!left) {
                setupOver(setup, base::Error{"action_failed",
                                             (lead + u"; the BIOS setup boot override was cleared again, so its next "
                                                     u"boot is an ordinary one. Look at the server."_s)
                                                 .toStdString()});
            } else if (*left == notCleared()) {
                setupOver(setup,
                          base::Error{"timeout", (lead + u"; "_s + *left + u". "_s + lookAgain()).toStdString()});
            } else {
                setupOver(setup, base::Error{"action_failed",
                                             (lead + u"; "_s + *left + u". Look at the server."_s).toStdString()});
            }
            return;
        }
        if (setup->forEveryBoot) {
            // (qKVM's own override kept for every boot, cleared now that the server had its time to start into its
            // setup with it: the owner's decision of 2026-11-18, b.)
            const QString lead =
                u"The server was %1 with its BIOS setup boot override set, but its controller kept the "
                u"override for every boot ('Continuous') in place of the next one only"_s.arg(
                    setup->running ? u"restarted"_s : u"switched on"_s);
            if (!left) {
                setupOver(setup,
                          Done{lead + u"; qKVM cleared it again, so the server's later boots are ordinary ones."_s});
                return;
            }
            setupOver(setup, base::Error{"timeout", (lead + u"; "_s + *left + u". "_s + lookAgain()).toStdString()});
            return;
        }
        const QString lead =
            setup->running
                ? u"The server was told to restart, and was not seen to start into its BIOS setup within %1 minutes"_s
                      .arg(minutes)
                : u"The server was told to switch on, and was not seen to start into its BIOS setup within %1 "
                  u"minutes"_s.arg(minutes);
        if (!left) {
            // ("Not carried out" only where a look came back and showed it as it was, still off, above; otherwise it
            // may have been.)
            setupOver(setup, base::Error{"timeout", (lead + u"; the BIOS setup boot override was cleared again, so "
                                                            u"its next boot is an ordinary one. Look at the server."_s)
                                                        .toStdString()});
            return;
        }
        setupOver(setup, base::Error{"timeout", (lead + u"; "_s + *left + u". "_s + lookAgain()).toStdString()});
    });
}

void RedfishDevice::clearTheOverride(const std::shared_ptr<Setup>& setup,
                                     std::function<void(std::optional<QString> left)> then)
{
    clearTheOverride(setup, QByteArray(), 0, std::move(then));
}

void RedfishDevice::clearTheOverride(const std::shared_ptr<Setup>& setup, const QByteArray& refused, int refusals,
                                     std::function<void(std::optional<QString> left)> then)
{
    // Read as the server it is: another one that took the address meanwhile is neither read for this one nor cleared.
    checkedRead(
        setup->place, true,
        [this, setup, refused, refusals, then = std::move(then)](const base::Result<QJsonObject>& read,
                                                                 const Checked& checked) mutable {
            if (!read) {
                then(notCleared());
                return;
            }
            if (setup->restarted) {
                setup->powerAfter = kvmd::plain(read.value().value("PowerState"_L1), 20);
            }
            if (const QString other = othersAfterTheChange(read.value()); !other.isEmpty()) {
                setup->seenOffAt = now(); // (the BIOS setup one is not set)
                then(u"the boot override is another tool's now (%1), and was left as it is"_s.arg(other));
                return;
            }
            // (Shown set: the change this press sent was made, and nothing of it can come after this where it
            // was not only queued. A queued one's mark is not taken off by this read: only by the reads that
            // came before the clearing, seenThrough. Kept for every boot, it is qKVM's own all the same, and
            // cleared: the owner's decision of 2026-11-18, b.)
            if (redfish::bootOverride(read.value()) == std::pair<QString, QString>(ours0, ours1)) {
                setup->landed = true;
            } else if (redfish::setupForEveryBoot(read.value())) {
                setup->landed = true;
                setup->forEveryBoot = true;
            }
            // (Sent also where it shows none: a change the controller accepted for later, or whose answer was
            // lost, may be made after this read, and the clearing is to come after it.)
            Ask change;
            change.method = "PATCH"_ba;
            change.path = setup->place;
            change.body =
                QJsonDocument(QJsonObject{{u"Boot"_s, QJsonObject{{u"BootSourceOverrideEnabled"_s, u"Disabled"_s}}}})
                    .toJson(QJsonDocument::Compact);
            change.bodyIs = "application/json"_ba;
            change.doesSomething = true;
            change.command = true;
            change.decidedFor = setup->place;
            change.quietMs = m_pace.bootQuietMs;
            // Refused for a mark that moved on since: shown the fresh one, so that another tool's override set
            // between this read and the clearing is refused again, not written over with "Disabled"; "*" only
            // where it refused the very mark it gives, gives none, or refused twice, as the change is sent again
            // (outside review of 0.2.0's Redfish fixes, L9: "*" after one refusal).
            const bool whatever = refusals > 0 && (checked.mark.isEmpty() || checked.mark == refused || refusals > 1);
            const QByteArray shown = whatever ? "*"_ba : checked.mark;
            if (!shown.isEmpty()) {
                change.sent = {{"If-Match"_ba, shown}};
            }
            request(change, [this, setup, whatever, shown, refusals, then](const base::Result<Reply>& reply) mutable {
                if (!reply) {
                    then(notCleared());
                    return;
                }
                const int status = reply.value().status;
                if ((status == 412 || status == 428) && !whatever) {
                    clearTheOverride(setup, shown, refusals + 1, std::move(then));
                    return;
                }
                // Read back, whatever it answered: only a read that shows it off counts as cleared (outside
                // review of 0.2.0, area 5, MEDIUM 5.7: a 204 counted, unread).
                if (status == 200 || status == 202 || status == 204) {
                    clearShown(setup, now() + m_pace.confirmWithinMs, std::move(then));
                    return;
                }
                then(notCleared());
            });
        });
}

void RedfishDevice::clearShown(const std::shared_ptr<Setup>& setup, qint64 until,
                               std::function<void(std::optional<QString> left)> then)
{
    checkedRead(
        setup->place, false,
        [this, setup, until, then = std::move(then)](const base::Result<QJsonObject>& read, const Checked&) mutable {
            if (read && setup->restarted) {
                setup->powerAfter = kvmd::plain(read.value().value("PowerState"_L1), 20);
            }
            if (read) {
                // (Cleared only where it says "Disabled": one that shows no override is read again within the time
                // left, then said to be maybe set, review 4's MEDIUM 2.)
                if (redfish::overrideOff(read.value()) == std::optional<bool>(true)) {
                    setup->seenOffAt = now();
                    then(std::nullopt);
                    return;
                }
                // (qKVM's own kept for every boot is not cleared yet: read again.)
                if (const QString other = othersAfterTheChange(read.value()); !other.isEmpty()) {
                    setup->seenOffAt = now();
                    then(u"the boot override is another tool's now (%1), and was left as it is"_s.arg(other));
                    return;
                }
            }
            if (now() >= until) {
                then(notCleared());
                return;
            }
            QTimer::singleShot(m_pace.confirmEveryMs, this, [this, setup, until, then = std::move(then)]() mutable {
                clearShown(setup, until, std::move(then));
            });
        });
}

bool RedfishDevice::anOverrideMayBeSet() const
{
    return std::ranges::any_of(m_setups, [](const std::shared_ptr<Setup>& setup) { return setup->maySet; });
}

void RedfishDevice::setupOver(const std::shared_ptr<Setup>& setup, const base::Result<Done>& result)
{
    if (std::exchange(setup->over, true)) {
        return;
    }
    m_setups.removeOne(setup);
    setup->endedWell = result.ok();
    settleTheNote(setup);
    // Said beside whatever came of it: the words of a failure said before the server was restarted (outside review of
    // the fuzz work, round 3, L2); and a change the controller only queued, that no read saw through: its mark stays.
    QStringList beside;
    if (setup->restarted && !setup->restartNote.isEmpty()) {
        beside.append(setup->restartNote);
    }
    if (setup->queued && setup->maySet && !setup->seenThrough()) {
        beside.append(setup->answerLost ? mayHaveQueuedSaid() : queuedSaid(setup->taskEnded));
    }
    // (And where that mark could not be written down, and stands so: outside review of 0.2.0, area 5, MEDIUM 5.9.)
    if (setup->markNotWritten && stillUnwritten(overrides(), m_where.address, m_machine)) {
        beside.append(markNotWrittenSaid());
    }
    // (And where its card asks now whether the computer at its place is its server: what was not sent since is why.)
    if (m_checkedAsking) {
        beside.append(asksSaid());
    }
    if (beside.isEmpty()) {
        pressed(setup->pressing, result);
        return;
    }
    const QString more = beside.join(u' ');
    if (result) {
        pressed(setup->pressing, Done{(result.value().note + u' ' + more).trimmed()});
    } else {
        pressed(setup->pressing, base::Error{result.error().code, result.error().message + ' ' + more.toStdString()});
    }
}

// --- what may be left, kept beside the settings (review 4's MEDIUM 1) --------------------------------------

SetupOverrides& RedfishDevice::overrides() const
{
    return m_pace.overrides ? *m_pace.overrides : SetupOverrides::program();
}

QDateTime RedfishDevice::wallAt(qint64 ms) const
{
    return QDateTime::currentDateTimeUtc().addMSecs(ms - now());
}

SetupOverrides::Noted RedfishDevice::noteMayBeSet(const std::shared_ptr<Setup>& setup)
{
    // Until the press can do nothing more: its end, the clearing then (a read and the change, each waited for as long
    // as the change), and a change still on its way after that.
    const qint64 last =
        std::max(setup->endsAt, setup->lateUntil) + (qint64{2} * m_pace.bootQuietMs) + m_pace.transferMs;
    const SetupOverrides::Noted noted = overrides().mayBeSet(m_where.address, m_machine, wallAt(last));
    if (!std::exchange(setup->noted, true)) {
        setup->before = noted.before;
        setup->underWay = overrides().pressUnderWay();
    }
    return noted;
}

void RedfishDevice::markTheQueue(const std::shared_ptr<Setup>& setup)
{
    // (Whether this press put the mark on: only that mark is taken off by its own reads, seenMade. And whether it
    // stands written down: a disk that filled since the note, or its folder taken away, leaves it in memory alone,
    // which the press says; it goes into the file with the next writing that works. Outside review of 0.2.0, 5.9. Only
    // of this press's own mark: one an earlier press put on, and wrote down, is in the file whatever this writing came
    // to, a file that could not be written over keeping what it had; and one that no record holds is nowhere to say
    // anything of. Outside review of 0.2.0's Redfish fixes, round 2, L4.)
    SetupOverrides& kept = overrides();
    const bool marked = kept.markQueued(m_where.address, m_machine);
    setup->markedQueued = marked || setup->markedQueued;
    // (This press's own mark, put on now or as its change went: outside review 2, 3.5.)
    setup->markNotWritten =
        setup->markNotWritten || (setup->markedQueued && stillUnwritten(kept, m_where.address, m_machine));
}

void RedfishDevice::unmarkTheQueue(const std::shared_ptr<Setup>& setup)
{
    // (Only the mark this press put on as its change went: one an earlier press put on stays, D1. Outside review
    // 2, 3.5.)
    if (std::exchange(setup->markedQueued, false)) {
        overrides().seenMade(m_where.address, m_machine);
    }
    setup->markNotWritten = false;
}

void RedfishDevice::settleTheNote(const std::shared_ptr<Setup>& setup)
{
    if (!setup->noted) {
        return;
    }
    SetupOverrides& kept = overrides();
    if (!setup->maySet) {
        // Nothing of this press's was set (refused for certain, or never sent): what stood before stands as it did, its
        // doubt on the clock it ran on (R3).
        if (setup->before) {
            kept.restore(m_where.address, m_machine, *setup->before);
        } else {
            static_cast<void>(kept.drop(m_where.address, m_machine));
        }
        return;
    }
    // Its change, which the controller only queued, seen through by reads of the server (seenThrough): nothing of it
    // can come any more, and the mark this press put on its record goes (never by its task's word: outside review of
    // D1, round 2, H1). The record stands as any press's does.
    if (setup->seenThrough() && std::exchange(setup->markedQueued, false)) {
        kept.seenMade(m_where.address, m_machine);
    }
    // It ended as a start into BIOS setup should, its change seen made and the override taken after it ("Disabled" read
    // after the restart; another tool's override then is no override taken: outside review of D1, round 2, L6): let go
    // of, a record marked queued too, of this press or of one before (D1); where this press's change was only queued,
    // only once reads saw it through.
    if (setup->endedWell && setup->landed && setup->taken && (!setup->queued || setup->seenThrough())) {
        kept.forget(m_where.address, m_machine);
        return;
    }
    // Seen off, and nothing of the change this press sent can come after that: let go of, but not one marked queued (a
    // queued change may be made after any look: D1). Else it stands, and a look lets it go once that change can no
    // longer come (the web's CLEARED_LATE: a change accepted, or whose answer was lost, may be made after the
    // clearing); its doubt, which alone holds an update, as for any press, until then.
    if (setup->seenOffAt && (setup->landed || *setup->seenOffAt >= setup->lateUntil) &&
        kept.drop(m_where.address, m_machine)) {
        return;
    }
    kept.settle(m_where.address, m_machine, wallAt(std::max(now(), setup->lateUntil)));
}

void RedfishDevice::lookedAtTheOverride(const QJsonObject& system)
{
    // (Off in so many words, or another tool's in its place: the BIOS setup one is not set. A read that shows none says
    // nothing, review 4's MEDIUM 2; nor does the BIOS setup one kept for every boot, which may be qKVM's own: the
    // owner's decision of 2026-11-18, b.)
    SetupOverrides& kept = overrides();
    if (kept.standing(m_where.address, m_machine) &&
        (redfish::overrideOff(system) == std::optional<bool>(true) ||
         (!redfish::anotherOverride(system).isEmpty() && !redfish::setupForEveryBoot(system)))) {
        kept.seenOff(m_where.address, m_machine, QDateTime::currentDateTimeUtc());
    }
}

bool RedfishDevice::handTheSetupsOver()
{
    SetupOverrides& kept = overrides();
    bool clearing = false;
    bool landed = true;
    qint64 lateUntil = 0;
    QString place;
    for (const std::shared_ptr<Setup>& setup : std::as_const(m_setups)) {
        setup->over = true; // (nothing of it runs on: its driver goes)
        if (!setup->maySet) {
            settleTheNote(setup);
            continue;
        }
        clearing = true;
        landed = landed && setup->landed;
        // (A change the controller only queued keeps its mark through a hand-over, whatever was read of it: the press
        // did not end, and no read after the restart was seen to take it. Said, as the card says it: outside review of
        // D1, round 2, H1. So does one whose answer is still out, marked as it went.)
        const bool queued = setup->queued;
        lateUntil = std::max(lateUntil, setup->lateUntil);
        place = setup->place;
        // Its press is told as it is (on the program's loop, as every press is: pressed): the override may be set, and
        // is cleared on the way out.
        std::string said = "The connection to the controller was let go of while the server was starting into its "
                           "BIOS setup: the BIOS setup boot override may be set, so the next boot may land in the BIOS "
                           "setup. It is cleared as the connection goes, and the server's card says so until a look "
                           "shows it off. Look at the server's boot override, or start into BIOS setup again.";
        if (queued) {
            said += ' ' + (setup->answerLost ? mayHaveQueuedSaid() : queuedSaid(setup->taskEnded)).toStdString();
        } else if (setup->changeOut) {
            // (Its change out, its answer not come: its record was marked as one the controller may queue before it
            // went, and stays so, whatever the clearing on the way out reads: outside review 2, 3.5.)
            said += ' ' + mayHaveQueuedSaid().toStdString();
        }
        if (setup->markNotWritten && stillUnwritten(kept, m_where.address, m_machine)) {
            said += ' ' + markNotWrittenSaid().toStdString();
        }
        pressed(setup->pressing, base::Error{"timeout", said});
    }
    m_setups.clear();
    if (!clearing) {
        return false;
    }
    // From now on a look (of a driver made anew for it) lets it go once nothing of these changes can come any more; and
    // the clearing below, where it is seen off after the change was seen made. Neither lets one marked queued go (its
    // mark written at the controller's answer: D1).
    kept.settle(m_where.address, m_machine, wallAt(landed ? now() : std::max(now(), lateUntil)));
    // Under this driver's session, which only its controller knows; and where the server is not known by its number,
    // only under the session it was last judged under.
    const QString number = redfish::marksOf(m_machine).server;
    const bool known = number.startsWith(u"server:"_s) && number != redfish::saidNone;
    if (m_session.token.isEmpty() || place.isEmpty() || (!known && m_session.token != m_checkedToken)) {
        return false; // (noted, and told at the next start: nothing here can clear it)
    }
    ControllerGoodbyes::SetupClearing clearingOf;
    clearingOf.where = DeviceAsker::Where{m_where.address, m_where.knownByCertificate, m_where.certificate};
    clearingOf.connectWithinMs = m_pace.connectWithinMs;
    clearingOf.system = place;
    clearingOf.token = m_session.token;
    clearingOf.machine = m_machine;
    clearingOf.withinMs = m_pace.setupClearWithinMs;
    clearingOf.againEveryMs = m_pace.confirmEveryMs;
    clearingOf.sessionPlace = m_downError ? QString() : m_session.place;
    clearingOf.signOutWithinMs = m_pace.signOutWithinMs;
    clearingOf.then = [overrides = m_pace.overrides, address = m_where.address, machine = m_machine,
                       landed](bool off, bool oursFirst) {
        if (off && (landed || oursFirst)) {
            static_cast<void>((overrides ? *overrides : SetupOverrides::program()).drop(address, machine));
        }
    };
    (m_pace.goodbyes ? *m_pace.goodbyes : ControllerGoodbyes::program()).clearSetupOverride(std::move(clearingOf));
    return true;
}

} // namespace qkvm::core
