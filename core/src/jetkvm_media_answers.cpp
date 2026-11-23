#include <qkvm/core/jetkvm_media.h>

#include <algorithm>
#include <cmath>

#include <QCryptographicHash>
#include <QJsonValue>
#include <QMessageAuthenticationCode>
#include <QRandomGenerator>
#include <QRegularExpression>

#include <qkvm/core/kvmd_answers.h>

using namespace Qt::StringLiterals;

namespace qkvm::core::jetkvm {

namespace {

constexpr double mostImageBytes = 1125899906842624.0;      // 2^50
constexpr double mostStorageBytes = 1152921504606846976.0; // 2^60

// A text or a number of the JetKVM's, as one line of `limit` at the most (kvmd::plain, the web's plain); nothing where
// it is neither, or empty.
QString plainOf(const json::Value* value, qsizetype limit)
{
    if (value == nullptr) {
        return {};
    }
    if (value->isText()) {
        return kvmd::plain(QJsonValue(value->text), limit);
    }
    return value->isNumber() ? kvmd::plain(QJsonValue(value->number), limit) : QString();
}

// A count of bytes within reason, whole: one with a fraction is none.
std::optional<qint64> bytesOf(const json::Value* value, double most)
{
    if (value == nullptr || !value->isNumber() || !std::isfinite(value->number) || value->number < 0 ||
        value->number > most || std::floor(value->number) != value->number) {
        return std::nullopt;
    }
    return static_cast<qint64>(value->number);
}

const json::Value* in(const json::Value& object, QStringView name)
{
    return object.isObject() ? object.find(name) : nullptr;
}

bool textIs(const json::Value* value, QLatin1StringView word)
{
    return value != nullptr && value->isText() && value->text == word;
}

} // namespace

bool isOneName(const QString& name)
{
    static const QRegularExpression rule(
        uR"(\A[A-Za-z0-9_+()\[\]~-](?:[A-Za-z0-9 _.,+()\[\]~-]{0,198}[A-Za-z0-9_+()\[\]~-])?\z)"_s);
    return rule.match(name).hasMatch();
}

bool isUploadId(const QString& id)
{
    static const QRegularExpression rule(
        uR"(\Aupload_[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\z)"_s);
    return rule.match(id).hasMatch();
}

std::optional<double> stampOf(const json::Value* createdAt)
{
    const QString said = plainOf(createdAt, 60);
    if (said.isEmpty()) {
        return std::nullopt;
    }
    const QByteArray digest = QCryptographicHash::hash(said.toUtf8(), QCryptographicHash::Sha256);
    quint64 first = 0;
    for (qsizetype at = 0; at < 6; ++at) {
        first = (first << 8) | static_cast<unsigned char>(digest.at(at));
    }
    // (Six bytes: a whole number a double holds as it is.)
    return static_cast<double>(first);
}

Files filesOf(const json::Value& said, const kvmd::MediaNotes& notes, const QString& key)
{
    Files files;
    const json::Value* found = in(said, u"files");
    if (found == nullptr || !found->isList()) {
        files.whole = false;
        return files;
    }
    for (const json::Value& entry : found->list) {
        const json::Value* filename = in(entry, u"filename");
        const QString name = filename != nullptr && filename->isText() ? filename->text : QString();
        const bool partial = name.endsWith(partialEnd);
        const QString base = partial ? name.chopped(partialEnd.size()) : name;
        if (filename == nullptr || !filename->isText() || files.images.size() >= mostMediaImages) {
            // (An entry whose name cannot be read, or one beyond the most, may be any image: not read whole.)
            files.whole = false;
        }
        if (filename == nullptr || !filename->isText() || !isOneName(base) || files.images.size() >= mostMediaImages) {
            ++files.leftOut;
            continue;
        }
        MediaImage image;
        image.id = name;
        image.name = base;
        image.size = bytesOf(in(entry, u"size"), mostImageBytes);
        image.partial = partial;
        const std::optional<double> stamp = stampOf(in(entry, u"createdAt"));
        files.listed.insert(name, Files::Listed{image.size, stamp});
        if (!partial) {
            image.sha256 = notes.hashOf(key, name, image.size, stamp);
        }
        files.images.append(image);
    }
    std::ranges::stable_sort(files.images, [](const MediaImage& one, const MediaImage& other) {
        const int byName = one.name.toCaseFolded().compare(other.name.toCaseFolded());
        return byName != 0 ? byName < 0 : !one.partial && other.partial;
    });
    return files;
}

base::Error driveNotRead()
{
    return {"bad_response", "The JetKVM does not say what is in its drive in a shape qKVM reads: look at it on its own "
                            "page."};
}

base::Error notReadWhole(const QString& notKnown)
{
    return {"bad_response", u"The JetKVM's list of its own storage could not be read whole (it is in no shape qKVM "
                            u"reads, or longer than the %1 entries qKVM reads), so %2: look at it on its own page."_s
                                .arg(kvmd::withCommas(mostMediaImages), notKnown)
                                .toStdString()};
}

std::optional<MountedMedia> mountedOf(const json::Value& state, const QHash<QString, Files::Listed>& listed,
                                      const kvmd::MediaNotes& notes, const QString& key)
{
    // Null: nothing in it. Anything else that is no object is no empty drive, but one that cannot be read (outside
    // review 4.7: it was taken for an empty one).
    if (state.kind == json::Value::Kind::Null) {
        return std::nullopt;
    }
    if (!state.isObject()) {
        return kvmd::unreadOf(u"what is in its drive, which the JetKVM does not say in a shape qKVM reads"_s);
    }
    // From its own storage, from an address or from a browser at its own page (the firmware's three sources); from
    // where else, or from where it does not say, what is in it cannot be read (review cx4-1 M2: any but "Storage"
    // was taken for an image from an address, which never stands against a deletion).
    const json::Value* source = state.find(u"source");
    const bool fromAnAddress = textIs(source, "HTTP"_L1);
    const bool fromABrowser = textIs(source, "WebRTC"_L1);
    if (!fromItsStorage(state) && !fromAnAddress && !fromABrowser) {
        return kvmd::unreadOf(u"what is in its drive, which the JetKVM does not say where it comes from"_s);
    }
    MountedMedia mounted;
    // (A drive of neither kind it says, or none, is neither: no mount of either kind is taken to be it. Outside
    // review 4.7: it was taken for a disk.)
    mounted.drive = textIs(state.find(u"mode"), "CDROM"_L1)  ? u"cd"_s
                    : textIs(state.find(u"mode"), "Disk"_L1) ? u"disk"_s
                                                             : QString();
    mounted.readOnly = true;
    const json::Value* filename = state.find(u"filename");
    if (fromItsStorage(state)) {
        if (filename != nullptr && filename->isText() && isOneName(filename->text)) {
            mounted.image = filename->text;
            mounted.name = mounted.image;
            if (const auto at = listed.constFind(mounted.image); at != listed.constEnd()) {
                mounted.sha256 = notes.hashOf(key, mounted.image, at->size, at->stamp);
            }
        } else {
            // (A name it does not hand back, or none, goes by an id of its own, never as the name it reads as, nor
            // with that one's SHA-256: review cx4-1 M2.)
            mounted.image = kvmd::namelessId(digestOf(state));
            mounted.name = plainOf(filename, 200);
            if (mounted.name.isEmpty()) {
                mounted.name = u"an image"_s;
            }
        }
    } else if (fromAnAddress) {
        // (Mounted at the JetKVM's own page from an address: what the address is, is not said, it may hold a password.
        // Nor is it taken for an image of its storage that is called so: it has no SHA-256 of one.)
        mounted.image = fromAnAddressId(state);
        mounted.name = u"an image from an address, mounted at the JetKVM's own page"_s;
    } else {
        // (Sent from a browser at its own page: none of its storage either, whatever it is called.)
        mounted.image = u"(a browser)/"_s + digestOf(state);
        mounted.name = u"an image from a browser, mounted at the JetKVM's own page"_s;
    }
    return mounted;
}

QString fromAnAddressId(const json::Value& state)
{
    return u"(an address)/"_s + digestOf(state);
}

QString digestOf(const json::Value& state)
{
    // (Made once, at random, and kept nowhere but here.)
    static const QByteArray key = [] {
        constexpr int keyBytes = 32;
        QByteArray made;
        made.reserve(keyBytes);
        for (int at = 0; at < keyBytes; ++at) {
            made.append(static_cast<char>(QRandomGenerator::system()->bounded(256)));
        }
        return made;
    }();
    QMessageAuthenticationCode code(QCryptographicHash::Sha256, key);
    for (const QStringView name : {u"source", u"url", u"mode", u"filename", u"size"}) {
        // Each as it came, by its kind: text, a number, anything else, or none at all.
        const json::Value* value = state.isObject() ? state.find(name) : nullptr;
        QByteArray part;
        if (value == nullptr) {
            part = "-"_ba;
        } else if (value->isText()) {
            part = "t"_ba;
            part.append(value->text.toUtf8());
        } else if (value->isNumber()) {
            part = "n"_ba;
            part.append(QByteArray::number(value->number, 'g', 17));
        } else {
            part = "?"_ba;
        }
        QByteArray counted = QByteArray::number(part.size());
        counted.append(':');
        counted.append(part);
        code.addData(counted);
    }
    constexpr qsizetype hexOfTheId = 16;
    return QString::fromLatin1(code.result().toHex().left(hexOfTheId));
}

bool fromItsStorage(const json::Value& state)
{
    return state.isObject() && textIs(state.find(u"source"), "Storage"_L1);
}

Space spaceOf(const json::Value& said)
{
    return Space{bytesOf(in(said, u"bytesUsed"), mostStorageBytes), bytesOf(in(said, u"bytesFree"), mostStorageBytes)};
}

MediaOffer offerOf(bool atx)
{
    MediaOffer offer;
    offer.drives = {u"cd"_s, u"disk"_s};
    offer.restart = atx;
    offer.upload = true;
    offer.where = u"device"_s;
    offer.canDelete = true;
    offer.dropsInput = true;
    return offer;
}

Started startedOf(const json::Value& said)
{
    Started started;
    const json::Value* already = in(said, u"alreadyUploadedBytes");
    started.anew = already != nullptr && already->isNumber() && already->number == 0;
    const json::Value* id = in(said, u"dataChannel");
    if (started.anew && id != nullptr && id->isText() && isUploadId(id->text)) {
        started.id = id->text;
    }
    return started;
}

base::Error notSent(const base::Error& why)
{
    return why.code == "dropped" ? base::Error{"action_failed", "It was not done: " + why.message} : why;
}

bool wentWhole(qint64 sent, qint64 size, const QString& sha256)
{
    // (A SHA-256 in hex is 64 signs: CarriedImage's, once every byte has passed.)
    constexpr qsizetype hexOfASha256 = 64;
    return size > 0 && sent == size && sha256.size() == hexOfASha256;
}

} // namespace qkvm::core::jetkvm
