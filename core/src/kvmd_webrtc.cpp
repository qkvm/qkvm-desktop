#include <QJsonDocument>
#include <QJsonParseError>
#include <QUuid>
#include <cmath>
#include <qkvm/core/kvmd_webrtc.h>
#include <qkvm/core/video_diagnostics.h>
#include <utility>

#include "rtc_sdp.h"

using namespace Qt::StringLiterals;
namespace qkvm::core {
namespace {
qint64 idOf(const QJsonValue& value)
{
    const double id = value.toDouble(-1);
    return id > 0 && id <= 9007199254740991.0 && std::floor(id) == id ? value.toInteger() : 0;
}
} // namespace
KvmdWebRtc::KvmdWebRtc(KvmdDevice& device, QObject* parent) : KvmdWebRtc(device, Pace{}, parent) {}
KvmdWebRtc::KvmdWebRtc(KvmdDevice& device, Pace pace, QObject* parent)
    : QObject(parent), m_device(&device), m_pace(pace)
{
    m_transaction = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_picture.setSingleShot(true);
    m_silent.setSingleShot(true);
    connect(&m_picture, &QTimer::timeout, this,
            [this] { fail(u"video_timeout"_s, u"Native video stopped sending usable pictures."_s); });
    connect(&m_silent, &QTimer::timeout, this,
            [this] { fail(u"video_timeout"_s, u"The native video service stopped answering."_s); });
    connect(&m_keepalive, &QTimer::timeout, this, [this] { send({{u"janus"_s, u"keepalive"_s}}); });
    connect(&device, &QObject::destroyed, this, [this] { fail(u"device_gone"_s, u"The device was closed."_s); });
}
KvmdWebRtc::~KvmdWebRtc()
{
    close();
}

void KvmdWebRtc::start()
{
    if (m_phase != Phase::New || m_device == nullptr) {
        return;
    }
    VideoDiagnostics::record("native_opening");
    m_phase = Phase::Opening;
    m_picture.start(m_pace.firstPictureMs);
    m_device->openVideoLine([self = QPointer<KvmdWebRtc>(this)](base::Result<std::unique_ptr<DeviceLine>> result) {
        if (self == nullptr || self->m_phase == Phase::Over) {
            return;
        }
        if (!result) {
            self->fail(QString::fromStdString(result.error().code), QString::fromStdString(result.error().message));
            return;
        }
        self->m_line = std::move(result).value();
        DeviceLine* line = self->m_line.get();
        connect(line, &DeviceLine::letIn, self, [self] {
            VideoDiagnostics::record("janus_creating");
            self->m_phase = Phase::Creating;
            self->send({{u"janus"_s, u"create"_s}});
            self->m_create = self->m_transaction + QString::number(self->m_serial);
            self->m_silent.start(self->m_pace.silentMs);
        });
        connect(line, &DeviceLine::textCame, self, &KvmdWebRtc::read);
        connect(line, &DeviceLine::ended, self, [self](DeviceLine::End why) {
            VideoDiagnostics::record("signaling_ended", {{"reason", static_cast<int>(why)}});
            const bool unsupported = why == DeviceLine::End::NotLetIn;
            self->fail(unsupported ? u"no_native_video"_s : u"video_connection"_s,
                       unsupported ? u"This device does not offer the supported native video service."_s
                                   : u"The native video service could not be reached or trusted."_s);
        });
        line->open();
    });
}

void KvmdWebRtc::send(QJsonObject message, bool handle)
{
    if (m_phase == Phase::Over || m_line == nullptr) {
        return;
    }
    message.insert(u"transaction"_s, m_transaction + QString::number(++m_serial));
    if (m_session > 0) {
        message.insert(u"session_id"_s, m_session);
    }
    if (handle) {
        message.insert(u"handle_id"_s, m_handle);
    }
    if (!m_line->say(QJsonDocument(message).toJson(QJsonDocument::Compact)) && m_phase != Phase::Over) {
        fail(u"video_connection"_s, u"The native video service is not answering."_s);
    }
}

void KvmdWebRtc::read(const QByteArray& text)
{
    if (m_phase == Phase::Over) {
        return;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(text, &error);
    if (text.size() > qsizetype{128} * 1024 || error.error != QJsonParseError::NoError || !document.isObject()) {
        fail(u"video_protocol"_s, u"The native video service sent an invalid message."_s);
        return;
    }
    const QJsonObject word = document.object();
    const QString kind = word[u"janus"_s].toString();
    const QString transaction = word[u"transaction"_s].toString();
    if (m_session > 0 && idOf(word[u"session_id"_s]) != m_session) {
        return;
    }
    if (word.contains(u"sender"_s) && idOf(word[u"sender"_s]) != m_handle) {
        return;
    }
    if (kind == u"error"_s || kind == u"hangup"_s || kind == u"detached"_s || kind == u"timeout"_s) {
        VideoDiagnostics::record("janus_error", {{"code", word[u"error"_s].toObject()[u"code"_s].toInt(-1)}});
        fail(u"video_connection"_s, u"The device ended native video."_s);
        return;
    }
    m_silent.start(m_pace.silentMs);
    if (m_phase == Phase::Creating && kind == u"success"_s && transaction == m_create) {
        m_session = idOf(word[u"data"_s].toObject()[u"id"_s]);
        if (m_session == 0) {
            fail(u"video_protocol"_s, u"The native video session is invalid."_s);
            return;
        }
        VideoDiagnostics::record("janus_attaching");
        m_phase = Phase::Attaching;
        send({{u"janus"_s, u"attach"_s}, {u"plugin"_s, u"janus.plugin.ustreamer"_s}});
        m_attach = m_transaction + QString::number(m_serial);
        m_keepalive.start(m_pace.keepaliveMs);
    } else if (m_phase == Phase::Attaching && kind == u"success"_s && transaction == m_attach) {
        m_handle = idOf(word[u"data"_s].toObject()[u"id"_s]);
        if (m_handle == 0) {
            fail(u"video_protocol"_s, u"The native video handle is invalid."_s);
            return;
        }
        VideoDiagnostics::record("janus_watching");
        m_phase = Phase::Watching;
        // Both families accept watch. GL.iNet's video_format=0 requests H.264; stock
        // PiKVM ignores that optional field. No audio, microphone, camera or HID channel.
        const QJsonObject params{{u"orientation"_s, 0},
                                 {u"audio"_s, false},
                                 {u"mic"_s, false},
                                 {u"camera"_s, false},
                                 {u"video_format"_s, 0}};
        send({{u"janus"_s, u"message"_s}, {u"body"_s, QJsonObject{{u"request"_s, u"watch"_s}, {u"params"_s, params}}}},
             true);
    } else if (kind == u"event"_s && (m_phase == Phase::Watching || m_phase == Phase::Playing)) {
        const QJsonObject plugin = word[u"plugindata"_s].toObject();
        if (idOf(word[u"sender"_s]) != m_handle || plugin[u"plugin"_s].toString() != u"janus.plugin.ustreamer"_s) {
            return;
        }
        const QJsonObject data = plugin[u"data"_s].toObject();
        if (data.contains(u"error"_s) || data.contains(u"error_code"_s) ||
            data[u"result"_s].toObject()[u"status"_s].toString() == u"stopped"_s) {
            VideoDiagnostics::record("janus_plugin_error", {{"code", data[u"error_code"_s].toInt(-1)}});
            fail(u"no_native_video"_s, u"The device cannot start native H.264 video."_s);
            return;
        }
        if (!word.contains(u"jsep"_s)) {
            return;
        }
        const QJsonObject jsep = word[u"jsep"_s].toObject();
        if (m_receiver || jsep[u"type"_s].toString() != u"offer"_s) {
            fail(u"video_protocol"_s, u"The native video format changed; reconnecting."_s);
            return;
        }
        VideoDiagnostics::record("janus_offer", {{"sdp_bytes", jsep[u"sdp"_s].toString().size()}});
        WebRtcReceiver::Setup setup;
        // (This PC's loopback only where the KVM is spoken to there: a test's stand-in.)
        setup.loopbackAllowed = m_device != nullptr && spokenToOnThisPc(m_device->where().address);
        m_receiver = std::make_unique<WebRtcReceiver>(setup);
        connect(m_receiver.get(), &WebRtcReceiver::answerReady, this, [this](const QByteArray& sdp) {
            VideoDiagnostics::record("janus_answer", {{"sdp_bytes", sdp.size()}});
            sayWhatWasRefused();
            send({{u"janus"_s, u"message"_s},
                  {u"body"_s, QJsonObject{{u"request"_s, u"start"_s}}},
                  {u"jsep"_s,
                   QJsonObject{{u"type"_s, u"answer"_s}, {u"sdp"_s, QString::fromUtf8(sdp)}, {u"trickle"_s, false}}}},
                 true);
        });
        connect(m_receiver.get(), &WebRtcReceiver::pictureCame, this,
                [this](const QByteArray& bytes, bool key, qint64 arrivedUs) {
                    if (m_phase == Phase::Over) {
                        return;
                    }
                    if (m_phase != Phase::Playing) {
                        VideoDiagnostics::record("native_first_picture", {{"key", key}, {"bytes", bytes.size()}});
                    }
                    m_phase = Phase::Playing;
                    m_picture.start(m_pace.pictureMs);
                    emit pictureCame(bytes, key, arrivedUs);
                });
        connect(m_receiver.get(), &WebRtcReceiver::failed, this,
                [this](const QString& why) { fail(u"video_connection"_s, why); });
        if (!m_receiver->offer(jsep[u"sdp"_s].toString().toUtf8())) {
            fail(u"no_native_video"_s, u"The offered video format is not supported by the native H.264 receiver."_s);
        }
    } else if (kind == u"trickle"_s && m_receiver) {
        // An address it trickles: judged as every address of a KVM's is, its mid let go (WebRtcReceiver). One
        // that is refused is counted and let go, and the session goes on with those taken, as a JetKVM's does
        // (before, the 65th, or one the library could not read, ended it).
        const QJsonObject candidate = word[u"candidate"_s].toObject();
        if (!candidate[u"completed"_s].toBool()) {
            static_cast<void>(m_receiver->candidate(candidate[u"candidate"_s].toString().toUtf8()));
        }
    }
}
void KvmdWebRtc::requestKey()
{
    if (m_receiver) {
        m_receiver->requestKey();
    }
}
void KvmdWebRtc::close()
{
    if (m_phase == Phase::Over) {
        return;
    }
    VideoDiagnostics::record("native_closed", {{"phase", static_cast<int>(m_phase)}});
    m_phase = Phase::Over;
    m_picture.stop();
    m_silent.stop();
    m_keepalive.stop();
    if (m_receiver) {
        m_receiver->close();
    }
    if (m_line) {
        m_line->cut();
    }
}
void KvmdWebRtc::fail(const QString& code, const QString& reason)
{
    if (m_phase == Phase::Over) {
        return;
    }
    const QStringList codes{u"video_timeout"_s, u"video_connection"_s, u"no_native_video"_s, u"video_protocol"_s,
                            u"device_gone"_s};
    VideoDiagnostics::record("native_failed", {{"phase", static_cast<int>(m_phase)}, {"code", codes.indexOf(code)}});
    sayWhatWasRefused();
    close();
    emit ended(code, reason);
}

void KvmdWebRtc::sayWhatWasRefused() const
{
    // Of the KVM's addresses, what was refused, by its reason, beside the rest of the video's diagnostics: where
    // a Janus had all of its refused, the slow fall-back to JPEG has its reason there (the reading of 56258cd,
    // L2). Numbers only, as everything there is.
    if (!m_receiver) {
        return;
    }
    const WebRtcReceiver::Refused refused = m_receiver->refused();
    VideoDiagnostics::record("rtc_addresses_refused",
                             {{"all", static_cast<qint64>(refused.candidates)},
                              {"malformed", static_cast<qint64>(refused.malformedCandidates)},
                              {"not_udp", static_cast<qint64>(refused.candidatesNotUdp)},
                              {"named", static_cast<qint64>(refused.namedCandidates)},
                              {"unreachable", static_cast<qint64>(refused.unreachableCandidates)},
                              {"beyond", static_cast<qint64>(refused.candidatesBeyond)}});
}
} // namespace qkvm::core
