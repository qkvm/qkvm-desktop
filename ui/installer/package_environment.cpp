#include "package_environment.h"
#include <QString>
#include <array>
#include <string_view>
#include <vector>
#include <windows.h>

namespace qkvm::installer {
void preparePackageCheckEnvironment()
{
    // Activation uses the broker's environment, not Setup's sanitized child block.
    // Remove toolkit/developer overrides before constructing QGuiApplication.
    LPWCH inherited = GetEnvironmentStringsW();
    if (inherited == nullptr)
        return;
    std::vector<std::wstring> remove;
    for (const wchar_t* at = inherited; *at != L'\0'; at += wcslen(at) + 1) {
        const std::wstring_view entry(at);
        if (_wcsnicmp(at, L"QT_", 3) == 0 || _wcsnicmp(at, L"QML", 3) == 0 || _wcsnicmp(at, L"QSG_", 4) == 0 ||
            _wcsnicmp(at, L"QKVM_", 5) == 0 || _wcsnicmp(at, L"QV4_", 4) == 0)
            remove.emplace_back(entry.substr(0, entry.find(L'=')));
    }
    FreeEnvironmentStringsW(inherited);
    for (const auto& name : remove)
        qunsetenv(QString::fromStdWString(name).toLocal8Bit().constData());
    // Diagnostics never write the QML disk cache into a user profile.
    qputenv("QML_DISABLE_DISK_CACHE", "1");
    std::array<wchar_t, 32768> windows{};
    const UINT count = GetWindowsDirectoryW(windows.data(), static_cast<UINT>(windows.size()));
    if (count > 0 && count < windows.size()) {
        const std::wstring path = std::wstring(windows.data()) + L"\\System32;" + windows.data();
        qputenv("PATH", QString::fromStdWString(path).toLocal8Bit());
    }
}
} // namespace qkvm::installer
