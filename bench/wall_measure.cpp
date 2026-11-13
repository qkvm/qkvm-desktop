#include "wall_measure.h"

#include <algorithm>
#include <numeric>

#include <QAbstractItemModel>
#include <QFile>
#include <QTextStream>
#include <QTimer>

#include <qkvm/base/clock.h>

// Windows' own headers; the first has to come first.
#include <windows.h>

#include <dbghelp.h>
#include <psapi.h>

namespace qkvm::bench {

namespace {

// A frame that is not ready within this long is one the monitor shows late, at sixty a second.
constexpr double frameMs = 16.7;
// How often the interface thread is asked to answer; from how late on it is looked at; how many
// of the latest answers are kept with what it was doing; and how far down what it calls.
constexpr int askEveryMs = 3;
constexpr qint64 lateFromUs = 10'000;
constexpr std::size_t mostCaught = 10;
constexpr int deepest = 12;
// While it stays late it is looked at again every so often, so many times at the most.
constexpr qint64 lookAgainUs = 10'000;
constexpr int mostLooks = 14;

// The most of them, and how many are over a frame's time.
struct Spread {
    double mean = 0;
    double mostOf = 0; // 99 in 100 are within this
    double longest = 0;
    qsizetype late = 0;
};

Spread spreadOf(std::vector<double> values)
{
    if (values.empty()) {
        return {};
    }
    std::ranges::sort(values);
    return {std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size()),
            values.at(std::min(values.size() - 1, values.size() * 99 / 100)), values.back(),
            static_cast<qsizetype>(std::ranges::count_if(values, [](double ms) { return ms > frameMs; }))};
}

// A wait of about so long that does not lean on how finely Windows counts time for everybody.
void nap(HANDLE timer, int ms)
{
    LARGE_INTEGER due{};
    due.QuadPart = -10'000LL * ms;
    if (timer != nullptr && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) != FALSE) {
        WaitForSingleObject(timer, INFINITE);
    } else {
        Sleep(static_cast<DWORD>(ms));
    }
}

} // namespace

WallMeasure::WallMeasure(QQuickWindow& window, ui::WallView& wall, QString file, int settleSeconds, int seconds,
                         std::function<void()> done, std::vector<Moment> start, QObject* parent)
    : QObject(parent), m_window(window), m_wall(wall), m_file(std::move(file)), m_seconds(seconds),
      m_done(std::move(done)), m_start(std::move(start))
{
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &m_interfaceThread, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    connect(&m_window, &QQuickWindow::frameSwapped, this, [this] {
        if (m_firstFrameMs < 0) {
            m_firstFrameMs = sinceTheStart();
        }
    });
    connect(m_wall.machines(), &QAbstractItemModel::modelReset, this, [this, settleSeconds] {
        if (m_cardsMs < 0 && m_wall.machines()->rowCount() > 0) {
            m_cardsMs = sinceTheStart();
            // The second thread gets ready while the wall settles, and not while the program starts.
            m_watching = true;
            m_watcher = std::thread([this] { watch(); });
            QTimer::singleShot(settleSeconds * 1000, this, &WallMeasure::begin);
        }
    });
    // The moments of a frame, each said by the thread that draws.
    const auto note = [this](std::vector<double>& where, qint64 sinceUs) {
        const double ms = static_cast<double>(base::steadyNowUs() - sinceUs) / 1000.0;
        const std::scoped_lock held(m_guard);
        if (m_measuring) {
            where.push_back(ms);
        }
    };
    // (A frame that the drawing thread asks for itself, for a new picture, has no sync with the
    // interface thread: its work begins as its rendering does.)
    connect(
        &m_window, &QQuickWindow::beforeFrameBegin, this,
        [this] {
            m_frameAskedUs = base::steadyNowUs();
            m_frameBeganUs = 0;
        },
        Qt::DirectConnection);
    connect(
        &m_window, &QQuickWindow::beforeRendering, this,
        [this] {
            if (m_frameBeganUs == 0) {
                m_frameBeganUs = base::steadyNowUs();
            }
        },
        Qt::DirectConnection);
    connect(
        &m_window, &QQuickWindow::beforeSynchronizing, this,
        [this, note] {
            note(m_waitedMs, m_frameAskedUs);
            m_frameBeganUs = base::steadyNowUs();
        },
        Qt::DirectConnection);
    connect(
        &m_window, &QQuickWindow::afterSynchronizing, this, [this, note] { note(m_heldMs, m_frameBeganUs); },
        Qt::DirectConnection);
    connect(
        &m_window, &QQuickWindow::afterRendering, this,
        [this, note] {
            note(m_workMs, m_frameBeganUs);
            m_frameReadyUs = base::steadyNowUs();
        },
        Qt::DirectConnection);
    connect(
        &m_window, &QQuickWindow::afterFrameEnd, this, [this, note] { note(m_handOverMs, m_frameReadyUs); },
        Qt::DirectConnection);
}

