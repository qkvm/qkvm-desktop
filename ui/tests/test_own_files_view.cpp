#include <array>
#include <functional>
#include <memory>
#include <string>

#include <QAbstractItemModelTester>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <qkvm/core/exchange.h>
#include <qkvm/core/kvm_editing.h>
#include <qkvm/core/local_wall.h>
#include <qkvm/core/wall_exchange.h>

#include "own_files_view.h"
#include "wall_files_by_hand.h"

// Windows' own header, for how Windows takes the name of a file.
#include <windows.h>

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::WallExchange;
using core::exchange::Device;
using core::testing::thePassphrase;
using ui::LaidOutList;
using ui::OwnFilesView;
using ui::testing::fileOf;
using ui::testing::HeldWork;
using ui::testing::knownBy;
using ui::testing::kvmAt;
using ui::testing::notKeptHere;
using ui::testing::quickly;
using ui::testing::saying;
using ui::testing::WallOfOneTest;

// (A device of a file is a row's value in the tests that have a row for each kind of device.)
Q_DECLARE_METATYPE(qkvm::core::exchange::Device)

namespace {

// The boxes of one window for this PC's wall as a file, as a test has them: the view, over a
// WallExchange of its own and a wall that is held to this PC's own address; and doors of the
// test's own: where a file is saved and which is opened is said here, how often either was asked
// is counted, and something can be made to happen while one of them is open, as it can behind a
// box of Windows'.
//
// The two moments in which nothing is taken and nothing is saved are short here (`quickRest`),
// so that what is waited for comes soon. A test that looks at what is NOT to be had yet makes
// them long first (`longRest`): it then has seconds to look in, on a PC that is busy too.
struct Desk {
    static constexpr int quickRest = 100;
    static constexpr int longRest = 3000;
    // How long a wait is that is surely longer than a quick moment.
    static constexpr int pastAQuickRest = 5 * quickRest;

