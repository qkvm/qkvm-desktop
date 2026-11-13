#pragma once

#include "clips.h"
#include "film_sender.h"
#include "made_up_kvm.h"
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTest>
#include <QTimer>
#include <QWebSocket>
#include <QWebSocketServer>
#include <filesystem>
#include <memory>
#include <rtc/rtc.h>
#include <span>
#include <utility>
#include <vector>

namespace qkvm::core::testing {
// Original local fixture: authenticated HTTP belongs to MadeUpKvm, Janus to this
// socket, and real encrypted H.264 media to a loopback-only libdatachannel sender.
class JanusByHand : public QObject {
public:
    explicit JanusByHand(MadeUpKvm& kvm) : m_server(QStringLiteral("Janus stand-in"), QWebSocketServer::NonSecureMode)
    {
        m_server.setSupportedSubprotocols({QStringLiteral("janus-protocol")});
        kvm.stand->upgrade = [this](QTcpSocket* socket, const QByteArray& request) {
            if (!request.startsWith("GET /janus/ws ")) {
                return false;
            }
            ++connections;
            m_wire = socket;
            cookieSeen = request.contains("Cookie: auth_token=");
            QTimer::singleShot(0, &m_server, [this, socket = QPointer<QTcpSocket>(socket)] {
                if (socket != nullptr) {
                    m_server.handleConnection(socket);
                    emit socket->readyRead();
                }
            });
            return true;
        };
        connect(&m_server, &QWebSocketServer::newConnection, this, [this] {
            stopPeer();
            m_socket = m_server.nextPendingConnection();
            connect(m_socket, &QWebSocket::textMessageReceived, this, &JanusByHand::read);
            connect(m_socket, &QWebSocket::disconnected, m_socket, &QObject::deleteLater);
        });
        m_timer.setInterval(20);
        connect(&m_timer, &QTimer::timeout, this, &JanusByHand::tick);
        m_timer.start();
        const auto file = bench::readFile(std::filesystem::path(QKVM_TESTDATA) / "pattern-720p15.h264");
        if (file) {
            m_bytes = file.value();
            m_cuts = bench::cutIntoPictures(m_bytes);
        }
    }
    ~JanusByHand() override { stopPeer(); }
    JanusByHand(const JanusByHand&) = delete;
    JanusByHand& operator=(const JanusByHand&) = delete;
    JanusByHand(JanusByHand&&) = delete;
    JanusByHand& operator=(JanusByHand&&) = delete;
    int connections = 0;
    int creates = 0;
    int answers = 0;
    int packets = 0;
    int keepalives = 0;
    bool cookieSeen = false;
    bool unsupported = false;
    bool silent = false;
    bool wrongSession = false;
    bool sendPictures = true;
    bool protocolError = false;
    // Addresses ("candidate:..." texts) its offer carries beside its own, after them; and those it trickles
    // right after its offer, as a Janus with full trickle does (each a "trickle" with the video's mid).
    QList<QByteArray> moreInItsOffer;
    QList<QByteArray> trickledAfterItsOffer;
    // Requests for a key frame heard on the track (on the library's thread), and whether they are
    // answered (fromAThread only).
    std::atomic<int> keyFrameAsks{0};
    std::atomic<bool> answersKeyFrameRequests{false};
    bool fixtureLoaded() const { return m_cuts.size() >= 30; }
    // One picture every `ms` milliseconds (67 as it comes: the clip's own 15 a second).
    void paceEvery(int ms)
    {
        m_pace = ms;
        m_timer.setInterval(std::min(20, ms));
        m_timer.setTimerType(Qt::PreciseTimer);
    }
    // The pictures from a thread of their own, one every `ms` milliseconds, from the next start on:
    // a stalled Qt loop then stalls the receiving side alone, as on a real KVM (film_sender.h).
    void fromAThread(int ms)
    {
        m_threadEveryMs = ms;
        m_film.film(m_bytes, m_cuts);
    }
    const FilmSender& film() const { return m_film; }
    void noSubprotocol() { m_server.setSupportedSubprotocols({}); }
    void frame(const QByteArray& bytes)
    {
        if (m_wire) {
            m_wire->write(bytes);
        }
    }
    void fall()
    {
        if (m_socket) {
            m_socket->abort();
        }
        stopPeer();
    }
    void say(const QJsonObject& object)
    {
        if (m_socket) {
            m_socket->sendTextMessage(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
        }
    }
    void raw(const QString& text)
    {
        if (m_socket) {
            m_socket->sendTextMessage(text);
        }
    }

private:
    void stopPeer()
    {
        m_film.stop();
        if (m_track >= 0) {
            rtcDeleteTrack(std::exchange(m_track, -1));
        }
        if (m_peer >= 0) {
            rtcDeletePeerConnection(std::exchange(m_peer, -1));
        }
        m_offered = false;
        m_started = false;
        m_index = 0;
    }
    QJsonObject pluginEvent(const QJsonObject& data) const
    {
        return {{u"janus"_s, u"event"_s},
                {u"session_id"_s, wrongSession ? 4322 : 4321},
                {u"sender"_s, 5432},
                {u"plugindata"_s, QJsonObject{{u"plugin"_s, u"janus.plugin.ustreamer"_s}, {u"data"_s, data}}}};
    }
    void read(const QString& text)
    {
        if (silent) {
            return;
        }
        const auto word = QJsonDocument::fromJson(text.toUtf8()).object();
        const QString kind = word[u"janus"_s].toString();
        QJsonObject response{{u"janus"_s, u"success"_s}, {u"transaction"_s, word[u"transaction"_s]}};
        if (kind == u"create"_s) {
            ++creates;
            response.insert(u"data"_s, QJsonObject{{u"id"_s, 4321}});
            say(response);
            return;
        }
        if (word[u"session_id"_s].toInteger() != 4321) {
            protocolError = true;
            return;
        }
        response.insert(u"session_id"_s, 4321);
        if (kind == u"attach"_s) {
            protocolError = word[u"plugin"_s].toString() != u"janus.plugin.ustreamer"_s;
            response.insert(u"data"_s, QJsonObject{{u"id"_s, 5432}});
            say(response);
        } else if (kind == u"keepalive"_s) {
            ++keepalives;
            response[u"janus"_s] = u"ack"_s;
            say(response);
        } else if (kind == u"message"_s) {
            if (word[u"handle_id"_s].toInteger() != 5432) {
                protocolError = true;
                return;
            }
            const QString request = word[u"body"_s].toObject()[u"request"_s].toString();
            if (request == u"watch"_s) {
                const QJsonObject params = word[u"body"_s].toObject()[u"params"_s].toObject();
                protocolError =
                    params[u"audio"_s].toBool() || params[u"mic"_s].toBool() || params[u"camera"_s].toBool();
                rtcConfiguration config{};
                config.bindAddress = "127.0.0.1";
                config.disableAutoNegotiation = true;
                m_peer = rtcCreatePeerConnection(&config);
                m_track = rtcAddTrack(
                    m_peer, "m=video 9 UDP/TLS/RTP/SAVPF 96\r\na=mid:video\r\na=sendonly\r\na=rtcp-mux\r\na=rtpmap:96 "
                            "H264/90000\r\na=fmtp:96 packetization-mode=1;profile-level-id=64001f\r\na=rtcp-fb:96 nack "
                            "pli\r\na=ssrc:1234 cname:local-fixture\r\n");
                rtcPacketizerInit packetizer{};
                packetizer.ssrc = 1234;
                packetizer.cname = "local-fixture";
                packetizer.payloadType = 96;
                packetizer.clockRate = 90000;
                packetizer.sequenceNumber = 65530;
                packetizer.maxFragmentSize = 600;
                packetizer.nalSeparator = RTC_NAL_SEPARATOR_START_SEQUENCE;
                if (m_peer < 0 || m_track < 0 || rtcSetH264Packetizer(m_track, &packetizer) < 0 ||
                    rtcChainRtcpSrReporter(m_track) < 0) {
                    protocolError = true;
                }
                // Each request for a key frame it hears (a PLI) is counted. It is answered only where a
                // test says so, and only by the film sent from a thread (the film's next key frame goes
                // next); otherwise the film goes on to its own next key frame, as a KVM's may.
                rtcSetUserPointer(m_track, this);
                if (rtcChainPliHandler(m_track,
                                       [](int, void* janus) {
                                           auto* self = static_cast<JanusByHand*>(janus);
                                           ++self->keyFrameAsks;
                                           if (self->answersKeyFrameRequests) {
                                               self->m_film.keyFrameNext = true;
                                           }
                                       }) < 0 ||
                    rtcSetLocalDescription(m_peer, "offer") < 0) {
                    protocolError = true;
                }
                m_gathered.start();
            } else if (request == u"start"_s) {
                ++answers;
                const QJsonObject jsep = word[u"jsep"_s].toObject();
                if (jsep[u"type"_s].toString() != u"answer"_s ||
                    rtcSetRemoteDescription(m_peer, jsep[u"sdp"_s].toString().toUtf8().constData(), "answer") < 0) {
                    protocolError = true;
                }
                m_started = true;
                m_paced.start();
                if (m_threadEveryMs > 0) {
                    m_film.start(m_track, std::chrono::milliseconds(m_threadEveryMs));
                }
                say(pluginEvent({{u"result"_s, QJsonObject{{u"status"_s, u"started"_s}}}}));
            }
        }
    }
    void tick()
    {
        if (m_peer < 0 || !m_socket) {
            return;
        }
        if (!m_offered && m_gathered.elapsed() >= 200) {
            const int length = rtcGetLocalDescription(m_peer, nullptr, 0);
            if (length <= 0 || length > 65536) {
                return;
            }
            QByteArray sdp(length, '\0');
            if (rtcGetLocalDescription(m_peer, sdp.data(), length) < 0) {
                return;
            }
            sdp.truncate(length - 1);
            if (!sdp.contains("a=candidate:")) {
                return;
            }
            if (unsupported) {
                sdp.replace("H264/90000", "H265/90000");
            }
            // (Its one media section is the last: lines at the end are its.)
            for (const QByteArray& more : std::as_const(moreInItsOffer)) {
                sdp += "a=" + more + "\r\n";
            }
            auto message = pluginEvent({{u"result"_s, QJsonObject{{u"status"_s, u"starting"_s}}}});
            message.insert(u"jsep"_s, QJsonObject{{u"type"_s, u"offer"_s}, {u"sdp"_s, QString::fromUtf8(sdp)}});
            m_offered = true;
            say(message);
            for (const QByteArray& address : std::as_const(trickledAfterItsOffer)) {
                say({{u"janus"_s, u"trickle"_s},
                     {u"session_id"_s, 4321},
                     {u"sender"_s, 5432},
                     {u"candidate"_s, QJsonObject{{u"candidate"_s, QString::fromUtf8(address)},
                                                  {u"sdpMid"_s, u"video"_s},
                                                  {u"sdpMLineIndex"_s, 0}}}});
            }
        }
        m_film.sending = sendPictures;
        if (m_threadEveryMs > 0 || !m_started || !sendPictures || !rtcIsOpen(m_track) || m_cuts.empty() ||
            m_paced.elapsed() < m_pace) {
            return;
        }
        m_paced.restart();
        const bench::Cut cut = m_cuts[m_index++ % m_cuts.size()];
        rtcSetTrackRtpTimestamp(m_track, static_cast<uint32_t>(m_index * 6000));
        QByteArray picture(static_cast<qsizetype>(cut.size), '\0');
        std::memcpy(picture.data(), m_bytes.data() + cut.from, cut.size);
        if (rtcSendMessage(m_track, picture.constData(), static_cast<int>(picture.size())) < 0) {
            protocolError = true;
        }
        ++packets;
    }
    QWebSocketServer m_server;
    QPointer<QWebSocket> m_socket;
    QPointer<QTcpSocket> m_wire;
    QTimer m_timer;
    int m_peer = -1;
    int m_track = -1;
    bool m_offered = false;
    bool m_started = false;
    std::size_t m_index = 0;
    int m_pace = 67;
    int m_threadEveryMs = 0;
    QElapsedTimer m_gathered;
    QElapsedTimer m_paced;
    std::vector<std::byte> m_bytes;
    std::vector<bench::Cut> m_cuts;
    // (Last: it stops before anything it sends with goes.)
    FilmSender m_film;
};

// Janus's offer alone, for a receiver that is given it by hand: a peer bound to 127.0.0.1 with a video it
// sends (as JanusByHand's), and its offer once its addresses are gathered. Nothing is sent.
class JanusOfferByHand {
public:
    JanusOfferByHand()
    {
        rtcConfiguration config{};
        config.bindAddress = "127.0.0.1";
        config.disableAutoNegotiation = true;
        m_peer = rtcCreatePeerConnection(&config);
        if (m_peer >= 0) {
            m_track =
                rtcAddTrack(m_peer, "m=video 9 UDP/TLS/RTP/SAVPF 96\r\na=mid:video\r\na=sendonly\r\na=rtcp-mux\r\n"
                                    "a=rtpmap:96 H264/90000\r\na=fmtp:96 packetization-mode=1;profile-level-id="
                                    "64001f\r\na=rtcp-fb:96 nack pli\r\na=ssrc:1234 cname:local-fixture\r\n");
            static_cast<void>(rtcSetLocalDescription(m_peer, "offer"));
        }
    }
    ~JanusOfferByHand()
    {
        if (m_track >= 0) {
            rtcDeleteTrack(m_track);
        }
        if (m_peer >= 0) {
            rtcDeletePeerConnection(m_peer);
        }
    }
    JanusOfferByHand(const JanusOfferByHand&) = delete;
    JanusOfferByHand& operator=(const JanusOfferByHand&) = delete;
    JanusOfferByHand(JanusOfferByHand&&) = delete;
    JanusOfferByHand& operator=(JanusOfferByHand&&) = delete;

    // Its offer, with its own addresses in it (empty where none came within 10 s).
    QByteArray offer() const
    {
        QElapsedTimer waited;
        waited.start();
        while (m_peer >= 0 && waited.elapsed() < 10'000) {
            const int length = rtcGetLocalDescription(m_peer, nullptr, 0);
            if (length > 0 && length <= 65536) {
                QByteArray sdp(length, '\0');
                if (rtcGetLocalDescription(m_peer, sdp.data(), length) >= 0) {
                    sdp.truncate(length - 1);
                    if (sdp.contains("a=candidate:")) {
                        return sdp;
                    }
                }
            }
            QTest::qWait(20);
        }
        return {};
    }
    // Its own addresses, as "candidate:..." texts.
    QList<QByteArray> addresses() const
    {
        QList<QByteArray> own;
        for (const QByteArray& line : offer().split('\n')) {
            if (line.trimmed().startsWith("a=candidate:")) {
                own.append(line.trimmed().sliced(2));
            }
        }
        return own;
    }

private:
    int m_peer = -1;
    int m_track = -1;
};
} // namespace qkvm::core::testing
