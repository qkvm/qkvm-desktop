#include <functional>
#include <optional>

#include <QFile>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <qkvm/core/server_client.h>
#include <qkvm/core/server_types.h>

#include "files_view.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using ui::FilesView;

namespace {

// A view with doors of the test's own: where a file is saved and which is opened is said here,
// how often either was asked is counted, and something can be made to happen while one of them
// is open, as it can behind a box of Windows'.
struct Desk {
    Desk()
    {
        view.setDoor({[this](const QString& suggested) -> base::Result<QString> {
                          suggestions.append(suggested);
                          wasBusyWhileAsked = view.isBusy();
                          if (meanwhile) {
                              meanwhile();
                          }
                          if (doorIsStuck) {
                              return base::Error{"box", "Windows did not open its box for choosing a file."};
                          }
                          return savesTo;
                      },
                      [this]() -> base::Result<QString> {
                          ++opened;
                          wasBusyWhileAsked = view.isBusy();
                          if (meanwhile) {
                              meanwhile();
                          }
                          if (doorIsStuck) {
                              return base::Error{"box", "Windows did not open its box for choosing a file."};
                          }
                          return opens;
                      }});
    }

    QString write(const QString& name, const QByteArray& text) const
    {
        const QString path = folder.filePath(name);
        QFile file(path);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(text);
        }
        return path;
    }

    static QByteArray read(const QString& path)
    {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("(no such file)");
    }

    // The number the last question of its kind went out under.
    quint64 exportTurn() const { return exportAsked.last().at(0).toULongLong(); }
    quint64 readTurn() const { return readAsked.last().at(0).toULongLong(); }
    quint64 takeTurn() const { return takeAsked.last().at(0).toULongLong(); }

    // A file is chosen and read, and the server says this is in it.
    void hasRead(const QString& name, const QByteArray& text, const core::FileContents& contents,
                 const QString& passphrase = {})
    {
        opens = write(name, text);
        view.chooseFile();
        view.readFile(passphrase);
        view.fileWasRead(readTurn(), contents);
    }

    QTemporaryDir folder;
    FilesView view;
    QString savesTo;
    QString opens;
    QStringList suggestions;
    int opened = 0;
    bool wasBusyWhileAsked = false;
    bool doorIsStuck = false;
    std::function<void()> meanwhile;
    QSignalSpy exportAsked{&view, &FilesView::exportAsked};
    QSignalSpy readAsked{&view, &FilesView::readAsked};
    QSignalSpy takeAsked{&view, &FilesView::takeAsked};
    QSignalSpy done{&view, &FilesView::done};
};

const QByteArray sealedFile = "{\"format\": \"qkvm-export\", \"sealed\": {\"v\": 1}}";
const QByteArray plainFile = "{\"format\": \"qkvm-export\", \"devices\": []}";

core::WallFile made(int notOpened = 0, bool sealed = true)
{
    core::WallFile file;
    file.text = sealed ? sealedFile : plainFile;
    file.sealed = sealed;
    file.notOpened = notOpened;
    return file;
}

core::FileDevice device(const QString& name, const QString& host)
{
    core::FileDevice one;
    one.name = name;
    one.host = host;
    one.hasPassword = true;
    one.isNew = true;
    return one;
}

core::FileAction typing(const QString& name, const QString& text)
{
    core::FileAction one;
    one.name = name;
    one.types = true;
    one.text = text;
    one.isNew = true;
    return one;
}

core::FileContents aFile()
{
    core::FileContents file;
    file.encrypted = true;
    file.program = u"0.1.0"_s;
    file.madeAt = 1791676800;
    core::FileDevice kvm = device(u"Build server"_s, u"192.168.1.21"_s);
    kvm.tls = u"pinned"_s;
    core::FileDevice bare = device(u"Rack"_s, u"192.168.1.30:8443"_s);
    bare.hasPassword = false;
    bare.tls = u"insecure"_s;
    bare.openUrl = u"https:"_s + u"//192.168.1.30:9443/console"_s;
    bare.group = u"Lab"_s;
    core::FileDevice here = device(u"NAS"_s, u"192.168.1.22"_s);
    here.isNew = false;
    core::FileDevice twin = device(u"NAS again"_s, u"192.168.1.23"_s);
    twin.isNew = false;
    twin.sameAs = u"Storage"_s;
    file.devices = QList<core::FileDevice>{kvm, bare, here, twin};
    core::FileAction lock;
    lock.name = u"Lock"_s;
    lock.keys = u"MetaLeft,KeyL"_s;
    core::FileAction restart = typing(u"Restart now"_s, u"shutdown /r /t 0"_s);
    restart.enter = true;
    restart.danger = true;
    file.actions = QList<core::FileAction>{lock, restart};
    core::FileAlerts alerts;
    alerts.enabled = true;
    alerts.hasAddress = true;
    alerts.host = u"hooks.example.com"_s;
    alerts.kinds = {u"offline"_s, u"recovered"_s};
    file.alerts = alerts;
    core::FileWallSettings wall;
    wall.pictureEverySeconds = 2.5;
    wall.asksForNewerVersions = false;
    file.wall = wall;
    file.leftOut = {u"device 5: no address"_s};
    return file;
}

QVariantMap partOf(const FilesView& view, const QString& key)
{
    const QVariantList parts = view.parts();
    for (const QVariant& part : parts) {
        if (part.toMap().value(u"key"_s).toString() == key) {
            return part.toMap();
        }
    }
    return {};
}

QString lineOf(const FilesView& view, const QString& key, qsizetype index, const QString& what = u"text"_s)
{
    return partOf(view, key).value(u"lines"_s).toList().value(index).toMap().value(what).toString();
}

} // namespace

// The wall as a file: what the two boxes of a window ask, and what they are shown.
class TestFilesView : public QObject {
    Q_OBJECT

private slots:
    void theBoxListsWhatAFileCanHoldInThePagesWords()
    {
        const QVariantList parts = FilesView::partsOfAFile();
        QCOMPARE(parts.size(), 4);
        QCOMPARE(parts[0].toMap(), (QVariantMap{{u"key"_s, u"devices"_s}, {u"label"_s, u"KVMs and servers"_s}}));
        QCOMPARE(parts[1].toMap().value(u"label"_s).toString(), u"Saved actions"_s);
        QCOMPARE(parts[2].toMap().value(u"label"_s).toString(), u"Alert settings"_s);
        QCOMPARE(parts[3].toMap(), (QVariantMap{{u"key"_s, u"wall"_s}, {u"label"_s, u"Wall settings"_s}}));
    }

    void aFileIsNotAskedForUntilItIsKnownWhatAndForWhomAndWhere()
    {
        Desk desk;
        desk.savesTo = desk.folder.filePath(u"wall.qkvm"_s);
        desk.view.exportWall({}, u"a passphrase of mine"_s, u"my own"_s);
        QCOMPARE(desk.view.problem(), u"Choose what the file is to hold."_s);
        // Ten characters are a passphrase; nine are not.
        desk.view.exportWall({u"devices"_s}, u"123456789"_s, u"my own"_s);
        QCOMPARE(desk.view.problem(),
                 u"The passphrase needs 10 characters or more. Leave it empty for a file without passwords."_s);
        desk.view.exportWall({u"devices"_s}, u"a passphrase of mine"_s, {});
        QCOMPARE(desk.view.problem(), u"Enter your password."_s);
        // Nothing was asked of anybody so far: not the server, and not where to save.
        QCOMPARE(desk.exportAsked.count(), 0);
        QVERIFY(desk.suggestions.isEmpty());
        QVERIFY(!desk.view.isBusy());

        // Where it is to be kept is asked first, under the name the page's files have. Nobody says
        // where: no file is made, and what was wrong before is not said any more.
        desk.savesTo.clear();
        desk.view.exportWall({u"devices"_s}, u"a passphrase of mine"_s, u"my own"_s);
        QCOMPARE(desk.suggestions.size(), 1);
        QVERIFY2(QRegularExpression(u"^qkvm-\\d{4}-\\d{2}-\\d{2}\\.qkvm$"_s).match(desk.suggestions[0]).hasMatch(),
                 qPrintable(desk.suggestions[0]));
        QCOMPARE(desk.exportAsked.count(), 0);
        QCOMPARE(desk.view.problem(), QString());
        QVERIFY(!desk.view.isBusy());
        // Without a passphrase it is a plain file, and is named as one.
        desk.view.exportWall({u"devices"_s}, {}, u"my own"_s);
        QVERIFY(desk.suggestions[1].endsWith(u".json"_s));
        // And ten characters are asked about like any passphrase.
        desk.view.exportWall({u"devices"_s}, u"1234567890"_s, u"my own"_s);
        QCOMPARE(desk.suggestions.size(), 3);
        QCOMPARE(desk.view.problem(), QString());
    }

