#include <qkvm/core/read_ahead.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <system_error>
#include <thread>
#include <utility>

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QPointer>
#include <QThread>

using namespace Qt::StringLiterals;

namespace qkvm::core {

// What the device and the thread that reads its file share, under its lock.
struct ReadAhead::Shared {
    std::mutex lock;
    // The reading waits on this for room, for a place to begin again at, or to stop.
    std::condition_variable room;
    qint64 ahead = aheadBytes;
    qint64 size = 0;
    // What was read and not taken yet, in its order, and how much of the first piece was taken.
    std::deque<QByteArray> pieces;
    qsizetype firstTaken = 0;
    qint64 held = 0;
    // Where the reading is to begin again (the device was moved there), and a count of such places: a piece read for
    // an earlier one is nobody's.
    std::optional<qint64> from;
    quint64 turn = 0;
    // The file was read to its length; or a read of it failed, and why.
    bool ended = false;
    QString failed;
    // The device is gone: the reading ends.
    bool stopped = false;
    // The device, while it stands, which is told when something comes after a read of it was given nothing (once
    // until that telling has come).
    ReadAhead* reader = nullptr;
    bool waiting = false;
    bool telling = false;
};

namespace {

// Said the way the program says that a file could not be read, with the device's own words where it has some.
QString whyNot(const QIODevice& device, const QString& otherwise)
{
    const QString said = device.errorString();
    return said.isEmpty() || said == "Unknown error"_L1 ? otherwise : said;
}

} // namespace

ReadAhead::Source ReadAhead::fileAt(const QString& path)
{
    return [path]() -> base::Result<std::unique_ptr<QIODevice>> {
        auto file = std::make_unique<QFile>(path);
        if (!file->open(QIODevice::ReadOnly | QIODevice::Unbuffered)) {
            return base::Error{"unreadable", file->errorString().toStdString()};
        }
        return std::unique_ptr<QIODevice>(std::move(file));
    };
}

ReadAhead::ReadAhead(std::shared_ptr<Shared> shared, qint64 size) : m_shared(std::move(shared)), m_size(size)
{
    QIODevice::open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    const std::lock_guard held(m_shared->lock);
    m_shared->reader = this;
}

ReadAhead::~ReadAhead()
{
    // The reading is let go of, and not waited for: it may be in a read of a share that does not answer.
    {
        const std::lock_guard held(m_shared->lock);
        m_shared->stopped = true;
        m_shared->reader = nullptr;
        m_shared->pieces.clear();
        m_shared->held = 0;
    }
    m_shared->room.notify_all();
}

// Told on the device's thread, where it still stands, that something came after a read of it was given nothing, or that
// the file ended or could not be read. Under the shared lock, which the device's end takes too: it is not ended while
// this is asked, and what was asked is not delivered after its end (Qt takes it back then).
void ReadAhead::tellTheReader(const std::shared_ptr<Shared>& shared)
{
    ReadAhead* reader = shared->reader;
    if (reader == nullptr || !shared->waiting || shared->telling) {
        return;
    }
    shared->telling = true;
    QMetaObject::invokeMethod(
        reader,
        [reader, shared] {
            bool over = false;
            {
                const std::lock_guard held(shared->lock);
                shared->telling = false;
                shared->waiting = false;
                over = shared->pieces.empty() && (shared->ended || !shared->failed.isEmpty());
            }
            emit reader->readyRead();
            if (over) {
                emit reader->readChannelFinished();
            }
        },
        Qt::QueuedConnection);
}

// The reading, on its own thread: from `next` on, a piece at a time while there is room, until the file is read to its
// length, a read fails, or the device is gone. The file is let go of here, on this thread.
void ReadAhead::readOn(const std::shared_ptr<Shared>& shared, std::unique_ptr<QIODevice> device, qint64 next)
{
    bool moved = false;
    for (;;) {
        quint64 turn = 0;
        qint64 want = 0;
        {
            std::unique_lock held(shared->lock);
            shared->room.wait(held, [&] {
                return shared->stopped || shared->from.has_value() ||
                       (!shared->ended && shared->failed.isEmpty() && shared->held < shared->ahead);
            });
            if (shared->stopped) {
                break;
            }
            if (shared->from) {
                next = *std::exchange(shared->from, std::nullopt);
                moved = true;
            }
            if (!shared->failed.isEmpty()) {
                // (Moved after a read failed: that stands. Nothing more is read.)
                continue;
            }
            turn = shared->turn;
            want = std::min(pieceBytes, shared->size - next);
            if (want <= 0) {
                shared->ended = true;
                tellTheReader(shared);
                continue;
            }
        }
        QString failed;
        QByteArray piece;
        if (moved && !device->seek(next)) {
            failed = whyNot(*device, u"It could not be read from where it was to be read again."_s);
        } else {
            moved = false;
            piece = QByteArray(static_cast<qsizetype>(want), Qt::Uninitialized);
            const qint64 got = device->read(piece.data(), want);
            if (got < 0) {
                failed = whyNot(*device, u"It could not be read."_s);
            } else if (got == 0) {
                failed = u"It ended before its length: it was changed while it was read."_s;
            } else {
                piece.truncate(static_cast<qsizetype>(got));
            }
        }
        const std::lock_guard held(shared->lock);
        if (shared->stopped) {
            break;
        }
        if (shared->turn != turn) {
            continue; // (moved meanwhile: read again from there)
        }
        if (!failed.isEmpty()) {
            shared->failed = failed;
        } else {
            next += piece.size();
            shared->held += piece.size();
            shared->pieces.push_back(std::move(piece));
            shared->ended = next >= shared->size;
        }
        tellTheReader(shared);
    }
    device.reset();
}

void ReadAhead::open(Source source, QObject* context,
                     std::function<void(base::Result<std::unique_ptr<ReadAhead>>)> opened, qint64 ahead)
{
    auto shared = std::make_shared<Shared>();
    shared->ahead = std::max<qint64>(ahead, pieceBytes);
    const QPointer<QObject> asker(context);
    // (Held once, for the reading's thread, or for saying that none could be begun.)
    const auto told =
        std::make_shared<std::function<void(base::Result<std::unique_ptr<ReadAhead>>)>>(std::move(opened));
    // Told on the program's own thread (its QCoreApplication's), which is where this is asked.
    const auto tell = [](auto what) {
        if (QCoreApplication* program = QCoreApplication::instance(); program != nullptr) {
            QMetaObject::invokeMethod(program, std::move(what), Qt::QueuedConnection);
        }
    };
    try {
        std::thread([shared, source = std::move(source), asker, told, tell] {
            base::Result<std::unique_ptr<QIODevice>> made =
                source ? source()
                       : base::Result<std::unique_ptr<QIODevice>>(base::Error{"unreadable", "Nothing to read."});
            std::unique_ptr<QIODevice> device;
            std::optional<base::Error> why;
            if (!made) {
                why = made.error();
            } else if (device = std::move(made).value();
                       device == nullptr || !device->isOpen() || !device->isReadable() || device->isSequential()) {
                why = base::Error{"unreadable", "It is no file that is read at a place."};
            } else if (const qint64 size = device->size(); size < 0) {
                why = base::Error{"unreadable", whyNot(*device, u"Its length is not known."_s).toStdString()};
            } else {
                shared->size = size;
            }
            if (why) {
                device.reset();
                tell([asker, told, why = *why] {
                    if (asker != nullptr) {
                        (*told)(why);
                    }
                });
                return;
            }
            tell([shared, asker, told, size = shared->size] {
                if (asker == nullptr) {
                    {
                        const std::lock_guard held(shared->lock);
                        shared->stopped = true;
                    }
                    shared->room.notify_all();
                    return;
                }
                (*told)(std::unique_ptr<ReadAhead>(new ReadAhead(shared, size)));
            });
            readOn(shared, std::move(device), 0);
        }).detach();
    } catch (const std::system_error&) {
        tell([asker, told] {
            if (asker != nullptr) {
                (*told)(base::Error{"unreadable", "No thread could be begun to read it on."});
            }
        });
    }
}

std::unique_ptr<ReadAhead> ReadAhead::of(std::unique_ptr<QIODevice> device, qint64 ahead)
{
    if (device == nullptr || device->parent() != nullptr || !device->isOpen() || !device->isReadable() ||
        device->isSequential() || device->size() < 0) {
        return nullptr;
    }
    auto shared = std::make_shared<Shared>();
    shared->ahead = std::max<qint64>(ahead, pieceBytes);
    shared->size = device->size();
    const qint64 from = std::clamp<qint64>(device->pos(), 0, shared->size);
    std::unique_ptr<ReadAhead> reader(new ReadAhead(shared, shared->size));
    reader->QIODevice::seek(from);
    // (Pulled into the reading's own thread there: an object of no thread may be.)
    device->moveToThread(nullptr);
    try {
        std::thread([shared, device = std::move(device), from]() mutable {
            device->moveToThread(QThread::currentThread());
            readOn(shared, std::move(device), from);
        }).detach();
    } catch (const std::system_error&) {
        const std::lock_guard held(shared->lock);
        shared->failed = u"No thread could be begun to read it on."_s;
    }
    return reader;
}

bool ReadAhead::seek(qint64 pos)
{
    if (pos < 0 || pos > m_size) {
        return false;
    }
    if (pos == this->pos()) {
        return QIODevice::seek(pos);
    }
    if (!QIODevice::seek(pos)) {
        return false;
    }
    {
        const std::lock_guard held(m_shared->lock);
        m_shared->pieces.clear();
        m_shared->firstTaken = 0;
        m_shared->held = 0;
        m_shared->from = pos;
        ++m_shared->turn;
        m_shared->ended = pos >= m_size;
    }
    m_shared->room.notify_all();
    return true;
}

QString ReadAhead::failure() const
{
    const std::lock_guard held(m_shared->lock);
    return m_shared->failed;
}

bool ReadAhead::waitingForTheFile() const
{
    const std::lock_guard held(m_shared->lock);
    return m_shared->waiting && m_shared->pieces.empty() && !m_shared->ended && m_shared->failed.isEmpty();
}

qint64 ReadAhead::readData(char* data, qint64 most)
{
    qint64 taken = 0;
    QString failed;
    {
        const std::lock_guard held(m_shared->lock);
        Shared& shared = *m_shared;
        while (taken < most && !shared.pieces.empty()) {
            const QByteArray& first = shared.pieces.front();
            const qint64 some = std::min<qint64>(most - taken, first.size() - shared.firstTaken);
            std::memcpy(data + taken, first.constData() + shared.firstTaken, static_cast<std::size_t>(some));
            taken += some;
            shared.firstTaken += static_cast<qsizetype>(some);
            if (shared.firstTaken == first.size()) {
                shared.pieces.pop_front();
                shared.firstTaken = 0;
            }
        }
        shared.held -= taken;
        if (taken == 0) {
            if (!shared.failed.isEmpty()) {
                failed = shared.failed;
            } else if (!shared.ended) {
                // Nothing yet: told when something comes.
                shared.waiting = true;
            }
        }
    }
    if (taken > 0) {
        m_shared->room.notify_all();
        return taken;
    }
    if (!failed.isEmpty()) {
        setErrorString(failed);
        return -1;
    }
    return 0;
}

qint64 ReadAhead::writeData(const char* /*data*/, qint64 /*length*/)
{
    return -1;
}

} // namespace qkvm::core
