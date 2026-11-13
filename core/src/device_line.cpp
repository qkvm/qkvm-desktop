#include <qkvm/core/device_line.h>
#include <qkvm/core/video_diagnostics.h>

#include <algorithm>
#include <array>
#include <utility>

#include <QCryptographicHash>
#include <QNetworkProxy>
#include <QRandomGenerator>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslError>
#include <QStringDecoder>
#include <QTimer>

#include <qkvm/core/kvm_certificate.h>
#include <qkvm/core/kvmd_answers.h>
#include <qkvm/core/tls.h>

#include <winsock2.h>

using namespace Qt::StringLiterals;

namespace qkvm::core {

namespace {

// The room Windows gets for what this line has said and the KVM has not taken yet: some fifty
// keys' worth. (Windows' own choice is sixty-four kilobytes and more.)
constexpr int roomWithWindowsBytes = 4096;

// The most that is read of an answer to the asking, and the most messages a second a KVM has
// reason to send.
constexpr qsizetype longestHead = 8192;
// What may wait to be read: a whole message's frame, and never less than the longest answer that
// lets a line in, which is read through the same buffer (a smaller one never held all of it).
qint64 readBufferFor(const DeviceLine::Limits& limits)
{
    return std::max<qint64>(limits.largestFrameBytes + 14, longestHead + 4);
}
// What says "this is a WebSocket" in the answer, by the rule of WebSocket itself.
constexpr const char* webSocketMark = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// The kinds of message that are sent from here, by WebSocket's numbers for them, and its number
// for a goodbye with nothing wrong.
constexpr quint8 kindText = 1;
constexpr quint8 kindBytes = 2;
constexpr quint8 kindGoodbye = 8;
constexpr quint8 kindAreYouThere = 9;
constexpr quint8 kindIAm = 10;
constexpr quint16 nothingWrong = 1000;

// How many bytes a message of this length is on the line: how it begins, the four bytes it is
// hidden with, and itself.
constexpr qint64 onTheLine(qsizetype length)
{
    return length + (length <= 125 ? 6 : length <= 0xFFFF ? 8 : 14);
}

// One whole message the way a caller has to send it: hidden with four bytes that nobody could
// have known, new ones for each.
QByteArray message(quint8 kind, const QByteArray& what)
{
    const auto length = static_cast<quint64>(what.size());
    QByteArray whole;
    whole.reserve(onTheLine(what.size()));
    whole += static_cast<char>(0x80 | kind);
    if (length <= 125) {
        whole += static_cast<char>(0x80 | length);
    } else if (length <= 0xFFFF) {
        whole += static_cast<char>(0x80 | 126);
        whole += static_cast<char>((length >> 8) & 0xFF);
        whole += static_cast<char>(length & 0xFF);
    } else {
        whole += static_cast<char>(0x80 | 127);
        for (int shift = 56; shift >= 0; shift -= 8) {
            whole += static_cast<char>((length >> shift) & 0xFF);
        }
    }
    const quint32 hide = QRandomGenerator::system()->generate();
    for (int shift = 24; shift >= 0; shift -= 8) {
        whole += static_cast<char>((hide >> shift) & 0xFF);
    }
    for (qsizetype index = 0; index < what.size(); ++index) {
        const auto mask = static_cast<quint8>((hide >> (24 - 8 * (index % 4))) & 0xFF);
        whole += static_cast<char>(static_cast<quint8>(what.at(index)) ^ mask);
    }
    return whole;
}

// Whether the asking can carry these as they are (DeviceLine::Where says what that takes). Both
// are written into it raw: a line's end in either would make a header of somebody else's choosing
// out of it, or a second asking.
bool fitToSend(const DeviceLine::Where& where, bool live)
{
    const auto visible = [](char one) { return one > ' ' && one < '\x7F'; };
    const auto ofAName = [](char one) {
        return (one >= 'a' && one <= 'z') || (one >= 'A' && one <= 'Z') || (one >= '0' && one <= '9') || one == '_' ||
               one == '-';
    };
    const qsizetype sign = where.cookie.indexOf('=');
    if (live && !where.cookie.isEmpty()) {
        return false; // Server authentication is the first message, never a header.
    }
    const bool askingFits = where.asking.startsWith('/') && std::ranges::all_of(where.asking, visible);
    if (where.withoutCookie) {
        // (Nothing to say who asks, and nothing in its place either.)
        return where.cookie.isEmpty() && askingFits;
    }
    const bool cookieFits = live || (sign > 0 && std::ranges::all_of(where.cookie.first(sign), ofAName) &&
                                     kvmd::isToken(where.cookie.sliced(sign + 1)));
    return askingFits && cookieFits;
}

} // namespace

DeviceLine::DeviceLine(Where where, Limits limits, QObject* parent)
    : QObject(parent), m_where(std::move(where)), m_limits(limits)
{
    m_clock.start();
}

DeviceLine::~DeviceLine()
{
    // Nothing is heard of the socket's own end, and nothing said of this one's.
    m_over = true;
    disconnect(&m_socket, nullptr, this, nullptr);
    // Thrown away after its goodbye, before the KVM has ended it: the socket goes with this, and
    // Windows closes the line. Over what had come and was not read it would reset the line
    // instead (close() says what that costs): so that is taken off the line first, as much as a
    // KVM has reason to send in a second and no more. (Off the line itself, as it lies there:
    // over TLS that is what came hidden, which nobody reads. Tried both ways: without this, a
    // far end on this PC lost what was said last, in the clear and over TLS.)
    if (const qintptr socket = m_socket.socketDescriptor(); m_leaving && !m_gone && socket != -1) {
        std::array<char, 4096> some{};
        for (qint64 taken = 0; taken < m_limits.mostBytesASecond;) {
            const int came = ::recv(static_cast<SOCKET>(socket), some.data(), static_cast<int>(some.size()), 0);
            if (came <= 0) {
                break;
            }
            taken += came;
        }
    }
}

void DeviceLine::open()
{
    // Once. And not on a line that was ended from here before it was begun: nobody holds that
    // one, and the pass is not sent for nobody.
    if (m_opened || m_over) {
        return;
    }
    m_opened = true;
    if (!fitToSend(m_where, m_live)) {
        QTimer::singleShot(0, this, [this] { end(End::NotAsked); });
        return;
    }
    if (refusedForItsTls(m_windowsOwnTls, m_where.address)) {
        // Whoever would judge its certificate is not Windows: the asking, which carries the
        // pass, does not go out.
        QTimer::singleShot(0, this, [this] { end(End::TlsDamaged); });
        return;
    }
    const bool encrypted = m_where.address.scheme() == "https"_L1;
    const bool known = m_where.knownByCertificate && m_where.certificate.size() == 32;
    if (encrypted && m_where.knownByCertificate && !known) {
        QTimer::singleShot(0, this, [this] { end(End::CertificateUnread); });
        return;
    }
    m_socket.setProxy(QNetworkProxy::NoProxy);
    if (encrypted && known) {
        // Known by its certificate alone, as the driver has it for its questions: Windows
        // vouches for nobody on this line and objects to whatever is shown, the KVM's own
        // included.
        m_socket.setSslConfiguration(kvm::knownByItsCertificateAlone(QSslConfiguration::defaultConfiguration()));
    }
    const auto shown = [this] {
        const QSslCertificate certificate = m_socket.peerCertificate();
        return certificate.isNull() ? QByteArray() : certificate.digest(QCryptographicHash::Sha256);
    };
    connect(&m_socket, &QSslSocket::sslErrors, this, [this](const QList<QSslError>& errors) {
        // Windows objects to what the KVM showed. What comes of that is the driver's rule, by
        // the driver's code: passed over for the certificate the KVM is known by where every
        // objection names that one itself, and for nothing else. The line then ends by itself.
        switch (kvm::objectedTo(m_socket.peerCertificate(), errors, m_where.knownByCertificate, m_where.certificate)) {
        case kvm::Objected::PassedOver:
            m_socket.ignoreSslErrors(errors);
            break;
        case kvm::Objected::BySigner:
            m_objection = End::CertificateOfUnknownSigner;
            break;
        case kvm::Objected::Changed:
            m_objection = End::CertificateChanged;
            break;
        case kvm::Objected::Untrusted:
            m_objection = End::CertificateUntrusted;
            break;
        }
    });
    connect(&m_socket, &QSslSocket::encrypted, this, [this, known, shown] {
        // The two have agreed on how to speak, and nothing has been written: here the look is
        // made for every line, also for one whose certificate Windows had nothing against.
        if (known && shown() != m_where.certificate) {
            end(End::CertificateChanged);
            return;
        }
        ask();
    });
    connect(&m_socket, &QSslSocket::connected, this, [this, encrypted] {
        // Each message goes out when it is said. Left to itself, Windows holds a short one back
        // until the KVM has said that it got the one before: a key that comes late, a pointer
        // that moves in jumps. (Set, and not tried: on this PC's own address nothing is held
        // back either way, so no test here can tell.)
        m_socket.setSocketOption(QAbstractSocket::LowDelayOption, 1);
        // And Windows is given little room to hold what was said: what it holds is out of this
        // program's sight (see say()), and a key that waits there is one that may arrive late.
        m_socket.setSocketOption(QAbstractSocket::SendBufferSizeSocketOption, roomWithWindowsBytes);
        if (!encrypted) {
            ask();
        }
    });
    connect(&m_socket, &QSslSocket::readyRead, this, [this] { read(); });
    connect(&m_socket, &QSslSocket::disconnected, this, [this] { fell(); });
    connect(&m_socket, &QSslSocket::errorOccurred, this, [this] { fell(); });
    QTimer::singleShot(m_limits.letInWithinMs, this, [this] {
        if (!m_in) {
            end(m_objection.value_or(m_asked ? End::Silent : End::NotReached));
        }
    });
    const QString host = m_where.address.host();
    if (encrypted) {
        m_socket.connectToHostEncrypted(host, static_cast<quint16>(m_where.address.port(443)));
    } else {
        m_socket.connectToHost(host, static_cast<quint16>(m_where.address.port(80)));
    }
}

void DeviceLine::receiveText(QByteArray protocol)
{
    if (!m_opened && !protocol.isEmpty() && protocol.size() <= 64 && std::ranges::all_of(protocol, [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        })) {
        m_protocol = std::move(protocol);
        m_receivesText = true;
        m_socket.setReadBufferSize(readBufferFor(m_limits));
    }
}

void DeviceLine::receiveBytes(QByteArray protocol)
{
    if (!m_opened && !m_live && !m_receivesText && !protocol.isEmpty() && protocol.size() <= 64 &&
        std::ranges::all_of(protocol,
                            [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; })) {
        m_protocol = std::move(protocol);
        m_receivesBytes = true;
        m_socket.setReadBufferSize(readBufferFor(m_limits));
    }
}

void DeviceLine::receiveText()
{
    if (!m_opened && !m_live && !m_receivesBytes) {
        m_receivesText = true;
        m_socket.setReadBufferSize(readBufferFor(m_limits));
    }
}

void DeviceLine::hearShortTexts(qint64 mostBytes)
{
    if (!m_opened && !m_live && !m_receivesText && !m_receivesBytes && mostBytes > 0) {
        m_hearsUpTo = std::min(mostBytes, m_limits.largestFrameBytes);
    }
}

void DeviceLine::receiveLive()
{
    if (!m_opened && !m_receivesText && !m_receivesBytes) {
        m_live = true;
        m_socket.setReadBufferSize(readBufferFor(m_limits));
    }
}

void DeviceLine::holdBack(bool held)
{
    if (std::exchange(m_heldBack, held) == held || held) {
        return;
    }
    QTimer::singleShot(0, this, [this] {
        if (!m_heldBack && m_in && !m_over) {
            read();
        }
        // Qt stops taking from Windows once what waits fills the line's buffer, and does not by itself begin again
        // when that is taken here later (seen: a line held back took nothing more after it was let go). Its size said
        // anew has it look whether there is room now.
        if (!m_heldBack && m_in && !m_over) {
            const qint64 size = m_socket.readBufferSize();
            m_socket.setReadBufferSize(size + 1);
            m_socket.setReadBufferSize(size);
        }
    });
}

bool DeviceLine::ping()
{
    return sayAs(kindAreYouThere, {});
}

void DeviceLine::ask()
{
    // Sixteen bytes that nobody could have known, for the mark the answer has to carry.
    std::array<quint32, 4> random{};
    QRandomGenerator::system()->fillRange(random.data(), random.size());
    QByteArray sixteen;
    for (const quint32 four : random) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            sixteen += static_cast<char>((four >> shift) & 0xFF);
        }
    }
    m_key = sixteen.toBase64();
    const bool encrypted = m_where.address.scheme() == "https"_L1;
    QByteArray host = m_where.address.host(QUrl::FullyEncoded).toLatin1();
    if (host.contains(':')) {
        host = '[' + host + ']';
    }
    if (const int port = m_where.address.port(-1); port != -1 && port != (encrypted ? 443 : 80)) {
        host += ':' + QByteArray::number(port);
    }
    // Who asks: a server's live line says it in its first message, after the line is in; a line asked
    // for with nothing to say who asks says nothing (Where::withoutCookie).
    const QByteArray whoAsks = m_live                  ? QByteArray("X-QKVM: 1\r\n")
                               : m_where.withoutCookie ? QByteArray()
                                                       : "Cookie: " + m_where.cookie + "\r\n";
    // The page it is asked from, where it is one of the KVM's own (a management controller's console).
    const QByteArray from =
        m_receivesBytes ? "Origin: " + m_where.address.scheme().toLatin1() + "://" + host + "\r\n" : QByteArray();
    m_asked = true;
    write("GET " + m_where.asking + " HTTP/1.1\r\nHost: " + host +
          "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + m_key +
          "\r\nSec-WebSocket-Version: 13\r\n" + whoAsks + from +
          (m_protocol.isEmpty() ? QByteArray{} : "Sec-WebSocket-Protocol: " + m_protocol + "\r\n") + "\r\n");
}

