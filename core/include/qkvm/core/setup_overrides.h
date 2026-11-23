#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <qkvm/core/settings.h>

namespace qkvm::core {

// The servers of this PC's own wall whose BIOS setup boot override may be set, as far as this program knows (review 4's
// MEDIUM 1): a start into BIOS setup that did not end (the program ended, Windows restarted for its updates, or the
// server's driver was let go of while the override was being set or the start watched), and the clearing sent then not
// seen to work. A server left booting into its setup stops there at its next ordinary boot, and stays down. So it is
// kept beside the settings, and told at the next start, on the server's card and as a notice, until a look shows the
// override off. Nothing secret is in it: the controller's address and the server's mark, as the settings keep them, and
// two times.
//
// * Noted (mayBeSet) the moment the change of the override can go out, and written to the file before it goes: a
//   program ended by force leaves it noted. With it, until when a look proves nothing (`doubtUntil`): the press still
//   going, or a change of its own still on its way to the controller, which may make it after any look.
// * Let go of by what is seen: a look, or a clearing read back, that shows the override off in so many words
//   ("Disabled", or another tool's override in its place) at or after `doubtUntil` (seenOff); a press that ends knowing
//   it cleared it, with nothing of it still on its way, at once (drop); one that ends in doubt leaves it standing, and
//   says until when a change of its may still come (settle).
// * But one marked queued (the controller only queued a press's change: markQueued, outside review's confirm round of
// 0.1.2, D1) is let
//   go of by no look and by no time: only by a later start into BIOS setup on that server that sees its change made
//   and the override off after it, or by the server taken off the wall (forget). Its mark goes where reads of the
//   press that marked it saw that change through (seenMade), never by a task's word.
// * A server is found by the mark of its computer where both that kept here and the one asked about are a number, else
//   by its controller's address (the mark is the settings' OwnKvm::machine, redfish::marksOf).
// * Written whole (QSaveFile) at each change, on the interface thread, as the settings are: one small file, written at
//   a press and at its end, never at a look that changes nothing. At most mostKept servers, and none is let go of to
//   make room (each stands for a server whose override may be set, and its card says so): where mostKept are kept, a
//   new one is not noted, and its press does not go (Noted::full: the merged review of 0.2.0, A-L1, and round 2's
//   L4).
// * Read as the settings are (outside review of the fuzz work, round 3, M1): one that is there and cannot be opened is
//   tried again (Settings::Patience: forty times, 50 ms apart, at the start). One that still cannot be read whole (held
//   by another program for longer, larger than any this writes, no JSON, of another version, as after a way back to an
//   older build, or holding a record that is none) is never written over, nor replaced by one that drops its records:
//   before anything is written, it is put aside beside where it was, byte for byte (moved, never copied: `.unreadable`,
//   `.unreadable-N`), as the sign-in counts are; what could be read of it is kept and told as ever. Where it cannot be
//   put aside (something holds it), nothing is written while it stands, so that no start into BIOS setup goes out (its
//   note cannot be written: C6), and it is read again at each writing: once it can be read, what it holds is taken in
//   beside what was noted meanwhile. The start says which (toldOfItsFile).
//
// On the interface thread; what pressUnderWay gives may be given back on any.
class SetupOverrides {
public:
    static constexpr qsizetype mostKept = 256;
    static constexpr qint64 mostFileBytes = qint64{256} * 1024;
    // The longest doubt a press notes, from the moment it notes it: its four minutes, the clearing then (a read and the
    // change, each waited for as long as the change) and a change still on its way after that, 6.4 minutes at the
    // driver's own pace (RedfishDevice::noteMayBeSet), with room. A record of a run before whose doubt reaches further
    // than this from the wall clock's now was noted by a clock that ran ahead and was put right since (review
    // 012-hold's LOW 2): no change of it can come that late, and it holds nothing (changeMayStillCome) until the clock
    // is within this of the end of its doubt. One noted in this run lasts no longer than this on the steady clock.
    static constexpr qint64 longestDoubtMs = qint64{10} * 60 * 1000;

