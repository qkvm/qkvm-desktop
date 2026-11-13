#pragma once

#include <functional>
#include <list>
#include <memory>
#include <optional>

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringView>
#include <QUrl>

namespace qkvm::core {

// Which passwords the KVMs of this PC's wall have said no to, and when each may be offered again
// (SPEC.md section 8): the web's `Rejected` (../app/qkvm/kvm.py), for every family's driver. Kept
// apart from the drivers, which are made anew whenever their KVM is changed: a pause that lived in a
// driver began at nothing with each new one, and a device that locks an address out after a few
// wrong passwords (a JetKVM does after five) was offered the wrong one again at once. What is kept is
// a digest of where the sign-in goes, who signs in and the password: never the password. Where the
// program keeps it beside its settings (PacingFile, a9), what is kept outlives the program; nothing of the
// password is written there either.
//
// * A no in a row waits longer each time: a minute, five, fifteen, then an hour (`Steps`).
//   A password that was refused does not become right by being offered again.
// * A sign-in that came to neither yes nor no (nothing answered it, or an answer that is no word on
//   the password) is never a no, and is not sent again at every look either: after ten seconds, then
//   twice as long each time, five minutes at the most.
// * Counted as it goes out (`sent`), and put back where it did not after all (`putBack`): noted only
//   once its answer came, a sign-in whose driver was let go meanwhile left nothing behind. While it is
//   out, nobody else sends the same, and what it comes to (a no, a yes, nothing, "come back then", put
//   back) ends that: the end of its line, which every family's sign-in has of its own and which
//   outlives its driver (a driver let go of mid sign-in leaves its line to hear the answer). Held by
//   the clock only for one that ran out of time ten seconds after it went, a no that a slow controller
//   sent after twelve came after the hold, and the other driver of the same KVM sent the wrong password
//   a second time; by the clock the hold is only a cap now, for a line that never ends: twice as long
//   as it can be out (Steps::outMs).
// * Whoever else would send it while it is out, and nothing but that stands (no no before it), waits
//   for what it comes to (`whenSettled`), and then looks again: as the web's second asker waits for
//   the one sign-in of its connection. A no is then that one's no too; after a yes it signs in itself.
// * A device's own "come back then" (a JetKVM's 429) is no word on the password (`later`).
// * A password a person has just typed for a KVM is tried as for the first time (`forget`): its noes
//   and the sign-ins that came to nothing are forgotten. A sign-in with it that is out is not: the
//   typed one waits for it, and one password at a time goes to the KVM still.
// * Room is made only of what protects nothing any more (from the outside review of the desktop, F3): a
//   hold that has run out, with no sign-in out and no no that still counts (from the merged review, 6:
//   the next no after it waits longer); the oldest of that first. A sign-in that is out, a hold that has
//   not ended, and a count of noes are never let go of to make room: one dropped was a password offered
//   again before its time, beside one on the wire, or offered as for the first time. Where all that is kept still
//   protects, more than the most are kept, and a KVM new to this is let in as ever, with nothing standing against it
//   (no "wrong password", no wait it did not earn); room is made of those holds once they have run out, but of a count
//   of noes, which is kept. What can be kept so is bounded by what goes out: every one stands for a sign-in that went
//   (or what it came to; a count of noes, for a password a KVM refused); the drivers pace each, and a new one comes
//   only of a new KVM, a new address, a new user or password, and goes where its KVM is taken off the wall
//   (forgetPlace).
//
// On the interface thread.
class SignInPacing {
public:
    // The first wait of each kind, which is the driver's (KvmdDevice::Pace: a minute and ten seconds,
    // the web's; a test's are short): after a no in a row, `refusedMs`, then five, fifteen and sixty
    // times as long (the web's minute, five, fifteen, an hour); after one that came to nothing,
    // `unansweredMs`, twice as long each time, thirty times as long at the most (ten seconds to five
    // minutes). And the longest a sign-in of the driver's can be out (`outFor`): one that is out is held
    // by the clock twice as long at the most (its line's end ends the hold first). 0: no longer than
    // the wait after one that came to nothing.
    struct Steps {
        qint64 refusedMs = 60'000;
        qint64 unansweredMs = 10'000;
        qint64 outMs = 0;
    };
    // The longest a sign-in sent on a line with these times (DeviceAsker::Times, the sign-in's own wait
    // for an answer as `quietMs`) can be out: the whole that line gives a question (DeviceAsker::send).
    static constexpr qint64 outFor(int connectWithinMs, int quietMs, int transferMs)
    {
        return qint64{connectWithinMs} + quietMs + transferMs;
    }
    // What is kept of one: noes in a row, sign-ins in a row that came to nothing, when it may be
    // offered again (by this pacing's clock), and the sign-in that is out (sent, nothing heard of it
    // yet), by the number `sent` gave it; 0: none is.
    struct Kept {
        int noes = 0;
        int silent = 0;
        qint64 untilMs = 0;
        quint64 out = 0;
        bool operator==(const Kept&) const = default;
    };
    // Of a sign-in that goes out: what was kept before, and what was noted as it went (putBack).
    struct Sent {
        std::optional<Kept> before;
        Kept noted;
    };
    // What is offered where (what): its digest, by which it is known again, and beside it where it goes and who signs
    // in (the road, the address as written, the user), which is what may be kept of it on disk beside the digest
    // (PacingFile) and what a KVM taken off the wall is known by (forgetPlace). Never the password.
    struct Mark {
        QByteArray digest;
        QString road;
        QString address;
        QString user;
        Mark() = default;
        // (A test's own: a digest, and nothing of where it goes.)
        Mark(QByteArray bytes) : digest(std::move(bytes)) {} // NOLINT(google-explicit-constructor)
        Mark(const char* bytes) : digest(bytes) {}           // NOLINT(google-explicit-constructor)
        bool operator==(const Mark& other) const { return digest == other.digest; }
        friend size_t qHash(const Mark& mark, size_t seed = 0) noexcept { return qHash(mark.digest, seed); }
    };
    // What is kept of one, as it is handed out to be written down and taken back at the next start (PacingFile): its
    // mark, its noes and the sign-ins that came to nothing in a row, and for how long it may not be offered yet
    // (milliseconds from now; 0: it may be).
    struct Saved {
        Mark mark;
        int noes = 0;
        int silent = 0;
        qint64 leftMs = 0;
    };

