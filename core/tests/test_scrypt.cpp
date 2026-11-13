#include <algorithm>
#include <chrono>
#include <cstddef>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <QElapsedTimer>
#include <QTest>

#include <qkvm/core/scrypt.h>

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::scrypt::Cost;

namespace {

// Memory for a test: it counts what is asked of it, has some or has none, and looks at what
// it is given back.
class WatchedMemory : public core::scrypt::Memory {
public:
    explicit WatchedMemory(bool hasSome) : m_hasSome(hasSome) {}

    void* take(std::size_t bytes) override
    {
        ++m_taken;
        m_bytes = bytes;
        if (!m_hasSome) {
            return nullptr;
        }
        m_block.assign(bytes / sizeof(quint64) + 1, 0);
        return m_block.data();
    }

    void give(void* block, std::size_t bytes) override
    {
        ++m_given;
        m_givenWhatWasTaken = block == m_block.data() && bytes == m_bytes;
        const auto* first = static_cast<const quint8*>(block);
        m_wipedWhenGiven = std::all_of(first, first + bytes, [](quint8 byte) { return byte == 0; });
    }

    int taken() const { return m_taken; }
    int given() const { return m_given; }
    std::size_t bytesTaken() const { return m_bytes; }
    bool givenWhatWasTaken() const { return m_givenWhatWasTaken; }
    bool wipedWhenGiven() const { return m_wipedWhenGiven; }

private:
    bool m_hasSome;
    int m_taken = 0;
    int m_given = 0;
    std::size_t m_bytes = 0;
    bool m_givenWhatWasTaken = false;
    bool m_wipedWhenGiven = false;
    std::vector<quint64> m_block;
};

QByteArray keyOf(QByteArrayView passphrase, QByteArrayView salt, const Cost& cost, qsizetype length)
{
    QByteArray key(length, '\0');
    const base::Result<void> made = core::scrypt::derive(
        passphrase, salt, cost,
        std::span<quint8>(reinterpret_cast<quint8*>(key.data()), static_cast<std::size_t>(length)));
    return made ? key : QByteArray("refused: ") + made.error().code.c_str();
}

// The block the RFC mixes in its sections 9 and 10: two blocks of 64 bytes.
QByteArray theRfcsBlock()
{
    return QByteArray::fromHex("f7ce0b653d2d72a4108cf5abe912ffdd777616dbbb27a70e8204f3ae2d0f6fad"
                               "89f68f4811d1e87bcc3bd7400a9ffd29094f0184639574f39ae5a1315217bcd7"
                               "894991447213bb226c25b54da86370fbcd984380374666bb8ffcb5bf40c254b0"
                               "67d27c51ce4ad5fed829c90b505a571b7f4d1cad6a523cda770e67bceaaf7e89");
}

} // namespace

// scrypt as it is written here, against the examples RFC 7914 gives of each of its parts and of
// the whole (written into this file from memory, and each seen to be what this code makes, which
// also opens the files a QKVM server's own scrypt sealed: core/tests/test_exchange.cpp); and what
// it does before it asks for memory, and with the memory it was given.
class TestScrypt : public QObject {
    Q_OBJECT

private slots:
    void theSalsa20CoreOfEightRoundsGivesTheRfcsAnswer()
    {
        // RFC 7914, section 8.
        const QByteArray in = QByteArray::fromHex("7e879a214f3ec9867ca940e641718f26baee555b8c61c1b50df846116dcd3b1d"
                                                  "ee24f319df9b3d8514121e4b5ac5aa3276021d2909c74829edebc68db8b8c25e");
        const QByteArray out = QByteArray::fromHex("a41f859c6608cc993b81cacb020cef05044b2181a2fd337dfd7b1c6396682f29"
                                                   "b4393168e3c9e6bcfe6bc5b7a06d96bae424cc102c91745c24ad673dc7618f81");
        QCOMPARE(core::scrypt::parts::salsa20Core8(in).toHex(), out.toHex());
        // And nothing for what is no block.
        QCOMPARE(core::scrypt::parts::salsa20Core8(in.left(63)), QByteArray());
    }

    void blockMixGivesTheRfcsAnswer()
    {
        // RFC 7914, section 9: r = 1.
        const QByteArray out = QByteArray::fromHex("a41f859c6608cc993b81cacb020cef05044b2181a2fd337dfd7b1c6396682f29"
                                                   "b4393168e3c9e6bcfe6bc5b7a06d96bae424cc102c91745c24ad673dc7618f81"
                                                   "20edc975323881a80540f64c162dcd3c21077cfe5f8d5fe2b1a4168f953678b7"
                                                   "7d3b3d803b60e4ab920996e59b4d53b65d2a225877d5edf5842cb9f14eefe425");
        QCOMPARE(core::scrypt::parts::blockMix(theRfcsBlock(), 1).toHex(), out.toHex());
    }

