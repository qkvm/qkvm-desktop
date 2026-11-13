#pragma once

#include "clips.h"
#include "film_sender.h"
#include <QByteArray>
#include <QList>
#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <rtc/rtc.h>
#include <string>
#include <utility>
#include <vector>

namespace qkvm::core::testing {
// A JetKVM's own WebRTC peer, as pion is on the device (design 2, M6 2e), bound to 127.0.0.1: it takes
// an offer, answers with a send-only video of the offer's video mid and the first H.264 payload offered
// in packetization mode 1, and takes the data channels the offer opens. Its answer is bare: its
// addresses are handed over one by one afterwards (candidates()), as the device trickles them. It records
// what came on each channel, the PLIs it heard, and the channels it was handed; it sends a film's
// pictures from a thread of its own (film_sender.h) once its track is open. This is the far end: it
// calls the library as it likes, and nothing of the program's lock rule holds here.
class JetKvmPeerByHand {
public:
    explicit JetKvmPeerByHand(const char* film = "jetkvm-1080p60.h264")
    {
        const auto bytes = bench::readFile(std::filesystem::path(QKVM_TESTDATA) / film);
        if (bytes) {
            auto cuts = bench::cutIntoPictures(bytes.value());
            m_loaded = cuts.size() >= 30;
            m_film.film(bytes.value(), std::move(cuts));
        }
    }
    ~JetKvmPeerByHand() { stop(); }
    JetKvmPeerByHand(const JetKvmPeerByHand&) = delete;
    JetKvmPeerByHand& operator=(const JetKvmPeerByHand&) = delete;
    JetKvmPeerByHand(JetKvmPeerByHand&&) = delete;
    JetKvmPeerByHand& operator=(JetKvmPeerByHand&&) = delete;

    bool filmLoaded() const { return m_loaded; }
    // Another film from now on, from its first picture: the screen behind changed its size. False where it is not
    // there, or nothing is sent yet.
    bool filmAnew(const char* film)
    {
        const auto bytes = bench::readFile(std::filesystem::path(QKVM_TESTDATA) / film);
        if (!bytes || m_track < 0 || everyMs <= 0) {
            return false;
        }
        auto cuts = bench::cutIntoPictures(bytes.value());
        m_film.film(bytes.value(), std::move(cuts));
        m_film.start(m_track, std::chrono::milliseconds(everyMs));
        return true;
    }
    // Whether the film is there, and long enough (tools/make_testdata.ps1 makes it): a test that has it sent says
    // so at its start, where it is not, rather than waiting for pictures that never come (a checkout whose
    // .testdata was made before the film was added counted seven rows of pictures that never came, five minutes).
    static bool filmIsThere(const char* film = "jetkvm-1080p60.h264")
    {
        const auto bytes = bench::readFile(std::filesystem::path(QKVM_TESTDATA) / film);
        return bytes && bench::cutIntoPictures(bytes.value()).size() >= 30;
    }
    static constexpr const char* noFilm =
        "There is no .testdata/jetkvm-1080p60.h264, or it is too short: make it with tools\\make_testdata.ps1";
    // A picture every so many milliseconds from the track's opening on (16: 60 a second, as the real
    // one sends); 0: none.
    int everyMs = 16;

