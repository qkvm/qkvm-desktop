#include "rfb_by_hand.h"
#include "rfb_client_by_hand.h"
#include "rfb_sweep_streams.h"

#include <QTest>
#include <qkvm/rfb/rfb.h>

#include <cstdint>
#include <span>
#include <vector>

// The reader's exhaustive reading, a program of its own (round-5 review MEDIUM 1: it runs for long, and has a limit
// of its own, rfb/CMakeLists.txt): every encoding's stream cut short at every byte, changed at every byte to every
// other value, and split at every byte, through rfb::Client and the inflater.
using namespace qkvm::rfb;
using namespace qkvm::rfb::tests;
using qkvm::rfb::tests::Bytes;
namespace base = qkvm::base;

namespace {

// A stream fed whole to a client of its own: what it came to.
struct Read {
    bool ok = false;
    std::uint64_t updates = 0;
    std::uint64_t frames = 0;
    Bytes rgb;
    bool operator==(const Read&) const = default;
};
Read readOf(Client& client, const base::Result<void>& fed)
{
    return Read{fed.ok(), client.updates(), client.frames(), client.rgb()};
}

} // namespace

class TestRfbSweeps : public QObject {
    Q_OBJECT

private slots:
    // The reader's exhaustive reading (round-3 review; round-4 review LOW 4), of each of sweepStreams(): cut short at
    // every byte, and every byte changed to each of its other 255 values in turn, the handshake's in the first stream
    // alone (it is the same in all), then the update's header, the rectangle's and the whole of its body, through the
    // inflater where it is deflated. Nothing may crash (the library is built with the standard library's hardening;
    // this runs under the address sanitizer too: the sanitizer's run of 2026-11-14), whatever the bytes. Each stream as
    // it is reads whole.
    void theReaderSurvivesEveryPrefixAndByte()
    {
        // (They are the fuzz target's first inputs too: core/fuzz/make_seeds.cpp.)
        const std::vector<Bytes> streams = sweepStreams();
        const std::size_t handshake = sweepHandshake().size();
        qint64 cut = 0;
        qint64 changed = 0;
        qint64 bytes = 0;
        for (std::size_t one = 0; one < streams.size(); ++one) {
            const Bytes& stream = streams[one];
            {
                Client client(withJpeg());
                const auto fed = client.feed(stream, 0);
                QVERIFY2(okay(fed) && client.updates() == 1, QByteArray::number(static_cast<qulonglong>(one)));
            }
            for (std::size_t n = 0; n < stream.size(); ++n) {
                Client client(withJpeg());
                static_cast<void>(
                    client.feed(Bytes(stream.begin(), stream.begin() + static_cast<std::ptrdiff_t>(n)), 0));
                ++cut;
            }
            for (std::size_t pos = one == 0 ? 0 : handshake; pos < stream.size(); ++pos) {
                ++bytes;
                for (int value = 0; value < 256; ++value) {
                    if (value == stream[pos]) {
                        continue;
                    }
                    Bytes other = stream;
                    other[pos] = static_cast<std::uint8_t>(value);
                    Client client(withJpeg());
                    static_cast<void>(client.feed(other, 0));
                    ++changed;
                }
            }
        }
        qInfo("rfb reader exhaustive: %zu streams, %lld cut short, %lld bytes changed to each other value: %lld "
              "single-byte changes, no crash",
              streams.size(), static_cast<long long>(cut), static_cast<long long>(bytes),
              static_cast<long long>(changed));
        QVERIFY(changed > 0);
    }

    // ...and fed in pieces (round-4 review LOW 4): split into two feeds at every byte, and a byte a feed, each stream
    // draws what it draws fed whole, so what a feed leaves unread is taken up again where it stopped.
    void aStreamSplitAnywhereDrawsWhatOneFeedDraws()
    {
        qint64 splits = 0;
        qint64 feeds = 0;
        for (const Bytes& stream : sweepStreams()) {
            Client whole(withJpeg());
            const auto fed = whole.feed(stream, 0);
            const Read expected = readOf(whole, fed);
            QVERIFY(expected.ok);
            for (std::size_t at = 1; at < stream.size(); ++at) {
                Client client(withJpeg());
                const auto first =
                    client.feed(Bytes(stream.begin(), stream.begin() + static_cast<std::ptrdiff_t>(at)), 0);
                QVERIFY(okay(first));
                const auto rest = client.feed(Bytes(stream.begin() + static_cast<std::ptrdiff_t>(at), stream.end()), 0);
                QVERIFY2(readOf(client, rest) == expected, QByteArray::number(static_cast<qulonglong>(at)));
                ++splits;
            }
            Client bytewise(withJpeg());
            base::Result<void> last = base::Result<void>{};
            for (const std::uint8_t one : stream) {
                last = bytewise.feed(Bytes{one}, 0);
                QVERIFY(okay(last));
                ++feeds;
            }
            QVERIFY(readOf(bytewise, last) == expected);
        }
        qInfo("rfb reader split: %lld streams split in two at every byte, %lld fed a byte at a time, all as fed whole",
              static_cast<long long>(splits), static_cast<long long>(feeds));
    }
};

QTEST_GUILESS_MAIN(TestRfbSweeps)
#include "test_rfb_sweeps.moc"
