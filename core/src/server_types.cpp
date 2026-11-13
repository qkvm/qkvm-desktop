#include <qkvm/core/server_types.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <utility>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>

#include <qkvm/core/kvmd_answers.h>

using namespace Qt::StringLiterals;

namespace qkvm::core {

namespace {

base::Error notUnderstood()
{
    return {"bad_answer", "The server answered in a way this program does not understand."};
}

// A text of the server's, never longer than a card or a sentence can be.
QString text(const QJsonValue& value)
{
    return value.isString() ? value.toString().left(longestText) : QString();
}

std::optional<bool> flag(const QJsonValue& value)
{
    return value.isBool() ? std::optional<bool>(value.toBool()) : std::nullopt;
}

std::optional<double> number(const QJsonValue& value)
{
    return value.isDouble() ? std::optional<double>(value.toDouble()) : std::nullopt;
}

// A whole number within what the program can use, or nothing.
int whole(const QJsonValue& value, int least, int most)
{
    if (!value.isDouble()) {
        return 0;
    }
    const double is = value.toDouble();
    return is >= least && is <= most ? static_cast<int>(is) : 0;
}

base::Result<QJsonObject> objectFrom(const QByteArray& answer, qsizetype largest = largestAnswerBytes)
{
    if (answer.size() > largest) {
        return base::Error{"too_large", "The server's answer is larger than this program takes."};
    }
    const QJsonDocument document = QJsonDocument::fromJson(answer);
    if (!document.isObject()) {
        return notUnderstood();
    }
    return document.object();
}

std::optional<Account> accountFrom(const QJsonValue& value)
{
    if (!value.isObject()) {
        return std::nullopt;
    }
    const QJsonObject said = value.toObject();
    Account account;
    account.id = text(said["id"_L1]);
    account.name = text(said["name"_L1]);
    account.role = text(said["role"_L1]);
    account.isDisplay = said["token"_L1].toBool();
    account.mustEnroll = said["must_enroll"_L1].toBool();
    // A role the program does not know is the least of them: it may look.
    if (account.role != "admin"_L1 && account.role != "operator"_L1) {
        account.role = u"viewer"_s;
    }
    return account.id.isEmpty() ? std::nullopt : std::optional<Account>(account);
}

ServerReadings readingsFrom(const QJsonObject& said)
{
    ServerReadings readings;
    readings.health = text(said["health"_L1]);
    readings.locator = flag(said["locator"_L1]);
    const QJsonArray fans = said["fans"_L1].toArray();
    for (const auto& value : fans) {
        const QJsonObject fan = value.toObject();
        ++readings.fans;
        readings.fansRunning += fan["reading"_L1].toDouble() > 0 ? 1 : 0;
        readings.fansNotWell += fan["ok"_L1].toBool(true) ? 0 : 1;
    }
    readings.processor = text(said["processor"_L1]);
    readings.processors = whole(said["processors"_L1], 0, 1024);
    readings.memoryGib = number(said["memory_gib"_L1]);
    readings.noScreen = text(said["no_screen"_L1]);
    // A PC with Intel AMT: its tile's view, only where it is one of the three.
    if (const QString view = text(said["amt"_L1].toObject()["view"_L1]); isAmtView(view)) {
        readings.amtView = view;
    }
    return readings;
}

std::optional<QStringList> capabilitiesFrom(const QJsonObject& said)
{
    if (!said.contains("can"_L1)) {
        return std::nullopt;
    }
    const QJsonValue value = said["can"_L1];
    QStringList result;
    if (!value.isArray() || value.toArray().size() > 64) {
        return result;
    }
    for (const auto& item : value.toArray()) {
        if (!item.isString() || item.toString().isEmpty() || item.toString().size() > 64) {
            return QStringList{};
        }
        if (!result.contains(item.toString())) {
            result.append(item.toString());
        }
    }
    return result;
}

// A text of the device's, of what it says of an image, as one plain line: what does not print is a blank.
QString line(const QJsonValue& value, qsizetype longest = 200)
{
    return value.isString() ? kvmd::plain(value, longest) : QString();
}

// A plain yes, and only that.
bool yes(const QJsonValue& value)
{
    return value.isBool() && value.toBool();
}

// A count of bytes, where it is one.
std::optional<qint64> bytes(const QJsonValue& value)
{
    const std::optional<double> is = number(value);
    return is && *is >= 0 && *is < 9e15 ? std::optional<qint64>(static_cast<qint64>(*is)) : std::nullopt;
}

// Of a list of the server's words, those it may say there, each once, in its order.
QStringList wordsOf(const QJsonValue& value, std::initializer_list<QLatin1StringView> known)
{
    QStringList kept;
    const QJsonArray said = value.toArray();
    for (qsizetype index = 0; index < said.size() && index < 16; ++index) {
        const QString word = said.at(index).toString();
        if (std::ranges::find(known, word) != known.end() && !kept.contains(word)) {
            kept.append(word);
        }
    }
    return kept;
}

// Sentences of the server's, as many as a box can say.
QStringList sentencesOf(const QJsonValue& value)
{
    QStringList kept;
    const QJsonArray said = value.toArray();
    for (qsizetype index = 0; index < said.size() && index < 20; ++index) {
        if (const QString sentence = line(said.at(index), 600); !sentence.isEmpty()) {
            kept.append(sentence);
        }
    }
    return kept;
}

// An image's id as the server listed it, to be sent back as it came: none where it is no text, or is longer than the
// server takes one (300, its UnmountIn and DeleteIn), which cut short could be the beginning of another image's.
QString imageIdOf(const QJsonValue& value)
{
    const QString id = value.isString() ? value.toString() : QString();
    return id.size() <= 300 ? id : QString();
}

// What is in a machine's virtual drive: an object that names an image, and nothing otherwise.
std::optional<MountedMedia> mountedFrom(const QJsonValue& value)
{
    if (!value.isObject()) {
        return std::nullopt;
    }
    const QJsonObject said = value.toObject();
    MountedMedia media;
    media.name = line(said["name"_L1]);
    if (media.name.isEmpty()) {
        return std::nullopt;
    }
    media.drive = line(said["drive"_L1], 16);
    // (Its id goes back to the server as it came: an unmount names it. One longer than the server takes is none
    // of its, and is never cut short: that could name another image.)
    media.image = imageIdOf(said["image"_L1]);
    media.where = line(said["where"_L1]);
    media.readOnly = said["read_only"_L1].isBool() && said["read_only"_L1].toBool();
    media.first = flag(said["first"_L1]);
    media.by = line(said["by"_L1], 80);
    if (const std::optional<double> at = number(said["at"_L1]); at && *at >= 0 && *at < 1e11) {
        media.at = static_cast<qint64>(*at);
    }
    media.sha256 = line(said["sha256"_L1], 64);
    return media;
}

std::optional<MediaUploading> uploadingOf(const QJsonValue& value)
{
    if (!value.isObject()) {
        return std::nullopt;
    }
    const QJsonObject said = value.toObject();
    MediaUploading up;
    up.name = line(said["name"_L1]);
    up.size = bytes(said["size"_L1]).value_or(0);
    up.sent = std::clamp<qint64>(bytes(said["sent"_L1]).value_or(0), 0, up.size);
    up.by = line(said["by"_L1], 80);
    return up.name.isEmpty() ? std::nullopt : std::optional<MediaUploading>(up);
}

Status statusOf(const QJsonObject& said)
{
    Status status;
    status.capabilities = capabilitiesFrom(said);
    if (const QString state = text(said["state"_L1]); !state.isEmpty()) {
        status.state = state;
    }
    if (const QJsonObject error = said["error"_L1].toObject(); !error.isEmpty()) {
        status.errorCode = text(error["code"_L1]);
        status.errorMessage = text(error["message"_L1]);
    }
    if (const std::optional<double> ago = number(said["last_seen_ago"_L1]); ago && *ago >= 0 && *ago < 4e9) {
        status.lastSeenSecondsAgo = static_cast<int>(*ago);
    }
    // Only a server's status says "picture", and says false.
    status.hasPicture = said["picture"_L1].toBool(true);
    status.hasScreen = said["screen"_L1].toBool(false);

    const QJsonObject video = said["video"_L1].toObject();
    status.signal = flag(video["signal"_L1]);
    status.width = whole(video["width"_L1], 1, 100000);
    status.height = whole(video["height"_L1], 1, 100000);

    const QJsonObject atx = said["atx"_L1].toObject();
    status.powerIsKnown = atx["enabled"_L1].toBool();
    status.power = flag(atx["power"_L1]);
    status.keyboard = flag(said["usb"_L1].toObject()["keyboard"_L1]);
    status.temperature = number(said["health"_L1].toObject()["temp"_L1]);
    status.model = text(said["about"_L1].toObject()["model"_L1]);
    status.version = text(said["about"_L1].toObject()["version"_L1]);
    status.opensSignedIn = said["console"_L1].toBool(false);

    const QJsonObject ports = said["ports"_L1].toObject();
    const QJsonArray list = ports["list"_L1].toArray();
    if (list.size() >= 2 && list.size() <= 64) {
        for (const auto& value : list) {
            const QJsonObject port = value.toObject();
            status.ports.append(Port{text(port["name"_L1]), flag(port["signal"_L1]) == std::optional<bool>(false)});
        }
        const int shown = ports["active"_L1].isDouble() ? ports["active"_L1].toInt(-1) : -1;
        status.shownPort = shown >= 0 && shown < status.ports.size() ? shown : -1;
    }

    if (const QJsonValue server = said["server"_L1]; server.isObject()) {
        status.server = readingsFrom(server.toObject());
    }
    // A Fingerbot paired with the KVM: its battery as a percentage, and low only where that is known (the server's
    // `low` is null without one).
    if (const QJsonValue bot = said["fingerbot"_L1]; bot.isObject()) {
        Fingerbot fingerbot;
        if (const QJsonValue battery = bot["battery"_L1];
            battery.isDouble() && battery.toDouble() >= 0 && battery.toDouble() <= 100) {
            fingerbot.battery = static_cast<int>(std::lround(battery.toDouble()));
        }
        fingerbot.low = fingerbot.battery.has_value() && bot["low"_L1].isBool() && bot["low"_L1].toBool();
        status.fingerbot = fingerbot;
    }
    status.media = mountedFrom(said["media"_L1]);
    return status;
}

} // namespace

base::Result<Health> healthFrom(const QByteArray& answer)
{
    // Longer than a QKVM's word of itself, or no object at all: what answered is no QKVM.
    const QJsonObject said =
        answer.size() > largestHealthBytes ? QJsonObject() : QJsonDocument::fromJson(answer).object();
    if (said["status"_L1].toString() != "ok"_L1 || !said["api"_L1].isArray()) {
        return notUnderstood();
    }
    Health health;
    // (A text, and shown as one line: a number where the version belongs is not read as one.)
    if (said["version"_L1].isString()) {
        health.version = kvmd::plain(said["version"_L1], longestVersion);
    }
    // What it lists is compared and may be shown in a sentence: only what is written as a version.
    static const QRegularExpression isVersion(u"\\Av[0-9]{1,4}\\z"_s);
    const QJsonArray apis = said["api"_L1].toArray();
    for (qsizetype index = 0; index < apis.size() && index < mostApis; ++index) {
        const QString api = apis.at(index).toString();
        if (isVersion.match(api).hasMatch() && !health.apis.contains(api)) {
            health.apis.append(api);
        }
    }
    // (Only a plain yes is one.)
    health.locked = said["locked"_L1].toBool();
    return health;
}

base::Result<Session> sessionFrom(const QByteArray& answer)
{
    const base::Result<QJsonObject> said = objectFrom(answer);
    if (!said) {
        return said.error();
    }
    const QJsonObject& is = said.value();
    if (!is["initialized"_L1].isBool() || !is["authenticated"_L1].isBool()) {
        return notUnderstood();
    }
    Session session;
    session.setUp = is["initialized"_L1].toBool();
    session.asksName = is["ask_name"_L1].toBool();
    if (is["authenticated"_L1].toBool()) {
        session.account = accountFrom(is["user"_L1]);
        if (!session.account) {
            return notUnderstood();
        }
    }
    return session;
}

base::Result<Account> accountFromSignIn(const QByteArray& answer)
{
    const base::Result<QJsonObject> said = objectFrom(answer);
    if (!said) {
        return said.error();
    }
    const std::optional<Account> account = accountFrom(said.value()["user"_L1]);
    if (!account) {
        return notUnderstood();
    }
    return *account;
}

base::Result<Wall> wallFrom(const QByteArray& answer)
{
    const base::Result<QJsonObject> said = objectFrom(answer);
    if (!said) {
        return said.error();
    }
    const QJsonObject& is = said.value();
    if (!is["devices"_L1].isArray()) {
        return notUnderstood();
    }
    const QJsonArray devices = is["devices"_L1].toArray();
    if (devices.size() > mostMachines) {
        return base::Error{"too_large", "The server lists more machines than this program shows."};
    }
    Wall wall;
    // A pace the server names is kept within what makes sense on a wall: never faster than four a
    // second, never slower than one a minute.
    wall.everySeconds = std::clamp(is["interval"_L1].toDouble(1), 0.25, 60.0);
    wall.liveForm = is["live"_L1].toInt();
    for (const auto& value : devices) {
        const QJsonObject device = value.toObject();
        Machine machine;
        machine.capabilities = capabilitiesFrom(device);
        machine.id = text(device["id"_L1]);
        machine.name = text(device["name"_L1]);
        machine.host = text(device["host"_L1]);
        machine.kind = text(device["kind"_L1]);
        machine.group = text(device["group"_L1]);
        machine.hasPicture = device["picture"_L1].toBool(true);
        machine.page = webAddress(device["open_url"_L1].toString());
        // A PC with Intel AMT: its view, where the server says one, and a word of a view this program does not have
        // is its first. Said with none (an older server: 0.2.2 has no views), it has none.
        if (const QJsonValue view = device["amt_view"_L1]; machine.kind == "amt"_L1 && view.isString()) {
            machine.amtView = isAmtView(text(view)) ? text(view) : u"stills"_s;
        }
        if (machine.id.isEmpty()) {
            return notUnderstood();
        }
        wall.machines.append(machine);
    }
    return wall;
}

base::Result<QHash<QString, Status>> statusFrom(const QByteArray& answer)
{
    const base::Result<QJsonObject> said = objectFrom(answer);
    if (!said) {
        return said.error();
    }
    if (said.value().size() > mostMachines) {
        return base::Error{"too_large", "The server lists more machines than this program shows."};
    }
    QHash<QString, Status> all;
    for (auto entry = said.value().begin(); entry != said.value().end(); ++entry) {
        if (!entry.value().isObject()) {
            return notUnderstood();
        }
        all.insert(entry.key().left(longestText), statusOf(entry.value().toObject()));
    }
    return all;
}

base::Result<Snapshot> snapshotFrom(QByteArray answer, const QByteArray& kind, const QByteArray& signal)
{
    // How a JPEG and a PNG begin, as the engine tells its two kinds of still apart. (Which of
    // the two it says and which it is need not be the same one: the engine goes by how it
    // begins.)
    static constexpr QByteArrayView jpegBegins("\xFF\xD8");
    static constexpr QByteArrayView pngBegins("\x89PNG\r\n\x1A\n");
    const qsizetype more = kind.indexOf(';');
    const QByteArray said = (more < 0 ? kind : kind.first(more)).trimmed().toLower();
    const bool saysSo = said == "image/jpeg" || said == "image/png";
    if (!saysSo || (!answer.startsWith(jpegBegins) && !answer.startsWith(pngBegins))) {
        return notUnderstood();
    }
    Snapshot snapshot;
    snapshot.picture = std::move(answer);
    snapshot.signal = signal != "0";
    return snapshot;
}

base::Result<QList<SavedAction>> savedActionsFrom(const QByteArray& answer)
{
    const base::Result<QJsonObject> said = objectFrom(answer);
    if (!said) {
        return said.error();
    }
    if (!said.value()["actions"_L1].isArray()) {
        return notUnderstood();
    }
    const QJsonArray actions = said.value()["actions"_L1].toArray();
    if (actions.size() > mostSavedActions) {
        return base::Error{"too_large", "The server lists more saved actions than this program shows."};
    }
    // What an action is known by goes back to the server in a question: only what the server makes.
    static const QRegularExpression isId(u"^[A-Za-z0-9_-]{1,32}$"_s);
    QList<SavedAction> all;
    for (const auto& value : actions) {
        const QJsonObject action = value.toObject();
        SavedAction saved;
        saved.id = text(action["id"_L1]);
        saved.name = text(action["name"_L1]);
        const QString kind = text(action["kind"_L1]);
        if (!isId.match(saved.id).hasMatch() || saved.name.isEmpty() || (kind != "keys"_L1 && kind != "text"_L1)) {
            return notUnderstood();
        }
        saved.types = kind == "text"_L1;
        saved.keys = saved.types ? QString() : text(action["keys"_L1]);
        saved.enter = saved.types && action["enter"_L1].toBool();
        saved.danger = action["danger"_L1].toBool();
        saved.changed = whole(action["changed"_L1], 0, std::numeric_limits<int>::max());
        all.append(saved);
    }
    return all;
}

base::Result<QList<WakeTarget>> wakeTargetsFrom(const QByteArray& answer)
{
    const base::Result<QJsonObject> said = objectFrom(answer);
    if (!said) {
        return said.error();
    }
    if (!said.value()["targets"_L1].isArray()) {
        return notUnderstood();
    }
    const QJsonArray targets = said.value()["targets"_L1].toArray();
    if (targets.size() > mostWakeTargets) {
        return base::Error{"too_large", "The server lists more computers to wake than this program shows."};
    }
    // An address goes back to the server in a question, as it came: only what is one.
    static const QRegularExpression isMac(u"^[0-9A-Fa-f]{2}([:-][0-9A-Fa-f]{2}){5}$"_s);
    QList<WakeTarget> all;
    for (const auto& value : targets) {
        const QJsonObject target = value.toObject();
        WakeTarget one{text(target["name"_L1]), text(target["mac"_L1])};
        if (!isMac.match(one.mac).hasMatch()) {
            return notUnderstood();
        }
        all.append(one);
    }
    return all;
}

bool supports(const Machine& machine, const Status* status, const QString& ability)
{
    const auto& capabilities = status != nullptr && status->capabilities ? status->capabilities : machine.capabilities;
    return !capabilities || capabilities->contains(ability);
}

bool isAmtView(const QString& word)
{
    return word == "health"_L1 || word == "stills"_L1 || word == "live"_L1;
}

QString amtViewOf(const Machine& machine, const Status* status)
{
    if (machine.kind != "amt"_L1) {
        return {};
    }
    // (The status's is the fresher: another window or browser may have changed it.)
    if (status != nullptr && status->server && isAmtView(status->server->amtView)) {
        return status->server->amtView;
    }
    // (Where the server said none, it has no views: none is offered or said, b11.)
    return isAmtView(machine.amtView) ? machine.amtView : QString();
}

bool hasAScreenToShow(const Machine& machine, const Status* status)
{
    const bool hasOne = machine.hasPicture || (status != nullptr && status->hasScreen);
    return hasOne && amtViewOf(machine, status) != "health"_L1;
}

void readStill(Snapshot& snapshot, const QByteArray& still, const QByteArray& age)
{
    snapshot.still = still.trimmed() == "1";
    snapshot.ageSeconds.reset();
    if (!snapshot.still) {
        return;
    }
    // In whole seconds, as the server writes it; anything else says no age.
    static const QRegularExpression whole(u"\\A[0-9]{1,9}\\z"_s);
    const QString written = QString::fromLatin1(age.trimmed());
    if (whole.match(written).hasMatch()) {
        snapshot.ageSeconds = written.toInt();
    }
}

base::Result<QString> amtViewFrom(const QByteArray& answer)
{
    const base::Result<QJsonObject> said = objectFrom(answer);
    if (!said) {
        return said.error();
    }
    const QString view = text(said.value()["amt_view"_L1]);
    if (!said.value()["ok"_L1].toBool() || !isAmtView(view)) {
        return notUnderstood();
    }
    return view;
}

QUrl webAddress(const QString& written)
{
    if (written.isEmpty() || written.size() > longestAddress) {
        return {};
    }
    const QUrl address(written, QUrl::StrictMode);
    const QString scheme = address.scheme();
    const bool isOne = address.isValid() && (scheme == "http"_L1 || scheme == "https"_L1) &&
                       !address.host().isEmpty() && address.userInfo().isEmpty();
    return isOne ? address : QUrl();
}

bool sameSite(const QUrl& one, const QUrl& other)
{
    const auto portOf = [](const QUrl& address) { return address.port(address.scheme() == "https"_L1 ? 443 : 80); };
    return !one.isEmpty() && one.scheme() == other.scheme() &&
           one.host().compare(other.host(), Qt::CaseInsensitive) == 0 && portOf(one) == portOf(other);
}

base::Result<QUrl> consoleFrom(const QByteArray& answer)
{
    const base::Result<QJsonObject> said = objectFrom(answer);
    if (!said) {
        return said.error();
    }
    const QUrl address = webAddress(said.value()["url"_L1].toString());
    if (address.isEmpty()) {
        return notUnderstood();
    }
    return address;
}

base::Result<Done> doneFrom(const QByteArray& answer)
{
    const base::Result<QJsonObject> said = objectFrom(answer);
    if (!said) {
        return said.error();
    }
    if (!said.value()["ok"_L1].toBool()) {
        return notUnderstood();
    }
    return Done{text(said.value()["note"_L1])};
}

base::Result<QByteArray> livePassFrom(const QByteArray& answer)
{
    const base::Result<QJsonObject> said = objectFrom(answer);
    if (!said) {
        return said.error();
    }
    if (said.value()["v"_L1].toInt() != spokenLiveForm) {
        return base::Error{"live_form", "This qKVM sends live pictures in a way this program does not know."};
    }
    // Letters, digits and the two signs a pass is written with, and not longer than any is.
    static const QRegularExpression shape(u"\\A[A-Za-z0-9_-]{16,200}\\z"_s);
    const QString pass = said.value()["ticket"_L1].toString();
    if (!shape.match(pass).hasMatch()) {
        return notUnderstood();
    }
    return pass.toLatin1();
}

QStringList wallFileParts()
{
    return {u"devices"_s, u"actions"_s, u"alerts"_s, u"wall"_s};
}

base::Result<WallFile> wallFileFrom(const QByteArray& answer, const QByteArray& notOpened)
{
    // The file is the server's to make and to read. What is looked at here is that it is one: of a
    // size a file has, and saying what every file the server writes says of itself.
    const QJsonObject said =
        answer.size() > largestFileBytes ? QJsonObject() : QJsonDocument::fromJson(answer).object();
    if (said["format"_L1].toString() != "qkvm-export"_L1) {
        return notUnderstood();
    }
    WallFile file;
    file.text = answer;
    file.sealed = said.contains("sealed"_L1);
    bool isNumber = false;
    const int left = notOpened.left(12).trimmed().toInt(&isNumber);
    file.notOpened = isNumber && left > 0 ? left : 0;
    return file;
}

bool isSealedFile(const QByteArray& file)
{
    return file.size() <= largestFileBytes && QJsonDocument::fromJson(file).object().contains("sealed"_L1);
}

base::Result<FileContents> fileContentsFrom(const QByteArray& answer)
{
    // (What is said of a file is as long as the file: every device and every text is in it.)
    const base::Result<QJsonObject> said = objectFrom(answer, largestFileBytes);
    if (!said) {
        return said.error();
    }
    const QJsonObject& is = said.value();
    // A part the file does not have is null; what is there has to be of its kind.
    const auto isListOrNothing = [](const QJsonValue& value) { return value.isArray() || value.isNull(); };
    const auto isObjectOrNothing = [](const QJsonValue& value) { return value.isObject() || value.isNull(); };
    if (!is["encrypted"_L1].isBool() || !isListOrNothing(is["devices"_L1]) || !isListOrNothing(is["actions"_L1]) ||
        !isObjectOrNothing(is["alerts"_L1]) || !isObjectOrNothing(is["wall"_L1]) || !is["left_out"_L1].isArray()) {
        return notUnderstood();
    }
    FileContents contents;
    contents.encrypted = is["encrypted"_L1].toBool();
    contents.hasSecrets = is["secrets"_L1].toBool();
    if (const std::optional<double> made = number(is["made"_L1]); made && *made > 0 && *made < 4e12) {
        contents.madeAt = static_cast<qint64>(*made);
    }
    contents.program = text(is["program"_L1]);

    if (is["devices"_L1].isArray()) {
        const QJsonArray devices = is["devices"_L1].toArray();
        if (devices.size() > mostMachines) {
            return base::Error{"too_large", "The file lists more machines than this program shows."};
        }
        QList<FileDevice> all;
        for (const auto& value : devices) {
            const QJsonObject device = value.toObject();
            FileDevice one;
            one.name = text(device["name"_L1]);
            one.host = text(device["host"_L1]);
            if (one.name.isEmpty() || one.host.isEmpty()) {
                return notUnderstood();
            }
            one.group = text(device["group"_L1]);
            one.hasPassword = device["password"_L1].toBool();
            one.tls = text(device["tls"_L1]);
            one.openUrl = device["open_url"_L1].toString().left(longestAddress);
            one.isNew = device["new"_L1].toBool();
            one.sameAs = text(device["same_as"_L1]);
            all.append(one);
        }
        contents.devices = all;
    }
    if (is["actions"_L1].isArray()) {
        const QJsonArray actions = is["actions"_L1].toArray();
        if (actions.size() > mostSavedActions) {
            return base::Error{"too_large", "The file lists more saved actions than this program shows."};
        }
        QList<FileAction> all;
        for (const auto& value : actions) {
            const QJsonObject action = value.toObject();
            FileAction one;
            one.name = text(action["name"_L1]);
            const QString kind = text(action["kind"_L1]);
            if (one.name.isEmpty() || (kind != "keys"_L1 && kind != "text"_L1)) {
                return notUnderstood();
            }
            // What an action types is shown whole to whoever takes it in, or the reading is none:
            // a text that was cut would hide its end. (The server counts characters.)
            const QString typed = action["text"_L1].toString();
            if (typed.size() > 2 * longestTypedText) {
                return notUnderstood();
            }
            one.types = kind == "text"_L1;
            one.keys = one.types ? QString() : text(action["keys"_L1]);
            one.text = one.types ? typed : QString();
            one.enter = one.types && action["enter"_L1].toBool();
            one.danger = action["danger"_L1].toBool();
            one.isNew = action["new"_L1].toBool();
            all.append(one);
        }
        contents.actions = all;
    }
    if (is["alerts"_L1].isObject()) {
        const QJsonObject alerts = is["alerts"_L1].toObject();
        FileAlerts told;
        told.enabled = alerts["enabled"_L1].toBool();
        told.hasAddress = alerts["address"_L1].toBool();
        told.host = text(alerts["host"_L1]);
        // (There are four kinds of alert; a list of thousands is not one of them.)
        const QJsonArray kinds = alerts["kinds"_L1].toArray();
        for (qsizetype index = 0; index < kinds.size() && index < 8; ++index) {
            if (const QString word = text(kinds.at(index)); !word.isEmpty()) {
                told.kinds.append(word);
            }
        }
        contents.alerts = told;
    }
    // (A file that has the part and nothing in it that is understood has nothing to set: as if it
    // had none.)
    const QJsonObject wall = is["wall"_L1].toObject();
    FileWallSettings settings;
    if (const std::optional<double> every = number(wall["interval"_L1]); every && *every > 0 && *every <= 3600) {
        settings.pictureEverySeconds = *every;
    }
    if (wall["update_check"_L1].isBool()) {
        settings.asksForNewerVersions = wall["update_check"_L1].toBool();
    }
    if (settings.pictureEverySeconds || settings.asksForNewerVersions) {
        contents.wall = settings;
    }
    const QJsonArray leftOut = is["left_out"_L1].toArray();
    for (qsizetype index = 0; index < leftOut.size() && index < mostLeftOut; ++index) {
        if (const QString why = text(leftOut.at(index)); !why.isEmpty()) {
            contents.leftOut.append(why);
        }
    }
    return contents;
}

base::Result<Taken> takenFrom(const QByteArray& answer)
{
    const base::Result<QJsonObject> said = objectFrom(answer);
    if (!said) {
        return said.error();
    }
    const QJsonObject& is = said.value();
    if (!is["devices"_L1].isDouble() || !is["actions"_L1].isDouble() || !is["alerts"_L1].isBool() ||
        !is["wall"_L1].isBool()) {
        return notUnderstood();
    }
    Taken taken;
    taken.devices = whole(is["devices"_L1], 0, mostMachines);
    taken.actions = whole(is["actions"_L1], 0, mostSavedActions);
    taken.alerts = is["alerts"_L1].toBool();
    taken.wall = is["wall"_L1].toBool();
    return taken;
}

QByteArray exportBody(const QString& password, const QString& passphrase, const QStringList& parts)
{
    const QJsonObject body{
        {u"password"_s, password}, {u"passphrase"_s, passphrase}, {u"parts"_s, QJsonArray::fromStringList(parts)}};
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

QByteArray importBody(const QByteArray& file, const QString& passphrase, const QStringList* take)
{
    QJsonObject body{{u"file"_s, QString::fromUtf8(file)}, {u"passphrase"_s, passphrase}};
    if (take != nullptr) {
        body.insert(u"take"_s, QJsonArray::fromStringList(*take));
    }
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

base::Result<QStringList> orderFrom(const QByteArray& answer)
{
    const base::Result<QJsonObject> said = objectFrom(answer);
    if (!said) {
        return said.error();
    }
    if (!said.value()["ok"_L1].toBool() || !said.value()["ids"_L1].isArray()) {
        return notUnderstood();
    }
    const QJsonArray ids = said.value()["ids"_L1].toArray();
    if (ids.size() > mostMachines) {
        return base::Error{"too_large", "The server lists more machines than this program shows."};
    }
    QStringList order;
    for (const auto& id : ids) {
        if (text(id).isEmpty()) {
            return notUnderstood();
        }
        order.append(text(id));
    }
    return order;
}

Action Action::plain(const QString& kind)
{
    Action action;
    action.kind = kind;
    return action;
}

Action Action::press(const QString& keys)
{
    Action action = plain(u"keys"_s);
    action.keys = keys;
    return action;
}

Action Action::type(const QString& text, bool enter)
{
    Action action = plain(u"text"_s);
    action.text = text;
    action.enter = enter;
    return action;
}

Action Action::show(int port)
{
    Action action = plain(u"port"_s);
    action.port = port;
    return action;
}

Action Action::wake(const QString& mac)
{
    Action action = plain(u"wol"_s);
    action.mac = mac;
    return action;
}

Action Action::run(const SavedAction& saved)
{
    Action action = plain(u"custom"_s);
    action.savedId = saved.id;
    action.savedChanged = saved.changed;
    action.savedTypes = saved.types;
    return action;
}

QByteArray bodyOf(const Action& action)
{
    QJsonObject body{{u"action"_s, action.kind}};
    if (action.kind == "keys"_L1) {
        body[u"keys"_s] = action.keys;
    } else if (action.kind == "text"_L1) {
        body[u"text"_s] = action.text;
        body[u"enter"_s] = action.enter;
    } else if (action.kind == "port"_L1) {
        body[u"port"_s] = action.port;
    } else if (action.kind == "wol"_L1) {
        body[u"mac"_s] = action.mac;
    } else if (action.kind == "custom"_L1) {
        body[u"custom_id"_s] = action.savedId;
        body[u"custom_changed"_s] = action.savedChanged;
    }
    if (!action.batch.isEmpty()) {
        body[u"batch"_s] = action.batch;
    }
    if (action.watching) {
        body[u"watching"_s] = true;
    }
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

bool isRefusalOfAServers(const QByteArray& answer)
{
    if (answer.size() > largestAnswerBytes) {
        return false;
    }
    const QJsonObject said = QJsonDocument::fromJson(answer).object();
    return !text(said["code"_L1]).isEmpty() && !text(said["message"_L1]).isEmpty();
}

base::Error refusalFrom(int httpStatus, const QByteArray& answer)
{
    if (isRefusalOfAServers(answer)) {
        const QJsonObject said = QJsonDocument::fromJson(answer).object();
        const QString code = text(said["code"_L1]);
        // The server's word for why, as it is. One with a colon in it is no word of a server's:
        // so this program writes what it finds itself (ServerClient::unreachable and the
        // others), and no answer is taken for one of those. The sentence is told all the same.
        return {code.contains(u':') ? "refused" : code.toStdString(), text(said["message"_L1]).toStdString()};
    }
    return {"refused", "The server refused (" + std::to_string(httpStatus) + ") and did not say why."};
}

base::Result<MediaList> mediaListFrom(const QByteArray& answer)
{
    const base::Result<QJsonObject> said = objectFrom(answer);
    if (!said) {
        return said.error();
    }
    const QJsonObject& is = said.value();
    if (!is["images"_L1].isArray() || !is["offers"_L1].isObject()) {
        return notUnderstood();
    }
    const QJsonArray images = is["images"_L1].toArray();
    if (images.size() > mostMediaImages) {
        return base::Error{"too_large", "The server lists more images than this program shows."};
    }
    MediaList list;
    for (const auto& value : images) {
        const QJsonObject image = value.toObject();
        MediaImage one;
        one.id = imageIdOf(image["id"_L1]);
        one.name = line(image["name"_L1]);
        if (one.id.isEmpty() || one.name.isEmpty()) {
            ++list.leftOut;
            continue;
        }
        one.where = line(image["where"_L1]);
        one.size = bytes(image["size"_L1]);
        one.partial = yes(image["partial"_L1]);
        one.sha256 = line(image["sha256"_L1], 64);
        list.images.append(one);
    }
    list.unseen = sentencesOf(is["unseen"_L1]);
    list.notes = sentencesOf(is["notes"_L1]);
    list.leftOut += whole(is["left_out"_L1], 0, 1'000'000);
    const QJsonObject space = is["space"_L1].toObject();
    list.free = bytes(space["free"_L1]);
    list.size = bytes(space["size"_L1]);
    list.mounted = mountedFrom(is["mounted"_L1]);
    const QJsonObject offers = is["offers"_L1].toObject();
    list.offers.drives = wordsOf(offers["drives"_L1], {"cd"_L1, "flash"_L1, "disk"_L1});
    list.offers.boot = wordsOf(offers["boot"_L1], {"once"_L1, "until_unmounted"_L1});
    list.offers.restart = yes(offers["restart"_L1]);
    list.offers.upload = yes(offers["upload"_L1]);
    list.offers.where = line(offers["where"_L1], 16);
    for (const QString& drive : wordsOf(offers["writable"_L1], {"cd"_L1, "flash"_L1, "disk"_L1})) {
        if (list.offers.drives.contains(drive)) {
            list.offers.writable.append(drive);
        }
    }
    list.offers.canDelete = yes(offers["delete"_L1]);
    list.offers.dropsInput = yes(offers["drops_input"_L1]);
    list.uploading = uploadingOf(is["uploading"_L1]);
    list.servedFrom = line(is["served_from"_L1], 300);
    return list;
}

base::Result<std::optional<MediaUploading>> uploadingFrom(const QByteArray& answer)
{
    const base::Result<QJsonObject> said = objectFrom(answer);
    if (!said) {
        return said.error();
    }
    if (!said.value().contains("uploading"_L1)) {
        return notUnderstood();
    }
    return uploadingOf(said.value()["uploading"_L1]);
}

base::Result<MediaDone> mediaDoneFrom(const QByteArray& answer)
{
    const base::Result<QJsonObject> said = objectFrom(answer);
    if (!said) {
        return said.error();
    }
    if (!yes(said.value()["ok"_L1])) {
        return notUnderstood();
    }
    return MediaDone{line(said.value()["note"_L1], 600), mountedFrom(said.value()["mounted"_L1])};
}

base::Result<MediaUploaded> uploadedFrom(const QByteArray& answer)
{
    const base::Result<QJsonObject> said = objectFrom(answer);
    if (!said) {
        return said.error();
    }
    if (!yes(said.value()["ok"_L1])) {
        return notUnderstood();
    }
    return MediaUploaded{line(said.value()["sha256"_L1], 64)};
}

QByteArray mountBody(const MediaMount& mount)
{
    const QJsonObject body{{u"image"_s, mount.image}, {u"machine"_s, mount.machine},
                           {u"drive"_s, mount.drive}, {u"writable"_s, mount.writable},
                           {u"boot"_s, mount.boot},   {u"restart"_s, mount.restart}};
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

QByteArray imageBody(const QString& image, const QString& machine)
{
    return QJsonDocument(QJsonObject{{u"image"_s, image}, {u"machine"_s, machine}}).toJson(QJsonDocument::Compact);
}

bool isImageName(const QString& name)
{
    static const QRegularExpression rule(
        uR"(\A[A-Za-z0-9_+()\[\]~-](?:[A-Za-z0-9 _.,+()\[\]~-]{0,198}[A-Za-z0-9_+()\[\]~-])?\z)"_s);
    return rule.match(name).hasMatch() && !name.endsWith(".incomplete"_L1);
}
} // namespace qkvm::core