    void aFileThatWasMadeIsWrittenWhereItWasToBeKept()
    {
        Desk desk;
        desk.savesTo = desk.folder.filePath(u"wall.qkvm"_s);
        desk.view.exportWall({u"devices"_s, u"wall"_s}, u"a passphrase of mine"_s, u"my own"_s);
        QCOMPARE(desk.exportAsked.count(), 1);
        QCOMPARE(desk.exportAsked.at(0).at(1).toStringList(), (QStringList{u"devices"_s, u"wall"_s}));
        QCOMPARE(desk.exportAsked.at(0).at(2).toString(), u"a passphrase of mine"_s);
        QCOMPARE(desk.exportAsked.at(0).at(3).toString(), u"my own"_s);
        QVERIFY(desk.view.isBusy());
        QVERIFY(desk.view.holdsItsWindow());
        QVERIFY(!desk.view.isTaking());
        // Asked once: a second press while the server works asks nothing.
        desk.view.exportWall({u"devices"_s}, u"a passphrase of mine"_s, u"my own"_s);
        QCOMPARE(desk.exportAsked.count(), 1);
        QCOMPARE(desk.suggestions.size(), 1);

        desk.view.exported(desk.exportTurn(), made());
        QVERIFY(!desk.view.isBusy());
        QCOMPARE(Desk::read(desk.savesTo), sealedFile);
        QCOMPARE(desk.done.count(), 1);
        QCOMPARE(desk.done.at(0).at(0).toString(),
                 u"Exported. The file opens with its passphrase only: keep the two apart."_s);
        QCOMPARE(desk.done.at(0).at(1).toBool(), true);
        // An answer nobody waits for is nobody's: nothing is written a second time, and nothing
        // is said to be wrong.
        QFile::remove(desk.savesTo);
        desk.view.exported(desk.exportTurn(), made());
        QVERIFY(!QFile::exists(desk.savesTo));
        QCOMPARE(desk.done.count(), 1);
        QCOMPARE(desk.view.problem(), QString());
    }

    void whatIsNotInAFileIsSaidWhenItWasMade_data()
    {
        QTest::addColumn<bool>("withPassphrase");
        QTest::addColumn<int>("notOpened");
        QTest::addColumn<QString>("said");
        QTest::addColumn<bool>("good");
        QTest::newRow("a plain file") << false << 0 << u"Exported, without passwords"_s << true;
        QTest::newRow("one that does not open")
            << true << 1
            << u"Exported, without one saved password or text that does not open any more. qKVM's log says which. Give it again on the wall, then export once more."_s
            << false;
        QTest::newRow("three that do not open")
            << true << 3
            << u"Exported, without 3 saved passwords or texts that do not open any more. qKVM's log says which. Give them again on the wall, then export once more."_s
            << false;
    }

    void whatIsNotInAFileIsSaidWhenItWasMade()
    {
        QFETCH(bool, withPassphrase);
        QFETCH(int, notOpened);
        QFETCH(QString, said);
        QFETCH(bool, good);
        Desk desk;
        desk.savesTo = desk.folder.filePath(u"wall.out"_s);
        desk.view.exportWall({u"devices"_s}, withPassphrase ? u"a passphrase of mine"_s : QString(), u"my own"_s);
        desk.view.exported(desk.exportTurn(), made(notOpened, withPassphrase));
        QCOMPARE(desk.done.count(), 1);
        QCOMPARE(desk.done.at(0).at(0).toString(), said);
        QCOMPARE(desk.done.at(0).at(1).toBool(), good);
    }

    void aFileThatCouldNotBeMadeOrWrittenIsSaidInTheBox()
    {
        Desk desk;
        desk.savesTo = desk.folder.filePath(u"wall.json"_s);
        desk.view.exportWall({u"devices"_s}, {}, u"not it"_s);
        desk.view.exported(desk.exportTurn(), base::Error{"bad_password", "Wrong password"});
        QCOMPARE(desk.view.problem(), u"Wrong password"_s);
        QVERIFY(!desk.view.isBusy());
        QCOMPARE(desk.done.count(), 0);
        QVERIFY(!QFile::exists(desk.savesTo));
        // The next try says nothing of the one before while it is under way.
        desk.view.exportWall({u"devices"_s}, {}, u"my own"_s);
        QCOMPARE(desk.view.problem(), QString());
        // A place that cannot be written to: a folder that is not there.
        desk.view.exported(desk.exportTurn(),
                           base::Error{"busy", "Another file is being worked on just now. Try again in a moment."});
        desk.savesTo = desk.folder.filePath(u"no such folder/wall.json"_s);
        desk.view.exportWall({u"devices"_s}, {}, u"my own"_s);
        desk.view.exported(desk.exportTurn(), made(0, false));
        QVERIFY2(desk.view.problem().startsWith(u"The file could not be written: "_s), qPrintable(desk.view.problem()));
        QCOMPARE(desk.done.count(), 0);
        // And Windows' own box that does not come up is said, and is not taken for a no.
        desk.doorIsStuck = true;
        desk.view.exportWall({u"devices"_s}, {}, u"my own"_s);
        QCOMPARE(desk.view.problem(), u"Windows did not open its box for choosing a file."_s);
        QVERIFY(!desk.view.isBusy());
        QCOMPARE(desk.exportAsked.count(), 3);
    }

    void whoeverComesBackFromWindowsBoxToSomethingElseHasAskedForNothing()
    {
        // The program goes on while Windows' box is open: nothing else of this view's is asked
        // meanwhile, and where the wall was left or the box closed behind it, the password and
        // the passphrase go nowhere.
        Desk desk;
        desk.savesTo = desk.folder.filePath(u"wall.qkvm"_s);
        int askedMeanwhile = 0;
        desk.meanwhile = [&] {
            desk.view.exportWall({u"devices"_s}, {}, u"my own"_s);
            desk.view.chooseFile();
            desk.view.readFile({});
            askedMeanwhile = static_cast<int>(desk.suggestions.size()) + desk.opened;
        };
        desk.view.exportWall({u"devices"_s}, u"a passphrase of mine"_s, u"my own"_s);
        QVERIFY(desk.wasBusyWhileAsked);
        QCOMPARE(askedMeanwhile, 1);
        QCOMPARE(desk.exportAsked.count(), 1);
        desk.view.exported(desk.exportTurn(), made());
        QCOMPARE(desk.done.count(), 1);

        // (For as long as the box is there, the window under it is not to go: also when nothing
        // is waited for any more.)
        bool heldTheWindow = false;
        bool wasBusy = true;
        desk.meanwhile = [&] {
            desk.view.reset();
            wasBusy = desk.view.isBusy();
            heldTheWindow = desk.view.holdsItsWindow();
            // (Nothing is waited for: and still no second box is opened behind the first.)
            desk.meanwhile = nullptr;
            desk.view.exportWall({u"devices"_s}, {}, u"my own"_s);
            desk.view.chooseFile();
        };
        const qsizetype askedBefore = desk.suggestions.size() + desk.opened;
        desk.view.exportWall({u"devices"_s}, u"a passphrase of mine"_s, u"my own"_s);
        QCOMPARE(desk.suggestions.size() + desk.opened, askedBefore + 1);
        QCOMPARE(desk.exportAsked.count(), 1);
        QVERIFY(!desk.view.isBusy());
        QVERIFY(!wasBusy);
        QVERIFY(heldTheWindow);
        QVERIFY(!desk.view.holdsItsWindow());
        // The same for a file that is to be opened: it is not read in for whoever left.
        desk.opens = desk.write(u"plain.json"_s, plainFile);
        heldTheWindow = false;
        desk.meanwhile = [&] {
            desk.view.reset();
            heldTheWindow = desk.view.holdsItsWindow();
        };
        desk.view.chooseFile();
        QVERIFY(desk.wasBusyWhileAsked);
        QCOMPARE(desk.view.fileName(), QString());
        QVERIFY(!desk.view.isBusy());
        QVERIFY(heldTheWindow);
        QVERIFY(!desk.view.holdsItsWindow());
        desk.meanwhile = nullptr;
        desk.view.chooseFile();
        QCOMPARE(desk.view.fileName(), u"plain.json"_s);
    }

    void anAnswerIsTakenOnlyForWhatIsWaitedForJustNow()
    {
        Desk desk;
        desk.savesTo = desk.folder.filePath(u"wall.qkvm"_s);
        desk.opens = desk.write(u"plain.json"_s, plainFile);
        // A file was asked for; the box was closed and opened again, and another is asked for.
        desk.view.exportWall({u"devices"_s}, u"a passphrase of mine"_s, u"my own"_s);
        const quint64 first = desk.exportTurn();
        desk.view.reset();
        QVERIFY(!desk.view.isBusy());
        desk.view.exportWall({u"devices"_s}, {}, u"my own"_s);
        QVERIFY(desk.exportTurn() != first);
        // The first one's answer is nobody's: it is not written where the second is to be kept.
        desk.view.exported(first, made());
        QVERIFY(!QFile::exists(desk.savesTo));
        QVERIFY(desk.view.isBusy());
        QCOMPARE(desk.done.count(), 0);
        // Neither is an answer of another kind under the number that is waited for.
        desk.view.fileWasRead(desk.exportTurn(), aFile());
        desk.view.fileWasTaken(desk.exportTurn(), core::Taken{});
        QVERIFY(!desk.view.wasRead());
        QVERIFY(desk.view.isBusy());
        QCOMPARE(desk.done.count(), 0);
        desk.view.exported(desk.exportTurn(), made(0, false));
        QCOMPARE(desk.done.count(), 1);
        QCOMPARE(desk.done.at(0).at(0).toString(), u"Exported, without passwords"_s);

        // A reading that was given up fills no box that was opened anew.
        desk.view.chooseFile();
        desk.view.readFile({});
        const quint64 reading = desk.readTurn();
        desk.view.reset();
        desk.view.fileWasRead(reading, aFile());
        QVERIFY(!desk.view.wasRead());
        QVERIFY(desk.view.parts().isEmpty());
        // And a file that was read is not said to be written or taken by an answer that is neither.
        desk.view.chooseFile();
        desk.view.readFile({});
        desk.view.exported(desk.readTurn(), made());
        desk.view.fileWasTaken(desk.readTurn(), core::Taken{});
        QVERIFY(desk.view.isBusy());
        QCOMPARE(desk.done.count(), 1);
        QCOMPARE(desk.view.problem(), QString());

        // An answer is taken once. Come a second time, it is not the answer to what was asked
        // after it, which has a number of its own.
        desk.view.reset();
        QFile::remove(desk.savesTo);
        desk.view.exportWall({u"devices"_s}, {}, u"not it"_s);
        const quint64 refused = desk.exportTurn();
        desk.view.exported(refused, base::Error{"bad_password", "Wrong password"});
        desk.view.exportWall({u"devices"_s}, {}, u"my own"_s);
        QVERIFY(desk.exportTurn() != refused);
        desk.view.exported(refused, made());
        QVERIFY(!QFile::exists(desk.savesTo));
        QVERIFY(desk.view.isBusy());
        desk.view.exported(desk.exportTurn(), made(0, false));
        QVERIFY(QFile::exists(desk.savesTo));
        QCOMPARE(desk.done.count(), 2);
    }

