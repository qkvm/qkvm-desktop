#include "jetkvm_peer_by_hand.h"
#include "rtc_sdp.h"
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTest>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <qkvm/base/clock.h>
#include <qkvm/core/webrtc_offer.h>
#include <thread>

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::WebRtcOffer;
using core::testing::JetKvmPeerByHand;
Q_DECLARE_METATYPE(qkvm::core::WebRtcOffer::Said)

namespace {
// The formats and channels a JetKVM is offered (design 2 and 6.2; the web's offer).
const QByteArray formatA = "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42001f";
const QByteArray formatB = "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f";

WebRtcOffer::Setup jetKvmSetup()
{
    WebRtcOffer::Setup setup;
    setup.h264Formats = {formatA, formatB};
    setup.channels = {{"rpc", true, 65536}, {"hidrpc", false, 64}};
    setup.stopsOn = [](QByteArrayView text) { return text.contains("\"otherSessionConnected\""); };
    // (The far end is on this PC's loopback, as the device's signalling address is in these tests.)
    setup.loopbackAllowed = true;
    return setup;
}

// An answer with `lines` put in at the end of its video section, as a device that puts its addresses
// into its answer does.
QByteArray withLinesInItsVideo(const QByteArray& answer, const QList<QByteArray>& lines)
{
    QByteArray out;
    bool inVideo = false;
    for (const QByteArray& line : answer.split('\n')) {
        if (line.startsWith("m=") && inVideo) {
            for (const QByteArray& added : lines) {
                out += added + "\r\n";
            }
        }
        if (line.startsWith("m=")) {
            inVideo = line.startsWith("m=video ");
        }
        if (!line.isEmpty()) {
            out += line + '\n';
        }
    }
    return out;
}

// The answer with the mid of its `kind` section ("video", "application") made another.
QByteArray withAnotherMid(const QByteArray& answer, const QByteArray& kind)
{
    QByteArray out;
    bool inIt = false;
    for (const QByteArray& line : answer.split('\n')) {
        if (line.startsWith("m=")) {
            inIt = line.startsWith("m=" + kind + ' ');
        }
        if (inIt && line.startsWith("a=mid:")) {
            out += "a=mid:another\r\n";
        } else if (!line.isEmpty()) {
            out += line + '\n';
        }
    }
    return out;
}

QByteArray awaitOffer(WebRtcOffer& offer)
{
    QSignalSpy ready(&offer, &WebRtcOffer::offerReady);
    if (!offer.begin() || !ready.wait(5000)) {
        return {};
    }
    return ready.first().first().toByteArray();
}

// The offer answered by the peer, and its addresses handed over one by one, 10 ms apart, as the device
// trickles them, until both channels are open (or 15 s).
bool connectTo(WebRtcOffer& offer, JetKvmPeerByHand& peer)
{
    QSignalSpy opened(&offer, &WebRtcOffer::channelOpened);
    const QByteArray sdp = awaitOffer(offer);
    if (sdp.isEmpty() || !offer.answer(peer.answer(sdp))) {
        return false;
    }
    qsizetype given = 0;
    QElapsedTimer waited;
    waited.start();
    while (opened.size() < 2 && waited.elapsed() < 15'000) {
        const QList<QByteArray> addresses = peer.candidates();
        for (; given < addresses.size(); ++given) {
            offer.candidate(addresses[given]);
            QTest::qWait(10);
        }
        QTest::qWait(20);
    }
    return opened.size() == 2;
}

void busyFor(int ms)
{
    const qint64 until = base::steadyNowUs() + qint64{ms} * 1000;
    while (base::steadyNowUs() < until) {
    }
}
} // namespace

// The offering side of a JetKVM's session (M6 2e), against a libdatachannel peer that plays the device
// on 127.0.0.1 (jetkvm_peer_by_hand.h). What the real device (pion) and the web's fake (aiortc) do of
// the same is the joint test's (2i) and the owner's device's (design 8).
class TestWebRtcOffer : public QObject {
    Q_OBJECT
private slots:
    void initTestCase()
    {
        qRegisterMetaType<qkvm::core::WebRtcOffer::Said>();
        QVERIFY2(JetKvmPeerByHand::filmIsThere(), JetKvmPeerByHand::noFilm);
    }

