#include <QByteArray>
#include <QTest>
#include <qkvm/rfb/rfb.h>

#include <cstdint>
#include <span>
#include <vector>

#include "../src/jpeg_pixels.h"
#include "rfb_by_hand.h"
#include "rfb_screen_sweep_inputs.h"

// The JPEG walk's exhaustive reading, and real JPEGs read through the reader, a program of its own (round-5 review
// MEDIUM 1: it runs for long, and has a limit of its own, core/CMakeLists.txt). Its inputs are the fuzz targets' first
// ones too (rfb_screen_sweep_inputs.h, core/fuzz/make_seeds.cpp).
using namespace qkvm::core;
namespace rfb = qkvm::rfb;
namespace rfbt = qkvm::rfb::tests;
using namespace Qt::StringLiterals;

namespace {

std::span<const std::uint8_t> spanOf(const QByteArray& bytes)
{
    return {reinterpret_cast<const std::uint8_t*>(bytes.data()), static_cast<std::size_t>(bytes.size())};
}

} // namespace

class TestRfbScreenSweeps : public QObject {
    Q_OBJECT

private slots:
    // Round 3's required exhaustive reading: every truncation, and every single-byte change of the first 128 bytes, of
    // a corpus of real and hostile JPEGs through the walk. Nothing may crash (the file is built with the standard
    // library's hardening, and this runs under the address sanitizer too), whatever the bytes.
    void theJpegWalkSurvivesEveryPrefixAndByte()
    {
        const std::vector<QByteArray> corpus = tests::jpegWalkCorpus();
        JpegPixels pixels;
        qint64 ran = 0;
        for (const QByteArray& one : corpus) {
            for (int n = 0; n <= one.size(); ++n) {
                static_cast<void>(JpegPixels::sizeOf(spanOf(one.left(n))));
                ++ran;
            }
            // The full input is decoded too (only an accepted one reaches FFmpeg).
            const std::optional<JpegPixels::Size> said = JpegPixels::sizeOf(spanOf(one));
            if (said.has_value()) {
                static_cast<void>(pixels.decode(spanOf(one), said->width, said->height));
            }
        }
        // Every byte of both real JPEGs (Qt's: SOI, APP0, its quantisation and Huffman tables, SOF0, SOS, the entropy
        // and EOI; round-4 review LOW 4: not only the first 128) changed to each of its other 255 values, through the
        // walk; each the walk takes is decoded too, at its rectangle's size, as the screen would give it to FFmpeg.
        struct Real {
            QByteArray bytes;
            int width = 0;
            int height = 0;
        };
        qint64 changed = 0;
        qint64 decoded = 0;
        QString markers;
        for (const Real& real : {Real{corpus[0], 40, 24}, Real{corpus[1], 16, 8}}) {
            QVERIFY(pixels.decode(spanOf(real.bytes), real.width, real.height).ok());
            // (What it is made of, said: its segments up to the scan.)
            for (qsizetype at = 2; at + 3 < real.bytes.size();) {
                const auto marker = static_cast<std::uint8_t>(real.bytes[at + 1]);
                markers += QString::number(marker, 16).toUpper() + u' ';
                if (marker == 0xDA) {
                    break;
                }
                at += 2 + ((static_cast<std::uint8_t>(real.bytes[at + 2]) << 8) |
                           static_cast<std::uint8_t>(real.bytes[at + 3]));
            }
            markers += u"| "_s;
            for (qsizetype pos = 0; pos < real.bytes.size(); ++pos) {
                for (int value = 0; value < 256; ++value) {
                    if (value == static_cast<std::uint8_t>(real.bytes[pos])) {
                        continue;
                    }
                    QByteArray one = real.bytes;
                    one[pos] = static_cast<char>(value);
                    ++changed;
                    if (JpegPixels::sizeOf(spanOf(one)).has_value()) {
                        static_cast<void>(pixels.decode(spanOf(one), real.width, real.height));
                        ++decoded;
                    }
                }
            }
        }
        qInfo(
            "jpeg walk exhaustive: %lld cut short or hostile; %lld single-byte changes of every byte of two real JPEGs "
            "(segments %s), %lld of them taken by the walk and decoded; no crash",
            static_cast<long long>(ran), static_cast<long long>(changed), qPrintable(markers),
            static_cast<long long>(decoded));
        QVERIFY(changed > 0);
    }