    void aFileIsChosenAndSaysWhetherItAsksForAPassphrase()
    {
        Desk desk;
        // Nobody chooses one: nothing changes.
        desk.view.chooseFile();
        QCOMPARE(desk.opened, 1);
        QCOMPARE(desk.view.fileName(), QString());
        QVERIFY(!desk.view.isBusy());
        desk.view.readFile({});
        QCOMPARE(desk.view.problem(), u"Choose a file first."_s);
        QCOMPARE(desk.readAsked.count(), 0);

        desk.opens = desk.write(u"plain.json"_s, plainFile);
        desk.view.chooseFile();
        QCOMPARE(desk.view.fileName(), u"plain.json"_s);
        QVERIFY(!desk.view.isSealed());
        QCOMPARE(desk.view.problem(), QString());
        // A plain file is read without a passphrase, whatever stands in the box.
        desk.view.readFile(u"left over"_s);
        QCOMPARE(desk.readAsked.count(), 1);
        QCOMPARE(desk.readAsked.at(0).at(1).toByteArray(), plainFile);
        QCOMPARE(desk.readAsked.at(0).at(2).toString(), QString());
        QVERIFY(desk.view.isBusy());
        // While the server reads, nothing else is asked: not the server again, and not Windows.
        desk.view.readFile({});
        desk.view.chooseFile();
        QCOMPARE(desk.readAsked.count(), 1);
        QCOMPARE(desk.opened, 2);
        desk.view.fileWasRead(desk.readTurn(), base::Error{"not_a_file", "This is not a file that qKVM made"});
        QCOMPARE(desk.view.problem(), u"This is not a file that qKVM made"_s);
        QVERIFY(!desk.view.wasRead());
        // (That it is no file of QKVM's is no reason to ask for a passphrase.)
        QVERIFY(!desk.view.isSealed());

        // Asked for another and none chosen: the one that was chosen is still the file.
        desk.opens.clear();
        desk.view.chooseFile();
        QCOMPARE(desk.view.fileName(), u"plain.json"_s);
        QCOMPARE(desk.view.problem(), u"This is not a file that qKVM made"_s);

        desk.opens = desk.write(u"kept.qkvm"_s, sealedFile);
        desk.view.chooseFile();
        QCOMPARE(desk.view.fileName(), u"kept.qkvm"_s);
        QVERIFY(desk.view.isSealed());
        QCOMPARE(desk.view.problem(), QString());
        desk.view.readFile(u"of the file"_s);
        QCOMPARE(desk.readAsked.count(), 2);
        QCOMPARE(desk.readAsked.at(1).at(1).toByteArray(), sealedFile);
        QCOMPARE(desk.readAsked.at(1).at(2).toString(), u"of the file"_s);
    }

    void aFileThatIsNoneOfQkvmsIsNotReadIn()
    {
        Desk desk;
        desk.opens = desk.write(u"film.json"_s, QByteArray(core::largestFileBytes + 1, ' '));
        desk.view.chooseFile();
        QCOMPARE(desk.view.problem(), u"The file is larger than anything qKVM writes."_s);
        QCOMPARE(desk.view.fileName(), QString());
        desk.view.readFile({});
        QCOMPARE(desk.readAsked.count(), 0);
        // One that is as large as a file may be is one.
        desk.opens =
            desk.write(u"large.json"_s, plainFile + QByteArray(core::largestFileBytes - plainFile.size(), ' '));
        desk.view.chooseFile();
        QCOMPARE(desk.view.fileName(), u"large.json"_s);
        // One that is not there any more: said, and the one before is not the file any more.
        desk.opens = desk.folder.filePath(u"gone.json"_s);
        desk.view.chooseFile();
        QVERIFY2(desk.view.problem().startsWith(u"The file could not be read: "_s), qPrintable(desk.view.problem()));
        QCOMPARE(desk.view.fileName(), QString());
        // And Windows' own box that does not come up.
        desk.doorIsStuck = true;
        desk.view.chooseFile();
        QCOMPARE(desk.view.problem(), u"Windows did not open its box for choosing a file."_s);
        QVERIFY(!desk.view.isBusy());
    }

    void aFileThatTurnsOutToBeEncryptedAsksForItsPassphrase()
    {
        // (It did not say so in the open: the server did.)
        Desk desk;
        desk.opens = desk.write(u"odd.json"_s, plainFile);
        desk.view.chooseFile();
        desk.view.readFile({});
        desk.view.fileWasRead(
            desk.readTurn(),
            base::Error{"passphrase_needed", "This file is encrypted. Enter the passphrase it was made with."});
        QVERIFY(desk.view.isSealed());
        QCOMPARE(desk.view.problem(), u"This file is encrypted. Enter the passphrase it was made with."_s);
        desk.view.readFile(u"of the file"_s);
        QCOMPARE(desk.readAsked.at(1).at(2).toString(), u"of the file"_s);
        // (While it is read again, what was wrong the time before is not said any more.)
        QCOMPARE(desk.view.problem(), QString());
        desk.view.fileWasRead(desk.readTurn(),
                              base::Error{"wrong_passphrase", "That is not the passphrase this file was made with"});
        QVERIFY(desk.view.isSealed());
        QVERIFY(!desk.view.wasRead());
        QCOMPARE(desk.view.problem(), u"That is not the passphrase this file was made with"_s);
    }

    void whatAFileHoldsIsSaidLineByLineBeforeAnythingIsTaken()
    {
        Desk desk;
        desk.hasRead(u"kept.qkvm"_s, sealedFile, aFile(), u"of the file"_s);
        QVERIFY(desk.view.wasRead());
        QVERIFY(!desk.view.isBusy());
        QVERIFY2(desk.view.about().startsWith(u"An encrypted file, made by qKVM 0.1.0, on "_s),
                 qPrintable(desk.view.about()));
        QVERIFY(desk.view.about().endsWith(u'.'));
        QVERIFY(!desk.view.hasPlainSecrets());
        QCOMPARE(desk.view.parts().size(), 4);
        QVERIFY(desk.view.canTake());

        const QVariantMap devices = partOf(desk.view, u"devices"_s);
        QCOMPARE(devices.value(u"label"_s).toString(), u"KVMs and servers: 2 to add"_s);
        QCOMPARE(
            devices.value(u"hint"_s).toString(),
            u"1 are on this wall already and stay as they are. 1 are left out as the same device as one that is here or earlier in the file (NAS again: Storage)."_s);
        QVERIFY(devices.value(u"can"_s).toBool());
        QVERIFY(devices.value(u"ticked"_s).toBool());
        QCOMPARE(devices.value(u"lines"_s).toList().size(), 2);
        QCOMPARE(lineOf(desk.view, u"devices"_s, 0), u"Build server · 192.168.1.21"_s);
        QCOMPARE(
            lineOf(desk.view, u"devices"_s, 1),
            u"Rack · 192.168.1.30:8443 · its certificate is not checked · no password · console link to 192.168.1.30:9443 · group Lab"_s);

        const QVariantMap actions = partOf(desk.view, u"actions"_s);
        QCOMPARE(actions.value(u"label"_s).toString(), u"Saved actions: 1 to add"_s);
        QCOMPARE(actions.value(u"hint"_s).toString(), u"1 are left out: an action of that name is here already."_s);
        QVERIFY(actions.value(u"can"_s).toBool());
        QVERIFY(actions.value(u"ticked"_s).toBool());
        QCOMPARE(actions.value(u"lines"_s).toList().size(), 1);
        QCOMPARE(lineOf(desk.view, u"actions"_s, 0), u"Restart now · types a text, then Enter · asks before it runs"_s);
        // What a button will type is not taken unread.
        QCOMPARE(lineOf(desk.view, u"actions"_s, 0, u"typed"_s), u"shutdown /r /t 0"_s);

        // What replaces something that is set here is asked for, not assumed.
        const QVariantMap alerts = partOf(desk.view, u"alerts"_s);
        QCOMPARE(alerts.value(u"label"_s).toString(), u"Alert settings"_s);
        QCOMPARE(
            alerts.value(u"hint"_s).toString(),
            u"Replaces what is set here: alerts on, told about: went offline, is over. They will go to hooks.example.com, the address in the file."_s);
        QVERIFY(alerts.value(u"can"_s).toBool());
        QVERIFY(!alerts.value(u"ticked"_s).toBool());
        const QVariantMap wall = partOf(desk.view, u"wall"_s);
        QCOMPARE(wall.value(u"hint"_s).toString(),
                 u"Replaces what is set here: a picture every 2.5 s, does not ask for newer versions."_s);
        QVERIFY(wall.value(u"can"_s).toBool());
        QVERIFY(!wall.value(u"ticked"_s).toBool());
        QCOMPARE(desk.view.leftOut(), u"1 entry was left out: device 5: no address"_s);
    }

