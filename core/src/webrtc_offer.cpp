#include "rtc_sdp.h"
#include "rtc_seam.h"
#include <qkvm/base/clock.h>
#include <qkvm/core/video_diagnostics.h>
#include <qkvm/core/webrtc_offer.h>

#include <QHash>
#include <QMetaObject>
#include <atomic>
#include <memory>
#include <mutex>
#include <rtc/rtc.h>
#include <utility>

using namespace Qt::StringLiterals;
namespace qkvm::core {
namespace {
constexpr int largestSdp = 64 * 1024;
constexpr int largestCandidate = 2048;
constexpr int firstPayload = 96;
// The mid its video is offered under (begin()).
constexpr const char* offeredVideoMid = "0";
} // namespace

using rtcseam::Held;
using rtcseam::intoLibrary;

struct WebRtcOffer::Mailbox {
    explicit Mailbox(const Setup& setup)
        : pictures(firstPayload, setup.pictures), channels(setup.channels), stopsOn(setup.stopsOn),
          mostWaiting(setup.mostWaiting), mostWaitingBytes(setup.mostWaitingBytes)
    {}

    std::mutex mutex;
    WebRtcOffer* target = nullptr; // read only while mutex is held; cleared before QObject destruction
    PictureBox pictures;
    bool answered = false;
    // It ended (lost, flooded, no offer): nothing more is taken, and that is said once (2e's reading).
    bool over = false;
    QByteArray offer;
    QString error;
    QList<Said> words;
    qint64 wordsBytes = 0;
    QList<std::pair<int, bool>> channelEvents; // a channel's place, and whether it opened or closed
    QHash<int, int> channelOf;                 // the library's id of a channel: its place
    Refused refused;
    bool posted = false;
    bool wantKey = false;
    // Set before any callback can come, and never changed after: read without the lock.
    const QList<Channel> channels;
    const std::function<bool(QByteArrayView)> stopsOn;
    const int mostWaiting;
    const qint64 mostWaitingBytes;
    // The stop word was read, or it ended: raised on the library's thread, read by whoever sends.
    std::atomic<bool> stopped{false};