    // As many as the program keeps of what protects nothing any more: for each KVM of a wall of the most
    // (Remembered::mostKvms) by every road a sign-in goes (a device's own; a controller's sessions and its web
    // page's), and room beside that for passwords changed meanwhile.
    static constexpr qsizetype mostKept = 8192;
    // `most`: as many as are kept, room being made of the oldest of what protects nothing any more (above).
    // `clockMs`: milliseconds on a steady clock (a test's own); none: this program's.
    explicit SignInPacing(qsizetype most = mostKept, std::function<qint64()> clockMs = {});
    // (Never copied: what it holds knows its own places in it.)
    SignInPacing(const SignInPacing&) = delete;
    SignInPacing& operator=(const SignInPacing&) = delete;

    // The program's own, for every driver that is given no other.
    static SignInPacing& program();

    // What is offered where, as a mark to know it by again: a digest of the road (the sign-in a
    // device is used through: "login"; the sessions a server's controller gives: "session"; its own
    // web page's: "web"), the address as written, the user and the password. What it is made of is
    // made in one place that is written over when this is left, and Windows makes the digest on it:
    // nothing of the password in a text made on the way, nor in what a hash keeps of what it was
    // handed (from the outside review, F5).
    static Mark what(QStringView road, const QUrl& address, QStringView user, QStringView password);

