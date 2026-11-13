#include <qkvm/core/kvmd_device.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <utility>

#include <QJsonDocument>
#include <QJsonValue>
#include <QTimer>

using namespace Qt::StringLiterals;

// A GL.iNet Fingerbot paired with a Comet (rule 29 of the web's FOR-THE-DESKTOP-APP, as a real one showed it on
// 2026-10-18; kvm.py `_fingerbot_press`, `_ask_fingerbot`, `_fingerbot_not_now`, `_battery_soon`): it presses the
// machine's power button with an arm, for a machine that has no power board. The Comet says whether one is paired only
// in a `fingerbot` event on its line for events; a press is POST /api/fingerbot/click with the press's time and its
// strength, answered once the Fingerbot took it (not once it is over); its battery is GET /api/fingerbot/battery, a
// round trip to the Fingerbot. The Comet does not keep one press at a time (one sent 0.3 s into another failed, one
// sent a second into another was taken): this driver does, in the same hold as the power board's.
namespace qkvm::core {

namespace {

// The Comet's `angle_enum` for "Firmly Press", its own page's default (the web's FINGERBOT_STRENGTHS["firm"]).
constexpr const char* firmly = "2";

base::Error fingerbotHeld(qint64 leftMs, bool unclear, BoardHolds::By by)
{
    const qint64 seconds = leftMs / 1000 + 1;
    if (by == BoardHolds::By::board) {
        // (Two power buttons of one machine: pressed together, the one undoes the other.)
        return {"busy", unclear ? u"Its power board may still be pressing a button: its last press got no clear "
                                  "answer. Look at the machine; try again in %1 seconds."_s.arg(seconds)
                                      .toStdString()
                                : u"Its power board is still holding a button it was told to press. Try again in %1 "
                                  "seconds."_s.arg(seconds)
                                      .toStdString()};
    }
    return {"busy", unclear ? u"The Fingerbot may still be pressing: the last press got no clear answer. Try again in "
                              "%1 seconds."_s.arg(seconds)
                                  .toStdString()
                            : u"The Fingerbot is still holding the power button it was told to press. Try again in %1 "
                              "seconds."_s.arg(seconds)
                                  .toStdString()};
}

base::Error fingerbotBeingPressed(BoardHolds::By by)
{
    if (by == BoardHolds::By::fingerbot) {
        return {"busy", "The Fingerbot is pressing the power button at this moment. Wait for it to finish."};
    }
    return {"busy", "A power button of this machine is being pressed at this moment. Wait for it to finish."};
}

base::Error noFingerbot()
{
    return {"invalid", "No Fingerbot is paired with this KVM"};
}

} // namespace

bool KvmdDevice::pressesTheFingerbot(QStringView kind)
{
    return kind == u"fingerbot_power" || kind == u"fingerbot_power_long";
}

std::optional<core::Fingerbot> KvmdDevice::fingerbotNow() const
{
    // (As the KVM said last on a line to its events; stock kvmd has none.)
    const BoardHolds::Fingerbot said = fingerbotSaid();
    if (m_glinet == std::optional<bool>(false) || said.paired != std::optional<bool>(true)) {
        return std::nullopt;
    }
    core::Fingerbot shown;
    shown.battery = said.battery;
    shown.low = said.battery.has_value() && *said.battery < kvmd::lowBattery;
    return shown;
}

void KvmdDevice::fingerbotHeard(bool paired)
{
    BoardHolds::Fingerbot said = fingerbotSaid();
    const bool news = said.paired != std::optional<bool>(paired);
    said.saidAt = boards().nowMs();
    if (news) {
        said.paired = paired;
        if (!paired) {
            // One paired anew (perhaps another) is asked soon: within a retry's time of the last question, and not at
            // every word of a KVM that says yes and no in turn (each question is a round trip on its battery).
            said.battery.reset();
            said.batteryNext = std::min(said.batteryNext, said.batteryAskedAt + m_pace.batteryRetryMs);
        }
    }
    boards().keepFingerbot(boardOf(), said);
    if (news) {
        // Its buttons come or go on the card: looked at again when next asked.
        m_statusAt = now() - m_pace.freshMs - 1;
        emit statusChanged();
    }
}

std::optional<base::Error> KvmdDevice::fingerbotNotNow(quint64 mine) const
{
    const BoardHolds::Of board = boardOf();
    if (mine != 0) {
        // (Its own reservation taken over meanwhile, by a press of another's that came to no clear answer: held as that
        // one.)
        if (!boards().stillOut(board, mine)) {
            return fingerbotHeld(std::max<qint64>(1, boards().heldMs(board)), boards().unclear(board),
                                 boards().heldBy(board));
        }
    } else if (const qint64 left = boards().heldMs(board); left > 0) {
        // A click of either button's out (or a press's look, reserved), or a hold of either's standing.
        return boards().pressing(board) ? fingerbotBeingPressed(boards().heldBy(board))
                                        : fingerbotHeld(left, boards().unclear(board), boards().heldBy(board));
    }
    const BoardHolds::Fingerbot said = fingerbotSaid();
    const qint64 at = boards().nowMs();
    if (said.paired == std::optional<bool>(false) && at - said.saidAt < m_pace.fingerbotAskAgainMs) {
        // As it said a moment ago. Not asked again on a line of its own, which may wake its computer, for a press that
        // is not made: where one has been paired since, it says so on the line held while its picture is watched, or
        // when it is asked again a while later.
        return noFingerbot();
    }
    if (said.paired != std::optional<bool>(true) && at - said.silentAt < m_pace.fingerbotAskAgainMs) {
        const qint64 left = m_pace.fingerbotAskAgainMs - (at - said.silentAt);
        return base::Error{"action_failed",
                           u"Nothing was pressed: the KVM did not say whether a Fingerbot is paired with it when it "
                           u"was asked a moment ago. It is asked again in %1 seconds, or as soon as its picture is "
                           u"watched."_s.arg(left / 1000 + 1)
                               .toStdString()};
    }
    return std::nullopt;
}

void KvmdDevice::pressFingerbot(const QString& kind, std::function<void(base::Result<Done>)> done)
{
    if (m_glinet == std::optional<bool>(false)) {
        later([done = std::move(done)] {
            done(base::Error{"invalid", "This KVM has no Fingerbot: that is a GL.iNet Comet's"});
        });
        return;
    }
    if (std::optional<base::Error> why = fingerbotNotNow()) {
        later([done = std::move(done), why = *why] { done(why); });
        return;
    }
    // One press of this driver's at a time, of either button: the machine's power buttons are one.
    if (m_pressing) {
        later([done = std::move(done)] {
            done(base::Error{"busy",
                             "A power button of this machine is being pressed at this moment. Wait for it to finish."});
        });
        return;
    }
    m_pressing = true;
    // The machine's buttons are held from here to the click's going out, for any driver of this KVM, as a board press
    // holds them from its look (the merged review's LOW 9): its battery's question may be waited for, and a line opened
    // to ask whether one is paired, and a sign-in before the click, meanwhile: held for as long as that road runs
    // (review 4's LOW 3). Let go of where nothing goes.
    m_fingerbotReserved = boards().reserve(boardOf(), BoardHolds::By::fingerbot);
    const auto over = [this, done = std::move(done)](const base::Result<Done>& result) {
        m_pressing = false;
        if (const quint64 reserved = std::exchange(m_fingerbotReserved, 0); reserved != 0) {
            boards().notPressed(boardOf(), reserved);
        }
        done(result);
    };
    // A question of its battery under way is let finish (the Comet talks to its Fingerbot one request at a time).
    afterTheFingerbotsTurn([this, kind, over] {
        // (As it is now: the turn may have been waited for.)
        if (std::optional<base::Error> why = fingerbotNotNow(m_fingerbotReserved)) {
            over(*why);
            return;
        }
        if (fingerbotSaid().paired == std::optional<bool>(true)) {
            clickTheFingerbot(kind, over);
            return;
        }
        // Not said either way since the program started (nobody has watched its picture), or not of late: asked. The
        // press goes to the machine anyway, so the line opened for it wakes nothing that the press would not.
        askWhetherOneIsPaired([this, kind, over](const base::Result<std::optional<bool>>& said) {
            if (!said) {
                over(base::Error{"action_failed", "Nothing was pressed: the KVM could not be asked whether a "
                                                  "Fingerbot is paired with it (" +
                                                      said.error().message + ")"});
                return;
            }
            if (said.value() == std::optional<bool>(false)) {
                over(noFingerbot());
                return;
            }
            if (!said.value()) {
                BoardHolds::Fingerbot kept = fingerbotSaid();
                kept.silentAt = boards().nowMs();
                boards().keepFingerbot(boardOf(), kept);
                over(base::Error{"action_failed",
                                 "Nothing was pressed: the KVM did not say whether a Fingerbot is paired with it"});
                return;
            }
            // (Pressed by somebody else meanwhile: held as before.)
            if (std::optional<base::Error> why = fingerbotNotNow(m_fingerbotReserved)) {
                over(*why);
                return;
            }
            clickTheFingerbot(kind, over);
        });
    });
}

void KvmdDevice::afterTheFingerbotsTurn(const std::function<void()>& then)
{
    // By the time it is kept, which another driver of the KVM may have set: one let go of with its question out leaves
    // it set until that question's time is up (the Comet goes on with it).
    const qint64 left = fingerbotSaid().askedUntil - boards().nowMs();
    if (left <= 0) {
        then();
        return;
    }
    QTimer::singleShot(std::chrono::milliseconds(std::min<qint64>(left, 50)), Qt::PreciseTimer, this,
                       [this, then] { afterTheFingerbotsTurn(then); });
}

void KvmdDevice::askWhetherOneIsPaired(std::function<void(base::Result<std::optional<bool>>)> then)
{
    withToken([this, then = std::move(then)](const base::Result<QByteArray>& token) {
        if (!token) {
            then(token.error());
            return;
        }
        // A line of its own to the KVM's events, which hears nothing of its video (stream=0), handed the pass as the
        // line that keeps the video awake is (keepAwake).
        AwakeLine::Where where;
        where.address = m_where.address;
        where.asking = "/api/ws?stream=0"_ba +
                       (m_glinet == std::optional<bool>(true) ? "&auth_token="_ba + token.value() : QByteArray());
        where.cookie = "auth_token="_ba + token.value();
        where.knownByCertificate = m_where.knownByCertificate;
        where.certificate = m_where.certificate;
        if (m_fingerbotLine) {
            m_fingerbotLine.release()->deleteLater();
        }
        m_fingerbotLine = std::make_unique<AwakeLine>(std::move(where), m_pace.connectWithinMs);
        AwakeLine* line = m_fingerbotLine.get();
        line->hearWhatItSays(kvmd::mostBoardWord, 0);
        auto answered = std::make_shared<bool>(false);
        const auto settle = [this, line, answered, then](const base::Result<std::optional<bool>>& said) {
            if (std::exchange(*answered, true)) {
                return;
            }
            // Closed a turn later: this is called from inside what the line says.
            if (m_fingerbotLine.get() == line) {
                m_fingerbotLine.release()->deleteLater();
            }
            then(said);
        };
        // (Only its word on that: what its board says there is the line's that keeps the video awake to read.)
        connect(line, &AwakeLine::said, this, [this, settle](const QByteArray& message) {
            if (const std::optional<bool> paired = kvmd::fingerbotHeard(message)) {
                fingerbotHeard(*paired);
                settle(std::optional<bool>(*paired));
            }
        });
        // (A line that ends, or never is let in, said nothing.)
        connect(line, &AwakeLine::ended, this, [settle] { settle(std::optional<bool>()); });
        QTimer::singleShot(m_pace.fingerbotAskWithinMs, line, [settle] { settle(std::optional<bool>()); });
        line->open();
    });
}

void KvmdDevice::clickTheFingerbot(const QString& kind, const std::function<void(base::Result<Done>)>& over)
{
    const BoardHolds::Of board = boardOf();
    const qint64 pressMs = kind == "fingerbot_power_long"_L1 ? m_pace.fingerbotLongPressMs : m_pace.fingerbotPressMs;
    // From when it goes, the machine's power buttons are held for as long as a press taken at that moment can last: the
    // answer's time, the press's own and the arm's (the Comet may go on with a request whose asker gave up).
    const qint64 mayTake = qint64{m_pace.fingerbotAnswerMs} + pressMs + m_pace.fingerbotBackMs;
    Ask click;
    click.method = "POST"_ba;
    click.path = u"/api/fingerbot/click"_s;
    click.query.addQueryItem(u"press_time"_s, QString::number(pressMs));
    click.query.addQueryItem(u"angle_enum"_s, QString::fromLatin1(firmly));
    click.quietMs = static_cast<int>(m_pace.fingerbotAnswerMs + pressMs);
    click.urgent = true;
    click.vital = false;
    click.doesSomething = true;
    click.goingOut = [this, board, mayTake] {
        // (Out now: the hold reserved for it is the click's from here.)
        m_fingerbotOut = boards().out(board, mayTake, BoardHolds::By::fingerbot);
        m_fingerbotReserved = 0;
        m_fingerbotMayTakeMs = mayTake;
    };
    request(click, [this, board, pressMs, mayTake, over](const base::Result<Reply>& reply) {
        const quint64 press = std::exchange(m_fingerbotOut, 0);
        if (!reply) {
            if (press != 0 && mayHaveBeenDone(reply.error())) {
                boards().mayBePressing(board, mayTake, BoardHolds::By::fingerbot);
                over(reply.error().code == "dropped"
                         ? base::Error{"dropped", "The KVM's answer to the press broke off. It may have pressed: look "
                                                  "at the machine before pressing again."}
                         : base::Error{"timeout",
                                       "The KVM did not say whether its Fingerbot pressed. It may have: look "
                                       "at the machine before pressing again."});
                return;
            }
            boards().notPressed(board, press);
            if (press == 0 && mayHaveBeenDone(reply.error())) {
                // (Signing in first, or a connection that never came about: the click never went out.)
                over(base::Error{"action_failed", "Nothing was pressed: the press did not reach the KVM (" +
                                                      reply.error().message + ")"});
                return;
            }
            over(reply.error());
            return;
        }
        const int status = reply.value().status;
        const QJsonObject said = QJsonDocument::fromJson(reply.value().body).object();
        const QJsonObject result = said.value("result"_L1).toObject();
        if (status >= 400 && status < 500) {
            // Only a 4xx is a no ("Press Failed" on the Comet's own page: its 400 "Failed to click"): nothing was
            // pressed as far as the Comet knows.
            boards().notPressed(board, press);
            QString why = kvmd::plain(result.value("error_msg"_L1), 120);
            if (why.isEmpty()) {
                why = kvmd::plain(result.value("error"_L1), 120);
            }
            over(base::Error{"action_failed",
                             why.isEmpty()
                                 ? u"The Fingerbot did not press (the KVM answered HTTP %1)"_s.arg(status).toStdString()
                                 : u"The Fingerbot did not press: the KVM said \"%1\""_s.arg(why).toStdString()});
            return;
        }
        if (status != 200 || said.value("ok"_L1) != QJsonValue(true) ||
            result.value("result"_L1) != QJsonValue(u"success"_s)) {
            // Neither its yes nor a no (a 5xx: kvmd's own error, or a proxy's, after the Fingerbot may have been told;
            // a yes of another shape): whether it pressed is not known, and it is not offered again on the strength of
            // it.
            boards().mayBePressing(board, mayTake, BoardHolds::By::fingerbot);
            over(base::Error{"action_unknown", "The KVM's answer to the press says neither that it pressed nor that it "
                                               "did not (HTTP " +
                                                   std::to_string(status) +
                                                   "). It may have: look at the machine before pressing again."});
            return;
        }
        // The Comet says the Fingerbot took the press, not that it is over: the machine is held for the press's own
        // time from now, and the arm's way back.
        boards().pressed(board, press, pressMs + m_pace.fingerbotBackMs, BoardHolds::By::fingerbot);
        m_statusAt = now() - m_pace.freshMs - 1;
        afterTheButtonsHold(board, [over] { over(Done{}); });
    });
}

void KvmdDevice::batterySoon()
{
    BoardHolds::Fingerbot said = fingerbotSaid();
    const qint64 at = boards().nowMs();
    if (m_glinet == std::optional<bool>(false) || said.paired != std::optional<bool>(true) || at < said.batteryNext ||
        m_askingBattery || m_pressing || said.askedUntil > at || boards().heldMs(boardOf()) > 0) {
        return;
    }
    m_askingBattery = true;
    said.batteryAskedAt = at;
    said.batteryNext = at + m_pace.batteryRetryMs; // (sooner once it has answered)
    said.askedUntil = at + m_pace.batteryWaitMs + m_pace.connectWithinMs;
    boards().keepFingerbot(boardOf(), said);
    Ask battery;
    battery.path = u"/api/fingerbot/battery"_s;
    battery.vital = false;
    // (Its answer waited for this long and no longer: a person's press waits for it.)
    battery.quietMs = m_pace.batteryWaitMs;
    ask(battery, [this](const base::Result<QJsonObject>& answer) {
        m_askingBattery = false;
        BoardHolds::Fingerbot kept = fingerbotSaid();
        kept.askedUntil = 0;
        const std::optional<int> percent = answer ? kvmd::batterySaid(answer.value()) : std::nullopt;
        // (Unanswered: asked again after a retry's time; what it said before stays.)
        if (percent && kept.paired == std::optional<bool>(true)) {
            kept.battery = percent;
            kept.batteryNext = boards().nowMs() + m_pace.batteryEveryMs;
            m_statusAt = now() - m_pace.freshMs - 1;
        }
        boards().keepFingerbot(boardOf(), kept);
        if (percent) {
            emit statusChanged();
        }
    });
}

} // namespace qkvm::core