    // The offer: video it only receives, in the two H.264 formats and nothing else (no RTX, no VP8, no
    // audio), one application for the two channels, and host addresses only (no STUN, no TURN).
    void theOfferIsVideoReceiveOnlyH264AndTwoChannels()
    {
        WebRtcOffer offer(jetKvmSetup());
        const QByteArray sdp = awaitOffer(offer);
        QVERIFY(!sdp.isEmpty());
        QCOMPARE(sdp.count("m=video "), 1);
        // (Its port is the one gathered.)
        QVERIFY2(QRegularExpression(u"m=video \\d+ UDP/TLS/RTP/SAVPF 96 97\r\n"_s)
                     .match(QString::fromLatin1(sdp))
                     .hasMatch(),
                 sdp.constData());
        QVERIFY(sdp.contains("a=rtpmap:96 H264/90000"));
        QVERIFY(sdp.contains("a=rtpmap:97 H264/90000"));
        QVERIFY(sdp.contains("a=fmtp:96 " + formatA + "\r\n"));
        QVERIFY(sdp.contains("a=fmtp:97 " + formatB + "\r\n"));
        QVERIFY(sdp.contains("a=recvonly"));
        QVERIFY(sdp.contains("a=rtcp-fb:96 nack pli"));
        QVERIFY(!sdp.contains("rtx") && !sdp.contains("RTX") && !sdp.contains("VP8") && !sdp.contains("m=audio"));
        QCOMPARE(sdp.count("m=application "), 1);
        QCOMPARE(sdp.count("a=rtpmap:"), 2);
        for (const QByteArray& line : sdp.split('\n')) {
            if (line.startsWith("a=candidate:")) {
                QVERIFY2(line.contains(" typ host"), line.constData());
            }
        }
    }

    // An answer that is not to this offer is refused, and nothing is taken of it: a payload that was
    // not offered, a video that only receives or does nothing, no application, larger than 64 KiB, a
    // NUL in it, no fingerprint. The answer itself is taken.
    void anAnswerNotOursIsRefused()
    {
        JetKvmPeerByHand peer;
        WebRtcOffer first(jetKvmSetup());
        const QByteArray sdp = awaitOffer(first);
        const QByteArray answer = peer.answer(sdp);
        QVERIFY2(answer.contains("a=sendonly"), answer.constData());
        const auto withoutSection = [&answer](const QByteArray& kind) {
            QByteArray out;
            bool skipping = false;
            for (const QByteArray& line : answer.split('\n')) {
                if (line.startsWith("m=")) {
                    skipping = line.startsWith("m=" + kind);
                }
                if (!skipping && !line.isEmpty()) {
                    out += line + '\n';
                }
            }
            return out;
        };
        QByteArray otherPayload = answer;
        otherPayload.replace("SAVPF 96", "SAVPF 100").replace("rtpmap:96", "rtpmap:100").replace("fmtp:96", "fmtp:100");
        QByteArray noFingerprint;
        for (const QByteArray& line : answer.split('\n')) {
            if (!line.startsWith("a=fingerprint:") && !line.isEmpty()) {
                noFingerprint += line + '\n';
            }
        }
        QByteArray withNul = answer;
        withNul.insert(answer.indexOf("a=sendonly"), QByteArray(1, '\0'));
        const QList<std::pair<const char*, QByteArray>> wrong = {
            {"a payload not offered", otherPayload},
            {"receive only", QByteArray(answer).replace("a=sendonly", "a=recvonly")},
            {"inactive", QByteArray(answer).replace("a=sendonly", "a=inactive")},
            {"no application", withoutSection("application")},
            {"over 64 KiB", answer + "a=x-filler:" + QByteArray(65536, 'x') + "\r\n"},
            {"a NUL", withNul},
            {"no fingerprint", noFingerprint},
        };
        for (const auto& [name, sdpOf] : wrong) {
            WebRtcOffer offer(jetKvmSetup());
            QVERIFY(!awaitOffer(offer).isEmpty());
            QVERIFY2(!offer.answer(sdpOf), name);
        }
        QVERIFY(first.answer(answer));
        QVERIFY2(!first.answer(answer), "a second answer is no answer");
    }

    // 2e's reading (L2): an answer whose video or application is not under the mid it was offered under is
    // refused before anything of it is taken: the right answer to the same offer is taken after it.
    void anAnswerOfOtherMidsIsRefusedAndNothingTaken()
    {
        for (const QByteArray& kind : {QByteArray("video"), QByteArray("application")}) {
            JetKvmPeerByHand peer;
            WebRtcOffer offer(jetKvmSetup());
            const QByteArray answer = peer.answer(awaitOffer(offer));
            const QByteArray other = withAnotherMid(answer, kind);
            QVERIFY(other != answer);
            QVERIFY2(!offer.answer(other), kind.constData());
            QVERIFY2(offer.answer(answer), kind.constData());
        }
    }

