#include "wall_measure.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <map>
#include <numeric>
#include <set>
#include <vector>

#include <QAbstractItemModel>
#include <QFile>
#include <QTextStream>
#include <QTimer>

#include <qkvm/base/clock.h>

// Windows' own headers; the first has to come first.
#include <windows.h>

#include <dbghelp.h>
#include <malloc.h>
#include <psapi.h>
#include <tlhelp32.h>

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
                         std::function<void()> done, std::vector<Moment> start, int machines, QObject* parent)
    : QObject(parent), m_window(window), m_wall(wall), m_file(std::move(file)), m_seconds(seconds),
      m_done(std::move(done)), m_start(std::move(start)), m_machines(machines), m_settleSeconds(settleSeconds)
{
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &m_interfaceThread, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    connect(&m_window, &QQuickWindow::frameSwapped, this, [this] {
        if (m_firstFrameMs < 0) {
            m_firstFrameMs = sinceTheStart();
        }
    });
    // (The cards come one by one: as a reset of the model, or as rows put in.)
    const auto looked = [this] {
        const int rows = m_wall.machines()->rowCount();
        if (m_cardsMs >= 0 || rows <= 0) {
            return;
        }
        if (m_firstCardMs < 0) {
            m_firstCardMs = sinceTheStart();
            // Not all of them within three times the settling: measured with those it has.
            QTimer::singleShot(m_settleSeconds * 3000, this, [this] {
                if (m_cardsMs < 0) {
                    cardsCame();
                }
            });
        }
        if (rows >= m_machines) {
            cardsCame();
        }
    };
    connect(m_wall.machines(), &QAbstractItemModel::modelReset, this, looked);
    connect(m_wall.machines(), &QAbstractItemModel::rowsInserted, this, looked);
    m_partsAtFirst = memoryParts();
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
#if defined(_M_ARM64)
    // On ARM64 by Windows' own unwinder, each return address taken without the authentication code in its upper bits
    // (an address of this process has none there). The functions of Windows' own libraries and the Visual C++ runtime
    // sign their return addresses, ours too (/guard:signret): dbghelp's StackWalk64 does not take those bits off, and
    // stopped at the first such frame, two or three down (seen on the ARM test PC, 2026-11-21: NtDelayExecution <
    // RtlDelayExecution < ?, where this way names the whole of it, review arm 1, L9).
    if (GetThreadContext(m_interfaceThread, &context) != FALSE) {
        constexpr DWORD64 address = 0x0000'FFFF'FFFF'FFFFULL;
        for (int depth = 0; depth < deepest; ++depth) {
            const DWORD64 pc = context.Pc & address;
            if (pc == 0) {
                break;
            }
            places.push_back(pc);
            // (A return address is the instruction after the call: the call's own function is the one before it.)
            DWORD64 base = 0;
            PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(depth == 0 ? pc : pc - 4, &base, nullptr);
            if (function == nullptr) {
                context.Pc = context.Lr; // a function that keeps no frame: its return address is still in LR
            } else {
                PVOID handlerData = nullptr;
                DWORD64 establisher = 0;
                static_cast<void>(RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, pc, function, &context, &handlerData,
                                                   &establisher, nullptr));
            }
        }
    }
#else
    if (GetThreadContext(m_interfaceThread, &context) != FALSE) {
        STACKFRAME64 frame{};
        constexpr DWORD machine = IMAGE_FILE_MACHINE_AMD64;
        frame.AddrPC.Offset = context.Rip;
        frame.AddrFrame.Offset = context.Rbp;
        frame.AddrStack.Offset = context.Rsp;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Mode = AddrModeFlat;
        for (int depth = 0; depth < deepest; ++depth) {
            if (StackWalk64(machine, process, m_interfaceThread, &frame, &context, nullptr, SymFunctionTableAccess64,
                            SymGetModuleBase64, nullptr) == FALSE ||
                frame.AddrPC.Offset == 0) {
                break;
            }
            places.push_back(frame.AddrPC.Offset);
        }
    }
#endif
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