    explicit Desk(WallOfOneTest& own, int keptForMs = WallExchange::keptForMsAsItIs,
                  core::exchange::Sealing sealing = quickly())
        : exchange(own.wall, std::move(sealing), keptForMs)
    {
        view.restFor(quickRest, quickRest);
        view.use(&exchange);
        view.setDoor({[this](const QString& suggested) -> base::Result<QString> {
                          suggestions.append(suggested);
                          heldItsWindowWhileAsked = view.holdsItsWindow();
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
                          heldItsWindowWhileAsked = view.holdsItsWindow();
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

    // A file is chosen, as somebody chooses it in Windows' box.
    void choose(const QString& name, const QByteArray& text)
    {
        opens = write(name, text);
        view.chooseFile();
    }
    // And read: true once what it holds is laid out; false where it was refused.
    bool layOut(const QByteArray& text, const QString& passphrase = thePassphrase())
    {
        choose(u"wall-%1.qkvm"_s.arg(++files), text);
        view.readFile(passphrase);
        return QTest::qWaitFor([this] { return !view.isBusy(); }, 30000) && view.isLaidOut();
    }
    // The list is looked at for its moment: true once something can be taken.
    bool lookedAtForItsMoment()
    {
        view.setLookedAt(true);
        return QTest::qWaitFor([this] { return view.mayTake(); }, 20000);
    }
    // The list is looked at, and has stood as it is for its moment: true once what is in it (a
    // tick, a row's "Details", the row that opens the notes) takes a press.
    bool settledForItsMoment()
    {
        view.setLookedAt(true);
        return QTest::qWaitFor([this] { return view.isSettled(); }, 20000);
    }
    // The file is made: true once it is, false where it was refused.
    bool make(const QString& passphrase)
    {
        view.makeFile(passphrase);
        return QTest::qWaitFor([this] { return !view.isBusy(); }, 30000) && view.isMade();
    }
    // What is not in the file is on show for its moment: true once it can be saved.
    bool onShowForItsMoment()
    {
        view.setBesideOnShow(true);
        return QTest::qWaitFor([this] { return view.maySave(); }, 20000);
    }

    QTemporaryDir folder;
    WallExchange exchange;
    OwnFilesView view;
    QString savesTo;
    QString opens;
    QStringList suggestions;
    int opened = 0;
    int files = 0;
    bool heldItsWindowWhileAsked = false;
    bool doorIsStuck = false;
    std::function<void()> meanwhile;
    QSignalSpy exported{&view, &OwnFilesView::exported};
    QSignalSpy taken{&view, &OwnFilesView::taken};
    QSignalSpy fileWasMade{&view, &OwnFilesView::fileWasMade};
    QSignalSpy fileWasRead{&view, &OwnFilesView::fileWasRead};
    QSignalSpy fileIsAnother{&view, &OwnFilesView::fileIsAnother};
    QSignalSpy closeAsked{&view, &OwnFilesView::closeAsked};
};

// One row of what a box lists, as the screens are handed it.
struct Row {
    QString kind;
    QString text;
    int place = 0;
    QString nameAndAddress;
    QString inShort;
    bool wantsALook = false;
    QString verdict;
    QStringList says;
    QString fingerprint;
    bool canBeTaken = false;
    bool ticked = false;
    bool open = false;
};

QList<Row> rowsOf(OwnFilesView& view)
{
    QList<Row> rows;
    const LaidOutList* list = view.offered();
    for (int at = 0; at < list->rowCount(); ++at) {
        const QModelIndex index = list->index(at, 0);
        Row row;
        row.kind = index.data(LaidOutList::KindRole).toString();
        row.text = index.data(LaidOutList::TextRole).toString();
        row.place = index.data(LaidOutList::PlaceRole).toInt();
        row.nameAndAddress = index.data(LaidOutList::NameAndAddressRole).toString();
        row.inShort = index.data(LaidOutList::InShortRole).toString();
        row.wantsALook = index.data(LaidOutList::WantsALookRole).toBool();
        row.verdict = index.data(LaidOutList::VerdictRole).toString();
        row.says = index.data(LaidOutList::SaysRole).toStringList();
        row.fingerprint = index.data(LaidOutList::FingerprintRole).toString();
        row.canBeTaken = index.data(LaidOutList::CanBeTakenRole).toBool();
        row.ticked = index.data(LaidOutList::TickedRole).toBool();
        row.open = index.data(LaidOutList::OpenRole).toBool();
        rows.append(row);
    }
    return rows;
}

// The kinds of the rows, in their order: what a list is made of, at a glance.
QStringList kindsOf(OwnFilesView& view)
{
    QStringList kinds;
    for (const Row& row : rowsOf(view)) {
        kinds.append(row.kind);
    }
    return kinds;
}

// The dot in the middle of the line that stands between two things a row says in short, with a
// blank on either side: made here from its number.
QString dot()
{
    return u' ' + QString(QChar(0x00B7)) + u' ';
}

// A certificate's SHA-256 as a row says it in short: its first two pairs and its last two, with
// the sign for what is left out between them (three dots in one, made here from its number).
QString inShortOf(const QByteArray& sha256)
{
    const QStringList pairs = core::kvm::fingerprint(sha256).split(u':');
    return pairs.first(2).join(u':') + u' ' + QChar(0x2026) + u' ' + pairs.last(2).join(u':');
}

// (A file with as many lines beside its devices as a test wants: fileWithSoManyLeftOut, which the
// tests of the screens have too.)
QByteArray fileWithSoManyNotes(int notes, const QList<Device>& devices)
{
    return ui::testing::fileWithSoManyLeftOut(notes, devices);
}

// The devices among them, by their places: which are ticked.
QList<int> tickedIn(OwnFilesView& view)
{
    QList<int> ticked;
    for (const Row& row : rowsOf(view)) {
        if (row.kind == u"device"_s && row.ticked) {
            ticked.append(row.place);
        }
    }
    return ticked;
}

// Addresses that reach nothing by the books, each of its own: for devices that are taken onto a
// wall of a test and are never asked anything.
QString elsewhere(int number)
{
    return u"https://192.0.2.%1"_s.arg(number);
}

// A device that is ticked to begin with: over HTTPS, known by Windows' word, at an address
// somewhere else.
Device ticked(int number, const QString& name)
{
    return saying(kvmAt(elsewhere(number), name), u"system"_s);
}

const QString busyElsewhere = u"Another file is being worked on just now, and one is worked on at a time. That passes: "
                              "try again in a moment."_s;

// The name of a file as Windows takes it, by its own reckoning (GetFullPathNameW): the file it
// looks for, makes, or asks about under that name, whole. Empty: Windows would not say.
QString asWindowsNamesIt(const QString& path)
{
    std::array<wchar_t, 2048> full{};
    const std::wstring asked = QDir::toNativeSeparators(path).toStdWString();
    const DWORD length = GetFullPathNameW(asked.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
    if (length == 0 || length >= full.size()) {
        return {};
    }
    return QDir::fromNativeSeparators(QString::fromWCharArray(full.data(), static_cast<qsizetype>(length)));
}

} // namespace

// This PC's own wall as a file: what the two boxes of a window ask, and what they are shown, over
// the program's own reader and writer and a wall that is held to this PC's own address.
class TestOwnFilesView : public QObject {
    Q_OBJECT

private slots:
    void actionsCanBeChosenWithoutDevicesOrReplacingSettings()
    {
        WallOfOneTest own;
        Desk desk(own);
        const QByteArray file = core::testing::plainFile(
            {{u"actions"_s,
              QJsonArray{QJsonObject{{u"name"_s, u"Shortcut"_s}, {u"kind"_s, u"keys"_s}, {u"keys"_s, u"F5"_s}}}},
             {u"wall"_s, QJsonObject{{u"interval"_s, 2.75}, {u"update_check"_s, false}}}});
        QVERIFY(desk.layOut(file, {}));
        QCOMPARE(desk.view.tickedCount(), 1);
        QVERIFY(desk.view.hasOtherParts());
        desk.view.setLookedAt(true);
        QTRY_VERIFY(desk.view.isSettled());
        desk.view.openNotes();
        QTRY_VERIFY(desk.view.mayTake());
        desk.view.take();
        QCOMPARE(own.wall.actions().all().size(), 1);
        QCOMPARE(own.settings.remembered().wallPictureEveryMs, 0);
        QVERIFY(own.settings.remembered().checkUpdates);
        QVERIFY(own.wall.kvms().isEmpty());
    }

    void exportWritesOnlySelectedParts()
    {
        WallOfOneTest own;
        QVERIFY(own.wall.actions().add({u"Shortcut"_s, false, u"F5"_s, {}, false, true}));
        Desk desk(own);
        desk.savesTo = desk.folder.filePath(u"actions.json"_s);
        desk.view.makeSelectedFile({}, false, true, false);
        QTRY_VERIFY(desk.view.isMade());
        QVERIFY(desk.onShowForItsMoment());
        desk.view.saveFile();
        const auto read = core::exchange::read(Desk::read(desk.savesTo), {});
        QVERIFY(read);
        QVERIFY(!read.value().devices);
        QVERIFY(!read.value().wall);
        QVERIFY(!read.value().alerts);
        QVERIFY(read.value().actions);
        QCOMPARE(read.value().actions.value_or(QList<core::exchange::Action>()).size(), 1);
        desk.view.reset();
        desk.view.makeSelectedFile({}, false, false, false);
        QVERIFY(!desk.view.isMade());
        QVERIFY(!desk.view.problem().isEmpty());
    }

    // --- export -------------------------------------------------------------------------------

    void theLinesBesideTheFileAreOnShowBeforeAnythingIsSaved()
    {
        WallOfOneTest own;
        QVERIFY(!own.put(u"http://127.0.0.1:20001"_s, u"Build server"_s).isEmpty());
        const QString rack = own.put(u"http://127.0.0.1:20002"_s, u"Rack"_s);
        // (Windows does not hand out the password of the second: it is written without.)
        own.secrets.all.remove(core::LocalWall::secretOf(rack));
        Desk desk(own);
        desk.savesTo = desk.folder.filePath(u"wall.qkvm"_s);
        QVERIFY(!desk.view.isMade());
        QVERIFY(desk.view.notInIt().isEmpty());

        // "Make the file": the box says that it works, and then that the file is made. Nothing
        // is asked of anybody meanwhile, and nothing is kept anywhere.
        desk.view.makeFile(thePassphrase());
        QVERIFY(desk.view.isBusy());
        QVERIFY(desk.view.isMaking());
        QVERIFY(desk.view.holdsItsWindow());
        // (Asked once: a second press while it is made asks nothing.)
        desk.view.makeFile(thePassphrase());
        QTRY_VERIFY(desk.view.isMade());
        QVERIFY(!desk.view.isBusy());
        QVERIFY(desk.view.isMadeEncrypted());
        QCOMPARE(desk.fileWasMade.count(), 1);
        // Every line of what is not in it, as the core hands them over: what of a wall is not
        // this PC's yet, and the KVM that is written without its password.
        QCOMPARE(
            desk.view.notInIt(),
            (QStringList{
                u"Server alert settings and this PC's accounts, app password, window layout and preferences are not in this file."_s,
                core::exchange::deviceOfTheWall(1, u"Rack"_s) +
                    u": its password is not in this file. Windows did not hand it out, or none is kept for this "
                    "device as it is."_s}));

        // Nothing is saved before those lines are on show: no place is asked for, and nothing
        // is written. Not when the screens have not said that they are; and not in the first
        // moment that they are. (A long moment here, to look in; as long as a second in the
        // program, which is as long as the box that changes a KVM holds back a yes.)
        QCOMPARE(OwnFilesView::saveRestsMs, 1000);
        desk.view.restFor(Desk::longRest, Desk::longRest);
        desk.view.saveFile();
        QVERIFY(!desk.view.maySave());
        QVERIFY(desk.suggestions.isEmpty());
        QElapsedTimer since;
        since.start();
        desk.view.setBesideOnShow(true);
        desk.view.saveFile();
        QVERIFY(desk.suggestions.isEmpty());
        QVERIFY(!QFile::exists(desk.savesTo));
        QTest::qWait(Desk::longRest / 6);
        QVERIFY2(!desk.view.maySave(), qPrintable(QString::number(since.elapsed())));
        desk.view.saveFile();
        QVERIFY(desk.suggestions.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(desk.view.maySave(), 20000);
        // (All of the moment, but for what a timer of Windows' may be early by: a twentieth,
        // and here a tenth is let pass.)
        QVERIFY2(since.elapsed() >= Desk::longRest * 9 / 10, qPrintable(QString::number(since.elapsed())));
        QCOMPARE(desk.exported.count(), 0);
        // And taken off show again, the moment begins anew.
        desk.view.setBesideOnShow(false);
        QVERIFY(!desk.view.maySave());
        desk.view.saveFile();
        QVERIFY(desk.suggestions.isEmpty());
        desk.view.restFor(Desk::quickRest, Desk::quickRest);
        QVERIFY(desk.onShowForItsMoment());

        // "Save": where it is kept is asked in Windows' own box, under the name the page's
        // files have, and the file is written there whole.
        desk.view.saveFile();
        QCOMPARE(desk.suggestions.size(), 1);
        QVERIFY2(QRegularExpression(u"^qkvm-\\d{4}-\\d{2}-\\d{2}\\.qkvm$"_s).match(desk.suggestions[0]).hasMatch(),
                 qPrintable(desk.suggestions[0]));
        QVERIFY(desk.heldItsWindowWhileAsked);
        QCOMPARE(desk.exported.count(), 1);
        QCOMPARE(desk.exported.at(0).at(0).toString(),
                 u"Exported to wall.qkvm. The file opens with its passphrase only: keep the two apart."_s);
        // What was written is the wall, as the reader reads it back with that passphrase: both
        // KVMs, the first with its password and the second without.
        const base::Result<core::exchange::Content> kept =
            core::exchange::read(Desk::read(desk.savesTo), thePassphrase());
        QVERIFY2(kept.ok(), kept.ok() ? "" : kept.error().message.c_str());
        const QList<Device> devices = kept.value().devices.value_or(QList<Device>());
        QCOMPARE(devices.size(), 2);
        QCOMPARE(devices.at(0).name, u"Build server"_s);
        QCOMPARE(devices.at(0).password, u"secret"_s);
        QCOMPARE(devices.at(1).name, u"Rack"_s);
        QCOMPARE(devices.at(1).password, QString());
        // The box keeps nothing of it: there is no file to save a second time.
        QVERIFY(!desk.view.isMade());
        QVERIFY(desk.view.notInIt().isEmpty());
        desk.view.saveFile();
        QCOMPARE(desk.suggestions.size(), 1);
        QCOMPARE(desk.exported.count(), 1);
    }

    void aFileWithoutAPassphraseIsAPlainOneWithNoPasswordInIt()
    {
        WallOfOneTest own;
        QVERIFY(!own.put(u"http://127.0.0.1:20001"_s, u"Build server"_s).isEmpty());
        Desk desk(own);
        desk.savesTo = desk.folder.filePath(u"wall.json"_s);
        QVERIFY(desk.make(QString()));
        QVERIFY(!desk.view.isMadeEncrypted());
        QVERIFY(desk.onShowForItsMoment());
        desk.view.saveFile();
        QCOMPARE(desk.suggestions.size(), 1);
        QVERIFY2(desk.suggestions[0].endsWith(u".json"_s), qPrintable(desk.suggestions[0]));
        QCOMPARE(desk.exported.count(), 1);
        QCOMPARE(desk.exported.at(0).at(0).toString(), u"Exported to wall.json, without passwords"_s);
        const QByteArray text = Desk::read(desk.savesTo);
        QVERIFY(text.contains("Build server"));
        QVERIFY(!text.contains("secret"));
        const base::Result<core::exchange::Content> kept = core::exchange::read(text, QString());
        QVERIFY(kept.ok());
        QVERIFY(!kept.value().sealed);
        // No password was read from the store for it.
        QCOMPARE(kept.value().devices.value_or(QList<Device>()).constFirst().password, QString());
    }

    void whatIsNoPassphraseMakesNoFile()
    {
        WallOfOneTest own;
        Desk desk(own);
        // Ten characters are a passphrase; nine are not. Said at once, and nothing is worked on.
        desk.view.makeFile(u"123456789"_s);
        QCOMPARE(desk.view.problem(), u"A passphrase has at least 10 characters."_s);
        QVERIFY(!desk.view.isBusy());
        QVERIFY(!desk.view.isMade());
        QVERIFY(!desk.exchange.isBusy());
        // The next try says nothing of the one before.
        QVERIFY(desk.make(u"1234567890"_s));
        QCOMPARE(desk.view.problem(), QString());
    }

    void windowsBoxGivenUpLeavesTheBoxAsItIsAndAFileThatCannotBeWrittenIsSaid()
    {
        WallOfOneTest own;
        QVERIFY(!own.put(u"http://127.0.0.1:20001"_s, u"Build server"_s).isEmpty());
        Desk desk(own);
        QVERIFY(desk.make(thePassphrase()));
        QVERIFY(desk.onShowForItsMoment());
        const QStringList lines = desk.view.notInIt();
        QVERIFY(!lines.isEmpty());

        // Nobody says where: nothing is written, nothing is said to be wrong, and the box is as
        // it was, with its file and its lines.
        desk.savesTo.clear();
        desk.view.saveFile();
        QCOMPARE(desk.suggestions.size(), 1);
        QVERIFY(desk.view.isMade());
        QCOMPARE(desk.view.notInIt(), lines);
        QVERIFY(!desk.view.isBusy());
        QVERIFY(!desk.view.holdsItsWindow());
        QCOMPARE(desk.view.problem(), QString());
        QCOMPARE(desk.exported.count(), 0);
        // Windows' own box that does not come up is said, and is not taken for a no.
        desk.doorIsStuck = true;
        desk.view.saveFile();
        QCOMPARE(desk.view.problem(), u"Windows did not open its box for choosing a file."_s);
        QVERIFY(desk.view.isMade());
        desk.doorIsStuck = false;
        // A place that cannot be written to: said, and the file is still there to be saved.
        desk.savesTo = desk.folder.filePath(u"no such folder/wall.qkvm"_s);
        desk.view.saveFile();
        QVERIFY2(desk.view.problem().startsWith(u"The file could not be written: "_s), qPrintable(desk.view.problem()));
        QVERIFY(desk.view.isMade());
        QCOMPARE(desk.exported.count(), 0);
        // And somewhere it can be: written, and what was wrong is not said any more.
        desk.savesTo = desk.folder.filePath(u"wall.qkvm"_s);
        desk.view.saveFile();
        QCOMPARE(desk.exported.count(), 1);
        QCOMPARE(desk.view.problem(), QString());
        QVERIFY(core::exchange::read(Desk::read(desk.savesTo), thePassphrase()).ok());
    }

    void aBoxThatIsClosedWhileItsFileIsMadeMakesNothingAndTheWindowNeverStandsStill()
    {
        WallOfOneTest own;
        QVERIFY(!own.put(u"http://127.0.0.1:20001"_s, u"Build server"_s).isEmpty());
        HeldWork work;
        Desk desk(own, WallExchange::keptForMsAsItIs, work.sealing());
        desk.savesTo = desk.folder.filePath(u"wall.qkvm"_s);
        work.hold();
        desk.view.makeFile(thePassphrase());
        QTRY_COMPARE(work.begun(), 1);
        QVERIFY(desk.view.isMaking());
        // "Cancel": the box is closed over it. That comes back while the work is still going on
        // (which is held here for as long as this test wants: whoever waited for it would wait
        // for ever), and nothing is waited for from then on.
        QElapsedTimer closing;
        closing.start();
        desk.view.reset();
        QVERIFY2(closing.elapsed() < 3000, qPrintable(QString::number(closing.elapsed())));
        QVERIFY(!desk.view.isBusy());
        QVERIFY(!desk.view.holdsItsWindow());
        // What is so of the work itself: it is NOT ended. It runs on, with nobody waiting for
        // it, and the program's one place for a file is taken until it has run out: whoever
        // asks for a file meanwhile, in this box or in another, is told that one is being worked
        // on, which passes.
        QVERIFY(desk.exchange.isBusy());
        QCOMPARE(work.begun(), 1);
        desk.view.makeFile(thePassphrase());
        QTRY_VERIFY(!desk.view.isBusy());
        QCOMPARE(desk.view.note(), busyElsewhere);
        QCOMPARE(desk.view.problem(), QString());
        QVERIFY(!desk.view.isMade());
        QVERIFY(desk.exchange.isBusy());
        desk.view.reset();
        // What the work comes to afterwards is nobody's: no file is made behind anybody's back,
        // and none can be saved.
        work.letGo();
        QTRY_VERIFY_WITH_TIMEOUT(!desk.exchange.isBusy(), 20000);
        QTest::qWait(100);
        QVERIFY(!desk.view.isMade());
        QVERIFY(desk.view.notInIt().isEmpty());
        QCOMPARE(desk.fileWasMade.count(), 0);
        desk.view.setBesideOnShow(true);
        QTest::qWait(Desk::pastAQuickRest);
        desk.view.saveFile();
        QVERIFY(desk.suggestions.isEmpty());
        QVERIFY(!QFile::exists(desk.savesTo));
        // And it has passed: the next file is made.
        QVERIFY(desk.make(thePassphrase()));
    }

    void whoeverComesBackFromWindowsBoxToSomethingElseHasSavedNothing()
    {
        WallOfOneTest own;
        QVERIFY(!own.put(u"http://127.0.0.1:20001"_s, u"Build server"_s).isEmpty());
        Desk desk(own);
        desk.savesTo = desk.folder.filePath(u"wall.qkvm"_s);
        QVERIFY(desk.make(thePassphrase()));
        QVERIFY(desk.onShowForItsMoment());
        // While Windows' box is open the wall is left, and the box of the file closed with it:
        // the place that is named afterwards is nobody's, and nothing is written there.
        bool heldTheWindow = false;
        desk.meanwhile = [&] {
            desk.view.reset();
            // (The window under Windows' box stays for as long as that box is there.)
            heldTheWindow = desk.view.holdsItsWindow();
            // And nothing is asked from behind it.
            desk.view.makeFile(thePassphrase());
            desk.view.saveFile();
            desk.view.chooseFile();
        };
        desk.view.saveFile();
        QVERIFY(heldTheWindow);
        QVERIFY(!desk.view.holdsItsWindow());
        QCOMPARE(desk.suggestions.size(), 1);
        QCOMPARE(desk.opened, 0);
        QVERIFY(!QFile::exists(desk.savesTo));
        QCOMPARE(desk.exported.count(), 0);
        QVERIFY(!desk.view.isMade());
        QVERIFY(!desk.view.isBusy());
        QVERIFY(!desk.exchange.isBusy());
    }

    void theNameAFileIsKeptUnderSaysWhatItIs_data()
    {
        // What comes back from Windows' box, for a file that is sealed or plain; and the name
        // the file is kept under.
        QTest::addColumn<bool>("sealed");
        QTest::addColumn<QString>("chosen");
        QTest::addColumn<QString>("keptAs");
        QTest::newRow("a sealed file, named as one") << true << u"wall.qkvm"_s << u"wall.qkvm"_s;
        QTest::newRow("a sealed file, named as a plain one") << true << u"wall.json"_s << u"wall.qkvm"_s;
        QTest::newRow("a sealed file, with no ending") << true << u"wall"_s << u"wall.qkvm"_s;
        QTest::newRow("a sealed file, with another ending") << true << u"wall.txt"_s << u"wall.txt.qkvm"_s;
        QTest::newRow("a sealed file, named in large letters") << true << u"WALL.qKVM"_s << u"WALL.qKVM"_s;
        QTest::newRow("a plain file, named as one") << false << u"wall.json"_s << u"wall.json"_s;
        QTest::newRow("a plain file, named as a sealed one") << false << u"wall.qkvm"_s << u"wall.json"_s;
        QTest::newRow("a plain file, named as a sealed one in large letters")
            << false << u"Wall.qKVM"_s << u"Wall.json"_s;
        QTest::newRow("a plain file, with no ending") << false << u"wall"_s << u"wall.json"_s;
        QTest::newRow("a plain file, with a dot and no ending") << false << u"wall."_s << u"wall.json"_s;
        // What Windows drops of a name anyway, a dot or a blank at its end, is dropped first; and
        // a wrong ending that stands last is taken off before the right one is put on, which is
        // not put on twice.
        QTest::newRow("a sealed file, named as a sealed one and then as a plain one")
            << true << u"wall.qkvm.json"_s << u"wall.qkvm"_s;
        QTest::newRow("a plain file, named as a plain one and then as a sealed one")
            << false << u"wall.json.qkvm"_s << u"wall.json"_s;
        QTest::newRow("a sealed file, with a dot after its ending") << true << u"wall.qkvm."_s << u"wall.qkvm"_s;
        QTest::newRow("a sealed file, with a blank after its ending") << true << u"wall.qkvm "_s << u"wall.qkvm"_s;
        QTest::newRow("a plain file, with blanks and dots after a sealed one's ending")
            << false << u"wall.qkvm . "_s << u"wall.json"_s;
        QTest::newRow("a plain file, with a blank and no ending") << false << u"wall "_s << u"wall.json"_s;
    }

    void theNameAFileIsKeptUnderSaysWhatItIs()
    {
        QFETCH(bool, sealed);
        QFETCH(QString, chosen);
        QFETCH(QString, keptAs);
        QCOMPARE(ui::namedAsWhatItIs(chosen, sealed), keptAs);
        // (And with the folder it is in, which is none of the name's ending.)
        QCOMPARE(ui::namedAsWhatItIs(u"C:/a folder.qkvm/"_s + chosen, sealed), u"C:/a folder.qkvm/"_s + keptAs);
        WallOfOneTest own;
        QVERIFY(!own.put(u"http://127.0.0.1:20001"_s, u"Build server"_s).isEmpty());
        Desk desk(own);
        QVERIFY(desk.make(sealed ? thePassphrase() : QString()));
        QVERIFY(desk.onShowForItsMoment());
        // Whatever name comes back from Windows' box, the file is kept under one that says
        // what it is: a sealed file is no ".json", and a plain one no ".qkvm".
        desk.savesTo = desk.folder.filePath(chosen);
        desk.view.saveFile();
        QCOMPARE(desk.view.problem(), QString());
        QCOMPARE(desk.exported.count(), 1);
        // The window's corner names the file that was written, by its name: above all where it
        // is another than the one Windows' box was given.
        QCOMPARE(desk.exported.at(0).at(0).toString(),
                 sealed ? u"Exported to %1. The file opens with its passphrase only: keep the two apart."_s.arg(keptAs)
                        : u"Exported to %1, without passwords"_s.arg(keptAs));
        const QString kept = desk.folder.filePath(keptAs);
        QVERIFY2(QFile::exists(kept), qPrintable(kept));
        QCOMPARE(QDir(desk.folder.path()).entryList(QDir::Files), QStringList{keptAs});
        const base::Result<core::exchange::Content> read =
            core::exchange::read(Desk::read(kept), sealed ? thePassphrase() : QString());
        QVERIFY2(read.ok(), read.ok() ? "" : read.error().message.c_str());
        QCOMPARE(read.value().sealed, sealed);
        // (And only that kind of file was offered: the name that was suggested says which.)
        QCOMPARE(desk.suggestions.size(), 1);
        QVERIFY(desk.suggestions.constFirst().endsWith(sealed ? u".qkvm"_s : u".json"_s));
    }

    void aFileIsNotWrittenOverAnotherThatNobodyWasAskedAbout()
    {
        WallOfOneTest own;
        QVERIFY(!own.put(u"http://127.0.0.1:20001"_s, u"Build server"_s).isEmpty());
        Desk desk(own);
        // Two files that are there: a sealed one, and a plain one beside it.
        const QString sealedThere = desk.write(u"wall.qkvm"_s, "a sealed file that was there");
        const QString plainThere = desk.write(u"wall.json"_s, "a plain file that was there");
        // A plain file is made, and "wall.qkvm" is chosen for it in Windows' box, which asked
        // whether that one is to be written over. It is not: a plain file is not kept under a
        // sealed one's name. Nor is it written over "wall.json", the name that says what it is:
        // nobody was asked about that one. Said, and the file is still there to be saved.
        QVERIFY(desk.make(QString()));
        QVERIFY(desk.onShowForItsMoment());
        desk.savesTo = sealedThere;
        desk.view.saveFile();
        QCOMPARE(desk.view.problem(),
                 u"This file is kept under a name that says what it is: wall.json. A file of that name is there "
                 "already, and nobody was asked whether to write over it. Choose another name."_s);
        QCOMPARE(desk.exported.count(), 0);
        QVERIFY(desk.view.isMade());
        QCOMPARE(Desk::read(sealedThere), QByteArray("a sealed file that was there"));
        QCOMPARE(Desk::read(plainThere), QByteArray("a plain file that was there"));
        // Chosen by its own name, Windows' box has asked: it is written over, and the sealed
        // one beside it is as it was.
        desk.savesTo = plainThere;
        desk.view.saveFile();
        QCOMPARE(desk.view.problem(), QString());
        QCOMPARE(desk.exported.count(), 1);
        QVERIFY(Desk::read(plainThere).contains("Build server"));
        QCOMPARE(Desk::read(sealedThere), QByteArray("a sealed file that was there"));

        // The other way round: a sealed file, and "wall.json" chosen for it.
        QVERIFY(desk.make(thePassphrase()));
        QVERIFY(desk.onShowForItsMoment());
        const QByteArray plainNow = Desk::read(plainThere);
        desk.savesTo = plainThere;
        desk.view.saveFile();
        QVERIFY2(desk.view.problem().startsWith(u"This file is kept under a name that says what it is: wall.qkvm."_s),
                 qPrintable(desk.view.problem()));
        QCOMPARE(desk.exported.count(), 1);
        QCOMPARE(Desk::read(plainThere), plainNow);
        QCOMPARE(Desk::read(sealedThere), QByteArray("a sealed file that was there"));
        // And under a name that is free, it is kept as what it is.
        desk.savesTo = desk.folder.filePath(u"another.json"_s);
        desk.view.saveFile();
        QCOMPARE(desk.exported.count(), 2);
        QVERIFY(!QFile::exists(desk.folder.filePath(u"another.json"_s)));
        QVERIFY(core::exchange::read(Desk::read(desk.folder.filePath(u"another.qkvm"_s)), thePassphrase()).ok());
    }

    void aFolderUnderTheNameAFileWouldBeKeptUnderIsCalledAFolder()
    {
        WallOfOneTest own;
        QVERIFY(!own.put(u"http://127.0.0.1:20001"_s, u"Build server"_s).isEmpty());
        Desk desk(own);
        // A folder that is called as the plain file would be; and "box.qkvm" chosen for it.
        QVERIFY(QDir(desk.folder.path()).mkdir(u"box.json"_s));
        QVERIFY(desk.make(QString()));
        QVERIFY(desk.onShowForItsMoment());
        desk.savesTo = desk.folder.filePath(u"box.qkvm"_s);
        desk.view.saveFile();
        QCOMPARE(desk.view.problem(),
                 u"This file is kept under a name that says what it is: box.json. A folder of that "
                 "name is there already. Choose another name."_s);
        QCOMPARE(desk.exported.count(), 0);
        QVERIFY(desk.view.isMade());
        QVERIFY(QFileInfo(desk.folder.filePath(u"box.json"_s)).isDir());
        QVERIFY(!QFile::exists(desk.folder.filePath(u"box.qkvm"_s)));
    }

    void aNameWhoseDotWindowsDropsNamesTheFileItsBoxAskedAbout()
    {
        WallOfOneTest own;
        QVERIFY(!own.put(u"http://127.0.0.1:20001"_s, u"Build server"_s).isEmpty());
        Desk desk(own);
        const QString sealedThere = desk.write(u"wall.qkvm"_s, "a sealed file that was there");
        // Windows takes a name with dots or blanks at its end for the name without them, by its
        // own reckoning: the file there is that one, and it is the one its box asked whether to
        // write over.
        // (And the program takes each of them as Windows does.)
        for (const QString& end : {u"."_s, u".."_s, u" "_s, u" . "_s, u". "_s}) {
            QCOMPARE(asWindowsNamesIt(sealedThere + end), asWindowsNamesIt(sealedThere));
            QCOMPARE(ui::asWindowsTakesIt(sealedThere + end), sealedThere);
        }
        QVERIFY(!asWindowsNamesIt(sealedThere).isEmpty());
        // A sealed file, and "wall.qkvm." handed back from Windows' box, its dot and all: what
        // that box asked about is the file of that name without its dot, which is the name that
        // says what the file is. It is written over, as was asked.
        QVERIFY(desk.make(thePassphrase()));
        QVERIFY(desk.onShowForItsMoment());
        desk.savesTo = sealedThere + u"."_s;
        desk.view.saveFile();
        QCOMPARE(desk.view.problem(), QString());
        QCOMPARE(desk.exported.count(), 1);
        QCOMPARE(desk.exported.at(0).at(0).toString(),
                 u"Exported to wall.qkvm. The file opens with its passphrase only: keep the two apart."_s);
        QVERIFY(core::exchange::read(Desk::read(sealedThere), thePassphrase()).ok());
        QCOMPARE(QDir(desk.folder.path()).entryList(QDir::Files), QStringList{u"wall.qkvm"_s});
        // So with blanks and dots after it.
        QVERIFY(desk.make(thePassphrase()));
        QVERIFY(desk.onShowForItsMoment());
        desk.savesTo = sealedThere + u" . "_s;
        desk.view.saveFile();
        QCOMPARE(desk.view.problem(), QString());
        QCOMPARE(desk.exported.count(), 2);
        // What the box did not ask about is not written over: a name that is made another by
        // more than what Windows drops of it ("wall.json." for a sealed file, where "wall.qkvm"
        // is there).
        QVERIFY(desk.make(thePassphrase()));
        QVERIFY(desk.onShowForItsMoment());
        const QByteArray now = Desk::read(sealedThere);
        desk.savesTo = desk.folder.filePath(u"wall.json."_s);
        desk.view.saveFile();
        QCOMPARE(desk.view.problem(),
                 u"This file is kept under a name that says what it is: wall.qkvm. A file of that name is there "
                 "already, and nobody was asked whether to write over it. Choose another name."_s);
        QCOMPARE(desk.exported.count(), 2);
        QCOMPARE(Desk::read(sealedThere), now);
        QCOMPARE(QDir(desk.folder.path()).entryList(QDir::Files), QStringList{u"wall.qkvm"_s});
    }

    // --- import: a file is chosen and read ------------------------------------------------------

    void aFileSaysOfItselfWhetherItIsSealedAndOnlyThenIsAPassphraseAskedFor()
    {
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(!desk.view.isSealed());
        QCOMPARE(desk.view.fileName(), QString());
        // Nobody chooses one: nothing changes, and nothing is said to be wrong.
        desk.view.chooseFile();
        QCOMPARE(desk.opened, 1);
        QCOMPARE(desk.view.fileName(), QString());
        QCOMPARE(desk.view.problem(), QString());
        desk.view.readFile(QString());
        QCOMPARE(desk.view.problem(), u"Choose a file first."_s);

        const QByteArray sealed = fileOf({ticked(1, u"One"_s)});
        const QByteArray plain = fileOf({ticked(2, u"Two"_s)}, false);
        desk.choose(u"sealed.qkvm"_s, sealed);
        QCOMPARE(desk.view.fileName(), u"sealed.qkvm"_s);
        QVERIFY(desk.view.isSealed());
        QCOMPARE(desk.view.problem(), QString());
        QCOMPARE(desk.view.bytesHeld(), sealed.size());
        QVERIFY(!desk.view.isLaidOut());
        // A plain one asks for none: and is read with none, whatever stands in a line that
        // nobody sees.
        desk.choose(u"plain.json"_s, plain);
        QCOMPARE(desk.view.fileName(), u"plain.json"_s);
        QVERIFY(!desk.view.isSealed());
        desk.view.readFile(u"left over in a line"_s);
        QVERIFY(desk.view.isBusy());
        QVERIFY(desk.view.isReading());
        QTRY_VERIFY(desk.view.isLaidOut());
        QCOMPARE(desk.fileWasRead.count(), 1);
        const QList<Row> rows = rowsOf(desk.view);
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.at(0).nameAndAddress, u"\"Two\" at https://192.0.2.2"_s);
        // Nothing on the wall has changed, and nothing was asked of anything.
        QVERIFY(own.wall.kvms().isEmpty());
        // The bytes of the file are let go as soon as it was laid out.
        QCOMPARE(desk.view.bytesHeld(), 0);
    }

    void aFileThatIsNotReadSaysWhyAndNothingIsLaidOut_data()
    {
        QTest::addColumn<QString>("which");
        QTest::addColumn<QString>("said");
        // Whether it is said when the file is chosen (it says of itself that it is none), or
        // when it is read; and whether it stays chosen, for another try.
        QTest::addColumn<bool>("atChoosing");
        QTest::addColumn<bool>("staysChosen");
        QTest::newRow("a wrong passphrase")
            << u"wrong"_s << u"That is not the passphrase this file was made with"_s << false << true;
        QTest::newRow("no passphrase for a sealed file")
            << u"none"_s << u"This file is encrypted. Enter the passphrase it was made with."_s << false << true;
        QTest::newRow("a damaged file") << u"damaged"_s
                                        << u"The file is damaged: what is in it does not open with its own key"_s
                                        << false << false;
        QTest::newRow("a file of a newer kind")
            << u"newer"_s << u"This file was made by a later version of qKVM than this one. Update this one first."_s
            << true << false;
        QTest::newRow("a file that is none of qKVM's")
            << u"none of qKVM's"_s << u"This is not a file that qKVM made"_s << true << false;
        QTest::newRow("more signs than a file of qKVM's has")
            << u"too many signs"_s << u"The file is larger than anything qKVM writes"_s << true << false;
        QTest::newRow("more bytes than are read from the disk")
            << u"too many bytes"_s << u"The file is larger than anything qKVM writes."_s << true << false;
    }

    void aFileThatIsNotReadSaysWhyAndNothingIsLaidOut()
    {
        QFETCH(QString, which);
        QFETCH(QString, said);
        QFETCH(bool, atChoosing);
        QFETCH(bool, staysChosen);
        WallOfOneTest own;
        Desk desk(own);
        const QByteArray good = fileOf({ticked(1, u"One"_s)});
        QByteArray file = good;
        QString passphrase = thePassphrase();
        if (which == u"wrong"_s) {
            passphrase = u"another passphrase"_s;
        } else if (which == u"none"_s) {
            passphrase.clear();
        } else if (which == u"damaged"_s) {
            // What is sealed in it is another by one sign: it does not open with its own key.
            QJsonObject parts = QJsonDocument::fromJson(good).object();
            QJsonObject sealed = parts[u"sealed"_s].toObject();
            QString data = sealed[u"data"_s].toString();
            QVERIFY(data.size() > 20);
            data[10] = data.at(10) == u'A' ? u'B' : u'A';
            sealed[u"data"_s] = data;
            parts[u"sealed"_s] = sealed;
            file = QJsonDocument(parts).toJson(QJsonDocument::Compact);
        } else if (which == u"newer"_s) {
            QJsonObject parts = QJsonDocument::fromJson(good).object();
            parts[u"version"_s] = 2;
            file = QJsonDocument(parts).toJson(QJsonDocument::Compact);
        } else if (which == u"none of qKVM's"_s) {
            file = "a letter to somebody, sealed";
        } else if (which == u"too many signs"_s) {
            file = core::testing::plainFile({{u"devices"_s, QJsonArray()}}) +
                   QByteArray(core::exchange::largestSealed + 1, ' ');
        } else if (which == u"too many bytes"_s) {
            file = QByteArray(OwnFilesView::largestFileBytes + 1, ' ');
        }
        desk.choose(u"wall.qkvm"_s, file);
        if (atChoosing) {
            QCOMPARE(desk.view.problem(), said);
        } else {
            QCOMPARE(desk.view.problem(), QString());
            QCOMPARE(desk.view.fileName(), u"wall.qkvm"_s);
            desk.view.readFile(passphrase);
            QTRY_VERIFY(!desk.view.isBusy());
            QCOMPARE(desk.view.problem(), said);
        }
        // Nothing is laid out, here or in the core, and nothing is on the wall.
        QVERIFY(!desk.view.isLaidOut());
        QCOMPARE(desk.view.offered()->rowCount(), 0);
        QCOMPARE(desk.view.tickedCount(), 0);
        QCOMPARE(desk.exchange.passwordsHeld(), 0);
        QVERIFY(own.wall.kvms().isEmpty());
        QCOMPARE(desk.fileWasRead.count(), 0);
        desk.view.setLookedAt(true);
        QTest::qWait(50);
        desk.view.take();
        QVERIFY(own.wall.kvms().isEmpty());
        // What no other try will read is not kept; a file whose passphrase was not the one
        // stays chosen, and is read with the right one.
        QCOMPARE(desk.view.fileName(), staysChosen ? u"wall.qkvm"_s : QString());
        QCOMPARE(desk.view.bytesHeld(), staysChosen ? file.size() : 0);
        if (staysChosen) {
            desk.view.readFile(thePassphrase());
            QTRY_VERIFY(desk.view.isLaidOut());
            QCOMPARE(desk.view.problem(), QString());
            QCOMPARE(desk.view.bytesHeld(), 0);
        }
    }

    void whatWasTypedForAFileIsToldToGoWhenThatFileIsNotTheChosenOneAnyMore()
    {
        // The passphrase is typed into a line of the screen, and is that file's: the screens are
        // told whenever the file it was typed for is not the chosen one any more, and empty
        // the line (the screens' own test holds them to that). Told here: at every way a file
        // stops being the chosen one. And not where it stays chosen for another try.
        WallOfOneTest own;
        Desk desk(own, 2000);
        const QByteArray sealed = fileOf({ticked(1, u"One"_s)});
        const auto toldSince = [&desk](qsizetype before) { return desk.fileIsAnother.count() - before; };
        qsizetype before = desk.fileIsAnother.count();
        // A file is chosen, and a wrong passphrase typed for it: it stays chosen, and what was
        // typed stays, to be put right.
        desk.choose(u"first.qkvm"_s, sealed);
        before = desk.fileIsAnother.count();
        desk.view.readFile(u"not the passphrase"_s);
        QTRY_VERIFY(!desk.view.isBusy());
        QCOMPARE(desk.view.fileName(), u"first.qkvm"_s);
        QCOMPARE(toldSince(before), 0);
        // Windows' box for another file, given up: the same file is chosen still.
        desk.opens.clear();
        desk.view.chooseFile();
        QCOMPARE(toldSince(before), 0);
        // Another sealed file is chosen: what was typed was the first one's.
        desk.choose(u"second.qkvm"_s, sealed);
        QCOMPARE(toldSince(before), 1);
        // A plain file is chosen after it.
        before = desk.fileIsAnother.count();
        desk.choose(u"plain.json"_s, fileOf({ticked(2, u"Two"_s)}, false));
        QCOMPARE(toldSince(before), 1);
        // A file is refused as one that no other try will read (it is damaged): it is not the
        // chosen one any more.
        QJsonObject parts = QJsonDocument::fromJson(sealed).object();
        QJsonObject inside = parts[u"sealed"_s].toObject();
        QString data = inside[u"data"_s].toString();
        data[10] = data.at(10) == u'A' ? u'B' : u'A';
        inside[u"data"_s] = data;
        parts[u"sealed"_s] = inside;
        desk.choose(u"damaged.qkvm"_s, QJsonDocument(parts).toJson(QJsonDocument::Compact));
        before = desk.fileIsAnother.count();
        desk.view.readFile(thePassphrase());
        QTRY_VERIFY(!desk.view.isBusy());
        QCOMPARE(desk.view.fileName(), QString());
        QVERIFY(!desk.view.problem().isEmpty());
        QCOMPARE(toldSince(before), 1);
        // One that is not a file of QKVM's at all, or is too large, is none from the choosing.
        before = desk.fileIsAnother.count();
        desk.choose(u"letter.qkvm"_s, "a letter to somebody, sealed");
        QCOMPARE(desk.view.fileName(), QString());
        QCOMPARE(toldSince(before), 1);
        // A file is read, and its list goes by itself when its time is over.
        QVERIFY(desk.layOut(sealed));
        before = desk.fileIsAnother.count();
        QTRY_VERIFY_WITH_TIMEOUT(!desk.view.isLaidOut(), 20000);
        QCOMPARE(toldSince(before), 1);
        // And the box is closed.
        desk.choose(u"last.qkvm"_s, sealed);
        before = desk.fileIsAnother.count();
        desk.view.reset();
        QCOMPARE(toldSince(before), 1);
    }

    // --- import: what is laid out, and which of it is ticked -----------------------------------

    void everyLineBesideTheListStandsBeforeTheFirstDeviceAndEveryDeviceIsAShortRowWithTheRestBehindIt()
    {
        WallOfOneTest own;
        QVERIFY(!own.put(u"https://192.0.2.9"_s, u"There already"_s).isEmpty());
        Desk desk(own);
        // A file as a server writes one, by hand: a device that is left out (it has no address),
        // one saved action, and four devices.
        const QJsonObject pinned =
            QJsonDocument::fromJson(fileOf({knownBy(kvmAt(elsewhere(1), u"Pinned"_s), "first")}, false))
                .object()[u"devices"_s]
                .toArray()
                .first()
                .toObject();
        QVERIFY(!pinned.isEmpty());
        const auto device = [](const QString& name, const QString& url, const QString& kind) {
            return QJsonObject{
                {u"name"_s, name},   {u"url"_s, url},         {u"username"_s, u"admin"_s},   {u"kind"_s, kind},
                {u"group"_s, u""_s}, {u"tls"_s, u"system"_s}, {u"cert_pem"_s, QJsonValue()}, {u"open_url"_s, u""_s},
                {u"alerts"_s, true}, {u"ident"_s, u""_s}};
        };
        // (One that cannot be taken: a user name of forty signs from far up, which this wall does not keep.)
        const auto notKept = [](QJsonObject entry) {
            const char32_t wide = 0x1F600;
            entry.insert(u"username"_s, QString::fromUcs4(&wide, 1).repeated(40));
            return entry;
        };
        const QByteArray file = core::testing::plainFile(
            {{u"devices"_s, QJsonArray{pinned, device(u"Nowhere"_s, QString(), u"kvmd"_s),
                                       device(u"In the clear"_s, u"http://192.0.2.3:8080"_s, u"kvmd"_s),
                                       notKept(device(u"A small one"_s, u"https://192.0.2.4"_s, u"kvmd"_s)),
                                       device(u"Twice"_s, u"https://192.0.2.9"_s, u"kvmd"_s)}},
             {u"actions"_s, QJsonArray{QJsonObject{{u"name"_s, u"Lock"_s},
                                                   {u"kind"_s, u"keys"_s},
                                                   {u"keys"_s, u"MetaLeft,KeyL"_s},
                                                   {u"enter"_s, false},
                                                   {u"danger"_s, false}}}}});
        QVERIFY2(desk.layOut(file, QString()), qPrintable(desk.view.problem()));
        // What stands beside the devices is on top, before any of them, in the same list: one
        // row that stands for it, folded; and once that is opened, every sentence of what the
        // reader left out, and what the file holds that is not this PC's.
        QCOMPARE(kindsOf(desk.view),
                 (QStringList{u"fold"_s, u"device"_s, u"device"_s, u"device"_s, u"device"_s, u"action"_s}));
        QCOMPARE(rowsOf(desk.view).at(0).text, u"1 note about this file"_s);
        QVERIFY(desk.settledForItsMoment());
        desk.view.openNotes();
        const QList<Row> rows = rowsOf(desk.view);
        const qsizetype beside = desk.view.offered()->besideCount();
        QCOMPARE(beside, 1);
        QCOMPARE(rows.size(), beside + 5);
        for (qsizetype at = 0; at < rows.size(); ++at) {
            QCOMPARE(rows.at(at).kind, at < beside ? u"beside"_s : at < beside + 4 ? u"device"_s : u"action"_s);
            QVERIFY(at >= beside || !rows.at(at).text.isEmpty());
        }
        QCOMPARE(rows.at(0).text, u"Device 2: it has no address"_s);

        // Each device in the file's order, by its place there: a row with its name and address
        // in one piece and the one thing that matters of it, in a few words. No row is opened
        // to begin with.
        const Row& first = rows.at(beside);
        QCOMPARE(first.place, 1);
        QCOMPARE(first.nameAndAddress, u"\"Pinned\" at https://192.0.2.1"_s);
        QVERIFY(first.canBeTaken);
        QVERIFY(first.ticked);
        QVERIFY(!first.wantsALook);
        QVERIFY(!first.open);
        // Its certificate and missing password are both on this device's row. A mixed
        // device/action list cannot describe every item as lacking a device password.
        const QByteArray sha256 = knownBy(kvmAt(elsewhere(1)), "first").certificate;
        QCOMPARE(sha256.size(), 32);
        const QString noPassword = u' ' + QString(QChar(0x00B7)) + u" No password in the file"_s;
        QCOMPARE(first.inShort, u"Known by its certificate "_s + inShortOf(sha256) + noPassword);
        QVERIFY2(
            QRegularExpression(u"\\AKnown by its certificate [0-9A-F]{2}:[0-9A-F]{2} . [0-9A-F]{2}:[0-9A-F]{2}\\z"_s)
                .match(first.inShort.first(first.inShort.size() - noPassword.size()))
                .hasMatch(),
            qPrintable(first.inShort));
        // Behind its row, nothing of it is lost: its verdict, as a sentence; each sentence that
        // is said of it, by itself, as the core hands them over and in its order (what it will
        // be known by, and that the file has no password for it); and its certificate's SHA-256
        // in four lines of eight pairs, whole: the letters one is compared in.
        QCOMPARE(first.verdict, u"It can be taken, as a KVM that runs kvmd (PiKVM, GL.iNet Comet)."_s);
        QCOMPARE(first.says,
                 (QStringList{u"The file names its certificate. It will be known by that certificate and by no "
                              "other."_s,
                              u"The file has no password for it. It is taken without one, and its password is to "
                              "be typed on this PC."_s}));
        QVERIFY2(QRegularExpression(u"\\A(?:(?:[0-9A-F]{2}:){7}[0-9A-F]{2}\\n){3}(?:[0-9A-F]{2}:){7}[0-9A-F]{2}\\z"_s)
                     .match(first.fingerprint)
                     .hasMatch(),
                 qPrintable(first.fingerprint));
        QCOMPARE(QString(first.fingerprint).replace(u'\n', u':'), core::kvm::fingerprint(sha256));
        // The one whose password goes in the clear is not ticked, and its row says why: that
        // is not left for what stands behind it. There, the whole sentence.
        const Row& plain = rows.at(beside + 1);
        QCOMPARE(plain.place, 3);
        QCOMPARE(plain.nameAndAddress, u"\"In the clear\" at http://192.0.2.3:8080"_s);
        QVERIFY(plain.canBeTaken);
        QVERIFY(!plain.ticked);
        QVERIFY(plain.wantsALook);
        QCOMPARE(plain.inShort, u"Plain HTTP: its password goes in the clear"_s + noPassword);
        QCOMPARE(plain.says.size(), 2);
        QVERIFY2(plain.says.at(0).startsWith(u"It is spoken to over plain HTTP: its password goes in the clear"_s),
                 qPrintable(plain.says.at(0)));
        QCOMPARE(plain.fingerprint, QString());
        // One that cannot be taken is shown all the same, with why in a few words on its row
        // and as a sentence behind it; it has no tick, and nothing about it asks for a look.
        const Row& small = rows.at(beside + 2);
        QCOMPARE(small.place, 4);
        QCOMPARE(small.nameAndAddress, u"\"A small one\" at https://192.0.2.4"_s);
        QVERIFY(!small.canBeTaken);
        QVERIFY(!small.ticked);
        QVERIFY(!small.wantsALook);
        QCOMPARE(small.inShort, u"Cannot be taken: this PC's wall does not keep it as the file has it"_s);
        QVERIFY2(
            small.verdict.startsWith(u"It cannot be taken: this PC's wall does not keep what the file has for it"_s),
            qPrintable(small.verdict));
        QVERIFY(small.says.isEmpty());
        const Row& twice = rows.at(beside + 3);
        QCOMPARE(twice.place, 5);
        QVERIFY(!twice.canBeTaken);
        QVERIFY(!twice.wantsALook);
        QCOMPARE(twice.inShort, u"Cannot be taken: on the wall already, or earlier in this file"_s);
        QCOMPARE(twice.verdict,
                 u"It cannot be taken: it is on the wall already (\"There already\" at https://192.0.2.9)."_s);
        const Row& action = rows.at(beside + 4);
        QCOMPARE(action.place, static_cast<int>(WallExchange::firstActionPlace));
        QCOMPARE(action.nameAndAddress, u"Saved action: \"Lock\""_s);
        QVERIFY(action.canBeTaken);
        QVERIFY(action.ticked);
        QCOMPARE(action.inShort, u"Presses keys: MetaLeft,KeyL"_s);
        QCOMPARE(desk.view.tickedCount(), 2);
        // And once, for all of them, what is so of the list as a whole.
        QCOMPARE(
            desk.view.counts(),
            u"5 items from this file are listed. 2 are ticked. 1 is not ticked and wants a look. 2 cannot be taken."_s);
        // No password of the file is in anything the box is handed. (The plain file has none;
        // a sealed one is in the tests of the taking.)
        QCOMPARE(own.wall.kvms().size(), 1);
    }

    void whichDevicesAreTickedToBeginWith_data()
    {
        QTest::addColumn<Device>("device");
        QTest::addColumn<bool>("canBeTaken");
        QTest::addColumn<bool>("isTicked");
        // What its row says of it in short: what it will be known by, where it is ticked; and
        // every reason why not, where it can be taken and is not ticked.
        QTest::addColumn<QString>("inShort");
        const Device pinned = knownBy(kvmAt(elsewhere(1), u"Pinned"_s), "first");
        QTest::newRow("known by the certificate its file names, somewhere else")
            << pinned << true << true << u"Known by its certificate "_s + inShortOf(pinned.certificate);
        QTest::newRow("known by Windows' word, somewhere else")
            << saying(kvmAt(elsewhere(2), u"Vouched for"_s), u"system"_s) << true << true
            << u"Known by Windows' word"_s;
        QTest::newRow("no certificate named, somewhere else")
            << kvmAt(elsewhere(5), u"None named"_s) << true << true
            << u"Known by Windows' word, not by what it shows first"_s;
        QTest::newRow("its certificate is not checked, somewhere else")
            << saying(kvmAt(elsewhere(6), u"Not checked"_s), u"insecure"_s) << true << true
            << u"Known by Windows' word, not by what it shows first"_s;
        QTest::newRow("its password goes in the clear") << kvmAt(u"http://192.0.2.3"_s, u"In the clear"_s) << true
                                                        << false << u"Plain HTTP: its password goes in the clear"_s;
        QTest::newRow("a name in its xn-- form")
            << saying(kvmAt(u"https://xn--80ak6aa92e.invalid"_s, u"Looks like another"_s), u"system"_s) << true << false
            << u"Its address is a name in xn-- form: compare it sign for sign"_s;
        QTest::newRow("this PC itself") << saying(kvmAt(u"https://127.0.0.1:20443"_s, u"Here"_s), u"system"_s) << true
                                        << false << u"Its address is this PC itself"_s;
        QTest::newRow("this PC by its name") << saying(kvmAt(u"https://localhost:20443"_s, u"Here too"_s), u"system"_s)
                                             << true << false << u"Its address is this PC itself"_s;
        QTest::newRow("of the kind no router passes on")
            << saying(kvmAt(u"https://169.254.7.7"_s, u"Link-local"_s), u"system"_s) << true << false
            << u"Its address is of the kind no router passes on"_s;
        QTest::newRow("in the clear, and at this PC")
            << kvmAt(u"http://127.0.0.1:20001"_s, u"Both"_s) << true << false
            << u"Plain HTTP: its password goes in the clear"_s + dot() + u"Its address is this PC itself"_s;
        QTest::newRow("one that cannot be taken")
            << notKeptHere(saying(kvmAt(elsewhere(4), u"A small one"_s), u"system"_s)) << false << false
            << u"Cannot be taken: this PC's wall does not keep it as the file has it"_s;
    }

    void whichDevicesAreTickedToBeginWith()
    {
        QFETCH(Device, device);
        QFETCH(bool, canBeTaken);
        QFETCH(bool, isTicked);
        QFETCH(QString, inShort);
        WallOfOneTest own;
        Desk desk(own);
        // (Beside one that is ticked: what is counted is each by itself.)
        QVERIFY2(desk.layOut(fileOf({device, ticked(200, u"Beside it"_s)})), qPrintable(desk.view.problem()));
        const QList<Row> rows = rowsOf(desk.view);
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows.at(0).canBeTaken, canBeTaken);
        QCOMPARE(rows.at(0).ticked, isTicked);
        // Why one that can be taken is not ticked stands on its row, and is a question to
        // whoever reads it; one that cannot be taken asks nothing.
        QCOMPARE(rows.at(0).inShort, inShort);
        QCOMPARE(rows.at(0).wantsALook, canBeTaken && !isTicked);
        QVERIFY(rows.at(1).ticked);
        QCOMPARE(rows.at(1).inShort, u"Known by Windows' word"_s);
        QCOMPARE(desk.view.tickedCount(), isTicked ? 2 : 1);
        QCOMPARE(tickedIn(desk.view), isTicked ? (QList<int>{1, 2}) : (QList<int>{2}));
        // Whoever wants one that can be taken ticks it; and takes the tick away again.
        QVERIFY(desk.settledForItsMoment());
        const QSignalSpy tickedChanged(&desk.view, &OwnFilesView::tickedChanged);
        desk.view.setTicked(1, true);
        QCOMPARE(desk.view.tickedCount(), canBeTaken ? 2 : 1);
        QCOMPARE(tickedChanged.count(), canBeTaken && !isTicked ? 1 : 0);
        desk.view.setTicked(1, false);
        QCOMPARE(desk.view.tickedCount(), 1);
        QVERIFY(own.wall.kvms().isEmpty());
    }

    void aDeviceThatCannotBeTakenCannotBeTickedByAnyCallTheScreenCouldMake()
    {
        WallOfOneTest own;
        QVERIFY(!own.put(u"https://192.0.2.9"_s, u"There already"_s).isEmpty());
        Desk desk(own);
        QVERIFY(desk.layOut(fileOf({notKeptHere(ticked(1, u"A small one"_s)), ticked(9, u"Twice"_s),
                                    ticked(2, u"Good"_s), notKeptHere(ticked(3, u"A server"_s))})));
        QCOMPARE(tickedIn(desk.view), (QList<int>{3}));
        const QSignalSpy dataChanged(desk.view.offered(), &QAbstractItemModel::dataChanged);
        // By the view's own call, as a tick's click makes it, by name as the screens call it; by
        // the list's own way of being written to; with a place that is none. (Once the list has
        // stood for its moment, when a tick that can be had is had.)
        QVERIFY(desk.settledForItsMoment());
        for (const int place : {1, 2, 4}) {
            desk.view.setTicked(place, true);
            QVERIFY(QMetaObject::invokeMethod(&desk.view, "setTicked", Q_ARG(int, place), Q_ARG(bool, true)));
            QVERIFY(!desk.view.offered()->setTicked(place, true));
        }
        desk.view.setTicked(0, true);
        desk.view.setTicked(5, true);
        desk.view.setTicked(-1, true);
        for (int row = 0; row < desk.view.offered()->rowCount(); ++row) {
            const QModelIndex index = desk.view.offered()->index(row, 0);
            QVERIFY(!desk.view.offered()->setData(index, true, LaidOutList::TickedRole));
            QVERIFY(!desk.view.offered()->setData(index, true, LaidOutList::CanBeTakenRole));
            QVERIFY(!desk.view.offered()->setData(index, true, Qt::EditRole));
        }
        QCOMPARE(tickedIn(desk.view), (QList<int>{3}));
        QCOMPARE(desk.view.tickedCount(), 1);
        QCOMPARE(dataChanged.count(), 0);
        // And what is taken is the one that was to be had.
        QVERIFY(desk.lookedAtForItsMoment());
        desk.view.take();
        QCOMPARE(desk.taken.count(), 1);
        QCOMPARE(own.names(), (QStringList{u"There already"_s, u"Good"_s}));
    }

    void aDeviceThatCannotBeTakenShowsNoCertificate()
    {
        // One that cannot be taken says why, and nothing of how it would be known: the
        // certificate its file names is not shown to be compared, for a device that is not taken.
        WallOfOneTest own;
        QVERIFY(!own.put(u"https://192.0.2.9"_s, u"There already"_s).isEmpty());
        Desk desk(own);
        QVERIFY(desk.layOut(fileOf({knownBy(kvmAt(u"https://192.0.2.9"_s, u"Twice"_s), "first"),
                                    knownBy(kvmAt(elsewhere(1), u"Pinned"_s), "second")})));
        const QList<Row> rows = rowsOf(desk.view);
        QCOMPARE(rows.size(), 2);
        QVERIFY(!rows.at(0).canBeTaken);
        QCOMPARE(rows.at(0).inShort, u"Cannot be taken: on the wall already, or earlier in this file"_s);
        QCOMPARE(rows.at(0).fingerprint, QString());
        QVERIFY(rows.at(0).says.isEmpty());
        QVERIFY(rows.at(1).canBeTaken);
        QCOMPARE(rows.at(1).fingerprint.count(u'\n'), 3);
        QCOMPARE(rows.at(1).inShort,
                 u"Known by its certificate "_s + inShortOf(knownBy(kvmAt(elsewhere(1)), "second").certificate));
    }

    void whatIsSoOfTheListAsAWholeIsSaidInWordsAndFollowsTheTicks()
    {
        WallOfOneTest own;
        Desk desk(own);
        // Nothing is said of a list where there is none.
        QCOMPARE(desk.view.counts(), QString());
        QVERIFY(desk.layOut(fileOf({ticked(1, u"One"_s), ticked(2, u"Two"_s), kvmAt(u"http://192.0.2.3"_s, u"Three"_s),
                                    notKeptHere(ticked(4, u"Four"_s)),
                                    saying(kvmAt(u"https://127.0.0.1:20443"_s, u"Five"_s), u"system"_s)})));
        // How many devices the file has, how many are ticked, how many are not ticked and want
        // a look, how many cannot be taken: in words, for what stands above the list at all
        // times, beside the number on the button.
        QCOMPARE(desk.view.counts(),
                 u"5 devices of this file are in this list. 2 are ticked. 2 are not ticked and want a look. 1 "
                 "cannot be taken."_s);
        QCOMPARE(desk.view.offered()->toLookAtCount(), 2);
        QCOMPARE(desk.view.offered()->cannotBeTakenCount(), 1);
        // It follows every tick, and whoever shows it is told.
        QVERIFY(desk.settledForItsMoment());
        const QSignalSpy tickedChanged(&desk.view, &OwnFilesView::tickedChanged);
        desk.view.setTicked(3, true);
        QCOMPARE(tickedChanged.count(), 1);
        QCOMPARE(desk.view.counts(),
                 u"5 devices of this file are in this list. 3 are ticked. 1 is not ticked and wants a look. 1 "
                 "cannot be taken."_s);
        desk.view.setTicked(5, true);
        QCOMPARE(desk.view.counts(), u"5 devices of this file are in this list. 4 are ticked. 1 cannot be taken."_s);
        // One whose tick somebody took away wants no look: nothing is wrong with it.
        desk.view.setTicked(1, false);
        QCOMPARE(desk.view.counts(), u"5 devices of this file are in this list. 3 are ticked. 1 cannot be taken."_s);
        desk.view.setTicked(2, false);
        desk.view.setTicked(3, false);
        desk.view.setTicked(5, false);
        QCOMPARE(desk.view.counts(),
                 u"5 devices of this file are in this list. None is ticked. 2 are not ticked and want a look. 1 "
                 "cannot be taken."_s);
        desk.view.setTicked(2, true);
        QCOMPARE(desk.view.counts(),
                 u"5 devices of this file are in this list. 1 is ticked. 2 are not ticked and want a look. 1 "
                 "cannot be taken."_s);
        QCOMPARE(tickedChanged.count(), 7);
        // And every form of it that may take the most room, whatever is ticked, for the room it
        // keeps (of how many are ticked, none, one and the most there can be, four; of how many
        // want a look, none, one and two; each with each): what the screens measure.
        // (theRoomOverTheListIsMadeForEveryFormItsCountsCanTake says more.)
        const QStringList forms = desk.view.countsToMakeRoomFor();
        QCOMPARE(forms.size(), 9);
        QVERIFY(forms.contains(u"5 devices of this file are in this list. None is ticked. 1 is not ticked and wants a "
                               "look. 1 cannot be taken."_s));
        QVERIFY(forms.contains(u"5 devices of this file are in this list. 4 are ticked. 2 are not ticked and want a "
                               "look. 1 cannot be taken."_s));
        QVERIFY(forms.contains(u"5 devices of this file are in this list. 1 is ticked. 1 cannot be taken."_s));
        // The list gone, nothing is said of it; and of a file of one, and of none, what is so.
        desk.view.reset();
        QCOMPARE(desk.view.counts(), QString());
        QCOMPARE(tickedChanged.count(), 8);
        QVERIFY(desk.layOut(fileOf({ticked(1, u"One"_s)})));
        QCOMPARE(desk.view.counts(), u"1 device of this file is in this list. 1 is ticked."_s);
        desk.view.reset();
        QVERIFY(desk.layOut(core::testing::plainFile({{u"devices"_s, QJsonArray()}}), QString()));
        QCOMPARE(desk.view.counts(), u"No device of this file is in this list."_s);
        QVERIFY(own.wall.kvms().isEmpty());
    }

    void whatTheCountSaysAgreesWithTheNotesThatNumberTheFilesDevices()
    {
        WallOfOneTest own;
        Desk desk(own);
        // Twenty devices of the file are left out, each with a note that numbers it as the file
        // does; the twenty-first and the twenty-second are in the list. What is said above it
        // counts the devices that are in this list as devices of this file, and does not say
        // that the file has two.
        QVERIFY(desk.layOut(fileWithSoManyNotes(20, {ticked(1, u"One"_s), ticked(2, u"Two"_s)}), QString()));
        QCOMPARE(desk.view.offered()->besideCount(), 20);
        // (Whole: the file is a plain one, and has no password for either.)
        QCOMPARE(desk.view.counts(), u"2 devices of this file are in this list. 2 are ticked. The file has no password "
                                     "for any of them."_s);
        // So of a file of one, and of one whose list holds none.
        QVERIFY(desk.layOut(fileOf({ticked(1, u"One"_s)})));
        QCOMPARE(desk.view.counts(), u"1 device of this file is in this list. 1 is ticked."_s);
        QVERIFY(desk.layOut(fileWithSoManyNotes(2, {}), QString()));
        QCOMPARE(desk.view.counts(), u"No device of this file is in this list."_s);
    }

    void noPasswordInTheFileIsSaidOnceWhereItIsSoOfEveryDevice()
    {
        WallOfOneTest own;
        Desk desk(own);
        const QString none = u"No password in the file"_s;
        // A plain file has no password for any of its devices: that is said once, above the
        // list, and on no row.
        QVERIFY(desk.layOut(fileOf({ticked(1, u"One"_s), kvmAt(u"http://192.0.2.3"_s, u"Two"_s),
                                    knownBy(kvmAt(elsewhere(4), u"Three"_s), "first")},
                                   false),
                            QString()));
        QList<Row> rows = rowsOf(desk.view);
        QCOMPARE(rows.size(), 3);
        for (const Row& row : std::as_const(rows)) {
            QVERIFY2(!row.inShort.contains(none), qPrintable(row.inShort));
        }
        QCOMPARE(desk.view.counts(),
                 u"3 devices of this file are in this list. 2 are ticked. 1 is not ticked and wants "
                 "a look. The file has no password for any of them."_s);
        QCOMPARE(rows.at(0).inShort, u"Known by Windows' word"_s);
        QCOMPARE(rows.at(1).inShort, u"Plain HTTP: its password goes in the clear"_s);
        // (Behind its row, the whole sentence stands all the same.)
        QVERIFY(rows.at(0).says.contains(
            u"The file has no password for it. It is taken without one, and its password is to be typed on this PC."_s));
        // A sealed file that has the password of each but one: on that one's row, and above the
        // list nothing of it.
        QVERIFY(desk.layOut(fileOf({ticked(1, u"One"_s), kvmAt(elsewhere(2), u"Two"_s, QString()),
                                    saying(kvmAt(elsewhere(3), u"Three"_s), u"system"_s)})));
        QVERIFY2(!desk.view.counts().contains(u"password"_s), qPrintable(desk.view.counts()));
        rows = rowsOf(desk.view);
        QCOMPARE(rows.at(0).inShort, u"Known by Windows' word"_s);
        QCOMPARE(rows.at(1).inShort, u"Known by Windows' word, not by what it shows first"_s + dot() + none);
        QCOMPARE(rows.at(2).inShort, u"Known by Windows' word"_s);
        // And one whose devices that can be taken all come with theirs: nothing of it anywhere.
        // (One that cannot be taken says nothing of its password.)
        QVERIFY(desk.layOut(fileOf({ticked(1, u"One"_s), notKeptHere(kvmAt(elsewhere(5), u"Small"_s, QString()))})));
        QVERIFY2(!desk.view.counts().contains(u"password"_s), qPrintable(desk.view.counts()));
        rows = rowsOf(desk.view);
        QCOMPARE(rows.at(0).inShort, u"Known by Windows' word"_s);
        QCOMPARE(rows.at(1).inShort, u"Cannot be taken: this PC's wall does not keep it as the file has it"_s);
        QVERIFY(own.wall.kvms().isEmpty());
    }

    void aDeviceWhoseAddressCannotBeShownIsCalledByItsPlaceInTheFile()
    {
        // The core hands no name and no address for a device whose address cannot be shown as
        // it would be kept: its row says which device of the file it is, as the notes do.
        LaidOutList list;
        WallExchange::Offered device;
        device.place = 7;
        device.canBeTaken = false;
        device.whyNot = u"invalid"_s;
        device.verdict = u"It cannot be taken: its address is not one this program can show as it would be kept."_s;
        WallExchange::Offered beside = device;
        beside.place = 8;
        beside.nameAndAddress = u"\"Eight\" at https://192.0.2.8"_s;
        list.show({}, {device, beside});
        QCOMPARE(list.index(0, 0).data(LaidOutList::NameAndAddressRole).toString(), u"Device 7 of the file"_s);
        QCOMPARE(list.nameAndAddressOf(7), u"Device 7 of the file"_s);
        QCOMPARE(list.index(1, 0).data(LaidOutList::NameAndAddressRole).toString(),
                 u"\"Eight\" at https://192.0.2.8"_s);
        QCOMPARE(list.index(0, 0).data(LaidOutList::InShortRole).toString(),
                 u"Cannot be taken: this PC's wall does not keep it as the file has it"_s);
    }

    void aTwinByItsNumberIsSaidToSayItIsTheSameDevice()
    {
        // From the reading of the twins: one that is another by the certificate it has the key to is that one
        // (proven); one that gives another's number for itself only says so, as the web says it ("This says it is
        // the same device as ..."). The few words of its row say which.
        LaidOutList list;
        WallExchange::Offered byCertificate;
        byCertificate.place = 1;
        byCertificate.nameAndAddress = u"\"One\" at https://192.0.2.1"_s;
        byCertificate.canBeTaken = false;
        byCertificate.whyNot = u"same_device"_s;
        WallExchange::Offered byNumber = byCertificate;
        byNumber.place = 2;
        byNumber.nameAndAddress = u"\"Two\" at http://192.0.2.2"_s;
        byNumber.saysItIsTheSame = true;
        list.show({}, {byCertificate, byNumber});
        QCOMPARE(list.index(0, 0).data(LaidOutList::InShortRole).toString(),
                 u"Cannot be taken: the same certificate as another, on the wall or earlier in this file"_s);
        QCOMPARE(list.index(1, 0).data(LaidOutList::InShortRole).toString(),
                 u"Cannot be taken: says it is the same device as another, on the wall or earlier in this file"_s);
    }

    void aTickWhereNoneWasBeginsTheMomentAnew()
    {
        WallOfOneTest own;
        Desk desk(own);
        // A file whose devices all want a look (their passwords go in the clear): none is ticked
        // to begin with, and there is nothing to take, however long the list is looked at.
        QVERIFY(
            desk.layOut(fileOf({kvmAt(u"http://192.0.2.3"_s, u"One"_s), kvmAt(u"http://192.0.2.4"_s, u"Two"_s)}, false),
                        QString()));
        QCOMPARE(desk.view.tickedCount(), 0);
        desk.view.setLookedAt(true);
        QTest::qWait(Desk::pastAQuickRest);
        desk.view.take();
        QVERIFY(own.wall.kvms().isEmpty());
        // The first tick, more than a moment later: what takes has just become something that
        // takes, and is not to be had by a click that was on its way meanwhile. The moment
        // begins anew, as it does when the notes are opened. (A long one here, to look in.)
        desk.view.restFor(Desk::longRest, Desk::longRest);
        const QSignalSpy changed(&desk.view, &OwnFilesView::changed);
        QElapsedTimer since;
        since.start();
        QVERIFY(desk.view.isSettled());
        desk.view.setTicked(1, true);
        QCOMPARE(desk.view.tickedCount(), 1);
        desk.view.take();
        QVERIFY2(own.wall.kvms().isEmpty(), "the first tick was taken at once, with no moment after it");
        QVERIFY(!desk.view.mayTake());
        QVERIFY(changed.count() >= 1);
        // (The list's own moment is not begun anew by a tick: nothing of the list has moved, and
        // the other ticks take a click as before.)
        QVERIFY(desk.view.isSettled());
        QTest::qWait(Desk::longRest / 6);
        QVERIFY2(!desk.view.mayTake(), qPrintable(QString::number(since.elapsed())));
        desk.view.take();
        QVERIFY(own.wall.kvms().isEmpty());
        QCOMPARE(desk.taken.count(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(desk.view.mayTake(), 20000);
        // (All of the moment, but for what a timer of Windows' may be early by: a twentieth, and
        // here a tenth is let pass.)
        QVERIFY2(since.elapsed() >= Desk::longRest * 9 / 10, qPrintable(QString::number(since.elapsed())));
        // Another tick beside one there is begins nothing anew; the last one taken away leaves
        // nothing to take; and a tick where none is, again, begins the moment anew.
        desk.view.setTicked(2, true);
        QVERIFY(desk.view.mayTake());
        desk.view.setTicked(1, false);
        desk.view.setTicked(2, false);
        // (Nothing ticked: nothing to take, and the button that takes says so by taking no click.)
        QVERIFY(!desk.view.mayTake());
        desk.view.take();
        QVERIFY(own.wall.kvms().isEmpty());
        desk.view.restFor(Desk::quickRest, Desk::quickRest);
        desk.view.setTicked(2, true);
        QVERIFY(!desk.view.mayTake());
        QTRY_VERIFY_WITH_TIMEOUT(desk.view.mayTake(), 20000);
        desk.view.take();
        QCOMPARE(own.names(), QStringList{u"Two"_s});
    }

    void theLinesBesideTheDevicesAreFoldedAndNothingIsTakenBeforeTheyWereOpened()
    {
        WallOfOneTest own;
        Desk desk(own);
        const QAbstractItemModelTester held(desk.view.offered(),
                                            QAbstractItemModelTester::FailureReportingMode::QtTest);
        // A file made by anybody: twenty devices that the reader leaves out, each with a
        // sentence, before the two that it takes. One row stands for all twenty sentences, and
        // the devices follow at once.
        QVERIFY2(desk.layOut(fileWithSoManyNotes(20, {ticked(1, u"One"_s), ticked(2, u"Two"_s)}), QString()),
                 qPrintable(desk.view.problem()));
        QCOMPARE(desk.view.offered()->besideCount(), 20);
        QCOMPARE(desk.view.foldedNotes(), 20);
        QCOMPARE(kindsOf(desk.view), (QStringList{u"fold"_s, u"device"_s, u"device"_s}));
        QList<Row> rows = rowsOf(desk.view);
        QCOMPARE(rows.at(0).text, u"20 notes about this file"_s);
        QCOMPARE(rows.at(1).place, 21);
        QCOMPARE(rows.at(1).nameAndAddress, u"\"One\" at https://192.0.2.1"_s);
        QCOMPARE(desk.view.tickedCount(), 2);
        // Looked at for its moment, and for several: nothing is to be taken, and nothing is,
        // while lines are folded away that nobody opened.
        desk.view.setLookedAt(true);
        QTest::qWait(Desk::pastAQuickRest);
        QVERIFY(!desk.view.mayTake());
        desk.view.take();
        QVERIFY(own.wall.kvms().isEmpty());
        QCOMPARE(desk.taken.count(), 0);
        QVERIFY(desk.view.isLaidOut());
        // A tick is as it was, and is set as ever, meanwhile.
        desk.view.setTicked(22, false);
        QCOMPARE(desk.view.tickedCount(), 1);

        // Opened: every one of them is a row, in the file's order, before the first device.
        // And what has just come on show is not passed over by a click: the moment begins anew.
        desk.view.restFor(Desk::longRest, Desk::longRest);
        const QSignalSpy changed(&desk.view, &OwnFilesView::changed);
        QVERIFY(desk.view.isSettled());
        desk.view.openNotes();
        QCOMPARE(changed.count(), 1);
        QCOMPARE(desk.view.foldedNotes(), 0);
        // (Every row has moved: the list's own moment begins anew too.)
        QVERIFY(!desk.view.isSettled());
        rows = rowsOf(desk.view);
        QCOMPARE(rows.size(), 22);
        for (int at = 0; at < 20; ++at) {
            QCOMPARE(rows.at(at).kind, u"beside"_s);
            QCOMPARE(rows.at(at).text, u"Device %1: it has no address"_s.arg(at + 1));
        }
        QCOMPARE(rows.at(20).kind, u"device"_s);
        QCOMPARE(rows.at(20).place, 21);
        QVERIFY(rows.at(20).ticked);
        QVERIFY(!rows.at(21).ticked);
        QVERIFY(!desk.view.mayTake());
        desk.view.take();
        QVERIFY(own.wall.kvms().isEmpty());
        // (Opened once: a second asking opens nothing, and begins nothing anew.)
        desk.view.openNotes();
        QCOMPARE(changed.count(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(desk.view.mayTake(), 20000);
        desk.view.take();
        QCOMPARE(own.names(), QStringList{u"One"_s});

        // However few they are: one sentence is folded too, and is to be opened. (In the smallest
        // window, one sentence of three lines would push the first device out of the first
        // screen: the screens' own test.)
        desk.view.restFor(Desk::quickRest, Desk::quickRest);
        QVERIFY(desk.layOut(fileWithSoManyNotes(1, {ticked(3, u"Three"_s)}), QString()));
        QCOMPARE(desk.view.foldedNotes(), 1);
        QCOMPARE(kindsOf(desk.view), (QStringList{u"fold"_s, u"device"_s}));
        QCOMPARE(rowsOf(desk.view).at(0).text, u"1 note about this file"_s);
        QVERIFY(desk.settledForItsMoment());
        desk.view.openNotes();
        QCOMPARE(kindsOf(desk.view), (QStringList{u"beside"_s, u"device"_s}));
        QVERIFY(desk.lookedAtForItsMoment());
        // A file with none has no row for them, and nothing to open.
        QVERIFY(desk.layOut(fileOf({ticked(4, u"Four"_s)})));
        QCOMPARE(desk.view.foldedNotes(), 0);
        QCOMPARE(kindsOf(desk.view), QStringList{u"device"_s});
        QVERIFY(desk.lookedAtForItsMoment());
        // And a list that comes after one that was opened is folded as any.
        QVERIFY(desk.layOut(fileWithSoManyNotes(5, {ticked(5, u"Five"_s)}), QString()));
        QCOMPARE(desk.view.foldedNotes(), 5);
        QVERIFY(desk.settledForItsMoment());
        desk.view.openNotes();
        QCOMPARE(desk.view.foldedNotes(), 0);
        QVERIFY(desk.layOut(fileWithSoManyNotes(2, {ticked(6, u"Six"_s)}), QString()));
        QCOMPARE(desk.view.foldedNotes(), 2);
    }

    void aRowIsOpenedAndClosedAndIsAsItWasLeft()
    {
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.layOut(fileOf({ticked(1, u"One"_s), ticked(2, u"Two"_s)})));
        const QSignalSpy dataChanged(desk.view.offered(), &QAbstractItemModel::dataChanged);
        // No row is opened to begin with. One is opened by its place in the file, and the list
        // says which, and what of it is another. (Each time once the list has stood for its
        // moment: an opened row moves those under it.)
        QVERIFY(desk.settledForItsMoment());
        desk.view.setOpen(2, true);
        QCOMPARE(dataChanged.count(), 1);
        QCOMPARE(dataChanged.at(0).at(0).toModelIndex().row(), 1);
        QCOMPARE(dataChanged.at(0).at(2).value<QList<int>>(), QList<int>{LaidOutList::OpenRole});
        QVERIFY(!rowsOf(desk.view).at(0).open);
        QVERIFY(rowsOf(desk.view).at(1).open);
        // Asked again, or of a place that is none, nothing is another. A tick does not close it.
        QVERIFY(desk.settledForItsMoment());
        desk.view.setOpen(2, true);
        desk.view.setOpen(7, true);
        desk.view.setOpen(0, true);
        QCOMPARE(dataChanged.count(), 1);
        desk.view.setTicked(2, false);
        QCOMPARE(tickedIn(desk.view), (QList<int>{1}));
        QVERIFY(rowsOf(desk.view).at(1).open);
        desk.view.setOpen(2, false);
        QVERIFY(!rowsOf(desk.view).at(1).open);
        // What is opened is no tick: nothing of it is taken for that, or left.
        QVERIFY(desk.settledForItsMoment());
        desk.view.setOpen(2, true);
        QCOMPARE(tickedIn(desk.view), (QList<int>{1}));
        // Another list has no row opened.
        QVERIFY(desk.layOut(fileOf({ticked(1, u"One"_s), ticked(2, u"Two"_s)})));
        QVERIFY(!rowsOf(desk.view).at(1).open);
    }

    void nothingOfTheListTakesAPressBeforeItHasStoodForItsMoment()
    {
        WallOfOneTest own;
        Desk desk(own);
        // A list with a note beside its devices, folded; one device that is ticked, one that is
        // not. (Their places in the file: 2 and 3.)
        QVERIFY(desk.layOut(fileWithSoManyNotes(1, {ticked(1, u"One"_s), kvmAt(u"http://192.0.2.3"_s, u"Two"_s)}),
                            QString()));
        const QSignalSpy dataChanged(desk.view.offered(), &QAbstractItemModel::dataChanged);
        // Every press the list has: a tick, a row's "Details", the row that opens the notes. By
        // the view's own calls, and by name, as the screens and whatever else speaks for a
        // person (the keyboard, a reader of the screen) call it: not one is taken until the list
        // has stood for its moment, whatever the screens show.
        const auto pressEverything = [&desk] {
            desk.view.setTicked(2, false);
            desk.view.setTicked(3, true);
            desk.view.setOpen(2, true);
            desk.view.openNotes();
            static_cast<void>(QMetaObject::invokeMethod(&desk.view, "setTicked", Q_ARG(int, 3), Q_ARG(bool, true)));
            static_cast<void>(QMetaObject::invokeMethod(&desk.view, "setOpen", Q_ARG(int, 3), Q_ARG(bool, true)));
            static_cast<void>(QMetaObject::invokeMethod(&desk.view, "openNotes"));
        };
        const auto asItCame = [&]() -> QString {
            if (tickedIn(desk.view) != QList<int>{2}) {
                return u"ticked: %1"_s.arg(desk.view.tickedCount());
            }
            if (desk.view.foldedNotes() != 1) {
                return u"the notes were opened"_s;
            }
            for (const Row& row : rowsOf(desk.view)) {
                if (row.open) {
                    return u"the row of device %1 was opened"_s.arg(row.place);
                }
            }
            return dataChanged.isEmpty() ? QString()
                                         : u"the list was told of a change %1 times"_s.arg(dataChanged.count());
        };
        // Not looked at yet.
        QVERIFY(!desk.view.isSettled());
        pressEverything();
        QString wrong = asItCame();
        QVERIFY2(wrong.isEmpty(), qPrintable(u"before the list was looked at: "_s + wrong));
        // Looked at, and in its first moment (a long one here, to look in).
        desk.view.restFor(Desk::longRest, Desk::longRest);
        QElapsedTimer since;
        since.start();
        desk.view.setLookedAt(true);
        pressEverything();
        QTest::qWait(Desk::longRest / 6);
        pressEverything();
        QVERIFY2(!desk.view.isSettled(), qPrintable(QString::number(since.elapsed())));
        wrong = asItCame();
        QVERIFY2(wrong.isEmpty(), qPrintable(u"in the list's first moment: "_s + wrong));
        // After its moment, each press is taken.
        QTRY_VERIFY_WITH_TIMEOUT(desk.view.isSettled(), 20000);
        desk.view.setTicked(3, true);
        QCOMPARE(tickedIn(desk.view), (QList<int>{2, 3}));
        desk.view.restFor(Desk::quickRest, Desk::quickRest);
        QVERIFY(QMetaObject::invokeMethod(&desk.view, "openNotes"));
        QCOMPARE(desk.view.foldedNotes(), 0);
        QVERIFY(desk.settledForItsMoment());
        QVERIFY(QMetaObject::invokeMethod(&desk.view, "setOpen", Q_ARG(int, 3), Q_ARG(bool, true)));
        QVERIFY(rowsOf(desk.view).at(2).open);
        QVERIFY(own.wall.kvms().isEmpty());
    }

    void aRowThatIsOpenedOrClosedBeginsTheMomentAnew()
    {
        // Opening or closing a row moves every row under it: whatever of the list stood there a
        // moment before has another under it now. So the list takes no press for its moment
        // after it, as after its notes were opened; and what takes, nothing either.
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.layOut(fileOf({ticked(1, u"One"_s), ticked(2, u"Two"_s), ticked(3, u"Three"_s)})));
        QVERIFY(desk.lookedAtForItsMoment());
        QVERIFY(desk.view.isSettled());
        desk.view.restFor(Desk::longRest, Desk::longRest);
        const QSignalSpy changed(&desk.view, &OwnFilesView::changed);
        QElapsedTimer since;
        since.start();
        desk.view.setOpen(1, true);
        QVERIFY(rowsOf(desk.view).at(0).open);
        QVERIFY2(!desk.view.isSettled(), "a row was opened, and the list took presses at once");
        QVERIFY2(!desk.view.mayTake(), "a row was opened, and what takes took at once");
        QVERIFY(changed.count() >= 1);
        desk.view.take();
        desk.view.setOpen(1, false);
        desk.view.setTicked(2, false);
        QTest::qWait(Desk::longRest / 6);
        QVERIFY2(!desk.view.mayTake(), qPrintable(QString::number(since.elapsed())));
        desk.view.take();
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY(rowsOf(desk.view).at(0).open);
        QCOMPARE(tickedIn(desk.view), (QList<int>{1, 2, 3}));
        QTRY_VERIFY_WITH_TIMEOUT(desk.view.mayTake(), 20000);
        // (All of the moment, but for what a timer of Windows' may be early by: a twentieth, and
        // here a tenth is let pass.)
        QVERIFY2(since.elapsed() >= Desk::longRest * 9 / 10, qPrintable(QString::number(since.elapsed())));
        QVERIFY(desk.view.isSettled());
        // Closed again: the same.
        since.restart();
        desk.view.setOpen(1, false);
        QVERIFY(!rowsOf(desk.view).at(0).open);
        QVERIFY(!desk.view.isSettled());
        QVERIFY(!desk.view.mayTake());
        QTRY_VERIFY_WITH_TIMEOUT(desk.view.mayTake(), 20000);
        QVERIFY2(since.elapsed() >= Desk::longRest * 9 / 10, qPrintable(QString::number(since.elapsed())));
        // And then it takes.
        desk.view.take();
        QCOMPARE(own.names(), (QStringList{u"One"_s, u"Two"_s, u"Three"_s}));
    }

    void aBoxThatMovesOrChangesSizeBeginsItsMomentsAnew()
    {
        // Where the box stands on the screen, and how large it is, as the screens say it: its
        // window changed size, moved, or came to a screen of another scale, and every control of
        // the box stands somewhere else under a pointer that rests where it rested. The list takes
        // no press for its moment after that, and what takes, and what saves, none either: by
        // the view's own calls, and by name, as whatever speaks for a person calls them.
        WallOfOneTest own;
        QVERIFY(!own.put(u"http://127.0.0.1:20001"_s, u"Build server"_s).isEmpty());
        Desk desk(own);
        const QRectF first(100, 100, 600, 500);
        // The place the box comes to when it opens is no change of its place.
        QVERIFY(desk.view.setProperty("boxPlace", first));
        QCOMPARE(desk.view.property("boxPlace").toRectF(), first);
        QVERIFY(desk.layOut(fileOf({ticked(1, u"One"_s), ticked(2, u"Two"_s)})));
        QVERIFY(desk.lookedAtForItsMoment());
        QVERIFY(desk.view.isSettled());
        // The same place again begins nothing anew.
        QVERIFY(desk.view.setProperty("boxPlace", first));
        QVERIFY(desk.view.isSettled() && desk.view.mayTake());

        desk.view.restFor(Desk::longRest, Desk::longRest);
        const auto pressEverything = [&desk] {
            desk.view.setTicked(1, false);
            desk.view.setOpen(2, true);
            desk.view.take();
            static_cast<void>(QMetaObject::invokeMethod(&desk.view, "setTicked", Q_ARG(int, 2), Q_ARG(bool, false)));
            static_cast<void>(QMetaObject::invokeMethod(&desk.view, "take"));
        };
        // Moved; and then as large as before, at another place; and of another size where it is.
        for (const QRectF& place : {QRectF(40, 60, 600, 500), QRectF(40, 60, 640, 500), QRectF(40, 60, 640, 440)}) {
            const QSignalSpy changed(&desk.view, &OwnFilesView::changed);
            QElapsedTimer since;
            since.start();
            QVERIFY(desk.view.setProperty("boxPlace", place));
            QVERIFY2(!desk.view.isSettled(), "the box moved, and its list took presses at once");
            QVERIFY2(!desk.view.mayTake(), "the box moved, and what takes took at once");
            QTRY_VERIFY(changed.count() >= 1);
            pressEverything();
            QTest::qWait(Desk::longRest / 6);
            pressEverything();
            QVERIFY2(!desk.view.mayTake(), qPrintable(QString::number(since.elapsed())));
            QCOMPARE(own.names(), QStringList{u"Build server"_s});
            QCOMPARE(tickedIn(desk.view), (QList<int>{1, 2}));
            QVERIFY(!rowsOf(desk.view).at(1).open);
            QTRY_VERIFY_WITH_TIMEOUT(desk.view.mayTake(), 20000);
            // (All of the moment, but for what a timer of Windows' may be early by: a twentieth,
            // and here a tenth is let pass.)
            QVERIFY2(since.elapsed() >= Desk::longRest * 9 / 10, qPrintable(QString::number(since.elapsed())));
            QVERIFY(desk.view.isSettled());
        }
        desk.view.take();
        QCOMPARE(own.names(), (QStringList{u"Build server"_s, u"One"_s, u"Two"_s}));

        // "Save" is held as well. (The box was closed and opened again: the place it comes to then
        // is a first one again.)
        desk.view.reset();
        desk.view.restFor(Desk::quickRest, Desk::quickRest);
        QVERIFY(desk.view.setProperty("boxPlace", first));
        QVERIFY(desk.make(QString()));
        QVERIFY(desk.onShowForItsMoment());
        desk.view.restFor(Desk::longRest, Desk::longRest);
        desk.savesTo = desk.folder.filePath(u"wall.json"_s);
        QElapsedTimer since;
        since.start();
        QVERIFY(desk.view.setProperty("boxPlace", QRectF(0, 0, 520, 400)));
        QVERIFY2(!desk.view.maySave(), "the box changed size, and what saves saved at once");
        desk.view.saveFile();
        static_cast<void>(QMetaObject::invokeMethod(&desk.view, "saveFile"));
        QVERIFY(desk.suggestions.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(desk.view.maySave(), 20000);
        QVERIFY2(since.elapsed() >= Desk::longRest * 9 / 10, qPrintable(QString::number(since.elapsed())));
        desk.view.saveFile();
        QCOMPARE(desk.suggestions.size(), 1);
        QCOMPARE(desk.exported.count(), 1);
    }

    void rowsThatTheKeysMoveHoldTheListsControlsForTheirMoment()
    {
        // The keys scroll the list a step, a page, or to an end at once, and the rows under a
        // pointer that rests where it rested are others: what is in the list takes no press for
        // its moment after that. What takes stands where it stood, outside the list, and is not
        // held for it.
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.layOut(fileOf({ticked(1, u"One"_s), ticked(2, u"Two"_s), ticked(3, u"Three"_s)})));
        QVERIFY(desk.lookedAtForItsMoment());
        QVERIFY(desk.view.isSettled());
        desk.view.restFor(Desk::longRest, Desk::longRest);
        QElapsedTimer since;
        since.start();
        QVERIFY(QMetaObject::invokeMethod(&desk.view, "rowsMoved"));
        QVERIFY2(!desk.view.isSettled(), "the keys moved the rows, and the list took presses at once");
        QVERIFY(desk.view.mayTake());
        desk.view.setTicked(1, false);
        desk.view.setOpen(2, true);
        QTest::qWait(Desk::longRest / 6);
        QVERIFY(!desk.view.isSettled());
        QCOMPARE(tickedIn(desk.view), (QList<int>{1, 2, 3}));
        QVERIFY(!rowsOf(desk.view).at(1).open);
        QTRY_VERIFY_WITH_TIMEOUT(desk.view.isSettled(), 20000);
        QVERIFY2(since.elapsed() >= Desk::longRest * 9 / 10, qPrintable(QString::number(since.elapsed())));
        desk.view.setTicked(1, false);
        QCOMPARE(tickedIn(desk.view), (QList<int>{2, 3}));
        // Without a list there is nothing to hold.
        desk.view.reset();
        QVERIFY(QMetaObject::invokeMethod(&desk.view, "rowsMoved"));
        QVERIFY(!desk.view.isSettled());
        QVERIFY(own.wall.kvms().isEmpty());
    }

    void theRoomOverTheListIsMadeForEveryFormItsCountsCanTake()
    {
        // What stands over the list keeps the room of the highest that it can say, as the screens
        // draw it: they are handed every form that may be the widest, and measure each. Of how
        // many are ticked: none, one, and the widest of the numbers it can come to; of how many
        // want a look: none, one, and the widest number there; each with each. A number is as
        // wide as the screens draw its digits; told nothing of that, the largest is taken.
        WallOfOneTest own;
        Desk desk(own);
        QList<Device> devices;
        for (int number = 1; number <= 100; ++number) {
            devices.append(
                saying(kvmAt(u"https://192.0.2.1:%1"_s.arg(1024 + number), u"KVM %1"_s.arg(number)), u"system"_s));
        }
        for (int number = 1; number <= 3; ++number) {
            devices.append(kvmAt(u"http://192.0.2.%1:8080"_s.arg(200 + number), u"Plain %1"_s.arg(number)));
        }
        devices.append(notKeptHere(ticked(150, u"Small"_s)));
        QVERIFY(desk.layOut(fileOf(devices)));
        const QString devicesPart = u"104 devices of this file are in this list. "_s;
        const QString cannotPart = u" 1 cannot be taken."_s;
        const auto made = [&](const QStringList& ticked, const QStringList& toLookAt) {
            QStringList forms;
            for (const QString& one : ticked) {
                for (const QString& other : toLookAt) {
                    forms.append(devicesPart + one + other + cannotPart);
                }
            }
            return forms;
        };
        const QVariant forms = desk.view.property("countsToMakeRoomFor");
        QVERIFY2(forms.isValid(), "the view hands the screens no forms of its counts to make room for");
        // (All that can be taken can be ticked: the most that can be ticked is 103.)
        QCOMPARE(forms.toStringList(),
                 made({u"None is ticked."_s, u"1 is ticked."_s, u"103 are ticked."_s},
                      {QString(), u" 1 is not ticked and wants a look."_s, u" 3 are not ticked and want a look."_s}));
        // Where eights are drawn widest and ones narrowest, of the numbers from 2 to 103 the widest
        // is 88; and of 2 and 3, where a 3 is drawn the wider, 3. Every form follows, and the
        // screens are told.
        const QSignalSpy told(&desk.view, SIGNAL(countsRoomChanged()));
        QVERIFY(desk.view.setProperty(
            "digitWidths", QVariant::fromValue(QList<qreal>{6.0, 3.0, 5.0, 7.0, 7.0, 7.0, 7.0, 7.0, 10.0, 9.0})));
        QCOMPARE(told.count(), 1);
        QCOMPARE(desk.view.property("countsToMakeRoomFor").toStringList(),
                 made({u"None is ticked."_s, u"1 is ticked."_s, u"88 are ticked."_s},
                      {QString(), u" 1 is not ticked and wants a look."_s, u" 3 are not ticked and want a look."_s}));
        // Every count it says, whatever is ticked, is one of them, but for its numbers.
        QVERIFY(desk.settledForItsMoment());
        desk.view.setTicked(101, true);
        desk.view.setTicked(7, false);
        QCOMPARE(desk.view.counts(), devicesPart + u"100 are ticked. 2 are not ticked and want a look."_s + cannotPart);
        // A list of one; of none; and none.
        QVERIFY(desk.layOut(fileOf({ticked(1, u"One"_s)})));
        QCOMPARE(desk.view.property("countsToMakeRoomFor").toStringList(),
                 (QStringList{u"1 device of this file is in this list. None is ticked."_s,
                              u"1 device of this file is in this list. 1 is ticked."_s}));
        QVERIFY(desk.layOut(core::testing::plainFile({{u"devices"_s, QJsonArray()}}), QString()));
        QCOMPARE(desk.view.property("countsToMakeRoomFor").toStringList(),
                 QStringList{u"No device of this file is in this list."_s});
        desk.view.reset();
        QCOMPARE(desk.view.property("countsToMakeRoomFor").toStringList(), QStringList());
        QVERIFY(own.wall.kvms().isEmpty());
    }

    // --- import: the taking -----------------------------------------------------------------------

    void aTakingThatTookAllHoldsWhatTheWindowShowsUnderItsBoxForAMoment()
    {
        // Where every device that was ticked is on the wall the box closes, and what the window
        // shows under it is under the pointer that pressed "Take": a click that follows it (the
        // second of a double click) lands there. Its window is told to let nothing there take a
        // press for a moment. Not after a taking where one was not taken: the box stays then.
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.layOut(fileOf({ticked(1, u"One"_s), ticked(2, u"Two"_s)})));
        QVERIFY(desk.lookedAtForItsMoment());
        const QVariant heldAtFirst = desk.view.property("heldAfterTaking");
        QVERIFY2(heldAtFirst.isValid(), "the view tells its window nothing of what is under its box after a taking");
        QVERIFY(!heldAtFirst.toBool());
        desk.view.restFor(Desk::longRest, Desk::longRest);
        QElapsedTimer since;
        since.start();
        desk.view.take();
        QCOMPARE(desk.taken.count(), 1);
        QVERIFY(desk.taken.at(0).at(1).toBool());
        QVERIFY(desk.view.property("heldAfterTaking").toBool());
        // (The box closes, and its view forgets everything of the file: the hold goes on.)
        desk.view.reset();
        QVERIFY(desk.view.property("heldAfterTaking").toBool());
        QTest::qWait(Desk::longRest / 6);
        QVERIFY(desk.view.property("heldAfterTaking").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(!desk.view.property("heldAfterTaking").toBool(), 20000);
        QVERIFY2(since.elapsed() >= Desk::longRest * 9 / 10, qPrintable(QString::number(since.elapsed())));
        // One that was not taken: the box stays, and holds what is under it itself.
        desk.view.restFor(Desk::quickRest, Desk::quickRest);
        QVERIFY(desk.layOut(fileOf({ticked(3, u"Three"_s), ticked(4, u"Four"_s)})));
        QVERIFY(desk.lookedAtForItsMoment());
        QVERIFY(!own.put(elsewhere(4), u"By hand"_s).isEmpty());
        desk.view.take();
        QVERIFY(desk.view.hasTaken());
        QVERIFY(!desk.view.property("heldAfterTaking").toBool());
    }

    void takingTakesExactlyTheTickedOnes()
    {
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.layOut(fileOf({ticked(1, u"One"_s), ticked(2, u"Two"_s), kvmAt(u"http://192.0.2.3"_s, u"Three"_s),
                                    ticked(4, u"Four"_s), kvmAt(u"http://192.0.2.5"_s, u"Five"_s)})));
        QCOMPARE(tickedIn(desk.view), (QList<int>{1, 2, 4}));
        QCOMPARE(desk.exchange.passwordsHeld(), 5);
        // One that was ticked to begin with is not wanted, and one that was not is.
        QVERIFY(desk.settledForItsMoment());
        desk.view.setTicked(2, false);
        desk.view.setTicked(5, true);
        QCOMPARE(tickedIn(desk.view), (QList<int>{1, 4, 5}));
        QCOMPARE(desk.view.tickedCount(), 3);
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY(desk.lookedAtForItsMoment());
        const QSignalSpy changed(&own.wall, &core::LocalWall::changed);
        desk.view.take();
        // Those three are on the wall, in the file's order, and no other; written down once.
        QCOMPARE(own.names(), (QStringList{u"One"_s, u"Four"_s, u"Five"_s}));
        QCOMPARE(changed.count(), 1);
        // Each with the password its file had for it, kept with the secrets and nowhere else.
        for (const core::OwnKvm& kvm : own.wall.kvms()) {
            QCOMPARE(own.secrets.all.value(core::LocalWall::secretOf(kvm.id)),
                     core::testing::savedFor("secret", kvm.address));
            // Not asked what it is yet: nothing goes to it before that.
            QVERIFY(!own.wall.wasAskedWhatItIs(kvm.id));
        }
        // All that were ticked are on the wall: the box closes, and its window says how many.
        // (That each is asked what it is before a password goes to it stood above the list, and
        // is not said once more.)
        QCOMPARE(desk.taken.count(), 1);
        QCOMPARE(desk.taken.at(0).at(0).toString(), u"3 KVMs were taken onto this wall."_s);
        QCOMPARE(desk.taken.at(0).at(1).toBool(), true);
        // And the box keeps nothing: no list, no file, and the core no password.
        QVERIFY(!desk.view.isLaidOut());
        QVERIFY(!desk.view.hasTaken());
        QCOMPARE(desk.view.offered()->rowCount(), 0);
        QCOMPARE(desk.view.tickedCount(), 0);
        QCOMPARE(desk.view.fileName(), QString());
        QCOMPARE(desk.exchange.passwordsHeld(), 0);
    }

    void nothingIsTakenByTheClickThatReadTheFile()
    {
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.layOut(fileOf({ticked(1, u"One"_s)}, false), QString()));
        QCOMPARE(desk.view.tickedCount(), 1);
        // Nobody has said that the list is looked at: however long it is there (several of its
        // moments, which are short at this point), nothing is taken.
        QVERIFY(!desk.view.mayTake());
        desk.view.take();
        QTest::qWait(Desk::pastAQuickRest);
        QVERIFY(!desk.view.mayTake());
        desk.view.take();
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY(desk.view.isLaidOut());

        // Looked at: not in the first moment, which in the program is as long as the box that
        // changes a KVM holds back a yes to a certificate; and is long here, to look in.
        QCOMPARE(OwnFilesView::takeRestsMs, 1000);
        desk.view.restFor(Desk::longRest, Desk::longRest);
        QElapsedTimer since;
        since.start();
        desk.view.setLookedAt(true);
        desk.view.take();
        QTest::qWait(Desk::longRest / 6);
        QVERIFY2(!desk.view.mayTake(), qPrintable(QString::number(since.elapsed())));
        desk.view.take();
        QVERIFY(own.wall.kvms().isEmpty());
        QCOMPARE(desk.taken.count(), 0);
        // Looked away from and at again (another program was in front): from the beginning,
        // and all of it. (But for what a timer of Windows' may be early by: a twentieth, and
        // here a tenth is let pass. A moment that went on from where it was would be over a
        // sixth sooner.)
        desk.view.setLookedAt(false);
        desk.view.setLookedAt(true);
        since.restart();
        QTRY_VERIFY_WITH_TIMEOUT(desk.view.mayTake(), 20000);
        QVERIFY2(since.elapsed() >= Desk::longRest * 9 / 10, qPrintable(QString::number(since.elapsed())));
        QVERIFY(own.wall.kvms().isEmpty());
        // And then it takes.
        desk.view.take();
        QCOMPARE(own.names(), QStringList{u"One"_s});
        QCOMPARE(desk.taken.count(), 1);
        QCOMPARE(desk.taken.at(0).at(0).toString(), u"1 KVM was taken onto this wall."_s);
    }

    void whatCameOfEachIsShownAndTheBoxStaysWhereOneWasNotTaken()
    {
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.layOut(fileOf({ticked(1, u"One"_s), ticked(2, u"Two"_s), ticked(3, u"Three"_s)})));
        QCOMPARE(desk.view.tickedCount(), 3);
        QVERIFY(desk.lookedAtForItsMoment());
        // Since the file was laid out, somebody added the second by hand in another window; and
        // Windows will not keep the password of the third.
        QVERIFY(!own.put(elsewhere(2), u"By hand"_s).isEmpty());
        own.secrets.refusesTheKeep = own.secrets.keeps + 2;
        desk.view.take();
        QCOMPARE(own.names(), (QStringList{u"By hand"_s, u"One"_s}));
        // Not all of them: the box stays, says how many are on the wall, and each that is not
        // with what it is and the sentence it was not taken with.
        QCOMPARE(desk.taken.count(), 0);
        QVERIFY(desk.view.hasTaken());
        QCOMPARE(desk.view.tookSummary(), u"1 of 3 KVMs were taken onto this wall."_s);
        const QVariantList notTaken = desk.view.notTaken();
        QCOMPARE(notTaken.size(), 2);
        QCOMPARE(notTaken.at(0).toMap().value(u"device"_s).toString(), u"\"Two\" at https://192.0.2.2"_s);
        QCOMPARE(notTaken.at(0).toMap().value(u"why"_s).toString(),
                 u"It cannot be taken: it is on the wall already (\"By hand\" at https://192.0.2.2)."_s);
        QCOMPARE(notTaken.at(1).toMap().value(u"device"_s).toString(), u"\"Three\" at https://192.0.2.3"_s);
        QVERIFY2(!notTaken.at(1).toMap().value(u"why"_s).toString().isEmpty(), "the third has no sentence");
        QVERIFY(!notTaken.at(1).toMap().value(u"why"_s).toString().contains(u"secret"_s));
        // The laying-out is over: there is no list, nothing to tick, nothing to take, no other
        // file to choose. Only the way out is left.
        QVERIFY(!desk.view.isLaidOut());
        QCOMPARE(desk.view.offered()->rowCount(), 0);
        QCOMPARE(desk.exchange.passwordsHeld(), 0);
        const int openedBefore = desk.opened;
        desk.view.chooseFile();
        desk.view.readFile(thePassphrase());
        desk.view.setTicked(2, true);
        desk.view.take();
        QCOMPARE(desk.opened, openedBefore);
        QVERIFY(desk.view.hasTaken());
        QCOMPARE(desk.view.notTaken().size(), 2);
        QCOMPARE(own.wall.kvms().size(), 2);
        // Closed, the box says nothing of it any more.
        desk.view.reset();
        QVERIFY(!desk.view.hasTaken());
        QCOMPARE(desk.view.tookSummary(), QString());
        QVERIFY(desk.view.notTaken().isEmpty());
    }

    void whereNoneWasTakenThatIsSaid()
    {
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.layOut(fileOf({ticked(1, u"One"_s)})));
        QVERIFY(desk.lookedAtForItsMoment());
        QVERIFY(!own.put(elsewhere(1), u"By hand"_s).isEmpty());
        desk.view.take();
        QVERIFY(desk.view.hasTaken());
        QCOMPARE(desk.view.tookSummary(), u"The KVM was not taken onto this wall."_s);
        QCOMPARE(desk.view.notTaken().size(), 1);
        QCOMPARE(desk.taken.count(), 0);
    }

    void whatCameOfATakingIsToldInTheCornerWhenItsBoxIsClosed()
    {
        // What came of a taking stands in the box until it is closed; and closed, by whatever
        // way, nothing of it is lost: the window's corner says what was taken, as something to
        // be looked at.
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.layOut(fileOf({ticked(1, u"One"_s), ticked(2, u"Two"_s)})));
        QVERIFY(desk.lookedAtForItsMoment());
        QVERIFY(!own.put(elsewhere(2), u"By hand"_s).isEmpty());
        desk.view.take();
        QVERIFY(desk.view.hasTaken());
        QCOMPARE(desk.taken.count(), 0);
        // Closed by its own button (the screens close the box, which is closed so here).
        desk.view.reset();
        QCOMPARE(desk.taken.count(), 1);
        QCOMPARE(desk.taken.at(0).at(0).toString(), u"1 of 2 KVMs were taken onto this wall."_s);
        QCOMPARE(desk.taken.at(0).at(1).toBool(), false);
        QVERIFY(!desk.view.hasTaken());
        // Once: closed again, it says nothing more.
        desk.view.reset();
        QCOMPARE(desk.taken.count(), 1);
        // So where the program closes it from outside, with screens or without: once.
        QVERIFY(desk.layOut(fileOf({ticked(3, u"Three"_s)})));
        QVERIFY(desk.lookedAtForItsMoment());
        QVERIFY(!own.put(elsewhere(3), u"By hand too"_s).isEmpty());
        desk.view.take();
        QVERIFY(desk.view.hasTaken());
        desk.view.close();
        QCOMPARE(desk.closeAsked.count(), 1);
        QCOMPARE(desk.taken.count(), 2);
        QCOMPARE(desk.taken.at(1).at(0).toString(), u"The KVM was not taken onto this wall."_s);
        QCOMPARE(desk.taken.at(1).at(1).toBool(), false);
        // And a box closed with nothing of a taking in it says nothing.
        QVERIFY(desk.layOut(fileOf({ticked(4, u"Four"_s)})));
        desk.view.reset();
        QCOMPARE(desk.taken.count(), 2);
    }

