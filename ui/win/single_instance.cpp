#include "single_instance.h"

#include <string>
#include <vector>

#include <QByteArray>
#include <QCoreApplication>

// Windows' own headers; the first has to come first.
#include <windows.h>

#include <sddl.h>

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

// This process's Windows user, as Windows names it (a SID: "S-1-5-21-..."); empty where it did not say.
std::wstring thisUsersSid()
{
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
        return {};
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> user(size);
    std::wstring sid;
    LPWSTR named = nullptr;
    if (size >= sizeof(TOKEN_USER) && GetTokenInformation(token, TokenUser, user.data(), size, &size) != FALSE &&
        ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &named) != FALSE) {
        sid = named;
        LocalFree(named);
    }
    CloseHandle(token);
    return sid;
}

// Whether a process runs as the Windows user whose SID this is (a process whose token cannot be read does not).
bool runsAs(DWORD process, const std::wstring& sid)
{
    const HANDLE opened = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process);
    if (opened == nullptr) {
        return false;
    }
    HANDLE token = nullptr;
    bool same = false;
    if (OpenProcessToken(opened, TOKEN_QUERY, &token) != FALSE) {
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        std::vector<BYTE> user(size);
        LPWSTR named = nullptr;
        if (size >= sizeof(TOKEN_USER) && GetTokenInformation(token, TokenUser, user.data(), size, &size) != FALSE &&
            ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &named) != FALSE) {
            same = !sid.empty() && sid == named;
            LocalFree(named);
        }
        CloseHandle(token);
    }
    CloseHandle(opened);
    return same;
}

} // namespace

unsigned long SingleInstance::serverOfThisUser(void* pipe)
{
    ULONG server = 0;
    if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE ||
        GetNamedPipeServerProcessId(static_cast<HANDLE>(pipe), &server) == FALSE || server == 0) {
        return 0;
    }
    return runsAs(server, thisUsersSid()) ? server : 0;
}

QString SingleInstance::ownName(const QString& name)
{
    return name + u'-' + QString::fromStdWString(thisUsersSid());
}

SingleInstance::SingleInstance(const QString& name, QObject* parent) : QObject(parent)
{
    // Both mutexes with one list: this Windows user may do anything with them, and nobody else anything (from 0.2.0;
    // before, Windows' own list of the account's). Only waited on, never owned: SYNCHRONIZE is all a copy asks of one
    // that is there, which a list of the account's own admits even where the copy that made it runs above it.
    const std::wstring user = thisUsersSid();
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const std::wstring rules = L"D:P(A;;GA;;;" + user + L")";
    if (user.empty() || ConvertStringSecurityDescriptorToSecurityDescriptorW(rules.c_str(), SDDL_REVISION_1,
                                                                             &descriptor, nullptr) == FALSE) {
        m_cannotTell = true;
        m_error = GetLastError();
        return;
    }
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    // This account's own, of the session of whoever is signed in ("Local"): which copy of this account is the first
    // here. Two accounts in one session (Run as different user) each have their own copy, and each its own profile
    // (outside review of 0.1.2, C3); which copy writes a profile, whatever session it runs in, is ProfileLock's.
    const std::wstring own = (u"Local\\"_s + ownName(name)).toStdWString();
    m_mutex = CreateMutexExW(&attributes, own.c_str(), 0, SYNCHRONIZE);
    const DWORD said = GetLastError();
    // The name Setup (AppMutex, and its uninstaller) and qKVM 0.1.0 to 0.1.2 look for, made too while this copy runs.
    const std::wstring shared = (u"Local\\"_s + name).toStdWString();
    m_shared = m_mutex != nullptr ? CreateMutexExW(&attributes, shared.c_str(), 0, SYNCHRONIZE) : nullptr;
    const DWORD sharedSaid = GetLastError();
    LocalFree(descriptor);
    // Where Windows makes none of this account's own and lets this copy at none (one made under its name by somebody
    // who lets nobody else at it), nothing is known of the others: never the first, which would let two copies write
    // one profile. Neither listened nor told; the program ends, saying why.
    if (m_mutex == nullptr) {
        m_cannotTell = true;
        m_error = said;
        return;
    }
    m_first = said != ERROR_ALREADY_EXISTS;
    // The shared name there already while this account's own was not: a copy of this account of a version before 0.2.0
    // runs in this session (it makes no name of the account's, and is told at its own pipe). Not the first, then. One
    // that Windows lets this copy at none of is another account's copy, whose list lets nobody else at it: this
    // account's copy is the first of its own all the same (Setup and the uninstaller of this account then see it by
    // ProfileLock's file: SECURITY.md).
    const bool older = m_first && m_shared != nullptr && sharedSaid == ERROR_ALREADY_EXISTS;
    if (older) {
        m_first = false;
    }

    // A pipe's name is the whole PC's: this account's own name and the session's number make it this account's in
    // this session (a version before 0.2.0 listens at the name and the session's number alone).
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    const QString pipe = u"%1-%2"_s.arg(older ? name : ownName(name)).arg(session);
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
                                       "for this Windows account in this session (error %1). Something else holds "
                                       "the name qKVM uses for that. Sign out of Windows and in again, then start "
                                       "qKVM again.")
        .arg(m_error);
}

SingleInstance::~SingleInstance()
{
    m_server.close();
    for (void* const mutex : {m_shared, m_mutex}) {
        if (mutex != nullptr) {
            CloseHandle(static_cast<HANDLE>(mutex));
        }
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
    QLocalSocket first;
    first.connectToServer(pipe);
    if (first.waitForConnected(waitMs)) {
        // Said only to a copy that Windows says runs as this user: the pipe's name is the whole PC's, and another
        // account could have made one of it first. Whoever started this copy is looking at this PC now: that copy, and
        // no other program, may come to the front on the strength of that, which Windows otherwise lets only the
        // program in front decide.
        const unsigned long server = serverOfThisUser(reinterpret_cast<void*>(first.socketDescriptor()));
        if (server == 0) {
            first.abort();
            return;
        }
        AllowSetForegroundWindow(server);
        first.write(static_cast<const char*>(showYourself));
        first.waitForBytesWritten(waitMs);
        first.disconnectFromServer();
        if (first.state() != QLocalSocket::UnconnectedState) {
            first.waitForDisconnected(waitMs);
        }
    }
}

} // namespace qkvm::ui