void WallMeasure::cardsCame()
{
    m_cardsMs = sinceTheStart();
    m_cardsCommittedMb = now(QString()).committedMb;
    // (Before the second thread is: the heaps are held still while they are walked.)
    m_partsWithCards = memoryParts();
    // The second thread gets ready while the wall settles, and not while the program starts.
    m_watching = true;
    m_watcher = std::thread([this] { watch(); });
    QTimer::singleShot(m_settleSeconds * 1000, this, &WallMeasure::begin);
}

QString WallMeasure::memoryParts()
{
    constexpr double mb = 1024.0 * 1024.0;
    // What is its own and committed, by the piece of memory it was asked for as (an allocation): a thread's stack is
    // the one that has a guard page at its end, which no heap has.
    std::uint64_t own = 0;
    std::uint64_t stacks = 0;
    int stacksCount = 0;
    struct Allocation {
        std::uint64_t committed = 0;
        bool guarded = false;
    };
    std::vector<std::pair<void*, Allocation>> allocations;
    {
        MEMORY_BASIC_INFORMATION region{};
        for (const char* at = nullptr; VirtualQuery(at, &region, sizeof(region)) == sizeof(region);
             at = static_cast<const char*>(region.BaseAddress) + region.RegionSize) {
            if (region.State != MEM_COMMIT || region.Type != MEM_PRIVATE) {
                continue;
            }
            own += region.RegionSize;
            if (allocations.empty() || allocations.back().first != region.AllocationBase) {
                allocations.emplace_back(region.AllocationBase, Allocation{});
            }
            allocations.back().second.committed += region.RegionSize;
            allocations.back().second.guarded = allocations.back().second.guarded || (region.Protect & PAGE_GUARD) != 0;
        }
        for (const auto& [base, allocation] : allocations) {
            if (allocation.guarded) {
                stacks += allocation.committed;
                ++stacksCount;
            }
        }
    }
    // The heaps, each walked while it is held: what is committed in their regions, what is in use there, and the
    // pieces in use by their size; a piece in use outside every region is one the heap asked Windows for by itself
    // (a large one), and is committed beside the regions.
    constexpr std::array<std::uint64_t, 6> upTo{4 << 10, 64 << 10, 512 << 10, 2 << 20, 8 << 20, ~std::uint64_t{0}};
    constexpr std::array<const char*, 6> named{"up to 4 KB", "up to 64 KB", "up to 512 KB",
                                               "up to 2 MB", "up to 8 MB",  "larger"};
    std::array<std::uint64_t, 6> pieces{};
    std::array<std::uint64_t, 6> piecesBytes{};
    std::uint64_t inRegions = 0;
    std::uint64_t large = 0;
    std::uint64_t inUse = 0;
    std::map<std::uint64_t, std::uint64_t> sizes; // by 16 KB
    std::set<const void*> ofHeaps;
    std::vector<HANDLE> heaps(64);
    DWORD count = GetProcessHeaps(static_cast<DWORD>(heaps.size()), heaps.data());
    if (count > heaps.size()) {
        heaps.resize(count);
        count = GetProcessHeaps(static_cast<DWORD>(heaps.size()), heaps.data());
    }
    heaps.resize(std::min<std::size_t>(count, heaps.size()));
    for (const HANDLE heap : heaps) {
        if (HeapLock(heap) == FALSE) {
            continue;
        }
        PROCESS_HEAP_ENTRY entry{};
        const char* regionFirst = nullptr;
        const char* regionLast = nullptr;
        // (Which allocations of Windows' are a heap's: the rest are told apart below.)
        const auto noteHeapAt = [&ofHeaps](const void* at) {
            MEMORY_BASIC_INFORMATION found{};
            if (VirtualQuery(at, &found, sizeof(found)) == sizeof(found)) {
                ofHeaps.insert(found.AllocationBase);
            }
        };
        while (HeapWalk(heap, &entry) != FALSE) {
            if ((entry.wFlags & PROCESS_HEAP_REGION) != 0) {
                inRegions += entry.Region.dwCommittedSize;
                regionFirst = static_cast<const char*>(entry.Region.lpFirstBlock);
                regionLast = static_cast<const char*>(entry.Region.lpLastBlock);
                noteHeapAt(entry.lpData);
                continue;
            }
            if ((entry.wFlags & PROCESS_HEAP_ENTRY_BUSY) == 0) {
                continue;
            }
            const std::uint64_t size = entry.cbData;
            inUse += size;
            const auto* at = static_cast<const char*>(entry.lpData);
            if (regionFirst == nullptr || at < regionFirst || at >= regionLast) {
                large += size + entry.cbOverhead;
                noteHeapAt(at);
            }
            const auto place = static_cast<std::size_t>(
                std::ranges::find_if(upTo, [size](std::uint64_t most) { return size <= most; }) - upTo.begin());
            ++pieces.at(place);
            piecesBytes.at(place) += size;
            // A piece of 64 KB and more, by its size.
            if (size >= (64 << 10)) {
                ++sizes[size / (16 << 10)];
            }
        }
        HeapUnlock(heap);
    }
    DWORD threads = 0;
    // The threads, by the name each was given (libdatachannel's "RTC worker" and "RTC poll", libjuice's, Qt's), and
    // the others by the library each began in (where it began, as Windows keeps it: a thread of the standard library's,
    // std::thread or a QThread, begins in its runtime, whoever made it).
    std::map<std::wstring, int> begunIn;
    using AskThread = LONG(WINAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
    // (Windows hands out every function by one type of its own: the bits are the function's.)
    const auto askThread =
        std::bit_cast<AskThread>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    if (const HANDLE all = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); all != INVALID_HANDLE_VALUE) {
        THREADENTRY32 thread{};
        thread.dwSize = sizeof(thread);
        for (BOOL more = Thread32First(all, &thread); more != FALSE; more = Thread32Next(all, &thread)) {
            if (thread.th32OwnerProcessID != GetCurrentProcessId()) {
                continue;
            }
            ++threads;
            std::wstring where = L"?";
            const HANDLE one = OpenThread(THREAD_QUERY_INFORMATION, FALSE, thread.th32ThreadID);
            PWSTR described = nullptr;
            if (one != nullptr && SUCCEEDED(GetThreadDescription(one, &described)) && described != nullptr) {
                if (described[0] != L'\0') {
                    where = L"\"" + std::wstring(described) + L"\"";
                }
                LocalFree(described);
            }
            void* start = nullptr;
            // (9: the address it began at, ThreadQuerySetWin32StartAddress.)
            if (where == L"?" && one != nullptr && askThread != nullptr &&
                askThread(one, 9, static_cast<PVOID>(&start), sizeof(start), nullptr) >= 0) {
                HMODULE module = nullptr;
                wchar_t name[MAX_PATH] = {};
                if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       static_cast<LPCWSTR>(start), &module) != FALSE &&
                    GetModuleFileNameW(module, static_cast<wchar_t*>(name), MAX_PATH) > 0) {
                    const std::wstring path(static_cast<const wchar_t*>(name));
                    where = path.substr(path.find_last_of(L'\\') + 1);
                }
            }
            if (one != nullptr) {
                CloseHandle(one);
            }
            ++begunIn[where];
        }
        CloseHandle(all);
    }
    const std::uint64_t heapsCommitted = inRegions + large;
    const std::uint64_t rest = own - std::min(own, heapsCommitted + stacks);
    QString said;
    QTextStream say(&said);
    say.setRealNumberNotation(QTextStream::FixedNotation);
    say.setRealNumberPrecision(1);
    say << "MB of its own memory committed: " << static_cast<double>(own) / mb << '\n';
    say << "MB of it its heaps' (" << heaps.size() << " heaps): " << static_cast<double>(heapsCommitted) / mb
        << ", of which in use " << static_cast<double>(inUse) / mb << " and pieces asked of Windows by themselves "
        << static_cast<double>(large) / mb << '\n';
    for (std::size_t place = 0; place < upTo.size(); ++place) {
        say << "  pieces in use " << named.at(place) << ": " << pieces.at(place) << ", "
            << static_cast<double>(piecesBytes.at(place)) / mb << " MB\n";
    }
    std::vector<std::pair<std::uint64_t, std::uint64_t>> common(sizes.begin(), sizes.end());
    std::ranges::sort(
        common, [](const auto& one, const auto& other) { return one.second * one.first > other.second * other.first; });
    say << "  the sizes of 64 KB and more that hold the most, in KB (and how many):";
    for (std::size_t at = 0; at < std::min<std::size_t>(8, common.size()); ++at) {
        say << ' ' << common.at(at).first * 16 << " (" << common.at(at).second << ')';
    }
    say << '\n';
    say << "MB of it its threads' stacks (" << stacksCount << " stacks, " << threads
        << " threads): " << static_cast<double>(stacks) / mb << '\n';
    say << "  threads by their name, or the library they began in:";
    for (const auto& [where, began] : begunIn) {
        say << ' ' << QString::fromStdWString(where) << " (" << began << ')';
    }
    say << '\n';
    say << "MB of it outside a heap and a stack (the graphics driver's, a library's own): "
        << static_cast<double>(rest) / mb << '\n';
    // Those by how much each allocation of Windows' has committed (to the next MB up), the sizes that hold the most.
    std::map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>> outside; // MB: how many, bytes
    for (const auto& [base, allocation] : allocations) {
        if (!allocation.guarded && !ofHeaps.contains(base)) {
            auto& one = outside[(allocation.committed + (1 << 20) - 1) >> 20];
            ++one.first;
            one.second += allocation.committed;
        }
    }
    std::vector<std::pair<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>>> most(outside.begin(), outside.end());
    std::ranges::sort(most, [](const auto& one, const auto& other) { return one.second.second > other.second.second; });
    say << "  of it, the allocations by MB committed in each, the sizes that hold the most (how many, MB in all):";
    for (std::size_t at = 0; at < std::min<std::size_t>(8, most.size()); ++at) {
        say << ' ' << most.at(at).first << " (" << most.at(at).second.first << ", "
            << static_cast<double>(most.at(at).second.second) / mb << ')';
    }
    say << '\n';
    return said;
}