    struct Left {
        QString address; // the controller's, as a URL says it whole
        QString machine; // the mark of its computer ("server:" and a digest); empty: none was kept
        QDateTime since; // when it was noted, in UTC
        QDateTime doubtUntil;
        // The controller only queued a change of a press's (markQueued): no look lets it go.
        bool queued = false;
        bool operator==(const Left&) const = default;
    };

    // `file`: where it is kept (read now); empty: in memory only. `wallClock`: none: now, in UTC. `steadyClock`, in
    // milliseconds, which nothing puts right: none: Windows' own (std::chrono::steady_clock). A doubt noted or settled
    // here (mayBeSet, settle) is measured on it, from its length when it is noted (as far as the wall clock says then,
    // and no longer than longestDoubtMs): a wall clock put back or forward neither ends it nor makes it look like one a
    // clock that ran ahead noted, and a look lets it go only once it is over (outside review of 0.1.2, C5). A record
    // read from the file, of a run before, whose doubt still runs (by the wall clock then, within longestDoubtMs) runs
    // on from there on the steady clock, for what is left of it (outside review's confirm round of 0.1.2, R3); one that
    // does not is judged by the wall clock, within longestDoubtMs.
    // `patience`: how a file that is there and cannot be opened is tried at its reading (a test waits less).
    explicit SetupOverrides(QString file = {}, std::function<QDateTime()> wallClock = {},
                            std::function<qint64()> steadyClock = {}, Settings::Patience patience = {});

    // The program's own, for every driver that is given no other: kept in memory until main.cpp gives it its file.
    static SetupOverrides& program();
    // Kept in `file` from now on, and what it keeps read now (what was noted before stays, and is written there).
    void keepIn(const QString& file);
    QString file() const { return m_file; }

