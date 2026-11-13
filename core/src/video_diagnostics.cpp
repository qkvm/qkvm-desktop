#include <qkvm/core/video_diagnostics.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>

using namespace Qt::StringLiterals;
namespace qkvm::core {

// What waits to be written, under `lock`; the file is the writer thread's alone (Writer::run).
struct VideoDiagnostics::Writer {
    QString path;
    qint64 maximum = 0;
    std::function<void()> beforeWriting;

    std::mutex lock;
    std::condition_variable wake;    // a line waits, or it is to stop
    std::condition_variable written; // its file opened (or did not), a line was written, or it stopped
    std::deque<QByteArray> waiting;
    quint64 handed = 0;         // lines handed to it
    quint64 done = 0;           // of those, written (or let go of where they could not be)
    quint64 dropped = 0;        // lines let go of since the last were said: no room, or not written
    std::optional<bool> opened; // whether its file opened, once its thread knows
    bool stopping = false;
    bool finished = false;
    bool watched = false;           // the process's one writer: what it counts is the crash handler's to read
    std::atomic<bool> open = false; // its file is open (false again once a write closed it)

    void run();
    void loop(QFile& file);
    void write(QFile& file, const QByteArray& line);
};

namespace {
std::mutex guard; // who the one writer is (VideoDiagnostics::current)

// What the crash handler reads of the one writer (writeWhatWaitsAtACrash), without a lock: atomics alone,
// destroyed by nothing, there for as long as the process is. Its lines handed and done are counted here
// too, under the writer's own lock, while it is the one (Writer::watched).
struct AtACrash {
    std::atomic<bool> on{false};
    std::atomic<std::thread::id> writer{};
    std::atomic<quint64> handed{0};
    std::atomic<quint64> done{0};
};
AtACrash atACrash;

bool nameIsSafe(const QByteArray& name)
{
    return !name.isEmpty() && name.size() <= 64 && std::ranges::all_of(name, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}

QByteArray lineOf(const char* event, const QJsonObject& values)
{
    const QJsonObject entry{{u"time"_s, QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                            {u"pid"_s, QCoreApplication::applicationPid()},
                            {u"event"_s, QString::fromLatin1(event)},
                            {u"values"_s, values}};
    return QJsonDocument(entry).toJson(QJsonDocument::Compact) + '\n';
}
} // namespace

VideoDiagnostics::Writer*& VideoDiagnostics::current()
{
    static Writer* one = nullptr;
    return one;
}

VideoDiagnostics::VideoDiagnostics(const QString& path, qint64 maximumBytes)
    : VideoDiagnostics(path, maximumBytes, ForTests{})
{}

VideoDiagnostics::VideoDiagnostics(const QString& path, qint64 maximumBytes, ForTests forTests)
{
    // Nothing leaves it, and nothing here waits for the disk: where its thread cannot be begun (std::thread throws),
    // nothing runs, nothing records to it, and the program goes on without diagnostics (from the second reading of
    // c7ec58c, L1: it was the one writer before its thread was begun, and the program ended at its start).
    try {
        auto made = std::make_shared<Writer>();
        made->path = path;
        made->maximum = std::clamp<qint64>(maximumBytes, 1024, qint64{1024} * 1024);
        made->beforeWriting = std::move(forTests.beforeWriting);
        const std::lock_guard lock(guard);
        if (current() != nullptr) {
            return;
        }
        if (forTests.beforeStarting) {
            forTests.beforeStarting();
        }
        // (Counted for the crash handler from its first line; the one before, if any, counts there no more.)
        atACrash.handed = 0;
        atACrash.done = 0;
        made->watched = true;
        std::thread thread([made] { made->run(); });
        // (Begun: nothing below throws.)
        atACrash.writer = thread.get_id();
        atACrash.on = true;
        current() = made.get();
        m_writer = std::move(made);
        m_thread = std::move(thread);
    } catch (...) {
        // (Before its thread was begun: nothing to end, and diagnostics are off.)
        m_writer.reset();
    }
}

VideoDiagnostics::~VideoDiagnostics()
{
    if (!m_writer) {
        return;
    }
    // (What can throw here is the system refusing a lock or a join: then the thread is left to end by itself, and
    // nothing leaves a destructor.)
    try {
        {
            const std::lock_guard lock(guard);
            if (current() == m_writer.get()) {
                current() = nullptr;
            }
        }
        std::unique_lock held(m_writer->lock);
        m_writer->stopping = true;
        m_writer->watched = false;
        atACrash.on = false;
        m_writer->wake.notify_all();
        const bool over = m_writer->written.wait_for(held, std::chrono::milliseconds(atTheEndMs),
                                                     [this] { return m_writer->finished; });
        held.unlock();
        if (over) {
            m_thread.join();
        } else {
            // (It holds what it writes to, and its file is its own: it ends by itself once the disk lets its write
            // return, and lets go of both there.)
            m_thread.detach();
        }
    } catch (...) {
        // (A thread that is joinable may not be destroyed: left to end by itself, as above.)
        try {
            if (m_thread.joinable()) {
                m_thread.detach();
            }
        } catch (...) {
            m_writer.reset();
        }
    }
}

bool VideoDiagnostics::isOpen() const
{
    if (!m_writer) {
        return false;
    }
    std::unique_lock held(m_writer->lock);
    m_writer->written.wait_for(held, std::chrono::milliseconds(atTheEndMs),
                               [this] { return m_writer->opened.has_value(); });
    return m_writer->open;
}

void VideoDiagnostics::flush() const
{
    if (!m_writer) {
        return;
    }
    std::unique_lock held(m_writer->lock);
    const quint64 handed = m_writer->handed;
    m_writer->written.wait(
        held, [this, handed] { return (m_writer->done >= handed && m_writer->dropped == 0) || m_writer->finished; });
}

void VideoDiagnostics::record(const char* event, std::initializer_list<std::pair<const char*, qint64>> fields)
{
    if (!nameIsSafe(event) || fields.size() > 32) {
        return;
    }
    QJsonObject values;
    for (const auto& [name, value] : fields) {
        if (!nameIsSafe(name)) {
            return;
        }
        values.insert(QString::fromLatin1(name), value);
    }
    const std::lock_guard lock(guard);
    Writer* const writer = current();
    if (writer == nullptr) {
        return;
    }
    const std::lock_guard held(writer->lock);
    if (writer->stopping || writer->finished) {
        return;
    }
    if (std::cmp_greater_equal(writer->waiting.size(), mostWaiting)) {
        ++writer->dropped;
        return;
    }
    // (Made under the lock the lines wait under: their times are in the order they wait, and so in the file.)
    QByteArray line = lineOf(event, values);
    if (line.size() > writer->maximum) {
        return;
    }
    writer->waiting.push_back(std::move(line));
    ++writer->handed;
    ++atACrash.handed;
    writer->wake.notify_one();
}

void VideoDiagnostics::writeWhatWaitsAtACrash() noexcept
{
    // (On the writer's own thread, it is the one that crashed: nothing writes what waits.)
    if (!atACrash.on || atACrash.writer.load() == std::this_thread::get_id()) {
        return;
    }
    const quint64 handed = atACrash.handed;
    try {
        for (int waited = 0; waited < atACrashMs; waited += 10) {
            if (!atACrash.on || atACrash.done >= handed) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    } catch (...) {
        // (Whatever a sleep may say: the crash goes on.)
        return;
    }
}

void VideoDiagnostics::Writer::run()
{
    // Its file is made, opened, written and let go of here, on this thread alone (a QObject of this thread's, from
    // the second reading of c7ec58c, L5: made on the interface thread, it was let go of on this one after a detach).
    // Whatever throws while a line is written is caught in loop(); what is left is the system refusing a lock, and
    // then this writer goes silent: nothing leaves this thread, which would end the program (L2).
    try {
        std::optional<QFile> file;
        bool ok = false;
        try {
            file.emplace(path);
            ok = QDir().mkpath(QFileInfo(path).absolutePath()) && file->open(QIODevice::WriteOnly | QIODevice::Append);
        } catch (...) {
            ok = false;
        }
        open = ok;
        {
            const std::lock_guard held(lock);
            opened = ok;
        }
        written.notify_all();
        if (ok) {
            loop(*file);
        }
    } catch (...) {
        // (Silent from here, and said closed.)
        open = false;
    }
    open = false;
    try {
        const std::lock_guard held(lock);
        finished = true;
        if (!opened) {
            opened = false;
        }
    } catch (...) {
        // (Nothing can be marked: its readers wait their bound at the most, the end two seconds, isOpen the same.)
        return;
    }
    written.notify_all();
}

void VideoDiagnostics::Writer::loop(QFile& file)
{
    for (;;) {
        QByteArray line;
        bool sayingDropped = false;
        {
            std::unique_lock held(lock);
            if (waiting.empty() && dropped > 0) {
                // Every line that waited before what was dropped is written: the count is said now, with this moment's
                // time, after theirs (from the second reading of c7ec58c, L4: said before the line that came first
                // to have room, it stood ahead of up to 1023 older lines with a later time). A count that cannot be
                // said is let go of.
                sayingDropped = true;
                try {
                    line = lineOf("diagnostics_dropped", QJsonObject{{u"count"_s, static_cast<qint64>(dropped)}});
                } catch (...) {
                    line.clear();
                }
                dropped = 0;
            } else {
                wake.wait(held, [this] { return stopping || !waiting.empty(); });
                if (waiting.empty()) {
                    return;
                }
                line = std::move(waiting.front());
                waiting.pop_front();
            }
        }
        bool wrote = true;
        try {
            if (!sayingDropped && beforeWriting) {
                beforeWriting();
            }
            write(file, line);
        } catch (...) {
            // (Memory run out, a file that throws, a test's disk: this line is let go of and counted, and the next
            // one is written as ever.)
            wrote = false;
        }
        {
            const std::lock_guard held(lock);
            if (!sayingDropped) {
                ++done;
                if (watched) {
                    ++atACrash.done;
                }
                if (!wrote) {
                    ++dropped;
                }
            }
        }
        written.notify_all();
    }
}

void VideoDiagnostics::Writer::write(QFile& file, const QByteArray& line)
{
    if (!file.isOpen() || line.isEmpty()) {
        return;
    }
    if (file.size() + line.size() > maximum) {
        const QString name = file.fileName();
        file.close();
        const QString previous = name + u".1"_s;
        if ((QFile::exists(previous) && !QFile::remove(previous)) || !QFile::rename(name, previous) ||
            !file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            open = false;
            return;
        }
    }
    if (file.write(line) != line.size() || !file.flush()) {
        file.close();
        open = false;
    }
}
} // namespace qkvm::core