    // 2e, the second reading (L): an answer this program and the library would read two ways is refused
    // before anything of it is taken: a section that says its mid twice (this program took the first, the
    // library keeps the last), and a line that begins with whitespace (this program trimmed it, the library
    // does not). The right answer to the same offer is taken after it.
    void anAnswerReadTwoWaysIsRefusedAndNothingTaken_data()
    {
        // In place of its mid's line (`@`), where this program read the one it offered (the video's last, the
        // application's first, each trimmed) and the library another.
        QTest::addColumn<QByteArray>("kind");
        QTest::addColumn<QByteArray>("lines");
        QTest::newRow("its video's mid twice") << QByteArray("video") << QByteArray("a=mid:another\r\n@\n");
        QTest::newRow("its application's mid twice") << QByteArray("application") << QByteArray("@\na=mid:another\r\n");
        QTest::newRow("its video's mid after a space") << QByteArray("video") << QByteArray("a=mid:another\r\n @\n");
        QTest::newRow("its application's mid after a tab")
            << QByteArray("application") << QByteArray("\t@\na=mid:another\r\n");
        // The reading of 56258cd (L3): a bare a=mid is a mid to the library, an empty one.
        QTest::newRow("its video's mid, then a bare one") << QByteArray("video") << QByteArray("@\na=mid\r\n");
        QTest::newRow("its application's mid, then one with a space")
            << QByteArray("application") << QByteArray("@\na=mid \r\n");
    }
    void anAnswerReadTwoWaysIsRefusedAndNothingTaken()
    {
        QFETCH(QByteArray, kind);
        QFETCH(QByteArray, lines);
        JetKvmPeerByHand peer;
        WebRtcOffer offer(jetKvmSetup());
        const QByteArray answer = peer.answer(awaitOffer(offer));
        QByteArray twoWays;
        bool inIt = false;
        for (const QByteArray& line : answer.split('\n')) {
            if (line.startsWith("m=")) {
                inIt = line.startsWith("m=" + kind + ' ');
            }
            if (inIt && line.startsWith("a=mid:")) {
                twoWays += QByteArray(lines).replace("@", line);
            } else if (!line.isEmpty()) {
                twoWays += line + '\n';
            }
        }
        QVERIFY(twoWays != answer);
        QVERIFY(!offer.answer(twoWays));
        QVERIFY(offer.answer(answer));
    }

    // 2e's reading (M1): the device's addresses inside its answer are taken out of it and go the way every
    // address goes (candidate()): judged, and counted toward the forty. Here its own, gathered, then 45 more
    // of loopback, one by a name and an end of candidates: forty taken, a name refused, the rest counted.
    void addressesInTheAnswerGoTheWayEveryAddressGoes()
    {
        JetKvmPeerByHand peer;
        WebRtcOffer offer(jetKvmSetup());
        QSignalSpy opened(&offer, &WebRtcOffer::channelOpened);
        const QByteArray answer = peer.answer(awaitOffer(offer));
        QTRY_VERIFY(!peer.candidates().isEmpty());
        QTest::qWait(300);
        const QList<QByteArray> own = peer.candidates();
        QList<QByteArray> lines;
        for (const QByteArray& one : own) {
            lines.append("a=" + one);
        }
        for (int port = 0; port < 45; ++port) {
            lines.append("a=candidate:9 1 UDP 1 127.0.0.1 " + QByteArray::number(20000 + port) + " typ host");
        }
        lines.append("a=candidate:9 1 UDP 1 n1.evil.example 9 typ host");
        lines.append("a=end-of-candidates");
        QVERIFY(offer.answer(withLinesInItsVideo(answer, lines)));
        const WebRtcOffer::Refused refused = offer.refused();
        QCOMPARE(refused.namedCandidates, quint64{1});
        QCOMPARE(refused.candidatesBeyond, quint64(own.size() + 45 - 40));
        QCOMPARE(refused.candidates, quint64(own.size() + 45 - 40 + 1));
        // (Its own came first, and are among the forty: it connects.)
        QTRY_COMPARE_WITH_TIMEOUT(opened.size(), 2, 15000);
    }