std::optional<DeviceLine::End> DeviceLine::refusedBy(const QByteArray& head) const
{
    // "You are in", in WebSocket's own words, and with the mark that only something which read
    // this asking can make. Whatever else it says (go there, sign on, no) is "no". Of a "no"
    // only this much is looked at: whether it is "not you", which whoever made the line can
    // do something about.
    const QList<QByteArray> lines = head.split('\n');
    const QByteArray first = lines.value(0);
    const bool signalling = !m_protocol.isEmpty() && !m_receivesBytes;
    if (signalling) {
        VideoDiagnostics::record("signaling_http", {{"status", first.split(' ').value(1).toInt()}});
    }
    if (!first.startsWith("HTTP/1.1 101")) {
        return first.startsWith("HTTP/1.1 401") || first.startsWith("HTTP/1.1 403") ? End::SignedOut : End::NotLetIn;
    }
    if (!m_protocol.isEmpty()) {
        int protocols = 0;
        for (const QByteArray& line : lines) {
            const qsizetype colon = line.indexOf(':');
            if (colon > 0 && line.first(colon).trimmed().toLower() == "sec-websocket-protocol") {
                if (line.sliced(colon + 1).trimmed() != m_protocol) {
                    if (signalling) {
                        VideoDiagnostics::record("signaling_wrong_subprotocol");
                    }
                    return End::NotLetIn;
                }
                ++protocols;
            }
        }
        if (protocols != 1) {
            if (signalling) {
                VideoDiagnostics::record("signaling_missing_subprotocol", {{"count", protocols}});
            }
            return End::NotLetIn;
        }
    }
    // A server's live line, and a line that reads what a device says with no subprotocol: "you are in"
    // in WebSocket's own words, and nothing that nobody asked for.
    if (m_live || (m_receivesText && m_protocol.isEmpty())) {
        bool upgrade = false;
        bool connection = false;
        int accepts = 0;
        for (const QByteArray& line : lines) {
            const qsizetype colon = line.indexOf(':');
            if (colon <= 0)
                continue;
            const QByteArray name = line.first(colon).trimmed().toLower();
            const QByteArray value = line.sliced(colon + 1).trimmed().toLower();
            if (name == "sec-websocket-extensions" || name == "sec-websocket-protocol")
                return End::NotLetIn;
            if (name == "upgrade") {
                if (upgrade || value != "websocket")
                    return End::NotLetIn;
                upgrade = true;
            }
            if (name == "connection") {
                for (const auto& token : value.split(','))
                    connection = connection || token.trimmed() == "upgrade";
            }
            if (name == "sec-websocket-accept")
                ++accepts;
        }
        if (!upgrade || !connection || accepts != 1 || first.split(' ').value(1).trimmed() != "101")
            return End::NotLetIn;
    }
    const QByteArray mark = QCryptographicHash::hash(m_key + webSocketMark, QCryptographicHash::Sha1).toBase64();
    for (qsizetype index = 1; index < lines.size(); ++index) {
        const qsizetype colon = lines.at(index).indexOf(':');
        if (colon > 0 && lines.at(index).left(colon).trimmed().toLower() == "sec-websocket-accept") {
            if (lines.at(index).mid(colon + 1).trimmed() == mark) {
                return std::nullopt;
            }
            break;
        }
    }
    return End::NotLetIn;
}