    void aSecondTakingFromOneLayingOutTakesNothing()
    {
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.layOut(fileOf({ticked(1, u"One"_s), ticked(2, u"Two"_s)})));
        QVERIFY(desk.settledForItsMoment());
        desk.view.setTicked(2, false);
        QVERIFY(desk.lookedAtForItsMoment());
        const QSignalSpy changed(&desk.view, &OwnFilesView::changed);
        desk.view.take();
        QCOMPARE(own.names(), QStringList{u"One"_s});
        QCOMPARE(desk.taken.count(), 1);
        // (What the window shows under the box is held for a moment after a taking that took all,
        // and its end is said: waited for here, before what is said is counted.)
        QTRY_VERIFY_WITH_TIMEOUT(!desk.view.isHeldAfterTaking(), 20000);
        const qsizetype saidSoFar = changed.count();
        // The same press once more, and again after the moment a list is looked at for: there is
        // nothing to take from, and nothing is said of it. (The second of the file, which was
        // laid out and not ticked, is not taken by it either.)
        desk.view.take();
        desk.view.setTicked(2, true);
        desk.view.take();
        QTest::qWait(Desk::pastAQuickRest);
        desk.view.take();
        QCOMPARE(own.names(), QStringList{u"One"_s});
        QCOMPARE(desk.taken.count(), 1);
        QVERIFY(!desk.view.hasTaken());
        QCOMPARE(desk.view.tookSummary(), QString());
        QVERIFY(desk.view.notTaken().isEmpty());
        QCOMPARE(desk.view.problem(), QString());
        QCOMPARE(changed.count(), saidSoFar);
    }

