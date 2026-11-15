#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

#include <windows.h>

#include <qkvm/core/profile.h>
#include <qkvm/core/settings.h>

using namespace Qt::StringLiterals;
using namespace qkvm;

// The profile's way back (SPEC.md section 11.2): at a start of another version than the one that wrote the profile
// last, the settings are kept as that one left them, and only then is this version recorded. In a scratch folder.
namespace {

QByteArray bytesOf(const QString& file)
{
    QFile kept(file);
    return kept.open(QIODevice::ReadOnly) ? kept.readAll() : QByteArray();
}

bool write(const QString& file, const QByteArray& bytes)
{
    QFile into(file);
    return into.open(QIODevice::WriteOnly) && into.write(bytes) == bytes.size();
}

// What is recorded; a record of no version where nothing is.
core::profile::Record recordOf(const QString& settings)
{
    return core::profile::recorded(settings).value_or(core::profile::Record{u"(none)"_s, u"(none)"_s, std::nullopt});
}

QStringList filesIn(const QTemporaryDir& folder)
{
    return QDir(folder.path()).entryList(QDir::Files | QDir::Hidden, QDir::Name);
}

// Settings as a version of 0.1.2 or later writes them: with its name in them.
QByteArray writtenBy(const QString& version)
{
    return QJsonDocument(QJsonObject{{u"theme"_s, u"light"_s}, {u"written_by"_s, version}}).toJson();
}

// The copies of a kind in the folder, by name.
QStringList copiesIn(const QTemporaryDir& folder, bool resets)
{
    QStringList found;
    for (const QString& name : QDir(folder.path()).entryList({u"settings.json.*.bak"_s}, QDir::Files, QDir::Name)) {
        if (name.startsWith(u"settings.json.reset-"_s) == resets) {
            found.append(name);
        }
    }
    return found;
}

// A file made older than now by so many minutes.
bool ageBy(const QString& file, int minutes)
{
    QFile kept(file);
    return kept.open(QIODevice::ReadWrite) && kept.setFileTime(QDateTime::currentDateTimeUtc().addSecs(-60LL * minutes),
                                                               QFileDevice::FileModificationTime);
}

// (A settings file as a version wrote it.)
const QByteArray aWall =
    QByteArrayLiteral("{\"kvms\": [{\"id\": \"0a1b2c3d\", \"kind\": \"kvmd\", \"address\": \"https://192.0.2.10\"}]}");

} // namespace

class TestProfile : public QObject {
    Q_OBJECT

private slots:
    void aFirstStartRecordsItsVersionAndKeepsNoCopy()
    {
        const QTemporaryDir folder;
        const QString settings = folder.filePath(u"settings.json"_s);
        const auto started = core::profile::recordStart(settings, u"0.1.2"_s);
        QVERIFY(started);
        QCOMPARE(started.value().previous, QString());
        QCOMPARE(started.value().keptAs, QString());
        QCOMPARE(filesIn(folder), QStringList{u"settings.json.version"_s});
        QCOMPARE(recordOf(settings).version, u"0.1.2"_s);
        QCOMPARE(recordOf(settings).previous, QString());
    }

    void aProfileOfAVersionThatKeptNoRecordIsKeptAsItWasFound()
    {
        const QTemporaryDir folder;
        const QString settings = folder.filePath(u"settings.json"_s);
        QVERIFY(write(settings, aWall));
        const auto started = core::profile::recordStart(settings, u"0.1.2"_s);
        QVERIFY(started);
        QVERIFY(started.value().another);
        QCOMPARE(started.value().previous, QString());
        // (Named for the version that found it: one that kept no record is from before this one.)
        QCOMPARE(started.value().keptAs, settings + u".before-0.1.2.bak"_s);
        QCOMPARE(bytesOf(started.value().keptAs), aWall);
        QCOMPARE(bytesOf(settings), aWall);
        QCOMPARE(recordOf(settings).version, u"0.1.2"_s);
        // The same version again: nothing more.
        const auto again = core::profile::recordStart(settings, u"0.1.2"_s);
        QVERIFY(again);
        QVERIFY(!again.value().another);
        QCOMPARE(again.value().keptAs, QString());
        QCOMPARE(filesIn(folder),
                 (QStringList{u"settings.json"_s, u"settings.json.before-0.1.2.bak"_s, u"settings.json.version"_s}));
    }

