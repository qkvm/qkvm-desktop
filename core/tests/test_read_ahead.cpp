#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <qkvm/core/read_ahead.h>

using namespace Qt::StringLiterals;
using namespace qkvm;

namespace {

// The thread the tests run on: the program's own.
Qt::HANDLE theTestsThread = nullptr;

// The byte at a place of a file of the pattern these tests read.
char byteAt(qint64 at)
{
    return static_cast<char>((at * 7 + 3) & 0xFF);
}

// A gate a read waits at, from another thread, until it is let go.
struct Gate {
    std::mutex lock;
    std::condition_variable opened;
    bool open = false;

    void letGo()
    {
        {
            const std::lock_guard held(lock);
            open = true;
        }
        opened.notify_all();
    }
    void waitHere()
    {
        std::unique_lock held(lock);
        opened.wait(held, [this] { return open; });
    }
};

// What the device saw: how much of it was read, on which thread it was made, read and ended.
struct Seen {
    std::atomic<qint64> read{0};
    std::atomic<bool> ended{false};
    std::atomic<bool> readHere{false};
    std::atomic<bool> endedHere{false};
    Qt::HANDLE made = nullptr;
};

// A file of the pattern, this long, read at a place; whose reads wait at a gate from a place on, where one is given.
class Patterned final : public QIODevice {
public:
    Patterned(qint64 size, std::shared_ptr<Seen> seen, qint64 stopAt = -1, std::shared_ptr<Gate> gate = {})
        : m_size(size), m_seen(std::move(seen)), m_stopAt(stopAt), m_gate(std::move(gate))
    {
        m_seen->made = QThread::currentThreadId();
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    }
    ~Patterned() override
    {
        m_seen->endedHere = QThread::currentThreadId() == theTestsThread;
        m_seen->ended = true;
    }
    qint64 size() const override { return m_size; }
    bool isSequential() const override { return false; }

protected:
    qint64 readData(char* data, qint64 most) override
    {
        if (QThread::currentThreadId() == theTestsThread) {
            m_seen->readHere = true;
        }
        if (m_gate && m_stopAt >= 0 && pos() + most > m_stopAt) {
            m_gate->waitHere();
        }
        const qint64 some = std::min(most, m_size - pos());
        for (qint64 at = 0; at < some; ++at) {
            data[at] = byteAt(pos() + at);
        }
        m_seen->read += some;
        return some;
    }
    qint64 writeData(const char* /*data*/, qint64 /*length*/) override { return -1; }

private:
    qint64 m_size;
    std::shared_ptr<Seen> m_seen;
    qint64 m_stopAt;
    std::shared_ptr<Gate> m_gate;
};

// Opens one, and waits for it as the program's loop waits.
base::Result<std::unique_ptr<core::ReadAhead>> opened(core::ReadAhead::Source source, QObject* context,
                                                      qint64 ahead = core::ReadAhead::aheadBytes)
{
    bool came = false;
    base::Result<std::unique_ptr<core::ReadAhead>> got = base::Error{"never", "Nothing came."};
    core::ReadAhead::open(
        std::move(source), context,
        [&](base::Result<std::unique_ptr<core::ReadAhead>> said) {
            got = std::move(said);
            came = true;
        },
        ahead);
    static_cast<void>(QTest::qWaitFor([&] { return came; }, 10000));
    return got;
}

// All of it, read as Qt reads a file it sends: what there is, and again once it says more came.
QByteArray readAll(core::ReadAhead& reader, int withinMs = 20000)
{
    QByteArray all;
    QElapsedTimer clock;
    clock.start();
    char piece[16384];
    while (!reader.atEnd() && clock.elapsed() < withinMs) {
        const qint64 got = reader.read(piece, sizeof(piece));
        if (got < 0) {
            break;
        }
        if (got == 0) {
            QTest::qWait(1);
            continue;
        }
        all.append(piece, static_cast<qsizetype>(got));
    }
    return all;
}

bool isThePattern(const QByteArray& bytes, qint64 from = 0)
{
    for (qsizetype at = 0; at < bytes.size(); ++at) {
        if (bytes[at] != byteAt(from + at)) {
            return false;
        }
    }
    return true;
}

} // namespace

