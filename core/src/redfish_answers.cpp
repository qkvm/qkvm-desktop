#include <qkvm/core/redfish_answers.h>

#include <algorithm>
#include <cmath>

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>

#include <qkvm/core/kvmd_answers.h>

using namespace Qt::StringLiterals;

namespace qkvm::core::redfish {

static_assert(saidNone.size() == 7 + 32);

namespace {

// A value some way down an answer; nothing (a null) where the way does not lead through objects (the
// web's _dig).
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

// What a controller sent where a list belongs: the list, or none. (A number there, or a yes, is not gone
// through.)
QJsonArray listed(const QJsonValue& value)
{
    return value.isArray() ? value.toArray() : QJsonArray();
}

// Whether a sensor is well: its health not said (or said as nothing), or "OK".
bool isWell(const QJsonValue& entry)
{
    const QJsonValue health = at(entry, {"Status"_L1, "Health"_L1});
    return health.isNull() || health.isUndefined() || health == QJsonValue(u"OK"_s);
}

// What is true of a value as a script would ask it.
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

// The digest a number is kept as; empty where it is no number worth the name (the web's _digest).
QString digestOf(const QJsonValue& number)
{
    static const QRegularExpression shape(u"\\A[0-9A-Fa-f]{8}-(?:[0-9A-Fa-f]{4}-){3}[0-9A-Fa-f]{12}\\z"_s);
    if (!number.isString() || !shape.match(number.toString()).hasMatch()) {
        return {};
    }
    const QString lower = number.toString().toLower();
    QString digits = lower;
    digits.remove(u'-');
    if (QSet<QChar>(digits.cbegin(), digits.cend()).size() < 4) {
        return {};
    }
    return QString::fromLatin1(QCryptographicHash::hash(lower.toUtf8(), QCryptographicHash::Sha256).toHex().first(32));
}

// What the controller said in an error answer, in its own words: the first of its extended messages,
// else its error's message; empty where it said nothing that reads as words.
QString saidIn(const QByteArray& body)
{
    const QJsonDocument document = QJsonDocument::fromJson(body);
    if (!document.isObject()) {
        return {};
    }
    const QJsonValue error = document.object().value("error"_L1);
    const QJsonValue detail = at(error, {"@Message.ExtendedInfo"_L1});
    if (!saysSomething(detail)) {
        return kvmd::plain(at(error, {"message"_L1}), 200);
    }
    // (Said as something that is not a list of messages: not shaped the way an error is.)
    return detail.isArray() ? kvmd::plain(at(detail.toArray().first(), {"Message"_L1}), 200) : QString();
}

} // namespace

QString link(const QJsonValue& value)
{
    // (A "%" only as an escape of two hexadecimal digits, as the web's parser takes it: one that is none
    // would be read here as one, and hide a backslash or a slash, "%%5c".)
    static const QRegularExpression path(u"\\A/redfish/v1(/(?:[A-Za-z0-9._~!$&'()*+,;=:@-]|%[0-9A-Fa-f]{2})+)*/?\\z"_s);
    if (!value.isString()) {
        return {};
    }
    QString said = value.toString();
    if (said.size() > 300 || !path.match(said).hasMatch()) {
        return {};
    }
    // Stricter than the web: no escape of an escape ("%25": decoded once more on its way, by whatever
    // decodes it, it is another place), none of a byte beyond ASCII (an overlong ".", "%c0%ae", among
    // them), and none of a sign that does not print (a line feed, a nought). No name a controller gives a
    // place of its own has any of them.
    const QByteArray decoded = QByteArray::fromPercentEncoding(said.toLatin1());
    if (said.contains(u"%25"_s, Qt::CaseInsensitive) || std::ranges::any_of(decoded, [](char sign) {
            const auto byte = static_cast<unsigned char>(sign);
            return byte >= 0x80 || byte < 0x20 || byte == 0x7f;
        })) {
        return {};
    }
    // And no way back up out of /redfish/v1, however it is spelt: the question would be sent to where that
    // leads.
    const QString spelt = QString::fromLatin1(decoded);
    if (spelt.contains(u'\\') || spelt.count(u'/') != said.count(u'/')) {
        return {};
    }
    const QStringList parts = spelt.split(u'/');
    if (parts.contains(u"."_s) || parts.contains(u".."_s)) {
        return {};
    }
    return said;
}

QString named(std::initializer_list<QJsonValue> values)
{
    static const QRegularExpression placeholder(
        u"\\A\\s*(to be filled by o\\.?e\\.?m\\.?|default string|system product name|not specified|n/?a|unknown)?\\s*\\z"_s,
        QRegularExpression::CaseInsensitiveOption);
    for (const QJsonValue& value : values) {
        const QString text = kvmd::plain(value, 60);
        if (!text.isEmpty() && !placeholder.match(text).hasMatch()) {
            return text;
        }
    }
    return {};
}

std::optional<double> number(const QJsonValue& value, double least, double most)
{
    if (!value.isDouble()) {
        return std::nullopt;
    }
    const double is = value.toDouble();
    return is >= least && is <= most ? std::optional<double>(is) : std::nullopt;
}

QByteArray token(const QByteArray& said)
{
    return kvmd::isToken(said) ? said : QByteArray();
}

QByteArray markOf(const QByteArray& etag, const QJsonObject& resource)
{
    static const QRegularExpression shape(u"\\A(W/)?\"[\\x21\\x23-\\x7e]{0,128}\"\\z"_s);
    const QJsonValue own = resource.value("@odata.etag"_L1);
    for (const QString& mark : {QString::fromLatin1(etag), own.isString() ? own.toString() : QString()}) {
        if (shape.match(mark).hasMatch()) {
            return mark.toLatin1();
        }
    }
    return {};
}

base::Result<QJsonObject> resourceFrom(const QString& path, int status, const QByteArray& body)
{
    if (status != 200) {
        return base::Error{"bad_response", u"%1 failed (HTTP %2)"_s.arg(path).arg(status).toStdString()};
    }
    const QJsonDocument document = QJsonDocument::fromJson(body);
    if (!document.isObject()) {
        return base::Error{"bad_response", u"%1 returned unexpected data"_s.arg(path).toStdString()};
    }
    const QJsonObject data = document.object();
    if (data.contains("error"_L1) && !data.contains("@odata.id"_L1)) {
        // An error, said with HTTP 200, as the standard's record or as a bare text. Every resource has its own
        // address in it and no "error"; this has it the other way round, and is no resource.
        QString said = kvmd::plain(at(data, {"error"_L1, "message"_L1}), 120);
        if (said.isEmpty()) {
            said = kvmd::plain(data.value("error"_L1), 120);
        }
        if (said.isEmpty()) {
            said = u"without a word"_s;
        }
        return base::Error{"bad_response", u"%1 answered an error: %2"_s.arg(path, said).toStdString()};
    }
    return data;
}

// --- the sensors ------------------------------------------------------------------------------------

QList<Temperature> temperaturesOf(const QJsonObject& thermal)
{
    QList<Temperature> found;
    const QJsonArray entries = listed(thermal.value("Temperatures"_L1));
    for (qsizetype index = 0; index < entries.size() && index < mostSensors; ++index) {
        const QJsonValue entry = entries.at(index);
        const std::optional<double> reading = number(at(entry, {"ReadingCelsius"_L1}), -100, 300);
        if (entry.isObject() && reading) {
            found.append(Temperature{kvmd::plain(at(entry, {"Name"_L1}), 40), *reading, isWell(entry)});
        }
    }
    return found;
}

QList<Fan> fansOf(const QJsonObject& thermal)
{
    QList<Fan> found;
    const QJsonArray entries = listed(thermal.value("Fans"_L1));
    for (qsizetype index = 0; index < entries.size() && index < mostSensors; ++index) {
        const QJsonValue entry = entries.at(index);
        if (entry.isObject() && at(entry, {"Status"_L1, "State"_L1}) == QJsonValue(u"Enabled"_s)) {
            found.append(Fan{kvmd::plain(at(entry, {"Name"_L1}), 40), number(at(entry, {"Reading"_L1}), 0, 1'000'000),
                             at(entry, {"ReadingUnits"_L1}) == QJsonValue(u"Percent"_s), isWell(entry)});
        }
    }
    return found;
}

QList<Temperature> readingsOf(const QJsonObject& metrics)
{
    QList<Temperature> found;
    const QJsonArray entries = listed(metrics.value("TemperatureReadingsCelsius"_L1));
    for (qsizetype index = 0; index < entries.size() && index < mostSensors; ++index) {
        const QJsonValue entry = entries.at(index);
        const std::optional<double> reading = number(at(entry, {"Reading"_L1}), -100, 300);
        if (!entry.isObject() || !reading) {
            continue;
        }
        QString name = kvmd::plain(at(entry, {"DeviceName"_L1}), 40);
        if (const QString source = link(at(entry, {"DataSourceUri"_L1})); name.isEmpty() && !source.isEmpty()) {
            QString trimmed = source;
            while (trimmed.endsWith(u'/')) {
                trimmed.chop(1);
            }
            name = kvmd::plain(trimmed.section(u'/', -1), 40);
        }
        found.append(Temperature{name, *reading, isWell(entry)});
    }
    return found;
}

std::optional<Fan> fanOf(const QJsonValue& fan)
{
    if (!fan.isObject() || at(fan, {"Status"_L1, "State"_L1}) != QJsonValue(u"Enabled"_s)) {
        return std::nullopt;
    }
    const QJsonValue speed = at(fan, {"SpeedPercent"_L1});
    if (!speed.isObject()) {
        return std::nullopt;
    }
    const std::optional<double> revolutions = number(at(speed, {"SpeedRPM"_L1}), 0, 1'000'000);
    const std::optional<double> share = number(at(speed, {"Reading"_L1}), 0, 100);
    const bool inRevolutions = revolutions || (!share && speed.toObject().contains("SpeedRPM"_L1));
    return Fan{kvmd::plain(at(fan, {"Name"_L1}), 40), inRevolutions ? revolutions : share, !inRevolutions, isWell(fan)};
}

// --- the light --------------------------------------------------------------------------------------

std::optional<Light> locator(const QJsonObject& system)
{
    if (const QJsonValue active = system.value("LocationIndicatorActive"_L1); active.isBool()) {
        return Light{active.toBool(), QJsonObject{{u"LocationIndicatorActive"_s, true}},
                     QJsonObject{{u"LocationIndicatorActive"_s, false}}};
    }
    const QJsonValue state = system.value("IndicatorLED"_L1);
    const QJsonValue said = system.value("IndicatorLED@Redfish.AllowableValues"_L1);
    QStringList offered;
    if (said.isArray()) {
        for (const QJsonValue kind : said.toArray()) {
            if (kind.isString()) {
                offered.append(kind.toString());
            }
        }
    } else {
        offered = {u"Lit"_s, u"Blinking"_s, u"Off"_s};
    }
    // On most, "Off" is off, and "Lit" and "Blinking" both say that somebody is looking for the machine. Where
    // there is no "Off" to choose, "Lit" is the light at rest and only "Blinking" calls for attention.
    const QString off = offered.contains(u"Off"_s) ? u"Off"_s : u"Lit"_s;
    QString on;
    for (const QString& kind : {u"Lit"_s, u"Blinking"_s}) {
        if (offered.contains(kind) && kind != off) {
            on = kind;
            break;
        }
    }
    const QStringList states{u"Lit"_s, u"Blinking"_s, u"Off"_s};
    if (!state.isString() || !states.contains(state.toString()) || on.isEmpty() || !offered.contains(off)) {
        return std::nullopt;
    }
    return Light{state.toString() != off, QJsonObject{{u"IndicatorLED"_s, on}}, QJsonObject{{u"IndicatorLED"_s, off}}};
}

// --- which server answers ---------------------------------------------------------------------------

QString serverNumber(const QJsonObject& system)
{
    // (Nor a number its board's maker put on every board of the make: AMI's, as its firmware gives it, and in the byte
    // order of the SMBIOS table it is read from. Two nodes that give it cannot be told apart by it, and one alone may
    // be either: such a computer is one that gives no number, held to where the card's computer was found. Outside
    // review 2, 3.2: one of two that gave it passed for the card's where the other was out.)
    static const QStringList everyBoards{u"03000200-0400-0500-0006-000700080009"_s,
                                         u"00020003-0004-0005-0006-000700080009"_s};
    const QJsonValue number = system.value("UUID"_L1);
    if (number.isString() && everyBoards.contains(number.toString().toLower())) {
        return {};
    }
    const QString digest = digestOf(number);
    return digest.isEmpty() ? QString() : u"server:"_s + digest;
}

QString controllerNumber(const QJsonValue& root)
{
    return root.isObject() ? digestOf(root.toObject().value("UUID"_L1)) : QString();
}

bool isOpenBmc(const QJsonObject& manager)
{
    return manager.value("Model"_L1) == "OpenBmc"_L1 ||
           manager.value("Oem"_L1).toObject().value("OpenBmc"_L1).isObject();
}

QString rfbConsole(const QJsonObject& manager)
{
    const QJsonObject console = manager.value("GraphicalConsole"_L1).toObject();
    const QJsonValue kinds = console.value("ConnectTypesSupported"_L1);
    const bool kvm = kinds.isArray() && kinds.toArray().contains(QJsonValue(u"KVMIP"_s));
    if (isOpenBmc(manager) && console.value("ServiceEnabled"_L1) == QJsonValue(true) && kvm) {
        return u"/kvm/0"_s;
    }
    return {};
}

bool amiRoot(const QJsonObject& root)
{
    return kvmd::plain(root.value("Vendor"_L1), 40).toUpper() == "AMI"_L1;
}

QStringList managerNumbers(const QJsonObject& manager)
{
    QStringList named;
    for (const QLatin1StringView name : {"ServiceEntryPointUUID"_L1, "UUID"_L1}) {
        if (const QString digest = digestOf(manager.value(name)); !digest.isEmpty() && !named.contains(digest)) {
            named.append(digest);
        }
    }
    return named;
}

Marks marksOf(const QString& machine)
{
    // A computer's mark kept of a maker's placeholder (0.1.x took AMI's for a number, as every published version before
    // 0.2.0 did) is read as one that gave none: the card is then known by its controller alone, as a card added for
    // such a board now is (review cx2-2 of outside review 2, M3: it refused everything, its words offering a way past
    // that could not come). A rule of reading only: what is kept is not written anew for it.
    static const QStringList placeholders{u"server:"_s + digestOf(u"03000200-0400-0500-0006-000700080009"_s),
                                          u"server:"_s + digestOf(u"00020003-0004-0005-0006-000700080009"_s)};
    Marks marks{machine, {}};
    if (machine.startsWith(u"server:"_s) && machine.size() == saidNone.size() + 32) {
        marks = Marks{machine.first(saidNone.size()), machine.sliced(saidNone.size())};
    }
    if (placeholders.contains(marks.server)) {
        marks.server = QString(saidNone);
    }
    return marks;
}

// --- what is done to it -----------------------------------------------------------------------------

Offered offeredKinds(const QJsonObject& resource, QLatin1StringView action)
{
    const QJsonValue described = at(resource, {"Actions"_L1, action});
    Offered offered;
    offered.target = link(at(described, {"target"_L1}));
    if (const QJsonValue kinds = at(described, {"ResetType@Redfish.AllowableValues"_L1}); kinds.isArray()) {
        QStringList texts;
        for (const QJsonValue kind : kinds.toArray()) {
            if (kind.isString()) {
                texts.append(kind.toString());
            }
        }
        offered.kinds = texts;
    } else {
        offered.described = link(at(described, {"@Redfish.ActionInfo"_L1}));
    }
    return offered;
}

QStringList kindsIn(const QJsonObject& actionInfo)
{
    for (const QJsonValue parameter : listed(actionInfo.value("Parameters"_L1))) {
        if (parameter.isObject() && at(parameter, {"Name"_L1}) == QJsonValue(u"ResetType"_s)) {
            QStringList kinds;
            for (const QJsonValue kind : listed(at(parameter, {"AllowableValues"_L1}))) {
                if (kind.isString()) {
                    kinds.append(kind.toString());
                }
            }
            return kinds;
        }
    }
    return {};
}

QStringList wanted(const QString& action, bool running)
{
    if (action == "power_on"_L1) {
        return {u"On"_s};
    }
    if (action == "power_off"_L1) {
        return {u"GracefulShutdown"_s};
    }
    if (action == "atx_power"_L1) {
        return {running ? u"GracefulShutdown"_s : u"On"_s};
    }
    if (action == "atx_power_long"_L1) {
        return {u"ForceOff"_s};
    }
    if (action == "atx_reset"_L1) {
        return {u"ForceRestart"_s, u"PowerCycle"_s};
    }
    if (action == "reboot"_L1) {
        return {u"ForceRestart"_s, u"GracefulRestart"_s};
    }
    return {};
}

QString kindToSend(const QStringList& wanted, const QStringList& offered)
{
    for (const QString& kind : wanted) {
        if (offered.isEmpty() || offered.contains(kind)) {
            return kind;
        }
    }
    return {};
}

bool runs(const QJsonObject& system)
{
    const QString power = kvmd::plain(system.value("PowerState"_L1), 20);
    return power != "Off"_L1 && power != "PoweringOff"_L1;
}

base::Result<void> verdictOn(int status, const QByteArray& body)
{
    if (status >= 400 && status < 500) {
        const QString said = saidIn(body);
        return base::Error{"action_failed",
                           (said.isEmpty() ? u"The controller refused (HTTP %1)"_s.arg(status) : said).toStdString()};
    }
    if (status != 200 && status != 202 && status != 204) {
        const QString how = status >= 500 ? u"an error of its own"_s : u"an answer a command does not have"_s;
        QString said = saidIn(body);
        if (said.isEmpty()) {
            said = u"HTTP %1"_s.arg(status);
        }
        return base::Error{"timeout", u"The controller answered with %1 (%2): it may or may not have been done. Look "
                                      u"at the server before pressing again."_s.arg(how, said)
                                          .toStdString()};
    }
    return {};
}

QString errorSaid(const QByteArray& body)
{
    return saidIn(body);
}

QStringList bootTargets(const QJsonObject& system)
{
    QStringList targets;
    for (const QJsonValue target :
         listed(at(system, {"Boot"_L1, "BootSourceOverrideTarget@Redfish.AllowableValues"_L1}))) {
        if (target.isString()) {
            targets.append(kvmd::plain(target, 30));
        }
    }
    return targets;
}

std::optional<QStringList> overrideKinds(const QJsonObject& system)
{
    const QJsonValue said = at(system, {"Boot"_L1, "BootSourceOverrideEnabled@Redfish.AllowableValues"_L1});
    if (!said.isArray()) {
        return std::nullopt;
    }
    QStringList kinds;
    for (const QJsonValue kind : listed(said)) {
        if (kind.isString()) {
            kinds.append(kvmd::plain(kind, 30).toLower());
        }
    }
    return kinds;
}

std::pair<QString, QString> bootOverride(const QJsonObject& system)
{
    return {kvmd::plain(at(system, {"Boot"_L1, "BootSourceOverrideEnabled"_L1}), 30).toLower(),
            kvmd::plain(at(system, {"Boot"_L1, "BootSourceOverrideTarget"_L1}), 30).toLower()};
}

std::optional<bool> overrideOff(const QJsonObject& system)
{
    const QString enabled = bootOverride(system).first;
    if (enabled.isEmpty()) {
        return std::nullopt; // (no Boot, a null, no text: nothing said of it)
    }
    return enabled == "disabled"_L1;
}

QString anotherOverride(const QJsonObject& system)
{
    const auto [enabled, target] = bootOverride(system);
    if (enabled.isEmpty() || enabled == "disabled"_L1 || (enabled == "once"_L1 && target == "biossetup"_L1)) {
        return {};
    }
    const QString said = kvmd::plain(at(system, {"Boot"_L1, "BootSourceOverrideTarget"_L1}), 30);
    return (said.isEmpty() ? u"another target"_s : said) + u", "_s +
           kvmd::plain(at(system, {"Boot"_L1, "BootSourceOverrideEnabled"_L1}), 30);
}

bool setupForEveryBoot(const QJsonObject& system)
{
    return bootOverride(system) == std::pair<QString, QString>(u"continuous"_s, u"biossetup"_s);
}

// --- how it is --------------------------------------------------------------------------------------

namespace {

// A place of a Task's own, not its monitor's (the web's: "/Tasks/" in it, and not ending in "/Monitor").
bool aTaskItself(QString place)
{
    while (place.endsWith(u'/')) {
        place.chop(1);
    }
    return place.contains(u"/Tasks/"_s) && !place.endsWith(u"/Monitor"_s);
}

// A place as a link names it, without the slashes it may end in.
QString plainPlace(QString place)
{
    while (place.endsWith(u'/')) {
        place.chop(1);
    }
    return place;
}

// Whether the place a Task names itself by (`itself`; the Task as read, or as a change's answer, `task`) and the place
// `other` name the one task: the same place, a monitor below the Task's own (".../Monitor"), the Task's own above a
// monitor, or the monitor the Task names (TaskMonitor) (outside review of D1, round 2, L4: nothing tied a Task in an
// answer to the monitor beside it).
bool oneTask(const QString& itself, const QJsonObject& task, const QString& other)
{
    const QString own = plainPlace(itself);
    const QString there = plainPlace(other);
    if (own == there || own + u"/Monitor"_s == there || there + u"/Monitor"_s == own) {
        return true;
    }
    const QString monitor = plainPlace(link(task.value("TaskMonitor"_L1)));
    return !monitor.isEmpty() && monitor == there;
}

} // namespace

QString taskPlace(const QByteArray& location, const QByteArray& body)
{
    const auto inTheService = [](const QString& place) { return place.startsWith(u"/redfish/v1/TaskService/"_s); };
    const QJsonObject said = QJsonDocument::fromJson(body).object();
    QString named = link(said.value("@odata.id"_L1));
    if (!inTheService(named)) {
        named.clear();
    }
    QString located;
    if (!location.isEmpty()) {
        // (Its path alone, as the web's urlsplit takes it: asked of this controller, under this session, wherever else
        // it says.)
        const QUrl place(QString::fromLatin1(location), QUrl::StrictMode);
        if (place.isValid()) {
            located = link(place.path(QUrl::FullyEncoded));
        }
        if (!inTheService(located)) {
            located.clear();
        }
    }
    if (!named.isEmpty() && !located.isEmpty() && !oneTask(named, said, located)) {
        // (A Task in the answer, and a Location of another task's: neither is surely this change's.)
        return {};
    }
    QStringList places;
    for (const QString& place : {named, located}) {
        if (!place.isEmpty()) {
            places.append(place);
        }
    }
    std::ranges::stable_sort(places, {}, [](const QString& place) { return !aTaskItself(place); });
    return places.isEmpty() ? QString() : places.constFirst();
}

namespace {

// Whether a Task, read at `place`, is surely of the change made to `of` (outside review of D1, H1): it names itself
// nothing else than that place, where it names itself; and its Payload, where it has one, names a change of that very
// place (by its path, as a Location is read: an address of the controller's own is the same place), by a PATCH. A
// Task that names either amiss is another's, or one a controller hands out for an old change.
bool ofTheChange(const QString& place, const QJsonObject& task, const QString& of)
{
    if (aTaskItself(place) && task.contains("@odata.id"_L1) &&
        plainPlace(link(task.value("@odata.id"_L1))) != plainPlace(place)) {
        return false; // (a link that is none names no place, and so not this one)
    }
    // (At a monitor, a Task that names itself is the monitor's own: its place above it, or one that names it as its
    // TaskMonitor; outside review of D1, round 2, L4.)
    if (!aTaskItself(place) && task.contains("@odata.id"_L1) &&
        !oneTask(link(task.value("@odata.id"_L1)), task, place)) {
        return false;
    }
    const QJsonValue payload = task.value("Payload"_L1);
    if (payload.isUndefined() || payload.isNull()) {
        return true;
    }
    if (!payload.isObject()) {
        return false;
    }
    const QJsonObject change = payload.toObject();
    if (change.contains("TargetUri"_L1)) {
        const QJsonValue target = change.value("TargetUri"_L1);
        const QUrl said(target.toString(), QUrl::StrictMode);
        const QString named = target.isString() && said.isValid() ? link(said.path(QUrl::FullyEncoded)) : QString();
        if (named.isEmpty() || plainPlace(named) != plainPlace(of)) {
            return false;
        }
    }
    if (change.contains("HttpOperation"_L1) &&
        kvmd::plain(change.value("HttpOperation"_L1), 20).compare("PATCH"_L1, Qt::CaseInsensitive) != 0) {
        return false;
    }
    return true;
}

} // namespace

TaskSaid taskOf(const QString& place, int status, const QByteArray& body, const QString& of)
{
    using State = TaskSaid::State;
    const bool itself = aTaskItself(place);
    if (status == 202) {
        return {State::UnderWay, {}}; // (a monitor: still under way)
    }
    if (status == 408 || status == 429 || (status >= 500 && status < 600)) {
        return {State::Passing, {}}; // (the place could not answer just then: nothing said of the task)
    }
    if (status == 204 || (status == 200 && body.isEmpty())) {
        // (A monitor's end: the action's own answer, and it did it. A Task's own place answers with the Task.)
        return {itself ? State::Unreadable : State::Done, {}};
    }
    if (status >= 400) {
        if (itself || status == 401 || status == 404 || status == 410) {
            return {State::Unreadable, {}}; // (a task gone, or not to be read)
        }
        return {State::Refused, saidIn(body)}; // (the change's own answer, as the monitor gives it: no end for certain)
    }
    if (status != 200) {
        return {State::Unreadable, {}};
    }
    const QJsonDocument read = QJsonDocument::fromJson(body);
    if (!read.isObject()) {
        return {State::Unreadable, {}};
    }
    const QJsonObject task = read.object();
    const QString state = kvmd::plain(task.value("TaskState"_L1), 20).toLower();
    if (!ofTheChange(place, task, of)) {
        return {State::Unreadable, {}};
    }
    const bool failed =
        state == "exception"_L1 || state == "killed"_L1 || state == "cancelled"_L1 ||
        (state == "completed"_L1 && kvmd::plain(task.value("TaskStatus"_L1), 20).toLower() == "critical"_L1);
    if (failed) {
        const QJsonArray messages = listed(task.value("Messages"_L1));
        return {State::Failed, messages.isEmpty() ? QString() : kvmd::plain(at(messages.first(), {"Message"_L1}), 200)};
    }
    if (state == "completed"_L1) {
        return {State::Done, {}};
    }
    // (Redfish's TaskState, those that are no end: a word it does not have, or none, says nothing.)
    static const QStringList underWay{u"new"_s,     u"starting"_s, u"running"_s, u"suspended"_s, u"interrupted"_s,
                                      u"pending"_s, u"stopping"_s, u"service"_s, u"cancelling"_s};
    return {underWay.contains(state) ? State::UnderWay : State::Unreadable, {}};
}

Seen statusFrom(const Looked& looked)
{
    const QJsonObject& system = looked.system;
    Seen seen;
    Status& status = seen.status;
    const QString power = kvmd::plain(system.value("PowerState"_L1), 20);
    const std::optional<bool> on =
        power.isEmpty() ? std::nullopt : std::optional<bool>(power != "Off"_L1 && power != "PoweringOff"_L1);

    // Its own health, taken as the whole of it where it says that (HealthRollup) before its own part.
    QJsonValue health = at(system, {"Status"_L1, "HealthRollup"_L1});
    if (!saysSomething(health)) {
        health = at(system, {"Status"_L1, "Health"_L1});
    }
    ServerReadings readings;
    readings.health = kvmd::plain(health, 20);
    QStringList unwell;
    for (const Temperature& one : looked.temperatures) {
        if (!one.ok && !one.name.isEmpty()) {
            unwell.append(one.name);
        }
    }
    for (const Fan& one : looked.fans) {
        if (!one.ok && !one.name.isEmpty()) {
            unwell.append(one.name);
        }
        ++readings.fans;
        readings.fansRunning += one.reading.value_or(0) > 0 ? 1 : 0;
        readings.fansNotWell += one.ok ? 0 : 1;
    }
    if (readings.health == "Critical"_L1) {
        // Its own verdict on itself, and the one thing here that somebody has to come and look at.
        const QString where = unwell.isEmpty()
                                  ? QString()
                                  : u" (%1)"_s.arg(unwell.first(std::min<qsizetype>(3, unwell.size())).join(u", "_s));
        status.state = u"error"_s;
        status.errorCode = u"health"_s;
        status.errorMessage = u"The server reports a critical fault%1"_s.arg(where);
        seen.fault = where;
    } else if (looked.faultHeld) {
        status.state = u"error"_s;
        status.errorCode = u"health"_s;
        status.errorMessage =
            u"The server reported a critical fault%1 a moment ago: it comes and goes"_s.arg(*looked.faultHeld);
    } else {
        status.state = on == std::optional<bool>(false) ? u"no_signal"_s : u"online"_s;
    }
    status.hasPicture = false;
    status.signal = on;
    status.powerIsKnown = true;
    status.power = on;
    // The processors' temperature where it says one, else the highest it says.
    std::optional<double> hottest;
    std::optional<double> hottestOfAll;
    for (const Temperature& one : looked.temperatures) {
        hottestOfAll = std::max(hottestOfAll.value_or(one.celsius), one.celsius);
        if (one.name.contains("cpu"_L1, Qt::CaseInsensitive)) {
            hottest = std::max(hottest.value_or(one.celsius), one.celsius);
        }
    }
    status.temperature = hottest ? hottest : hottestOfAll;
    status.model = named({system.value("Model"_L1), system.value("Manufacturer"_L1)});
    status.version = looked.version;

    const std::optional<Light> light = locator(system);
    if (light) {
        readings.locator = light->on;
    }
    // One board calls its processor "Example Processor 2000 CPU      , 3000 Mhz, 4 Core(s), ...".
    readings.processor = named({at(system, {"ProcessorSummary"_L1, "Model"_L1})}).section(u',', 0, 0).trimmed();
    const std::optional<double> processors = number(at(system, {"ProcessorSummary"_L1, "Count"_L1}), 1, 1024);
    readings.processors = processors && std::floor(*processors) == *processors ? static_cast<int>(*processors) : 0;
    readings.memoryGib = number(at(system, {"MemorySummary"_L1, "TotalSystemMemoryGiB"_L1}), 0, 1'000'000);
    status.server = readings;

    QStringList can{u"power"_s, u"reset"_s, u"power_state"_s, u"reboot"_s};
    if (light) {
        can.append(u"locator"_s);
    }
    // Start into BIOS setup, where the controller allows a "BiosSetup" boot override (the web's `can`).
    if (bootTargets(system).contains(u"BiosSetup"_s)) {
        can.append(u"bios_setup"_s);
    }
    status.capabilities = can;
    return seen;
}

} // namespace qkvm::core::redfish
