#include "own_files_screens_test.h"

using namespace qkvm;
using namespace qkvm::ui::testing::own_files_screens;

// The two boxes in every state and both looks, at the smallest window, and the previews of an action and of
// the settings.
class TestOwnFilesScreensLooks : public OwnFilesScreensTest {
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

    void theBoxesFitTheSmallestWindowAndScrollWhatDoesNotFit()
    {
        WallOfOneTest own;
        // A wall of fourteen, of which Windows hands out no password: fourteen lines beside the
        // file, and the one that every file of this PC's has.
        for (int number = 1; number <= 14; ++number) {
            const QString id = own.put(u"http://127.0.0.1:%1"_s.arg(20000 + number), u"Rack %1"_s.arg(number));
            QVERIFY(!id.isEmpty());
            own.secrets.all.remove(core::LocalWall::secretOf(id));
        }
        // And one more, called by the longest name a KVM can have, with no blank in it.
        const QString longNamed = own.put(u"http://127.0.0.1:20099"_s, QString(80, u'W'));
        QVERIFY(!longNamed.isEmpty());
        own.secrets.all.remove(core::LocalWall::secretOf(longNamed));
        Desk desk(own);
        desk.savesTo = desk.folder.filePath(u"small.qkvm"_s);
        QVERIFY(desk.load(640, 400) != nullptr);
        QCOMPARE(desk.window->height(), 400);
        QVERIFY(openFromTheSettings(desk, u"settingsExport"_s, u"ownExportBox"_s));
        QTRY_VERIFY(theOne(desk.window, u"ownExportPassphrase"_s)->property("hasKeyboard").toBool());
        type(desk.window, thePassphrase());
        clickOn(desk.window, theOne(desk.window, u"ownExportMake"_s));
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isMade(), 20000);
        QCOMPARE(desk.ownFiles.notInIt().size(), 16);
        aFrame(desk.window);
        // The box is whole inside the window, with its buttons; the lines are scrolled, and say so.
        const auto fits = [&desk](const QString& name) {
            const QObject* box = desk.box(name);
            const qreal top = box->property("y").toReal();
            return top >= 0 && top + box->property("height").toReal() <= desk.window->height();
        };
        QTRY_VERIFY(fits(u"ownExportBox"_s));
        QVERIFY(isInside(desk.window, theOne(desk.window, u"ownExportSave"_s)));
        QVERIFY(isInside(desk.window, theOne(desk.window, u"ownExportCancel"_s)));
        QVERIFY(theOne(desk.window, u"ownExportScrollMark"_s) != nullptr);
        QObject* lines = desk.window->findChild<QObject*>(u"ownExportNotInIt"_s);
        QVERIFY(lines != nullptr);
        QCOMPARE(lines->property("count").toInt(), 16);
        keepPictureOf(desk.window, u"own-export-small"_s);
        // Every line can be come to: the last one, at the list's end. It names a KVM whose name is
        // longer than a line of the box and has no blank to be broken at: it is whole all the same,
        // broken inside the name, and not cut off at the edge of its note.
        QVERIFY(QMetaObject::invokeMethod(lines, "positionViewAtEnd"));
        aFrame(desk.window);
        const QString last = desk.ownFiles.notInIt().constLast();
        QVERIFY2(last.contains(QString(80, u'W')), qPrintable(last));
        const QQuickItem* said = saying(desk.window->contentItem(), last);
        QVERIFY2(said != nullptr, qPrintable(last));
        QVERIFY(isInside(desk.window, said));
        QVERIFY(!said->property("truncated").toBool());
        QVERIFY2(said->property("contentWidth").toReal() <= said->width() + 0.5,
                 qPrintable(u"%1 in %2"_s.arg(said->property("contentWidth").toReal()).arg(said->width())));
        const QQuickItem* note = said->parentItem();
        QVERIFY(note != nullptr && note->objectName() == u"ownExportLine"_s);
        const QRectF letters = said->mapRectToItem(
            note, QRectF(0, 0, said->property("contentWidth").toReal(), said->property("contentHeight").toReal()));
        QVERIFY2(letters.right() <= note->width() && letters.bottom() <= note->height(),
                 "the line's letters reach beyond its note");
        keepPictureOf(desk.window, u"own-export-small-end"_s);
        clickOn(desk.window, theOne(desk.window, u"ownExportCancel"_s));
        QTRY_VERIFY(!desk.isOpen(u"ownExportBox"_s));

