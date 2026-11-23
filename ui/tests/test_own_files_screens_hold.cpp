#include "own_files_screens_test.h"

using namespace qkvm;
using namespace qkvm::ui::testing::own_files_screens;

// The two boxes hold still: no button, tick, note, taking or end of its time comes to stand where another
// stood.
class TestOwnFilesScreensHold : public OwnFilesScreensTest {
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

    void noButtonOfEitherBoxComesToStandWhereAnotherStood_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::newRow("the smallest window") << QSize(640, 400);
        QTest::newRow("a window of 1280 by 800") << QSize(1280, 800);
    }

    void noButtonOfEitherBoxComesToStandWhereAnotherStood()
    {
        // Whatever comes, goes, opens or closes in a box, what a person was pointing at a moment
        // before is where it was, or nothing that takes a click stands there now: the box keeps
        // its size and its place, its buttons stand at its foot, and what changes in the list
        // changes nothing outside the list. Looked at after each change the box knows, here and
        // in the next test: in the smallest window, and in one of the usual size.
        QFETCH(QSize, size);
        WallOfOneTest own;
        QVERIFY(!own.put(u"http://127.0.0.1:20001"_s, u"Build server"_s).isEmpty());
        Desk desk(own);
        desk.ownFiles.restFor(Desk::heldRest, Desk::heldRest);
        QVERIFY(desk.load(size.width(), size.height()) != nullptr);
        QCOMPARE(desk.window->size(), size);
        const QString at = u", at %1 by %2"_s.arg(size.width()).arg(size.height());
        const auto keep = [&](const QString& name) {
            keepPictureOf(desk.window, u"own-still-%1x%2-%3"_s.arg(size.width()).arg(size.height()).arg(name));
        };
        const QString box = u"ownImportBox"_s;
        QStringList wrong;
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, box));

        // A short list, of as many devices as the list has room for: a list comes; and a row of
        // it is opened and closed at its top, in its middle and at its end (where the last one is
        // read down to before it is closed).
        const int few = size.height() < 500 ? 2 : 3;
        QList<Device> shortOne;
        for (int number = 1; number <= few; ++number) {
            shortOne.append(ticked(number, u"Short %1"_s.arg(number)));
        }
        desk.opens = desk.write(u"short.json"_s, fileOf(shortOne, false));
        clickOn(desk.window, theOne(desk.window, u"ownImportChoose"_s));
        QVERIFY(!desk.ownFiles.fileName().isEmpty());
        Seen before = seenOf(desk, box);
        clickOn(desk.window, theOne(desk.window, u"ownImportRead"_s));
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isLaidOut(), 20000);
        QTRY_VERIFY(rowOf(desk.window, few) != nullptr);
        wrong += movedBy(before, seenOf(desk, box), u"a short list comes"_s + at);
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isSettled(), 20000);
        QVERIFY2(mostScrolled(listOf(desk)) < 0.5, qPrintable(QString::number(mostScrolled(listOf(desk)))));
        const std::function<void(const QString&)> keepShort = [&](const QString& step) {
            keep(u"short-row-"_s + step);
        };
        for (int place = 1; place <= few; ++place) {
            const QString where = place == 1 ? u"top"_s : place == few ? u"end"_s : u"middle"_s;
            const QString failed =
                openAndClose(desk, place, u"a row at the %1 of a short list"_s.arg(where) + at, wrong, place == few,
                             place == 1 ? keepShort : std::function<void(const QString&)>());
            QVERIFY2(failed.isEmpty(), qPrintable(failed));
        }
        // The row at the end was read down to and closed again: in the smallest window the list
        // stands where it stood, with room left after its end and its first row in part above its
        // upper edge. Wherever the list is not at its beginning, its mark says that there is more.
        QQuickItem* shortList = listOf(desk);
        keep(u"short-row-end-closed"_s);
        if (size.height() < 500) {
            QVERIFY2(scrolledBy(shortList) > 0.5, qPrintable(QString::number(scrolledBy(shortList))));
        }
        if (scrolledBy(shortList) > 0.5) {
            QVERIFY2(
                theOne(desk.window, u"ownImportScrollMark"_s) != nullptr,
                qPrintable(u"the list stands %1 from its beginning, and no mark says so"_s.arg(scrolledBy(shortList))));
        }
        // The keys while that room is left: Down and PageDown go no further than what is there,
        // and move nothing; Up moves the rows a step down, and no further than to the list's top;
        // and where the keys moved the rows, the list's controls take no press for their moment.
        if (scrolledBy(shortList) > 0.5) {
            QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isSettled(), 20000);
            shortList->forceActiveFocus();
            const qreal stood = scrolledBy(shortList);
            for (const Qt::Key key : {Qt::Key_Down, Qt::Key_PageDown}) {
                const QString pressed = key == Qt::Key_Down ? u"Down"_s : u"PageDown"_s;
                before = seenOf(desk, box);
                QTest::keyClick(desk.window, key);
                wrong += movedBy(before, seenOf(desk, box),
                                 u"%1 while room is left after a short list's end"_s.arg(pressed) + at, everyRow);
                QVERIFY2(qAbs(scrolledBy(shortList) - stood) < 0.5,
                         qPrintable(u"%1: the list stood %2 from its beginning, and stands %3 from it"_s.arg(pressed)
                                        .arg(stood)
                                        .arg(scrolledBy(shortList))));
                QVERIFY2(desk.ownFiles.isSettled(), qPrintable(pressed + u": nothing moved, and the list was held"_s));
            }
            before = seenOf(desk, box);
            QTest::keyClick(desk.window, Qt::Key_Up);
            const Seen up = seenOf(desk, box);
            QVERIFY2(qAbs(scrolledBy(shortList) - std::max(0.0, stood - 40)) < 0.5,
                     qPrintable(u"Up: the list stood %1 from its beginning, and stands %2 from it"_s.arg(stood).arg(
                         scrolledBy(shortList))));
            QVERIFY2(!desk.ownFiles.isSettled(),
                     "Up moved the rows under the pointer, and the list took presses at once");
            for (const QString& control : up.live) {
                QVERIFY2(!up.inTheList.contains(control), qPrintable(u"after Up, \"%1\" takes a click"_s.arg(control)));
            }
            const int tickedWere = desk.ownFiles.tickedCount();
            QTest::mouseClick(desk.window, Qt::LeftButton, {},
                              before.controls.value(u"ownImportTick of device 1"_s).center().toPoint());
            QTest::qWait(50);
            QCOMPARE(desk.ownFiles.tickedCount(), tickedWere);
            QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isSettled(), 20000);
        }

        // Another file chosen while that list is shown: a long one, and sealed, so that the line
        // for its passphrase comes where the list stood. And its list comes; a row of it is
        // opened and closed at its top, in its middle and at its end.
        QList<Device> longOne;
        for (int number = 1; number <= 30; ++number) {
            longOne.append(ticked(number, u"Long %1"_s.arg(number)));
        }
        desk.opens = desk.write(u"long.qkvm"_s, fileOf(longOne));
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isSettled(), 20000);
        before = seenOf(desk, box);
        clickOn(desk.window, theOne(desk.window, u"ownImportChoose"_s));
        QTRY_VERIFY(theOne(desk.window, u"ownImportPassphrase"_s) != nullptr);
        QVERIFY(!desk.ownFiles.isLaidOut());
        wrong += movedBy(before, seenOf(desk, box), u"another file is chosen"_s + at);
        keep(u"another-file-chosen"_s);
        clickOn(desk.window, theOne(desk.window, u"ownImportPassphrase"_s));
        type(desk.window, thePassphrase());
        before = seenOf(desk, box);
        clickOn(desk.window, theOne(desk.window, u"ownImportRead"_s));
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isLaidOut(), 20000);
        QTRY_VERIFY(rowOf(desk.window, 1) != nullptr);
        wrong += movedBy(before, seenOf(desk, box), u"a long list comes"_s + at);
        QQuickItem* list = listOf(desk);
        QVERIFY2(mostScrolled(list) > 200, qPrintable(QString::number(mostScrolled(list))));
        const std::function<void(const QString&)> keepLong = [&](const QString& step) { keep(u"long-row-"_s + step); };
        QString failed = openAndClose(desk, 1, u"the row at the top of a long list"_s + at, wrong, false, keepLong);
        QVERIFY2(failed.isEmpty(), qPrintable(failed));
        scrollTo(list, mostScrolled(list) / 2);
        aFrame(desk.window);
        const int middle = theRowInTheMiddle(desk);
        QVERIFY2(middle > 1 && middle < 30, qPrintable(QString::number(middle)));
        failed = openAndClose(desk, middle, u"a row in the middle of a long list"_s + at, wrong, false);
        QVERIFY2(failed.isEmpty(), qPrintable(failed));
        QVERIFY(QMetaObject::invokeMethod(list, "positionViewAtEnd"));
        QTRY_VERIFY(rowOf(desk.window, 30) != nullptr);
        aFrame(desk.window);
        failed = openAndClose(desk, 30, u"the row at the end of a long list"_s + at, wrong, true);
        QVERIFY2(failed.isEmpty(), qPrintable(failed));

        // A taking that takes some: one of the devices was put on the wall by hand meanwhile.
        QVERIFY(!own.put(elsewhere(2), u"By hand"_s).isEmpty());
        const QQuickItem* take = theOne(desk.window, u"ownImportTake"_s);
        QVERIFY(take != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(take->isEnabled(), 20000);
        before = seenOf(desk, box);
        clickOn(desk.window, take);
        QTRY_VERIFY(desk.ownFiles.hasTaken());
        wrong += movedBy(before, seenOf(desk, box), u"a taking that took some"_s + at);
        keep(u"took-some"_s);
        QCOMPARE(desk.ownFiles.tookSummary(), u"29 of 30 KVMs were taken onto this wall."_s);
        clickOn(desk.window, theOne(desk.window, u"ownImportCancel"_s));
        QTRY_VERIFY(!desk.isOpen(box));

        // Export: the file is made, and the name it is to be kept under is refused (a plain file
        // given a sealed one's name, where a plain one of its own name is there); each is said
        // in the box, and nothing moves for it.
        const QString exportBox = u"ownExportBox"_s;
        static_cast<void>(desk.write(u"wall.json"_s, "a plain file that was there"));
        desk.savesTo = desk.folder.filePath(u"wall.qkvm"_s);
        QVERIFY(openFromTheSettings(desk, u"settingsExport"_s, exportBox));
        before = seenOf(desk, exportBox);
        keep(u"export-asked"_s);
        clickOn(desk.window, theOne(desk.window, u"ownExportMake"_s));
        QTRY_VERIFY_WITH_TIMEOUT(theOne(desk.window, u"ownExportSave"_s) != nullptr, 20000);
        wrong += movedBy(before, seenOf(desk, exportBox), u"the file is made"_s + at);
        keep(u"export-made"_s);
        const QQuickItem* save = theOne(desk.window, u"ownExportSave"_s);
        QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(), 20000);
        before = seenOf(desk, exportBox);
        clickOn(desk.window, save);
        QTRY_VERIFY(theOne(desk.window, u"ownExportProblem"_s) != nullptr);
        wrong += movedBy(before, seenOf(desk, exportBox), u"a name is refused"_s + at);
        keep(u"export-name-refused"_s);
        QCOMPARE(desk.suggestions.size(), 1);
        clickOn(desk.window, theOne(desk.window, u"ownExportCancel"_s));
        QTRY_VERIFY(!desk.isOpen(exportBox));
        QVERIFY2(nothingMoved(wrong), qPrintable(u"%1 times; the first: %2"_s.arg(wrong.size()).arg(wrong.value(0))));
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void noTickNoteTakingOrEndOfItsTimeComesToStandWhereAnotherStood_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::newRow("the smallest window") << QSize(640, 400);
        QTest::newRow("a window of 1280 by 800") << QSize(1280, 800);
    }

    void noTickNoteTakingOrEndOfItsTimeComesToStandWhereAnotherStood()
    {
        // The rest of what the box knows: its notes opened; a tick, the first one and the last
        // one, and one after which what stands over the list is a line shorter; a taking that
        // takes none, and one that takes all; and the end of the time for which what was read is
        // kept.
        QFETCH(QSize, size);
        WallOfOneTest own;
        Desk desk(own);
        desk.ownFiles.restFor(Desk::heldRest, Desk::heldRest);
        // (A wall of cards under the box, as a wall that is used has them: what a click finds there
        // once the box has gone is looked at below.)
        desk.wall.machines()->setTiles(ui::pretendWall());
        QVERIFY(desk.load(size.width(), size.height()) != nullptr);
        const QString at = u", at %1 by %2"_s.arg(size.width()).arg(size.height());
        const QString box = u"ownImportBox"_s;
        QStringList wrong;
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, box));

        // A list with a note beside its two devices; and the note opened.
        desk.opens = desk.write(u"noted.json"_s, fileWithSoManyLeftOut(1, {ticked(1, u"One"_s), ticked(2, u"Two"_s)}));
        clickOn(desk.window, theOne(desk.window, u"ownImportChoose"_s));
        Seen before = seenOf(desk, box);
        clickOn(desk.window, theOne(desk.window, u"ownImportRead"_s));
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isLaidOut(), 20000);
        QTRY_VERIFY(rowOf(desk.window, 2) != nullptr);
        wrong += movedBy(before, seenOf(desk, box), u"a list with a note comes"_s + at);
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isSettled(), 20000);
        before = seenOf(desk, box);
        clickOn(desk.window, theOne(desk.window, u"ownImportMoreNotes"_s));
        QTRY_COMPARE(desk.ownFiles.foldedNotes(), 0);
        wrong += movedBy(before, seenOf(desk, box), u"the notes are opened"_s + at);
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isSettled(), 20000);

        // Another file: three devices whose passwords go in the clear, none of them ticked to
        // begin with. (What stands over the list is two lines in the smallest window while one
        // of them wants a look, and one line once none does.)
        desk.opens = desk.write(u"clear.json"_s, fileOf({kvmAt(u"http://192.0.2.3:8080"_s, u"One"_s),
                                                         kvmAt(u"http://192.0.2.4:8080"_s, u"Two"_s),
                                                         kvmAt(u"http://192.0.2.5:8080"_s, u"Three"_s)},
                                                        false));
        clickOn(desk.window, theOne(desk.window, u"ownImportChoose"_s));
        clickOn(desk.window, theOne(desk.window, u"ownImportRead"_s));
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isLaidOut(), 20000);
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isSettled(), 20000);
        // An error over the list that is shown: Windows' box for choosing a file does not open.
        // What it says stands over the box's buttons, and the list is shown as it was: its end
        // comes up to it, and nothing in it moves.
        desk.doorFails = true;
        before = seenOf(desk, box);
        clickOn(desk.window, theOne(desk.window, u"ownImportChoose"_s));
        QTRY_VERIFY(theOne(desk.window, u"ownImportProblem"_s) != nullptr);
        QVERIFY(desk.ownFiles.isLaidOut());
        wrong += movedBy(before, seenOf(desk, box), u"an error comes over the list"_s + at, everyRow);
        desk.doorFails = false;
        // (Nothing is ticked: the button that takes stands in its place all the same, says that
        // it would take none, and takes no click.)
        const QQuickItem* take = theOne(desk.window, u"ownImportTake"_s);
        QVERIFY(take != nullptr);
        QCOMPARE(take->property("text").toString(), u"Take 0 KVMs"_s);
        QVERIFY(!take->isEnabled());
        const QQuickItem* counts = theOne(desk.window, u"ownImportCounts"_s);
        QVERIFY(counts != nullptr);
        // A tick, by its click: and what moved for it.
        const auto tick = [&](int place, int ticks, const QString& when) -> QString {
            if (!bringIntoSight(desk, place)) {
                return u"%1: the tick of device %2 is never in sight"_s.arg(when).arg(place);
            }
            const Seen was = seenOf(desk, box);
            clickOn(desk.window, inRow(rowOf(desk.window, place), u"ownImportTick"_s));
            if (!QTest::qWaitFor([&] { return desk.ownFiles.tickedCount() == ticks; }, 20000)) {
                return u"%1: %2 are ticked"_s.arg(when).arg(desk.ownFiles.tickedCount());
            }
            wrong += movedBy(was, seenOf(desk, box), when + at, everyRow);
            return {};
        };
        QElapsedTimer sinceTheFirstTick;
        sinceTheFirstTick.start();
        QString failed = tick(1, 1, u"the first tick"_s);
        QVERIFY2(failed.isEmpty(), qPrintable(failed));
        // (From nothing ticked to something: it takes no click for its moment. A click on it in
        // that moment takes nothing.)
        QCOMPARE(take->property("text").toString(), u"Take 1 KVM"_s);
        QVERIFY(!take->isEnabled());
        clickOn(desk.window, take);
        QTest::qWait(50);
        QVERIFY2(sinceTheFirstTick.elapsed() < Desk::heldRest,
                 qPrintable(QString::number(sinceTheFirstTick.elapsed())));
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY(desk.ownFiles.isLaidOut());
        QCOMPARE(desk.ownFiles.tickedCount(), 1);
        failed = tick(2, 2, u"a second tick"_s);
        QVERIFY2(failed.isEmpty(), qPrintable(failed));
        const int linesWere = counts->property("lineCount").toInt();
        failed = tick(3, 3, u"a tick that leaves none to look at"_s);
        QVERIFY2(failed.isEmpty(), qPrintable(failed));
        // (Where the box is narrowest, that is a line less of it: what this is about. Wider, it
        // may be one line before and after.)
        QVERIFY2(size.height() >= 500 || counts->property("lineCount").toInt() < linesWere,
                 qPrintable(u"what stands over the list has %1 lines, and had %2: %3"_s
                                .arg(counts->property("lineCount").toInt())
                                .arg(linesWere)
                                .arg(counts->property("text").toString())));
        failed = tick(3, 2, u"a tick taken away"_s);
        QVERIFY2(failed.isEmpty(), qPrintable(failed));
        failed = tick(2, 1, u"another tick taken away"_s);
        QVERIFY2(failed.isEmpty(), qPrintable(failed));
        failed = tick(1, 0, u"the last tick taken away"_s);
        QVERIFY2(failed.isEmpty(), qPrintable(failed));
        QCOMPARE(take->property("text").toString(), u"Take 0 KVMs"_s);
        QVERIFY(isSeen(take));
        QVERIFY(!take->isEnabled());

        // A taking that takes none: both that are ticked were put on the wall by hand meanwhile.
        failed = tick(1, 1, u"a first tick again"_s);
        QVERIFY2(failed.isEmpty(), qPrintable(failed));
        failed = tick(2, 2, u"a second tick again"_s);
        QVERIFY2(failed.isEmpty(), qPrintable(failed));
        QVERIFY(!own.put(u"http://192.0.2.3:8080"_s, u"By hand"_s).isEmpty());
        QVERIFY(!own.put(u"http://192.0.2.4:8080"_s, u"By hand too"_s).isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(take->isEnabled(), 20000);
        before = seenOf(desk, box);
        clickOn(desk.window, take);
        QTRY_VERIFY(desk.ownFiles.hasTaken());
        wrong += movedBy(before, seenOf(desk, box), u"a taking that took none"_s + at);
        QCOMPARE(desk.ownFiles.tookSummary(), u"None of the 2 KVMs was taken onto this wall."_s);
        clickOn(desk.window, theOne(desk.window, u"ownImportCancel"_s));
        QTRY_VERIFY(!desk.isOpen(box));

        // A taking that takes all: the box closes, and nothing of it is left to be pressed.
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, box));
        QVERIFY(chooseAndRead(desk, fileOf({ticked(11, u"Eleven"_s), ticked(12, u"Twelve"_s)}, false)));
        take = theOne(desk.window, u"ownImportTake"_s);
        QVERIFY(take != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(take->isEnabled(), 20000);
        before = seenOf(desk, box);
        const QPoint takeStood = middleOf(take);
        const QSignalSpy menus(&desk.actions, &ui::ActionsView::openAsked);
        const QSignalSpy consoles(&desk.actions, &ui::ActionsView::consoleAsked);
        const QSignalSpy cardActions(&desk.actions, &ui::ActionsView::cardActionAsked);
        const QSignalSpy ports(&desk.actions, &ui::ActionsView::portAsked);
        clickOn(desk.window, take);
        QTRY_VERIFY(!desk.isOpen(box));
        const Seen gone = seenOf(desk, box);
        wrong += movedBy(before, gone, u"a taking that took all"_s + at);
        QVERIFY(gone.controls.isEmpty());
        QVERIFY2(toldBy(desk.actions).contains(u"ok: 2 KVMs were taken onto this wall."_s),
                 qPrintable(toldBy(desk.actions).join(u'\n')));
        // (It stood over the settings, which are there again.)
        QVERIFY(desk.isOpen(u"settingsBox"_s));
        // The box is gone, and the click that follows lands on what is under it: what is there, for
        // a pointer that rests where "Take" stood, takes no press for a moment, by one click or two.
        const QStringList there = pressableAt(desk.window, takeStood);
        qInfo("where \"Take\" stood%s, once the box is gone: %s", qPrintable(at),
              there.isEmpty() ? "nothing that takes a press" : qPrintable(there.join(u", "_s)));
        const QQuickItem* focused = desk.window->activeFocusItem();
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, takeStood);
        QTest::mouseDClick(desk.window, Qt::LeftButton, {}, takeStood);
        QTest::qWait(50);
        QVERIFY2(desk.window->activeFocusItem() == focused && desk.wall.enlarged().isEmpty() && menus.isEmpty() &&
                     consoles.isEmpty() && cardActions.isEmpty() && ports.isEmpty(),
                 qPrintable(u"a click where \"Take\" stood, just after the box went, was taken there (%1)"_s.arg(
                     there.join(u", "_s))));
        // After that moment, a click there is taken as any: with the settings closed, by the wall.
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.isOpen(u"settingsBox"_s));
        aFrame(desk.window);
        const QQuickItem* focusedThen = desk.window->activeFocusItem();
        if (!there.isEmpty()) {
            QTRY_VERIFY_WITH_TIMEOUT(
                [&] {
                    QTest::mouseClick(desk.window, Qt::LeftButton, {}, takeStood);
                    return desk.window->activeFocusItem() != focusedThen || !menus.isEmpty() || !consoles.isEmpty() ||
                           !cardActions.isEmpty() || !ports.isEmpty();
                }(),
                20000);
        }

        // The end of the time for which what was read is kept (six seconds here, in a window of
        // its own), with the list scrolled as by somebody who reads down it.
        WallOfOneTest ownToo;
        Desk brief(ownToo, 6000);
        QVERIFY(brief.load(size.width(), size.height()) != nullptr);
        QVERIFY(openFromTheSettings(brief, u"settingsImport"_s, box));
        QList<Device> thirty;
        for (int number = 1; number <= 30; ++number) {
            thirty.append(ticked(number, u"KVM %1"_s.arg(number)));
        }
        QVERIFY(chooseAndRead(brief, fileOf(thirty), thePassphrase()));
        scrollTo(listOf(brief), 100);
        before = seenOf(brief, box);
        // (The look before is of the list, with its rows and their ticks in sight: what is compared
        // is what was pointed at in the list, and not a box that had lost its list already.)
        QVERIFY2(
            brief.ownFiles.isLaidOut() && !before.firstLines.isEmpty() &&
                before.ofDevice.size() >= before.firstLines.size(),
            qPrintable(u"%1 rows, %2 of their controls"_s.arg(before.firstLines.size()).arg(before.ofDevice.size())));
        QTRY_VERIFY_WITH_TIMEOUT(!brief.ownFiles.isLaidOut(), 30000);
        QTRY_VERIFY(theOne(brief.window, u"ownImportNote"_s) != nullptr);
        wrong += movedBy(before, seenOf(brief, box), u"the list's time is over"_s + at);
        QVERIFY2(nothingMoved(wrong), qPrintable(u"%1 times; the first: %2"_s.arg(wrong.size()).arg(wrong.value(0))));
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
        QVERIFY2(brief.complaints.isEmpty(), qPrintable(brief.complaints.join(u'\n')));
    }
};

int main(int argc, char* argv[])
{
    return runOwnFilesScreensTests<TestOwnFilesScreensHold>(argc, argv);
}

#include "test_own_files_screens_hold.moc"