void DeviceLine::read()
{
    if (!m_in && !m_over) {
        // The answer to the asking, read to its end and not a byte further: what comes after it
        // in the same breath is a message already.
        const QByteArray seen = m_socket.peek(longestHead + 4);
        const qsizetype stop = seen.indexOf("\r\n\r\n");
        if (stop < 0) {
            if (seen.size() > longestHead) {
                end(End::NotLetIn);
            }
            return;
        }
        if (const std::optional<End> refused = refusedBy(m_socket.read(stop + 4))) {
            end(*refused);
            return;
        }
        m_in = true;
        emit letIn(); // (whoever hears that may close the line there and then)
    }
    if (m_over) {
        // Closed from here and not gone yet: what still comes is nobody's, and is thrown away.
        // Looked at for one thing, while the KVM's ending of the line is waited for: its own
        // goodbye, the one sign that it got what was said (noted where it is read). Until that
        // has come, or something that is no message: after either, nothing more is looked at.
        if (!m_socket.isOpen()) {
            return;
        }
        if (m_awaitsGoodbye && throwAway().has_value()) {
            m_awaitsGoodbye = false;
        }
        if (!m_awaitsGoodbye) {
            m_socket.skip(m_socket.bytesAvailable());
        }
        return;
    }
    const qint64 before = m_socket.bytesAvailable();
    if (const std::optional<End> why = throwAway()) {
        end(*why);
    } else if (unsent() > m_limits.mostUnsentBytes) {
        end(End::TakesNothing);
    } else if (m_socket.bytesAvailable() < before) {
        emit heard(); // (whoever hears that may end the line there and then: nothing is done after it)
    }
}