        // The box that takes a file in, with a list that is longer than the window is high.
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        QVERIFY(chooseAndRead(desk, fileOfEveryKind()));
        aFrame(desk.window);
        QTRY_VERIFY(fits(u"ownImportBox"_s));
        // (The server's controller, at the list's end, is one that can be taken, as the wall reads one from M7 1b-ii
        // on: said in short as what it will be known by.)
        bool controllerSaid = false;
        for (int place = 1; place <= 20 && !controllerSaid; ++place) {
            const QQuickItem* row =
                bringIntoSight(desk, place, u"ownImportInShort"_s) ? rowOf(desk.window, place) : nullptr;
            if (row != nullptr && saidIn(row, u"ownImportNameAndAddress"_s).contains(u"A server's controller"_s)) {
                QCOMPARE(saidIn(row, u"ownImportInShort"_s),
                         u"Known by Windows' word "_s + QChar(0xB7) + u" No password in the file"_s);
                controllerSaid = true;
            }
        }
        QVERIFY(controllerSaid);
        QQuickItem* offered = listOf(desk);
        offered->setProperty("contentY", offered->property("originY"));
        aFrame(desk.window);
        QVERIFY(isInside(desk.window, theOne(desk.window, u"ownImportTake"_s)));
        QVERIFY(isInside(desk.window, theOne(desk.window, u"ownImportCancel"_s)));
        QVERIFY(isInside(desk.window, theOne(desk.window, u"ownImportChoose"_s)));
        QVERIFY(isInside(desk.window, theOne(desk.window, u"ownImportCounts"_s)));
        QVERIFY(isInside(desk.window, theOne(desk.window, u"ownImportNothingYet"_s)));
        QVERIFY(theOne(desk.window, u"ownImportScrollMark"_s) != nullptr);
        keepPictureOf(desk.window, u"own-import-small"_s);
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    // --- pictures, for whoever wants to look -------------------------------------------------------

    void everyStateOfBothBoxesInBothLooks_data()
    {
        QTest::addColumn<QString>("look");
        QTest::addColumn<QSize>("size");
        QTest::newRow("dark") << u"dark"_s << QSize(1280, 800);
        QTest::newRow("light") << u"light"_s << QSize(1280, 800);
        QTest::newRow("dark, the smallest window") << u"dark"_s << QSize(640, 400);
        QTest::newRow("light, the smallest window") << u"light"_s << QSize(640, 400);
    }