    void roMixGivesTheRfcsAnswer()
    {
        // RFC 7914, section 10: N = 16, r = 1.
        const QByteArray out = QByteArray::fromHex("79ccc193629debca047f0b70604bf6b62ce3dd4a9626e355fafc6198e6ea2b46"
                                                   "d58413673b99b029d665c357601fb426a0b2f4bba200ee9f0a43d19b571a9c71"
                                                   "ef1142e65d5a266fddca832ce59faa7cac0b9cf1be2bffca300d01ee387619c4"
                                                   "ae12fd4438f203a0e4e1c47ec314861f4e9087cb33396a6873e8f9d2539a4b8e");
        QCOMPARE(core::scrypt::parts::roMix(theRfcsBlock(), 16, 1).toHex(), out.toHex());
    }

    void windowsPbkdf2GivesTheRfcsAnswers()
    {
        // RFC 7914, section 11: PBKDF2 with HMAC-SHA-256, one round and eighty thousand.
        const base::Result<QByteArray> once = core::scrypt::parts::pbkdf2("passwd", "salt", 1, 64);
        QVERIFY(once.ok());
        QCOMPARE(once.value().toHex(), QByteArray("55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"
                                                  "49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783"));
        const base::Result<QByteArray> often = core::scrypt::parts::pbkdf2("Password", "NaCl", 80000, 64);
        QVERIFY(often.ok());
        QCOMPARE(often.value().toHex(), QByteArray("4ddcd8f60b98be21830cee5ef22701f9641a4418d04c0414aeff08876b34ab56"
                                                   "a1d425a1225833549adb841b51c9b3176a272bdebba1d078478f62b397f33c8d"));
    }

    void scryptGivesTheRfcsAnswers_data()
    {
        // RFC 7914, section 12, but for the last, which takes a gigabyte.
        QTest::addColumn<QByteArray>("passphrase");
        QTest::addColumn<QByteArray>("salt");
        QTest::addColumn<quint64>("n");
        QTest::addColumn<quint32>("r");
        QTest::addColumn<quint32>("p");
        QTest::addColumn<QByteArray>("key");
        QTest::newRow("nothing and nothing, N 16")
            << QByteArray() << QByteArray() << quint64{16} << quint32{1} << quint32{1}
            << QByteArray("77d6576238657b203b19ca42c18a0497f16b4844e3074ae8dfdffa3fede21442"
                          "fcd0069ded0948f8326a753a0fc81f17e8d3e0fb2e0d3628cf35e20c38d18906");
        QTest::newRow("password and NaCl, N 1024, p 16")
            << QByteArray("password") << QByteArray("NaCl") << quint64{1024} << quint32{8} << quint32{16}
            << QByteArray("fdbabe1c9d3472007856e7190d01e9fe7c6ad7cbc8237830e77376634b373162"
                          "2eaf30d92e22a3886ff109279d9830dac727afb94a83ee6d8360cbdfa2cc0640");
        QTest::newRow("pleaseletmein and SodiumChloride, N 16384")
            << QByteArray("pleaseletmein") << QByteArray("SodiumChloride") << quint64{16384} << quint32{8} << quint32{1}
            << QByteArray("7023bdcb3afd7348461c06cd81fd38ebfda8fbba904f8e3ea9b543f6545da1f2"
                          "d5432955613f0fcf62d49705242a9af9e61e85dc0d651e40dfcf017b45575887");
    }

    void scryptGivesTheRfcsAnswers()
    {
        QFETCH(QByteArray, passphrase);
        QFETCH(QByteArray, salt);
        QFETCH(quint64, n);
        QFETCH(quint32, r);
        QFETCH(quint32, p);
        QFETCH(QByteArray, key);
        QCOMPARE(keyOf(passphrase, salt, Cost{n, r, p}, 64).toHex(), key);
        // A shorter key is the beginning of a longer one, as PBKDF2 makes them.
        QCOMPARE(keyOf(passphrase, salt, Cost{n, r, p}, 32).toHex(), key.left(64));
    }