    // --- import: what is kept, and for how long ---------------------------------------------------

    void whatWasLaidOutIsKeptForItsTimeAndThenTheBoxShowsNoListAndSaysSo()
    {
        // Ten minutes as the program has it, and a moment here.
        QCOMPARE(WallExchange::keptForMsAsItIs, 600'000);
        QCOMPARE(OwnFilesView::saidOfWhatIsKeptNoLonger(WallExchange::keptForMsAsItIs),
                 u"What is read from a file is kept for 10 minutes, and that time is over. Nothing of the file is "
                 "kept: choose it again."_s);
        WallOfOneTest own;
        // (Three seconds here: what is looked at while the list is still kept has seconds to
        // be looked at in.)
        Desk desk(own, 3000);
        const QByteArray file = fileOf({ticked(1, u"One"_s), ticked(2, u"Two"_s)});
        QVERIFY(desk.layOut(file));
        desk.view.setLookedAt(true);
        QCOMPARE(desk.view.note(), QString());
        QVERIFY(!desk.view.isKeptNoLonger());
        QCOMPARE(desk.exchange.passwordsHeld(), 2);
        QCOMPARE(desk.view.offered()->rowCount(), 2);
        const qsizetype anotherBefore = desk.fileIsAnother.count();
        // The time is over: the core has forgotten, and the box follows. No list, nothing of the
        // file, and a sentence that says what is so and what to do: choose the file again.
        // Nothing went wrong.
        QTRY_VERIFY_WITH_TIMEOUT(!desk.view.isLaidOut(), 20000);
        QVERIFY(desk.view.isKeptNoLonger());
        QCOMPARE(desk.exchange.passwordsHeld(), 0);
        QCOMPARE(desk.view.offered()->rowCount(), 0);
        QCOMPARE(desk.view.tickedCount(), 0);
        QCOMPARE(desk.view.counts(), QString());
        QCOMPARE(desk.view.fileName(), QString());
        QCOMPARE(desk.view.bytesHeld(), 0);
        QVERIFY(!desk.view.isSealed());
        QCOMPARE(desk.view.problem(), QString());
        QCOMPARE(desk.view.note(), u"What is read from a file is kept for 3 seconds, and that time is over. Nothing "
                                   "of the file is kept: choose it again."_s);
        // (And what was typed as its passphrase is not the next file's.)
        QCOMPARE(desk.fileIsAnother.count(), anotherBefore + 1);
        // Nothing is taken from what is not there, however long the box was looked at.
        QTest::qWait(Desk::pastAQuickRest);
        QVERIFY(!desk.view.mayTake());
        desk.view.take();
        QVERIFY(own.wall.kvms().isEmpty());
        // Nor read without the file being chosen again. Windows' box given up changes nothing.
        desk.view.readFile(thePassphrase());
        QCOMPARE(desk.view.problem(), u"Choose a file first."_s);
        desk.opens.clear();
        desk.view.chooseFile();
        QVERIFY(desk.view.isKeptNoLonger());
        // Chosen and read again, it is laid out again, and the sentence is gone.
        QVERIFY(desk.layOut(file));
        QVERIFY(!desk.view.isKeptNoLonger());
        QCOMPARE(desk.view.note(), QString());
        QCOMPARE(desk.view.offered()->rowCount(), 2);
        // Closed, the box says nothing of it any more.
        QTRY_VERIFY_WITH_TIMEOUT(!desk.view.isLaidOut(), 20000);
        QVERIFY(desk.view.isKeptNoLonger());
        desk.view.reset();
        QVERIFY(!desk.view.isKeptNoLonger());
        QCOMPARE(desk.view.note(), QString());
    }

    void aFileThatWasChosenAndNotReadIsKeptForTheSameTime()
    {
        WallOfOneTest own;
        // (Six seconds here, as what is read is kept for: what is looked at while the file is
        // still chosen has seconds to be looked at in, and so has a reading that is to end
        // within its time.)
        const int keptFor = 6000;
        Desk desk(own, keptFor);
        const QByteArray sealed = fileOf({ticked(1, u"One"_s)});
        // A sealed file is chosen, and a passphrase typed for it is not the one: it stays chosen,
        // its bytes with it, for another try.
        desk.choose(u"chosen.qkvm"_s, sealed);
        QCOMPARE(desk.view.bytesHeld(), sealed.size());
        desk.view.readFile(u"not the passphrase"_s);
        QTRY_VERIFY(!desk.view.isBusy());
        QCOMPARE(desk.view.fileName(), u"chosen.qkvm"_s);
        QCOMPARE(desk.view.bytesHeld(), sealed.size());
        const qsizetype anotherBefore = desk.fileIsAnother.count();
        // And nobody reads it. Its time counts from when it was chosen, as long as what is read
        // of a file is kept: then nothing of it is held any more, what was typed for it goes
        // out of its line, and the box says so as after that time. Nothing went wrong.
        QTRY_VERIFY_WITH_TIMEOUT(desk.view.bytesHeld() == 0, 20000);
        QCOMPARE(desk.view.fileName(), QString());
        QVERIFY(!desk.view.isSealed());
        QVERIFY(desk.view.isKeptNoLonger());
        QCOMPARE(desk.view.problem(), QString());
        QCOMPARE(desk.view.note(), u"What is read from a file is kept for 6 seconds, and that time is over. Nothing "
                                   "of the file is kept: choose it again."_s);
        QCOMPARE(desk.fileIsAnother.count(), anotherBefore + 1);
        QCOMPARE(desk.exchange.passwordsHeld(), 0);
        desk.view.readFile(thePassphrase());
        QCOMPARE(desk.view.problem(), u"Choose a file first."_s);
        QVERIFY(!desk.view.isBusy());
        // Chosen again, and read when two of its six seconds are over (the reading has four to
        // end in): it is laid out, and from then on its list has the time of its own, the
        // core's, which runs from the reading and which the time of the choosing does not cut
        // short. (Cut short, it would go four seconds after the reading.)
        desk.view.reset();
        desk.choose(u"again.qkvm"_s, sealed);
        QTest::qWait(2000);
        QVERIFY(desk.view.bytesHeld() > 0);
        QElapsedTimer sinceTheReading;
        sinceTheReading.start();
        desk.view.readFile(thePassphrase());
        QTRY_VERIFY_WITH_TIMEOUT(desk.view.isLaidOut(), 20000);
        QVERIFY(!desk.view.isKeptNoLonger());
        QTRY_VERIFY_WITH_TIMEOUT(!desk.view.isLaidOut(), 20000);
        QVERIFY2(sinceTheReading.elapsed() >= keptFor * 9 / 10, qPrintable(QString::number(sinceTheReading.elapsed())));
        QVERIFY(own.wall.kvms().isEmpty());
    }

    void aBoxThatIsClosedWhileItsFileIsReadLaysNothingOut()
    {
        WallOfOneTest own;
        HeldWork work;
        Desk desk(own, WallExchange::keptForMsAsItIs, work.sealing());
        desk.choose(u"wall.qkvm"_s, fileOf({ticked(1, u"One"_s)}));
        work.hold();
        desk.view.readFile(thePassphrase());
        QTRY_COMPARE(work.begun(), 1);
        QVERIFY(desk.view.isReading());
        QVERIFY(desk.view.holdsItsWindow());
        // "Cancel": the box is closed over it, and that comes back while the reading is still
        // going on (it is held here for as long as this test wants: whoever waited for it would
        // wait for ever). The reading itself is not ended: it runs on, unheard.
        QElapsedTimer closing;
        closing.start();
        desk.view.reset();
        QVERIFY2(closing.elapsed() < 3000, qPrintable(QString::number(closing.elapsed())));
        QVERIFY(!desk.view.isBusy());
        QVERIFY(!desk.view.holdsItsWindow());
        QVERIFY(desk.exchange.isBusy());
        QCOMPARE(desk.view.fileName(), QString());
        QCOMPARE(desk.view.bytesHeld(), 0);
        // What the reading comes to afterwards is nobody's: nothing is laid out behind anybody's
        // back, here or in the core, which keeps no password of that file for anybody to take.
        work.letGo();
        QTRY_VERIFY_WITH_TIMEOUT(!desk.exchange.isBusy(), 20000);
        QTest::qWait(100);
        QVERIFY(!desk.view.isLaidOut());
        QCOMPARE(desk.view.offered()->rowCount(), 0);
        QCOMPARE(desk.exchange.passwordsHeld(), 0);
        QCOMPARE(desk.fileWasRead.count(), 0);
        QCOMPARE(desk.view.problem(), QString());
        QCOMPARE(desk.view.note(), QString());
        desk.view.setLookedAt(true);
        QTest::qWait(Desk::pastAQuickRest);
        desk.view.take();
        QVERIFY(own.wall.kvms().isEmpty());
    }

    void windowsBoxGivenUpLeavesWhatIsLaidOutAsItIsAndAnotherFileTakesItsPlace()
    {
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.layOut(fileOf({ticked(1, u"One"_s), ticked(2, u"Two"_s)})));
        QVERIFY(desk.settledForItsMoment());
        desk.view.setTicked(2, false);
        QCOMPARE(desk.exchange.passwordsHeld(), 2);
        const QString name = desk.view.fileName();
        // "Choose a file" once more, and Windows' box given up: the list is as it was, with
        // its ticks.
        desk.opens.clear();
        desk.view.chooseFile();
        QVERIFY(desk.view.isLaidOut());
        QCOMPARE(desk.view.fileName(), name);
        QCOMPARE(tickedIn(desk.view), (QList<int>{1}));
        QCOMPARE(desk.exchange.passwordsHeld(), 2);
        QVERIFY(!desk.view.isBusy());
        // So where Windows' box did not come up, which is said.
        desk.doorIsStuck = true;
        desk.view.chooseFile();
        QCOMPARE(desk.view.problem(), u"Windows did not open its box for choosing a file."_s);
        QVERIFY(desk.view.isLaidOut());
        QCOMPARE(tickedIn(desk.view), (QList<int>{1}));
        desk.doorIsStuck = false;
        // Another file chosen: nothing of the one before, in the box or in the core.
        desk.choose(u"another.json"_s, fileOf({ticked(3, u"Three"_s)}, false));
        QCOMPARE(desk.view.fileName(), u"another.json"_s);
        QVERIFY(!desk.view.isLaidOut());
        QCOMPARE(desk.view.offered()->rowCount(), 0);
        QCOMPARE(desk.view.tickedCount(), 0);
        QCOMPARE(desk.exchange.passwordsHeld(), 0);
        QCOMPARE(desk.view.problem(), QString());
    }

