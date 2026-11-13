#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QObject>
#include <QThread>
#include <QTimer>

#include <intrin.h>
#include <windows.h>

namespace qkvm::core::testing {

// How long the thread it is made on is still, for the tests of what may not hold up the interface thread (review 3's
// HIGH 1, 2026-11-06), and how much of that it spent working. A thread of its own hands it a word every millisecond (by
// a timer of Windows' that keeps the millisecond: a sleep of one lasts one of Windows' ticks, 15.6 ms where nothing on
// the PC asked for finer), and each word says how long it waited to be taken (a timer of the thread's own is no
// measure: Windows turns one no more often than every 10 ms), and how much of that wait the thread was running: its
// own processor time, as Windows counts its cycles (in the units of the processor's time-stamp counter, at a steady
// pace whatever the core's).
// What it worked is what the program did on that thread; the rest of a wait is what it waited for (a lock, a file) and
// Windows' own pauses of it, which on a busy PC were half a second (2026-11-06, release, the slow cores beside a full
// build: 507.8 ms of stillness, 0.3 of it working). So while it measures, the thread and the one that asks run above
// the PC's other work (Windows' "highest" of a normal program: 10 instead of 8), and a test bounds the two apart, by
// the two below.

// The most the measured thread may work in any one wait: a monitor's frame (SPEC's target), in the build that is handed
// out and in the one with the standard library's checks (2026-11-06, the busy-screen cases on the slow cores beside a
// full build: at most 5.6 ms in 38 runs of the release build, 6.3 in 10 of the debug build; with the frames read on the
// thread, 38.7 and 265 ms). Under the address sanitizer, which checks every access the program's own code makes, the
// thread's own work in one wait came to 18 to 52 ms in the card's case (10.9 at most in the screen's), and with the
// frames read on it to 262 and 268 ms: there the bound lies between the two, and still tells that the reading came back
// onto the thread.
#if defined(__SANITIZE_ADDRESS__)
inline constexpr double mostWorkInOneWaitMs = 100.0;
#else
inline constexpr double mostWorkInOneWaitMs = 16.0;
#endif
// And how long it may be still at all: that work, and what it waits for and what Windows still takes from a thread
// above the PC's other work now and then (17.8 ms of pause, 0.2 of it working, at the most in those 38 runs).
inline constexpr double mostStillMs = mostWorkInOneWaitMs + 34.0;

class Stillness {
public:
    Stillness() = default;
    ~Stillness() { stop(); }
    Stillness(const Stillness&) = delete;
    Stillness& operator=(const Stillness&) = delete;
    Stillness(Stillness&&) = delete;
    Stillness& operator=(Stillness&&) = delete;

    // (On the thread to be measured.)
    void start()
    {
        stop();
        m_longestNs = 0;
        m_workedOfLongest = 0;
        m_longestWorked = 0;
        m_over.clear();
        m_words = 0;
        HANDLE measured = nullptr;
        static_cast<void>(DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &measured,
                                          THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0));
        m_measured = measured;
        m_priorityBefore = GetThreadPriority(GetCurrentThread());
        static_cast<void>(SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST));
        m_clock.start();
        m_stampsAtStart = __rdtsc();
        m_going = true;
        m_asker = std::thread([this] {
            static_cast<void>(SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST));
            HANDLE tick =
                CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            while (m_going.load()) {
                ULONG64 cycles = 0;
                static_cast<void>(QueryThreadCycleTime(m_measured, &cycles));
                const qint64 sent = m_clock.nsecsElapsed();
                QMetaObject::invokeMethod(
                    &m_here, [this, sent, cycles] { took(m_clock.nsecsElapsed() - sent, cycles); },
                    Qt::QueuedConnection);
                LARGE_INTEGER due{};
                due.QuadPart = -10'000; // (a millisecond from now, in Windows' tenths of a microsecond)
                if (tick == nullptr || SetWaitableTimer(tick, &due, 0, nullptr, nullptr, FALSE) == 0 ||
                    WaitForSingleObject(tick, INFINITE) != WAIT_OBJECT_0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            if (tick != nullptr) {
                CloseHandle(tick);
            }
        });
    }
    // Stops asking, and takes what was said before. (On the thread that was measured.)
    void stop()
    {
        if (m_asker.joinable()) {
            m_going = false;
            m_asker.join();
            QCoreApplication::sendPostedEvents(&m_here, QEvent::MetaCall);
            const qint64 ns = m_clock.nsecsElapsed();
            m_stampsPerNs = ns > 0 ? static_cast<double>(__rdtsc() - m_stampsAtStart) / static_cast<double>(ns) : 0;
            static_cast<void>(SetThreadPriority(GetCurrentThread(), m_priorityBefore));
            CloseHandle(m_measured);
            m_measured = nullptr;
        }
    }

    // In milliseconds, to a tenth: the longest wait, how much of it the thread was working, and the most it was working
    // in any one wait.
    double longestMs() const { return tenths(m_longestNs); }
    double workedOfLongestMs() const { return tenths(nsOf(m_workedOfLongest)); }
    double longestWorkedMs() const { return tenths(nsOf(m_longestWorked)); }
    int words() const { return m_words; }
    // The waits longer than 8 ms, in milliseconds, in the order they came, each with how much of it the thread was
    // working: for the message of a test that fails.
    QByteArray longOnes() const
    {
        QByteArray said;
        for (const auto& [waited, worked] : m_over) {
            said += QByteArray::number(tenths(waited)) + " (working " + QByteArray::number(tenths(nsOf(worked))) + ") ";
        }
        return said.isEmpty() ? QByteArray("none over 8 ms") : said.trimmed();
    }
    // What a test says of it.
    QByteArray said() const
    {
        return "still for " + QByteArray::number(longestMs()) + " ms at the longest, working " +
               QByteArray::number(workedOfLongestMs()) + " ms of it; working " + QByteArray::number(longestWorkedMs()) +
               " ms at the most in one wait (" + QByteArray::number(words()) +
               " words asked; over 8 ms: " + longOnes() + ")";
    }