    void aCostThatIsNoneIsRefusedBeforeAnyMemoryIsAskedFor_data()
    {
        QTest::addColumn<quint64>("n");
        QTest::addColumn<quint32>("r");
        QTest::addColumn<quint32>("p");
        QTest::newRow("N of nothing") << quint64{0} << quint32{8} << quint32{1};
        QTest::newRow("N of one") << quint64{1} << quint32{8} << quint32{1};
        QTest::newRow("N that is no power of two") << quint64{1000} << quint32{8} << quint32{1};
        QTest::newRow("N one above a power of two") << quint64{4097} << quint32{8} << quint32{1};
        QTest::newRow("r of nothing") << quint64{1024} << quint32{0} << quint32{1};
        QTest::newRow("p of nothing") << quint64{1024} << quint32{8} << quint32{0};
        // What the RFC says of N and r together: N is below 2^(16r).
        QTest::newRow("N of 2^16 with r of one") << (quint64{1} << 16) << quint32{1} << quint32{1};
        // More than is asked of any PC in one piece: a gigabyte, four of them, and far more. (The
        // RFC's own bound on r and p together is far above that.)
        QTest::newRow("r times p of 2^30") << quint64{16} << (quint32{1} << 15) << (quint32{1} << 15);
        QTest::newRow("a gigabyte") << (quint64{1} << 20) << quint32{8} << quint32{1};
        QTest::newRow("four gigabytes") << (quint64{1} << 22) << quint32{8} << quint32{1};
        QTest::newRow("N of 2^62") << (quint64{1} << 62) << quint32{8} << quint32{1};
        QTest::newRow("r of 2^29") << quint64{1024} << (quint32{1} << 29) << quint32{1};
        QTest::newRow("p of 2^29") << quint64{1024} << quint32{1} << (quint32{1} << 29);
    }

    void aCostThatIsNoneIsRefusedBeforeAnyMemoryIsAskedFor()
    {
        QFETCH(quint64, n);
        QFETCH(quint32, r);
        QFETCH(quint32, p);
        QCOMPARE(core::scrypt::bytesAskedFor(Cost{n, r, p}), quint64{0});
        // (A memory that has none: whatever is asked of it by mistake, nothing is worked on.)
        WatchedMemory memory(false);
        QByteArray key(32, 'k');
        const base::Result<void> made = core::scrypt::derive(
            "a passphrase", "a salt", Cost{n, r, p}, std::span<quint8>(reinterpret_cast<quint8*>(key.data()), 32),
            std::stop_token(), memory);
        QVERIFY(!made.ok());
        QCOMPARE(memory.taken(), 0);
        QCOMPARE(made.error().code, std::string(core::scrypt::notACost));
        QCOMPARE(key, QByteArray(32, 'k'));
    }

    void whatElseIsNothingToMakeAKeyOfIsRefusedBeforeAnyMemoryIsAskedFor()
    {
        const Cost cost{16, 1, 1};
        WatchedMemory memory(false);
        QByteArray key(100, 'k');
        const auto into = [&key](std::size_t bytes) {
            return std::span<quint8>(reinterpret_cast<quint8*>(key.data()), bytes);
        };
        const QByteArray tooLong(core::scrypt::longestInputBytes + 1, 'x');
        for (const base::Result<void>& made : {
                 core::scrypt::derive("a passphrase", "a salt", cost, into(0), std::stop_token(), memory),
                 core::scrypt::derive("a passphrase", "a salt", cost, into(core::scrypt::longestKeyBytes + 1),
                                      std::stop_token(), memory),
                 core::scrypt::derive(tooLong, "a salt", cost, into(32), std::stop_token(), memory),
                 core::scrypt::derive("a passphrase", tooLong, cost, into(32), std::stop_token(), memory),
             }) {
            QVERIFY(!made.ok());
            QCOMPARE(made.error().code, std::string(core::scrypt::notACost));
        }
        QCOMPARE(memory.taken(), 0);
        QCOMPARE(key, QByteArray(100, 'k'));
    }

    void aPcThatDoesNotHaveTheMemorySaysSoAndNothingFallsOver()
    {
        // As a file of the wall is written: 128 MiB, which this PC is said not to have.
        const Cost cost{quint64{1} << 17, 8, 1};
        WatchedMemory memory(false);
        QByteArray key(32, 'k');
        const base::Result<void> made = core::scrypt::derive(
            "made-up-words", "made-up-salt", cost, std::span<quint8>(reinterpret_cast<quint8*>(key.data()), 32),
            std::stop_token(), memory);
        QVERIFY(!made.ok());
        QCOMPARE(made.error().code, std::string(core::scrypt::noMemory));
        // How much, in a sentence for a person; and nothing of the passphrase or the salt.
        const QString sentence = QString::fromStdString(made.error().message);
        QVERIFY2(sentence.contains(u"(129 MB)"_s), qPrintable(sentence));
        QVERIFY2(!sentence.contains(u"made-up"_s), qPrintable(sentence));
        // Asked once, for all of it, and for nothing again.
        QCOMPARE(memory.taken(), 1);
        QCOMPARE(quint64{memory.bytesTaken()}, core::scrypt::bytesAskedFor(cost));
        QCOMPARE(memory.given(), 0);
        QCOMPARE(key, QByteArray(32, 'k'));
    }