void WallMeasure::begin()
{
    m_beganCommittedMb = now(QString()).committedMb;
    // With QKVM_RIG_COMPACT set: the heaps compacted as the measuring begins, once (the runtime's and every heap of
    // the program's), and what is committed then noted: what giving back the burst's freed memory comes to.
    if (qEnvironmentVariableIsSet("QKVM_RIG_COMPACT")) {
        static_cast<void>(_heapmin());
        std::vector<HANDLE> heaps(256);
        const DWORD count = GetProcessHeaps(static_cast<DWORD>(heaps.size()), heaps.data());
        for (DWORD at = 0; at < std::min<DWORD>(count, static_cast<DWORD>(heaps.size())); ++at) {
            static_cast<void>(HeapCompact(heaps.at(at), 0));
        }
        m_compactedMb = now(QString()).committedMb;
    }
    static_cast<void>(m_meter.ask());
    if (m_more) {
        static_cast<void>(m_more(false));
    }
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
    if (m_atBegin) {
        m_atBegin();
    }
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
        say << "machines the wall was to have: " << m_machines << '\n';
        say << "ms from the program's start to the wall's first card: " << m_firstCardMs << '\n';
        say << "ms from the program's start to the wall's cards: " << m_cardsMs << " (" << m_cardsCommittedMb
            << " MB committed by then)\n";
        say << "MB committed as the measuring began: " << m_beganCommittedMb << '\n';
        if (m_compactedMb >= 0) {
            say << "MB committed once the heaps were compacted then: " << m_compactedMb << '\n';
        }
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
        // (After the measuring: the heaps are held still while they are walked.)
        say << "what the memory was made of as the window was shown:\n" << m_partsAtFirst;
        say << "what it was made of when the wall had its cards:\n" << m_partsWithCards;
        say << "what it is made of at the end of the measuring:\n" << memoryParts();
        if (m_more) {
            say << m_more(true);
        }
    }
    if (m_done) {
        m_done();
    }
}

} // namespace qkvm::bench