    // Real JPEGs through the reader (round-4 review LOW 4): a Tight JPEG rectangle of a real 16 by 16 JPEG, read by an
    // rfb::Client with the decoder the screen gives it (JpegPixels: the walk, then FFmpeg), not a stand-in. Cut short
    // at every byte; every byte of the update (its header, the rectangle's, Tight's, and the whole JPEG) changed to
    // each of its other 255 values; split in two at every byte, and fed a byte at a time, which draws what one feed
    // draws.
    void aRealJpegRectangleSurvivesEveryChangeThroughTheReader()
    {
        const QByteArray jpeg = tests::realJpeg16();
        JpegPixels pixels;
        const auto options = [&pixels] {
            rfb::Options made;
            made.signsIn = {rfb::securityNone};
            made.jpeg = [&pixels](std::span<const std::uint8_t> data, int width, int height) {
                return pixels.decode(data, width, height);
            };
            return made;
        };
        const rfbt::Bytes handshake = tests::handshake16();
        const rfbt::Bytes stream = tests::realJpegStream(jpeg);
        rfb::Client whole(options());
        QVERIFY(whole.feed(stream, 0).ok());
        QCOMPARE(whole.frames(), std::uint64_t{1});
        const rfb::Bytes expected = whole.rgb();
        qint64 cut = 0;
        qint64 changed = 0;
        qint64 splits = 0;
        for (std::size_t n = 0; n < stream.size(); ++n) {
            rfb::Client client(options());
            static_cast<void>(
                client.feed(rfbt::Bytes(stream.begin(), stream.begin() + static_cast<std::ptrdiff_t>(n)), 0));
            ++cut;
        }
        for (std::size_t pos = handshake.size(); pos < stream.size(); ++pos) {
            for (int value = 0; value < 256; ++value) {
                if (value == stream[pos]) {
                    continue;
                }
                rfbt::Bytes other = stream;
                other[pos] = static_cast<std::uint8_t>(value);
                rfb::Client client(options());
                static_cast<void>(client.feed(other, 0));
                ++changed;
            }
        }
        for (std::size_t at = 1; at < stream.size(); ++at) {
            rfb::Client client(options());
            QVERIFY(client.feed(rfbt::Bytes(stream.begin(), stream.begin() + static_cast<std::ptrdiff_t>(at)), 0).ok());
            QVERIFY(client.feed(rfbt::Bytes(stream.begin() + static_cast<std::ptrdiff_t>(at), stream.end()), 0).ok());
            QVERIFY2(client.frames() == 1 && client.rgb() == expected, QByteArray::number(static_cast<qulonglong>(at)));
            ++splits;
        }
        rfb::Client bytewise(options());
        for (const std::uint8_t one : stream) {
            QVERIFY(bytewise.feed(rfbt::Bytes{one}, 0).ok());
        }
        QVERIFY(bytewise.frames() == 1 && bytewise.rgb() == expected);
        qInfo("real jpeg through the reader: a %lld-byte JPEG in a %zu-byte stream; %lld cut short, %lld single-byte "
              "changes, %lld splits and a byte at a time; no crash",
              static_cast<long long>(jpeg.size()), stream.size(), static_cast<long long>(cut),
              static_cast<long long>(changed), static_cast<long long>(splits));
    }
};

QTEST_GUILESS_MAIN(TestRfbScreenSweeps)
#include "test_rfb_screen_sweeps.moc"