    void theMemoryIsAskedForOnceAndIsWipedBeforeItIsGivenBack()
    {
        const Cost cost{1024, 8, 2};
        WatchedMemory memory(true);
        QByteArray key(32, '\0');
        const base::Result<void> made = core::scrypt::derive(
            "a passphrase", "a salt", cost, std::span<quint8>(reinterpret_cast<quint8*>(key.data()), 32),
            std::stop_token(), memory);
        QVERIFY(made.ok());
        // The same key as with Windows' own memory: it is the work that was done, in this memory.
        QCOMPARE(key.toHex(), keyOf("a passphrase", "a salt", cost, 32).toHex());
        QCOMPARE(memory.taken(), 1);
        // N blocks of 128 * r bytes, the block twice, 64 bytes for one mixing, and p blocks more.
        QCOMPARE(quint64{memory.bytesTaken()}, quint64{1024} * (1024 + 2 + 2) + 64);
        QCOMPARE(quint64{memory.bytesTaken()}, core::scrypt::bytesAskedFor(cost));
        QCOMPARE(memory.given(), 1);
        QVERIFY(memory.givenWhatWasTaken());
        QVERIFY(memory.wipedWhenGiven());
    }

    void workThatNobodyWaitsForStopsAndItsMemoryIsWipedToo()
    {
        const Cost cost{1024, 8, 1};
        WatchedMemory memory(true);
        std::stop_source nobodyWaits;
        nobodyWaits.request_stop();
        QByteArray key(32, 'k');
        const base::Result<void> made = core::scrypt::derive(
            "a passphrase", "a salt", cost, std::span<quint8>(reinterpret_cast<quint8*>(key.data()), 32),
            nobodyWaits.get_token(), memory);
        QVERIFY(!made.ok());
        QCOMPARE(made.error().code, std::string(core::scrypt::stopped));
        QCOMPARE(key, QByteArray(32, 'k'));
        QCOMPARE(memory.taken(), 1);
        QCOMPARE(memory.given(), 1);
        QVERIFY(memory.wipedWhenGiven());
    }

    void workThatIsUnderWayStopsWhenNobodyWaitsAnyMore()
    {
        // The most work a file may ask for, which takes a second in this build: told a moment
        // after it began that nobody waits, it ends there, and says so.
        std::stop_source nobodyWaits;
        base::Result<void> made = base::Error{"test", "it has not ended"};
        QByteArray key(32, 'k');
        QElapsedTimer watch;
        watch.start();
        {
            const std::jthread worker([&made, &key, &nobodyWaits] {
                made = core::scrypt::derive("a passphrase", "a salt", Cost{quint64{1} << 20, 2, 1},
                                            std::span<quint8>(reinterpret_cast<quint8*>(key.data()), 32),
                                            nobodyWaits.get_token());
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            nobodyWaits.request_stop();
            // (Waited for here, where the thread goes.)
        }
        qInfo("work that was told to stop after 50 ms had ended after %lld ms",
              static_cast<long long>(watch.elapsed()));
        QVERIFY(!made.ok());
        QCOMPARE(made.error().code, std::string(core::scrypt::stopped));
        QCOMPARE(key, QByteArray(32, 'k'));
    }

    void asAFileIsWrittenTheWorkAsksFor128Mib()
    {
        const quint64 asked = core::scrypt::bytesAskedFor(Cost{quint64{1} << 17, 8, 1});
        QVERIFY2(asked > (quint64{128} << 20) && asked < (quint64{129} << 20), qPrintable(QString::number(asked)));
    }

    void theMostWorkAFileMayAskForIsDoneOnceAndItsTimeSaid()
    {
        // What a file may ask for at the most (exchange.h, mayBeAsked): N * r * p of 2^21, which
        // is twice what a file is written with, and 256 MiB. Here with the largest N there is.
        const Cost most{quint64{1} << 20, 2, 1};
        const quint64 asked = core::scrypt::bytesAskedFor(most);
        QVERIFY2(asked > (quint64{256} << 20) && asked < (quint64{257} << 20), qPrintable(QString::number(asked)));
        QElapsedTimer watch;
        watch.start();
        const QByteArray key = keyOf("a passphrase for the file", "sixteen bytes!!!", most, 32);
        const qint64 took = watch.elapsed();
        QVERIFY2(!key.startsWith("refused"), key.constData());
        qInfo("the most work a file may ask for (N 2^20, r 2, p 1; %llu bytes): %lld ms",
              static_cast<unsigned long long>(asked), static_cast<long long>(took));
        // And what a file is written with, beside it.
        watch.restart();
        const QByteArray usual =
            keyOf("a passphrase for the file", "sixteen bytes!!!", Cost{quint64{1} << 17, 8, 1}, 32);
        qInfo("the work a file is written with (N 2^17, r 8, p 1): %lld ms", static_cast<long long>(watch.elapsed()));
        QVERIFY2(!usual.startsWith("refused"), usual.constData());
        QVERIFY(key != usual);
    }
};

QTEST_GUILESS_MAIN(TestScrypt)
#include "test_scrypt.moc"
