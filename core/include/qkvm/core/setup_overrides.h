#pragma once

#include <functional>
#include <optional>

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
// * A server is found by the mark of its computer where both that kept here and the one asked about are a number, else
//   by its controller's address (the mark is the settings' OwnKvm::machine, redfish::marksOf).
// * Written whole (QSaveFile) at each change, on the interface thread, as the settings are: one small file, written at
// a
//   press and at its end, never at a look that changes nothing. One that cannot be read is taken as empty and written
//   anew at the next change. At most mostKept servers (the oldest go).
//
// On the interface thread.
class SetupOverrides {
public:
    static constexpr qsizetype mostKept = 256;
    static constexpr qint64 mostFileBytes = qint64{256} * 1024;

    struct Left {
        QString address; // the controller's, as a URL says it whole
        QString machine; // the mark of its computer ("server:" and a digest); empty: none was kept
        QDateTime since; // when it was noted, in UTC
        QDateTime doubtUntil;
        bool operator==(const Left&) const = default;
    };

    // `file`: where it is kept (read now); empty: in memory only. `wallClock`: none: now, in UTC.
    explicit SetupOverrides(QString file = {}, std::function<QDateTime()> wallClock = {});

    // The program's own, for every driver that is given no other: kept in memory until main.cpp gives it its file.
    static SetupOverrides& program();
    // Kept in `file` from now on, and what it keeps read now (what was noted before stays, and is written there).
    void keepIn(const QString& file);
    QString file() const { return m_file; }

    // The change of a server's override can go out now: noted, until `doubtUntil` at least (a later one that stood
    // stays). What stood for it before, if anything did: until when its doubt went.
    std::optional<QDateTime> mayBeSet(const QUrl& address, const QString& machine, const QDateTime& doubtUntil);
    // Its doubt goes on until then (and no longer), where it is noted.
    void settle(const QUrl& address, const QString& machine, const QDateTime& doubtUntil);
    // It is known to be off: let go of.
    void drop(const QUrl& address, const QString& machine);
    // A look, or a clearing read back, showed it off at `at`: let go of where its doubt is over by then. Whether it
    // was.
    bool seenOff(const QUrl& address, const QString& machine, const QDateTime& at);
    // What stands for that server, if anything.
    std::optional<Left> standing(const QUrl& address, const QString& machine) const;
    QList<Left> all() const { return m_left; }
    // Whether the last writing of the file worked (none: nothing written yet).
    std::optional<bool> written() const { return m_written; }

    // What is told at a start, one notice for each card of `kvms` whose server stands here (a server that is on no card
    // any more is told of by none).
    QStringList toldAtStart(const QList<OwnKvm>& kvms) const;
    // What the card of a server that stands here says.
    static QString onTheCard();

    // Whether `left` is of the server at `address` whose computer's mark is `machine`.
    static bool isOf(const Left& left, const QUrl& address, const QString& machine);

private:
    QDateTime wallNow() const;
    void read();
    void write();
    qsizetype indexOf(const QUrl& address, const QString& machine) const;

    QString m_file;
    std::function<QDateTime()> m_wallClock;
    QList<Left> m_left;
    std::optional<bool> m_written;
};

} // namespace qkvm::core
