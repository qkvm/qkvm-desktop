#pragma once

#include <algorithm>
#include <optional>
#include <utility>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>
#include <QUrl>

#include <qkvm/core/incidents.h>
#include <qkvm/core/pacing_file.h>
#include <qkvm/core/profile.h>
#include <qkvm/core/sealing.h>
#include <qkvm/core/secrets.h>
#include <qkvm/core/setup_overrides.h>
#include <qkvm/core/sign_in_pacing.h>
#include <qkvm/core/wiped.h>

#include "profile_writing.h"

// A published version's profile (ui/tests/fixtures/profiles/<version>) put where the program keeps one, and what an
// upgrade made of it compared with what was put there: for the VM step "upgrade keeps everything"
// (tools/test_upgrade_keeps_vm.ps1, through qkvm-test-load-profile) and for its own test. Tests only: nothing of it is
// in the program. It takes the store and the folder it is handed; which those are is its caller's to say.
namespace qkvm::testing::profiles {

inline QJsonObject jsonIn(const QString& file)
{
    QFile kept(file);
    return kept.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(kept.readAll()).object() : QJsonObject();
}

inline QByteArray bytesOf(const QString& file)
{
    QFile kept(file);
    return kept.open(QIODevice::ReadOnly) ? kept.readAll() : QByteArray();
}

struct Fixture {
    QString folder;
    QJsonObject made;
    QJsonObject settings;
    QJsonObject secrets;

