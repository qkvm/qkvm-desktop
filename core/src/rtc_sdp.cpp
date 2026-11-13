#include "rtc_sdp.h"

#include <QHostAddress>
#include <QList>
#include <QMap>
#include <QRegularExpression>
#include <algorithm>
#include <qkvm/core/h264_rtp.h>

using namespace Qt::StringLiterals;
namespace qkvm::core {
namespace {
constexpr int largestSdp = 64 * 1024;

// Whether this program and the library read the description's lines alike (2e, the second reading): no
// line begins with whitespace (this program's reading trims it, the library's does not), and no section
// says its mid twice (this program took the first, the library keeps the last).
bool readAlike(const QByteArray& sdp)
{
    int mids = 0;
    for (const QByteArray& line : sdp.split('\n')) {
        const char first = line.isEmpty() ? 'x' : line.front();
        if (first == ' ' || first == '\t' || first == '\v' || first == '\f' || first == '\r') {
            return false;
        }
        // (The library takes a bare "a=mid", or one with whitespace after it, as a mid that is empty: counted
        // too, the reading of 56258cd, L3.)
        const bool mid = line.startsWith("a=mid:") || line.trimmed() == "a=mid" || line.startsWith("a=mid ") ||
                         line.startsWith("a=mid\t");
        if (line.startsWith("m=")) {
            mids = 0;
        } else if (mid && ++mids > 1) {
            return false;
        }
    }
    return true;
}
} // namespace

bool hasApplication(const QByteArray& sdp)
{
    if (sdp.isEmpty() || sdp.size() > largestSdp || sdp.contains('\0')) {
        return false;
    }
    int applications = 0;
    bool open = false;
    for (QByteArray line : sdp.split('\n')) {
        line = line.trimmed();
        if (line.startsWith("m=application ")) {
            const QList<QByteArray> words = line.split(' ');
            ++applications;
            open = words.size() >= 3 && words[1] != "0";
        }
    }
    return applications == 1 && open;
}

std::optional<QByteArray> applicationMid(const QByteArray& sdp)
{
    if (!hasApplication(sdp) || !readAlike(sdp)) {
        return std::nullopt;
    }
    bool inIt = false;
    for (QByteArray line : sdp.split('\n')) {
        line = line.trimmed();
        if (line.startsWith("m=")) {
            inIt = line.startsWith("m=application ");
        } else if (inIt && line.startsWith("a=mid:")) {
            return line.sliced(6);
        }
    }
    return std::nullopt;
}

WithoutAddresses withoutAddresses(const QByteArray& sdp)
{
    WithoutAddresses out;
    for (const QByteArray& line : sdp.split('\n')) {
        const QByteArray trimmed = line.trimmed();
        if (trimmed.startsWith("a=candidate:")) {
            out.candidates.append(trimmed.sliced(2));
        } else if (trimmed != "a=end-of-candidates" && !line.isEmpty()) {
            out.rest += line + '\n';
        }
    }
    return out;
}

JudgedAddress judgeAddress(const QByteArray& candidate, bool loopbackAllowed)
{
    if (!candidate.startsWith("candidate:")) {
        return {}; // (malformed)
    }
    // Each byte a visible one of ASCII, or a space that stands alone between two words: the library splits
    // a candidate at whitespace of any kind, and reads what it splits there and nothing else (2e, the second
    // reading).
    const QByteArray rest = candidate.sliced(10);
    for (qsizetype at = 0; at < rest.size(); ++at) {
        const auto byte = static_cast<unsigned char>(rest[at]);
        const bool visible = byte >= 0x21 && byte <= 0x7E;
        const bool lone = byte == ' ' && at > 0 && at + 1 < rest.size() && rest[at - 1] != ' ';
        if (!visible && !lone) {
            return {}; // (malformed)
        }
    }
    // foundation, component, transport, priority, address, port, "typ", type, and what may follow.
    const QList<QByteArray> words = rest.split(' ');
    const auto digits = [](const QByteArray& word, qsizetype most) {
        return !word.isEmpty() && word.size() <= most &&
               std::ranges::all_of(word, [](char c) { return c >= '0' && c <= '9'; });
    };
    const auto iceChar = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '+' || c == '/';
    };
    static const QList<QByteArray> types{"host", "srflx", "prflx", "relay"};
    if (words.size() < 8 || words[0].isEmpty() || words[0].size() > 32 || !std::ranges::all_of(words[0], iceChar) ||
        !digits(words[1], 3) || !digits(words[3], 10) || !digits(words[5], 5) || words[6] != "typ" ||
        !types.contains(words[7])) {
        return {}; // (malformed)
    }
    const int component = words[1].toInt();
    const qulonglong priority = words[3].toULongLong();
    const int port = words[5].toInt();
    if (component < 1 || component > 256 || priority > 0xFFFFFFFFULL || port < 1 || port > 65535) {
        return {}; // (malformed)
    }
    if (words[2].toLower() != "udp") {
        return {AddressFault::NotUdp, {}};
    }
    // A number, written as one: four parts of IPv4, or IPv6 (which has colons). Anything else would be looked
    // up by the library, on a thread of its own, at a name the far end chose.
    const QByteArray& host = words[4];
    // An IPv4 part written with a nought before it (as in 0127.0.0.1, or the IPv4 inside an IPv6 one) is no
    // number here: Qt reads it as octal, and another reader may read it as decimal, or look a name up (the
    // reading of 56258cd, L1).
    for (const QByteArray& part : host.mid(host.lastIndexOf(':') + 1).split('.')) {
        if (host.contains('.') && part.size() > 1 && part.front() == '0') {
            return {}; // (malformed)
        }
    }
    static const QRegularExpression four(u"^[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}$"_s);
    static const QRegularExpression six(u"^[0-9A-Fa-f:.]+$"_s);
    const QString text = QString::fromLatin1(host);
    QHostAddress address;
    if ((!four.match(text).hasMatch() && !(host.contains(':') && six.match(text).hasMatch())) ||
        !address.setAddress(text)) {
        return {AddressFault::Named, {}};
    }
    // (An IPv4 address written as IPv6 is judged as the IPv4 one it is.)
    bool mapped = false;
    const quint32 v4 = address.toIPv4Address(&mapped);
    const QHostAddress judged = mapped ? QHostAddress(v4) : address;
    // 0.0.0.0/8 ("this network", 0.0.0.0 among it), and ::/96 but ::1 (unspecified, and IPv4 written in the
    // old way, which some stacks send as IPv4 and some never send at all): nowhere to send to (2e, the second
    // reading).
    bool inThisNetwork = false;
    bool oldIpv4 = false;
    if (judged.protocol() == QAbstractSocket::IPv4Protocol) {
        inThisNetwork = (judged.toIPv4Address() >> 24) == 0;
    } else {
        const Q_IPV6ADDR bytes = judged.toIPv6Address();
        oldIpv4 = std::all_of(bytes.c, bytes.c + 12, [](quint8 byte) { return byte == 0; }) && !judged.isLoopback();
    }
    if (inThisNetwork || oldIpv4 || judged.isMulticast() || judged.isBroadcast() ||
        (judged.isLoopback() && !loopbackAllowed)) {
        return {AddressFault::Unreachable, {}};
    }
    // Made anew of what was judged: the far end's own text never reaches the library.
    QByteArray made = "candidate:" + words[0] + ' ' + QByteArray::number(component) + " UDP " +
                      QByteArray::number(priority) + ' ' + judged.toString().toLatin1() + ' ' +
                      QByteArray::number(port) + " typ " + words[7];
    return {AddressFault::None, std::move(made)};
}

