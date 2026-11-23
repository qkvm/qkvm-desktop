#include "profile_lock.h"

#include <string>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

// Windows' own header.
#include <windows.h>

namespace qkvm::ui {

ProfileLock::ProfileLock(const QString& file) : m_folder(QDir::toNativeSeparators(QFileInfo(file).absolutePath()))
{
    // (A first start: the folder is made here, as the settings' first writing would make it.)
    if (!QDir().mkpath(QFileInfo(file).absolutePath())) {
        m_error = ERROR_PATH_NOT_FOUND;
        return;
    }
    const std::wstring path = QDir::toNativeSeparators(file).toStdWString();
    // Opened for the lock alone, by any copy (that is how a later one asks for it); nobody may delete it meanwhile.
    const HANDLE opened = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (opened == INVALID_HANDLE_VALUE) {
        m_error = GetLastError();
        return;
    }
    // One byte, the first, exclusively, at once or not at all: a lock Windows takes back from a program that ends.
    OVERLAPPED at{};
    if (LockFileEx(opened, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &at) == FALSE) {
        m_error = GetLastError();
        m_held = m_error == ERROR_LOCK_VIOLATION;
        CloseHandle(opened);
        return;
    }
    m_file = opened;
}

ProfileLock::~ProfileLock()
{
    if (m_file != nullptr) {
        // (Closing the handle lets the lock go.)
        CloseHandle(static_cast<HANDLE>(m_file));
    }
}

QString ProfileLock::whyNot() const
{
    if (taken()) {
        return {};
    }
    if (m_held) {
        return QCoreApplication::translate("ProfileLock",
                                           "qKVM is already running for this Windows account, in another Windows "
                                           "session (this account is signed in twice, at this PC and by Remote "
                                           "Desktop, for one). Quit it there, or go back to that session.");
    }
    return QCoreApplication::translate("ProfileLock",
                                       "qKVM cannot start: Windows did not let it see whether another copy of qKVM "
                                       "writes its settings (error %1). It could not see whether one runs for this "
                                       "Windows account in another session. Check the folder %2, then start qKVM "
                                       "again.")
        .arg(m_error)
        .arg(m_folder);
}

} // namespace qkvm::ui
