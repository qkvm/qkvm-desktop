#pragma once

#include <QLocalServer>
#include <QLocalSocket>
#include <QObject>
#include <QString>

namespace qkvm::ui {

// One running copy of the program for whoever is signed in to Windows. Started a second time, the
// program does not run twice: the second copy tells the first to bring its window to the front,
// and ends. A program that can go on out of sight behind its icon would otherwise be started again
// by whoever does not see it, and two copies would write the same settings.
//
// Which copy is the first is settled by something of Windows' that only one can make: a mutex of
// this name and the Windows account's SID (ownName), in the session of whoever is signed in, which
// only that account may use (from 0.2.0: each account in a session its own copy, outside review of
// 0.1.2, C3). The mutex of the name alone is made beside it, for Setup and older versions, which
// look for it. The telling goes over a pipe of the account's name that only the same user may
// open, and all that anybody can say over it is "show yourself"; it is said only to a first copy
// that Windows says runs as this user, which alone is let come to the front. Which copy writes the
// profile, in whatever session of the account it runs, is ProfileLock's (win/profile_lock.h).
//
// On the interface thread.
class SingleInstance : public QObject {
    Q_OBJECT

public:
    // `name`: what the copies know each other by. A test names its own.
    explicit SingleInstance(const QString& name, QObject* parent = nullptr);
    ~SingleInstance() override;

    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;
    SingleInstance(SingleInstance&&) = delete;
    SingleInstance& operator=(SingleInstance&&) = delete;

    // True: this is the only copy of this account in this session, and it listens for later ones.
    // False: another runs already (of this account, or of a version before 0.2.0 of this account,
    // which holds the name alone), and has been told to show itself, or this cannot tell (below); this
    // one has nothing left to do.
    bool isFirst() const { return m_first; }
    // Windows did not make this account's mutex, nor let this copy at the one there is (one made under
    // its name by somebody who lets nobody else at it). This copy cannot tell whether another runs, so
    // it is never the first, which would let two copies write one profile; it neither listens nor
    // tells, and the program ends, saying why (whyNot: in plain words, with Windows' number; outside
    // review of 0.1.2, C3). The name alone is still made: Setup and qKVM 0.1.0 to 0.1.2 look for it.
    bool cannotTell() const { return m_cannotTell; }
    QString whyNot() const;

    // The name of this Windows account's own copies: `name`, a dash and the account's SID (empty where Windows did
    // not say it).
    static QString ownName(const QString& name);
    // The number of the process at the other end of a pipe this copy connected to (a HANDLE), where Windows says it
    // runs as this copy's user; 0 otherwise, and where that cannot be told. What a later copy tells, and lets come to
    // the front, is only that.
    static unsigned long serverOfThisUser(void* pipe);

signals:
    // A later copy was started: whoever started it wants to see the program.
    void askedToShow();

private:
    void listen(const QString& pipe);
    // Takes what a caller has said so far, if that is a whole line or too much to be one.
    void hear(QLocalSocket* caller);
    static void tellTheFirst(const QString& pipe);

    // Windows' own handle for this account's mutex; nothing where it could not be made.
    void* m_mutex = nullptr;
    // And for the name every version makes, which Setup looks for; nothing where another account's holds it.
    void* m_shared = nullptr;
    bool m_first = false;
    bool m_cannotTell = false;
    // What Windows said where it made no mutex.
    unsigned long m_error = 0;
    QLocalServer m_server;
};

} // namespace qkvm::ui
