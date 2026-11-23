#include "rfb_by_hand.h"

#include <QByteArray>
#include <QRandomGenerator>
#include <QTest>
#include <qkvm/rfb/inflate.h>

#include <cstdint>
#include <span>
#include <vector>

// The zlib streams ZRLE and Tight carry their pixels in, read by the reader's own inflater: against Qt's own zlib
// (qCompress, which is zlib's deflate with every kind of block), and against the tests' own deflater, whose streams go
// on from part to part as a server's do.
using qkvm::rfb::Inflater;
using qkvm::rfb::tests::Bytes;
using qkvm::rfb::tests::Deflater;

namespace {

// A zlib stream of Qt's zlib (qCompress puts the length in front of it, four bytes).
Bytes zlibOf(const QByteArray& data, int level = 9)
{
    const QByteArray made = qCompress(data, level);
    return Bytes(made.begin() + 4, made.end());
}

QByteArray assorted(qsizetype size, quint32 seed)
{
    // Text, runs and noise, so that the deflater makes codes of every length, copies near and far.
    QRandomGenerator chance(seed);
    QByteArray made;
    while (made.size() < size) {
        switch (chance.bounded(4)) {
        case 0:
            made += "The quick brown fox jumps over the lazy dog, again and again. ";
            break;
        case 1:
            made += QByteArray(static_cast<qsizetype>(chance.bounded(300)), static_cast<char>(chance.bounded(256)));
            break;
        case 2:
            for (int one = 0; one < 64; ++one) {
                made += static_cast<char>(chance.bounded(256));
            }
            break;
        default:
            made += made.right(std::min<qsizetype>(made.size(), static_cast<qsizetype>(chance.bounded(2000))));
        }
    }
    made.truncate(size);
    return made;
}

QByteArray asQt(const Bytes& bytes)
{
    return QByteArray(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size()));
}

std::span<const std::uint8_t> spanOf(const QByteArray& bytes)
{
    return {reinterpret_cast<const std::uint8_t*>(bytes.data()), static_cast<std::size_t>(bytes.size())};
}

} // namespace

