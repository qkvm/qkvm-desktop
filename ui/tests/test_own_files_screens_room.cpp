#include "own_files_screens_test.h"

using namespace qkvm;
using namespace qkvm::ui::testing::own_files_screens;

// The two boxes' room: what stands over the list at every width of the box, a window that changes size, the
// smallest size, and the list's first moment.
class TestOwnFilesScreensRoom : public OwnFilesScreensTest {
    Q_OBJECT

private slots:
    void init()
    {
        // Whatever Qt has to say of these screens while they are worked fails the test that
        // made it say so: a box that is opened, filled, scrolled and emptied does all of it
        // without a word of complaint.
        QTest::failOnWarning();
    }

    void cleanup()
    {
        // (One look for every test that does not say otherwise.)
        ui::Look::tellQt(Qt::ColorScheme::Dark);
    }

    void aWindowThatChangesSizeHoldsWhatTakesAndWhatSavesForTheirMoment_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::addColumn<QSize>("then");
        QTest::newRow("from 1280 by 800 to 900 by 600") << QSize(1280, 800) << QSize(900, 600);
        QTest::newRow("from the smallest window to 800 by 500") << QSize(640, 400) << QSize(800, 500);
    }

    void aWindowThatChangesSizeHoldsWhatTakesAndWhatSavesForTheirMoment()
    {
        // The box's size and place follow its window's: the window is snapped, made as large as
        // its screen, or comes to another screen, and the box stands somewhere else, larger or
        // smaller, with something else under a pointer that rests where it rested. Nothing of the
        // list, and nothing that takes or saves, takes a press in the moment after: by a click,
        // and by whatever else speaks for a person (the program's own calls).
        QFETCH(QSize, size);
        QFETCH(QSize, then);
        WallOfOneTest own;
        QVERIFY(!own.put(u"http://127.0.0.1:20001"_s, u"Build server"_s).isEmpty());
        Desk desk(own);
        desk.ownFiles.restFor(Desk::heldRest, Desk::heldRest);
        QVERIFY(desk.load(size.width(), size.height()) != nullptr);
        const QString of =
            u"%1x%2-to-%3x%4"_s.arg(size.width()).arg(size.height()).arg(then.width()).arg(then.height());
        const auto keep = [&](const QString& name) { keepPictureOf(desk.window, u"own-resize-%1-%2"_s.arg(of, name)); };
        const QString box = u"ownImportBox"_s;
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, box));
        // A list that has stood for its moment, with something ticked: its ticks, its rows'
        // "Details" and "Take" take a press.
        QVERIFY(chooseAndRead(
            desk,
            fileOf({ticked(1, u"One"_s), ticked(2, u"Two"_s), kvmAt(u"http://192.0.2.3:8080"_s, u"Three"_s)}, false)));
        const QQuickItem* take = theOne(desk.window, u"ownImportTake"_s);
        QVERIFY(take != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(take->isEnabled(), 20000);
        const QString takes = u"the box's own button (import)"_s;
        const QStringList held{u"ownImportTick of device 1"_s, u"ownImportTick of device 3"_s,
                               u"ownImportDetails of device 2"_s, takes};
        Seen before = seenOf(desk, box);
        for (const QString& control : held) {
            QVERIFY2(before.live.contains(control), qPrintable(control));
        }
        keep(u"import-before"_s);

        // The window changes size: the box with it. (Looked at once the window has drawn it and
        // what the program said of it meanwhile has come to the screens, a turn of the loop later:
        // the program says it so, and holds every press itself from the moment it was told.)
        QElapsedTimer since;
        since.start();
        desk.window->resize(then);
        QTRY_COMPARE(desk.window->size(), then);
        aFrame(desk.window);
        QCoreApplication::processEvents();
        Seen after = seenOf(desk, box);
        keep(u"import-after"_s);
        QVERIFY2(after.box != before.box, "the box did not change with its window");
        QStringList pressable;
        for (const QString& control : std::as_const(after.live)) {
            if (after.inTheList.contains(control) || control == takes) {
                pressable.append(control);
            }
        }
        QVERIFY2(pressable.isEmpty(),
                 qPrintable(u"the window changed size %1, and at once these took a press: %2"_s.arg(
                     of, pressable.join(u", "_s))));
        QVERIFY(!desk.ownFiles.isSettled());
        QVERIFY(!desk.ownFiles.mayTake());
        // (What stands where something else stood a moment before, and is not held: say so.)
        for (const QString& moved : movedBy(before, after, u"the window changed size "_s + of)) {
            qInfo("%s", qPrintable(moved));
        }
        // Pressed in that moment, each where it stands now, and by the program's own calls:
        // nothing of it is taken.
        for (const QString& control : held) {
            QTest::mouseClick(desk.window, Qt::LeftButton, {}, after.controls.value(control).center().toPoint());
        }
        desk.ownFiles.setTicked(1, false);
        desk.ownFiles.setOpen(2, true);
        desk.ownFiles.take();
        QTest::qWait(50);
        QVERIFY2(since.elapsed() < Desk::heldRest, qPrintable(QString::number(since.elapsed())));
        QCOMPARE(own.names(), QStringList{u"Build server"_s});
        QCOMPARE(desk.ownFiles.tickedCount(), 2);
        QVERIFY(!rowOf(desk.window, 2)->property("open").toBool());
        QVERIFY(desk.isOpen(box));
        QVERIFY(desk.ownFiles.isLaidOut());
        // After its moment, each takes a press again. (All of the moment, but for what a timer of
        // Windows' may be early by: a twentieth, and here a tenth is let pass.)
        QTRY_VERIFY_WITH_TIMEOUT(take->isEnabled(), 20000);
        QVERIFY2(since.elapsed() >= Desk::heldRest * 9 / 10, qPrintable(QString::number(since.elapsed())));
        QVERIFY(desk.ownFiles.isSettled());
        // The window moves, as large as it is (to another place on its screen, or to another
        // screen): the box is in its middle as before, and stands elsewhere under the pointer all
        // the same. The same moment.
        const QPoint was = desk.window->position();
        desk.window->setPosition(was + QPoint(37, 23));
        QTRY_COMPARE(desk.window->position(), was + QPoint(37, 23));
        aFrame(desk.window);
        QCoreApplication::processEvents();
        after = seenOf(desk, box);
        QVERIFY2(!after.live.contains(takes), "the window moved, and \"Take\" took a press at once");
        QVERIFY2(!after.live.contains(u"ownImportTick of device 1"_s),
                 "the window moved, and a tick took a press at once");
        QVERIFY(!desk.ownFiles.isSettled());
        QVERIFY(!desk.ownFiles.mayTake());
        QTRY_VERIFY_WITH_TIMEOUT(take->isEnabled(), 20000);
        clickOn(desk.window, take);
        QTRY_VERIFY(!desk.isOpen(box));
        QCOMPARE(own.names(), (QStringList{u"Build server"_s, u"One"_s, u"Two"_s}));

        // The box that writes the wall: a file that was made, with "Save" taking a press; and the
        // window goes back to its size.
        const QString exportBox = u"ownExportBox"_s;
        desk.savesTo = desk.folder.filePath(u"wall.json"_s);
        QVERIFY(openFromTheSettings(desk, u"settingsExport"_s, exportBox));
        clickOn(desk.window, theOne(desk.window, u"ownExportMake"_s));
        QTRY_VERIFY_WITH_TIMEOUT(theOne(desk.window, u"ownExportSave"_s) != nullptr, 20000);
        const QQuickItem* save = theOne(desk.window, u"ownExportSave"_s);
        QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(), 20000);
        before = seenOf(desk, exportBox);
        QVERIFY(before.live.contains(u"the box's own button (export)"_s));
        keep(u"export-before"_s);
        since.restart();
        desk.window->resize(size);
        QTRY_COMPARE(desk.window->size(), size);
        aFrame(desk.window);
        QCoreApplication::processEvents();
        after = seenOf(desk, exportBox);
        keep(u"export-after"_s);
        QVERIFY2(after.box != before.box, "the box did not change with its window");
        QVERIFY2(!after.live.contains(u"the box's own button (export)"_s),
                 qPrintable(u"the window changed size %1, and \"Save\" took a press at once"_s.arg(of)));
        QVERIFY(!desk.ownFiles.maySave());
        clickOn(desk.window, save);
        desk.ownFiles.saveFile();
        QTest::qWait(50);
        QVERIFY2(since.elapsed() < Desk::heldRest, qPrintable(QString::number(since.elapsed())));
        QVERIFY(desk.suggestions.isEmpty());
        QVERIFY(!QFile::exists(desk.savesTo));
        QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(), 20000);
        QVERIFY2(since.elapsed() >= Desk::heldRest * 9 / 10, qPrintable(QString::number(since.elapsed())));
        clickOn(desk.window, save);
        QTRY_VERIFY(!desk.isOpen(exportBox));
        QCOMPARE(desk.suggestions.size(), 1);
        QVERIFY(QFile::exists(desk.savesTo));
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void whatStandsOverTheListHasRoomForEveryCountItCanSayAtEveryWidthOfTheBox()
    {
        // What stands over the list keeps the room that the highest of what it can say takes as
        // it is drawn, at the width the box has: so no count needs more lines than it has room
        // for, and the sentence under it never runs under the list, whatever is ticked. Looked at
        // for every count each list can come to, at every width the box can have (from the
        // smallest window to one in which the box is at its widest), with lists where two forms
        // of a count that are as long by their letters are not as wide as they are drawn ("None
        // is ticked." and "100 are ticked."; "1 is not ticked and wants a look." and "3 are not
        // ticked and want a look.").
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.load(1280, 600) != nullptr);
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        // A line of text as the counts are drawn, to measure every count in, one after the other.
        QQmlComponent probeOf(&desk.engine);
        probeOf.setData("import QtQuick\nText { textFormat: Text.PlainText; wrapMode: Text.WordWrap; visible: false }",
                        QUrl());
        const std::unique_ptr<QQuickItem> probe(qobject_cast<QQuickItem*>(probeOf.create()));
        QVERIFY2(probe != nullptr, qPrintable(probeOf.errorString()));
        probe->setParentItem(desk.window->contentItem());
        struct OfAList {
            int ticked = 0;
            int toLookAt = 0;
            int cannot = 0;
            bool sealed = true;
        };
        QStringList wrong;
        for (const OfAList& kind : {OfAList{100, 3, 0, true}, OfAList{100, 3, 0, false}, OfAList{100, 3, 1, true},
                                    OfAList{100, 3, 1, false}}) {
            const QString called = u"%1 ticked, %2 to look at, %3 that cannot be taken, %4"_s.arg(kind.ticked)
                                       .arg(kind.toLookAt)
                                       .arg(kind.cannot)
                                       .arg(kind.sealed ? u"sealed"_s : u"plain"_s);
            QList<Device> devices;
            for (int number = 1; number <= kind.ticked; ++number) {
                devices.append(ticked(number, u"KVM %1"_s.arg(number)));
            }
            for (int number = 1; number <= kind.toLookAt; ++number) {
                devices.append(kvmAt(u"http://192.0.2.%1:8080"_s.arg(200 + number), u"Plain %1"_s.arg(number)));
            }
            for (int number = 1; number <= kind.cannot; ++number) {
                devices.append(notKeptHere(ticked(150 + number, u"Small %1"_s.arg(number))));
            }
            desk.window->resize(1280, 600);
            QVERIFY(chooseAndRead(desk, fileOf(devices, kind.sealed), kind.sealed ? thePassphrase() : QString()));
            // Every count this list can come to: of the devices ticked to begin with, any number
            // ticked, and of those that want a look, any number.
            ui::LaidOutList* listed = desk.ownFiles.offered();
            QCOMPARE(listed->deviceCount(), static_cast<int>(devices.size()));
            QStringList said;
            for (int looked = 0; looked <= kind.toLookAt; ++looked) {
                for (int number = 1; number <= kind.toLookAt; ++number) {
                    static_cast<void>(listed->setTicked(kind.ticked + number, number <= looked));
                }
                for (int number = 1; number <= kind.ticked; ++number) {
                    static_cast<void>(listed->setTicked(number, false));
                }
                said.append(listed->saidOfItsCounts());
                for (int number = 1; number <= kind.ticked; ++number) {
                    static_cast<void>(listed->setTicked(number, true));
                    said.append(listed->saidOfItsCounts());
                }
            }
            for (int number = 1; number <= kind.toLookAt; ++number) {
                static_cast<void>(listed->setTicked(kind.ticked + number, false));
            }
            QCOMPARE(said.size(), (kind.ticked + 1) * (kind.toLookAt + 1));
            for (int width = 640; width <= 724; ++width) {
                desk.window->resize(width, 600);
                QTRY_COMPARE(desk.window->width(), width);
                aFrame(desk.window);
                const QQuickItem* counts = theOne(desk.window, u"ownImportCounts"_s);
                const QQuickItem* nothingYet = theOne(desk.window, u"ownImportNothingYet"_s);
                const QQuickItem* above = theOne(desk.window, u"ownImportAbove"_s);
                QVERIFY(counts != nullptr && nothingYet != nullptr && above != nullptr);
                const QQuickItem* band = above->parentItem();
                QCOMPARE(desk.box(u"ownImportBox"_s)->property("width").toReal(), std::min(700.0, width - 24.0));
                probe->setProperty("font", counts->property("font"));
                probe->setWidth(counts->width());
                qreal highest = 0;
                QString highestSaid;
                for (const QString& one : std::as_const(said)) {
                    probe->setProperty("text", one);
                    const qreal high = probe->implicitHeight();
                    if (high > highest) {
                        highest = high;
                        highestSaid = one;
                    }
                }
                if (4 + highest + nothingYet->height() > band->height() + 0.5) {
                    wrong.append(u"%1, a box %2 wide: \"%3\" is %4 high, and over the list there is room for %5 with "
                                 "the sentence under it"_s.arg(called)
                                     .arg(std::min(700, width - 24))
                                     .arg(highestSaid)
                                     .arg(highest)
                                     .arg(band->height() - 4 - nothingYet->height()));
                }
                if (width == 640) {
                    keepPictureOf(desk.window, u"own-import-counts-narrowest-%1-%2-%3-%4"_s.arg(kind.ticked)
                                                   .arg(kind.toLookAt)
                                                   .arg(kind.cannot)
                                                   .arg(kind.sealed ? u"sealed"_s : u"plain"_s));
                }
            }
        }
        for (const QString& one : std::as_const(wrong)) {
            qInfo("%s", qPrintable(one));
        }
        QVERIFY2(wrong.isEmpty(), qPrintable(u"%1 times; the first: %2"_s.arg(wrong.size()).arg(wrong.value(0))));
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void whatComesWithTheListTakesNoClickInItsFirstMoment()
    {
        // A list comes where something else stood a moment before (the line for the passphrase,
        // the button that reads), and the notes, opened, move every row. So nothing of the list
        // takes a click in the moment it has come, nor in the moment after its notes were
        // opened: as "Take" does not.
        WallOfOneTest own;
        Desk desk(own);
        desk.ownFiles.restFor(Desk::longRest, Desk::longRest);
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        desk.opens =
            desk.write(u"plain.json"_s,
                       fileWithSoManyLeftOut(1, {kvmAt(u"http://192.0.2.3:8080"_s, u"One"_s), ticked(4, u"Two"_s)}));
        clickOn(desk.window, theOne(desk.window, u"ownImportChoose"_s));
        // (From the press that reads the file: the list's moment begins after it, when the list
        // has come, and what is looked at here is to have been within that moment.)
        QElapsedTimer since;
        since.start();
        clickOn(desk.window, theOne(desk.window, u"ownImportRead"_s));
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isLaidOut(), 20000);
        QTRY_VERIFY(rowOf(desk.window, 3) != nullptr);
        aFrame(desk.window);
        const QQuickItem* tick = inRow(rowOf(desk.window, 2), u"ownImportTick"_s);
        const QQuickItem* details = inRow(rowOf(desk.window, 2), u"ownImportDetails"_s);
        const QQuickItem* more = theOne(desk.window, u"ownImportMoreNotes"_s);
        QVERIFY(tick != nullptr && details != nullptr && more != nullptr);
        QVERIFY2(!tick->isEnabled(), "a tick takes a click in the moment the list has come");
        QVERIFY(!details->isEnabled());
        QVERIFY(!more->isEnabled());
        clickOn(desk.window, tick);
        clickOn(desk.window, details);
        clickOn(desk.window, more);
        QTest::qWait(50);
        QVERIFY2(since.elapsed() < Desk::longRest, qPrintable(QString::number(since.elapsed())));
        QCOMPARE(desk.ownFiles.tickedCount(), 1);
        QVERIFY(!rowOf(desk.window, 2)->property("open").toBool());
        QCOMPARE(desk.ownFiles.foldedNotes(), 1);
        // After its moment, they do.
        QTRY_VERIFY_WITH_TIMEOUT(tick->isEnabled(), 20000);
        QVERIFY(details->isEnabled() && more->isEnabled());
        // The notes opened: every row moves, and its moment begins anew.
        clickOn(desk.window, more);
        QTRY_COMPARE(desk.ownFiles.foldedNotes(), 0);
        aFrame(desk.window);
        tick = inRow(rowOf(desk.window, 2), u"ownImportTick"_s);
        QVERIFY(tick != nullptr && !tick->isEnabled());
        clickOn(desk.window, tick);
        QTest::qWait(50);
        QCOMPARE(desk.ownFiles.tickedCount(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(tick->isEnabled(), 20000);
        // And a tick does not hold the others back: the first tick where none was begins the
        // moment of "Take" anew, and nothing else.
        clickOn(desk.window, tick);
        QTRY_COMPARE(desk.ownFiles.tickedCount(), 2);
        QVERIFY(inRow(rowOf(desk.window, 3), u"ownImportTick"_s)->isEnabled());
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void atTheSmallestSizeTheFirstDeviceIsInSightWhateverNotesThereAre_data()
    {
        QTest::addColumn<int>("notes");
        QTest::newRow("one note") << 1;
        QTest::newRow("three notes") << 3;
        QTest::newRow("twenty notes") << 20;
    }

    void atTheSmallestSizeTheFirstDeviceIsInSightWhateverNotesThereAre()
    {
        QFETCH(int, notes);
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.load(640, 400) != nullptr);
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        // Notes as long as a file can make them: devices that the reader leaves out, as they are
        // at the address of one that stands before them, each called by the longest name of the
        // widest letters there is; and four devices that it takes.
        QJsonArray devices = QJsonDocument::fromJson(fileOf({ticked(1, u"First"_s), ticked(2, u"Second"_s),
                                                             ticked(3, u"Third"_s), ticked(4, u"Fourth"_s)},
                                                            false))
                                 .object()[u"devices"_s]
                                 .toArray();
        QCOMPARE(devices.size(), 4);
        for (int note = 0; note < notes; ++note) {
            QJsonObject again = devices.at(0).toObject();
            again[u"name"_s] = QString(80, u'W');
            devices.insert(1, again);
        }
        QVERIFY(chooseAndRead(desk, core::testing::plainFile({{u"devices"_s, devices}})));
        QCOMPARE(desk.ownFiles.offered()->besideCount(), notes);
        QCOMPARE(desk.ownFiles.offered()->deviceCount(), 4);
        aFrame(desk.window);
        const QQuickItem* list = listOf(desk);
        QVERIFY(list != nullptr);
        QVERIFY(isInside(desk.window, list));
        QVERIFY(scrolledBy(list) < 0.5);
        // In the first screen: the first device, whole, with its tick, and whatever stands for
        // the notes before it.
        QTRY_VERIFY(rowOf(desk.window, 1) != nullptr);
        const QQuickItem* first = rowOf(desk.window, 1);
        keepPictureOf(desk.window, u"own-import-smallest-%1-notes"_s.arg(notes));
        QVERIFY2(isWholeIn(list, first), qPrintable(u"%1 of the first device's %2 px are in sight in a list of %3 px"_s
                                                        .arg(heightInSight(list, first))
                                                        .arg(first->height())
                                                        .arg(list->height())));
        QVERIFY(isWholeIn(list, inRow(first, u"ownImportTick"_s)));
        // And what stands for the notes says how many there are, and that they are to be opened
        // before anything is taken: nothing is, until they are.
        const QQuickItem* more = theOne(desk.window, u"ownImportMoreNotes"_s);
        QVERIFY(more != nullptr);
        QVERIFY(isWholeIn(list, more));
        QCOMPARE(more->property("text").toString(),
                 notes == 1 ? u"1 note about this file"_s : u"%1 notes about this file"_s.arg(notes));
        QCOMPARE(desk.ownFiles.foldedNotes(), notes);
        QTest::qWait(Desk::pastAQuickRest);
        QVERIFY(!theOne(desk.window, u"ownImportTake"_s)->isEnabled());
        clickOn(desk.window, more);
        QTRY_COMPARE(desk.ownFiles.foldedNotes(), 0);
        QTRY_COMPARE(seenOnes(desk.window, u"ownImportBeside"_s).size() > 0, true);
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }
};

int main(int argc, char* argv[])
{
    return runOwnFilesScreensTests<TestOwnFilesScreensRoom>(argc, argv);
}

#include "test_own_files_screens_room.moc"
