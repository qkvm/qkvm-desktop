#include <qkvm/core/kvmd_answers.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

using namespace Qt::StringLiterals;

namespace qkvm::core::kvmd {

namespace {

// A value some way down an answer; nothing (a null) where the way does not lead through objects.
QJsonValue at(const QJsonValue& from, std::initializer_list<QLatin1StringView> way)
{
    QJsonValue here = from;
    for (const QLatin1StringView step : way) {
        if (!here.isObject()) {
            return {};
        }
        here = here.toObject().value(step);
    }
    return here;
}

std::optional<bool> flag(const QJsonValue& value)
{
    return value.isBool() ? std::optional<bool>(value.toBool()) : std::nullopt;
}

// A KVM's number if it is one and within reason.
std::optional<double> number(const QJsonValue& value, double least, double most)
{
    if (!value.isDouble()) {
        return std::nullopt;
    }
    const double is = value.toDouble();
    return is >= least && is <= most ? std::optional<double>(is) : std::nullopt;
}

// A whole number of a KVM's within reason; nothing for one with a fraction: a count is none.
std::optional<int> whole(const QJsonValue& value, int least, int most)
{
    const std::optional<double> is = number(value, least, most);
    return is && std::floor(*is) == *is ? std::optional<int>(static_cast<int>(*is)) : std::nullopt;
}

// What is true of a value as a script would ask it: an answer may say "token": null, or "".
bool saysSomething(const QJsonValue& value)
{
    switch (value.type()) {
    case QJsonValue::Bool:
        return value.toBool();
    case QJsonValue::Double:
        return value.toDouble() != 0;
    case QJsonValue::String:
        return !value.toString().isEmpty();
    case QJsonValue::Array:
        return !value.toArray().isEmpty();
    case QJsonValue::Object:
        return !value.toObject().isEmpty();
    default:
        return false;
    }
}

bool prints(char32_t sign)
{
    switch (QChar::category(sign)) {
    case QChar::Other_Control:
    case QChar::Other_Format:
    case QChar::Other_PrivateUse:
    case QChar::Other_NotAssigned:
    case QChar::Separator_Line:
    case QChar::Separator_Paragraph:
    case QChar::Separator_Space: // (the blank itself is put in by hand)
        return false;
    default:
        return true;
    }
}

base::Error notUnderstood(const char* what)
{
    return {"bad_response", what};
}

// The KVM's own sizes for a smaller picture: how wide and high at most, and how good. Kept good:
// the letters on a screen turn to mush below about 85.
struct Size {
    QLatin1StringView name;
    int width;
    int height;
    int quality;
};
constexpr std::array sizes{Size{"480"_L1, 480, 270, 86}, Size{"640"_L1, 640, 360, 88}, Size{"960"_L1, 960, 540, 90}};
constexpr QLatin1StringView full = "full"_L1;

// What needs nothing but its name: where it goes, and with what.
struct Plain {
    QLatin1StringView kind;
    QLatin1StringView path;
    QLatin1StringView name;
    QLatin1StringView value;
};
constexpr std::array plainOnes{
    Plain{"reboot"_L1, "/api/upgrade/reboot"_L1, {}, {}},
    Plain{"ctrl_alt_del"_L1, "/api/hid/events/send_shortcut"_L1, "keys"_L1, "ControlLeft,AltLeft,Delete"_L1},
    Plain{"atx_power"_L1, "/api/atx/click"_L1, "button"_L1, "power"_L1},
    Plain{"atx_power_long"_L1, "/api/atx/click"_L1, "button"_L1, "power_long"_L1},
    Plain{"atx_reset"_L1, "/api/atx/click"_L1, "button"_L1, "reset"_L1},
    // (The KVM nudges the mouse to keep the computer awake.)
    Plain{"jiggler_on"_L1, "/api/hid/set_params"_L1, "jiggler"_L1, "1"_L1},
    Plain{"jiggler_off"_L1, "/api/hid/set_params"_L1, "jiggler"_L1, "0"_L1},
};

} // namespace

QString plain(const QJsonValue& value, qsizetype limit)
{
    QString said;
    if (value.isString()) {
        said = value.toString();
    } else if (value.isDouble()) {
        const double is = value.toDouble();
        said = std::floor(is) == is && std::abs(is) < 9e15 ? QString::number(static_cast<qint64>(is))
                                                           : QString::number(is, 'g', 12);
    } else {
        return {};
    }
    // Cut before it is gone through: what is wanted is a line, whatever length of text was sent.
    const QList<uint> signs = QStringView(said).left(limit * 8).toUcs4();
    QString line;
    for (const uint one : signs) {
        const char32_t sign = one;
        if (prints(sign)) {
            line += QString::fromUcs4(&sign, 1);
        } else {
            line += u' ';
        }
    }
    line = line.simplified();
    if (line.size() > limit) {
        // Not through the middle of a sign that takes two places.
        line.truncate(line.at(limit - 1).isHighSurrogate() ? limit - 1 : limit);
    }
    return line;
}

base::Result<QJsonObject> resultFrom(const QByteArray& answer)
{
    if (answer.size() > largestReplyBytes) {
        return notUnderstood("The device sent far more data than this program accepts");
    }
    const QJsonDocument document = QJsonDocument::fromJson(answer);
    if (!document.isObject() || !document.object().value("result"_L1).isObject()) {
        return notUnderstood("The device returned unexpected data");
    }
    return document.object().value("result"_L1).toObject();
}

bool isKvmd(const QByteArray& answer)
{
    if (answer.size() > largestReplyBytes) {
        return false;
    }
    const QJsonDocument document = QJsonDocument::fromJson(answer);
    return document.isObject() && document.object().contains("ok"_L1) &&
           document.object().value("result"_L1).isObject();
}

bool isToken(const QByteArray& value)
{
    static const QRegularExpression shape(u"\\A[A-Za-z0-9._~+/=-]{1,512}\\z"_s);
    return shape.match(QString::fromLatin1(value)).hasMatch();
}

QByteArray tokenFrom(const QByteArray& answer, const QList<QNetworkCookie>& cookies)
{
    QJsonValue said;
    if (answer.size() <= largestReplyBytes) {
        said = at(QJsonDocument::fromJson(answer).object(), {"result"_L1, "token"_L1});
    }
    if (saysSomething(said)) {
        // What the answer says is the pass, whatever a cookie beside it says; and what it says is
        // either a pass or none.
        const QByteArray token = said.isString() ? said.toString().toUtf8() : QByteArray();
        return isToken(token) ? token : QByteArray();
    }
    // A KVM that sets its session twice has not said which one it is.
    QByteArray token;
    int found = 0;
    for (const QNetworkCookie& cookie : cookies) {
        if (cookie.name() == "auth_token") {
            token = cookie.value();
            ++found;
        }
    }
    return found == 1 && isToken(token) ? token : QByteArray();
}

About aboutFrom(const QJsonObject& version)
{
    return {plain(version.value("model"_L1), 40), plain(version.value("version"_L1), 40)};
}

QString hostnameFrom(const QJsonObject& said)
{
    return plain(said.value("hostname"_L1));
}

std::optional<Ports> portsFrom(const QJsonObject& switchState)
{
    const QJsonArray ports = at(switchState, {"model"_L1, "ports"_L1}).toArray();
    const QJsonArray links = at(switchState, {"video"_L1, "links"_L1}).toArray();
    const int count = static_cast<int>(std::min<qsizetype>(std::max(ports.size(), links.size()), mostPorts));
    const std::optional<int> shown = whole(at(switchState, {"summary"_L1, "active_port"_L1}), 0, mostPorts);
    if (count < 2 || !shown || *shown >= count) {
        return std::nullopt;
    }
    Ports all;
    all.shown = *shown;
    for (int index = 0; index < count; ++index) {
        Port port;
        if (index < ports.size()) {
            port.name = plain(at(ports.at(index), {"name"_L1}), 40);
        }
        // Whether a computer is on it: only a plain "no" counts as none.
        port.empty = index < links.size() && flag(links.at(index)) == std::optional<bool>(false);
        all.list.append(port);
    }
    return all;
}

std::optional<bool> powerSaid(const QJsonObject& atx)
{
    // (GLKVM's word, as its own page shows it; on a Comet with its board, 2026-10-14: "power": "on" and a light that
    // was off, under a running PC.)
    if (atx.contains("power"_L1)) {
        const QJsonValue said = atx.value("power"_L1);
        const QString word = said.isString() ? said.toString() : QString();
        if (word == "on"_L1 || word == "sleep"_L1) {
            return true;
        }
        if (word == "off"_L1) {
            return false;
        }
        return std::nullopt;
    }
    return flag(at(atx, {"leds"_L1, "power"_L1}));
}

bool boardIsThere(const QJsonObject& atx)
{
    return flag(atx.value("enabled"_L1)) == std::optional<bool>(true);
}

std::optional<QJsonObject> boardHeard(const QByteArray& message)
{
    // (Only a message that can be one is read as JSON: the line carries the KVM's other states too.)
    if (message.size() > mostBoardWord || !message.contains("\"atx\"")) {
        return std::nullopt;
    }
    const QJsonDocument read = QJsonDocument::fromJson(message);
    if (!read.isObject() || read.object().value("event_type"_L1) != QJsonValue(u"atx"_s) ||
        !read.object().value("event"_L1).isObject()) {
        return std::nullopt;
    }
    const QJsonObject event = read.object().value("event"_L1).toObject();
    const auto kept = [](const QJsonValue& value) {
        const std::optional<bool> said = flag(value);
        return said ? QJsonValue(*said) : QJsonValue();
    };
    QJsonObject board{{u"enabled"_s, kept(event.value("enabled"_L1))}, {u"busy"_s, kept(event.value("busy"_L1))}};
    if (event.contains("power"_L1)) {
        // (Only where it is: a word of another kind says nothing, and the light beside it is no witness then.)
        const QJsonValue word = event.value("power"_L1);
        const bool known =
            word == QJsonValue(u"on"_s) || word == QJsonValue(u"off"_s) || word == QJsonValue(u"sleep"_s);
        board.insert(u"power"_s, known ? word : QJsonValue());
    }
    const QJsonValue lights = event.value("leds"_L1);
    board.insert(u"leds"_s, lights.isObject() ? QJsonObject{{u"power"_s, kept(lights.toObject().value("power"_L1))}}
                                              : QJsonObject{});
    return board;
}

std::optional<bool> fingerbotHeard(const QByteArray& message)
{
    if (message.size() > mostBoardWord || !message.contains("\"fingerbot\"")) {
        return std::nullopt;
    }
    const QJsonDocument read = QJsonDocument::fromJson(message);
    if (!read.isObject() || read.object().value("event_type"_L1) != QJsonValue(u"fingerbot"_s) ||
        !read.object().value("event"_L1).isObject()) {
        return std::nullopt;
    }
    return flag(read.object().value("event"_L1).toObject().value("exist"_L1));
}

std::optional<int> batterySaid(const QJsonObject& result)
{
    const QJsonValue said = result.value("battery"_L1);
    if (!said.isDouble() || !std::isfinite(said.toDouble()) || said.toDouble() < 0 || said.toDouble() > 255) {
        return std::nullopt;
    }
    return std::min(100, static_cast<int>(std::lround(said.toDouble())));
}

int sourceWidthFrom(const QJsonObject& streamer)
{
    return whole(at(streamer, {"streamer"_L1, "source"_L1, "resolution"_L1, "width"_L1}), 1, 100'000).value_or(0);
}

Status statusFrom(const Looked& looked)
{
    Status status;
    const QJsonValue source = at(looked.streamer, {"streamer"_L1, "source"_L1});
    // One maker says whether the computer sends a picture; the other only whether its capture is
    // there, and nothing while that rests.
    const QJsonValue said = at(looked.streamer, {"streamer"_L1, "hdmi"_L1, "signal"_L1});
    const std::optional<bool> signal =
        said.isNull() || said.isUndefined() ? flag(at(source, {"online"_L1})) : flag(said);
    status.state = signal == std::optional<bool>(false) ? u"no_signal"_s : u"online"_s;
    status.signal = signal;
    status.width = whole(at(source, {"resolution"_L1, "width"_L1}), 1, 100'000).value_or(0);
    status.height = whole(at(source, {"resolution"_L1, "height"_L1}), 1, 100'000).value_or(0);

    if (looked.hid) {
        status.keyboard = flag(at(*looked.hid, {"keyboard"_L1, "online"_L1}));
    }
    if (looked.atx) {
        // Only from an answer in which the board says it is there: a Comet's board tells whoever asks while it answers
        // somebody else that there is none, and "off", which says nothing of the computer. (Its driver keeps what the
        // board said before through such an answer: KvmdDevice::Pace::boardKeptMs.)
        status.powerIsKnown = flag(looked.atx->value("enabled"_L1)) == std::optional<bool>(true);
        if (status.powerIsKnown) {
            status.power = powerSaid(*looked.atx);
        }
    }
    if (looked.info) {
        // Where the two makers keep it.
        QJsonValue health = looked.info->value("health"_L1);
        if (!saysSomething(health)) {
            health = at(*looked.info, {"hw"_L1, "health"_L1});
        }
        status.temperature = number(at(health, {"temp"_L1, "cpu"_L1}), -100, 300);
    }
    status.model = looked.about.model;
    status.version = looked.about.version;
    if (looked.ports) {
        status.ports = looked.ports->list;
        status.shownPort = looked.ports->shown;
    }
    return status;
}

QString sizeFor(int width, int sourceWidth)
{
    for (const Size& size : sizes) {
        if (width <= size.width) {
            // Nothing to shrink: the picture as captured, and not one made anew at the same size.
            return sourceWidth > 0 && size.width >= sourceWidth ? QString(full) : QString(size.name);
        }
    }
    return full;
}

QUrlQuery snapshotQuery(const QString& size)
{
    QUrlQuery query;
    // Without a picture from the computer kvmd still gives its own in place of one, marked as
    // that, and not an error.
    query.addQueryItem(u"allow_offline"_s, u"1"_s);
    for (const Size& one : sizes) {
        if (one.name == size) {
            query.addQueryItem(u"preview"_s, u"1"_s);
            query.addQueryItem(u"preview_max_width"_s, QString::number(one.width));
            query.addQueryItem(u"preview_max_height"_s, QString::number(one.height));
            query.addQueryItem(u"preview_quality"_s, QString::number(one.quality));
        }
    }
    return query;
}

std::optional<QSize> pictureSize(const QByteArray& data)
{
    const auto byte = [&data](qsizetype index) { return static_cast<quint8>(data.at(index)); };
    const auto two = [&byte](qsizetype index) { return (byte(index) << 8) | byte(index + 1); };
    if (data.startsWith("\x89PNG\r\n\x1a\n") && data.size() >= 24 && data.mid(12, 4) == "IHDR") {
        const auto four = [&two](qsizetype index) { return (static_cast<qint64>(two(index)) << 16) | two(index + 2); };
        const qint64 width = four(16);
        const qint64 height = four(20);
        // (What does not fit a size at all is none that is shown: isShown says no to it.)
        constexpr qint64 most = 0x7FFFFFFF;
        return QSize(static_cast<int>(std::min(width, most)), static_cast<int>(std::min(height, most)));
    }
    if (!data.startsWith("\xff\xd8")) {
        return std::nullopt;
    }
    qsizetype here = 2;
    // A picture says its size within its first few parts. What goes on for megabytes without
    // saying it is not looked through to the end.
    for (int part = 0; part < 2000; ++part) {
        if (here + 9 > data.size() || byte(here) != 0xFF) {
            return std::nullopt;
        }
        const int marker = byte(here + 1);
        if (marker == 0xFF) { // padding between two parts
            here += 1;
            continue;
        }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD9)) { // parts that have no length
            here += 2;
            continue;
        }
        const int length = two(here + 2);
        // The parts that begin the picture proper say how large it is: any "start of frame", and
        // not the three of that range that are tables.
        if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            return QSize(two(here + 7), two(here + 5));
        }
        if (length < 2) {
            return std::nullopt;
        }
        here += 2 + length;
    }
    return std::nullopt;
}

bool isShown(QSize size)
{
    return size.width() > 0 && size.height() > 0 && size.width() <= longestPictureSide &&
           size.height() <= longestPictureSide &&
           static_cast<qint64>(size.width()) * size.height() <= mostPicturePixels;
}

bool namesThePowerKey(QStringView keys)
{
    const QList<QStringView> names = keys.split(u',');
    return std::ranges::any_of(names, [](QStringView name) { return name == u"Power"; });
}

base::Result<Call> callFor(const Action& action, int ports)
{
    Call call;
    if (action.kind == "port"_L1) {
        if (action.port < 0 || action.port >= ports) {
            return base::Error{"invalid", "This KVM has no such port"};
        }
        call.path = u"/api/switch/set_active"_s;
        call.query.addQueryItem(u"port"_s, QString::number(action.port));
        // The KVM answers once it has switched, which takes it several seconds.
        call.quietMs = 30'000;
        return call;
    }
    if (action.kind == "wol"_L1) {
        static const QRegularExpression mac(u"\\A[0-9A-Fa-f]{2}([:-][0-9A-Fa-f]{2}){5}\\z"_s);
        if (!mac.match(action.mac).hasMatch()) {
            return base::Error{"invalid", "Not a valid MAC address"};
        }
        call.path = u"/api/wol/wake"_s;
        call.query.addQueryItem(u"mac"_s, action.mac);
        return call;
    }
    if (action.kind == "keys"_L1) {
        // Up to eight keys by the names browsers and kvmd have for them: "ControlLeft,AltLeft,Delete".
        static const QRegularExpression keys(u"\\A[A-Z][A-Za-z0-9]{1,15}(,[A-Z][A-Za-z0-9]{1,15}){0,7}\\z"_s);
        if (!keys.match(action.keys).hasMatch()) {
            return base::Error{"invalid", "Not a valid key combination"};
        }
        // (kvmd is told the names as they come, and has this one.)
        if (namesThePowerKey(action.keys)) {
            return base::Error{"invalid", powerKeyRefused};
        }
        call.path = u"/api/hid/events/send_shortcut"_s;
        call.query.addQueryItem(u"keys"_s, action.keys);
        return call;
    }
    if (action.kind == "text"_L1) {
        const QString text = action.text + (action.enter ? u"\n"_s : QString());
        // Counted as the server counts it: in characters, of which some take two places here.
        if (text.isEmpty() || QStringView(text).toUcs4().size() > mostTypedCharacters) {
            return base::Error{"invalid", "Text must be 1 to 2000 characters"};
        }
        call.path = u"/api/hid/print"_s;
        call.query.addQueryItem(u"limit"_s, u"0"_s);
        call.body = text.toUtf8();
        // The KVM answers once it has finished typing, which takes a while for a long text.
        call.quietMs = 90'000;
        return call;
    }
    for (const Plain& one : plainOnes) {
        if (action.kind == one.kind) {
            call.path = one.path;
            if (!one.name.isEmpty()) {
                call.query.addQueryItem(one.name, one.value);
            }
            return call;
        }
    }
    return base::Error{"invalid", "This KVM cannot do that"};
}

base::Error refusalOf(int httpStatus, const QByteArray& answer)
{
    QString detail;
    if (const base::Result<QJsonObject> said = resultFrom(answer)) {
        detail = plain(said.value().value("error_msg"_L1), 200);
        if (detail.isEmpty()) {
            detail = plain(said.value().value("error"_L1), 200);
        }
    }
    if (detail.isEmpty()) {
        detail = u"The device refused (HTTP %1)"_s.arg(httpStatus);
    }
    return {"action_failed", detail.toStdString()};
}

QList<WakeTarget> wakeTargetsFrom(const QJsonObject& said)
{
    static const QRegularExpression mac(u"\\A[0-9A-Fa-f]{2}([:-][0-9A-Fa-f]{2}){5}\\z"_s);
    QList<WakeTarget> targets;
    const QJsonArray devices = said.value("devices"_L1).toArray();
    for (const auto& entry : devices) {
        if (targets.size() >= mostWakeTargets) {
            break;
        }
        const QString address = entry.toObject().value("mac"_L1).toString();
        if (!mac.match(address).hasMatch()) {
            continue;
        }
        QString name = plain(at(entry, {"name"_L1}));
        if (name.isEmpty()) {
            name = plain(at(entry, {"hostname"_L1}));
        }
        if (name.isEmpty()) {
            name = plain(at(entry, {"ip"_L1}));
        }
        targets.append(WakeTarget{name, address});
    }
    return targets;
}

} // namespace qkvm::core::kvmd