    // 2e's reading (M1): an address is taken only where it is UDP, to a host by its number (no name is ever
    // looked up), and to one that may be reached: not unspecified, multicast or the broadcast, and not this
    // PC's loopback unless the device's own signalling address is loopback. Each refusal counted by its reason.
    void anAddressIsUdpToAHostByItsNumber_data()
    {
        QTest::addColumn<QByteArray>("text");
        QTest::addColumn<bool>("loopbackAllowed");
        QTest::addColumn<QByteArray>("reason"); // empty: taken
        const QByteArray at = "candidate:1 1 UDP 2122317823 ";
        // (Of those taken, only this PC's loopback here: what is taken is handed to the library, which may send to
        // it. Addresses on the network that are taken are anAddressIsHandedOnMadeAnew's, which hands nothing on.)
        QTest::newRow("loopback, the device on it") << at + "127.0.0.1 9000 typ host" << true << QByteArray();
        QTest::newRow("in small letters")
            << QByteArray("candidate:1 1 udp 1 127.0.0.1 9 typ host") << true << QByteArray();
        QTest::newRow("TCP") << QByteArray("candidate:1 1 TCP 2122317823 192.168.1.20 9 typ host tcptype active")
                             << false << QByteArray("notUdp");
        QTest::newRow("a name") << at + "n1.evil.example 9 typ host" << false << QByteArray("named");
        QTest::newRow("a local name") << at + "3d1c6c47-0d1e.local 9 typ host" << false << QByteArray("named");
        QTest::newRow("a number written short") << at + "127.1 9 typ host" << true << QByteArray("named");
        QTest::newRow("unspecified") << at + "0.0.0.0 9 typ host" << false << QByteArray("unreachable");
        QTest::newRow("unspecified IPv6") << at + ":: 9 typ host" << false << QByteArray("unreachable");
        QTest::newRow("unspecified IPv4 written as IPv6")
            << at + "::ffff:0.0.0.0 9 typ host" << false << QByteArray("unreachable");
        QTest::newRow("multicast") << at + "239.255.255.250 1900 typ host" << false << QByteArray("unreachable");
        QTest::newRow("multicast IPv6") << at + "ff02::1 9 typ host" << false << QByteArray("unreachable");
        QTest::newRow("the broadcast") << at + "255.255.255.255 9 typ host" << false << QByteArray("unreachable");
        QTest::newRow("loopback, the device not on it")
            << at + "127.0.0.1 445 typ host" << false << QByteArray("unreachable");
        QTest::newRow("IPv6 loopback, the device not on it")
            << at + "::1 53 typ host" << false << QByteArray("unreachable");
        QTest::newRow("loopback as IPv6, the device not on it")
            << at + "::ffff:127.0.0.1 53 typ host" << false << QByteArray("unreachable");
        QTest::newRow("no port") << at + "192.168.1.20 0 typ host" << false << QByteArray("malformed");
        QTest::newRow("a port too large") << at + "192.168.1.20 65536 typ host" << false << QByteArray("malformed");
        QTest::newRow("too few words") << QByteArray("candidate:1 1 UDP 2122317823 192.168.1.20") << false
                                       << QByteArray("malformed");
        QTest::newRow("no typ") << at + "192.168.1.20 9 kind host" << false << QByteArray("malformed");
        // 2e, the second reading (M1): each byte a visible one of ASCII or a space alone between two words (the
        // library splits at whitespace of any kind: what it would read is not what was judged); a foundation of
        // ICE's signs; the numbers within their bounds; a type ICE names.
        // (Each on this PC's loopback, with the device on it: refused here for its form alone.)
        QTest::newRow("a tab in place of a space") << at + "127.0.0.1\t9 typ host" << true << QByteArray("malformed");
        QTest::newRow("a tab after its type") << at + "127.0.0.1 9 typ host x\ty" << true << QByteArray("malformed");
        QTest::newRow("a carriage return after its type")
            << at + "127.0.0.1 9 typ host x\ry" << true << QByteArray("malformed");
        QTest::newRow("a vertical tab after its type")
            << at + "127.0.0.1 9 typ host x\vy" << true << QByteArray("malformed");
        QTest::newRow("a form feed after its type")
            << at + "127.0.0.1 9 typ host x\fy" << true << QByteArray("malformed");
        QTest::newRow("two spaces") << at + "127.0.0.1 9 typ host  x" << true << QByteArray("malformed");
        QTest::newRow("a space at its end") << at + "127.0.0.1 9 typ host " << true << QByteArray("malformed");
        QTest::newRow("a sign beyond ASCII")
            << at + "127.0.0.1 9 typ host x\xC3\xA9" << true << QByteArray("malformed");
        QTest::newRow("a foundation of other signs")
            << QByteArray("candidate:a.b 1 UDP 1 127.0.0.1 9 typ host") << true << QByteArray("malformed");
        QTest::newRow("a foundation too long")
            << "candidate:" + QByteArray(33, 'f') + " 1 UDP 1 127.0.0.1 9 typ host" << true << QByteArray("malformed");
        QTest::newRow("component 0") << QByteArray("candidate:1 0 UDP 1 127.0.0.1 9 typ host") << true
                                     << QByteArray("malformed");
        QTest::newRow("a priority beyond 32 bits")
            << QByteArray("candidate:1 1 UDP 4294967296 127.0.0.1 9 typ host") << true << QByteArray("malformed");
        QTest::newRow("a type ICE does not name") << at + "127.0.0.1 9 typ local" << true << QByteArray("malformed");
        // The reading of 56258cd (L1): an IPv4 part with a nought before it, which readers read two ways.
        QTest::newRow("a part with a nought first") << at + "127.0.0.01 9 typ host" << true << QByteArray("malformed");
        QTest::newRow("its first part so") << at + "0127.0.0.1 9 typ host" << true << QByteArray("malformed");
        QTest::newRow("the IPv4 in an IPv6 so")
            << at + "::ffff:127.0.0.01 9 typ host" << true << QByteArray("malformed");
        // 2e, the second reading (L): 0.0.0.0/8, and IPv4 written in the old way inside ::/96.
        QTest::newRow("this network") << at + "0.1.2.3 9 typ host" << true << QByteArray("unreachable");
        QTest::newRow("IPv4 the old way") << at + "::127.0.0.1 9 typ host" << true << QByteArray("unreachable");
        QTest::newRow("IPv6 loopback, the device on it") << at + "::1 9 typ host" << true << QByteArray();
    }