    // The answer to `offer`, bare (no addresses in it); empty where it was not taken.
    QByteArray answer(const QByteArray& offer)
    {
        stop();
        m_gone = false;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_offers.append(offer);
        }
        const auto [mid, payload, format] = videoOf(offer);
        if (mid.isEmpty() || payload < 0) {
            return {};
        }
        rtcConfiguration config{};
        config.bindAddress = "127.0.0.1";
        config.disableAutoNegotiation = true;
        m_peer = rtcCreatePeerConnection(&config);
        if (m_peer < 0) {
            return {};
        }
        rtcSetUserPointer(m_peer, this);
        rtcSetStateChangeCallback(m_peer, [](int, rtcState state, void* me) {
            auto* self = static_cast<JetKvmPeerByHand*>(me);
            if ((state == RTC_CLOSED || state == RTC_DISCONNECTED || state == RTC_FAILED) && self->onGone &&
                !self->m_gone.exchange(true)) {
                self->onGone(*self);
            }
        });
        rtcSetLocalCandidateCallback(m_peer, [](int, const char* candidate, const char*, void* me) {
            auto* self = static_cast<JetKvmPeerByHand*>(me);
            QByteArray text(candidate);
            if (text.startsWith("a=")) {
                text = text.sliced(2);
            }
            const std::lock_guard<std::mutex> lock(self->m_mutex);
            self->m_candidates.append(text);
        });
        rtcSetDataChannelCallback(
            m_peer, [](int, int channel, void* me) { static_cast<JetKvmPeerByHand*>(me)->took(channel); });
        const QByteArray pt = QByteArray::number(payload);
        const QByteArray trackSdp = "m=video 9 UDP/TLS/RTP/SAVPF " + pt + "\r\na=mid:" + mid +
                                    "\r\na=sendonly\r\na=rtcp-mux\r\na=rtpmap:" + pt + " H264/90000\r\na=fmtp:" + pt +
                                    ' ' + format + "\r\na=rtcp-fb:" + pt +
                                    " nack pli\r\na=ssrc:4321 cname:jetkvm-fixture\r\n";
        m_track = rtcAddTrack(m_peer, trackSdp.constData());
        rtcPacketizerInit packetizer{};
        packetizer.ssrc = 4321;
        packetizer.cname = "jetkvm-fixture";
        packetizer.payloadType = static_cast<uint8_t>(payload);
        packetizer.clockRate = 90000;
        packetizer.sequenceNumber = 65000;
        packetizer.maxFragmentSize = 1200;
        packetizer.nalSeparator = RTC_NAL_SEPARATOR_START_SEQUENCE;
        if (m_track < 0 || rtcSetH264Packetizer(m_track, &packetizer) < 0 || rtcChainRtcpSrReporter(m_track) < 0) {
            return {};
        }
        rtcSetUserPointer(m_track, this);
        rtcChainPliHandler(m_track, [](int, void* me) { ++static_cast<JetKvmPeerByHand*>(me)->plis; });
        if (rtcSetRemoteDescription(m_peer, offer.constData(), "offer") < 0 ||
            rtcSetLocalDescription(m_peer, "answer") < 0) {
            return {};
        }
        const int length = rtcGetLocalDescription(m_peer, nullptr, 0);
        if (length <= 0) {
            return {};
        }
        QByteArray sdp(length, '\0');
        rtcGetLocalDescription(m_peer, sdp.data(), length);
        sdp.truncate(length - 1);
        QByteArray bare;
        for (const QByteArray& line : sdp.split('\n')) {
            if (!line.startsWith("a=candidate:") && !line.startsWith("a=end-of-candidates") && !line.isEmpty()) {
                bare += line + '\n';
            }
        }
        if (everyMs > 0 && m_loaded) {
            m_film.start(m_track, std::chrono::milliseconds(everyMs));
        }
        return bare;
    }
    // Its addresses gathered so far, each as "candidate:...".
    QList<QByteArray> candidates() const
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_candidates;
    }
    QList<QByteArray> offers() const
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_offers;
    }
    // The channels the offer opened, by their labels.
    QList<QByteArray> labels() const
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        QList<QByteArray> said;
        for (const auto& [id, label] : m_channels) {
            said.append(label);
        }
        return said;
    }
    bool isOpen(const QByteArray& label) const
    {
        const int id = channel(label);
        return id >= 0 && rtcIsOpen(id);
    }
    bool sendText(const QByteArray& label, const QByteArray& text)
    {
        const int id = channel(label);
        return id >= 0 && rtcSendMessage(id, text.constData(), -1) >= 0;
    }
    bool sendBinary(const QByteArray& label, const QByteArray& bytes)
    {
        const int id = channel(label);
        return id >= 0 && rtcSendMessage(id, bytes.constData(), static_cast<int>(bytes.size())) >= 0;
    }
    // What came on a channel, in order, each with whether it came as text.
    QList<std::pair<QByteArray, bool>> heard(const QByteArray& label) const
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_heard.find(label.toStdString());
        return found == m_heard.end() ? QList<std::pair<QByteArray, bool>>{} : found->second;
    }
    // A channel of its own making (the offer's side never takes one).
    int openOwnChannel(const QByteArray& label)
    {
        return m_peer >= 0 ? rtcCreateDataChannel(m_peer, label.constData()) : -1;
    }
    // Closes a channel it was handed, and keeps the rest of the connection (fake_jetkvm.py close_hid).
    bool closeChannel(const QByteArray& label)
    {
        const int id = channel(label);
        return id >= 0 && rtcClose(id) >= 0;
    }
    // Told, on the library's thread and with no lock of its own held, of each channel it was handed as it
    // opens, and of each message that comes on one (with whether it came as text). Set before answer().
    std::function<void(JetKvmPeerByHand& peer, const QByteArray& label)> onOpen;
    std::function<void(JetKvmPeerByHand& peer, const QByteArray& label, const QByteArray& bytes, bool text)> onMessage;
    // Told once, on the library's thread, where the connection closed, failed or went (the far end let go of it).
    std::function<void(JetKvmPeerByHand& peer)> onGone;
    const FilmSender& film() const { return m_film; }
    FilmSender& film() { return m_film; }
    void stop()
    {
        // (Let go of here: whoever let go of it knows, and is not told from inside the library's deleting.)
        m_gone = true;
        m_film.stop();
        if (m_track >= 0) {
            rtcDeleteTrack(std::exchange(m_track, -1));
        }
        if (m_peer >= 0) {
            rtcDeletePeerConnection(std::exchange(m_peer, -1));
        }
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_channels.clear();
    }

    std::atomic<int> plis{0};