    void eachThingADeviceOrAnActionHasIsSaidByItself()
    {
        Desk desk;
        core::FileContents file;
        core::FileDevice checked = device(u"Web"_s, u"kvm.example.com"_s);
        checked.tls = u"system"_s;
        core::FileDevice linked = device(u"Far"_s, u"192.168.1.40"_s);
        linked.openUrl = u"https:"_s + u"//192.168.1.40/kvm/"_s;
        core::FileDevice long6 = device(u"Six"_s, u"192.168.1.41"_s);
        long6.openUrl = u"http:"_s + u"//[2001:db8::1]:8443/"_s;
        core::FileDevice usual = device(u"Usual"_s, u"192.168.1.42"_s);
        usual.openUrl = u"https:"_s + u"//kvm.example.com:443/"_s;
        core::FileDevice plain = device(u"Plain"_s, u"192.168.1.43"_s);
        plain.openUrl = u"http:"_s + u"//kvm.example.com:80/"_s;
        // (A long numbered address may end in four numbers with dots, and is one all the same.)
        core::FileDevice mixed = device(u"Mixed"_s, u"192.168.1.44"_s);
        mixed.openUrl = u"http:"_s + u"//[64:ff9b::192.168.1.44]:8443/"_s;
        file.devices = QList<core::FileDevice>{checked, linked, long6, usual, plain, mixed};
        for (int number = 1; number <= 4; ++number) {
            core::FileDevice twin = device(u"Twin %1"_s.arg(number), u"192.168.1.5%1"_s.arg(number));
            twin.isNew = false;
            twin.sameAs = u"Tile %1"_s.arg(number);
            file.devices->append(twin);
        }
        core::FileAction enters = typing(u"Says"_s, u"hello"_s);
        enters.enter = true;
        core::FileAction asks = typing(u"Asks"_s, u"bye"_s);
        asks.danger = true;
        core::FileAction presses;
        presses.name = u"Lock"_s;
        presses.keys = u"MetaLeft,KeyL"_s;
        presses.isNew = true;
        file.actions = QList<core::FileAction>{enters, asks, presses};
        core::FileAlerts alerts;
        alerts.kinds = {u"offline"_s, u"no_signal"_s, u"problem"_s, u"recovered"_s, u"something new"_s};
        file.alerts = alerts;
        core::FileWallSettings wall;
        wall.pictureEverySeconds = 4;
        file.wall = wall;
        desk.hasRead(u"plain.json"_s, plainFile, file);

        QCOMPARE(lineOf(desk.view, u"devices"_s, 0),
                 u"Web · kvm.example.com · its certificate is checked against the public list"_s);
        // Of a console link, where it leads: the name and the port, as a browser would say them.
        QCOMPARE(lineOf(desk.view, u"devices"_s, 1), u"Far · 192.168.1.40 · console link to 192.168.1.40"_s);
        QCOMPARE(lineOf(desk.view, u"devices"_s, 2), u"Six · 192.168.1.41 · console link to [2001:db8::1]:8443"_s);
        QCOMPARE(lineOf(desk.view, u"devices"_s, 3), u"Usual · 192.168.1.42 · console link to kvm.example.com"_s);
        QCOMPARE(lineOf(desk.view, u"devices"_s, 4), u"Plain · 192.168.1.43 · console link to kvm.example.com"_s);
        QVERIFY2(lineOf(desk.view, u"devices"_s, 5).startsWith(u"Mixed · 192.168.1.44 · console link to [64:ff9b::"_s),
                 qPrintable(lineOf(desk.view, u"devices"_s, 5)));
        QVERIFY(lineOf(desk.view, u"devices"_s, 5).endsWith(u"]:8443"_s));
        // Of the devices that are left out as another's twin, the first three by name.
        QCOMPARE(
            partOf(desk.view, u"devices"_s).value(u"hint"_s).toString(),
            u"4 are left out as the same device as one that is here or earlier in the file (Twin 1: Tile 1; Twin 2: Tile 2; Twin 3: Tile 3; …)."_s);
        QCOMPARE(lineOf(desk.view, u"actions"_s, 0), u"Says · types a text, then Enter"_s);
        QCOMPARE(lineOf(desk.view, u"actions"_s, 1), u"Asks · types a text · asks before it runs"_s);
        QCOMPARE(lineOf(desk.view, u"actions"_s, 2), u"Lock · presses Win+L"_s);
        QCOMPARE(lineOf(desk.view, u"actions"_s, 2, u"typed"_s), QString());
        QCOMPARE(partOf(desk.view, u"actions"_s).value(u"hint"_s).toString(), QString());
        QCOMPARE(
            partOf(desk.view, u"alerts"_s).value(u"hint"_s).toString(),
            u"Replaces what is set here: alerts off, told about: went offline, lost its picture, has a problem, is over, something new. The address this wall sends them to is kept."_s);
        QCOMPARE(partOf(desk.view, u"wall"_s).value(u"hint"_s).toString(),
                 u"Replaces what is set here: a picture every 4 s."_s);
    }

    // A sign by its number. (Those that draw nothing, or turn a line around, or look like a
    // letter they are not, stand nowhere in this file as themselves: nobody could see that they
    // are there, and a compiler takes some of them for an attack on the program, and rightly.)
    static QString sign(char32_t number) { return QString::fromUcs4(&number, 1); }

    void nothingAFileSaysGoesUnseen()
    {
        // A file may come from somebody else. What draws nothing, or turns a line around, or
        // begins another line, is written out as its number: in a name, and above all in what a
        // button will type.
        Desk desk;
        core::FileContents file;
        core::FileDevice odd = device(u"NAS"_s + sign(0x200B), u"192.168.1.22\n192.168.1.99"_s);
        odd.group = u"safe"_s + sign(0x202E) + u"txt.exe"_s;
        core::FileDevice twin = device(u"Twin"_s + sign(0x2028), u"192.168.1.23"_s);
        twin.isNew = false;
        twin.sameAs = u"Tile\a"_s;
        file.devices = QList<core::FileDevice>{odd, twin};
        // A tag character, which draws nothing: between what is seen and what is done. And a
        // line's end at the very end, which is an Enter that nobody would see.
        core::FileAction hidden =
            typing(u"Tidy"_s + sign(0x200F) + u"up"_s,
                   u"echo ok"_s + sign(0xE0041) + u"\trm -rf /\r\nexit"_s + sign(0x2028) + u"now\n"_s);
        file.actions = QList<core::FileAction>{hidden};
        core::FileAlerts alerts;
        alerts.hasAddress = true;
        alerts.host = u"hooks"_s + sign(0x00AD) + u".example.com"_s;
        alerts.kinds = {u"new"_s + sign(0x200B) + u"kind"_s};
        file.alerts = alerts;
        file.program = u"0.1"_s + sign(0x202E) + u"0"_s;
        file.leftOut = {u"device 3:"_s + sign(0x2029) + u"gone"_s};
        desk.hasRead(u"plain.json"_s, plainFile, file);

        QCOMPARE(
            lineOf(desk.view, u"devices"_s, 0),
            u"NAS<U+200B> · a place that is no plain one: 192.168.1.22<U+000A>192.168.1.99 · group safe<U+202E>txt.exe"_s);
        QCOMPARE(
            partOf(desk.view, u"devices"_s).value(u"hint"_s).toString(),
            u"1 are left out as the same device as one that is here or earlier in the file (Twin<U+2028>: Tile<U+0007>)."_s);
        QCOMPARE(lineOf(desk.view, u"actions"_s, 0), u"Tidy<U+200F>up · types a text"_s);
        // What is typed is shown as it will be typed: a line's end is a line's end, and Enter,
        // and is marked where it stands. (And only that one: a line's end of another kind is no
        // key of the keyboard's.)
        QCOMPARE(lineOf(desk.view, u"actions"_s, 0, u"typed"_s),
                 u"echo ok<U+E0041><U+0009>rm -rf /<U+000D>↵\nexit<U+2028>now↵\n"_s);
        QCOMPARE(
            partOf(desk.view, u"alerts"_s).value(u"hint"_s).toString(),
            u"Replaces what is set here: alerts off, told about: new<U+200B>kind. They will go to hooks.example.com, the address in the file."_s);
        // (A place is said in its plain form, which is where a connection goes: a sign that this
        // form leaves out, as the hyphen that is only shown at a line's end, is not in it.)
        QCOMPARE(desk.view.about(), u"A plain file, made by qKVM 0.1<U+202E>0."_s);
        QCOMPARE(desk.view.leftOut(), u"1 entry was left out: device 3:<U+2029>gone"_s);
    }