    // 2e, the second reading (M1): what the library is handed is made anew of what was judged, never the far
    // end's own text: its numbers as numbers, its host as the number judged (IPv4 written as IPv6 as the IPv4
    // it is), and nothing of what follows its type.
    void anAddressIsHandedOnMadeAnew_data()
    {
        QTest::addColumn<QByteArray>("text");
        QTest::addColumn<QByteArray>("handed");
        QTest::newRow("as it came") << QByteArray("candidate:1 1 UDP 2122317823 192.168.1.20 9 typ host")
                                    << QByteArray("candidate:1 1 UDP 2122317823 192.168.1.20 9 typ host");
        QTest::newRow("what follows its type")
            << QByteArray("candidate:1 1 udp 2122317823 192.168.1.20 9 typ host generation 0 ufrag Ab12")
            << QByteArray("candidate:1 1 UDP 2122317823 192.168.1.20 9 typ host");
        QTest::newRow("numbers with noughts first")
            << QByteArray("candidate:F+/9 01 UDP 0007 10.0.0.2 09 typ srflx raddr 10.0.0.9 rport 9")
            << QByteArray("candidate:F+/9 1 UDP 7 10.0.0.2 9 typ srflx");
        QTest::newRow("IPv4 written as IPv6") << QByteArray("candidate:1 1 UDP 1 ::ffff:192.168.1.20 9 typ host")
                                              << QByteArray("candidate:1 1 UDP 1 192.168.1.20 9 typ host");
        QTest::newRow("IPv6 written long") << QByteArray("candidate:1 1 UDP 1 fd00:0:0:0:0:0:0:20 9 typ host")
                                           << QByteArray("candidate:1 1 UDP 1 fd00::20 9 typ host");
        QTest::newRow("in small letters") << QByteArray("candidate:1 1 udp 1 10.0.0.2 9 typ host")
                                          << QByteArray("candidate:1 1 UDP 1 10.0.0.2 9 typ host");
    }
    void anAddressIsHandedOnMadeAnew()
    {
        QFETCH(QByteArray, text);
        QFETCH(QByteArray, handed);
        const core::JudgedAddress judged = core::judgeAddress(text, false);
        QCOMPARE(judged.fault, core::AddressFault::None);
        QCOMPARE(judged.candidate, handed);
    }

    // ... and that is what the library reads of it, not the far end's text (on this PC's loopback, the device
    // on it): its transport as UDP, its numbers as numbers, nothing after its type.
    void theLibraryReadsTheAddressAsItWasJudged()
    {
        JetKvmPeerByHand peer;
        WebRtcOffer offer(jetKvmSetup());
        QVERIFY(offer.answer(peer.answer(awaitOffer(offer))));
        QVERIFY(offer.candidate("candidate:7 01 udp 0212231 127.0.0.1 9 typ host generation 0 ufrag Ab12"));
        const QByteArray held = offer.remoteDescription();
        QVERIFY2(held.contains("a=candidate:7 1 UDP 212231 127.0.0.1 9 typ host"), held.constData());
        QVERIFY2(!held.contains("generation") && !held.contains(" udp "), held.constData());
    }
    void anAddressIsUdpToAHostByItsNumber()
    {
        QFETCH(QByteArray, text);
        QFETCH(bool, loopbackAllowed);
        QFETCH(QByteArray, reason);
        JetKvmPeerByHand peer;
        WebRtcOffer::Setup setup = jetKvmSetup();
        setup.loopbackAllowed = loopbackAllowed;
        WebRtcOffer offer(setup);
        QVERIFY(offer.answer(peer.answer(awaitOffer(offer))));
        QCOMPARE(offer.candidate(text), reason.isEmpty());
        const WebRtcOffer::Refused refused = offer.refused();
        QCOMPARE(refused.candidates, quint64(reason.isEmpty() ? 0 : 1));
        QCOMPARE(refused.malformedCandidates, quint64(reason == "malformed" ? 1 : 0));
        QCOMPARE(refused.candidatesNotUdp, quint64(reason == "notUdp" ? 1 : 0));
        QCOMPARE(refused.namedCandidates, quint64(reason == "named" ? 1 : 0));
        QCOMPARE(refused.unreachableCandidates, quint64(reason == "unreachable" ? 1 : 0));
    }

