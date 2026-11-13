#include <qkvm/core/jetkvm_connection.h>

#include <algorithm>
#include <cmath>
#include <utility>

#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonDocument>

#include "rtc_sdp.h"

#include <qkvm/base/clock.h>
#include <qkvm/core/device_line.h>
#include <qkvm/core/jetkvm_device.h>
#include <qkvm/core/kvmd_answers.h>
#include <qkvm/core/video_diagnostics.h>

using namespace Qt::StringLiterals;

namespace qkvm::core {

namespace {

// The two H.264 formats a JetKVM is offered (design 6.2; the web's offer), and its two channels.
const QList<QByteArray> formats{
    "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42001f",
    "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f",
};
constexpr qsizetype largestRpc = qsizetype{64} * 1024;
constexpr qsizetype largestHid = 64;
// Addresses kept that came before the answer, at the most (the offering side takes forty in all).
constexpr qsizetype earliestKept = 40;
// The largest address the offering side takes (WebRtcOffer::candidate).
constexpr qsizetype largestCandidate = 2048;
// What its input may say of its size and rate, at the most (clamped).
constexpr int largestSide = 16384;
constexpr double mostFps = 1000;

// The JetKVM's own words where they are the web's (jetkvm.py).
const char* const inUseSentence =
    "Someone is using this JetKVM's own console. It shows its picture to one viewer at a time.";

// A device's text as one short line, or nothing (kvmd::plain, for what was read by the book).
QString plainOf(const json::Value* value, qsizetype limit)
{
    if (value == nullptr) {
        return {};
    }
    if (value->isText()) {
        return kvmd::plain(QJsonValue(value->text), limit);
    }
    if (value->isNumber()) {
        return kvmd::plain(QJsonValue(value->number), limit);
    }
    return {};
}

// A number it said, within reason: clamped to [0, most]; nothing that is no number is 0.
double clampedNumber(const json::Value* value, double most)
{
    if (value == nullptr || !value->isNumber() || !std::isfinite(value->number)) {
        return 0;
    }
    return std::clamp(value->number, 0.0, most);
}

// Whether a text the JetKVM said on `rpc` is the word that another viewer took it: read on the library's
// thread, so pure (json::parse is).
bool isOtherSessionConnected(QByteArrayView text)
{
    if (!text.contains("otherSessionConnected")) {
        return false;
    }
    const base::Result<json::Value> said = json::parse(text);
    if (!said || !said.value().isObject()) {
        return false;
    }
    const json::Value* method = said.value().find(u"method");
    return method != nullptr && method->isText() && method->text == u"otherSessionConnected"_s;
}

} // namespace

JetKvmConnection::JetKvmConnection(JetKvmDevice& device, quint64 serial, Pace pace, std::shared_ptr<JetKvmKept> kept,
                                   QObject* parent)
    : QObject(parent), m_device(&device), m_serial(serial), m_pace(pace),
      m_kept(kept ? std::move(kept) : std::make_shared<JetKvmKept>())
{
    m_shake.setInterval(m_pace.shakeEveryMs);
    connect(&m_shake, &QTimer::timeout, this, &JetKvmConnection::shake);
    m_shakeWithin.setSingleShot(true);
    connect(&m_shakeWithin, &QTimer::timeout, this, [this] { decide(m_shaken ? Road::Hid : Road::Rpc); });
    m_hidWithin.setSingleShot(true);
    connect(&m_hidWithin, &QTimer::timeout, this, [this] {
        // (Its channel did not open in time: keys go over JSON-RPC, and the late road asks once it is open.)
        if (m_road == Road::Undecided && !m_shaking) {
            decide(Road::Rpc);
        }
    });
    connect(&m_lateRoad, &QTimer::timeout, this, &JetKvmConnection::askAgain);
    m_keepAlive.setSingleShot(true);
    m_keepAlive.setTimerType(Qt::PreciseTimer);
    connect(&m_keepAlive, &QTimer::timeout, this, &JetKvmConnection::keepAlive);
    m_watch.setInterval(m_pace.watchEveryMs);
    connect(&m_watch, &QTimer::timeout, this, &JetKvmConnection::watch);
    m_openWithin.setSingleShot(true);
    connect(&m_openWithin, &QTimer::timeout, this,
            [this] { finish(u"timeout"_s, u"The JetKVM is taking too long to connect"_s); });
    m_answerWithin.setSingleShot(true);
    connect(&m_answerWithin, &QTimer::timeout, this,
            [this] { finish(u"timeout"_s, u"The JetKVM did not answer the request for video"_s); });
}

JetKvmConnection::~JetKvmConnection()
{
    // (What can throw in end() is memory running out as the calls that waited are told: they are not told
    // then, and nothing leaves a destructor.)
    try {
        // Thrown away while something of the program's is down (the program ending): let go of as far as it can still
        // be said, not waited for.
        if (!m_over && !m_taken) {
            sendLetGo();
        }
        end();
    } catch (...) {
        m_waiting.clear();
    }
}

void JetKvmConnection::start()
{
    if (std::exchange(m_started, true) || m_over) {
        return;
    }
    if (m_device == nullptr) {
        finish(u"dropped"_s, u"The JetKVM is no longer on this wall"_s);
        return;
    }
    m_askedUs = base::steadyNowUs();
    step("jetkvm_asked");
    m_openWithin.start(m_pace.openWithinMs);
    m_device->openSignalling([here = QPointer<JetKvmConnection>(this)](const base::Result<DeviceLine*>& line) {
        if (here == nullptr || here->m_over) {
            // Gone, or ended meanwhile: the line is nobody's.
            if (line) {
                line.value()->close();
                line.value()->deleteLater();
            }
            return;
        }
        if (!line) {
            here->finish(QString::fromStdString(line.error().code), QString::fromStdString(line.error().message));
            return;
        }
        here->lineCame(line.value());
    });
}

void JetKvmConnection::lineCame(DeviceLine* line)
{
    // Taken here and now (JetKvmDevice::openSignalling): connected before anything it says next is read.
    line->setParent(this);
    m_line = line;
    connect(line, &DeviceLine::textCame, this, &JetKvmConnection::read);
    connect(line, &DeviceLine::ended, this, [this, line](DeviceLine::End /*why*/) {
        line->deleteLater();
        // The line falling after the answer ends nothing: the connection lives on without it (web :917-920).
        if (!m_answered) {
            finish(u"dropped"_s, u"The JetKVM closed the connection"_s);
        }
    });

    WebRtcOffer::Setup setup;
    setup.h264Formats = formats;
    setup.channels = {{"rpc", true, largestRpc}, {"hidrpc", false, largestHid}};
    setup.stopsOn = &isOtherSessionConnected;
    setup.mostCandidates = 40; // the web's MAX_CANDIDATES
    // (This PC's loopback only where the JetKVM is spoken to there: a test's stand-in.)
    setup.loopbackAllowed = m_device != nullptr && spokenToOnThisPc(m_device->address());
    m_peer = std::make_unique<WebRtcOffer>(setup);
    connect(m_peer.get(), &WebRtcOffer::offerReady, this, [this](const QByteArray& sdp) {
        const QByteArray description =
            QJsonDocument(QJsonObject{{u"type"_s, u"offer"_s}, {u"sdp"_s, QString::fromUtf8(sdp)}})
                .toJson(QJsonDocument::Compact);
        const QByteArray offer =
            QJsonDocument(QJsonObject{{u"type"_s, u"offer"_s},
                                      {u"data"_s, QJsonObject{{u"sd"_s, QString::fromLatin1(description.toBase64())}}}})
                .toJson(QJsonDocument::Compact);
        if (m_line == nullptr || !m_line->say(offer)) {
            finish(u"dropped"_s, u"The JetKVM closed the connection"_s);
            return;
        }
        m_answerWithin.start(m_pace.answerWithinMs);
        step("jetkvm_offer_sent");
        emit offerWent();
    });
    connect(m_peer.get(), &WebRtcOffer::channelOpened, this, [this](int channel) {
        if (channel == hidChannel) {
            hidOpened();
            return;
        }
        if (channel != rpcChannel || m_open) {
            return;
        }
        m_open = true;
        m_openedUs = base::steadyNowUs();
        m_openWithin.stop();
        m_watch.start();
        step("jetkvm_open");
        // Which way keys go is found out from here (jetkvm_keys.cpp): its channel is waited for as long as its
        // answer is.
        if (m_hidOpen) {
            beginShaking();
        } else {
            m_hidWithin.start(m_pace.shakeWithinMs);
        }
        emit opened();
    });
    connect(m_peer.get(), &WebRtcOffer::channelClosed, this, [this](int channel) {
        if (channel == hidChannel) {
            m_hidOpen = false;
            noMoreHid();
            return;
        }
        if (channel == rpcChannel && m_open) {
            finish(u"dropped"_s, u"The JetKVM closed the channel it is asked over"_s);
        }
    });
    connect(m_peer.get(), &WebRtcOffer::said, this, &JetKvmConnection::heard);
    connect(m_peer.get(), &WebRtcOffer::pictureCame, this,
            [this](const QByteArray& annexB, bool key, qint64 arrivedUs) {
                if (!std::exchange(m_pictured, true)) {
                    step("jetkvm_first_picture", key ? 1 : 0, annexB.size());
                }
                emit pictureCame(annexB, key, arrivedUs);
            });
    connect(m_peer.get(), &WebRtcOffer::failed, this, [this](const QString& /*reason*/) {
        if (m_open) {
            finish(u"dropped"_s, u"The JetKVM closed the video connection"_s);
        } else {
            finish(u"dropped"_s, u"Could not open a video connection to the JetKVM"_s);
        }
    });
    if (!m_peer->begin()) {
        finish(u"dropped"_s, u"Could not open a video connection to the JetKVM"_s);
    }
}

void JetKvmConnection::read(const QByteArray& text)
{
    if (m_over) {
        return;
    }
    // What is no JSON object (a "pong") is let go, as is what is not for this connection.
    const base::Result<json::Value> parsed = json::parse(text);
    if (!parsed || !parsed.value().isObject()) {
        return;
    }
    const json::Value& message = parsed.value();
    const json::Value* type = message.find(u"type");
    if (!m_answered) {
        const json::Value* error = message.find(u"error");
        if (type == nullptr && error != nullptr && !(error->kind == json::Value::Kind::Null) &&
            !(error->isFlag() && !error->flag) && !(error->isText() && error->text.isEmpty())) {
            const QString words = plainOf(error, 200);
            finish(u"bad_response"_s,
                   u"The JetKVM refused the request for video: "_s + (words.isEmpty() ? u"no reason given"_s : words));
            return;
        }
        if (type != nullptr && type->isText() && type->text == u"answer"_s) {
            const json::Value* data = message.find(u"data");
            answered(data != nullptr ? *data : json::Value{});
            return;
        }
    }
    if (type != nullptr && type->isText() && type->text == u"new-ice-candidate"_s) {
        // Its address; the mid it says is let go (the library takes the bundle's). One that comes before the
        // answer is kept for it (as the web's _await_answer takes them in either order), a few at the most.
        const json::Value* data = message.find(u"data");
        const json::Value* candidate = data != nullptr && data->isObject() ? data->find(u"candidate") : nullptr;
        if (candidate == nullptr || !candidate->isText() || !m_peer) {
            return;
        }
        if (!m_answered) {
            // (Only one that may be taken later, 2048 bytes at most, is kept: forty of a line's largest were
            // kept before, the reading of a9b950b, L4.)
            const QByteArray address = candidate->text.toUtf8();
            if (m_early.size() < earliestKept && address.size() <= largestCandidate) {
                m_early.append(address);
            }
            return;
        }
        m_peer->candidate(candidate->text.toUtf8());
    }
}

void JetKvmConnection::answered(const json::Value& data)
{
    const auto unreadable = [this] { finish(u"bad_response"_s, u"The JetKVM sent an answer that cannot be read"_s); };
    if (!data.isText()) {
        unreadable();
        return;
    }
    // (Its size before it is decoded: the reading of a9b950b, L4.)
    if (data.text.size() > qsizetype{256} * 1024) {
        unreadable();
        return;
    }
    const auto decoded = QByteArray::fromBase64Encoding(data.text.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded) {
        unreadable();
        return;
    }
    const base::Result<json::Value> description = json::parse(decoded.decoded);
    const json::Value* sdp = description && description.value().isObject() ? description.value().find(u"sdp") : nullptr;
    if (sdp == nullptr || !sdp->isText() || sdp->text.isEmpty() || !m_peer || !m_peer->answer(sdp->text.toUtf8())) {
        unreadable();
        return;
    }
    m_answered = true;
    m_answerWithin.stop();
    step("jetkvm_answer");
    for (const QByteArray& early : std::exchange(m_early, {})) {
        m_peer->candidate(early);
    }
}

void JetKvmConnection::heard(const WebRtcOffer::Said& word)
{
    if (m_over) {
        return;
    }
    if (word.channel == hidChannel) {
        heardHid(word.bytes);
        return;
    }
    if (word.channel != rpcChannel) {
        return;
    }
    const base::Result<json::Value> parsed = json::parse(word.bytes);
    if (!parsed || !parsed.value().isObject()) {
        return;
    }
    const json::Value& said = parsed.value();
    // An answer: only to a number of ours that waits, a whole one (w14 row 1: in any order).
    if (const json::Value* id = said.find(u"id"); id != nullptr && id->isNumber()) {
        if (!id->whole || id->number < 1 || id->number > 9.0e15) {
            return;
        }
        const auto number = static_cast<qint64>(id->number);
        // (A report's answer, whoever waits for it: keys do not change road while one may still be done.)
        m_unanswered.remove(number);
        const auto waiting = m_waiting.find(number);
        if (waiting == m_waiting.end()) {
            // A press no answer had come for in time, answered after all: it was done, maybe after the letting go sent
            // when its wait was given up (the JetKVM does each call in a task of its own). Let go of again, now in
            // turn (rule 25).
            const auto givenUp =
                std::ranges::find_if(m_givenUp, [number](const auto& one) { return one.first == number; });
            if (givenUp != m_givenUp.end()) {
                const Report press = givenUp->second;
                m_givenUp.erase(givenUp);
                letGoOf(press);
            }
            return;
        }
        if (m_taken) {
            // Answered after the JetKVM said its own page took it (its channel keeps the order of what it says): what
            // it asked for was done after the hand-over.
            m_afterHandOver.insert(number);
        }
        Waiting call = std::move(waiting.value());
        m_waiting.erase(waiting);
        settled(call, true);
        const json::Value* error = said.find(u"error");
        if (error != nullptr && error->kind != json::Value::Kind::Null) {
            QString words;
            if (error->isObject()) {
                const json::Value* detail = error->find(u"data");
                words = plainOf(detail, 200);
                if (words.isEmpty()) {
                    words = plainOf(error->find(u"message"), 200);
                }
            } else {
                words = plainOf(error, 200);
            }
            tell(std::move(call.done),
                 base::Error{"action_failed", (words.isEmpty() ? u"The JetKVM refused"_s : words).toStdString()});
            return;
        }
        const json::Value* result = said.find(u"result");
        tell(std::move(call.done), result != nullptr ? *result : json::Value{});
        return;
    }
    if (said.find(u"id") != nullptr && said.find(u"method") == nullptr) {
        return; // (an answer under a number that is none of ours: nothing)
    }
    const json::Value* method = said.find(u"method");
    if (method == nullptr || !method->isText()) {
        return;
    }
    const json::Value* params = said.find(u"params");
    if (method->text == u"otherSessionConnected"_s) {
        // Another viewer took it: once (a device may say it as often as it likes). Nothing more of ours goes to it,
        // but the letting go of what it may still do of ours (rule 28).
        if (!std::exchange(m_taken, true)) {
            letGoAtHandOver();
            emit takenByAnother();
        }
    } else if (method->text == u"videoInputState"_s && params != nullptr && params->isObject()) {
        ++m_videoEvents; // (an answer to getVideoState asked before this is older than it: 10f)
        const json::Value* ready = params->find(u"ready");
        if (ready != nullptr && ready->isFlag()) {
            // Back after it said there was none: the first picture of it is waited for from this moment.
            if (ready->flag && m_video.ready == std::optional<bool>(false)) {
                m_video.signalAtUs = word.arrivedUs;
            }
            m_video.ready = ready->flag;
        }
        m_video.width = static_cast<int>(clampedNumber(params->find(u"width"), largestSide));
        m_video.height = static_cast<int>(clampedNumber(params->find(u"height"), largestSide));
        m_video.fps = clampedNumber(params->find(u"fps"), mostFps);
        emit videoSaid();
    } else if (method->text == u"usbState"_s && params != nullptr && params->isText()) {
        m_usb = plainOf(params, 40);
    } else if (method->text == u"atxState"_s && params != nullptr && params->isObject()) {
        m_atx = *params;
    } else if (method->text == u"localVersion"_s && params != nullptr && params->isObject()) {
        m_version = plainOf(params->find(u"appVersion"), 40);
    } else if (method->text == u"keysDownState"_s && params != nullptr && params->isObject() && m_road == Road::Hid) {
        // (Where a JetKVM says its keyboard here rather than on hidrpc: the same word, taken the same way.)
        const json::Value* keys = params->find(u"keys");
        if (keys != nullptr && keys->isList() && keys->list.size() <= 16) {
            Keyboard keyboard;
            keyboard.modifiers = static_cast<int>(clampedNumber(params->find(u"modifier"), 255));
            for (const json::Value& key : keys->list) {
                if (const int usage = static_cast<int>(clampedNumber(&key, 255)); usage != 0) {
                    keyboard.keys.append(usage);
                }
            }
            keysSaid(keyboard);
        }
    }
    // (The rest is let go.)
}

void JetKvmConnection::call(const QString& method, const QJsonObject& params,
                            std::function<void(base::Result<json::Value>)> done, std::optional<int> withinMs)
{
    Waiting waiting;
    waiting.done = std::move(done);
    ask(method, params, std::move(waiting), withinMs.value_or(m_pace.callWithinMs));
}

std::optional<qint64> JetKvmConnection::ask(const QString& method, const QJsonObject& params, Waiting waiting,
                                            int withinMs)
{
    const auto refuse = [this, &waiting](base::Error why) {
        // (Never sent: nothing of it is in the JetKVM's hands, and nothing of it is let go of.)
        waiting.report = false;
        waiting.press.reset();
        settled(waiting, false);
        tell(std::move(waiting.done), std::move(why));
        return std::nullopt;
    };
    if (m_taken) {
        return refuse(base::Error{"in_use", inUseSentence});
    }
    if (!isOpen() || !m_peer) {
        return refuse(base::Error{"dropped", "The JetKVM closed the connection"});
    }
    if (m_closing || !m_anew.isEmpty()) {
        return refuse(endedError());
    }
    const qint64 id = ++m_lastId;
    const QByteArray text =
        QJsonDocument(
            QJsonObject{{u"jsonrpc"_s, u"2.0"_s}, {u"method"_s, method}, {u"params"_s, params}, {u"id"_s, id}})
            .toJson(QJsonDocument::Compact);
    if (!m_peer->send(rpcChannel, text)) {
        // (Not sent: not done. The channel it is asked over is gone, or another viewer took it just now.)
        return refuse(m_taken ? base::Error{"in_use", inUseSentence}
                              : base::Error{"dropped", "The JetKVM closed the channel it is asked over"});
    }
    if (waiting.report) {
        // (Until its answer comes, however long after its wait: keys do not change road while it may be done.)
        m_unanswered.insert(id);
    }
    if (waiting.press) {
        m_pressing.insert(id, *waiting.press);
    }
    waiting.id = id;
    m_waiting.insert(id, std::move(waiting));
    // Its time: where its answer came first, nothing waits under its number by then.
    QTimer::singleShot(withinMs, this, [this, id] {
        const auto found = m_waiting.find(id);
        if (found == m_waiting.end()) {
            return;
        }
        Waiting call = std::move(found.value());
        m_waiting.erase(found);
        settled(call, false);
        tell(std::move(call.done), base::Error{"timeout", "The JetKVM did not answer in time"});
    });
    return id;
}

void JetKvmConnection::tell(std::function<void(base::Result<json::Value>)> done, base::Result<json::Value> what)
{
    if (!done) {
        return;
    }
    // A turn of the loop later, never from inside a signal of the line or the peer: whoever is told may let
    // go of this connection. (The application's, so that a call that waited is told also where this
    // connection is gone by then.)
    QTimer::singleShot(0, QCoreApplication::instance(),
                       [done = std::move(done), what = std::move(what)] { done(what); });
}

void JetKvmConnection::requestKey()
{
    if (m_peer) {
        m_peer->requestKey();
    }
}

void JetKvmConnection::keyFramesOnly(bool only)
{
    if (m_peer) {
        m_peer->keyFramesOnly(only);
    }
}

void JetKvmConnection::fromTheNextKeyFrame()
{
    if (m_peer) {
        m_peer->fromTheNextKeyFrame();
    }
}

WebRtcOffer::Refused JetKvmConnection::refused() const
{
    return m_peer ? m_peer->refused() : WebRtcOffer::Refused{};
}

void JetKvmConnection::watch()
{
    if (m_over || !m_peer) {
        return;
    }
    const qint64 now = base::steadyNowUs();
    const qint64 last = m_peer->lastWholePictureUs();
    const qint64 margin = qint64{m_pace.afterSignalMarginMs} * 1000;
    const bool saysNoSignal = m_video.ready == std::optional<bool>(false);
    const qint64 afterSignalUs = qint64{m_pace.pictureAfterSignalMs} * 1000;
    bool stalled = false;
    if (m_video.signalAtUs > 0 && last < m_video.signalAtUs + margin) {
        // Its input came back since its last picture of it (one within the margin after the word may be
        // from before): the first one is waited for from that moment.
        stalled = now - m_video.signalAtUs > afterSignalUs;
    } else if (last == 0) {
        // No picture yet on this connection: its first one is waited for as long from `rpc`'s opening (10e; the
        // web's _hold): a rested JetKVM takes seconds, and one that never sends one would otherwise say "Waiting
        // for the JetKVM's video" for as long as it is held. Where none comes by then, it is made anew.
        stalled = m_openedUs > 0 && now - m_openedUs > afterSignalUs;
    } else {
        // (A JetKVM sends video all the time, a still screen too: none for so long, while it claims a
        // signal, is one that stopped.)
        stalled = now - last > qint64{m_pace.frameStaleMs} * 1000;
    }
    if (stalled && !saysNoSignal) {
        finish(u"dropped"_s, last == 0 ? u"The JetKVM sent no picture"_s : u"The JetKVM stopped sending video"_s);
    }
}

void JetKvmConnection::step(const char* event, std::optional<qint64> key, std::optional<qint64> bytes) const
{
    const qint64 ms = (base::steadyNowUs() - m_askedUs) / 1000;
    if (key && bytes) {
        VideoDiagnostics::record(event, {{"ms", ms}, {"key", *key}, {"bytes", *bytes}});
    } else {
        VideoDiagnostics::record(event, {{"ms", ms}});
    }
}

void JetKvmConnection::finish(const QString& code, const QString& message)
{
    if (m_over) {
        return;
    }
    // What this connection has down is let go of while it is still open, where nobody else has taken its viewer (not
    // waited for: it ended by itself), and owed to the next one where anything was (the web's _run).
    if (!m_taken) {
        sendLetGo();
    }
    leftWhatWasDown();
    end();
    // Taken by another viewer, its end is that (whatever came of the JetKVM closing it then).
    if (m_taken) {
        emit ended(u"in_use"_s, QString::fromLatin1(inUseSentence));
        return;
    }
    emit ended(code, message);
}

void JetKvmConnection::end()
{
    if (m_over) {
        return;
    }
    // What this connection may have pressed and has no answer for is let go of while it is still open (rule 24): a
    // press the JetKVM does after this is otherwise down for whoever has its viewer then.
    letGoOfPressesInHand();
    m_over = true;
    m_watch.stop();
    m_openWithin.stop();
    m_answerWithin.stop();
    m_shake.stop();
    m_shakeWithin.stop();
    m_hidWithin.stop();
    m_lateRoad.stop();
    m_keepAlive.stop();
    m_held.clear();
    m_outbox.clear();
    if (m_line != nullptr) {
        m_line->close();
        m_line->deleteLater();
        m_line = nullptr;
    }
    if (m_peer) {
        m_peer->close();
        // (Its own signals may be on their way out of it: let go of a turn later.)
        m_peer.release()->deleteLater();
    }
    // Every call that waits is told: it went out, and what came of it is not known.
    base::Error cut = m_taken ? base::Error{"timeout", "The JetKVM's own page took it before it answered: it "
                                                       "may have been done"}
                              : base::Error{"timeout", "The connection to the JetKVM ended before it "
                                                       "answered: it may have been done"};
    if (!m_taken && !m_anew.isEmpty()) {
        cut.message = "The connection to the JetKVM was made anew before it answered (" + m_anew.toStdString() +
                      "): it may have been done";
    }
    QHash<qint64, Waiting> cutOff = std::exchange(m_waiting, {});
    for (Waiting& call : cutOff) {
        settled(call, false);
        tell(std::move(call.done), cut);
    }
    echoesFail(cut);
    m_pressing.clear();
    m_givenUp.clear();
}

namespace {

// The connections let go of that are letting go still, and the loop that waits for them at the program's end.
int goingAway = 0;
QEventLoop* waitingForThem = nullptr;

} // namespace

int JetKvmConnection::lettingGo()
{
    return goingAway;
}

void JetKvmConnection::finishLettingGo(int withinMs)
{
    if (goingAway == 0 || waitingForThem != nullptr) {
        return;
    }
    QEventLoop waiting;
    waitingForThem = &waiting;
    QTimer::singleShot(withinMs, &waiting, &QEventLoop::quit);
    waiting.exec();
    waitingForThem = nullptr;
}

void JetKvmConnection::letGo(std::unique_ptr<JetKvmConnection> connection)
{
    if (connection == nullptr) {
        return;
    }
    JetKvmConnection* const going = connection.release();
    // (Kept until it is gone, by the application: with the program at the latest.)
    going->setParent(QCoreApplication::instance());
    if (going->m_over) {
        going->deleteLater();
        return;
    }
    // (Counted until it is gone: the program's end waits for that, finishLettingGo.)
    ++goingAway;
    connect(going, &QObject::destroyed, QCoreApplication::instance(), [] {
        if (--goingAway == 0 && waitingForThem != nullptr) {
            waitingForThem->quit();
        }
    });
    going->m_closing = true;
    going->m_watch.stop();
    going->m_openWithin.stop();
    going->m_answerWithin.stop();
    going->closeAfterLettingGo([going] {
        going->leftWhatWasDown();
        going->end();
        going->deleteLater();
    });
}

} // namespace qkvm::core
