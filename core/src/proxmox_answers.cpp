#include <qkvm/core/proxmox_answers.h>

#include <algorithm>
#include <cmath>

#include <QCryptographicHash>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QUrlQuery>

#include <qkvm/core/kvm_probe.h>
#include <qkvm/core/kvmd_answers.h>

using namespace Qt::StringLiterals;

namespace qkvm::core::proxmox {

namespace {

// The longest that is read of what was typed for an address, as for any device (kvm::addressFrom).
constexpr qsizetype longestTyped = 2048;
// The most a guest's number is written with, and the bounds of what a Proxmox says of a guest (the web's).
constexpr qsizetype longestNumber = 9;
constexpr double mostCpus = 4096;
constexpr double mostMemory = 1125899906842624.0; // 2^50 bytes
constexpr double mostUptime = 1099511627776.0;    // 2^40 seconds
constexpr double bytesInAGib = 1073741824.0;
// The longest drive setting, and boot order, read (the web's), and the largest size an image is said to have.
constexpr qsizetype longestDriveSetting = 2048;
constexpr qsizetype longestBootOrder = 1024;
constexpr double mostImageBytes = 1125899906842624.0; // 2^50

base::Error notAGuest(QStringView text)
{
    return {"invalid", (u"Not a Proxmox guest's address: \""_s + text.left(200).toString() + u'"').toStdString()};
}
base::Error notAProxmox(QStringView text, QStringView why = {})
{
    return {"invalid", (u"Not a Proxmox address: \""_s + text.left(200).toString() + u'"' +
                        (why.isEmpty() ? QString() : u" ("_s + why.toString() + u')'))
                           .toStdString()};
}
base::Error aContainer()
{
    return {"invalid", "A container's console is not shown on the wall yet: only virtual machines"};
}

// A guest's number as written: digits alone, nine at the most, from 100 on.
std::optional<qint64> numberIn(QStringView text)
{
    if (text.isEmpty() || text.size() > longestNumber ||
        !std::ranges::all_of(text, [](QChar sign) { return sign >= u'0' && sign <= u'9'; })) {
        return std::nullopt;
    }
    const qint64 vmid = text.toLongLong();
    return isGuestNumber(vmid) ? std::optional<qint64>(vmid) : std::nullopt;
}

// A whole number of an answer's, within bounds; none for anything else (a text, a flag, a fraction).
std::optional<qint64> wholeIn(const QJsonValue& value, double least, double most)
{
    if (!value.isDouble()) {
        return std::nullopt;
    }
    const double is = value.toDouble();
    if (!std::isfinite(is) || std::floor(is) != is || is < least || is > most) {
        return std::nullopt;
    }
    return static_cast<qint64>(is);
}
// ...and a number of any kind (a guest's processor share is a fraction).
std::optional<double> numberOf(const QJsonValue& value, double least, double most)
{
    if (!value.isDouble()) {
        return std::nullopt;
    }
    const double is = value.toDouble();
    return std::isfinite(is) && is >= least && is <= most ? std::optional<double>(is) : std::nullopt;
}

QString powerWordOf(const QJsonValue& value)
{
    const QString word = value.isString() ? value.toString() : QString();
    return word == "running"_L1 || word == "stopped"_L1 || word == "paused"_L1 ? word : QString();
}

// The text with every spelling of the token's pieces cut out (case and all), and whether it says the token in another
// spelling all the same (the web's `_unsaid`).
QString unsaid(QString text, QStringView tokenId, QStringView secret)
{
    if (text.isEmpty()) {
        return text;
    }
    const QString token = tokenId.toString() + u'=' + secret.toString();
    const QString sent = u"PVEAPIToken="_s + token;
    QList<QString> pieces{sent, token, tokenId.toString(), secret.toString()};
    std::ranges::sort(pieces, [](const QString& one, const QString& other) { return one.size() > other.size(); });
    for (const QString& piece : pieces) {
        if (piece.size() >= 4) {
            text.replace(piece, u"(the token)"_s, Qt::CaseInsensitive);
        }
    }
    // Said back in another spelling (split by a line break, without its dashes, %-encoded): none of its words then.
    const QString decoded = QUrl::fromPercentEncoding(QUrl::fromPercentEncoding(text.toUtf8()).toUtf8()).toLower();
    const auto hexOnly = [](QStringView of) {
        QString kept;
        for (const QChar sign : of) {
            if ((sign >= u'0' && sign <= u'9') || (sign >= u'a' && sign <= u'f')) {
                kept += sign;
            }
        }
        return kept;
    };
    const QString digits = hexOnly(decoded);
    const QString secretDigits = hexOnly(secret.toString().toLower());
    if (tokenId.size() >= 4 && decoded.contains(tokenId.toString().toLower())) {
        return QString(saidTheToken);
    }
    for (qsizetype at = 0; at + 8 <= secretDigits.size(); ++at) {
        if (digits.contains(QStringView(secretDigits).mid(at, 8))) {
            return QString(saidTheToken);
        }
    }
    return text;
}

} // namespace

bool isNode(QStringView text)
{
    static const QRegularExpression shape(u"\\A[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?\\z"_s);
    return shape.matchView(text).hasMatch();
}

bool isGuestNumber(qint64 vmid)
{
    return vmid >= lowestGuest && vmid <= highestGuest;
}

namespace {

// The port an address without one is at: Proxmox's own, as what is typed is read; or HTTPS's own, as a wall keeps an
// address (review cx3-3, M1).
enum class NoPort { Proxmoxs, Https };

base::Result<Address> addressRead(const QString& typed, NoPort noPort)
{
    const QString text = typed.trimmed();
    if (text.isEmpty()) {
        return base::Error{"invalid", "No address"};
    }
    if (text.size() > longestTyped) {
        return notAProxmox(text);
    }
    // Its parts by hand, as the web's reader takes them apart: how to speak, then where, then what is named there.
    QStringView rest(text);
    if (const qsizetype said = rest.indexOf("://"_L1); said >= 0) {
        if (rest.left(said).compare("https"_L1, Qt::CaseInsensitive) != 0) {
            return notAProxmox(text, u"its API is spoken over HTTPS only");
        }
        rest = rest.mid(said + 3);
    }
    const auto ends = [](QChar sign) { return sign == u'/' || sign == u'?' || sign == u'#'; };
    const qsizetype cut = std::ranges::find_if(rest, ends) - rest.begin();
    const QStringView authority = rest.left(cut);
    rest = rest.mid(cut);
    QStringView fragment;
    if (const qsizetype cross = rest.indexOf(u'#'); cross >= 0) {
        fragment = rest.mid(cross + 1);
        rest = rest.left(cross);
    }
    QStringView query;
    if (const qsizetype mark = rest.indexOf(u'?'); mark >= 0) {
        query = rest.mid(mark + 1);
        rest = rest.left(mark);
    }
    const QStringView path = rest;
    if (authority.isEmpty() || authority.contains(u'@')) {
        return notAProxmox(text);
    }
    // Without a port, Proxmox's own: 8006 (the address of a device keeps 443 unwritten, and so does this). As a wall
    // keeps it, HTTPS's own: 443, which kvm::addressFrom keeps unwritten.
    const qsizetype bracket = authority.lastIndexOf(u']');
    const bool portTyped = authority.indexOf(u':', bracket < 0 ? 0 : bracket) >= 0 && !authority.startsWith(u':');
    const QString withPort = portTyped || noPort == NoPort::Https
                                 ? authority.toString()
                                 : authority.toString() + u':' + QString::number(usualPort);
    const base::Result<QUrl> origin = kvm::addressFrom(u"https://"_s + withPort);
    if (!origin) {
        return notAProxmox(text);
    }
    Address found{origin.value(), {}, std::nullopt};
    QList<QStringView> steps;
    for (const QStringView step : path.split(u'/')) {
        if (!step.isEmpty()) {
            steps.append(step);
        }
    }
    const auto take = [&found, &text](QStringView node, QStringView vmid) -> std::optional<base::Error> {
        const std::optional<qint64> number = numberIn(vmid);
        if (!number || (!node.isEmpty() && !isNode(node))) {
            return notAGuest(text);
        }
        found.node = node.toString();
        found.vmid = number;
        return std::nullopt;
    };
    if (steps.size() == 4 && steps.first() == "nodes"_L1) {
        if (steps.at(2) == "lxc"_L1) {
            return aContainer();
        }
        if (steps.at(2) != "qemu"_L1) {
            return notAGuest(text);
        }
        if (steps.at(1).isEmpty() || !isNode(steps.at(1))) {
            return notAGuest(text);
        }
        if (const std::optional<base::Error> why = take(steps.at(1), steps.at(3))) {
            return *why;
        }
    } else if (steps.size() == 2) {
        if (!isNode(steps.at(0))) {
            return notAGuest(text);
        }
        if (const std::optional<base::Error> why = take(steps.at(0), steps.at(1))) {
            return *why;
        }
    } else if (steps.size() == 1) {
        if (const std::optional<base::Error> why = take({}, steps.at(0))) {
            return *why;
        }
    } else if (!steps.isEmpty()) {
        return notAGuest(text);
    }
    if (!found.vmid && !query.isEmpty()) {
        // The address of a console Proxmox's own page opened.
        const QUrlQuery asked(query.toString());
        if (asked.hasQueryItem(u"vmid"_s)) {
            if (asked.hasQueryItem(u"console"_s) && asked.queryItemValue(u"console"_s) != "kvm"_L1) {
                return base::Error{"invalid", "A container's or a node's console is not shown on the wall yet: only "
                                              "virtual machines"};
            }
            const QString node = asked.queryItemValue(u"node"_s, QUrl::FullyDecoded);
            if (node.isEmpty() ? !numberIn(asked.queryItemValue(u"vmid"_s)) : !isNode(node)) {
                return notAGuest(text);
            }
            if (const std::optional<base::Error> why = take(node, asked.queryItemValue(u"vmid"_s))) {
                return *why;
            }
        }
    }
    if (!found.vmid && !fragment.isEmpty()) {
        // The address of Proxmox's own page with a guest picked in its tree: "#v1:0:=qemu%2F100:4:::::::".
        static const QRegularExpression picked(u"=(qemu|lxc)/(\\d+)"_s);
        const QString said = QUrl::fromPercentEncoding(fragment.toString().toUtf8());
        if (const QRegularExpressionMatch match = picked.match(said); match.hasMatch()) {
            if (match.captured(1) == "lxc"_L1) {
                return aContainer();
            }
            if (const std::optional<base::Error> why = take({}, match.capturedView(2))) {
                return *why;
            }
        }
    }
    return found;
}

} // namespace

base::Result<Address> addressOf(const QString& typed)
{
    return addressRead(typed, NoPort::Proxmoxs);
}

base::Result<Address> keptAddressOf(const QString& kept)
{
    return addressRead(kept, NoPort::Https);
}

QString guestUrl(const QUrl& origin, const QString& node, qint64 vmid)
{
    return origin.toString(QUrl::FullyEncoded) + u"/nodes/"_s + node + u"/qemu/"_s + QString::number(vmid);
}

QUrl consolePage(const QUrl& origin, const QString& node, qint64 vmid)
{
    QUrl page = origin;
    page.setPath(u"/"_s);
    QUrlQuery asked;
    asked.addQueryItem(u"console"_s, u"kvm"_s);
    asked.addQueryItem(u"novnc"_s, u"1"_s);
    asked.addQueryItem(u"vmid"_s, QString::number(vmid));
    asked.addQueryItem(u"node"_s, node);
    asked.addQueryItem(u"resize"_s, u"scale"_s);
    page.setQuery(asked);
    return page;
}

bool isTokenId(QStringView text)
{
    static const QRegularExpression shape(
        u"\\A[A-Za-z0-9._+-]{1,64}@[A-Za-z][A-Za-z0-9._-]{0,31}![A-Za-z][A-Za-z0-9._-]{0,63}\\z"_s);
    return text.size() <= longestTokenId && shape.matchView(text).hasMatch();
}

bool isTokenSecret(QStringView text)
{
    static const QRegularExpression shape(
        u"\\A[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\\z"_s);
    return shape.matchView(text).hasMatch();
}

base::Error notAToken()
{
    return {"invalid", "The user name is to be an API token's ID (user@realm!token) and the password its secret"};
}

bool answerLost(int status)
{
    return status == 502 || status == 503 || status == 504 || status == 596 || status == 597;
}

// What Proxmox said with an answer, as one line of 1000 (its token not cut out yet): its body's message, as its other
// format writes one, else the words of its status line, where its API server says them (review c9b-2, H1). Of the
// status line ASCII alone, as the web's reader decodes it (httpx: ascii, errors ignored).
namespace {
QString wordsOf(const Answer& answer)
{
    if (QString message = kvmd::plain(QJsonDocument::fromJson(answer.body).object().value("message"_L1), 1000);
        !message.isEmpty()) {
        return message;
    }
    QByteArray ascii = answer.reason;
    ascii.removeIf([](char sign) { return static_cast<unsigned char>(sign) >= 0x80; });
    return kvmd::plain(QJsonValue(QString::fromLatin1(ascii)), 1000);
}
} // namespace

Fault faultOf(const Answer& answer)
{
    const int status = answer.status;
    if (status == 500) {
        // The guest's settings file not on that node (Proxmox VE 9.2's words, as the web's stand-in was typed from a
        // real one: "Configuration file 'nodes/<node>/qemu-server/<vmid>.conf' does not exist"), or a node there is
        // none of; no other "does not exist" (a storage's, a volume's, a boot device's: review c9b-3, L4).
        static const QRegularExpression wrongNode(
            u"Configuration file '(?:[^'/]*/)*qemu-server/[0-9]+\\.conf' does not exist|hostname lookup|"
            u"no such (cluster )?node"_s,
            QRegularExpression::CaseInsensitiveOption);
        if (wrongNode.match(wordsOf(answer)).hasMatch()) {
            return Fault::WrongNode;
        }
    }
    if (answerLost(status)) {
        return Fault::AnswerLost;
    }
    // (595 is the node it cannot connect to: nothing reached it, and no moment of Proxmox's own.)
    return status >= 500 && status != 595 ? Fault::Unwell : Fault::None;
}

QString saidBy(const Answer& answer, QStringView tokenId, QStringView secret)
{
    // (Made one line before the token is cut out, and cut to its length after: cut first, a token at the end would be
    // left in part.)
    if (QString said = kvmd::plain(QJsonValue(unsaid(wordsOf(answer), tokenId, secret)), 160); !said.isEmpty()) {
        return said;
    }
    return u"HTTP %1"_s.arg(answer.status);
}

base::Result<QJsonValue> dataOf(const Answer& answer, QStringView what, QStringView tokenId, QStringView secret)
{
    const QString asked = what.toString();
    const int status = answer.status;
    const QByteArray& body = answer.body;
    if (status == 200) {
        QJsonParseError error{};
        const QJsonDocument read = QJsonDocument::fromJson(body, &error);
        if (error.error != QJsonParseError::NoError || !read.isObject() || !read.object().contains("data"_L1)) {
            return base::Error{
                "bad_response",
                (u"Proxmox answered "_s + asked + u" with what is not its API's answer"_s).toStdString()};
        }
        return read.object().value("data"_L1);
    }
    const QString said = saidBy(answer, tokenId, secret);
    if (status == 403) {
        return base::Error{"bad_response",
                           (u"Proxmox does not let the token "_s + asked + u": "_s + said).toStdString()};
    }
    if (faultOf(answer) == Fault::WrongNode) {
        return base::Error{"bad_response", (u"Proxmox has no such guest there: "_s + said).toStdString()};
    }
    if (status == 595 || status == 596) {
        return base::Error{"unreachable", (u"Proxmox cannot reach the node the guest is on: "_s + said).toStdString()};
    }
    return base::Error{"bad_response",
                       (u"Proxmox answered %1 with HTTP %2: %3"_s.arg(asked).arg(status).arg(said)).toStdString()};
}

bool saysItIsAProxmox(const Answer& answer, bool challenged)
{
    if (answer.status == 401) {
        return !challenged;
    }
    if (answer.status != 200) {
        return false;
    }
    const base::Result<QJsonValue> said = dataOf(answer, u"say its version", {}, {});
    return said && said.value().toObject().value("version"_L1).isString();
}

base::Error notAProxmoxThere(int status, bool challenged)
{
    const QString how =
        challenged && status == 401 ? u"HTTP 401, asking for a sign-in of its own"_s : u"HTTP %1"_s.arg(status);
    return {"bad_response",
            u"What answers at that address does not say it is Proxmox VE (%1): the token was not sent to it. Check "
            u"the address and its port (8006 unless another was set)."_s.arg(how)
                .toStdString()};
}

Listing guestsIn(const QJsonValue& resources)
{
    Listing listing;
    QHash<qint64, Listed> found;
    const QJsonArray entries = resources.isArray() ? resources.toArray() : QJsonArray();
    for (const auto& each : entries) {
        if (!each.isObject()) {
            continue;
        }
        const QJsonObject entry = each.toObject();
        // (A template is no guest to look at.)
        const QJsonValue madeFrom = entry.value("template"_L1);
        if (madeFrom == QJsonValue(1) || madeFrom == QJsonValue(true)) {
            continue;
        }
        const std::optional<qint64> vmid =
            wholeIn(entry.value("vmid"_L1), static_cast<double>(lowestGuest), static_cast<double>(highestGuest));
        const QJsonValue node = entry.value("node"_L1);
        if (!vmid || !node.isString() || !isNode(node.toString())) {
            continue;
        }
        const QJsonValue type = entry.value("type"_L1);
        if (type == QJsonValue(u"lxc"_s)) {
            ++listing.containers;
            continue;
        }
        if (type != QJsonValue(u"qemu"_s) || found.contains(*vmid) || found.size() >= mostGuests) {
            continue;
        }
        QString name = kvmd::plain(entry.value("name"_L1), 80);
        if (name.isEmpty()) {
            name = u"VM %1"_s.arg(*vmid);
        }
        found.insert(*vmid, Listed{*vmid, node.toString(), name, powerWordOf(entry.value("status"_L1))});
    }
    QList<qint64> numbers = found.keys();
    std::ranges::sort(numbers);
    for (const qint64 vmid : numbers) {
        listing.guests.append(found.value(vmid));
    }
    return listing;
}

Current currentOf(const QJsonValue& data)
{
    Current current;
    const QJsonObject said = data.isObject() ? data.toObject() : QJsonObject();
    const QJsonValue status = said.value("status"_L1);
    // Running or stopped in so many words, and nothing else: a word of a kind of its own is no state.
    if (status == QJsonValue(u"running"_s)) {
        current.on = true;
    } else if (status == QJsonValue(u"stopped"_s)) {
        current.on = false;
    }
    current.qmpStatus = kvmd::plain(said.value("qmpstatus"_L1), 20);
    current.name = kvmd::plain(said.value("name"_L1), 60);
    current.uptime = wholeIn(said.value("uptime"_L1), 0, mostUptime);
    current.cpus = static_cast<int>(wholeIn(said.value("cpus"_L1), 1, mostCpus).value_or(0));
    if (const std::optional<double> memory = numberOf(said.value("maxmem"_L1), 0, mostMemory); memory && *memory > 0) {
        current.memoryGib = std::round(*memory / bytesInAGib * 100.0) / 100.0;
    }
    return current;
}

Status statusOf(const Current& current, qint64 vmid, const QString& node, const QString& version)
{
    Status status;
    status.state = current.on == std::optional<bool>(false) ? u"no_signal"_s : u"online"_s;
    status.hasPicture = false;
    status.signal = current.on;
    status.powerIsKnown = true;
    status.power = current.on;
    status.model = u"VM %1 on %2"_s.arg(vmid).arg(node);
    status.version = version.isEmpty() ? QString() : u"Proxmox VE "_s + version;
    ServerReadings readings;
    if (current.cpus > 0) {
        readings.processor = u"vCPU"_s;
        readings.processors = current.cpus;
    }
    readings.memoryGib = current.memoryGib;
    status.server = readings;
    return status;
}

QString ownNumber(const QJsonValue& config)
{
    const QJsonValue said = config.isObject() ? config.toObject().value("smbios1"_L1) : QJsonValue();
    if (!said.isString() || said.toString().size() >= 1024) {
        return {};
    }
    static const QRegularExpression uuid(
        u"(?:^|,)uuid=([0-9A-Fa-f]{8}-(?:[0-9A-Fa-f]{4}-){3}[0-9A-Fa-f]{12})(?:,|$)"_s);
    const QRegularExpressionMatch found = uuid.match(said.toString());
    if (!found.hasMatch()) {
        return {};
    }
    const QString number = found.captured(1).toLower();
    QSet<QChar> digits;
    for (const QChar sign : number) {
        if (sign != u'-') {
            digits.insert(sign);
        }
    }
    if (digits.size() < 4) {
        return {};
    }
    return u"proxmox:"_s +
           QString::fromLatin1(QCryptographicHash::hash(number.toUtf8(), QCryptographicHash::Sha256).toHex().first(32));
}

// --- its virtual media ----------------------------------------------------------------------------------------------

bool isStorageId(QStringView text)
{
    static const QRegularExpression shape(u"\\A[A-Za-z][A-Za-z0-9._-]{0,63}\\z"_s);
    return shape.matchView(text).hasMatch();
}

std::optional<IsoVolume> isoVolumeOf(QStringView volume)
{
    // (Its ending in either case spelt out, and no case-blind match: one takes the Kelvin sign for a k and the long s
    // for an s, which are no characters of an uploaded name. Review c9b-2, L5.)
    static const QRegularExpression shape(
        u"\\A([A-Za-z][A-Za-z0-9._-]{0,63}):iso/([A-Za-z0-9_+-][A-Za-z0-9_.+-]{0,199}\\.(?:[iI][sS][oO]|[iI][mM][gG]))\\z"_s);
    const QRegularExpressionMatch found = shape.matchView(volume);
    if (!found.hasMatch()) {
        return std::nullopt;
    }
    return IsoVolume{found.captured(1), found.captured(2)};
}

std::optional<Drive> driveOf(const QJsonValue& setting)
{
    if (!setting.isString() || setting.toString().isEmpty() || setting.toString().size() > longestDriveSetting) {
        return std::nullopt;
    }
    Drive drive;
    bool discSaid = false;
    const QStringList parts = setting.toString().split(u',');
    for (qsizetype at = 0; at < parts.size(); ++at) {
        const QString& part = parts.at(at);
        const qsizetype equals = part.indexOf(u'=');
        if (equals < 0) {
            // (The disc comes first, or is "file=": a word alone after it is no option.)
            if (at > 0) {
                return std::nullopt;
            }
            drive.disc = part;
            discSaid = true;
        } else if (part.first(equals) == "file"_L1 && !discSaid) {
            drive.disc = part.mid(equals + 1);
            discSaid = true;
        } else {
            drive.options.insert(part.first(equals), part.mid(equals + 1));
        }
    }
    if (drive.disc.isEmpty()) {
        return std::nullopt;
    }
    return drive;
}

const QStringList& cdSlots()
{
    // (Not named "slots": Qt's word.)
    static const QStringList places = [] {
        QStringList made{u"ide2"_s, u"ide0"_s, u"ide1"_s, u"ide3"_s};
        for (int sata = 0; sata < 6; ++sata) {
            made.append(u"sata%1"_s.arg(sata));
        }
        for (int scsi = 0; scsi < 31; ++scsi) {
            made.append(u"scsi%1"_s.arg(scsi));
        }
        return made;
    }();
    return places;
}

const QStringList& newCdSlots()
{
    static const QStringList places = cdSlots().first(10);
    return places;
}

std::optional<Cd> cdOf(const QJsonValue& config)
{
    const QJsonObject settings = config.isObject() ? config.toObject() : QJsonObject();
    std::optional<QString> first;
    for (const QString& slot : cdSlots()) {
        const std::optional<Drive> found = driveOf(settings.value(slot));
        if (!found || found->options.value(u"media"_s) != "cdrom"_L1) {
            continue;
        }
        if (found->disc != "none"_L1) {
            return Cd{slot, found->disc};
        }
        if (!first) {
            first = slot;
        }
    }
    if (!first) {
        return std::nullopt;
    }
    return Cd{*first, u"none"_s};
}

std::optional<QStringList> bootOrderOf(const QJsonValue& config)
{
    const QJsonValue said = config.isObject() ? config.toObject().value("boot"_L1) : QJsonValue();
    if (!said.isString() || !said.toString().startsWith("order="_L1) || said.toString().size() > longestBootOrder) {
        return std::nullopt;
    }
    static const QRegularExpression device(u"\\A[a-z]{2,8}[0-9]{1,3}\\z"_s);
    QStringList order = said.toString().mid(6).split(u';');
    const QSet<QString> once(order.cbegin(), order.cend());
    if (once.size() != order.size() ||
        !std::ranges::all_of(order, [](const QString& one) { return device.match(one).hasMatch(); })) {
        return std::nullopt;
    }
    return order;
}

std::optional<MountedMedia> mountedIn(const QJsonValue& config)
{
    const std::optional<Cd> cd = cdOf(config);
    if (!cd || cd->disc == "none"_L1) {
        return std::nullopt;
    }
    MountedMedia mounted;
    mounted.drive = u"cd"_s;
    mounted.image = kvmd::plain(QJsonValue(cd->disc), 300);
    if (const std::optional<IsoVolume> iso = isoVolumeOf(cd->disc)) {
        mounted.name = iso->name;
        mounted.where = iso->storage;
    } else if (cd->disc == "cdrom"_L1) {
        mounted.name = u"the host's own CD drive"_s;
    } else {
        mounted.name = kvmd::plain(QJsonValue(cd->disc.section(u'/', -1)), 120);
    }
    if (mounted.name.isEmpty() || mounted.image.isEmpty()) {
        return std::nullopt; // (a drive whose image is not named is not taken for one: MountedMedia)
    }
    mounted.readOnly = true;
    const std::optional<QStringList> order = bootOrderOf(config);
    mounted.first = order && !order->isEmpty() && order->first() == cd->slot;
    return mounted;
}

QString digestOf(const QJsonValue& config)
{
    static const QRegularExpression shape(u"\\A[0-9a-f]{40}\\z"_s);
    const QJsonValue said = config.isObject() ? config.toObject().value("digest"_L1) : QJsonValue();
    return said.isString() && shape.match(said.toString()).hasMatch() ? said.toString() : QString();
}

Storages storagesIn(const QJsonValue& listed)
{
    Storages storages;
    const QJsonArray entries = listed.isArray() ? listed.toArray() : QJsonArray();
    for (const auto& each : entries) {
        const QJsonObject entry = each.toObject();
        const QJsonValue name = entry.value("storage"_L1);
        const QJsonValue content = entry.value("content"_L1);
        if (!name.isString() || !isStorageId(name.toString()) || storages.names.contains(name.toString()) ||
            !content.isString() || !content.toString().split(u',').contains("iso"_L1)) {
            continue;
        }
        const QJsonValue active = entry.value("active"_L1);
        if (active == QJsonValue(0) || active == QJsonValue(false)) {
            if (storages.unseen.size() < mostStorages) {
                storages.unseen.append(u"%1: Proxmox says it is not active"_s.arg(name.toString()));
            }
            continue;
        }
        if (storages.names.size() < mostStorages) {
            storages.names.append(name.toString());
        }
    }
    return storages;
}

Images imagesIn(const QJsonValue& content, const QString& storage, qsizetype room)
{
    Images found;
    const QJsonArray entries = content.isArray() ? content.toArray() : QJsonArray();
    for (const auto& each : entries) {
        const QJsonObject entry = each.toObject();
        const QJsonValue volume = entry.value("volid"_L1);
        const std::optional<IsoVolume> iso =
            volume.isString() ? isoVolumeOf(volume.toString()) : std::optional<IsoVolume>();
        // (Taken by kind: an image of this storage, named as the program hands names on, and nothing else.)
        const QJsonValue kind = entry.value("content"_L1);
        if (!iso || iso->storage != storage || !(kind.isUndefined() || kind == QJsonValue(u"iso"_s)) ||
            found.images.size() >= room) {
            ++found.leftOut;
            continue;
        }
        MediaImage image;
        image.id = volume.toString();
        image.name = iso->name;
        image.where = storage;
        image.size = wholeIn(entry.value("size"_L1), 0, mostImageBytes);
        found.images.append(image);
    }
    return found;
}

} // namespace qkvm::core::proxmox