    // What stood for a server before a note: until when its doubt went, and, where it runs on the steady clock (noted
    // or loaded in this run), its end there.
    struct Earlier {
        QDateTime doubtUntil;
        std::optional<qint64> steadyUntil;
        bool operator==(const Earlier&) const = default;
    };
    // What noting a server came to: whether it is written down (in its file; kept in memory where there is none), and
    // what stood for it before, if anything did. `full`: it was not noted at all, mostKept being kept already, none of
    // which is let go of to make room: nothing was written, nothing let go of.
    struct Noted {
        bool written = false;
        std::optional<Earlier> before;
        bool full = false;
    };
    // The change of a server's override can go out now: noted, until `doubtUntil` at least (a later one that stood
    // stays). The change goes out only where it is written (outside review of 0.1.2, C6: a program that then ended
    // would have no note of it).
    [[nodiscard]] Noted mayBeSet(const QUrl& address, const QString& machine, const QDateTime& doubtUntil);
    // Its doubt goes on until then (and no longer), where it is noted.
    void settle(const QUrl& address, const QString& machine, const QDateTime& doubtUntil);
    // What stood for it before a note is put back as it stood (a press that set nothing), its doubt on the clock it
    // ran on: a wall clock moved in between does not end it (outside review's confirm round of 0.1.2, R3).
    void restore(const QUrl& address, const QString& machine, const Earlier& earlier);
    // The controller only queued the change a press sent (a 202, the monitor of a task named, or a Task as its answer):
    // it may be made after any look, at any time. Marked so, and written down at once, as the change goes and at the
    // answer (a program ended before the answer, or before the press ends, leaves it marked: outside review 2, 3.5;
    // the answer takes the first mark off again where it says the change was not queued, seenMade). From then on
    // neither a look nor any time lets it go (seenOff,
    // drop), nor the prune: only reads of that press that saw its change through (seenMade), a later start into BIOS
    // setup on that server that sees its change made and the override off after it, or the server taken off the wall
    // (forget). It holds no update: what holds one is the press's own doubt (changeMayStillCome). outside review's
    // confirm round of 0.1.2, D1. Whether this marked it (it was not marked before).
    bool markQueued(const QUrl& address, const QString& machine);
    // The change that marked it (markQueued) was seen through by reads of the server's own computer (from 0.2.0 on,
    // D1): shown made at a read after its task's end, or shown made and then taken ("Disabled") at a read after the
    // restart (RedfishDevice's start into BIOS setup). Nothing of that change can come any more, and the mark goes; the
    // record stands as any press's does, let go of by what is seen once its doubt is over. Never by the task's own
    // word, done or failed (outside review of D1, round 2, H1). (Only for the press that marked it: one marked before
    // stays marked.) Also where a press marked it as its change went, and the controller's answer said that change was
    // not queued (made, refused, or never sent: outside review 2, 3.5).
    void seenMade(const QUrl& address, const QString& machine);
    // It is known to be off: let go of, but not one marked queued (markQueued). Whether it was.
    bool drop(const QUrl& address, const QString& machine);
    // A look, or a clearing read back, showed it off at `at`: let go of where its doubt is over by then (one noted or
    // settled in this run: by the steady clock now, as the look is told), and it is not marked queued. Whether it was.
    bool seenOff(const QUrl& address, const QString& machine, const QDateTime& at);
    // Let go of whatever it is, a record marked queued too: a start into BIOS setup that saw its change made and the
    // override off after it, or the server taken off the wall (its card gone, and the record with it).
    void forget(const QUrl& address, const QString& machine);
    // What stands for that server, if anything.
    std::optional<Left> standing(const QUrl& address, const QString& machine) const;
    QList<Left> all() const;
    // Whether a change of some server's override may still come: a press under way (pressUnderWay), or a record whose
    // doubt is not over (a change sent that may still be made): one noted or settled in this run, by the steady clock;
    // one of a run before, by the wall clock's now, reaching no further than longestDoubtMs from it. What only stands
    // from before, its doubt over (a server that did not answer, or that is on no card any more) or noted by a clock
    // that ran ahead, is no such change: it is kept, and told on its card and at the next start, but nothing of it can
    // come any more. What holds an update (WallWindows::whatHoldsAnUpdate), read anew at each press and each answer.
    bool changeMayStillCome() const;
    // The records of servers on no card of the wall's list as `settings` hold it (redfish's, by isOf) whose doubt is
    // over, and that are not marked queued: let go of, and the file written anew. Nothing of their changes can come any
    // more, and no card or start tells of them: before 0.2.0 such a record stayed in its file for good. One whose doubt
    // lasts stays, and goes at a later prune: never while a change of it may still come (as changeMayStillCome judges
    // it). Whether any went. Only where the settings read their list whole at the start (Settings::listReadWhole):
    // settings that could not be read begin as a wall of no cards, and a list that is no list, held what this build
    // does not read, or lost an entry in its reading is no proof that a server is on no card. Nothing goes then
    // (outside review of the prune, M2). A record marked queued never goes so, whatever the list says: a list read
    // whole at one start may be the one a run wrote after its settings could not be read, with none of the old cards;
    // it goes only with its card (forget), as in 0.1.2, or stays in its file for good, holding nothing (outside review
    // of the prune, round 2, M1). Nor does anything go while settings that could not be read at a start stand put aside
    // beside these (Settings::keepsAsideWhatCouldNotBeRead): the list read whole may be one a run wrote after them, and
    // the kept file put back brings their cards back, each with what its record says (outside review of the fuzz work,
    // round 3, M2). By the settings alone, and never by a list of cards somebody hands it. At each start (main.cpp) and
    // each change of the wall's list (LocalWall::remember).
    bool prune(const Settings& settings);
    // A press noted here and not settled yet (RedfishDevice's start into BIOS setup, from its note to its end or its
    // hand-over): a change may still come while what this gives is kept, by no clock (review 012-hold's LOW 1: a press
    // its window let go of runs on in its driver, and can outlast the doubt it noted). Given back where it is let go
    // of, on any thread, even after this is gone; never empty while it is kept.
    [[nodiscard]] std::shared_ptr<void> pressUnderWay();
    // How many are under way now.
    qsizetype pressesUnderWay() const { return m_underWay->load(); }
    // The wall clock from now on (none: now, in UTC). For a test of the program's own.
    void useWallClock(std::function<QDateTime()> wallClock) { m_wallClock = std::move(wallClock); }
    // The clock nothing puts right from now on (none: Windows' own). For a test of the program's own.
    void useSteadyClock(std::function<qint64()> steadyClock) { m_steadyClock = std::move(steadyClock); }
    // Whether the last writing of the file worked (none: nothing written yet).
    std::optional<bool> written() const { return m_written; }
    // The file was there when it was read (keepIn, or this made) and could not be read whole: what of it was read is
    // kept, and the file is never written over (the header's comment).
    bool wasUnreadable() const { return m_wasUnreadable; }
    // Where that file is now: empty while it is where it was, and where there was none.
    QString putAside() const { return m_putAside; }
    // Puts that file aside, where it is still in the way, beside where it was, under the first name of its kind that is
    // free: moved, never copied. One that cannot be moved (something holds it) stays as and where it is, and nothing is
    // written until it can be read or moved. Whoever starts the program does this before anything is written, and says
    // what came of it (toldOfItsFile); every writing does it first in any case. Nothing where no such file is in the
    // way.
    base::Result<void> putAsideWhatCouldNotBeRead();
    // What a start says of the file where it could not be read whole: where it is kept, or that nothing is written over
    // it while it cannot be put aside. Empty where it was read whole, or read whole since.
    QString toldOfItsFile() const;

