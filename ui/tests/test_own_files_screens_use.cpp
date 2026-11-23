#include "own_files_screens_test.h"

using namespace qkvm;
using namespace qkvm::ui::testing::own_files_screens;

// The two boxes in use: where they hang, a wall written to a file and one taken in by the buttons a person
// presses, the rows and their ticks, a list of two thousand, a passphrase's line, the times a list and a file
// are kept for, and the boxes going with their window.
class TestOwnFilesScreensUse : public OwnFilesScreensTest {
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

    // --- where the boxes hang --------------------------------------------------------------------

    void thisPcOffersItsWallAsAFileInTheSettingsWhereAnAccountOffersAServers()
    {
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.load() != nullptr);
        // "This PC" has no file of the wall any more: whose the wall is, and the way to a server.
        clickOn(desk.window, theOne(desk.window, u"accountButton"_s));
        QTRY_VERIFY(desk.isOpen(u"accountMenu"_s));
        QVERIFY(says(desk.window, u"This PC"_s));
        QVERIFY(theOne(desk.window, u"connectToAServer"_s) != nullptr);
        QVERIFY(!says(desk.window, u"Export…"_s));
        QVERIFY(!says(desk.window, u"Import…"_s));
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.isOpen(u"accountMenu"_s));

        // The settings have it: their section of this PC's wall as a file, chosen in their list,
        // with its two buttons in its first line, and what the page says of the two kinds of file.
        QVERIFY(QMetaObject::invokeMethod(desk.box(u"settingsBox"_s), "open"));
        QTRY_VERIFY(desk.isOpen(u"settingsBox"_s));
        const QList<const QQuickItem*> sections = seenOnes(desk.window, u"settingsSection"_s);
        const auto files = std::ranges::find_if(
            sections, [](const QQuickItem* one) { return one->property("key").toString() == u"files"_s; });
        QVERIFY(files != sections.end());
        clickOn(desk.window, *files);
        QTRY_VERIFY(theOne(desk.window, u"settingsExport"_s) != nullptr);
        QVERIFY(theOne(desk.window, u"settingsImport"_s) != nullptr);
        QVERIFY(says(desk.window, u"Export and import"_s));
        QVERIFY(says(desk.window,
                     u"Choose KVMs, saved actions and wall settings to keep in a file or bring to this PC. "
                     "A qKVM server writes and reads the same file."_s));
        QVERIFY(says(desk.window, u"Encrypted (.qkvm)"_s));
        QVERIFY(says(desk.window, u"Plain (.json)"_s));
        QVERIFY(says(desk.window, u"Adds what is not here yet"_s));
        // Each opens its box for this PC's wall, and not a server's, over the settings, as the page
        // stacks them; and closes back to them.
        clickOn(desk.window, theOne(desk.window, u"settingsExport"_s));
        QTRY_VERIFY(desk.isOpen(u"ownExportBox"_s));
        QVERIFY(desk.isOpen(u"settingsBox"_s));
        QVERIFY(!desk.isOpen(u"exportBox"_s));
        QVERIFY(says(desk.window, u"Export to a file"_s));
        // (A wall with a box over it is told so.)
        QVERIFY(desk.window->property("boxOpen").toBool());
        clickOn(desk.window, theOne(desk.window, u"ownExportCancel"_s));
        QTRY_VERIFY(!desk.isOpen(u"ownExportBox"_s));
        QVERIFY(desk.isOpen(u"settingsBox"_s));
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        QVERIFY(!desk.isOpen(u"importBox"_s));
        QVERIFY(says(desk.window, u"Import from a file"_s));
        QVERIFY(desk.window->property("boxOpen").toBool());
        clickOn(desk.window, theOne(desk.window, u"ownImportCancel"_s));
        QTRY_VERIFY(!desk.isOpen(u"ownImportBox"_s));
        QVERIFY(desk.isOpen(u"settingsBox"_s));
        QTRY_VERIFY(theOne(desk.window, u"settingsImport"_s) != nullptr);
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.isOpen(u"settingsBox"_s));
        QVERIFY(!desk.window->property("boxOpen").toBool());
        // (The settings open again where they were left: at that section.)
        QVERIFY(QMetaObject::invokeMethod(desk.box(u"settingsBox"_s), "open"));
        QTRY_VERIFY(theOne(desk.window, u"settingsImport"_s) != nullptr);
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.isOpen(u"settingsBox"_s));

        // At a server's wall that whoever is signed in manages, the settings list no section of this
        // PC's wall: they list that server's wall as a file, under a heading of its own, as the page
        // lists it for an admin, in the page's words. Its two buttons open the server's boxes, and
        // not these, over the settings; and each closes back to them. The account has neither.
        desk.session.show(atAServersWall());
        QVERIFY(QMetaObject::invokeMethod(desk.box(u"settingsBox"_s), "open"));
        QTRY_VERIFY(desk.isOpen(u"settingsBox"_s));
        QTRY_VERIFY(!seenOnes(desk.window, u"settingsSection"_s).isEmpty());
        QStringList keys;
        for (const QQuickItem* one : seenOnes(desk.window, u"settingsSection"_s)) {
            keys.append(one->property("key").toString());
        }
        QCOMPARE(keys, (QStringList{u"general"_s, u"video"_s, u"alerts"_s, u"security"_s, u"updates"_s, u"about"_s,
                                    u"files"_s}));
        QVERIFY(says(desk.window, u"The server's wall"_s));
        QVERIFY(!says(desk.window, u"This PC's wall"_s));
        // (It opens where it was left, at "Export and import": the server's now.)
        QTRY_VERIFY(theOne(desk.window, u"serverExport"_s) != nullptr);
        QVERIFY(seenOnes(desk.window, u"settingsExport"_s).isEmpty());
        for (const QString& line :
             {u"The wall as a file: to keep, or to take to another qKVM."_s,
              u"The KVMs and servers on this wall, the saved actions and the alert settings can be written to a "
              u"file, and a file can be taken in here or on another qKVM."_s,
              u"Made with a passphrase you choose. It holds the passwords of the devices, the saved texts and the "
              u"address alerts go to, and opens with that passphrase only."_s,
              u"Made without a passphrase. The same, with every password and saved text left out: its devices ask "
              u"for their password where they are taken in."_s,
              u"Nothing on this wall is changed or removed by it. The alert and wall settings are replaced only "
              u"where you tick them."_s,
              u"Accounts, tokens, two-factor keys and the recovery key are in no such file: each belongs to the qKVM "
              u"it was made on. To keep a whole qKVM, accounts and all, keep a copy of its data folder."_s}) {
            QVERIFY2(says(desk.window, line), qPrintable(line));
        }
        keepPictureOf(desk.window, u"settings-server-files"_s);
        clickOn(desk.window, theOne(desk.window, u"serverExport"_s));
        QTRY_VERIFY(desk.isOpen(u"exportBox"_s));
        QVERIFY(desk.isOpen(u"settingsBox"_s));
        QVERIFY(!desk.isOpen(u"ownExportBox"_s));
        clickOn(desk.window, theOne(desk.window, u"exportCancel"_s));
        QTRY_VERIFY(!desk.isOpen(u"exportBox"_s));
        QVERIFY(desk.isOpen(u"settingsBox"_s));
        clickOn(desk.window, theOne(desk.window, u"serverImport"_s));
        QTRY_VERIFY(desk.isOpen(u"importBox"_s));
        QVERIFY(desk.isOpen(u"settingsBox"_s));
        QVERIFY(!desk.isOpen(u"ownImportBox"_s));
        clickOn(desk.window, theOne(desk.window, u"importCancel"_s));
        QTRY_VERIFY(!desk.isOpen(u"importBox"_s));
        QVERIFY(desk.isOpen(u"settingsBox"_s));
        QTRY_VERIFY(theOne(desk.window, u"serverImport"_s) != nullptr);
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.isOpen(u"settingsBox"_s));
        clickOn(desk.window, theOne(desk.window, u"accountButton"_s));
        QTRY_VERIFY(desk.isOpen(u"accountMenu"_s));
        QVERIFY(!says(desk.window, u"Export…"_s));
        QVERIFY(!says(desk.window, u"Import…"_s));
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.isOpen(u"accountMenu"_s));

        // Whoever only acts on that server's machines, or only looks, is offered no such file: the
        // server would say no. And the settings open at the first section they list.
        ui::SessionView::Shown operating = atAServersWall();
        operating.manages = false;
        desk.session.show(operating);
        QVERIFY(QMetaObject::invokeMethod(desk.box(u"settingsBox"_s), "open"));
        QTRY_VERIFY(desk.isOpen(u"settingsBox"_s));
        QTRY_VERIFY(!seenOnes(desk.window, u"settingsSection"_s).isEmpty());
        for (const QQuickItem* one : seenOnes(desk.window, u"settingsSection"_s)) {
            QVERIFY(one->property("key").toString() != u"files"_s);
        }
        QVERIFY(!says(desk.window, u"The server's wall"_s));
        QVERIFY(seenOnes(desk.window, u"serverExport"_s).isEmpty());
        // (And where the same wall comes to let them manage it, the box stays and lists in place
        // what comes with that: the server's wall as a file. Only more came.)
        desk.session.show(atAServersWall());
        QTRY_VERIFY(std::ranges::any_of(seenOnes(desk.window, u"settingsSection"_s), [](const QQuickItem* one) {
            return one->property("key").toString() == u"files"_s;
        }));
        QVERIFY(desk.isOpen(u"settingsBox"_s));
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    // --- export, through -------------------------------------------------------------------------

    void theWallIsWrittenToAFileByTheButtonsAPersonPresses()
    {
        WallOfOneTest own;
        QVERIFY(!own.put(u"http://127.0.0.1:20001"_s, u"Build server"_s).isEmpty());
        const QString rack = own.put(u"http://127.0.0.1:20002"_s, u"Rack"_s);
        own.secrets.all.remove(core::LocalWall::secretOf(rack));
        HeldWork work;
        Desk desk(own, WallExchange::keptForMsAsItIs, work.sealing());
        // (A long moment before "Save" takes a click: this test looks at it not taking one.)
        desk.ownFiles.restFor(Desk::longRest, Desk::longRest);
        desk.savesTo = desk.folder.filePath(u"wall.qkvm"_s);
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openFromTheSettings(desk, u"settingsExport"_s, u"ownExportBox"_s));

        // A passphrase for the file, or none, and what either does. No password of an account:
        // there is none on this PC.
        const QQuickItem* passphrase = theOne(desk.window, u"ownExportPassphrase"_s);
        QVERIFY(passphrase != nullptr);
        QVERIFY(says(desk.window, u"Passphrase for the file"_s));
        QVERIFY(says(desk.window, u"A passphrase encrypts the file, including device passwords and saved text actions. "
                                  "Left empty, the file omits passwords and all text actions."_s));
        QVERIFY(!says(desk.window, u"Your password"_s));
        QTRY_VERIFY(passphrase->property("hasKeyboard").toBool());
        QVERIFY(theOne(desk.window, u"ownExportSave"_s) == nullptr);
        QVERIFY(theOne(desk.window, u"ownExportWorking"_s) == nullptr);

        // What is typed is hidden, and shown while its eye is open; sent, it is on show no longer.
        type(desk.window, thePassphrase());
        QCOMPARE(passphrase->property("text").toString(), thePassphrase());
        QVERIFY(!passphrase->property("shown").toBool());
        QList<const QQuickItem*> eyes;
        named(passphrase, u"eye"_s, eyes);
        QCOMPARE(eyes.size(), 1);
        clickOn(desk.window, eyes.constFirst());
        QTRY_VERIFY(passphrase->property("shown").toBool());

        // "Make the file": the box says that it works, there is nothing to press twice, and what
        // the file is made with is not written over. The window does not stand still meanwhile.
        work.hold();
        clickOn(desk.window, theOne(desk.window, u"ownExportMake"_s));
        QTRY_COMPARE(work.begun(), 1);
        QVERIFY(!passphrase->property("shown").toBool());
        QTRY_VERIFY(theOne(desk.window, u"ownExportWorking"_s) != nullptr);
        // (Its last sign is the three dots in one, made here from its number.)
        QVERIFY(says(desk.window, u"Making the file"_s + QChar(0x2026)));
        QVERIFY(!theOne(desk.window, u"ownExportMake"_s)->isEnabled());
        QVERIFY(!passphrase->isEnabled());
        QVERIFY(theOne(desk.window, u"ownExportCancel"_s)->isEnabled());
        aFrame(desk.window);
        // (Not by a slip of the hand: a click beside the box does not end it.)
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, QPoint(8, desk.window->height() - 8));
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTest::qWait(100);
        QVERIFY(desk.isOpen(u"ownExportBox"_s));
        QVERIFY(desk.ownFiles.isMaking());

        // Made: every line of what is not in the file stands in the box, each whole, and only
        // then "Save". The passphrase is out of its line. What the lines are is said so that it
        // can be read.
        work.letGo();
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isMade(), 20000);
        QTRY_VERIFY(theOne(desk.window, u"ownExportSave"_s) != nullptr);
        QVERIFY(theOne(desk.window, u"ownExportMake"_s) == nullptr);
        QVERIFY(theOne(desk.window, u"ownExportPassphrase"_s) == nullptr);
        QCOMPARE(passphrase->property("text").toString(), QString());
        QVERIFY(says(desk.window, u"The file is made, encrypted with its passphrase, and is not saved yet. Before you "
                                  "save it: what is not in it, and what a qKVM server will read differently."_s));
        const QStringList lines = desk.ownFiles.notInIt();
        QCOMPARE(lines.size(), 2);
        QTRY_COMPARE(seenOnes(desk.window, u"ownExportLine"_s).size(), 2);
        for (const QString& line : lines) {
            const QQuickItem* said = saying(desk.window->contentItem(), line);
            QVERIFY2(said != nullptr, qPrintable(line));
            QVERIFY(!said->property("truncated").toBool());
            QVERIFY(isInside(desk.window, said));
        }
        // Not by the click that made the file: "Save" stands where "Make the file" stood, and
        // takes no click for the first moment the lines are on show.
        const QQuickItem* save = theOne(desk.window, u"ownExportSave"_s);
        QVERIFY(!save->isEnabled());
        clickOn(desk.window, save);
        QVERIFY(desk.suggestions.isEmpty());
        QVERIFY(!QFile::exists(desk.savesTo));

        // "Save": Windows' own box, and the file is written whole. The box closes, and its
        // window's corner says what was written.
        QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(), 20000);
        clickOn(desk.window, save);
        QTRY_VERIFY(!desk.isOpen(u"ownExportBox"_s));
        QCOMPARE(desk.suggestions.size(), 1);
        QVERIFY(desk.suggestions.constFirst().endsWith(u".qkvm"_s));
        QCOMPARE(
            toldBy(desk.actions),
            QStringList{u"ok: Exported to wall.qkvm. The file opens with its passphrase only: keep the two apart."_s});
        const base::Result<core::exchange::Content> kept =
            core::exchange::read(Desk::read(desk.savesTo), thePassphrase());
        QVERIFY2(kept.ok(), kept.ok() ? "" : kept.error().message.c_str());
        QCOMPARE(kept.value().devices.value_or(QList<Device>()).size(), 2);
        // Closed, the box keeps nothing; opened again, it begins anew.
        QVERIFY(!desk.ownFiles.isMade());
        QVERIFY(openFromTheSettings(desk, u"settingsExport"_s, u"ownExportBox"_s));
        QVERIFY(theOne(desk.window, u"ownExportMake"_s) != nullptr);
        QCOMPARE(theOne(desk.window, u"ownExportPassphrase"_s)->property("text").toString(), QString());
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void cancelClosesTheBoxOverAFileThatIsBeingMadeAndThatFileIsNobodys()
    {
        WallOfOneTest own;
        QVERIFY(!own.put(u"http://127.0.0.1:20001"_s, u"Build server"_s).isEmpty());
        HeldWork work;
        Desk desk(own, WallExchange::keptForMsAsItIs, work.sealing());
        desk.savesTo = desk.folder.filePath(u"wall.qkvm"_s);
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openFromTheSettings(desk, u"settingsExport"_s, u"ownExportBox"_s));
        QTRY_VERIFY(theOne(desk.window, u"ownExportPassphrase"_s)->property("hasKeyboard").toBool());
        type(desk.window, thePassphrase());
        work.hold();
        clickOn(desk.window, theOne(desk.window, u"ownExportMake"_s));
        QTRY_COMPARE(work.begun(), 1);
        QTRY_VERIFY(theOne(desk.window, u"ownExportWorking"_s) != nullptr);
        // "Cancel" while the file is made: the box is gone at once, and waits for nothing. The
        // making itself is not ended: it goes on, with nobody waiting for it.
        clickOn(desk.window, theOne(desk.window, u"ownExportCancel"_s));
        QTRY_VERIFY(!desk.isOpen(u"ownExportBox"_s));
        QVERIFY(!desk.ownFiles.isBusy());
        QVERIFY(!desk.ownFiles.holdsItsWindow());
        QVERIFY(desk.exchange.isBusy());
        // Whoever asks for a file meanwhile is told that one is being worked on, which passes.
        QVERIFY(openFromTheSettings(desk, u"settingsExport"_s, u"ownExportBox"_s));
        clickOn(desk.window, theOne(desk.window, u"ownExportMake"_s));
        QTRY_VERIFY(theOne(desk.window, u"ownExportNote"_s) != nullptr);
        QCOMPARE(theOne(desk.window, u"ownExportNote"_s)->property("text").toString(),
                 u"Another file is being worked on just now, and one is worked on at a time. That passes: try again "
                 "in a moment."_s);
        QVERIFY(theOne(desk.window, u"ownExportProblem"_s) == nullptr);
        clickOn(desk.window, theOne(desk.window, u"ownExportCancel"_s));
        QTRY_VERIFY(!desk.isOpen(u"ownExportBox"_s));
        // What the work comes to afterwards is nobody's.
        work.letGo();
        QTRY_VERIFY_WITH_TIMEOUT(!desk.exchange.isBusy(), 20000);
        QTest::qWait(100);
        QVERIFY(!desk.ownFiles.isMade());
        QVERIFY(toldBy(desk.actions).isEmpty());
        QVERIFY(desk.suggestions.isEmpty());
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    // --- import, through -------------------------------------------------------------------------

    void kvmsOfAFileAreTakenOntoTheWallByTheButtonsAPersonPresses()
    {
        WallOfOneTest own;
        QVERIFY(!own.put(elsewhere(11), u"There already"_s).isEmpty());
        Desk desk(own);
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        // Nothing chosen yet: nothing to read, no passphrase asked for, no list.
        QVERIFY(!theOne(desk.window, u"ownImportRead"_s)->isEnabled());
        QVERIFY(theOne(desk.window, u"ownImportPassphrase"_s) == nullptr);
        QVERIFY(theOne(desk.window, u"ownImportTake"_s) == nullptr);
        QVERIFY(theOne(desk.window, u"ownImportOffered"_s) == nullptr);
        QVERIFY(theOne(desk.window, u"ownImportAbove"_s) == nullptr);

        // A file is chosen in Windows' own box. It says of itself that it is sealed, and only
        // then is its passphrase asked for.
        const Device pinned = knownBy(kvmAt(elsewhere(1), u"Rack 1, top"_s), "first");
        const QByteArray sealed =
            fileOf({pinned, ticked(2, u"By Windows"_s), kvmAt(u"http://192.0.2.4:8080"_s, u"In the clear"_s),
                    notKeptHere(ticked(9, u"A small one"_s)), ticked(11, u"Twice"_s)});
        desk.opens = desk.write(u"wall.qkvm"_s, sealed);
        clickOn(desk.window, theOne(desk.window, u"ownImportChoose"_s));
        QCOMPARE(desk.opened, 1);
        QTRY_VERIFY(says(desk.window, u"wall.qkvm"_s));
        const QQuickItem* passphrase = theOne(desk.window, u"ownImportPassphrase"_s);
        QVERIFY(passphrase != nullptr);
        QVERIFY(says(desk.window, u"Passphrase of the file"_s));
        QVERIFY(theOne(desk.window, u"ownImportRead"_s)->isEnabled());
        // A wrong passphrase: said in the server's words, and nothing is laid out. What was
        // typed stays in its line, to be put right.
        clickOn(desk.window, passphrase);
        type(desk.window, u"not the passphrase"_s);
        clickOn(desk.window, theOne(desk.window, u"ownImportRead"_s));
        QTRY_VERIFY(says(desk.window, u"That is not the passphrase this file was made with"_s));
        QVERIFY(theOne(desk.window, u"ownImportOffered"_s) == nullptr);
        QVERIFY(theOne(desk.window, u"ownImportTake"_s) == nullptr);
        QCOMPARE(desk.exchange.passwordsHeld(), 0);
        QCOMPARE(passphrase->property("text").toString(), u"not the passphrase"_s);
        // The right one, with Enter in its line.
        clickOn(desk.window, passphrase);
        QTest::keyClick(desk.window, Qt::Key_A, Qt::ControlModifier);
        type(desk.window, thePassphrase());
        QTest::keyClick(desk.window, Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isLaidOut(), 20000);
        // (Nothing of the list takes a click until it has stood for its moment.)
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isSettled(), 20000);

        // Laid out, and nothing on the wall has changed. The passphrase is asked for no more,
        // and is out of its line.
        QCOMPARE(own.names(), QStringList{u"There already"_s});
        QTRY_VERIFY(theOne(desk.window, u"ownImportOffered"_s) != nullptr);
        QVERIFY(theOne(desk.window, u"ownImportPassphrase"_s) == nullptr);
        QCOMPARE(passphrase->property("text").toString(), QString());
        QVERIFY(theOne(desk.window, u"ownImportRead"_s) == nullptr);
        aFrame(desk.window);
        // Above the list, once: how many devices the file has, how many are ticked, how many
        // are not and want a look, how many cannot be taken; that nothing has changed yet, nor
        // was anything asked anywhere; and that each KVM that is taken is asked what it is
        // before a password goes to it.
        const QQuickItem* list = listOf(desk);
        QVERIFY(list != nullptr);
        const QQuickItem* counts = theOne(desk.window, u"ownImportCounts"_s);
        QVERIFY(counts != nullptr);
        QCOMPARE(
            counts->property("text").toString(),
            u"5 devices of this file are in this list. 2 are ticked. 1 is not ticked and wants a look. 2 cannot be "
            "taken."_s);
        const QString once = u"Nothing on the wall has changed yet, and nothing was asked at any address. Each KVM "
                             "that is taken is asked what it is before a password goes to it."_s;
        QVERIFY(says(desk.window, once));
        QVERIFY(isInside(desk.window, counts));
        // (It is no row of the list: it is not scrolled away with it.)
        for (const QQuickItem* up = counts; up != nullptr; up = up->parentItem()) {
            QVERIFY2(up != list, "what stands above the list is in the list");
        }

        // Every device of the file in the file's order, all five in sight at once, each a
        // short row: its name and address in one piece, and one line of a few words. What is
        // said of every device alike is said of none.
        for (int place = 1; place <= 5; ++place) {
            const QQuickItem* row = rowOf(desk.window, place);
            QVERIFY2(row != nullptr, qPrintable(QString::number(place)));
            QVERIFY2(isWholeIn(list, row), qPrintable(QString::number(place)));
            QVERIFY2(row->height() < 60, qPrintable(u"%1: %2"_s.arg(place).arg(row->height())));
            QVERIFY(!isSeen(inRow(row, u"ownImportBehind"_s)));
        }
        QStringList inTheList;
        everyTextIn(list, inTheList);
        for (const QString& said : std::as_const(inTheList)) {
            QVERIFY2(!said.contains(u"asked"_s), qPrintable(said));
            QVERIFY2(!said.contains(u"SHA-256"_s), qPrintable(said));
        }
        const QQuickItem* first = rowOf(desk.window, 1);
        QCOMPARE(saidIn(first, u"ownImportNameAndAddress"_s), u"\"Rack 1, top\" at https://192.0.2.1"_s);
        QCOMPARE(saidIn(first, u"ownImportInShort"_s), u"Known by its certificate "_s + inShortOf(pinned.certificate));
        QCOMPARE(inRow(first, u"ownImportInShort"_s)->property("color").value<QColor>(), desk.colour("muted"));
        QVERIFY(isSeen(inRow(first, u"ownImportTick"_s)));
        QVERIFY(inRow(first, u"ownImportTick"_s)->property("checked").toBool());
        QCOMPARE(saidIn(rowOf(desk.window, 2), u"ownImportInShort"_s), u"Known by Windows' word"_s);
        // Which are ticked to begin with: not the one whose password goes in the clear. Why
        // not stands on its row, in sight, in the colour of a warning; its tick is there to be
        // set.
        QCOMPARE(desk.ownFiles.tickedCount(), 2);
        const QQuickItem* take = theOne(desk.window, u"ownImportTake"_s);
        QVERIFY(take != nullptr);
        QCOMPARE(take->property("text").toString(), u"Take 2 KVMs"_s);
        const QQuickItem* plain = rowOf(desk.window, 3);
        QVERIFY(isSeen(inRow(plain, u"ownImportTick"_s)));
        QVERIFY(inRow(plain, u"ownImportTick"_s)->isEnabled());
        QVERIFY(!inRow(plain, u"ownImportTick"_s)->property("checked").toBool());
        QCOMPARE(saidIn(plain, u"ownImportInShort"_s), u"Plain HTTP: its password goes in the clear"_s);
        QVERIFY(isSeen(inRow(plain, u"ownImportInShort"_s)));
        QCOMPARE(inRow(plain, u"ownImportInShort"_s)->property("color").value<QColor>(), desk.colour("warn"));
        QCOMPARE(inRow(plain, u"ownImportNameAndAddress"_s)->property("color").value<QColor>(), desk.colour("text"));
        // One that cannot be taken is shown all the same, with why, and does not look like one
        // that waits to be ticked: it has no tick at all, and is grey. A click where its tick
        // would be does nothing.
        const QQuickItem* small = rowOf(desk.window, 4);
        QCOMPARE(saidIn(small, u"ownImportInShort"_s),
                 u"Cannot be taken: this PC's wall does not keep it as the file has it"_s);
        QVERIFY(!isSeen(inRow(small, u"ownImportTick"_s)));
        QVERIFY(isSeen(inRow(small, u"ownImportNoTick"_s)));
        QVERIFY(!isSeen(inRow(plain, u"ownImportNoTick"_s)));
        QCOMPARE(inRow(small, u"ownImportInShort"_s)->property("color").value<QColor>(), desk.colour("muted"));
        QCOMPARE(inRow(small, u"ownImportNameAndAddress"_s)->property("color").value<QColor>(), desk.colour("muted"));
        aFrame(desk.window);
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, middleOf(inRow(small, u"ownImportNoTick"_s)));
        const QQuickItem* twice = rowOf(desk.window, 5);
        QCOMPARE(saidIn(twice, u"ownImportInShort"_s),
                 u"Cannot be taken: on the wall already, or earlier in this file"_s);
        QTest::mouseClick(desk.window, Qt::LeftButton, {},
                          inRow(twice, u"ownImportNoTick"_s)->mapToScene(QPointF(5, 1)).toPoint());
        QTest::qWait(50);
        QCOMPARE(desk.ownFiles.tickedCount(), 2);
        // (Which KVM of the wall is in its way is behind its row, as a sentence.)
        clickOn(desk.window, inRow(twice, u"ownImportDetails"_s));
        QTRY_VERIFY(isSeen(inRow(twice, u"ownImportBehind"_s)));
        QCOMPARE(saidIn(twice, u"ownImportVerdict"_s),
                 u"It cannot be taken: it is on the wall already (\"There already\" at https://192.0.2.11)."_s);
        // Ticked by whoever wants it; and the tick of one that is not wanted taken away. What
        // stands above the list follows, and so does the button. (Once the list has stood for
        // its moment again: a row that is opened moves those under it.)
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isSettled(), 20000);
        clickOn(desk.window, inRow(plain, u"ownImportTick"_s));
        QTRY_COMPARE(desk.ownFiles.tickedCount(), 3);
        QVERIFY(inRow(plain, u"ownImportTick"_s)->property("checked").toBool());
        QCOMPARE(take->property("text").toString(), u"Take 3 KVMs"_s);
        QCOMPARE(counts->property("text").toString(),
                 u"5 devices of this file are in this list. 3 are ticked. 2 cannot be taken."_s);
        clickOn(desk.window, inRow(rowOf(desk.window, 2), u"ownImportTick"_s));
        QTRY_COMPARE(desk.ownFiles.tickedCount(), 2);
        QVERIFY(!inRow(rowOf(desk.window, 2), u"ownImportTick"_s)->property("checked").toBool());
        QCOMPARE(take->property("text").toString(), u"Take 2 KVMs"_s);
        QCOMPARE(counts->property("text").toString(),
                 u"5 devices of this file are in this list. 2 are ticked. 2 cannot be taken."_s);

        // "Take 2 KVMs": those two are on the wall, the box closes, and its window's corner says
        // how many, and nothing a second time.
        QTRY_VERIFY_WITH_TIMEOUT(take->isEnabled(), 20000);
        clickOn(desk.window, take);
        QTRY_VERIFY(!desk.isOpen(u"ownImportBox"_s));
        QCOMPARE(own.names(), (QStringList{u"There already"_s, u"Rack 1, top"_s, u"In the clear"_s}));
        QCOMPARE(toldBy(desk.actions), QStringList{u"ok: 2 KVMs were taken onto this wall."_s});
        QCOMPARE(desk.exchange.passwordsHeld(), 0);
        QVERIFY(!desk.ownFiles.isLaidOut());
        // Opened again, the box begins anew.
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        QVERIFY(theOne(desk.window, u"ownImportOffered"_s) == nullptr);
        QVERIFY(!says(desk.window, u"wall.qkvm"_s));
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void nineDevicesOfAnOrdinaryFileAreInSightAtOnceWithTheirTicks()
    {
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.load(1280, 800) != nullptr);
        QCOMPARE(desk.window->size(), QSize(1280, 800));
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        QVERIFY(chooseAndRead(desk, anOrdinaryFile(), thePassphrase()));
        QTRY_VERIFY(rowOf(desk.window, 9) != nullptr);
        aFrame(desk.window);
        const QQuickItem* list = listOf(desk);
        QVERIFY(list != nullptr);
        // All of the list is in sight: there is nothing to scroll, and nothing says there is.
        QVERIFY2(mostScrolled(list) < 0.5, qPrintable(QString::number(mostScrolled(list))));
        QVERIFY(theOne(desk.window, u"ownImportScrollMark"_s) == nullptr);
        QVERIFY(isInside(desk.window, list));
        QCOMPARE(deviceRows(desk.window).size(), 9);
        // Each of the nine, whole: its name and address, its one line, and its tick, or what
        // stands where a tick would be.
        for (int place = 1; place <= 9; ++place) {
            const QQuickItem* row = rowOf(desk.window, place);
            QVERIFY2(row != nullptr, qPrintable(QString::number(place)));
            QVERIFY2(isWholeIn(list, row), qPrintable(QString::number(place)));
            const QQuickItem* name = inRow(row, u"ownImportNameAndAddress"_s);
            QVERIFY(name != nullptr && isSeen(name) && !name->property("truncated").toBool());
            QVERIFY(isSeen(inRow(row, u"ownImportInShort"_s)));
            const QQuickItem* tick =
                inRow(row, row->property("canBeTaken").toBool() ? u"ownImportTick"_s : u"ownImportNoTick"_s);
            QVERIFY2(tick != nullptr && isSeen(tick) && isWholeIn(list, tick), qPrintable(QString::number(place)));
        }
        // What each says in short, in the file's order.
        QStringList inShort;
        for (int place = 1; place <= 9; ++place) {
            inShort.append(saidIn(rowOf(desk.window, place), u"ownImportInShort"_s));
        }
        const QString byCertificate = u"Known by its certificate "_s;
        QCOMPARE(inShort,
                 (QStringList{byCertificate + inShortOf(knownBy(kvmAt(elsewhere(1)), "first").certificate),
                              byCertificate + inShortOf(knownBy(kvmAt(elsewhere(2)), "second").certificate),
                              byCertificate + inShortOf(knownBy(kvmAt(elsewhere(3)), "comet").certificate),
                              u"Known by Windows' word"_s, u"Known by Windows' word"_s,
                              u"Known by Windows' word, not by what it shows first"_s,
                              u"Plain HTTP: its password goes in the clear"_s, u"Its address is this PC itself"_s,
                              u"Cannot be taken: this PC's wall does not keep it as the file has it"_s}));
        QCOMPARE(
            theOne(desk.window, u"ownImportCounts"_s)->property("text").toString(),
            u"9 devices of this file are in this list. 6 are ticked. 2 are not ticked and want a look. 1 cannot be "
            "taken."_s);
        QCOMPARE(theOne(desk.window, u"ownImportTake"_s)->property("text").toString(), u"Take 6 KVMs"_s);
        QVERIFY(isInside(desk.window, theOne(desk.window, u"ownImportTake"_s)));
        keepPictureOf(desk.window, u"own-import-nine-devices"_s);
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void whatStandsBehindARowIsOpenedByItsDetailsAndNothingOfADeviceIsLost()
    {
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        // The first of forty is known by the certificate its file names; the second would be
        // known by what it shows first, to a server.
        const Device pinned = knownBy(kvmAt(elsewhere(1), u"Rack 1, top"_s), "first");
        QList<Device> devices{pinned, saying(kvmAt(elsewhere(2), u"Not checked"_s), u"insecure"_s)};
        for (int number = 3; number <= 40; ++number) {
            devices.append(ticked(number, u"KVM %1"_s.arg(number)));
        }
        QVERIFY(chooseAndRead(desk, fileOf(devices), thePassphrase()));
        QTRY_VERIFY(rowOf(desk.window, 1) != nullptr);
        aFrame(desk.window);
        QQuickItem* list = listOf(desk);
        const QQuickItem* first = rowOf(desk.window, 1);
        const qreal closed = first->height();
        // Closed to begin with: its name and address, and its one line; and "Details".
        QVERIFY(!isSeen(inRow(first, u"ownImportBehind"_s)));
        QVERIFY(!isSeen(inRow(first, u"ownImportFingerprint"_s)));
        const QQuickItem* details = inRow(first, u"ownImportDetails"_s);
        QVERIFY(details != nullptr && isSeen(details));
        QCOMPARE(details->property("text").toString(), u"Details"_s);

        // Opened: everything else that is to be seen of it before it is taken. Whether it can
        // be taken and as what, as a sentence; every sentence that is said of it, each by
        // itself, in the core's order; and the SHA-256 of the certificate its file names,
        // whole, in the letters the box that changes a KVM shows one in: four lines of eight
        // pairs, to be compared by eye.
        clickOn(desk.window, details);
        QTRY_VERIFY(isSeen(inRow(first, u"ownImportBehind"_s)));
        QVERIFY(first->property("open").toBool());
        QVERIFY(details->property("checked").toBool());
        // (How high a row is with what stands behind it is known when it was laid out anew.)
        QTRY_VERIFY2(first->height() > closed + 100, qPrintable(QString::number(first->height())));
        QCOMPARE(saidIn(first, u"ownImportInShort"_s), u"Known by its certificate "_s + inShortOf(pinned.certificate));
        QCOMPARE(saidIn(first, u"ownImportVerdict"_s),
                 u"It can be taken, as a KVM that runs kvmd (PiKVM, GL.iNet Comet)."_s);
        QStringList saidOfIt;
        QList<const QQuickItem*> sentences;
        named(first, u"ownImportSays"_s, sentences);
        for (const QQuickItem* sentence : std::as_const(sentences)) {
            QVERIFY(isSeen(sentence));
            QVERIFY(!sentence->property("truncated").toBool());
            saidOfIt.append(sentence->property("text").toString());
        }
        QCOMPARE(saidOfIt,
                 (QStringList{u"The file names its certificate. It will be known by that certificate and by no "
                              "other."_s,
                              u"The file has its password. It is kept with Windows, for this address and for what "
                              "the KVM is known by."_s}));
        QVERIFY(says(desk.window, u"SHA-256 of the certificate the file names, to compare with what the KVM's own "
                                  "page shows:"_s));
        const QQuickItem* fingerprint = inRow(first, u"ownImportFingerprint"_s);
        QVERIFY(fingerprint != nullptr && isSeen(fingerprint));
        QCOMPARE(fingerprint->property("text").toString().replace(u'\n', u':'),
                 core::kvm::fingerprint(pinned.certificate));
        QCOMPARE(fingerprint->property("text").toString().count(u'\n'), 3);
        QCOMPARE(fingerprint->property("font").value<QFont>().family(), u"Consolas"_s);
        QVERIFY(!fingerprint->property("truncated").toBool());
        QVERIFY(isWholeIn(list, fingerprint));
        keepPictureOf(desk.window, u"own-import-row-opened"_s);
        // Its tick is as it was, and is set as before.
        QVERIFY(inRow(first, u"ownImportTick"_s)->property("checked").toBool());
        QCOMPARE(desk.ownFiles.tickedCount(), 40);

        // The second, opened: what a server would know it by, and what this program does
        // instead, as the whole sentence. On its row, that in a few words.
        const QQuickItem* second = rowOf(desk.window, 2);
        QVERIFY(second != nullptr);
        QCOMPARE(saidIn(second, u"ownImportInShort"_s), u"Known by Windows' word, not by what it shows first"_s);
        // (Once the list has stood for its moment again: the first one, opened, moved it.)
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isSettled(), 20000);
        clickOn(desk.window, inRow(second, u"ownImportDetails"_s));
        QTRY_VERIFY(isSeen(inRow(second, u"ownImportBehind"_s)));
        sentences.clear();
        named(second, u"ownImportSays"_s, sentences);
        QCOMPARE(sentences.size(), 2);
        QVERIFY2(sentences.constFirst()->property("text").toString().startsWith(
                     u"The file says that its certificate is not checked. This program does not take the first "
                     "certificate a device from a file shows."_s),
                 qPrintable(sentences.constFirst()->property("text").toString()));
        QVERIFY(!isSeen(inRow(second, u"ownImportFingerprint"_s)));

        // What is opened is the program's to keep, as a tick is: scrolled out and in, the row
        // is as it was left.
        QVERIFY(QMetaObject::invokeMethod(list, "positionViewAtEnd"));
        QTRY_VERIFY(rowOf(desk.window, 40) != nullptr);
        aFrame(desk.window);
        QTRY_VERIFY(rowOf(desk.window, 1) == nullptr);
        QVERIFY(!isSeen(inRow(rowOf(desk.window, 40), u"ownImportBehind"_s)));
        QVERIFY(QMetaObject::invokeMethod(list, "positionViewAtBeginning"));
        QTRY_VERIFY(rowOf(desk.window, 1) != nullptr);
        aFrame(desk.window);
        first = rowOf(desk.window, 1);
        QVERIFY(isSeen(inRow(first, u"ownImportBehind"_s)));
        QVERIFY(isSeen(inRow(first, u"ownImportFingerprint"_s)));
        // Closed again by the same button.
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isSettled(), 20000);
        clickOn(desk.window, inRow(first, u"ownImportDetails"_s));
        QTRY_VERIFY(!isSeen(inRow(first, u"ownImportBehind"_s)));
        QTRY_VERIFY2(qAbs(first->height() - closed) < 0.5, qPrintable(QString::number(first->height())));
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void aVncServersRowSaysWhatItSaysInItsFamilysWords()
    {
        // Outside review c9a-3, L3 (c9a-4, L3): a VNC server of a file was said of in a KVM's words: its certificate to
        // be compared "with what the KVM's own page shows" (a VNC server has none), and one in the clear "Plain HTTP:
        // its password goes in the clear" (vnc:// is no HTTP, and VNC sends no password). In its family's words.
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        Device pinned = knownBy(kvmAt(u"vncs://192.0.2.1:5901"_s, u"Console"_s), "first");
        pinned.kind = u"vnc"_s;
        Device plain = kvmAt(u"vnc://192.0.2.2:5901"_s, u"Guest"_s);
        plain.kind = u"vnc"_s;
        QVERIFY(chooseAndRead(desk, fileOf({pinned, plain}), thePassphrase()));
        QTRY_VERIFY(rowOf(desk.window, 2) != nullptr);
        aFrame(desk.window);
        QCOMPARE(saidIn(rowOf(desk.window, 2), u"ownImportInShort"_s),
                 u"Unencrypted (vnc): what is seen and typed goes in the clear"_s);
        const QQuickItem* first = rowOf(desk.window, 1);
        QCOMPARE(saidIn(first, u"ownImportInShort"_s), u"Known by its certificate "_s + inShortOf(pinned.certificate));
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isSettled(), 20000);
        clickOn(desk.window, inRow(first, u"ownImportDetails"_s));
        QTRY_VERIFY(isSeen(inRow(first, u"ownImportBehind"_s)));
        QCOMPARE(saidIn(first, u"ownImportVerdict"_s), u"It can be taken, as a VNC server."_s);
        QVERIFY(isSeen(inRow(first, u"ownImportFingerprint"_s)));
        QVERIFY(says(desk.window, u"SHA-256 of the certificate the file names, to compare with the certificate the "
                                  "server was set up with:"_s));
        QVERIFY(!says(desk.window, u"SHA-256 of the certificate the file names, to compare with what the KVM's own "
                                   "page shows:"_s));
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void aTickIsInSightWheneverAnyOfItsRowIs()
    {
        // The smallest window: the list has room for three short rows, and a row that is
        // opened is higher than the list.
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.load(640, 400) != nullptr);
        QCOMPARE(desk.window->size(), QSize(640, 400));
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        QVERIFY(chooseAndRead(desk, anOrdinaryFile(), thePassphrase()));
        QTRY_VERIFY(rowOf(desk.window, 1) != nullptr);
        aFrame(desk.window);
        QQuickItem* list = listOf(desk);
        QVERIFY(list != nullptr);
        QVERIFY(isInside(desk.window, list));
        // What is looked at wherever the list stands: of every device that can be taken and has
        // any of its row in sight, as much of its tick is in sight as of its row can be.
        int looks = 0;
        const auto everyTickIsInSight = [&](const QString& when) -> QString {
            for (const QQuickItem* row : deviceRows(desk.window)) {
                const qreal ofTheRow = heightInSight(list, row);
                if (ofTheRow < 0.5 || !row->property("canBeTaken").toBool()) {
                    continue;
                }
                ++looks;
                const QQuickItem* tick = inRow(row, u"ownImportTick"_s);
                if (tick == nullptr || !isSeen(tick)) {
                    return u"%1: device %2 has %3 px of its row in sight, and no tick at all"_s.arg(when)
                        .arg(row->property("place").toInt())
                        .arg(ofTheRow);
                }
                const qreal ofTheTick = heightInSight(list, tick);
                if (ofTheTick < std::min(tick->height(), ofTheRow) - 1.0) {
                    return u"%1: device %2 has %3 px of its row in sight, and %4 of the %5 px of its tick"_s.arg(when)
                        .arg(row->property("place").toInt())
                        .arg(ofTheRow)
                        .arg(ofTheTick)
                        .arg(tick->height());
                }
            }
            return {};
        };
        // Through the whole list, a few pixels at a time, with every row closed.
        const auto throughTheList = [&](const QString& when) -> QString {
            for (qreal at = 0;; at += 3) {
                const qreal to = std::min(at, mostScrolled(list));
                scrollTo(list, to);
                if (const QString wrong = everyTickIsInSight(u"%1, scrolled by %2"_s.arg(when).arg(to));
                    !wrong.isEmpty()) {
                    return wrong;
                }
                if (to >= mostScrolled(list) || at > 20000) {
                    return {};
                }
            }
        };
        QVERIFY2(mostScrolled(list) > 100, qPrintable(QString::number(mostScrolled(list))));
        QString wrong = throughTheList(u"rows closed"_s);
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong));
        QVERIFY2(looks > 100, qPrintable(QString::number(looks)));
        // (A picture of a row that is in sight in part, at each edge of the list.)
        scrollTo(list, 20);
        keepPictureOf(desk.window, u"own-import-small-row-in-part"_s);

        // The first row opened: it is higher than the list now. While its top is out of sight
        // and more of it is left than a tick is high, its tick stands whole at the list's upper
        // edge, to be seen and to be pressed.
        scrollTo(list, 0);
        aFrame(desk.window);
        clickOn(desk.window, inRow(rowOf(desk.window, 1), u"ownImportDetails"_s));
        QTRY_VERIFY(isSeen(inRow(rowOf(desk.window, 1), u"ownImportBehind"_s)));
        aFrame(desk.window);
        const QQuickItem* first = rowOf(desk.window, 1);
        QVERIFY2(first->height() > list->height() + 40,
                 qPrintable(u"%1 in %2"_s.arg(first->height()).arg(list->height())));
        looks = 0;
        wrong = throughTheList(u"the first row opened"_s);
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong));
        // (Back to its top, and down again a little at a time, as a person scrolls: a list that is
        // sent to a far place at once reckons where its rows are from how high they are on
        // average, and one of them is six times as high as the others.)
        QVERIFY(QMetaObject::invokeMethod(list, "positionViewAtBeginning"));
        aFrame(desk.window);
        for (int scrolled = 5; scrolled <= 70; scrolled += 5) {
            scrollTo(list, scrolled);
        }
        aFrame(desk.window);
        first = rowOf(desk.window, 1);
        QVERIFY(first != nullptr);
        const QQuickItem* tick = inRow(first, u"ownImportTick"_s);
        const QRectF where = tick->mapRectToItem(list, QRectF(0, 0, tick->width(), tick->height()));
        QVERIFY2(qAbs(where.top()) < 1.0, qPrintable(QString::number(where.top())));
        QVERIFY(isWholeIn(list, tick));
        keepPictureOf(desk.window, u"own-import-small-row-opened"_s);
        // Pressed there, it is the first one's tick that is taken away.
        QVERIFY(tick->property("checked").toBool());
        QCOMPARE(desk.ownFiles.tickedCount(), 6);
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isSettled(), 20000);
        clickOn(desk.window, tick);
        QTRY_COMPARE(desk.ownFiles.tickedCount(), 5);
        QVERIFY(!tick->property("checked").toBool());
        QVERIFY(!desk.ownFiles.offered()->tickedPlaces().contains(1));
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void anOpenedRowTallerThanTheListKeepsTheDevicesNameWithItsTick()
    {
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.load(640, 400) != nullptr);
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        QVERIFY(chooseAndRead(desk, anOrdinaryFile(), thePassphrase()));
        QTRY_VERIFY(rowOf(desk.window, 1) != nullptr);
        aFrame(desk.window);
        QQuickItem* list = listOf(desk);
        QVERIFY(list != nullptr);
        // The first row opened: it is higher than the list. Scrolled down into it, as a person
        // reads down it, a little at a time.
        clickOn(desk.window, inRow(rowOf(desk.window, 1), u"ownImportDetails"_s));
        QTRY_VERIFY(isSeen(inRow(rowOf(desk.window, 1), u"ownImportBehind"_s)));
        aFrame(desk.window);
        QVERIFY(rowOf(desk.window, 1)->height() > list->height() + 40);
        const QString called = u"\"Rack 1, top\" at https://192.0.2.1"_s;
        // (Not before its top has gone: then its name stands where it stands, once.)
        QVERIFY(!isSeen(inRow(rowOf(desk.window, 1), u"ownImportStuckName"_s)));
        for (int scrolled = 5; scrolled <= 70; scrolled += 5) {
            scrollTo(list, scrolled);
        }
        aFrame(desk.window);
        const QQuickItem* first = rowOf(desk.window, 1);
        QVERIFY(first != nullptr);
        // Its own line with the name is scrolled out over the list's upper edge; the tick stands
        // at that edge, and the device's name and address with it, whole: what is ticked and
        // what is read below it are never of two devices that the eye cannot tell apart.
        QVERIFY(heightInSight(list, inRow(first, u"ownImportNameAndAddress"_s)) < 0.5);
        const QQuickItem* tick = inRow(first, u"ownImportTick"_s);
        QVERIFY(tick != nullptr && isWholeIn(list, tick));
        const QQuickItem* stuck = inRow(first, u"ownImportStuckName"_s);
        QVERIFY2(stuck != nullptr && isSeen(stuck), "the tick stands at the list's edge without the device's name");
        keepPictureOf(desk.window, u"own-import-small-row-opened-name-kept"_s);
        const QQuickItem* name = saying(stuck, called);
        QVERIFY2(name != nullptr, qPrintable(saidIn(stuck, u"ownImportStuckNameText"_s)));
        QVERIFY(isWholeIn(list, name));
        QVERIFY(!name->property("truncated").toBool());
        const QRectF ofTheTick = tick->mapRectToItem(list, QRectF(0, 0, tick->width(), tick->height()));
        const QRectF ofTheName = name->mapRectToItem(list, QRectF(0, 0, name->width(), name->height()));
        QVERIFY2(qAbs(ofTheTick.top()) < 1.0 && ofTheName.top() < ofTheTick.bottom(),
                 qPrintable(u"the tick at %1, the name at %2"_s.arg(ofTheTick.top()).arg(ofTheName.top())));
        QVERIFY(ofTheName.left() >= ofTheTick.right());
        // (And what scrolls under it is not seen through it.)
        QCOMPARE(stuck->property("color").value<QColor>(), desk.colour("panel"));
        // Scrolled back to the row's top, it is gone: its name stands once.
        scrollTo(list, 0);
        aFrame(desk.window);
        QVERIFY(!isSeen(inRow(rowOf(desk.window, 1), u"ownImportStuckName"_s)));
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void theMarkOfATickIsDrawnWithinTheListAndNotOverWhatLiesBeyondItsEdge()
    {
        // The tests' pictures are drawn without a graphics card, by Qt Quick's own drawing in
        // software. There, a drawing of lines (the mark of a tick is one) was painted whole over
        // what lies beyond the edge of the list it is in, where its box was half scrolled out.
        // Looked at here in the picture itself: the strip of the box just above the list, and
        // the one just under it, are the same whatever the list is scrolled to.
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        QList<Device> devices;
        for (int number = 1; number <= 30; ++number) {
            devices.append(ticked(number, u"KVM %1"_s.arg(number)));
        }
        QVERIFY(chooseAndRead(desk, fileOf(devices, false)));
        QTRY_VERIFY(rowOf(desk.window, 1) != nullptr);
        aFrame(desk.window);
        QQuickItem* list = listOf(desk);
        QVERIFY(mostScrolled(list) > 200);
        const QPoint corner = list->mapToScene(QPointF(0, 0)).toPoint();
        const int width = 40;
        const int height = 12;
        const QRect above(corner.x(), corner.y() - height - 1, width, height);
        const QRect under(corner.x(), corner.y() + qRound(list->height()) + 1, width, height);
        QVERIFY(above.top() > 0 && under.bottom() < desk.window->height());
        const auto picture = [&] {
            aFrame(desk.window);
            return desk.window->grabWindow().convertToFormat(QImage::Format_RGB32);
        };
        const QImage atItsTop = picture();
        // (The mark is drawn at all: lighter than anything else in the tick of the first row, where
        // all of it is in sight. A mark that is not drawn would not be drawn beyond any edge either.)
        const QQuickItem* tick = inRow(rowOf(desk.window, 1), u"ownImportTick"_s);
        const QRect ofTheTick = tick->mapRectToScene(QRectF(0, 0, tick->width(), tick->height())).toRect();
        int white = 0;
        for (int y = ofTheTick.top(); y <= ofTheTick.bottom(); ++y) {
            for (int x = ofTheTick.left(); x <= ofTheTick.right(); ++x) {
                const QColor one = atItsTop.pixelColor(x, y);
                white += one.red() > 120 && one.green() > 140 ? 1 : 0;
            }
        }
        QVERIFY2(white >= 6, qPrintable(u"%1 pixels of the mark"_s.arg(white)));
        // Scrolled a pixel at a time through more than one row and the gap after it: every way
        // in which a tick is cut off by the list's upper edge and by its lower one.
        const auto differing = [&](const QImage& now, const QRect& strip) {
            int pixels = 0;
            for (int y = strip.top(); y <= strip.bottom(); ++y) {
                for (int x = strip.left(); x <= strip.right(); ++x) {
                    pixels += now.pixel(x, y) != atItsTop.pixel(x, y) ? 1 : 0;
                }
            }
            return pixels;
        };
        for (int scrolled = 1; scrolled <= 60; ++scrolled) {
            scrollTo(list, scrolled);
            const QImage now = picture();
            const int over = differing(now, above);
            const int below = differing(now, under);
            if (over > 0 || below > 0) {
                keepPictureOf(desk.window, u"own-import-mark-beyond-the-edge"_s);
            }
            QVERIFY2(over == 0, qPrintable(u"scrolled by %1: %2 pixels just above the list's edge are not the "
                                           "box's own any more"_s.arg(scrolled)
                                               .arg(over)));
            QVERIFY2(below == 0, qPrintable(u"scrolled by %1: %2 pixels just under the list's edge are not the "
                                            "box's own any more"_s.arg(scrolled)
                                                .arg(below)));
        }
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void theAddressOfADeviceWithTheLongestNameIsOnShowWhole_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::newRow("a window of the usual size") << QSize(1280, 800);
        QTest::newRow("the smallest window") << QSize(640, 400);
    }

    void theAddressOfADeviceWithTheLongestNameIsOnShowWhole()
    {
        QFETCH(QSize, size);
        // The longest name a file holds (eighty signs: a device with a longer one is left out by
        // the reader), of the widest letters there are, and an address of two hundred signs.
        const QString name = QString(80, u'W');
        QStringList parts;
        for (int part = 0; part < 4; ++part) {
            parts.append(QString(46, QChar(u'a' + part)));
        }
        const QString address = u"https://"_s + parts.join(u'.') + u".invalid:8443"_s;
        QVERIFY(address.size() >= 200);
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.load(size.width(), size.height()) != nullptr);
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        QVERIFY(chooseAndRead(desk, fileOf({saying(kvmAt(address, name), u"system"_s)}, false)));
        aFrame(desk.window);
        const QQuickItem* row = rowOf(desk.window, 1);
        QVERIFY(row != nullptr);
        const QQuickItem* said = inRow(row, u"ownImportNameAndAddress"_s);
        QVERIFY(said != nullptr && isSeen(said));
        // In one piece, as it is handed over: the name set off, then the address.
        const QString whole = u'"' + name + u"\" at "_s + address;
        QCOMPARE(said->property("text").toString(), whole);
        // Whole: broken into lines, each within the row, and cut short nowhere.
        QVERIFY(!said->property("truncated").toBool());
        QCOMPARE(said->property("elide").toInt(), 3);
        QVERIFY2(said->property("lineCount").toInt() >= 3, qPrintable(said->property("lineCount").toString()));
        QVERIFY2(said->property("contentWidth").toReal() <= said->width() + 0.5,
                 qPrintable(u"%1 in %2"_s.arg(said->property("contentWidth").toReal()).arg(said->width())));
        QVERIFY(said->height() >= said->property("contentHeight").toReal() - 0.5);
        // (It does not run under the row's button.)
        const QQuickItem* details = inRow(row, u"ownImportDetails"_s);
        QVERIFY(details != nullptr);
        QVERIFY(said->mapRectToItem(row, QRectF(0, 0, said->width(), said->height())).right() <=
                details->mapRectToItem(row, QRectF(0, 0, details->width(), details->height())).left());
        // And in sight to its last sign, where the list is scrolled to its top: the row's own
        // letters lie within the list, which lies within the box, which lies within the window.
        const QQuickItem* list = theOne(desk.window, u"ownImportOffered"_s);
        QVERIFY(list != nullptr);
        const QRectF letters = said->mapRectToItem(list, QRectF(0, 0, said->width(), said->height()));
        QVERIFY2(letters.left() >= 0 && letters.right() <= list->width() && letters.top() >= 0 &&
                     letters.bottom() <= list->height(),
                 qPrintable(u"%1,%2 %3x%4 in %5x%6"_s.arg(letters.x())
                                .arg(letters.y())
                                .arg(letters.width())
                                .arg(letters.height())
                                .arg(list->width())
                                .arg(list->height())));
        QVERIFY(isInside(desk.window, list));
        QVERIFY(isInside(desk.window, theOne(desk.window, u"ownImportCancel"_s)));
        keepPictureOf(desk.window, u"own-import-long-name-%1x%2"_s.arg(size.width()).arg(size.height()));
        // One is ticked, and the button says so; with none ticked it takes no click, and says
        // that it would take none.
        const QQuickItem* take = theOne(desk.window, u"ownImportTake"_s);
        QVERIFY(take != nullptr);
        QCOMPARE(take->property("text").toString(), u"Take 1 KVM"_s);
        QVERIFY(isInside(desk.window, take));
        clickOn(desk.window, inRow(row, u"ownImportTick"_s));
        QTRY_COMPARE(desk.ownFiles.tickedCount(), 0);
        QTRY_COMPARE(take->property("text").toString(), u"Take 0 KVMs"_s);
        QVERIFY(isSeen(take));
        QVERIFY(!take->isEnabled());
        QCOMPARE(theOne(desk.window, u"ownImportCounts"_s)->property("text").toString(),
                 u"1 device of this file is in this list. None is ticked. The file has no password for any of them."_s);
        QTest::qWait(Desk::pastAQuickRest);
        desk.ownFiles.take();
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY(desk.isOpen(u"ownImportBox"_s));
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void theClickThatReadsTheFileDoesNotTake()
    {
        WallOfOneTest own;
        Desk desk(own);
        // (A long moment before "Take" takes a click: this test looks at it not taking one, and
        // has seconds to do that in.)
        const int rest = Desk::longRest;
        desk.ownFiles.restFor(rest, rest);
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        desk.opens = desk.write(u"wall.json"_s, fileOf({ticked(1, u"One"_s), ticked(2, u"Two"_s)}, false));
        clickOn(desk.window, theOne(desk.window, u"ownImportChoose"_s));
        // A plain file asks for no passphrase.
        QVERIFY(theOne(desk.window, u"ownImportPassphrase"_s) == nullptr);
        const QQuickItem* read = theOne(desk.window, u"ownImportRead"_s);
        QVERIFY(read != nullptr && read->isEnabled());
        aFrame(desk.window);
        const QPoint where = middleOf(read);

        // A double click on "Read the file". The first click reads; the list is there a moment
        // later, with "Take 2 KVMs" in the place "Read the file" had in the box's row of
        // buttons. (Where that is in the window depends on how high the box has become, which
        // stands in the middle of its window: in a low window it is the very same place.)
        QElapsedTimer since;
        since.start();
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, where);
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isLaidOut(), 20000);
        QTRY_VERIFY(theOne(desk.window, u"ownImportTake"_s) != nullptr);
        aFrame(desk.window);
        const QQuickItem* take = theOne(desk.window, u"ownImportTake"_s);
        const QRectF takes = take->mapRectToScene(QRectF(0, 0, take->width(), take->height()));
        const QRectF readStood = QRectF(where, QSizeF(1, 1));
        QVERIFY2(takes.left() <= readStood.left() && takes.right() >= readStood.right(),
                 "\"Take\" does not stand in the place \"Read the file\" had in its row");
        // It takes no click in that moment, and looks so; and the program takes nothing: not by
        // the second click where the first one was, and not by one on the button where it is now.
        QVERIFY(!take->isEnabled());
        QVERIFY(!desk.ownFiles.mayTake());
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, where);
        QTest::mouseDClick(desk.window, Qt::LeftButton, {}, where);
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, middleOf(take));
        QTest::mouseDClick(desk.window, Qt::LeftButton, {}, middleOf(take));
        QTest::qWait(100);
        // (All of it in the moment: of four seconds, this is to have taken a fraction.)
        QVERIFY2(since.elapsed() < rest, qPrintable(QString::number(since.elapsed())));
        // (Neither took a tick away, either: the list is as it was.)
        QCOMPARE(desk.ownFiles.tickedCount(), 2);
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY(desk.isOpen(u"ownImportBox"_s));
        QVERIFY(desk.ownFiles.isLaidOut());
        QVERIFY(toldBy(desk.actions).isEmpty());
        // Nor does a key that was meant for the line or the button before: the keyboard is at
        // the list, and at no button.
        QVERIFY(!take->hasActiveFocus());
        QTest::keyClick(desk.window, Qt::Key_Return);
        QTest::keyClick(desk.window, Qt::Key_Space);
        QVERIFY(own.wall.kvms().isEmpty());

        // Looked at for its moment, it takes. (All of the moment, but for what a timer of
        // Windows' may be early by: a twentieth, and here a tenth is let pass.)
        QTRY_VERIFY_WITH_TIMEOUT(take->isEnabled(), 30000);
        QVERIFY2(since.elapsed() >= rest * 9 / 10, qPrintable(QString::number(since.elapsed())));
        clickOn(desk.window, take);
        QTRY_VERIFY(!desk.isOpen(u"ownImportBox"_s));
        QCOMPARE(own.names(), (QStringList{u"One"_s, u"Two"_s}));
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void whatCameOfATakingIsNotLostOnAnyWayTheBoxIsClosed_data()
    {
        QTest::addColumn<QString>("way");
        QTest::newRow("by its own button") << u"button"_s;
        QTest::newRow("by the way out in its title") << u"title"_s;
        QTest::newRow("by the program, from outside") << u"outside"_s;
        QTest::newRow("as its window leaves this PC's wall") << u"wall"_s;
    }

    void whatCameOfATakingIsNotLostOnAnyWayTheBoxIsClosed()
    {
        QFETCH(QString, way);
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.load() != nullptr);
        const QString box = u"ownImportBox"_s;
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, box));
        QVERIFY(chooseAndRead(desk, fileOf({ticked(1, u"One"_s), ticked(2, u"Two"_s)}), thePassphrase()));
        const QQuickItem* take = theOne(desk.window, u"ownImportTake"_s);
        QVERIFY(take != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(take->isEnabled(), 20000);
        const QPoint takeStood = middleOf(take);
        // Somebody put the second on the wall by hand meanwhile: one of the two is taken.
        QVERIFY(!own.put(elsewhere(2), u"By hand"_s).isEmpty());
        clickOn(desk.window, take);
        QTRY_VERIFY(desk.ownFiles.hasTaken());
        aFrame(desk.window);
        // While what came of it is shown, nothing beside the box closes it, nor Esc: a second
        // click where "Take" stood lands on the box, and one beside it on nothing.
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, takeStood);
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, QPoint(8, desk.window->height() - 8));
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, QPoint(desk.window->width() / 2, 6));
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTest::qWait(100);
        QVERIFY2(desk.isOpen(box), "what came of the taking was closed by a click beside it, unread");
        QVERIFY(desk.ownFiles.hasTaken());
        QVERIFY(toldBy(desk.actions).isEmpty());
        // Closed, by the way of this row: the window's corner says what was taken, as something
        // to be looked at.
        if (way == u"button"_s) {
            clickOn(desk.window, theOne(desk.window, u"ownImportCancel"_s));
        } else if (way == u"title"_s) {
            // (Its own: the settings it stands over have one too.)
            QList<const QQuickItem*> ways;
            named(desk.box(box)->property("header").value<QQuickItem*>(), u"closeBox"_s, ways);
            QCOMPARE(ways.size(), 1);
            clickOn(desk.window, ways.constFirst());
        } else if (way == u"outside"_s) {
            desk.ownFiles.close();
        } else {
            desk.session.show(atAServersWall());
        }
        QTRY_VERIFY(!desk.isOpen(box));
        QCOMPARE(toldBy(desk.actions), QStringList{u"error: 1 of 2 KVMs were taken onto this wall."_s});
        QVERIFY(!desk.ownFiles.hasTaken());
        QCOMPARE(own.names(), (QStringList{u"By hand"_s, u"One"_s}));
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void aListOfTwoThousandDevicesIsShownSoonAndOnlyWhereItIsSeenAndATickSurvivesItsRowGoing()
    {
        const int many = 2000;
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        desk.opens = desk.write(u"many.json"_s, fileOfSoMany(many));
        clickOn(desk.window, theOne(desk.window, u"ownImportChoose"_s));
        QVERIFY(!desk.ownFiles.fileName().isEmpty());
        const QQuickItem* read = theOne(desk.window, u"ownImportRead"_s);
        aFrame(desk.window);

        // From the press on "Read the file" until the list is on the screen, drawn.
        QElapsedTimer since;
        since.start();
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, middleOf(read));
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isLaidOut(), 60000);
        const qint64 laidOut = since.elapsed();
        QTRY_VERIFY(rowOf(desk.window, 1) != nullptr);
        aFrame(desk.window);
        const qint64 shown = since.elapsed();
        qInfo("a file of %d devices: laid out %lld ms after the press, and on the screen after %lld ms", many,
              static_cast<long long>(laidOut), static_cast<long long>(shown));
        QVERIFY2(shown < 20000, qPrintable(QString::number(shown)));
        // Every device is in the list, and ticked: but a row was made only where the list is seen.
        QObject* list = desk.window->findChild<QObject*>(u"ownImportOffered"_s);
        QVERIFY(list != nullptr);
        QCOMPARE(list->property("count").toInt(), many);
        QCOMPARE(desk.ownFiles.tickedCount(), many);
        QCOMPARE(theOne(desk.window, u"ownImportTake"_s)->property("text").toString(), u"Take 2000 KVMs"_s);
        QCOMPARE(
            theOne(desk.window, u"ownImportCounts"_s)->property("text").toString(),
            u"2000 devices of this file are in this list. 2000 are ticked. The file has no password for any of them."_s);
        const int made = static_cast<int>(deviceRows(desk.window).size());
        qInfo("rows made for it: %d", made);
        QVERIFY2(made >= 1 && made < 60, qPrintable(QString::number(made)));
        QVERIFY(theOne(desk.window, u"ownImportScrollMark"_s) != nullptr);
        keepPictureOf(desk.window, u"own-import-two-thousand-top"_s);

        // The tick of the second device is taken away (once the list has stood for its moment),
        // and the list scrolled to its end: the row of the second is gone, and that of the last
        // is there.
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isSettled(), 20000);
        clickOn(desk.window, inRow(rowOf(desk.window, 2), u"ownImportTick"_s));
        QTRY_COMPARE(desk.ownFiles.tickedCount(), many - 1);
        QVERIFY(!inRow(rowOf(desk.window, 2), u"ownImportTick"_s)->property("checked").toBool());
        QVERIFY(QMetaObject::invokeMethod(list, "positionViewAtEnd"));
        QTRY_VERIFY(rowOf(desk.window, many) != nullptr);
        aFrame(desk.window);
        QTRY_VERIFY(rowOf(desk.window, 2) == nullptr);
        QVERIFY(static_cast<int>(deviceRows(desk.window).size()) < 60);
        // (And the last one's is taken away there.)
        clickOn(desk.window, inRow(rowOf(desk.window, many), u"ownImportTick"_s));
        QTRY_COMPARE(desk.ownFiles.tickedCount(), many - 2);
        keepPictureOf(desk.window, u"own-import-two-thousand-end"_s);
        // Scrolled back: the second comes back as it was left, not ticked, and its neighbours
        // ticked as they were.
        QVERIFY(QMetaObject::invokeMethod(list, "positionViewAtBeginning"));
        QTRY_VERIFY(rowOf(desk.window, 2) != nullptr);
        aFrame(desk.window);
        QVERIFY(!inRow(rowOf(desk.window, 2), u"ownImportTick"_s)->property("checked").toBool());
        QVERIFY(inRow(rowOf(desk.window, 1), u"ownImportTick"_s)->property("checked").toBool());
        QVERIFY(inRow(rowOf(desk.window, 3), u"ownImportTick"_s)->property("checked").toBool());
        QTRY_VERIFY(rowOf(desk.window, many) == nullptr);
        QCOMPARE(desk.ownFiles.tickedCount(), many - 2);
        QCOMPARE(theOne(desk.window, u"ownImportTake"_s)->property("text").toString(), u"Take 1998 KVMs"_s);
        QCOMPARE(
            theOne(desk.window, u"ownImportCounts"_s)->property("text").toString(),
            u"2000 devices of this file are in this list. 1998 are ticked. The file has no password for any of them."_s);
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void theLinesBesideTheDevicesAreFoldedAndNothingIsTakenBeforeTheyWereOpened()
    {
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        // A file made by anybody: twenty devices that the reader leaves out, each with a
        // sentence, and six that it takes.
        QList<Device> devices;
        for (int number = 1; number <= 6; ++number) {
            devices.append(ticked(number, u"KVM %1"_s.arg(number)));
        }
        QVERIFY(chooseAndRead(desk, fileWithSoManyLeftOut(20, devices)));
        QTRY_VERIFY(rowOf(desk.window, 21) != nullptr);
        aFrame(desk.window);
        QQuickItem* list = listOf(desk);
        QVERIFY(list != nullptr);
        // The first screen: one row that says how many lines there are beside the devices, and
        // the devices, every one of them in sight with its tick. Not twenty lines, and the
        // devices somewhere under them.
        QCOMPARE(seenOnes(desk.window, u"ownImportBeside"_s).size(), 0);
        QVERIFY(!says(desk.window, u"Device 1: it has no address"_s));
        const QQuickItem* more = theOne(desk.window, u"ownImportMoreNotes"_s);
        QVERIFY(more != nullptr);
        QCOMPARE(more->property("text").toString(), u"20 notes about this file"_s);
        QVERIFY(isWholeIn(list, more));
        QVERIFY(says(desk.window, u"To be opened before anything is taken."_s));
        for (int place = 21; place <= 26; ++place) {
            const QQuickItem* row = rowOf(desk.window, place);
            QVERIFY2(row != nullptr && isWholeIn(list, row), qPrintable(QString::number(place)));
            QVERIFY(isWholeIn(list, inRow(row, u"ownImportTick"_s)));
        }
        QVERIFY(scrolledBy(list) < 0.5);
        // (A plain file has no passwords: that is said once, above the list, and on no row.)
        QCOMPARE(saidIn(rowOf(desk.window, 21), u"ownImportInShort"_s), u"Known by Windows' word"_s);
        // What is ticked stands above the list in words, beside the number on the button.
        QCOMPARE(theOne(desk.window, u"ownImportCounts"_s)->property("text").toString(),
                 u"6 devices of this file are in this list. 6 are ticked. The file has no password for any of them."_s);
        const QQuickItem* take = theOne(desk.window, u"ownImportTake"_s);
        QVERIFY(take != nullptr);
        QCOMPARE(take->property("text").toString(), u"Take 6 KVMs"_s);
        keepPictureOf(desk.window, u"own-import-twenty-notes-folded"_s);
        // "Take" takes no click while lines are folded away that nobody opened, however long
        // the list is looked at (several of its moments, which are short here).
        QTest::qWait(Desk::pastAQuickRest);
        QVERIFY(!take->isEnabled());
        QVERIFY(!desk.ownFiles.mayTake());
        clickOn(desk.window, take);
        QTest::qWait(50);
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY(desk.isOpen(u"ownImportBox"_s));

        // Opened by a click: every line is in the list, before the first device, and the list
        // stands where it stood. Then "Take" takes a click, after its moment.
        clickOn(desk.window, more);
        QTRY_COMPARE(desk.ownFiles.foldedNotes(), 0);
        aFrame(desk.window);
        QVERIFY(theOne(desk.window, u"ownImportMoreNotes"_s) == nullptr);
        QVERIFY(seenOnes(desk.window, u"ownImportBeside"_s).size() > 3);
        QVERIFY(says(desk.window, u"Device 1: it has no address"_s));
        QVERIFY(says(desk.window, u"Device 4: it has no address"_s));
        QCOMPARE(list->property("count").toInt(), 26);
        QVERIFY(scrolledBy(list) < 0.5);
        QVERIFY(theOne(desk.window, u"ownImportScrollMark"_s) != nullptr);
        keepPictureOf(desk.window, u"own-import-twenty-notes-opened"_s);
        QTRY_VERIFY_WITH_TIMEOUT(take->isEnabled(), 20000);
        // (The last of the lines and the devices, at the list's end.)
        QVERIFY(QMetaObject::invokeMethod(list, "positionViewAtEnd"));
        QTRY_VERIFY(rowOf(desk.window, 26) != nullptr);
        aFrame(desk.window);
        QVERIFY(isWholeIn(list, rowOf(desk.window, 26)));
        clickOn(desk.window, take);
        QTRY_VERIFY(!desk.isOpen(u"ownImportBox"_s));
        QCOMPARE(own.wall.kvms().size(), 6);
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void aNewListBeginsAtItsTopWhereverTheOneBeforeWasScrolledTo()
    {
        // (The smallest window: the list has room for three rows, and the tenth is far down.)
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.load(640, 400) != nullptr);
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        const auto fileOfForty = [](const QString& called) {
            QList<Device> devices;
            for (int number = 1; number <= 40; ++number) {
                devices.append(ticked(number, u"%1 %2"_s.arg(called).arg(number)));
            }
            return fileWithSoManyLeftOut(1, devices);
        };
        QVERIFY(chooseAndRead(desk, fileOfForty(u"First"_s)));
        QTRY_VERIFY(rowOf(desk.window, 2) != nullptr);
        aFrame(desk.window);
        QQuickItem* list = listOf(desk);
        QVERIFY(list != nullptr);
        // The keyboard is at the list, and its keys scroll it: Down, ten times.
        QTRY_VERIFY(list->hasActiveFocus());
        QVERIFY(scrolledBy(list) < 0.5);
        for (int press = 0; press < 10; ++press) {
            QTest::keyClick(desk.window, Qt::Key_Down);
        }
        // (Looked at once the list stands still: wherever its keys have taken it by then.)
        QTRY_VERIFY2_WITH_TIMEOUT(scrolledBy(list) > 150, qPrintable(QString::number(scrolledBy(list))), 20000);
        aFrame(desk.window);
        QVERIFY(!says(desk.window, u"1 note about this file"_s));
        QVERIFY(rowOf(desk.window, 2) == nullptr);

        // Another file is laid out: its list begins at its top, with the row that stands for
        // the line beside its devices and its first devices in sight. Nothing is kept from the
        // list before: not where that one was scrolled to, and no row of the keyboard's.
        const auto standsAtItsTop = [&](const QString& called) -> QString {
            aFrame(desk.window);
            if (scrolledBy(list) > 0.5) {
                return u"the list is scrolled by %1"_s.arg(scrolledBy(list));
            }
            if (!says(desk.window, u"1 note about this file"_s)) {
                return u"the row for the line beside the devices is not in sight"_s;
            }
            const QQuickItem* first = rowOf(desk.window, 2);
            if (first == nullptr || heightInSight(list, first) < 1) {
                return u"the first device is not in sight"_s;
            }
            if (saidIn(first, u"ownImportNameAndAddress"_s) != u"\"%1 1\" at https://192.0.2.1"_s.arg(called)) {
                return u"the first device is %1"_s.arg(saidIn(first, u"ownImportNameAndAddress"_s));
            }
            return list->property("currentIndex").toInt() > 0
                       ? u"the list's own row is %1"_s.arg(list->property("currentIndex").toInt())
                       : QString();
        };
        QVERIFY(chooseAndRead(desk, fileOfForty(u"Second"_s)));
        QString wrong = standsAtItsTop(u"Second"_s);
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong));
        // So where the box was closed over a list that was scrolled, and is opened again.
        QTRY_VERIFY(list->hasActiveFocus());
        for (int press = 0; press < 10; ++press) {
            QTest::keyClick(desk.window, Qt::Key_Down);
        }
        QTRY_VERIFY2_WITH_TIMEOUT(scrolledBy(list) > 150, qPrintable(QString::number(scrolledBy(list))), 20000);
        clickOn(desk.window, theOne(desk.window, u"ownImportCancel"_s));
        QTRY_VERIFY(!desk.isOpen(u"ownImportBox"_s));
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        QVERIFY(chooseAndRead(desk, fileOfForty(u"Third"_s)));
        wrong = standsAtItsTop(u"Third"_s);
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong));
        // (And what else the keys of a list do: back up, a page at a time, and to either end.)
        QTRY_VERIFY(list->hasActiveFocus());
        QTest::keyClick(desk.window, Qt::Key_PageDown);
        const qreal aPageDown = scrolledBy(list);
        QVERIFY2(aPageDown > 40, qPrintable(QString::number(aPageDown)));
        QTest::keyClick(desk.window, Qt::Key_Up);
        QVERIFY(scrolledBy(list) < aPageDown);
        QTest::keyClick(desk.window, Qt::Key_End);
        QTRY_VERIFY(rowOf(desk.window, 41) != nullptr);
        QVERIFY(qAbs(scrolledBy(list) - mostScrolled(list)) < 1.0);
        QTest::keyClick(desk.window, Qt::Key_PageUp);
        QVERIFY(scrolledBy(list) < mostScrolled(list) - 40);
        QTest::keyClick(desk.window, Qt::Key_Home);
        QTRY_VERIFY(scrolledBy(list) < 0.5);
        QVERIFY(own.wall.kvms().isEmpty());
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void whatWasTypedAsAPassphraseIsInItsLineForNoLongerThanItsFileIsTheChosenOne()
    {
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        const QByteArray sealed = fileOf({ticked(1, u"One"_s)});
        const QByteArray plain = fileOf({ticked(2, u"Two"_s)}, false);
        // (Sealed, and what is sealed in it is another by one sign: it does not open with its
        // own key, and no other try will read it.)
        QJsonObject parts = QJsonDocument::fromJson(sealed).object();
        QJsonObject inside = parts[u"sealed"_s].toObject();
        QString data = inside[u"data"_s].toString();
        data[10] = data.at(10) == u'A' ? u'B' : u'A';
        inside[u"data"_s] = data;
        parts[u"sealed"_s] = inside;
        const QByteArray damaged = QJsonDocument(parts).toJson(QJsonDocument::Compact);
        parts = QJsonDocument::fromJson(sealed).object();
        parts[u"version"_s] = 2;
        const QByteArray newer = QJsonDocument(parts).toJson(QJsonDocument::Compact);

        const auto choose = [&desk](const QString& name, const QByteArray& file) {
            desk.opens = desk.write(name, file);
            clickOn(desk.window, theOne(desk.window, u"ownImportChoose"_s));
        };
        // The line, once it is there: looked at from then on whether or not it is in sight.
        choose(u"first.qkvm"_s, sealed);
        const QQuickItem* line = theOne(desk.window, u"ownImportPassphrase"_s);
        QVERIFY(line != nullptr);
        const auto typed = [&] { return line->property("text").toString(); };
        const auto typeIntoIt = [&](const QString& text) {
            clickOn(desk.window, line);
            type(desk.window, text);
            return typed() == text;
        };
        // What this is about, at every step: a line that nobody sees holds nothing.
        const auto holdsNothingUnseen = [&] { return isSeen(line) || typed().isEmpty(); };

        // A file that is damaged: its passphrase is typed (the right one: its lock opens, and
        // what is in it does not), it is read, and refused. It is not the chosen one any more,
        // its line is gone, and what was typed is gone with it: a passphrase that opens files
        // does not stay in a line nobody sees. The next sealed file has an empty line.
        choose(u"damaged.qkvm"_s, damaged);
        QVERIFY(isSeen(line));
        QVERIFY(typeIntoIt(thePassphrase()));
        clickOn(desk.window, theOne(desk.window, u"ownImportRead"_s));
        QTRY_VERIFY(theOne(desk.window, u"ownImportProblem"_s) != nullptr);
        QCOMPARE(desk.ownFiles.fileName(), QString());
        QVERIFY(!isSeen(line));
        QCOMPARE(typed(), QString());
        choose(u"first.qkvm"_s, sealed);
        QVERIFY(isSeen(line));
        QCOMPARE(typed(), QString());
        // A plain file chosen after a passphrase was typed for a sealed one.
        QVERIFY(typeIntoIt(u"typed for the first"_s));
        choose(u"plain.json"_s, plain);
        QVERIFY(!isSeen(line));
        QCOMPARE(typed(), QString());
        choose(u"first.qkvm"_s, sealed);
        QCOMPARE(typed(), QString());
        // A file of a newer kind chosen after it: refused at the choosing.
        QVERIFY(typeIntoIt(u"typed for the first"_s));
        choose(u"newer.qkvm"_s, newer);
        QVERIFY(theOne(desk.window, u"ownImportProblem"_s) != nullptr);
        QVERIFY(holdsNothingUnseen());
        QCOMPARE(typed(), QString());
        // Another sealed file chosen, with the line in sight throughout: what was typed was
        // the first one's, and is not the second one's.
        choose(u"first.qkvm"_s, sealed);
        QVERIFY(typeIntoIt(u"typed for the first"_s));
        choose(u"second.qkvm"_s, sealed);
        QVERIFY(isSeen(line));
        QCOMPARE(typed(), QString());
        // Windows' box given up: the same file is chosen still, and what was typed for it stays.
        QVERIFY(typeIntoIt(u"typed for the second"_s));
        desk.opens.clear();
        clickOn(desk.window, theOne(desk.window, u"ownImportChoose"_s));
        QCOMPARE(typed(), u"typed for the second"_s);
        // So after a passphrase that was not the one: the file stays chosen, to be tried again.
        clickOn(desk.window, theOne(desk.window, u"ownImportRead"_s));
        QTRY_VERIFY(says(desk.window, u"That is not the passphrase this file was made with"_s));
        QVERIFY(isSeen(line));
        QCOMPARE(typed(), u"typed for the second"_s);
        // The box closed over it, and opened again.
        clickOn(desk.window, theOne(desk.window, u"ownImportCancel"_s));
        QTRY_VERIFY(!desk.isOpen(u"ownImportBox"_s));
        QCOMPARE(typed(), QString());
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        choose(u"first.qkvm"_s, sealed);
        QVERIFY(isSeen(line));
        QCOMPARE(typed(), QString());
        // And read with the right one: laid out, and the line is empty and gone.
        QVERIFY(typeIntoIt(thePassphrase()));
        clickOn(desk.window, theOne(desk.window, u"ownImportRead"_s));
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.isLaidOut(), 20000);
        QVERIFY(holdsNothingUnseen());
        QCOMPARE(typed(), QString());
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void whatCameOfATakingWhereOneWasNotTakenIsShownAndOnlyTheWayOutIsLeft()
    {
        WallOfOneTest own;
        Desk desk(own);
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        QVERIFY(chooseAndRead(desk, fileOf({ticked(1, u"One"_s), ticked(2, u"Two"_s), ticked(3, u"Three"_s)}),
                              thePassphrase()));
        const QQuickItem* take = theOne(desk.window, u"ownImportTake"_s);
        QVERIFY(take != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(take->isEnabled(), 20000);
        // Since the file was laid out, somebody added the second by hand in another window.
        QVERIFY(!own.put(elsewhere(2), u"By hand"_s).isEmpty());
        clickOn(desk.window, take);
        QTRY_VERIFY(desk.ownFiles.hasTaken());
        // The box stays: how many were taken, and each that was not, with what it is and why.
        QVERIFY(desk.isOpen(u"ownImportBox"_s));
        QCOMPARE(own.names(), (QStringList{u"By hand"_s, u"One"_s, u"Three"_s}));
        QVERIFY(says(desk.window, u"2 of 3 KVMs were taken onto this wall."_s));
        QVERIFY(says(desk.window, u"Not taken:"_s));
        QTRY_COMPARE(seenOnes(desk.window, u"ownImportLeft"_s).size(), 1);
        QVERIFY(says(desk.window, u"\"Two\" at https://192.0.2.2"_s));
        QVERIFY(
            says(desk.window, u"It cannot be taken: it is on the wall already (\"By hand\" at https://192.0.2.2)."_s));
        // The laying-out is over: no list, no file to choose, nothing to read or take. Only
        // "Close" is left, and it closes.
        QVERIFY(theOne(desk.window, u"ownImportOffered"_s) == nullptr);
        QVERIFY(theOne(desk.window, u"ownImportNothingYet"_s) == nullptr);
        QVERIFY(theOne(desk.window, u"ownImportChoose"_s) == nullptr);
        QVERIFY(theOne(desk.window, u"ownImportRead"_s) == nullptr);
        QVERIFY(theOne(desk.window, u"ownImportTake"_s) == nullptr);
        QVERIFY(theOne(desk.window, u"ownImportPassphrase"_s) == nullptr);
        const QQuickItem* close = theOne(desk.window, u"ownImportCancel"_s);
        QVERIFY(close != nullptr);
        QCOMPARE(close->property("text").toString(), u"Close"_s);
        QVERIFY(toldBy(desk.actions).isEmpty());
        clickOn(desk.window, close);
        QTRY_VERIFY(!desk.isOpen(u"ownImportBox"_s));
        QVERIFY(!desk.ownFiles.hasTaken());
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void afterItsTimeTheListGoesAndTheBoxStaysWhereItIsAndSaysThatTheFileIsToBeChosenAgain()
    {
        WallOfOneTest own;
        // (What is read is kept for five seconds here: what is looked at while the list is
        // there has seconds to be looked at in.)
        Desk desk(own, 5000);
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        QList<Device> devices;
        for (int number = 1; number <= 30; ++number) {
            devices.append(ticked(number, u"KVM %1"_s.arg(number)));
        }
        const QByteArray file = fileOf(devices);
        QVERIFY(chooseAndRead(desk, file, thePassphrase()));
        QTRY_VERIFY(theOne(desk.window, u"ownImportOffered"_s) != nullptr);
        QCOMPARE(desk.exchange.passwordsHeld(), 30);
        aFrame(desk.window);
        QQuickItem* list = listOf(desk);
        // Where the box is, how large, and where its buttons stand, with the list in it. The
        // list is scrolled, as by somebody who reads down it.
        const QObject* box = desk.box(u"ownImportBox"_s);
        const QRectF boxWas(box->property("x").toReal(), box->property("y").toReal(), box->property("width").toReal(),
                            box->property("height").toReal());
        const QQuickItem* take = theOne(desk.window, u"ownImportTake"_s);
        QVERIFY(take != nullptr);
        const QPoint takeStood = middleOf(take);
        const QPoint cancelStood = middleOf(theOne(desk.window, u"ownImportCancel"_s));
        QTRY_VERIFY(list->hasActiveFocus());
        for (int press = 0; press < 10; ++press) {
            QTest::keyClick(desk.window, Qt::Key_Down);
        }
        QVERIFY(scrolledBy(list) > 150);

        // The time for which what was read is kept is over: the list is gone, nothing of the
        // file is there, and a sentence says what is so and what there is to do: choose the
        // file again. As something that is nothing wrong.
        QTRY_VERIFY_WITH_TIMEOUT(!desk.ownFiles.isLaidOut(), 30000);
        QVERIFY(desk.isOpen(u"ownImportBox"_s));
        QTRY_VERIFY(theOne(desk.window, u"ownImportOffered"_s) == nullptr);
        aFrame(desk.window);
        QVERIFY(theOne(desk.window, u"ownImportTake"_s) == nullptr);
        QVERIFY(theOne(desk.window, u"ownImportAbove"_s) == nullptr);
        QVERIFY(theOne(desk.window, u"ownImportPassphrase"_s) == nullptr);
        QVERIFY(!says(desk.window, u"wall-0.qkvm"_s));
        const QQuickItem* note = theOne(desk.window, u"ownImportNote"_s);
        QVERIFY(note != nullptr);
        QCOMPARE(note->property("text").toString(),
                 u"What is read from a file is kept for 5 seconds, and that time is over. Nothing of the file is "
                 "kept: choose it again."_s);
        QVERIFY(isInside(desk.window, note));
        QVERIFY(theOne(desk.window, u"ownImportProblem"_s) == nullptr);
        QCOMPARE(desk.exchange.passwordsHeld(), 0);
        // The box has not moved, and is as large as it was: a click that was on its way to
        // "Take" lands where "Take" stood, on a button that takes none ("Read the file", with
        // no file to read), and one that was on its way to "Cancel" lands on "Cancel".
        const QRectF boxIs(box->property("x").toReal(), box->property("y").toReal(), box->property("width").toReal(),
                           box->property("height").toReal());
        QVERIFY2(qAbs(boxIs.y() - boxWas.y()) < 0.5 && qAbs(boxIs.height() - boxWas.height()) < 0.5 &&
                     qAbs(boxIs.x() - boxWas.x()) < 0.5 && qAbs(boxIs.width() - boxWas.width()) < 0.5,
                 qPrintable(u"the box was at %1,%2 and %3 by %4, and is at %5,%6 and %7 by %8"_s.arg(boxWas.x())
                                .arg(boxWas.y())
                                .arg(boxWas.width())
                                .arg(boxWas.height())
                                .arg(boxIs.x())
                                .arg(boxIs.y())
                                .arg(boxIs.width())
                                .arg(boxIs.height())));
        const QQuickItem* read = theOne(desk.window, u"ownImportRead"_s);
        QVERIFY(read != nullptr && !read->isEnabled());
        QVERIFY(read->mapRectToScene(QRectF(0, 0, read->width(), read->height())).contains(takeStood));
        const QQuickItem* cancel = theOne(desk.window, u"ownImportCancel"_s);
        QVERIFY(cancel->mapRectToScene(QRectF(0, 0, cancel->width(), cancel->height())).contains(cancelStood));
        keepPictureOf(desk.window, u"own-import-after-its-time"_s);
        const int openedBefore = desk.opened;
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, takeStood);
        // Nor does a click beside the box close it at that moment, nor Esc: the sentence is
        // there to be read until the box is left by one of its own buttons.
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, QPoint(8, desk.window->height() - 8));
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, QPoint(desk.window->width() / 2, 6));
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTest::qWait(100);
        QVERIFY(desk.isOpen(u"ownImportBox"_s));
        QCOMPARE(desk.opened, openedBefore);
        QVERIFY(theOne(desk.window, u"ownImportNote"_s) != nullptr);
        QVERIFY(own.wall.kvms().isEmpty());

        // What there is to do: choose the file again, and read it. Its list begins at its top,
        // whatever the one before was scrolled to; the sentence is gone, and the box is as any.
        QVERIFY(theOne(desk.window, u"ownImportChoose"_s)->isEnabled());
        QVERIFY(chooseAndRead(desk, file, thePassphrase()));
        aFrame(desk.window);
        QVERIFY(theOne(desk.window, u"ownImportNote"_s) == nullptr);
        QVERIFY2(scrolledBy(list) < 0.5, qPrintable(QString::number(scrolledBy(list))));
        QVERIFY(rowOf(desk.window, 1) != nullptr);
        QCOMPARE(desk.exchange.passwordsHeld(), 30);
        // (And with a file chosen, a box with no list in it closes for a click beside it, as
        // it did before any of this.)
        clickOn(desk.window, theOne(desk.window, u"ownImportCancel"_s));
        QTRY_VERIFY(!desk.isOpen(u"ownImportBox"_s));
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        aFrame(desk.window);
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, QPoint(8, desk.window->height() - 8));
        QTRY_VERIFY(!desk.isOpen(u"ownImportBox"_s));
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    void aFileThatIsChosenAndNotReadGoesAfterTheSameTimeWithWhatWasTypedForIt()
    {
        WallOfOneTest own;
        // (What is read is kept for five seconds here.)
        Desk desk(own, 5000);
        QVERIFY(desk.load() != nullptr);
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        // A sealed file is chosen, and a passphrase typed for it; and the box is left so.
        desk.opens = desk.write(u"left.qkvm"_s, fileOf({ticked(1, u"One"_s)}));
        clickOn(desk.window, theOne(desk.window, u"ownImportChoose"_s));
        const QQuickItem* line = theOne(desk.window, u"ownImportPassphrase"_s);
        QVERIFY(line != nullptr);
        clickOn(desk.window, line);
        type(desk.window, u"typed for it"_s);
        QCOMPARE(line->property("text").toString(), u"typed for it"_s);
        aFrame(desk.window);
        const QObject* box = desk.box(u"ownImportBox"_s);
        const QRectF boxWas(box->property("x").toReal(), box->property("y").toReal(), box->property("width").toReal(),
                            box->property("height").toReal());
        const QPoint readStood = middleOf(theOne(desk.window, u"ownImportRead"_s));
        // Its time is over: nothing of the file is held, what was typed for it is out of its
        // line, and the box says so as it does after that time. It stays where it is, as large
        // as it was, and is not closed by a click beside it.
        QTRY_VERIFY_WITH_TIMEOUT(desk.ownFiles.bytesHeld() == 0, 30000);
        aFrame(desk.window);
        QCOMPARE(line->property("text").toString(), QString());
        QVERIFY(!isSeen(line));
        QVERIFY(!says(desk.window, u"left.qkvm"_s));
        const QQuickItem* note = theOne(desk.window, u"ownImportNote"_s);
        QVERIFY(note != nullptr);
        QCOMPARE(note->property("text").toString(),
                 u"What is read from a file is kept for 5 seconds, and that time is over. Nothing of the file is "
                 "kept: choose it again."_s);
        keepPictureOf(desk.window, u"own-import-chosen-kept-no-longer"_s);
        const QRectF boxIs(box->property("x").toReal(), box->property("y").toReal(), box->property("width").toReal(),
                           box->property("height").toReal());
        QVERIFY2(qAbs(boxIs.y() - boxWas.y()) < 0.5 && qAbs(boxIs.height() - boxWas.height()) < 0.5,
                 qPrintable(u"the box was at %1 and %2 high, and is at %3 and %4 high"_s.arg(boxWas.y())
                                .arg(boxWas.height())
                                .arg(boxIs.y())
                                .arg(boxIs.height())));
        const QQuickItem* read = theOne(desk.window, u"ownImportRead"_s);
        QVERIFY(read != nullptr && !read->isEnabled());
        QVERIFY(read->mapRectToScene(QRectF(0, 0, read->width(), read->height())).contains(readStood));
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, QPoint(8, desk.window->height() - 8));
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTest::qWait(100);
        QVERIFY(desk.isOpen(u"ownImportBox"_s));
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }

    // --- with the wall, and with this PC's wall ----------------------------------------------------

    void theBoxesGoWhenTheirWindowIsNotAtThisPcsWallAnyMore()
    {
        WallOfOneTest own;
        QVERIFY(!own.put(u"http://127.0.0.1:20001"_s, u"Build server"_s).isEmpty());
        Desk desk(own);
        QVERIFY(desk.load() != nullptr);
        // A list is shown, and another window went on to a server: this one shows that server's
        // wall now. The box is not left open over it, and what was laid out is forgotten.
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        QVERIFY(chooseAndRead(desk, fileOf({ticked(1, u"One"_s)}), thePassphrase()));
        QCOMPARE(desk.exchange.passwordsHeld(), 1);
        desk.session.show(atAServersWall());
        QTRY_VERIFY(!desk.isOpen(u"ownImportBox"_s));
        QVERIFY(!desk.ownFiles.isLaidOut());
        QCOMPARE(desk.ownFiles.fileName(), QString());
        QCOMPARE(desk.exchange.passwordsHeld(), 0);
        // Nor over the screen that asks for a server: with the wall, the boxes go.
        desk.session.show(atItsOwnWall());
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        QVERIFY(chooseAndRead(desk, fileOf({ticked(1, u"One"_s)}), thePassphrase()));
        desk.session.show({});
        QTRY_VERIFY(!desk.isOpen(u"ownImportBox"_s));
        QVERIFY(!desk.ownFiles.isLaidOut());
        QCOMPARE(desk.exchange.passwordsHeld(), 0);
        // (And with the wall alone, whatever is said of whose wall it was: a window that shows
        // no wall has no box of a file open.)
        desk.session.show(atItsOwnWall());
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        QVERIFY(chooseAndRead(desk, fileOf({ticked(1, u"One"_s)}), thePassphrase()));
        ui::SessionView::Shown noWall = atItsOwnWall();
        noWall.stage = u"address"_s;
        desk.session.show(noWall);
        QTRY_VERIFY(!desk.isOpen(u"ownImportBox"_s));
        QVERIFY(!desk.ownFiles.isLaidOut());
        QCOMPARE(desk.exchange.passwordsHeld(), 0);

        // So the box that writes the wall, with a file made and not kept yet.
        desk.session.show(atItsOwnWall());
        desk.savesTo = desk.folder.filePath(u"never.json"_s);
        QVERIFY(openFromTheSettings(desk, u"settingsExport"_s, u"ownExportBox"_s));
        clickOn(desk.window, theOne(desk.window, u"ownExportMake"_s));
        QTRY_VERIFY(desk.ownFiles.isMade());
        desk.session.show(atAServersWall());
        QTRY_VERIFY(!desk.isOpen(u"ownExportBox"_s));
        QVERIFY(!desk.ownFiles.isMade());
        desk.session.show(atItsOwnWall());
        QVERIFY(openFromTheSettings(desk, u"settingsExport"_s, u"ownExportBox"_s));
        clickOn(desk.window, theOne(desk.window, u"ownExportMake"_s));
        QTRY_VERIFY(desk.ownFiles.isMade());
        desk.session.show({});
        QTRY_VERIFY(!desk.isOpen(u"ownExportBox"_s));
        QVERIFY(!desk.ownFiles.isMade());
        QVERIFY(desk.suggestions.isEmpty());
        QVERIFY(!QFile::exists(desk.savesTo));
        // And from outside, where the window goes out of sight: the program closes them.
        desk.session.show(atItsOwnWall());
        QVERIFY(openFromTheSettings(desk, u"settingsImport"_s, u"ownImportBox"_s));
        QVERIFY(chooseAndRead(desk, fileOf({ticked(1, u"One"_s)}), thePassphrase()));
        desk.ownFiles.close();
        QTRY_VERIFY(!desk.isOpen(u"ownImportBox"_s));
        QCOMPARE(desk.exchange.passwordsHeld(), 0);
        QVERIFY(own.names() == QStringList{u"Build server"_s});
        QVERIFY2(desk.complaints.isEmpty(), qPrintable(desk.complaints.join(u'\n')));
    }
};

int main(int argc, char* argv[])
{
    return runOwnFilesScreensTests<TestOwnFilesScreensUse>(argc, argv);
}

#include "test_own_files_screens_use.moc"