    void aBlankThatIsNoneAndALetterThatIsOneAreWrittenOutToo_data()
    {
        // (Not of the kind that says of itself that it draws nothing: the server has a list of
        // them, and this is that list.)
        QTest::addColumn<uint>("number");
        QTest::addColumn<QString>("written");
        QTest::newRow("a blank that does not break") << 0x00A0U << u"<U+00A0>"_s;
        QTest::newRow("a wide blank") << 0x3000U << u"<U+3000>"_s;
        QTest::newRow("a thin blank") << 0x2009U << u"<U+2009>"_s;
        QTest::newRow("a joiner of marks") << 0x034FU << u"<U+034F>"_s;
        QTest::newRow("a filler") << 0x115FU << u"<U+115F>"_s;
        QTest::newRow("another filler") << 0x1160U << u"<U+1160>"_s;
        QTest::newRow("a vowel that is not drawn") << 0x17B4U << u"<U+17B4>"_s;
        QTest::newRow("another of them") << 0x17B5U << u"<U+17B5>"_s;
        QTest::newRow("the dots for the blind, with no dot") << 0x2800U << u"<U+2800>"_s;
        QTest::newRow("a filler of half width") << 0xFFA0U << u"<U+FFA0>"_s;
        QTest::newRow("a filler of full width") << 0x3164U << u"<U+3164>"_s;
        QTest::newRow("the first that picks a drawing, of one script") << 0x180BU << u"<U+180B>"_s;
        QTest::newRow("the last of them") << 0x180FU << u"<U+180F>"_s;
        QTest::newRow("the first that picks a drawing") << 0xFE00U << u"<U+FE00>"_s;
        QTest::newRow("the last of them, as after a picture") << 0xFE0FU << u"<U+FE0F>"_s;
        QTest::newRow("the first of the many more") << 0xE0100U << u"<U+E0100>"_s;
        QTest::newRow("the last of the many more") << 0xE01EFU << u"<U+E01EF>"_s;
    }

    void aBlankThatIsNoneAndALetterThatIsOneAreWrittenOutToo()
    {
        QFETCH(uint, number);
        QFETCH(QString, written);
        Desk desk;
        core::FileContents file;
        file.devices = QList<core::FileDevice>{device(u"a"_s + sign(number) + u"b"_s, u"192.168.1.22"_s)};
        file.actions = QList<core::FileAction>{typing(u"Types"_s, u"x"_s + sign(number) + u"y"_s)};
        desk.hasRead(u"plain.json"_s, plainFile, file);
        QCOMPARE(lineOf(desk.view, u"devices"_s, 0), u"a"_s + written + u"b · 192.168.1.22"_s);
        QCOMPARE(lineOf(desk.view, u"actions"_s, 0, u"typed"_s), u"x"_s + written + u"y"_s);
    }

    void whatIsWrittenInOtherLettersIsLeftAsItIs()
    {
        // A name is a name, in whatever alphabet; so are the signs beside the listed ones.
        Desk desk;
        core::FileContents named;
        const QString inCyrillic =
            sign(0x0421) + sign(0x0435) + sign(0x0440) + sign(0x0432) + sign(0x0435) + sign(0x0440) + u" 1"_s;
        const QString inKanaWithAPicture = sign(0x30B5) + sign(0x30FC) + sign(0x30D0) + sign(0x1F5A5);
        // (The signs just before and after those that are written out.)
        const QString neighbours = sign(0x180A) + sign(0x1810) + sign(0xFE10) + sign(0x115E) + sign(0x1161);
        named.devices = QList<core::FileDevice>{device(inCyrillic, u"192.168.1.24"_s),
                                                device(inKanaWithAPicture, u"192.168.1.25"_s),
                                                device(neighbours, u"192.168.1.26"_s)};
        desk.hasRead(u"plain.json"_s, plainFile, named);
        QCOMPARE(lineOf(desk.view, u"devices"_s, 0), inCyrillic + u" · 192.168.1.24"_s);
        QCOMPARE(lineOf(desk.view, u"devices"_s, 1), inKanaWithAPicture + u" · 192.168.1.25"_s);
        QCOMPARE(lineOf(desk.view, u"devices"_s, 2), neighbours + u" · 192.168.1.26"_s);
    }

    void aNameCannotPoseAsWhatIsSaidOfIt()
    {
        // The dot between the things said of a device is this program's: one that a name brings
        // along is written out, so that the name ends where it ends.
        Desk desk;
        core::FileContents file;
        file.devices = QList<core::FileDevice>{device(u"NAS · 192.168.1.99 · no password"_s, u"192.168.1.22"_s)};
        file.actions = QList<core::FileAction>{typing(u"Tidy · presses Enter"_s, u"a · b"_s)};
        desk.hasRead(u"plain.json"_s, plainFile, file);
        QCOMPARE(lineOf(desk.view, u"devices"_s, 0),
                 u"NAS <U+00B7> 192.168.1.99 <U+00B7> no password · 192.168.1.22"_s);
        QCOMPARE(lineOf(desk.view, u"actions"_s, 0), u"Tidy <U+00B7> presses Enter · types a text"_s);
        // (In what is typed it is a letter like any other, and is typed.)
        QCOMPARE(lineOf(desk.view, u"actions"_s, 0, u"typed"_s), u"a · b"_s);
    }

    void aDevicesOwnPlaceIsSaidInTheLettersThatCannotBeTakenForOthers()
    {
        // Where a device is, is where a password goes that is typed for it afterwards; and where
        // alerts go is where what happens on the wall is told.
        Desk desk;
        core::FileContents file;
        const QString copied = u"ex"_s + sign(0x0430) + u"mple.com"_s;
        file.devices = QList<core::FileDevice>{
            device(u"Copy"_s, u"kvm."_s + copied + u":8443"_s), device(u"Long"_s, u"[2001:db8::1]:8443"_s),
            device(u"Odd"_s, u"kvm."_s + copied + u"/x y"_s), device(u"NoPort"_s, u"kvm."_s + copied)};
        core::FileAlerts alerts;
        alerts.hasAddress = true;
        alerts.host = u"hooks."_s + copied;
        file.alerts = alerts;
        desk.hasRead(u"plain.json"_s, plainFile, file);
        QCOMPARE(lineOf(desk.view, u"devices"_s, 0), u"Copy · kvm.xn--exmple-4nf.com:8443"_s);
        QCOMPARE(lineOf(desk.view, u"devices"_s, 1), u"Long · [2001:db8::1]:8443"_s);
        // (What cannot be read as a place is shown whole, with the letter written out.)
        QCOMPARE(lineOf(desk.view, u"devices"_s, 2),
                 u"Odd · a place that is no plain one: kvm.ex<U+0430>mple.com/x y"_s);
        QCOMPARE(lineOf(desk.view, u"devices"_s, 3), u"NoPort · kvm.xn--exmple-4nf.com"_s);
        QCOMPARE(
            partOf(desk.view, u"alerts"_s).value(u"hint"_s).toString(),
            u"Replaces what is set here: alerts off, told about: nothing. They will go to hooks.xn--exmple-4nf.com, the address in the file."_s);
    }

    void aPlaceWithMoreInItThanAPlaceIsNotReadAsOne_data()
    {
        // What begins something else in an address: with one of these in it, what the server
        // names as a device's place is shown whole, with every letter of another alphabet
        // written out, and not as the place a part of it would be.
        QTest::addColumn<QString>("more");
        QTest::newRow("a way") << u"/x"_s;
        QTest::newRow("a question") << u"?x"_s;
        QTest::newRow("a mark") << u"#x"_s;
        QTest::newRow("a name before it") << u"@kvm.example.com"_s;
        // (And a backslash, with which Qt reads no place at all.)
        QTest::newRow("a backslash") << u"\\x"_s;
    }

    void aPlaceWithMoreInItThanAPlaceIsNotReadAsOne()
    {
        QFETCH(QString, more);
        Desk desk;
        core::FileContents file;
        file.devices = QList<core::FileDevice>{device(u"Odd"_s, u"ex"_s + sign(0x0430) + u"mple.com"_s + more)};
        desk.hasRead(u"plain.json"_s, plainFile, file);
        QCOMPARE(lineOf(desk.view, u"devices"_s, 0),
                 u"Odd · a place that is no plain one: ex<U+0430>mple.com"_s + more);
        // In plain letters as well: with a name before it, the first part would read as the place
        // and the second be it.
        core::FileContents plain;
        plain.devices = QList<core::FileDevice>{device(u"Plain"_s, u"kvm.example.com"_s + more)};
        desk.view.readFile({});
        desk.view.fileWasRead(desk.readTurn(), plain);
        QCOMPARE(lineOf(desk.view, u"devices"_s, 0), u"Plain · a place that is no plain one: kvm.example.com"_s + more);
    }

    void aPlaceInNumbersOfAnotherAlphabetIsNoPlainOne()
    {
        // Digits of another alphabet come out as these ones where the name is put in its plain
        // form. Whether whoever follows the address reads them so is not known here.
        Desk desk;
        core::FileContents file;
        const QString wide = sign(0xFF11) + sign(0xFF12) + sign(0xFF17) + u"."_s + sign(0xFF11);
        file.devices = QList<core::FileDevice>{device(u"Wide"_s, wide)};
        desk.hasRead(u"plain.json"_s, plainFile, file);
        QCOMPARE(lineOf(desk.view, u"devices"_s, 0),
                 u"Wide · a place that is no plain one: <U+FF11><U+FF12><U+FF17>.<U+FF11>"_s);
    }

