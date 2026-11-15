#include "quit_requests.h"

#include <array>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QMetaObject>

// Windows' own headers; the first has to come first.
#include <windows.h>

#include <sddl.h>

using namespace Qt::StringLiterals;

namespace qkvm::ui {

namespace {

// The rules of the event: one entry, of nothing inherited, that lets this process's Windows user, and nobody else, do
// anything with it. Empty where that user could not be told.
std::wstring onlyThisUser()
{
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
        return {};
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> user(size);
    std::wstring rules;
    LPWSTR named = nullptr;
    if (size != 0 && GetTokenInformation(token, TokenUser, user.data(), size, &size) != FALSE &&
        ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &named) != FALSE) {
        rules = L"D:P(A;;GA;;;" + std::wstring(named) + L")";
        LocalFree(named);
    }
    CloseHandle(token);
    return rules;
}

// The event, made here and now with those rules; none where one of the name was there already, or none could be made.
HANDLE makeEvent(const std::wstring& name)
{
    const std::wstring rules = onlyThisUser();
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (rules.empty() || ConvertStringSecurityDescriptorToSecurityDescriptorW(rules.c_str(), SDDL_REVISION_1,
                                                                              &descriptor, nullptr) == FALSE) {
        return nullptr;
    }
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    // Set once for each request, and taken back by being heard.
    HANDLE event = CreateEventW(&attributes, FALSE, FALSE, name.c_str());
    const DWORD error = GetLastError();
    LocalFree(descriptor);
    if (event != nullptr && error == ERROR_ALREADY_EXISTS) {
        CloseHandle(event);
        event = nullptr;
    }
    return event;
}

// Setup's ticket of a request that stands taken, where it is there: an event that takes itself back as it is waited
// for, so that taking it and Setup's taking it back are one step of Windows' each, and whoever is first has it (reviews
// C4 and R1). Whether it was taken here. One that may not be opened, or is not there, is not taken.
bool takeTheTicket(const QString& name)
{
    const std::wstring named = name.toStdWString();
    const HANDLE there = OpenEventW(SYNCHRONIZE, FALSE, named.c_str());
    if (there == nullptr) {
        return false;
    }
    const bool taken = WaitForSingleObject(there, 0) == WAIT_OBJECT_0;
    CloseHandle(there);
    return taken;
}

// Whether a mutex of this name is there now (one there that may not be opened is there all the same).
bool mutexIsThere(const QString& name)
{
    const std::wstring named = name.toStdWString();
    const HANDLE there = OpenMutexW(SYNCHRONIZE, FALSE, named.c_str());
    if (there == nullptr) {
        return GetLastError() == ERROR_ACCESS_DENIED;
    }
    CloseHandle(there);
    return true;
}

// The answer window's own procedure: its title is set by QuitRequests alone, through Windows' own procedure, and the
// window is closed by it alone. Anything else is Windows' own way.
LRESULT CALLBACK answering(HWND window, UINT message, WPARAM more, LPARAM what)
{
    if (message == WM_SETTEXT || message == WM_CLOSE) {
        return 0;
    }
    return DefWindowProcW(window, message, more, what);
}

} // namespace

QuitRequests::QuitRequests(QuitNames names, std::function<QString()> holds, std::function<void()> end, QuitPace pace,
                           QObject* parent)
    : QObject(parent), m_names(std::move(names)), m_pace(pace), m_kind(m_names.window.toStdWString()),
      m_holds(std::move(holds)), m_end(std::move(end))
{
    // (And the waiting thread's stop, first: without it nothing would be heard.)
    m_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE made = m_stop != nullptr ? makeEvent(m_names.event.toStdWString()) : nullptr;
    if (made == nullptr) {
        qWarning("qKVM: qKVM Setup cannot ask this copy of qKVM to close: the name it asks by was taken by another, or "
                 "could not be made. Quit qKVM before an upgrade.");
        return;
    }
    WNDCLASSW kind{};
    kind.lpfnWndProc = &answering;
    kind.hInstance = GetModuleHandleW(nullptr);
    kind.lpszClassName = m_kind.c_str();
    // A top-level window that is never shown, so that Setup finds it among them; with no parent and no owner. With a
    // caption: Windows gives another program the title of such a window without asking the window, so Setup reads it
    // while this program does not answer.
    HWND answerWindow = nullptr;
    if (RegisterClassW(&kind) != 0) {
        answerWindow = CreateWindowExW(0, m_kind.c_str(), L"0", WS_OVERLAPPED | WS_CAPTION, 0, 0, 0, 0, nullptr,
                                       nullptr, kind.hInstance, nullptr);
    }
    if (answerWindow == nullptr) {
        UnregisterClassW(m_kind.c_str(), kind.hInstance);
        CloseHandle(made);
        qWarning("qKVM: qKVM Setup cannot ask this copy of qKVM to close: its answer window could not be made.");
        return;
    }
    m_window = answerWindow;
    m_event = made;
    // When each request came is noted here, as it comes: the interface thread may get to it much later.
    m_waiting = std::thread([this, event = made, stop = static_cast<HANDLE>(m_stop)] {
        const std::array<HANDLE, 2> either{stop, event};
        while (WaitForMultipleObjects(static_cast<DWORD>(either.size()), either.data(), FALSE, INFINITE) ==
               WAIT_OBJECT_0 + 1) {
            const auto came = std::chrono::steady_clock::now();
            QMetaObject::invokeMethod(this, [this, came] { hear(came); }, Qt::QueuedConnection);
        }
    });
}

