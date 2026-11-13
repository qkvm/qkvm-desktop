#include "control.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>

#include <QCoreApplication>
#include <QStringList>

namespace qkvm::ui {

namespace {

QString words(const char* text)
{
    return QCoreApplication::translate("Control", text);
}

// How far the pointer goes each way over a picture (live.h).
constexpr int farthest = 32767;
// A notch of the wheel, as Qt says a turn; and the most notches one turn is said as: a number
// from nowhere does not become a thousand messages.
constexpr int notch = 120;
constexpr int mostNotches = 16;
constexpr int msInAMinute = 60'000;
// Windows' name for L: its lock goes by the name, wherever a layout has the letter.
constexpr std::uint32_t nameOfL = 0x4C;
// How many things of the mouse wait at the most behind keys that are still to be told: as many
// as keys wait where they are heard.
constexpr std::size_t mostWaiting = 32;

// What is said where keys, or what the mouse did behind them, would have come too late.
QString notInTime()
{
    return words("Keys could not be passed on in time. Keyboard and mouse were given back, and every key on "
                 "the machine was let go.");
}

bool isWindowsKey(std::uint16_t usage)
{
    return usage == input::usageLeftWindows || usage == input::usageRightWindows;
}

// What a modifier is called where the bar names it. (By the order of USB's numbers for the
// eight: Ctrl, Shift, Alt and the Windows key, left and then right.)
QString nameOf(std::uint16_t modifier)
{
    static constexpr std::array<const char*, 8> names = {
        QT_TRANSLATE_NOOP("Control", "Left Ctrl"),  QT_TRANSLATE_NOOP("Control", "Left Shift"),
        QT_TRANSLATE_NOOP("Control", "Left Alt"),   QT_TRANSLATE_NOOP("Control", "Left Windows key"),
        QT_TRANSLATE_NOOP("Control", "Right Ctrl"), QT_TRANSLATE_NOOP("Control", "Right Shift"),
        QT_TRANSLATE_NOOP("Control", "Right Alt"),  QT_TRANSLATE_NOOP("Control", "Right Windows key"),
    };
    return input::isModifier(modifier) ? words(names.at(static_cast<std::size_t>(modifier - input::usageLeftCtrl)))
                                       : QString();
}

} // namespace

MachineControl::MachineControl(ControlView& view, KeySource& keys, QObject* parent)
    : MachineControl(view, keys, nullptr, Pace{}, parent)
{}

MachineControl::MachineControl(ControlView& view, KeySource& keys, Pace pace, QObject* parent)
    : MachineControl(view, keys, nullptr, pace, parent)
{}

MachineControl::MachineControl(ControlView& view, KeySource& keys, KeysKept& kept, QObject* parent)
    : MachineControl(view, keys, &kept, Pace{}, parent)
{}

MachineControl::MachineControl(ControlView& view, KeySource& keys, KeysKept& kept, Pace pace, QObject* parent)
    : MachineControl(view, keys, &kept, pace, parent)
{}

MachineControl::MachineControl(ControlView& view, KeySource& keys, KeysKept* shared, Pace pace, QObject* parent)
    : QObject(parent), m_view(view), m_keys(keys), m_pace(pace),
      m_own(shared == nullptr ? std::make_unique<KeysKept>(pace.lingerMs) : nullptr),
      m_kept(shared != nullptr ? *shared : *m_own)
{
    m_clock.start();
    m_textDeadline.setSingleShot(true);
    m_textDeadline.setInterval(m_pace.textReleaseWithinMs);
    connect(&m_textDeadline, &QTimer::timeout, this, [this] { cancelText(false); });
    connect(&m_view, &ControlView::takeAsked, this, &MachineControl::take);
    connect(&m_view, &ControlView::takeoverAsked, this, &MachineControl::askTakeover);
    connect(&m_view, &ControlView::takeoverAnswered, this, &MachineControl::answerTakeover);
    connect(&m_view, &ControlView::giveBackAsked, this, &MachineControl::giveBack);
    connect(&m_view, &ControlView::ctrlAltDelAsked, this, &MachineControl::ctrlAltDel);
    connect(&m_view, &ControlView::localInputAsked, this, &MachineControl::setLocalInput);
    connect(&m_view, &ControlView::keysTapped, this, &MachineControl::tapKeys);
    connect(&m_view, &ControlView::wentOnSaid, this, &MachineControl::wentOn);
    connect(&m_view, &ControlView::pointerMoved, this, &MachineControl::pointerAt);
    connect(&m_view, &ControlView::buttonChanged, this, &MachineControl::button);
    connect(&m_view, &ControlView::wheelTurned, this, &MachineControl::wheel);
    connect(&m_view, &ControlView::relativeAsked, this, &MachineControl::setRelative);
    connect(&m_view, &ControlView::pointerShifted, this, &MachineControl::pointerBy);
    connect(&m_view, &ControlView::captureBegan, this, [this] {
        if (m_on && m_relative && !m_localInput) {
            m_captured = true;
            tell();
        }
    });
    connect(&m_view, &ControlView::captureEnded, this, [this] {
        if (m_on && m_relative && !m_localInput) {
            stop(words("Relative mouse was released. Keyboard and mouse were given back."));
        }
    });
    connect(&m_keys, &KeySource::keyChanged, this, &MachineControl::keyCame);
    connect(&m_keys, &KeySource::fellBehind, this, [this] { heardNoMore(notInTime()); });
    connect(&m_keys, &KeySource::lostHearing, this, [this](KeySource::Lost why) {
        // (Where the reason is one that is over with the next taking, that is said with it.)
        heardNoMore(sentenceFor(why), why == KeySource::Lost::BeganToTell ? Remedy::AskAgain : Remedy::None);
    });
    connect(&m_keys, &KeySource::nothingIsDown, this, &MachineControl::nothingIsDown);
    // What another program types reaches no machine: said from the first such key on, for as
    // long as they are held.
    connect(&m_keys, &KeySource::typedByAProgram, this, [this] {
        if (m_on && !m_typed) {
            m_typed = true;
            tell();
        }
    });
    // What is held is one thing for all the windows: a key that another window's keys heard go
    // up is not waited for here any longer, and holds nothing back.
    connect(&m_kept, &KeysKept::changed, this, [this] {
        listenNoLongerThanNeeded();
        tell();
    });
    m_alive.setInterval(m_pace.aliveEveryMs);
    connect(&m_alive, &QTimer::timeout, this, [this] {
        // (Where its thread no longer looks, it says so in here, and they are given back.)
        m_keys.stillThere();
        // Nothing is swallowed any more, and why was not told here yet: asked for, before
        // anything is made of it. The key source may know (keys fell behind while this thread
        // was held up; Windows showed a screen of its own), and that word may still be on its
        // way: the reason that is known is the one that is said.
        if (m_on && !m_localInput && !m_keys.swallows()) {
            m_keys.tellNow();
        }
        // It had none: the leave-key gave the keyboard back where the keys are heard, and it
        // was not heard of here.
        if (m_on && !m_localInput && !m_keys.swallows()) {
            stop({});
            return;
        }
        // Whether the keyboard is watched may be known only once keys have come; and whether a
        // keyboard that goes would be noticed is another window's by the time that one has
        // taken keyboard and mouse.
        if (m_on && (m_watched != m_keys.isWatched() || m_goingIsTold != m_keys.isToldOfKeyboardsGoing())) {
            m_watched = m_keys.isWatched();
            m_goingIsTold = m_keys.isToldOfKeyboardsGoing();
            tell();
        }
        // What the mouse did and still waits behind a key: sent where it may go by now, and
        // not sent late.
        passTheMouse();
        // Nobody is at the keyboard or the mouse any more: a PC that was left does not keep its
        // keyboard on a machine. (Said in minutes, which is what the time is counted in.)
        if (m_on && m_clock.elapsed() - m_touchedAtMs >= m_pace.idleAfterMs) {
            stop(words("Keyboard and mouse were given back: no key was pressed and the mouse was not used for %1 "
                       "min. Every key on the machine was let go.")
                     .arg((m_pace.idleAfterMs + msInAMinute - 1) / msInAMinute));
        }
    });
    m_moves.setSingleShot(true);
    // (To the millisecond: a timer that may be early would say more than sixty places a second.)
    m_moves.setTimerType(Qt::PreciseTimer);
    m_moves.setInterval(m_pace.moveEveryMs);
    connect(&m_moves, &QTimer::timeout, this, &MachineControl::sayThePlace);
    m_answer.setSingleShot(true);
    m_answer.setTimerType(Qt::PreciseTimer);
    m_answer.setInterval(m_pace.answerWithinMs);
    connect(&m_answer, &QTimer::timeout, this, &MachineControl::answerTimeIsOver);
    m_owing.setSingleShot(true);
    // (To the millisecond: the button is busy for as long as is said, and not a part of it more.)
    m_owing.setTimerType(Qt::PreciseTimer);
    connect(&m_owing, &QTimer::timeout, this, [this] {
        // What has run out is owed no more; what has not is looked at again when it has.
        forgetWhatIsOverdue();
        waitForWhatIsOwed();
        tell();
    });
    m_owedIsSaid.setSingleShot(true);
    // (To the millisecond: one that may be early would find the time not over, and say nothing.)
    m_owedIsSaid.setTimerType(Qt::PreciseTimer);
    m_owedIsSaid.setInterval(m_pace.owedIsSaidAfterMs);
    connect(&m_owedIsSaid, &QTimer::timeout, this, [this] {
        // A word has been owed for longer than a machine that says its words takes: from here
        // the bar says why Control is not offered.
        m_owedForLong = !m_owed.isEmpty();
        tell();
    });
    tell();
}

MachineControl::~MachineControl()
{
    // Nothing is waited for: the key source is ended with this. What is still held stays in a
    // memory that the windows have together: a finger may be on it, and the next window that
    // takes keyboard and mouse is to know.
    m_ending = true;
    try {
        cancelText(false);
    } catch (...) {
        // A completion observer must not interrupt teardown or prevent input release.
        qWarning("A text completion observer failed during control teardown.");
    }
    disconnect(&m_kept, nullptr, this, nullptr);
    stop({});
}

QString MachineControl::sentenceFor(KeySource::Lost why)
{
    switch (why) {
    case KeySource::Lost::AnotherDesktop:
        return words("Windows showed a screen of its own (the lock screen, Ctrl+Alt+Del, or a question about "
                     "administrator rights). Keyboard and mouse were given back, and every key on the machine was "
                     "let go.");
    case KeySource::Lost::KeysOfWindows:
        return words("Windows itself acts on the keys that were pressed, whoever they were meant for: the Windows "
                     "key with L locks this PC, and Ctrl+Alt+Del brings Windows' own screen. Keyboard and mouse "
                     "were given back, and every key on the machine was let go.");
    case KeySource::Lost::NotLooking:
        return words("qKVM stopped looking at the keys of this PC. Keyboard and mouse were given back, and "
                     "every key on the machine was let go.");
    // A keyboard went. Said as what Windows said, with what it counts as a keyboard, and with
    // why that ends the control: which keyboard it was, and whether a key was held on it, is
    // not known here, and is not claimed.
    case KeySource::Lost::AKeyboardWent:
        return words("Windows reported that a keyboard was taken away from this PC: unplugged, or switched to "
                     "another computer (it counts some mice, headsets and docks as keyboards). A key that was held "
                     "on it would have stayed down on the machine, so keyboard and mouse were given back, and every "
                     "key on the machine was let go.");
    // Windows' other account of the keyboard began to tell while they were held, and a key of
    // its first moment found no hearing. True of both that it may have been: a key that was
    // handed over before anything was noted, and one that was not handed over. (What can be
    // done about it is said with it: `Remedy::AskAgain`.)
    case KeySource::Lost::BeganToTell:
        return words("Windows began to report this PC's keyboard to qKVM only after keyboard and mouse were "
                     "taken (a keyboard may have been plugged in), and a key from that first moment could not be "
                     "checked. Keyboard and mouse were given back, and every key on the machine was let go.");
    // The two accounts of the keyboard differed. Said as what was seen, and no more: which of
    // the two told of a key that the other did not; what may be why; and what can be done.
    // (Of this one, what is known is that Windows' other account of the keyboard, which had told
    // of keys, did not tell of this key. Not, that Windows did not see it on the keyboard: that
    // account may be told to this program no more, which is not followed here. So both are
    // said, and neither as what happened.)
    case KeySource::Lost::NotToldOfKeys:
        return words("qKVM was handed a key that Windows did not report from the keyboard. Windows may no "
                     "longer be reporting the keyboard to qKVM, or a key remapper, a macro tool or a Windows "
                     "key helper (Sticky, Filter or Mouse Keys) made or changed the key. Keyboard and mouse were "
                     "given back, and every key on the machine was let go. If it happens again, close such a "
                     "program or switch the helper off.");
    case KeySource::Lost::ToldDifferently:
        return words("Windows saw one key on the keyboard, and qKVM was handed another. A remapped key, a "
                     "program that changes keys or a Windows key helper (Sticky, Filter or Mouse Keys) may be why. "
                     "Keyboard and mouse were given back, and every key on the machine was let go. If it happens "
                     "again, close that program or switch the helper off.");
    case KeySource::Lost::NotHandedKeys:
        break;
    }
    // (Also what it looks like when Windows has taken the listening away: not told apart here.)
    return words("Windows saw a key on the keyboard that qKVM was not handed. Windows may have stopped "
                 "handing it keys, or a key remapper, a launcher or a Windows key helper (Sticky, Filter or Mouse "
                 "Keys) held the key back. Keyboard and mouse were given back, and every key on the machine was let "
                 "go. If it happens again, close that program or switch the helper off.");
}

bool MachineControl::isPossible() const
{
    return m_allowed && m_live != nullptr && m_live->isOpen();
}

void MachineControl::have(core::Live* live, const QString& machine)
{
    const bool changed = live != m_live || machine != m_machine;
    if (changed) {
        ++m_targetTurn;
        cancelTakeover();
    }
    if (live != m_live) {
        cancelText(false);
        // What is held through the one before is let go there, while it stands.
        stop({});
        if (m_live != nullptr) {
            disconnect(m_live.data(), nullptr, this, nullptr);
        }
        m_live = live;
        m_picture = {};
        // (A yes of the one before is no answer of this one's, and nothing that was asked of
        // the one before is owed here.)
        m_gaveUp = GaveUp::No;
        m_owed.clear();
        waitForWhatIsOwed();
        m_answer.stop();
        if (live != nullptr) {
            connect(live, &core::Live::controlOwnerChanged, this, [this] {
                if (m_takeover && (m_live == nullptr || *m_takeover != m_live->controlOwner()))
                    cancelTakeover();
                tell();
            });
            connect(live, &core::Live::controlChanged, this,
                    [this](bool on, const QString& code, const QString& message) {
                        answered(on, code, message);
                        textReleaseAnswered(on, code, message);
                    });
            connect(live, &core::Live::videoChanged, this, [this](const core::Live::Video& video) {
                m_picture = {video.width, video.height};
                tell();
            });
            connect(live, &core::Live::opened, this, &MachineControl::tell);
            connect(live, &core::Live::pointerModeChanged, this, &MachineControl::followPointerMode);
            // Something this program pressed may still be down on the machine: it could not
            // be let go from here, or the word for it may not have arrived. Whoever sits here
            // is the one who can do something about it, and is told.
            connect(live, &core::Live::leftDown, this, [this] {
                m_mayBeDown = true;
                tell();
            });
            // (For a live that was handed in by somebody who does not say so themselves. In the
            // program LivePicture hears of the end first and says it through lineEnded().)
            connect(live, &core::Live::closed, this,
                    [this](const QString&, const QString& message) { lineEnded(message); });
            connect(live, &QObject::destroyed, this, [this] { stop({}); });
        }
    }
    if (machine != m_machine) {
        cancelText(false);
        // (Why another machine's were not given is nothing to this one, nor what may be down
        // on another.)
        m_machine = machine;
        m_relative = false;
        m_problem.clear();
        m_mayBeDown = false;
        m_remedy = Remedy::None;
    }
    if (changed)
        followPointerMode();
    tell();
}

void MachineControl::lineEnded(const QString& message)
{
    cancelTakeover();
    cancelText(false);
    // (Where it held them, it has said so itself, with this sentence, and that is kept. Where
    // they were only asked for, this is the answer. And where neither, it is nothing to say.)
    if (m_on || m_asking) {
        stop(message);
    }
}

void MachineControl::setAllowed(bool allowed)
{
    if (allowed == m_allowed) {
        return;
    }
    m_allowed = allowed;
    if (!allowed) {
        cancelTakeover();
        cancelText(false);
        stop({});
    }
    tell();
}

void MachineControl::setInFront(bool inFront)
{
    if (inFront == m_inFront) {
        return;
    }
    m_inFront = inFront;
    if (!inFront) {
        cancelTakeover();
        cancelText(false);
        // Whoever asked for them is somewhere else by now: said as what it was, and a yes that
        // still comes is given back. (Where they were held, giving back says it all.)
        if (m_asking) {
            stop(words("This window was no longer in front before the machine answered, so keyboard and mouse "
                       "were not taken."),
                 Remedy::AskAgain);
            m_gaveUp = GaveUp::Otherwise;
        } else {
            stop({});
        }
    }
    tell();
}

void MachineControl::take()
{
    cancelTakeover();
    requestControl({});
}

void MachineControl::cancelTakeover()
{
    if (m_takeover) {
        m_takeover.reset();
        tell();
    }
}

void MachineControl::setTakeoverAllowed(bool allowed)
{
    if (m_takeoverAllowed == allowed)
        return;
    m_takeoverAllowed = allowed;
    cancelTakeover();
    if (!allowed && m_asking && m_takingOver)
        stop(words("Your account may no longer take over control."));
    tell();
}

bool MachineControl::mayTakeOver() const
{
    if (!m_takeoverAllowed || !isPossible() || !m_inFront || m_on || m_asking || !m_owed.isEmpty() || m_textHeld ||
        m_textRunning)
        return false;
    const auto owner = m_live->controlOwner();
    return !owner.anotherWindow && !owner.name.isEmpty() && !owner.reference.isEmpty();
}

void MachineControl::askTakeover()
{
    if (!mayTakeOver() || m_takeover)
        return;
    m_takeover = m_live->controlOwner();
    tell();
}

void MachineControl::answerTakeover(bool yes)
{
    const auto expected = std::exchange(m_takeover, std::nullopt);
    const auto live = m_live;
    const auto target = m_targetTurn;
    const bool valid = yes && expected && mayTakeOver() && *expected == m_live->controlOwner();
    tell();
    // Closing the confirmation emits view signals. A slot may switch sources, replace the
    // holder or refresh the account before it returns. Consent belongs to the original target.
    if (valid && live == m_live && target == m_targetTurn && mayTakeOver() && *expected == m_live->controlOwner())
        requestControl(expected->reference);
}

void MachineControl::requestControl(const QString& takeover)
{
    // Not while the machine owes a word: the answer to an asking made now could not be told
    // from it. (So an asking finds nothing owed, and the first word that comes is its answer.)
    forgetWhatIsOverdue();
    if (!isPossible() || !m_inFront || m_on || m_asking || !m_owed.isEmpty() || m_textHeld || m_textRunning) {
        return;
    }
    if (!takeover.isEmpty() && (!mayTakeOver() || takeover != m_live->controlOwner().reference))
        return;
    const auto live = m_live;
    const auto target = m_targetTurn;
    const auto owner = m_live->controlOwner();
    m_problem.clear();
    m_remedy = Remedy::None;
    m_gaveUp = GaveUp::No;
    m_asking = true;
    m_takingOver = !takeover.isEmpty();
    m_answer.start();
    tell();
    const auto stillValid = [&] {
        return m_asking && isPossible() && m_inFront && live == m_live && target == m_targetTurn &&
               (takeover.isEmpty() || (m_takeoverAllowed && owner == m_live->controlOwner()));
    };
    if (stillValid()) {
        m_live->chooseRelativeMouse(m_relative);
        if (stillValid()) {
            if (takeover.isEmpty())
                m_live->askControl(true);
            else
                m_live->takeOver(takeover);
            return;
        }
    }
    if (m_asking) {
        m_asking = false;
        m_answer.stop();
        tell();
    }
}

void MachineControl::giveBack()
{
    cancelTakeover();
    cancelText(false);
    stop({});
}

void MachineControl::pauseForText(const QString& machine)
{
    if (machine == m_machine && m_on && !m_textRunning) {
        stop({}, Remedy::None, Answer::Owed, true);
        m_textHeld = m_live != nullptr && m_live->hasControl();
        tell();
    }
}

void MachineControl::cancelText(bool resume)
{
    ++m_textTurn;
    m_textDeadline.stop();
    auto refused = std::move(m_textRefused);
    const bool waiting = m_textRunning && !m_textStarted;
    m_textStart = {};
    m_textRunning = false;
    m_textStarted = false;
    const bool held = std::exchange(m_textHeld, false);
    if (held) {
        if (resume && isPossible() && m_inFront && m_live->hasControl()) {
            begin();
        } else if (m_live != nullptr) {
            m_live->releaseAll();
            oweForAGivingBack(false, 0);
            m_live->askControl(false);
        }
    }
    tell();
    if (waiting && refused) {
        refused();
    }
}

void MachineControl::textAfterRelease(const QString& machine, TextStart start, std::function<void()> refused)
{
    // (Nor while something pressed from here may still be down on that machine: what is sent now could land beside it,
    // a Delete as Ctrl+Delete. Until keyboard and mouse were taken anew there, and it could be let go of.)
    if (m_textRunning || (machine == m_machine && (m_asking || m_mayBeDown))) {
        refused();
        return;
    }
    if (machine != m_machine || (!m_on && !m_textHeld && m_owed.isEmpty())) {
        start([](bool) {});
        return;
    }
    m_textResume = m_on || m_textHeld;
    m_textStart = std::move(start);
    m_textRefused = std::move(refused);
    m_textRunning = true;
    m_textStarted = false;
    ++m_textTurn;
    m_textDeadline.start();
    // Establish the pending state before releasing; a direct device can answer synchronously.
    stop({});
    tell();
}

void MachineControl::textReleaseAnswered(bool on, const QString& code, const QString& message)
{
    if (m_textHeld && !on) {
        cancelText(false);
    }
    if (!m_textRunning || m_textStarted) {
        return;
    }
    if (!on && code.isEmpty() && message.isEmpty() && m_owed.isEmpty() && isPossible() && !m_live->hasControl()) {
        // The plain word for the giving back, which a live says also where it could not let go of everything, and
        // said first that something may still be down (Live::leftDown): then nothing is sent, and that is why.
        if (m_mayBeDown) {
            cancelText(false);
        } else {
            startText();
        }
    } else if (!on && (!code.isEmpty() || !message.isEmpty())) {
        cancelText(false);
    }
}

void MachineControl::startText()
{
    m_textDeadline.stop();
    m_textStarted = true;
    auto start = std::move(m_textStart);
    m_textRefused = {};
    const quint64 turn = m_textTurn;
    start([guard = QPointer<MachineControl>(this), turn](bool success) {
        if (guard == nullptr || turn != guard->m_textTurn || !guard->m_textRunning) {
            return;
        }
        const bool resume = success && guard->m_textResume;
        guard->m_textRunning = false;
        guard->m_textStarted = false;
        guard->tell();
        if (resume) {
            guard->take();
        }
    });
}

void MachineControl::wentOn(bool byAKey)
{
    if (m_asking) {
        // Said as what happened: the machine was not late, somebody typed, or opened something.
        stop(byAKey ? words("A key was pressed before the machine answered, so keyboard and mouse were not taken.")
                    : words("Something else was opened in this window before the machine answered, so keyboard "
                            "and mouse were not taken."),
             Remedy::AskAgain);
        m_gaveUp = GaveUp::Otherwise;
    }
}

void MachineControl::oweForAGivingBack(bool ofAnAsking, int restOfTheAskingMs)
{
    // (The machine answers in the order it was asked: its word for the giving back comes no
    // sooner than its answer to the asking that the giving back ended, which has the rest of
    // its time.)
    const qint64 until = m_clock.elapsed() + (ofAnAsking ? restOfTheAskingMs : 0) + m_pace.owedForMs;
    if (ofAnAsking) {
        m_owed.add({true, until});
    }
    m_owed.add({false, until});
    waitForWhatIsOwed();
}

std::size_t MachineControl::OwedWords::firstAnswer() const noexcept
{
    for (std::size_t at = 0; at < m_count; ++at) {
        if (m_words[at].anAnswer) {
            return at;
        }
    }
    return most;
}

qint64 MachineControl::OwedWords::firstOverMs() const noexcept
{
    qint64 first = m_count > 0 ? m_words[0].untilMs : 0;
    for (std::size_t at = 1; at < m_count; ++at) {
        first = std::min(first, m_words[at].untilMs);
    }
    return first;
}

void MachineControl::OwedWords::add(const Owed& word) noexcept
{
    if (m_count == most) {
        dropFirst(1);
    }
    m_words[m_count] = word;
    ++m_count;
}

void MachineControl::OwedWords::dropFirst(std::size_t count) noexcept
{
    const std::size_t gone = std::min(count, m_count);
    for (std::size_t at = gone; at < m_count; ++at) {
        m_words[at - gone] = m_words[at];
    }
    m_count -= gone;
}

void MachineControl::OwedWords::dropOverdue(qint64 nowMs) noexcept
{
    std::size_t kept = 0;
    for (std::size_t at = 0; at < m_count; ++at) {
        if (m_words[at].untilMs > nowMs) {
            m_words[kept] = m_words[at];
            ++kept;
        }
    }
    m_count = kept;
}

void MachineControl::OwedWords::noLongerThan(qint64 untilMs) noexcept
{
    for (std::size_t at = 0; at < m_count; ++at) {
        m_words[at].untilMs = std::min(m_words[at].untilMs, untilMs);
    }
}

void MachineControl::forgetWhatIsOverdue()
{
    m_owed.dropOverdue(m_clock.elapsed());
    lookWhetherAWordIsOwed();
}

void MachineControl::lookWhetherAWordIsOwed()
{
    // (Nothing is begun as this one ends.)
    if (m_owed.isEmpty() || m_ending) {
        m_owedIsSaid.stop();
        m_owedForLong = false;
        return;
    }
    // From when one is owed, and not anew with each that is owed beside it: the button has
    // been busy since then.
    if (!m_owedForLong && !m_owedIsSaid.isActive()) {
        m_owedIsSaid.start();
    }
}

bool MachineControl::takeTheAnswerThatIsOwed()
{
    const std::size_t answer = m_owed.firstAnswer();
    if (answer == OwedWords::most) {
        return false;
    }
    // (And whatever is owed before it: the machine answers in the order it was asked.)
    m_owed.dropFirst(answer + 1);
    return true;
}

void MachineControl::theMachineSpoke()
{
    // What it still owes was asked before now, and it has come as far as what it just said: none
    // of that is owed for longer than a word is, from here.
    m_owed.noLongerThan(m_clock.elapsed() + m_pace.owedForMs);
    waitForWhatIsOwed();
    tell();
}

void MachineControl::waitForWhatIsOwed()
{
    lookWhetherAWordIsOwed();
    // (Nothing is begun as this one ends.)
    if (m_owed.isEmpty() || m_ending) {
        m_owing.stop();
        return;
    }
    const qint64 left = m_owed.firstOverMs() - m_clock.elapsed();
    m_owing.start(static_cast<int>(std::clamp<qint64>(left, 1, std::numeric_limits<int>::max())));
}

void MachineControl::answered(bool on, const QString& code, const QString& message)
{
    if (m_on) {
        // Held: a no is theirs, and they are taken away. (Nothing is owed while they are held:
        // nothing was as they were asked for.)
        if (!on) {
            stop(message.isEmpty() ? words("Keyboard and mouse of this machine were taken away.") : message);
        }
        return;
    }
    if (m_asking) {
        // Nothing was owed as they were asked for (`take`), and nothing is owed since: this is
        // the asking's own answer, whatever it is.
        if (on) {
            begin();
            return;
        }
        // A no: shown as what the machine said, where it said a sentence. (Its answer has come:
        // of this asking only the word for the giving back is owed from here.)
        stop(message.isEmpty() ? words("Keyboard and mouse of this machine were not given.") : message, Remedy::None,
             Answer::Came);
        return;
    }
    // Nobody waits: what the machine says is nobody's, and is held against what it owes.
    forgetWhatIsOverdue();
    if (!on && code == QLatin1String("given_up")) {
        // Its answer to an asking that was called off from here before it had answered: the
        // answer that is owed, and nothing to show. Its sentence says what was done here, and
        // why that was done was said then and stands: that the machine did not answer in time,
        // where it was that. (No yes comes for that asking any more; and its word for the
        // giving back that called it off comes behind this one, and stays owed.)
        static_cast<void>(takeTheAnswerThatIsOwed());
        m_gaveUp = GaveUp::No;
        theMachineSpoke();
        return;
    }
    if (!on && message.isEmpty()) {
        // A no without a sentence: the first of what is owed, whichever that is. (Where nothing
        // is owed, nothing.)
        m_owed.dropFirst(1);
        theMachineSpoke();
        return;
    }
    // A yes, or a no with its sentence: the answer to an asking, and never the machine's word
    // for a giving back. The first answer that is owed, with whatever is owed before it; where
    // none is owed, it comes after everything that was asked, and nothing of that is owed.
    if (!takeTheAnswerThatIsOwed()) {
        m_owed.clear();
    }
    if (on) {
        // Given, and nobody waits for them: given back at once. What is typed by now is typed
        // into something else. (And the machine's word for that is owed.)
        if (m_live != nullptr) {
            m_live->releaseAll();
            m_live->askControl(false);
            oweForAGivingBack(false, 0);
        }
        // (Why they were no longer waited for was said then. Where it was the time, the machine
        // has answered late, and that is what is said now.)
        if (m_gaveUp == GaveUp::TimeOver) {
            m_problem = words("Keyboard and mouse were not taken: the machine answered late.");
            m_remedy = Remedy::None;
        }
    } else if (m_gaveUp == GaveUp::TimeOver) {
        // Nobody waits any more, because the machine was late with this: it did answer, and
        // what it said is said in the place of "did not answer".
        m_problem = message;
        m_remedy = Remedy::None;
    }
    m_gaveUp = GaveUp::No;
    theMachineSpoke();
}

void MachineControl::answerTimeIsOver()
{
    // Not answered within the time the machine is given: said. The asking is over; its answer
    // and the machine's word for the giving back are owed from here, and nobody's.
    if (m_asking) {
        stop(words("The machine did not answer when its keyboard and mouse were asked for."));
        m_gaveUp = GaveUp::TimeOver;
    }
}

void MachineControl::heardNoMore(const QString& why, Remedy remedy)
{
    // (While nothing is taken, it is nothing.)
    if (m_on) {
        stop(why, remedy);
    }
}

void MachineControl::begin()
{
    ++m_inputTurn;
    m_localInput = false;
    m_asking = false;
    m_answer.stop();
    m_on = true;
    // (Whoever sits here has the machine's keyboard again: what may have stayed down there
    // can be pressed and let go.)
    m_mayBeDown = false;
    m_control.emplace(m_view.leaveKey());
    m_down.emplace();
    m_there.clear();
    m_windowsKey.reset();
    m_typed = false;
    touched();
    // What had been kept from Windows before, in whichever window, and was not heard to be let
    // go since, was not pressed for the machine now: Windows does not have it as down, and a key
    // source that begins anew did not hear it. (Noted before anything is swallowed for this
    // time.)
    m_keptBack = m_kept.all();
    m_fromBefore.clear();
    // Said at every taking: a key source that listens goes on, and one whose listening ended by
    // itself, or was lost, begins anew. Then swallowing, and what is down only after that: from
    // here on every key that goes down is told, with whether it was kept from Windows, so that
    // none goes down unknown between the two.
    m_listens = m_keys.begin(m_view.leaveKey());
    if (m_listens) {
        m_keys.stillThere();
        m_keys.setSwallowing(true);
    }
    if (!m_listens || !m_keys.swallows()) {
        stop(words("Windows did not let qKVM listen for keys. Keyboard and mouse were given back."));
        return;
    }
    m_counted = true;
    m_kept.controlBegan();
    // Those are the keys that are held from before, each silent since this moment, from which
    // the keys are heard: not since before the listening began, which may have taken a while.
    const qint64 heardFromMs = m_clock.elapsed();
    for (const std::uint16_t usage : m_keptBack) {
        m_fromBefore.emplace(usage, heardFromMs);
    }
    // Nor was what is down at this moment.
    for (const input::Pressed& key : m_keys.whatIsDown()) {
        if (const std::uint16_t usage = input::usageOf(key); usage != 0 && !m_there.contains(usage)) {
            m_keptBack.insert(usage);
        }
    }
    m_watched = m_keys.isWatched();
    m_goingIsTold = m_keys.isToldOfKeyboardsGoing();
    m_alive.start();
    tell();
}

void MachineControl::lookWhetherItGoesDownAnew(std::uint16_t usage)
{
    const auto held = m_fromBefore.find(usage);
    if (held == m_fromBefore.end()) {
        return;
    }
    const qint64 now = m_clock.elapsed();
    if (now - held->second <= m_pace.repeatsWithinMs) {
        // It may be held, and only said again: it stays kept back, until it goes up.
        held->second = now;
        return;
    }
    // A key that is held and said again is never silent for this long, and the keys were heard
    // all that time: the finger had left it, where nobody heard. This is a press.
    m_fromBefore.erase(held);
    m_keptBack.erase(usage);
    m_kept.letGo(usage);
}

void MachineControl::keyCame(const input::Pressed& key, bool down, bool swallowed, quint64 number)
{
    takeKey(key, down, swallowed);
    if (!m_on) {
        return;
    }
    // A key was told, going down or up: where it is one of those that were on their way when
    // the mouse did something (its number is no higher than the last of them had), one fewer of
    // them is to come before that, and what need not wait any longer goes to the machine now,
    // after this key. A key that was heard later and is told sooner is none of them.
    for (Waited& one : m_mouse) {
        if (number <= one.behind) {
            one.keysBefore = std::max(0, one.keysBefore - 1);
        }
    }
    passTheMouse();
    // (Which keys are kept back may be others now, and the bar names the modifiers among them.)
    tell();
}

void MachineControl::nothingIsDown()
{
    if (!m_on || m_localInput) {
        return;
    }
    // By the account of where the keys are heard, no key that it heard go down is down any
    // more, and everything that is down on the machine went down in its hearing: nothing stays
    // down there. This is the net beneath what the key source does itself: it tells of every
    // key it heard go down that it went up, or says that it hears no more (ui/win/
    // keyboard_hook), so by this word nothing is down on the machine any more, as far as this
    // one kept to what it was told. Where it did not, what is still down there is let go now.
    // (A Windows key that still waited to be said was pressed and let go.)
    sayTheWindowsKey();
    while (m_on && !m_there.empty()) {
        const std::uint16_t usage = *m_there.begin();
        m_kept.letGo(usage);
        say(usage, false);
        // (Out of the set whatever became of saying it: none is gone through twice.)
        m_there.erase(usage);
    }
}

void MachineControl::takeKey(const input::Pressed& key, bool down, bool swallowed)
{
    const std::uint16_t usage = input::usageOf(key);
    if (m_on && down && usage != 0) {
        lookWhetherItGoesDownAnew(usage);
    }
    // Somebody is at the keyboard: a key went up, or one went down that was not down. (A key
    // that is held and said again and again is nobody: something may lie on it.)
    const bool isSaidAgain =
        down && (usage == 0 || m_kept.isIn(usage) || m_keptBack.contains(usage) || m_there.contains(usage));
    if (m_on && !isSaidAgain) {
        touched();
    }
    if (usage != 0) {
        if (!down) {
            m_fromBefore.erase(usage);
            m_kept.letGo(usage);
        } else if (swallowed && (m_on || isKeptWhereTheKeysAreHeard(key))) {
            // Kept from Windows: remembered for as long as it is held, whatever becomes of the
            // control. (Told while nothing is taken, it was kept from Windows in the very moment
            // in which they were given back: so the key source says, which is asked.)
            m_kept.keep(usage);
        }
    }
    if (!m_on) {
        // (Only waited for: the keys that were kept from Windows, to be let go.)
        listenNoLongerThanNeeded();
        return;
    }
    if (m_localInput) {
        // Delayed hook events belong to the old capture, never to the remote panel.
        if (usage != 0) {
            if (down)
                m_keptBack.insert(usage);
            else
                m_keptBack.erase(usage);
        }
        return;
    }
    if (usage != 0) {
        const bool wasThere = m_there.contains(usage);
        if (down && !swallowed && !wasThere) {
            // It went to Windows (Windows had it already): it was typed into this PC, and is
            // kept back as a key that was down when they were taken. (One that is down on the
            // machine and is only said again stays the machine's.)
            m_keptBack.insert(usage);
        } else {
            pass(usage, down, key.virtualKey == nameOfL);
        }
        if (!m_on) {
            return;
        }
    }
    // Nothing is swallowed any more where the keys are heard, and this one was not told why. The
    // key source may know (keys are heard no more, and the word for it is on its way behind this
    // very key): asked for before anything is made of it, and that reason is the one that is
    // said. Where it has none, the leave-key gave the keyboard back there, where it is decided:
    // the machine's is given back here, whatever was made of the key itself.
    if (!m_keys.swallows()) {
        m_keys.tellNow();
        if (m_on) {
            stop({});
        }
    }
}

bool MachineControl::isKeptWhereTheKeysAreHeard(const input::Pressed& key) const
{
    const std::vector<input::Pressed> kept = m_keys.whatIsKept();
    return std::ranges::any_of(
        kept, [&](const input::Pressed& one) { return one.scanCode == key.scanCode && one.extended == key.extended; });
}

void MachineControl::pass(std::uint16_t usage, bool down, bool isL)
{
    if (!m_control) {
        return;
    }
    if (m_keptBack.contains(usage)) {
        if (!down) {
            m_keptBack.erase(usage);
            // (It never went down there. Were it down there all the same, it would go up there.)
            say(usage, false);
        }
        return;
    }
    using Does = input::ControlKeys::Does;
    // (A key that went down under the leave-key is nothing until it was let go itself, said
    // again or not, the leave-key up by then or not: that is input::ControlKeys' rule. And one
    // that is down on the machine and is only said again is no key going down: it uses the
    // leave-key for nothing.)
    const input::ControlKeys::Step step = m_control->take(usage, down, down && m_there.contains(usage));
    if (!down) {
        // The Windows key goes up by itself: it was no Win+L. Down and up together.
        if (m_windowsKey == usage) {
            sayTheWindowsKey();
        }
        // Whatever was made of the key: where it is down on the machine it goes up there. An
        // "up" that is lost leaves a key down on a production server.
        say(usage, false);
    } else if (step.does == Does::Send) {
        if (holdsBackAModifier()) {
            // On the machine that modifier is up: this key would arrive there bare.
            m_keptBack.insert(usage);
        } else if (isL && m_windowsKey.has_value()) {
            // L under a Windows key: with the two Windows locks this PC, whoever they were meant
            // for. Where the keys are heard that ends the control before L comes here. Were it
            // to come all the same, neither goes to the machine.
            m_keptBack.insert(usage);
            m_keptBack.insert(*m_windowsKey);
            m_windowsKey.reset();
        } else if (isWindowsKey(usage) && !m_there.contains(usage)) {
            // Not said yet: what comes next shows whether it is a Win+L. (The other Windows key
            // going down under it shows that it is none.)
            if (m_windowsKey != usage) {
                sayTheWindowsKey();
                m_windowsKey = usage;
            }
        } else {
            sayTheWindowsKey();
            say(usage, true);
        }
    }
    if (step.does == Does::Leave) {
        stop({});
    } else if (step.does == Does::CtrlAltDel) {
        ctrlAltDel();
    }
}

void MachineControl::say(std::uint16_t usage, bool down)
{
    if (m_on && m_down && m_live != nullptr && m_down->take(usage, down)) {
        if (down) {
            m_there.insert(usage);
        } else {
            m_there.erase(usage);
        }
        m_live->key(usage, down);
    }
}

void MachineControl::sayTheWindowsKey()
{
    if (m_windowsKey.has_value()) {
        const std::uint16_t waited = *m_windowsKey;
        m_windowsKey.reset();
        say(waited, true);
    }
}

bool MachineControl::holdsBackAModifier() const
{
    // One that is kept back here; or one that went down under the leave-key and is kept back
    // where the leave-key is known.
    const auto modifier = m_keptBack.lower_bound(input::usageLeftCtrl);
    return (modifier != m_keptBack.end() && input::isModifier(*modifier)) ||
           (m_control.has_value() && m_control->keepsBackAModifier());
}

void MachineControl::touched()
{
    m_touchedAtMs = m_clock.elapsed();
}

void MachineControl::ctrlAltDel()
{
    if (!m_on) {
        return;
    }
    touched();
    static constexpr std::array<std::uint16_t, 2> modifiers = {input::usageLeftCtrl, input::usageLeftAlt};
    const std::array<bool, 2> wasThere = {m_there.contains(modifiers[0]), m_there.contains(modifiers[1])};
    // Del that is down there already is let go first: it has to go down last, after Ctrl and
    // Alt, or the three are no Ctrl+Alt+Del. It stays up there afterwards, and is not pressed
    // again by its repeat: kept back until it was let go here.
    if (m_there.contains(input::usageDelete)) {
        say(input::usageDelete, false);
        m_keptBack.insert(input::usageDelete);
    }
    // Down in the order a hand presses them (one that is down there already is not said a
    // second time), and up the other way round.
    say(modifiers[0], true);
    say(modifiers[1], true);
    say(input::usageDelete, true);
    say(input::usageDelete, false);
    say(modifiers[1], false);
    say(modifiers[0], false);
    // What fingers still hold is as it was before: a modifier that was down there is down there
    // again, and the next key does not arrive bare.
    for (std::size_t which = 0; which < modifiers.size(); ++which) {
        if (wasThere.at(which)) {
            say(modifiers.at(which), true);
        }
    }
}

void MachineControl::releasePressed()
{
    // Remember physical presses before releasing the remote state; held repeats must
    // not become fresh input after a menu closes or after a screen-key chord.
    m_keptBack.insert(m_there.begin(), m_there.end());
    if (m_windowsKey)
        m_keptBack.insert(*m_windowsKey);
    m_there.clear();
    m_windowsKey.reset();
    m_control.emplace(m_view.leaveKey());
    m_down.emplace();
    m_buttons.clear();
    m_mouse.clear();
    m_moves.stop();
    m_place.reset();
    m_at.reset();
    m_said.reset();
    m_over = false;
    m_delta = {};
    m_turned = {};
    if (m_live != nullptr)
        m_live->releaseAll();
}

void MachineControl::setLocalInput(bool local)
{
    if (!m_on || !m_inFront || !isPossible() || local == m_localInput)
        return;
    const auto live = m_live;
    const quint64 turn = ++m_inputTurn;
    m_localInput = local;
    if (local) {
        m_captured = false;
        m_keys.setSwallowing(false);
        releasePressed();
    } else {
        m_keys.stillThere();
        m_keys.setSwallowing(true);
        if (!m_keys.swallows()) {
            stop(words("Windows did not let qKVM listen for keys. Keyboard and mouse were given back."));
            return;
        }
    }
    if (!m_on || live != m_live || turn != m_inputTurn)
        return;
    const auto down = m_keys.whatIsDown();
    if (!m_on || live != m_live || turn != m_inputTurn)
        return;
    for (const auto& key : down) {
        if (const auto usage = input::usageOf(key); usage != 0)
            m_keptBack.insert(usage);
    }
    touched();
    emit m_view.inputReset();
    if (m_on && live == m_live && turn == m_inputTurn)
        tell();
}

void MachineControl::tapKeys(const QList<int>& usages)
{
    if (!m_on || !m_inFront || !isPossible() || !m_live->hasControl() || usages.isEmpty() || usages.size() > 9)
        return;
    std::set<int> unique;
    int ordinary = 0;
    for (const int usage : usages) {
        if (usage < 4 || usage > 0xe7 || usage == 0x66 || !unique.insert(usage).second)
            return;
        if (!input::isModifier(static_cast<std::uint16_t>(usage)) && ++ordinary > 1)
            return;
    }
    const auto live = m_live;
    const quint64 turn = ++m_inputTurn;
    const auto current = [&] {
        return m_on && m_inFront && isPossible() && live == m_live && turn == m_inputTurn && live->hasControl();
    };
    const auto down = m_keys.whatIsDown();
    if (!current())
        return;
    for (const auto& key : down) {
        if (const auto usage = input::usageOf(key); usage != 0)
            m_keptBack.insert(usage);
    }
    releasePressed();
    if (!current())
        return;
    emit m_view.inputReset();
    if (!current())
        return;
    touched();
    for (const int usage : usages) {
        if (!current())
            return;
        say(static_cast<std::uint16_t>(usage), true);
    }
    for (auto key = usages.crbegin(); key != usages.crend(); ++key) {
        if (!current())
            return;
        say(static_cast<std::uint16_t>(*key), false);
    }
}

void MachineControl::pointerAt(double x, double y)
{
    if (!m_on || m_localInput || m_relative) {
        return;
    }
    touched();
    m_over = x >= 0.0 && x <= 1.0 && y >= 0.0 && y <= 1.0;
    if (!m_over) {
        m_place.reset();
        m_at.reset();
        return;
    }
    m_place = QPoint(static_cast<int>(std::lround(x * farthest)), static_cast<int>(std::lround(y * farthest)));
    m_at = m_place;
    // The first at once; while one was said a moment ago, the last of what follows.
    if (!m_moves.isActive()) {
        sayThePlace();
    }
}

void MachineControl::sayThePlace()
{
    if (m_relative) {
        sayRelative();
        return;
    }
    // (Not while something of the mouse waits behind a key: each of those lands where the
    // pointer was for it, and where the pointer is by now is said after them.)
    if (!m_on || !m_place || m_live == nullptr || !m_mouse.empty()) {
        return;
    }
    // (Noted as said before it is said: the live may end in the middle of being told, and then
    // nothing of this is as it was.)
    const QPoint place = *m_place;
    m_place.reset();
    m_said = place;
    m_moves.start();
    m_live->pointerAt(place.x(), place.y());
}

void MachineControl::setRelative(bool relative)
{
    if (m_on || m_asking || !m_owed.isEmpty() || !isPossible()) {
        return;
    }
    if (m_live->pointerMode() != core::Live::Pointer::Both) {
        return;
    }
    m_relative = relative;
    tell();
}

void MachineControl::followPointerMode()
{
    if (m_live == nullptr) {
        return;
    }
    const auto mode = m_live->pointerMode();
    if (mode == core::Live::Pointer::Both) {
        tell();
        return;
    }
    const bool relative = mode == core::Live::Pointer::Relative;
    if (relative != m_relative || mode == core::Live::Pointer::Unknown) {
        if (m_on || m_asking) {
            stop(words("The machine's mouse mode changed. Take control again to use the new mode."));
        }
        m_relative = relative;
    }
    tell();
}

void MachineControl::pointerBy(int dx, int dy)
{
    if (!m_on || m_localInput || !m_relative || !m_captured || (dx == 0 && dy == 0)) {
        return;
    }
    const qint64 x = static_cast<qint64>(m_delta.x()) + dx;
    const qint64 y = static_cast<qint64>(m_delta.y()) + dy;
    // Bound both memory and transport work. A corrupt or stalled input stream ends
    // control rather than sending a large jump, truncating it or replaying it later.
    if (std::abs(x) > 32767 || std::abs(y) > 32767) {
        stop(notInTime());
        return;
    }
    touched();
    m_delta = {static_cast<int>(x), static_cast<int>(y)};
    if (!m_moves.isActive()) {
        sayRelative();
    }
}

void MachineControl::sayRelative()
{
    if (!m_on || !m_relative || !m_captured || m_delta.isNull()) {
        return;
    }
    const QPoint delta = std::exchange(m_delta, {});
    m_moves.start();
    if (theMouseWaits()) {
        holdBack({Waited::What::Move, 0, 0, 0, delta});
    } else {
        moveRelative(delta);
    }
}

void MachineControl::moveRelative(QPoint delta)
{
    // Stay below the per-message limit of both live transports (kvmd accepts up
    // to 32 HID steps of 127). Keep the whole distance and its direction.
    constexpr int part = 2048;
    const int count = (std::max(std::abs(delta.x()), std::abs(delta.y())) + part - 1) / part;
    for (int step = 1; step <= count && m_on && m_relative && m_captured && m_live != nullptr; ++step) {
        m_live->pointerBy(delta.x() * step / count - delta.x() * (step - 1) / count,
                          delta.y() * step / count - delta.y() * (step - 1) / count);
    }
}

bool MachineControl::theMouseWaits() const
{
    return !m_mouse.empty() || m_keys.onTheirWay().count > 0;
}

void MachineControl::holdBack(Waited what)
{
    if (m_mouse.size() >= mostWaiting) {
        // More than waits at a time: as with keys that there is no room to tell of.
        stop(notInTime());
        return;
    }
    // Behind the keys that are on their way at this moment, those that go up as those that go
    // down (a modifier that was let go before a click is up on the machine before the click),
    // and behind what waits already.
    const KeySource::OnTheirWay way = m_keys.onTheirWay();
    what.behind = way.last;
    what.keysBefore = std::max(0, way.count);
    what.sinceMs = m_clock.elapsed();
    m_mouse.push_back(what);
}

void MachineControl::passTheMouse()
{
    while (m_on && !m_mouse.empty()) {
        const Waited next = m_mouse.front();
        if (m_clock.elapsed() - next.sinceMs >= m_pace.lateAfterMs) {
            // What waited too long is not sent late, and nothing behind it is sent at all.
            stop(notInTime());
            return;
        }
        if (next.keysBefore > 0) {
            return;
        }
        m_mouse.erase(m_mouse.begin());
        switch (next.what) {
        case Waited::What::Press:
            press(next.number, next.place);
            break;
        case Waited::What::Release:
            release(next.number, next.place);
            break;
        case Waited::What::Wheel:
            turn(next.away, next.right, next.place);
            break;
        case Waited::What::Move:
            moveRelative(next.place);
            break;
        }
        // (Where the pointer is by now, after the last of them, where that is another place.)
        if (m_on && m_mouse.empty()) {
            m_place = m_at != m_said ? m_at : std::optional<QPoint>();
            sayThePlace();
        }
    }
}

void MachineControl::sayWhereItWas(const std::optional<QPoint>& at)
{
    if (m_relative) {
        // A queued click/turn already has its preceding movement in the queue.
        // Movement received since that click belongs after it.
        if (!at.has_value()) {
            sayRelative();
        }
        return;
    }
    if (!at.has_value()) {
        // Now: where the pointer is, where that was not said yet.
        sayThePlace();
        return;
    }
    // Where it was then, unless that is where the machine has the pointer already. (Noted as
    // said before it is said: the live may end in the middle of being told.)
    if (m_said == at || m_live == nullptr) {
        return;
    }
    m_said = at;
    m_moves.start();
    m_live->pointerAt(at->x(), at->y());
}

void MachineControl::button(int number, bool down)
{
    if (!m_on || m_localInput || m_live == nullptr) {
        return;
    }
    touched();
    // What waits of this button, where something does: the last of it says whether the button
    // will be down on the machine once all of it went out.
    const auto last = std::find_if(m_mouse.crbegin(), m_mouse.crend(), [number](const Waited& one) {
        return (one.what == Waited::What::Press || one.what == Waited::What::Release) && one.number == number;
    });
    const bool waits = last != m_mouse.crend();
    const bool isDown = waits ? last->what == Waited::What::Press : m_buttons.contains(number);
    if (!down) {
        // Behind its own going down, where that still waits (let go beside the picture: where
        // it went down). One whose press has gone out goes up at once: letting go needs
        // nobody's word.
        if (!waits) {
            release(number, std::nullopt);
        } else if (isDown) {
            holdBack({Waited::What::Release, number, 0, 0, m_at.has_value() ? *m_at : last->place});
        }
        return;
    }
    if (number < 1 || number > 5 || (m_relative ? !m_captured : (!m_over || !m_at.has_value())) || isDown) {
        return;
    }
    // Not before a key that was pressed before it and is still to be told: Shift, then a click,
    // is no bare click.
    if (m_relative) {
        sayRelative();
    }
    if (theMouseWaits()) {
        holdBack({Waited::What::Press, number, 0, 0, m_at.value_or(QPoint())});
        return;
    }
    press(number, std::nullopt);
}

void MachineControl::press(int number, const std::optional<QPoint>& at)
{
    if (!m_on || m_live == nullptr || holdsBackAModifier() || m_buttons.contains(number)) {
        return;
    }
    // A click lands where the pointer is, not where it was said to be last.
    sayWhereItWas(at);
    // A click under a Windows key that waited is a click with the Windows key, and no Win+L.
    sayTheWindowsKey();
    // The live may have ended under that, and with it the holding: then nothing is pressed, and
    // nothing is noted as down for the next time.
    if (!m_on || m_live == nullptr) {
        return;
    }
    m_buttons.insert(number);
    m_live->button(number, true);
}

void MachineControl::release(int number, const std::optional<QPoint>& at)
{
    // Up only where it went down there, and where the pointer is by now: what is dragged is
    // dropped there.
    if (!m_on || m_live == nullptr || m_buttons.erase(number) == 0) {
        return;
    }
    sayWhereItWas(at);
    // (Where the live ended under that, everything was let go there already.)
    if (m_on && m_live != nullptr) {
        m_live->button(number, false);
    }
}

void MachineControl::wheel(int angleX, int angleY)
{
    if (m_on) {
        touched();
    }
    if (!m_on || m_localInput || (m_relative ? !m_captured : (!m_over || !m_at.has_value())) || m_live == nullptr) {
        m_turned = {};
        return;
    }
    m_turned += QPoint(angleX, angleY);
    // Away from the hand is positive, as Windows has it (WM_MOUSEWHEEL) and Qt after it: the
    // machine scrolls up, as this PC would. Sideways Windows says a turn to the right as
    // positive (WM_MOUSEHWHEEL) and Qt one to the left: turned around here.
    const int away = std::clamp(m_turned.y() / notch, -mostNotches, mostNotches);
    const int right = std::clamp(-(m_turned.x() / notch), -mostNotches, mostNotches);
    m_turned = {m_turned.x() % notch, m_turned.y() % notch};
    if (away == 0 && right == 0) {
        return;
    }
    // (Not before a key that was pressed before it and is still to be told, as a click.)
    if (m_relative) {
        sayRelative();
    }
    if (theMouseWaits()) {
        holdBack({Waited::What::Wheel, 0, away, right, m_at.value_or(QPoint())});
        return;
    }
    turn(away, right, std::nullopt);
}

void MachineControl::turn(int away, int right, const std::optional<QPoint>& at)
{
    if (!m_on || m_live == nullptr) {
        return;
    }
    sayWhereItWas(at);
    // (Each only while they are still held: the live may end under any of them.)
    for (int said = 0; said < std::abs(away) && m_on && m_live != nullptr; ++said) {
        m_live->wheel(0, away > 0 ? 1 : -1);
    }
    for (int said = 0; said < std::abs(right) && m_on && m_live != nullptr; ++said) {
        m_live->wheel(right > 0 ? 1 : -1, 0);
    }
}

void MachineControl::stop(const QString& problem, Remedy remedy, Answer answer, bool keepOwnership)
{
    ++m_inputTurn;
    m_localInput = false;
    const bool held = m_on || std::exchange(m_textHeld, false);
    const bool asked = m_asking;
    // (What an asking that ends here had left of its time: its answer may still come within
    // that, and is owed for as long. See below.)
    const int restOfTheAsking = asked ? std::max(0, m_answer.remainingTime()) : 0;
    m_on = false;
    m_asking = false;
    m_answer.stop();
    m_alive.stop();
    m_moves.stop();
    m_control.reset();
    m_down.reset();
    m_there.clear();
    m_keptBack.clear();
    m_fromBefore.clear();
    m_windowsKey.reset();
    m_buttons.clear();
    m_place.reset();
    m_at.reset();
    m_said.reset();
    m_over = false;
    m_captured = false;
    m_delta = {};
    m_turned = {};
    // What the mouse did and still waited behind a key never reached the machine: dropped.
    m_mouse.clear();
    m_typed = false;
    if (held || asked) {
        // The machine first: everything that is down there is let go, and the live is told,
        // where it still stands. (What it answers to that finds nothing held or asked for: its
        // word for it is owed, and where an asking ends so that the machine has not answered,
        // its answer to that asking before it. They are not asked for again while either is.)
        if (m_live != nullptr) {
            m_live->releaseAll();
            if (!keepOwnership) {
                oweForAGivingBack(asked && answer == Answer::Owed, restOfTheAsking);
                m_live->askControl(false);
            }
        }
        m_problem = problem;
        m_remedy = remedy;
        if (held) {
            m_keys.setSwallowing(false);
            // What it has kept from Windows and has not heard go up is still held, told of or
            // not: a press that was kept in the moment in which keys were heard no more is told
            // to nobody, and Windows does not know of it either.
            // Set, and not shown by a test: that no key is kept from Windows between the line
            // above and this one. A key that was looked at on the keys' own thread in that
            // very instant may be: it would be unknown at the next taking.
            for (const input::Pressed& key : m_keys.whatIsKept()) {
                m_kept.keep(input::usageOf(key));
            }
        }
    }
    if (m_counted) {
        m_counted = false;
        m_kept.controlEnded();
    }
    listenNoLongerThanNeeded();
    tell();
}

void MachineControl::listenNoLongerThanNeeded()
{
    if (!m_listens || m_on) {
        return;
    }
    if (!m_kept.isEmpty() && !m_ending) {
        // A finger is still on a key that Windows never saw go down. Were they taken again now,
        // nobody but the key source would hear it go up: it listens on, swallowing nothing,
        // until the key was let go, and not for ever (the memory forgets, and says so).
        return;
    }
    m_listens = false;
    m_keys.end();
}

QString MachineControl::note() const
{
    if (!m_on) {
        return {};
    }
    // A modifier that is kept back keeps everything else from the machine: said, with which key
    // it is and what ends it, so that nothing is eaten without a word. Whyever it is kept back:
    // held from before (it goes on being so until it is let go or seen to go down anew), or
    // kept back since they were taken (down then; gone to Windows; pressed under another that
    // is kept back, or under the leave-key), each until it is let go.
    QStringList notes;
    QStringList fromBefore;
    std::set<std::uint16_t> since;
    for (const auto& key : m_fromBefore) {
        if (input::isModifier(key.first)) {
            fromBefore.append(nameOf(key.first));
        }
    }
    for (const std::uint16_t usage : m_keptBack) {
        if (input::isModifier(usage) && !m_fromBefore.contains(usage)) {
            since.insert(usage);
        }
    }
    if (m_control.has_value()) {
        for (const std::uint16_t usage : m_control->keptBack()) {
            if (input::isModifier(usage)) {
                since.insert(usage);
            }
        }
    }
    if (!fromBefore.isEmpty()) {
        notes.append(words("Held when keyboard and mouse were last given back, and not heard to be let go since: "
                           "%1. Until each is let go or pressed anew, no other key and no click goes to the machine.")
                         .arg(fromBefore.join(QStringLiteral(", "))));
    }
    if (!since.empty()) {
        QStringList names;
        for (const std::uint16_t usage : since) {
            names.append(nameOf(usage));
        }
        notes.append(words("Kept back from the machine until it is let go: %1. Until then, no other key and no "
                           "click goes to the machine.")
                         .arg(names.join(QStringLiteral(", "))));
    }
    // What another program types goes to no machine: said from the first such key on.
    if (m_typed) {
        notes.append(words("Keys that another program types (a remote-control program, a keyboard shared over the "
                           "network, the on-screen keyboard) are not sent to the machine: only what is pressed on "
                           "this PC's own keyboard is."));
    }
    // This PC gives no second account of its keyboard: said short, while they are taken.
    if (!m_watched) {
        notes.append(words("If Windows stops passing keys to qKVM, that is not noticed on this PC: click "
                           "Control to give keyboard and mouse back."));
    }
    // Nor does it say when a keyboard is taken away: a key that was held on one stays down on
    // the machine, and nothing here would end that. (Of a keyboard whose wireless link drops
    // behind a receiver that stays plugged in, Windows says nothing on any PC: that is not
    // claimed by the line's absence, and stands as open.)
    if (!m_goingIsTold) {
        notes.append(words("If a keyboard is unplugged while a key is held, that is not noticed on this PC, and the "
                           "key stays down on the machine: click Control to give keyboard and mouse back."));
    }
    return notes.join(QLatin1Char(' '));
}

QString MachineControl::whyControlIsNotOffered() const
{
    // Said once a word has been owed for a moment, and not before: a machine that says its
    // words has said them by then, and the bar does not change twice at every giving back.
    // True whatever the giving back came of (it was asked for; an asking was given up, was
    // refused or got no answer; they were taken away): the machine was told that keyboard and
    // mouse are asked of it no longer, has not said its word for that, and they are not asked
    // for until it has, or the time for that is over. And only where Control is there to be
    // clicked: of a machine that is not had live any more, or by whoever may not act, nothing
    // is said of it.
    if (m_owed.isEmpty() || !m_owedForLong || !isPossible()) {
        return {};
    }
    return words("The machine has still to confirm that its keyboard and mouse are no longer held or asked for here. "
                 "Control can be clicked again once it has, or after a few seconds.");
}

QString MachineControl::problemAsItIsSaid() const
{
    if (!m_problem.isEmpty() && m_remedy == Remedy::AskAgain) {
        // What can be done about it is said as it is at this moment: nothing is asked of a
        // machine that still owes a word, and nobody is told to click what does nothing.
        return m_problem + QLatin1Char(' ') +
               (m_owed.isEmpty() ? words("Click Control again.")
                                 : words("Control can be clicked again once the machine has answered, or after the "
                                         "time it is given for that."));
    }
    // And where nothing was said of clicking: why Control does nothing, for as long as that is
    // so. (It was dead for the whole time a word was owed, with nothing on the screen for why.)
    const QString why = whyControlIsNotOffered();
    if (m_problem.isEmpty() || why.isEmpty()) {
        return m_problem + why;
    }
    return m_problem + QLatin1Char(' ') + why;
}

void MachineControl::tell()
{
    ControlView::Shown shown;
    shown.possible = isPossible();
    shown.on = m_on;
    shown.localInput = m_localInput;
    shown.asking = m_asking;
    // (The machine owes a word: they are not asked for until it has come, or its time is over.)
    shown.busy = !m_owed.isEmpty() || m_textHeld || m_textRunning;
    shown.watched = !m_on || m_watched;
    // What may still be down on the machine is said where the bar says things: behind why
    // keyboard and mouse went, or before the notes of a time of holding them.
    // (What to do about it is said as it can be done: they are held, or have to be taken first.)
    QString mayBeDown;
    if (m_mayBeDown) {
        mayBeDown = m_on ? words("Something that was pressed before may still be down on this machine: the word "
                                 "that lets go of it may not have arrived. Press and let go of it.")
                         : words("Something that was pressed may still be down on this machine: the word that lets "
                                 "go of it may not have arrived. Take keyboard and mouse again, and press and let "
                                 "go of it.");
    }
    const auto both = [](const QString& first, const QString& second) {
        return first.isEmpty() || second.isEmpty() ? first + second : first + u' ' + second;
    };
    shown.problem = both(problemAsItIsSaid(), m_on ? QString() : mayBeDown);
    shown.note = both(m_on ? mayBeDown : QString(), note());
    shown.picture = m_picture;
    shown.relative = m_relative;
    shown.mouseSelectable = m_live != nullptr && m_live->pointerMode() == core::Live::Pointer::Both;
    shown.textAllowed = m_textAllowed;
    shown.pointerCaptured = m_captured;
    const auto owner = m_live != nullptr && m_live->isOpen() ? m_live->controlOwner() : core::Live::ControlOwner{};
    shown.ownerName = owner.name;
    shown.ownerInAnotherWindow = owner.anotherWindow;
    shown.takeoverPossible = mayTakeOver();
    shown.takeoverPending = m_takeover.has_value();
    shown.takeoverOwner = m_takeover ? m_takeover->name : QString();
    m_view.show(shown);
}

} // namespace qkvm::ui