    void aConsoleLinkIsSaidInTheLettersThatCannotBeTakenForOthers()
    {
        // A name written with a letter of another alphabet reads as the one it copies. A browser
        // says such a name in its plain form, and so does this.
        Desk desk;
        core::FileContents file;
        core::FileDevice copied = device(u"Copy"_s, u"192.168.1.60"_s);
        copied.openUrl = u"https:"_s + u"//ex"_s + sign(0x0430) + u"mple.com/kvm"_s;
        file.devices = QList<core::FileDevice>{copied};
        desk.hasRead(u"plain.json"_s, plainFile, file);
        QCOMPARE(lineOf(desk.view, u"devices"_s, 0), u"Copy · 192.168.1.60 · console link to xn--exmple-4nf.com"_s);
    }

    void aConsoleLinkThatIsNoPlainAddressIsSaidAsThat_data()
    {
        // What programs read differently, or cannot be read as a place at all, is not said as a
        // place: what would be said here could be another one than the browser goes to.
        QTest::addColumn<QString>("link");
        QTest::addColumn<QString>("shown");
        const QString scheme = u"https:"_s;
        QTest::newRow("a backslash, which a browser takes for a stroke")
            << scheme + u"//kvm.example.org\\.corp.example.com/"_s
            << scheme + u"//kvm.example.org\\.corp.example.com/"_s;
        QTest::newRow("a tab, which a browser drops")
            << scheme + u"//kvm.exam\tple.com/"_s << scheme + u"//kvm.exam<U+0009>ple.com/"_s;
        QTest::newRow("a blank") << scheme + u"//kvm.example.com /x"_s << scheme + u"//kvm.example.com /x"_s;
        // (Also after the place, where the place itself would read the same to both.)
        QTest::newRow("a backslash after the place")
            << scheme + u"//kvm.example.com/a\\b"_s << scheme + u"//kvm.example.com/a\\b"_s;
        QTest::newRow("a tab after the place")
            << scheme + u"//kvm.example.com/a\tb"_s << scheme + u"//kvm.example.com/a<U+0009>b"_s;
        QTest::newRow("a blank after the place")
            << scheme + u"//kvm.example.com/a b"_s << scheme + u"//kvm.example.com/a b"_s;
        // A place written as numbers in another form than four of them with dots: programs do
        // not agree on where that is.
        for (const QString& numbers : {u"192.168.0x"_s, u"0x7f.1"_s, u"2130706433"_s, u"0177.0.0.1"_s, u"1.0x"_s,
                                       u"192.168.1"_s, u"kvm.example.0X1f"_s, u"7"_s}) {
            QTest::newRow(qPrintable(u"numbers: "_s + numbers))
                << scheme + u"//"_s + numbers + u"/"_s << scheme + u"//"_s + numbers + u"/"_s;
        }
        QTest::newRow("a port that is none")
            << scheme + u"//kvm.example.com:0/"_s << scheme + u"//kvm.example.com:0/"_s;
        QTest::newRow("a numbered address with a network card named in it")
            << scheme + u"//[fe80::1%25eth0]/"_s << scheme + u"//[fe80::1%25eth0]/"_s;
        QTest::newRow("a name and a password in it") << scheme + u"//somebody:something@kvm.example.com:8443/"_s
                                                     << scheme + u"//somebody:something@kvm.example.com:8443/"_s;
        QTest::newRow("no place") << u"not an address"_s << u"not an address"_s;
        QTest::newRow("no strokes, which a browser adds")
            << scheme + u"kvm.example.com"_s << scheme + u"kvm.example.com"_s;
        QTest::newRow("another way than the web's")
            << u"ftp:"_s + u"//kvm.example.com/"_s << u"ftp:"_s + u"//kvm.example.com/"_s;
        QTest::newRow("a script") << u"javascript:alert(1)"_s << u"javascript:alert(1)"_s;
        // (Where it cannot be said as a place, a letter of another alphabet is not left to be
        // read as one it is not either.)
        QTest::newRow("a look-alike letter and a backslash")
            << scheme + u"//ex"_s + sign(0x0430) + u"mple.com\\x"_s << scheme + u"//ex<U+0430>mple.com\\x"_s;
    }

    void aNameThatBeginsWithADigitIsAName()
    {
        // What a browser reads as numbers is a last part of digits only, or of sixteens. A name
        // that only begins with a digit is a name, and many a machine at home has one.
        Desk desk;
        core::FileContents file;
        core::FileDevice first = device(u"A"_s, u"192.168.1.71"_s);
        first.openUrl = u"http:"_s + u"//4kvm/"_s;
        core::FileDevice second = device(u"B"_s, u"192.168.1.72"_s);
        second.openUrl = u"https:"_s + u"//kvm.example.3com:8443/x"_s;
        core::FileDevice third = device(u"C"_s, u"192.168.1.73"_s);
        third.openUrl = u"https:"_s + u"//1kvm.example.com/"_s;
        file.devices = QList<core::FileDevice>{first, second, third};
        desk.hasRead(u"plain.json"_s, plainFile, file);
        QCOMPARE(lineOf(desk.view, u"devices"_s, 0), u"A · 192.168.1.71 · console link to 4kvm"_s);
        QCOMPARE(lineOf(desk.view, u"devices"_s, 1), u"B · 192.168.1.72 · console link to kvm.example.3com:8443"_s);
        QCOMPARE(lineOf(desk.view, u"devices"_s, 2), u"C · 192.168.1.73 · console link to 1kvm.example.com"_s);
    }

    void aSignThatLooksLikeTheDotBetweenIsWrittenOutToo_data()
    {
        QTest::addColumn<uint>("number");
        QTest::addColumn<QString>("written");
        QTest::newRow("the Greek one, which is the same sign") << 0x0387U << u"<U+0387>"_s;
        QTest::newRow("the runic one") << 0x16EBU << u"<U+16EB>"_s;
        QTest::newRow("the bullet") << 0x2022U << u"<U+2022>"_s;
        QTest::newRow("the one that parts a word") << 0x2027U << u"<U+2027>"_s;
        QTest::newRow("the one of sums") << 0x2219U << u"<U+2219>"_s;
        QTest::newRow("the one of products") << 0x22C5U << u"<U+22C5>"_s;
        QTest::newRow("the one that parts words") << 0x2E31U << u"<U+2E31>"_s;
        QTest::newRow("the one of another script") << 0x30FBU << u"<U+30FB>"_s;
        QTest::newRow("its half-wide form") << 0xFF65U << u"<U+FF65>"_s;
    }

    void aSignThatLooksLikeTheDotBetweenIsWrittenOutToo()
    {
        QFETCH(uint, number);
        QFETCH(QString, written);
        Desk desk;
        core::FileContents file;
        file.devices =
            QList<core::FileDevice>{device(u"NAS "_s + sign(number) + u" 192.168.1.99"_s, u"192.168.1.22"_s)};
        file.actions = QList<core::FileAction>{typing(u"Types"_s, u"a"_s + sign(number) + u"b"_s)};
        desk.hasRead(u"plain.json"_s, plainFile, file);
        QCOMPARE(lineOf(desk.view, u"devices"_s, 0), u"NAS "_s + written + u" 192.168.1.99 · 192.168.1.22"_s);
        // (What is typed is typed as it is.)
        QCOMPARE(lineOf(desk.view, u"actions"_s, 0, u"typed"_s), u"a"_s + sign(number) + u"b"_s);
    }

    void aConsoleLinkThatIsNoPlainAddressIsSaidAsThat()
    {
        QFETCH(QString, link);
        QFETCH(QString, shown);
        Desk desk;
        core::FileContents file;
        core::FileDevice odd = device(u"Odd"_s, u"192.168.1.43"_s);
        odd.openUrl = link;
        file.devices = QList<core::FileDevice>{odd};
        desk.hasRead(u"plain.json"_s, plainFile, file);
        QCOMPARE(lineOf(desk.view, u"devices"_s, 0),
                 u"Odd · 192.168.1.43 · a console link that is no plain address: "_s + shown);
    }

    void aFileOfAnotherKindThanWasAskedForIsNotKept()
    {
        // Asked for with a passphrase, and what came has none: kept under the name of an encrypted
        // file and called one, it would have every password of the wall in it to be read.
        Desk desk;
        desk.savesTo = desk.folder.filePath(u"wall.qkvm"_s);
        desk.view.exportWall({u"devices"_s}, u"a passphrase of mine"_s, u"my own"_s);
        desk.view.exported(desk.exportTurn(), made(0, false));
        QCOMPARE(desk.view.problem(),
                 u"qKVM answered with another kind of file than was asked for. It was not kept."_s);
        QVERIFY(!QFile::exists(desk.savesTo));
        QCOMPARE(desk.done.count(), 0);
        QVERIFY(!desk.view.isBusy());
        // And the other way round.
        desk.view.exportWall({u"devices"_s}, {}, u"my own"_s);
        desk.view.exported(desk.exportTurn(), made(0, true));
        QCOMPARE(desk.view.problem(),
                 u"qKVM answered with another kind of file than was asked for. It was not kept."_s);
        QVERIFY(!QFile::exists(desk.savesTo));
        QCOMPARE(desk.done.count(), 0);
    }

