#include <optional>
#include <string>
#include <thread>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include <qkvm/core/exchange.h>
#include <qkvm/core/server_types.h>
#include <qkvm/core/settings.h>

#include "exchange_by_hand.h"

#include <qt_windows.h>

using namespace Qt::StringLiterals;
using namespace qkvm;

namespace {

// A window as the settings hold one, said in one line: where it was, or nowhere yet, and of its
// wall the group, the order, the columns and whether the groups take turns.
core::WallWindow windowAt(const std::optional<core::WindowPlace>& place, const QString& group = {},
                          const QString& order = {}, int columns = 0, bool rotates = false)
{
    core::WallWindow window;
    window.place = place;
    window.group = group;
    window.order = order;
    window.columns = columns;
    window.rotates = rotates;
    return window;
}

// One that has not been anywhere yet.
core::WallWindow windowShowing(const QString& group, const QString& order = {}, int columns = 0)
{
    return windowAt(std::nullopt, group, order, columns);
}

// One of which only the columns are other than a new window's.
core::WallWindow windowIn(int columns)
{
    return windowAt(std::nullopt, {}, {}, columns);
}

// The certificate of the server's example files of a wall, as its bytes: one that Windows reads
// as a certificate, and nobody's.
QByteArray exampleCertificate()
{
    const QJsonObject file = QJsonDocument::fromJson(core::testing::exampleFile("plain.json")).object();
    const QString pem = file[u"devices"_s].toArray().first().toObject()[u"cert_pem"_s].toString();
    return QByteArray::fromBase64(
        QString(pem).remove(u"-----BEGIN CERTIFICATE-----"_s).remove(u"-----END CERTIFICATE-----"_s).toLatin1());
}

// A KVM of this PC's own that is known by these bytes' certificate, over HTTPS.
core::OwnKvm knownBy(const QByteArray& der, const QString& id = u"0a1b2c3d"_s)
{
    core::OwnKvm kvm;
    kvm.id = id;
    kvm.address = u"https://192.168.1.20"_s;
    kvm.knownByCertificate = true;
    kvm.certificate = QCryptographicHash::hash(der, QCryptographicHash::Sha256);
    return kvm;
}

// The same as the file has one, with a certificate itself beside it as `said`.
QJsonObject inTheFile(const core::OwnKvm& kvm, const QJsonValue& said)
{
    return QJsonObject{{u"id"_s, kvm.id},
                       {u"kind"_s, kvm.kind},
                       {u"address"_s, kvm.address},
                       {u"known_by_certificate"_s, kvm.knownByCertificate},
                       {u"certificate"_s, QString::fromLatin1(kvm.certificate.toHex())},
                       {u"asked"_s, kvm.askedWhatItIs},
                       {u"certificate_pem"_s, said}};
}

} // namespace

// What the program remembers between runs: one file, written whole.
class TestSettings : public QObject {
    Q_OBJECT

private:
    static void write(const QString& file, const QByteArray& content)
    {
        QFile made(file);
        QVERIFY(made.open(QIODevice::WriteOnly));
        QCOMPARE(made.write(content), content.size());
    }

    // Opens a file as a program does that lets nobody else in while it has it.
    static HANDLE holdAlone(const QString& file)
    {
        return CreateFileW(QDir::toNativeSeparators(file).toStdWString().c_str(), GENERIC_READ, 0, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    }

    static QByteArray read(const QString& file)
    {
        QFile kept(file);
        return kept.open(QIODevice::ReadOnly) ? kept.readAll() : QByteArray("(no such file)");
    }

    // A saved action that presses a key, as the file has one.
    static QJsonObject actionInTheFile(const QString& id, const QString& name)
    {
        return QJsonObject{{u"id"_s, id},       {u"name"_s, name},    {u"kind"_s, u"keys"_s}, {u"keys"_s, u"F1"_s},
                           {u"enter"_s, false}, {u"danger"_s, false}, {u"changed"_s, 0},      {u"writing"_s, u""_s}};
    }

private slots:
    void workspacesKeepWindowSourcesAndRejectUnsafeOrUnboundedInput()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath(u"settings.json"_s);
        core::Settings settings(path);
        auto saved = settings.remembered();
        auto local = windowIn(2);
        local.source = u"pc"_s;
        auto server = windowAt(core::WindowPlace{2040, 80, 900, 600, false, true}, u"Rack B"_s, u"name"_s, 4, true);
        server.source = u"https://example.test/"_s;
        saved.windows = {local, server};
        saved.workspaces = {{QString(32, u'a'), u"Office monitors"_s, {local, server}}};
        QVERIFY(settings.remember(saved));
        QCOMPARE(core::Settings(path).remembered(), saved);

        const auto entry = [](const QString& id, const QString& name, const QJsonArray& windows) {
            return QJsonObject{{u"id"_s, id}, {u"name"_s, name}, {u"windows"_s, windows}};
        };
        const QJsonArray layout{QJsonObject{{u"source"_s, u"https://user:secret@example.test/?display=key"_s}}};
        QJsonArray values{entry(QString(32, u'a'), u"Office"_s, layout),
                          entry(QString(32, u'b'), u" office "_s, layout), entry(u"bad-id"_s, u"Invalid"_s, layout),
                          entry(QString(32, u'c'), QString(81, u'n'), layout),
                          entry(QString(32, u'd'), u"Empty"_s, {})};
        for (int i = 0; i < 25; ++i)
            values.append(entry(QString::number(i, 16).rightJustified(32, u'0'), QString::number(i), layout));
        write(path, QJsonDocument(QJsonObject{{u"workspaces"_s, values}}).toJson());
        const auto parsed = core::Settings(path).remembered().workspaces;
        QCOMPARE(parsed.size(), 12); // First sixteen inputs only, four invalid entries.
        QCOMPARE(parsed.first().windows.first().source, u"connect"_s);
        QVERIFY(!read(path).isEmpty());
    }

