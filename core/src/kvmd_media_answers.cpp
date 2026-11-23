#include <qkvm/core/kvmd_media.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

#include <qkvm/core/kvmd_answers.h>

using namespace Qt::StringLiterals;

namespace qkvm::core::kvmd {

namespace {

// (As kvmd_answers.cpp reads a KVM's answers: a value some way down, a plain yes or no, a number within reason.)
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

bool plainly(const QJsonValue& value)
{
    return flag(value) == std::optional<bool>(true);
}

std::optional<double> number(const QJsonValue& value, double least, double most)
{
    if (!value.isDouble() || !std::isfinite(value.toDouble())) {
        return std::nullopt;
    }
    const double is = value.toDouble();
    return is >= least && is <= most ? std::optional<double>(is) : std::nullopt;
}

// A count of bytes within reason, whole: one with a fraction is none.
std::optional<qint64> bytes(const QJsonValue& value, double most)
{
    const std::optional<double> is = number(value, 0, most);
    return is && std::floor(*is) == *is ? std::optional<qint64>(static_cast<qint64>(*is)) : std::nullopt;
}

// The time kvmd gives a file of its storage (`mod_ts`, seconds since 1970).
std::optional<double> stampOf(const QJsonValue& value)
{
    return number(value, 0, 1e11);
}

constexpr double mostImageBytes = 1125899906842624.0;      // 2^50
constexpr double mostStorageBytes = 1152921504606846976.0; // 2^60

// The id of the image a drive says (its `image`): its name where that is one the program hands back; else namelessId,
// made of all it says of that image (a name it does not hand back, or none, never goes as the name it reads as: review
// cx4-1 M2).
QString idOf(const QJsonValue& image)
{
    const QJsonValue raw = image.isObject() ? image.toObject().value("name"_L1) : QJsonValue();
    if (raw.isString() && isDeviceImage(raw.toString())) {
        return raw.toString();
    }
    const QByteArray said = QJsonDocument(QJsonArray{image}).toJson(QJsonDocument::Compact);
    const QByteArray digest = QCryptographicHash::hash(said, QCryptographicHash::Sha256);
    return namelessId(QString::fromLatin1(digest.toHex().left(16)));
}

} // namespace

QJsonObject msdKept(const QJsonObject& state)
{
    QJsonObject kept;
    for (const QLatin1StringView key : {"enabled"_L1, "online"_L1, "busy"_L1, "drive"_L1, "drive_partition"_L1}) {
        if (state.contains(key)) {
            kept.insert(key, state.value(key));
        }
    }
    if (const QJsonValue storage = state.value("storage"_L1); storage.isObject()) {
        QJsonObject of;
        for (const QLatin1StringView key : {"images"_L1, "parts"_L1}) {
            if (storage.toObject().contains(key)) {
                of.insert(key, storage.toObject().value(key));
            }
        }
        kept.insert(u"storage"_s, of);
    }
    return kept;
}

std::optional<QJsonObject> msdMerged(const std::optional<QJsonObject>& before, const QJsonObject& event)
{
    if (event.contains("online"_L1) || !before) {
        if (!event.contains("online"_L1)) {
            return std::nullopt;
        }
        return msdKept(event);
    }
    const QJsonObject said = msdKept(event);
    QJsonObject merged = *before;
    for (auto one = said.constBegin(); one != said.constEnd(); ++one) {
        if (one.key() != "storage"_L1) {
            merged.insert(one.key(), one.value());
        }
    }
    if (said.contains("storage"_L1)) {
        QJsonObject storage = merged.value("storage"_L1).toObject();
        const QJsonObject came = said.value("storage"_L1).toObject();
        for (auto one = came.constBegin(); one != came.constEnd(); ++one) {
            storage.insert(one.key(), one.value());
        }
        merged.insert(u"storage"_s, storage);
    }
    return merged;
}

std::optional<QJsonObject> msdHeard(const QByteArray& message)
{
    if (message.size() > mostMsdWord || !message.contains("\"msd\"")) {
        return std::nullopt;
    }
    const QJsonDocument read = QJsonDocument::fromJson(message);
    if (!read.isObject() || read.object().value("event_type"_L1) != QJsonValue(u"msd"_s) ||
        !read.object().value("event"_L1).isObject()) {
        return std::nullopt;
    }
    return read.object().value("event"_L1).toObject();
}

bool isDeviceImage(const QString& name)
{
    static const QRegularExpression rule(
        uR"(\A[A-Za-z0-9_+()\[\]~-](?:[A-Za-z0-9 _.,+()\[\]~-]{0,198}[A-Za-z0-9_+()\[\]~-])?(?:/[A-Za-z0-9_+()\[\]~-](?:[A-Za-z0-9 _.,+()\[\]~-]{0,198}[A-Za-z0-9_+()\[\]~-])?){0,3}\z)"_s);
    return rule.match(name).hasMatch();
}

MediaNotes& MediaNotes::program()
{
    static MediaNotes notes;
    return notes;
}

void MediaNotes::carried(const QString& kvm, const QString& image, const QString& sha256, qint64 size,
                         std::optional<double> stamp)
{
    m_kept.insert(kvm + QChar(0) + image, Kept{sha256, size, stamp, ++m_order});
    while (m_kept.size() > most) {
        // (The oldest goes.)
        const auto oldest = std::ranges::min_element(m_kept, {}, &Kept::order);
        m_kept.erase(oldest);
    }
}

QString MediaNotes::hashOf(const QString& kvm, const QString& image, std::optional<qint64> size,
                           std::optional<double> stamp) const
{
    const auto found = m_kept.constFind(kvm + QChar(0) + image);
    if (found == m_kept.constEnd() || std::optional<qint64>(found->size) != size || found->stamp != stamp) {
        return {};
    }
    return found->sha256;
}

void MediaNotes::forget(const QString& kvm, const QString& image)
{
    m_kept.remove(kvm + QChar(0) + image);
}

Listed imagesOf(const QJsonObject& state, const MediaNotes& notes, const QString& kvm)
{
    Listed listed;
    const QJsonValue found = at(state, {"storage"_L1, "images"_L1});
    // (No list of its images at all, an image of a name it hands back in no image's shape, or one beyond the most: not
    // read whole, and what is not found in it is not known to be gone. Outside review 4.6.)
    listed.whole = found.isObject();
    const QJsonObject images = found.toObject();
    for (auto one = images.constBegin(); one != images.constEnd(); ++one) {
        const QString name = one.key();
        if (isDeviceImage(name) && (!one.value().isObject() || listed.images.size() >= mostMediaImages)) {
            listed.whole = false;
        }
        if (!isDeviceImage(name) || !one.value().isObject() || listed.images.size() >= mostMediaImages) {
            ++listed.leftOut;
            continue;
        }
        const QJsonObject about = one.value().toObject();
        MediaImage image;
        image.id = name;
        image.name = name;
        image.size = bytes(about.value("size"_L1), mostImageBytes);
        // (Whole only where it plainly says so: one that does not say is no image to mount, and its SHA-256 is not
        // said. Outside review 4.7; the web takes it for whole.)
        image.partial = flag(about.value("complete"_L1)) != std::optional<bool>(true);
        if (!image.partial) {
            image.sha256 = notes.hashOf(kvm, name, image.size, stampOf(about.value("mod_ts"_L1)));
        }
        listed.images.append(image);
    }
    std::ranges::sort(listed.images, [](const MediaImage& one, const MediaImage& other) {
        return one.name.compare(other.name, Qt::CaseInsensitive) < 0;
    });
    return listed;
}

QString notReadWholeNote(QString said)
{
    if (said.endsWith(u'.')) {
        said.chop(1);
    }
    return said;
}

std::optional<bool> completeOf(const QJsonObject& state, const QString& image)
{
    return flag(at(state, {"storage"_L1, "images"_L1}).toObject().value(image).toObject().value("complete"_L1));
}

base::Error notReadWhole(const QString& notKnown)
{
    return {"bad_response", u"The KVM's list of its own storage could not be read whole (it is in no shape qKVM "
                            u"reads, or longer than the %1 images qKVM reads), so %2: look at it on its own page."_s
                                .arg(withCommas(mostMediaImages), notKnown)
                                .toStdString()};
}

base::Error switchedOff()
{
    return {"no_virtual_media", "The KVM's own drive is switched off in its own settings"};
}

std::optional<base::Error> standsAgainst(const QJsonObject& state)
{
    if (!plainly(state.value("enabled"_L1))) {
        return switchedOff();
    }
    if (!plainly(state.value("online"_L1))) {
        return base::Error{"action_failed",
                           "The KVM says its drive is not there just now (offline): look at it on its own page"};
    }
    if (plainly(state.value("busy"_L1))) {
        return base::Error{"busy", "The KVM's drive is busy with something else (an upload, or its own page): try "
                                   "again in a moment"};
    }
    if (plainly(at(state, {"drive_partition"_L1, "connected"_L1}))) {
        // (GL.iNet's File Sharing: the whole image storage open to the machine, read-write. Nothing is written beside
        // it.)
        return base::Error{"action_failed",
                           "The KVM's File Sharing is on: its whole image storage is open to the machine, read-write. "
                           "Switch it off on the KVM's own page first."};
    }
    return std::nullopt;
}

MountedMedia unreadOf(const QString& said)
{
    MountedMedia unread;
    unread.drive = QString();
    unread.image = QString(unreadDrive);
    unread.name = said;
    unread.readOnly = false;
    return unread;
}

QString namelessId(const QString& digest)
{
    return u"(name not read: "_s + digest + u")/"_s;
}

bool isNameless(const QString& id)
{
    return id.startsWith("(name not read: "_L1) && id.endsWith(")/"_L1);
}

std::optional<QString> chosenIdOf(const QJsonObject& state)
{
    const QJsonValue drive = state.value("drive"_L1);
    if (!drive.isObject()) {
        return std::nullopt;
    }
    const QJsonValue image = drive.toObject().value("image"_L1);
    if (image.isNull() || image.isUndefined()) {
        return std::nullopt;
    }
    return idOf(image);
}

bool saysNoName(const MountedMedia& now)
{
    return isNameless(now.image) && now.name == "an image"_L1;
}

QString opensASentence(const MountedMedia& now)
{
    // (Never an image's own name, which goes by itself: one of the storage may be called "an image".)
    const bool ours = now.image != now.name && (now.name == "an image"_L1 || now.name.startsWith("an image from "_L1) ||
                                                now.name.startsWith("what is in its drive"_L1));
    return ours ? QString(now.name.front().toUpper()) + now.name.mid(1) : now.name;
}

base::Error driveNotRead()
{
    return {"bad_response", "The KVM does not say what is in its drive in a shape qKVM reads: look at it on its own "
                            "page."};
}

std::optional<MountedMedia> mountedOf(const QJsonObject& state, const MediaNotes& notes, const QString& kvm)
{
    const QJsonValue drive = state.value("drive"_L1);
    if (!plainly(state.value("enabled"_L1))) {
        return std::nullopt;
    }
    // (None at all: what kvmd says of a drive that is offline.)
    if (drive.isNull() || drive.isUndefined()) {
        return std::nullopt;
    }
    // A drive in no shape it reads, or one that says neither yes nor no of being connected: not an empty drive, but
    // something that cannot be read (outside review 4.7).
    if (!drive.isObject() || !flag(drive.toObject().value("connected"_L1))) {
        return unreadOf(u"what is in its drive, which the KVM does not say in a shape qKVM reads"_s);
    }
    if (!plainly(drive.toObject().value("connected"_L1))) {
        return std::nullopt;
    }
    const QJsonValue image = drive.toObject().value("image"_L1);
    const QJsonValue raw = image.isObject() ? image.toObject().value("name"_L1) : QJsonValue();
    const bool named = raw.isString() && isDeviceImage(raw.toString());
    QString name = named ? raw.toString() : plain(raw, 200);
    if (name.isEmpty()) {
        name = u"an image"_s;
    }
    MountedMedia mounted;
    const bool cd = plainly(drive.toObject().value("cdrom"_L1));
    mounted.drive = cd ? u"cd"_s : u"flash"_s;
    mounted.image = idOf(image);
    mounted.name = name;
    // (Read-only where it is a CD-ROM drive or plainly not writable: one that does not say is taken to be writable.
    // Outside review 4.7; the web takes it for read-only.)
    mounted.readOnly = cd || flag(drive.toObject().value("rw"_L1)) == std::optional<bool>(false);
    if (named) {
        mounted.sha256 =
            notes.hashOf(kvm, name, bytes(at(image, {"size"_L1}), mostImageBytes), stampOf(at(image, {"mod_ts"_L1})));
    }
    return mounted;
}

std::optional<Chosen> chosenOf(const QJsonObject& state)
{
    const QJsonValue drive = state.value("drive"_L1);
    if (!plainly(state.value("enabled"_L1)) || !drive.isObject()) {
        return std::nullopt;
    }
    const QJsonObject of = drive.toObject();
    const QJsonValue name = at(of.value("image"_L1), {"name"_L1});
    const std::optional<bool> connected = flag(of.value("connected"_L1));
    if (!name.isString() || !connected) {
        return std::nullopt;
    }
    Chosen chosen;
    chosen.image = name.toString();
    const bool cd = plainly(of.value("cdrom"_L1));
    chosen.drive = cd ? u"cd"_s : u"flash"_s;
    chosen.readOnly = cd || flag(of.value("rw"_L1)) == std::optional<bool>(false);
    chosen.connected = *connected;
    return chosen;
}

std::optional<MediaOffer> offerOf(const std::optional<QJsonObject>& state, bool board)
{
    if (!state || !plainly(state->value("enabled"_L1))) {
        return std::nullopt;
    }
    MediaOffer offer;
    offer.drives = {u"cd"_s, u"flash"_s};
    offer.restart = board;
    offer.upload = true;
    offer.where = u"device"_s;
    offer.writable = {u"flash"_s};
    offer.canDelete = true;
    return offer;
}

std::optional<qint64> freeOf(const QJsonObject& state)
{
    return bytes(at(state, {"storage"_L1, "parts"_L1}).toObject().value(QString()).toObject().value("free"_L1),
                 mostStorageBytes);
}

std::optional<qint64> sizeOf(const QJsonObject& state)
{
    return bytes(at(state, {"storage"_L1, "parts"_L1}).toObject().value(QString()).toObject().value("size"_L1),
                 mostStorageBytes);
}

QString withCommas(qint64 bytes)
{
    QString digits = QString::number(bytes < 0 ? -bytes : bytes);
    for (qsizetype at = digits.size() - 3; at > 0; at -= 3) {
        digits.insert(at, u',');
    }
    return bytes < 0 ? u'-' + digits : digits;
}

std::optional<qint64> writtenOf(const QByteArray& answer)
{
    const QJsonObject said =
        QJsonDocument::fromJson(answer.size() <= largestReplyBytes ? answer : QByteArray()).object();
    return bytes(at(said, {"result"_L1, "image"_L1, "written"_L1}), mostStorageBytes);
}

QString errorOf(const QByteArray& answer)
{
    const QJsonObject said =
        QJsonDocument::fromJson(answer.size() <= largestReplyBytes ? answer : QByteArray()).object();
    return plain(at(said, {"result"_L1, "error_msg"_L1}), 200);
}

std::optional<double> stampOf(const QJsonObject& state, const QString& image)
{
    return stampOf(at(state, {"storage"_L1, "images"_L1}).toObject().value(image).toObject().value("mod_ts"_L1));
}

base::Error msdRefusal(int httpStatus, const QByteArray& answer, const QString& what)
{
    const QJsonObject said =
        QJsonDocument::fromJson(answer.size() <= largestReplyBytes ? answer : QByteArray()).object();
    const QString detail = plain(at(said, {"result"_L1, "error_msg"_L1}), 200);
    if (httpStatus == 409) {
        return {"busy", u"The KVM's drive is busy with something else: %1"_s
                            .arg(detail.isEmpty() ? u"try again in a moment"_s : detail)
                            .toStdString()};
    }
    return {"action_failed", (detail.isEmpty() ? u"The KVM did not %1 (HTTP %2)"_s.arg(what).arg(httpStatus)
                                               : u"The KVM did not %1: %2"_s.arg(what, detail))
                                 .toStdString()};
}

} // namespace qkvm::core::kvmd
