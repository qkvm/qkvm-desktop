#include "rtc_sdp.h"
#include "rtc_seam.h"
#include <qkvm/base/clock.h>
#include <qkvm/core/picture_box.h>
#include <qkvm/core/video_diagnostics.h>
#include <qkvm/core/webrtc_receiver.h>

#include <QMetaObject>
#include <memory>
#include <mutex>
#include <optional>
#include <rtc/rtc.h>
#include <utility>

using namespace Qt::StringLiterals;
namespace qkvm::core {
namespace {
constexpr int largestSdp = 64 * 1024;
constexpr qsizetype largestCandidate = 2048;
// A Comet's own pace: a key frame asked for twice a second at most, as before the box (M6 2b).
constexpr PictureBox::Pace cometPace{8, 500};
} // namespace

using rtcseam::Held;
using rtcseam::intoLibrary;

struct WebRtcReceiver::Mailbox {
    std::mutex mutex;
    WebRtcReceiver* target = nullptr; // read only while mutex is held; cleared before QObject destruction
    PictureBox pictures{96, cometPace};
    QByteArray answer;
    QString error;
    bool posted = false;
    bool wantKey = false;
    int track = -1;

    using Known = rtcseam::Registry<Mailbox>;
    void post()
    {
        if (target != nullptr && !posted) {
            posted = true;
            QMetaObject::invokeMethod(target, &WebRtcReceiver::drain, Qt::QueuedConnection);
        }
    }
    static void state(int id, rtcState state, void*)
    {
        VideoDiagnostics::record("rtc_state", {{"peer", id}, {"state", static_cast<int>(state)}});
        if (state != RTC_FAILED && state != RTC_DISCONNECTED && state != RTC_CLOSED) {
            return;
        }
        if (const auto box = Known::find(id)) {
            const Held lock(box->mutex);
            if (box->target != nullptr) {
                box->error = u"The native video connection was lost."_s;
                box->post();
            }
        }
    }
    static void gathered(int id, rtcGatheringState state, void*)
    {
        VideoDiagnostics::record("rtc_gathering", {{"peer", id}, {"state", static_cast<int>(state)}});
        if (state != RTC_GATHERING_COMPLETE) {
            return;
        }
        if (const auto box = Known::find(id)) {
            {
                const Held lock(box->mutex);
                if (box->target == nullptr) {
                    return;
                }
            }
            // The answer is asked of the library with no mailbox lock held (M-D4), and handed in under it.
            QByteArray answer;
            QString error;
            intoLibrary();
            const int length = rtcGetLocalDescription(id, nullptr, 0);
            if (length <= 0 || length > largestSdp) {
                error = u"The native video answer is invalid."_s;
            } else {
                answer = QByteArray(length, '\0');
                intoLibrary();
                if (rtcGetLocalDescription(id, answer.data(), length) < 0) {
                    error = u"The native video answer could not be made."_s;
                } else {
                    answer.truncate(length - 1);
                }
            }
            const Held lock(box->mutex);
            if (box->target == nullptr) {
                return;
            }
            if (!error.isEmpty()) {
                box->error = error;
            } else {
                box->answer = std::move(answer);
            }
            box->post();
        }
    }
    static void message(int id, const char* bytes, int length, void*)
    {
        if (const auto box = Known::find(id)) {
            // Stamped as it came, on the library's thread: a picture it makes whole arrived now (2a).
            const qint64 now = base::steadyNowUs();
            const Held lock(box->mutex);
            if (box->target == nullptr) {
                return;
            }
            // (What is no packet, a text or larger than the library's own largest, is a loss.)
            const QByteArrayView rtp = length > 0 && length <= 65536 ? QByteArrayView(bytes, length) : QByteArrayView();
            const bool waiting = box->pictures.packet(rtp, now);
            if (box->pictures.lostAndAsks(now)) {
                box->wantKey = true;
            }
            if (waiting || box->wantKey) {
                box->post();
            }
        }
    }
};

WebRtcReceiver::WebRtcReceiver(Setup setup, QObject* parent) : QObject(parent), m_setup(setup)
{
    m_wishWaits.setSingleShot(true);
    connect(&m_wishWaits, &QTimer::timeout, this, &WebRtcReceiver::askAWishThatWaits);
}
WebRtcReceiver::~WebRtcReceiver()
{
    close();
}
bool WebRtcReceiver::supports(const QByteArray& sdp)
{
    return chooseVideo(sdp).has_value();
}

bool WebRtcReceiver::offer(const QByteArray& sdp)
{
    if (m_peer >= 0) {
        return false;
    }
    // Its own addresses are taken out of it and go the way every address of the far end's goes, after it
    // (judged and counted, as a JetKVM's: 852bf06). The library would take those inside a description
    // straight, beyond any count, and look up the names among them.
    const WithoutAddresses parts = withoutAddresses(sdp);
    const auto chosen = chooseVideo(parts.rest);
    if (!chosen) {
        VideoDiagnostics::record("rtc_offer_rejected", {{"bytes", sdp.size()},
                                                        {"has_h264", sdp.contains("H264/90000")},
                                                        {"has_mode1", sdp.contains("packetization-mode=1")},
                                                        {"has_video", sdp.contains("m=video ")},
                                                        {"has_fingerprint", sdp.contains("a=fingerprint:sha-256 ")}});
        return false;
    }
    VideoDiagnostics::record("rtc_offer_selected", {{"payload", chosen->payload}});
    m_mailbox = std::make_shared<Mailbox>();
    m_mailbox->target = this;
    m_mailbox->pictures = PictureBox(chosen->payload, cometPace);
    if (!chosen->sps.isEmpty()) {
        m_mailbox->pictures.parameters(chosen->sps, chosen->pps);
    }
    rtcConfiguration config{};
    config.disableAutoNegotiation = true;
    config.forceMediaTransport = true;
    config.maxMessageSize = 65536;
    // No public STUN/TURN servers or credentials. Device candidates are learned through
    // the authenticated signaling connection; DTLS fingerprint verification stays enabled.
    intoLibrary();
    m_peer = rtcCreatePeerConnection(&config);
    if (m_peer < 0) {
        close();
        return false;
    }
    Mailbox::Known::remember(m_peer, m_mailbox);
    // (Each call into the library on a line of its own, said to the seam first: test_review_guards.ps1
    // holds every one in this file to that.)
    intoLibrary();
    rtcSetStateChangeCallback(m_peer, &Mailbox::state);
    intoLibrary();
    rtcSetGatheringStateChangeCallback(m_peer, &Mailbox::gathered);
    intoLibrary();
    rtcSetTrackCallback(m_peer, [](int, int track, void*) {
        intoLibrary();
        rtcDeleteTrack(track);
    });
    intoLibrary();
    rtcSetDataChannelCallback(m_peer, [](int, int channel, void*) {
        intoLibrary();
        rtcDeleteDataChannel(channel);
    });
    const QByteArray trackSdp = "m=video 9 UDP/TLS/RTP/SAVPF " + QByteArray::number(chosen->payload) +
                                "\r\na=mid:" + chosen->mid +
                                "\r\na=recvonly\r\na=rtcp-mux\r\na=rtpmap:" + QByteArray::number(chosen->payload) +
                                " H264/90000\r\na=fmtp:" + QByteArray::number(chosen->payload) + " " + chosen->format +
                                "\r\na=rtcp-fb:" + QByteArray::number(chosen->payload) + " nack pli\r\n";
    intoLibrary();
    m_track = rtcAddTrack(m_peer, trackSdp.constData());
    if (m_track < 0) {
        close();
        return false;
    }
    m_mailbox->track = m_track;
    Mailbox::Known::remember(m_track, m_mailbox);
    intoLibrary();
    bool ready = rtcChainRtcpReceivingSession(m_track) >= 0;
    intoLibrary();
    ready = ready && rtcSetMessageCallback(m_track, &Mailbox::message) >= 0;
    intoLibrary();
    ready = ready && rtcSetRemoteDescription(m_peer, parts.rest.constData(), "offer") >= 0;
    // (Where the library adds a description's own, after it and before the answer.)
    for (qsizetype each = 0; ready && each < parts.candidates.size(); ++each) {
        candidate(parts.candidates[each]);
    }
    intoLibrary();
    ready = ready && rtcSetLocalDescription(m_peer, "answer") >= 0;
    if (!ready) {
        close();
        return false;
    }
    return true;
}

bool WebRtcReceiver::candidate(const QByteArray& candidate)
{
    if (m_peer < 0) {
        return false;
    }
    const JudgedAddress judged = candidate.size() > largestCandidate || candidate.contains('\0')
                                     ? JudgedAddress{}
                                     : judgeAddress(candidate, m_setup.loopbackAllowed);
    // (Only those taken count toward the most: before, every one that came did, a refused one among them.)
    const bool beyond = judged.fault == AddressFault::None && m_candidates >= m_setup.mostCandidates;
    if (judged.fault != AddressFault::None || beyond) {
        ++m_refused.candidates;
        m_refused.malformedCandidates += judged.fault == AddressFault::Malformed ? 1 : 0;
        m_refused.candidatesNotUdp += judged.fault == AddressFault::NotUdp ? 1 : 0;
        m_refused.namedCandidates += judged.fault == AddressFault::Named ? 1 : 0;
        m_refused.unreachableCandidates += judged.fault == AddressFault::Unreachable ? 1 : 0;
        m_refused.candidatesBeyond += beyond ? 1 : 0;
        return false;
    }
    ++m_candidates;
    // As it was judged, made anew, and with no mid: the library takes the bundle's (Janus's is not trusted).
    intoLibrary();
    return rtcAddRemoteCandidate(m_peer, judged.candidate.constData(), nullptr) >= 0;
}

void WebRtcReceiver::requestKey()
{
    if (m_track < 0 || !m_mailbox) {
        return;
    }
    // A window's wish (design 3.2, M2): asked at the box's pace, and nothing it holds or is putting
    // together is thrown away for it, so that on a shared connection the other windows go on.
    bool ask = false;
    qint64 leftUs = 0;
    {
        const Held lock(m_mailbox->mutex);
        const qint64 now = base::steadyNowUs();
        ask = m_mailbox->pictures.mayAsk(now);
        leftUs = ask ? 0 : m_mailbox->pictures.paceLeftUs(now);
    }
    if (!ask) {
        // Kept by the box, and asked at the next packet once the pace has passed; and by a timer then,
        // where no packet comes (a screen that stands still: 2c, the second reading).
        if (!m_wishWaits.isActive()) {
            m_wishWaits.start(static_cast<int>(leftUs / 1000) + 1);
        }
        return;
    }
    // Asked of the library with no mailbox lock held (M-D4).
    intoLibrary();
    if (rtcIsOpen(m_track)) {
        intoLibrary();
        rtcRequestKeyframe(m_track);
    }
}

quint64 WebRtcReceiver::picturesLetGo() const
{
    // As it stands, not as of the last drain (2c, the second reading); once closed, as it was then.
    if (m_mailbox) {
        const Held lock(m_mailbox->mutex);
        return m_mailbox->pictures.counts().letGoBehind;
    }
    return m_letGo;
}

void WebRtcReceiver::askAWishThatWaits()
{
    if (m_track < 0 || !m_mailbox) {
        return;
    }
    bool ask = false;
    qint64 leftUs = 0;
    {
        const Held lock(m_mailbox->mutex);
        const qint64 now = base::steadyNowUs();
        ask = m_mailbox->pictures.wishAsks(now);
        leftUs = ask ? 0 : m_mailbox->pictures.paceLeftUs(now);
    }
    if (!ask) {
        // Not yet (a loss asked meanwhile), or answered already (a key frame came, or a packet asked it).
        if (leftUs > 0) {
            m_wishWaits.start(static_cast<int>(leftUs / 1000) + 1);
        }
        return;
    }
    intoLibrary();
    if (rtcIsOpen(m_track)) {
        intoLibrary();
        rtcRequestKeyframe(m_track);
    }
}

int WebRtcReceiver::libraryCallsUnderOwnLock()
{
    return rtcseam::callsUnderOwnLock();
}

QByteArray WebRtcReceiver::remoteDescription() const
{
    if (m_peer < 0) {
        return {};
    }
    intoLibrary();
    const int length = rtcGetRemoteDescription(m_peer, nullptr, 0);
    if (length <= 0 || length > 4 * largestSdp) {
        return {};
    }
    QByteArray sdp(length, '\0');
    intoLibrary();
    if (rtcGetRemoteDescription(m_peer, sdp.data(), length) < 0) {
        return {};
    }
    sdp.truncate(length - 1);
    return sdp;
}

void WebRtcReceiver::drain()
{
    if (!m_mailbox) {
        return;
    }
    QList<PictureBox::Picture> pictures;
    QByteArray answer;
    QString error;
    bool wantKey = false;
    PictureBox::Counts counts;
    {
        const Held lock(m_mailbox->mutex);
        m_mailbox->posted = false;
        pictures = m_mailbox->pictures.take();
        answer = std::exchange(m_mailbox->answer, {});
        error = std::exchange(m_mailbox->error, {});
        wantKey = std::exchange(m_mailbox->wantKey, false);
        counts = m_mailbox->pictures.counts();
    }
    // What the box let go is said here, with no lock of ours held (nothing that writes may stall the
    // library's thread), and to whoever shows the machine (picturesLetGo).
    if (counts.letGoBehind != m_letGo) {
        m_letGo = counts.letGoBehind;
        VideoDiagnostics::record("rtc_let_go", {{"pictures", static_cast<qint64>(counts.letGoBehind)},
                                                {"runs", static_cast<qint64>(counts.runsLost)},
                                                {"key_asks", static_cast<qint64>(counts.keyAsks)}});
    }
    if (!error.isEmpty()) {
        emit failed(error);
        return;
    }
    if (wantKey && m_track >= 0) {
        intoLibrary();
        if (rtcIsOpen(m_track)) {
            intoLibrary();
            rtcRequestKeyframe(m_track);
        }
    }
    if (!answer.isEmpty()) {
        emit answerReady(answer);
    }
    for (const PictureBox::Picture& picture : pictures) {
        if (!m_mailbox) {
            break;
        }
        emit pictureCame(picture.bytes, picture.key, picture.arrivedUs);
    }
}

void WebRtcReceiver::close()
{
    m_wishWaits.stop();
    if (m_mailbox) {
        const Held lock(m_mailbox->mutex);
        m_mailbox->target = nullptr;
        m_letGo = m_mailbox->pictures.counts().letGoBehind;
    }
    if (m_track >= 0) {
        Mailbox::Known::forget(m_track);
        intoLibrary();
        rtcDeleteTrack(std::exchange(m_track, -1));
    }
    if (m_peer >= 0) {
        Mailbox::Known::forget(m_peer);
        intoLibrary();
        rtcDeletePeerConnection(std::exchange(m_peer, -1));
    }
    m_mailbox.reset();
}
} // namespace qkvm::core