std::optional<DeviceLine::End> DeviceLine::throwAway()
{
    while (m_socket.bytesAvailable() > 0) {
        if (m_heldBack && !m_over) {
            return std::nullopt; // (held back from inside what was handed out last: the rest waits)
        }
        if (m_toSkip > 0) {
            const qint64 skipped = m_socket.skip(m_toSkip);
            if (skipped < 0) {
                return End::Fell;
            }
            m_toSkip -= skipped;
            if (tooMuch(0, skipped)) {
                return End::Misbehaved; // more in a second than a KVM has reason to send
            }
            if (m_toSkip > 0) {
                return std::nullopt; // the rest of it has not come yet
            }
            continue;
        }
        // How a message begins: what it is, and how long.
        const QByteArray begun = m_socket.peek(10);
        if (begun.size() < 2) {
            return std::nullopt;
        }
        const auto first = static_cast<quint8>(begun.at(0));
        const auto second = static_cast<quint8>(begun.at(1));
        const int kind = first & 0x0F;
        const int said = second & 0x7F;
        const qsizetype beginning = said == 126 ? 4 : said == 127 ? 10 : 2;
        // Nothing of what this program never agreed to, and nothing hidden as a caller hides it.
        if ((first & 0x70) != 0 || (second & 0x80) != 0 || (kind > 2 && kind < 8) || kind > 10) {
            return End::Misbehaved;
        }
        if (begun.size() < beginning) {
            return std::nullopt;
        }
        qint64 length = said;
        if (said >= 126) {
            length = 0;
            for (qsizetype index = 2; index < beginning; ++index) {
                length = (length << 8) | static_cast<quint8>(begun.at(index));
            }
            if (length < 0) {
                return End::Misbehaved;
            }
        }
        // Refused by what it says of itself, before any of it is waited for: thrown away at the
        // pace it came, a message without end would keep this thread reading for as long.
        if (length > m_limits.largestFrameBytes) {
            return End::Misbehaved;
        }
        if (kind >= kindGoodbye) {
            // "Are you there", its answer, and "goodbye": short by their own rule.
            if (said > 125 || (first & 0x80) == 0) {
                return End::Misbehaved;
            }
            if (m_socket.bytesAvailable() < beginning + said) {
                return std::nullopt;
            }
            m_socket.skip(beginning);
            const QByteArray with = m_socket.read(said);
            if (kind == kindGoodbye) {
                // It said goodbye. Behind the goodbye from here, that is its answer to it: it
                // got that one, and all that was said before it.
                m_answered = m_leaving;
                return End::Fell;
            }
            if (tooMuch(1, beginning + said)) {
                return End::Misbehaved;
            }
            if (kind == kindAreYouThere && !m_leaving) {
                // Answered with what it sent, hidden the way a caller has to hide what it sends.
                // (Not behind the goodbye from here, after which nothing is said.)
                write(message(kindIAm, with));
            }
            continue;
        }
        if ((m_live || m_receivesText || m_receivesBytes) && !m_over) {
            const bool final = (first & 0x80) != 0;
            const int messageKind = kind == 0 ? m_fragmentedKind : kind;
            const qint64 limit = m_live && messageKind == kindText ? qint64{64} * 1024 : m_limits.largestFrameBytes;
            // What each line takes: a console's bytes alone, a server's live line both, any other text alone.
            const bool taken =
                kind == 0 || (m_receivesBytes ? kind == kindBytes : kind == kindText || (m_live && kind == kindBytes));
            if (!taken || (kind == 0) != (m_fragmentedKind != 0) || m_text.size() + length > limit) {
                return End::Misbehaved;
            }
            if (m_socket.bytesAvailable() < beginning + length) {
                return std::nullopt;
            }
            m_socket.skip(beginning);
            m_text.append(m_socket.read(length));
            if (tooMuch(1, beginning + length)) {
                return End::Misbehaved;
            }
            m_fragmentedKind = final ? 0 : messageKind;
            if (final) {
                QByteArray complete = std::exchange(m_text, {});
                if (messageKind == kindBytes) {
                    emit bytesCame(complete);
                } else {
                    QStringDecoder utf8(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
                    const QString checked = utf8.decode(complete);
                    if (utf8.hasError() || checked.contains(QChar::Null)) {
                        return End::Misbehaved;
                    }
                    emit textCame(complete);
                }
                if (m_over) {
                    return std::nullopt;
                }
            }
            continue;
        }
        if (m_hearsUpTo > 0 && kind == kindText && (first & 0x80) != 0 && length <= m_hearsUpTo) {
            // A short text in one part, on a line that hears those (hearShortTexts): handed out whole, as it came.
            if (m_socket.bytesAvailable() < beginning + length) {
                return std::nullopt;
            }
            m_socket.skip(beginning);
            const QByteArray whole = m_socket.read(length);
            if (tooMuch(1, beginning + length)) {
                return End::Misbehaved;
            }
            emit textCame(whole);
            if (m_over) {
                return std::nullopt;
            }
            continue;
        }
        m_socket.skip(beginning);
        if (tooMuch(1, beginning)) {
            return End::Misbehaved; // more messages in a second than a KVM has reason to send
        }
        m_toSkip = length;
    }
    return std::nullopt;
}

bool DeviceLine::tooMuch(int messages, qint64 bytes)
{
    const qint64 second = m_clock.elapsed() / 1000;
    if (second != m_second) {
        m_second = second;
        m_heard = 0;
        m_heardBytes = 0;
    }
    m_heard += messages;
    m_heardBytes += bytes;
    return m_heard > m_limits.mostMessagesASecond || m_heardBytes > m_limits.mostBytesASecond;
}

void DeviceLine::write(const QByteArray& bytes)
{
    forgetWhatLeft();
    m_socket.write(bytes);
    m_written += bytes.size();
    const qint64 now = m_clock.elapsed();
    if (!m_waiting.isEmpty() && m_waiting.last().atMs == now) {
        m_waiting.last().upTo = m_written;
    } else {
        m_waiting.append({m_written, now});
    }
}

void DeviceLine::forgetWhatLeft()
{
    // All that was written, less what still waits, has left. (Over TLS what waits is counted
    // hidden, which is more bytes than it was written as: so less is taken to have left than
    // has, and a line is found late a little sooner for it, never later.)
    const qint64 left = m_written - unsent();
    while (!m_waiting.isEmpty() && m_waiting.first().upTo <= left) {
        m_waiting.removeFirst();
    }
}

bool DeviceLine::late()
{
    forgetWhatLeft();
    return !m_waiting.isEmpty() && m_clock.elapsed() - m_waiting.first().atMs > m_limits.lateAfterMs;
}

qint64 DeviceLine::waitedMs()
{
    forgetWhatLeft();
    return m_waiting.isEmpty() ? 0 : m_clock.elapsed() - m_waiting.first().atMs;
}

qint64 DeviceLine::unsent() const
{
    // Over TLS what was written leaves the socket's own count a turn of the loop later, hidden
    // and handed on, whether or not the KVM takes it: it is counted where it waits then, too.
    return m_socket.bytesToWrite() + m_socket.encryptedBytesToWrite();
}

bool DeviceLine::say(const QByteArray& text)
{
    return sayAs(kindText, text);
}

bool DeviceLine::sayBytes(const QByteArray& bytes)
{
    return sayAs(kindBytes, bytes);
}

bool DeviceLine::sayAs(quint8 kind, const QByteArray& text)
{
    if (!m_in || m_over) {
        return false;
    }
    // What waits is offered to Windows first: only what Windows does not take is waiting, and
    // not what this program was too busy to hand on. (Offering it can find the line fallen.)
    if (unsent() > 0) {
        m_socket.flush();
        if (m_over) {
            return false;
        }
    }
    // Nothing is said behind what has waited too long: all of it would arrive at once when the
    // way comes back, a key from then pressed now.
    if (late()) {
        end(End::Late);
        return false;
    }
    // Looked at before the text is taken in: what waits never becomes more than may.
    if (unsent() + onTheLine(text.size()) > m_limits.mostUnsentBytes) {
        end(End::TakesNothing);
        return false;
    }
    write(message(kind, text));
    // Handed to Windows now, and not at the next turn of the loop.
    m_socket.flush();
    return !m_over;
}

bool DeviceLine::close()
{
    if (std::exchange(m_closed, true)) {
        return false;
    }
    if (m_over) {
        // It ended by itself, or was cut, before this: there is nothing to wait for.
        QTimer::singleShot(0, this, [this] { sayGone(); });
        return false;
    }
    const bool wasIn = std::exchange(m_in, false);
    m_over = true;
    if (wasIn && unsent() > 0) {
        m_socket.flush(); // as before a text is said: what Windows takes now has not waited
    }
    // Nothing was agreed on it yet, and there is nobody to say goodbye to. Or what was said has
    // waited too long already: it is not pushed out now, to arrive when the way comes back.
    if (!wasIn || late()) {
        reset();
        QTimer::singleShot(0, this, [this] { sayGone(); });
        return false;
    }
    QByteArray why;
    why += static_cast<char>(nothingWrong >> 8);
    why += static_cast<char>(nothingWrong & 0xFF);
    write(message(kindGoodbye, why));
    // Pushed out now: whoever closes a line may throw it away in the same breath, and what was
    // said on it last is to have left by then.
    m_socket.flush();
    // The KVM ends a line it was said goodbye on, as the book has it, and that is waited for: the
    // end of the line does not travel with the last that was said on it. (Ended from here in
    // the same breath, a line on which something of the KVM's had come and was not read is
    // reset by Windows, and a far end that is Windows too throws away what it had not taken:
    // the tests' made-up KVMs lost the last "up" that way, the one that reads its line by hand
    // as the one that is Qt's own WebSocket, each time it was tried.) A KVM that does not end
    // it, or takes nothing off the line, is not waited for longer: what has not gone by then
    // has waited too long, and goes with the line.
    m_leaving = true;
    m_awaitsGoodbye = true;
    QTimer::singleShot(m_limits.goodbyeWithinMs, this, [this] {
        if (!m_gone) {
            reset();
            sayGone();
        }
    });
    // (Pushed out as far as Windows took it: offering it can find the line fallen.)
    return !m_gone && m_socket.state() == QAbstractSocket::ConnectedState;
}

void DeviceLine::fell()
{
    if (m_leaving) {
        // The KVM has ended the line that was closed from here: all of it has gone.
        sayGone();
        return;
    }
    end(fallen());
}

void DeviceLine::sayGone()
{
    if (!std::exchange(m_gone, true)) {
        emit gone(m_answered);
    }
}

void DeviceLine::cut()
{
    if (m_over) {
        return;
    }
    m_over = true;
    m_in = false;
    reset();
}

void DeviceLine::reset()
{
    // What waits in this program to leave goes with the line. What Windows had taken would not:
    // left to itself, Windows goes on sending it for as long as the far end takes it, and a key
    // typed into a line that stalled would be played when the way came back. So the line is
    // reset, not closed: what Windows holds for it is thrown away with it. (That it is reset a
    // test shows, by what the far end's Windows says of the line, and that on this PC nothing
    // more arrives after it. Set, and not shown by a test: a way that itself stalls, which no
    // test on one PC can make. There the far end hears of no reset, and what its own system
    // has taken is its own: a KVM whose program reads again later may play that, and only the
    // watch on the line bounds how much it is.)
    if (const qintptr socket = m_socket.socketDescriptor(); socket != -1) {
        const linger atOnce{.l_onoff = 1, .l_linger = 0};
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): Winsock takes every option as bytes
        ::setsockopt(static_cast<SOCKET>(socket), SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&atOnce),
                     static_cast<int>(sizeof(atOnce)));
    }
    m_socket.abort();
}

DeviceLine::End DeviceLine::fallen() const
{
    if (m_objection) {
        return *m_objection;
    }
    return m_asked ? End::Fell : End::NotReached;
}

void DeviceLine::end(End why)
{
    if (m_over) {
        return;
    }
    if (!m_protocol.isEmpty() && !m_receivesBytes) {
        VideoDiagnostics::record("signaling_failure", {{"reason", static_cast<int>(why)},
                                                       {"socket_error", static_cast<int>(m_socket.error())}});
    }
    m_over = true;
    m_in = false;
    // As a line that is cut: nothing of what was said on it arrives after it has ended, the
    // keys that waited too long least of all.
    reset();
    emit ended(why);
}

} // namespace qkvm::core