// A file read on a thread of its own for Qt to send (core::ReadAhead): opened, measured and read there, never on the
// thread that reads this device; no more of it held than it may read ahead; read again from where it is moved to; and
// let go of, at its end, without waiting for a read that does not come back.
class TestReadAhead : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { theTestsThread = QThread::currentThreadId(); }

    void theFileIsOpenedAndReadOnAThreadOfItsOwnAndComesWholeAndInOrder()
    {
        QObject asker;
        const auto seen = std::make_shared<Seen>();
        auto reader = opened(
            [seen] { return base::Result<std::unique_ptr<QIODevice>>(std::make_unique<Patterned>(3'000'001, seen)); },
            &asker);
        QVERIFY2(reader.ok(), reader.ok() ? "" : reader.error().message.c_str());
        QCOMPARE(reader.value()->size(), qint64{3'000'001});
        QVERIFY(seen->made != QThread::currentThreadId());
        const QByteArray all = readAll(*reader.value());
        QCOMPARE(all.size(), qsizetype{3'000'001});
        QVERIFY(isThePattern(all));
        QVERIFY(!seen->readHere);
        QVERIFY(reader.value()->failure().isEmpty());
        reader.value().reset();
        QTRY_VERIFY(seen->ended);
        QVERIFY(!seen->endedHere);
    }

    void noMoreIsReadAheadThanItMay()
    {
        QObject asker;
        const auto seen = std::make_shared<Seen>();
        constexpr qint64 ahead = qint64{1} << 20;
        auto reader = opened(
            [seen] { return base::Result<std::unique_ptr<QIODevice>>(std::make_unique<Patterned>(40'000'000, seen)); },
            &asker, ahead);
        QVERIFY(reader.ok());
        QTRY_VERIFY(seen->read >= ahead);
        QTest::qWait(300);
        QVERIFY2(seen->read <= ahead + core::ReadAhead::pieceBytes, qPrintable(QString::number(seen->read)));
        // Some taken: as much more is read, and no more.
        char piece[100'000];
        QCOMPARE(reader.value()->read(piece, sizeof(piece)), qint64{sizeof(piece)});
        QVERIFY(isThePattern(QByteArray(piece, sizeof(piece))));
        QTest::qWait(300);
        QVERIFY2(seen->read <= ahead + core::ReadAhead::pieceBytes + qint64{sizeof(piece)},
                 qPrintable(QString::number(seen->read)));
    }

    void itIsReadAgainFromWhereItIsMovedTo()
    {
        QObject asker;
        const auto seen = std::make_shared<Seen>();
        auto reader = opened(
            [seen] { return base::Result<std::unique_ptr<QIODevice>>(std::make_unique<Patterned>(2'000'000, seen)); },
            &asker);
        QVERIFY(reader.ok());
        core::ReadAhead& file = *reader.value();
        QTRY_VERIFY(seen->read > 0);
        char piece[5000];
        QCOMPARE(file.read(piece, sizeof(piece)), qint64{sizeof(piece)});
        QVERIFY(file.seek(123));
        QCOMPARE(file.pos(), qint64{123});
        const QByteArray rest = readAll(file);
        QCOMPARE(rest.size(), qsizetype{2'000'000 - 123});
        QVERIFY(isThePattern(rest, 123));
        // And back to the beginning, as Qt moves a file it sends again.
        QVERIFY(file.seek(0));
        QCOMPARE(readAll(file).size(), qsizetype{2'000'000});
    }

    void whatCannotBeOpenedIsSaidAndIsNoDevice()
    {
        QObject asker;
        const QTemporaryDir folder;
        auto none = opened(core::ReadAhead::fileAt(folder.filePath(u"not there.iso"_s)), &asker);
        QVERIFY(!none.ok());
        QCOMPARE(none.error().code, std::string("unreadable"));
        QVERIFY(!none.error().message.empty());
        // Nor whoever asked, gone meanwhile: nobody is told.
        const auto seen = std::make_shared<Seen>();
        bool told = false;
        {
            QObject gone;
            core::ReadAhead::open(
                [seen] { return base::Result<std::unique_ptr<QIODevice>>(std::make_unique<Patterned>(1000, seen)); },
                &gone, [&](auto) { told = true; });
        }
        QTRY_VERIFY(seen->ended);
        QVERIFY(!told);
    }

    void itsEndWaitsForNoReadThatDoesNotComeBack()
    {
        // The share stops answering: a read stands still there. The device ends at once, and the file is let go of
        // on the thread that reads it, once that read comes back.
        QObject asker;
        const auto seen = std::make_shared<Seen>();
        const auto gate = std::make_shared<Gate>();
        auto reader = opened(
            [seen, gate] {
                return base::Result<std::unique_ptr<QIODevice>>(
                    std::make_unique<Patterned>(4'000'000, seen, 600'000, gate));
            },
            &asker);
        QVERIFY(reader.ok());
        core::ReadAhead& file = *reader.value();
        // (Two pieces come, and the third waits at the gate.)
        QTRY_VERIFY(seen->read >= 2 * core::ReadAhead::pieceBytes);
        const QByteArray some = readAll(file, 1500);
        QCOMPARE(some.size(), qsizetype{2 * core::ReadAhead::pieceBytes});
        QVERIFY(isThePattern(some));
        QVERIFY(file.waitingForTheFile());
        QElapsedTimer clock;
        clock.start();
        reader.value().reset();
        QVERIFY2(clock.elapsed() < 100, qPrintable(QString::number(clock.elapsed())));
        QTest::qWait(200);
        QVERIFY(!seen->ended);
        gate->letGo();
        QTRY_VERIFY(seen->ended);
        QVERIFY(!seen->endedHere);
    }
};

QTEST_GUILESS_MAIN(TestReadAhead)
#include "test_read_ahead.moc"
