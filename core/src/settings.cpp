#include <qkvm/core/settings.h>

#include <qkvm/core/screen_watch.h>

#include <qkvm/core/exchange.h>
#include <qkvm/core/kvm_probe.h>
#include <qkvm/core/own_actions.h>
#include <qkvm/core/server_client.h>
#include <qkvm/core/server_types.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iterator>
#include <optional>
#include <utility>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QUrl>

using namespace Qt::StringLiterals;

namespace qkvm::core {

namespace {

// More than this is not a file this program wrote. (The largest it writes is a wall of as many
// KVMs as a wall holds, each with the longest of everything, in letters that take three bytes
// each, and with the longest certificate a file of the wall holds, kept beside each: a little
// over two megabytes without the certificates, and some forty-three with them. As many saved
// actions as are kept, each with the longest name, are a quarter of a megabyte more.)
constexpr qint64 largestFile = qint64{64} * 1024 * 1024;
// As many files as this are put aside, and no more: a PC on which every start finds an unreadable
// file has another trouble, and is not filled with them.
constexpr int mostPutAside = 20;
constexpr qsizetype longestAddress = 2048;
constexpr qsizetype mostServers = 200;
constexpr qsizetype longestName = 300;
// A window is somewhere on a desktop of many large monitors, and of a size one can see and use.
constexpr double farthest = 100'000;
constexpr double smallestWindow = 100;
constexpr double largestWindow = 32'000;

QList<MachineReference> referencesFrom(const QJsonValue& value, qsizetype limit)
{
    static const QRegularExpression scope(u"\\A(?:pc|server-[0-9a-f]{64})\\z"_s);
    QList<MachineReference> result;
    const auto list = value.toArray();
    for (qsizetype at = 0; at < list.size() && at < limit; ++at) {
        const auto object = list.at(at).toObject();
        MachineReference reference{object["scope"_L1].toString(), object["machine"_L1].toString()};
        if (reference.scope.size() > 71 || !scope.match(reference.scope).hasMatch() || reference.machine.isEmpty() ||
            reference.machine.size() > 512 || !reference.machine.isValidUtf16() || reference.machine == "@source"_L1)
            continue;
        if (!result.contains(reference))
            result.append(reference);
    }
    return result;
}

QJsonArray writtenReferences(const QList<MachineReference>& references, qsizetype limit)
{
    QJsonArray result;
    for (qsizetype at = 0; at < references.size() && at < limit; ++at)
        result.append(QJsonObject{{u"scope"_s, references.at(at).scope}, {u"machine"_s, references.at(at).machine}});
    return result;
}

// The sizes the machines were last shown at large (Remembered::pictureScales): by the same references as
// the favorites, and only a scale the page offers; once for each machine. Of the file's list, the first
// `mostPictureScales` entries are looked at, and no entry behind them.
QList<MachineScale> scalesFrom(const QJsonValue& value)
{
    static const QStringList offered{u"50"_s, u"75"_s, u"100"_s, u"125"_s, u"140"_s, u"200"_s};
    QList<MachineScale> result;
    const auto list = value.toArray();
    for (qsizetype at = 0; at < list.size() && at < Remembered::mostPictureScales; ++at) {
        const QJsonObject object = list.at(at).toObject();
        const QList<MachineReference> one = referencesFrom(QJsonArray{object}, 1);
        const QJsonValue scale = object["scale"_L1];
        if (one.isEmpty() || !scale.isString() || !offered.contains(scale.toString()) ||
            std::ranges::any_of(result, [&](const MachineScale& kept) { return kept.machine == one.front(); }))
            continue;
        result.append({one.front(), scale.toString()});
    }
    return result;
}

QJsonArray writtenScales(const QList<MachineScale>& scales)
{
    QJsonArray result;
    for (qsizetype at = 0; at < scales.size() && at < Remembered::mostPictureScales; ++at)
        result.append(QJsonObject{{u"scope"_s, scales.at(at).machine.scope},
                                  {u"machine"_s, scales.at(at).machine.machine},
                                  {u"scale"_s, scales.at(at).scale}});
    return result;
}

// A whole number between two others, or nothing.
std::optional<int> whole(const QJsonValue& said, double least, double most)
{
    const double number = said.toDouble();
    if (!said.isDouble() || number < least || number > most || number != std::floor(number)) {
        return std::nullopt;
    }
    return static_cast<int>(number);
}

// All of it or none: half a place is no place.
std::optional<WindowPlace> placeFrom(const QJsonValue& said)
{
    const QJsonObject kept = said.toObject();
    const std::optional<int> x = whole(kept["x"_L1], -farthest, farthest);
    const std::optional<int> y = whole(kept["y"_L1], -farthest, farthest);
    const std::optional<int> width = whole(kept["width"_L1], smallestWindow, largestWindow);
    const std::optional<int> height = whole(kept["height"_L1], smallestWindow, largestWindow);
    if (!x || !y || !width || !height) {
        return std::nullopt;
    }
    WindowPlace place;
    place.x = *x;
    place.y = *y;
    place.width = *width;
    place.height = *height;
    place.maximized = kept["maximized"_L1].toBool();
    place.fullScreen = kept["full_screen"_L1].toBool();
    return place;
}

// Of how a window's wall was shown, only what can be so.
WallWindow windowFrom(const QJsonValue& said)
{
    const QJsonObject kept = said.toObject();
    WallWindow window;
    const auto source = kept["source"_L1].toString();
    if (source == "pc"_L1 || source == "connect"_L1) {
        window.source = source;
    } else if (!source.isEmpty()) {
        const auto address = source.size() <= longestAddress ? ServerClient::addressFrom(source)
                                                             : base::Result<QUrl>(base::Error{"address", "Too long"});
        window.source = address && address.value().toString() == source ? source : u"connect"_s;
    }
    window.place = placeFrom(kept["place"_L1]);
    if (const QString group = kept["group"_L1].toString(); group.size() <= longestName) {
        window.group = group;
    }
    if (const QString order = kept["order"_L1].toString();
        order == "name"_L1 || order == "group"_L1 || order == "state"_L1) {
        window.order = order;
    }
    if (const QJsonValue columns = kept["columns"_L1];
        columns.isDouble() && columns.toDouble() >= 0 && columns.toDouble() <= 6) {
        window.columns = columns.toInt();
    }
    window.rotates = kept["rotate"_L1].toBool();
    return window;
}

QJsonObject written(const WallWindow& window)
{
    QJsonObject said{{u"group"_s, window.group},
                     {u"order"_s, window.order},
                     {u"columns"_s, window.columns},
                     {u"rotate"_s, window.rotates}};
    if (!window.source.isEmpty())
        said.insert(u"source"_s, window.source);
    if (window.place) {
        said.insert(u"place"_s, QJsonObject{{u"x"_s, window.place->x},
                                            {u"y"_s, window.place->y},
                                            {u"width"_s, window.place->width},
                                            {u"height"_s, window.place->height},
                                            {u"maximized"_s, window.place->maximized},
                                            {u"full_screen"_s, window.place->fullScreen}});
    }
    return said;
}

QList<Workspace> workspacesFrom(const QJsonValue& value)
{
    static const QRegularExpression id(u"\\A[0-9a-f]{32}\\z"_s);
    QList<Workspace> result;
    QSet<QString> ids;
    QSet<QString> names;
    const auto list = value.toArray();
    for (qsizetype at = 0; at < list.size() && at < Remembered::mostWorkspaces; ++at) {
        const auto object = list.at(at).toObject();
        Workspace workspace;
        workspace.id = object["id"_L1].toString();
        workspace.name = object["name"_L1].toString().simplified();
        const auto windows = object["windows"_L1].toArray();
        if (workspace.id.size() != 32 || !id.match(workspace.id).hasMatch() || ids.contains(workspace.id) ||
            workspace.name.isEmpty() || workspace.name.size() > Remembered::longestWorkspaceName ||
            !workspace.name.isValidUtf16() || names.contains(workspace.name.toCaseFolded()) || windows.isEmpty() ||
            windows.size() > Remembered::mostWindows)
            continue;
        for (const auto& window : windows)
            workspace.windows.append(windowFrom(window));
        ids.insert(workspace.id);
        names.insert(workspace.name.toCaseFolded());
        result.append(workspace);
    }
    return result;
}

// The longest a KVM's name, group and user name are, as the server has them.
constexpr qsizetype longestKvmName = 80;
constexpr qsizetype longestKvmGroup = 40;
constexpr qsizetype longestKvmUser = 64;

// What an entry of the file's list of KVMs is: a KVM of this PC's own (`kvm`); none where it is
// none this program wrote, without an id of its shape or without an address that is one; or one of
// its shape that says no family this build speaks to (`ofAnotherFamily`): written by another build of
// the program, which this one does not read, and keeps as it stands (Remembered::kvmsItDoesNotRead).
struct KvmEntry {
    std::optional<OwnKvm> kvm;
    bool ofAnotherFamily = false;
};

KvmEntry kvmFrom(const QJsonValue& said)
{
    static const QRegularExpression idShape(u"\\A[0-9a-f]{8}\\z"_s);
    const QJsonObject is = said.toObject();
    OwnKvm kvm;
    kvm.id = is["id"_L1].toString();
    kvm.address = is["address"_L1].toString();
    const QUrl address(kvm.address, QUrl::StrictMode);
    // An address as the program writes one, and in no other way: one KVM has one address, and
    // whether a KVM "is on the wall already" is asked by it (kvm::addressFrom). One that says
    // the same otherwise (the port every such address has anyway, a path, capitals) is none the
    // program wrote.
    const base::Result<QUrl> asWritten = kvm::addressFrom(kvm.address);
    const bool isAddress = address.isValid() && !address.host().isEmpty() && address.userInfo().isEmpty() &&
                           (address.scheme() == "https"_L1 || address.scheme() == "http"_L1) && asWritten &&
                           asWritten.value().toString(QUrl::FullyEncoded) == kvm.address;
    if (!idShape.match(kvm.id).hasMatch() || !isAddress) {
        return {};
    }
    // Its family, in so many words: of none, or of one this build does not speak to by itself, it is
    // a KVM this build does not read.
    if (const QJsonValue kind = is["kind"_L1]; kind.isString() && OwnKvm::namesAFamily(kind.toString())) {
        kvm.kind = kind.toString();
    } else {
        return {std::nullopt, true};
    }
    kvm.name = is["name"_L1].toString().left(longestKvmName);
    kvm.group = is["group"_L1].toString().left(longestKvmGroup);
    if (const QString user = is["user"_L1].toString().left(longestKvmUser); !user.isEmpty()) {
        kvm.user = user;
    }
    kvm.knownByCertificate = is["known_by_certificate"_L1].toBool();
    // Whatever stands there, as it stands: a certificate that was cut short is not "none", and
    // nothing is trusted in its place (the driver says so).
    kvm.certificate = QByteArray::fromHex(is["certificate"_L1].toString().toLatin1());
    // Asked what it is only where the file says so in so many words. A text, a number and
    // nothing at all are "not asked yet": the wall then asks once more, which sends nothing
    // that says who asks. (Read as Qt reads a flag, "asked": "no" would be a KVM that was asked.)
    const QJsonValue asked = is["asked"_L1];
    kvm.askedWhatItIs = asked.isBool() && asked.toBool();
    // The number it gives for itself, where it is one a driver gives; anything else there is none.
    if (const QString number = is["number"_L1].toString(); exchange::isADevicesOwnNumber(number)) {
        kvm.number = number;
    }
    // The mark its server is kept by (a server's controller), as a driver gives one, or none. One that stands there
    // and is of no such shape is no "none" (its driver would take whatever server answers first): an entry this
    // build does not read, kept as it stands.
    if (const QJsonValue machine = is["machine"_L1]; !machine.isUndefined()) {
        const QString mark = machine.toString();
        if (!machine.isString() || (!mark.isEmpty() && !exchange::isADevicesOwnNumber(mark))) {
            return {std::nullopt, true};
        }
        kvm.machine = mark;
    }
    // Kept beside its twin, where the file says so as a flag does.
    const QJsonValue beside = is["kept_beside_twin"_L1];
    kvm.keptBesideItsTwin = beside.isBool() && beside.toBool();
    // Its screen watches: what is no whole number above 0 is none, a longer one a day.
    kvm.noSignalMinutes = ScreenWatch::minutesFrom(is["no_signal_minutes"_L1].toInteger());
    kvm.unchangedMinutes = ScreenWatch::minutesFrom(is["unchanged_minutes"_L1].toInteger());
    return {kvm, false};
}

// The certificate itself that stands with a KVM in the file, where it is one to believe: a
// certificate as a file of the wall holds one (exchange.h: one certificate, no longer than a
// file holds, and one to Windows), of a KVM that is known by a certificate over HTTPS, with the
// very fingerprint that KVM is known by. Nothing otherwise: a damaged one, one that is too
// long and one that is another's are as none, and the fingerprint stays what the KVM is known by.
QString certificateWith(const QJsonValue& said, const OwnKvm& kvm)
{
    constexpr qsizetype sha256Bytes = 32;
    if (!said.isString() || !kvm.knownByCertificate || kvm.certificate.size() != sha256Bytes ||
        !kvm.address.startsWith("https://"_L1)) {
        return {};
    }
    const QString written = said.toString();
    if (written.isEmpty() || written.size() > exchange::longestCertificate) {
        return {};
    }
    const std::optional<exchange::KeptCertificate> kept = exchange::certificateFrom(written);
    return kept && kept->sha256 == kvm.certificate ? kept->pem : QString();
}

QJsonObject written(const OwnKvm& kvm)
{
    QJsonObject said{{u"id"_s, kvm.id},
                     {u"kind"_s, kvm.kind},
                     {u"name"_s, kvm.name},
                     {u"address"_s, kvm.address},
                     {u"user"_s, kvm.user},
                     {u"group"_s, kvm.group},
                     {u"known_by_certificate"_s, kvm.knownByCertificate},
                     {u"certificate"_s, QString::fromLatin1(kvm.certificate.toHex())},
                     {u"asked"_s, kvm.askedWhatItIs},
                     {u"number"_s, kvm.number},
                     {u"machine"_s, kvm.machine}};
    if (kvm.noSignalMinutes > 0) {
        said.insert(u"no_signal_minutes"_s, kvm.noSignalMinutes);
    }
    if (kvm.unchangedMinutes > 0) {
        said.insert(u"unchanged_minutes"_s, kvm.unchangedMinutes);
    }
    return said;
}

// A saved action of this PC's own from the file; nothing where it is none this program wrote.
// Every field has to be there as the program writes it (a kind that is one of the two, flags
// that are flags, a count of edits that is a whole number), and the action has to hold by the
// rules one is saved by (action::isOne): there is no older shape of this to read.
std::optional<OwnAction> actionFrom(const QJsonValue& said)
{
    const QJsonObject is = said.toObject();
    const QString kind = is["kind"_L1].toString();
    const QJsonValue enter = is["enter"_L1];
    const QJsonValue danger = is["danger"_L1];
    const std::optional<int> changed = whole(is["changed"_L1], 0, action::mostEdits);
    // (Read as Qt reads a flag, "danger": "no" would be an action that asks, and one that says
    // nothing of it would be one that does not.)
    if ((kind != "keys"_L1 && kind != "text"_L1) || !enter.isBool() || !danger.isBool() || !changed) {
        return std::nullopt;
    }
    OwnAction action;
    action.id = is["id"_L1].toString();
    action.name = is["name"_L1].toString();
    action.types = kind == "text"_L1;
    action.keys = is["keys"_L1].toString();
    action.enter = enter.toBool();
    action.danger = danger.toBool();
    action.changed = *changed;
    // Which writing of its text one that types has. What is not written as the program writes
    // that names none: the action is there, without a text, and says so when it is run. (Of one
    // that presses nothing is read here: it has no text.)
    if (const QString writing = is["writing"_L1].toString(); action.types && action::namesAWriting(writing)) {
        action.writing = writing;
    }
    return action::isOne(action) ? std::optional<OwnAction>(action) : std::nullopt;
}

QJsonObject written(const OwnAction& action)
{
    return {{u"id"_s, action.id},
            {u"name"_s, action.name},
            {u"kind"_s, action.types ? u"text"_s : u"keys"_s},
            {u"keys"_s, action.keys},
            {u"enter"_s, action.enter},
            {u"danger"_s, action.danger},
            {u"changed"_s, action.changed},
            {u"writing"_s, action.writing}};
}

// What was read: nothing where there is no file; and of a file that could not be read, or is
// not what this program writes, nothing but that it is so.
std::optional<Remembered> readFrom(const QString& file, Settings::Patience patience)
{
    // (What is there and is no file, a folder of that name, is not one that could not be read:
    // nothing is lost with it, and it is in the way of the writing, which says so.)
    if (!QFileInfo(file).isFile()) {
        return Remembered{};
    }
    QFile kept(file);
    bool open = kept.open(QIODevice::ReadOnly);
    for (int tries = 1; tries < patience.tries && !open; ++tries) {
        QThread::msleep(static_cast<unsigned long>(std::max(0, patience.betweenMs)));
        open = kept.open(QIODevice::ReadOnly);
    }
    if (!open || kept.size() > largestFile) {
        return std::nullopt;
    }
    const QJsonDocument document = QJsonDocument::fromJson(kept.readAll());
    if (!document.isObject()) {
        return std::nullopt;
    }
    const QJsonObject said = document.object();
    Remembered remembered;
    remembered.favorites = referencesFrom(said["favorites"_L1], Remembered::mostFavorites);
    remembered.recentMachines = referencesFrom(said["recent_machines"_L1], Remembered::mostRecentMachines);
    remembered.pictureScales = scalesFrom(said["picture_scales"_L1]);
    remembered.workspaces = workspacesFrom(said["workspaces"_L1]);
    if (const QJsonValue server = said["server"_L1]; server.isString() && server.toString().size() <= longestAddress) {
        remembered.server = server.toString();
    }
    const auto servers = said["servers"_L1].toArray();
    for (qsizetype at = 0; at < servers.size() && at < Remembered::mostMonitoredServers; ++at) {
        const auto value = servers.at(at);
        if (!value.isString() || value.toString().size() > longestAddress)
            continue;
        if (const auto address = ServerClient::addressFrom(value.toString()); address) {
            const QString canonical = address.value().toString();
            if (!remembered.servers.contains(canonical))
                remembered.servers.append(canonical);
        }
    }
    // Of each list, as many entries are looked at as this program writes into it at the most,
    // whatever they are, and none behind those: the looking stops at a number that is said, and
    // not at the end of a list of millions of entries of which none is taken. (A limit that
    // counted only what was taken went through such a list whole.)
    const QJsonArray encrypted = said["reached_encrypted"_L1].toArray();
    for (qsizetype at = 0; at < encrypted.size() && at < mostServers; ++at) {
        const QJsonValue name = encrypted.at(at);
        if (name.isString() && !name.toString().isEmpty() && name.toString().size() <= longestName) {
            remembered.reachedEncrypted.append(name.toString());
        }
    }
    // Every window that is written there, up to as many as the program opens: one that says
    // nothing the program can use is a window as a new one is, and is a window all the same.
    const QJsonArray windows = said["windows"_L1].toArray();
    for (const auto& window : windows) {
        if (remembered.windows.size() >= Remembered::mostWindows) {
            break;
        }
        remembered.windows.append(windowFrom(window));
    }
    if (const QJsonValue every = said["picture_every_ms"_L1];
        every.isDouble() && every.toDouble() >= 100 && every.toDouble() <= 600'000) {
        remembered.wallPictureEveryMs = every.toInt();
    }
    if (const QString theme = said["theme"_L1].toString();
        theme == "dark"_L1 || theme == "light"_L1 || theme == "system"_L1) {
        remembered.theme = theme;
    }
    remembered.staysInTray = said["stay_in_tray"_L1].toBool();
    // Off only where it says so.
    remembered.tellsNews = said["tell_news"_L1].toBool(true);
    if (const QString key = said["release_key"_L1].toString(); Remembered::namesAReleaseKey(key)) {
        remembered.releaseKey = key;
    }
    if (const QString word = said["hardware_decoding"_L1].toString(); Remembered::namesAHardwareDecoding(word)) {
        remembered.hardwareDecoding = word;
    }
    if (const QString word = said["graphics_card"_L1].toString(); Remembered::namesAGraphicsCard(word)) {
        remembered.graphicsCard = word;
    }
    remembered.checkUpdates = said["check_updates"_L1].toBool(true);
    remembered.lastUpdateCheck = QDateTime::fromString(said["last_update_check"_L1].toString(), Qt::ISODate);
    remembered.showsOwnWall = said["own_wall"_L1].toBool(false);
    if (const QJsonValue section = said["settings_section"_L1];
        section.isString() && Remembered::namesASettingsSection(section.toString())) {
        remembered.settingsSection = section.toString();
    }
    // (The first `mostKvms` entries, KVMs or not: settings.h says so where the list is.)
    const QJsonArray kvms = said["kvms"_L1].toArray();
    QSet<QString> ids;
    for (qsizetype at = 0; at < kvms.size() && at < Remembered::mostKvms; ++at) {
        const QJsonValue one = kvms.at(at);
        const KvmEntry entry = kvmFrom(one);
        if (entry.ofAnotherFamily) {
            // Kept as it stands, and once by its id, as the wall's own are.
            if (const QString id = one.toObject()["id"_L1].toString(); !ids.contains(id)) {
                ids.insert(id);
                remembered.kvmsItDoesNotRead.append(one);
            }
            continue;
        }
        if (const std::optional<OwnKvm>& kvm = entry.kvm; kvm && !ids.contains(kvm->id)) {
            ids.insert(kvm->id);
            remembered.kvms.append(*kvm);
            if (const QString pem = certificateWith(one.toObject()["certificate_pem"_L1], *kvm); !pem.isEmpty()) {
                remembered.kvmCertificates.insert(kvm->id, pem);
            }
            // The wall's mark that a password is kept for it: only where the file says so as a
            // flag does. (Read as Qt reads a flag, "yes" would be one.)
            if (const QJsonValue marked = one.toObject()["secret_kept"_L1]; marked.isBool() && marked.toBool()) {
                remembered.kvmPasswordsKept.insert(kvm->id);
            }
            // That the wall followed it onto HTTPS, and nobody has seen that yet: only where it says so of the
            // address the KVM has.
            if (const QJsonValue followed = one.toObject()["followed_onto_https"_L1];
                followed.isString() && followed.toString() == kvm->address) {
                remembered.kvmsFollowedOntoHttps.insert(kvm->id, kvm->address);
            }
            // That the wall holds it back for a twin, and which: only of a JetKVM over plain HTTP (and, once the whole
            // list is read, only for another KVM of it: the one it is held for may come after it).
            if (const QJsonValue twin = one.toObject()["held_for_twin"_L1];
                twin.isString() && kvm->kind == "jetkvm"_L1 && kvm->address.startsWith("http://"_L1)) {
                remembered.kvmsHeldForATwin.insert(kvm->id, twin.toString());
            }
        }
    }
    for (auto held = remembered.kvmsHeldForATwin.begin(); held != remembered.kvmsHeldForATwin.end();) {
        const QString& twin = held.value();
        const bool another = twin != held.key() && std::ranges::any_of(remembered.kvms, [&twin](const OwnKvm& kvm) {
                                 return kvm.id == twin;
                             });
        held = another ? std::next(held) : remembered.kvmsHeldForATwin.erase(held);
    }
    // (The first `mostSavedActions` entries, actions or not, as of the KVMs; and no id twice.)
    const QJsonArray actions = said["actions"_L1].toArray();
    QSet<QString> actionIds;
    for (qsizetype at = 0; at < actions.size() && at < mostSavedActions; ++at) {
        if (const std::optional<OwnAction> action = actionFrom(actions.at(at));
            action && !actionIds.contains(action->id)) {
            actionIds.insert(action->id);
            remembered.actions.append(*action);
        }
    }
    const QJsonObject certificates = said["certificates"_L1].toObject();
    qsizetype lookedAt = 0;
    for (auto known = certificates.begin(); known != certificates.end() && lookedAt < mostServers; ++known) {
        ++lookedAt;
        // A SHA-256 and nothing else: thirty-two bytes, written as sixty-four letters.
        const QByteArray written = known.value().toString().toLatin1();
        const QByteArray sha256 = QByteArray::fromHex(written);
        if (!known.key().isEmpty() && known.key().size() <= longestName && written.size() == 64 &&
            sha256.size() == 32 && sha256.toHex() == written.toLower()) {
            remembered.certificates.insert(known.key(), sha256);
        }
    }
    return remembered;
}

} // namespace

bool OwnKvm::namesAFamily(const QString& kind)
{
    return kind == "kvmd"_L1 || kind == "jetkvm"_L1 || kind == "redfish"_L1;
}

bool Remembered::namesAReleaseKey(const QString& key)
{
    return key == "right-ctrl"_L1 || key == "scroll-lock"_L1 || key == "f12"_L1;
}

bool Remembered::namesAHardwareDecoding(const QString& word)
{
    return word == "automatic"_L1 || word == "on"_L1 || word == "off"_L1;
}

bool Remembered::namesAGraphicsCard(const QString& word)
{
    return word == "most-powerful"_L1 || word == "power-saving"_L1 || word == "windows"_L1;
}

bool Remembered::namesASettingsSection(const QString& key)
{
    return key == "kvms"_L1 || key == "actions"_L1 || key == "files"_L1 || key == "general"_L1 || key == "video"_L1 ||
           key == "alerts"_L1 || key == "security"_L1 || key == "updates"_L1 || key == "about"_L1;
}

Settings::Settings(QString file) : Settings(std::move(file), Patience{}) {}

Settings::Settings(QString file, Patience patience) : m_file(std::move(file))
{
    if (std::optional<Remembered> read = readFrom(m_file, patience)) {
        m_remembered = std::move(*read);
    } else {
        m_wasUnreadable = true;
        m_inTheWay = true;
    }
}

QString Settings::usualFile()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(u"settings.json"_s);
}

