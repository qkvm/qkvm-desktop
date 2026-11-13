#include <string>

#include <windows.h>

#include <shellapi.h>
#include <shlobj.h>

// Says where a program started with the same environment would keep its data, for the test that starts the program
// itself with a scratch home (it must never be the person's own): into the file its one argument names, one line each
// (UTF-16, as Windows says them), its local application data (Qt's AppLocalDataLocation and the program's cache are
// below it), its roaming application data, and its Temp. A Windows program: it shows nothing.
namespace {

std::wstring known(REFKNOWNFOLDERID which)
{
    PWSTR value = nullptr;
    if (FAILED(SHGetKnownFolderPath(which, KF_FLAG_NO_PACKAGE_REDIRECTION, nullptr, &value))) {
        return {};
    }
    std::wstring said(value);
    CoTaskMemFree(value);
    return said;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (arguments == nullptr || count != 2) {
        return 90;
    }
    const std::wstring into = arguments[1];
    LocalFree(static_cast<void*>(arguments));
    wchar_t temp[MAX_PATH + 1] = {};
    const DWORD length = GetTempPathW(MAX_PATH + 1, temp);
    const std::wstring said = known(FOLDERID_LocalAppData) + L"\n" + known(FOLDERID_RoamingAppData) + L"\n" +
                              std::wstring(temp, length) + L"\n";
    const HANDLE file =
        CreateFileW(into.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return 91;
    }
    DWORD written = 0;
    const DWORD bytes = static_cast<DWORD>(said.size() * sizeof(wchar_t));
    const bool wrote = WriteFile(file, said.data(), bytes, &written, nullptr) != FALSE && written == bytes;
    CloseHandle(file);
    return wrote ? 0 : 92;
}