WallMeasure::~WallMeasure()
{
    m_watching = false;
    if (m_watcher.joinable()) {
        m_watcher.join();
    }
    if (m_interfaceThread != nullptr) {
        CloseHandle(m_interfaceThread);
    }
}

std::string WallMeasure::whereTheInterfaceThreadIs() const
{
    const HANDLE process = GetCurrentProcess();
    // Held still for the look, and let go at once: the names are found afterwards.
    std::vector<DWORD64> places;
    if (SuspendThread(m_interfaceThread) == static_cast<DWORD>(-1)) {
        return {};
    }
    CONTEXT context{};
    context.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(m_interfaceThread, &context) != FALSE) {
        STACKFRAME64 frame{};
        frame.AddrPC.Offset = context.Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = context.Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = context.Rsp;
        frame.AddrStack.Mode = AddrModeFlat;
        for (int depth = 0; depth < deepest; ++depth) {
            if (StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, m_interfaceThread, &frame, &context, nullptr,
                            SymFunctionTableAccess64, SymGetModuleBase64, nullptr) == FALSE ||
                frame.AddrPC.Offset == 0) {
                break;
            }
            places.push_back(frame.AddrPC.Offset);
        }
    }
    ResumeThread(m_interfaceThread);

    std::string where;
    for (const DWORD64 place : places) {
        char room[sizeof(SYMBOL_INFO) + 256] = {};
        auto* named = reinterpret_cast<SYMBOL_INFO*>(static_cast<char*>(room));
        named->SizeOfStruct = sizeof(SYMBOL_INFO);
        named->MaxNameLen = 255;
        DWORD64 into = 0;
        where += where.empty() ? "" : " < ";
        where += SymFromAddr(process, place, &into, named) != FALSE ? static_cast<const char*>(named->Name) : "?";
    }
    return where;
}

void WallMeasure::watch()
{
    const HANDLE process = GetCurrentProcess();
    // Every name is found now, and one look is taken for nothing: neither waits for the measuring.
    SymSetOptions(SYMOPT_UNDNAME);
    SymInitialize(process, nullptr, TRUE);
    static_cast<void>(whereTheInterfaceThreadIs());
    // (0x2: a timer of high resolution, where this Windows has them.)
    const HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x2, TIMER_ALL_ACCESS);
    while (m_watching) {
        m_answeredUs = 0;
        const qint64 askedUs = base::steadyNowUs();
        // What is posted to the interface thread it takes as soon as it comes round to anything.
        QMetaObject::invokeMethod(this, [this] { m_answeredUs = base::steadyNowUs(); }, Qt::QueuedConnection);
        std::string where;
        int looks = 0;
        while (m_watching && m_answeredUs == 0) {
            nap(timer, 1);
            if (looks < mostLooks && m_answeredUs == 0 &&
                base::steadyNowUs() - askedUs > lateFromUs + looks * lookAgainUs) {
                where += (looks == 0 ? "" : "\n      then: ") + whereTheInterfaceThreadIs();
                ++looks;
            }
        }
        if (const qint64 answeredUs = m_answeredUs; answeredUs != 0) {
            const double tookMs = static_cast<double>(answeredUs - askedUs) / 1000.0;
            const std::scoped_lock held(m_guard);
            if (m_measuring) {
                m_answerMs.push_back(tookMs);
                if (!where.empty()) {
                    m_caught.push_back({tookMs, static_cast<double>(askedUs - m_beganUs) / 1e6, where});
                }
            }
        }
        nap(timer, askEveryMs);
    }
    if (timer != nullptr) {
        CloseHandle(timer);
    }
    SymCleanup(process);
}

WallMeasure::Moment WallMeasure::now(const QString& what)
{
    PROCESS_MEMORY_COUNTERS_EX counted{};
    counted.cb = sizeof(counted);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counted), sizeof(counted));
    return {what, sinceTheStart(), static_cast<double>(counted.PrivateUsage) / (1024.0 * 1024.0)};
}