    // 2e's reading (L3): it is begun once: begun again, after it was closed or not, it refuses.
    void itIsBegunOnce()
    {
        WebRtcOffer offer(jetKvmSetup());
        QVERIFY(!awaitOffer(offer).isEmpty());
        QVERIFY(!offer.begin());
        offer.close();
        QVERIFY(!offer.begin());
    }

    // The answer comes bare and the device's addresses after it, one by one: they are taken, both
    // channels open, and pictures come. (Whatever mid the device gives an address with is its signalling's
    // to read and drop, 2f: here an address is its text alone.)
    void addressesTrickleAfterTheAnswer()
    {
        JetKvmPeerByHand peer;
        QVERIFY(peer.filmLoaded());
        WebRtcOffer offer(jetKvmSetup());
        int pictures = 0;
        connect(&offer, &WebRtcOffer::pictureCame, this, [&](const QByteArray&, bool, qint64) { ++pictures; });
        QVERIFY(connectTo(offer, peer));
        QCOMPARE(peer.labels(), (QList<QByteArray>{"rpc", "hidrpc"}));
        QTRY_VERIFY_WITH_TIMEOUT(pictures >= 30, 15000);
        QVERIFY(!offer.stopped());
    }

    void onlyCandidateTextsAreTaken()
    {
        JetKvmPeerByHand peer;
        WebRtcOffer offer(jetKvmSetup());
        QVERIFY(offer.answer(peer.answer(awaitOffer(offer))));
        QVERIFY(!offer.candidate("a=candidate:1 1 UDP 2122317823 127.0.0.1 9 typ host"));
        QVERIFY(!offer.candidate("1 1 UDP 2122317823 127.0.0.1 9 typ host"));
        QVERIFY(!offer.candidate("candidate:" + QByteArray(2040, '1')));
        QVERIFY(!offer.candidate(QByteArray("candidate:1 1 UDP 2122317823 127.0.0.1 9 typ host\0x", 52)));
        QVERIFY(offer.candidate("candidate:1 1 UDP 2122317823 127.0.0.1 9 typ host"));
        QCOMPARE(offer.refused().candidates, quint64{4});
    }

    void theFortyFirstIsNot()
    {
        JetKvmPeerByHand peer;
        WebRtcOffer offer(jetKvmSetup());
        QVERIFY(offer.answer(peer.answer(awaitOffer(offer))));
        for (int port = 0; port < 40; ++port) {
            QVERIFY(offer.candidate("candidate:1 1 UDP 2122317823 127.0.0.1 " + QByteArray::number(9000 + port) +
                                    " typ host"));
        }
        QVERIFY(!offer.candidate("candidate:1 1 UDP 2122317823 127.0.0.1 9100 typ host"));
        QCOMPARE(offer.refused().candidates, quint64{1});
    }

    // rpc takes text and nothing else, hidrpc binary and nothing else, either way: what is of the other
    // kind is dropped and counted, never handed on; what is sent goes by the channel's kind.
    void rpcTakesTextOnlyAndHidrpcBinaryOnly()
    {
        JetKvmPeerByHand peer;
        WebRtcOffer offer(jetKvmSetup());
        QList<WebRtcOffer::Said> said;
        connect(&offer, &WebRtcOffer::said, this, [&](const WebRtcOffer::Said& word) { said.append(word); });
        QVERIFY(connectTo(offer, peer));
        QVERIFY(peer.sendText("rpc", R"({"jsonrpc":"2.0","method":"videoInputState","params":{"ready":true}})"));
        QVERIFY(peer.sendBinary("rpc", QByteArray::fromHex("0101")));
        QVERIFY(peer.sendBinary("hidrpc", QByteArray::fromHex("0101")));
        QVERIFY(peer.sendText("hidrpc", "text"));
        QTRY_COMPARE(said.size(), 2);
        QTest::qWait(200);
        QCOMPARE(said.size(), 2);
        QCOMPARE(said[0].channel, 0);
        QVERIFY(said[0].bytes.startsWith("{\"jsonrpc\""));
        QCOMPARE(said[1].channel, 1);
        QCOMPARE(said[1].bytes, QByteArray::fromHex("0101"));
        QCOMPARE(offer.refused().wrongKind, quint64{2});
        QVERIFY(offer.send(0, R"({"jsonrpc":"2.0","id":1,"method":"getVideoState"})"));
        QVERIFY(offer.send(1, QByteArray::fromHex("0101")));
        QVERIFY(!offer.send(0, QByteArray("a\0b", 3)));
        QVERIFY(!offer.send(2, "nowhere"));
        QTRY_COMPARE(peer.heard("rpc").size(), 1);
        QTRY_COMPARE(peer.heard("hidrpc").size(), 1);
        QVERIFY(peer.heard("rpc")[0].second);
        QVERIFY(!peer.heard("hidrpc")[0].second);
    }