class TestInflate : public QObject {
    Q_OBJECT

private slots:
    void aStreamOfZlibsOwnInflatesToWhatItWas_data()
    {
        QTest::addColumn<QByteArray>("data");
        QTest::addColumn<int>("level");
        QTest::newRow("one byte") << QByteArray("x") << 9;
        QTest::newRow("text, the fixed codes") << QByteArray("hello hello hello hello") << 9;
        QTest::newRow("assorted, its own codes") << assorted(200'000, 1) << 9;
        QTest::newRow("assorted, stored") << assorted(150'000, 2) << 0;
        QTest::newRow("assorted, fast") << assorted(150'000, 3) << 1;
        QTest::newRow("one run, far copies") << QByteArray(300'000, 'a') << 9;
    }
    void aStreamOfZlibsOwnInflatesToWhatItWas()
    {
        QFETCH(QByteArray, data);
        QFETCH(int, level);
        const Bytes stream = zlibOf(data, level);
        Inflater inflater;
        Bytes out;
        const auto inflated = inflater.inflate(stream, static_cast<std::size_t>(data.size()), out);
        QVERIFY2(inflated.ok(), inflated.ok() ? "" : inflated.error().message.c_str());
        QCOMPARE(asQt(out), data);
        QVERIFY(inflater.ended());
    }

    void cutAnywhereAStreamInflatesTheSame()
    {
        // Taken up where it was left at any bit: byte by byte, and in pieces of every size.
        const QByteArray data = assorted(40'000, 7);
        const Bytes stream = zlibOf(data);
        for (const std::size_t piece :
             {std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{7}, std::size_t{100}, std::size_t{4093}}) {
            Inflater inflater;
            Bytes out;
            for (std::size_t at = 0; at < stream.size(); at += piece) {
                const std::size_t size = std::min(piece, stream.size() - at);
                const auto inflated =
                    inflater.inflate(std::span(stream).subspan(at, size), static_cast<std::size_t>(data.size()), out);
                QVERIFY2(inflated.ok(), qPrintable(QString::number(piece)));
            }
            QCOMPARE(asQt(out), data);
            QVERIFY(inflater.ended());
        }
    }

    void aStreamThatGoesOnIsTakenUpWhereItWasLeft()
    {
        // As a server's: parts one after the other, each ended by a flush, copies reaching into the parts before
        // (`Far`, whose copies are seen to: a reader that forgot the last 32 KiB at a part's end would read them wrong;
        // outside review 2.7).
        for (const Deflater::Way way : {Deflater::Way::Stored, Deflater::Way::Fixed, Deflater::Way::Far}) {
            Deflater deflater(way);
            Inflater inflater;
            for (int part = 0; part < 20; ++part) {
                const QByteArray data = assorted(1 + part * 997, static_cast<quint32>(part + 11));
                const Bytes said = deflater.part(spanOf(data));
                Bytes out;
                QVERIFY(inflater.inflate(said, static_cast<std::size_t>(data.size()), out).ok());
                QCOMPARE(asQt(out), data);
                QVERIFY(!inflater.ended());
            }
            if (way == Deflater::Way::Far) {
                // (Tens of copies began in a part before their own, some of them a kilobyte back and more: 48 of them
                // on 2026-11-18, of these parts.)
                QVERIFY2(deflater.copiesFromPartsBefore() > 20,
                         qPrintable(QString::number(deflater.copiesFromPartsBefore())));
                QVERIFY2(deflater.farthestCopy() > 1024, qPrintable(QString::number(deflater.farthestCopy())));
            }
            // An empty part is a flush alone.
            Bytes out;
            QVERIFY(inflater.inflate(deflater.part({}), 0, out).ok());
            QVERIFY(out.empty());
            // Ended, and nothing after.
            QVERIFY(inflater.inflate(deflater.end(), 0, out).ok());
            QVERIFY(inflater.ended());
            const auto after = inflater.inflate(Bytes{}, 10, out);
            QVERIFY(!after.ok());
            QCOMPARE(after.error().code, std::string("ended"));
        }
    }

    void moreThanThereIsRoomForIsRefused()
    {
        const QByteArray data = assorted(10'000, 5);
        for (const int level : {0, 9}) {
            const Bytes stream = zlibOf(data, level);
            Inflater inflater;
            Bytes out;
            const auto inflated = inflater.inflate(stream, static_cast<std::size_t>(data.size() - 1), out);
            QVERIFY(!inflated.ok());
            QCOMPARE(inflated.error().code, std::string("too_long"));
            QVERIFY(out.size() <= static_cast<std::size_t>(data.size() - 1));
            // ...and the stream is not read on.
            QVERIFY(!inflater.inflate(stream, 1'000'000, out).ok());
        }
    }

    void whatIsNoZlibStreamIsRefused_data()
    {
        QTest::addColumn<QByteArray>("stream");
        QTest::addColumn<QByteArray>("code");
        const Bytes good = zlibOf("some text, some text, some text");
        QByteArray damaged = asQt(good);
        QTest::newRow("not deflate") << QByteArray("\x79\x9C", 2) << QByteArray("garbled");
        QTest::newRow("a header that does not check") << QByteArray("\x78\x02", 2) << QByteArray("garbled");
        QTest::newRow("a dictionary") << QByteArray("\x78\xBB", 2) << QByteArray("garbled");
        QTest::newRow("a block of a fourth kind") << QByteArray("\x78\x01\x07", 3) << QByteArray("garbled");
        QTest::newRow("a stored length that is none")
            << QByteArray("\x78\x01\x00\x05\x00\x00\x00", 7) << QByteArray("garbled");
        // A fixed block that copies from 1 byte back before anything came: literal-less length 3 (symbol 257, code
        // 0000001), distance symbol 0.
        QTest::newRow("a copy from before the start") << QByteArray("\x78\x01\x03\x02\x00", 5) << QByteArray("garbled");
        damaged[damaged.size() - 1] = static_cast<char>(damaged.back() ^ 1);
        QTest::newRow("a checksum that is not its own") << damaged << QByteArray("garbled");
        QTest::newRow("bytes after its end") << asQt(good) + QByteArray("x") << QByteArray("ended");
    }
    void whatIsNoZlibStreamIsRefused()
    {
        QFETCH(QByteArray, stream);
        QFETCH(QByteArray, code);
        Inflater inflater;
        Bytes out;
        const auto inflated = inflater.inflate(spanOf(stream), 1000, out);
        QVERIFY(!inflated.ok());
        QVERIFY2(QByteArray::fromStdString(inflated.error().code) == code, inflated.error().message.c_str());
    }

    void aDamagedStreamNeverRunsPastItsRoom()
    {
        // Streams damaged every which way, the same at every run: whatever they come to, never more than the room
        // given, and nothing falls over.
        QRandomGenerator chance(42);
        const QByteArray data = assorted(20'000, 9);
        const Bytes good = zlibOf(data);
        int refused = 0;
        for (int round = 0; round < 400; ++round) {
            Bytes stream = good;
            const int damages = 1 + static_cast<int>(chance.bounded(8));
            for (int one = 0; one < damages; ++one) {
                stream[chance.bounded(static_cast<quint32>(stream.size()))] ^=
                    static_cast<std::uint8_t>(1U << chance.bounded(8));
            }
            Inflater inflater;
            Bytes out;
            const std::size_t room = 1 + chance.bounded(30'000U);
            const auto inflated = inflater.inflate(stream, room, out);
            QVERIFY(out.size() <= room);
            refused += inflated.ok() ? 0 : 1;
        }
        QVERIFY(refused > 300); // (most damage is seen; a damaged stored length can go unseen, and the room holds it)
    }

    void emptyDynamicBlocksInflateToNothingAndCountTheirTableWork()
    {
        // The review's attack (finding 1): dynamic blocks with only an end-of-block code, each ~12 bytes, each making
        // the reader build a 32,768-entry table. They inflate to nothing, and are well formed deflate.
        Deflater deflater;
        const Bytes stream = deflater.emptyDynamicStream(200);
        Inflater inflater;
        Bytes out;
        const auto inflated = inflater.inflate(stream, 1'000'000, out);
        QVERIFY2(inflated.ok(), inflated.ok() ? "" : inflated.error().message.c_str());
        QVERIFY(out.empty());
        QVERIFY(inflater.ended());
        // Each block's table building is counted: ~16,384 slots a block (one one-bit literal code), so far more than
        // nothing, for the caller to hold to a server's work.
        QVERIFY2(inflater.tableWorkTaken() > std::uint64_t{200} * 8000,
                 qPrintable(QString::number(inflater.tableWorkTaken())));
        QCOMPARE(inflater.tableWorkTaken(), std::uint64_t{0}); // (taken once, then reset)
    }

    void aStreamThatCostsTooMuchTimeIsCutOff()
    {
        // A run of tiny dynamic blocks: with a clock that says time has passed, the inflater is cut off part way (it
        // asks before every dynamic block's tables), rather than building tables for all of them.
        Deflater deflater;
        const Bytes stream = deflater.emptyDynamicStream(8000);
        Inflater inflater;
        Bytes out;
        int asked = 0;
        Inflater::Bounds bounds;
        bounds.overBudget = [&asked] { return ++asked >= 2; };
        const auto inflated = inflater.inflate(stream, 1'000'000, out, bounds);
        QVERIFY(!inflated.ok());
        QCOMPARE(inflated.error().code, std::string("too_slow"));
        QCOMPARE(inflater.blocksTaken(), std::uint64_t{2}); // (cut off at the second block's asking)
        // And the stream is not read on after that.
        QVERIFY(!inflater.inflate(stream, 1'000'000, out).ok());
    }

    void theCallerIsAskedBeforeTheTablesOfEveryDynamicBlock()
    {
        // Outside review 2.1: before every dynamic block's tables are built (each costs three tables 32,768 entries
        // wide), not every 1024 blocks. Told "no more" at the first asking, nothing of the first block's tables is
        // built; asked of a hundred blocks, it is asked a hundred times.
        Deflater deflater;
        const Bytes stream = deflater.emptyDynamicStream(100);
        {
            Inflater inflater;
            Bytes out;
            Inflater::Bounds bounds;
            bounds.overBudget = [] { return true; };
            const auto inflated = inflater.inflate(stream, 1000, out, bounds);
            QCOMPARE(inflated.ok() ? std::string("ok") : inflated.error().code, std::string("too_slow"));
            QCOMPARE(inflater.tableWorkTaken(), std::uint64_t{0});
        }
        Inflater inflater;
        Bytes out;
        int asked = 0;
        Inflater::Bounds bounds;
        bounds.overBudget = [&asked] {
            ++asked;
            return false;
        };
        QVERIFY(inflater.inflate(stream, 1000, out, bounds).ok());
        QCOMPARE(asked, 100);
    }

    void theCallerIsAskedEverySoManyOtherBlocksAndCodes()
    {
        // ...and every 64 of the other blocks (cheap ones, but a stream of them may be long), and every 65,536 codes of
        // a block (what one inflates to is bounded, and its time with that, but a block may hold all of it).
        Deflater stored(Deflater::Way::Stored);
        Bytes blocks = stored.part({}); // the header, and one empty stored block
        const Bytes more = Deflater::emptyStoredBlocks(1279);
        blocks.insert(blocks.end(), more.begin(), more.end());
        int asked = 0;
        Inflater::Bounds bounds;
        bounds.overBudget = [&asked] {
            ++asked;
            return false;
        };
        {
            Inflater inflater;
            Bytes out;
            QVERIFY(inflater.inflate(blocks, 1000, out, bounds).ok());
            QCOMPARE(asked, 1280 / 64);
        }
        QByteArray noise(300'000, Qt::Uninitialized);
        QRandomGenerator chance(3);
        for (char& one : noise) {
            one = static_cast<char>(chance.bounded(256));
        }
        Deflater fixed;
        const Bytes codes = fixed.part(spanOf(noise)); // one block of the fixed codes, about 300,000 of them
        asked = 0;
        Inflater inflater;
        Bytes out;
        QVERIFY(inflater.inflate(codes, static_cast<std::size_t>(noise.size()), out, bounds).ok());
        QCOMPARE(asQt(out), noise);
        QVERIFY2(asked >= 4, qPrintable(QString::number(asked)));
    }

    void aReadsTableWorkIsHeldToItsBoundBeforeTheTablesAreBuilt()
    {
        // Outside review 2.1: a table's slots are counted before it is built, and one that would take the read past
        // its bound is not built: the read ends there ("too_costly"), its work no more than its bound. (Counted after
        // the read, a stream of tiny dynamic blocks built every table it had before it was refused.)
        Deflater deflater;
        const Bytes stream = deflater.emptyDynamicStream(8000);
        Inflater inflater;
        Bytes out;
        Inflater::Bounds bounds;
        bounds.mostTableWork = 1'000'000;
        const auto inflated = inflater.inflate(stream, 1000, out, bounds);
        QCOMPARE(inflated.ok() ? std::string("ok") : inflated.error().code, std::string("too_costly"));
        const std::uint64_t work = inflater.tableWorkTaken();
        QVERIFY2(work <= 1'000'000 && work > 800'000, qPrintable(QString::number(work)));
        QVERIFY(inflater.blocksTaken() < 10);
    }

    void aReadsBlocksAreHeldToTheirBound()
    {
        // ...and so are its blocks, counted as each begins.
        Deflater deflater;
        const Bytes stream = deflater.emptyDynamicStream(8000);
        Inflater inflater;
        Bytes out;
        Inflater::Bounds bounds;
        bounds.mostBlocks = 100;
        const auto inflated = inflater.inflate(stream, 1000, out, bounds);
        QCOMPARE(inflated.ok() ? std::string("ok") : inflated.error().code, std::string("too_costly"));
        QCOMPARE(inflater.blocksTaken(), std::uint64_t{100});
        // (A read's bounds are its own: the next read of another stream counts from nothing.)
        Inflater next;
        const Bytes fits = deflater.emptyDynamicStream(100);
        QVERIFY(next.inflate(fits, 1000, out, bounds).ok());
        QCOMPARE(next.blocksTaken(), std::uint64_t{100});
    }
};

QTEST_GUILESS_MAIN(TestInflate)
#include "test_inflate.moc"
