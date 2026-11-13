#include <qkvm/core/update_release.h>

#include <cstring>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

#include <qkvm/core/auto_start.h>
#include <qkvm/core/settings.h>
#include <qkvm/core/strict_json.h>

#include "cng.h"
#include "update_channel.h"

using namespace Qt::StringLiterals;

namespace qkvm::core {
namespace {

base::Error invalid()
{
    return {"update:release", "The update information is invalid, expired or not signed by qKVM."};
}

bool uniqueObject(const json::Value& value)
{
    if (!value.isObject()) {
        return false;
    }
    QSet<QString> names;
    for (const auto& member : value.members) {
        if (names.contains(member.name)) {
            return false;
        }
        names.insert(member.name);
    }
    return true;
}

QString text(const json::Value& object, QStringView key)
{
    const auto* value = object.find(key);
    return value != nullptr && value->isText() ? value->text : QString();
}

QByteArray base64(const QString& value)
{
    const auto encoded = value.toLatin1();
    const auto decoded = QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
    return decoded && decoded.decoded.toBase64() == encoded ? decoded.decoded : QByteArray();
}

bool signedBy(const QByteArray& payload, QByteArray signature, const QByteArray& publicKey)
{
    if (publicKey.size() != 64 || signature.size() != 64) {
        return false;
    }
    const cng::Algorithm algorithm(BCRYPT_ECDSA_P256_ALGORITHM, 0);
    if (!algorithm) {
        return false;
    }
    const BCRYPT_ECCKEY_BLOB header{BCRYPT_ECDSA_PUBLIC_P256_MAGIC, 32};
    QByteArray blob(static_cast<qsizetype>(sizeof(header)), '\0');
    std::memcpy(blob.data(), &header, sizeof(header));
    blob.append(publicKey);
    BCRYPT_KEY_HANDLE key = nullptr;
    if (!cng::worked(BCryptImportKeyPair(algorithm.get(), nullptr, BCRYPT_ECCPUBLIC_BLOB, &key,
                                         reinterpret_cast<PUCHAR>(blob.data()), static_cast<ULONG>(blob.size()), 0))) {
        return false;
    }
    QByteArray digest = QCryptographicHash::hash(payload, QCryptographicHash::Sha256);
    const bool valid = cng::worked(
        BCryptVerifySignature(key, nullptr, reinterpret_cast<PUCHAR>(digest.data()), static_cast<ULONG>(digest.size()),
                              reinterpret_cast<PUCHAR>(signature.data()), static_cast<ULONG>(signature.size()), 0));
    BCryptDestroyKey(key);
    return valid;
}

QDateTime utcDate(const QString& value)
{
    // A single unambiguous spelling, no local time and no ignored suffix.
    const auto date = QDateTime::fromString(value, Qt::ISODate);
    return value.endsWith(u'Z') && date.isValid() && date.toUTC().toString(Qt::ISODate) == value ? date.toUTC()
                                                                                                 : QDateTime();
}

// What the signed payload calls each kind of release, and what its package's name ends in.
QString platformOf(UpdateKind kind)
{
    switch (kind) {
    case UpdateKind::Msix:
        return u"windows-x64-msix"_s;
    case UpdateKind::Setup:
        return u"windows-x64-setup"_s;
    case UpdateKind::Portable:
        break;
    }
    return u"windows-x64-portable"_s;
}

QString endingOf(UpdateKind kind)
{
    switch (kind) {
    case UpdateKind::Msix:
        return u".msix"_s;
    case UpdateKind::Setup:
        return u"-setup.exe"_s;
    case UpdateKind::Portable:
        break;
    }
    return u".zip"_s;
}

} // namespace

UpdateKind updateKindOf(const QString& programFolder, bool packaged)
{
    // Setup's marker, read whole: one byte more or less, or another byte, and this is no copy of Setup's.
    // (A folder of that name does not open as a file.)
    QFile marker(QDir(programFolder).filePath(QString::fromLatin1(setupMarkerName)));
    const QByteArray expected(setupMarkerContent);
    if (marker.open(QIODevice::ReadOnly) && marker.read(expected.size() + 1) == expected) {
        return UpdateKind::Setup;
    }
    // A copy that Windows installed from a package has the package's identity; a portable copy has none.
    return packaged ? UpdateKind::Msix : UpdateKind::Portable;
}

QUrl setupFeedBeside(const QUrl& feed)
{
    return updateAddress(feed) ? feed.resolved(QUrl(u"stable-setup.json"_s)) : QUrl();
}

UpdateChannel updateChannelFor(const QUrl& builtInFeed, const QByteArray& publicKey, const QString& currentVersion,
                               const QString& programFolder, bool packaged, const QString& dataFolder)
{
    const UpdateKind kind = updateKindOf(programFolder, packaged);
    return {kind == UpdateKind::Setup ? setupFeedBeside(builtInFeed) : builtInFeed,
            publicKey,
            currentVersion,
            kind,
            programFolder,
            dataFolder};
}

UpdateChannel desktopUpdateChannel()
{
    return updateChannelFor(QUrl(QString::fromLatin1(updateFeed)), QByteArray::fromHex(updatePublicKey),
                            QString::fromLatin1(updateCurrentVersion), QCoreApplication::applicationDirPath(),
                            !windowsPackageFamily().isEmpty(), QFileInfo(Settings::usualFile()).absolutePath());
}

bool updateAddress(const QUrl& address)
{
    return address.isValid() && address.scheme() == u"https"_s && !address.host().isEmpty() &&
           address.userInfo().isEmpty() && !address.hasFragment() && !address.hasQuery() &&
           address.toEncoded().size() <= 2048;
}

bool updatePackageRedirect(const QUrl& from, const QUrl& to)
{
    static const QRegularExpression release(u"\\A/[A-Za-z0-9._-]+/[A-Za-z0-9._-]+/releases/download/[^/]+/[^/]+\\z"_s);
    static const QStringList fileHosts{u"release-assets.githubusercontent.com"_s, u"objects.githubusercontent.com"_s};
    const auto plain = [](const QUrl& address) {
        return address.isValid() && address.scheme() == u"https"_s && address.userInfo().isEmpty() &&
               (address.port() == -1 || address.port() == 443) && !address.hasFragment() &&
               address.toEncoded().size() <= 4096;
    };
    // (A path that climbs, ".." or ".", is not the release's, whatever it would come to.)
    return plain(from) && from.host() == u"github.com"_s && !from.hasQuery() &&
           from.adjusted(QUrl::NormalizePathSegments) == from &&
           release.match(from.path(QUrl::FullyEncoded)).hasMatch() && plain(to) && fileHosts.contains(to.host());
}

QVersionNumber updateVersion(const QString& text)
{
    static const QRegularExpression shape(u"\\A(0|[1-9][0-9]{0,4})\\.(0|[1-9][0-9]{0,4})\\.(0|[1-9][0-9]{0,4})\\z"_s);
    if (!shape.match(text).hasMatch()) {
        return {};
    }
    auto version = QVersionNumber::fromString(text);
    for (int segment : version.segments()) {
        if (segment > 65535) {
            return {};
        }
    }
    return version;
}

base::Result<UpdateRelease> verifiedUpdate(QByteArrayView envelope, const QByteArray& publicKey, const QDateTime& now,
                                           UpdateKind kind)
{
    if (envelope.size() > mostUpdateMetadata || !now.isValid()) {
        return invalid();
    }
    const auto outer = json::parse(envelope);
    if (!outer || !uniqueObject(outer.value()) || outer.value().members.size() != 2) {
        return invalid();
    }
    const QByteArray payload = base64(text(outer.value(), u"payload"));
    if (payload.isEmpty() || !signedBy(payload, base64(text(outer.value(), u"signature")), publicKey)) {
        return invalid();
    }
    const auto parsed = json::parse(payload);
    if (!parsed || !uniqueObject(parsed.value())) {
        return invalid();
    }
    const auto& value = parsed.value();
    const auto* schema = value.find(u"schema");
    const auto* bytes = value.find(u"bytes");
    if (schema == nullptr || !schema->isNumber() || !schema->whole || schema->number != 1 || bytes == nullptr ||
        !bytes->isNumber() || !bytes->whole || bytes->number < 1 || bytes->number > mostUpdatePackage ||
        text(value, u"product") != u"qkvm-desktop"_s || text(value, u"platform") != platformOf(kind) ||
        text(value, u"channel") != u"stable"_s) {
        return invalid();
    }
    UpdateRelease release;
    release.version = text(value, u"version");
    release.notes = text(value, u"notes");
    release.published = utcDate(text(value, u"published"));
    release.expires = utcDate(text(value, u"expires"));
    release.package = QUrl(text(value, u"package"), QUrl::StrictMode);
    release.bytes = static_cast<qint64>(bytes->number);
    const auto hash = text(value, u"sha256").toLatin1();
    release.sha256 = QByteArray::fromHex(hash);
    if (updateVersion(release.version).isNull() || release.notes.size() > 16000 || !release.published.isValid() ||
        !release.expires.isValid() || release.published > now.addSecs(300) || release.expires <= now ||
        release.expires <= release.published || release.expires > release.published.addDays(31) ||
        !updateAddress(release.package) || !release.package.path().endsWith(endingOf(kind), Qt::CaseInsensitive) ||
        hash.size() != 64 || release.sha256.size() != 32 || release.sha256.toHex() != hash) {
        return invalid();
    }
    return release;
}

} // namespace qkvm::core
