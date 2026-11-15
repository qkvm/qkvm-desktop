#pragma once

#include <optional>

#include <QByteArray>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <qkvm/core/secrets.h>

// A published version's profile (ui/tests/fixtures/profiles/<version>) put where the program keeps one: its files
// into a folder and its secrets into a store, byte for byte, each read back. What the VM step's writer does
// (profile_writer.cpp, built at the fixture's own commit, so that the store is that version's own Windows code: review
// 012, M3), and what the tests do with a store in memory. Of the program's code it uses SecretStore alone, which every
// published version has. Tests only.
namespace qkvm::testing::profiles {

// Every file under a folder, as a path from it ("settings.json", "alert-pictures/0a1b2c3d-1.jpg"), sorted.
inline QStringList filesUnder(const QString& folder)
{
    QStringList found;
    QDirIterator each(folder, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (each.hasNext()) {
        found.append(QDir(folder).relativeFilePath(each.next()));
    }
    found.sort();
    return found;
}

inline QByteArray bytesIn(const QString& file)
{
    QFile kept(file);
    return kept.open(QIODevice::ReadOnly) ? kept.readAll() : QByteArray();
}

// The fixture's secrets, by name: their bytes.
inline std::optional<QJsonObject> secretsOf(const QString& fixture)
{
    QFile file(QDir(fixture).filePath(QStringLiteral("secrets.json")));
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    const QJsonDocument read = QJsonDocument::fromJson(file.readAll());
    return read.isObject() && !read.object().isEmpty() ? std::optional<QJsonObject>(read.object()) : std::nullopt;
}

inline QByteArray secretIn(const QJsonObject& secrets, const QString& name)
{
    return QByteArray::fromBase64(secrets[name].toObject()[QStringLiteral("bytes")].toString().toLatin1());
}

// Puts the fixture at `fixture` where the program keeps a profile: its files into `folder` and its secrets into
// `store`, byte for byte, each read back. Only into an empty place: nothing of the program's in the store, no
// settings in the folder. What went wrong, in words; nothing where all went in.
inline QStringList writeProfile(const QString& fixture, core::SecretStore& store, const QString& folder)
{
    const std::optional<QJsonObject> secrets = secretsOf(fixture);
    const QString from = QDir(fixture).filePath(QStringLiteral("profile"));
    const QStringList files = filesUnder(from);
    if (!secrets || !files.contains(QStringLiteral("settings.json"))) {
        return {QStringLiteral("the fixture is not whole")};
    }
    const base::Result<QStringList> there = store.names();
    if (!there) {
        return {QStringLiteral("the store's names were not handed over")};
    }
    if (!there.value().isEmpty()) {
        return {QStringLiteral("the store holds the program's secrets already: %1").arg(there.value().join(u", "))};
    }
    if (QFileInfo::exists(QDir(folder).filePath(QStringLiteral("settings.json")))) {
        return {QStringLiteral("the folder holds settings already")};
    }
    QStringList wrong;
    for (const QString& file : files) {
        const QString into = QDir(folder).filePath(file);
        if (QFileInfo::exists(into) || !QDir().mkpath(QFileInfo(into).absolutePath()) ||
            !QFile::copy(QDir(from).filePath(file), into) || bytesIn(into) != bytesIn(QDir(from).filePath(file))) {
            wrong.append(QStringLiteral("%1 was not put there whole").arg(file));
        }
    }
    for (auto one = secrets->begin(); one != secrets->end(); ++one) {
        const QByteArray bytes = secretIn(*secrets, one.key());
        const base::Result<void> kept = store.keep(one.key(), bytes);
        const base::Result<std::optional<QByteArray>> read = store.read(one.key());
        if (!kept || !read || !read.value() || *read.value() != bytes) {
            wrong.append(QStringLiteral("%1 was not kept byte for byte").arg(one.key()));
        }
    }
    return wrong;
}

} // namespace qkvm::testing::profiles