private:
    struct Video {
        QByteArray mid;
        int payload = -1;
        QByteArray format;
    };
    // The offer's video mid, and its first H.264 payload of packetization mode 1, as pion picks.
    static Video videoOf(const QByteArray& offer)
    {
        Video video;
        bool inVideo = false;
        QList<int> order;
        std::map<int, QByteArray> formats;
        QList<int> h264;
        for (QByteArray line : offer.split('\n')) {
            line = line.trimmed();
            if (line.startsWith("m=")) {
                inVideo = line.startsWith("m=video ");
                if (inVideo) {
                    for (const QByteArray& word : line.split(' ').mid(3)) {
                        order.append(word.toInt());
                    }
                }
            } else if (inVideo && line.startsWith("a=mid:")) {
                video.mid = line.sliced(6);
            } else if (inVideo && line.startsWith("a=rtpmap:") && line.contains("H264/90000")) {
                h264.append(line.sliced(9).split(' ').value(0).toInt());
            } else if (inVideo && line.startsWith("a=fmtp:")) {
                const qsizetype space = line.indexOf(' ');
                formats[line.sliced(7, space - 7).toInt()] = line.sliced(space + 1);
            }
        }
        for (const int pt : order) {
            if (h264.contains(pt) && formats[pt].contains("packetization-mode=1")) {
                video.payload = pt;
                video.format = formats[pt];
                break;
            }
        }
        return video;
    }
    int channel(const QByteArray& label) const
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& [id, known] : m_channels) {
            if (known == label) {
                return id;
            }
        }
        return -1;
    }
    void took(int channel)
    {
        char label[256] = {};
        rtcGetDataChannelLabel(channel, label, sizeof(label));
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_channels.emplace_back(channel, QByteArray(label));
        }
        rtcSetUserPointer(channel, this);
        rtcSetOpenCallback(channel, [](int id, void* me) {
            auto* self = static_cast<JetKvmPeerByHand*>(me);
            const QByteArray label = self->labelOf(id);
            if (self->onOpen && !label.isEmpty()) {
                self->onOpen(*self, label);
            }
        });
        rtcSetMessageCallback(channel, [](int id, const char* bytes, int size, void* me) {
            auto* self = static_cast<JetKvmPeerByHand*>(me);
            const bool text = size < 0;
            const int length = text ? -(size + 1) : size;
            const QByteArray said(bytes, length);
            const QByteArray label = self->labelOf(id);
            {
                const std::lock_guard<std::mutex> lock(self->m_mutex);
                self->m_heard[label.toStdString()].append({said, text});
            }
            if (self->onMessage && !label.isEmpty()) {
                self->onMessage(*self, label, said, text);
            }
        });
    }
    QByteArray labelOf(int id) const
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& [known, label] : m_channels) {
            if (known == id) {
                return label;
            }
        }
        return {};
    }

    mutable std::mutex m_mutex;
    QList<QByteArray> m_offers;
    QList<QByteArray> m_candidates;
    std::vector<std::pair<int, QByteArray>> m_channels;
    std::map<std::string, QList<std::pair<QByteArray, bool>>> m_heard;
    int m_peer = -1;
    int m_track = -1;
    bool m_loaded = false;
    std::atomic<bool> m_gone{false};
    // (Last: it stops before anything it sends with goes.)
    FilmSender m_film;
};

} // namespace qkvm::core::testing
