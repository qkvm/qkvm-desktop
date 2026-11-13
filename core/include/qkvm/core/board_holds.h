#pragma once

#include <functional>
#include <limits>
#include <memory>
#include <optional>

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QUrl>

namespace qkvm::core {

// Until when each KVM's power board may still be pressing a button (SPEC.md section 8; the web's `_AllPaced` and
// `board_until`, ../app/qkvm/kvm.py, and rule 23 of its FOR-THE-DESKTOP-APP): kept apart from the drivers, which are
// made anew whenever their KVM is changed. State that paces something never lives in an object that is made anew: a
// hold that lived in a driver began at nothing with each new one, and a press could follow one that had no clear
// answer at once. A power button pressed twice undoes itself; a reset inside a long press resets a machine on its way
// down.
//
// * A press is held from when its click goes out (`out`), for as long as a click with no answer can still come to be
//   pressed and held: the board may take it as late as its answer is given up on. Its answer settles it: a yes, for
//   the button's own hold from then (`pressed`); a no, nothing (`notPressed`). An answer that says neither (none in
//   time, one that broke off, the KVM's 5xx), or a driver let go of while its click is out, holds it that long again
//   from then (`mayBePressing`), as the web holds it after such a press. A click whose answer is still to come holds it
//   however long it takes, past its time (a line that trickles): twice its time at the most, for a line that never
//   ends (from the merged review, 9). A driver reserves the board (`reserve`) from its look at a press to its click's
//   going out, and lets go of that where nothing went (`notPressed`): a reservation stands for as long as its holder's
//   road runs, whatever that takes (a battery question, a sign-in, a line to the KVM's events), and is given back by
//   its holder (its click's `out`, or `notPressed`, its driver let go of included); only one whose holder is lost runs
//   out, after reservedMostMs, far above the longest road (review 4's LOW 3: one held only for its click's own time
//   ran out while a sign-in before the click went on, and another driver's press went beside it).
// * A KVM is found by its address as written, and by the certificate it is known by where it is known by one: a card
//   edited to another address of the same KVM finds the hold there, as the web finds it. Over plain HTTP nothing tells
//   a KVM at another address from another one, and it starts afresh there. Not across a restart of the program.
// * The machine's power buttons are one: a press of a Fingerbot's on the same machine (kvmd_fingerbot.cpp) is held by
//   the same hold and holds it the same way (the web's FINGERBOT_PRESSES share it), and the hold says which of the two
//   holds it (`By`), for the words of a refusal.
// * Beside the hold, what the KVM said last of a Fingerbot paired with it, and its battery (Fingerbot): the web keeps
//   them in the same place (its Paced), for the same reason. A KVM says whether one is paired only on a line to its
//   events, which is not opened for that alone: a driver made anew would not know until somebody watched again.
// * Room is made of what holds nothing any more, once more than `most` are kept; what holds is never let go of.
//
// On the interface thread.
class BoardHolds {
public:
    static constexpr qsizetype mostKept = 4096;
    // The longest a reservation stands whose holder never gives it back: five minutes, where a press's road before its
    // click takes some forty seconds at the very most.
    static constexpr qint64 reservedMostMs = 300'000;
    // Which of the machine's power buttons holds it: its KVM's power board, or a Fingerbot paired with the KVM.
    enum class By : quint8 { board, fingerbot };
    // A time that is long past, by nowMs().
    static constexpr qint64 longAgo = std::numeric_limits<qint64>::min() / 4;
    // `clockMs`: milliseconds on a steady clock (a test's own); none: this program's.
    explicit BoardHolds(qsizetype most = mostKept, std::function<qint64()> clockMs = {});
    ~BoardHolds();
    BoardHolds(const BoardHolds&) = delete;
    BoardHolds& operator=(const BoardHolds&) = delete;
    BoardHolds(BoardHolds&&) = delete;
    BoardHolds& operator=(BoardHolds&&) = delete;

    // The program's own, for every driver that is given no other.
    static BoardHolds& program();

    // What a KVM is found by.
    struct Of {
        QUrl address;
        QByteArray certificate; // its SHA-256, where it is known by one; else nothing
    };