    void thePartsOfAFileSayWhenTheyAreOthersAndOnlyThen()
    {
        // (The screen makes its ticks anew when the parts say that they changed: said at any
        // other time, what somebody unticked would be ticked again.)
        Desk desk;
        const QSignalSpy partsChanged(&desk.view, &FilesView::partsChanged);
        desk.hasRead(u"kept.qkvm"_s, sealedFile, aFile(), u"of the file"_s);
        QCOMPARE(partsChanged.count(), 1);
        // Nothing ticked; a take that the server refuses; a take that is under way.
        desk.view.takeIn({}, u"of the file"_s);
        desk.view.takeIn({u"devices"_s}, u"of the file"_s);
        desk.view.fileWasTaken(desk.takeTurn(), base::Error{"busy", "Another file is being worked on just now."});
        desk.view.takeIn({u"devices"_s}, u"of the file"_s);
        QCOMPARE(partsChanged.count(), 1);
        QCOMPARE(desk.view.parts().size(), 4);
        // Taken: there are none any more.
        desk.view.fileWasTaken(desk.takeTurn(), core::Taken{});
        QCOMPARE(partsChanged.count(), 2);
        // A box with nothing in it that is closed says nothing of parts.
        desk.view.reset();
        QCOMPARE(partsChanged.count(), 2);
        // A file with no part in it has none to tell of.
        desk.hasRead(u"plain.json"_s, plainFile, core::FileContents{});
        QCOMPARE(partsChanged.count(), 2);
    }

    // A taking under way that the box gives up (its wall left, or no longer one it may take a file
    // into) is no taking given up at the server, which may take the file in all the same: said, so
    // that it is not handed in a second time unlooked; and its answer, if it comes, is nobody's.
    // What is given up with nothing under way says nothing.
    void aTakingThatIsCutOffSaysWhatMayHaveCome()
    {
        Desk desk;
        const QSignalSpy cutOff(&desk.view, &FilesView::cutOff);
        desk.hasRead(u"kept.qkvm"_s, sealedFile, aFile(), u"of the file"_s);
        desk.view.reset();
        QCOMPARE(cutOff.count(), 0);
        desk.hasRead(u"kept.qkvm"_s, sealedFile, aFile(), u"of the file"_s);
        desk.view.takeIn({u"devices"_s}, u"of the file"_s);
        QVERIFY(desk.view.isTaking());
        desk.view.reset();
        QCOMPARE(cutOff.count(), 1);
        QCOMPARE(
            cutOff.at(0).at(0).toString(),
            u"Taking the file in was cut off before the server said what came of it. The file may have been taken in all the same: look at the wall before trying again."_s);
        QVERIFY(!desk.view.isTaking());
        const qsizetype doneBefore = desk.done.count();
        desk.view.fileWasTaken(desk.takeTurn(), core::Taken{});
        QCOMPARE(desk.done.count(), doneBefore);
        desk.view.reset();
        QCOMPARE(cutOff.count(), 1);
    }

    void aFileWithLittleInItSaysThatToo()
    {
        Desk desk;
        core::FileContents file;
        file.hasSecrets = true;
        core::FileDevice here = device(u"NAS"_s, u"192.168.1.22"_s);
        here.isNew = false;
        file.devices = QList<core::FileDevice>{here};
        core::FileAction lock;
        lock.name = u"Lock"_s;
        lock.keys = u"MetaLeft,KeyL"_s;
        file.actions = QList<core::FileAction>{lock, typing(u"Old"_s, u"never shown"_s)};
        (*file.actions)[1].isNew = false;
        for (int number = 1; number <= 7; ++number) {
            file.leftOut.append(u"entry %1"_s.arg(number));
        }
        desk.hasRead(u"plain.json"_s, plainFile, file);
        QCOMPARE(desk.view.about(), u"A plain file."_s);
        // A plain file with passwords in it was written by hand: said.
        QVERIFY(desk.view.hasPlainSecrets());
        // Nothing of it is new: the parts are there, cannot be taken, and are not ticked.
        for (const QString& key : {u"devices"_s, u"actions"_s}) {
            const QVariantMap part = partOf(desk.view, key);
            QVERIFY2(!part.value(u"can"_s).toBool(), qPrintable(key));
            QVERIFY2(!part.value(u"ticked"_s).toBool(), qPrintable(key));
            QVERIFY2(part.value(u"lines"_s).toList().isEmpty(), qPrintable(key));
        }
        QCOMPARE(partOf(desk.view, u"devices"_s).value(u"label"_s).toString(), u"KVMs and servers: 0 to add"_s);
        QCOMPARE(partOf(desk.view, u"devices"_s).value(u"hint"_s).toString(),
                 u"1 are on this wall already and stay as they are."_s);
        QCOMPARE(partOf(desk.view, u"actions"_s).value(u"hint"_s).toString(),
                 u"2 are left out: an action of that name is here already."_s);
        QVERIFY(!desk.view.canTake());
        QCOMPARE(desk.view.leftOut(), u"7 entries were left out: entry 1; entry 2; entry 3; entry 4; entry 5…"_s);

        // The settings alone can be taken, and say what they would set.
        core::FileContents settings;
        settings.alerts = core::FileAlerts{};
        core::FileWallSettings wall;
        wall.asksForNewerVersions = true;
        settings.wall = wall;
        desk.view.readFile({});
        desk.view.fileWasRead(desk.readTurn(), settings);
        QVERIFY(desk.view.canTake());
        QCOMPARE(desk.view.parts().size(), 2);
        QVERIFY(!desk.view.hasPlainSecrets());
        QCOMPARE(
            partOf(desk.view, u"alerts"_s).value(u"hint"_s).toString(),
            u"Replaces what is set here: alerts off, told about: nothing. The address this wall sends them to is kept."_s);
        QCOMPARE(partOf(desk.view, u"wall"_s).value(u"hint"_s).toString(),
                 u"Replaces what is set here: asks once a day whether there is a newer version."_s);
        QCOMPARE(desk.view.leftOut(), QString());

        // A file with no part at all: there is nothing to show but what it is.
        desk.view.readFile({});
        desk.view.fileWasRead(desk.readTurn(), core::FileContents{});
        QVERIFY(desk.view.wasRead());
        QVERIFY(desk.view.parts().isEmpty());
        QVERIFY(!desk.view.canTake());
    }

    void passwordsInTheOpenAreSaidOnlyWhereTheyAre_data()
    {
        QTest::addColumn<bool>("encrypted");
        QTest::addColumn<bool>("hasSecrets");
        QTest::addColumn<bool>("said");
        QTest::newRow("a plain file with passwords") << false << true << true;
        QTest::newRow("a plain file without") << false << false << false;
        QTest::newRow("an encrypted file with passwords") << true << true << false;
    }

    void passwordsInTheOpenAreSaidOnlyWhereTheyAre()
    {
        QFETCH(bool, encrypted);
        QFETCH(bool, hasSecrets);
        QFETCH(bool, said);
        Desk desk;
        core::FileContents file;
        file.encrypted = encrypted;
        file.hasSecrets = hasSecrets;
        desk.hasRead(u"a.json"_s, plainFile, file);
        QCOMPARE(desk.view.hasPlainSecrets(), said);
    }

    void aPassphraseThatIsChangedAfterTheReadingHasTheFileReadAgain()
    {
        Desk desk;
        // (Nothing was read: there is nothing to forget.)
        const QSignalSpy changed(&desk.view, &FilesView::changed);
        desk.view.readAnew();
        QCOMPARE(changed.count(), 0);
        desk.hasRead(u"kept.qkvm"_s, sealedFile, aFile(), u"of the file"_s);
        desk.view.takeIn({}, u"of the file"_s);
        QVERIFY(!desk.view.problem().isEmpty());
        desk.view.readAnew();
        QVERIFY(!desk.view.wasRead());
        QVERIFY(desk.view.parts().isEmpty());
        QVERIFY(!desk.view.canTake());
        QCOMPARE(desk.view.problem(), QString());
        // The file is still the file, and still asks for its passphrase.
        QCOMPARE(desk.view.fileName(), u"kept.qkvm"_s);
        QVERIFY(desk.view.isSealed());
        desk.view.takeIn({u"devices"_s}, u"another"_s);
        QCOMPARE(desk.takeAsked.count(), 0);
        desk.view.readFile(u"another"_s);
        QCOMPARE(desk.readAsked.last().at(2).toString(), u"another"_s);
        // While the server reads or takes, what is typed changes nothing.
        desk.view.readAnew();
        QVERIFY(desk.view.isBusy());
    }