    void anotherVersionKeepsTheSettingsAsTheOneBeforeLeftThem()
    {
        const QTemporaryDir folder;
        const QString settings = folder.filePath(u"settings.json"_s);
        QVERIFY(write(settings, aWall));
        QVERIFY(core::profile::recordStart(settings, u"0.1.2"_s));
        // (0.1.2 writes them, and says so in them.)
        const QByteArray later = writtenBy(u"0.1.2"_s);
        QVERIFY(write(settings, later));
        const auto newer = core::profile::recordStart(settings, u"0.1.3"_s);
        QVERIFY(newer);
        QVERIFY(newer.value().another);
        QCOMPARE(newer.value().previous, u"0.1.2"_s);
        QCOMPARE(newer.value().keptAs, settings + u".0.1.2.bak"_s);
        QCOMPARE(bytesOf(newer.value().keptAs), later);
        QCOMPARE(recordOf(settings).version, u"0.1.3"_s);
        QCOMPARE(recordOf(settings).previous, u"0.1.2"_s);
        // And back to the older one (an update undone), once the newer one wrote them: the newer one's are kept too.
        QVERIFY(write(settings, writtenBy(u"0.1.3"_s)));
        const auto older = core::profile::recordStart(settings, u"0.1.2"_s);
        QVERIFY(older);
        QCOMPARE(older.value().previous, u"0.1.3"_s);
        QCOMPARE(older.value().keptAs, settings + u".0.1.3.bak"_s);
    }

    void aNameThatIsTakenIsNumberedAndWhatHasItStaysAsItIs()
    {
        const QTemporaryDir folder;
        const QString settings = folder.filePath(u"settings.json"_s);
        QVERIFY(write(settings, aWall));
        QVERIFY(write(settings + u".before-0.1.2.bak"_s, "kept before"));
        const auto kept = core::profile::keepCopy(settings, core::profile::CopyKind::VersionChange, u"before-0.1.2"_s);
        QVERIFY(kept);
        QCOMPARE(kept.value(), settings + u".before-0.1.2-2.bak"_s);
        QCOMPARE(bytesOf(settings + u".before-0.1.2.bak"_s), QByteArray("kept before"));
        QCOMPARE(bytesOf(kept.value()), aWall);
        // No settings: nothing to keep.
        const auto none = core::profile::keepCopy(folder.filePath(u"none.json"_s),
                                                  core::profile::CopyKind::VersionChange, u"0.1.2"_s);
        QVERIFY(none);
        QCOMPARE(none.value(), QString());
    }

