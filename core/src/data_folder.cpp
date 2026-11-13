#include <qkvm/core/data_folder.h>

#include <string>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#include <windows.h>

namespace qkvm::core {
namespace {

std::wstring native(const QString& path)
{
    return QDir::toNativeSeparators(path).toStdWString();
}

bool plainlyNotThere(DWORD error)
{
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

QString windowsWords(DWORD error)
{
    LPWSTR text = nullptr;
    const DWORD length =
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, error, 0, reinterpret_cast<LPWSTR>(&text), 0, nullptr);
    QString said = length > 0 ? QString::fromWCharArray(text, static_cast<qsizetype>(length)).trimmed()
                              : QStringLiteral("error %1").arg(error);
    LocalFree(text);
    return said;
}

QString cannotRead(const QString& where, DWORD error)
{
    return QCoreApplication::translate("Data", "qKVM cannot read its data in %1 (Windows says: %2). Nothing was "
                                               "changed. Make it readable for you again, then start qKVM.")
        .arg(QDir::toNativeSeparators(where), windowsWords(error));
}

} // namespace

QString whyDataCannotBeRead(const QString& settingsFile)
{
    const QString folder = QFileInfo(settingsFile).absolutePath();
    const std::wstring there = native(folder);
    if (GetFileAttributesW(there.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        return plainlyNotThere(error) ? QString() : cannotRead(folder, error);
    }
    // What is in it, listed.
    WIN32_FIND_DATAW found{};
    const HANDLE listing = FindFirstFileW((there + L"\\*").c_str(), &found);
    if (listing == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND) {
            return cannotRead(folder, error);
        }
    } else {
        FindClose(listing);
    }
    // And the settings, opened to be read.
    const HANDLE file =
        CreateFileW(native(settingsFile).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return plainlyNotThere(error) ? QString() : cannotRead(settingsFile, error);
    }
    CloseHandle(file);
    return {};
}

} // namespace qkvm::core