    void shortcutReferencesRejectMalformedUnboundedAndDuplicateInput()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath(u"settings.json"_s);
        QJsonArray references;
        const auto entry = [](const QString& scope, const QString& machine) {
            return QJsonObject{{u"scope"_s, scope}, {u"machine"_s, machine}};
        };
        references.append(entry(u"pc"_s, u"a"_s));
        references.append(entry(u"pc"_s, u"a"_s));
        references.append(entry(u"https://private.test"_s, u"a"_s));
        references.append(entry(u"pc"_s, u"@source"_s));
        references.append(entry(u"pc"_s, QString(513, u'x')));
        references.append(42);
        references.append(entry(u"server-"_s + QString(64, u'a'), u"a"_s));
        for (int i = 0; i < 40; ++i)
            references.append(entry(u"pc"_s, QString::number(i)));
        write(path,
              QJsonDocument(QJsonObject{{u"favorites"_s, references}, {u"recent_machines"_s, references}}).toJson());
        const auto saved = core::Settings(path).remembered();
        QCOMPARE(saved.favorites.size(), 27);     // Only the first 32 inputs, five rejected.
        QCOMPARE(saved.recentMachines.size(), 5); // Only the first ten inputs.
        QCOMPARE(saved.favorites.front().machine, u"a"_s);
        QCOMPARE(saved.favorites.at(1).scope, u"server-"_s + QString(64, u'a'));
    }

    // b10: the size each machine was last shown at in its enlarged view is kept, by the same reference
    // to it as a favorite's; only a scale the page offers, once for each machine, and not without end.
    void pictureScalesAreKeptForEachMachineAsThePageOffersThem()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath(u"settings.json"_s);
        const QString server = u"server-"_s + QString(64, u'b');
        {
            core::Settings settings(path);
            core::Remembered what = settings.remembered();
            what.pictureScales = {{{u"pc"_s, u"a"_s}, u"140"_s}, {{server, u"m/1"_s}, u"50"_s}};
            QVERIFY(settings.remember(what).ok());
        }
        const auto kept = core::Settings(path).remembered().pictureScales;
        QCOMPARE(kept.size(), 2);
        QCOMPARE(kept[0].machine, (core::MachineReference{u"pc"_s, u"a"_s}));
        QCOMPARE(kept[0].scale, u"140"_s);
        QCOMPARE(kept[1].machine, (core::MachineReference{server, u"m/1"_s}));
        QCOMPARE(kept[1].scale, u"50"_s);
        const auto entry = [](const QString& scope, const QString& machine, const QJsonValue& scale) {
            return QJsonObject{{u"scope"_s, scope}, {u"machine"_s, machine}, {u"scale"_s, scale}};
        };
        QJsonArray scales{entry(u"pc"_s, u"a"_s, u"140"_s),     entry(u"pc"_s, u"a"_s, u"200"_s),
                          entry(u"pc"_s, u"b"_s, u"fit"_s),     entry(u"pc"_s, u"c"_s, u"150"_s),
                          entry(u"pc"_s, u"d"_s, 140),          entry(u"https://private.test"_s, u"a"_s, u"50"_s),
                          entry(u"pc"_s, u"@source"_s, u"50"_s)};
        for (int i = 0; i < 250; ++i) {
            scales.append(entry(u"pc"_s, u"e%1"_s.arg(i), u"75"_s));
        }
        write(path, QJsonDocument(QJsonObject{{u"picture_scales"_s, scales}}).toJson());
        const auto read = core::Settings(path).remembered().pictureScales;
        // Of the first 200 entries, six are none of that.
        QCOMPARE(read.size(), 194);
        QCOMPARE(read.front().scale, u"140"_s);
        QCOMPARE(read.at(1).machine.machine, u"e0"_s);
    }

    void savedServerAddressesAreBoundedCanonicalAndPersisted()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath(u"settings.json"_s);
        {
            core::Settings settings(path);
            auto saved = settings.remembered();
            saved.servers = {u"https://example.test/"_s, u"http://127.0.0.1:8788/"_s};
            QVERIFY(settings.remember(saved));
        }
        QCOMPARE(core::Settings(path).remembered().servers,
                 (QStringList{u"https://example.test/"_s, u"http://127.0.0.1:8788/"_s}));
        QJsonArray values;
        values.append(u"https://example.test/"_s);
        values.append(u"https://example.test/"_s);
        values.append(u"file:///private"_s);
        values.append(42);
        for (int i = 0; i < 40; ++i)
            values.append(u"https://host%1.test/"_s.arg(i));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(QJsonObject{{u"servers"_s, values}}).toJson());
        file.close();
        const auto saved = core::Settings(path).remembered();
        QCOMPARE(saved.servers.size(), 29); // First 32 inputs, one duplicate and two invalid.
        QVERIFY(!saved.servers.contains(u"https://host39.test/"_s));
    }

    void releaseKeyValuesAreBoundedAndSurviveRestart()
    {
        QTemporaryDir folder;
        const QString path = folder.filePath(u"settings.json"_s);
        for (const QString& key : {u"right-ctrl"_s, u"scroll-lock"_s, u"f12"_s}) {
            core::Settings settings(path);
            auto remembered = settings.remembered();
            remembered.releaseKey = key;
            QVERIFY(settings.remember(remembered));
            QCOMPARE(core::Settings(path).remembered().releaseKey, key);
        }
        for (const QJsonValue& value : {QJsonValue(u"invalid"_s), QJsonValue(69), QJsonValue(false), QJsonValue()}) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            const auto bytes = QJsonDocument(QJsonObject{{u"release_key"_s, value}}).toJson();
            QCOMPARE(file.write(bytes), bytes.size());
            file.close();
            QCOMPARE(core::Settings(path).remembered().releaseKey, u"right-ctrl"_s);
        }
    }

    // How video is decoded and on which graphics adapter: one of the words for each, and
    // otherwise what is chosen where nothing was.
    void videoChoicesAreBoundedAndSurviveRestart()
    {
        QTemporaryDir folder;
        const QString path = folder.filePath(u"settings.json"_s);
        QCOMPARE(core::Remembered{}.hardwareDecoding, u"automatic"_s);
        QCOMPARE(core::Remembered{}.graphicsCard, u"most-powerful"_s);
        for (const QString& decoding : {u"automatic"_s, u"on"_s, u"off"_s}) {
            for (const QString& card : {u"most-powerful"_s, u"power-saving"_s, u"windows"_s}) {
                core::Settings settings(path);
                auto remembered = settings.remembered();
                remembered.hardwareDecoding = decoding;
                remembered.graphicsCard = card;
                QVERIFY(settings.remember(remembered));
                QCOMPARE(core::Settings(path).remembered().hardwareDecoding, decoding);
                QCOMPARE(core::Settings(path).remembered().graphicsCard, card);
            }
        }
        for (const QJsonValue& value :
             {QJsonValue(u"gpu"_s), QJsonValue(u"On"_s), QJsonValue(1), QJsonValue(true), QJsonValue()}) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            const auto bytes =
                QJsonDocument(QJsonObject{{u"hardware_decoding"_s, value}, {u"graphics_card"_s, value}}).toJson();
            QCOMPARE(file.write(bytes), bytes.size());
            file.close();
            QCOMPARE(core::Settings(path).remembered().hardwareDecoding, u"automatic"_s);
            QCOMPARE(core::Settings(path).remembered().graphicsCard, u"most-powerful"_s);
        }
        // What is remembered may hold anything: only a word of the list is written.
        {
            core::Settings settings(path);
            auto remembered = settings.remembered();
            remembered.hardwareDecoding = u"fast"_s;
            remembered.graphicsCard = u"Some graphics card"_s;
            QVERIFY(settings.remember(remembered));
        }
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject written = QJsonDocument::fromJson(file.readAll()).object();
        QCOMPARE(written[u"hardware_decoding"_s].toString(), u"automatic"_s);
        QCOMPARE(written[u"graphics_card"_s].toString(), u"most-powerful"_s);
    }

    void withoutAFileNothingIsRemembered()
    {
        const QTemporaryDir folder;
        const core::Settings settings(folder.filePath(u"settings.json"_s));
        QCOMPARE(settings.remembered(), core::Remembered{});
    }

    void whatIsRememberedIsThereTheNextTime()
    {
        const QTemporaryDir folder;
        // In a folder that is not there yet, as on a PC that never ran the program.
        const QString file = folder.filePath(u"QKVM Desktop/settings.json"_s);
        core::Remembered what;
        what.server = u"http://192.168.1.10:8787/"_s;
        what.reachedEncrypted = {u"qkvm.example"_s};
        what.wallPictureEveryMs = 2000;
        // Two windows: one on a monitor to the left of the first one, maximized there, and one
        // that has not been anywhere yet.
        what.windows = {
            windowAt(core::WindowPlace{-1800, 40, 1280, 800, true, false}, u"Rack B"_s, u"state"_s, 4, true),
            windowShowing(u"Office"_s)};
        what.theme = u"light"_s;
        what.staysInTray = true;
        what.tellsNews = false;
        what.settingsSection = u"files"_s;
        what.certificates.insert(u"qkvm.example:8443"_s, QByteArray(32, '\x5a'));
        what.certificates.insert(u"192.168.1.10:8443"_s, QByteArray::fromHex(QByteArray(64, 'f')));
        {
            core::Settings settings(file);
            QVERIFY(settings.remember(what).ok());
            QCOMPARE(settings.remembered(), what);
        }
        const core::Settings again(file);
        QCOMPARE(again.remembered(), what);
    }

    void theOldFileIsReplacedWholeAndNothingIsLeftBesideIt()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        core::Settings settings(file);
        core::Remembered what;
        what.server = u"http://192.168.1.10:8787/"_s;
        QVERIFY(settings.remember(what).ok());
        what.server = u"https://qkvm.example/"_s;
        QVERIFY(settings.remember(what).ok());
        QCOMPARE(core::Settings(file).remembered().server, u"https://qkvm.example/"_s);
        QCOMPARE(QDir(folder.path()).entryList(QDir::Files), QStringList{u"settings.json"_s});
    }

    void aFileThatIsNotTheProgramsCountsAsNone_data()
    {
        QTest::addColumn<QByteArray>("content");
        // Whether it is a file that could not be read at all: one that is the program's in its
        // shape, with something in it that the program cannot use, is read, and that is left out.
        QTest::addColumn<bool>("unreadable");
        QTest::newRow("empty") << QByteArray() << true;
        QTest::newRow("not JSON") << QByteArray("server = http://192.168.1.10:8787/") << true;
        // (No address is written out inside a raw string in this file: the two strokes in it end the
        // line for the tool that reads these tests before the compiler does.)
        QTest::newRow("a list where an object belongs")
            << QJsonDocument(QJsonArray{u"http://192.168.1.10:8787/"_s}).toJson() << true;
        QTest::newRow("a number where the address belongs") << QByteArray(R"({"server": 8787})") << false;
        QTest::newRow("an address longer than any is")
            << QJsonDocument(QJsonObject{{u"server"_s, QString(u"http://"_s + QString(5000, u'a') + u'/')}}).toJson()
            << false;
        // In every other way a file of the program's, with sixty-five megabytes of something else
        // in it. (The largest the program writes has some forty-three: theLargestWallThereIs...)
        QTest::newRow("larger than the program writes")
            << QJsonDocument(QJsonObject{{u"server"_s, u"http://192.168.1.10:8787/"_s},
                                         {u"more"_s, QString(qsizetype{65} * 1024 * 1024, u'a')}})
                   .toJson()
            << true;
    }

    void aFileThatIsNotTheProgramsCountsAsNone()
    {
        QFETCH(QByteArray, content);
        QFETCH(bool, unreadable);
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        write(file, content);
        const core::Settings settings(file);
        QCOMPARE(settings.remembered(), core::Remembered{});
        QCOMPARE(settings.wasUnreadable(), unreadable);
    }

    void aFileThatCouldNotBeReadIsPutAsideAndNeverWrittenOver()
    {
        // The list of this PC's own KVMs is in that file: one damaged place is not to cost it.
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        // (Cut off in the middle, as a file is that was not written whole.)
        const QByteArray damaged =
            QJsonDocument(QJsonObject{{u"kvms"_s, QJsonArray{QJsonObject{{u"address"_s, u"https://192.168.1.20"_s}}}}})
                .toJson()
                .chopped(12);
        write(file, damaged);
        core::Settings settings(file);
        QVERIFY(settings.wasUnreadable());
        QCOMPARE(settings.putAside(), QString());
        // Nothing is touched for as long as nothing is written.
        QCOMPARE(QDir(folder.path()).entryList(QDir::Files), QStringList{u"settings.json"_s});
        QCOMPARE(read(file), damaged);

        core::Remembered what;
        what.theme = u"light"_s;
        QVERIFY(settings.remember(what).ok());
        const QString aside = folder.filePath(u"settings.unreadable.json"_s);
        QCOMPARE(settings.putAside(), aside);
        QCOMPARE(read(aside), damaged);
        QCOMPARE(core::Settings(file).remembered(), what);
        // Once: what is written after that is written as ever.
        what.theme = u"dark"_s;
        QVERIFY(settings.remember(what).ok());
        QCOMPARE(QDir(folder.path()).entryList(QDir::Files, QDir::Name),
                 (QStringList{u"settings.json"_s, u"settings.unreadable.json"_s}));
        QCOMPARE(read(aside), damaged);

        // A second time it happens: beside the first, which stays what it is.
        const QByteArray other = "not this program's at all";
        write(file, other);
        core::Settings again(file);
        QVERIFY(again.wasUnreadable());
        QVERIFY(again.remember(what).ok());
        QCOMPARE(again.putAside(), folder.filePath(u"settings.unreadable-2.json"_s));
        QCOMPARE(read(again.putAside()), other);
        QCOMPARE(read(aside), damaged);
        QCOMPARE(core::Settings(file).remembered(), what);

        // A file that is read is nothing of the kind, and neither is none.
        QVERIFY(!core::Settings(file).wasUnreadable());
        QVERIFY(!core::Settings(folder.filePath(u"none.json"_s)).wasUnreadable());
    }

    void whereAnUnreadableFileCannotBePutAsideNothingIsWritten()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        const QByteArray damaged = "{";
        write(file, damaged);
        core::Settings settings(file);
        QVERIFY(settings.wasUnreadable());
        core::Remembered what;
        what.theme = u"light"_s;
        {
            // Something else has the file open: Windows moves no file that is.
            QFile held(file);
            QVERIFY(held.open(QIODevice::ReadOnly));
            const base::Result<void> written = settings.remember(what);
            QVERIFY(!written.ok());
            QCOMPARE(written.error().code, std::string("settings"));
            QCOMPARE(settings.putAside(), QString());
        }
        QCOMPARE(read(file), damaged);
        QCOMPARE(QDir(folder.path()).entryList(QDir::Files), QStringList{u"settings.json"_s});
        // It is remembered for this run all the same, and written the next time it can be.
        QCOMPARE(settings.remembered(), what);
        QVERIFY(settings.remember(what).ok());
        QCOMPARE(read(folder.filePath(u"settings.unreadable.json"_s)), damaged);
        QCOMPARE(core::Settings(file).remembered(), what);
    }

    void aFileThatSomethingElseHoldsForAMomentIsReadAllTheSame()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        core::Remembered what;
        what.theme = u"light"_s;
        {
            core::Settings settings(file);
            QVERIFY(settings.remember(what).ok());
        }
        const QByteArray written = read(file);
        // Held as a program holds a file that lets nobody else in (one that looks for viruses, one
        // that makes copies), and let go a moment later.
        {
            const HANDLE held = holdAlone(file);
            QVERIFY(held != INVALID_HANDLE_VALUE);
            const std::jthread lettingGo([held] {
                Sleep(120);
                CloseHandle(held);
            });
            const core::Settings settings(file);
            QVERIFY(!settings.wasUnreadable());
            QCOMPARE(settings.remembered(), what);
        }
        // Held for longer than is waited for: nothing is read, and nothing is written over it.
        const HANDLE held = holdAlone(file);
        QVERIFY(held != INVALID_HANDLE_VALUE);
        core::Settings settings(file, core::Settings::Patience{3, 20});
        QVERIFY(settings.wasUnreadable());
        QCOMPARE(settings.remembered(), core::Remembered{});
        core::Remembered other;
        other.theme = u"dark"_s;
        QVERIFY(!settings.remember(other).ok());
        // Nor is it copied in the place of a move, to any of the names it might be put aside under.
        QVERIFY(!settings.putAsideWhatCouldNotBeRead().ok());
        QCOMPARE(settings.putAside(), QString());
        QCOMPARE(QDir(folder.path()).entryList(QDir::Files), QStringList{u"settings.json"_s});
        CloseHandle(held);
        QCOMPARE(read(file), written);
        // Once it is let go, what is written goes beside it, and it is kept as it was.
        QVERIFY(settings.remember(other).ok());
        QCOMPARE(read(settings.putAside()), written);
        QCOMPARE(core::Settings(file).remembered(), other);
    }

    void whereAsManyWerePutAsideAsAreNothingMoreIsWrittenOver()
    {
        // A PC on which every start finds an unreadable file has another trouble, and is not
        // filled with them: after twenty, the next one stays where it is, and nothing is written.
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        write(folder.filePath(u"settings.unreadable.json"_s), "an older one");
        for (int number = 2; number <= 20; ++number) {
            write(folder.filePath(u"settings.unreadable-%1.json"_s.arg(number)), "an older one");
        }
        const QByteArray damaged = "{";
        write(file, damaged);
        core::Settings settings(file);
        QVERIFY(settings.wasUnreadable());
        core::Remembered what;
        what.theme = u"light"_s;
        const base::Result<void> written = settings.remember(what);
        QVERIFY(!written.ok());
        QCOMPARE(written.error().code, std::string("settings"));
        QCOMPARE(read(file), damaged);
        QCOMPARE(settings.putAside(), QString());
        QCOMPARE(QDir(folder.path()).entryList(QDir::Files).size(), 21);
        QCOMPARE(read(folder.filePath(u"settings.unreadable-20.json"_s)), "an older one"_ba);
    }

    void whoeverStartsTheProgramPutsAnUnreadableFileAsideAtOnce()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        const QByteArray damaged = "{";
        write(file, damaged);
        core::Settings settings(file);
        QVERIFY(settings.wasUnreadable());
        QVERIFY(settings.putAsideWhatCouldNotBeRead().ok());
        QCOMPARE(settings.putAside(), folder.filePath(u"settings.unreadable.json"_s));
        QCOMPARE(read(settings.putAside()), damaged);
        QVERIFY(!QFile::exists(file));
        // Once: there is nothing in the way any more.
        QVERIFY(settings.putAsideWhatCouldNotBeRead().ok());
        QCOMPARE(QDir(folder.path()).entryList(QDir::Files), QStringList{u"settings.unreadable.json"_s});
        core::Remembered what;
        what.theme = u"light"_s;
        QVERIFY(settings.remember(what).ok());
        QCOMPARE(core::Settings(file).remembered(), what);

        // A file that somebody took away meanwhile is in nobody's way: what is remembered is
        // written, and nothing is said to have been put anywhere.
        const QString other = folder.filePath(u"other.json"_s);
        write(other, damaged);
        core::Settings second(other);
        QVERIFY(second.wasUnreadable());
        QVERIFY(QFile::remove(other));
        QVERIFY(second.remember(what).ok());
        QCOMPARE(second.putAside(), QString());
        QCOMPARE(core::Settings(other).remembered(), what);
        // And one that was read, or was never there, is none to put aside.
        core::Settings fine(file);
        QVERIFY(fine.putAsideWhatCouldNotBeRead().ok());
        QCOMPARE(fine.putAside(), QString());
        QVERIFY(QFile::exists(file));
    }

    void theLargestWallThereIsIsReadBackWhole()
    {
        // As many KVMs as a wall holds, each with the longest of everything, in letters that take
        // the most room in a file; and beside each the longest certificate that a file of the wall
        // holds, which is the longest that is kept (20,000 signs as it is kept, in lines of 64).
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        const QString wide(QChar(0x4E2D));
        const core::exchange::KeptCertificate longest =
            core::exchange::certificateOf(core::testing::certificateOfSoManyBytes(exampleCertificate(), 14'700))
                .value_or(core::exchange::KeptCertificate{});
        QVERIFY(longest.pem.size() > 19'900 && longest.pem.size() <= core::exchange::longestCertificate);
        core::Remembered what;
        for (int number = 0; number < core::Remembered::mostKvms; ++number) {
            core::OwnKvm kvm;
            kvm.id = u"%1"_s.arg(number, 8, 16, QLatin1Char('0'));
            kvm.name = wide.repeated(80);
            kvm.group = wide.repeated(40);
            kvm.user = wide.repeated(64);
            const QString label = u"a"_s.repeated(60);
            kvm.address = u"https://%1.%1.%1.example:%2"_s.arg(label).arg(1024 + number);
            kvm.knownByCertificate = true;
            kvm.certificate = longest.sha256;
            kvm.askedWhatItIs = number % 2 == 0;
            what.kvms.append(kvm);
            what.kvmCertificates.insert(kvm.id, longest.pem);
        }
        {
            core::Settings settings(file);
            QVERIFY(settings.remember(what).ok());
        }
        QVERIFY(QFileInfo(file).size() > qint64{40} * 1024 * 1024);
        const core::Settings settings(file);
        QVERIFY(!settings.wasUnreadable());
        QCOMPARE(settings.remembered().kvms.size(), core::Remembered::mostKvms);
        QCOMPARE(settings.remembered(), what);
    }

    void ofAListOnlyWhatCanBeANameIsTaken()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        const QJsonArray names{u"qkvm.example"_s, 7, QString(), QJsonValue(), u"other.example"_s};
        write(file,
              QJsonDocument(QJsonObject{{u"server"_s, u"https://qkvm.example/"_s}, {u"reached_encrypted"_s, names}})
                  .toJson());
        const core::Settings settings(file);
        QCOMPARE(settings.remembered().server, u"https://qkvm.example/"_s);
        QCOMPARE(settings.remembered().reachedEncrypted, (QStringList{u"qkvm.example"_s, u"other.example"_s}));
    }

    void onlyAWholeSha256IsTakenForACertificate()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        const QString whole(64, u'a');
        const QJsonObject certificates{
            {u"good.example:8443"_s, whole},
            {u"capitals.example:8443"_s, QString(64, u'A')},
            {u"short.example:8443"_s, u"abcd"_s},
            {u"long.example:8443"_s, QString(66, u'a')},
            {u"letters.example:8443"_s, QString(64, u'z')},
            {u"number.example:8443"_s, 7},
            {QString(), whole},
        };
        write(file, QJsonDocument(QJsonObject{{u"certificates"_s, certificates}}).toJson());
        const core::Settings settings(file);
        QCOMPARE(settings.remembered().certificates.size(), 2);
        QCOMPARE(settings.remembered().certificates.value(u"good.example:8443"_s), QByteArray(32, '\xaa'));
        QCOMPARE(settings.remembered().certificates.value(u"capitals.example:8443"_s), QByteArray(32, '\xaa'));
    }

    void ofHowTheWallIsShownOnlyWhatCanBeSoIsTaken()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        // The first window of what a file says of the program's windows.
        const auto kept = [&](const QJsonObject& window) {
            write(file, QJsonDocument(QJsonObject{{u"windows"_s, QJsonArray{window}}}).toJson());
            const QList<core::WallWindow> windows = core::Settings(file).remembered().windows;
            return windows.size() == 1 ? windows.constFirst() : windowIn(-1);
        };
        core::WallWindow window =
            kept(QJsonObject{{u"group"_s, u"Rack A"_s}, {u"order"_s, u"by colour"_s}, {u"columns"_s, 60}});
        QCOMPARE(window.group, u"Rack A"_s);
        QCOMPARE(window.order, QString());
        QCOMPARE(window.columns, 0);
        window = kept(QJsonObject{{u"group"_s, 7}, {u"order"_s, u"group"_s}, {u"columns"_s, u"three"_s}});
        QCOMPARE(window.group, QString());
        QCOMPARE(window.order, u"group"_s);
        QCOMPARE(window.columns, 0);

        // How often a picture is asked for: a number of milliseconds a wall could be asked at, or
        // not chosen. Never nothing at all, which would be a wall that stands still for no reason
        // anybody can see.
        for (const QJsonValue& every : {QJsonValue(500), QJsonValue(10000), QJsonValue(100), QJsonValue(600000)}) {
            write(file, QJsonDocument(QJsonObject{{u"picture_every_ms"_s, every}}).toJson());
            QCOMPARE(core::Settings(file).remembered().wallPictureEveryMs, every.toInt());
        }
        // Whether the groups take turns: yes where it says so, and otherwise no.
        for (const QJsonValue& turns : {QJsonValue(1), QJsonValue(u"true"_s), QJsonValue(false), QJsonValue()}) {
            QVERIFY(!kept(QJsonObject{{u"rotate"_s, turns}}).rotates);
        }
        QVERIFY(kept(QJsonObject{{u"rotate"_s, true}}).rotates);
        // Whether the program goes on when its window is closed: the same.
        for (const QJsonValue& stays : {QJsonValue(1), QJsonValue(u"true"_s), QJsonValue(false), QJsonValue()}) {
            write(file, QJsonDocument(QJsonObject{{u"stay_in_tray"_s, stays}}).toJson());
            QVERIFY(!core::Settings(file).remembered().staysInTray);
        }
        write(file, QJsonDocument(QJsonObject{{u"stay_in_tray"_s, true}}).toJson());
        QVERIFY(core::Settings(file).remembered().staysInTray);
        // Whether Windows shows messages of the wall: no only where it says so, and otherwise yes,
        // as on a PC where nothing was chosen.
        for (const QJsonValue& tells : {QJsonValue(0), QJsonValue(u"false"_s), QJsonValue(true), QJsonValue()}) {
            write(file, QJsonDocument(QJsonObject{{u"tell_news"_s, tells}}).toJson());
            QVERIFY(core::Settings(file).remembered().tellsNews);
        }
        write(file, QJsonDocument(QJsonObject{{u"tell_news"_s, false}}).toJson());
        QVERIFY(!core::Settings(file).remembered().tellsNews);
        for (const QJsonValue& every : {QJsonValue(0), QJsonValue(-500), QJsonValue(99), QJsonValue(600001),
                                        QJsonValue(u"1000"_s), QJsonValue(true), QJsonValue()}) {
            write(file, QJsonDocument(QJsonObject{{u"picture_every_ms"_s, every}}).toJson());
            QCOMPARE(core::Settings(file).remembered().wallPictureEveryMs, 0);
        }
    }

    void whereTheWindowWasIsAPlaceOrNothing()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        // The place of the one window a file tells of.
        const auto kept = [&](const QJsonValue& place) {
            write(file,
                  QJsonDocument(QJsonObject{{u"windows"_s, QJsonArray{QJsonObject{{u"place"_s, place}}}}}).toJson());
            return core::Settings(file).remembered().windows.value(0).place;
        };
        const QJsonObject whole{{u"x"_s, 2000}, {u"y"_s, -30}, {u"width"_s, 900}, {u"height"_s, 650}};
        QCOMPARE(kept(whole), (core::WindowPlace{2000, -30, 900, 650, false, false}));
        QJsonObject over = whole;
        over[u"maximized"_s] = true;
        over[u"full_screen"_s] = true;
        QCOMPARE(kept(over), (core::WindowPlace{2000, -30, 900, 650, true, true}));
        // (Yes where it says so in so many words, and otherwise no.)
        over[u"maximized"_s] = 1;
        over[u"full_screen"_s] = u"true"_s;
        QCOMPARE(kept(over), (core::WindowPlace{2000, -30, 900, 650, false, false}));

        // Half a place is no place; nor is one no window could have.
        for (const QString& missing : {u"x"_s, u"y"_s, u"width"_s, u"height"_s}) {
            QJsonObject half = whole;
            half.remove(missing);
            QVERIFY2(!kept(half).has_value(), qPrintable(missing));
            half[missing] = u"900"_s;
            QVERIFY2(!kept(half).has_value(), qPrintable(missing));
            half[missing] = 900.5;
            QVERIFY2(!kept(half).has_value(), qPrintable(missing));
        }
        for (const auto& [name, value] :
             {std::pair{u"width"_s, 99}, std::pair{u"height"_s, 0}, std::pair{u"width"_s, -900},
              std::pair{u"height"_s, 32001}, std::pair{u"x"_s, 100001}, std::pair{u"y"_s, -100001}}) {
            QJsonObject wrong = whole;
            wrong[name] = value;
            QVERIFY2(!kept(wrong).has_value(), qPrintable(name));
        }
        QVERIFY(!kept(u"where it was"_s).has_value());
        // A window without a place is a window all the same.
        QCOMPARE(core::Settings(file).remembered().windows.size(), 1);
        // No place remembered: none is written either.
        core::Settings settings(file);
        core::Remembered nowhere;
        nowhere.windows = {core::WallWindow{}};
        QVERIFY(settings.remember(nowhere).ok());
        QFile written(file);
        QVERIFY(written.open(QIODevice::ReadOnly));
        QVERIFY(!written.readAll().contains("place"));
    }

    void asManyWindowsAreRememberedAsWereOpenAndNoMoreThanTheProgramOpens()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        // What is no window at all is a window as a new one is: how many there were is kept.
        write(file, QJsonDocument(QJsonObject{{u"windows"_s, QJsonArray{QJsonObject{{u"columns"_s, 2}}, 7,
                                                                        QJsonObject{{u"columns"_s, 3}}}}})
                        .toJson());
        QList<core::WallWindow> windows = core::Settings(file).remembered().windows;
        QCOMPARE(windows.size(), 3);
        QCOMPARE(windows.at(0).columns, 2);
        QCOMPARE(windows.at(1), core::WallWindow{});
        QCOMPARE(windows.at(2).columns, 3);
        // Not a list: no windows.
        write(file, QJsonDocument(QJsonObject{{u"windows"_s, QJsonObject{{u"columns"_s, 2}}}}).toJson());
        QVERIFY(core::Settings(file).remembered().windows.isEmpty());

        // A file that tells of a hundred: the first of them, as many as the program opens.
        QJsonArray hundred;
        for (int columns = 0; columns < 100; ++columns) {
            hundred.append(QJsonObject{{u"columns"_s, columns % 7}});
        }
        write(file, QJsonDocument(QJsonObject{{u"windows"_s, hundred}}).toJson());
        windows = core::Settings(file).remembered().windows;
        QCOMPARE(windows.size(), core::Remembered::mostWindows);
        QCOMPARE(windows.constLast().columns, static_cast<int>((core::Remembered::mostWindows - 1) % 7));
        // And no more are written than that.
        core::Settings settings(file);
        core::Remembered many;
        many.windows = QList<core::WallWindow>(100, windowIn(5));
        QVERIFY(settings.remember(many).ok());
        QCOMPARE(core::Settings(file).remembered().windows.size(), core::Remembered::mostWindows);
        QFile written(file);
        QVERIFY(written.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(written.readAll()).object()[u"windows"_s].toArray().size(),
                 core::Remembered::mostWindows);
    }

    void aWindowHasItsOwnPartOfWhatIsRemembered()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        core::Settings settings(file);
        core::Remembered before;
        before.server = u"http://192.168.1.10:8787/"_s;
        QVERIFY(settings.remember(before).ok());

        core::WindowSlot first(settings, 0);
        core::WindowSlot third(settings, 2);
        // Nothing is remembered of either yet: each is a window as a new one is.
        QCOMPARE(first.remembered(), core::WallWindow{});
        QCOMPARE(third.remembered(), core::WallWindow{});
        // The third says something: the two before it are windows as new ones are.
        QVERIFY(third.remember(windowShowing(u"Rack B"_s, {}, 3)).ok());
        QCOMPARE(core::Settings(file).remembered().windows.size(), 3);
        QCOMPARE(core::Settings(file).remembered().windows.at(2).group, u"Rack B"_s);
        QCOMPARE(core::Settings(file).remembered().windows.at(0), core::WallWindow{});
        // What else is remembered stays.
        QCOMPARE(core::Settings(file).remembered().server, u"http://192.168.1.10:8787/"_s);
        // The first says something: the third's is untouched.
        QVERIFY(first.remember(windowShowing({}, u"name"_s)).ok());
        QCOMPARE(core::Settings(file).remembered().windows.at(0).order, u"name"_s);
        QCOMPARE(core::Settings(file).remembered().windows.at(2).columns, 3);
        QCOMPARE(third.remembered().columns, 3);

        // The window before it was closed, and whoever keeps the windows took it off the list: the
        // third is the second from now on, and finds its own there.
        core::Remembered fewer = settings.remembered();
        fewer.windows.removeAt(1);
        QVERIFY(settings.remember(fewer).ok());
        third.moveTo(1);
        QCOMPARE(third.index(), 1);
        QCOMPARE(third.remembered().group, u"Rack B"_s);
        QVERIFY(third.remember(windowShowing(u"Rack B"_s, {}, 4)).ok());
        QCOMPARE(core::Settings(file).remembered().windows.size(), 2);
        QCOMPARE(core::Settings(file).remembered().windows.at(1).columns, 4);

        // What is as it was is not written again: a file that is gone stays gone.
        QVERIFY(QFile::remove(file));
        QVERIFY(third.remember(third.remembered()).ok());
        QVERIFY(!QFile::exists(file));
        // And one window more than the program opens is not remembered.
        core::WindowSlot beyond(settings, core::Remembered::mostWindows);
        QVERIFY(beyond.remember(windowIn(2)).ok());
        QCOMPARE(settings.remembered().windows.size(), 2);
        QVERIFY(!QFile::exists(file));
    }

    void aThemeIsOneOfThreeOrNotChosen()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        for (const QString& theme : {u"dark"_s, u"light"_s, u"system"_s}) {
            write(file, QJsonDocument(QJsonObject{{u"theme"_s, theme}}).toJson());
            QCOMPARE(core::Settings(file).remembered().theme, theme);
        }
        for (const QJsonValue& other : {QJsonValue(u"pink"_s), QJsonValue(u"Dark"_s), QJsonValue(7), QJsonValue()}) {
            write(file, QJsonDocument(QJsonObject{{u"theme"_s, other}}).toJson());
            QCOMPARE(core::Settings(file).remembered().theme, QString());
        }
    }

    void theSectionOfTheSettingsShownLastIsOneOfTheirsOrNone()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        // None yet, where nothing was written.
        QCOMPARE(core::Settings(file).remembered().settingsSection, QString());
        // Written, and read back: each of the box's sections.
        for (const QString& section : {u"kvms"_s, u"files"_s, u"general"_s, u"video"_s, u"updates"_s, u"about"_s}) {
            QVERIFY(core::Remembered::namesASettingsSection(section));
            {
                core::Settings settings(file);
                core::Remembered what = settings.remembered();
                what.settingsSection = section;
                QVERIFY(settings.remember(what).ok());
            }
            QCOMPARE(core::Settings(file).remembered().settingsSection, section);
        }
        // In the file, what names no section is as none: another word, another way of writing one
        // of theirs, what is no word at all.
        for (const QJsonValue& other :
             {QJsonValue(u"printers"_s), QJsonValue(u"KVMs"_s), QJsonValue(u" kvms"_s), QJsonValue(u""_s),
              QJsonValue(3), QJsonValue(true), QJsonValue(QJsonArray{u"kvms"_s}), QJsonValue()}) {
            write(file, QJsonDocument(QJsonObject{{u"settings_section"_s, other}, {u"theme"_s, u"light"_s}}).toJson());
            const core::Settings settings(file);
            QVERIFY(!settings.wasUnreadable());
            QCOMPARE(settings.remembered().settingsSection, QString());
            // (And the rest of the file is read as it is.)
            QCOMPARE(settings.remembered().theme, u"light"_s);
        }
        // Nor is what names none written down, whatever is remembered.
        {
            core::Settings settings(file);
            core::Remembered what;
            what.settingsSection = u"printers"_s;
            QVERIFY(settings.remember(what).ok());
        }
        QCOMPARE(QJsonDocument::fromJson(read(file)).object()[u"settings_section"_s].toString(), QString());
        QCOMPARE(core::Settings(file).remembered().settingsSection, QString());
    }

    void whatCannotBeWrittenIsToldAndStillRemembered()
    {
        const QTemporaryDir folder;
        // Where the file should be, there is a folder.
        const QString file = folder.filePath(u"settings.json"_s);
        QVERIFY(QDir().mkpath(file));
        core::Settings settings(file);
        core::Remembered what;
        what.server = u"http://192.168.1.10:8787/"_s;
        const base::Result<void> written = settings.remember(what);
        QVERIFY(!written.ok());
        QCOMPARE(written.error().code, std::string("settings"));
        QCOMPARE(settings.remembered(), what);
    }

    void whichWallWasShownLastIsRemembered()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        QVERIFY(!core::Settings(file).remembered().showsOwnWall);
        {
            core::Settings settings(file);
            core::Remembered what;
            what.showsOwnWall = true;
            QVERIFY(settings.remember(what).ok());
        }
        QVERIFY(core::Settings(file).remembered().showsOwnWall);
    }

    void thisPcsOwnKvmsAreRememberedInTheirOrder()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        core::OwnKvm first;
        first.id = u"0a1b2c3d"_s;
        first.name = u"Build server"_s;
        first.address = u"https://192.168.1.20"_s;
        first.group = u"Rack 2"_s;
        first.knownByCertificate = true;
        first.certificate = QByteArray(32, '\x5a');
        core::OwnKvm second;
        second.id = u"ffffffff"_s;
        second.address = u"http://kvm.lan:8080"_s;
        second.user = u"operator"_s;
        {
            core::Settings settings(file);
            core::Remembered what;
            what.kvms = {first, second};
            QVERIFY(settings.remember(what).ok());
        }
        const core::Settings settings(file);
        QCOMPARE(settings.remembered().kvms, (QList<core::OwnKvm>{first, second}));
        QCOMPARE(settings.remembered().kvms.constFirst().user, u"admin"_s);
    }

    void whetherAKvmWasAskedWhatItIsIsSoOnlyWhereTheFileSaysSoInSoManyWords()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        // As the program writes it: either way it is read back as it was.
        core::OwnKvm asked;
        asked.id = u"0a1b2c3d"_s;
        asked.address = u"https://192.168.1.20"_s;
        asked.askedWhatItIs = true;
        core::OwnKvm notYet;
        notYet.id = u"0a1b2c3e"_s;
        notYet.address = u"https://192.168.1.21"_s;
        notYet.askedWhatItIs = false;
        {
            core::Settings settings(file);
            core::Remembered what;
            what.kvms = {asked, notYet};
            QVERIFY(settings.remember(what).ok());
        }
        QCOMPARE(core::Settings(file).remembered().kvms, (QList<core::OwnKvm>{asked, notYet}));
        QVERIFY(read(file).contains("\"asked\": true"));
        QVERIFY(read(file).contains("\"asked\": false"));

        // As somebody may have written it: asked is only what says "true" as a flag does. A text
        // that says so, a number, nothing, and no word at all are "not asked yet": the wall then
        // asks once more, which sends nothing that says who asks.
        QJsonArray kvms;
        const QList<QJsonValue> said{QJsonValue(true),
                                     QJsonValue(false),
                                     QJsonValue(u"true"_s),
                                     QJsonValue(1),
                                     QJsonValue(QJsonValue::Null),
                                     QJsonValue(QJsonObject{{u"asked"_s, true}}),
                                     QJsonValue(QJsonValue::Undefined)};
        for (qsizetype number = 0; number < said.size(); ++number) {
            QJsonObject kvm{{u"id"_s, u"%1"_s.arg(number, 8, 16, QLatin1Char('0'))},
                            {u"kind"_s, u"kvmd"_s},
                            {u"address"_s, u"https://192.168.1.%1"_s.arg(20 + number)}};
            if (!said.at(number).isUndefined()) {
                kvm.insert(u"asked"_s, said.at(number));
            }
            kvms.append(kvm);
        }
        write(file, QJsonDocument(QJsonObject{{u"kvms"_s, kvms}}).toJson());
        const core::Settings byHand(file);
        QList<bool> taken;
        for (const core::OwnKvm& kvm : byHand.remembered().kvms) {
            taken.append(kvm.askedWhatItIs);
        }
        QCOMPARE(taken, (QList<bool>{true, false, false, false, false, false, false}));
    }

    void aServersControllerIsReadWithTheMarkItsServerIsKeptBy()
    {
        // M7 1b-i (W6): "redfish" is a family this build reads, in its own spelling; the mark its server is kept by
        // (OwnKvm::machine, as a driver gives one) is written and read back; one that stands there in another shape
        // is no "none" (its driver would take whatever server answers first): that entry is not read, and is kept as
        // it stands.
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        core::OwnKvm server;
        server.id = u"0a1b2c3d"_s;
        server.address = u"https://192.168.1.30"_s;
        server.kind = u"redfish"_s;
        server.number = u"server:0123456789abcdef0123456789abcdef"_s;
        server.machine = u"server:0123456789abcdef0123456789abcdeffedcba9876543210fedcba9876543210"_s;
        core::Remembered what;
        what.kvms = {server};
        QVERIFY(core::Settings(file).remember(what).ok());
        const QJsonObject written = QJsonDocument::fromJson(read(file)).object()[u"kvms"_s].toArray().at(0).toObject();
        QCOMPARE(written.value(u"kind"_s), QJsonValue(u"redfish"_s));
        QCOMPARE(written.value(u"machine"_s), QJsonValue(server.machine));
        QCOMPARE(core::Settings(file).remembered().kvms, what.kvms);
        // None kept, said so or not said at all: none.
        for (const QJsonValue& none : {QJsonValue(u""_s), QJsonValue(QJsonValue::Undefined)}) {
            QJsonObject kvm = written;
            kvm.remove(u"machine"_s);
            if (!none.isUndefined()) {
                kvm.insert(u"machine"_s, none);
            }
            write(file, QJsonDocument(QJsonObject{{u"kvms"_s, QJsonArray{kvm}}}).toJson());
            const core::Settings byHand(file);
            QCOMPARE(byHand.remembered().kvms.size(), 1);
            QVERIFY(byHand.remembered().kvms.constFirst().machine.isEmpty());
            QVERIFY(!byHand.heldKvmsItDoesNotRead());
        }
        const QList<QJsonValue> notOne{QJsonValue(u"SERVER:0123456789abcdef0123456789abcdef"_s),
                                       QJsonValue(u"own:0123456789abcdef"_s),
                                       QJsonValue(u"server:0123"_s),
                                       QJsonValue(u"server:0123456789abcdef0123456789abcdeg"_s),
                                       QJsonValue(u" server:0123456789abcdef0123456789abcdef"_s),
                                       QJsonValue(7),
                                       QJsonValue(true),
                                       QJsonValue(QJsonValue::Null),
                                       QJsonValue(QJsonArray{server.machine})};
        for (const QJsonValue& said : notOne) {
            QJsonObject kvm = written;
            kvm.insert(u"machine"_s, said);
            write(file, QJsonDocument(QJsonObject{{u"kvms"_s, QJsonArray{kvm}}}).toJson());
            const core::Settings byHand(file);
            const QByteArray asSaid = QJsonDocument(QJsonArray{said}).toJson();
            QVERIFY2(!byHand.wasUnreadable(), asSaid.constData());
            QVERIFY2(byHand.heldKvmsItDoesNotRead(), asSaid.constData());
            QVERIFY2(byHand.remembered().kvms.isEmpty(), asSaid.constData());
            QCOMPARE(byHand.remembered().kvmsItDoesNotRead, QJsonArray{kvm});
        }
        // Not in another spelling.
        QJsonObject shouted = written;
        shouted.insert(u"kind"_s, u"REDFISH"_s);
        write(file, QJsonDocument(QJsonObject{{u"kvms"_s, QJsonArray{shouted}}}).toJson());
        QVERIFY(core::Settings(file).remembered().kvms.isEmpty());
        QVERIFY(core::Settings(file).heldKvmsItDoesNotRead());
    }

    void aKvmOfTheFileIsReadOnlyWhereTheFileSaysItsFamily()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        // As the program writes one: its family in so many words, and read back as it was.
        core::OwnKvm one;
        one.id = u"0a1b2c3d"_s;
        one.address = u"https://192.168.1.20"_s;
        core::Remembered what;
        what.kvms = {one};
        QVERIFY(core::Settings(file).remember(what).ok());
        const QJsonArray written = QJsonDocument::fromJson(read(file)).object()[u"kvms"_s].toArray();
        QCOMPARE(written.at(0).toObject().value(u"kind"_s), QJsonValue(u"kvmd"_s));
        QCOMPARE(core::Settings(file).remembered().kvms, what.kvms);

        // A JetKVM, the other family this build speaks to by itself: written as the program writes
        // it, and read back as it was.
        core::OwnKvm jet = one;
        jet.id = u"0a1b2c3f"_s;
        jet.address = u"http://192.168.1.25"_s;
        jet.kind = u"jetkvm"_s;
        core::Remembered both;
        both.kvms = {one, jet};
        QVERIFY(core::Settings(file).remember(both).ok());
        const QJsonArray writtenBoth = QJsonDocument::fromJson(read(file)).object()[u"kvms"_s].toArray();
        QCOMPARE(writtenBoth.at(1).toObject().value(u"kind"_s), QJsonValue(u"jetkvm"_s));
        const core::Settings jetRead(file);
        QCOMPARE(jetRead.remembered().kvms, both.kvms);
        QVERIFY(!jetRead.heldKvmsItDoesNotRead());

        // As somebody may have written it: only where it says a family this build speaks to by
        // itself, as a text and in its own spelling. A KVM of the program's shape that says none (an
        // older build's), an Intel AMT machine's (not read), or a word that is no family, is one this
        // build does not read: kept as it stood, not on the wall, and written back as it stood, before
        // the wall's own (a build that is gone back to does not lose a wall it cannot read).
        const QList<QJsonValue> said{QJsonValue(QJsonValue::Undefined),
                                     QJsonValue(u"amt"_s),
                                     QJsonValue(u"JETKVM"_s),
                                     QJsonValue(u"jetkvm "_s),
                                     QJsonValue(u"KVMD"_s),
                                     QJsonValue(u"kvmd "_s),
                                     QJsonValue(u""_s),
                                     QJsonValue(1),
                                     QJsonValue(true),
                                     QJsonValue(QJsonArray{u"kvmd"_s}),
                                     QJsonValue()};
        for (const QJsonValue& kind : said) {
            QJsonObject kvm{{u"id"_s, u"0a1b2c3e"_s}, {u"address"_s, u"https://192.168.1.21"_s}};
            if (!kind.isUndefined()) {
                kvm.insert(u"kind"_s, kind);
            }
            write(file,
                  QJsonDocument(QJsonObject{{u"kvms"_s, QJsonArray{inTheFile(one, QJsonValue()), kvm}}}).toJson());
            core::Settings byHand(file);
            const QByteArray asSaid = QJsonDocument(QJsonArray{kind}).toJson();
            QVERIFY2(!byHand.wasUnreadable(), asSaid.constData());
            QVERIFY2(byHand.heldKvmsItDoesNotRead(), asSaid.constData());
            QCOMPARE(byHand.remembered().kvms, what.kvms);
            QCOMPARE(byHand.remembered().kvmsItDoesNotRead, QJsonArray{kvm});
            // Written back as it stood, whatever the wall's own became meanwhile.
            core::Remembered renamed = byHand.remembered();
            renamed.kvms.first().name = u"Renamed"_s;
            QVERIFY(byHand.remember(renamed).ok());
            const QJsonArray after = QJsonDocument::fromJson(read(file)).object()[u"kvms"_s].toArray();
            QCOMPARE(after.size(), 2);
            QCOMPARE(after.at(0).toObject().value(u"name"_s), QJsonValue(u"Renamed"_s));
            QCOMPARE(after.at(1).toObject(), kvm);
            QCOMPARE(core::Settings(file).remembered().kvmsItDoesNotRead, QJsonArray{kvm});
        }
        // Alone, first, and once by its id: an entry of the wall's own that comes after one of another
        // family with the same id is the second of that id, and let go of.
        const QJsonObject older{{u"id"_s, u"0a1b2c3d"_s}, {u"address"_s, u"https://192.168.1.29"_s}};
        write(file, QJsonDocument(QJsonObject{{u"kvms"_s, QJsonArray{older, inTheFile(one, QJsonValue())}}}).toJson());
        const core::Settings first(file);
        QCOMPARE(first.remembered().kvmsItDoesNotRead, QJsonArray{older});
        QVERIFY(first.remembered().kvms.isEmpty());
        // An entry that is none of the program's (no id of its shape) is let go of as before, and the
        // rest is read: no KVM of the wall was in it.
        write(file, QJsonDocument(
                        QJsonObject{{u"kvms"_s, QJsonArray{inTheFile(one, QJsonValue()),
                                                           QJsonObject{{u"id"_s, u"0A1B2C3E"_s},
                                                                       {u"address"_s, u"https://192.168.1.21"_s}}}}})
                        .toJson());
        const core::Settings well(file);
        QVERIFY(!well.wasUnreadable());
        QVERIFY(!well.heldKvmsItDoesNotRead());
        QCOMPARE(well.remembered().kvms, what.kvms);
        // A file that could not be read for another reason held no KVM that is known of.
        write(file, "{ cut short");
        const core::Settings damaged(file);
        QVERIFY(damaged.wasUnreadable());
        QVERIFY(!damaged.heldKvmsItDoesNotRead());
    }

    void theMarkThatAPasswordIsKeptStandsWithItsKvmAndIsSoOnlyWhereTheFileSaysSo()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        core::OwnKvm marked;
        marked.id = u"0a1b2c3d"_s;
        marked.address = u"https://192.168.1.20"_s;
        core::OwnKvm unmarked;
        unmarked.id = u"0a1b2c3e"_s;
        unmarked.address = u"https://192.168.1.21"_s;
        // As the program writes it: read back as it was, with its KVM; and a mark of no KVM of the
        // list is not written down.
        core::Remembered what;
        what.kvms = {marked, unmarked};
        what.kvmPasswordsKept = {marked.id};
        {
            core::Settings settings(file);
            core::Remembered stray = what;
            stray.kvmPasswordsKept.insert(u"ffffffff"_s);
            QVERIFY(settings.remember(stray).ok());
        }
        QCOMPARE(core::Settings(file).remembered(), what);
        const QJsonArray kvms = QJsonDocument::fromJson(read(file)).object()[u"kvms"_s].toArray();
        QCOMPARE(kvms.at(0).toObject().value(u"secret_kept"_s), QJsonValue(true));
        QCOMPARE(kvms.at(1).toObject().value(u"secret_kept"_s), QJsonValue(false));
        // Nothing of a password is in the file, nor the word.
        QVERIFY(!read(file).contains("password"));

        // As somebody may have written it: kept only where it says true as a flag does. A text that
        // says so, a number, nothing, and no word at all are no mark: so every KVM of a file written
        // before the program kept the mark has none.
        QJsonArray byHand;
        const QList<QJsonValue> said{QJsonValue(true),
                                     QJsonValue(false),
                                     QJsonValue(u"true"_s),
                                     QJsonValue(1),
                                     QJsonValue(QJsonValue::Null),
                                     QJsonValue(QJsonObject{{u"secret_kept"_s, true}}),
                                     QJsonValue(QJsonValue::Undefined)};
        for (qsizetype number = 0; number < said.size(); ++number) {
            QJsonObject kvm{{u"id"_s, u"%1"_s.arg(number, 8, 16, QLatin1Char('0'))},
                            {u"kind"_s, u"kvmd"_s},
                            {u"address"_s, u"https://192.168.1.%1"_s.arg(20 + number)}};
            if (!said.at(number).isUndefined()) {
                kvm.insert(u"secret_kept"_s, said.at(number));
            }
            byHand.append(kvm);
        }
        write(file, QJsonDocument(QJsonObject{{u"kvms"_s, byHand}}).toJson());
        const core::Settings settings(file);
        QCOMPARE(settings.remembered().kvms.size(), said.size());
        QCOMPARE(settings.remembered().kvmPasswordsKept, QSet<QString>{u"00000000"_s});
        // (And a mark without its KVM, of an entry that is no KVM, is none.)
        write(file,
              QJsonDocument(
                  QJsonObject{{u"kvms"_s, QJsonArray{QJsonObject{{u"id"_s, u"0a1b2c3d"_s}, {u"secret_kept"_s, true}}}}})
                  .toJson());
        QVERIFY(core::Settings(file).remembered().kvmPasswordsKept.isEmpty());
    }

    void theCertificateItselfOfAKvmIsRememberedBesideItAndNothingElseOfTheFileIsOtherwise()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        const core::exchange::KeptCertificate kept =
            core::exchange::certificateOf(exampleCertificate()).value_or(core::exchange::KeptCertificate{});
        QVERIFY(!kept.pem.isEmpty());
        const core::OwnKvm known = knownBy(exampleCertificate());
        QCOMPARE(known.certificate, kept.sha256);
        core::OwnKvm without = knownBy(exampleCertificate(), u"0a1b2c3e"_s);
        without.address = u"https://192.168.1.21"_s;
        core::Remembered what;
        what.kvms = {known, without};
        what.kvmCertificates.insert(known.id, kept.pem);
        {
            core::Settings settings(file);
            QVERIFY(settings.remember(what).ok());
        }
        const core::Settings settings(file);
        QCOMPARE(settings.remembered(), what);
        QCOMPARE(settings.remembered().kvmCertificates.value(known.id), kept.pem);
        QVERIFY(!settings.remembered().kvmCertificates.contains(without.id));
        // It stands with its KVM, as the text a file of the wall has for it; a KVM that has none
        // has no such line; and the file has the parts it had, and no other.
        const QJsonObject written = QJsonDocument::fromJson(read(file)).object();
        QCOMPARE(
            written.keys(),
            (QStringList{u"actions"_s,          u"certificates"_s,      u"check_updates"_s,  u"favorites"_s,
                         u"graphics_card"_s,    u"hardware_decoding"_s, u"kvms"_s,           u"last_update_check"_s,
                         u"own_wall"_s,         u"picture_every_ms"_s,  u"picture_scales"_s, u"reached_encrypted"_s,
                         u"recent_machines"_s,  u"release_key"_s,       u"server"_s,         u"servers"_s,
                         u"settings_section"_s, u"stay_in_tray"_s,      u"tell_news"_s,      u"theme"_s,
                         u"windows"_s,          u"workspaces"_s}));
        QCOMPARE(written[u"release_key"_s].toString(), u"right-ctrl"_s);
        const QJsonArray kvms = written[u"kvms"_s].toArray();
        QCOMPARE(kvms.at(0).toObject().keys(),
                 (QStringList{u"address"_s, u"asked"_s, u"certificate"_s, u"certificate_pem"_s, u"group"_s, u"id"_s,
                              u"kind"_s, u"known_by_certificate"_s, u"machine"_s, u"name"_s, u"number"_s,
                              u"secret_kept"_s, u"user"_s}));
        QCOMPARE(kvms.at(0).toObject()[u"certificate_pem"_s].toString(), kept.pem);
        QCOMPARE(kvms.at(1).toObject().keys(), (QStringList{u"address"_s, u"asked"_s, u"certificate"_s, u"group"_s,
                                                            u"id"_s, u"kind"_s, u"known_by_certificate"_s, u"machine"_s,
                                                            u"name"_s, u"number"_s, u"secret_kept"_s, u"user"_s}));
        // A certificate that is remembered for no KVM of the list is not written down.
        core::Remembered stray = what;
        stray.kvmCertificates.insert(u"ffffffff"_s, kept.pem);
        {
            core::Settings again(file);
            QVERIFY(again.remember(stray).ok());
        }
        QCOMPARE(core::Settings(file).remembered(), what);
    }

    void aCertificateInTheFileIsBelievedOnlyWhereItIsTheOneItsKvmIsKnownBy()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        const QByteArray der = exampleCertificate();
        const core::exchange::KeptCertificate kept =
            core::exchange::certificateOf(der).value_or(core::exchange::KeptCertificate{});
        QVERIFY(!kept.pem.isEmpty());
        const core::OwnKvm known = knownBy(der);
        // What is read of a KVM as the file has it: the KVM, and the certificate itself that is
        // believed for it (empty: none).
        struct Read {
            core::OwnKvm kvm;
            QString certificate;
        };
        const auto readOf = [&file](const QJsonObject& kvm) {
            write(file, QJsonDocument(QJsonObject{{u"kvms"_s, QJsonArray{kvm}}}).toJson());
            const core::Settings settings(file);
            const QList<core::OwnKvm> kvms = settings.remembered().kvms;
            return Read{kvms.value(0), settings.remembered().kvmCertificates.value(kvms.value(0).id)};
        };
        // The one it is known by, as the program writes it; and written in one line, it is the
        // same certificate, and kept as the program keeps one.
        QCOMPARE(readOf(inTheFile(known, kept.pem)).certificate, kept.pem);
        QCOMPARE(readOf(inTheFile(known, core::testing::inOneLine(der))).certificate, kept.pem);

        // Another certificate than the one the KVM is known by: a good one, and not this KVM's.
        const QByteArray other = core::testing::certificateOfSoManyBytes(der, 600);
        const core::exchange::KeptCertificate others =
            core::exchange::certificateOf(other).value_or(core::exchange::KeptCertificate{});
        QVERIFY(!others.pem.isEmpty());
        QVERIFY(others.sha256 != kept.sha256);
        // One that is damaged: a sign of it another, or cut short.
        QString damaged = kept.pem;
        damaged[100] = damaged.at(100) == u'A' ? u'B' : u'A';
        // One that is too long for a file of the wall: as it is written, and only as it is kept.
        const QByteArray tooLong = core::testing::certificateOfSoManyBytes(der, 15'000);
        const QByteArray tooLongAsKept = core::testing::certificateOfSoManyBytes(der, 14'900);
        QVERIFY(core::testing::inOneLine(tooLong).size() > core::exchange::longestCertificate);
        QVERIFY(core::testing::inOneLine(tooLongAsKept).size() <= core::exchange::longestCertificate);
        struct Case {
            const char* what;
            core::OwnKvm kvm;
            QJsonValue said;
        };
        core::OwnKvm byNone = known;
        byNone.knownByCertificate = false;
        core::OwnKvm inTheClear = known;
        inTheClear.address = u"http://192.168.1.20"_s;
        core::OwnKvm byHalfAFingerprint = known;
        byHalfAFingerprint.certificate = known.certificate.left(16);
        const QList<Case> none{
            {"another KVM's certificate", known, others.pem},
            {"a damaged one", known, damaged},
            {"one that is cut short", known, kept.pem.left(kept.pem.size() / 2)},
            {"one that is too long", knownBy(tooLong), core::testing::inOneLine(tooLong)},
            {"one that is too long as it is kept", knownBy(tooLongAsKept), core::testing::inOneLine(tooLongAsKept)},
            {"two certificates", known, kept.pem + kept.pem},
            {"no text", known, 7},
            {"a list", known, QJsonArray{kept.pem}},
            {"nothing", known, QJsonValue(QJsonValue::Null)},
            {"an empty text", known, QString()},
            {"of a KVM that is known by none", byNone, kept.pem},
            {"of a KVM that is spoken to in the clear", inTheClear, kept.pem},
            {"of a KVM whose fingerprint is none", byHalfAFingerprint, kept.pem},
        };
        for (const Case& one : none) {
            const Read got = readOf(inTheFile(one.kvm, one.said));
            // It is not believed; and the KVM is what the list says, known by what it was known by.
            QVERIFY2(got.certificate.isEmpty(), one.what);
            QVERIFY2(got.kvm == one.kvm, one.what);
        }
    }

    void aKvmInTheFileThatIsNoneThisProgramWroteIsNotOne()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        const auto kvm = [](const QString& id, const QString& address) {
            return QJsonObject{
                {u"id"_s, id}, {u"kind"_s, u"kvmd"_s}, {u"address"_s, address}, {u"name"_s, QString(500, u'n')}};
        };
        const QJsonArray kvms{
            kvm(u"0a1b2c3d"_s, u"https://192.168.1.20"_s),
            kvm(u"0a1b2c3d"_s, u"https://192.168.1.21"_s),          // an id twice: the first is the one
            kvm(u"0A1B2C3E"_s, u"https://192.168.1.22"_s),          // no id of the program's shape
            kvm(u"kvm:1"_s, u"https://192.168.1.23"_s),             // nor this
            kvm(u"0a1b2c3f"_s, u"ftp://192.168.1.24"_s),            // no address of a KVM
            kvm(u"0a1b2c40"_s, u"https://admin:pw@192.168.1.25"_s), // a name and a password in it
            kvm(u"0a1b2c41"_s, u"not an address"_s),
            kvm(u"0a1b2c42"_s, QString()),
            u"not an entry"_s,
            QJsonObject{{u"id"_s, u"0a1b2c43"_s},
                        {u"kind"_s, u"kvmd"_s},
                        {u"address"_s, u"https://192.168.1.26"_s},
                        {u"user"_s, u""_s},
                        {u"known_by_certificate"_s, true},
                        {u"certificate"_s, u"abcd"_s}},
        };
        QFile written(file);
        QVERIFY(written.open(QIODevice::WriteOnly));
        written.write(QJsonDocument(QJsonObject{{u"kvms"_s, kvms}}).toJson());
        written.close();

        const core::Settings settings(file);
        const QList<core::OwnKvm> taken = settings.remembered().kvms;
        QCOMPARE(taken.size(), 2);
        QCOMPARE(taken.at(0).address, u"https://192.168.1.20"_s);
        QCOMPARE(taken.at(0).name.size(), 80);
        // A user name of nothing is the usual one. A certificate that was cut short stays what
        // stands there: it is not "none", and nothing is trusted in its place.
        QCOMPARE(taken.at(1).user, u"admin"_s);
        QVERIFY(taken.at(1).knownByCertificate);
        QCOMPARE(taken.at(1).certificate.size(), 2);
    }

    void anAddressInTheFileIsOneOnlyTheWayTheProgramWritesIt()
    {
        // One KVM has one address: what says the same otherwise is none the program wrote, and
        // would be another place to whoever asks whether a KVM is on the wall already.
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        QJsonArray kvms;
        const QStringList addresses{
            u"https://192.168.1.20"_s,   u"http://192.168.1.21:8080"_s, u"https://192.168.1.22:443"_s,
            u"http://192.168.1.23:80"_s, u"https://192.168.1.24/"_s,    u"https://192.168.1.25/kvm"_s,
            u"HTTPS://192.168.1.26"_s,   u"https://KVM.example"_s,      u"https://kvm.example."_s,
            u"https://kvm.example"_s,    u"https://192.168.1.27?x=1"_s, u"192.168.1.28"_s};
        for (qsizetype number = 0; number < addresses.size(); ++number) {
            kvms.append(QJsonObject{{u"id"_s, u"%1"_s.arg(number, 8, 16, QLatin1Char('0'))},
                                    {u"kind"_s, u"kvmd"_s},
                                    {u"address"_s, addresses.at(number)}});
        }
        write(file, QJsonDocument(QJsonObject{{u"kvms"_s, kvms}}).toJson());
        const core::Settings settings(file);
        QStringList taken;
        for (const core::OwnKvm& kvm : settings.remembered().kvms) {
            taken.append(kvm.address);
        }
        QCOMPARE(taken,
                 (QStringList{u"https://192.168.1.20"_s, u"http://192.168.1.21:8080"_s, u"https://kvm.example"_s}));
    }

    void noMoreKvmsThanAWallHolds()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        core::Remembered what;
        for (int number = 0; number < core::Remembered::mostKvms + 20; ++number) {
            core::OwnKvm kvm;
            kvm.id = u"%1"_s.arg(number, 8, 16, QLatin1Char('0'));
            kvm.address = u"https://192.168.1.20:%1"_s.arg(1024 + number);
            what.kvms.append(kvm);
        }
        {
            core::Settings settings(file);
            QVERIFY(settings.remember(what).ok());
        }
        const core::Settings settings(file);
        QCOMPARE(settings.remembered().kvms.size(), core::Remembered::mostKvms);

        // Nor are more taken from a file that somebody wrote more into.
        QJsonArray many;
        for (int number = 0; number < core::Remembered::mostKvms + 20; ++number) {
            many.append(QJsonObject{{u"id"_s, u"%1"_s.arg(number, 8, 16, QLatin1Char('0'))},
                                    {u"kind"_s, u"kvmd"_s},
                                    {u"address"_s, u"https://192.168.1.20:%1"_s.arg(1024 + number)}});
        }
        const QString other = folder.filePath(u"written-by-hand.json"_s);
        QFile byHand(other);
        QVERIFY(byHand.open(QIODevice::WriteOnly));
        byHand.write(QJsonDocument(QJsonObject{{u"kvms"_s, many}}).toJson());
        byHand.close();
        const core::Settings fromHand(other);
        QCOMPARE(fromHand.remembered().kvms.size(), core::Remembered::mostKvms);
    }

    void ofAListInTheFileSoManyEntriesAreLookedAtAndNoneBehindThem()
    {
        // A list is not gone through to its end, however long somebody made it: the looking
        // stops at as many entries as this program writes into that list, taken or not. (The
        // limit counted what was taken once: a list of millions of entries of nothing was
        // looked at whole.) The addresses are ones that reach nothing, by the books.
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        const QJsonObject kvm{
            {u"id"_s, u"0a1b2c3d"_s}, {u"kind"_s, u"kvmd"_s}, {u"address"_s, u"https://192.0.2.20"_s}};
        const auto behind = [&kvm](qsizetype entriesOfNothing) {
            QJsonArray list;
            for (qsizetype number = 0; number < entriesOfNothing; ++number) {
                list.append(QJsonObject{});
            }
            list.append(kvm);
            return list;
        };
        // The last entry that is looked at: the two-thousandth.
        write(file, QJsonDocument(QJsonObject{{u"kvms"_s, behind(core::Remembered::mostKvms - 1)}}).toJson());
        QCOMPARE(core::Settings(file).remembered().kvms.size(), 1);
        // And the first that is not.
        write(file, QJsonDocument(QJsonObject{{u"kvms"_s, behind(core::Remembered::mostKvms)}}).toJson());
        QCOMPARE(core::Settings(file).remembered().kvms.size(), 0);
        write(file, QJsonDocument(QJsonObject{{u"kvms"_s, behind(50'000)}}).toJson());
        QCOMPARE(core::Settings(file).remembered().kvms.size(), 0);

        // So with the servers that were reached encrypted, of which two hundred are kept.
        const auto namesBehind = [](qsizetype entriesOfNothing) {
            QJsonArray list;
            for (qsizetype number = 0; number < entriesOfNothing; ++number) {
                list.append(QJsonValue(7));
            }
            list.append(u"qkvm.invalid"_s);
            return list;
        };
        write(file, QJsonDocument(QJsonObject{{u"reached_encrypted"_s, namesBehind(199)}}).toJson());
        QCOMPARE(core::Settings(file).remembered().reachedEncrypted, QStringList{u"qkvm.invalid"_s});
        write(file, QJsonDocument(QJsonObject{{u"reached_encrypted"_s, namesBehind(200)}}).toJson());
        QCOMPARE(core::Settings(file).remembered().reachedEncrypted, QStringList{});

        // And with their certificates. (Such a list is gone through by its names, in the order
        // of the alphabet: the one that is a certificate stands last.)
        const QString sha256 = QString::fromLatin1(QByteArray(32, '\x5a').toHex());
        const auto certificatesBehind = [&sha256](int entriesOfNothing) {
            QJsonObject list;
            for (int number = 0; number < entriesOfNothing; ++number) {
                list.insert(u"a%1"_s.arg(number, 4, 10, QLatin1Char('0')), u"no certificate"_s);
            }
            list.insert(u"z.invalid:443"_s, sha256);
            return list;
        };
        write(file, QJsonDocument(QJsonObject{{u"certificates"_s, certificatesBehind(199)}}).toJson());
        QCOMPARE(core::Settings(file).remembered().certificates.size(), 1);
        write(file, QJsonDocument(QJsonObject{{u"certificates"_s, certificatesBehind(200)}}).toJson());
        QCOMPARE(core::Settings(file).remembered().certificates.size(), 0);
    }

    void thisPcsSavedActionsAreRememberedInTheirOrder()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        core::OwnAction signIn;
        signIn.id = u"0a1b2c3e"_s;
        signIn.name = u"Sign in"_s;
        signIn.types = true;
        signIn.enter = true;
        signIn.changed = 1;
        signIn.writing = u"b:0123456789abcdef"_s;
        core::OwnAction lock;
        lock.id = u"0a1b2c3d"_s;
        lock.name = u"Lock"_s;
        lock.keys = u"MetaLeft,KeyL"_s;
        lock.danger = true;
        lock.changed = 4;
        // (One that types and has no text kept for it: it is one all the same.)
        core::OwnAction withoutAText;
        withoutAText.id = u"0a1b2c3f"_s;
        withoutAText.name = u"Sign out"_s;
        withoutAText.types = true;
        core::Remembered what;
        what.actions = {signIn, lock, withoutAText};
        {
            core::Settings settings(file);
            QVERIFY(settings.remember(what).ok());
        }
        const core::Settings settings(file);
        QCOMPARE(settings.remembered().actions, what.actions);
        QCOMPARE(settings.remembered(), what);
        // As the file says it: each field by its name, the kind in the server's word for it,
        // and of a text only which writing of it the action has.
        const QJsonArray said = QJsonDocument::fromJson(read(file)).object()[u"actions"_s].toArray();
        QCOMPARE(said.size(), 3);
        QCOMPARE(said.at(0).toObject(), (QJsonObject{{u"id"_s, u"0a1b2c3e"_s},
                                                     {u"name"_s, u"Sign in"_s},
                                                     {u"kind"_s, u"text"_s},
                                                     {u"keys"_s, u""_s},
                                                     {u"enter"_s, true},
                                                     {u"danger"_s, false},
                                                     {u"changed"_s, 1},
                                                     {u"writing"_s, u"b:0123456789abcdef"_s}}));
        QCOMPARE(said.at(1).toObject().value(u"kind"_s).toString(), u"keys"_s);
    }

    void anActionInTheFileIsOneOnlyAsTheProgramKeepsOne_data()
    {
        QTest::addColumn<QJsonValue>("entry");
        QTest::addColumn<bool>("taken");
        const QJsonObject presses{
            {u"id"_s, u"0a1b2c3d"_s}, {u"name"_s, u"Lock"_s}, {u"kind"_s, u"keys"_s}, {u"keys"_s, u"MetaLeft,KeyL"_s},
            {u"enter"_s, false},      {u"danger"_s, true},    {u"changed"_s, 2},      {u"writing"_s, u""_s}};
        const QJsonObject types{{u"id"_s, u"0a1b2c3e"_s}, {u"name"_s, u"Sign in"_s},
                                {u"kind"_s, u"text"_s},   {u"keys"_s, u""_s},
                                {u"enter"_s, true},       {u"danger"_s, false},
                                {u"changed"_s, 0},        {u"writing"_s, u"a:0123456789abcdef"_s}};
        const auto with = [](QJsonObject entry, const QString& field, const QJsonValue& value) {
            entry.insert(field, value);
            return QJsonValue(entry);
        };
        const auto without = [](QJsonObject entry, const QString& field) {
            entry.remove(field);
            return QJsonValue(entry);
        };
        QTest::newRow("one that presses") << QJsonValue(presses) << true;
        QTest::newRow("one that types") << QJsonValue(types) << true;
        QTest::newRow("no entry at all") << QJsonValue(u"not an entry"_s) << false;
        QTest::newRow("an entry of nothing") << QJsonValue(QJsonObject{}) << false;
        QTest::newRow("an id in capitals") << with(presses, u"id"_s, u"0A1B2C3D"_s) << false;
        QTest::newRow("an id of another shape") << with(presses, u"id"_s, u"action:1"_s) << false;
        QTest::newRow("no id") << without(presses, u"id"_s) << false;
        QTest::newRow("a name of sixty signs") << with(presses, u"name"_s, QString(60, u'n')) << true;
        QTest::newRow("a name of sixty-one signs") << with(presses, u"name"_s, QString(61, u'n')) << false;
        QTest::newRow("a name of nothing") << with(presses, u"name"_s, u""_s) << false;
        QTest::newRow("no name") << without(types, u"name"_s) << false;
        QTest::newRow("a name of blanks") << with(presses, u"name"_s, u"   "_s) << false;
        QTest::newRow("a name with blanks around it") << with(presses, u"name"_s, u" Lock "_s) << false;
        QTest::newRow("a name with a tab in it") << with(presses, u"name"_s, u"Lock\tscreen"_s) << false;
        QTest::newRow("a name of two lines") << with(types, u"name"_s, u"Sign\nin"_s) << false;
        QTest::newRow("a name that turns the writing round")
            << with(types, u"name"_s, u"Sign"_s + QChar(0x202E) + u"ni"_s) << false;
        QTest::newRow("a name that is a number") << with(presses, u"name"_s, 7) << false;
        QTest::newRow("a kind that is none") << with(presses, u"kind"_s, u"macro"_s) << false;
        QTest::newRow("a kind in capitals") << with(types, u"kind"_s, u"Text"_s) << false;
        QTest::newRow("no kind") << without(presses, u"kind"_s) << false;
        QTest::newRow("one key") << with(presses, u"keys"_s, u"F1"_s) << true;
        QTest::newRow("keys that are none") << with(presses, u"keys"_s, u"ctrl+alt+del"_s) << false;
        QTest::newRow("the keyboard's Power key") << with(presses, u"keys"_s, u"ControlLeft,Power"_s) << false;
        QTest::newRow("nine keys") << with(presses, u"keys"_s, u"KeyA,KeyB,KeyC,KeyD,KeyE,KeyF,KeyG,KeyH,KeyI"_s)
                                   << false;
        QTest::newRow("keys and the end of a line") << with(presses, u"keys"_s, u"KeyA\n"_s) << false;
        QTest::newRow("no keys, of one that presses") << with(presses, u"keys"_s, u""_s) << false;
        QTest::newRow("keys, of one that types") << with(types, u"keys"_s, u"F1"_s) << false;
        QTest::newRow("Enter, of one that presses") << with(presses, u"enter"_s, true) << false;
        QTest::newRow("no Enter, of one that types") << with(types, u"enter"_s, false) << true;
        QTest::newRow("Enter that is no flag") << with(types, u"enter"_s, u"yes"_s) << false;
        QTest::newRow("no word on Enter") << without(types, u"enter"_s) << false;
        QTest::newRow("dangerous that is no flag") << with(presses, u"danger"_s, 1) << false;
        QTest::newRow("no word on whether it is dangerous") << without(presses, u"danger"_s) << false;
        QTest::newRow("edited as often as any") << with(presses, u"changed"_s, 1'000'000'000) << true;
        QTest::newRow("edited more often than any") << with(presses, u"changed"_s, 1'000'000'001) << false;
        QTest::newRow("edited less often than never") << with(presses, u"changed"_s, -1) << false;
        QTest::newRow("edited half a time") << with(presses, u"changed"_s, 1.5) << false;
        QTest::newRow("a count of edits that is a text") << with(presses, u"changed"_s, u"2"_s) << false;
        QTest::newRow("no count of edits") << without(types, u"changed"_s) << false;
    }

    void anActionInTheFileIsOneOnlyAsTheProgramKeepsOne()
    {
        // Each entry is held to the rules an action is saved by, and one that does not hold is
        // left out: there is no older shape of this to read.
        QFETCH(QJsonValue, entry);
        QFETCH(bool, taken);
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        // (Between two that hold: one that is left out takes neither of those with it.)
        const QJsonObject before{{u"id"_s, u"00000001"_s}, {u"name"_s, u"Before"_s}, {u"kind"_s, u"keys"_s},
                                 {u"keys"_s, u"F1"_s},     {u"enter"_s, false},      {u"danger"_s, false},
                                 {u"changed"_s, 0},        {u"writing"_s, u""_s}};
        QJsonObject after = before;
        after.insert(u"id"_s, u"00000002"_s);
        after.insert(u"name"_s, u"After"_s);
        write(file, QJsonDocument(QJsonObject{{u"actions"_s, QJsonArray{before, entry, after}}}).toJson());
        const core::Settings settings(file);
        QVERIFY(!settings.wasUnreadable());
        const QList<core::OwnAction> actions = settings.remembered().actions;
        QCOMPARE(actions.size(), taken ? 3 : 2);
        QCOMPARE(actions.constFirst().name, u"Before"_s);
        QCOMPARE(actions.constLast().name, u"After"_s);
        if (taken) {
            // And what is taken is what the file said: written again, it says the same.
            core::Settings again(folder.filePath(u"again.json"_s));
            QVERIFY(again.remember(settings.remembered()).ok());
            const QJsonArray said =
                QJsonDocument::fromJson(read(folder.filePath(u"again.json"_s))).object()[u"actions"_s].toArray();
            QCOMPARE(said.at(1), entry);
        }
    }

    void anActionInTheFileThatNamesNoWritingOfItsTextIsThereWithoutAText_data()
    {
        QTest::addColumn<QJsonValue>("writing");
        QTest::addColumn<QString>("kept");
        QTest::newRow("in the first place") << QJsonValue(u"a:0123456789abcdef"_s) << u"a:0123456789abcdef"_s;
        QTest::newRow("in the second place") << QJsonValue(u"b:fedcba9876543210"_s) << u"b:fedcba9876543210"_s;
        QTest::newRow("none") << QJsonValue(u""_s) << QString();
        QTest::newRow("no word on it") << QJsonValue(QJsonValue::Undefined) << QString();
        QTest::newRow("in a place there is none of") << QJsonValue(u"c:0123456789abcdef"_s) << QString();
        QTest::newRow("a mark that is too short") << QJsonValue(u"a:0123456789abcde"_s) << QString();
        QTest::newRow("a mark that is too long") << QJsonValue(u"a:0123456789abcdef0"_s) << QString();
        QTest::newRow("a mark in capitals") << QJsonValue(u"a:0123456789ABCDEF"_s) << QString();
        QTest::newRow("a mark and the end of a line") << QJsonValue(u"a:0123456789abcdef\n"_s) << QString();
        QTest::newRow("a number") << QJsonValue(7) << QString();
        QTest::newRow("a text, as somebody wrote it in") << QJsonValue(u"the text itself"_s) << QString();
    }

    void anActionInTheFileThatNamesNoWritingOfItsTextIsThereWithoutAText()
    {
        QFETCH(QJsonValue, writing);
        QFETCH(QString, kept);
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        QJsonObject types{{u"id"_s, u"0a1b2c3e"_s}, {u"name"_s, u"Sign in"_s}, {u"kind"_s, u"text"_s},
                          {u"keys"_s, u""_s},       {u"enter"_s, true},        {u"danger"_s, false},
                          {u"changed"_s, 0}};
        // Of one that presses nothing is read as a writing of a text: it has no text.
        QJsonObject presses = types;
        presses.insert(u"id"_s, u"0a1b2c3d"_s);
        presses.insert(u"kind"_s, u"keys"_s);
        presses.insert(u"keys"_s, u"F1"_s);
        presses.insert(u"enter"_s, false);
        if (!writing.isUndefined()) {
            types.insert(u"writing"_s, writing);
            presses.insert(u"writing"_s, writing);
        }
        write(file, QJsonDocument(QJsonObject{{u"actions"_s, QJsonArray{types, presses}}}).toJson());
        const core::Settings settings(file);
        const QList<core::OwnAction> actions = settings.remembered().actions;
        QCOMPARE(actions.size(), 2);
        QVERIFY(actions.at(0).types);
        QCOMPARE(actions.at(0).writing, kept);
        QVERIFY(!actions.at(1).types);
        QCOMPARE(actions.at(1).writing, QString());
    }

    void noActionInTheFileIsTakenTwice()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        // An id twice: the first is the one.
        write(file, QJsonDocument(QJsonObject{{u"actions"_s, QJsonArray{actionInTheFile(u"0a1b2c3d"_s, u"First"_s),
                                                                        actionInTheFile(u"0a1b2c3d"_s, u"Second"_s),
                                                                        actionInTheFile(u"0a1b2c3e"_s, u"Third"_s)}}})
                        .toJson());
        const core::Settings settings(file);
        QStringList names;
        for (const core::OwnAction& one : settings.remembered().actions) {
            names.append(one.name);
        }
        QCOMPARE(names, (QStringList{u"First"_s, u"Third"_s}));
    }

    void ofTheActionsInTheFileSoManyEntriesAreLookedAtAndNoneBehindThem()
    {
        // As many as are kept, whatever they are, as of the KVMs.
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        const auto behind = [](qsizetype entriesOfNothing) {
            QJsonArray list;
            for (qsizetype number = 0; number < entriesOfNothing; ++number) {
                list.append(QJsonObject{});
            }
            list.append(actionInTheFile(u"0a1b2c3d"_s, u"Last"_s));
            return list;
        };
        write(file, QJsonDocument(QJsonObject{{u"actions"_s, behind(core::mostSavedActions - 1)}}).toJson());
        QCOMPARE(core::Settings(file).remembered().actions.size(), 1);
        write(file, QJsonDocument(QJsonObject{{u"actions"_s, behind(core::mostSavedActions)}}).toJson());
        QCOMPARE(core::Settings(file).remembered().actions.size(), 0);
        write(file, QJsonDocument(QJsonObject{{u"actions"_s, behind(50'000)}}).toJson());
        QCOMPARE(core::Settings(file).remembered().actions.size(), 0);
    }

    void noMoreActionsAreWrittenThanAreKept()
    {
        const QTemporaryDir folder;
        core::Remembered what;
        for (int number = 0; number < core::mostSavedActions + 20; ++number) {
            core::OwnAction one;
            one.id = u"%1"_s.arg(number, 8, 16, QLatin1Char('0'));
            one.name = u"Action %1"_s.arg(number);
            one.keys = u"F1"_s;
            what.actions.append(one);
        }
        const QString written = folder.filePath(u"written.json"_s);
        {
            core::Settings settings(written);
            QVERIFY(settings.remember(what).ok());
        }
        QCOMPARE(QJsonDocument::fromJson(read(written)).object()[u"actions"_s].toArray().size(),
                 core::mostSavedActions);
        QCOMPARE(core::Settings(written).remembered().actions.size(), core::mostSavedActions);
    }

    void whatIsNotedIsRememberedAtOnceAndWrittenWithTheNextWritingOrWhenThatIsSaid()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        core::Settings settings(file);
        QVERIFY(!settings.hasUnwritten());
        QCOMPARE(settings.writings(), quint64{0});
        // Nothing noted: nothing is written, and no file is made.
        QVERIFY(settings.writeDown().ok());
        QVERIFY(!QFile::exists(file));

        // Noted, a hundred times over: remembered at once, and no file is written for it.
        core::Remembered what;
        for (int number = 0; number < 100; ++number) {
            what.wallPictureEveryMs = 1000 + number;
            settings.note(what);
        }
        QCOMPARE(settings.remembered(), what);
        QVERIFY(settings.hasUnwritten());
        QCOMPARE(settings.writings(), quint64{0});
        QVERIFY(!QFile::exists(file));
        // Written when that is said: once, all of it.
        QVERIFY(settings.writeDown().ok());
        QVERIFY(!settings.hasUnwritten());
        QCOMPARE(settings.writings(), quint64{1});
        QCOMPARE(core::Settings(file).remembered(), what);
        QVERIFY(settings.writeDown().ok());
        QCOMPARE(settings.writings(), quint64{1});

        // And what is noted goes with whatever is written next, by whoever remembers anything.
        what.theme = u"dark"_s;
        settings.note(what);
        QCOMPARE(core::Settings(file).remembered().theme, QString());
        core::Remembered more = settings.remembered();
        more.staysInTray = true;
        QVERIFY(settings.remember(more).ok());
        QVERIFY(!settings.hasUnwritten());
        QCOMPARE(settings.writings(), quint64{2});
        QCOMPARE(core::Settings(file).remembered(), more);
        QCOMPARE(core::Settings(file).remembered().theme, u"dark"_s);
    }

    void whatCouldNotBeWrittenIsWrittenWhenThatIsSaidAgain()
    {
        // The settings' folder is a file for a while: nothing can be written. What was to be
        // remembered is remembered, counts as unwritten, and is written once it can be.
        const QTemporaryDir folder;
        const QString inTheWay = folder.filePath(u"kept"_s);
        const QString file = inTheWay + u"/settings.json"_s;
        write(inTheWay, "in the way");
        core::Settings settings(file);
        core::Remembered what;
        what.theme = u"light"_s;
        QVERIFY(!settings.remember(what).ok());
        QVERIFY(settings.hasUnwritten());
        QCOMPARE(settings.writings(), quint64{0});
        QVERIFY(!settings.writeDown().ok());
        QVERIFY(QFile::remove(inTheWay));
        QVERIFY(settings.writeDown().ok());
        QVERIFY(!settings.hasUnwritten());
        QCOMPARE(settings.writings(), quint64{1});
        QCOMPARE(core::Settings(file).remembered().theme, u"light"_s);
    }
};

QTEST_GUILESS_MAIN(TestSettings)
#include "test_settings.moc"
