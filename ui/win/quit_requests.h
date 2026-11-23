#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>

#include <QObject>
#include <QString>

namespace qkvm::ui {

// The names of the contract with qKVM's Setup (0.1.2): packaging/qkvm-desktop.iss names the same, and
// tools/test_setup.ps1 holds the two to each other. Both events are of this session's own names ("Local\").
inline constexpr char quitRequestEvent[] = "Local\\qKVM-Desktop-Quit";
inline constexpr char quitAnswerWindow[] = "qKVM Desktop quit answer";
// Setup's own ticket of a request that stands: an event that takes itself back as it is waited for, set once for each
// request. The program takes the request by taking the ticket; Setup withdraws the request by taking it back; whoever
// is first has it, in one step of Windows', and the other finds it gone: never both, and nothing between that a stall
// or a timeout could cut (reviews C4 and R1). Never by its being there, which a handle kept elsewhere keeps.
inline constexpr char quitWantedEvent[] = "Local\\qKVM-Desktop-Quit-Wanted";

// What a request is known by: the event Setup sets, the class of the window that answers, Setup's ticket of a request
// that stands, and the mutex Setup holds while it runs (setupMutexName). A test names its own.
struct QuitNames {
    QString event;
    QString window;
    QString wanted;
    QString setup;
};

// How long after it was made a request is still honoured: longer than Setup waits for an answer (10 s, and the second
// it gives after giving up), so that one is refused as too late only once Setup has given up on it; until then Setup's
// withdrawal (its ticket taken back) is what says that it no longer stands (review N1). tools/test_setup.ps1 holds it
// to the setup's CloseWaitMs.
struct QuitPace {
    std::chrono::milliseconds fresh{12'000};
};

// qKVM's Setup asking an installed copy of the program to end, the orderly way Install and restart ends it, so that it
// can upgrade it: the person chose "Close qKVM and continue", or a silent install asked. The request is an event of
// Windows that this makes, of this session's names, that only this Windows user may set: it carries nothing, and says
// only "end, if nothing would be cut short". An event of that name that is there already (made by another, who may let
// others set it) is never listened to, and that is said (qWarning).
//
// A thread of its own waits for the event and notes when each request came; the interface thread then honours one only
// where it stands: heard within `QuitPace::fresh` of being made, while Setup's mutex is there (a Setup runs), and taken
// only by taking Setup's ticket (Setup has not withdrawn it: Setup takes it back as it gives up, and whoever is first
// has it). Any other is answered as too late, and the program stays: an interface that did not answer for a while never
// ends the program for a Setup that has gone on without it.
//
// The answer is the title of a window of this program's that nobody sees, which Setup reads (it takes the window only
// where it is of the process it found, whose file Windows says is the installed program): "0" before any request; "<n>"
// once the n-th was heard and the program is ending; "<n> <words>" where it was heard and the program stays, the words
// saying what holds it (one line, at most `longestWords` signs); "<n>* <words>" where it came too late (a request that
// Setup gave up on, or none of Setup's), so that Setup never takes that for the program's staying open. Nothing but
// this sets that title: a WM_SETTEXT or a WM_CLOSE from outside is refused. Setup asks only a process that has this
// window.
//
// Decided on the interface thread: a program whose interface does not answer does not answer here either, and Setup
// then offers to end it by force.
class QuitRequests : public QObject {
    Q_OBJECT

public:
    static constexpr qsizetype longestWords = 400;

    // At each request that stands, `holds` says what keeps the program from ending now (empty where nothing does); only
    // where nothing does is `end` called, once: the orderly end.
    QuitRequests(QuitNames names, std::function<QString()> holds, std::function<void()> end, QuitPace pace = {},
                 QObject* parent = nullptr);
    ~QuitRequests() override;
    QuitRequests(const QuitRequests&) = delete;
    QuitRequests& operator=(const QuitRequests&) = delete;
    QuitRequests(QuitRequests&&) = delete;
    QuitRequests& operator=(QuitRequests&&) = delete;

    // Whether a request can reach this copy: the event and the window were made here.
    bool listens() const;
    // The answer window's title now (empty where there is no window).
    QString answer() const;
    // How many requests the waiting thread has noted the coming of so far (each as it came, before the interface thread
    // hears it): what a test waits for, rather than for a time, before it stalls the interface (outside review of
    // 0.2.0, 1.10).
    quint32 noted() const { return m_noted.load(); }

private:
    void hear(std::chrono::steady_clock::time_point made);
    void say(const QString& title);

    QuitNames m_names;
    QuitPace m_pace;
    // Windows' own handles: the event, the waiting thread's stop, and the window (an HWND); nothing where they could
    // not be made.
    void* m_event = nullptr;
    void* m_stop = nullptr;
    void* m_window = nullptr;
    std::wstring m_kind;
    std::thread m_waiting;
    std::function<QString()> m_holds;
    std::function<void()> m_end;
    quint32 m_heard = 0;
    std::atomic<quint32> m_noted{0};
    bool m_ending = false;
};

} // namespace qkvm::ui