bool spokenToOnThisPc(const QUrl& address)
{
    const QString host = address.host();
    return host == u"localhost"_s || QHostAddress(host).isLoopback();
}

std::optional<VideoChoice> chooseVideo(const QByteArray& sdp, const QList<int>& onlyThese)
{
    if (sdp.isEmpty() || sdp.size() > largestSdp || sdp.contains('\0') || !sdp.startsWith("v=0\r\n") ||
        !readAlike(sdp)) {
        return {};
    }
    const QList<QByteArray> lines = sdp.split('\n');
    bool video = false;
    int videos = 0;
    int media = 0;
    bool fingerprint = false;
    QByteArray mid;
    QList<int> offered;
    QList<int> h264;
    QMap<int, QByteArray> formats;
    for (QByteArray line : lines) {
        line = line.trimmed();
        if (line.size() > 4096) {
            return {};
        }
        if (line.startsWith("a=fingerprint:sha-256 ")) {
            fingerprint = true;
        }
        if (line.startsWith("m=")) {
            if (++media > 8) {
                return {};
            }
            video = line.startsWith("m=video ");
            if (video) {
                if (++videos > 1) {
                    return {};
                }
                const QList<QByteArray> words = line.split(' ');
                if (words.size() < 4 || words[1] == "0" || words[2] != "UDP/TLS/RTP/SAVPF") {
                    return {};
                }
                for (qsizetype i = 3; i < words.size(); ++i) {
                    bool ok = false;
                    const int pt = words[i].toInt(&ok);
                    if (!ok || pt < 96 || pt > 127 || offered.contains(pt)) {
                        return {};
                    }
                    offered.append(pt);
                }
            }
        } else if (video) {
            if (line == "a=recvonly" || line == "a=inactive") {
                return {};
            }
            if (line.startsWith("a=mid:")) {
                mid = line.sliced(6);
            }
            if (line.startsWith("a=rtpmap:")) {
                const QList<QByteArray> words = line.sliced(9).split(' ');
                if (words.size() == 2 && words[1].toUpper() == "H264/90000") {
                    h264.append(words[0].toInt());
                }
            }
            if (line.startsWith("a=fmtp:")) {
                const qsizetype space = line.indexOf(' ');
                if (space > 7) {
                    formats.insert(line.sliced(7, space - 7).toInt(), line.sliced(space + 1));
                }
            }
        }
    }
    const auto token = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    };
    if (!fingerprint || videos != 1 || mid.isEmpty() || mid.size() > 64 || !std::ranges::all_of(mid, token)) {
        return {};
    }
    // (An answer to an offer of ours: only what we offered, in the video line's own order.)
    QList<int> candidates = h264;
    if (!onlyThese.isEmpty()) {
        candidates.clear();
        for (const int pt : offered) {
            if (h264.contains(pt) && onlyThese.contains(pt)) {
                candidates.append(pt);
            }
        }
    }
    for (const int pt : candidates) {
        if (!offered.contains(pt)) {
            continue;
        }
        QMap<QByteArray, QByteArray> params;
        for (const QByteArray& field : formats.value(pt).split(';')) {
            const qsizetype equals = field.indexOf('=');
            if (equals > 0) {
                params.insert(field.first(equals).trimmed(), field.sliced(equals + 1).trimmed());
            }
        }
        if (params.value("packetization-mode") != "1") {
            continue;
        }
        const QByteArray profile = params.value("profile-level-id", "42e01f").toLower();
        const QRegularExpression hex(u"^[0-9a-f]{6}$"_s);
        if (!hex.match(QString::fromLatin1(profile)).hasMatch() ||
            (profile.first(2) != "42" && profile.first(2) != "4d" && profile.first(2) != "64")) {
            continue;
        }
        VideoChoice choice{pt, mid, formats.value(pt), {}, {}};
        if (params.contains("sprop-parameter-sets")) {
            const QList<QByteArray> sets = params.value("sprop-parameter-sets").split(',');
            if (sets.size() != 2) {
                continue;
            }
            const auto a = QByteArray::fromBase64Encoding(sets[0], QByteArray::AbortOnBase64DecodingErrors);
            const auto b = QByteArray::fromBase64Encoding(sets[1], QByteArray::AbortOnBase64DecodingErrors);
            H264Rtp check(pt);
            if (!a || !b || !check.parameters(a.decoded, b.decoded)) {
                continue;
            }
            choice.sps = a.decoded;
            choice.pps = b.decoded;
        }
        return choice;
    }
    return {};
}

} // namespace qkvm::core
