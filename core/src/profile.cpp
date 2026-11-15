#include <qkvm/core/profile.h>

#include <algorithm>
#include <cmath>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>

using namespace Qt::StringLiterals;

namespace qkvm::core::profile {

namespace {

// As many copies of one name as are numbered, before they are named by the time; the largest settings file a copy is
// made of (four times the largest the program writes: settings.cpp), and the largest record read.
constexpr int mostNumbered = 999;
constexpr qint64 largestSettings = qint64{256} * 1024 * 1024;
constexpr qint64 largestRecord = 4096;

// The version the settings say wrote them ("written_by"); empty where they say none, or no version a profile records.
QString writtenBy(const QString& settings)
{
    QFile file(settings);
    if (!file.open(QIODevice::ReadOnly) || file.size() > largestSettings) {
        return {};
    }
    const QString said = QJsonDocument::fromJson(file.readAll()).object().value("written_by"_L1).toString();
    return namesAVersion(said) ? said : QString();
}

} // namespace

QString signInsFile(const QString& settings)
{
    return settings + u".sign-ins"_s;
}

QString lockFile(const QString& settings)
{
    return settings + u".locked"_s;
}

QString setupOverridesFile(const QString& settings)
{
    return settings + u".setup-overrides"_s;
}

QString incidentsFile(const QString& settings)
{
    return settings + u".incidents"_s;
}

QString alertPicturesFolder(const QString& settings)
{
    return QFileInfo(settings).absolutePath() + u"/alert-pictures"_s;
}

QString versionFile(const QString& settings)
{
    return settings + u".version"_s;
}

bool namesAVersion(const QString& version)
{
    static const QRegularExpression shape(u"\\A[0-9A-Za-z.+-]{1,32}\\z"_s);
    static const QRegularExpression something(u"[0-9A-Za-z]"_s);
    return shape.match(version).hasMatch() && something.match(version).hasMatch();
}

std::optional<Record> recorded(const QString& settings)
{
    QFile file(versionFile(settings));
    if (!file.open(QIODevice::ReadOnly) || file.size() > largestRecord) {
        return std::nullopt;
    }
    const QJsonDocument read = QJsonDocument::fromJson(file.readAll());
    const QJsonValue version = read.object().value("version"_L1);
    const QJsonValue previous = read.object().value("previous"_L1);
    const QJsonValue time = read.object().value("settings_time"_L1);
    if (!read.isObject() || !version.isString() || !namesAVersion(version.toString()) || !previous.isString() ||
        (!previous.toString().isEmpty() && !namesAVersion(previous.toString()))) {
        return std::nullopt;
    }
    // (A record of 0.1.2's first builds says no time: as one whose settings were written since.)
    std::optional<qint64> settingsTime;
    if (time.isDouble() && time.toDouble() >= 0 && time.toDouble() == std::floor(time.toDouble())) {
        settingsTime = static_cast<qint64>(time.toDouble());
    }
    return Record{version.toString(), previous.toString(), settingsTime};
}

namespace {

// The name a copy of this kind and name has before its number, after the settings' own: "<name>" or "reset-<name>".
QString stemOf(CopyKind kind, const QString& name)
{
    return kind == CopyKind::Reset ? u"reset-"_s + name : name;
}

// The copies of a kind beside the settings, but `except`, the newest first (by their time, then their name).
QFileInfoList copiesOf(const QString& settings, CopyKind kind, const QString& except)
{
    const QFileInfo was(settings);
    const QString start = was.fileName() + u'.';
    QFileInfoList found;
    for (const QFileInfo& one :
         was.dir().entryInfoList({start + u"*.bak"_s}, QDir::Files | QDir::Hidden | QDir::System, QDir::Name)) {
        static const QRegularExpression named(u"\\A[0-9A-Za-z.+-]+\\z"_s);
        const QString stem = one.fileName().mid(start.size()).chopped(4);
        const bool isReset = stem.startsWith(u"reset-"_s);
        if (isReset == (kind == CopyKind::Reset) && named.match(stem).hasMatch() &&
            one.absoluteFilePath().compare(QFileInfo(except).absoluteFilePath(), Qt::CaseInsensitive) != 0) {
            found.append(one);
        }
    }
    std::ranges::stable_sort(
        found, [](const QFileInfo& one, const QFileInfo& other) { return one.lastModified() > other.lastModified(); });
    return found;
}

} // namespace

QString nextCopy(const QString& settings, CopyKind kind, const QString& name)
{
    if (!namesAVersion(name)) {
        return {};
    }
    const QString stem = stemOf(kind, name);
    for (int number = 1; number <= mostNumbered; ++number) {
        const QString copy =
            number == 1 ? u"%1.%2.bak"_s.arg(settings, stem) : u"%1.%2-%3.bak"_s.arg(settings, stem).arg(number);
        if (!QFileInfo::exists(copy)) {
            return copy;
        }
    }
    // Past those, by the time, which is free but where two are made within one millisecond.
    const QString at = QDateTime::currentDateTimeUtc().toString(u"yyyyMMddHHmmsszzz"_s);
    for (int number = 1;; ++number) {
        const QString copy = u"%1.%2-%3-%4.bak"_s.arg(settings, stem, at).arg(number);
        if (!QFileInfo::exists(copy)) {
            return copy;
        }
    }
}

base::Result<QString> keepCopy(const QString& settings, CopyKind kind, const QString& name, const QString& as)
{
    if (!namesAVersion(name)) {
        return base::Error{"profile", "A copy of the settings is not named so."};
    }
    if (!QFileInfo(settings).isFile()) {
        return QString();
    }
    QFile from(settings);
    if (!from.open(QIODevice::ReadOnly) || from.size() > largestSettings) {
        return base::Error{"profile",
                           "The settings could not be read for their copy: " + from.errorString().toStdString()};
    }
    const QByteArray bytes = from.readAll();
    if (from.error() != QFileDevice::NoError || bytes.size() != from.size()) {
        return base::Error{"profile", "The settings could not be read whole for their copy."};
    }
    // Where a box said it goes, where that is still free and a name of this kind and name.
    const QString stem = stemOf(kind, name);
    const QString start = u"%1.%2"_s.arg(QFileInfo(settings).fileName(), stem);
    const QFileInfo said(as);
    const bool asSaid = !as.isEmpty() && !said.exists() && said.absolutePath() == QFileInfo(settings).absolutePath() &&
                        said.fileName().startsWith(start) && said.fileName().endsWith(u".bak"_s) &&
                        (said.fileName().size() == start.size() + 4 || said.fileName().at(start.size()) == u'-');
    const QString copy = asSaid ? as : nextCopy(settings, kind, name);
    // Whole or not at all, and beside the settings: written as a new file that takes its name at the end.
    QSaveFile into(copy);
    if (!into.open(QIODevice::WriteOnly) || into.write(bytes) != bytes.size() || !into.commit()) {
        return base::Error{"profile",
                           "The copy of the settings could not be written: " + into.errorString().toStdString()};
    }
    // The newest few of this kind, the new one among them; one that cannot be taken away is left.
    const QFileInfoList others = copiesOf(settings, kind, copy);
    for (qsizetype index = copiesKept - 1; index < others.size(); ++index) {
        static_cast<void>(QFile::remove(others.at(index).absoluteFilePath()));
    }
    return copy;
}

namespace {

// Who wrote the settings last: they say so from 0.1.2 on; where they say nobody, the version recorded, where they
// were not written since it recorded them; else one from before 0.1.2 (which drops the name when it writes them):
// empty then, and where there are no settings.
QString writerOf(const QString& settings, const std::optional<Record>& record)
{
    const QFileInfo found(settings);
    if (!found.isFile()) {
        return {};
    }
    QString writer = writtenBy(settings);
    if (writer.isEmpty() && record && record->settingsTime == found.lastModified().toMSecsSinceEpoch()) {
        writer = record->version;
    }
    return writer;
}

} // namespace

base::Result<void> recordVersion(const QString& settings, const QString& version)
{
    if (!namesAVersion(version)) {
        return base::Error{"profile", "The program's version is not one a profile records."};
    }
    const std::optional<Record> record = recorded(settings);
    const QFileInfo found(settings);
    const qint64 time = found.isFile() ? found.lastModified().toMSecsSinceEpoch() : 0;
    // (At a first start the folder is made here, as the settings' first writing would make it.)
    if (!QDir().mkpath(found.absolutePath())) {
        return base::Error{"profile", "The folder of the program's settings could not be made."};
    }
    QSaveFile file(versionFile(settings));
    const QByteArray said =
        QJsonDocument(QJsonObject{{u"version"_s, version},
                                  {u"previous"_s, record && record->version != version ? record->version
                                                  : record                             ? record->previous
                                                                                       : QString()},
                                  {u"settings_time"_s, static_cast<double>(time)}})
            .toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(said) != said.size() || !file.commit()) {
        return base::Error{"profile",
                           "The program's version could not be recorded: " + file.errorString().toStdString()};
    }
    return {};
}