void Settings::note(const Remembered& what)
{
    m_remembered = what;
    m_unwritten = true;
}

base::Result<void> Settings::writeDown()
{
    if (!m_unwritten) {
        return {};
    }
    // (Its own copy: what is remembered is what is written over.)
    const Remembered asItIs = m_remembered;
    return remember(asItIs);
}

base::Result<void> Settings::remember(const Remembered& what)
{
    m_remembered = what;
    // Until the file says it: a writing that fails leaves it unwritten, and the next one, or
    // writeDown(), brings it.
    m_unwritten = true;
    QJsonObject certificates;
    for (auto known = what.certificates.begin(); known != what.certificates.end(); ++known) {
        certificates.insert(known.key(), QString::fromLatin1(known.value().toHex()));
    }
    QJsonObject said{
        {u"server"_s, what.server},
        {u"favorites"_s, writtenReferences(what.favorites, Remembered::mostFavorites)},
        {u"recent_machines"_s, writtenReferences(what.recentMachines, Remembered::mostRecentMachines)},
        {u"picture_scales"_s, writtenScales(what.pictureScales)},
        {u"servers"_s, QJsonArray::fromStringList(
                           what.servers.first(std::min(what.servers.size(), Remembered::mostMonitoredServers)))},
        {u"reached_encrypted"_s, QJsonArray::fromStringList(what.reachedEncrypted)},
        {u"certificates"_s, certificates},
        {u"theme"_s, what.theme},
        {u"stay_in_tray"_s, what.staysInTray},
        {u"tell_news"_s, what.tellsNews},
        {u"release_key"_s, Remembered::namesAReleaseKey(what.releaseKey) ? what.releaseKey : u"right-ctrl"_s},
        {u"hardware_decoding"_s,
         Remembered::namesAHardwareDecoding(what.hardwareDecoding) ? what.hardwareDecoding : u"automatic"_s},
        {u"graphics_card"_s,
         Remembered::namesAGraphicsCard(what.graphicsCard) ? what.graphicsCard : u"most-powerful"_s},
        {u"check_updates"_s, what.checkUpdates},
        {u"last_update_check"_s, what.lastUpdateCheck.toUTC().toString(Qt::ISODate)},
        {u"own_wall"_s, what.showsOwnWall},
        {u"picture_every_ms"_s, what.wallPictureEveryMs},
        // (Only a key that names a section: what is remembered may hold anything.)
        {u"settings_section"_s,
         Remembered::namesASettingsSection(what.settingsSection) ? what.settingsSection : QString()},
    };
    QJsonArray windows;
    for (const WallWindow& window : what.windows.first(std::min(what.windows.size(), Remembered::mostWindows))) {
        windows.append(written(window));
    }
    said.insert(u"windows"_s, windows);
    QJsonArray workspaces;
    for (const auto& workspace : what.workspaces.first(std::min(what.workspaces.size(), Remembered::mostWorkspaces))) {
        QJsonArray layout;
        for (const auto& window : workspace.windows.first(std::min(workspace.windows.size(), Remembered::mostWindows)))
            layout.append(written(window));
        workspaces.append(QJsonObject{{u"id"_s, workspace.id}, {u"name"_s, workspace.name}, {u"windows"_s, layout}});
    }
    said.insert(u"workspaces"_s, workspaces);
    // The wall's own, and after them, as they stood, the KVMs this build does not read: no more than
    // mostKvms in all, which is as many as are read back, the kept ones first to be let room (the
    // wall adds none where they would not fit, LocalWall::add). Where they stood among the wall's own
    // is not kept: they come after it.
    QJsonArray kvms;
    const qsizetype room = std::max<qsizetype>(0, Remembered::mostKvms - what.kvmsItDoesNotRead.size());
    const QList<OwnKvm> those = what.kvms.first(std::min(what.kvms.size(), room));
    QSet<QString> writtenIds;
    for (const OwnKvm& kvm : those) {
        writtenIds.insert(kvm.id);
    }
    for (const OwnKvm& kvm : those) {
        QJsonObject one = written(kvm);
        // (The certificate itself stands with its KVM, where one is kept for it; and so does the
        // wall's mark of whether a password is kept for it.)
        if (const QString pem = what.kvmCertificates.value(kvm.id); !pem.isEmpty()) {
            one.insert(u"certificate_pem"_s, pem);
        }
        one.insert(u"secret_kept"_s, what.kvmPasswordsKept.contains(kvm.id));
        if (kvm.keptBesideItsTwin) {
            one.insert(u"kept_beside_twin"_s, true);
        }
        // (And that the wall followed it onto HTTPS, until that was seen: of the address it has, and not of one
        // it had.)
        if (const QString followed = what.kvmsFollowedOntoHttps.value(kvm.id);
            !followed.isEmpty() && followed == kvm.address) {
            one.insert(u"followed_onto_https"_s, followed);
        }
        // (And that the wall holds it back for a twin, and which: only of a JetKVM over plain HTTP, and only for
        // another KVM that is written too.)
        if (const QString twin = what.kvmsHeldForATwin.value(kvm.id);
            !twin.isEmpty() && twin != kvm.id && writtenIds.contains(twin) && kvm.kind == "jetkvm"_L1 &&
            kvm.address.startsWith("http://"_L1)) {
            one.insert(u"held_for_twin"_s, twin);
        }
        kvms.append(one);
    }
    for (const auto& kept : what.kvmsItDoesNotRead) {
        if (kvms.size() >= Remembered::mostKvms) {
            break;
        }
        kvms.append(kept);
    }
    said.insert(u"kvms"_s, kvms);
    QJsonArray actions;
    for (const OwnAction& action : what.actions.first(std::min<qsizetype>(what.actions.size(), mostSavedActions))) {
        actions.append(written(action));
    }
    said.insert(u"actions"_s, actions);
    if (!QDir().mkpath(QFileInfo(m_file).absolutePath())) {
        return base::Error{"settings", "The folder for the program's settings could not be made."};
    }
    if (const base::Result<void> aside = putAsideWhatCouldNotBeRead(); !aside) {
        return aside.error();
    }
    QSaveFile fresh(m_file);
    if (!fresh.open(QIODevice::WriteOnly) || fresh.write(QJsonDocument(said).toJson()) < 0 || !fresh.commit()) {
        return base::Error{"settings",
                           "The program's settings could not be written: " + fresh.errorString().toStdString()};
    }
    m_unwritten = false;
    ++m_writings;
    return {};
}

