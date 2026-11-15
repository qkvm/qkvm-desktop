#include "shell_start.h"

#include <filesystem>
#include <string>
#include <vector>

#include "setup_wait.h"

#include <windows.h>

// (In this order: the program's headers leave OLE out of windows.h.)
#include <ole2.h>

#include <shlobj.h>

#include <exdisp.h>
#include <servprov.h>
#include <shldisp.h>
#include <shlguid.h>

namespace qkvm::ui {
namespace {

// Lets go of a COM object as it goes out of scope.
template <typename Interface>
class Held {
public:
    Held() = default;
    Held(const Held&) = delete;
    Held& operator=(const Held&) = delete;
    Held(Held&&) = delete;
    Held& operator=(Held&&) = delete;
    ~Held()
    {
        if (m_object != nullptr) {
            m_object->Release();
        }
    }
    Interface** put() { return &m_object; }
    void** asVoid() { return reinterpret_cast<void**>(&m_object); }
    Interface* operator->() const { return m_object; }
    explicit operator bool() const { return m_object != nullptr; }

private:
    Interface* m_object = nullptr;
};

// A string for a call of the shell's, freed as it goes out of scope.
class Text {
public:
    explicit Text(const std::wstring& text)
    {
        m_value.vt = VT_BSTR;
        m_value.bstrVal = SysAllocString(text.c_str());
    }
    Text(const Text&) = delete;
    Text& operator=(const Text&) = delete;
    Text(Text&&) = delete;
    Text& operator=(Text&&) = delete;
    ~Text() { VariantClear(&m_value); }
    const VARIANT& value() const { return m_value; }
    BSTR string() const { return m_value.bstrVal; }

private:
    VARIANT m_value{};
};

// A token's user (its TOKEN_USER, whole); empty where it cannot be read.
std::vector<unsigned char> userOf(HANDLE token)
{
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> user(size);
    if (size < sizeof(TOKEN_USER) || GetTokenInformation(token, TokenUser, user.data(), size, &size) == FALSE ||
        IsValidSid(reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid) == FALSE) {
        return {};
    }
    return user;
}

bool startThroughTheShell(const std::wstring& program, const std::wstring& folder)
{
    Held<IShellWindows> windows;
    if (FAILED(
            CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_IShellWindows, windows.asVoid()))) {
        return false;
    }
    VARIANT none{};
    VariantInit(&none);
    long window = 0;
    Held<IDispatch> desktop;
    if (windows->FindWindowSW(&none, &none, SWC_DESKTOP, &window, SWFO_NEEDDISPATCH, desktop.put()) != S_OK ||
        !desktop) {
        return false;
    }
    Held<IServiceProvider> services;
    Held<IShellBrowser> browser;
    Held<IShellView> view;
    Held<IDispatch> background;
    Held<IShellFolderViewDual> folderView;
    Held<IDispatch> application;
    Held<IShellDispatch2> shell;
    if (FAILED(desktop->QueryInterface(IID_IServiceProvider, services.asVoid())) ||
        FAILED(services->QueryService(SID_STopLevelBrowser, IID_IShellBrowser, browser.asVoid())) ||
        FAILED(browser->QueryActiveShellView(view.put())) ||
        FAILED(view->GetItemObject(SVGIO_BACKGROUND, IID_IDispatch, background.asVoid())) ||
        FAILED(background->QueryInterface(IID_IShellFolderViewDual, folderView.asVoid())) ||
        FAILED(folderView->get_Application(application.put())) ||
        FAILED(application->QueryInterface(IID_IShellDispatch2, shell.asVoid()))) {
        return false;
    }
    // The shell starts what it starts as its own user, at its own integrity: only one of the person's own, this
    // install's (whoever runs this start, the user Setup ran as), and not above them (review C1). Another person's
    // desktop, a shell started elevated, or one that cannot be told, starts nothing.
    HWND shellWindow = nullptr;
    DWORD shellProcess = 0;
    if (FAILED(browser->GetWindow(&shellWindow)) || shellWindow == nullptr ||
        GetWindowThreadProcessId(shellWindow, &shellProcess) == 0 || !processIsThePersons(shellProcess)) {
        return false;
    }
    const Text file(program);
    const Text arguments(L"");
    const Text where(folder);
    const Text verb(L"open");
    VARIANT show{};
    show.vt = VT_I4;
    show.lVal = SW_SHOWNORMAL;
    return SUCCEEDED(shell->ShellExecute(file.string(), arguments.value(), where.value(), verb.value(), show));
}

} // namespace

bool tokenIsThePersons(void* theirs, void* ours)
{
    const std::vector<unsigned char> their = userOf(theirs);
    const std::vector<unsigned char> our = userOf(ours);
    return !their.empty() && !our.empty() &&
           EqualSid(reinterpret_cast<const TOKEN_USER*>(their.data())->User.Sid,
                    reinterpret_cast<const TOKEN_USER*>(our.data())->User.Sid) != FALSE &&
           !tokenRunsAboveThePerson(theirs);
}

bool processIsThePersons(unsigned long processId)
{
    if (processId == 0) {
        return false;
    }
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr) {
        return false;
    }
    HANDLE theirs = nullptr;
    HANDLE ours = nullptr;
    const bool opened = OpenProcessToken(process, TOKEN_QUERY, &theirs) != FALSE &&
                        OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &ours) != FALSE;
    CloseHandle(process);
    const bool persons = opened && tokenIsThePersons(theirs, ours);
    if (theirs != nullptr) {
        CloseHandle(theirs);
    }
    if (ours != nullptr) {
        CloseHandle(ours);
    }
    return persons;
}

bool startAnewThroughTheShell()
{
    std::vector<wchar_t> self(32768);
    const DWORD length = GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size()));
    if (length == 0 || length >= self.size()) {
        return false;
    }
    const std::filesystem::path program(std::wstring(self.data(), length));
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool started = startThroughTheShell(program.wstring(), program.parent_path().wstring());
    if (SUCCEEDED(apartment)) {
        CoUninitialize();
    }
    return started;
}

} // namespace qkvm::ui