    void aCopyThatCannotBeMadeRecordsNothingAndChangesNothing()
    {
        const QTemporaryDir folder;
        const QString settings = folder.filePath(u"settings.json"_s);
        QVERIFY(write(settings, aWall));
        // Held by another program, which shares it with nobody: it cannot be read for the copy.
        const HANDLE held = CreateFileW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(settings).utf16()),
                                        GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(held != INVALID_HANDLE_VALUE);
        const auto started = core::profile::recordStart(settings, u"0.1.2"_s);
        CloseHandle(held);
        QVERIFY(!started);
        QCOMPARE(filesIn(folder), QStringList{u"settings.json"_s});
        QVERIFY(!core::profile::recorded(settings));
        // The next start keeps it.
        const auto next = core::profile::recordStart(settings, u"0.1.2"_s);
        QVERIFY(next);
        QCOMPARE(bytesOf(next.value().keptAs), aWall);
    }

    void aRecordThatCannotBeReadIsAsNone()
    {
        const QTemporaryDir folder;
        const QString settings = folder.filePath(u"settings.json"_s);
        QVERIFY(write(settings, aWall));
        // (Each wrong in one thing alone: the rest of it is as a record is.)
        for (const QByteArray& damaged : {QByteArray("0.1.2"), QByteArray(R"({"version": "../0.1.2", "previous": ""})"),
                                          QByteArray(R"({"version": "0.1.2", "previous": "..\\0.1.1"})"),
                                          QByteArray(R"({"version": 12, "previous": ""})"),
                                          QByteArray(R"({"version": "0.1.2"})"), QByteArray(R"([])")}) {
            QVERIFY(write(core::profile::versionFile(settings), damaged));
            QVERIFY2(!core::profile::recorded(settings), damaged.constData());
        }
        const auto started = core::profile::recordStart(settings, u"0.1.2"_s);
        QVERIFY(started);
        QCOMPARE(started.value().previous, QString());
        QCOMPARE(started.value().keptAs, settings + u".before-0.1.2.bak"_s);
    }

    void aVersionOfNoShapeIsRefusedAndWritesNothing()
    {
        const QTemporaryDir folder;
        const QString settings = folder.filePath(u"settings.json"_s);
        QVERIFY(write(settings, aWall));
        for (const QString& version : {QString(), u"../x"_s, u"0.1.2\\x"_s, u"0.1.2 x"_s, QString(33, u'1')}) {
            QVERIFY2(!core::profile::namesAVersion(version), qPrintable(version));
            QVERIFY(!core::profile::recordStart(settings, version));
            QVERIFY(!core::profile::keepCopy(settings, core::profile::CopyKind::VersionChange, version));
            QVERIFY(!core::profile::keepCopy(settings, core::profile::CopyKind::Reset, version));
            QCOMPARE(core::profile::nextCopy(settings, core::profile::CopyKind::Reset, version), QString());
        }
        QCOMPARE(filesIn(folder), QStringList{u"settings.json"_s});
        QVERIFY(core::profile::namesAVersion(u"0.1.2"_s));
        QVERIFY(core::profile::namesAVersion(u"0.2.0-beta.1+b5"_s));
        QVERIFY(core::profile::namesAVersion(u"before-0.1.2"_s));
    }

    void theRecordSaysTheVersionAndTheOneBeforeAndNothingElse()
    {
        const QTemporaryDir folder;
        const QString settings = folder.filePath(u"settings.json"_s);
        QVERIFY(write(settings, aWall));
        QVERIFY(core::profile::recordStart(settings, u"0.1.2"_s));
        QVERIFY(core::profile::recordStart(settings, u"0.1.3"_s));
        const QJsonObject said = QJsonDocument::fromJson(bytesOf(core::profile::versionFile(settings))).object();
        QCOMPARE(said.keys(), (QStringList{u"previous"_s, u"settings_time"_s, u"version"_s}));
        QCOMPARE(said[u"version"_s].toString(), u"0.1.3"_s);
        QCOMPARE(said[u"previous"_s].toString(), u"0.1.2"_s);
        QCOMPARE(said[u"settings_time"_s].toInteger(), QFileInfo(settings).lastModified().toMSecsSinceEpoch());
        QCOMPARE(core::profile::versionFile(settings), settings + u".version"_s);
    }

    // Review 012, L4 (a): each kind keeps its newest few, apart, and a full count never stops a copy.
    //   FAIL (before): copies are never taken away, a reset's shares the names of a version's, and past 20 none is
    //   made.
    void eachKindKeepsItsNewestFewApart()
    {
        const QTemporaryDir folder;
        const QString settings = folder.filePath(u"settings.json"_s);
        QVERIFY(write(settings, aWall));
        for (int number = 0; number < 5; ++number) {
            const auto kept =
                core::profile::keepCopy(settings, core::profile::CopyKind::VersionChange, u"0.1.%1"_s.arg(number));
            QVERIFY(kept);
            QVERIFY(ageBy(kept.value(), 10 - number));
        }
        QCOMPARE(copiesIn(folder, false), (QStringList{u"settings.json.0.1.2.bak"_s, u"settings.json.0.1.3.bak"_s,
                                                       u"settings.json.0.1.4.bak"_s}));
        QStringList resets;
        for (int number = 0; number < 5; ++number) {
            const auto kept = core::profile::keepCopy(settings, core::profile::CopyKind::Reset, u"0.1.2"_s);
            QVERIFY(kept);
            QVERIFY(QFileInfo(kept.value()).fileName().startsWith(u"settings.json.reset-0.1.2"_s));
            QCOMPARE(bytesOf(kept.value()), aWall);
            QVERIFY(ageBy(kept.value(), 10 - number));
            resets.append(QFileInfo(kept.value()).fileName());
        }
        // The newest three resets' copies, and the version changes' as they were.
        QCOMPARE(copiesIn(folder, true).size(), core::profile::copiesKept);
        for (const QString& name : resets.last(core::profile::copiesKept)) {
            QVERIFY2(copiesIn(folder, true).contains(name), qPrintable(name));
        }
        QCOMPARE(copiesIn(folder, false).size(), core::profile::copiesKept);
    }

    void aFullCountNeverStopsACopy()
    {
        const QTemporaryDir folder;
        const QString settings = folder.filePath(u"settings.json"_s);
        QVERIFY(write(settings, aWall));
        // Every numbered name taken, as a folder full of copies nobody could take away would have them.
        QVERIFY(write(settings + u".reset-0.1.2.bak"_s, "taken"));
        for (int number = 2; number <= 999; ++number) {
            QVERIFY(write(u"%1.reset-0.1.2-%2.bak"_s.arg(settings).arg(number), "taken"));
        }
        const QString next = core::profile::nextCopy(settings, core::profile::CopyKind::Reset, u"0.1.2"_s);
        QVERIFY(!next.isEmpty() && !QFileInfo::exists(next));
        const auto kept = core::profile::keepCopy(settings, core::profile::CopyKind::Reset, u"0.1.2"_s, next);
        QVERIFY2(kept, kept ? "" : kept.error().message.c_str());
        QCOMPARE(kept.value(), next);
        QCOMPARE(bytesOf(next), aWall);
    }

    // (b): the file the next copy goes to is the one it is written to, numbered where the first name is taken.
    void theNextCopyIsTheFileItGoesTo()
    {
        const QTemporaryDir folder;
        const QString settings = folder.filePath(u"settings.json"_s);
        QVERIFY(write(settings, aWall));
        QCOMPARE(core::profile::nextCopy(settings, core::profile::CopyKind::Reset, u"0.1.2"_s),
                 settings + u".reset-0.1.2.bak"_s);
        QVERIFY(write(settings + u".reset-0.1.2.bak"_s, "kept before"));
        const QString next = core::profile::nextCopy(settings, core::profile::CopyKind::Reset, u"0.1.2"_s);
        QCOMPARE(next, settings + u".reset-0.1.2-2.bak"_s);
        const auto kept = core::profile::keepCopy(settings, core::profile::CopyKind::Reset, u"0.1.2"_s, next);
        QVERIFY(kept);
        QCOMPARE(kept.value(), next);
        // A name given that is not of this kind and name is not taken for it.
        const auto other = core::profile::keepCopy(settings, core::profile::CopyKind::Reset, u"0.1.2"_s,
                                                   folder.filePath(u"elsewhere.json"_s));
        QVERIFY(other);
        QVERIFY(!QFileInfo::exists(folder.filePath(u"elsewhere.json"_s)));
        QCOMPARE(core::profile::nextCopy(settings, core::profile::CopyKind::VersionChange, u"0.1.2"_s),
                 settings + u".0.1.2.bak"_s);
    }

    // (d): a version from before 0.1.2 run in between, which leaves the record as it was, is seen by the settings it
    // wrote; one that only read them is not.
    void aVersionRunInBetweenIsSeenByTheSettingsItWrote()
    {
        const QTemporaryDir folder;
        const QString settings = folder.filePath(u"settings.json"_s);
        QVERIFY(write(settings, aWall));
        QVERIFY(core::profile::recordStart(settings, u"0.1.2"_s));
        // 0.1.2 wrote them, then 0.1.1 did (it writes no name, and leaves the record as it was).
        QVERIFY(write(settings, writtenBy(u"0.1.2"_s)));
        QVERIFY(core::profile::recordStart(settings, u"0.1.2"_s));
        QVERIFY(write(settings, aWall + "\n"));
        QVERIFY(ageBy(settings, -1));
        const auto back = core::profile::recordStart(settings, u"0.1.2"_s);
        QVERIFY(back);
        QVERIFY(back.value().another);
        QCOMPARE(QFileInfo(back.value().keptAs).fileName(), u"settings.json.before-0.1.2-2.bak"_s);
        QCOMPARE(bytesOf(back.value().keptAs), aWall + "\n");
        // Read and not written since: no copy.
        const auto read = core::profile::recordStart(settings, u"0.1.2"_s);
        QVERIFY(read && read.value().keptAs.isEmpty() && !read.value().another);
        // Written by this one: no copy either.
        QVERIFY(write(settings, writtenBy(u"0.1.2"_s)));
        const auto wrote = core::profile::recordStart(settings, u"0.1.2"_s);
        QVERIFY(wrote && wrote.value().keptAs.isEmpty());
        // Written by a newer one that started here before (and recorded itself): kept under its name.
        QVERIFY(write(settings, writtenBy(u"0.1.3"_s)));
        const auto newer = core::profile::recordStart(settings, u"0.1.2"_s);
        QVERIFY(newer);
        QCOMPARE(QFileInfo(newer.value().keptAs).fileName(), u"settings.json.0.1.3.bak"_s);
    }

    // The settings say which version wrote them, from 0.1.2 on (what a version before 0.1.2 drops when it writes them).
    void theSettingsSayWhichVersionWroteThem()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        QCoreApplication::setApplicationVersion(u"0.1.2"_s);
        const auto unset = qScopeGuard([] { QCoreApplication::setApplicationVersion({}); });
        core::Settings settings(file);
        QVERIFY(settings.remember(settings.remembered()));
        QCOMPARE(QJsonDocument::fromJson(bytesOf(file)).object()[u"written_by"_s].toString(), u"0.1.2"_s);
        // (And one that names no version a profile records writes none.)
        QCoreApplication::setApplicationVersion(u"../x"_s);
        QVERIFY(!core::Settings::documentOf(settings.remembered()).contains(u"written_by"_s));
    }

    // (c): where the start's copy could not be kept, the settings owe it: kept before anything is written, and nothing
    // written while it cannot be (each writing says why), so the settings stay as the version before left them; once
    // kept, the version is recorded, and the settings written.
    void settingsThatOweACopyKeepItBeforeTheyAreWritten()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        QCoreApplication::setApplicationVersion(u"0.1.2"_s);
        const auto unset = qScopeGuard([] { QCoreApplication::setApplicationVersion({}); });
        QVERIFY(write(file, QJsonDocument(QJsonObject{{u"theme"_s, u"dark"_s}}).toJson()));
        const QByteArray asFound = bytesOf(file);
        core::Settings settings(file);
        settings.owesACopy(u"0.1.1"_s);
        QCOMPARE(settings.owedCopy(), u"0.1.1"_s);
        // While the copy cannot be kept (another program lets nobody read the settings): remembered, not written.
        const HANDLE locked = CreateFileW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(file).utf16()),
                                          GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(locked != INVALID_HANDLE_VALUE);
        auto chosen = settings.remembered();
        chosen.tellsNews = false;
        const base::Result<void> said = settings.remember(chosen);
        CloseHandle(locked);
        QVERIFY(!said);
        QVERIFY2(QString::fromStdString(said.error().message).contains(u"until a copy of them is kept"_s),
                 said.error().message.c_str());
        QVERIFY(!settings.remembered().tellsNews);
        QVERIFY(settings.hasUnwritten());
        QCOMPARE(settings.owedCopy(), u"0.1.1"_s);
        QCOMPARE(bytesOf(file), asFound);
        QVERIFY(!QFileInfo::exists(core::profile::versionFile(file)));
        // Free again: the copy kept first, byte for byte, the version recorded, then the settings written.
        QVERIFY(settings.writeDown());
        QCOMPARE(bytesOf(file + u".0.1.1.bak"_s), asFound);
        QCOMPARE(settings.owedCopy(), QString());
        QCOMPARE(core::profile::recorded(file).value_or(core::profile::Record{}).version, u"0.1.2"_s);
        QVERIFY(!QJsonDocument::fromJson(bytesOf(file)).object()[u"tell_news"_s].toBool(true));
        // And it is kept once: the next writing makes no other copy.
        QVERIFY(settings.remember(settings.remembered()));
        QCOMPARE(QDir(folder.path()).entryList({u"settings.json.*.bak"_s}, QDir::Files).size(), 1);
    }

    // What a start does where its copy could not be kept: the person told, and the copy owed under the name it was to
    // have; nothing of that where it was kept, or where what failed was not the copy.
    void aStartWhoseCopyCannotBeKeptSaysSoAndOwesIt()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        QVERIFY(write(file, QJsonDocument(core::Settings::documentOf(core::Remembered{})).toJson()));
        // Held by another program that lets nobody read it: no copy, and owed (a version before 0.1.2 left them).
        const HANDLE locked = CreateFileW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(file).utf16()),
                                          GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(locked != INVALID_HANDLE_VALUE);
        const core::profile::AtStart unread = core::profile::atStart(file, u"0.1.2"_s);
        CloseHandle(locked);
        QVERIFY2(unread.said.contains(u"could not keep a copy"_s), qPrintable(unread.said));
        QVERIFY(unread.said.contains(u"writes nothing of them until it has kept one"_s));
        QCOMPARE(unread.owedCopy, u"before-0.1.2"_s);
        QVERIFY(!QFileInfo::exists(core::profile::versionFile(file)));
        // A copy that failed as a full disk fails it, of settings a version recorded: owed under that version's name.
        const qint64 time = QFileInfo(file).lastModified().toMSecsSinceEpoch();
        QVERIFY(write(core::profile::versionFile(file),
                      QJsonDocument(QJsonObject{{u"version"_s, u"0.1.1"_s},
                                                {u"previous"_s, QString()},
                                                {u"settings_time"_s, static_cast<double>(time)}})
                          .toJson()));
        const base::Result<core::profile::Started> failed =
            base::Error{core::profile::copyNotKept, "The disk is full."};
        const core::profile::AtStart full = core::profile::afterRecording(file, u"0.1.2"_s, failed);
        QVERIFY2(full.said.contains(u"The disk is full."_s), qPrintable(full.said));
        QCOMPARE(full.owedCopy, u"0.1.1"_s);
        // What failed was not the copy (the record, kept at the next start): nothing said, nothing owed.
        const base::Result<core::profile::Started> other = base::Error{"profile", "The record could not be written."};
        const core::profile::AtStart notTheCopy = core::profile::afterRecording(file, u"0.1.2"_s, other);
        QCOMPARE(notTheCopy.said, QString());
        QCOMPARE(notTheCopy.owedCopy, QString());
        // Where the copy is kept: nothing to say, nothing owed.
        const core::profile::AtStart fine = core::profile::atStart(file, u"0.1.2"_s);
        QCOMPARE(fine.said, QString());
        QCOMPARE(fine.owedCopy, QString());
    }

    // Settings that another program held while the start looked at them (no copy could be made of them, nor could
    // they be read) are never written over afterwards with no copy of them, once they are free and the program's own
    // Settings reads them: what a newer version put in them is kept, in them or in a copy (outside review of 0.1.2,
    // C2).
    void settingsHeldWhileTheStartLookedAreNeverWrittenOverWithoutACopy()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        QJsonObject newer = core::Settings::documentOf(core::Remembered{});
        newer.insert(u"a_newer_versions_key"_s, true);
        newer.insert(u"written_by"_s, u"0.2.0"_s);
        QVERIFY(write(file, QJsonDocument(newer).toJson()));
        const HANDLE locked = CreateFileW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(file).utf16()),
                                          GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(locked != INVALID_HANDLE_VALUE);
        const core::profile::AtStart at = core::profile::atStart(file, u"0.1.2"_s);
        CloseHandle(locked);
        QVERIFY2(at.said.contains(u"could not keep a copy"_s), qPrintable(at.said));
        // The program's own, as main.cpp makes it once the start has looked: the settings are free now.
        core::Settings settings(file);
        QVERIFY(!settings.wasUnreadable());
        if (!at.owedCopy.isEmpty()) {
            settings.owesACopy(at.owedCopy);
        }
        auto chosen = settings.remembered();
        chosen.theme = u"light"_s;
        static_cast<void>(settings.remember(chosen));
        const auto keeps = [](const QString& one) {
            return QJsonDocument::fromJson(bytesOf(one)).object().contains(u"a_newer_versions_key"_s);
        };
        const QFileInfoList copies = QDir(folder.path()).entryInfoList({u"settings.json.*.bak"_s}, QDir::Files);
        const bool copied = std::ranges::any_of(copies, [&](const QFileInfo& one) { return keeps(one.filePath()); });
        QVERIFY2(keeps(file) || copied, "a newer version's key was written over, with no copy of it");
    }

    // A copy that the start of another version could not keep (a full disk) is owed until it is kept: no writing of the
    // settings in between marks the change of version done without it, and a later start still keeps it (outside review
    // of 0.1.2, C9).
    void aCopyTheStartCouldNotKeepIsKeptBeforeTheSettingsAreWritten()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        QCoreApplication::setApplicationVersion(QString());
        const auto unset = qScopeGuard([] { QCoreApplication::setApplicationVersion({}); });
        // Settings as 0.1.1 left them, all of which this version keeps, and 0.1.1's record of them.
        QVERIFY(write(file, QJsonDocument(core::Settings::documentOf(core::Remembered{})).toJson()));
        const qint64 time = QFileInfo(file).lastModified().toMSecsSinceEpoch();
        QVERIFY(write(core::profile::versionFile(file),
                      QJsonDocument(QJsonObject{{u"version"_s, u"0.1.1"_s},
                                                {u"previous"_s, QString()},
                                                {u"settings_time"_s, static_cast<double>(time)}})
                          .toJson()));
        const QByteArray asFound = bytesOf(file);
        // The start of 0.1.2 could not keep its copy (as recordStart leaves it: nothing recorded).
        QCoreApplication::setApplicationVersion(u"0.1.2"_s);
        const core::profile::AtStart at = core::profile::afterRecording(
            file, u"0.1.2"_s, base::Error{core::profile::copyNotKept, "The disk is full."});
        QVERIFY2(at.said.contains(u"The disk is full."_s), qPrintable(at.said));
        core::Settings settings(file);
        if (!at.owedCopy.isEmpty()) {
            settings.owesACopy(at.owedCopy);
        }
        // Room again on the disk; a choice made.
        auto chosen = settings.remembered();
        chosen.theme = u"light"_s;
        static_cast<void>(settings.remember(chosen));
        // And the next start: the settings as 0.1.1 left them are kept somewhere, byte for byte.
        QVERIFY(core::profile::atStart(file, u"0.1.2"_s).said.isEmpty());
        const QFileInfoList copies = QDir(folder.path()).entryInfoList({u"settings.json.*.bak"_s}, QDir::Files);
        const bool kept = bytesOf(file) == asFound || std::ranges::any_of(copies, [&](const QFileInfo& one) {
                              return bytesOf(one.filePath()) == asFound;
                          });
        QVERIFY2(kept, "the settings as 0.1.1 left them were written over, and no copy of them was ever kept");
    }
};

QTEST_GUILESS_MAIN(TestProfile)
#include "test_profile.moc"
