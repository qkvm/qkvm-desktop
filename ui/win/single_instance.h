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
// Which copy is the first is settled by something of Windows' that only one can make (a mutex of
// this name, in the session of whoever is signed in). The telling goes over a pipe of the same
// name that only the same user may open, and all that anybody can say over it is "show yourself".
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

    // True: this is the only copy, and it listens for later ones. False: another runs already,
    // and has been told to show itself, or this cannot tell (below); this one has nothing left to do.
    bool isFirst() const { return m_first; }
    // Windows did not make the mutex, nor let this copy at the one there is (another account's in this
    // session, Run as different user: its own lets nobody else at it). This copy cannot tell whether
    // another runs, so it is never the first, which would let two copies write one profile; it neither
    // listens nor tells, and the program ends, saying why (whyNot: in plain words, with Windows' number;
    // outside review of 0.1.2, C3). The names stay as they are: Setup and qKVM 0.1.0 and 0.1.1 look for
    // them.
    bool cannotTell() const { return m_cannotTell; }
    QString whyNot() const;

signals:
    // A later copy was started: whoever started it wants to see the program.
    void askedToShow();

private:
    void listen(const QString& pipe);
    // Takes what a caller has said so far, if that is a whole line or too much to be one.
    void hear(QLocalSocket* caller);
    static void tellTheFirst(const QString& pipe);

    // Windows' own handle for the mutex; nothing where it could not be made.
    void* m_mutex = nullptr;
    bool m_first = false;
    bool m_cannotTell = false;
    // What Windows said where it made no mutex.
    unsigned long m_error = 0;
    QLocalServer m_server;
};

} // namespace qkvm::ui