    void whatIsTickedIsTakenInAndSaid()
    {
        Desk desk;
        desk.opens = desk.write(u"kept.qkvm"_s, sealedFile);
        desk.view.chooseFile();
        // Nothing is taken of a file that was not read.
        desk.view.takeIn({u"devices"_s}, u"of the file"_s);
        QCOMPARE(desk.takeAsked.count(), 0);
        desk.view.readFile(u"of the file"_s);
        desk.view.fileWasRead(desk.readTurn(), aFile());
        desk.view.takeIn({}, u"of the file"_s);
        QCOMPARE(desk.view.problem(), u"Tick what is to be taken in."_s);
        QCOMPARE(desk.takeAsked.count(), 0);

        desk.view.takeIn({u"devices"_s, u"alerts"_s}, u"of the file"_s);
        QCOMPARE(desk.takeAsked.count(), 1);
        QCOMPARE(desk.takeAsked.at(0).at(1).toByteArray(), sealedFile);
        QCOMPARE(desk.takeAsked.at(0).at(2).toString(), u"of the file"_s);
        QCOMPARE(desk.takeAsked.at(0).at(3).toStringList(), (QStringList{u"devices"_s, u"alerts"_s}));
        QVERIFY(desk.view.isBusy());
        // (This is what cannot be given up: the box stays while it lasts.)
        QVERIFY(desk.view.isTaking());
        QCOMPARE(desk.view.problem(), QString());
        desk.view.takeIn({u"devices"_s}, u"of the file"_s);
        QCOMPARE(desk.takeAsked.count(), 1);
        // What is typed into the passphrase's line meanwhile changes nothing of what is shown.
        desk.view.readAnew();
        QVERIFY(desk.view.wasRead());
        QCOMPARE(desk.view.parts().size(), 4);

        // Refused: said in the box, and what was read is still there to try again.
        desk.view.fileWasTaken(
            desk.takeTurn(),
            base::Error{"locked", "The saved passwords are locked just now. Sign in again and try once more."});
        QCOMPARE(desk.view.problem(), u"The saved passwords are locked just now. Sign in again and try once more."_s);
        QVERIFY(desk.view.wasRead());
        QVERIFY(!desk.view.isTaking());
        QCOMPARE(desk.done.count(), 0);
        // Where the server's answer did not come, nobody knows whether it took the file in: said.
        desk.view.takeIn({u"devices"_s}, u"of the file"_s);
        desk.view.fileWasTaken(desk.takeTurn(), base::Error{core::ServerClient::unreachable,
                                                            "qKVM could not be reached: it took too long"});
        QCOMPARE(
            desk.view.problem(),
            u"qKVM could not be reached: it took too long. The file may have been taken in all the same: look at the wall before trying again."_s);
        // (A sentence that ends as one is not given a second full stop.)
        desk.view.takeIn({u"devices"_s}, u"of the file"_s);
        desk.view.fileWasTaken(desk.takeTurn(), base::Error{core::ServerClient::unreachable, "No answer."});
        QCOMPARE(desk.view.problem(),
                 u"No answer. The file may have been taken in all the same: look at the wall before trying again."_s);
        QVERIFY(desk.view.wasRead());

        desk.view.takeIn({u"devices"_s, u"alerts"_s}, u"of the file"_s);
        core::Taken taken;
        taken.devices = 2;
        taken.actions = 1;
        taken.alerts = true;
        taken.wall = true;
        desk.view.fileWasTaken(desk.takeTurn(), taken);
        QCOMPARE(desk.done.count(), 1);
        QCOMPARE(desk.done.at(0).at(0).toString(),
                 u"2 devices added, 1 saved actions, alert settings replaced, wall settings replaced"_s);
        QCOMPARE(desk.done.at(0).at(1).toBool(), true);
        // The file is nobody's any more: neither its text nor what it held.
        QVERIFY(!desk.view.isBusy());
        QVERIFY(!desk.view.wasRead());
        QCOMPARE(desk.view.fileName(), QString());
        QVERIFY(desk.view.parts().isEmpty());
        desk.view.readFile({});
        QCOMPARE(desk.view.problem(), u"Choose a file first."_s);
    }

    void whetherAFileWasTakenInIsLeftOpenWhereItIsNotTheServerThatSaysNo_data()
    {
        QTest::addColumn<QString>("code");
        QTest::addColumn<bool>("isLeftOpen");
        // The server's own refusals: it gives them before it changes anything.
        for (const char* code : {"passphrase_needed", "wrong_passphrase", "not_a_file", "newer", "damaged", "busy",
                                 "locked", "not_allowed", "forbidden", "not_saved", "invalid"}) {
            QTest::newRow(code) << QString::fromLatin1(code) << false;
        }
        // The program's own word for a file that was never sent to the server at all.
        QTest::newRow("never sent") << QString::fromLatin1(ui::FilesView::nothingWasSent) << false;
        // No answer; an answer that was cut short or was too long; one from something that stands
        // before the server; a word this program does not know.
        QTest::newRow("no answer") << QString::fromLatin1(core::ServerClient::unreachable) << true;
        for (const char* code : {"bad_answer", "too_large", "refused", "error", ""}) {
            QTest::newRow(qPrintable(u"said: "_s + QString::fromLatin1(code))) << QString::fromLatin1(code) << true;
        }
    }

    void whetherAFileWasTakenInIsLeftOpenWhereItIsNotTheServerThatSaysNo()
    {
        QFETCH(QString, code);
        QFETCH(bool, isLeftOpen);
        Desk desk;
        desk.hasRead(u"plain.json"_s, plainFile, aFile());
        desk.view.takeIn({u"devices"_s}, {});
        desk.view.fileWasTaken(desk.takeTurn(), base::Error{code.toStdString(), "It did not work"});
        QCOMPARE(
            desk.view.problem(),
            isLeftOpen
                ? u"It did not work. The file may have been taken in all the same: look at the wall before trying again."_s
                : u"It did not work"_s);
        // Either way what was read is still there.
        QVERIFY(desk.view.wasRead());
        QCOMPARE(desk.done.count(), 0);
    }

    void whatWasTakenIsSaidAsItWas_data()
    {
        QTest::addColumn<int>("devices");
        QTest::addColumn<int>("actions");
        QTest::addColumn<bool>("alerts");
        QTest::addColumn<bool>("wall");
        QTest::addColumn<QString>("said");
        QTest::newRow("one device") << 1 << 0 << false << false << u"1 device added"_s;
        QTest::newRow("the alert settings only")
            << 0 << 0 << true << false << u"0 devices added, alert settings replaced"_s;
        QTest::newRow("the wall settings only")
            << 0 << 0 << false << true << u"0 devices added, wall settings replaced"_s;
        QTest::newRow("actions only") << 0 << 3 << false << false << u"0 devices added, 3 saved actions"_s;
    }

    void whatWasTakenIsSaidAsItWas()
    {
        QFETCH(int, devices);
        QFETCH(int, actions);
        QFETCH(bool, alerts);
        QFETCH(bool, wall);
        QFETCH(QString, said);
        Desk desk;
        desk.hasRead(u"plain.json"_s, plainFile, aFile());
        // A plain file is taken without a passphrase, whatever stands in the box.
        desk.view.takeIn({u"devices"_s}, u"left over"_s);
        QCOMPARE(desk.takeAsked.at(0).at(2).toString(), QString());
        core::Taken taken;
        taken.devices = devices;
        taken.actions = actions;
        taken.alerts = alerts;
        taken.wall = wall;
        desk.view.fileWasTaken(desk.takeTurn(), taken);
        QCOMPARE(desk.done.at(0).at(0).toString(), said);
    }

    void aBoxThatIsOpenedAnewOrClosedKeepsNothing()
    {
        Desk desk;
        core::FileContents file = aFile();
        file.encrypted = false;
        file.hasSecrets = true;
        desk.hasRead(u"kept.qkvm"_s, sealedFile, file, u"of the file"_s);
        QVERIFY(desk.view.hasPlainSecrets());
        desk.view.takeIn({}, {});
        QVERIFY(!desk.view.problem().isEmpty());
        const QSignalSpy changed(&desk.view, &FilesView::changed);
        desk.view.reset();
        QCOMPARE(changed.count(), 1);
        QCOMPARE(desk.view.problem(), QString());
        QCOMPARE(desk.view.fileName(), QString());
        QVERIFY(!desk.view.isSealed());
        QVERIFY(!desk.view.wasRead());
        QCOMPARE(desk.view.about(), QString());
        QVERIFY(!desk.view.hasPlainSecrets());
        QVERIFY(desk.view.parts().isEmpty());
        QVERIFY(!desk.view.canTake());
        QCOMPARE(desk.view.leftOut(), QString());
        // Not the file's text either: there is none to read.
        desk.view.readFile({});
        QCOMPARE(desk.view.problem(), u"Choose a file first."_s);
        QCOMPARE(desk.readAsked.count(), 1);
    }

    void whoeverLeavesTheWallLeavesNothingUnderWay()
    {
        Desk desk;
        desk.savesTo = desk.folder.filePath(u"wall.qkvm"_s);
        desk.opens = desk.write(u"plain.json"_s, plainFile);
        desk.view.chooseFile();
        desk.view.readFile({});
        QVERIFY(desk.view.isBusy());
        const quint64 reading = desk.readTurn();
        desk.view.reset();
        QVERIFY(!desk.view.isBusy());
        QCOMPARE(desk.view.fileName(), QString());
        // What the server answers after that is nobody's.
        desk.view.fileWasRead(reading, aFile());
        QVERIFY(!desk.view.wasRead());
        // The same for a file that was being made: it is written nowhere.
        desk.view.exportWall({u"devices"_s}, u"a passphrase of mine"_s, u"my own"_s);
        const quint64 making = desk.exportTurn();
        desk.view.reset();
        desk.view.exported(making, made());
        QVERIFY(!QFile::exists(desk.savesTo));
        // And for one that was being taken in: nothing is told of it here.
        desk.hasRead(u"plain.json"_s, plainFile, aFile());
        desk.view.takeIn({u"devices"_s}, {});
        const quint64 taking = desk.takeTurn();
        desk.view.reset();
        QVERIFY(!desk.view.isTaking());
        desk.view.fileWasTaken(taking, core::Taken{});
        QCOMPARE(desk.done.count(), 0);
        QCOMPARE(desk.view.problem(), QString());
    }
};

QTEST_GUILESS_MAIN(TestFilesView)

#include "test_files_view.moc"