base::Result<void> Settings::putAsideWhatCouldNotBeRead()
{
    if (!m_inTheWay) {
        return {};
    }
    const QFileInfo was(m_file);
    // (Gone meanwhile, moved by whoever looks after the PC: nothing is in the way any more.)
    if (!was.exists()) {
        m_inTheWay = false;
        return {};
    }
    for (int number = 1; number <= mostPutAside; ++number) {
        const QString name = number == 1
                                 ? u"%1.unreadable.%2"_s.arg(was.completeBaseName(), was.suffix())
                                 : u"%1.unreadable-%3.%2"_s.arg(was.completeBaseName(), was.suffix()).arg(number);
        const QString aside = was.dir().filePath(name);
        if (QFile::exists(aside)) {
            continue;
        }
        // A plain move, which Windows refuses for a file that something holds. (Qt's own would
        // copy the file then, megabytes of it, and try the next name.)
        std::error_code refused;
        std::filesystem::rename(std::filesystem::path(m_file.toStdU16String()),
                                std::filesystem::path(aside.toStdU16String()), refused);
        if (refused) {
            break;
        }
        m_putAside = aside;
        m_inTheWay = false;
        return {};
    }
    return base::Error{"settings", "The program's settings could not be read when it started, and could not be "
                                   "put aside. Nothing is written over them."};
}

WallWindow WindowSlot::remembered() const
{
    return m_settings.remembered().windows.value(m_index);
}

base::Result<void> WindowSlot::remember(const WallWindow& what)
{
    // (One that would be a window more than the program opens is not written down.)
    if (m_index < 0 || m_index >= Remembered::mostWindows || what == remembered()) {
        return {};
    }
    Remembered all = m_settings.remembered();
    // The windows before it that nothing was said of yet are windows as new ones are.
    while (all.windows.size() <= m_index) {
        all.windows.append(WallWindow{});
    }
    all.windows[m_index] = what;
    return m_settings.remember(all);
}

} // namespace qkvm::core