    // The largest a channel takes is held both ways: 64 bytes on hidrpc, 64 KiB on rpc.
    void theLargestIsHeld()
    {
        JetKvmPeerByHand peer;
        WebRtcOffer offer(jetKvmSetup());
        QList<WebRtcOffer::Said> said;
        connect(&offer, &WebRtcOffer::said, this, [&](const WebRtcOffer::Said& word) { said.append(word); });
        QVERIFY(connectTo(offer, peer));
        QVERIFY(peer.sendBinary("hidrpc", QByteArray(65, '\x09')));
        QVERIFY(peer.sendBinary("hidrpc", QByteArray(64, '\x09')));
        QTRY_COMPARE(said.size(), 1);
        QCOMPARE(said[0].bytes.size(), 64);
        QTRY_COMPARE(offer.refused().tooLarge, quint64{1});
        QVERIFY(!offer.send(1, QByteArray(65, '\x09')));
        QVERIFY(offer.send(1, QByteArray(64, '\x09')));
        QVERIFY(!offer.send(0, QByteArray(65537, 'x')));
        QVERIFY(offer.send(0, QByteArray(65536, 'x')));
        QTRY_COMPARE(peer.heard("rpc").size(), 1);
        QCOMPARE(peer.heard("rpc")[0].first.size(), 65536);
        // 2e's reading (L1): one byte over on rpc is refused too, and not taken cut to 64 KiB as if whole (the
        // library cuts what is larger than the most a message may be, which is now one over the largest).
        QVERIFY(peer.sendText("rpc", QByteArray(65537, 'x')));
        QVERIFY(peer.sendText("rpc", QByteArray(65536, 'y')));
        QTRY_COMPARE(said.size(), 2);
        QCOMPARE(said[1].bytes, QByteArray(65536, 'y'));
        QCOMPARE(offer.refused().tooLarge, quint64{2});
    }

    // Each picture is stamped as it became whole, in the box, on the library's thread.
    void picturesAreStampedInTheBox()
    {
        JetKvmPeerByHand peer;
        WebRtcOffer offer(jetKvmSetup());
        QList<qint64> stamps;
        int keys = 0;
        connect(&offer, &WebRtcOffer::pictureCame, this, [&](const QByteArray& annexB, bool key, qint64 arrivedUs) {
            stamps.append(arrivedUs);
            keys += key ? 1 : 0;
            QVERIFY(!key || (annexB.startsWith(QByteArray::fromHex("00000001")) && (annexB.at(4) & 0x1F) == 7));
        });
        QVERIFY(connectTo(offer, peer));
        QTRY_VERIFY_WITH_TIMEOUT(stamps.size() >= 40 && keys >= 1, 15000);
        for (qsizetype at = 1; at < stamps.size(); ++at) {
            QVERIFY(stamps[at] >= stamps[at - 1] && stamps[at] > 0);
        }
        QVERIFY(offer.lastWholePictureUs() >= stamps.back());
        QVERIFY(offer.counts().whole >= quint64(stamps.size()));
    }

    // The stop word is read as it comes, on the library's thread: with the Qt loop stalled, stopped() says
    // so before any drain, and nothing more is sent.
    void theStopWordIsRaisedOnTheLibrarysThread()
    {
        JetKvmPeerByHand peer;
        WebRtcOffer offer(jetKvmSetup());
        QList<WebRtcOffer::Said> said;
        connect(&offer, &WebRtcOffer::said, this, [&](const WebRtcOffer::Said& word) { said.append(word); });
        QVERIFY(connectTo(offer, peer));
        QVERIFY(!offer.stopped());
        QVERIFY(peer.sendText("rpc", R"({"jsonrpc":"2.0","method":"otherSessionConnected","params":null})"));
        const qint64 until = base::steadyNowUs() + 3'000'000;
        bool stoppedInTheStall = false;
        while (base::steadyNowUs() < until && !stoppedInTheStall) {
            stoppedInTheStall = offer.stopped();
        }
        QVERIFY(stoppedInTheStall);
        QVERIFY(said.isEmpty());
        QVERIFY(!offer.send(0, R"({"jsonrpc":"2.0","id":1,"method":"getVideoState"})"));
        QTRY_COMPARE(said.size(), 1);
    }