QuitRequests::~QuitRequests()
{
    if (m_waiting.joinable()) {
        SetEvent(static_cast<HANDLE>(m_stop));
        m_waiting.join();
    }
    if (m_stop != nullptr) {
        CloseHandle(static_cast<HANDLE>(m_stop));
    }
    if (m_window != nullptr) {
        DestroyWindow(static_cast<HWND>(m_window));
        UnregisterClassW(m_kind.c_str(), GetModuleHandleW(nullptr));
    }
    if (m_event != nullptr) {
        CloseHandle(static_cast<HANDLE>(m_event));
    }
}

bool QuitRequests::listens() const
{
    return m_event != nullptr && m_window != nullptr;
}

QString QuitRequests::answer() const
{
    if (m_window == nullptr) {
        return {};
    }
    std::wstring text(1024, L'\0');
    const int length = GetWindowTextW(static_cast<HWND>(m_window), text.data(), static_cast<int>(text.size()));
    return QString::fromWCharArray(text.data(), length);
}

void QuitRequests::hear(std::chrono::steady_clock::time_point made)
{
    const QString number = QString::number(++m_heard);
    if (m_ending) {
        say(number);
        return;
    }
    // Only a request that stands: heard soon enough after it was made, while Setup still wants it (it has not given up
    // on it) and runs. Any other was given up on, or is none of Setup's: the program stays.
    const auto tooLate = [this, &number] {
        say(number + u"* "_s +
            QCoreApplication::translate("Setup", "The request to close came too late: qKVM Setup had stopped waiting "
                                                 "for it. qKVM stays open; ask again."));
    };
    // (Whether Setup still wants it is said by its ticket alone, taken below as the request is taken: looking at it
    // first would take it.)
    if (std::chrono::steady_clock::now() - made > m_pace.fresh || !mutexIsThere(m_names.setup)) {
        tooLate();
        return;
    }
    QString holds = m_holds ? m_holds() : QString();
    if (!holds.isEmpty() || !m_end) {
        // One line, with no control sign between its words, and no longer than Setup reads; never no words at all,
        // which would say that the program is ending.
        for (QChar& sign : holds) {
            if (sign.unicode() < 0x20 || sign.unicode() == 0x7F) {
                sign = u' ';
            }
        }
        holds = holds.simplified().left(longestWords);
        if (holds.isEmpty()) {
            holds = QCoreApplication::translate("Setup", "qKVM cannot close now.");
        }
        say(number + u' ' + holds);
        return;
    }
    // Taken only by taking Setup's ticket (reviews C4 and R1): where Setup took it back first, it had withdrawn the
    // request, and the program stays; where this took it first, Setup finds it gone, and waits for the program as for
    // one that ends. Said before the end begins (the end lets the loop end, and the window goes with this).
    if (!takeTheTicket(m_names.wanted)) {
        tooLate();
        return;
    }
    m_ending = true;
    say(number);
    m_end();
}

void QuitRequests::say(const QString& title)
{
    if (m_window == nullptr) {
        return;
    }
    const std::wstring text = title.toStdWString();
    // (Windows' own procedure, past the window's, which refuses a title from anybody.)
    DefWindowProcW(static_cast<HWND>(m_window), WM_SETTEXT, 0, reinterpret_cast<LPARAM>(text.c_str()));
}

} // namespace qkvm::ui