    // How many times in a row it was told no.
    int times(const Mark& what) const;
    // Milliseconds until it may be offered again; 0 where it may be now.
    qint64 waitLeftMs(const Mark& what) const;
    // One more no in a row: how long it is not offered again.
    qint64 saidNo(const Mark& what, const Steps& steps);
    // It goes out now: until its answer is noted, it stands as one that nothing answered, and holds
    // every other at least as long as it can be out (Steps::outMs).
    Sent sent(const Mark& what, const Steps& steps);
    // It did not go out after all: what was kept before `sent` is what is kept, where it is the one
    // that is out still (nothing but its password typed anew since: then nothing is kept).
    void putBack(const Mark& what, const Sent& mark);
    // It came to neither yes nor no: how long it is not sent again, from now.
    qint64 unanswered(const Mark& what, const Steps& steps);
    // ...the sign-in `answering` (what `sent` handed back): the hold noted as it went is replaced by
    // this, where it is the one that is out still.
    qint64 unanswered(const Mark& what, const Steps& steps, const Sent& answering);
    // Once more it came to nothing where nothing was sent just now: counted as one sent and never
    // answered (a controller that refuses the sessions it gives: RedfishDevice).
    qint64 cameToNothingAgain(const Mark& what, const Steps& steps);
    // Whether what stands is a sign-in that is out and nothing else: no no before it, and nothing
    // heard of it yet.
    bool onlyOut(const Mark& what) const;
    // `then`, once, from `context`'s loop: when what stands for it is a sign-in that is out no longer
    // (what it came to was noted, it was put back, or all was cleared), or after `atMostMs`, whichever
    // comes first. Never from within the call that settled it, and never where `context` is gone.
    void whenSettled(const Mark& what, QObject* context, qint64 atMostMs, std::function<void()> then);
    // How many sign-ins in a row came to nothing, counting the one that is out.
    int silent(const Mark& what) const;
    // All that is kept of it now; nothing where nothing is.
    std::optional<Kept> standing(const Mark& what) const;
    // How many are kept now.
    qsizetype kept() const { return m_known.size(); }
    // The device says when to come back: then, and not sooner; and never sooner than a hold that stands
    // (another driver's no). No word on the password.
    qint64 later(const Mark& what, qint64 waitMs);
    // ...said in answer to the sign-in `answering` (what `sent` handed back): what was noted as it went
    // is no wait to be kept; what was kept before it, and what was noted since, are.
    qint64 later(const Mark& what, qint64 waitMs, const Sent& answering);
    void accepted(const Mark& what);
    // A person has typed this password for this KVM just now: tried as for the first time, but after
    // a sign-in with it that is out.
    void forget(const QUrl& address, const QString& user, const QString& password);
    void clear();

    // All that is kept, the oldest first, to be written down (PacingFile). A sign-in that is out is kept as what it
    // holds: one that came to nothing, for as long as it holds.
    QList<Saved> saved() const;
    // What was written down in the run before, taken back at the start: each as it stood, with no sign-in out, and
    // never shortening what stands already. One without a digest is passed over.
    void restore(const QList<Saved>& kept);
    // Everything kept of sign-ins to the KVM at this address (taken off the wall), whoever signed in and with
    // whatever password; but a sign-in that is out still, which its line's end settles.
    void forgetPlace(const QUrl& address);
    // `changed` is told after every change of what is kept, on this thread (PacingFile writes it down a while later).
    // None: nobody is.
    void whenChanged(std::function<void()> changed);

private:
    struct Waiter;

    qint64 now() const;
    // `out`: the sign-in that is out from now on; none: the one that was (a write that is no verdict on it).
    qint64 keep(const Mark& what, int noes, int silent, qint64 waitMs, std::optional<quint64> out = std::nullopt);
    static qint64 quiet(int silent, const Steps& steps);
    void drop(const Mark& what);
    // Where more than the most are kept: room made of what protects nothing any more, the oldest first, until no
    // more are kept than the most, or nothing else is left that protects nothing.
    void makeRoom();
    // Whether the sign-in `answering` is the one that is out still.
    bool stillOut(const Mark& what, const Sent& answering) const;
    // Whoever waits for what stands for it is told, where that is a sign-in that is out no longer.
    void settled(const Mark& what);
    // Whoever is told of a change is told.
    void touched();

    qsizetype m_most;
    std::function<qint64()> m_clockMs;
    quint64 m_sent = 0;      // the sign-ins that went out, counted: each one's number
    std::list<Mark> m_order; // the oldest first
    struct Held {
        Kept kept;
        std::list<Mark>::iterator place;
    };
    QHash<Mark, Held> m_known;
    QHash<Mark, QList<std::shared_ptr<Waiter>>> m_waiting;
    std::function<void()> m_changed;
};

} // namespace qkvm::core
