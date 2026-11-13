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
    // and has been told to show itself; this one has nothing left to do.
    bool isFirst() const { return m_first; }

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
    QLocalServer m_server;
};

} // namespace qkvm::ui