    // What is told at a start, one notice for each card of `kvms` whose server stands here (a server that is on no card
    // any more is told of by none).
    QStringList toldAtStart(const QList<OwnKvm>& kvms) const;
    // What the card of a server that stands here says; `queued`: its record is marked queued (markQueued).
    static QString onTheCard(bool queued = false);

    // Whether `left` is of the server at `address` whose computer's mark is `machine`.
    static bool isOf(const Left& left, const QUrl& address, const QString& machine);

private:
    // One server's record, and, where its doubt was noted or settled in this run, when it ends on the steady clock.
    struct Kept {
        Left left;
        std::optional<qint64> steadyUntil;
    };

    QDateTime wallNow() const;
    qint64 steadyNow() const;
    // When a doubt until `doubtUntil` (by the wall clock now) ends on the steady clock: no later than longestDoubtMs
    // on.
    qint64 steadyEndOf(const QDateTime& doubtUntil) const;
    // Whether the doubt of `kept` is over now.
    bool doubtIsOver(const Kept& kept, const QDateTime& at) const;
    // Whether a change of `kept`'s may still come, by the wall clock and the steady clock now (changeMayStillCome).
    static bool mayStillCome(const Kept& kept, const QDateTime& now, qint64 steady);
    // What the file holds (none where there is no file), opened in so many tries; and whether it was read whole.
    struct Reading {
        QList<Kept> kept;
        bool whole = true;
    };
    Reading readFile(int tries) const;
    // The file read in so many tries, and what it holds kept (beside what is kept already, as keepIn takes it in).
    void read(int tries);
    // Records kept beside those kept now: one of the same server keeps the later doubt, and its mark where either had
    // one.
    void takeIn(const QList<Kept>& others);
    void write();
    qsizetype indexOf(const QUrl& address, const QString& machine) const;
    // The prune itself, by a list that was read whole (prune).
    bool pruneBy(const QList<OwnKvm>& kvms);
    // Whether there is room for one more record within mostKept (none is let go of to make it).
    bool roomForOneMore() const;

    QString m_file;
    std::function<QDateTime()> m_wallClock;
    std::function<qint64()> m_steadyClock;
    Settings::Patience m_patience;
    QList<Kept> m_left;
    std::optional<bool> m_written;
    bool m_wasUnreadable = false;
    // The file that could not be read whole is still where it was, and in the way of what is written.
    bool m_inTheWay = false;
    QString m_putAside;
    // How many presses are under way (pressUnderWay): shared with what each was given, which may outlive this, and
    // atomic, as what was given may be given back on another thread than this one's (review 012-final's INFO 3).
    std::shared_ptr<std::atomic<qsizetype>> m_underWay = std::make_shared<std::atomic<qsizetype>>(0);
};

} // namespace qkvm::core