qint64 WallMeasure::sinceTheStart()
{
    FILETIME created{};
    FILETIME ended{};
    FILETIME inKernel{};
    FILETIME inUser{};
    FILETIME now{};
    GetProcessTimes(GetCurrentProcess(), &created, &ended, &inKernel, &inUser);
    GetSystemTimeAsFileTime(&now);
    const auto ticks = [](const FILETIME& time) {
        return (static_cast<qint64>(time.dwHighDateTime) << 32) | static_cast<qint64>(time.dwLowDateTime);
    };
    // (Windows counts in tenths of a microsecond.)
    return (ticks(now) - ticks(created)) / 10'000;
}

void WallMeasure::begin()
{
    static_cast<void>(m_meter.ask());
    {
        const std::scoped_lock held(m_guard);
        m_waitedMs.clear();
        m_workMs.clear();
        m_heldMs.clear();
        m_handOverMs.clear();
        m_answerMs.clear();
        m_caught.clear();
        m_beganUs = base::steadyNowUs();
        m_measuring = true;
    }
    m_inFrontAtFirst = GetForegroundWindow() == reinterpret_cast<HWND>(m_window.winId());
    QTimer::singleShot(m_seconds * 1000, this, &WallMeasure::end);
}

void WallMeasure::end()
{
    m_watching = false;
    if (m_watcher.joinable()) {
        m_watcher.join();
    }
    const Cost cost = m_meter.ask();
    Spread waited;
    Spread work;
    Spread held;
    Spread handOver;
    Spread answer;
    qsizetype frames = 0;
    std::vector<Late> caught;
    {
        const std::scoped_lock locked(m_guard);
        m_measuring = false;
        frames = static_cast<qsizetype>(m_workMs.size());
        waited = spreadOf(m_waitedMs);
        work = spreadOf(m_workMs);
        held = spreadOf(m_heldMs);
        handOver = spreadOf(m_handOverMs);
        answer = spreadOf(m_answerMs);
        caught = m_caught;
    }
    // The longest first, and no more of them than can be read.
    std::ranges::sort(caught, [](const Late& one, const Late& other) { return one.tookMs > other.tookMs; });
    if (caught.size() > mostCaught) {
        caught.resize(mostCaught);
    }
    const bool inFrontAtLast = GetForegroundWindow() == reinterpret_cast<HWND>(m_window.winId());

    if (QFile report(m_file); report.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream say(&report);
        say.setRealNumberNotation(QTextStream::FixedNotation);
        say.setRealNumberPrecision(2);
        say << "machines: " << m_wall.machines()->rowCount() << '\n';
        say << "a new picture every ms: " << m_wall.pace() << '\n';
        say << "the window, in the monitor's pixels: " << m_window.width() * m_window.effectiveDevicePixelRatio()
            << " x " << m_window.height() * m_window.effectiveDevicePixelRatio() << '\n';
        say << "the window was the one in front, at first and at last: " << (m_inFrontAtFirst ? "yes" : "no") << ", "
            << (inFrontAtLast ? "yes" : "no") << '\n';
        for (const Moment& moment : m_start) {
            say << "ms from the program's start until " << moment.what << ": " << moment.ms << " ("
                << moment.committedMb << " MB committed by then)\n";
        }
        say << "ms from the program's start to the window's first frame: " << m_firstFrameMs << '\n';
        say << "ms from the program's start to the wall's cards: " << m_cardsMs << '\n';
        say << "seconds measured: " << m_seconds << '\n';
        say << "percent of the PC's processors: " << cost.processorPercent << '\n';
        say << "MB committed: " << static_cast<double>(cost.committedBytes) / (1024.0 * 1024.0) << '\n';
        say << "MB of the graphics cards' own memory: " << static_cast<double>(cost.videoBytes) / (1024.0 * 1024.0)
            << '\n';
        say << "handles: " << cost.handles << '\n';
        say << "frames drawn: " << frames << '\n';
        say << "frames a second: " << static_cast<double>(frames) / std::max(1, m_seconds) << '\n';
        const auto line = [&say](const char* what, const Spread& spread) {
            say << what << ": mean " << spread.mean << ", 99 in 100 within " << spread.mostOf << ", longest "
                << spread.longest << ", over 16.7 ms " << spread.late << '\n';
        };
        line("ms a frame waited for the monitor before its work", waited);
        line("ms of work for a frame", work);
        line("ms of it with the interface thread held", held);
        line("ms to hand a finished frame over", handOver);
        line("ms the interface thread took to answer", answer);
        for (const Late& late : caught) {
            say << "late by " << late.tookMs << " ms at second " << late.atSecond
                << ", and doing: " << QString::fromStdString(late.doing) << '\n';
        }
    }
    if (m_done) {
        m_done();
    }
}

} // namespace qkvm::bench