    explicit Fixture(const QString& at)
        : folder(at), made(jsonIn(at + QStringLiteral("/made.json"))),
          settings(jsonIn(at + QStringLiteral("/profile/settings.json"))),
          secrets(jsonIn(at + QStringLiteral("/secrets.json")))
    {}
    bool whole() const { return !made.isEmpty() && !settings.isEmpty() && !secrets.isEmpty(); }
    QString profileFolder() const { return folder + QStringLiteral("/profile"); }
    // Every file of the profile, as a path from its folder.
    QStringList files() const { return filesUnder(profileFolder()); }
    QByteArray secret(const QString& name) const
    {
        return QByteArray::fromBase64(secrets[name].toObject()[QStringLiteral("bytes")].toString().toLatin1());
    }
    QString kind() const { return made[QStringLiteral("made_by")][QStringLiteral("kind")].toString(); }
    // The version the settings say wrote them (from 0.1.2 on); empty where they say none.
    QString writer() const { return settings[QStringLiteral("written_by")].toString(); }
    // The version the profile recorded as the one that started with it last (from 0.1.2 on); empty where none.
    QString recorded() const
    {
        return core::profile::recorded(QDir(profileFolder()).filePath(QStringLiteral("settings.json")))
            .value_or(core::profile::Record{})
            .version;
    }
};

// Puts the fixture where the program keeps a profile (profile_writing.h): into a store in memory, for the tests; the VM
// step's writer, built at the fixture's own commit, does the same into Windows' own.
inline QStringList load(const Fixture& fixture, core::SecretStore& store, const QString& folder)
{
    return fixture.whole() ? writeProfile(fixture.folder, store, folder)
                           : QStringList{QStringLiteral("the fixture is not whole")};
}

// Files the program itself takes away by its own rules, which the check says and does not count as lost: a hand-over
// note to Setup, read and taken away at the next start (Updates), and the screen watches' pictures, taken away a month
// after their alert (ScreenWatch).
inline bool takenAwayByTheProgram(const QString& file)
{
    return file == QStringLiteral("updates/setup-handover.json") || file.startsWith(QStringLiteral("alert-pictures/"));
}
// What the program moves on by its own rules, and is no loss: the time of its last look for an update, which a look
// moves later (one at a start after a hand-over to Setup that did not install, or the daily one: dry run 3 of the VM
// step, from 0.1.0, 2026-11-15), and never earlier or away; and two lists that it keeps as sets, the servers (the
// one signed in to last comes first: ServerSource) and the addresses reached encrypted (only ever more), of which
// each one there before is there still, in any place.
inline bool movesLaterOnly(const QString& path)
{
    return path == QStringLiteral("last_update_check");
}
inline bool keptAsASet(const QString& path)
{
    return path == QStringLiteral("servers") || path == QStringLiteral("reached_encrypted");
}

// Where `old` says something that `now` does not say the same, as paths ("kvms[2].user"); what `now` says more is no
// loss. A window's place is said apart (`moved`): Windows may move a window onto the screen it is shown on; and so is
// a time the program moved later (`later`). (A time it never had is "": from that, any time is later.)
inline void lostFrom(const QJsonValue& old, const QJsonValue& now, const QString& at, QStringList& lost,
                     QStringList& moved, QStringList& later)
{
    if (old.isObject() && now.isObject()) {
        const QJsonObject was = old.toObject();
        const QJsonObject is = now.toObject();
        for (auto one = was.begin(); one != was.end(); ++one) {
            const QString path = at.isEmpty() ? one.key() : at + u'.' + one.key();
            if (!is.contains(one.key())) {
                lost.append(path);
            } else if (movesLaterOnly(path) && one.value() != is[one.key()]) {
                const QDateTime then = QDateTime::fromString(one.value().toString(), Qt::ISODate);
                const QDateTime since = QDateTime::fromString(is[one.key()].toString(), Qt::ISODate);
                const bool never = one.value().isString() && one.value().toString().isEmpty();
                (since.isValid() && (never || (then.isValid() && since > then)) ? later : lost).append(path);
            } else if (keptAsASet(path) && one.value().isArray() && is[one.key()].isArray()) {
                const QJsonArray before = one.value().toArray();
                const QJsonArray still = is[one.key()].toArray();
                for (qsizetype index = 0; index < before.size(); ++index) {
                    if (!still.contains(before.at(index))) {
                        lost.append(QStringLiteral("%1[%2]").arg(path).arg(index));
                    }
                }
            } else {
                lostFrom(one.value(), is[one.key()], path, lost, moved, later);
            }
        }
        return;
    }
    if (old.isArray() && now.isArray()) {
        const QJsonArray was = old.toArray();
        const QJsonArray is = now.toArray();
        for (qsizetype index = 0; index < was.size(); ++index) {
            const QString path = QStringLiteral("%1[%2]").arg(at).arg(index);
            if (index >= is.size()) {
                lost.append(path);
            } else {
                lostFrom(was.at(index), is.at(index), path, lost, moved, later);
            }
        }
        return;
    }
    if (old != now) {
        static const QRegularExpression place(QStringLiteral("\\Awindows\\[\\d+\\]\\.place\\."));
        (place.match(at).hasMatch() ? moved : lost).append(at);
    }
}

// The counts of the sign-ins as the file holds them, by each one's digest (hex): noes and silent ones. Read with the
// key the store holds, as the program reads them, and nothing is changed: no new key, nothing put aside.
inline std::optional<QHash<QString, std::pair<int, int>>> signInsIn(const QString& file, core::SecretStore& store)
{
    const base::Result<std::optional<QByteArray>> key = store.read(QString::fromLatin1(core::PacingFile::keyName));
    QByteArray bytes = key ? key.value().value_or(QByteArray()) : QByteArray();
    const core::WipedOnLeaving overBytes(bytes);
    if (bytes.size() != core::SecretKey::bytes) {
        return std::nullopt;
    }
    core::SecretKey sealedUnder;
    std::copy(bytes.cbegin(), bytes.cend(), sealedUnder.data());
    const QJsonObject top = jsonIn(file);
    const base::Result<core::SecretBytes> opened = core::sealing::open(
        sealedUnder, QByteArray::fromBase64(top[QStringLiteral("nonce")].toString().toLatin1()),
        QByteArray::fromBase64(top[QStringLiteral("sealed")].toString().toLatin1()), "qkvm sign-in pacing 1");
    if (!opened) {
        return std::nullopt;
    }
    QHash<QString, std::pair<int, int>> kept;
    const QJsonArray all =
        QJsonDocument::fromJson(opened.value().view().toByteArray()).object()[QStringLiteral("kept")].toArray();
    for (const QJsonValue one : all) {
        kept.insert(one[QStringLiteral("digest")].toString(),
                    {one[QStringLiteral("noes")].toInt(), one[QStringLiteral("silent")].toInt()});
    }
    return kept;
}

// What an upgrade made of the fixture that was put in `folder` and `store`, by `version` (the one this is built as):
// every credential, the settings, the sign-ins' counts, the alerts, the setup overrides, the copy of the settings kept
// at its first start and its record, and no lock. "passed" where all of it holds.
inline QJsonObject check(const Fixture& fixture, core::SecretStore& store, const QString& folder,
                         const QString& version)
{
    QStringList failures;
    const QString settingsFile = QDir(folder).filePath(QStringLiteral("settings.json"));
    // Every credential, byte for byte, and what is there besides.
    QJsonObject credentials;
    for (auto one = fixture.secrets.begin(); one != fixture.secrets.end(); ++one) {
        const base::Result<std::optional<QByteArray>> read = store.read(one.key());
        const QString said = !read || !read.value()                       ? QStringLiteral("missing")
                             : *read.value() == fixture.secret(one.key()) ? QStringLiteral("same")
                                                                          : QStringLiteral("changed");
        credentials.insert(one.key(), said);
        if (said != QStringLiteral("same")) {
            failures.append(QStringLiteral("the credential %1 is %2").arg(one.key(), said));
        }
    }
    QJsonArray extra;
    if (const base::Result<QStringList> names = store.names(); names) {
        for (const QString& name : names.value()) {
            if (!fixture.secrets.contains(name)) {
                extra.append(name);
            }
        }
    } else {
        failures.append(QStringLiteral("the store's names were not handed over"));
    }
    // The settings: all the old file said, said the same.
    QStringList lost;
    QStringList moved;
    QStringList later;
    // (But which version wrote them, from 0.1.2 on: the one before, or this one once it has written them. The copy
    // kept at the first start holds the old name.)
    const QJsonObject settingsNow = jsonIn(settingsFile);
    QJsonObject settingsThen = fixture.settings;
    settingsThen.remove(QStringLiteral("written_by"));
    lostFrom(settingsThen, settingsNow, {}, lost, moved, later);
    if (const QString writtenBy = settingsNow[QStringLiteral("written_by")].toString();
        !fixture.writer().isEmpty() && writtenBy != fixture.writer() && writtenBy != version) {
        lost.append(QStringLiteral("written_by"));
    }
    for (const QString& path : lost) {
        failures.append(QStringLiteral("settings.json lost %1").arg(path));
    }
    // Each file, byte for byte (the program may write one anew: what it says is held above and below).
    QJsonObject files;
    for (const QString& file : fixture.files()) {
        const QString now = QDir(folder).filePath(file);
        files.insert(
            file, !QFileInfo::exists(now) ? (takenAwayByTheProgram(file) ? QStringLiteral("taken away by the program")
                                                                         : QStringLiteral("missing"))
                  : bytesOf(now) == bytesOf(QDir(fixture.profileFolder()).filePath(file)) ? QStringLiteral("same")
                                                                                          : QStringLiteral("changed"));
        if (!QFileInfo::exists(now) && !takenAwayByTheProgram(file)) {
            failures.append(QStringLiteral("%1 is gone").arg(file));
        }
    }
    // The sign-ins' counts: no refusal forgotten (nothing can refuse anew: the KVMs are at addresses that reach
    // nothing), none that came to nothing forgotten.
    QJsonArray signIns;
    const auto counts = signInsIn(core::profile::signInsFile(settingsFile), store);
    if (!counts) {
        failures.append(QStringLiteral("the sign-ins' counts do not open"));
    }
    for (const QJsonValue one : fixture.made[QStringLiteral("pacing")].toArray()) {
        const core::SignInPacing::Mark mark = core::SignInPacing::what(
            one[QStringLiteral("road")].toString(), QUrl(one[QStringLiteral("address")].toString()),
            one[QStringLiteral("user")].toString(), one[QStringLiteral("password")].toString());
        const std::pair<int, int> now =
            counts ? counts->value(QString::fromLatin1(mark.digest.toHex()), {-1, -1}) : std::pair<int, int>{-1, -1};
        signIns.append(QJsonObject{{QStringLiteral("address"), one[QStringLiteral("address")]},
                                   {QStringLiteral("noes_then"), one[QStringLiteral("noes")]},
                                   {QStringLiteral("noes_now"), now.first},
                                   {QStringLiteral("silent_then"), one[QStringLiteral("silent")]},
                                   {QStringLiteral("silent_now"), now.second}});
        if (now.first != one[QStringLiteral("noes")].toInt() || now.second < one[QStringLiteral("silent")].toInt()) {
            failures.append(QStringLiteral("the sign-ins' counts at %1 do not hold")
                                .arg(one[QStringLiteral("address")].toString()));
        }
    }
    // The alerts as they were (more may have come since), and the setup overrides.
    {
        const core::Incidents incidents(core::profile::incidentsFile(settingsFile));
        const QList<core::Incidents::Entry> entries = incidents.entries(QStringLiteral("pc"));
        for (const QJsonValue one : fixture.made[QStringLiteral("incidents")].toArray()) {
            const bool there = std::ranges::any_of(entries, [&one](const core::Incidents::Entry& entry) {
                return entry.id == one[QStringLiteral("id")].toString() &&
                       entry.key == one[QStringLiteral("key")].toString() &&
                       entry.state == one[QStringLiteral("state")].toString();
            });
            if (incidents.damaged() || !there) {
                failures.append(QStringLiteral("the alert %1 is gone").arg(one[QStringLiteral("key")].toString()));
            }
        }
    }
    {
        const core::SetupOverrides overrides(core::profile::setupOverridesFile(settingsFile));
        for (const QJsonValue one : fixture.made[QStringLiteral("setup_overrides")].toArray()) {
            // (Marked queued where it was, from 0.1.2 on: then no look lets it go.)
            const bool there = std::ranges::any_of(overrides.all(), [&one](const core::SetupOverrides::Left& left) {
                return left.address == one[QStringLiteral("address")].toString() &&
                       left.machine == one[QStringLiteral("machine")].toString() &&
                       left.queued == one[QStringLiteral("queued")].toBool();
            });
            if (!there) {
                failures.append(
                    QStringLiteral("the setup override of %1 is gone").arg(one[QStringLiteral("address")].toString()));
            }
        }
    }
    // The way back: the settings as they were found, kept at the first start of this version (named by the version
    // that wrote them, where they say one: from 0.1.2 on; else as found before this one), and the record, with the
    // version recorded before it (none before 0.1.2).
    const QString writer = fixture.writer();
    const QString keptAs =
        QStringLiteral("%1.%2.bak").arg(settingsFile, writer.isEmpty() ? QStringLiteral("before-") + version : writer);
    const bool keptWhole =
        bytesOf(keptAs) == bytesOf(QDir(fixture.profileFolder()).filePath(QStringLiteral("settings.json")));
    if (!QFileInfo::exists(keptAs) || !keptWhole) {
        failures.append(QStringLiteral("the settings were not kept as %1").arg(QFileInfo(keptAs).fileName()));
    }
    const std::optional<core::profile::Record> record = core::profile::recorded(settingsFile);
    if (!record || record->version != version) {
        failures.append(QStringLiteral("the version is not recorded"));
    }
    const QString recordedBefore = fixture.recorded();
    if (record && record->previous != recordedBefore) {
        failures.append(QStringLiteral("the version before is not recorded as %1")
                            .arg(recordedBefore.isEmpty() ? QStringLiteral("none") : recordedBefore));
    }
    const bool lockedAside = QFileInfo::exists(core::profile::lockFile(settingsFile));
    if (lockedAside) {
        failures.append(QStringLiteral("the app lock left its marker"));
    }
    return QJsonObject{
        {QStringLiteral("passed"), failures.isEmpty()},
        {QStringLiteral("failures"), QJsonArray::fromStringList(failures)},
        {QStringLiteral("credentials"), credentials},
        {QStringLiteral("extra_credentials"), extra},
        {QStringLiteral("settings_lost"), QJsonArray::fromStringList(lost)},
        {QStringLiteral("windows_moved"), QJsonArray::fromStringList(moved)},
        {QStringLiteral("settings_moved_on"), QJsonArray::fromStringList(later)},
        {QStringLiteral("files"), files},
        {QStringLiteral("sign_ins"), signIns},
        {QStringLiteral("kept_as"), QFileInfo(keptAs).fileName()},
        {QStringLiteral("kept_whole"), keptWhole},
        {QStringLiteral("recorded"), record ? QJsonObject{{QStringLiteral("version"), record->version},
                                                          {QStringLiteral("previous"), record->previous}}
                                            : QJsonObject()},
        {QStringLiteral("lock_marker"), lockedAside},
    };
}

} // namespace qkvm::testing::profiles