    // More of what the channels say than may wait for the interface thread: it ends, and says so.
    void aFloodEndsIt()
    {
        JetKvmPeerByHand peer;
        WebRtcOffer::Setup setup = jetKvmSetup();
        setup.mostWaiting = 5;
        WebRtcOffer offer(setup);
        QSignalSpy failed(&offer, &WebRtcOffer::failed);
        QVERIFY(connectTo(offer, peer));
        for (int each = 0; each < 10; ++each) {
            QVERIFY(peer.sendText("rpc", R"({"jsonrpc":"2.0","method":"usbState","params":"configured"})"));
        }
        busyFor(500);
        QTRY_COMPARE(failed.size(), 1);
        QVERIFY(offer.stopped());
        // 2e's reading (L4): once it ended, nothing more is taken or said, and it says so once: what the far
        // end says after that, and its going, are nothing.
        QSignalSpy said(&offer, &WebRtcOffer::said);
        for (int each = 0; each < 10; ++each) {
            QVERIFY(peer.sendText("rpc", R"({"jsonrpc":"2.0","method":"usbState","params":"configured"})"));
        }
        QTest::qWait(500);
        peer.stop();
        QTest::qWait(2000);
        QCOMPARE(said.size(), 0);
        QCOMPARE(failed.size(), 1);
    }

    // A channel the far end opens is no channel of this one: deleted, counted.
    void aRemoteChannelIsNotTaken()
    {
        JetKvmPeerByHand peer;
        WebRtcOffer offer(jetKvmSetup());
        QSignalSpy opened(&offer, &WebRtcOffer::channelOpened);
        QVERIFY(connectTo(offer, peer));
        QVERIFY(peer.openOwnChannel("rogue") >= 0);
        QTRY_COMPARE(offer.refused().remoteChannels, quint64{1});
        QTest::qWait(200);
        QCOMPARE(opened.size(), 2);
    }

    // Closed again and again while it connects: nothing comes after, nothing is touched that went (an
    // address sanitizer run says the rest).
    void closingWhileConnecting()
    {
        JetKvmPeerByHand peer;
        peer.everyMs = 0;
        for (int round = 0; round < 20; ++round) {
            auto offer = std::make_unique<WebRtcOffer>(jetKvmSetup());
            const QByteArray sdp = awaitOffer(*offer);
            QVERIFY(!sdp.isEmpty());
            QVERIFY(offer->answer(peer.answer(sdp)));
            for (const QByteArray& address : peer.candidates()) {
                offer->candidate(address);
            }
            QTest::qWait(round % 5);
            QSignalSpy said(offer.get(), &WebRtcOffer::said);
            offer->close();
            offer->close();
            QTest::qWait(5);
            QCOMPARE(said.size(), 0);
            QVERIFY(offer->stopped());
            QVERIFY(!offer->send(0, "{}"));
            offer.reset();
        }
    }

    // M-D4 for the offering side: key frames asked and rpc messages sent every millisecond for 20 s,
    // against pictures at 60 a second and the peer's messages, with a watchdog on the Qt loop: the
    // library is never called with a lock of ours held.
    void theLibraryIsNeverCalledUnderAMailboxLock()
    {
        const int before = WebRtcOffer::libraryCallsUnderOwnLock();
        JetKvmPeerByHand peer;
        WebRtcOffer offer(jetKvmSetup());
        int pictures = 0;
        connect(&offer, &WebRtcOffer::pictureCame, this, [&](const QByteArray&, bool, qint64) { ++pictures; });
        QVERIFY(connectTo(offer, peer));
        std::atomic<qint64> beat = base::steadyNowUs();
        std::atomic<bool> finished = false;
        std::thread watchdog([&] {
            while (!finished) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                if (!finished && base::steadyNowUs() - beat > 10'000'000) {
                    std::fputs("M-D4 watchdog: the Qt loop has not turned for 10 s; the offer is deadlocked\n", stderr);
                    std::fflush(stderr);
                    std::_Exit(3);
                }
            }
        });
        QTimer asking;
        asking.setTimerType(Qt::PreciseTimer);
        asking.setInterval(1);
        int asked = 0;
        connect(&asking, &QTimer::timeout, this, [&] {
            beat = base::steadyNowUs();
            for (int each = 0; each < 16; ++each) {
                offer.requestKey();
                offer.send(0, R"({"jsonrpc":"2.0","id":1,"method":"getVideoState"})");
                ++asked;
            }
            peer.sendText("rpc", R"({"jsonrpc":"2.0","method":"usbState","params":"configured"})");
        });
        QElapsedTimer ran;
        ran.start();
        asking.start();
        while (ran.elapsed() < 20'000) {
            QTest::qWait(100);
        }
        asking.stop();
        finished = true;
        watchdog.join();
        qInfo() << "asked:" << asked << "pictures:" << pictures << "in" << ran.elapsed() << "ms";
        QVERIFY2(asked > 10'000, qPrintable(QString::number(asked)));
        QVERIFY2(pictures > 300, qPrintable(QString::number(pictures)));
        offer.close();
        QCOMPARE(WebRtcOffer::libraryCallsUnderOwnLock() - before, 0);
    }
};
QTEST_GUILESS_MAIN(TestWebRtcOffer)
#include "test_webrtc_offer.moc"
