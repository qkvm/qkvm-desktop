#pragma once

#include <QString>
#include <functional>
#include <initializer_list>
#include <memory>
#include <thread>
#include <utility>

namespace qkvm::core {

// Local, bounded diagnostics. Fields are numbers only: callers cannot accidentally
// record passwords, cookies, SDP, addresses, device names or picture contents.
// One writer per process. Nothing of it waits for the disk where it is made or where a record is
// made: the line is made there (with that moment's time, under the lock the lines wait under, so
// the file's times are in order) and handed to the writer's own thread, which alone makes, opens,
// writes and closes the file; nothing recorded from the interface thread can stall it (a slow or
// full disk included). At the most mostWaiting lines wait to be written; beyond that they are
// dropped, counted, and the count said in a `diagnostics_dropped` line once every line that waited
// before is written. A line its thread could not write (memory run out, say) is counted so too. A
// writer whose thread cannot be begun leaves diagnostics off.
class VideoDiagnostics {
public:
    // For tests: `beforeStarting` on the thread that makes it, just before its writer's thread is
    // begun (one that throws: a thread that cannot be begun); `beforeWriting` on the writer's
    // thread before each line is written (a disk that is slow, or what throws there).
    struct ForTests {
        std::function<void()> beforeStarting = {};
        std::function<void()> beforeWriting = {};
    };
    explicit VideoDiagnostics(const QString& path, qint64 maximumBytes = qint64{1024} * 1024);
    VideoDiagnostics(const QString& path, qint64 maximumBytes, ForTests forTests);
    // What waits is written first, atTheEndMs at the most; after that the writer's thread is left
    // to finish by itself (the program is ending, and waits for no disk).
    ~VideoDiagnostics();
    VideoDiagnostics(const VideoDiagnostics&) = delete;
    VideoDiagnostics& operator=(const VideoDiagnostics&) = delete;
    VideoDiagnostics(VideoDiagnostics&&) = delete;
    VideoDiagnostics& operator=(VideoDiagnostics&&) = delete;
    // Whether its file is open: not where it never opened, nor once a write that failed closed it.
    // It waits for the writer's thread to have opened it (or not), atTheEndMs at the most: for the
    // rigs and the tests, as flush() is.
    bool isOpen() const;
    // Every line recorded before it was called is in the file (or was dropped, and that said)
    // when it returns: for a reader in this process, the rigs and the tests. It waits for the
    // disk, so the program itself never calls it.
    void flush() const;
    static void record(const char* event, std::initializer_list<std::pair<const char*, qint64>> fields = {});
    // For the crash handler (ui/win/crash_report.cpp), on the thread that crashes: the writer's
    // thread is given atACrashMs at the most to write the lines that wait, the nearest to the
    // crash. It takes no lock, makes nothing and calls no Qt: it reads what the writer counts, and
    // sleeps. On the writer's own thread, or where no writer runs, it returns at once.
    static void writeWhatWaitsAtACrash() noexcept;

    static constexpr qsizetype mostWaiting = 1024;
    static constexpr int atTheEndMs = 2000;
    static constexpr int atACrashMs = 2000;

private:
    struct Writer;
    // The process's one writer, where there is one (under the .cpp's lock): a plain pointer, which
    // nothing destroys at the program's end; this object owns it.
    static Writer*& current();
    std::shared_ptr<Writer> m_writer;
    std::thread m_thread;
};
} // namespace qkvm::core