    // Milliseconds it may still be pressing; 0 where it is not.
    qint64 heldMs(const Of& kvm) const;
    // Whether what holds it now is a press that came to no clear answer (and not one the board said yes to).
    bool unclear(const Of& kvm) const;
    // Whether a click is out to it now (or a press is on its way to one: reserved), its answer not heard yet.
    bool pressing(const Of& kvm) const;
    // Whether what is out to it now is click `press`, and no other.
    bool stillOut(const Of& kvm, quint64 press) const;
    // Which button's press holds it now, where one does: the one whose click (or reserved look) is out, else the one
    // whose hold ends last; the board's where none does.
    By heldBy(const Of& kvm) const;
    // A click goes out now, to be held for `forMs` at the most (as long as its answer may take and the button's hold):
    // its number, by which its answer settles it. `by`: whose click it is.
    quint64 out(const Of& kvm, qint64 forMs, By by = By::board);
    // A press of `by`'s is on its way to its click (its look, a question before it, a sign-in): the machine is held for
    // it from now, as a click out holds it, until its holder gives it back (`out` as its click goes, which takes it
    // over, or `notPressed`), reservedMostMs at the most. Its number, as `out`'s.
    quint64 reserve(const Of& kvm, By by = By::board);
    // The board said yes to click `press`: it holds the button for `forMs` from now. A hold that is longer stands.
    void pressed(const Of& kvm, quint64 press, qint64 forMs, By by = By::board);
    // The board said no to click `press`: nothing was pressed, and that click holds nothing. (Where another click is
    // out by now, or another hold came, that stands.)
    void notPressed(const Of& kvm, quint64 press);
    // What a click came to is not known (no answer in time, one that broke off, the KVM's 5xx, its driver let go of
    // while it was out): the board may take it as late as now, and hold the button, for `forMs` from now.
    void mayBePressing(const Of& kvm, qint64 forMs, By by = By::board);

    // What the KVM said last of a Fingerbot paired with it, and of its battery (the web's Paced: `fingerbot`,
    // `fingerbot_heard_at`, `fingerbot_silent_at`, `battery`, `battery_next`, `battery_asked_at`). Times by nowMs().
    struct Fingerbot {
        std::optional<bool> paired; // as the KVM said last on its line for events; nothing: never said
        qint64 saidAt = longAgo;    // when it said so
        qint64 silentAt = longAgo;  // when a line opened to ask it heard nothing
        std::optional<int> battery; // in percent, as last answered
        qint64 batteryNext = 0;     // when it may be asked again
        qint64 batteryAskedAt = longAgo;
        // A question to the Fingerbot (its battery) is out, of whichever driver of the KVM, until then at the most: the
        // Comet talks to its Fingerbot one request at a time. 0: none is.
        qint64 askedUntil = 0;
        bool operator==(const Fingerbot&) const = default;
    };
    Fingerbot fingerbot(const Of& kvm) const;
    void keepFingerbot(const Of& kvm, const Fingerbot& said);
    // The clock the holds and the Fingerbot's times are kept by.
    qint64 nowMs() const { return now(); }

private:
    struct Hold {
        qint64 untilMs = 0;
        bool unclear = false;
        quint64 out = 0;           // the click that is out, by its number; 0: none
        qint64 outUntilMs = 0;     // while it is out, held until then at least: twice its time from when it went
        qint64 settledUntilMs = 0; // what held it before that click went out
        bool settledUnclear = false;
        By by = By::board;    // whose press holds it until untilMs (not changed while a click is out)
        By outBy = By::board; // whose the click that is out is
        Fingerbot bot;
        // Whether it holds nothing any more, and keeps no word of the KVM's (room may be made of it).
        bool idle(qint64 at) const;
    };
    // The keys it is found by.
    static QString byAddress(const Of& kvm);
    static QString byCertificate(const Of& kvm);
    // What holds it, where anything is kept; none where nothing is.
    std::shared_ptr<Hold> find(const Of& kvm) const;
    // What holds it, kept from now on under both its keys (one found under one of them is put under the other too;
    // two found, one under each, become one, the longer hold of the two standing).
    std::shared_ptr<Hold> of(const Of& kvm);
    void makeRoom();
    qint64 now() const;

    qsizetype m_most;
    std::function<qint64()> m_clockMs;
    QHash<QString, std::shared_ptr<Hold>> m_kept;
    quint64 m_clicks = 0;
};

} // namespace qkvm::core
