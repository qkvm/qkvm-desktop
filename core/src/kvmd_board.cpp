#include <qkvm/core/kvmd_device.h>

#include <chrono>
#include <memory>
#include <string>
#include <utility>

#include <QJsonValue>
#include <QTimer>

using namespace Qt::StringLiterals;

// A kvmd KVM's power board (rule 23 of the web's FOR-THE-DESKTOP-APP, as a real Comet's showed it; kvm.py `act`,
// `_board_not_now`, `_board_may_be_pressing`, `_let_go`): the presses of KvmdDevice, apart from its other questions.
namespace qkvm::core {

namespace {

// The web's words, which a card and a notice show as they are.
base::Error stillHeld(qint64 leftMs, bool unclear, BoardHolds::By by)
{
    const qint64 seconds = leftMs / 1000 + 1;
    if (by == BoardHolds::By::fingerbot) {
        // (Two power buttons of one machine: pressed together, the one undoes the other.)
        return {"busy", unclear ? u"Its Fingerbot may still be pressing the power button: its last press got no clear "
                                  "answer. Try again in %1 seconds."_s.arg(seconds)
                                      .toStdString()
                                : u"Its Fingerbot is still holding the power button it was told to press. Try again in "
                                  "%1 seconds."_s.arg(seconds)
                                      .toStdString()};
    }
    return {"busy", unclear ? u"Its power board may still be pressing a button: its last press got no clear answer. "
                              "Look at the machine; try again in %1 seconds."_s.arg(seconds)
                                  .toStdString()
                            : u"Its power board is still holding a button it was told to press. Try again in %1 "
                              "seconds."_s.arg(seconds)
                                  .toStdString()};
}

base::Error beingPressed(BoardHolds::By by = BoardHolds::By::board)
{
    if (by == BoardHolds::By::fingerbot) {
        return {"busy", "Its Fingerbot is pressing the power button at this moment. Wait for it to finish."};
    }
    return {"busy", "A power button of this machine is being pressed at this moment. Wait for it to finish."};
}

// What a press that went out and got no clear answer is said as: it may have pressed.
base::Error mayHavePressed(const base::Error& why)
{
    if (why.code == "timeout") {
        return {"timeout", "The KVM did not say whether its board pressed the button. It may have: look at the machine "
                           "before pressing again."};
    }
    if (why.code == "dropped") {
        return {"dropped", "The KVM's answer to the press broke off. It may have pressed: look at the machine before "
                           "pressing again."};
    }
    return why;
}

base::Error boardBusy()
{
    return {"busy", "The KVM's power board is pressing a button at this moment (from its own page, or another "
                    "program). Wait for it to finish."};
}

} // namespace

bool KvmdDevice::pressesTheBoard(QStringView kind)
{
    return kind == u"atx_power" || kind == u"atx_power_long" || kind == u"atx_reset";
}

BoardHolds& KvmdDevice::boards() const
{
    return m_pace.boards ? *m_pace.boards : BoardHolds::program();
}

BoardHolds::Of KvmdDevice::boardOf() const
{
    return {m_where.address, m_where.knownByCertificate ? m_where.certificate : QByteArray()};
}

void KvmdDevice::pressBoard(const QString& kind, Ask click, std::function<void(base::Result<Done>)> done)
{
    // One press of this driver's at a time, for the whole of it: the wall holds the machine as one thing meanwhile,
    // and so does this.
    if (m_pressing) {
        later([done = std::move(done)] { done(beingPressed()); });
        return;
    }
    // A press of this KVM's board that got no clear answer may still be pressing: whoever asks, a driver made anew
    // too. (Nothing is asked of the KVM for this.)
    const BoardHolds::Of board = boardOf();
    if (const qint64 left = boards().heldMs(board); left > 0) {
        later([done = std::move(done),
               refusal = boards().pressing(board)
                             ? beingPressed(boards().heldBy(board))
                             : stillHeld(left, boards().unclear(board), boards().heldBy(board))] { done(refusal); });
        return;
    }
    m_pressing = true;
    // The board is held from here to the click's going out, for any driver of this KVM (the wall's and a box's, one
    // made anew): its board is asked first, and another that found nothing held meanwhile pressed as well (from the
    // merged review, 9), for as long as its road runs, a sign-in included (review 4's LOW 3). Let go of where nothing
    // goes.
    const qint64 held = kind == "atx_power_long"_L1 ? m_pace.longPressHeldMs : m_pace.pressHeldMs;
    m_pressReserved = boards().reserve(board);
    m_pressOutHeldMs = held;
    const auto over = [this, done = std::move(done)](const base::Result<Done>& result) {
        m_pressing = false;
        if (const quint64 reserved = std::exchange(m_pressReserved, 0); reserved != 0) {
            boards().notPressed(boardOf(), reserved);
        }
        done(result);
    };
    // The board is asked first, whatever its line said: refused where it is pressing a button (from its own page, or
    // another program's); and whether the computer is on is what it says now.
    askTheBoard([this, kind, click = std::move(click), over](std::optional<bool> on, bool busy) mutable {
        if (busy) {
            over(boardBusy());
            return;
        }
        // (Its hold taken over meanwhile, by a press of another's that came to no clear answer: held as that one.)
        const BoardHolds::Of board = boardOf();
        if (!boards().stillOut(board, m_pressReserved)) {
            m_pressReserved = 0;
            over(stillHeld(std::max<qint64>(1, boards().heldMs(board)), boards().unclear(board),
                           boards().heldBy(board)));
            return;
        }
        // A light that says "off" beside a picture is not wired up: it is no witness of what a press does.
        std::optional<bool> before = on;
        if (before == std::optional<bool>(false) && (!m_status || m_status->signal != std::optional<bool>(false))) {
            before.reset();
        }
        sendTheClick(kind, std::move(click), before, over);
    });
}

void KvmdDevice::sendTheClick(const QString& kind, Ask click, std::optional<bool> before,
                              const std::function<void(base::Result<Done>)>& over)
{
    const BoardHolds::Of board = boardOf();
    const qint64 held = kind == "atx_power_long"_L1 ? m_pace.longPressHeldMs : m_pace.pressHeldMs;
    // Its answer is waited for as long as the web waits (BOARD_ANSWER); and from when it goes, the board is held for
    // as long as that and the button's hold: a press that comes to no clear answer may be taken as late as that.
    click.quietMs = m_pace.boardAnswerMs;
    const qint64 mayTake = qint64{m_pace.boardAnswerMs} + held;
    click.goingOut = [this, board, mayTake, held] {
        // (Out now: the hold reserved for it is the click's from here.)
        m_pressOut = boards().out(board, mayTake);
        m_pressReserved = 0;
        m_pressOutHeldMs = held;
    };
    request(click, [this, kind, board, held, mayTake, before, over](const base::Result<Reply>& reply) {
        const quint64 press = std::exchange(m_pressOut, 0);
        if (!reply) {
            if (press != 0 && mayHaveBeenDone(reply.error())) {
                // No answer in time, or one that broke off: the click may have gone out, and the board may take it
                // as late as now. Left alone from now, whoever asks.
                boards().mayBePressing(board, mayTake);
                over(mayHavePressed(reply.error()));
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
        if (status != 200 && (status < 400 || status >= 500)) {
            // Neither its yes nor a no: its web server's 502, 503 or 504 while kvmd restarts behind it, or kvmd's own
            // 500, after the board may have been told. Whether it pressed is not known.
            boards().mayBePressing(board, mayTake);
            over(base::Error{"action_unknown", "The KVM's answer to the press says neither that it pressed nor that it "
                                               "did not (HTTP " +
                                                   std::to_string(status) +
                                                   "). It may have: look at the machine before pressing again."});
            return;
        }
        if (status != 200) {
            // Only a 4xx is a no: nothing was pressed.
            boards().notPressed(board, press);
            over(kvmd::refusalOf(status, reply.value().body));
            return;
        }
        // The board answers at once and holds the button on: until it has let go, the machine is being done something
        // to, and a second press meanwhile is taken by the board as well.
        boards().pressed(board, press, held);
        // (What the board said on its line before this answer is no witness of what the press did.)
        afterTheButtonsHold(
            board, [this, kind, before, words = m_boardWords, over] { afterTheHold(kind, before, words, over); });
    });
}

void KvmdDevice::askTheBoard(const std::function<void(std::optional<bool> on, bool busy)>& then)
{
    std::optional<QJsonObject> heard = boardOnTheLine();
    if (heard && !kvmd::boardIsThere(*heard)) {
        heard.reset(); // (a "no board" on the line: asked, as below)
    }
    askTheBoardAgain(2, heard, then);
}

void KvmdDevice::askTheBoardAgain(int left, const std::optional<QJsonObject>& heard,
                                  const std::function<void(std::optional<bool> on, bool busy)>& then)
{
    const bool heardBusy = heard && heard->value("busy"_L1) == QJsonValue(true);
    Ask board;
    board.path = u"/api/atx"_s;
    board.urgent = true;
    board.vital = false;
    ask(board, [this, left, heard, heardBusy, then](const base::Result<QJsonObject>& said) {
        if (said && kvmd::boardIsThere(said.value())) {
            then(kvmd::powerSaid(said.value()), heardBusy || said.value().value("busy"_L1) == QJsonValue(true));
            return;
        }
        if (said && left > 1) {
            // A Comet's board says "no board" to whoever asks while it answers somebody else: once more.
            askTheBoardAgain(left - 1, heard, then);
            return;
        }
        // (One that does not answer is not asked again: it says nothing.)
        if (heard) {
            then(kvmd::powerSaid(*heard), heardBusy);
            return;
        }
        then(std::nullopt, false);
    });
}

std::optional<QJsonObject> KvmdDevice::boardOnTheLine() const
{
    if (!m_lineBoard || !m_awake || !m_awake->isIn() || m_eventsHeardAt < 0 ||
        now() - m_eventsHeardAt > m_pace.eventsQuietMs) {
        return std::nullopt;
    }
    return m_lineBoard;
}

void KvmdDevice::heardOnTheLine(const QByteArray& message)
{
    // A Fingerbot paired with the KVM, or none: said on every line to its events (kvmd_fingerbot.cpp).
    if (const std::optional<bool> paired = kvmd::fingerbotHeard(message)) {
        fingerbotHeard(*paired);
        return;
    }
    // A word that says what the last one said is no news: a KVM that repeated its state would have its card looked at
    // again at every repetition.
    const std::optional<QJsonObject> board = kvmd::boardHeard(message);
    if (!board || board == m_lineBoard) {
        return;
    }
    m_lineBoard = board;
    ++m_boardWords;
    // What the card says may be out of date now: looked at again when it is next asked.
    m_statusAt = now() - m_pace.freshMs - 1;
    emit boardSaid();
}

void KvmdDevice::afterTheButtonsHold(const BoardHolds::Of& board, const std::function<void()>& then)
{
    // By the hold as it is kept, and not by a timer of its own: a timer that fired a moment early answered the press
    // while the hold still stood, and the next press was refused for it. (Where somebody else's press holds the board
    // longer, that is waited for too.)
    const qint64 left = boards().heldMs(board);
    if (left <= 0) {
        then();
        return;
    }
    QTimer::singleShot(std::chrono::milliseconds(left), Qt::PreciseTimer, this,
                       [this, board, then] { afterTheButtonsHold(board, then); });
}

void KvmdDevice::afterTheHold(const QString& kind, std::optional<bool> before, quint64 words,
                              const std::function<void(base::Result<Done>)>& over)
{
    // What the card says may be out of date now: looked at again when it is next asked.
    m_statusAt = now() - m_pace.freshMs - 1;
    // A press that must change the computer's power (a short one on a computer that is off, the long one on one that
    // is on) is answered once the board shows the change. A running system asked to shut down takes the time it
    // takes, and a reset leaves the power as it was.
    std::optional<bool> wanted;
    if (kind == "atx_power"_L1 && before == std::optional<bool>(false)) {
        wanted = true;
    } else if (kind == "atx_power_long"_L1 && before == std::optional<bool>(true)) {
        wanted = false;
    }
    if (!wanted) {
        over(Done{});
        return;
    }
    // Where it does not come in time, the press was made all the same, and says what was seen instead: never a
    // failure, whose retry presses the button a second time on a computer that may be on its way.
    auto watch = std::make_shared<Watch>();
    watch->wanted = *wanted;
    watch->words = words;
    watch->over = over;
    QTimer::singleShot(m_pace.seenWithinMs, this, [watch] {
        const QString pressed = watch->wanted ? u"pressed"_s : u"held"_s;
        if (!watch->said) {
            watch->settle(Done{pressed + u"; its board has not said since"_s});
        } else {
            watch->settle(Done{watch->wanted ? u"pressed, but the computer is still off"_s
                                             : u"held, but the computer is still on"_s});
        }
    });
    lookForTheChange(watch);
}

void KvmdDevice::lookForTheChange(const std::shared_ptr<Watch>& watch)
{
    // From the board's line where it has said something there since the click, and says the board is there; asked
    // otherwise.
    if (const std::optional<QJsonObject> heard = boardOnTheLine();
        heard && m_boardWords != watch->words && kvmd::boardIsThere(*heard)) {
        sawTheBoard(watch, kvmd::powerSaid(*heard));
        return;
    }
    Ask board;
    board.path = u"/api/atx"_s;
    board.urgent = true;
    board.vital = false;
    ask(board, [this, watch](const base::Result<QJsonObject>& said) {
        if (watch->settled) {
            return;
        }
        // (Only an answer in which the board says it is there says anything of the computer.)
        sawTheBoard(watch, said && kvmd::boardIsThere(said.value()) ? kvmd::powerSaid(said.value()) : std::nullopt);
    });
}

void KvmdDevice::sawTheBoard(const std::shared_ptr<Watch>& watch, std::optional<bool> on)
{
    if (on == std::optional<bool>(watch->wanted)) {
        watch->settle(Done{});
        return;
    }
    watch->said = watch->said || on.has_value();
    // Looked at again in a moment; or, where its line is held, at the board's next word there, if that comes first:
    // the press is answered as soon as the change is said (and the line's end is told the same way: then it is asked).
    // A turn of the loop later, never from inside what the line says.
    auto fired = std::make_shared<bool>(false);
    auto next = std::make_shared<QMetaObject::Connection>();
    const auto again = [this, watch, fired, next] {
        if (std::exchange(*fired, true)) {
            return;
        }
        disconnect(*next);
        later([this, watch] {
            if (!watch->settled) {
                lookForTheChange(watch);
            }
        });
    };
    if (boardOnTheLine()) {
        *next = connect(this, &KvmdDevice::boardSaid, this, again);
    }
    QTimer::singleShot(m_pace.seenEveryMs, this, again);
}

void KvmdDevice::Watch::settle(const base::Result<Done>& result)
{
    if (!std::exchange(settled, true)) {
        over(result);
    }
}

} // namespace qkvm::core
