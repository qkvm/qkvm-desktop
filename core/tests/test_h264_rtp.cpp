#include <QTest>
#include <QtEndian>
#include <qkvm/core/h264_rtp.h>
#include <qkvm/core/webrtc_receiver.h>
using namespace Qt::StringLiterals;
using qkvm::core::H264Rtp;
using qkvm::core::WebRtcReceiver;
namespace {
QByteArray packet(quint16 sequence, quint32 time, const QByteArray& payload, bool end = true)
{
    QByteArray bytes(12, '\0');
    bytes[0] = '\x80';
    bytes[1] = char(96 | (end ? 128 : 0));
    qToBigEndian(sequence, bytes.data() + 2);
    qToBigEndian(time, bytes.data() + 4);
    qToBigEndian<quint32>(123, bytes.data() + 8);
    return bytes + payload;
}
QByteArray sps()
{
    return QByteArray::fromHex("6742e01faa");
}
QByteArray pps()
{
    return QByteArray::fromHex("68ceaa");
}
void configured(H264Rtp& reader)
{
    QVERIFY(reader.parameters(sps(), pps()));
}
QByteArray offer()
{
    return "v=0\r\na=fingerprint:sha-256 00:11\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\na=mid:video\r\na=sendonly\r\na=rtpmap:96 H264/90000\r\na=fmtp:96 packetization-mode=1;profile-level-id=42e01f\r\n"_ba;
}
} // namespace
class TestH264Rtp : public QObject {
    Q_OBJECT
private slots:
    void startsOnlyAtAKeyFrame()
    {
        H264Rtp reader;
        configured(reader);
        QVERIFY(!reader.take(packet(1, 1, "\x41\x22"_ba)));
        const auto key = reader.take(packet(2, 2, "\x65\x22"_ba));
        if (!key) {
            QFAIL("Expected an assembled key frame");
        }
        QVERIFY(key->key);
        QVERIFY(key->bytes.contains(sps()));
        QVERIFY(key->bytes.contains(pps()));
        QVERIFY(reader.take(packet(3, 3, "\x41\x23"_ba)));
    }
    void fragmentedNalAndSequenceWrap()
    {
        H264Rtp reader;
        configured(reader);
        QVERIFY(!reader.take(packet(65535, 42, QByteArray::fromHex("7c851122"), false)));
        const auto frame = reader.take(packet(0, 42, QByteArray::fromHex("7c453344")));
        if (!frame) {
            QFAIL("Expected an assembled fragmented frame");
        }
        QVERIFY(frame->key);
        QVERIFY(frame->bytes.endsWith(QByteArray::fromHex("000000016511223344")));
    }
    void missingFragmentWaitsForAnotherKey()
    {
        H264Rtp reader;
        configured(reader);
        QVERIFY(!reader.take(packet(1, 1, QByteArray::fromHex("7c851122"), false)));
        QVERIFY(!reader.take(packet(3, 1, QByteArray::fromHex("7c453344"))));
        QVERIFY(reader.needsKey());
        QVERIFY(!reader.take(packet(4, 2, "\x41\x22"_ba)));
        QVERIFY(reader.take(packet(5, 3, "\x65\x22"_ba)));
    }
    void duplicateAndLatePacketsDoNotRewind()
    {
        H264Rtp reader;
        configured(reader);
        QVERIFY(reader.take(packet(20, 10, "\x65\x22"_ba)));
        QVERIFY(!reader.take(packet(20, 10, "\x65\x22"_ba)));
        QVERIFY(!reader.take(packet(19, 9, "\x41\x22"_ba)));
        QVERIFY(reader.take(packet(21, 11, "\x41\x22"_ba)));
    }
    void aggregatesParameterSetsAndRejectsTruncation()
    {
        H264Rtp reader;
        QByteArray aggregate(1, char(0x78));
        for (const QByteArray& nal : {sps(), pps(), "\x65\x22"_ba}) {
            QByteArray length(2, '\0');
            qToBigEndian(static_cast<quint16>(nal.size()), length.data());
            aggregate += length + nal;
        }
        QVERIFY(reader.take(packet(1, 1, aggregate)));
        aggregate.chop(1);
        QVERIFY(!reader.take(packet(2, 2, aggregate)));
        QVERIFY(reader.needsKey());
    }
    void boundsFragmentsAndRejectsMalformedHeaders()
    {
        H264Rtp reader;
        configured(reader);
        QVERIFY(
            !reader.take(packet(1, 1, QByteArray::fromHex("7c85") + QByteArray(H264Rtp::largestPicture, 'x'), false)));
        QVERIFY(reader.needsKey());
        for (const QByteArray& bytes : {""_ba, "tiny"_ba, QByteArray(12, '\xff'), packet(2, 2, "\x7c\xc5\x11"_ba)}) {
            QVERIFY(!reader.take(bytes));
        }
        QByteArray extended = packet(3, 3, "\x65\x11"_ba);
        extended[0] = '\x90';
        QVERIFY(!reader.take(extended));
    }
    void parameterChangeRequiresKeyAndSsrcDoesNotReuseParameters()
    {
        H264Rtp reader;
        configured(reader);
        QVERIFY(reader.take(packet(1, 1, "\x65\x22"_ba)));
        QVERIFY(!reader.take(packet(2, 2, sps() + 'x', false)));
        QVERIFY(!reader.take(packet(3, 2, "\x41\x22"_ba)));
        QVERIFY(reader.take(packet(4, 3, "\x65\x22"_ba)));
        QByteArray next = packet(5, 4, "\x65\x22"_ba);
        next[11] = char(124);
        QVERIFY(!reader.take(next));
    }
    void negotiatesOnlySupportedH264()
    {
        QVERIFY(WebRtcReceiver::supports(offer()));
        for (const QByteArray& altered :
             {QByteArray(offer()).replace("H264/90000", "H265/90000"),
              QByteArray(offer()).replace("packetization-mode=1", "packetization-mode=2"),
              QByteArray(offer()).replace("42e01f", "6e001f"), QByteArray(offer()).replace("sha-256", "sha-1"),
              offer() + "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"_ba, offer() + QByteArray(65536, 'x')}) {
            QVERIFY(!WebRtcReceiver::supports(altered));
        }
    }
};
QTEST_GUILESS_MAIN(TestH264Rtp)
#include "test_h264_rtp.moc"