private:
    static double tenths(qint64 ns) { return std::round(static_cast<double>(ns) / 100'000.0) / 10.0; }
    qint64 nsOf(ULONG64 cycles) const
    {
        return m_stampsPerNs > 0 ? static_cast<qint64>(static_cast<double>(cycles) / m_stampsPerNs) : 0;
    }
    void took(qint64 waitedNs, ULONG64 cyclesWhenSent)
    {
        ULONG64 cycles = 0;
        static_cast<void>(QueryThreadCycleTime(GetCurrentThread(), &cycles));
        const ULONG64 worked = cycles > cyclesWhenSent ? cycles - cyclesWhenSent : 0;
        ++m_words;
        if (waitedNs > m_longestNs) {
            m_longestNs = waitedNs;
            m_workedOfLongest = worked;
        }
        m_longestWorked = std::max(m_longestWorked, worked);
        if (waitedNs > 8'000'000 && m_over.size() < 64) {
            m_over.emplace_back(waitedNs, worked);
        }
    }

    QObject m_here;
    QElapsedTimer m_clock;
    unsigned long long m_stampsAtStart = 0;
    double m_stampsPerNs = 0;
    HANDLE m_measured = nullptr;
    int m_priorityBefore = THREAD_PRIORITY_NORMAL;
    std::atomic<bool> m_going = false;
    std::thread m_asker;
    qint64 m_longestNs = 0;
    ULONG64 m_workedOfLongest = 0;
    ULONG64 m_longestWorked = 0;
    std::vector<std::pair<qint64, ULONG64>> m_over;
    int m_words = 0;
};

// Runs the thread's loop for `ms`: the way a test waits while it measures (QTest::qWait sleeps 10 ms at a time between
// its turns of the loop, which would be measured as the thread's own stillness).
inline void runFor(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// A thread of its own for stand-ins whose work is not to be counted as the tested thread's (a console that sends a busy
// screen): what is made there is used there and ended there, through there(), which waits for it.
class Elsewhere {
public:
    Elsewhere()
    {
        m_thread.start();
        m_host.moveToThread(&m_thread);
    }
    ~Elsewhere()
    {
        m_thread.quit();
        m_thread.wait();
    }
    Elsewhere(const Elsewhere&) = delete;
    Elsewhere& operator=(const Elsewhere&) = delete;
    Elsewhere(Elsewhere&&) = delete;
    Elsewhere& operator=(Elsewhere&&) = delete;

    void there(const std::function<void()>& what)
    {
        QMetaObject::invokeMethod(&m_host, what, Qt::BlockingQueuedConnection);
    }

    // One object made there, and ended there also where a test ends early. Used from the tested thread only through
    // there(), or while that thread does nothing else.
    template <typename T>
    class Kept {
    public:
        explicit Kept(Elsewhere& at) : m_at(at)
        {
            m_at.there([this] { m_made = std::make_unique<T>(); });
        }
        ~Kept()
        {
            m_at.there([this] { m_made.reset(); });
        }
        Kept(const Kept&) = delete;
        Kept& operator=(const Kept&) = delete;
        Kept(Kept&&) = delete;
        Kept& operator=(Kept&&) = delete;

        T& operator*() const { return *m_made; }
        T* operator->() const { return m_made.get(); }

    private:
        Elsewhere& m_at;
        std::unique_ptr<T> m_made;
    };

private:
    QThread m_thread;
    QObject m_host;
};

} // namespace qkvm::core::testing