    void actionAndSettingsPreviewFitsAndShowsTextOnlyOnRequest_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::newRow("wide") << QSize(1280, 800);
        QTest::newRow("small") << QSize(640, 400);
    }

    void actionAndSettingsPreviewFitsAndShowsTextOnlyOnRequest()
    {
        QFETCH(QSize, size);
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.load(size.width(), size.height()) != nullptr);
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        const QByteArray file =
            core::testing::plainFile({{u"actions"_s, QJsonArray{QJsonObject{{u"name"_s, u"Example text"_s},
                                                                            {u"kind"_s, u"text"_s},
                                                                            {u"text"_s, u"fixture text\nsecond line"_s},
                                                                            {u"danger"_s, true}}}},
                                      {u"wall"_s, QJsonObject{{u"interval"_s, 2.75}, {u"update_check"_s, false}}}});
        QVERIFY(chooseAndRead(desk, file));
        QTRY_VERIFY(desk.ownFiles.isSettled());
        QVERIFY(!says(desk.window, u"fixture text"_s));
        QCOMPARE(desk.ownFiles.tickedCount(), 1);
        QVERIFY(isInside(desk.window, theOne(desk.window, u"ownImportTake"_s)));
        keepPictureOf(desk.window, u"action-import-%1"_s.arg(size.width()));
        QCOMPARE(theOne(desk.window, u"ownImportTake"_s)->property("text").toString(), u"Import 1"_s);
        const auto actionRow = [&desk]() -> const QQuickItem* {
            QList<const QQuickItem*> rows;
            named(desk.window->contentItem(), u"ownImportRow"_s, rows);
            for (const auto* row : rows) {
                if (row->property("kind").toString() == u"action"_s) {
                    return row;
                }
            }
            return nullptr;
        };
        QVERIFY(actionRow() != nullptr);
        clickOn(desk.window, inRow(actionRow(), u"ownImportDetails"_s));
        QTRY_VERIFY(actionRow() != nullptr && actionRow()->property("open").toBool());
        aFrame(desk.window);
        const QQuickItem* reveal = inRow(actionRow(), u"importShowText"_s);
        QVERIFY(reveal != nullptr);
        QQuickItem* listItem = listOf(desk);
        const qreal buttonY = reveal->mapToItem(listItem, QPointF()).y();
        scrollTo(listItem,
                 std::clamp(scrolledBy(listItem) + buttonY - listItem->height() / 2, 0.0, mostScrolled(listItem)));
        aFrame(desk.window);
        clickOn(desk.window, inRow(actionRow(), u"importShowText"_s));
        QTRY_VERIFY(actionRow() != nullptr && !actionRow()->property("revealedText").toString().isEmpty());
        keepPictureOf(desk.window, u"action-text-review-%1"_s.arg(size.width()));
        QTRY_VERIFY(desk.ownFiles.isSettled());
        desk.ownFiles.setOpen(static_cast<int>(WallExchange::firstActionPlace), false);
        QTRY_VERIFY(actionRow() == nullptr || actionRow()->property("revealedText").toString().isEmpty());
        const auto* list = desk.ownFiles.offered();
        bool foundSettings = false;
        for (int at = 0; at < list->rowCount(); ++at) {
            const auto index = list->index(at);
            if (index.data(ui::LaidOutList::KindRole).toString() == u"settings"_s) {
                foundSettings = true;
                QVERIFY(!index.data(ui::LaidOutList::TickedRole).toBool());
            }
        }
        QVERIFY(foundSettings);
        QCOMPARE(desk.ownFiles.actionText(static_cast<int>(WallExchange::firstActionPlace)),
                 u"fixture text"_s + QChar(0x21B5) + u"\nsecond line"_s);
        desk.ownFiles.reset();
        QVERIFY(desk.ownFiles.actionText(static_cast<int>(WallExchange::firstActionPlace)).isEmpty());
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void everyStateOfBothBoxesInBothLooks()
    {
        QFETCH(QString, look);
        QFETCH(QSize, size);
        WallOfOneTest own;
        QVERIFY(!own.put(u"http://127.0.0.1:20001"_s, u"Build server"_s).isEmpty());
        const QString rack = own.put(u"http://127.0.0.1:20002"_s, u"Rack"_s);
        own.secrets.all.remove(core::LocalWall::secretOf(rack));
        QVERIFY(!own.put(elsewhere(11), u"There already"_s).isEmpty());
        HeldWork work;
        Desk desk(own, WallExchange::keptForMsAsItIs, work.sealing());
        desk.savesTo = desk.folder.filePath(u"wall.qkvm"_s);
        QVERIFY(desk.load(size.width(), size.height()) != nullptr);
        QCOMPARE(desk.window->size(), size);
        // (Every look is drawn and kept: the program's own, which a test sets without Windows. The
        // pictures of the smallest window are called so.)
        QVERIFY2(lookIs(desk, look), qPrintable(u"the screens are not drawn in the %1 look"_s.arg(look)));
        const QString ofTheSize = size.height() < 500 ? u"-%1x%2"_s.arg(size.width()).arg(size.height()) : QString();
        const auto keep = [&](const QString& name) { keepPictureOf(desk.window, name + u'-' + look + ofTheSize); };
        // (The pictures of a button that takes no click yet are taken in a long moment; and
        // what is then waited for, in a short one.)
        const auto restLong = [&desk] { desk.ownFiles.restFor(Desk::longRest, Desk::longRest); };
        const auto restShort = [&desk] { desk.ownFiles.restFor(Desk::quickRest, Desk::quickRest); };

        // Export: empty; working; the lines, with "Save".
        QVERIFY(openFromTheSettings(desk, u"settingsExport"_s, u"ownExportBox"_s));
        QTRY_VERIFY(theOne(desk.window, u"ownExportPassphrase"_s)->property("hasKeyboard").toBool());
        keep(u"own-export-1-empty"_s);
        type(desk.window, thePassphrase());
        work.hold();
        restLong();
        clickOn(desk.window, theOne(desk.window, u"ownExportMake"_s));
        QTRY_COMPARE(work.begun(), 1);
        QTRY_VERIFY(theOne(desk.window, u"ownExportWorking"_s) != nullptr);
        keep(u"own-export-2-working"_s);
        work.letGo();
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isMade(), 20000);
        QTRY_VERIFY(theOne(desk.window, u"ownExportSave"_s) != nullptr);
        keep(u"own-export-3-lines-save-resting"_s);
        QTRY_VERIFY_WITH_TIMEOUT(theOne(desk.window, u"ownExportSave"_s)->isEnabled(), 20000);
        keep(u"own-export-3-lines-save"_s);
        clickOn(desk.window, theOne(desk.window, u"ownExportCancel"_s));
        QTRY_VERIFY(!desk.isOpen(u"ownExportBox"_s));

        // Import: a file chosen, and sealed; laid out, with devices of every kind (from its
        // top to its end); the top of a long list; what came of a taking where one was not taken.
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        keep(u"own-import-0-empty"_s);
        desk.opens = desk.write(u"sealed.qkvm"_s, fileOf({ticked(1, u"One"_s)}));
        clickOn(desk.window, theOne(desk.window, u"ownImportChoose"_s));
        QTRY_VERIFY(theOne(desk.window, u"ownImportPassphrase"_s) != nullptr);
        clickOn(desk.window, theOne(desk.window, u"ownImportPassphrase"_s));
        type(desk.window, u"a passphrase"_s);
        keep(u"own-import-1-chosen-sealed"_s);
        desk.opens = desk.write(u"every-kind.json"_s, fileOfEveryKind());
        clickOn(desk.window, theOne(desk.window, u"ownImportChoose"_s));
        clickOn(desk.window, theOne(desk.window, u"ownImportRead"_s));
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isLaidOut(), 20000);
        QTRY_VERIFY(theOne(desk.window, u"ownImportTake"_s) != nullptr);
        keep(u"own-import-2-laid-out-resting"_s);
        // (Its notes, folded, opened once the list has stood for its moment.)
        QVERIFY(theOne(desk.window, u"ownImportMoreNotes"_s) != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isSettled(), 20000);
        clickOn(desk.window, theOne(desk.window, u"ownImportMoreNotes"_s));
        QTRY_COMPARE(desk.ownFiles.foldedNotes(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(theOne(desk.window, u"ownImportTake"_s)->isEnabled(), 20000);
        keep(u"own-import-2-laid-out-a"_s);
        restShort();
        QObject* list = desk.window->findChild<QObject*>(u"ownImportOffered"_s);
        QVERIFY(list != nullptr);
        const qreal page = list->property("height").toReal();
        const char* const parts = "bcdefghij";
        for (int part = 0; parts[part] != '\0'; ++part) {
            const qreal most = list->property("contentHeight").toReal() - page;
            const qreal next = std::min(most, list->property("contentY").toReal() + page - 24);
            if (next <= list->property("contentY").toReal() + 1) {
                break;
            }
            list->setProperty("contentY", next);
            aFrame(desk.window);
            keep(u"own-import-2-laid-out-"_s + QLatin1Char(parts[part]));
        }
        // (And the same list with a row of each kind opened, from its top: before, opened, and
        // closed again.)
        list->setProperty("contentY", list->property("originY"));
        aFrame(desk.window);
        keep(u"own-import-2-laid-out-rows-before"_s);
        for (const bool opening : {true, false}) {
            for (const int place : {1, 5}) {
                QVERIFY(bringIntoSight(desk, place, u"ownImportDetails"_s));
                QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isSettled(), 20000);
                clickOn(desk.window, inRow(rowOf(desk.window, place), u"ownImportDetails"_s));
                QTRY_COMPARE(isSeen(inRow(rowOf(desk.window, place), u"ownImportBehind"_s)), opening);
            }
            QVERIFY(bringIntoSight(desk, 1, u"ownImportDetails"_s));
            keep(opening ? u"own-import-2-laid-out-rows-opened"_s : u"own-import-2-laid-out-rows-closed-again"_s);
        }
        desk.opens = desk.write(u"two-thousand.json"_s, fileOfSoMany(2000));
        clickOn(desk.window, theOne(desk.window, u"ownImportChoose"_s));
        clickOn(desk.window, theOne(desk.window, u"ownImportRead"_s));
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isLaidOut(), 60000);
        QTRY_VERIFY(rowOf(desk.window, 1) != nullptr);
        keep(u"own-import-3-long-list-top"_s);
        desk.opens = desk.write(u"three.json"_s,
                                fileOf({ticked(21, u"One"_s), ticked(22, u"Two"_s), ticked(23, u"Three"_s)}, false));
        clickOn(desk.window, theOne(desk.window, u"ownImportChoose"_s));
        clickOn(desk.window, theOne(desk.window, u"ownImportRead"_s));
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isLaidOut(), 20000);
        QTRY_VERIFY_WITH_TIMEOUT(theOne(desk.window, u"ownImportTake"_s) != nullptr &&
                                     theOne(desk.window, u"ownImportTake"_s)->isEnabled(),
                                 20000);
        QVERIFY(!own.put(elsewhere(22), u"By hand"_s).isEmpty());
        clickOn(desk.window, theOne(desk.window, u"ownImportTake"_s));
        QTRY_VERIFY(desk.ownFiles.hasTaken());
        keep(u"own-import-4-took-one-not-taken"_s);
        clickOn(desk.window, theOne(desk.window, u"ownImportCancel"_s));
        QTRY_VERIFY(!desk.isOpen(u"ownImportBox"_s));
        // And another file is being worked on just now, which is said as what passes.
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        WallExchange other(own.wall, work.sealing());
        QObject asker;
        work.hold();
        other.layOut(fileOf({ticked(1, u"One"_s)}), thePassphrase(), &asker, [](const auto&) {});
        QTRY_COMPARE(work.begun(), 2);
        desk.opens = desk.write(u"waits.json"_s, fileOf({ticked(31, u"One"_s)}, false));
        clickOn(desk.window, theOne(desk.window, u"ownImportChoose"_s));
        clickOn(desk.window, theOne(desk.window, u"ownImportRead"_s));
        QTRY_VERIFY(theOne(desk.window, u"ownImportNote"_s) != nullptr);
        keep(u"own-import-6-another-file-is-worked-on"_s);
        work.letGo();
        QTRY_VERIFY_WITH_TIMEOUT(!other.isBusy(), 20000);
        clickOn(desk.window, theOne(desk.window, u"ownImportCancel"_s));
        QTRY_VERIFY(!desk.isOpen(u"ownImportBox"_s));
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));

        // After the time for which what was read is kept (a few seconds, here): the box as it
        // was with its list, and then with its sentence.
        WallOfOneTest ownToo;
        Desk brief(ownToo, 4000);
        QVERIFY(brief.load(size.width(), size.height()) != nullptr);
        QVERIFY(lookIs(brief, look));
        QVERIFY(openFromTheSettings(brief, u"settingsImport"_s, u"ownImportBox"_s));
        QVERIFY(chooseAndRead(brief, anOrdinaryFile(), thePassphrase()));
        QTRY_VERIFY_WITH_TIMEOUT(!brief.ownFiles.isLaidOut(), 30000);
        QTRY_VERIFY(theOne(brief.window, u"ownImportNote"_s) != nullptr);
        keepPictureOf(brief.window, u"own-import-5-kept-no-longer-"_s + look + ofTheSize);
        QVERIFY2(brief.complaints.isEmpty(), qPrintable(brief.complaints.join(u'\n')));
    }
};

int main(int argc, char* argv[])
{
    return runOwnFilesScreensTests<TestOwnFilesScreensLooks>(argc, argv);
}

#include "test_own_files_screens_looks.moc"