    void nothingIsAskedFromBehindOneOfWindowsBoxes()
    {
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.layOut(fileOf({ticked(1, u"One"_s), ticked(2, u"Two"_s)})));
        QVERIFY(desk.lookedAtForItsMoment());
        // While Windows' box for another file stands over the window, whatever is asked of the
        // box under it is asked by nobody who sits there: nothing is ticked, taken, read or made.
        int openedBehind = -1;
        desk.opens.clear();
        desk.meanwhile = [&] {
            desk.view.setTicked(2, false);
            desk.view.take();
            desk.view.readFile(thePassphrase());
            desk.view.makeFile(QString());
            desk.view.chooseFile();
            openedBehind = desk.opened;
        };
        desk.view.chooseFile();
        QCOMPARE(openedBehind, 1 + 1);
        QVERIFY(own.wall.kvms().isEmpty());
        QCOMPARE(tickedIn(desk.view), (QList<int>{1, 2}));
        QVERIFY(desk.view.isLaidOut());
        QVERIFY(!desk.view.isBusy());
        QVERIFY(!desk.exchange.isBusy());
        QCOMPARE(desk.taken.count(), 0);
    }

    void aWindowThatLeavesThisPcsWallOrGoesKeepsNothingOfAFile()
    {
        WallOfOneTest own;
        auto desk = std::make_unique<Desk>(own);
        QVERIFY(desk->layOut(fileOf({ticked(1, u"One"_s), ticked(2, u"Two"_s)})));
        QVERIFY(desk->lookedAtForItsMoment());
        QCOMPARE(desk->exchange.passwordsHeld(), 2);
        // The window is left for a server's wall, or goes out of sight, while the list is
        // shown: its boxes are closed from outside. The screens are told to close them, and
        // whether or not there are screens, nothing is shown or kept from that moment, and the
        // core has forgotten the file.
        desk->view.close();
        QCOMPARE(desk->closeAsked.count(), 1);
        QVERIFY(!desk->view.isLaidOut());
        QCOMPARE(desk->view.offered()->rowCount(), 0);
        QCOMPARE(desk->view.fileName(), QString());
        QCOMPARE(desk->exchange.passwordsHeld(), 0);
        desk->view.take();
        QVERIFY(own.wall.kvms().isEmpty());

        // And a window that goes with a list in it: forgotten in the core too, which may be
        // there for longer than the window's view is.
        WallExchange outlives(own.wall, quickly());
        {
            Desk gone(own);
            gone.view.use(&outlives);
            QVERIFY(gone.layOut(fileOf({ticked(1, u"One"_s)})));
            QCOMPARE(outlives.passwordsHeld(), 1);
        }
        QCOMPARE(outlives.passwordsHeld(), 0);
        // (And the other way round: what makes the files goes first, and the box asks nothing.)
        auto shortLived = std::make_unique<WallExchange>(own.wall, quickly());
        Desk stays(own);
        stays.view.use(shortLived.get());
        shortLived.reset();
        stays.view.makeFile(QString());
        QCOMPARE(stays.view.problem(), u"Files of this PC's wall cannot be made or read in this window."_s);
        QVERIFY(!stays.view.isBusy());
        stays.view.reset();
    }

    void eachWindowHasAListOfItsOwn()
    {
        WallOfOneTest own;
        Desk first(own);
        Desk second(own);
        QVERIFY(first.layOut(fileOf({ticked(1, u"One"_s), ticked(2, u"Two"_s)})));
        QVERIFY(second.layOut(fileOf({ticked(3, u"Three"_s)}, false), QString()));
        // The second's reading took nothing from the first, and each shows its own.
        QVERIFY(first.view.isLaidOut());
        QCOMPARE(rowsOf(first.view).size(), 2);
        QCOMPARE(rowsOf(first.view).at(0).nameAndAddress, u"\"One\" at https://192.0.2.1"_s);
        QCOMPARE(rowsOf(second.view).size(), 1);
        QCOMPARE(rowsOf(second.view).at(0).nameAndAddress, u"\"Three\" at https://192.0.2.3"_s);
        // A tick in the one is none in the other.
        QVERIFY(first.settledForItsMoment());
        first.view.setTicked(1, false);
        QCOMPARE(tickedIn(first.view), (QList<int>{2}));
        QCOMPARE(tickedIn(second.view), (QList<int>{1}));
        QVERIFY(first.lookedAtForItsMoment());
        QVERIFY(second.lookedAtForItsMoment());
        // The second takes its own; the first's list is as it was, and is taken from afterwards.
        second.view.take();
        QCOMPARE(own.names(), QStringList{u"Three"_s});
        QCOMPARE(second.taken.count(), 1);
        QCOMPARE(first.taken.count(), 0);
        QVERIFY(first.view.isLaidOut());
        QCOMPARE(tickedIn(first.view), (QList<int>{2}));
        QCOMPARE(first.exchange.passwordsHeld(), 2);
        // The second is closed: the first's list stays.
        second.view.reset();
        QVERIFY(first.view.isLaidOut());
        first.view.take();
        QCOMPARE(own.names(), (QStringList{u"Three"_s, u"Two"_s}));
        QCOMPARE(first.taken.count(), 1);
    }

    void oneFileAtATimeIsWorkedOnAndThatIsSaidAsSomethingThatPasses()
    {
        WallOfOneTest own;
        QVERIFY(!own.put(u"http://127.0.0.1:20001"_s, u"Build server"_s).isEmpty());
        HeldWork work;
        Desk first(own, WallExchange::keptForMsAsItIs, work.sealing());
        Desk second(own);
        const QByteArray file = fileOf({ticked(1, u"One"_s)});
        // One window reads a file, which takes a while.
        work.hold();
        first.choose(u"wall.qkvm"_s, file);
        first.view.readFile(thePassphrase());
        QTRY_COMPARE(work.begun(), 1);
        // Another reads one meanwhile: it is told that another file is being worked on, which is
        // so and passes. Nothing went wrong, its file stays chosen, and nothing is waited for.
        second.choose(u"wall.qkvm"_s, file);
        second.view.readFile(thePassphrase());
        QTRY_VERIFY(!second.view.isBusy());
        QCOMPARE(second.view.note(), busyElsewhere);
        QCOMPARE(second.view.problem(), QString());
        QVERIFY(!second.view.isLaidOut());
        QCOMPARE(second.view.fileName(), u"wall.qkvm"_s);
        QCOMPARE(second.view.bytesHeld(), file.size());
        // So with a file that is to be made.
        second.view.makeFile(thePassphrase());
        QTRY_VERIFY(!second.view.isBusy());
        QCOMPARE(second.view.note(), busyElsewhere);
        QCOMPARE(second.view.problem(), QString());
        QVERIFY(!second.view.isMade());
        // It passes: the first is laid out, and the second, tried again, is too. What was said
        // is said no longer.
        work.letGo();
        QTRY_VERIFY(first.view.isLaidOut());
        second.view.readFile(thePassphrase());
        QCOMPARE(second.view.note(), QString());
        QTRY_VERIFY(second.view.isLaidOut());
        QCOMPARE(second.view.note(), QString());
        second.view.reset();
        QVERIFY(second.make(thePassphrase()));
        QVERIFY(first.view.isLaidOut());
    }

    void aBoxThatIsOpenedAnewOrClosedKeepsNothing()
    {
        WallOfOneTest own;
        QVERIFY(!own.put(u"http://127.0.0.1:20001"_s, u"Build server"_s).isEmpty());
        Desk desk(own);
        QVERIFY(desk.make(thePassphrase()));
        QVERIFY(desk.onShowForItsMoment());
        desk.view.reset();
        QVERIFY(!desk.view.isMade());
        QVERIFY(desk.view.notInIt().isEmpty());
        QVERIFY(!desk.view.maySave());
        desk.view.saveFile();
        QVERIFY(desk.suggestions.isEmpty());

        desk.choose(u"wall.qkvm"_s, fileOf({ticked(1, u"One"_s)}));
        desk.view.readFile(u"not it at all"_s);
        QTRY_VERIFY(!desk.view.isBusy());
        QVERIFY(!desk.view.problem().isEmpty());
        desk.view.reset();
        QCOMPARE(desk.view.problem(), QString());
        QCOMPARE(desk.view.fileName(), QString());
        QVERIFY(!desk.view.isSealed());
        QCOMPARE(desk.view.bytesHeld(), 0);
    }
};

QTEST_GUILESS_MAIN(TestOwnFilesView)
#include "test_own_files_view.moc"
