#include "single_instance.h"

#include <string>

#include <QByteArray>
#include <QCoreApplication>

// Windows' own header.
#include <windows.h>

using namespace Qt::StringLiterals;

namespace qkvm::ui {

namespace {

// All that one copy says to another, and how long the other is waited for at each step. (A pipe
// hands nothing over until the other end reads: a first copy that is busy for longer than this is
// not told, and the later copy ends all the same.)
constexpr char showYourself[] = "show\n";
constexpr int waitMs = 2000;
// More than this is nothing a copy of the program says.
constexpr qint64 longestSaying = 64;

} // namespace

SingleInstance::SingleInstance(const QString& name, QObject* parent) : QObject(parent)
{
    // "Local": of the session of whoever is signed in. Two people signed in to the same PC each
    // have their own copy.
    const std::wstring mutex = (u"Local\\"_s + name).toStdWString();
    m_mutex = CreateMutexW(nullptr, FALSE, mutex.c_str());
    const DWORD said = GetLastError();
    // Where Windows makes none, and lets this copy at none (another account's in this session lets
    // nobody else at it), nothing is known of the others: never the first, which would let two copies
    // write one profile. Neither listened nor told; the program ends, saying why.
    if (m_mutex == nullptr) {
        m_cannotTell = true;
        m_error = said;
        return;
    }
    m_first = said != ERROR_ALREADY_EXISTS;

    // A pipe's name is the whole PC's: the session's number makes it this session's.
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    const QString pipe = u"%1-%2"_s.arg(name).arg(session);
    if (m_first) {
        listen(pipe);
    } else {
        tellTheFirst(pipe);
    }
}

QString SingleInstance::whyNot() const
{
    if (!m_cannotTell) {
        return {};
    }
    return QCoreApplication::translate("SingleInstance",
                                       "qKVM cannot start: Windows did not let it see whether qKVM is running already "
                                       "in this Windows session (error %1). qKVM may be running here for another "
                                       "Windows account (Run as different user). Quit it there, then start qKVM "
                                       "again.")
        .arg(m_error);
}

SingleInstance::~SingleInstance()
{
    m_server.close();
    if (m_mutex != nullptr) {
        CloseHandle(static_cast<HANDLE>(m_mutex));
    }
}

void SingleInstance::listen(const QString& pipe)
{
    // Only whoever is signed in as the same user may speak to it.
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&m_server, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket* caller = m_server.nextPendingConnection()) {
            // (Nothing of a caller has come before this: a pipe hands nothing over until it is
            // read, and it is read from here on.)
            connect(caller, &QLocalSocket::readyRead, this, [this, caller] { hear(caller); });
            connect(caller, &QLocalSocket::disconnected, caller, &QObject::deleteLater);
        }
    });
    // A copy that cannot listen is still the first: it is only not told of later ones.
    m_server.listen(pipe);
}

void SingleInstance::hear(QLocalSocket* caller)
{
    // One line, and the one line there is. What is too much to be one is hung up on.
    if (caller->bytesAvailable() > longestSaying) {
        caller->abort();
        return;
    }
    // (A later copy says its line once, and goes by itself.)
    while (caller->canReadLine()) {
        if (caller->readLine(longestSaying) == QByteArray(static_cast<const char*>(showYourself))) {
            emit askedToShow();
        }
    }
}

void SingleInstance::tellTheFirst(const QString& pipe)
{
    // Whoever started this copy is looking at this PC now: the first copy may come to the front
    // on the strength of that, which Windows otherwise lets only the program in front decide.
    AllowSetForegroundWindow(ASFW_ANY);
    QLocalSocket first;
    first.connectToServer(pipe);
    if (first.waitForConnected(waitMs)) {
        first.write(static_cast<const char*>(showYourself));
        first.waitForBytesWritten(waitMs);
        first.disconnectFromServer();
        if (first.state() != QLocalSocket::UnconnectedState) {
            first.waitForDisconnected(waitMs);
        }
    }
}

} // namespace qkvm::ui