    using Known = rtcseam::Registry<Mailbox>;
    void post()
    {
        if (target != nullptr && !posted) {
            posted = true;
            QMetaObject::invokeMethod(target, &WebRtcOffer::drain, Qt::QueuedConnection);
        }
    }
    // With the lock held. Once: what ends it again after that is nothing.
    void end(const QString& why)
    {
        stopped.store(true, std::memory_order_release);
        if (std::exchange(over, true)) {
            return;
        }
        error = why;
        post();
    }
    static void state(int id, rtcState state, void*)
    {
        VideoDiagnostics::record("rtc_offer_state", {{"peer", id}, {"state", static_cast<int>(state)}});
        if (state != RTC_FAILED && state != RTC_DISCONNECTED && state != RTC_CLOSED) {
            return;
        }
        if (const auto box = Known::find(id)) {
            const Held lock(box->mutex);
            if (box->target != nullptr) {
                box->end(u"The WebRTC connection was lost."_s); // (once: end says it once)
            }
        }
    }
    static void gathered(int id, rtcGatheringState state, void*)
    {
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
            // Asked of the library with no mailbox lock held (M-D4), and handed in under it.
            QByteArray offer;
            intoLibrary();
            const int length = rtcGetLocalDescription(id, nullptr, 0);
            if (length > 0 && length <= largestSdp) {
                offer = QByteArray(length, '\0');
                intoLibrary();
                if (rtcGetLocalDescription(id, offer.data(), length) < 0) {
                    offer.clear();
                } else {
                    offer.truncate(length - 1);
                }
            }
            const Held lock(box->mutex);
            if (box->target == nullptr) {
                return;
            }
            if (offer.isEmpty()) {
                box->end(u"The WebRTC offer could not be made."_s);
            } else {
                box->offer = std::move(offer);
                box->post();
            }
        }
    }
    static void picture(int id, const char* bytes, int length, void*)
    {
        if (const auto box = Known::find(id)) {
            // Stamped as it came, on the library's thread (2a).
            const qint64 now = base::steadyNowUs();
            const Held lock(box->mutex);
            if (box->target == nullptr || !box->answered || box->over) {
                return;
            }
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
    static void opened(int id, void*) { told(id, true); }
    static void closed(int id, void*) { told(id, false); }
    static void told(int id, bool open)
    {
        if (const auto box = Known::find(id)) {
            const Held lock(box->mutex);
            const int place = box->channelOf.value(id, -1);
            if (box->target != nullptr && place >= 0 && !box->over) {
                box->channelEvents.append({place, open});
                box->post();
            }
        }
    }
    static void message(int id, const char* bytes, int size, void*)
    {
        if (const auto box = Known::find(id)) {
            const qint64 now = base::steadyNowUs();
            // Text comes with its size as -(n+1) (the C API's own way).
            const bool text = size < 0;
            const qsizetype length = text ? static_cast<qsizetype>(-(qint64{size} + 1)) : size;
            if (bytes == nullptr || length < 0) {
                return;
            }
            // The stop word, before the lock: read here, on the library's thread, as it comes, so that
            // whoever sends knows before any drain (design 3.3).
            if (text && box->stopsOn && length <= 65536 && box->stopsOn(QByteArrayView(bytes, length))) {
                box->stopped.store(true, std::memory_order_release);
            }
            const Held lock(box->mutex);
            const int place = box->channelOf.value(id, -1);
            if (box->target == nullptr || place < 0 || box->over) {
                return;
            }
            const Channel& channel = box->channels[place];
            if (channel.text != text) {
                ++box->refused.wrongKind;
                return;
            }
            if (length > channel.largest) {
                ++box->refused.tooLarge;
                return;
            }
            if (box->words.size() >= box->mostWaiting || box->wordsBytes + length > box->mostWaitingBytes) {
                box->end(u"The far end sent more than is taken."_s);
                return;
            }
            box->words.append({place, QByteArray(bytes, length), now});
            box->wordsBytes += length;
            box->post();
        }
    }
    static void remoteChannel(int pc, int channel, void*)
    {
        if (const auto box = Known::find(pc)) {
            const Held lock(box->mutex);
            ++box->refused.remoteChannels;
        }
        // What the far end opens is no channel of ours: deleted, with no lock of ours held (M-D4).
        intoLibrary();
        rtcDeleteDataChannel(channel);
    }
};

WebRtcOffer::WebRtcOffer(Setup setup, QObject* parent) : QObject(parent), m_setup(std::move(setup))
{
    m_gather.setSingleShot(true);
    connect(&m_gather, &QTimer::timeout, this, &WebRtcOffer::sayTheOffer);
    m_wishWaits.setSingleShot(true);
    connect(&m_wishWaits, &QTimer::timeout, this, &WebRtcOffer::askAWishThatWaits);
}

WebRtcOffer::~WebRtcOffer()
{
    close();
}

bool WebRtcOffer::begin()
{
    // (Once: begun again, closed or not, it is no new session; 2e's reading.)
    if (std::exchange(m_begun, true) || m_setup.h264Formats.isEmpty() || m_setup.h264Formats.size() > 8) {
        return false;
    }
    m_mailbox = std::make_shared<Mailbox>(m_setup);
    m_mailbox->target = this;
    rtcConfiguration config{};
    config.disableAutoNegotiation = true;
    config.forceMediaTransport = true;
    // One over the largest any channel takes: the library cuts a larger message down to this, so that what
    // is larger than a channel takes still arrives larger, and is refused, never cut to look whole (2e's
    // reading: at 64 KiB, a megabyte on rpc came as its first 64 KiB).
    qsizetype largest = 0;
    for (const Channel& channel : std::as_const(m_setup.channels)) {
        largest = std::max(largest, channel.largest);
    }
    config.maxMessageSize = static_cast<int>(std::min<qsizetype>(largest, qsizetype{1} << 24) + 1);
    // No STUN or TURN server, and no credentials: host addresses only, learned from and given to the
    // far end over its signalling line (design 7). DTLS fingerprints are checked.
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
    rtcSetDataChannelCallback(m_peer, &Mailbox::remoteChannel);
    // Video it only receives: H.264 in each format handed in, each with PLI, and no RTX (the
    // assembler does not put it together, h264_rtp.h: a loss is mended by the next key frame).
    QByteArray trackSdp = "m=video 9 UDP/TLS/RTP/SAVPF";
    m_offered.clear();
    for (qsizetype format = 0; format < m_setup.h264Formats.size(); ++format) {
        m_offered.append(firstPayload + static_cast<int>(format));
        trackSdp += ' ' + QByteArray::number(m_offered.back());
    }
    trackSdp += "\r\na=mid:" + QByteArray(offeredVideoMid) + "\r\na=recvonly\r\na=rtcp-mux\r\n";
    for (qsizetype format = 0; format < m_setup.h264Formats.size(); ++format) {
        const QByteArray payload = QByteArray::number(m_offered[format]);
        trackSdp += "a=rtpmap:" + payload + " H264/90000\r\na=fmtp:" + payload + ' ' + m_setup.h264Formats[format] +
                    "\r\na=rtcp-fb:" + payload + " nack pli\r\n";
    }
    intoLibrary();
    m_track = rtcAddTrack(m_peer, trackSdp.constData());
    if (m_track < 0) {
        close();
        return false;
    }
    Mailbox::Known::remember(m_track, m_mailbox);
    intoLibrary();
    bool ready = rtcChainRtcpReceivingSession(m_track) >= 0;
    intoLibrary();
    ready = ready && rtcSetMessageCallback(m_track, &Mailbox::picture) >= 0;
    // The channels it opens itself, reliable and in order, each with its callbacks.
    for (qsizetype place = 0; ready && place < m_setup.channels.size(); ++place) {
        rtcDataChannelInit init{};
        intoLibrary();
        const int id = rtcCreateDataChannelEx(m_peer, m_setup.channels[place].label.constData(), &init);
        if (id < 0) {
            ready = false;
            break;
        }
        m_channels.append(id);
        {
            const Held lock(m_mailbox->mutex);
            m_mailbox->channelOf.insert(id, static_cast<int>(place));
        }
        Mailbox::Known::remember(id, m_mailbox);
        intoLibrary();
        ready = rtcSetOpenCallback(id, &Mailbox::opened) >= 0;
        intoLibrary();
        ready = ready && rtcSetClosedCallback(id, &Mailbox::closed) >= 0;
        intoLibrary();
        ready = ready && rtcSetMessageCallback(id, &Mailbox::message) >= 0;
    }
    intoLibrary();
    ready = ready && rtcSetLocalDescription(m_peer, "offer") >= 0;
    if (!ready) {
        close();
        return false;
    }
    m_gather.start(m_setup.gatherWithinMs);
    return true;
}

bool WebRtcOffer::answer(const QByteArray& sdp)
{
    if (m_peer < 0 || !m_mailbox || m_applicationMid.isEmpty() || sdp.size() > largestSdp || sdp.contains('\0')) {
        return false;
    }
    // Its own addresses go the way every one goes, after it (2e's reading).
    const WithoutAddresses parts = withoutAddresses(sdp);
    const auto chosen = chooseVideo(parts.rest, m_offered);
    // Each under the mid it was offered under (2e's reading: another was taken).
    const bool video = chosen && chosen->mid == offeredVideoMid;
    const bool application = applicationMid(parts.rest) == m_applicationMid;
    if (!video || !application) {
        VideoDiagnostics::record("rtc_answer_refused",
                                 {{"bytes", sdp.size()}, {"video", video}, {"application", application}});
        return false;
    }
    {
        const Held lock(m_mailbox->mutex);
        if (m_mailbox->answered) {
            return false;
        }
        m_mailbox->pictures = PictureBox(chosen->payload, m_setup.pictures);
        if (!chosen->sps.isEmpty()) {
            m_mailbox->pictures.parameters(chosen->sps, chosen->pps);
        }
        m_mailbox->answered = true;
    }
    VideoDiagnostics::record("rtc_answer_taken",
                             {{"payload", chosen->payload}, {"addresses", parts.candidates.size()}});
    intoLibrary();
    if (rtcSetRemoteDescription(m_peer, parts.rest.constData(), "answer") < 0) {
        return false;
    }
    for (const QByteArray& address : parts.candidates) {
        candidate(address);
    }
    return true;
}

bool WebRtcOffer::candidate(const QByteArray& text)
{
    if (m_peer < 0 || !m_mailbox) {
        return false;
    }
    const JudgedAddress judged = text.size() > largestCandidate || text.contains('\0')
                                     ? JudgedAddress{}
                                     : judgeAddress(text, m_setup.loopbackAllowed);
    const AddressFault fault = judged.fault;
    const bool beyond = fault == AddressFault::None && m_candidates >= m_setup.mostCandidates;
    if (fault != AddressFault::None || beyond) {
        const Held lock(m_mailbox->mutex);
        Refused& refused = m_mailbox->refused;
        ++refused.candidates;
        refused.malformedCandidates += fault == AddressFault::Malformed ? 1 : 0;
        refused.candidatesNotUdp += fault == AddressFault::NotUdp ? 1 : 0;
        refused.namedCandidates += fault == AddressFault::Named ? 1 : 0;
        refused.unreachableCandidates += fault == AddressFault::Unreachable ? 1 : 0;
        refused.candidatesBeyond += beyond ? 1 : 0;
        return false;
    }
    ++m_candidates;
    // As it was judged, made anew (never the far end's own text: 2e, the second reading). Its mid is the
    // bundle's, which the library takes where none is given: the far end's is not trusted.
    intoLibrary();
    return rtcAddRemoteCandidate(m_peer, judged.candidate.constData(), nullptr) >= 0;
}

bool WebRtcOffer::send(int channel, const QByteArray& bytes)
{
    if (stopped()) {
        return false;
    }
    return sendLettingGo(channel, bytes);
}

bool WebRtcOffer::sendLettingGo(int channel, const QByteArray& bytes)
{
    if (!m_mailbox || channel < 0 || channel >= m_channels.size()) {
        return false;
    }
    const Channel& kind = m_setup.channels[channel];
    if (bytes.size() > kind.largest || (kind.text && bytes.contains('\0'))) {
        return false;
    }
    const int id = m_channels[channel];
    intoLibrary();
    if (!rtcIsOpen(id)) {
        return false;
    }
    // Text as a string (its size said as below nought), binary by its size.
    const int size = kind.text ? -1 : static_cast<int>(bytes.size());
    intoLibrary();
    return rtcSendMessage(id, bytes.constData(), size) >= 0;
}

qint64 WebRtcOffer::bufferedAmount(int channel) const
{
    if (!m_mailbox || channel < 0 || channel >= m_channels.size()) {
        return 0;
    }
    intoLibrary();
    const int amount = rtcGetBufferedAmount(m_channels[channel]);
    return amount > 0 ? amount : 0;
}

void WebRtcOffer::requestKey()
{
    if (m_track < 0 || !m_mailbox) {
        return;
    }
    bool ask = false;
    qint64 leftUs = 0;
    {
        const Held lock(m_mailbox->mutex);
        const qint64 now = base::steadyNowUs();
        ask = m_mailbox->pictures.mayAsk(now);
        leftUs = ask ? 0 : m_mailbox->pictures.paceLeftUs(now);
    }
    if (!ask) {
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

void WebRtcOffer::askAWishThatWaits()
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

void WebRtcOffer::keyFramesOnly(bool only)
{
    if (m_mailbox) {
        const Held lock(m_mailbox->mutex);
        m_mailbox->pictures.keyFramesOnly(only);
    }
}

void WebRtcOffer::fromTheNextKeyFrame()
{
    if (m_mailbox) {
        const Held lock(m_mailbox->mutex);
        m_mailbox->pictures.fromTheNextKeyFrame();
    }
}

bool WebRtcOffer::stopped() const
{
    return !m_mailbox || m_mailbox->stopped.load(std::memory_order_acquire);
}

qint64 WebRtcOffer::lastWholePictureUs() const
{
    if (!m_mailbox) {
        return 0;
    }
    const Held lock(m_mailbox->mutex);
    return m_mailbox->pictures.lastWholeUs();
}

PictureBox::Counts WebRtcOffer::counts() const
{
    if (!m_mailbox) {
        return {};
    }
    const Held lock(m_mailbox->mutex);
    return m_mailbox->pictures.counts();
}

WebRtcOffer::Refused WebRtcOffer::refused() const
{
    if (!m_mailbox) {
        return {};
    }
    const Held lock(m_mailbox->mutex);
    return m_mailbox->refused;
}

int WebRtcOffer::libraryCallsUnderOwnLock()
{
    return rtcseam::callsUnderOwnLock();
}

QByteArray WebRtcOffer::remoteDescription() const
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

void WebRtcOffer::sayTheOffer()
{
    // The host addresses took longer than they may to gather: the offer as it stands (with what was
    // gathered meanwhile), asked of the library with no lock of ours held.
    if (m_offerSaid || m_peer < 0 || !m_mailbox) {
        return;
    }
    intoLibrary();
    const int length = rtcGetLocalDescription(m_peer, nullptr, 0);
    if (length <= 0 || length > largestSdp) {
        emit failed(u"The WebRTC offer could not be made."_s);
        return;
    }
    QByteArray offer(length, '\0');
    intoLibrary();
    if (rtcGetLocalDescription(m_peer, offer.data(), length) < 0) {
        emit failed(u"The WebRTC offer could not be made."_s);
        return;
    }
    offer.truncate(length - 1);
    m_offerSaid = true;
    m_applicationMid = applicationMid(offer).value_or(QByteArray());
    emit offerReady(offer);
}

void WebRtcOffer::drain()
{
    if (!m_mailbox) {
        return;
    }
    QByteArray offer;
    QString error;
    QList<Said> words;
    QList<std::pair<int, bool>> events;
    QList<PictureBox::Picture> pictures;
    bool wantKey = false;
    {
        const Held lock(m_mailbox->mutex);
        m_mailbox->posted = false;
        offer = std::exchange(m_mailbox->offer, {});
        error = std::exchange(m_mailbox->error, {});
        words = std::exchange(m_mailbox->words, {});
        m_mailbox->wordsBytes = 0;
        events = std::exchange(m_mailbox->channelEvents, {});
        pictures = m_mailbox->pictures.take();
        wantKey = std::exchange(m_mailbox->wantKey, false);
    }
    if (wantKey && m_track >= 0) {
        intoLibrary();
        if (rtcIsOpen(m_track)) {
            intoLibrary();
            rtcRequestKeyframe(m_track);
        }
    }
    // What is said may close this one from where it is told (a QObject that ends it): nothing more is
    // told after that.
    if (!offer.isEmpty() && !m_offerSaid) {
        m_offerSaid = true;
        m_gather.stop();
        // (The mid its application is offered under: an answer's is held to it.)
        m_applicationMid = applicationMid(offer).value_or(QByteArray());
        emit offerReady(offer);
    }
    for (const auto& [place, open] : events) {
        if (!m_mailbox) {
            return;
        }
        if (open) {
            emit channelOpened(place);
        } else {
            emit channelClosed(place);
        }
    }
    for (const Said& word : words) {
        if (!m_mailbox) {
            return;
        }
        emit said(word);
    }
    for (const PictureBox::Picture& picture : pictures) {
        if (!m_mailbox) {
            return;
        }
        emit pictureCame(picture.bytes, picture.key, picture.arrivedUs);
    }
    if (!error.isEmpty() && m_mailbox) {
        emit failed(error);
    }
}

void WebRtcOffer::close()
{
    m_gather.stop();
    m_wishWaits.stop();
    if (m_mailbox) {
        const Held lock(m_mailbox->mutex);
        m_mailbox->target = nullptr;
        m_mailbox->stopped.store(true, std::memory_order_release);
    }
    // Every id forgotten first: a callback that still comes (closing a peer that is still connecting
    // runs ours on this thread) finds nobody. Then deleted, with no lock of ours held (M-D4).
    for (const int id : std::as_const(m_channels)) {
        Mailbox::Known::forget(id);
    }
    if (m_track >= 0) {
        Mailbox::Known::forget(m_track);
    }
    if (m_peer >= 0) {
        Mailbox::Known::forget(m_peer);
    }
    for (const int id : std::exchange(m_channels, {})) {
        intoLibrary();
        rtcDeleteDataChannel(id);
    }
    if (m_track >= 0) {
        intoLibrary();
        rtcDeleteTrack(std::exchange(m_track, -1));
    }
    if (m_peer >= 0) {
        intoLibrary();
        rtcDeletePeerConnection(std::exchange(m_peer, -1));
    }
    m_mailbox.reset();
}

} // namespace qkvm::core
