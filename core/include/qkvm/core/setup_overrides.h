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
//   and the override off after it, or by the server taken off the wall (forget).
// * A server is found by the mark of its computer where both that kept here and the one asked about are a number, else
//   by its controller's address (the mark is the settings' OwnKvm::machine, redfish::marksOf).
// * Written whole (QSaveFile) at each change, on the interface thread, as the settings are: one small file, written at
// a
//   press and at its end, never at a look that changes nothing. One that cannot be read is taken as empty and written
//   anew at the next change. At most mostKept servers (the oldest go).
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
    explicit SetupOverrides(QString file = {}, std::function<QDateTime()> wallClock = {},
                            std::function<qint64()> steadyClock = {});

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
    // what stood for it before, if anything did.
    struct Noted {
        bool written = false;
        std::optional<Earlier> before;
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
    // it may be made after any look, at any time, and this program does not follow the task to its end (its monitor:
    // 0.2.0). Marked so, and written down at once, at the answer (a program ended by force before the press ends leaves
    // it marked). From then on neither a look nor any time lets it go (seenOff, drop): only a later start into BIOS
    // setup on that server that sees its change made and the override off after it, or the server taken off the wall
    // (forget). It holds no update: what holds one is the press's own doubt (changeMayStillCome). outside review's
    // confirm round of 0.1.2, D1.
    void markQueued(const QUrl& address, const QString& machine);
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
    void read();
    void write();
    qsizetype indexOf(const QUrl& address, const QString& machine) const;

    QString m_file;
    std::function<QDateTime()> m_wallClock;
    std::function<qint64()> m_steadyClock;
    QList<Kept> m_left;
    std::optional<bool> m_written;
    // How many presses are under way (pressUnderWay): shared with what each was given, which may outlive this, and
    // atomic, as what was given may be given back on another thread than this one's (review 012-final's INFO 3).
    std::shared_ptr<std::atomic<qsizetype>> m_underWay = std::make_shared<std::atomic<qsizetype>>(0);
};

} // namespace qkvm::core
