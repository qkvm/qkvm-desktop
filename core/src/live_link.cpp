#include <qkvm/core/live_link.h>

#include <algorithm>
#include <utility>

#include <QJsonArray>
#include <QJsonDocument>
#include <QtEndian>

#include <qkvm/core/server_client.h>
#include <qkvm/core/server_types.h>
#include <qkvm/core/tls.h>
#include <qkvm/core/wiped.h>

using namespace Qt::StringLiterals;

namespace qkvm::core {

namespace {

// A picture's message begins with so many bytes that say what it is.
constexpr qsizetype headBytes = 16;
constexpr char kindH264 = 1;
constexpr char kindJpeg = 2;

// A word of the server's is never longer than this: it is a few names and numbers.
constexpr qsizetype longestWord = qsizetype{64} * 1024;

// The pointer's place, as the draft has it: each way 0 to 32767 over the picture.
constexpr int farthest = 32767;

// No picture is wider or higher than this; what a server says beyond it is no size.
constexpr int largestSide = 16384;

QString unreachable()
{
    return QString::fromLatin1(ServerClient::unreachable);
}

// A code as a server said it. This program's own codes have a ':' in them, and much hangs on
// them: one of those is not taken from a server, and `otherwise` stands in its place.
QString theServers(const QString& code, const QString& otherwise)
{
    return code.contains(u':') ? otherwise : code;
}

// The words a qKVM server ends a live line with (app/qkvm/live.py of 0.2.0: Line.end, _turn_away and
// the standing check's SAID). Any other ends it for good (LiveLink::endsForGood).
bool aGoodbyeThisProgramKnows(const QString& code)
{
    static const QStringList known{u"closed"_s,      u"slow"_s,        u"internal"_s, u"closing"_s,     u"no_pass"_s,
                                   u"no_live"_s,     u"gone"_s,        u"flooding"_s, u"bad_message"_s, u"signed_out"_s,
                                   u"unconfirmed"_s, u"not_allowed"_s, u"not_found"_s};
    return known.contains(code);
}

DeviceLine::Where liveWhere(const QNetworkRequest& request, const QByteArray& certificate)
{
    QUrl address = request.url();
    if (address.scheme() != "ws"_L1 && address.scheme() != "wss"_L1)
        return {};
    address.setScheme(address.scheme() == "wss"_L1 ? u"https"_s : u"http"_s);
    QByteArray path = address.path(QUrl::FullyEncoded).toLatin1();
    if (address.hasQuery())
        path += '?' + address.query(QUrl::FullyEncoded).toLatin1();
    return {address, path, {}, !certificate.isEmpty(), certificate, false};
}

DeviceLine::Limits liveLimits(const LiveLink::Watch& watch)
{
    DeviceLine::Limits limits;
    limits.letInWithinMs = watch.openWithinMs;
    limits.mostUnsentBytes = watch.mostUnsentBytes;
    limits.lateAfterMs = watch.silentAfterMs;
    limits.largestFrameBytes = largestPictureBytes + headBytes;
    // A bounded live-video budget: at most sixty maximum-sized pictures a second,
    // with headroom for frame headers and control messages. No limit is inferred
    // from a server's declared frame length before it has arrived.
    limits.mostBytesASecond = qint64{512} * 1024 * 1024;
    return limits;
}

} // namespace

LiveLink::LiveLink(const QNetworkRequest& request, QByteArray pass, Need need, const QByteArray& knownCertificate,
                   Watch watch, QObject* parent)
    : Live(parent), m_socket(liveWhere(request, knownCertificate), liveLimits(watch)), m_pass(std::move(pass)),
      m_need(need), m_watch(watch)
{
    // DeviceLine checks TLS before its sole upgrade request, disables proxies,
    // never implements HTTP authentication, and applies bounded message reception.
    m_pass.detach(); // This line owns the bytes it must wipe after the one use.
    m_socket.receiveLive();
    connect(&m_socket, &DeviceLine::letIn, this, [this] {
        m_opening.stop();
        m_open = true;
        // The line's first word: the pass, and what this program can show and needs.
        say({{u"t"_s, u"hello"_s},
             {u"v"_s, spokenLiveForm},
             {u"ticket"_s, QString::fromLatin1(m_pass)},
             {u"video"_s, QJsonArray{u"h264"_s, u"jpeg"_s}},
             {u"max_w"_s, m_need.maxWidth},
             {u"max_fps"_s, m_need.maxFps}});
        wipeInPlace(m_pass);
        m_asking.start();
        m_silence.start();
        emit opened();
    });
    connect(&m_socket, &DeviceLine::textCame, this, [this](const QByteArray& text) { take(QString::fromUtf8(text)); });
    connect(&m_socket, &DeviceLine::bytesCame, this, [this](const QByteArray& picture) { take(picture); });
    connect(&m_socket, &DeviceLine::heard, this, [this] { m_silence.start(); });
    connect(&m_socket, &DeviceLine::ended, this, [this](DeviceLine::End why) {
        switch (why) {
        case DeviceLine::End::Silent:
            end(unreachable(), u"qKVM did not answer for the live picture."_s);
            return;
        case DeviceLine::End::TakesNothing:
        case DeviceLine::End::Late:
            end(unreachable(), u"qKVM stopped taking what is sent on the live picture's line."_s);
            return;
        case DeviceLine::End::TlsDamaged:
            end(QString::fromLatin1(tlsDamaged), QString::fromLatin1(tlsDamagedSentence));
            return;
        case DeviceLine::End::CertificateChanged:
        case DeviceLine::End::CertificateUnread:
            end(QString::fromLatin1(ServerClient::certificateChanged),
                u"The server showed a certificate that is not the one it is known by."_s);
            return;
        case DeviceLine::End::CertificateUntrusted:
            end(QString::fromLatin1(ServerClient::certificateUnknown),
                u"The server showed a certificate that Windows does not trust."_s);
            return;
        case DeviceLine::End::CertificateOfUnknownSigner:
            end(QString::fromLatin1(ServerClient::certificateSignerUnknown),
                u"Windows does not trust the signer of the server's certificate."_s);
            return;
        default:
            break;
        }
        if (!m_byeCode.isEmpty()) {
            end(m_byeCode, m_byeMessage);
        } else {
            end(unreachable(), m_open ? u"The live picture's line to qKVM was closed."_s
                                      : u"qKVM could not be reached for the live picture."_s);
        }
    });

    m_opening.setSingleShot(true);
    m_opening.setInterval(m_watch.openWithinMs);
    connect(&m_opening, &QTimer::timeout, this,
            [this] { end(unreachable(), u"qKVM did not answer for the live picture."_s); });
    // A line with nothing on it for a while says nothing by itself: a still screen sends no
    // pictures. So the server is asked now and then whether it is there, and one of which
    // nothing at all has come for too long is gone.
    m_asking.setInterval(m_watch.askEveryMs);
    connect(&m_asking, &QTimer::timeout, this, [this] { static_cast<void>(m_socket.ping()); });
    m_silence.setSingleShot(true);
    m_silence.setInterval(m_watch.silentAfterMs);
    connect(&m_silence, &QTimer::timeout, this,
            [this] { end(unreachable(), u"qKVM stopped answering on the live picture's line."_s); });

    m_opening.start();
    // Begun when whoever made this can hear what it says: a line that cannot even be begun says
    // so there and then.
    QTimer::singleShot(0, this, [this] {
        if (!m_ended)
            m_socket.open();
    });
}

LiveLink::~LiveLink()
{
    m_closing = true;
    wipeInPlace(m_pass);
    // Thrown away while it holds keyboard and mouse: what can still be said is said. And
    // whatever was said last is pushed out before the line is cut.
    if (!m_ended) {
        releaseAll();
    }
    m_ended = true;
    static_cast<void>(m_socket.close());
}

bool LiveLink::endsForGood(const QString& code) const
{
    return !code.contains(u':') && !aGoodbyeThisProgramKnows(code);
}

void LiveLink::end(const QString& code, const QString& message)
{
    if (m_ended || m_closing) {
        return;
    }
    m_ended = true;
    wipeInPlace(m_pass);
    m_open = false;
    m_opening.stop();
    m_asking.stop();
    m_silence.stop();
    // Nothing more comes of the line, and nothing is kept for it. (What that sets off in here
    // finds the line ended already.)
    m_socket.cut();
    m_owner = {};
    emit controlOwnerChanged();
    if (m_control) {
        m_control = false;
        emit controlChanged(false, code, message);
    }
    emit closed(code, message);
}

void LiveLink::close()
{
    if (m_ended || m_closing) {
        return;
    }
    // A refused release can end DeviceLine synchronously; this is still our own close.
    m_closing = true;
    wipeInPlace(m_pass);
    // What is down on the machine is let go while the line can still say so.
    releaseAll();
    m_ended = true;
    wipeInPlace(m_pass);
    m_open = false;
    m_opening.stop();
    m_asking.stop();
    m_silence.stop();
    m_control = false;
    static_cast<void>(m_socket.close());
    m_owner = {};
    emit controlOwnerChanged();
}

void LiveLink::say(const QJsonObject& what)
{
    if (m_open && !m_ended) {
        static_cast<void>(m_socket.say(QJsonDocument(what).toJson(QJsonDocument::Compact)));
    }
}

void LiveLink::setNeed(Need need)
{
    if (need == m_need) {
        return;
    }
    m_need = need;
    say({{u"t"_s, u"shape"_s}, {u"max_w"_s, need.maxWidth}, {u"max_fps"_s, need.maxFps}});
}

void LiveLink::askForAKeyFrame()
{
    say({{u"t"_s, u"key_frame"_s}});
}

void LiveLink::askControl(bool on)
{
    if (!on) {
        // Given back: what is down is let go first, while the line still holds it.
        releaseAll();
    }
    say({{u"t"_s, u"control"_s}, {u"on"_s, on}});
}

void LiveLink::takeOver(const QString& expectedOwner)
{
    if (!m_open || m_control || expectedOwner.isEmpty() || expectedOwner != m_owner.reference) {
        return;
    }
    say({{u"t"_s, u"control"_s}, {u"on"_s, true}, {u"take"_s, true}, {u"from"_s, expectedOwner}});
}

void LiveLink::key(int usage, bool down)
{
    if (m_control && usage > 0 && usage <= 0xffff) {
        say({{u"t"_s, u"k"_s}, {u"u"_s, usage}, {u"d"_s, down}});
    }
}

void LiveLink::pointerAt(int x, int y)
{
    if (m_control && (m_pointer == Pointer::Absolute || m_pointer == Pointer::Both)) {
        say({{u"t"_s, u"m"_s}, {u"x"_s, std::clamp(x, 0, farthest)}, {u"y"_s, std::clamp(y, 0, farthest)}});
    }
}

void LiveLink::pointerBy(int dx, int dy)
{
    if (m_control && (m_pointer == Pointer::Relative || m_pointer == Pointer::Both) && (dx != 0 || dy != 0)) {
        say({{u"t"_s, u"mr"_s}, {u"dx"_s, dx}, {u"dy"_s, dy}});
    }
}

void LiveLink::button(int number, bool down)
{
    if (m_control && number >= 1 && number <= 5) {
        say({{u"t"_s, u"mb"_s}, {u"b"_s, number}, {u"d"_s, down}});
    }
}

void LiveLink::wheel(int dx, int dy)
{
    if (m_control && (dx != 0 || dy != 0)) {
        say({{u"t"_s, u"mw"_s}, {u"dx"_s, dx}, {u"dy"_s, dy}});
    }
}

void LiveLink::releaseAll()
{
    if (m_control) {
        say({{u"t"_s, u"up"_s}});
    }
}

void LiveLink::take(const QString& text)
{
    if (m_ended || m_closing) {
        return;
    }
    // Whatever it is, it came: the server is there.
    m_silence.start();
    if (text.size() > longestWord) {
        return;
    }
    const QJsonDocument read = QJsonDocument::fromJson(text.toUtf8());
    if (!read.isObject()) {
        return;
    }
    const QJsonObject said = read.object();
    const QString kind = said["t"_L1].toString();
    const auto word = [](const QJsonValue& value) { return value.toString().left(longestText); };
    if (kind == "video"_L1) {
        const QString codec = said["codec"_L1].toString();
        if (codec != "h264"_L1 && codec != "jpeg"_L1) {
            return;
        }
        Video video;
        video.h264 = codec == "h264"_L1;
        const auto side = [](const QJsonValue& value) {
            const int pixels = value.toInt();
            return pixels >= 1 && pixels <= largestSide ? pixels : 0;
        };
        video.width = side(said["w"_L1]);
        video.height = side(said["h"_L1]);
        emit videoChanged(video);
    } else if (kind == "state"_L1) {
        const QString pointer = said["pointer"_L1].toString();
        const auto mode = pointer == "absolute"_L1   ? Pointer::Absolute
                          : pointer == "relative"_L1 ? Pointer::Relative
                          : pointer == "both"_L1     ? Pointer::Both
                                                     : Pointer::Unknown;
        if (mode != m_pointer) {
            m_pointer = mode;
            emit pointerModeChanged();
        }
        // The machine's error as the server's list of how machines are has it.
        const QJsonObject error = said["error"_L1].toObject();
        emit stateChanged(word(said["state"_L1]), word(error["code"_L1]), word(error["message"_L1]));
    } else if (kind == "control"_L1) {
        if (!said["on"_L1].isBool())
            return;
        const bool on = said["on"_L1].toBool();
        ControlOwner owner;
        if (!on && said["code"_L1].toString() == "taken"_L1) {
            const QString name = said["by"_L1].toString();
            // The display name and the wire reference are separate. Never truncate a
            // reference into one that could accidentally identify somebody else.
            owner.name = name.left(longestText).simplified();
            const QJsonValue reference = said.contains("from"_L1) ? said["from"_L1] : said["by"_L1];
            if (!owner.name.isEmpty() && reference.isString() && reference.toString().size() <= longestText)
                owner.reference = reference.toString();
        } else if (!on && !m_control && said["code"_L1].toString().isEmpty()) {
            // A release acknowledgement after a refusal only concerns this line. It does
            // not say that the other holder left; keep the last report for confirmation.
            owner = m_owner;
        }
        if (owner != m_owner) {
            m_owner = owner;
            emit controlOwnerChanged();
        }
        if (on != m_control || !on) {
            m_control = on;
            emit controlChanged(on, theServers(word(said["code"_L1]), u"refused"_s), word(said["message"_L1]));
        }
    } else if (kind == "bye"_L1) {
        const QJsonValue code = said["code"_L1];
        m_byeMessage = word(said["message"_L1]);
        // (A server that says goodbye without a reason, no code, null or an empty one, has said that it
        // is going, and no more: a passing end. One whose reason is no server's word, one of this
        // program's own kind with a ':' in it, or a number, an object or a truth value, ended the line
        // for a reason this program cannot know: for good, endsForGood.)
        if (code.isUndefined() || code.isNull() || (code.isString() && code.toString().isEmpty())) {
            m_byeCode = u"closed"_s;
        } else if (!code.isString() || code.toString().contains(u':')) {
            m_byeCode = u"unrecognised"_s;
        } else {
            m_byeCode = word(code);
        }
    }
}

void LiveLink::take(const QByteArray& picture)
{
    if (m_ended || m_closing) {
        return;
    }
    m_silence.start();
    if (picture.size() <= headBytes) {
        return;
    }
    const char kind = picture.at(0);
    if (kind != kindH264 && kind != kindJpeg) {
        return;
    }
    Picture one;
    one.h264 = kind == kindH264;
    // A JPEG is a whole picture, whatever its flags say.
    one.keyFrame = !one.h264 || (picture.at(1) & 1) != 0;
    one.counter = qFromLittleEndian<quint32>(picture.constData() + 4);
    one.takenUs = qFromLittleEndian<qint64>(picture.constData() + 8);

    const bool gap = one.counter != m_lastCounter + 1;
    m_lastCounter = one.counter;
    if (!one.h264) {
        // H.264 that comes after a JPEG begins anew, as at the start.
        m_needsKeyFrame = true;
    } else if (one.keyFrame) {
        m_needsKeyFrame = false;
        m_askedForKeyFrame = false;
    } else if (gap || m_needsKeyFrame) {
        // It builds on a picture that never came. Asked for once; the server may have sent the
        // key frame already, as the draft has it do for whoever fell behind.
        m_needsKeyFrame = true;
        if (!m_askedForKeyFrame) {
            m_askedForKeyFrame = true;
            say({{u"t"_s, u"key_frame"_s}});
        }
        ++m_skipped;
        return;
    }
    one.data = picture.mid(headBytes);
    emit pictureCame(one);
}

} // namespace qkvm::core