base::Result<Started> recordStart(const QString& settings, const QString& version)
{
    if (!namesAVersion(version)) {
        return base::Error{"profile", "The program's version is not one a profile records."};
    }
    const std::optional<Record> record = recorded(settings);
    const QFileInfo found(settings);
    const bool there = found.isFile();
    const qint64 time = there ? found.lastModified().toMSecsSinceEpoch() : 0;
    const QString writer = writerOf(settings, record);
    Started started;
    started.previous = record ? record->version : QString();
    started.another = there ? writer != version : record && record->version != version;
    if (!started.another && record && record->version == version && record->settingsTime == time) {
        return started;
    }
    // The settings as the version before left them, before this one writes anything of them.
    if (started.another && there) {
        const base::Result<QString> kept =
            keepCopy(settings, CopyKind::VersionChange, writer.isEmpty() ? u"before-"_s + version : writer);
        if (!kept) {
            return base::Error{copyNotKept, kept.error().message};
        }
        started.keptAs = kept.value();
    }
    if (const base::Result<void> noted = recordVersion(settings, version); !noted) {
        return noted.error();
    }
    return started;
}
AtStart atStart(const QString& settings, const QString& version)
{
    return afterRecording(settings, version, recordStart(settings, version));
}

AtStart afterRecording(const QString& settings, const QString& version, const base::Result<Started>& started)
{
    if (started) {
        return {};
    }
    qWarning("qKVM: the settings were not kept before this version's first start: %s", started.error().message.c_str());
    // (Only a copy that could not be kept is owed; a record that could not be written is written at the next start,
    // and its copy, kept, is not made again where nothing was written since.)
    const QString writer = writerOf(settings, recorded(settings));
    if (started.error().code != copyNotKept || !QFileInfo(settings).isFile() || writer == version) {
        return {};
    }
    // Owed: kept before anything of them is written in this run, or nothing is written (Settings::owesACopy).
    return {QCoreApplication::translate("Profile",
                                        "qKVM could not keep a copy of its settings before its first start as %1 "
                                        "(%2). It writes nothing of them until it has kept one: it tries again "
                                        "before it writes anything, and at its next start.")
                .arg(version, QString::fromStdString(started.error().message)),
            writer.isEmpty() ? u"before-"_s + version : writer};
}

} // namespace qkvm::core::profile
