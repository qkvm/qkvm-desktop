#include <qkvm/core/redfish_device.h>

#include <algorithm>
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
// where it is seen off with nothing of the change still on its way, and told at the next start otherwise.
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
    bool running = true;    // the server was on: it is restarted (else switched on)
    bool maySet = false;    // the change of the override may have been made (from when it can go out)
    bool seenUnset = false; // a read after the change came, and did not show it
    QString lastPower;      // the server's power as the last read said it
    bool over = false;
    // What is kept beside the settings for it (setup_overrides.h, review 4's MEDIUM 1): whether it was noted there, and
    // until when what stood there before this press doubted; until when the change it sent may still come to be made
    // (this driver's clock); whether that change was seen made (a 204, a 200 or a read that shows it, a read before a
    // clearing that shows it), so that nothing of it can come later; and when the override was seen off at the end
    // (taken, cleared, another tool's in its place).
    bool noted = false;
    std::optional<QDateTime> before;
    qint64 lateUntil = 0;
    bool landed = false;
    std::optional<qint64> seenOffAt;
};

void RedfishDevice::startIntoSetup(const std::shared_ptr<Pressing>& pressing)
{
    auto setup = std::make_shared<Setup>();
    setup->pressing = pressing;
    setup->restartBy = now() + m_pace.setupWithinMs;
    setup->endsAt = now() + m_pace.setupPatienceMs;
    m_setups.append(setup);
    find([this, setup](const base::Result<Places>& places) {
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
            // Another tool's override (a provisioning tool's network boot, an override for every boot) is not replaced.
            if (const QString other = redfish::anotherOverride(system); !other.isEmpty()) {
                setupOver(setup,
                          base::Error{"action_failed", u"Nothing was done: the controller has a boot override set "
                                                       u"already (%1). Clear it first."_s.arg(other)
                                                           .toStdString()});
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
    change.quietMs = m_pace.bootQuietMs;
    // A controller wants to be shown the state the change was decided on; one that will not take the mark it gave
    // (or gave none) is told that the change is meant whatever the state, once the computer was read and looked at
    // again (setupRead).
    const QByteArray shown = meant ? "*"_ba : mark;
    if (!shown.isEmpty()) {
        change.sent = {{"If-Match"_ba, shown}};
    }
    // From here on what comes may have set it: it may be set from the moment it can go out (a driver let go of has it
    // cleared on the way out), and it is noted beside the settings before it goes (review 4's MEDIUM 1).
    sentFor(setup->pressing);
    setup->maySet = true;
    setup->lateUntil = now() + m_pace.bootQuietMs + m_pace.transferMs;
    noteMayBeSet(setup);
    request(change, [this, setup, meant](const base::Result<Reply>& reply) {
        if (setup->over) {
            return;
        }
        if (!reply) {
            if (!mayHaveBeenDone(reply.error())) {
                // (It never went out: nothing was set.)
                setup->maySet = false;
                setupOver(setup, base::Error{reply.error().code, "Nothing was done: the change did not reach the "
                                                                 "controller (" +
                                                                     reply.error().message + ")."});
                return;
            }
            setup->maySet = true;
            clearTheOverride(setup, false, [this, setup, why = reply.error()](const std::optional<QString>& left) {
                setupOver(setup, leftAs(u"Nothing was restarted: the controller's answer to the BIOS setup boot "
                                        u"override did not come or could not be read (%1)"_s.arg(
                                            QString::fromStdString(why.message)),
                                        left));
            });
            return;
        }
        const int status = reply.value().status;
        if ((status == 412 || status == 428) && !meant) {
            // The state changed since it was read, or the controller wants a mark it did not give: nothing was set.
            // Read again, and looked at again (another tool's override, or a power change meanwhile, is not gone
            // over), then sent as meant.
            setup->maySet = false;
            setupRead(setup, true);
            return;
        }
        if (status >= 400 && status < 500) {
            // Refused for certain: nothing was set.
            setup->maySet = false;
            const base::Result<void> verdict = redfish::verdictOn(status, reply.value().body);
            setupOver(setup,
                      verdict ? base::Error{"action_failed", "The controller refused the change"} : verdict.error());
            return;
        }
        setup->maySet = true;
        if (status != 200 && status != 202 && status != 204) {
            QString said = redfish::errorSaid(reply.value().body);
            if (said.isEmpty()) {
                said = u"HTTP %1"_s.arg(status);
            }
            const QString how = status >= 500 ? u"an error of its own"_s : u"an answer a change does not have"_s;
            clearTheOverride(setup, false, [this, setup, how, said](const std::optional<QString>& left) {
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
            setup->landed = true;
            restartIntoSetup(setup);
            return;
        }
        setup->seenUnset = setup->seenUnset || read.ok();
        if (now() < until) {
            QTimer::singleShot(m_pace.confirmEveryMs, this, [this, setup, until] { lookForTheOverride(setup, until); });
            return;
        }
        if (setup->seenUnset) {
            // It took the change and does not show it: no restart, since it would not land in the setup, and the
            // override cleared, in case it is made later.
            clearTheOverride(setup, false, [this, setup](const std::optional<QString>& left) {
                setupOver(setup, leftAs(u"Nothing was restarted: the controller took the BIOS setup boot override and "
                                        u"does not show it set"_s,
                                        left));
            });
            return;
        }
        // (Never read back: the controller said yes to the change, and the restart is what was asked for.)
        restartIntoSetup(setup);
    });
}

void RedfishDevice::restartIntoSetup(const std::shared_ptr<Setup>& setup)
{
    if (now() >= setup->restartBy) {
        // Too late for a restart: whoever pressed may have stopped waiting for it.
        clearTheOverride(setup, false, [this, setup](const std::optional<QString>& left) {
            setupOver(setup, leftAs(u"Nothing was restarted: the controller took more than %1 seconds before the "
                                    u"restart"_s.arg(m_pace.setupWithinMs / 1000),
                                    left));
        });
        return;
    }
    Ask command;
    command.method = "POST"_ba;
    command.path = setup->resetTarget;
    command.body = QJsonDocument(QJsonObject{{u"ResetType"_s, setup->resetKind}}).toJson(QJsonDocument::Compact);
    command.bodyIs = "application/json"_ba;
    command.doesSomething = true;
    command.command = true;
    command.quietMs = m_pace.bootQuietMs;
    request(command, [this, setup](const base::Result<Reply>& reply) {
        if (setup->over) {
            return;
        }
        if (!reply && !mayHaveBeenDone(reply.error())) {
            // It never went out: the override is cleared again, so that no ordinary boot lands in the setup.
            clearTheOverride(setup, false, [this, setup, why = reply.error()](const std::optional<QString>& left) {
                setupOver(setup, unset(why, left));
            });
            return;
        }
        if (reply && reply.value().status >= 400 && reply.value().status < 500) {
            const base::Result<void> verdict = redfish::verdictOn(reply.value().status, reply.value().body);
            clearTheOverride(
                setup, false,
                [this, setup, why = verdict ? base::Error{"action_failed", "The controller refused"} : verdict.error()](
                    const std::optional<QString>& left) { setupOver(setup, unset(why, left)); });
            return;
        }
        // Taken, or perhaps taken (no answer, an error of the controller's own): the server may be starting into its
        // setup now. Watched until it has taken the override, and the override cleared at the press's end where it
        // has not.
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
                    if (redfish::overrideOff(read.value()) == std::optional<bool>(true)) {
                        // Taken as it started: the controller clears a one-time override at the boot that uses it.
                        // (Only where it says "Disabled": a read that shows no override says nothing of it, and the
                        // start is watched on, review 4's MEDIUM 2.)
                        setup->seenOffAt = now();
                        setupOver(setup, Done{});
                        return;
                    }
                    if (const QString other = redfish::anotherOverride(read.value()); !other.isEmpty()) {
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
    clearTheOverride(setup, false, [this, setup](const std::optional<QString>& left) {
        const int minutes = std::max(1, m_pace.setupPatienceMs / 60'000);
        const bool stillOff = !setup->running && setup->lastPower == "Off"_L1;
        const QString lead =
            stillOff
                ? u"The controller accepted switching on, but the server is still off after %1 minutes"_s.arg(minutes)
            : setup->running
                ? u"The server was told to restart, and was not seen to start into its BIOS setup within %1 minutes"_s
                      .arg(minutes)
                : u"The server was told to switch on, and was not seen to start into its BIOS setup within %1 "
                  u"minutes"_s.arg(minutes);
        if (!left) {
            // ("Not carried out" only where a look came back and showed it as it was; otherwise it may have been.)
            setupOver(setup, base::Error{stillOff ? "action_failed" : "timeout",
                                         (lead + u"; the BIOS setup boot override was cleared again, so its next boot "
                                                 u"is an ordinary one. Look at the server."_s)
                                             .toStdString()});
            return;
        }
        setupOver(setup, base::Error{"timeout", (lead + u"; "_s + *left + u". "_s + lookAgain()).toStdString()});
    });
}

void RedfishDevice::clearTheOverride(const std::shared_ptr<Setup>& setup, bool meant,
                                     std::function<void(std::optional<QString> left)> then)
{
    // Read as the server it is: another one that took the address meanwhile is neither read for this one nor cleared.
    checkedRead(setup->place, true,
                [this, setup, meant, then = std::move(then)](const base::Result<QJsonObject>& read,
                                                             const Checked& checked) mutable {
                    if (!read) {
                        then(notCleared());
                        return;
                    }
                    if (const QString other = redfish::anotherOverride(read.value()); !other.isEmpty()) {
                        setup->seenOffAt = now(); // (the BIOS setup one is not set)
                        then(u"the boot override is another tool's now (%1), and was left as it is"_s.arg(other));
                        return;
                    }
                    // (Shown set: the change this press sent was made, and nothing of it can come after this.)
                    if (redfish::bootOverride(read.value()) == std::pair<QString, QString>(ours0, ours1)) {
                        setup->landed = true;
                    }
                    // (Sent also where it shows none: a change the controller accepted for later, or whose answer was
                    // lost, may be made after this read, and the clearing is to come after it.)
                    Ask change;
                    change.method = "PATCH"_ba;
                    change.path = setup->place;
                    change.body =
                        QJsonDocument(
                            QJsonObject{{u"Boot"_s, QJsonObject{{u"BootSourceOverrideEnabled"_s, u"Disabled"_s}}}})
                            .toJson(QJsonDocument::Compact);
                    change.bodyIs = "application/json"_ba;
                    change.doesSomething = true;
                    change.command = true;
                    change.quietMs = m_pace.bootQuietMs;
                    const QByteArray shown = meant ? "*"_ba : checked.mark;
                    if (!shown.isEmpty()) {
                        change.sent = {{"If-Match"_ba, shown}};
                    }
                    request(change, [this, setup, meant, then](const base::Result<Reply>& reply) mutable {
                        if (!reply) {
                            then(notCleared());
                            return;
                        }
                        const int status = reply.value().status;
                        if ((status == 412 || status == 428) && !meant) {
                            clearTheOverride(setup, true, std::move(then));
                            return;
                        }
                        if (status == 204) {
                            setup->seenOffAt = now();
                            then(std::nullopt);
                            return;
                        }
                        if (status == 200 || status == 202) {
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
            if (read) {
                // (Cleared only where it says "Disabled": one that shows no override is read again within the time
                // left, then said to be maybe set, review 4's MEDIUM 2.)
                if (redfish::overrideOff(read.value()) == std::optional<bool>(true)) {
                    setup->seenOffAt = now();
                    then(std::nullopt);
                    return;
                }
                if (const QString other = redfish::anotherOverride(read.value()); !other.isEmpty()) {
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
    settleTheNote(setup);
    pressed(setup->pressing, result);
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

void RedfishDevice::noteMayBeSet(const std::shared_ptr<Setup>& setup)
{
    // Until the press can do nothing more: its end, the clearing then (a read and the change, each waited for as long
    // as the change), and a change still on its way after that.
    const qint64 last =
        std::max(setup->endsAt, setup->lateUntil) + (qint64{2} * m_pace.bootQuietMs) + m_pace.transferMs;
    const std::optional<QDateTime> before = overrides().mayBeSet(m_where.address, m_machine, wallAt(last));
    if (!std::exchange(setup->noted, true)) {
        setup->before = before;
    }
}

void RedfishDevice::settleTheNote(const std::shared_ptr<Setup>& setup)
{
    if (!setup->noted) {
        return;
    }
    SetupOverrides& kept = overrides();
    if (!setup->maySet) {
        // Nothing of this press's was set (refused for certain, or never sent): what stood before stands as it did.
        if (setup->before) {
            kept.settle(m_where.address, m_machine, *setup->before);
        } else {
            kept.drop(m_where.address, m_machine);
        }
        return;
    }
    // Seen off, and nothing of the change this press sent can come after that: let go of. Else it stands, and a look
    // lets it go once that change can no longer come (the web's CLEARED_LATE: a change accepted, or whose answer was
    // lost, may be made after the clearing).
    if (setup->seenOffAt && (setup->landed || *setup->seenOffAt >= setup->lateUntil)) {
        kept.drop(m_where.address, m_machine);
        return;
    }
    kept.settle(m_where.address, m_machine, wallAt(std::max(now(), setup->lateUntil)));
}

void RedfishDevice::lookedAtTheOverride(const QJsonObject& system)
{
    // (Off in so many words, or another tool's in its place: the BIOS setup one is not set. A read that shows none says
    // nothing, review 4's MEDIUM 2.)
    SetupOverrides& kept = overrides();
    if (kept.standing(m_where.address, m_machine) &&
        (redfish::overrideOff(system) == std::optional<bool>(true) || !redfish::anotherOverride(system).isEmpty())) {
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
        lateUntil = std::max(lateUntil, setup->lateUntil);
        place = setup->place;
        // Its press is told as it is (on the program's loop, as every press is: pressed): the override may be set, and
        // is cleared on the way out.
        pressed(
            setup->pressing,
            base::Error{"timeout",
                        "The connection to the controller was let go of while the server was starting into its "
                        "BIOS setup: the BIOS setup boot override may be set, so the next boot may land in the BIOS "
                        "setup. It is cleared as the connection goes, and the server's card says so until a look "
                        "shows it off. Look at the server's boot override, or start into BIOS setup again."});
    }
    m_setups.clear();
    if (!clearing) {
        return false;
    }
    // From now on a look (of a driver made anew for it) lets it go once nothing of these changes can come any more; and
    // the clearing below, where it is seen off after the change was seen made.
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
            (overrides ? *overrides : SetupOverrides::program()).drop(address, machine);
        }
    };
    (m_pace.goodbyes ? *m_pace.goodbyes : ControllerGoodbyes::program()).clearSetupOverride(std::move(clearingOf));
    return true;
}

} // namespace qkvm::core
